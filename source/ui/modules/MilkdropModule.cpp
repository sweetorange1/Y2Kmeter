/*
  ==============================================================================

  MilkdropModule.cpp
  Y2Kmeter — Milkdrop 模块（自 2.0.4 起，libprojectM 4 原生实现）

  参见 MilkdropModule.h 顶部注释，尤其是"架构概览"与"生命周期规则"两段。

  ==============================================================================
*/
#include "MilkdropModule.h"
#include "ProjectMApi.h"
#if JUCE_MAC
#include "MilkdropModule_mac.h"
#endif
#include "source/ui/PinkXPStyle.h"
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

// ==========================================================
// 布局锁定判断（ModulePanel / TamagotchiModule 各自都有一份,
//   供 mouseDown 中判断是否禁止拖拽/缩放/关闭等操作）
// ==========================================================
namespace
{
    bool isPanelLayoutLocked(const juce::Component& panel) noexcept
    {
        if (auto* ws = dynamic_cast<const ModuleWorkspace*>(panel.getParentComponent()))
            return ws->isLayoutLocked();
        return false;
    }

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
    if (overlayView_ != nullptr && overlayView_->isOnDesktop())
        overlayView_->removeFromDesktop();
    overlayView_.reset();
#endif

    glView.reset(); // 现在可以安全地销毁子组件
}

juce::ValueTree MilkdropModule::saveModuleSpecificState() const
{
  juce::ValueTree s("state");
  if (glView != nullptr && (isFloating() || restored_preset_index_ < 0))
    glView->SyncOwnerPresetIndexFromRenderer();
  const int idx = restored_preset_index_;
  if (idx >= 0)
    s.setProperty("presetIndex", idx, nullptr);
  s.setProperty("autoMode", isAutoMode_, nullptr);
  s.setProperty("autoInterval", autoIntervalSeconds_, nullptr);
  return s;
}

void MilkdropModule::restoreModuleSpecificState(const juce::ValueTree& state)
{
  if (state.hasProperty("presetIndex"))
  {
    int idx = static_cast<int>(state.getProperty("presetIndex"));
    // 范围校验由 newOpenGLContextCreated 负责（此时 presetPaths 可能还未扫描）
    if (idx >= 0)
      restored_preset_index_ = idx;
  }
  if (state.hasProperty("autoMode"))
    isAutoMode_ = static_cast<bool>(state.getProperty("autoMode"));
  if (state.hasProperty("autoInterval"))
  {
    autoIntervalSeconds_ = juce::jlimit(kMinAutoInterval, kMaxAutoInterval,
                                        static_cast<float>(state.getProperty("autoInterval")));
  }
}

juce::Rectangle<int> MilkdropModule::GetContentLocalBounds() const {
  return getContentBounds();
}

void MilkdropModule::paint(juce::Graphics& g) {
  // Editor::renderOpenGL 已经将 projectM 帧渲染到 Editor CachedImage FBO 中
  // 本模块内容区屏幕坐标对应的区域。这里绘制卡片外壳（边框、标题栏、关闭按钮），
  // 内容区保持透明以保留 GPU 渲染的 projectM 帧。
  const auto bounds = getLocalBounds();

  // 1. 像素凸起窗口边框（边框必须不透明，内容区必须透明以透出 projectM 帧）
  //   · macOS：Editor GL 未启用，GLView 自己的 GL surface 高于 paint，用不透明 face。
  //   · Windows 脱离态：GLView 自己的 native GL surface 高于 paint，同理用不透明 face。
  //   · Windows 非脱离态：Editor GL 渲染到 FBO 0（CachedImage）位于组件树之下，
  //     drawRaised 首行的 fillRect 会覆盖整个模块矩形；用不透明 face 会遮住
  //     projectM 帧，表现为"模块底色色块"。必须用 transparentBlack 让内容区
  //     透明透出帧（边框 hl/dark/shdw 仍为不透明正常显示）。
#if JUCE_MAC
  PinkXP::drawRaised(g, bounds, PinkXP::face);
#else
  if (isFloating())
    PinkXP::drawRaised(g, bounds, PinkXP::face);
  else
    PinkXP::drawRaised(g, bounds, juce::Colours::transparentBlack);
#endif

  // 2. 玫瑰粉标题栏
  auto tb = getTitleBarBounds();
  PinkXP::drawPinkTitleBar(g, tb, titleText, 12.0f);

  // 标题栏下沿深色分割线
  g.setColour(PinkXP::dark);
  g.fillRect(tb.getX(), tb.getBottom(), tb.getWidth(), 1);

  // 3. 关闭按钮（×）—— 借用基类的 pressed/hover 标志，与 ModulePanel 风格一致
  auto cb = getCloseButtonBounds();
  if (closeButtonPressed)
    PinkXP::drawPressed(g, cb, PinkXP::pink100);
  else
    PinkXP::drawRaised(g, cb, closeButtonHovered ? PinkXP::pink200 : PinkXP::btnFace);
  g.setColour(PinkXP::ink);
  g.setFont(PinkXP::getFont(11.0f, juce::Font::bold));
  auto cbText = cb;
  cbText.translate(0, -1);
  if (closeButtonPressed) cbText.translate(1, 1);
  g.drawText("x", cbText, juce::Justification::centred, false);

  // 3.5. 弹出/停靠按钮
  if (isPopOutEnabled() || isFloating())
  {
    auto popBtn = getPopOutButtonBounds();
    if (popOutButtonPressed_)
      PinkXP::drawPressed(g, popBtn, PinkXP::pink100);
    else
      PinkXP::drawRaised(g, popBtn, popOutButtonHovered_ ? PinkXP::pink200 : PinkXP::btnFace);
    g.setColour(PinkXP::ink);
    g.setFont(PinkXP::getFont(11.0f, juce::Font::bold));
    auto popBtnText = popBtn;
    popBtnText.translate(0, -1);
    if (popOutButtonPressed_) popBtnText.translate(1, 1);
    g.drawText(isFloating() ? "=" : "-", popBtnText, juce::Justification::centred, false);
  }

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
      else
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
        g.fillAll(juce::Colours::black);
        auto msg = glView->GetError().isEmpty()
                       ? juce::String("Milkdrop initializing...")
                       : juce::String("Milkdrop error: ") + glView->GetError();
        g.setColour(juce::Colours::grey);
        g.setFont(juce::Font(12.0f));
        g.drawText(msg, content, juce::Justification::centred, false);
      }
    }
  } else {
    g.fillAll(juce::Colours::black);
    g.setColour(juce::Colours::grey);
    g.setFont(juce::Font(12.0f));
    g.drawText("Milkdrop initializing...", content, juce::Justification::centred, false);
  }

  if (glView != nullptr && glView->IsRenderReady())
    PaintLoadingIndicator(g, content);

  // 控制栏（overlay control bar）以 CPU paint 叠加方式绘制在 GL 帧之上。
  // Windows：直接在 paintContent 绘制即可见（JUCE 能正确混合 CPU/GL 层）。
  // macOS：此处绘制会被 native NSOpenGLView 遮挡（看不见），但必须保留
  //   该调用，因为 paintOverlayControlBar 内部会更新 cachedNameArea_，
  //   供 MilkdropModule::mouseDown/hitTestOverlayButton 使用。
  //   真正可见的控制栏由顶层 overlayView_（NSWindow）另行绘制。
  if (focused_ && glView != nullptr) {
    auto topBar = content.withHeight(26);
    paintOverlayControlBar(g, topBar);
    if (isAutoMode_)
      paintAutoControlRow(g, topBar);
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
    // triggerRepaint removed: Editor GL drives rendering;
}

void MilkdropModule::nextPreset()
{
    if (glView != nullptr) glView->RequestPresetDelta (+1);
}

void MilkdropModule::prevPreset()
{
    if (glView != nullptr) glView->RequestPresetDelta (-1);
}

void MilkdropModule::randomPreset()
{
    if (glView != nullptr) glView->RequestPresetRandom();
}

void MilkdropModule::jumpToPresetIndex(int index)
{
    if (glView != nullptr) glView->RequestPresetJump(index);
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

  auto content = owner_.getContentBounds();
  const auto offset = getPosition();
  g.saveState();
  g.addTransform(juce::AffineTransform::translation(
      -static_cast<float>(offset.getX()),
      -static_cast<float>(offset.getY())));
  owner_.paintOverlayControlBar(g, content);
  if (owner_.isAutoModeActive())
    owner_.paintAutoControlRow(g, content.withHeight(26));
  g.restoreState();
#else
  juce::ignoreUnused(g);
#endif
}

void MilkdropModule::GLView::UpdateOpenGLAttachment() {
  // macOS：Editor GL 上下文在 macOS 下被关闭（#if !JUCE_MAC 宏），
  //   嵌入态也必须使用 GLView 自己的 OpenGL 上下文来驱动 projectM 渲染。
  //   因此 macOS 上无论嵌入态还是浮动态，只要组件可见且有尺寸就 attach。
  // Windows：嵌入态由 Editor GL 上下文渲染，GLView 只在浮动态 attach。
#if JUCE_MAC
  const bool should_attach = isShowing() && getWidth() > 0 && getHeight() > 0;
#else
  const bool should_attach = owner_.isFloating() && isShowing() && getWidth() > 0 && getHeight() > 0;
#endif
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

  // 只有 Windows 浮动态 detach 时才需要恢复 Editor renderer：
  //   · Windows 嵌入态：Editor GL 负责渲染，浮动时挂起，dock 回来时恢复。
  //   · macOS：Editor GL 未启用，GLView 自己的 GL 上下文负责渲染，
  //     无论嵌入/浮动都不需要操作 Editor renderer。
#if ! JUCE_MAC
  const bool should_resume_editor_renderer = !owner_.isFloating();
#else
  const bool should_resume_editor_renderer = false;
#endif

  open_gl_context_.detach();
  attached_ = false;
  if (owner_.editor_ != nullptr && should_resume_editor_renderer) {
    const int preset_index = owner_.restored_preset_index_;
    if (preset_index >= 0)
      owner_.editor_->RequestMilkdropPresetJump(preset_index);
    owner_.editor_->ResumeMilkdropEditorRendererAfterFloating();
  }
}

void MilkdropModule::GLView::ScanPresetFiles() {
  local_preset_paths_.clear();
  auto presets_dir = FindMilkdropAssetsDirForModule("milkdrop_presets");
  if (!presets_dir.exists())
    return;

  auto files = presets_dir.findChildFiles(juce::File::findFiles, false, "*.milk");
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
  juce::Logger::writeToLog (juce::String ("[Milkdrop] LoadCurrentPreset idx=")
                            + juce::String (local_current_preset_)
                            + " hasLoadPresetData=" + juce::String (api.hasLoadPresetData() ? 1 : 0)
                            + " path='" + path + "'");
  if (api.hasLoadPresetData()) {
    juce::File file(path);
    if (file.existsAsFile()) {
      auto data = file.loadFileAsString().toStdString();
      juce::Logger::writeToLog (juce::String ("[Milkdrop] LoadCurrentPreset file OK, data bytes=")
                                + juce::String (static_cast<int> (data.size())));
      api.loadPresetData(local_pm_handle_, FixMilkdropShaderTypes(data), true);
    } else {
      juce::Logger::writeToLog ("[Milkdrop] LoadCurrentPreset file MISSING!");
    }
  } else {
    api.loadPresetFile(local_pm_handle_, path.toRawUTF8(), true);
  }

  while (juce::gl::glGetError() != juce::gl::GL_NO_ERROR) {}
}

void MilkdropModule::GLView::newOpenGLContextCreated() {
  juce::Logger::writeToLog ("[Milkdrop] newOpenGLContextCreated: BEGIN");
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
  juce::Logger::writeToLog (juce::String ("[Milkdrop] Api::instance() got, isAvailable=")
                            + juce::String (api.isAvailable() ? 1 : 0)
                            + " loadError='" + juce::String (api.loadError()) + "'");
  api.resetGlewInitialization();
  if (!api.isAvailable()) {
    local_error_ = api.loadError();
    juce::Logger::writeToLog ("[Milkdrop] ABORT: api not available -> " + local_error_);
    return;
  }
  if (!api.initGlew()) {
    local_error_ = api.loadError();
    juce::Logger::writeToLog ("[Milkdrop] ABORT: initGlew failed -> " + local_error_);
    return;
  }

  local_pm_handle_ = api.create();
  juce::Logger::writeToLog (juce::String ("[Milkdrop] projectm_create -> handle=")
                            + juce::String (local_pm_handle_ != nullptr ? "ok" : "null"));
  if (local_pm_handle_ == nullptr) {
    local_error_ = "projectm_create() returned NULL.";
    juce::Logger::writeToLog ("[Milkdrop] ABORT: projectm_create returned NULL");
    return;
  }

  api.setMeshSize(local_pm_handle_, kDefaultMeshWidth, kDefaultMeshHeight);
  api.setFps(local_pm_handle_, kTargetFps);
  api.setPresetDuration(local_pm_handle_, kPresetDuration);
  api.setSoftCutDuration(local_pm_handle_, kSoftCutDuration);
  api.setHardCutEnabled(local_pm_handle_, false);

  auto tex_dir = FindMilkdropAssetsDirForModule("milkdrop_textures");
  juce::Logger::writeToLog (juce::String ("[Milkdrop] tex_dir exists=")
                            + juce::String (tex_dir.exists() ? 1 : 0)
                            + " path='" + tex_dir.getFullPathName() + "'");
  if (tex_dir.exists()) {
    std::vector<std::string> paths{tex_dir.getFullPathName().toStdString()};
    api.setTextureSearchPaths(local_pm_handle_, paths);
  }

  // 每次 GL 上下文重建都重新扫描预设，保证读取 restored_preset_index_ 前
  // local_preset_paths_ 是最新且完整的（与 e4bc0b78 行为一致）。
  ScanPresetFiles();
  juce::Logger::writeToLog ("[Milkdrop] preset count="
                            + juce::String (local_preset_paths_.size()));
  int pending = owner_.restored_preset_index_;
  owner_.restored_preset_index_ = -1;
  local_current_preset_ = (pending >= 0 && pending < local_preset_paths_.size()) ? pending : 0;
  juce::Logger::writeToLog (juce::String ("[Milkdrop] restore preset: pending=")
                            + juce::String (pending)
                            + " current=" + juce::String (local_current_preset_)
                            + " total=" + juce::String (local_preset_paths_.size()));

  // 预设 shader 编译前后清错误，避免 projectM 内部 hlslparser→GLSL 编译
  // 过程产生的无害 warning 污染 error 队列、拖到下一帧 checkGLError 里卡死。
  while (juce::gl::glGetError() != juce::gl::GL_NO_ERROR) {}
  LoadCurrentPreset();
  while (juce::gl::glGetError() != juce::gl::GL_NO_ERROR) {}

  last_preset_switch_ms_ = static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
  local_render_ready_ = true;
  juce::Logger::writeToLog ("[Milkdrop] newOpenGLContextCreated: END (render_ready=true)");
}

void MilkdropModule::GLView::openGLContextClosing() {
  SyncOwnerPresetIndexFromRenderer();
  // 在销毁 handle 之前先抓取最后一帧快照，
  // 供 detach/attach 重建期间 paintContent 显示，消除切换模式时的黑屏闪烁。
  CaptureLastFrame();
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
  int jump = requested_preset_jump_.exchange(-1);
  int delta = requested_preset_delta_.exchange(0);
  bool random = requested_preset_random_.exchange(false);
  bool switched = false;

  if (jump >= 0 && jump < local_preset_paths_.size()) {
    local_current_preset_ = jump;
    switched = true;
  } else if (random && !local_preset_paths_.isEmpty()) {
    local_current_preset_ = juce::Random::getSystemRandom().nextInt(local_preset_paths_.size());
    switched = true;
  } else if (delta != 0 && !local_preset_paths_.isEmpty()) {
    local_current_preset_ = (local_current_preset_ + delta) % local_preset_paths_.size();
    if (local_current_preset_ < 0)
      local_current_preset_ += local_preset_paths_.size();
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

  // local_render_scale_：1 = 原始分辨率，2 = 半分辨率，4 = 四分之一分辨率。
  // 值越大渲染分辨率越低，性能越好，画质越差。
  if (local_render_scale_ <= 1) {
    // ---- 1:1 模式：直接渲染到默认 framebuffer，无需离屏 FBO ----
    DestroyScaleFbo();  // 释放不再需要的 FBO
    api.setWindowSize(local_pm_handle_,
                      static_cast<std::size_t>(surface_w),
                      static_cast<std::size_t>(surface_h));
    juce::gl::glBindFramebuffer(juce::gl::GL_FRAMEBUFFER, 0);
    juce::gl::glViewport(0, 0, surface_w, surface_h);
    api.openglRenderFrame(local_pm_handle_);
  } else {
    // ---- 降分辨率模式：渲染到离屏 FBO，再 blit 到全屏 ----
    int render_w = juce::jmax(1, surface_w / local_render_scale_);
    int render_h = juce::jmax(1, surface_h / local_render_scale_);

    // 优先使用 projectM 4.2+ 的 openglRenderFrameFbo（直接渲染到指定 FBO）
    if (api.hasOpenglRenderFrameFbo()) {
      EnsureScaleFbo(render_w, render_h);
      api.setWindowSize(local_pm_handle_,
                        static_cast<std::size_t>(render_w),
                        static_cast<std::size_t>(render_h));
      api.openglRenderFrameFbo(local_pm_handle_, scale_fbo_);
    } else {
      // 回退：用 openglRenderFrame + 临时重定向 framebuffer
      EnsureScaleFbo(render_w, render_h);
      api.setWindowSize(local_pm_handle_,
                        static_cast<std::size_t>(render_w),
                        static_cast<std::size_t>(render_h));
      juce::gl::glBindFramebuffer(juce::gl::GL_FRAMEBUFFER, scale_fbo_);
      juce::gl::glViewport(0, 0, render_w, render_h);
      api.openglRenderFrame(local_pm_handle_);
      juce::gl::glBindFramebuffer(juce::gl::GL_FRAMEBUFFER, 0);
    }

    // 将离屏 FBO 内容 blit（拉伸）到默认 framebuffer（全屏）
    // 注意：macOS OpenGL Core Profile 对 glBlitFramebuffer 有严格限制：
    //   当源/目标尺寸不同（拉伸 blit）时，macOS 驱动对部分 FBO 格式
    //   使用 GL_LINEAR 会触发 GL_INVALID_OPERATION，导致 GL 上下文进入
    //   错误状态，后续所有渲染调用均被忽略，表现为画面卡死。
    //   改用 GL_NEAREST 在 macOS 上始终安全，画质差异在降分辨率场景下不明显。
    // 清除 projectM 内部渲染可能留下的残留 GL 错误，避免影响 blit 状态检测
    while (juce::gl::glGetError() != juce::gl::GL_NO_ERROR) {}
    juce::gl::glBindFramebuffer(juce::gl::GL_READ_FRAMEBUFFER, scale_fbo_);
    juce::gl::glBindFramebuffer(juce::gl::GL_DRAW_FRAMEBUFFER, 0);
    juce::gl::glBlitFramebuffer(
        0, 0, render_w, render_h,          // 源：离屏 FBO 全区域
        0, 0, surface_w, surface_h,        // 目标：默认 framebuffer 全区域
        juce::gl::GL_COLOR_BUFFER_BIT,
        juce::gl::GL_NEAREST);             // macOS 拉伸 blit 必须用 NEAREST，LINEAR 会触发 GL_INVALID_OPERATION
    juce::gl::glBindFramebuffer(juce::gl::GL_FRAMEBUFFER, 0);
  }
}

void MilkdropModule::GLView::PushPcm(const float* interleaved_lr,
                                      unsigned int frame_count) {
  // macOS：Editor GL 未启用，嵌入态也使用 GLView 本地 GL 上下文，
  //   不需要转发给 Editor renderer。
#if ! JUCE_MAC
  if (owner_.editor_ != nullptr && !owner_.isFloating())
    owner_.editor_->PushMilkdropPcm(interleaved_lr, frame_count);
#endif

  std::lock_guard<std::mutex> lock(pcm_mutex_);
  pending_pcm_.assign(interleaved_lr, interleaved_lr + frame_count * 2);
  pending_frames_ = frame_count;
}

void MilkdropModule::GLView::RequestPresetDelta(int delta) {
  // macOS：Editor GL 未启用，嵌入态也使用 GLView 本地 GL 上下文
#if JUCE_MAC
  requested_preset_delta_.fetch_add(delta);
#else
  if (owner_.isFloating()) {
    requested_preset_delta_.fetch_add(delta);
  } else if (owner_.editor_ != nullptr) {
    owner_.restored_preset_index_ = -1;
    owner_.editor_->RequestMilkdropPresetDelta(delta);
  }
#endif
}

void MilkdropModule::GLView::RequestPresetRandom() {
  // macOS：Editor GL 未启用，嵌入态也使用 GLView 本地 GL 上下文
#if JUCE_MAC
  if (!local_preset_paths_.isEmpty())
    requested_preset_jump_.store(juce::Random::getSystemRandom().nextInt(local_preset_paths_.size()));
#else
  if (owner_.isFloating()) {
    if (!local_preset_paths_.isEmpty())
      requested_preset_jump_.store(juce::Random::getSystemRandom().nextInt(local_preset_paths_.size()));
  } else if (owner_.editor_ != nullptr) {
    owner_.restored_preset_index_ = -1;
    owner_.editor_->RequestMilkdropPresetRandom();
  }
#endif
}

void MilkdropModule::GLView::RequestPresetJump(int index) {
  // macOS：Editor GL 未启用，嵌入态也使用 GLView 本地 GL 上下文
#if JUCE_MAC
  requested_preset_jump_.store(index);
#else
  if (owner_.isFloating()) {
    requested_preset_jump_.store(index);
  } else if (owner_.editor_ != nullptr) {
    owner_.restored_preset_index_ = -1;
    owner_.editor_->RequestMilkdropPresetJump(index);
  }
#endif
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

bool MilkdropModule::GLView::IsRenderReady() const {
  // macOS：Editor GL 未启用，嵌入态也使用 GLView 本地 GL 上下文
#if JUCE_MAC
  return local_render_ready_;
#else
  if (owner_.isFloating())
    return local_render_ready_;
  if (owner_.editor_ != nullptr)
    return owner_.editor_->IsMilkdropRenderReady();
  return false;
#endif
}

juce::String MilkdropModule::GLView::GetError() const {
  // macOS：Editor GL 未启用，嵌入态也使用 GLView 本地 GL 上下文
#if JUCE_MAC
  return local_error_;
#else
  if (owner_.isFloating())
    return local_error_;
  if (owner_.editor_ != nullptr)
    return owner_.editor_->GetMilkdropError();
  return "Editor not found";
#endif
}

int MilkdropModule::GLView::GetCurrentPresetIndex() const {
  // macOS：Editor GL 未启用，嵌入态也使用 GLView 本地 GL 上下文
#if JUCE_MAC
  const bool use_local = true;
#else
  const bool use_local = owner_.isFloating() || attached_;
#endif
  if (use_local) {
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
  // macOS：Editor GL 未启用，嵌入态也使用 GLView 本地 GL 上下文
#if JUCE_MAC
  return local_preset_paths_.size();
#else
  if (owner_.isFloating())
    return local_preset_paths_.size();
  if (owner_.editor_ != nullptr)
    return owner_.editor_->GetMilkdropTotalPresets();
  return 0;
#endif
}

juce::String MilkdropModule::GLView::GetCurrentPresetName() const {
  // macOS：Editor GL 未启用，嵌入态也使用 GLView 本地 GL 上下文
#if JUCE_MAC
  const bool use_local = true;
#else
  const bool use_local = owner_.isFloating();
#endif
  if (use_local) {
    if (local_current_preset_ >= 0 && local_current_preset_ < local_preset_paths_.size()) {
      return local_preset_paths_[local_current_preset_]
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

int64_t MilkdropModule::GLView::GetLastPresetSwitchTimeMs() const {
  // macOS：Editor GL 未启用，嵌入态也使用 GLView 本地 GL 上下文
#if JUCE_MAC
  return last_preset_switch_ms_;
#else
  if (owner_.isFloating())
    return last_preset_switch_ms_;
  if (owner_.editor_ != nullptr)
    return owner_.editor_->GetMilkdropLastPresetSwitchTimeMs();
  return 0;
#endif
}

void MilkdropModule::GLView::timerCallback() {
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
    if (isAutoMode_ && glView != nullptr && !isDraggingSlider_)
    {
        auto content = getContentBounds();
        auto topBar = content.withHeight(26);
        auto autoRow = getAutoRowBounds(topBar);
        auto sliderBounds = getSliderBounds(autoRow);
        if (sliderBounds.expanded(4).contains(e.getPosition()))
        {
            isDraggingSlider_ = true;
            if (!focused_)
                setFocusVisual(true);
            touchOverlayIdleTimer();
            float proportion = static_cast<float>(e.getPosition().x - sliderBounds.getX())
                               / static_cast<float>(sliderBounds.getWidth());
            updateAutoIntervalFromSlider(proportion);
            repaint(autoRow);
            glView->repaint();
            return;  // 不调用基类，避免 ModulePanel::mouseDown 启动标题栏/边缘拖拽
        }
        // ---- auto 行时间标签点击检测（弹出间隔输入对话框） ----
        if (cachedAutoTimeLabel_.contains(e.getPosition()))
        {
            if (!focused_)
                setFocusVisual(true);
            touchOverlayIdleTimer();
            showAutoIntervalDialog();
            return;
        }
    }

    // 基类处理：toFront + onBroughtToFront + 关闭按钮 + 缩放边缘 + 标题栏拖动
    // 所有涉及 private 成员的逻辑（closeButtonPressed / dragMode / detectEdge 等）
    // 均由基类完成，我们只在上层附加 overlay 按钮处理。
    ModulePanel::mouseDown(e);

    setFocusVisual(true);

    if (isPanelLayoutLocked(*this))
        return;

    // 内容区 overlay 按钮点击
    if (focused_ && glView != nullptr)
    {
        auto content = getContentBounds();
        auto overlay = getOverlayBounds(content);
        auto btn = hitTestOverlayButton(e.getPosition(), overlay);
        if (btn != OverlayButton::kNone)
        {
            pressedOverlayBtn_ = btn;
            touchOverlayIdleTimer();
            repaint(overlay);
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
        repaint();
        glView->repaint();
        return;
    }

    // 优先处理 overlay 按钮释放
    if (pressedOverlayBtn_ != OverlayButton::kNone)
    {
        auto content = getContentBounds();
        auto overlay = getOverlayBounds(content);
        auto hit = hitTestOverlayButton(e.getPosition(), overlay);
        if (hit == pressedOverlayBtn_)
            executeOverlayAction(hit);

        pressedOverlayBtn_ = OverlayButton::kNone;
        repaint(overlay);
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
        updateAutoIntervalFromSlider(proportion);
        repaint(autoRow);
        glView->repaint();
        return;
    }

    if (focused_ && glView != nullptr)
    {
        auto content = getContentBounds();
        auto overlay = getOverlayBounds(content);
        auto hit = hitTestOverlayButton(e.getPosition(), overlay);
        if (hit != hoveredOverlayBtn_)
        {
            hoveredOverlayBtn_ = hit;
            repaint(overlay);
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
        else if (isAutoMode_ && cachedAutoTimeLabel_.contains(e.getPosition()))
        {
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

void MilkdropModule::mouseDrag(const juce::MouseEvent& e)
{
    // slider 拖动中（mouseDrag 是 JUCE 专为拖拽设计的回调，比 mouseMove 更可靠
    // 地接收按下鼠标后的移动事件，尤其当组件树中存在原生 HWND 子窗口时）
    if (isDraggingSlider_)
    {
        auto content = getContentBounds();
        auto topBar = content.withHeight(26);
        auto autoRow = getAutoRowBounds(topBar);
        auto sliderBounds = getSliderBounds(autoRow);
        float proportion = static_cast<float>(e.getPosition().x - sliderBounds.getX())
                           / static_cast<float>(sliderBounds.getWidth());
        updateAutoIntervalFromSlider(proportion);
        repaint(autoRow);
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

juce::Rectangle<int> MilkdropModule::getOverlayBounds(juce::Rectangle<int> content) const
{
    constexpr int kBarHeight = 26;
    return content.withHeight(juce::jmin(kBarHeight, content.getHeight()));
}

MilkdropModule::OverlayButton MilkdropModule::hitTestOverlayButton(
    juce::Point<int> pos, juce::Rectangle<int> overlay) const
{
    if (! overlay.contains(pos))
        return OverlayButton::kNone;

    constexpr int kBtnSize = 22;
    constexpr int kPadding = 4;

    auto prevBtn   = juce::Rectangle<int>(overlay.getX() + kPadding, overlay.getY() + 2, kBtnSize, kBtnSize);
    auto randomBtn = juce::Rectangle<int>(overlay.getRight() - kPadding - kBtnSize, overlay.getY() + 2, kBtnSize, kBtnSize);
    auto nextBtn   = juce::Rectangle<int>(randomBtn.getX() - kPadding - kBtnSize, overlay.getY() + 2, kBtnSize, kBtnSize);
    auto autoBtn   = juce::Rectangle<int>(nextBtn.getX() - kPadding - kAutoBtnW, overlay.getY() + 2, kAutoBtnW, kBtnSize);
    auto resBtn    = juce::Rectangle<int>(autoBtn.getX() - kPadding - kResBtnW, overlay.getY() + 2, kResBtnW, kBtnSize);

    if (prevBtn.contains(pos))   return OverlayButton::kPrev;
    // 分辨率切换按钮已全平台阉割，不响应 kRenderScale 点击
#if 0
    if (resBtn.contains(pos))    return OverlayButton::kRenderScale;
#endif
    if (autoBtn.contains(pos))   return OverlayButton::kAuto;
    if (nextBtn.contains(pos))   return OverlayButton::kNext;
    if (randomBtn.contains(pos)) return OverlayButton::kRandom;

    // name area：覆盖 < 和 [1:1] 之间的空余区域
    if (cachedNameArea_.contains(pos))
        return OverlayButton::kPresetName;

    return OverlayButton::kNone;
}

juce::Rectangle<int> MilkdropModule::getOverlayButtonRect(
    juce::Rectangle<int> overlay, OverlayButton btn) const
{
    constexpr int kBtnSize = 22;
    constexpr int kPadding = 4;

    switch (btn)
    {
    case OverlayButton::kPrev:
        return { overlay.getX() + kPadding, overlay.getY() + 2, kBtnSize, kBtnSize };
    case OverlayButton::kRandom:
        return { overlay.getRight() - kPadding - kBtnSize, overlay.getY() + 2, kBtnSize, kBtnSize };
    case OverlayButton::kNext:
    {
        auto randomBtn = juce::Rectangle<int>(overlay.getRight() - kPadding - kBtnSize,
                                              overlay.getY() + 2, kBtnSize, kBtnSize);
        return { randomBtn.getX() - kPadding - kBtnSize, overlay.getY() + 2, kBtnSize, kBtnSize };
    }
    case OverlayButton::kAuto:
    {
        auto randomBtn = juce::Rectangle<int>(overlay.getRight() - kPadding - kBtnSize,
                                              overlay.getY() + 2, kBtnSize, kBtnSize);
        auto nextBtn   = juce::Rectangle<int>(randomBtn.getX() - kPadding - kBtnSize,
                                              overlay.getY() + 2, kBtnSize, kBtnSize);
        auto autoBtn   = juce::Rectangle<int>(nextBtn.getX() - kPadding - kAutoBtnW,
                                              overlay.getY() + 2, kAutoBtnW, kBtnSize);
        return autoBtn;
    }
    // 分辨率切换按钮已全平台阉割
#if 0
    case OverlayButton::kRenderScale:
    {
        auto randomBtn = juce::Rectangle<int>(overlay.getRight() - kPadding - kBtnSize,
                                              overlay.getY() + 2, kBtnSize, kBtnSize);
        auto nextBtn   = juce::Rectangle<int>(randomBtn.getX() - kPadding - kBtnSize,
                                              overlay.getY() + 2, kBtnSize, kBtnSize);
        auto autoBtn   = juce::Rectangle<int>(nextBtn.getX() - kPadding - kAutoBtnW,
                                              overlay.getY() + 2, kAutoBtnW, kBtnSize);
        auto resBtn    = juce::Rectangle<int>(autoBtn.getX() - kPadding - kResBtnW,
                                              overlay.getY() + 2, kResBtnW, kBtnSize);
        return resBtn;
    }
#endif
    default:
        return {};
    }
}

void MilkdropModule::executeOverlayAction(OverlayButton btn)
{
    switch (btn)
    {
    case OverlayButton::kPrev:   prevPreset();              break;
    case OverlayButton::kNext:   nextPreset();              break;
    case OverlayButton::kRandom: randomPreset();            break;
    case OverlayButton::kPresetName: showPresetJumpDialog();   break;
    case OverlayButton::kAuto:       toggleAutoMode();          break;
    // 分辨率切换按钮已全平台阉割
#if 0
    case OverlayButton::kRenderScale: glView->RequestRenderScale(); break;
#endif
    default: break;
    }
}

// ---- 叠加层绘制 ----

void MilkdropModule::paintOverlayControlBar(juce::Graphics& g, juce::Rectangle<int> content)
{
    constexpr int kBarHeight = 26;
    constexpr int kBtnSize   = 22;
    constexpr int kPadding   = 4;

    if (content.getHeight() < kBarHeight)
        return;

    auto bar = content.withHeight(kBarHeight);

    // 半透明暗底
    g.setColour(juce::Colour(0x00, 0x00, 0x00).withAlpha(0.78f));
    g.fillRect(bar);

    // 底部分割线（粉色）
    g.setColour(PinkXP::pink300.withAlpha(0.7f));
    g.fillRect(bar.getX(), bar.getBottom(), bar.getWidth(), 1);

    // 按钮位置: [<] nameArea [1:n] [auto] [>] [?]
    auto prevBtn   = juce::Rectangle<int>(bar.getX() + kPadding, bar.getY() + 2, kBtnSize, kBtnSize);
    auto randomBtn = juce::Rectangle<int>(bar.getRight() - kPadding - kBtnSize, bar.getY() + 2, kBtnSize, kBtnSize);
    auto nextBtn   = juce::Rectangle<int>(randomBtn.getX() - kPadding - kBtnSize, bar.getY() + 2, kBtnSize, kBtnSize);
    auto autoBtn   = juce::Rectangle<int>(nextBtn.getX() - kPadding - kAutoBtnW, bar.getY() + 2, kAutoBtnW, kBtnSize);
    auto resBtn    = juce::Rectangle<int>(autoBtn.getX() - kPadding - kResBtnW, bar.getY() + 2, kResBtnW, kBtnSize);
    // macOS：resBtn 不显示，nameArea 延伸到 autoBtn 左侧
#if JUCE_MAC
    auto nameArea  = juce::Rectangle<int>(prevBtn.getRight() + 2, bar.getY(),
                                          autoBtn.getX() - prevBtn.getRight() - 4, kBarHeight);
#else
    auto nameArea  = juce::Rectangle<int>(prevBtn.getRight() + 2, bar.getY(),
                                          resBtn.getX() - prevBtn.getRight() - 4, kBarHeight);
#endif

    // 按钮绘制 lambda
    auto drawBtn = [&](juce::Rectangle<int> r, const juce::String& text, OverlayButton btn)
    {
        bool hovered = (hoveredOverlayBtn_ == btn);
        bool pressed = (pressedOverlayBtn_ == btn);

        if (pressed)
            PinkXP::drawPressed(g, r, PinkXP::pink100);
        else if (hovered)
            PinkXP::drawRaised(g, r, PinkXP::pink200);
        else
        {
            g.setColour(juce::Colour(0xFF, 0xFF, 0xFF).withAlpha(0.2f));
            g.fillRect(r);
            g.setColour(PinkXP::pink300.withAlpha(0.55f));
            g.drawRect(r, 1);
        }

        g.setColour(juce::Colour(0xEE, 0xEE, 0xEE));
        g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
        g.drawText(text, r, juce::Justification::centred, false);
    };

    drawBtn(prevBtn,   "<",   OverlayButton::kPrev);
    drawBtn(nextBtn,   ">",   OverlayButton::kNext);
    drawBtn(randomBtn, "?",   OverlayButton::kRandom);

    // 渲染分辨率按钮 [1:n]：全平台阉割，不绘制此按钮
#if 0
    {
      int s = 1;
      if (isFloating()) {
        if (glView != nullptr)
          s = glView->GetLocalRenderScale();
      } else if (editor_ != nullptr) {
        s = editor_->GetMilkdropRenderScale();
      }
      juce::String label = juce::String("1:") + juce::String(s);
      drawBtn(resBtn, label, OverlayButton::kRenderScale);
    }
#endif

    // auto 按钮：轮播模式激活时用高亮 toggle 样式
    {
        bool hovered = (hoveredOverlayBtn_ == OverlayButton::kAuto);
        bool pressed = (pressedOverlayBtn_ == OverlayButton::kAuto);
        bool active  = isAutoMode_;

        if (pressed || active)
            PinkXP::drawPressed(g, autoBtn, PinkXP::pink100);
        else if (hovered)
            PinkXP::drawRaised(g, autoBtn, PinkXP::pink200);
        else
        {
            g.setColour(juce::Colour(0xFF, 0xFF, 0xFF).withAlpha(0.2f));
            g.fillRect(autoBtn);
            g.setColour(PinkXP::pink300.withAlpha(0.55f));
            g.drawRect(autoBtn, 1);
        }

        g.setColour(active ? PinkXP::pink300 : juce::Colour(0xDD, 0xDD, 0xDD));
        g.setFont(PinkXP::getFont(8.0f, juce::Font::bold));
        g.drawText("auto", autoBtn, juce::Justification::centred, false);
    }

    // 预设名：格式 "3/100  presetName"
    int idx = glView->GetCurrentPresetIndex();
    int total = glView->GetTotalPresetCount();
    juce::String presetDisplay;
    if (total > 0 && idx >= 0)
      presetDisplay = juce::String(idx + 1) + "/" + juce::String(total) + "  ";
    presetDisplay += glView->GetCurrentPresetName();
    if (presetDisplay.isEmpty())
      presetDisplay = "(no preset)";

    // 序号部分用粉色高亮，名称部分用白色
    juce::String idxPart = juce::String(idx + 1) + "/" + juce::String(total) + "  ";
    float idxW = PinkXP::getFont(9.0f, juce::Font::bold).getStringWidthFloat(idxPart) + 2.0f;

    auto idxRect = nameArea.withWidth(juce::jmin((int)idxW, nameArea.getWidth()));
    auto nameRect = nameArea.withTrimmedLeft(idxRect.getWidth());

    // 缓存 nameArea 供 hitTestOverlayButton 使用
    cachedNameArea_ = nameArea;

    // name area 交互视觉：hover 时底部淡粉线，pressed 时亮粉底色
    bool nameHovered = (hoveredOverlayBtn_ == OverlayButton::kPresetName);
    bool namePressed = (pressedOverlayBtn_ == OverlayButton::kPresetName);
    if (namePressed)
    {
        g.setColour(PinkXP::pink300.withAlpha(0.18f));
        g.fillRect(nameArea);
    }
    else if (nameHovered)
    {
        g.setColour(PinkXP::pink300.withAlpha(0.45f));
        g.fillRect(nameArea.getX(), nameArea.getBottom() - 1, nameArea.getWidth(), 1);
    }

    g.setColour(PinkXP::pink300.withAlpha(namePressed ? 1.0f : 0.95f));
    g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
    g.drawText(idxPart, idxRect, juce::Justification::centredLeft, true);

    g.setColour(juce::Colour(0xEE, 0xEE, 0xEE).withAlpha(0.95f));
    g.setFont(PinkXP::getFont(9.0f, juce::Font::plain));
    g.drawText(presetDisplay.substring(idxPart.length()), nameRect, juce::Justification::centredLeft, true);
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

  g.setColour(juce::Colour(0x00, 0x00, 0x00).withAlpha(alpha * 0.75f));
  g.fillRoundedRectangle(bar.toFloat(), 3.0f);

  g.setColour(PinkXP::pink300.withAlpha(alpha));
  g.setFont(PinkXP::getFont(8.0f, juce::Font::plain));
  g.drawText("Switching...", bar, juce::Justification::centred, false);
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

    editor_.setText(juce::String(current, 3));
    editor_.setFont(PinkXP::getFont(11.0f, juce::Font::plain));
    editor_.setColour(juce::TextEditor::backgroundColourId,
                      PinkXP::pink50);
    editor_.setColour(juce::TextEditor::textColourId, PinkXP::ink);
    editor_.setColour(juce::TextEditor::outlineColourId,
                      PinkXP::pink600.withAlpha(0.6f));
    editor_.setColour(juce::TextEditor::focusedOutlineColourId,
                      PinkXP::pink500.withAlpha(0.9f));
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

    // 面板底色跟随主题
    g.setColour(PinkXP::content.withAlpha(0.95f));
    g.fillRoundedRectangle(dlg.toFloat(), 4.0f);
    g.setColour(PinkXP::pink300.withAlpha(0.7f));
    g.drawRoundedRectangle(dlg.toFloat().reduced(0.5f), 4.0f, 1.5f);

    // 标题
    g.setColour(PinkXP::ink);
    g.setFont(PinkXP::getFont(11.0f, juce::Font::bold));
    g.drawText("Set Auto Interval",
               dlg.getX() + 14, dlg.getY() + 8,
               dlg.getWidth() - 28, 20,
               juce::Justification::centredLeft, false);

    // 提示文字
    g.setColour(PinkXP::pink700.withAlpha(0.75f));
    g.setFont(PinkXP::getFont(9.0f, juce::Font::plain));
    g.drawText("Enter interval (1.0-60.0 seconds):",
               dlg.getX() + 14, dlg.getY() + 28,
               dlg.getWidth() - 28, 18,
               juce::Justification::centredLeft, false);

    // OK 按钮（右侧）
    auto goRect = juce::Rectangle<int>(
        dlg.getRight() - 66, dlg.getBottom() - 34, 54, 22);
    PinkXP::drawRaised(g, goRect, PinkXP::btnFace);
    g.setColour(PinkXP::ink);
    g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
    g.drawText("OK", goRect, juce::Justification::centred, false);

    // Cancel 按钮
    auto cancelRect = juce::Rectangle<int>(
        goRect.getX() - 62, dlg.getBottom() - 34, 54, 22);
    PinkXP::drawRaised(g, cancelRect, PinkXP::btnFace);
    g.setColour(PinkXP::ink);
    g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
    g.drawText("Cancel", cancelRect, juce::Justification::centred, false);
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

    editor_.setText(juce::String(current + 1));
    editor_.setFont(PinkXP::getFont(11.0f, juce::Font::plain));
    editor_.setColour(juce::TextEditor::backgroundColourId,
                      PinkXP::pink50);
    editor_.setColour(juce::TextEditor::textColourId, PinkXP::ink);
    editor_.setColour(juce::TextEditor::outlineColourId,
                      PinkXP::pink600.withAlpha(0.6f));
    editor_.setColour(juce::TextEditor::focusedOutlineColourId,
                      PinkXP::pink500.withAlpha(0.9f));
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

    // 面板底色跟随主题
    g.setColour(PinkXP::content.withAlpha(0.95f));
    g.fillRoundedRectangle(dlg.toFloat(), 4.0f);
    g.setColour(PinkXP::pink300.withAlpha(0.7f));
    g.drawRoundedRectangle(dlg.toFloat().reduced(0.5f), 4.0f, 1.5f);

    // 标题
    g.setColour(PinkXP::ink);
    g.setFont(PinkXP::getFont(11.0f, juce::Font::bold));
    g.drawText("Jump to Preset",
               dlg.getX() + 14, dlg.getY() + 8,
               dlg.getWidth() - 28, 20,
               juce::Justification::centredLeft, false);

    // 提示文字
    g.setColour(PinkXP::pink700.withAlpha(0.75f));
    g.setFont(PinkXP::getFont(9.0f, juce::Font::plain));
    g.drawText("Enter preset number (1-" + juce::String(total_) + "):",
               dlg.getX() + 14, dlg.getY() + 28,
               dlg.getWidth() - 28, 18,
               juce::Justification::centredLeft, false);

    // Go 按钮（右侧）
    auto goRect = juce::Rectangle<int>(
        dlg.getRight() - 66, dlg.getBottom() - 34, 54, 22);
    PinkXP::drawRaised(g, goRect, PinkXP::btnFace);
    g.setColour(PinkXP::ink);
    g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
    g.drawText("Go", goRect, juce::Justification::centred, false);

    // Cancel 按钮
    auto cancelRect = juce::Rectangle<int>(
        goRect.getX() - 62, dlg.getBottom() - 34, 54, 22);
    PinkXP::drawRaised(g, cancelRect, PinkXP::btnFace);
    g.setColour(PinkXP::ink);
    g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
    g.drawText("Cancel", cancelRect, juce::Justification::centred, false);
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
    if (owner_.isAutoMode_)
        owner_.paintAutoControlRow(g, moduleTopBar);
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
    // - 若启用 auto 模式，额外向下扩展 kAutoRowHeight 供第二行绘制。
    auto contentLocal = GetContentLocalBounds();
    if (contentLocal.isEmpty())
    {
        if (overlayView_->isOnDesktop())
            overlayView_->removeFromDesktop();
        overlayView_->setVisible(false);
        return;
    }

    int barH = 26;
    if (isAutoMode_)
        barH += static_cast<int>(kAutoRowHeight);
    barH = juce::jmin(barH, contentLocal.getHeight());

    auto barLocal = contentLocal.withHeight(barH);
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
  isAutoMode_ = !isAutoMode_;
  if (isAutoMode_)
  {
    lastAutoSwitchTime_ = juce::Time::getMillisecondCounter();
  }
  // 重新布局并重绘
  layoutContent(getContentBounds());
  repaint();
  glView->repaint();
}

void MilkdropModule::checkAutoMode()
{
  if (!isAutoMode_)
    return;

  juce::uint32 now = juce::Time::getMillisecondCounter();
  juce::uint32 intervalMs = static_cast<juce::uint32>(autoIntervalSeconds_ * 1000.0f);
  if (now - lastAutoSwitchTime_ >= intervalMs)
  {
    lastAutoSwitchTime_ = now;
    randomPreset();
  }
}

void MilkdropModule::applyAutoInterval(float seconds)
{
  seconds = juce::jlimit(kMinAutoInterval, kMaxAutoInterval, seconds);
  // 四舍五入到 0.001
  seconds = std::round(seconds * 1000.0f) / 1000.0f;
  autoIntervalSeconds_ = seconds;
  // 用户确认间隔时始终重置计时器，从此刻起算经过完整间隔后执行第一次切换
  lastAutoSwitchTime_ = juce::Time::getMillisecondCounter();
  repaint();
  glView->repaint();
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
  }
}

juce::Rectangle<int> MilkdropModule::getAutoRowBounds(juce::Rectangle<int> topBar) const
{
  return juce::Rectangle<int>(topBar.getX(), topBar.getBottom(),
                              topBar.getWidth(), kAutoRowHeight);
}

juce::Rectangle<int> MilkdropModule::getSliderBounds(juce::Rectangle<int> autoRow) const
{
  constexpr int kSliderPadR = 44;
  constexpr int kSliderH = 8;
  // 布局: "Auto:"(x+6, 38px) + gap(4px) + slider
  int sliderX = autoRow.getX() + 6 + 38 + 4;
  int sliderW = autoRow.getWidth() - sliderX - kSliderPadR;
  return juce::Rectangle<int>(sliderX,
                              autoRow.getY() + (autoRow.getHeight() - kSliderH) / 2,
                              juce::jmax(20, sliderW), kSliderH);
}

void MilkdropModule::paintAutoControlRow(juce::Graphics& g, juce::Rectangle<int> topBar)
{
  auto autoRow = getAutoRowBounds(topBar);

  // 半透明暗底（比顶栏稍亮以区分层级）
  g.setColour(juce::Colour(0x00, 0x00, 0x00).withAlpha(0.72f));
  g.fillRect(autoRow);

  // 底部分割线
  g.setColour(PinkXP::pink300.withAlpha(0.5f));
  g.fillRect(autoRow.getX(), autoRow.getBottom(), autoRow.getWidth(), 1);

  // "Auto:" 标签（左侧）
  g.setColour(PinkXP::pink300.withAlpha(0.95f));
  g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
  g.drawText("Auto:", autoRow.getX() + 6, autoRow.getY(),
             38, autoRow.getHeight(), juce::Justification::centredLeft, false);

  // ---- Slider 轨道与滑块 ----
  auto sliderBounds = getSliderBounds(autoRow);
  float proportion = static_cast<float>(autoIntervalSeconds_ - kMinAutoInterval)
                     / static_cast<float>(kMaxAutoInterval - kMinAutoInterval);

  // 轨道底色
  g.setColour(juce::Colour(0xFF, 0xFF, 0xFF).withAlpha(0.18f));
  g.fillRoundedRectangle(sliderBounds.toFloat(), 2.0f);

  // 已填充部分
  int fillW = static_cast<int>(sliderBounds.getWidth() * proportion);
  if (fillW > 0)
  {
    g.setColour(PinkXP::pink300.withAlpha(0.7f));
    g.fillRoundedRectangle(
        juce::Rectangle<int>(sliderBounds.getX(), sliderBounds.getY(),
                             fillW, sliderBounds.getHeight()).toFloat(), 2.0f);
  }

  // 滑块手柄（粉色小方块）
  int knobX = sliderBounds.getX() + fillW - 4;
  int knobSize = 12;
  auto knobBounds = juce::Rectangle<int>(
      knobX, sliderBounds.getY() - (knobSize - sliderBounds.getHeight()) / 2,
      knobSize, knobSize);
  g.setColour(isDraggingSlider_ ? PinkXP::pink200 : PinkXP::pink100);
  g.fillRect(knobBounds);
  g.setColour(PinkXP::pink600);
  g.drawRect(knobBounds, 1);

  // 右侧时间标签（如 "10.000s"、"1m30.000s"）—— 可点击弹出输入对话框
  juce::String timeLabel;
  if (autoIntervalSeconds_ >= 60.0f)
  {
    int mins = static_cast<int>(autoIntervalSeconds_) / 60;
    float secs = std::fmod(autoIntervalSeconds_, 60.0f);
    timeLabel = juce::String(mins) + "m";
    if (secs > 0.05f)
      timeLabel += juce::String(secs, 3) + "s";
  }
  else
  {
    timeLabel = juce::String(autoIntervalSeconds_, 3) + "s";
  }

  auto timeLabelRect = juce::Rectangle<int>(
      sliderBounds.getRight() + 4, autoRow.getY(),
      40, autoRow.getHeight());
  cachedAutoTimeLabel_ = timeLabelRect;  // 供 mouseDown/mouseMove hit-test

  // hover 时微亮底色，提示可点击
  bool timeHovered = timeLabelRect.contains(
      getMouseXYRelative() - juce::Point<int>(0, 0));
  if (timeHovered)
  {
    g.setColour(juce::Colour(0xFF, 0xFF, 0xFF).withAlpha(0.08f));
    g.fillRoundedRectangle(timeLabelRect.toFloat().reduced(2, 4), 2.0f);
  }

  g.setColour(PinkXP::pink300.withAlpha(timeHovered ? 1.0f : 0.9f));
  g.setFont(PinkXP::getFont(8.0f, juce::Font::plain));
  g.drawText(timeLabel, timeLabelRect, juce::Justification::centredLeft, false);
}