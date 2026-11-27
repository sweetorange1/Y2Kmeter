#pragma once

// ==========================================================
// ToyKit —— Toybox 点击解压互动（v3.0.0）单头接口
//
//   按需求案 §11.1 合并为单头，规避 MSVC 多文件 include guard 串扰。
//   实现分布在 ToyKit.cpp / ToyEmoji.cpp / ToyCanvasFx.cpp / fx/ToyFx_*.cpp。
//
//   GL 约束（§4.3）：本头及实现一律不出现 juce::gl / gl 调用 / shader / FBO，
//   全部效果都画在 JUCE 组件层（恒叠在 GL 内容之上）。
// ==========================================================

#include <JuceHeader.h>
#include <memory>
#include <vector>
#include <functional>
#include <unordered_map>

#include "source/ui/toys/ToyPerf.h"
#include "source/ui/toys/ToyPhysics.h"
#include "source/ui/ModuleWorkspace.h"   // ModulePanel / ModuleType / ModuleWorkspace
#include "source/ui/PinkXPStyle.h"
#include "source/analysis/AnalyserHub.h"

class ToyDirector;
class ToyClock;

class ToyOverlay;
class EmojiAtlas;
class ModuleToyState;
struct ToyActor;

// ==========================================================
// 档位
//   Off 已从 UI 下线（不再给用户提供关闭选项）：ToyDirector::setLevel
//   会把任何 Off / 越界入参钳制到 [Chill, Party]，启动恒为 Chill。
// ==========================================================
enum class ToyLevel : int
{
    Off    = 0,   // 仅为兼容旧存档/枚举数值保留，运行时不可达
    Chill  = 1,
    Normal = 2,
    Party  = 3
};

juce::String toyLevelToString (ToyLevel l);
ToyLevel     toyLevelFromInt    (int v, bool* ok = nullptr);
ToyLevel     toyLevelFromString (const juce::String& s, bool* ok = nullptr);

// ==========================================================
// Emoji 白名单（附录 A）
// ==========================================================
enum class EmojiId : int
{
    // ---- 通用池（Tier-1）----
    Heart, SparklingHeart, Star, GlowStar, Sparkles, Note, Notes,
    Sunglasses, HeartEyes, Dizzy, Fire, PartyPopper, Gem,
    // ---- Tier-2 加成 ----
    PartyFace,
    // ---- 主题池 ----
    Butterfly, Bolt, Cherry, Vhs, Cd, BlackHeart,
    Strawberry, Ribbon, Lollipop, Candy, Blossom,
    Moon, Earth, Comet, ShootingStar,
    Alien, CrystalBall, Headphone, Joystick, SpaceInvader,
    Orange, Sun, IceCream, Cocktail, Umbrella,
    Shell, Drop, Fish, OceanWave, Dolphin, Crab,
    CupWithStraw, Leaf, GreenApple, Tea, Herb, Clover,
    Frog, Mushroom, PalmTree, Lizard,
    Floppy, Mouse, Hourglass, Folder, Desktop, Laptop,
    Skull, Bat, Rose, WiltedRose, Spider,
    BlackSquare, WhiteSquare, ButtonSquare, FrameSquare, Plus, Check,
    // ---- 模块玩法 ----
    Boom, SlotMachine, MoneyBag, Clamp, Satellite, RedCircle,
    SoccerBall, Popcorn, Camera, CameraOld, Guitar,
    RaisedHand, WaveHand, Tv, Flag, Volcano, Warning, CrossMark, Paperclip,
    // ---- Tier-3 加成 ----
    Coin, Planet, Bubbles, DiscoBall, Ice, Blood,

    Count
};

juce::String emojiUtf8 (EmojiId id);
bool         emojiIsTier3 (EmojiId id);
EmojiId      emojiFallback (EmojiId id);

// 唯一 emoji 绘制入口（FIX-1 / E.5）：中心对齐 pos、尺寸 drawSize、旋转 angle。
// scaleY：纵向缩放（1=正常，<1 落地挤压）。
// 变换系数由 y2ktoy::emojiTransform 纯函数计算，禁止各 actor 自行拼变换。
void drawEmoji (juce::Graphics& g, const juce::Image& img,
                juce::Point<float> pos, float drawSize, float angle, float alpha = 1.0f,
                float scaleY = 1.0f);

// ==========================================================
// 语义角色与主题色（E.3）
// ==========================================================
// 角色（E.3.1）：玩法写角色，不直接写 EmojiId；由当前主题决定用哪个 emoji。
enum class ToyRole : int
{
    Reward = 0,   // 奖励（LUFS 命中、Meters 顶砖、贴纸）
    Burst,        // 爆裂（碎片、点爆、通用兜底）
    Orb,          // 小球（弹珠、Line Rider 球）
    Foe,          // 入侵者
    Cheer,        // 欢呼（人浪）
    Pin,          // 标记（贴纸、旗帜）
    Alert,        // 警告（固定 ⚠️，语义不能变）
    Clear,        // 放心（固定 ✔️）
    Flame,        // 火焰（固定 🔥）
    Count
};

// ToyPalette 语义色（E.3.3）：实时从 PinkXP 当前主题映射，不缓存。
namespace ToyPalette
{
    inline juce::Colour ink     () { return PinkXP::ink; }
    inline juce::Colour paper   () { return PinkXP::face; }
    inline juce::Colour accent  () { return PinkXP::sel; }
    inline juce::Colour accent2 () { return PinkXP::accent2; }
    inline juce::Colour glow    () { return juce::Colours::white; }
    inline juce::Colour shadow  () { return PinkXP::dark; }
    inline juce::Colour danger  () { return juce::Colours::red; }
    inline juce::Colour warn    () { return juce::Colours::yellow; }
    inline juce::Colour ok      () { return juce::Colours::lime; }
    inline juce::Colour face    () { return PinkXP::face; }
    inline juce::Colour shdw    () { return PinkXP::shdw; }
}

// 角色 → emoji（按当前主题，E.3.1 草案表）
EmojiId toyRoleEmoji (ToyRole role);

// ==========================================================
// 命中结构（§11.3）
// ==========================================================
struct ToyHit
{
    int                     kind = 0;
    int                     id   = 0;
    juce::Rectangle<float>  rect;
    juce::Point<float>      pos;
    float                   v[3] = { 0.0f, 0.0f, 0.0f };   // 带给 actor 的物理量（E.5）
    int                     row  = 0;
    int                     col  = 0;
};

// ==========================================================
// Actor 基类（模块内 actor 与自由 actor 共用）
// ==========================================================
struct ToyActor
{
    float age = 0.0f;
    float ttl = 8.0f;
    bool  alive = true;

    juce::Rectangle<float> prevBounds;
    bool                   hasPrevBounds = false;

    virtual ~ToyActor() = default;
    virtual void step (float dt, ToyDirector& dir) = 0;
    virtual void paint (juce::Graphics& g, ToyDirector& dir) = 0;
    virtual juce::Rectangle<float> getBounds() const = 0;
    virtual bool isHighDetailForPerformance() const { return false; }
    virtual bool simplifyForPerformance() { return false; }

    // 可点击元素（TruePeak 的 OK、Invader、Clippo 等，E.5）。
    //   返回 true 表示本次点击被消费（不再路由到模块玩法/画布喷发）。
    virtual bool onClick (juce::Point<float> p) { juce::ignoreUnused (p); return false; }

    // 画布"点爆"目标（C-4）：自由 emoji 粒子可被点击引爆。
    virtual bool isPoppable() const { return false; }

    // 外力冲量（点爆邻近波、鼠标力场共用）。
    virtual void impulse (juce::Point<float> from, float strength)
    {
        juce::ignoreUnused (from, strength);
    }
};

// ==========================================================
// 工厂（实现分散在 ToyCanvasFx.cpp / ToyKit.cpp）
// ==========================================================
// 自由 emoji 粒子（Editor 坐标）
std::unique_ptr<ToyActor> makeEmojiParticle (EmojiId id, juce::Point<float> at,
                                             juce::Point<float> v, int sizeTier,
                                             ToyDirector& dir,
                                             float ttl = 8.0f,
                                             float gravity = 1100.0f);
std::unique_ptr<ToyActor> makeConfettiRainParticle (juce::Point<float> at,
                                                    juce::Point<float> velocity,
                                                    ToyDirector& dir, bool rainbow,
                                                    juce::Rectangle<int> clipBounds);
// Oscilloscope 双态荧光电子玩法（模块内 actor，实现见 fx/ToyFx_Oscilloscope.cpp）：
//   · Waveform 模式：两颗荧光光珠贴着真实波形向两侧冲浪（波峰起跳、撞边溅火花）
//   · X-Y / Lissajous 模式：中心引力轨道荧光电子（椭圆/玫瑰余辉轨迹，轻度响应音频能量）
//   两者均为极细透明矢量、零遮挡块，并在用户按住慢放期间自动降速至子弹时间并柔淡退让。
std::unique_ptr<ToyActor> makeOscWaveSurferActor (juce::Point<float> clickPos,
                                                  juce::Rectangle<int> canvas,
                                                  const juce::Array<float>& waveSamples,
                                                  std::function<bool()> isSlowMo = {});
std::unique_ptr<ToyActor> makeOscOrbitElectronActor (juce::Point<float> clickPos,
                                                     juce::Rectangle<int> canvas,
                                                     std::function<bool()> isSlowMo = {});

// ==========================================================
// 模块专属玩法 actor 工厂（v3.5 全模块统一荧光矢量物理重构，实现见 ToyModuleFx.cpp）
// ==========================================================
// 频率 → 音名（A4=440Hz，纯函数）
juce::String toyFreqToNoteName (float freq);

// Spectrum：拨弦驻波 + 真实泛音节点（2f/3f/4f）+ 沿真实频谱轮廓冲浪的双微火花（按住子弹时间自动退让）
std::unique_ptr<ToyActor> makeSpectrumPluckActor (juce::Point<float> clickPos,
                                                  juce::Rectangle<int> canvas,
                                                  const std::vector<float>& curveY,
                                                  const std::vector<juce::Point<float>>& harmonics,
                                                  std::function<bool()> isSlowMo = {});

// Waveform：贴合真实波形上下包络的双磷光扫描波前（单/双声道自适应，按住冻结时柔淡降速退让）
std::unique_ptr<ToyActor> makeWaveformShockActor (juce::Point<float> clickPos,
                                                  juce::Rectangle<int> laneRect,
                                                  const std::vector<float>& envTopY,
                                                  const std::vector<float>& envBotY,
                                                  std::function<bool()> isSlowMo = {});

// Spectrogram：四方向真实跟随瀑布流的十字声呐浮标（按住冻结时完全静止 0x 并柔淡退让）
std::unique_ptr<ToyActor> makeSpectrogramBuoyActor (juce::Point<float> clickPos,
                                                    juce::Rectangle<int> canvas,
                                                    juce::Point<float> driftVelPxPerSec,
                                                    std::function<bool()> isFrozen = {});

// Spectrogram3D：随投影斜向（Back-right / Back-left）对齐的透视激光雷达脉冲 + 沿深度轴漂移的磷光微粒
std::unique_ptr<ToyActor> makeSpec3DPulseActor (juce::Point<float> clickPos,
                                                juce::Rectangle<int> canvas,
                                                juce::Point<float> depthDirUnit);

// StereoField：半圆雷达扫描光束 + 极坐标弧形涟漪 + 沿等电平弧往复摆动的阻尼荧光光珠
std::unique_ptr<ToyActor> makeStereoRadarSweepActor (juce::Point<float> clickPos,
                                                     juce::Rectangle<int> canvas,
                                                     juce::Point<float> origin,
                                                     float radarRadius);

// StereoMap：PAN 模式弹性中线拨弦 + 对称双光珠 / MONO 模式相干干涉波纹（按住子弹时间自动退让）
std::unique_ptr<ToyActor> makeStereoMapBeamActor (juce::Point<float> clickPos,
                                                  juce::Rectangle<int> plot,
                                                  bool isMonoMode,
                                                  std::function<bool()> isSlowMo = {});

// Phase Gonio：限制在 M/S 菱形内的荧光引力电子（四壁菱形反射 + 微火花）
std::unique_ptr<ToyActor> makePhaseGonioOrbiterActor (juce::Point<float> clickPos,
                                                      juce::Rectangle<int> gonioArea);

// Phase WIDTH / BAL 条形槽：纯矢量荧光弹珠（渐变拖尾 + 撞壁微火花）
std::unique_ptr<ToyActor> makeTrackOrbActor (juce::Rectangle<float> track,
                                             juce::Point<float> clickPos,
                                             bool useAccent2 = false);

// 指针弹拨虚影（Phase Corr / VU 指针式共用，叠加在真实指针上）：
//   theta 为数学角度（0=最左，π/2=正上，π=最右）；startThetaRad < 0 时默认以 restThetaRad 为基点轻拨。
std::unique_ptr<ToyActor> makeGhostNeedleActor (juce::Point<float> pivot,
                                                float radius,
                                                float restThetaRad,
                                                float startThetaRad = -1.0f);

// 纵向 LED 柱体（Loudness LED 柱 / Dynamics Peak-RMS 柱）：自底向上荧光扫光柱 + 弹跳顶帽 + 微火花
std::unique_ptr<ToyActor> makeMeterBarLaunchActor (juce::Rectangle<float> barRect,
                                                   float clickY,
                                                   juce::Colour accentColour);

// 数字读数框（Loudness Readouts）：沿框体四周双向疾驰的双霓虹光束（零遮挡中央数字）
std::unique_ptr<ToyActor> makePerimeterSweepActor (juce::Rectangle<float> boxRect,
                                                   juce::Point<float> clickPos,
                                                   juce::Colour accentColour);

// Dynamics DR 框：左右对称荧光压缩括号 [ ] 弹性收紧再回弹（零遮挡中央数字）
std::unique_ptr<ToyActor> makeCompressorClampActor (juce::Rectangle<float> cellRect,
                                                    juce::Point<float> clickPos);

// Dynamics Crest 历史带：沿真实 Crest 历史折线向两侧冲浪的双荧光光珠
std::unique_ptr<ToyActor> makeCrestSurferActor (juce::Point<float> clickPos,
                                                juce::Rectangle<int> plotRect,
                                                const std::vector<float>& curveY);

// TempoKey BPM 列：严格按真实 BPM 节拍周期律动的节拍脉冲环 + 底边摆动节拍光点
std::unique_ptr<ToyActor> makeBpmMetronomePulseActor (juce::Rectangle<int> colRect,
                                                      juce::Point<float> clickPos,
                                                      float bpm);

// TempoKey KEY 列：12 音高星座环，点亮当前调式真实 7 个音级并以极细星轨连线
std::unique_ptr<ToyActor> makeKeyConstellationActor (juce::Rectangle<int> colRect,
                                                     juce::Point<float> clickPos,
                                                     int keyRoot, int keyMode);

// SystemMonitor：PCB 电路板 90°/45° 信号走线脉冲 + 发光焊盘端点
std::unique_ptr<ToyActor> makeSysCircuitPulseActor (juce::Point<float> clickPos,
                                                    juce::Rectangle<int> tileRect,
                                                    juce::Colour accentColour);

// VU 横向刻度条：弹簧游标（幽灵游标从点击处带过冲弹回真实电平，拖一根弹簧 + 荧光残影）
std::unique_ptr<ToyActor> makeSliderFlickActor (float top, float bottom, float clickX, float restX);
// VU LED 梯柱：跑马冲顶（从左逐格点亮到点击格，顶帽弹飞溅微火花后落回；capDir -1 向上 / +1 向下）
std::unique_ptr<ToyActor> makeLedChaseActor (std::vector<juce::Rectangle<float>> cols,
                                             std::vector<juce::Colour> colours, float capDir);
// VU 极简游标：卷尺（从当前游标量到点击处，标出 dB 差值后卷回）
std::unique_ptr<ToyActor> makeMeasureTapeActor (float lineY, float fromX, float toX, const juce::String& label);
// VU 镜像刻度：水滴涟漪（以中脊为水面上下对称荡开三圈，限制在 top..bottom / xMin..xMax 内）
std::unique_ptr<ToyActor> makeMirrorRippleActor (float x, float spineY, float top, float bottom, float xMin, float xMax);
// VU 削波灯：霓虹光晕脉冲环
std::unique_ptr<ToyActor> makeDiscoLedActor (juce::Rectangle<float> ledArea);

// ==========================================================
// EmojiAtlas —— 系统 emoji 字体烘焙成软件图集（§5 / E.3.2）
//   预烘焙在消息线程分帧完成；paint 只读，未就绪返回空图。
//   主题版本（duotone + 描边）单独缓存，主题切换后按需重生成。
// ==========================================================
class EmojiAtlas : private juce::Timer
{
public:
    enum class Size : int { S = 0, M = 1, L = 2, Count = 3 };

    EmojiAtlas();
    ~EmojiAtlas() override;

    const juce::Image& image (EmojiId id, Size size);        // 原图，只读
    const juce::Image& themedImage (EmojiId id, Size size);  // 主题版本（无则回退原图）
    bool has (EmojiId id) const;
    bool isBaked (EmojiId id, Size size) const;              // 是否已烘焙（不触发烘焙，纯查）

    // 旋转分桶（性能优化 P1）：angleRad 量化到 kRotBuckets 个角度，
    //   首次按需烘焙（主题 duotone 版），之后返回缓存图。
    //   paint 时用该图做整数坐标 drawImage，替代逐像素旋转插值。
    //   线程模型（卡死修复）：烘焙只在 step 阶段（ensureRotated），
    //   paint 只做无锁快查（rotatedCached，值传递），miss 走慢路径。
    static constexpr int kRotBuckets = 12;
    static int bucketOf (float angleRad);                               // 纯角度量化（无锁）
    juce::Image rotatedCached (EmojiId id, Size size, float angleRad);  // 只查不烘焙
    // step 阶段预热（查 + 烘焙）。返回 true = 缓存里已有该条目（可用于 paint）；
    //   false = 被烘焙节流 / 原图未就绪，调用方应在后续帧重试。
    bool ensureRotated (EmojiId id, Size size, float angleRad);

    // Win 性能优化：物理像素缩放恰为 1.0（Windows 100% 缩放）时，旋转分桶图直接烘焙成
    //   drawSize×drawSize，paint 走 1:1 drawImageAt。原先图集是 2x+2 px、绘制尺寸
    //   18/24/34，每个 emoji 都走 drawImageTransformed —— JUCE GL 后端对非平移
    //   变换要建 Path→EdgeTable 光栅化（实测 0.07~0.5ms/个），是点击喷 emoji 掉帧主因。
    //   缩放 != 1 时保持旧行为（图集 2x 分辨率，避免模糊）。由 ToyOverlay::paint 回写。
    void setPhysicalScale (float s) noexcept { fitDrawSize_ = std::abs (s - 1.0f) < 0.01f; }
    bool fitsDrawSize() const noexcept       { return fitDrawSize_; }

    static int logicalSize (Size s);
    int drawSize (Size s) const;

    void invalidateThemed();   // 主题切换：清空主题版本并重新排队
    void release();

private:
    struct Entry
    {
        juce::Image img;
        bool        baked = false;
        bool        ok    = false;
    };
    Entry entries_[(int) EmojiId::Count][(int) Size::Count];

    struct ThemedEntry
    {
        juce::Image img;
        bool        baked = false;
        bool        ok    = false;
    };
    ThemedEntry themed_[(int) EmojiId::Count][(int) Size::Count];

    juce::String emojiFamily;
    int          prebakeCursor_ = 0;     // 预烘焙游标（id*3+size 展平）
    int          themedCursor_  = 0;     // 主题版本游标
    double       prebakeStartDelay_ = 0.0;  // 启动约 1 秒后再开始
    bool         prebakeScheduled_ = false;

    // 旋转分桶缓存（性能优化 P1）：key = id*12 + size*4 + bucket
    juce::CriticalSection rotLock_;
    std::unordered_map<size_t, juce::Image> rotCache_;
    bool fitDrawSize_ = false;

    void timerCallback() override;      // 分帧预烘焙（每 tick ≤ 8 个）
    void bakeOne (EmojiId id, Size size);
    void bakeThemedOne (EmojiId id, Size size);
    bool probe  (const juce::Image& white, const juce::Image& black, float coverageThreshold);

    static float duotoneStrength();     // E.3.2 强度表

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EmojiAtlas)
};

// ==========================================================
// ModuleToyState —— 每个被点击过的面板才分配（§7）
// ==========================================================
class ModuleToyState
{
public:
    ModuleToyState (ModulePanel& owner, ToyDirector& dir);

    juce::Rectangle<int> step (float dt);
    void paint (juce::Graphics& g);
    void clear();

    std::vector<std::unique_ptr<ToyActor>> actors;

private:
    ModulePanel&  owner;
    ToyDirector*  director;
};

// ==========================================================
// ToyContext —— 给玩法用的受限能力（§11.3）
// ==========================================================
struct ToyContext
{
    ToyDirector*    director = nullptr;
    ModulePanel*    panel    = nullptr;
    ModuleToyState* state    = nullptr;
    ToyHit          hit;

    ToyLevel level() const;
    float    audioEnergy() const;
    float    uiScale() const noexcept;

    void spawnEmoji (EmojiId id, juce::Point<float> at,
                     juce::Point<float> v, int sizeTier = 1);
    void addModuleActor (std::unique_ptr<ToyActor> a);
    void addModuleActorCapped (std::unique_ptr<ToyActor> a, size_t maxActors = 6);
    void promoteToFree (std::unique_ptr<ToyActor> a);

    juce::Rectangle<int> contentBounds() const;

    // E.5 扩展
    float ui (float logicalPx) const noexcept { return logicalPx * uiScale(); }
    EmojiId role (ToyRole r) const { return toyRoleEmoji (r); }
    // 统一读数气泡：面板局部坐标，自动避让模块边缘，1.6 秒上浮淡出
    void bubble (juce::Point<float> at, const juce::String& text, ToyRole r);
};

// 读数气泡工厂（E.5）：Win95 凸起框，主题色；钳制在 clampRect（模块内容区）内。
std::unique_ptr<ToyActor> makeReadoutBubble (juce::Point<float> at,
                                             const juce::String& text,
                                             EmojiId icon, juce::Colour accentColour,
                                             juce::Rectangle<int> clampRect = {});

// ==========================================================
// ToyClock —— AnalyserHub::FrameListener，驱动所有 actor（§7）
// ==========================================================
class ToyClock : public AnalyserHub::FrameListener
{
public:
    explicit ToyClock (ToyDirector& dir);
    ~ToyClock() override = default;

    void onFrame (const AnalyserHub::FrameSnapshot& frame) override;

    void trackPanel   (ModulePanel& p);
    void cancelPanel  (ModulePanel& p);

    std::vector<std::unique_ptr<ToyActor>> freeActors;

    double lastActivityTime = 0.0;

    ToyDirector& director;

    /** 诊断快照（取走并清零窗口累计量；消息线程 / 持消息锁时调用）。 */
    struct DiagSnapshot
    {
        int    freeActors = 0, fullEmoji = 0, emojiTarget = 0;
        double smoothedFps = 0.0;
        double overlayPaintMs = 0.0;
        int    overlayPaints = 0, overlayActorsPainted = 0;
    };
    DiagSnapshot takeDiagSnapshot() noexcept
    {
        DiagSnapshot s;
        s.freeActors = (int) freeActors.size();
        s.fullEmoji = highDetailFreeActorCount_;
        s.emojiTarget = targetFullEmojiCount_;
        s.smoothedFps = smoothedFps_;
        s.overlayPaintMs = overlayPaintTotalMs_;
        s.overlayPaints = overlayPaintCalls_;
        s.overlayActorsPainted = overlayActorsPainted_;
        overlayPaintTotalMs_ = 0.0;
        overlayPaintCalls_ = 0;
        overlayActorsPainted_ = 0;
        return s;
    }
private:
    std::vector<juce::Component::SafePointer<ModulePanel>> panels;

    // 性能日志（每秒一行，写入 exe 目录 toy_perf_log.txt）
    int    perfFrames_      = 0;
    double perfLastLogTime_ = 0.0;
    double lastFrameMs_     = 0.0;   // 上一帧 actor 处理耗时
    double frameTotalMs_    = 0.0;
    double frameMaxMs_      = 0.0;
    double overlayPaintTotalMs_ = 0.0;
    int    overlayPaintCalls_   = 0;
    int    overlayActorsPainted_ = 0;
    int    visibleFreeActorCount_ = 0;
    int    highDetailFreeActorCount_ = 0;
    double lastFrameTime_ = 0.0;
    double smoothedFps_ = 60.0;
    int    targetFullEmojiCount_ = 160;
    double platformCacheTime_ = -1.0;  // 平台几何缓存时间戳（0.5s 刷新，模块移动不改每帧遍历）

    friend class ToyOverlay;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ToyClock)
};

// ==========================================================
// ToyOverlay —— 按需创建的全窗口叠加层（鼠标穿透、非不透明，§7）
// ==========================================================
class ToyOverlay : public juce::Component
{
public:
    explicit ToyOverlay (ToyClock& clock);
    void paint (juce::Graphics& g) override;

    ToyClock& clock;
};

// ==========================================================
// ToyDirector —— 全局鼠标监听器 + 路由 + 连击 + 彩蛋（§7）
// ==========================================================
class ToyDirector : public juce::MouseListener,
                    private juce::Timer
{
public:
    ToyDirector (juce::Component& editorComponent,
                 ModuleWorkspace& workspace,
                 AnalyserHub& hub);
    ~ToyDirector() override;

    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp   (const juce::MouseEvent& e) override;

    void setLevel (ToyLevel l);
    ToyLevel getLevel() const noexcept { return level_; }

    std::unique_ptr<EmojiAtlas> atlas;
    std::unique_ptr<ToyClock>   clock;
    std::unique_ptr<ToyOverlay> overlay;

    void attachToFloating   (ModulePanel& p);
    void detachFromFloating (ModulePanel& p);

    void spawnCanvasFountain (juce::Point<float> editorPos, float charge = 0.0f);
    void spawnSmallBurst     (juce::Point<float> editorPos, float speedBoost);   // C-2 持续喷
    void spawnGenericFallback (ModulePanel& panel, juce::Point<float> panelPos);

    // C-2/C-3/C-4：按住持续喷、鼠标力场、点爆（由 ToyClock::onFrame 驱动）
    void sprayTick  (double timeSec);
    void tickConfettiRain (double timeSec);
    void mouseForceField (juce::Point<float> editorPos, float dtSec);
    bool tryPopEmoji (juce::Point<float> editorPos);
    void stopSpraying() { spraying_ = false; }

    int  comboCountForFx() const noexcept { return comboCount_; }

    void spawnBurst (juce::Point<float> editorPos);   // 爆裂火花（点爆/击落共用）

    float audioEnergy() const;
    float uiScale() const noexcept { return PinkXP::uiScale(); }

    // 确保叠加层已挂载并可见（供 clock 调用）
    void ensureOverlay();

    // 平台上沿（Editor 坐标，FIX-3 / B-03）：带 x 范围，命中需 x∈[x0,x1]
    struct Platform { float x0 = 0.0f, x1 = 0.0f, y = 0.0f; };
    std::vector<Platform> platformTops;

    // 档位被连击逻辑自动改变（含 3 s 超时降级）后通知 Editor 同步状态。
    std::function<void(int)>             onLevelChanged;

    juce::Component&  editor;
    ModuleWorkspace&  workspace;
    AnalyserHub&      hub;

private:
    ToyLevel level_ = ToyLevel::Chill;

    int    comboCount_   = 0;
    double lastComboTime_ = 0.0;

    bool   charging_ = false;
    juce::Point<float> chargePos_;
    double chargeStartTime_ = 0.0;

    int    totalPokes_ = 0;

    double lastStartledTime_ = 0.0;   // 宠物 startled 冷却（8 秒）

    int themeSubToken = -1;           // 主题订阅（图集 duotone 失效）

    juce::Point<float> lastDragPos_;
    bool   draggingOnCanvas_ = false;

    // C-2 按住持续喷（花洒）
    bool   spraying_ = false;
    juce::Point<float> sprayPos_;
    double sprayStartTime_ = 0.0;
    double lastSprayTime_ = 0.0;
    int    confettiRainRemaining_ = 0;
    double nextConfettiRainTime_ = 0.0;

    // C-3 鼠标力场
    juce::Point<float> lastMousePos_;
    bool   hasLastMousePos_ = false;

    void routeGlobal   (juce::Component* orig, const juce::MouseEvent& e);
    void routeModule   (ModulePanel& panel, const juce::MouseEvent& e);
    bool gateAllows    (const juce::MouseEvent& e) const;
    bool isOptOut      (juce::Component* c) const;

    void timerCallback() override;   // 3 s 无互动 → 连击归零并降级回 Chill
    void registerComboClick (juce::Point<float> editorPos);
    void onComboMilestone (int count, juce::Point<float> pos);
    void notifyPetReaction (int animId);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ToyDirector)
};

// ==========================================================
// 通用兜底 actor（✨ 小火花 + 扩散环，§8.5）
// ==========================================================
struct SparkBurstActor : public ToyActor
{
    struct Spark { juce::Point<float> p; juce::Point<float> v; float life; float maxLife; };
    std::vector<Spark> sparks;
    juce::Point<float> center;

    SparkBurstActor (juce::Point<float> c, int count, float spread);

    void step (float dt, ToyDirector& dir) override;
    void paint (juce::Graphics& g, ToyDirector& dir) override;
    juce::Rectangle<float> getBounds() const override;
};
