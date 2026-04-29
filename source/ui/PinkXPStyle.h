#ifndef PBEQ_PINKXP_STYLE_H_INCLUDED
#define PBEQ_PINKXP_STYLE_H_INCLUDED

#include <JuceHeader.h>

// ==========================================================
// Pink XP · 像素复古风（Win95/98 + 粉色主题）
//   1. PinkXP 命名空间：配色常量 + 像素凸起/凹陷/桌面底纹绘制工具
//   2. PinkXPLookAndFeel：统一的 JUCE 控件绘制（按钮/滑条/标签）
//   3. getPinkXPFont / getPinkXPLookAndFeel：全局单例入口
// 用于整个多模块框架中所有模块的视觉统一。
// ==========================================================

namespace PinkXP
{
    // ---- 调色板（运行时可变——由当前主题写入）----
    //   注意：去掉了 const，让 applyTheme() 能覆盖这些变量；
    //   所有模块仍使用 PinkXP::pink300 / pink500 等符号，无需改动。
    inline juce::Colour pink50  { 0xffffe9f2 };
    inline juce::Colour pink100 { 0xffffd6e7 };
    inline juce::Colour pink200 { 0xffffb6d1 };
    inline juce::Colour pink300 { 0xffff8fb5 };
    inline juce::Colour pink400 { 0xffff6fa0 };
    inline juce::Colour pink500 { 0xffec4d85 };
    inline juce::Colour pink600 { 0xffc23368 };
    inline juce::Colour pink700 { 0xff8a2048 };

    // Win95 四色边框（会被主题覆盖）
    inline juce::Colour hl      { 0xffffffff };   // 高光（左上）
    inline juce::Colour face    { 0xffffd6e7 };   // 按钮面
    inline juce::Colour shdw    { 0xffc23368 };   // 阴影（内层）
    inline juce::Colour dark    { 0xff4a0d2a };   // 最外层深色

    inline juce::Colour ink     { 0xff2b0815 };   // 正文墨色
    inline juce::Colour sel     { 0xffec4d85 };   // 选中色（标题栏粉）
    inline juce::Colour selInk  { 0xffffffff };

    inline juce::Colour desktop  { 0xffc23368 };  // 桌面主色
    inline juce::Colour desktop2 { 0xffa12654 };  // 桌面纹理色

    // 模块内容画布底色（跟随主题，深色主题下也保持浅色以保证 ink 文字可读）
    inline juce::Colour content  { 0xfffff8fc };

    // 按钮/小控件专用底色（深色主题下也保持浅色，保证按钮文字/图标可读）
    inline juce::Colour btnFace  { 0xffffd6e7 };

    // ---- 绘制工具 ----
    void drawRaised   (juce::Graphics& g, juce::Rectangle<int> r, juce::Colour fill = face);
    void drawSunken   (juce::Graphics& g, juce::Rectangle<int> r, juce::Colour fill);
    void drawPressed  (juce::Graphics& g, juce::Rectangle<int> r, juce::Colour fill = face);
    void drawHardShadow(juce::Graphics& g, juce::Rectangle<int> r, int offset = 4);
    void drawDesktop  (juce::Graphics& g, juce::Rectangle<int> r);

    // 在 area 中央绘制插件 logo（由调用方提供 Image，通常是 Editor 成员缓存，
    // 这样避免 ImageCache / static 跨 DLL 卸载时的悬垂引用）
    void drawLogo     (juce::Graphics& g, juce::Rectangle<int> area,
                       const juce::Image& logo);

    // 绘制一个 Pink XP 风格的标题栏（玫瑰粉横条 + 高光/阴影/下沿深色分割线）
    //   title：文字
    //   fontHeight：字号
    // 返回值：标题栏占据的矩形（已在 bounds 内部 1px 边框内）
    juce::Rectangle<int> drawPinkTitleBar(juce::Graphics& g,
                                          juce::Rectangle<int> bounds,
                                          const juce::String& title,
                                          float fontHeight = 11.0f);

    // 在 iconArea 中央以 colour 绘制一个文本符号图标（由主题配置）
    void drawTitleIconText(juce::Graphics& g,
                           juce::Rectangle<int> iconArea,
                           const juce::String& iconText,
                           juce::Colour colour,
                           float fontHeight = 12.0f);

    // ==========================================================
    // Y2K / 像素装饰工具（供各模块图谱内部使用）
    // ==========================================================

    // CRT 扫描线：每 2 像素一条淡色横线，alpha 控制强度
    void drawScanlines(juce::Graphics& g, juce::Rectangle<int> area,
                       juce::Colour colour = pink300, float alpha = 0.10f);

    // 四角像素 L 形装饰（Y2K 典型元素）
    //   armLen: L 形臂长  thickness: 粗细
    void drawPixelCorners(juce::Graphics& g, juce::Rectangle<int> area,
                          juce::Colour colour = pink500,
                          int armLen = 6, int thickness = 2);

    // 外围棋盘格边框（1 像素格，外贴贴边）
    void drawCheckerBorder(juce::Graphics& g, juce::Rectangle<int> area,
                           juce::Colour a = pink400,
                           juce::Colour b = pink100,
                           int cellSize = 2);

    // 随机像素星（伪随机按 phase 选位置，可用于动态闪烁）
    //   count 个 2x2 像素点，分布在 area 内部
    void drawPixelStars(juce::Graphics& g, juce::Rectangle<int> area,
                        int phase, int count = 8,
                        juce::Colour colour = pink200);

    // 虚线像素边框（每段 3px on / 3px off）
    void drawDashedBorder(juce::Graphics& g, juce::Rectangle<int> area,
                          juce::Colour colour = pink400, int dash = 3);

    // Y2K 竖向量化渐变（把渐变分成 N 个色带，模拟 8bit 显示）
    void drawY2KGradient(juce::Graphics& g, juce::Rectangle<int> area,
                         juce::Colour top, juce::Colour bottom, int bands = 6);

    // ---- 字体 ----
    // 初始化全局 Typeface（由 PluginEditor 在启动时调用一次）
    void initCustomTypeface(juce::Typeface::Ptr ptr);

    // 从 BinaryData 加载当前字体（Silkscreen-Regular.ttf）
    juce::Typeface::Ptr loadActiveTypeface();

    // 取自定义字体（按激活字体）
    // 注意：为整体可读性，此函数会对高度做 1.5x 放大（坐标轴刻度请用 getAxisFont）
    juce::Font getFont(float height, int styleFlags = juce::Font::plain);

    // 坐标轴刻度专用字体：不做放大，保持原始 height
    juce::Font getAxisFont(float height, int styleFlags = juce::Font::plain);

    // ======================================================
    // 主题系统（运行时可切换）
    // ======================================================
    enum class ThemeId
    {
        bubblegum,    // 🌸 糖果粉（默认）
        starlight,    // 🌌 星空深蓝 + 黄星
        cyberLilac,   // 🪩 赛博紫 + 青
        tangerinePop, // 🍊 橘色波普 + 奶油
        aquaPearl,    // 🪞 水蓝珠光
        matchaSoda,   // 🫧 苏打绿 + 粉
        winXP,        // 🪟 Windows XP 经典配色（Luna 蓝 / 绿草地）
        crimsonNoir,  // 🩸 红黑暗夜（深酒红 + 漆黑）
        voidGrey,     // ⬛ 纯黑灰（OLED 友好的暗色极简）
        paperGrey     // ⬜ 纯白灰（纸面极简，亮色）
    };

    // 桌面纹理样式
    enum class DesktopPattern
    {
        checker,     // 棋盘格（默认）
        pixelStars,  // 像素小星星
        scanGrid,    // 网格扫描线
        bigDots,     // 大圆点
        bubbles,     // 横条泡泡
        diagStripes  // 斜条纹
    };

    // 单一主题定义
    struct Theme
    {
        ThemeId        id;
        const char*    displayName;   // UI 显示名（含 emoji）
        const char*    keywordHint;   // 关键词（tooltip 用）

        juce::Colour pink50, pink100, pink200, pink300, pink400, pink500, pink600, pink700;
        juce::Colour hl, face, shdw, dark;
        juce::Colour ink, sel, selInk;
        juce::Colour desktop, desktop2;
        juce::Colour content;         // 模块画布底色（浅色、保持文字可读）
        juce::Colour btnFace;         // 按钮/小控件底色（深色主题下也保持浅色）

        juce::Colour swatch;          // 调色板色票（UI 选择器里显示的色块）

        DesktopPattern desktopPattern;

        // 模块标题栏左侧的文本符号图标（跟随主题变化）
        const char* titleIconText;
    };

    // 取所有预设主题（按 ThemeId 顺序排列）
    const std::vector<Theme>& getAllThemes();

    // 取当前激活主题
    ThemeId     getCurrentThemeId();
    const Theme& getCurrentTheme();

    // 切换主题（写入全局调色板变量 + 触发 onThemeChanged 回调）
    void applyTheme(ThemeId id);

    // 订阅主题变更事件（例如顶层 Editor 注册一个 repaint 全局刷新）
    // 返回订阅 token，调用 unsubscribeThemeChanged(token) 取消。
    using ThemeChangedCallback = std::function<void()>;
    int  subscribeThemeChanged(ThemeChangedCallback cb);
    void unsubscribeThemeChanged(int token);
}

// ==========================================================
// Pink XP LookAndFeel —— 统一所有 JUCE 控件外观
// ==========================================================
class PinkXPLookAndFeel : public juce::LookAndFeel_V4
{
public:
    PinkXPLookAndFeel();

    void drawButtonBackground(juce::Graphics&, juce::Button&,
                              const juce::Colour&, bool, bool) override;
    void drawButtonText(juce::Graphics&, juce::TextButton&, bool, bool) override;
    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;

    void drawLinearSlider(juce::Graphics&, int x, int y, int w, int h,
                          float sliderPos, float, float,
                          const juce::Slider::SliderStyle, juce::Slider&) override;

    juce::Font getLabelFont(juce::Label&) override;
    juce::Label* createSliderTextBox(juce::Slider&) override;

    void drawPopupMenuBackground(juce::Graphics& g, int width, int height) override;
    void drawPopupMenuItem(juce::Graphics&, const juce::Rectangle<int>& area,
                           bool isSeparator, bool isActive, bool isHighlighted,
                           bool isTicked, bool hasSubMenu,
                           const juce::String& text, const juce::String& shortcutKeyText,
                           const juce::Drawable* icon, const juce::Colour* textColour) override;
    juce::Font getPopupMenuFont() override;

    // 全局字体兜底：任何通过 LookAndFeel 请求 Typeface 的控件都优先使用
    // PinkXP 当前激活的自定义字体（Silkscreen）。这能覆盖未显式 setFont 的
    // 组件路径，避免“局部生效、整体回退系统字体”。
    juce::Typeface::Ptr getTypefaceForFont (const juce::Font&) override;

    // Popup 菜单条目理想尺寸：按实际文字宽度 + 左右内边距计算，避免
    //   ComboBox 弹出的下拉菜单宽度只等于 ComboBox 本身宽度而把长文本
    //   截断（例如 "Horizontal Bar(T)" / "Horizontal Bar(B)"）。
    //   JUCE 内部会取 max(comboBoxWidth, maxItemIdealWidth)，所以只会把
    //   popup 变宽，不会影响 ComboBox 自身或其他未使用该 item 的菜单。
    void getIdealPopupMenuItemSize(const juce::String& text, bool isSeparator,
                                   int standardMenuItemHeight,
                                   int& idealWidth, int& idealHeight) override;

    // ---- ComboBox（Pink XP 像素风）----
    //   · 灰粉凸起框体 + 右侧"▼"箭头按钮凹陷（被按下时）
    //   · 文字颜色 / 字体与 Label 统一，和主题同色系
    void drawComboBox(juce::Graphics&, int width, int height,
                      bool isButtonDown, int buttonX, int buttonY,
                      int buttonW, int buttonH, juce::ComboBox&) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override;
    void positionComboBoxText(juce::ComboBox&, juce::Label&) override;
};

// 进程级单例 LookAndFeel
PinkXPLookAndFeel& getPinkXPLookAndFeel();

#endif // PBEQ_PINKXP_STYLE_H_INCLUDED