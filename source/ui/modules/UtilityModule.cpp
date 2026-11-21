#include "source/ui/modules/UtilityModule.h"

#include "source/ui/PinkXPStyle.h"

using y2k::util::ToolKind;

// ==========================================================
// UtilityModule（小工具模块）
// ==========================================================

namespace
{
    constexpr const char* kToolParam = "tool";
}

UtilityModule::UtilityModule()
    : ModulePanel (ModuleType::utility)
{
    setMinSize (120, 96);
    setDefaultSize (304, 288);
    setControlPanelEnabled (true);

    // 交互型模块：点击按钮 / 输入框时不要触发 Toybox 的点击彩蛋
    getProperties().set ("toy.optOut", true);

    themeToken = PinkXP::subscribeThemeChanged ([this]
    {
        if (view != nullptr)
            view->themeChanged();
        repaint();
    });

    setTool (ToolKind::calculator, true);

    // 低频交互模块：常被摆在每帧刷新的模块上方，挂位图缓存避免被"陪着"每帧
    //   整块重画（计算器 20 个按钮 + 文本，mac 上曾是 GL 线程主要热点之一）
    y2k::ui::setGlSafeImageCache (*this, true);
}

UtilityModule::~UtilityModule()
{
    if (themeToken >= 0)
        PinkXP::unsubscribeThemeChanged (themeToken);

    if (view != nullptr)
        view->onStateChanged = nullptr;
    view.reset();
}

void UtilityModule::updateTitle()
{
    setTitleText (y2k::util::toolDisplayName (tool));
}

void UtilityModule::captureCurrentToolState() const
{
    if (view == nullptr)
        return;

    const juce::Identifier name (y2k::util::toolStateName (tool));
    juce::ValueTree fresh (name);
    view->save (fresh);

    auto existing = toolStates.getChildWithName (name);
    if (existing.isValid())
        toolStates.removeChild (existing, nullptr);
    toolStates.appendChild (fresh, nullptr);
}

void UtilityModule::setTool (ToolKind kind, bool force)
{
    if (! force && kind == tool && view != nullptr)
        return;

    if (view != nullptr)
    {
        captureCurrentToolState();
        view->onStateChanged = nullptr;
        removeChildComponent (view.get());
        view.reset();
    }

    tool = kind;
    view = y2k::util::createToolView (kind);

    const auto state = toolStates.getChildWithName (y2k::util::toolStateName (kind));
    if (state.isValid())
        view->restore (state);

    view->onStateChanged = [this]
    {
        if (onParamChangedByUser)
            onParamChangedByUser (*this);
    };

    // z 序放在最底层：控制面板抽屉 / 其它子组件始终盖在工具上面
    addAndMakeVisible (*view, 0);
    layoutContent (getContentBounds());
    updateTitle();
    repaint();
}

// ----------------------------------------------------------
// 布局 / 绘制
// ----------------------------------------------------------
void UtilityModule::layoutContent (juce::Rectangle<int> contentBounds)
{
    if (view != nullptr)
        view->setBounds (contentBounds.reduced (6));
}

void UtilityModule::paintContent (juce::Graphics& g, juce::Rectangle<int> contentBounds)
{
    g.setColour (PinkXP::btnFace);
    g.fillRect (contentBounds);
}

// ----------------------------------------------------------
// 控制面板
// ----------------------------------------------------------
void UtilityModule::buildParamDefs (std::vector<ParamDef>& out) const
{
    juce::StringArray names;
    for (int i = 0; i < y2k::util::kNumTools; ++i)
        names.add (y2k::util::toolDisplayName ((ToolKind) i));

    out.push_back (ParamDef::choice (kToolParam, "Tool", names,
        "Which tool this module shows. Every tool keeps its own contents when you switch away and back.")
        .in ("Tool"));

    // 列出全部工具的参数，用 when 只显示当前工具的那几项
    for (int i = 0; i < y2k::util::kNumTools; ++i)
    {
        const auto kind = (ToolKind) i;
        std::vector<ParamDef> defs;
        y2k::util::buildToolParams (kind, defs);

        for (auto& d : defs)
        {
            d.group = y2k::util::toolDisplayName (kind);
            d.visibleIf = [this, kind] { return tool == kind; };
            out.push_back (d);
        }
    }
}

double UtilityModule::getParamValue (const juce::String& id) const
{
    if (id == kToolParam)
        return (double) (int) tool;

    double v = 0.0;
    if (view != nullptr && view->getParam (id, v))
        return v;
    return 0.0;
}

void UtilityModule::setParamValue (const juce::String& id, double v)
{
    if (id == kToolParam)
    {
        setTool ((ToolKind) juce::jlimit (0, y2k::util::kNumTools - 1, juce::roundToInt (v)));
        return;
    }

    if (view != nullptr)
        view->setParam (id, v);
}

// ----------------------------------------------------------
// 存档
//   <state tool="0"><tools><calculator .../><notes text="..."/>...</tools></state>
// ----------------------------------------------------------
juce::ValueTree UtilityModule::saveModuleSpecificState() const
{
    captureCurrentToolState();

    juce::ValueTree s ("state");
    s.setProperty (kToolParam, (int) tool, nullptr);
    s.appendChild (toolStates.createCopy(), nullptr);
    return s;
}

void UtilityModule::restoreModuleSpecificState (const juce::ValueTree& state)
{
    const auto tools = state.getChildWithName ("tools");
    if (tools.isValid())
        toolStates = tools.createCopy();

    auto kind = tool;
    if (state.hasProperty (kToolParam))
        kind = (ToolKind) juce::jlimit (0, y2k::util::kNumTools - 1, (int) state.getProperty (kToolParam));

    // 强制重建：即使工具没变，也要用存档里的内容替换当前视图
    if (view != nullptr)
    {
        view->onStateChanged = nullptr;
        removeChildComponent (view.get());
        view.reset();
    }
    setTool (kind, true);

    refreshControlPanel();
}
