/*
  ==============================================================================

  Md3DebugLog.h
  Y2Kmeter — MilkDrop3 模块调试日志工具（自 v2.4.0-dev）。

  输出同时写往:
    1. %TEMP%/milkdrop3_debug.log（追加模式，UTF-8）
    2. OutputDebugStringA（VS / DebugView 可捕获）

  用法:
    #include "source/ui/modules/Md3DebugLog.h"

    MD3_LOG("layoutContent: x=%d y=%d w=%d h=%d", x, y, w, h);
    MD3_LOG("MapWindowPoints: in=(%d,%d) out=(%d,%d)", inX, inY, outX, outY);

  线程安全: 使用临界区保护文件写入，OutputDebugString 本身线程安全。

  ==============================================================================
*/
#pragma once

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <ctime>

namespace md3_debug {

void InitLogFile();
void CloseLogFile();
void WriteLog(const char* file, int line, const char* func, const char* fmt, ...);

}  // namespace md3_debug

#define MD3_LOG(fmt, ...) \
  do { \
    md3_debug::InitLogFile(); \
    md3_debug::WriteLog(__FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__); \
  } while (0)
