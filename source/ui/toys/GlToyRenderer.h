#pragma once

// ==========================================================
// GlToyRenderer —— Direct OpenGL Batched ToyBox Sprite Pass
//
//   Runs in OpenGLRenderer::renderOpenGLPostComponentBuffer(),
//   immediately AFTER JUCE's drawComponentBuffer() and AFTER
//   MessageManager::Lock has been released — zero UI-thread
//   blocking and zero ToyOverlay repaint amplification!
//
//   Architecture:
//     - Single 1024x1024 RGBA8 premultiplied emoji atlas texture
//       holding all 97 x 3 size tiers with 1px transparent gutters.
//     - Incremental glTexSubImage2D upload as EmojiAtlas bakes slots.
//     - Single uber-shader + single dynamic VBO (with VBO orphaning)
//       drawing up to 400 Party-mode emojis + radial rainbow halos +
//       optional dots in ONE glDrawElements call per frame.
//     - Fragment-level exclusion masking for in-window drawers,
//       tooltips, PalettePanel, TutorialOverlay, hide-mode toolbars.
//     - Premultiplied alpha blending (GL_ONE, GL_ONE_MINUS_SRC_ALPHA)
//       compatible with Normal, GreenScreen, and Windows DWM
//       hardware Transparent backdrop modes.
// ==========================================================

#include <JuceHeader.h>
#include <juce_opengl/juce_opengl.h>

#include "source/ui/toys/ToyGlTypes.h"
#include "source/ui/toys/ToyKit.h"
#include "source/diag/Y2KDiagLog.h"
#include "source/Y2KLogging.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace y2ktoy
{
    // Shared GLSL shader source (reused by tests/toybox_bench.cpp)
    struct GlToyShaderSource
    {
        static const char* vertexShader() noexcept
        {
            return
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
        }

        static const char* fragmentShader() noexcept
        {
            return
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
        }
    };

    class GlToyRenderer
    {
    public:
        GlToyRenderer() = default;
        ~GlToyRenderer() = default;

        // Called on GL thread from Editor::renderOpenGLPostComponentBuffer()
        // (OUTSIDE MessageManager::Lock — never touches JUCE Components!).
        // Returns false ONLY if GL shader/buffer initialization permanently failed.
        bool render (ToyDirector& dir, juce::OpenGLContext& ctx)
        {
            using namespace juce::gl;

            if (! ensureInitialised (ctx))
                return false;

            const auto snap = dir.getGlToySnapshot();
            if (snap == nullptr || snap->sprites.empty()
                || snap->editorLogicalW <= 0 || snap->editorLogicalH <= 0)
            {
                dir.consumeGlToyClearPending();
                return true;
            }

            // Save ALL GL state we mutate (restored via RAII guard on return)
            struct GlStateGuard
            {
                GLint prevProg = 0, prevVao = 0, prevVbo = 0;
                GLint prevActiveTex = GL_TEXTURE0, prevTex2D = 0;
                GLint prevBlendSrcRgb = GL_ONE, prevBlendDstRgb = GL_ZERO;
                GLint prevBlendSrcA = GL_ONE, prevBlendDstA = GL_ZERO;
                GLint prevBlendEqRgb = GL_FUNC_ADD, prevBlendEqA = GL_FUNC_ADD;
                GLint prevUnpackAlign = 4, prevUnpackRowLen = 0;
                GLboolean prevBlend = GL_FALSE, prevScissor = GL_FALSE;
                GLboolean prevDepth = GL_FALSE, prevCull = GL_FALSE;
                GLboolean prevColorMask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };

                GlStateGuard() noexcept
                {
                    glGetIntegerv (GL_CURRENT_PROGRAM, &prevProg);
                    glGetIntegerv (GL_VERTEX_ARRAY_BINDING, &prevVao);
                    glGetIntegerv (GL_ARRAY_BUFFER_BINDING, &prevVbo);
                    glGetIntegerv (GL_ACTIVE_TEXTURE, &prevActiveTex);
                    glActiveTexture (GL_TEXTURE0);
                    glGetIntegerv (GL_TEXTURE_BINDING_2D, &prevTex2D);
                    glGetIntegerv (GL_BLEND_SRC_RGB, &prevBlendSrcRgb);
                    glGetIntegerv (GL_BLEND_DST_RGB, &prevBlendDstRgb);
                    glGetIntegerv (GL_BLEND_SRC_ALPHA, &prevBlendSrcA);
                    glGetIntegerv (GL_BLEND_DST_ALPHA, &prevBlendDstA);
                    glGetIntegerv (GL_BLEND_EQUATION_RGB, &prevBlendEqRgb);
                    glGetIntegerv (GL_BLEND_EQUATION_ALPHA, &prevBlendEqA);
                    glGetIntegerv (GL_UNPACK_ALIGNMENT, &prevUnpackAlign);
                   #ifdef GL_UNPACK_ROW_LENGTH
                    glGetIntegerv (GL_UNPACK_ROW_LENGTH, &prevUnpackRowLen);
                   #endif
                    prevBlend   = glIsEnabled (GL_BLEND);
                    prevScissor = glIsEnabled (GL_SCISSOR_TEST);
                    prevDepth   = glIsEnabled (GL_DEPTH_TEST);
                    prevCull    = glIsEnabled (GL_CULL_FACE);
                    glGetBooleanv (GL_COLOR_WRITEMASK, prevColorMask);
                }

                ~GlStateGuard() noexcept
                {
                    glActiveTexture (GL_TEXTURE0);
                    glBindTexture (GL_TEXTURE_2D, (GLuint) prevTex2D);
                    glActiveTexture ((GLenum) prevActiveTex);
                    // EBO binding is VAO-owned state; restoring prevVao restores its EBO safely in GL 3.2 Core Profile.
                    glBindVertexArray ((GLuint) prevVao);
                    glBindBuffer (GL_ARRAY_BUFFER, (GLuint) prevVbo);
                    glUseProgram ((GLuint) prevProg);
                    glPixelStorei (GL_UNPACK_ALIGNMENT, prevUnpackAlign);
                   #ifdef GL_UNPACK_ROW_LENGTH
                    glPixelStorei (GL_UNPACK_ROW_LENGTH, prevUnpackRowLen);
                   #endif
                    if (prevBlend)   glEnable (GL_BLEND);        else glDisable (GL_BLEND);
                    if (prevScissor) glEnable (GL_SCISSOR_TEST); else glDisable (GL_SCISSOR_TEST);
                    if (prevDepth)   glEnable (GL_DEPTH_TEST);   else glDisable (GL_DEPTH_TEST);
                    if (prevCull)    glEnable (GL_CULL_FACE);    else glDisable (GL_CULL_FACE);
                    glBlendFuncSeparate ((GLenum) prevBlendSrcRgb, (GLenum) prevBlendDstRgb,
                                         (GLenum) prevBlendSrcA,   (GLenum) prevBlendDstA);
                    glBlendEquationSeparate ((GLenum) prevBlendEqRgb, (GLenum) prevBlendEqA);
                    glColorMask (prevColorMask[0], prevColorMask[1], prevColorMask[2], prevColorMask[3]);
                }
            } stateGuard;

            // 1) Sync dirty emoji slots into our single 1024x1024 premultiplied RGBA8 atlas
            if (dir.atlas != nullptr)
                syncAtlasTextures (*dir.atlas);

            // 2) Pack all sprites (Party halo + emoji/dot) into one contiguous quad batch
            packToySpritesToQuads (snap->sprites.data(), snap->sprites.size(),
                                   vertsScratch_, indicesScratch_);
            if (indicesScratch_.empty())
            {
                dir.consumeGlToyClearPending();
                return true;
            }

            // 3) Configure state & premultiplied Porter-Duff Source-Over blending
            glDisable (GL_SCISSOR_TEST);
            glDisable (GL_DEPTH_TEST);
            glDisable (GL_CULL_FACE);
            glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glEnable (GL_BLEND);
            glBlendEquationSeparate (GL_FUNC_ADD, GL_FUNC_ADD);
            glBlendFuncSeparate (GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
                                 GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

            glUseProgram (shader_->getProgramID());
            glActiveTexture (GL_TEXTURE0);
            glBindTexture (GL_TEXTURE_2D, atlasTex_);

            if (uAtlasLoc_ >= 0)
                glUniform1i (uAtlasLoc_, 0);
            if (uEditorSizeLoc_ >= 0)
                glUniform2f (uEditorSizeLoc_, (float) snap->editorLogicalW, (float) snap->editorLogicalH);

            const int exCount = juce::jlimit (0, kMaxExclusionRects, snap->exclusionCount);
            if (uExclusionCountLoc_ >= 0)
                glUniform1i (uExclusionCountLoc_, exCount);
            if (exCount > 0 && uExclusionsLoc_ >= 0)
            {
                float exBuf[kMaxExclusionRects * 4];
                for (int i = 0; i < exCount; ++i)
                {
                    exBuf[i * 4 + 0] = snap->exclusions[(size_t) i].x0;
                    exBuf[i * 4 + 1] = snap->exclusions[(size_t) i].y0;
                    exBuf[i * 4 + 2] = snap->exclusions[(size_t) i].x1;
                    exBuf[i * 4 + 3] = snap->exclusions[(size_t) i].y1;
                }
                glUniform4fv (uExclusionsLoc_, exCount, exBuf);
            }

            // 4) Upload vertices + indices with VBO orphaning (zero GPU stall) and issue 1 draw call
            glBindVertexArray (vao_);
            const GLsizeiptr vBytes = (GLsizeiptr) (vertsScratch_.size() * sizeof (GlToyVertex));
            const GLsizeiptr iBytes = (GLsizeiptr) (indicesScratch_.size() * sizeof (uint16_t));

            glBindBuffer (GL_ARRAY_BUFFER, vbo_);
            vboCapacityBytes_ = std::max (vboCapacityBytes_, vBytes);
            glBufferData (GL_ARRAY_BUFFER, vboCapacityBytes_, nullptr, GL_STREAM_DRAW);
            glBufferSubData (GL_ARRAY_BUFFER, 0, vBytes, vertsScratch_.data());

            glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, ebo_);
            eboCapacityBytes_ = std::max (eboCapacityBytes_, iBytes);
            glBufferData (GL_ELEMENT_ARRAY_BUFFER, eboCapacityBytes_, nullptr, GL_STREAM_DRAW);
            glBufferSubData (GL_ELEMENT_ARRAY_BUFFER, 0, iBytes, indicesScratch_.data());

            glDrawElements (GL_TRIANGLES, (GLsizei) indicesScratch_.size(), GL_UNSIGNED_SHORT, nullptr);

            dir.consumeGlToyClearPending();
            return true;
        }

        // Must be called on GL thread inside openGLContextClosing()
        void releaseResources() noexcept
        {
            using namespace juce::gl;

            if (atlasTex_ != 0) { glDeleteTextures (1, &atlasTex_); atlasTex_ = 0; }
            if (vbo_ != 0)      { glDeleteBuffers (1, &vbo_);       vbo_ = 0; }
            if (ebo_ != 0)      { glDeleteBuffers (1, &ebo_);       ebo_ = 0; }
            if (vao_ != 0)      { glDeleteVertexArrays (1, &vao_);  vao_ = 0; }
            shader_.reset();

            vboCapacityBytes_ = 0;
            eboCapacityBytes_ = 0;
            uploadedAtlasGen_ = 0;
            initChecked_      = false;
            glReady_          = false;
        }

    private:
        bool ensureInitialised (juce::OpenGLContext& ctx)
        {
            using namespace juce::gl;

            if (initChecked_)
                return glReady_;
            initChecked_ = true;

            shader_ = std::make_unique<juce::OpenGLShaderProgram> (ctx);
            const bool linked =
                shader_->addVertexShader (juce::OpenGLHelpers::translateVertexShaderToV3 (GlToyShaderSource::vertexShader()))
                && shader_->addFragmentShader (juce::OpenGLHelpers::translateFragmentShaderToV3 (GlToyShaderSource::fragmentShader()))
                && shader_->link();

            if (! linked)
            {
                Y2K_LOG ("[GlToy] shader link FAILED: " + shader_->getLastError());
                Y2K_DIAGC ("GL", "GlToyRenderer shader link FAILED (falling back to CPU ToyOverlay)");
                shader_.reset();
                glReady_ = false;
                return false;
            }

            const GLuint prog = shader_->getProgramID();
            aPosLoc_            = glGetAttribLocation (prog, "aPos");
            aUvLoc_             = glGetAttribLocation (prog, "aUv");
            aColorLoc_          = glGetAttribLocation (prog, "aColor");
            aClipLoc_           = glGetAttribLocation (prog, "aClip");
            aModeLoc_           = glGetAttribLocation (prog, "aMode");
            uAtlasLoc_          = glGetUniformLocation (prog, "uAtlas");
            uEditorSizeLoc_     = glGetUniformLocation (prog, "uEditorSize");
            uExclusionCountLoc_ = glGetUniformLocation (prog, "uExclusionCount");
            uExclusionsLoc_     = glGetUniformLocation (prog, "uExclusions");

            if (aPosLoc_ < 0 || aUvLoc_ < 0 || aColorLoc_ < 0 || aClipLoc_ < 0 || aModeLoc_ < 0)
            {
                Y2K_LOG ("[GlToy] shader attrib lookup FAILED");
                shader_.reset();
                glReady_ = false;
                return false;
            }

            // Save bindings & pixel-unpack state around VAO/VBO/EBO/Texture creation
            GLint prevVao = 0, prevVbo = 0;
            GLint prevActiveTex = GL_TEXTURE0, prevTex = 0;
            GLint prevUnpackAlign = 4, prevUnpackRowLen = 0;
            glGetIntegerv (GL_VERTEX_ARRAY_BINDING, &prevVao);
            glGetIntegerv (GL_ARRAY_BUFFER_BINDING, &prevVbo);
            glGetIntegerv (GL_ACTIVE_TEXTURE, &prevActiveTex);
            glActiveTexture (GL_TEXTURE0);
            glGetIntegerv (GL_TEXTURE_BINDING_2D, &prevTex);
            glGetIntegerv (GL_UNPACK_ALIGNMENT, &prevUnpackAlign);
           #ifdef GL_UNPACK_ROW_LENGTH
            glGetIntegerv (GL_UNPACK_ROW_LENGTH, &prevUnpackRowLen);
           #endif

            glGenVertexArrays (1, &vao_);
            glGenBuffers (1, &vbo_);
            glGenBuffers (1, &ebo_);

            glBindVertexArray (vao_);
            glBindBuffer (GL_ARRAY_BUFFER, vbo_);
            glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, ebo_);

            constexpr GLsizei stride = (GLsizei) sizeof (GlToyVertex);
            glEnableVertexAttribArray ((GLuint) aPosLoc_);
            glVertexAttribPointer ((GLuint) aPosLoc_, 2, GL_FLOAT, GL_FALSE, stride,
                                   (const void*) offsetof (GlToyVertex, x));
            glEnableVertexAttribArray ((GLuint) aUvLoc_);
            glVertexAttribPointer ((GLuint) aUvLoc_, 2, GL_FLOAT, GL_FALSE, stride,
                                   (const void*) offsetof (GlToyVertex, u));
            glEnableVertexAttribArray ((GLuint) aColorLoc_);
            glVertexAttribPointer ((GLuint) aColorLoc_, 4, GL_FLOAT, GL_FALSE, stride,
                                   (const void*) offsetof (GlToyVertex, r));
            glEnableVertexAttribArray ((GLuint) aClipLoc_);
            glVertexAttribPointer ((GLuint) aClipLoc_, 4, GL_FLOAT, GL_FALSE, stride,
                                   (const void*) offsetof (GlToyVertex, cx0));
            glEnableVertexAttribArray ((GLuint) aModeLoc_);
            glVertexAttribPointer ((GLuint) aModeLoc_, 1, GL_FLOAT, GL_FALSE, stride,
                                   (const void*) offsetof (GlToyVertex, mode));

            glGenTextures (1, &atlasTex_);
            glBindTexture (GL_TEXTURE_2D, atlasTex_);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            // Allocate cleared transparent 1024x1024 RGBA8 backing texture once so 1px gutters are zeroed.
            std::vector<uint8_t> zeros ((size_t) EmojiAtlasLayout::kAtlasWidth
                                        * (size_t) EmojiAtlasLayout::kAtlasHeight * 4u, 0u);
            glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
           #ifdef GL_UNPACK_ROW_LENGTH
            glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
           #endif
            glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8,
                          EmojiAtlasLayout::kAtlasWidth, EmojiAtlasLayout::kAtlasHeight,
                          0, GL_RGBA, GL_UNSIGNED_BYTE, zeros.data());

            glPixelStorei (GL_UNPACK_ALIGNMENT, prevUnpackAlign);
           #ifdef GL_UNPACK_ROW_LENGTH
            glPixelStorei (GL_UNPACK_ROW_LENGTH, prevUnpackRowLen);
           #endif
            glBindVertexArray ((GLuint) prevVao);
            glBindBuffer (GL_ARRAY_BUFFER, (GLuint) prevVbo);
            glBindTexture (GL_TEXTURE_2D, (GLuint) prevTex);
            glActiveTexture ((GLenum) prevActiveTex);

            uploadedAtlasGen_ = 0;
            glReady_ = (vao_ != 0 && vbo_ != 0 && ebo_ != 0 && atlasTex_ != 0);
            Y2K_DIAGC ("GL", glReady_ ? "GlToyRenderer init OK (1024x1024 packed atlas)"
                                      : "GlToyRenderer init FAILED");
            return glReady_;
        }

        void syncAtlasTextures (EmojiAtlas& atlas)
        {
            using namespace juce::gl;

            const uint32_t curGen = atlas.glAtlasGeneration();
            if (uploadedAtlasGen_ == 0 || uploadedAtlasGen_ != curGen)
                atlas.requestFullGlAtlasReupload();

            pendingUploadsScratch_.clear();
            uint32_t drainedGen = curGen;
            atlas.drainGlAtlasUploads (pendingUploadsScratch_, drainedGen);
            uploadedAtlasGen_ = drainedGen;

            if (pendingUploadsScratch_.empty())
                return;

            glActiveTexture (GL_TEXTURE0);
            glBindTexture (GL_TEXTURE_2D, atlasTex_);
            glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
           #ifdef GL_UNPACK_ROW_LENGTH
            glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
           #endif

            for (const auto& up : pendingUploadsScratch_)
            {
                const auto slot = EmojiAtlasLayout::slotFor (up.emojiIndex, up.sizeIndex);
                if (! slot.valid || up.width != slot.w || up.height != slot.h)
                    continue;
                if (up.premulRgba.size() < (size_t) (up.width * up.height * 4))
                    continue;

                glTexSubImage2D (GL_TEXTURE_2D, 0, slot.x, slot.y, slot.w, slot.h,
                                 GL_RGBA, GL_UNSIGNED_BYTE, up.premulRgba.data());
            }
        }

        std::unique_ptr<juce::OpenGLShaderProgram> shader_;
        GLuint vao_      = 0;
        GLuint vbo_      = 0;
        GLuint ebo_      = 0;
        GLuint atlasTex_ = 0;

        GLint aPosLoc_            = -1;
        GLint aUvLoc_             = -1;
        GLint aColorLoc_          = -1;
        GLint aClipLoc_           = -1;
        GLint aModeLoc_           = -1;
        GLint uAtlasLoc_          = -1;
        GLint uEditorSizeLoc_     = -1;
        GLint uExclusionCountLoc_ = -1;
        GLint uExclusionsLoc_     = -1;

        GLsizeiptr vboCapacityBytes_ = 0;
        GLsizeiptr eboCapacityBytes_ = 0;
        uint32_t   uploadedAtlasGen_ = 0;
        bool       initChecked_      = false;
        bool       glReady_          = false;

        std::vector<GlToyVertex>              vertsScratch_;
        std::vector<uint16_t>                 indicesScratch_;
        std::vector<EmojiAtlas::GlAtlasUpload> pendingUploadsScratch_;
    };
}
