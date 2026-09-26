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

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <nvutils/camera_manipulator.hpp>
#include <vulkan/vulkan_core.h>

struct GLFWwindow;

#ifndef CURVED_STEREO_GS_ACTIVE_STEREO_USE_NATIVE_WGL
#if defined(_WIN32)
#define CURVED_STEREO_GS_ACTIVE_STEREO_USE_NATIVE_WGL 1
#else
#define CURVED_STEREO_GS_ACTIVE_STEREO_USE_NATIVE_WGL 0
#endif
#endif

namespace curved_stereo_gs {

class StereoInteropVk;

class ActiveStereoQuadBufferPresenter
{
public:
  ActiveStereoQuadBufferPresenter();
  ~ActiveStereoQuadBufferPresenter();

  void setCameraManipulator(std::shared_ptr<nvutils::CameraManipulator> camera);
  void setCloseCallback(std::function<void()> callback);
  void setStereoToggleCallback(std::function<void()> callback);
  bool present(const VkExtent2D& size, const std::vector<uint8_t>& leftRgba, const std::vector<uint8_t>& rightRgba);
  bool present(const VkExtent2D& size, const std::vector<float>& leftRgba, const std::vector<float>& rightRgba);
  bool presentInterop(StereoInteropVk& interop, uint64_t frameNumber);
  void releaseInterop();
  bool hasFailed() const { return m_failed; }
  void deinit();
#if defined(_WIN32) && CURVED_STEREO_GS_ACTIVE_STEREO_USE_NATIVE_WGL
  intptr_t handleWindowMessage(void* hwnd, uint32_t message, uintptr_t wparam, intptr_t lparam);
#endif

private:
  struct InteropGlState;

  bool        init(const VkExtent2D& size);
  void        drawEye(uint32_t width, uint32_t height, unsigned int drawBuffer, const std::vector<uint8_t>& rgba);
  void        drawEye(uint32_t width, uint32_t height, unsigned int drawBuffer, const std::vector<float>& rgba);
  void        updateCameraWindowSize(uint32_t width, uint32_t height);
  static void handleGlfwWindowClose(GLFWwindow* window);
  static void handleGlfwKey(GLFWwindow* window, int key, int scancode, int action, int mods);
  static void handleGlfwFramebufferSize(GLFWwindow* window, int width, int height);

#if defined(_WIN32) && CURVED_STEREO_GS_ACTIVE_STEREO_USE_NATIVE_WGL
  void updateMousePosition(int32_t x, int32_t y, bool forceReset);
  void updateModifierKeys();
  void releaseMouseCaptureIfNeeded(void* hwnd);
  void processKeyboardCameraInput(uintptr_t virtualKey);
  void registerRawInput(void* hwnd);
  void processRawInput(intptr_t lparam);
  void applySpaceMouseMotion(const int16_t motion[6]);
  void applySpaceMouseButtons(uint32_t buttonMask);

  void*                              m_windowHandle{nullptr};
  void*                              m_deviceContext{nullptr};
  void*                              m_glContext{nullptr};
  unsigned int                       m_glTexture{0};
  VkExtent2D                         m_glTextureSize{0, 0};
  bool                               m_glTextureFloat{false};
  bool                               m_leftMouseDown{false};
  bool                               m_middleMouseDown{false};
  bool                               m_rightMouseDown{false};
  bool                               m_shiftDown{false};
  bool                               m_ctrlDown{false};
  bool                               m_altDown{false};
  bool                               m_rawInputRegistered{false};
  bool                               m_spaceMouseHomeValid{false};
  bool                               m_loggedSpaceMouseSample{false};
  uint32_t                           m_spaceMouseButtonMask{0};
  nvutils::CameraManipulator::Camera m_spaceMouseHomeCamera;
#else
  GLFWwindow* m_window{nullptr};
#endif
  VkExtent2D                                  m_size{0, 0};
  bool                                        m_failed{false};
  bool                                        m_loggedFirstPresent{false};
  std::shared_ptr<nvutils::CameraManipulator> m_cameraManip;
  std::function<void()>                       m_closeCallback;
  std::function<void()>                       m_stereoToggleCallback;
  std::unique_ptr<InteropGlState>             m_interopGl;
};

}  // namespace curved_stereo_gs
