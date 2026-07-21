/*
  ==============================================================================

  MilkdropModule.cpp
  Y2Kmeter — Milkdrop 模块（自 2.0.4 起，libprojectM 4 原生实现）

  参见 MilkdropModule.h 顶部注释，尤其是"架构概览"与"生命周期规则"两段。

  ==============================================================================
*/
#include "MilkdropModule.h"
#include "ProjectMApi.h"
#include "source/ui/PinkXPStyle.h"

#include "projectM-4/projectM.h"

#include <chrono>
#include <cmath>
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
    static std::string FixMilkdropShaderTypes(const std::string& data)
    {
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
    // 0x0 崩溃。因此运行时硬限 1 个实例（UI 层的“菜单置灰”
    // 只是前置防御；即便布局反序列化或拖拽复制插入了第二个
    // Milkdrop，此处的计数也会拒绝挂 projectM，换为兑底提示。
    std::atomic<int> gActiveProjectMInstances { 0 };
}

// ==========================================================
// MilkdropModule
// ==========================================================
MilkdropModule::MilkdropModule (AnalyserHub* hub_)
    : ModulePanel (ModuleType::milkdrop),
      hub (hub_)
{
    // 尝试激活 Hub 的 Oscilloscope 路径 —— 有 hub 才有 PCM 输入。
    if (hub != nullptr)
    {
        hub->retain (AnalyserHub::Kind::Oscilloscope);
        hub->addFrameListener (this);
        hubRetained = true;
    }

    // 构造 GLView 并挂到内容区。
    juce::Logger::writeToLog("[Milkdrop] Module ctor: creating GLView...");
    glView = std::make_unique<GLView> (*this);
    juce::Logger::writeToLog("[Milkdrop] Module ctor: GLView created, calling addAndMakeVisible...");
    addAndMakeVisible (glView.get());
    juce::Logger::writeToLog("[Milkdrop] Module ctor: done. GLView peer="
                             + juce::String(glView->getPeer() ? "exists" : "null")
                             + ", isShowing=" + juce::String(glView->isShowing() ? "true" : "false"));
}

MilkdropModule::~MilkdropModule()
{
    // 关键顺序：
    //   1) 显式 detach GL —— 同步等待 GL 线程收尾（destroy projectM handle）；
    //   2) 解除 hub 挂钩（保证在 detach 之后再解除，避免 GL 线程 render 中
    //      读到 pcmMutex 保护的数据被并发销毁）；
    if (glView != nullptr)
        glView->detachAndWait();

    if (hub != nullptr && hubRetained)
    {
        hub->removeFrameListener (this);
        hub->release (AnalyserHub::Kind::Oscilloscope);
        hubRetained = false;
    }

    glView.reset(); // 现在可以安全地销毁子组件
}

juce::ValueTree MilkdropModule::saveModuleSpecificState() const
{
  juce::ValueTree s("state");
  if (glView != nullptr)
  {
    int idx = glView->getCurrentPresetIndex();
    if (idx >= 0)
      s.setProperty("presetIndex", idx, nullptr);
  }
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
}

void MilkdropModule::paintContent (juce::Graphics& g, juce::Rectangle<int> content)
{
    // ---- Phase 1: 渲染主内容（GL 帧或兜底黑底） ----
    if (glView != nullptr && ! glView->getBounds().isEmpty())
    {
        static int paintLogCounter = 0;
        if (paintLogCounter < 5 || paintLogCounter % 120 == 0)
        {
            juce::Logger::writeToLog("[Milkdrop] paintContent #" + juce::String(paintLogCounter)
                                     + ": glView " + juce::String(glView->getWidth()) + "x" + juce::String(glView->getHeight())
                                     + ", initialized=" + juce::String(glView->isRenderInitialized() ? "true" : "false"));
        }
        ++paintLogCounter;

        if (glView->isRenderInitialized())
        {
          // projectM 已正常运行。ModulePanel::paint 已将整个区域填为底色，
          // 这里用 CPU 回读的 GL 帧覆盖内容区——渲染在底色之上。
          std::lock_guard<std::mutex> lock(glView->getGlFrameMutex());
          auto& frame = glView->getCachedGlFrame();
          if (frame.isValid() && frame.getWidth() > 0 && frame.getHeight() > 0)
            g.drawImage(frame, content.toFloat());
        }
        else
        {
          // GL 尚未就绪：显示黑色背景 + 诊断文字
          g.fillAll(juce::Colours::black);
          auto msg = glView->getRenderError().isEmpty()
                     ? juce::String("Milkdrop initializing...")
                     : juce::String("Milkdrop error: ")
                       + glView->getRenderError();
          g.setColour(juce::Colours::grey);
          g.setFont(juce::Font(12.0f));
          g.drawText(msg, content, juce::Justification::centred, false);
        }
    }
    else
    {
      // 兜底提示（GLView 尚未被布局或尺寸为 0）
      g.fillAll(juce::Colours::black);
      g.setColour(juce::Colours::grey);
      g.setFont(juce::Font(12.0f));
      g.drawText("Milkdrop initializing...", content, juce::Justification::centred, false);
    }

    // ---- Phase 2: 加载指示器（右下角，不依赖聚焦态） ----
    if (glView != nullptr && glView->isRenderInitialized())
      PaintLoadingIndicator(g, content);

    // ---- Phase 3: 聚焦时在所有内容之上绘制预设控制叠加条 ----
    if (focused_ && glView != nullptr)
      paintOverlayControlBar(g, content);
}

void MilkdropModule::layoutContent (juce::Rectangle<int> content)
{
    if (glView != nullptr)
        glView->setBounds (content);
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

    glView->pushPcm (tmp, (unsigned int) N);
    glView->triggerRepaint();
}

void MilkdropModule::nextPreset()
{
    if (glView != nullptr) glView->requestPresetDelta (+1);
}

void MilkdropModule::prevPreset()
{
    if (glView != nullptr) glView->requestPresetDelta (-1);
}

void MilkdropModule::randomPreset()
{
    if (glView != nullptr) glView->requestPresetRandom();
}

// ==========================================================
// GLView
// ==========================================================
MilkdropModule::GLView::GLView (MilkdropModule& owner_)
    : owner (owner_)
{
    // 重要：不在构造时立即 attachTo。
    // juce::OpenGLContext 必须在宿主 Component 已经进入 peer（窗口句柄层级）
    // 且 isShowing() == true 时才能安全 attach，否则内部 CachedImage 会在
    // 非消息线程上被回调 paint()，命中 juce_OpenGLContext.cpp:239 的
    // jassertfalse —— “添加模块就异常卡住”的元凶。
    //
    // 正确时机—— parentHierarchyChanged() / visibilityChanged() 里检测 isShowing()
    // 后再调用 attachIfNeeded()。

    // -------- 关键：明确要求 4.1 Core Profile --------
    // JUCE 默认的 defaultGLVersion 在 Windows 上走 legacy wglCreateContext，
    // driver 通常只回退到很旧的 compatibility profile（可能 1.x/2.x），
    // 这会让 projectM 4 内部 Sampler/Shader 构造访问空指针 -> 0xC0000005。
    // projectM 官方要求 ≥ OpenGL 3.3 Core；JUCE 提供的档位是 3.2/4.1/4.3，
    // 我们选 4.1（macOS 上限、桌面 GPU 普遍支持、projectMSDL 官方使用的版本）。
    // 这必须在 attachTo() 之前设置，attach 之后再调无效。
    glContext.setOpenGLVersionRequired (juce::OpenGLContext::openGL4_1);

    glContext.setRenderer (this);
    // 已由 setContinuousRepainting(true) 驱动：JUCE GL 线程会按 vsync 自己不断
    // 回调 renderOpenGL()，无需 PCM/UI 线程手动 triggerRepaint()。
    // 这使 projectM 预设在无音频输入时也能正常动画（它本身就能自行模
    // 拟音频包包的波形），而不依赖 AnalyserHub 的 onFrame 回调 tick。
    glContext.setContinuousRepainting (true);
    glContext.setSwapInterval (1);             // vsync
    glContext.setComponentPaintingEnabled (true); // CachedImage 合成管线；
                                                // paintContent 负责用 CPU 回读帧覆盖底色

    // 扫描预设目录（UI 线程；GL 线程之后只读 presetPaths）
    auto presetsDir = findAssetsDir ("milkdrop_presets");
    if (presetsDir.exists() && presetsDir.isDirectory())
    {
        auto files = presetsDir.findChildFiles (juce::File::findFiles,
                                                false,          // 非递归：预设已全部扁平化到根目录
                                                "*.milk");
        for (auto& f : files)
            presetPaths.add (f.getFullPathName());

        presetPaths.sort (false); // 稳定顺序：跨会话保持一致
    }

    // 随机化起始索引，让每个新加的模块从不同预设开始
    // 若后续 restoreModuleSpecificState 设置了存档索引，则 newOpenGLContextCreated
    // 会覆盖此值；若无存档，首次启动时从第 0 个开始。
    juce::Logger::writeToLog("[Milkdrop] GLView ctor done. presets="
                             + juce::String(presetPaths.size())
                             + ", attached=" + juce::String(glContext.isAttached() ? "true" : "false"));
}

MilkdropModule::GLView::~GLView()
{
    // 兜底：即便 owner 忘了 detachAndWait()，析构里再来一次也是安全的
    // （detach 会等 GL 线程收尾并触发 openGLContextClosing → destroy handle）。
    if (glContext.isAttached())
        glContext.detach();
}

void MilkdropModule::GLView::detachAndWait()
{
    if (glContext.isAttached())
        glContext.detach();  // 同步；返回时 openGLContextClosing 已执行完
}

void MilkdropModule::GLView::attachIfNeeded()
{
    // 仅在 UI/消息线程调用，且必须确保 Component 已经进入桌面窗口层级。
    // JUCE 的 OpenGLContext 会依赖宿主 Component 的 peer 建立 native GL surface；
    // 若在 addToDesktop 之前 attach，其内部 CachedImage 会在渲染线程 tick
    // 时试图回调 paint()，触发 juce_OpenGLContext.cpp:239 的 jassertfalse。
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());

    if (glContext.isAttached())
        return;

    if (! isShowing())      // 还未挂到桌面（父窗未 show / 未 addAndMakeVisible）就直接返回
        return;

    // 到这一步：宿主已进入 peer 且 visible —— 可安全 attach。
    glContext.attachTo (*this);
}

void MilkdropModule::GLView::scheduleAsyncAttach()
{
    // 已 attach → 无需再试。
    if (glContext.isAttached())
    {
        juce::Logger::writeToLog("[Milkdrop] scheduleAsyncAttach: already attached, skip");
        return;
    }

    // 递归深度限制：最多重试 60 次（每 callAsync ~16ms → ~1 秒上限）。
    // 超过上限说明宿主窗口永远不会 visible，放弃 attach。
    if (attachRetries >= kMaxAttachRetries)
    {
        renderErrorMessage = "GLView attach failed: component never became visible after "
                           + juce::String(kMaxAttachRetries).toStdString() + " retries.";
        juce::Logger::writeToLog("[Milkdrop] scheduleAsyncAttach: GAVE UP after "
                                 + juce::String(kMaxAttachRetries) + " retries. "
                                 + "isShowing=" + juce::String(isShowing() ? "true" : "false")
                                 + ", size=" + juce::String(getWidth()) + "x" + juce::String(getHeight()));
        return;
    }

    ++attachRetries;

    juce::Logger::writeToLog("[Milkdrop] scheduleAsyncAttach: retry="
                             + juce::String(attachRetries)
                             + ", isShowing=" + juce::String(isShowing() ? "true" : "false")
                             + ", size=" + juce::String(getWidth()) + "x" + juce::String(getHeight()));

    // 用 SafePointer + weak_ptr 风格确保回调时组件未销毁。
    juce::Component::SafePointer<GLView> weak(this);
    juce::MessageManager::callAsync([weak]
    {
        if (auto* self = weak.getComponent())
        {
            juce::Logger::writeToLog("[Milkdrop] callAsync cb: isShowing="
                                     + juce::String(self->isShowing() ? "true" : "false")
                                     + ", size=" + juce::String(self->getWidth()) + "x" + juce::String(self->getHeight())
                                     + ", attached=" + juce::String(self->glContext.isAttached() ? "true" : "false")
                                     + ", retries=" + juce::String(self->attachRetries));

            if (self->glContext.isAttached())
                return;

            if (self->isShowing() && self->getWidth() > 0 && self->getHeight() > 0)
            {
                juce::Logger::writeToLog("[Milkdrop] callAsync cb: conditions met, calling attachTo");

                // 重要：attachTo 返回并不意味着 GL 线程已就绪——
                // OpenGLContext 内部异步初始化 GL surface + 创建线程。
                // 我们通过 newOpenGLContextCreated() 回调确认就绪。
                self->glContext.attachTo(*self);

                juce::Logger::writeToLog("[Milkdrop] callAsync cb: attachTo returned, attached="
                                         + juce::String(self->glContext.isAttached() ? "true" : "false"));
            }
            else
            {
                juce::Logger::writeToLog("[Milkdrop] callAsync cb: conditions NOT met, will retry");
                self->scheduleAsyncAttach();  // 递归重试
            }
        }
        else
        {
            juce::Logger::writeToLog("[Milkdrop] callAsync cb: weak component is null (destroyed)");
        }
    });
}

void MilkdropModule::GLView::parentHierarchyChanged()
{
    juce::Logger::writeToLog("[Milkdrop] GLView::parentHierarchyChanged, "
                             "isShowing=" + juce::String(isShowing() ? "true" : "false")
                             + ", attached=" + juce::String(glContext.isAttached() ? "true" : "false"));

    // 不做 isShowing() 门控——scheduleAsyncAttach 内部会用 callAsync 推迟到
    // 消息循环末尾重新判断 isShowing()，此时 native peer 已完全建立。
    scheduleAsyncAttach();
}

void MilkdropModule::GLView::visibilityChanged()
{
    juce::Logger::writeToLog("[Milkdrop] GLView::visibilityChanged, "
                             "isShowing=" + juce::String(isShowing() ? "true" : "false"));

    scheduleAsyncAttach();
}

void MilkdropModule::GLView::resized()
{
    const int w = getWidth();
    const int h = getHeight();
    juce::Logger::writeToLog("[Milkdrop] GLView::resized() "
                             + juce::String(w) + "x" + juce::String(h)
                             + ", isShowing=" + juce::String(isShowing() ? "true" : "false")
                             + ", attached=" + juce::String(glContext.isAttached() ? "true" : "false")
                             + ", retries=" + juce::String(attachRetries));

    // 无条件尝试 attach（scheduleAsyncAttach 内部有递归重试 + 上限保护）。
    // 这里不做 isShowing() 预判：在"手动添加模块"场景下，resized() 触发时
    // 组件树的 native peer 可能尚未建立完毕，isShowing() 返回 false 是
    // 正常的——scheduleAsyncAttach 会通过自递归 callAsync 等待 peer 就绪。
    scheduleAsyncAttach();
}

// ---- GLView 鼠标事件全部转发给父组件 MilkdropModule -------------------
//  GLView 覆盖整个内容区，默认 juce::Component 会吞掉所有鼠标事件，
//  导致 MilkdropModule::mouseDown 永远收不到事件 → 焦点无法获取、
//  叠加层按钮无法点击。这里把所有鼠标事件用父组件坐标重发过去。

void MilkdropModule::GLView::mouseDown(const juce::MouseEvent& e)
{
    if (auto* parent = getParentComponent())
        parent->mouseDown(e.getEventRelativeTo(parent));
}

void MilkdropModule::GLView::mouseUp(const juce::MouseEvent& e)
{
    if (auto* parent = getParentComponent())
        parent->mouseUp(e.getEventRelativeTo(parent));
}

void MilkdropModule::GLView::mouseMove(const juce::MouseEvent& e)
{
    if (auto* parent = getParentComponent())
        parent->mouseMove(e.getEventRelativeTo(parent));
}

void MilkdropModule::GLView::mouseExit(const juce::MouseEvent& e)
{
    if (auto* parent = getParentComponent())
        parent->mouseExit(e.getEventRelativeTo(parent));
}

void MilkdropModule::GLView::mouseDrag(const juce::MouseEvent& e)
{
    if (auto* parent = getParentComponent())
        parent->mouseDrag(e.getEventRelativeTo(parent));
}

void MilkdropModule::GLView::triggerRepaint()
{
    // 若 continuousRepainting=false，需要 triggerRepaint() 才会重新调度 GL 线程。
    // 这个 API 是线程安全的（内部消息队列）。
    glContext.triggerRepaint();
}

void MilkdropModule::GLView::pushPcm (const float* interleavedLR, unsigned int frameCount)
{
    std::lock_guard<std::mutex> lock (pcmMutex);
    pendingPcm.assign (interleavedLR, interleavedLR + frameCount * 2);
    pendingFrames = frameCount;
}

// ---- Renderer 回调（均在 GL 线程） ----------------------------

void MilkdropModule::GLView::newOpenGLContextCreated()
{
    juce::Logger::writeToLog ("[MilkdropModule] newOpenGLContextCreated() begin, thread="
                              + juce::String::toHexString ((juce::pointer_sized_int) juce::Thread::getCurrentThreadId()));

    auto& api = projectm_api::Api::instance();
    if (! api.isAvailable())
    {
        renderErrorMessage = api.loadError();
        renderInitialized = false;
        juce::Logger::writeToLog ("[MilkdropModule] Api not available: " + juce::String (renderErrorMessage));
        return;
    }

    // ------------------------------------------------------------
    // 单实例防御：仅允许进程内一个 MilkdropModule 拥有 projectM handle。
    //   这里用 CAS（compare_exchange）而非 fetch_add，避免临时计数 > 1
    //   导致后续销毁时 fetch_sub 不匹配的问题（同时也避免了两个
    //   实例同时防护彼此的互斥环境下，B 拒绝后错误把 A 的计数减掉）。
    // ------------------------------------------------------------
    int expected = 0;
    if (! gActiveProjectMInstances.compare_exchange_strong (expected, 1))
    {
        renderErrorMessage = "Only 1 Milkdrop instance is allowed at a time (libprojectM Windows limitation).";
        renderInitialized = false;
        juce::Logger::writeToLog ("[MilkdropModule] CAS reject: another Milkdrop is active.");
        return;
    }

    juce::Logger::writeToLog ("[MilkdropModule] about to call projectm_create()");

    // 关键：projectM-4.1.x on Windows 内部使用 glew32.dll 提供 GL 扩展函数指针表，
    // 但 projectm_create() 自身不 glewInit()——它假设宿主已经 init 过。若不 init，
    // GLEW 全局指针表为 NULL，projectm_create 里第一次调 glGen*/glCompile* 就 0xC0000005。
    // 见 ProjectMApi::initGlew() 的详细注释。
    if (! api.initGlew())
    {
        renderErrorMessage = api.loadError();
        renderInitialized  = false;
        gActiveProjectMInstances.store (0);
        juce::Logger::writeToLog ("[MilkdropModule] initGlew() failed: " + juce::String (renderErrorMessage));
        return;
    }

    pmHandle = api.create();
    juce::Logger::writeToLog (juce::String ("[MilkdropModule] projectm_create returned handle=0x")
                              + juce::String::toHexString ((juce::pointer_sized_int) pmHandle));
    if (pmHandle == nullptr)
    {
        renderErrorMessage = "projectm_create() returned NULL (bad GL context or unsupported driver).";
        renderInitialized = false;
        // 释放名额，令下次新 MilkdropModule 可以尝试接手
        gActiveProjectMInstances.store (0);
        return;
    }

    // 基础参数
    api.setMeshSize        (pmHandle, kDefaultMeshWidth, kDefaultMeshHeight);
    api.setFps             (pmHandle, kTargetFps);
    api.setPresetDuration  (pmHandle, kPresetDuration);
    api.setSoftCutDuration (pmHandle, kSoftCutDuration);
    api.setHardCutEnabled  (pmHandle, false);

    // 纹理搜索路径（.milk 预设里 sampler 引用的纹理文件从这里加载）
    auto texDir = findAssetsDir ("milkdrop_textures");
    if (texDir.exists() && texDir.isDirectory())
    {
        std::vector<std::string> paths { texDir.getFullPathName().toStdString() };
        api.setTextureSearchPaths (pmHandle, paths);
    }

    // 初始窗口尺寸（若已 layout 完成）
    const int w = juce::jmax (1, desiredWidth.load());
    const int h = juce::jmax (1, desiredHeight.load());
    api.setWindowSize (pmHandle, (std::size_t) w, (std::size_t) h);
    lastAppliedWidth  = w;
    lastAppliedHeight = h;

    // 加载起始预设：优先从布局存档恢复，否则从第 0 个开始
    if (owner.restored_preset_index_ >= 0
        && owner.restored_preset_index_ < presetPaths.size())
    {
      currentPresetIndex = owner.restored_preset_index_;
    }
    else if (!presetPaths.isEmpty())
    {
      // 无存档时从精选预设池中随机选取（显示号 58/65/70/72/74/76/78/79）
      constexpr int kDefaultPool[] = {57, 64, 69, 71, 73, 75, 77, 78};
      constexpr int kPoolSize = static_cast<int>(sizeof(kDefaultPool) / sizeof(kDefaultPool[0]));
      std::mt19937 rng{static_cast<uint32_t>(
          std::chrono::high_resolution_clock::now().time_since_epoch().count())};
      currentPresetIndex = kDefaultPool[rng() % kPoolSize];
    }
    owner.restored_preset_index_ = -1;  // 消费一次即清空
    loadPresetInternal();

    renderInitialized = true;
    startTimerHz(30);  // UI 线程每 ~33ms 调用 paintContent 消费 cachedGlFrame_
    frameCounter = 0;   // context 重建后日志采样重新对齐
    juce::Logger::writeToLog ("[MilkdropModule] newOpenGLContextCreated() done, initialized=true, w="
                              + juce::String (w) + ", h=" + juce::String (h)
                              + ", presets=" + juce::String (presetPaths.size()));
}

void MilkdropModule::GLView::renderOpenGL()
{
    // 密一点的日志：前 5 帧 + 每 300 帧一次，既能确认新建上下文后
    // renderOpenGL 很快重新启动，又不会偷走 60fps 主环。
    ++frameCounter;
    if (frameCounter <= 5 || frameCounter % 300 == 0)
    {
        juce::Logger::writeToLog ("[MilkdropModule] renderOpenGL tick #"
                                  + juce::String (frameCounter)
                                  + ", pmHandle=0x"
                                  + juce::String::toHexString ((juce::pointer_sized_int) pmHandle)
                                  + ", size=" + juce::String (getWidth()) + "x" + juce::String (getHeight())
                                  + ", initialized=" + (renderInitialized ? "true" : "false"));
    }

    if (! renderInitialized || pmHandle == nullptr)
    {
        // 兑底：GL 清屏成黑色，避免暴露未初始化的 framebuffer 内容。
        juce::gl::glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
        juce::gl::glClear (juce::gl::GL_COLOR_BUFFER_BIT);
        return;
    }

    auto& api = projectm_api::Api::instance();

    // 1) 尺寸同步（Component 尺寸随时可能改变）
    const int cw = getWidth();
    const int ch = getHeight();
    if (cw > 0 && ch > 0)
    {
        desiredWidth  = cw;
        desiredHeight = ch;

        // JUCE 的 GL context 会自动设置 viewport 匹配 Component 尺寸，
        // 但 projectM 内部有自己的 FBO 尺寸——必须显式通知它。
        if (cw != lastAppliedWidth || ch != lastAppliedHeight)
        {
            api.setWindowSize (pmHandle, (std::size_t) cw, (std::size_t) ch);
            lastAppliedWidth  = cw;
            lastAppliedHeight = ch;
        }
    }

    // 2) 消费预设切换请求（UI 线程通过 requestedPresetDelta / requestedPresetRandom 传递）
    int delta = requestedPresetDelta.exchange (0);
    bool random = requestedPresetRandom.exchange (false);
    if (! presetPaths.isEmpty())
    {
        if (random)
        {
            std::mt19937 rng { (uint32_t) std::chrono::high_resolution_clock::now()
                                   .time_since_epoch().count() };
            currentPresetIndex = (int) (rng() % (juce::uint32) presetPaths.size());
            last_preset_switch_time_ms_.store(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            loadPresetInternal();
        }
        else if (delta != 0)
        {
            currentPresetIndex = (currentPresetIndex + delta + presetPaths.size())
                                 % presetPaths.size();
            last_preset_switch_time_ms_.store(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            loadPresetInternal();
        }
    }

    // 3) 推 PCM 到 projectM。有真实音频时消费并备份；无新数据时复播上一帧
    //    的真实 PCM。只有从未收到过音频时才用合成兜底（冷启动）。
    {
        std::lock_guard<std::mutex> lock(pcmMutex);
        if (pendingFrames > 0 && !pendingPcm.empty()) {
          api.addPcmFloat(pmHandle, pendingPcm.data(), pendingFrames, true);
          // 备份为复播源
          lastRealPcm = pendingPcm;
          lastRealFrames = pendingFrames;
          hasEverReceivedRealPcm = true;
          pendingFrames = 0;
        } else if (hasEverReceivedRealPcm) {
          // 无新数据 → 复播上一帧真实 PCM，保持频谱连续
          api.addPcmFloat(pmHandle, lastRealPcm.data(), lastRealFrames, true);
        } else {
          // 冷启动：从未收到过音频，合成低幅度多频波形防止首帧黑屏
          constexpr unsigned int kSynthFrames = 256;
          float synth[kSynthFrames * 2];
          const double t0 = static_cast<double>(frameCounter)
              * (kSynthFrames / 44100.0);
          for (unsigned int i = 0; i < kSynthFrames; ++i) {
            const double t = t0 + static_cast<double>(i) / 44100.0;
            const float s =
                0.20f *
                    static_cast<float>(
                        std::sin(2.0 * juce::MathConstants<double>::pi *
                                 220.0 * t)) +
                0.10f *
                    static_cast<float>(
                        std::sin(2.0 * juce::MathConstants<double>::pi * 55.0 *
                                 t));
            synth[i * 2 + 0] = s;
            synth[i * 2 + 1] = s;
          }
          api.addPcmFloat(pmHandle, synth, kSynthFrames, true);
        }
    }

    // 4) 让 projectM 出一帧
    //
    // 关键：JUCE 在调用 renderOpenGL 之前会把它的 CachedImage FBO
    // 绑到 GL_FRAMEBUFFER（Windows 上非 0），期望我们把最终画面写入
    // 这个 FBO；bufferSwapper.swap() 再把它送到 HWND。
    //
    // 但 projectM 4 内部渲染最终帧时会 glBindFramebuffer(GL_FRAMEBUFFER, 0)，
    // 把画面写到了 “系统默认 FBO”——JUCE 拿不到，屏幕就永远是黑的。
    //
    // projectM 4.2.0+ 提供 projectm_opengl_render_frame_fbo(handle, fbo)，
    // 让调用方指定输出 FBO。我们查询 GL 当前 draw framebuffer binding，
    // 把这个 ID 传给 projectM。
    if (api.hasOpenglRenderFrameFbo())
    {
        GLint currentDrawFbo = 0;
        juce::gl::glGetIntegerv (juce::gl::GL_DRAW_FRAMEBUFFER_BINDING, &currentDrawFbo);

        if (frameCounter <= 3)
        {
            juce::Logger::writeToLog ("[MilkdropModule] using render_frame_fbo, JUCE draw FBO="
                                      + juce::String ((int) currentDrawFbo));
        }

        api.openglRenderFrameFbo (pmHandle, (uint32_t) currentDrawFbo);
        juce::gl::glBindFramebuffer (juce::gl::GL_FRAMEBUFFER, (GLuint) currentDrawFbo);
    }
    else
    {
      // projectM 内部强制 glBindFramebuffer(0) 渲染到默认 framebuffer。
      // 不尝试任何 FBO→FBO blit（跨驱动不可靠）。改为 glReadPixels
      // 从 FBO 0 回读到 CPU 像素缓冲，交由 paintContent 用 JUCE Graphics
      // 绘制到内容区——GL 帧在 ModulePanel 底色之上，不会被覆盖。

      // 保存 JUCE 的当前 draw FBO（CachedImage），projectM 之后再恢复
      GLint juceDrawFbo = 0;
      juce::gl::glGetIntegerv(juce::gl::GL_DRAW_FRAMEBUFFER_BINDING, &juceDrawFbo);

      juce::gl::glBindFramebuffer(juce::gl::GL_FRAMEBUFFER, 0);
      api.openglRenderFrame(pmHandle);

      // 回读像素（FBO 0 → CPU buffer → cachedGlFrame_），支持降分辨率读取
      {
        const int scale = static_cast<int>(readback_scale_.load());
        const int rw = cw / scale;
        const int rh = ch / scale;
        const int rwClamped = juce::jmax(1, rw);
        const int rhClamped = juce::jmax(1, rh);

        std::lock_guard<std::mutex> lock(glFrameMutex_);
        if (cachedGlFrame_.getWidth() != rwClamped
            || cachedGlFrame_.getHeight() != rhClamped)
          cachedGlFrame_ = juce::Image(juce::Image::ARGB, rwClamped, rhClamped, true);

        // glReadPixels 不支持子采样，必须按全分辨率 cw×ch 读取
        std::vector<uint8_t> pixels(
            static_cast<size_t>(cw * ch * 4));
        juce::gl::glReadBuffer(juce::gl::GL_BACK);
        juce::gl::glReadPixels(0, 0, (GLsizei) cw, (GLsizei) ch,
                               juce::gl::GL_RGBA, juce::gl::GL_UNSIGNED_BYTE,
                               pixels.data());

        // Y轴翻转 + 降采样：从全分辨率向上取行，跳过 scale-1 行
        juce::Image::BitmapData bd(cachedGlFrame_, juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < rhClamped; ++y)
        {
          const int srcY = ch - 1 - (y * scale + scale / 2);
          const uint8_t* srcRow = pixels.data()
              + static_cast<size_t>(srcY * cw * 4);
          for (int x = 0; x < rwClamped; ++x)
          {
            const int srcX = x * scale + scale / 2;
            const int srcIdx = srcX * 4;
            *reinterpret_cast<uint32_t*>(bd.getPixelPointer(x, y)) =
                (static_cast<uint32_t>(srcRow[srcIdx + 3]) << 24)
                | (static_cast<uint32_t>(srcRow[srcIdx])     << 16)
                | (static_cast<uint32_t>(srcRow[srcIdx + 1]) << 8)
                | static_cast<uint32_t>(srcRow[srcIdx + 2]);
          }
        }
      }

      // 恢复 JUCE CachedImage FBO
      juce::gl::glBindFramebuffer(juce::gl::GL_FRAMEBUFFER, (GLuint) juceDrawFbo);
    }

    // 5) continuousRepainting=true 已由 JUCE GL 线程自持续驱动，
    //    onFrame 送 PCM 只负责“数据侧”，无需再手动 triggerRepaint。
    //    hub 断线时 renderOpenGL 仍会以 vsync 频率被调用，但推入合成 PCM，
    //    projectM 也会保持动画不冻结。
}

void MilkdropModule::GLView::openGLContextClosing()
{
    juce::Logger::writeToLog ("[MilkdropModule] openGLContextClosing() begin, pmHandle=0x"
                              + juce::String::toHexString ((juce::pointer_sized_int) pmHandle));
    if (pmHandle != nullptr)
    {
        auto& api = projectm_api::Api::instance();
        api.destroy (pmHandle);
        pmHandle = nullptr;

        // 关键：清空 projectM DLL 内的全局状态（尤其是 GLEW 函数指针表）。
        // libprojectM 4 在 Windows 上使用 GLEW，函数指针表是 DLL 全局。
        // 若不 reload，下一次 MilkdropModule 在新的 wgl 上下文里 create()
        // 时会继续使用上一次残留的函数指针 —— 这些指针在新上下文里可能
        // 无效 —— 触发 0xC0000005 DEP violation at 0x0。
        //
        // 通过 FreeLibrary + LoadLibrary 强制 DLL 内所有静态/全局重新初始化，
        // 保证下一次 GLEW 会重新 wglGetProcAddress 到当前上下文里合法的函数指针。
        api.reload();

        // 释放全局名额，令后续新添加的 Milkdrop 可以接管。
        gActiveProjectMInstances.store (0);
    }
    stopTimer();
    renderInitialized = false;
    juce::Logger::writeToLog ("[MilkdropModule] openGLContextClosing() done");
}

void MilkdropModule::GLView::timerCallback()
{
  // GL 线程在 renderOpenGL 中以 ~60fps 更新 cachedGlFrame_，
  // 本 Timer 在 UI 线程以 ~30Hz 唤醒 owner 的重绘管线，
  // 使 paintContent 消费最新帧并覆盖 ModulePanel 底色。
  if (isRenderInitialized())
    owner.repaint();
}

// ---- 私有辅助 -------------------------------------------------

void MilkdropModule::GLView::loadPresetInternal()
{
    if (pmHandle == nullptr) return;
    if (currentPresetIndex < 0 || currentPresetIndex >= presetPaths.size()) return;

    auto& api = projectm_api::Api::instance();
    auto path = presetPaths[currentPresetIndex];

    // 如果 DLL 提供 load_preset_data，走"内存修正"路径：读取 .milk → 修正
    // Milkdrop DSL 与 GLSL 之间的类型不兼容问题 → 从内存加载。
    // 如果不提供（老旧 DLL），回退到传统 loadPresetFile。
    if (api.hasLoadPresetData())
    {
      juce::File file(path);
      if (file.existsAsFile())
      {
        auto raw = file.loadFileAsString().toStdString();
        auto fixed = FixMilkdropShaderTypes(raw);
        juce::Logger::writeToLog("[Milkdrop] loadPresetInternal: using memory path (loadPresetData), idx="
                                 + juce::String(currentPresetIndex) + " file=" + path);
        api.loadPresetData(pmHandle, fixed, true /*smooth*/);
      }
    }
    else
    {
      juce::Logger::writeToLog("[Milkdrop] loadPresetInternal: using file path (loadPresetFile), idx="
                               + juce::String(currentPresetIndex) + " file=" + path);
      api.loadPresetFile(pmHandle, path.toStdString(), true /*smooth*/);
    }

    // 同步预设索引到 UI 线程可读的 atomic，供 paintContent 显示预设名
    currentPresetIndexUi_.store(currentPresetIndex);
}

juce::File MilkdropModule::GLView::findAssetsDir (const juce::String& subdir)
{
    // 查找顺序，从"部署产物"到"源码开发"依次兜底：
    //   1) 当前可执行文件同目录  ← Standalone/EXE
    //   2) 当前动态库同目录       ← VST3 bundle
    //   3) 源码树 assets/<subdir> ← IDE 直接跑，无 Post-build 拷贝
    juce::Array<juce::File> candidates;

    auto addAndParentUp = [&] (juce::File start, int levels)
    {
        auto cur = start;
        for (int i = 0; i < levels && cur.exists(); ++i)
        {
            candidates.add (cur.getChildFile (subdir));
            cur = cur.getParentDirectory();
        }
    };

    addAndParentUp (juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory(), 1);
    addAndParentUp (juce::File::getSpecialLocation (juce::File::currentApplicationFile).getParentDirectory(), 1);
    addAndParentUp (juce::File::getSpecialLocation (juce::File::hostApplicationPath).getParentDirectory(), 1);

    // 源码兜底：从 exe 向上遍历，找 "assets/<subdir>"
    auto up = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
    for (int i = 0; i < 8 && up.exists(); ++i)
    {
        candidates.add (up.getChildFile ("assets/" + subdir));
        up = up.getParentDirectory();
    }

    for (auto& c : candidates)
        if (c.exists() && c.isDirectory())
            return c;

    return {};
}

// ==========================================================
// MilkdropModule —— 焦点与叠加层交互
// ==========================================================
void MilkdropModule::setFocusVisual(bool shouldFocus)
{
    if (focused_ == shouldFocus)
        return;

    focused_ = shouldFocus;
    if (! focused_)
    {
        hoveredOverlayBtn_ = OverlayButton::kNone;
        pressedOverlayBtn_ = OverlayButton::kNone;
    }

    repaint();
}

void MilkdropModule::mouseDown(const juce::MouseEvent& e)
{
    // 右键 → 冒泡给 workspace 弹出"添加模块"菜单
    if (e.mods.isPopupMenu())
    {
        if (onRightClick)
            onRightClick(*this, e.getPosition());
        return;
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
            repaint(overlay);
        }
    }
}

void MilkdropModule::mouseUp(const juce::MouseEvent& e)
{
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
        return;
    }

    // 否则走基类（关闭按钮释放、拖拽/缩放收尾）
    ModulePanel::mouseUp(e);
}

void MilkdropModule::mouseMove(const juce::MouseEvent& e)
{
    if (focused_ && glView != nullptr)
    {
        auto content = getContentBounds();
        auto overlay = getOverlayBounds(content);
        auto hit = hitTestOverlayButton(e.getPosition(), overlay);
        if (hit != hoveredOverlayBtn_)
        {
            hoveredOverlayBtn_ = hit;
            repaint(overlay);
        }

        if (hit != OverlayButton::kNone)
            setMouseCursor(juce::MouseCursor::PointingHandCursor);
        else if (overlay.contains(e.getPosition()))
            setMouseCursor(juce::MouseCursor::NormalCursor);
        else
            ModulePanel::mouseMove(e); // 基类处理边缘光标
    }
    else
    {
        ModulePanel::mouseMove(e);
    }
}

void MilkdropModule::mouseExit(const juce::MouseEvent& e)
{
    if (hoveredOverlayBtn_ != OverlayButton::kNone)
    {
        hoveredOverlayBtn_ = OverlayButton::kNone;
        repaint();
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
    constexpr int kResBtnW = 30;
    constexpr int kPadding = 4;

    auto prevBtn   = juce::Rectangle<int>(overlay.getX() + kPadding, overlay.getY() + 2, kBtnSize, kBtnSize);
    auto randomBtn = juce::Rectangle<int>(overlay.getRight() - kPadding - kBtnSize, overlay.getY() + 2, kBtnSize, kBtnSize);
    auto nextBtn   = juce::Rectangle<int>(randomBtn.getX() - kPadding - kBtnSize, overlay.getY() + 2, kBtnSize, kBtnSize);
    auto resBtn    = juce::Rectangle<int>(nextBtn.getX() - kPadding - kResBtnW, overlay.getY() + 2, kResBtnW, kBtnSize);

    if (prevBtn.contains(pos))   return OverlayButton::kPrev;
    if (resBtn.contains(pos))    return OverlayButton::kRes;
    if (nextBtn.contains(pos))   return OverlayButton::kNext;
    if (randomBtn.contains(pos)) return OverlayButton::kRandom;

    return OverlayButton::kNone;
}

juce::Rectangle<int> MilkdropModule::getOverlayButtonRect(
    juce::Rectangle<int> overlay, OverlayButton btn) const
{
    constexpr int kBtnSize = 22;
    constexpr int kResBtnW = 30;
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
    case OverlayButton::kRes:
    {
        auto randomBtn = juce::Rectangle<int>(overlay.getRight() - kPadding - kBtnSize,
                                              overlay.getY() + 2, kBtnSize, kBtnSize);
        auto nextBtn   = juce::Rectangle<int>(randomBtn.getX() - kPadding - kBtnSize,
                                              overlay.getY() + 2, kBtnSize, kBtnSize);
        return { nextBtn.getX() - kPadding - kResBtnW, overlay.getY() + 2, kResBtnW, kBtnSize };
    }
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
    case OverlayButton::kRes:    glView->cycleReadbackScale(); break;
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

    // 按钮位置: [<] nameArea [1:1] [>] [?]
    auto prevBtn   = juce::Rectangle<int>(bar.getX() + kPadding, bar.getY() + 2, kBtnSize, kBtnSize);
    auto randomBtn = juce::Rectangle<int>(bar.getRight() - kPadding - kBtnSize, bar.getY() + 2, kBtnSize, kBtnSize);
    auto nextBtn   = juce::Rectangle<int>(randomBtn.getX() - kPadding - kBtnSize, bar.getY() + 2, kBtnSize, kBtnSize);
    auto resBtn    = juce::Rectangle<int>(nextBtn.getX() - kPadding - 30, bar.getY() + 2, 30, kBtnSize);
    auto nameArea  = juce::Rectangle<int>(prevBtn.getRight() + 2, bar.getY(),
                                          resBtn.getX() - prevBtn.getRight() - 4, kBarHeight);

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
            g.setColour(juce::Colour(0xFF, 0xFF, 0xFF).withAlpha(0.12f));
            g.fillRect(r);
            g.setColour(PinkXP::pink300.withAlpha(0.4f));
            g.drawRect(r, 1);
        }

        g.setColour(PinkXP::ink);
        g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
        g.drawText(text, r, juce::Justification::centred, false);
    };

    drawBtn(prevBtn,   "<",   OverlayButton::kPrev);
    drawBtn(resBtn,    glView->getReadbackScaleLabel(), OverlayButton::kRes);
    drawBtn(nextBtn,   ">",   OverlayButton::kNext);
    drawBtn(randomBtn, "?",   OverlayButton::kRandom);

    // 预设名：格式 "3/100  presetName"
    int idx = glView->getCurrentPresetIndex();
    int total = glView->getTotalPresetCount();
    juce::String presetDisplay;
    if (total > 0 && idx >= 0)
      presetDisplay = juce::String(idx + 1) + "/" + juce::String(total) + "  ";
    presetDisplay += glView->getCurrentPresetName();
    if (presetDisplay.isEmpty())
      presetDisplay = "(no preset)";

    // 序号部分用粉色高亮，名称部分用白色
    juce::String idxPart = juce::String(idx + 1) + "/" + juce::String(total) + "  ";
    float idxW = PinkXP::getFont(9.0f, juce::Font::bold).getStringWidthFloat(idxPart) + 2.0f;

    auto idxRect = nameArea.withWidth(juce::jmin((int)idxW, nameArea.getWidth()));
    auto nameRect = nameArea.withTrimmedLeft(idxRect.getWidth());

    g.setColour(PinkXP::pink300.withAlpha(0.85f));
    g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
    g.drawText(idxPart, idxRect, juce::Justification::centredLeft, true);

    g.setColour(juce::Colour(0xFF, 0xFF, 0xFF).withAlpha(0.9f));
    g.setFont(PinkXP::getFont(9.0f, juce::Font::plain));
    g.drawText(presetDisplay.substring(idxPart.length()), nameRect, juce::Justification::centredLeft, true);
}

// ---- 加载指示器绘制 ----

void MilkdropModule::PaintLoadingIndicator(juce::Graphics& g, juce::Rectangle<int> content)
{
  // projectM soft-cut 过渡在 1-2 秒内完成，指示器只需短暂提示"正在切换"，
  // 不应延长到过渡结束之后。连续点击会不断重置时间戳、保持指示器可见。
  constexpr int64_t kIndicatorDurationMs = 1200;

  if (glView == nullptr)
    return;

  int64_t last_switch = glView->getLastPresetSwitchTimeMs();
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