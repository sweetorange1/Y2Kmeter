// =============================================================================
// GpuCompositor.cpp — Editor-side GPU compositing dispatcher implementation
// =============================================================================

#include "source/ui/GpuCompositor.h"

// =============================================================================
// Lifecycle
// =============================================================================

GpuCompositor::~GpuCompositor() {
  // SSBO should already be cleaned up via GpuShutdown. This is a safety net
  // in case the caller forgot; note that we cannot safely call GL functions
  // without a current context.
  ssbo_handle_ = 0;
  ssbo_initialized_ = false;
}

void GpuCompositor::GpuInit() {
  jassert(!ssbo_initialized_);

  // Create the shared SSBO (binding point 0).
  juce::gl::glGenBuffers(1, &ssbo_handle_);
  juce::gl::glBindBuffer(juce::gl::GL_SHADER_STORAGE_BUFFER, ssbo_handle_);
  juce::gl::glBufferData(juce::gl::GL_SHADER_STORAGE_BUFFER,
                          static_cast<GLsizeiptr>(kGpuDataBlockBytes),
                          nullptr, juce::gl::GL_DYNAMIC_DRAW);
  juce::gl::glBindBufferBase(juce::gl::GL_SHADER_STORAGE_BUFFER, 0,
                              ssbo_handle_);
  juce::gl::glBindBuffer(juce::gl::GL_SHADER_STORAGE_BUFFER, 0);
  ssbo_initialized_ = true;

  // Init all registered modules.
  for (auto* m : modules_) {
    jassert(m != nullptr);
    m->GpuInit();
  }
}

void GpuCompositor::GpuShutdown() {
  for (auto* m : modules_) {
    jassert(m != nullptr);
    m->GpuShutdown();
  }

  if (ssbo_handle_ != 0) {
    juce::gl::glDeleteBuffers(1, &ssbo_handle_);
    ssbo_handle_ = 0;
  }
  ssbo_initialized_ = false;
}

// =============================================================================
// Per-frame
// =============================================================================

void GpuCompositor::UploadIfDirty(
    const AnalyserHub::FrameSnapshot& snapshot) {
  if (!ssbo_initialized_) return;

  // Build a fresh block from the snapshot.
  GpuDataBlock fresh;
  fresh.FromSnapshot(snapshot);

  // Early out if tick hasn't advanced.
  {
    juce::ScopedLock lock(block_mutex_);
    if (!fresh.IsNewerThan(uploaded_block_)) return;
  }

  // Upload to SSBO with buffer invalidation (orphaning) to avoid GPU stalls.
  juce::gl::glBindBuffer(juce::gl::GL_SHADER_STORAGE_BUFFER, ssbo_handle_);
  void* mapped = juce::gl::glMapBufferRange(
      juce::gl::GL_SHADER_STORAGE_BUFFER, 0,
      static_cast<GLsizeiptr>(kGpuDataBlockBytes),
      juce::gl::GL_MAP_WRITE_BIT | juce::gl::GL_MAP_INVALIDATE_BUFFER_BIT);
  if (mapped != nullptr) {
    std::memcpy(mapped, &fresh, kGpuDataBlockBytes);
    juce::gl::glUnmapBuffer(juce::gl::GL_SHADER_STORAGE_BUFFER);
  }
  juce::gl::glBindBuffer(juce::gl::GL_SHADER_STORAGE_BUFFER, 0);

  // Cache for dirty detection next frame.
  {
    juce::ScopedLock lock(block_mutex_);
    uploaded_block_ = fresh;
  }
  data_dirty_ = true;
}

void GpuCompositor::GpuRender() {
  if (!ssbo_initialized_ || modules_.empty()) return;

  // Re-bind SSBO to binding 0 (in case some module changed it).
  juce::gl::glBindBufferBase(juce::gl::GL_SHADER_STORAGE_BUFFER, 0,
                              ssbo_handle_);

  // Composite FBO is already bound by the Editor at this point.

  for (auto* m : modules_) {
    if (!m->GpuIsVisible()) continue;

    auto bounds = m->GpuScreenBounds();
    if (bounds.getWidth() <= 0 || bounds.getHeight() <= 0) continue;

    juce::gl::glViewport(bounds.getX(), bounds.getY(),
                          bounds.getWidth(), bounds.getHeight());
    juce::gl::glScissor(bounds.getX(), bounds.getY(),
                         bounds.getWidth(), bounds.getHeight());
    juce::gl::glEnable(juce::gl::GL_SCISSOR_TEST);
    m->GpuRender();
    juce::gl::glDisable(juce::gl::GL_SCISSOR_TEST);
  }
}

// =============================================================================
// Module registry
// =============================================================================

void GpuCompositor::RegisterModule(GpuRenderer* module) {
  jassert(module != nullptr);
  modules_.push_back(module);
}

void GpuCompositor::UnregisterModule(GpuRenderer* module) {
  modules_.erase(
      std::remove(modules_.begin(), modules_.end(), module),
      modules_.end());
}
