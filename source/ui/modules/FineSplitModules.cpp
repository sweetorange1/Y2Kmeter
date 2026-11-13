#include "source/ui/modules/FineSplitModules.h"
#include "source/i18n/Localisation.h"
#include "source/ui/PinkXPStyle.h"
#include "source/ui/HoverHelp.h"
#include "source/ui/toys/ToyKit.h"
#include "source/analysis/AnalyserHub.h"

// ==========================================================
// VuMeterModule —— 指针式模拟 VU 表
//
// 视觉差异化：
//   · 采用**半圆扇形表盘 + 单指针**的经典模拟 VU 表造型，区别于项目里
//     其他基于方块柱 / 数字读数 / 示波器曲线的模块
//   · 圆心位于表盘底部中点，扇形跨度约 180° (水平左 -> 正上 -> 水平右)
//   · 指针 = pink600（单指针，总电平）
//   · 刻度：-36 .. +3 dBFS 均匀等距
//   · 右上角 LED 圆灯：dim / 绿 (> -25 dBFS) / 红 (>= 0 dBFS 危险)
//
// 数据来源：AnalyserHub::Kind::Oscilloscope → 从 2048 样本原始波形尾部
//         取 ~20ms 窗口自算瞬时 RMS，后端音频线程零新增分析计算
// ==========================================================
VuMeterModule::VuMeterModule(AnalyserHub& h)
    : ModulePanel(ModuleType::vuMeter), hub(h)
{
    // 订阅 Oscilloscope —— 复用 Waveform 模块用的同一份 2048 样本原始波形，
    //   这是目前后端最"快"的电平数据源（每 33ms 帧到达时里面就有最新样本）。
    //   不订阅 Loudness：它的 rmsL/R 只在 400ms 块边界刷新一次，滞后严重。
    //   → 后端音频线程零新增计算。
    hub.retain(AnalyserHub::Kind::Oscilloscope);
    hub.addFrameListener(this);

    setMinSize(96, 80);
    setDefaultSize(320, 224);
    setTitleText("VU Meter");
    setControlPanelEnabled (true);

    // v2.9：主路径改为 onFrame 推进弹道 + 重绘（跟随帧分发频率，可达 120fps）；
    //   Timer 仅作 10Hz 兜底 —— onFrame 超过 100ms 未到达时主动拉最新帧推进。
    lastTickMs = juce::Time::getMillisecondCounterHiRes();
    startTimerHz(10);
}

VuMeterModule::~VuMeterModule()
{
    stopTimer();
    hub.removeFrameListener(this);
    hub.release(AnalyserHub::Kind::Oscilloscope);
}

void VuMeterModule::onFrame (const AnalyserHub::FrameSnapshot& frame)
{
    if (! frame.has (AnalyserHub::Kind::Oscilloscope)) return;

    // 从 oscL/R 尾部（即最新的一段样本）取 ~20ms 窗口算瞬时 RMS
    //   · 波形缓冲是 2048 样本、"时间从旧到新"，末尾就是最近的样本
    //   · 采样率通过 AudioProcessor 拿不到最方便的引用，这里用启发式估计：
    //     典型 44.1/48 kHz → 20ms ≈ 880/960 样本；我们取 960 作为上限，
    //     若实际 sr 偏低（比如 44.1k）窗口略超 20ms 也无碍。
    const int bufLen = (int) frame.oscL.size(); // = 2048
    if (bufLen <= 0) return;

    constexpr int windowSamples = 960; // ≈ 20ms @ 48kHz，≈ 21.8ms @ 44.1kHz
    const int n = juce::jmin(bufLen, windowSamples);
    const int startIdx = bufLen - n; // 从末尾向前 n 个样本

    double sumSqL = 0.0;
    double sumSqR = 0.0;
    for (int i = startIdx; i < bufLen; ++i)
    {
        const float sL = frame.oscL[(size_t) i];
        const float sR = frame.oscR[(size_t) i];
        sumSqL += (double) sL * (double) sL;
        sumSqR += (double) sR * (double) sR;
    }
    const double meanSqL = sumSqL / (double) n;
    const double meanSqR = sumSqR / (double) n;

    // 线性 RMS → dBFS；低于门限（接近数字底噪）直接报 -144 让指针回到左端
    auto msToDb = [](double meanSq) -> float
    {
        if (meanSq <= 1.0e-10) return -144.0f;
        return (float)(10.0 * std::log10(meanSq));
    };
    targetL = msToDb(meanSqL);
    targetR = msToDb(meanSqR);

    // v2.9：主路径由分发帧直接推进弹道 + repaint。
    // v2.9.2：读数类 30Hz 重绘 —— 隔帧推进弹道（dt 翻倍保持时间常数一致）。
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    lastOnFrameMs = nowMs;
    lastTickMs    = nowMs;
    if ((frame.tickCount & 1ull) == 0)
        advanceBallistics (frame.dtSec * 2000.0f);
}

void VuMeterModule::timerCallback()
{
    // v2.9：Timer 降级为兜底（见构造函数）：onFrame 正常到达时什么都不做
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    if (nowMs - lastOnFrameMs < 100.0)
    {
        lastTickMs = nowMs;
        return;
    }

    // 按真实 dt（毫秒）推进
    double dtMs = nowMs - lastTickMs;
    lastTickMs  = nowMs;
    if (dtMs < 0.0)   dtMs = 16.0;
    if (dtMs > 100.0) dtMs = 100.0;
    const float dt = (float) dtMs;

    // ======================================================================
    // 防御性 pull 兜底：
    //
    //   主路径依赖 AnalyserHub::FrameDispatcher 的 fanout 调用 VuMeterModule::
    //   onFrame —— 但 Hub 的 fanout 里有个 `if (!comp->isShowing() ...) skip` 的
    //   活跃检查（见 AnalyserHub.cpp 里对 FrameListener 做 dynamic_cast<Component*>
    //   后调 isShowing 的那段）。在以下场景会触发跳过：
    //
    //     · 插件宿主挂起 / 隐藏 Editor 窗口瞬间 isShowing()=false；
    //     · 切换布局预设 / 新建模块 的一两帧里组件尺寸为 0；
    //     · workspace 子组件 hierarchy 在重排中间态 isShowing() 暂短为 false；
    //     · OpenGLContext attachTo 初始化的首帧 Component 仍未 showing；
    //
    //   以上任何一种瞬间出现，VU 的 onFrame 都拿不到新的 frame 数据，targetL/R
    //   保持 -144，而 60Hz Timer 的 smoothOne 让 displayedL/R 也收敛到 -144，
    //   指针就"贴死在 -20"（minDisplayDb）并很难恢复。
    //
    //   修复：每个 60Hz tick 主动 hub.getLatestFrame() 拉一次最新帧，自己重算
    //   targetL/R。这一路和 onFrame push 路径完全独立，即便 fanout 出于任何
    //   原因跳过本模块，Pull 路径仍能把数据灌进来。
    //
    //   成本：每帧一次 SpinLock + shared_ptr 拷贝（Hub::getLatestFrame 内部已
    //   做原子发布），在 60Hz 下开销可忽略。
    // ======================================================================
    if (auto frame = hub.getLatestFrame())
    {
        if (frame->has (AnalyserHub::Kind::Oscilloscope))
        {
            const int bufLen = (int) frame->oscL.size(); // 2048
            if (bufLen > 0)
            {
                constexpr int windowSamples = 960; // ≈ 20ms @ 48kHz
                const int n = juce::jmin (bufLen, windowSamples);
                const int startIdx = bufLen - n;

                double sumSqL = 0.0;
                double sumSqR = 0.0;
                for (int i = startIdx; i < bufLen; ++i)
                {
                    const float sL = frame->oscL[(size_t) i];
                    const float sR = frame->oscR[(size_t) i];
                    sumSqL += (double) sL * (double) sL;
                    sumSqR += (double) sR * (double) sR;
                }
                const double meanSqL = sumSqL / (double) n;
                const double meanSqR = sumSqR / (double) n;

                auto msToDb = [](double ms) -> float
                {
                    if (ms <= 1.0e-10) return -144.0f;
                    return (float) (10.0 * std::log10 (ms));
                };
                targetL = msToDb (meanSqL);
                targetR = msToDb (meanSqR);
            }
        }
    }

    advanceBallistics (dt);
}

void VuMeterModule::advanceBallistics (float dt)
{
    dt = juce::jlimit (0.0f, 100.0f, dt);

    // 非对称弹道：上升 τ=tauRiseMs（抓瞬态），下降 τ=tauFallMs（模拟 VU 回落）。
    //   · 连续瞬态情形：每个瞬态都通过上升项立刻把指针抬到新高，之间的回落
    //     按 fall 时间常数执行 —— 指针不会被锁死在顶端，会"踩着节拍"往上
    //     走，节拍停的瞬间也会继续按 fall 缓慢下行。
    //   · 静音（target -> -144）时：由下降项主导，指针平缓归位。
    //   · 反馈修复（外网 #6）：完全静音回零使用更慢的 tauMuteFallMs，
    //     表针不再"一瞬间打到底"。
    const float aUp      = 1.0f - std::exp(-dt / tauRiseMs);
    const float aDown    = 1.0f - std::exp(-dt / tauFallMs);
    const float aMuteDown = 1.0f - std::exp(-dt / tauMuteFallMs);

    auto smoothOne = [&](float& disp, float tgt)
    {
        if (! std::isfinite(disp)) disp = -144.0f;
        if (! std::isfinite(tgt))  tgt  = -144.0f;
        const float a = (tgt > disp) ? aUp
                      : (tgt <= -143.0f ? aMuteDown : aDown);
        disp += a * (tgt - disp);
    };
    smoothOne(displayedL, targetL);
    smoothOne(displayedR, targetR);

    // LED 跟随（基于 displayed 的单声道合成），非对称：上升立即跟、下降快速衰减
    const float monoNow = currentMonoDbfs();
    if (monoNow > ledLevelDb)
    {
        ledLevelDb = monoNow;
    }
    else
    {
        constexpr float tauLedMs = 150.0f; // 略快于指针回落
        const float aLed = 1.0f - std::exp(-dt / tauLedMs);
        ledLevelDb += aLed * (monoNow - ledLevelDb);
    }

    // 脉动相位（~0.9 秒一圈）
    constexpr float omegaRadPerMs = juce::MathConstants<float>::twoPi / 900.0f;
    pulsePhase += omegaRadPerMs * dt;
    if (pulsePhase > juce::MathConstants<float>::twoPi)
        pulsePhase -= juce::MathConstants<float>::twoPi;

    repaintFrameContent();
}

float VuMeterModule::currentMonoDbfs() const noexcept
{
    // 把 displayed L/R dBFS 转线性功率、平均、再转 dBFS
    //   等价于 10*log10((10^(L/10) + 10^(R/10)) / 2)
    auto dbToPower = [](float db) -> float
    {
        if (db <= -143.0f || ! std::isfinite(db)) return 0.0f;
        return std::pow(10.0f, db * 0.1f);
    };
    const float pL = dbToPower(displayedL);
    const float pR = dbToPower(displayedR);
    const float pAvg = (pL + pR) * 0.5f;
    return (pAvg > 1.0e-14f) ? 10.0f * std::log10(pAvg) : -144.0f;
}

float VuMeterModule::dialTFor(float db) const noexcept
{
    // dBFS 线性映射到 [0,1] —— 表盘上的 dB 刻度是等距线性的，符合
    //   "-36 / -20 / -6 / 0" 这些整数 dB 在表盘上的常规感知。
    return juce::jlimit(0.0f, 1.0f,
                        juce::jmap(db, minDisplayDb, maxDisplayDb, 0.0f, 1.0f));
}

float VuMeterModule::vuToAngle(float valueDb) const noexcept
{
    // 半圆：-90° ~ +90°（水平左 → 水平右）
    // 弧形：-120° ~ +120°（240° 圆表，两端落在轴心下方）
    const float half = (dialStyle == 1)
                         ? juce::MathConstants<float>::pi * 2.0f / 3.0f
                         : juce::MathConstants<float>::halfPi;
    return juce::jmap(dialTFor(valueDb), 0.0f, 1.0f, -half, half);
}

float VuMeterModule::wedgeXFor(float db) const noexcept
{
    return juce::jmap(dialTFor(db), 0.0f, 1.0f, dialGeom.leftX, dialGeom.rightX);
}

float VuMeterModule::wedgeTickLengthFor(float db) const noexcept
{
    // 楔形剪影：低电平一侧刻度长、高电平一侧短；警戒段再抬高一档形成浅 V 形基线。
    //   具体像素长度按模块高度缩放（dialGeom.tickLenHi），小模块里不会顶到底边。
    const float t  = dialTFor(db);
    const float tw = dialTFor(warnDbfs);
    const float f  = (t <= tw)
                       ? juce::jmap(t, 0.0f, juce::jmax(1.0e-4f, tw), 1.00f, 0.46f)
                       : juce::jmap(t, tw, 1.0f, 0.38f, 0.12f);
    return dialGeom.tickLenHi * f;
}

float VuMeterModule::wedgeBaselineY(float db) const noexcept
{
    return dialGeom.tickTopY + wedgeTickLengthFor(db);
}

float VuMeterModule::needleAngleFor(float db) const noexcept
{
    if (dialStyle == 2 && dialGeom.rightX > dialGeom.leftX + 1.0f)
    {
        // 楔形刻度：尖端落在刻度基线上，角度由尖端位置反算（方向 = (sinθ, -cosθ)）
        const float dx = wedgeXFor(db) - dialGeom.pivot.x;
        const float dy = wedgeBaselineY(db) - dialGeom.pivot.y;
        return std::atan2(dx, -dy);
    }
    return vuToAngle(db);
}

float VuMeterModule::needleLengthFor(float db) const noexcept
{
    if (dialStyle == 2 && dialGeom.rightX > dialGeom.leftX + 1.0f)
    {
        const float dx = wedgeXFor(db) - dialGeom.pivot.x;
        const float dy = wedgeBaselineY(db) - dialGeom.pivot.y;
        return juce::jmax(8.0f, std::sqrt(dx * dx + dy * dy));
    }
    return juce::jmax(8.0f, dialGeom.radius - 10.0f);
}

// ----------------------------------------------------------
// Toybox（v3.0.0）：玻璃裂 + 迪斯科灯（§8.2）
// ----------------------------------------------------------
bool VuMeterModule::toyHitTest (juce::Point<int> panelPos, ToyHit& out) const
{
    const auto area = getContentBounds().reduced (6);
    if (! area.contains (panelPos))
        return false;

    const auto ledArea = ledBounds (area);

    out.kind = ledArea.contains (panelPos) ? 1 : 2;   // 1=迪斯科灯 2=玻璃裂
    out.rect = area.toFloat();
    out.pos  = panelPos.toFloat();
    return true;
}

void VuMeterModule::toyPoke (const ToyHit& hit, ToyContext& ctx)
{
    if (hit.kind == 1)
    {
        const auto ledRect = ledBounds (hit.rect.getSmallestIntegerContainer());
        ctx.addModuleActor (makeDiscoLedActor (ledRect.toFloat()));
        // 阈值气泡（E.4.2）
        const juce::String th = (ledLevelDb >= warnDbfs) ? "CLIP >= 0"
                              : (ledLevelDb >= ledSignalDbfs) ? "SIGNAL > -25 dBFS" : "NO SIGNAL";
        ctx.bubble (hit.pos.translated (0.0f, -18.0f), th, ToyRole::Reward);
    }
    else
    {
        // 反馈 A6：碎屏改为指针虚影 —— 几何直接取上一帧 drawDial 的结果，
        //   三种表盘（半圆 / 弧形 / 梯形）的轴心与半径都正确。
        const float th = needleAngleFor (currentMonoDbfs())
                           + juce::MathConstants<float>::halfPi;
        const bool haveGeom = dialGeom.radius > 4.0f;
        const juce::Point<float> piv = haveGeom
            ? dialGeom.pivot
            : juce::Point<float> ((float) hit.rect.getCentreX(), (float) hit.rect.getBottom() - 8.0f);
        const float R = haveGeom
            ? dialGeom.radius
            : juce::jmax (4.0f, juce::jmin (hit.rect.getWidth() * 0.5f,
                                            hit.rect.getHeight()) - 14.0f);
        ctx.addModuleActor (makeGhostNeedleActor (piv, R, th));
        ctx.bubble (hit.pos.translated (0.0f, -20.0f),
                    "VU " + juce::String (currentMonoDbfs(), 1) + " dB", ToyRole::Reward);
    }
}

namespace
{
    // VU 表盘刻度：-36 … +3 dBFS，每 1 dB 一根；主刻度带数字。
    //   三种表盘共用同一张表（半圆 / 弧形按角度排布，梯形按顶边线性排布）。
    struct VuTick { float db; const char* label; bool major; };

    const VuTick kVuTicks[] = {
        { -36.0f, "-36", true  }, { -35.0f, nullptr, false }, { -34.0f, nullptr, false },
        { -33.0f, nullptr, false }, { -32.0f, nullptr, false }, { -31.0f, nullptr, false },
        { -30.0f, "-30", true  }, { -29.0f, nullptr, false }, { -28.0f, nullptr, false },
        { -27.0f, nullptr, false }, { -26.0f, nullptr, false }, { -25.0f, "-25", true  },
        { -24.0f, nullptr, false }, { -23.0f, nullptr, false }, { -22.0f, nullptr, false },
        { -21.0f, nullptr, false }, { -20.0f, "-20", true  }, { -19.0f, nullptr, false },
        { -18.0f, nullptr, false }, { -17.0f, nullptr, false }, { -16.0f, nullptr, false },
        { -15.0f, "-15", true  }, { -14.0f, nullptr, false }, { -13.0f, nullptr, false },
        { -12.0f, nullptr, false }, { -11.0f, nullptr, false }, { -10.0f, "-10", true  },
        {  -9.0f, nullptr, false }, {  -8.0f, nullptr, false }, {  -7.0f, nullptr, false },
        {  -6.0f, nullptr, false }, {  -5.0f, "-5",  true  }, {  -4.0f, nullptr, false },
        {  -3.0f, "-3",  true  }, {  -2.0f, nullptr, false }, {  -1.0f, nullptr, false },
        {   0.0f,  "0",  true  }, {  +1.0f, nullptr, false }, {  +2.0f, nullptr, false },
        {  +3.0f,  "+3", true  },
    };
}

void VuMeterModule::drawDial(juce::Graphics& g,
                             juce::Rectangle<int> dialArea,
                             juce::Point<float>&  outPivot,
                             float&               outRadius) const
{
    // 三种结构完全不同的表盘：
    //   0 半圆（经典）· 1 弧形（240° 圆表，深色面）· 2 梯形（顶边直线刻度）
    if (dialStyle == 1) { drawArcDial (g, dialArea, outPivot, outRadius); return; }
    if (dialStyle == 2) { drawTrapezoidDial (g, dialArea, outPivot, outRadius); return; }

    // 注意：sunken 卡片背景（pink50）已由 paintContent 在外层统一绘制，
    //   这里不再重复 drawSunken，避免出现双层凹陷边框。
    //   dialArea 即 sunken 卡片的整体区域，我们只需从中留出一点边距
    //   供圆弧/刻度/文字使用即可。
    auto inner = dialArea.reduced(10);
    if (inner.getWidth() < 20 || inner.getHeight() < 20) return;

    // 圆心 = 内框底边中点（半圆 0°-180° 以底边水平为直径）
    //   · 指针从水平左(-90°) 扫到水平右(+90°)，所以圆心必须在底边上
    //   · cy 稍微上移 2px，给底部的圆心轴 + 描边留一点边距
    const float cx = (float) inner.getCentreX();
    const float cy = (float) inner.getBottom() - 2.0f;

    // 半径约束：
    //   · 横向 —— 半圆直径 = 2R，所以 R <= width/2
    //   · 纵向 —— 半圆高度 = R，所以 R <= height
    //   再各留 4px 呼吸空间防止贴到 drawSunken 内沿
    const float R = juce::jmin((float) inner.getWidth()  * 0.5f - 4.0f,
                                (float) inner.getHeight()        - 4.0f);
    if (R < 10.0f) return;

    outPivot  = { cx, cy };
    outRadius = R;

    // 半圆表盘的前景统一用主题 ink（在浅粉底上有良好对比度）
    const juce::Colour inkCol = PinkXP::ink;

    // ---- 弧形主刻度轨（0°~180° 上半圆） ----
    juce::Path arc;
    const float leftRad  = -juce::MathConstants<float>::halfPi;
    const float rightRad = +juce::MathConstants<float>::halfPi;
    // JUCE addCentredArc 的角度 0 = 12 点方向、顺时针为正
    arc.addCentredArc(cx, cy, R, R, 0.0f, leftRad, rightRad, true);
    g.setColour(inkCol.withAlpha(0.85f));
    g.strokePath(arc, juce::PathStrokeType(1.8f));

    // ---- 内弧（红色警戒段：warnDbfs 到 0 dBFS） ----
    juce::Path arcRed;
    const float redStart = vuToAngle(warnDbfs);
    arcRed.addCentredArc(cx, cy, R, R, 0.0f, redStart, rightRad, true);
    g.setColour(juce::Colour(0xffd21f3a).withAlpha(0.9f));
    g.strokePath(arcRed, juce::PathStrokeType(3.0f));

    // ---- 刻度与数字（dBFS，-36 .. +3 均匀等距，表见 kVuTicks）----
    g.setFont(PinkXP::getAxisFont(9.0f, juce::Font::bold));
    for (const auto& t : kVuTicks)
    {
        const float ang = vuToAngle(t.db);
        const float sinA = std::sin(ang);
        const float cosA = std::cos(ang);
        // 刻度线从半径 R 向内延伸 (长刻度 7px，短刻度 4px)
        const float tickLen = t.major ? 7.0f : 4.0f;
        const float x1 = cx + sinA * R;
        const float y1 = cy - cosA * R;
        const float x2 = cx + sinA * (R - tickLen);
        const float y2 = cy - cosA * (R - tickLen);

        // 红色警戒段：达到 warnDbfs 开始变红
        const bool red = t.db >= warnDbfs;
        g.setColour(red ? juce::Colour(0xffb61325) : inkCol);
        g.drawLine(x1, y1, x2, y2, t.major ? 1.8f : 1.2f);

        // 数字标签 (贴在刻度外侧内圈 R-14)
        if (t.label != nullptr)
        {
            const float tx = cx + sinA * (R - 16.0f);
            const float ty = cy - cosA * (R - 16.0f);
            juce::Rectangle<float> labRect (tx - 12.0f, ty - 6.0f, 24.0f, 12.0f);
            g.setColour(red ? juce::Colour(0xffb61325) : inkCol);
            PinkXP::drawCachedText(g, t.label, labRect, juce::Justification::centred,
                                   9.0f, juce::Font::bold, red ? juce::Colour(0xffb61325) : inkCol, true);
        }
    }

    // ---- 表盘面文字 "dBFS" ----
    g.setColour(inkCol.withAlpha(0.75f));
    g.setFont(PinkXP::getFont(12.0f, juce::Font::bold));
    // 圆心在底边之上一点，"dBFS" 放在圆心偏上
    const float ly = cy - R * 0.42f;
    PinkXP::drawCachedText(g, "dBFS", juce::Rectangle<float>(cx - 30.0f, ly - 8.0f, 60.0f, 16.0f),
                           juce::Justification::centred, 12.0f, juce::Font::bold, inkCol.withAlpha(0.75f));

    // ---- 圆心轴 (主题色小凸起 + ink 描边) ----
    g.setColour(PinkXP::pink300);
    g.fillEllipse(cx - 5.0f, cy - 5.0f, 10.0f, 10.0f);
    g.setColour(inkCol);
    g.drawEllipse(cx - 5.0f, cy - 5.0f, 10.0f, 10.0f, 1.2f);

    dialGeom.pivot  = outPivot;
    dialGeom.radius = outRadius;
}

// ==========================================================
// 弧形表盘（样式 1）：240° 圆表 —— 粗弧轨 + 轴心居中，深色面
//   与半圆表盘的区别：不是"底边为直径的上半圆"，而是绕着轴心转
//   240° 的整圈仪表，两端落在轴心下方。
// ==========================================================
void VuMeterModule::drawArcDial(juce::Graphics& g,
                                juce::Rectangle<int> dialArea,
                                juce::Point<float>&  outPivot,
                                float&               outRadius) const
{
    auto inner = dialArea.reduced(8);
    if (inner.getWidth() < 40 || inner.getHeight() < 34) return;

    const juce::Colour inkCol = PinkXP::ink;        // 跟随主题（暗色主题下自动变浅）
    const juce::Colour redCol(0xffd21f3a);          // 警戒段：与另两种表盘同一语义色
    const float half = juce::MathConstants<float>::pi * 2.0f / 3.0f;   // 120°

    // 图形竖向范围 = R（上）+ 0.5R（下）+ 内外描边，横向 = 2R
    const float R = juce::jmin((float) inner.getWidth()  * 0.5f - 10.0f,
                               ((float) inner.getHeight() - 14.0f) / 1.5f);
    if (R < 14.0f) return;

    const float cx = (float) inner.getCentreX();
    const float cy = (float) inner.getY() + 7.0f + R;

    outPivot  = { cx, cy };
    outRadius = R;
    dialGeom.pivot   = outPivot;
    dialGeom.radius  = R;

    // ---- 粗弧轨：整段淡色 + 警戒段红色 ----
    {
        juce::Path rail;
        rail.addCentredArc(cx, cy, R, R, 0.0f, -half, half, true);
        g.setColour(inkCol.withAlpha(0.16f));
        g.strokePath(rail, juce::PathStrokeType(9.0f, juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded));
    }
    {
        juce::Path railRed;
        railRed.addCentredArc(cx, cy, R, R, 0.0f, vuToAngle(warnDbfs), half, true);
        g.setColour(redCol.withAlpha(0.85f));
        g.strokePath(railRed, juce::PathStrokeType(9.0f, juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded));
    }

    // ---- 内外细描边（让弧轨像一根嵌进面板的轨道）----
    {
        juce::Path inEdge, outEdge;
        inEdge .addCentredArc(cx, cy, R - 5.5f, R - 5.5f, 0.0f, -half, half, true);
        outEdge.addCentredArc(cx, cy, R + 5.5f, R + 5.5f, 0.0f, -half, half, true);
        g.setColour(inkCol.withAlpha(0.45f));
        g.strokePath(inEdge,  juce::PathStrokeType(1.0f));
        g.strokePath(outEdge, juce::PathStrokeType(1.0f));
    }

    // ---- 刻度（向内）+ 数字（再靠内一档）----
    g.setFont(PinkXP::getAxisFont(9.0f, juce::Font::bold));
    for (const auto& t : kVuTicks)
    {
        const float ang = vuToAngle(t.db);
        const float sa = std::sin(ang), ca = std::cos(ang);
        const float r0 = R - 6.0f;
        const float r1 = r0 - (t.major ? 7.0f : 4.0f);
        const bool red = t.db >= warnDbfs;
        const auto col = red ? juce::Colour(0xffb61325) : inkCol;

        g.setColour(col.withAlpha(t.major ? 0.95f : 0.6f));
        g.drawLine(cx + sa * r0, cy - ca * r0, cx + sa * r1, cy - ca * r1,
                   t.major ? 1.6f : 1.0f);

        if (t.label != nullptr)
        {
            const float tx = cx + sa * (r1 - 11.0f);
            const float ty = cy - ca * (r1 - 11.0f);
            PinkXP::drawCachedText(g, t.label,
                                   juce::Rectangle<float>(tx - 12.0f, ty - 6.0f, 24.0f, 12.0f),
                                   juce::Justification::centred, 9.0f, juce::Font::bold, col, true);
        }
    }

    // ---- 中央单位文字 ----
    g.setColour(inkCol.withAlpha(0.65f));
    PinkXP::drawCachedText(g, "dBFS",
                           juce::Rectangle<float>(cx - 30.0f, cy - R * 0.45f - 8.0f, 60.0f, 16.0f),
                           juce::Justification::centred, 11.0f, juce::Font::bold,
                           inkCol.withAlpha(0.65f));

    // ---- 轴心 ----
    g.setColour(PinkXP::pink300);
    g.fillEllipse(cx - 6.0f, cy - 6.0f, 12.0f, 12.0f);
    g.setColour(inkCol);
    g.drawEllipse(cx - 6.0f, cy - 6.0f, 12.0f, 12.0f, 1.2f);
}

// ==========================================================
// 楔形刻度表盘（样式 2）：长指针 + 梯形剪影的刻度带
//   沿用老式 VU 表的"扇形刻度"画法：
//     · 数字排在刻度线正上方；
//     · 刻度线长度随电平原位变化 —— 低电平侧长、高电平侧短，整排刻度
//       的外轮廓就是一个梯形（警戒段再抬高一档，基线呈浅 V 形）；
//     · 指针从底部中点扫向刻度基线，尖端落基线上，像一根长摆杆。
// ==========================================================
void VuMeterModule::drawTrapezoidDial(juce::Graphics& g,
                                      juce::Rectangle<int> dialArea,
                                      juce::Point<float>&  outPivot,
                                      float&               outRadius) const
{
    auto inner = dialArea.reduced(8);
    if (inner.getWidth() < 48 || inner.getHeight() < 42) return;

    const juce::Colour inkCol = PinkXP::ink;
    const juce::Colour redCol(0xffd21f3a);

    dialGeom.leftX    = (float) inner.getX() + 6.0f;
    dialGeom.rightX   = (float) inner.getRight() - 6.0f;
    dialGeom.tickTopY = (float) inner.getY() + 17.0f;      // 数字行（14px）之下
    if (dialGeom.rightX - dialGeom.leftX < 40.0f) return;

    // 最长刻度：给轴心留出 8px，其余高度按 0.62 折算 → 小模块也不会溢出
    dialGeom.tickLenHi = juce::jlimit(8.0f, 30.0f,
                                      ((float) inner.getBottom() - 2.0f
                                        - dialGeom.tickTopY) * 0.62f);

    const float numTop = (float) inner.getY() + 1.0f;

    // ---- 数字（刻度线正上方）----
    g.setFont(PinkXP::getAxisFont(9.0f, juce::Font::bold));
    for (const auto& t : kVuTicks)
    {
        if (t.label == nullptr) continue;

        const float tx  = wedgeXFor(t.db);
        const bool  red = t.db >= warnDbfs;
        const auto  col = red ? juce::Colour(0xffb61325) : inkCol;
        const float lx  = juce::jlimit((float) inner.getX(),
                                       (float) inner.getRight() - 24.0f, tx - 12.0f);
        PinkXP::drawCachedText(g, t.label, juce::Rectangle<float>(lx, numTop, 24.0f, 12.0f),
                               juce::Justification::centred, 9.0f, juce::Font::bold, col, true);
    }

    // ---- 刻度线：长度按电平变化 → 梯形剪影 ----
    for (const auto& t : kVuTicks)
    {
        const float tx  = wedgeXFor(t.db);
        const float by  = wedgeBaselineY(t.db);
        const bool  red = t.db >= warnDbfs;
        const auto  col = red ? juce::Colour(0xffb61325) : inkCol;

        g.setColour(col.withAlpha(t.major ? 0.95f : 0.55f));
        g.drawLine(tx, dialGeom.tickTopY, tx, by, t.major ? 2.0f : 1.2f);
    }

    // ---- 基线：跟着刻度末端走，警戒段单独一段红色并加粗 ----
    {
        juce::Path baseInk, baseRed;
        bool firstInk = true, firstRed = true;

        for (const auto& t : kVuTicks)
        {
            const float x = wedgeXFor(t.db);
            const float y = wedgeBaselineY(t.db) + 1.5f;

            if (t.db < warnDbfs)
            {
                if (firstInk) { baseInk.startNewSubPath(x, y); firstInk = false; }
                else            baseInk.lineTo(x, y);
            }

            // 红段从警戒前 1 dB 起笔，和墨段重叠一格，避免出现断口
            if (t.db >= warnDbfs - 1.0f)
            {
                if (firstRed) { baseRed.startNewSubPath(x, y); firstRed = false; }
                else            baseRed.lineTo(x, y);
            }
        }

        g.setColour(inkCol.withAlpha(0.85f));
        g.strokePath(baseInk, juce::PathStrokeType(2.4f));
        g.setColour(redCol.withAlpha(0.95f));
        g.strokePath(baseRed, juce::PathStrokeType(3.2f));
    }

    // ---- 轴心（底边中点，指针从这里摆出）----
    const juce::Point<float> piv((float) inner.getCentreX(), (float) inner.getBottom() - 2.0f);
    g.setColour(PinkXP::pink300);
    g.fillEllipse(piv.x - 4.0f, piv.y - 4.0f, 8.0f, 8.0f);
    g.setColour(inkCol);
    g.drawEllipse(piv.x - 4.0f, piv.y - 4.0f, 8.0f, 8.0f, 1.1f);

    outPivot  = piv;
    outRadius = juce::jmax(10.0f, piv.y - dialGeom.tickTopY);

    dialGeom.pivot  = piv;
    dialGeom.radius = outRadius;
}

void VuMeterModule::drawNeedle(juce::Graphics& g,
                               float              valueDb,
                               juce::Colour       colour,
                               float              thickness,
                               bool               wedge) const
{
    // 几何由 drawDial 按当前样式缓存：半圆 / 弧形是"角度 + 半径"，
    //   梯形是"尖端沿顶边线性移动"，这里统一换算成角度 + 长度。
    const float ang  = needleAngleFor(valueDb);
    const float sinA = std::sin(ang);
    const float cosA = std::cos(ang);
    const auto  pivot = dialGeom.pivot;

    // 指针尖端：长度已按样式算好（半圆/弧形 = R-10，避开刻度数字）
    const float tipR = needleLengthFor(valueDb);
    // 指针根部：圆心后方伸出一点（配重效果）；梯形轴心在底边，几乎不留尾巴
    const float tailR = (dialStyle == 2) ? 2.0f : 8.0f;

    const float x1 = pivot.x - sinA * tailR;
    const float y1 = pivot.y + cosA * tailR;
    const float x2 = pivot.x + sinA * tipR;
    const float y2 = pivot.y - cosA * tipR;

    if (wedge)
    {
        // 锥形指针：根部宽 (thickness * 2.4)、尖端收到一点
        const float nx = cosA * thickness * 1.2f;
        const float ny = sinA * thickness * 1.2f;
        juce::Path p;
        p.startNewSubPath(x1 + nx, y1 + ny);
        p.lineTo(x2, y2);
        p.lineTo(x1 - nx, y1 - ny);
        p.closeSubPath();

        g.setColour(juce::Colour(0xff2a1a08).withAlpha(0.35f));
        g.fillPath(p, juce::AffineTransform::translation(1.0f, 1.5f));
        g.setColour(colour);
        g.fillPath(p);
        return;
    }

    // 阴影 (在指针下方偏移 1.5 px 画一根半透明深色)
    g.setColour(juce::Colour(0xff2a1a08).withAlpha(0.35f));
    g.drawLine(x1 + 1.0f, y1 + 1.5f, x2 + 1.0f, y2 + 1.5f, thickness);

    // 指针本体
    g.setColour(colour);
    g.drawLine(x1, y1, x2, y2, thickness);

    // 指针尖端小圆点
    g.setColour(colour.darker(0.2f));
    g.fillEllipse(x2 - 1.5f, y2 - 1.5f, 3.0f, 3.0f);
}

void VuMeterModule::drawLed(juce::Graphics& g, juce::Rectangle<int> ledArea) const
{
    if (ledArea.getWidth() < 6 || ledArea.getHeight() < 6) return;

    const int sz = juce::jmin(ledArea.getWidth(), ledArea.getHeight());
    auto square = juce::Rectangle<int>(ledArea.getRight() - sz,
                                        ledArea.getY(),
                                        sz, sz).reduced(2);

    const float cx = (float) square.getCentreX();
    const float cy = (float) square.getCentreY();
    const float r  = (float) square.getWidth() * 0.5f - 1.0f;

    // 状态判定（使用 onFrame 里维护的实时 ledLevelDb — peak-follower，
    //   基于 currentMonoDbfs，和指针视觉同步）
    //   · ledLevelDb <= ledSignalDbfs → 视为无信号（暗灭，不发光）
    //   · ledLevelDb >= warnDbfs     → 危险（红灯 + 快速脉动），和表盘红色段一致
    //   · 否则                        → 绿灯（慢速呼吸）
    //   注：绿灯点亮阈值 ledSignalDbfs(-25) 与表底 minDisplayDb(-36) 已解耦。
    const bool hasSignal = ledLevelDb > ledSignalDbfs;
    const bool danger    = ledLevelDb >= warnDbfs;

    juce::Colour coreCol, glowCol;
    float pulse = 0.5f;

    if (! hasSignal)
    {
        // 无信号：不亮 —— 用一个非常暗的灰色，且无晕光
        coreCol = juce::Colour(0xff2a2a2a);
        glowCol = juce::Colour(0x00000000);
    }
    else if (danger)
    {
        // 危险：红色 + 快速脉动 (每帧 2× 加速)
        const float phaseFast = pulsePhase * 2.0f;
        pulse = 0.55f + 0.45f * std::sin(phaseFast);
        coreCol = juce::Colour(0xffff2040).interpolatedWith(juce::Colour(0xffffc0c8), 0.15f * pulse);
        glowCol = juce::Colour(0xffff2040).withAlpha(0.45f + 0.45f * pulse);
    }
    else
    {
        // 正常：绿色 + 慢速呼吸
        pulse = 0.6f + 0.4f * std::sin(pulsePhase);
        coreCol = juce::Colour(0xff30d86a).interpolatedWith(juce::Colour(0xffc8ffd4), 0.15f * pulse);
        glowCol = juce::Colour(0xff30d86a).withAlpha(0.35f + 0.45f * pulse);
    }

    // 不使用外围泛光 —— 仅画 LED 本体 + 高光 + 描边
    juce::ignoreUnused(glowCol);

    // LED 本体
    g.setColour(coreCol);
    g.fillEllipse(cx - r, cy - r, r * 2.0f, r * 2.0f);

    // 高光（仅在有信号时显示，凸显球面立体感）
    if (hasSignal)
    {
        g.setColour(juce::Colours::white.withAlpha(0.55f));
        g.fillEllipse(cx - r * 0.6f, cy - r * 0.75f,
                      r * 0.6f,      r * 0.5f);
    }

    // 外圈金属边 (统一用主题 ink)
    g.setColour(PinkXP::ink);
    g.drawEllipse(cx - r, cy - r, r * 2.0f, r * 2.0f, 1.2f);
}

void VuMeterModule::paintContent(juce::Graphics& g, juce::Rectangle<int> content)
{
    // 外层面板背景：统一用主题 btnFace，保证和其它模块留白风格一致
    g.setColour(PinkXP::btnFace);
    g.fillRect(content);

    // 参照 PhaseModule / PhaseCorrelation：只留很小的边距，直接把整块内容
    //   下沉成一个 sunken 卡片（底色为当前主题浅色 pink50），不再在模块边框
    //   上独占一条 LED band。这样模块外边框跟 Phase 模块一致的紧凑效果。
    auto area = content.reduced(6);
    if (area.getWidth() < 40 || area.getHeight() < 40) return;

    // 盘面底色统一跟随主题（不再给弧形表盘固定深色底，否则换主题不生效）
    PinkXP::drawSunken(g, area, PinkXP::content);

    // 表盘区域 = sunken 卡片内部（再减去一点内描边余量，和 drawDial 保持一致）
    auto dialArea = area;

    // 画表盘 (获取圆心 + 半径)
    juce::Point<float> pivot;
    float radius = 0.0f;
    drawDial(g, dialArea, pivot, radius);

    if (radius > 10.0f)
    {
        // 指针和 LED 共用的"单声道总电平"（dBFS）
        const float monoDb = currentMonoDbfs();

        if (needleStyle == 2)
        {
            // Stereo：L / R 各一根细指针（取已平滑的 displayedL/R）
            drawNeedle(g, displayedL, PinkXP::pink500, 1.6f);
            drawNeedle(g, displayedR, PinkXP::pink700, 1.6f);
        }
        else
        {
            // 单一指针（总电平），主题粉色系 pink600，稍粗一点
            drawNeedle(g, monoDb, PinkXP::pink600, 2.2f, needleStyle == 1);
        }
    }

    // ---- LED 信号灯：放到表盘旁边（sunken 卡片内的右上角空白） ----
    //   VU 表盘是 0°~180° 的上半圆，所以卡片的左上角和右上角天然是空白，
    //   把 LED 塞在右上角可避免和指针/刻度重叠，也不再占用模块边框。
    if (showLed)
        drawLed(g, ledBounds(area));
}

juce::Rectangle<int> VuMeterModule::ledBounds(juce::Rectangle<int> area) const
{
    const int ledSz = juce::jlimit(10, 18,
                                   juce::jmin(area.getWidth(), area.getHeight()) / 12);
    // 相对 sunken 内沿再缩 6px，给 drawSunken 的阴影/内描边一点呼吸。
    //   楔形刻度把数字排在顶部，所以灯改放右下角；另两种表盘顶部是空的。
    const int y = (dialStyle == 2) ? (area.getBottom() - ledSz - 6)
                                   : (area.getY()      + 6);
    return { area.getRight() - ledSz - 6, y, ledSz, ledSz };
}

// ----------------------------------------------------------
// 持久化 / 控制面板（v3.1）
// ----------------------------------------------------------
juce::ValueTree VuMeterModule::saveModuleSpecificState() const
{
    juce::ValueTree s ("state");
    s.setProperty ("dialStyle",   dialStyle, nullptr);
    s.setProperty ("needleStyle", needleStyle, nullptr);
    s.setProperty ("showLed",     showLed, nullptr);
    return s;
}

void VuMeterModule::restoreModuleSpecificState (const juce::ValueTree& state)
{
    // 缺键 = 保持当前值（构造默认 = 旧 VU 外观）
    if (state.hasProperty ("dialStyle"))   dialStyle   = juce::jlimit (0, 2, (int) state.getProperty ("dialStyle"));
    if (state.hasProperty ("needleStyle")) needleStyle = juce::jlimit (0, 2, (int) state.getProperty ("needleStyle"));
    if (state.hasProperty ("showLed"))     showLed     = (bool) state.getProperty ("showLed");
    refreshControlPanel();
    repaint();
}

void VuMeterModule::buildParamDefs (std::vector<ParamDef>& out) const
{
    out.push_back (ParamDef::choice ("dialStyle", "Dial style",
        { "Half circle", "Arc", "Trapezoid" },
        "Half circle: the classic pink semicircle. Arc: a 240-degree round gauge with a thick rail. "
        "Trapezoid: a vintage fan scale - long ticks on the loud side, short on the quiet side, "
        "with a long needle swinging across it.")
        .in ("Dial"));
    out.push_back (ParamDef::choice ("needleStyle", "Needle style",
        { "Thin line", "Wedge", "Stereo L/R" },
        "Thin line: thin needle. Wedge: tapered needle. Stereo L/R: two thin needles, one per channel.")
        .in ("Dial"));
    out.push_back (ParamDef::toggle ("showLed", "Signal LED",
        "Round lamp in the corner: dark = no signal, green = signal, red = at or above 0 dBFS.").in ("Dial"));
}

double VuMeterModule::getParamValue (const juce::String& id) const
{
    if (id == "dialStyle")   return (double) dialStyle;
    if (id == "needleStyle") return (double) needleStyle;
    if (id == "showLed")     return showLed ? 1.0 : 0.0;
    return 0.0;
}

void VuMeterModule::setParamValue (const juce::String& id, double v)
{
    if      (id == "dialStyle")   dialStyle   = juce::jlimit (0, 2, juce::roundToInt (v));
    else if (id == "needleStyle") needleStyle = juce::jlimit (0, 2, juce::roundToInt (v));
    else if (id == "showLed")     showLed     = v >= 0.5;
    else return;
    repaint();
}
