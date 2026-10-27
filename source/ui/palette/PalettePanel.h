// ==========================================================
// PalettePanel.h —— 调色面板（替换旧 CustomThemePicker）
//
//   需求对应：
//     R2/R3   展开调色面板、实时作用于工作区、应用并保存/还原
//     R4      撤销/重做（≥20 步）+ A/B 对比
//     R5/R6   简单模式：色相环 + 明暗 + 氛围 + 和谐 + 随机 + 锁定
//     R7/R8/R9 高级区：语义角色行（hex + 吸管 + 自动/手动）+ 轻量取色器
//     R11-R14 我的主题（命名/重命名/删除/排序）、分享码、导入导出、
//             桌面纹理/标题图标选择、Reset
//     R15/R16 迷你预览卡 + 质量徽章
//     S6      16ms 节流实时预览
//
//   面板内所有控件走 PinkXP LookAndFeel（Y2K 像素风）。
// ==========================================================
#pragma once

#include <JuceHeader.h>
#include "source/ui/palette/ThemeSpec.h"
#include "source/ui/palette/PaletteGenerator.h"
#include <memory>
#include <vector>

// 前向声明（避免把整个 PinkXPStyle 拖进来）
namespace PinkXP
{
    class ThemeLibrary;
}

// ------------------------------------------------------------
// HueRing —— 色相环（R5）：环上颜色按"当前明暗/vibe 下的最佳 accent"渲染
// ------------------------------------------------------------
class HueRing : public juce::Component
{
public:
    HueRing();

    void paint(juce::Graphics& g) override;
    void resized() override {}
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;

    void setHue(float h)               { hue_ = y2kpal::wrapHue(h); repaint(); }
    float getHue() const noexcept      { return hue_; }
    void setContext(float vibe, int brightness) noexcept
    {
        if (vibe != vibe_ || brightness != brightness_)
            ringDirty_ = true;   // 环颜色只随 vibe/明暗变化，标脏待重建
        vibe_ = vibe;
        brightness_ = brightness;
        repaint();
    }

    std::function<void(float)> onHueChanged;          // 拖动中实时回调
    std::function<void()>      onHueDragStart;        // 按下瞬间
    std::function<void()>      onHueDragEnd;          // 松手（拖动结束提交点）

private:
    float hue_ = 330.0f;
    float vibe_ = 1.0f;
    int   brightness_ = 0;
    bool  dragging_ = false;

    // 环预渲染缓存：逐像素填充（含边界 1px 羽化），消除旧实现用 360 个独立
    //   fillPath 扇形在低物理像素密度（如 2k 屏）下因抗锯齿边缘产生的放射状
    //   "缝合线"（表现为摩尔纹）。环颜色与当前选中 hue 无关，仅 vibe/明暗变化时重建。
    juce::Image ringImage_;
    bool        ringDirty_ = true;

    void rebuildRing(int w, int h, float scale);
};

// ------------------------------------------------------------
// MiniPreviewCard —— 面板内迷你预览卡（R15）
//   数据源是面板的 lastGen_（拖动期间也实时），而非 PinkXP 全局
//   （全局只在"提交主题"时更新，会导致拖动中预览卡不动）。
// ------------------------------------------------------------
class MiniPreviewCard : public juce::Component
{
public:
    void paint(juce::Graphics& g) override;

    void setGen(const y2kpal::GeneratedTheme* gen) noexcept { gen_ = gen; }

private:
    const y2kpal::GeneratedTheme* gen_ = nullptr;
};

// ------------------------------------------------------------
// SvHuePicker —— 轻量取色器（R9）：饱和度-明度方块 + 色相条，Y2K 像素风
// ------------------------------------------------------------
class SvHuePicker : public juce::Component
{
public:
    SvHuePicker();

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;

    void setColourToEdit(juce::Colour c);
    void setCurrentColour(juce::Colour c);
    juce::Colour getCurrentColour() const { return current_; }
    std::function<void(juce::Colour)> onColourChanged;   // 拖动中实时（只更新面板内预览）
    std::function<void(juce::Colour)> onDragEnd;         // 松手（提交主题）
    std::function<void(juce::Colour)> onAccepted;        // 点 OK 时

private:
    juce::Rectangle<int> svRect_;
    juce::Rectangle<int> hueRect_;
    juce::Rectangle<int> okRect_;
    juce::Colour current_;
    float hue_ = 0.0f, sat_ = 1.0f, val_ = 1.0f;
    bool  dragging_ = false;

    void updateFromMouse(juce::Point<int> p);
    void commitColour();
};

// ------------------------------------------------------------
// RoleRow —— 高级区单角色行（R7/R8）
// ------------------------------------------------------------
class RoleRow : public juce::Component
{
public:
    RoleRow();

    void paint(juce::Graphics& g) override;
    void resized() override;

    void setup(int role,
               std::function<void(int, y2kpal::Rgb8)> onManualSet,
               std::function<void(int)> onAutoReset,
               std::function<void(int)> onPick);

    // 刷新行显示（来自 spec.overrides + 生成结果）
    void refresh(const y2kpal::ThemeSpec& spec, const y2kpal::GeneratedTheme& gen);

private:
    // 输入框配色的**唯一写入口**：构造时一次 + 每次 refresh() 一次。
    //   绝不能在 paint() 里调用（paint 期改子组件属性会触发 colourChanged→repaint，
    //   导致同一帧内文本重绘/重影，且各行更新时机不一致）。
    void applyRowColours(bool darkMode);

    int  role_ = 0;
    juce::Label nameLabel_;
    juce::TextButton swatchBtn_;      // 色块 + ▶（点击弹取色器）
    juce::TextEditor hexEditor_;
    juce::TextButton autoToggle_;     // Auto 开关（按下=自动，弹起=手动锁定）

    std::function<void(int, y2kpal::Rgb8)> onManualSet_;
    std::function<void(int)> onAutoReset_;
    std::function<void(int)> onPick_;

    bool updating_ = false;   // 防止 hex 解析回写死循环
    y2kpal::Rgb8 lastShown_ { 0xff, 0xff, 0xff };   // 最近一次刷新显示的颜色（Auto 关闭时锁定它）

    // hex 输入框与 Auto 开关之间的纯展示色块（实时显示当前角色颜色）
    y2kpal::Rgb8 displayColour_ { 0xff, 0xff, 0xff };
    juce::Rectangle<int> swatchViewRect_;
};

// ------------------------------------------------------------
// MyThemesList —— 我的主题列表（R11）
// ------------------------------------------------------------
class MyThemesList : public juce::Component
{
public:
    MyThemesList();

    void paint(juce::Graphics& g) override;
    void resized() override {}
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    void mouseDown(const juce::MouseEvent& e) override;

    // 面板提供的动作
    std::function<void(int)> onApply;
    std::function<void(int)> onRename;
    std::function<void(int)> onDelete;
    std::function<void(int)> onMoveUp;
    std::function<void(int)> onMoveDown;

    int  rowHeight = 24;
    void refreshLayout();

private:
    int hitTestRow(juce::Point<int> p) const;
    juce::Rectangle<int> rowBounds(int index) const;
    int hoverRow_ = -1;
};

// ------------------------------------------------------------
// PalettePanel —— 主面板
// ------------------------------------------------------------
class PalettePanel : public juce::Component,
                     public juce::Button::Listener,
                     public juce::Timer
{
public:
    PalettePanel();
    ~PalettePanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void timerCallback() override;
    void buttonStateChanged(juce::Button*) override;
    void buttonClicked(juce::Button*) override {}   // Button::Listener 纯虚，空实现
    bool keyPressed(const juce::KeyPress& key) override;

    void show();
    void dismiss();
    bool isShowing() const noexcept { return visible_; }

    std::function<void()> onClosed;         // 关闭时通知父组件
    std::function<void()> onHeightChanged;  // 面板高度变化（折叠展开）时通知父组件重定位

private:
    // ---- 打开面板时的快照（还原点，R3）----
    struct Snapshot
    {
        bool wasCustom = false;
        int  themeId = 0;
        y2kpal::ThemeSpec spec;
    };

    // ---- 控件 ----
    std::unique_ptr<HueRing> hueRing_;
    juce::TextButton modeDarkBtn_, modeLightBtn_;
    juce::Slider vibeSlider_, tintSlider_;
    juce::Label vibeLabel_, tintLabel_, modeLabel_, harmonyLabel_, patternLabel_, iconLabel_, noteLabel_, badgeLabel_;
    juce::TextButton harmonyBtns_[5];
    juce::ComboBox patternCombo_;
    juce::TextButton iconBtns_[8];
    juce::TextButton randomBtn_, compareBtn_, revertBtn_, saveBtn_, closeBtn_;
    MiniPreviewCard previewCard_;
    juce::TextButton advancedToggle_, myThemesToggle_;
    std::unique_ptr<RoleRow> roleRows_[7];
    SvHuePicker svPicker_;
    MyThemesList myThemesList_;
    std::unique_ptr<juce::Viewport> myThemesViewport_;
    juce::Label myThemesHint_;
    juce::TextButton copyCodeBtn_, pasteCodeBtn_, exportBtn_, importBtn_;

    // ---- 内嵌命名输入栏（插件模式下 JUCE_MODAL_LOOPS_PERMITTED=0，
    //   AlertWindow::runModalLoop 不可用，故命名/重命名走面板内嵌输入）----
    juce::TextEditor nameEditor_;
    juce::TextButton nameOkBtn_, nameCancelBtn_;
    bool namePromptVisible_ = false;
    std::function<void(const juce::String&)> namePromptOnOk_;

    // ---- 面板内轻提示（toast，替代 JUCE 默认样式 AlertWindow 弹窗）----
    juce::Label toastLabel_;
    uint32 toastUntil_ = 0;

    // ---- 状态 ----
    y2kpal::ThemeSpec spec_;
    y2kpal::GeneratedTheme lastGen_;
    Snapshot snapshot_;
    bool visible_ = false;
    bool compareHeld_ = false;
    bool advancedOpen_ = false;
    bool myThemesOpen_ = false;
    int  pickingRole_ = -1;

    // 拖动状态：色环/滑条拖动期间只刷新面板内预览（不提交全局主题，
    //   避免每帧触发桌面纹理重烘焙 + 全模块重绘的卡顿），松手统一提交。
    bool hueDragging_    = false;
    bool sliderDragging_ = false;

    // 主题库订阅
    int libToken_ = 0;
    int themeSubToken_ = -1;   ///< PinkXP 主题变更订阅（外部套用主题时刷新行配色）

    // 外部点击关闭监听
    class DismissListener;
    std::unique_ptr<DismissListener> dismissListener_;
    void attachDismissListener();
    void detachDismissListener();

    // ---- 内部逻辑 ----
    void loadSpecIntoUi(const y2kpal::ThemeSpec& s);
    void applyCurrentSpec();                     // 实时预览（节流入口）
    void applySpecImmediately();                 // 立即应用
    void specChanged();                          // 参数变化统一入口：重新生成 + 节流应用
    void regenerateAndRefresh();                 // 重新生成 + 刷新角色行/徽章/预览
    void refreshRoleRows();
    void refreshBadge();
    void updatePanelHeight();
    void randomize();
    void enterCompare();
    void exitCompare();
    void revertToSnapshot();
    void saveCurrentAsUserTheme();
    void copyShareCode();
    void pasteShareCode();
    void exportCurrentToFile();
    void importFromFile();

    // 弹出内嵌命名栏（onOk 接收去空白后的名字）
    void promptForName(const juce::String& initial,
                       std::function<void(const juce::String&)> onOk);
    void closeNamePrompt(bool commit);

    // 自动生成一个未被占用的"My Palette N"名字（R11 保存/粘贴共用）
    juce::String nextAutoThemeName();

    // 某角色当前实际显示色（手动覆盖优先，否则取 lastGen_ 对应槽位）
    y2kpal::Rgb8 roleColourFor(int role) const;

    // 面板内轻提示（2 秒自动消失）
    void showToast(const juce::String& msg);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PalettePanel)
};
