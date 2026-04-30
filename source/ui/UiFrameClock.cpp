#include "UiFrameClock.h"

#include <algorithm>
#include <chrono>

namespace y2k
{

UiFrameClock& UiFrameClock::instance()
{
    static UiFrameClock inst;
    return inst;
}

UiFrameClock::UiFrameClock()
{
    // 默认以 30Hz 起步，既能满足大多数仪表刷新，也给后续动态升到 60Hz 留空间。
    applyEffectiveFps (30);
}

UiFrameClock::~UiFrameClock()
{
    stopTimer();
}

void UiFrameClock::setTargetFps (int hz)
{
    hz = juce::jlimit (10, 120, hz);
    targetFps.store (hz, std::memory_order_relaxed);

    // 自适应关闭时，目标值即生效值；自适应开启时，上调不会立即 bump，
    // 以免刚降频又被用户覆盖，等下一轮自适应回升决策。
    if (! adaptiveEnabled.load (std::memory_order_relaxed))
        applyEffectiveFps (hz);
    else if (hz < effectiveFps.load (std::memory_order_relaxed))
        applyEffectiveFps (hz);
}

UiFrameClock::Subscription UiFrameClock::subscribe (Callback cb)
{
    if (! cb)
        return {};

    const Token token = nextToken.fetch_add (1, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lk (entriesMutex);
        entries.push_back ({ token, std::move (cb) });
    }

    return Subscription (this, token);
}

void UiFrameClock::unsubscribe (Token token)
{
    std::lock_guard<std::mutex> lk (entriesMutex);
    entries.erase (std::remove_if (entries.begin(), entries.end(),
                                   [token] (const Entry& e) { return e.token == token; }),
                   entries.end());
}

void UiFrameClock::requestImmediateTick()
{
    // 下一次 timerCallback 由 juce::Timer 内部调度；这里不强制插入消息，
    // 只将计时器起点推到"立刻"：重新 startTimer 会重置剩余等待时间。
    const int hz = effectiveFps.load (std::memory_order_relaxed);
    if (hz > 0)
        startTimerHz (hz);
}

void UiFrameClock::applyEffectiveFps (int hz)
{
    hz = juce::jlimit (10, 120, hz);
    effectiveFps.store (hz, std::memory_order_relaxed);
    startTimerHz (hz);
}

void UiFrameClock::timerCallback()
{
    const auto frameStart = std::chrono::steady_clock::now();
    const int hzNow       = effectiveFps.load (std::memory_order_relaxed);
    const int frameBudgetUs = hzNow > 0 ? (1'000'000 / hzNow) : 16'666;

    // 拷贝出回调快照，避免回调里再次 subscribe/unsubscribe 引发迭代器失效。
    std::vector<Entry> snapshot;
    {
        std::lock_guard<std::mutex> lk (entriesMutex);
        snapshot = entries;
    }

    for (auto& e : snapshot)
    {
        if (e.cb)
        {
            try { e.cb(); }
            catch (...) { /* 单个订阅者异常不应拖垮节拍器 */ }
        }
    }

    if (! adaptiveEnabled.load (std::memory_order_relaxed))
        return;

    const auto frameEnd = std::chrono::steady_clock::now();
    const auto elapsedUs = (int) std::chrono::duration_cast<std::chrono::microseconds> (frameEnd - frameStart).count();

    // 使用双阈值：超过 80% 预算算"紧张"，低于 40% 算"富裕"。
    const int tightUs = (int) (frameBudgetUs * 0.80);
    const int looseUs = (int) (frameBudgetUs * 0.40);

    if (elapsedUs >= tightUs)
    {
        ++overBudgetStreak;
        underBudgetStreak = 0;

        // 连续 6 帧紧张 → 降一档（60→30→20→15→10）。
        if (overBudgetStreak >= 6)
        {
            overBudgetStreak = 0;
            int next = hzNow;
            if (hzNow > 60)      next = 60;
            else if (hzNow > 30) next = 30;
            else if (hzNow > 20) next = 20;
            else if (hzNow > 15) next = 15;
            else                 next = 10;

            if (next != hzNow)
                applyEffectiveFps (next);
        }
    }
    else if (elapsedUs <= looseUs)
    {
        ++underBudgetStreak;
        overBudgetStreak = 0;

        // 连续 60 帧都很富裕 → 尝试回升，但不超过目标值。
        if (underBudgetStreak >= 60)
        {
            underBudgetStreak = 0;
            const int tgt = targetFps.load (std::memory_order_relaxed);
            if (hzNow < tgt)
            {
                int next = hzNow;
                if (hzNow < 20)      next = 20;
                else if (hzNow < 30) next = 30;
                else if (hzNow < 60) next = 60;
                else                 next = tgt;

                next = std::min (next, tgt);
                if (next != hzNow)
                    applyEffectiveFps (next);
            }
        }
    }
    else
    {
        overBudgetStreak  = 0;
        underBudgetStreak = 0;
    }
}

} // namespace y2k
