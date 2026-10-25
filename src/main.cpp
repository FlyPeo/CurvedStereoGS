/*
 * Copyright (c) 2023-2026, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-FileCopyrightText: Copyright (c) 2023-2026, NVIDIA CORPORATION.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "active_stereo_presenter.h"
#include "elem_camera_custom.hpp"
#include "gaussian_splatting_app.h"
#include "hardware_support.h"
#include "stereo_frame_stats.h"
#include "stereo_interop_vk.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#undef APIENTRY

#if USE_DLSS
#include "dlss_wrapper.hpp"
#endif

using namespace curved_stereo_gs;

namespace {

std::filesystem::path getDefaultScenePath()
{
  const std::filesystem::path exeDir = nvutils::getExecutablePath().parent_path();

  // Layered model layout, checked first: models/static/ holds plain scenes,
  // models/dynamic/ holds canonical .ply + matching .dgs + training_cameras/.
  // Scanning dynamic before static prefers the richer scene when both exist;
  // name-sorted first non-empty match keeps the choice deterministic. Covers
  // both the portable layout (models/ next to the executable) and the
  // development tree (models/ at the repository root).
  const std::filesystem::path sourceRoot = std::filesystem::path(TARGET_EXE_TO_SOURCE_DIRECTORY);
  for(const std::filesystem::path& base : {exeDir, exeDir / sourceRoot, exeDir / ".." / sourceRoot})
  {
    for(const std::filesystem::path& folder :
        {base / "models" / "dynamic", base / "models" / "static", base / "models"})
    {
      std::error_code                    ec;
      std::vector<std::filesystem::path> candidates;
      for(std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
      {
        if(!it->is_regular_file() || it->file_size(ec) == 0)
          continue;
        std::string ext = it->path().extension().string();
        for(char& c : ext)
          c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if(ext == ".ply" || ext == ".spz" || ext == ".csgs" || ext == ".vkgs")
          candidates.push_back(it->path());
      }
      std::sort(candidates.begin(), candidates.end());
      if(!candidates.empty())
        return candidates.front();
    }
  }

  const std::filesystem::path temporalSample = sourceRoot / "assets" / "basketball_canonical.ply";
  if(std::filesystem::exists(exeDir / temporalSample))
    return temporalSample;

  const std::filesystem::path sourceScene = sourceRoot / "models" / "point_cloud.ply";
  return sourceScene;
}

std::filesystem::path resolveDefaultScenePath(const std::filesystem::path& sceneFile)
{
  if(sceneFile.is_absolute())
    return sceneFile;
  return nvutils::getExecutablePath().parent_path() / sceneFile;
}

bool getPrimaryMonitorResolution(glm::uvec2& resolution)
{
  GLFWmonitor* primaryMonitor = glfwGetPrimaryMonitor();
  if(primaryMonitor == nullptr)
    return false;

  const GLFWvidmode* videoMode = glfwGetVideoMode(primaryMonitor);
  if(videoMode == nullptr || videoMode->width <= 0 || videoMode->height <= 0)
    return false;

  resolution = {uint32_t(videoMode->width), uint32_t(videoMode->height)};
  return true;
}

void requestStereoToggle(const std::shared_ptr<GaussianSplattingApp>& gaussianSplatting);

float gQuadBufferStereoEyeSeparation = 0.065f;
bool  gQuadBufferStereoSwitch        = true;  // true = stereo, false = zero parallax

struct SideBySideOutput
{
  bool     enabled{false};
  bool     announced{false};
  VkFormat checkedSource{VK_FORMAT_UNDEFINED};
  VkFormat checkedDestination{VK_FORMAT_UNDEFINED};
  bool     blitSupported{false};
};

void recordSideBySide(VkPhysicalDevice physicalDevice, VkCommandBuffer cmd, const StereoPresentationSource& source,
                      VkImage destination, VkFormat destinationFormat, VkExtent2D destinationExtent, SideBySideOutput& output)
{
  if(source.image == VK_NULL_HANDLE || source.layerCount < 2 || source.extent.width == 0 || source.extent.height == 0
     || destinationExtent.width < 2 || destinationExtent.height == 0)
    return;

  if(source.format != output.checkedSource || destinationFormat != output.checkedDestination)
  {
    VkFormatProperties sourceProperties{}, destinationProperties{};
    vkGetPhysicalDeviceFormatProperties(physicalDevice, source.format, &sourceProperties);
    vkGetPhysicalDeviceFormatProperties(physicalDevice, destinationFormat, &destinationProperties);
    output.checkedSource      = source.format;
    output.checkedDestination = destinationFormat;
    output.blitSupported = (sourceProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)
                           && (destinationProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT);
    if(!output.blitSupported)
      LOGE("Vulkan side-by-side stereo: image formats do not support GPU blit.\n");
  }
  if(!output.blitSupported)
    return;

  const VkImageSubresourceRange sourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
  const VkImageSubresourceRange destinationRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  std::array<VkImageMemoryBarrier2, 2> before{};
  before[0] = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
               .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
               .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
               .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
               .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
               .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               .image = source.image,
               .subresourceRange = sourceRange};
  before[1] = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
               .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
               .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
               .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
               .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
               .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               .image = destination,
               .subresourceRange = destinationRange};
  VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.imageMemoryBarrierCount = uint32_t(before.size());
  dependency.pImageMemoryBarriers     = before.data();
  vkCmdPipelineBarrier2(cmd, &dependency);

  std::array<VkImageBlit2, 2> regions{};
  const int32_t halfWidth = int32_t(destinationExtent.width / 2);
  for(uint32_t eye = 0; eye < 2; ++eye)
  {
    VkImageBlit2& region = regions[eye];
    region.sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2;
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, eye, 1};
    region.srcOffsets[1] = {int32_t(source.extent.width), int32_t(source.extent.height), 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstOffsets[0] = {eye == 0 ? 0 : halfWidth, 0, 0};
    region.dstOffsets[1] = {eye == 0 ? halfWidth : int32_t(destinationExtent.width), int32_t(destinationExtent.height), 1};
  }
  VkBlitImageInfo2 blit{VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2};
  blit.srcImage       = source.image;
  blit.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  blit.dstImage       = destination;
  blit.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  blit.regionCount    = uint32_t(regions.size());
  blit.pRegions       = regions.data();
  blit.filter         = VK_FILTER_NEAREST;
  vkCmdBlitImage2(cmd, &blit);

  std::array<VkImageMemoryBarrier2, 2> after{};
  after[0] = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
              .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
              .srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
              .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
              .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
              .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              .newLayout = VK_IMAGE_LAYOUT_GENERAL,
              .image = source.image,
              .subresourceRange = sourceRange};
  after[1] = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
              .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
              .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
              .dstStageMask = VK_PIPELINE_STAGE_2_NONE,
              .dstAccessMask = VK_ACCESS_2_NONE,
              .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
              .image = destination,
              .subresourceRange = destinationRange};
  dependency.pImageMemoryBarriers = after.data();
  vkCmdPipelineBarrier2(cmd, &dependency);

  if(!output.announced)
  {
    LOGI("Vulkan side-by-side stereo active: Multiview layers 0/1 -> half-width left/right swapchain regions.\n");
    output.announced = true;
  }
}

class StereoHotkeysElement : public nvapp::IAppElement
{
public:
  explicit StereoHotkeysElement(std::shared_ptr<GaussianSplattingApp> app)
      : m_gaussianSplatting(std::move(app))
  {
  }

  void onAttach(nvapp::Application* app) override { m_app = app; }

  void onUIRender() override { handleStereoToggleHotkey(); }

  void onPreRender() override { handleStereoToggleHotkey(); }

private:
  void handleStereoToggleHotkey()
  {
    const bool isDown = isStereoToggleKeyDown();

    if(isDown && !m_wasStereoToggleKeyDown)
    {
      requestStereoToggle(m_gaussianSplatting);
    }

    m_wasStereoToggleKeyDown = isDown;
  }

  bool isStereoToggleKeyDown() const
  {
    if(m_app != nullptr && m_app->getWindowHandle() != nullptr)
    {
      GLFWwindow* window = m_app->getWindowHandle();
      return glfwGetKey(window, GLFW_KEY_0) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_KP_0) == GLFW_PRESS;
    }

    return ImGui::IsKeyDown(ImGuiKey_0) || ImGui::IsKeyDown(ImGuiKey_Keypad0);
  }

  std::shared_ptr<GaussianSplattingApp> m_gaussianSplatting;
  nvapp::Application*                   m_app{nullptr};
  bool                                  m_wasStereoToggleKeyDown{false};
};

// Exits the application when ESC is pressed in the render window.
class ExitHotkeysElement : public nvapp::IAppElement
{
public:
  void onAttach(nvapp::Application* app) override { m_app = app; }

  void onUIRender() override { handleExitHotkey(); }

  void onPreRender() override { handleExitHotkey(); }

private:
  void handleExitHotkey()
  {
    const bool isDown = isExitKeyDown();
    if(isDown && !m_wasExitKeyDown)
    {
      LOGI("ESC pressed - closing the renderer.\n");
      if(m_app != nullptr)
        m_app->close();
    }
    m_wasExitKeyDown = isDown;
  }

  bool isExitKeyDown() const
  {
    if(m_app != nullptr && m_app->getWindowHandle() != nullptr)
      return glfwGetKey(m_app->getWindowHandle(), GLFW_KEY_ESCAPE) == GLFW_PRESS;
    return ImGui::IsKeyDown(ImGuiKey_Escape);
  }

  nvapp::Application* m_app{nullptr};
  bool                m_wasExitKeyDown{false};
};

void requestStereoToggle(const std::shared_ptr<GaussianSplattingApp>& gaussianSplatting)
{
  prmStereo.mode      = STEREO_MODE_MULTIVIEW;
  prmStereo.activeEye = STEREO_EYE_CENTER;

  if(!prmStereo.enabled)
  {
    // Mono start (single-layer targets): enabling stereo needs the G-buffer /
    // pipeline rebuild path - a one-time hitch, after which toggles below are
    // instant flips of the eye separation.
    prmStereo.enabled = true;
    if(prmStereo.eyeSeparation <= 1.0e-5F)
      prmStereo.eyeSeparation = gQuadBufferStereoEyeSeparation;
    gQuadBufferStereoSwitch = true;
    gaussianSplatting->requestStereoModeRebuild();
    LOGI("Stereo hotkey 0: stereo enabled, rebuilding the viewport targets and pipelines.\n");
    return;
  }

  // Zero-parallax toggle (upstream semantics): multiview targets and pipelines
  // stay alive, only the eye separation flips between the calibrated value and
  // 0 - instant, no rebuild.
  if(gQuadBufferStereoSwitch && prmStereo.eyeSeparation > 1.0e-5F)
    gQuadBufferStereoEyeSeparation = prmStereo.eyeSeparation;

  gQuadBufferStereoSwitch = !gQuadBufferStereoSwitch;
  prmStereo.eyeSeparation = gQuadBufferStereoSwitch ? gQuadBufferStereoEyeSeparation : 0.0F;
  gaussianSplatting->requestFrameReset();

  LOGI("Stereo hotkey 0: %s eye separation %.3fm.\n",
       gQuadBufferStereoSwitch ? "restored stereo" : "zero parallax (mono content)",
       prmStereo.eyeSeparation);
}

}  // namespace

// create, setup and run an nvapp::Application
// with a GaussianSplatting element.
int main(int argc, char** argv)
{
  nvutils::Logger::getInstance().breakOnError(false);
  // nvutils::Logger::getInstance().setLogLevel(nvutils::Logger::LogLevel::eDEBUG);

  nvutils::ProfilerManager              profilerManager;
  nvutils::ParameterRegistry            parameterRegistry;
  nvutils::ParameterParser              parameterParser(nvutils::getExecutablePath().stem().string(), {".txt"});
  nvutils::ParameterSequencer::InitInfo sequencerInfo{// sequencer always requires a parser and registry
                                                      .parameterParser   = &parameterParser,
                                                      .parameterRegistry = &parameterRegistry,
                                                      // sequencer uses the profiler for benchmarking
                                                      .profilerManager = &profilerManager};

  nvvk::Context                vkContext;  // The Vulkan context
  nvvk::ContextInitInfo        vkSetup;    // Information to create the Vulkan context
  nvapp::Application           application;
  nvapp::ApplicationCreateInfo appInfo;  // Information to create the application
  bool                         benchmarkMode                = false;
  bool                         activeStereoPresenterEnabled = true;
  bool                         activeStereoOnlyWindow       = true;
  bool                         activeStereoGpuInteropEnabled = true;
  int                          stereoOutputMode = 0;  // 0=auto, 1=WGL quad-buffer, 2=Vulkan side-by-side.
  std::string                  stereoStatsCsv;             // optional per-frame CSV of stereo presentation timings
  uint32_t                     stereoStatsInterval = 300;  // running summary every N frames (0=off)

  // Use the OSG-style curved screen path by default in the main executable.
  // Pass 1 renders the scene with an off-axis frustum for the virtual flat
  // screen. Pass 2 remaps that result through the physical three-section curved
  // screen.
  prmCurvedScreenWarp.enabled          = true;
  prmCurvedScreenWarp.width            = 1.92f;
  prmCurvedScreenWarp.radius           = 0.96f;
  prmCurvedScreenWarp.bottomHeight     = 0.8f;
  prmCurvedScreenWarp.toScreenDistance = 0.96f;
  prmCurvedScreenWarp.sectionLength    = glm::vec3(0.389f, 1.508f, 0.502f);
  prmCurvedScreenWarp.offset           = glm::vec3(0.0f);
  prmStereo.enabled                    = true;
  prmStereo.mode                       = STEREO_MODE_MULTIVIEW;
  prmStereo.eyeSeparation              = 0.065f;
  prmStereo.activeEye                  = STEREO_EYE_CENTER;
  prmStereo.frameSequentialFallback    = false;

  /////////////////////////////////
  // Parse the command line to get the application creation information
  // those parameter will have no effect if changed via benchmark script
  // see GaussianSplatting constructor for other options
  parameterRegistry.addVector({"size", "Size of the window to be created"}, &appInfo.windowSize);
  parameterRegistry.add({"vsync"}, &appInfo.vSync);
  parameterRegistry.add({"headless", "Run without a window (no swapchain, no display)"}, &appInfo.headless);
  parameterRegistry.add({"headlessFrameCount",
                         "Number of frames to render in headless mode, or to "
                         "auto-close a windowed run after N frames"},
                        &appInfo.headlessFrameCount);
  parameterRegistry.add({"verbose", "Verbose output of the Vulkan context"}, &vkSetup.verbose);
  parameterRegistry.add({"validation", "Enable validation layers"}, &vkSetup.enableValidationLayers);
  parameterRegistry.add({"benchmark", "Enable benchmarking, prevents async loadings and turns off vsync"}, &benchmarkMode);
  parameterRegistry.add({"forcegpu", "Force the use of a specific GPU by probviding its ID"}, &vkSetup.forceGPU);
  parameterRegistry.add({"activeStereoPresenter",
                         "1=create an OpenGL quad-buffer stereo output "
                         "window for active shutter glasses"},
                        &activeStereoPresenterEnabled);
  parameterRegistry.add({"activeStereoOnlyWindow",
                         "1=hide the Vulkan preview window after the "
                         "active stereo window is presenting"},
                        &activeStereoOnlyWindow);
  parameterRegistry.add({"activeStereoGpuInterop",
                         "1=share stereo eye images directly between Vulkan and OpenGL; "
                         "0=use the CPU readback path for A/B measurements"},
                        &activeStereoGpuInteropEnabled);
  parameterRegistry.add({"stereoOutputMode", "0=auto (quad-buffer then Vulkan side-by-side), 1=quad-buffer only, 2=Vulkan side-by-side"},
                        &stereoOutputMode);
  parameterRegistry.add({"stereoStatsCsv", "Write per-frame stereo presentation CPU timings (milliseconds) to this CSV file"},
                        &stereoStatsCsv);
  parameterRegistry.add({"stereoStatsInterval", "Log a stereo frame-statistics summary every N frames (0 disables periodic reporting)"},
                        &stereoStatsInterval);

  const std::string defaultSceneFile = getDefaultScenePath().generic_string();
  std::string       sceneFile        = defaultSceneFile;
  parameterRegistry.add({"scene", "Path to a .ply, .spz, .csgs, or legacy .vkgs file to load on startup"}, &sceneFile);
  std::string dynamicSequenceFile = "";
  parameterRegistry.add({"dynamicSequence", "Path to a .dgs dynamic 4DGS keyframe pack to play over the loaded scene"},
                        &dynamicSequenceFile);
  std::string trainingCameraMetadata = "";
  parameterRegistry.add({"trainingCameraMetadata", "Dynamic3DGaussians train_meta.json used for the initial camera view"},
                        &trainingCameraMetadata);
  // Camera 8 is the frontal basketball training view; users can select any of
  // the 27 calibrated cameras from the control center.
  int32_t trainingCameraIndex = 8;
  parameterRegistry.add({"trainingCameraIndex", "Camera index from the Dynamic3DGaussians training metadata"},
                        &trainingCameraIndex);

  registerCommandLineParameters(&parameterRegistry);

  /////////////////////////////////
  // Create elements of the application, including the core of the sample
  // (gaussianSplatting)

  // The GaussianSplattingApp includes the core GaussianSplatting class by
  // inheritance
  auto gaussianSplatting = std::make_shared<GaussianSplattingApp>(&profilerManager, &parameterRegistry, &benchmarkMode);

  // add a few more parameters to registry and parser to handle sequencer
  // settings
  sequencerInfo.registerScriptParameters(parameterRegistry, parameterParser);

  // extends reporting output with memory consumption information
  sequencerInfo.postCallbacks.emplace_back(
      [&](const nvutils::ParameterSequencer::State& /* unused */) { gaussianSplatting->benchmarkAdvance(); });

  // After the creation of the elements we have more parameters in the registry
  // than before (from gaussianSplatting). Therefore add the entire registry to
  // the commandline parser again, to add new ones.
  parameterParser.add(parameterRegistry);
  // commandline parsing
  parameterParser.parse(argc, argv);
  if(stereoOutputMode < 0 || stereoOutputMode > 2)
  {
    LOGW("Unknown stereo output mode %d; using automatic output selection.\n", stereoOutputMode);
    stereoOutputMode = 0;
  }
  StereoFrameStats::setReportInterval(stereoStatsInterval);
  if(!stereoStatsCsv.empty())
    StereoFrameStats::openCsv(stereoStatsCsv.c_str());
  if(!dynamicSequenceFile.empty())
    gaussianSplatting->setDynamicSequencePath(dynamicSequenceFile);

  std::filesystem::path selectedScene = sceneFile == defaultSceneFile ? resolveDefaultScenePath(sceneFile)
                                                                       : std::filesystem::path(sceneFile);
  std::string sceneStem = selectedScene.stem().string();
  constexpr std::string_view canonicalSuffix = "_canonical";
  if(sceneStem.size() >= canonicalSuffix.size()
     && sceneStem.compare(sceneStem.size() - canonicalSuffix.size(), canonicalSuffix.size(), canonicalSuffix) == 0)
    sceneStem.resize(sceneStem.size() - canonicalSuffix.size());

  if(dynamicSequenceFile.empty() && !sceneStem.empty())
  {
    const auto pairedSequence = selectedScene.parent_path() / (sceneStem + ".dgs");
    if(std::filesystem::exists(pairedSequence))
    {
      gaussianSplatting->setDynamicSequencePath(pairedSequence.string());
      LOGI("Using the matching dynamic 4DGS sequence: %s\n", pairedSequence.string().c_str());
    }
  }
  if(trainingCameraMetadata.empty() && !sceneStem.empty())
  {
    const auto cameraMetadata = selectedScene.parent_path() / "training_cameras" / sceneStem / "train_meta.json";
    if(std::filesystem::exists(cameraMetadata))
      trainingCameraMetadata = cameraMetadata.string();
  }
  if(!trainingCameraMetadata.empty())
  {
    gaussianSplatting->setTrainingCameraMetadataPath(trainingCameraMetadata,
                                                     uint32_t(std::max<int32_t>(trainingCameraIndex, 0)));
  }
  // The stereo pipeline is multiview-only; keep mode and active eye pinned,
  // but let --stereo 0 opt out entirely for flat single-view rendering.
  prmStereo.mode      = STEREO_MODE_MULTIVIEW;
  prmStereo.activeEye = STEREO_EYE_CENTER;
  if(!prmStereo.enabled)
  {
    LOGI("Stereo rendering disabled at startup; press 0 to enable stereo (one-time target rebuild).\n");
  }
  if(prmStereo.eyeSeparation > 1.0e-5F)
  {
    gQuadBufferStereoEyeSeparation = prmStereo.eyeSeparation;
  }
  else
  {
    prmStereo.eyeSeparation = gQuadBufferStereoEyeSeparation;
  }
  if(!appInfo.headless && appInfo.windowSize.x == 0 && appInfo.windowSize.y == 0)
  {
    glm::uvec2 monitorResolution{0, 0};
    if(getPrimaryMonitorResolution(monitorResolution))
    {
      appInfo.windowSize = monitorResolution;
      LOGI(
          "Window size not specified; using primary monitor resolution %ux%u "
          "for quad-buffer fullscreen stereo.\n",
          appInfo.windowSize.x, appInfo.windowSize.y);
    }
    else
    {
      LOGW(
          "Window size not specified and primary monitor resolution was "
          "unavailable; falling back to nvapp default size.\n");
    }
  }

  // backup the default applications parameters, including those modified by
  // command line
  storeDefaultParameters();
  // set more verbose for benchmark usage later on
  parameterParser.setVerbose(true);

  LOGI(
      "Curved screen warp: %s, width=%.3f, radius=%.3f, bottomHeight=%.3f, "
      "sectionLength=(%.3f, %.3f, %.3f)\n",
      prmCurvedScreenWarp.enabled ? "enabled" : "disabled", prmCurvedScreenWarp.width, prmCurvedScreenWarp.radius,
      prmCurvedScreenWarp.bottomHeight, prmCurvedScreenWarp.sectionLength.x, prmCurvedScreenWarp.sectionLength.y,
      prmCurvedScreenWarp.sectionLength.z);
  LOGI("Stereo: %s, mode=%d, eyeSeparation=%.3fm, frameSequentialFallback=%s\n", prmStereo.enabled ? "enabled" : "disabled",
       prmStereo.mode, prmStereo.eyeSeparation, prmStereo.frameSequentialFallback ? "enabled" : "disabled");
  if(prmStereo.enabled && prmStereo.mode == STEREO_MODE_MULTIVIEW)
  {
    LOGI(
        "Stereo multiview enabled: rendering left/right eyes into a two-layer "
        "Vulkan render target.\n");
  }

  if(!sceneFile.empty())
  {
    const std::filesystem::path startupSceneFile =
        sceneFile == defaultSceneFile ? resolveDefaultScenePath(sceneFile) : std::filesystem::path(sceneFile);
    gaussianSplatting->onFileDrop(startupSceneFile);
  }

  // this element requires sequencerInfo that is potentially updated by
  // parameterParser
  auto elemSequencer = std::make_shared<nvapp::ElementSequencer>(sequencerInfo);

  /////////////////////////////////
  // Vulkan creation context information
  vkSetup.enableAllFeatures = true;

  // - Instance extensions
  vkSetup.instanceExtensions.emplace_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

  // - Device extensions
  static VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR baryFeaturesKHR = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR};
  static VkPhysicalDeviceMeshShaderFeaturesEXT meshFeaturesEXT = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT,
  };

  static VkPhysicalDeviceFragmentShadingRateFeaturesKHR fragFeaturesKHR = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR,
  };
  vkSetup.deviceExtensions.emplace_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);  // for vk_radix_sort (vrdx)
  vkSetup.deviceExtensions.emplace_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  vkSetup.deviceExtensions.emplace_back(VK_EXT_MESH_SHADER_EXTENSION_NAME, &meshFeaturesEXT, false);
  vkSetup.deviceExtensions.emplace_back(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME, &fragFeaturesKHR, true);
  vkSetup.deviceExtensions.emplace_back(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME, &baryFeaturesKHR, false);
  vkSetup.deviceExtensions.emplace_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);  // for ImGui
#if defined(_WIN32)
  // Optional: keeps WGL quad-buffer presentation while allowing Vulkan images
  // and binary semaphores to be imported directly by OpenGL.
  vkSetup.deviceExtensions.emplace_back(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, nullptr, false);
  vkSetup.deviceExtensions.emplace_back(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME, nullptr, false);
#endif

  // Dynamic depth/stencil state (vkCmdSetDepthTestEnable,
  // vkCmdSetDepthCompareOp, etc.) is core and mandatory since Vulkan 1.3
  // (promoted from VK_EXT_extended_dynamic_state / _2), so no extension is
  // requested here; the 1.4 baseline guarantees it.

  // Ray tracing extensions: all three are requested as optional. The
  // composite isSupported.raytracing flag (hardware_support.cpp) is true only
  // when the trio is actually enabled on the device, and every RTX code path
  // (shader compilation, pipeline creation, AS builds, UI entries) is gated
  // by it.
  VkPhysicalDeviceAccelerationStructureFeaturesKHR accelFeature = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
  vkSetup.deviceExtensions.emplace_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, &accelFeature,
                                        false);  // To build acceleration structures
  VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtPipelineFeature = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
  vkSetup.deviceExtensions.emplace_back(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME, &rtPipelineFeature,
                                        false);  // To use vkCmdTraceRaysKHR
  vkSetup.deviceExtensions.emplace_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME, nullptr,
                                        false);  // Required by ray tracing pipeline
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

  // Sphere primitives for ray tracing acceleration structures (optional NV
  // extension)
  static VkPhysicalDeviceRayTracingLinearSweptSpheresFeaturesNV lssFeaturesNV = {
      .sType   = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_LINEAR_SWEPT_SPHERES_FEATURES_NV,
      .spheres = VK_TRUE,
  };
  vkSetup.deviceExtensions.emplace_back(VK_NV_RAY_TRACING_LINEAR_SWEPT_SPHERES_EXTENSION_NAME, &lssFeaturesNV, false);

  // Memory budget extension for querying VRAM usage
  vkSetup.deviceExtensions.emplace_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);

  // Fragment shader interlock for order-independent transparency (FTB
  // rendering)
  static VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT interlockFeatures = {
      .sType                        = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT,
      .fragmentShaderPixelInterlock = VK_TRUE,
  };
  vkSetup.deviceExtensions.emplace_back(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME, &interlockFeatures, true);

  // Note: smooth line rasterization is a Vulkan 1.4 core feature (enabled via
  // enableAllFeatures).

  if(!appInfo.headless)
  {
    nvvk::addSurfaceExtensions(vkSetup.instanceExtensions);
    vkSetup.deviceExtensions.emplace_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  }

#if USE_DLSS
  // Adding the DLSS extensions to the instance
  static std::vector<VkExtensionProperties> extraInstanceExtensions;
  DlssRayReconstruction::getRequiredInstanceExtensions({}, extraInstanceExtensions);
  for(auto& ext : extraInstanceExtensions)
  {
    vkSetup.instanceExtensions.emplace_back(ext.extensionName);
  }

  // Adding the extra device extensions required by DLSS (Using callback)
  static std::vector<VkExtensionProperties> extraDeviceExtensions;
  vkSetup.postSelectPhysicalDeviceCallback = [](VkInstance instance, VkPhysicalDevice physicalDevice, nvvk::ContextInitInfo& vkSetup) {
    DlssRayReconstruction::getRequiredDeviceExtensions({}, instance, physicalDevice, extraDeviceExtensions);
    for(auto& ext : extraDeviceExtensions)
    {
      vkSetup.deviceExtensions.push_back({.extensionName = ext.extensionName, .specVersion = ext.specVersion});
    }
    return true;
  };
#endif

  // Setting up the validation layers
  nvvk::ValidationSettings vvlInfo{};
  vvlInfo.setPreset(nvvk::ValidationSettings::LayerPresets::eStandard);
  // vvlInfo.setPreset(nvvk::ValidationSettings::LayerPresets::eSynchronization);
  //  Disable expensive shader validation during pipeline creation
  vvlInfo.check_shaders         = VK_FALSE;
  vvlInfo.check_shaders_caching = VK_FALSE;

  // Adding the validation layer settings
  vkSetup.instanceCreateInfoExt = vvlInfo.buildPNextChain();

  // Create Vulkan context. Vulkan 1.4 is required: maintenance5 (buffer usage
  // flags2, inline SPIR-V pipeline modules), vertex attribute divisor and
  // smooth line rasterization are used as core 1.4 features.
  vkSetup.apiVersion = VK_API_VERSION_1_4;
  if(vkContext.init(vkSetup) != VK_SUCCESS)
  {
    LOGE("Error in Vulkan context creation (Vulkan 1.4 is required).\n");
    return 1;
  }

  // Populate the global hardware capability registry from the freshly created
  // context. After this point, code can branch on isSupported.* (see
  // src/hardware_support.h). DLSS runtime availability is filled in later
  // from GaussianSplatting once NGX has probed the device.
  isSupported.initFromVulkanContext(vkContext);

  /////////////////////////////////
  // Application setup
  appInfo.name                  = TARGET_NAME;
  appInfo.instance              = vkContext.getInstance();
  appInfo.device                = vkContext.getDevice();
  appInfo.physicalDevice        = vkContext.getPhysicalDevice();
  appInfo.queues                = vkContext.getQueueInfos();
  appInfo.hasUndockableViewport = false;
  appInfo.useMenu               = false;  // no UI
  if(appInfo.headless && benchmarkMode)
  {
    appInfo.headlessFrameCount = UINT32_MAX;  // Let the sequencer's close() terminate the loop
  }

  // Initializes the application
  application.init(appInfo);

  // Add all application elements including our sample specific
  // gaussianSplatting onAttach will be invoked on elements at this stage
  application.addElement(elemSequencer);
  application.addElement(gaussianSplatting);
  // Registered for every windowed run: stereo starts toggle instantly via the
  // eye separation; mono starts enable stereo through a one-time rebuild.
  if(!appInfo.headless)
    application.addElement(std::make_shared<StereoHotkeysElement>(gaussianSplatting));
  application.addElement(std::make_shared<ExitHotkeysElement>());

  auto elemCamera = std::make_shared<curved_stereo_gs::ElementCameraCustom>();
  elemCamera->setCameraManipulator(gaussianSplatting->cameraManip);
  // Disable camera updates when dragging comparison slider or transform gizmo
  elemCamera->setDisableCallback([gaussianSplatting]() {
    return gaussianSplatting->isDraggingComparisonSlider() || gaussianSplatting->isDraggingTransformHelper()
           || gaussianSplatting->isDraggingCursorTarget();
  });
  application.addElement(elemCamera);

  std::shared_ptr<ActiveStereoQuadBufferPresenter> activeStereoPresenter;
  std::shared_ptr<StereoInteropVk>                 stereoInterop;
  auto sideBySideOutput = std::make_shared<SideBySideOutput>();
  sideBySideOutput->enabled = stereoOutputMode == 2;
  if(activeStereoPresenterEnabled && stereoOutputMode != 2 && !appInfo.headless)
  {
    application.setRenderWhileMinimized(prmStereo.enabled && activeStereoOnlyWindow);
    if(prmStereo.enabled && activeStereoOnlyWindow)
      application.setVsync(false);  // WGL SwapBuffers owns display pacing; avoid
                                    // a second hidden-window VSync.

    activeStereoPresenter = std::make_shared<ActiveStereoQuadBufferPresenter>();
    stereoInterop         = std::make_shared<StereoInteropVk>();
    if(activeStereoGpuInteropEnabled)
      stereoInterop->init(appInfo.physicalDevice, appInfo.device, application.getQueue(0).familyIndex);
    activeStereoPresenter->setCameraManipulator(gaussianSplatting->cameraManip);
    activeStereoPresenter->setCloseCallback([&application]() { application.close(); });
    auto mainWindowHidden = std::make_shared<bool>(false);
    activeStereoPresenter->setStereoToggleCallback([gaussianSplatting, &application, activeStereoOnlyWindow, mainWindowHidden]() {
      requestStereoToggle(gaussianSplatting);
      if(!prmStereo.enabled && activeStereoOnlyWindow && application.getWindowHandle() != nullptr)
      {
        glfwShowWindow(application.getWindowHandle());
        *mainWindowHidden = false;
      }
    });
    application.setFrameGenerationCallback(
        [gaussianSplatting, activeStereoPresenter, stereoInterop, sideBySideOutput,
         &application, physicalDevice = appInfo.physicalDevice](VkCommandBuffer cmd, VkImage swapchainImage, VkImageView,
                                                                VkFormat swapchainFormat, VkExtent2D swapchainExtent,
                                                                uint64_t frameNumber) {
          if(!prmStereo.enabled || prmStereo.mode != STEREO_MODE_MULTIVIEW)
            return;

          const StereoPresentationSource source = gaussianSplatting->getStereoPresentationSource();
          if(sideBySideOutput->enabled)
          {
            StereoFrameStats::beginFrame(StereoFrameStats::Path::kVulkanSideBySide, frameNumber);
            recordSideBySide(physicalDevice, cmd, source, swapchainImage, swapchainFormat, swapchainExtent,
                             *sideBySideOutput);
            StereoFrameStats::endFrame(StereoFrameStats::Path::kVulkanSideBySide);
            return;
          }
          if(!stereoInterop->isAvailable() || source.image == VK_NULL_HANDLE || source.layerCount < StereoInteropVk::kEyeCount)
            return;

          if(!stereoInterop->matches(source.extent, source.format))
          {
            // Resize/calibration mode changes are rare.  Finish both APIs
            // before replacing exported objects.
            activeStereoPresenter->releaseInterop();
            if(!stereoInterop->recreate(source.extent, source.format))
              return;
          }

          if(!stereoInterop->recordFrame(cmd, source, frameNumber, application))
            stereoInterop->disable();
        });

    auto stereoPresentationWasEnabled = std::make_shared<bool>(prmStereo.enabled);
    auto stereoPresenterWasActive     = std::make_shared<bool>(false);
    application.setQueueSubmitEndCallback([gaussianSplatting, activeStereoPresenter, stereoInterop, sideBySideOutput,
                                           stereoOutputMode, activeStereoOnlyWindow, mainWindowHidden,
                                           stereoPresentationWasEnabled, stereoPresenterWasActive,
                                           originalVsync = appInfo.vSync, &application, leftRgba8 = std::vector<uint8_t>{},
                                           rightRgba8 = std::vector<uint8_t>{}, benchmarkEnabled = benchmarkMode,
                                           frameLimit = appInfo.headlessFrameCount,
                                           stereoStatsFrames = uint64_t{0}](uint64_t frameNumber) mutable {
      // Count every submitted frame regardless of the active stereo path: the
      // mode-specific branches below return early.
      if(frameLimit != 0 && frameLimit != UINT32_MAX && ++stereoStatsFrames >= frameLimit)
        application.close();
      if(sideBySideOutput->enabled)
        return;

      if(*stereoPresentationWasEnabled != prmStereo.enabled)
      {
        *stereoPresentationWasEnabled = prmStereo.enabled;
        application.setRenderWhileMinimized(prmStereo.enabled && activeStereoOnlyWindow);
        application.setVsync(prmStereo.enabled && activeStereoOnlyWindow ? false : originalVsync);
      }

      if(!prmStereo.enabled || prmStereo.mode != STEREO_MODE_MULTIVIEW)
      {
        if(*stereoPresenterWasActive)
        {
          activeStereoPresenter->deinit();
          *stereoPresenterWasActive = false;
        }
        if(*mainWindowHidden && application.getWindowHandle() != nullptr)
        {
          glfwShowWindow(application.getWindowHandle());
          *mainWindowHidden = false;
        }
        return;
      }

      bool presented = false;
      if(stereoInterop->hasProducedFrame(frameNumber))
      {
        StereoFrameStats::beginFrame(StereoFrameStats::Path::kGpuInterop, frameNumber);
        presented = activeStereoPresenter->presentInterop(*stereoInterop, frameNumber);
        StereoFrameStats::endFrame(StereoFrameStats::Path::kGpuInterop);
        if(!presented)
          stereoInterop->disable();
      }

      if(!presented && !activeStereoPresenter->hasFailed())
      {
        VkExtent2D stereoSize{0, 0};
        StereoFrameStats::beginFrame(StereoFrameStats::Path::kCpuReadback, frameNumber);
        presented = gaussianSplatting->readStereoPresentationImages(leftRgba8, rightRgba8, stereoSize)
                    && activeStereoPresenter->present(stereoSize, leftRgba8, rightRgba8);
        StereoFrameStats::endFrame(StereoFrameStats::Path::kCpuReadback);
      }

      if(presented)
      {
        *stereoPresenterWasActive = true;
        if(activeStereoOnlyWindow && !*mainWindowHidden && application.getWindowHandle() != nullptr)
        {
          glfwHideWindow(application.getWindowHandle());
          *mainWindowHidden = true;
          LOGI(
              "Active stereo presenter: Vulkan preview window hidden; "
              "active stereo window is the visible interactive output.\n");
        }
      }
      else if(stereoOutputMode == 0 && activeStereoPresenter->hasFailed())
      {
        sideBySideOutput->enabled = true;
        if(*mainWindowHidden && application.getWindowHandle() != nullptr)
        {
          glfwShowWindow(application.getWindowHandle());
          *mainWindowHidden = false;
        }
        application.setVsync(originalVsync);
        LOGW("WGL quad-buffer unavailable; falling back to Vulkan side-by-side stereo.\n");
      }
    });
    LOGI(
        "Active stereo presenter enabled: Vulkan renders left/right layers; "
        "OpenGL quad-buffer window presents GL_BACK_LEFT/GL_BACK_RIGHT%s.\n",
        activeStereoOnlyWindow ? " as the only visible window" : "");
  }
  else if(stereoOutputMode == 2 && !appInfo.headless)
  {
    application.setFrameGenerationCallback(
        [gaussianSplatting, sideBySideOutput,
         physicalDevice = appInfo.physicalDevice](VkCommandBuffer cmd, VkImage swapchainImage, VkImageView,
                                                  VkFormat swapchainFormat, VkExtent2D swapchainExtent, uint64_t frameNumber) {
          if(prmStereo.enabled && prmStereo.mode == STEREO_MODE_MULTIVIEW)
          {
            StereoFrameStats::beginFrame(StereoFrameStats::Path::kVulkanSideBySide, frameNumber);
            recordSideBySide(physicalDevice, cmd, gaussianSplatting->getStereoPresentationSource(), swapchainImage,
                             swapchainFormat, swapchainExtent, *sideBySideOutput);
            StereoFrameStats::endFrame(StereoFrameStats::Path::kVulkanSideBySide);
          }
        });
    // The generic submit-end callback is not registered in this mode; keep the
    // frame limit functional for side-by-side runs.
    application.setQueueSubmitEndCallback([benchmarkEnabled = benchmarkMode, frameLimit = appInfo.headlessFrameCount,
                                           stereoStatsFrames = uint64_t{0}, &application](uint64_t) mutable {
      if(frameLimit != 0 && frameLimit != UINT32_MAX && ++stereoStatsFrames >= frameLimit)
        application.close();
    });
    LOGI("Vulkan side-by-side stereo selected; WGL quad-buffer is not required.\n");
  }

  if(benchmarkMode)
  {
    // In this mode we do not display the GUI elements
    application.setVsync(false);
  }

  //
  application.run();

  StereoFrameStats::logSummary("final");
  StereoFrameStats::closeCsv();

  // Cleanup
  if(appInfo.device != VK_NULL_HANDLE)
    vkDeviceWaitIdle(appInfo.device);
  if(activeStereoPresenter)
    activeStereoPresenter->deinit();
  if(stereoInterop)
    stereoInterop->deinit();
  application.deinit();
  vkContext.deinit();

  return 0;
}
