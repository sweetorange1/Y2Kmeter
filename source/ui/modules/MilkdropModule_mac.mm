// ==========================================================
// source/ui/modules/MilkdropModule_mac.mm
//   macOS 专属 ObjC++ 实现：将 overlay NSWindow 绑定为主窗口的子窗口。
//
//   原理：
//     overlay 通过 JUCE addToDesktop 创建的是独立的 NSWindow（层级为
//     NSNormalWindowLevel=0，与主窗口相同）。如果什么都不做，点击主窗口
//     会让主窗口 orderFront 覆盖 overlay。之前的 setAlwaysOnTop(true)
//     把 overlay 提到 NSFloatingWindowLevel=3，虽然解决了覆盖问题，
//     但也导致 overlay 高于所有普通应用，盖住其他软件。
//
//     正确做法：[parentWin addChildWindow:overlayWin ordered:NSWindowAbove]
//     子窗口永悬浮于父窗口上方，但跟随父窗口的 Z-order：
//     - 父窗口在前台时，子窗口在最上（可见）
//     - 父窗口被其他应用盖住时，子窗口也一起被盖（不干扰其他软件）
//
//   为什么必须用 .mm 文件：
//     @interface / NSWindow / addChildWindow 是 Objective-C 语法，
//     .cpp 文件无法编译。必须经由 Objective-C++ (.mm) 编译单元。
// ==========================================================

// 注意：MilkdropModule_mac.h → <JuceHeader.h> → juce_opengl.h 必须
// 在 <AppKit/AppKit.h>（会间接引入 <OpenGL/gl.h>）之前加载，否则触发
// JUCE 的 "gltypes.h included before juce_gl.h" static_assert。
#include "MilkdropModule_mac.h"

// AppKit 伞形头文件间接引入 Carbon/CoreServices（MacTypes.h 中的 Point、
// Components.h 中的 Component 等），这些全局 C 类型与 JUCE 的 juce::Point、
// juce::Component 类名冲突，导致 "reference to 'Point'/'Component' is ambiguous"。
// 在引入 AppKit 前用宏暂存冲突符号，加载完毕后再还原。
// 这对 AppKit 自身无影响——它内部使用 NSView/NSWindow，不依赖 Carbon 的 Point。
#define Point   JUCE_CARBON_Point
#define Component JUCE_CARBON_Component
#import <AppKit/AppKit.h>
#undef Component
#undef Point

namespace y2k {
namespace ui {

void MacAttachOverlayToParent(juce::Component* overlay,
                              juce::Component* parent) {
    if (overlay == nullptr || parent == nullptr)
        return;

    // 1) 获取 overlay 的 NSWindow
    auto* overlayPeer = overlay->getPeer();
    if (overlayPeer == nullptr)
        return;

    NSView* overlayNSView = (__bridge NSView*)overlayPeer->getNativeHandle();
    if (overlayNSView == nil)
        return;

    NSWindow* overlayWin = [overlayNSView window];
    if (overlayWin == nil)
        return;

    // 2) 获取 parent 所在顶层窗口的 NSWindow
    auto* topLevel = parent->getTopLevelComponent();
    if (topLevel == nullptr)
        return;

    auto* parentPeer = topLevel->getPeer();
    if (parentPeer == nullptr)
        return;

    NSView* parentNSView = (__bridge NSView*)parentPeer->getNativeHandle();
    if (parentNSView == nil)
        return;

    NSWindow* parentWin = [parentNSView window];
    if (parentWin == nil)
        return;

    // 3) 避免重复绑定或自己绑自己
    if (overlayWin == parentWin)
        return;

    // 与之前 setAlwaysOnTop(true) 行为对比：
    //   setAlwaysOnTop → NSFloatingWindowLevel (3)，高于所有普通窗口
    //   childWindow     → 子窗口跟随父窗口层级，仅高于父窗口本身
    [parentWin addChildWindow:overlayWin ordered:NSWindowAbove];
}

}  // namespace ui
}  // namespace y2k