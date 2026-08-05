#include "wasabi.h"

#include <stdio.h>

#ifdef MD3_Y2KMETER

// 静态缓冲区，用于 wasabiApiLangString(int) 重载
static wchar_t s_buf[4096];

// MilkDrop3 字符串资源表 —— 映射 resource.h 中的 ID 到英文文本。
// 原始 Winamp 版从 HINSTANCE 资源表加载，Y2Kmeter 构建中不可用。
// 此处按需覆盖 Y2Kmeter 集成期间实际踩到的 ID（主要来自 plugin.rc）。
//
// 注意：MilkDrop3 引擎在渲染时会用这些字符串做 HUD 叠加（如页号、锁状态、
// 预设名、着色器版本等）。若 ID 缺失 → fallback 返回 "[MD3:%d]"，
// swprintf 调用方会将其中的 %d 替换为传入参数值，在 D3D9 画面上显示为
//  "[MD3:512177562]" 之类的乱码。覆盖 ID 越多，HUD 显示越干净。
static const wchar_t* Md3LookupString(int id) {
  switch (id) {
    // ---- 对话框标题 / HUD 前缀 ----
    case  14:  return L"MILKDROP ERROR";
    case  19:  return L"MILKDROP WARNING";
    case 459:  return L"MILKDROP SUGGESTION";

    // ---- OSD / HUD 叠加文字（渲染时画到 D3D9 表面）----
    case   2:  return L"Use UP/DOWN arrow keys to navigate menu.  ";
    case   3:  return L"Untitled menu item";
    case   4:  return L"Untitled menu";
    case   5:  return L"on";
    case   6:  return L"off";
    case   7:  return L"Use UP/DOWN arrow keys.";
    case   8:  return L"current value of '%s'";
    case   9:  return L"Load From File...";
    case  10:  return L"Save To File...";
    case  11:  return L"enter the new string for '%s'";
    case  23:  return L"Rating";
    case  32:  return L" (page %d of %d) ";
    case  33:  return L"<locked> ";
    case 334:  return L"Press F1 for Help ";
    case 607:  return L" Page %d ";

    // ---- 字体 / GDI / D3DX 创建错误 ----
    case  20:  return L"Could not re-create doublesize (gdi-based) title font.";
    case  21:  return L"Could not create doublesize D3DX title font.";
    case 414:  return L"Could not create GDI desktop font.";
    case 415:  return L"Could not create desktop font.";
    case 444:  return L"Error creating the GDI fonts.";
    case 452:  return L"Error creating the D3DX fonts.";

    // ---- D3D9 设备 / 全屏模式 / DX 上下文错误 ----
    case 416:  return L"Error creating texture for Desktop Icon bitmaps.";
    case 418:  return L"Error: desktop icons not available (error creating hook proc).";
    case 447:  return L"Error creating Direct3D device for VJ mode.";
    case 450:  return L"Error creating VJ window.";
    case 451:  return L"Error creating D3D device for VJ mode.";
    case 784:  return L"Unable to init DXContext.";

    // ---- 着色器模型 / 编译 / 回退 ----
    case 467:  return L"Could not create my vertex declaration:\n   %s";
    case 468:  return L"Could not create the wf vertex declaration:\n   %s";
    case 469:  return L"Could not create sprite vertex declaration:\n   %s";
    case 470:  return L"Shader model %d.%d";
    case 471:  return L"Shader model %d.%d";
    case 472:  return L"Shader model %d.%d";
    case 473:  return L"(unknown case: %d)";
    case 474:  return L"Failed to compile pixel shaders using %s [PSVersion=0x%X]";
    case 475:  return L"Failed to compile pixel shaders; hardware mis-report.";
    case 476:  return L"Could not compile fallback warp vertex shader";
    case 477:  return L"Could not compile fallback comp vertex shader";
    case 478:  return L"Could not compile fallback comp pixel shader";
    case 479:  return L"Could not compile blur1 vertex shader";
    case 480:  return L"Could not compile blur1 pixel shader";
    case 481:  return L"Could not compile blur2 vertex shader";
    case 482:  return L"Could not compile blur2 pixel shader";
    case 498:  return L"Error creating shader:\n   %s";
    case 537:  return L"Pixel Shader model 2";
    case 538:  return L"Pixel Shader model 3";

    // ---- 纹理 / 画布 / 噪声 ----
    case 413:  return L"MilkDrop error: file missing:\n   %s";
    case 466:  return L"Unable to read the data file:\n   %s";
    case 483:  return L"Could not create internal canvas texture; reducing display size...";
    case 484:  return L"Could not create internal canvas texture; not enough video memory.  Recommendation: restart Winamp, then go to config screen and lower canvas size.";
    case 485:  return L"Could not create internal canvas texture; not enough video memory.";
    case 486:  return L"Successfully created VS0 and VS1 surfaces (texture size: %d x %d, render size: %d x %d)";
    case 487:  return L"Error creating blur textures:\n   %s";
    case 488:  return L"Could not create noise texture.";
    case 489:  return L"Could not lock noise texture.";
    case 490:  return L"Noise texture byte layout not recognised.";
    case 491:  return L"Could not create 3D noise texture.";
    case 492:  return L"Could not lock 3D noise texture.";
    case 493:  return L"3D noise texture byte layout not recognised.";
    case 560:  return L"Error creating D3DX fonts.";
    case 676:  return L"MILKDROP ERROR";
    case 680:  return L"Could not create my vertex declaration:\n   %s";
    case 681:  return L"Could not create the wf vertex declaration:\n   %s";
    case 682:  return L"Could not create sprite vertex declaration:\n   %s";
    case 690:  return L"Shader model %d.%d";
    case 691:  return L"(unknown case: %d)";
    case 692:  return L"Failed to compile pixel shaders using %s [PSVersion=0x%X]";
    case 693:  return L"Could not compile fallback warp vertex shader";
    case 694:  return L"Could not compile fallback comp vertex shader";
    case 695:  return L"Could not compile fallback comp pixel shader";
    case 696:  return L"Could not compile blur1 vertex shader";
    case 697:  return L"Could not compile blur1 pixel shader";
    case 698:  return L"Could not compile blur2 vertex shader";
    case 699:  return L"Could not compile blur2 pixel shader";

    // ---- 通用 fallback（纯文本，无格式说明符，所有调用场景安全）----
    default:   return L"[MD3]";
  }
}

wchar_t* wasabiApiLangString(int id, wchar_t* out_buffer, int len) {
  swprintf(out_buffer, static_cast<size_t>(len), L"%s", Md3LookupString(id));
  return out_buffer;
}

wchar_t* wasabiApiLangString(int id) {
  swprintf(s_buf, 4096, L"%s", Md3LookupString(id));
  return s_buf;
}

HWND wasabiApiCreateDialogParam(int /*templateName*/, HWND /*parent*/,
                                DLGPROC /*proc*/, LPARAM /*initParam*/) {
  return nullptr;
}

HMENU wasabiApiLoadMenu(int /*menuId*/) {
  return nullptr;
}

#else
// 原始 Winamp 实现：从 Winamp 的 HINSTANCE 加载字符串/菜单资源。
// 在 Y2Kmeter 构建中不编译此路径。

#include <Windows.h>

extern HINSTANCE api_orig_hinstance;

static wchar_t buffer[4096];

wchar_t* wasabiApiLangString(int id, wchar_t* out_buffer, int len) {
  LoadStringW(api_orig_hinstance, id, out_buffer, len);
  return out_buffer;
}

wchar_t* wasabiApiLangString(int id) {
  LoadStringW(api_orig_hinstance, id, buffer, 4096);
  return buffer;
}

HWND wasabiApiCreateDialogParam(int templateName, HWND parent, DLGPROC proc,
                                LPARAM initParam) {
  return CreateDialogParamW(api_orig_hinstance,
                            wasabiApiLangString(templateName),
                            parent, proc, initParam);
}

HMENU wasabiApiLoadMenu(int menuId) {
  return LoadMenuW(api_orig_hinstance, MAKEINTRESOURCEW(menuId));
}

#endif  // MD3_Y2KMETER