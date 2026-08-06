/*
  ==============================================================================

  Milkdrop3Module.cpp
  Y2Kmeter — MilkDrop3 独立 D3D9 可视化模块实现。

  ==============================================================================
*/

#include "Milkdrop3Module.h"
#include "Milkdrop3Api.h"
#include "Md3DebugLog.h"
#include "source/ui/PinkXPStyle.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <commctrl.h>

// ==========================================================================
// D3dChildWindow —— 原生 HWND popup 封装
//
// 使用 WS_POPUP，owned by JUCE root HWND：
//   · Owned popup 自动维持 Z 序（永远在 owner 之上），无需手动 SetWindowPos。
//   · 覆盖模块内容区（内容区被 popup 遮挡是有意的：D3D9 直接输出到 popup）。
//   · 模块标题栏不被 popup 覆盖，仍由 JUCE 绘制并接收鼠标事件。
//
// 坐标空间 (PMv2)：JUCE 的 localPointToGlobal 返回逻辑 DIP，
//   Win32 API 期望物理像素。通过
//   juce::Desktop::getInstance().getDisplays().logicalToPhysical() 转换。
// ==========================================================================

class Milkdrop3Module::D3dChildWindow {
 public:
  explicit D3dChildWindow(Milkdrop3Module& owner) : owner_(owner) {}
  ~D3dChildWindow() { Destroy(); }

  bool CreateHWNDOnly(HWND parent_hwnd, int x, int y, int w, int h) {
    static bool class_registered = false;
    if (!class_registered) {
      WNDCLASSEXW wc = {};
      wc.cbSize        = sizeof(wc);
      wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
      wc.lpfnWndProc   = &D3dChildWindow::StaticWndProc;
      wc.cbWndExtra    = sizeof(D3dChildWindow*);
      wc.hInstance     = GetModuleHandleW(nullptr);
      wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
      wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
      wc.lpszClassName = L"Y2Kmeter_MD3_Embed";
      if (!RegisterClassExW(&wc)) return false;
      class_registered = true;
    }

    // 逻辑 DIP → 物理像素（PMv2 下 Win32 API 需要物理坐标）
    const juce::Point<int> logicalGlobalPt =
        owner_.localPointToGlobal(juce::Point<int>(x, y));
    const juce::Rectangle<int> physRect =
        juce::Desktop::getInstance().getDisplays().logicalToPhysical(
            juce::Rectangle<int>(logicalGlobalPt.x, logicalGlobalPt.y, w, h));

    const HWND rootOwner = GetAncestor(parent_hwnd, GA_ROOT);

    hwnd_ = CreateWindowExW(
        0, L"Y2Kmeter_MD3_Embed", L"",
        WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN,
        physRect.getX(), physRect.getY(),
        physRect.getWidth(), physRect.getHeight(),
        rootOwner, nullptr, GetModuleHandleW(nullptr), this);

    if (!hwnd_) {
      MD3_LOG("CreateHWNDOnly: CreateWindowExW failed err=%lu", GetLastError());
    }
    return hwnd_ != nullptr;
  }

  void Destroy() {
    if (hwnd_) {
      DestroyWindow(hwnd_);
      hwnd_ = nullptr;
    }
  }

  void Reposition(int x, int y, int w, int h) {
    if (!hwnd_) return;

    const juce::Point<int> logicalGlobalPt =
        owner_.localPointToGlobal(juce::Point<int>(x, y));
    const juce::Rectangle<int> physRect =
        juce::Desktop::getInstance().getDisplays().logicalToPhysical(
            juce::Rectangle<int>(logicalGlobalPt.x, logicalGlobalPt.y, w, h));

    SetWindowPos(hwnd_, nullptr,
                 physRect.getX(), physRect.getY(),
                 physRect.getWidth(), physRect.getHeight(),
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);

    if (owner_.initialized_) {
      milkdrop3_api::Api::Instance().OnResize(physRect.getWidth(),
                                               physRect.getHeight());
    }
  }

  // 外部窗口模式：以屏幕物理坐标直接定位（不经过 JUCE DPI 转换）
  void RepositionScreen(int physX, int physY, int physW, int physH) {
    if (!hwnd_) return;
    SetWindowPos(hwnd_, nullptr,
                 physX, physY, physW, physH,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
    if (owner_.initialized_) {
      milkdrop3_api::Api::Instance().OnResize(physW, physH);
    }
  }

  HWND GetHwnd() const { return hwnd_; }

 private:
  static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg,
                                        WPARAM wparam, LPARAM lparam) {
    D3dChildWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
      self = static_cast<D3dChildWindow*>(cs->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(self));
    } else {
      self = reinterpret_cast<D3dChildWindow*>(
          GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->WndProc(hwnd, msg, wparam, lparam);
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }

  LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    // 模块内部所有交互仅使用左键；在 popup 内直接处理，不再向 JUCE 转发。
    switch (msg) {
      case WM_MOUSEACTIVATE:
        // 拒绝鼠标激活：D3D9 popup 被点击时不改变 z-order，
        // 否则 Windows 会将该 popup 提升到 ControlBarOverlay 之上导致遮挡。
        return MA_NOACTIVATE;
      case WM_LBUTTONDOWN: {
        // 聚焦模块（不再检查 jump_dialog_open_，因为 SetFocusVisual(false) 已
        // 通过 CloseJumpDialog() 确保状态一致）。
        juce::MessageManager::callAsync([owner_ptr = &owner_] {
          owner_ptr->SetFocusVisual(true);
        });
        return 0;
      }
      case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
      }
      case WM_ERASEBKGND:
        return 1;  // 由 D3D9 Present 负责像素输出
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }

  Milkdrop3Module& owner_;
  HWND             hwnd_ = nullptr;
};

// ==========================================================================
// ControlBarOverlay —— 控件栏 GDI 覆盖层
//
// 悬浮于 D3D9 popup 之上的独立 WS_POPUP，使用 GDI 绘制控件栏按钮与文字。
// 与 D3dChildWindow 同为 root-owned popup，创建在其后以获得更高 z-order。
//
// 聚焦时显示、失焦时隐藏；D3D9 popup 尺寸始终不变，避免设备重置闪烁。
// 跳转弹窗打开时 overlay 向下扩展并内嵌原生 EDIT 控件接收数字输入。
// ==========================================================================

class Milkdrop3Module::ControlBarOverlay {
 public:
  explicit ControlBarOverlay(Milkdrop3Module& owner) : owner_(owner) {}
  ~ControlBarOverlay() { Destroy(); }

  bool Create(HWND rootOwner) {
    static bool class_registered = false;
    if (!class_registered) {
      WNDCLASSEXW wc = {};
      wc.cbSize        = sizeof(wc);
      wc.style         = CS_HREDRAW | CS_VREDRAW;
      wc.lpfnWndProc   = &ControlBarOverlay::StaticWndProc;
      wc.cbWndExtra    = sizeof(ControlBarOverlay*);
      wc.hInstance     = GetModuleHandleW(nullptr);
      wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
      wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
      wc.lpszClassName = L"Y2Kmeter_MD3_Overlay";
      if (!RegisterClassExW(&wc)) return false;
      class_registered = true;
    }

    hwnd_ = CreateWindowExW(
        0, L"Y2Kmeter_MD3_Overlay", L"",
        WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 1, 1,
        rootOwner, nullptr, GetModuleHandleW(nullptr), this);

    if (!hwnd_) return false;
    return true;
  }

  void Destroy() {
    CloseJumpDialog();
    if (hwnd_) {
      DestroyWindow(hwnd_);
      hwnd_ = nullptr;
    }
  }

  void SetVisible(bool visible) {
    if (visible == visible_) return;
    visible_ = visible;

    if (visible_) {
      ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
      // 将 overlay 提升到 D3D9 popup 之上。两者同为 root-owned WS_POPUP，
      // 被 SW_HIDE 隐藏后再次 SW_SHOWNOACTIVATE 时 z-order 可能落后于
      // 此前被点击激活的 D3D9 popup，因此显式置顶。
      SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else {
      CloseJumpDialog();
      ShowWindow(hwnd_, SW_HIDE);
    }
  }

  bool IsVisible() const { return visible_; }

  void Reposition(int x, int y, int w) {
    if (!hwnd_ || !visible_) return;

    if (use_screen_coords_) {
      // 外部窗口模式：x, y, w 已是屏幕物理坐标
      width_ = w;
      SetWindowPos(hwnd_, HWND_TOP,
                   x, y,
                   width_, bar_height_,
                   SWP_NOACTIVATE | SWP_NOCOPYBITS);
    } else {
      // 内嵌模式：x, y, w 来自 JUCE 逻辑坐标，必须转换为物理坐标
      const juce::Point<int> physPt =
          juce::Desktop::getInstance().getDisplays().logicalToPhysical(
              owner_.localPointToGlobal(juce::Point<int>(x, y)));
      const int physW =
          juce::Desktop::getInstance().getDisplays().logicalToPhysical(
              juce::Rectangle<int>(0, 0, w, 1)).getWidth();
      width_ = physW;
      SetWindowPos(hwnd_, nullptr,
                   physPt.x, physPt.y,
                   width_, bar_height_,
                   SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
  }

  // 外部窗口模式：以屏幕物理坐标直接定位（不经过 JUCE DPI 转换）
  void RepositionAtScreenPos(int physX, int physY, int physW) {
    if (!hwnd_ || !visible_) return;
    width_ = physW;
    SetWindowPos(hwnd_, HWND_TOP,
                 physX, physY,
                 width_, bar_height_,
                 SWP_NOACTIVATE | SWP_NOCOPYBITS);
    InvalidateRect(hwnd_, nullptr, FALSE);
  }

  void SetUseScreenCoords(bool use) { use_screen_coords_ = use; }

  void Invalidate() {
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
  }

  void SetPresetDisplay(const std::wstring& text, int idxPartLen) {
    preset_display_ = text;
    preset_idx_part_len_ = idxPartLen;
    Invalidate();
  }

  void SetAutoMode(bool active) {
    if (auto_mode_active_ == active) return;
    auto_mode_active_ = active;
    Invalidate();
  }

  /** 使用固定的黑白灰色调（与软件颜色预设解耦）。在创建 overlay 和主题切换时调用。 */
  void UpdateThemeColors() {
    auto jc2cr = [](juce::Colour c) -> COLORREF {
      return RGB(c.getRed(), c.getGreen(), c.getBlue());
    };

    // 固定黑白灰色调，不跟随软件颜色预设
    theme_bg_        = jc2cr(juce::Colour(0x1E, 0x1E, 0x1E));  // 深灰底色
    theme_bg_darker  = jc2cr(juce::Colour(0x16, 0x16, 0x16));  // 更深底色（弹窗面板）
    theme_pink100    = jc2cr(juce::Colour(0xCC, 0xCC, 0xCC));  // 浅灰（按钮按下/悬停）
    theme_pink200    = jc2cr(juce::Colour(0x99, 0x99, 0x99));  // 中浅灰（按钮悬停）
    theme_pink300    = jc2cr(juce::Colour(0x66, 0x66, 0x66));  // 中灰（分割线/边框/索引文字）
    theme_pink600    = jc2cr(juce::Colour(0x4A, 0x4A, 0x4A));  // 深灰（活跃状态/提示文字）
    theme_btn_normal = jc2cr(juce::Colour(0x33, 0x33, 0x33));  // 按钮正常底色
    theme_btn_face   = jc2cr(juce::Colour(0xDD, 0xDD, 0xDD));  // 按钮面板底色
    theme_ink        = jc2cr(juce::Colour(0xF0, 0xF0, 0xF0));  // 亮色文字

    Invalidate();
  }

  HWND GetHwnd() const { return hwnd_; }

  // ---- 跳转弹窗 ----
  void ShowJumpDialog(int nFiles, int currentPreset) {
    if (jump_dialog_open_) return;
    jump_dialog_open_ = true;

    // 扩展 overlay 高度
    bar_height_ = kControlBarHeight + kJumpDlgHeight;
    if (visible_) {
      // 重新定位以适配新高度
      if (use_screen_coords_) {
        // 外部窗口模式：保持 x 不变，仅更新高度
        RECT rc; GetWindowRect(hwnd_, &rc);
        width_ = rc.right - rc.left;
        SetWindowPos(hwnd_, nullptr,
                     rc.left, rc.top,
                     width_, bar_height_,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
        InvalidateRect(hwnd_, nullptr, FALSE);
      } else {
        const auto content = owner_.getContentBounds();
        Reposition(content.getX(), content.getY(), content.getWidth());
      }
    }

    // 创建原生 EDIT 控件作为 overlay 的子窗口。
    // 宽度 80px 仅容纳少数数字位，避免占满弹窗宽度压缩其他元素。
    edit_hwnd_ = CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER | ES_RIGHT,
        14, kControlBarHeight + 50,
        80, 22,
        hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (edit_hwnd_) {
      // 子类化 EDIT 以拦截 Enter/Esc
      SetWindowSubclass(edit_hwnd_, EditSubclassProc, 0,
                        reinterpret_cast<DWORD_PTR>(this));
      SetWindowTextW(edit_hwnd_,
                     std::to_wstring(currentPreset + 1).c_str());
      SendMessageW(edit_hwnd_, EM_SETSEL, 0, -1);
      SetFocus(edit_hwnd_);

      // 设置 EDIT 字体（使用系统默认 GUI 字体，与控件栏风格统一）
      NONCLIENTMETRICSW ncm = { sizeof(ncm) };
      if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS,
                                sizeof(ncm), &ncm, 0)) {
        HFONT editFont = CreateFontIndirectW(&ncm.lfMessageFont);
        if (editFont)
          SendMessageW(edit_hwnd_, WM_SETFONT,
                       reinterpret_cast<WPARAM>(editFont), TRUE);
      }
    }
  }

  void CloseJumpDialog() {
    if (!jump_dialog_open_) return;
    jump_dialog_open_ = false;

    if (edit_hwnd_) {
      DestroyWindow(edit_hwnd_);
      edit_hwnd_ = nullptr;
    }

    // 收缩 overlay 高度
    bar_height_ = kControlBarHeight;
    if (visible_) {
      if (use_screen_coords_) {
        RECT rc; GetWindowRect(hwnd_, &rc);
        width_ = rc.right - rc.left;
        SetWindowPos(hwnd_, nullptr,
                     rc.left, rc.top,
                     width_, bar_height_,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
        InvalidateRect(hwnd_, nullptr, FALSE);
      } else {
        const auto content = owner_.getContentBounds();
        Reposition(content.getX(), content.getY(), content.getWidth());
      }
    }
  }

  bool IsJumpDialogOpen() const { return jump_dialog_open_; }

 private:
  // ---- 窗口过程 ----
  static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg,
                                        WPARAM wparam, LPARAM lparam) {
    ControlBarOverlay* self = nullptr;
    if (msg == WM_NCCREATE) {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
      self = static_cast<ControlBarOverlay*>(cs->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(self));
    } else {
      self = reinterpret_cast<ControlBarOverlay*>(
          GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->WndProc(hwnd, msg, wparam, lparam);
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }

  LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
      case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        OnPaint(hdc);
        EndPaint(hwnd, &ps);
        return 0;
      }
      case WM_ERASEBKGND:
        return 1;  // 在 OnPaint 中自行擦除背景

      case WM_LBUTTONDOWN: {
        const int px = LOWORD(lparam);
        const int py = HIWORD(lparam);

        // 跳转弹窗 Go/Cancel 按钮优先处理——不与控件栏按钮共享 HitTest/状态
        if (jump_dialog_open_) {
          RECT goRc, cancelRc;
          GetJumpDialogButtonRects(goRc, cancelRc);
          if (PtInRect(&goRc, {px, py})) {
            dialog_btn_pressed_ = DialogBtn::kGo;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
          }
          if (PtInRect(&cancelRc, {px, py})) {
            dialog_btn_pressed_ = DialogBtn::kCancel;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
          }
          // 点击弹窗内其他区域 → 关闭弹窗
          if (py >= kControlBarHeight &&
              py < kControlBarHeight + kJumpDlgHeight) {
            juce::MessageManager::callAsync(
                [this] { owner_.CloseJumpDialog(); });
            return 0;
          }
        }

        const OverlayButton hit = HitTest(px, py);
        if (hit != OverlayButton::kNone) {
          pressed_btn_ = hit;
          InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
      }
      case WM_LBUTTONUP: {
        const int px = LOWORD(lparam);
        const int py = HIWORD(lparam);

        // 弹窗按钮释放
        if (dialog_btn_pressed_ != DialogBtn::kNone) {
          RECT goRc, cancelRc;
          GetJumpDialogButtonRects(goRc, cancelRc);
          if (dialog_btn_pressed_ == DialogBtn::kGo &&
              PtInRect(&goRc, {px, py})) {
            DoPresetJump();
          } else if (dialog_btn_pressed_ == DialogBtn::kCancel &&
                     PtInRect(&cancelRc, {px, py})) {
            juce::MessageManager::callAsync(
                [this] { owner_.CloseJumpDialog(); });
          }
          dialog_btn_pressed_ = DialogBtn::kNone;
          InvalidateRect(hwnd_, nullptr, FALSE);
          return 0;
        }

        if (pressed_btn_ != OverlayButton::kNone) {
          const OverlayButton hit = HitTest(px, py);
          if (hit == pressed_btn_)
            ExecuteAction(hit);
          pressed_btn_ = OverlayButton::kNone;
          InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
      }
      case WM_MOUSEMOVE: {
        const int px = LOWORD(lparam);
        const int py = HIWORD(lparam);

        // 弹窗区域内不更新控件栏按钮 hover 状态
        if (jump_dialog_open_ && py >= kControlBarHeight) {
          if (hovered_btn_ != OverlayButton::kNone) {
            hovered_btn_ = OverlayButton::kNone;
            InvalidateRect(hwnd_, nullptr, FALSE);
          }
          return 0;
        }

        const OverlayButton hit = HitTest(px, py);
        if (hit != hovered_btn_) {
          hovered_btn_ = hit;
          InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
      }
      case WM_MOUSELEAVE: {
        if (hovered_btn_ != OverlayButton::kNone) {
          hovered_btn_ = OverlayButton::kNone;
          InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
      }
      case WM_SETCURSOR: {
        if (LOWORD(lparam) == HTCLIENT) {
          POINT pt;
          GetCursorPos(&pt);
          ScreenToClient(hwnd, &pt);
          if (HitTest(pt.x, pt.y) != OverlayButton::kNone)
            SetCursor(LoadCursor(nullptr, IDC_HAND));
          else
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
          return TRUE;
        }
        break;
      }

      // 跳转弹窗内的 Go/Cancel 按钮
      case WM_COMMAND: {
        if (HIWORD(wparam) == BN_CLICKED) {
          const int px = LOWORD(lparam);  // control id approximation
          // Go 和 Cancel 在 OnPaint 中以文字绘制，不是真实 Button 控件，
          // 此路径留空——点击由 WM_LBUTTONUP 中的 HitTest 处理。
        }
        break;
      }
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }

  // ---- EDIT 子类化（拦截 Enter / Esc）----
  static LRESULT CALLBACK EditSubclassProc(HWND hwnd, UINT msg,
                                            WPARAM wparam, LPARAM lparam,
                                            UINT_PTR /*uIdSubclass*/,
                                            DWORD_PTR dwRefData) {
    auto* self = reinterpret_cast<ControlBarOverlay*>(dwRefData);
    switch (msg) {
      case WM_KEYDOWN:
        if (wparam == VK_RETURN) {
          self->DoPresetJump();
          return 0;
        }
        if (wparam == VK_ESCAPE) {
          // 关闭弹窗：通过 JUCE message thread 确保线程安全
          juce::MessageManager::callAsync([s = self] { s->owner_.CloseJumpDialog(); });
          return 0;
        }
        break;
      case WM_DESTROY: {
        RemoveWindowSubclass(hwnd, EditSubclassProc, 0);
        break;
      }
    }
    return DefSubclassProc(hwnd, msg, wparam, lparam);
  }

  void DoPresetJump() {
    if (!edit_hwnd_) return;
    wchar_t buf[16] = {};
    GetWindowTextW(edit_hwnd_, buf, 15);
    int val = _wtoi(buf);
    if (val < 1) val = 1;
    int nFiles = owner_.api_.GetFileablePresetCount();
    if (nFiles > 0 && val > nFiles) val = nFiles;
    if (nFiles > 0) {
      // 文件级索引（0-based）：JumpToPreset 内部映射到数组下标
      owner_.api_.JumpToPreset(val - 1);

      // 异步加载时 m_szCurrentPresetFile 尚未更新，
      // 不在此处调用 AnnouncePresetNameToEngine()，避免读取旧文件名。
      // timerCallback 中的 SyncOverlayContent 会在异步加载完成后自动刷新。
    }
    // 通过 owner 关闭弹窗（走 Milkdrop3Module::CloseJumpDialog → overlay.CloseJumpDialog）
    juce::MessageManager::callAsync([owner_ptr = &owner_] {
      owner_ptr->CloseJumpDialog();
    });
  }

  // ---- 绘制 ----
  void OnPaint(HDC hdc) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;

    // 双缓冲：先画到 memDC，再一次 BitBlt 到屏幕
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
    HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(memDC, memBmp));

    // ---- 背景 ----
    HBRUSH bgBrush = CreateSolidBrush(theme_bg_);
    FillRect(memDC, &rc, bgBrush);
    DeleteObject(bgBrush);

    // ---- 底部分割线 ----
    HPEN dividerPen = CreatePen(PS_SOLID, 1, theme_pink300);
    HPEN oldPen = static_cast<HPEN>(SelectObject(memDC, dividerPen));
    MoveToEx(memDC, rc.left, kControlBarHeight - 1, nullptr);
    LineTo(memDC, rc.right, kControlBarHeight - 1);
    SelectObject(memDC, oldPen);
    DeleteObject(dividerPen);

    SetBkMode(memDC, TRANSPARENT);

    // 创建字体（如果尚未创建）
    if (!font_bold_) {
      font_bold_ = CreateFontW(-11, 0, 0, 0, FW_BOLD,
                                FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                L"Tahoma");
    }
    if (!font_plain_) {
      font_plain_ = CreateFontW(-11, 0, 0, 0, FW_NORMAL,
                                 FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                 L"Tahoma");
    }

    // ---- 绘制按钮 ----
    DrawButton(memDC, OverlayButton::kPrev, L"<");
    DrawButton(memDC, OverlayButton::kAuto,
               auto_mode_active_ ? L"A" : L"a");
    DrawButton(memDC, OverlayButton::kNext, L">");
    DrawButton(memDC, OverlayButton::kRandom, L"?");

    // ---- 预设名显示 ----
    if (!preset_display_.empty()) {
      RECT nameRc = GetButtonRect(OverlayButton::kPresetName);
      const bool nameHovered = (hovered_btn_ == OverlayButton::kPresetName);
      const bool namePressed = (pressed_btn_ == OverlayButton::kPresetName);

      if (namePressed) {
        HBRUSH hb = CreateSolidBrush(theme_pink100);
        FillRect(memDC, &nameRc, hb);
        DeleteObject(hb);
      } else if (nameHovered) {
        RECT underline = { nameRc.left, nameRc.bottom - 1,
                           nameRc.right, nameRc.bottom };
        HBRUSH hb = CreateSolidBrush(theme_pink300);
        FillRect(memDC, &underline, hb);
        DeleteObject(hb);
      }

      if (preset_idx_part_len_ > 0) {
        RECT idxRc = nameRc;
        idxRc.right = nameRc.left + preset_idx_part_len_ * 7 + 4;
        HFONT oldF = static_cast<HFONT>(SelectObject(memDC, font_bold_));
        SetTextColor(memDC, theme_pink300);
        std::wstring idxPart = preset_display_.substr(0, preset_idx_part_len_);
        DrawTextW(memDC, idxPart.c_str(), static_cast<int>(idxPart.length()),
                  &idxRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(memDC, oldF);

        nameRc.left = idxRc.right;
      }

      HFONT oldF = static_cast<HFONT>(SelectObject(memDC, font_plain_));
      SetTextColor(memDC, RGB(0xEE, 0xEE, 0xEE));
      std::wstring namePart = preset_display_.substr(preset_idx_part_len_);
      DrawTextW(memDC, namePart.c_str(), static_cast<int>(namePart.length()),
                &nameRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
      SelectObject(memDC, oldF);
    }

    // ---- 跳转弹窗面板 ----
    if (jump_dialog_open_)
      PaintJumpDialog(memDC, rc);

    // 双缓冲提交
    BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
    SelectObject(memDC, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDC);
  }

  void DrawButton(HDC hdc, OverlayButton btn, const wchar_t* text) {
    RECT r = GetButtonRect(btn);
    const bool hovered = (hovered_btn_ == btn);
    const bool pressed = (pressed_btn_ == btn);
    const bool active  = (btn == OverlayButton::kAuto && auto_mode_active_);

    COLORREF fillColor;
    if (pressed) {
      fillColor = theme_pink100;
    } else if (hovered || active) {
      fillColor = active ? theme_pink600 : theme_pink200;
    } else {
      fillColor = theme_btn_normal;
    }

    HBRUSH hb = CreateSolidBrush(fillColor);
    FillRect(hdc, &r, hb);
    DeleteObject(hb);

    if (!pressed && !hovered && !active) {
      HPEN borderPen = CreatePen(PS_SOLID, 1, theme_pink300);
      HPEN oldP = static_cast<HPEN>(SelectObject(hdc, borderPen));
      HBRUSH nullBr = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
      HBRUSH oldBr = static_cast<HBRUSH>(SelectObject(hdc, nullBr));
      ::Rectangle(hdc, r.left, r.top, r.right, r.bottom);
      SelectObject(hdc, oldP);
      SelectObject(hdc, oldBr);
      DeleteObject(borderPen);
    }

    HFONT oldF = static_cast<HFONT>(SelectObject(hdc, font_bold_));
    SetTextColor(hdc, RGB(0xEE, 0xEE, 0xEE));
    DrawTextW(hdc, text, -1, &r,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, oldF);
  }

  void PaintJumpDialog(HDC hdc, RECT clientRc) {
    const int dlgY = kControlBarHeight;
    const int dlgW = clientRc.right - clientRc.left;

    RECT dlgRc = { clientRc.left, dlgY,
                   clientRc.right, dlgY + kJumpDlgHeight };
    HBRUSH dlgBg = CreateSolidBrush(theme_bg_darker);
    FillRect(hdc, &dlgRc, dlgBg);
    DeleteObject(dlgBg);

    // ---- 标题 ----
    RECT titleRc = { clientRc.left + 12, dlgY + 8,
                     clientRc.right - 12, dlgY + 28 };
    HFONT oldF = static_cast<HFONT>(SelectObject(hdc, font_bold_));
    SetTextColor(hdc, theme_ink);
    DrawTextW(hdc, L"Jump to Preset", -1, &titleRc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, oldF);

    // ---- 提示文字 ----
    int nFiles = owner_.api_.GetFileablePresetCount();
    wchar_t hint[64];
    swprintf_s(hint, L"Enter preset number (1-%d):", nFiles);
    RECT hintRc = { clientRc.left + 12, dlgY + 30,
                    clientRc.right - 12, dlgY + 48 };
    SetTextColor(hdc, theme_pink200);
    oldF = static_cast<HFONT>(SelectObject(hdc, font_plain_));
    DrawTextW(hdc, hint, -1, &hintRc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, oldF);

    // ---- Go 按钮（右侧）----
    // 按钮填充使用深色底 + 白色文字，确保高对比度。控件栏按钮（DrawButton）也使用
    // theme_btn_normal (0x33,0x33,0x33) 深灰底 + 白色文字，弹窗按钮应保持一致。
    RECT goRc = { clientRc.right - 14 - 54, dlgY + 50,
                  clientRc.right - 14, dlgY + 72 };
    const bool goPressed = (dialog_btn_pressed_ == DialogBtn::kGo);
    COLORREF goColor = goPressed ? theme_pink100 : theme_btn_normal;
    HBRUSH goBr = CreateSolidBrush(goColor);
    FillRect(hdc, &goRc, goBr);
    DeleteObject(goBr);
    // 非按下态绘制 1px 边框
    if (!goPressed) {
      HPEN goPen = CreatePen(PS_SOLID, 1, theme_pink300);
      HPEN oldPn = static_cast<HPEN>(SelectObject(hdc, goPen));
      HBRUSH nullBr = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
      HBRUSH oldBr2 = static_cast<HBRUSH>(SelectObject(hdc, nullBr));
      ::Rectangle(hdc, goRc.left, goRc.top, goRc.right, goRc.bottom);
      SelectObject(hdc, oldPn);
      SelectObject(hdc, oldBr2);
      DeleteObject(goPen);
    }
    SetTextColor(hdc, RGB(0xF0, 0xF0, 0xF0));
    oldF = static_cast<HFONT>(SelectObject(hdc, font_bold_));
    DrawTextW(hdc, L"Go", -1, &goRc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, oldF);

    // ---- Cancel 按钮 ----
    RECT cancelRc = { goRc.left - 8 - 56, dlgY + 50,
                       goRc.left - 8, dlgY + 72 };
    const bool cnPressed = (dialog_btn_pressed_ == DialogBtn::kCancel);
    COLORREF cnColor = cnPressed ? theme_pink100 : theme_btn_normal;
    HBRUSH cnBr = CreateSolidBrush(cnColor);
    FillRect(hdc, &cancelRc, cnBr);
    DeleteObject(cnBr);
    if (!cnPressed) {
      HPEN cnPen = CreatePen(PS_SOLID, 1, theme_pink300);
      HPEN oldPnCn = static_cast<HPEN>(SelectObject(hdc, cnPen));
      HBRUSH nullBrCn = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
      HBRUSH oldBrCn = static_cast<HBRUSH>(SelectObject(hdc, nullBrCn));
      ::Rectangle(hdc, cancelRc.left, cancelRc.top, cancelRc.right, cancelRc.bottom);
      SelectObject(hdc, oldPnCn);
      SelectObject(hdc, oldBrCn);
      DeleteObject(cnPen);
    }
    SetTextColor(hdc, RGB(0xF0, 0xF0, 0xF0));
    oldF = static_cast<HFONT>(SelectObject(hdc, font_bold_));
    DrawTextW(hdc, L"Cancel", -1, &cancelRc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, oldF);
  }

  // ---- 命中测试 ----
  OverlayButton HitTest(int px, int py) const {
    for (OverlayButton btn : { OverlayButton::kPrev, OverlayButton::kPresetName,
                               OverlayButton::kAuto, OverlayButton::kNext,
                               OverlayButton::kRandom }) {
      RECT r = GetButtonRect(btn);
      if (px >= r.left && px < r.right && py >= r.top && py < r.bottom)
        return btn;
    }
    // 跳转弹窗区域内的 Go/Cancel 不映射到控件栏按钮——由 WndProc 独立处理点击，
    // 避免与 [<]/[>] 按钮共享 hover/press 状态导致动画错乱和逻辑误触发。
    return OverlayButton::kNone;
  }

  // 获取跳转弹窗 Go/Cancel 按钮矩形
  void GetJumpDialogButtonRects(RECT& goRc, RECT& cancelRc) const {
    int tmpW = width_;
    goRc = { tmpW - 14 - 54, kControlBarHeight + 50,
             tmpW - 14, kControlBarHeight + 72 };
    cancelRc = { goRc.left - 8 - 56, kControlBarHeight + 50,
                 goRc.left - 8, kControlBarHeight + 72 };
  }

  RECT GetButtonRect(OverlayButton btn) const {
    const int y = (kControlBarHeight - kBtnSize) / 2;

    auto rightBtn = [&](int idxFromRight) -> RECT {
      int x = width_ - kPadding - kBtnSize
              - idxFromRight * (kBtnSize + kPadding);
      return { x, y, x + kBtnSize, y + kBtnSize };
    };

    RECT prevRc    = { kPadding, y, kPadding + kBtnSize, y + kBtnSize };
    RECT randomRc  = rightBtn(0);  // [?] 最右
    RECT nextRc    = rightBtn(1);  // [>]
    RECT autoRc    = { nextRc.left - kPadding - kAutoBtnW,
                       y, nextRc.left - kPadding, y + kBtnSize };
    RECT nameRc    = { prevRc.right + kPadding, y,
                       autoRc.left - kPadding, y + kBtnSize };

    switch (btn) {
      case OverlayButton::kPrev:       return prevRc;
      case OverlayButton::kAuto:       return autoRc;
      case OverlayButton::kNext:       return nextRc;
      case OverlayButton::kRandom:     return randomRc;
      case OverlayButton::kPresetName: return nameRc;
      default:                         return {};
    }
  }

  void ExecuteAction(OverlayButton btn) {
    switch (btn) {
      case OverlayButton::kPrev:       owner_.PrevPreset();       break;
      case OverlayButton::kNext:       owner_.NextPreset();       break;
      case OverlayButton::kRandom:     owner_.RandomPreset();     break;
      case OverlayButton::kAuto:       owner_.ToggleAutoMode();   break;
      case OverlayButton::kPresetName: owner_.ShowPresetJumpDialog(); break;
      default: break;
    }
  }

  // ---- 颜色常量（固定黑白灰 GDI 映射，与软件颜色预设解耦）----
  //   注：这些 static constexpr 是默认值。UpdateThemeColors() 使用相同的固定值覆盖，
  //   确保控件栏始终使用通用黑白灰色调，不随软件主题变化。
  static constexpr COLORREF kBgColor      = RGB(0x1E, 0x1E, 0x1E);
  static constexpr COLORREF kBgDarker     = RGB(0x16, 0x16, 0x16);
  static constexpr COLORREF kPink100      = RGB(0xCC, 0xCC, 0xCC);
  static constexpr COLORREF kPink200      = RGB(0x99, 0x99, 0x99);
  static constexpr COLORREF kPink300      = RGB(0x66, 0x66, 0x66);
  static constexpr COLORREF kPink600      = RGB(0x4A, 0x4A, 0x4A);
  static constexpr COLORREF kPink600Text  = RGB(0x4A, 0x4A, 0x4A);
  static constexpr COLORREF kBtnNormal    = RGB(0x33, 0x33, 0x33);
  static constexpr COLORREF kBtnFace      = RGB(0xDD, 0xDD, 0xDD);
  static constexpr COLORREF kTextColor    = RGB(0xF0, 0xF0, 0xF0);
  static constexpr COLORREF kInkColor     = RGB(0xF0, 0xF0, 0xF0);

  // ---- 运行时主题色（由 UpdateThemeColors() 从 PinkXP 动态读取）----
  COLORREF theme_bg_       = kBgColor;
  COLORREF theme_bg_darker = kBgDarker;
  COLORREF theme_pink100   = kPink100;
  COLORREF theme_pink200   = kPink200;
  COLORREF theme_pink300   = kPink300;
  COLORREF theme_pink600   = kPink600;
  COLORREF theme_btn_normal = kBtnNormal;
  COLORREF theme_btn_face  = kBtnFace;
  COLORREF theme_ink       = kInkColor;

  static constexpr int kBtnSize     = 22;
  static constexpr int kPadding     = 4;
  static constexpr int kAutoBtnW    = 28;
  static constexpr int kControlBarHeight = 26;
  static constexpr int kJumpDlgHeight    = 84;

  // ---- 成员 ----
  Milkdrop3Module& owner_;
  HWND hwnd_ = nullptr;
  HWND edit_hwnd_ = nullptr;

  bool visible_ = false;
  bool jump_dialog_open_ = false;
  bool use_screen_coords_ = false;  ///< 外部窗口模式时 overlay x/y 已是物理像素

  OverlayButton hovered_btn_ = OverlayButton::kNone;
  OverlayButton pressed_btn_ = OverlayButton::kNone;

  // 弹窗按钮独立状态（不与控件栏 OverlayButton 共享，避免动画/逻辑混淆）
  enum class DialogBtn { kNone, kGo, kCancel };
  DialogBtn dialog_btn_pressed_ = DialogBtn::kNone;

  int width_      = 0;
  int bar_height_ = kControlBarHeight;

  std::wstring preset_display_;
  int preset_idx_part_len_ = 0;
  bool auto_mode_active_ = false;

  HFONT font_bold_  = nullptr;
  HFONT font_plain_ = nullptr;
};

// ==========================================================================
// NativeExternalWindow —— 独立外部窗口（自 v2.4.0）
//
// WS_POPUP | WS_EX_TOOLWINDOW 顶层窗口，owned by 主窗口 root HWND，
// 不生成任务栏图标。GDI 自绘标题栏(26px) + 2px 边框，
// 包裹 D3D9 popup 和 ControlBarOverlay 提供完整窗口 chrome。
//
// D3D9 popup 与 overlay 保持原有 root-owned 关系不变（避免 D3D9 device
// recreate），仅通过屏幕物理坐标与外部窗口对齐。外部窗口通过
// WM_NCHITTEST→HTCAPTION 支持系统级拖拽，可自由移动到任意显示器。
//
// 最小化同步由 Milkdrop3Module::timerCallback 轮询 IsIconic 实现。
// 关闭(×)按钮触发模块关闭回调。
// ==========================================================================

class Milkdrop3Module::NativeExternalWindow {
 public:
  explicit NativeExternalWindow(Milkdrop3Module& owner) : owner_(owner) {}
  ~NativeExternalWindow() { Destroy(); }

  using CloseCallback = std::function<void()>;

  bool Create(HWND rootOwner, int physX, int physY, int physW, int physH) {
    static bool s_class_registered = false;
    if (!s_class_registered) {
      WNDCLASSEXW wc = {};
      wc.cbSize        = sizeof(wc);
      wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
      wc.lpfnWndProc   = &NativeExternalWindow::StaticWndProc;
      wc.cbWndExtra    = sizeof(NativeExternalWindow*);
      wc.hInstance     = GetModuleHandleW(nullptr);
      wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
      wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
      wc.lpszClassName = L"Y2Kmeter_MD3_ExtWin";
      if (!RegisterClassExW(&wc)) return false;
      s_class_registered = true;
    }

    root_owner_ = rootOwner;
    content_w_ = physW;
    content_h_ = physH;

    const int totalW = physW + kBorderW * 2;
    const int totalH = physH + kTitleBarH + kBorderW;

    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW, L"Y2Kmeter_MD3_ExtWin", L"",
        WS_POPUP | WS_CLIPCHILDREN,
        physX - kBorderW, physY - kTitleBarH,
        totalW, totalH,
        rootOwner, nullptr, GetModuleHandleW(nullptr), this);

    if (!hwnd_) return false;

    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    return true;
  }

  void Destroy() {
    if (hwnd_) {
      DestroyWindow(hwnd_);
      hwnd_ = nullptr;
    }
  }

  void SetCloseCallback(CloseCallback cb) { on_close_ = std::move(cb); }

  HWND GetHwnd() const { return hwnd_; }
  int GetContentX() const {
    RECT rc; GetWindowRect(hwnd_, &rc); return rc.left + kBorderW;
  }
  int GetContentY() const {
    RECT rc; GetWindowRect(hwnd_, &rc); return rc.top + kTitleBarH;
  }
  int GetContentW() const { return content_w_; }
  int GetContentH() const { return content_h_; }

  void SetVisible(bool visible) {
    if (!hwnd_) return;
    ShowWindow(hwnd_, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
  }

 private:
  static constexpr int kTitleBarH = 26;
  static constexpr int kBorderW   = 2;

  static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg,
                                        WPARAM wparam, LPARAM lparam) {
    NativeExternalWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
      self = static_cast<NativeExternalWindow*>(cs->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(self));
    } else {
      self = reinterpret_cast<NativeExternalWindow*>(
          GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->WndProc(hwnd, msg, wparam, lparam);
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }

  LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
      case WM_NCHITTEST: {
        // 标题栏区域 → 系统拖拽
        POINT pt = { static_cast<int>(static_cast<short>(LOWORD(lparam))),
                     static_cast<int>(static_cast<short>(HIWORD(lparam))) };
        ScreenToClient(hwnd, &pt);
        if (pt.y >= 0 && pt.y < kTitleBarH) {
          RECT crc; GetClientRect(hwnd, &crc);
          // 关闭按钮区域(右侧 28px)保留给 WM_LBUTTONDOWN 处理
          if (pt.x >= crc.right - 28)
            return HTCLIENT;
          return HTCAPTION;
        }
        return HTCLIENT;
      }

      case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        OnPaint(hdc);
        EndPaint(hwnd, &ps);
        return 0;
      }

      case WM_ERASEBKGND:
        return 1;

      case WM_CLOSE:
        if (on_close_) on_close_();
        return 0;

      case WM_LBUTTONDOWN: {
        POINT pt = { static_cast<int>(static_cast<short>(LOWORD(lparam))),
                     static_cast<int>(static_cast<short>(HIWORD(lparam))) };
        if (pt.y < kTitleBarH) {
          RECT crc; GetClientRect(hwnd, &crc);
          if (pt.x >= crc.right - 28) {
            if (on_close_) on_close_();
            return 0;
          }
        }
        break;
      }

      case WM_WINDOWPOSCHANGED:
        // 拖拽后通知 owner 同步 D3D9/overlay 位置
        juce::MessageManager::callAsync(
            [owner_ptr = &owner_] { owner_ptr->SyncExternalWindowChildren(); });
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }

  void OnPaint(HDC hdc) {
    RECT rc; GetClientRect(hwnd_, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;

    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
    HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(memDC, memBmp));

    // 内容区底色（D3D9 popup 覆盖，实际由 D3D9 Present 填充）
    HBRUSH contentBg = CreateSolidBrush(RGB(0x00, 0x00, 0x00));
    RECT contentRc = { kBorderW, kTitleBarH, w - kBorderW, h - kBorderW };
    FillRect(memDC, &contentRc, contentBg);
    DeleteObject(contentBg);

    // ---- 标题栏 ----
    RECT titleRc = { kBorderW, 0, w - kBorderW, kTitleBarH };
    HBRUSH titleBg = CreateSolidBrush(RGB(0x2A, 0x2A, 0x2A));
    FillRect(memDC, &titleRc, titleBg);
    DeleteObject(titleBg);

    SetBkMode(memDC, TRANSPARENT);

    // 标题文字
    SetTextColor(memDC, RGB(0xF0, 0xF0, 0xF0));
    HFONT titleFont = CreateFontW(-12, 0, 0, 0, FW_BOLD,
                                   FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                   L"Tahoma");
    HFONT oldF = static_cast<HFONT>(SelectObject(memDC, titleFont));
    RECT textRc = { kBorderW + 6, 0, w - kBorderW - 32, kTitleBarH };
    DrawTextW(memDC, L"MilkDrop3", -1, &textRc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(memDC, oldF);
    DeleteObject(titleFont);

    // 关闭按钮 (×)
    RECT closeRc = { w - kBorderW - 28, 0, w - kBorderW, kTitleBarH };
    SetTextColor(memDC, RGB(0xCC, 0xCC, 0xCC));
    HFONT closeFont = CreateFontW(-14, 0, 0, 0, FW_BOLD,
                                   FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                   L"Tahoma");
    oldF = static_cast<HFONT>(SelectObject(memDC, closeFont));
    DrawTextW(memDC, L"\u00D7", -1, &closeRc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(memDC, oldF);
    DeleteObject(closeFont);

    // ---- 边框 (2px) ----
    HPEN borderPen = CreatePen(PS_SOLID, kBorderW, RGB(0x4A, 0x4A, 0x4A));
    HPEN oldPen = static_cast<HPEN>(SelectObject(memDC, borderPen));
    HBRUSH nullBr = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    HBRUSH oldBr = static_cast<HBRUSH>(SelectObject(memDC, nullBr));
    ::Rectangle(memDC, 0, 0, w, h);
    SelectObject(memDC, oldPen);
    SelectObject(memDC, oldBr);
    DeleteObject(borderPen);

    // 标题栏下沿分割线
    HPEN divPen = CreatePen(PS_SOLID, 1, RGB(0x66, 0x66, 0x66));
    oldPen = static_cast<HPEN>(SelectObject(memDC, divPen));
    MoveToEx(memDC, kBorderW, kTitleBarH, nullptr);
    LineTo(memDC, w - kBorderW, kTitleBarH);
    SelectObject(memDC, oldPen);
    DeleteObject(divPen);

    BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
    SelectObject(memDC, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDC);
  }

  Milkdrop3Module& owner_;
  HWND hwnd_      = nullptr;
  HWND root_owner_ = nullptr;
  int content_w_   = 0;
  int content_h_   = 0;
  CloseCallback on_close_;
};

// ==========================================================================
// Milkdrop3Module
// ==========================================================================

Milkdrop3Module::Milkdrop3Module(AnalyserHub* hub)
    : ModulePanel(ModuleType::milkdrop3),
      api_(milkdrop3_api::Api::Instance()),
      hub_(hub) {
  setDefaultSize(640, 480);
  setMinSize(160, 120);
  setTitleText("MilkDrop3");

  if (hub_) {
    hub_->retain(AnalyserHub::Kind::Oscilloscope);
    hub_->retain(AnalyserHub::Kind::Spectrum);
    hub_->addFrameListener(this);
  }

  // 预设跳转 TextEditor 采用懒创建策略（unique_ptr），在首次打开弹窗时
  // 才 new + addAndMakeVisible。避免在 loadInitialModules → createEditor 阶段
  // （OS 窗口 peer 未就绪）构造重型 JUCE 组件（内部含 Viewport + TextHolderComponent）
  // 触发图形上下文死锁。
}

Milkdrop3Module::~Milkdrop3Module() {
  // 0) 销毁外部窗口（若存在），释放原生资源
  external_window_.reset();

  // 1) 反注册 pre-render injector，防止 Api 在销毁过程中回调本对象。
  if (audio_injector_token_ != 0) {
    api_.RemovePreRenderInjector(audio_injector_token_);
    audio_injector_token_ = 0;
  }

  // 2) 停止 Timer / 摘 FrameListener，避免竞态。
  stopTimer();
  if (hub_) hub_->removeFrameListener(this);

  // 3) 销毁 D3D9 popup + 引擎。
  d3d_window_.reset();
  if (initialized_) api_.Destroy();

  // 4) 释放 hub Kind 引用计数。
  if (hub_) {
    hub_->release(AnalyserHub::Kind::Spectrum);
    hub_->release(AnalyserHub::Kind::Oscilloscope);
  }
}

// ==========================================================================
// ModulePanel 覆写
//
// paint() 在加载/错误时绘进度条/黑底；已初始化时自绘标题栏+边框，
// 不委托 ModulePanel::paint()。原因：
//   · D3D9 WS_POPUP 子窗口已覆盖整个内容区，ModulePanel 的 drawSunken
//     白色填充会在边框缝隙处露出灰色占位条，与黑色视频区视觉割裂。
//   · 参考同项目 MilkdropModule（projectM WebGL 模块）做法：drawRaised
//     使用透明填充，仅绘制 3D 边框+标题栏，让 D3D9 黑色底自然透出。
// ==========================================================================

void Milkdrop3Module::paint(juce::Graphics& g) {
  if (!initialized_ && !error_state_) {
    // 加载中：只画黑底 + 橙色进度条，完全不触碰 drawText
    const auto bounds = getLocalBounds();
    g.fillAll(juce::Colours::black);

    const int barW = juce::jmin(300, bounds.getWidth() - 40);
    const int barH = 6;
    const int barX = bounds.getCentreX() - barW / 2;
    const int barY = bounds.getCentreY() - barH / 2;
    g.setColour(juce::Colour(0xFF333333));
    g.fillRect(barX, barY, barW, barH);
    if (init_phase_ >= -1) {
      const int effective_phase = juce::jmax(0, init_phase_);
      const float progress = juce::jlimit(
          0.0f, 1.0f, static_cast<float>(effective_phase + 1) / 6.0f);
      g.setColour(juce::Colour(0xFFCC6600));
      g.fillRect(static_cast<float>(barX), static_cast<float>(barY),
                 barW * progress, static_cast<float>(barH));
    }
    return;
  }

  if (error_state_) {
    g.fillAll(juce::Colours::black);
    return;
  }

  // 外部窗口模式：JUCE 组件仅显示占位面板，实际渲染在 NativeExternalWindow 中
  if (initialized_ && external_window_) {
    const auto bounds = getLocalBounds();
    PinkXP::drawRaised(g, bounds, juce::Colours::transparentBlack);

    auto tb = getTitleBarBounds();
    PinkXP::drawPinkTitleBar(g, tb, titleText, 12.0f);
    g.setColour(PinkXP::dark);
    g.fillRect(tb.getX(), tb.getBottom(), tb.getWidth(), 1);

    auto cb = getCloseButtonBounds();
    PinkXP::drawRaised(g, cb, PinkXP::btnFace);
    g.setColour(PinkXP::ink);
    g.setFont(PinkXP::getFont(11.0f, juce::Font::bold));
    auto cbText = cb;
    cbText.translate(-1, -1);
    g.drawText("x", cbText, juce::Justification::centred, false);

    auto content = getContentBounds();
    g.setColour(juce::Colour(0xFF1E1E1E));
    g.fillRect(content);
    g.setColour(juce::Colour(0xFF888888));
    g.setFont(13.0f);
    g.drawText("MilkDrop3 — External Window\nDrag title bar to reposition",
               content, juce::Justification::centred, false);
    return;
  }

  // 已初始化：只绘制模块边框和标题栏，不绘制内容区背景。
  // 内容区由 D3D9 WS_POPUP 子窗口独立覆盖，JUCE 侧绘制内容区 drawSunken
  // 的白色填充会在边框缝隙处露出灰色/白色占位条，与黑色视频区视觉割裂。
  // 参考 MilkdropModule 的做法：drawRaised 用透明填充，让 D3D9 黑色底自然透出。
  const auto bounds = getLocalBounds();

  // 1. 像素凸起窗口边框（透明填充，不让内容底色透过）
  PinkXP::drawRaised(g, bounds, juce::Colours::transparentBlack);

  // 2. 玫瑰粉标题栏
  auto tb = getTitleBarBounds();
  PinkXP::drawPinkTitleBar(g, tb, titleText, 12.0f);

  // 标题栏下沿深色分割线（凸出边框外 1px）
  g.setColour(PinkXP::dark);
  g.fillRect(tb.getX(), tb.getBottom(), tb.getWidth(), 1);

  // 3. 关闭按钮（×）—— 始终画默认状态；hover/press 由基类鼠标事件处理
  auto cb = getCloseButtonBounds();
  PinkXP::drawRaised(g, cb, PinkXP::btnFace);

  g.setColour(PinkXP::ink);
  g.setFont(PinkXP::getFont(11.0f, juce::Font::bold));
  auto cbText = cb;
  cbText.translate(-1, -1);
  g.drawText("x", cbText, juce::Justification::centred, false);
}

void Milkdrop3Module::paintContent(juce::Graphics& g,
                                   juce::Rectangle<int> contentBounds) {
  if (error_state_) {
    g.fillAll(juce::Colours::black);
    g.setColour(juce::Colours::red);
    g.setFont(14.0f);
    g.drawText("MilkDrop3 Init Failed",
               contentBounds.reduced(8), juce::Justification::centredTop);
    g.setColour(juce::Colours::white);
    g.setFont(11.0f);
    g.drawText(error_message_,
               contentBounds.reduced(8).withTop(contentBounds.getY() + 28),
               juce::Justification::topLeft);
    return;
  }

  if (!initialized_) {
    // 加载进度页（D3D9 popup 尚未创建，此处 JUCE 内容区可见）
    g.fillAll(juce::Colours::black);
    g.setColour(juce::Colours::white);
    g.setFont(16.0f);
    g.drawText("MilkDrop3 Loading...",
               contentBounds.withBottom(contentBounds.getCentreY() - 4),
               juce::Justification::centredBottom);

    if (status_message_.isNotEmpty()) {
      g.setColour(juce::Colour(0xFFAAAAAA));
      g.setFont(12.0f);
      g.drawText(status_message_,
                 contentBounds.withTop(contentBounds.getCentreY() + 4),
                 juce::Justification::centredTop);
    }

    const int barW = juce::jmin(300, contentBounds.getWidth() - 40);
    const int barH = 6;
    const int barX = contentBounds.getCentreX() - barW / 2;
    const int barY = contentBounds.getCentreY() + 28;
    g.setColour(juce::Colour(0xFF333333));
    g.fillRect(barX, barY, barW, barH);
    if (init_phase_ >= -1) {
      const int effective_phase = juce::jmax(0, init_phase_);
      const float progress = juce::jlimit(
          0.0f, 1.0f, static_cast<float>(effective_phase + 1) / 6.0f);
      g.setColour(juce::Colour(0xFFCC6600));
      g.fillRect(static_cast<float>(barX), static_cast<float>(barY),
                 barW * progress, static_cast<float>(barH));
    }
    return;
  }

  // 已初始化：控件栏由 overlay HWND 独立 GDI 渲染，聚焦/失焦时 overlay 自动显隐。
  // D3D9 popup 尺寸不变，无需在 contentBounds 内绘制任何 JUCE 控件栏内容。
  if (initialized_ && focused_) {
    // overlay 已在 SetFocusVisual 中显示，此处不做额外绘制。
    // 保留此判断以防未来需要在此添加逻辑。
  }
}

void Milkdrop3Module::layoutContent(juce::Rectangle<int> contentBounds) {
  const int x = contentBounds.getX();
  const int y = contentBounds.getY();
  const int w = contentBounds.getWidth();
  const int h = contentBounds.getHeight();

  if (w <= 0 || h <= 0) return;

  // D3D9 popup 始终占满整个内容区，不再因控件栏/弹窗而改变尺寸。
  // 控件栏通过独立的 overlay HWND 悬浮于 popup 之上，聚焦时显示、失焦时隐藏。
  const int popup_y = y;
  const int popup_h = juce::jmax(1, h);

  // 首次布局：延迟启动 Timer 驱动的分阶段初始化。
  if (!d3d_window_ && !initialized_ && !error_state_ && init_phase_ < 0) {
    init_x_ = x;
    init_y_ = popup_y;
    init_width_  = w;
    init_height_ = popup_h;
    init_phys_w_ = w;
    init_phys_h_ = popup_h;
    init_phase_  = -1;
    status_message_ = "Waiting for native window peer...";
    juce::MessageManager::callAsync([this] { startTimer(5); });
    return;
  }

  // 位置/大小变更：同步 D3D9 popup
  if (d3d_window_) {
    if (x != last_content_x_ || popup_y != last_content_y_ ||
        w != last_content_w_ || popup_h != last_content_h_) {
      d3d_window_->Reposition(x, popup_y, w, popup_h);
      last_content_x_ = x;
      last_content_y_ = popup_y;
      last_content_w_ = w;
      last_content_h_ = popup_h;
    }
  }

  // 同步 overlay 位置（位于 popup 顶部，不随 popup 尺寸变化）
  if (overlay_ && overlay_->IsVisible()) {
    overlay_->Reposition(x, y, w);
  }
}

// ==========================================================================
// AnalyserHub::FrameListener —— 只做快照，实际投喂在 pre-render injector
// ==========================================================================

void Milkdrop3Module::onFrame(const AnalyserHub::FrameSnapshot& frame) {
  if (!initialized_ || error_state_) return;

  std::lock_guard<std::mutex> lock(audio_mutex_);

  if (frame.has(AnalyserHub::Kind::Oscilloscope)) {
    constexpr int N = AudioSnapshot::kOscSize;
    for (int i = 0; i < N; ++i) {
      audio_snapshot_.interleaved[i * 2]     = frame.oscL[static_cast<size_t>(i)];
      audio_snapshot_.interleaved[i * 2 + 1] = frame.oscR[static_cast<size_t>(i)];
    }
    audio_snapshot_.has_pcm = true;
  }

  if (frame.has(AnalyserHub::Kind::Spectrum)) {
    // Y2Kmeter 目前只提供混合单声道 FFT 结果（spectrumMag），
    // 两声道使用同一份数据。若后续增加独立 L/R 谱，可在此分离。
    constexpr int M = AudioSnapshot::kSpectrumSize;
    for (int i = 0; i < M; ++i) {
      const float m = frame.spectrumMag[static_cast<size_t>(i)];
      audio_snapshot_.specL[i] = m;
      audio_snapshot_.specR[i] = m;
    }
    audio_snapshot_.sample_rate =
        static_cast<float>(hub_ ? hub_->getSampleRate() : 48000.0);
    audio_snapshot_.has_spectrum = true;
  }
}

void Milkdrop3Module::FeedEngineFromSnapshot() {
  AudioSnapshot local;
  {
    std::lock_guard<std::mutex> lock(audio_mutex_);
    if (!audio_snapshot_.has_pcm && !audio_snapshot_.has_spectrum) return;
    local = audio_snapshot_;
    // 保留快照，让下一帧在无新数据时也能持续投喂上一次值（visualiser 不至于停顿）
  }

  if (local.has_pcm) {
    api_.FeedPcm(local.interleaved.data(), AudioSnapshot::kOscSize);
  }
  if (local.has_spectrum) {
    api_.FeedSpectrum(local.specL.data(), local.specR.data(),
                      AudioSnapshot::kSpectrumSize, local.sample_rate);
  }
}

// ==========================================================================
// 用户交互 API
// ==========================================================================

void Milkdrop3Module::NextPreset() {
  if (!initialized_) return;
  api_.NextPreset(2.0f);
  SyncOverlayContent();
  repaint();
}

void Milkdrop3Module::PrevPreset() {
  if (!initialized_) return;
  api_.PrevPreset(2.0f);
  SyncOverlayContent();
  repaint();
}

void Milkdrop3Module::RandomPreset() {
  if (!initialized_) return;
  api_.RandomPreset(2.0f);
  SyncOverlayContent();
  repaint();
}

void Milkdrop3Module::AnnouncePresetNameToEngine() {
  const std::wstring name = api_.GetCurrentPresetName();
  if (!name.empty()) {
    api_.ShowPresetTitleAnim(name.c_str());
  }
}

// ==========================================================================
// 键盘（← / → / space）
// ==========================================================================

bool Milkdrop3Module::keyPressed(const juce::KeyPress& key) {
  if (!initialized_ || error_state_) return false;

  if (jump_dialog_open_) {
    // Enter/Esc 由 overlay 内 EDIT 控件的子类化 WndProc 拦截处理，
    // 不再依赖 JUCE TextEditor。此处作为后备：Esc 仍然关闭弹窗。
    if (key == juce::KeyPress::escapeKey) {
      CloseJumpDialog();
      return true;
    }
    return false;
  }

  if (key == juce::KeyPress::leftKey)  { PrevPreset();       return true; }
  if (key == juce::KeyPress::rightKey) { NextPreset();       return true; }
  if (key == juce::KeyPress::spaceKey) { RandomPreset();     return true; }
  if (key == juce::KeyPress('a'))      { ToggleAutoMode();   return true; }
  return false;
}

// ==========================================================================
// 鼠标 —— 控件栏按钮 + 焦点 + 自动隐藏
// ==========================================================================

void Milkdrop3Module::mouseDown(const juce::MouseEvent& e) {
  // 弹窗打开时：overlay 内的 Go/Cancel 按钮由其 WndProc 处理，
  // 点击 overlay 内其他区域会通过 overlay 的 WndProc 触发 CloseJumpDialog。
  // 此处仅处理右键冒泡和非聚焦时的点击聚焦。
  if (jump_dialog_open_) {
    // overlay 已处理所有交互；此处仅作为后备关闭
    CloseJumpDialog();
    return;
  }

  // 右键 → 冒泡给 workspace
  if (e.mods.isPopupMenu()) {
    ModulePanel::mouseDown(e);
    return;
  }

  // 点击内容区任意位置 → 聚焦
  if (!focused_) {
    SetFocusVisual(true);
  }

  ModulePanel::mouseDown(e);
}

void Milkdrop3Module::mouseUp(const juce::MouseEvent& e) {
  // 控件栏按钮交互已由 overlay HWND 的 WndProc 独立处理，
  // JUCE 端不再需要 hit-test 按钮区域。
  ModulePanel::mouseUp(e);
}

void Milkdrop3Module::mouseMove(const juce::MouseEvent& e) {
  // overlay 独立处理鼠标指针样式和按钮 hover 高亮，
  // JUCE 端仅保留基类行为。
  ModulePanel::mouseMove(e);
}

void Milkdrop3Module::mouseExit(const juce::MouseEvent& e) {
  if (hovered_btn_ != OverlayButton::kNone) {
    hovered_btn_ = OverlayButton::kNone;
    repaint();
  }
  ModulePanel::mouseExit(e);
}

// ==========================================================================
// 外部窗口同步
// ==========================================================================

void Milkdrop3Module::SyncExternalWindowChildren() {
  if (!external_window_ || !d3d_window_) return;

  const int cx = external_window_->GetContentX();
  const int cy = external_window_->GetContentY();
  const int cw = external_window_->GetContentW();
  const int ch = external_window_->GetContentH();

  d3d_window_->RepositionScreen(cx, cy, cw, ch);

  if (overlay_ && overlay_->IsVisible()) {
    overlay_->RepositionAtScreenPos(cx, cy, cw);
  }
}

// ==========================================================================
// 焦点与叠加层显隐（与 MilkdropModule 统一）
// ==========================================================================

void Milkdrop3Module::SetFocusVisual(bool shouldFocus) {
  if (focused_ == shouldFocus) return;

  focused_ = shouldFocus;
  if (!focused_) {
    hovered_btn_ = OverlayButton::kNone;
    pressed_btn_ = OverlayButton::kNone;
    // 失去焦点时关闭跳转弹窗，防止 jump_dialog_open_ 与 overlay 内部状态不一致。
    // 否则下次点击视频区恢复焦点时，D3dChildWindow WM_LBUTTONDOWN 检查到
    // jump_dialog_open_==true 会直接 return，SetFocusVisual(true) 永远不会被调用。
    CloseJumpDialog();
  }

  // 通过 overlay 显隐控制控件栏，不再改变 D3D9 popup 尺寸。
  // D3D9 视频窗始终保持固定大小，避免设备重置引起的界面闪烁。
  if (overlay_) {
    overlay_->SetVisible(focused_);
    if (focused_) {
      // 显示时同步位置 + 刷新预设名
      if (external_window_) {
        overlay_->RepositionAtScreenPos(
            external_window_->GetContentX(),
            external_window_->GetContentY(),
            external_window_->GetContentW());
      } else {
        const auto content = getContentBounds();
        overlay_->Reposition(content.getX(), content.getY(), content.getWidth());
      }
      SyncOverlayContent();
    }
  }

  repaint();
}

// ==========================================================================
// 自动轮播
// ==========================================================================

void Milkdrop3Module::ToggleAutoMode() {
  is_auto_mode_ = !is_auto_mode_;
  if (is_auto_mode_) {
    last_auto_switch_ms_ = juce::Time::getMillisecondCounter();
  }
  SyncOverlayContent();
  repaint();
}

void Milkdrop3Module::CheckAutoMode() {
  if (!is_auto_mode_) return;

  const juce::uint32 now = juce::Time::getMillisecondCounter();
  const juce::uint32 interval_ms =
      static_cast<juce::uint32>(auto_interval_secs_ * 1000.0f);
  if (now - last_auto_switch_ms_ >= interval_ms) {
    last_auto_switch_ms_ = now;
    RandomPreset();
  }
}

void Milkdrop3Module::UpdateAutoIntervalFromSlider(float proportion) {
  proportion = juce::jlimit(0.0f, 1.0f, proportion);
  float secs = kMinAutoInterval
               + proportion * (kMaxAutoInterval - kMinAutoInterval);
  secs = juce::jlimit(kMinAutoInterval, kMaxAutoInterval, secs);
  secs = std::round(secs * 10.0f) / 10.0f;

  if (secs != auto_interval_secs_) {
    auto_interval_secs_ = secs;
  }
}

// ==========================================================================
// 控件栏 —— 布局 / 命中测试 / 绘制 / 执行
//   位于内容区顶部置顶（与 MilkdropModule 统一）。
//   仅在 focused_ 时绘制，聚焦显示、失焦隐藏。
//   按钮布局：[<]  nameArea  [A] [>] [?]
// ==========================================================================

namespace {
constexpr int kCtrlBtnSize  = 22;
constexpr int kCtrlPadding  = 4;
constexpr int kAutoBtnW     = 28;
}  // namespace

juce::Rectangle<int> Milkdrop3Module::GetControlBarRect() const {
  const auto content = getContentBounds();
  const int h = jump_dialog_open_ ? kControlBarHeight + kJumpDlgHeight
                                   : kControlBarHeight;
  if (content.getHeight() <= h) return {};
  return content.withHeight(h);
}

juce::Rectangle<int> Milkdrop3Module::GetControlBarBtnRect(OverlayButton btn) const {
  const auto bar = GetControlBarRect();
  if (bar.isEmpty()) return {};

  // 按钮始终定位在顶部 26px 区域，不随弹窗面板扩展而偏移
  const int y = bar.getY() + (kControlBarHeight - kCtrlBtnSize) / 2;

  // 右侧按钮（从右往左排）：[?] [>] [A]
  auto right_btn = [&](int index_from_right) -> juce::Rectangle<int> {
    const int x = bar.getRight() - kCtrlPadding - kCtrlBtnSize
                  - index_from_right * (kCtrlBtnSize + kCtrlPadding);
    return juce::Rectangle<int>(x, y, kCtrlBtnSize, kCtrlBtnSize);
  };

  // [<] 左侧
  auto prev_btn = juce::Rectangle<int>(
      bar.getX() + kCtrlPadding, y, kCtrlBtnSize, kCtrlBtnSize);

  // 预设名区域：[<] 右侧 → 到 [A] 左侧
  auto auto_btn = juce::Rectangle<int>(
      right_btn(1).getX() - kCtrlPadding - kAutoBtnW,
      y, kAutoBtnW, kCtrlBtnSize);
  juce::Rectangle<int> nameArea(
      prev_btn.getRight() + kCtrlPadding, y,
      auto_btn.getX() - kCtrlPadding - prev_btn.getRight() - kCtrlPadding,
      kCtrlBtnSize);

  switch (btn) {
    case OverlayButton::kPrev:       return prev_btn;
    case OverlayButton::kAuto:       return auto_btn;
    case OverlayButton::kNext:       return right_btn(1);
    case OverlayButton::kRandom:     return right_btn(0);
    case OverlayButton::kPresetName: return nameArea;
    default:                         return {};
  }
}

Milkdrop3Module::OverlayButton
Milkdrop3Module::HitTestControlBarBtn(juce::Point<int> pos) const {
  for (OverlayButton btn : { OverlayButton::kPrev, OverlayButton::kPresetName,
                             OverlayButton::kAuto, OverlayButton::kNext,
                             OverlayButton::kRandom }) {
    if (GetControlBarBtnRect(btn).contains(pos)) return btn;
  }
  return OverlayButton::kNone;
}

void Milkdrop3Module::ExecuteOverlayAction(OverlayButton btn) {
  switch (btn) {
    case OverlayButton::kPrev:       PrevPreset();          break;
    case OverlayButton::kNext:       NextPreset();          break;
    case OverlayButton::kRandom:     RandomPreset();        break;
    case OverlayButton::kAuto:       ToggleAutoMode();      break;
    case OverlayButton::kPresetName: ShowPresetJumpDialog(); break;
    default: break;
  }
}

juce::String Milkdrop3Module::GetPresetDisplayName() const {
  if (!initialized_) return {};
  int idx = api_.GetCurrentPresetIndex();         // 文件级 0-based
  int nFiles = api_.GetFileablePresetCount();
  juce::String display;
  if (nFiles > 0 && idx >= 0)
    display = juce::String(idx + 1) + "/" + juce::String(nFiles) + "  ";
  std::wstring w = api_.GetCurrentPresetName();
  if (!w.empty()) {
    juce::String name(w.c_str());
    // 去掉 .milk / .milk2 后缀
    if (name.endsWithIgnoreCase(".milk2"))
      name = name.dropLastCharacters(6);
    else if (name.endsWithIgnoreCase(".milk"))
      name = name.dropLastCharacters(5);
    // 只保留文件名（不带目录）
    const int slash =
        (std::max)(name.lastIndexOfChar('/'), name.lastIndexOfChar('\\'));
    if (slash >= 0) name = name.substring(slash + 1);
    display += name;
  }
  if (display.isEmpty()) display = "(no preset)";
  return display;
}

void Milkdrop3Module::PaintControlBar(juce::Graphics& g) {
  // 控件栏已迁移至 overlay HWND 通过 GDI 独立渲染。
  // 此方法保留为接口兼容，仅同步 overlay 的预设名和自动轮播状态。
  juce::ignoreUnused(g);
  SyncOverlayContent();
}

void Milkdrop3Module::SyncOverlayContent() {
  if (!overlay_ || !overlay_->IsVisible()) return;

  // 同步预设名显示
  const juce::String display = GetPresetDisplayName();
  if (display.isNotEmpty()) {
    int idx = api_.GetCurrentPresetIndex();         // 文件级 0-based
    int nFiles = api_.GetFileablePresetCount();
    juce::String idxPart =
        juce::String(idx + 1) + "/" + juce::String(nFiles) + "  ";
    std::wstring wstr(display.toWideCharPointer());
    overlay_->SetPresetDisplay(wstr, idxPart.length());
  }

  // 同步自动轮播状态
  overlay_->SetAutoMode(is_auto_mode_);
}

// ==========================================================================
// juce::Timer —— 分阶段异步初始化 + 渲染循环
// ==========================================================================

void Milkdrop3Module::timerCallback() {
  // ---- 已初始化：渲染循环 ----
  if (initialized_) {
    if (!isShowing()) return;

    // 外部窗口最小化同步：主窗口最小化时隐藏外部窗口
    if (external_window_ && root_hwnd_) {
      external_window_->SetVisible(!IsIconic(root_hwnd_));
    }

    // 模块被拖动时同步 popup 位置（layoutContent 不因拖动被回调）
    const auto sp = getScreenPosition();
    if (sp.x != last_screen_x_ || sp.y != last_screen_y_) {
      last_screen_x_ = sp.x;
      last_screen_y_ = sp.y;
      // 外部窗口模式下，JUCE placeholder 位置变化 → 同步外部窗口
      if (external_window_) {
        // 将 JUCE 逻辑坐标转换为屏幕物理坐标，移动外部窗口
        const juce::Point<int> physTopLeft =
            juce::Desktop::getInstance().getDisplays().logicalToPhysical(
                sp);
        const int physContentW =
            juce::Desktop::getInstance().getDisplays().logicalToPhysical(
                juce::Rectangle<int>(0, 0, last_content_w_, 1)).getWidth();
        const int physContentH =
            juce::Desktop::getInstance().getDisplays().logicalToPhysical(
                juce::Rectangle<int>(0, 0, 1, last_content_h_)).getHeight();

        // external window 的 content (没有标题栏和边框) 位于 (physX, physY)
        RECT ewRc; GetWindowRect(external_window_->GetHwnd(), &ewRc);
        const int border = 2;
        const int titleH = 26;
        const int targetX = physTopLeft.x - border;
        const int targetY = physTopLeft.y - titleH;

        if (targetX != ewRc.left || targetY != ewRc.top) {
          SetWindowPos(external_window_->GetHwnd(), nullptr,
                       targetX, targetY,
                       physContentW + border * 2,
                       physContentH + titleH + border,
                       SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
        }
      } else {
        if (d3d_window_ && last_content_w_ > 0 && last_content_h_ > 0) {
          d3d_window_->Reposition(last_content_x_, last_content_y_,
                                   last_content_w_, last_content_h_);
        }
        // overlay 也跟随移动
        if (overlay_ && overlay_->IsVisible()) {
          const auto content = getContentBounds();
          overlay_->Reposition(content.getX(), content.getY(), content.getWidth());
        }
      }
    }

    // 自动轮播计时检查
    CheckAutoMode();

    // 预设名称变更检测：异步加载完成后 m_szCurrentPresetFile 更新，
    // 此时才触发中央动画并刷新 overlay 显示。避免在异步加载中提前读取旧名称。
    {
      const std::wstring curName = api_.GetCurrentPresetName();
      if (!curName.empty() && curName != last_announced_name_) {
        last_announced_name_ = curName;
        AnnouncePresetNameToEngine();
        SyncOverlayContent();
      }
    }

    api_.RenderFrame();
    return;
  }

  // ---- 分阶段初始化 ----
  switch (init_phase_) {
    case -1: {
      // 等待 JUCE native peer 就绪
      HWND parent_hwnd = reinterpret_cast<HWND>(getWindowHandle());
      if (!parent_hwnd) return;

      root_hwnd_ = GetAncestor(parent_hwnd, GA_ROOT);

      // 精确物理尺寸（用模块 top-left 所在屏幕的缩放）
      const juce::Point<int> logicalGlobalTL =
          localPointToGlobal(juce::Point<int>(init_x_, init_y_));
      const juce::Rectangle<int> physRect =
          juce::Desktop::getInstance().getDisplays().logicalToPhysical(
              juce::Rectangle<int>(logicalGlobalTL.x, logicalGlobalTL.y,
                                    init_width_, init_height_));
      init_phys_w_ = physRect.getWidth();
      init_phys_h_ = physRect.getHeight();

      init_parent_hwnd_ = parent_hwnd;
      init_phase_ = 0;
      status_message_ = "Phase 0/5: Creating D3D9 popup HWND...";
      repaint();
      return;
    }

    case 0: {
      d3d_window_ = std::make_unique<D3dChildWindow>(*this);
      if (!d3d_window_->CreateHWNDOnly(init_parent_hwnd_,
                                        init_x_, init_y_,
                                        init_width_, init_height_)) {
        error_state_ = true;
        error_message_ = "Failed to create D3D9 popup window.";
        stopTimer();
        repaint();
        return;
      }
      init_phase_ = 1;
      status_message_ = "Phase 1/5: Creating MilkDrop3 render window...";
      repaint();
      return;
    }

    case 1: {
      const HWND embed_hwnd = d3d_window_->GetHwnd();
      if (!api_.Initialize_CreateRenderWindow(embed_hwnd,
                                               init_phys_w_, init_phys_h_)) {
        error_state_ = true;
        error_message_ = juce::String(api_.GetError());
        stopTimer();
        repaint();
        return;
      }
      init_phase_ = 2;
      status_message_ = "Phase 2/5: Setting up preset paths...";
      repaint();
      return;
    }

    case 2: {
      if (!api_.Initialize_SetPaths()) {
        error_state_ = true;
        error_message_ = juce::String(api_.GetError());
        stopTimer();
        repaint();
        return;
      }
      init_phase_ = 3;
      status_message_ = "Phase 3/5: PluginPreInitialize (fonts, defaults)...";
      repaint();
      return;
    }

    case 3: {
      if (!api_.Initialize_PreInit()) {
        error_state_ = true;
        error_message_ = juce::String(api_.GetError());
        stopTimer();
        repaint();
        return;
      }
      init_phase_ = 4;
      status_message_ = "Phase 4/5: Creating D3D9 device...";
      repaint();
      return;
    }

    case 4: {
      if (!api_.Initialize_CreateDevice(init_phys_w_, init_phys_h_)) {
        error_state_ = true;
        error_message_ = juce::String(api_.GetError());
        stopTimer();
        repaint();
        return;
      }
      init_phase_ = 5;
      status_message_ = "Phase 5/5: PluginInitialize (shaders, presets, textures)...";
      repaint();
      return;
    }

    case 5: {
      if (!api_.Initialize_PluginInit(init_phys_w_, init_phys_h_)) {
        error_state_ = true;
        error_message_ = juce::String(api_.GetError());
        stopTimer();
        repaint();
        return;
      }

      // 完成初始化
      initialized_ = true;
      init_phase_  = -1;
      status_message_.clear();

      last_content_x_ = init_x_;
      last_content_y_ = init_y_;
      last_content_w_ = init_width_;
      last_content_h_ = init_height_;
      last_screen_x_  = getScreenPosition().x;
      last_screen_y_  = getScreenPosition().y;

      // 创建控件栏 overlay（先于外部窗口创建，以便获取其 HWND）
      {
        overlay_ = std::make_unique<ControlBarOverlay>(*this);
        if (!overlay_->Create(root_hwnd_)) {
          overlay_.reset();
        } else {
          overlay_->UpdateThemeColors();
        }
      }

      // 创建外部窗口（self-drawn GDI chrome，WS_EX_TOOLWINDOW）
      // D3D9 popup 由 root_hwnd_ own，随外部窗口位置对齐。
      {
        // 计算 D3D9 popup 当前物理坐标（与外部窗口内容区对齐）
        const juce::Point<int> d3dLogicalTL =
            localPointToGlobal(juce::Point<int>(init_x_, init_y_));
        const juce::Rectangle<int> physContentRect =
            juce::Desktop::getInstance().getDisplays().logicalToPhysical(
                juce::Rectangle<int>(d3dLogicalTL.x, d3dLogicalTL.y,
                                      init_width_, init_height_));

        external_window_ = std::make_unique<NativeExternalWindow>(*this);
        if (external_window_->Create(root_hwnd_,
                                      physContentRect.getX(),
                                      physContentRect.getY(),
                                      physContentRect.getWidth(),
                                      physContentRect.getHeight())) {
          external_window_->SetCloseCallback([this] {
            juce::MessageManager::callAsync([this] {
              if (onCloseClicked) onCloseClicked(*this);
            });
          });

          // overlay 使用屏幕坐标模式
          if (overlay_) {
            overlay_->SetUseScreenCoords(true);
          }
        } else {
          external_window_.reset();
        }
      }

      // 将 D3D9 popup 对齐到外部窗口内容区
      if (d3d_window_ && external_window_) {
        d3d_window_->RepositionScreen(
            external_window_->GetContentX(),
            external_window_->GetContentY(),
            external_window_->GetContentW(),
            external_window_->GetContentH());
      }

      // 关闭引擎自带的预设名 D3D9 overlay
      api_.EnablePresetInfoOverlay(false);

      // 禁用引擎内置的预设自动轮播
      api_.DisableAutoAdvance();

      // 注册 pre-render injector
      audio_injector_token_ = api_.AddPreRenderInjector(
          [this] { FeedEngineFromSnapshot(); });

      // 首帧 + 显示当前预设名的中央弹出动画
      api_.RenderFrame();
      AnnouncePresetNameToEngine();

      // 初始化完成后显式同步 overlay 显隐状态
      if (overlay_ && focused_) {
        overlay_->SetVisible(true);
        if (external_window_) {
          overlay_->RepositionAtScreenPos(
              external_window_->GetContentX(),
              external_window_->GetContentY(),
              external_window_->GetContentW());
        }
        SyncOverlayContent();
      }

      MD3_LOG("Milkdrop3Module: init complete. preset=%d/%d",
              api_.GetCurrentPresetIndex() + 1, api_.GetFileablePresetCount());

      // 切到渲染循环（30 fps）
      stopTimer();
      startTimer(33);
      repaint();
      return;
    }

    default:
      break;
  }
}

// ==========================================================================
// 预设跳转弹窗（overlay 内嵌原生 EDIT 控件，非模态）
//   弹窗打开时 overlay 向下扩展 kJumpDlgHeight px，不再通过 D3D9 popup 位移。
//   overlay 内嵌原生 EDIT 控件接收输入（不再依赖 JUCE TextEditor）。
//   Enter / 点击 Go → 跳转；Esc / 点击 Cancel / 点击别处 → 关闭。
// ==========================================================================

void Milkdrop3Module::ShowPresetJumpDialog() {
  if (!initialized_ || jump_dialog_open_) return;

  const int nFiles = api_.GetFileablePresetCount();
  if (nFiles <= 0) return;
  const int current = api_.GetCurrentPresetIndex();  // 文件级 0-based
  if (current < 0) return;

  jump_dialog_open_ = true;

  // 委托 overlay 管理跳转弹窗（扩展高度 + 创建原生 EDIT 控件）
  if (overlay_) {
    overlay_->ShowJumpDialog(nFiles, current);
  }

  repaint();
}

void Milkdrop3Module::CloseJumpDialog() {
  if (!jump_dialog_open_) return;
  jump_dialog_open_ = false;

  if (overlay_)
    overlay_->CloseJumpDialog();

  repaint();
}

juce::Rectangle<int> Milkdrop3Module::GetJumpDlgGoRect() const {
  // overlay 使用 GDI 渲染，Go/Cancel 按钮位置由 overlay 内部计算，
  // 此方法保留仅为接口兼容（当前已无 JUCE 端调用）。
  return {};
}

juce::Rectangle<int> Milkdrop3Module::GetJumpDlgCancelRect() const {
  return {};
}

void Milkdrop3Module::PaintJumpDialog(juce::Graphics& g,
                                       juce::Rectangle<int> bar) {
  // overlay 使用 GDI 渲染跳转弹窗面板和按钮，
  // 不再通过 JUCE Graphics 绘制。
  juce::ignoreUnused(g, bar);
}