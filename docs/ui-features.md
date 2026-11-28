# VirtuPet / Toybox / 新手引导 / 悬停提示 / i18n

## 1. VirtuPet（电子宠物）

- 音频信号驱动状态机（蛋/孵化/觅食/睡眠/生病/生命值耗尽）；`hub.retain(Kind::Loudness)` 只在存在 VirtuPet 模块时保持（见 PROJECT_OVERVIEW.md §4.3）。
- **置顶逻辑有两处**：`hookPanel` 的 `onBroughtToFront` + `mouseDown` 图片聚焦分支；新增可 `toFront` 组件（视频/3D 层）必须同步加入。
- **多宠物**：当前单实例（添加菜单不允许多选同类型），但代码已按"可能有多个"编写，放开无需大改。
- **carried 拖拽状态机**：
  - `mouseDrag` 是 `petPos` 唯一写入路径；新增改 petPos 的路径（定时器/resize）必须加 carried 防御。
  - 拖拽触发用 `toFront(false)`（**不可改 `true`**，non-opaque 组件闪烁根因之一）。
  - HUD 边界用 `playArea.getY()`（hudHeight=64），不要硬编码 64。
- **瞬时状态机入口必须对称排除 egg/hatching**（carried/falling 等）。
- `roleName` 由文件名（`getFileNameWithoutExtension`）提取，资源目录命名变化要同步。
- `onBroughtToFront` 回调里不可操作已删除的 VirtuPet（异步删除需防护）。
- **布局恢复单次 PNG 解码（v3.5.0）**：`VirtuPetModule(bool deferInitialRoleLoad = false)` 与 RAII 守卫 `VirtuPetModule::ScopedDeferInitialRoleLoad` 供 `ModuleWorkspace::loadLayoutFromTree()` 包裹 `factory(ModuleType::virtuPet)` 使用，跳过构造期随机角色 ~134 张 PNG 解码，由紧随其后的 `restorePersistentState()` 直接解码目标存档角色一次；新建添加、存档角色为空或历史角色缺失时均自动回退 `loadRandomRoleAnimations()`。

## 2. Toybox（玩具盒，`source/ui/toys/`）

- 独立子系统，约 30 文件。
- **档位已是"连续互动自动升级"**（v3.0.1 起），不再是 OFF/CHILL/NORMAL/PARTY 手动四档循环。
- **`EmojiAtlas` 后台预烘焙（v3.5.0）**：Windows 启用低优先级后台线程 `PrebakeWorker`（`Priority::low`，启动延后 150ms 避开首帧初始化）配合每尺寸 1 套共 3 套复用 D2D/WIC 软目标（S/M/L = 38/50/70px；291 字形 + 描边约 3.1× 提速，UI 线程阻塞 0ms）。通过值拷贝 `ThemeBakeParams` + `themeEpoch_` 无锁应对主题切换，优先烘焙当前主题池并单趟产出 duotone 描边与 GL 预乘 RGBA；热路径全原子无锁读取，冷启动极早期点击走单槽按需回退，并提供 `prebakeDone()` / `prebakeMs()` 查询。
- **`GlToyRenderer` GPU 批渲染路径（v3.5.0）**：`ToyKit.*` 保持零 OpenGL 头文件依赖并产出 `ToyGlTypes.h` 纯 POD 快照，由 `GlToyRenderer` 在 `renderOpenGLPostComponentBuffer()` 中基于单张 `1024×1024` 图集以单次批绘制叠加到组件缓冲之上（零 `ToyOverlay` 重绘），shader 内按排除矩形列表遮挡调色板/抽屉/弹窗等浮层；GL 脱离时自动回退 `ToyOverlay`；诊断快照新增 `ToyClock::DiagSnapshot::degradedFreeActors`，准确区分真正降级粒子与飞出画布上边缘视野外的粒子。
- **目标帧率相对迟滞降级（`DegradationController`，v3.5.0）**：按目标刷新率比例做迟滞判定，支持可逆平滑恢复；GL 批渲染路径下满载 400 粒子仍可维持全量高细节（`degradedFreeActors == 0`）。
- **工作区调色板懒创建与画布点阵优化（v3.5.0）**：`ModuleWorkspace::palettePanel` 推迟至首次点击主题栏预览块 / `"+"` 时经 `ensurePalettePanel()` 按需构造（所有引用点均已空指针防护）；`rebuildCanvasBgCacheIfNeeded()` 改用 `BitmapData` + `PixelARGB::blend` 直接写入网格点阵，消除 26,000+ 次逐点 `fillRect`。

## 3. 新手引导（Tutorial Overlay）

- **仅 Standalone**（所有入口 `!isPluginHost` 守卫），插件模式构造时完全跳过。
- **按需创建/销毁是硬约束**：`startTutorial()` 时 `make_unique + addChildComponent`，`dismissTutorialOverlay()` 时 `removeChildComponent + reset()`。**任何预创建常驻 child list 的做法都会导致模块拖拽/缩放/删除失效**（OpenGL 合成层下事件路由被干扰）。
- 首次判定 `!processor.isTutorialCompleted()`（缺省 false）。
- `skipTutorial()` 不标记完成（下次切回 default 预设可重触发）；`completeTutorial()` 标记并持久化。两者不可混用。

## 4. 底部主题栏（`ThemeSwatchBar`）

- 结构：`[当前主题预览块] [预设色票 ×12] [我的主题 ×N] [+] [SKIN] [主题名]`；点预览块 / "+" 展开调色工作室，点 SKIN 弹出视觉风格菜单。
- **宽度优先级（v3.4.3 bug1）**：`SKIN > "+" > …收纳按钮 > 我的主题 > 预设色票`。`reservedTailWidth()` 先扣掉"+"、SKIN 与右侧内边距 `kBarRightPad`，剩余宽度由 `visibleUserCount()` / `visiblePresetCount()` 分配 —— 窗口变窄时**先少显示预设色票**，SKIN 按钮永远在栏内，不会再被右侧控制区挡住。新增栏内控件必须并入 `reservedTailWidth()`。
- **装不下的预设不能"消失"（v3.4.3 补充）**：`presetOverflowVisible()` 为真时，最后一个可见色票右侧画一个"…"按钮，点开是全部官方预设的菜单（条目用该预设代表色作文字色）；极窄时宁可一个色票都不画也要保留该入口。几何口径统一在 `presetStartX()` / `presetAvailWidth()` / `presetOverflowVisible()`，不要在别处重算。
- **强制英文 UI 字体**：按钮设属性 `getProperties().set ("y2k.iconFont", true)`，`drawButtonText` 与宽度测量都会整颗用 `SymbolGlyphs::iconFont`（中文界面下不再落到没有粗体的中文字体）。计算器全键盘即按此处理 —— 纯拉丁/符号控件都该这么做。
- SKIN 菜单与右侧 ComboBox 下拉统一走 `PinkXP::withFullScreenMenuArea()`（stick 驻留 / mac 原生全屏 / 控件不在 `userArea` 内时把允许区域放宽到整块显示器），否则全屏下菜单会被 clamp 到离按钮很远的地方（v3.4.3 bug2）。

## 5. 悬停提示（Hover Help）

- `source/ui/HoverHelp.h`：`y2k::help` 文案库 + `Y2KTooltipWindow`；绘制在 `PinkXPLookAndFeel::drawTooltip`；仪表内容区静止才弹出（详见 PROJECT_OVERVIEW.md §3.3）。

## 6. i18n / 中文本地化

- `source/i18n/Localisation.h`：`y2k::i18n` / `y2k::tr` / `y2k::trf`；语言包 `i18n/*.lang` 注册与合并；查表翻译（精确/数字模板/逐行）；按语言字体；运行时切换广播（详见 PROJECT_OVERVIEW.md §3.3）。
- 新用户默认语言跟随系统。
- macOS 中文显示要点（曾踩坑）：混排文本整串换字体会丢 CJK（需逐 run 化）；glyph advance 略大于图标框会整颗丢弃（♥/★ 空白）；需显式回退字体族并避开 PingFang 缺字。
