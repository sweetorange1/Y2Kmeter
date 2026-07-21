/*
  ==============================================================================

  ProjectMApi.cpp
  见 ProjectMApi.h 头部注释。

  ==============================================================================
*/
#include "ProjectMApi.h"

#include <juce_core/juce_core.h>

#if defined (_WIN32)
 #define WIN32_LEAN_AND_MEAN
 #define NOMINMAX
 #include <windows.h>
#endif

namespace projectm_api
{

/**
 * @brief 在候选路径列表里挑一个存在的返回。用于同时兼容：
 *          1) 开发期 CMake 源目录下的 third_party/projectm/bin/*.dll；
 *          2) 生产期 exe / vst3 bundle 旁边的 *.dll（Post-build 拷贝的目标）。
 */
static juce::File findExistingFile (const juce::StringArray& candidates)
{
    for (auto& p : candidates)
    {
        juce::File f (p);
        if (f.existsAsFile())
            return f;
    }
    return {};
}

/**
 * @brief 猜测 projectM-4.dll 的位置。
 *
 * 顺序：
 *   1) 与当前模块（exe 或 vst3）同目录 —— 生产期由 CMake Post-build 拷贝。
 *   2) exe 同目录 —— 兼容 Standalone 加载 vst3 场景。
 *   3) 源码树 third_party/projectm/bin —— 开发期从 IDE 直接跑 Standalone。
 */
static juce::File locateProjectMDll()
{
    juce::StringArray candidates;

   #if defined (_WIN32)
    auto sameDirAs = [&] (juce::File dir)
    {
        candidates.add (dir.getChildFile ("projectM-4.dll").getFullPathName());
    };

    // 1) 当前动态库所在目录（VST3 / DLL 场景）
    sameDirAs (juce::File::getSpecialLocation (juce::File::currentApplicationFile).getParentDirectory());
    // 2) 主可执行文件目录（Standalone / DAW 主进程）
    sameDirAs (juce::File::getSpecialLocation (juce::File::hostApplicationPath).getParentDirectory());
    // 3) 开发树兜底：从 exe 向上找 third_party/projectm/bin
    {
        auto up = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
        for (int i = 0; i < 8 && up.exists(); ++i)
        {
            auto guess = up.getChildFile ("third_party/projectm/bin/projectM-4.dll");
            if (guess.existsAsFile())
            {
                candidates.add (guess.getFullPathName());
                break;
            }
            up = up.getParentDirectory();
        }
    }
   #endif

    return findExistingFile (candidates);
}

Api& Api::instance()
{
    static Api singleton;
    return singleton;
}

Api::Api()
{
    loadLibrary();
    if (moduleHandle != nullptr)
        resolveSymbols();
}

Api::~Api()
{
    unloadLibrary();
}

void Api::unloadLibrary()
{
   #if defined (_WIN32)
    if (moduleHandle != nullptr)
    {
        ::FreeLibrary ((HMODULE) moduleHandle);
        moduleHandle = nullptr;
    }
    if (glewModule != nullptr)
    {
        ::FreeLibrary ((HMODULE) glewModule);
        glewModule = nullptr;
    }
   #endif
    glewInitialized = false;
    resetSymbols();
}

void Api::resetSymbols()
{
    fn_create                = nullptr;
    fn_destroy               = nullptr;
    fn_setWindowSize         = nullptr;
    fn_setMeshSize           = nullptr;
    fn_setFps                = nullptr;
    fn_openglRenderFrame     = nullptr;
    fn_openglRenderFrameFbo  = nullptr;
    fn_setTextureSearchPaths = nullptr;
    fn_setPresetDuration     = nullptr;
    fn_setSoftCutDuration    = nullptr;
    fn_setHardCutEnabled     = nullptr;
    fn_loadPresetFile        = nullptr;
    fn_loadPresetData        = nullptr;
    fn_pcmAddFloat           = nullptr;
    available    = false;
    errorMessage.clear();
}

void Api::reload()
{
    // 调用侧已确保无存活 handle（MilkdropModule 中在 openGLContextClosing
    // 里destroy() -> reload()）。这里直接卸载、重新加载、重新解析。
    juce::Logger::writeToLog ("[projectm_api] reload() begin");
    unloadLibrary();
    // unloadLibrary 已把 glewInitialized 置 false，下一次 initGlew() 会在新上下文重新初始化。
    loadLibrary();
    if (moduleHandle != nullptr)
        resolveSymbols();
    juce::Logger::writeToLog (juce::String ("[projectm_api] reload() end, available=")
                              + (available ? "true" : "false"));
}

// ============================================================
// GLEW 初始化
// ============================================================
bool Api::initGlew()
{
   #if defined (_WIN32)
    if (glewInitialized)
        return true;

    // glew32.dll 与 projectM-4.dll 同目录，projectM 加载时已隐式拉起。
    // 这里直接 GetModuleHandleW 拿一个不增引的句柄。若不在当前进程（理论上不应该，
    // 因为 projectM-4.dll 已 LoadLibrary），回退到 LoadLibraryW 主动拉。
    HMODULE glew = ::GetModuleHandleW (L"glew32.dll");
    if (glew == nullptr)
    {
        // 已 SetDllDirectoryW 到 projectM-4.dll 目录，直接拉名字即可。
        glew = ::LoadLibraryW (L"glew32.dll");
        glewModule = (void*) glew;
    }
    if (glew == nullptr)
    {
        errorMessage = "Cannot load glew32.dll (expected in same directory as projectM-4.dll).";
        juce::Logger::writeToLog ("[projectm_api] initGlew() FAIL: cannot load glew32.dll");
        return false;
    }

    // GLEW 导出的是 "__declspec(dllimport) GLboolean glewExperimental"，在 DLL 侧真实名也叫
    // glewExperimental，GetProcAddress 拿到的是变量地址。
    auto* glewExperimentalPtr = (unsigned char*) ::GetProcAddress (glew, "glewExperimental");
    using GlewInitFn = int (__stdcall*) ();  // GLEW_API GLenum GLEWAPIENTRY glewInit(void)
    auto glewInitFn = reinterpret_cast<GlewInitFn> (::GetProcAddress (glew, "glewInit"));

    if (glewInitFn == nullptr)
    {
        errorMessage = "glew32.dll is missing the glewInit export.";
        juce::Logger::writeToLog ("[projectm_api] initGlew() FAIL: no glewInit export");
        return false;
    }

    if (glewExperimentalPtr != nullptr)
    {
        *glewExperimentalPtr = 1u; // GL_TRUE
        juce::Logger::writeToLog ("[projectm_api] glewExperimental set to GL_TRUE");
    }

    juce::Logger::writeToLog ("[projectm_api] calling glewInit()");
    const int glewErr = glewInitFn();
    juce::Logger::writeToLog (juce::String ("[projectm_api] glewInit() returned ") + juce::String (glewErr));

    // GLEW_OK == 0；GLEW_ERROR_NO_GL_VERSION==1、GLEW_ERROR_GL_VERSION_10_ONLY==2 等。
    // 使用 glewExperimental 后，即使非 0 也往往可以继续工作——但我们只在 OK 时才
    // 置位 avoid 重复；若不 OK 也尝试继续。
    if (glewErr == 0)
        glewInitialized = true;
    return true;
   #else
    return true;
   #endif
}

void Api::loadLibrary()
{
   #if defined (_WIN32)
    auto dll = locateProjectMDll();
    juce::Logger::writeToLog ("[projectm_api] loadLibrary() candidate=" + dll.getFullPathName());
    if (! dll.existsAsFile())
    {
        errorMessage = "Cannot find projectM-4.dll (expected next to executable or in third_party/projectm/bin/).";
        juce::Logger::writeToLog ("[projectm_api] loadLibrary() FAIL: " + juce::String (errorMessage));
        return;
    }

    // 提前把 glew32.dll 所在目录加入 DLL 搜索路径 —— projectM-4.dll 加载时依赖它。
    // SetDllDirectoryW 只影响进程后续的隐式加载搜索路径；对已加载的模块无影响。
    auto dllDir = dll.getParentDirectory().getFullPathName();
    ::SetDllDirectoryW (dllDir.toWideCharPointer());

    // LOAD_WITH_ALTERED_SEARCH_PATH 让 LoadLibrary 把 dll 所在目录当作搜索根，
    // 从而能自动找到同目录里的 glew32.dll。
    moduleHandle = (void*) ::LoadLibraryExW (dll.getFullPathName().toWideCharPointer(),
                                             nullptr,
                                             LOAD_WITH_ALTERED_SEARCH_PATH);
    if (moduleHandle == nullptr)
    {
        const auto err = ::GetLastError();
        errorMessage = "LoadLibraryExW(projectM-4.dll) failed, Win32 error code = "
                     + juce::String ((int) err).toStdString();
        juce::Logger::writeToLog ("[projectm_api] loadLibrary() FAIL: " + juce::String (errorMessage));
    }
    else
    {
        juce::Logger::writeToLog ("[projectm_api] loadLibrary() OK, HMODULE=0x"
                                  + juce::String::toHexString ((juce::pointer_sized_int) moduleHandle));
    }
   #else
    errorMessage = "libprojectM dynamic loading is not implemented on this platform.";
   #endif
}

void* Api::resolveOptional (const char* name)
{
   #if defined (_WIN32)
    if (moduleHandle == nullptr) return nullptr;
    return (void*) ::GetProcAddress ((HMODULE) moduleHandle, name);
   #else
    (void) name;
    return nullptr;
   #endif
}

void* Api::resolveRequired (const char* name)
{
    auto* p = resolveOptional (name);
    if (p == nullptr)
    {
        if (errorMessage.empty())
            errorMessage = std::string ("projectM-4.dll missing symbol: ") + name;
        available = false;
    }
    return p;
}

void Api::resolveSymbols()
{
    available = true; // 若下方 resolveRequired 失败会置回 false

    fn_create              = (Fn_projectm_create)              resolveRequired ("projectm_create");
    fn_destroy             = (Fn_projectm_destroy)             resolveRequired ("projectm_destroy");
    fn_setWindowSize       = (Fn_projectm_set_window_size)     resolveRequired ("projectm_set_window_size");
    fn_setMeshSize         = (Fn_projectm_set_mesh_size)       resolveRequired ("projectm_set_mesh_size");
    fn_setFps              = (Fn_projectm_set_fps)             resolveRequired ("projectm_set_fps");
    fn_openglRenderFrame   = (Fn_projectm_opengl_render_frame) resolveRequired ("projectm_opengl_render_frame");
    // fbo 版本仅在 projectM ≥ 4.2.0 提供；旧版本无此符号，故用可选解析。
    fn_openglRenderFrameFbo = (Fn_projectm_opengl_render_frame_fbo) resolveOptional ("projectm_opengl_render_frame_fbo");
    fn_setTextureSearchPaths
        = (Fn_projectm_set_texture_search_paths) resolveRequired ("projectm_set_texture_search_paths");
    fn_setPresetDuration   = (Fn_projectm_set_preset_duration)  resolveRequired ("projectm_set_preset_duration");
    fn_setSoftCutDuration  = (Fn_projectm_set_soft_cut_duration)resolveRequired ("projectm_set_soft_cut_duration");
    fn_setHardCutEnabled   = (Fn_projectm_set_hard_cut_enabled) resolveRequired ("projectm_set_hard_cut_enabled");
    fn_loadPresetFile      = (Fn_projectm_load_preset_file)     resolveRequired ("projectm_load_preset_file");
    fn_loadPresetData      = (Fn_projectm_load_preset_data)     resolveOptional ("projectm_load_preset_data");
    fn_pcmAddFloat         = (Fn_projectm_pcm_add_float)        resolveRequired ("projectm_pcm_add_float");

    // 诊断日志：方便在 IDE Output 里直接看到符号解析情况。
    juce::Logger::writeToLog (
        juce::String ("[projectm_api] resolveSymbols done. available=")
        + (available ? "true" : "false")
        + ", fn_create="     + juce::String::toHexString ((juce::pointer_sized_int) fn_create)
        + ", fn_render="     + juce::String::toHexString ((juce::pointer_sized_int) fn_openglRenderFrame)
        + ", fn_render_fbo=" + juce::String::toHexString ((juce::pointer_sized_int) fn_openglRenderFrameFbo)
        + ", fn_presetData=" + juce::String::toHexString ((juce::pointer_sized_int) fn_loadPresetData)
        + (errorMessage.empty() ? "" : ", err=" + juce::String (errorMessage)));
}

// ============================================================================
//  Thin dispatch wrappers
// ============================================================================

#if defined (_WIN32)
namespace {
    // 与 Api::Fn_projectm_create 保持一致的函数指针别名（此处的匿名 namespace
    // 里无法直接引用 private typedef，故重新声明一份）。
    using LocalFnCreate = projectm_handle (*)();

    // SEH 包裹调用，捕获访问违例等，返回是否成功。
    // 注意：只能在 __try/__except 与 C++ 对象共存 —— 这里刻意做成裸函数以避免 C4509。
    static bool callFnCreateWithSeh (LocalFnCreate fn, projectm_handle& outHandle, DWORD& outCode)
    {
        __try
        {
            outHandle = fn();
            outCode   = 0;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            outHandle = nullptr;
            outCode   = GetExceptionCode();
            return false;
        }
    }
}
#endif

projectm_handle Api::create() const
{
    if (! available || fn_create == nullptr)
    {
        // ⬆ 关键防御：不能在 available==true 但 fn_create==nullptr 时盲目调用。
        // 这个分支历史上真实到达过——因为 resolveRequired 在中途失败会把 available 置 false，
        // 但中途失败之前已经解析成功的指针照样不为空；而后续失败又会继续赋 nullptr——
        // 一旦有外部调用方先看到 available==true 就直接进入了这里，就会跳到 0x0。
        return nullptr;
    }

    // 诊断日志：便于在 Release 出现"跳转 0x0"时确认是不是 fn_create 内部崩，
    // 而不是我们这里错误地调用了空指针。
    juce::Logger::writeToLog (juce::String ("[projectm_api] calling fn_create @ 0x")
                              + juce::String::toHexString ((juce::pointer_sized_int) fn_create));

    projectm_handle h = nullptr;
   #if defined (_WIN32)
    DWORD sehCode = 0;
    // reinterpret_cast 因两个别名类型完全一致（同签名的 C 函数指针）是无害的。
    const bool ok = callFnCreateWithSeh (reinterpret_cast<LocalFnCreate> (fn_create), h, sehCode);
    if (! ok)
    {
        juce::Logger::writeToLog (juce::String ("[projectm_api] fn_create raised SEH exception, code=0x")
                                  + juce::String::toHexString ((int) sehCode));
        return nullptr;
    }
   #else
    h = fn_create();
   #endif

    juce::Logger::writeToLog (juce::String ("[projectm_api] fn_create returned handle=0x")
                              + juce::String::toHexString ((juce::pointer_sized_int) h));
    return h;
}

void Api::destroy (projectm_handle h) const
{
    if (available && h != nullptr) fn_destroy (h);
}

void Api::setWindowSize (projectm_handle h, std::size_t w, std::size_t hgt) const
{
    if (available && h != nullptr) fn_setWindowSize (h, w, hgt);
}

void Api::setMeshSize (projectm_handle h, std::size_t w, std::size_t hgt) const
{
    if (available && h != nullptr) fn_setMeshSize (h, w, hgt);
}

void Api::setFps (projectm_handle h, int32_t fps) const
{
    if (available && h != nullptr) fn_setFps (h, fps);
}

void Api::openglRenderFrame (projectm_handle h) const
{
    if (available && h != nullptr) fn_openglRenderFrame (h);
}

void Api::openglRenderFrameFbo (projectm_handle h, uint32_t framebufferObjectId) const
{
    if (available && h != nullptr && fn_openglRenderFrameFbo != nullptr)
        fn_openglRenderFrameFbo (h, framebufferObjectId);
}

void Api::setTextureSearchPaths (projectm_handle h,
                                 const std::vector<std::string>& paths) const
{
    if (! available || h == nullptr || paths.empty()) return;

    std::vector<const char*> cptrs;
    cptrs.reserve (paths.size());
    for (auto& s : paths) cptrs.push_back (s.c_str());

    fn_setTextureSearchPaths (h, cptrs.data(), cptrs.size());
}

void Api::setPresetDuration (projectm_handle h, double seconds) const
{
    if (available && h != nullptr) fn_setPresetDuration (h, seconds);
}

void Api::setSoftCutDuration (projectm_handle h, double seconds) const
{
    if (available && h != nullptr) fn_setSoftCutDuration (h, seconds);
}

void Api::setHardCutEnabled (projectm_handle h, bool enabled) const
{
    if (available && h != nullptr) fn_setHardCutEnabled (h, enabled);
}

void Api::loadPresetFile (projectm_handle h,
                          const std::string& filename,
                          bool smoothTransition) const
{
    if (available && h != nullptr)
        fn_loadPresetFile (h, filename.c_str(), smoothTransition);
}

void Api::loadPresetData (projectm_handle h,
                          const std::string& data,
                          bool smoothTransition) const
{
    if (available && h != nullptr && fn_loadPresetData != nullptr)
        fn_loadPresetData (h, data.c_str(), smoothTransition);
}

void Api::addPcmFloat (projectm_handle h,
                       const float* samples, unsigned int count,
                       bool stereo) const
{
    if (available && h != nullptr && samples != nullptr && count > 0)
        fn_pcmAddFloat (h, samples, count, stereo ? PROJECTM_STEREO : PROJECTM_MONO);
}

} // namespace projectm_api
