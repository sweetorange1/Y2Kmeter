/*
  ==============================================================================

  Milkdrop3Window.cpp
  Y2Kmeter — MilkDrop3 独立弹窗实现。

  ==============================================================================
*/

#include "source/ui/Milkdrop3Window.h"
#include "source/ui/modules/Milkdrop3Api.h"
#include "source/ui/modules/Md3DebugLog.h"
#include "source/ui/PinkXPStyle.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <commctrl.h>

// ==========================================================================
// D3dChildHwnd —— D3D9 渲染子 HWND
//
// 使用 WS_CHILD 作为窗口的直接子窗口，填满客户区。
// 坐标空间 (PMv2)：D3D9 渲染分辨率使用物理像素。
// ==========================================================================

bool Milkdrop3Window::D3dChildHwnd::Create(HWND parent, int w, int h) {
  static bool class_registered = false;
  if (!class_registered) {
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = L"Y2Kmeter_MD3W_D3D";
    if (!RegisterClassExW(&wc)) return false;
    class_registered = true;
  }

  hwnd_ = CreateWindowExW(
      0, L"Y2Kmeter_MD3W_D3D", L"",
      WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
      0, 0, w, h,
      parent, nullptr, GetModuleHandleW(nullptr), nullptr);

  if (!hwnd_) {
    MD3_LOG("D3dChildHwnd::Create: CreateWindowExW failed err=%lu",
            GetLastError());
  }
  return hwnd_ != nullptr;
}

void Milkdrop3Window::D3dChildHwnd::Destroy() {
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

void Milkdrop3Window::D3dChildHwnd::Resize(int w, int h) {
  if (!hwnd_) return;

  // D3D9 子窗口使用物理像素。窗口客户区尺寸在 PMv2 下为物理像素，
  // 由调用方在 resized() 中完成 logicalToPhysical 转换后传入。
  SetWindowPos(hwnd_, nullptr,
               0, 0, w, h,
               SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);

  // 通知引擎分辨率变更
  milkdrop3_api::Api::Instance().OnResize(w, h);
}

// ==========================================================================
// ControlBarOverlay —— 控件栏 GDI 覆盖层（内嵌类）
//
// WS_CHILD 子窗口，悬浮于 D3D9 渲染区之上。使用 GDI 绘制按钮与文字，
// 不再依赖 JUCE 组件绘制。按钮从左到右：
//   [📌] [<]  presetName  [A] [>] [?]
// ==========================================================================

class Milkdrop3Window::ControlBarOverlay {
 public:
  enum class DialogBtn { kNone, kGo, kCancel };

  explicit ControlBarOverlay(Milkdrop3Window& owner) : owner_(owner) {}
  ~ControlBarOverlay() { Destroy(); }

  bool Create(HWND parent, int w) {
    if (hwnd_) return true;

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
      wc.lpszClassName = L"Y2Kmeter_MD3W_Overlay";
      if (!RegisterClassExW(&wc)) return false;
      class_registered = true;
    }

    width_ = w;
    int h = owner_.jump_dialog_open_
        ? kControlBarHeight + kJumpDlgHeight
        : kControlBarHeight;

    hwnd_ = CreateWindowExW(
        0, L"Y2Kmeter_MD3W_Overlay", L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, 0, w, h,
        parent, nullptr, GetModuleHandleW(nullptr), this);

    if (!hwnd_) return false;

    // 创建控件栏字体
    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
      ncm.lfMessageFont.lfWeight = FW_BOLD;
      font_bold_ = CreateFontIndirectW(&ncm.lfMessageFont);
      ncm.lfMessageFont.lfWeight = FW_NORMAL;
      font_plain_ = CreateFontIndirectW(&ncm.lfMessageFont);
    }

    UpdateThemeColors();
    return true;
  }

  void Destroy() {
    CloseJumpDialog();
    if (font_bold_) { DeleteObject(font_bold_); font_bold_ = nullptr; }
    if (font_plain_) { DeleteObject(font_plain_); font_plain_ = nullptr; }
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
  }

  void Resize(int w) {
    width_ = w;
    int h = owner_.jump_dialog_open_
        ? kControlBarHeight + kJumpDlgHeight
        : kControlBarHeight;
    if (hwnd_) {
      SetWindowPos(hwnd_, nullptr, 0, 0, w, h,
                   SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
      InvalidateRect(hwnd_, nullptr, FALSE);
    }
  }

  void SetPresetDisplay(const std::wstring& text, int idxPartLen) {
    preset_display_ = text;
    preset_idx_part_len_ = idxPartLen;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
  }

  void SetAutoMode(bool active) {
    if (auto_mode_active_ == active) return;
    auto_mode_active_ = active;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
  }

  void SetPinState(bool pinned) {
    if (pin_state_ == pinned) return;
    pin_state_ = pinned;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
  }

  void UpdateThemeColors() {
    auto jc2cr = [](juce::Colour c) -> COLORREF {
      return RGB(c.getRed(), c.getGreen(), c.getBlue());
    };
    theme_bg_        = jc2cr(juce::Colour(0x1E, 0x1E, 0x1E));
    theme_bg_darker  = jc2cr(juce::Colour(0x16, 0x16, 0x16));
    theme_pink100    = jc2cr(juce::Colour(0xCC, 0xCC, 0xCC));
    theme_pink200    = jc2cr(juce::Colour(0x99, 0x99, 0x99));
    theme_pink300    = jc2cr(juce::Colour(0x66, 0x66, 0x66));
    theme_pink600    = jc2cr(juce::Colour(0x4A, 0x4A, 0x4A));
    theme_btn_normal = jc2cr(juce::Colour(0x33, 0x33, 0x33));
    theme_btn_face   = jc2cr(juce::Colour(0xDD, 0xDD, 0xDD));
    theme_ink        = jc2cr(juce::Colour(0xF0, 0xF0, 0xF0));
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
  }

  HWND GetHwnd() const { return hwnd_; }

  // ---- 跳转弹窗 ----
  void ShowJumpDialog(int nFiles, int currentPreset) {
    if (jump_dialog_open_ || !hwnd_) return;
    jump_dialog_open_ = true;

    // 扩展高度
    const int newH = kControlBarHeight + kJumpDlgHeight;
    SetWindowPos(hwnd_, nullptr, 0, 0, width_, newH,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);

    edit_hwnd_ = CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER | ES_RIGHT,
        14, kControlBarHeight + 50, 80, 22,
        hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (edit_hwnd_) {
      SetWindowSubclass(edit_hwnd_, EditSubclassProc, 0,
                        reinterpret_cast<DWORD_PTR>(this));
      SetWindowTextW(edit_hwnd_, std::to_wstring(currentPreset + 1).c_str());
      SendMessageW(edit_hwnd_, EM_SETSEL, 0, -1);
      SetFocus(edit_hwnd_);

      NONCLIENTMETRICSW ncm = { sizeof(ncm) };
      if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS,
                                sizeof(ncm), &ncm, 0)) {
        HFONT editFont = CreateFontIndirectW(&ncm.lfMessageFont);
        if (editFont)
          SendMessageW(edit_hwnd_, WM_SETFONT,
                       reinterpret_cast<WPARAM>(editFont), TRUE);
      }
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
  }

  void CloseJumpDialog() {
    if (!jump_dialog_open_) return;
    jump_dialog_open_ = false;
    dialog_btn_pressed_ = DialogBtn::kNone;
    if (edit_hwnd_) {
      DestroyWindow(edit_hwnd_);
      edit_hwnd_ = nullptr;
    }
    const int newH = kControlBarHeight;
    SetWindowPos(hwnd_, nullptr, 0, 0, width_, newH,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
    InvalidateRect(hwnd_, nullptr, FALSE);
  }

  bool IsJumpDialogOpen() const { return jump_dialog_open_; }

  // ---- 命中测试 ----
  OverlayButton HitTest(int px, int py) const {
    for (OverlayButton btn : { OverlayButton::kPin,
                               OverlayButton::kPrev,
                               OverlayButton::kPresetName,
                               OverlayButton::kAuto,
                               OverlayButton::kNext,
                               OverlayButton::kRandom }) {
      RECT r = GetButtonRect(btn);
      if (px >= r.left && px < r.right && py >= r.top && py < r.bottom)
        return btn;
    }
    return OverlayButton::kNone;
  }

  void GetJumpDialogButtonRects(RECT& goRc, RECT& cancelRc) const {
    goRc = { width_ - 14 - 54, kControlBarHeight + 50,
             width_ - 14, kControlBarHeight + 72 };
    cancelRc = { goRc.left - 8 - 56, kControlBarHeight + 50,
                 goRc.left - 8, kControlBarHeight + 72 };
  }

  DialogBtn HitTestJumpDialog(int px, int py) const {
    if (!jump_dialog_open_) return DialogBtn::kNone;
    RECT goRc, cancelRc;
    GetJumpDialogButtonRects(goRc, cancelRc);
    POINT pt = { px, py };
    if (PtInRect(&goRc, pt)) return DialogBtn::kGo;
    if (PtInRect(&cancelRc, pt)) return DialogBtn::kCancel;
    return DialogBtn::kNone;
  }

 private:
  static constexpr int kControlBarHeight = 26;
  static constexpr int kBtnSize = 20;
  static constexpr int kPinBtnW = 24;
  static constexpr int kBtnGap = 2;
  static constexpr int kJumpDlgHeight = 80;

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

  static LRESULT CALLBACK EditSubclassProc(HWND hwnd, UINT msg,
                                           WPARAM wparam, LPARAM lparam,
                                           UINT_PTR, DWORD_PTR refData) {
    auto* self = reinterpret_cast<ControlBarOverlay*>(refData);
    if (!self) return DefSubclassProc(hwnd, msg, wparam, lparam);

    if (msg == WM_KEYDOWN) {
      if (wparam == VK_RETURN) {
        self->DoPresetJump();
        return 0;
      }
      if (wparam == VK_ESCAPE) {
        self->owner_.CloseJumpDialog();
        return 0;
      }
    }
    return DefSubclassProc(hwnd, msg, wparam, lparam);
  }

  void DoPresetJump() {
    if (!edit_hwnd_) return;
    wchar_t buf[16] = {};
    GetWindowTextW(edit_hwnd_, buf, 15);
    int num = _wtoi(buf);
    int nFiles = owner_.api_.GetFileablePresetCount();
    if (num < 1 || num > nFiles) return;
    owner_.api_.JumpToPreset(num - 1);
    owner_.CloseJumpDialog();
    owner_.SyncOverlayContent();
    owner_.AnnouncePresetNameToEngine();
  }

  LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
      case WM_LBUTTONDOWN: {
        int px = LOWORD(lparam);
        int py = HIWORD(lparam);
        SetCapture(hwnd);

        if (jump_dialog_open_) {
          DialogBtn db = HitTestJumpDialog(px, py);
          if (db == DialogBtn::kGo) {
            dialog_btn_pressed_ = DialogBtn::kGo;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
          }
          if (db == DialogBtn::kCancel) {
            dialog_btn_pressed_ = DialogBtn::kCancel;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
          }
        }

        OverlayButton btn = HitTest(px, py);
        if (btn != OverlayButton::kNone) {
          owner_.pressed_btn_ = btn;
          InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
      }

      case WM_LBUTTONUP: {
        ReleaseCapture();
        int px = LOWORD(lparam);
        int py = HIWORD(lparam);

        if (jump_dialog_open_ && dialog_btn_pressed_ != DialogBtn::kNone) {
          bool wasGo = (dialog_btn_pressed_ == DialogBtn::kGo);
          dialog_btn_pressed_ = DialogBtn::kNone;
          InvalidateRect(hwnd, nullptr, FALSE);
          if (wasGo) {
            RECT goRc, cancelRc;
            GetJumpDialogButtonRects(goRc, cancelRc);
            POINT pt = { px, py };
            if (PtInRect(&goRc, pt)) DoPresetJump();
            else owner_.CloseJumpDialog();
          } else {
            owner_.CloseJumpDialog();
          }
          return 0;
        }

        OverlayButton btn = HitTest(px, py);
        if (btn != OverlayButton::kNone &&
            btn == owner_.pressed_btn_) {
          owner_.ExecuteOverlayAction(btn);
        }
        owner_.pressed_btn_ = OverlayButton::kNone;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }

      case WM_MOUSEMOVE: {
        int px = LOWORD(lparam);
        int py = HIWORD(lparam);
        OverlayButton btn = HitTest(px, py);
        if (btn != owner_.hovered_btn_) {
          owner_.hovered_btn_ = btn;

          if (owner_.hovered_btn_ == OverlayButton::kNone)
            owner_.setMouseCursor(juce::MouseCursor::NormalCursor);
          else
            owner_.setMouseCursor(juce::MouseCursor::PointingHandCursor);

          InvalidateRect(hwnd, nullptr, FALSE);
        }

        // 跟踪 WM_MOUSELEAVE
        TRACKMOUSEEVENT tme = { sizeof(tme) };
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);
        return 0;
      }

      case WM_MOUSELEAVE: {
        owner_.hovered_btn_ = OverlayButton::kNone;
        owner_.setMouseCursor(juce::MouseCursor::NormalCursor);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }

      case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        // 背景
        HBRUSH bgBr = CreateSolidBrush(theme_bg_);
        FillRect(hdc, &rc, bgBr);
        DeleteObject(bgBr);

        // 绘制控件栏按钮
        DrawButton(hdc, OverlayButton::kPin);
        DrawButton(hdc, OverlayButton::kPrev);
        DrawButton(hdc, OverlayButton::kRandom);
        DrawButton(hdc, OverlayButton::kAuto);
        DrawButton(hdc, OverlayButton::kNext);

        // 绘制预设名文本
        DrawPresetName(hdc);

        // 绘制跳转弹窗
        if (jump_dialog_open_) PaintJumpDialog(hdc, rc);

        EndPaint(hwnd, &ps);
        return 0;
      }

      case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }

  // ---- 控件栏按钮矩形 ----
  RECT GetButtonRect(OverlayButton btn) const {
    const int y = (kControlBarHeight - kBtnSize) / 2;

    // 从右侧排列：[?] [>] [A] [presetName center] [<] [📌]
    auto rightBtn = [&](int idxFromRight) -> RECT {
      int x = width_ - (idxFromRight + 1) * (kBtnSize + kBtnGap) - 4;
      return { x, y, x + kBtnSize, y + kBtnSize };
    };

    switch (btn) {
      case OverlayButton::kRandom: return rightBtn(0);      // [?] 最右
      case OverlayButton::kNext:   return rightBtn(1);      // [>]
      case OverlayButton::kAuto:   return rightBtn(2);      // [A]
      // [<] 按钮位置计算：名字区左侧留出间距
      case OverlayButton::kPrev: {
        constexpr int nameSpaceL = kPinBtnW + kBtnGap + 4;
        return { nameSpaceL, y, nameSpaceL + kBtnSize, y + kBtnSize };
      }
      case OverlayButton::kPin: {
        return { 4, y, 4 + kPinBtnW, y + kBtnSize };
      }
      default: return { 0, 0, 0, 0 };
    }
  }

  // ---- GDI 按钮绘制 ----
  void DrawButton(HDC hdc, OverlayButton btn) {
    RECT r = GetButtonRect(btn);
    if (r.right <= r.left) return;

    bool pressed = (owner_.pressed_btn_ == btn);
    bool hovered = (owner_.hovered_btn_ == btn);
    COLORREF bgColor = pressed  ? theme_pink100
                     : hovered ? theme_pink200
                     : theme_btn_normal;

    HBRUSH br = CreateSolidBrush(bgColor);
    FillRect(hdc, &r, br);
    DeleteObject(br);

    if (!pressed) {
      HPEN borderPen = CreatePen(PS_SOLID, 1, theme_pink300);
      HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, borderPen));
      HBRUSH nullBr = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
      HBRUSH oldBr = static_cast<HBRUSH>(SelectObject(hdc, nullBr));
      ::Rectangle(hdc, r.left, r.top, r.right, r.bottom);
      SelectObject(hdc, oldPen);
      SelectObject(hdc, oldBr);
      DeleteObject(borderPen);
    }

    HFONT oldF = static_cast<HFONT>(SelectObject(hdc, font_bold_));
    SetTextColor(hdc, theme_ink);
    SetBkMode(hdc, TRANSPARENT);

    const wchar_t* label = L"";
    switch (btn) {
      case OverlayButton::kPrev:   label = L"<"; break;
      case OverlayButton::kNext:   label = L">"; break;
      case OverlayButton::kRandom: label = L"?"; break;
      case OverlayButton::kAuto:   label = owner_.is_auto_mode_ ? L"A" : L"a"; break;
      case OverlayButton::kPin:    label = owner_.always_on_top_ ? L"📌" : L"📍"; break;
      default: break;
    }
    DrawTextW(hdc, label, -1, &r,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, oldF);
  }

  // ---- 预设名绘制 ----
  void DrawPresetName(HDC hdc) {
    if (preset_display_.empty()) return;

    // 名字区域：在 [<] 按钮右侧和 [A] 按钮左侧之间
    RECT prevR = GetButtonRect(OverlayButton::kPrev);
    RECT autoR = GetButtonRect(OverlayButton::kAuto);
    RECT nameR = { prevR.right + 4, (kControlBarHeight - kBtnSize) / 2,
                   autoR.left - 4, (kControlBarHeight - kBtnSize) / 2 + kBtnSize };

    HFONT oldF = static_cast<HFONT>(SelectObject(hdc, font_plain_));
    SetBkMode(hdc, TRANSPARENT);

    // 索引部分用灰色
    if (preset_idx_part_len_ > 0) {
      SetTextColor(hdc, theme_pink300);
      RECT idxR = nameR;
      DrawTextW(hdc, preset_display_.c_str(), preset_idx_part_len_,
                &idxR, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
      SIZE sz = {};
      GetTextExtentPoint32W(hdc, preset_display_.c_str(),
                            preset_idx_part_len_, &sz);
      nameR.left += sz.cx;
    }

    // 名称部分用亮色
    SetTextColor(hdc, theme_ink);
    DrawTextW(hdc,
              preset_display_.c_str() + preset_idx_part_len_,
              static_cast<int>(preset_display_.size()) - preset_idx_part_len_,
              &nameR, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    SelectObject(hdc, oldF);
  }

  // ---- 跳转弹窗绘制 ----
  void PaintJumpDialog(HDC hdc, RECT clientRc) {
    const int dlgY = kControlBarHeight;
    const int dlgW = clientRc.right - clientRc.left;

    RECT dlgRc = { clientRc.left, dlgY, clientRc.right, dlgY + kJumpDlgHeight };
    HBRUSH dlgBg = CreateSolidBrush(theme_bg_darker);
    FillRect(hdc, &dlgRc, dlgBg);
    DeleteObject(dlgBg);

    // 标题
    RECT titleRc = { clientRc.left + 12, dlgY + 8,
                     clientRc.right - 12, dlgY + 28 };
    HFONT oldF = static_cast<HFONT>(SelectObject(hdc, font_bold_));
    SetTextColor(hdc, theme_ink);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextW(hdc, L"Jump to Preset", -1, &titleRc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, oldF);

    // 提示
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

    // Go 按钮
    RECT goRc = { clientRc.right - 14 - 54, dlgY + 50,
                  clientRc.right - 14, dlgY + 72 };
    bool goPressed = (dialog_btn_pressed_ == DialogBtn::kGo);
    COLORREF goColor = goPressed ? theme_pink100 : theme_btn_normal;
    HBRUSH goBr = CreateSolidBrush(goColor);
    FillRect(hdc, &goRc, goBr);
    DeleteObject(goBr);
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

    // Cancel 按钮
    RECT cancelRc = { goRc.left - 8 - 56, dlgY + 50,
                       goRc.left - 8, dlgY + 72 };
    bool cnPressed = (dialog_btn_pressed_ == DialogBtn::kCancel);
    COLORREF cnColor = cnPressed ? theme_pink100 : theme_btn_normal;
    HBRUSH cnBr = CreateSolidBrush(cnColor);
    FillRect(hdc, &cancelRc, cnBr);
    DeleteObject(cnBr);
    if (!cnPressed) {
      HPEN cnPen = CreatePen(PS_SOLID, 1, theme_pink300);
      HPEN oldPnCn = static_cast<HPEN>(SelectObject(hdc, cnPen));
      HBRUSH nullBrCn = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
      HBRUSH oldBrCn = static_cast<HBRUSH>(SelectObject(hdc, nullBrCn));
      ::Rectangle(hdc, cancelRc.left, cancelRc.top,
                  cancelRc.right, cancelRc.bottom);
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

  // ---- 数据成员 ----
  Milkdrop3Window& owner_;
  HWND  hwnd_       = nullptr;
  HWND  edit_hwnd_  = nullptr;
  int   width_      = 0;
  bool  jump_dialog_open_ = false;
  DialogBtn dialog_btn_pressed_ = DialogBtn::kNone;

  std::wstring preset_display_;
  int preset_idx_part_len_ = 0;
  bool auto_mode_active_ = false;
  bool pin_state_        = false;

  HFONT font_bold_  = nullptr;
  HFONT font_plain_ = nullptr;

  COLORREF theme_bg_        = 0;
  COLORREF theme_bg_darker  = 0;
  COLORREF theme_pink100    = 0;
  COLORREF theme_pink200    = 0;
  COLORREF theme_pink300    = 0;
  COLORREF theme_pink600    = 0;
  COLORREF theme_btn_normal = 0;
  COLORREF theme_btn_face   = 0;
  COLORREF theme_ink        = 0;
};

// ==========================================================================
// Milkdrop3Window 构造 / 析构
// ==========================================================================

Milkdrop3Window::Milkdrop3Window(AnalyserHub* hub)
    : juce::ResizableWindow("MilkDrop3", juce::Colours::black, true),
      api_(milkdrop3_api::Api::Instance()),
      hub_(hub) {
  // 窗口属性
  setResizable(true, true);
  setUsingNativeTitleBar(true);
  setResizeLimits(kMinWidth, kMinHeight, 3840, 2160);
  centreWithSize(kDefaultWidth, kDefaultHeight);

  // 聚焦策略：保持置顶状态可控
  setAlwaysOnTop(false);

  if (hub_) {
    hub_->retain(AnalyserHub::Kind::Oscilloscope);
    hub_->retain(AnalyserHub::Kind::Spectrum);
    hub_->addFrameListener(this);
  }

  // 异步分步初始化，避免在构造函数中做重型操作
  startTimer(5);
}

Milkdrop3Window::~Milkdrop3Window() {
  // 铁律 5：严格 7 步析构顺序
  // 1) 反注册 pre-render injector
  if (audio_injector_token_ != 0) {
    api_.RemovePreRenderInjector(audio_injector_token_);
    audio_injector_token_ = 0;
  }

  // 2) 停止 Timer / 摘 FrameListener
  stopTimer();
  if (hub_) hub_->removeFrameListener(this);

  // 3) 销毁 overlay
  overlay_.reset();

  // 4) 销毁 D3D9 子窗口 + 引擎
  d3d_child_.Destroy();
  if (initialized_) api_.Destroy();

  // 5) 释放 hub Kind 引用计数
  if (hub_) {
    hub_->release(AnalyserHub::Kind::Spectrum);
    hub_->release(AnalyserHub::Kind::Oscilloscope);
  }
}

// ==========================================================================
// ResizableWindow 覆写
// ==========================================================================

void Milkdrop3Window::userTriedToCloseWindow() {
  // 仅隐藏窗口（保留 D3D9 引擎 + 音频注入状态），用户再次点击按钮时复用。
  // 完全销毁见 ~Milkdrop3Window()。
  setVisible(false);
}

void Milkdrop3Window::resized() {
  const auto bounds = getLocalBounds();
  const int clientW = bounds.getWidth();
  const int clientH = bounds.getHeight();

  if (clientW <= 0 || clientH <= 0) return;

  // D3D9 子窗口：填满客户区，使用物理像素
  const juce::Rectangle<int> physRect =
      juce::Desktop::getInstance().getDisplays().logicalToPhysical(
          juce::Rectangle<int>(0, 0, clientW, clientH));
  const int physW = physRect.getWidth();
  const int physH = physRect.getHeight();

  if (initialized_ && d3d_child_.hwnd()) {
    if (physW != last_w_ || physH != last_h_) {
      last_w_ = physW;
      last_h_ = physH;
      d3d_child_.Resize(physW, physH);
    }
  } else if (d3d_child_.hwnd()) {
    // 初始化期间：只调整子窗口大小，不触发 Device Reset
    SetWindowPos(d3d_child_.hwnd(), nullptr, 0, 0, clientW, clientH,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
  }

  // 控件栏 overlay：适配新宽度
  if (overlay_) {
    overlay_->Resize(clientW);
  }
}

// ==========================================================================
// 分阶段异步初始化 + 渲染循环
// ==========================================================================

void Milkdrop3Window::timerCallback() {
  // ---- 已初始化：渲染循环 ----
  if (initialized_) {
    if (!isShowing()) return;

    // 自动轮播
    CheckAutoMode();

    // 预设名称变更检测
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
      // 等待 native peer 就绪
      HWND hwnd = reinterpret_cast<HWND>(getWindowHandle());
      if (!hwnd) return;

      const auto bounds = getLocalBounds();
      const juce::Rectangle<int> physRect =
          juce::Desktop::getInstance().getDisplays().logicalToPhysical(
              bounds);
      last_w_ = physRect.getWidth();
      last_h_ = physRect.getHeight();

      init_phase_ = 0;
      status_message_ = "Phase 0/5: Creating D3D9 child HWND...";
      repaint();
      return;
    }

    case 0: {
      HWND hwnd = reinterpret_cast<HWND>(getWindowHandle());
      if (!d3d_child_.Create(hwnd, last_w_, last_h_)) {
        error_state_ = true;
        error_message_ = "Failed to create D3D9 child window.";
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
      if (!api_.Initialize_CreateRenderWindow(d3d_child_.hwnd(),
                                               last_w_, last_h_)) {
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
      status_message_ = "Phase 3/5: PluginPreInitialize...";
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
      if (!api_.Initialize_CreateDevice(last_w_, last_h_)) {
        error_state_ = true;
        error_message_ = juce::String(api_.GetError());
        stopTimer();
        repaint();
        return;
      }
      init_phase_ = 5;
      status_message_ = "Phase 5/5: PluginInitialize...";
      repaint();
      return;
    }

    case 5: {
      if (!api_.Initialize_PluginInit(last_w_, last_h_)) {
        error_state_ = true;
        error_message_ = juce::String(api_.GetError());
        stopTimer();
        repaint();
        return;
      }

      initialized_ = true;
      init_phase_  = -1;
      status_message_.clear();

      // 创建控件栏 overlay
      {
        HWND hwnd = reinterpret_cast<HWND>(getWindowHandle());
        const auto bounds = getLocalBounds();
        overlay_ = std::make_unique<ControlBarOverlay>(*this);
        if (!overlay_->Create(hwnd, bounds.getWidth())) {
          overlay_.reset();
        }
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
      SyncOverlayContent();

      MD3_LOG("Milkdrop3Window: init complete. preset=%d/%d",
              api_.GetCurrentPresetIndex() + 1,
              api_.GetFileablePresetCount());

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
// AnalyserHub::FrameListener
// ==========================================================================

void Milkdrop3Window::onFrame(const AnalyserHub::FrameSnapshot& frame) {
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

void Milkdrop3Window::FeedEngineFromSnapshot() {
  AudioSnapshot local;
  {
    std::lock_guard<std::mutex> lock(audio_mutex_);
    if (!audio_snapshot_.has_pcm && !audio_snapshot_.has_spectrum) return;
    local = audio_snapshot_;
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

void Milkdrop3Window::NextPreset() {
  if (!initialized_) return;
  api_.NextPreset(2.0f);
  SyncOverlayContent();
  repaint();
}

void Milkdrop3Window::PrevPreset() {
  if (!initialized_) return;
  api_.PrevPreset(2.0f);
  SyncOverlayContent();
  repaint();
}

void Milkdrop3Window::RandomPreset() {
  if (!initialized_) return;
  api_.RandomPreset(2.0f);
  SyncOverlayContent();
  repaint();
}

void Milkdrop3Window::ToggleAutoMode() {
  is_auto_mode_ = !is_auto_mode_;
  last_auto_switch_ms_ = juce::Time::getMillisecondCounter();
  if (overlay_) overlay_->SetAutoMode(is_auto_mode_);
  repaint();
}

void Milkdrop3Window::ToggleAlwaysOnTop() {
  always_on_top_ = !always_on_top_;
  setAlwaysOnTop(always_on_top_);
  if (overlay_) overlay_->SetPinState(always_on_top_);
}

void Milkdrop3Window::AnnouncePresetNameToEngine() {
  const std::wstring name = api_.GetCurrentPresetName();
  if (!name.empty()) {
    api_.ShowPresetTitleAnim(name.c_str());
  }
}

void Milkdrop3Window::SyncOverlayContent() {
  if (!overlay_ || !initialized_) return;

  int idx = api_.GetCurrentPresetIndex();
  int total = api_.GetFileablePresetCount();

  wchar_t buf[512];
  if (total > 0) {
    swprintf_s(buf, L"%d/%d ", idx + 1, total);
  } else {
    wcscpy_s(buf, L"0/0 ");
  }
  std::wstring display(buf);
  int idxPartLen = static_cast<int>(display.size());

  const std::wstring name = api_.GetCurrentPresetName();
  if (!name.empty()) {
    // 只取文件名部分（去掉路径）
    size_t lastSlash = name.find_last_of(L"\\/");
    display += (lastSlash != std::wstring::npos) ? name.substr(lastSlash + 1)
                                                  : name;
  } else {
    display += L"(no preset)";
  }

  overlay_->SetPresetDisplay(display, idxPartLen);
  overlay_->SetAutoMode(is_auto_mode_);
}

juce::String Milkdrop3Window::GetPresetDisplayName() const {
  return juce::String(api_.GetCurrentPresetName().c_str());
}

// ==========================================================================
// 控件栏交互
// ==========================================================================

Milkdrop3Window::OverlayButton Milkdrop3Window::HitTestOverlayBtn(
    int px, int py) const {
  if (overlay_) return overlay_->HitTest(px, py);
  return OverlayButton::kNone;
}

void Milkdrop3Window::ExecuteOverlayAction(OverlayButton btn) {
  switch (btn) {
    case OverlayButton::kPin:  ToggleAlwaysOnTop();  return;
    case OverlayButton::kPrev: PrevPreset();          return;
    case OverlayButton::kNext: NextPreset();          return;
    case OverlayButton::kRandom: RandomPreset();      return;
    case OverlayButton::kAuto: ToggleAutoMode();      return;
    case OverlayButton::kPresetName: ShowPresetJumpDialog(); return;
    case OverlayButton::kNone: return;
  }
}

// ==========================================================================
// 自动轮播
// ==========================================================================

void Milkdrop3Window::CheckAutoMode() {
  if (!is_auto_mode_ || !initialized_) return;
  const juce::uint32 now = juce::Time::getMillisecondCounter();
  if (now - last_auto_switch_ms_ >=
      static_cast<juce::uint32>(auto_interval_secs_ * 1000.0f)) {
    NextPreset();
    last_auto_switch_ms_ = now;
  }
}

void Milkdrop3Window::UpdateAutoIntervalFromSlider(float proportion) {
  auto_interval_secs_ = kMinAutoInterval +
      proportion * (kMaxAutoInterval - kMinAutoInterval);
}

// ==========================================================================
// 预设跳转弹窗
// ==========================================================================

void Milkdrop3Window::ShowPresetJumpDialog() {
  if (!overlay_ || !initialized_ || jump_dialog_open_) return;
  jump_dialog_open_ = true;
  overlay_->ShowJumpDialog(api_.GetFileablePresetCount(),
                            api_.GetCurrentPresetIndex());
}

void Milkdrop3Window::CloseJumpDialog() {
  if (!jump_dialog_open_) return;
  jump_dialog_open_ = false;
  if (overlay_) overlay_->CloseJumpDialog();
}

// ==========================================================================
// 键盘 / 鼠标
// ==========================================================================

bool Milkdrop3Window::keyPressed(const juce::KeyPress& key) {
  if (!initialized_ || error_state_) return false;

  if (key == juce::KeyPress::escapeKey) {
    if (jump_dialog_open_) {
      CloseJumpDialog();
      return true;
    }
    // Esc 不关闭窗口，让 userTriedToCloseWindow 处理
    return false;
  }

  if (jump_dialog_open_) return false;

  if (key == juce::KeyPress::leftKey)  { PrevPreset();   return true; }
  if (key == juce::KeyPress::rightKey) { NextPreset();   return true; }
  if (key == juce::KeyPress::spaceKey) { RandomPreset(); return true; }
  if (key == juce::KeyPress('a'))      { ToggleAutoMode(); return true; }
  return false;
}

void Milkdrop3Window::mouseDown(const juce::MouseEvent& e) {
  if (!initialized_ || error_state_) return;

  if (jump_dialog_open_ && overlay_) {
    // 点击 overlay 之外 → 关闭弹窗
    HWND overlayHwnd = overlay_->GetHwnd();
    if (overlayHwnd) {
      POINT pt = { e.getMouseDownScreenPosition().x,
                   e.getMouseDownScreenPosition().y };
      ScreenToClient(overlayHwnd, &pt);
      OverlayButton btn = overlay_->HitTest(pt.x, pt.y);
      if (btn == OverlayButton::kNone) {
        auto db = overlay_->HitTestJumpDialog(pt.x, pt.y);
        if (db == ControlBarOverlay::DialogBtn::kNone) {
          CloseJumpDialog();
          return;
        }
      }
    }
  }

  // 将点击事件传递给 overlay WndProc 处理
  if (overlay_) {
    HWND overlayHwnd = overlay_->GetHwnd();
    if (overlayHwnd) {
      POINT pt = { e.getMouseDownScreenPosition().x,
                   e.getMouseDownScreenPosition().y };
      ScreenToClient(overlayHwnd, &pt);

      // 点击在 overlay 区域内：通过 SendMessage 模拟点击给 overlay
      RECT rc;
      GetClientRect(overlayHwnd, &rc);
      if (pt.x >= 0 && pt.x < rc.right && pt.y >= 0 && pt.y < rc.bottom) {
        SendMessageW(overlayHwnd, WM_LBUTTONDOWN, MK_LBUTTON,
                     MAKELPARAM(pt.x, pt.y));
      }
    }
  }
}

void Milkdrop3Window::mouseUp(const juce::MouseEvent& e) {
  if (!initialized_ || error_state_) return;
  if (overlay_) {
    HWND overlayHwnd = overlay_->GetHwnd();
    if (overlayHwnd) {
      POINT pt = { e.getMouseDownScreenPosition().x,
                   e.getMouseDownScreenPosition().y };
      ScreenToClient(overlayHwnd, &pt);
      SendMessageW(overlayHwnd, WM_LBUTTONUP, 0, MAKELPARAM(pt.x, pt.y));
    }
  }
}

void Milkdrop3Window::mouseMove(const juce::MouseEvent& e) {
  if (!initialized_ || error_state_) return;
  if (overlay_) {
    HWND overlayHwnd = overlay_->GetHwnd();
    if (overlayHwnd) {
      POINT pt = { e.getScreenPosition().x, e.getScreenPosition().y };
      ScreenToClient(overlayHwnd, &pt);
      SendMessageW(overlayHwnd, WM_MOUSEMOVE, 0, MAKELPARAM(pt.x, pt.y));
    }
  }
}

void Milkdrop3Window::mouseExit(const juce::MouseEvent&) {
  if (!initialized_ || error_state_) return;
  if (overlay_) {
    HWND overlayHwnd = overlay_->GetHwnd();
    if (overlayHwnd)
      SendMessageW(overlayHwnd, WM_MOUSELEAVE, 0, 0);
  }
}

