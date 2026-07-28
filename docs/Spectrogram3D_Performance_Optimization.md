# Spectrogram3D 回退后性能优化报告

> 日期：2026-07-28 | 版本：v2.2.5

---

## 1. 背景

Spectrogram3D 模块从 GPU 渲染路径回退为纯 CPU 实现（b41f0a5）后，在实际运行中出现性能退化：
- 启动初期可稳定跑满 60fps；
- 持续运行一段时间后，帧率逐渐下降，最终跌落并稳定在 30fps，未再恢复。

本文档基于性能测试报告（`I:/Y2KMeter/性能测试/intel/`）定位瓶颈、实施优化，并记录验证结论。

---

## 2. 性能测试数据分析

### 2.1 测试条件

- 三组 60 秒采样窗口：13:16、13:17、13:18
- 模块布局：Spectrogram3D + Oscilloscope ×2 + Spectrum + Dynamics + Waveform + VU Meter + EQ 等共 9 个模块
- CPU：Intel i7（8 核 16 线程），约 3.6GHz
- 构建：Release with Debug Info

### 2.2 各模块 paint 耗时对比（单位：ms avg）

| 模块 | 13:16 | 13:17 | 13:18 |
|------|:----:|:----:|:----:|
| **spectrogram3d** | **15.8** | **16.3** | **16.1** |
| oscilloscope | 2.5 | 3.2 | 3.1 |
| oscilloscopeWave | 2.3 | 2.5 | 2.5 |
| spectrum | 1.9 | 1.9 | 2.0 |
| waveform | 0.4 | 0.4 | 0.4 |
| vuMeter | 0.25 | 0.27 | 0.28 |
| dynamics | 0.3 | 0.3 | 0.3 |

Spectrogram3D 单个 paint 调用 **平均 16ms**，占全部 UI paint 总耗时的 **~59%**。

### 2.3 Spectrogram3D 帧超限情况

| 指标 | 13:16 | 13:17 | 13:18 |
|------|:----:|:----:|:----:|
| paint >16ms 占比 | 50.9% | 56.6% | 53.6% |
| paint max | 20.6ms | 44.6ms | 61.9ms |
| repaint 请求/分钟 | 1472 | 1472 | 1462 |
| repaint 合并次数 | 135 | 112 | 100 |

**关键发现**：即使在 33ms repaint 节流下，仍有 >50% 的 paint 调用超出 16ms。在 16.67ms 的 60fps 帧预算中，Spectrogram3D 一个模块就几乎消耗了整个预算。

### 2.4 UI 线程 CPU 演变

| 指标 | 13:16 | 13:17 | 13:18 |
|------|:----:|:----:|:----:|
| UI 线程 CPU 占比 | 62.5% | 65.3% | 66.9% |
| 音频线程 CPU 占比 | 2.3% | 2.3% | 2.3% |
| 帧分发器 Hz | 36 | 30 | 31 |

UI 线程负载随时间缓慢上升，帧分发频率从 36Hz 降至 ~30Hz，与"启动满帧→衰减→稳定 30fps"的现象一致。

---

## 3. 根因分析

### 3.1 直接原因：single paint 太重

Spectrogram3D 的 `renderToImage()` 每帧执行：
- **150 层** 深度遍历 × **128 bins** = **19,200 次** `juce::Graphics::fillRect()`
- 150 层 × 1 条 **`strokePath()`**
- 1 次 `PinkXP::drawSunken()` 底衬填充

JUCE 的 `SoftwareRendererImage`（Windows D2D / macOS CoreGraphics）中，每次 `fillRect` 都涉及：
1. 状态检查与合并
2. 裁剪区域求交
3. 像素写入与 alpha 混合

19,200 次 fillRect 的累积开销在 384×256 模块下约 **12-14ms**（加上 strokePath、底衬绘制共 ~16ms）。

### 3.2 衰减机制

```
onFrame 数据到达 → imageCacheDirty = true → repaint (33ms节流)
→ paintContent → renderToImage (16ms) → drawImage (<1ms)
→ drawAxisLabels (<1ms)
```

1. 初期 `effLen <= 0`，`renderToImage` 提前返回 → paint 极快
2. 历史数据积累到 150 层后，renderToImage 开始全量执行 → paint 16ms
3. 其余模块合计 ~7ms → 帧总 paint ≈ 23ms，远超 16.67ms 预算
4. JUCE 消息泵检测到处理超时 → 跳过中间帧 → 帧频降至 ~30fps
5. 30fps 下帧预算 33ms，paint 23ms ≈ 70% 占用 → 达成新的勉强平衡

### 3.3 排除项

| 怀疑方向 | 结论 |
|----------|------|
| 内存泄漏 | ❌ 三窗口 alloc/free 完全平衡，`allocHz == freeHz` |
| 锁竞争 | ❌ 总等待 < 3ms/窗口，max 单次等待 < 44μs |
| 音频线程过载 | ❌ 仅 2.3% CPU，低于 UI 线程两个数量级 |
| 对象分配膨胀 | ❌ alloc 频率与 `dataPublishLatestFrame` 一致（36→31Hz），下降而非增长 |
| Frame Listener 数量泄漏 | ❌ 始终为 9，稳定不变 |

---

## 4. 优化措施

### 4.1 visibleRows 150 → 100（P5-1）

**文件**：`source/ui/modules/Spectrogram3DModule.h` L146

```cpp
// 修改前
static constexpr int visibleRows = 150;
// 修改后
static constexpr int visibleRows = 100;
```

**效果**：fillRect 从 19,200 次/帧降至 **12,800 次/帧**（-33%），paint 理论耗时从 16ms 降至 **~10.5ms**。

**代价**：频谱瀑布的历史可见深度减少 33%（150→100 层）。在 340×157 的典型模块尺寸下，层深由 4.5 像素/层缩小到 3.0 像素/层，视觉效果仅略微密集，未破坏 isometric 纵深感。

### 4.2 renderToImage 重建节流至 ~20fps（P5-2）

**文件**：`source/ui/modules/Spectrogram3DModule.h` L114，`Spectrogram3DModule.cpp` L336-352

新增成员 `double lastRenderToImageMs = 0.0`。

`paintContent` 中 `renderToImage` 调用增加 50ms 最小间隔限制：

```cpp
// 修改前：每次 imageCacheDirty=true 立即重建
if (imageCacheDirty || ...) {
    renderToImage(canvas);
    imageCacheDirty = false;
}

// 修改后：限制重建频率 ~20fps，尺寸变化时立即重建
const bool needRebuild = imageCacheDirty || ...;
if (needRebuild) {
    const double now = juce::Time::getMillisecondCounterHiRes();
    if ((now - lastRenderToImageMs) >= 50.0
        || cachedCanvasW != canvas.getWidth()
        || cachedCanvasH != canvas.getHeight())
    {
        renderToImage(canvas);
        imageCacheDirty = false;
        lastRenderToImageMs = now;
    }
}
```

**效果**：renderToImage 从 ~30fps 降至 ~20fps，CPU 时间再减 **~33%**。paint 理论耗时从 10.5ms 降至 **~7ms**。

**原理**：`drawImage` blit 仍随 `repaint` 频率执行（~30fps），但离屏 Image 内容只在新数据积累 50ms 后才重建。频谱瀑布的滚动速度为 60px/s，50ms 对应 3px 位移——视觉上无明显卡顿感。这是 CPU 渲染的经典优化：渲染到离屏缓冲的帧率可以低于显示帧率，人眼对渐进移动的频谱流不敏感。

### 4.3 综合效果预估

| 指标 | 优化前 | 优化后(预估) | 降幅 |
|------|:----:|:----:|:--:|
| Spectro3D paint avg | 16ms | ~7ms | -56% |
| fillRect/帧 | 19,200 | 12,800 × 0.67 | -56% |
| renderToImage/秒 | ~30次 | ~20次 | -33% |
| UI 线程总 paint | ~23ms | ~14ms | -39% |
| 60fps 帧预算利用率 | 138% | 84% | -- |
| 预期稳定帧率 | 30fps | **60fps** | -- |

---

## 5. 修改清单

| 文件 | 行 | 变更 |
|------|-----|------|
| `Spectrogram3DModule.h` | L146 | `visibleRows`: 150 → 100 |
| `Spectrogram3DModule.h` | L113-114 | 新增 `lastRenderToImageMs` 成员 |
| `Spectrogram3DModule.cpp` | L301 | 更新注释 `19k→12.7k` fillRect |
| `Spectrogram3DModule.cpp` | L336-352 | `paintContent` 中 `renderToImage` 增加 50ms 节流 |

---

## 6. 验证与后续

### 6.1 待验证

- 重新编译并运行为期 3 分钟的稳定性测试，确认 60fps 不再跌落至 30fps
- 观察 Spectro3D paint avg 是否降至 ~7ms 区间（需重新采集性能测试报告）

### 6.2 后续可选的进一步优化（如仍有性能压力）

1. **numBins 128→96**（减少 25% fillRect，频谱分辨率仍可观）
2. **histogram-based 降采样**：将 128 bins 在渲染前合并到 64 列宽柱——但在 isometric 视图中 bin 宽度已是独立的视觉维度，合并会显著改变观感
3. **增量渲染**：仅重绘最新的 1-2 层（depth=0,1），旧层从上一帧的 cached Image 继承——可降至 ~256 fillRect/帧，但实现复杂度较高

### 6.3 文档维护

本文档存放于 `I:/Y2KMeter/docs/Spectrogram3D_Performance_Optimization.md`，后续如迭代性能优化应同步更新。