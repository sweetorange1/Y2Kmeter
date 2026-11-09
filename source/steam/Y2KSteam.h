// ==========================================================
// Y2KSteam.h
//   Steam 功能（成就 / 统计）的唯一入口。
//
//   隔离设计（对应 Steam 需求书 C1 / C6 / C7）：
//     · 本头文件**不包含任何 Steamworks 头文件**，因此任何包含它的翻译单元
//       都不会引入 steam/*.h 的符号与宏。
//     · 默认构建（Y2K_STEAM_BUILD=OFF）下 Y2KSteam.cpp 只提供空实现，
//       产物既不含 Steam 导入，也不会依赖 steam_api64.dll。
//     · 真实实现放在 Y2KSteamSteamworks.cpp，**只**被编译进 Y2Kmeter_Standalone
//       （且仅在 Y2K_STEAM_BUILD=ON 时），并在静态初始化阶段把后端函数表注册进
//       本模块。→ VST3 目标（Y2Kmeter / Y2Kmeter_milkdrop）永远拿不到 Steam
//       后端，等价于"插件里不存在任何 Steam 调用"。
//
//   为什么不是单纯 `#if Y2K_STEAM` 双实现：
//     juce_add_plugin 把共享代码编译成静态库（Y2Kmeter = *_SharedCode），
//     PluginEditor / ModuleWorkspace 等埋点都在这个库里。若把 Steam 调用写进
//     这个库，VST3 / AU 链接时同样会带上 steam_api64 依赖；反之若只在
//     Y2Kmeter_Standalone 上定义 Y2K_STEAM，宏也传不到共享代码。
//     用"运行期后端注册"两头都能满足：默认构建零符号，Steam 构建只有
//     Standalone 的 exe 里有真实实现。
//
//   线程约定：本文件所有函数只允许在**消息线程**调用
//   （juce::Timer 回调 / 按钮回调 / UI 事件）；音频线程一律不准碰。
// ==========================================================

#pragma once

// 刻意不 include 任何 JUCE / Steamworks 头：
//   · 调度层（Y2KSteam.cpp）与真实后端（Y2KSteamSteamworks.cpp）都用不到 JUCE；
//   · 这样后端 TU 可以在任何编译配置下独立成立，不受 JUCE 模块宏影响。

namespace y2k::steam
{
    // ------------------------------------------------------
    // 后端函数表
    //   由 Y2KSteamSteamworks.cpp 在静态初始化阶段安装；
    //   未安装时（默认构建 / VST3 / 插件宿主内）所有接口静默降级。
    //   全部字段都必须是"不会抛异常、任何输入都不崩"的纯 C 函数指针。
    // ------------------------------------------------------
    struct Backend
    {
        bool (*initialise)      ()                                    = nullptr;
        void (*shutdown)        ()                                    = nullptr;
        void (*tick)            ()                                    = nullptr;
        bool (*available)       ()                                    = nullptr;
        bool (*unlock)          (const char* apiName)                 = nullptr;
        void (*indicateProgress)(const char* apiName, int cur, int max) = nullptr;
        bool (*isUnlocked)      (const char* apiName)                 = nullptr;
        bool (*restartRequested)()                                    = nullptr;
    };

    namespace detail
    {
        /** 安装后端。只能由 Steam 后端 TU 的静态初始化调用；传入 nullptr 表示卸载。 */
        void installBackend (const Backend* backend) noexcept;

        /** 当前已安装的后端（无则 nullptr）。 */
        const Backend* backend() noexcept;
    }

    // ------------------------------------------------------
    // 生命周期
    // ------------------------------------------------------

    /** 启动时调用一次（幂等）。返回 true 表示 Steam 可用。
        返回 false 时程序必须继续以"无 Steam"模式正常工作。
        若 restartRequested() 为 true，说明 Steam 请求通过客户端重启本进程，
        调用方应尽快退出。 */
    bool initialise();

    /** 退出时调用一次。未初始化时安全。 */
    void shutdown();

    /** 需要周期性调用（200ms 一次，官方要求至少 1Hz）。
        内部只在已初始化时执行 SteamAPI_RunCallbacks()。 */
    void tick();

    // ------------------------------------------------------
    // 状态查询
    // ------------------------------------------------------

    /** Steam 是否可用（未初始化 / 初始化失败 / 非 Steam 构建 一律 false）。 */
    bool available();

    /** 本进程是否链接了 Steam 后端（= exe 是不是 Steam 版）。
        可在 initialise() 之前调用（后端在 main 之前就已注册）。 */
    bool backendLinked();

    /** initialise() 是否请求了"通过 Steam 重启"。 */
    bool restartRequested();

    // ------------------------------------------------------
    // 成就
    // ------------------------------------------------------

    /** 解锁成就。幂等：已解锁不会重复上报；Steam 不可用则静默返回 false。
        返回 true 表示本次调用后该成就处于已解锁状态。 */
    bool unlock (const char* apiName);

    /** 带进度的成就提示（仅用于后台配置了 progress_stat 的成就）。
        cur/max 与后台配置的 progress_stat_min/max 对应；无进度条的成就
        调用本函数是安全的 no-op。 */
    void indicateProgress (const char* apiName, int cur, int max);

    /** 查询是否已解锁。Steam 不可用时恒 false。 */
    bool isUnlocked (const char* apiName);

} // namespace y2k::steam
