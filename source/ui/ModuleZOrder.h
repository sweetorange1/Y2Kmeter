#pragma once

// ==========================================================
// ModuleZOrder —— Pure C++17 Z-Order No-Op Guard Predicate
//                 (ZERO JUCE dependencies; directly testable).
//
//   In ModuleWorkspace, Perler stickers, VirtuPet, hideBtn,
//   and toolbar overlays always live AFTER regular ModulePanels
//   in childComponentList. Checking `childList.getLast() != panel`
//   is therefore ALWAYS true, causing JUCE Component::toFront()
//   to call reorderChildInternal() -> repaintParent() +
//   sendFakeMouseMove() on EVERY repeated click to the same
//   already-frontmost module.
//
//   This predicate determines whether a regular ModulePanel
//   actually has any other regular ModulePanel stacked above it.
// ==========================================================

#include <cstddef>
#include <cstdint>

namespace y2k::ui
{
    enum class WorkspaceChildRole : uint8_t
    {
        regularModule    = 0,
        overlayOrTopmost = 1,
        other            = 2
    };

    inline bool regularModuleNeedsRaise (const WorkspaceChildRole* roles,
                                         size_t count,
                                         size_t targetIndex) noexcept
    {
        if (roles == nullptr || targetIndex >= count)
            return false;
        if (roles[targetIndex] != WorkspaceChildRole::regularModule)
            return false;

        for (size_t i = targetIndex + 1; i < count; ++i)
        {
            if (roles[i] == WorkspaceChildRole::regularModule)
                return true;
        }
        return false;
    }
}
