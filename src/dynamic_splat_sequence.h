/*
 * Dynamic splat keyframe sequence: .dgs asset loader + VRAM keyframe pool +
 * playback clock for dynamic (time-varying) Gaussian splat content.
 *
 * Design contract (docs/dynamic-4dgs.md):
 *  - This module owns asset parsing, the VRAM keyframe pool and the playback
 *    clock. It does NOT own any graphics pipeline: the renderer's `animate`
 *    compute pass reads the keyframe pair exposed here and writes the live
 *    position buffer consumed by sorting/rasterization.
 *  - Only positions are dynamic in v1 (SH/scale/rotation stay canonical and
 *    are loaded through the existing static path).
 *  - update() is a pure state step except at VRAM window boundaries, where a
 *    synchronous block copy fills the slide (v2: async prefetch).
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace curved_stereo_gs {

struct DynamicSplatSequenceInfo
{
  uint32_t frameCount{0};
  uint32_t splatCount{0};
  float    fps{30.0f};
  bool     fp16{true};
  size_t   vramBytes{0};
};

enum class DynamicLoopMode : uint32_t
{
  Clamp    = 0,  // hold first/last frame outside the sequence span
  Loop     = 1,  // wrap around
  PingPong = 2,  // reverse at the ends
};

class DynamicSplatSequence
{
public:
  // `commandPool` must support the transfer queue family of `queue`.
  bool open(VkPhysicalDevice physicalDevice, VkDevice device, VkQueue queue, VkCommandPool commandPool,
            const std::filesystem::path& dgsFile, size_t maxVramBytes);
  void close();
  bool isOpen() const { return m_device != VK_NULL_HANDLE && m_info.frameCount > 0; }

  const DynamicSplatSequenceInfo& info() const { return m_info; }

  // Advance the playback clock. `animationTimeSeconds` is absolute (scrubber-
  // friendly): the caller owns the clock, the module maps time -> keyframes.
  // Pure state step except at VRAM window boundaries (synchronous slide).
  bool update(double animationTimeSeconds);

  void setLoopMode(DynamicLoopMode mode) { m_loopMode = mode; }

  // Inputs for the `animate` compute pass: bind the two keyframe position
  // buffers (offsets into the pool) and push `alpha()`.
  VkBuffer keyframeBuffer(uint32_t slot) const;  // slot 0 -> frame k, slot 1 -> frame k+1
  VkDeviceSize keyframeBufferOffset(uint32_t slot) const;
  uint32_t keyframeIndex(uint32_t slot) const;   // source frame index in the .dgs
  float    alpha() const { return m_alpha; }

  // One contiguous device buffer backs the whole VRAM window; slots are
  // regions within it (one allocation, stable buffer handle).
  VkBuffer poolBuffer() const { return m_pool.buffer; }

private:
  struct Pool
  {
    VkBuffer       buffer{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    VkDeviceSize   bytes{0};
    uint32_t       windowFrames{0};
    uint32_t       firstFrame{0};  // logical source-frame index at the current window start
  };

  size_t frameBytes() const;
  bool   allocPool(uint32_t windowFrames);
  void   destroyPool();
  bool   loadFrameBlock(uint32_t frameIndex, uint32_t windowSlot);
  bool   slideWindowTo(uint32_t firstFrame);
  bool   isFrameResident(uint32_t logicalFrame) const;
  void   submitCopies();

  DynamicSplatSequenceInfo m_info{};
  DynamicLoopMode          m_loopMode{DynamicLoopMode::Loop};

  VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};
  VkDevice         m_device{VK_NULL_HANDLE};
  VkQueue          m_queue{VK_NULL_HANDLE};
  VkCommandPool    m_commandPool{VK_NULL_HANDLE};

  std::ifstream          m_file;
  std::vector<uint64_t>  m_frameOffsets;  // per-frame byte offsets from the file start
  size_t                 m_frameBytes{0};

  Pool                  m_pool{};
  std::vector<uint32_t> m_residentFrames;  // source frame stored in each pool slot
  VkBuffer              m_staging{VK_NULL_HANDLE};
  VkDeviceMemory        m_stagingMemory{VK_NULL_HANDLE};
  void*                 m_stagingMapped{nullptr};

  // playback state (valid after update())
  uint32_t m_keyframe[2]{0, 0};
  uint32_t m_keyframePoolSlot[2]{0, 0};
  float    m_alpha{0.0f};
};

}  // namespace curved_stereo_gs
