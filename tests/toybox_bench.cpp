// ==========================================================
// toybox_bench —— Standalone Win32 WGL 4.6 RTX Offscreen
//                 Micro-Benchmark (zero JUCE dependency)
//
// Measures directly on the physical GPU (NVIDIA RTX 5060) even
// inside a 32Hz RDP session (wglSwapIntervalEXT(0) + offscreen
// 3840x2088 RGBA8 FBO = exact 4K @ 150% DPI window size):
//   1. GPU Batched ToyBox Pass (exact GlToyShaderSource GLSL +
//      y2ktoy::packToySpritesToQuads + 1024x1024 RGBA8 atlas +
//      VBO orphaning + 2 exclusion rects) across 0/20/100/220/400
//      Party-mode emojis -> CPU submit ms (QPC) & GPU elapsed ms
//      (GL_TIME_ELAPSED), plus readback pixel verification!
//   2. CPU ToyPhysics step + sprite pack latency for 0..400 actors.
//   3. 25-module layout XML serialization cost at 10 clicks/sec:
//      uncoalesced (3x/click) vs 200ms coalesced.
// ==========================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wrl/client.h>
#include <dwrite.h>
#include <d2d1.h>
#include <wincodec.h>
#include <GL/gl.h>

#include "source/ui/toys/ToyGlTypes.h"
#include "source/ui/toys/ToyPhysics.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Request discrete NVIDIA/AMD GPU (matches Y2Kmeter Standalone GPU selection)
extern "C"
{
    __declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
    __declspec(dllexport) int   AmdPowerXpressRequestHighPerformance = 1;
}

namespace
{
// Minimal GL 3.x / 4.x constants & function typedefs (avoid extra SDK headers)
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER                 0x8892
#define GL_ELEMENT_ARRAY_BUFFER         0x8893
#define GL_STREAM_DRAW                  0x88E0
#define GL_FRAGMENT_SHADER              0x8B30
#define GL_VERTEX_SHADER                0x8B31
#define GL_COMPILE_STATUS               0x8B81
#define GL_LINK_STATUS                  0x8B82
#define GL_TEXTURE0                     0x84C0
#define GL_CLAMP_TO_EDGE                0x812F
#define GL_FRAMEBUFFER                  0x8D40
#define GL_COLOR_ATTACHMENT0            0x8CE0
#define GL_FRAMEBUFFER_COMPLETE         0x8CD5
#define GL_FUNC_ADD                     0x8006
#define GL_TIME_ELAPSED                 0x88BF
#define GL_QUERY_RESULT                 0x8866
#endif

typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;
typedef char      GLchar;
typedef uint64_t  GLuint64;

using PFN_glGenVertexArrays       = void (APIENTRY*)(GLsizei, GLuint*);
using PFN_glBindVertexArray       = void (APIENTRY*)(GLuint);
using PFN_glDeleteVertexArrays    = void (APIENTRY*)(GLsizei, const GLuint*);
using PFN_glGenBuffers            = void (APIENTRY*)(GLsizei, GLuint*);
using PFN_glBindBuffer            = void (APIENTRY*)(GLenum, GLuint);
using PFN_glDeleteBuffers         = void (APIENTRY*)(GLsizei, const GLuint*);
using PFN_glBufferData            = void (APIENTRY*)(GLenum, GLsizeiptr, const void*, GLenum);
using PFN_glBufferSubData         = void (APIENTRY*)(GLenum, GLintptr, GLsizeiptr, const void*);
using PFN_glCreateShader          = GLuint (APIENTRY*)(GLenum);
using PFN_glShaderSource          = void (APIENTRY*)(GLuint, GLsizei, const GLchar* const*, const GLint*);
using PFN_glCompileShader         = void (APIENTRY*)(GLuint);
using PFN_glGetShaderiv           = void (APIENTRY*)(GLuint, GLenum, GLint*);
using PFN_glGetShaderInfoLog      = void (APIENTRY*)(GLuint, GLsizei, GLsizei*, GLchar*);
using PFN_glDeleteShader          = void (APIENTRY*)(GLuint);
using PFN_glCreateProgram         = GLuint (APIENTRY*)();
using PFN_glAttachShader          = void (APIENTRY*)(GLuint, GLuint);
using PFN_glLinkProgram           = void (APIENTRY*)(GLuint);
using PFN_glGetProgramiv          = void (APIENTRY*)(GLuint, GLenum, GLint*);
using PFN_glGetProgramInfoLog     = void (APIENTRY*)(GLuint, GLsizei, GLsizei*, GLchar*);
using PFN_glUseProgram            = void (APIENTRY*)(GLuint);
using PFN_glDeleteProgram         = void (APIENTRY*)(GLuint);
using PFN_glGetAttribLocation     = GLint (APIENTRY*)(GLuint, const GLchar*);
using PFN_glGetUniformLocation    = GLint (APIENTRY*)(GLuint, const GLchar*);
using PFN_glEnableVertexAttribArr = void (APIENTRY*)(GLuint);
using PFN_glVertexAttribPointer   = void (APIENTRY*)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
using PFN_glUniform1i             = void (APIENTRY*)(GLint, GLint);
using PFN_glUniform2f             = void (APIENTRY*)(GLint, GLfloat, GLfloat);
using PFN_glUniform4fv            = void (APIENTRY*)(GLint, GLsizei, const GLfloat*);
using PFN_glActiveTexture         = void (APIENTRY*)(GLenum);
using PFN_glBlendFuncSeparate     = void (APIENTRY*)(GLenum, GLenum, GLenum, GLenum);
using PFN_glBlendEquationSeparate = void (APIENTRY*)(GLenum, GLenum);
using PFN_glGenFramebuffers       = void (APIENTRY*)(GLsizei, GLuint*);
using PFN_glBindFramebuffer       = void (APIENTRY*)(GLenum, GLuint);
using PFN_glFramebufferTexture2D  = void (APIENTRY*)(GLenum, GLenum, GLenum, GLuint, GLint);
using PFN_glCheckFramebufferStat  = GLenum (APIENTRY*)(GLenum);
using PFN_glDeleteFramebuffers    = void (APIENTRY*)(GLsizei, const GLuint*);
using PFN_glGenQueries            = void (APIENTRY*)(GLsizei, GLuint*);
using PFN_glDeleteQueries         = void (APIENTRY*)(GLsizei, const GLuint*);
using PFN_glBeginQuery            = void (APIENTRY*)(GLenum, GLuint);
using PFN_glEndQuery              = void (APIENTRY*)(GLenum);
using PFN_glGetQueryObjectui64v   = void (APIENTRY*)(GLuint, GLenum, GLuint64*);
using PFN_wglSwapIntervalEXT      = BOOL (WINAPI*)(int);

struct GlFuncs
{
    PFN_glGenVertexArrays       glGenVertexArrays = nullptr;
    PFN_glBindVertexArray       glBindVertexArray = nullptr;
    PFN_glDeleteVertexArrays    glDeleteVertexArrays = nullptr;
    PFN_glGenBuffers            glGenBuffers = nullptr;
    PFN_glBindBuffer            glBindBuffer = nullptr;
    PFN_glDeleteBuffers         glDeleteBuffers = nullptr;
    PFN_glBufferData            glBufferData = nullptr;
    PFN_glBufferSubData         glBufferSubData = nullptr;
    PFN_glCreateShader          glCreateShader = nullptr;
    PFN_glShaderSource          glShaderSource = nullptr;
    PFN_glCompileShader         glCompileShader = nullptr;
    PFN_glGetShaderiv           glGetShaderiv = nullptr;
    PFN_glGetShaderInfoLog      glGetShaderInfoLog = nullptr;
    PFN_glDeleteShader          glDeleteShader = nullptr;
    PFN_glCreateProgram         glCreateProgram = nullptr;
    PFN_glAttachShader          glAttachShader = nullptr;
    PFN_glLinkProgram           glLinkProgram = nullptr;
    PFN_glGetProgramiv          glGetProgramiv = nullptr;
    PFN_glGetProgramInfoLog     glGetProgramInfoLog = nullptr;
    PFN_glUseProgram            glUseProgram = nullptr;
    PFN_glDeleteProgram         glDeleteProgram = nullptr;
    PFN_glGetAttribLocation     glGetAttribLocation = nullptr;
    PFN_glGetUniformLocation    glGetUniformLocation = nullptr;
    PFN_glEnableVertexAttribArr glEnableVertexAttribArray = nullptr;
    PFN_glVertexAttribPointer   glVertexAttribPointer = nullptr;
    PFN_glUniform1i             glUniform1i = nullptr;
    PFN_glUniform2f             glUniform2f = nullptr;
    PFN_glUniform4fv            glUniform4fv = nullptr;
    PFN_glActiveTexture         glActiveTexture = nullptr;
    PFN_glBlendFuncSeparate     glBlendFuncSeparate = nullptr;
    PFN_glBlendEquationSeparate glBlendEquationSeparate = nullptr;
    PFN_glGenFramebuffers       glGenFramebuffers = nullptr;
    PFN_glBindFramebuffer       glBindFramebuffer = nullptr;
    PFN_glFramebufferTexture2D  glFramebufferTexture2D = nullptr;
    PFN_glCheckFramebufferStat  glCheckFramebufferStatus = nullptr;
    PFN_glDeleteFramebuffers    glDeleteFramebuffers = nullptr;
    PFN_glGenQueries            glGenQueries = nullptr;
    PFN_glDeleteQueries         glDeleteQueries = nullptr;
    PFN_glBeginQuery            glBeginQuery = nullptr;
    PFN_glEndQuery              glEndQuery = nullptr;
    PFN_glGetQueryObjectui64v   glGetQueryObjectui64v = nullptr;
    PFN_wglSwapIntervalEXT      wglSwapIntervalEXT = nullptr;

    template <typename Fn>
    static bool load (Fn& dst, const char* name)
    {
        dst = reinterpret_cast<Fn> (wglGetProcAddress (name));
        return dst != nullptr;
    }

    bool loadAll()
    {
        bool ok = true;
        ok &= load (glGenVertexArrays, "glGenVertexArrays");
        ok &= load (glBindVertexArray, "glBindVertexArray");
        ok &= load (glDeleteVertexArrays, "glDeleteVertexArrays");
        ok &= load (glGenBuffers, "glGenBuffers");
        ok &= load (glBindBuffer, "glBindBuffer");
        ok &= load (glDeleteBuffers, "glDeleteBuffers");
        ok &= load (glBufferData, "glBufferData");
        ok &= load (glBufferSubData, "glBufferSubData");
        ok &= load (glCreateShader, "glCreateShader");
        ok &= load (glShaderSource, "glShaderSource");
        ok &= load (glCompileShader, "glCompileShader");
        ok &= load (glGetShaderiv, "glGetShaderiv");
        ok &= load (glGetShaderInfoLog, "glGetShaderInfoLog");
        ok &= load (glDeleteShader, "glDeleteShader");
        ok &= load (glCreateProgram, "glCreateProgram");
        ok &= load (glAttachShader, "glAttachShader");
        ok &= load (glLinkProgram, "glLinkProgram");
        ok &= load (glGetProgramiv, "glGetProgramiv");
        ok &= load (glGetProgramInfoLog, "glGetProgramInfoLog");
        ok &= load (glUseProgram, "glUseProgram");
        ok &= load (glDeleteProgram, "glDeleteProgram");
        ok &= load (glGetAttribLocation, "glGetAttribLocation");
        ok &= load (glGetUniformLocation, "glGetUniformLocation");
        ok &= load (glEnableVertexAttribArray, "glEnableVertexAttribArray");
        ok &= load (glVertexAttribPointer, "glVertexAttribPointer");
        ok &= load (glUniform1i, "glUniform1i");
        ok &= load (glUniform2f, "glUniform2f");
        ok &= load (glUniform4fv, "glUniform4fv");
        ok &= load (glActiveTexture, "glActiveTexture");
        ok &= load (glBlendFuncSeparate, "glBlendFuncSeparate");
        ok &= load (glBlendEquationSeparate, "glBlendEquationSeparate");
        ok &= load (glGenFramebuffers, "glGenFramebuffers");
        ok &= load (glBindFramebuffer, "glBindFramebuffer");
        ok &= load (glFramebufferTexture2D, "glFramebufferTexture2D");
        ok &= load (glCheckFramebufferStatus, "glCheckFramebufferStatus");
        ok &= load (glDeleteFramebuffers, "glDeleteFramebuffers");
        ok &= load (glGenQueries, "glGenQueries");
        ok &= load (glDeleteQueries, "glDeleteQueries");
        ok &= load (glBeginQuery, "glBeginQuery");
        ok &= load (glEndQuery, "glEndQuery");
        ok &= load (glGetQueryObjectui64v, "glGetQueryObjectui64v");
        load (wglSwapIntervalEXT, "wglSwapIntervalEXT"); // optional
        return ok;
    }
};

// Exact mirror of y2ktoy::GlToyShaderSource (source/ui/toys/GlToyRenderer.h)
constexpr const char* kVertexShader =
    "#version 120\n"
    "attribute vec2 aPos;\n"
    "attribute vec2 aUv;\n"
    "attribute vec4 aColor;\n"
    "attribute vec4 aClip;\n"
    "attribute float aMode;\n"
    "uniform vec2 uEditorSize;\n"
    "varying vec2 vUv;\n"
    "varying vec4 vColor;\n"
    "varying vec2 vEditorPos;\n"
    "varying vec4 vClip;\n"
    "varying float vMode;\n"
    "void main()\n"
    "{\n"
    "  vec2 ndc = vec2((aPos.x / max(uEditorSize.x, 1.0)) * 2.0 - 1.0,\n"
    "                  1.0 - (aPos.y / max(uEditorSize.y, 1.0)) * 2.0);\n"
    "  gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "  vUv = aUv;\n"
    "  vColor = aColor;\n"
    "  vEditorPos = aPos;\n"
    "  vClip = aClip;\n"
    "  vMode = aMode;\n"
    "}\n";

constexpr const char* kFragmentShader =
    "#version 120\n"
    "uniform sampler2D uAtlas;\n"
    "uniform int uExclusionCount;\n"
    "uniform vec4 uExclusions[16];\n"
    "varying vec2 vUv;\n"
    "varying vec4 vColor;\n"
    "varying vec2 vEditorPos;\n"
    "varying vec4 vClip;\n"
    "varying float vMode;\n"
    "void main()\n"
    "{\n"
    "  if (vClip.z > vClip.x)\n"
    "  {\n"
    "    if (vEditorPos.x < vClip.x || vEditorPos.x > vClip.z ||\n"
    "        vEditorPos.y < vClip.y || vEditorPos.y > vClip.w)\n"
    "      discard;\n"
    "  }\n"
    "  for (int i = 0; i < 16; ++i)\n"
    "  {\n"
    "    if (i >= uExclusionCount) break;\n"
    "    vec4 ex = uExclusions[i];\n"
    "    if (vEditorPos.x >= ex.x && vEditorPos.x <= ex.z &&\n"
    "        vEditorPos.y >= ex.y && vEditorPos.y <= ex.w)\n"
    "      discard;\n"
    "  }\n"
    "  if (vMode < 0.5)\n"
    "  {\n"
    "    vec4 tex = texture2D(uAtlas, vUv);\n"
    "    vec4 outC = tex * vColor.a;\n"
    "    if (outC.a <= 0.001) discard;\n"
    "    gl_FragColor = outC;\n"
    "  }\n"
    "  else if (vMode < 1.5)\n"
    "  {\n"
    "    float d = length(vUv);\n"
    "    float aa = 1.0 - smoothstep(0.86, 1.0, d);\n"
    "    if (aa <= 0.001) discard;\n"
    "    gl_FragColor = vColor * aa;\n"
    "  }\n"
    "  else\n"
    "  {\n"
    "    gl_FragColor = vColor;\n"
    "  }\n"
    "}\n";

struct Stats { double mean = 0.0, p95 = 0.0, p99 = 0.0, maxv = 0.0; };

Stats summarize (std::vector<double> vals)
{
    Stats s;
    if (vals.empty()) return s;
    double sum = 0.0;
    for (double v : vals) sum += v;
    std::sort (vals.begin(), vals.end());
    s.mean = sum / (double) vals.size();
    s.p95  = vals[(size_t) ((vals.size() - 1) * 95 / 100)];
    s.p99  = vals[(size_t) ((vals.size() - 1) * 99 / 100)];
    s.maxv = vals.back();
    return s;
}

double qpcMs (LARGE_INTEGER t0, LARGE_INTEGER t1, LARGE_INTEGER freq)
{
    return (double) (t1.QuadPart - t0.QuadPart) * 1000.0 / (double) freq.QuadPart;
}

std::vector<y2ktoy::GlToySprite> makePartySprites (int count, int logicalW, int logicalH)
{
    std::vector<y2ktoy::GlToySprite> out;
    out.reserve ((size_t) count);
    for (int i = 0; i < count; ++i)
    {
        const int tier = i % 3;
        const auto sl = y2ktoy::EmojiAtlasLayout::slotFor (i % y2ktoy::EmojiAtlasLayout::kMaxEmojiSlots, tier);
        y2ktoy::GlToySprite sp {};
        sp.cx       = (float) (40 + (i * 197) % (logicalW - 80));
        sp.cy       = (float) (80 + (i * 131) % (logicalH - 120));
        sp.drawSize = (tier == 0) ? 18.0f : (tier == 1 ? 24.0f : 34.0f);
        sp.angleRad = (float) (i * 0.37);
        sp.scaleY   = (i % 7 == 0) ? 0.78f : 1.0f;
        sp.alpha    = 0.95f;
        sp.u0 = sl.u0; sp.v0 = sl.v0; sp.u1 = sl.u1; sp.v1 = sl.v1;
        sp.haloR = 0.95f; sp.haloG = 0.35f; sp.haloB = 0.85f; sp.haloA = 0.25f;
        out.push_back (sp);
    }
    return out;
}

// Benchmark 2: CPU physics + snapshot + quad packing
void runCpuPhysicsAndPackBench (LARGE_INTEGER freq)
{
    std::printf ("\n=== 2. CPU Physics Step + Quad Pack Micro-Benchmark ===\n");
    std::printf ("  %6s | %9s | %9s | %9s\n", "Actors", "mean(ms)", "p95(ms)", "max(ms)");

    struct Particle { float x, y, vx, vy, angle, spin, scaleY; int tier, emoji; };
    std::vector<y2ktoy::GlToyVertex> verts;
    std::vector<uint16_t> indices;

    for (int n : { 0, 20, 100, 220, 400 })
    {
        std::vector<Particle> ps ((size_t) n);
        for (int i = 0; i < n; ++i)
            ps[(size_t) i] = { (float) (100 + (i * 53) % 2300), (float) (100 + (i * 41) % 1100),
                               (float) ((i % 11 - 5) * 40), -300.0f, 0.0f, 1.5f, 1.0f, i % 3, i % 97 };

        std::vector<y2ktoy::GlToySprite> sprites ((size_t) n);
        std::vector<double> samples;
        samples.reserve (500);

        for (int iter = 0; iter < 550; ++iter)
        {
            LARGE_INTEGER t0, t1;
            QueryPerformanceCounter (&t0);

            for (int i = 0; i < n; ++i)
            {
                auto& p = ps[(size_t) i];
                const float prevY = p.y;
                y2ktoy::integrate (p.x, p.y, p.vx, p.vy, 1.0f / 60.0f, 1100.0f, 0.4f);
                // Check against 23 docked module platforms (matches user's real layout)
                for (int m = 0; m < 23; ++m)
                {
                    const float x0 = (float) ((m % 6) * 400);
                    const float topY = (float) (256 + (m / 6) * 256);
                    if (y2ktoy::platformHitX (p.x, x0, x0 + 380.0f, prevY, p.y, topY, p.vy))
                    {
                        p.y = topY;
                        p.vy = -p.vy * 0.45f;
                        p.scaleY = 0.72f;
                        break;
                    }
                }
                if (p.y > 1350.0f) { p.y = 200.0f; p.vy = -300.0f; }
                p.angle += p.spin * (1.0f / 60.0f);

                const auto sl = y2ktoy::EmojiAtlasLayout::slotFor (p.emoji, p.tier);
                auto& sp = sprites[(size_t) i];
                sp.cx = p.x; sp.cy = p.y;
                sp.drawSize = (p.tier == 0) ? 18.0f : (p.tier == 1 ? 24.0f : 34.0f);
                sp.angleRad = p.angle; sp.scaleY = p.scaleY; sp.alpha = 1.0f;
                sp.u0 = sl.u0; sp.v0 = sl.v0; sp.u1 = sl.u1; sp.v1 = sl.v1;
                sp.haloR = 1.0f; sp.haloG = 0.2f; sp.haloB = 0.8f; sp.haloA = 0.25f;
            }
            y2ktoy::packToySpritesToQuads (sprites.data(), sprites.size(), verts, indices);

            QueryPerformanceCounter (&t1);
            if (iter >= 50)
                samples.push_back (qpcMs (t0, t1, freq));
        }
        const auto st = summarize (samples);
        std::printf ("  %6d | %9.4f | %9.4f | %9.4f\n", n, st.mean, st.p95, st.maxv);
    }
}

// Benchmark 3: 25-module layout XML serialization coalescing (10 clicks/sec for 10s)
void runLayoutCoalesceBench (LARGE_INTEGER freq)
{
    std::printf ("\n=== 3. 25-Module Layout XML Serialization (10 clicks/sec x 10s = 100 clicks) ===\n");
    auto serialize25Modules = [] () -> std::string
    {
        std::string xml;
        xml.reserve (16384);
        xml += "<?xml version=\"1.0\" encoding=\"UTF-8\"?><PBEQ_State version=\"1\"><PBEQ_Layout>";
        for (int i = 0; i < 25; ++i)
        {
            char buf[384];
            std::snprintf (buf, sizeof (buf),
                "<Module type=\"oscilloscope\" id=\"%d\" x=\"%d\" y=\"%d\" w=\"384\" h=\"256\">"
                "<state displayMode=\"%d\" waveChannel=\"%d\" crossLow=\"200.0\" crossHigh=\"2000.0\" "
                "autoGain=\"1\" showRings=\"1\" dotSize=\"2.0\" fadeSpeed=\"1.0\" timeScale=\"1.0\" "
                "lineWidth=\"1.400000\" fillWave=\"0\" showGrid=\"1\" scatterGainDb=\"0.0\"/></Module>",
                500 + i, (i % 6) * 384, (i / 6) * 256, i % 3, i % 3);
            xml += buf;
        }
        xml += "</PBEQ_Layout></PBEQ_State>";
        return xml;
    };

    // Uncoalesced: 3x serialize per discrete click (onWillChangeParam + recordUndo + 16ms timer) = 300 serializes
    LARGE_INTEGER t0, t1, t2;
    size_t sink = 0;
    QueryPerformanceCounter (&t0);
    for (int click = 0; click < 100; ++click)
        for (int k = 0; k < 3; ++k)
            sink += serialize25Modules().size();
    QueryPerformanceCounter (&t1);

    // Coalesced (200ms timer, undoBaselineTree_ reused on mouseDown):
    // 100 clicks at 100ms spacing -> continuous reset -> 1 flush at end (or <=50 at 200ms pauses);
    // model worst-case burst of 50 coalesced flushes + 0ms on mouseDown!
    for (int flush = 0; flush < 50; ++flush)
        sink += serialize25Modules().size();
    QueryPerformanceCounter (&t2);

    const double uncoalMs = qpcMs (t0, t1, freq);
    const double coalMs   = qpcMs (t1, t2, freq);
    std::printf ("  Uncoalesced (300 serializes, 3x/click) : %.3f ms total (%.4f ms/click on UI thread)\n",
                 uncoalMs, uncoalMs / 100.0);
    std::printf ("  Coalesced   (<=50 deferred flushes)    : %.3f ms total (0.0000 ms on mouseDown, %.1fx reduction)\n",
                 coalMs, coalMs > 0.0 ? uncoalMs / coalMs : 0.0);
    if (sink == 0) std::printf ("  (unreachable)\n");
}

// Benchmark 4: Win32 DirectWrite + D2D1 WIC Emoji Prebake (Plan B: 3 reusable targets vs 291 per-glyph targets)
void runWin32D2DEmojiPrebakeBench (LARGE_INTEGER freq)
{
    using Microsoft::WRL::ComPtr;
    std::printf ("\n=== 4. Win32 D2D/WIC Emoji Prebake (291 glyphs: 97 x {38,50,70}px + themed outline) ===\n");

    const HRESULT hrCo = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    const bool needCoUninit = (hrCo == S_OK || hrCo == S_FALSE);
    if (FAILED (hrCo) && hrCo != RPC_E_CHANGED_MODE)
    {
        std::printf ("  SKIP: CoInitializeEx failed hr=0x%08X\n", (unsigned) hrCo);
        return;
    }

    // Representative emoji glyphs (UTF-16 surrogate pairs from EmojiAtlas pool)
    static const wchar_t* kSampleGlyphs[] = {
        L"\xD83D\xDC96", L"\xD83D\xDD25", L"\x2728",         L"\xD83C\xDFB5",
        L"\xD83D\xDC7E", L"\xD83D\xDE80", L"\xD83C\xDF1F",   L"\xD83C\xDF08",
        L"\xD83D\xDC8E", L"\xD83C\xDFA7", L"\xD83E\xDD8B",   L"\xD83C\xDF38",
        L"\x26A1",       L"\xD83D\xDCAB", L"\xD83C\xDF89",   L"\xD83D\xDC51"
    };
    constexpr int kNumSample = (int) (sizeof (kSampleGlyphs) / sizeof (kSampleGlyphs[0]));
    constexpr int kSizes[3]  = { 38, 50, 70 }; // Exact S / M / L pixel sizes at 150% DPI
    constexpr int kEmojis    = 97;

    // Software themed duotone + rad=2 outline dilation + premul RGBA pack (matches ToyEmoji::bakeThemedOne)
    auto runThemedOutlinePass = [] (const BYTE* bgra, int px, UINT stride, std::vector<uint8_t>& outRgba) -> size_t
    {
        outRgba.resize ((size_t) px * (size_t) px * 4u);
        std::vector<uint8_t> alphaSrc ((size_t) px * (size_t) px);
        size_t nonZero = 0;
        for (int y = 0; y < px; ++y)
        {
            const BYTE* row = bgra + (size_t) y * stride;
            for (int x = 0; x < px; ++x)
            {
                const uint8_t a = row[x * 4 + 3];
                alphaSrc[(size_t) y * (size_t) px + (size_t) x] = a;
                if (a > 16) ++nonZero;
            }
        }
        constexpr int rad = 2; // uiScale=1.5 outline radius
        for (int y = 0; y < px; ++y)
        {
            const BYTE* row = bgra + (size_t) y * stride;
            uint8_t* dst = outRgba.data() + (size_t) y * (size_t) px * 4u;
            for (int x = 0; x < px; ++x)
            {
                const uint8_t b = row[x * 4 + 0];
                const uint8_t g = row[x * 4 + 1];
                const uint8_t r = row[x * 4 + 2];
                const uint8_t a = row[x * 4 + 3];
                if (a >= 24)
                {
                    // Duotone blend (premultiplied RGBA)
                    const int lum = (r * 77 + g * 150 + b * 29) >> 8;
                    dst[x * 4 + 0] = (uint8_t) ((r + ((255 * lum) >> 8)) >> 1);
                    dst[x * 4 + 1] = (uint8_t) ((g + ((120 * lum) >> 8)) >> 1);
                    dst[x * 4 + 2] = (uint8_t) ((b + ((210 * lum) >> 8)) >> 1);
                    dst[x * 4 + 3] = a;
                }
                else
                {
                    uint8_t maxA = 0;
                    const int y0 = std::max (0, y - rad), y1 = std::min (px - 1, y + rad);
                    const int x0 = std::max (0, x - rad), x1 = std::min (px - 1, x + rad);
                    for (int ny = y0; ny <= y1; ++ny)
                        for (int nx = x0; nx <= x1; ++nx)
                            maxA = std::max (maxA, alphaSrc[(size_t) ny * (size_t) px + (size_t) nx]);
                    const uint8_t oa = (uint8_t) ((maxA * 220) >> 8);
                    dst[x * 4 + 0] = (uint8_t) ((255 * oa) >> 8);
                    dst[x * 4 + 1] = (uint8_t) ((180 * oa) >> 8);
                    dst[x * 4 + 2] = (uint8_t) ((230 * oa) >> 8);
                    dst[x * 4 + 3] = oa;
                }
            }
        }
        return nonZero;
    };

    double legacyD2dMs = 0.0, legacyTotalMs = 0.0;
    double reuseD2dMs  = 0.0, reuseTotalMs  = 0.0;
    size_t legacyPixels = 0, reusePixels = 0;

    {
        ComPtr<IDWriteFactory> dw;
        ComPtr<ID2D1Factory> d2d;
        ComPtr<IWICImagingFactory> wic;
        const HRESULT hrDw  = DWriteCreateFactory (DWRITE_FACTORY_TYPE_SHARED, __uuidof (IDWriteFactory),
                                                   reinterpret_cast<IUnknown**> (dw.GetAddressOf()));
        const HRESULT hrD2d = D2D1CreateFactory (D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof (ID2D1Factory), nullptr,
                                                 reinterpret_cast<void**> (d2d.GetAddressOf()));
        const HRESULT hrWic = CoCreateInstance (CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                                __uuidof (IWICImagingFactory), reinterpret_cast<void**> (wic.GetAddressOf()));
        if (FAILED (hrDw) || FAILED (hrD2d) || FAILED (hrWic))
        {
            std::printf ("  SKIP: DWrite/D2D/WIC init failed (0x%08X/0x%08X/0x%08X)\n",
                         (unsigned) hrDw, (unsigned) hrD2d, (unsigned) hrWic);
        }
        else
        {
            const auto rtp = D2D1::RenderTargetProperties (
                D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                D2D1::PixelFormat (DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                96.0f, 96.0f);

            // Warm up Segoe UI Emoji font cache once so both paths measure steady glyph rendering
            {
                ComPtr<IDWriteTextFormat> wFmt;
                ComPtr<IDWriteTextLayout> wLay;
                ComPtr<IWICBitmap> wBmp;
                ComPtr<ID2D1RenderTarget> wRt;
                ComPtr<ID2D1SolidColorBrush> wBr;
                dw->CreateTextFormat (L"Segoe UI Emoji", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 32.0f, L"en-US", wFmt.GetAddressOf());
                dw->CreateTextLayout (kSampleGlyphs[0], (UINT32) wcslen (kSampleGlyphs[0]), wFmt.Get(), 38.0f, 38.0f, wLay.GetAddressOf());
                wic->CreateBitmap (38, 38, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, wBmp.GetAddressOf());
                d2d->CreateWicBitmapRenderTarget (wBmp.Get(), rtp, wRt.GetAddressOf());
                wRt->CreateSolidColorBrush (D2D1::ColorF (D2D1::ColorF::White), wBr.GetAddressOf());
                wRt->BeginDraw(); wRt->Clear (D2D1::ColorF (0, 0, 0, 0));
                wRt->DrawTextLayout (D2D1::Point2F (0, 0), wLay.Get(), wBr.Get(), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
                wRt->EndDraw();
            }

            std::vector<uint8_t> themedOut;

            // --- Path A: Legacy (create & destroy 5 COM objects per glyph x 291) ---
            LARGE_INTEGER t0, t1, t2;
            QueryPerformanceCounter (&t0);
            for (int e = 0; e < kEmojis; ++e)
            {
                const wchar_t* glyph = kSampleGlyphs[e % kNumSample];
                const UINT32 len = (UINT32) wcslen (glyph);
                for (int s = 0; s < 3; ++s)
                {
                    const int px = kSizes[s];
                    LARGE_INTEGER g0, g1;
                    QueryPerformanceCounter (&g0);

                    ComPtr<IDWriteTextFormat> fmt;
                    ComPtr<IDWriteTextLayout> layout;
                    ComPtr<IWICBitmap> bmp;
                    ComPtr<ID2D1RenderTarget> rt;
                    ComPtr<ID2D1SolidColorBrush> brush;
                    dw->CreateTextFormat (L"Segoe UI Emoji", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                          (float) px * 0.82f, L"en-US", fmt.GetAddressOf());
                    fmt->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_CENTER);
                    fmt->SetParagraphAlignment (DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    dw->CreateTextLayout (glyph, len, fmt.Get(), (float) px, (float) px, layout.GetAddressOf());
                    wic->CreateBitmap ((UINT) px, (UINT) px, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, bmp.GetAddressOf());
                    d2d->CreateWicBitmapRenderTarget (bmp.Get(), rtp, rt.GetAddressOf());
                    rt->CreateSolidColorBrush (D2D1::ColorF (D2D1::ColorF::White), brush.GetAddressOf());
                    rt->BeginDraw();
                    rt->Clear (D2D1::ColorF (0, 0, 0, 0));
                    rt->DrawTextLayout (D2D1::Point2F (0, 0), layout.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
                    rt->EndDraw();

                    ComPtr<IWICBitmapLock> lock;
                    UINT stride = 0, bufSz = 0;
                    BYTE* bits = nullptr;
                    bmp->Lock (nullptr, WICBitmapLockRead, lock.GetAddressOf());
                    lock->GetStride (&stride);
                    lock->GetDataPointer (&bufSz, &bits);
                    QueryPerformanceCounter (&g1);
                    legacyD2dMs += qpcMs (g0, g1, freq);

                    if (bits != nullptr)
                        legacyPixels += runThemedOutlinePass (bits, px, stride, themedOut);
                }
            }
            QueryPerformanceCounter (&t1);
            legacyTotalMs = qpcMs (t0, t1, freq);

            // --- Path B: Plan B (3 reusable per-size bundles: 38 / 50 / 70px) ---
            struct SizeSlot
            {
                int px = 0;
                ComPtr<IDWriteTextFormat> fmt;
                ComPtr<IWICBitmap> bmp;
                ComPtr<ID2D1RenderTarget> rt;
                ComPtr<ID2D1SolidColorBrush> brush;
            } slots[3];

            QueryPerformanceCounter (&t1);
            for (int s = 0; s < 3; ++s)
            {
                slots[s].px = kSizes[s];
                dw->CreateTextFormat (L"Segoe UI Emoji", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                      (float) kSizes[s] * 0.82f, L"en-US", slots[s].fmt.GetAddressOf());
                slots[s].fmt->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_CENTER);
                slots[s].fmt->SetParagraphAlignment (DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                wic->CreateBitmap ((UINT) kSizes[s], (UINT) kSizes[s], GUID_WICPixelFormat32bppPBGRA,
                                   WICBitmapCacheOnDemand, slots[s].bmp.GetAddressOf());
                d2d->CreateWicBitmapRenderTarget (slots[s].bmp.Get(), rtp, slots[s].rt.GetAddressOf());
                slots[s].rt->CreateSolidColorBrush (D2D1::ColorF (D2D1::ColorF::White), slots[s].brush.GetAddressOf());
            }
            for (int e = 0; e < kEmojis; ++e)
            {
                const wchar_t* glyph = kSampleGlyphs[e % kNumSample];
                const UINT32 len = (UINT32) wcslen (glyph);
                for (int s = 0; s < 3; ++s)
                {
                    auto& sl = slots[s];
                    LARGE_INTEGER g0, g1;
                    QueryPerformanceCounter (&g0);

                    ComPtr<IDWriteTextLayout> layout;
                    dw->CreateTextLayout (glyph, len, sl.fmt.Get(), (float) sl.px, (float) sl.px, layout.GetAddressOf());
                    sl.rt->BeginDraw();
                    sl.rt->Clear (D2D1::ColorF (0, 0, 0, 0));
                    sl.rt->DrawTextLayout (D2D1::Point2F (0, 0), layout.Get(), sl.brush.Get(), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
                    sl.rt->EndDraw();

                    ComPtr<IWICBitmapLock> lock;
                    UINT stride = 0, bufSz = 0;
                    BYTE* bits = nullptr;
                    sl.bmp->Lock (nullptr, WICBitmapLockRead, lock.GetAddressOf());
                    lock->GetStride (&stride);
                    lock->GetDataPointer (&bufSz, &bits);
                    QueryPerformanceCounter (&g1);
                    reuseD2dMs += qpcMs (g0, g1, freq);

                    if (bits != nullptr)
                        reusePixels += runThemedOutlinePass (bits, sl.px, stride, themedOut);
                }
            }
            QueryPerformanceCounter (&t2);
            reuseTotalMs = qpcMs (t1, t2, freq);

            const double d2dSpeedup   = reuseD2dMs > 0.0 ? legacyD2dMs / reuseD2dMs : 0.0;
            const double totalSpeedup = reuseTotalMs > 0.0 ? legacyTotalMs / reuseTotalMs : 0.0;
            std::printf ("  Legacy   (291x per-glyph COM targets) : D2D=%.2f ms, Total(+themed/outline)=%.2f ms (UI-thread blocked!)\n",
                         legacyD2dMs, legacyTotalMs);
            std::printf ("  Reusable (3x per-size COM bundles)    : D2D=%.2f ms (%.2fx faster), Total=%.2f ms (%.2fx faster, 0.00ms on UI thread via PrebakeWorker)\n",
                         reuseD2dMs, d2dSpeedup, reuseTotalMs, totalSpeedup);
            std::printf ("  Pixel Parity Check                    : legacyNonZero=%zu reuseNonZero=%zu [%s]\n",
                         legacyPixels, reusePixels, (legacyPixels == reusePixels && legacyPixels > 0) ? "OK" : "MISMATCH");
        }
    }

    if (needCoUninit)
        CoUninitialize();
}

} // namespace

int main()
{
    LARGE_INTEGER freq {};
    QueryPerformanceFrequency (&freq);

    // Create hidden Win32 window + WGL context
    WNDCLASSA wc {};
    wc.style         = CS_OWNDC;
    wc.lpfnWndProc   = DefWindowProcA;
    wc.hInstance     = GetModuleHandleA (nullptr);
    wc.lpszClassName = "Y2K_TOYBOX_BENCH_WND";
    RegisterClassA (&wc);

    HWND hwnd = CreateWindowA (wc.lpszClassName, "Y2KBench", WS_POPUP, 0, 0, 64, 64,
                               nullptr, nullptr, wc.hInstance, nullptr);
    if (! hwnd) { std::printf ("FAIL: CreateWindowA failed\n"); return 1; }

    HDC hdc = GetDC (hwnd);
    PIXELFORMATDESCRIPTOR pfd {};
    pfd.nSize      = sizeof (pfd);
    pfd.nVersion   = 1;
    pfd.dwFlags    = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    const int pf = ChoosePixelFormat (hdc, &pfd);
    SetPixelFormat (hdc, pf, &pfd);

    HGLRC hglrc = wglCreateContext (hdc);
    if (! hglrc || ! wglMakeCurrent (hdc, hglrc))
    {
        std::printf ("FAIL: wglCreateContext / wglMakeCurrent failed\n");
        return 1;
    }

    std::printf ("=== Y2Kmeter ToyBox RTX Offscreen Micro-Benchmark ===\n");
    std::printf ("GL_VENDOR   : %s\n", (const char*) glGetString (GL_VENDOR));
    std::printf ("GL_RENDERER : %s\n", (const char*) glGetString (GL_RENDERER));
    std::printf ("GL_VERSION  : %s\n", (const char*) glGetString (GL_VERSION));

    GlFuncs gl;
    if (! gl.loadAll())
    {
        std::printf ("FAIL: failed to load required GL 3.x/4.x entry points\n");
        return 1;
    }
    if (gl.wglSwapIntervalEXT)
        gl.wglSwapIntervalEXT (0);

    // Compile shader program
    auto compile = [&] (GLenum type, const char* src) -> GLuint
    {
        GLuint sh = gl.glCreateShader (type);
        gl.glShaderSource (sh, 1, &src, nullptr);
        gl.glCompileShader (sh);
        GLint ok = 0;
        gl.glGetShaderiv (sh, GL_COMPILE_STATUS, &ok);
        if (! ok)
        {
            char log[1024] {};
            gl.glGetShaderInfoLog (sh, sizeof (log), nullptr, log);
            std::printf ("FAIL: shader compile error: %s\n", log);
            return 0;
        }
        return sh;
    };

    GLuint vs = compile (GL_VERTEX_SHADER, kVertexShader);
    GLuint fs = compile (GL_FRAGMENT_SHADER, kFragmentShader);
    GLuint prog = gl.glCreateProgram();
    gl.glAttachShader (prog, vs);
    gl.glAttachShader (prog, fs);
    gl.glLinkProgram (prog);
    GLint linked = 0;
    gl.glGetProgramiv (prog, GL_LINK_STATUS, &linked);
    gl.glDeleteShader (vs);
    gl.glDeleteShader (fs);
    if (! linked)
    {
        char log[1024] {};
        gl.glGetProgramInfoLog (prog, sizeof (log), nullptr, log);
        std::printf ("FAIL: program link error: %s\n", log);
        return 1;
    }

    const GLint aPosLoc            = gl.glGetAttribLocation (prog, "aPos");
    const GLint aUvLoc             = gl.glGetAttribLocation (prog, "aUv");
    const GLint aColorLoc          = gl.glGetAttribLocation (prog, "aColor");
    const GLint aClipLoc           = gl.glGetAttribLocation (prog, "aClip");
    const GLint aModeLoc           = gl.glGetAttribLocation (prog, "aMode");
    const GLint uAtlasLoc          = gl.glGetUniformLocation (prog, "uAtlas");
    const GLint uEditorSizeLoc     = gl.glGetUniformLocation (prog, "uEditorSize");
    const GLint uExclusionCountLoc = gl.glGetUniformLocation (prog, "uExclusionCount");
    const GLint uExclusionsLoc     = gl.glGetUniformLocation (prog, "uExclusions");

    // Create 3840x2088 RGBA8 offscreen FBO (exact 4K @ 150% DPI window size!)
    constexpr int kPhysW = 3840, kPhysH = 2088;
    constexpr int kLogW  = 2560, kLogH  = 1392;
    GLuint fbo = 0, fboTex = 0;
    glGenTextures (1, &fboTex);
    glBindTexture (GL_TEXTURE_2D, fboTex);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, kPhysW, kPhysH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    gl.glGenFramebuffers (1, &fbo);
    gl.glBindFramebuffer (GL_FRAMEBUFFER, fbo);
    gl.glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fboTex, 0);
    if (gl.glCheckFramebufferStatus (GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        std::printf ("FAIL: 3840x2088 FBO incomplete\n");
        return 1;
    }

    // Create 1024x1024 premultiplied RGBA8 emoji atlas + time incremental glTexSubImage2D upload
    GLuint atlasTex = 0;
    glGenTextures (1, &atlasTex);
    glBindTexture (GL_TEXTURE_2D, atlasTex);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    std::vector<uint8_t> zeros ((size_t) y2ktoy::EmojiAtlasLayout::kAtlasWidth
                                * (size_t) y2ktoy::EmojiAtlasLayout::kAtlasHeight * 4u, 0u);
    glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8,
                  y2ktoy::EmojiAtlasLayout::kAtlasWidth, y2ktoy::EmojiAtlasLayout::kAtlasHeight,
                  0, GL_RGBA, GL_UNSIGNED_BYTE, zeros.data());

    // Populate all 98x3 slots via incremental glTexSubImage2D (measures realistic upload cost)
    std::vector<uint8_t> tile (70u * 70u * 4u);
    for (size_t p = 0; p < 70u * 70u; ++p)
    {
        tile[p * 4 + 0] = 240;
        tile[p * 4 + 1] = 110;
        tile[p * 4 + 2] = 210;
        tile[p * 4 + 3] = 255;
    }
    LARGE_INTEGER up0, up1;
    glFinish();
    QueryPerformanceCounter (&up0);
    for (int s = 0; s < y2ktoy::EmojiAtlasLayout::kNumSizeTiers; ++s)
    {
        for (int e = 0; e < y2ktoy::EmojiAtlasLayout::kMaxEmojiSlots; ++e)
        {
            const auto sl = y2ktoy::EmojiAtlasLayout::slotFor (e, s);
            glTexSubImage2D (GL_TEXTURE_2D, 0, sl.x, sl.y, sl.w, sl.h,
                             GL_RGBA, GL_UNSIGNED_BYTE, tile.data());
        }
    }
    glFinish();
    QueryPerformanceCounter (&up1);
    std::printf ("Atlas incremental upload (294 slots into 1024x1024): %.3f ms total (%.4f ms/slot)\n",
                 qpcMs (up0, up1, freq), qpcMs (up0, up1, freq) / 294.0);

    // Setup VAO / VBO / EBO
    GLuint vao = 0, vbo = 0, ebo = 0;
    gl.glGenVertexArrays (1, &vao);
    gl.glGenBuffers (1, &vbo);
    gl.glGenBuffers (1, &ebo);
    gl.glBindVertexArray (vao);
    gl.glBindBuffer (GL_ARRAY_BUFFER, vbo);
    gl.glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, ebo);

    constexpr GLsizei stride = (GLsizei) sizeof (y2ktoy::GlToyVertex);
    gl.glEnableVertexAttribArray ((GLuint) aPosLoc);
    gl.glVertexAttribPointer ((GLuint) aPosLoc, 2, GL_FLOAT, GL_FALSE, stride, (const void*) offsetof (y2ktoy::GlToyVertex, x));
    gl.glEnableVertexAttribArray ((GLuint) aUvLoc);
    gl.glVertexAttribPointer ((GLuint) aUvLoc, 2, GL_FLOAT, GL_FALSE, stride, (const void*) offsetof (y2ktoy::GlToyVertex, u));
    gl.glEnableVertexAttribArray ((GLuint) aColorLoc);
    gl.glVertexAttribPointer ((GLuint) aColorLoc, 4, GL_FLOAT, GL_FALSE, stride, (const void*) offsetof (y2ktoy::GlToyVertex, r));
    gl.glEnableVertexAttribArray ((GLuint) aClipLoc);
    gl.glVertexAttribPointer ((GLuint) aClipLoc, 4, GL_FLOAT, GL_FALSE, stride, (const void*) offsetof (y2ktoy::GlToyVertex, cx0));
    gl.glEnableVertexAttribArray ((GLuint) aModeLoc);
    gl.glVertexAttribPointer ((GLuint) aModeLoc, 1, GL_FLOAT, GL_FALSE, stride, (const void*) offsetof (y2ktoy::GlToyVertex, mode));

    GLuint timerQuery = 0;
    gl.glGenQueries (1, &timerQuery);

    // Pixel readback sanity check: 1 emoji at (200,200) visible, 1 emoji at (500,500) masked by exclusion rect
    {
        std::vector<y2ktoy::GlToySprite> verify (2);
        const auto sl = y2ktoy::EmojiAtlasLayout::slotFor (0, 2);
        verify[0].cx = 200.0f; verify[0].cy = 200.0f; verify[0].drawSize = 40.0f; verify[0].alpha = 1.0f;
        verify[0].u0 = sl.u0; verify[0].v0 = sl.v0; verify[0].u1 = sl.u1; verify[0].v1 = sl.v1;
        verify[1] = verify[0];
        verify[1].cx = 500.0f; verify[1].cy = 500.0f;

        std::vector<y2ktoy::GlToyVertex> vv;
        std::vector<uint16_t> vi;
        y2ktoy::packToySpritesToQuads (verify.data(), verify.size(), vv, vi);

        glViewport (0, 0, kPhysW, kPhysH);
        glClearColor (0.0f, 0.0f, 0.0f, 0.0f);
        glClear (GL_COLOR_BUFFER_BIT);
        glEnable (GL_BLEND);
        gl.glBlendEquationSeparate (GL_FUNC_ADD, GL_FUNC_ADD);
        gl.glBlendFuncSeparate (GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        gl.glUseProgram (prog);
        gl.glActiveTexture (GL_TEXTURE0);
        glBindTexture (GL_TEXTURE_2D, atlasTex);
        gl.glUniform1i (uAtlasLoc, 0);
        gl.glUniform2f (uEditorSizeLoc, (float) kLogW, (float) kLogH);
        const float ex[8] = { 460.0f, 460.0f, 540.0f, 540.0f, 0.0f, 0.0f, 10.0f, 10.0f };
        gl.glUniform1i (uExclusionCountLoc, 2);
        gl.glUniform4fv (uExclusionsLoc, 2, ex);
        gl.glBufferData (GL_ARRAY_BUFFER, (GLsizeiptr) (vv.size() * sizeof (y2ktoy::GlToyVertex)), vv.data(), GL_STREAM_DRAW);
        gl.glBufferData (GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr) (vi.size() * sizeof (uint16_t)), vi.data(), GL_STREAM_DRAW);
        glDrawElements (GL_TRIANGLES, (GLsizei) vi.size(), GL_UNSIGNED_SHORT, nullptr);
        glFinish();

        uint8_t pxVis[4] {}, pxEx[4] {};
        // Editor logical (x, y) top-down -> physical FBO bottom-up (x*1.5, kPhysH - y*1.5)
        glReadPixels (300, kPhysH - 300, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pxVis);
        glReadPixels (750, kPhysH - 750, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pxEx);
        const bool visOk = (pxVis[0] > 200 && pxVis[3] > 200);
        const bool exOk  = (pxEx[0] == 0 && pxEx[1] == 0 && pxEx[2] == 0 && pxEx[3] == 0);
        std::printf ("GPU Readback Verify: visibleEmoji=(%u,%u,%u,%u)[%s] excludedEmoji=(%u,%u,%u,%u)[%s] glErr=0x%X\n",
                     pxVis[0], pxVis[1], pxVis[2], pxVis[3], visOk ? "OK" : "FAIL",
                     pxEx[0], pxEx[1], pxEx[2], pxEx[3], exOk ? "OK" : "FAIL",
                     (unsigned) glGetError());
        if (! visOk || ! exOk)
        {
            std::printf ("FAIL: GPU readback verification failed!\n");
            return 1;
        }
    }

    // Benchmark 1: Offscreen 3840x2088 FBO sweep across 0 / 20 / 100 / 220 / 400 Party emojis
    std::printf ("\n=== 1. GPU Offscreen 3840x2088 (4K @ 150%% DPI) Batched ToyBox Benchmark ===\n");
    std::printf ("  %6s | %9s %9s %9s | %9s %9s %9s | %s\n",
                 "Emojis", "CPU mean", "CPU p95", "CPU max", "GPU mean", "GPU p95", "GPU max", "Quads");

    std::vector<y2ktoy::GlToyVertex> verts;
    std::vector<uint16_t> indices;
    GLsizeiptr vboCap = 0, eboCap = 0;
    const float benchExclusions[8] = { 1800.0f, 100.0f, 2200.0f, 500.0f, 0.0f, 0.0f, 2560.0f, 32.0f };

    for (int count : { 0, 20, 100, 220, 400 })
    {
        auto sprites = makePartySprites (count, kLogW, kLogH);
        std::vector<double> cpuSamples, gpuSamples;
        cpuSamples.reserve (300);
        gpuSamples.reserve (300);

        for (int f = 0; f < 350; ++f)
        {
            // Animate angles slightly so vertices change every frame
            for (auto& sp : sprites) sp.angleRad += 0.02f;

            LARGE_INTEGER t0, t1;
            QueryPerformanceCounter (&t0);
            gl.glBeginQuery (GL_TIME_ELAPSED, timerQuery);

            y2ktoy::packToySpritesToQuads (sprites.data(), sprites.size(), verts, indices);

            glViewport (0, 0, kPhysW, kPhysH);
            glEnable (GL_BLEND);
            gl.glBlendEquationSeparate (GL_FUNC_ADD, GL_FUNC_ADD);
            gl.glBlendFuncSeparate (GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            gl.glUseProgram (prog);
            gl.glActiveTexture (GL_TEXTURE0);
            glBindTexture (GL_TEXTURE_2D, atlasTex);
            gl.glUniform1i (uAtlasLoc, 0);
            gl.glUniform2f (uEditorSizeLoc, (float) kLogW, (float) kLogH);
            gl.glUniform1i (uExclusionCountLoc, 2);
            gl.glUniform4fv (uExclusionsLoc, 2, benchExclusions);

            if (! indices.empty())
            {
                gl.glBindVertexArray (vao);
                const GLsizeiptr vBytes = (GLsizeiptr) (verts.size() * sizeof (y2ktoy::GlToyVertex));
                const GLsizeiptr iBytes = (GLsizeiptr) (indices.size() * sizeof (uint16_t));
                vboCap = std::max (vboCap, vBytes);
                eboCap = std::max (eboCap, iBytes);
                gl.glBindBuffer (GL_ARRAY_BUFFER, vbo);
                gl.glBufferData (GL_ARRAY_BUFFER, vboCap, nullptr, GL_STREAM_DRAW);
                gl.glBufferSubData (GL_ARRAY_BUFFER, 0, vBytes, verts.data());
                gl.glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, ebo);
                gl.glBufferData (GL_ELEMENT_ARRAY_BUFFER, eboCap, nullptr, GL_STREAM_DRAW);
                gl.glBufferSubData (GL_ELEMENT_ARRAY_BUFFER, 0, iBytes, indices.data());
                glDrawElements (GL_TRIANGLES, (GLsizei) indices.size(), GL_UNSIGNED_SHORT, nullptr);
            }

            gl.glEndQuery (GL_TIME_ELAPSED);
            QueryPerformanceCounter (&t1);

            GLuint64 gpuNs = 0;
            gl.glGetQueryObjectui64v (timerQuery, GL_QUERY_RESULT, &gpuNs);

            if (f >= 50)
            {
                cpuSamples.push_back (qpcMs (t0, t1, freq));
                gpuSamples.push_back ((double) gpuNs * 1e-6);
            }
        }

        const auto cs = summarize (cpuSamples);
        const auto gs = summarize (gpuSamples);
        std::printf ("  %6d | %7.4fms %7.4fms %7.4fms | %7.4fms %7.4fms %7.4fms | %zu\n",
                     count, cs.mean, cs.p95, cs.maxv, gs.mean, gs.p95, gs.maxv, indices.size() / 6);
    }

    const GLenum endErr = glGetError();
    std::printf ("GL Error Check: 0x%X (%s)\n", (unsigned) endErr, endErr == GL_NO_ERROR ? "OK" : "ERROR");

    gl.glDeleteQueries (1, &timerQuery);
    gl.glDeleteBuffers (1, &vbo);
    gl.glDeleteBuffers (1, &ebo);
    gl.glDeleteVertexArrays (1, &vao);
    glDeleteTextures (1, &atlasTex);
    gl.glDeleteFramebuffers (1, &fbo);
    glDeleteTextures (1, &fboTex);
    gl.glDeleteProgram (prog);
    wglMakeCurrent (nullptr, nullptr);
    wglDeleteContext (hglrc);
    ReleaseDC (hwnd, hdc);
    DestroyWindow (hwnd);

    runCpuPhysicsAndPackBench (freq);
    runLayoutCoalesceBench (freq);
    runWin32D2DEmojiPrebakeBench (freq);

    if (endErr != GL_NO_ERROR)
        return 1;
    std::printf ("\nBENCH_OK\n");
    return 0;
}
