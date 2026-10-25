/*
 * Dynamic splat keyframe sequence: implementation.
 * See dynamic_splat_sequence.h for the module contract and
 * docs/dynamic-4dgs.md for the .dgs format spec and design rationale.
 *
 * Vulkan note: the keyframe pool and staging buffer are allocated with plain
 * vkCreateBuffer/vkAllocateMemory on purpose (no framework dependency in the
 * data layer). The integration pass may switch to the project allocator if
 * preferred; memory requirements are trivial (DEVICE_LOCAL pool + one mapped
 * staging block).
 */

#include "dynamic_splat_sequence.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include <volk.h>
#include <vulkan/vulkan.h>

#include <nvutils/logger.hpp>

namespace curved_stereo_gs {
namespace {

constexpr char     kMagic[4]  = {'D', 'G', 'S', '1'};
constexpr uint16_t kVersion   = 1;
constexpr size_t   kHeaderSize = 64;

struct DgsHeader
{
  char     magic[4];
  uint16_t version;
  uint16_t flags;
  uint32_t frameCount;
  uint32_t splatCount;
  float    fps;
  uint32_t reserved[11];  // header is a fixed 64 bytes
};
static_assert(sizeof(DgsHeader) == 64, "unexpected padding in .dgs header");

uint32_t findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeBits, VkMemoryPropertyFlags properties)
{
  VkPhysicalDeviceMemoryProperties memProperties{};
  vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
  for(uint32_t i = 0; i < memProperties.memoryTypeCount; ++i)
  {
    if((typeBits & (1U << i)) != 0
       && (memProperties.memoryTypes[i].propertyFlags & properties) == properties)
      return i;
  }
  return UINT32_MAX;
}

bool allocMemory(VkPhysicalDevice physicalDevice, VkDevice device, VkBuffer buffer,
                 VkMemoryPropertyFlags properties, VkDeviceMemory* outMemory)
{
  VkMemoryRequirements req{};
  vkGetBufferMemoryRequirements(device, buffer, &req);
  const uint32_t type = findMemoryType(physicalDevice, req.memoryTypeBits, properties);
  if(type == UINT32_MAX)
    return false;
  VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocInfo.allocationSize  = req.size;
  allocInfo.memoryTypeIndex = type;
  return vkAllocateMemory(device, &allocInfo, nullptr, outMemory) == VK_SUCCESS;
}

double wrapTime(double t, double spanSeconds, DynamicLoopMode mode)
{
  if(spanSeconds <= 0.0)
    return 0.0;
  switch(mode)
  {
    case DynamicLoopMode::Clamp: return t < 0.0 ? 0.0 : (t > spanSeconds ? spanSeconds : t);
    case DynamicLoopMode::PingPong: {
      const double period = spanSeconds * 2.0;
      double       local  = t - std::floor(t / period) * period;  // in [0, period)
      return local <= spanSeconds ? local : period - local;
    }
    case DynamicLoopMode::Loop:
    default: {
      double local = t - std::floor(t / spanSeconds) * spanSeconds;  // in [0, span)
      return local >= spanSeconds ? 0.0 : local;                     // guard fp edge
    }
  }
}

}  // namespace

size_t DynamicSplatSequence::frameBytes() const
{
  return size_t(m_info.splatCount) * 3U * (m_info.fp16 ? 2U : 4U);
}

bool DynamicSplatSequence::open(VkPhysicalDevice physicalDevice, VkDevice device, VkQueue queue,
                                VkCommandPool commandPool, const std::filesystem::path& dgsFile,
                                size_t maxVramBytes)
{
  close();

  m_physicalDevice = physicalDevice;
  m_device         = device;
  m_queue          = queue;
  m_commandPool    = commandPool;

  m_file.open(dgsFile, std::ios::binary);
  if(!m_file)
    return false;

  DgsHeader header{};
  m_file.read(reinterpret_cast<char*>(&header), sizeof(header));
  if(!m_file || std::memcmp(header.magic, kMagic, 4) != 0 || header.version != kVersion
     || header.frameCount == 0 || header.splatCount == 0 || header.fps <= 0.0f)
  {
    m_file.close();
    return false;
  }

  m_info.frameCount = header.frameCount;
  m_info.splatCount = header.splatCount;
  m_info.fps        = header.fps;
  m_info.fp16       = (header.flags & 1U) != 0;

  if(m_info.fp16)
  {
    LOGE("fp16 .dgs sequences are not supported by this build; re-export with --fp32 positions\n");
    m_file.close();
    return false;
  }

  m_frameOffsets.resize(m_info.frameCount);
  m_file.read(reinterpret_cast<char*>(m_frameOffsets.data()),
              std::streamsize(m_frameOffsets.size() * sizeof(uint64_t)));
  if(!m_file)
  {
    close();
    return false;
  }

  m_frameBytes = frameBytes();

  // A storage-buffer descriptor cannot expose bytes beyond maxStorageBufferRange.
  // Also retain two frames for interpolation whenever the sequence has motion.
  VkPhysicalDeviceProperties deviceProperties{};
  vkGetPhysicalDeviceProperties(m_physicalDevice, &deviceProperties);
  maxVramBytes = std::min(maxVramBytes, size_t(deviceProperties.limits.maxStorageBufferRange));
  const uint32_t minWindowFrames = m_info.frameCount > 1 ? 2U : 1U;
  const size_t   budgetFrames    = maxVramBytes / (m_frameBytes > 0 ? m_frameBytes : 1);
  if(budgetFrames < minWindowFrames)
  {
    LOGE(".dgs frame pair exceeds the device storage-buffer range/budget (%zu bytes per frame, %zu bytes available)\n",
         m_frameBytes, maxVramBytes);
    close();
    return false;
  }
  const uint32_t windowFrames = uint32_t(std::min<size_t>(m_info.frameCount,
                                                          std::max<size_t>(budgetFrames, minWindowFrames)));

  if(!allocPool(windowFrames))
  {
    close();
    return false;
  }
  m_info.vramBytes = m_pool.bytes;

  // Load the initial window [0, windowFrames).
  m_residentFrames.assign(windowFrames, UINT32_MAX);
  for(uint32_t i = 0; i < windowFrames; ++i)
  {
    const uint32_t slot = i % windowFrames;
    if(!loadFrameBlock(i, slot))
    {
      close();
      return false;
    }
    m_residentFrames[slot] = i;
  }
  m_pool.firstFrame = 0;

  update(0.0);
  return true;
}

bool DynamicSplatSequence::allocPool(uint32_t windowFrames)
{
  const VkDeviceSize poolBytes = VkDeviceSize(windowFrames) * VkDeviceSize(m_frameBytes);
  VkBufferCreateInfo bufInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bufInfo.size  = poolBytes;
  bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  if(vkCreateBuffer(m_device, &bufInfo, nullptr, &m_pool.buffer) != VK_SUCCESS)
    return false;

  if(!allocMemory(m_physicalDevice, m_device, m_pool.buffer, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                  &m_pool.memory))
  {
    vkDestroyBuffer(m_device, m_pool.buffer, nullptr);
    m_pool.buffer = VK_NULL_HANDLE;
    return false;
  }
  if(vkBindBufferMemory(m_device, m_pool.buffer, m_pool.memory, 0) != VK_SUCCESS)
  {
    vkDestroyBuffer(m_device, m_pool.buffer, nullptr);
    vkFreeMemory(m_device, m_pool.memory, nullptr);
    m_pool.buffer = VK_NULL_HANDLE;
    m_pool.memory = VK_NULL_HANDLE;
    return false;
  }
  m_pool.bytes        = poolBytes;
  m_pool.windowFrames = windowFrames;
  m_pool.firstFrame   = 0;

  // One mapped staging block of a single frame is all v1 needs (sync slide).
  VkBufferCreateInfo stagingInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  stagingInfo.size  = m_frameBytes;
  stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  if(vkCreateBuffer(m_device, &stagingInfo, nullptr, &m_staging) != VK_SUCCESS)
    return false;
  if(!allocMemory(m_physicalDevice, m_device, m_staging,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  &m_stagingMemory))
    return false;
  if(vkBindBufferMemory(m_device, m_staging, m_stagingMemory, 0) != VK_SUCCESS)
    return false;
  return vkMapMemory(m_device, m_stagingMemory, 0, m_frameBytes, 0, &m_stagingMapped) == VK_SUCCESS;
}

bool DynamicSplatSequence::loadFrameBlock(uint32_t frameIndex, uint32_t windowSlot)
{
  if(frameIndex >= m_info.frameCount || windowSlot >= m_pool.windowFrames)
    return false;
  m_file.clear();
  m_file.seekg(std::streamoff(m_frameOffsets[frameIndex]), std::ios::beg);
  m_file.read(reinterpret_cast<char*>(m_stagingMapped), std::streamsize(m_frameBytes));
  if(!m_file)
    return false;

  VkCommandBufferAllocateInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cmdInfo.commandPool        = m_commandPool;
  cmdInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cmdInfo.commandBufferCount = 1;
  VkCommandBuffer cmd{VK_NULL_HANDLE};
  if(vkAllocateCommandBuffers(m_device, &cmdInfo, &cmd) != VK_SUCCESS)
    return false;

  VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &beginInfo);

  VkBufferCopy copy{};
  copy.srcOffset = 0;
  copy.dstOffset = VkDeviceSize(windowSlot) * VkDeviceSize(m_frameBytes);
  copy.size      = m_frameBytes;
  vkCmdCopyBuffer(cmd, m_staging, m_pool.buffer, 1, &copy);
  vkEndCommandBuffer(cmd);

  VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers    = &cmd;
  vkQueueSubmit(m_queue, 1, &submitInfo, VK_NULL_HANDLE);
  vkQueueWaitIdle(m_queue);  // v1: synchronous slide; v2 moves this to a prefetch thread
  vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
  return true;
}

bool DynamicSplatSequence::isFrameResident(uint32_t logicalFrame) const
{
  if(m_pool.windowFrames == 0 || m_residentFrames.empty())
    return false;
  const uint32_t slot          = logicalFrame % m_pool.windowFrames;
  const uint32_t sourceFrame   = logicalFrame % m_info.frameCount;
  return slot < m_residentFrames.size() && m_residentFrames[slot] == sourceFrame;
}

bool DynamicSplatSequence::slideWindowTo(uint32_t firstFrame)
{
  firstFrame = firstFrame % m_info.frameCount;
  // Keep frames in ring slots keyed by frameIndex % windowFrames. This avoids
  // moving large GPU blocks when the logical window advances by one frame.
  const uint32_t count    = m_pool.windowFrames;
  for(uint32_t i = 0; i < count; ++i)
  {
    const uint32_t logicalFrame = firstFrame + i;
    const uint32_t srcFrame     = logicalFrame % m_info.frameCount;
    const uint32_t slot         = logicalFrame % count;
    if(m_residentFrames[slot] != srcFrame)
    {
      if(!loadFrameBlock(srcFrame, slot))
        return false;
      m_residentFrames[slot] = srcFrame;
    }
  }
  m_pool.firstFrame = firstFrame;
  return true;
}

bool DynamicSplatSequence::update(double animationTimeSeconds)
{
  if(!isOpen())
    return false;
  // Looping sequences have a wrap interval from the last frame to frame 0.
  // Clamp and ping-pong sequences end on the actual last frame instead.
  const double spanFrames  = m_loopMode == DynamicLoopMode::Loop ? double(m_info.frameCount)
                                                                  : double(m_info.frameCount - 1U);
  const double spanSeconds = spanFrames / double(m_info.fps);
  const double t           = wrapTime(animationTimeSeconds, spanSeconds, m_loopMode);
  const double cf          = t * double(m_info.fps);  // continuous frame index

  uint32_t k  = uint32_t(std::floor(cf));
  float    a  = float(cf - std::floor(cf));
  k           = k % m_info.frameCount;
  uint32_t k1 = (k + 1U) % m_info.frameCount;
  if(m_loopMode != DynamicLoopMode::Loop && cf >= double(m_info.frameCount - 1U))
  {
    k  = m_info.frameCount - 1;
    k1 = k;
    a  = 0.0f;
  }
  // Adjacent identical keyframes (clamped end): avoid a meaningless blend.
  if(k1 == k)
    a = 0.0f;

  // VRAM window: make sure both keyframes are resident.
  const uint32_t logicalK1 = (k1 == k) ? k : (k + 1U);
  if(!isFrameResident(k) || !isFrameResident(logicalK1))
  {
    if(!slideWindowTo(k))
      return false;
  }
  if(!isFrameResident(k) || !isFrameResident(logicalK1))
    return false;

  m_keyframe[0] = k;
  m_keyframe[1] = k1;
  m_keyframePoolSlot[0] = k % m_pool.windowFrames;
  m_keyframePoolSlot[1] = logicalK1 % m_pool.windowFrames;
  m_alpha       = a;
  return true;
}

VkBuffer DynamicSplatSequence::keyframeBuffer(uint32_t slot) const
{
  return m_pool.buffer;
}

VkDeviceSize DynamicSplatSequence::keyframeBufferOffset(uint32_t slot) const
{
  const uint32_t poolSlot = m_keyframePoolSlot[slot < 1 ? 0 : 1];
  return VkDeviceSize(poolSlot) * VkDeviceSize(m_frameBytes);
}

uint32_t DynamicSplatSequence::keyframeIndex(uint32_t slot) const
{
  return m_keyframe[slot < 1 ? 0 : 1];
}

void DynamicSplatSequence::close()
{
  if(m_device == VK_NULL_HANDLE)
  {
    m_file.close();
    return;
  }
  if(m_stagingMapped != nullptr)
    vkUnmapMemory(m_device, m_stagingMemory);
  if(m_staging != VK_NULL_HANDLE)
    vkDestroyBuffer(m_device, m_staging, nullptr);
  if(m_stagingMemory != VK_NULL_HANDLE)
    vkFreeMemory(m_device, m_stagingMemory, nullptr);
  if(m_pool.buffer != VK_NULL_HANDLE)
    vkDestroyBuffer(m_device, m_pool.buffer, nullptr);
  if(m_pool.memory != VK_NULL_HANDLE)
    vkFreeMemory(m_device, m_pool.memory, nullptr);
  m_staging       = VK_NULL_HANDLE;
  m_stagingMemory = VK_NULL_HANDLE;
  m_pool          = {};
  m_residentFrames.clear();
  m_stagingMapped = nullptr;
  m_file.close();
  m_device = VK_NULL_HANDLE;
}

}  // namespace curved_stereo_gs
