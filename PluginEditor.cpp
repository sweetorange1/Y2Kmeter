#include "PluginEditor.h"
#include <JuceHeader.h>
#include <cmath>
#include "BinaryData.h"
#include "source/ui/PinkXPStyle.h"
#include "source/ui/ModuleWorkspace.h"
#include "source/ui/modules/EqModule.h"
#include "source/ui/modules/LoudnessModule.h"
#include "source/ui/modules/OscilloscopeModule.h"
#include "source/ui/modules/OscilloscopeWaveModule.h"
#include "source/ui/modules/SpectrumModule.h"
#include "source/ui/modules/PhaseModule.h"
#include "source/ui/modules/DynamicsModule.h"
#include "source/ui/modules/FineSplitModules.h"
#include "source/ui/modules/WaveformModule.h"
#include "source/ui/modules/SpectrogramModule.h"
#include "source/ui/modules/Spectrogram3DModule.h"
#include "source/ui/modules/TamagotchiModule.h"
#include "source/ui/modules/MilkdropModule.h"
#include "source/analysis/AnalyserHub.h"

// ==========================================================
// FpsFrameListener——嵌套实现（封装在 .cpp 以绕开 AnalyserHub 完整头
//   在 PluginEditor.h 里暴露时导致的 MSVC include guard 串扰问题）
// ==========================================================
class Y2KmeterAudioProcessorEditor::FpsFrameListener : public AnalyserHub::FrameListener
{
public:
    explicit FpsFrameListener (Y2KmeterAudioProcessorEditor& ownerRef) : owner (ownerRef) {}

    void onFrame (const AnalyserHub::FrameSnapshot& /*frame*/) override
    {
        // UI 线程回调：仅原子计数器 +1（轻量、不做其它处理）
        owner.frameCounter.fetch_add (1, std::memory_order_relaxed);
    }

private:
    Y2KmeterAudioProcessorEditor& owner;
};

// ==========================================================
// ChromeHiddenOverlay —— chrome 隐藏态的"纯视觉"浮层
//
// 职责：
//   · 底图上打印软件名 / 版本号 / 官网链接（低对比度）
//   · 右上角半透明关闭按钮（视觉）
//
// 关键设计：
//   · 整体宽度与 Editor 同宽、高度 = titleBarHeight。
//   · z-order 最底层（在 workspace 之下）→ 模块可以自然遮挡 overlay 的文字和按钮。
//   · 本组件不处理任何鼠标事件：setInterceptsMouseClicks(false, false)。
//     顶部按钮的点击由 Editor 在 mouseMove/Down/Up 中统一处理（workspace 在对应
//     矩形区域做 hit-test 挖洞，让事件冒泡到 Editor）。
//   · 视觉状态（closeButtonHovered / titleTextHovered 等）由 Editor 在鼠标事件中
//     通过 setCloseButtonHovered / setTitleTextHovered 下发。
// ==========================================================
class Y2KmeterAudioProcessorEditor::ChromeHiddenOverlay : public juce::Component
{
public:
    explicit ChromeHiddenOverlay (Y2KmeterAudioProcessorEditor& ownerRef)
        : owner (ownerRef)
    {
        // 纯视觉层：完全不拦截鼠标事件（事件全部透传到下方的 workspace / Editor）
        setInterceptsMouseClicks (false, false);

        // 预先计算标题文字像素宽度，这样 Editor 首次调用 updateWorkspaceHitTestHoles
        //   就能拿到精确的文字矩形，而不用等第一次 paint 写入。
        //   （与 paint() 里完全相同的字体/文本/间距计算）
        const juce::Font nameFont    = PinkXP::getFont (12.0f, juce::Font::bold);
        const juce::Font versionFont = PinkXP::getFont (10.0f, juce::Font::italic);
        const juce::Font urlFont     = PinkXP::getFont (10.0f, juce::Font::plain);
        const int nameW    = nameFont.getStringWidth ("Y2Kmeter");
const int versionW = versionFont.getStringWidth ("v2.1.12");
        const int urlW     = urlFont.getStringWidth ("iisaacbeats.cn");
        constexpr int gap1 = 6;
        constexpr int gap2 = 10;
        cachedTitleTextW = nameW + gap1 + versionW + gap2 + urlW;
    }

    // 由 Editor 更新视觉状态（hover/pressed）
    void setCloseButtonHovered (bool h)
    {
        if (h != closeButtonHovered) { closeButtonHovered = h; repaint (getFloatingCloseButtonRect()); }
    }
    void setCloseButtonPressed (bool p)
    {
        if (p != closeButtonPressed) { closeButtonPressed = p; repaint (getFloatingCloseButtonRect()); }
    }
    void setTitleTextHovered (bool h)
    {
        if (h != titleTextHovered) { titleTextHovered = h; repaint(); }
    }

    // 几何（Editor 需要用来做 hit-test 与冒泡处理）
    juce::Rectangle<int> getFloatingCloseButtonRect() const
    {
        constexpr int margin = 4;
        constexpr int size   = 18; // 与 Editor::closeButtonSize 保持一致
        return { getWidth() - margin - size, margin, size, size };
    }

    // 标题文字整体命中矩形（宽度取实际像素宽度，由 paint 写入）
    juce::Rectangle<int> getTitleTextHitRect() const
    {
        const int x = 28;
        const int w = juce::jmax (0, cachedTitleTextW);
        return { x, 0, w, juce::jmin (26, getHeight()) };
    }

    void paint (juce::Graphics& g) override
    {
        // ------- 1) 顶部抬头文字：软件名 + 版本号 + 官网（低对比度，贴在底图上）-------
        const juce::String nameText    = "Y2Kmeter";
const juce::String versionText = "v2.1.12";
        const juce::String urlText     = "iisaacbeats.cn";

        const juce::Font nameFont    = PinkXP::getFont (12.0f, juce::Font::bold);
        const juce::Font versionFont = PinkXP::getFont (10.0f, juce::Font::italic);
        const juce::Font urlFont     = PinkXP::getFont (10.0f, juce::Font::plain);

        const int nameW    = nameFont.getStringWidth ("Y2Kmeter");
        const int versionW = versionFont.getStringWidth (versionText);
        const int urlW     = urlFont.getStringWidth (urlText);

        constexpr int gap1 = 6;
        constexpr int gap2 = 10;

        const int x0 = 28;
        const int y  = 0;
        const int h  = juce::jmin (26, getHeight());

        // chrome 隐藏态：文字淡淡贴在底图上，hover 时略亮并加下划线
        const float textAlpha = titleTextHovered ? 0.95f : 0.55f;

        // 文字描边（1px 阴影），帮助文字在任何底图上都可读
        g.setColour (juce::Colours::white.withAlpha (textAlpha * 0.6f));
        g.setFont (nameFont);
        g.drawText (nameText, x0 + 1, y + 1, nameW, h, juce::Justification::centredLeft, false);
        g.setFont (versionFont);
        g.drawText (versionText, x0 + nameW + gap1 + 1, y + 1, versionW, h, juce::Justification::centredLeft, false);
        g.setFont (urlFont);
        g.drawText (urlText, x0 + nameW + gap1 + versionW + gap2 + 1, y + 1, urlW, h, juce::Justification::centredLeft, false);

        // 主文字（墨色）
        g.setColour (PinkXP::ink.withAlpha (textAlpha));
        g.setFont (nameFont);
        g.drawText (nameText, x0, y, nameW, h, juce::Justification::centredLeft, false);
        g.setFont (versionFont);
        g.drawText (versionText, x0 + nameW + gap1, y, versionW, h, juce::Justification::centredLeft, false);
        g.setFont (urlFont);
        g.drawText (urlText, x0 + nameW + gap1 + versionW + gap2, y, urlW, h, juce::Justification::centredLeft, false);

        if (titleTextHovered)
        {
            const int lineY = y + h - 3;
            const int totalW = nameW + gap1 + versionW + gap2 + urlW;
            g.setColour (PinkXP::ink.withAlpha (0.95f));
            g.fillRect (x0, lineY, totalW, 1);
        }

        // 缓存实际绘制宽度，供 Editor 做精确命中测试
        cachedTitleTextW = nameW + gap1 + versionW + gap2 + urlW;

        // ------- 2) 右上角浮动关闭按钮（15% / 100% 两档透明度）-------
        juce::Graphics::ScopedSaveState save (g);
        g.setOpacity (closeButtonHovered ? 1.0f : 0.15f);

        auto cb = getFloatingCloseButtonRect();
        if (closeButtonPressed)
            PinkXP::drawPressed (g, cb, PinkXP::pink100);
        else
            PinkXP::drawRaised  (g, cb, closeButtonHovered ? PinkXP::pink200 : PinkXP::btnFace);

        g.setColour (PinkXP::ink);
        g.setFont   (PinkXP::getFont (12.0f, juce::Font::bold));
        auto cbText = cb;
        cbText.translate (-1, -1);
        if (closeButtonPressed) cbText.translate (1, 1);
        g.drawText ("x", cbText, juce::Justification::centred, false);
    }

    int getCachedTitleTextWidth() const noexcept { return cachedTitleTextW; }

private:
    Y2KmeterAudioProcessorEditor& owner;
    bool closeButtonHovered = false;
    bool closeButtonPressed = false;
    bool titleTextHovered   = false;
    mutable int cachedTitleTextW = 0;
};

// ==========================================================
// TutorialOverlay —— 新手引导全屏覆盖层
//
// 职责：
//   · 半透明遮罩 + "聚光灯"镂空区域（高亮引导目标）
//   · Y2K 风格气泡对话框提示（右上角带 × 关闭按钮）
//   · STEP 1：右键画布添加 Tamagotchi
//   · STEP 2：播放音频孵化宠物蛋
//   · 点击 × 按钮 → 弹出二次确认对话框（"跳过引导不可撤销"）
//
// 关键设计：
//   · 覆盖整个 Editor，z-order 最高（在 workspace 之上）
//   · setInterceptsMouseClicks (true, true)：拦截点击
//   · 仅右键点击聚光灯区域触发回调；左键/其他区域忽略
//   · 视觉风格参考 TamagotchiConfirmOverlay（PinkXP 凸起边框对话框）
// ==========================================================
class Y2KmeterAudioProcessorEditor::TutorialOverlay : public juce::Component
{
public:
    TutorialOverlay()
    {
        setOpaque (false);
    }

    void showStep1 (const juce::Rectangle<int>& canvasAreaInEditor)
    {
        currentStep = Step::step1;
        highlightArea = canvasAreaInEditor;
        confirmSkipVisible = false;
        menuIsOpen = false;
        setAlwaysOnTop (true);
        setInterceptsMouseClicks (true, true);
        setVisible (true);
        repaint();
    }

    // 右键菜单已弹出：保持遮罩和聚光灯不变，只更换气泡引导文案
    void showStep1MenuOpened()
    {
        jassert (currentStep == Step::step1); // 仅 STEP 1 有效
        menuIsOpen = true;
        repaint();
    }

    void showStep2 (const juce::Rectangle<int>& petAreaInEditor)
    {
        currentStep = Step::step2;
        highlightArea = petAreaInEditor.expanded (12);
        confirmSkipVisible = false;
        menuIsOpen = false;
        setAlwaysOnTop (true);
        setInterceptsMouseClicks (true, true);
        setVisible (true);
        repaint();
    }

    void hide()
    {
        currentStep = Step::hidden;
        highlightArea = {};
        bubbleHovered = false;
        closeBtnHovered = false;
        closeBtnPressed = false;
        confirmSkipVisible = false;
        confirmBtnHovered = false;
        menuIsOpen = false;
        setVisible (false);
        setAlwaysOnTop (false);
        setInterceptsMouseClicks (false, false);
    }

    void paint (juce::Graphics& g) override
    {
        if (currentStep == Step::hidden) return;

        drawSpotlight (g);
        drawSpeechBubble (g);

        // 二次确认弹窗（在气泡之上）
        if (confirmSkipVisible)
            drawConfirmSkipDialog (g);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (currentStep == Step::hidden) return;
        const auto pos = e.getPosition();

        // 二次确认弹窗可见 → 只处理确认/取消
        if (confirmSkipVisible)
        {
            if (e.mods.isLeftButtonDown())
            {
                const auto dlg = getConfirmDialogBounds();
                if (! dlg.contains (pos)) { confirmSkipVisible = false; repaint(); return; }
                if (getConfirmBtnBounds (1).contains (pos)) // Skip 按钮
                {
                    confirmSkipVisible = false;
                    hide();
                    if (onSkipRequested) onSkipRequested();
                    return;
                }
                // Cancel 按钮或 dialog 内非按钮区 → 关闭弹窗
                confirmSkipVisible = false;
                repaint();
                return;
            }
        }

        // 左键点击气泡 × 关闭按钮
        if (e.mods.isLeftButtonDown())
        {
            const auto closeBtn = getCloseButtonBounds();
            if (closeBtn.contains (pos))
            {
                confirmSkipVisible = true;
                confirmBtnHovered = false;
                repaint();
                return;
            }
        }

        // STEP 1：右键聚光灯区域 → 触发添加
        if (currentStep == Step::step1 && e.mods.isRightButtonDown())
        {
            if (highlightArea.contains (pos) && onRightClickHighlight)
                onRightClickHighlight (pos);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        if (currentStep == Step::hidden) return;
        const auto pos = e.getPosition();

        if (confirmSkipVisible)
        {
            const auto dlg = getConfirmDialogBounds();
            const bool btnHover = getConfirmBtnBounds (1).contains (pos)
                               || getConfirmBtnBounds (0).contains (pos);
            const bool dlgHover = dlg.contains (pos);
            if (btnHover != confirmBtnHovered || dlgHover != bubbleHovered)
            {
                confirmBtnHovered = btnHover;
                bubbleHovered = dlgHover;
                repaint (dlg);
            }
            return;
        }

        // 关闭按钮 hover
        const auto closeBtn = getCloseButtonBounds();
        const bool cbHover = closeBtn.contains (pos);
        if (cbHover != closeBtnHovered)
        {
            closeBtnHovered = cbHover;
            repaint (closeBtn.expanded (2));
        }

        // 气泡整体 hover
        const auto bubble = getBubbleBounds();
        const bool hover = bubble.contains (pos);
        if (hover != bubbleHovered)
        {
            bubbleHovered = hover;
            repaint (bubble);
        }
    }

    // 用户右键点击了高亮区域 → 回调（参数：相对本组件的点击位置）
    std::function<void(juce::Point<int> clickPos)> onRightClickHighlight;
    // 用户确认跳过新手引导 → 回调
    std::function<void()> onSkipRequested;

    // 返回当前聚光灯区域（Editor 坐标系），供外部恢复 STEP1 时复用
    juce::Rectangle<int> getHighlightArea() const { return highlightArea; }

private:
    enum class Step { hidden, step1, step2 };
    Step currentStep = Step::hidden;
    juce::Rectangle<int> highlightArea;
    bool bubbleHovered = false;
    bool closeBtnHovered = false;
    bool closeBtnPressed = false;
    bool confirmSkipVisible = false;
    bool confirmBtnHovered = false;
    bool menuIsOpen = false;     // STEP 1 右键菜单已弹出，此时切换气泡文案

    // 气泡右上角 × 按钮（相对本组件坐标，基于 getBubbleBounds 偏移）
    juce::Rectangle<int> getCloseButtonBounds() const
    {
        const auto bubble = getBubbleBounds();
        constexpr int btnSize = 18;
        constexpr int margin  = 4;
        return { bubble.getRight() - margin - btnSize,
                 bubble.getY() + margin,
                 btnSize, btnSize };
    }

    // 二次确认弹窗区域（居中于 Editor）
    juce::Rectangle<int> getConfirmDialogBounds() const
    {
        constexpr int dlgW = 260;
        constexpr int dlgH = 100;
        const int cx = getWidth()  / 2;
        const int cy = getHeight() / 2;
        return { cx - dlgW / 2, cy - dlgH / 2, dlgW, dlgH };
    }

    // 二次确认弹窗按钮（idx: 0=Cancel, 1=Skip）
    juce::Rectangle<int> getConfirmBtnBounds (int idx) const
    {
        const auto dlg = getConfirmDialogBounds();
        auto inner = dlg.reduced (6);
        // 跳过标题行 18 + 分隔线(1) 后的正文区
        inner.removeFromTop (juce::jmin (18, inner.getHeight() - 30));

        const int btnH = 20;
        auto btnRow = juce::Rectangle<int> (inner.getX(), inner.getBottom() - btnH,
                                             inner.getWidth(), btnH);
        const int btnW = juce::jmin (70, (btnRow.getWidth() - 12) / 2);
        const int gap = 8;

        if (idx == 0) // Cancel（左）
            return { btnRow.getCentreX() - btnW - gap / 2, btnRow.getY(), btnW, btnH };
        else          // Skip（右）
            return { btnRow.getCentreX() + gap / 2, btnRow.getY(), btnW, btnH };
    }

    void drawConfirmSkipDialog (juce::Graphics& g)
    {
        const auto dlg = getConfirmDialogBounds();

        // 半透明底衬
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRect (getLocalBounds());

        // 对话框外壳
        PinkXP::drawRaised (g, dlg, PinkXP::btnFace);

        auto inner = dlg.reduced (6);
        if (inner.getHeight() < 50) return;

        // 标题行
        auto titleRow = inner.removeFromTop (juce::jmin (18, inner.getHeight() - 30));
        g.setColour (PinkXP::sel);
        g.fillRect (titleRow);
        g.setColour (PinkXP::dark);
        g.fillRect (titleRow.getX(), titleRow.getBottom(), titleRow.getWidth(), 1);
        g.setColour (PinkXP::selInk);
        g.setFont (PinkXP::getFont (10.0f, juce::Font::bold));
        g.drawText ("SKIP TUTORIAL?", titleRow.reduced (3, 1), juce::Justification::centredLeft, false);

        // 正文
        auto body = inner.reduced (2, 2);
        auto textArea = body.removeFromTop (body.getHeight() - 24);
        g.setColour (PinkXP::ink);
        g.setFont (PinkXP::getFont (9.0f));
        g.drawFittedText ("U will miss the fun :(\nthis can't be undone~",
                          textArea, juce::Justification::centred, 2);

        // Cancel 按钮（始终平面，不单独高亮）
        auto cancelBounds = getConfirmBtnBounds (0);
        PinkXP::drawRaised (g, cancelBounds, PinkXP::btnFace);
        g.setColour (PinkXP::ink);
        g.setFont (PinkXP::getFont (9.0f, juce::Font::bold));
        g.drawText ("Cancel", cancelBounds, juce::Justification::centred, false);

        // Skip 按钮（粉红突出，始终醒目）
        auto skipBounds = getConfirmBtnBounds (1);
        PinkXP::drawRaised (g, skipBounds, PinkXP::pink200);
        g.setColour (PinkXP::pink700);
        g.setFont (PinkXP::getFont (9.0f, juce::Font::bold));
        g.drawText ("Skip", skipBounds, juce::Justification::centred, false);
    }

    void drawSpotlight (juce::Graphics& g)
    {
        const auto fullArea = getLocalBounds();

        // 半透明黑色遮罩 —— 镂空聚光灯区域（Tamagotchi 模块保持可见）
        {
            juce::Graphics::ScopedSaveState save (g);
            g.excludeClipRegion (highlightArea);
            g.setColour (juce::Colours::black.withAlpha (0.65f));
            g.fillRect (fullArea);
        }

        // 聚光灯边框：虚线闪烁效果（粉色，暗示交互区域）
        {
            g.setColour (PinkXP::pink600.withAlpha (0.8f));
            const float dash[] = { 4.0f, 4.0f };
            g.drawDashedLine (juce::Line<float> (highlightArea.getTopLeft().toFloat(),
                              highlightArea.getTopRight().toFloat()), dash, 2);
            g.drawDashedLine (juce::Line<float> (highlightArea.getTopRight().toFloat(),
                              highlightArea.getBottomRight().toFloat()), dash, 2);
            g.drawDashedLine (juce::Line<float> (highlightArea.getBottomRight().toFloat(),
                              highlightArea.getBottomLeft().toFloat()), dash, 2);
            g.drawDashedLine (juce::Line<float> (highlightArea.getBottomLeft().toFloat(),
                              highlightArea.getTopLeft().toFloat()), dash, 2);
        }
    }

    // 气泡位置：
    //   STEP 1：优先聚光灯上方 → 下方 → 居中（canvas 占满整个区域）
    //   STEP 2：放在 Tamagotchi 模块的左侧或右侧（取决于模块在 Editor 中的位置），
    //           绝不覆盖模块本身
    juce::Rectangle<int> getBubbleBounds() const
    {
        constexpr int bubbleW = 280;
        constexpr int bubbleH = 76;

        if (currentStep == Step::step2)
        {
            const int editorW = getWidth();

            // 模块中心 X > Editor 中心 → 模块偏右 → 气泡放在模块左侧
            // 模块中心 X < Editor 中心 → 模块偏左 → 气泡放在模块右侧
            const bool petOnRight = highlightArea.getCentreX() > editorW / 2;

            int bubbleX;
            if (petOnRight)
                bubbleX = highlightArea.getX() - bubbleW - 12;  // 气泡右缘贴模块左缘
            else
                bubbleX = highlightArea.getRight() + 12;         // 气泡左缘贴模块右缘

            // 垂直居中于模块
            int bubbleY = highlightArea.getCentreY() - bubbleH / 2;

            // 夹紧到 Editor 可见范围
            bubbleX = juce::jlimit (8, juce::jmax (8, editorW - bubbleW - 8), bubbleX);
            bubbleY = juce::jlimit (4, juce::jmax (4, getHeight() - bubbleH - 4), bubbleY);

            return { bubbleX, bubbleY, bubbleW, bubbleH };
        }

        // --- STEP 1: 原有上下避让逻辑 ---
        const int cx = highlightArea.getCentreX();
        int bubbleX = cx - bubbleW / 2;

        // 尝试上方
        int bubbleY = highlightArea.getY() - bubbleH - 14;

        // 上方不够 → 尝试下方
        if (bubbleY < 10)
            bubbleY = highlightArea.getBottom() + 14;

        // 下方也超出 Editor → 直接居中放
        if (bubbleY + bubbleH > getHeight() - 8)
            bubbleY = (getHeight() - bubbleH) / 2;

        // 保底不超出顶部
        bubbleY = juce::jmax (4, bubbleY);

        // 不超出左右边界
        const int rightLimit = getWidth() - 16;
        bubbleX = juce::jlimit (8, juce::jmax (8, rightLimit - bubbleW), bubbleX);

        return { bubbleX, bubbleY, bubbleW, bubbleH };
    }

    void drawSpeechBubble (juce::Graphics& g)
    {
        if (currentStep == Step::hidden) return;

        auto bubble = getBubbleBounds();

        // 外壳
        PinkXP::drawRaised (g, bubble, bubbleHovered ? PinkXP::pink200 : PinkXP::btnFace);

        auto inner = bubble.reduced (6);
        if (inner.getHeight() < 40) return;

        // 标题行
        auto titleRow = inner.removeFromTop (juce::jmin (18, inner.getHeight() - 24));
        g.setColour (PinkXP::sel);
        g.fillRect (titleRow);
        g.setColour (PinkXP::dark);
        g.fillRect (titleRow.getX(), titleRow.getBottom(), titleRow.getWidth(), 1);

        // 标题
        g.setColour (PinkXP::selInk);
        g.setFont (PinkXP::getFont (10.0f, juce::Font::bold));
        const juce::String title = (currentStep == Step::step1)
            ? (menuIsOpen ? "NOW CHOOSE IT!" : "WELCOME 2 Y2KMETER!")
            : "ALMOST THERE!";
        g.drawText (title, titleRow.reduced (3, 1), juce::Justification::centredLeft, false);

        // 正文
        auto body = inner.reduced (2, 2);
        g.setColour (PinkXP::ink);
        g.setFont (PinkXP::getFont (9.0f));
        const juce::String text = (currentStep == Step::step1)
            ? (menuIsOpen
                ? "Click 'Tamagotchi' in\nthe menu to add ur pet! <3"
                : "Right-click the canvas to\nadd ur Tamagotchi pet! <3")
            : "Play some audio to\nhatch the egg! :3";
        g.drawFittedText (text, body, juce::Justification::centred, 2);

        // 右上角 × 关闭按钮（参考 ChromeHiddenOverlay 的浮动关闭按钮样式）
        {
            auto cb = getCloseButtonBounds();
            juce::Graphics::ScopedSaveState save (g);
            g.setOpacity (closeBtnHovered ? 1.0f : 0.55f);

            if (closeBtnPressed)
                PinkXP::drawPressed (g, cb, PinkXP::pink100);
            else
                PinkXP::drawRaised  (g, cb, closeBtnHovered ? PinkXP::pink200 : PinkXP::btnFace);

            g.setColour (PinkXP::ink);
            g.setFont   (PinkXP::getFont (11.0f, juce::Font::bold));
            auto cbText = cb;
            cbText.translate (-1, -1);
            if (closeBtnPressed) cbText.translate (1, 1);
            g.drawText ("x", cbText, juce::Justification::centred, false);
        }
    }
};

// ==========================================================
// Y2KmeterAudioProcessorEditor —— 多模块框架外壳
// 窗口：960 × 640（Pink XP 桌面 + 凸起主窗口 + 粉色标题栏）
// 中部放置 ModuleWorkspace，所有分析模块都作为子模块存在
// ==========================================================

juce::Font Y2KmeterAudioProcessorEditor::getCustomFont(float height, int styleFlags)
{
    return PinkXP::getFont(height, styleFlags);
}

Y2KmeterAudioProcessorEditor::Y2KmeterAudioProcessorEditor(Y2KmeterAudioProcessor& p)
    : juce::AudioProcessorEditor(&p),
      processor(p),
      // 根据 AudioProcessor::wrapperType 一次性判定是否插件宿主模式。
      // JUCE 在 AudioProcessor 构造时会把本次加载的 wrapper 类型填好
      // （VST3 / AU / AAX / LV2 / Standalone / Undefined 等），对于
      // Standalone 以外的一切情况我们都视为"插件宿主模式"。
      isPluginHost (p.wrapperType != juce::AudioProcessor::wrapperType_Standalone)
{
    // 0) 先加载 typeface / logo 到实例成员（避免 static 跨 DLL 卸载导致崩溃）
    customTypeface = PinkXP::loadActiveTypeface();
    logoImage = juce::ImageFileFormat::loadFrom(BinaryData::logo_png,
                                                 (size_t) BinaryData::logo_pngSize);

    initLookAndFeel();

    // 1) 先创建 ChromeHiddenOverlay 并以 invisible 状态加入 —— 让它成为"最底层"子组件
    //    （后续 workspace / 其它组件 addAndMakeVisible 时都会排到它之上）。
    //    这样 chrome 隐藏态下，overlay 的抬头文字和浮动关闭按钮会被模块自然遮挡。
    chromeHiddenOverlay = std::make_unique<ChromeHiddenOverlay> (*this);
    addChildComponent (*chromeHiddenOverlay);

    // 2) 创建 workspace 并挂到编辑器上（z-order 高于 overlay）
    workspace = std::make_unique<ModuleWorkspace>();
    workspace->setModuleFactory([this](ModuleType t) -> std::unique_ptr<ModulePanel>
    {
        return createModule(t);
    });
    workspace->setAvailableModuleTypes({
        ModuleType::eq,
        ModuleType::loudness,
        ModuleType::lufsRealtime,
        ModuleType::truePeak,

        // 模拟 VU 指针表（复用 Loudness 路的 RMS L/R，后端零新增计算）
        ModuleType::vuMeter,

        ModuleType::oscilloscope,
        ModuleType::oscilloscopeWave,

        ModuleType::spectrum,

        ModuleType::phase,
        ModuleType::phaseCorrelation,
        ModuleType::phaseBalance,

        ModuleType::dynamics,
        ModuleType::dynamicsMeters,
        ModuleType::dynamicsDr,
        ModuleType::dynamicsCrest,

        // 持续滚动瀑布波形（复用 Oscilloscope 原始样本，后端零新增计算）
        ModuleType::waveform,

        // 实时频谱瀑布图（复用 Spectrum 路的 FFT 幅度，后端零新增计算）
        ModuleType::spectrogram,

        // 3D 频谱瀑布图（45° 俯视三维曲面效果）
        ModuleType::spectrogram3d,

        // 独立小宠物模块（右键/双击空白区添加）
        ModuleType::tamagotchi,

        // Milkdrop WebGL 可视化模块（WebView 嵌入 Butterchurn 引擎）
        ModuleType::milkdrop
    });

    addAndMakeVisible(*workspace);

    // 2) 先给编辑器设尺寸 —— 触发 resized() 让 workspace 拿到实际 bounds
    //    这样后续 addModuleByType 走 findNextSlot 时 canvas 不再是 0x0
    //
    //   尺寸上限分模式：
    //     · Standalone：1600×1100 —— 默认预设下的合理上限；切换到"横向铺满"
    //       类预设时 applyLayoutPreset 会临时把上限抬到屏幕宽度，预设回 1 时
    //       会再夹回 1600×1100，避免用户手动拖得过大错位。
    //     · VST3 / AU 插件宿主：8192×8192 —— 远大于常规显示器分辨率。插件模式
    //       下布局预设下拉被隐藏（不会再有地方动态抬高上限），如果仍用 1600×1100
    //       会被 JUCE 的 ComponentBoundsConstrainer 死死夹住；用户在 DAW 里
    //       拉宽窗口到 1600 就拉不动了。给一个足够大的上限让宿主自由 resize。
    //
    //   尺寸下限同样分模式：
    //     · Standalone：640×420 —— 保证伪标题栏 + 底部 toolbar + 至少一行模块
    //       能完整显示，避免用户误缩到按钮挤压错位。
    //     · VST3 / AU 插件宿主：320×240 —— 插件模式下不画伪标题栏（chrome 由宿主
    //       提供），底部 toolbar 里大量 UI 也被隐藏（Source 下拉、布局预设下拉等），
    //       实际需要的最小可用宽度远小于 Standalone；若仍沿用 640×420，用户在 DAW
    //       里就会"缩到一半缩不动了"。给到 320×240 让宿主充分自由。
    setResizable(true, true);
    if (isPluginHost)
        setResizeLimits(50, 50, 8192, 8192);
    else
        setResizeLimits(640, 420, 1600, 1100);

    // ------------------------------------------------------------------
    // v1.8.3：从 Processor state 恢复"布局锁定态"。
    //   · 初始化时已确保 workspace / 顶层窗口已就位（workspace 上面刚 make_unique
    //     + addAndMakeVisible；顶层窗口则交给 setResizable 自己取）。
    //   · initial=true 避免回写 processor（保持与已保存一致即可）；
    //   · 若 processor 未保存过（缺省 false），则仅刷新一下按钮视觉即 no-op，
    //     applyLayoutLocked 内部会将 setResizable(true,true) 重推一次——与上面刚刚手动
    //     setResizable(true,true) 同步，安全。
    applyLayoutLocked (processor.getLayoutLocked(), /*initial*/ true);
    // ------------------------------------------------------------------

    // 插件模式（VST3/AU）：若 host state 里保存过上次 Editor 关闭时的窗口尺寸，
    //   优先用它恢复，实现"每次重开窗口保持上次大小"（尤其是 FL Studio Windows 版
    //   本身不会记住 VST3 窗口尺寸）。
    //   Standalone 模式由 PropertiesFile 体系记忆位置+尺寸，不走这里。
    //
    //   下限校验与 resized() 的回写下限 (400×300) 保持一致：若历史 state 里
    //   残留了异常小尺寸（例如早期版本曾回存过 FL Studio 关闭子窗口时的
    //   ~175×85 中间态），直接视为无效并回退到默认 960×640，不要盲目 setSize
    //   让用户看到一个几乎看不见的小窗口。
    constexpr int kMinRestoreW = 400;
    constexpr int kMinRestoreH = 300;
    const int savedW = processor.getSavedEditorWidth();
    const int savedH = processor.getSavedEditorHeight();

    if (isPluginHost
        && savedW >= kMinRestoreW
        && savedH >= kMinRestoreH)
    {
        const int maxW = 8192, maxH = 8192;
        const int w = juce::jlimit (kMinRestoreW, maxW, savedW);
        const int h = juce::jlimit (kMinRestoreH, maxH, savedH);
        setSize (w, h);
    }
    else
    {
        setSize(960, 640);
    }

    // 3) 再装载默认/已保存模块
    loadInitialModules();

    // 4) 用户布局变更 → 立即写回 Processor state
    workspace->onLayoutChanged = [this]()
    {
        processor.setSavedLayoutXml(workspace->saveLayoutAsXml());
    };

    // 4.b) P4：host 调用 getStateInformation 前，Processor 会触发此钩子，
    //      把 ModuleWorkspace 里 debounce 合并中的变更立即 flush，并
    //      同步回写最新 XML 到 savedLayoutXml，避免保存到工程的布局
    //      落后 16ms 的"上一版"。
    processor.flushPendingUiStateBeforeSave = [this]()
    {
        if (workspace != nullptr)
        {
            workspace->flushPendingLayoutChange();
            // flushPendingLayoutChange 内部若有待决通知会同步回调
            // onLayoutChanged → setSavedLayoutXml；若无待决，这里兜底
            // 再写一次也廉价（仅字符串赋值），保证一致性。
            processor.setSavedLayoutXml (workspace->saveLayoutAsXml());
        }
    };

    // 4.05) 布局预设切换：Preset 1 = 默认布局 + 默认窗口大小；
    //       Preset 2 = 把顶层窗口宽度拉到当前屏幕宽，并让默认模块横向等分 canvas。
    //       实现细节：
    //         · 先清空 workspace 的所有现有模块 / 拼豆贴画；
    //         · Preset 1：恢复 Editor 尺寸到 960×640，然后调 loadInitialModules()
    //           的"默认分支"（强制传空 XML 以绕过恢复路径）；
    //         · Preset 2：取当前所在屏幕 userArea 的宽度，调整顶层窗口宽度到该值，
    //           顶层窗口 x 移到屏幕左边；workspace 的 canvas 拿到后按横向等分
    //           7 个默认模块。
    //         · 切换后手动 notifyLayoutChanged 以把新布局回写 Processor。
    workspace->onLayoutPresetChanged = [this](ModuleWorkspace::LayoutPreset preset)
    {
        // 切换布局预设期间抑制 auto-show，避免窗口瞬间跳动到屏幕边缘时
        // 鼠标被判定为"进入窗口"而触发临时 chrome 展开，挤占模块区域。
        ++suppressAutoShowCounter;

        // ★ 预设切换前：自动退出 auto-hide 模式 + 解除布局锁定。
        //   否则 applyLayoutPreset 中的 setSize/setBounds 会被
        //   applyLayoutLocked 锁定的 resizeLimits(w,h,w,h) 夹回
        //   当前收缩尺寸，导致预设宽度/高度无法正确应用。
        if (autoHideMode)
        {
            autoHideMode = false;
            if (workspace != nullptr) workspace->setAutoHideActive (false);
            hasSavedBoundsBeforeHide = false;
        }

        if (layoutLocked)
            applyLayoutLocked (false, /*initial*/ false);

        // 确保 workspace chrome 可见：applyLayoutPreset 通过
        // getCanvasArea() 反推 overheadH 时依赖 chromeVisible 状态；
        // chrome 隐藏时 canvas 满铺导致 overheadH 少算 toolbarHeight(36px)，
        // 最终窗口高度比预期多 36px。
        if (workspace != nullptr && ! workspace->isChromeVisible())
            workspace->setChromeVisible (true);

        autoHideNeedsExitFirst = false;

        // v1.9.x：新手引导期间的预设切换处理
        //   · 切换到非 default 预设 → 跳过引导
        //   · 切换回 default 时若之前被跳过 → 重新触发
        if (tutorialStep != TutorialStep::hidden && preset != ModuleWorkspace::LayoutPreset::defaultGrid)
        {
            skipTutorial();
        }
        else if (tutorialStep == TutorialStep::hidden && tutorialWasSkipped
                 && preset == ModuleWorkspace::LayoutPreset::defaultGrid)
        {
            startTutorial();
        }

        applyLayoutPreset ((int) preset);
        --suppressAutoShowCounter;
    };

    // 4.05b) 预设 Save/Load —— 仅做透传：workspace 已经弹完 FileChooser，
    //        Editor 没有 settings 文件的写入能力（PropertiesFile 归 Standalone App
    //        持有），所以把 File 原样转发给外层订阅者（Y2KStandaloneApp）。
    //        外层未订阅时 onSave/LoadSettingsRequested 为空回调 → 点击按钮无效
    //        但不崩溃（VST3/AU 插件模式下即使按钮被误显也安全）。
    workspace->onSavePresetRequested = [this](juce::File dest)
    {
        if (onSaveSettingsRequested) onSaveSettingsRequested (dest);
    };
    workspace->onLoadPresetRequested = [this](juce::File src)
    {
        if (onLoadSettingsRequested) onLoadSettingsRequested (src);
    };

    // 4.1) chrome 可见性变化 → 隐藏/显示顶部 TitleBar
    //     切换时需要 resized() 重新布局（隐藏后 workspace 应占满整个窗口），
    //     并整体 repaint 而不仅仅 repaint 标题栏矩形。
    //     模块 y 坐标不做任何补偿，靠 workspace 自身位置变化带动模块整体平移。
    workspace->onChromeVisibleChanged = [this](bool visible)
    {
        // ★ 整个 chrome 显隐回调期间抑制 auto-show，防止窗口 resize
        //   （topComp->setBounds）过程中 JUCE 分发虚假 mouseEnter/mouseMove
        //   事件导致误触发 hover-show。首次启动时此路径不经过
        //   onLayoutPresetChanged 的保护，需要独立包裹。
        //   使用计数器而非 bool：setBounds() 会 post 异步 mouse 事件，
        //   回调返回后 counter>0 仍能拦截 ~300ms（3 个 timer tick）内的异步事件。
        suppressAutoShowCounter = 3;

        const bool toHide = ! visible;           // 本次切换是"进入隐藏态"吗？

        // 切换态时重置 hover/pressed，避免残留视觉
        closeButtonHovered = closeButtonPressed = false;
        pinButtonHovered   = pinButtonPressed   = false;
        minButtonHovered   = minButtonPressed   = false;

        // ========================================================
        // v1.8.6：auto-hide 模式 —— HIDE 按钮按下后自动固定+置顶，
        //   鼠标悬停暂显 chrome，离开恢复隐藏。
        //   · 进入隐藏：autoHide=true、layoutLocked=true、alwaysOnTop=true
        //   · 临时显隐（鼠标 hover）：不触动 lock/pin 状态，不缩放窗口
        //   · 真正 Show（用户点 Show 按钮）：解锁、取消 autoHide，但保留置顶状态
        //   · 窗口缩放：首次 HIDE / 显式 SHOW / hover 进出 均会执行
        // ========================================================
        const bool wasInAutoHide = autoHideMode;
        bool shouldShrink = false;
        bool shouldExpand = false;
        bool isTemporaryResize = false;  // hover 触发 vs 按钮点击触发

        if (toHide)
        {
            // 首次 HIDE → 进入 auto-hide 模式（lock + pin 仅设一次）+ 收缩窗口
            if (! autoHideMode)
            {
                autoHideMode = true;
                if (workspace != nullptr) workspace->setAutoHideActive (true);
                applyLayoutLocked (true, /*initial*/ false);
                setAlwaysOnTopActive (true);
                shouldShrink = true;
            }
            else
            {
                // Hover-hide（鼠标移出窗口）：重新收缩窗口，保持 autoHide 状态不变
                shouldShrink = true;
                isTemporaryResize = true;
            }
        }
        else // showing chrome
        {
            if (autoHideMode && ! temporaryChromeShow)
            {
                // 用户点 Show 按钮：真正退出 autoHide + 展开窗口
                autoHideMode = false;
                if (workspace != nullptr) workspace->setAutoHideActive (false);
                applyLayoutLocked (false, /*initial*/ false);
                shouldExpand = true;
            }
            else if (autoHideMode)
            {
                // Hover-show（鼠标移入窗口）：临时展开窗口，保持 autoHide/lock/pin 不变
                shouldExpand = true;
                isTemporaryResize = true;
            }
        }

        // ------------------------------------------------------------
        // Hide 收缩窗口 / Show 反向展开（仅在首次 HIDE / 显式 SHOW 时）
        //   · Hide：窗口高度 -shrink（上半屏顶边固定、下半屏底边固定），
        //            workspace 占满整窗后，模块屏幕位置自然贴向对应屏幕边。
        //   · Show：严格还原 Hide 前的完整 bounds 快照（幂等），不做浮动计算。
        //
        // 过渡期保护：ModuleWorkspace::setChromeVisible 会开启
        //   chromeTransitionActive，使过渡期内的 resized() 跳过模块 clamp，
        //   避免模块高度在中间帧被错误压缩。
        //
        // 启动守卫 isShowing()：Standalone App 恢复持久化状态时会调
        //   setChromeVisible，此时 Editor 还未上屏，不应改窗口尺寸。
        // ------------------------------------------------------------
        constexpr int kToolbarHeight = 36; // 与 ModuleWorkspace::toolbarHeight 保持一致
        const int shrink = titleBarHeight + kToolbarHeight; // 62px

        auto* topComp = getTopLevelComponent();
        const bool isStandalone = juce::JUCEApplicationBase::isStandaloneApp()
                                  && topComp != nullptr && topComp != this;
        // 只在 Standalone 下真正调整顶层窗口尺寸。插件宿主（VST3/AU 等）由 DAW
        //   管理窗口，插件擅自 setSize 极易与宿主打架（被宿主拉回、视觉抖动）。
        //   宿主模式下走"不改窗口"兜底路径，chromeDim 已让 Editor::resized 让出
        //   titleBarHeight 给 workspace，模块会自然跟随 workspace 上移/下移。
        const bool canResizeWindow = isShowing() && isStandalone && ! isPluginHost;

        // 判断当前窗口处于屏幕"上半"还是"下半"：
        //   · Hide 路径用"切换前"的位置（当前就是切换前，因为 setBounds 还没执行）
        //   · Show 路径用"当前"位置（窗口目前仍处在被 Hide 后的收缩态）。
        //     由于 Hide 时只改了高度和可能的 y，上/下半判断对 Show 仍然一致。
        bool bottomAligned = false;
        if (canResizeWindow && topComp != nullptr)
        {
            const auto topScreenBounds = (topComp == this) ? getScreenBounds() : topComp->getScreenBounds();
            const auto* display = juce::Desktop::getInstance()
                                     .getDisplays()
                                     .getDisplayForRect (topScreenBounds);
            const auto userArea = (display != nullptr) ? display->userArea
                                                        : juce::Rectangle<int> (1280, 720);
            const int windowCenterY = topScreenBounds.getCentreY();
            const int screenCenterY = userArea.getCentreY();
            bottomAligned = (windowCenterY > screenCenterY);
        }

        // 拼豆贴画与模块共享 workspace 坐标系，会随 workspace 原点一起平移，
        //   因此无需在此处单独补偿。

        // 切换 overlay 可见性：仅在 hide 态下显示抬头文字 + 浮动按钮
        if (chromeHiddenOverlay != nullptr)
            chromeHiddenOverlay->setVisible (toHide);

        // ------------------------------------------------------------
        // 实际收缩 / 展开顶层窗口（仅在 canResizeWindow 时执行）
        //   先 setBounds（会触发 Editor::resized → workspace 重新布局），
        //   再 repaint，避免闪烁。
        //
        //   幂等化关键：Hide 时快照切换前窗口 bounds + resizeLimits，
        //     Show 时直接 setBounds 回快照值，不做"当前 ± shrink"浮动计算。
        //     这样可彻底消除累积偏差（DocumentWindow inset、DPI 舍入、
        //     bottomAligned 翻转、constrainer 夹紧等），N 次 Hide→Show 循环
        //     后窗口 bounds 严格等于初始值。
        // ------------------------------------------------------------
        if (canResizeWindow && (shouldShrink || shouldExpand))
        {
            if (shouldShrink)
            {
                // 临时收缩（hover-hide）使用首次 HIDE 时保存的快照，不重新取值
                if (! isTemporaryResize)
                {
                    // 1) 快照当前完整 bounds + resizeLimits（将在 Show 时原样还原）
                    savedTopBoundsBeforeHide = topComp->getBounds();
                    if (auto* cbc = getConstrainer())
                    {
                        savedResizeMinW = cbc->getMinimumWidth();
                        savedResizeMinH = cbc->getMinimumHeight();
                        savedResizeMaxW = cbc->getMaximumWidth();
                        savedResizeMaxH = cbc->getMaximumHeight();
                    }
                    hasSavedBoundsBeforeHide = true;
                }

                // 2) 计算 Hide 后的目标 bounds（基于完整快照）
                //    · 上半屏：顶边固定，高度 -shrink（底边上移 shrink）
                //    · 下半屏：底边固定，顶边 +shrink（高度 -shrink）
                const auto b = savedTopBoundsBeforeHide;
                const int newY = bottomAligned ? (b.getY() + shrink) : b.getY();
                const int newH = juce::jmax (1, b.getHeight() - shrink);

                // 3) 放宽 resizeLimits 以让 newH 能被接受（不让 min 夹回）
                //   · Editor 自身的 constrainer
                if (getConstrainer() != nullptr)
                {
                    setResizeLimits (juce::jmin (savedResizeMinW, b.getWidth()),
                                     juce::jmin (savedResizeMinH, newH),
                                     juce::jmax (savedResizeMaxW, b.getWidth()),
                                     juce::jmax (savedResizeMaxH, b.getHeight()));
                }
                //   · 顶层 ResizableWindow 的 constrainer（applyLayoutLocked 锁在此处）
                if (auto* rw = dynamic_cast<juce::ResizableWindow*> (topComp))
                {
                    rw->setResizeLimits (juce::jmin (savedResizeMinW, b.getWidth()),
                                         juce::jmin (savedResizeMinH, newH),
                                         juce::jmax (savedResizeMaxW, b.getWidth()),
                                         juce::jmax (savedResizeMaxH, b.getHeight()));
                }

                // 4) 应用收缩后的窗口 bounds
                topComp->setBounds (b.getX(), newY, b.getWidth(), newH);
            }
            else
            {
                // Show 路径：严格还原到 Hide 前的快照，绝不基于当前窗口做浮动计算
                if (hasSavedBoundsBeforeHide)
                {
                    // 1) 先放宽 resizeLimits 让快照 bounds 能被接受
                    if (getConstrainer() != nullptr)
                    {
                        setResizeLimits (juce::jmin (savedResizeMinW, savedTopBoundsBeforeHide.getWidth()),
                                         juce::jmin (savedResizeMinH, savedTopBoundsBeforeHide.getHeight()),
                                         juce::jmax (savedResizeMaxW, savedTopBoundsBeforeHide.getWidth()),
                                         juce::jmax (savedResizeMaxH, savedTopBoundsBeforeHide.getHeight()));
                    }

                    // v1.9.0: 若处于布局锁定态，shrink 路径的 applyLayoutLocked 会把
                    //   顶层 ResizableWindow 的 limits 锁死在收缩后的尺寸上。此处
                    //   必须在 setBounds 扩展窗口前同样放松 RW 层的 limits，否则
                    //   native 窗口受 OS 级 constrainer 约束无法扩回原尺寸，
                    //   导致模块渲染区域被压缩。扩展完成后再由后续的
                    //   applyLayoutLocked(true) 重新锁到当前（扩展后）尺寸。
                    if (layoutLocked)
                    {
                        if (auto* rw = dynamic_cast<juce::ResizableWindow*> (topComp))
                        {
                            rw->setResizeLimits (
                                juce::jmin (savedResizeMinW, savedTopBoundsBeforeHide.getWidth()),
                                juce::jmin (savedResizeMinH, savedTopBoundsBeforeHide.getHeight()),
                                juce::jmax (savedResizeMaxW, savedTopBoundsBeforeHide.getWidth()),
                                juce::jmax (savedResizeMaxH, savedTopBoundsBeforeHide.getHeight()));
                        }
                    }

                    // 2) 直接还原完整 bounds（x/y/w/h 全部等于 Hide 前快照）
                    topComp->setBounds (savedTopBoundsBeforeHide);

                    // 3) 还原 resizeLimits（永久展开时）；临时展开不还原，保持宽松
                    //    以便下次 hover-hide 能顺利收缩。
                    if (! isTemporaryResize)
                    {
                        //    场景：快照的 savedResizeMin/Max 是"Hide 按下那一刻"的
                        //      值——而 resizeLimits 未持久化，启动后仅恢复模块 XML
                        //      不会重跑 preset 扩展 limits；一旦 savedTopBoundsBeforeHide
                        //      来自 horizontal bar 预设（2558×246 这种超出默认 limits
                        //      的尺寸），直接写回默认 limits（1600×1100）会把它夹
                        //      成1600×420。
                        //    解决：min 向下夹到当前宽高、max 向上抬到当前宽高，其
                        //      余保持快照值。对 default/tiled 等天然在默认 limits 内的
                        //      预设无影响；对 horizontal bar 则把上限抬到 screenW。
                        const auto restoredBounds = savedTopBoundsBeforeHide;
                        setResizeLimits (juce::jmin (savedResizeMinW, restoredBounds.getWidth()),
                                         juce::jmin (savedResizeMinH, restoredBounds.getHeight()),
                                         juce::jmax (savedResizeMaxW, restoredBounds.getWidth()),
                                         juce::jmax (savedResizeMaxH, restoredBounds.getHeight()));

                        hasSavedBoundsBeforeHide = false;
                    }
                }
                else
                {
                    // 防御性路径（当前启动流程下不可达）：
                    //   Standalone App 在启动时会先 setBounds(show 态)
                    //   → setChromeVisible(false) 走标准 Hide 路径 → 写入
                    //   hasSavedBoundsBeforeHide。因此用户首次点 Show 必走主路径。
                    //   保留此分支是为了防御未来启动流程重构后出现"无快照
                    //   但需要 Show"的退化情形，避免被 constrainer 夹成狭长矩形。
                    auto b = topComp->getBounds();
                    const int newY = bottomAligned ? (b.getY() - shrink) : b.getY();
                    const int newH = b.getHeight() + shrink;

                    if (auto* cbc = getConstrainer())
                    {
                        setResizeLimits (juce::jmin (cbc->getMinimumWidth(),  b.getWidth()),
                                         juce::jmin (cbc->getMinimumHeight(), newH),
                                         juce::jmax (cbc->getMaximumWidth(),  b.getWidth()),
                                         juce::jmax (cbc->getMaximumHeight(), newH));
                    }

                    topComp->setBounds (b.getX(), newY, b.getWidth(), newH);
                }
            }
        }

        // ★ v1.8.6 修复：chromeDim 必须在窗口 resize 完成后才设置，避免
        //   chromeDim=true 但窗口未实际缩小导致的 workspace 越界空余。
        chromeDim = toHide;

        // 任何 chrome 显隐切换都需要让 Editor 根据 chromeDim 重新分配内部区域：
        //   · chrome 显示 → workspace 从顶部让出 titleBarHeight
        //   · chrome 隐藏 → workspace 占满整窗
        // 窗口缩放路径中 topComp->setBounds 已触发一次，这里再调一次确保最终态正确。
        resized();
        repaint();

        // v1.9.0 修复：shrink/expand 代码块会放宽顶层 ResizableWindow 的
        //   resize limits（rw->setResizeLimits）以让窗口成功缩小/放大。
        //   但若当前处于布局锁定态（L 按钮按下），放宽后的 limits 会绕过锁定，
        //   导致用户即使锁定状态下也能拉动窗口边缘调整大小。
        //   此处在 resize 完成后重新调用 applyLayoutLocked(true)，
        //   将 limits 重新夹紧到当前窗口尺寸，恢复 lock 对 resize 的约束。
        //   注意：只有当 layoutLocked=true 且确实发生了 shrink/expand 时才需要；
        //   permanent show（退出 auto-hide）路径中 layoutLocked 已被设为 false。
        if (layoutLocked && canResizeWindow && (shouldShrink || shouldExpand))
            applyLayoutLocked (true, /*initial*/ false);

        // auto-hide 守卫：shouldShrink 后在窗口 resize 完成后设置标志，因此
        //   resize 引发的虚假 mouseEnter 不会触发。mouseExit 无条件清除，
        //   用户下次 mouseEnter 即可正常触发 hover show。
        //  shouldExpand（用户点 Show）时清除标志，恢复正常。
        if (shouldShrink)
        {
            autoHideNeedsExitFirst = true;

            // 上半屏 shrink 后底边上移，鼠标可能已经落在窗口外。
            // 此时"先离开一次"的条件天然满足，应直接清零 guard，
            // 否则首次移入会被拦截，用户需要移出再移入两次才能触发 auto-show。
            if (auto* top = getTopLevelComponent())
            {
                const auto topScreenBounds = (top == this) ? getScreenBounds() : top->getScreenBounds();
                const auto mouseScreen = juce::Desktop::getInstance().getMainMouseSource().getScreenPosition().toInt();
                if (! topScreenBounds.contains (mouseScreen))
                    autoHideNeedsExitFirst = false;
            }
        }
        else if (shouldExpand)
        {
            autoHideNeedsExitFirst = false;
        }

        // 切换后同步 workspace 的 hit-test 挖洞：让 overlay 按钮/文字区的鼠标事件
        //   冒泡到 Editor，从而 Editor 能直接接管浮动按钮的 hover/press 逻辑，
        //   同时任何压在按钮之上的模块都能正常独占鼠标事件（JUCE 先派发给子组件）。
        updateWorkspaceHitTestHoles();

        // suppressAutoShowCounter 由 timerCallback 递减，此处不清零（异步事件窗口保护）
    };

    // 4.1b) 鼠标进入 workspace → auto-hide 模式下暂显 chrome
    //   · workspace 覆盖整个 Editor 时 Editor::mouseEnter 可能不触发，
    //     workspace 自身的 mouseEnter 回调更可靠。
    workspace->onMouseEntered = [this]()
    {
        // 布局预设切换期间抑制 auto-show（见 onLayoutPresetChanged）
        if (suppressAutoShowCounter > 0)
            return;

        // Hide 后必须等鼠标先离开窗口一次，才允许鼠标再次进入时触发 hover show
        if (autoHideNeedsExitFirst)
            return;

        if (autoHideMode && workspace != nullptr && ! workspace->isChromeVisible())
        {
            temporaryChromeShow = true;
            workspace->setChromeVisible (true);
            temporaryChromeShow = false;
        }
    };

    // 4.1c) 鼠标在 workspace 内移动 → auto-hide 模式下暂显 chrome
    //   · mouseMove 比 mouseEnter 更可靠：resize 后 JUCE 内部
    //     isMouseOverOrDragging 状态可能导致 mouseEnter 被跳过，
    //     但 mouseMove 每次鼠标移动都会触发，不依赖 enter/exit 状态。
    //   · setChromeVisible 内部有 idempotent guard，重复调用是安全的。
    workspace->onMouseMoved = [this]()
    {
        // 布局预设切换期间抑制 auto-show（见 onLayoutPresetChanged）
        if (suppressAutoShowCounter > 0)
            return;

        if (autoHideNeedsExitFirst)
            return;

        if (autoHideMode && workspace != nullptr && ! workspace->isChromeVisible())
        {
            temporaryChromeShow = true;
            workspace->setChromeVisible (true);
            temporaryChromeShow = false;
        }
    };

    // 4.1d) 鼠标离开 workspace → auto-hide 模式下检测是否真正离开了顶层窗口
    //   · 仅判断是否在顶层窗口 bounds 外（不在任何 Editor/workspace 子区域）。
    //   · hide 态 workspace 占满整窗时 Editor::mouseExit 不触发，靠此路径。
    //   · 无论 chrome 当前是否可见，只要鼠标真的离开了窗口就必须清零
    //     autoHideNeedsExitFirst，否则后续 mouseEnter 永远被拦截。
    workspace->onMouseExited = [this](juce::Point<int> mouseScreenPos)
    {
        if (! autoHideMode || workspace == nullptr) return;

        auto* top = getTopLevelComponent();
        if (top == nullptr) return;

        const auto topScreenBounds = (top == this) ? getScreenBounds() : top->getScreenBounds();
        if (! topScreenBounds.contains (mouseScreenPos))
        {
            if (workspace->isChromeVisible())
                workspace->setChromeVisible (false);
            autoHideNeedsExitFirst = false;
        }
    };

    // 4.2) 音频源下拉变化 → 透传给外部（Standalone App 订阅后会真正切换音频设备）
    workspace->onAudioSourceChanged = [this](const juce::String& sourceId, bool isLoopback)
    {
        if (onAudioSourceChanged) onAudioSourceChanged (sourceId, isLoopback);
    };

    // 4.3) FPS 限制按钮切换 → 修改 AnalyserHub 的 FrameDispatcher 频率
    workspace->onFpsLimitChanged = [this](int hz)
    {
        // 记录用户期望值，并用自适应策略换算出当前实际下发的 hz。
        userRequestedFpsLimit = juce::jlimit (15, 120, hz);
        adaptiveDispatchHz    = isPluginHost ? juce::jmin (48, userRequestedFpsLimit)
                                             : juce::jlimit (15, 120, userRequestedFpsLimit + 5);
        adaptiveRecoverTicks  = 0;
        adaptiveDropTicks     = 0;

        processor.getAnalyserHub().startFrameDispatcher (adaptiveDispatchHz);
        // 立即重置统计起点，避免切换后站显示跨区间的均值
        frameCounter.store (0, std::memory_order_relaxed);
        lastFrameCounterSample = 0;
        lastFpsTimeMs = juce::Time::getMillisecondCounterHiRes();
    };

    // 4.4) gain 控制条变化 → 下发到分析输入增益（仅影响分析链，不影响透传输出）
    workspace->onInputGainChanged = [this](float db)
    {
        processor.setAnalysisInputGainDb (db);
    };
    workspace->setInputGainDb (processor.getAnalysisInputGainDb());

    // 订阅主题变更 → 切主题后刷新全局重绘
    themeSubToken = PinkXP::subscribeThemeChanged([this]()
    {
        if (auto* lnf = dynamic_cast<juce::LookAndFeel_V4*>(&getLookAndFeel()))
            juce::ignoreUnused(lnf);
        invalidateDesktopCache();
        repaint();
    });

    // 应用默认主题（会把全局配色重写一次，确保状态一致）
    PinkXP::applyTheme(PinkXP::getCurrentThemeId());

    // 构造阶段 isShowing() 可能仍为 false，先确保分析链启动；
    // 后续由 visibilityChanged() 继续做可见性联动。
    processor.setAnalysisActive(true);

    // Editor 自身 10Hz 轮询：拉 Processor 的 CPU 占比并广播到所有模块的
    //   右下角标签，同时计算实际 FPS 下发给 workspace 的 FPS 标签
    startTimerHz (10);

    // Phase F：启动全局 FrameDispatcher（默认 30Hz 统一滚 UI 分发、模块后续订阅）
    //   模块构造中的 retain() 已经让 refCounts 就绪，这里开 Timer 即可开始工作。
    userRequestedFpsLimit = juce::jlimit (15, 120, workspace->getFpsLimit());
    adaptiveDispatchHz    = isPluginHost ? juce::jmin (48, userRequestedFpsLimit)
                                         : juce::jlimit (15, 120, userRequestedFpsLimit + 2);
    adaptiveRecoverTicks  = 0;
    adaptiveDropTicks     = 0;
    processor.getAnalyserHub().startFrameDispatcher (adaptiveDispatchHz);

    // 订阅帧分发，以计算实际 FPS（内部辅助类，避免 header 对 AnalyserHub 的硬依赖）
    fpsListener = std::make_unique<FpsFrameListener> (*this);
    processor.getAnalyserHub().addFrameListener (fpsListener.get());
    lastFpsTimeMs = juce::Time::getMillisecondCounterHiRes();

    // ------------------------------------------------------
    // 插件宿主（VST3 / AU / AAX / LV2）模式下的一次性差异化配置
    //   · 隐藏底部 Toolbar 中的"Source"下拉（宿主直接提供音频）
    //   · 不绘制自画伪标题栏 / 右上三按钮 / 窗口拖拽（由宿主窗口边框负责）
    //   · 不启用"默认置顶"行为（对宿主无意义，且多数宿主会忽略该调用）
    //   · 关闭 chromeHiddenOverlay（插件下永远不会进入 Hide 态的浮层）
    // 注：paint/resized/mouseXxx 中会再次基于 isPluginHost 做绕行，
    //     此处只是一次性的初始状态设置。
    // ------------------------------------------------------
    if (isPluginHost)
    {
        if (workspace != nullptr)
        {
            workspace->setAudioSourceUiVisible (false);
            // 布局预设下拉在插件模式下同样隐藏：
            //   切换预设会改顶层窗口尺寸 / 位置（横向铺满屏幕等），和宿主窗口会打架；
            //   插件下始终使用默认预设即可，无需用户选择。
            workspace->setLayoutPresetUiVisible (false);

            // 但 Save/Load 两个按钮仍然保留 —— 用户依然需要在 DAW 里导入/导出
            //   预设文件（与 Standalone Save 出的 .settings 互通）。按钮独立贴在
            //   Grid 左侧显示。
            workspace->setSaveLoadUiVisible (true);
        }

        // 插件模式下 Save/Load 不经过 Standalone App，直接由 Editor 就地处理：
        //   · Save：把 Processor::getStateInformation 的数据包装成与 Standalone
        //     完全兼容的 .settings 文件（<PROPERTIES> 外壳 + base64 编码的
        //     filterState 条目），Standalone 下一次打开该文件也能正常恢复。
        //   · Load：容错读取三种格式
        //       a) Standalone .settings —— 解析 <PROPERTIES> 里 filterState 条目，
        //          base64 解码 + copyXmlToBinary 反序列化为 MemoryBlock，再喂给
        //          Processor::setStateInformation；
        //       b) 裸 PBEQ_State XML（Processor state 原生 XML 形态）；
        //       c) 裸 PBEQ_Layout XML（仅布局，不含其它）—— 直接灌给 workspace。
        //     成功后调用 workspace->loadLayoutFromXml 热重载，无需重启宿主。
        //
        //   注：插件模式下"主题/窗口尺寸/音频源"等 runtime 状态由 DAW 或 Editor 自身
        //   管理，Load 过来的 .settings 里的那些 key 在这里被有意忽略；我们只抽取
        //   和 Processor state 相关的部分 —— 这与"预设 = 模块布局"的用户心智一致。
        onSaveSettingsRequested = [this](juce::File dest)
        {
            if (dest == juce::File{}) return;
            saveStateAsSettingsFile (dest);
        };
        onLoadSettingsRequested = [this](juce::File src)
        {
            if (src == juce::File{} || ! src.existsAsFile()) return;
            loadStateFromSettingsFile (src);
        };

        // 插件模式下不画伪标题栏 / chrome 浮层
        chromeHiddenOverlay.reset();

        // 插件下不主动置顶（让宿主完全掌控窗口行为）
        alwaysOnTopActive             = false;
        initialAlwaysOnTopApplied     = true; // 抑制 visibilityChanged 里的首次推送

        // 触发一次重排：workspace 在插件模式下从 y=0 起铺满整个 Editor
        resized();
    }

    // ==================================================================
    // GPU 合成层挂载（Standalone + VST3 共用此入口）
    //
    //   · attachTo(*this) 之后，本 Editor 及其所有子组件（workspace、各
    //     ModulePanel、chromeHiddenOverlay）的 paint() 命令都会被 JUCE 翻译
    //     成 OpenGL 批次在 GPU 上执行。软光栅路径（CoreGraphicsContext::
    //     fillCGRect / drawGlyphs / fillPath / strokePath）彻底绕开，主线程
    //     只剩 draw 命令组装与提交，CPU 负载大幅下降。
    //
    //   · setContinuousRepainting(false)：**关键**。默认为 true 时，GL 上下文
    //     会按屏幕刷新率不停重绘，等于强制 60~120Hz，不受我们 FrameDispatcher
    //     的 adaptiveDispatchHz 控制。设为 false 后只在子组件调用 repaint()
    //     时才触发一次 GL 重绘，和现有的节流/脏区策略完美配合。
    //
    //   · setComponentPaintingEnabled(true) 是默认值，显式写出来只是强调
    //     "让 GL 上下文负责普通 JUCE 组件的 paint"，不是手动 glDraw。
    //
    //   · 插件宿主（VST3/AU）场景：
    //       - macOS：JUCE 为 Editor 顶层 NSView 创建一个 NSOpenGLContext 子层，
    //         宿主窗口其余部分不受影响。ProTools/Logic/Cubase/Live 等主流 DAW 均支持。
    //       - Windows：JUCE 为 HWND 创建一个子 GL 子窗口，同理。
    //     极少数老版 ProTools/AU-Hosts 对子 GL 上下文兼容性差；若将来发现某宿主
    //     花屏/黑屏，可在此处加 isPluginHost 保护把插件模式下回退到软光栅。
    //     当前按"两种模式都走 GPU"的需求开启。
    //
    //   · 析构顺序：openGLContext 声明在 Editor 类末尾 → 成员反向析构会最先析构
    //     GL 上下文（JUCE 会自动 detach），但为了防止 GL 资源释放时 child
    //     components 已被部分销毁的 UB，~Editor 起始处仍显式 detach() 一次兜底。
    //
    //   · **macOS 专项关闭**（2026-05 性能修复）：
    //       Apple 从 10.14 起把 OpenGL 标记为 Deprecated，legacy NSOpenGLContext 在
    //       Apple Silicon / 新版 Intel GPU 上走的是 Metal 兼容转译层；每次 CPU 端
    //       修改 juce::Image（Spectrogram 的 imageBuf、Oscilloscope 的 staticLayer/
    //       dynamicLayer、Editor 的 desktopCacheImage 等）都会触发"纹理失效 →
    //       整张重新 upload"的隐式同步，多个 SpectrogramModule / SpectrumModule /
    //       WaveformModule / OscilloscopeModule 叠加时会线性放大到几十 MB/帧的
    //       带宽，导致明显卡顿。macOS 下 CoreGraphics 本身已有 Metal 后端加速，
    //       直接走软光栅路径反而更快。
    //     Windows 侧 WGL + 硬件驱动对小批次 draw / 纹理部分更新优化成熟，保持开启。
    // ==================================================================
    //
    // ==================================================================
// ✅ v2.2.0 恢复 Editor 级 OpenGL 上下文 —— 核心目的：解决 Z-order 遮盖
    //
    //   Editor 级 GL (setComponentPaintingEnabled=true, attachTo(*this))
    //   使主窗口进入 GPU 合成管线，所有子组件（包括 Milkdrop GLView）的 paint
    //   输出通过 Editor 的 CachedImage FBO 在 GPU 侧统一合成 → 单次 SwapBuffers
    //   输出到屏幕。不存在"GL 原生 HWND 覆盖 GDI 内容"的 Z-order 冲突。
    //
    //   嵌套 GL 上下文说明：
    //     Editor GL + Milkdrop GLView GL (componentPaintingEnabled=false)
    //     两个 GL context 在同一窗口中共存。JUCE 官方不建议嵌套，但在
    //     Release 模式下 (NDEBUG) jassertfalse 编译为 no-op。实测 NVIDIA 驱动
    //     (nvoglv64.dll) 下嵌套 GL 工作正常—v2.1.9 (d39397e) 已验证 30fps 无遮盖。
    //     AMD (atidxx64.dll) 可能有兼容问题，但当前用户为 NVIDIA 环境。
    //
    //   macOS：CoreGraphics 已有 Metal 后端，Editor GL 反而降低性能，跳过 attach。
    // ==================================================================
#if ! JUCE_MAC
    openGLContext.setContinuousRepainting(false);
    openGLContext.setComponentPaintingEnabled(true);
    openGLContext.attachTo(*this);
#endif

    // Temporary profiling hook: with Y2K_ENABLE_PERF_COUNTERS=1 this writes
    // perf snapshots every 60 seconds to %APPDATA%/Y2Kmeter/perf_counters.
#if  Y2K_ENABLE_PERF_COUNTERS    
    processor.setPerfAutoExportEnabled (true);
#endif

    // ==================================================================
    // v1.9.x：新手引导覆盖层 —— 按需创建/销毁，不提前驻留在 child list 中
    //   · startTutorial() 时创建 + addChildComponent
    //   · dismissTutorialOverlay() 时 removeChildComponent + reset
    //   · 关键：引导不活动时 Editor 子组件列表中完全不存在此组件，
    //     从根本上杜绝 JUCE 子组件遍历 / OpenGL 渲染合成层的任何干扰
    // ==================================================================

    // 首次启动检测：非插件模式且 processor 未记录完成 → 启动引导
    if (! isPluginHost && ! processor.isTutorialCompleted())
        startTutorial();
}

Y2KmeterAudioProcessorEditor::~Y2KmeterAudioProcessorEditor()
{
    // 进入析构：后续宿主/JUCE 对 Editor 的任何 resize（包括 FL Studio 关闭
    //   嵌入子窗口时塞给我们的 ~175×85 极小中间态）都不应被 resized() 回写到
    //   Processor.savedEditorSize，否则下次打开窗口会恢复成异常小尺寸。
    editorBeingDestructed = true;

    // P4：先解绑 Processor → Editor 的 flush 钩子，避免析构阶段里
    //   宿主线程调 getStateInformation 触发回调到已部分销毁的 this。
    processor.flushPendingUiStateBeforeSave = nullptr;

    // -1) Editor 级 GL 上下文 detach：最先解除 GPU 绑定，确保 workspace 等子组件
    //      析构时 GL 资源（Texture/VBO/FBO）已经在上下文解除后安全释放。
    //      macOS 下构造时未 attach，这里同样跳过。
#if ! JUCE_MAC
    openGLContext.detach();
#endif

    // 0) 先停掉 Editor 自身的 timer，避免 workspace.reset() 中途被调
    stopTimer();

    // 兜底：若曾为 Tamagotchi 临时保活 Loudness，析构前配平 release。
    if (tamagotchiSignalRetained)
    {
        processor.getAnalyserHub().release (AnalyserHub::Kind::Loudness);
        tamagotchiSignalRetained = false;
    }

    // 取消订阅帧分发（在 stopFrameDispatcher 之前取消，更严谨）
    if (fpsListener != nullptr)
    {
        processor.getAnalyserHub().removeFrameListener (fpsListener.get());
    }

    // Phase F：停掉 FrameDispatcher，避免 workspace/模块离场后
    //   还有 UI Timer 回调到已析构的 FrameListener。
    processor.getAnalyserHub().stopFrameDispatcher();

    if (themeSubToken != 0)
    {
        PinkXP::unsubscribeThemeChanged(themeSubToken);
        themeSubToken = 0;
    }

    // 1) 释放 workspace（内部会停止所有 Timer、清空子组件）
    //    必须在清理 LookAndFeel / Typeface 之前完成
    workspace.reset();

    // Editor 销毁后关闭分析，避免后台继续进行无意义计算
    processor.setAnalysisActive(false);

    // 2) 解绑 LookAndFeel（本组件 + 自身）
    setLookAndFeel(nullptr);

    // 3) 关键：清空全局 Typeface 缓存，防止插件 DLL 卸载时
    //    BinaryData 内存已被释放而全局 gTypeface 仍持有悬垂引用导致宿主卡死
    PinkXP::initCustomTypeface(nullptr);

    // 3.1) 关键：清空 PinkXPStyle 的桌面纹理缓存（getSharedDesktopTexture）。
    //
    //   【此行是 2026-05 Windows VST3 FL Studio 删除插件卡死修复的核心】
    //
    //   根因：该缓存以 TU 内部 static vector 持有若干 juce::Image，Windows 上
    //   实际类型是 Direct2DPixelData，内部通过 SharedResourcePointer<DirectX>
    //   持有 ID3D11Device / DxgiAdapters 全局单例。若不在这里主动清理：
    //     · DLL 卸载时 CRT 走 execute_onexit_table 析构这些 Image；
    //     · Direct2DPixelData 析构 → DirectX 引用计数归零 → ID3D11Device 释放；
    //     · AMD 驱动 atidxx64.dll::CDevice::DestroyDriverInstance 内部需要
    //       等待 GPU worker thread 退出（GetExitCodeThread / SleepEx）；
    //     · 但此时 CRT 持有 atexit lock，loader lock 也在 FreeLibrary 链路里
    //       被持有，驱动 worker 线程无法完成 DLL 引用链处理 → 主线程永久等待。
    //   修复：在 Editor 析构时（此时 loader lock 未持有、audio 回调已停），
    //   主动把这些 Image 释放干净，让 DirectX 单例在此刻就自然析构，DLL 卸载
    //   时 atexit 链里就没有"等 GPU 驱动 worker 线程"这种重活。
    PinkXP::invalidateDesktopTextureCache();

    // 3.2) 关键：解除 LookAndFeel 对 Typeface 的引用。
    //   PinkXPLookAndFeel 是 TU 级 static 实例（生命周期与 DLL 相同），通过
    //   setDefaultSansSerifTypeface 持有 Typeface::Ptr。Windows 上
    //   DirectWriteTypeface 内部持有 SharedResourcePointer<Direct2DFactories>，
    //   不提前解引用的话，同样会在 atexit 阶段触发 DirectWrite / Direct2D
    //   全局资源的 DLL 卸载链析构。
    getPinkXPLookAndFeel().setDefaultSansSerifTypeface (nullptr);
    juce::LookAndFeel::getDefaultLookAndFeel().setDefaultSansSerifTypeface (nullptr);

    // 4) 清空 ImageCache（保险；我们自己不用它，但有些 JUCE 默认路径可能写入）
    juce::ImageCache::releaseUnusedImages();

    // 4.1) 清空 JUCE 内置 Typeface 缓存（juce::Font 查字型时内部 LRU；
    //   也可能持有 DirectWriteTypeface 实例 → 同样的 atexit 风险）。
    juce::Typeface::clearTypefaceCache();

    // 5) 实例成员 customTypeface / logoImage 会在随后自动析构，
    //    此时已没有任何 LookAndFeel / Font / Image 引用 BinaryData，安全
}

// ----------------------------------------------------------
// 初始化
// ----------------------------------------------------------
void Y2KmeterAudioProcessorEditor::initLookAndFeel()
{
    // 把字体注入全局 PinkXP，供所有模块使用
    PinkXP::initCustomTypeface(customTypeface);
    setLookAndFeel(&getPinkXPLookAndFeel());
}

void Y2KmeterAudioProcessorEditor::initWorkspace()
{
    // 已合并到构造器 + loadInitialModules()。保留空实现以兼容旧声明（若 header 仍声明）
    loadInitialModules();
}

void Y2KmeterAudioProcessorEditor::loadInitialModules()
{
    // 1) 优先恢复已保存布局
    const auto savedXml = processor.getSavedLayoutXml();
    if (savedXml.isNotEmpty() && workspace->loadLayoutFromXml(savedXml))
        return;

    // 2) 首次打开（无已保存布局）的默认预设：
    //    · Standalone 模式：Horizontal Bar(T)——保留老用户习惯（横向铺满屏幕顶部）；
    //    · 插件模式（VST3 / AU 等）：Default 预设——宿主 DAW 的子窗口不适合让
    //      Y2Kmeter 主动去改顶层窗口尺寸/位置（horizontalFull 会尝试把窗口拉到
    //      屏幕宽度并移到屏幕左上，和宿主嵌入布局冲突），因此首次使用"默认瀑布
    //      布局 + 默认 960×640 窗口"更安全、体验更一致。
    //    注意：仅变更首次默认入口，不影响预设列表里的 Default / Horizontal Bar 选项。
    const auto firstRunPreset = isPluginHost
                                    ? ModuleWorkspace::LayoutPreset::defaultGrid
                                    : ModuleWorkspace::LayoutPreset::horizontalFull;
    applyLayoutPreset ((int) firstRunPreset);
}

// ----------------------------------------------------------
// v1.9.x：新手引导流程控制（仅 Standalone 模式）
//   · startTutorial()       —— 启动 STEP 1：右键添加 Tamagotchi
//   · advanceTutorialStep2()—— 用户添加了 Tamagotchi → 推进到 STEP 2
//   · completeTutorial()    —— 宠物孵化完成 → 标记完成并持久化
//   · skipTutorial()        —— 用户切换预设时跳过引导
//   · dismissTutorialOverlay—— 隐藏覆盖层，清空 step 状态
//   · checkTutorialStep2Condition —— timer 中轮询：蛋是否孵化
// ----------------------------------------------------------

void Y2KmeterAudioProcessorEditor::startTutorial()
{
    // 仅 Standalone 生效；插件模式与已完成状态直接跳过
    if (isPluginHost || processor.isTutorialCompleted()) return;
    if (workspace == nullptr) return;

    // 按需创建覆盖层（不提前创建，避免干扰子组件事件路由）
    if (tutorialOverlay == nullptr)
    {
        tutorialOverlay = std::make_unique<TutorialOverlay>();
        addChildComponent (*tutorialOverlay);

        // 订阅 "右键点击聚光灯区域" → 打开模块选择菜单
        tutorialOverlay->onRightClickHighlight = [this](juce::Point<int> clickPos)
        {
            if (tutorialStep == TutorialStep::step1_rightClick && workspace != nullptr)
            {
                const auto screenPos = tutorialOverlay->localPointToGlobal (clickPos);
                const auto wsPos = workspace->getPosition();
                const auto canvasPos = clickPos - wsPos;

                // 不销毁 overlay！切换文案引导用户点击菜单中的 Tamagotchi
                tutorialOverlay->showStep1MenuOpened();
                tutorialStep = TutorialStep::step1_menuOpened;

                // 弹出受限菜单：仅 Tamagotchi 可选，关闭时恢复 STEP1 文案
                workspace->showAddMenu (screenPos, canvasPos,
                    { ModuleType::tamagotchi },
                    [this]()
                    {
                        // 用户关闭了菜单但没有选择 → 恢复文案，回到 STEP1
                        if (tutorialOverlay != nullptr && tutorialStep == TutorialStep::step1_menuOpened)
                        {
                            tutorialOverlay->showStep1 (
                                tutorialOverlay->getHighlightArea());
                            tutorialStep = TutorialStep::step1_rightClick;
                        }
                    });
            }
        };

        // 订阅 "点击气泡 × 按钮 → 确认跳过" → 跳过新手引导
        tutorialOverlay->onSkipRequested = [this]()
        {
            skipTutorial();
        };
    }

    // 获取 canvas 区域（相对 Editor 坐标 = workspace 坐标 + workspace 的 Y 偏移）
    const auto canvas = workspace->getCanvasArea();
    const auto wsPos  = workspace->getPosition();
    const auto canvasInEditor = canvas.translated (wsPos.x, wsPos.y);

    tutorialStep = TutorialStep::step1_rightClick;
    tutorialWasSkipped = false;

    tutorialOverlay->setBounds (getLocalBounds());
    tutorialOverlay->showStep1 (canvasInEditor);
    tutorialOverlay->toFront (false);
}

void Y2KmeterAudioProcessorEditor::advanceTutorialStep2()
{
    if (tutorialStep != TutorialStep::step1_rightClick
        && tutorialStep != TutorialStep::step1_menuOpened) return;

    // overlay 一直存活（STEP1 右键后未销毁），直接切换到 STEP2
    jassert (tutorialOverlay != nullptr);

    // 查找刚添加的 Tamagotchi 模块所在位置
    juce::Rectangle<int> petAreaInEditor;
    if (workspace != nullptr)
    {
        const auto wsPos = workspace->getPosition();
        for (int i = 0; i < workspace->getNumModules(); ++i)
        {
            if (auto* m = workspace->getModule (i))
            {
                if (m->getModuleType() == ModuleType::tamagotchi)
                {
                    const auto modBounds = m->getBounds();
                    petAreaInEditor = modBounds.translated (wsPos.x, wsPos.y);
                    break;
                }
            }
        }
    }

    tutorialStep = TutorialStep::step2_playAudio;

    tutorialOverlay->setBounds (getLocalBounds());
    tutorialOverlay->showStep2 (petAreaInEditor);
    tutorialOverlay->toFront (false);
}

void Y2KmeterAudioProcessorEditor::completeTutorial()
{
    tutorialStep = TutorialStep::hidden;
    tutorialWasSkipped = false;
    dismissTutorialOverlay();

    // 持久化完成状态
    processor.setTutorialCompleted (true);

    // 清理 Tamagotchi 信号保留（由正常的 tick 逻辑接管）
}

void Y2KmeterAudioProcessorEditor::skipTutorial()
{
    tutorialStep = TutorialStep::hidden;
    tutorialWasSkipped = true;
    dismissTutorialOverlay();

    // 持久化跳过状态：用户主动跳过引导后，下次启动不再触发。
    // （v2.1.5 之前不标记 tutorialCompleted，导致每次重启都重新出现）
    processor.setTutorialCompleted (true);
}

void Y2KmeterAudioProcessorEditor::dismissTutorialOverlay()
{
    if (tutorialOverlay != nullptr)
    {
        // 先隐藏（内部清理状态），再移出 child list，最后销毁
        tutorialOverlay->hide();
        removeChildComponent (tutorialOverlay.get());
        tutorialOverlay.reset();
    }
}

void Y2KmeterAudioProcessorEditor::checkTutorialStep2Condition()
{
    if (workspace == nullptr || isPluginHost) return;

    // ------ STEP 1: 等待用户从右键菜单中选中 Tamagotchi -------
    if (tutorialStep == TutorialStep::step1_menuOpened)
    {
        for (int i = 0; i < workspace->getNumModules(); ++i)
        {
            if (auto* m = workspace->getModule (i))
            {
                if (m->getModuleType() == ModuleType::tamagotchi)
                {
                    // 用户已添加 Tamagotchi → 推进到 STEP 2
                    advanceTutorialStep2();
                    return;
                }
            }
        }
        return;
    }

    // ------ STEP 2: 等待蛋孵化 -------
    if (tutorialStep != TutorialStep::step2_playAudio) return;

    // 遍历寻找 Tamagotchi 模块：检查是否已从蛋阶段孵化
    for (int i = 0; i < workspace->getNumModules(); ++i)
    {
        if (auto* m = workspace->getModule (i))
        {
            if (m->getModuleType() == ModuleType::tamagotchi)
            {
                if (auto* tamagotchi = dynamic_cast<TamagotchiModule*> (m))
                {
                    // 宠物已不再处于蛋/孵化阶段 → 孵化完成
                    if (! tamagotchi->isInEggPhase())
                    {
                        completeTutorial();
                        return;
                    }
                }
            }
        }
    }

    // 如果没有找到 Tamagotchi（被用户删除了），则重试 STEP 1
    bool hasTamagotchi = false;
    for (int i = 0; i < workspace->getNumModules(); ++i)
    {
        if (auto* m = workspace->getModule (i))
        {
            if (m->getModuleType() == ModuleType::tamagotchi)
            {
                hasTamagotchi = true;
                break;
            }
        }
    }

    if (! hasTamagotchi)
    {
        // 用户删除了 Tamagotchi，重新回到 STEP 1
        dismissTutorialOverlay();
        startTutorial();
    }
}

// ----------------------------------------------------------
// 按默认层叠瀑布布局加载七个默认模块（eq / loudness / oscilloscope /
// spectrum / phase / dynamics / waveform）。
//   · 调用方需先清空 workspace（首次启动由构造器保证）
//   · 每个模块相对前一个偏移 (stepX, stepY)，形成老式"弹出大量窗口"的层叠感
// ----------------------------------------------------------
void Y2KmeterAudioProcessorEditor::seedDefaultModules()
{
    static const ModuleType defaultOrder[] = {
        ModuleType::eq,
        ModuleType::loudness,
        ModuleType::vuMeter,
        ModuleType::oscilloscope,
        ModuleType::spectrum,
        ModuleType::phase,
        ModuleType::dynamics,
        ModuleType::waveform,
        ModuleType::spectrogram,
        ModuleType::spectrogram3d
    };

    // workspace 的 canvas 原点（此时 setSize 已触发 resized，canvas 有效）
    const auto canvas = workspace->getLocalBounds();

    // 瀑布布局的行列偏移 / 起点（与模块默认大小无关，保持固定）
    constexpr int stepX          = 28;
    constexpr int stepY          = 28;
    constexpr int startX         = 16;
    constexpr int startY         = 16;

    // 当瀑布到达右/下边缘时，换一列重新从顶部开始（像 XP 那样）
    int x = startX;
    int y = startY;
    int column = 0;

    for (auto type : defaultOrder)
    {
        auto panel = createModule(type);
        if (panel == nullptr) continue;

        // 尺寸：优先使用每个模块自己声明的"默认大小"（setDefaultSize），
        //   再用 minW/minH 做下限保护；这样 EQ / Loudness / Spectrum 等
        //   各自拿到 384×256 / 320×256 / 384×256 …而不是一刀切 340×240。
        const int w = juce::jmax(panel->getMinWidth(),  panel->getDefaultWidth());
        const int h = juce::jmax(panel->getMinHeight(), panel->getDefaultHeight());

        // 如果越界，换到下一列的顶部
        if (x + w > canvas.getRight() - 8 || y + h > canvas.getBottom() - 8)
        {
            ++column;
            x = startX + column * (stepX * 4);
            y = startY;
            // 还是越界就截取到边缘以内
            if (x + w > canvas.getRight() - 8)
                x = juce::jmax(0, canvas.getRight()  - w - 8);
            if (y + h > canvas.getBottom() - 8)
                y = juce::jmax(0, canvas.getBottom() - h - 8);
        }

        panel->setBounds(x, y, w, h);

        // autoPosition=false 让 workspace 保留我们设定的 bounds
        workspace->addModule(std::move(panel), /*autoPosition*/ false);

        x += stepX;
        y += stepY;
    }
}

// ----------------------------------------------------------
// 应用布局预设（由 ModuleWorkspace 的布局下拉框触发）
//   · presetId = 1 (defaultGrid)    → 恢复默认布局 + 默认窗口大小 960×640
//   · presetId = 2 (horizontalFull) → 拉伸顶层窗口到当前屏幕宽，默认模块横向
//                                      等分 canvas，高度撑满 canvas
// ----------------------------------------------------------
void Y2KmeterAudioProcessorEditor::applyLayoutPreset (int presetId)
{
    if (workspace == nullptr) return;

    // 保存所有 Tamagotchi 模块的状态（切换预设时保留，不被清除）
    struct TamagotchiState { juce::String roleName; float hunger; float health; };
    juce::Array<TamagotchiState> tamagotchiStates;
    for (int i = 0; i < workspace->getNumModules(); ++i)
    {
        auto* m = workspace->getModule (i);
        if (m->getModuleType() == ModuleType::tamagotchi)
            if (auto* t = dynamic_cast<TamagotchiModule*> (m))
                tamagotchiStates.add ({ t->getRoleName(), t->getHunger(), t->getHealth() });
    }

    // 先清空现有模块 / 拼豆贴画（clearAllModules 不触发 onLayoutChanged）
    workspace->clearAllModules();

    if (presetId == 1)
    {
        // Preset 1: 默认布局 + 默认窗口 960×640
        //   · 通过 setSize 触发 resized，workspace 会拿到对应的 canvas 尺寸后
        //     seedDefaultModules 才能正确计算默认瀑布位置。
        //   · 如果当前已经是 960×640（用户刚刚打开），setSize 不会有副作用。
        setSize (960, 640);
        seedDefaultModules();
    }
    else if (presetId == 2 || presetId == 3)
    {
        // Preset 2 / 3: 横向铺满屏幕宽度（高 250px）
        //   · Preset 2 = 贴屏幕顶部 (Y = userArea.getY())
        //   · Preset 3 = 贴屏幕底部 (Y = userArea.getBottom() - targetH)
        //
        //   其余逻辑（宽度取屏幕 userArea 宽、固定高 250、横向等分 7 个默认模块、
        //   放开 resizeLimits）完全相同。之前 Preset 2 累积 *2/3 导致高度不断缩水
        //   的问题已在上一轮改成常量修复。
        //
        //   Y 必须基于 userArea 而不是 top->getY()：Editor 被用户拖到副屏后，
        //   top->getY() 是跨屏绝对坐标（可能是 1440 或负数），而要的是"当前屏的
        //   顶/底边在屏幕坐标中的 Y"——这正是 userArea.getY() / getBottom()。
        const bool bottomAligned = (presetId == 3);

        auto* top = getTopLevelComponent();
        if (top == nullptr) top = this;

        const auto display = juce::Desktop::getInstance()
                                 .getDisplays()
                                 .getDisplayForRect (top->getScreenBounds());
        const auto userArea = (display != nullptr) ? display->userArea
                                                   : juce::Rectangle<int> (1280, 720);
        const int screenW = userArea.getWidth();

        // ------------------------------------------------------------
        // 目标高度自动对齐到 8px 网格整数倍 canvas：
        //   期望高度 kHorizontalStripHeight = 250，但 Editor 内 = Y2K 标题栏(26) +
        //   workspace；workspace 内 = canvas + 底部 toolbar(36)。因此：
        //       canvas.height = targetH - 26 - 36 = targetH - 62
        //   250 - 62 = 188，188 % 8 = 4 → canvas 不是整格，7 个等分模块
        //   floor 到 8 倍数后底部会留 4px 空白。
        //
        //   这里不想硬编码"26+36"这类依赖 ModuleWorkspace 内部常量的数值，
        //   改为"先试探一次布局，读取实际 canvas 高度，反推 overheadH"——
        //   这样无论未来 chrome/toolbar 高度怎么变都能自洽。
        //     1) 先按期望高度 setSize，触发 resized() → workspace 拿到 canvas
        //     2) overheadH = targetH - canvas.getHeight()（Editor 里非 canvas 的部分）
        //     3) 调整 targetH 使 (targetH - overheadH) 是 8 的倍数，并尽量靠近 250
        // ------------------------------------------------------------
        constexpr int kHorizontalStripHeight = 250;
        int targetH = kHorizontalStripHeight;

        // 关键前置：先把 resizeLimits 放到一个足够宽松的区间，确保后面的
        //   试探 setSize(screenW, 250) 不会被夹回（Editor 默认
        //   setResizeLimits(640, 420, 1600, 1100)：高度下限 420、宽度上限 1600）。
        //   如果试探被夹，probeCanvasH 会变成夹后 Editor 高度对应的 canvas，
        //   反推出来的 overheadH 是错的 → 最终 canvas 高度不是 8 的倍数 →
        //   模块 floor 后底部留 4~6px 空白。
        //   · 下限给一个极小值（例如 kHorizontalStripHeight 本身 250），
        //     上限至少覆盖 screenW × 1200（单屏宽 × 超出极限的高度）。
        if (auto* cbc0 = getConstrainer())
        {
            setResizeLimits (juce::jmin (cbc0->getMinimumWidth(),  screenW),
                             juce::jmin (cbc0->getMinimumHeight(), kHorizontalStripHeight),
                             juce::jmax (cbc0->getMaximumWidth(),  screenW),
                             juce::jmax (cbc0->getMaximumHeight(), 1200));
        }

        // 第 1 步：试探布局，让 workspace 计算出当前 chrome 状态下的 canvas 高。
        //   · Standalone 模式（top != this）：直接对顶层窗口 setBounds，
        //     这样 probe 反推的 overhead 天然包含"ResizableBorder 4px 上下边框"
        //     （setResizable(true, false) 下 DocumentWindow 会给 content 挖上下各 4px，
        //     setBoundsInset 压缩 Editor 高度 8px）。若 probe 只对 Editor setSize，
        //     反推的 overheadH 少算这 8px，最终 top->setBounds 走的才是"扣边框"路径，
        //     真实 canvas 比预期少 8px → 模块底部出现 8px 空白。
        //   · 插件模式（top == this）：无法搬动顶层窗口，直接 setSize。
        if (top == this)
        {
            setSize (screenW, targetH);
        }
        else
        {
            top->setBounds (userArea.getX(), userArea.getY(), screenW, targetH);
        }

        // 第 2 步：根据试探结果反推"顶层窗口高度里不属于 canvas 的那部分"。
        //   probeContainerH 是 probe 时真正被我们"占用"的高度：
        //     · standalone：top 的 height（=250，不被 border 吃掉，因为 border 从
        //       top 高度里内扣分给 Editor）
        //     · 插件：Editor 自身 height（=250）
        const int probeContainerH = (top == this) ? getHeight() : top->getHeight();
        const int probeCanvasH    = workspace->getCanvasArea().getHeight();
        const int overheadH       = probeContainerH - probeCanvasH;

        // 第 3 步：取最接近 250-overheadH 的"8 的倍数"作为期望 canvas 高，
        //          再加回 overheadH 得到对齐后的 targetH
        constexpr int kGridForH = 8;
        const int desiredCanvasRaw = kHorizontalStripHeight - overheadH;
        const int desiredCanvasH   = juce::jmax (kGridForH,
                                                 ((desiredCanvasRaw + kGridForH / 2) / kGridForH) * kGridForH);
        targetH = desiredCanvasH + overheadH;

        // 第 4 步：按最终 targetH 锁定 resizeLimits（上下限收紧到 screenW × targetH）
        if (auto* cbc = getConstrainer())
        {
            setResizeLimits (juce::jmin (cbc->getMinimumWidth(),  screenW),
                             juce::jmin (cbc->getMinimumHeight(), targetH),
                             juce::jmax (cbc->getMaximumWidth(),  screenW),
                             juce::jmax (cbc->getMaximumHeight(), targetH));
        }

        // 目标 Y：按贴顶/贴底切换
        const int targetY = bottomAligned ? (userArea.getBottom() - targetH)
                                          :  userArea.getY();

        if (top == this)
        {
            // 插件模式（嵌在宿主里）：无法移动顶层窗口，只调整尺寸
            setSize (screenW, targetH);
        }
        else
        {
            // Standalone 模式：连同外层窗口一起搬到目标位置
            top->setBounds (userArea.getX(), targetY, screenW, targetH);
        }

        // setSize / setBounds 会触发 resized()，workspace 随之拿到新 canvas。
        //   此时按横向等分的方式铺默认 7 个模块。
        static const ModuleType horizOrder[] = {
            ModuleType::spectrogram3d,
            ModuleType::dynamics,
            ModuleType::vuMeter,
            ModuleType::oscilloscope,       // 下面循环中设为 Liss 模式
            ModuleType::spectrum,
            ModuleType::oscilloscopeWave,
            ModuleType::waveform
        };

        // 使用 workspace->getCanvasArea() 而不是 getLocalBounds()，
        //   后者包含底部 toolbarHeight（36px）与 chrome 控件区。
        const auto canvas = workspace->getCanvasArea();
        const int count    = (int) (sizeof (horizOrder) / sizeof (horizOrder[0]));

        // ============================================================
        // 网格对齐：必须与 ModuleWorkspace::gridSize 保持一致（8 像素）。
        //   原实现直接用 canvas.getWidth()/count 作为 slotW，不是 8 的倍数，
        //   canvas 原点也未必对齐 8；模块落点和大小都偏离网格。
        //   → 一旦用户之后拖动/缩放任意模块，snapToGrid 会把它吸附到最近
        //     的 8 像素位，立刻与相邻模块拉开空隙或错位，"密排列"被破坏。
        //
        //   修复思路：
        //     1) 起点 x0/y0 按 gridSize 向上取整，保证第一个模块左上角在网格上。
        //     2) 可用宽度 usableW 从 canvas.getRight() 向下取整到 gridSize
        //        的倍数后，减去 x0；高度 slotH 同理。
        //     3) 把 usableW 切成 count 份，每份都是 gridSize 的整倍数：
        //        · baseCells = usableW / gridSize / count 个小格
        //        · 剩余 leftoverCells 分摊到前若干个模块各 +1 小格。
        //        这样 7 个模块宽度之和 == usableW，全部在网格上且密排无缝。
        // ============================================================
        constexpr int kGrid = 8;
        auto ceilToGrid  = [kGrid] (int v) { return ((v + kGrid - 1) / kGrid) * kGrid; };
        auto floorToGrid = [kGrid] (int v) { return (v / kGrid) * kGrid; };

        const int x0 = ceilToGrid  (canvas.getX());
        const int y0 = ceilToGrid  (canvas.getY());
        const int xR = floorToGrid (canvas.getRight());
        const int yB = floorToGrid (canvas.getBottom());

        const int usableW = juce::jmax (kGrid * count, xR - x0);
        const int usableH = juce::jmax (kGrid,         yB - y0);

        const int totalCells    = usableW / kGrid;              // 小格数量（8px/格）

        // v1.8.6：加权宽度分配（非均分），按以下比例从左到右：
        //   SPECTROGRAM3D:1.0 | DYNAMICS:1.0 | VU:0.7 | OSC(Liss):0.7
        //   SPECTRUM:1.5 | OSC WAVE:1.0 | WAVEFORM:1.5
        static const float kWidthRatios[] = {
            1.0f, 1.0f, 0.7f, 0.7f, 1.5f, 1.0f, 1.5f
        };
        static constexpr float kTotalRatio = 7.4f;

        int cellsForModule[7] = {};
        int cellsAllocated = 0;
        for (int i = 0; i < count; ++i)
        {
            int cells = (int) std::round ((float) totalCells * kWidthRatios[i] / kTotalRatio);
            cells = juce::jmax (1, cells);
            cellsForModule[i] = cells;
            cellsAllocated += cells;
        }
        // 修正最后一个模块（WAVEFORM）消纳舍入误差
        cellsForModule[count - 1] += totalCells - cellsAllocated;

        const int slotH = juce::jmax (80, usableH);             // 高度占满 canvas 的整网格

        int curX = x0;
        for (int i = 0; i < count; ++i)
        {
            auto panel = createModule (horizOrder[i]);
            if (panel == nullptr) continue;

            // Horizontal Bar 预设中 Oscilloscope 默认使用 Lissajous 模式
            if (horizOrder[i] == ModuleType::oscilloscope)
                if (auto* osc = dynamic_cast<OscilloscopeModule*>(panel.get()))
                    osc->setDisplayMode(OscilloscopeModule::DisplayMode::lissajous);

            const int cellsForThis = cellsForModule[i];
            const int slotW        = cellsForThis * kGrid;

            const int w = juce::jmax (panel->getMinWidth(),  slotW);
            const int h = juce::jmax (panel->getMinHeight(), slotH);
            panel->setBounds (curX, y0, w, h);
            workspace->addModule (std::move (panel), /*autoPosition*/ false);

            curX += slotW;
        }
    }

    // 重新添加 Tamagotchi 模块到 canvas 右下角（切换预设时保留宠物不被清除）
    for (const auto& state : tamagotchiStates)
    {
        auto tamagotchi = std::make_unique<TamagotchiModule>();
        if (state.roleName.isNotEmpty())
            tamagotchi->restorePersistentState (state.roleName, state.hunger, state.health);

        const auto canvas = workspace->getCanvasArea();
        constexpr int padding = 8;
        const int petW = tamagotchi->getDefaultWidth();
        const int petH = tamagotchi->getDefaultHeight();
        tamagotchi->setBounds (canvas.getRight()  - petW - padding,
                               canvas.getBottom() - petH - padding,
                               petW, petH);
        workspace->addModule (std::move (tamagotchi), false);
    }

    // 手动回写布局到 Processor（clearAllModules + 批量 addModule 会触发多次
    // onLayoutChanged，我们在这里统一再写一次确保最终态被持久化）
    processor.setSavedLayoutXml (workspace->saveLayoutAsXml());

    // ------------------------------------------------------------------
    // 修复：切换预设（特别是预设 2）后顶层窗口的 alwaysOnTop 会"显示按下但
    // 实际不置顶"的 bug。根因：
    //   · 预设 2 会通过 top->setBounds(...) 大幅改变顶层窗口，Windows 在某些
    //     情况下会把 HWND_TOPMOST 丢掉；
    //   · 但 JUCE Component::setAlwaysOnTop 有早退优化：
    //       if (shouldStayOnTop != flags.alwaysOnTopFlag) { ... }
    //     当我们再次调用 setAlwaysOnTop(true) 时，内部 flag 仍是 true，
    //     方法会直接 return，OS 层的 TopMost 位就不会被重新打上。
    //   · 用户"按两下 pin 按钮"能恢复，就是先 false 再 true 绕开了早退。
    // 解决：这里在布局变更后显式做一次 false→true（仅在当前是置顶态时），
    //      强制让 peer 重新应用 HWND_TOPMOST。
    // ------------------------------------------------------------------
    if (alwaysOnTopActive)
        setAlwaysOnTopActive (true);
}

// ----------------------------------------------------------
// 工厂方法
// ----------------------------------------------------------
std::unique_ptr<ModulePanel> Y2KmeterAudioProcessorEditor::createModule(ModuleType type)
{
    switch (type)
    {
        case ModuleType::eq:
            return std::make_unique<EqModule>(processor);
        case ModuleType::loudness:
            return std::make_unique<LoudnessModule>(processor.getAnalyserHub());
        case ModuleType::oscilloscope:
            return std::make_unique<OscilloscopeModule>(processor.getAnalyserHub());
        case ModuleType::spectrum:
            return std::make_unique<SpectrumModule>(processor.getAnalyserHub());
        case ModuleType::phase:
            return std::make_unique<PhaseModule>(processor.getAnalyserHub());
        case ModuleType::dynamics:
            return std::make_unique<DynamicsModule>(processor.getAnalyserHub());

        case ModuleType::lufsRealtime:
            return std::make_unique<LufsRealtimeModule>(processor.getAnalyserHub());
        case ModuleType::truePeak:
            return std::make_unique<TruePeakModule>(processor.getAnalyserHub());
        case ModuleType::oscilloscopeWave:
            return std::make_unique<OscilloscopeWaveModule>(processor.getAnalyserHub());
        case ModuleType::phaseCorrelation:
            return std::make_unique<PhaseCorrelationModule>(processor.getAnalyserHub());
        case ModuleType::phaseBalance:
            return std::make_unique<PhaseBalanceModule>(processor.getAnalyserHub());
        case ModuleType::dynamicsMeters:
            return std::make_unique<DynamicsMetersModule>(processor.getAnalyserHub());
        case ModuleType::dynamicsDr:
            return std::make_unique<DynamicsDrModule>(processor.getAnalyserHub());
        case ModuleType::dynamicsCrest:
            return std::make_unique<DynamicsCrestModule>(processor.getAnalyserHub());

        case ModuleType::waveform:
            return std::make_unique<WaveformModule>(processor.getAnalyserHub());

        case ModuleType::vuMeter:
            return std::make_unique<VuMeterModule>(processor.getAnalyserHub());

        case ModuleType::spectrogram:
            return std::make_unique<SpectrogramModule>(processor.getAnalyserHub());

        case ModuleType::spectrogram3d:
            return std::make_unique<Spectrogram3DModule>(processor.getAnalyserHub());

        case ModuleType::tamagotchi:
            return std::make_unique<TamagotchiModule>();

        case ModuleType::milkdrop:
            return std::make_unique<MilkdropModule>(&processor.getAnalyserHub());

        default:
            jassertfalse; // 暂未实现
            return nullptr;

    }
}

// ============================================================
// 插件模式（VST3 / AU / AAX）下的预设 Save —— 把当前 Processor state
// 打包成一个与 Standalone 完全兼容的 <PROPERTIES> XML 文件
//
// 文件结构（与 JUCE PropertiesFile 在磁盘上的 XML 格式对齐）：
//   <?xml version="1.0" ...?>
//   <PROPERTIES>
//     <VALUE name="filterState" val="BASE64(processor state binary)"/>
//   </PROPERTIES>
//
// 这样：
//   · 用户在 VST3 里 Save 出来的 .settings，Standalone 下次启动时 Load
//     能原样恢复（Standalone 的 reloadPluginState 会读 filterState）；
//   · Standalone 里 Save 出来的 .settings，VST3 这边 Load 也能正确解析
//     （loadStateFromSettingsFile 会识别同样的 <PROPERTIES>/filterState 结构）。
//
// 注：插件模式下我们**有意不**写入主题 / 窗口 bounds / 音频源 等 Standalone
// 专属字段 —— 这些在 VST3 场景下的语义与 Standalone 完全不同（窗口由宿主管、
// 主题由插件内独立持久化）。如果用户用 VST3 存的文件里缺这些键，Standalone
// 首次 Load 时会按其默认值兜底，不会报错。
// ============================================================
void Y2KmeterAudioProcessorEditor::saveStateAsSettingsFile (const juce::File& dest)
{
    // 1) 先把"当前 workspace 的实时布局"回写到 Processor，保证 getStateInformation
    //    拿到的是用户眼睛看到的最新状态（onLayoutChanged 是异步节流的，不能指望
    //    点击 Save 的那一瞬间刚好已经被刷到 Processor）
    if (workspace != nullptr)
        processor.setSavedLayoutXml (workspace->saveLayoutAsXml());

    // 2) 导出 Processor state（内部是 XML 的二进制化形式）
    juce::MemoryBlock stateBlock;
    processor.getStateInformation (stateBlock);

    // 3) 构造 <PROPERTIES> 根节点 + 一个 filterState VALUE 子节点（base64）
    juce::XmlElement props ("PROPERTIES");
    auto* v = props.createNewChildElement ("VALUE");
    v->setAttribute ("name", "filterState");
    v->setAttribute ("val",  stateBlock.toBase64Encoding());

    // 4) 原子化写入目标文件（先写 TempFile，成功后 rename；避免半截文件）
    dest.getParentDirectory().createDirectory();
    juce::TemporaryFile tmp (dest);
    if (! props.writeTo (tmp.getFile()))
    {
        DBG ("saveStateAsSettingsFile: write temp failed");
        return;
    }
    if (! tmp.overwriteTargetFileWithTemporary())
    {
        DBG ("saveStateAsSettingsFile: replace failed → " + dest.getFullPathName());
    }
}

// ============================================================
// 插件模式下的预设 Load —— 把用户选择的 .settings 文件解析出 Processor state
// 并就地热重载（不重启宿主）。容错三种来源：
//   a) Standalone / 本 Save 写出的 <PROPERTIES> + filterState (base64) 结构；
//   b) 裸 <PBEQ_State ...> XML（即 Processor::getStateInformation 的原生 XML 形态）；
//   c) 裸 <PBEQ_Layout ...> XML（仅包含布局，无其他元信息）。
// 优先级 a > b > c。任一成功即可。
// ============================================================
void Y2KmeterAudioProcessorEditor::loadStateFromSettingsFile (const juce::File& src)
{
    if (! src.existsAsFile()) return;

    auto xml = juce::parseXML (src);
    if (xml == nullptr)
    {
        DBG ("loadStateFromSettingsFile: not a valid XML → " + src.getFullPathName());
        return;
    }

    // ---- 分支 a：<PROPERTIES> 外壳，含 filterState base64 -------
    if (xml->hasTagName ("PROPERTIES"))
    {
        // 在 VALUE 子节点里找 name="filterState" 的那一个
        const juce::XmlElement* filterVal = nullptr;
        for (auto* child : xml->getChildWithTagNameIterator ("VALUE"))
        {
            if (child != nullptr && child->getStringAttribute ("name") == "filterState")
            {
                filterVal = child;
                break;
            }
        }
        if (filterVal != nullptr)
        {
            const auto b64 = filterVal->getStringAttribute ("val");
            juce::MemoryBlock block;
            if (block.fromBase64Encoding (b64) && block.getSize() > 0)
            {
                // 反序列化回 Processor —— 内部会更新 savedLayoutXml
                processor.setStateInformation (block.getData(), (int) block.getSize());
                // 立刻把新布局灌到 workspace（不用等宿主再开关一次 Editor）
                if (workspace != nullptr)
                {
                    const auto layoutXml = processor.getSavedLayoutXml();
                    if (layoutXml.isNotEmpty())
                        workspace->loadLayoutFromXml (layoutXml);
                }
                return;
            }
        }
        DBG ("loadStateFromSettingsFile: <PROPERTIES> without valid filterState");
        // 不 return，允许继续尝试把整文件当作其它格式（极少见）
    }

    // ---- 分支 b：裸 PBEQ_State XML -------
    if (xml->hasTagName ("PBEQ_State"))
    {
        juce::MemoryBlock block;
        juce::AudioProcessor::copyXmlToBinary (*xml, block);
        if (block.getSize() > 0)
        {
            processor.setStateInformation (block.getData(), (int) block.getSize());
            if (workspace != nullptr)
            {
                const auto layoutXml = processor.getSavedLayoutXml();
                if (layoutXml.isNotEmpty())
                    workspace->loadLayoutFromXml (layoutXml);
            }
            return;
        }
    }

    // ---- 分支 c：裸 PBEQ_Layout XML -------
    if (xml->hasTagName ("PBEQ_Layout"))
    {
        const auto layoutXml = xml->toString (juce::XmlElement::TextFormat{}.singleLine());
        // 同步写回 Processor 以便下次 getStateInformation 能拿到
        processor.setSavedLayoutXml (layoutXml);
        if (workspace != nullptr)
            workspace->loadLayoutFromXml (layoutXml);
        return;
    }

    DBG ("loadStateFromSettingsFile: unknown root tag = " + xml->getTagName());
}

void Y2KmeterAudioProcessorEditor::invalidateDesktopCache() noexcept
{
    desktopCacheDirty = true;
}

void Y2KmeterAudioProcessorEditor::rebuildDesktopCacheIfNeeded()
{
    // P6 · 跨实例共享桌面纹理：
    //   之前按"每个 Editor 实例"各自维护一张 desktopCacheImage，
    //   并且 macOS 分支因顾虑 legacy NSOpenGLContext 的纹理 upload 开销
    //   选择**每帧重绘** drawDesktop / drawLogo 的循环图样——多实例场景
    //   （DAW 同时加载 N 份）下这段循环被放大 N 倍，是 UI 线程热点。
    //
    //   P0 已不再 attach OpenGLContext 到 mac，软光栅 drawImageAt 走的是
    //   CoreGraphics blit，比每帧重新画循环图样便宜一个数量级；因此 mac
    //   分支也启用共享 cache，并把底图 Image 抽到 PinkXP 进程级 weak-ref
    //   共享池（getSharedDesktopTexture），多实例 + 同尺寸时零重复绘制。
    //   Logo 仍由各实例按自己的 logoImage 独立叠加（中心区单次 drawImage，
    //   成本可忽略）。
    const auto bounds = workspace != nullptr ? workspace->getBounds() : getLocalBounds();
    if (bounds.isEmpty())
    {
        desktopCacheImage = {};
        desktopCacheBounds = {};
        desktopCacheDirty = false;
        return;
    }

    if (! desktopCacheDirty
        && desktopCacheImage.isValid()
        && desktopCacheBounds == bounds)
        return;

    // 1) 从进程级共享池取（或新建）底图 —— 多个 Editor 同尺寸时共用像素数据
    juce::Image sharedBase = PinkXP::getSharedDesktopTexture (bounds.getWidth(),
                                                              bounds.getHeight());

    if (! sharedBase.isValid())
    {
        // 兜底：共享池失败时退回原有路径
        desktopCacheImage = {};
        desktopCacheBounds = {};
        desktopCacheDirty = false;
        return;
    }

    // 2) 本实例的 cache Image = 共享底图 + 居中 logo 叠加
    //    由于 logo 位置/尺寸依赖 bounds，无法跨实例共用最终图；
    //    但共享部分已经覆盖了 drawDesktop 所有重复循环（最大头）。
    desktopCacheImage = juce::Image (juce::Image::RGB,
                                     bounds.getWidth(),
                                     bounds.getHeight(),
                                     false);
    desktopCacheBounds = bounds;
    desktopCacheDirty = false;

    juce::Graphics cacheGraphics (desktopCacheImage);
    cacheGraphics.drawImageAt (sharedBase, 0, 0);
    // drawLogo 期望 area 是"目标绘制矩形"，而 cache Image 以 (0,0) 起点，
    // 因此传递一个左上角为 (0,0) 的等尺寸矩形。
    PinkXP::drawLogo (cacheGraphics,
                      juce::Rectangle<int>(0, 0, bounds.getWidth(), bounds.getHeight()),
                      logoImage);
}

// ----------------------------------------------------------
// 绘制：方案乙（完全铺满）
//   · 顶部 TitleBar（26px，Pink XP 风格抬头；含 Logo + 标题文字 + 右侧 × 按钮）
//   · 中部 Workspace（半透明），下面铺桌面棋盘纹理 + 中央 logo，作为"透过来的纹理背景"
//   · 底部 Toolbar（由 ModuleWorkspace 自己画）
//   关闭按钮嵌入 TitleBar 最右侧；chrome 隐藏时 TitleBar + × 均按 chromeAlpha 半透明
// ----------------------------------------------------------
void Y2KmeterAudioProcessorEditor::paint(juce::Graphics& g)
{
    // 1) 桌面纹理底图：只在 workspace 矩形范围内绘制
    //    workspace 是半透明的，纹理会作为模块背后的视觉肌理透出来
    if (workspace != nullptr)
    {
        const auto wsBounds = workspace->getBounds();
        if (! wsBounds.isEmpty())
        {
            rebuildDesktopCacheIfNeeded();

            juce::Graphics::ScopedSaveState save (g);
            g.reduceClipRegion (wsBounds);

            if (desktopCacheImage.isValid() && desktopCacheBounds == wsBounds)
                g.drawImageAt (desktopCacheImage, wsBounds.getX(), wsBounds.getY());
            else
            {
                PinkXP::drawDesktop (g, getLocalBounds());
                PinkXP::drawLogo    (g, wsBounds, logoImage);
            }
        }
    }

    // 2) 顶部 TitleBar + 右上角按钮组
    //    · chrome 可见：正常绘制完整标题栏 + 三个按钮
    //    · chrome 隐藏：整个标题栏消失，只在右上角绘制一个半透明小关闭浮标
    //      （样式参照底部 Hide 按钮：未悬停时 15%、悬停时 100%）
    //    · 插件宿主模式（VST3 等）：也画"精简抬头"——只绘制背景 + Logo +
    //      "Y2Kmeter v1.1 iisaacbeats.cn"文字；不画右侧最小化/固定/关闭按钮
    //      （宿主窗口已经提供系统级边框/关闭按钮，伪按钮会产生两套 UI 打架）。
    if (! chromeDim)
    {
        auto tb = getTitleBarBounds();

        // 标题栏背景（Pink XP 风格 —— 粉色渐变 + 像素高光/阴影）
        //   文字留给我们自己画，这样"软件名 + 版本号 + 官网"可以拥有不同字号 / 下划线
        PinkXP::drawPinkTitleBar (g, tb, juce::String(), 12.0f);

        // 左侧 Logo 图标：drawPinkTitleBar 已保留最左侧 24px 给 icon 区域，
        //   我们继续在 icon 右侧顺延绘制标题文字，所以 textArea 的 x 从 28 开始。
        //   标题布局：[Y2Kmeter] [空格] [v1.1 小字] [空格-空格] [iisaacbeats.cn 小字]
        auto textArea = getTitleTextBounds();

        // 主标题 "Y2Kmeter"
        const juce::String nameText    = "Y2Kmeter";
const juce::String versionText = "v2.1.12";
        const juce::String urlText     = "iisaacbeats.cn";

        const juce::Font nameFont    = PinkXP::getFont (12.0f, juce::Font::bold);
        const juce::Font versionFont = PinkXP::getFont (10.0f, juce::Font::italic);
        const juce::Font urlFont     = PinkXP::getFont (10.0f, juce::Font::plain);

        const int nameW    = nameFont.getStringWidth (nameText);
const int versionW = versionFont.getStringWidth ("v2.1.12");
        const int urlW     = urlFont.getStringWidth (urlText);

        constexpr int gap1 = 6;   // name ↔ version 之间
        constexpr int gap2 = 10;  // version ↔ url 之间

        const int y  = textArea.getY();
        const int h  = textArea.getHeight();
        const int x0 = textArea.getX();

        // 文字阴影（1,1）
        g.setColour (PinkXP::sel.darker(0.55f));
        g.setFont (nameFont);
        g.drawText (nameText, x0 + 1, y + 1, nameW, h, juce::Justification::centredLeft, false);
        g.setFont (versionFont);
        g.drawText (versionText, x0 + nameW + gap1 + 1, y + 1, versionW, h, juce::Justification::centredLeft, false);
        g.setFont (urlFont);
        g.drawText (urlText, x0 + nameW + gap1 + versionW + gap2 + 1, y + 1, urlW, h, juce::Justification::centredLeft, false);

        // 主文字（白色）
        g.setColour (PinkXP::selInk);
        g.setFont (nameFont);
        g.drawText (nameText, x0, y, nameW, h, juce::Justification::centredLeft, false);
        g.setFont (versionFont);
        g.drawText (versionText, x0 + nameW + gap1, y, versionW, h, juce::Justification::centredLeft, false);
        g.setFont (urlFont);
        g.drawText (urlText, x0 + nameW + gap1 + versionW + gap2, y, urlW, h, juce::Justification::centredLeft, false);

        // hover 时在可点击区域（name + version + url 整段）画下划线
        if (titleTextHovered)
        {
            const int lineY = y + h - 3;
            const int totalW = nameW + gap1 + versionW + gap2 + urlW;
            g.setColour (PinkXP::selInk);
            g.fillRect (x0, lineY, totalW, 1);
        }

        // 缓存实际绘制宽度，供 mouseMove / mouseDown 精确命中（参考 titleTextHovered）
        cachedTitleTextW = nameW + gap1 + versionW + gap2 + urlW;

        // 标题栏下沿 1px 深色分割线（与 ModulePanel 视觉一致）
        g.setColour (PinkXP::dark);
        g.fillRect (tb.getX(), tb.getBottom(), tb.getWidth(), 1);

        // 3) 右上角三个按钮（从右到左：关闭 × / 固定 ★ / 最小化 _ ）
        //    · 插件宿主模式（VST3 等）：完全不画这三个按钮 —— 宿主自带系统级
        //      最小化/关闭按钮，我们只保留"软件名 + 版本号 + 官网"文字抬头。
        //    样式完全参考 XP：drawRaised + hover 粉色 + pressed 凹陷
        if (! isPluginHost)
        {
            auto drawTitleBtn = [&] (juce::Rectangle<int> rc,
                                     bool hovered, bool pressed, bool activeLatched,
                                     const juce::String& glyph,
                                     float fontHeight,
                                     int glyphDxWhenIdle,
                                     int glyphDyWhenIdle)
            {
                // Pin 按钮激活（alwaysOnTop=true）时呈现"凹陷锁定"视觉
                if (pressed || activeLatched)
                    PinkXP::drawPressed (g, rc, activeLatched ? PinkXP::pink300 : PinkXP::pink100);
                else
                    PinkXP::drawRaised  (g, rc, hovered ? PinkXP::pink200 : PinkXP::btnFace);

                g.setColour (PinkXP::ink);
                g.setFont   (PinkXP::getFont (fontHeight, juce::Font::bold));
                auto txt = rc;
                txt.translate (glyphDxWhenIdle, glyphDyWhenIdle);
                if (pressed || activeLatched) txt.translate (1, 1);
                g.drawText (glyph, txt, juce::Justification::centred, false);
            };

            // 3a-lock) 布局锁定按钮（v1.8.3 新增）："L" 作为 Lock 的助记符号
            //   · 之所以选 "L" 而非 Unicode 锁头符 🔒 是因为项目内自制像素字体 PinkXP
            //     未包含 Emoji 字形，直接绘会回退到系统字体，与周围三个按钮不归一。
            //   · pressed（点击瞬间凹陷）与 activeLatched=layoutLocked（长按锁定态凹陷）
            //     两条视觉路径复用同一 pressedLook，切换手感与 Pin 按钮一致。
            drawTitleBtn (getLockButtonBounds(),
                          lockButtonHovered, lockButtonPressed, layoutLocked,
                          "L", 12.0f, -1, -2);

            // 3a) 最小化按钮："_"（字体里下划线位置偏低，y=-3 让底横线略高一点更像传统 _ 位置）
            drawTitleBtn (getMinimiseButtonBounds(),
                          minButtonHovered, minButtonPressed, false,
                          "_", 12.0f, -1, -3);

            // 3b) 固定（置顶）按钮：未激活 "*"  / 激活 "*"(凹陷) —— 使用易识别的星号符号
            //     字体里 '*' 位置偏下，y=-2 做居中微调（相对最小化 y=-3 略低 1 像素更对眼）
            drawTitleBtn (getPinButtonBounds(),
                          pinButtonHovered, pinButtonPressed, alwaysOnTopActive,
                          "*", 14.0f, -1, -2);

            // 3c) 关闭按钮："x"（字体里 'x' 位置正常，仅 y=-1 做微调）
            drawTitleBtn (getCloseButtonBounds(),
                          closeButtonHovered, closeButtonPressed, false,
                          "x", 12.0f, -1, -1);
        }
    }
    // chrome 隐藏态的浮动关闭按钮 + 抬头文字已交由 ChromeHiddenOverlay 处理
    //   （作为独立 child 组件，z-order 高于 workspace，避免被覆盖导致无法交互）
}

// ----------------------------------------------------------
// 布局：方案乙（完全铺满）
//   titleBar (26px) | workspace（自带底部 toolbar 36px） —— 三段占满整个 Editor
//   workspace 内部的 toolbar 会自动占据最底部 36px（由 ModuleWorkspace 处理）
// ----------------------------------------------------------
void Y2KmeterAudioProcessorEditor::resized()
{
    // 注意：这里**不再**无条件 invalidateDesktopCache()。
    //   · rebuildDesktopCacheIfNeeded() 内部已经做了 bounds 对比，workspace 尺寸
    //     真正变化时会自己走重建分支；
    //   · 模块冒前 / 拖动 / hide-chrome 过渡等场景会触发 Editor::resized()，但
    //     workspace.getBounds() 通常不变，没必要把整张背景纹理扔掉重传 GPU。
    //   · 主题切换仍会在 themeSubscribe 回调里显式调用 invalidateDesktopCache()。
    //   · 这一改动对 Windows 路径无影响（行为等价），对 macOS 能避免每次 resize
    //     触发整张 workspace 尺寸 RGB Image 重建 + legacy NSOpenGLContext 的同步
    //     纹理 upload，是 macOS 多模块卡顿的关键修复之一。

    auto r = getLocalBounds();
    // chrome 可见时顶部让给 TitleBar；chrome 隐藏时让 workspace 占满整个窗口，
    //   浮层 overlay 作为"最底层 child"铺在顶部 titleBarHeight 区域，会被模块自然遮挡。
    // 插件宿主模式（VST3 等）：也为标题栏预留 titleBarHeight —— 我们会画一个
    //   "精简抬头"（只有软件名 + 版本号 + 官网文字，无右侧最小化/固定/关闭按钮），
    //   宿主窗口已提供自己的系统标题栏和边框，不会与此抬头冲突。
    if (! chromeDim)
        r.removeFromTop (titleBarHeight);
    workspace->setBounds (r);

    // 浮层固定在顶部，与 TitleBar 同尺寸（Editor 同宽 × titleBarHeight）。
    if (chromeHiddenOverlay != nullptr)
        chromeHiddenOverlay->setBounds (0, 0, getWidth(), titleBarHeight);

    // 教程覆盖层始终铺满整个 Editor
    if (tutorialOverlay != nullptr && tutorialOverlay->isVisible())
        tutorialOverlay->setBounds (getLocalBounds());

    // 同步 workspace 的 hit-test 挖洞（按钮位置依赖 getWidth()，resize 后必须重新计算）
    updateWorkspaceHitTestHoles();

    // 插件模式下实时把 Editor 尺寸写回 Processor：host 调用 getStateInformation
    //   时会把最新尺寸一起序列化，下一次重新打开编辑器窗口时即可恢复。
    //   · Standalone 不走此路径（PropertiesFile 体系自行处理位置+尺寸）；
    //   · 析构期间 (editorBeingDestructed) 不再写回：FL Studio 等宿主在关闭
    //     嵌入子窗口的瞬间会先把插件视图 resize 到 ~175×85 这种极小中间态
    //     （目的是让窗口先从屏幕上隐形），若此时回写到 savedEditorSize，下次
    //     重新打开插件窗口就会恢复成这个异常小尺寸；
    //   · 写入下限提到 400×300：该阈值大于任何正常可用的最小 Editor 尺寸
    //     （标题栏 26 + toolbar 36 + 至少一行模块 ≈ 需要 300+ 高度），但又
    //     远小于构造默认值 960×640，起到"防止宿主偷偷塞小尺寸"的护栏作用，
    //     同时不会阻碍用户主动把窗口拉到合理的较小尺寸。
    if (isPluginHost && ! editorBeingDestructed)
    {
        constexpr int kMinPersistW = 400;
        constexpr int kMinPersistH = 300;

        const int w = getWidth();
        const int h = getHeight();
        if (w >= kMinPersistW && h >= kMinPersistH)
            processor.setSavedEditorSize (w, h);
    }
}

void Y2KmeterAudioProcessorEditor::parentHierarchyChanged()
{
    // --------------------------------------------------------------------
    // Windows VST3 卡死修复 —— 强制使用软件渲染引擎
    //
    // 背景（基于 dump 栈实证）：
    //   JUCE 8 在 Windows 默认启用 Direct2D 渲染器；Direct2D 用
    //   SharedResourcePointer<DxgiAdapters>（DLL 级 static shared_ptr）缓存
    //   D3D11/DXGI 设备。当 FL Studio / 某些 DAW 刷新/删除 VST3 时会调用
    //   FreeLibrary，CRT 在 DLL_PROCESS_DETACH 路径（持有 loader lock 的上下文）
    //   里执行 atexit 析构链，触发 DxgiAdapters 析构 → ID3D11Device 释放 →
    //   AMD 驱动 atidxx64.dll::CDevice::DestroyDriverInstance 内部 SleepEx
    //   等待 GPU fence / worker thread，而 loader lock 又阻塞了那些线程的推进，
    //   主线程从此无限 Sleep（= 进程未响应 / 宿主卡死）。
    //
    //   解决方案：在 peer 刚拿到时把渲染引擎切到 Software（index 0），
    //   让 Direct2D 全局资源根本不被构造，DLL 卸载链路上就没有 AMD 驱动销毁
    //   环节，彻底避开 loader lock 死等。
    //
    // 限定范围：
    //   · 仅 Windows 生效（mac/Linux 没有该问题）。
    //   · peer 可用且有 Direct2D 可选项时才切换（少数旧系统仅 Software 一档，
    //     此时 setCurrentRenderingEngine 是 no-op）。
    //   · 只做一次：首次切换成功后 renderingEngineConfigured 置 true，后续
    //     parentHierarchyChanged 不再重复（避免用户手动切回 Direct2D 后被覆盖）。
    //
    // 代价：Windows 下 UI 由 GDI/软件光栅绘制，性能略降。对当前插件（UI 主要为
    //   低频仪表 + 频谱小图，且我们另外开启了 OpenGLContext 兜底）几乎无感知。
    // --------------------------------------------------------------------
   #if JUCE_WINDOWS
    if (! renderingEngineConfigured)
    {
        if (auto* peer = getPeer())
        {
            const auto engines = peer->getAvailableRenderingEngines();
            if (engines.size() > 1)
            {
                peer->setCurrentRenderingEngine (0); // 0 = Software Renderer
            }
            renderingEngineConfigured = true;
        }
    }
   #endif

    // v1.8.6：如果顶层窗口监听器已注册但 Editor 正在被从窗口树中移除，
    //   释放监听器（顶层窗口即将销毁，继续持有原始指针不安全）。
    if (topLevelExitWatcher != nullptr && getParentComponent() == nullptr)
        topLevelExitWatcher.reset();

    // v1.9.0：同理，Editor 从窗口树移除时释放 workspace 嵌套子组件监听器。
    if (autoHideChildWatcher != nullptr && getParentComponent() == nullptr)
    {
        if (workspace != nullptr)
            workspace->removeMouseListener (autoHideChildWatcher.get());
        autoHideChildWatcher.reset();
    }

    juce::AudioProcessorEditor::parentHierarchyChanged();
}

void Y2KmeterAudioProcessorEditor::visibilityChanged()
{
    processor.setAnalysisActive(true);

    // 默认启用"固定窗口置顶"（alwaysOnTopActive 初始 true）：
    //   · 仅在 Standalone 下应用（插件模式下顶层是宿主窗口，setAlwaysOnTop
    //     会影响宿主行为，且多数 DAW 会忽略该调用，但为避免副作用，限定
    //     Standalone 场景）。
    //   · visibilityChanged 是 getTopLevelComponent() 已指向实际顶层 Y2KMainWindow
    //     的最早时机；构造器里调 getTopLevelComponent() 可能还是 Editor 自己。
    //   · 只在首次 visibilityChanged 做一次，避免每次隐藏/显示时反复强推。
    //   · 通过 setAlwaysOnTopActive() 强推（内部做 false→true 绕开 JUCE 早退），
    //     防止"按钮显按下但实际未置顶"的 bug。
    if (! initialAlwaysOnTopApplied
        && juce::JUCEApplicationBase::isStandaloneApp())
    {
        if (auto* top = getTopLevelComponent())
        {
            if (top != this)
            {
                setAlwaysOnTopActive (alwaysOnTopActive);
            }
        }
    }

    // v1.8.3：布局锁定态延迟 apply。
    //   构造期 applyLayoutLocked(initial=true, locked=true) 因顶层未挂载 / 尺寸=0
    //   走了早退分支，仅把 layoutLocked=true 和 pendingLockApplyOnAttach=true 记下。
    //   visibilityChanged 是顶层可用且尺寸稳定的最早时机——若确实需要补做且当前
    //   仍是锁定态，则再走一次 initial=true（此时顶层已 ready 会真正跑 constrainer 分支，
    //   同时 initial=true 保证不回写 Processor 造成 dirty state）。
    //   触发前先清 pending 避免任何潜在重入。
    if (pendingLockApplyOnAttach && layoutLocked)
    {
        if (auto* top = getTopLevelComponent())
        {
            if (top != this && top->getWidth() > 0 && top->getHeight() > 0)
            {
                pendingLockApplyOnAttach = false;
                applyLayoutLocked (true, /*initial*/ true);
            }
        }
    }
    else if (pendingLockApplyOnAttach && ! layoutLocked)
    {
        // 未锁但仍残留 pending：清掉，防止后续每次 hide/show 都重复检查
        pendingLockApplyOnAttach = false;
    }

    // v1.8.6：注册顶层窗口 mouseExit 监听器（仅一次），用于处理 auto-hide 下
    //   鼠标穿过标题栏离开窗口的场景——此时 Editor::mouseExit 已提前触发且
    //   因鼠标在标题栏内判定"未离开顶层窗口"而不清零 autoHideNeedsExitFirst，
    //   需要监听顶层窗口自身的 mouseExit 来补清零。
    if (topLevelExitWatcher == nullptr)
    {
        auto* top = getTopLevelComponent();
        if (top != nullptr && top != this)
        {
            topLevelExitWatcher = std::make_unique<TopLevelExitWatcher>();
            topLevelExitWatcher->onExit = [this]()
            {
                // 无论鼠标是从 workspace 直接离开还是穿过标题栏离开，
                // 只要离开了整个顶层窗口（含标题栏），就必须：
                //   · 若当前 chrome 因 hover-show 可见 → 隐藏 chrome
                //   · 清零 autoHideNeedsExitFirst 守卫（否则后续 hover show 永远被拦截）
                if (! autoHideMode) return;

                if (workspace != nullptr && workspace->isChromeVisible())
                    workspace->setChromeVisible (false);

                autoHideNeedsExitFirst = false;
            };
            top->addMouseListener (topLevelExitWatcher.get(), false);
        }
    }

    // v1.9.0：注册 workspace 嵌套子组件鼠标监听器（修复 auto-hide 下模块上鼠标事件无法
    //   触发 auto-show/hide 的问题）。addMouseListener 第二个参数 true 表示接收 workspace
    //   所有嵌套子组件（ModulePanel/TamagotchiModule 等）的鼠标事件。
    if (autoHideChildWatcher == nullptr && workspace != nullptr)
    {
        autoHideChildWatcher = std::make_unique<AutoHideChildWatcher>();

        // mouseMove / mouseEnter：auto-hide 模式下鼠标在任何子组件上移动时触发 auto-show。
        //   复用 suppressAutoShowCounter 和 autoHideNeedsExitFirst 守卫。
        autoHideChildWatcher->onMouseActivity = [this](const juce::MouseEvent& e)
        {
            (void) e;
            if (suppressAutoShowCounter > 0) return;
            if (autoHideNeedsExitFirst) return;
            if (autoHideMode && workspace != nullptr && ! workspace->isChromeVisible())
            {
                temporaryChromeShow = true;
                workspace->setChromeVisible (true);
                temporaryChromeShow = false;
            }
        };

        // mouseExit：鼠标从任何子组件（含模块）离开 workspace 时，检查是否真正
        //   离开了顶层窗口。若是，则触发 auto-hide 并清零守卫。
        autoHideChildWatcher->onMouseLeave = [this](const juce::MouseEvent& e)
        {
            if (! autoHideMode || workspace == nullptr) return;
            auto* top = getTopLevelComponent();
            if (top == nullptr) return;
            const auto mouseScreen = e.getScreenPosition().toInt();
            const auto topScreenBounds = (top == this) ? getScreenBounds() : top->getScreenBounds();
            if (! topScreenBounds.contains (mouseScreen))
            {
                if (workspace->isChromeVisible())
                    workspace->setChromeVisible (false);
                autoHideNeedsExitFirst = false;
            }
        };

        workspace->addMouseListener (autoHideChildWatcher.get(), true);
    }
}

// ==========================================================
// 标题栏 / 关闭按钮几何
// ==========================================================
juce::Rectangle<int> Y2KmeterAudioProcessorEditor::getTitleBarBounds() const
{
    return { 0, 0, getWidth(), titleBarHeight };
}

juce::Rectangle<int> Y2KmeterAudioProcessorEditor::getTitleTextBounds() const
{
    // 标题栏左侧可点击文字区域：从 x=28 开始（drawPinkTitleBar 已为左侧 icon 保留 6..25 的 19px）
    //   · Standalone：右边界到最左按钮（lock）左侧留 8px 间隔，避免误点到按钮
    //   · 插件宿主模式：没有右侧按钮，右边界直接贴到 Editor 右缘，留 8px 边距
    auto tb = getTitleBarBounds();
    const int x = tb.getX() + 28;
    const int right = isPluginHost ? (tb.getRight() - 8)
                                    : (getLockButtonBounds().getX() - 8);
    return { x, tb.getY(), juce::jmax (0, right - x), tb.getHeight() };
}

juce::Rectangle<int> Y2KmeterAudioProcessorEditor::getCloseButtonBounds() const
{
    auto tb = getTitleBarBounds();
    const int y = tb.getY() + (tb.getHeight() - closeButtonSize) / 2;
    return { tb.getRight() - closeButtonMargin - closeButtonSize, y,
             closeButtonSize, closeButtonSize };
}

juce::Rectangle<int> Y2KmeterAudioProcessorEditor::getPinButtonBounds() const
{
    // 关闭按钮左侧（中间那一个）：固定（置顶）按钮
    auto cb = getCloseButtonBounds();
    return { cb.getX() - titleButtonGap - closeButtonSize, cb.getY(),
             closeButtonSize, closeButtonSize };
}

juce::Rectangle<int> Y2KmeterAudioProcessorEditor::getMinimiseButtonBounds() const
{
    // Pin 按钮左侧（中间那一个）：最小化按钮
    auto pb = getPinButtonBounds();
    return { pb.getX() - titleButtonGap - closeButtonSize, pb.getY(),
             closeButtonSize, closeButtonSize };
}

// 最小化按钮左侧（最左那一个）：布局锁定按钮（v1.8.3 新增）
juce::Rectangle<int> Y2KmeterAudioProcessorEditor::getLockButtonBounds() const
{
    auto mb = getMinimiseButtonBounds();
    return { mb.getX() - titleButtonGap - closeButtonSize, mb.getY(),
             closeButtonSize, closeButtonSize };
}

juce::Rectangle<int> Y2KmeterAudioProcessorEditor::getFloatingCloseButtonBounds() const
{
    // chrome 隐藏态下的悬浮关闭按钮：右上角，距边距 4px
    constexpr int margin = 4;
    return { getWidth()  - margin - closeButtonSize, margin,
             closeButtonSize, closeButtonSize };
}

// chrome 可见时恒为 1.0；chrome 隐藏时本函数已废弃（整个标题栏直接不绘制，
// 只留悬浮关闭按钮按自己的 hover 态切换透明度）。保留仅为兼容旧调用点。
float Y2KmeterAudioProcessorEditor::getChromeAlpha() const
{
    return 1.0f;
}

void Y2KmeterAudioProcessorEditor::handleCloseClicked()
{
    // Standalone 模式：请求应用退出（JUCE 会调用 StandaloneApp::systemRequestedQuit）
    // VST3 模式：顶层是 Editor 本身，调用 quit 无效；改为发 WM_CLOSE 到宿主无意义，
    //           直接忽略即可（关闭按钮在插件模式下仅视觉存在，宿主自带窗口 × 会关闭）。
    if (juce::JUCEApplicationBase::isStandaloneApp())
    {
        if (auto* app = juce::JUCEApplicationBase::getInstance())
            app->systemRequestedQuit();
    }
}

// 固定（置顶）：切换顶层窗口的 alwaysOnTop 属性
//   · Standalone：生效于自定义的 Y2KMainWindow（继承 DocumentWindow）
//   · 插件模式：顶层通常是宿主窗口，setAlwaysOnTop 会被 JUCE 向上调用；
//     宿主有时会忽略，但不会有负作用。
void Y2KmeterAudioProcessorEditor::handlePinClicked()
{
    setAlwaysOnTopActive (! alwaysOnTopActive);
}

// 直接设置"固定置顶"状态（供 handlePinClicked 与 StandaloneApp 恢复使用）
//   为什么要显式 false→true：JUCE Component::setAlwaysOnTop 有早退优化
//   （flag 与当前相同则 no-op）。Editor 首次 visibilityChanged 可能在
//   top==this 的瞬间被调用过一次（flag 已置 true 但实际未落到真正的顶层窗口），
//   之后再推 setAlwaysOnTop(true) 就被早退吃掉，出现"按钮按下但实际未置顶"的
//   bug。这里总是先 false 再 true 强推一次，彻底避开早退。
void Y2KmeterAudioProcessorEditor::setAlwaysOnTopActive (bool shouldBeOnTop)
{
    alwaysOnTopActive = shouldBeOnTop;

    if (auto* top = getTopLevelComponent())
    {
        if (top != this)
        {
            // 先清一次再置目标值，绕开 flag 相同时的 no-op 早退
            top->setAlwaysOnTop (false);
            top->setAlwaysOnTop (shouldBeOnTop);
        }
        else
        {
            top->setAlwaysOnTop (shouldBeOnTop);
        }
    }

    initialAlwaysOnTopApplied = true;
    repaint (getTitleBarBounds());
}

// 最小化：仅在 Standalone 下有意义；调用顶层窗口的 peer 将其最小化
void Y2KmeterAudioProcessorEditor::handleMinimiseClicked()
{
    if (auto* top = getTopLevelComponent())
    {
        if (auto* peer = top->getPeer())
            peer->setMinimised (true);
    }
}

// ==========================================================
// 布局锁定按钮（v1.8.3 新增）
//   · 切换 layoutLocked 状态；同步 workspace / Processor 持久化 / 顶层窗口 resize 上下限。
//   · 生效范围：
//       1) 顶层窗口不可拖动（Editor::mouseDown 在锁定态跳过 startDraggingComponent）
//       2) 顶层窗口不可 resize（setResizeLimits(w,h,w,h) 夹紧当前尺寸；不 recreatePeer）
//       3) 模块 tile 不可拖动/缩放/关闭（ModulePanel/Tamagotchi mouseDown 锁定态早退）
//       4) 拼豆贴画不可拖动/缩放/滑块拖动/添加（ModuleWorkspace::mouseDown 锁定态早退）
//       5) 空白区右键"添加模块"菜单/双击"添加"/文件拖入 均被阻断
//   · 关闭 / 固定 / 最小化 / 双击标题栏切换全屏 均**不**受锁定影响
//     （这些是"窗口生命周期 / 窗口尺寸档位"级操作，与"用户手动布局"语义不同）。
//   · 关键实现细节（v1.8.3 踩坑）：**不能**调用 juce::ResizableWindow::setResizable
//     来切换锁定 —— 该函数内部会 recreateDesktopWindow()，直接导致窗口在切换瞬间
//     隐藏一帧再重建，用户看到的就是"闪现消失再出现"。改走 setResizeLimits：
//     锁定时把 min/max 都夹到"当前尺寸"，corner resizer 图标即使还在，也拉不动。
// ==========================================================
void Y2KmeterAudioProcessorEditor::handleLockClicked()
{
    applyLayoutLocked (! layoutLocked, /*initial*/ false);
}

void Y2KmeterAudioProcessorEditor::applyLayoutLocked (bool locked, bool initial)
{
    layoutLocked = locked;

    // v1.8.3：构造期极简保护。
    //   Editor 构造末尾会调 applyLayoutLocked(processor.getLayoutLocked(), initial=true)
    //   来从 Processor state 恢复锁定态。此时 Editor 通常尚未 attach 到 Y2KMainWindow，
    //   或即使已挂上，顶层窗口 getWidth()/getHeight() 仍为 0。若在这种状态下调
    //   setResizeLimits(w,h,w,h)，会命中 ComponentBoundsConstrainer::setSizeLimits 内部
    //   jassert(minW>0 && minH>0)，Debug 构建表现为 EXCEPTION_BREAKPOINT (0x80000003)。
    //   保护策略：initial=true 且顶层尚未就绪时——只把状态位记下，pending 置起来，
    //   等 visibilityChanged 阶段（顶层已挂载且尺寸稳定）再补做真正 apply。
    //   locked=false 时构造期无事可做（constrainer 也没备份可还原），也走早退。
    bool topReady = false;
    if (auto* topProbe = getTopLevelComponent())
    {
        if (topProbe != this
            && dynamic_cast<juce::ResizableWindow*> (topProbe) != nullptr
            && topProbe->getWidth() > 0
            && topProbe->getHeight() > 0)
        {
            topReady = true;
        }
    }

    if (initial && (! topReady || ! locked))
    {
        pendingLockApplyOnAttach = locked; // 只有真的锁着才需要补做
        return;
    }

    // 1) 复位任何进行中的窗口拖拽（避免用户按住鼠标切锁的边缘态）
    draggingWindow = false;

    // 2) 同步 workspace（模块 tile / 贴画所在层）；也让子组件的关闭按钮 hover/press
    //    在锁定态下不响应（由 ModulePanel/TamagotchiModule 内部检查 workspace 锁定态）。
    if (workspace != nullptr)
        workspace->setLayoutLocked (locked);

    // 3) 顶层窗口尺寸约束：使用 setResizeLimits 而非 setResizable，避免 peer 重建。
    //    · 首次进入锁定前先备份当前 min/max，解锁时还原；
    //    · 锁定时把 min = max = 当前尺寸，用户即使拖 corner 也无法改变尺寸。
    if (auto* top = getTopLevelComponent())
    {
        if (top != this)
        {
            if (auto* rw = dynamic_cast<juce::ResizableWindow*> (top))
            {
                const int w = top->getWidth();
                const int h = top->getHeight();
                if (w > 0 && h > 0)
                {
                    if (auto* cbc = rw->getConstrainer())
                    {
                        if (locked)
                        {
                            if (! savedLockLimitsValid)
                            {
                                savedLockMinW = cbc->getMinimumWidth();
                                savedLockMinH = cbc->getMinimumHeight();
                                savedLockMaxW = cbc->getMaximumWidth();
                                savedLockMaxH = cbc->getMaximumHeight();
                                savedLockLimitsValid = true;
                            }
                            rw->setResizeLimits (w, h, w, h);
                        }
                        else
                        {
                            if (savedLockLimitsValid)
                            {
                                rw->setResizeLimits (savedLockMinW, savedLockMinH,
                                                     savedLockMaxW, savedLockMaxH);
                                savedLockLimitsValid = false;
                            }
                        }
                        pendingLockApplyOnAttach = false;
                    }
                }
            }
        }
    }

    // 4) 持久化：把布局锁定态写回 Processor（初次由 Processor state 驱动时不写回）
    if (! initial)
        processor.setLayoutLocked (locked);

    // 5) 视觉反馈：重绘标题栏，让 lock 按钮显示为 pressed-latched 或 raised。
    repaint (getTitleBarBounds());
}

// 同步 workspace 的 hit-test 挖洞。
//   · chrome 可见时：无挖洞（workspace 占据 y=titleBarHeight 以下区域，顶部 26px 是
//     Editor 的标题栏范围，workspace 根本不覆盖那里，自然不需要挖洞）。
//   · chrome 隐藏时：workspace 扩到全窗口，顶部 26px 中存在"浮动关闭按钮矩形 +
//     标题文字矩形"两块区域需要冒泡给 Editor 处理；挖洞坐标已转换成 workspace
//     坐标系（workspace 此时 y=0，与 Editor 坐标系一致）。
void Y2KmeterAudioProcessorEditor::updateWorkspaceHitTestHoles()
{
    if (workspace == nullptr) return;

    // 插件宿主模式：没有自画浮动关闭按钮 / 标题文字，强制清空挖洞列表即可
    if (isPluginHost)
    {
        workspace->setHitTestHoles ({});
        return;
    }

    if (! chromeDim)
    {
        workspace->setHitTestHoles ({});
        return;
    }

    juce::Array<juce::Rectangle<int>> holes;

    // 1) 浮动关闭按钮矩形（固定位置，与 ChromeHiddenOverlay::getFloatingCloseButtonRect 保持一致）
    constexpr int floatMargin = 4;
    juce::Rectangle<int> closeRect (getWidth() - floatMargin - closeButtonSize,
                                     floatMargin,
                                     closeButtonSize, closeButtonSize);
    holes.add (closeRect);

    // 2) 标题文字矩形（实际像素宽度由 overlay paint 后回写；首次未 paint 时宽度为 0 不添加）
    if (chromeHiddenOverlay != nullptr)
    {
        const int textW = chromeHiddenOverlay->getCachedTitleTextWidth();
        if (textW > 0)
        {
            juce::Rectangle<int> textRect (28, 0, textW, titleBarHeight);
            holes.add (textRect);
        }
    }

    workspace->setHitTestHoles (holes);
}

// ==========================================================
// 鼠标事件：关闭按钮 hover/press/click + TitleBar 区拖动顶层窗口
//   · chromeDim 模式下，mouseEnter/Exit 控制 TitleBar 恢复/淡化
// ==========================================================
void Y2KmeterAudioProcessorEditor::mouseMove(const juce::MouseEvent& e)
{
    // 插件宿主模式：不处理按钮 hover 与 chromeDim 分支；但仍要处理"标题文字 hover"，
    //   让鼠标悬停到 "Y2Kmeter v1.1 iisaacbeats.cn" 上时出现手型光标和下划线，
    //   以便点击打开官网。
    if (isPluginHost)
    {
        mouseInsideEditor = true;

        auto tt = getTitleTextBounds();
        const int textW = juce::jmax (0, juce::jmin (cachedTitleTextW, tt.getWidth()));
        juce::Rectangle<int> hotspot (tt.getX(), tt.getY(), textW, tt.getHeight());
        const bool hovered = textW > 0 && hotspot.contains (e.getPosition());
        if (hovered != titleTextHovered)
        {
            titleTextHovered = hovered;
            setMouseCursor (hovered ? juce::MouseCursor::PointingHandCursor
                                    : juce::MouseCursor::NormalCursor);
            repaint (getTitleBarBounds());
        }
        return;
    }

    mouseInsideEditor = true;

    auto updateHover = [&] (bool& state, juce::Rectangle<int> rc)
    {
        const bool h = rc.contains (e.getPosition());
        if (h != state) { state = h; repaint (rc); }
    };

    if (chromeDim)
    {
        // chrome 隐藏态：Editor 负责处理"浮动关闭按钮 / 标题文字"的 hover，
        //   workspace 已通过 hit-test 挖洞让这些事件冒泡过来；视觉状态由 overlay 负责绘制。
        if (chromeHiddenOverlay == nullptr)
            return;

        const auto p = e.getPosition();
        const auto closeRc = chromeHiddenOverlay->getFloatingCloseButtonRect()
                                .translated (chromeHiddenOverlay->getX(),
                                             chromeHiddenOverlay->getY());
        const bool overClose = closeRc.contains (p);
        chromeHiddenOverlay->setCloseButtonHovered (overClose);
        closeButtonHovered = overClose; // 复用 Editor 的状态跟踪 press/click 流转

        // 标题文字 hover（宽度按 overlay 缓存的实际像素宽度计算，避免整条横条命中）
        const int textW = chromeHiddenOverlay->getCachedTitleTextWidth();
        juce::Rectangle<int> textRc (28 + chromeHiddenOverlay->getX(),
                                      chromeHiddenOverlay->getY(),
                                      juce::jmax (0, textW),
                                      titleBarHeight);
        const bool overText = textW > 0 && textRc.contains (p);
        chromeHiddenOverlay->setTitleTextHovered (overText);
        if (overText != titleTextHovered)
        {
            titleTextHovered = overText;
            setMouseCursor (overText ? juce::MouseCursor::PointingHandCursor
                                     : juce::MouseCursor::NormalCursor);
        }

        // 隐藏态下不再有 pin/minimise 按钮；强制清零以免切换回显示态时有残留 hover 视觉
        if (pinButtonHovered) pinButtonHovered = false;
        if (minButtonHovered) minButtonHovered = false;
    }
    else
    {
        updateHover (closeButtonHovered, getCloseButtonBounds());
        updateHover (pinButtonHovered,   getPinButtonBounds());
        updateHover (minButtonHovered,   getMinimiseButtonBounds());
        updateHover (lockButtonHovered,  getLockButtonBounds());

        // 标题文字 hover 检测（实际文字像素矩形，宽度由 paint 回写）
        auto tt = getTitleTextBounds();
        const int textW = juce::jmax (0, juce::jmin (cachedTitleTextW, tt.getWidth()));
        juce::Rectangle<int> hotspot (tt.getX(), tt.getY(), textW, tt.getHeight());
        const bool hovered = textW > 0 && hotspot.contains (e.getPosition());
        if (hovered != titleTextHovered)
        {
            titleTextHovered = hovered;
            setMouseCursor (hovered ? juce::MouseCursor::PointingHandCursor
                                    : juce::MouseCursor::NormalCursor);
            repaint (getTitleBarBounds());
        }
    }
}

// v1.8.6：auto-hide 模式下的 mouseEnter —— 鼠标悬停暂时恢复 chrome
void Y2KmeterAudioProcessorEditor::mouseEnter(const juce::MouseEvent&)
{
    mouseInsideEditor = true;

    // 布局预设切换期间抑制 auto-show（见 onLayoutPresetChanged）
    if (suppressAutoShowCounter > 0)
        return;

    // Hide 后必须等鼠标先离开窗口一次，才允许鼠标再次进入时触发 hover show
    if (autoHideNeedsExitFirst)
        return;

    // 仅在 auto-hide 模式 + chrome 当前隐藏时：暂时显示 chrome（保持 lock+pin）
    if (autoHideMode && workspace != nullptr && ! workspace->isChromeVisible())
    {
        temporaryChromeShow = true;
        workspace->setChromeVisible (true);
        temporaryChromeShow = false;
    }
}

void Y2KmeterAudioProcessorEditor::mouseExit(const juce::MouseEvent& e)
{
    // 插件宿主模式：没有按钮 hover 要清理，但标题文字 hover 状态仍要复位
    //   （否则鼠标移出顶部抬头后，下划线和手型光标不会消失）。
    if (isPluginHost)
    {
        if (titleTextHovered)
        {
            titleTextHovered = false;
            setMouseCursor (juce::MouseCursor::NormalCursor);
            repaint (getTitleBarBounds());
        }
        return;
    }

    auto clearHover = [&] (bool& state, juce::Rectangle<int> rc)
    {
        if (state) { state = false; repaint (rc); }
    };
    // 统一清理（两套 rect 都 repaint 一下开销可忽略）
    clearHover (closeButtonHovered, getCloseButtonBounds());
    clearHover (pinButtonHovered,   getPinButtonBounds());
    clearHover (minButtonHovered,   getMinimiseButtonBounds());
    clearHover (lockButtonHovered,  getLockButtonBounds());

    if (titleTextHovered)
    {
        titleTextHovered = false;
        setMouseCursor (juce::MouseCursor::NormalCursor);
        repaint (getTitleBarBounds());
    }

    // 隐藏态：同步清空 overlay 的 hover 视觉
    if (chromeHiddenOverlay != nullptr)
    {
        chromeHiddenOverlay->setCloseButtonHovered (false);
        chromeHiddenOverlay->setTitleTextHovered (false);
    }

    // v1.8.6：auto-hide 模式下鼠标离开窗口 → 重新隐藏 chrome（保持 lock+pin）
    //   · 必须检查鼠标屏幕坐标是否真正离开了顶层窗口。
    //   · 用户从抬头区移到模块/工具栏区也会触发 Editor::mouseExit，
    //     仅靠组件边界判断会误隐藏——必须用屏幕坐标最终裁决。
    //   · chrome 隐藏时真正离开也要清零 autoHideNeedsExitFirst，
    //     否则下次 mouseEnter 永远被拦截。
    //   · 使用 MouseEvent 的屏幕坐标而非 Desktop::getMainMouseSource()
    //     （后者在高速移动时可能返回 stale 坐标）。
    if (autoHideMode)
    {
        auto* top = getTopLevelComponent();
        if (top != nullptr)
        {
            const auto mouseScreen = e.getScreenPosition().toInt();
            const auto topScreenBounds = (top == this) ? getScreenBounds() : top->getScreenBounds();
            if (! topScreenBounds.contains (mouseScreen))
            {
                if (workspace != nullptr && workspace->isChromeVisible())
                    workspace->setChromeVisible (false);
                autoHideNeedsExitFirst = false;
            }
        }
    }

    mouseInsideEditor = false;
}

void Y2KmeterAudioProcessorEditor::mouseDown(const juce::MouseEvent& e)
{
    // 插件宿主模式：不处理右侧按钮（因为根本没画）和窗口拖拽（宿主负责），
    //   但允许"标题文字"点击打开官网。其他区域点击继续冒泡给子组件。
    if (isPluginHost)
    {
        auto tt = getTitleTextBounds();
        const int textW = juce::jmax (0, juce::jmin (cachedTitleTextW, tt.getWidth()));
        juce::Rectangle<int> hotspot (tt.getX(), tt.getY(), textW, tt.getHeight());
        if (textW > 0 && hotspot.contains (e.getPosition()))
        {
            juce::URL ("https://iisaacbeats.cn").launchInDefaultBrowser();
        }
        return;
    }

    // chrome 隐藏态：仅处理浮动关闭按钮 + 标题文字点击；其他区域不处理（也不支持从顶部拖窗口）
    if (chromeDim)
    {
        if (chromeHiddenOverlay == nullptr) return;

        const auto p = e.getPosition();
        const auto closeRc = chromeHiddenOverlay->getFloatingCloseButtonRect()
                                .translated (chromeHiddenOverlay->getX(),
                                             chromeHiddenOverlay->getY());
        if (closeRc.contains (p))
        {
            closeButtonPressed = true;
            chromeHiddenOverlay->setCloseButtonPressed (true);
            return;
        }

        const int textW = chromeHiddenOverlay->getCachedTitleTextWidth();
        juce::Rectangle<int> textRc (28 + chromeHiddenOverlay->getX(),
                                      chromeHiddenOverlay->getY(),
                                      juce::jmax (0, textW),
                                      titleBarHeight);
        if (textW > 0 && textRc.contains (p))
        {
            juce::URL ("https://iisaacbeats.cn").launchInDefaultBrowser();
        }
        return;
    }

    // 1) 命中标题栏右侧三个按钮之一
    if (getCloseButtonBounds().contains (e.getPosition()))
    {
        closeButtonPressed = true;
        repaint (getCloseButtonBounds());
        return;
    }
    if (getPinButtonBounds().contains (e.getPosition()))
    {
        pinButtonPressed = true;
        repaint (getPinButtonBounds());
        return;
    }
    if (getMinimiseButtonBounds().contains (e.getPosition()))
    {
        minButtonPressed = true;
        repaint (getMinimiseButtonBounds());
        return;
    }
    if (getLockButtonBounds().contains (e.getPosition()))
    {
        lockButtonPressed = true;
        repaint (getLockButtonBounds());
        return;
    }

    // 2) 只有 TitleBar 区域才允许拖拽顶层窗口（模块 / toolbar 区不参与窗口拖拽）
    if (! getTitleBarBounds().contains(e.getPosition()))
        return;

    // 2.1) 命中标题文字热区 → 打开官网（iisaacbeats.cn），不启动窗口拖拽
    {
        auto tt = getTitleTextBounds();
        const int textW = juce::jmax (0, juce::jmin (cachedTitleTextW, tt.getWidth()));
        juce::Rectangle<int> hotspot (tt.getX(), tt.getY(), textW, tt.getHeight());
        if (textW > 0 && hotspot.contains (e.getPosition()))
        {
            juce::URL ("https://iisaacbeats.cn").launchInDefaultBrowser();
            return;
        }
    }

    if (juce::JUCEApplicationBase::isStandaloneApp())
    {
        // 锁定态：不启动顶层窗口拖拽。双击全屏的前置 mouseDown 不会受影响，
        //   因为下面不会将 draggingWindow 置 true，mouseDrag 也不会拖。双击全屏
        //   本身在锁定态下仍允许（属于"窗口尺寸"而非"用户拖拽"）。
        if (! layoutLocked)
        {
            if (auto* top = getTopLevelComponent())
            {
                windowDragger.startDraggingComponent(top, e.getEventRelativeTo(top));
                draggingWindow = true;
            }
        }
    }
}

void Y2KmeterAudioProcessorEditor::mouseDrag(const juce::MouseEvent& e)
{
    // 插件宿主模式：无 chrome 拖窗需求
    if (isPluginHost) return;

    if (draggingWindow)
    {
        if (auto* top = getTopLevelComponent())
            windowDragger.dragComponent(top, e.getEventRelativeTo(top), nullptr);
    }
}

void Y2KmeterAudioProcessorEditor::mouseUp(const juce::MouseEvent& e)
{
    // 插件宿主模式：无 chrome 按钮点击处理
    if (isPluginHost) return;

    draggingWindow = false;

    // 按下与抬起都落在同一按钮内才触发 click —— XP 风按钮的标准行为
    if (closeButtonPressed)
    {
        closeButtonPressed = false;

        // 根据当前 chrome 状态决定使用"浮动关闭按钮"还是"标题栏内的关闭按钮"的几何
        juce::Rectangle<int> rc;
        if (chromeDim && chromeHiddenOverlay != nullptr)
        {
            rc = chromeHiddenOverlay->getFloatingCloseButtonRect()
                    .translated (chromeHiddenOverlay->getX(),
                                 chromeHiddenOverlay->getY());
            chromeHiddenOverlay->setCloseButtonPressed (false);
        }
        else
        {
            rc = getCloseButtonBounds();
        }

        const bool stillOn = rc.contains (e.getPosition());
        repaint (rc);
        if (stillOn) handleCloseClicked();
        return;
    }
    if (pinButtonPressed)
    {
        pinButtonPressed = false;
        const bool stillOn = getPinButtonBounds().contains (e.getPosition());
        repaint (getPinButtonBounds());
        if (stillOn) handlePinClicked();
        return;
    }
    if (minButtonPressed)
    {
        minButtonPressed = false;
        const bool stillOn = getMinimiseButtonBounds().contains (e.getPosition());
        repaint (getMinimiseButtonBounds());
        if (stillOn) handleMinimiseClicked();
        return;
    }
    if (lockButtonPressed)
    {
        lockButtonPressed = false;
        const bool stillOn = getLockButtonBounds().contains (e.getPosition());
        repaint (getLockButtonBounds());
        if (stillOn) handleLockClicked();
        return;
    }
}

// ==========================================================
// 双击标题栏空白区 → 切换顶层窗口全屏 / 还原
//   · 参考 Windows 系统标题栏双击最大化的用户习惯；
//   · 命中矩形：抬头 titleBarHeight 高度内、避开右侧三个按钮和左侧标题文字热区；
//   · 仅 Standalone 有效，插件宿主模式下窗口由 DAW 管理，直接忽略；
//   · chrome 隐藏态下不响应（隐藏时"标题栏"实际不存在，浮层区域也不合适当"抬头"）；
//   · 全屏与还原都通过顶层 Component::setFullScreen(bool)；JUCE ComponentPeer
//     内部会记住 restore-bounds，切回时自动还原到原来的位置和大小。
//
// 踩坑记录：
//   · JUCE 双击流程是 mouseDown → mouseUp → mouseDoubleClick，中间的 mouseDown
//     会启动 windowDragger.startDraggingComponent()。这里进入全屏后要主动把
//     draggingWindow 复位并调用 setFullScreen 前先 clear，否则接下来任何 mouseDrag
//     都会尝试拖动全屏窗口（在 Windows 上 dragComponent 对全屏窗口是 no-op，
//     但视觉上会给用户"卡了一下"的错觉）。
// ==========================================================
void Y2KmeterAudioProcessorEditor::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (isPluginHost) return;
    if (chromeDim)    return;

    // 命中区域：仅标题栏矩形，且不落在三个按钮 + 标题文字热区
    const auto pos = e.getPosition();
    if (! getTitleBarBounds().contains (pos)) return;

    if (getCloseButtonBounds()   .contains (pos)) return;
    if (getPinButtonBounds()     .contains (pos)) return;
    if (getMinimiseButtonBounds().contains (pos)) return;
    if (getLockButtonBounds()    .contains (pos)) return;

    {
        auto tt = getTitleTextBounds();
        const int textW = juce::jmax (0, juce::jmin (cachedTitleTextW, tt.getWidth()));
        juce::Rectangle<int> hotspot (tt.getX(), tt.getY(), textW, tt.getHeight());
        if (textW > 0 && hotspot.contains (pos)) return;
    }

    if (! juce::JUCEApplicationBase::isStandaloneApp()) return;

    auto* top = getTopLevelComponent();
    if (top == nullptr) return;

    // 前置的 mouseDown 已启动了 windowDragger —— 复位，避免全屏后残留拖动状态
    draggingWindow = false;

    // 全屏 API 定义在 juce::ResizableWindow 上（DocumentWindow → ResizableWindow），
    // 不在 juce::Component / juce::TopLevelWindow 上。Standalone 的顶层
    // Y2KMainWindow : DocumentWindow，能命中；否则退回到 ComponentPeer 层。
    if (auto* rw = dynamic_cast<juce::ResizableWindow*> (top))
    {
        const bool nowFullScreen = rw->isFullScreen();
        rw->setFullScreen (! nowFullScreen);
    }
    else if (auto* peer = top->getPeer())
    {
        // Fallback：直接走 peer 层。restore-bounds 由 peer 内部维护。
        peer->setFullScreen (! peer->isFullScreen());
    }
}

// ==========================================================
// 音频源下拉：Editor 对外 API → 转发到 Workspace
// ==========================================================
void Y2KmeterAudioProcessorEditor::setAudioSourceItems (
    const juce::Array<AudioSourceEntry>& items,
    const juce::String& selectedSourceId)
{
    if (workspace == nullptr) return;

    juce::Array<ModuleWorkspace::AudioSourceItem> ws;
    ws.ensureStorageAllocated (items.size());
    for (const auto& it : items)
        ws.add ({ it.displayName, it.sourceId, it.isLoopback });

    workspace->setAudioSourceItems (ws, selectedSourceId);
}

// ==========================================================
// 持久化辅助：chrome 可见性（Standalone 在 save/restore 时使用）
// ==========================================================
bool Y2KmeterAudioProcessorEditor::isChromeVisible() const
{
    return workspace != nullptr ? workspace->isChromeVisible() : true;
}

void Y2KmeterAudioProcessorEditor::setChromeVisible (bool shouldBeVisible)
{
    if (workspace != nullptr)
        workspace->setChromeVisible (shouldBeVisible);
}

// ==========================================================
// timerCallback —— 10Hz 拉 Processor 的 CPU 占用，广播给所有模块；
//                  同时按 1s 窗口换算实际 FPS，下发给 workspace 的 FPS 标签
// ==========================================================
void Y2KmeterAudioProcessorEditor::timerCallback()
{
    if (workspace == nullptr) return;

    // v1.8.6：递减 auto-show 抑制计数器（用于 onChromeVisibleChanged 异步事件窗口保护）
    if (suppressAutoShowCounter > 0)
        --suppressAutoShowCounter;

    // v1.9.x：新手引导 STEP 2 条件检查（孵化完成检测）
    if (! isPluginHost)
        checkTutorialStep2Condition();

    // v1.8.6：焦点保护 —— timer 轮询检测前台状态，弥补 mouseExit 漏判场景
    //   · 始终追踪 windowWasForeground（即使 autoHideMode 为 false），确保首次 HIDE
    //     时前台状态已同步，避免因初始值 false 导致误触发 auto-show。
    //   · 仅当 autoHideMode 活跃时才执行 hide/show 动作。
    //   · Timer 周期为 ~100ms（startTimerHz(10)），响应延时 < 200ms，用户感知不明显。
    {
        auto* top = getTopLevelComponent();
        if (top != nullptr)
        {
            auto* focused = juce::Component::getCurrentlyFocusedComponent();
            const bool isForeground = (focused != nullptr
                                       && focused->getTopLevelComponent() == top);
            if (isForeground != windowWasForeground)
            {
                windowWasForeground = isForeground;
                if (autoHideMode)
                {
                    if (isForeground)
                    {
                        // 用户切回本软件 → auto-show（模拟 hover show 行为）
                        if (! workspace->isChromeVisible()
                            && suppressAutoShowCounter == 0)
                        {
                            temporaryChromeShow = true;
                            workspace->setChromeVisible (true);
                            temporaryChromeShow = false;
                        }
                        autoHideNeedsExitFirst = false;
                    }
                    else
                    {
                        // 用户切到别的软件 → auto-hide
                        if (workspace->isChromeVisible())
                            workspace->setChromeVisible (false);
                        autoHideNeedsExitFirst = true;
                    }
                }
            }
        }
    }

    // 仅当存在 Tamagotchi 模块时，才保活/计算对应的音频信号。
    const int n = workspace->getNumModules();
    juce::Array<TamagotchiModule*> tamagotchiModules;
    tamagotchiModules.ensureStorageAllocated (n);

    for (int i = 0; i < n; ++i)
        if (auto* tamagotchi = dynamic_cast<TamagotchiModule*> (workspace->getModule (i)))
            tamagotchiModules.add (tamagotchi);

    const bool hasTamagotchi = ! tamagotchiModules.isEmpty();
    if (hasTamagotchi != tamagotchiSignalRetained)
    {
        if (hasTamagotchi)
            processor.getAnalyserHub().retain (AnalyserHub::Kind::Loudness);
        else
            processor.getAnalyserHub().release (AnalyserHub::Kind::Loudness);

        tamagotchiSignalRetained = hasTamagotchi;
    }

    float signal01 = 0.0f;
    if (hasTamagotchi)
    {
        if (auto frame = processor.getAnalyserHub().getLatestFrame())
        {
            if (frame->has (AnalyserHub::Kind::Loudness))
            {
                const float rmsDb = juce::jmax (frame->loudness.rmsL, frame->loudness.rmsR);
                signal01 = juce::jlimit (0.0f, 1.0f, juce::Decibels::decibelsToGain (rmsDb, -144.0f));
            }
            else if (frame->has (AnalyserHub::Kind::Dynamics))
            {
                const float rmsDb = juce::jmax (frame->dynamics.rmsL, frame->dynamics.rmsR);
                signal01 = juce::jlimit (0.0f, 1.0f, juce::Decibels::decibelsToGain (rmsDb, -144.0f));
            }
            else if (frame->has (AnalyserHub::Kind::Oscilloscope))
            {
                double sumSqL = 0.0;
                double sumSqR = 0.0;
                for (int i = 0; i < AnalyserHub::oscilloscopeBufferSize; ++i)
                {
                    const double l = (double) frame->oscL[(size_t) i];
                    const double r = (double) frame->oscR[(size_t) i];
                    sumSqL += l * l;
                    sumSqR += r * r;
                }

                const double invN = 1.0 / (double) AnalyserHub::oscilloscopeBufferSize;
                const float rmsLinear = (float) std::sqrt (juce::jmax (0.0, juce::jmax (sumSqL * invN, sumSqR * invN)));
                signal01 = juce::jlimit (0.0f, 1.0f, rmsLinear);
            }
        }
    }

    const float cpu01 = (float) processor.getCpuLoad();
    for (int i = 0; i < n; ++i)
        if (auto* m = workspace->getModule (i))
            m->setCpuLoad (cpu01);

    for (auto* tamagotchi : tamagotchiModules)
        tamagotchi->setSignalLevel01 (signal01);

    // FPS 统计：每秒更新一次显示（使用高精度时戳防止 wall-clock 跨日问题）
    const double nowMs   = juce::Time::getMillisecondCounterHiRes();
    const double deltaMs = nowMs - lastFpsTimeMs;
    if (deltaMs >= 1000.0)
    {
        const juce::int64 cur = frameCounter.load (std::memory_order_relaxed);
        const juce::int64 diff = cur - lastFrameCounterSample;
        const float fps = (float) (diff * 1000.0 / deltaMs);

        float displayedFps = juce::jmin ((float) userRequestedFpsLimit, fps);
        if (userRequestedFpsLimit == 30 && fps >= 28.0f)
            displayedFps = 30.0f;
        else if (userRequestedFpsLimit == 60 && fps >= 58.0f)
            displayedFps = 60.0f;

        workspace->setMeasuredFps (displayedFps);
        applyAdaptiveFrameRate (fps);

        lastFrameCounterSample = cur;
        lastFpsTimeMs          = nowMs;
    }
}

// ==========================================================
// applyAdaptiveFrameRate —— 根据测得 FPS 动态下调 / 回升 FrameDispatcher
//   · 插件宿主更容易受 UI 消息循环节流，上限 48Hz，分档降帧；持续 4 tick
    //   达标才回升。
//   · Standalone 贴近用户设定，低于 60% 下调，持续 2 tick 达标即回升。
//   · 只在目标值真变化或当前 Hub hz 不一致时才调 startFrameDispatcher，
//     双保险避免频繁 startTimerHz。
// ==========================================================
void Y2KmeterAudioProcessorEditor::applyAdaptiveFrameRate (float measuredFps)
{
    auto& hub = processor.getAnalyserHub();

    const int requested = juce::jlimit (15, 120, userRequestedFpsLimit);
    const int maxAllowedHz = isPluginHost ? juce::jmin (48, requested) : requested;
    const int currentHz = juce::jlimit (15, maxAllowedHz, adaptiveDispatchHz);
    int targetHz = currentHz;

    // 插件宿主里更容易受 UI 消息循环节流，优先保证稳定感而非硬追目标帧。
    if (isPluginHost)
    {
        int requestedDropHz = currentHz;
        if (measuredFps < (float) currentHz * 0.55f)
        {
            requestedDropHz = juce::jmax (15, currentHz - 12);
        }
        else if (measuredFps < (float) currentHz * 0.70f)
        {
            requestedDropHz = juce::jmax (16, currentHz - 8);
        }
        else if (measuredFps < (float) currentHz * 0.84f)
        {
            requestedDropHz = juce::jmax (18, currentHz - 4);
        }

        if (requestedDropHz < currentHz)
        {
            adaptiveRecoverTicks = 0;
            if (++adaptiveDropTicks >= 3)
            {
                targetHz = requestedDropHz;
                adaptiveDropTicks = 0;
            }
        }
        else if (currentHz < maxAllowedHz && measuredFps >= (float) currentHz * 0.92f)
        {
            adaptiveDropTicks = 0;
            ++adaptiveRecoverTicks;
            if (adaptiveRecoverTicks >= 4)
            {
                targetHz = juce::jmin (maxAllowedHz, currentHz + 6);
                adaptiveRecoverTicks = 0;
            }
        }
        else
        {
            adaptiveDropTicks = 0;
            adaptiveRecoverTicks = 0;
        }
    }
    else
    {
        // standalone 优先贴近用户设定，但在重负载瞬间允许轻微降帧。
        if (measuredFps < (float) currentHz * 0.70f)
        {
            adaptiveRecoverTicks = 0;
            if (++adaptiveDropTicks >= 2)
            {
                targetHz = juce::jmax (20, currentHz - 6);
                adaptiveDropTicks = 0;
            }
        }
        else if (currentHz < maxAllowedHz && measuredFps >= (float) currentHz * 0.92f)
        {
            adaptiveDropTicks = 0;
            ++adaptiveRecoverTicks;
            if (adaptiveRecoverTicks >= 2)
            {
                targetHz = juce::jmin (maxAllowedHz, currentHz + 6);
                adaptiveRecoverTicks = 0;
            }
        }
        else if (currentHz >= maxAllowedHz)
        {
            adaptiveDropTicks = 0;
            adaptiveRecoverTicks = 0;
        }
        else
        {
            adaptiveDropTicks = 0;
            adaptiveRecoverTicks = 0;
        }
    }

    targetHz = juce::jlimit (15, maxAllowedHz, targetHz);

    if (targetHz != adaptiveDispatchHz || hub.getFrameDispatcherHz() != targetHz)
    {
        adaptiveDispatchHz = targetHz;
        hub.startFrameDispatcher (adaptiveDispatchHz);
    }
}

// FrameListener 的实际实现在文件顶端的 FpsFrameListener 嵌套类中定义，
// 这里不再重复。
