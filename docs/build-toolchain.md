# 构建 / 工具链 / 补丁 / 枚举

## 1. 构建配置与目录

| 用途 | 配置 / 目录 | 命令 / 脚本 | 产物 |
| --- | --- | --- | --- |
| 正式 Release | `cmake-build-release-visual-studio` | 先 `vcvars64` + CLion 自带 ninja；或 `build/msvc`（VS 生成器）`cmake --build build/msvc --config Release --target Y2Kmeter_Standalone -j 8` | `Y2Kmeter_artefacts/Release/Standalone/Y2Kmeter.exe` |
| 正式安装包 | 同上 | `build_installer.bat` | `dist/Y2Kmeter_Setup_<ver>_x64.exe` |
| 诊断包（带日志） | `cmake-build-relwithdebinfo-visual-studio-ninja` | `build_installer_diag.bat`（需 `-DY2K_DIAG_LOG_DEFAULT_ON=ON`） | `dist/Y2Kmeter_Setup_<ver>_Diag_x64.exe`（带 PDB） |

- 语言 C++17；CMake ≥ 3.22；JUCE 8.0.12 vendored（见下）。
- 诊断包 `.dmp` 只能用同一次构建的 PDB 解析；打包后不要再重编译。
- 版本号改动后，增量构建**不会**重写 JUCE 生成的 `*_resources.rc`（EXE/VST3 版本信息仍是旧值）——删 `build/*/JuceLibraryCode/*_resources.rc` 或重新 configure。

## 2. vendored JUCE 补丁管理（重要）

- `third_party/JUCE` 源码直接入库、构建优先（`FETCHCONTENT_SOURCE_DIR_JUCE` 指向它）；`build/msvc` 曾错误指向 `D:/Code/_deps/JUCE`（缺补丁 0005/0006，配置期校验会失败），勿用。
- 补丁流程（改 JUCE 源码时）：
  1. 在 `third_party/JUCE` 源码处加 `[Y2K patch]` 注释（说明原因 + 替代方案）；
  2. 导出 diff 到 `third_party/juce_patches/`（0001~0006）；
  3. 补 `Y2KJucePatches.cmake` 的配置期校验标记；
  4. 更新 `third_party/juce_patches/README.md` 表格。
- 升级 JUCE：按源码内 `[Y2K patch]` 注释逐条回贴补丁。
- 已知本地扩展：`juce::Displays` 的 `withParentAreaOverride`（见 window-layout.md），升级 JUCE 时也要重新应用。

## 3. 版本号同步点（升版本必须全部改到）

1. `CMakeLists.txt`：`project(Y2Kmeter VERSION x.y.z)` + 两个 `juce_add_plugin(... VERSION x.y.z)`（共 3 处）。
2. `Y2Kmeter_installer.iss`：`MyAppVersion` 等版本字段。
3. `PluginEditor.cpp`：`"vx.y.z"` 字面量 3 处 —— `getStringWidth` 一处（含 debug/正式两变体）+ `versionText` 两处。当前约 1055/1056、1098、4629 行（行号会漂移，以 `grep "v3.1.0"` 为准）。
4. `README.md` 徽章、文档版本声明（`PROJECT_OVERVIEW.md` §1.1）。

## 4. `ModuleType` 枚举规则

- **只追加、不重排**（保护旧存档里 `LayoutPreset` id 与字符串映射）。
- **新增模块类型**要同步 8 处（详见 analysis-modules.md「新模块注册」）。
- **删除/替换类型**额外两条：
  - `stringToModuleType` 保留旧字符串 → 新类型映射（旧存档兼容的唯一防线，缺了旧存档模块会静默丢失）；
  - `PerformanceCounterSystem::moduleTypeNameById` 重编号（ID 按枚举序数编排）。

## 5. 何时必须全量构建（否则 `0x80000003` STATUS_BREAKPOINT / ABI 错配）

症状：Debug 正常、Release 窗口弹出前崩；或改了头文件崩溃位置不变。根因是旧 `.obj` 与新 `.h` 类布局/枚举序数不一致。

触发条件：
- `ModuleType` 枚举值重编号（插入/删除中间项 → 后续值 +1）。
- 类成员布局变更（如 `std::array` → `std::vector`、新增成员）。
- 头文件内容变了但时间戳没变（`git checkout` / merge）。
- NMake 空 `.d` 依赖失效 → obj 不重编（仓库有 `_clean_project_objs.ps1` 用于清 obj）。

处置：删除构建目录重配，或强制全量重编相关 `.obj`。

## 6. MSVC 特有坑

| 坑 | 说明 |
| --- | --- |
| `near` / `far` 保留关键字 | 16 位时代遗留；用作标识符会在该行及之后炸一串 C2059/C2143，真正错点只是第一个 |
| 中文源文件编码 | 存 UTF-8 **with BOM**，否则 MSVC 按 GBK 把中文标点认成非法 |
| 块注释禁 `*/` | `glGen*` / `glCompile*` 等标识符会让块注释提前闭合 |
| `static constexpr` 类内声明顺序 | 用作 `std::array` 模板参数时，必须在解析处**之前**已声明（类内成员按文本顺序解析，无"全类可见"） |
| `enum class` 不隐式转 `int` | 需显式 `static_cast<int>(...)` |
| 热路径大结构体 | 100KB+ 的成员数组用 `std::vector` 延迟分配，避免构造期成员初始化顺序问题 |

## 7. CRT

- Release 用 `/MT`（静态 CRT，免装 VC redist）；Debug 用 `/MDd`（`MultiThreadedDebugDLL`）。
- 改 `CMAKE_MSVC_RUNTIME_LIBRARY` 必须删整个 build 目录再 configure（`.rsp` 里已固化 flag）。
- `juceaide` 的 `MSVC_RUNTIME_LIBRARY` 被 JUCE 硬编码为 target 属性，优先级高于全局，需在 `FetchContent_MakeAvailable(juce)` 之后用 `set_target_properties(juceaide ...)` 覆盖。
