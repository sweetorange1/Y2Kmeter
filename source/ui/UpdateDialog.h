// ==========================================================
// source/ui/UpdateDialog.h
//   自定义 PinkXP 风格更新提示弹窗。
//
//   替代 juce::NativeMessageBox::showAsync() 的系统原生对话框，
//   使用与 Y2KMeter 整体 UI 一致的 PinkXP 像素复古风格。
//
//   特性：
//     · 通过 addToDesktop 创建独立原生小窗口（480×340），setAlwaysOnTop
//       确始终置顶；不使用全屏半透明遮罩以避免焦点抢占与边界裁剪
//     · 标题栏区域支持鼠标拖拽移动窗口位置
//     · 两个操作按钮：Download / Remind Me Later
//     · 按钮操作后自删除（removeFromDesktop + MessageManager::callAsync）
//     · 视觉风格对齐软件其他模块窗口（硬阴影 + 像素角标 + PinkXP 标题栏）
// ==========================================================

#pragma once

#include <JuceHeader.h>
#include <functional>

#include "source/network/UpdateChecker.h"

namespace y2k {
namespace ui {

class UpdateDialog : public juce::Component {
public:
  UpdateDialog(const y2k::network::UpdateInfo& info,
               juce::PropertiesFile* settings,
               std::function<void()> onClose = {});

  void paint(juce::Graphics& g) override;
  void resized() override;
  void mouseDown(const juce::MouseEvent& e) override;
  void mouseDrag(const juce::MouseEvent& e) override;
  void mouseMove(const juce::MouseEvent& e) override;
  void mouseUp(const juce::MouseEvent& e) override;

  // 便捷工厂：创建独立原生小窗口（480×340）并居中于父窗口屏幕坐标。
  //   若 parentComponent 为 nullptr，则尝试查找当前活跃的 TopLevelWindow。
  //   返回创建的 UpdateDialog 指针（生命周期自管理，按钮操作后自删除）。
  static UpdateDialog* ShowInComponent(
      juce::Component* parentComponent,
      const y2k::network::UpdateInfo& info,
      juce::PropertiesFile* settings,
      std::function<void()> onClose = {});

private:
  enum class ButtonId { kDownload = 0, kRemind };

  // 对话框面板尺寸
  static constexpr int kDlgW = 480;
  static constexpr int kDlgH = 340;

  // 按钮尺寸（两个按钮水平居中排列）
  static constexpr int kBtnW = 140;
  static constexpr int kBtnH = 26;
  static constexpr int kBtnGap = 12;
  static constexpr int kNumBtns = 2;

  // 标题栏高度
  static constexpr int kTitleBarH = 28;

  y2k::network::UpdateInfo info_;
  juce::PropertiesFile* settings_;
  std::function<void()> onClose_;

  int hoveredBtn_ = -1;
  int pressedBtn_ = -1;

  // 标题栏拖拽
  bool is_dragging_ = false;
  juce::Point<int> drag_offset_;

  juce::Rectangle<int> GetDialogBounds() const;
  juce::Rectangle<int> GetTitleBarBounds() const;
  juce::Rectangle<int> GetButtonBounds(int idx) const;
  int HitTestButton(juce::Point<int> pos) const;
  void ExecuteButton(int idx);

  void CloseDialog();

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(UpdateDialog)
};

}  // namespace ui
}  // namespace y2k
