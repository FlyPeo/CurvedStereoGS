/*
 * Vulkan/OpenGL active-stereo interop support.
 *
 * The renderer keeps using a WGL quad-buffered default framebuffer.  This
 * module only replaces the per-frame Vulkan -> CPU -> OpenGL image transfer
 * with exportable Vulkan images and binary semaphores.
 */

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace nvapp {
class Application;
}

namespace curved_stereo_gs {

struct StereoPresentationSource
{
  VkImage    image{VK_NULL_HANDLE};
  VkFormat   format{VK_FORMAT_UNDEFINED};
  VkExtent2D extent{0, 0};
  uint32_t   layerCount{0};
};

class StereoInteropVk
{
public:
  static constexpr uint32_t kEyeCount = 2;
  static constexpr uint32_t kRingSize = 3;

  struct ExportedEye
  {
    void*        memoryHandle{nullptr};
    VkDeviceSize allocationSize{0};
  };

  struct ExportedSlot
  {
    std::array<ExportedEye, kEyeCount> eyes{};
    void*                              vulkanReadyHandle{nullptr};
    void*                              openglDoneHandle{nullptr};
  };

  ~StereoInteropVk();

  bool init(VkPhysicalDevice physicalDevice, VkDevice device, uint32_t queueFamilyIndex);
  void deinit();

  bool recreate(VkExtent2D size, VkFormat sourceFormat);
  bool matches(VkExtent2D size, VkFormat sourceFormat) const;

  bool recordFrame(VkCommandBuffer cmd, const StereoPresentationSource& source, uint64_t frameNumber, nvapp::Application& app);

  void notifyOpenGlPresented(uint64_t frameNumber);
  void disable();

  bool isAvailable() const { return m_available && !m_disabled; }
  bool hasProducedFrame(uint64_t frameNumber) const;

  VkExtent2D          getSize() const { return m_size; }
  VkFormat            getSharedFormat() const { return VK_FORMAT_R8G8B8A8_UNORM; }
  uint64_t            getGeneration() const { return m_generation; }
  uint32_t            getRingSize() const { return kRingSize; }
  const ExportedSlot& getExportedSlot(uint32_t slotIndex) const;

private:
  struct EyeResource
  {
    VkImage        image{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    VkDeviceSize   allocationSize{0};
    void*          memoryHandle{nullptr};
  };

  struct Slot
  {
    std::array<EyeResource, kEyeCount> eyes{};
    VkSemaphore                        vulkanReady{VK_NULL_HANDLE};
    VkSemaphore                        openglDone{VK_NULL_HANDLE};
    void*                              vulkanReadyHandle{nullptr};
    void*                              openglDoneHandle{nullptr};
    bool                               acquiredFromOpenGl{false};
    bool                               hasBeenPresented{false};
    bool                               hasProducedFrame{false};
    uint64_t                           frameNumber{0};
  };

  bool     supportsExternalImage(VkFormat format) const;
  bool     supportsBlit(VkFormat sourceFormat) const;
  bool     createEye(EyeResource& eye);
  bool     createSemaphore(VkSemaphore& semaphore, void*& handle);
  uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required) const;
  void     destroyResources();
  void     closeNativeHandle(void*& handle);

  VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};
  VkDevice         m_device{VK_NULL_HANDLE};
  uint32_t         m_queueFamilyIndex{VK_QUEUE_FAMILY_IGNORED};
  VkExtent2D       m_size{0, 0};
  VkFormat         m_sourceFormat{VK_FORMAT_UNDEFINED};
  uint64_t         m_generation{0};
  bool             m_available{false};
  bool             m_disabled{false};

  std::array<Slot, kRingSize>         m_slots{};
  std::array<ExportedSlot, kRingSize> m_exports{};
  PFN_vkVoidFunction                  m_getMemoryWin32Handle{nullptr};
  PFN_vkVoidFunction                  m_getSemaphoreWin32Handle{nullptr};
};

}  // namespace curved_stereo_gs
