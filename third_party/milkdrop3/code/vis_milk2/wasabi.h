#pragma once

#include <windows.h>

// Y2Kmeter stub: 这些函数在原 MilkDrop3 中用于从 Winamp 的字符串/菜单资源表
// 中加载 UI 字符串。在 Y2Kmeter 环境中，wasabiApiLangString 返回以 ID 格式化
// 的占位字符串（形如 "[MD3:123]"），足够在错误消息中使用。
// wasabiApiCreateDialogParam / wasabiApiLoadMenu 在 Y2Kmeter 中不被调用，
// 返回 nullptr 即可。

wchar_t* wasabiApiLangString(int id, wchar_t* buffer, int len);
wchar_t* wasabiApiLangString(int id);
HMENU wasabiApiLoadMenu(int id);
HWND wasabiApiCreateDialogParam(int templateName, HWND parent, DLGPROC proc,
                                LPARAM initParam);