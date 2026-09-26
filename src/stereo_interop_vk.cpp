#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <volk.h>

#include "stereo_interop_vk.h"

#include <algorithm>
#include <array>

#include <nvapp/application.hpp>
#include <nvutils/logger.hpp>
#include <vulkan/vk_enum_string_helper.h>

namespace curved_stereo_gs {

StereoInteropVk::~StereoInteropVk()
{
  deinit();
}

bool StereoInteropVk::init(VkPhysicalDevice physicalDevice, VkDevice device, uint32_t queueFamilyIndex)
{
  deinit();

  m_physicalDevice   = physicalDevice;
  m_device           = device;
  m_queueFamilyIndex = queueFamilyIndex;
  m_disabled         = false;

#if defined(_WIN32)
  m_getMemoryWin32Handle    = vkGetDeviceProcAddr(device, "vkGetMemoryWin32HandleKHR");
  m_getSemaphoreWin32Handle = vkGetDeviceProcAddr(device, "vkGetSemaphoreWin32HandleKHR");
  m_available               = m_getMemoryWin32Handle != nullptr && m_getSemaphoreWin32Handle != nullptr;
  if(!m_available)
  {
    LOGW(
        "Active stereo interop: Vulkan Win32 external-memory functions are "
        "unavailable; using CPU readback fallback.\n");
  }
#else
  m_available = false;
#endif

  return m_available;
}

void StereoInteropVk::deinit()
{
  if(m_device != VK_NULL_HANDLE)
    vkDeviceWaitIdle(m_device);

  destroyResources();
  m_physicalDevice          = VK_NULL_HANDLE;
  m_device                  = VK_NULL_HANDLE;
  m_queueFamilyIndex        = VK_QUEUE_FAMILY_IGNORED;
  m_getMemoryWin32Handle    = nullptr;
  m_getSemaphoreWin32Handle = nullptr;
  m_available               = false;
  m_disabled                = false;
  m_generation              = 0;
}

bool StereoInteropVk::supportsExternalImage(VkFormat format) const
{
#if defined(_WIN32)
  VkPhysicalDeviceExternalImageFormatInfo externalInfo{
      .sType      = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
  };
  VkPhysicalDeviceImageFormatInfo2 formatInfo{
      .sType  = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
      .pNext  = &externalInfo,
      .format = format,
      .type   = VK_IMAGE_TYPE_2D,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage  = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
      .flags  = 0,
  };
  VkExternalImageFormatProperties externalProperties{
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
  };
  VkImageFormatProperties2 properties{
      .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
      .pNext = &externalProperties,
  };

  if(vkGetPhysicalDeviceImageFormatProperties2(m_physicalDevice, &formatInfo, &properties) != VK_SUCCESS)
    return false;

  const VkExternalMemoryFeatureFlags features = externalProperties.externalMemoryProperties.externalMemoryFeatures;
  return (features & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0;
#else
  (void)format;
  return false;
#endif
}

bool StereoInteropVk::supportsBlit(VkFormat sourceFormat) const
{
  VkFormatProperties sourceProperties{};
  VkFormatProperties destinationProperties{};
  vkGetPhysicalDeviceFormatProperties(m_physicalDevice, sourceFormat, &sourceProperties);
  vkGetPhysicalDeviceFormatProperties(m_physicalDevice, getSharedFormat(), &destinationProperties);

  return (sourceProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0
         && (destinationProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0;
}

uint32_t StereoInteropVk::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required) const
{
  VkPhysicalDeviceMemoryProperties memoryProperties{};
  vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memoryProperties);
  for(uint32_t index = 0; index < memoryProperties.memoryTypeCount; ++index)
  {
    if((typeBits & (1U << index)) != 0 && (memoryProperties.memoryTypes[index].propertyFlags & required) == required)
      return index;
  }
  return UINT32_MAX;
}

bool StereoInteropVk::createEye(EyeResource& eye)
{
#if !defined(_WIN32)
  (void)eye;
  return false;
#else
  const VkExternalMemoryImageCreateInfo externalImageInfo{
      .sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
  };
  const VkImageCreateInfo imageInfo{
      .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext         = &externalImageInfo,
      .imageType     = VK_IMAGE_TYPE_2D,
      .format        = getSharedFormat(),
      .extent        = {m_size.width, m_size.height, 1},
      .mipLevels     = 1,
      .arrayLayers   = 1,
      .samples       = VK_SAMPLE_COUNT_1_BIT,
      .tiling        = VK_IMAGE_TILING_OPTIMAL,
      .usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
      .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };

  if(vkCreateImage(m_device, &imageInfo, nullptr, &eye.image) != VK_SUCCESS)
    return false;

  VkMemoryRequirements requirements{};
  vkGetImageMemoryRequirements(m_device, eye.image, &requirements);
  const uint32_t memoryType = findMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if(memoryType == UINT32_MAX)
    return false;

  const VkExportMemoryAllocateInfo exportInfo{
      .sType       = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
  };
  const VkMemoryDedicatedAllocateInfo dedicatedInfo{
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .pNext = &exportInfo,
      .image = eye.image,
  };
  const VkMemoryAllocateInfo allocationInfo{
      .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext           = &dedicatedInfo,
      .allocationSize  = requirements.size,
      .memoryTypeIndex = memoryType,
  };

  if(vkAllocateMemory(m_device, &allocationInfo, nullptr, &eye.memory) != VK_SUCCESS)
    return false;
  if(vkBindImageMemory(m_device, eye.image, eye.memory, 0) != VK_SUCCESS)
    return false;

  auto getMemoryHandle = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(m_getMemoryWin32Handle);
  const VkMemoryGetWin32HandleInfoKHR handleInfo{
      .sType      = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR,
      .memory     = eye.memory,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
  };
  HANDLE handle = nullptr;
  if(getMemoryHandle(m_device, &handleInfo, &handle) != VK_SUCCESS || handle == nullptr)
    return false;

  eye.memoryHandle   = handle;
  eye.allocationSize = requirements.size;
  return true;
#endif
}

bool StereoInteropVk::createSemaphore(VkSemaphore& semaphore, void*& handle)
{
#if !defined(_WIN32)
  (void)semaphore;
  (void)handle;
  return false;
#else
  const VkExportSemaphoreCreateInfo exportInfo{
      .sType       = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
      .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT,
  };
  const VkSemaphoreCreateInfo createInfo{
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
      .pNext = &exportInfo,
  };
  if(vkCreateSemaphore(m_device, &createInfo, nullptr, &semaphore) != VK_SUCCESS)
    return false;

  auto getSemaphoreHandle = reinterpret_cast<PFN_vkGetSemaphoreWin32HandleKHR>(m_getSemaphoreWin32Handle);
  const VkSemaphoreGetWin32HandleInfoKHR handleInfo{
      .sType      = VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR,
      .semaphore  = semaphore,
      .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT,
  };
  HANDLE nativeHandle = nullptr;
  if(getSemaphoreHandle(m_device, &handleInfo, &nativeHandle) != VK_SUCCESS || nativeHandle == nullptr)
    return false;

  handle = nativeHandle;
  return true;
#endif
}

bool StereoInteropVk::recreate(VkExtent2D size, VkFormat sourceFormat)
{
  if(!isAvailable() || size.width == 0 || size.height == 0 || sourceFormat == VK_FORMAT_UNDEFINED)
    return false;

  vkDeviceWaitIdle(m_device);
  destroyResources();

  m_size         = size;
  m_sourceFormat = sourceFormat;

  if(!supportsExternalImage(getSharedFormat()))
  {
    LOGW(
        "Active stereo interop: RGBA8 exportable Vulkan images are "
        "unsupported; using CPU fallback.\n");
    m_disabled = true;
    return false;
  }
  if(!supportsBlit(sourceFormat))
  {
    LOGW(
        "Active stereo interop: Vulkan format conversion %s -> RGBA8 cannot "
        "use image blit; using CPU fallback.\n",
        string_VkFormat(sourceFormat));
    m_disabled = true;
    return false;
  }

  for(uint32_t slotIndex = 0; slotIndex < kRingSize; ++slotIndex)
  {
    Slot& slot = m_slots[slotIndex];
    for(uint32_t eyeIndex = 0; eyeIndex < kEyeCount; ++eyeIndex)
    {
      if(!createEye(slot.eyes[eyeIndex]))
      {
        LOGW(
            "Active stereo interop: failed to create exportable Vulkan eye "
            "image; using CPU fallback.\n");
        destroyResources();
        m_disabled = true;
        return false;
      }
      m_exports[slotIndex].eyes[eyeIndex] = {
          .memoryHandle   = slot.eyes[eyeIndex].memoryHandle,
          .allocationSize = slot.eyes[eyeIndex].allocationSize,
      };
    }

    if(!createSemaphore(slot.vulkanReady, slot.vulkanReadyHandle) || !createSemaphore(slot.openglDone, slot.openglDoneHandle))
    {
      LOGW(
          "Active stereo interop: failed to create exportable semaphores; "
          "using CPU fallback.\n");
      destroyResources();
      m_disabled = true;
      return false;
    }
    m_exports[slotIndex].vulkanReadyHandle = slot.vulkanReadyHandle;
    m_exports[slotIndex].openglDoneHandle  = slot.openglDoneHandle;
  }

  ++m_generation;
  LOGI(
      "Active stereo interop: created %u GPU-shared stereo slots at %ux%u, "
      "Vulkan %s -> OpenGL RGBA8.\n",
      kRingSize, size.width, size.height, string_VkFormat(sourceFormat));
  return true;
}

bool StereoInteropVk::matches(VkExtent2D size, VkFormat sourceFormat) const
{
  return isAvailable() && m_generation != 0 && m_size.width == size.width && m_size.height == size.height
         && m_sourceFormat == sourceFormat;
}

bool StereoInteropVk::recordFrame(VkCommandBuffer cmd, const StereoPresentationSource& source, uint64_t frameNumber, nvapp::Application& app)
{
  if(!matches(source.extent, source.format) || source.image == VK_NULL_HANDLE || source.layerCount < kEyeCount)
    return false;

  Slot& slot = m_slots[frameNumber % kRingSize];

  if(slot.hasBeenPresented)
  {
    app.addWaitSemaphore({
        .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = slot.openglDone,
        .value     = 0,
        .stageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
    });
    slot.hasBeenPresented = false;
  }

  const VkImageMemoryBarrier2 sourceToTransfer{
      .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask        = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
      .srcAccessMask       = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
      .dstStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
      .dstAccessMask       = VK_ACCESS_2_TRANSFER_READ_BIT,
      .oldLayout           = VK_IMAGE_LAYOUT_GENERAL,
      .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image               = source.image,
      .subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kEyeCount},
  };

  std::array<VkImageMemoryBarrier2, kEyeCount> destinationAcquire{};
  for(uint32_t eyeIndex = 0; eyeIndex < kEyeCount; ++eyeIndex)
  {
    const bool fromOpenGl        = slot.hasProducedFrame;
    destinationAcquire[eyeIndex] = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask        = VK_PIPELINE_STAGE_2_NONE,
        .srcAccessMask       = 0,
        .dstStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .dstAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .oldLayout           = fromOpenGl ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = fromOpenGl ? VK_QUEUE_FAMILY_EXTERNAL : VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = fromOpenGl ? m_queueFamilyIndex : VK_QUEUE_FAMILY_IGNORED,
        .image               = slot.eyes[eyeIndex].image,
        .subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
  }

  std::array<VkImageMemoryBarrier2, kEyeCount + 1> acquireBarriers{};
  acquireBarriers[0] = sourceToTransfer;
  std::copy(destinationAcquire.begin(), destinationAcquire.end(), acquireBarriers.begin() + 1);
  const VkDependencyInfo acquireDependency{
      .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = static_cast<uint32_t>(acquireBarriers.size()),
      .pImageMemoryBarriers    = acquireBarriers.data(),
  };
  vkCmdPipelineBarrier2(cmd, &acquireDependency);

  for(uint32_t eyeIndex = 0; eyeIndex < kEyeCount; ++eyeIndex)
  {
    const VkImageBlit2 region{
        .sType          = VK_STRUCTURE_TYPE_IMAGE_BLIT_2,
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, eyeIndex, 1},
        .srcOffsets      = {{0, 0, 0},
                            {static_cast<int32_t>(source.extent.width), static_cast<int32_t>(source.extent.height), 1}},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstOffsets      = {{0, 0, 0}, {static_cast<int32_t>(m_size.width), static_cast<int32_t>(m_size.height), 1}},
    };
    const VkBlitImageInfo2 blitInfo{
        .sType          = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2,
        .srcImage       = source.image,
        .srcImageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .dstImage       = slot.eyes[eyeIndex].image,
        .dstImageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .regionCount    = 1,
        .pRegions       = &region,
        .filter         = VK_FILTER_NEAREST,
    };
    vkCmdBlitImage2(cmd, &blitInfo);
  }

  std::array<VkImageMemoryBarrier2, kEyeCount + 1> releaseBarriers{};
  releaseBarriers[0] = {
      .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
      .srcAccessMask       = VK_ACCESS_2_TRANSFER_READ_BIT,
      .dstStageMask        = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      .dstAccessMask       = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
      .oldLayout           = VK_IMAGE_LAYOUT_GENERAL,
      .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image               = source.image,
      .subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kEyeCount},
  };
  for(uint32_t eyeIndex = 0; eyeIndex < kEyeCount; ++eyeIndex)
  {
    releaseBarriers[eyeIndex + 1] = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask        = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .srcAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask        = VK_PIPELINE_STAGE_2_NONE,
        .dstAccessMask       = 0,
        .oldLayout           = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = m_queueFamilyIndex,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
        .image               = slot.eyes[eyeIndex].image,
        .subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
  }
  const VkDependencyInfo releaseDependency{
      .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = static_cast<uint32_t>(releaseBarriers.size()),
      .pImageMemoryBarriers    = releaseBarriers.data(),
  };
  vkCmdPipelineBarrier2(cmd, &releaseDependency);

  app.addSignalSemaphore({
      .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
      .semaphore = slot.vulkanReady,
      .value     = 0,
      .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
  });

  slot.frameNumber      = frameNumber;
  slot.hasProducedFrame = true;
  return true;
}

void StereoInteropVk::notifyOpenGlPresented(uint64_t frameNumber)
{
  Slot& slot = m_slots[frameNumber % kRingSize];
  if(slot.hasProducedFrame && slot.frameNumber == frameNumber)
    slot.hasBeenPresented = true;
}

bool StereoInteropVk::hasProducedFrame(uint64_t frameNumber) const
{
  if(!isAvailable() || m_generation == 0)
    return false;
  const Slot& slot = m_slots[frameNumber % kRingSize];
  return slot.hasProducedFrame && slot.frameNumber == frameNumber;
}

const StereoInteropVk::ExportedSlot& StereoInteropVk::getExportedSlot(uint32_t slotIndex) const
{
  return m_exports[slotIndex % kRingSize];
}

void StereoInteropVk::disable()
{
  if(!m_disabled)
    LOGW(
        "Active stereo interop: disabling GPU-shared presentation and falling "
        "back to CPU readback.\n");
  m_disabled = true;
}

void StereoInteropVk::closeNativeHandle(void*& handle)
{
#if defined(_WIN32)
  if(handle != nullptr)
    CloseHandle(static_cast<HANDLE>(handle));
#endif
  handle = nullptr;
}

void StereoInteropVk::destroyResources()
{
  if(m_device == VK_NULL_HANDLE)
    return;

  for(Slot& slot : m_slots)
  {
    closeNativeHandle(slot.vulkanReadyHandle);
    closeNativeHandle(slot.openglDoneHandle);
    if(slot.vulkanReady != VK_NULL_HANDLE)
      vkDestroySemaphore(m_device, slot.vulkanReady, nullptr);
    if(slot.openglDone != VK_NULL_HANDLE)
      vkDestroySemaphore(m_device, slot.openglDone, nullptr);

    for(EyeResource& eye : slot.eyes)
    {
      closeNativeHandle(eye.memoryHandle);
      if(eye.image != VK_NULL_HANDLE)
        vkDestroyImage(m_device, eye.image, nullptr);
      if(eye.memory != VK_NULL_HANDLE)
        vkFreeMemory(m_device, eye.memory, nullptr);
      eye = {};
    }
    slot = {};
  }

  m_exports      = {};
  m_size         = {0, 0};
  m_sourceFormat = VK_FORMAT_UNDEFINED;
}

}  // namespace curved_stereo_gs
