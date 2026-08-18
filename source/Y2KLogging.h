// ==========================================================
// source/Y2KLogging.h
//   运行时日志开关宏。
//
//   通过编译宏 Y2K_ENABLE_LOGGING 控制：
//     · 仅在 RelWithDebInfo 构建配置下由 CMake 定义该宏
//       （见 CMakeLists.txt 中 target_compile_definitions 的生成器表达式）。
//     · 其余构建（Debug / Release / MinSizeRel）不定义该宏，
//       所有 Y2K_LOG(...) 在预处理阶段直接展开为空表达式，
//       连参数里的字符串拼接、juce::String 构造都不会发生，
//       从根上消除日志对正式包算力的占用。
//
//   用法：
//     Y2K_LOG("[Foo] value=" + juce::String(v));
//
//   注意：
//     · 宏参数必须是纯字符串/拼接表达式，不允许带副作用（如函数调用
//       修改状态），否则在关闭日志的构建中副作用会被一并删除。
//     · 本宏跨多个 .cpp 使用，因此必须在头文件中定义且不能 #undef，
//       属「必要例外」。使用后如需局部取消，请在包含方自行处理。
// ==========================================================

#pragma once

#include <juce_core/juce_core.h>

#ifdef Y2K_ENABLE_LOGGING
  #define Y2K_LOG(msg) juce::Logger::writeToLog(msg)
#else
  #define Y2K_LOG(msg) ((void)0)
#endif
