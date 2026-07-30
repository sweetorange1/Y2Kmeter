// ==========================================================
// source/ui/UpdateDialog.cpp
//   自定义 PinkXP 风格更新提示弹窗实现。
//
//   通过 addToDesktop 创建独立原生小窗口（480×340），
//   setAlwaysOnTop(true) → WS_EX_TOPMOST 确保始终置顶。
//   视觉风格对齐软件模块窗口：硬阴影 + 像素角标 + PinkXP 标题栏。
//   标题栏支持鼠标拖拽移动窗口位置。
//   两个操作按钮：Download / Remind Me Later。
//   按钮操作后通过 CloseDialog() → removeFromDesktop + 异步自删除。
// ==========================================================

#include "UpdateDialog.h"
#include "PinkXPStyle.h"
#include "source/network/UpdateChecker.h"

namespace y2k {
namespace ui {

// ==========================================================
// 构造
// ==========================================================
UpdateDialog::UpdateDialog(const y2k::network::UpdateInfo& info,
                           juce::PropertiesFile* settings,
                           std::function<void()> onClose)
    : info_(info)
    , settings_(settings)
    , onClose_(std::move(onClose)) {
  setOpaque(false);
  setInterceptsMouseClicks(true, true);
}

// ==========================================================
// paint
//   三层结构：硬阴影 → 凸起面板（Win95 边框）→ 像素角标
//   面板内部：PinkXP 标题栏 + 内容区 + 按钮行
// ==========================================================
void UpdateDialog::paint(juce::Graphics& g) {
  const auto dlg = GetDialogBounds();

  // 0. 硬阴影（右侧+底部 4px 偏移，模拟模块窗口的层次感）
  PinkXP::drawHardShadow(g, dlg, 4);

  // 1. 对话框外壳：凸起边框（Win95 风格）
  PinkXP::drawRaised(g, dlg, PinkXP::btnFace);

  // 2. 面板内部区域（边框内缩 6px）
  auto inner = dlg.reduced(6);
  if (inner.getHeight() < 60) return;

  // 3. 标题栏（使用与模块窗口一致的风格）
  auto titleRow = inner.removeFromTop(kTitleBarH);
  g.setColour(PinkXP::sel);
  g.fillRect(titleRow);

  // 标题栏顶部高光线（基于当前主题 sel 色推演）
  g.setColour(PinkXP::sel.brighter(0.35f));
  g.fillRect(titleRow.getX(), titleRow.getY(), titleRow.getWidth(), 1);

  // 标题栏底部暗线
  g.setColour(PinkXP::sel.darker(0.45f));
  g.fillRect(titleRow.getX(), titleRow.getBottom() - 1, titleRow.getWidth(), 1);

  // 标题图标（根据 force_update 显示不同图标）
  const auto iconArea = titleRow.removeFromLeft(24).reduced(4, 2);
  g.setColour(PinkXP::selInk);
  g.setFont(PinkXP::getFont(12.0f, juce::Font::bold));
  g.drawText(info_.force_update ? "!" : "i", iconArea,
             juce::Justification::centred, false);

  // 标题文字（带阴影增强可读性）
  g.setColour(PinkXP::selInk.contrasting().withAlpha(0.45f));
  g.setFont(PinkXP::getFont(11.0f, juce::Font::bold));
  g.drawText("Update Available", titleRow.reduced(6, 0).translated(1, 1),
             juce::Justification::centredLeft, false);

  g.setColour(PinkXP::selInk);
  g.drawText("Update Available", titleRow.reduced(6, 0),
             juce::Justification::centredLeft, false);

  // 4. 内容区域
  auto body = inner.reduced(8, 6);
  if (body.getHeight() < 40) return;

  // 左侧状态图标区
  auto iconCol = body.removeFromLeft(56);
  auto iconBox = juce::Rectangle<int>(iconCol.getX() + 4,
                                       iconCol.getY() + 6, 42, 42);
  if (info_.force_update) {
    g.setColour(PinkXP::pink600.withAlpha(0.12f));
    g.fillRoundedRectangle(iconBox.toFloat(), 4.0f);
    g.setColour(PinkXP::pink600);
    g.setFont(PinkXP::getFont(22.0f, juce::Font::bold));
    g.drawText("!!", iconBox, juce::Justification::centred, false);
  } else {
    g.setColour(PinkXP::pink300.withAlpha(0.20f));
    g.fillRoundedRectangle(iconBox.toFloat(), 4.0f);
    g.setColour(PinkXP::pink500);
    g.setFont(PinkXP::getFont(22.0f, juce::Font::bold));
    g.drawText("i", iconBox, juce::Justification::centred, false);
  }

  auto textCol = body;

  // 第一行：有新版本提示
  g.setColour(PinkXP::ink);
  g.setFont(PinkXP::getFont(11.0f, juce::Font::bold));
  g.drawText("A new version of Y2Kmeter is available!",
             textCol.getX(), textCol.getY(),
             textCol.getWidth(), 18,
             juce::Justification::centredLeft, false);

  // 第二行：当前版本 + 最新版本
  auto verRow = textCol.withY(textCol.getY() + 20).withHeight(16);
  g.setColour(PinkXP::pink700.withAlpha(0.7f));
  g.setFont(PinkXP::getFont(9.0f, juce::Font::plain));
  g.drawText("Current: " + juce::String(JucePlugin_VersionString)
               + "    \xe2\x86\x92    Latest: " + info_.latest_version,
             verRow, juce::Justification::centredLeft, false);

  // 更新日志区域
  if (info_.changelog.isNotEmpty()) {
    auto changeArea = juce::Rectangle<int>(
        textCol.getX(), verRow.getBottom() + 8,
        textCol.getWidth(),
        inner.getBottom() - verRow.getBottom() - 8 - 40);

    if (changeArea.getHeight() > 20) {
      g.setColour(PinkXP::pink700.withAlpha(0.6f));
      g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
      g.drawText("What's new:",
                 changeArea.getX(), changeArea.getY(),
                 changeArea.getWidth(), 16,
                 juce::Justification::centredLeft, false);

      auto logArea = changeArea.withY(changeArea.getY() + 14)
                                .withHeight(changeArea.getHeight() - 14);
      g.setColour(PinkXP::pink50.withAlpha(0.5f));
      g.fillRoundedRectangle(logArea.toFloat().reduced(0, 2), 2.0f);

      g.setColour(PinkXP::ink.withAlpha(0.75f));
      g.setFont(PinkXP::getFont(8.5f, juce::Font::plain));
      g.drawFittedText(info_.changelog,
                       logArea.reduced(6, 4),
                       juce::Justification::topLeft,
                       12);
    }
  }

  // 5. 按钮行（两个按钮水平居中）
  for (int i = 0; i < kNumBtns; ++i) {
    auto btnRect = GetButtonBounds(i);

    juce::Colour fill = PinkXP::btnFace;
    if (pressedBtn_ == i) {
      fill = PinkXP::pink300;
    } else if (hoveredBtn_ == i) {
      fill = PinkXP::pink200;
    }

    PinkXP::drawRaised(g, btnRect, fill);

    juce::Colour textColour = PinkXP::ink;
    if (i == static_cast<int>(ButtonId::kDownload)) {
      textColour = PinkXP::pink700;
    }

    g.setColour(textColour);
    g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));

    juce::String label;
    switch (static_cast<ButtonId>(i)) {
      case ButtonId::kDownload: label = "Download"; break;
      case ButtonId::kRemind:   label = "Remind Me Later"; break;
    }

    if (i == static_cast<int>(ButtonId::kRemind)) {
      g.setFont(PinkXP::getFont(8.5f, juce::Font::bold));
    }

    g.drawText(label, btnRect, juce::Justification::centred, false);
  }

  // 6. 四角像素 L 形装饰（Y2K 典型元素，对齐模块窗口风格）
  PinkXP::drawPixelCorners(g, dlg, PinkXP::pink400, 6, 2);
}

// ==========================================================
// resized
// ==========================================================
void UpdateDialog::resized() {
  // 弹窗窗口尺寸固定为 kDlgW×kDlgH，创建后不再变化。
}

// ==========================================================
// 鼠标事件
// ==========================================================
void UpdateDialog::mouseDown(const juce::MouseEvent& e) {
  const int hit = HitTestButton(e.getPosition());
  if (hit >= 0) {
    pressedBtn_ = hit;
    repaint();
    return;
  }

  // 标题栏区域：开始拖拽
  if (GetTitleBarBounds().contains(e.getPosition())) {
    is_dragging_ = true;
    drag_offset_ = e.getPosition();
    return;
  }
}

void UpdateDialog::mouseDrag(const juce::MouseEvent& e) {
  if (!is_dragging_) return;

  // 计算窗口新位置：当前屏幕坐标 + 鼠标相对于 drag_offset 的移动量
  const auto delta = e.getPosition() - drag_offset_;
  auto newBounds = getScreenBounds();
  newBounds.translate(delta.x, delta.y);
  setBounds(newBounds);
}

void UpdateDialog::mouseMove(const juce::MouseEvent& e) {
  if (is_dragging_) return;

  const int hit = HitTestButton(e.getPosition());
  if (hit != hoveredBtn_) {
    hoveredBtn_ = hit;
    repaint();
  }
}

void UpdateDialog::mouseUp(const juce::MouseEvent& e) {
  if (is_dragging_) {
    is_dragging_ = false;
    return;
  }

  if (pressedBtn_ >= 0) {
    const int hit = HitTestButton(e.getPosition());
    const int triggered = (hit == pressedBtn_) ? pressedBtn_ : -1;
    pressedBtn_ = -1;
    repaint();

    if (triggered >= 0) {
      ExecuteButton(triggered);
    }
  }
}

// ==========================================================
// 布局计算
// ==========================================================
juce::Rectangle<int> UpdateDialog::GetDialogBounds() const {
  return getLocalBounds();
}

juce::Rectangle<int> UpdateDialog::GetTitleBarBounds() const {
  // 对话框面板内缩 6px 边框后，取顶部 kTitleBarH 高度
  return GetDialogBounds().reduced(6).removeFromTop(kTitleBarH);
}

juce::Rectangle<int> UpdateDialog::GetButtonBounds(int idx) const {
  const auto dlg = GetDialogBounds();
  const int totalBtnW = kBtnW * kNumBtns + kBtnGap * (kNumBtns - 1);
  const int startX = dlg.getX() + (dlg.getWidth() - totalBtnW) / 2;
  const int btnY = dlg.getBottom() - 42;

  return juce::Rectangle<int>(
      startX + idx * (kBtnW + kBtnGap), btnY, kBtnW, kBtnH);
}

int UpdateDialog::HitTestButton(juce::Point<int> pos) const {
  for (int i = 0; i < kNumBtns; ++i) {
    if (GetButtonBounds(i).contains(pos)) {
      return i;
    }
  }
  return -1;
}

void UpdateDialog::ExecuteButton(int idx) {
  switch (static_cast<ButtonId>(idx)) {
    case ButtonId::kDownload: {
      if (info_.download_url.isNotEmpty()) {
        juce::URL(info_.download_url).launchInDefaultBrowser();
      } else {
        juce::URL("https://iisaacbeats.cn").launchInDefaultBrowser();
      }
      break;
    }
    case ButtonId::kRemind: {
      break;
    }
  }

  CloseDialog();
}

// ==========================================================
// CloseDialog：隐藏、回调、从桌面移除并异步自删除
// ==========================================================
void UpdateDialog::CloseDialog() {
  setVisible(false);
  if (onClose_) {
    onClose_();
  }
  juce::MessageManager::callAsync([self = this] {
    self->removeFromDesktop();
    delete self;
  });
}

// ==========================================================
// ShowInComponent（静态工厂）
//
//   通过 addToDesktop 创建独立原生小窗口（480×340），
//   居中于当前显示器屏幕中心，setAlwaysOnTop(true) 确保置顶。
// ==========================================================
UpdateDialog* UpdateDialog::ShowInComponent(
    juce::Component* parentComponent,
    const y2k::network::UpdateInfo& info,
    juce::PropertiesFile* settings,
    std::function<void()> onClose) {
  // 需要父组件引用以在 VST3 模式下判断是否有可用 UI 上下文。
  // 但弹窗定位不再依赖父窗口位置，而是居中于显示器屏幕。
  if (parentComponent == nullptr) {
    for (int i = 0; i < juce::TopLevelWindow::getNumTopLevelWindows(); ++i) {
      auto* tw = juce::TopLevelWindow::getTopLevelWindow(i);
      if (tw != nullptr && tw->isShowing()) {
        parentComponent = tw;
        break;
      }
    }
  }

  if (parentComponent == nullptr) {
    return nullptr;
  }

  auto* dlg = new UpdateDialog(info, settings, std::move(onClose));

  dlg->addToDesktop(juce::ComponentPeer::windowIsTemporary);
  dlg->setAlwaysOnTop(true);

  // 居中于当前显示器屏幕（userArea 排除任务栏等系统区域）
  const auto& displays = juce::Desktop::getInstance().getDisplays();
  const auto screenArea = displays.getPrimaryDisplay() != nullptr
      ? displays.getPrimaryDisplay()->userArea
      : juce::Rectangle<int>(0, 0, 1280, 720);
  const int x = screenArea.getCentreX() - kDlgW / 2;
  const int y = screenArea.getCentreY() - kDlgH / 2 - 20;
  dlg->setBounds(x, y, kDlgW, kDlgH);
  dlg->setVisible(true);

  return dlg;
}

}  // namespace ui
}  // namespace y2k
