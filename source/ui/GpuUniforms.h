// =============================================================================
// GpuUniforms.h — CPU→GPU unified data block
//
// All audio-analysis results consumed by GPU shaders are packed into a single
// GpuDataBlock.  The Editor uploads this block to a GL_SHADER_STORAGE_BUFFER
// once per frame (only when tickCount has advanced).
//
// SSBO layout (GLSL side, std430):
//   layout(std430, binding = 0) readonly buffer GpuData {
//       Spectrum
//       float spectrum_mag[1024];     // offset    0, size 4096
//       float spectrum_data[160];      // offset 4096, size  640
//       float spectrum_mag_lo[4096];   // offset 4736, size 16384
//       // Oscilloscope
//       float osc_l[2048];             // offset 21120, size 8192
//       float osc_r[2048];             // offset 29312, size 8192
//       // Loudness
//       float lufs_m;                  // offset 37504
//       float lufs_s;                  // offset 37508
//       float lufs_i;                  // offset 37512
//       float rms_l;                   // offset 37516
//       float rms_r;                   // offset 37520
//       float true_peak_l;             // offset 37524
//       float true_peak_r;             // offset 37528
//       // Phase
//       float phase_correlation;       // offset 37532
//       float phase_width;             // offset 37536
//       float phase_balance;           // offset 37540
//       // Dynamics
//       float dyn_peak_l;              // offset 37544
//       float dyn_peak_r;              // offset 37548
//       float dyn_rms_l;               // offset 37552
//       float dyn_rms_r;               // offset 37556
//       float dyn_crest;               // offset 37560
//       float dyn_short_dr;            // offset 37564
//       float dyn_integrated_dr;       // offset 37568
//       // Metadata
//       uint  tick_count_lo;           // offset 37572
//       uint  tick_count_hi;           // offset 37576
//       uint  active_mask;             // offset 37580
//   };  // total 37584 bytes
//
// All fields are tightly packed; SSBO std430 has no padding for arrays of
// scalars.  The block is under 64 KB (UBO minimum guarantee) so it would also
// work as a UBO with std140 if we ever wanted to switch.
// =============================================================================
#pragma once

#include <cstdint>
#include <cstring>
#include <array>
#include "source/analysis/AnalyserHub.h"

// =============================================================================
// GpuDataBlock — CPU→GPU data, tightly packed (SSBO std430 compatible)
// =============================================================================
struct GpuDataBlock {
  // ---- Spectrum (moduleTypeId = 3, 17) ----
  float spectrum_mag[AnalyserHub::spectrumMagSize];       // 1024
  float spectrum_data[AnalyserHub::spectrumBins];         //  160
  float spectrum_mag_lo[AnalyserHub::spectrumMagSizeLo];  // 4096

  // ---- Oscilloscope (moduleTypeId = 2, 8, 14) ----
  float osc_l[AnalyserHub::oscilloscopeBufferSize];       // 2048
  float osc_r[AnalyserHub::oscilloscopeBufferSize];       // 2048

  // ---- Loudness (moduleTypeId = 4, 7, 8, 15) ----
  float lufs_m       = -144.0f;
  float lufs_s       = -144.0f;
  float lufs_i       = -144.0f;
  float rms_l        = -144.0f;
  float rms_r        = -144.0f;
  float true_peak_l  = -144.0f;
  float true_peak_r  = -144.0f;

  // ---- Phase (moduleTypeId = 9, 10) ----
  float phase_correlation = 0.0f;
  float phase_width       = 0.0f;
  float phase_balance     = 0.0f;

  // ---- Dynamics (moduleTypeId = 6, 11, 12, 13) ----
  float dyn_peak_l        = -144.0f;
  float dyn_peak_r        = -144.0f;
  float dyn_rms_l         = -144.0f;
  float dyn_rms_r         = -144.0f;
  float dyn_crest         = 0.0f;
  float dyn_short_dr      = 0.0f;
  float dyn_integrated_dr = 0.0f;

  // ---- Metadata ----
  uint32_t tick_count_lo = 0;
  uint32_t tick_count_hi = 0;
  uint32_t active_mask   = 0;

  // ---- Populate from FrameSnapshot ----
  void FromSnapshot(const AnalyserHub::FrameSnapshot& snapshot) {
    tick_count_lo = static_cast<uint32_t>(snapshot.tickCount & 0xFFFFFFFFu);
    tick_count_hi = static_cast<uint32_t>(snapshot.tickCount >> 32);
    active_mask   = snapshot.activeMask;

    // Spectrum
    for (std::size_t i = 0; i < AnalyserHub::spectrumMagSize; ++i)
      spectrum_mag[i] = snapshot.spectrumMag[i];
    for (std::size_t i = 0; i < AnalyserHub::spectrumBins; ++i)
      spectrum_data[i] = snapshot.spectrumData[i];
    for (std::size_t i = 0; i < AnalyserHub::spectrumMagSizeLo; ++i)
      spectrum_mag_lo[i] = snapshot.spectrumMagLo[i];

    // Oscilloscope
    for (std::size_t i = 0; i < AnalyserHub::oscilloscopeBufferSize; ++i) {
      osc_l[i] = snapshot.oscL[i];
      osc_r[i] = snapshot.oscR[i];
    }

    // Loudness
    lufs_m      = snapshot.loudness.lufsM;
    lufs_s      = snapshot.loudness.lufsS;
    lufs_i      = snapshot.loudness.lufsI;
    rms_l       = snapshot.loudness.rmsL;
    rms_r       = snapshot.loudness.rmsR;
    true_peak_l = snapshot.loudness.truePeakL;
    true_peak_r = snapshot.loudness.truePeakR;

    // Phase
    phase_correlation = snapshot.phase.correlation;
    phase_width       = snapshot.phase.width;
    phase_balance     = snapshot.phase.balance;

    // Dynamics
    dyn_peak_l        = snapshot.dynamics.peakL;
    dyn_peak_r        = snapshot.dynamics.peakR;
    dyn_rms_l         = snapshot.dynamics.rmsL;
    dyn_rms_r         = snapshot.dynamics.rmsR;
    dyn_crest         = snapshot.dynamics.crest;
    dyn_short_dr      = snapshot.dynamics.shortDR;
    dyn_integrated_dr = snapshot.dynamics.integratedDR;
  }

  // Whether the current block is different from another (by tickCount).
  bool IsNewerThan(const GpuDataBlock& other) const {
    return tick_count_lo != other.tick_count_lo
        || tick_count_hi != other.tick_count_hi;
  }
};

// Compile-time size sanity check.
// gpu_total_bytes must match the GLSL SSBO total (37584 bytes for v1.0).
static constexpr std::size_t kGpuDataBlockBytes = sizeof(GpuDataBlock);
