# Compile Verify（编译验证指南）

> 正式构建（Release 包 / 诊断包 / 安装包）的命令与目录见 [docs/build-toolchain.md](../../build-toolchain.md) §1。本文件只负责**改完代码后"能不能编过"的快速验证**。

## 1. 环境要求

| 项 | 值 |
|---|---|
| 操作系统 | Windows 10 / 11 x64 |
| 编译器 | MSVC 14.51（VS 2026） |
| CMake | ≥ 3.22 |
| 构建类型 | RelWithDebInfo |
| 生成器 | NMake Makefiles |
| 目标架构 | x64 |
| 独立验证目录 | `build/skill-verify/`（不污染 IDE 构建目录） |

## 2. 核心原则

- 验证构建写在 `build/skill-verify/`，用户的 CLion 目录（`cmake-build-*`）不动。
- 先静态检查（lints / 明显语法），再构建。
- `.cpp` 改动 → 增量；`.h` / CMake / 宏 / 模板 / 内联 / 枚举序数 / 类布局 → **完整**。
- 验证通过 ≠ 用户运行正确：用户跑的是自己的构建产物。

## 3. 脚本

| 目的 | 命令 |
|---|---|
| 完整构建（首次 / 头文件改动） | `cmd //c "docs\skills\project-maintenance-guard\scripts\_bg_build.bat"` |
| 增量构建（只改 `.cpp`） | `cmd //c "docs\skills\project-maintenance-guard\scripts\_bg_incremental_build.bat"` |
| 日志 | `build/skill-verify/_build_log.txt` |

后台启动器会用 `%~dp0` 反推项目根目录，然后把日志重定向到上面这个文件；轮询日志尾部即可判断 `[PASS]` / `[FAIL]`。

## 4. 何时必须完整构建（否则可能 `0x80000003` STATUS_BREAKPOINT / ABI 错配）

- `ModuleType` 枚举插值 / 重排（后续枚举值整体偏移）
- 类成员布局变化（`std::array` ↔ `std::vector`、增删成员）
- 头文件内容变了但时间戳没变（`git checkout` / merge / 分支切换）
- `CMakeLists.txt` / 编译宏 / 目标链接选项
- 版本号改动（顺带会牵扯 `*_resources.rc`）

处置：删 `build/skill-verify/` 重新 configure，或强制全量重编相关 `.obj`（仓库有 `_clean_project_objs.ps1`）。

## 5. 推荐验证流程

### Step 1 静态检查

- `read_lints` 检查改动文件；顺手 `grep` 旧 API / 旧常量残留。

### Step 2 选增量或全量

按 §4 判定；不确定就全量。

### Step 3 后台启动 + 轮询

```bash
cmd //c "docs\skills\project-maintenance-guard\scripts\_bg_incremental_build.bat"
tail -10 build/skill-verify/_build_log.txt
grep -E "\[PASS\]|\[FAIL\]" build/skill-verify/_build_log.txt
```

### Step 4 失败定位

```bash
grep -in "error " build/skill-verify/_build_log.txt | head -30
grep -in "LNK"   build/skill-verify/_build_log.txt | head -20
```

## 6. 常见问题与经验

### 6.1 `vcvars64.bat` 输出重定向

VS 环境初始化脚本不要把详细输出重定向到主日志，否则其子进程可能持有句柄，导致后续 CMake / NMake 写日志失败。脚本里已把 vcvars 输出丢到 `nul`，只记结论。

### 6.2 Git Bash 下 `%~dp0` 不稳定

从 Git Bash 通过 `cmd //c` 调批处理时 `%~dp0` 有时解析异常。后台启动器已改为"先 `for %%i in ("%~dp0..\..\..\..") do set ROOT=%%~fi` 再 `cd /d`"，若路径仍不对，直接改用绝对路径调用。

### 6.3 头文件改动后的缓存问题

改 `.h` / 宏 / 模板 / 内联后，增量可能覆盖不到所有依赖。出现"验证构建通过但运行还是旧行为"时，清理 IDE 构建目录后重配 + 全量。

### 6.4 版本号改了但 EXE 版本信息是旧的

JUCE 生成的 `JuceLibraryCode/*_resources.rc` 只在 configure 时生成；删掉该文件或重新 configure 即可（build-toolchain §1）。

### 6.5 CRT 组合问题

Release 走 `/MT`、Debug 走 `/MDd`。异常时检查 `CMAKE_MSVC_RUNTIME_LIBRARY`、`juceaide` 的 target 属性覆盖、以及是否残留旧 build 目录（详见 build-toolchain §7）。

### 6.6 文件编码

含中文的源文件必须是 UTF-8 **with BOM**，否则 MSVC 按 GBK 解析会报 C2xxx（build-toolchain §6）。

## 7. 与用户 IDE 构建目录的关系

- Skill 验证目录：`build/skill-verify/`
- 用户常用目录：`cmake-build-release-visual-studio`、`cmake-build-relwithdebinfo-visual-studio-ninja`

Skill 构建通过只代表源码在独立目录能编过；用户实际运行的是自己目录里的产物。
