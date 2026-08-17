# Y2Kmeter v2.6.5 开发总结

> 本文档记录 v2.6.5 版本相对 v2.6.1 的开发内容：Milkdrop 效果系统架构重构、Shadows 加性叠加（对齐 MilkDrop3）、38 个后处理效果（含第三批 19 个实验性效果）、effects 面板动态网格布局。

---

## 1. 背景

v2.6.1 完成 color/effects 面板与脱离模式渲染修复后，本轮聚焦 Milkdrop 后处理效果系统的深度重构：

- 将散落的效果状态与后处理 pass 抽象为注册表驱动的可扩展效果系统；
- 用加性叠加的 effbottom 实现贴近 MilkDrop3 的 Shadows（原实现画面只是变暗，观感差距大）；
- 引入 efftop/effbottom 二分类对齐 MilkDrop3 Effect Injection 语义；
- 累计落地 38 个开关效果（含第三批 19 个实验性效果）+ effects 面板动态网格布局；
- 修复脱离→切回后 effect 渲染偏移 bug。

## 2. 核心改动

### 2.1 效果系统架构重构

- 新增 `source/ui/modules/MilkdropEffect.h`（header-only）：
  - `MilkdropEffectId` 枚举（19 个效果）；
  - `MilkdropEffectDef` 元数据（id / display_name / implemented / get / set lambda）；
  - `GetMilkdropEffectDefs()` 注册表。
- effects 面板 UI 完全由注册表驱动，新增效果无需改面板布局代码。
- `MilkdropTintPass` 统一 pass 内多效果分支，按注册顺序串行执行。

### 2.2 Shadows 加性叠加（对齐 MilkDrop3）

- 旧实现：单帧暗部压暗（亮度掩码 + 平方）→ 画面只是变暗。
- 新实现：对当前帧的上下翻转位置采样灰度，`ret += pow(gray(flip(uv)), 2)` 加性叠加 → 黑白镜像纹理。
- 单 pass 内 effbottom 分支，执行顺序对齐 MilkDrop3 composite shader。

### 2.3 efftop / effbottom 二分类

- efftop（采样前 uv 重映射）：split、zoom、multi、kaleidoscope、swirl、pinch、pixelate。
- effbottom（采样后 ret 变换）：invert、shadows、solarize、rainbow、blow、burn、glitch、posterize、sepia、grayscale、edge、vignette。

### 2.4 19 个效果清单

| 效果 | 类型 | 逻辑 |
|---|---|---|
| invert | effbottom | ret = 1 - ret |
| shadows | effbottom | ret += pow(gray(flip(uv)), 2) |
| solarize | effbottom | ret = ret*(1-ret)*4 |
| split | efftop | uv = (abs(uv.x-0.5), uv.y) |
| zoom | efftop | uv = 0.25 + 0.5*uv |
| multi | efftop | uv 多重折叠 |
| rainbow | effbottom | 程序化彩虹染色 |
| blow | effbottom | ret += blur(uv) 加性模糊 |
| burn | effbottom | color burn 近似 |
| kaleidoscope | efftop | 极坐标角度折叠（π/3） |
| swirl | efftop | 绕中心旋转 |
| pinch | efftop | 径向缩放（鱼眼） |
| pixelate | efftop | uv 量化 24×24 |
| glitch | effbottom | RGB 通道微偏移采样 |
| posterize | effbottom | ret 量化 8 级 |
| sepia | effbottom | sepia 颜色矩阵 |
| grayscale | effbottom | 亮度加权灰度 |
| edge | effbottom | 邻域差分边缘检测 |
| vignette | effbottom | 强暗角 + 桶形畸变 |

### 2.5 effects 面板动态网格布局

- 按钮根据模块宽度动态决定每行列数，多按钮一行。
- 面板高度由实际行数动态决定。
- 修复按钮被挤窄：先扣间距再均分，余数补给最后一列。

### 2.6 脱离→切回 effect 渲染偏移修复

- 根因：`openGLContextClosing()` 漏重置 `milkdrop_post_w_/h_`，post FBO 重建后尺寸缓存残留导致跳过附件绑定，得到残缺 framebuffer。
- 修复：销毁 post FBO 后补 `milkdrop_post_w_ = 0; milkdrop_post_h_ = 0;`。

### 2.7 vignette 强化

- 从简单径向暗角改为强暗角（起止 0.2~0.72、平方衰减、强度 0.95）+ 桶形畸变扭曲。

### 2.8 第三批 19 个实验性效果

在前两批基础上新增 19 种更"疯狂"的实验性/迷幻效果，累计达 38 个。遵循注册表驱动 + efftop/effbottom 二分类，无需改面板布局。

| 效果 | 类型 | 核心逻辑 | 视觉效果 |
|---|---|---|---|
| tunnel | efftop | 极坐标 `depth=1/(0.3+r*2)` 映射 uv | 无限纵深隧道 |
| ripple | efftop | `uv += p*sin(r*30)*0.06` 径向正弦扰动 | 同心水波涟漪 |
| melt | efftop | `uv.y += (1-uv.y)*sin(uv.x*20)*0.25` | 画面向下融化 |
| fisheye | efftop | `uv = p*(1+0.8*r²)+0.5` 强桶形畸变 | 超广角鱼眼 |
| noise_warp | efftop | `uv += (sin,cos)` 组合伪噪声偏移 | 高频噪波蠕动 |
| mirror_maze | efftop | `uv = abs(fract(uv*3)*2-1)` 递归折叠 | 无限镜像嵌套 |
| fragment | efftop | 12×12 网格切块 + hash 随机错位 | 像素碎片打乱 |
| spiral | efftop | `atan(p.y,p.x)+r*8` 复合旋转 | 强烈螺旋卷曲 |
| twist | efftop | 沿 y 轴旋转 `p.y*6` | 垂直扭转 |
| color_shift | effbottom | RGB 通道 ±0.03 分离采样 ×1.6 | 色彩爆炸撕裂 |
| neon | effbottom | 亮部提取 + 邻域模糊平方叠加 | 霓虹光晕 |
| thermal | effbottom | 亮度映射蓝→红→黄热力色谱 | 热成像 |
| acid | effbottom | `abs(c-0.5)*2` + 绿增蓝减 | 酸性迷幻撞色 |
| vhs | effbottom | 横向条纹 + RGB 错位 + 高频噪点 | VHS 录像带故障 |
| crt | effbottom | 正弦扫描线 + 隔行暗化 | CRT 扫描线 |
| duotone | effbottom | 亮度映射双色（深紫→橙黄） | 双色调艺术 |
| bloom | effbottom | 亮部阈值 + 邻域模糊 ×3 叠加 | 强泛光爆发 |
| binary | effbottom | `step(0.5, l)` 硬阈值二值化 | 极端黑白剪影 |
| prismatic | effbottom | 径向 `dir*0.02` 红蓝分离 + 增益 | 棱镜色散 |

- efftop 执行链（采样前）：`split → zoom → multi → kaleidoscope → swirl → pinch → pixelate → tunnel → ripple → melt → fisheye → noise_warp → mirror_maze → fragment → spiral → twist`。
- effbottom 执行链（采样后）：`shadows → invert → solarize → rainbow → blow → burn → glitch → posterize → sepia → grayscale → edge → vignette → color_shift → neon → thermal → acid → vhs → crt → duotone → bloom → binary → prismatic`。

## 3. 踩坑记录

1. **Shadows 观感差距是语义错误而非强度**：MilkDrop3 Shadows 是"翻转灰度 pow 后加性叠加"，旧实现是"暗部平方乘法压暗"，语义不同导致画面只是变暗。
2. **销毁 GL 资源必须同步清零尺寸缓存**：post FBO 重建时残留 `milkdrop_post_w_/h_` 使重建逻辑误判，跳过附件绑定。
3. **网格布局 gap 未先扣除**：`cell_w = avail_w / cols` 未扣间距，却用 `col * (cell_w + gap)` 累加偏移，导致最后一列被挤窄。

## 4. 版本号更新（v2.6.1 → v2.6.5）

- `CMakeLists.txt`：`project(Y2Kmeter VERSION 2.6.5 ...)`、`juce_add_plugin(... VERSION 2.6.5 ...)`。
- `Y2Kmeter_installer.iss`：`#define MyAppVersion "2.6.5"`。
- `PluginEditor.cpp`：关于页面标题/抬头 4 处 `"v2.6.1"` → `"v2.6.5"`。
- `PROJECT_OVERVIEW.md`：当前版本标识 `2.6.1` → `2.6.5`。
- `MACOS_ADAPTATION_DIFFS.md`：当前打包版本标识 `v2.6.1` → `v2.6.5`。

## 5. 编译建议

本次改动涉及头文件 `MilkdropEffect.h`（新增）、`MilkdropVisualState.h`、`MilkdropModule.h`，且新增了 19 个 shader uniform 与效果分支，**发布前需执行一次 clean 全量构建**以刷新头文件依赖与 shader 编译缓存。
