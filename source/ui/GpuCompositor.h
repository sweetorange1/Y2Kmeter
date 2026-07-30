// =============================================================================
// GpuCompositor.h — Editor-side GPU compositing dispatcher
//
// Owns the GL SSBO for GpuDataBlock, manages a list of GpuRenderer modules,
// and drives the per-frame upload + render loop from Editor::renderOpenGL().
//
// Intended usage (in PluginEditor):
//
//   class Y2KmeterAudioProcessorEditor : public juce::AudioProcessorEditor,
//                                         public juce::OpenGLRenderer {
//     GpuCompositor compositor_;
//     ...
//     // OpenGLRenderer callbacks delegate to compositor:
//     void newOpenGLContextCreated() override { compositor_.GpuInit(); }
//     void renderOpenGL()            override { compositor_.GpuRender(); }
//     void openGLContextClosing()    override { compositor_.GpuShutdown(); }
//
//     // FrameListener callback:
//     void onFrame(const FrameSnapshot& snap) {
//       compositor_.UploadIfDirty(snap);
//       triggerRepaint();
//     }
//   };
//
// Design notes:
//   - Uses GL_SHADER_STORAGE_BUFFER at binding point 0 for GpuDataBlock.
//   - Dirty detection via tickCount comparison; upload only when data changes.
//   - Thread safety: GpuInit/GpuShutdown/GpuRender on GL thread;
//     UploadIfDirty on UI thread (GL buffer mapping is thread-safe).
// =============================================================================
#pragma once

#include <JuceHeader.h>
#include <memory>
#include <vector>
#include "source/ui/GpuRenderer.h"
#include "source/ui/GpuUniforms.h"

// =============================================================================
// GpuCompositor
// =============================================================================
class GpuCompositor {
public:
  GpuCompositor() = default;
  ~GpuCompositor();

  // Disallow copy/move.
  GpuCompositor(const GpuCompositor&) = delete;
  GpuCompositor& operator=(const GpuCompositor&) = delete;

  // ---------------------------------------------------------------------------
  // Lifecycle (call on GL thread)
  // ---------------------------------------------------------------------------

  // Create SSBO and init all registered modules' GPU resources.
  void GpuInit();

  // Release SSBO and shutdown all registered modules' GPU resources.
  void GpuShutdown();

  // ---------------------------------------------------------------------------
  // Per-frame
  // ---------------------------------------------------------------------------

  // Upload GpuDataBlock to SSBO if tickCount has changed since last upload.
  // Called from UI thread (FrameListener::onFrame).
  void UploadIfDirty(const AnalyserHub::FrameSnapshot& snapshot);

  // Render all visible modules in Z-order. Called from GL thread.
  // Editor must have bound its composite FBO and set up the GL state.
  void GpuRender();

  // ---------------------------------------------------------------------------
  // Module registry
  // ---------------------------------------------------------------------------

  void RegisterModule(GpuRenderer* module);
  void UnregisterModule(GpuRenderer* module);

  // Check whether GpuInit has been called and SSBO is valid.
  bool IsInitialized() const { return ssbo_initialized_; }

private:
  std::vector<GpuRenderer*> modules_;
  GpuDataBlock              current_block_{};
  GpuDataBlock              uploaded_block_{};
  GLuint                    ssbo_handle_ = 0;
  bool                      ssbo_initialized_ = false;
  bool                      data_dirty_ = false;
  juce::CriticalSection     block_mutex_;
};
