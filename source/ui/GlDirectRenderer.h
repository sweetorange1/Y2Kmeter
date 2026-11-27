#pragma once

#include <JuceHeader.h>
#include <juce_opengl/juce_opengl.h>
#include <cmath>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#include "source/diag/Y2KDiagLog.h"
#include "source/ui/GlDirectModule.h"
#include "source/ui/PinkXPStyle.h"
#include "source/Y2KLogging.h"

// ================================================================
// GlDirectRenderer —— 跨宿主可复用的 GL 直渲染统一管线（v3.5）
//
//   由 Y2KmeterAudioProcessorEditor（嵌入态）与 FloatingModuleWindow（脱离态）
//   共同持有，确保所有 8 个 GlDirectModuleBase 模块在停靠与独立浮窗两种模式下
//   走完全一致的 GPU 渲染管线（同一套着色器、2x SSAA、多通道残影 FBO、列主序
//   增量纹理补丁、CRT/Glass/Neon 画布后处理与透明覆层预乘合成）。
//
//   每上下文隔离设计：
//     · 每个 OpenGLContext 拥有独立的 GlDirectRenderer 实例，内部以单调递增的
//       glResourceKey 维护本上下文创建的 VBO / FBO / Texture / TrailFBO；
//     · 模块在 Editor ↔ FloatingModuleWindow 间迁移时，旧宿主仅通过
//       scheduleReleaseModule(key) 在下一次 GL 帧或上下文关闭时回收旧句柄，
//       新宿主懒创建新句柄 —— 彻底杜绝跨 HGLRC 句柄串扰与 UI 线程阻塞。
// ================================================================
class GlDirectRenderer
{
public:
    GlDirectRenderer() = default;
    ~GlDirectRenderer() = default;

    struct FrameGuard
    {
        bool prevSrgb = false;
        bool active   = false;
    };

    static bool isGloballyDisabled() noexcept
    {
        static const bool disabled =
            juce::SystemStats::getEnvironmentVariable ("Y2K_NO_S3D_GL", "").isNotEmpty();
        return disabled;
    }

    bool isReady() const noexcept { return ! isGloballyDisabled() && glReady_; }

    // 必须在目标 OpenGLContext 当前激活的 GL 线程上调用
    bool ensureInitialised (juce::OpenGLContext& ctx)
    {
        using namespace juce::gl;

        if (isGloballyDisabled())
            return false;

        if (initChecked_)
            return glReady_;

        initChecked_ = true;

        const char* vs =
            "attribute vec2 position;\n"
            "attribute vec4 colour;\n"
            "uniform vec4 transform;\n"
            "uniform vec4 ndcScale;\n"
            "uniform float snapMode;\n"
            "varying vec4 vColour;\n"
            "void main()\n"
            "{\n"
            "  vec2 px = position * transform.xy + transform.zw;\n"
            "  if (snapMode > 0.5) px = floor (px) + vec2 (0.5);\n"
            "  gl_Position = vec4 (px * ndcScale.xy + ndcScale.zw, 0.0, 1.0);\n"
            "  vColour = colour;\n"
            "}\n";
        const char* fs =
            "varying vec4 vColour;\n"
            "void main() { gl_FragColor = vColour; }\n";

        shader_ = std::make_unique<juce::OpenGLShaderProgram> (ctx);
        if (shader_->addVertexShader (juce::OpenGLHelpers::translateVertexShaderToV3 (vs))
            && shader_->addFragmentShader (juce::OpenGLHelpers::translateFragmentShaderToV3 (fs))
            && shader_->link())
        {
            shader_->use();
            transformUniform_.reset (new juce::OpenGLShaderProgram::Uniform (*shader_, "transform"));
            ndcUniform_.reset (new juce::OpenGLShaderProgram::Uniform (*shader_, "ndcScale"));
            snapUniform_.reset (new juce::OpenGLShaderProgram::Uniform (*shader_, "snapMode"));
            glUseProgram (shader_->getProgramID());
            posAttrib_ = (int) glGetAttribLocation (shader_->getProgramID(), "position");
            colAttrib_ = (int) glGetAttribLocation (shader_->getProgramID(), "colour");

            glGenVertexArrays (1, &vao_);
            glBindVertexArray (vao_);
            glBindVertexArray (0);
            glBindBuffer (GL_ARRAY_BUFFER, 0);
            glGenBuffers (1, &quadVbo_);

            glReady_ = (posAttrib_ >= 0 && colAttrib_ >= 0);
        }

        Y2K_LOG (glReady_ ? juce::String ("[GlDirect] vertex shader link OK")
                          : juce::String ("[GlDirect] vertex shader link FAILED"));
        Y2K_DIAGC ("GL", juce::String ("s3d vertex shader link ")
                              + (glReady_ ? "OK" : "FAILED（模块将回退软件绘制）"));

        // 纹理通道 + trail FBO 合成着色器
        const char* tvs =
            "attribute vec2 position;\n"
            "attribute vec2 texCoord;\n"
            "uniform vec4 transform;\n"
            "uniform vec4 ndcScale;\n"
            "varying vec2 vUv;\n"
            "void main()\n"
            "{\n"
            "  vec2 px = position * transform.xy + transform.zw;\n"
            "  gl_Position = vec4 (px * ndcScale.xy + ndcScale.zw, 0.0, 1.0);\n"
            "  vUv = texCoord;\n"
            "}\n";
        const char* tfs =
            "varying vec2 vUv;\n"
            "uniform sampler2D tex;\n"
            "uniform vec2 texWindow;\n"
            "void main()\n"
            "{\n"
            "  vec2 uv = vec2 (vUv.x * texWindow.y + texWindow.x, vUv.y);\n"
            "  gl_FragColor = texture2D (tex, uv);\n"
            "}\n";

        texShader_ = std::make_unique<juce::OpenGLShaderProgram> (ctx);
        if (texShader_->addVertexShader (juce::OpenGLHelpers::translateVertexShaderToV3 (tvs))
            && texShader_->addFragmentShader (juce::OpenGLHelpers::translateFragmentShaderToV3 (tfs))
            && texShader_->link())
        {
            texShader_->use();
            texTransformUniform_.reset (new juce::OpenGLShaderProgram::Uniform (*texShader_, "transform"));
            texNdcUniform_.reset (new juce::OpenGLShaderProgram::Uniform (*texShader_, "ndcScale"));
            texWindowUniform_.reset (new juce::OpenGLShaderProgram::Uniform (*texShader_, "texWindow"));
            glUseProgram (texShader_->getProgramID());
            texPosAttrib_ = (int) glGetAttribLocation (texShader_->getProgramID(), "position");
            texUvAttrib_  = (int) glGetAttribLocation (texShader_->getProgramID(), "texCoord");
            if (texPosAttrib_ < 0 || texUvAttrib_ < 0)
                texShader_.reset();
        }
        else
        {
            texShader_.reset();
        }

        // 轻量画布视觉风格后处理着色器（CRT 扫描线+暗角 / Glass 顶部穹顶高光 / Neon 柔光内沿）
        const char* fxVs =
            "attribute vec2 position;\n"
            "attribute vec2 texCoord;\n"
            "uniform vec4 transform;\n"
            "uniform vec4 ndcScale;\n"
            "varying vec2 vUv;\n"
            "void main()\n"
            "{\n"
            "  vec2 px = position * transform.xy + transform.zw;\n"
            "  gl_Position = vec4 (px * ndcScale.xy + ndcScale.zw, 0.0, 1.0);\n"
            "  vUv = texCoord;\n"
            "}\n";
        const char* fxFs =
            "varying vec2 vUv;\n"
            "uniform vec4 fxParams;\n"
            "uniform vec4 fxTint;\n"
            "uniform vec2 fxRectSize;\n"
            "void main()\n"
            "{\n"
            "  vec3 rgb = vec3 (0.0);\n"
            "  float alpha = 0.0;\n"
            "  if (fxParams.x > 0.001 || fxParams.y > 0.001)\n"
            "  {\n"
            "    float pitch = max (2.0, fxTint.a);\n"
            "    float row = floor (vUv.y * fxRectSize.y);\n"
            "    float scan = (mod (row, pitch) >= 1.0) ? fxParams.x : 0.0;\n"
            "    vec2 d = (vUv - vec2 (0.5)) * 2.0;\n"
            "    float vig = clamp ((dot (d, d) - 0.50) * 0.72, 0.0, 1.0) * fxParams.y;\n"
            "    alpha = clamp (scan + vig, 0.0, 0.38);\n"
            "    rgb = vec3 (0.0);\n"
            "  }\n"
            "  if (fxParams.z > 0.001)\n"
            "  {\n"
            "    float band = clamp (1.0 - vUv.y / 0.40, 0.0, 1.0);\n"
            "    float dx = (vUv.x - 0.5) * 2.0;\n"
            "    float xCurve = 1.0 - 0.18 * dx * dx;\n"
            "    float dome = band * band * xCurve * fxParams.z;\n"
            "    float topEdge = (vUv.y * fxRectSize.y < 1.25) ? (fxParams.z * 0.90) : 0.0;\n"
            "    float sheenA = clamp (dome + topEdge, 0.0, 0.30);\n"
            "    rgb = mix (rgb, vec3 (1.0), sheenA / max (alpha + sheenA, 0.0001));\n"
            "    alpha = clamp (alpha + sheenA * (1.0 - alpha), 0.0, 0.45);\n"
            "  }\n"
            "  if (fxParams.w > 0.001)\n"
            "  {\n"
            "    vec2 px = vUv * fxRectSize;\n"
            "    float edgeDist = min (min (px.x, fxRectSize.x - px.x),\n"
            "                          min (px.y, fxRectSize.y - px.y));\n"
            "    float rim = exp (-max (edgeDist, 0.0) * 0.48) * fxParams.w;\n"
            "    float rimA = clamp (rim, 0.0, 0.28);\n"
            "    rgb = mix (rgb, fxTint.rgb, rimA / max (alpha + rimA, 0.0001));\n"
            "    alpha = clamp (alpha + rimA * (1.0 - alpha), 0.0, 0.45);\n"
            "  }\n"
            "  if (alpha <= 0.001)\n"
            "    discard;\n"
            "  gl_FragColor = vec4 (rgb, alpha);\n"
            "}\n";

        skinFxShader_ = std::make_unique<juce::OpenGLShaderProgram> (ctx);
        if (skinFxShader_->addVertexShader (juce::OpenGLHelpers::translateVertexShaderToV3 (fxVs))
            && skinFxShader_->addFragmentShader (juce::OpenGLHelpers::translateFragmentShaderToV3 (fxFs))
            && skinFxShader_->link())
        {
            skinFxShader_->use();
            fxTransformUniform_.reset (new juce::OpenGLShaderProgram::Uniform (*skinFxShader_, "transform"));
            fxNdcUniform_.reset (new juce::OpenGLShaderProgram::Uniform (*skinFxShader_, "ndcScale"));
            fxParamsUniform_.reset (new juce::OpenGLShaderProgram::Uniform (*skinFxShader_, "fxParams"));
            fxTintUniform_.reset (new juce::OpenGLShaderProgram::Uniform (*skinFxShader_, "fxTint"));
            fxRectSizeUniform_.reset (new juce::OpenGLShaderProgram::Uniform (*skinFxShader_, "fxRectSize"));
            glUseProgram (skinFxShader_->getProgramID());
            fxPosAttrib_ = (int) glGetAttribLocation (skinFxShader_->getProgramID(), "position");
            fxUvAttrib_  = (int) glGetAttribLocation (skinFxShader_->getProgramID(), "texCoord");
            if (fxPosAttrib_ < 0 || fxUvAttrib_ < 0)
                resetSkinFxShader();
        }
        else
        {
            resetSkinFxShader();
        }

        return glReady_;
    }

    FrameGuard beginFrame (int vw, int vh)
    {
        using namespace juce::gl;
        FrameGuard g;
        g.prevSrgb = (glIsEnabled (GL_FRAMEBUFFER_SRGB) != GL_FALSE);
        g.active   = true;

        flushPendingReleases();

        glBindFramebuffer (GL_FRAMEBUFFER, 0);
        glViewport (0, 0, vw, vh);
        glDisable (GL_CULL_FACE);
        glDisable (GL_SCISSOR_TEST);
        glDisable (GL_STENCIL_TEST);
        glDisable (GL_DEPTH_TEST);
        glDisable (GL_FRAMEBUFFER_SRGB);
        glEnable (GL_BLEND);
        glBlendFuncSeparate (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        if (isReady())
        {
            shader_->use();
            ndcUniform_->set (2.0f / (float) vw, 2.0f / (float) vh, -1.0f, -1.0f);
            glBindVertexArray (vao_);
        }
        return g;
    }

    void endFrame (const FrameGuard& g)
    {
        using namespace juce::gl;
        if (! g.active)
            return;

        if (isReady())
        {
            glBindVertexArray (0);
            glBindBuffer (GL_ARRAY_BUFFER, 0);
            glUseProgram (0);
            glEnable (GL_BLEND);
            glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }

        if (g.prevSrgb)
            glEnable (GL_FRAMEBUFFER_SRGB);
        else
            glDisable (GL_FRAMEBUFFER_SRGB);
    }

    void renderModule (juce::Component& hostComponent,
                       GlDirectModuleBase& mod,
                       float dpiScale,
                       int hostLogicalH,
                       int hostPhysH,
                       int vw,
                       int vh)
    {
        using namespace juce::gl;
        if (! isReady())
            return;

        const bool dOn = y2k::diag::isEnabled();
        const double dStart = dOn ? juce::Time::getMillisecondCounterHiRes() : 0.0;
        const auto& skinTokens = PinkXP::getSkinTokens();
        const bool isTransparentBackdrop = PinkXP::isTransparentBackdrop();
        constexpr GLenum fboFilter = GL_LINEAR;

        auto diagCheckGl = [dOn] (const char* where)
        {
            if (! dOn) return;
            const GLenum e = juce::gl::glGetError();
            if (e != juce::gl::GL_NO_ERROR)
                Y2K_DIAG ("GL", juce::String ("module GL error @ ") + where
                                      + " = 0x" + juce::String::toHexString ((int) e));
        };

        auto snap = mod.getGlSnapshot();
        if (snap == nullptr)
        {
            mod.diagSetVisible (true);
            return;
        }

        auto& gpu = moduleStates_[mod.glResourceKey];

        if ((snap->segments.empty() || snap->vertices.empty()) && snap->texW <= 0 && ! snap->trailFbo)
        {
            if (isTransparentBackdrop)
            {
                gpu.lastUploadedVersion = snap->version;
                if (! mod.glDirectActive.load())
                {
                    mod.glDirectActive.store (true);
                    mod.repaint();
                }
            }
            mod.diagSetVisible (true);
            return;
        }

        // 状态重置：避免 projectM 或其它组件绘制残留破坏 2D 直绘
        glBindFramebuffer (GL_FRAMEBUFFER, 0);
        glViewport (0, 0, vw, vh);
        glDisable (GL_CULL_FACE);
        glDisable (GL_SCISSOR_TEST);
        glDisable (GL_STENCIL_TEST);
        glDisable (GL_DEPTH_TEST);
        glDisable (GL_RASTERIZER_DISCARD);
        glDisable (GL_FRAMEBUFFER_SRGB);
        glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask (GL_FALSE);
        glBlendEquation (GL_FUNC_ADD);
        glEnable (GL_BLEND);
        glBlendFuncSeparate (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glActiveTexture (GL_TEXTURE0);
        glBindBuffer (GL_PIXEL_UNPACK_BUFFER, 0);
        glBindVertexArray (vao_);

        // 已接管模块先清底色，防止 FBO/blit 异常产生上一帧拖影（透明模式跳过）
        if (! isTransparentBackdrop && mod.glDirectActive.load (std::memory_order_relaxed))
        {
            const auto tl = hostComponent.getLocalPoint (&mod, juce::Point<int> (0, 0));
            const auto br = hostComponent.getLocalPoint (&mod, juce::Point<int> (mod.getWidth(), mod.getHeight()));
            const int gx = (int) std::floor ((double) tl.x * dpiScale);
            const int gy = (int) std::floor ((double) (hostLogicalH - br.y) * dpiScale);
            const int gw = juce::jmax (1, (int) std::ceil ((double) br.x * dpiScale) - gx);
            const int gh = juce::jmax (1, (int) std::ceil ((double) (hostLogicalH - tl.y) * dpiScale) - gy);

            const auto c = PinkXP::isGreenScreenBackdrop() ? PinkXP::chromaKeyGreen() : PinkXP::face;
            glEnable (GL_SCISSOR_TEST);
            glScissor (gx, gy, gw, gh);
            glClearColor (c.getFloatRed(), c.getFloatGreen(), c.getFloatBlue(), 1.0f);
            glClear (GL_COLOR_BUFFER_BIT);
            glDisable (GL_SCISSOR_TEST);
        }
        diagCheckGl ("state-reset");

        if (dOn)
            y2k::diag::setPhase (mod.diagRenderPhase.c_str());

        struct DiagRenderScope
        {
            GlDirectModuleBase& m;
            double startMs;
            bool   on;
            ~DiagRenderScope()
            {
                if (on)
                    m.diagOnRendered (juce::Time::getMillisecondCounterHiRes() - startMs);
            }
        } diagRenderScope { mod, dStart, dOn };
        juce::ignoreUnused (diagRenderScope);

        const bool needUpload = (snap->version != gpu.lastUploadedVersion);
        if (gpu.glVbo == 0)
            glGenBuffers (1, &gpu.glVbo);

        auto setVertexAttribs = [&] ()
        {
            glBindBuffer (GL_ARRAY_BUFFER, gpu.glVbo);
            glVertexAttribPointer ((GLuint) posAttrib_, 2, GL_FLOAT, GL_FALSE, 24, nullptr);
            glEnableVertexAttribArray ((GLuint) posAttrib_);
            glVertexAttribPointer ((GLuint) colAttrib_, 4, GL_FLOAT, GL_FALSE, 24, (void*) 8);
            glEnableVertexAttribArray ((GLuint) colAttrib_);
        };

        auto drawTexQuad = [&] (GLuint tex, juce::Rectangle<int> target,
                                float scrollU, float winW, bool transposed = false)
        {
            const auto tl = hostComponent.getLocalPoint (&mod, juce::Point<int>());
            glActiveTexture (GL_TEXTURE0);
            glBindTexture (GL_TEXTURE_2D, tex);
            texShader_->use();
            texNdcUniform_->set (2.0f / (float) vw, 2.0f / (float) vh, -1.0f, -1.0f);
            texTransformUniform_->set (dpiScale, -dpiScale,
                                       (float) tl.x * dpiScale,
                                       (float) hostPhysH - (float) tl.y * dpiScale);
            const float x0 = (float) target.getX(), y0 = (float) target.getY();
            const float x1 = (float) target.getRight(), y1 = (float) target.getBottom();
            float quad[24];
            if (transposed)
            {
                texWindowUniform_->set (0.0f, 1.0f);
                const float vL = scrollU;
                const float vR = scrollU + winW;
                const float q[24] = {
                    x0, y0, 0.0f, vL,  x1, y0, 0.0f, vR,  x0, y1, 1.0f, vL,
                    x1, y0, 0.0f, vR,  x1, y1, 1.0f, vR,  x0, y1, 1.0f, vL,
                };
                std::memcpy (quad, q, sizeof (quad));
            }
            else
            {
                texWindowUniform_->set (scrollU, winW);
                const float q[24] = {
                    x0, y0, 0.0f, 1.0f,  x1, y0, 1.0f, 1.0f,  x0, y1, 0.0f, 0.0f,
                    x1, y0, 1.0f, 1.0f,  x1, y1, 1.0f, 0.0f,  x0, y1, 0.0f, 0.0f,
                };
                std::memcpy (quad, q, sizeof (quad));
            }
            glBindBuffer (GL_ARRAY_BUFFER, quadVbo_);
            glVertexAttribPointer ((GLuint) texPosAttrib_, 2, GL_FLOAT, GL_FALSE, 16, nullptr);
            glEnableVertexAttribArray ((GLuint) texPosAttrib_);
            glVertexAttribPointer ((GLuint) texUvAttrib_, 2, GL_FLOAT, GL_FALSE, 16, (void*) 8);
            glEnableVertexAttribArray ((GLuint) texUvAttrib_);
            glBufferData (GL_ARRAY_BUFFER, sizeof (quad), quad, GL_DYNAMIC_DRAW);
            glDrawArrays (GL_TRIANGLES, 0, 6);
        };

        auto drawSkinFxQuad = [&] (juce::Rectangle<int> target)
        {
            if (skinFxShader_ == nullptr)
                return;

            const auto tl = hostComponent.getLocalPoint (&mod, juce::Point<int>());
            glBindFramebuffer (GL_FRAMEBUFFER, 0);
            glViewport (0, 0, vw, vh);
            glEnable (GL_BLEND);
            glBlendEquation (GL_FUNC_ADD);
            glBlendFuncSeparate (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

            skinFxShader_->use();
            fxNdcUniform_->set (2.0f / (float) vw, 2.0f / (float) vh, -1.0f, -1.0f);
            fxTransformUniform_->set (dpiScale, -dpiScale,
                                      (float) tl.x * dpiScale,
                                      (float) hostPhysH - (float) tl.y * dpiScale);

            const float scanAlpha  = (skinTokens.frameStyle == PinkXP::FrameStyle::crtPhosphor)
                                         ? juce::jlimit (0.0f, 0.10f, skinTokens.scanlineAlpha)
                                         : 0.0f;
            const float vigAlpha   = (skinTokens.frameStyle == PinkXP::FrameStyle::crtPhosphor)
                                         ? juce::jlimit (0.0f, 0.28f, skinTokens.vignetteAlpha)
                                         : 0.0f;
            const float sheenAlpha = (skinTokens.frameStyle == PinkXP::FrameStyle::frostedGlass)
                                         ? juce::jlimit (0.0f, 0.14f, skinTokens.surfaceSheenAlpha)
                                         : 0.0f;
            const float rimAlpha   = (skinTokens.frameStyle == PinkXP::FrameStyle::neonTube)
                                         ? juce::jlimit (0.0f, 0.24f, 0.21f * juce::jlimit (0.6f, 1.15f, skinTokens.glowIntensity))
                                         : 0.0f;
            fxParamsUniform_->set (scanAlpha, vigAlpha, sheenAlpha, rimAlpha);
            fxTintUniform_->set (PinkXP::sel.getFloatRed(),
                                 PinkXP::sel.getFloatGreen(),
                                 PinkXP::sel.getFloatBlue(),
                                 (float) juce::jmax (2, skinTokens.scanlinePitch));
            fxRectSizeUniform_->set ((float) target.getWidth(), (float) target.getHeight());

            const float x0 = (float) target.getX(), y0 = (float) target.getY();
            const float x1 = (float) target.getRight(), y1 = (float) target.getBottom();
            const float quad[24] = {
                x0, y0, 0.0f, 0.0f,  x1, y0, 1.0f, 0.0f,  x0, y1, 0.0f, 1.0f,
                x1, y0, 1.0f, 0.0f,  x1, y1, 1.0f, 1.0f,  x0, y1, 0.0f, 1.0f,
            };
            glBindBuffer (GL_ARRAY_BUFFER, quadVbo_);
            glVertexAttribPointer ((GLuint) fxPosAttrib_, 2, GL_FLOAT, GL_FALSE, 16, nullptr);
            glEnableVertexAttribArray ((GLuint) fxPosAttrib_);
            glVertexAttribPointer ((GLuint) fxUvAttrib_, 2, GL_FLOAT, GL_FALSE, 16, (void*) 8);
            glEnableVertexAttribArray ((GLuint) fxUvAttrib_);
            glBufferData (GL_ARRAY_BUFFER, sizeof (quad), quad, GL_DYNAMIC_DRAW);
            glDrawArrays (GL_TRIANGLES, 0, 6);
        };

        if (snap->renderToLogical && snap->logicalW > 0 && snap->logicalH > 0)
        {
            shader_->use();

            const bool needSsaa = ((juce::int64) snap->logicalW * (juce::int64) snap->logicalH * 4
                                   <= 16 * 1000 * 1000);
            const int kSsaa = needSsaa ? 2 : 1;
            const int fboW  = snap->logicalW * kSsaa;
            const int fboH  = snap->logicalH * kSsaa;

            if (gpu.glFbo == 0 || gpu.glFboW != fboW || gpu.glFboH != fboH)
            {
                if (gpu.glFbo != 0)
                {
                    glDeleteFramebuffers (1, &gpu.glFbo);
                    glDeleteTextures (1, &gpu.glFboTex);
                    gpu.glFbo = 0;
                    gpu.glFboTex = 0;
                }
                glGenFramebuffers (1, &gpu.glFbo);
                glGenTextures (1, &gpu.glFboTex);
                glBindTexture (GL_TEXTURE_2D, gpu.glFboTex);
                glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, fboW, fboH, 0,
                              GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint) fboFilter);
                glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint) fboFilter);
                glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glBindTexture (GL_TEXTURE_2D, 0);
                glBindFramebuffer (GL_FRAMEBUFFER, gpu.glFbo);
                glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                        GL_TEXTURE_2D, gpu.glFboTex, 0);
                if (dOn)
                {
                    const GLenum st = glCheckFramebufferStatus (GL_FRAMEBUFFER);
                    if (st != GL_FRAMEBUFFER_COMPLETE)
                        Y2K_DIAG ("GL", "module FBO incomplete: status=0x" + juce::String::toHexString ((int) st)
                                              + " size=" + juce::String (fboW) + "x" + juce::String (fboH));
                }
                glBindFramebuffer (GL_FRAMEBUFFER, 0);
                gpu.glFboW = fboW;
                gpu.glFboH = fboH;
            }

            Y2K_DIAG_PHASE ("gl:draw module (logical FBO)");
            glBindFramebuffer (GL_FRAMEBUFFER, gpu.glFbo);
            glViewport (0, 0, fboW, fboH);
            if (isTransparentBackdrop)
            {
                glClearColor (0.0f, 0.0f, 0.0f, 0.0f);
                glClear (GL_COLOR_BUFFER_BIT);
            }
            glBlendFuncSeparate (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            ndcUniform_->set (2.0f / (float) fboW, 2.0f / (float) fboH, -1.0f, -1.0f);
            transformUniform_->set ((float) kSsaa, (float) -kSsaa,
                                    -(float) snap->canvasLocal.getX() * (float) kSsaa,
                                    (float) (fboH + snap->canvasLocal.getY() * kSsaa));
            snapUniform_->set (0.0f);

            setVertexAttribs();
            if (needUpload && ! snap->vertices.empty())
                glBufferData (GL_ARRAY_BUFFER,
                              (GLsizeiptr) (snap->vertices.size() * sizeof (float)),
                              snap->vertices.data(), GL_DYNAMIC_DRAW);
            for (const auto& seg : snap->segments)
                if (seg.count > 0)
                    glDrawArrays (seg.primitive == GlDirectModuleBase::Primitive::Lines ? GL_LINES : GL_TRIANGLES,
                                  seg.offset, seg.count);
            diagCheckGl ("logical-draw");

            const auto cPos = hostComponent.getLocalPoint (&mod, snap->canvasLocal.getPosition());
            const auto cBR  = hostComponent.getLocalPoint (&mod, snap->canvasLocal.getBottomRight());

            if (isTransparentBackdrop && texShader_ != nullptr)
            {
                glBindFramebuffer (GL_FRAMEBUFFER, 0);
                glViewport (0, 0, vw, vh);
                glActiveTexture (GL_TEXTURE0);
                glBindTexture (GL_TEXTURE_2D, gpu.glFboTex);
                glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint) fboFilter);
                glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint) fboFilter);
                glEnable (GL_BLEND);
                glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                drawTexQuad (gpu.glFboTex, snap->canvasLocal, 0.0f, 1.0f);
                glBlendFuncSeparate (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                shader_->use();
            }
            else
            {
                if (! blitBroken_)
                {
                    const bool probeNow = ! blitProbed_;
                    if (probeNow)
                    {
                        blitProbed_ = true;
                        while (juce::gl::glGetError() != juce::gl::GL_NO_ERROR) {}
                    }

                    Y2K_DIAG_PHASE ("gl:BlitFramebuffer (module logical->screen)");
                    glBindFramebuffer (GL_READ_FRAMEBUFFER, gpu.glFbo);
                    glBindFramebuffer (GL_DRAW_FRAMEBUFFER, 0);
                    glViewport (0, 0, vw, vh);
                    glBlitFramebuffer (0, 0, fboW, fboH,
                                       (int) (cPos.x * dpiScale),
                                       (int) (hostPhysH - cBR.y * dpiScale),
                                       (int) (cBR.x * dpiScale),
                                       (int) (hostPhysH - cPos.y * dpiScale),
                                       GL_COLOR_BUFFER_BIT, fboFilter);
                    glBindFramebuffer (GL_READ_FRAMEBUFFER, 0);

                    if (probeNow)
                    {
                        const GLenum e = juce::gl::glGetError();
                        if (e != juce::gl::GL_NO_ERROR)
                        {
                            blitBroken_ = true;
                            Y2K_DIAGC ("GL", juce::String ("!! glBlitFramebuffer 失败 (0x")
                                                  + juce::String::toHexString ((int) e)
                                                  + ") → 直渲染模块改用纹理 quad 合成回退");
                        }
                    }
                }

                if (blitBroken_ && texShader_ != nullptr)
                {
                    glBindFramebuffer (GL_FRAMEBUFFER, 0);
                    glViewport (0, 0, vw, vh);
                    glActiveTexture (GL_TEXTURE0);
                    glBindTexture (GL_TEXTURE_2D, gpu.glFboTex);
                    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint) fboFilter);
                    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint) fboFilter);
                    glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                    drawTexQuad (gpu.glFboTex, snap->canvasLocal, 0.0f, 1.0f);
                    glBlendFuncSeparate (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                    shader_->use();
                }
            }
            diagCheckGl ("logical-fbo-blit");
        }
        else if (snap->trailFbo && snap->texW > 0 && snap->texH > 0 && texShader_ != nullptr)
        {
            const int layerCount = juce::jlimit (1, 3, snap->trailLayers);

            if (gpu.glTrailW != snap->texW || gpu.glTrailH != snap->texH)
            {
                for (int li = 0; li < 3; ++li)
                {
                    if (gpu.glTrailFbo[li] != 0)
                    {
                        glDeleteFramebuffers (1, &gpu.glTrailFbo[li]);
                        glDeleteTextures (1, &gpu.glTrailTex[li]);
                        gpu.glTrailFbo[li] = 0;
                        gpu.glTrailTex[li] = 0;
                    }
                    gpu.glTrailCleared[li] = true;
                }
                gpu.glTrailW = snap->texW;
                gpu.glTrailH = snap->texH;
            }

            for (int li = 0; li < layerCount; ++li)
            {
                if (gpu.glTrailFbo[li] != 0)
                    continue;

                glGenFramebuffers (1, &gpu.glTrailFbo[li]);
                glGenTextures (1, &gpu.glTrailTex[li]);
                glBindTexture (GL_TEXTURE_2D, gpu.glTrailTex[li]);
                glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8,
                              snap->texW, snap->texH, 0,
                              GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint) fboFilter);
                glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint) fboFilter);
                glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glBindTexture (GL_TEXTURE_2D, 0);
                glBindFramebuffer (GL_FRAMEBUFFER, gpu.glTrailFbo[li]);
                glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                        GL_TEXTURE_2D, gpu.glTrailTex[li], 0);
                glBindFramebuffer (GL_FRAMEBUFFER, 0);
                gpu.glTrailCleared[li] = false;
            }

            Y2K_DIAG_PHASE ("gl:draw module (trail FBO)");

            setVertexAttribs();
            if (needUpload && ! snap->vertices.empty())
                glBufferData (GL_ARRAY_BUFFER,
                              (GLsizeiptr) (snap->vertices.size() * sizeof (float)),
                              snap->vertices.data(), GL_DYNAMIC_DRAW);

            const bool forceClearTrail = mod.glForceTrailClear.exchange (false, std::memory_order_acq_rel);
            for (int li = 0; li < layerCount; ++li)
            {
                glBindFramebuffer (GL_FRAMEBUFFER, gpu.glTrailFbo[li]);
                glViewport (0, 0, snap->texW, snap->texH);
                if (! gpu.glTrailCleared[li] || snap->trailClear || forceClearTrail)
                {
                    glClearColor (0.0f, 0.0f, 0.0f, 0.0f);
                    glClear (GL_COLOR_BUFFER_BIT);
                    gpu.glTrailCleared[li] = true;
                }

                shader_->use();
                ndcUniform_->set (2.0f / (float) snap->texW, 2.0f / (float) snap->texH, -1.0f, -1.0f);
                transformUniform_->set (1.0f, -1.0f, 0.0f, (float) snap->texH);
                snapUniform_->set (0.0f);
                glBlendFunc (GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
                {
                    const float w = (float) snap->texW, h = (float) snap->texH;
                    const float a = 1.0f - snap->trailDecay;
                    const float quad[36] = {
                        0.0f, 0.0f, 1, 1, 1, a,   w, 0.0f, 1, 1, 1, a,   0.0f, h, 1, 1, 1, a,
                        w, 0.0f, 1, 1, 1, a,      w, h, 1, 1, 1, a,      0.0f, h, 1, 1, 1, a,
                    };
                    glBindBuffer (GL_ARRAY_BUFFER, quadVbo_);
                    glVertexAttribPointer ((GLuint) posAttrib_, 2, GL_FLOAT, GL_FALSE, 24, nullptr);
                    glEnableVertexAttribArray ((GLuint) posAttrib_);
                    glVertexAttribPointer ((GLuint) colAttrib_, 4, GL_FLOAT, GL_FALSE, 24, (void*) 8);
                    glEnableVertexAttribArray ((GLuint) colAttrib_);
                    glBufferData (GL_ARRAY_BUFFER, sizeof (quad), quad, GL_DYNAMIC_DRAW);
                    glDrawArrays (GL_TRIANGLES, 0, 6);
                }

                // 残值清零 pass（dst -= 2/255，杜绝 8bit 量化灰点）
                {
                    constexpr float kFlush = 2.0f / 255.0f;
                    const float w = (float) snap->texW, h = (float) snap->texH;
                    const float quad[36] = {
                        0.0f, 0.0f, kFlush, kFlush, kFlush, kFlush,
                        w, 0.0f, kFlush, kFlush, kFlush, kFlush,
                        0.0f, h, kFlush, kFlush, kFlush, kFlush,
                        w, 0.0f, kFlush, kFlush, kFlush, kFlush,
                        w, h, kFlush, kFlush, kFlush, kFlush,
                        0.0f, h, kFlush, kFlush, kFlush, kFlush,
                    };
                    glBlendEquation (GL_FUNC_REVERSE_SUBTRACT);
                    glBlendFunc (GL_ONE, GL_ONE);
                    glBindBuffer (GL_ARRAY_BUFFER, quadVbo_);
                    glBufferData (GL_ARRAY_BUFFER, sizeof (quad), quad, GL_DYNAMIC_DRAW);
                    glDrawArrays (GL_TRIANGLES, 0, 6);
                    glBlendEquation (GL_FUNC_ADD);
                }

                glBlendFuncSeparate (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                transformUniform_->set (1.0f, -1.0f,
                                        -(float) snap->canvasLocal.getX(),
                                        (float) (snap->texH + snap->canvasLocal.getY()));
                setVertexAttribs();
                for (size_t si = (size_t) snap->trailSegStart; si < snap->segments.size(); ++si)
                {
                    const auto& seg = snap->segments[si];
                    if (seg.count <= 0 || juce::jlimit (0, layerCount - 1, seg.trailLayer) != li)
                        continue;
                    glDrawArrays (seg.primitive == GlDirectModuleBase::Primitive::Lines ? GL_LINES : GL_TRIANGLES,
                                  seg.offset, seg.count);
                }
            }

            glBindFramebuffer (GL_FRAMEBUFFER, 0);
            glViewport (0, 0, vw, vh);
            {
                const auto tl = hostComponent.getLocalPoint (&mod, juce::Point<int>());
                ndcUniform_->set (2.0f / (float) vw, 2.0f / (float) vh, -1.0f, -1.0f);
                transformUniform_->set (dpiScale, -dpiScale,
                                        (float) tl.x * dpiScale,
                                        (float) hostPhysH - (float) tl.y * dpiScale);
                snapUniform_->set (snap->snapToPixels ? 1.0f : 0.0f);
                setVertexAttribs();
                for (size_t si = 0; si < (size_t) snap->trailSegStart && si < snap->segments.size(); ++si)
                {
                    const auto& seg = snap->segments[si];
                    if (seg.count > 0)
                        glDrawArrays (seg.primitive == GlDirectModuleBase::Primitive::Lines ? GL_LINES : GL_TRIANGLES,
                                      seg.offset, seg.count);
                }
            }

            glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            for (int li = 0; li < layerCount; ++li)
            {
                glActiveTexture (GL_TEXTURE0);
                glBindTexture (GL_TEXTURE_2D, gpu.glTrailTex[li]);
                glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint) fboFilter);
                glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint) fboFilter);
                drawTexQuad (gpu.glTrailTex[li], snap->texTargetRect, 0.0f, 1.0f);
            }
            glBlendFuncSeparate (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            diagCheckGl ("trail-compose");
        }
        else
        {
            const auto tl = hostComponent.getLocalPoint (&mod, juce::Point<int>());
            Y2K_DIAG_PHASE ("gl:draw module (direct)");
            shader_->use();
            ndcUniform_->set (2.0f / (float) vw, 2.0f / (float) vh, -1.0f, -1.0f);
            transformUniform_->set (dpiScale, -dpiScale,
                                    (float) tl.x * dpiScale,
                                    (float) hostPhysH - (float) tl.y * dpiScale);
            snapUniform_->set (snap->snapToPixels ? 1.0f : 0.0f);

            setVertexAttribs();
            if (needUpload && ! snap->vertices.empty())
                glBufferData (GL_ARRAY_BUFFER,
                              (GLsizeiptr) (snap->vertices.size() * sizeof (float)),
                              snap->vertices.data(), GL_DYNAMIC_DRAW);
            for (const auto& seg : snap->segments)
                if (seg.count > 0)
                    glDrawArrays (seg.primitive == GlDirectModuleBase::Primitive::Lines ? GL_LINES : GL_TRIANGLES,
                                  seg.offset, seg.count);

            if (snap->texW > 0 && snap->texH > 0 && texShader_ != nullptr)
            {
                const bool needFullUpload = snap->texDirty
                                         || gpu.glTex == 0
                                         || gpu.glTexW != snap->texW
                                         || gpu.glTexH != snap->texH;
                const size_t requiredPixels = (size_t) snap->texW * (size_t) snap->texH;
                const GLint texFilter = snap->texLinear ? GL_LINEAR : GL_NEAREST;
                const GLint wrapT     = snap->texWrapRepeatT ? GL_REPEAT : GL_CLAMP_TO_EDGE;

                if (needFullUpload && ! snap->texPixels.empty() && snap->texPixels.size() >= requiredPixels)
                {
                    const bool hadTex = (gpu.glTex != 0);
                    if (! hadTex)
                        glGenTextures (1, &gpu.glTex);
                    if (gpu.glTex != 0)
                    {
                        Y2K_DIAG_PHASE ("gl:TexImage2D (module texture upload)");
                        glBindTexture (GL_TEXTURE_2D, gpu.glTex);
                        if (hadTex && gpu.glTexW == snap->texW && gpu.glTexH == snap->texH)
                        {
                            glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0,
                                             snap->texW, snap->texH,
                                             GL_BGRA, GL_UNSIGNED_BYTE, snap->texPixels.data());
                        }
                        else
                        {
                            glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8,
                                          snap->texW, snap->texH, 0,
                                          GL_BGRA, GL_UNSIGNED_BYTE, snap->texPixels.data());
                            gpu.glTexW = snap->texW;
                            gpu.glTexH = snap->texH;
                        }
                        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, texFilter);
                        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, texFilter);
                        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapT);
                        glBindTexture (GL_TEXTURE_2D, 0);
                        mod.glTexUploadedGen.store (snap->texDataGen, std::memory_order_release);
                    }
                }
                else if (! needFullUpload && ! snap->texRowPatches.empty()
                         && gpu.glTex != 0 && gpu.glTexW == snap->texW && gpu.glTexH == snap->texH)
                {
                    Y2K_DIAG_PHASE ("gl:TexSubImage2D (module incremental patch)");
                    glBindTexture (GL_TEXTURE_2D, gpu.glTex);
                    for (const auto& patch : snap->texRowPatches)
                    {
                        if (patch.rowCount > 0 && patch.rowStart >= 0
                            && patch.rowStart + patch.rowCount <= snap->texH
                            && patch.pixels.size() >= (size_t) snap->texW * (size_t) patch.rowCount)
                        {
                            glTexSubImage2D (GL_TEXTURE_2D, 0, 0, patch.rowStart,
                                             snap->texW, patch.rowCount,
                                             GL_BGRA, GL_UNSIGNED_BYTE, patch.pixels.data());
                        }
                    }
                    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, texFilter);
                    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, texFilter);
                    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapT);
                    glBindTexture (GL_TEXTURE_2D, 0);
                    mod.glTexUploadedGen.store (snap->texDataGen, std::memory_order_release);
                }

                if (gpu.glTex != 0 && gpu.glTexW == snap->texW && gpu.glTexH == snap->texH)
                {
                    if (snap->texPremultiplied)
                        glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                    drawTexQuad (gpu.glTex, snap->texTargetRect, snap->texScrollU, snap->texWinW, snap->texTransposed);
                    if (snap->texPremultiplied)
                        glBlendFuncSeparate (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                    shader_->use();
                }
            }
            diagCheckGl ("direct-draw");
        }

        if (! skinTokens.isChromaKey && ! isTransparentBackdrop
            && skinTokens.hasCanvasFx() && skinFxShader_ != nullptr)
        {
            const auto fxRect = ! snap->fxRect.isEmpty()
                                    ? snap->fxRect
                                    : (! snap->canvasLocal.isEmpty() ? snap->canvasLocal : snap->texTargetRect);
            if (fxRect.getWidth() > 2 && fxRect.getHeight() > 2)
            {
                drawSkinFxQuad (fxRect);
                shader_->use();
                glBlendFuncSeparate (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                diagCheckGl ("skin-fx");
            }
        }

        if (needUpload)
            gpu.lastUploadedVersion = snap->version;

        if (! mod.glDirectActive.load (std::memory_order_acquire))
        {
            mod.glDirectActive.store (true, std::memory_order_release);
            mod.repaint();
        }

        mod.diagSetVisible (true);
    }

    // 任意线程可调用：标记离场模块，下一帧或上下文关闭时在 GL 线程安全释放其 GPU 对象
    void scheduleReleaseModule (juce::uint64 resourceKey)
    {
        const juce::SpinLock::ScopedLockType sl (pendingReleaseLock_);
        pendingReleases_.push_back (resourceKey);
    }

    // GL 线程：回收所有待释放模块的 GPU 对象
    void flushPendingReleases()
    {
        std::vector<juce::uint64> toFree;
        {
            const juce::SpinLock::ScopedLockType sl (pendingReleaseLock_);
            if (pendingReleases_.empty())
                return;
            toFree.swap (pendingReleases_);
        }

        for (auto key : toFree)
        {
            auto it = moduleStates_.find (key);
            if (it != moduleStates_.end())
            {
                deleteModuleGpuState (it->second);
                moduleStates_.erase (it);
            }
        }
    }

    // GL 线程：仅保留当前仍在宿主树中的模块 GPU 状态，自动回收已关闭/切预设移除模块的 VBO/FBO/纹理
    void retainOnlyKeys (const juce::uint64* liveKeys, size_t count)
    {
        if (moduleStates_.empty())
            return;
        for (auto it = moduleStates_.begin(); it != moduleStates_.end(); )
        {
            bool alive = false;
            for (size_t i = 0; i < count; ++i)
            {
                if (liveKeys[i] == it->first) { alive = true; break; }
            }
            if (! alive)
            {
                deleteModuleGpuState (it->second);
                it = moduleStates_.erase (it);
            }
            else
            {
                ++it;
            }
        }
    }

    // GL 线程（openGLContextClosing）：释放本上下文拥有的全部 GL 对象
    void releaseAll()
    {
        using namespace juce::gl;

        if (vao_ != 0)     { glDeleteVertexArrays (1, &vao_);     vao_ = 0; }
        if (quadVbo_ != 0) { glDeleteBuffers (1, &quadVbo_);      quadVbo_ = 0; }

        for (auto& kv : moduleStates_)
            deleteModuleGpuState (kv.second);
        moduleStates_.clear();

        {
            const juce::SpinLock::ScopedLockType sl (pendingReleaseLock_);
            pendingReleases_.clear();
        }

        transformUniform_.reset();
        ndcUniform_.reset();
        snapUniform_.reset();
        shader_.reset();

        texTransformUniform_.reset();
        texNdcUniform_.reset();
        texWindowUniform_.reset();
        texShader_.reset();

        resetSkinFxShader();

        glReady_     = false;
        initChecked_ = false;
        blitProbed_  = false;
        blitBroken_  = false;
    }

private:
    struct ModuleGpuState
    {
        unsigned     glVbo = 0;
        unsigned     glFbo = 0, glFboTex = 0;
        int          glFboW = 0, glFboH = 0;
        unsigned     glTex = 0;
        int          glTexW = 0, glTexH = 0;
        unsigned     glTrailFbo[3] { 0, 0, 0 };
        unsigned     glTrailTex[3] { 0, 0, 0 };
        int          glTrailW = 0, glTrailH = 0;
        bool         glTrailCleared[3] { true, true, true };
        juce::uint64 lastUploadedVersion = 0;
    };

    void deleteModuleGpuState (ModuleGpuState& st)
    {
        using namespace juce::gl;
        if (st.glVbo != 0) { glDeleteBuffers (1, &st.glVbo); st.glVbo = 0; }
        if (st.glFbo != 0)
        {
            glDeleteFramebuffers (1, &st.glFbo);
            glDeleteTextures (1, &st.glFboTex);
            st.glFbo = 0;
            st.glFboTex = 0;
            st.glFboW = 0;
            st.glFboH = 0;
        }
        for (int li = 0; li < 3; ++li)
        {
            if (st.glTrailFbo[li] != 0)
            {
                glDeleteFramebuffers (1, &st.glTrailFbo[li]);
                glDeleteTextures (1, &st.glTrailTex[li]);
                st.glTrailFbo[li] = 0;
                st.glTrailTex[li] = 0;
            }
            st.glTrailCleared[li] = true;
        }
        st.glTrailW = 0;
        st.glTrailH = 0;
        if (st.glTex != 0) { glDeleteTextures (1, &st.glTex); st.glTex = 0; }
        st.glTexW = 0;
        st.glTexH = 0;
        st.lastUploadedVersion = 0;
    }

    void resetSkinFxShader()
    {
        fxTransformUniform_.reset();
        fxNdcUniform_.reset();
        fxParamsUniform_.reset();
        fxTintUniform_.reset();
        fxRectSizeUniform_.reset();
        skinFxShader_.reset();
        fxPosAttrib_ = -1;
        fxUvAttrib_  = -1;
    }

    std::unordered_map<juce::uint64, ModuleGpuState> moduleStates_;
    juce::SpinLock pendingReleaseLock_;
    std::vector<juce::uint64> pendingReleases_;

    std::unique_ptr<juce::OpenGLShaderProgram>          shader_;
    std::unique_ptr<juce::OpenGLShaderProgram::Uniform> transformUniform_;
    std::unique_ptr<juce::OpenGLShaderProgram::Uniform> ndcUniform_;
    std::unique_ptr<juce::OpenGLShaderProgram::Uniform> snapUniform_;

    std::unique_ptr<juce::OpenGLShaderProgram>          texShader_;
    std::unique_ptr<juce::OpenGLShaderProgram::Uniform> texTransformUniform_;
    std::unique_ptr<juce::OpenGLShaderProgram::Uniform> texNdcUniform_;
    std::unique_ptr<juce::OpenGLShaderProgram::Uniform> texWindowUniform_;

    std::unique_ptr<juce::OpenGLShaderProgram>          skinFxShader_;
    std::unique_ptr<juce::OpenGLShaderProgram::Uniform> fxTransformUniform_;
    std::unique_ptr<juce::OpenGLShaderProgram::Uniform> fxNdcUniform_;
    std::unique_ptr<juce::OpenGLShaderProgram::Uniform> fxParamsUniform_;
    std::unique_ptr<juce::OpenGLShaderProgram::Uniform> fxTintUniform_;
    std::unique_ptr<juce::OpenGLShaderProgram::Uniform> fxRectSizeUniform_;

    unsigned int vao_     = 0;
    unsigned int quadVbo_ = 0;
    int posAttrib_ = 0, colAttrib_ = 1;
    int texPosAttrib_ = 0, texUvAttrib_ = 1;
    int fxPosAttrib_ = -1, fxUvAttrib_ = -1;

    bool initChecked_ = false;
    bool glReady_     = false;
    bool blitProbed_  = false;
    bool blitBroken_  = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GlDirectRenderer)
};
