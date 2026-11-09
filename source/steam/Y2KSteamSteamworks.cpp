// ==========================================================
// Y2KSteamSteamworks.cpp
//   Steamworks 真实后端（**只**编译进 Y2Kmeter_Standalone，且只在
//   Y2K_STEAM_BUILD=ON 时）。这是整个仓库里唯一 include <steam/steam_api.h>
//   的地方。
//
//   注册方式：本 TU 的静态初始化在 main() 之前把函数表塞进 y2k::steam，
//   因此共享代码（PluginEditor / ModuleWorkspace / ...）里的 unlock() 调用
//   在 Steam 版 Standalone 中会走到这里，而在 VST3 / 默认构建中永远走空实现。
//   这样既不需要在业务代码里写 #if，也不会让插件目标链接 Steam。
// ==========================================================

#include "Y2KSteam.h"

#if Y2K_STEAM

#include <steam/steam_api.h>

#include <atomic>

namespace y2k::steam
{
namespace
{
    // Y2Kmeter 在 Steamworks 后台的 AppID（需求书 §2）
    constexpr AppId_t kAppId = 4972120;

    std::atomic<bool> g_ready          { false };
    std::atomic<bool> g_restartRequest { false };

    bool steamInitialise()
    {
        if (g_ready.load (std::memory_order_acquire))
            return true;

        // 从 steamapps 目录直接双击 exe 时，交由 Steam 客户端重启本进程。
        //   · 开发期 exe 同目录存在 steam_appid.txt → 恒返回 false。
        //   · 返回值 true 时必须尽快退出，否则会和 Steam 拉起的新实例互斥。
        if (SteamAPI_RestartAppIfNecessary (kAppId))
        {
            g_restartRequest.store (true, std::memory_order_release);
            return false;
        }

        // Steam 未运行 / 无许可 / 初始化失败 → 静默降级，程序照常工作。
        if (! SteamAPI_Init())
            return false;

        g_ready.store (true, std::memory_order_release);

        // 不需要（也不能）再调 ISteamUserStats::RequestCurrentStats()：
        //   SDK v1.58 起该接口已删除（v1.65 的 isteamuserstats.h 里只剩注释），
        //   成就 / 统计缓存会在 SteamAPI_Init() 内部自动异步拉取。
        //   在 UserStatsReceived_t 回调到达之前查询可能读到"未解锁"，
        //   但 SetAchievement + StoreStats 可以立即调用，不影响解锁结果。

        return true;
    }

    void steamShutdown()
    {
        if (! g_ready.exchange (false, std::memory_order_acq_rel))
            return;

        SteamAPI_Shutdown();
    }

    void steamTick()
    {
        if (g_ready.load (std::memory_order_acquire))
            SteamAPI_RunCallbacks();
    }

    bool steamAvailable()
    {
        return g_ready.load (std::memory_order_acquire);
    }

    bool steamUnlock (const char* apiName)
    {
        if (apiName == nullptr || ! g_ready.load (std::memory_order_acquire))
            return false;

        auto* stats = SteamUserStats();
        if (stats == nullptr)
            return false;

        // 幂等：已解锁直接返回，不重复 StoreStats（避免无谓的上报与日志）。
        bool alreadyAchieved = false;
        if (stats->GetAchievement (apiName, &alreadyAchieved) && alreadyAchieved)
            return true;

        if (! stats->SetAchievement (apiName))
            return false;

        // 必须 StoreStats，否则只留在本地缓存、Steam 客户端不会弹通知。
        if (! stats->StoreStats())
            return false;

        return true;
    }

    void steamIndicateProgress (const char* apiName, int cur, int max)
    {
        if (apiName == nullptr || ! g_ready.load (std::memory_order_acquire))
            return;

        if (auto* stats = SteamUserStats())
            stats->IndicateAchievementProgress (apiName,
                                                (unsigned int) (cur > 0 ? cur : 0),
                                                (unsigned int) (max > 1 ? max : 1));
    }

    bool steamIsUnlocked (const char* apiName)
    {
        if (apiName == nullptr || ! g_ready.load (std::memory_order_acquire))
            return false;

        auto* stats = SteamUserStats();
        if (stats == nullptr)
            return false;

        bool achieved = false;
        return stats->GetAchievement (apiName, &achieved) && achieved;
    }

    bool steamRestartRequested()
    {
        return g_restartRequest.load (std::memory_order_acquire);
    }

    const Backend g_backend
    {
        &steamInitialise,
        &steamShutdown,
        &steamTick,
        &steamAvailable,
        &steamUnlock,
        &steamIndicateProgress,
        &steamIsUnlocked,
        &steamRestartRequested
    };

    // 静态注册：保证 main() 之前后端就已就位（早于 JUCEApplication::initialise）。
    struct BackendRegistrar
    {
        BackendRegistrar()  { detail::installBackend (&g_backend); }
        ~BackendRegistrar() { detail::installBackend (nullptr); }
    };

    BackendRegistrar g_registrar;
}

} // namespace y2k::steam

#endif // Y2K_STEAM
