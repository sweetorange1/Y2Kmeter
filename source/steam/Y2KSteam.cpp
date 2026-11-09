// ==========================================================
// Y2KSteam.cpp
//   y2k::steam 的调度层：只持有后端函数表，不做任何 Steamworks 调用。
//
//   ★ 本文件会被编进共享代码静态库（Y2Kmeter / Y2Kmeter_milkdrop 都用它），
//     因此这里**绝对不能**出现 steam/*.h、SteamAPI_* 或任何 Steam 类型，
//     否则默认构建的 exe 就会带上 steam_api64.dll 依赖（违反需求书 C1）。
// ==========================================================

#include "source/steam/Y2KSteam.h"

#include <atomic>

namespace y2k::steam
{

namespace
{
    // 常量初始化（nullptr）+ 原子访问：静态初始化顺序无关，后端 TU 的
    //   静态初始化无论先后都不会踩到未构造的全局对象。
    std::atomic<const Backend*>& slot() noexcept
    {
        static std::atomic<const Backend*> s { nullptr };
        return s;
    }

    const Backend* activeBackend() noexcept
    {
        return slot().load (std::memory_order_acquire);
    }
}

namespace detail
{
    void installBackend (const Backend* backend) noexcept
    {
        slot().store (backend, std::memory_order_release);
    }

    const Backend* backend() noexcept
    {
        return activeBackend();
    }
}

bool initialise()
{
    const auto* b = activeBackend();
    if (b == nullptr || b->initialise == nullptr)
        return false;

    return b->initialise();
}

void shutdown()
{
    const auto* b = activeBackend();
    if (b != nullptr && b->shutdown != nullptr)
        b->shutdown();
}

void tick()
{
    const auto* b = activeBackend();
    if (b != nullptr && b->tick != nullptr)
        b->tick();
}

bool available()
{
    const auto* b = activeBackend();
    return b != nullptr && b->available != nullptr && b->available();
}

bool backendLinked()
{
    return activeBackend() != nullptr;
}

bool restartRequested()
{
    const auto* b = activeBackend();
    return b != nullptr && b->restartRequested != nullptr && b->restartRequested();
}

bool unlock (const char* apiName)
{
    if (apiName == nullptr)
        return false;

    const auto* b = activeBackend();
    if (b == nullptr || b->unlock == nullptr)
        return false;

    return b->unlock (apiName);
}

void indicateProgress (const char* apiName, int cur, int max)
{
    if (apiName == nullptr)
        return;

    const auto* b = activeBackend();
    if (b != nullptr && b->indicateProgress != nullptr)
        b->indicateProgress (apiName, cur, max);
}

bool isUnlocked (const char* apiName)
{
    if (apiName == nullptr)
        return false;

    const auto* b = activeBackend();
    if (b == nullptr || b->isUnlocked == nullptr)
        return false;

    return b->isUnlocked (apiName);
}

} // namespace y2k::steam
