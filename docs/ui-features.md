# VirtuPet / Toybox / 新手引导 / 悬停提示 / i18n

## 1. VirtuPet（电子宠物）

- 音频信号驱动状态机（蛋/孵化/觅食/睡眠/生病/死亡）；`hub.retain(Kind::Loudness)` 只在存在 VirtuPet 模块时保持（见 PROJECT_OVERVIEW.md §4.3）。
- **置顶逻辑有两处**：`hookPanel` 的 `onBroughtToFront` + `mouseDown` 图片聚焦分支；新增可 `toFront` 组件（视频/3D 层）必须同步加入。
- **多宠物**：当前单实例（添加菜单不允许多选同类型），但代码已按"可能有多个"编写，放开无需大改。
- **carried 拖拽状态机**：
  - `mouseDrag` 是 `petPos` 唯一写入路径；新增改 petPos 的路径（定时器/resize）必须加 carried 防御。
  - 拖拽触发用 `toFront(false)`（**不可改 `true`**，non-opaque 组件闪烁根因之一）。
  - HUD 边界用 `playArea.getY()`（hudHeight=64），不要硬编码 64。
- **瞬时状态机入口必须对称排除 egg/hatching**（carried/falling 等）。
- `roleName` 由文件名（`getFileNameWithoutExtension`）提取，资源目录命名变化要同步。
- `onBroughtToFront` 回调里不可操作已删除的 VirtuPet（异步删除需防护）。

## 2. Toybox（玩具盒，`source/ui/toys/`）

- 独立子系统，约 30 文件。
- **档位已是"连续互动自动升级"**（v3.0.1 起），不再是 OFF/CHILL/NORMAL/PARTY 手动四档循环。

## 3. 新手引导（Tutorial Overlay）

- **仅 Standalone**（所有入口 `!isPluginHost` 守卫），插件模式构造时完全跳过。
- **按需创建/销毁是硬约束**：`startTutorial()` 时 `make_unique + addChildComponent`，`dismissTutorialOverlay()` 时 `removeChildComponent + reset()`。**任何预创建常驻 child list 的做法都会导致模块拖拽/缩放/删除失效**（OpenGL 合成层下事件路由被干扰）。
- 首次判定 `!processor.isTutorialCompleted()`（缺省 false）。
- `skipTutorial()` 不标记完成（下次切回 default 预设可重触发）；`completeTutorial()` 标记并持久化。两者不可混用。

## 4. 悬停提示（Hover Help）

- `source/ui/HoverHelp.h`：`y2k::help` 文案库 + `Y2KTooltipWindow`；绘制在 `PinkXPLookAndFeel::drawTooltip`；仪表内容区静止才弹出（详见 PROJECT_OVERVIEW.md §3.3）。

## 5. i18n / 中文本地化

- `source/i18n/Localisation.h`：`y2k::i18n` / `y2k::tr` / `y2k::trf`；语言包 `i18n/*.lang` 注册与合并；查表翻译（精确/数字模板/逐行）；按语言字体；运行时切换广播（详见 PROJECT_OVERVIEW.md §3.3）。
- 新用户默认语言跟随系统。
- macOS 中文显示要点（曾踩坑）：混排文本整串换字体会丢 CJK（需逐 run 化）；glyph advance 略大于图标框会整颗丢弃（♥/★ 空白）；需显式回退字体族并避开 PingFang 缺字。
