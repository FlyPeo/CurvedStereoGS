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

#ifndef _GAUSSIAN_SPLATTING_APP_H_
#define _GAUSSIAN_SPLATTING_APP_H_

#include <nvapp/application.hpp>
#include "gaussian_splatting.h"

namespace curved_stereo_gs {

class GaussianSplattingApp : public GaussianSplatting, public nvapp::IAppElement
{
  friend class CurvedStereoGSProjectReader;
  friend class CurvedStereoGSProjectWriter;

public:
  GaussianSplattingApp(nvutils::ProfilerManager* profilerManager, nvutils::ParameterRegistry* parameterRegistry, bool* benchmarkEnabled);
  ~GaussianSplattingApp() override = default;

  void onAttach(nvapp::Application* app) override;
  void onDetach() override;
  void onResize(VkCommandBuffer cmd, const VkExtent2D& size) override;
  void onPreRender() override;
  void onRender(VkCommandBuffer cmd) override;
  void onFileDrop(const std::filesystem::path& filename) override;
  void onUIRender() override;

  // Query if user is dragging transform helper (used to disable camera)
  bool isDraggingTransformHelper() const { return m_helpers.transform.isDragging(); }
  bool isDraggingComparisonSlider() const { return false; }
  bool isDraggingCursorTarget() const { return false; }
  void requestFrameReset() { resetFrameCounter(); }
  void requestStereoModeRebuild()
  {
    resetFrameCounter();
    m_pendingGBufferReinitSeen = false;
    m_requestGBufferReinit     = true;
  }

private:
  bool* m_pBenchmarkEnabled = {};
};

}  // namespace curved_stereo_gs

#endif // _GAUSSIAN_SPLATTING_APP_H_
