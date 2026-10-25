/*
 * CPU frame-time statistics for the stereo presentation paths.
 *
 * The nvutils profiler sections time the Vulkan render graph, but they cannot
 * see the presentation chain that runs outside the render loop: the
 * Vulkan -> OpenGL external-memory handoff, the CPU readback fallback, and
 * the Vulkan side-by-side blit.  This module measures the wall-clock CPU cost
 * of those paths per frame, including the blocking SwapBuffers call, and
 * reports mean/percentile statistics plus an optional per-frame CSV trace.
 *
 * Single-threaded: every entry point is called from the main application
 * loop, so no synchronization is required.
 */

#pragma once

#include <cstdint>

namespace curved_stereo_gs {

class StereoFrameStats
{
public:
  // Which stereo presentation path produced the frame.
  enum class Path : uint32_t
  {
    kGpuInterop       = 0,  // Vulkan external-memory images + binary semaphores -> WGL quad-buffer blit
    kCpuReadback      = 1,  // Vulkan readback (queue sync + memcpy) -> GL upload -> quad-buffer draw
    kVulkanSideBySide = 2,  // Multiview layers blitted into one ordinary swapchain image
    kPathCount        = 3
  };

  static const char* pathName(Path path);

  // Start/stop one frame sample on `path`.  endFrame() commits presentMs and
  // the SwapBuffers sub-metric (see setSwapBuffersMs) into the accumulator.
  static void beginFrame(Path path, uint64_t frameNumber);
  static void endFrame(Path path);

  // Called by the presenter right after SwapBuffers() returns, while a frame
  // started with beginFrame() is still open.  Stored as a sub-metric of the
  // current sample; 0 when the caller does not report it.
  static void setSwapBuffersMs(double milliseconds);

  // Log a running summary every `interval` frames across all paths
  // (0 disables periodic reporting; the final report is separate).
  static void setReportInterval(uint32_t interval);

  // Open `filePath` for incremental per-frame CSV output
  // (frame, path, presentMs, swapBuffersMs).  Returns false when the file
  // cannot be opened.
  static bool openCsv(const char* filePath);
  static void closeCsv();

  // Print per-path count / mean / p50 / p95 / p99 / max (ms) through LOGI.
  static void logSummary(const char* context);

private:
  struct Impl;
  static Impl& impl();
};

}  // namespace curved_stereo_gs
