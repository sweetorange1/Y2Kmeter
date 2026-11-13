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
    // 指针式：0 半圆 = 180° 上半圆（-90° ~ +90°）；1 复古扇形 = 窄开口小扇形（±fanHalfAngle）。
    //   横向式（2..5）不走角度，位置见 linearXFor。
    const float half = (dialStyle == 1) ? fanHalfAngle
                                        : juce::MathConstants<float>::halfPi;
    return juce::jmap(dialTFor(valueDb), 0.0f, 1.0f, -half, half);
}

float VuMeterModule::linearXFor(float db) const noexcept
{
    return juce::jmap(dialTFor(db), 0.0f, 1.0f, dialGeom.leftX, dialGeom.rightX);
}

float VuMeterModule::linearDbForX(float x) const noexcept
{
    if (dialGeom.rightX <= dialGeom.leftX + 1.0f) return minDisplayDb;
    const float t = juce::jlimit(0.0f, 1.0f, (x - dialGeom.leftX) / (dialGeom.rightX - dialGeom.leftX));
    return juce::jmap(t, minDisplayDb, maxDisplayDb);
}

float VuMeterModule::needleAngleFor(float db) const noexcept
{
    // 横向式没有旋转指针；玩具虚影按"竖直立在游标处"处理（0 = 正上）
    if (isLinearStyle())
        return 0.0f;
    return vuToAngle(db);
}

float VuMeterModule::needleLengthFor(float db) const noexcept
{
    juce::ignoreUnused(db);
    if (isLinearStyle())
        return juce::jmax(8.0f, dialGeom.trackBottom - dialGeom.trackTop);
    if (dialStyle == 1)
        return juce::jmax(8.0f, dialGeom.needleLen);   // 复古扇形：细长针刺进刻度圈
    return juce::jmax(8.0f, dialGeom.radius - 10.0f);  // 半圆：R-10，避开刻度数字
}

// ----------------------------------------------------------
// Toybox（v3.0.0）：迪斯科灯 + 指针虚影（§8.2）；横向式表盘各有专属互动（toyPokeLinear）
// ----------------------------------------------------------
bool VuMeterModule::toyHitTest (juce::Point<int> panelPos, ToyHit& out) const
{
    const auto area = getContentBounds().reduced (6);
    if (! area.contains (panelPos))
        return false;

    const auto ledArea = ledBounds (area);

    // 1 = 迪斯科灯；2 = 指针虚影（指针式）；3 = 横向式专属互动
    out.kind = ledArea.contains (panelPos) ? 1 : (isLinearStyle() ? 3 : 2);
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
    else if (isLinearStyle() && dialGeom.valid)
    {
        toyPokeLinear (hit, ctx);
    }
    else
    {
        // 反馈 A6：碎屏改为指针虚影 —— 几何直接取上一帧 drawDial 的结果
        //   （半圆 / 复古扇形的轴心 + 半径）。
        const float monoDb = currentMonoDbfs();
        const float th = needleAngleFor (monoDb)
                           + juce::MathConstants<float>::halfPi;
        const bool haveGeom = dialGeom.valid && ! isLinearStyle() && dialGeom.radius > 4.0f;
        const juce::Point<float> piv = haveGeom
            ? dialGeom.pivot
            : juce::Point<float> ((float) hit.rect.getCentreX(), (float) hit.rect.getBottom() - 8.0f);
        const float R = haveGeom
            ? dialGeom.radius
            : juce::jmax (4.0f, juce::jmin (hit.rect.getWidth() * 0.5f,
                                            hit.rect.getHeight()) - 14.0f);
        ctx.addModuleActor (makeGhostNeedleActor (piv, R, th));
        ctx.bubble (hit.pos.translated (0.0f, -20.0f),
                    "VU " + juce::String (monoDb, 1) + " dB", ToyRole::Reward);
    }
}

namespace
{
    // VU 表盘刻度：-36 … +3 dBFS，每 1 dB 一根；主刻度带数字。
    //   六种表盘共用同一张表（指针式按角度排布，横向式按 linearXFor 线性排布）。
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

    // 横向式三色段的黄段起点（dBFS）。红段起点仍是 VuMeterModule::warnDbfs（0 dBFS）。
    constexpr float kVuCautionDb = -6.0f;

    // 横向式分段着色：绿（安全）/ 黄（临界）/ 红（过载），全部取主题语义色
    juce::Colour vuZoneColour (float db, float warnDb) noexcept
    {
        if (db >= warnDb)       return PinkXP::danger;
        if (db >= kVuCautionDb) return PinkXP::warn;
        return PinkXP::ok;
    }

    // 经典 VU 下排百分比刻度：p% ↔ 20·log10(p/100) dBFS（100% = 0 dBFS，0% 落到表底）
    float vuPercentToDb (int pct) noexcept
    {
        return pct <= 0 ? -144.0f : 20.0f * std::log10 ((float) pct / 100.0f);
    }

    // 三色分区的 x 边界：表底 / 黄段起点 / 红段起点 / 表顶
    struct VuZoneX { float x0, xCaution, xWarn, x1; };

    // 任意形状按 绿 / 黄 / 红 三段上色：x < xLit 的部分实色，其余淡色（dimAlpha=0 则不画）。
    //   用竖条裁剪实现，所以楔形 / 梯形 / 矩形都能共用。
    void fillZonedShape (juce::Graphics& g, const juce::Path& shape,
                         const VuZoneX& z, float xLit, float dimAlpha)
    {
        const float xs[4] = { z.x0, z.xCaution, z.xWarn, z.x1 };
        const juce::Colour cs[3] = { PinkXP::ok, PinkXP::warn, PinkXP::danger };
        const auto b  = shape.getBounds();
        const int  y0 = (int) std::floor (b.getY()) - 1;
        const int  h  = (int) std::ceil  (b.getHeight()) + 3;

        auto clipFill = [&](float l, float r, juce::Colour c)
        {
            const int il = juce::roundToInt (l), ir = juce::roundToInt (r);
            if (ir <= il) return;
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (juce::Rectangle<int> (il, y0, ir - il, h));
            g.setColour (c);
            g.fillPath (shape);
        };

        for (int i = 0; i < 3; ++i)
        {
            const float a = xs[i], c = xs[i + 1];
            if (c <= a) continue;
            if (dimAlpha > 0.0f) clipFill (juce::jmax (a, xLit), c, cs[i].withAlpha (dimAlpha));
            clipFill (a, juce::jmin (c, xLit), cs[i]);
        }
    }

    // 横向式里的 L / R 小字（轨道左侧留白处）
    void drawVuChannelTag (juce::Graphics& g, const char* tag, float x, float centreY)
    {
        PinkXP::drawCachedText (g, tag, juce::Rectangle<float> (x, centreY - 6.0f, 11.0f, 12.0f),
                                juce::Justification::centredLeft, 9.0f, juce::Font::bold,
                                PinkXP::ink.withAlpha (0.75f), true);
    }

    // 玩具读数："-12.0 dB · 25%"（百分比 = 经典 VU 的 100% = 0 dBFS）
    juce::String vuDbPctText (float db)
    {
        const int pct = juce::roundToInt (100.0f * std::pow (10.0f, db / 20.0f));
        return (db > 0.05f ? "+" : "") + juce::String (db, 1) + " dB  " + juce::String (pct) + "%";
    }
}

// ----------------------------------------------------------
// 横向式的 Toybox 互动：每种表盘一个专属小玩具（坐标取上一帧 drawDial 的几何）
//   2 横向刻度条 = 弹簧游标：幽灵游标被拽到点击处，拖着弹簧带过冲弹回真实电平
//   3 LED 梯柱   = 跑马冲顶：从左逐格点亮到点击那格，顶帽弹飞再落回
//   4 极简游标   = 卷尺：从当前游标量到点击处，标出相差多少 dB
//   5 镜像刻度   = 水滴涟漪：以中脊为水面，上下对称荡开三圈
// ----------------------------------------------------------
void VuMeterModule::toyPokeLinear (const ToyHit& hit, ToyContext& ctx)
{
    const float clickX  = juce::jlimit (dialGeom.leftX, dialGeom.rightX, hit.pos.x);
    const float clickDb = linearDbForX (clickX);
    const float monoDb  = currentMonoDbfs();
    const float restX   = linearXFor (monoDb);          // dialTFor 已夹紧：静音时在左端

    switch (dialStyle)
    {
        case 2:
        {
            ctx.addModuleActor (makeSliderFlickActor (dialGeom.bandTop - 5.0f, dialGeom.bandBottom + 5.0f,
                                                      clickX, restX));
            ctx.bubble ({ clickX, dialGeom.bandTop - 16.0f }, vuDbPctText (clickDb), ToyRole::Reward);
            break;
        }
        case 3:
        {
            // 点击落在哪一行（立体声两行）
            int row = 0;
            if (dialGeom.ladderRows == 2 && hit.pos.y > dialGeom.rowBot[0])
                row = 1;
            const float rTop = dialGeom.rowTop[row], rBot = dialGeom.rowBot[row];
            const bool  hang = dialGeom.rowHang[row];

            const int   nCols = juce::roundToInt (maxDisplayDb - minDisplayDb);
            const float pitch = (dialGeom.rightX - dialGeom.leftX) / (float) nCols;
            const float gap   = (needleStyle == 0) ? pitch * 0.62f      // 与 drawLedLadderDial 同一套格宽
                              : (pitch >= 3.0f ? juce::jlimit (1.0f, 3.0f, pitch * 0.3f) : 0.0f);
            const float colW  = juce::jmax (1.0f, pitch - gap);
            const float hMax  = rBot - rTop;
            const float hMin  = hMax * 0.28f;
            const int   last  = juce::jlimit (0, nCols - 1, (int) std::floor (clickDb - minDisplayDb));

            std::vector<juce::Rectangle<float>> cols;
            std::vector<juce::Colour>           colours;
            cols.reserve ((size_t) last + 1);
            colours.reserve ((size_t) last + 1);
            for (int i = 0; i <= last; ++i)
            {
                const float db0 = minDisplayDb + (float) i;
                const float h   = hMin + (hMax - hMin) * (float) i / (float) juce::jmax (1, nCols - 1);
                cols.push_back ({ linearXFor (db0) + gap * 0.5f, hang ? rTop : rBot - h, colW, h });
                colours.push_back (vuZoneColour (db0, warnDbfs));
            }
            ctx.addModuleActor (makeLedChaseActor (std::move (cols), std::move (colours), hang ? 1.0f : -1.0f));

            const float peakDb = minDisplayDb + (float) last + 1.0f;
            ctx.bubble ({ clickX, rTop - 14.0f },
                        "PEAK " + juce::String (peakDb > 0.0f ? "+" : "") + juce::String ((int) peakDb) + " dB",
                        ToyRole::Reward);
            break;
        }
        case 4:
        {
            const float fromDb = (std::isfinite (monoDb) && monoDb >= minDisplayDb) ? monoDb : minDisplayDb;
            const float delta  = clickDb - fromDb;
            const juce::String label = (std::abs (delta) < 0.05f) ? juce::String ("= 0.0 dB")
                                     : (delta > 0.0f ? "+" : "") + juce::String (delta, 1) + " dB";
            ctx.addModuleActor (makeMeasureTapeActor (dialGeom.bandTop - 20.0f, restX, clickX, label));
            break;
        }
        case 5:
        default:
        {
            const float spineY = (dialGeom.bandTop + dialGeom.bandBottom) * 0.5f;
            ctx.addModuleActor (makeMirrorRippleActor (clickX, spineY, dialGeom.rowTop[0], dialGeom.rowBot[0],
                                                       dialGeom.leftX, dialGeom.rightX));
            ctx.bubble ({ clickX, dialGeom.rowTop[0] - 12.0f }, vuDbPctText (clickDb), ToyRole::Reward);
            break;
        }
    }
}

namespace
{

    // 读数格式：低于表底显示 "--.-"，正值带 "+"
    juce::String vuReadoutText (float db, float floorDb)
    {
        if (! std::isfinite (db) || db < floorDb) return "--.-";
        return (db > 0.05f ? "+" : "") + juce::String (db, 1);
    }

    // 横向式顶部标题行左侧的小字（与右上角 LED 同一行）
    void drawVuHeaderCaption (juce::Graphics& g, juce::Rectangle<int> ledRect,
                              float x, const juce::String& text)
    {
        PinkXP::drawCachedText (g, text,
                                juce::Rectangle<float> (x, (float) ledRect.getY(),
                                                        80.0f, (float) ledRect.getHeight()),
                                juce::Justification::centredLeft, 9.0f, juce::Font::bold,
                                PinkXP::ink.withAlpha (0.6f), true);
    }
}

void VuMeterModule::drawDial(juce::Graphics& g,
                             juce::Rectangle<int> dialArea,
                             juce::Point<float>&  outPivot,
                             float&               outRadius) const
{
    // 每帧重新产出几何；画不出来（模块太小）时 valid 保持 false
    dialGeom = DialGeom{};

    // 六种表盘：0 半圆（经典，下面原样保留）· 1 复古扇形 · 2 横向刻度条
    //           3 LED 梯柱 · 4 极简游标 · 5 镜像刻度
    switch (dialStyle)
    {
        case 1: drawVintageFanDial (g, dialArea, outPivot, outRadius); return;
        case 2: drawStripDial      (g, dialArea); return;
        case 3: drawLedLadderDial  (g, dialArea); return;
        case 4: drawMinimalDial    (g, dialArea); return;
        case 5: drawMirrorDial     (g, dialArea); return;
        default: break;
    }

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
    dialGeom.valid  = true;
}

// ==========================================================
// 复古扇形（样式 1）：窄开口的小扇形老式 VU 表头（±fanHalfAngle ≈ ±47°）
//   · 盘面是一块"扇形窗"：两条直边从轴心放射出去，顶上一段圆弧 —— 剪影是一块
//     披萨角，而不是样式 0 那种底边为直径的完整半圆；
//   · 一条粗弧轨当主刻度轨，刻度线从弧轨外沿向外伸，dB 数字排在最外圈；
//   · 弧轨内侧一条细副刻度弧，挂经典 VU 的百分比小圆点（100% = 0 dBFS）；
//   · 0 dB 之后弧轨 / 副弧 / 刻度 / 数字整段转红（PinkXP::danger）；
//   · 左上角 "VU" 字标 + dBFS 单位（与扇形相交时挪进扇形中部）；
//   · 细长直针从扇形尖端摆出，根部盖半圆盖板（drawFanCap）。
// ==========================================================
void VuMeterModule::drawVintageFanDial(juce::Graphics& g,
                                       juce::Rectangle<int> dialArea,
                                       juce::Point<float>&  outPivot,
                                       float&               outRadius) const
{
    auto inner = dialArea.reduced(10);
    if (inner.getWidth() < 60 || inner.getHeight() < 40) return;

    const juce::Colour inkCol = PinkXP::ink;
    const juce::Colour redCol = PinkXP::danger;
    const float halfA = fanHalfAngle;
    const float sinA  = std::sin(halfA);

    // R = 最外圈 dB 数字的中心半径；扇形窗外沿 Rw = R + 10，盖板约 0.13R。
    //   横向：两端数字中心在 ±R·sinθ 处，再留 14px；
    //   纵向：Rw + 2（顶） + 4（轴心下沿），盖板在轴心之上不额外占高。
    //   整体再乘 0.92 留一圈空气，让"小扇形"居中浮在盘面上。
    const float R = 0.92f * juce::jmin(((float) inner.getWidth() * 0.5f - 14.0f) / sinA,
                                       (float) inner.getHeight() - 16.0f);
    if (R < 34.0f) return;

    const float Rw     = R + 10.0f;
    const float blockH = Rw + 6.0f;
    const float cx = (float) inner.getCentreX();
    const float cy = (float) inner.getY() + ((float) inner.getHeight() - blockH) * 0.5f + Rw + 2.0f;
    const float capR = juce::jlimit(7.0f, 16.0f, R * 0.13f);
    // 两端 dB 数字（半宽 12px）中心落在 ±halfA 上：表窗直边再外扩 ≈16px 的角度，数字不压边
    const float pad  = juce::jmin(0.25f, 16.0f / R);

    // ---- 扇形窗：淡色底 + 墨色轮廓（两条直边 + 外弧 + 盖板处内弧）----
    {
        juce::Path win;
        win.addPieSegment(cx - Rw, cy - Rw, Rw * 2.0f, Rw * 2.0f,
                          -halfA - pad, halfA + pad, (capR + 3.0f) / Rw);
        g.setColour(inkCol.withAlpha(0.05f));
        g.fillPath(win);
        g.setColour(inkCol.withAlpha(0.55f));
        g.strokePath(win, juce::PathStrokeType(1.2f));
    }

    const float railW  = juce::jlimit(3.0f, 6.0f, R * 0.055f);                  // 粗弧轨线宽
    const float rRail  = R - 14.0f - railW * 0.5f;                              // 弧轨中心半径
    const float rTick0 = rRail + railW * 0.5f;                                  // 刻度起点（弧轨外沿）
    const float rSub   = rRail - railW * 0.5f - juce::jlimit(6.0f, 12.0f, R * 0.10f);  // 副刻度弧
    const float aRed   = vuToAngle(warnDbfs);                                   // 0 dB 的角度

    auto arcPath = [cx, cy](float r, float a0, float a1)
    {
        juce::Path p;
        p.addCentredArc(cx, cy, r, r, 0.0f, a0, a1, true);
        return p;
    };
    const juce::PathStrokeType railStroke(railW, juce::PathStrokeType::curved,
                                          juce::PathStrokeType::butt);

    // ---- 粗弧轨：0 dB 前墨色，0 dB 后整段红 ----
    g.setColour(inkCol.withAlpha(0.85f));
    g.strokePath(arcPath(rRail, -halfA, aRed), railStroke);
    g.setColour(redCol);
    g.strokePath(arcPath(rRail, aRed, halfA), railStroke);

    // ---- 主刻度（弧轨外沿向外）+ 最外圈 dB 数字 ----
    for (const auto& t : kVuTicks)
    {
        const float ang = vuToAngle(t.db);
        const float sa = std::sin(ang), ca = std::cos(ang);
        const float r1 = rTick0 + (t.major ? 6.0f : 3.0f);
        const auto  col = (t.db >= warnDbfs) ? redCol : inkCol;

        g.setColour(col.withAlpha(t.major ? 0.95f : 0.6f));
        g.drawLine(cx + sa * rTick0, cy - ca * rTick0, cx + sa * r1, cy - ca * r1,
                   t.major ? 1.5f : 1.0f);

        if (t.label != nullptr)
            PinkXP::drawCachedText(g, t.label,
                                   juce::Rectangle<float>(cx + sa * R - 12.0f, cy - ca * R - 6.0f, 24.0f, 12.0f),
                                   juce::Justification::centred, 9.0f, juce::Font::bold, col, true);
    }

    // ---- 副刻度弧（细）+ 百分比圆点（向内）；100% 之后同样转红 ----
    if (rSub > capR + 10.0f)
    {
        g.setColour(inkCol.withAlpha(0.55f));
        g.strokePath(arcPath(rSub, -halfA, aRed), juce::PathStrokeType(1.0f));
        g.setColour(redCol.withAlpha(0.9f));
        g.strokePath(arcPath(rSub, aRed, halfA), juce::PathStrokeType(1.0f));

        const bool withPctText = R >= 70.0f && rSub - 12.0f > capR + 8.0f;
        for (int p = 0; p <= 100; p += 10)
        {
            const float ang = vuToAngle(vuPercentToDb(p));
            const float sa = std::sin(ang), ca = std::cos(ang);
            const float rd = rSub - 3.5f;
            const float dr = (p % 50 == 0) ? 1.7f : 1.1f;
            g.setColour(inkCol.withAlpha(0.75f));
            g.fillEllipse(cx + sa * rd - dr, cy - ca * rd - dr, dr * 2.0f, dr * 2.0f);

            if (withPctText && p % 50 == 0)
            {
                const float rt = rSub - 12.0f;
                PinkXP::drawCachedText(g, p == 100 ? juce::String("100%") : juce::String(p),
                                       juce::Rectangle<float>(cx + sa * rt - 16.0f, cy - ca * rt - 5.0f, 32.0f, 10.0f),
                                       juce::Justification::centred, 8.0f, juce::Font::plain,
                                       inkCol.withAlpha(0.7f), true);
            }
        }
    }

    // ---- "VU" 字标 + 单位：默认放盘面左上角（窄扇形之外的空角）；
    //   窄高模块里扇形会顶到左上角，这时改放扇形中部（老式表头的经典位置）----
    {
        juce::Rectangle<float> mark((float) inner.getX() + 2.0f, (float) inner.getY(), 44.0f, 31.0f);

        auto insideFan = [&](juce::Point<float> p)
        {
            const float dx = p.x - cx, dy = p.y - cy;
            return std::sqrt(dx * dx + dy * dy) <= Rw + 6.0f
                && std::abs(std::atan2(dx, -dy)) <= halfA + pad + 0.05f;
        };
        if (insideFan(mark.getTopLeft())    || insideFan(mark.getTopRight())
         || insideFan(mark.getBottomLeft()) || insideFan(mark.getBottomRight()))
        {
            const float my = cy - R * 0.46f;
            mark = { cx - 22.0f, my - 15.0f, 44.0f, 31.0f };
            PinkXP::drawCachedText(g, "VU", mark.withHeight(22.0f), juce::Justification::centred,
                                   13.0f, juce::Font::bold, inkCol.withAlpha(0.85f));
            PinkXP::drawCachedText(g, "dBFS", juce::Rectangle<float>(mark.getX(), mark.getY() + 21.0f, 44.0f, 10.0f),
                                   juce::Justification::centred, 8.0f, juce::Font::bold,
                                   inkCol.withAlpha(0.55f), true);
        }
        else
        {
            PinkXP::drawCachedText(g, "VU", mark.withHeight(22.0f), juce::Justification::centredLeft,
                                   13.0f, juce::Font::bold, inkCol.withAlpha(0.85f));
            PinkXP::drawCachedText(g, "dBFS", juce::Rectangle<float>(mark.getX() + 1.0f, mark.getY() + 21.0f, 44.0f, 10.0f),
                                   juce::Justification::centredLeft, 8.0f, juce::Font::bold,
                                   inkCol.withAlpha(0.55f), true);
        }
    }

    outPivot  = { cx, cy };
    outRadius = rRail;
    dialGeom.pivot     = outPivot;
    dialGeom.radius    = rRail;
    dialGeom.needleLen = rTick0 + 4.0f;                        // 针尖刺进刻度圈
    dialGeom.capR      = capR;
    dialGeom.valid     = true;
}

void VuMeterModule::drawFanCap(juce::Graphics& g) const
{
    if (! dialGeom.valid || dialGeom.capR <= 0.0f) return;

    const float r = dialGeom.capR;
    const auto  c = dialGeom.pivot;

    // 半圆盖板：盖住指针根部（老式表头的黑色盖板，这里用主题按钮面色）
    juce::Path cap;
    cap.addPieSegment(c.x - r, c.y - r, r * 2.0f, r * 2.0f,
                      -juce::MathConstants<float>::halfPi,
                      +juce::MathConstants<float>::halfPi, 0.0f);
    g.setColour(PinkXP::btnFace);
    g.fillPath(cap);
    g.setColour(PinkXP::ink);
    g.strokePath(cap, juce::PathStrokeType(1.2f));

    // 盖板上的轴钉
    const float sy = c.y - r * 0.45f;
    g.setColour(PinkXP::pink300);
    g.fillEllipse(c.x - 2.5f, sy - 2.5f, 5.0f, 5.0f);
    g.setColour(PinkXP::ink);
    g.drawEllipse(c.x - 2.5f, sy - 2.5f, 5.0f, 5.0f, 1.0f);
}

// ==========================================================
// 横向式公共：扣掉顶部"标题小字 + LED"一行后的绘制区
// ==========================================================
juce::Rectangle<float> VuMeterModule::linearBody(juce::Rectangle<int> dialArea) const
{
    const auto  led = ledBounds(dialArea);
    const float top = (float) led.getBottom() + 2.0f;
    return { (float) dialArea.getX() + 8.0f, top,
             (float) dialArea.getWidth() - 16.0f,
             juce::jmax(0.0f, (float) dialArea.getBottom() - 8.0f - top) };
}

// ==========================================================
// 横向刻度条（样式 2）
//   · 一根水平粗横杆，按 绿 → 黄 → 红 分段着色（-6 / 0 dBFS 分界）；
//   · 上方一排斜刻度 + 数字：刻度线向两侧外撇，像把扇形表盘"摊平"；
//   · 下方一排小圆点标经典 VU 的百分比（100% = 0 dBFS），数字放不下就跳过；
//   · 0 dB 位一条竖直分界线贯穿刻度 / 横杆 / 圆点三排；
//   · 指示器随 needleStyle 换形态：发丝针 + 骑杆圆珠 / 填充条 + 大三角箭头 / L、R 双轨道滑块。
//   两端仍是 minDisplayDb / maxDisplayDb（dBFS 线性），只借经典横条 VU 的形式。
// ==========================================================
void VuMeterModule::drawStripDial(juce::Graphics& g, juce::Rectangle<int> dialArea) const
{
    const auto body = linearBody(dialArea);
    if (body.getWidth() < 60.0f || body.getHeight() < 16.0f) return;

    const juce::Colour inkCol = PinkXP::ink;
    drawVuHeaderCaption(g, ledBounds(dialArea), body.getX(), "dBFS");

    dialGeom.leftX  = body.getX() + 12.0f;
    dialGeom.rightX = body.getRight() - 12.0f;

    // 高度不够时依次省掉：百分比数字 → 圆点排 → 刻度排
    const float H = body.getHeight();
    const bool withTicks   = H >= 34.0f;
    const bool withDots    = H >= 48.0f;
    const bool withPctText = H >= 62.0f;

    const float numH   = withTicks ? 12.0f : 0.0f;
    const float tickH  = withTicks ? juce::jlimit(6.0f, 14.0f, H * 0.14f) : 0.0f;
    const float gapT   = withTicks ? 6.0f : 4.0f;          // 同时给游标上端伸出量留位置
    const float barH   = (needleStyle == 2) ? juce::jlimit(10.0f, 20.0f, H * 0.22f)   // 立体声：两条轨道
                                            : juce::jlimit( 6.0f, 16.0f, H * 0.16f);
    const float gapD   = withDots ? 7.0f : 4.0f;
    const float dotsH  = withDots ? (4.0f + (withPctText ? 12.0f : 0.0f)) : 0.0f;
    const float blockH = numH + tickH + gapT + barH + gapD + dotsH;

    const float numTop  = body.getY() + juce::jmax(0.0f, (H - blockH) * 0.5f);
    const float tickTop = numTop + numH;
    const float tickBot = tickTop + tickH;
    const float barTop  = tickBot + gapT;
    const float barBot  = barTop + barH;
    const float barMid  = (barTop + barBot) * 0.5f;
    const float dotY    = barBot + gapD + 2.0f;
    const float pctTop  = dotY + 4.0f;

    const float cxs   = (dialGeom.leftX + dialGeom.rightX) * 0.5f;
    const float halfW = juce::jmax(1.0f, (dialGeom.rightX - dialGeom.leftX) * 0.5f);

    // ---- 上排：斜刻度 + 数字（斜率随离中心距离增大，中间近乎竖直）----
    if (withTicks)
    {
        for (const auto& t : kVuTicks)
        {
            const float x    = linearXFor(t.db);
            const float len  = t.major ? tickH : tickH * 0.55f;
            const float lean = (x - cxs) / halfW * len * 0.5f;
            const auto  col  = (t.db >= warnDbfs) ? PinkXP::danger : inkCol;

            g.setColour(col.withAlpha(t.major ? 0.95f : 0.6f));
            g.drawLine(x, tickBot, x + lean, tickBot - len, t.major ? 1.6f : 1.0f);

            if (t.label != nullptr)
                PinkXP::drawCachedText(g, t.label,
                                       juce::Rectangle<float>(x + lean - 12.0f, numTop, 24.0f, 12.0f),
                                       juce::Justification::centred, 9.0f, juce::Font::bold, col, true);
        }
    }

    // ---- 横杆：绿 → 黄 → 红 分段 + 顶边高光 + 墨色描边 ----
    //   指示器形态（needleStyle）决定横杆怎么亮：
    //     细线   = 横杆整根常亮，指示器是一根贯穿上下三排的发丝针 + 骑杆圆珠；
    //     楔形   = 横杆变成填充条（当前电平之前实色、之后淡色）+ 上下一对大三角箭头；
    //     立体声 = 横杆劈成上下两条轨道，L / R 各自填充、各自一个滑块。
    const VuZoneX zx { linearXFor(minDisplayDb), linearXFor(kVuCautionDb),
                       linearXFor(warnDbfs),     linearXFor(maxDisplayDb) };
    const juce::Rectangle<float> barRect(dialGeom.leftX, barTop,
                                         dialGeom.rightX - dialGeom.leftX, barH);
    const float monoDb = currentMonoDbfs();
    const bool  stereo = (needleStyle == 2);
    const bool  wedge  = (needleStyle == 1);
    const float laneGap = 2.0f;
    const float laneH   = (barH - laneGap) * 0.5f;

    if (stereo)
    {
        juce::Path laneL, laneR;
        laneL.addRectangle(barRect.withHeight(laneH));
        laneR.addRectangle(barRect.withTrimmedTop(laneH + laneGap));
        fillZonedShape(g, laneL, zx, linearXFor(displayedL), 0.18f);
        fillZonedShape(g, laneR, zx, linearXFor(displayedR), 0.18f);
        g.setColour(inkCol);
        g.drawRect(barRect.withHeight(laneH), 1.0f);
        g.drawRect(barRect.withTrimmedTop(laneH + laneGap), 1.0f);
        drawVuChannelTag(g, "L", body.getX(), barTop + laneH * 0.5f);
        drawVuChannelTag(g, "R", body.getX(), barBot - laneH * 0.5f);
    }
    else
    {
        juce::Path bar;
        bar.addRectangle(barRect);
        fillZonedShape(g, bar, zx, wedge ? linearXFor(monoDb) : zx.x1, wedge ? 0.18f : 0.0f);
        g.setColour(PinkXP::hl.withAlpha(0.35f));
        g.fillRect(barRect.withHeight(1.0f).translated(0.0f, 1.0f));
        g.setColour(inkCol);
        g.drawRect(barRect, 1.0f);
    }

    // ---- 下排：百分比小圆点（每 10%），20% 一档放数字，从右往左排、挤不下就跳过 ----
    if (withDots)
    {
        float lastLeft = 1.0e9f;
        for (int p = 100; p >= 0; p -= 10)
        {
            const float x  = linearXFor(vuPercentToDb(p));
            const bool  mj = (p % 20 == 0);
            const float dr = mj ? 1.8f : 1.2f;
            g.setColour(inkCol.withAlpha(mj ? 0.85f : 0.55f));
            g.fillEllipse(x - dr, dotY - dr, dr * 2.0f, dr * 2.0f);

            if (withPctText && mj)
            {
                const juce::String s = (p == 100) ? juce::String("100%") : juce::String(p);
                const float w = 7.0f * (float) s.length() + 6.0f;
                if (x + w * 0.5f <= lastLeft - 2.0f)
                {
                    PinkXP::drawCachedText(g, s, juce::Rectangle<float>(x - w * 0.5f, pctTop, w, 10.0f),
                                           juce::Justification::centred, 8.0f, juce::Font::plain,
                                           inkCol.withAlpha(0.7f), true);
                    lastLeft = x - w * 0.5f;
                }
            }
        }
    }

    // ---- 0 dB 竖直分界线（贯穿三排）----
    {
        const float x0 = linearXFor(warnDbfs);
        const float yA = withTicks ? tickTop - 1.0f : barTop - 3.0f;
        const float yB = withDots  ? dotY + 3.0f    : barBot + 3.0f;
        g.setColour(inkCol.withAlpha(0.9f));
        g.drawLine(x0, yA, x0, yB, 1.2f);
    }

    // ---- 指示器 ----
    const float needleTop = withTicks ? tickTop + 2.0f : barTop - 5.0f;
    const float needleBot = withDots  ? dotY + 4.0f    : barBot + 5.0f;
    float curBot = barBot + 4.0f;          // 指示器下沿（玩具虚影 / 互动定位用）

    if (stereo)
    {
        // 每条轨道一个滑块：向轨道外侧多伸 4px，L 朝上、R 朝下
        auto slider = [&](float db, juce::Colour c, float y0, float y1)
        {
            const float x = linearXFor(db);
            const juce::Rectangle<float> r(x - 3.0f, y0, 6.0f, y1 - y0);
            g.setColour(c);
            g.fillRect(r);
            g.setColour(inkCol);
            g.drawRect(r, 1.0f);
        };
        slider(displayedL, PinkXP::pink500, barTop - 5.0f, barTop + laneH);
        slider(displayedR, PinkXP::pink700, barBot - laneH, barBot + 5.0f);
        curBot = barBot + 5.0f;
    }
    else if (wedge)
    {
        // 一对大三角箭头从上下夹住填充条的前沿，中间一根 3px 墨色前沿线
        const float x = linearXFor(monoDb);
        const float s = juce::jlimit(5.0f, 9.0f, barH * 0.7f);
        juce::Path p;
        p.addTriangle(x - s, barTop - s * 1.4f - 1.0f, x + s, barTop - s * 1.4f - 1.0f, x, barTop - 1.0f);
        p.addTriangle(x - s, barBot + s * 1.4f + 1.0f, x + s, barBot + s * 1.4f + 1.0f, x, barBot + 1.0f);
        g.setColour(inkCol);
        g.fillRect(juce::Rectangle<float>(x - 1.5f, barTop, 3.0f, barH));
        g.setColour(PinkXP::pink600);
        g.fillPath(p);
        g.setColour(inkCol);
        g.strokePath(p, juce::PathStrokeType(1.0f));
        curBot = barBot + s * 1.4f + 1.0f;
    }
    else
    {
        // 发丝针：贯穿刻度排 / 横杆 / 圆点排，骑杆处一颗圆珠
        const float x  = linearXFor(monoDb);
        const float kr = juce::jlimit(3.0f, 6.0f, barH * 0.45f);
        g.setColour(PinkXP::pink600);
        g.fillRect(juce::Rectangle<float>(x - 0.75f, needleTop, 1.5f, needleBot - needleTop));
        g.fillEllipse(x - kr, barMid - kr, kr * 2.0f, kr * 2.0f);
        g.setColour(inkCol);
        g.drawEllipse(x - kr, barMid - kr, kr * 2.0f, kr * 2.0f, 1.0f);
        curBot = needleBot;
    }

    dialGeom.trackTop    = body.getY();
    dialGeom.trackBottom = curBot;
    dialGeom.radius      = curBot - body.getY();
    dialGeom.bandTop     = barTop;
    dialGeom.bandBottom  = barBot;
    dialGeom.valid       = true;
}

// ==========================================================
// LED 梯柱（样式 3）
//   1 dB 一格的 LED 柱排成一行，柱高从左到右逐格加高（阶梯剪影，越响越高），
//   按电平点亮；绿 / 黄 / 红分段（-6 / 0 dBFS），最高一格提亮；未点亮的格子
//   留淡色底 + 段色细框，静音时也能看出整条梯子和三色分区。
//   指示器形态（needleStyle）：
//     细线   = 一排细针状 LED（格宽只有间距的 ~38%），像梳齿；
//     楔形   = 整条梯子合成一块实心楔形（格缝只留每 3 dB 一道细缝），按电平填充；
//     立体声 = 上下镜像两排：L 从中线往上长、R 从中线往下倒挂。
// ==========================================================
void VuMeterModule::drawLedLadderDial(juce::Graphics& g, juce::Rectangle<int> dialArea) const
{
    const auto body = linearBody(dialArea);
    if (body.getWidth() < 60.0f || body.getHeight() < 16.0f) return;

    const juce::Colour inkCol = PinkXP::ink;
    const bool stereo = (needleStyle == 2);
    const bool wedge  = (needleStyle == 1);
    drawVuHeaderCaption(g, ledBounds(dialArea), body.getX(), "dBFS");

    dialGeom.leftX  = body.getX() + 12.0f;
    dialGeom.rightX = body.getRight() - 12.0f;

    const bool  labels = body.getHeight() >= 34.0f;
    const float colTop = body.getY();
    const float colBot = body.getBottom() - (labels ? 16.0f : 0.0f);
    if (colBot - colTop < 8.0f) return;

    // 每格 = 1 dB，格子边界正好落在 linearXFor(整数 dB) 上，格缝居中于边界
    const int   nCols = juce::roundToInt(maxDisplayDb - minDisplayDb);
    const float pitch = (dialGeom.rightX - dialGeom.leftX) / (float) nCols;
    const float gap   = (needleStyle == 0) ? pitch * 0.62f                       // 细针：格缝比格子宽
                      : (pitch >= 3.0f ? juce::jlimit(1.0f, 3.0f, pitch * 0.3f) : 0.0f);
    const float colW  = juce::jmax(1.0f, pitch - gap);

    // ---- 0 dB 分界线：先画，从格缝里透出来 ----
    {
        const float x0 = linearXFor(warnDbfs);
        g.setColour(inkCol.withAlpha(0.55f));
        g.drawLine(x0, colTop, x0, colBot + (labels ? 3.0f : 0.0f), 1.0f);
    }

    // 逐格画一排；hang = 从 rowTop 往下长（立体声 R 行）
    auto ladder = [&](float rowTop, float rowBot, float level, bool hang)
    {
        const float hMax = rowBot - rowTop;
        const float hMin = hMax * 0.28f;
        const int   topLit = (std::isfinite(level) && level >= minDisplayDb)
                               ? juce::jmin(nCols - 1, (int) std::floor(level - minDisplayDb))
                               : -1;

        for (int i = 0; i < nCols; ++i)
        {
            const float db0 = minDisplayDb + (float) i;
            const float h   = hMin + (hMax - hMin) * (float) i / (float) juce::jmax(1, nCols - 1);
            const juce::Rectangle<float> r(linearXFor(db0) + gap * 0.5f, hang ? rowTop : rowBot - h, colW, h);
            const auto zc  = vuZoneColour(db0, warnDbfs);

            if (i <= topLit)
            {
                g.setColour(i == topLit ? zc.brighter(0.25f) : zc);
                g.fillRect(r);
            }
            else
            {
                g.setColour(inkCol.withAlpha(0.07f));
                g.fillRect(r);
                if (colW >= 3.0f)
                {
                    g.setColour(zc.withAlpha(0.35f));
                    g.drawRect(r, 1.0f);
                }
            }
        }
    };

    dialGeom.ladderRows = 1;
    dialGeom.rowTop[0]  = colTop;
    dialGeom.rowBot[0]  = colBot;
    dialGeom.rowHang[0] = false;

    if (stereo)
    {
        const float mid = (colTop + colBot) * 0.5f;
        ladder(colTop,     mid - 1.5f, displayedL, false);
        ladder(mid + 1.5f, colBot,     displayedR, true);

        // 中线：两排共用的"地面"
        g.setColour(inkCol.withAlpha(0.6f));
        g.fillRect(juce::Rectangle<float>(dialGeom.leftX, mid - 0.5f, dialGeom.rightX - dialGeom.leftX, 1.0f));

        // 行首 L / R（阶梯最矮那端靠中线，上下两角都空着）
        drawVuChannelTag(g, "L", body.getX(), mid - 8.0f);
        drawVuChannelTag(g, "R", body.getX(), mid + 8.0f);

        dialGeom.ladderRows = 2;
        dialGeom.rowTop[0] = colTop;      dialGeom.rowBot[0] = mid - 1.5f; dialGeom.rowHang[0] = false;
        dialGeom.rowTop[1] = mid + 1.5f;  dialGeom.rowBot[1] = colBot;     dialGeom.rowHang[1] = true;
    }
    else if (wedge)
    {
        // 实心楔形：左低右高的梯形剪影，按电平三色填充
        const float hMax = colBot - colTop;
        const float hMin = hMax * 0.28f;
        juce::Path w;
        w.startNewSubPath(dialGeom.leftX,  colBot);
        w.lineTo        (dialGeom.leftX,  colBot - hMin);
        w.lineTo        (dialGeom.rightX, colTop);
        w.lineTo        (dialGeom.rightX, colBot);
        w.closeSubPath();

        const VuZoneX zx { linearXFor(minDisplayDb), linearXFor(kVuCautionDb),
                           linearXFor(warnDbfs),     linearXFor(maxDisplayDb) };
        const float monoDb = currentMonoDbfs();
        const float xLit = (std::isfinite(monoDb) && monoDb >= minDisplayDb) ? linearXFor(monoDb)
                                                                             : zx.x0;
        fillZonedShape(g, w, zx, xLit, 0.16f);

        // 每 3 dB 一道细缝（底色），保留"LED 拼起来"的颗粒感
        g.setColour(PinkXP::content);
        for (int i = 3; i < nCols; i += 3)
        {
            const float x = linearXFor(minDisplayDb + (float) i);
            g.fillRect(juce::Rectangle<float>(x - 0.5f, colTop, 1.0f, colBot - colTop));
        }

        // 前沿：一根墨色竖线 + 顶端小三角，标出当前电平落在楔形上的高度
        if (xLit > zx.x0)
        {
            const float tTop = colBot - (hMin + (hMax - hMin) * (xLit - zx.x0) / juce::jmax(1.0f, zx.x1 - zx.x0));
            const float ty   = juce::jmax(tTop, colTop + 6.0f);   // 右端最高处别顶进 LED 那一行
            g.setColour(inkCol);
            g.fillRect(juce::Rectangle<float>(xLit - 1.0f, tTop, 2.0f, colBot - tTop));
            juce::Path tri;
            tri.addTriangle(xLit - 4.0f, ty - 6.0f, xLit + 4.0f, ty - 6.0f, xLit, ty);
            g.fillPath(tri);
        }

        g.setColour(inkCol.withAlpha(0.6f));
        g.strokePath(w, juce::PathStrokeType(1.0f));
    }
    else
    {
        ladder(colTop, colBot, currentMonoDbfs(), false);
    }

    // ---- 底排刻度数字 ----
    if (labels)
    {
        for (const auto& t : kVuTicks)
        {
            if (t.label == nullptr) continue;
            const float x   = linearXFor(t.db);
            const auto  col = (t.db >= warnDbfs) ? PinkXP::danger : inkCol;
            g.setColour(col.withAlpha(0.8f));
            g.drawLine(x, colBot + 1.0f, x, colBot + 4.0f, 1.0f);
            PinkXP::drawCachedText(g, t.label, juce::Rectangle<float>(x - 12.0f, colBot + 4.0f, 24.0f, 12.0f),
                                   juce::Justification::centred, 9.0f, juce::Font::bold, col, true);
        }
    }

    dialGeom.trackTop    = colTop;
    dialGeom.trackBottom = colBot;
    dialGeom.radius      = colBot - colTop;
    dialGeom.bandTop     = colTop;
    dialGeom.bandBottom  = colBot;
    dialGeom.valid       = true;
}

// ==========================================================
// 极简游标（样式 4）
//   大号数字读数占上部；下方只有一根发丝刻度线 + 5 个稀疏刻度（-36 / -20 / -10 / 0 / +3）。
//   没有表盘、没有色块，信息只剩"数字 + 位置"。指示器形态（needleStyle）：
//     细线   = 2px 进度线 + 竖发丝游标（顶端小圆点）；
//     楔形   = 进度变成一块从左端尖起、越往右越厚的实心楔形（0 dB 后转红）+ 大三角游标；
//     立体声 = 两根独立的发丝线（上 L 下 R）各自进度 + 游标，读数分左右两格。
// ==========================================================
void VuMeterModule::drawMinimalDial(juce::Graphics& g, juce::Rectangle<int> dialArea) const
{
    const auto body = linearBody(dialArea);
    if (body.getWidth() < 60.0f || body.getHeight() < 20.0f) return;

    const juce::Colour inkCol = PinkXP::ink;
    const bool stereo = (needleStyle == 2);
    const bool wedge  = (needleStyle == 1);

    dialGeom.leftX  = body.getX() + 12.0f;
    dialGeom.rightX = body.getRight() - 12.0f;

    // 线下预留：刻度短线 + 数字 10px；线上预留：游标 / 楔形 / 立体声第二根线
    const bool  labels = body.getHeight() >= 40.0f;
    const float lineY  = std::floor(body.getBottom() - (labels ? 23.0f : 11.0f));
    const float lineL  = lineY - 14.0f;                       // 立体声：L 线（R 线 = lineY）
    const float aboveH = stereo ? 26.0f : (wedge ? 22.0f : 12.0f);
    const float monoDb = currentMonoDbfs();
    const float x0     = linearXFor(warnDbfs);

    // ---- 大号读数（动态文本，直接 drawText，不进静态文本缓存）----
    const juce::Rectangle<float> readRect(body.getX(), body.getY(),
                                          body.getWidth(), lineY - aboveH - body.getY());
    if (readRect.getHeight() >= 12.0f)
    {
        const bool  withUnit = readRect.getHeight() >= 34.0f;
        const auto  numRect  = withUnit ? readRect.withTrimmedBottom(11.0f) : readRect;
        const int   cols     = stereo ? 2 : 1;
        const float cellW    = numRect.getWidth() / (float) cols;
        // 字高先按高度取，再按"5 个字符"的宽度收缩，窄模块不裁字
        const float fh = juce::jlimit(9.0f, 48.0f,
                                      juce::jmin(numRect.getHeight() * 0.8f, cellW / 5.5f));
        g.setFont(PinkXP::getAxisFont(fh, juce::Font::bold));

        for (int c = 0; c < cols; ++c)
        {
            const float v    = stereo ? (c == 0 ? displayedL : displayedR) : monoDb;
            const auto  cell = numRect.withX(numRect.getX() + cellW * (float) c).withWidth(cellW);
            g.setColour(v >= warnDbfs ? PinkXP::danger : inkCol);
            g.drawText(vuReadoutText(v, minDisplayDb), cell, juce::Justification::centred, false);

            if (withUnit)
            {
                const juce::String unit = stereo ? (c == 0 ? "L dBFS" : "R dBFS") : "dBFS";
                PinkXP::drawCachedText(g, unit,
                                       juce::Rectangle<float>(cell.getX(), numRect.getBottom(), cellW, 10.0f),
                                       juce::Justification::centred, 8.0f, juce::Font::plain,
                                       inkCol.withAlpha(0.55f), true);
            }
        }
    }

    // ---- 发丝刻度线 + 稀疏刻度（刻度 / 数字挂在最下面那根线下）----
    const float scaleW = dialGeom.rightX - dialGeom.leftX;
    g.setColour(inkCol.withAlpha(0.35f));
    g.fillRect(juce::Rectangle<float>(dialGeom.leftX, lineY, scaleW, 1.0f));
    if (stereo)
    {
        g.fillRect(juce::Rectangle<float>(dialGeom.leftX, lineL, scaleW, 1.0f));
        drawVuChannelTag(g, "L", body.getX(), lineL - 3.0f);
        drawVuChannelTag(g, "R", body.getX(), lineY - 3.0f);
    }

    struct Mark { float db; const char* label; };
    static const Mark marks[] = { { -36.0f, "-36" }, { -20.0f, "-20" }, { -10.0f, "-10" },
                                  {   0.0f, "0"   }, {   3.0f, "+3"  } };
    for (const auto& m : marks)
    {
        const float x   = linearXFor(m.db);
        const auto  col = ((m.db >= warnDbfs) ? PinkXP::danger : inkCol).withAlpha(0.6f);
        g.setColour(col);
        g.fillRect(juce::Rectangle<float>(x - 0.5f, lineY + 1.0f, 1.0f, 3.0f));
        if (labels)
            PinkXP::drawCachedText(g, m.label, juce::Rectangle<float>(x - 12.0f, lineY + 12.0f, 24.0f, 10.0f),
                                   juce::Justification::centred, 8.0f, juce::Font::plain, col, true);
    }

    // 有效电平 → x；低于表底返回 -1（不画进度）
    auto litX = [&](float db) -> float
    {
        return (std::isfinite(db) && db >= minDisplayDb) ? linearXFor(db) : -1.0f;
    };

    // ---- 细线进度：左端 → 当前电平，紧贴线上 2px，0 dB 之后转红 ----
    auto progress = [&](float db, float y, juce::Colour c)
    {
        const float x = litX(db);
        if (x < 0.0f) return;
        g.setColour(c);
        g.fillRect(juce::Rectangle<float>(dialGeom.leftX, y - 2.0f, juce::jmin(x, x0) - dialGeom.leftX, 2.0f));
        if (x > x0)
        {
            g.setColour(PinkXP::danger);
            g.fillRect(juce::Rectangle<float>(x0, y - 2.0f, x - x0, 2.0f));
        }
    };

    // ---- 发丝游标：线上竖线 + 顶端小圆点 ----
    auto hairCursor = [&](float db, float y, juce::Colour c)
    {
        const float x = linearXFor(db);
        g.setColour(c);
        g.fillRect(juce::Rectangle<float>(x - 0.75f, y - 9.0f, 1.5f, 9.0f));
        g.fillEllipse(x - 2.5f, y - 12.0f, 5.0f, 5.0f);
    };

    if (stereo)
    {
        progress(displayedL, lineL, PinkXP::pink500);
        progress(displayedR, lineY, PinkXP::pink700);
        hairCursor(displayedL, lineL, PinkXP::pink500);
        hairCursor(displayedR, lineY, PinkXP::pink700);
    }
    else if (wedge)
    {
        // 楔形：淡色"满量程"剪影 + 实心楔形填到当前电平（越响越厚）
        const float hMax = 12.0f;
        auto wedgeH = [&](float x) { return 1.0f + (hMax - 1.0f) * (x - dialGeom.leftX) / juce::jmax(1.0f, scaleW); };

        juce::Path ghost;
        ghost.addTriangle(dialGeom.leftX, lineY, dialGeom.rightX, lineY - hMax, dialGeom.rightX, lineY);
        g.setColour(inkCol.withAlpha(0.08f));
        g.fillPath(ghost);

        const float x = litX(monoDb);
        if (x > dialGeom.leftX)
        {
            juce::Path w;
            w.startNewSubPath(dialGeom.leftX, lineY);
            w.lineTo(x, lineY - wedgeH(x));
            w.lineTo(x, lineY);
            w.closeSubPath();
            {
                juce::Graphics::ScopedSaveState ss(g);
                g.reduceClipRegion(juce::Rectangle<int>((int) dialGeom.leftX - 1, (int) (lineY - hMax) - 2,
                                                        juce::roundToInt(x0) - (int) dialGeom.leftX + 1, (int) hMax + 4));
                g.setColour(PinkXP::pink600);
                g.fillPath(w);
            }
            if (x > x0)
            {
                juce::Graphics::ScopedSaveState ss(g);
                g.reduceClipRegion(juce::Rectangle<int>(juce::roundToInt(x0), (int) (lineY - hMax) - 2,
                                                        juce::roundToInt(x) - juce::roundToInt(x0) + 1, (int) hMax + 4));
                g.setColour(PinkXP::danger);
                g.fillPath(w);
            }
        }

        // 大三角游标：尖端落在楔形顶边上
        const float cx  = linearXFor(monoDb);
        const float tip = lineY - (x > dialGeom.leftX ? wedgeH(cx) : 1.0f) - 1.0f;
        juce::Path tri;
        tri.addTriangle(cx - 6.0f, tip - 9.0f, cx + 6.0f, tip - 9.0f, cx, tip);
        g.setColour(PinkXP::pink600);
        g.fillPath(tri);
        g.setColour(inkCol);
        g.strokePath(tri, juce::PathStrokeType(1.0f));
    }
    else
    {
        progress(monoDb, lineY, PinkXP::pink600);
        hairCursor(monoDb, lineY, PinkXP::pink600);
    }

    dialGeom.trackTop    = body.getY();
    dialGeom.trackBottom = lineY;
    dialGeom.radius      = lineY - body.getY();
    dialGeom.bandTop     = stereo ? lineL : lineY;
    dialGeom.bandBottom  = lineY;
    dialGeom.valid       = true;
}

// ==========================================================
// 镜像刻度（样式 5）
//   dB 数字沿中脊排一行，上下两排刻度以中脊对称（上排朝上、下排朝下），再往外是
//   上下两条填充条：未点亮部分以淡色显示绿 / 黄 / 红分段，点亮到当前电平。
//   Mono = 上下同读总电平（完全镜像）· Stereo = 上 L 下 R；条子形状见下方 barShape。
// ==========================================================
void VuMeterModule::drawMirrorDial(juce::Graphics& g, juce::Rectangle<int> dialArea) const
{
    const auto body = linearBody(dialArea);
    if (body.getWidth() < 60.0f || body.getHeight() < 24.0f) return;

    const juce::Colour inkCol = PinkXP::ink;
    const bool stereo = (needleStyle == 2);
    drawVuHeaderCaption(g, ledBounds(dialArea), body.getX(), "dBFS");

    dialGeom.leftX  = body.getX() + 14.0f;      // 左侧给 Stereo 的 L / R 字母留位置
    dialGeom.rightX = body.getRight() - 12.0f;

    const float H      = body.getHeight();
    const bool  labels = H >= 40.0f;
    const float spineH = labels ? 12.0f : 2.0f;
    const float tickH  = juce::jlimit(3.0f, 9.0f, H * 0.10f);
    const float gap    = 3.0f;
    const float barH   = juce::jlimit(4.0f, 22.0f, (H - spineH - 2.0f * (tickH + gap)) * 0.5f);
    const float blockH = spineH + 2.0f * (tickH + gap + barH);

    const float upTop    = body.getY() + juce::jmax(0.0f, (H - blockH) * 0.5f);
    const float upBot    = upTop + barH;
    const float spineTop = upBot + gap + tickH;
    const float spineBot = spineTop + spineH;
    const float loTop    = spineBot + tickH + gap;
    const float loBot    = loTop + barH;
    const float scaleW   = dialGeom.rightX - dialGeom.leftX;

    // ---- 中脊：两条基线 + 对称刻度 + 数字 ----
    g.setColour(inkCol.withAlpha(0.5f));
    g.fillRect(juce::Rectangle<float>(dialGeom.leftX, spineTop - 1.0f, scaleW, 1.0f));
    g.fillRect(juce::Rectangle<float>(dialGeom.leftX, spineBot,        scaleW, 1.0f));
    for (const auto& t : kVuTicks)
    {
        const float x   = linearXFor(t.db);
        const float len = t.major ? tickH : tickH * 0.5f;
        const float w   = t.major ? 1.4f : 1.0f;
        const auto  col = (t.db >= warnDbfs) ? PinkXP::danger : inkCol;
        g.setColour(col.withAlpha(t.major ? 0.9f : 0.55f));
        g.drawLine(x, spineTop - 1.0f, x, spineTop - 1.0f - len, w);
        g.drawLine(x, spineBot + 1.0f, x, spineBot + 1.0f + len, w);

        if (labels && t.label != nullptr)
            PinkXP::drawCachedText(g, t.label, juce::Rectangle<float>(x - 12.0f, spineTop, 24.0f, spineH),
                                   juce::Justification::centred, 9.0f, juce::Font::bold, col, true);
    }

    // ---- 上下填充条：形状随指示器形态（needleStyle）变化 ----
    //   细线   = 两条贴着刻度排的窄条 + 一根上下贯穿中脊的发丝针（两端圆珠）；
    //   楔形   = 两条条子都变成楔形（靠中脊的一边是直线，外沿从左往右张开），
    //            上下合起来像一只横放的喇叭口；前沿墨线 + 三角箭头直指中脊；
    //   立体声 = 满高矩形条，上 L 下 R，各自一块声道色的粗端帽。
    const VuZoneX zx { linearXFor(minDisplayDb), linearXFor(kVuCautionDb),
                       linearXFor(warnDbfs),     linearXFor(maxDisplayDb) };
    const float monoDb = currentMonoDbfs();
    auto litX = [&](float db) -> float
    {
        return (std::isfinite(db) && db >= minDisplayDb) ? linearXFor(db) : dialGeom.leftX;
    };

    // spineBelow = 该条在中脊上方（内沿在下）
    auto barShape = [&](float top, bool spineBelow) -> juce::Path
    {
        juce::Path p;
        const float inner = spineBelow ? top + barH : top;           // 靠中脊的一边
        const float outer = spineBelow ? top        : top + barH;
        if (needleStyle == 1)
        {
            const float dir = spineBelow ? 1.0f : -1.0f;             // outer → inner 的方向
            p.startNewSubPath(dialGeom.leftX, inner);
            p.lineTo(dialGeom.leftX,  inner - dir * barH * 0.2f);
            p.lineTo(dialGeom.rightX, outer);
            p.lineTo(dialGeom.rightX, inner);
            p.closeSubPath();
        }
        else if (needleStyle == 0)
        {
            const float h = juce::jmax(3.0f, barH * 0.38f);
            p.addRectangle(dialGeom.leftX, spineBelow ? inner - h : inner, scaleW, h);
        }
        else
        {
            p.addRectangle(dialGeom.leftX, top, scaleW, barH);
        }
        return p;
    };

    auto bar = [&](float top, float level, bool spineBelow, juce::Colour capCol)
    {
        const auto  shape = barShape(top, spineBelow);
        const float xl    = litX(level);
        fillZonedShape(g, shape, zx, xl, 0.14f);
        g.setColour(inkCol.withAlpha(0.6f));
        g.strokePath(shape, juce::PathStrokeType(1.0f));

        if (xl <= dialGeom.leftX) return;
        if (needleStyle == 1)
        {
            // 前沿墨线（楔形在该处的厚度）+ 三角箭头：底边贴条的内沿，尖端落中脊基线
            const float inner = spineBelow ? top + barH : top;
            const float t     = (xl - dialGeom.leftX) / juce::jmax(1.0f, scaleW);
            const float th    = barH * (0.2f + 0.8f * t);
            g.setColour(inkCol);
            g.fillRect(juce::Rectangle<float>(xl - 1.0f, spineBelow ? inner - th : inner, 2.0f, th));

            const float tip = spineBelow ? spineTop - 1.0f : spineBot + 1.0f;
            juce::Path p;
            p.addTriangle(xl - 4.5f, inner, xl + 4.5f, inner, xl, tip);
            g.setColour(capCol);
            g.fillPath(p);
            g.setColour(inkCol);
            g.strokePath(p, juce::PathStrokeType(1.0f));
        }
        else if (needleStyle == 2)
        {
            const juce::Rectangle<float> cap(xl - 2.5f, top - 2.0f, 5.0f, barH + 4.0f);
            g.setColour(capCol);
            g.fillRect(cap);
            g.setColour(inkCol);
            g.drawRect(cap, 1.0f);
        }
    };

    bar(upTop, stereo ? displayedL : monoDb, true,  stereo ? PinkXP::pink500 : PinkXP::pink600);
    bar(loTop, stereo ? displayedR : monoDb, false, stereo ? PinkXP::pink700 : PinkXP::pink600);

    if (needleStyle == 0)
    {
        // 发丝针：上下贯穿两条窄条和中脊，两端各一颗圆珠
        const float x = litX(monoDb);
        g.setColour(PinkXP::pink600);
        g.fillRect(juce::Rectangle<float>(x - 0.75f, upTop, 1.5f, loBot - upTop));
        g.fillEllipse(x - 3.0f, upTop - 3.0f, 6.0f, 6.0f);
        g.fillEllipse(x - 3.0f, loBot - 3.0f, 6.0f, 6.0f);
        g.setColour(inkCol);
        g.drawEllipse(x - 3.0f, upTop - 3.0f, 6.0f, 6.0f, 1.0f);
        g.drawEllipse(x - 3.0f, loBot - 3.0f, 6.0f, 6.0f, 1.0f);
    }

    if (stereo)
    {
        drawVuChannelTag(g, "L", body.getX(), upTop + barH * 0.5f);
        drawVuChannelTag(g, "R", body.getX(), loTop + barH * 0.5f);
    }

    // ---- 0 dB 竖直分界线（上下贯穿）----
    {
        const float x0 = linearXFor(warnDbfs);
        g.setColour(inkCol.withAlpha(0.85f));
        g.drawLine(x0, upTop - 2.0f, x0, loBot + 2.0f, 1.0f);
    }

    dialGeom.trackTop    = body.getY();
    dialGeom.trackBottom = loBot;
    dialGeom.radius      = loBot - body.getY();
    dialGeom.bandTop     = spineTop;
    dialGeom.bandBottom  = spineBot;
    dialGeom.rowTop[0]   = upTop;      // 镜像：上条 / 下条外沿（玩具涟漪的上下范围）
    dialGeom.rowBot[0]   = loBot;
    dialGeom.valid       = true;
}

void VuMeterModule::drawNeedle(juce::Graphics& g,
                               float              valueDb,
                               juce::Colour       colour,
                               float              thickness,
                               bool               wedge) const
{
    // 几何由 drawDial 按当前样式缓存（只有指针式 0 半圆 / 1 复古扇形会走到这里）
    const float ang  = needleAngleFor(valueDb);
    const float sinA = std::sin(ang);
    const float cosA = std::cos(ang);
    const auto  pivot = dialGeom.pivot;

    // 指针尖端：长度已按样式算好（半圆 = R-10 避开数字；复古扇形刺进刻度圈）
    const float tipR = needleLengthFor(valueDb);
    // 指针根部：半圆在圆心后方伸出一点（配重效果）；复古扇形根部藏在盖板下，不留尾巴
    const float tailR = (dialStyle == 1) ? 0.0f : 8.0f;

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

    // 画表盘 (获取圆心 + 半径)。横向式（2..5）的游标 / 灯格 / 填充条在各自
    //   draw*Dial 里一并画完，这里只给指针式（0 半圆 / 1 复古扇形）画指针。
    juce::Point<float> pivot;
    float radius = 0.0f;
    drawDial(g, dialArea, pivot, radius);

    if (! isLinearStyle() && radius > 10.0f)
    {
        // 指针和 LED 共用的"单声道总电平"（dBFS）
        const float monoDb = currentMonoDbfs();
        // 指针式里只有半圆保持旧线宽；复古扇形的三种指针形态拉开差异：
        //   细线 = 1.2px 发丝长针；楔形 = 根部宽约 1.4 个盖板半径的矛形针（根部藏进盖板）；
        //   立体声 = 两根细针 + 针身上各挂一枚 L / R 圆标（错开半径，重叠时也能分清）。
        const bool  fan    = (dialStyle == 1);

        if (needleStyle == 2)
        {
            // Stereo：L / R 各一根细指针（取已平滑的 displayedL/R）
            drawNeedle(g, displayedL, PinkXP::pink500, fan ? 1.4f : 1.6f);
            drawNeedle(g, displayedR, PinkXP::pink700, fan ? 1.4f : 1.6f);

            if (fan)
            {
                auto tag = [&](float db, juce::Colour c, const char* text, float frac)
                {
                    const float ang = needleAngleFor(db);
                    const float r   = needleLengthFor(db) * frac;
                    const juce::Point<float> p(dialGeom.pivot.x + std::sin(ang) * r,
                                               dialGeom.pivot.y - std::cos(ang) * r);
                    const float tr = 6.0f;
                    g.setColour(c);
                    g.fillEllipse(p.x - tr, p.y - tr, tr * 2.0f, tr * 2.0f);
                    g.setColour(PinkXP::ink);
                    g.drawEllipse(p.x - tr, p.y - tr, tr * 2.0f, tr * 2.0f, 1.0f);
                    PinkXP::drawCachedText(g, text, juce::Rectangle<float>(p.x - tr, p.y - tr, tr * 2.0f, tr * 2.0f),
                                           juce::Justification::centred, 8.0f, juce::Font::bold,
                                           PinkXP::readableInkOn(c), true);
                };
                tag(displayedL, PinkXP::pink500, "L", 0.52f);
                tag(displayedR, PinkXP::pink700, "R", 0.70f);
            }
        }
        else
        {
            // 单一指针（总电平），主题粉色系 pink600
            const float th = fan ? (needleStyle == 1 ? dialGeom.capR * 0.6f : 1.2f) : 2.2f;
            drawNeedle(g, monoDb, PinkXP::pink600, th, needleStyle == 1);
        }

        if (fan)
            drawFanCap(g);
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
    //   六种样式统一放右上角：指针式（0 / 1）的盘面顶角本来就空；横向式（2..5）
    //   由 linearBody 在顶部为这一行让位，刻度数字不会压到灯。
    return { area.getRight() - ledSz - 6, area.getY() + 6, ledSz, ledSz };
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
    // 缺键 = 保持当前值（构造默认 = 旧 VU 外观）。
    //   dialStyle 1 / 2 旧值（弧形 / 梯形）已被新样式取代：索引原地复用，读档后直接显示
    //   复古扇形 / 横向刻度条；0 半圆不变。
    if (state.hasProperty ("dialStyle"))   dialStyle   = juce::jlimit (0, numDialStyles - 1, (int) state.getProperty ("dialStyle"));
    if (state.hasProperty ("needleStyle")) needleStyle = juce::jlimit (0, 2, (int) state.getProperty ("needleStyle"));
    if (state.hasProperty ("showLed"))     showLed     = (bool) state.getProperty ("showLed");
    refreshControlPanel();
    repaint();
}

void VuMeterModule::buildParamDefs (std::vector<ParamDef>& out) const
{
    out.push_back (ParamDef::choice ("dialStyle", "Dial style",
        { "Half circle", "Vintage fan", "Bar scale", "LED ladder", "Minimal", "Mirror" },
        "Half circle: the classic pink semicircle. "
        "Vintage fan: a narrow fan-shaped window with a thick arc rail, a thin percent scale inside, a long thin needle and a red zone past 0 dB. "
        "Bar scale: a green-yellow-red bar with slanted ticks above, percent dots below and a slider riding on it. "
        "LED ladder: a row of LED columns that step up in height and light up with the level. "
        "Minimal: a big number over a hairline scale with a small cursor. "
        "Mirror: the dB scale runs along the middle with a fill bar above and a mirrored one below.")
        .in ("Dial"));
    out.push_back (ParamDef::choice ("needleStyle", "Needle style",
        { "Thin line", "Wedge", "Stereo L/R" },
        "Thin line: thin needle. Wedge: tapered needle. Stereo L/R: two thin needles, one per channel. "
        "On the bar-type dials it changes the indicator: Thin line = hairline cursor or slim LEDs, "
        "Wedge = filled bar or solid wedge with a big arrow, Stereo L/R = separate L and R tracks.")
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
    if      (id == "dialStyle")   dialStyle   = juce::jlimit (0, numDialStyles - 1, juce::roundToInt (v));
    else if (id == "needleStyle") needleStyle = juce::jlimit (0, 2, juce::roundToInt (v));
    else if (id == "showLed")     showLed     = v >= 0.5;
    else return;
    repaint();
}
