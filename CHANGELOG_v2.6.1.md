# Y2Kmeter v2.6.1 开发总结

> 本文档记录 v2.6.1 版本相对 v2.6.0 的开发内容：Milkdrop 后处理效果系统（color / effects 面板）与脱离模式渲染修复。

---

## 1. 背景

v2.6.0 完成脱离态存档恢复与预设控制台交互修复后，本轮聚焦 Milkdrop 后处理效果系统：

- 将 bright 从 effects 面板移回 color 面板；
- 将 effects 面板简化为纯开关按钮；
- 修复脱离模式（floating）下修改任意参数后视频不渲染的 bug；
- 增强 bright / shadows 效果的视觉观感；
- bright 滑块改为非线性映射，使默认值位于中点。

## 2. 核心改动

### 2.1 统一视觉状态 + 持久化

- 新增 `source/ui/modules/MilkdropVisualState.h`：`MilkdropVisualState` 结构体（`tint_r/g/b`、`brightness`、`invert`、`shadows`、`isNeutral()`）。
- `PluginProcessor`：新增 `savedMilkdropVisualState_` 成员与 set/get 接口，`getStateInformation` / `setStateInformation` 序列化到 host state 顶层 XML 属性。
- `PluginEditor`：新增 `SetMilkdropVisualState()` / `GetMilkdropVisualState()`（UI 写 / GL 读，内部加锁），写时同步回 Processor。

### 2.2 color / effects 面板交互

- color 面板：R/G/B 三行 → R/G/B/Bri 四行，bright 从 effects 移回 color；Reset 重置四行。
- effects 面板：简化为 invert / shadows 两个纯开关按钮（按下=开、弹起=关），去掉左侧效果名标签。
- 面板展开期间跳过控制台自动隐藏（`checkOverlayAutoHide`），拖动滑块时刷新 idle 计时器。

### 2.3 bright 效果增强

- shader 从软 gamma `pow(c, 1/brightness)` 改为纯线性增益 `c.rgb *= uBrightness`（对齐 MilkDrop3 的 `ret *= brightness`）。
- 上限从 2 提到 8（对齐 MilkDrop3 的 1~8 范围）。
- bright 滑块非线性映射：分段二次曲线（ease-in / ease-out），`brightness=1.0` 位于滑块中点，两端变化率放缓。

### 2.4 shadows 效果优化

- 从全局平方 `c.rgb *= c.rgb` 改为暗部针对性压暗并保留高光（亮度掩码 `smoothstep` + 平方）。

### 2.5 脱离模式 FBO 渲染修复

- GLView offscreen 渲染路径对齐 Editor 嵌入态：`openglRenderFrameFbo` 前先 bind FBO + viewport + scissor + clear。
- fallback 路径修正：`openglRenderFrame` 内部强制 bind FBO0，改为"先渲染 FBO0 再跨 FBO blit 到 scale_fbo"。
- GLEW `reload()` 时机修正：`Suspend` 无条件重载；`Detach` 仅在恢复 Editor renderer 分支里重载。
- 新增节流诊断日志（前缀 `[MilkdropGLView]`），落盘到 exe 同目录 `Y2Kmeter_debug.log`。

## 3. 踩坑记录

1. **脱离模式非默认值不渲染的根因是 FBO 状态缺失，而非 GLEW**：前几轮误判为 GLEW reload 时机问题，实际根因是 GLView offscreen 路径没有像 Editor 那样在 `openglRenderFrameFbo` 前 bind FBO + clear，导致 `scale_fbo_` 保持全黑，后处理对黑纹理做加性偏移/反相 → 纯色 / 纯黑 / 纯白。
2. **`openglRenderFrame` 内部强制 bind FBO0**：fallback 路径不能预先绑定 scale_fbo 并期望它渲染到该 FBO，必须先渲染到 FBO0 再跨 FBO blit。
3. **bright 软 knee 公式分母错误导致双向变暗**：`c/(1+c*(b-1))` 中的 c 已是乘过增益后的值，b>1 时分母被放大、画面反被压暗。恢复纯线性增益 + 最终 clamp。
4. **bright 线性映射默认值位于最左端**：改为分段二次曲线后，默认值 1.0 位于中点，符合操作直觉。

## 4. 版本号更新（v2.6.0 → v2.6.1）

- `CMakeLists.txt`：`project(Y2Kmeter VERSION 2.6.1 ...)`、`juce_add_plugin(... VERSION 2.6.1 ...)`。
- `Y2Kmeter_installer.iss`：`#define MyAppVersion "2.6.1"`。
- `PluginEditor.cpp`：关于页面标题/抬头 4 处 `"v2.6.0"` → `"v2.6.1"`。
- `PROJECT_OVERVIEW.md`：当前版本标识 `2.6.0` → `2.6.1`。
- `MACOS_ADAPTATION_DIFFS.md`：当前打包版本标识 `v2.6.0` → `v2.6.1`。

## 5. 编译建议

本次改动涉及头文件 `MilkdropModule.h`（新增静态成员函数声明）与新增头文件 `MilkdropVisualState.h`，但未改变类成员布局 / 虚表 / 现有函数签名。理论上增量编译即可；为稳妥起见，发布前建议执行一次 clean 全量构建验证。
