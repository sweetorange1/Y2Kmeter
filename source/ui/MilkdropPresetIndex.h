#pragma once

#include <cstddef>
#include <string>
#include <vector>

// ==========================================================
// MilkdropPresetIndex —— Pure C++17 async preset index reconciliation
//
// Zero JUCE dependencies so it can be directly unit-tested by
// Y2Kmeter_perf_pacing_test and shared with PluginEditor.
//
// Used when MilkDrop boots immediately with a fast-path initial preset
// while the full 9,921-preset directory scan completes on a background
// thread: reconciles the already-loaded preset path to its exact sorted
// index and applies any user jump/delta/random requests queued in flight.
// ==========================================================
namespace y2k::milkdrop
{

inline char asciiToLower (char c) noexcept
{
    if (c >= 'A' && c <= 'Z')
        return static_cast<char> (c - 'A' + 'a');
    return c;
}

inline char normalizePathChar (char c) noexcept
{
    return (c == '\\') ? '/' : asciiToLower (c);
}

inline bool pathsEqualIgnoreCase (const std::string& a, const std::string& b) noexcept
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (normalizePathChar (a[i]) != normalizePathChar (b[i]))
            return false;
    return true;
}

inline int findPresetPathIndex (const std::vector<std::string>& sortedPaths,
                                const std::string& targetPath) noexcept
{
    if (targetPath.empty())
        return -1;

    for (std::size_t i = 0; i < sortedPaths.size(); ++i)
        if (pathsEqualIgnoreCase (sortedPaths[i], targetPath))
            return static_cast<int> (i);

    return -1;
}

inline int wrapPresetIndex (int current, int delta, int count) noexcept
{
    if (count <= 0)
        return -1;
    const int base = (current >= 0 && current < count) ? current : 0;
    int next = (base + delta) % count;
    if (next < 0)
        next += count;
    return next;
}

struct PresetReconcileResult
{
    int  index        = -1;
    bool shouldReload = false;
};

inline PresetReconcileResult reconcileLoadedPresetIndex (
    const std::vector<std::string>& sortedPaths,
    const std::string& loadedPath,
    int pendingJumpIndex,
    int pendingDelta,
    bool pendingRandom,
    std::size_t randomSeed = 0) noexcept
{
    const int count = static_cast<int> (sortedPaths.size());
    if (count <= 0)
        return { -1, false };

    const int currentIdx = findPresetPathIndex (sortedPaths, loadedPath);

    if (pendingJumpIndex >= 0 && pendingJumpIndex < count)
        return { pendingJumpIndex, pendingJumpIndex != currentIdx };

    if (pendingRandom)
    {
        if (count == 1)
            return { 0, currentIdx != 0 };

        const int span = (currentIdx >= 0) ? (count - 1) : count;
        int next = static_cast<int> (randomSeed % static_cast<std::size_t> (span));
        if (currentIdx >= 0 && next >= currentIdx)
            ++next;
        return { next, true };
    }

    if (pendingDelta != 0)
    {
        const int next = wrapPresetIndex (currentIdx, pendingDelta, count);
        return { next, (currentIdx < 0) || (next != currentIdx) };
    }

    if (currentIdx >= 0)
        return { currentIdx, false };

    return { 0, true };
}

} // namespace y2k::milkdrop
