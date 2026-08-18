# Y2Kmeter v2.6.7 开发总结

> 本文档记录 v2.6.7 版本相对 v2.6.6 的开发内容：将 Milkdrop 的 **tweak 面板从「预设注入 per_frame 变量」重构为「后处理层 uv 几何畸变」**，实现实时生效、不修改预设、对所有预设生效，并新增 3 个整数**万花镜对称控制器**。

---

## 1. 背景

v2.6.6 的 tweak 面板通过向 `.milk` 预设文本追加 `per_frame_<N>=zoom=zoom+delta; rot=rot+delta; ...` 实现「最终值偏移」。该方案存在两个根本缺陷：

1. **仅对引用了这些变量的预设生效**：`zoom / rot / warp / sx / sy` 等是 MilkDrop 的 **warp shader uniform**，引擎只负责传值，用不用完全看预设作者写的 shader。大量「波形占比小」的几何/粒子/隧道类预设根本不读这些变量，导致 tweak 无效。
2. **必须重载预设**：注入发生在加载阶段，改一次参数就要重新 `projectm_load_preset_data`，画面会「跳一下」，无法实时拖拽。

本轮将 tweak 的作用位置从「预设 warp shader」搬到「projectM 输出之后的后处理 pass（`MilkdropTintPass`）」，在采样最终画面纹理前对 **uv** 做几何重映射。后处理对最终输出画面生效，与预设内部 shader 内容无关，因此**每一个预设都 100% 生效**，且 tint pass 每帧读取最新视觉状态，实现**逐帧实时**调整。

## 2. 核心改动

### 2.1 tweak 参数迁移到后处理 uv 重映射

- 9 个 tweak 参数改由 `MilkdropTintPass` 后处理着色器消费，在采样 projectM 输出纹理前对 `uv` 做几何变换。
- 数据作为 `MilkdropVisualState::offset` 成员（`MilkdropVisualOffsetState`），随视觉状态每帧传递到 `apply()`，嵌入态（Editor）与浮动态（GLView）均实时生效。

### 2.2 参数语义重构（最终 7 浮点 + 3 整数）

**连续浮点控制器（偏移量，0 = 中性）**：

| 参数 | uv 变换 | 说明 |
|---|---|---|
| zoom | `p /= 1 + zoom*4` | 整体缩放（下限 -0.3） |
| rot | 绕中心旋转 `±2π` | 整体旋转 |
| warp | `atan + warp*r*12` | 径向扭曲漩涡 |
| dx / dy | `p += (dx, dy)` | 水平/垂直平移 |
| sx / sy | `p *= exp(-s*1.5)` | 指数映射：负值拉伸、正值压缩、永不镜像 |

**整数万花镜对称控制器（1~16，滑块）**：

| 控制器 | 对称规则 |
|---|---|
| kaleido | 径向角度万花镜：围绕中心按极坐标把角度折叠成 N 瓣（`wedge = π/N`），形成 N 重旋转对称 |
| fold x | 水平对称折叠：沿 X 方向切成 N 段并镜像（三角波折叠） |
| fold y | 垂直对称折叠：沿 Y 方向切成 N 段并镜像 |

三者可叠加，组合出复杂网格状万花镜图案。

### 2.3 删除 cx / cy

- 原 `cx/cy` 语义在改造过程中曾一度与 `dx/dy` 重复（数学等价），故最终删除，只保留 `dx/dy` 作为平移。
- 从数据结构、shader uniform、面板 UI、持久化字段中完全移除。

### 2.4 数据结构重构

- `MilkdropVisualOffsetState` 结构体重构为 `7 个 float value[]` + `3 个 int ivalue[]`，新增 `GetVisualOffsetIntParams()` 元数据。
- 删除 `FindMaxPerFrameIndex()` / `ApplyVisualOffsetsToPresetText()` 注入函数（不再需要）。
- offset 状态并入 `MilkdropVisualState`，删除 `PluginEditor` / `PluginProcessor` 中独立的 `milkdrop_offset_state_` / `savedMilkdropVisualOffsetState_` 及 `Set/GetMilkdropVisualOffsetState()`。

### 2.5 shader 扩展（GLSL 150 / 120 两处同步）

- 新增 9 个 tweak uniform：`uTweakZoom/Rot/Warp/Dx/Dy/Sx/Sy`（浮点）+ `uTweakKaleido/FoldX/FoldY`（整数，以 float 传入）。
- tweak 变换块：缩放 → 旋转 → 拉伸 → 平移 → 径向扭曲 → 万花镜对称，全部组合在单个 pass 内。

### 2.6 整数控制器 UI（滑块）

- 三个整数控制器最终采用**横向滑块**（非 stepper 按钮），拖动实时生效。
- 范围 `1~16`（不含 0，避免「0 和 1 效果一样」）。
- shader 判断阈值从 `>= 2.0` 改为 `>= 1.0`，使 `1` 成为第一个有效档位。

## 3. 踩坑记录

1. **tweak 注入方案对「不引用变量的预设」无效**：`zoom/rot/warp` 是 warp shader uniform，引擎只传值不消费，用不用取决于预设 shader 是否写了这些变量。这是 tweak「效果弱」的根本原因，也是必须迁移到后处理 uv 层的原因。
2. **cx/cy 与 dx/dy 语义重复**：改造中曾把 cx/cy 实现为「平移」，结果与 dx/dy 数学等价（`p += vec2(dx + cx, dy + cy)`），用户调二者效果一样。最终删除 cx/cy。
3. **sx/sy 负值镜像**：线性 `p.x *= 1 + sx*2` 在 sx < -0.5 时系数为负产生镜像（表现为「反向压缩」），改为指数映射 `exp(-s*1.5)` 恒为正、永不镜像。
4. **zoom 下限无意义**：`z = 1 + zoom*4` 在 zoom < -0.25 后 `max(z, 0.01)` 已 clamp 到底，继续调无意义，故将滑块下限定为 -0.3。
5. **整数 0 与 1 效果相同**：shader 判断阈值原为 `>= 2.0`，导致 `1` 不生效；改为 `>= 1.0` 后 `1` 成为首个有效档位，滑块范围也去掉 `0`。

## 4. 版本号更新（v2.6.6 → v2.6.7）

- `CMakeLists.txt`：`project(Y2Kmeter VERSION 2.6.7 ...)`、`juce_add_plugin(... VERSION 2.6.7 ...)`。
- `Y2Kmeter_installer.iss`：`#define MyAppVersion "2.6.7"`。
- `PluginEditor.cpp`：关于页面标题/抬头 4 处 `"v2.6.6"` → `"v2.6.7"`。
- `PROJECT_OVERVIEW.md`：当前版本标识 `2.6.6` → `2.6.7`，并追加 v2.6.7 章节。
- `MACOS_ADAPTATION_DIFFS.md`：当前打包版本标识 `v2.6.6` → `v2.6.7`。

## 5. 编译建议

本次改动涉及头文件 `MilkdropVisualOffsetState.h`（结构体重构）、`MilkdropVisualState.h`（新增 offset 成员）、`PluginProcessor.h`、`PluginEditor.h`、`MilkdropModule.h`，以及 `MilkdropModule.cpp` 的 shader 代码，**发布前需执行一次 clean 全量构建**以刷新头文件依赖。
