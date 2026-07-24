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
    // 0x0 崩溃。因此运行时硬限 1 个实例（UI 层的"菜单置灰"
    // 只是前置防御；即便布局反序列化或拖拽复制插入了第二个
    // Milkdrop，此处的计数也会拒绝挂 projectM，换为兑底提示。
    std::atomic<int> gActiveProjectMInstances { 0 };

    // 用于 showPresetJumpDialog：enterModalState 是非阻塞的（立即返回），
    // 不能在其后直接 setVisible(true)。此类作为 ModalComponentManager::Callback
    // 在对话框真正退出模态状态时才恢复 GLView 的可见性。
    struct GlViewRestorer : juce::ModalComponentManager::Callback
    {
        explicit GlViewRestorer(juce::Component& v) : view(v) {}
        void modalStateFinished(int) override { view.setVisible(true); }
        juce::Component& view;
    };
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

    glView = std::make_unique<GLView> (*this);
    addAndMakeVisible (glView.get());
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

void MilkdropModule::paintContent (juce::Graphics& g, juce::Rectangle<int> content)
{
    // Phase 1: 显示 GL 帧。Triple-buffer 无锁读取：
    //   Producer（GL 线程 renderOpenGL）写入 frameSlots_，
    //   Consumer（本线程 paintContent）通过 getLatestFrame() 无锁获取最新就绪帧。
    if (glView != nullptr && !glView->getBounds().isEmpty())
    {
        if (glView->isRenderInitialized())
        {
          auto& frame = glView->getLatestFrame();
          if (frame.isValid() && frame.getWidth() > 0 && frame.getHeight() > 0)
            g.drawImage(frame, glView->getBounds().toFloat());
        }
        else
        {
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
      g.fillAll(juce::Colours::black);
      g.setColour(juce::Colours::grey);
      g.setFont(juce::Font(12.0f));
      g.drawText("Milkdrop initializing...", content, juce::Justification::centred, false);
    }

    // ---- Phase 2: 加载指示器（右下角，不依赖聚焦态） ----
    if (glView != nullptr && glView->isRenderInitialized())
      PaintLoadingIndicator(g, content);

    // ---- Phase 3: 聚焦时在顶部 GDI 控制栏区域绘制预设控制叠加条 ----
    //   GLView 上方留出固定空间，此区域无 GL 原生窗口覆盖，GDI 控制栏可见。
    if (focused_ && glView != nullptr)
    {
      auto topBar = content.withHeight(26);
      paintOverlayControlBar(g, topBar);

      // Phase 3b: auto 模式下在顶栏下方绘制自动轮播控制行
      if (isAutoMode_)
        paintAutoControlRow(g, topBar);
    }
}

void MilkdropModule::layoutContent (juce::Rectangle<int> content)
{
    if (glView != nullptr)
    {
        // GLView 始终占满整个内容区；叠加控制栏通过 paintContent 中的 GDI 绘制
        // 覆盖在 GL 帧之上，不挤压 GLView 显示区，避免 resize 导致的白闪。
        glView->setBounds(content);

        // 定位 auto 间隔输入框（悬浮在内容区上方，由 paintAutoControlRow 负责绘制背景）
        if (autoIntervalEditor_ != nullptr && isAutoMode_)
        {
            auto topBar = content.withHeight(26);
            auto autoRow = getAutoRowBounds(topBar);
            constexpr int kEditorW = 36;
            constexpr int kEditorH = 20;
            // "Auto:" 标签从 x+6 起宽 38px，编辑器紧跟其后
            autoIntervalEditor_->setBounds(
                autoRow.getX() + 48, autoRow.getY() + (autoRow.getHeight() - kEditorH) / 2,
                kEditorW, kEditorH);
        }
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

void MilkdropModule::jumpToPresetIndex(int index)
{
    if (glView != nullptr) glView->requestPresetJump(index);
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
    // jassertfalse —— "添加模块就异常卡住"的元凶。
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

// v2.2.1 直连 GL + Editor GL 合成架构：
    //   componentPaintingEnabled(false) → GL context 直接挂到 GLView 原生 HWND，
    //   projectM 渲染到 FBO 0，SwapBuffers GPU 直出（~50fps）。
    //   Z-order 由 Editor 级 OpenGL 上下文（setComponentPaintingEnabled(true)）
    //   保障——主窗口 GPU 合成管线内所有组件自然正确层叠，无需手动 Z-order 处理。
    glContext.setComponentPaintingEnabled(false);

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
}

MilkdropModule::GLView::~GLView()
{
    // 置位析构标志，阻止任何 pending callAsync 回调继续触发
    // scheduleAsyncAttach / attachIfNeeded / parentHierarchyChanged 等路径。
    // 这防止 ~GLView 里 glContext.detach() 与异步 attach 操作竞态导致卡死。
    destroying_.store(true);

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
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());

    if (glContext.isAttached())
        return;

    if (!isShowing() || getPeer() == nullptr)
        return;

    // componentPaintingEnabled(false)：GL context 直接挂到 GLView 原生 HWND。
    // projectM 渲染到此 HWND 的 FBO 0，SwapBuffers GPU 直出。
    // Z-order 由 Editor 级 OpenGL 上下文保障：主窗口 GPU 合成管线内自然正确。
    glContext.attachTo(*this);
}

void MilkdropModule::GLView::scheduleAsyncAttach()
{
    // 已 attach → 无需再试。
    if (glContext.isAttached())
        return;

    // 析构期间禁止异步 attach：此时 ~GLView 正在等待 detach 收尾，
    // 任何 callAsync 回调里的 attachTo 都会与 detach 形成竞态导致卡死。
    if (destroying_.load())
        return;

    // 递归深度限制：最多重试 60 次（每 callAsync ~16ms → ~1 秒上限）。
    // 超过上限说明宿主窗口永远不会 visible，放弃 attach。
    if (attachRetries >= kMaxAttachRetries)
    {
        renderErrorMessage = "GLView attach failed: component never became visible after "
                           + juce::String(kMaxAttachRetries).toStdString() + " retries.";
        return;
    }

    ++attachRetries;

    // 用 SafePointer + weak_ptr 风格确保回调时组件未销毁。
    juce::Component::SafePointer<GLView> weak(this);
    juce::MessageManager::callAsync([weak]
    {
        if (auto* self = weak.getComponent())
        {
            // 第二道防线：即使 SafePointer 仍有效（基类 ~Component 尚未执行），
            // ~GLView 已设置 destroying_，此时绝不能再尝试 attach。
            if (self->destroying_.load())
                return;

            if (self->glContext.isAttached())
                return;

            if (self->isShowing() && self->getWidth() > 0 && self->getHeight() > 0
                && self->getPeer() != nullptr)
            {
                self->glContext.attachTo(*self);
            }
            else
            {
                self->scheduleAsyncAttach();
            }
        }
    });
}

void MilkdropModule::GLView::parentHierarchyChanged()
{
    // 不做 isShowing() 门控——scheduleAsyncAttach 内部会用 callAsync 推迟到
    // 消息循环末尾重新判断 isShowing()，此时 native peer 已完全建立。
    scheduleAsyncAttach();
}

void MilkdropModule::GLView::visibilityChanged()
{
    scheduleAsyncAttach();
}

void MilkdropModule::GLView::resized()
{
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
    auto& api = projectm_api::Api::instance();
    if (! api.isAvailable())
    {
        renderErrorMessage = api.loadError();
        renderInitialized = false;
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
        return;
    }

    // 关键：projectM-4.1.x on Windows 内部使用 glew32.dll 提供 GL 扩展函数指针表，
    // 但 projectm_create() 自身不 glewInit()——它假设宿主已经 init 过。若不 init，
    // GLEW 全局指针表为 NULL，projectm_create 里第一次调 glGen*/glCompile* 就 0xC0000005。
    // 见 ProjectMApi::initGlew() 的详细注释。
    if (! api.initGlew())
    {
        renderErrorMessage = api.loadError();
        renderInitialized  = false;
        gActiveProjectMInstances.store (0);
        return;
    }

    pmHandle = api.create();
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
    startTimerHz(30);  // UI 线程 ~33ms 驱动 repaint，刷新 Overlay 控制栏与轮播

    // 初始化双缓冲 PBO（用于异步 glReadPixels，消除 GPU 管线停顿）
    // 使用 viewport 物理像素尺寸，由 paintContent drawImage 做 bilinear 缩放
    const size_t pboSize = static_cast<size_t>(w * h * 4);
    juce::gl::glGenBuffers(2, pboIds_);
    for (int i = 0; i < 2; ++i)
    {
        juce::gl::glBindBuffer(juce::gl::GL_PIXEL_PACK_BUFFER, pboIds_[i]);
        juce::gl::glBufferData(juce::gl::GL_PIXEL_PACK_BUFFER, pboSize, nullptr,
                               juce::gl::GL_STREAM_READ);
    }
    juce::gl::glBindBuffer(juce::gl::GL_PIXEL_PACK_BUFFER, 0);
    hasPboData_ = false;
    pboWriteIdx_ = 0;
}

void MilkdropModule::GLView::renderOpenGL()
{
    if (! renderInitialized || pmHandle == nullptr)
    {
        // 兑底：GL 清屏成黑色，避免暴露未初始化的 framebuffer 内容。
        juce::gl::glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
        juce::gl::glClear (juce::gl::GL_COLOR_BUFFER_BIT);
        return;
    }

    auto& api = projectm_api::Api::instance();

    // 1) 尺寸同步（Component 尺寸随时可能改变）
    GLint viewport[4];
    juce::gl::glGetIntegerv(juce::gl::GL_VIEWPORT, viewport);
    const int pw = juce::jmax(1, viewport[2]);
    const int ph = juce::jmax(1, viewport[3]);

    if (pw > 0 && ph > 0)
    {
        desiredWidth  = pw;
        desiredHeight = ph;

        // JUCE 的 GL context 会自动设置 viewport 匹配 Component 尺寸，
        // 但 projectM 内部有自己的 FBO 尺寸——必须显式通知它。
        if (pw != lastAppliedWidth || ph != lastAppliedHeight)
        {
            api.setWindowSize(pmHandle, (std::size_t)pw, (std::size_t)ph);
            lastAppliedWidth  = pw;
            lastAppliedHeight = ph;
        }
    }

    // 2) 消费预设切换请求（UI 线程通过 requestedPresetJump / requestedPresetDelta /
    //    requestedPresetRandom 传递）。优先级：跳跃 > 随机 > 增量。
    int jumpIdx = requestedPresetJump.exchange (-1);
    int delta   = requestedPresetDelta.exchange (0);
    bool random = requestedPresetRandom.exchange (false);
    if (! presetPaths.isEmpty())
    {
        if (jumpIdx >= 0)
        {
            currentPresetIndex = juce::jlimit (0, presetPaths.size() - 1, jumpIdx);
            last_preset_switch_time_ms_.store(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            loadPresetInternal();
        }
        else if (random)
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
          const double t0 = 0.0;
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
    if (api.hasOpenglRenderFrameFbo())
    {
        GLint currentDrawFbo = 0;
        juce::gl::glGetIntegerv(juce::gl::GL_DRAW_FRAMEBUFFER_BINDING, &currentDrawFbo);
        api.openglRenderFrameFbo(pmHandle, (uint32_t)currentDrawFbo);
        juce::gl::glBindFramebuffer(juce::gl::GL_FRAMEBUFFER, (GLuint)currentDrawFbo);
    }
    else
    {
        juce::gl::glBindFramebuffer(juce::gl::GL_FRAMEBUFFER, 0);
        api.openglRenderFrame(pmHandle);
    }

    // 5) 统一回读像素（FBO 0 → PBO → triple-buffer frameSlots_）。
    //    Producer（本 GL 线程）：PBO 异步回读 → 写入下一个空闲 slot → 原子发布。
    //    Consumer（Editor GL 线程 paintContent）：getLatestFrame() 无锁读取。
    //    3 个 slot 确保 producer 从不阻塞。
    if (pw > 0 && ph > 0)
    {
        const size_t bufSize = static_cast<size_t>(pw * ph * 4);

        // 5a) PBO 尺寸变更时重建（仅在窗口 resize 时发生）
        if (pboIds_[0] != 0)
        {
            GLint curSize = 0;
            juce::gl::glBindBuffer(juce::gl::GL_PIXEL_PACK_BUFFER, pboIds_[0]);
            juce::gl::glGetBufferParameteriv(juce::gl::GL_PIXEL_PACK_BUFFER,
                                              juce::gl::GL_BUFFER_SIZE, &curSize);
            if (static_cast<size_t>(curSize) != bufSize)
            {
                juce::gl::glBufferData(juce::gl::GL_PIXEL_PACK_BUFFER, bufSize, nullptr,
                                       juce::gl::GL_STREAM_READ);
                juce::gl::glBindBuffer(juce::gl::GL_PIXEL_PACK_BUFFER, pboIds_[1]);
                juce::gl::glBufferData(juce::gl::GL_PIXEL_PACK_BUFFER, bufSize, nullptr,
                                       juce::gl::GL_STREAM_READ);
            }
            juce::gl::glBindBuffer(juce::gl::GL_PIXEL_PACK_BUFFER, 0);
        }

        // 5b) 异步 glReadPixels → PBO（不阻塞 GPU 管线）
        juce::gl::glBindFramebuffer(juce::gl::GL_FRAMEBUFFER, 0);
        juce::gl::glReadBuffer(juce::gl::GL_BACK);
        if (pboIds_[pboWriteIdx_] != 0)
        {
            juce::gl::glBindBuffer(juce::gl::GL_PIXEL_PACK_BUFFER, pboIds_[pboWriteIdx_]);
            juce::gl::glReadPixels(0, 0, (GLsizei)pw, (GLsizei)ph,
                                   juce::gl::GL_RGBA, juce::gl::GL_UNSIGNED_BYTE,
                                   nullptr);
            juce::gl::glBindBuffer(juce::gl::GL_PIXEL_PACK_BUFFER, 0);
        }

        // 5c) 读取上一帧的 PBO（此时 DMA 传输已完毕）→ 临时缓冲
        const int readIdx = (pboWriteIdx_ + 1) % 2;
        std::vector<uint8_t> tempPixels;
        if (hasPboData_ && pboIds_[readIdx] != 0)
        {
            juce::gl::glBindBuffer(juce::gl::GL_PIXEL_PACK_BUFFER, pboIds_[readIdx]);
            const uint8_t* src = static_cast<const uint8_t*>(
                juce::gl::glMapBuffer(juce::gl::GL_PIXEL_PACK_BUFFER, juce::gl::GL_READ_ONLY));
            if (src != nullptr)
            {
                tempPixels.assign(src, src + bufSize);
                juce::gl::glUnmapBuffer(juce::gl::GL_PIXEL_PACK_BUFFER);
            }
            juce::gl::glBindBuffer(juce::gl::GL_PIXEL_PACK_BUFFER, 0);
        }
        else
        {
            // 冷启动首帧：退化为同步 glReadPixels
            tempPixels.resize(bufSize);
            juce::gl::glReadPixels(0, 0, (GLsizei)pw, (GLsizei)ph,
                                   juce::gl::GL_RGBA, juce::gl::GL_UNSIGNED_BYTE,
                                   tempPixels.data());
            hasPboData_ = true;
        }

        pboWriteIdx_ = readIdx;

        // 5d) Triple-buffer 无锁发布 —— 写入下一个 slot，原子通知 consumer。
        if (!tempPixels.empty())
        {
            FrameSlot& slot = frameSlots_[producerSlot_];

            // 确保 slot 的 Image 尺寸正确（预分配，避免 resize 时 D2D clear）
            if (slot.image.getWidth() != pw || slot.image.getHeight() != ph)
                slot.image = juce::Image(juce::Image::ARGB, pw, ph, false);

            // Y 轴翻转（OpenGL bottom-up → Image top-down）+ RGBA→ARGB 转换
            {
                juce::Image::BitmapData bd(slot.image, juce::Image::BitmapData::writeOnly);
                for (int y = 0; y < ph; ++y)
                {
                    const int srcY = ph - 1 - y;
                    const uint8_t* srcRow = tempPixels.data()
                        + static_cast<size_t>(srcY * pw * 4);
                    for (int x = 0; x < pw; ++x)
                    {
                        const int s = x * 4;
                        *reinterpret_cast<uint32_t*>(bd.getPixelPointer(x, y)) =
                            (static_cast<uint32_t>(srcRow[s + 3]) << 24)
                            | (static_cast<uint32_t>(srcRow[s])     << 16)
                            | (static_cast<uint32_t>(srcRow[s + 1]) << 8)
                            | static_cast<uint32_t>(srcRow[s + 2]);
                    }
                }
            }

            // 发布：先设 ready，再更新索引（确保 consumer 读到 ready 为 true 时数据完整）
            slot.ready.store(true, std::memory_order_release);
            latestReadySlot_.store(producerSlot_, std::memory_order_release);

            // 标记两帧前的 slot 为非就绪（triple-buffer：3个slot，写指针循环）
            const int freeSlot = (producerSlot_ + 2) % 3;
            frameSlots_[freeSlot].ready.store(false, std::memory_order_relaxed);

            producerSlot_ = (producerSlot_ + 1) % 3;
        }
    }
}

void MilkdropModule::GLView::openGLContextClosing()
{
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

    // 清理 PBO
    if (pboIds_[0] != 0)
    {
        juce::gl::glDeleteBuffers(2, pboIds_);
        pboIds_[0] = pboIds_[1] = 0;
    }
    hasPboData_ = false;
}

void MilkdropModule::GLView::timerCallback()
{
  // GLView 使用 componentPaintingEnabled(false)，projectM 帧由原生 HWND
  // 通过 SwapBuffers 直接显示。本 Timer（UI 线程 ~30Hz）驱动 repaint
  // 以刷新 Overlay 控制栏、auto-hide 与 auto 轮播。

  if (!isRenderInitialized())
    return;

  // -------- 首次自动激活焦点（仅一次） --------
  // focused_ 默认为 false。在 render 就绪后自动激活一次以展示预设名等控件。
  // 之后不再自动激活——若 auto-hide 清除焦点或用户点击了其他模块，不再恢复。
  // 这避免了 auto 轮播模式下 overhead 控件反复闪现的问题。
  if (!first_focus_done_)
  {
    first_focus_done_ = true;
    if (!owner.focused_)
    {
      owner.setFocusVisual(true);
      owner.touchOverlayIdleTimer();
    }
  }

  // -------- Auto-hide 检测（UI 线程安全） --------
  owner.checkOverlayAutoHide();

  // -------- Auto 轮播切换检测（UI 线程安全） --------
  owner.checkAutoMode();

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
        api.loadPresetData(pmHandle, fixed, true /*smooth*/);
      }
    }
    else
    {
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
    if (!focused_)
    {
        hoveredOverlayBtn_ = OverlayButton::kNone;
        pressedOverlayBtn_ = OverlayButton::kNone;
        // overlay 隐藏时同步隐藏 auto 行 TextEditor
        if (autoIntervalEditor_ != nullptr && isAutoMode_)
        {
            autoIntervalEditor_->giveAwayKeyboardFocus();
            autoIntervalEditor_->setVisible(false);
        }
    }
    else
    {
        touchOverlayIdleTimer();  // 聚焦时重置 4 秒倒计时
        // overlay 显示时恢复 auto 行 TextEditor
        if (autoIntervalEditor_ != nullptr && isAutoMode_)
            autoIntervalEditor_->setVisible(true);
    }

    repaint();

    // 叠加控制栏覆盖在 GLView 之上渲染，不再挤压 GLView 尺寸，
    // 因此切换焦点时只需重绘，无需重新布局。
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
    // 右键 → 冒泡给 workspace 弹出"添加模块"菜单
    if (e.mods.isPopupMenu())
    {
        if (onRightClick)
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
            return;  // 不调用基类，避免 ModulePanel::mouseDown 启动标题栏/边缘拖拽
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
        }

        if (hit != OverlayButton::kNone)
        {
            touchOverlayIdleTimer();
            if (hit == OverlayButton::kPresetName)
                setMouseCursor(juce::MouseCursor::IBeamCursor);
            else
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

    if (prevBtn.contains(pos))   return OverlayButton::kPrev;
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
        return { nextBtn.getX() - kPadding - kAutoBtnW, overlay.getY() + 2, kAutoBtnW, kBtnSize };
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
    case OverlayButton::kPresetName: showPresetJumpDialog();   break;
    case OverlayButton::kAuto:       toggleAutoMode();          break;
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

    // 按钮位置: [<] nameArea [auto] [>] [?]
    auto prevBtn   = juce::Rectangle<int>(bar.getX() + kPadding, bar.getY() + 2, kBtnSize, kBtnSize);
    auto randomBtn = juce::Rectangle<int>(bar.getRight() - kPadding - kBtnSize, bar.getY() + 2, kBtnSize, kBtnSize);
    auto nextBtn   = juce::Rectangle<int>(randomBtn.getX() - kPadding - kBtnSize, bar.getY() + 2, kBtnSize, kBtnSize);
    auto autoBtn   = juce::Rectangle<int>(nextBtn.getX() - kPadding - kAutoBtnW, bar.getY() + 2, kAutoBtnW, kBtnSize);
    auto nameArea  = juce::Rectangle<int>(prevBtn.getRight() + 2, bar.getY(),
                                          autoBtn.getX() - prevBtn.getRight() - 4, kBarHeight);

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
    drawBtn(nextBtn,   ">",   OverlayButton::kNext);
    drawBtn(randomBtn, "?",   OverlayButton::kRandom);

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
            g.setColour(juce::Colour(0xFF, 0xFF, 0xFF).withAlpha(0.12f));
            g.fillRect(autoBtn);
            g.setColour(PinkXP::pink300.withAlpha(0.4f));
            g.drawRect(autoBtn, 1);
        }

        g.setColour(active ? PinkXP::pink300 : PinkXP::ink);
        g.setFont(PinkXP::getFont(8.0f, juce::Font::bold));
        g.drawText("auto", autoBtn, juce::Justification::centred, false);
    }

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

    g.setColour(PinkXP::pink300.withAlpha(namePressed ? 1.0f : 0.85f));
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

// ==========================================================
// PresetJumpDialog：自定义 PinkXP 风格预设跳转对话框
// ==========================================================
MilkdropModule::PresetJumpDialog::PresetJumpDialog(
    MilkdropModule& owner_, int total, int current,
    std::function<void(int)> onResult)
    : owner_(owner_), total_(total), onResult_(std::move(onResult))
{
    setOpaque(false);

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
    editor_.onReturnKey = [this] {
        juce::String input = editor_.getText().trim();
        int val = input.getIntValue();
        if (val < 1) val = 1;
        if (val > total_) val = total_;
        onResult_(val - 1);
        exitModalState(1);
    };
    editor_.onEscapeKey = [this] {
        exitModalState(0);
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
    if (goRect.contains(getMouseXYRelative()))
    {
        juce::String input = editor_.getText().trim();
        int val = input.getIntValue();
        if (val < 1) val = 1;
        if (val > total_) val = total_;
        onResult_(val - 1);
        exitModalState(1);
        return;
    }

    // Cancel 按钮区域
    auto cancelRect = juce::Rectangle<int>(
        goRect.getX() - 62, dlgY + kDlgH - 34, 54, 22);
    if (cancelRect.contains(getMouseXYRelative()))
    {
        exitModalState(0);
        return;
    }
}

void MilkdropModule::showPresetJumpDialog()
{
    if (glView == nullptr)
        return;

    int total = glView->getTotalPresetCount();
    if (total <= 0)
        return;

    int current = glView->getCurrentPresetIndex();
    if (current < 0) current = 0;

    auto* dlg = new PresetJumpDialog(*this, total, current,
        [this](int result) {
            if (result >= 0)
                jumpToPresetIndex(result);
        });
    dlg->setBounds(getLocalBounds());
    addAndMakeVisible(dlg);

    // componentPaintingEnabled(false) 使 GLView 拥有独立原生 HWND 子窗口，
    // Z-order 高于 JUCE 普通 Component。模态对话框无法覆盖此原生窗口，
    // 导致对话框不可见且鼠标事件被 GL 窗口捕获（界面卡死）。
    // 解决方案：弹窗前隐藏 GLView，通过 ModalComponentManager::Callback
    // 在对话框真正退出模态状态时才恢复 GLView 可见性。
    // 注意：enterModalState 是**非阻塞**的，不能在其后直接 setVisible(true)。
    glView->setVisible(false);
    dlg->enterModalState(true, new GlViewRestorer(*glView), true);
}

// ==========================================================
// Auto 轮播模式
// ==========================================================

void MilkdropModule::ensureAutoIntervalEditor()
{
  if (autoIntervalEditor_ != nullptr)
    return;

  autoIntervalEditor_ = std::make_unique<juce::TextEditor>();
  autoIntervalEditor_->setInputRestrictions(5, "0123456789.");
  autoIntervalEditor_->setFont(PinkXP::getFont(9.0f, juce::Font::plain));
  autoIntervalEditor_->setColour(juce::TextEditor::backgroundColourId,
                                 PinkXP::pink50);
  autoIntervalEditor_->setColour(juce::TextEditor::textColourId, PinkXP::ink);
  autoIntervalEditor_->setColour(juce::TextEditor::outlineColourId,
                                 PinkXP::pink600.withAlpha(0.6f));
  autoIntervalEditor_->setColour(juce::TextEditor::focusedOutlineColourId,
                                 PinkXP::pink500.withAlpha(0.9f));
  autoIntervalEditor_->setText(juce::String(autoIntervalSeconds_, 1));
  autoIntervalEditor_->onReturnKey = [this] {
    float val = autoIntervalEditor_->getText().getFloatValue();
    applyAutoInterval(val);
  };
  autoIntervalEditor_->onEscapeKey = [this] {
    autoIntervalEditor_->setText(juce::String(autoIntervalSeconds_, 1), false);
    autoIntervalEditor_->giveAwayKeyboardFocus();
  };
  autoIntervalEditor_->onFocusLost = [this] {
    float val = autoIntervalEditor_->getText().getFloatValue();
    applyAutoInterval(val);
  };
  autoIntervalEditor_->setVisible(false);
  addAndMakeVisible(autoIntervalEditor_.get());
}

void MilkdropModule::toggleAutoMode()
{
  isAutoMode_ = !isAutoMode_;
  if (isAutoMode_)
  {
    ensureAutoIntervalEditor();
    lastAutoSwitchTime_ = juce::Time::getMillisecondCounter();
    autoIntervalEditor_->setText(juce::String(autoIntervalSeconds_, 1));
    autoIntervalEditor_->setVisible(focused_);
  }
  else
  {
    if (autoIntervalEditor_ != nullptr)
    {
      autoIntervalEditor_->giveAwayKeyboardFocus();
      autoIntervalEditor_->setVisible(false);
    }
  }
  // 重新布局以调整 GLView 尺寸和 editor 位置
  layoutContent(getContentBounds());
  repaint();
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
  // 四舍五入到 0.1
  seconds = std::round(seconds * 10.0f) / 10.0f;
  if (seconds != autoIntervalSeconds_)
  {
    autoIntervalSeconds_ = seconds;
    lastAutoSwitchTime_ = juce::Time::getMillisecondCounter();
  }
  if (autoIntervalEditor_ != nullptr)
    autoIntervalEditor_->setText(juce::String(autoIntervalSeconds_, 1), false);
  repaint();
}

void MilkdropModule::updateAutoIntervalFromSlider(float proportion)
{
  proportion = juce::jlimit(0.0f, 1.0f, proportion);
  float seconds = kMinAutoInterval
                  + proportion * (kMaxAutoInterval - kMinAutoInterval);
  seconds = juce::jlimit(kMinAutoInterval, kMaxAutoInterval, seconds);
  // 四舍五入到 0.1
  seconds = std::round(seconds * 10.0f) / 10.0f;

  if (seconds != autoIntervalSeconds_)
  {
    autoIntervalSeconds_ = seconds;
    if (autoIntervalEditor_ != nullptr)
      autoIntervalEditor_->setText(juce::String(seconds, 1), false);
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
  // 布局: "Auto:"(x+6, 38px) + gap(4px) + editor(36px) + gap(8px) + slider
  int sliderX = autoRow.getX() + 6 + 38 + 4 + 36 + 8;
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
  g.setColour(PinkXP::pink300.withAlpha(0.85f));
  g.setFont(PinkXP::getFont(9.0f, juce::Font::bold));
  g.drawText("Auto:", autoRow.getX() + 6, autoRow.getY(),
             38, autoRow.getHeight(), juce::Justification::centredLeft, false);

  // ---- Slider 轨道与滑块 ----
  auto sliderBounds = getSliderBounds(autoRow);
  float proportion = static_cast<float>(autoIntervalSeconds_ - kMinAutoInterval)
                     / static_cast<float>(kMaxAutoInterval - kMinAutoInterval);

  // 轨道底色
  g.setColour(juce::Colour(0xFF, 0xFF, 0xFF).withAlpha(0.1f));
  g.fillRoundedRectangle(sliderBounds.toFloat(), 2.0f);

  // 已填充部分
  int fillW = static_cast<int>(sliderBounds.getWidth() * proportion);
  if (fillW > 0)
  {
    g.setColour(PinkXP::pink300.withAlpha(0.55f));
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

  // 右侧时间标签（如 "10.0s"、"1m30.0s"）
  juce::String timeLabel;
  if (autoIntervalSeconds_ >= 60.0f)
  {
    int mins = static_cast<int>(autoIntervalSeconds_) / 60;
    float secs = std::fmod(autoIntervalSeconds_, 60.0f);
    timeLabel = juce::String(mins) + "m";
    if (secs > 0.05f)
      timeLabel += juce::String(secs, 1) + "s";
  }
  else
  {
    timeLabel = juce::String(autoIntervalSeconds_, 1) + "s";
  }

  g.setColour(PinkXP::pink300.withAlpha(0.75f));
  g.setFont(PinkXP::getFont(8.0f, juce::Font::plain));
  g.drawText(timeLabel,
             sliderBounds.getRight() + 4, autoRow.getY(),
             40, autoRow.getHeight(),
             juce::Justification::centredLeft, false);
}