// ==========================================================
// source/ui/modules/MilkdropModule_mac.h
//   macOS 专属：将 overlay NSWindow 绑定为主窗口的子窗口，
//   使其 Z-order 高于主窗口（覆盖 GL NSOpenGLView），但跟随
//   主窗口相对于其他应用的层级（不会盖住其他软件）。
//
//   为什么需要子窗口？
//   - 独立 NSWindow + setAlwaysOnTop → NSFloatingWindowLevel（层级3）
//     会盖住其他所有普通应用窗口（NSNormalWindowLevel=0）。
//   - 子窗口（addChildWindow:ordered:NSWindowAbove）则永悬浮于
//     父窗口上方，但跟随父窗口的层级——父窗口被盖时子窗口也被盖。
//
//   用法：在 addToDesktop 后调用，仅需一次。
// ==========================================================

#pragma once

#include <JuceHeader.h>

namespace y2k {
namespace ui {

#if JUCE_MAC
/// 将 overlay（已通过 addToDesktop 创建的独立 NSWindow）绑定为
/// parent 所在顶层窗口的 macOS 子窗口。
///
/// 调用时机：overlay 已完成 addToDesktop 但尚未 setVisible 之前。
/// 仅需调用一次；子窗口关系建立后 overlay 自动跟随父窗口的 Z-order。
void MacAttachOverlayToParent(juce::Component* overlay,
                              juce::Component* parent);
#endif

}  // namespace ui
}  // namespace y2k