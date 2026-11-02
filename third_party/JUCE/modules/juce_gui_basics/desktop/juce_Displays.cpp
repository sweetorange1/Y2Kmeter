/*
  ==============================================================================

   This file is part of the JUCE framework.
   Copyright (c) Raw Material Software Limited

   JUCE is an open source framework subject to commercial or open source
   licensing.

   By downloading, installing, or using the JUCE framework, or combining the
   JUCE framework with any other source code, object code, content or any other
   copyrightable work, you agree to the terms of the JUCE End User Licence
   Agreement, and all incorporated terms including the JUCE Privacy Policy and
   the JUCE Website Terms of Service, as applicable, which will bind you. If you
   do not agree to the terms of these agreements, we will not license the JUCE
   framework to you, and you must discontinue the installation or download
   process and cease use of the JUCE framework.

   JUCE End User Licence Agreement: https://juce.com/legal/juce-8-licence/
   JUCE Privacy Policy: https://juce.com/juce-privacy-policy
   JUCE Website Terms of Service: https://juce.com/juce-website-terms-of-service/

   Or:

   You may also use this code under the terms of the AGPLv3:
   https://www.gnu.org/licenses/agpl-3.0.en.html

   THE JUCE FRAMEWORK IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL
   WARRANTIES, WHETHER EXPRESSED OR IMPLIED, INCLUDING WARRANTY OF
   MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, ARE DISCLAIMED.

  ==============================================================================
*/

namespace juce
{

Displays::Displays (const Desktop& desktop)
{
    init (desktop);
}

void Displays::init (const Desktop& desktop)
{
    findDisplays (desktop);
}

const Displays::Display* Displays::getDisplayForRect (Rectangle<int> rect, bool isPhysical) const noexcept
{
    int maxArea = -1;
    const Display* foundDisplay = nullptr;

    for (auto& display : displays)
    {
        auto displayArea = display.totalArea;

        if (isPhysical)
            displayArea = (displayArea.withZeroOrigin() * display.scale) + display.topLeftPhysical;

        displayArea = displayArea.getIntersection (rect);
        auto area = displayArea.getWidth() * displayArea.getHeight();

        if (area >= maxArea)
        {
            maxArea = area;
            foundDisplay = &display;
        }
    }

    return foundDisplay;
}

const Displays::Display* Displays::getDisplayForPoint (Point<int> point, bool isPhysical) const noexcept
{
    auto minDistance = std::numeric_limits<int>::max();
    const Display* foundDisplay = nullptr;

    for (auto& display : displays)
    {
        auto displayArea = display.totalArea;

        if (isPhysical)
            displayArea = (displayArea.withZeroOrigin() * display.scale) + display.topLeftPhysical;

        if (displayArea.contains (point))
            return &display;

        auto distance = displayArea.getCentre().getDistanceFrom (point);

        if (distance <= minDistance)
        {
            minDistance = distance;
            foundDisplay = &display;
        }
    }

    return foundDisplay;
}

Rectangle<int> Displays::physicalToLogical (Rectangle<int> rect, const Display* useScaleFactorOfDisplay) const noexcept
{
    return physicalToLogical (rect.toFloat(), useScaleFactorOfDisplay).toNearestInt();
}

Rectangle<float> Displays::physicalToLogical (Rectangle<float> rect, const Display* useScaleFactorOfDisplay) const noexcept
{
    const auto* display = useScaleFactorOfDisplay != nullptr ? useScaleFactorOfDisplay
                                                             : getDisplayForRect (rect.toNearestInt(), true);

    if (display == nullptr)
        return rect;

    auto globalScale = Desktop::getInstance().getGlobalScaleFactor();

    return ((rect - display->topLeftPhysical.toFloat()) / (display->scale / globalScale))
            + (display->totalArea.getTopLeft().toFloat() * globalScale);
}

Rectangle<int> Displays::logicalToPhysical (Rectangle<int> rect, const Display* useScaleFactorOfDisplay) const noexcept
{
    return logicalToPhysical (rect.toFloat(), useScaleFactorOfDisplay).toNearestInt();
}

Rectangle<float> Displays::logicalToPhysical (Rectangle<float> rect, const Display* useScaleFactorOfDisplay) const noexcept
{
    const auto* display = useScaleFactorOfDisplay != nullptr ? useScaleFactorOfDisplay
                                                             : getDisplayForRect (rect.toNearestInt(), false);

    if (display == nullptr)
        return rect;

    auto globalScale = Desktop::getInstance().getGlobalScaleFactor();

    return ((rect.toFloat() - (display->totalArea.getTopLeft().toFloat() * globalScale)) * (display->scale / globalScale))
             + display->topLeftPhysical.toFloat();
}

template <typename ValueType>
Point<ValueType> Displays::physicalToLogical (Point<ValueType> point, const Display* useScaleFactorOfDisplay) const noexcept
{
    const auto* display = useScaleFactorOfDisplay != nullptr ? useScaleFactorOfDisplay
                                                             : getDisplayForPoint (point.roundToInt(), true);

    if (display == nullptr)
        return point;

    auto globalScale = Desktop::getInstance().getGlobalScaleFactor();

    Point<ValueType> logicalTopLeft  (static_cast<ValueType> (display->totalArea.getX()),       static_cast<ValueType> (display->totalArea.getY()));
    Point<ValueType> physicalTopLeft (static_cast<ValueType> (display->topLeftPhysical.getX()), static_cast<ValueType> (display->topLeftPhysical.getY()));

    return ((point - physicalTopLeft) / (display->scale / globalScale)) + (logicalTopLeft * globalScale);
}

template <typename ValueType>
Point<ValueType> Displays::logicalToPhysical (Point<ValueType> point, const Display* useScaleFactorOfDisplay)  const noexcept
{
    const auto* display = useScaleFactorOfDisplay != nullptr ? useScaleFactorOfDisplay
                                                             : getDisplayForPoint (point.roundToInt(), false);

    if (display == nullptr)
        return point;

    auto globalScale = Desktop::getInstance().getGlobalScaleFactor();

    Point<ValueType> logicalTopLeft  (static_cast<ValueType> (display->totalArea.getX()),       static_cast<ValueType> (display->totalArea.getY()));
    Point<ValueType> physicalTopLeft (static_cast<ValueType> (display->topLeftPhysical.getX()), static_cast<ValueType> (display->topLeftPhysical.getY()));

    return ((point - (logicalTopLeft * globalScale)) * (display->scale / globalScale)) + physicalTopLeft;
}

const Displays::Display* Displays::getPrimaryDisplay() const noexcept
{
    JUCE_ASSERT_MESSAGE_MANAGER_IS_LOCKED

    const auto iter = std::find_if (displays.begin(), displays.end(), [] (auto& d) { return d.isMain; });
    return iter != displays.end() ? iter : nullptr;
}

RectangleList<int> Displays::getRectangleList (bool userAreasOnly) const
{
    JUCE_ASSERT_MESSAGE_MANAGER_IS_LOCKED
    RectangleList<int> rl;

    for (auto& d : displays)
        rl.addWithoutMerging (userAreasOnly ? d.userArea : d.totalArea);

    return rl;
}

Rectangle<int> Displays::getTotalBounds (bool userAreasOnly) const
{
    return getRectangleList (userAreasOnly).getBounds();
}

void Displays::refresh()
{
    Array<Display> oldDisplays;
    oldDisplays.swapWith (displays);

    init (Desktop::getInstance());

    if (oldDisplays != displays)
    {
        for (auto i = ComponentPeer::getNumPeers(); --i >= 0;)
            if (auto* peer = ComponentPeer::getPeer (i))
                peer->handleScreenSizeChange();
    }
}

static auto tie (const Displays::Display& d)
{
    return std::tie (d.dpi,
                     d.isMain,
                     d.keyboardInsets,
                     d.safeAreaInsets,
                     d.scale,
                     d.topLeftPhysical,
                     d.totalArea,
                     d.userArea);
}

static bool operator== (const Displays::Display& d1, const Displays::Display& d2) noexcept
{
    return tie (d1) == tie (d2);
}

//==============================================================================
// These methods are used for converting the totalArea and userArea Rectangles in Display from physical to logical
// pixels. We do this by constructing a graph of connected displays where the root node has position (0, 0); this can be
// safely converted to logical pixels using its scale factor and we can then traverse the graph and work out the logical pixels
// for all the other connected displays. We need to do this as the logical bounds of a display depend not only on its scale
// factor but also the scale factor of the displays connected to it.

/**
    Represents a node in our graph of displays.
*/
struct DisplayNode
{
    /** The Display object that this represents. */
    Displays::Display* display;

    /** True if this represents the 'root' display with position (0, 0). */
    bool isRoot = false;

    /** The parent node of this node in our display graph. This will have a correct logicalArea. */
    DisplayNode* parent = nullptr;

    /** The logical area to be calculated. This will be valid after processDisplay() has
        been called on this node.
    */
    Rectangle<double> logicalArea;
};

/** Y2K patch：两块物理矩形之间的边缘距离平方（重叠时为 0）。
    Windows 走 updateToLogical 的逐屏换算分支后本函数不再被引用，故标记 maybe_unused。 */
[[maybe_unused]] static double displayEdgeDistanceSquared (const Rectangle<double>& a, const Rectangle<double>& b) noexcept
{
    const auto dx = jmax (0.0, jmax (a.getX() - b.getRight(), b.getX() - a.getRight()));
    const auto dy = jmax (0.0, jmax (a.getY() - b.getBottom(), b.getY() - a.getBottom()));
    return dx * dx + dy * dy;
}

/** Recursive - will calculate and set the logicalArea member of current.

    Y2K patch（混合 DPI 多屏）：
      原实现只在"物理矩形精确相接"时才把子屏挂到父屏上，并且 on top / on bottom
      分支的前导坐标用 `physical / parentScale`（假设整张桌面共享父屏的缩放）。
      两个后果：
        · 混合 DPI 桌面里，DIP 相邻的两块屏在物理坐标中必然出现空洞（Windows 按
          各屏自己的缩放换算物理矩形），精确相接判定失败 → 子屏永远挂不上父屏，
          其 logicalArea 保持 (0,0,0,0) → 该屏的逻辑坐标整体失效（窗口 setBounds、
          getDisplayForRect、弹窗/菜单/提示全部错位）；
        · 子屏与父屏缩放不同时，逻辑 x 会被 parentScale 拉偏，上下排布的两块屏在
          逻辑空间里横向错开。
      改为：
        1) 精确相接优先（非混合 DPI 桌面与原实现完全等效）；
        2) 找不到相接邻居时按"最近边缘"挂接，保证不留下孤立屏；
        3) 前导轴坐标改为「父屏逻辑原点 + 物理偏移 / 父屏 scale」，物理上同 x 的
           上下两块屏在逻辑空间里也保持同 x。 */
[[maybe_unused]] static void processDisplay (DisplayNode* currentNode, Array<DisplayNode>& allNodes)
{
    const auto physicalArea = currentNode->display->totalArea.toDouble();
    const auto scale = currentNode->display->scale;

    if (! currentNode->isRoot)
    {
        const auto logicalWidth  = physicalArea.getWidth() / scale;
        const auto logicalHeight = physicalArea.getHeight() / scale;

        const auto physicalParentArea = currentNode->parent->display->totalArea.toDouble();
        const auto logicalParentArea  = currentNode->parent->logicalArea; // logical area of parent has already been calculated
        const auto parentScale        = currentNode->parent->display->scale;

        const auto offsetX = (physicalArea.getX() - physicalParentArea.getX()) / parentScale;
        const auto offsetY = (physicalArea.getY() - physicalParentArea.getY()) / parentScale;

        Rectangle<double> logicalArea (0.0, 0.0, logicalWidth, logicalHeight);

        if      (approximatelyEqual (physicalArea.getRight(), physicalParentArea.getX()))     logicalArea.setPosition ({ logicalParentArea.getX() - logicalWidth, logicalParentArea.getY() + offsetY });  // on left
        else if (approximatelyEqual (physicalArea.getX(), physicalParentArea.getRight()))     logicalArea.setPosition ({ logicalParentArea.getRight(),  logicalParentArea.getY() + offsetY });            // on right
        else if (approximatelyEqual (physicalArea.getBottom(), physicalParentArea.getY()))    logicalArea.setPosition ({ logicalParentArea.getX() + offsetX, logicalParentArea.getY() - logicalHeight }); // on top
        else if (approximatelyEqual (physicalArea.getY(), physicalParentArea.getBottom()))    logicalArea.setPosition ({ logicalParentArea.getX() + offsetX, logicalParentArea.getBottom() });            // on bottom
        else
        {
            // 兜底：物理矩形之间有空洞。按中心相对方向贴到父屏对应边（保证逻辑空间
            // 不重叠），另一轴保留真实物理偏移。绝不能让 logicalArea 停在 (0,0,0,0)。
            const auto myCentre     = physicalArea.getCentre();
            const auto parentCentre = physicalParentArea.getCentre();

            if (std::abs (myCentre.y - parentCentre.y) >= std::abs (myCentre.x - parentCentre.x))
            {
                if (myCentre.y < parentCentre.y)
                    logicalArea.setPosition ({ logicalParentArea.getX() + offsetX, logicalParentArea.getY() - logicalHeight });
                else
                    logicalArea.setPosition ({ logicalParentArea.getX() + offsetX, logicalParentArea.getBottom() });
            }
            else
            {
                if (myCentre.x < parentCentre.x)
                    logicalArea.setPosition ({ logicalParentArea.getX() - logicalWidth, logicalParentArea.getY() + offsetY });
                else
                    logicalArea.setPosition ({ logicalParentArea.getRight(), logicalParentArea.getY() + offsetY });
            }
        }

        currentNode->logicalArea = logicalArea;
    }
    else
    {
        // If currentNode is the root (position (0, 0)) then we can just scale the physical area
        currentNode->logicalArea = physicalArea / scale;
        currentNode->parent = currentNode;
    }

    // Find child nodes
    Array<DisplayNode*> children;
    for (auto& node : allNodes)
    {
        // Already calculated
        if (node.parent != nullptr)
            continue;

        const auto otherPhysicalArea = node.display->totalArea.toDouble();

        // If the displays are touching on any side
        if (approximatelyEqual (otherPhysicalArea.getX(), physicalArea.getRight())  || approximatelyEqual (otherPhysicalArea.getRight(),  physicalArea.getX())
         || approximatelyEqual (otherPhysicalArea.getY(), physicalArea.getBottom()) || approximatelyEqual (otherPhysicalArea.getBottom(), physicalArea.getY()))
        {
            node.parent = currentNode;
            children.add (&node);
        }
    }

    // Y2K patch：混合 DPI 桌面里物理矩形可能互不相接（DIP 相邻、物理有空洞）。
    //   此时若一个子屏都找不到，就把"最近的未分配屏"接上，否则它会成为孤立节点，
    //   logicalArea 恒为 (0,0,0,0)（调用方随后 jassert + 整张逻辑表失效）。
    if (children.isEmpty())
    {
        DisplayNode* nearest = nullptr;
        auto bestDistance = std::numeric_limits<double>::max();

        for (auto& node : allNodes)
        {
            if (node.parent != nullptr)
                continue;

            const auto distance = displayEdgeDistanceSquared (physicalArea, node.display->totalArea.toDouble());

            if (distance < bestDistance)
            {
                bestDistance = distance;
                nearest = &node;
            }
        }

        if (nearest != nullptr)
        {
            nearest->parent = currentNode;
            children.add (nearest);
        }
    }

    // Recursively process all child nodes
    for (auto child : children)
        processDisplay (child, allNodes);
}

/** This is called when the displays Array has been filled out with the info for all connected displays and the
    totalArea and userArea Rectangles need to be converted from physical to logical coordinates.
*/
void Displays::updateToLogical()
{
    if (displays.size() == 1)
    {
        auto& display = displays.getReference (0);

        display.totalArea = (display.totalArea.toDouble() / display.scale).toNearestInt();
        display.userArea  = (display.userArea.toDouble()  / display.scale).toNearestInt();

        return;
    }

   #if JUCE_WINDOWS
    // Y2K patch（Windows 混合 DPI 多屏）：
    //   Windows 的 GetMonitorInfo 返回的物理矩形本身就是"该屏 DIP 矩形 × 该屏 scale"，
    //   所以逐屏 `physical / ownScale` 得到的就是与系统显示设置完全一致、且各屏
    //   严格相邻的 DIP 布局（这也正是"物理桌面有空洞"的来源：DIP 相邻的两块不同
    //   缩放的屏，物理坐标必然不连续）。
    //   原来的"图遍历 + 物理矩形是否精确相接"在混合 DPI 桌面里必然判错：
    //     · 物理矩形有空洞 → 子屏挂不上父屏 → logicalArea 保持 (0,0,0,0)，该屏
    //       逻辑坐标整体失效（窗口/弹窗/菜单错位）；
    //     · 前导轴用 `physical / parentScale` → 与子屏自身 scale 不一致，上下排布
    //       的两块屏在逻辑空间里横向错开。
    //   逐屏换算同时保证 physical→logical 在中缝两侧连续（对滚动/拖动至关重要）。
    for (auto& display : displays)
    {
        const auto relativeUserArea = (display.userArea.toDouble()
                                       - display.totalArea.toDouble().getTopLeft()) / display.scale;

        display.topLeftPhysical = display.totalArea.getTopLeft();
        display.totalArea       = (display.totalArea.toDouble() / display.scale).toNearestInt();
        display.userArea        = (relativeUserArea + display.totalArea.toDouble().getTopLeft()).toNearestInt();
    }
   #else

    Array<DisplayNode> displayNodes;

    for (auto& d : displays)
    {
        DisplayNode node;

        node.display = &d;

        if (d.totalArea.getTopLeft() == Point<int>())
            node.isRoot = true;

        displayNodes.add (node);
    }

    auto* root = [&displayNodes]() -> DisplayNode*
    {
        for (auto& node : displayNodes)
            if (node.isRoot)
                return &node;

        auto minDistance = std::numeric_limits<int>::max();
        DisplayNode* retVal = nullptr;

        for (auto& node : displayNodes)
        {
            auto distance = node.display->totalArea.getTopLeft().getDistanceFrom ({});

            if (distance < minDistance)
            {
                minDistance = distance;
                retVal = &node;
            }
        }

        if (retVal != nullptr)
            retVal->isRoot = true;

        return retVal;
    }();

    // Must have a root node!
    jassert (root != nullptr);

    // Recursively traverse the display graph from the root and work out logical bounds
    processDisplay (root, displayNodes);

    for (auto& node : displayNodes)
    {
        // All of the nodes should have a parent
        jassert (node.parent != nullptr);

        auto relativeUserArea = (node.display->userArea.toDouble() - node.display->totalArea.toDouble().getTopLeft()) / node.display->scale;

        // Now set Display::totalArea and ::userArea using the logical area that we have calculated
        node.display->topLeftPhysical = node.display->totalArea.getTopLeft();
        node.display->totalArea       = node.logicalArea.toNearestInt();
        node.display->userArea        = (relativeUserArea + node.logicalArea.getTopLeft()).toNearestInt();
    }
   #endif
}

/** @cond */
// explicit template instantiations
template Point<int>   Displays::physicalToLogical (Point<int>,   const Display*) const noexcept;
template Point<float> Displays::physicalToLogical (Point<float>, const Display*) const noexcept;

template Point<int>   Displays::logicalToPhysical (Point<int>,   const Display*) const noexcept;
template Point<float> Displays::logicalToPhysical (Point<float>, const Display*) const noexcept;
/** @endcond */

//==============================================================================
// Deprecated methods
const Displays::Display& Displays::getDisplayContaining (Point<int> position) const noexcept
{
    JUCE_ASSERT_MESSAGE_MANAGER_IS_LOCKED
    const auto* best = &displays.getReference (0);
    auto bestDistance = std::numeric_limits<int>::max();

    for (auto& d : displays)
    {
        if (d.totalArea.contains (position))
        {
            best = &d;
            break;
        }

        auto distance = d.totalArea.getCentre().getDistanceFrom (position);

        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = &d;
        }
    }

    return *best;
}

const Displays::Display& Displays::findDisplayForRect (Rectangle<int> rect, bool isPhysical) const noexcept
{
    if (auto* display = getDisplayForRect (rect, isPhysical))
        return *display;

    return emptyDisplay;
}

const Displays::Display& Displays::findDisplayForPoint (Point<int> point, bool isPhysical) const noexcept
{
    if (auto* display = getDisplayForPoint (point, isPhysical))
        return *display;

    return emptyDisplay;
}

const Displays::Display& Displays::getMainDisplay() const noexcept
{
    if (auto* display = getPrimaryDisplay())
        return *display;

    return emptyDisplay;
}

} // namespace juce
