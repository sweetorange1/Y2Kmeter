// ==========================================================
// Y2KAchievements.cpp
//   成就触发逻辑实现。只依赖 y2k::steam 的薄封装，不含任何 Steamworks 头。
//
//   后台成就已由 16 条精简为 7 条（2026-11-09），本文件与之严格一一对应：
//     #1 ACH_FIRST_BOOT        reportFirstBoot()          初次启动
//     #2 ACH_MARATHON          sessionTick() 累计 10 h    忠实用户（唯一的长期成就）
//     #3 ACH_MILKDROP_DIVER    reportMilkdropPreset() 5 个深潜
//     #4 ACH_PALETTE_SMITH     reportThemeSaved()         调色师
//     #5 ACH_CAPTURE           reportClipSaved()          录下来
//     #6 ACH_LOUDNESS_MASTER   reportShortTermLufs()      响度达标
//     #7 ACH_ISAAC_DEPT_STORE  reportWebsiteVisited()     艾萨克百货大楼
//
//   设计原则：除 #2 忠实用户（累计 10 小时）外，其余成就都应"顺手就能拿到"，
//   不要设需要刻意刷的门槛。
// ==========================================================

#include "source/steam/Y2KAchievements.h"
#include "source/steam/Y2KSteam.h"

#include <cmath>

namespace y2k::ach
{

namespace
{
    // ---- settings key ----
    //   ★ 绝对不能改名：老用户 / 测试玩家的存档里已有累计进度，
    //     改名等于把「忠实用户」「深潜」的进度清零。
    constexpr const char* kKeyRuntimeSeconds = "steam.runtimeSeconds";
    constexpr const char* kKeyPresetCount    = "steam.presetCount";
    constexpr const char* kKeyPresetPrefix   = "steam.preset";

    // ---- 判定阈值 ----
    //   ★ 设计目标：除「忠实用户」（累计 10 小时）外，全部成就都应当"顺手就能拿到"。
    constexpr double kMarathonSeconds     = 10.0 * 3600.0;   // #2 累计 10 小时（唯一的长期成就）
    constexpr double kLoudnessHoldSeconds = 1.0;             // #6 稳定 1 s（原 2 s，过严）
    constexpr float  kLoudnessTargetLufs  = -14.0f;
    constexpr float  kLoudnessTolerance   = 10.0f;           // ±10 LU（覆盖 -24 ~ -4）
                                                            //   2026-11-09 实测：用户播放的内容短期响度约 -7 LUFS，
                                                            //   原 ±0.5 差 7 LU 永远进不了窗；±1.5 也不够。
                                                            //   -14 仍作为"理想目标"保留在文案里，判定给足容差。

    constexpr int kPresetsForDiver   = 5;    // #3 累计 5 个不同预设（原 10，改成"顺手"）
    constexpr int kMaxTrackedPresets = 16;   // 存档上限，避免无限增长

    constexpr double kPersistIntervalSeconds = 300.0;  // 5 分钟落盘一次

    juce::PropertiesFile* gSettings = nullptr;

    double gRuntimeSeconds      = 0.0;
    double gLoudnessHoldSeconds = 0.0;
    double gSecondsSincePersist = 0.0;

    juce::StringArray gPresetNames;

    void persist()
    {
        if (gSettings == nullptr)
            return;

        gSettings->setValue (kKeyRuntimeSeconds, juce::roundToInt (gRuntimeSeconds));
        gSettings->setValue (kKeyPresetCount,    gPresetNames.size());

        for (int i = 0; i < gPresetNames.size(); ++i)
            gSettings->setValue (juce::String (kKeyPresetPrefix) + juce::String (i),
                                 gPresetNames[i]);

        // 清掉可能残留的多余下标（预设集合变小 / 首次写入时）
        for (int i = gPresetNames.size(); i < kMaxTrackedPresets; ++i)
            gSettings->removeValue (juce::String (kKeyPresetPrefix) + juce::String (i));
    }
}

// ----------------------------------------------------------
// 生命周期
// ----------------------------------------------------------
void attach (juce::PropertiesFile& settings)
{
    gSettings = &settings;

    gRuntimeSeconds      = juce::jmax (0.0, settings.getDoubleValue (kKeyRuntimeSeconds, 0.0));
    gLoudnessHoldSeconds = 0.0;
    gSecondsSincePersist = 0.0;

    gPresetNames.clear();
    const int saved = juce::jlimit (0, kMaxTrackedPresets,
                                    settings.getIntValue (kKeyPresetCount, 0));
    for (int i = 0; i < saved; ++i)
    {
        const auto name = settings.getValue (juce::String (kKeyPresetPrefix) + juce::String (i));
        if (name.isNotEmpty())
            gPresetNames.addIfNotAlreadyThere (name);
    }
}

void detach()
{
    persist();

    gSettings = nullptr;
    gRuntimeSeconds      = 0.0;
    gLoudnessHoldSeconds = 0.0;
    gSecondsSincePersist = 0.0;
    gPresetNames.clear();
}

void flush()
{
    persist();
    gSecondsSincePersist = 0.0;
}

bool isActive()
{
    return gSettings != nullptr;
}

// ----------------------------------------------------------
// #1 初次启动
// ----------------------------------------------------------
void reportFirstBoot()
{
    // 不做本地标记：Steam 账号侧的解锁状态才是真值。离线首启没解锁的玩家，
    // 下次带 Steam 启动时会自动补上（unlock 幂等）。
    y2k::steam::unlock (ids::firstBoot);
}

// ----------------------------------------------------------
// #4 主题保存（后台只剩「调色师」，不再有主题数量成就）
// ----------------------------------------------------------
void reportThemeSaved (int /*librarySize*/)
{
    if (gSettings == nullptr)
        return;

    y2k::steam::unlock (ids::paletteSmith);
}

// ----------------------------------------------------------
// #5 录音片段保存
// ----------------------------------------------------------
void reportClipSaved()
{
    if (gSettings == nullptr)
        return;

    y2k::steam::unlock (ids::capture);
}

// ----------------------------------------------------------
// #3 Milkdrop 预设（按文件名去重，内置库 / 收藏库同名视为同一个）
// ----------------------------------------------------------
void reportMilkdropPreset (const juce::String& presetPath)
{
    if (gSettings == nullptr || presetPath.isEmpty())
        return;

    const auto name = juce::File (presetPath).getFileName();
    if (name.isEmpty() || gPresetNames.contains (name))
        return;

    gPresetNames.addIfNotAlreadyThere (name);

    // 超过上限时丢最早的记录：只影响"见过几个"的统计精度，
    // 不影响成就本身（阈值远小于上限）。
    while (gPresetNames.size() > kMaxTrackedPresets)
        gPresetNames.remove (0);

    persist();

    if (gPresetNames.size() >= kPresetsForDiver)
        y2k::steam::unlock (ids::milkdropDiver);
}

// ----------------------------------------------------------
// #6 响度计短期读数 -14 LUFS ±1.5 稳定 1 s
// ----------------------------------------------------------
void reportShortTermLufs (float lufsS, double deltaSeconds)
{
    if (gSettings == nullptr || deltaSeconds <= 0.0)
        return;

    // lufsS 在无信号时是 -144（静音哨兵），不参与判定
    const bool inRange = (lufsS > -100.0f)
                      && (std::abs (lufsS - kLoudnessTargetLufs) <= kLoudnessTolerance);

    if (! inRange)
    {
        gLoudnessHoldSeconds = 0.0;
        return;
    }

    gLoudnessHoldSeconds += juce::jlimit (0.0, 0.25, deltaSeconds);  // 防掉帧跳变

    if (gLoudnessHoldSeconds >= kLoudnessHoldSeconds)
        y2k::steam::unlock (ids::loudnessMaster);
}

// ----------------------------------------------------------
// #2 会话心跳（累计运行时长）
// ----------------------------------------------------------
void sessionTick (double deltaSeconds)
{
    if (gSettings == nullptr || deltaSeconds <= 0.0)
        return;

    const double dt = juce::jlimit (0.0, 5.0, deltaSeconds);  // 防休眠唤醒后一次性跳变

    gRuntimeSeconds += dt;

    if (gRuntimeSeconds >= kMarathonSeconds)
        y2k::steam::unlock (ids::marathon);

    gSecondsSincePersist += dt;
    if (gSecondsSincePersist >= kPersistIntervalSeconds)
    {
        persist();
        gSecondsSincePersist = 0.0;

        // 累计值定期落盘：崩溃 / 强杀也不会把「忠实用户」的进度清零。
        //   5 分钟一次，且不在音频线程 → 不会与 Auto-Cloud 形成频繁上传。
        if (gSettings != nullptr)
            gSettings->saveIfNeeded();
    }
}

// ----------------------------------------------------------
// #7 点击左上角标题打开官网
// ----------------------------------------------------------
void reportWebsiteVisited()
{
    if (gSettings == nullptr)
        return;

    y2k::steam::unlock (ids::isaacDeptStore);
}

} // namespace y2k::ach
