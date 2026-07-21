/*
  ==============================================================================

  ProjectMApi.h
  Y2Kmeter — Milkdrop 模块（自 2.0.4 起）使用的 libprojectM 4 动态加载封装。

  为什么用 LoadLibrary + GetProcAddress，而不是链接期依赖 projectM-4.lib？
  --------------------------------------------------------------------------
  · 官方 Windows 预编译发布（frontend-sdl-cpp 的 ZIP）只提供 .dll，不带 .lib
    导入库，也不带 SDK 头文件；我们在 third_party/projectm/include 里放的
    是从 projectM 源码抓的公共 C 头，而 PROJECTM_EXPORT 已被替换成空宏
    （参见 third_party/projectm/include/projectM-4/projectM_export.h）。
  · 走 GetProcAddress 后：
      - MSVC 链接期完全不依赖 projectM 库；
      - Y2Kmeter.dll / Y2Kmeter.exe 在 projectM-4.dll 缺失时也能启动，
        只是 Milkdrop 模块会显示一个"未安装 libprojectM"的兜底面板；
      - 未来想升级 projectM 只需替换 DLL，不用重编。

  只暴露 Y2Kmeter 需要用到的那一小部分 C API：
    · projectm_create / projectm_destroy
    · projectm_set_window_size / projectm_set_texture_search_paths
    · projectm_set_fps / projectm_set_mesh_size
    · projectm_set_preset_duration / projectm_set_soft_cut_duration
    · projectm_set_hard_cut_enabled
    · projectm_load_preset_file
    · projectm_pcm_add_float
    · projectm_opengl_render_frame

  使用方法（简化）:

      auto& api = projectm_api::Api::instance();
      if (! api.isAvailable()) { showFallbackUI(); return; }
      auto handle = api.create();         // 需在当前线程持有活动 OpenGL 上下文
      api.setWindowSize(handle, w, h);
      api.setTextureSearchPaths(handle, { texturesDir });
      api.loadPresetFile(handle, "some.milk", true);
      // 每帧：
      api.addPcmFloat(handle, pcmLR, count, true  // stereo
                       );
      api.openglRenderFrame(handle);
      // 销毁：
      api.destroy(handle);

  实例（handle）本身是线程绑定的（内部持有 OpenGL 资源），必须在同一个
  OpenGL 上下文下 create / render / destroy —— 由 MilkdropModule 里挂的
  juce::OpenGLContext 保证。

  ==============================================================================
*/
#pragma once

// 先引入 projectM 官方 C 头（PROJECTM_EXPORT 已被替换为空，见 projectM_export.h），
// 避免标准库头文件与 projectM C 头在 MSVC 下产生不可预期的交互。
#include "projectM-4/types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <mutex>

namespace projectm_api
{

/**
 * @brief 单例封装，负责在首次访问时 LoadLibrary 加载 projectM-4.dll，
 *        以及解析我们需要的所有函数指针。
 *
 * 线程安全：instance() 使用 Meyers singleton；isAvailable() / 各 API
 * 调用无内部锁，函数指针在初始化后只读，可从任意线程访问（前提是调用
 * 侧自己保证 projectm_handle 的线程使用规则 —— 通常绑定到 OpenGL 上下文）。
 */
class Api
{
public:
    static Api& instance();

    Api (const Api&) = delete;
    Api& operator= (const Api&) = delete;

    /** 库加载成功、所有必需函数指针都解析到位时返回 true。 */
    bool isAvailable() const noexcept { return available; }

    /** 若加载失败，返回一段可读的诊断字符串（供 UI 兜底展示）。 */
    const std::string& loadError() const noexcept { return errorMessage; }

    /**
     * @brief 在当前线程活动的 OpenGL 上下文里初始化 GLEW。
     *
     * 为什么必须显式做这一步？
     * ------------------------------------------------------------------
     * projectM-4.dll (Windows 官方 4.1.x 构建) 内部使用 glew32.dll 提供的
     * GL 扩展函数指针表，但 projectm_create() 自己 **不** 调用 glewInit()——
     * 它假设宿主已经调过。如果宿主没调，GLEW 全局函数指针表全为 NULL，
     * projectm_create 内部第一次 glGen* 或 glCompile* 就会 0xC0000005 崩溃。
     *
     * 另外，Core Profile 下 glGetString(GL_EXTENSIONS) 会返回 NULL，标准
     * glewInit 里 strstr(NULL,"...") 会崩；这需要 glewExperimental = GL_TRUE
     * 让 GLEW 走替代路径。
     *
     * 本函数：动态加载 glew32.dll，取到 glewInit 与 glewExperimental 变量
     * 地址，先置 experimental，再调 glewInit()。只在首次成功后置位；
     * reload() 也会把状态复位以配合下一次上下文重建。
     *
     * 调用约束：必须在当前线程持有活动 OpenGL 上下文时调用（一般在
     * newOpenGLContextCreated() 里，create() 之前）。返回 true 表示可以
     * 继续 create；false 会同时写 errorMessage。
     */
    bool initGlew();

    /**
     * @brief 卸载并重新加载 projectM-4.dll，同时重新解析所有函数指针。
     *
     * 为什么需要 reload？
     * ------------------------------------------------------------------
     * libprojectM 4 (Windows/GLEW) 在 DLL 内维护一整套 GL 扩展函数指针
     * 表（GLEW），这些指针在 projectm_create() 内部通过 wglGetProcAddress
     * 首次填充；一旦第一次挂载的 juce::OpenGLContext 关闭，再新建下一个
     * OpenGLContext 里的 HGLRC 不同，之前那批函数指针在新上下文里可能
     * 无效（wgl 规范：函数指针与 HGLRC 绑定），projectM 内部却不重新
     * glewInit → 直接调用旧指针 → 跳到已释放地址或 0x0 → 0xC0000005。
     *
     * 对策：每次销毁 pmHandle 时 FreeLibrary → 让 DLL 内全局状态归零；
     *       下一次 create 前重新 LoadLibrary + resolveSymbols。
     *
     * 线程约束：必须在没有任何存活 handle 的时刻调用（调用侧责任）。
     */
    void reload();

    // --- Core -------------------------------------------------------------
    projectm_handle create() const;
    void destroy (projectm_handle instance) const;

    // --- Window / Rendering ---------------------------------------------
    void setWindowSize     (projectm_handle instance, std::size_t w, std::size_t h) const;
    void setMeshSize       (projectm_handle instance, std::size_t w, std::size_t h) const;
    void setFps            (projectm_handle instance, int32_t fps) const;
    void openglRenderFrame (projectm_handle instance) const;

    /**
     * @brief 渲染一帧到指定 FBO（projectM >= 4.2.0）。
     *
     * Windows 上的 JUCE OpenGL 读/写 offscreen “CachedImage” FBO，
     * 它在调 renderer->renderOpenGL() 时已将 nativeContext->getFrameBufferID()
     * 绑定到 GL_FRAMEBUFFER。但 projectM 内部会 glBindFramebuffer(0)，把最终帧
     * 写到“默认 FBO 0”——结果 JUCE swap() 时拿到的那个 FBO 一直是黑的。
     * 调用 fbo 版本并传入 JUCE 的 FBO ID 就能修复这个问题。
     *
     * 可选符号：如果当前加载的 DLL 不提供该符号（projectM 4.1.x），
     * @ref hasOpenglRenderFrameFbo 返回 false，调用者应回退到 @ref openglRenderFrame。
     */
    void openglRenderFrameFbo (projectm_handle instance, uint32_t framebufferObjectId) const;

    /** 当前加载的 DLL 是否提供 projectm_opengl_render_frame_fbo。 */
    bool hasOpenglRenderFrameFbo() const noexcept { return fn_openglRenderFrameFbo != nullptr; }

    /** 当前加载的 DLL 是否提供 projectm_load_preset_data。 */
    bool hasLoadPresetData() const noexcept { return fn_loadPresetData != nullptr; }

    // --- Presets / Transitions -------------------------------------------
    /** paths 为 UTF-8 字符串。projectM 会把这些目录加入纹理搜索路径。 */
    void setTextureSearchPaths (projectm_handle instance,
                                const std::vector<std::string>& paths) const;
    void setPresetDuration     (projectm_handle instance, double seconds) const;
    void setSoftCutDuration    (projectm_handle instance, double seconds) const;
    void setHardCutEnabled     (projectm_handle instance, bool enabled) const;
    void loadPresetFile        (projectm_handle instance,
                                const std::string& filename,
                                bool smoothTransition) const;
    void loadPresetData        (projectm_handle instance,
                                const std::string& data,
                                bool smoothTransition) const;

    // --- Audio ------------------------------------------------------------
    /** samples 长度：单声道 count；立体声 count 对（内部按 LRLR 交错读取 2*count 个 float）。 */
    void addPcmFloat (projectm_handle instance,
                      const float* samples, unsigned int count,
                      bool stereo) const;

private:
    Api();
    ~Api();

    void  loadLibrary();
    void  unloadLibrary();
    void  resolveSymbols();
    void  resetSymbols();

    /** 首次成功后置位，避免重复 glewInit。reload() 会复位。 */
    bool  glewInitialized = false;
    /** 惰性加载的 glew32.dll 句柄；随主 DLL 一同 unload。 */
    void* glewModule = nullptr;
    void* resolveOptional (const char* name);
    void* resolveRequired (const char* name);

    // --- 平台句柄 --------------------------------------------------------
#if defined (_WIN32)
    void* moduleHandle = nullptr; // HMODULE
#else
    void* moduleHandle = nullptr; // dlopen handle，暂未启用
#endif

    // --- 函数指针 typedef -----------------------------------------------
    using Fn_projectm_create              = projectm_handle (*)();
    using Fn_projectm_destroy             = void (*)(projectm_handle);
    using Fn_projectm_set_window_size     = void (*)(projectm_handle, std::size_t, std::size_t);
    using Fn_projectm_set_mesh_size       = void (*)(projectm_handle, std::size_t, std::size_t);
    using Fn_projectm_set_fps             = void (*)(projectm_handle, int32_t);
    using Fn_projectm_opengl_render_frame = void (*)(projectm_handle);
    using Fn_projectm_opengl_render_frame_fbo = void (*)(projectm_handle, uint32_t);
    using Fn_projectm_set_texture_search_paths
        = void (*)(projectm_handle, const char**, std::size_t);
    using Fn_projectm_set_preset_duration  = void (*)(projectm_handle, double);
    using Fn_projectm_set_soft_cut_duration= void (*)(projectm_handle, double);
    using Fn_projectm_set_hard_cut_enabled = void (*)(projectm_handle, bool);
    using Fn_projectm_load_preset_file     = void (*)(projectm_handle, const char*, bool);
    using Fn_projectm_load_preset_data     = void (*)(projectm_handle, const char*, bool);
    using Fn_projectm_pcm_add_float        = void (*)(projectm_handle, const float*, unsigned int, projectm_channels);

    Fn_projectm_create              fn_create              = nullptr;
    Fn_projectm_destroy             fn_destroy             = nullptr;
    Fn_projectm_set_window_size     fn_setWindowSize       = nullptr;
    Fn_projectm_set_mesh_size       fn_setMeshSize         = nullptr;
    Fn_projectm_set_fps             fn_setFps              = nullptr;
    Fn_projectm_opengl_render_frame fn_openglRenderFrame   = nullptr;
    Fn_projectm_opengl_render_frame_fbo fn_openglRenderFrameFbo = nullptr;
    Fn_projectm_set_texture_search_paths fn_setTextureSearchPaths = nullptr;
    Fn_projectm_set_preset_duration  fn_setPresetDuration  = nullptr;
    Fn_projectm_set_soft_cut_duration fn_setSoftCutDuration = nullptr;
    Fn_projectm_set_hard_cut_enabled fn_setHardCutEnabled  = nullptr;
    Fn_projectm_load_preset_file     fn_loadPresetFile     = nullptr;
    Fn_projectm_load_preset_data     fn_loadPresetData     = nullptr;
    Fn_projectm_pcm_add_float        fn_pcmAddFloat        = nullptr;

    bool         available = false;
    std::string  errorMessage;
};

} // namespace projectm_api
