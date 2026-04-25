#ifndef PBEQ_LOUDNESS_METER_H_INCLUDED
#define PBEQ_LOUDNESS_METER_H_INCLUDED

// 注意：LoudnessMeter 类的完整声明已统一合并到 AnalyserHub.h 中，
// 以规避 MSVC 多文件同进程编译时的 include guard 跨 TU 串扰。
// 本头保留仅为兼容旧的 include 路径（LoudnessMeter.cpp 等继续可用）。
#include "source/analysis/AnalyserHub.h"

#endif // PBEQ_LOUDNESS_METER_H_INCLUDED