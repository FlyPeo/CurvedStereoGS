/*
 * Test entry: render the current CurvedStereoGS scene to the regular offscreen output,
 * then sample that image on a trackball-controlled quad drawn into the swapchain.
 */

#include "gaussian_splatting_app.h"
#include "hardware_support.h"

#include <array>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <nvvk/barriers.hpp>
#include <nvvk/check_error.hpp>
#include <nvvk/debug_util.hpp>
#include <nvvk/default_structs.hpp>
#include <nvvk/graphics_pipeline.hpp>
#include <nvvk/helpers.hpp>
#include <nvvk/physical_device.hpp>
#include <nvvk/resource_allocator.hpp>
#include <nvvk/sampler_pool.hpp>
#include <nvvk/shaders.hpp>
#include <nvvk/validation_settings.hpp>
#include <nvslang/slang.hpp>

#include <glm/common.hpp>

using namespace curved_stereo_gs;

class TextureSourceApp : public GaussianSplattingApp
{
public:
  using GaussianSplattingApp::GaussianSplattingApp;

  VkDescriptorSet currentTextureDescriptor() { return getPresentationImageDescriptorSet(); }

  void onAttach(nvapp::Application* app) override
  {
    GaussianSplattingApp::onAttach(app);
    m_clearColor = {{0.0f, 0.0f, 0.0f, 1.0f}};
  }

  void onUIRender() override
  {
    // The test draws the scene result onto a custom textured quad instead of
    // placing it directly as an ImGui background image.
  }
};

class TexturedQuadOverlay : public nvapp::IAppElement
{
public:
  explicit TexturedQuadOverlay(TextureSourceApp* source)
      : m_source(source)
  {
  }

  void onAttach(nvapp::Application* app) override
  {
    m_app            = app;
    m_device         = app->getDevice();
    m_physicalDevice = app->getPhysicalDevice();

    m_alloc.init(VmaAllocatorCreateInfo{
        .flags            = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
        .physicalDevice   = app->getPhysicalDevice(),
        .device           = app->getDevice(),
        .instance         = app->getInstance(),
        .vulkanApiVersion = VK_API_VERSION_1_4,
    });

    createDescriptorLayouts();
    createSamplerSet();
    createShaders();

    app->setFrameGenerationCallback([this](VkCommandBuffer cmd, VkImage, VkImageView swapchainView, VkFormat swapchainFormat,
                                           VkExtent2D size, uint64_t frameNumber) {
      draw(cmd, swapchainView, swapchainFormat, size, frameNumber);
    });
  }

  void onDetach() override
  {
    if(m_app)
      m_app->setFrameGenerationCallback({});

    vkDeviceWaitIdle(m_device);

    if(m_pipeline)
      vkDestroyPipeline(m_device, m_pipeline, nullptr);
    if(m_pipelineLayout)
      vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    if(m_vertexShader)
      vkDestroyShaderModule(m_device, m_vertexShader, nullptr);
    if(m_fragmentShader)
      vkDestroyShaderModule(m_device, m_fragmentShader, nullptr);
    if(m_sampler)
      vkDestroySampler(m_device, m_sampler, nullptr);
    if(m_samplerSetLayout)
      vkDestroyDescriptorSetLayout(m_device, m_samplerSetLayout, nullptr);
    if(m_imageSetLayout)
      vkDestroyDescriptorSetLayout(m_device, m_imageSetLayout, nullptr);

    m_alloc.deinit();
  }

  void onUIRender() override
  {
    ImGuiIO& io = ImGui::GetIO();
    if(io.WantCaptureMouse)
    {
      m_dragging = false;
      return;
    }

    const ImVec2 mousePos = ImGui::GetMousePos();
    if(ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
      m_dragging  = true;
      m_lastMouse = {mousePos.x, mousePos.y};
    }

    if(ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
      m_dragging = false;
    }

    if(m_dragging && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
      const glm::vec2 current{mousePos.x, mousePos.y};
      const glm::vec2 delta = current - m_lastMouse;
      m_lastMouse           = current;

      constexpr float sensitivity = 0.01f;
      m_yaw += delta.x * sensitivity;
      m_pitch += delta.y * sensitivity;
      m_pitch = glm::clamp(m_pitch, -1.45f, 1.45f);
    }
  }

private:
  struct PushConstant
  {
    glm::vec4 color{1.0f};
    glm::vec4 rect{0.0f, 0.0f, 1.0f, 1.0f};    // center.xy, halfSize.xy in clip space
    glm::vec4 params{0.0f, 0.0f, 0.0f, 0.0f};  // mode, z, yaw, pitch
  };

  static constexpr const char* kQuadShader = R"(
struct PushConstant
{
  float4 color;
  float4 rect;
  float4 params;
};

[[vk::push_constant]] ConstantBuffer<PushConstant> pc;
[[vk::binding(0, 0)]] Texture2D<float4> sceneTexture;
[[vk::binding(0, 1)]] SamplerState sceneSampler;

struct VertexOutput
{
  [[vk::location(0)]] float2 uv : TEXCOORD0;
  float4 position : SV_Position;
};

static const float2 kQuadPositions[6] = {
  float2(-1.0, -1.0),
  float2( 1.0, -1.0),
  float2( 1.0,  1.0),
  float2(-1.0, -1.0),
  float2( 1.0,  1.0),
  float2(-1.0,  1.0)
};

static const float2 kUvs[6] = {
  float2(0.0, 1.0),
  float2(1.0, 1.0),
  float2(1.0, 0.0),
  float2(0.0, 1.0),
  float2(1.0, 0.0),
  float2(0.0, 0.0)
};

[shader("vertex")]
VertexOutput vertmain(uint vertexId : SV_VertexID)
{
  VertexOutput output;

  float3 pos = float3(kQuadPositions[vertexId] * pc.rect.zw, 0.0);

  const float cy = cos(pc.params.z);
  const float sy = sin(pc.params.z);
  const float cp = cos(pc.params.w);
  const float sp = sin(pc.params.w);

  pos = float3(cy * pos.x + sy * pos.z, pos.y, -sy * pos.x + cy * pos.z);
  pos = float3(pos.x, cp * pos.y - sp * pos.z, sp * pos.y + cp * pos.z);

  const float perspective = 1.0 / max(0.35, 1.0 - pos.z * 0.35);
  const float2 screenPos = pc.rect.xy + pos.xy * perspective;

  output.position = float4(screenPos, pc.params.y, 1.0);
  output.uv = kUvs[vertexId];
  return output;
}

[shader("fragment")]
float4 fragmain(VertexOutput input) : SV_Target
{
  if(pc.params.x < 0.5)
  {
    return pc.color;
  }
  const float4 color = sceneTexture.Sample(sceneSampler, input.uv);
  return float4(color.rgb, 1.0);
}
)";

  void createDescriptorLayouts()
  {
    const VkDescriptorSetLayoutBinding imageBinding{
        .binding         = 0,
        .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        .descriptorCount = 1,
        .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    const VkDescriptorSetLayoutCreateInfo imageLayoutInfo{
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings    = &imageBinding,
    };
    NVVK_CHECK(vkCreateDescriptorSetLayout(m_device, &imageLayoutInfo, nullptr, &m_imageSetLayout));

    const VkDescriptorSetLayoutBinding samplerBinding{
        .binding         = 0,
        .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    const VkDescriptorSetLayoutCreateInfo samplerLayoutInfo{
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings    = &samplerBinding,
    };
    NVVK_CHECK(vkCreateDescriptorSetLayout(m_device, &samplerLayoutInfo, nullptr, &m_samplerSetLayout));

    const std::array<VkDescriptorSetLayout, 2> setLayouts{m_imageSetLayout, m_samplerSetLayout};
    const VkPushConstantRange pushRange{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset     = 0,
        .size       = sizeof(PushConstant),
    };
    const VkPipelineLayoutCreateInfo pipelineLayoutInfo{
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount         = static_cast<uint32_t>(setLayouts.size()),
        .pSetLayouts            = setLayouts.data(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &pushRange,
    };
    NVVK_CHECK(vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout));
  }

  void createSamplerSet()
  {
    VkSamplerCreateInfo samplerInfo = DEFAULT_VkSamplerCreateInfo;
    samplerInfo.addressModeU       = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV       = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW       = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    NVVK_CHECK(vkCreateSampler(m_device, &samplerInfo, nullptr, &m_sampler));

    const VkDescriptorSetAllocateInfo allocInfo{
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = m_app->getTextureDescriptorPool(),
        .descriptorSetCount = 1,
        .pSetLayouts        = &m_samplerSetLayout,
    };
    NVVK_CHECK(vkAllocateDescriptorSets(m_device, &allocInfo, &m_samplerSet));

    const VkDescriptorImageInfo samplerDescriptor{.sampler = m_sampler};
    const VkWriteDescriptorSet write{
        .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet          = m_samplerSet,
        .dstBinding      = 0,
        .descriptorCount = 1,
        .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER,
        .pImageInfo      = &samplerDescriptor,
    };
    vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
  }

  void createShaders()
  {
    nvslang::SlangCompiler compiler;
    compiler.defaultTarget();
    compiler.defaultOptions();

    if(!compiler.loadFromSourceString("curved_stereo_gs_texture_quad_test", kQuadShader))
    {
      LOGE("Failed to compile quad shader: %s\n", compiler.getLastDiagnosticMessage().c_str());
      return;
    }

    const std::span<const uint32_t> spirv(compiler.getSpirv(), compiler.getSpirvSize() / sizeof(uint32_t));
    NVVK_CHECK(nvvk::createShaderModule(m_vertexShader, m_device, spirv));
    NVVK_CHECK(nvvk::createShaderModule(m_fragmentShader, m_device, spirv));
  }

  void ensurePipeline(VkFormat swapchainFormat)
  {
    if(m_pipeline && m_pipelineFormat == swapchainFormat)
      return;

    if(m_pipeline)
      vkDestroyPipeline(m_device, m_pipeline, nullptr);
    m_pipeline = VK_NULL_HANDLE;

    nvvk::GraphicsPipelineState state;
    state.rasterizationState.cullMode        = VK_CULL_MODE_NONE;
    state.depthStencilState.depthTestEnable  = VK_FALSE;
    state.depthStencilState.depthWriteEnable = VK_FALSE;

    nvvk::GraphicsPipelineCreator creator;
    creator.pipelineInfo.layout = m_pipelineLayout;
    creator.colorFormats        = {swapchainFormat};
    creator.addShader(VK_SHADER_STAGE_VERTEX_BIT, "vertmain", m_vertexShader);
    creator.addShader(VK_SHADER_STAGE_FRAGMENT_BIT, "fragmain", m_fragmentShader);

    NVVK_CHECK(creator.createGraphicsPipeline(m_device, VK_NULL_HANDLE, state, &m_pipeline));
    m_pipelineFormat = swapchainFormat;
  }

  void drawWithPush(VkCommandBuffer cmd, const PushConstant& push)
  {
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 6, 1, 0, 0);
  }

  void draw(VkCommandBuffer cmd, VkImageView swapchainView, VkFormat swapchainFormat, VkExtent2D size, uint64_t frameNumber)
  {
    VkDescriptorSet sceneTexture = m_source ? m_source->currentTextureDescriptor() : VK_NULL_HANDLE;
    if(sceneTexture == VK_NULL_HANDLE || size.width == 0 || size.height == 0 || m_vertexShader == VK_NULL_HANDLE
       || m_fragmentShader == VK_NULL_HANDLE)
    {
      return;
    }

    ensurePipeline(swapchainFormat);

    VkRenderingAttachmentInfo colorAttachment = DEFAULT_VkRenderingAttachmentInfo;
    colorAttachment.imageView                 = swapchainView;
    colorAttachment.imageLayout               = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp                    = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp                   = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.clearValue.color          = {{0.01f, 0.03f, 0.16f, 1.0f}};

    VkRenderingInfo renderingInfo      = DEFAULT_VkRenderingInfo;
    renderingInfo.renderArea           = DEFAULT_VkRect2D(size);
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments    = &colorAttachment;

    nvvk::DebugUtil::ScopedCmdLabel label(cmd, "Centered trackball textured quad");
    vkCmdBeginRendering(cmd, &renderingInfo);
    nvvk::GraphicsPipelineState::cmdSetViewportAndScissor(cmd, size);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    const std::array<VkDescriptorSet, 2> sets{sceneTexture, m_samplerSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, static_cast<uint32_t>(sets.size()),
                            sets.data(), 0, nullptr);

    PushConstant push{};
    push.params = {1.0f, 0.5f, m_yaw, m_pitch};
    push.rect   = {0.0f, 0.0f, 0.5f, 0.5f};
    drawWithPush(cmd, push);

    vkCmdEndRendering(cmd);

    (void)frameNumber;
  }

  TextureSourceApp* m_source = nullptr;
  nvapp::Application* m_app  = nullptr;

  VkDevice         m_device         = VK_NULL_HANDLE;
  VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;

  nvvk::ResourceAllocator m_alloc;

  VkDescriptorSetLayout m_imageSetLayout   = VK_NULL_HANDLE;
  VkDescriptorSetLayout m_samplerSetLayout = VK_NULL_HANDLE;
  VkDescriptorSet       m_samplerSet       = VK_NULL_HANDLE;
  VkSampler             m_sampler          = VK_NULL_HANDLE;

  VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
  VkPipeline       m_pipeline       = VK_NULL_HANDLE;
  VkFormat         m_pipelineFormat = VK_FORMAT_UNDEFINED;
  VkShaderModule   m_vertexShader   = VK_NULL_HANDLE;
  VkShaderModule   m_fragmentShader = VK_NULL_HANDLE;

  glm::vec2 m_lastMouse{0.0f, 0.0f};
  float     m_yaw      = 0.0f;
  float     m_pitch    = 0.0f;
  bool      m_dragging = false;
};

int main(int argc, char** argv)
{
  nvutils::Logger::getInstance().breakOnError(false);

  nvutils::ProfilerManager              profilerManager;
  nvutils::ParameterRegistry            parameterRegistry;
  nvutils::ParameterParser              parameterParser(nvutils::getExecutablePath().stem().string(), {".txt"});
  nvutils::ParameterSequencer::InitInfo sequencerInfo{
      .parameterParser   = &parameterParser,
      .parameterRegistry = &parameterRegistry,
      .profilerManager   = &profilerManager,
  };

  nvvk::Context                vkContext;
  nvvk::ContextInitInfo        vkSetup;
  nvapp::Application           application;
  nvapp::ApplicationCreateInfo appInfo;
  bool                         benchmarkMode = false;

  parameterRegistry.addVector({"size", "Size of the window to be created"}, &appInfo.windowSize);
  parameterRegistry.add({"vsync"}, &appInfo.vSync);
  parameterRegistry.add({"headless", "Run without a window"}, &appInfo.headless);
  parameterRegistry.add({"headlessFrameCount", "Number of frames to render in headless mode"}, &appInfo.headlessFrameCount);
  parameterRegistry.add({"verbose", "Verbose output of the Vulkan context"}, &vkSetup.verbose);
  parameterRegistry.add({"validation", "Enable validation layers"}, &vkSetup.enableValidationLayers);
  parameterRegistry.add({"benchmark", "Enable benchmarking"}, &benchmarkMode);
  parameterRegistry.add({"forcegpu", "Force the use of a specific GPU by providing its ID"}, &vkSetup.forceGPU);

  std::string sceneFile;
  parameterRegistry.add({"scene", "Path to a .ply, .spz, .splat, .obj, .gltf, .glb, .csgs, or legacy .vkgs file to load on startup"}, &sceneFile);

  registerCommandLineParameters(&parameterRegistry);

  auto gaussianSplatting = std::make_shared<TextureSourceApp>(&profilerManager, &parameterRegistry, &benchmarkMode);

  sequencerInfo.registerScriptParameters(parameterRegistry, parameterParser);
  sequencerInfo.postCallbacks.emplace_back(
      [&](const nvutils::ParameterSequencer::State&) { gaussianSplatting->benchmarkAdvance(); });

  parameterParser.add(parameterRegistry);
  parameterParser.parse(argc, argv);
  storeDefaultParameters();
  parameterParser.setVerbose(true);

  if(!sceneFile.empty())
    gaussianSplatting->onFileDrop(sceneFile);

  auto elemSequencer = std::make_shared<nvapp::ElementSequencer>(sequencerInfo);

  vkSetup.enableAllFeatures = true;
  vkSetup.instanceExtensions.emplace_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

  static VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR baryFeaturesKHR = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR};
  static VkPhysicalDeviceMeshShaderFeaturesEXT meshFeaturesEXT = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT,
  };
  static VkPhysicalDeviceFragmentShadingRateFeaturesKHR fragFeaturesKHR = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR,
  };

  vkSetup.deviceExtensions.emplace_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
  vkSetup.deviceExtensions.emplace_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  vkSetup.deviceExtensions.emplace_back(VK_EXT_MESH_SHADER_EXTENSION_NAME, &meshFeaturesEXT, false);
  vkSetup.deviceExtensions.emplace_back(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME, &fragFeaturesKHR, true);
  vkSetup.deviceExtensions.emplace_back(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME, &baryFeaturesKHR, false);
  vkSetup.deviceExtensions.emplace_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);

  VkPhysicalDeviceAccelerationStructureFeaturesKHR accelFeature = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
  vkSetup.deviceExtensions.emplace_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, &accelFeature, false);
  VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtPipelineFeature = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
  vkSetup.deviceExtensions.emplace_back(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME, &rtPipelineFeature, false);
  vkSetup.deviceExtensions.emplace_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME, nullptr, false);
  VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR rtPositionFetchFeature = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_POSITION_FETCH_FEATURES_KHR, .rayTracingPositionFetch = VK_TRUE};
  vkSetup.deviceExtensions.emplace_back(VK_KHR_RAY_TRACING_POSITION_FETCH_EXTENSION_NAME, &rtPositionFetchFeature, false);

  VkPhysicalDeviceShaderClockFeaturesKHR clockFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR};
  vkSetup.deviceExtensions.emplace_back(VK_KHR_SHADER_CLOCK_EXTENSION_NAME, &clockFeatures);

  VkPhysicalDeviceRayTracingInvocationReorderFeaturesNV serFeatures = {
      .sType                       = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_NV,
      .rayTracingInvocationReorder = VK_TRUE,
  };
  vkSetup.deviceExtensions.emplace_back(VK_NV_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME, &serFeatures, false);

  static VkPhysicalDeviceRayTracingLinearSweptSpheresFeaturesNV lssFeaturesNV = {
      .sType   = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_LINEAR_SWEPT_SPHERES_FEATURES_NV,
      .spheres = VK_TRUE,
  };
  vkSetup.deviceExtensions.emplace_back(VK_NV_RAY_TRACING_LINEAR_SWEPT_SPHERES_EXTENSION_NAME, &lssFeaturesNV, false);
  vkSetup.deviceExtensions.emplace_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);

  static VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT interlockFeatures = {
      .sType                        = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT,
      .fragmentShaderPixelInterlock = VK_TRUE,
  };
  vkSetup.deviceExtensions.emplace_back(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME, &interlockFeatures, true);

  if(!appInfo.headless)
  {
    nvvk::addSurfaceExtensions(vkSetup.instanceExtensions);
    vkSetup.deviceExtensions.emplace_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  }

  nvvk::ValidationSettings vvlInfo{};
  vvlInfo.setPreset(nvvk::ValidationSettings::LayerPresets::eStandard);
  vvlInfo.check_shaders         = VK_FALSE;
  vvlInfo.check_shaders_caching = VK_FALSE;
  vkSetup.instanceCreateInfoExt = vvlInfo.buildPNextChain();

  vkSetup.apiVersion = VK_API_VERSION_1_4;
  if(vkContext.init(vkSetup) != VK_SUCCESS)
  {
    LOGE("Error in Vulkan context creation (Vulkan 1.4 is required).\n");
    return 1;
  }

  isSupported.initFromVulkanContext(vkContext);

  appInfo.name                  = "CurvedStereoGS_Test";
  appInfo.instance              = vkContext.getInstance();
  appInfo.device                = vkContext.getDevice();
  appInfo.physicalDevice        = vkContext.getPhysicalDevice();
  appInfo.queues                = vkContext.getQueueInfos();
  appInfo.hasUndockableViewport = false;
  appInfo.useMenu               = false;
  if(appInfo.headless && benchmarkMode)
    appInfo.headlessFrameCount = UINT32_MAX;

  application.init(appInfo);
  application.addElement(elemSequencer);
  application.addElement(gaussianSplatting);

  application.addElement(std::make_shared<TexturedQuadOverlay>(gaussianSplatting.get()));

  if(benchmarkMode)
    application.setVsync(false);

  application.run();

  application.deinit();
  vkContext.deinit();
  return 0;
}
