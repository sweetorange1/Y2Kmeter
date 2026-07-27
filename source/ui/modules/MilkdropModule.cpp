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
#include "PluginEditor.h"

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
        // detachAndWait removed: no GL context;

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
    int idx = glView->GetCurrentPresetIndex();
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

juce::Rectangle<int> MilkdropModule::GetContentLocalBounds() const {
  return getContentBounds();
}

void MilkdropModule::paint(juce::Graphics& g) {
  // Editor::renderOpenGL 已经将 projectM 帧渲染到 Editor CachedImage FBO 中
  // 本模块内容区屏幕坐标对应的区域。这里绘制卡片外壳（边框、标题栏、关闭按钮），
  // 内容区保持透明以保留 GPU 渲染的 projectM 帧。
  const auto bounds = getLocalBounds();

  // 1. 像素凸起窗口边框（仅边框，不填充内容区 — 保留 projectM 帧）
  PinkXP::drawRaised(g, bounds, juce::Colours::transparentBlack);

  // 2. 玫瑰粉标题栏
  auto tb = getTitleBarBounds();
  PinkXP::drawPinkTitleBar(g, tb, titleText, 12.0f);

  // 标题栏下沿深色分割线
  g.setColour(PinkXP::dark);
  g.fillRect(tb.getX(), tb.getBottom(), tb.getWidth(), 1);

  // 3. 关闭按钮（×）—— 始终画默认状态；hover/press 由基类鼠标事件处理，仅影响 button 的 hit area 逻辑
  auto cb = getCloseButtonBounds();
  PinkXP::drawRaised(g, cb, PinkXP::btnFace);
  g.setColour(PinkXP::ink);
  g.setFont(PinkXP::getFont(11.0f, juce::Font::bold));
  auto cbText = cb;
  cbText.translate(-1, -1);
  g.drawText("x", cbText, juce::Justification::centred, false);

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
  if (glView != nullptr) {
    if (!glView->IsRenderReady()) {
      g.fillAll(juce::Colours::black);
      auto msg = glView->GetError().isEmpty()
                     ? juce::String("Milkdrop initializing...")
                     : juce::String("Milkdrop error: ") + glView->GetError();
      g.setColour(juce::Colours::grey);
      g.setFont(juce::Font(12.0f));
      g.drawText(msg, content, juce::Justification::centred, false);
    }
  } else {
    g.fillAll(juce::Colours::black);
    g.setColour(juce::Colours::grey);
    g.setFont(juce::Font(12.0f));
    g.drawText("Milkdrop initializing...", content, juce::Justification::centred, false);
  }

  if (glView != nullptr && glView->IsRenderReady())
    PaintLoadingIndicator(g, content);

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
  // 纯 CPU 子组件。projectM 由 Editor GL 上下文在 renderOpenGL 中直接渲染，
  // 本组件仅负责：① 鼠标事件转发 ② 30Hz Timer 驱动 Overlay 刷新
  //     ③ auto-hide 检测 ④ auto 轮播 ⑤ PCM 和 preset 请求桥接到 Editor
  startTimerHz(30);
}

MilkdropModule::GLView::~GLView() {
  stopTimer();
}

// ---- GLView: 所有 projectM 状态现在由 Editor 持有，这里只做桥接 ----

void MilkdropModule::GLView::PushPcm(const float* interleaved_lr,
                                      unsigned int frame_count) {
  if (auto* ed = owner_.findParentComponentOfClass<Y2KmeterAudioProcessorEditor>())
    ed->PushMilkdropPcm(interleaved_lr, frame_count);
}

void MilkdropModule::GLView::RequestPresetDelta(int delta) {
  if (auto* ed = owner_.findParentComponentOfClass<Y2KmeterAudioProcessorEditor>())
    ed->RequestMilkdropPresetDelta(delta);
}

void MilkdropModule::GLView::RequestPresetRandom() {
  if (auto* ed = owner_.findParentComponentOfClass<Y2KmeterAudioProcessorEditor>())
    ed->RequestMilkdropPresetRandom();
}

void MilkdropModule::GLView::RequestPresetJump(int index) {
  if (auto* ed = owner_.findParentComponentOfClass<Y2KmeterAudioProcessorEditor>())
    ed->RequestMilkdropPresetJump(index);
}

void MilkdropModule::GLView::RequestRenderScale() {
  if (auto* ed = owner_.findParentComponentOfClass<Y2KmeterAudioProcessorEditor>())
    ed->RequestMilkdropRenderScale();
}

bool MilkdropModule::GLView::IsRenderReady() const {
  if (auto* ed = owner_.findParentComponentOfClass<Y2KmeterAudioProcessorEditor>())
    return ed->IsMilkdropRenderReady();
  return false;
}

juce::String MilkdropModule::GLView::GetError() const {
  if (auto* ed = owner_.findParentComponentOfClass<Y2KmeterAudioProcessorEditor>())
    return ed->GetMilkdropError();
  return "Editor not found";
}

int MilkdropModule::GLView::GetCurrentPresetIndex() const {
  if (auto* ed = owner_.findParentComponentOfClass<Y2KmeterAudioProcessorEditor>())
    return ed->GetMilkdropCurrentPresetIndex();
  return -1;
}

int MilkdropModule::GLView::GetTotalPresetCount() const {
  if (auto* ed = owner_.findParentComponentOfClass<Y2KmeterAudioProcessorEditor>())
    return ed->GetMilkdropTotalPresets();
  return 0;
}

juce::String MilkdropModule::GLView::GetCurrentPresetName() const {
  if (auto* ed = owner_.findParentComponentOfClass<Y2KmeterAudioProcessorEditor>())
    return ed->GetMilkdropCurrentPresetName();
  return {};
}

int64_t MilkdropModule::GLView::GetLastPresetSwitchTimeMs() const {
  if (auto* ed = owner_.findParentComponentOfClass<Y2KmeterAudioProcessorEditor>())
    return ed->GetMilkdropLastPresetSwitchTimeMs();
  return 0;
}

void MilkdropModule::GLView::timerCallback() {
  // 检查 Editor projectM 是否就绪（代替旧的 isRenderInitialized）
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
    auto resBtn    = juce::Rectangle<int>(autoBtn.getX() - kPadding - kResBtnW, overlay.getY() + 2, kResBtnW, kBtnSize);

    if (prevBtn.contains(pos))   return OverlayButton::kPrev;
    if (resBtn.contains(pos))    return OverlayButton::kRenderScale;
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
    case OverlayButton::kRenderScale: glView->RequestRenderScale(); break;
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
    drawBtn(nextBtn,   ">",   OverlayButton::kNext);
    drawBtn(randomBtn, "?",   OverlayButton::kRandom);

    // 渲染分辨率按钮 [1:n]
    {
      int s = 1;
      if (auto* ed = findParentComponentOfClass<Y2KmeterAudioProcessorEditor>())
        s = ed->GetMilkdropRenderScale();
      juce::String label = juce::String("1:") + juce::String(s);
      drawBtn(resBtn, label, OverlayButton::kRenderScale);
    }

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

    // 隐藏 GLView 以避免原生窗口 Z-order 遮住模态对话框。
    // 通过 GlViewRestorer::modalStateFinished 在对话框退出时恢复可见性。
    glView->setVisible(false);
    dlg->enterModalState(true, new GlViewRestorer(*glView));
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