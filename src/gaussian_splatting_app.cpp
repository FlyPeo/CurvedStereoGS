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

#include "gaussian_splatting_app.h"
#include "parameters.h"
#include "curved_stereo_gs_project_reader.h"
#include <algorithm>
#include <fstream>
#include <tinygltf/json.hpp>
#include <imgui.h>

namespace curved_stereo_gs {

GaussianSplattingApp::GaussianSplattingApp(nvutils::ProfilerManager* profilerManager, nvutils::ParameterRegistry* parameterRegistry, bool* benchmarkEnabled)
    : GaussianSplatting(profilerManager, parameterRegistry)
    , m_pBenchmarkEnabled(benchmarkEnabled)
{
}



void GaussianSplattingApp::onDetach()
{
  GaussianSplatting::onDetach();
}

void GaussianSplattingApp::onResize(VkCommandBuffer cmd, const VkExtent2D& size)
{
  GaussianSplatting::onResize(cmd, size);
}

void GaussianSplattingApp::onAttach(nvapp::Application* app)
{
  GaussianSplatting::onAttach(app);
  m_helpers.grid.setVisible(false);
  m_helpers.setEditingMode(false);
  m_showLightProxies = false;
  cameraManip->setMode(nvutils::CameraManipulator::Examine);
}

void GaussianSplattingApp::onPreRender()
{
  GaussianSplatting::onPreRender();

  // 1. Process project loads
  if(!prmScene.projectToLoadFilename.empty())
  {
    std::string path = prmScene.projectToLoadFilename.string();
    std::ifstream i(path);
    if(i.is_open())
    {
      nlohmann::json data;
      try { 
        i >> data; 
        CurvedStereoGSProjectReader::loadProject(data, path, this); 
      }
      catch(...) { 
        LOGE("Error loading project %s\n", path.c_str()); 
      }
    }
    prmScene.projectToLoadFilename = "";
  }

  // 2. Start ply/splat loads
  if(!prmScene.sceneLoadQueue.empty() && m_plyLoader.getStatus() == PlyLoaderAsync::State::E_READY)
  {
    vkDeviceWaitIdle(m_device);
    const SceneLoadRequest& request = prmScene.sceneLoadQueue.front();
    std::shared_ptr<SplatSetVk> splatSetToLoad = request.splatSet ? request.splatSet : std::make_shared<SplatSetVk>();
    
    if(m_plyLoader.loadScene(request.path, splatSetToLoad))
    {
      m_currentLoadRequest = request;
    }
    prmScene.sceneLoadQueue.pop_front();
  }

  // 3. Handle ply load completion
  if(m_plyLoader.getStatus() == PlyLoaderAsync::State::E_LOADED)
  {
    auto loadedSplatSet = m_assets.splatSets.createSplatSet(m_currentLoadRequest.path.string(), m_plyLoader.getLoadedSplatSet());
    if(m_currentLoadRequest.instance)
    {
      m_currentLoadRequest.instance->splatSet = loadedSplatSet;
      m_selectedSplatInstance = m_assets.splatSets.registerInstance(loadedSplatSet, m_currentLoadRequest.instance);
      for(auto& additionalInstance : m_currentLoadRequest.additionalInstances)
      {
        additionalInstance->splatSet = loadedSplatSet;
        m_assets.splatSets.registerInstance(loadedSplatSet, additionalInstance);
      }
    }
    else
    {
      m_selectedSplatInstance = m_assets.splatSets.createInstance(loadedSplatSet);
    }
    m_plyLoader.reset();
  }
  else if(m_plyLoader.getStatus() == PlyLoaderAsync::State::E_FAILURE)
  {
    m_plyLoader.reset();
  }
}

void GaussianSplattingApp::onRender(VkCommandBuffer cmd)
{
  GaussianSplatting::onRender(cmd);
}

void GaussianSplattingApp::onUIRender()
{
  if(m_app)
  {
    VkDescriptorSet displayDescriptor = getPresentationImageDescriptorSet();
    if(displayDescriptor)
    {
      ImVec2 size(m_app->getViewportSize().width, m_app->getViewportSize().height);
      ImGui::GetBackgroundDrawList()->AddImage((ImTextureID)displayDescriptor, ImVec2(0, 0), size);
    }
  }
}

void GaussianSplattingApp::onFileDrop(const std::filesystem::path& filename)
{
  std::string extension = filename.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

  if(extension == ".ply" || extension == ".spz" || extension == ".splat")
  {
    prmScene.pushLoadRequest(filename);
  }
  else if(extension == ".csgs" || extension == ".vkgs")
  {
    prmScene.projectToLoadFilename = filename;
  }
  else if(extension == ".obj" || extension == ".gltf" || extension == ".glb")
  {
    prmScene.meshToImportFilename = filename;
  }
}

}  // namespace curved_stereo_gs
