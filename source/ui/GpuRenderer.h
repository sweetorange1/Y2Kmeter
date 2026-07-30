// =============================================================================
// GpuRenderer.h — GPU 渲染器接口
//
// Y2Kmeter GPU rendering architecture.
// All visual modules implement this interface to participate in the
// Editor-level OpenGL compositing pipeline (§2 of GPU_ARCHITECTURE_DESIGN.md).
//
// Lifecycle (all methods called on the Editor's GL thread or UI thread):
//   GpuInit()       — once, after Editor OpenGL context is created
//   GpuResize()     — whenever window / module bounds change
//   GpuUpdateUniforms() — per frame, UI thread, before GpuRender
//   GpuRender()     — per frame, inside Editor::renderOpenGL()
//   GpuShutdown()   — once, before Editor OpenGL context is destroyed
// =============================================================================
#pragma once

#include <JuceHeader.h>

class AnalyserHub;

// =============================================================================
// GpuRenderer — interface implemented by every visual module
// =============================================================================
class GpuRenderer {
public:
  virtual ~GpuRenderer() = default;

  // ---------------------------------------------------------------------------
  // Lifecycle — called on Editor's GL thread (context is current)
  // ---------------------------------------------------------------------------

  // Create all GL resources (shaders, VBOs, FBOs, textures, UBO bindings).
  // Called once when the Editor OpenGL context is first created.
  virtual void GpuInit() = 0;

  // Destroy all GL resources created in GpuInit.
  // Called once when the Editor OpenGL context is about to be destroyed.
  virtual void GpuShutdown() = 0;

  // Rebuild resolution-dependent resources (FBOs, textures).
  // Called when the module's pixel bounds change.
  virtual void GpuResize(int pixel_width, int pixel_height) = 0;

  // ---------------------------------------------------------------------------
  // Per-frame — GpuUpdateUniforms on UI thread, GpuRender on GL thread
  // ---------------------------------------------------------------------------

  // Consume the latest FrameSnapshot and upload audio data to GPU buffers.
  // Called from the UI thread (FrameListener::onFrame), only when dirty.
  // Must be fast: ~10-50 μs (UBO map, memcpy, unmap).
  virtual void GpuUpdateUniforms(
      const AnalyserHub::FrameSnapshot& snapshot) = 0;

  // Execute pure-GPU rendering for this module.
  // Called inside Editor::renderOpenGL(). At entry:
  //   - GL viewport is already set to GpuScreenBounds()
  //   - GL scissor test is already enabled for the same rectangle
  //   - Composite FBO is bound as GL_DRAW_FRAMEBUFFER
  // Module must NOT change the framebuffer binding.
  virtual void GpuRender() = 0;

  // ---------------------------------------------------------------------------
  // Queries
  // ---------------------------------------------------------------------------

  // Whether this module should be rendered this frame.
  virtual bool GpuIsVisible() const = 0;

  // The module's pixel rectangle in the Editor window coordinate system.
  // Used by the compositor for glViewport + glScissor.
  virtual juce::Rectangle<int> GpuScreenBounds() const = 0;
};
