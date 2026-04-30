#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <functional>
#include <mutex>
#include <vector>
#include <atomic>

namespace y2k
{

// =============================================================
// UiFrameClock
// -------------------------------------------------------------
// 统一的 UI 帧时钟节拍器（阶段1性能改造）：
//   · 各模块不再自行 startTimerHz() 各自调度 repaint，
//     而是向 UiFrameClock 注册一个回调，由它统一在 UI 线程按
//     目标帧率触发。
//   · 支持自适应降频：当帧回调耗时持续超过预算时，会临时把
//     实际刷新率降档（60 → 30 → 20），系统负载降低后自动恢复。
//   · 仅在 UI 线程使用（依赖 juce::Timer）。跨线程请先 MessageManager。
// -------------------------------------------------------------
// 用法示例：
//   token = UiFrameClock::instance().subscribe([this]{ this->repaint(); });
//   // 析构时自动 unsubscribe（Subscription 持有 RAII 句柄）
// =============================================================
class UiFrameClock : private juce::Timer
{
public:
    using Callback = std::function<void()>;
    using Token    = std::uint64_t;

    // RAII 订阅句柄：析构时自动取消注册，避免悬空回调。
    class Subscription
    {
    public:
        Subscription() = default;
        Subscription (UiFrameClock* clk, Token tk) noexcept : clock (clk), token (tk) {}

        Subscription (const Subscription&) = delete;
        Subscription& operator= (const Subscription&) = delete;

        Subscription (Subscription&& other) noexcept
            : clock (other.clock), token (other.token)
        {
            other.clock = nullptr;
            other.token = 0;
        }

        Subscription& operator= (Subscription&& other) noexcept
        {
            reset();
            clock = other.clock;
            token = other.token;
            other.clock = nullptr;
            other.token = 0;
            return *this;
        }

        ~Subscription() { reset(); }

        void reset() noexcept
        {
            if (clock != nullptr && token != 0)
                clock->unsubscribe (token);
            clock = nullptr;
            token = 0;
        }

        bool isActive() const noexcept { return clock != nullptr && token != 0; }

    private:
        UiFrameClock* clock = nullptr;
        Token         token = 0;
    };

    static UiFrameClock& instance();

    // 目标帧率（上限），实际刷新会受自适应降频影响。
    void setTargetFps (int hz);
    int  getTargetFps() const noexcept { return targetFps.load (std::memory_order_relaxed); }

    // 当前正在使用的实际帧率（供 UI 显示 & 调试）。
    int  getEffectiveFps() const noexcept { return effectiveFps.load (std::memory_order_relaxed); }

    // 开启/关闭自适应降频。默认开启。
    void setAdaptiveEnabled (bool enabled) noexcept { adaptiveEnabled.store (enabled, std::memory_order_relaxed); }

    // 订阅帧回调。返回 RAII 句柄，析构即反注册。
    [[nodiscard]] Subscription subscribe (Callback cb);

    // 手动反注册（一般不直接调用，用 Subscription 就够了）。
    void unsubscribe (Token token);

    // 立即请求下一帧触发（不会改变周期，只是"不要等满"）。
    void requestImmediateTick();

private:
    UiFrameClock();
    ~UiFrameClock() override;

    void timerCallback() override;

    void applyEffectiveFps (int hz);

    struct Entry
    {
        Token    token = 0;
        Callback cb;
    };

    std::mutex          entriesMutex;
    std::vector<Entry>  entries;
    std::atomic<Token>  nextToken { 1 };

    std::atomic<int>    targetFps    { 30 };
    std::atomic<int>    effectiveFps { 30 };
    std::atomic<bool>   adaptiveEnabled { true };

    // 自适应状态：连续 N 帧超预算 → 降档；连续 M 帧富裕 → 回升。
    int overBudgetStreak  = 0;
    int underBudgetStreak = 0;
};

} // namespace y2k
