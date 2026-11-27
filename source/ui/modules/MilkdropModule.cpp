/*
  ==============================================================================

  MilkdropModule.cpp
  Y2Kmeter — Milkdrop 模块（自 2.0.4 起，libprojectM 4 原生实现）

  参见 MilkdropModule.h 顶部注释，尤其是"架构概览"与"生命周期规则"两段。

  ==============================================================================
*/
#include "MilkdropModule.h"
#include "source/i18n/Localisation.h"
#include "ProjectMApi.h"
#if JUCE_MAC
#include "MilkdropModule_mac.h"
#endif
#include "source/ui/PinkXPStyle.h"
#include "source/ui/SkinCraft.h"
#include "source/ui/HoverHelp.h"
#include "source/diag/Y2KDiagLog.h"
#include "source/Y2KLogging.h"
#include "PluginEditor.h"

#include "projectM-4/projectM.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <vector>

#if JUCE_MAC
// 控制栏窗口内的提示气泡：完全由 updateOverlayHoverTip 的轮询驱动。
//   控制栏是独立子 NSWindow，鼠标落在它上面时主窗口收不到鼠标事件，
//   juce::TooltipWindow 自带的 123ms 定时解析在这里既不会更新也不会隐藏
//   （还可能把轮询刚显示的提示立刻收掉）——这里直接禁用其定时器逻辑，
//   只保留 displayTip / hideTip 两个手动接口。
class MilkdropModule::OverlayTipWindow : public juce::TooltipWindow
{
public:
    explicit OverlayTipWindow (juce::Component* parentComponent)
        : juce::TooltipWindow (parentComponent, 500)
    {
        setInterceptsMouseClicks (false, false);
        setLookAndFeel (&getPinkXPLookAndFeel());
    }

    ~OverlayTipWindow() override
    {
        setLookAndFeel (nullptr);
    }

    // TooltipWindow 的定时回调（私有虚函数）：轮询驱动模式下禁用
    void timerCallback() override {}

    void showTip (juce::Point<int> screenPos, const juce::String& text)
    {
        if (text.isNotEmpty())
            displayTip (screenPos, text);
    }

    void hideTipNow() { hideTip(); }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OverlayTipWindow)
};
#endif

// ==========================================================
// v3.0.1：布局锁定只在基类 ModulePanel::mouseDown 内部拦截"拖拽/缩放/关闭"，
//   模块内容按钮（预设切换 / color / 收藏 …）不再受锁定影响 —— 这里原先那份
//   isPanelLayoutLocked() 已删除（它导致 L 锁定后 milkdrop 内部按钮全部失效）。
// ==========================================================
namespace
{
    /**
     * @brief 运行时修正 .milk 预设中 Milkdrop DSL → GLSL 的类型不兼容问题。
     *
     * 原始 Milkdrop (Winamp) 表达式引擎是弱类型的，对空格和类型转换非常宽容。
     * projectM 4 将其翻译为真正的 GLSL，以下三种模式都会导致 shader 编译失败：
     *
     * 1) float2 (0,1)   → 空格在类型和 '(' 之间，GLSL 认为 float2 是未定义变量
     * 2) float3 (b,m,t) → 同上
     * 3) float2 uv2 = ... - float3(a,b,c)  → float2 = float3，类型不匹配
     *
     * 此函数在内存中预处理预设文本，不修改磁盘上的 .milk 文件。
     */
#if JUCE_MAC
    // num_inst 自动限制阈值：**这是 macOS 上的核心性能瓶颈**。
    // 通过 `sample` 工具对 5185_FXSetting...Glow3.milk 的实时线程采样发现：
    //   OpenGL Renderer 线程 33% 时间卡在 CustomShape::Draw() → glDrawArrays
    //   → intelSubmitCommands → mach_msg2_trap（内核陷入提交 GPU 命令）。
    // macOS Intel 集成显卡每 draw call 走一次 IOKit mach_msg 同步，
    // 单次开销约 30~60µs，是 Windows / 独立显卡的 10 倍。
    //
    // 对比实测：
    //   - 5185_FXSetting  (4 shape 全启用, num_inst=1939) → 10 fps
    //   - 5187_Goody      (4 shape 全禁用, num_inst=0)    → 110 fps
    //   两者 warp/comp shader 复杂度、wavecode_samples 均相当，唯一差异
    //   就是 CustomShape 的 draw call 数量。
    //
    // 上限推导：
    //   目标 60fps → 单帧预算 16.7ms
    //   扣除 warp/comp/清屏/composite 等固定开销 ≈ 5ms
    //   剩 11.7ms 给 shape draw call
    //   每 draw call 保守 60µs → 总 num_inst 上限 ≈ 192
    //
    // 采用「单个上限 + 总量上限」双重策略：
    //   1) 每个 shapecode_N_num_inst 单独截断到 kMaxNumInst（防个别极端值）
    //   2) 若截断后所有 shape 的 num_inst 之和仍超过 kMaxTotalNumInst，
    //      按比例整体缩减，保持各 shape 间原有的相对比例。
    //
    // 例如：5185_FXSetting...Glow3.milk 原总量 1939（512+92+311+1024），
    //   1) 单个 clamp 到 96 → 96+92+96+96 = 380
    //   2) 按比例缩到 192   → 48+46+48+48 ≈ 190（每 shape 至少保 1）
    //   预期帧率从 10fps 提升到 55~70fps。
    //
    // 仅 macOS 生效：Windows 端 draw call 开销低一个数量级，无需限制。
    static constexpr int kMaxNumInst      = 96;    // 单个 shape 上限
    static constexpr int kMaxTotalNumInst = 192;   // 所有 shape 总量上限

    // wavecode_N_samples 上限。**辅助优化项**（非主要瓶颈）。
    // wave_N_per_point 表达式在 CPU 上对每个 wave sample 求值，理论上是负担；
    // 但实测证明：5187_Goody 有 4 个 wavecode_samples=512（比 5185 更高），
    // 却能跑 110fps，说明 wave per_point 在 CPU 上的开销远低于
    // GPU 端 CustomShape::Draw 的 draw call 提交开销。
    // 因此这里只做轻度限制（512→256），减少 50% CPU 求值，视觉几乎无损。
    // 仅 macOS 生效。
    static constexpr int kMaxWaveSamples = 256;

    // warp shader 中 GetPixel 采样次数上限。
    // GetPixel 在 projectM 4 中展开为一次完整的 texture2D 采样，
    // 8 次卷积在 1080p 下约 1600 万次采样/帧，是帧率骤降的主因之一。
    // 限制为 4 次后 GPU 负载降低约 50%，视觉效果仍可接受（模糊半径减半）。
    // 仅 macOS 生效：Windows 端不需要此限制。
    static constexpr int kMaxWarpGetPixel = 4;

    // 在字符串 s 中，从 start 位置开始找到 GetPixel( 的完整调用（含括号内参数），
    // 返回整个调用的结束位置（闭合 ')' 之后），找不到返回 std::string::npos。
    static size_t FindGetPixelCallEnd(const std::string& s, size_t start)
    {
      size_t pos = s.find("GetPixel(", start);
      if (pos == std::string::npos)
        return std::string::npos;
      // 找到 GetPixel( 后，追踪括号深度找到匹配的 ')'
      size_t i = pos + 9; // 跳过 "GetPixel("
      int depth = 1;
      while (i < s.size() && depth > 0)
      {
        if (s[i] == '(') ++depth;
        else if (s[i] == ')') --depth;
        ++i;
      }
      return (depth == 0) ? i : std::string::npos; // i 指向闭合 ')' 之后
    }

    // 统计字符串 s 中 GetPixel( 的出现次数
    static int CountGetPixel(const std::string& s)
    {
      int count = 0;
      size_t pos = 0;
      while ((pos = s.find("GetPixel(", pos)) != std::string::npos)
      {
        ++count;
        pos += 9;
      }
      return count;
    }
#endif // JUCE_MAC

    static std::string FixMilkdropShaderTypes(const std::string& data)
    {
#if JUCE_MAC
      // ---- 预处理 1：扫描 shapecode_N_num_inst，构建 shape_index → final_value 映射 ----
      // 策略：
      //   step1) 每个 shape 先按 kMaxNumInst 单独截断得到 clamped[N]
      //   step2) 若 sum(clamped) > kMaxTotalNumInst，按比例缩减：
      //          final[N] = max(1, round(clamped[N] * kMaxTotalNumInst / sum(clamped)))
      //   step3) 否则 final[N] = clamped[N]
      // 主循环遇到 shapecode_N_num_inst= 行时，直接查表改写为 final[N]。
      // 使用 map<int,int> 避免同一 shape_index 多次出现时的重复计算。
      std::map<int, int> finalNumInst;   // shape_index → 最终值
      {
        std::map<int, int> clamped;      // shape_index → 单独截断后的值
        int sumClamped = 0;

        std::istringstream pre(data);
        std::string ln;
        while (std::getline(pre, ln))
        {
          // 快速预筛
          if (ln.find("shapecode_") == std::string::npos
              || ln.find("_num_inst=") == std::string::npos)
            continue;

          // 解析 shapecode_N_num_inst=<数字>
          size_t idxStart = std::strlen("shapecode_");
          if (ln.size() <= idxStart) continue;
          size_t idxEnd = idxStart;
          while (idxEnd < ln.size() && std::isdigit((unsigned char)ln[idxEnd]))
            ++idxEnd;
          if (idxEnd == idxStart) continue;

          // 校验后面紧跟 "_num_inst="
          const std::string kTail = "_num_inst=";
          if (ln.compare(idxEnd, kTail.size(), kTail) != 0)
            continue;

          size_t valStart = idxEnd + kTail.size();
          size_t valEnd = valStart;
          while (valEnd < ln.size() && std::isdigit((unsigned char)ln[valEnd]))
            ++valEnd;
          if (valEnd == valStart) continue;

          int shapeIdx = std::stoi(ln.substr(idxStart, idxEnd - idxStart));
          int val      = std::stoi(ln.substr(valStart, valEnd - valStart));
          int c        = std::min(val, kMaxNumInst);

          // 若同一 shape_index 多次出现（异常预设），取首次
          if (clamped.find(shapeIdx) == clamped.end())
          {
            clamped[shapeIdx] = c;
            sumClamped += c;
          }
        }

        if (sumClamped > kMaxTotalNumInst && sumClamped > 0)
        {
          // 按比例整体缩减
          const double scale = static_cast<double>(kMaxTotalNumInst)
                             / static_cast<double>(sumClamped);
          for (auto& kv : clamped)
          {
            int f = static_cast<int>(std::lround(kv.second * scale));
            if (f < 1) f = 1;              // 保底为 1，避免 num_inst=0 引发除零
            finalNumInst[kv.first] = f;
          }
        }
        else
        {
          finalNumInst = clamped;
        }
      }

      // ---- 预处理 2：统计 warp shader 段内 GetPixel 总次数（仅 macOS）----
      // warp shader 行格式：warp_N=`...（行首为 warp_，含反引号）
      // 需要先统计总数，再决定哪些行需要替换。
      int totalWarpGetPixel = 0;
      {
        std::istringstream pre(data);
        std::string ln;
        while (std::getline(pre, ln))
        {
          // warp 行：以 "warp_" 开头且含 "=`"
          if (ln.size() > 5 && ln.substr(0, 5) == "warp_"
              && ln.find("=`") != std::string::npos)
          {
            totalWarpGetPixel += CountGetPixel(ln);
          }
        }
      }
      // 需要裁剪的 GetPixel 数量（超出阈值的部分）
      const int warpGetPixelToRemove = std::max(0, totalWarpGetPixel - kMaxWarpGetPixel);
      int warpGetPixelRemoved = 0;  // 已裁剪的 GetPixel 计数
#endif // JUCE_MAC

      std::string result;
      result.reserve(data.size() + 512);

      std::istringstream stream(data);
      std::string line;
      while (std::getline(stream, line))
      {
        // ---- A. 修复空格：float2 ( → float2(、float3 ( → float3( ----
        // 只在构造函数调用场景生效（类型后紧跟空格+括号），不影响声明 float2 uv2
        static const std::pair<const char*, const char*> kSpaceFixes[] = {
          {"float2 (",  "float2("},
          {"float3 (",  "float3("},
          {"float2x2 (","float2x2("},
          {"float3x3 (","float3x3("},
          {"float4 (",  "float4("},
          {"float4x4 (","float4x4("},
        };
        for (auto& fix : kSpaceFixes)
        {
          size_t pos = 0;
          while ((pos = line.find(fix.first, pos)) != std::string::npos)
          {
            line.replace(pos, std::strlen(fix.first), fix.second);
            pos += std::strlen(fix.second);
          }
        }

        // ---- B. 修复类型不匹配：float2 声明行里的 float3(...) → float2(...) ----
        if (line.find("float2") != std::string::npos
            && line.find("float3(") != std::string::npos)
        {
          size_t searchPos = 0;
          while ((searchPos = line.find("float3(", searchPos)) != std::string::npos)
          {
            size_t argStart = searchPos + 7; // 跳过 "float3("
            int depth = 1;
            size_t argEnd = argStart;
            while (argEnd < line.size() && depth > 0)
            {
              if (line[argEnd] == '(') ++depth;
              else if (line[argEnd] == ')') --depth;
              ++argEnd;
            }
            --argEnd; // 指向闭合 ')'

            std::string args = line.substr(argStart, argEnd - argStart);

            // 找第二个顶层逗号（跳过嵌套括号），只保留前两个参数
            int nest = 0;
            int commaCount = 0;
            size_t secondComma = std::string::npos;
            for (size_t i = 0; i < args.size(); ++i)
            {
              if (args[i] == '(') ++nest;
              else if (args[i] == ')') --nest;
              else if (args[i] == ',' && nest == 0)
              {
                ++commaCount;
                if (commaCount == 2) { secondComma = i; break; }
              }
            }

            if (secondComma != std::string::npos)
            {
              std::string first2 = args.substr(0, secondComma);
              line.replace(searchPos, argEnd - searchPos + 1,
                           "float2(" + first2 + ")");
            }
            ++searchPos;
          }
        }

        // ---- F. 修复「前两个分量相同」的 swizzle（.xx / .xxy / .xxx / .yy 等）----
        // 对比实测：.xxy 会触发 hlslparser "(N) : Syntax error: expected ';'
        // near '.'" 导致 shader 编译失败；而 .xyy / .zww（重复在后两个分量）正常。
        // 因此只需处理「前两个分量相同」的 swizzle，改写为等价的显式构造函数
        // （语义不变，跨平台生效）。例如 ret.xxy → float3(ret.x, ret.x, ret.y)。
        {
          const std::string kSwizChars = "xyzwrgba";
          size_t pos = 0;
          while (pos < line.size())
          {
            const size_t dot = line.find('.', pos);
            if (dot == std::string::npos)
              break;

            // 跳过小数点（后面紧跟数字）以及非 swizzle 的 '.' 用法
            if (dot + 2 >= line.size()
                || kSwizChars.find(line[dot + 1]) == std::string::npos)
            {
              pos = dot + 1;
              continue;
            }

            // 收集连续 swizzle 字母（最多 4 个分量）
            size_t end = dot + 1;
            while (end < line.size()
                   && (end - dot - 1) < 4
                   && kSwizChars.find(line[end]) != std::string::npos)
              ++end;
            const size_t swizLen = end - (dot + 1);

            // 仅处理「前两个分量相同」的 swizzle（至少 2 个分量）
            if (swizLen < 2 || line[dot + 1] != line[dot + 2])
            {
              pos = dot + 1;
              continue;
            }

            // 向前扫描出被 swizzle 的变量名（标识符 [A-Za-z0-9_]）
            size_t idStart = dot;
            while (idStart > 0
                   && (std::isalnum((unsigned char)line[idStart - 1])
                       || line[idStart - 1] == '_'))
              --idStart;
            const std::string id = line.substr(idStart, dot - idStart);
            if (id.empty())
            {
              pos = dot + 1;
              continue;
            }

            // 构造 floatN(id.c0, id.c1, ...)，例如 .xxy → float3(id.x, id.x, id.y)
            std::string repl = "float" + std::to_string(swizLen) + "(" + id;
            for (size_t i = 0; i < swizLen; ++i)
            {
              repl += '.';
              repl += line[dot + 1 + i];
              if (i + 1 < swizLen)
                repl += ", ";
            }
            repl += ')';

            line.replace(dot, end - dot, repl);
            pos = dot + repl.size();
          }
        }

#if JUCE_MAC
        // ---- C. 限制 shapecode_N_num_inst 过高值，防止 GPU 负载激增（仅 macOS）----
        // 采用「单个上限 + 总量上限」双重策略（详见文件顶部 kMaxNumInst / kMaxTotalNumInst
        // 常量注释）。此处只负责把当前行的 num_inst 值替换为预扫描阶段计算好的
        // finalNumInst[shape_index]。
        // Windows 端模块分离较好，高开销预设不会拖垮软件本体帧率，无需限制。
        {
          // 快速预筛：行中必须同时含有 "shapecode_" 和 "_num_inst="
          size_t idxStart = line.find("shapecode_");
          if (idxStart != std::string::npos)
          {
            idxStart += std::strlen("shapecode_");
            size_t idxEnd = idxStart;
            while (idxEnd < line.size() && std::isdigit((unsigned char)line[idxEnd]))
              ++idxEnd;

            const std::string kTail = "_num_inst=";
            if (idxEnd > idxStart
                && line.compare(idxEnd, kTail.size(), kTail) == 0)
            {
              size_t valStart = idxEnd + kTail.size();
              size_t valEnd = valStart;
              while (valEnd < line.size() && std::isdigit((unsigned char)line[valEnd]))
                ++valEnd;
              if (valEnd > valStart)
              {
                int shapeIdx = std::stoi(line.substr(idxStart, idxEnd - idxStart));
                auto it = finalNumInst.find(shapeIdx);
                if (it != finalNumInst.end())
                {
                  int curVal = std::stoi(line.substr(valStart, valEnd - valStart));
                  if (curVal != it->second)
                  {
                    line.replace(valStart, valEnd - valStart,
                                 std::to_string(it->second));
                  }
                }
              }
            }
          }
        }

        // ---- D. 限制 warp shader 中 GetPixel 采样次数，防止 GPU 纹理采样过载（仅 macOS）----
        // warp 行格式：warp_N=`...（行首为 "warp_"，含 "=`"）
        // GetPixel 在 projectM 4 中展开为一次完整的 texture2D 采样，
        // 8 次卷积在 1080p 下约 1600 万次采样/帧，是帧率骤降的主因之一。
        // 超出阈值的 GetPixel(...) 调用替换为 float3(0.0,0.0,0.0)（零贡献），
        // 等效于减少模糊卷积核的采样点数，视觉上模糊半径略减，性能显著提升。
        // Windows 端不需要此限制。
        if (warpGetPixelRemoved < warpGetPixelToRemove
            && line.size() > 5 && line.substr(0, 5) == "warp_"
            && line.find("=`") != std::string::npos
            && line.find("GetPixel(") != std::string::npos)
        {
          size_t searchPos = 0;
          while (warpGetPixelRemoved < warpGetPixelToRemove)
          {
            size_t callEnd = FindGetPixelCallEnd(line, searchPos);
            if (callEnd == std::string::npos)
              break;
            // 找到 GetPixel( 的起始位置
            size_t callStart = line.rfind("GetPixel(", callEnd);
            if (callStart == std::string::npos || callStart < searchPos)
              break;
            // 替换整个 GetPixel(...) 为 float3(0.0,0.0,0.0)
            line.replace(callStart, callEnd - callStart, "float3(0.0,0.0,0.0)");
            ++warpGetPixelRemoved;
            searchPos = callStart + 19; // 跳过替换后的 "float3(0.0,0.0,0.0)"
          }
        }

        // ---- E. 限制 wavecode_N_samples 过高值，防止 CPU 表达式引擎过载（仅 macOS）----
        // 格式：wavecode_0_samples=512  或  wavecode_10_samples=256
        // wave_N_per_point 表达式对每个 sample 都要单独求值（CPU 端解释执行，
        // 非 GPU shader）。这是 5185_FXSetting / 9604_Pithlit 等高开销预设
        // 从 110fps 骤降到 10fps 的**核心瓶颈**——GPU 并未过载，是 CPU 表达式
        // 引擎跑满导致 JUCE UI 线程 stall。截断到 kMaxWaveSamples 后，
        // 每帧 wave 表达式求值次数减少约 75%，帧率显著回升。
        // Windows 端模块分离较好，无需此限制。
        {
          size_t idxStart = line.find("wavecode_");
          if (idxStart != std::string::npos && idxStart == 0)
          {
            idxStart += std::strlen("wavecode_");
            size_t idxEnd = idxStart;
            while (idxEnd < line.size() && std::isdigit((unsigned char)line[idxEnd]))
              ++idxEnd;

            const std::string kSamplesTail = "_samples=";
            if (idxEnd > idxStart
                && line.compare(idxEnd, kSamplesTail.size(), kSamplesTail) == 0)
            {
              size_t valStart = idxEnd + kSamplesTail.size();
              size_t valEnd = valStart;
              while (valEnd < line.size() && std::isdigit((unsigned char)line[valEnd]))
                ++valEnd;
              if (valEnd > valStart)
              {
                int val = std::stoi(line.substr(valStart, valEnd - valStart));
                if (val > kMaxWaveSamples)
                {
                  line.replace(valStart, valEnd - valStart,
                               std::to_string(kMaxWaveSamples));
                }
              }
            }
          }
        }
#endif // JUCE_MAC

        result += line;
        result += '\n';
      }
      return result;
    }
}

// ==========================================================
// 常量：预设/纹理相对 exe 目录的位置（CMake Post-build 已同步）
// ==========================================================
namespace
{
    constexpr int    kDefaultMeshWidth  = 128;    // projectM 默认 32×24，我们用 128×80，画面更细腻
    constexpr int    kDefaultMeshHeight = 80;
    constexpr int    kTargetFps         = 60;     // 内部动画时基
    constexpr double kPresetDuration    = 20.0;   // 秒
    constexpr double kSoftCutDuration   = 1.0;    // 秒（projectM 预设间视觉渐变过渡时长）

    // 进程内当前已拥有 projectM handle 的 Milkdrop GLView 数。
    // libprojectM 4 (Windows/GLEW) 依赖进程全局的函数指针表，
    // 同一时刻跨多个 juce::OpenGLContext 共存会导致新挂的
    // context 里 GLEW 未重新初始化——表现为 projectm_create 内部跳到
    // 0x0 崩溃。因此运行时硬限 1 个实例（UI 层的"菜单置灰"
    // 只是前置防御；即便布局反序列化或拖拽复制插入了第二个
    // Milkdrop，此处的计数也会拒绝挂 projectM，换为兑底提示。
    //
    // 注：v2.3 GPU 改造后 projectM 由 Editor::newOpenGLContextCreated 创建，
    //     该处有自己的 gEditorProjectMInstances 原子防护。此处只保留注释。

    // 用于 showPresetJumpDialog：enterModalState 是非阻塞的（立即返回），
    // 不能在其后直接 setVisible(true)。此类作为 ModalComponentManager::Callback
    // 在对话框真正退出模态状态时才恢复 GLView 的可见性。
    class GlViewRestorer : public juce::ModalComponentManager::Callback {
    public:
        explicit GlViewRestorer(juce::Component& v) : view(v) {}
        void modalStateFinished(int) override { view.setVisible(true); }
        juce::Component& view;
    };

    static juce::File FindMilkdropAssetsDirForModule(const juce::String& subdir)
    {
      // 判断路径 A 是否是"有效"的资源目录：
      //   · milkdrop_presets: 至少存在 1 个 .milk 文件
      //   · milkdrop_textures: 至少存在 1 个子文件
      // 这个判空规则保证："AppData 目录存在但是空的（例如旧版本残留）"
      // 不会屏蔽掉 bundle 内合法的资源目录。
      auto isValidAssetsDir = [&](const juce::File& d) -> bool {
        if (!d.exists() || !d.isDirectory()) return false;
        if (subdir == "milkdrop_presets") {
          return d.findChildFiles(juce::File::findFiles, false, "*.milk").size() > 0;
        }
        return d.getNumberOfChildFiles(juce::File::findFiles) > 0;
      };

      // 1) 用户数据目录（macOS: ~/Library/Application Support/Y2Kmeter/，
      //    Windows: %APPDATA%\Y2Kmeter\）—— 最高优先级，允许用户自行替换预设。
      //    仅当目录"有效"（含 .milk / 子文件）时才使用；空目录会继续 fallback，
      //    避免旧版本残留空目录屏蔽 bundle 内合法资源。
      juce::File appDataDir = juce::File::getSpecialLocation(
          juce::File::userApplicationDataDirectory)
          .getChildFile("Y2Kmeter")
          .getChildFile(subdir);
      if (isValidAssetsDir(appDataDir))
        return appDataDir;

     #if defined (__APPLE__)
      // macOS Seed 机制（v2.4+）：
      //   VST3/AU bundle 中不再内置 milkdrop_presets（DMG 瘦身约 500 MB），
      //   而是由 Standalone 首次启动时把 bundle 内的 presets 复制到 AppData，
      //   三端（Standalone / VST3 / AU）通过 AppData 共享同一份预设，
      //   用户手动增删预设立即对三端生效。
      //   此 lambda：若 bundle 内 srcDir 有效且 AppData 目标目录为空/不存在，
      //   同步复制到 AppData 并返回 AppData 路径；否则返回原 srcDir。
      auto seedToAppDataIfNeeded = [&](const juce::File& srcDir) -> juce::File {
        if (!isValidAssetsDir(srcDir))
          return srcDir;
        // 只对 milkdrop_presets 做 seed（textures 三端 bundle 各自内置，无需共享）
        if (subdir != "milkdrop_presets")
          return srcDir;
        // AppData 已有有效内容 —— 上面已经 return 了，这里 AppData 一定无效
        // （不存在 / 空目录 / 无 .milk）。触发一次性 seed 复制。
        auto parent = appDataDir.getParentDirectory();
        if (!parent.exists())
          parent.createDirectory();
        // copyDirectoryTo 会在目标不存在时创建目标目录。若目标存在（空目录），
        // 会把源目录内容合并进去。macOS 上大约需要 3-5 秒复制 ~200 MB。
        if (srcDir.copyDirectoryTo(appDataDir) && isValidAssetsDir(appDataDir))
          return appDataDir;
        return srcDir;
      };

      // v2.9.6：安装包将 milkdrop_presets 压缩为单文件 zip（约 1 万个 .milk → 1 个
      //   zip）以大幅加速 pkg 安装。这里负责：bundle 内只有 zip（无目录）时，
      //   首次启动解压 seed 到 AppData。zip 顶层直接是 .milk 文件，故解压目标
      //   是 appDataDir 本身（而非其父目录）。
      auto seedZipToAppDataIfNeeded = [&](const juce::File& zipFile) -> juce::File {
        auto parent = appDataDir.getParentDirectory();
        if (!parent.exists())
          parent.createDirectory();
        if (!appDataDir.exists())
          appDataDir.createDirectory();
        juce::ZipFile zf (zipFile);
        if (zf.uncompressTo (appDataDir, true).wasOk() && isValidAssetsDir (appDataDir))
          return appDataDir;
        return appDataDir;
      };
     #endif

     #if defined (__APPLE__)
      // 2) macOS bundle 内置路径：
      //    可执行文件位于 Y2Kmeter.app/Contents/MacOS/Y2Kmeter，
      //    资源打包在    Y2Kmeter.app/Contents/Resources/assets/<subdir>
      //    currentExecutableFile → .../Contents/MacOS/Y2Kmeter
      //    getParentDirectory()  → .../Contents/MacOS
      //    getParentDirectory()  → .../Contents
      {
        auto contentsDir = juce::File::getSpecialLocation(
            juce::File::currentExecutableFile)
            .getParentDirectory()   // MacOS/
            .getParentDirectory();  // Contents/
        auto bundleCandidate = contentsDir
            .getChildFile("Resources")
            .getChildFile("assets")
            .getChildFile(subdir);
        if (isValidAssetsDir(bundleCandidate))
          return seedToAppDataIfNeeded(bundleCandidate);
        // 安装包压缩形态：目录被压缩成 milkdrop_presets.zip
        if (subdir == "milkdrop_presets")
        {
          auto zipCandidate = contentsDir
              .getChildFile("Resources")
              .getChildFile("assets")
              .getChildFile("milkdrop_presets.zip");
          if (zipCandidate.existsAsFile())
            return seedZipToAppDataIfNeeded(zipCandidate);
        }
      }
      // 3) VST3 / AU bundle 内置路径：
      //    Y2Kmeter.vst3/Contents/MacOS/Y2Kmeter.vst3（或 .component）
      //    资源打包在 Y2Kmeter.vst3/Contents/Resources/assets/<subdir>
      //    同上逻辑，currentApplicationFile 指向 bundle 根，
      //    再进入 Contents/Resources/assets/
      //    注意：v2.4+ VST3/AU bundle 已剥离 milkdrop_presets（走共享 AppData），
      //    只保留 milkdrop_textures 副本；此分支仅对 textures 命中。
      {
        auto appFile = juce::File::getSpecialLocation(
            juce::File::currentApplicationFile);
        auto pluginContents = appFile.getChildFile("Contents");
        if (pluginContents.isDirectory())
        {
          auto pluginCandidate = pluginContents
              .getChildFile("Resources")
              .getChildFile("assets")
              .getChildFile(subdir);
          if (isValidAssetsDir(pluginCandidate))
            return seedToAppDataIfNeeded(pluginCandidate);
          // 安装包压缩形态：目录被压缩成 milkdrop_presets.zip
          if (subdir == "milkdrop_presets")
          {
            auto pluginZip = pluginContents
                .getChildFile("Resources")
                .getChildFile("assets")
                .getChildFile("milkdrop_presets.zip");
            if (pluginZip.existsAsFile())
              return seedZipToAppDataIfNeeded(pluginZip);
          }
        }
      }
     #endif

      // 4) 开发期兜底：从可执行文件目录向上逐级查找 assets/<subdir>
      //    （Windows 生产环境也走此路径，exe 旁边有 assets/ 目录）
      juce::File exeDir = juce::File::getSpecialLocation(
          juce::File::currentExecutableFile).getParentDirectory();
      juce::File cur = exeDir;
      for (int i = 0; i < 8; ++i)
      {
        auto candidate = cur.getChildFile("assets").getChildFile(subdir);
        if (isValidAssetsDir(candidate))
          return candidate;
        cur = cur.getParentDirectory();
      }
      return {};
    }

    // 收藏库目录：固定位于用户数据目录 Y2Kmeter/milkdrop_presets_like，
    // 与内置 milkdrop_presets 同级。GLView 浮动态 / macOS 本地渲染路径使用。
    static juce::File FindMilkdropLikeDirForModule()
    {
      return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
          .getChildFile("Y2Kmeter")
          .getChildFile("milkdrop_presets_like");
    }
}

// ==========================================================
// MilkdropTintPass —— projectM 输出帧的整体染色后处理着色器
// ==========================================================
MilkdropTintPass::MilkdropTintPass (juce::OpenGLContext& context)
    : context_ (context)
{
}

bool MilkdropTintPass::init()
{
    if (ready_)
        return true;

    coreProfile_ = context_.isCoreProfile();
    program_.reset (new juce::OpenGLShaderProgram (context_));

    juce::String vertexSource;
    juce::String fragmentSource;

    if (coreProfile_)
    {
        vertexSource =
            "#version 150\n"
            "in vec2 aPos;\n"
            "out vec2 vUV;\n"
            "void main() {\n"
            "  vUV = aPos * 0.5 + 0.5;\n"
            "  gl_Position = vec4(aPos, 0.0, 1.0);\n"
            "}\n";
        fragmentSource =
            "#version 150\n"
            "in vec2 vUV;\n"
            "out vec4 fragColor;\n"
            "uniform sampler2D uTex;\n"
            "uniform vec3 uTint;\n"
            "uniform float uBrightness;\n"
            "uniform float uInvert;\n"
            "uniform float uShadows;\n"
            "uniform float uShadowsStrength;\n"
            "uniform float uSolarize;\n"
            "uniform float uSplit;\n"
            "uniform float uZoom;\n"
            "uniform float uMulti;\n"
            "uniform float uRainbow;\n"
            "uniform float uBlow;\n"
            "uniform float uBurn;\n"
            "uniform float uKaleidoscope;\n"
            "uniform float uSwirl;\n"
            "uniform float uPinch;\n"
            "uniform float uPixelate;\n"
            "uniform float uGlitch;\n"
            "uniform float uPosterize;\n"
            "uniform float uSepia;\n"
            "uniform float uGrayscale;\n"
            "uniform float uEdge;\n"
            "uniform float uVignette;\n"
            "uniform float uTunnel;\n"
            "uniform float uRipple;\n"
            "uniform float uMelt;\n"
            "uniform float uFisheye;\n"
            "uniform float uNoiseWarp;\n"
            "uniform float uMirrorMaze;\n"
            "uniform float uFragment;\n"
            "uniform float uSpiral;\n"
            "uniform float uTwist;\n"
            "uniform float uColorShift;\n"
            "uniform float uNeon;\n"
            "uniform float uThermal;\n"
            "uniform float uAcid;\n"
            "uniform float uVhs;\n"
            "uniform float uCrt;\n"
            "uniform float uDuotone;\n"
            "uniform float uBloom;\n"
            "uniform float uBinary;\n"
            "uniform float uPrismatic;\n"
            "uniform float uTweakZoom;\n"
            "uniform float uTweakRot;\n"
            "uniform float uTweakWarp;\n"
            "uniform float uTweakDx;\n"
            "uniform float uTweakDy;\n"
            "uniform float uTweakSx;\n"
            "uniform float uTweakSy;\n"
            "uniform float uTweakKaleido;\n"
            "uniform float uTweakFoldX;\n"
            "uniform float uTweakFoldY;\n"
            "void main() {\n"
            "  vec2 uv = vUV;\n"
            "  // tweak：后处理 uv 几何畸变（连续强度，0 = 中性，实时生效，不修改预设）。\n"
            "  {\n"
            "    vec2 c = vec2(0.5, 0.5);\n"
            "    vec2 p = uv - c;\n"
            "    float z = 1.0 + uTweakZoom * 4.0;\n"
            "    p /= max(z, 0.01);\n"
            "    float ang = uTweakRot * 6.28318;\n"
            "    float s = sin(ang);\n"
            "    float co = cos(ang);\n"
            "    p = vec2(p.x * co - p.y * s, p.x * s + p.y * co);\n"
            "    p.x *= exp(-uTweakSx * 1.5);\n"
            "    p.y *= exp(-uTweakSy * 1.5);\n"
            "    p += vec2(uTweakDx, uTweakDy);\n"
            "    uv = p + c;\n"
            "    if (abs(uTweakWarp) > 0.001) {\n"
            "      vec2 w = uv - c;\n"
            "      float r = length(w);\n"
            "      float a = atan(w.y, w.x) + uTweakWarp * r * 12.0;\n"
            "      uv = vec2(cos(a), sin(a)) * r + c;\n"
            "    }\n"
            "    // 万花镜整数对称控制器：kaleido（角度）+ fold_x/fold_y（笛卡尔网格）。\n"
            "    if (uTweakKaleido >= 1.0) {\n"
            "      float segs = floor(uTweakKaleido + 0.5);\n"
            "      vec2 kp = uv - 0.5;\n"
            "      float kr = length(kp);\n"
            "      float ka = atan(kp.y, kp.x);\n"
            "      float wedge = 3.14159265 / segs;\n"
            "      ka = abs(mod(ka, wedge * 2.0) - wedge);\n"
            "      uv = vec2(cos(ka), sin(ka)) * kr + 0.5;\n"
            "    }\n"
            "    if (uTweakFoldX >= 1.0) {\n"
            "      float n = floor(uTweakFoldX + 0.5);\n"
            "      uv.x = abs(fract(uv.x * n) * 2.0 - 1.0);\n"
            "    }\n"
            "    if (uTweakFoldY >= 1.0) {\n"
            "      float n = floor(uTweakFoldY + 0.5);\n"
            "      uv.y = abs(fract(uv.y * n) * 2.0 - 1.0);\n"
            "    }\n"
            "  }\n"
            "  // efftop：采样前 uv 重映射（split → zoom → multi → kaleidoscope → swirl → pinch → pixelate → 第三批）。\n"
            "  if (uSplit > 0.5) uv = vec2(abs(uv.x - 0.5), uv.y);\n"
            "  if (uZoom > 0.5) uv = 0.25 + 0.5 * uv;\n"
            "  if (uMulti > 0.5) uv = 1.0 - abs(fract((uv - 0.5) * 0.75) * 2.0 - 1.0);\n"
            "  if (uKaleidoscope > 0.5) {\n"
            "    vec2 p = uv - 0.5;\n"
            "    float r = length(p);\n"
            "    float a = atan(p.y, p.x);\n"
            "    float seg = 3.14159265 / 3.0;\n"
            "    a = abs(mod(a, seg * 2.0) - seg);\n"
            "    uv = vec2(cos(a), sin(a)) * r + 0.5;\n"
            "  }\n"
            "  if (uSwirl > 0.5) {\n"
            "    vec2 p = uv - 0.5;\n"
            "    float r = length(p);\n"
            "    float a = atan(p.y, p.x) + (1.0 - r) * 3.0;\n"
            "    uv = vec2(cos(a), sin(a)) * r + 0.5;\n"
            "  }\n"
            "  if (uPinch > 0.5) {\n"
            "    vec2 p = uv - 0.5;\n"
            "    float r = length(p);\n"
            "    uv = p * (0.3 + 0.7 * r) / max(r, 0.0001) + 0.5;\n"
            "  }\n"
            "  if (uPixelate > 0.5) uv = floor(uv * 24.0) / 24.0 + vec2(1.0 / 48.0);\n"
            "  // 第三批 efftop：隧道/涟漪/融化/鱼眼/噪波/镜像/破碎/螺旋/扭转。\n"
            "  if (uTunnel > 0.5) {\n"
            "    vec2 p = uv - 0.5;\n"
            "    float r = length(p);\n"
            "    float a = atan(p.y, p.x);\n"
            "    float depth = 1.0 / (0.3 + r * 2.0);\n"
            "    uv = vec2(a / 3.14159265 * 0.5 + 0.5, depth);\n"
            "  }\n"
            "  if (uRipple > 0.5) {\n"
            "    vec2 p = uv - 0.5;\n"
            "    float r = length(p);\n"
            "    uv += p * sin(r * 30.0) * 0.06;\n"
            "  }\n"
            "  if (uMelt > 0.5) uv.y += (1.0 - uv.y) * sin(uv.x * 20.0) * 0.25;\n"
            "  if (uFisheye > 0.5) {\n"
            "    vec2 p = uv - 0.5;\n"
            "    float r = length(p);\n"
            "    uv = p * (1.0 + 0.8 * r * r) + 0.5;\n"
            "  }\n"
            "  if (uNoiseWarp > 0.5) {\n"
            "    float nx = sin(vUV.y * 40.0 + vUV.x * 20.0);\n"
            "    float ny = cos(vUV.x * 35.0 - vUV.y * 25.0);\n"
            "    uv += vec2(nx, ny) * 0.04;\n"
            "  }\n"
            "  if (uMirrorMaze > 0.5) uv = abs(fract(uv * 3.0) * 2.0 - 1.0);\n"
            "  if (uFragment > 0.5) {\n"
            "    vec2 cell = floor(uv * 12.0);\n"
            "    uv = fract(uv * 12.0) / 12.0 + cell / 12.0;\n"
            "    float h = fract(sin(dot(cell, vec2(127.1, 311.7))) * 43758.5453);\n"
            "    uv.x += (h - 0.5) * 0.3;\n"
            "  }\n"
            "  if (uSpiral > 0.5) {\n"
            "    vec2 p = uv - 0.5;\n"
            "    float r = length(p);\n"
            "    float a = atan(p.y, p.x) + r * 8.0;\n"
            "    uv = vec2(cos(a), sin(a)) * r + 0.5;\n"
            "  }\n"
            "  if (uTwist > 0.5) {\n"
            "    vec2 p = uv - 0.5;\n"
            "    float a = p.y * 6.0;\n"
            "    float s = sin(a);\n"
            "    float co = cos(a);\n"
            "    uv = vec2(p.x * co - p.y * s, p.x * s + p.y * co) + 0.5;\n"
            "  }\n"
            "  vec4 c = texture(uTex, uv);\n"
            "  // 加性偏移：uTint=(1,1,1) 为中性点，调高让整体（含黑色）偏向该色。\n"
            "  c.rgb += (uTint - vec3(1.0));\n"
            "  c.rgb = max(c.rgb, vec3(0.0));\n"
            "  // bright：纯线性增益（对齐 MilkDrop3 的 ret *= brightness）。\n"
            "  c.rgb *= uBrightness;\n"
            "  // shadows：上下翻转 + 灰度 + pow + 加性叠加（对齐 MilkDrop3 shadow 注入效果）。\n"
            "  // 注意：是 += 加法而非乘法，画面不变暗，只叠加黑白镜像纹理。\n"
            "  if (uShadows > 0.5) {\n"
            "    float gray = texture(uTex, vec2(vUV.x, 1.0 - vUV.y)).g;\n"
            "    float shadow = pow(gray, 2.0);\n"
            "    c.rgb += shadow * uShadowsStrength;\n"
            "  }\n"
            "  // invert：反相 / 负片（ret = 1 - ret）。\n"
            "  if (uInvert > 0.5) c.rgb = vec3(1.0) - c.rgb;\n"
            "  // solarize：曝光反转（ret = ret*(1-ret)*4）。\n"
            "  if (uSolarize > 0.5) c.rgb = c.rgb * (vec3(1.0) - c.rgb) * 4.0;\n"
            "  // rainbow：程序化彩虹染色（原版依赖噪声纹理，此处降级为水平彩虹渐变）。\n"
            "  if (uRainbow > 0.5) {\n"
            "    vec3 rainbow = 0.5 + 0.5 * cos(6.28318 * (vUV.x + vec3(0.0, 0.3333, 0.6667)));\n"
            "    c.rgb = mix(c.rgb, rainbow, 0.6);\n"
            "  }\n"
            "  // blow：加性模糊叠加（近似 MilkDrop3 的 ret += GetBlur1(uv)）。\n"
            "  if (uBlow > 0.5) {\n"
            "    vec3 blur = texture(uTex, vUV).rgb;\n"
            "    blur += texture(uTex, vUV + vec2(0.02, 0.0)).rgb;\n"
            "    blur += texture(uTex, vUV - vec2(0.02, 0.0)).rgb;\n"
            "    blur += texture(uTex, vUV + vec2(0.0, 0.02)).rgb;\n"
            "    blur += texture(uTex, vUV - vec2(0.0, 0.02)).rgb;\n"
            "    blur /= 5.0;\n"
            "    c.rgb += blur * 0.4;\n"
            "  }\n"
            "  // burn：灼烧混合（color burn 近似）。\n"
            "  if (uBurn > 0.5) {\n"
            "    vec3 d = texture(uTex, vUV).rgb;\n"
            "    vec3 burned = vec3(1.0) - (vec3(1.0) - c.rgb) / max(d + 0.001, vec3(0.001));\n"
            "    c.rgb = mix(c.rgb, burned, 0.7);\n"
            "  }\n"
            "  // glitch：故障色差，RGB 通道沿 x 微偏移重新采样。\n"
            "  if (uGlitch > 0.5) {\n"
            "    vec2 off = vec2(0.015, 0.0);\n"
            "    c.r = texture(uTex, vUV + off).r;\n"
            "    c.b = texture(uTex, vUV - off).b;\n"
            "  }\n"
            "  // posterize：色调分离，ret 量化成 8 级。\n"
            "  if (uPosterize > 0.5) c.rgb = floor(c.rgb * 8.0) / 8.0;\n"
            "  // sepia：复古棕褐颜色矩阵。\n"
            "  if (uSepia > 0.5) {\n"
            "    vec3 s;\n"
            "    s.r = dot(c.rgb, vec3(0.393, 0.769, 0.189));\n"
            "    s.g = dot(c.rgb, vec3(0.349, 0.686, 0.168));\n"
            "    s.b = dot(c.rgb, vec3(0.272, 0.534, 0.131));\n"
            "    c.rgb = mix(c.rgb, s, 0.85);\n"
            "  }\n"
            "  // grayscale：灰度化（亮度加权）。\n"
            "  if (uGrayscale > 0.5) {\n"
            "    float l = dot(c.rgb, vec3(0.299, 0.587, 0.114));\n"
            "    c.rgb = vec3(l);\n"
            "  }\n"
            "  // edge：邻域差分边缘检测（亮线 / 浮雕感）。\n"
            "  if (uEdge > 0.5) {\n"
            "    float o = 0.005;\n"
            "    float tl = dot(texture(uTex, vUV + vec2(-o, -o)).rgb, vec3(0.333));\n"
            "    float tr = dot(texture(uTex, vUV + vec2(o, -o)).rgb, vec3(0.333));\n"
            "    float bl = dot(texture(uTex, vUV + vec2(-o, o)).rgb, vec3(0.333));\n"
            "    float br = dot(texture(uTex, vUV + vec2(o, o)).rgb, vec3(0.333));\n"
            "    float gx = tr + br - tl - bl;\n"
            "    float gy = bl + br - tl - tr;\n"
            "    float e = clamp(sqrt(gx * gx + gy * gy), 0.0, 1.0);\n"
            "    c.rgb = mix(c.rgb, vec3(1.0 - e), 0.8);\n"
            "  }\n"
            "  // vignette：强暗角 + 桶形畸变扭曲。\n"
            "  if (uVignette > 0.5) {\n"
            "    vec2 vp = vUV - 0.5;\n"
            "    float vr2 = dot(vp, vp);\n"
            "    vec2 warped_uv = vUV + vp * vr2 * 0.4;\n"
            "    vec3 warped = texture(uTex, warped_uv).rgb;\n"
            "    c.rgb = mix(c.rgb, warped, 0.5);\n"
            "    float vd = length(vp);\n"
            "    float vig = smoothstep(0.2, 0.72, vd);\n"
            "    c.rgb *= 1.0 - vig * vig * 0.95;\n"
            "  }\n"
            "  // 第三批 effbottom：色彩爆炸/霓虹/热成像/酸性/VHS/CRT/双色调/泛光/二值化/棱镜。\n"
            "  if (uColorShift > 0.5) {\n"
            "    vec3 shifted;\n"
            "    shifted.r = texture(uTex, vUV + vec2(0.03, 0.0)).r;\n"
            "    shifted.g = c.g;\n"
            "    shifted.b = texture(uTex, vUV - vec2(0.03, 0.0)).b;\n"
            "    c.rgb = shifted * 1.6;\n"
            "  }\n"
            "  if (uNeon > 0.5) {\n"
            "    vec3 glow = texture(uTex, vUV + vec2(0.01, 0.0)).rgb;\n"
            "    glow += texture(uTex, vUV - vec2(0.01, 0.0)).rgb;\n"
            "    glow += texture(uTex, vUV + vec2(0.0, 0.01)).rgb;\n"
            "    glow += texture(uTex, vUV - vec2(0.0, 0.01)).rgb;\n"
            "    glow /= 4.0;\n"
            "    vec3 bright = max(c.rgb - vec3(0.5), vec3(0.0)) * 2.0;\n"
            "    c.rgb = bright + glow * glow;\n"
            "  }\n"
            "  if (uThermal > 0.5) {\n"
            "    float l = dot(c.rgb, vec3(0.299, 0.587, 0.114));\n"
            "    vec3 heat = mix(vec3(0.0, 0.0, 1.0), vec3(1.0, 0.0, 0.0), l);\n"
            "    heat = mix(heat, vec3(1.0, 1.0, 0.0), smoothstep(0.5, 0.8, l));\n"
            "    c.rgb = heat;\n"
            "  }\n"
            "  if (uAcid > 0.5) {\n"
            "    c.rgb = abs(c.rgb - vec3(0.5)) * 2.0;\n"
            "    c.g *= 1.4;\n"
            "    c.b *= 0.7;\n"
            "  }\n"
            "  if (uVhs > 0.5) {\n"
            "    float band = step(0.9, fract(vUV.y * 20.0 + sin(vUV.x * 10.0)));\n"
            "    vec3 off = vec3(0.02, 0.0, -0.02) * band;\n"
            "    c.r = texture(uTex, vUV + off.xx).r;\n"
            "    c.g = texture(uTex, vUV + off.yy).g;\n"
            "    c.b = texture(uTex, vUV + off.zz).b;\n"
            "    c.rgb += sin(vUV.y * 500.0) * 0.1;\n"
            "  }\n"
            "  if (uCrt > 0.5) {\n"
            "    c.rgb *= 0.85 + 0.15 * sin(vUV.y * 300.0);\n"
            "    if (fract(vUV.y * 120.0) < 0.5) c.rgb *= 0.8;\n"
            "  }\n"
            "  if (uDuotone > 0.5) {\n"
            "    float l = dot(c.rgb, vec3(0.299, 0.587, 0.114));\n"
            "    c.rgb = mix(vec3(0.1, 0.0, 0.4), vec3(1.0, 0.8, 0.1), l);\n"
            "  }\n"
            "  if (uBloom > 0.5) {\n"
            "    vec3 blur = texture(uTex, vUV).rgb;\n"
            "    blur += texture(uTex, vUV + vec2(0.015, 0.0)).rgb;\n"
            "    blur += texture(uTex, vUV - vec2(0.015, 0.0)).rgb;\n"
            "    blur += texture(uTex, vUV + vec2(0.0, 0.015)).rgb;\n"
            "    blur += texture(uTex, vUV - vec2(0.0, 0.015)).rgb;\n"
            "    blur /= 5.0;\n"
            "    vec3 bright = max(c.rgb - vec3(0.6), vec3(0.0));\n"
            "    c.rgb += bright * blur * 3.0;\n"
            "  }\n"
            "  if (uBinary > 0.5) {\n"
            "    float l = dot(c.rgb, vec3(0.299, 0.587, 0.114));\n"
            "    c.rgb = step(0.5, l) > 0.5 ? vec3(1.0) : vec3(0.0);\n"
            "  }\n"
            "  if (uPrismatic > 0.5) {\n"
            "    vec2 p = vUV - 0.5;\n"
            "    float d = length(p);\n"
            "    vec2 dir = p / max(d, 0.0001);\n"
            "    c.r = texture(uTex, vUV + dir * 0.02).r;\n"
            "    c.b = texture(uTex, vUV - dir * 0.02).b;\n"
            "    c.rgb += vec3(0.1, 0.0, 0.1);\n"
            "  }\n"
            "  c.rgb = clamp(c.rgb, 0.0, 1.0);\n"
            "  fragColor = c;\n"
            "}\n";
    }
    else
    {
        vertexSource =
            "#version 120\n"
            "attribute vec2 aPos;\n"
            "varying vec2 vUV;\n"
            "void main() {\n"
            "  vUV = aPos * 0.5 + 0.5;\n"
            "  gl_Position = vec4(aPos, 0.0, 1.0);\n"
            "}\n";
        fragmentSource =
            "#version 120\n"
            "varying vec2 vUV;\n"
            "uniform sampler2D uTex;\n"
            "uniform vec3 uTint;\n"
            "uniform float uBrightness;\n"
            "uniform float uInvert;\n"
            "uniform float uShadows;\n"
            "uniform float uShadowsStrength;\n"
            "uniform float uSolarize;\n"
            "uniform float uSplit;\n"
            "uniform float uZoom;\n"
            "uniform float uMulti;\n"
            "uniform float uRainbow;\n"
            "uniform float uBlow;\n"
            "uniform float uBurn;\n"
            "uniform float uKaleidoscope;\n"
            "uniform float uSwirl;\n"
            "uniform float uPinch;\n"
            "uniform float uPixelate;\n"
            "uniform float uGlitch;\n"
            "uniform float uPosterize;\n"
            "uniform float uSepia;\n"
            "uniform float uGrayscale;\n"
            "uniform float uEdge;\n"
            "uniform float uVignette;\n"
            "uniform float uTunnel;\n"
            "uniform float uRipple;\n"
            "uniform float uMelt;\n"
            "uniform float uFisheye;\n"
            "uniform float uNoiseWarp;\n"
            "uniform float uMirrorMaze;\n"
            "uniform float uFragment;\n"
            "uniform float uSpiral;\n"
            "uniform float uTwist;\n"
            "uniform float uColorShift;\n"
            "uniform float uNeon;\n"
            "uniform float uThermal;\n"
            "uniform float uAcid;\n"
            "uniform float uVhs;\n"
            "uniform float uCrt;\n"
            "uniform float uDuotone;\n"
            "uniform float uBloom;\n"
            "uniform float uBinary;\n"
            "uniform float uPrismatic;\n"
            "uniform float uTweakZoom;\n"
            "uniform float uTweakRot;\n"
            "uniform float uTweakWarp;\n"
            "uniform float uTweakDx;\n"
            "uniform float uTweakDy;\n"
            "uniform float uTweakSx;\n"
            "uniform float uTweakSy;\n"
            "uniform float uTweakKaleido;\n"
            "uniform float uTweakFoldX;\n"
            "uniform float uTweakFoldY;\n"
            "void main() {\n"
            "  vec2 uv = vUV;\n"
            "  // tweak：后处理 uv 几何畸变（连续强度，0 = 中性，实时生效，不修改预设）。\n"
            "  {\n"
            "    vec2 c = vec2(0.5, 0.5);\n"
            "    vec2 p = uv - c;\n"
            "    float z = 1.0 + uTweakZoom * 4.0;\n"
            "    p /= max(z, 0.01);\n"
            "    float ang = uTweakRot * 6.28318;\n"
            "    float s = sin(ang);\n"
            "    float co = cos(ang);\n"
            "    p = vec2(p.x * co - p.y * s, p.x * s + p.y * co);\n"
            "    p.x *= exp(-uTweakSx * 1.5);\n"
            "    p.y *= exp(-uTweakSy * 1.5);\n"
            "    p += vec2(uTweakDx, uTweakDy);\n"
            "    uv = p + c;\n"
            "    if (abs(uTweakWarp) > 0.001) {\n"
            "      vec2 w = uv - c;\n"
            "      float r = length(w);\n"
            "      float a = atan(w.y, w.x) + uTweakWarp * r * 12.0;\n"
            "      uv = vec2(cos(a), sin(a)) * r + c;\n"
            "    }\n"
            "    // 万花镜整数对称控制器：kaleido（角度）+ fold_x/fold_y（笛卡尔网格）。\n"
            "    if (uTweakKaleido >= 1.0) {\n"
            "      float segs = floor(uTweakKaleido + 0.5);\n"
            "      vec2 kp = uv - 0.5;\n"
            "      float kr = length(kp);\n"
            "      float ka = atan(kp.y, kp.x);\n"
            "      float wedge = 3.14159265 / segs;\n"
            "      ka = abs(mod(ka, wedge * 2.0) - wedge);\n"
            "      uv = vec2(cos(ka), sin(ka)) * kr + 0.5;\n"
            "    }\n"
            "    if (uTweakFoldX >= 1.0) {\n"
            "      float n = floor(uTweakFoldX + 0.5);\n"
            "      uv.x = abs(fract(uv.x * n) * 2.0 - 1.0);\n"
            "    }\n"
            "    if (uTweakFoldY >= 1.0) {\n"
            "      float n = floor(uTweakFoldY + 0.5);\n"
            "      uv.y = abs(fract(uv.y * n) * 2.0 - 1.0);\n"
            "    }\n"
            "  }\n"
            "  // efftop：采样前 uv 重映射（split → zoom → multi → kaleidoscope → swirl → pinch → pixelate → 第三批）。\n"
            "  if (uSplit > 0.5) uv = vec2(abs(uv.x - 0.5), uv.y);\n"
            "  if (uZoom > 0.5) uv = 0.25 + 0.5 * uv;\n"
            "  if (uMulti > 0.5) uv = 1.0 - abs(fract((uv - 0.5) * 0.75) * 2.0 - 1.0);\n"
            "  if (uKaleidoscope > 0.5) {\n"
            "    vec2 p = uv - 0.5;\n"
            "    float r = length(p);\n"
            "    float a = atan(p.y, p.x);\n"
            "    float seg = 3.14159265 / 3.0;\n"
            "    a = abs(mod(a, seg * 2.0) - seg);\n"
            "    uv = vec2(cos(a), sin(a)) * r + 0.5;\n"
            "  }\n"
            "  if (uSwirl > 0.5) {\n"
            "    vec2 p = uv - 0.5;\n"
            "    float r = length(p);\n"
            "    float a = atan(p.y, p.x) + (1.0 - r) * 3.0;\n"
            "    uv = vec2(cos(a), sin(a)) * r + 0.5;\n"
            "  }\n"
            "  if (uPinch > 0.5) {\n"
            "    vec2 p = uv - 0.5;\n"
            "    float r = length(p);\n"
            "    uv = p * (0.3 + 0.7 * r) / max(r, 0.0001) + 0.5;\n"
            "  }\n"
            "  if (uPixelate > 0.5) uv = floor(uv * 24.0) / 24.0 + vec2(1.0 / 48.0);\n"
            "  // 第三批 efftop：隧道/涟漪/融化/鱼眼/噪波/镜像/破碎/螺旋/扭转。\n"
            "  if (uTunnel > 0.5) {\n"
            "    vec2 p = uv - 0.5;\n"
            "    float r = length(p);\n"
            "    float a = atan(p.y, p.x);\n"
            "    float depth = 1.0 / (0.3 + r * 2.0);\n"
            "    uv = vec2(a / 3.14159265 * 0.5 + 0.5, depth);\n"
            "  }\n"
            "  if (uRipple > 0.5) {\n"
            "    vec2 p = uv - 0.5;\n"
            "    float r = length(p);\n"
            "    uv += p * sin(r * 30.0) * 0.06;\n"
            "  }\n"
            "  if (uMelt > 0.5) uv.y += (1.0 - uv.y) * sin(uv.x * 20.0) * 0.25;\n"
            "  if (uFisheye > 0.5) {\n"
            "    vec2 p = uv - 0.5;\n"
            "    float r = length(p);\n"
            "    uv = p * (1.0 + 0.8 * r * r) + 0.5;\n"
            "  }\n"
            "  if (uNoiseWarp > 0.5) {\n"
            "    float nx = sin(vUV.y * 40.0 + vUV.x * 20.0);\n"
            "    float ny = cos(vUV.x * 35.0 - vUV.y * 25.0);\n"
            "    uv += vec2(nx, ny) * 0.04;\n"
            "  }\n"
            "  if (uMirrorMaze > 0.5) uv = abs(fract(uv * 3.0) * 2.0 - 1.0);\n"
            "  if (uFragment > 0.5) {\n"
            "    vec2 cell = floor(uv * 12.0);\n"
            "    uv = fract(uv * 12.0) / 12.0 + cell / 12.0;\n"
            "    float h = fract(sin(dot(cell, vec2(127.1, 311.7))) * 43758.5453);\n"
            "    uv.x += (h - 0.5) * 0.3;\n"
            "  }\n"
            "  if (uSpiral > 0.5) {\n"
            "    vec2 p = uv - 0.5;\n"
            "    float r = length(p);\n"
            "    float a = atan(p.y, p.x) + r * 8.0;\n"
            "    uv = vec2(cos(a), sin(a)) * r + 0.5;\n"
            "  }\n"
            "  if (uTwist > 0.5) {\n"
            "    vec2 p = uv - 0.5;\n"
            "    float a = p.y * 6.0;\n"
            "    float s = sin(a);\n"
            "    float co = cos(a);\n"
            "    uv = vec2(p.x * co - p.y * s, p.x * s + p.y * co) + 0.5;\n"
            "  }\n"
            "  vec4 c = texture2D(uTex, uv);\n"
            "  // 加性偏移：uTint=(1,1,1) 为中性点，调高让整体（含黑色）偏向该色。\n"
            "  c.rgb += (uTint - vec3(1.0));\n"
            "  c.rgb = max(c.rgb, vec3(0.0));\n"
            "  // bright：纯线性增益（对齐 MilkDrop3 的 ret *= brightness）。\n"
            "  c.rgb *= uBrightness;\n"
            "  // shadows：上下翻转 + 灰度 + pow + 加性叠加（对齐 MilkDrop3 shadow 注入效果）。\n"
            "  // 注意：是 += 加法而非乘法，画面不变暗，只叠加黑白镜像纹理。\n"
            "  if (uShadows > 0.5) {\n"
            "    float gray = texture2D(uTex, vec2(vUV.x, 1.0 - vUV.y)).g;\n"
            "    float shadow = pow(gray, 2.0);\n"
            "    c.rgb += shadow * uShadowsStrength;\n"
            "  }\n"
            "  // invert：反相 / 负片（ret = 1 - ret）。\n"
            "  if (uInvert > 0.5) c.rgb = vec3(1.0) - c.rgb;\n"
            "  // solarize：曝光反转（ret = ret*(1-ret)*4）。\n"
            "  if (uSolarize > 0.5) c.rgb = c.rgb * (vec3(1.0) - c.rgb) * 4.0;\n"
            "  // rainbow：程序化彩虹染色（原版依赖噪声纹理，此处降级为水平彩虹渐变）。\n"
            "  if (uRainbow > 0.5) {\n"
            "    vec3 rainbow = 0.5 + 0.5 * cos(6.28318 * (vUV.x + vec3(0.0, 0.3333, 0.6667)));\n"
            "    c.rgb = mix(c.rgb, rainbow, 0.6);\n"
            "  }\n"
            "  // blow：加性模糊叠加（近似 MilkDrop3 的 ret += GetBlur1(uv)）。\n"
            "  if (uBlow > 0.5) {\n"
            "    vec3 blur = texture2D(uTex, vUV).rgb;\n"
            "    blur += texture2D(uTex, vUV + vec2(0.02, 0.0)).rgb;\n"
            "    blur += texture2D(uTex, vUV - vec2(0.02, 0.0)).rgb;\n"
            "    blur += texture2D(uTex, vUV + vec2(0.0, 0.02)).rgb;\n"
            "    blur += texture2D(uTex, vUV - vec2(0.0, 0.02)).rgb;\n"
            "    blur /= 5.0;\n"
            "    c.rgb += blur * 0.4;\n"
            "  }\n"
            "  // burn：灼烧混合（color burn 近似）。\n"
            "  if (uBurn > 0.5) {\n"
            "    vec3 d = texture2D(uTex, vUV).rgb;\n"
            "    vec3 burned = vec3(1.0) - (vec3(1.0) - c.rgb) / max(d + 0.001, vec3(0.001));\n"
            "    c.rgb = mix(c.rgb, burned, 0.7);\n"
            "  }\n"
            "  // glitch：故障色差，RGB 通道沿 x 微偏移重新采样。\n"
            "  if (uGlitch > 0.5) {\n"
            "    vec2 off = vec2(0.015, 0.0);\n"
            "    c.r = texture2D(uTex, vUV + off).r;\n"
            "    c.b = texture2D(uTex, vUV - off).b;\n"
            "  }\n"
            "  // posterize：色调分离，ret 量化成 8 级。\n"
            "  if (uPosterize > 0.5) c.rgb = floor(c.rgb * 8.0) / 8.0;\n"
            "  // sepia：复古棕褐颜色矩阵。\n"
            "  if (uSepia > 0.5) {\n"
            "    vec3 s;\n"
            "    s.r = dot(c.rgb, vec3(0.393, 0.769, 0.189));\n"
            "    s.g = dot(c.rgb, vec3(0.349, 0.686, 0.168));\n"
            "    s.b = dot(c.rgb, vec3(0.272, 0.534, 0.131));\n"
            "    c.rgb = mix(c.rgb, s, 0.85);\n"
            "  }\n"
            "  // grayscale：灰度化（亮度加权）。\n"
            "  if (uGrayscale > 0.5) {\n"
            "    float l = dot(c.rgb, vec3(0.299, 0.587, 0.114));\n"
            "    c.rgb = vec3(l);\n"
            "  }\n"
            "  // edge：邻域差分边缘检测（亮线 / 浮雕感）。\n"
            "  if (uEdge > 0.5) {\n"
            "    float o = 0.005;\n"
            "    float tl = dot(texture2D(uTex, vUV + vec2(-o, -o)).rgb, vec3(0.333));\n"
            "    float tr = dot(texture2D(uTex, vUV + vec2(o, -o)).rgb, vec3(0.333));\n"
            "    float bl = dot(texture2D(uTex, vUV + vec2(-o, o)).rgb, vec3(0.333));\n"
            "    float br = dot(texture2D(uTex, vUV + vec2(o, o)).rgb, vec3(0.333));\n"
            "    float gx = tr + br - tl - bl;\n"
            "    float gy = bl + br - tl - tr;\n"
            "    float e = clamp(sqrt(gx * gx + gy * gy), 0.0, 1.0);\n"
            "    c.rgb = mix(c.rgb, vec3(1.0 - e), 0.8);\n"
            "  }\n"
            "  // vignette：强暗角 + 桶形畸变扭曲。\n"
            "  if (uVignette > 0.5) {\n"
            "    vec2 vp = vUV - 0.5;\n"
            "    float vr2 = dot(vp, vp);\n"
            "    vec2 warped_uv = vUV + vp * vr2 * 0.4;\n"
            "    vec3 warped = texture2D(uTex, warped_uv).rgb;\n"
            "    c.rgb = mix(c.rgb, warped, 0.5);\n"
            "    float vd = length(vp);\n"
            "    float vig = smoothstep(0.2, 0.72, vd);\n"
            "    c.rgb *= 1.0 - vig * vig * 0.95;\n"
            "  }\n"
            "  // 第三批 effbottom：色彩爆炸/霓虹/热成像/酸性/VHS/CRT/双色调/泛光/二值化/棱镜。\n"
            "  if (uColorShift > 0.5) {\n"
            "    vec3 shifted;\n"
            "    shifted.r = texture2D(uTex, vUV + vec2(0.03, 0.0)).r;\n"
            "    shifted.g = c.g;\n"
            "    shifted.b = texture2D(uTex, vUV - vec2(0.03, 0.0)).b;\n"
            "    c.rgb = shifted * 1.6;\n"
            "  }\n"
            "  if (uNeon > 0.5) {\n"
            "    vec3 glow = texture2D(uTex, vUV + vec2(0.01, 0.0)).rgb;\n"
            "    glow += texture2D(uTex, vUV - vec2(0.01, 0.0)).rgb;\n"
            "    glow += texture2D(uTex, vUV + vec2(0.0, 0.01)).rgb;\n"
            "    glow += texture2D(uTex, vUV - vec2(0.0, 0.01)).rgb;\n"
            "    glow /= 4.0;\n"
            "    vec3 bright = max(c.rgb - vec3(0.5), vec3(0.0)) * 2.0;\n"
            "    c.rgb = bright + glow * glow;\n"
            "  }\n"
            "  if (uThermal > 0.5) {\n"
            "    float l = dot(c.rgb, vec3(0.299, 0.587, 0.114));\n"
            "    vec3 heat = mix(vec3(0.0, 0.0, 1.0), vec3(1.0, 0.0, 0.0), l);\n"
            "    heat = mix(heat, vec3(1.0, 1.0, 0.0), smoothstep(0.5, 0.8, l));\n"
            "    c.rgb = heat;\n"
            "  }\n"
            "  if (uAcid > 0.5) {\n"
            "    c.rgb = abs(c.rgb - vec3(0.5)) * 2.0;\n"
            "    c.g *= 1.4;\n"
            "    c.b *= 0.7;\n"
            "  }\n"
            "  if (uVhs > 0.5) {\n"
            "    float band = step(0.9, fract(vUV.y * 20.0 + sin(vUV.x * 10.0)));\n"
            "    vec3 off = vec3(0.02, 0.0, -0.02) * band;\n"
            "    c.r = texture2D(uTex, vUV + off.xx).r;\n"
            "    c.g = texture2D(uTex, vUV + off.yy).g;\n"
            "    c.b = texture2D(uTex, vUV + off.zz).b;\n"
            "    c.rgb += sin(vUV.y * 500.0) * 0.1;\n"
            "  }\n"
            "  if (uCrt > 0.5) {\n"
            "    c.rgb *= 0.85 + 0.15 * sin(vUV.y * 300.0);\n"
            "    if (fract(vUV.y * 120.0) < 0.5) c.rgb *= 0.8;\n"
            "  }\n"
            "  if (uDuotone > 0.5) {\n"
            "    float l = dot(c.rgb, vec3(0.299, 0.587, 0.114));\n"
            "    c.rgb = mix(vec3(0.1, 0.0, 0.4), vec3(1.0, 0.8, 0.1), l);\n"
            "  }\n"
            "  if (uBloom > 0.5) {\n"
            "    vec3 blur = texture2D(uTex, vUV).rgb;\n"
            "    blur += texture2D(uTex, vUV + vec2(0.015, 0.0)).rgb;\n"
            "    blur += texture2D(uTex, vUV - vec2(0.015, 0.0)).rgb;\n"
            "    blur += texture2D(uTex, vUV + vec2(0.0, 0.015)).rgb;\n"
            "    blur += texture2D(uTex, vUV - vec2(0.0, 0.015)).rgb;\n"
            "    blur /= 5.0;\n"
            "    vec3 bright = max(c.rgb - vec3(0.6), vec3(0.0));\n"
            "    c.rgb += bright * blur * 3.0;\n"
            "  }\n"
            "  if (uBinary > 0.5) {\n"
            "    float l = dot(c.rgb, vec3(0.299, 0.587, 0.114));\n"
            "    c.rgb = step(0.5, l) > 0.5 ? vec3(1.0) : vec3(0.0);\n"
            "  }\n"
            "  if (uPrismatic > 0.5) {\n"
            "    vec2 p = vUV - 0.5;\n"
            "    float d = length(p);\n"
            "    vec2 dir = p / max(d, 0.0001);\n"
            "    c.r = texture2D(uTex, vUV + dir * 0.02).r;\n"
            "    c.b = texture2D(uTex, vUV - dir * 0.02).b;\n"
            "    c.rgb += vec3(0.1, 0.0, 0.1);\n"
            "  }\n"
            "  c.rgb = clamp(c.rgb, 0.0, 1.0);\n"
            "  gl_FragColor = c;\n"
            "}\n";
    }

    // ---- shader 版本适配（v2.9.9 fix）----
    //   v2.9.7 起 Editor GL 强制请求 3.2（openGLContext.setOpenGLVersionRequired
    //   (openGL3_2)），因此在 3.2 上下文里必须把 GLSL 120 语法换成 150。但
    //   translate*ShaderToV3() 会**自己追加一行 #version 150**：源里本来就带
    //   #version（core 分支 150 / legacy 分支 120）时，就变成两行 #version 的
    //   非法 shader → link 失败 → pass 被静默 reset → Milkdrop 所有自定义后处理
    //   （染色 / 亮度 / 效果开关 / tweak 偏移）全部不生效。
    //   正确做法：源里已有的 #version 行先剥掉，只有 legacy 源在 GL>=3.2 时才翻译。
    auto stripVersionDirective = [] (const juce::String& src) -> juce::String
    {
        auto s = src.trimStart();
        if (! s.startsWith ("#version"))
            return src;
        const int nl = s.indexOfChar ('\n');
        return nl >= 0 ? s.substring (nl + 1) : juce::String();
    };

    // 当前上下文的 GL 版本是否 >= 3.2（决定 JUCE 会不会真的做 V3 翻译；
    //   translate*ShaderToV3 在 GL<3.2 时原样返回，剥掉 #version 后会导致
    //   语法与声明不匹配，所以只在会翻译的前提下才剥）。
    auto glVersionAtLeast32 = []() -> bool
    {
        const auto* raw = reinterpret_cast<const char*> (
            juce::gl::glGetString (juce::gl::GL_VERSION));
        if (raw == nullptr)
            return false;

        const juce::String v (raw);
        const int major = v.upToFirstOccurrenceOf (".", false, false).getIntValue();
        const int minor = v.fromFirstOccurrenceOf (".", false, false)
                           .upToFirstOccurrenceOf (".", false, false).getIntValue();
        return major > 3 || (major == 3 && minor >= 2);
    };

    juce::String vs = vertexSource;
    juce::String fs = fragmentSource;

    if (! coreProfile_ && glVersionAtLeast32())
    {
        vs = juce::OpenGLHelpers::translateVertexShaderToV3 (stripVersionDirective (vertexSource));
        fs = juce::OpenGLHelpers::translateFragmentShaderToV3 (stripVersionDirective (fragmentSource));
    }

    if (!program_->addVertexShader (vs)
        || !program_->addFragmentShader (fs)
        || !program_->link())
    {
        lastError_ = program_->getLastError();
        program_.reset();
        return false;
    }

    texLoc_             = program_->getUniformIDFromName ("uTex");
    tintLoc_            = program_->getUniformIDFromName ("uTint");
    brightnessLoc_      = program_->getUniformIDFromName ("uBrightness");
    invertLoc_          = program_->getUniformIDFromName ("uInvert");
    shadowsLoc_         = program_->getUniformIDFromName ("uShadows");
    shadowsStrengthLoc_ = program_->getUniformIDFromName ("uShadowsStrength");
    solarizeLoc_        = program_->getUniformIDFromName ("uSolarize");
    splitLoc_           = program_->getUniformIDFromName ("uSplit");
    zoomLoc_            = program_->getUniformIDFromName ("uZoom");
    multiLoc_           = program_->getUniformIDFromName ("uMulti");
    rainbowLoc_         = program_->getUniformIDFromName ("uRainbow");
    blowLoc_            = program_->getUniformIDFromName ("uBlow");
    burnLoc_            = program_->getUniformIDFromName ("uBurn");
    kaleidoscopeLoc_    = program_->getUniformIDFromName ("uKaleidoscope");
    swirlLoc_           = program_->getUniformIDFromName ("uSwirl");
    pinchLoc_           = program_->getUniformIDFromName ("uPinch");
    pixelateLoc_        = program_->getUniformIDFromName ("uPixelate");
    glitchLoc_          = program_->getUniformIDFromName ("uGlitch");
    posterizeLoc_       = program_->getUniformIDFromName ("uPosterize");
    sepiaLoc_           = program_->getUniformIDFromName ("uSepia");
    grayscaleLoc_       = program_->getUniformIDFromName ("uGrayscale");
    edgeLoc_            = program_->getUniformIDFromName ("uEdge");
    vignetteLoc_        = program_->getUniformIDFromName ("uVignette");
    tunnelLoc_          = program_->getUniformIDFromName ("uTunnel");
    rippleLoc_          = program_->getUniformIDFromName ("uRipple");
    meltLoc_            = program_->getUniformIDFromName ("uMelt");
    fisheyeLoc_         = program_->getUniformIDFromName ("uFisheye");
    noiseWarpLoc_       = program_->getUniformIDFromName ("uNoiseWarp");
    mirrorMazeLoc_      = program_->getUniformIDFromName ("uMirrorMaze");
    fragmentLoc_        = program_->getUniformIDFromName ("uFragment");
    spiralLoc_          = program_->getUniformIDFromName ("uSpiral");
    twistLoc_           = program_->getUniformIDFromName ("uTwist");
    colorShiftLoc_      = program_->getUniformIDFromName ("uColorShift");
    neonLoc_            = program_->getUniformIDFromName ("uNeon");
    thermalLoc_         = program_->getUniformIDFromName ("uThermal");
    acidLoc_            = program_->getUniformIDFromName ("uAcid");
    vhsLoc_             = program_->getUniformIDFromName ("uVhs");
    crtLoc_             = program_->getUniformIDFromName ("uCrt");
    duotoneLoc_         = program_->getUniformIDFromName ("uDuotone");
    bloomLoc_           = program_->getUniformIDFromName ("uBloom");
    binaryLoc_          = program_->getUniformIDFromName ("uBinary");
    prismaticLoc_       = program_->getUniformIDFromName ("uPrismatic");
    tweakZoomLoc_       = program_->getUniformIDFromName ("uTweakZoom");
    tweakRotLoc_        = program_->getUniformIDFromName ("uTweakRot");
    tweakWarpLoc_       = program_->getUniformIDFromName ("uTweakWarp");
    tweakDxLoc_         = program_->getUniformIDFromName ("uTweakDx");
    tweakDyLoc_         = program_->getUniformIDFromName ("uTweakDy");
    tweakSxLoc_         = program_->getUniformIDFromName ("uTweakSx");
    tweakSyLoc_         = program_->getUniformIDFromName ("uTweakSy");
    tweakKaleidoLoc_    = program_->getUniformIDFromName ("uTweakKaleido");
    tweakFoldXLoc_      = program_->getUniformIDFromName ("uTweakFoldX");
    tweakFoldYLoc_      = program_->getUniformIDFromName ("uTweakFoldY");

    // 全屏三角形（单个大三角形覆盖整个视口，顶点为 NDC 坐标）
    static const float kQuadVertices[6] = {
        -1.0f, -1.0f,
         3.0f, -1.0f,
        -1.0f,  3.0f
    };

    juce::gl::glGenBuffers (1, &vbo_);
    juce::gl::glBindBuffer (juce::gl::GL_ARRAY_BUFFER, vbo_);
    juce::gl::glBufferData (juce::gl::GL_ARRAY_BUFFER,
                            static_cast<GLsizeiptr> (sizeof (kQuadVertices)),
                            kQuadVertices, juce::gl::GL_STATIC_DRAW);

    if (coreProfile_)
    {
        juce::gl::glGenVertexArrays (1, &vao_);
        juce::gl::glBindVertexArray (vao_);
        juce::gl::glBindBuffer (juce::gl::GL_ARRAY_BUFFER, vbo_);
        juce::gl::glEnableVertexAttribArray (0);
        juce::gl::glVertexAttribPointer (0, 2, juce::gl::GL_FLOAT,
                                         juce::gl::GL_FALSE, 0, nullptr);
        juce::gl::glBindVertexArray (0);
    }
    juce::gl::glBindBuffer (juce::gl::GL_ARRAY_BUFFER, 0);

    ready_ = true;
    return true;
}

void MilkdropTintPass::shutdown()
{
    if (vao_ != 0)
    {
        juce::gl::glDeleteVertexArrays (1, &vao_);
        vao_ = 0;
    }
    if (vbo_ != 0)
    {
        juce::gl::glDeleteBuffers (1, &vbo_);
        vbo_ = 0;
    }
    if (program_ != nullptr)
    {
        program_->release();
        program_.reset();
    }
    ready_ = false;
}

bool MilkdropTintPass::apply (GLuint srcTex, const MilkdropVisualState& state)
{
    // 返回 false = 这一帧没画成。调用方必须据此把 post FBO 判为无效，
    //   回退到 projectM 原始输出；否则会把未被写入的空 FBO 当有效画面 blit（纯黑）。
    if (!ready_ || program_ == nullptr)
        return false;

    program_->use();

    // 纹理单元 0：当前帧 projectM 输出
    juce::gl::glActiveTexture (juce::gl::GL_TEXTURE0);
    juce::gl::glBindTexture (juce::gl::GL_TEXTURE_2D, srcTex);
    juce::gl::glUniform1i (texLoc_, 0);

    juce::gl::glUniform3f (tintLoc_, state.tint_r, state.tint_g, state.tint_b);
    juce::gl::glUniform1f (brightnessLoc_, state.brightness);
    juce::gl::glUniform1f (invertLoc_, state.invert ? 1.0f : 0.0f);
    juce::gl::glUniform1f (shadowsLoc_, state.shadows ? 1.0f : 0.0f);
    juce::gl::glUniform1f (shadowsStrengthLoc_, 0.8f);  // shadows 加性叠加强度
    juce::gl::glUniform1f (solarizeLoc_, state.solarize ? 1.0f : 0.0f);
    juce::gl::glUniform1f (splitLoc_, state.split ? 1.0f : 0.0f);
    juce::gl::glUniform1f (zoomLoc_, state.zoom ? 1.0f : 0.0f);
    juce::gl::glUniform1f (multiLoc_, state.multi ? 1.0f : 0.0f);
    juce::gl::glUniform1f (rainbowLoc_, state.rainbow ? 1.0f : 0.0f);
    juce::gl::glUniform1f (blowLoc_, state.blow ? 1.0f : 0.0f);
    juce::gl::glUniform1f (burnLoc_, state.burn ? 1.0f : 0.0f);
    juce::gl::glUniform1f (kaleidoscopeLoc_, state.kaleidoscope ? 1.0f : 0.0f);
    juce::gl::glUniform1f (swirlLoc_, state.swirl ? 1.0f : 0.0f);
    juce::gl::glUniform1f (pinchLoc_, state.pinch ? 1.0f : 0.0f);
    juce::gl::glUniform1f (pixelateLoc_, state.pixelate ? 1.0f : 0.0f);
    juce::gl::glUniform1f (glitchLoc_, state.glitch ? 1.0f : 0.0f);
    juce::gl::glUniform1f (posterizeLoc_, state.posterize ? 1.0f : 0.0f);
    juce::gl::glUniform1f (sepiaLoc_, state.sepia ? 1.0f : 0.0f);
    juce::gl::glUniform1f (grayscaleLoc_, state.grayscale ? 1.0f : 0.0f);
    juce::gl::glUniform1f (edgeLoc_, state.edge ? 1.0f : 0.0f);
    juce::gl::glUniform1f (vignetteLoc_, state.vignette ? 1.0f : 0.0f);
    juce::gl::glUniform1f (tunnelLoc_, state.tunnel ? 1.0f : 0.0f);
    juce::gl::glUniform1f (rippleLoc_, state.ripple ? 1.0f : 0.0f);
    juce::gl::glUniform1f (meltLoc_, state.melt ? 1.0f : 0.0f);
    juce::gl::glUniform1f (fisheyeLoc_, state.fisheye ? 1.0f : 0.0f);
    juce::gl::glUniform1f (noiseWarpLoc_, state.noise_warp ? 1.0f : 0.0f);
    juce::gl::glUniform1f (mirrorMazeLoc_, state.mirror_maze ? 1.0f : 0.0f);
    juce::gl::glUniform1f (fragmentLoc_, state.fragment ? 1.0f : 0.0f);
    juce::gl::glUniform1f (spiralLoc_, state.spiral ? 1.0f : 0.0f);
    juce::gl::glUniform1f (twistLoc_, state.twist ? 1.0f : 0.0f);
    juce::gl::glUniform1f (colorShiftLoc_, state.color_shift ? 1.0f : 0.0f);
    juce::gl::glUniform1f (neonLoc_, state.neon ? 1.0f : 0.0f);
    juce::gl::glUniform1f (thermalLoc_, state.thermal ? 1.0f : 0.0f);
    juce::gl::glUniform1f (acidLoc_, state.acid ? 1.0f : 0.0f);
    juce::gl::glUniform1f (vhsLoc_, state.vhs ? 1.0f : 0.0f);
    juce::gl::glUniform1f (crtLoc_, state.crt ? 1.0f : 0.0f);
    juce::gl::glUniform1f (duotoneLoc_, state.duotone ? 1.0f : 0.0f);
    juce::gl::glUniform1f (bloomLoc_, state.bloom ? 1.0f : 0.0f);
    juce::gl::glUniform1f (binaryLoc_, state.binary ? 1.0f : 0.0f);
    juce::gl::glUniform1f (prismaticLoc_, state.prismatic ? 1.0f : 0.0f);
    juce::gl::glUniform1f (tweakZoomLoc_, state.offset.value[0]);
    juce::gl::glUniform1f (tweakRotLoc_,  state.offset.value[1]);
    juce::gl::glUniform1f (tweakWarpLoc_, state.offset.value[2]);
    juce::gl::glUniform1f (tweakDxLoc_,   state.offset.value[3]);
    juce::gl::glUniform1f (tweakDyLoc_,   state.offset.value[4]);
    juce::gl::glUniform1f (tweakSxLoc_,   state.offset.value[5]);
    juce::gl::glUniform1f (tweakSyLoc_,   state.offset.value[6]);
    juce::gl::glUniform1f (tweakKaleidoLoc_, static_cast<float>(state.offset.ivalue[0]));
    juce::gl::glUniform1f (tweakFoldXLoc_,   static_cast<float>(state.offset.ivalue[1]));
    juce::gl::glUniform1f (tweakFoldYLoc_,   static_cast<float>(state.offset.ivalue[2]));

    if (coreProfile_)
    {
        juce::gl::glBindVertexArray (vao_);
    }
    else
    {
        juce::gl::glBindBuffer (juce::gl::GL_ARRAY_BUFFER, vbo_);
        juce::gl::glEnableVertexAttribArray (0);
        juce::gl::glVertexAttribPointer (0, 2, juce::gl::GL_FLOAT,
                                         juce::gl::GL_FALSE, 0, nullptr);
    }

    juce::gl::glDrawArrays (juce::gl::GL_TRIANGLES, 0, 3);

    if (coreProfile_)
    {
        juce::gl::glBindVertexArray (0);
    }
    else
    {
        juce::gl::glDisableVertexAttribArray (0);
        juce::gl::glBindBuffer (juce::gl::GL_ARRAY_BUFFER, 0);
    }

    juce::gl::glActiveTexture (juce::gl::GL_TEXTURE0);
    juce::gl::glBindTexture (juce::gl::GL_TEXTURE_2D, 0);
    juce::gl::glUseProgram (0);

    // 绘制是否真的落地：驱动拒绝（program 失效 / FBO 状态非法 / shader 被丢弃）时
    //   这里会留下 GL 错误。调用方在调用前已清空错误队列，所以这条判定只反映本次绘制。
    return juce::gl::glGetError() == juce::gl::GL_NO_ERROR;
}

// ==========================================================
// MilkdropModule
// ==========================================================
MilkdropModule::MilkdropModule (AnalyserHub* hub_,
                               Y2KmeterAudioProcessorEditor* editor)
    : ModulePanel (ModuleType::milkdrop),
      hub (hub_),
      editor_ (editor)
{
    // 默认初始尺寸 300×250 (宽×高)
    setDefaultSize(400, 300);
    // 最小尺寸保护：模块高度低于此值会导致 projectM 内容区（扣除 22px 标题栏
    // 和边框后）过小甚至为 0，GL FBO/纹理分配失败，模块进入纯黑不可用状态。
    setMinSize(160, 70);

    // Toybox：整块排除（点击语义已被占用，§6.1）
    getProperties().set ("toy.optOut", true);

    // 尝试激活 Hub 的 Oscilloscope 路径 —— 有 hub 才有 PCM 输入。
    if (hub != nullptr)
    {
        hub->retain (AnalyserHub::Kind::Oscilloscope);
        hub->addFrameListener (this);
        hubRetained = true;
    }

    glView = std::make_unique<GLView> (*this);
    addAndMakeVisible (glView.get());
#if JUCE_MAC
    // macOS：控制栏通过顶层 NSWindow 悬浮绘制（addToDesktop），
    // Z-order 高于主窗口内嵌的 NSOpenGLView，确保 GL 帧上可见。
    // 实际 addToDesktop 与尺寸/位置同步由 UpdateOverlayViewPlacement 处理，
    // 仅在 focused_ && isShowing() 时才创建 native peer。
    overlayView_ = std::make_unique<OverlayView>(*this);
#endif
}

MilkdropModule::~MilkdropModule()
{
    // 关键顺序：
    //   1) 显式 detach GL —— 同步等待 GL 线程收尾（destroy projectM handle）；
    //   2) 解除 hub 挂钩（保证在 detach 之后再解除，避免 GL 线程 render 中
    //      读到 pcmMutex 保护的数据被并发销毁）；
    if (glView != nullptr)
        // detachAndWait removed: no GL context;

    if (hub != nullptr && hubRetained)
    {
        hub->removeFrameListener (this);
        hub->release (AnalyserHub::Kind::Oscilloscope);
        hubRetained = false;
    }

#if JUCE_MAC
    // macOS：先撚下顶层控制栏窗口（native peer），再销毁 unique_ptr。
    // 顺序保证：不会在 native peer 尚存活时被 unique_ptr 析构。
    //   提示窗口是 overlayView_ 的子组件，必须先于它释放。
    overlayTooltip_.reset();
    if (overlayView_ != nullptr && overlayView_->isOnDesktop())
        overlayView_->removeFromDesktop();
    overlayView_.reset();
#endif

    glView.reset(); // 现在可以安全地销毁子组件
}

int MilkdropModule::resolveLivePresetIndex() const
{
  if (pendingPresetTarget_ >= 0)
  {
    // 若渲染器已经切到该目标，自动交还实时追踪（否则在 GL 消费前保持确定性目标）
    if (glView != nullptr && glView->GetCurrentPresetIndex() == pendingPresetTarget_)
      pendingPresetTarget_ = -1;
    else
      return pendingPresetTarget_;
  }
  if (glView != nullptr)
  {
    const int live = glView->GetCurrentPresetIndex();
    if (live >= 0)
      return live;
  }
  return restored_preset_index_;
}

void MilkdropModule::notifyBeforeUserEdit()
{
  const int liveIdx = resolveLivePresetIndex();
  if (liveIdx >= 0)
    restored_preset_index_ = liveIdx;
  syncVisualFromEditor();
  syncWaveFromEditor();
  if (onWillChangeParamByUser)
    onWillChangeParamByUser (*this);
}

void MilkdropModule::notifyAfterUserEdit(const juce::String& actionLabel,
                                         const juce::String& focusHint,
                                         const juce::String& coalesceKey)
{
  setLastChangedParamActionLabel (actionLabel);
  setLastChangedParamFocusHint   (focusHint);
  setLastChangedParamCoalesceKey (coalesceKey);
  if (onParamChangedByUser)
    onParamChangedByUser (*this);
  setLastChangedParamActionLabel ({});
  setLastChangedParamFocusHint   ({});
  setLastChangedParamCoalesceKey ({});
}

void MilkdropModule::revealUndoTarget(const juce::String& focusHint)
{
  // 收起其它模块可能打开的设置抽屉，避免挡住 MilkDrop 控制器
  ModulePanel::closeOpenControlDrawers (nullptr, {});

  // 强制聚焦并刷新 4s 自动隐藏计时器，确保顶部控制栏立即可见
  if (! focused_)
    setFocusVisual (true);
  touchOverlayIdleTimer();

  auto closeAllSubPanels = [this]()
  {
    isAutoRowExpanded_  = false;
    isColorPanelOpen_   = false;
    isEffectsPanelOpen_ = false;
    isWavePanelOpen_    = false;
    isTweakPanelOpen_   = false;
  };

  if (focusHint == "color")
  {
    closeAllSubPanels();
    isColorPanelOpen_ = true;
    syncVisualFromEditor();
  }
  else if (focusHint == "effects")
  {
    closeAllSubPanels();
    isEffectsPanelOpen_ = true;
    syncVisualFromEditor();
  }
  else if (focusHint == "wave")
  {
    closeAllSubPanels();
    isWavePanelOpen_ = true;
    syncWaveFromEditor();
  }
  else if (focusHint == "tweak")
  {
    closeAllSubPanels();
    isTweakPanelOpen_ = true;
    syncVisualFromEditor();
  }
  else if (focusHint == "auto_row")
  {
    closeAllSubPanels();
    isAutoRowExpanded_ = true;
  }
  else if (focusHint == "auto")
  {
    // 仅收起与 auto 互斥的四个子面板；isAutoRowExpanded_ 已由 restoreModuleSpecificState 与 isAutoMode_ 同步
    isColorPanelOpen_   = false;
    isEffectsPanelOpen_ = false;
    isWavePanelOpen_    = false;
    isTweakPanelOpen_   = false;
  }
  else if (focusHint == "reset_all")
  {
    // 同步全部视觉与波形缓存（保留已展开的子面板同步刷新，或直接清晰展示顶部控制栏）
    syncVisualFromEditor();
    syncWaveFromEditor();
  }

  layoutContent (getContentBounds());
  repaint();
  if (glView != nullptr)
    glView->repaint();
#if JUCE_MAC
  UpdateOverlayViewPlacement();
  if (overlayView_ != nullptr && overlayView_->isOnDesktop())
    overlayView_->repaint();
#endif
}

void MilkdropModule::notifyUserGestureEnd()
{
  if (onParamGestureEnded)
    onParamGestureEnded (*this);
}

juce::ValueTree MilkdropModule::saveModuleSpecificState() const
{
  juce::ValueTree s("state");
  const int idx = resolveLivePresetIndex();
  if (idx >= 0)
  {
    restored_preset_index_ = idx;
    s.setProperty("presetIndex", idx, nullptr);
  }
  s.setProperty("autoMode", isAutoMode_, nullptr);
  s.setProperty("autoInterval", autoIntervalSeconds_, nullptr);
  s.setProperty("autoBeatMode", autoModeIsBeat_, nullptr);
  s.setProperty("autoBeats", autoBeats_, nullptr);
  s.setProperty("keyboardEnabled", keyboardControlEnabled_, nullptr);

  // 序列化全局视觉状态（color / effects / tweak），用于 Ctrl+Z 撤回重做与完整布局还原
  const MilkdropVisualState vis = (editor_ != nullptr) ? editor_->GetMilkdropVisualState() : visualState_;
  {
    juce::ValueTree vt("visual");
    vt.setProperty("tintR", (double) vis.tint_r, nullptr);
    vt.setProperty("tintG", (double) vis.tint_g, nullptr);
    vt.setProperty("tintB", (double) vis.tint_b, nullptr);
    vt.setProperty("brightness", (double) vis.brightness, nullptr);
    for (const auto& def : GetMilkdropEffectDefs())
      if (def.implemented && def.get(vis))
        vt.setProperty(def.display_name, true, nullptr);
    for (int i = 0; i < MilkdropVisualOffsetState::kParamCount; ++i)
      if (std::fabs(vis.offset.value[i]) > 1e-6f)
        vt.setProperty("off" + juce::String(i), (double) vis.offset.value[i], nullptr);
    for (int i = 0; i < MilkdropVisualOffsetState::kIntParamCount; ++i)
      if (vis.offset.ivalue[i] != 0)
        vt.setProperty("ioff" + juce::String(i), vis.offset.ivalue[i], nullptr);
    s.appendChild(vt, nullptr);
  }

  // 序列化简单波形覆盖状态（wave）
  const MilkdropWaveState wv = (editor_ != nullptr) ? editor_->GetMilkdropWaveState() : waveState_;
  {
    juce::ValueTree wt("wave");
    wt.setProperty("enabled",  wv.enabled,  nullptr);
    wt.setProperty("mode",     wv.mode,     nullptr);
    wt.setProperty("x",        (double) wv.x, nullptr);
    wt.setProperty("y",        (double) wv.y, nullptr);
    wt.setProperty("r",        (double) wv.r, nullptr);
    wt.setProperty("g",        (double) wv.g, nullptr);
    wt.setProperty("b",        (double) wv.b, nullptr);
    wt.setProperty("a",        (double) wv.a, nullptr);
    wt.setProperty("mystery",  (double) wv.mystery, nullptr);
    wt.setProperty("usedots",  wv.usedots,  nullptr);
    wt.setProperty("thick",    wv.thick,    nullptr);
    wt.setProperty("additive", wv.additive, nullptr);
    wt.setProperty("brighten", wv.brighten, nullptr);
    s.appendChild(wt, nullptr);
  }

  return s;
}

void MilkdropModule::restoreModuleSpecificState(const juce::ValueTree& state)
{
  const int prevIdx = resolveLivePresetIndex();

  // 1) 恢复视觉状态（color / effects / tweak）
  if (auto vt = state.getChildWithName("visual"); vt.isValid())
  {
    MilkdropVisualState v;
    v.tint_r     = juce::jlimit(kTintMin, kTintMax, (float) (double) vt.getProperty("tintR", 1.0));
    v.tint_g     = juce::jlimit(kTintMin, kTintMax, (float) (double) vt.getProperty("tintG", 1.0));
    v.tint_b     = juce::jlimit(kTintMin, kTintMax, (float) (double) vt.getProperty("tintB", 1.0));
    v.brightness = juce::jlimit(kBrightMin, kBrightMax, (float) (double) vt.getProperty("brightness", 1.0));
    for (const auto& def : GetMilkdropEffectDefs())
      if (def.implemented)
        def.set(v, (bool) vt.getProperty(def.display_name, false));
    for (int i = 0; i < MilkdropVisualOffsetState::kParamCount; ++i)
      v.offset.value[i] = (float) (double) vt.getProperty("off" + juce::String(i), 0.0);
    for (int i = 0; i < MilkdropVisualOffsetState::kIntParamCount; ++i)
      v.offset.ivalue[i] = (int) vt.getProperty("ioff" + juce::String(i), 0);
    visualState_ = v;
    applyVisualToEditor();
  }

  // 2) 恢复波形覆盖状态（wave），若发生变更则标记需重载当前预设使注入生效
  bool waveChanged = false;
  if (auto wt = state.getChildWithName("wave"); wt.isValid())
  {
    const MilkdropWaveState prevW = (editor_ != nullptr) ? editor_->GetMilkdropWaveState() : waveState_;
    MilkdropWaveState w;
    w.enabled  = (bool) wt.getProperty("enabled", false);
    w.mode     = juce::jlimit(0, 15, (int) wt.getProperty("mode", 6));
    w.x        = (float) (double) wt.getProperty("x", 0.5);
    w.y        = (float) (double) wt.getProperty("y", 0.5);
    w.r        = (float) (double) wt.getProperty("r", 1.0);
    w.g        = (float) (double) wt.getProperty("g", 1.0);
    w.b        = (float) (double) wt.getProperty("b", 1.0);
    w.a        = (float) (double) wt.getProperty("a", 1.0);
    w.mystery  = (float) (double) wt.getProperty("mystery", 0.0);
    w.usedots  = (bool) wt.getProperty("usedots", false);
    w.thick    = (bool) wt.getProperty("thick", false);
    w.additive = (bool) wt.getProperty("additive", false);
    w.brighten = (bool) wt.getProperty("brighten", false);
    waveChanged = (w.enabled != prevW.enabled || w.mode != prevW.mode
                   || std::fabs(w.x - prevW.x) > 1e-4f || std::fabs(w.y - prevW.y) > 1e-4f
                   || std::fabs(w.r - prevW.r) > 1e-4f || std::fabs(w.g - prevW.g) > 1e-4f
                   || std::fabs(w.b - prevW.b) > 1e-4f || std::fabs(w.a - prevW.a) > 1e-4f
                   || std::fabs(w.mystery - prevW.mystery) > 1e-4f
                   || w.usedots != prevW.usedots || w.thick != prevW.thick
                   || w.additive != prevW.additive || w.brighten != prevW.brighten);
    waveState_ = w;
    applyWaveToEditor();
  }

  // 3) 恢复预设索引（仅在目标预设真的变了或 wave 覆盖变更时才跳转重载，避免撤回 color/effects 时重播预设）
  bool jumpedPreset = false;
  if (state.hasProperty("presetIndex"))
  {
    int idx = static_cast<int>(state.getProperty("presetIndex"));
    if (idx >= 0)
    {
      restored_preset_index_ = idx;
      if (idx != prevIdx || waveChanged)
      {
        pendingPresetTarget_ = idx;
        if (isFloating() && glView != nullptr)
          glView->RequestPresetJump(idx);
        else if (editor_ != nullptr)
          editor_->RequestMilkdropPresetJump(idx);
        jumpedPreset = true;
      }
    }
  }
  if (waveChanged && ! jumpedPreset)
    reloadCurrentPresetForWave();

  if (state.hasProperty("autoMode"))
  {
    isAutoMode_ = static_cast<bool>(state.getProperty("autoMode"));
    // 恢复 auto 开关状态时同步展开时间控制条，保持与手动开启 auto 时一致的表现。
    isAutoRowExpanded_ = isAutoMode_;
  }
  if (state.hasProperty("autoInterval"))
  {
    autoIntervalSeconds_ = juce::jlimit(kMinAutoInterval, kMaxAutoInterval,
                                        static_cast<float>(state.getProperty("autoInterval")));
  }
  if (state.hasProperty("autoBeatMode"))
    autoModeIsBeat_ = static_cast<bool>(state.getProperty("autoBeatMode"));
  if (state.hasProperty("autoBeats"))
  {
    autoBeats_ = juce::jlimit(kMinAutoBeats, kMaxAutoBeats,
                              static_cast<int>(state.getProperty("autoBeats")));
  }
  if (state.hasProperty("keyboardEnabled"))
    keyboardControlEnabled_ = static_cast<bool>(state.getProperty("keyboardEnabled"));
  else if (state.hasProperty("keyboardDisabled"))
    keyboardControlEnabled_ = ! static_cast<bool>(state.getProperty("keyboardDisabled"));
  // Standalone（无宿主 playhead）下 BPM 同步模式不可用，回退时间模式。
  //   覆盖旧存档（无 autoBeatMode 字段，默认 beat）在 Standalone 下恢复后
  //   auto 开启却无法切换的边界情况。
  if (autoModeIsBeat_ && !isHostPlayheadAvailable())
    autoModeIsBeat_ = false;

  if (editor_ != nullptr)
    editor_->pushMilkdropAutomationParams();
  repaint();
  if (glView != nullptr)
    glView->repaint();
#if JUCE_MAC
  if (overlayView_ != nullptr && overlayView_->isOnDesktop())
    overlayView_->repaint();
#endif
}

juce::Rectangle<int> MilkdropModule::GetContentLocalBounds() const {
  return getContentBounds();
}

void MilkdropModule::paint(juce::Graphics& g) {
  // Editor::renderOpenGL 已经将 projectM 帧渲染到 Editor CachedImage FBO 中
  // 本模块内容区屏幕坐标对应的区域。这里绘制卡片外壳（边框、标题栏、关闭按钮），
  // 内容区保持透明以保留 GPU 渲染的 projectM 帧。
  // 裸模式（milkdrop-only）或透明模式未悬停时跳过卡片外壳，保持纯 alpha=0。
  const bool transparentHidden = PinkXP::isTransparentBackdrop()
                                 && ! isBareMode()
                                 && ! PinkXP::isChromaRevealed (*this);
  if (! isBareMode() && ! transparentHidden)
  {
    const auto bounds = getLocalBounds();

  // 1. 像素凸起窗口边框（边框必须不透明，内容区必须透明以透出 projectM 帧）
  //   · 脱离态（浮动窗口）：GLView 自己的 native GL surface 高于 paint，用不透明 face。
  //   · 嵌入态：Editor GL 渲染到 FBO 0（CachedImage）位于组件树之下，
  //     drawRaised 首行的 fillRect 会覆盖整个模块矩形；用不透明 face 会遮住
  //     projectM 帧，表现为"模块底色色块"。必须用 transparentBlack 让内容区
  //     透明透出帧（边框 hl/dark/shdw 仍为不透明正常显示）。
  // 嵌入态外壳透明（透出 Editor GL 在 FBO 0 上的 Milkdrop 内容）；浮动态不透明。
  // 软件绘制目标（添加模块菜单的 hover 预览快照）同样是"没有 GL 帧可透出"的场景，
  // 必须用不透明 face，否则透明像素在 macOS 的 GL 合成下会透出后缓冲残留（花屏）。
  if (isFloating() || isSoftwarePaintTarget())
    PinkXP::drawRaised(g, bounds, PinkXP::face);
  else
    PinkXP::drawRaised(g, bounds, juce::Colours::transparentBlack);

  // 2. 玫瑰粉标题栏
  auto tb = getTitleBarBounds();
  PinkXP::drawPinkTitleBar(g, tb, titleText, 12.0f);

  // 标题栏下沿深色分割线（v2.9.9：加厚到 2 行）
  //   嵌入态下 content 区域在 CPU 组件层是透明的（透出 GL 视频帧），而 1 逻辑
  //   像素的分割线在非整数 DPI（125%/150%）下会被 `Graphics::fillRect` 走 AA
  //   路径填充 → 它最下面那个物理像素行只有部分覆盖率（如 25%），剩余 75%
  //   会透出后缓冲里该行的内容，也就是先绘制好的下层 GL 模块（Spectrogram3D
  //   等）→ "视频区与抬头之间一条极细的线能看到被覆盖的模块"。
  //   多画一行，让这条线的实体部分完整盖住那个边界行；AA 残差落在视频的第二
  //   行上（= 暗线 + 视频的柔和过渡），不会再透出别的模块。
  g.setColour(PinkXP::titleBarDividerColour());
  g.fillRect(tb.getX(), tb.getBottom(), tb.getWidth(), 2);

  // 3. 关闭按钮（×）与 3.5 弹出/停靠按钮 —— 随 SkinTokens 统一工艺
  const auto& chromeTokens = PinkXP::getSkinTokens();
  const bool modernTitleBtns = (chromeTokens.frameStyle == PinkXP::FrameStyle::modernStudio);
  auto drawTitleGlyphBtn = [&](juce::Rectangle<int> r, const char* glyph, bool hovered, bool pressed)
  {
    if (modernTitleBtns)
    {
      const auto rf = r.toFloat();
      const float rad = juce::jmin(3.5f, rf.getHeight() * 0.35f);
      if (pressed || hovered)
      {
        g.setColour(PinkXP::selInk.withAlpha(pressed ? 0.24f : 0.15f));
        g.fillRoundedRectangle(rf, rad);
        g.setColour(PinkXP::selInk.withAlpha(0.28f));
        g.drawRoundedRectangle(rf.reduced(0.5f), rad, 1.0f);
      }
      auto tr = r;
      if (pressed) tr.translate(0, 1);
      PinkXP::SymbolGlyphs::drawIconGlyph(g, glyph, tr, 11.0f,
                                          PinkXP::selInk.withAlpha(hovered || pressed ? 1.0f : 0.88f), 0, -1);
    }
    else
    {
      const auto state = pressed ? PinkXP::ControlState::pressed
                       : hovered ? PinkXP::ControlState::hover
                                 : PinkXP::ControlState::normal;
      const auto fill  = pressed ? PinkXP::pink100
                       : hovered ? PinkXP::pink200
                                 : PinkXP::btnFace;
      PinkXP::Craft::drawControlChrome(g, r, fill, state);
      auto tr = r;
      if (pressed) tr.translate(1, 1);
      PinkXP::SymbolGlyphs::drawIconGlyph(g, glyph, tr, 11.0f, PinkXP::readableInkOn(fill), 0, -1);
    }
  };

  drawTitleGlyphBtn(getCloseButtonBounds(), "x", closeButtonHovered, closeButtonPressed);

  if (isPopOutEnabled() || isFloating())
    drawTitleGlyphBtn(getPopOutButtonBounds(), isFloating() ? "=" : "-",
                      popOutButtonHovered_, popOutButtonPressed_);
  } // if (!isBareMode())

  // 4. 内容区叠加控件（不填充背景 — projectM 帧已由 GPU 渲染）
  auto content = getContentBounds();
  if (content.getWidth() > 0 && content.getHeight() > 0)
    paintContent(g, content);
}

void MilkdropModule::paintContent(juce::Graphics& g, juce::Rectangle<int> content) {
  // projectM 帧已由 Editor::renderOpenGL 直接渲染到 Editor CachedImage FBO 中
  // 本模块内容区对应的屏幕区域。paintContent 仅负责：
  //   · 未就绪时的兜底黑屏 + 错误提示
  //   · 加载指示器 / 叠加控制栏（top bar、auto 控件等）
  //   · 浮动态：读取 Editor 共享帧（glReadPixels 抓取的离线 FBO 内容）

  // ---- 软件绘制目标：添加模块菜单的 hover 预览快照 ----
  //   快照是一张 CPU Image，嵌入态的 projectM 帧完全不在里面（帧由
  //   Editor::renderOpenGL 直接写进后缓冲，CPU 侧内容区是透明的）。
  //   Windows 的软件光栅器下面就是画布底色，透明看不出问题；macOS 的窗口
  //   走 OpenGL 合成，快照里那块透明区域会把后缓冲的残留内容透出来，
  //   表现为「预览窗内花屏 / 彩色噪声」。这里主动画一块不透明底色 +
  //   静态标识，让预览在任何平台都是确定的画面（真实插入后的模块不受影响，
  //   那条路径没有本分支）。
  if (isSoftwarePaintTarget())
  {
    g.setColour(juce::Colours::black);
    g.fillRect(content);

    g.setColour(juce::Colours::grey.withAlpha(0.55f));
    g.setFont(PinkXP::getFont(11.0f, juce::Font::bold));
    g.drawText(y2k::tr ("MILKDROP"), content, juce::Justification::centred, false);
    return;
  }

  // ---- 诊断：模块自身是否在画"未就绪黑块" ----
  //   用户看到的"纯黑"有两种完全不同的来源，必须分开：
  //     a) 这里 —— 模块认为 projectM 没就绪 → 整块填黑 + 提示文字；
  //     b) 模块内容区透明，但后缓冲里那块 GL 内容本身是黑的（Editor 侧 FBO/blit）。
  //   只在状态切换时记一行（paintContent 30Hz 调用，不能逐帧刷日志）。
  if (Y2K_DIAG_ENABLED())
  {
    static int sLastPaintState = -1;
    const int paintState = (glView == nullptr) ? 0 : (glView->IsRenderReady() ? 2 : 1);

    if (paintState != sLastPaintState)
    {
      sLastPaintState = paintState;

      // 注：这里刻意用 ASCII —— juce::String 的 const char* 构造按 ASCII 解析，
      //   拼接路径才是 UTF-8；状态文案放常量表里更不容易踩坑。
      const char* stateText = (paintState == 2) ? "ready(content transparent)"
                            : (paintState == 1) ? "notReady(fills black + hint)"
                                                : "glView=null(fills black)";

      Y2K_DIAG ("MD", juce::String ("module paint state=") + stateText
                            + " floating=" + (isFloating() ? "1" : "0")
                            + " content=" + juce::String (content.getWidth()) + "x"
                                           + juce::String (content.getHeight())
                            + " err=" + (glView != nullptr ? glView->GetError()
                                                           : juce::String ("-")));
    }
  }

  const bool transparentBackdrop = PinkXP::isTransparentBackdrop();
  const bool transparentHidden   = transparentBackdrop && ! PinkXP::isChromaRevealed (*this);

  // 浮动态由 GLView 自己的 native OpenGL surface 直接渲染 projectM。
  // 不再绘制 Editor 共享帧，避免画面继续受主窗口 FBO 0 尺寸裁剪。
  if (isFloating())
  {
    if (glView == nullptr || !glView->IsRenderReady())
    {
      // 优先显示最后一帧快照，消除 detach/attach 重建期间的黑屏闪烁
      auto snapshot = (glView != nullptr) ? glView->GetLastFrameSnapshot() : juce::Image();
      if (snapshot.isValid())
        g.drawImage(snapshot, content.toFloat());
      else if (! transparentBackdrop)
        g.fillAll(juce::Colours::black);
    }
  }
  else if (glView != nullptr) {
    if (!glView->IsRenderReady()) {
      // 优先显示最后一帧快照，消除 detach/attach 重建期间的黑屏闪烁
      auto snapshot = glView->GetLastFrameSnapshot();
      if (snapshot.isValid())
      {
        g.drawImage(snapshot, content.toFloat());
      }
      else
      {
        if (! transparentBackdrop)
          g.fillAll(juce::Colours::black);
        if (! transparentHidden)
        {
          auto msg = glView->GetError().isEmpty()
                         ? juce::String("Milkdrop initializing...")
                         : y2k::trf ("Milkdrop error: {0}", y2k::tr (glView->GetError()));
          g.setColour(juce::Colours::grey);
          g.setFont(juce::Font(12.0f));
          g.drawText(y2k::tr (msg), content, juce::Justification::centred, false);
        }
      }
    }
  } else {
    if (! transparentBackdrop)
      g.fillAll(juce::Colours::black);
    if (! transparentHidden)
    {
      g.setColour(juce::Colours::grey);
      g.setFont(juce::Font(12.0f));
      g.drawText(y2k::tr ("Milkdrop initializing..."), content, juce::Justification::centred, false);
    }
  }

  if (! transparentHidden && glView != nullptr && glView->IsRenderReady())
    PaintLoadingIndicator(g, content);

  // 控制栏（overlay control bar）以 CPU paint 叠加方式绘制在 GL 帧之上。
  //   嵌入态（Editor GL 合成）：Editor 先把 projectM 帧 blit 到合成用的
  //     CachedImage，再由此处 CPU 绘制覆盖 —— 与 Windows 完全同路径。
  //   macOS 的 overlayView_（顶层子 NSWindow）同样绘制控制栏，只是用于保证
  //     原生 NSOpenGLView（浮动态）/ 合成层之外的稳定可见性；同时它承载
  //     自己的悬停提示窗口（见 OverlayView::getTooltip）。
  if (focused_ && glView != nullptr && ! transparentHidden) {
    auto topBar = content.withHeight(26);
    paintOverlayControlBar(g, topBar);
    paintLibraryButtons(g, content);
    if (isAutoRowExpanded_)
      paintAutoControlRow(g, topBar);
    if (isColorPanelOpen_)
      paintColorPanel(g, topBar);
    if (isEffectsPanelOpen_)
      paintEffectsPanel(g, topBar);
    if (isWavePanelOpen_)
      paintWavePanel(g, topBar);
    if (isTweakPanelOpen_)
      paintTweakPanel(g, topBar);
  }
}

void MilkdropModule::layoutContent (juce::Rectangle<int> content)
{
    if (glView != nullptr)
    {
        // 控制栏（overlay control bar）始终以纯绘制叠加方式覆盖在 GL 帧上方，
        // 不占用布局空间、不改变 GLView 尺寸，避免触发 projectM setWindowSize 重建。
        // 脱离态（floating）下，Windows 由 GLView::paint() 将控制栏绘制在
        // projectM GL 帧之上；macOS 由顶层 overlayView_（NSWindow）另行绘制，
        // 因此二者都无需再为控制栏预留空间，布局与非脱离态保持一致。
        glView->setBounds(content);
#if JUCE_MAC
        UpdateOverlayViewPlacement();
#endif
    }
}

void MilkdropModule::onFrame (const AnalyserHub::FrameSnapshot& frame)
{
    if (! frame.has (AnalyserHub::Kind::Oscilloscope) || glView == nullptr)
        return;

    // 把 L/R 数组交错成 LRLR，供 projectM 的立体声接口消费。
    constexpr int N = (int) AnalyserHub::oscilloscopeBufferSize;
    // 用 alloca-alike 静态缓冲避免每帧 heap alloc：60Hz × 2×2048 float ≈ 240 KB/s
    // 但 GLView::pushPcm 内部会拷贝到自己的 vector；这里可以直接堆栈缓冲。
    float tmp[N * 2];
    const auto& L = frame.oscL;
    const auto& R = frame.oscR;
    for (int i = 0; i < N; ++i)
    {
        tmp[i * 2 + 0] = L[(size_t) i];
        tmp[i * 2 + 1] = R[(size_t) i];
    }

    glView->PushPcm (tmp, (unsigned int) N);

    // 嵌入态（非脱离）：内容由 Editor GL 渲染，必须逐帧向 Editor 要一帧。
    //   · Editor 的 OpenGLContext 是 setContinuousRepainting(false) → 组件层没有
    //     脏区就不出帧；整个界面只剩 Milkdrop 时（布局预设 Only Milkdrop），
    //     唯一的重绘来源曾是 GLView 的 30Hz 兜底 Timer，帧率被钉死在 30fps。
    //   · RequestGlFrame 只驱动 renderOpenGL，不重新光栅化组件层 —— 比
    //     repaintFrameContent()（整块内容区每帧重绘）便宜。
    //   · 浮动态由 GLView 自己的 native 上下文 setContinuousRepainting(true)
    //     连续渲染，不需要额外驱动。
    if (! isFloating() && editor_ != nullptr)
        editor_->RequestGlFrame();
}

void MilkdropModule::nextPreset()
{
    if (glView == nullptr) return;
    const int total = glView->GetTotalPresetCount();
    if (total > 0)
    {
        int cur = resolveLivePresetIndex();
        if (cur < 0 || cur >= total) cur = 0;
        const int next = (cur + 1) % total;
        notifyBeforeUserEdit();
        pendingPresetTarget_   = next;
        restored_preset_index_ = next;
        glView->RequestPresetJump (next);
        pendingPresetTarget_   = next;
        restored_preset_index_ = next;
        notifyAfterUserEdit (y2k::tr ("MilkDrop preset"), "preset");
        return;
    }
    glView->RequestPresetDelta (+1);
}

void MilkdropModule::prevPreset()
{
    if (glView == nullptr) return;
    const int total = glView->GetTotalPresetCount();
    if (total > 0)
    {
        int cur = resolveLivePresetIndex();
        if (cur < 0 || cur >= total) cur = 0;
        const int prev = (cur - 1 + total) % total;
        notifyBeforeUserEdit();
        pendingPresetTarget_   = prev;
        restored_preset_index_ = prev;
        glView->RequestPresetJump (prev);
        pendingPresetTarget_   = prev;
        restored_preset_index_ = prev;
        notifyAfterUserEdit (y2k::tr ("MilkDrop preset"), "preset");
        return;
    }
    glView->RequestPresetDelta (-1);
}

void MilkdropModule::randomPreset()
{
    if (glView == nullptr) return;
    const int total = glView->GetTotalPresetCount();
    if (total > 0)
    {
        const int cur = resolveLivePresetIndex();
        int next = 0;
        if (total > 1)
        {
            if (cur >= 0 && cur < total)
            {
                next = juce::Random::getSystemRandom().nextInt (total - 1);
                if (next >= cur) ++next;
            }
            else
            {
                next = juce::Random::getSystemRandom().nextInt (total);
            }
        }
        notifyBeforeUserEdit();
        pendingPresetTarget_   = next;
        restored_preset_index_ = next;
        glView->RequestPresetJump (next);
        pendingPresetTarget_   = next;
        restored_preset_index_ = next;
        notifyAfterUserEdit (y2k::tr ("MilkDrop preset"), "preset");
        return;
    }
    glView->RequestPresetRandom();
}

void MilkdropModule::jumpToPresetIndex(int index)
{
    if (glView == nullptr) return;
    const int total = glView->GetTotalPresetCount();
    if (total > 0)
        index = juce::jlimit (0, total - 1, index);
    if (index < 0)
        return;
    notifyBeforeUserEdit();
    pendingPresetTarget_   = index;
    restored_preset_index_ = index;
    glView->RequestPresetJump (index);
    pendingPresetTarget_   = index;
    restored_preset_index_ = index;
    notifyAfterUserEdit (y2k::tr ("MilkDrop preset"), "preset");
}

// ==========================================================
// GLView
// ==========================================================
MilkdropModule::GLView::GLView(MilkdropModule& owner)
    : owner_(owner) {
#if JUCE_MAC
  // projectM 4 的 GLSL shader 需要 OpenGL Core Profile 3.2+。
  // 不设置此项时 macOS 默认给 Legacy Profile（NSOpenGLProfileVersionLegacy），
  // projectM 内部 shader 编译失败，渲染输出全黑。
  // 仅 macOS 强制：Windows 端强制 Core Profile 会导致部分预设 shader 编译失败，
  // projectM 回退到 Idle 动画（e4bc0b78 用默认 GL 版本无此问题）。
  open_gl_context_.setOpenGLVersionRequired(juce::OpenGLContext::openGL3_2);
#endif
  open_gl_context_.setRenderer(this);
  // macOS 关键：必须开启 setComponentPaintingEnabled(true)，才能让
  // CPU paint 层（含 overlay 控制栏、模态对话框）叠加在 GL 帧之上可见。
  // 若关闭，native NSOpenGLView 的 Z-order 会永久覆盖 JUCE 组件树绘制，
  // 导致控制栏与对话框看不见，且对话框 enterModalState 后整应用看似置灰。
  // Windows 下 JUCE 依旧能正确混合 CPU/GL 层，也保持开启。
  open_gl_context_.setComponentPaintingEnabled(true);
  open_gl_context_.setContinuousRepainting(true);
  startTimerHz(30);
}

MilkdropModule::GLView::~GLView() {
  stopTimer();
  DetachOpenGL();
}

void MilkdropModule::GLView::parentHierarchyChanged() {
  UpdateOpenGLAttachment();
}

void MilkdropModule::GLView::visibilityChanged() {
  UpdateOpenGLAttachment();
}

void MilkdropModule::GLView::resized() {
  if (attached_)
    open_gl_context_.triggerRepaint();
}

void MilkdropModule::GLView::paint(juce::Graphics& g) {
#if ! JUCE_MAC
  // Windows 脱离态：控制栏必须绘制在 GLView 自己的 paint 层上。
  // 由于 GLView 开启了 setComponentPaintingEnabled(true)，该 paint 会被
  // 合成到 projectM GL 帧之上；若仍绘制在 owner 的 paintContent 中，
  // 则会被 GLView 的原生 GL surface 遮挡（这也是此前需要挤压 GLView 的原因）。
  // 这里通过平移 Graphics 到 owner 坐标系，复用 paintOverlayControlBar /
  // paintAutoControlRow，保证 cachedNameArea_ / cachedAutoTimeLabel_ 的
  // 命中测试坐标仍与 owner 坐标一致。
  if (!owner_.isFloating() || !owner_.focused_)
    return;
  if (PinkXP::isTransparentBackdrop() && ! PinkXP::isChromaRevealed (owner_))
    return;

  auto content = owner_.getContentBounds();
  const auto offset = getPosition();
  g.saveState();
  g.addTransform(juce::AffineTransform::translation(
      -static_cast<float>(offset.getX()),
      -static_cast<float>(offset.getY())));
  owner_.paintOverlayControlBar(g, content);
  owner_.paintLibraryButtons(g, content);
  if (owner_.isAutoRowExpanded_)
    owner_.paintAutoControlRow(g, content.withHeight(26));
  if (owner_.isColorPanelOpen_)
    owner_.paintColorPanel(g, content.withHeight(26));
  if (owner_.isEffectsPanelOpen_)
    owner_.paintEffectsPanel(g, content.withHeight(26));
  if (owner_.isWavePanelOpen_)
    owner_.paintWavePanel(g, content.withHeight(26));
  if (owner_.isTweakPanelOpen_)
    owner_.paintTweakPanel(g, content.withHeight(26));
  g.restoreState();
#else
  juce::ignoreUnused(g);
#endif
}

void MilkdropModule::GLView::UpdateOpenGLAttachment() {
  // v2.9.6：macOS 已启用 Editor GL（GPU 化），嵌入态由 Editor GL 上下文渲染，
  //   GLView 只在浮动态 attach 自己的上下文——与 Windows 完全一致。
  //   嵌入态不再挂本地 NSOpenGLView，Milkdrop 内容由 Editor 按 z 序合成，
  //   可被其他模块正常覆盖。
  const bool should_attach = owner_.isFloating() && isShowing() && getWidth() > 0 && getHeight() > 0;
  if (should_attach == attached_)
    return;

  if (should_attach) {
    if (owner_.isFloating() && owner_.editor_ != nullptr)
      owner_.editor_->SuspendMilkdropEditorRendererForFloating();
    open_gl_context_.attachTo(*this);
    attached_ = true;
  } else {
    DetachOpenGL();
  }
}

void MilkdropModule::GLView::DetachOpenGL() {
  if (!attached_)
    return;

  SyncOwnerPresetIndexFromRenderer();

  // 浮动态 detach 时恢复 Editor renderer（dock 回嵌入态由 Editor GL 继续渲染）。
  const bool should_resume_editor_renderer = !owner_.isFloating();

  open_gl_context_.detach();
  attached_ = false;

  if (owner_.editor_ != nullptr && should_resume_editor_renderer) {
    // Windows/GLEW：local handle 已销毁。恢复 Editor renderer 前彻底重载
    // projectM/GLEW DLL，让全局 GL 函数指针表归零。否则 Editor GL 上下文再
    // create 时，GLEW 仍残留本地 HGLRC 的旧指针，导致 dock 回嵌入态后
    // projectM 渲染卡死/黑帧。仅在此分支执行，避免程序退出时 FreeLibrary 卡死。
#if defined(_WIN32)
    projectm_api::Api::instance().reload();
#endif
    const int preset_index = owner_.restored_preset_index_;
    if (preset_index >= 0)
      owner_.editor_->RequestMilkdropPresetJump(preset_index);
    owner_.editor_->ResumeMilkdropEditorRendererAfterFloating();
  }
}

void MilkdropModule::GLView::ScanPresetFiles() {
  // 根据全局收藏库状态选择预设目录（浮动态 / macOS 本地渲染路径）。
  // 磁盘 IO（findChildFiles）在锁外完成，避免 GL 线程长时间持锁阻塞 UI 绘制。
  const bool use_like = (owner_.editor_ != nullptr)
                        && owner_.editor_->IsMilkdropUseLikeLibrary();
  auto presets_dir = use_like
                         ? FindMilkdropLikeDirForModule()
                         : FindMilkdropAssetsDirForModule("milkdrop_presets");
  if (!presets_dir.exists()) {
    std::lock_guard<std::mutex> lock(preset_paths_mutex_);
    local_preset_paths_.clear();
    return;
  }

  auto files = presets_dir.findChildFiles(juce::File::findFiles, false, "*.milk");
  std::lock_guard<std::mutex> lock(preset_paths_mutex_);
  local_preset_paths_.clear();
  for (auto& file : files)
    local_preset_paths_.add(file.getFullPathName());
  local_preset_paths_.sort(false);
}

void MilkdropModule::GLView::LoadCurrentPreset() {
  if (local_pm_handle_ == nullptr || local_preset_paths_.isEmpty())
    return;
  if (local_current_preset_ < 0 || local_current_preset_ >= local_preset_paths_.size())
    return;

  // Debug 死循环修复：projectM 在 loadPreset 内部会做 HLSL→GLSL 转译、
  // glCompileShader / glLinkProgram，这些调用在 macOS OpenGL over Metal
  // 后端有较大概率累积 GL error 队列。若不清空，下一帧 renderOpenGL()
  // 进入 JUCE 的 checkGLError() 就会命中 jassertfalse 或死循环（peer
  // 未 valid 时的 continue 无法消费错误）。前后各清一次保双保险。
  while (juce::gl::glGetError() != juce::gl::GL_NO_ERROR) {}

  auto& api = projectm_api::Api::instance();
  auto path = local_preset_paths_[local_current_preset_];
  if (api.hasLoadPresetData()) {
    juce::File file(path);
    if (file.existsAsFile()) {
      auto data = file.loadFileAsString().toStdString();
      // 注入 wave 样式覆盖（启用时才生效），再走 macOS shader 修复预处理。
      MilkdropWaveState wave;
      if (owner_.editor_ != nullptr)
        wave = owner_.editor_->GetMilkdropWaveState();
      data = ApplyWaveParamsToPresetText(data, wave);
      api.loadPresetData(local_pm_handle_, FixMilkdropShaderTypes(data), true);
    }
  } else {
    api.loadPresetFile(local_pm_handle_, path.toRawUTF8(), true);
  }

  while (juce::gl::glGetError() != juce::gl::GL_NO_ERROR) {}
}

void MilkdropModule::GLView::newOpenGLContextCreated() {
  // 诊断：浮动态独立 GL 上下文的创建（projectM handle / GLEW / 预设扫描）
  //   在这一步卡住时，整个 JUCE GL 线程都会被占住 → 所有 GL 模块一起"卡死"。
  y2k::diag::setThreadTag ("GL");
  y2k::diag::setPhase ("MilkdropGLView::newOpenGLContextCreated");
  Y2K_DIAGC ("GL", "MilkdropGLView context created (浮动态独立上下文)");

  // === Debug 死循环修复 + 首帧乱码修复 ===
  // 1) 清空可能存在的历史 GL 错误。macOS Core Profile / OpenGL over Metal
  //    下，刚 attach 时 GL error 队列里常有残留项，而 JUCE Debug 构建的
  //    checkGLError() 内部（juce_opengl.cpp:214）在 peer 未 valid 时会
  //    无限 continue 而不消费错误，造成卡死。先 clear 一下释放错误。
  // 2) 立即黑帧。后面 projectm_create() / initGlew() 中程会阻塞 GL 线程
  //    几百毫秒，期间 AppKit 仍会以 NSOpenGLView 的 back buffer 合成到
  //    窗口。若不先 clear，用户看到的是未初始化的 GPU 显存 =“乱码”。
  while (juce::gl::glGetError() != juce::gl::GL_NO_ERROR) {}
  juce::OpenGLHelpers::clear(juce::Colours::black);
  while (juce::gl::glGetError() != juce::gl::GL_NO_ERROR) {}

  auto& api = projectm_api::Api::instance();
  Y2K_LOG("[MilkdropGLView] newOpenGLContextCreated: isAvailable="
      + juce::String(api.isAvailable() ? 1 : 0)
      + " hasFboAPI=" + juce::String(api.hasOpenglRenderFrameFbo() ? 1 : 0)
      + " loadErr=" + api.loadError());
  api.resetGlewInitialization();
  if (!api.isAvailable()) {
    local_error_ = api.loadError();
    Y2K_LOG("[MilkdropGLView] projectM unavailable: " + local_error_);
    return;
  }
  if (!api.initGlew()) {
    local_error_ = api.loadError();
    Y2K_LOG("[MilkdropGLView] glewInit failed: " + local_error_);
    return;
  }

  local_pm_handle_ = api.create();
  Y2K_LOG("[MilkdropGLView] projectm_create -> "
      + juce::String(local_pm_handle_ != nullptr ? "OK" : "NULL"));
  if (local_pm_handle_ == nullptr) {
    local_error_ = "projectm_create() returned NULL.";
    return;
  }

  api.setMeshSize(local_pm_handle_, kDefaultMeshWidth, kDefaultMeshHeight);
  api.setFps(local_pm_handle_, kTargetFps);
  api.setPresetDuration(local_pm_handle_, kPresetDuration);
  api.setSoftCutDuration(local_pm_handle_, kSoftCutDuration);
  api.setHardCutEnabled(local_pm_handle_, false);

  auto tex_dir = FindMilkdropAssetsDirForModule("milkdrop_textures");
  if (tex_dir.exists()) {
    std::vector<std::string> paths{tex_dir.getFullPathName().toStdString()};
    api.setTextureSearchPaths(local_pm_handle_, paths);
  }

  // 每次 GL 上下文重建都重新扫描预设，保证读取 restored_preset_index_ 前
  // local_preset_paths_ 是最新且完整的（与 e4bc0b78 行为一致）。
  ScanPresetFiles();
  int pending = owner_.restored_preset_index_;
  owner_.restored_preset_index_ = -1;
  local_current_preset_ = (pending >= 0 && pending < local_preset_paths_.size()) ? pending : 0;

  // 预设 shader 编译前后清错误，避免 projectM 内部 hlslparser→GLSL 编译
  // 过程产生的无害 warning 污染 error 队列、拖到下一帧 checkGLError 里卡死。
  while (juce::gl::glGetError() != juce::gl::GL_NO_ERROR) {}
  LoadCurrentPreset();
  while (juce::gl::glGetError() != juce::gl::GL_NO_ERROR) {}

  last_preset_switch_ms_ = static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());

  // 初始化整体染色后处理着色器（浮动态 / macOS 本地 GL 上下文渲染路径使用）。
  // 编译失败不致命，只关闭染色能力，Milkdrop 渲染本身不受影响。
  tint_pass_.reset (new MilkdropTintPass (open_gl_context_));
  if (!tint_pass_->init())
    tint_pass_.reset();
  Y2K_LOG("[MilkdropGLView] tint pass init -> "
      + juce::String(tint_pass_ != nullptr ? "OK" : "FAIL"));

  local_render_ready_ = true;
}

void MilkdropModule::GLView::openGLContextClosing() {
  SyncOwnerPresetIndexFromRenderer();
  // 在销毁 handle 之前先抓取最后一帧快照，
  // 供 detach/attach 重建期间 paintContent 显示，消除切换模式时的黑屏闪烁。
  CaptureLastFrame();
  // 释放整体染色后处理着色器（必须在 GL 上下文关闭前，此时 GL context 仍有效）
  if (tint_pass_ != nullptr)
  {
    tint_pass_->shutdown();
    tint_pass_.reset();
  }
  // 销毁离屏 FBO（必须在 GL 上下文关闭前，此时 GL context 仍有效）
  DestroyScaleFbo();
  if (local_pm_handle_ != nullptr) {
    auto& api = projectm_api::Api::instance();
    api.destroy(local_pm_handle_);
    local_pm_handle_ = nullptr;
    api.resetGlewInitialization();
  }
  local_render_ready_ = false;
}

void MilkdropModule::GLView::CaptureLastFrame() {
  // 此函数必须在 GL 线程（openGLContextClosing 回调中）调用，
  // 此时 GL context 仍然有效，可以安全地读取 framebuffer。
  const int w = getWidth();
  const int h = getHeight();
  if (w <= 0 || h <= 0)
    return;

  // 用 glReadPixels 从默认 framebuffer 读取当前帧（RGBA，从左下角开始）
  std::vector<juce::uint8> pixels(static_cast<size_t>(w * h * 4));
  juce::gl::glReadPixels(0, 0, w, h,
                         juce::gl::GL_RGBA,
                         juce::gl::GL_UNSIGNED_BYTE,
                         pixels.data());

  // OpenGL 坐标系 Y 轴朝上，JUCE Image Y 轴朝下，需要垂直翻转
  juce::Image img(juce::Image::ARGB, w, h, false);
  juce::Image::BitmapData bmp(img, juce::Image::BitmapData::writeOnly);
  for (int row = 0; row < h; ++row)
  {
    const juce::uint8* src = pixels.data() + static_cast<size_t>((h - 1 - row) * w * 4);
    juce::uint8* dst = bmp.getLinePointer(row);
    for (int col = 0; col < w; ++col)
    {
      // OpenGL: RGBA → JUCE ARGB（内存布局 B G R A on little-endian）
      dst[0] = src[2]; // B
      dst[1] = src[1]; // G
      dst[2] = src[0]; // R
      dst[3] = src[3]; // A
      src += 4;
      dst += 4;
    }
  }

  std::lock_guard<std::mutex> lock(snapshot_mutex_);
  last_frame_snapshot_ = std::move(img);
}

juce::Image MilkdropModule::GLView::GetLastFrameSnapshot() const {
  std::lock_guard<std::mutex> lock(snapshot_mutex_);
  return last_frame_snapshot_;
}

void MilkdropModule::GLView::ConsumePresetRequests() {
  // 消费收藏库切换请求：重扫本地预设列表并恢复到切换后库之前记录的预设。
  if (requested_library_toggle_.exchange(false)) {
    // 切换前 Editor 的 milkdrop_use_like_library_ 已被 ToggleMilkdropLibraryState 翻转，
    // 按新值反推旧库，把当前索引（仍指向旧库）存入旧库的记忆。
    const bool now_like = (owner_.editor_ != nullptr)
                          && owner_.editor_->IsMilkdropUseLikeLibrary();
    if (now_like)
      local_builtin_preset_index_ = local_current_preset_;
    else
      local_like_preset_index_ = local_current_preset_;

    ScanPresetFiles();

    int target = now_like ? local_like_preset_index_
                          : local_builtin_preset_index_;
    if (target < 0 || target >= local_preset_paths_.size())
      target = 0;
    local_current_preset_ = local_preset_paths_.isEmpty() ? -1 : target;

    if (local_current_preset_ >= 0) {
      while (juce::gl::glGetError() != juce::gl::GL_NO_ERROR) {}
      LoadCurrentPreset();
      while (juce::gl::glGetError() != juce::gl::GL_NO_ERROR) {}
      last_preset_switch_ms_ = static_cast<int64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now().time_since_epoch()).count());
    }
  }

  // 消费取消收藏触发的重扫请求：预设列表少一项，保持当前索引（后续预设顶上来），
  // 越界则回绕到 0；若收藏库因此被删空，则回退到内置库。
  if (requested_rescan_.exchange(false)) {
    ScanPresetFiles();
    if (local_preset_paths_.isEmpty()) {
      if (owner_.editor_ != nullptr)
        owner_.editor_->SetMilkdropUseLikeLibrary(false);
      ScanPresetFiles();
      int target = local_builtin_preset_index_;
      if (target < 0 || target >= local_preset_paths_.size())
        target = 0;
      local_current_preset_ = local_preset_paths_.isEmpty() ? -1 : target;
    } else if (local_current_preset_ >= local_preset_paths_.size()) {
      local_current_preset_ = 0;
    }

    if (local_current_preset_ >= 0) {
      while (juce::gl::glGetError() != juce::gl::GL_NO_ERROR) {}
      LoadCurrentPreset();
      while (juce::gl::glGetError() != juce::gl::GL_NO_ERROR) {}
      last_preset_switch_ms_ = static_cast<int64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now().time_since_epoch()).count());
    }
  }

  int jump = requested_preset_jump_.exchange(-1);
  int delta = requested_preset_delta_.exchange(0);
  bool random = requested_preset_random_.exchange(false);
  bool switched = false;

  if (jump >= 0 && jump < local_preset_paths_.size()) {
    local_current_preset_ = jump;
    switched = true;
  } else if (random && !local_preset_paths_.isEmpty()) {
    const int n = local_preset_paths_.size();
    if (n <= 1) {
      // 特殊处理：目录里只有一个预设，无法排除当前，重新加载当前预设
      // （软切换刷新），给用户一次"有反馈"的随机点击。
      if (local_current_preset_ < 0 || local_current_preset_ >= n)
        local_current_preset_ = 0;
      switched = true;
    } else {
      // 排除当前预设：从 n-1 个候选中均匀随机，选中 >= 当前索引则 +1 跳过。
      const int cur_val = local_current_preset_.load();
      const int cur = (cur_val >= 0 && cur_val < n) ? cur_val : -1;
      if (cur < 0) {
        local_current_preset_ = juce::Random::getSystemRandom().nextInt(n);
      } else {
        int next = juce::Random::getSystemRandom().nextInt(n - 1);
        if (next >= cur)
          ++next;
        local_current_preset_ = next;
      }
      switched = true;
    }
  } else if (delta != 0 && !local_preset_paths_.isEmpty()) {
    local_current_preset_ = (local_current_preset_.load() + delta) % local_preset_paths_.size();
    if (local_current_preset_.load() < 0)
      local_current_preset_.fetch_add(local_preset_paths_.size(), std::memory_order_relaxed);
    switched = true;
  }

  if (switched) {
    // 卡顿感缓解：将 shader 编译期间的“旧帧定格”换成黑帧，
    // 让用户感知为“切换中”而非“卡住”。
    // 同时前后清错误避免 Debug checkGLError 死循环。
    while (juce::gl::glGetError() != juce::gl::GL_NO_ERROR) {}
    juce::OpenGLHelpers::clear(juce::Colours::black);
    while (juce::gl::glGetError() != juce::gl::GL_NO_ERROR) {}
    LoadCurrentPreset();
    while (juce::gl::glGetError() != juce::gl::GL_NO_ERROR) {}
    last_preset_switch_ms_ = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
  }
}

void MilkdropModule::GLView::ConsumePcm() {
  std::vector<float> pcm;
  unsigned int frames = 0;
  {
    std::lock_guard<std::mutex> lock(pcm_mutex_);
    pcm.swap(pending_pcm_);
    frames = pending_frames_;
    pending_frames_ = 0;
  }
  if (!pcm.empty() && local_pm_handle_ != nullptr)
    projectm_api::Api::instance().addPcmFloat(local_pm_handle_, pcm.data(), frames, true);
}

void MilkdropModule::GLView::EnsureScaleFbo(int render_w, int render_h) {
  // 尺寸未变则直接复用
  if (scale_fbo_ != 0 && scale_fbo_w_ == render_w && scale_fbo_h_ == render_h)
    return;

  // 销毁旧的
  DestroyScaleFbo();

  // 创建颜色纹理
  juce::gl::glGenTextures(1, &scale_texture_);
  juce::gl::glBindTexture(juce::gl::GL_TEXTURE_2D, scale_texture_);
  juce::gl::glTexImage2D(juce::gl::GL_TEXTURE_2D, 0, juce::gl::GL_RGBA8,
                         render_w, render_h, 0,
                         juce::gl::GL_RGBA, juce::gl::GL_UNSIGNED_BYTE, nullptr);
  juce::gl::glTexParameteri(juce::gl::GL_TEXTURE_2D, juce::gl::GL_TEXTURE_MIN_FILTER, juce::gl::GL_LINEAR);
  juce::gl::glTexParameteri(juce::gl::GL_TEXTURE_2D, juce::gl::GL_TEXTURE_MAG_FILTER, juce::gl::GL_LINEAR);
  juce::gl::glBindTexture(juce::gl::GL_TEXTURE_2D, 0);

  // 创建 FBO 并附加纹理
  juce::gl::glGenFramebuffers(1, &scale_fbo_);
  juce::gl::glBindFramebuffer(juce::gl::GL_FRAMEBUFFER, scale_fbo_);
  juce::gl::glFramebufferTexture2D(juce::gl::GL_FRAMEBUFFER,
                                   juce::gl::GL_COLOR_ATTACHMENT0,
                                   juce::gl::GL_TEXTURE_2D,
                                   scale_texture_, 0);

  // FBO 完整性校验：projectM 渲染到自定义 FBO 依赖该状态。若在脱离模式上下文
  // 切换后 FBO 意外不完整，projectM 会静默输出黑帧，导致后处理对黑纹理做偏移。
  const GLenum status = juce::gl::glCheckFramebufferStatus (juce::gl::GL_FRAMEBUFFER);
  if (status != juce::gl::GL_FRAMEBUFFER_COMPLETE)
    local_error_ = "scale FBO incomplete: 0x"
                 + juce::String::toHexString (static_cast<int> (status));
  else
    local_error_.clear();

  Y2K_LOG("[MilkdropGLView] EnsureScaleFbo "
      + juce::String(render_w) + "x" + juce::String(render_h)
      + " fbo=" + juce::String(static_cast<int>(scale_fbo_))
      + " tex=" + juce::String(static_cast<int>(scale_texture_))
      + " status=0x" + juce::String::toHexString(static_cast<int>(status)));

  juce::gl::glBindFramebuffer(juce::gl::GL_FRAMEBUFFER, 0);

  scale_fbo_w_ = render_w;
  scale_fbo_h_ = render_h;
}

void MilkdropModule::GLView::DestroyScaleFbo() {
  if (scale_fbo_ != 0) {
    juce::gl::glDeleteFramebuffers(1, &scale_fbo_);
    scale_fbo_ = 0;
  }
  if (scale_texture_ != 0) {
    juce::gl::glDeleteTextures(1, &scale_texture_);
    scale_texture_ = 0;
  }
  scale_fbo_w_ = 0;
  scale_fbo_h_ = 0;
}

void MilkdropModule::GLView::renderOpenGL() {
  // 诊断：浮动态（脱离到独立窗口）时 projectM 由 GLView 自己的上下文渲染，
  //   但用的是同一条 JUCE GL 线程 —— 这里卡住会让整个程序的 GL 全部停摆，
  //   因此必须单独打阶段标记，否则日志里只会看到 Editor 的最后一步。
  y2k::diag::setThreadTag ("GL");
  y2k::diag::setPhase ("MilkdropGLView::renderOpenGL");
  y2k::diag::heartbeat (y2k::diag::Channel::GlFrame);

  juce::OpenGLHelpers::clear(juce::Colours::black);
  if (!local_render_ready_ || local_pm_handle_ == nullptr)
    return;

  auto& api = projectm_api::Api::instance();
  ConsumePresetRequests();
  ConsumePcm();

  // surface 的物理像素尺寸（含 HiDPI 缩放）
  auto dpi_scale = static_cast<float>(open_gl_context_.getRenderingScale());
  int surface_w = juce::jmax(1, static_cast<int>(getWidth()  * dpi_scale));
  int surface_h = juce::jmax(1, static_cast<int>(getHeight() * dpi_scale));

  // 读取全局视觉状态（master output colors + bright/invert/shadows 效果）。
  // isNeutral() 为 true 时保持原零拷贝/直接渲染路径，零额外开销。
  MilkdropVisualState visual_state;
  if (owner_.editor_ != nullptr)
    visual_state = owner_.editor_->GetMilkdropVisualState();
  const bool tint_active = !visual_state.isNeutral();

  // local_render_scale_：1 = 原始分辨率，2 = 半分辨率，4 = 四分之一分辨率。
  int render_w = juce::jmax(1, surface_w / local_render_scale_);
  int render_h = juce::jmax(1, surface_h / local_render_scale_);

  // 需要离屏渲染的情况：染色后处理，或降分辨率（local_render_scale_ > 1）。
  const bool need_offscreen = tint_active || (local_render_scale_ > 1);

  // 诊断日志节流：前 30 帧每帧输出，之后每 300 帧输出一次，避免刷屏。
  static int glDbgFrame = 0;
  ++glDbgFrame;
  const bool logFrame = (glDbgFrame <= 30) || (glDbgFrame % 300 == 0);
  if (logFrame) {
    Y2K_LOG("[MilkdropGLView] renderOpenGL frame=" + juce::String(glDbgFrame)
        + " tint=" + juce::String(tint_active ? 1 : 0)
        + " offscreen=" + juce::String(need_offscreen ? 1 : 0)
        + " hasFbo=" + juce::String(api.hasOpenglRenderFrameFbo() ? 1 : 0)
        + " scale=" + juce::String(local_render_scale_)
        + " surf=" + juce::String(surface_w) + "x" + juce::String(surface_h)
        + " render=" + juce::String(render_w) + "x" + juce::String(render_h));
  }

  if (need_offscreen) {
    // ---- 先渲染到离屏 FBO ----
    EnsureScaleFbo (render_w, render_h);

    // 优先使用 projectM 4.2+ 的 openglRenderFrameFbo（直接渲染到指定 FBO）。
    // 关键：必须像 Editor 嵌入态那样，在调用前先绑定 FBO、设置 viewport/scissor
    // 并 clear。否则 projectM 不会把画面写入 scale_fbo_（纹理保持全黑），
    // 后续后处理会对黑纹理做加性偏移/反相 → 纯色 / 纯黑 / 纯白。
    if (api.hasOpenglRenderFrameFbo()) {
      if (logFrame)
        Y2K_LOG("[MilkdropGLView] branch: openglRenderFrameFbo -> scale_fbo_");
      juce::gl::glBindFramebuffer (juce::gl::GL_FRAMEBUFFER, scale_fbo_);
      juce::gl::glViewport (0, 0, render_w, render_h);
      juce::gl::glScissor (0, 0, render_w, render_h);
      juce::gl::glEnable (juce::gl::GL_SCISSOR_TEST);
      juce::gl::glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
      juce::gl::glClear (juce::gl::GL_COLOR_BUFFER_BIT);
      api.setWindowSize (local_pm_handle_,
                         static_cast<std::size_t>(render_w),
                         static_cast<std::size_t>(render_h));
      Y2K_DIAG_PHASE ("gl:MilkdropGLView openglRenderFrameFbo");
      api.openglRenderFrameFbo (local_pm_handle_, scale_fbo_);
      juce::gl::glDisable (juce::gl::GL_SCISSOR_TEST);
    } else {
      // 回退：projectM 的 openglRenderFrame 内部会强制 glBindFramebuffer(0)，
      // 不能预先绑定 scale_fbo_ 并期望它渲染到该 FBO——否则画面会写到
      // framebuffer 0，scale_fbo_ 保持黑。这里与 Editor 降级路径保持一致：
      // 先渲染到 framebuffer 0，再跨 FBO blit 到 scale_fbo_。
      if (logFrame)
        Y2K_LOG("[MilkdropGLView] branch: openglRenderFrame -> FBO0, blit -> scale_fbo_");
      juce::gl::glBindFramebuffer (juce::gl::GL_FRAMEBUFFER, 0);
      juce::gl::glViewport (0, 0, render_w, render_h);
      juce::gl::glScissor (0, 0, render_w, render_h);
      juce::gl::glEnable (juce::gl::GL_SCISSOR_TEST);
      juce::gl::glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
      juce::gl::glClear (juce::gl::GL_COLOR_BUFFER_BIT);
      api.setWindowSize (local_pm_handle_,
                         static_cast<std::size_t>(render_w),
                         static_cast<std::size_t>(render_h));
      api.openglRenderFrame (local_pm_handle_);
      juce::gl::glDisable (juce::gl::GL_SCISSOR_TEST);

      // ⚠ 这里不用 glBlitFramebuffer：外网 RTX 3070 / NVIDIA 595.79 现场实测
      //   "默认后缓冲 → 纹理 FBO" 的 blit 会被驱动拒绝（GL_INVALID_OPERATION 0x502），
      //   结果离屏 FBO 一直是空的 → 画面纯黑且所有健康指标正常。
      //   glCopyTexSubImage2D 走的是另一条（1.1 时代就有、极稳的）拷贝路径，同为 1:1 拷屏。
      juce::gl::glBindFramebuffer (juce::gl::GL_READ_FRAMEBUFFER, 0);
      juce::gl::glBindFramebuffer (juce::gl::GL_DRAW_FRAMEBUFFER, scale_fbo_);
      juce::gl::glBindTexture (juce::gl::GL_TEXTURE_2D, scale_texture_);
      juce::gl::glCopyTexSubImage2D (juce::gl::GL_TEXTURE_2D, 0,
                                     0, 0,          // 目标偏移（纹理内）
                                     0, 0,          // 源左下角（后缓冲内）
                                     render_w, render_h);
      juce::gl::glBindTexture (juce::gl::GL_TEXTURE_2D, 0);
      juce::gl::glBindFramebuffer (juce::gl::GL_FRAMEBUFFER, 0);
    }

    // projectM 渲染完成后的 GL error 检查（定位 scale_fbo_ 是否成功接收画面）
    {
      const GLenum pm_err = juce::gl::glGetError();
      if (pm_err != juce::gl::GL_NO_ERROR && logFrame)
        Y2K_LOG("[MilkdropGLView] projectM GL error=0x"
            + juce::String::toHexString(static_cast<int>(pm_err)));
    }

    if (tint_active) {
      // ---- 染色后处理：采样离屏纹理，应用 RGB 增益，绘制全屏三角形 ----
      if (logFrame)
        Y2K_LOG("[MilkdropGLView] branch: tint apply (tex="
            + juce::String(static_cast<int>(scale_texture_)) + ")");
      if (tint_pass_ == nullptr)
        tint_pass_.reset (new MilkdropTintPass (open_gl_context_));
      if (!tint_pass_->isReady())
        tint_pass_->init();

      // 统一 pass：采样 scale_fbo_ 纹理，应用完整视觉状态，渲染到 framebuffer 0。
      // shadows 等效果均在单帧内完成，无需上一帧累积（与 MilkDrop3 注入语义一致）。
      juce::gl::glBindFramebuffer (juce::gl::GL_FRAMEBUFFER, 0);
      juce::gl::glViewport (0, 0, surface_w, surface_h);

      // apply() 内部已经"画完查错"：返回 false 表示这一帧没画成（program 失效 /
      //   FBO 状态非法），此时后缓冲是空的 —— 必须退回"直接把离屏纹理搬上去"，
      //   宁可丢染色也不能整块纯黑。
      const bool tintOk = tint_pass_->apply (scale_texture_, visual_state);

      if (! tintOk)
      {
        while (juce::gl::glGetError() != juce::gl::GL_NO_ERROR) {}
        juce::gl::glBindFramebuffer (juce::gl::GL_READ_FRAMEBUFFER, scale_fbo_);
        juce::gl::glBindFramebuffer (juce::gl::GL_DRAW_FRAMEBUFFER, 0);
        juce::gl::glBlitFramebuffer (
            0, 0, render_w, render_h,
            0, 0, surface_w, surface_h,
            juce::gl::GL_COLOR_BUFFER_BIT,
            juce::gl::GL_NEAREST);
        juce::gl::glBindFramebuffer (juce::gl::GL_FRAMEBUFFER, 0);

        if (logFrame)
          Y2K_LOG("[MilkdropGLView] tint pass 未画成 → 回退为无染色输出");
      }
    } else {
      // ---- 降分辨率无染色：blit 拉伸到默认 framebuffer ----
      // 注意：macOS OpenGL Core Profile 对拉伸 blit 有严格限制（须 GL_NEAREST）。
      while (juce::gl::glGetError() != juce::gl::GL_NO_ERROR) {}
      juce::gl::glBindFramebuffer (juce::gl::GL_READ_FRAMEBUFFER, scale_fbo_);
      juce::gl::glBindFramebuffer (juce::gl::GL_DRAW_FRAMEBUFFER, 0);
      juce::gl::glBlitFramebuffer (
          0, 0, render_w, render_h,
          0, 0, surface_w, surface_h,
          juce::gl::GL_COLOR_BUFFER_BIT,
          juce::gl::GL_NEAREST);
      juce::gl::glBindFramebuffer (juce::gl::GL_FRAMEBUFFER, 0);
    }
  } else {
    // ---- 1:1 无染色：直接渲染到默认 framebuffer，无需离屏 FBO ----
    DestroyScaleFbo();
    api.setWindowSize (local_pm_handle_,
                       static_cast<std::size_t>(surface_w),
                       static_cast<std::size_t>(surface_h));
    juce::gl::glBindFramebuffer (juce::gl::GL_FRAMEBUFFER, 0);
    juce::gl::glViewport (0, 0, surface_w, surface_h);
    Y2K_DIAG_PHASE ("gl:MilkdropGLView openglRenderFrame (1:1)");
    api.openglRenderFrame (local_pm_handle_);
  }
}

void MilkdropModule::GLView::PushPcm(const float* interleaved_lr,
                                      unsigned int frame_count) {
  // 嵌入态转发给 Editor renderer（Editor GL 消费），浮动态留在本地队列。
  if (owner_.editor_ != nullptr && !owner_.isFloating())
    owner_.editor_->PushMilkdropPcm(interleaved_lr, frame_count);

  std::lock_guard<std::mutex> lock(pcm_mutex_);
  pending_pcm_.assign(interleaved_lr, interleaved_lr + frame_count * 2);
  pending_frames_ = frame_count;
}

void MilkdropModule::GLView::RequestPresetDelta(int delta) {
  if (owner_.isFloating()) {
    requested_preset_delta_.fetch_add(delta);
  } else if (owner_.editor_ != nullptr) {
    owner_.restored_preset_index_ = -1;
    owner_.editor_->RequestMilkdropPresetDelta(delta);
  }
}

void MilkdropModule::GLView::RequestPresetRandom() {
  if (owner_.isFloating()) {
    if (!local_preset_paths_.isEmpty())
      requested_preset_jump_.store(juce::Random::getSystemRandom().nextInt(local_preset_paths_.size()));
  } else if (owner_.editor_ != nullptr) {
    owner_.restored_preset_index_ = -1;
    owner_.editor_->RequestMilkdropPresetRandom();
  }
}

void MilkdropModule::GLView::RequestPresetJump(int index) {
  if (owner_.isFloating()) {
    requested_preset_jump_.store(index);
  } else if (owner_.editor_ != nullptr) {
    owner_.restored_preset_index_ = -1;
    owner_.editor_->RequestMilkdropPresetJump(index);
  }
}

void MilkdropModule::GLView::RequestLibraryToggle() {
  // 浮动态：翻转 Editor 全局收藏库状态并置本地重扫标志，
  //   由 ConsumePresetRequests 在本地 GL 线程消费。
  // 嵌入态：由 Editor GL 线程消费切换请求，无需本地重扫。
  if (owner_.isFloating()) {
    if (owner_.editor_ != nullptr)
      owner_.editor_->ToggleMilkdropLibraryState();
    requested_library_toggle_.store(true);
  } else if (owner_.editor_ != nullptr) {
    owner_.editor_->RequestMilkdropToggleLibrary();
  }
}

void MilkdropModule::GLView::RequestUnlinkReload() {
  // 取消收藏后（当前浏览收藏库时），预设列表少一项，需重扫并切换到下一个预设。
  // 嵌入态由 Editor GL 线程重扫；浮动态由本地 GL 线程重扫。
  if (owner_.isFloating()) {
    requested_rescan_.store(true);
  } else if (owner_.editor_ != nullptr) {
    owner_.editor_->RequestMilkdropUnlinkReload();
  }
}

void MilkdropModule::GLView::RequestRenderScale() {
  // macOS：分辨率切换功能在 macOS 上已阉割（glBlitFramebuffer 兼容性问题），
  //   强制保持 1:1，忽略所有切换请求。
#if JUCE_MAC
  local_render_scale_ = 1;  // 始终 1:1，不允许切换
  return;
#else
  if (owner_.isFloating()) {
    local_render_scale_ = (local_render_scale_ == 1) ? 2 : (local_render_scale_ == 2 ? 4 : 1);
  } else if (owner_.editor_ != nullptr) {
    owner_.editor_->RequestMilkdropRenderScale();
  }
#endif
}

juce::String MilkdropModule::diagDescribeGlView() const
{
  if (glView == nullptr)
    return "glView=null";

  juce::String s;
  s << "ready="      << (glView->IsRenderReady() ? 1 : 0)
    << " float="     << (isFloating() ? 1 : 0)
    << " glAttached=" << (glView->IsGLAttached() ? 1 : 0)
    << " onDesktop=" << (glView->isOnDesktop() ? 1 : 0)
    << " timer="     << (int) glView->diagTimerTicks;

  glView->diagTimerTicks = 0;
  return s;
}

bool MilkdropModule::GLView::IsRenderReady() const {
  // 嵌入态由 Editor GL 渲染，浮动态由本地上下文渲染。
  if (owner_.isFloating())
    return local_render_ready_;
  if (owner_.editor_ != nullptr)
    return owner_.editor_->IsMilkdropRenderReady();
  return false;
}

juce::String MilkdropModule::GLView::GetError() const {
  if (owner_.isFloating())
    return local_error_;
  if (owner_.editor_ != nullptr)
    return owner_.editor_->GetMilkdropError();
  return "Editor not found";
}

int MilkdropModule::GLView::GetCurrentPresetIndex() const {
  const bool use_local = owner_.isFloating() || attached_;
  if (use_local) {
    std::lock_guard<std::mutex> lock(preset_paths_mutex_);
    const int total = local_preset_paths_.size();
    if (total <= 0)
      return local_current_preset_;

    const int jump = requested_preset_jump_.load();
    if (jump >= 0 && jump < total)
      return jump;

    const int delta = requested_preset_delta_.load();
    if (delta != 0) {
      int current = local_current_preset_;
      if (current < 0 || current >= total)
        current = 0;
      current = (current + delta) % total;
      if (current < 0)
        current += total;
      return current;
    }

    return local_current_preset_;
  }
  if (owner_.editor_ != nullptr)
    return owner_.editor_->GetMilkdropCurrentPresetIndex();
  return -1;
}

void MilkdropModule::GLView::SyncOwnerPresetIndexFromRenderer() const {
  const int preset_index = GetCurrentPresetIndex();
  if (preset_index >= 0)
    owner_.restored_preset_index_ = preset_index;
}

int MilkdropModule::GLView::GetTotalPresetCount() const {
  if (owner_.isFloating()) {
    std::lock_guard<std::mutex> lock(preset_paths_mutex_);
    return local_preset_paths_.size();
  }
  if (owner_.editor_ != nullptr)
    return owner_.editor_->GetMilkdropTotalPresets();
  return 0;
}

juce::String MilkdropModule::GLView::GetCurrentPresetName() const {
  const bool use_local = owner_.isFloating();
  if (use_local) {
    std::lock_guard<std::mutex> lock(preset_paths_mutex_);
    const int current = local_current_preset_.load();
    if (current >= 0 && current < local_preset_paths_.size()) {
      return local_preset_paths_[current]
          .fromLastOccurrenceOf("/", false, false)
          .fromLastOccurrenceOf("\\", false, false)
          .upToLastOccurrenceOf(".milk", false, false);
    }
    return {};
  }
  if (owner_.editor_ != nullptr)
    return owner_.editor_->GetMilkdropCurrentPresetName();
  return {};
}

juce::String MilkdropModule::GLView::GetCurrentPresetFilePath() const {
  const bool use_local = owner_.isFloating();
  if (use_local) {
    std::lock_guard<std::mutex> lock(preset_paths_mutex_);
    const int current = local_current_preset_.load();
    if (current >= 0 && current < local_preset_paths_.size())
      return local_preset_paths_[current];
    return {};
  }
  if (owner_.editor_ != nullptr)
    return owner_.editor_->GetMilkdropCurrentPresetFilePath();
  return {};
}

int64_t MilkdropModule::GLView::GetLastPresetSwitchTimeMs() const {
  if (owner_.isFloating())
    return last_preset_switch_ms_;
  if (owner_.editor_ != nullptr)
    return owner_.editor_->GetMilkdropLastPresetSwitchTimeMs();
  return 0;
}

void MilkdropModule::GLView::timerCallback() {
  ++diagTimerTicks;
  UpdateOpenGLAttachment();
  if (!IsRenderReady()) return;

  // ---- 首次自动激活焦点（仅一次）----
  if (!first_focus_done_) {
    first_focus_done_ = true;
    if (!owner_.focused_) {
      owner_.setFocusVisual(true);
      owner_.touchOverlayIdleTimer();
    }
  }

  // ---- Auto-hide / Auto 轮播检测 ----
  owner_.checkOverlayAutoHide();
  owner_.checkAutoMode();
  owner_.repaint();
  // 脱离态下控制栏由 GLView::paint 覆盖绘制，必须显式触发 GLView 重绘，
  // 否则自动隐藏 / hover / auto 展开等状态变化不会刷新到画面。
  repaint();
#if JUCE_MAC
  // macOS：顶层控制栏悬浮窗需要跟随模块屏幕位置、可见性、hover/press 高亮实时刷新。
  // 与主 UI 重绘 30Hz 同步，既能跟踪窗口拖动/resize，又保证 hover 反馈及时。
  owner_.UpdateOverlayViewPlacement();
  // 控制栏窗口会吞掉鼠标事件 → 提示只能用轮询（见 updateOverlayHoverTip 注释）
  owner_.updateOverlayHoverTip();
  if (owner_.overlayView_ != nullptr && owner_.overlayView_->isOnDesktop())
      owner_.overlayView_->repaint();
#endif
}

// ---- GLView mouse forwarding ----

void MilkdropModule::GLView::mouseDown(const juce::MouseEvent& e) {
  if (auto* parent = getParentComponent())
    parent->mouseDown(e.getEventRelativeTo(parent));
}

void MilkdropModule::GLView::mouseUp(const juce::MouseEvent& e) {
  if (auto* parent = getParentComponent())
    parent->mouseUp(e.getEventRelativeTo(parent));
}

void MilkdropModule::GLView::mouseMove(const juce::MouseEvent& e) {
  if (auto* parent = getParentComponent())
    parent->mouseMove(e.getEventRelativeTo(parent));
}

void MilkdropModule::GLView::mouseExit(const juce::MouseEvent& e) {
  if (auto* parent = getParentComponent())
    parent->mouseExit(e.getEventRelativeTo(parent));
}

void MilkdropModule::GLView::mouseDrag(const juce::MouseEvent& e) {
  if (auto* parent = getParentComponent())
    parent->mouseDrag(e.getEventRelativeTo(parent));
}


// ==========================================================
// MilkdropModule —— 焦点与叠加层交互
// ==========================================================
void MilkdropModule::setFocusVisual(bool shouldFocus)
{
    if (focused_ == shouldFocus)
        return;

    focused_ = shouldFocus;
    if (!focused_)
    {
        hoveredOverlayBtn_ = OverlayButton::kNone;
        pressedOverlayBtn_ = OverlayButton::kNone;
    }
    else
    {
        touchOverlayIdleTimer();  // 聚焦时重置 4 秒倒计时
    }

    // 控制栏以 overlay 方式绘制，不改变 GLView 尺寸，无需重新 layout。
#if JUCE_MAC
    // macOS: Overlay 可见性同步（非聚焦时隐藏内部控制栏窗口）。
    // 注意：Milkdrop 模块在 macOS 非脱离模式下始终置顶（由
    // ModuleWorkspace::onBroughtToFront 统一执行 toFront），因为 NSOpenGLView
    // 作为 AppKit 原生子视图无法被 CoreGraphics 绘制的其他模块遮盖，
    // 为保持“边框/抬头/视频”视觉一致，整个模块都预为最上层。
    UpdateOverlayViewPlacement();
#endif
    repaint();
    // 脱离态控制栏由 GLView::paint 覆盖绘制，聚焦/失焦需同步触发 GLView 重绘，
    // 否则自动隐藏后控制栏不会从画面消失。
    if (glView != nullptr)
        glView->repaint();
}

void MilkdropModule::checkOverlayAutoHide()
{
  if (!focused_)
    return;

  // color / effects / wave / tweak 面板展开期间不自动隐藏，避免用户调整或撤回查看参数时控制台中途消失。
  // auto 模式下允许自动隐藏：进入"纯粹可视化"状态，hover 时重新显示控制栏。
  if (isColorPanelOpen_ || isEffectsPanelOpen_ || isWavePanelOpen_ || isTweakPanelOpen_)
    return;

  // overlay 无交互超过 4 秒 → 自动隐藏
  if (juce::Time::getMillisecondCounter() - lastInteractionTime_ >= 4000)
  {
    setFocusVisual(false);
  }
}

void MilkdropModule::mouseDown(const juce::MouseEvent& e)
{
    // 右键 → 仅嵌入态允许冒泡给 workspace 弹出"添加模块"菜单。
    // 浮动窗口内禁用添加菜单，避免脱离后右键误触发主窗口菜单。
    if (e.mods.isPopupMenu())
    {
        if (!isFloating() && onRightClick)
            onRightClick(*this, e.getPosition());
        return;
    }

    // ---- auto 行 slider 拖动检测（必须在基类之前，避免基类启动拖拽状态） ----
    if (isAutoRowExpanded_ && glView != nullptr && !isDraggingSlider_)
    {
        auto content = getContentBounds();
        auto topBar = content.withHeight(26);
        auto autoRow = getAutoRowBounds(topBar);

        // ---- 模式切换按钮点击（sec ↔ beat，仅插件宿主模式） ----
        if (!cachedAutoModeBtnRect_.isEmpty() &&
            cachedAutoModeBtnRect_.contains(e.getPosition()))
        {
            if (!focused_)
                setFocusVisual(true);
            touchOverlayIdleTimer();
            toggleAutoModeType();
            return;
        }

        auto sliderBounds = getSliderBounds(autoRow);
        if (sliderBounds.expanded(4).contains(e.getPosition()))
        {
            notifyBeforeUserEdit();
            isDraggingSlider_ = true;
            if (!focused_)
                setFocusVisual(true);
            touchOverlayIdleTimer();
            float proportion = static_cast<float>(e.getPosition().x - sliderBounds.getX())
                               / static_cast<float>(sliderBounds.getWidth());
            if (autoModeIsBeat_)
                updateAutoBeatsFromSlider(proportion);
            else
                updateAutoIntervalFromSlider(proportion);
            repaint(autoRow);
            glView->repaint();
            return;  // 不调用基类，避免 ModulePanel::mouseDown 启动标题栏/边缘拖拽
        }
        // ---- 时间标签点击检测（仅时间模式弹出间隔输入对话框） ----
        if (!autoModeIsBeat_ && cachedAutoTimeLabel_.contains(e.getPosition()))
        {
            if (!focused_)
                setFocusVisual(true);
            touchOverlayIdleTimer();
            showAutoIntervalDialog();
            return;
        }
    }

    // ---- color panel 交互检测（Reset + RGB 滑块拖动） ----
    if (isColorPanelOpen_ && glView != nullptr)
    {
        auto content = getContentBounds();
        auto topBar = content.withHeight(26);
        auto panel = getColorPanelBounds(topBar);

        // Reset 按钮点击（重置 RGB + Bright 四行）
        if (getTintResetBounds(panel).contains(e.getPosition()))
        {
            notifyBeforeUserEdit();
            if (!focused_)
                setFocusVisual(true);
            touchOverlayIdleTimer();
            visualState_.tint_r = 1.0f;
            visualState_.tint_g = 1.0f;
            visualState_.tint_b = 1.0f;
            visualState_.brightness = 1.0f;
            applyVisualToEditor();
            notifyAfterUserEdit (y2k::tr ("MilkDrop color"), "color");
            repaint(topBar);
            repaint(panel);
            glView->repaint();
            return;
        }

        // RGB + Bright 滑块拖动
        for (int row = 0; row < 4; ++row)
        {
            auto sb = getTintSliderBounds(panel, row);
            if (sb.expanded(6).contains(e.getPosition()))
            {
                notifyBeforeUserEdit();
                draggingTintRow_ = row;
                if (!focused_)
                    setFocusVisual(true);
                touchOverlayIdleTimer();
                float proportion = static_cast<float>(e.getPosition().x - sb.getX())
                                   / static_cast<float>(sb.getWidth());
                updateTintFromSlider(row, proportion);
                repaint(topBar);
                repaint(panel);
                glView->repaint();
                return;
            }
        }
    }

    // ---- effects panel 交互检测（Reset + invert/shadows 开关） ----
    if (isEffectsPanelOpen_ && glView != nullptr)
    {
        auto content = getContentBounds();
        auto topBar = content.withHeight(26);
        auto panel = getEffectsPanelBounds(topBar);

        // Reset 按钮点击（重置所有已实现效果）
        if (getEffectsResetBounds(panel).contains(e.getPosition()))
        {
            notifyBeforeUserEdit();
            if (!focused_)
                setFocusVisual(true);
            touchOverlayIdleTimer();
            for (const auto& def : GetMilkdropEffectDefs())
                if (def.implemented)
                    def.set(visualState_, false);
            applyVisualToEditor();
            notifyAfterUserEdit (y2k::tr ("MilkDrop effects"), "effects");
            repaint(topBar);
            repaint(panel);
            glView->repaint();
            return;
        }

        // 开关点击（遍历已实现效果）
        const int effect_count = CountImplementedMilkdropEffects();
        for (int row = 0; row < effect_count; ++row)
        {
            auto toggle = getEffectsToggleBounds(panel, row);
            if (toggle.expanded(4).contains(e.getPosition()))
            {
                if (!focused_)
                    setFocusVisual(true);
                touchOverlayIdleTimer();
                toggleEffectSwitch(row);
                repaint(topBar);
                repaint(panel);
                glView->repaint();
                return;
            }
        }
    }

    // ---- wave panel 交互检测（Reset + mode stepper + 开关 + slider 拖动） ----
    if (isWavePanelOpen_ && glView != nullptr)
    {
        auto content = getContentBounds();
        auto topBar = content.withHeight(26);
        auto panel = getWavePanelBounds(topBar);

        // Reset 按钮点击（恢复默认，关闭覆盖）
        if (getWaveResetBounds(panel).contains(e.getPosition()))
        {
            notifyBeforeUserEdit();
            if (!focused_)
                setFocusVisual(true);
            touchOverlayIdleTimer();
            waveState_.reset();
            applyWaveToEditor();
            reloadCurrentPresetForWave();
            notifyAfterUserEdit (y2k::tr ("MilkDrop wave"), "wave");
            repaint(topBar);
            repaint(panel);
            glView->repaint();
            return;
        }

        // mode stepper：左半区 -1，右半区 +1（循环 0~15）
        auto stepper = getWaveModeStepperBounds(panel);
        if (stepper.contains(e.getPosition()))
        {
            notifyBeforeUserEdit();
            if (!focused_)
                setFocusVisual(true);
            touchOverlayIdleTimer();
            const int delta = (e.getPosition().x < stepper.getCentreX()) ? -1 : +1;
            waveModeStepperPressed_ = (delta < 0) ? 0 : 1;
            waveState_.mode = (waveState_.mode + delta + 16) % 16;
            waveState_.enabled = true;
            applyWaveToEditor();
            reloadCurrentPresetForWave();
            notifyAfterUserEdit (y2k::tr ("MilkDrop wave"), "wave");
            repaint(topBar);
            repaint(panel);
            glView->repaint();
            return;
        }

        // 开关点击（usedots / thick / additive / brighten）
        for (int idx = 0; idx < 4; ++idx)
        {
            auto sw = getWaveSwitchBounds(panel, idx);
            if (sw.expanded(4).contains(e.getPosition()))
            {
                notifyBeforeUserEdit();
                if (!focused_)
                    setFocusVisual(true);
                touchOverlayIdleTimer();
                bool* target = (idx == 0) ? &waveState_.usedots
                             : (idx == 1) ? &waveState_.thick
                             : (idx == 2) ? &waveState_.additive
                                          : &waveState_.brighten;
                *target = !*target;
                waveState_.enabled = true;
                applyWaveToEditor();
                reloadCurrentPresetForWave();
                notifyAfterUserEdit (y2k::tr ("MilkDrop wave"), "wave");
                repaint(topBar);
                repaint(panel);
                glView->repaint();
                return;
            }
        }

        // slider 拖动（X / Y / R / G / B / A / Mystery）
        for (int row = 0; row < 7; ++row)
        {
            auto sb = getWaveSliderBounds(panel, row);
            if (sb.expanded(6).contains(e.getPosition()))
            {
                notifyBeforeUserEdit();
                draggingWaveRow_ = row;
                if (!focused_)
                    setFocusVisual(true);
                touchOverlayIdleTimer();
                float proportion = static_cast<float>(e.getPosition().x - sb.getX())
                                   / static_cast<float>(sb.getWidth());
                updateWaveFromSlider(row, proportion);
                repaint(topBar);
                repaint(panel);
                glView->repaint();
                return;
            }
        }
    }

    // ---- tweak panel 交互检测（Reset + 双向偏移滑块拖动） ----
    if (isTweakPanelOpen_ && glView != nullptr)
    {
        auto content = getContentBounds();
        auto topBar = content.withHeight(26);
        auto panel = getTweakPanelBounds(topBar);

        // Reset 按钮点击（恢复默认，所有偏移归零，实时生效）
        if (getTweakResetBounds(panel).contains(e.getPosition()))
        {
            notifyBeforeUserEdit();
            if (!focused_)
                setFocusVisual(true);
            touchOverlayIdleTimer();
            visualState_.offset.reset();
            applyVisualToEditor();
            notifyAfterUserEdit (y2k::tr ("MilkDrop tweak"), "tweak");
            repaint(topBar);
            repaint(panel);
            glView->repaint();
            return;
        }

        // 整数滑块拖动（kaleido / fold_x / fold_y，1~16）
        for (int row = 0; row < MilkdropVisualOffsetState::kIntParamCount; ++row)
        {
            auto sb = getTweakIntSliderBounds(panel, row);
            if (sb.expanded(6).contains(e.getPosition()))
            {
                notifyBeforeUserEdit();
                draggingTweakIntRow_ = row;
                if (!focused_)
                    setFocusVisual(true);
                touchOverlayIdleTimer();
                float proportion = static_cast<float>(e.getPosition().x - sb.getX())
                                   / static_cast<float>(sb.getWidth());
                updateTweakIntFromSlider(row, proportion);
                repaint(topBar);
                repaint(panel);
                glView->repaint();
                return;
            }
        }

        // 双向偏移滑块拖动（zoom/rot/warp/dx/dy/sx/sy）
        for (int row = 0; row < MilkdropVisualOffsetState::kParamCount; ++row)
        {
            auto sb = getTweakSliderBounds(panel, row);
            if (sb.expanded(6).contains(e.getPosition()))
            {
                notifyBeforeUserEdit();
                draggingTweakRow_ = row;
                if (!focused_)
                    setFocusVisual(true);
                touchOverlayIdleTimer();
                float proportion = static_cast<float>(e.getPosition().x - sb.getX())
                                   / static_cast<float>(sb.getWidth());
                updateTweakFromSlider(row, proportion);
                repaint(topBar);
                repaint(panel);
                glView->repaint();
                return;
            }
        }
    }

    // 基类处理：toFront + onBroughtToFront + 关闭按钮 + 缩放边缘 + 标题栏拖动
    // 所有涉及 private 成员的逻辑（closeButtonPressed / dragMode / detectEdge 等）
    // 均由基类完成，我们只在上层附加 overlay 按钮处理。
    ModulePanel::mouseDown(e);

    setFocusVisual(true);

    // 布局锁定只锁"布局交互"（拖拽 / 缩放 / 关闭，已在基类 ModulePanel::mouseDown
    // 内部单独拦截）；模块内容按钮（预设切换 / color / 收藏 / 库 / auto …）属于
    // 内容交互，锁定态下必须继续可用。
    // v3.0.1 修复：旧实现在这里对非裸模式直接 return，导致开启右上角 L 锁定后
    // milkdrop 模块内部所有按钮都点不动（与上面的注释自相矛盾）。

    // 内容区 overlay 按钮点击（顶部控制栏 + 右下角收藏/切换按钮）
    if (focused_ && glView != nullptr)
    {
        auto content = getContentBounds();
        auto overlay = getOverlayBounds(content);
        auto btn = hitTestOverlayButton(e.getPosition(), overlay);
        if (btn == OverlayButton::kNone)
            btn = hitTestLibraryButton(e.getPosition(), content);
        if (btn != OverlayButton::kNone)
        {
            pressedOverlayBtn_ = btn;
            touchOverlayIdleTimer();
            repaint(overlay);
            repaint(content);
            glView->repaint();
        }
    }
}

void MilkdropModule::mouseUp(const juce::MouseEvent& e)
{
    // slider 拖动结束
    if (isDraggingSlider_)
    {
        isDraggingSlider_ = false;
        // 拖动结束提交：把 auto_interval 回写宿主参数，避免 10Hz pull 反向覆盖。
        if (editor_ != nullptr)
            editor_->pushMilkdropAutomationParams();
        notifyAfterUserEdit (y2k::tr ("MilkDrop auto"), "auto_row");
        notifyUserGestureEnd();
        repaint();
        glView->repaint();
        return;
    }

    // color/bright slider 拖动结束
    if (draggingTintRow_ >= 0)
    {
        draggingTintRow_ = -1;
        notifyAfterUserEdit (y2k::tr ("MilkDrop color"), "color");
        notifyUserGestureEnd();
        repaint();
        glView->repaint();
        return;
    }

    // wave slider 拖动结束：应用当前覆盖并重载预设（重载在松手时一次性触发）
    if (draggingWaveRow_ >= 0)
    {
        draggingWaveRow_ = -1;
        reloadCurrentPresetForWave();
        notifyAfterUserEdit (y2k::tr ("MilkDrop wave"), "wave");
        notifyUserGestureEnd();
        repaint();
        glView->repaint();
        return;
    }

    // tweak 整数滑块拖动结束：实时生效，仅复位拖动状态
    if (draggingTweakIntRow_ >= 0)
    {
        draggingTweakIntRow_ = -1;
        notifyAfterUserEdit (y2k::tr ("MilkDrop tweak"), "tweak");
        notifyUserGestureEnd();
        repaint();
        glView->repaint();
        return;
    }

    // tweak slider 拖动结束：实时生效，仅复位拖动状态
    if (draggingTweakRow_ >= 0)
    {
        draggingTweakRow_ = -1;
        notifyAfterUserEdit (y2k::tr ("MilkDrop tweak"), "tweak");
        notifyUserGestureEnd();
        repaint();
        glView->repaint();
        return;
    }

    // mode stepper 按下结束
    if (waveModeStepperPressed_ >= 0)
    {
        waveModeStepperPressed_ = -1;
        repaint();
        glView->repaint();
        return;
    }

    // 优先处理 overlay 按钮释放（顶部控制栏 + 右下角收藏/切换按钮）
    if (pressedOverlayBtn_ != OverlayButton::kNone)
    {
        auto content = getContentBounds();
        auto overlay = getOverlayBounds(content);
        auto hit = hitTestOverlayButton(e.getPosition(), overlay);
        if (hit == OverlayButton::kNone)
            hit = hitTestLibraryButton(e.getPosition(), content);
        if (hit == pressedOverlayBtn_)
        {
            if (hit == OverlayButton::kLike || hit == OverlayButton::kLibraryToggle)
                executeLibraryAction(hit);
            else
                executeOverlayAction(hit);
        }

        pressedOverlayBtn_ = OverlayButton::kNone;
        repaint(overlay);
        repaint(content);
        glView->repaint();
        return;
    }

    // 否则走基类（关闭按钮释放、拖拽/缩放收尾）
    ModulePanel::mouseUp(e);
}

void MilkdropModule::mouseMove(const juce::MouseEvent& e)
{
    // slider 拖动中
    if (isDraggingSlider_)
    {
        auto content = getContentBounds();
        auto topBar = content.withHeight(26);
        auto autoRow = getAutoRowBounds(topBar);
        auto sliderBounds = getSliderBounds(autoRow);
        float proportion = static_cast<float>(e.getPosition().x - sliderBounds.getX())
                           / static_cast<float>(sliderBounds.getWidth());
        if (autoModeIsBeat_)
            updateAutoBeatsFromSlider(proportion);
        else
            updateAutoIntervalFromSlider(proportion);
        repaint(autoRow);
        glView->repaint();
        return;
    }

    // color/bright slider 拖动中
    if (draggingTintRow_ >= 0)
    {
        touchOverlayIdleTimer();
        auto content = getContentBounds();
        auto topBar = content.withHeight(26);
        auto panel = getColorPanelBounds(topBar);
        auto sb = getTintSliderBounds(panel, draggingTintRow_);
        float proportion = static_cast<float>(e.getPosition().x - sb.getX())
                           / static_cast<float>(sb.getWidth());
        updateTintFromSlider(draggingTintRow_, proportion);
        repaint(panel);
        glView->repaint();
        return;
    }

    // wave slider 拖动中（只更新本地状态，不重载，松手时统一重载）
    if (draggingWaveRow_ >= 0)
    {
        touchOverlayIdleTimer();
        auto content = getContentBounds();
        auto topBar = content.withHeight(26);
        auto panel = getWavePanelBounds(topBar);
        auto sb = getWaveSliderBounds(panel, draggingWaveRow_);
        float proportion = static_cast<float>(e.getPosition().x - sb.getX())
                           / static_cast<float>(sb.getWidth());
        updateWaveFromSlider(draggingWaveRow_, proportion);
        repaint(panel);
        glView->repaint();
        return;
    }

    // tweak slider 拖动中（只更新本地状态，不重载，松手时统一重载）
    if (draggingTweakRow_ >= 0)
    {
        touchOverlayIdleTimer();
        auto content = getContentBounds();
        auto topBar = content.withHeight(26);
        auto panel = getTweakPanelBounds(topBar);
        auto sb = getTweakSliderBounds(panel, draggingTweakRow_);
        float proportion = static_cast<float>(e.getPosition().x - sb.getX())
                           / static_cast<float>(sb.getWidth());
        updateTweakFromSlider(draggingTweakRow_, proportion);
        repaint(panel);
        glView->repaint();
        return;
    }

    // tweak 整数滑块拖动中
    if (draggingTweakIntRow_ >= 0)
    {
        touchOverlayIdleTimer();
        auto content = getContentBounds();
        auto topBar = content.withHeight(26);
        auto panel = getTweakPanelBounds(topBar);
        auto sb = getTweakIntSliderBounds(panel, draggingTweakIntRow_);
        float proportion = static_cast<float>(e.getPosition().x - sb.getX())
                           / static_cast<float>(sb.getWidth());
        updateTweakIntFromSlider(draggingTweakIntRow_, proportion);
        repaint(panel);
        glView->repaint();
        return;
    }

    // mode stepper 悬停检测（wave 面板打开且聚焦时）
    if (focused_ && isWavePanelOpen_ && glView != nullptr)
    {
        auto content = getContentBounds();
        auto topBar = content.withHeight(26);
        auto panel = getWavePanelBounds(topBar);
        auto stepper = getWaveModeStepperBounds(panel);
        constexpr int kArrowW = 22;
        auto leftArrow  = juce::Rectangle<int>(stepper.getX(), stepper.getY(),
                                               kArrowW, stepper.getHeight());
        auto rightArrow = juce::Rectangle<int>(stepper.getRight() - kArrowW,
                                               stepper.getY(), kArrowW, stepper.getHeight());

        int hover = -1;
        if (leftArrow.contains(e.getPosition()))
            hover = 0;
        else if (rightArrow.contains(e.getPosition()))
            hover = 1;

        if (hover != waveModeStepperHover_)
        {
            waveModeStepperHover_ = hover;
            repaint(panel);
            glView->repaint();
        }
    }

    if (focused_ && glView != nullptr)
    {
        auto content = getContentBounds();
        auto overlay = getOverlayBounds(content);
        auto hit = hitTestOverlayButton(e.getPosition(), overlay);
        if (hit == OverlayButton::kNone)
            hit = hitTestLibraryButton(e.getPosition(), content);
        if (hit != hoveredOverlayBtn_)
        {
            hoveredOverlayBtn_ = hit;
            repaint(overlay);
            repaint(content);
            glView->repaint();
        }

        if (hit != OverlayButton::kNone)
        {
            touchOverlayIdleTimer();
            if (hit == OverlayButton::kPresetName)
                setMouseCursor(juce::MouseCursor::IBeamCursor);
            else
                setMouseCursor(juce::MouseCursor::PointingHandCursor);
        }
        else if (isAutoRowExpanded_ &&
                 (!cachedAutoModeBtnRect_.isEmpty() &&
                  cachedAutoModeBtnRect_.contains(e.getPosition())))
        {
            // auto 模式切换按钮 hover（sec ↔ beat）
            touchOverlayIdleTimer();
            setMouseCursor(juce::MouseCursor::PointingHandCursor);
        }
        else if (isAutoRowExpanded_ && !autoModeIsBeat_ &&
                 cachedAutoTimeLabel_.contains(e.getPosition()))
        {
            // 时间标签 hover（仅时间模式可点击弹框）
            touchOverlayIdleTimer();
            setMouseCursor(juce::MouseCursor::PointingHandCursor);
        }
        else if (overlay.contains(e.getPosition()))
        {
            touchOverlayIdleTimer();
            setMouseCursor(juce::MouseCursor::NormalCursor);
        }
        else
            ModulePanel::mouseMove(e); // 基类处理边缘光标
    }
    else
    {
        ModulePanel::mouseMove(e);
    }
}

// ==========================================================
// 悬停提示（HoverHelp）
//   命中顺序与 mouseDown 一致：顶部控制栏 / 收藏按钮 → auto 行 → 各展开面板，
//   全部复用现有的几何函数（与绘制 / 点击同源，不会漂移）。
// ==========================================================
namespace
{
    juce::String milkdropEffectHelp (MilkdropEffectId id)
    {
        switch (id)
        {
            case MilkdropEffectId::kInvert:       return "Invert all colours.";
            case MilkdropEffectId::kShadows:      return "Add a mirrored grey glow on top (brightens, never darkens).";
            case MilkdropEffectId::kSolarize:     return "Solarize: mid tones flare up, darks and highlights fold back.";
            case MilkdropEffectId::kSplit:        return "Mirror the picture left / right around the centre.";
            case MilkdropEffectId::kZoom:         return "Zoom 2x into the centre.";
            case MilkdropEffectId::kMulti:        return "Tile the picture into repeated copies.";
            case MilkdropEffectId::kRainbow:      return "Wash the picture with cycling rainbow colours.";
            case MilkdropEffectId::kBlow:         return "Add a soft blurred glow on top.";
            case MilkdropEffectId::kBurn:         return "Burn-style overlay blend for extra contrast.";
            case MilkdropEffectId::kKaleidoscope: return "Fold the picture into mirrored kaleidoscope wedges.";
            case MilkdropEffectId::kSwirl:        return "Twist the picture around the centre.";
            case MilkdropEffectId::kPinch:        return "Pinch / bulge the centre like a lens.";
            case MilkdropEffectId::kPixelate:     return "Chunky pixel blocks.";
            case MilkdropEffectId::kGlitch:       return "Slightly offset RGB channels - digital glitch.";
            case MilkdropEffectId::kPosterize:    return "Reduce to a few flat colour levels.";
            case MilkdropEffectId::kSepia:        return "Old photo brown tint.";
            case MilkdropEffectId::kGrayscale:    return "Black and white.";
            case MilkdropEffectId::kEdge:         return "Edge detection - outlines only.";
            case MilkdropEffectId::kVignette:     return "Darken the corners.";
            case MilkdropEffectId::kTunnel:       return "Fly through an endless tunnel.";
            case MilkdropEffectId::kRipple:       return "Concentric water ripples.";
            case MilkdropEffectId::kMelt:         return "The picture droops and melts downwards.";
            case MilkdropEffectId::kFisheye:      return "Extreme wide-angle fisheye lens.";
            case MilkdropEffectId::kNoiseWarp:    return "High-frequency noise jitter.";
            case MilkdropEffectId::kMirrorMaze:   return "Infinite nested mirrors.";
            case MilkdropEffectId::kFragment:     return "Shuffle small blocks - shattered glass.";
            case MilkdropEffectId::kSpiral:       return "Spiral twist (angle + radius).";
            case MilkdropEffectId::kTwist:        return "Twist along the vertical axis.";
            case MilkdropEffectId::kColorShift:   return "Explosive RGB split with boosted colour.";
            case MilkdropEffectId::kNeon:         return "Neon glow around bright areas.";
            case MilkdropEffectId::kThermal:      return "Thermal camera colour palette.";
            case MilkdropEffectId::kAcid:         return "High-contrast green / magenta acid tones.";
            case MilkdropEffectId::kVhs:          return "VHS tape: stripes, colour bleed and noise.";
            case MilkdropEffectId::kCrt:          return "CRT scanlines and phosphor glow.";
            case MilkdropEffectId::kDuotone:      return "Map brightness onto two colours.";
            case MilkdropEffectId::kBloom:        return "Bright areas bloom and spread.";
            case MilkdropEffectId::kBinary:       return "Hard black / white threshold.";
            case MilkdropEffectId::kPrismatic:    return "Radial prism colour fringing.";
            default:                              break;
        }
        return {};
    }

    juce::String milkdropTweakHelp (int row)
    {
        switch (row)
        {
            case 0: return "Zoom in or out.";
            case 1: return "Rotate the picture.";
            case 2: return "Wobbly warp distortion.";
            case 3: return "Shift horizontally.";
            case 4: return "Shift vertically.";
            case 5: return "Stretch horizontally.";
            case 6: return "Stretch vertically.";
            default: return {};
        }
    }

    juce::String milkdropTweakIntHelp (int row)
    {
        switch (row)
        {
            case 0: return "Kaleidoscope petals: N-fold mirror symmetry around the centre (1 = off).";
            case 1: return "Cut the picture into N strips horizontally and mirror them (1 = off).";
            case 2: return "Cut the picture into N strips vertically and mirror them (1 = off).";
            default: return {};
        }
    }
}

ModulePanel::HoverHelp MilkdropModule::getHoverHelp (juce::Point<int> pos) const
{
    using y2k::help::make;

    HoverHelp h;
    auto tip = [&h] (const juce::String& text) { h.text = text; return h; };

    if (glView == nullptr)
        return ModulePanel::getHoverHelp (pos);

    const auto content = getContentBounds();
    const auto topBar  = content.withHeight (26);

    // ---- 顶部控制栏 + 右下角收藏 / 切换库 ----
    //   ★ 不按 focused_ 早退：macOS 的控制栏由顶层 overlay 窗口绘制，聚焦状态
    //     与绘制之间只要有一帧错位，按 focused_ 早退就会把这些按钮位置的悬停
    //     错落到基类的"模块说明"上（用户看到的正是"悬停画面/颜色却显示某个
    //     模块的介绍"）。命中具体按钮就返回按钮说明；控制栏条带内未命中按钮
    //     则不出提示（见本函数末尾），绝不把模块说明冒充成按钮说明。
    {
        auto btn = hitTestOverlayButton (pos, getOverlayBounds (content));
        if (btn == OverlayButton::kNone)
            btn = hitTestLibraryButton (pos, content);

        switch (btn)
        {
            case OverlayButton::kPrev:
                return tip (make ("Previous preset", "Go back one preset. Shortcut: Left arrow."));
            case OverlayButton::kNext:
                return tip (make ("Next preset", "Go to the next preset. Shortcut: Right arrow."));
            case OverlayButton::kRandom:
                return tip (make ("Random preset", "Jump to a random preset (no repeats until all are seen). "
                                                   "Shortcut: Space."));
            case OverlayButton::kPresetName:
                return tip (make ("Preset name", "Click to jump straight to a preset number."));
            case OverlayButton::kAuto:
                return tip (make (isAutoMode_ ? "Auto: ON" : "Auto: OFF",
                                  "Switch presets automatically - every N seconds, or every N beats "
                                  "in a DAW. Click to cycle: on -> show settings -> off."));
            case OverlayButton::kColor:
                return tip (make ("Color", "Open the colour panel: tint the red / green / blue channels "
                                           "and set the overall brightness."));
            case OverlayButton::kEffects:
                return tip (make ("Effects", "Open the effects panel: stack post-processing effects "
                                             "(kaleidoscope, VHS, CRT, neon, glitch...) on any preset."));
            case OverlayButton::kWave:
                return tip (make ("Wave", "Open the wave panel: override the preset's waveform shape, "
                                          "position, colour and drawing style."));
            case OverlayButton::kTweak:
                return tip (make ("Tweak", "Open the tweak panel: zoom, rotate, warp, shift, stretch and "
                                           "mirror-fold the final picture in real time."));
            case OverlayButton::kResetAll:
                return tip (make ("Reset all", "Reset all color, effects, wave and tweak adjustments back to defaults."));
            case OverlayButton::kKeyLock:
                return tip (keyboardControlEnabled_
                    ? make ("Keyboard control: ON",
                            "Left / Right / Space switch MilkDrop presets. Click to disable keyboard shortcuts.")
                    : make ("Keyboard control: OFF",
                            "Left / Right / Space preset shortcuts are disabled. Click to re-enable."));
            case OverlayButton::kLike:
            {
                bool liked = false;
                const auto path = glView->GetCurrentPresetFilePath();
                if (path.isNotEmpty())
                    liked = FindMilkdropLikeDirForModule()
                                .getChildFile (juce::File (path).getFileName()).existsAsFile();
                return tip (liked ? make ("Liked", "This preset is in your Likes. Click to remove it.")
                                  : make ("Like", "Save this preset to your Likes so you can browse "
                                                  "your favourites only."));
            }
            case OverlayButton::kLibraryToggle:
            {
                const bool inLikeLib = (editor_ != nullptr) && editor_->IsMilkdropUseLikeLibrary();
                return tip (inLikeLib ? make ("Browsing: Likes", "Click to go back to all built-in presets.")
                                      : make ("Browsing: All presets", "Click to browse only your liked presets."));
            }
            default:
                break;
        }
    }

    // 控制栏条带内但没命中具体按钮（或控制栏当前未显示）：不出提示，
    //   避免把基类的"模块说明"当成按钮说明显示出来。
    if (getOverlayBounds (content).contains (pos))
        return {};

    // ---- auto 展开行 ----
    if (isAutoRowExpanded_)
    {
        if (! cachedAutoModeBtnRect_.isEmpty() && cachedAutoModeBtnRect_.contains (pos))
            return tip (make ("Auto timing", autoModeIsBeat_
                ? juce::String ("Beat mode: switch every N beats, locked to the DAW tempo. Click for seconds.")
                : juce::String ("Time mode: switch every N seconds. Click for beats (DAW tempo).")));

        const auto autoRow = getAutoRowBounds (topBar);
        if (getSliderBounds (autoRow).expanded (4).contains (pos))
            return tip (make (autoModeIsBeat_ ? "Beats per switch" : "Seconds per switch",
                              autoModeIsBeat_ ? "Drag to set how many beats each preset stays (1 - 64)."
                                              : "Drag to set how long each preset stays (1 - 60 s)."));

        if (! autoModeIsBeat_ && cachedAutoTimeLabel_.contains (pos))
            return tip (make ("Interval", "Click to type an exact number of seconds."));
    }

    // ---- color 面板 ----
    if (isColorPanelOpen_)
    {
        const auto panel = getColorPanelBounds (topBar);
        if (getTintResetBounds (panel).contains (pos))
            return tip (make ("Reset color", "Back to neutral colours and normal brightness."));

        static const char* const kTintHelp[][2] = {
            { "Red tint",   "Scale the red channel of the whole picture (1.00 = unchanged)." },
            { "Green tint", "Scale the green channel of the whole picture (1.00 = unchanged)." },
            { "Blue tint",  "Scale the blue channel of the whole picture (1.00 = unchanged)." },
            { "Brightness", "Centre = normal, left = darker, right = glowing (up to 8x)." },
        };
        for (int row = 0; row < 4; ++row)
            if (getTintSliderBounds (panel, row).expanded (6).contains (pos))
                return tip (make (kTintHelp[row][0], kTintHelp[row][1]));
    }

    // ---- effects 面板 ----
    if (isEffectsPanelOpen_)
    {
        const auto panel = getEffectsPanelBounds (topBar);
        if (getEffectsResetBounds (panel).contains (pos))
            return tip (make ("Reset effects", "Turn every effect off."));

        const int count = CountImplementedMilkdropEffects();
        for (int row = 0; row < count; ++row)
        {
            if (! getEffectsToggleBounds (panel, row).expanded (4).contains (pos))
                continue;

            if (const auto* def = GetImplementedMilkdropEffect (row))
                return tip (make (y2k::trf ("Effect: {0}", y2k::tr (def->display_name)) + " "
                                      + y2k::tr (def->get (visualState_) ? "(on)" : "(off)"),
                                  milkdropEffectHelp (def->id) + "\nEffects stack - try mixing a few."));
        }
    }

    // ---- wave 面板 ----
    if (isWavePanelOpen_)
    {
        const auto panel = getWavePanelBounds (topBar);
        if (getWaveResetBounds (panel).contains (pos))
            return tip (make ("Reset wave", "Drop all wave overrides and use the preset's own waveform."));

        if (getWaveModeStepperBounds (panel).contains (pos))
            return tip (make (y2k::trf ("Wave shape: {0}", y2k::tr (GetWaveModeName (waveState_.mode))),
                              "Click the left / right half to step through the 16 waveform shapes."));

        static const char* const kSwitchHelp[][2] = {
            { "Dots",   "Draw the waveform as dots instead of a line." },
            { "Thick",  "Draw a thicker waveform." },
            { "Add",    "Additive blending - overlapping parts glow brighter." },
            { "Bright", "Boost the waveform brightness." },
        };
        for (int idx = 0; idx < 4; ++idx)
            if (getWaveSwitchBounds (panel, idx).expanded (4).contains (pos))
                return tip (make (kSwitchHelp[idx][0], kSwitchHelp[idx][1]));

        static const char* const kSliderHelp[][2] = {
            { "Position X", "Move the waveform left / right." },
            { "Position Y", "Move the waveform up / down." },
            { "Red",        "Waveform red amount." },
            { "Green",      "Waveform green amount." },
            { "Blue",       "Waveform blue amount." },
            { "Alpha",      "Waveform opacity." },
            { "Mystery",    "Shape-specific extra parameter - its effect depends on the wave shape." },
        };
        for (int row = 0; row < 7; ++row)
            if (getWaveSliderBounds (panel, row).expanded (6).contains (pos))
                return tip (make (kSliderHelp[row][0], kSliderHelp[row][1]));
    }

    // ---- tweak 面板 ----
    if (isTweakPanelOpen_)
    {
        const auto panel = getTweakPanelBounds (topBar);
        if (getTweakResetBounds (panel).contains (pos))
            return tip (make ("Reset tweak", "Remove all zoom / rotate / warp / shift / stretch / fold changes."));

        for (int row = 0; row < MilkdropVisualOffsetState::kIntParamCount; ++row)
            if (getTweakIntSliderBounds (panel, row).expanded (6).contains (pos))
                return tip (make (GetVisualOffsetIntParams()[row].display_name, milkdropTweakIntHelp (row)));

        for (int row = 0; row < MilkdropVisualOffsetState::kParamCount; ++row)
            if (getTweakSliderBounds (panel, row).expanded (6).contains (pos))
                return tip (make (GetVisualOffsetParams()[row].display_name,
                                  milkdropTweakHelp (row) + "\nCentre = no change. Applies live."));
    }

    return ModulePanel::getHoverHelp (pos);
}

void MilkdropModule::mouseDrag(const juce::MouseEvent& e)
{
    // slider 拖动中（mouseDrag 是 JUCE 专为拖拽设计的回调，比 mouseMove 更可靠
    // 地接收按下鼠标后的移动事件，尤其当组件树中存在原生 HWND 子窗口时）
    if (isDraggingSlider_)
    {
        // 与 color / wave / tweak 各分支保持一致：拖动中刷新 overlay 活跃时间，
        // 否则拖满 4 s 会被 checkOverlayAutoHide 判成 idle 而把控制栏藏掉。
        touchOverlayIdleTimer();
        auto content = getContentBounds();
        auto topBar = content.withHeight(26);
        auto autoRow = getAutoRowBounds(topBar);
        auto sliderBounds = getSliderBounds(autoRow);
        float proportion = static_cast<float>(e.getPosition().x - sliderBounds.getX())
                           / static_cast<float>(sliderBounds.getWidth());
        if (autoModeIsBeat_)
            updateAutoBeatsFromSlider(proportion);
        else
            updateAutoIntervalFromSlider(proportion);
        repaint(autoRow);
        glView->repaint();
        return;
    }

    // color/bright slider 拖动中
    if (draggingTintRow_ >= 0)
    {
        touchOverlayIdleTimer();
        auto content = getContentBounds();
        auto topBar = content.withHeight(26);
        auto panel = getColorPanelBounds(topBar);
        auto sb = getTintSliderBounds(panel, draggingTintRow_);
        float proportion = static_cast<float>(e.getPosition().x - sb.getX())
                           / static_cast<float>(sb.getWidth());
        updateTintFromSlider(draggingTintRow_, proportion);
        repaint(panel);
        glView->repaint();
        return;
    }

    // wave slider 拖动中
    if (draggingWaveRow_ >= 0)
    {
        touchOverlayIdleTimer();
        auto content = getContentBounds();
        auto topBar = content.withHeight(26);
        auto panel = getWavePanelBounds(topBar);
        auto sb = getWaveSliderBounds(panel, draggingWaveRow_);
        float proportion = static_cast<float>(e.getPosition().x - sb.getX())
                           / static_cast<float>(sb.getWidth());
        updateWaveFromSlider(draggingWaveRow_, proportion);
        repaint(panel);
        glView->repaint();
        return;
    }

    // tweak slider 拖动中
    if (draggingTweakRow_ >= 0)
    {
        touchOverlayIdleTimer();
        auto content = getContentBounds();
        auto topBar = content.withHeight(26);
        auto panel = getTweakPanelBounds(topBar);
        auto sb = getTweakSliderBounds(panel, draggingTweakRow_);
        float proportion = static_cast<float>(e.getPosition().x - sb.getX())
                           / static_cast<float>(sb.getWidth());
        updateTweakFromSlider(draggingTweakRow_, proportion);
        repaint(panel);
        glView->repaint();
        return;
    }

    // tweak 整数滑块拖动中
    if (draggingTweakIntRow_ >= 0)
    {
        touchOverlayIdleTimer();
        auto content = getContentBounds();
        auto topBar = content.withHeight(26);
        auto panel = getTweakPanelBounds(topBar);
        auto sb = getTweakIntSliderBounds(panel, draggingTweakIntRow_);
        float proportion = static_cast<float>(e.getPosition().x - sb.getX())
                           / static_cast<float>(sb.getWidth());
        updateTweakIntFromSlider(draggingTweakIntRow_, proportion);
        repaint(panel);
        glView->repaint();
        return;
    }

    ModulePanel::mouseDrag(e);
}

void MilkdropModule::mouseExit(const juce::MouseEvent& e)
{
    if (hoveredOverlayBtn_ != OverlayButton::kNone)
    {
        hoveredOverlayBtn_ = OverlayButton::kNone;
        repaint();
        glView->repaint();
    }
    ModulePanel::mouseExit(e);
}

// ---- 叠加层布局辅助 ----

namespace
{
    bool isWaveStateDefault (const MilkdropWaveState& wv) noexcept
    {
        return ! wv.enabled
            && wv.mode == 6
            && std::fabs (wv.x - 0.5f) <= 1e-4f
            && std::fabs (wv.y - 0.5f) <= 1e-4f
            && std::fabs (wv.r - 1.0f) <= 1e-4f
            && std::fabs (wv.g - 1.0f) <= 1e-4f
            && std::fabs (wv.b - 1.0f) <= 1e-4f
            && std::fabs (wv.a - 1.0f) <= 1e-4f
            && std::fabs (wv.mystery)  <= 1e-4f
            && ! wv.usedots
            && ! wv.thick
            && ! wv.additive
            && ! wv.brighten;
    }
}

// 控制栏文字按钮统一宽度（auto / color / effects / wave / tweak / reset / keys）：
//   按当前语言的按钮文本测量（中文模式下"颜色/效果/波形/画面/重置/自动/键盘"两字等宽，
//   按钮不再长短不一），与绘制处 getFont(8.0f, bold) 同字体同字号。
int MilkdropModule::overlayTextBtnWidth()
{
    static const char* const kLabels[] = { "auto", "color", "effects", "wave", "tweak", "reset", "keys" };
    const auto font = PinkXP::getFont (8.0f, juce::Font::bold);

    int w = 32;   // 最小宽度（图标按钮 22 + 余量）
    for (const auto* t : kLabels)
        w = juce::jmax (w, juce::GlyphArrangement::getStringWidthInt (font, y2k::tr (t)) + 8);
    return w;
}

juce::Rectangle<int> MilkdropModule::getOverlayBounds(juce::Rectangle<int> content) const
{
    constexpr int kBarHeight = 26;
    return content.withHeight(juce::jmin(kBarHeight, content.getHeight()));
}

MilkdropModule::OverlayBarLayout MilkdropModule::computeOverlayBarLayout(
    juce::Rectangle<int> overlay) const
{
    constexpr int kBtnSize           = 22;
    constexpr int kNumTextBtns       = 7;   // keys / reset / tweak / wave / effects / color / auto
    constexpr int kPreferredMinNameW = 32;  // 窄宽度（如默认 400px）下优先给预设名留出空间，且不与 < 重叠

    const int idealTextBtnW = overlayTextBtnWidth();
    int edgePad  = 4;
    int gap      = 3;
    int textBtnW = idealTextBtnW;

    const int idealNonNameW = 2 * edgePad + 3 * kBtnSize + 4 + (kNumTextBtns + 1) * gap
                            + kNumTextBtns * idealTextBtnW;
    if (overlay.getWidth() - idealNonNameW < kPreferredMinNameW)
    {
        edgePad = 3;
        gap     = 2;
        const int fixedWithName = 2 * edgePad + 3 * kBtnSize + 4 + (kNumTextBtns + 1) * gap;
        const int availWithName = overlay.getWidth() - fixedWithName - kPreferredMinNameW;
        const int availNoName   = overlay.getWidth() - (2 * edgePad + 3 * kBtnSize + 2 + (kNumTextBtns + 1) * gap);

        if (availWithName >= kNumTextBtns * 28)
            textBtnW = juce::jmin (idealTextBtnW, availWithName / kNumTextBtns);
        else
            textBtnW = juce::jlimit (0, idealTextBtnW, availNoName / kNumTextBtns);
    }

    const int btnY = overlay.getY() + 2;

    OverlayBarLayout l;
    l.prevBtn     = { overlay.getX() + edgePad, btnY, kBtnSize, kBtnSize };
    l.randomBtn   = { overlay.getRight() - edgePad - kBtnSize, btnY, kBtnSize, kBtnSize };
    l.nextBtn     = { l.randomBtn.getX() - gap - kBtnSize, btnY, kBtnSize, kBtnSize };
    l.autoBtn     = { l.nextBtn.getX() - gap - textBtnW, btnY, textBtnW, kBtnSize };
    l.colorBtn    = { l.autoBtn.getX() - gap - textBtnW, btnY, textBtnW, kBtnSize };
    l.effectsBtn  = { l.colorBtn.getX() - gap - textBtnW, btnY, textBtnW, kBtnSize };
    l.waveBtn     = { l.effectsBtn.getX() - gap - textBtnW, btnY, textBtnW, kBtnSize };
    l.tweakBtn    = { l.waveBtn.getX() - gap - textBtnW, btnY, textBtnW, kBtnSize };
    l.resetAllBtn = { l.tweakBtn.getX() - gap - textBtnW, btnY, textBtnW, kBtnSize };
    l.keyLockBtn  = { l.resetAllBtn.getX() - gap - textBtnW, btnY, textBtnW, kBtnSize };

    const int nameW = juce::jmax(0, l.keyLockBtn.getX() - l.prevBtn.getRight() - 4);
    l.nameArea = { l.prevBtn.getRight() + 2, overlay.getY(), nameW, overlay.getHeight() };
    return l;
}

MilkdropModule::OverlayButton MilkdropModule::hitTestOverlayButton(
    juce::Point<int> pos, juce::Rectangle<int> overlay) const
{
    if (! overlay.contains(pos))
        return OverlayButton::kNone;

    const auto l = computeOverlayBarLayout(overlay);

    if (l.prevBtn.contains(pos))     return OverlayButton::kPrev;
    if (l.keyLockBtn.contains(pos))  return OverlayButton::kKeyLock;
    if (l.resetAllBtn.contains(pos)) return OverlayButton::kResetAll;
    if (l.tweakBtn.contains(pos))    return OverlayButton::kTweak;
    if (l.waveBtn.contains(pos))     return OverlayButton::kWave;
    if (l.effectsBtn.contains(pos))  return OverlayButton::kEffects;
    if (l.colorBtn.contains(pos))    return OverlayButton::kColor;
    if (l.autoBtn.contains(pos))     return OverlayButton::kAuto;
    if (l.nextBtn.contains(pos))     return OverlayButton::kNext;
    if (l.randomBtn.contains(pos))   return OverlayButton::kRandom;

    // name area：覆盖 < 和最左侧功能按钮之间的空余区域
    if (! cachedNameArea_.isEmpty() && cachedNameArea_.contains(pos))
        return OverlayButton::kPresetName;

    return OverlayButton::kNone;
}

juce::Rectangle<int> MilkdropModule::getOverlayButtonRect(
    juce::Rectangle<int> overlay, OverlayButton btn) const
{
    const auto l = computeOverlayBarLayout(overlay);

    switch (btn)
    {
    case OverlayButton::kPrev:       return l.prevBtn;
    case OverlayButton::kRandom:     return l.randomBtn;
    case OverlayButton::kNext:       return l.nextBtn;
    case OverlayButton::kAuto:       return l.autoBtn;
    case OverlayButton::kColor:      return l.colorBtn;
    case OverlayButton::kEffects:    return l.effectsBtn;
    case OverlayButton::kWave:       return l.waveBtn;
    case OverlayButton::kTweak:      return l.tweakBtn;
    case OverlayButton::kResetAll:   return l.resetAllBtn;
    case OverlayButton::kKeyLock:    return l.keyLockBtn;
    case OverlayButton::kPresetName: return l.nameArea;
    default:
        return {};
    }
}

void MilkdropModule::executeOverlayAction(OverlayButton btn)
{
    switch (btn)
    {
    case OverlayButton::kPrev:       prevPreset();                    break;
    case OverlayButton::kNext:       nextPreset();                    break;
    case OverlayButton::kRandom:     randomPreset();                  break;
    case OverlayButton::kPresetName: showPresetJumpDialog();          break;
    case OverlayButton::kAuto:       toggleAutoMode();                break;
    case OverlayButton::kColor:      toggleColorPanel();              break;
    case OverlayButton::kEffects:    toggleEffectsPanel();            break;
    case OverlayButton::kWave:       toggleWavePanel();               break;
    case OverlayButton::kTweak:      toggleTweakPanel();              break;
    case OverlayButton::kResetAll:   resetAllUserEffects();           break;
    case OverlayButton::kKeyLock:    toggleKeyboardControlEnabled();  break;
    // 分辨率切换按钮已全平台阉割
#if 0
    case OverlayButton::kRenderScale: glView->RequestRenderScale(); break;
#endif
    default: break;
    }
}

void MilkdropModule::setKeyboardControlEnabled(bool enabled)
{
    if (keyboardControlEnabled_ == enabled)
        return;

    keyboardControlEnabled_ = enabled;
    repaint();
    if (glView != nullptr)
        glView->repaint();
#if JUCE_MAC
    if (overlayView_ != nullptr && overlayView_->isOnDesktop())
        overlayView_->repaint();
#endif
}

void MilkdropModule::toggleKeyboardControlEnabled()
{
    notifyBeforeUserEdit();
    setKeyboardControlEnabled(! keyboardControlEnabled_);
    notifyAfterUserEdit (y2k::tr ("MilkDrop keys"), "keys");
}

bool MilkdropModule::hasAnyUserVisualOverrides() const
{
    const MilkdropVisualState vis = (editor_ != nullptr) ? editor_->GetMilkdropVisualState() : visualState_;
    if (! vis.isNeutral())
        return true;

    const MilkdropWaveState wv = (editor_ != nullptr) ? editor_->GetMilkdropWaveState() : waveState_;
    return ! isWaveStateDefault (wv);
}

void MilkdropModule::resetAllUserEffects()
{
    syncVisualFromEditor();
    syncWaveFromEditor();

    if (visualState_.isNeutral() && isWaveStateDefault (waveState_))
        return;

    notifyBeforeUserEdit();
    if (focused_)
        touchOverlayIdleTimer();

    visualState_.tint_r     = 1.0f;
    visualState_.tint_g     = 1.0f;
    visualState_.tint_b     = 1.0f;
    visualState_.brightness = 1.0f;
    for (const auto& def : GetMilkdropEffectDefs())
        if (def.implemented)
            def.set (visualState_, false);
    visualState_.offset.reset();

    const bool hadWave = waveState_.enabled;
    waveState_.reset();

    applyVisualToEditor();
    applyWaveToEditor();
    if (hadWave)
        reloadCurrentPresetForWave();

    notifyAfterUserEdit (y2k::tr ("MilkDrop reset all"), "reset_all");

    repaint();
    if (glView != nullptr)
        glView->repaint();
#if JUCE_MAC
    if (overlayView_ != nullptr && overlayView_->isOnDesktop())
        overlayView_->repaint();
#endif
}

// ---- 叠加层绘制（风格化工艺原语：跟随 SkinTokens / Craft）----

namespace
{
    void drawMilkdropOverlaySurface (juce::Graphics& g, juce::Rectangle<int> r, bool isTopBar)
    {
        if (r.isEmpty())
            return;

        const auto& t = PinkXP::getSkinTokens();
        switch (t.frameStyle)
        {
            case PinkXP::FrameStyle::pixelBevel:
            {
                // Pixel XP：0px 锐利暗色控制台 + Win95 1px 内高光 / 底部清晰分割线
                g.setColour (juce::Colour (0x08, 0x07, 0x0d).withAlpha (isTopBar ? 0.85f : 0.81f));
                g.fillRect (r);
                g.setColour (PinkXP::hl.withAlpha (isTopBar ? 0.28f : 0.18f));
                g.fillRect (r.getX(), r.getY(), r.getWidth(), 1);
                g.fillRect (r.getX(), r.getY(), 1, r.getHeight());
                g.setColour (PinkXP::dark.withAlpha (0.90f));
                g.fillRect (r.getRight() - 1, r.getY(), 1, r.getHeight());
                g.setColour (PinkXP::pink300.withAlpha (isTopBar ? 0.78f : 0.56f));
                g.fillRect (r.getX(), r.getBottom(), r.getWidth(), 1);
                return;
            }

            case PinkXP::FrameStyle::machinedChrome:
            {
                // Millennium Chrome：阳极枪灰 / 拉丝钢 4-stop 控制台面板（38% 折痕高光 + 顶沿镜面 + 底部机加工接缝）
                const float a = isTopBar ? 0.89f : 0.85f;
                juce::ColourGradient grad (juce::Colour (0x36, 0x3a, 0x44).withAlpha (a), 0.0f, (float) r.getY(),
                                           juce::Colour (0x0c, 0x0e, 0x13).withAlpha (a), 0.0f, (float) r.getBottom(), false);
                grad.addColour (0.38, juce::Colour (0x22, 0x25, 0x2e).withAlpha (a));
                grad.addColour (0.42, juce::Colour (0x15, 0x17, 0x1e).withAlpha (a));
                g.setGradientFill (grad);
                g.fillRect (r);
                // 顶部 1px 锐利镜面铬沿 + 左右拉丝钢边缝
                g.setColour (juce::Colours::white.withAlpha (isTopBar ? 0.34f : 0.24f));
                g.fillRect (r.getX(), r.getY(), r.getWidth(), 1);
                g.setColour (juce::Colours::white.withAlpha (0.12f));
                g.fillRect (r.getX(), r.getY(), 1, r.getHeight());
                g.setColour (juce::Colours::black.withAlpha (0.58f));
                g.fillRect (r.getRight() - 1, r.getY(), 1, r.getHeight());
                // 底部 2px 机加工金属接缝（1px 暗槽 + 1px 主题金属亮沿）
                g.setColour (juce::Colours::black.withAlpha (0.78f));
                g.fillRect (r.getX(), r.getBottom() - 1, r.getWidth(), 1);
                g.setColour (PinkXP::sel.interpolatedWith (juce::Colours::white, 0.32f)
                                        .withAlpha (isTopBar ? 0.78f : 0.56f));
                g.fillRect (r.getX(), r.getBottom(), r.getWidth(), 1);
                return;
            }

            case PinkXP::FrameStyle::frostedGlass:
            {
                // Frosted Glass：暗烟熏水晶玻璃控制台（顶部 42% 凸面玻璃穹顶高光带 + 1px 亮冰白顶沿 + 折射底边）
                const float a = isTopBar ? 0.84f : 0.80f;
                juce::ColourGradient grad (juce::Colour (0x24, 0x2a, 0x38).withAlpha (a), 0.0f, (float) r.getY(),
                                           juce::Colour (0x0b, 0x0e, 0x16).withAlpha (a + 0.04f), 0.0f, (float) r.getBottom(), false);
                g.setGradientFill (grad);
                g.fillRect (r);
                auto sheen = r.withHeight (juce::jmax (3, r.getHeight() * 42 / 100));
                juce::ColourGradient dome (juce::Colours::white.withAlpha (0.14f), 0.0f, (float) sheen.getY(),
                                           juce::Colours::white.withAlpha (0.02f), 0.0f, (float) sheen.getBottom(), false);
                g.setGradientFill (dome);
                g.fillRect (sheen);
                g.setColour (juce::Colours::white.withAlpha (isTopBar ? 0.36f : 0.26f));
                g.fillRect (r.getX(), r.getY(), r.getWidth(), 1);
                g.setColour (juce::Colours::white.withAlpha (0.12f));
                g.fillRect (r.getX(), r.getY(), 1, r.getHeight());
                g.fillRect (r.getRight() - 1, r.getY(), 1, r.getHeight());
                g.setColour (PinkXP::sel.interpolatedWith (juce::Colours::white, 0.32f)
                                        .withAlpha (isTopBar ? 0.72f : 0.52f));
                g.fillRect (r.getX(), r.getBottom(), r.getWidth(), 1);
                return;
            }

            case PinkXP::FrameStyle::neonTube:
            {
                // Neon Cyber：深黑曜石赛博控制台 + 顶部微激光沿 + 底部双通道发光霓虹底轨
                g.setColour (juce::Colour (0x07, 0x05, 0x0e).withAlpha (isTopBar ? 0.90f : 0.86f));
                g.fillRect (r);
                g.setColour (PinkXP::sel.withAlpha (0.28f));
                g.fillRect (r.getX(), r.getY(), r.getWidth(), 1);
                g.setColour (PinkXP::sel.withAlpha (0.30f));
                g.fillRect (r.getX(), r.getY(), 1, r.getHeight());
                g.fillRect (r.getRight() - 1, r.getY(), 1, r.getHeight());
                // 双通道发光霓虹底轨（宽柔光晕 + 灼热白芯）
                g.setColour (PinkXP::sel.withAlpha (isTopBar ? 0.30f : 0.22f));
                g.fillRect (r.getX(), r.getBottom() - 2, r.getWidth(), 3);
                g.setColour (PinkXP::sel.interpolatedWith (juce::Colours::white, 0.42f)
                                        .withAlpha (isTopBar ? 0.95f : 0.74f));
                g.fillRect (r.getX(), r.getBottom(), r.getWidth(), 1);
                return;
            }

            case PinkXP::FrameStyle::crtPhosphor:
            {
                // CRT Terminal：0px 深色荧光终端控制台 + 2px 水平扫描线 + 锐利 1px 荧光边框
                g.setColour (juce::Colour (0x04, 0x09, 0x06).withAlpha (isTopBar ? 0.90f : 0.86f));
                g.fillRect (r);
                g.setColour (juce::Colours::black.withAlpha (0.20f));
                for (int py = r.getY() + 1; py < r.getBottom(); py += 2)
                    g.fillRect (r.getX(), py, r.getWidth(), 1);
                g.setColour (PinkXP::sel.withAlpha (isTopBar ? 0.82f : 0.64f));
                g.drawRect (r, 1);
                g.fillRect (r.getX(), r.getBottom(), r.getWidth(), 1);
                return;
            }

            case PinkXP::FrameStyle::modernStudio:
            default:
            {
                // Modern Minimal：细腻哑光演播室暗色控制台 + 1px 精致细边框
                const float a = isTopBar ? 0.84f : 0.80f;
                juce::ColourGradient grad (juce::Colour (0x18, 0x1a, 0x20).withAlpha (a), 0.0f, (float) r.getY(),
                                           juce::Colour (0x0f, 0x11, 0x16).withAlpha (a), 0.0f, (float) r.getBottom(), false);
                g.setGradientFill (grad);
                g.fillRect (r);
                g.setColour (juce::Colours::white.withAlpha (0.09f));
                g.fillRect (r.getX(), r.getY(), r.getWidth(), 1);
                if (! isTopBar)
                {
                    // 子面板左右微细描边，增强弹出层级辨识
                    g.setColour (juce::Colours::white.withAlpha (0.08f));
                    g.fillRect (r.getX(), r.getY(), 1, r.getHeight());
                    g.fillRect (r.getRight() - 1, r.getY(), 1, r.getHeight());
                }
                g.setColour (PinkXP::sel.withAlpha (isTopBar ? 0.52f : 0.36f));
                g.fillRect (r.getX(), r.getBottom(), r.getWidth(), 1);
                return;
            }
        }
    }

    // 返回该按钮状态下推荐的高对比文字/图标颜色
    juce::Colour drawMilkdropOverlayButton (juce::Graphics& g,
                                            juce::Rectangle<int> r,
                                            bool active,
                                            bool hovered,
                                            bool pressed,
                                            bool muted = false,
                                            bool accentAlert = false,
                                            bool translucentActive = false)
    {
        if (r.isEmpty())
            return juce::Colour (0xEE, 0xEE, 0xEE);

        const auto& t = PinkXP::getSkinTokens();
        switch (t.frameStyle)
        {
            case PinkXP::FrameStyle::pixelBevel:
            {
                // Pixel XP：全态纯正 0px Win95 硬斜角按钮（绝无圆角，绝无平框混搭）
                if (pressed || (active && ! translucentActive))
                {
                    const auto fill = pressed ? PinkXP::pink200 : PinkXP::pink100;
                    PinkXP::drawPressed (g, r, fill);
                    if (active && r.getWidth() >= 8 && r.getHeight() >= 8)
                    {
                        g.setColour (PinkXP::sel);
                        g.fillRect (r.getX() + 2, r.getBottom() - 3, r.getWidth() - 4, 2);
                    }
                    return PinkXP::readableInkOn (fill);
                }
                if (active && translucentActive)
                {
                    g.setColour (PinkXP::pink300.withAlpha (0.34f));
                    g.fillRect (r);
                    g.setColour (PinkXP::dark.withAlpha (0.92f));
                    g.fillRect (r.getX(), r.getY(), r.getWidth(), 1);
                    g.fillRect (r.getX(), r.getY(), 1, r.getHeight());
                    g.setColour (PinkXP::hl.withAlpha (0.72f));
                    g.fillRect (r.getX() + 1, r.getBottom() - 1, r.getWidth() - 1, 1);
                    g.fillRect (r.getRight() - 1, r.getY() + 1, 1, r.getHeight() - 1);
                    return PinkXP::pink200;
                }
                if (hovered)
                {
                    PinkXP::drawRaised (g, r, PinkXP::pink200);
                    return PinkXP::readableInkOn (PinkXP::pink200);
                }

                // 常规暗底 Win95 凸起斜角按键
                const auto bg = accentAlert ? PinkXP::pink500.withAlpha (0.58f)
                              : muted       ? juce::Colour (0x24, 0x23, 0x2c).withAlpha (0.74f)
                                            : juce::Colour (0x35, 0x33, 0x40).withAlpha (0.86f);
                g.setColour (bg);
                g.fillRect (r);
                // 外层 1px 硬斜角 + 内层 1px 暗斜角
                g.setColour (PinkXP::hl.withAlpha (accentAlert ? 0.75f : (muted ? 0.28f : 0.62f)));
                g.fillRect (r.getX(), r.getY(), r.getWidth(), 1);
                g.fillRect (r.getX(), r.getY(), 1, r.getHeight());
                g.setColour (PinkXP::dark.withAlpha (0.95f));
                g.fillRect (r.getX(), r.getBottom() - 1, r.getWidth(), 1);
                g.fillRect (r.getRight() - 1, r.getY(), 1, r.getHeight());
                if (r.getWidth() >= 5 && r.getHeight() >= 5)
                {
                    g.setColour (PinkXP::shdw.withAlpha (muted ? 0.35f : 0.68f));
                    g.fillRect (r.getX() + 1, r.getBottom() - 2, r.getWidth() - 2, 1);
                    g.fillRect (r.getRight() - 2, r.getY() + 1, 1, r.getHeight() - 2);
                }
                return accentAlert ? PinkXP::pink50
                     : muted       ? juce::Colour (0xDD, 0xDD, 0xDD).withAlpha (0.58f)
                                   : juce::Colour (0xEE, 0xEE, 0xEE);
            }

            case PinkXP::FrameStyle::machinedChrome:
            {
                // Millennium Chrome：阳极枪灰 / 拉丝铝硬件按键（全态 0px 机加工斜角 + 4-stop 折痕高光）
                if (pressed || (active && ! translucentActive))
                {
                    const auto fill = pressed ? PinkXP::pink200 : PinkXP::btnFace;
                    PinkXP::Craft::drawControlChrome (g, r, fill,
                                                      pressed ? PinkXP::ControlState::pressed : PinkXP::ControlState::active,
                                                      0.0f);
                    return PinkXP::readableInkOn (fill);
                }

                const auto base = (active && translucentActive) ? PinkXP::sel.interpolatedWith (juce::Colour (0x2c, 0x30, 0x3a), 0.55f).withAlpha (0.86f)
                                : hovered     ? juce::Colour (0x4c, 0x52, 0x60).withAlpha (0.92f)
                                : accentAlert ? PinkXP::sel.interpolatedWith (juce::Colour (0x36, 0x22, 0x2a), 0.45f).withAlpha (0.86f)
                                : muted       ? juce::Colour (0x24, 0x26, 0x2e).withAlpha (0.76f)
                                              : juce::Colour (0x36, 0x3a, 0x45).withAlpha (0.88f);
                juce::ColourGradient grad (base.interpolatedWith (juce::Colours::white, hovered ? 0.34f : 0.26f),
                                           0.0f, (float) r.getY(),
                                           base.darker (0.22f),
                                           0.0f, (float) r.getBottom(), false);
                grad.addColour (0.38, base.interpolatedWith (juce::Colours::white, 0.08f));
                grad.addColour (0.42, base.darker (0.06f));
                g.setGradientFill (grad);
                g.fillRect (r);

                g.setColour (juce::Colours::black.withAlpha (0.92f));
                g.drawRect (r, 1);
                if (r.getWidth() >= 4 && r.getHeight() >= 4)
                {
                    g.setColour (juce::Colours::white.withAlpha (hovered ? 0.54f : (muted ? 0.16f : 0.36f)));
                    g.fillRect (r.getX() + 1, r.getY() + 1, r.getWidth() - 2, 1);
                    g.fillRect (r.getX() + 1, r.getY() + 1, 1, r.getHeight() - 2);
                    g.setColour (juce::Colours::black.withAlpha (0.42f));
                    g.fillRect (r.getX() + 1, r.getBottom() - 2, r.getWidth() - 2, 1);
                    g.fillRect (r.getRight() - 2, r.getY() + 1, 1, r.getHeight() - 2);
                }
                if ((active && translucentActive) && r.getWidth() >= 8 && r.getHeight() >= 8)
                {
                    // 锁定 / 自动开关硬件 LED 指示条
                    g.setColour (PinkXP::sel);
                    g.fillRect (r.getX() + 2, r.getBottom() - 3, r.getWidth() - 4, 2);
                }
                return (active && translucentActive) ? juce::Colours::white.withAlpha (0.96f)
                     : accentAlert ? juce::Colours::white.withAlpha (0.96f)
                     : hovered     ? juce::Colours::white.withAlpha (0.98f)
                     : muted       ? juce::Colour (0xDD, 0xDD, 0xDD).withAlpha (0.58f)
                                   : juce::Colour (0xF0, 0xF2, 0xF6);
            }

            case PinkXP::FrameStyle::frostedGlass:
            {
                // Frosted Glass：5px 圆角晶透玻璃胶囊按钮 + 顶部 44% 穹顶反光 + 底部莹光折射
                const auto rf   = r.toFloat();
                const float rad = juce::jmin (5.0f, juce::jmin (t.controlRadius, juce::jmin (rf.getWidth(), rf.getHeight()) * 0.48f));
                const bool hot  = pressed || active;
                const auto base = (active && translucentActive) ? PinkXP::sel.withAlpha (0.42f)
                                : hot         ? PinkXP::sel.withAlpha (pressed ? 0.92f : 0.84f)
                                : hovered     ? juce::Colours::white.withAlpha (0.28f)
                                : accentAlert ? PinkXP::pink400.withAlpha (0.46f)
                                : muted       ? juce::Colours::white.withAlpha (0.08f)
                                              : juce::Colours::white.withAlpha (0.17f);
                g.setColour (base);
                g.fillRoundedRectangle (rf, rad);
                if (! pressed && r.getHeight() >= 8)
                {
                    auto sheen = rf.reduced (1.5f);
                    sheen.setHeight (juce::jmax (2.0f, rf.getHeight() * 0.44f));
                    juce::ColourGradient dome (juce::Colours::white.withAlpha (hot ? 0.34f : 0.22f),
                                               0.0f, sheen.getY(),
                                               juce::Colours::white.withAlpha (0.03f),
                                               0.0f, sheen.getBottom(), false);
                    g.setGradientFill (dome);
                    g.fillRoundedRectangle (sheen, juce::jmax (1.0f, rad - 1.5f));
                }
                const auto border = (hot || hovered) ? PinkXP::sel.interpolatedWith (juce::Colours::white, 0.38f).withAlpha (0.88f)
                                  : accentAlert      ? PinkXP::pink200.withAlpha (0.92f)
                                                     : juce::Colours::white.withAlpha (muted ? 0.18f : 0.34f);
                g.setColour (border);
                g.drawRoundedRectangle (rf.reduced (0.5f), rad, 1.0f);
                return (hot && ! translucentActive) ? PinkXP::readableInkOn (PinkXP::sel)
                     : (active && translucentActive) ? juce::Colours::white.withAlpha (0.96f)
                     : accentAlert ? PinkXP::pink50
                     : muted       ? juce::Colour (0xDD, 0xDD, 0xDD).withAlpha (0.58f)
                                   : juce::Colours::white.withAlpha (0.95f);
            }

            case PinkXP::FrameStyle::neonTube:
            {
                // Neon Cyber：2px 科技角暗烟熏 HUD 按键 + 双通道发光霓虹管边
                const auto rf   = r.toFloat();
                const float rad = juce::jmin (2.0f, juce::jmin (rf.getWidth(), rf.getHeight()) * 0.25f);
                const bool hot  = pressed || active;
                const auto bg   = (active && translucentActive) ? PinkXP::sel.withAlpha (0.32f)
                                : hot         ? PinkXP::sel.withAlpha (pressed ? 0.48f : 0.34f)
                                : hovered     ? PinkXP::sel.withAlpha (0.22f)
                                : accentAlert ? PinkXP::pink400.withAlpha (0.34f)
                                              : juce::Colour (0x0b, 0x07, 0x16).withAlpha (muted ? 0.68f : 0.86f);
                g.setColour (bg);
                g.fillRoundedRectangle (rf, rad);
                if (hot || hovered || accentAlert)
                {
                    g.setColour (PinkXP::sel.withAlpha (0.42f));
                    g.drawRoundedRectangle (rf.reduced (0.5f), rad, 2.0f);
                    g.setColour (PinkXP::sel.interpolatedWith (juce::Colours::white, hot ? 0.54f : 0.34f));
                    g.drawRoundedRectangle (rf.reduced (1.0f), juce::jmax (0.5f, rad - 0.5f), 1.0f);
                    if ((active && translucentActive) && r.getWidth() >= 8 && r.getHeight() >= 8)
                    {
                        g.setColour (PinkXP::sel.interpolatedWith (juce::Colours::white, 0.45f));
                        g.fillRect (r.getX() + 2, r.getBottom() - 3, r.getWidth() - 4, 2);
                    }
                }
                else
                {
                    g.setColour (PinkXP::sel.withAlpha (muted ? 0.34f : 0.62f));
                    g.drawRoundedRectangle (rf.reduced (0.5f), rad, 1.0f);
                }
                return (hot || accentAlert) ? PinkXP::sel.interpolatedWith (juce::Colours::white, 0.78f)
                     : muted                ? juce::Colour (0xDD, 0xDD, 0xDD).withAlpha (0.56f)
                                            : juce::Colour (0xF0, 0xEE, 0xF8);
            }

            case PinkXP::FrameStyle::crtPhosphor:
            {
                // CRT Terminal：0px 锐利荧光终端按键（激活/按下反相亮荧光底 + WCAG 可读字）
                const bool hot = pressed || active;
                if (hot && ! translucentActive)
                {
                    g.setColour (PinkXP::sel);
                    g.fillRect (r);
                    return PinkXP::readableInkOn (PinkXP::sel);
                }
                const auto bg = (active && translucentActive) ? PinkXP::sel.withAlpha (0.34f)
                              : hovered     ? PinkXP::sel.withAlpha (0.24f)
                              : accentAlert ? PinkXP::sel.withAlpha (0.32f)
                                            : juce::Colour (0x05, 0x0b, 0x07).withAlpha (0.86f);
                g.setColour (bg);
                g.fillRect (r);
                g.setColour (PinkXP::sel.withAlpha ((hovered || accentAlert || active) ? 0.96f : (muted ? 0.40f : 0.72f)));
                g.drawRect (r, 1);
                return (hovered || accentAlert || active) ? PinkXP::sel.interpolatedWith (juce::Colours::white, 0.30f)
                     : muted                              ? PinkXP::sel.withAlpha (0.55f)
                                                          : juce::Colour (0xEC, 0xF4, 0xEE);
            }

            case PinkXP::FrameStyle::modernStudio:
            default:
            {
                // Modern Minimal：4-5px 圆角演播室胶囊键 + 清晰激活染色与细描边
                const auto rf   = r.toFloat();
                const float rad = juce::jmin (t.controlRadius, juce::jmin (rf.getWidth(), rf.getHeight()) * 0.48f);
                const bool hot  = pressed || active;
                const auto bg   = (active && translucentActive) ? PinkXP::sel.withAlpha (0.36f)
                                : hot         ? PinkXP::sel.withAlpha (pressed ? 0.94f : 0.86f)
                                : hovered     ? juce::Colours::white.withAlpha (0.20f)
                                : accentAlert ? PinkXP::sel.withAlpha (0.28f)
                                : muted       ? juce::Colours::white.withAlpha (0.055f)
                                              : juce::Colours::white.withAlpha (0.115f);
                g.setColour (bg);
                g.fillRoundedRectangle (rf, rad);

                if (hot && ! pressed && ! translucentActive && rf.getHeight() >= 10.0f)
                {
                    g.setColour (juce::Colours::white.withAlpha (0.12f));
                    g.fillRoundedRectangle (rf.withHeight (rf.getHeight() * 0.45f), rad);
                }

                const auto border = hot         ? PinkXP::sel.interpolatedWith (juce::Colours::white, 0.22f)
                                  : hovered     ? PinkXP::sel.withAlpha (0.78f)
                                  : accentAlert ? PinkXP::sel.withAlpha (0.85f)
                                                : juce::Colours::white.withAlpha (muted ? 0.12f : 0.20f);
                g.setColour (border);
                g.drawRoundedRectangle (rf.reduced (0.5f), rad, 1.0f);
                return (hot && ! translucentActive) ? PinkXP::readableInkOn (PinkXP::sel)
                     : (active && translucentActive) ? PinkXP::sel.interpolatedWith (juce::Colours::white, 0.55f)
                     : accentAlert ? juce::Colours::white.withAlpha (0.96f)
                     : muted       ? juce::Colour (0xDD, 0xDD, 0xDD).withAlpha (0.52f)
                                   : juce::Colour (0xF2, 0xF2, 0xF4);
            }
        }
    }

    void drawMilkdropOverlaySlider (juce::Graphics& g,
                                    juce::Rectangle<int> sliderBounds,
                                    float proportion,
                                    juce::Colour fillCol,
                                    bool dragging,
                                    float neutralProportion = -1.0f)
    {
        if (sliderBounds.isEmpty())
            return;

        const auto& t = PinkXP::getSkinTokens();
        const float p = juce::jlimit (0.0f, 1.0f, proportion);
        const int valueX = sliderBounds.getX() + juce::roundToInt (sliderBounds.getWidth() * p);
        const bool bipolar = (neutralProportion >= 0.0f);
        const int neutralX = bipolar
            ? (sliderBounds.getX() + juce::roundToInt (sliderBounds.getWidth() * juce::jlimit (0.0f, 1.0f, neutralProportion)))
            : sliderBounds.getX();

        const int fillLeft = bipolar ? juce::jmin (neutralX, valueX) : sliderBounds.getX();
        const int fillW    = bipolar ? juce::jmax (1, std::abs (valueX - neutralX))
                                     : juce::jmax (0, valueX - sliderBounds.getX());

        const bool smooth = (t.frameStyle == PinkXP::FrameStyle::modernStudio
                             || t.frameStyle == PinkXP::FrameStyle::frostedGlass);
        const auto trackF = sliderBounds.toFloat();

        // 1. 轨道与已填充条
        if (t.frameStyle == PinkXP::FrameStyle::pixelBevel)
        {
            // Pixel XP：0px 锐利 Win95 凹陷暗槽 + 锐利直角像素填充条
            g.setColour (juce::Colour (0x0c, 0x0b, 0x10).withAlpha (0.92f));
            g.fillRect (sliderBounds);
            g.setColour (PinkXP::dark.withAlpha (0.95f));
            g.fillRect (sliderBounds.getX(), sliderBounds.getY(), sliderBounds.getWidth(), 1);
            g.fillRect (sliderBounds.getX(), sliderBounds.getY(), 1, sliderBounds.getHeight());
            g.setColour (PinkXP::hl.withAlpha (0.52f));
            g.fillRect (sliderBounds.getX() + 1, sliderBounds.getBottom() - 1, sliderBounds.getWidth() - 1, 1);
            g.fillRect (sliderBounds.getRight() - 1, sliderBounds.getY() + 1, 1, sliderBounds.getHeight() - 1);

            const auto inner = sliderBounds.reduced (1);
            if (fillW > 0 && ! inner.isEmpty())
            {
                const auto fillR = juce::Rectangle<int> (fillLeft, inner.getY(), fillW, inner.getHeight())
                                       .getIntersection (inner);
                if (! fillR.isEmpty())
                {
                    g.setColour (fillCol.withAlpha (0.88f));
                    g.fillRect (fillR);
                    if (fillR.getHeight() >= 3)
                    {
                        g.setColour (juce::Colours::white.withAlpha (0.28f));
                        g.fillRect (fillR.getX(), fillR.getY(), fillR.getWidth(), 1);
                    }
                }
            }
        }
        else if (smooth)
        {
            const bool isModern = (t.frameStyle == PinkXP::FrameStyle::modernStudio);
            const auto drawTrack = isModern
                                     ? trackF.withSizeKeepingCentre (trackF.getWidth(), juce::jmin (4.5f, trackF.getHeight()))
                                     : trackF;
            const float trackRad = juce::jmin (t.sliderTrackRadius, drawTrack.getHeight() * 0.5f);
            g.setColour (juce::Colours::white.withAlpha (isModern ? 0.18f : 0.16f));
            g.fillRoundedRectangle (drawTrack, trackRad);
            g.setColour (juce::Colours::white.withAlpha (t.frameStyle == PinkXP::FrameStyle::frostedGlass ? 0.24f : 0.12f));
            g.drawRoundedRectangle (drawTrack.reduced (0.5f), trackRad, 1.0f);

            if (fillW > 0)
            {
                const auto fillF = juce::Rectangle<float> ((float) fillLeft, drawTrack.getY(),
                                                           (float) fillW, drawTrack.getHeight())
                                       .getIntersection (drawTrack);
                if (fillF.getWidth() > 0.5f)
                {
                    g.setColour (fillCol.withAlpha (isModern ? 0.92f : 0.86f));
                    g.fillRoundedRectangle (fillF, trackRad);
                    if (t.frameStyle == PinkXP::FrameStyle::frostedGlass && fillF.getHeight() >= 4.0f)
                    {
                        auto sheen = fillF.withHeight (fillF.getHeight() * 0.45f);
                        g.setColour (juce::Colours::white.withAlpha (0.30f));
                        g.fillRoundedRectangle (sheen, juce::jmax (0.5f, trackRad * 0.7f));
                    }
                }
            }
        }
        else
        {
            // Chrome / Neon / CRT：0px 锐利材质轨道
            g.setColour (juce::Colour (0x08, 0x0a, 0x0f).withAlpha (0.94f));
            g.fillRect (sliderBounds);
            const auto borderCol = (t.frameStyle == PinkXP::FrameStyle::machinedChrome)
                                       ? PinkXP::dark
                                       : PinkXP::sel.withAlpha (0.58f);
            g.setColour (borderCol);
            g.drawRect (sliderBounds, 1);

            if (t.frameStyle == PinkXP::FrameStyle::machinedChrome && sliderBounds.getHeight() >= 4)
            {
                // CNC 凹槽顶部内阴影 + 底部微铬反光沿
                g.setColour (juce::Colours::black.withAlpha (0.65f));
                g.fillRect (sliderBounds.getX() + 1, sliderBounds.getY() + 1, sliderBounds.getWidth() - 2, 1);
                g.setColour (juce::Colours::white.withAlpha (0.24f));
                g.fillRect (sliderBounds.getX() + 1, sliderBounds.getBottom() - 1, sliderBounds.getWidth() - 2, 1);
            }

            const auto inner = sliderBounds.reduced (1);
            if (fillW > 0 && ! inner.isEmpty())
            {
                const auto fillR = juce::Rectangle<int> (fillLeft, inner.getY(), fillW, inner.getHeight())
                                       .getIntersection (inner);
                if (! fillR.isEmpty())
                {
                    if (t.frameStyle == PinkXP::FrameStyle::machinedChrome)
                    {
                        // 3-band 金属宝石推子槽填充（顶高光折痕 + 饱满核心 + 底斜角阴影）
                        juce::ColourGradient grad (fillCol.interpolatedWith (juce::Colours::white, 0.36f),
                                                   0.0f, (float) fillR.getY(),
                                                   fillCol.darker (0.26f),
                                                   0.0f, (float) fillR.getBottom(), false);
                        grad.addColour (0.42, fillCol);
                        g.setGradientFill (grad);
                        g.fillRect (fillR);
                        if (fillR.getHeight() >= 3)
                        {
                            g.setColour (fillCol.interpolatedWith (juce::Colours::white, 0.55f).withAlpha (0.85f));
                            g.fillRect (fillR.getX(), fillR.getY(), fillR.getWidth(), 1);
                        }
                    }
                    else if (t.frameStyle == PinkXP::FrameStyle::neonTube)
                    {
                        // Neon Cyber：激光发光滑块填充（外柔光 + 灼热白芯激光束）
                        g.setColour (fillCol.withAlpha (0.34f));
                        g.fillRect (fillR.expanded (0, 1).getIntersection (sliderBounds));
                        g.setColour (fillCol.withAlpha (0.90f));
                        g.fillRect (fillR);
                        g.setColour (fillCol.interpolatedWith (juce::Colours::white, 0.60f));
                        g.fillRect (fillR.getX(), fillR.getCentreY(), fillR.getWidth(), 1);
                    }
                    else // crtPhosphor
                    {
                        g.setColour (fillCol.withAlpha (0.92f));
                        g.fillRect (fillR);
                        if (fillR.getHeight() >= 3)
                        {
                            g.setColour (fillCol.interpolatedWith (juce::Colours::white, 0.38f));
                            g.fillRect (fillR.getX(), fillR.getCentreY(), fillR.getWidth(), 1);
                        }
                    }
                }
            }
        }

        // 中性点刻度线（Tweak 双向滑块）
        if (bipolar)
        {
            g.setColour (juce::Colours::white.withAlpha (0.55f));
            g.fillRect (neutralX - 1, sliderBounds.getY() - 2, 2, sliderBounds.getHeight() + 4);
        }

        // 2. 滑块手柄（Pixel XP：0px 锐利方块斜角；Modern/Glass：圆润/圆形手柄）
        constexpr int kKnobSize = 12;
        const int knobX = juce::jlimit (sliderBounds.getX() - 2,
                                        sliderBounds.getRight() - kKnobSize + 2,
                                        valueX - kKnobSize / 2);
        const auto knobBounds = juce::Rectangle<int> (
            knobX, sliderBounds.getY() - (kKnobSize - sliderBounds.getHeight()) / 2,
            kKnobSize, kKnobSize);

        const auto thumbFill  = dragging ? PinkXP::pink200 : PinkXP::btnFace;
        const auto thumbState = dragging ? PinkXP::ControlState::pressed : PinkXP::ControlState::normal;

        if (t.frameStyle == PinkXP::FrameStyle::pixelBevel)
        {
            PinkXP::drawRaised (g, knobBounds, dragging ? PinkXP::pink200 : PinkXP::pink100);
            // 中央 1px 像素刻槽（经典硬件推子触感）
            g.setColour (PinkXP::shdw.withAlpha (0.75f));
            g.fillRect (knobBounds.getCentreX() - 1, knobBounds.getY() + 3, 1, knobBounds.getHeight() - 6);
            g.setColour (PinkXP::hl.withAlpha (0.85f));
            g.fillRect (knobBounds.getCentreX(), knobBounds.getY() + 3, 1, knobBounds.getHeight() - 6);
        }
        else if (smooth)
        {
            if (t.frameStyle == PinkXP::FrameStyle::modernStudio)
            {
                // Modern Minimal：精致演播室纯圆推子头（轻阴影 + 锐利描边 + 拖动核心点）
                const float dia = 11.0f;
                const float cx  = (float) juce::jlimit (sliderBounds.getX() + 4, sliderBounds.getRight() - 4, valueX);
                const float cy  = trackF.getCentreY();
                const juce::Rectangle<float> circ (cx - dia * 0.5f, cy - dia * 0.5f, dia, dia);

                g.setColour (juce::Colours::black.withAlpha (0.36f));
                g.fillEllipse (circ.translated (0.0f, 1.0f));

                g.setColour (dragging ? PinkXP::sel.interpolatedWith (juce::Colours::white, 0.35f)
                                      : juce::Colour (0xf4, 0xf5, 0xf8));
                g.fillEllipse (circ);

                g.setColour (dragging ? PinkXP::sel : juce::Colours::black.withAlpha (0.45f));
                g.drawEllipse (circ.reduced (0.5f), 1.15f);

                if (dragging)
                {
                    g.setColour (PinkXP::sel);
                    g.fillEllipse (cx - 1.75f, cy - 1.75f, 3.5f, 3.5f);
                }
            }
            else
            {
                // Frosted Glass：晶透圆球水晶推子帽（柔和底影 + 玻璃珠球面渐变 + 左上镜面新月高光）
                const float dia = 12.0f;
                const float cx  = (float) juce::jlimit (sliderBounds.getX() + 4, sliderBounds.getRight() - 4, valueX);
                const float cy  = trackF.getCentreY();
                const juce::Rectangle<float> orb (cx - dia * 0.5f, cy - dia * 0.5f, dia, dia);

                g.setColour (juce::Colours::black.withAlpha (0.34f));
                g.fillEllipse (orb.translated (0.0f, 1.2f));
                if (dragging)
                {
                    g.setColour (PinkXP::sel.withAlpha (0.42f));
                    g.fillEllipse (orb.expanded (2.0f));
                }

                const auto orbBase = dragging ? PinkXP::sel.interpolatedWith (juce::Colours::white, 0.35f)
                                              : juce::Colour (0xe6, 0xf0, 0xfa);
                juce::ColourGradient grad (juce::Colours::white, cx, orb.getY(),
                                           orbBase.darker (0.18f), cx, orb.getBottom(), false);
                grad.addColour (0.55, orbBase);
                g.setGradientFill (grad);
                g.fillEllipse (orb);

                // 顶部水晶穹顶高光点
                g.setColour (juce::Colours::white.withAlpha (0.82f));
                g.fillEllipse (orb.getX() + 2.5f, orb.getY() + 1.5f, dia - 5.0f, dia * 0.38f);

                g.setColour (dragging ? PinkXP::sel : juce::Colours::white.withAlpha (0.62f));
                g.drawEllipse (orb.reduced (0.5f), 1.0f);
            }
        }
        else if (t.frameStyle == PinkXP::FrameStyle::machinedChrome)
        {
            // Millennium Chrome：CNC 机加工矩形拉丝金属推子帽（4-stop 金属折痕 + 顶左镜面沿 + 中央双像素蚀刻握槽）
            const auto capBase = dragging ? PinkXP::sel.interpolatedWith (PinkXP::btnFace, 0.55f)
                                          : PinkXP::btnFace.interpolatedWith (juce::Colours::white, 0.16f);
            juce::ColourGradient grad (capBase.interpolatedWith (juce::Colours::white, 0.38f),
                                       0.0f, (float) knobBounds.getY(),
                                       capBase.darker (0.26f),
                                       0.0f, (float) knobBounds.getBottom(), false);
            grad.addColour (0.38, capBase.interpolatedWith (juce::Colours::white, 0.12f));
            grad.addColour (0.42, capBase.darker (0.06f));
            g.setGradientFill (grad);
            g.fillRect (knobBounds);

            g.setColour (PinkXP::dark);
            g.drawRect (knobBounds, 1);
            if (knobBounds.getWidth() >= 6 && knobBounds.getHeight() >= 6)
            {
                g.setColour (juce::Colours::white.withAlpha (0.76f));
                g.fillRect (knobBounds.getX() + 1, knobBounds.getY() + 1, knobBounds.getWidth() - 2, 1);
                g.fillRect (knobBounds.getX() + 1, knobBounds.getY() + 1, 1, knobBounds.getHeight() - 2);
                // 中央 CNC 蚀刻握持刻度线（1px 暗刻槽 + 1px 亮铬反光沿）
                const int cx = knobBounds.getCentreX();
                g.setColour (PinkXP::dark.withAlpha (0.82f));
                g.fillRect (cx - 1, knobBounds.getY() + 3, 1, knobBounds.getHeight() - 6);
                g.setColour (juce::Colours::white.withAlpha (0.72f));
                g.fillRect (cx, knobBounds.getY() + 3, 1, knobBounds.getHeight() - 6);
            }
        }
        else if (t.frameStyle == PinkXP::FrameStyle::neonTube)
        {
            // Neon Cyber：发光菱形赛博推子头（外霓虹光晕 + 暗烟熏菱芯 + 炽白发光边）
            const float cx = (float) juce::jlimit (sliderBounds.getX() + 4, sliderBounds.getRight() - 4, valueX);
            const float cy = trackF.getCentreY();
            const float r  = 6.5f;
            juce::Path diamond;
            diamond.startNewSubPath (cx,     cy - r);
            diamond.lineTo          (cx + r, cy);
            diamond.lineTo          (cx,     cy + r);
            diamond.lineTo          (cx - r, cy);
            diamond.closeSubPath();

            g.setColour (PinkXP::sel.withAlpha (dragging ? 0.45f : 0.28f));
            g.strokePath (diamond, juce::PathStrokeType (3.0f));
            g.setColour (dragging ? PinkXP::sel : juce::Colour (0x12, 0x0b, 0x20));
            g.fillPath (diamond);
            g.setColour (PinkXP::sel.interpolatedWith (juce::Colours::white, dragging ? 0.65f : 0.35f));
            g.strokePath (diamond, juce::PathStrokeType (1.25f));
            g.setColour (juce::Colours::white.withAlpha (0.90f));
            g.fillRect (juce::roundToInt (cx) - 1, juce::roundToInt (cy) - 1, 2, 2);
        }
        else if (t.frameStyle == PinkXP::FrameStyle::crtPhosphor)
        {
            // CRT Terminal：0px 方形荧光终端推子块 + 中央荧光准星
            g.setColour (dragging ? PinkXP::sel : juce::Colour (0x06, 0x10, 0x0a));
            g.fillRect (knobBounds);
            g.setColour (PinkXP::sel);
            g.drawRect (knobBounds, 1);
            g.setColour (dragging ? PinkXP::readableInkOn (PinkXP::sel) : PinkXP::sel);
            g.fillRect (knobBounds.getCentreX(), knobBounds.getY() + 3, 1, juce::jmax (2, knobBounds.getHeight() - 6));
        }
        else
        {
            PinkXP::Craft::drawControlChrome (g, knobBounds, thumbFill, thumbState, t.sliderThumbRadius);
        }
    }
}

void MilkdropModule::paintOverlayControlBar(juce::Graphics& g, juce::Rectangle<int> content)
{
    constexpr int kBarHeight = 26;

    if (content.getHeight() < kBarHeight)
        return;

    auto bar = content.withHeight(kBarHeight);
    const auto& tokens = PinkXP::getSkinTokens();

    // 风格化半透明暗色控制台容器
    drawMilkdropOverlaySurface (g, bar, true);

    // 按钮位置: [<] nameArea [keys] [reset] [tweak] [wave] [effects] [color] [auto] [>] [?]
    const auto layout = computeOverlayBarLayout(bar);
    const auto& prevBtn     = layout.prevBtn;
    const auto& keyLockBtn  = layout.keyLockBtn;
    const auto& resetAllBtn = layout.resetAllBtn;
    const auto& tweakBtn    = layout.tweakBtn;
    const auto& waveBtn     = layout.waveBtn;
    const auto& effectsBtn  = layout.effectsBtn;
    const auto& colorBtn    = layout.colorBtn;
    const auto& autoBtn     = layout.autoBtn;
    const auto& nextBtn     = layout.nextBtn;
    const auto& randomBtn   = layout.randomBtn;
    const auto& nameArea    = layout.nameArea;
    const int   idealTextBtnW = overlayTextBtnWidth();

    auto drawTextBtnLabel = [&](const juce::String& label, juce::Rectangle<int> r, bool shiftDown)
    {
        if (r.isEmpty())
            return;
        if (shiftDown)
            r.translate (1, 1);
        g.setFont(PinkXP::getFont(8.0f, juce::Font::bold));
        if (r.getWidth() < idealTextBtnW)
            g.drawFittedText(label, r.reduced(2, 0), juce::Justification::centred, 1, 0.72f);
        else
            g.drawText(label, r, juce::Justification::centred, false);
    };

    // 图标按钮（< / > / ?）
    auto drawBtn = [&](juce::Rectangle<int> r, const juce::String& text, OverlayButton btn)
    {
        const bool hovered = (hoveredOverlayBtn_ == btn);
        const bool pressed = (pressedOverlayBtn_ == btn);

        const auto fg = drawMilkdropOverlayButton (g, r, false, hovered, pressed);
        g.setColour (fg);
        auto tr = r;
        if (pressed)
            tr.translate (1, 1);
        // 图标字符（< / > / ?）：恒用英文 UI 字体，不随语言切换变样
        g.setFont (PinkXP::SymbolGlyphs::iconFont (9.0f, juce::Font::bold));
        g.drawText (y2k::tr (text), tr, juce::Justification::centred, false);
    };

    drawBtn(prevBtn,   "<",   OverlayButton::kPrev);
    drawBtn(nextBtn,   ">",   OverlayButton::kNext);
    drawBtn(randomBtn, "?",   OverlayButton::kRandom);

    // 面板切换 / 模式切换按钮统一走 drawMilkdropOverlayButton
    auto drawToggleBtn = [&](juce::Rectangle<int> r, const char* key, OverlayButton btn, bool active)
    {
        const bool hovered = (hoveredOverlayBtn_ == btn);
        const bool pressed = (pressedOverlayBtn_ == btn);
        const auto fg = drawMilkdropOverlayButton (g, r, active, hovered, pressed);
        g.setColour (fg);
        drawTextBtnLabel (y2k::tr (key), r, pressed || active);
    };

    drawToggleBtn (colorBtn,   "color",   OverlayButton::kColor,   isColorPanelOpen_);
    drawToggleBtn (effectsBtn, "effects", OverlayButton::kEffects, isEffectsPanelOpen_);
    drawToggleBtn (waveBtn,    "wave",    OverlayButton::kWave,    isWavePanelOpen_);
    drawToggleBtn (tweakBtn,   "tweak",   OverlayButton::kTweak,   isTweakPanelOpen_);

    // reset（一键全部重置）按钮：存在任何 color / effects / wave / tweak 覆盖时高亮提示，中性态柔和弱化
    {
        const bool hovered      = (hoveredOverlayBtn_ == OverlayButton::kResetAll);
        const bool pressed      = (pressedOverlayBtn_ == OverlayButton::kResetAll);
        const bool hasOverrides = hasAnyUserVisualOverrides();
        const auto fg = drawMilkdropOverlayButton (g, resetAllBtn, false, hovered, pressed,
                                                   ! hasOverrides, hasOverrides);
        g.setColour (fg);
        drawTextBtnLabel (y2k::tr ("reset"), resetAllBtn, pressed);
    }

    drawToggleBtn (keyLockBtn, "keys", OverlayButton::kKeyLock, keyboardControlEnabled_);
    drawToggleBtn (autoBtn,    "auto", OverlayButton::kAuto,    isAutoMode_);

    // 缓存 nameArea 供 hitTestOverlayButton 使用
    cachedNameArea_ = nameArea;
    if (nameArea.getWidth() <= 0)
        return;

    // 预设名：格式 "3/100  presetName"；尚未载入预设时仅显示 "(no preset)"，不拼接无效 "0/0" 前缀
    const int idx   = glView->GetCurrentPresetIndex();
    const int total = glView->GetTotalPresetCount();
    const bool hasValidIdx = (total > 0 && idx >= 0);

    const juce::String idxPart = hasValidIdx
        ? (juce::String(idx + 1) + "/" + juce::String(total) + "  ")
        : juce::String();
    juce::String namePart = glView->GetCurrentPresetName();
    if (namePart.isEmpty() && ! hasValidIdx)
        namePart = y2k::tr ("(no preset)");

    // name area 交互视觉：hover 时底部淡粉线，pressed 时亮粉底色（遵循 SkinTokens 圆角）
    bool nameHovered = (hoveredOverlayBtn_ == OverlayButton::kPresetName);
    bool namePressed = (pressedOverlayBtn_ == OverlayButton::kPresetName);
    if (namePressed)
    {
        g.setColour(PinkXP::pink300.withAlpha(0.18f));
        if (! tokens.pixelSnapped && tokens.controlRadius > 0.5f)
            g.fillRoundedRectangle(nameArea.toFloat(), juce::jmin(tokens.controlRadius, 4.0f));
        else
            g.fillRect(nameArea);
    }
    else if (nameHovered)
    {
        g.setColour(PinkXP::pink300.withAlpha(0.45f));
        g.fillRect(nameArea.getX(), nameArea.getBottom() - 1, nameArea.getWidth(), 1);
    }

    auto nameRect = nameArea;
    if (idxPart.isNotEmpty())
    {
        const float idxW = PinkXP::getFont(9.0f, juce::Font::bold).getStringWidthFloat(idxPart) + 2.0f;
        const auto idxRect = nameArea.withWidth(juce::jmin((int)idxW, nameArea.getWidth()));
        nameRect = nameArea.withTrimmedLeft(idxRect.getWidth());

        g.setColour(PinkXP::pink300.withAlpha(namePressed ? 1.0f : 0.95f));
        g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
        g.drawText(idxPart, idxRect, juce::Justification::centredLeft, true);
    }

    if (namePart.isNotEmpty() && nameRect.getWidth() > 0)
    {
        g.setColour(juce::Colour(0xEE, 0xEE, 0xEE).withAlpha(0.95f));
        g.setFont(PinkXP::getFont(9.0f, juce::Font::plain));
        g.drawText(namePart, nameRect, juce::Justification::centredLeft, true);
    }
}

// ---- 右下角收藏 / 切换库按钮 ----

bool MilkdropModule::isLibraryToggleVisible() const
{
    if (editor_ == nullptr)
        return false;
    return editor_->HasMilkdropLikedPresets();
}

juce::Rectangle<int> MilkdropModule::getLibraryButtonRect(
    juce::Rectangle<int> content, OverlayButton btn) const
{
    constexpr int kSize = kLibraryBtnSize;
    constexpr int kPad  = 4;
    const int y = content.getBottom() - kPad - kSize;

    // 切换按钮固定在最右；收藏按钮在其左（若切换按钮不可见则复用最右位置）。
    auto toggleBtn = juce::Rectangle<int>(content.getRight() - kPad - kSize, y, kSize, kSize);
    auto likeBtn   = juce::Rectangle<int>(toggleBtn.getX() - kPad - kSize, y, kSize, kSize);

    switch (btn)
    {
    case OverlayButton::kLibraryToggle:
        return toggleBtn;
    case OverlayButton::kLike:
        return isLibraryToggleVisible() ? likeBtn : toggleBtn;
    default:
        return {};
    }
}

MilkdropModule::OverlayButton MilkdropModule::hitTestLibraryButton(
    juce::Point<int> pos, juce::Rectangle<int> content) const
{
    if (getLibraryButtonRect(content, OverlayButton::kLike).contains(pos))
        return OverlayButton::kLike;
    if (isLibraryToggleVisible()
        && getLibraryButtonRect(content, OverlayButton::kLibraryToggle).contains(pos))
        return OverlayButton::kLibraryToggle;
    return OverlayButton::kNone;
}

void MilkdropModule::executeLibraryAction(OverlayButton btn)
{
    if (glView == nullptr || editor_ == nullptr)
        return;

    if (btn == OverlayButton::kLike)
    {
        // 收藏 / 取消收藏 toggle。
        const juce::String path = glView->GetCurrentPresetFilePath();
        if (path.isEmpty())
            return;
        juce::File src(path);
        if (!src.existsAsFile())
            return;

        juce::File likeDir = FindMilkdropLikeDirForModule();
        juce::File dst = likeDir.getChildFile(src.getFileName());

        if (dst.existsAsFile())
        {
            // 已收藏 → 取消收藏：删除收藏库内的对应预设文件。
            dst.deleteFile();
            // 若当前浏览的是收藏库，该预设已从列表中移除，需重扫并切换到下一个；
            // 若仍在内置库，仅删除收藏副本，内置列表不变，无需重扫。
            if (editor_->IsMilkdropUseLikeLibrary())
                glView->RequestUnlinkReload();
        }
        else
        {
            // 未收藏 → 收藏：拷贝当前 .milk 到收藏库目录。
            if (!likeDir.exists())
                likeDir.createDirectory();
            src.copyFileTo(dst);
        }
    }
    else if (btn == OverlayButton::kLibraryToggle)
    {
        // 切换预设库（内置库 ↔ 收藏库）。
        glView->RequestLibraryToggle();
    }
}

void MilkdropModule::paintLibraryButtons(juce::Graphics& g, juce::Rectangle<int> content)
{
    constexpr int kSize = kLibraryBtnSize;
    if (content.getHeight() < kSize + 8 || content.getWidth() < (kSize + 4) * 2 + 8)
        return;

    // 收藏按钮：当前预设已收藏时显示按下态（再次点击取消收藏）。
    const bool liked = [&]() {
        if (glView == nullptr) return false;
        const juce::String path = glView->GetCurrentPresetFilePath();
        if (path.isEmpty()) return false;
        return FindMilkdropLikeDirForModule()
            .getChildFile(juce::File(path).getFileName()).existsAsFile();
    }();

    // 切换按钮：当前浏览收藏库时显示按下态。
    const bool inLikeLib = (editor_ != nullptr) && editor_->IsMilkdropUseLikeLibrary();

    auto drawIconBtn = [&](juce::Rectangle<int> r, OverlayButton btn,
                           bool active, const std::function<void(juce::Graphics&, juce::Rectangle<float>)>& icon)
    {
        const bool hovered = (hoveredOverlayBtn_ == btn);
        const bool pressed = (pressedOverlayBtn_ == btn);
        const auto fg = drawMilkdropOverlayButton (g, r, active, hovered, pressed,
                                                   false, false, /*translucentActive*/ true);
        g.setColour (fg);
        auto iconRect = r.toFloat().reduced (5.0f);
        if (pressed)
            iconRect.translate (1.0f, 1.0f);
        icon (g, iconRect);
    };

    // 收藏按钮（爱心）
    {
        auto likeBtn = getLibraryButtonRect(content, OverlayButton::kLike);
        drawIconBtn(likeBtn, OverlayButton::kLike, liked, [](juce::Graphics& gg, juce::Rectangle<float> rf)
        {
            // 胖心形（拼接方案）：两个饱满的圆 + 一个宽矮的底部三角。
            // 两个圆半径 0.30w、圆心距 0.40w，重叠 0.20w，顶部形成自然的 V 形凹陷；
            // 三角底边几乎贴满宽度、尖角收于底部，顶点插入两圆交界处自然衔接。
            const float x = rf.getX();
            const float y = rf.getY();
            const float w = rf.getWidth();
            const float h = rf.getHeight();

            juce::Path heart;
            heart.addEllipse(x, y, w * 0.60f, h * 0.60f);           // 左圆
            heart.addEllipse(x + w * 0.40f, y, w * 0.60f, h * 0.60f); // 右圆
            heart.addTriangle(x + w * 0.01f, y + h * 0.40f,          // 底部三角
                              x + w * 0.99f, y + h * 0.40f,
                              x + w * 0.50f, y + h * 0.97f);
            gg.fillPath(heart);
        });
    }

    // 切换按钮（双向箭头）：仅收藏库非空时显示
    if (isLibraryToggleVisible())
    {
        auto toggleBtn = getLibraryButtonRect(content, OverlayButton::kLibraryToggle);
        drawIconBtn(toggleBtn, OverlayButton::kLibraryToggle, inLikeLib,
                    [](juce::Graphics& gg, juce::Rectangle<float> rf)
        {
            const float cx = rf.getCentreX();
            const float cy = rf.getCentreY();
            const float w  = rf.getWidth();
            const float h  = rf.getHeight();
            // 双向切换：一条水平箭杆贯穿，两端各一个放大的实心箭头头（尖朝外）。
            gg.fillRect(cx - w * 0.14f, cy - h * 0.07f, w * 0.28f, h * 0.14f);

            juce::Path left;
            left.addTriangle(cx - w * 0.40f, cy,
                             cx - w * 0.14f, cy - h * 0.26f,
                             cx - w * 0.14f, cy + h * 0.26f);
            gg.fillPath(left);

            juce::Path right;
            right.addTriangle(cx + w * 0.40f, cy,
                              cx + w * 0.14f, cy - h * 0.26f,
                              cx + w * 0.14f, cy + h * 0.26f);
            gg.fillPath(right);
        });
    }
}

// ---- 加载指示器绘制 ----

void MilkdropModule::PaintLoadingIndicator(juce::Graphics& g, juce::Rectangle<int> content)
{
  // 自动轮播模式下不显示切换提示，避免右下角频繁闪烁
  if (isAutoMode_)
    return;

  // projectM soft-cut 过渡在 1-2 秒内完成，指示器只需短暂提示"正在切换"，
  // 不应延长到过渡结束之后。连续点击会不断重置时间戳、保持指示器可见。
  constexpr int64_t kIndicatorDurationMs = 1200;

  if (glView == nullptr)
    return;

  int64_t last_switch = glView->GetLastPresetSwitchTimeMs();
  if (last_switch == 0)
    return;

  auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  int64_t elapsed = now - last_switch;
  if (elapsed > kIndicatorDurationMs)
    return;

  // 右下角半透明提示条
  constexpr int kBarW = 90;
  constexpr int kBarH = 18;
  constexpr int kPad = 4;

  auto bar = juce::Rectangle<int>(content.getRight() - kPad - kBarW,
                                   content.getBottom() - kPad - kBarH,
                                   kBarW, kBarH);

  // 渐出：最后 300ms 透明度从 0.8 线性降到 0
  float alpha = 0.8f;
  constexpr int64_t kFadeMs = 300;
  int64_t fadeout = kIndicatorDurationMs - kFadeMs;
  if (elapsed > fadeout)
    alpha = 0.8f * (1.0f - static_cast<float>(elapsed - fadeout) / static_cast<float>(kFadeMs));

  const auto& tokens = PinkXP::getSkinTokens();
  const float rad = (! tokens.pixelSnapped && tokens.badgeRadius > 0.5f)
                      ? juce::jmin (tokens.badgeRadius, (float) bar.getHeight() * 0.5f)
                      : 0.0f;

  g.setColour(juce::Colour(0x00, 0x00, 0x00).withAlpha(alpha * 0.78f));
  if (rad > 0.5f)
  {
    g.fillRoundedRectangle(bar.toFloat(), rad);
    g.setColour(PinkXP::pink300.withAlpha(alpha * 0.55f));
    g.drawRoundedRectangle(bar.toFloat().reduced(0.5f), rad, 1.0f);
  }
  else
  {
    g.fillRect(bar);
    g.setColour(PinkXP::pink300.withAlpha(alpha * 0.75f));
    g.drawRect(bar, 1);
  }

  g.setColour(PinkXP::pink300.withAlpha(alpha));
  g.setFont(PinkXP::getFont(8.0f, juce::Font::plain));
  g.drawText(y2k::tr ("Switching..."), bar, juce::Justification::centred, false);
}

// ==========================================================
// AutoIntervalDialog：自定义 PinkXP 风格自动轮播间隔设置对话框
// ==========================================================
MilkdropModule::AutoIntervalDialog::AutoIntervalDialog(
    MilkdropModule& owner_, float current,
    std::function<void(float)> onResult)
    : owner_(owner_), onResult_(std::move(onResult))
{
    setOpaque(false);
    setInterceptsMouseClicks(true, true);

    const auto dlgStyle = PinkXP::getSkinTokens().frameStyle;
    const bool themedDlg = (dlgStyle == PinkXP::FrameStyle::machinedChrome
                            || dlgStyle == PinkXP::FrameStyle::frostedGlass
                            || dlgStyle == PinkXP::FrameStyle::neonTube
                            || dlgStyle == PinkXP::FrameStyle::crtPhosphor);
    editor_.setText(juce::String(current, 3));
    editor_.setFont(PinkXP::getFont(11.0f, juce::Font::plain));
    editor_.setColour(juce::TextEditor::backgroundColourId,
                      themedDlg ? PinkXP::content : PinkXP::pink50);
    editor_.setColour(juce::TextEditor::textColourId,
                      themedDlg ? PinkXP::readableInkOn(PinkXP::content) : PinkXP::ink);
    editor_.setColour(juce::TextEditor::outlineColourId,
                      (dlgStyle == PinkXP::FrameStyle::neonTube || dlgStyle == PinkXP::FrameStyle::crtPhosphor)
                          ? PinkXP::sel.withAlpha(0.75f)
                          : (themedDlg ? PinkXP::dark.withAlpha(0.80f) : PinkXP::pink600.withAlpha(0.6f)));
    editor_.setColour(juce::TextEditor::focusedOutlineColourId,
                      themedDlg ? PinkXP::sel : PinkXP::pink500.withAlpha(0.9f));
    editor_.setInputRestrictions(8, "0123456789.");
    editor_.setSelectAllWhenFocused(true);
    AutoIntervalDialog* self = this;
    editor_.onReturnKey = [this, self] {
        float val = juce::jlimit(kMinAutoInterval, kMaxAutoInterval,
                                 editor_.getText().getFloatValue());
        val = std::round(val * 1000.0f) / 1000.0f;
        onResult_(val);
        exitModalState(1);
        setVisible(false);
        juce::MessageManager::callAsync([self] { delete self; });
    };
    editor_.onEscapeKey = [this, self] {
        exitModalState(0);
        setVisible(false);
        juce::MessageManager::callAsync([self] { delete self; });
    };
    addAndMakeVisible(editor_);
}

void MilkdropModule::AutoIntervalDialog::paint(juce::Graphics& g)
{
    // 半透明暗色遮罩
    g.fillAll(juce::Colour(0x00, 0x00, 0x00).withAlpha(0.55f));

    // 对话框主体位置（居中）
    constexpr int kDlgW = 290;
    constexpr int kDlgH = 130;
    auto dlg = juce::Rectangle<int>(
        (getWidth() - kDlgW) / 2, (getHeight() - kDlgH) / 2,
        kDlgW, kDlgH);

    // 对话框外壳与标题栏跟随当前 Skin
    const auto& dlgTokens  = PinkXP::getSkinTokens();
    const bool isChromeDlg = (dlgTokens.frameStyle == PinkXP::FrameStyle::machinedChrome);
    const bool isGlassDlg  = (dlgTokens.frameStyle == PinkXP::FrameStyle::frostedGlass);
    const bool isNeonDlg   = (dlgTokens.frameStyle == PinkXP::FrameStyle::neonTube);
    const bool isCrtDlg    = (dlgTokens.frameStyle == PinkXP::FrameStyle::crtPhosphor);
    const bool hasStyledCard = (isChromeDlg || isGlassDlg || isNeonDlg || isCrtDlg);
    PinkXP::drawRaised(g, dlg, PinkXP::face);
    auto titleBar = juce::Rectangle<int>(dlg.getX() + 2, dlg.getY() + 2, dlg.getWidth() - 4, 22);
    PinkXP::drawPinkTitleBar(g, titleBar, "Set Auto Interval", 10.0f);
    g.setColour(PinkXP::titleBarDividerColour());
    g.fillRect(titleBar.getX(), titleBar.getBottom(), titleBar.getWidth(), 1);

    if (hasStyledCard)
    {
        const bool isDarkFace = PinkXP::face.getPerceivedBrightness() < 0.45f;
        const auto plateFill  = isNeonDlg  ? (isDarkFace ? PinkXP::face.interpolatedWith(juce::Colours::black, 0.24f)
                                                         : PinkXP::face.interpolatedWith(PinkXP::content, 0.48f))
                              : isCrtDlg   ? PinkXP::content.interpolatedWith(PinkXP::face, 0.25f)
                              : isGlassDlg ? (isDarkFace ? PinkXP::face.interpolatedWith(juce::Colours::white, 0.08f)
                                                         : PinkXP::face.interpolatedWith(juce::Colours::white, 0.42f))
                                           : (isDarkFace ? PinkXP::face.interpolatedWith(juce::Colours::white, 0.06f)
                                                         : PinkXP::face.interpolatedWith(PinkXP::content, 0.36f));
        const auto bodyPlate  = juce::Rectangle<int>(dlg.getX() + 8, titleBar.getBottom() + 4,
                                                     dlg.getWidth() - 16, dlg.getHeight() - 68);
        if (! dlgTokens.isChromaKey)
        {
            PinkXP::Craft::drawPanelWell(g, bodyPlate, plateFill, PinkXP::WellKind::card);
        }
        else if (isGlassDlg)
        {
            const auto rf = bodyPlate.toFloat();
            g.setColour(plateFill);
            g.fillRoundedRectangle(rf, dlgTokens.cardRadius);
            g.setColour(PinkXP::dark.withAlpha(0.32f));
            g.drawRoundedRectangle(rf.reduced(0.5f), dlgTokens.cardRadius, 1.0f);
        }
        else if (isNeonDlg || isCrtDlg)
        {
            g.setColour(plateFill);
            g.fillRect(bodyPlate);
            g.setColour(PinkXP::sel.withAlpha(isNeonDlg ? 0.80f : 0.72f));
            g.drawRect(bodyPlate, 1);
        }
        else
        {
            g.setColour(plateFill);
            g.fillRect(bodyPlate);
            g.setColour(PinkXP::dark.withAlpha(0.78f));
            g.drawRect(bodyPlate, 1);
            g.setColour(PinkXP::hl.withAlpha(0.55f));
            g.fillRect(bodyPlate.getX() + 1, bodyPlate.getY() + 1, bodyPlate.getWidth() - 2, 1);
        }

        const int seamY = dlg.getBottom() - 40;
        if (isChromeDlg)
        {
            g.setColour(PinkXP::dark.withAlpha(isDarkFace ? 0.72f : 0.48f));
            g.fillRect(dlg.getX() + 8, seamY, dlg.getWidth() - 16, 1);
            g.setColour(PinkXP::hl.interpolatedWith(juce::Colours::white, 0.50f).withAlpha(isDarkFace ? 0.28f : 0.74f));
            g.fillRect(dlg.getX() + 8, seamY + 1, dlg.getWidth() - 16, 1);
        }
        else if (isGlassDlg)
        {
            g.setColour(PinkXP::dark.withAlpha(isDarkFace ? 0.28f : 0.16f));
            g.fillRect(dlg.getX() + 8, seamY, dlg.getWidth() - 16, 1);
            g.setColour(juce::Colours::white.withAlpha(isDarkFace ? 0.16f : 0.65f));
            g.fillRect(dlg.getX() + 8, seamY + 1, dlg.getWidth() - 16, 1);
        }
        else
        {
            g.setColour(PinkXP::sel.withAlpha(isNeonDlg ? 0.72f : 0.65f));
            g.fillRect(dlg.getX() + 8, seamY, dlg.getWidth() - 16, 1);
        }
    }

    // 提示文字
    g.setColour(PinkXP::readableInkOn(PinkXP::face).withAlpha(0.85f));
    g.setFont(PinkXP::getFont(9.0f, juce::Font::plain));
    g.drawText(y2k::tr ("Enter interval (1.0-60.0 seconds):"),
               dlg.getX() + 14, dlg.getY() + 28,
               dlg.getWidth() - 28, 18,
               juce::Justification::centredLeft, false);

    // OK 按钮（右侧）
    auto goRect = juce::Rectangle<int>(
        dlg.getRight() - 66, dlg.getBottom() - 34, 54, 22);
    const auto goFill = isCrtDlg ? PinkXP::sel : PinkXP::btnFace;
    PinkXP::Craft::drawControlChrome(g, goRect, goFill,
                                     hasStyledCard ? PinkXP::ControlState::active : PinkXP::ControlState::normal);
    g.setColour(PinkXP::readableInkOn(goFill));
    g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
    g.drawText(y2k::tr ("OK"), goRect, juce::Justification::centred, false);

    // Cancel 按钮
    auto cancelRect = juce::Rectangle<int>(
        goRect.getX() - 62, dlg.getBottom() - 34, 54, 22);
    PinkXP::Craft::drawControlChrome(g, cancelRect, PinkXP::btnFace, PinkXP::ControlState::normal);
    g.setColour(PinkXP::readableInkOn(PinkXP::btnFace));
    g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
    g.drawText(y2k::tr ("Cancel"), cancelRect, juce::Justification::centred, false);
}

void MilkdropModule::AutoIntervalDialog::resized()
{
    constexpr int kDlgW = 290;
    constexpr int kDlgH = 130;
    auto dlgX = (getWidth() - kDlgW) / 2;
    auto dlgY = (getHeight() - kDlgH) / 2;

    // TextEditor 位于提示文字下方
    editor_.setBounds(dlgX + 14, dlgY + 48, kDlgW - 28, 24);
}

void MilkdropModule::AutoIntervalDialog::mouseDown(const juce::MouseEvent&)
{
    constexpr int kDlgW = 290;
    constexpr int kDlgH = 130;
    auto dlgX = (getWidth() - kDlgW) / 2;
    auto dlgY = (getHeight() - kDlgH) / 2;

    // OK 按钮区域
    auto goRect = juce::Rectangle<int>(
        dlgX + kDlgW - 66, dlgY + kDlgH - 34, 54, 22);
    if (goRect.contains(getMouseXYRelative())) {
      float val = juce::jlimit(kMinAutoInterval, kMaxAutoInterval,
                               editor_.getText().getFloatValue());
      val = std::round(val * 1000.0f) / 1000.0f;
      onResult_(val);
      exitModalState(1);
      setVisible(false);
      juce::MessageManager::callAsync([self = this] { delete self; });
      return;
    }

    // Cancel 按钮区域
    auto cancelRect = juce::Rectangle<int>(
        goRect.getX() - 62, dlgY + kDlgH - 34, 54, 22);
    if (cancelRect.contains(getMouseXYRelative())) {
      exitModalState(0);
      setVisible(false);
      juce::MessageManager::callAsync([self = this] { delete self; });
      return;
    }
}

// ==========================================================
// PresetJumpDialog：自定义 PinkXP 风格预设跳转对话框
// ==========================================================
MilkdropModule::PresetJumpDialog::PresetJumpDialog(
    MilkdropModule& owner_, int total, int current,
    std::function<void(int)> onResult)
    : owner_(owner_), total_(total), onResult_(std::move(onResult))
{
    setOpaque(false);
    setInterceptsMouseClicks(true, true);

    const auto dlgStyle = PinkXP::getSkinTokens().frameStyle;
    const bool themedDlg = (dlgStyle == PinkXP::FrameStyle::machinedChrome
                            || dlgStyle == PinkXP::FrameStyle::frostedGlass
                            || dlgStyle == PinkXP::FrameStyle::neonTube
                            || dlgStyle == PinkXP::FrameStyle::crtPhosphor);
    editor_.setText(juce::String(current + 1));
    editor_.setFont(PinkXP::getFont(11.0f, juce::Font::plain));
    editor_.setColour(juce::TextEditor::backgroundColourId,
                      themedDlg ? PinkXP::content : PinkXP::pink50);
    editor_.setColour(juce::TextEditor::textColourId,
                      themedDlg ? PinkXP::readableInkOn(PinkXP::content) : PinkXP::ink);
    editor_.setColour(juce::TextEditor::outlineColourId,
                      (dlgStyle == PinkXP::FrameStyle::neonTube || dlgStyle == PinkXP::FrameStyle::crtPhosphor)
                          ? PinkXP::sel.withAlpha(0.75f)
                          : (themedDlg ? PinkXP::dark.withAlpha(0.80f) : PinkXP::pink600.withAlpha(0.6f)));
    editor_.setColour(juce::TextEditor::focusedOutlineColourId,
                      themedDlg ? PinkXP::sel : PinkXP::pink500.withAlpha(0.9f));
    editor_.setInputRestrictions(6, "0123456789");
    editor_.setSelectAllWhenFocused(true);
    PresetJumpDialog* self = this;
    editor_.onReturnKey = [this, self] {
        juce::String input = editor_.getText().trim();
        int val = input.getIntValue();
        if (val < 1) val = 1;
        if (val > total_) val = total_;
        onResult_(val - 1);
        exitModalState(1);
        setVisible(false);
        juce::MessageManager::callAsync([self] { delete self; });
    };
    editor_.onEscapeKey = [this, self] {
        exitModalState(0);
        setVisible(false);
        juce::MessageManager::callAsync([self] { delete self; });
    };
    addAndMakeVisible(editor_);
}

void MilkdropModule::PresetJumpDialog::paint(juce::Graphics& g)
{
    // 半透明暗色遮罩
    g.fillAll(juce::Colour(0x00, 0x00, 0x00).withAlpha(0.55f));

    // 对话框主体位置（居中）
    constexpr int kDlgW = 290;
    constexpr int kDlgH = 130;
    auto dlg = juce::Rectangle<int>(
        (getWidth() - kDlgW) / 2, (getHeight() - kDlgH) / 2,
        kDlgW, kDlgH);

    // 对话框外壳与标题栏跟随当前 Skin
    const auto& dlgTokens  = PinkXP::getSkinTokens();
    const bool isChromeDlg = (dlgTokens.frameStyle == PinkXP::FrameStyle::machinedChrome);
    const bool isGlassDlg  = (dlgTokens.frameStyle == PinkXP::FrameStyle::frostedGlass);
    const bool isNeonDlg   = (dlgTokens.frameStyle == PinkXP::FrameStyle::neonTube);
    const bool isCrtDlg    = (dlgTokens.frameStyle == PinkXP::FrameStyle::crtPhosphor);
    const bool hasStyledCard = (isChromeDlg || isGlassDlg || isNeonDlg || isCrtDlg);
    PinkXP::drawRaised(g, dlg, PinkXP::face);
    auto titleBar = juce::Rectangle<int>(dlg.getX() + 2, dlg.getY() + 2, dlg.getWidth() - 4, 22);
    PinkXP::drawPinkTitleBar(g, titleBar, "Jump to Preset", 10.0f);
    g.setColour(PinkXP::titleBarDividerColour());
    g.fillRect(titleBar.getX(), titleBar.getBottom(), titleBar.getWidth(), 1);

    if (hasStyledCard)
    {
        const bool isDarkFace = PinkXP::face.getPerceivedBrightness() < 0.45f;
        const auto plateFill  = isNeonDlg  ? (isDarkFace ? PinkXP::face.interpolatedWith(juce::Colours::black, 0.24f)
                                                         : PinkXP::face.interpolatedWith(PinkXP::content, 0.48f))
                              : isCrtDlg   ? PinkXP::content.interpolatedWith(PinkXP::face, 0.25f)
                              : isGlassDlg ? (isDarkFace ? PinkXP::face.interpolatedWith(juce::Colours::white, 0.08f)
                                                         : PinkXP::face.interpolatedWith(juce::Colours::white, 0.42f))
                                           : (isDarkFace ? PinkXP::face.interpolatedWith(juce::Colours::white, 0.06f)
                                                         : PinkXP::face.interpolatedWith(PinkXP::content, 0.36f));
        const auto bodyPlate  = juce::Rectangle<int>(dlg.getX() + 8, titleBar.getBottom() + 4,
                                                     dlg.getWidth() - 16, dlg.getHeight() - 68);
        if (! dlgTokens.isChromaKey)
        {
            PinkXP::Craft::drawPanelWell(g, bodyPlate, plateFill, PinkXP::WellKind::card);
        }
        else if (isGlassDlg)
        {
            const auto rf = bodyPlate.toFloat();
            g.setColour(plateFill);
            g.fillRoundedRectangle(rf, dlgTokens.cardRadius);
            g.setColour(PinkXP::dark.withAlpha(0.32f));
            g.drawRoundedRectangle(rf.reduced(0.5f), dlgTokens.cardRadius, 1.0f);
        }
        else if (isNeonDlg || isCrtDlg)
        {
            g.setColour(plateFill);
            g.fillRect(bodyPlate);
            g.setColour(PinkXP::sel.withAlpha(isNeonDlg ? 0.80f : 0.72f));
            g.drawRect(bodyPlate, 1);
        }
        else
        {
            g.setColour(plateFill);
            g.fillRect(bodyPlate);
            g.setColour(PinkXP::dark.withAlpha(0.78f));
            g.drawRect(bodyPlate, 1);
            g.setColour(PinkXP::hl.withAlpha(0.55f));
            g.fillRect(bodyPlate.getX() + 1, bodyPlate.getY() + 1, bodyPlate.getWidth() - 2, 1);
        }

        const int seamY = dlg.getBottom() - 40;
        if (isChromeDlg)
        {
            g.setColour(PinkXP::dark.withAlpha(isDarkFace ? 0.72f : 0.48f));
            g.fillRect(dlg.getX() + 8, seamY, dlg.getWidth() - 16, 1);
            g.setColour(PinkXP::hl.interpolatedWith(juce::Colours::white, 0.50f).withAlpha(isDarkFace ? 0.28f : 0.74f));
            g.fillRect(dlg.getX() + 8, seamY + 1, dlg.getWidth() - 16, 1);
        }
        else if (isGlassDlg)
        {
            g.setColour(PinkXP::dark.withAlpha(isDarkFace ? 0.28f : 0.16f));
            g.fillRect(dlg.getX() + 8, seamY, dlg.getWidth() - 16, 1);
            g.setColour(juce::Colours::white.withAlpha(isDarkFace ? 0.16f : 0.65f));
            g.fillRect(dlg.getX() + 8, seamY + 1, dlg.getWidth() - 16, 1);
        }
        else
        {
            g.setColour(PinkXP::sel.withAlpha(isNeonDlg ? 0.72f : 0.65f));
            g.fillRect(dlg.getX() + 8, seamY, dlg.getWidth() - 16, 1);
        }
    }

    // 提示文字
    g.setColour(PinkXP::readableInkOn(PinkXP::face).withAlpha(0.85f));
    g.setFont(PinkXP::getFont(9.0f, juce::Font::plain));
    g.drawText(y2k::trf ("Enter preset number ({0}-{1}):", 1, total_),
               dlg.getX() + 14, dlg.getY() + 28,
               dlg.getWidth() - 28, 18,
               juce::Justification::centredLeft, false);

    // Go 按钮（右侧）
    auto goRect = juce::Rectangle<int>(
        dlg.getRight() - 66, dlg.getBottom() - 34, 54, 22);
    const auto goFill = isCrtDlg ? PinkXP::sel : PinkXP::btnFace;
    PinkXP::Craft::drawControlChrome(g, goRect, goFill,
                                     hasStyledCard ? PinkXP::ControlState::active : PinkXP::ControlState::normal);
    g.setColour(PinkXP::readableInkOn(goFill));
    g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
    g.drawText(y2k::tr ("Go"), goRect, juce::Justification::centred, false);

    // Cancel 按钮
    auto cancelRect = juce::Rectangle<int>(
        goRect.getX() - 62, dlg.getBottom() - 34, 54, 22);
    PinkXP::Craft::drawControlChrome(g, cancelRect, PinkXP::btnFace, PinkXP::ControlState::normal);
    g.setColour(PinkXP::readableInkOn(PinkXP::btnFace));
    g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
    g.drawText(y2k::tr ("Cancel"), cancelRect, juce::Justification::centred, false);
}

void MilkdropModule::PresetJumpDialog::resized()
{
    constexpr int kDlgW = 290;
    constexpr int kDlgH = 130;
    auto dlgX = (getWidth() - kDlgW) / 2;
    auto dlgY = (getHeight() - kDlgH) / 2;

    // TextEditor 位于标题下方
    editor_.setBounds(dlgX + 14, dlgY + 48, kDlgW - 28, 24);
}

void MilkdropModule::PresetJumpDialog::mouseDown(const juce::MouseEvent&)
{
    constexpr int kDlgW = 290;
    constexpr int kDlgH = 130;
    auto dlgX = (getWidth() - kDlgW) / 2;
    auto dlgY = (getHeight() - kDlgH) / 2;

    // Go 按钮区域
    auto goRect = juce::Rectangle<int>(
        dlgX + kDlgW - 66, dlgY + kDlgH - 34, 54, 22);
    if (goRect.contains(getMouseXYRelative())) {
      juce::String input = editor_.getText().trim();
      int val = input.getIntValue();
      if (val < 1) val = 1;
      if (val > total_) val = total_;
      onResult_(val - 1);
      exitModalState(1);
      setVisible(false);
      juce::MessageManager::callAsync([self = this] { delete self; });
      return;
    }

    // Cancel 按钮区域
    auto cancelRect = juce::Rectangle<int>(
        goRect.getX() - 62, dlgY + kDlgH - 34, 54, 22);
    if (cancelRect.contains(getMouseXYRelative())) {
      exitModalState(0);
      setVisible(false);
      juce::MessageManager::callAsync([self = this] { delete self; });
      return;
    }
}

void MilkdropModule::showPresetJumpDialog()
{
    if (glView == nullptr)
        return;

    int total = glView->GetTotalPresetCount();
    if (total <= 0)
        return;

    int current = glView->GetCurrentPresetIndex();
    if (current < 0) current = 0;

    auto* dlg = new PresetJumpDialog(*this, total, current,
        [this](int result) {
            if (result >= 0)
                jumpToPresetIndex(result);
        });
    dlg->setBounds(getLocalBounds());
    addAndMakeVisible(dlg);

    // 隐藏 GLView 避免其 native GL surface 遮盖模态对话框，
    // 并在 modalStateFinished 时恢复可见性。macOS 与 Windows 行为统一：
    //   虽然开启了 setComponentPaintingEnabled(true)，GL 帧作为背景可以让
    //   模态对话框在 CPU paint 层可见，但隐藏 GLView 可以避免 GL 线程
    //   在弹窗期间持续消耗 CPU，同时保证弹窗背景干净。
    //   macOS：GLView 隐藏会触发 detach，GL 上下文销毁后 projectM handle 一同销毁，
    //     恢复可见时重建。由于 CaptureLastFrame 已在 openGLContextClosing 中抓帧，
    //     paintContent 在 GLView 不可用时会显示快照，无黑屏。
    glView->setVisible(false);
    dlg->enterModalState(true, new GlViewRestorer(*glView));
}

void MilkdropModule::showAutoIntervalDialog()
{
    if (glView == nullptr)
        return;

    auto* dlg = new AutoIntervalDialog(*this, autoIntervalSeconds_,
        [this](float result) {
            applyAutoInterval(result);
        });
    dlg->setBounds(getLocalBounds());
    addAndMakeVisible(dlg);

    // macOS 与 Windows 统一：隐藏 GLView 以保证弹窗背景干净，
    // modalStateFinished 时自动恢复可见性。
    glView->setVisible(false);
    dlg->enterModalState(true, new GlViewRestorer(*glView));
}

#if JUCE_MAC
// ==========================================================
// OverlayView / UpdateOverlayViewPlacement —— macOS 顶层控制栏
// ==========================================================

void MilkdropModule::OverlayView::paint(juce::Graphics& g)
{
    if (!owner_.focused_ || owner_.glView == nullptr)
        return;
    if (PinkXP::isTransparentBackdrop() && ! PinkXP::isChromaRevealed (owner_))
        return;
    auto localBounds = getLocalBounds();
    if (localBounds.isEmpty())
        return;

    // OverlayView 是独立顶层 NSWindow，其本地坐标 (0,0) 对应控制栏左上角。
    // 但 paintOverlayControlBar 内部会把绘制中计算出的 nameArea/按钮矩形
    // 缓存到 owner_.cachedNameArea_，该缓存被 hit-test（MilkdropModule::mouseDown
    // + hitTestOverlayButton）使用，而 hit-test 输入的是 MilkdropModule 坐标系。
    // 若此处以 OverlayView 本地坐标 (0,0,W,26) 调用 paintOverlayControlBar，
    // 会用 OverlayView 坐标覆盖 cachedNameArea_，导致标题区 hit-test 永远失败，
    // 无法弹出 PresetJumpDialog。
    //
    // 解法：先将本地坐标转换回 MilkdropModule 内容区坐标（moduleTopBar），
    // 再对 g 应用相反方向的平移，使得视觉输出仍位于本窗口 (0,0)，
    // 但传给 paintOverlayControlBar 的 content 参数已是正确的模块坐标系。
    auto moduleTopBar = owner_.getContentBounds().withHeight(26);
    if (moduleTopBar.isEmpty())
        return;

    juce::Graphics::ScopedSaveState save(g);
    g.addTransform(juce::AffineTransform::translation(
        static_cast<float>(-moduleTopBar.getX()),
        static_cast<float>(-moduleTopBar.getY())));

    owner_.paintOverlayControlBar(g, moduleTopBar);
    owner_.paintLibraryButtons(g, owner_.getContentBounds());
    if (owner_.isAutoRowExpanded_)
        owner_.paintAutoControlRow(g, moduleTopBar);
    if (owner_.isColorPanelOpen_)
        owner_.paintColorPanel(g, moduleTopBar);
    if (owner_.isEffectsPanelOpen_)
        owner_.paintEffectsPanel(g, moduleTopBar);
    if (owner_.isWavePanelOpen_)
        owner_.paintWavePanel(g, moduleTopBar);
    if (owner_.isTweakPanelOpen_)
        owner_.paintTweakPanel(g, moduleTopBar);
}

juce::String MilkdropModule::OverlayView::getTooltip()
{
    if (! owner_.focused_ || ! owner_.isShowing() || owner_.glView == nullptr)
        return {};

    // 鼠标位置换算回 MilkdropModule 本地坐标：控制栏命中测试与绘制共用同一套
    //   坐标（见 paint() 里的坐标平移说明）。
    const auto pos = owner_.getLocalPoint (nullptr, juce::Desktop::getMousePosition());
    const auto info = owner_.getHoverHelp (pos);

    // 内容区（整块视频）说明会盖住画面；本窗口没有 TooltipWindow 的静止判定，
    //   因此只提供按钮 / 展开面板等明确控件的文案。
    return info.contentZone ? juce::String() : info.text;
}

bool MilkdropModule::usesNativeOverlayTips() const
{
    // 控制栏窗口覆盖整个内容区：Editor 的提示窗口在它上面会被遮挡，且 JUCE 的
    //   提示定时器拿不到鼠标事件（不会刷新）。交给 updateOverlayHoverTip 轮询。
    return overlayView_ != nullptr && overlayView_->isOnDesktop() && overlayView_->isVisible();
}

void MilkdropModule::hideOverlayHoverTip()
{
    if (overlayTooltip_ != nullptr && overlayTooltip_->isVisible())
        overlayTooltip_->hideTipNow();
}

void MilkdropModule::updateOverlayHoverTip()
{
    if (overlayTooltip_ == nullptr || overlayView_ == nullptr
        || ! overlayView_->isOnDesktop() || ! overlayView_->isVisible()
        || ! isShowing())
    {
        tipPollState = 0;
        hideOverlayHoverTip();
        return;
    }

    const auto mouse = juce::Desktop::getMousePosition();

    // 只接管控制栏窗口覆盖的区域（=整个内容区）；其余位置由 Editor 的提示窗口
    //   正常处理，避免两边各弹一个。
    if (! overlayView_->getScreenBounds().contains (mouse))
    {
        tipPollState = 0;
        hideOverlayHoverTip();
        return;
    }

    const auto now = juce::Time::getMillisecondCounter();

    if (mouse != tipPollMousePos)
    {
        tipPollMousePos = mouse;
        tipPollStillMs  = now;
        tipPollState    = 0;   // 移动即收起，重新计时
        hideOverlayHoverTip();
        return;
    }

    // 0 = 未决；1 = 已显示提示；2 = 此处无提示。已决时不再解析（省 30Hz 文案开销）
    if (tipPollState != 0)
        return;

    // 与普通提示同样的静止延迟：移动途中不弹，停住 500ms 才出现
    constexpr juce::uint32 kStillMs = 500;
    if (now - tipPollStillMs < kStillMs)
        return;

    const auto info = getHoverHelp (getLocalPoint (nullptr, mouse));

    // 内容区说明会盖住画面；控制栏条带内没命中按钮时 getHoverHelp 已返回空文案。
    if (info.text.isEmpty() || info.contentZone)
    {
        tipPollState = 2;
        return;
    }

    tipPollState = 1;
    overlayTooltip_->showTip (mouse, info.text);
}

void MilkdropModule::UpdateOverlayViewPlacement()
{
    if (overlayView_ == nullptr)
        return;

    // 判定条件：模块聚焦 + MilkdropModule 已在屏（showing）+ 内容区非空
    const bool wants_visible = focused_ && isShowing() && glView != nullptr;

    if (!wants_visible)
    {
        if (overlayView_->isOnDesktop())
            overlayView_->removeFromDesktop();
        overlayView_->setVisible(false);
        return;
    }

    // 计算控制栏在屏幕坐标下的矩形。
    // - 顶部第一行（overlay control bar）：内容区顶部 26px。
    // - 若 auto 时间控制条展开，额外向下扩展 kAutoRowHeight 供第二行绘制。
    auto contentLocal = GetContentLocalBounds();
    if (contentLocal.isEmpty())
    {
        if (overlayView_->isOnDesktop())
            overlayView_->removeFromDesktop();
        overlayView_->setVisible(false);
        return;
    }

    int barH = 26;
    if (isAutoRowExpanded_)
        barH += static_cast<int>(kAutoRowHeight);

    // 展开的二级面板（color/effects/wave/tweak）绘制在控制栏正下方，
    // 必须把对应面板高度一并计入 overlay NSWindow 的高度，否则面板内容
    // 会绘制在 overlayView 的 bounds 之外被裁剪（macOS 专属问题）。
    // 这些面板彼此互斥（toggle 时互相关闭），同一时刻至多展开一个；
    // 这里按实际展开状态累加，保持与 OverlayView::paint 的绘制逻辑一致。
    auto moduleTopBar = contentLocal.withHeight(26);
    if (isColorPanelOpen_)
        barH += getColorPanelBounds(moduleTopBar).getHeight();
    if (isEffectsPanelOpen_)
        barH += getEffectsPanelBounds(moduleTopBar).getHeight();
    if (isWavePanelOpen_)
        barH += getWavePanelBounds(moduleTopBar).getHeight();
    if (isTweakPanelOpen_)
        barH += getTweakPanelBounds(moduleTopBar).getHeight();

    barH = juce::jmin(barH, contentLocal.getHeight());

    // 右下角收藏 / 切换按钮位于内容区底部，不在顶部控制栏区域内。
    // 为让其可见，overlayView 必须覆盖整个内容区（而非仅顶部 barH 高度），
    // 否则按钮绘制会被 overlayView 的 bounds 裁剪。OverlayView::paint 内部
    // 已按内容区坐标完整绘制顶部控制栏与右下角按钮，此处直接采用完整内容区。
    auto barLocal = contentLocal;
    auto barScreen = localAreaToGlobal(barLocal);
    if (barScreen.isEmpty())
    {
        if (overlayView_->isOnDesktop())
            overlayView_->removeFromDesktop();
        overlayView_->setVisible(false);
        return;
    }

    // 首次显示时创建独立顶层原生窗口并绑定为父窗口的子窗口。
    // windowIsTemporary：短暂弹出型窗口（无边框、无标题栏、不出现在 Dock）
    // windowIgnoresKeyPresses：不接管键盘输入
    // windowIgnoresMouseClicks：整窗鼠标点击透传给下方（配合
    //   setInterceptsMouseClicks(false, false) 双保险）
    //
    // 子窗口（addChildWindow:ordered:NSWindowAbove）vs setAlwaysOnTop：
    //   · setAlwaysOnTop → NSFloatingWindowLevel (3)，高于所有普通窗口，
    //     会盖住系统其他应用的窗口。
    //   · 子窗口 → 跟随父窗口的 NSNormalWindowLevel (0)，永悬浮于父
    //     窗口上方，但父窗口被其他应用盖住时也跟着被盖。
    if (!overlayView_->isOnDesktop())
    {
        overlayView_->addToDesktop(
            juce::ComponentPeer::windowIsTemporary
            | juce::ComponentPeer::windowIgnoresKeyPresses
            | juce::ComponentPeer::windowIgnoresMouseClicks);
        // 将 overlay NSWindow 绑定为父窗口（editor）的子窗口，
        // Z-order 高于主窗口内的 GL NSOpenGLView，但不高于其他应用。
        y2k::ui::MacAttachOverlayToParent(overlayView_.get(), this);

        // 控制栏窗口自己的悬停提示宿主（独立 peer，Editor 的提示窗口管不到）。
        overlayTooltip_ = std::make_unique<OverlayTipWindow> (overlayView_.get());
    }

    overlayView_->setBounds(barScreen);
    // setVisible(true) 在 JUCE macOS 上会触发 NSWindow orderFront:，
    // 若每帧无脑调用则覆盖同层级的 UpdateDialog。
    // 仅当 overlay 当前不可见时才调用，避免抢占弹窗 z-order。
    if (!overlayView_->isVisible())
        overlayView_->setVisible(true);
}
#endif  // JUCE_MAC

// ==========================================================
// Auto 轮播模式
// ==========================================================

void MilkdropModule::toggleAutoMode()
{
  notifyBeforeUserEdit();
  if (!isAutoMode_)
  {
    // 开启 auto：进入自动轮播 + 展开时间控制条，收起其它面板。
    isAutoMode_ = true;
    isAutoRowExpanded_ = true;
    // Standalone（无宿主 playhead）下 BPM 同步模式不可用，回退到时间模式。
    if (autoModeIsBeat_ && !isHostPlayheadAvailable())
        autoModeIsBeat_ = false;
    if (isColorPanelOpen_)    isColorPanelOpen_ = false;
    if (isEffectsPanelOpen_)  isEffectsPanelOpen_ = false;
    if (isWavePanelOpen_)     isWavePanelOpen_ = false;
    if (isTweakPanelOpen_)    isTweakPanelOpen_ = false;
    lastAutoSwitchTime_ = juce::Time::getMillisecondCounter();
    beatBucketInitialized_ = false;  // 开启 auto 时重置拍桶基准，从当前拍位重新对齐
  }
  else if (!isAutoRowExpanded_)
  {
    // auto 已开启但控制条被其它面板收起：重新展开控制条并收起其它面板，
    // 不关闭 auto 轮播开关（互斥只影响展开面板，不影响 auto 模式）。
    isAutoRowExpanded_ = true;
    if (isColorPanelOpen_)    isColorPanelOpen_ = false;
    if (isEffectsPanelOpen_)  isEffectsPanelOpen_ = false;
    if (isWavePanelOpen_)     isWavePanelOpen_ = false;
    if (isTweakPanelOpen_)    isTweakPanelOpen_ = false;
  }
  else
  {
    // auto 已开启且控制条展开：关闭 auto + 收起控制条。
    isAutoMode_ = false;
    isAutoRowExpanded_ = false;
  }
  // 重新布局并重绘
  layoutContent(getContentBounds());
  repaint();
  glView->repaint();
  // UI 修改 auto 状态 → 回写宿主参数，避免 10Hz pull 反向覆盖。
  if (editor_ != nullptr)
    editor_->pushMilkdropAutomationParams();
  notifyAfterUserEdit (y2k::tr ("MilkDrop auto"), "auto");
}

void MilkdropModule::setAutoModeEnabled(bool enabled)
{
  if (enabled)
  {
    // 开启 auto：进入自动轮播 + 展开时间控制条，收起其它面板。
    // 若开关已开启（仅控制条被其它面板收起），不重置轮播计时器。
    if (!isAutoMode_)
    {
      isAutoMode_ = true;
      // Standalone（无宿主 playhead）下 BPM 同步模式不可用，回退到时间模式。
      if (autoModeIsBeat_ && !isHostPlayheadAvailable())
          autoModeIsBeat_ = false;
      lastAutoSwitchTime_ = juce::Time::getMillisecondCounter();
      beatBucketInitialized_ = false;  // 开启 auto 时重置拍桶基准
    }
    isAutoRowExpanded_ = true;
    if (isColorPanelOpen_)    isColorPanelOpen_ = false;
    if (isEffectsPanelOpen_)  isEffectsPanelOpen_ = false;
    if (isWavePanelOpen_)     isWavePanelOpen_ = false;
    if (isTweakPanelOpen_)    isTweakPanelOpen_ = false;
  }
  else
  {
    // 关闭 auto：关闭轮播开关 + 收起控制条。
    isAutoMode_ = false;
    isAutoRowExpanded_ = false;
  }
  layoutContent(getContentBounds());
  repaint();
  glView->repaint();
}

void MilkdropModule::checkAutoMode()
{
  if (!isAutoMode_)
    return;

  if (autoModeIsBeat_)
  {
    // ----------------------------------------------------------------
    // BPM 同步模式：对齐宿主拍网格，每 N 拍切换一次。
    //   · 无播放信息 / 宿主未播放 / BPM 未知 / 拍数非法 → 不切换；
    //   · 用 floor(ppq / N) 作为"拍桶"，跨过桶边界即切换，切换点对齐
    //     到 ppq 的 N 整数倍（即宿主拍网格）；
    //   · 播放跳回（seek / loop）时重置基准，不触发误切换。
    // ----------------------------------------------------------------
    if (editor_ == nullptr)
      return;

    const auto snap = editor_->getHostPlayheadSnapshot();
    if (! snap.valid || ! snap.isPlaying)
      return;
    if (snap.bpm <= 0.0 || autoBeats_ <= 0)
      return;

    const double bucket = std::floor (snap.ppqPosition / static_cast<double> (autoBeats_));
    if (! beatBucketInitialized_)
    {
      lastBeatBucket_ = bucket;
      beatBucketInitialized_ = true;
      return;
    }

    if (bucket > lastBeatBucket_)
    {
      lastBeatBucket_ = bucket;
      // 后台自动轮播不进入 Ctrl+Z 撤回栈（避免淹没用户操作记录）
      pendingPresetTarget_ = -1;
      restored_preset_index_ = -1;
      if (glView != nullptr)
        glView->RequestPresetRandom();
    }
    else if (bucket < lastBeatBucket_)
    {
      // 播放位置回退（宿主 seek / loop），重置基准，不触发切换。
      lastBeatBucket_ = bucket;
    }
    return;
  }

  // 时间模式（原有逻辑）
  juce::uint32 now = juce::Time::getMillisecondCounter();
  juce::uint32 intervalMs = static_cast<juce::uint32>(autoIntervalSeconds_ * 1000.0f);
  if (now - lastAutoSwitchTime_ >= intervalMs)
  {
    lastAutoSwitchTime_ = now;
    pendingPresetTarget_ = -1;
    restored_preset_index_ = -1;
    if (glView != nullptr)
      glView->RequestPresetRandom();
  }
}

void MilkdropModule::applyAutoInterval(float seconds)
{
  notifyBeforeUserEdit();
  seconds = juce::jlimit(kMinAutoInterval, kMaxAutoInterval, seconds);
  // 四舍五入到 0.001
  seconds = std::round(seconds * 1000.0f) / 1000.0f;
  autoIntervalSeconds_ = seconds;
  // 用户确认间隔时始终重置计时器，从此刻起算经过完整间隔后执行第一次切换
  lastAutoSwitchTime_ = juce::Time::getMillisecondCounter();
  repaint();
  glView->repaint();
  // UI 修改 auto 间隔 → 回写宿主参数，避免 10Hz pull 反向覆盖。
  if (editor_ != nullptr)
    editor_->pushMilkdropAutomationParams();
  notifyAfterUserEdit (y2k::tr ("MilkDrop auto"), "auto_row");
}

void MilkdropModule::updateAutoIntervalFromSlider(float proportion)
{
  proportion = juce::jlimit(0.0f, 1.0f, proportion);
  float seconds = kMinAutoInterval
                  + proportion * (kMaxAutoInterval - kMinAutoInterval);
  seconds = juce::jlimit(kMinAutoInterval, kMaxAutoInterval, seconds);
  // 四舍五入到 0.001
  seconds = std::round(seconds * 1000.0f) / 1000.0f;

  if (seconds != autoIntervalSeconds_)
  {
    autoIntervalSeconds_ = seconds;
    // 不重置计时器：用户拖动期间不触发自动切换
    // 拖动过程中实时回写宿主参数，避免 10Hz pull 用旧参数把值覆盖回去。
    if (editor_ != nullptr)
      editor_->pushMilkdropAutomationParams();
  }
}

void MilkdropModule::updateAutoBeatsFromSlider(float proportion)
{
  proportion = juce::jlimit(0.0f, 1.0f, proportion);
  int beats = kMinAutoBeats
              + juce::roundToInt(proportion * (kMaxAutoBeats - kMinAutoBeats));
  beats = juce::jlimit(kMinAutoBeats, kMaxAutoBeats, beats);

  if (beats != autoBeats_)
  {
    autoBeats_ = beats;
    // 拍数变化后重置拍桶基准，避免用旧基准误触发切换。
    beatBucketInitialized_ = false;
    // 拖动过程中实时回写宿主参数，避免 10Hz pull 用旧参数把值覆盖回去。
    if (editor_ != nullptr)
      editor_->pushMilkdropAutomationParams();
  }
}

void MilkdropModule::toggleAutoModeType()
{
  // 仅插件宿主模式可切换到 BPM 同步模式（Standalone 无 playhead）。
  if (! isHostPlayheadAvailable())
    return;

  notifyBeforeUserEdit();
  autoModeIsBeat_ = !autoModeIsBeat_;
  // 切换模式后重置拍桶基准，避免用旧基准误触发切换。
  beatBucketInitialized_ = false;
  lastBeatBucket_ = 0.0;

  repaint();
  glView->repaint();
  // UI 修改 auto 模式类型 → 回写宿主参数。
  if (editor_ != nullptr)
    editor_->pushMilkdropAutomationParams();
  notifyAfterUserEdit (y2k::tr ("MilkDrop auto"), "auto_row");
}

void MilkdropModule::setAutoBeatMode(bool beatMode)
{
  if (autoModeIsBeat_ == beatMode)
    return;
  autoModeIsBeat_ = beatMode;
  beatBucketInitialized_ = false;
  lastBeatBucket_ = 0.0;
  repaint();
  glView->repaint();
}

void MilkdropModule::setAutoBeats(int beats)
{
  beats = juce::jlimit(kMinAutoBeats, kMaxAutoBeats, beats);
  if (autoBeats_ == beats)
    return;
  autoBeats_ = beats;
  beatBucketInitialized_ = false;
  repaint();
  glView->repaint();
}

juce::Rectangle<int> MilkdropModule::getAutoRowBounds(juce::Rectangle<int> topBar) const
{
  return juce::Rectangle<int>(topBar.getX(), topBar.getBottom(),
                              topBar.getWidth(), kAutoRowHeight);
}

bool MilkdropModule::isHostPlayheadAvailable() const
{
  // 仅插件宿主模式（VST3/AU 等）能拿到宿主播放位置；Standalone 无 playhead，
  // 按用户需求隐藏 BPM 模式入口，只保留时间模式。
  return editor_ != nullptr && editor_->isPluginHostMode();
}

juce::Rectangle<int> MilkdropModule::getAutoModeBtnBounds(juce::Rectangle<int> autoRow) const
{
  // Standalone（无宿主 playhead）返回空矩形 → 不绘制模式按钮，隐藏 BPM 入口。
  if (! isHostPlayheadAvailable())
    return {};

  constexpr int kModeBtnW = 42;
  constexpr int kModeBtnH = 20;
  return juce::Rectangle<int>(autoRow.getX() + 6,
                              autoRow.getY() + (autoRow.getHeight() - kModeBtnH) / 2,
                              kModeBtnW, kModeBtnH);
}

juce::Rectangle<int> MilkdropModule::getSliderBounds(juce::Rectangle<int> autoRow) const
{
  constexpr int kSliderPadR = 44;
  constexpr int kSliderH = 8;
  int sliderX;
  if (isHostPlayheadAvailable())
    sliderX = autoRow.getX() + 6 + 42 + 6;  // 模式按钮(42px) + gap
  else
    sliderX = autoRow.getX() + 6 + 38 + 4;  // "Auto:" 标签(38px) + gap
  int sliderW = autoRow.getWidth() - sliderX - kSliderPadR;
  return juce::Rectangle<int>(sliderX,
                              autoRow.getY() + (autoRow.getHeight() - kSliderH) / 2,
                              juce::jmax(20, sliderW), kSliderH);
}

void MilkdropModule::paintAutoControlRow(juce::Graphics& g, juce::Rectangle<int> topBar)
{
  auto autoRow = getAutoRowBounds(topBar);
  const auto& tokens = PinkXP::getSkinTokens();

  // 风格化半透明子面板容器
  drawMilkdropOverlaySurface (g, autoRow, false);

  const auto mousePos = getMouseXYRelative();

  // ---- 左侧：模式切换按钮（sec / beat）或 "Auto:" 标签 ----
  auto modeBtn = getAutoModeBtnBounds(autoRow);
  cachedAutoModeBtnRect_ = modeBtn;  // 供 hit-test（Standalone 下为空矩形）
  if (! modeBtn.isEmpty())
  {
    // 插件宿主模式：绘制可点击的模式切换按钮（时间 ↔ BPM）
    const bool hovered = modeBtn.contains (mousePos);
    const auto fg = drawMilkdropOverlayButton (g, modeBtn, autoModeIsBeat_, hovered, false);
    g.setColour (fg);
    g.setFont (PinkXP::getFont (9.0f, juce::Font::bold));
    g.drawText (y2k::tr (autoModeIsBeat_ ? "beat" : "sec"), modeBtn, juce::Justification::centred, false);
  }
  else
  {
    // Standalone（无宿主 playhead）：只保留时间模式标签，隐藏 BPM 入口
    g.setColour (PinkXP::pink300.withAlpha (0.95f));
    g.setFont (PinkXP::getFont (9.0f, juce::Font::bold));
    g.drawText (y2k::tr ("Auto:"), autoRow.getX() + 6, autoRow.getY(),
                38, autoRow.getHeight(), juce::Justification::centredLeft, false);
  }

  // ---- Slider 轨道与滑块（经由 drawMilkdropOverlaySlider 跟随当前风格）----
  auto sliderBounds = getSliderBounds(autoRow);
  const float proportion = autoModeIsBeat_
      ? static_cast<float> (autoBeats_ - kMinAutoBeats)
        / static_cast<float> (kMaxAutoBeats - kMinAutoBeats)
      : static_cast<float> (autoIntervalSeconds_ - kMinAutoInterval)
        / static_cast<float> (kMaxAutoInterval - kMinAutoInterval);

  drawMilkdropOverlaySlider (g, sliderBounds, proportion, PinkXP::pink300, isDraggingSlider_);

  // ---- 右侧数值标签 ----
  //   时间模式：如 "10.000s" / "1m30.000s"（可点击弹输入框）；
  //   BPM 模式：如 "4 beat"（仅滑块调节，标签不可点击）。
  juce::String valueLabel;
  if (autoModeIsBeat_)
  {
    valueLabel = juce::String (autoBeats_) + " beat";
  }
  else if (autoIntervalSeconds_ >= 60.0f)
  {
    int mins = static_cast<int>(autoIntervalSeconds_) / 60;
    float secs = std::fmod(autoIntervalSeconds_, 60.0f);
    valueLabel = juce::String(mins) + "m";
    if (secs > 0.05f)
      valueLabel += juce::String(secs, 3) + "s";
  }
  else
  {
    valueLabel = juce::String(autoIntervalSeconds_, 3) + "s";
  }

  auto valueLabelRect = juce::Rectangle<int>(
      sliderBounds.getRight() + 4, autoRow.getY(),
      40, autoRow.getHeight());
  cachedAutoTimeLabel_ = valueLabelRect;  // 供 mouseDown/mouseMove hit-test

  // 时间模式下标签可点击 → hover 微亮底色提示可点击（遵循 SkinTokens 圆角）
  const bool labelClickable = ! autoModeIsBeat_;
  bool labelHovered = labelClickable && valueLabelRect.contains (mousePos);
  if (labelHovered)
  {
    g.setColour(juce::Colour(0xFF, 0xFF, 0xFF).withAlpha(0.10f));
    const auto hoverR = valueLabelRect.reduced(2, 4);
    if (! tokens.pixelSnapped && tokens.controlRadius > 0.5f)
      g.fillRoundedRectangle(hoverR.toFloat(), juce::jmin(tokens.controlRadius, 3.0f));
    else
      g.fillRect(hoverR);
  }

  g.setColour(PinkXP::pink300.withAlpha(labelHovered ? 1.0f : 0.9f));
  g.setFont(PinkXP::getFont(8.0f, juce::Font::plain));
  g.drawText(y2k::tr (valueLabel), valueLabelRect, juce::Justification::centredLeft, false);
}

// ==========================================================
// 整体染色控制器（master output colors）
// ==========================================================
void MilkdropModule::toggleColorPanel()
{
  isColorPanelOpen_ = !isColorPanelOpen_;
  if (isColorPanelOpen_)
  {
    // 与 auto 时间控制条 / 效果 / 波形 / 偏移面板互斥，避免控制器重叠遮挡。
    // 只收起 auto 控制条，不关闭 auto 轮播开关。
    if (isAutoRowExpanded_)
      isAutoRowExpanded_ = false;
    if (isEffectsPanelOpen_)
      isEffectsPanelOpen_ = false;
    if (isWavePanelOpen_)
      isWavePanelOpen_ = false;
    if (isTweakPanelOpen_)
      isTweakPanelOpen_ = false;
    // 打开时从 Editor 读回当前全局视觉状态，保证 UI 显示与渲染一致。
    syncVisualFromEditor();
  }

  layoutContent(getContentBounds());
  repaint();
  glView->repaint();
}

void MilkdropModule::toggleEffectsPanel()
{
  isEffectsPanelOpen_ = !isEffectsPanelOpen_;
  if (isEffectsPanelOpen_)
  {
    // 与 auto 时间控制条 / 染色 / 波形 / 偏移面板互斥，避免控制器重叠遮挡。
    // 只收起 auto 控制条，不关闭 auto 轮播开关。
    if (isAutoRowExpanded_)
      isAutoRowExpanded_ = false;
    if (isColorPanelOpen_)
      isColorPanelOpen_ = false;
    if (isWavePanelOpen_)
      isWavePanelOpen_ = false;
    if (isTweakPanelOpen_)
      isTweakPanelOpen_ = false;
    syncVisualFromEditor();
  }

  layoutContent(getContentBounds());
  repaint();
  glView->repaint();
}

void MilkdropModule::toggleWavePanel()
{
  isWavePanelOpen_ = !isWavePanelOpen_;
  if (isWavePanelOpen_)
  {
    // 与 auto 时间控制条 / 染色 / 效果 / 偏移面板互斥，避免控制器重叠遮挡。
    // 只收起 auto 控制条，不关闭 auto 轮播开关。
    if (isAutoRowExpanded_)
      isAutoRowExpanded_ = false;
    if (isColorPanelOpen_)
      isColorPanelOpen_ = false;
    if (isEffectsPanelOpen_)
      isEffectsPanelOpen_ = false;
    if (isTweakPanelOpen_)
      isTweakPanelOpen_ = false;
    // 打开时从 Editor 读回当前全局 wave 状态，保证 UI 显示与渲染一致。
    syncWaveFromEditor();
  }

  layoutContent(getContentBounds());
  repaint();
  glView->repaint();
}

void MilkdropModule::toggleTweakPanel()
{
  isTweakPanelOpen_ = !isTweakPanelOpen_;
  if (isTweakPanelOpen_)
  {
    // 与 auto 时间控制条 / 染色 / 效果 / 波形面板互斥，避免控制器重叠遮挡。
    // 只收起 auto 控制条，不关闭 auto 轮播开关。
    if (isAutoRowExpanded_)
      isAutoRowExpanded_ = false;
    if (isColorPanelOpen_)
      isColorPanelOpen_ = false;
    if (isEffectsPanelOpen_)
      isEffectsPanelOpen_ = false;
    if (isWavePanelOpen_)
      isWavePanelOpen_ = false;
    // 打开时从 Editor 读回当前全局视觉状态（含 tweak offset），保证 UI 与渲染一致。
    syncVisualFromEditor();
  }

  layoutContent(getContentBounds());
  repaint();
  glView->repaint();
}

void MilkdropModule::syncWaveFromEditor()
{
  if (editor_ != nullptr)
    waveState_ = editor_->GetMilkdropWaveState();
}

void MilkdropModule::applyWaveToEditor()
{
  if (editor_ != nullptr)
    editor_->SetMilkdropWaveState(waveState_);
}

void MilkdropModule::syncVisualFromEditor()
{
  if (editor_ != nullptr)
    visualState_ = editor_->GetMilkdropVisualState();
}

void MilkdropModule::applyVisualToEditor()
{
  if (editor_ != nullptr)
    editor_->SetMilkdropVisualState(visualState_);
}

void MilkdropModule::refreshStateFromEditor()
{
  // 宿主自动化 pull 修改 Editor 全局状态后调用：把 Editor 的 visual/wave
  // 状态读回本地缓存并重绘，让二级面板（染色/波形）控件实时反映新值。
  syncVisualFromEditor();
  syncWaveFromEditor();
  repaint();
  if (glView != nullptr)
    glView->repaint();
}

// ==========================================================
// Bright 滑块非线性映射
//
// 需求：brightness=1.0（中性点）位于滑块正中间（proportion=0.5），
// 左端 brightness=0.0，右端 brightness=8.0，且靠近两端时变化率放缓，
// 便于精细调节暗部（<1.0）与强发光（>1.0）区域。
//
// 采用分段二次曲线（ease-in / ease-out）：
//   · 左半段 [0, 0.5] → [0, 1]：brightness = t^2，t = 2p。
//     在 brightness→0 处导数趋近 0，暗部调节更细腻。
//   · 右半段 [0.5, 1] → [1, 8]：brightness = 1 + 7*(1 - (1-t)^2)，t = 2p-1。
//     在 brightness→8 处导数趋近 0，强发光调节更细腻。
// ==========================================================
float MilkdropModule::SliderProportionToBrightness(float proportion)
{
  proportion = juce::jlimit(0.0f, 1.0f, proportion);

  if (proportion <= 0.5f)
  {
    const float t = proportion * 2.0f;  // 0~1
    return t * t;                        // 0~1，中点 t=1 → 1.0
  }

  const float t = (proportion - 0.5f) * 2.0f;  // 0~1
  const float one_minus_t = 1.0f - t;
  // brightness = 1 + (kBrightMax-1) * (1 - (1-t)^2)
  return 1.0f + (kBrightMax - 1.0f) * (1.0f - one_minus_t * one_minus_t);
}

float MilkdropModule::BrightnessToSliderProportion(float brightness)
{
  brightness = juce::jlimit(kBrightMin, kBrightMax, brightness);

  if (brightness <= 1.0f)
  {
    // brightness = t^2 → t = sqrt(brightness)，比例 = t / 2
    return std::sqrt(brightness) * 0.5f;  // 0~0.5
  }

  // brightness = 1 + (kBrightMax-1)*(1 - (1-t)^2)
  // (1-t)^2 = 1 - (brightness-1)/(kBrightMax-1)
  // t = 1 - sqrt(1 - (brightness-1)/(kBrightMax-1))
  const float ratio = (brightness - 1.0f) / (kBrightMax - 1.0f);
  const float t = 1.0f - std::sqrt(1.0f - ratio);
  return 0.5f + t * 0.5f;
}

void MilkdropModule::updateTintFromSlider(int row, float proportion)
{
  proportion = juce::jlimit(0.0f, 1.0f, proportion);

  float value = 0.0f;
  if (row == 3)
  {
    // Bright 行：非线性映射，中点 proportion=0.5 对应 brightness=1.0。
    value = SliderProportionToBrightness(proportion);
  }
  else
  {
    // RGB 行：线性加性偏移范围 0~2。
    value = kTintMin + proportion * (kTintMax - kTintMin);
    value = juce::jlimit(kTintMin, kTintMax, value);
  }
  value = std::round(value * 100.0f) / 100.0f;  // 1% 精度

  switch (row)
  {
    case 0: visualState_.tint_r = value; break;
    case 1: visualState_.tint_g = value; break;
    case 2: visualState_.tint_b = value; break;
    case 3: visualState_.brightness = value; break;
    default: break;
  }
  applyVisualToEditor();
}

void MilkdropModule::toggleEffectSwitch(int row)
{
  const MilkdropEffectDef* def = GetImplementedMilkdropEffect(row);
  if (def == nullptr)
    return;
  notifyBeforeUserEdit();
  def->set(visualState_, !def->get(visualState_));
  applyVisualToEditor();
  notifyAfterUserEdit (y2k::tr ("MilkDrop effects"), "effects");
}

juce::Rectangle<int> MilkdropModule::getColorPanelBounds(juce::Rectangle<int> topBar) const
{
  return juce::Rectangle<int>(topBar.getX(), topBar.getBottom(),
                              topBar.getWidth(),
                              static_cast<int>(kColorPanelHeight));
}

juce::Rectangle<int> MilkdropModule::getTintSliderBounds(juce::Rectangle<int> panel, int row) const
{
  constexpr int kPad     = 6;
  constexpr int kLabelW  = 22;
  constexpr int kValueW  = 48;
  constexpr int kHeaderH = 22;
  constexpr int kRowH    = 20;
  constexpr int kSliderH = 8;

  int sliderX = panel.getX() + kPad + kLabelW + 4;
  int sliderW = panel.getWidth() - kPad - kLabelW - 4 - kValueW - kPad;
  int y = panel.getY() + kHeaderH + row * kRowH + (kRowH - kSliderH) / 2;

  return juce::Rectangle<int>(sliderX, y, juce::jmax(20, sliderW), kSliderH);
}

juce::Rectangle<int> MilkdropModule::getTintResetBounds(juce::Rectangle<int> panel) const
{
  constexpr int kPad = 6;
  return juce::Rectangle<int>(panel.getRight() - kPad - 44, panel.getY() + 2, 44, 18);
}

void MilkdropModule::paintColorPanel(juce::Graphics& g, juce::Rectangle<int> topBar)
{
  auto panel = getColorPanelBounds(topBar);
  cachedColorPanelRect_ = panel;

  // 风格化半透明子面板容器
  drawMilkdropOverlaySurface (g, panel, false);

  // Reset 按钮（先算位置，标题宽度自适应到按钮左侧，避免 VT323/Orbitron 截断）
  auto resetRect = getTintResetBounds(panel);
  cachedTintResetRect_ = resetRect;

  // 标题 "COLOR"
  g.setColour(PinkXP::pink300.withAlpha(0.95f));
  g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
  g.drawFittedText(y2k::tr ("COLOR"),
                   juce::Rectangle<int>(panel.getX() + 6, panel.getY(),
                                        juce::jmax(60, panel.getWidth() - resetRect.getWidth() - 16), 22),
                   juce::Justification::centredLeft, 1, 0.78f);

  const bool resetHovered = resetRect.contains(getMouseXYRelative());
  g.setColour (drawMilkdropOverlayButton (g, resetRect, false, resetHovered, false));
  g.setFont(PinkXP::getFont(8.0f, juce::Font::bold));
  g.drawFittedText(y2k::tr ("Reset"), resetRect.reduced(2, 0), juce::Justification::centred, 1, 0.72f);

  // RGB + Bright 四行滑块
  static const char* kLabels[4] = { "R", "G", "B", "Bri" };
  static const juce::Colour kRowColours[4] = {
      juce::Colour(0xFF, 0x6B, 0x6B),  // R
      juce::Colour(0x6B, 0xFF, 0x6B),  // G
      juce::Colour(0x6B, 0x9B, 0xFF),  // B
      juce::Colour(0xE8, 0xE8, 0xE8),  // Bright（中性白）
  };

  for (int row = 0; row < 4; ++row)
  {
    float value = (row == 0) ? visualState_.tint_r
                : (row == 1) ? visualState_.tint_g
                : (row == 2) ? visualState_.tint_b
                             : visualState_.brightness;

    float proportion = 0.0f;
    if (row == 3)
    {
      // Bright 行：非线性映射（中点 brightness=1.0 对应 proportion=0.5）。
      proportion = BrightnessToSliderProportion(value);
    }
    else
    {
      proportion = (value - kTintMin) / (kTintMax - kTintMin);
      proportion = juce::jlimit(0.0f, 1.0f, proportion);
    }

    auto sliderBounds = getTintSliderBounds(panel, row);
    int labelY = panel.getY() + 22 + row * 20;
    auto labelRect = juce::Rectangle<int>(panel.getX() + 6, labelY, 22, 20);

    g.setColour(kRowColours[row]);
    g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
    g.drawText(y2k::tr (kLabels[row]), labelRect, juce::Justification::centredLeft, false);

    drawMilkdropOverlaySlider (g, sliderBounds, proportion, kRowColours[row], draggingTintRow_ == row);

    // 右侧数值（百分比）
    auto valueRect = juce::Rectangle<int>(sliderBounds.getRight() + 4, labelY, 48, 20);
    // Bright 行：比例 0~1 映射为 0%~200%（中点 100%）。
    // RGB 行：value 0~2 直接 *100 即 0%~200%。
    int percent = (row == 3)
        ? juce::roundToInt(proportion * 200.0f)
        : juce::roundToInt(value * 100.0f);
    g.setColour(kRowColours[row].withAlpha(0.95f));
    g.setFont(PinkXP::getFont(8.0f, juce::Font::plain));
    g.drawText(y2k::tr (juce::String(percent) + "%"), valueRect, juce::Justification::centredLeft, false);
  }
}

int MilkdropModule::getEffectsColumns(juce::Rectangle<int> panel) const
{
  const int effect_count = CountImplementedMilkdropEffects();
  if (effect_count <= 0)
    return 1;

  constexpr int kPad = 6;
  const int avail_w = panel.getWidth() - kPad * 2;
  if (avail_w <= 0)
    return 1;

  // 每行至少 1 个，最多 effect_count 个；按最小按钮宽度 + 间距推算能容纳的列数。
  const int max_cols = juce::jmin(effect_count,
      juce::jmax(1, static_cast<int>((avail_w + kEffectsToggleGap)
                                     / (kEffectsToggleMinW + kEffectsToggleGap))));
  return max_cols;
}

int MilkdropModule::getEffectsRowCount(juce::Rectangle<int> panel) const
{
  const int effect_count = CountImplementedMilkdropEffects();
  if (effect_count <= 0)
    return 0;
  const int cols = getEffectsColumns(panel);
  return (effect_count + cols - 1) / cols;
}

juce::Rectangle<int> MilkdropModule::getEffectsPanelBounds(juce::Rectangle<int> topBar) const
{
  const int rows = getEffectsRowCount(
      juce::Rectangle<int>(topBar.getX(), topBar.getBottom(),
                           topBar.getWidth(), 10));
  const float height = kEffectsHeaderH
                     + static_cast<float>(rows) * kEffectsRowH
                     + kEffectsPadBottom;
  return juce::Rectangle<int>(topBar.getX(), topBar.getBottom(),
                              topBar.getWidth(),
                              static_cast<int>(height));
}

juce::Rectangle<int> MilkdropModule::getEffectsToggleBounds(juce::Rectangle<int> panel, int row) const
{
  constexpr int kPad     = 6;
  const int kHeaderH     = static_cast<int>(kEffectsHeaderH);
  const int kRowH        = static_cast<int>(kEffectsRowH);
  const int kToggleH     = 16;
  const int kGap         = static_cast<int>(kEffectsToggleGap);

  const int cols = getEffectsColumns(panel);
  const int col  = row % cols;
  const int r    = row / cols;

  // 可用宽度先扣除列间间距，再均分到每列。此前 cell_w = avail_w / cols
  // 未扣间距，却用 col * (cell_w + kGap) 累加列偏移，导致每列都向右多偏
  // col*kGap，最后一列被挤窄（模块拉宽、列数增多时尤其明显）。整除余数
  // 补给最后一列，保证最右侧按钮不会被挤小。
  const int avail_w   = panel.getWidth() - kPad * 2;
  const int total_gap = (cols - 1) * kGap;
  const int cell_w    = (avail_w - total_gap) / cols;
  const int remainder = (avail_w - total_gap) % cols;
  const int x = panel.getX() + kPad + col * (cell_w + kGap);
  const int w = (col == cols - 1) ? (cell_w + remainder) : cell_w;
  const int y = panel.getY() + kHeaderH + r * kRowH + (kRowH - kToggleH) / 2;

  return juce::Rectangle<int>(x, y, juce::jmax(20, w), juce::jmax(16, kToggleH));
}

juce::Rectangle<int> MilkdropModule::getEffectsResetBounds(juce::Rectangle<int> panel) const
{
  constexpr int kPad = 6;
  return juce::Rectangle<int>(panel.getRight() - kPad - 44, panel.getY() + 2, 44, 18);
}

void MilkdropModule::paintEffectsPanel(juce::Graphics& g, juce::Rectangle<int> topBar)
{
  auto panel = getEffectsPanelBounds(topBar);
  cachedEffectsPanelRect_ = panel;

  // 风格化半透明子面板容器
  drawMilkdropOverlaySurface (g, panel, false);

  // Reset 按钮（先算位置，标题宽度自适应至按钮左侧，避免 VT323 下 "EFFECTS" 截断）
  const auto mousePos = getMouseXYRelative();
  auto resetRect = getEffectsResetBounds(panel);
  cachedEffectsResetRect_ = resetRect;

  // 标题 "EFFECTS"
  g.setColour(PinkXP::pink300.withAlpha(0.95f));
  g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
  g.drawFittedText(y2k::tr ("EFFECTS"),
                   juce::Rectangle<int>(panel.getX() + 6, panel.getY(),
                                        juce::jmax(60, panel.getWidth() - resetRect.getWidth() - 16), 22),
                   juce::Justification::centredLeft, 1, 0.78f);

  const bool resetHovered = resetRect.contains(mousePos);
  g.setColour (drawMilkdropOverlayButton (g, resetRect, false, resetHovered, false));
  g.setFont(PinkXP::getFont(8.0f, juce::Font::bold));
  g.drawFittedText(y2k::tr ("Reset"), resetRect.reduced(2, 0), juce::Justification::centred, 1, 0.72f);

  // 开关按钮：基于效果注册表动态生成，按钮自身即开关（按下=开启，弹起=关闭）。
  const bool isCrtEffects = (PinkXP::getSkinTokens().frameStyle == PinkXP::FrameStyle::crtPhosphor);
  const auto toggleFont   = PinkXP::getFont(isCrtEffects ? 7.2f : 8.0f, juce::Font::bold);
  int row = 0;
  for (const auto& def : GetMilkdropEffectDefs())
  {
    if (!def.implemented)
      continue;

    const bool enabled = def.get(visualState_);
    auto toggle = getEffectsToggleBounds(panel, row);
    const bool hovered = toggle.contains(mousePos);

    g.setColour (drawMilkdropOverlayButton (g, toggle, enabled, hovered, false));
    auto tr = toggle;
    if (enabled)
      tr.translate (1, 1);
    g.setFont(toggleFont);
    g.drawFittedText(y2k::tr (def.display_name), tr.reduced(2, 0),
                     juce::Justification::centred, 1, 0.72f);
    ++row;
  }
}

// ==========================================================
// 波形样式控制器（simple waveform style）
// ==========================================================
juce::Rectangle<int> MilkdropModule::getWavePanelBounds(juce::Rectangle<int> topBar) const
{
  const float height = kWaveHeaderH + kWaveModeRowH + 7.0f * kWaveSliderRowH
                     + kWaveSwitchRowH + kWavePadBottom;
  return juce::Rectangle<int>(topBar.getX(), topBar.getBottom(),
                              topBar.getWidth(), static_cast<int>(height));
}

juce::Rectangle<int> MilkdropModule::getWaveModeStepperBounds(juce::Rectangle<int> panel) const
{
  constexpr int kPad = 6;
  const int y = panel.getY() + static_cast<int>(kWaveHeaderH);
  const int h = static_cast<int>(kWaveModeRowH);
  return juce::Rectangle<int>(panel.getX() + kPad, y, panel.getWidth() - kPad * 2, h);
}

juce::Rectangle<int> MilkdropModule::getWaveSliderBounds(juce::Rectangle<int> panel, int row) const
{
  constexpr int kPad    = 6;
  constexpr int kLabelW = 26;
  constexpr int kValueW = 44;
  constexpr int kSliderH = 8;

  const int sliderX = panel.getX() + kPad + kLabelW + 4;
  const int sliderW = panel.getWidth() - kPad - kLabelW - 4 - kValueW - kPad;
  const int y = panel.getY() + static_cast<int>(kWaveHeaderH + kWaveModeRowH)
              + row * static_cast<int>(kWaveSliderRowH)
              + (static_cast<int>(kWaveSliderRowH) - kSliderH) / 2;

  return juce::Rectangle<int>(sliderX, y, juce::jmax(20, sliderW), kSliderH);
}

juce::Rectangle<int> MilkdropModule::getWaveSwitchBounds(juce::Rectangle<int> panel, int index) const
{
  constexpr int kPad  = 6;
  constexpr int kGap  = 4;
  const int y = panel.getY() + static_cast<int>(kWaveHeaderH + kWaveModeRowH)
              + 7 * static_cast<int>(kWaveSliderRowH) + 2;
  const int h = static_cast<int>(kWaveSwitchRowH) - 6;
  const int totalW = panel.getWidth() - kPad * 2 - kGap * 3;
  const int cellW  = juce::jmax(20, totalW / 4);
  const int x = panel.getX() + kPad + index * (cellW + kGap);
  return juce::Rectangle<int>(x, y, cellW, h);
}

juce::Rectangle<int> MilkdropModule::getWaveResetBounds(juce::Rectangle<int> panel) const
{
  constexpr int kPad = 6;
  return juce::Rectangle<int>(panel.getRight() - kPad - 44, panel.getY() + 2, 44, 18);
}

void MilkdropModule::updateWaveFromSlider(int row, float proportion)
{
  proportion = juce::jlimit(0.0f, 1.0f, proportion);
  waveState_.enabled = true;
  switch (row)
  {
    case 0: waveState_.x = proportion; break;
    case 1: waveState_.y = proportion; break;
    case 2: waveState_.r = proportion; break;
    case 3: waveState_.g = proportion; break;
    case 4: waveState_.b = proportion; break;
    case 5: waveState_.a = proportion; break;
    case 6: waveState_.mystery = proportion * 2.0f - 1.0f; break;
    default: break;
  }
  applyWaveToEditor();
}

void MilkdropModule::reloadCurrentPresetForWave()
{
  if (glView == nullptr)
    return;

  // 嵌入态由 Editor 全局 projectM 渲染；浮动态由 GLView 本地 projectM 渲染。
  // 通过"跳转到当前索引"复用现有预设切换链路，触发带 wave 注入的重新加载。
  if (isFloating())
  {
    const int idx = glView->GetCurrentPresetIndex();
    if (idx >= 0)
      glView->RequestPresetJump(idx);
  }
  else if (editor_ != nullptr)
  {
    const int idx = editor_->GetMilkdropCurrentPresetIndex();
    if (idx >= 0)
      editor_->RequestMilkdropPresetJump(idx);
  }
}

void MilkdropModule::paintWavePanel(juce::Graphics& g, juce::Rectangle<int> topBar)
{
  auto panel = getWavePanelBounds(topBar);
  cachedWavePanelRect_ = panel;

  // 风格化半透明子面板容器
  drawMilkdropOverlaySurface (g, panel, false);

  // Reset 按钮（先算位置，标题宽度自适应至按钮左侧）
  const auto mousePos = getMouseXYRelative();
  auto resetRect = getWaveResetBounds(panel);
  cachedWaveResetRect_ = resetRect;

  // 标题 "WAVE"
  g.setColour(PinkXP::pink300.withAlpha(0.95f));
  g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
  g.drawFittedText(y2k::tr ("WAVE"),
                   juce::Rectangle<int>(panel.getX() + 6, panel.getY(),
                                        juce::jmax(60, panel.getWidth() - resetRect.getWidth() - 16), 22),
                   juce::Justification::centredLeft, 1, 0.78f);

  const bool resetHovered = resetRect.contains(mousePos);
  g.setColour (drawMilkdropOverlayButton (g, resetRect, false, resetHovered, false));
  g.setFont(PinkXP::getFont(8.0f, juce::Font::bold));
  g.drawFittedText(y2k::tr ("Reset"), resetRect.reduced(2, 0), juce::Justification::centred, 1, 0.72f);

  // mode stepper：< 模式名 >
  {
    auto stepper = getWaveModeStepperBounds(panel);
    const int arrowW = 22;
    auto leftArrow  = juce::Rectangle<int>(stepper.getX(), stepper.getY(), arrowW, stepper.getHeight());
    auto rightArrow = juce::Rectangle<int>(stepper.getRight() - arrowW, stepper.getY(), arrowW, stepper.getHeight());
    auto nameRect   = juce::Rectangle<int>(stepper.getX() + arrowW, stepper.getY(),
                                           stepper.getWidth() - arrowW * 2, stepper.getHeight());

    // 绘制单个箭头（按下 > 悬停 > 常态）
    auto drawArrow = [&](juce::Rectangle<int> r, const juce::String& label, int zone)
    {
        const bool pressed = (waveModeStepperPressed_ == zone);
        const bool hovered = (waveModeStepperHover_ == zone);

        g.setColour (drawMilkdropOverlayButton (g, r, false, hovered, pressed));
        auto tr = r;
        if (pressed)
            tr.translate (1, 1);
        g.setFont(PinkXP::SymbolGlyphs::iconFont(9.0f, juce::Font::bold));
        g.drawText(y2k::tr (label), tr, juce::Justification::centred, false);
    };

    drawArrow(leftArrow,  "<", 0);
    drawArrow(rightArrow, ">", 1);

    // 模式名（当前是否启用覆盖用颜色区分）
    const bool override_on = waveState_.enabled;
    g.setColour(override_on ? PinkXP::pink300 : juce::Colour(0xDD, 0xDD, 0xDD));
    g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
    g.drawText(y2k::tr (juce::String(GetWaveModeName(waveState_.mode))),
               nameRect, juce::Justification::centred, false);
  }

  // 7 个 slider：X / Y / R / G / B / A / Mystery
  static const char* kLabels[7] = { "X", "Y", "R", "G", "B", "A", "Mys" };
  static const juce::Colour kRowColours[7] = {
      juce::Colour(0xFF, 0x6B, 0x6B),  // X
      juce::Colour(0x6B, 0xFF, 0x6B),  // Y
      juce::Colour(0xFF, 0x6B, 0x6B),  // R
      juce::Colour(0x6B, 0xFF, 0x6B),  // G
      juce::Colour(0x6B, 0x9B, 0xFF),  // B
      juce::Colour(0xE8, 0xE8, 0xE8),  // A
      juce::Colour(0xE0, 0xA0, 0xFF),  // Mystery
  };

  for (int row = 0; row < 7; ++row)
  {
    float value = 0.0f;
    float proportion = 0.0f;
    switch (row)
    {
      case 0: value = waveState_.x; proportion = value; break;
      case 1: value = waveState_.y; proportion = value; break;
      case 2: value = waveState_.r; proportion = value; break;
      case 3: value = waveState_.g; proportion = value; break;
      case 4: value = waveState_.b; proportion = value; break;
      case 5: value = waveState_.a; proportion = value; break;
      case 6: value = waveState_.mystery; proportion = (value + 1.0f) * 0.5f; break;
      default: break;
    }
    proportion = juce::jlimit(0.0f, 1.0f, proportion);

    auto sliderBounds = getWaveSliderBounds(panel, row);
    int labelY = sliderBounds.getY() - 6;
    auto labelRect = juce::Rectangle<int>(panel.getX() + 6, labelY, 26, 20);

    g.setColour(kRowColours[row]);
    g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
    g.drawText(y2k::tr (kLabels[row]), labelRect, juce::Justification::centredLeft, false);

    drawMilkdropOverlaySlider (g, sliderBounds, proportion, kRowColours[row], draggingWaveRow_ == row);

    // 右侧数值
    auto valueRect = juce::Rectangle<int>(sliderBounds.getRight() + 4, labelY, 44, 20);
    int percent = (row == 6)
        ? juce::roundToInt(value * 100.0f)          // Mystery: -100% ~ 100%
        : juce::roundToInt(value * 100.0f);         // 其余: 0% ~ 100%
    g.setColour(kRowColours[row].withAlpha(0.95f));
    g.setFont(PinkXP::getFont(8.0f, juce::Font::plain));
    g.drawText(y2k::tr (juce::String(percent) + "%"), valueRect, juce::Justification::centredLeft, false);
  }

  // 4 个开关：dots / thick / additive / brighten
  static const char* kSwitchNames[4] = { "dots", "thick", "add", "bright" };
  for (int idx = 0; idx < 4; ++idx)
  {
    const bool enabled = (idx == 0) ? waveState_.usedots
                       : (idx == 1) ? waveState_.thick
                       : (idx == 2) ? waveState_.additive
                                    : waveState_.brighten;
    auto sw = getWaveSwitchBounds(panel, idx);
    const bool hovered = sw.contains(mousePos);

    g.setColour (drawMilkdropOverlayButton (g, sw, enabled, hovered, false));
    auto tr = sw;
    if (enabled)
      tr.translate (1, 1);
    g.setFont(PinkXP::getFont(8.0f, juce::Font::bold));
    g.drawText(y2k::tr (kSwitchNames[idx]), tr, juce::Justification::centred, false);
  }
}

// ==========================================================
// 全局变换偏移控制器（tweak panel）
// ==========================================================
juce::Rectangle<int> MilkdropModule::getTweakPanelBounds(juce::Rectangle<int> topBar) const
{
  const float height = kTweakHeaderH
                     + MilkdropVisualOffsetState::kParamCount * kTweakSliderRowH
                     + MilkdropVisualOffsetState::kIntParamCount * kTweakIntRowH
                     + kTweakPadBottom;
  return juce::Rectangle<int>(topBar.getX(), topBar.getBottom(),
                              topBar.getWidth(), static_cast<int>(height));
}

juce::Rectangle<int> MilkdropModule::getTweakSliderBounds(juce::Rectangle<int> panel, int row) const
{
  constexpr int kPad    = 6;
  constexpr int kLabelW = 30;
  constexpr int kValueW = 44;
  constexpr int kSliderH = 8;

  const int sliderX = panel.getX() + kPad + kLabelW + 4;
  const int sliderW = panel.getWidth() - kPad - kLabelW - 4 - kValueW - kPad;
  const int y = panel.getY() + static_cast<int>(kTweakHeaderH)
              + row * static_cast<int>(kTweakSliderRowH)
              + (static_cast<int>(kTweakSliderRowH) - kSliderH) / 2;

  return juce::Rectangle<int>(sliderX, y, juce::jmax(20, sliderW), kSliderH);
}

juce::Rectangle<int> MilkdropModule::getTweakIntSliderBounds(juce::Rectangle<int> panel, int row) const
{
  constexpr int kPad    = 6;
  constexpr int kLabelW = 30;
  constexpr int kValueW = 44;
  constexpr int kSliderH = 8;

  const int sliderX = panel.getX() + kPad + kLabelW + 4;
  const int sliderW = panel.getWidth() - kPad - kLabelW - 4 - kValueW - kPad;
  const int y = panel.getY() + static_cast<int>(kTweakHeaderH)
              + MilkdropVisualOffsetState::kParamCount * static_cast<int>(kTweakSliderRowH)
              + row * static_cast<int>(kTweakIntRowH)
              + (static_cast<int>(kTweakIntRowH) - kSliderH) / 2;

  return juce::Rectangle<int>(sliderX, y, juce::jmax(20, sliderW), kSliderH);
}

juce::Rectangle<int> MilkdropModule::getTweakResetBounds(juce::Rectangle<int> panel) const
{
  constexpr int kPad = 6;
  return juce::Rectangle<int>(panel.getRight() - kPad - 44, panel.getY() + 2, 44, 18);
}

void MilkdropModule::updateTweakFromSlider(int row, float proportion)
{
  proportion = juce::jlimit(0.0f, 1.0f, proportion);
  if (row < 0 || row >= MilkdropVisualOffsetState::kParamCount)
    return;

  // 线性映射：proportion 0→min_value，1→max_value（中性点 0 的位置由 min/max 决定）。
  const auto* params = GetVisualOffsetParams();
  const float min_v = params[row].min_value;
  const float max_v = params[row].max_value;
  visualState_.offset.value[row] = min_v + proportion * (max_v - min_v);
  applyVisualToEditor();
}

void MilkdropModule::updateTweakIntFromSlider(int row, float proportion)
{
  proportion = juce::jlimit(0.0f, 1.0f, proportion);
  if (row < 0 || row >= MilkdropVisualOffsetState::kIntParamCount)
    return;

  // 线性映射并四舍五入取整：proportion 0→min_value，1→max_value。
  const auto* params = GetVisualOffsetIntParams();
  const int min_v = params[row].min_value;
  const int max_v = params[row].max_value;
  const float vf = min_v + proportion * (max_v - min_v);
  visualState_.offset.ivalue[row] = juce::roundToInt(vf);
  applyVisualToEditor();
}

void MilkdropModule::paintTweakPanel(juce::Graphics& g, juce::Rectangle<int> topBar)
{
  auto panel = getTweakPanelBounds(topBar);
  cachedTweakPanelRect_ = panel;

  // 风格化半透明子面板容器
  drawMilkdropOverlaySurface (g, panel, false);

  // Reset 按钮（先算位置，标题宽度自适应至按钮左侧）
  auto resetRect = getTweakResetBounds(panel);
  cachedTweakResetRect_ = resetRect;

  // 标题 "TWEAK"
  g.setColour(PinkXP::pink300.withAlpha(0.95f));
  g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
  g.drawFittedText(y2k::tr ("TWEAK"),
                   juce::Rectangle<int>(panel.getX() + 6, panel.getY(),
                                        juce::jmax(60, panel.getWidth() - resetRect.getWidth() - 16), 22),
                   juce::Justification::centredLeft, 1, 0.78f);

  const bool resetHovered = resetRect.contains(getMouseXYRelative());
  g.setColour (drawMilkdropOverlayButton (g, resetRect, false, resetHovered, false));
  g.setFont(PinkXP::getFont(8.0f, juce::Font::bold));
  g.drawFittedText(y2k::tr ("Reset"), resetRect.reduced(2, 0), juce::Justification::centred, 1, 0.72f);

  const auto* params = GetVisualOffsetParams();
  for (int row = 0; row < MilkdropVisualOffsetState::kParamCount; ++row)
  {
    const float value = visualState_.offset.value[row];
    const float min_v = params[row].min_value;
    const float max_v = params[row].max_value;

    // 当前值与中性点(0)在滑块轨道上的比例位置（0~1）。
    // 中性点位置由 min/max 决定：zoom 的 min=-0.3、max=1.0，故中性点偏左而非居中。
    float proportion = (value - min_v) / (max_v - min_v);
    proportion = juce::jlimit(0.0f, 1.0f, proportion);
    float neutral = (0.0f - min_v) / (max_v - min_v);
    neutral = juce::jlimit(0.0f, 1.0f, neutral);

    auto sliderBounds = getTweakSliderBounds(panel, row);
    int labelY = sliderBounds.getY() - 6;
    auto labelRect = juce::Rectangle<int>(panel.getX() + 6, labelY, 30, 20);

    g.setColour(PinkXP::pink300.withAlpha(0.95f));
    g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
    g.drawText(y2k::tr (params[row].display_name), labelRect, juce::Justification::centredLeft, false);

    drawMilkdropOverlaySlider (g, sliderBounds, proportion, PinkXP::pink200,
                               draggingTweakRow_ == row, neutral);

    // 右侧数值（显示偏移量，带正负号）
    auto valueRect = juce::Rectangle<int>(sliderBounds.getRight() + 4, labelY, 44, 20);
    g.setColour(PinkXP::pink300.withAlpha(0.95f));
    g.setFont(PinkXP::getFont(8.0f, juce::Font::plain));
    g.drawText(y2k::tr (juce::String(value, 2)), valueRect, juce::Justification::centredLeft, false);
  }

  // 3 个整数滑块（kaleido / fold_x / fold_y，1~16）
  const auto* intParams = GetVisualOffsetIntParams();
  for (int row = 0; row < MilkdropVisualOffsetState::kIntParamCount; ++row)
  {
    const int value = visualState_.offset.ivalue[row];
    const int min_v = intParams[row].min_value;
    const int max_v = intParams[row].max_value;

    auto sliderBounds = getTweakIntSliderBounds(panel, row);
    const int labelY = sliderBounds.getY() - 6;
    auto labelRect = juce::Rectangle<int>(panel.getX() + 6, labelY, 30, 20);

    // 名称 label
    g.setColour(PinkXP::pink300.withAlpha(0.95f));
    g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
    g.drawText(y2k::tr (intParams[row].display_name), labelRect, juce::Justification::centredLeft, false);

    // 当前值在轨道上的比例（value 0 与 1 均落在最左，因为滑块范围从 1 开始）。
    float proportion = static_cast<float>(value - min_v) / static_cast<float>(max_v - min_v);
    proportion = juce::jlimit(0.0f, 1.0f, proportion);

    drawMilkdropOverlaySlider (g, sliderBounds, proportion, PinkXP::pink200,
                               draggingTweakIntRow_ == row);

    // 右侧数值（整数值）
    auto valueRect = juce::Rectangle<int>(sliderBounds.getRight() + 4, labelY, 44, 20);
    g.setColour(PinkXP::pink300.withAlpha(0.95f));
    g.setFont(PinkXP::getFont(8.0f, juce::Font::plain));
    g.drawText(y2k::tr (juce::String(value)), valueRect, juce::Justification::centredLeft, false);
  }
}