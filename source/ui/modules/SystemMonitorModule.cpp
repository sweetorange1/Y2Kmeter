#include "source/ui/modules/SystemMonitorModule.h"

#include "source/i18n/Localisation.h"
#include "source/ui/PinkXPStyle.h"
#include "source/ui/HoverHelp.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

using y2k::sys::Metric;
using y2k::sys::Snapshot;

// ==========================================================
// SystemMonitorModule（电脑状态模块）
// ==========================================================

namespace
{
    struct ItemMeta
    {
        const char* id;          // 参数 id = 存档键
        const char* label;       // 模块内短标签（翻译键）
        const char* paramLabel;  // 控制面板里的名字（翻译键）
        const char* tip;         // 控制面板悬停提示（翻译键）
        bool        defaultOn;
        Metric      metric;
    };

    const ItemMeta kItems[] =
    {
        { "itemCpu",     "CPU",      "CPU usage",       "Total processor load, the same number Task Manager / Activity Monitor shows.", true,  Metric::cpu },
        { "itemCpuTemp", "CPU TEMP", "CPU temperature", "Processor temperature. On Windows it is read from HWiNFO or LibreHardwareMonitor when they are running.", true,  Metric::cpuTemp },
        { "itemGpu",     "GPU",      "GPU usage",       "Load of the busiest engine on the main graphics card.", true,  Metric::gpu },
        { "itemGpuTemp", "GPU TEMP", "GPU temperature", "Graphics card temperature (Windows driver data, NVIDIA NVML or a running monitoring tool).", true,  Metric::gpuTemp },
        { "itemRam",     "RAM",      "Memory",          "System memory in use.", true,  Metric::ram },
        { "itemVram",    "VRAM",     "Video memory",    "Graphics memory in use on the main graphics card.", true,  Metric::vram },
        { "itemDisk",    "DISK",     "Disk activity",   "How busy the disks are, plus read / write speed.", false, Metric::disk },
        { "itemNet",     "NET",      "Network",         "Download speed (big number) and upload speed, all network adapters combined.", true,  Metric::netDown },
        { "itemBattery", "BATTERY",  "Battery",         "Battery charge. Hidden automatically on computers without a battery.", true,  Metric::battery },
    };

    static_assert (sizeof (kItems) / sizeof (kItems[0]) == 9, "kItems must match SystemMonitorModule::Item");

    constexpr float kTempLo = 20.0f;
    constexpr float kTempHi = 100.0f;

    // 返回传给 getAxisFont 的字号（逐次缩小直到放得下，最多 5 轮）
    float fitHeightUncached (const juce::String& text, float maxW, float maxH, float minH, int flags)
    {
        float h = juce::jmax (minH, maxH);

        for (int pass = 0; pass < 5; ++pass)
        {
            const float w = juce::GlyphArrangement::getStringWidth (PinkXP::getAxisFont (h, flags), text);

            if (w <= maxW || h <= minH)
                return h;

            h = juce::jmax (minH, h * maxW / w * 0.97f);
        }

        return h;
    }

    // 自适应字号结果缓存（性能）：每段文字原来每次绘制都要按字号逐次塑形测宽
    //   （最多 5 轮）。标签 / 单位等文本与布局尺寸都很少变化，按
    //   （文本, 区域, 字号上下限, 样式, UI 密度, 字体代次）缓存字号。仅消息线程 /
    //   持消息锁的 GL 线程调用，天然串行。
    juce::Font fitFont (const juce::String& text, float maxW, float maxH, float minH, int flags)
    {
        static std::unordered_map<juce::String, float> cache;
        static juce::uint32 cacheGeneration = 0;

        const auto gen = PinkXP::getFontGeneration();
        if (gen != cacheGeneration || cache.size() > 512)
        {
            cache.clear();
            cacheGeneration = gen;
        }

        juce::String key;
        key.preallocateBytes ((size_t) text.getNumBytesAsUTF8() + 48);
        key << text << '\x1f' << maxW << '\x1f' << maxH << '\x1f' << minH << '\x1f' << flags
            << '\x1f' << PinkXP::uiScale();

        auto it = cache.find (key);
        if (it == cache.end())
            it = cache.emplace (std::move (key), fitHeightUncached (text, maxW, maxH, minH, flags)).first;

        return PinkXP::getAxisFont (it->second, flags);
    }

    void drawFitted (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> r,
                     juce::Justification j, float maxH, float minH, int flags)
    {
        if (r.isEmpty() || text.isEmpty())
            return;

        g.setFont (fitFont (text, (float) r.getWidth() - 2.0f, maxH, minH, flags));
        g.drawText (text, r, j, true);
    }

    // 分段环形表：270° 弧，从左下（-135°）顺时针到右下（+135°）
    void drawSegmentRing (juce::Graphics& g, juce::Point<float> c, float radius, float thickness,
                          float frac, juce::Colour colour, int segments)
    {
        const float start = -juce::MathConstants<float>::pi * 0.75f;
        const float sweep =  juce::MathConstants<float>::pi * 1.5f;
        const float seg   = sweep / (float) segments;
        const float gap   = juce::jmin (seg * 0.28f, 0.06f);

        const float filled = juce::jlimit (0.0f, 1.0f, frac) * (float) segments;
        const juce::PathStrokeType stroke (thickness, juce::PathStrokeType::mitered, juce::PathStrokeType::butt);

        // 性能：分段之间留有间隙、互不重叠，同色同 alpha 的段合并成一条 Path 一次描边
        //   （原来每段 1~2 次 strokePath，28 段 × 每张卡片，是本模块主要开销之一）。
        //   底色段一条；完全点亮的段一条；只有部分点亮的那一段单独描边。
        juce::Path track, lit, partial;
        float partialCover = 0.0f;

        for (int i = 0; i < segments; ++i)
        {
            const float a0 = start + seg * (float) i + gap * 0.5f;
            const float a1 = a0 + seg - gap;

            track.addCentredArc (c.x, c.y, radius, radius, 0.0f, a0, a1, true);

            const float cover = juce::jlimit (0.0f, 1.0f, filled - (float) i);

            if (frac >= 0.0f && cover > 0.0f)
            {
                if (cover >= 1.0f)
                {
                    lit.addCentredArc (c.x, c.y, radius, radius, 0.0f, a0, a1, true);
                }
                else
                {
                    partial.addCentredArc (c.x, c.y, radius, radius, 0.0f, a0, a1, true);
                    partialCover = cover;
                }
            }
        }

        g.setColour (PinkXP::ink.withAlpha (0.10f));
        g.strokePath (track, stroke);

        if (! lit.isEmpty())
        {
            g.setColour (colour);   // 0.35 + 0.65 × 1
            g.strokePath (lit, stroke);
        }

        if (! partial.isEmpty())
        {
            g.setColour (colour.withMultipliedAlpha (0.35f + 0.65f * partialCover));
            g.strokePath (partial, stroke);
        }
    }

    // LED 分段横条：colourAt(pos) 给出每一格的颜色（pos = 该格在条上的位置 0..1）
    template <typename ColourFn>
    void drawSegmentBar (juce::Graphics& g, juce::Rectangle<int> r, float frac, ColourFn colourAt)
    {
        if (r.getWidth() < 4 || r.getHeight() < 2)
            return;

        const int blockW = juce::jlimit (3, 6, r.getHeight() / 2 + 1);
        const int gap    = 1;
        const int n      = juce::jmax (1, (r.getWidth() + gap) / (blockW + gap));
        const float filled = juce::jlimit (0.0f, 1.0f, frac) * (float) n;

        int x = r.getX();
        for (int i = 0; i < n; ++i)
        {
            const juce::Rectangle<int> b (x, r.getY(), blockW, r.getHeight());
            const float cover = juce::jlimit (0.0f, 1.0f, filled - (float) i);

            g.setColour (PinkXP::ink.withAlpha (0.09f));
            g.fillRect (b);

            if (frac >= 0.0f && cover > 0.0f)
            {
                g.setColour (colourAt (((float) i + 0.5f) / (float) n).withMultipliedAlpha (0.3f + 0.7f * cover));
                g.fillRect (b);
            }

            x += blockW + gap;
        }
    }

    float tempToFrac (float c) noexcept
    {
        return juce::jlimit (0.0f, 1.0f, (c - kTempLo) / (kTempHi - kTempLo));
    }
}

// ----------------------------------------------------------
// 构造 / 析构
// ----------------------------------------------------------
SystemMonitorModule::SystemMonitorModule()
    : ModulePanel (ModuleType::systemMonitor)
{
    setMinSize (96, 64);
    setDefaultSize (384, 240);
    setTitleText ("System Monitor");
    setControlPanelEnabled (true);

    for (int i = 0; i < numItems; ++i)
    {
        enabled[(size_t) i]    = kItems[i].defaultOn;
        shownFrac[(size_t) i]  = 0.0f;
        targetFrac[(size_t) i] = -1.0f;
    }

    // 20 Hz：检查新样本 + 数值缓动；订阅本身等到真正显示在屏幕上才发生
    //   （添加菜单的悬停预览会临时构造本模块，不应因此启动采集线程）
    startTimerHz (20);

    // 最高 20Hz 自绘：常被摆在每帧刷新的模块上方，挂位图缓存避免被"陪着"
    //   每帧整块重画（环形表描边 + 自适应字号文本，mac 上曾占 GL 线程约 15%）
    y2k::ui::setGlSafeImageCache (*this, true);
}

SystemMonitorModule::~SystemMonitorModule()
{
    stopTimer();

    if (subscribed)
        sampler->unsubscribe (this);
}

void SystemMonitorModule::updateSubscription()
{
    const bool want = isShowing();

    if (want && ! subscribed)
    {
        sampler->subscribe (this, intervalSec);
        subscribed = true;
    }
    else if (! want && subscribed)
    {
        sampler->unsubscribe (this);
        subscribed = false;
    }
}

bool SystemMonitorModule::itemShown (Item i) const noexcept
{
    if (! enabled[(size_t) i])
        return false;

    // 电池：采到数据且确认没有电池时自动隐藏
    if (i == itemBattery && snap.serial > 0 && ! snap.hasBattery)
        return false;

    return true;
}

int SystemMonitorModule::historySamples() const
{
    const double interval = juce::jmax (0.5, sampler->getIntervalSec());
    return juce::jlimit (2, y2k::sys::kHistoryLen, (int) std::lround (historySec / interval));
}

// ----------------------------------------------------------
// 定时器：取新样本 + 缓动
// ----------------------------------------------------------
void SystemMonitorModule::timerCallback()
{
    updateSubscription();

    bool dirty = false;

    if (subscribed)
    {
        const auto serial = sampler->getSerial();

        if (serial != lastSerial)
        {
            lastSerial = serial;
            snap = sampler->getLatest();
            dirty = true;

            // 网络曲线量程：最近历史峰值 × 1.15，上涨立即跟随、回落缓慢
            if (enabled[itemNet])
            {
                const int n = historySamples();
                sampler->copyHistory (Metric::netDown, histA, n);
                sampler->copyHistory (Metric::netUp,   histB, n);

                float peak = 0.0f;
                for (float v : histA) if (std::isfinite (v)) peak = juce::jmax (peak, v);
                for (float v : histB) if (std::isfinite (v)) peak = juce::jmax (peak, v);

                const float target = juce::jmax (64.0f * 1024.0f, peak * 1.15f);
                netScale = target > netScale ? target : netScale + (target - netScale) * 0.15f;
            }

            for (const auto& v : buildViews())
                targetFrac[(size_t) v.item] = v.frac;
        }
    }

    for (int i = 0; i < numItems; ++i)
    {
        const float target = targetFrac[(size_t) i];
        float& shown = shownFrac[(size_t) i];

        if (target < 0.0f)
        {
            if (shown != 0.0f) { shown = 0.0f; dirty = true; }
            continue;
        }

        const float diff = target - shown;
        if (std::abs (diff) > 0.0015f)
        {
            shown += diff * 0.35f;
            dirty = true;
        }
        else if (diff != 0.0f)
        {
            shown = target;
            dirty = true;
        }
    }

    if (dirty && isVisuallyActiveInWorkspace())
        repaintFrameContent();
}

// ----------------------------------------------------------
// 读数 → 视图
// ----------------------------------------------------------
juce::String SystemMonitorModule::formatTemp (float c) const
{
    if (! Snapshot::hasTemp (c))
        return "N/A";

    if (fahrenheit)
        return juce::String (juce::roundToInt (c * 9.0f / 5.0f + 32.0f)) + juce::String::fromUTF8 ("\xc2\xb0" "F");

    return juce::String (juce::roundToInt (c)) + juce::String::fromUTF8 ("\xc2\xb0" "C");
}

juce::Colour SystemMonitorModule::levelColour (Item item, float frac, float raw) const
{
    const auto accent = PinkXP::pink500;

    if (! colourByLoad)
        return accent;

    switch (item)
    {
        case itemCpuTemp:
        case itemGpuTemp:
            if (! std::isfinite (raw)) return accent;
            return raw >= 85.0f ? PinkXP::danger : raw >= 70.0f ? PinkXP::warn : accent;

        case itemBattery:
            if (snap.charging) return PinkXP::ok;
            return raw <= 15.0f ? PinkXP::danger : raw <= 30.0f ? PinkXP::warn : accent;

        case itemNet:
            return accent;

        default:
            return frac >= 0.85f ? PinkXP::danger : frac >= 0.60f ? PinkXP::warn : accent;
    }
}

std::vector<SystemMonitorModule::View> SystemMonitorModule::buildViews() const
{
    std::vector<View> out;
    out.reserve (numItems);

    const auto& s = snap;

    auto pctText = [] (float p) { return juce::String (juce::roundToInt (p)) + "%"; };

    for (int i = 0; i < numItems; ++i)
    {
        const auto item = (Item) i;
        if (! itemShown (item))
            continue;

        View v;
        v.item   = item;
        v.label  = kItems[i].label;
        v.metric = kItems[i].metric;
        v.value  = "--";

        float raw = std::numeric_limits<float>::quiet_NaN();

        switch (item)
        {
            case itemCpu:
                if (Snapshot::isValid (s.cpuLoad))
                {
                    raw = s.cpuLoad;
                    v.value = pctText (raw);
                    v.frac  = raw / 100.0f;
                    v.available = true;
                }
                if (s.cpuMHz > 0.0f)
                    v.sub = juce::String (s.cpuMHz / 1000.0f, 2) + " GHz";
                else if (s.numCores > 0)
                    v.sub = y2k::trf ("{0} threads", s.numCores);
                break;

            case itemCpuTemp:
            case itemGpuTemp:
            {
                const bool cpu = item == itemCpuTemp;
                const float c  = cpu ? s.cpuTempC : s.gpuTempC;
                const auto& src = cpu ? s.cpuTempSource : s.gpuTempSource;

                if (Snapshot::hasTemp (c))
                {
                    raw = c;
                    v.value = formatTemp (c);
                    v.frac  = tempToFrac (c);
                    v.available = true;
                    v.sub = y2k::trf ("via {0}", src);
                }
                else
                {
                    v.value = s.serial > 0 ? "N/A" : "--";
                    if (s.serial > 0)
                        v.sub = y2k::tr ("no sensor");
                }
                break;
            }

            case itemGpu:
                if (Snapshot::isValid (s.gpuLoad))
                {
                    raw = s.gpuLoad;
                    v.value = pctText (raw);
                    v.frac  = raw / 100.0f;
                    v.available = true;
                }
                v.sub = shortHardwareName (s.gpuName);
                break;

            case itemRam:
            {
                const float p = s.metricValue (Metric::ram);
                if (std::isfinite (p))
                {
                    raw = p;
                    v.value = pctText (p);
                    v.frac  = p / 100.0f;
                    v.available = true;
                    v.sub = y2k::sys::formatBytesShort (s.ramUsed) + " / " + y2k::sys::formatBytesShort (s.ramTotal);
                }
                break;
            }

            case itemVram:
            {
                const float p = s.metricValue (Metric::vram);
                if (std::isfinite (p))
                {
                    raw = p;
                    v.value = pctText (p);
                    v.frac  = p / 100.0f;
                    v.available = true;
                    v.sub = y2k::sys::formatBytesShort (s.vramUsed) + " / " + y2k::sys::formatBytesShort (s.vramTotal);
                }
                else if (Snapshot::isValid (s.vramUsed))
                {
                    // 统一内存（Apple Silicon）：只有占用量，没有独立总量
                    v.value = y2k::sys::formatBytesShort (s.vramUsed);
                    v.available = true;
                    v.sub = y2k::tr ("shared memory");
                }
                else if (s.serial > 0)
                {
                    v.value = "N/A";
                }
                break;
            }

            case itemDisk:
                if (Snapshot::isValid (s.diskActive))
                {
                    raw = s.diskActive;
                    v.value = pctText (raw);
                    v.frac  = raw / 100.0f;
                    v.available = true;
                }
                if (Snapshot::isValid (s.diskReadBps))
                    v.sub = "R " + y2k::sys::formatBytesShort (s.diskReadBps)
                          + "  W " + y2k::sys::formatBytesShort (s.diskWriteBps);
                break;

            case itemNet:
                v.dualSeries = true;
                if (Snapshot::isValid (s.netDownBps))
                {
                    raw = (float) s.netDownBps;
                    v.value = y2k::sys::formatBytesShort (s.netDownBps) + "/s";
                    v.frac  = juce::jlimit (0.0f, 1.0f, raw / juce::jmax (1.0f, netScale));
                    v.available = true;
                    v.sub = y2k::trf ("UP {0}/s", y2k::sys::formatBytesShort (s.netUpBps));
                }
                break;

            case itemBattery:
                if (s.hasBattery && Snapshot::isValid (s.batteryPct))
                {
                    raw = s.batteryPct;
                    v.value = pctText (raw);
                    v.frac  = raw / 100.0f;
                    v.available = true;
                    v.sub = y2k::tr (s.charging ? "Charging" : (s.onAcPower ? "Plugged in" : "On battery"));
                }
                break;

            case numItems:
                break;
        }

        v.colour = levelColour (item, v.frac, raw);
        out.push_back (v);
    }

    return out;
}

float SystemMonitorModule::historyLo (const View& v) const
{
    return v.metric == Metric::cpuTemp || v.metric == Metric::gpuTemp ? kTempLo : 0.0f;
}

float SystemMonitorModule::historyHi (const View& v) const
{
    if (v.metric == Metric::cpuTemp || v.metric == Metric::gpuTemp) return kTempHi;
    if (v.metric == Metric::netDown || v.metric == Metric::netUp)   return juce::jmax (1.0f, netScale);
    return 100.0f;
}

juce::String SystemMonitorModule::shortHardwareName (const juce::String& name)
{
    auto s = name;

    for (auto* junk : { "(R)", "(TM)", "(tm)", "(r)", "Processor", "CPU", "Graphics Adapter" })
        s = s.replace (junk, "");

    if (s.contains ("@"))
        s = s.upToFirstOccurrenceOf ("@", false, false);

    s = s.replace ("NVIDIA ", "").replace ("GeForce ", "").replace ("AMD Radeon", "Radeon")
         .replace ("-Core", "-core").replace ("  ", " ").trim();

    // "AMD Ryzen 7 5800X 8-core" → 去掉 "N-core" 尾巴
    const auto lastWord = s.fromLastOccurrenceOf (" ", false, false);
    if (lastWord.endsWithIgnoreCase ("-core"))
        s = s.upToLastOccurrenceOf (" ", false, false).trim();

    return s;
}

// ----------------------------------------------------------
// 网格切分：在给定区域里放 count 块，选"每块最大且接近 4:3"的列数
// ----------------------------------------------------------
std::vector<juce::Rectangle<int>> SystemMonitorModule::gridTiles (juce::Rectangle<int> area, int count, int gap)
{
    std::vector<juce::Rectangle<int>> out;
    if (count <= 0 || area.isEmpty())
        return out;

    int bestCols = 1;
    float bestScore = -1.0f;

    for (int cols = 1; cols <= count; ++cols)
    {
        const int rows = (count + cols - 1) / cols;
        const float w = ((float) area.getWidth()  - (float) (gap * (cols - 1))) / (float) cols;
        const float h = ((float) area.getHeight() - (float) (gap * (rows - 1))) / (float) rows;
        if (w < 8.0f || h < 8.0f)
            continue;

        const float score = juce::jmin (w, h * 1.35f) - (float) (rows * cols - count) * 2.0f;
        if (score > bestScore)
        {
            bestScore = score;
            bestCols = cols;
        }
    }

    const int rows = (count + bestCols - 1) / bestCols;
    const int rowH = (area.getHeight() - gap * (rows - 1)) / rows;

    int index = 0;
    for (int r = 0; r < rows; ++r)
    {
        const int inRow = juce::jmin (bestCols, count - index);
        const int y = area.getY() + r * (rowH + gap);
        const int colW = (area.getWidth() - gap * (inRow - 1)) / inRow;

        for (int c = 0; c < inRow; ++c)
        {
            const int x = area.getX() + c * (colW + gap);
            const int w = (c == inRow - 1) ? area.getRight() - x : colW;
            const int h = (r == rows - 1) ? area.getBottom() - y : rowH;
            out.emplace_back (x, y, w, h);
            ++index;
        }
    }

    return out;
}

// ----------------------------------------------------------
// 绘制
// ----------------------------------------------------------
void SystemMonitorModule::paintContent (juce::Graphics& g, juce::Rectangle<int> contentBounds)
{
    g.setColour (PinkXP::btnFace);
    g.fillRect (contentBounds);

    auto area = contentBounds.reduced (4);
    if (area.getWidth() < 24 || area.getHeight() < 16)
        return;

    PinkXP::drawSunken (g, area, PinkXP::content);
    area = area.reduced (4);

    hitTiles.clear();

    if (infoBar && style != Style::compact && area.getHeight() >= 96)
    {
        const int barH = juce::jlimit (10, 14, area.getHeight() / 14);
        paintInfoBar (g, area.removeFromTop (barH));
        area.removeFromTop (3);
    }

    const auto views = buildViews();
    if (views.empty())
    {
        g.setColour (PinkXP::ink.withAlpha (0.45f));
        drawFitted (g, y2k::tr ("Nothing selected - open the gear menu"), area,
                    juce::Justification::centred, 11.0f, 7.0f, juce::Font::plain);
        return;
    }

    switch (style)
    {
        case Style::cards:   paintCards   (g, area, views); break;
        case Style::bars:    paintBars    (g, area, views); break;
        case Style::graphs:  paintGraphs  (g, area, views); break;
        case Style::compact: paintCompact (g, area, views); break;
    }
}

void SystemMonitorModule::paintInfoBar (juce::Graphics& g, juce::Rectangle<int> area)
{
    juce::StringArray parts;

    const auto cpu = shortHardwareName (snap.cpuName);
    const auto gpu = shortHardwareName (snap.gpuName);

    if (cpu.isNotEmpty()) parts.add (cpu);
    if (gpu.isNotEmpty()) parts.add (gpu);
    if (Snapshot::isValid (snap.uptimeSec))
        parts.add (y2k::trf ("Uptime {0}", y2k::sys::formatUptime (snap.uptimeSec)));

    if (parts.isEmpty())
        parts.add (y2k::tr ("Collecting system info..."));

    g.setColour (PinkXP::ink.withAlpha (0.55f));
    drawFitted (g, parts.joinIntoString ("  |  "), area, juce::Justification::centredLeft,
                (float) area.getHeight() - 1.0f, 6.5f, juce::Font::plain);

    g.setColour (PinkXP::ink.withAlpha (0.10f));
    g.fillRect (area.getX(), area.getBottom() + 1, area.getWidth(), 1);
}

void SystemMonitorModule::paintCoreStrip (juce::Graphics& g, juce::Rectangle<int> area, juce::Colour colour)
{
    const int n = juce::jlimit (0, y2k::sys::kMaxCores, snap.numCores);
    if (n <= 0 || area.getWidth() < n || area.getHeight() < 4)
        return;

    const int gap = (area.getWidth() >= n * 4) ? 1 : 0;
    const float w = ((float) area.getWidth() - (float) (gap * (n - 1))) / (float) n;

    for (int i = 0; i < n; ++i)
    {
        const float x = (float) area.getX() + (float) i * (w + (float) gap);
        const juce::Rectangle<float> col (x, (float) area.getY(), juce::jmax (1.0f, w), (float) area.getHeight());

        g.setColour (PinkXP::ink.withAlpha (0.08f));
        g.fillRect (col);

        const float f = juce::jlimit (0.0f, 1.0f, snap.coreLoad[(size_t) i] / 100.0f);
        if (f > 0.0f)
        {
            const float lvl = colourByLoad ? f : 0.0f;
            const auto c = lvl >= 0.85f ? PinkXP::danger : lvl >= 0.60f ? PinkXP::warn : colour;
            g.setColour (c.withAlpha (0.85f));
            g.fillRect (col.withTop (col.getBottom() - juce::jmax (1.0f, col.getHeight() * f)));
        }
    }
}

void SystemMonitorModule::paintHistory (juce::Graphics& g, juce::Rectangle<int> area, const View& v, bool withGrid)
{
    if (area.getWidth() < 8 || area.getHeight() < 6)
        return;

    const auto r = area.toFloat();

    if (withGrid)
    {
        g.setColour (PinkXP::ink.withAlpha (0.07f));
        for (int k = 1; k < 4; ++k)
            g.fillRect (r.getX(), r.getY() + r.getHeight() * (float) k / 4.0f, r.getWidth(), 1.0f);
    }

    const int n = historySamples();
    const float lo = historyLo (v), hi = historyHi (v);

    auto drawSeries = [&] (Metric m, std::vector<float>& buf, juce::Colour colour, bool fill, float thickness)
    {
        sampler->copyHistory (m, buf, n);
        if (buf.size() < 2)
            return;

        const float step = r.getWidth() / (float) juce::jmax (1, n - 1);
        const float x0   = r.getRight() - step * (float) (buf.size() - 1);

        auto yOf = [&] (float val)
        {
            const float t = juce::jlimit (0.0f, 1.0f, (val - lo) / juce::jmax (1.0e-6f, hi - lo));
            return r.getBottom() - t * (r.getHeight() - 1.0f);
        };

        juce::Path line, area2;
        bool open = false;
        float segStartX = 0.0f, lastX = 0.0f;

        auto closeFill = [&]
        {
            if (! open || ! fill) return;
            area2.lineTo (lastX, r.getBottom());
            area2.lineTo (segStartX, r.getBottom());
            area2.closeSubPath();
        };

        for (size_t i = 0; i < buf.size(); ++i)
        {
            const float x = juce::jmax (r.getX(), x0 + step * (float) i);
            const float val = buf[i];

            if (! std::isfinite (val))
            {
                closeFill();
                open = false;
                continue;
            }

            const float y = yOf (val);

            if (! open)
            {
                line.startNewSubPath (x, y);
                if (fill) { area2.startNewSubPath (x, r.getBottom()); area2.lineTo (x, y); }
                segStartX = x;
                open = true;
            }
            else
            {
                line.lineTo (x, y);
                if (fill) area2.lineTo (x, y);
            }
            lastX = x;
        }
        closeFill();

        if (fill)
        {
            g.setColour (colour.withAlpha (0.18f));
            g.fillPath (area2);
        }

        g.setColour (colour);
        g.strokePath (line, juce::PathStrokeType (thickness));
    };

    drawSeries (v.metric, histA, v.colour, true, 1.5f);

    if (v.dualSeries)
        drawSeries (Metric::netUp, histB, PinkXP::accent2.withAlpha (0.9f), false, 1.2f);
}

// ---------------- Cards ----------------
void SystemMonitorModule::paintCards (juce::Graphics& g, juce::Rectangle<int> area, const std::vector<View>& views)
{
    const auto tiles = gridTiles (area, (int) views.size(), 4);

    for (size_t i = 0; i < tiles.size() && i < views.size(); ++i)
    {
        const auto& v = views[i];
        const auto tile = tiles[i];
        hitTiles.emplace_back (v.item, tile);

        g.setColour (PinkXP::btnFace.withAlpha (0.30f));
        g.fillRect (tile);
        g.setColour (PinkXP::shdw.withAlpha (0.30f));
        g.drawRect (tile, 1);

        auto inner = tile.reduced (5, 4);
        if (inner.getWidth() < 16 || inner.getHeight() < 16)
            continue;

        const int labelH = juce::jlimit (8, 13, inner.getHeight() / 6);
        auto labelRow = inner.removeFromTop (labelH);
        g.setColour (PinkXP::ink.withAlpha (0.65f));
        drawFitted (g, y2k::tr (v.label), labelRow, juce::Justification::centredLeft,
                    (float) labelH, 6.0f, juce::Font::bold);
        inner.removeFromTop (2);

        const bool wide = inner.getWidth() > (int) ((float) inner.getHeight() * 1.7f);
        const bool showCores = v.item == itemCpu && perCore && snap.numCores > 0;

        juce::Rectangle<int> ringArea, side;

        if (wide)
        {
            ringArea = inner.removeFromLeft (inner.getHeight());
            inner.removeFromLeft (6);
            side = inner;
        }
        else
        {
            juce::Rectangle<int> bottom;
            if (inner.getHeight() >= 100)
                bottom = inner.removeFromBottom (juce::jlimit (12, 22, inner.getHeight() / 6));

            const int subH = (v.sub.isNotEmpty() && inner.getHeight() >= 46) ? juce::jlimit (8, 11, inner.getHeight() / 7) : 0;
            auto subRow = inner.removeFromBottom (subH);
            ringArea = inner;

            if (! subRow.isEmpty())
            {
                g.setColour (PinkXP::ink.withAlpha (0.5f));
                drawFitted (g, v.sub, subRow, juce::Justification::centred, (float) subH, 6.0f, juce::Font::plain);
            }

            if (! bottom.isEmpty())
            {
                bottom.removeFromTop (2);
                if (showCores) paintCoreStrip (g, bottom, v.colour);
                else           paintHistory (g, bottom, v, false);
            }
        }

        // ---- 环形表 + 中心读数 ----
        const float d = (float) juce::jmin (ringArea.getWidth(), ringArea.getHeight());
        if (d >= 14.0f)
        {
            const auto c = ringArea.toFloat().getCentre().translated (0.0f, d * 0.04f);
            const float thickness = juce::jmax (2.5f, d * 0.10f);
            const float radius = d * 0.5f - thickness * 0.5f - 1.0f;
            const int segments = d >= 70.0f ? 28 : (d >= 40.0f ? 20 : 14);

            drawSegmentRing (g, c, radius, thickness, v.available ? shownFrac[(size_t) v.item] : -1.0f,
                             v.colour, segments);

            const float inner2 = radius - thickness * 0.5f;
            const juce::Rectangle<int> valueRect ((int) (c.x - inner2 * 0.82f), (int) (c.y - inner2 * 0.42f),
                                                  (int) (inner2 * 1.64f), (int) (inner2 * 0.84f));

            g.setColour (v.available ? PinkXP::ink : PinkXP::ink.withAlpha (0.35f));
            drawFitted (g, v.value, valueRect, juce::Justification::centred,
                        inner2 * 0.62f, 6.0f, juce::Font::bold);
        }

        // ---- 宽卡片右侧：副读数 + 曲线 / 每核 ----
        if (wide && side.getWidth() > 20)
        {
            const int subH = v.sub.isNotEmpty() ? juce::jlimit (8, 12, side.getHeight() / 4) : 0;
            if (subH > 0)
            {
                g.setColour (PinkXP::ink.withAlpha (0.55f));
                drawFitted (g, v.sub, side.removeFromTop (subH), juce::Justification::centredLeft,
                            (float) subH, 6.0f, juce::Font::plain);
                side.removeFromTop (3);
            }

            if (showCores && side.getHeight() >= 24)
            {
                auto cores = side.removeFromBottom (side.getHeight() * 2 / 5);
                side.removeFromBottom (3);
                paintCoreStrip (g, cores, v.colour);
            }
            else if (showCores)
            {
                paintCoreStrip (g, side, v.colour);
                continue;
            }

            paintHistory (g, side, v, false);
        }
    }
}

// ---------------- Bars ----------------
void SystemMonitorModule::paintBars (juce::Graphics& g, juce::Rectangle<int> area, const std::vector<View>& views)
{
    const bool cores = perCore && snap.numCores > 0
                    && std::any_of (views.begin(), views.end(), [] (const View& v) { return v.item == itemCpu; });

    const int rows = (int) views.size() + (cores ? 1 : 0);
    const int gap  = 3;
    const int rowH = juce::jlimit (11, 34, (area.getHeight() - gap * (rows - 1)) / juce::jmax (1, rows));

    const int labelW = juce::jlimit (30, 84, area.getWidth() / 5);
    const int valueW = juce::jlimit (36, 96, area.getWidth() / 5);

    for (const auto& v : views)
    {
        if (area.getHeight() < rowH)
            break;

        auto row = area.removeFromTop (rowH);
        area.removeFromTop (gap);
        hitTiles.emplace_back (v.item, row);

        g.setColour (PinkXP::ink.withAlpha (0.7f));
        drawFitted (g, y2k::tr (v.label), row.removeFromLeft (labelW), juce::Justification::centredLeft,
                    juce::jmin (12.0f, (float) rowH * 0.7f), 6.0f, juce::Font::bold);

        g.setColour (v.available ? PinkXP::ink : PinkXP::ink.withAlpha (0.35f));
        drawFitted (g, v.value, row.removeFromRight (valueW), juce::Justification::centredRight,
                    juce::jmin (14.0f, (float) rowH * 0.8f), 6.0f, juce::Font::bold);
        row.removeFromRight (6);

        const bool withSub = v.sub.isNotEmpty() && rowH >= 24;
        auto barArea = row;
        juce::Rectangle<int> subArea;

        if (withSub)
        {
            subArea = barArea.removeFromBottom (juce::jlimit (8, 11, rowH / 3));
            barArea.removeFromBottom (1);
        }

        const int barH = juce::jlimit (4, 14, (int) ((float) barArea.getHeight() * 0.6f));
        auto bar = barArea.withSizeKeepingCentre (barArea.getWidth(), barH);

        const bool loadLike = v.item != itemNet && v.item != itemBattery
                           && v.item != itemCpuTemp && v.item != itemGpuTemp;

        drawSegmentBar (g, bar, v.available ? shownFrac[(size_t) v.item] : -1.0f,
                        [&] (float pos)
                        {
                            if (! colourByLoad || ! loadLike) return v.colour;
                            return pos >= 0.85f ? PinkXP::danger : pos >= 0.60f ? PinkXP::warn : PinkXP::pink500;
                        });

        if (! v.available)
        {
            g.setColour (PinkXP::ink.withAlpha (0.4f));
            drawFitted (g, y2k::tr ("not available"), bar, juce::Justification::centred,
                        (float) barH, 6.0f, juce::Font::plain);
        }

        if (withSub)
        {
            g.setColour (PinkXP::ink.withAlpha (0.5f));
            drawFitted (g, v.sub, subArea, juce::Justification::centredLeft,
                        (float) subArea.getHeight(), 6.0f, juce::Font::plain);
        }

        if (v.item == itemCpu && cores && area.getHeight() >= 10)
        {
            const int h = juce::jlimit (10, 40, (int) ((float) rowH * 1.2f));
            auto coreRow = area.removeFromTop (juce::jmin (h, area.getHeight()));
            area.removeFromTop (gap);
            coreRow.removeFromLeft (labelW);
            coreRow.removeFromRight (valueW + 6);
            paintCoreStrip (g, coreRow, v.colour);
        }
    }
}

// ---------------- Graphs ----------------
void SystemMonitorModule::paintGraphs (juce::Graphics& g, juce::Rectangle<int> area, const std::vector<View>& views)
{
    const auto tiles = gridTiles (area, (int) views.size(), 4);

    for (size_t i = 0; i < tiles.size() && i < views.size(); ++i)
    {
        const auto& v = views[i];
        const auto tile = tiles[i];
        hitTiles.emplace_back (v.item, tile);

        g.setColour (PinkXP::shdw.withAlpha (0.35f));
        g.drawRect (tile, 1);

        auto inner = tile.reduced (4, 3);
        if (inner.getHeight() < 14)
            continue;

        const int headH = juce::jlimit (8, 14, inner.getHeight() / 5);
        auto head = inner.removeFromTop (headH);
        inner.removeFromTop (2);

        const int valueW = juce::jmax (head.getWidth() / 2, 24);
        g.setColour (v.available ? PinkXP::ink : PinkXP::ink.withAlpha (0.35f));
        drawFitted (g, v.value, head.removeFromRight (valueW), juce::Justification::centredRight,
                    (float) headH, 6.0f, juce::Font::bold);

        g.setColour (PinkXP::ink.withAlpha (0.65f));
        drawFitted (g, y2k::tr (v.label), head, juce::Justification::centredLeft,
                    (float) headH, 6.0f, juce::Font::bold);

        paintHistory (g, inner, v, true);

        if (v.sub.isNotEmpty() && inner.getHeight() >= 40)
        {
            const int subH = juce::jlimit (7, 10, inner.getHeight() / 6);
            auto subArea = inner.removeFromBottom (subH + 2).reduced (2, 1);
            g.setColour (PinkXP::ink.withAlpha (0.5f));
            drawFitted (g, v.sub, subArea, juce::Justification::centredLeft, (float) subH, 6.0f, juce::Font::plain);
        }
    }
}

// ---------------- Compact ----------------
void SystemMonitorModule::paintCompact (juce::Graphics& g, juce::Rectangle<int> area, const std::vector<View>& views)
{
    struct Token { const View* v; juce::String label; int w; };

    const int gapX = 10, gapY = 4, barH = 2;

    // 从大到小试字号，直到所有读数能排进区域
    float fh = juce::jlimit (8.0f, 18.0f, (float) area.getHeight() * 0.5f);
    std::vector<Token> tokens;
    std::vector<int> lineOf;
    int lines = 1;

    for (int attempt = 0; attempt < 12; ++attempt)
    {
        const auto fLabel = PinkXP::getAxisFont (fh * 0.8f, juce::Font::plain);
        const auto fValue = PinkXP::getAxisFont (fh, juce::Font::bold);

        tokens.clear();
        lineOf.clear();
        lines = 1;
        int x = 0;

        for (const auto& v : views)
        {
            const auto label = y2k::tr (v.label);
            const int w = (int) std::ceil (juce::GlyphArrangement::getStringWidth (fLabel, label + " ")
                                         + juce::GlyphArrangement::getStringWidth (fValue, v.value));
            if (x > 0 && x + w > area.getWidth())
            {
                ++lines;
                x = 0;
            }
            tokens.push_back ({ &v, label, w });
            lineOf.push_back (lines - 1);
            x += w + gapX;
        }

        const int needH = lines * ((int) fh + barH + 2) + (lines - 1) * gapY;
        if (needH <= area.getHeight() || fh <= 7.0f)
            break;

        fh = juce::jmax (7.0f, fh * 0.88f);
    }

    const auto fLabel = PinkXP::getAxisFont (fh * 0.8f, juce::Font::plain);
    const auto fValue = PinkXP::getAxisFont (fh, juce::Font::bold);
    const int lineH = (int) fh + barH + 2;
    const int totalH = lines * lineH + (lines - 1) * gapY;
    int y = area.getY() + juce::jmax (0, (area.getHeight() - totalH) / 2);

    for (int ln = 0; ln < lines; ++ln)
    {
        int x = area.getX();

        for (size_t i = 0; i < tokens.size(); ++i)
        {
            if (lineOf[i] != ln)
                continue;

            const auto& t = tokens[i];
            const auto& v = *t.v;
            const juce::Rectangle<int> cell (x, y, juce::jmin (t.w, area.getRight() - x), lineH);
            hitTiles.emplace_back (v.item, cell);

            const int lw = (int) std::ceil (juce::GlyphArrangement::getStringWidth (fLabel, t.label + " "));

            g.setFont (fLabel);
            g.setColour (PinkXP::ink.withAlpha (0.55f));
            g.drawText (t.label, cell.withWidth (lw).withHeight ((int) fh), juce::Justification::centredLeft, false);

            g.setFont (fValue);
            g.setColour (v.available ? PinkXP::ink : PinkXP::ink.withAlpha (0.35f));
            g.drawText (v.value, cell.withTrimmedLeft (lw).withHeight ((int) fh), juce::Justification::centredLeft, false);

            const auto bar = juce::Rectangle<int> (cell.getX(), cell.getY() + (int) fh + 1, cell.getWidth(), barH);
            g.setColour (PinkXP::ink.withAlpha (0.08f));
            g.fillRect (bar);
            if (v.available)
            {
                g.setColour (v.colour);
                g.fillRect (bar.withWidth ((int) std::lround ((float) bar.getWidth() * shownFrac[(size_t) v.item])));
            }

            x += t.w + gapX;
        }

        y += lineH + gapY;
    }
}

// ----------------------------------------------------------
// 悬停说明：温度格显示数据来源 / 读不到时的解决办法；CPU / GPU 格显示完整型号
// ----------------------------------------------------------
ModulePanel::HoverHelp SystemMonitorModule::getHoverHelp (juce::Point<int> localPos) const
{
    for (const auto& [item, rect] : hitTiles)
    {
        if (! rect.contains (localPos))
            continue;

        if (item == itemCpuTemp || item == itemGpuTemp)
        {
            const bool cpu = item == itemCpuTemp;
            const float c  = cpu ? snap.cpuTempC : snap.gpuTempC;
            const auto& src = cpu ? snap.cpuTempSource : snap.gpuTempSource;
            const auto title = y2k::tr (cpu ? "CPU temperature" : "GPU temperature");

            juce::String body;

            if (Snapshot::hasTemp (c))
            {
                body = y2k::trf ("Source: {0}", src);
                if (src == "ACPI")
                    body << "\n" << y2k::tr ("ACPI thermal zone: a motherboard sensor, it can differ from the real CPU temperature.");
                else if (src == "Windows")
                    body << "\n" << y2k::tr ("Reported by the graphics driver (same data as Task Manager).");
            }
            else
            {
               #if JUCE_WINDOWS
                body = y2k::tr ("No temperature sensor could be read.") + "\n"
                     + y2k::tr ("Windows has no driver-free way to read CPU temperature.") + "\n"
                     + y2k::tr ("Run HWiNFO (enable Settings > Shared Memory Support) or LibreHardwareMonitor in the background - Y2Kmeter picks it up automatically.");
               #else
                body = y2k::tr ("No temperature sensor could be read.") + "\n"
                     + y2k::tr ("macOS does not offer a public API for temperature sensors.");
               #endif
            }

            return { y2k::help::make (title, body), true };
        }

        if (item == itemCpu && snap.cpuName.isNotEmpty())
        {
            juce::String body = snap.cpuName;
            if (snap.numCores > 0)
                body << "\n" << y2k::trf ("{0} logical processors", snap.numCores);
            if (snap.cpuMHz > 0.0f)
                body << "\n" << y2k::trf ("Clock: {0} GHz", juce::String (snap.cpuMHz / 1000.0f, 2));
            return { y2k::help::make (y2k::tr ("CPU usage"), body), true };
        }

        if (item == itemGpu && snap.gpuName.isNotEmpty())
            return { y2k::help::make (y2k::tr ("GPU usage"), snap.gpuName), true };

        if (item == itemDisk && Snapshot::isValid (snap.sysDriveTotal))
        {
            const auto body = y2k::trf ("System drive: {0} free of {1}",
                                        y2k::sys::formatBytes (snap.sysDriveFree),
                                        y2k::sys::formatBytes (snap.sysDriveTotal));
            return { y2k::help::make (y2k::tr ("Disk activity"), body), true };
        }

        break;
    }

    return ModulePanel::getHoverHelp (localPos);
}

// ----------------------------------------------------------
// 控制面板 / 存档
// ----------------------------------------------------------
void SystemMonitorModule::buildParamDefs (std::vector<ParamDef>& out) const
{
    out.push_back (ParamDef::choice ("style", "Style",
        { "Cards", "Bars", "Graphs", "Compact" },
        "Cards: ring gauges with a mini history. Bars: Task Manager style list. "
        "Graphs: a history chart per item. Compact: one-line readouts for small strips.").in ("Style"));

    for (const auto& m : kItems)
        out.push_back (ParamDef::toggle (m.id, m.paramLabel, m.tip).in ("Show"));

    out.push_back (ParamDef::toggle ("perCore", "Per-core CPU bars",
        "Small bars for every logical processor under the CPU reading.")
        .in ("Options").when ([this] { return enabled[itemCpu] && (style == Style::cards || style == Style::bars); }));

    out.push_back (ParamDef::toggle ("infoBar", "Info bar",
        "A line at the top with the CPU and GPU model and the system uptime.")
        .in ("Options").when ([this] { return style != Style::compact; }));

    out.push_back (ParamDef::toggle ("colourByLoad", "Colour by load",
        "Turn readings amber when busy / warm and red when maxed out / hot. Off = theme colour only.")
        .in ("Options"));

    out.push_back (ParamDef::choice ("tempUnit", "Temperature unit", { "Celsius", "Fahrenheit" },
        "Unit used for CPU and GPU temperatures.")
        .in ("Options").when ([this] { return enabled[itemCpuTemp] || enabled[itemGpuTemp]; }));

    out.push_back (ParamDef::slider ("interval", "Refresh interval", 0.5, 5.0, 0.5, " s",
        "How often the system is sampled. Longer = even lighter on the CPU.")
        .in ("Options"));

    out.push_back (ParamDef::slider ("histSpan", "History span", 15.0, 600.0, 15.0, " s",
        "Time covered by the history curves.", 120.0)
        .in ("Options").when ([this] { return style == Style::cards || style == Style::graphs; }));
}

double SystemMonitorModule::getParamValue (const juce::String& id) const
{
    for (int i = 0; i < numItems; ++i)
        if (id == kItems[i].id)
            return enabled[(size_t) i] ? 1.0 : 0.0;

    if (id == "style")        return (double) (int) style;
    if (id == "perCore")      return perCore ? 1.0 : 0.0;
    if (id == "infoBar")      return infoBar ? 1.0 : 0.0;
    if (id == "colourByLoad") return colourByLoad ? 1.0 : 0.0;
    if (id == "tempUnit")     return fahrenheit ? 1.0 : 0.0;
    if (id == "interval")     return intervalSec;
    if (id == "histSpan")     return historySec;
    return 0.0;
}

void SystemMonitorModule::setParamValue (const juce::String& id, double v)
{
    const bool on = v >= 0.5;

    for (int i = 0; i < numItems; ++i)
    {
        if (id == kItems[i].id)
        {
            if (! on)
            {
                int count = 0;
                for (bool e : enabled) count += e ? 1 : 0;
                if (count <= 1 && enabled[(size_t) i])
                    return;   // 至少保留一项，避免空白模块
            }

            enabled[(size_t) i] = on;
            for (const auto& view : buildViews())
                targetFrac[(size_t) view.item] = view.frac;
            repaint();
            return;
        }
    }

    if (id == "style")             style = (Style) juce::jlimit (0, 3, juce::roundToInt (v));
    else if (id == "perCore")      perCore = on;
    else if (id == "infoBar")      infoBar = on;
    else if (id == "colourByLoad") colourByLoad = on;
    else if (id == "tempUnit")     fahrenheit = on;
    else if (id == "interval")
    {
        intervalSec = juce::jlimit (0.5, 5.0, v);
        if (subscribed)
            sampler->subscribe (this, intervalSec);   // 同一订阅者：更新间隔
    }
    else if (id == "histSpan")     historySec = juce::jlimit (15.0, 600.0, v);
    else return;

    repaint();
}

juce::ValueTree SystemMonitorModule::saveModuleSpecificState() const
{
    juce::ValueTree s ("state");

    for (int i = 0; i < numItems; ++i)
        s.setProperty (kItems[i].id, enabled[(size_t) i], nullptr);

    s.setProperty ("style",        (int) style,  nullptr);
    s.setProperty ("perCore",      perCore,      nullptr);
    s.setProperty ("infoBar",      infoBar,      nullptr);
    s.setProperty ("colourByLoad", colourByLoad, nullptr);
    s.setProperty ("tempUnit",     fahrenheit ? 1 : 0, nullptr);
    s.setProperty ("interval",     intervalSec,  nullptr);
    s.setProperty ("histSpan",     historySec,   nullptr);
    return s;
}

void SystemMonitorModule::restoreModuleSpecificState (const juce::ValueTree& state)
{
    // 缺键 = 保持当前值
    auto en = enabled;
    for (int i = 0; i < numItems; ++i)
        if (state.hasProperty (kItems[i].id))
            en[(size_t) i] = (bool) state.getProperty (kItems[i].id);

    if (std::any_of (en.begin(), en.end(), [] (bool b) { return b; }))
        enabled = en;

    if (state.hasProperty ("style"))        style = (Style) juce::jlimit (0, 3, (int) state.getProperty ("style"));
    if (state.hasProperty ("perCore"))      perCore      = (bool) state.getProperty ("perCore");
    if (state.hasProperty ("infoBar"))      infoBar      = (bool) state.getProperty ("infoBar");
    if (state.hasProperty ("colourByLoad")) colourByLoad = (bool) state.getProperty ("colourByLoad");
    if (state.hasProperty ("tempUnit"))     fahrenheit   = (int) state.getProperty ("tempUnit") == 1;
    if (state.hasProperty ("interval"))     intervalSec  = juce::jlimit (0.5, 5.0, (double) state.getProperty ("interval"));
    if (state.hasProperty ("histSpan"))     historySec   = juce::jlimit (15.0, 600.0, (double) state.getProperty ("histSpan"));

    if (subscribed)
        sampler->subscribe (this, intervalSec);

    refreshControlPanel();
    repaint();
}
