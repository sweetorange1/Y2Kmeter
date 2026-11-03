# Y2KMeter stick 模式 —— 方案设计

> 背景：MiniMeters 的 reposition → stick，用的是 **Windows AppBar API
> （`SHAppBarMessage`）**——任务栏贴边、保留屏幕工作区的同一套机制。
> 用户顾虑：Y2KMeter 窗口/模块"可随意调整的范围较大"，stick 该以什么形态落地。

---

## 一、结论（推荐）

| 层级 | 推荐形态 | 优先级 |
|------|----------|--------|
| 窗口级（standalone 顶层窗口） | **"软 stick"：屏幕边缘吸附 + 可选置顶**（`SetWindowPos` + `MonitorFromWindow`） | ⭐ 先做 |
| 窗口级（可选高级） | **"硬 stick"：真 AppBar 驻留**（`SHAppBarMessage`） | 后置、仅窄条布局 |
| 模块级（工作区内） | 复用/增强现有 `snapToGrid` + `snapToNeighbours`，加"贴边后锁定" | 顺带 |

**一句话**：Y2KMeter 不需要一开始就上 AppBar（它会全局收缩工作区、和"随意调大小"冲突）；先做**边缘吸附**，AppBar 作为可选的"任务栏式驻留"留给窄条场景。

---

## 二、两种 stick 语义（关键区分）

| | 软 stick（边缘吸附） | 硬 stick（AppBar 驻留） |
|---|---|---|
| 机制 | `SetWindowPos` 把窗口贴到屏幕边 | `SHAppBarMessage(ABM_NEW/SETPOS)` 注册为 AppBar |
| 系统行为 | 普通窗口，其他窗口仍可覆盖 | 系统收缩工作区，其他窗口（含最大化）自动让开这条边 |
| 可否再拖动 | 可随时拖离 | 需 `ABM_REMOVE` 取消 |
| 适合 | 随意大小/位置 | 固定窄条（像任务栏） |
| 风险 | 低 | 中（影响全局桌面布局，需谨慎） |

MiniMeters 的 stick 是**硬 stick**（它那些 meter 就是窄条，贴边后别的窗口让开）。
但 Y2KMeter 是"可随意缩放的面板"，硬 stick 会在窗口变大时占掉大片工作区，
体验反而差——这是"随意调整范围较大"和 AppBar 的天然冲突点。

---

## 三、现状盘点

### 3.1 窗口级（standalone）
- `Y2KStandaloneApp` 有顶层无边框窗口，位置/尺寸持久化到 `window.{x,y,w,h}`。
- `restoreMainWindowBounds()` 已有"显示器 userArea 越界修正"逻辑（找到归属显示器、
  夹回工作区）——**这就是边缘吸附的现成基础**，只需再加"吸附阈值 + 贴边"。

### 3.2 模块级（工作区）
- `ModuleWorkspace` 已有：
  - `snapRect`：8px 网格吸附。
  - `snapToNeighbours`：邻居/工作区边界边缘吸附（阈值 6px）。
  - `snapToGrid`：网格 + 邻居 + 夹回画布。
- 即"拖动时对齐"已经具备，缺的是"吸附后固定（stick/dock）"的语义。

---

## 四、方案设计

### 4.1 层 A：窗口级"软 stick"（推荐先做）

**交互**：拖动窗口，当任一边进入屏幕边缘（工作区边界）的 `STICK_THRESHOLD`（如 8px）
内时，松手即吸附到该边；再次拖动则自动脱离。

**技术要点**：
```
WM_MOVING / 拖动结束（mouseUp）时：
  1. MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) → 归属显示器
  2. GetMonitorInfo → rcWork（工作区，已扣任务栏）
  3. 计算窗口矩形与 rcWork 四条边的距离
  4. 最近边距离 ≤ STICK_THRESHOLD → SetWindowPos 把对应边贴到 rcWork 该边
  5. 可选：吸附后置顶 SetWindowPos(hwnd, HWND_TOPMOST, ...)
```

**与现有代码的关系**：`restoreMainWindowBounds()` 的"归属显示器 + 夹回 userArea"
逻辑可直接复用，抽成 `stickWindowToEdge()`。

**可选增强——置顶**：stick 后常伴随"置顶"需求（参考 MiniMeters 的 Pin To Top）。
用 `SetWindowPos(HWND_TOPMOST)` / `HWND_NOTOPMOST` 切换，配一个 UI 开关。

### 4.2 层 B：窗口级"硬 stick"（AppBar，可选）

仅当用户想要"Y2KMeter 像任务栏一样固定占一条边、别的窗口自动让开"时才启用。

**技术要点**（复刻 MiniMeters）：
```
APPBARDATA abd = { sizeof(abd), hwnd, ... };
SHAppBarMessage(ABM_NEW,    &abd);           // 注册
abd.uEdge = ABE_TOP/BOTTOM/LEFT/RIGHT;        // 选择停靠边
SHAppBarMessage(ABM_SETPOS, &abd);            // 停靠 + 收缩工作区
// 退出时 SHAppBarMessage(ABM_REMOVE, &abd)
```

**约束**：
- 窗口必须符合 AppBar 的"条状"预期（贴一条边），过大窗口会异常。
- 建议：进入 AppBar 模式时强制窗口收缩为"窄条"（如高度压到 ≤ 工作区 1/4）。
- 仅 standalone 有意义（VST3 插件窗口由宿主管理，不能自己 SHAppBarMessage）。

### 4.3 层 C：模块级 stick（顺带增强）

工作区内已有吸附，增强语义即可：
- 拖动模块到工作区边缘 → 吸附并**可选锁定**（locked 状态：不可再拖动，需解锁）。
- 复用 `snapToNeighbours` 的"工作区边界作为对齐目标"逻辑，加一个 `sticky` 标志位。

---

## 五、VST3 插件 vs standalone 的适用性

| 能力 | standalone | VST3 插件 |
|------|-----------|-----------|
| 窗口级边缘吸附（软 stick） | ✅ | ❌（窗口归宿主管） |
| 窗口级 AppBar（硬 stick） | ✅ | ❌ |
| 模块级 stick（工作区内） | ✅ | ✅ |

> VST3 插件窗口由 DAW 宿主拥有，不能自行移动/贴边（会干扰宿主布局）。
> 所以 stick 的窗口级能力**只在 standalone 生效**；模块级能力两者通用。

---

## 六、推荐落地步骤

1. **阶段 1（最小可用）**：standalone 窗口"边缘吸附"（层 A），复用
   `restoreMainWindowBounds` 的显示器归属逻辑，加吸附阈值 + 贴边。UI 加一个
   "Stick" 开关（参考已有 gridBtn 的像素风 TextButton）。
2. **阶段 2（可选）**：吸附后"置顶"开关。
3. **阶段 3（可选）**：窄条布局 + 真 AppBar 驻留（层 B）。
4. **阶段 4（可选）**：模块级 stick 锁定（层 C）。

---

## 七、风险与权衡

| 风险 | 说明 | 缓解 |
|------|------|------|
| AppBar 全局改工作区 | 影响所有应用，体验突兀 | 默认不启用；仅窄条布局开放 |
| 多显示器 | 吸附要归属到正确显示器 | `MonitorFromWindow` 按窗口中心归属 |
| 无边框窗口 | 边缘吸附需手动算 `AdjustWindowRectEx` | 用 `rcWork` 直接贴，不做边框补偿 |
| 与"随意调整"冲突 | 吸附可能让用户"拖不动" | 吸附阈值小（≤8px）+ 拖动即脱离 |

---

## 附：stick 模式的实现依据

- 采用 Windows AppBar（`SHAppBarMessage`，shell32）实现"贴边驻留"；
- 需要持久化的状态：是否启用（开关）+ 停靠边（枚举）+ 窗口矩形；
- 菜单项：Position at Top / Position at Bottom / Stick / Move To Display…；
- 结论：stick = "开关 AppBar 驻留"，停靠边由独立枚举决定。
