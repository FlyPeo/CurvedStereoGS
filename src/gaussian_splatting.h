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

#ifndef _GAUSSIAN_SPLATTING_H_
#define _GAUSSIAN_SPLATTING_H_

#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include <vulkan/vulkan_core.h>
// mathematics
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/string_cast.hpp>
#include <glm/gtx/transform.hpp>
#include <glm/vec3.hpp>
// threading
#include <condition_variable>
#include <mutex>
#include <thread>
// GPU radix sort
#include <vk_radix_sort.h>
//
#include <nvutils/alignment.hpp>
#include <nvutils/file_operations.hpp>
#include <nvutils/logger.hpp>

#include <nvvk/acceleration_structures.hpp>
#include <nvvk/context.hpp>
#include <nvvk/debug_util.hpp>
#include <nvvk/default_structs.hpp>
#include <nvvk/descriptors.hpp>
#include <nvvk/gbuffers.hpp>
#include <nvvk/graphics_pipeline.hpp>
#include <nvvk/helpers.hpp>
#include <nvvk/physical_device.hpp>
#include <nvvk/pipeline.hpp>
#include <nvvk/profiler_vk.hpp>
#include <nvvk/resource_allocator.hpp>
#include <nvvk/resources.hpp>
#include <nvvk/sampler_pool.hpp>
#include <nvvk/sbt_generator.hpp>
#include <nvvk/staging.hpp>
#include <nvvk/validation_settings.hpp>

#include <nvslang/slang.hpp>
#include <nvvkglsl/glsl.hpp>

#include <nvapp/application.hpp>
#include <nvapp/elem_camera.hpp>
#include <nvapp/elem_default_menu.hpp>
#include <nvapp/elem_default_title.hpp>
#include <nvapp/elem_profiler.hpp>
#include <nvapp/elem_sequencer.hpp>

#include <nvgpu_monitor/elem_gpu_monitor.hpp>

// Shared between host and device
#include "shaderio.h"

#include "asset_manager_vk.h"
#include "memory_statistics.h"
#include "parameters.h"
#include "ply_loader_async.h"
#include "sky_sun_and_ibl.h"
#include "dynamic_splat_sequence.h"
#include "splat_set.h"
#include "splat_set_vk.h"
#include "splat_sorter_async.h"
#include "stereo_interop_vk.h"
#include "utilities.h"
#include "visual_helpers_vk.h"  // 3D gizmo and grid visualization

// #DLSS
#if defined(USE_DLSS)
#include "dlss_denoiser.hpp"
#endif

#include "image_compare.h"

#include <nvshaders_host/tonemapper.hpp>

namespace curved_stereo_gs {

struct BufferDumpInfo
{
  VkImage     image;
  VkFormat    format;
  VkExtent2D  size;
  std::string postfix;
};

class GaussianSplatting
{
public:
  // Benchmarking, print extended info
  // invoked by parameter sequencer
  void benchmarkAdvance();

  bool readStereoPresentationImages(std::vector<uint8_t>& leftRgba, std::vector<uint8_t>& rightRgba, VkExtent2D& size);
  bool readStereoPresentationImagesF32(std::vector<float>& leftRgba, std::vector<float>& rightRgba, VkExtent2D& size);
  StereoPresentationSource getStereoPresentationSource() const;

public:
  // Camera manipulator
  // public so that it can be accessed by main
  std::shared_ptr<nvutils::CameraManipulator> cameraManip{};

  // Dynamic 4DGS sequence (docs/dynamic-4dgs.md): opens lazily once the
  // canonical scene is on the GPU; the animate pass runs before sorting.
  void setDynamicSequencePath(const std::string& path)
  {
    m_dynamicSequencePath = path;
    m_dynamicSequenceRequested = true;
  }

  // Use the selected Dynamic3DGaussians camera as the initial view when the
  // renderer attaches. Users control the camera freely after initialization.
  void setTrainingCameraMetadataPath(const std::string& path, uint32_t cameraIndex)
  {
    m_trainingCameraMetadataPath = path;
    m_trainingCameraIndex        = cameraIndex;
    m_trainingCameraRequested    = !path.empty();
  }

  // Invalidate the cached sorted splat order (sort re-runs on the next frame).
  // Callers: splat-set loads and any event that changes splat positions outside
  // the paths that already mark the order dirty internally.
  void markSplatSortDirty() { m_sortDirty = true; }

  // Attach the radial point-cloud-to-Gaussian reveal to a newly loaded asset.
  void startSplatReveal(const std::shared_ptr<SplatSetVk>& splatSet);

protected:
  GaussianSplatting(nvutils::ProfilerManager* profilerManager, nvutils::ParameterRegistry* parameterRegistry);

  ~GaussianSplatting();

  void onAttach(nvapp::Application* app);

  void onDetach();

  void onResize(VkCommandBuffer cmd, const VkExtent2D& size);

  void onPreRender();

  // reset frame counter for temporal accumulated multi-sampling
  // will cause a restart of the frame construction
  // When DLSS is enabled, frameSampleId must never reset — DLSS handles
  // temporal history via motion vectors and needs a continuously incrementing
  // frame index.
  inline void resetFrameCounter()
  {
#if defined(USE_DLSS)
    if(m_dlss.isEnabled())
      return;
#endif
    prmFrame.frameSampleId = -1;
  }

  void onRender(VkCommandBuffer cmd);

  // reset the rendering settings that can
  // be modified by the user interface
  inline void resetRenderSettings()
  {
    resetFrameParameters();
    resetRenderParameters();
    resetCurvedScreenWarpParameters();
    resetStereoParameters();
    resetRasterParameters();
    resetRtxParameters();
  }

  // Check if current pipeline uses ray tracing (RTX, Hybrid, or Hybrid 3DGUT)
  inline bool isRtxPipelineActive() const
  {
    return (prmSelectedPipeline == PIPELINE_RTX || prmSelectedPipeline == PIPELINE_HYBRID || prmSelectedPipeline == PIPELINE_HYBRID_3DGUT);
  }

  // Check if current pipeline is pure ray tracing (no rasterization)
  inline bool isRtxPipelineOnly() const { return prmSelectedPipeline == PIPELINE_RTX; }

  // Check if current pipeline uses rasterization (Vert, Mesh, Mesh 3DGUT, or
  // Hybrid variants)
  inline bool isRasterPipelineActive() const
  {
    return (prmSelectedPipeline == PIPELINE_VERT || prmSelectedPipeline == PIPELINE_MESH || prmSelectedPipeline == PIPELINE_MESH_3DGUT
            || prmSelectedPipeline == PIPELINE_HYBRID || prmSelectedPipeline == PIPELINE_HYBRID_3DGUT);
  }

  // Check if current pipeline is pure rasterization (no ray tracing)
  inline bool isRasterPipelineOnly() const
  {
    return (prmSelectedPipeline == PIPELINE_VERT || prmSelectedPipeline == PIPELINE_MESH || prmSelectedPipeline == PIPELINE_MESH_3DGUT);
  }

  // Check if a pipeline relies on VK_EXT_mesh_shader (raster MESH, MESH_3DGUT
  // and the hybrid raster paths all dispatch via vkCmdDrawMeshTasksEXT).
  static inline bool isMeshShaderPipeline(uint32_t pipeline)
  {
    return pipeline == PIPELINE_MESH || pipeline == PIPELINE_HYBRID || pipeline == PIPELINE_MESH_3DGUT || pipeline == PIPELINE_HYBRID_3DGUT;
  }

  // True when the distance compute (+ optional VRDX radix) GPU path is active
  // this frame.
  inline bool usesGpuDistSort() const
  {
    return (prmRaster.sortingMethod == SORTING_GPU_SYNC_RADIX) || (prmRaster.sortingMethod == SORTING_STOCHASTIC_SPLAT);
  }

  // Check if DLSS is supported in current pipeline (RTX and hybrid pipelines)
  inline bool isDlssSupportedPipeline() const
  {
    return (prmSelectedPipeline == PIPELINE_RTX || prmSelectedPipeline == PIPELINE_HYBRID || prmSelectedPipeline == PIPELINE_HYBRID_3DGUT);
  }

  // Check if auto-focus is supported in current pipeline
  // Requires ray tracing for distance feedback (pure 3DGRT or hybrid
  // 3DGUT+3DGRT)
  inline bool supportsAutoFocus() const
  {
    return (prmSelectedPipeline == PIPELINE_RTX || prmSelectedPipeline == PIPELINE_HYBRID_3DGUT);
  }

  // Check if surface info (depth, normal, splat ID) is needed by any feature
  // Must match the NEED_SURFACE_INFO shader macro computation in
  // updateSlangMacros()
  inline bool needSurfaceInfo()
  {
    bool need = prmRender.lightingEnabled || (m_assets.cameras.getCamera().dofMode != DOF_DISABLED);
#if defined(USE_DLSS)
    need = need || m_dlss.isEnabled();
#endif
    const int v = prmRender.visualize;
    need        = need || (v >= VISUALIZE_DEPTH && v <= VISUALIZE_DEPTH_FOR_DLSS)
           || (v >= VISUALIZE_NORMAL && v <= VISUALIZE_NORMAL_FOR_DLSS) || (v == VISUALIZE_SPLAT_ID);
    return need;
  }

  // Reset all scene and rendering related resources (for scene/project reset)
  // NOTE: vkDeviceWaitIdle shall be invoked before calling this method
  virtual void reset();

  // free scene (splat set) from RAM
  void deinitScene();

private:
  // Utility: create a graphics pipeline and log/null-out the handle on failure.
  // Returns true on success.
  static bool createGraphicsPipelineChecked(VkDevice                           device,
                                            VkPipelineCache                    cache,
                                            nvvk::GraphicsPipelineCreator&     creator,
                                            const nvvk::GraphicsPipelineState& state,
                                            VkPipeline*                        pipeline,
                                            const char*                        debugName);

  // init the raster pipelines
  void initPipelines();

  // Update BINDING_SPLAT_TEXTURES in the descriptor set after textures are
  // (re)created. Called after processVramUpdates() to ensure descriptor set
  // matches current GPU textures.
  void updateSplatTextureDescriptors();

  // Update BINDING_MESH_TEXTURES in the descriptor set after mesh textures are
  // loaded or freed.
  void updateMeshTextureDescriptors();

  // deinit raster and rtx pipelines TODO move rtx in separate method
  void deinitPipelines();

  void initRendererBuffers();

  void deinitRendererBuffers();

  void initHelperPass();

  void deinitHelperPass();

  void updateSlangMacros(void);

  bool compileSlangShader(const std::string& filename, VkShaderModule& module);
  // Disk cache for compiled SPIR-V: key = digest of all shader sources
  // (covers every #include-able header in the shader dirs) + filename + macro
  // set, so any source or mode change invalidates automatically. Cuts the
  // per-launch Slang compilation (the startup white screen) down to file reads.
  bool        tryLoadCachedSpirv(const std::string& filename, std::vector<uint32_t>& spirv);
  void        storeCachedSpirv(const std::string& filename, const uint32_t* code, size_t byteSize);
  void        ensureShaderSourceDigest();
  std::string m_shaderCacheDigest;  // digest of all shader sources, computed lazily once
  bool        m_shaderCacheDigestReady{false};
  uint32_t    m_shaderCacheHits{0};
  uint32_t    m_shaderCacheMisses{0};
  // Set when onDetach started: processUpdateRequests() must then drop the
  // pending shader-recompile request instead of rebuilding every pipeline for
  // an application that is exiting anyway (that path wasted seconds at exit
  // and crashed when combined with the SPIR-V cache).
  bool m_shuttingDown{false};

  bool initShaders(void);

  void deinitShaders(void);

  /////////////
  // Rendering submethods

  void renderPureRaytracingPipeline(VkCommandBuffer cmd, uint32_t splatCount, bool temporalConverged);

  void renderHybridPipeline(VkCommandBuffer cmd, uint32_t splatCount, bool temporalConverged);

  void renderVisualHelpers(VkCommandBuffer cmd);

  // Output image getters (returns helper buffer if rendered, else COLOR_MAIN)
  VkImage         getOutputColorImage() const;
  VkImageView     getOutputColorImageView() const;
  VkDescriptorSet getOutputDescriptorSet() const;

  // process eventual update requests comming from UI or benchmark
  // that requires to be performed before a new rendering after a DeviceWaitIdle
  // forceAll: if true, process all requests including RTX even if not in RTX
  // pipeline (for cleanup on reset/exit)
  void processUpdateRequests(bool forceAll = false);

  // Release GPU buffers not needed by the current pipeline to reclaim VRAM:
  //   Pure RTX:    release sorting buffers (no raster pass)
  //   Pure raster: release RTX acceleration structures (no ray tracing)
  //   Hybrid:      keep both
  // Buffers are reallocated on demand when switching to a pipeline that needs
  // them.
  void releaseUnusedBuffers();

  // Process GBuffer reinitialization requests (format changes)
  // Called from onPreRender before the frame command buffer is recorded
  void processGBufferUpdateRequests();

  // Updates frame information uniform buffer and frame camera info
  void updateAndUploadFrameInfoUBO(VkCommandBuffer cmd, const uint32_t splatCount);
  bool isStereoMultiviewEnabled() const;
  bool usesStereoMultiviewRenderTargets() const;

  void processSortingOnGPU(VkCommandBuffer cmd, const uint32_t splatCount);

  void coerceRasterParamsForSortingMethod();

  void drawSplatPrimitives(VkCommandBuffer cmd, const uint32_t splatCount);

  void drawMeshPrimitives(VkCommandBuffer cmd, bool ftbColorPass = false);

  // Helper to check if mesh rendering pipeline should be active (meshes OR
  // lights exist)
  inline bool shouldUseMeshPipeline() const { return !m_assets.meshes.instances.empty(); }

  // for statistics display in the UI
  // copy form m_indirectReadbackHost updated at previous frame to
  // m_indirectReadback
  void collectReadBackValuesIfNeeded(void);
  // for statistics display in the UI
  // read back updated indirect parameters from m_indirect into
  // m_indirectReadbackHost
  void readBackIndirectParametersIfNeeded(VkCommandBuffer cmd);

  void updateRasterizationMemoryStatistics(const uint32_t splatCount);

  //////////////
  // RTX specific

  // updates the frame counter and returns true if a new raytracing pass is
  // needed
  bool updateFrameCounter();

  void initRtDescriptorSet();
  void updateRtDescriptorSet();
  void initRtPipeline();
  void raytrace(const VkCommandBuffer& cmdBuf, bool meshDepthOnly = false);

  //////////////
  // Post processing

  void initDescriptorSetPostProcessing();
  void updateDescriptorSetPostProcessing();
  void initPipelinePostProcessing();
  void postProcess(VkCommandBuffer cmd);

protected:
  int getPayloadArraySize() const;

  // Number of mesh-task workgroups needed to rasterize splatCount splats (one
  // workgroup per RASTER_MESH_WORKGROUP_SIZE =
  // prmRaster.meshShaderWorkgroupSize splats).
  uint32_t getMeshTaskWorkgroupCount(uint32_t splatCount) const;

  // 2D mesh-task dispatch grid {groupCountX, groupCountY} covering splatCount
  // splats. The grid width is the device maxMeshWorkGroupCount[0]
  // (HardwareSupport::maxMeshWorkGroupCountX); the raster mesh shaders
  // linearize the grid id (groupID.y * width + groupID.x), so a single-row grid
  // (y == 1) is identical to a plain 1D dispatch.
  glm::uvec2 getMeshTaskDispatchGrid(uint32_t splatCount) const;

  // True when splatCount needs more mesh-task workgroups than the device can
  // dispatch, i.e. the 2D grid would exceed the Y per-dimension limit or the
  // X*Y total-count limit. Beyond this point the scene cannot be fully
  // rasterized via the mesh pipeline (a hardware ceiling, not a grid-shape
  // issue); the UI flags it so the user knows splats are being dropped.
  bool isMeshTaskDispatchOverflow(uint32_t splatCount) const;

  // path of the splat set file currently being loaded (transient, used across
  // async load frames)
  std::filesystem::path m_currentLoadingSplatSetFilename;

  // path of the currently loaded/saved .csgs project (empty = untitled)
  std::filesystem::path m_projectPath;

  // Current load request being processed (for queue-based loading)
  SceneLoadRequest m_currentLoadRequest;

  // scene loader
  PlyLoaderAsync m_plyLoader;

  // Centralized asset management
  AssetManagerVk m_assets = {};

  // Index of the camera preset selected (-1 = active camera, >= 0 = preset
  // index)
  int64_t m_selectedCameraPresetIndex = -1;
  // Track selected splat set instance (for UI selection)
  std::shared_ptr<SplatSetInstanceVk> m_selectedSplatInstance = nullptr;
  // Selected mesh instance (nullptr if none selected)
  std::shared_ptr<MeshInstanceVk> m_selectedMeshInstance = nullptr;
  // Selected light instance (nullptr if none selected)
  std::shared_ptr<LightSourceInstanceVk> m_selectedLightInstance = nullptr;
  // Index of the last camera loaded
  uint64_t m_lastLoadedCamera = 0;

  // Push constant for rasterizer
  shaderio::PushConstant m_pcRaster{};

  // counting benchmark steps
  int m_benchmarkId = 0;

  // Trigger a rebuild of the shaders and pipelines at next frame
  bool m_requestUpdateShaders = false;
  // Defer shader rebuild until camera animation completes
  bool m_requestUpdateShadersAfterCameraAnim = false;
  // Track scene composition for RTX_HAS_MESHES / RTX_HAS_PARTICLES macro
  // changes
  bool m_lastHadMeshes    = false;
  bool m_lastHadParticles = false;
  // Pipeline-specific optimization: defer RTX AS rebuild when in raster mode
  // When transforms are modified in raster mode, we don't rebuild RTX
  // structures immediately. This flag tracks that RTX needs rebuilding, and
  // will trigger rebuild when switching to RTX pipeline. This avoids expensive
  // RTX structure updates when not actively using ray tracing.
  bool m_deferredRtxRebuildPending = false;
  // trigger update of assets buffer (bindless SceneAssets structure)
  // Set when underlying buffers change (mesh descriptors, splat descriptors,
  // lights, etc.)
  bool m_requestUpdateAssetsBuffer = false;
  // GBuffer reinitialization (for color format changes)
  bool m_requestGBufferReinit     = false;  // Set by UI, deferred to avoid ImGui stale descriptor
  bool m_pendingGBufferReinitSeen = false;  // Two-pass deferral (onUIRender runs before onPreRender)
  bool m_activeStereoMultiview    = false;  // Resource/pipeline mode currently safe to render with

  // Steady-state frame timing, collected for benchmark comparisons and logged
  // at detach (startup/shader-compilation time is excluded by construction).
  std::chrono::steady_clock::time_point m_frameStatLast{};
  std::vector<float>                    m_frameStatMs{};

  // Sorted-splat-order cache validity (see renderHybridPipeline): the sort key
  // derives from frameInfo.viewMatrix + projection, so the order only changes
  // when the camera moves, splat positions change (dynamic sequence, loads,
  // gizmo drags), or opacity culling toggles.
  bool      m_sortDirty{true};
  std::chrono::steady_clock::time_point m_splatRevealLastClockUpdate{std::chrono::steady_clock::now()};
  float     m_splatRevealClock{0.0F};
  float     m_splatRevealEndTime{0.0F};
  glm::mat4 m_lastSortViewMatrix{0.0F};
  double    m_lastSortFov{-1.0};
  bool      m_lastSortOpacityDisabled{false};

  // Last dispatched dynamic-sequence blend parameters: an identical blend
  // produces identical buffer contents, so the dispatch (and the sort
  // invalidation it triggers) can be skipped.
  uint32_t m_lastAnimateCount{0};
  uint32_t m_lastAnimateKey0{0};
  uint32_t m_lastAnimateKey1{0};
  float    m_lastAnimateAlpha{-1.0F};

  nvapp::Application*         m_app{nullptr};
  nvutils::ProfilerManager*   m_profilerManager;
  nvutils::ParameterRegistry* m_parameterRegistry;
  nvvk::StagingUploader       m_uploader{};     // utility to upload buffers to device
  nvvk::SamplerPool           m_samplerPool{};  // The sampler pool, used to create texture samplers
  VkSampler                   m_sampler{};      // texture sampler (nearest)
  nvvk::ResourceAllocator     m_alloc;
  nvvk::PhysicalDeviceInfo    m_physicalDeviceInfo;

  nvutils::ProfilerTimeline* m_profilerTimeline{};
  nvvk::ProfilerGpuTimer     m_profilerGpuTimer;

  glm::vec2         m_viewSize      = {0, 0};
  VkFormat          m_depthFormat   = VK_FORMAT_UNDEFINED;       // Depth format of the depth buffer
  VkClearColorValue m_clearColor    = {0.0F, 0.0F, 0.0F, 0.0F};  // Clear color
  VkDevice          m_device        = VK_NULL_HANDLE;            // Convenient shortcut to device
  VkPipelineCache   m_pipelineCache = VK_NULL_HANDLE;            // In-memory cache for pipeline compilation

  // Convenient enum to dereference color buffers in GBuffers
  enum
  {
    COLOR_MAIN              = 0,  // Main output / temporal accumulation buffer
    COLOR_AUX1              = 1,  // Temporal sampling intermediate buffer
    COLOR_COMPARISON_OUTPUT = 2,  // Comparison mode composite output
    COLOR_RASTER_NORMAL     = 3,  // Rasterization: integrated normals (RGB16F)
    COLOR_RASTER_DEPTH      = 4,  // Rasterization: picked depth (R) + transmittance (G) for FTB
    COLOR_RASTER_SPLATID    = 5,  // Rasterization: global splat ID (R32_UINT)
    COLOR_LDR               = 6,  // Tone-mapped LDR output, or high precision stereo
                                  // presentation buffer
  };
  // G-Buffers: 9 color buffers + 1 depth buffer
  nvvk::GBuffer m_gBuffers;
  VkFormat      m_normalFormat = VK_FORMAT_R16G16B16A16_SFLOAT;  // Normal buffer format (RGB16F + alpha for
                                                                 // roughness)
  VkFormat m_rasterDepthFormat = VK_FORMAT_R32G32_SFLOAT;        // Raster depth buffer format (R=depth,
                                                                 // G=transmittance for FTB)
  VkFormat m_splatIdFormat = VK_FORMAT_R32_UINT;                 // Splat ID buffer format (single uint32)

  // camera info for current frame, updated by onRender
  glm::vec3 m_eye{};
  glm::vec3 m_center{};
  glm::vec3 m_up{};

  // IndirectParams structure defined in shaderio.h
  nvvk::Buffer             m_indirect;                    // indirect parameter buffer
  nvvk::Buffer             m_indirectReadbackHost;        // buffer for readback
  shaderio::IndirectParams m_indirectReadback;            // readback values
  bool                     m_canCollectReadback = false;  // tells wether readback will be available
                                                          // in Host buffer at next frame

  nvvk::Buffer m_quadVertices;                // Buffer of vertices for the splat quad
  nvvk::Buffer m_quadIndices;                 // Buffer of indices for the splat quad
  nvvk::Buffer m_stereoPresentationReadback;  // Left/right readback for active
                                              // stereo presentation
  VkExtent2D m_stereoPresentationReadbackSize{0, 0};
  VkFormat   m_stereoPresentationReadbackFormat{VK_FORMAT_UNDEFINED};

  // macro definitions shared by all shaders
  std::vector<std::pair<std::string, std::string>> m_shaderMacros;
  // used to load and compile shaders
  nvslang::SlangCompiler m_slangCompiler{};

  // The different shaders that are used in the pipelines
  struct Shaders
  {
    // 3D Gaussians Raster
    VkShaderModule distShader{};
    VkShaderModule meshShader{};
    VkShaderModule vertexShader{};
    VkShaderModule fragmentShader{};
    VkShaderModule animateShader{};  // dynamic 4DGS animate pass (docs/dynamic-4dgs.md)
    VkShaderModule threedgutMeshShader{};
    VkShaderModule threedgutFragmentShader{};
    // 3D Meshes raster
    VkShaderModule meshVertexShader{};
    VkShaderModule meshFragmentShader{};
    // for RTX
    VkShaderModule rtxRgenShader{};           // The ray generator
    VkShaderModule rtxRmissShader{};          // The miss shader
    VkShaderModule rtxRmiss2Shader{};         // For shadows (no support yet)
    VkShaderModule rtxRchitShader{};          // Closest Hit (for meshes)
    VkShaderModule rtxRahitShader{};          // Any Hit
    VkShaderModule rtxRintShader{};           // Intersection
    VkShaderModule rtxRintBillboardShader{};  // Intersection (billboard variant)
    // Post processings
    VkShaderModule postComputeShader{};
    // Deferred shading
    VkShaderModule deferredShadingShader{};
    // Particle AS build compute
    VkShaderModule particleAsBuildShader{};
    // Depth consolidation (FTB: write picked splat depth to hw depth buffer)
    VkShaderModule depthConsolidateVertShader{};
    VkShaderModule depthConsolidateFragShader{};
    // Utility storage to process shaders in loop
    std::vector<VkShaderModule*> modules{};
    // true if all the shaders are succesfully build
    bool valid = false;
  } m_shaders;

  // 3D Gaussians Pipelines
  VkPipeline m_computePipelineGsDistCull = VK_NULL_HANDLE;  // The compute pipeline to compute gaussian splats
                                                            // distances to eye and cull
  VkPipeline m_computePipelineParticleAs = VK_NULL_HANDLE;  // Compute pipeline to generate particle AS buffers
  VkPipeline m_graphicsPipelineGsVert    = VK_NULL_HANDLE;  // The graphic pipeline to rasterize gaussian splats using
                                                            // vertex shaders
  VkPipeline m_graphicsPipelineGsMesh = VK_NULL_HANDLE;     // The graphic pipeline to rasterize gaussian splats using
                                                            // mesh shaders
  VkPipeline m_graphicsPipeline3dgutMesh = VK_NULL_HANDLE;  // The graphic pipeline to rasterize 3DGUT splats using
                                                            // mesh shaders
  // 3D Meshes Pipelines
  VkPipeline m_graphicsPipelineMesh             = VK_NULL_HANDLE;  // The graphic pipeline to rasterize meshes
  VkPipeline m_graphicsPipelineMeshFtbColor     = VK_NULL_HANDLE;  // FTB: mesh color pass with additive blend
  VkPipeline m_graphicsPipelineDepthConsolidate = VK_NULL_HANDLE;  // FTB: consolidate picked depth to hw depth

  // Common to 3D meshes and 3D Gaussians pipeline
  VkPipelineLayout         m_pipelineLayout           = VK_NULL_HANDLE;  // Raster Pipelines layout
  VkPipelineLayout         m_particleAsPipelineLayout = VK_NULL_HANDLE;  // Particle AS compute pipeline layout
  nvvk::DescriptorBindings m_descriptorBindings       = {};              // Raster Descriptor bindings
  VkDescriptorSetLayout    m_descriptorSetLayout      = VK_NULL_HANDLE;  // Descriptor set layout
  VkDescriptorSet          m_descriptorSet            = VK_NULL_HANDLE;  // Raster Descriptor set
  VkDescriptorPool         m_descriptorPool           = VK_NULL_HANDLE;  // Raster Descriptor pool

  nvvk::Buffer m_frameInfoBuffer;  // uniform buffer to store frame parameters
                                   // defined by global variable prmFrame

  /////////////////////////
  // RTX specific

  VkPhysicalDeviceRayTracingPipelinePropertiesKHR m_rtProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
  VkPhysicalDeviceAccelerationStructurePropertiesKHR m_accelStructProps{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};

  std::vector<VkRayTracingShaderGroupCreateInfoKHR> m_rtShaderGroups;
  VkPipelineLayout                                  m_rtPipelineLayout = VK_NULL_HANDLE;
  VkPipeline                                        m_rtPipeline = VK_NULL_HANDLE;  // The RTX pipeline to ray trace
                                                                                    // gaussian splats and meshes

  nvvk::DescriptorBindings m_rtDescriptorBindings  = {};
  VkDescriptorSetLayout    m_rtDescriptorSetLayout = VK_NULL_HANDLE;
  VkDescriptorSet          m_rtDescriptorSet       = VK_NULL_HANDLE;
  VkDescriptorPool         m_rtDescriptorPool      = VK_NULL_HANDLE;

  nvvk::Buffer m_payloadDevice;

  nvvk::Buffer m_rtSBTBuffer;  // common to GS and Mesh
  // The 4 SBT regions (raygen, miss, chit, call in this order)
  nvvk::SBTGenerator::Regions m_sbtRegions{};  // common to GS and Mesh

  shaderio::PushConstantRay m_pcRay{};  // Push constant for ray tracer

  ///////////////////////////////
  // Deferred shading (compute shader for lighting in raster-only pipelines)
  VkPipeline m_computePipelineDeferredShading = VK_NULL_HANDLE;

  // Post processing

  VkPipeline       m_computePipelinePostProcess = VK_NULL_HANDLE;
  VkPipelineLayout m_pipelineLayoutPostProcess  = VK_NULL_HANDLE;

  nvvk::DescriptorBindings m_descriptorBindingsPostProcess{};
  VkDescriptorSetLayout    m_descriptorSetLayoutPostProcess = VK_NULL_HANDLE;
  VkDescriptorSet          m_descriptorSetPostProcess       = VK_NULL_HANDLE;
  VkDescriptorPool         m_descriptorPoolPostProcess      = VK_NULL_HANDLE;

  ///////////////////////////////
  // Dynamic 4DGS sequence (docs/dynamic-4dgs.md)

  DynamicSplatSequence       m_dynamicSequence;                        // asset + VRAM keyframe pool + clock
  std::string                m_dynamicSequencePath;                    // requested .dgs (empty = none)
  bool                       m_dynamicSequenceRequested = false;
  bool                       m_dynamicSequenceOpen      = false;
  double                     m_sequenceStartSeconds     = 0.0;         // wall-clock at open
  VkCommandPool              m_dynamicCommandPool       = VK_NULL_HANDLE;

  std::string                m_trainingCameraMetadataPath;
  uint32_t                   m_trainingCameraIndex      = 0;
  bool                       m_trainingCameraRequested  = false;

  nvvk::DescriptorBindings m_descriptorBindingsAnimate = {};
  VkDescriptorSetLayout    m_descriptorSetLayoutAnimate = VK_NULL_HANDLE;
  VkDescriptorPool         m_descriptorPoolAnimate      = VK_NULL_HANDLE;
  VkDescriptorSet          m_descriptorSetAnimate       = VK_NULL_HANDLE;
  VkPipelineLayout         m_pipelineLayoutAnimate      = VK_NULL_HANDLE;
  VkPipeline               m_computePipelineAnimate     = VK_NULL_HANDLE;
  uint32_t                 m_dynamicFrameId             = 0;  // onRender invocations since attach

  bool openDynamicSequence();
  void initializeTrainingCamera();
  void destroyDynamicAnimate();
  void initDynamicAnimate();
  void animateDynamicSplats(VkCommandBuffer cmd, uint32_t splatCount);

  ///////////////////////////////
  // DLSS
#if defined(USE_DLSS)
  DlssDenoiser m_dlss;
  glm::mat4    m_prevMVP{1.0f};              // Previous frame's model-view-projection for motion vectors
  glm::vec2    m_currentJitter{0.0f, 0.0f};  // Current frame's jitter for DLSS
#endif

  ///////////////////////////////
  // Image Comparison
  ImageCompare m_imageCompare;
  bool         m_requestCaptureComparison = false;  // Request to capture reference at end of next frame

  // Returns the appropriate buffer based on comparison mode and visualization
  // settings
  VkDescriptorSet getPresentationImageDescriptorSet(void);

  ///////////////////////////////
  // Tone Mapping
  nvshaders::Tonemapper    m_tonemapper;
  shaderio::TonemapperData m_tonemapperData{.isActive = 0, .method = shaderio::ToneMapMethod::eClip};

  void initTonemapper();
  void deinitTonemapper();
  void tonemap(VkCommandBuffer cmd);

  ///////////////////////////////
  // Sky, Sun & IBL environment
  SkySunAndIBL m_sky;
  nvvk::Buffer m_envAccelDummyBuffer{};  // Dummy buffer for RTX_BINDING_ENV_ACCEL when no
                                         // HDR loaded

  ///////////////////////////////
  // Visual Helpers (3D Gizmo and Grid)
  VisualHelpers m_helpers;
  bool          m_showLightProxies = true;  // Show/hide light proxy meshes

  ///////////////////////////////
  // Helper Methods for Image Comparison

  // Returns the source image info for the current visualization mode (used by
  // ImageCompare)
  ImageCompare::ImageInfo getCurrentVisualizationImageInfo() const;

  // Returns all dumpable image buffers for multi-buffer frame capture
  std::vector<BufferDumpInfo> getAllDumpableBuffers() const;

#if defined(USE_DLSS)
  // Helper to map visualization mode to DLSS buffer
  shaderio::DlssImages getDlssBufferForVisuMode(int visualizeMode) const;
#endif
};

}  // namespace curved_stereo_gs

#endif
