/*
  ==============================================================================

  Md3DebugLog.cpp
  Y2Kmeter — MilkDrop3 模块调试日志工具实现。

  ==============================================================================
*/

#include "Md3DebugLog.h"

#include <cstdlib>
#include <cstring>

namespace md3_debug {

// ---- 内部状态 ----

static FILE*   g_log_file = nullptr;
static CRITICAL_SECTION g_cs;
static bool    g_cs_inited = false;

// ---- 实现 ----

static const char* ShortFilename(const char* full_path) {
  const char* last = full_path;
  for (const char* p = full_path; *p; ++p) {
    if (*p == '\\' || *p == '/') last = p + 1;
  }
  return last;
}

void InitLogFile() {
  if (g_log_file) return;

  if (!g_cs_inited) {
    InitializeCriticalSection(&g_cs);
    g_cs_inited = true;
  }

  EnterCriticalSection(&g_cs);
  if (!g_log_file) {
    wchar_t path[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, path)) {
      wcscat_s(path, L"milkdrop3_debug.log");
      g_log_file = _wfopen(path, L"a");
    }
  }
  LeaveCriticalSection(&g_cs);
}

void CloseLogFile() {
  if (g_cs_inited) {
    EnterCriticalSection(&g_cs);
    if (g_log_file) {
      fclose(g_log_file);
      g_log_file = nullptr;
    }
    LeaveCriticalSection(&g_cs);
    DeleteCriticalSection(&g_cs);
    g_cs_inited = false;
  }
}

void WriteLog(const char* file, int line, const char* func, const char* fmt, ...) {
  // ---- 时间戳 ----
  SYSTEMTIME st;
  GetLocalTime(&st);

  // ---- 格式化用户消息 ----
  char user_msg[2048] = {};
  va_list args;
  va_start(args, fmt);
  _vsnprintf_s(user_msg, sizeof(user_msg), _TRUNCATE, fmt, args);
  va_end(args);

  // ---- 组装最终行 ----
  const char* short_file = ShortFilename(file);
  char line_buf[2560] = {};
  _snprintf_s(line_buf, sizeof(line_buf), _TRUNCATE,
              "%04d-%02d-%02d %02d:%02d:%02d.%03d [%s:%d] %s: %s\n",
              st.wYear, st.wMonth, st.wDay,
              st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
              short_file, line, func, user_msg);

  // ---- OutputDebugString ----
  OutputDebugStringA(line_buf);

  // ---- 写文件 ----
  if (g_cs_inited) EnterCriticalSection(&g_cs);
  if (g_log_file) {
    fputs(line_buf, g_log_file);
    fflush(g_log_file);
  }
  if (g_cs_inited) LeaveCriticalSection(&g_cs);
}

}  // namespace md3_debug
