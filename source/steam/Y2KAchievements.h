// ==========================================================
// Y2KAchievements.h
//   成就触发逻辑的集中入口。
//
//   业务代码只调用本文件的 report* / sessionTick，不直接接触 y2k::steam，
//   也不关心"是不是 Steam 版"：
//     · Standalone 启动时调用 attach() → 之后才开始累计与解锁；
//     · VST3 / 插件宿主 / 默认构建里从未 attach() → 所有接口立即返回，
//       零磁盘写入、零 Steam 调用。
//
//   累计型成就（忠实用户 / 深潜）的计数会随 settings 持久化，
//   避免"每次重启从零开始"。Steamworks 后台当前没有配置 Stat，因此这里
//   **不调用** SetStat；等后台加了 Stat 再在 unlock 前补一次上报即可。
//
//   线程约束：全部只允许在消息线程调用（UI 回调 / juce::Timer）。
// ==========================================================

#pragma once

#include <juce_data_structures/juce_data_structures.h>

namespace y2k::ach
{
    // ------------------------------------------------------
    // Steamworks 后台的成就 API Name（7 条，已发布）
    //   ★ 后台已从 16 条精简为 7 条；下面这些名字必须与后台完全一致，
    //     否则 SetAchievement 会静默失败（不崩、不报错、只是不解锁）。
    // ------------------------------------------------------
    namespace ids
    {
        inline constexpr const char* firstBoot       = "ACH_FIRST_BOOT";
        inline constexpr const char* marathon        = "ACH_MARATHON";
        inline constexpr const char* milkdropDiver   = "ACH_MILKDROP_DIVER";
        inline constexpr const char* paletteSmith    = "ACH_PALETTE_SMITH";
        inline constexpr const char* capture         = "ACH_CAPTURE";
        inline constexpr const char* loudnessMaster  = "ACH_LOUDNESS_MASTER";
        inline constexpr const char* isaacDeptStore  = "ACH_ISAAC_DEPT_STORE";
    }

    // ------------------------------------------------------
    // 生命周期（仅 Standalone 调用）
    // ------------------------------------------------------

    /** 读入跨会话累计值；此后所有 report* 才会生效。可重复调用。 */
    void attach (juce::PropertiesFile& settings);

    /** 把累计值写回并停用本模块（退出时调用，需早于 appProperties.saveIfNeeded）。 */
    void detach();

    /** 立刻把累计值写入 PropertiesFile（不负责落盘）。 */
    void flush();

    /** 是否处于"Steam 版 Standalone 会话"中。 */
    bool isActive();

    // ------------------------------------------------------
    // 触发点
    // ------------------------------------------------------

    /** #1 初次启动。每次启动都调用即可 —— unlock 幂等，账号侧状态才是真值。 */
    void reportFirstBoot();

    /** #2 会话心跳（建议 1 Hz 调用一次）。deltaSeconds 为上两次调用的真实间隔。 */
    void sessionTick (double deltaSeconds);

    /** #3 当前 Milkdrop 预设（传完整路径或文件名均可，按文件名去重）。 */
    void reportMilkdropPreset (const juce::String& presetPath);

    /** #4 主题保存成功（librarySize 仍保留在签名里，当前不参与判定）。 */
    void reportThemeSaved (int librarySize);

    /** #5 录音片段落盘成功。 */
    void reportClipSaved();

    /** #6 响度计短期读数（每帧调用，内部做 2 s 稳定判定）。 */
    void reportShortTermLufs (float lufsS, double deltaSeconds);

    /** #7 用户点击左上角标题文字打开官网（iisaacbeats.cn）。
        三处标题点击（插件宿主 / chrome 隐藏态 / 常规态）统一走这个入口。 */
    void reportWebsiteVisited();

} // namespace y2k::ach
