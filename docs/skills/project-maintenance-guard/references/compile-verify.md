# Compile Verify（通用编译验证指南）

## 1. 环境要求

| 项 | 值 |
|---|---|
| 操作系统 | Windows 10 / 11 x64 |
| 编译器 | MSVC 14.51（VS 2026 Professional） |
| SDK | Windows Kits 10.0.26100.0 或 10.0.28000.0 |
| CMake | ≥ 3.22 |
| 构建类型 | RelWithDebInfo |
| 生成器 | NMake Makefiles |
| 目标架构 | x64 |
| 独立验证目录 | `build/skill-verify/` |

## 2. 核心原则

- 验证构建使用独立目录 `build/skill-verify/`，不要污染用户 CLion 的构建目录。
- 先做静态检查，再做构建验证。
- `.cpp` 改动优先增量构建；`.h` / `CMakeLists.txt` / 构建参数改动必须完整构建。
- 若验证构建通过但用户运行仍是旧行为，优先怀疑 IDE 构建缓存。

## 3. 脚本说明

### 3.1 完整构建

```bash
cmd //c "docs\skills\project-maintenance-guard\scripts\_bg_build.bat"
```

后台启动完整构建，实际执行 `build_skill_verify.bat`。

### 3.2 增量构建

```bash
cmd //c "docs\skills\project-maintenance-guard\scripts\_bg_incremental_build.bat"
```

前提：`build/skill-verify/CMakeCache.txt` 已存在。

### 3.3 日志位置

```bash
build/skill-verify/_build_log.txt
```

轮询日志尾部即可判断 `[PASS]` / `[FAIL]`。

## 4. 何时使用哪种构建

| 场景 | 处理 |
|---|---|
| 仅改文档 | 跳过 |
| 仅改 `.cpp` | 增量构建 |
| 改 `.h` | 完整构建 |
| 改 `CMakeLists.txt` / 构建选项 | 完整构建 |
| 改宏 / 模板 / 内联函数 / 目标链接设置 | 完整构建 |

## 5. 推荐验证流程

### Step 1：静态检查

- 对改过的源码先做 lints / 编译错误预检
- 若能明确判断是文档或纯注释改动，可跳过构建

### Step 2：后台启动构建

- 完整构建：`_bg_build.bat`
- 增量构建：`_bg_incremental_build.bat`

### Step 3：轮询日志

```bash
tail -10 build/skill-verify/_build_log.txt
grep -E "\[PASS\]|\[FAIL\]" build/skill-verify/_build_log.txt
```

### Step 4：失败定位

```bash
grep -in "error " build/skill-verify/_build_log.txt | head -30
grep -in "LNK" build/skill-verify/_build_log.txt | head -20
```

## 6. 常见问题与经验

### 6.1 `vcvars64.bat` 输出重定向问题

VS 环境初始化脚本不要把详细输出重定向到主日志，否则其子进程可能持有日志句柄，导致后续 CMake / NMake 写日志失败。正确做法是把 `vcvars64.bat` 输出丢到 `nul`，只记录成功或失败结论。

### 6.2 Git Bash 下 `%~dp0` 路径不稳定

从 Git Bash 通过 `cmd //c` 调用批处理时，`%~dp0` 有时不稳定。后台启动器应优先使用项目绝对路径，避免路径推导错误。

### 6.3 头文件改动后的缓存问题

头文件、宏、模板、内联函数变化后，增量构建可能无法覆盖所有依赖。出现以下情况时应建议清理 IDE 构建目录后重新全量构建：

- 修改了 `.h`
- 修改了 `CMakeLists.txt`
- 修改了编译宏或目标属性
- 用户反馈“验证构建通过，但实际运行还是旧行为”

### 6.4 CRT 组合问题

项目 Windows 发布构建默认偏向静态 CRT；若 Debug / JUCE helper target 出现 CRT 组合异常，优先检查：

- `CMAKE_MSVC_RUNTIME_LIBRARY`
- JUCE helper target 是否显式覆盖了 `MSVC_RUNTIME_LIBRARY`
- 是否删除过旧 build 目录并重新 configure

## 7. 与用户 IDE 构建目录的关系

- Skill 验证目录：`build/skill-verify/`
- 用户常用 IDE 构建目录：例如 `cmake-build-relwithdebinfo-visual-studio/`

Skill 构建通过只代表源码和独立验证目录通过；若用户运行的是 IDE 旧产物，仍需用户自行清理对应 IDE 构建目录并重新构建。

## 8. 结论

本指南保留了原技能里最有价值的“完整编译项目经验”：

- 使用独立验证目录
- 使用后台构建避免终端超时
- 使用日志轮询判断结果
- 严格区分增量与全量构建
- 把构建失败与缓存失效问题分开处理

这些经验适用于 Y2Kmeter 主工程的大多数维护任务，而不再绑定某个特定模块。