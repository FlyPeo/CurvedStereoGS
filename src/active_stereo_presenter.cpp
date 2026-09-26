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
#include "stereo_interop_vk.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#if CURVED_STEREO_GS_ACTIVE_STEREO_USE_NATIVE_WGL
#include <windowsx.h>
#endif
#endif

#include <GL/glcustom.h>
#include <nvutils/camera_manipulator.hpp>

#ifndef GL_FRAMEBUFFER_SRGB
#define GL_FRAMEBUFFER_SRGB 0x8DB9
#endif
#ifndef GL_RGBA32F
#define GL_RGBA32F 0x8814
#endif

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <array>
#include <cmath>
#include <cstring>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nvutils/logger.hpp>

namespace curved_stereo_gs {

struct ActiveStereoQuadBufferPresenter::InteropGlState
{
#if defined(_WIN32) && CURVED_STEREO_GS_ACTIVE_STEREO_USE_NATIVE_WGL
  struct Slot
  {
    std::array<GLuint, StereoInteropVk::kEyeCount> memoryObjects{};
    std::array<GLuint, StereoInteropVk::kEyeCount> textures{};
    std::array<GLuint, StereoInteropVk::kEyeCount> framebuffers{};
    GLuint                                         vulkanReady{0};
    GLuint                                         openglDone{0};
  };

  PFNGLCREATEMEMORYOBJECTSEXTPROC        createMemoryObjects{nullptr};
  PFNGLDELETEMEMORYOBJECTSEXTPROC        deleteMemoryObjects{nullptr};
  PFNGLMEMORYOBJECTPARAMETERIVEXTPROC    memoryObjectParameteriv{nullptr};
  PFNGLIMPORTMEMORYWIN32HANDLEEXTPROC    importMemoryWin32Handle{nullptr};
  PFNGLTEXSTORAGEMEM2DEXTPROC            texStorageMem2D{nullptr};
  PFNGLGENSEMAPHORESEXTPROC              genSemaphores{nullptr};
  PFNGLDELETESEMAPHORESEXTPROC           deleteSemaphores{nullptr};
  PFNGLIMPORTSEMAPHOREWIN32HANDLEEXTPROC importSemaphoreWin32Handle{nullptr};
  PFNGLWAITSEMAPHOREEXTPROC              waitSemaphore{nullptr};
  PFNGLSIGNALSEMAPHOREEXTPROC            signalSemaphore{nullptr};
  PFNGLGENFRAMEBUFFERSPROC               genFramebuffers{nullptr};
  PFNGLDELETEFRAMEBUFFERSPROC            deleteFramebuffers{nullptr};
  PFNGLBINDFRAMEBUFFERPROC               bindFramebuffer{nullptr};
  PFNGLFRAMEBUFFERTEXTURE2DPROC          framebufferTexture2D{nullptr};
  PFNGLCHECKFRAMEBUFFERSTATUSPROC        checkFramebufferStatus{nullptr};
  PFNGLBLITFRAMEBUFFERPROC               blitFramebuffer{nullptr};

  std::vector<Slot> slots;
  uint64_t          generation{0};
  VkExtent2D        size{0, 0};
#endif
};

ActiveStereoQuadBufferPresenter::ActiveStereoQuadBufferPresenter() = default;

namespace {

#if defined(_WIN32) && CURVED_STEREO_GS_ACTIVE_STEREO_USE_NATIVE_WGL

constexpr const char* kStereoWindowClass  = "curved_stereo_gs_active_stereo_wgl_window";
constexpr const char* kCloseRequestedProp = "curved_stereo_gs_active_stereo_close_requested";

template <typename T>
T loadOpenGlFunction(const char* name)
{
  PROC proc = wglGetProcAddress(name);
  if(proc == nullptr || proc == reinterpret_cast<PROC>(1) || proc == reinterpret_cast<PROC>(2)
     || proc == reinterpret_cast<PROC>(3) || proc == reinterpret_cast<PROC>(-1))
  {
    HMODULE module = GetModuleHandleA("opengl32.dll");
    proc           = module != nullptr ? GetProcAddress(module, name) : nullptr;
  }
  return reinterpret_cast<T>(proc);
}

bool hasOpenGlExtension(const char* extension)
{
  const char* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
  if(extensions == nullptr || extension == nullptr || *extension == '\0' || std::strchr(extension, ' ') != nullptr)
    return false;

  const size_t extensionLength = std::strlen(extension);
  const char*  current         = extensions;
  while((current = std::strstr(current, extension)) != nullptr)
  {
    const bool startsAtBoundary = current == extensions || current[-1] == ' ';
    const bool endsAtBoundary   = current[extensionLength] == '\0' || current[extensionLength] == ' ';
    if(startsAtBoundary && endsAtBoundary)
      return true;
    current += extensionLength;
  }
  return false;
}

#ifndef WGL_NUMBER_PIXEL_FORMATS_ARB
#define WGL_NUMBER_PIXEL_FORMATS_ARB 0x2000
#endif
#ifndef WGL_DRAW_TO_WINDOW_ARB
#define WGL_DRAW_TO_WINDOW_ARB 0x2001
#endif
#ifndef WGL_ACCELERATION_ARB
#define WGL_ACCELERATION_ARB 0x2003
#endif
#ifndef WGL_SUPPORT_OPENGL_ARB
#define WGL_SUPPORT_OPENGL_ARB 0x2010
#endif
#ifndef WGL_DOUBLE_BUFFER_ARB
#define WGL_DOUBLE_BUFFER_ARB 0x2011
#endif
#ifndef WGL_STEREO_ARB
#define WGL_STEREO_ARB 0x2012
#endif
#ifndef WGL_PIXEL_TYPE_ARB
#define WGL_PIXEL_TYPE_ARB 0x2013
#endif
#ifndef WGL_COLOR_BITS_ARB
#define WGL_COLOR_BITS_ARB 0x2014
#endif
#ifndef WGL_RED_BITS_ARB
#define WGL_RED_BITS_ARB 0x2015
#endif
#ifndef WGL_GREEN_BITS_ARB
#define WGL_GREEN_BITS_ARB 0x2017
#endif
#ifndef WGL_BLUE_BITS_ARB
#define WGL_BLUE_BITS_ARB 0x2019
#endif
#ifndef WGL_ALPHA_BITS_ARB
#define WGL_ALPHA_BITS_ARB 0x201B
#endif
#ifndef WGL_NO_ACCELERATION_ARB
#define WGL_NO_ACCELERATION_ARB 0x2025
#endif
#ifndef WGL_GENERIC_ACCELERATION_ARB
#define WGL_GENERIC_ACCELERATION_ARB 0x2026
#endif
#ifndef WGL_FULL_ACCELERATION_ARB
#define WGL_FULL_ACCELERATION_ARB 0x2027
#endif
#ifndef WGL_TYPE_RGBA_ARB
#define WGL_TYPE_RGBA_ARB 0x202B
#endif
#ifndef WGL_TYPE_RGBA_FLOAT_ARB
#define WGL_TYPE_RGBA_FLOAT_ARB 0x21A0
#endif
#ifndef GL_CLAMP_VERTEX_COLOR_ARB
#define GL_CLAMP_VERTEX_COLOR_ARB 0x891A
#endif
#ifndef GL_CLAMP_FRAGMENT_COLOR_ARB
#define GL_CLAMP_FRAGMENT_COLOR_ARB 0x891B
#endif
#ifndef GL_CLAMP_READ_COLOR_ARB
#define GL_CLAMP_READ_COLOR_ARB 0x891C
#endif

using WglGetPixelFormatAttribivArbProc = BOOL(WINAPI*)(HDC, int, int, UINT, const int*, int*);
using GlClampColorProc                 = void(APIENTRY*)(GLenum, GLenum);

struct WglPixelFormatFunctions
{
  WglGetPixelFormatAttribivArbProc getPixelFormatAttribiv{nullptr};
};

LRESULT CALLBACK stereoWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
  if(message == WM_NCCREATE)
  {
    const auto* createStruct = reinterpret_cast<const CREATESTRUCTA*>(lparam);
    SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(createStruct->lpCreateParams));
  }

  auto* presenter = reinterpret_cast<ActiveStereoQuadBufferPresenter*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
  if(presenter != nullptr)
    return static_cast<LRESULT>(presenter->handleWindowMessage(hwnd, message, wparam, lparam));

  return DefWindowProcA(hwnd, message, wparam, lparam);
}

bool registerStereoWindowClass()
{
  WNDCLASSEXA windowClass{};
  windowClass.cbSize        = sizeof(windowClass);
  windowClass.style         = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
  windowClass.lpfnWndProc   = stereoWindowProc;
  windowClass.hInstance     = GetModuleHandleA(nullptr);
  windowClass.hCursor       = LoadCursor(nullptr, IDC_ARROW);
  windowClass.lpszClassName = kStereoWindowClass;

  if(RegisterClassExA(&windowClass) != 0)
    return true;

  return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

WglPixelFormatFunctions loadWglPixelFormatFunctions()
{
  WglPixelFormatFunctions functions{};

  HWND dummyWindow = CreateWindowExA(0, kStereoWindowClass, "CurvedStereoGS WGL extension loader", WS_POPUP, 0, 0, 1, 1, nullptr,
                                     nullptr, GetModuleHandleA(nullptr), nullptr);
  if(dummyWindow == nullptr)
    return functions;

  HDC dummyDc = GetDC(dummyWindow);
  if(dummyDc == nullptr)
  {
    DestroyWindow(dummyWindow);
    return functions;
  }

  PIXELFORMATDESCRIPTOR dummyFormat{};
  dummyFormat.nSize      = sizeof(dummyFormat);
  dummyFormat.nVersion   = 1;
  dummyFormat.dwFlags    = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
  dummyFormat.iPixelType = PFD_TYPE_RGBA;
  dummyFormat.cColorBits = 24;
  dummyFormat.cAlphaBits = 8;

  const int dummyPixelFormat = ChoosePixelFormat(dummyDc, &dummyFormat);
  if(dummyPixelFormat != 0 && SetPixelFormat(dummyDc, dummyPixelFormat, &dummyFormat) != FALSE)
  {
    HGLRC dummyContext = wglCreateContext(dummyDc);
    if(dummyContext != nullptr && wglMakeCurrent(dummyDc, dummyContext) != FALSE)
    {
      functions.getPixelFormatAttribiv =
          reinterpret_cast<WglGetPixelFormatAttribivArbProc>(wglGetProcAddress("wglGetPixelFormatAttribivARB"));
      wglMakeCurrent(nullptr, nullptr);
    }

    if(dummyContext != nullptr)
      wglDeleteContext(dummyContext);
  }

  ReleaseDC(dummyWindow, dummyDc);
  DestroyWindow(dummyWindow);
  return functions;
}

bool isUsableStereoPixelFormat(const PIXELFORMATDESCRIPTOR& format)
{
  constexpr DWORD requiredFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER | PFD_STEREO;
  return (format.dwFlags & requiredFlags) == requiredFlags && format.iPixelType == PFD_TYPE_RGBA && format.cColorBits >= 24;
}

int chooseLegacyStereoPixelFormat(HDC deviceContext, PIXELFORMATDESCRIPTOR& chosenFormat, bool& chosenIsFloat)
{
  PIXELFORMATDESCRIPTOR format{};
  const int             formatCount = DescribePixelFormat(deviceContext, 1, sizeof(format), &format);

  int bestFormat = 0;
  int bestScore  = -1;

  for(int formatIndex = 1; formatIndex <= formatCount; ++formatIndex)
  {
    PIXELFORMATDESCRIPTOR candidate{};
    if(DescribePixelFormat(deviceContext, formatIndex, sizeof(candidate), &candidate) == 0)
      continue;
    if(!isUsableStereoPixelFormat(candidate))
      continue;

    int score = int(candidate.cColorBits) * 100 + int(candidate.cAlphaBits) * 10;
    if((candidate.dwFlags & PFD_GENERIC_FORMAT) == 0)
      score += 100000;
    if((candidate.dwFlags & PFD_GENERIC_ACCELERATED) != 0)
      score += 50000;

    if(score > bestScore)
    {
      bestScore     = score;
      bestFormat    = formatIndex;
      chosenFormat  = candidate;
      chosenIsFloat = false;
    }
  }

  return bestFormat;
}

int chooseArbStereoPixelFormat(HDC deviceContext, PIXELFORMATDESCRIPTOR& chosenFormat, bool& chosenIsFloat)
{
  const WglPixelFormatFunctions wgl = loadWglPixelFormatFunctions();
  if(wgl.getPixelFormatAttribiv == nullptr)
    return 0;

  int       formatCount = 0;
  const int countAttr   = WGL_NUMBER_PIXEL_FORMATS_ARB;
  if(wgl.getPixelFormatAttribiv(deviceContext, 1, 0, 1, &countAttr, &formatCount) == FALSE || formatCount <= 0)
    return 0;

  constexpr std::array<int, 11> attrs = {
      WGL_DRAW_TO_WINDOW_ARB, WGL_SUPPORT_OPENGL_ARB, WGL_DOUBLE_BUFFER_ARB, WGL_STEREO_ARB,
      WGL_PIXEL_TYPE_ARB,     WGL_COLOR_BITS_ARB,     WGL_RED_BITS_ARB,      WGL_GREEN_BITS_ARB,
      WGL_BLUE_BITS_ARB,      WGL_ALPHA_BITS_ARB,     WGL_ACCELERATION_ARB,
  };

  int bestFormat = 0;
  int bestScore  = -1;

  for(int formatIndex = 1; formatIndex <= formatCount; ++formatIndex)
  {
    std::array<int, attrs.size()> values{};
    if(wgl.getPixelFormatAttribiv(deviceContext, formatIndex, 0, static_cast<UINT>(attrs.size()), attrs.data(), values.data()) == FALSE)
    {
      continue;
    }

    const bool drawToWindow = values[0] != 0;
    const bool supportsGl   = values[1] != 0;
    const bool doubleBuffer = values[2] != 0;
    const bool stereo       = values[3] != 0;
    const int  pixelType    = values[4];
    const int  colorBits    = values[5];
    const int  redBits      = values[6];
    const int  greenBits    = values[7];
    const int  blueBits     = values[8];
    const int  alphaBits    = values[9];
    const int  acceleration = values[10];
    const bool isRgba       = pixelType == WGL_TYPE_RGBA_ARB;
    const bool isFloat      = pixelType == WGL_TYPE_RGBA_FLOAT_ARB;

    // Use normalized RGBA stereo pixel formats for the Vulkan readback
    // presenter. Float window formats can display GL_FLOAT pixels differently
    // across drivers and are a common source of washed-out or shifted colors on
    // quad-buffer output.
    if(!drawToWindow || !supportsGl || !doubleBuffer || !stereo || !isRgba || colorBits < 24)
      continue;

    int score = colorBits * 1000 + alphaBits * 100 + redBits + greenBits + blueBits;
    if(acceleration == WGL_FULL_ACCELERATION_ARB)
      score += 1000000;
    else if(acceleration == WGL_GENERIC_ACCELERATION_ARB)
      score += 100000;
    else if(acceleration == WGL_NO_ACCELERATION_ARB)
      score -= 1000000;

    if(score > bestScore)
    {
      PIXELFORMATDESCRIPTOR candidate{};
      if(DescribePixelFormat(deviceContext, formatIndex, sizeof(candidate), &candidate) == 0)
        continue;

      bestScore     = score;
      bestFormat    = formatIndex;
      chosenFormat  = candidate;
      chosenIsFloat = false;
    }
  }

  return bestFormat;
}

int chooseStereoPixelFormat(HDC deviceContext, PIXELFORMATDESCRIPTOR& chosenFormat, bool& chosenIsFloat)
{
  int pixelFormat = chooseArbStereoPixelFormat(deviceContext, chosenFormat, chosenIsFloat);
  if(pixelFormat != 0)
    return pixelFormat;

  return chooseLegacyStereoPixelFormat(deviceContext, chosenFormat, chosenIsFloat);
}

void logStereoPixelFormatSummary(HDC deviceContext)
{
  PIXELFORMATDESCRIPTOR format{};
  const int             formatCount = DescribePixelFormat(deviceContext, 1, sizeof(format), &format);

  int stereoFormats       = 0;
  int usableStereoFormats = 0;
  for(int formatIndex = 1; formatIndex <= formatCount; ++formatIndex)
  {
    PIXELFORMATDESCRIPTOR candidate{};
    if(DescribePixelFormat(deviceContext, formatIndex, sizeof(candidate), &candidate) == 0)
      continue;

    if((candidate.dwFlags & PFD_STEREO) != 0)
      ++stereoFormats;
    if(isUsableStereoPixelFormat(candidate))
      ++usableStereoFormats;
  }

  LOGE(
      "Active stereo presenter: WGL exposes %d pixel formats, %d with "
      "PFD_STEREO, %d usable for quad-buffer OpenGL.\n",
      formatCount, stereoFormats, usableStereoFormats);
}

void disableFloatColorClampingIfAvailable()
{
  auto clampColor = reinterpret_cast<GlClampColorProc>(wglGetProcAddress("glClampColorARB"));
  if(clampColor == nullptr)
    clampColor = reinterpret_cast<GlClampColorProc>(wglGetProcAddress("glClampColor"));
  if(clampColor == nullptr)
    return;

  clampColor(GL_CLAMP_VERTEX_COLOR_ARB, GL_FALSE);
  clampColor(GL_CLAMP_FRAGMENT_COLOR_ARB, GL_FALSE);
  clampColor(GL_CLAMP_READ_COLOR_ARB, GL_FALSE);
}

void pumpStereoWindowMessages(HWND hwnd)
{
  MSG message{};
  while(PeekMessageA(&message, hwnd, 0, 0, PM_REMOVE) != 0)
  {
    TranslateMessage(&message);
    DispatchMessageA(&message);
  }
}

#endif

}  // namespace

ActiveStereoQuadBufferPresenter::~ActiveStereoQuadBufferPresenter()
{
  deinit();
}

void ActiveStereoQuadBufferPresenter::setCameraManipulator(std::shared_ptr<nvutils::CameraManipulator> camera)
{
  m_cameraManip = std::move(camera);
  if(m_cameraManip)
  {
    m_spaceMouseHomeCamera = m_cameraManip->getCamera();
    m_spaceMouseHomeValid  = true;
  }
  else
  {
    m_spaceMouseHomeValid = false;
  }
  updateCameraWindowSize(m_size.width, m_size.height);
}

void ActiveStereoQuadBufferPresenter::setCloseCallback(std::function<void()> callback)
{
  m_closeCallback = std::move(callback);
}

void ActiveStereoQuadBufferPresenter::setStereoToggleCallback(std::function<void()> callback)
{
  m_stereoToggleCallback = std::move(callback);
}

void ActiveStereoQuadBufferPresenter::updateCameraWindowSize(uint32_t width, uint32_t height)
{
  if(m_cameraManip && width > 0 && height > 0)
  {
    m_cameraManip->setWindowSize({width, height});
    m_cameraManip->adjustOrthographicAspect();
  }
}

#if defined(_WIN32) && CURVED_STEREO_GS_ACTIVE_STEREO_USE_NATIVE_WGL

intptr_t ActiveStereoQuadBufferPresenter::handleWindowMessage(void* hwndPtr, uint32_t message, uintptr_t wparam, intptr_t lparam)
{
  HWND hwnd = static_cast<HWND>(hwndPtr);

  switch(message)
  {
    case WM_CLOSE:
      SetPropA(hwnd, kCloseRequestedProp, reinterpret_cast<HANDLE>(1));
      ShowWindow(hwnd, SW_HIDE);
      return 0;

    case WM_SETFOCUS:
      updateModifierKeys();
      return 0;

    case WM_KILLFOCUS:
      m_leftMouseDown = m_middleMouseDown = m_rightMouseDown = false;
      return 0;

    case WM_SIZE: {
      const uint32_t width  = static_cast<uint32_t>(LOWORD(lparam));
      const uint32_t height = static_cast<uint32_t>(HIWORD(lparam));
      updateCameraWindowSize(width, height);
      return 0;
    }

    case WM_LBUTTONDOWN:
      SetFocus(hwnd);
      SetCapture(hwnd);
      m_leftMouseDown = true;
      updateMousePosition(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), true);
      return 0;

    case WM_MBUTTONDOWN:
      SetFocus(hwnd);
      SetCapture(hwnd);
      m_middleMouseDown = true;
      updateMousePosition(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), true);
      return 0;

    case WM_RBUTTONDOWN:
      SetFocus(hwnd);
      SetCapture(hwnd);
      m_rightMouseDown = true;
      updateMousePosition(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), true);
      return 0;

    case WM_LBUTTONUP:
      m_leftMouseDown = false;
      updateMousePosition(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), false);
      releaseMouseCaptureIfNeeded(hwnd);
      return 0;

    case WM_MBUTTONUP:
      m_middleMouseDown = false;
      updateMousePosition(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), false);
      releaseMouseCaptureIfNeeded(hwnd);
      return 0;

    case WM_RBUTTONUP:
      m_rightMouseDown = false;
      updateMousePosition(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), false);
      releaseMouseCaptureIfNeeded(hwnd);
      return 0;

    case WM_MOUSEMOVE:
      updateMousePosition(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), false);
      return 0;

    case WM_MOUSEWHEEL: {
      if(m_cameraManip)
      {
        updateModifierKeys();
        nvutils::CameraManipulator::Inputs inputs;
        inputs.lmb              = m_leftMouseDown;
        inputs.mmb              = m_middleMouseDown;
        inputs.rmb              = m_rightMouseDown;
        inputs.shift            = m_shiftDown;
        inputs.ctrl             = m_ctrlDown;
        inputs.alt              = m_altDown;
        const double wheelSteps = double(GET_WHEEL_DELTA_WPARAM(wparam)) / double(WHEEL_DELTA);
        m_cameraManip->wheel(wheelSteps * -10.0, inputs);
      }
      return 0;
    }

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
      const bool wasDown = (lparam & (intptr_t(1) << 30)) != 0;
      if(!wasDown && wparam == VK_ESCAPE)
      {
        if(m_closeCallback)
          m_closeCallback();
        return 0;
      }
      if(!wasDown && (wparam == '0' || wparam == VK_NUMPAD0))
      {
        if(m_stereoToggleCallback)
          m_stereoToggleCallback();
        return 0;
      }
      processKeyboardCameraInput(wparam);
      return 0;
    }

    case WM_INPUT:
      processRawInput(lparam);
      return 0;

    default:
      break;
  }

  return DefWindowProcA(hwnd, message, wparam, lparam);
}

void ActiveStereoQuadBufferPresenter::updateModifierKeys()
{
  m_shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  m_ctrlDown  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
  m_altDown   = (GetKeyState(VK_MENU) & 0x8000) != 0;
}

void ActiveStereoQuadBufferPresenter::updateMousePosition(int32_t x, int32_t y, bool forceReset)
{
  if(!m_cameraManip)
    return;

  updateModifierKeys();
  const glm::dvec2 mousePosition{double(x), double(y)};

  nvutils::CameraManipulator::Inputs inputs;
  inputs.lmb   = m_leftMouseDown;
  inputs.mmb   = m_middleMouseDown;
  inputs.rmb   = m_rightMouseDown;
  inputs.shift = m_shiftDown;
  inputs.ctrl  = m_ctrlDown;
  inputs.alt   = m_altDown;

  if(forceReset || (!inputs.lmb && !inputs.mmb && !inputs.rmb))
    m_cameraManip->setMousePosition(mousePosition);
  else
    m_cameraManip->mouseMove(mousePosition, inputs);
}

void ActiveStereoQuadBufferPresenter::releaseMouseCaptureIfNeeded(void* hwndPtr)
{
  if(!m_leftMouseDown && !m_middleMouseDown && !m_rightMouseDown && GetCapture() == static_cast<HWND>(hwndPtr))
    ReleaseCapture();
}

void ActiveStereoQuadBufferPresenter::processKeyboardCameraInput(uintptr_t virtualKey)
{
  if(!m_cameraManip)
    return;

  updateModifierKeys();
  if(m_altDown)
    return;

  double keyMotionFactor = 1.0 / 60.0;
  if(m_shiftDown)
    keyMotionFactor *= 5.0;
  if(m_ctrlDown)
    keyMotionFactor *= 0.1;

  switch(virtualKey)
  {
    case 'W':
      m_cameraManip->keyMotion({keyMotionFactor, 0.0}, nvutils::CameraManipulator::Actions::Dolly);
      break;
    case 'S':
      m_cameraManip->keyMotion({-keyMotionFactor, 0.0}, nvutils::CameraManipulator::Actions::Dolly);
      break;
    case 'D':
    case VK_RIGHT:
      m_cameraManip->keyMotion({keyMotionFactor, 0.0}, nvutils::CameraManipulator::Actions::Pan);
      break;
    case 'A':
    case VK_LEFT:
      m_cameraManip->keyMotion({-keyMotionFactor, 0.0}, nvutils::CameraManipulator::Actions::Pan);
      break;
    case VK_UP:
      m_cameraManip->keyMotion({0.0, keyMotionFactor}, nvutils::CameraManipulator::Actions::Pan);
      break;
    case VK_DOWN:
      m_cameraManip->keyMotion({0.0, -keyMotionFactor}, nvutils::CameraManipulator::Actions::Pan);
      break;
    default:
      break;
  }
}

void ActiveStereoQuadBufferPresenter::registerRawInput(void* hwndPtr)
{
  if(m_rawInputRegistered)
    return;

  RAWINPUTDEVICE devices[1]{};
  devices[0].usUsagePage = 0x01;  // Generic Desktop Controls
  devices[0].usUsage     = 0x08;  // Multi-axis controller, used by common 3Dconnexion devices
  devices[0].dwFlags     = RIDEV_INPUTSINK;
  devices[0].hwndTarget  = static_cast<HWND>(hwndPtr);

  if(RegisterRawInputDevices(devices, 1, sizeof(RAWINPUTDEVICE)) == FALSE)
  {
    LOGW(
        "Active stereo presenter: failed to register RawInput multi-axis "
        "controller for 3D mouse input.\n");
    return;
  }

  m_rawInputRegistered = true;
  LOGI(
      "Active stereo presenter: RawInput multi-axis controller registered for "
      "3D mouse input.\n");
}

void ActiveStereoQuadBufferPresenter::processRawInput(intptr_t lparam)
{
  if(!m_cameraManip)
    return;

  UINT rawInputSize = 0;
  if(GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, nullptr, &rawInputSize, sizeof(RAWINPUTHEADER)) != 0
     || rawInputSize == 0)
  {
    return;
  }

  std::vector<uint8_t> rawInput(rawInputSize);
  if(GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, rawInput.data(), &rawInputSize, sizeof(RAWINPUTHEADER)) != rawInputSize)
  {
    return;
  }

  const RAWINPUT* input = reinterpret_cast<const RAWINPUT*>(rawInput.data());
  if(input->header.dwType != RIM_TYPEHID || input->data.hid.dwSizeHid == 0 || input->data.hid.dwCount == 0)
    return;

  int16_t  motion[6]  = {};
  bool     hasMotion  = false;
  bool     hasButtons = false;
  uint32_t buttonMask = m_spaceMouseButtonMask;

  const BYTE* data = input->data.hid.bRawData;
  for(DWORD reportIndex = 0; reportIndex < input->data.hid.dwCount; ++reportIndex)
  {
    const BYTE* report = data + reportIndex * input->data.hid.dwSizeHid;
    const DWORD size   = input->data.hid.dwSizeHid;
    if(size < 7)
      continue;

    const uint8_t reportId = report[0];
    auto readAxis = [](const BYTE* ptr) { return static_cast<int16_t>(uint16_t(ptr[0]) | (uint16_t(ptr[1]) << 8)); };

    if(reportId == 1 && size >= 13)
    {
      for(int axis = 0; axis < 6; ++axis)
        motion[axis] = static_cast<int16_t>(motion[axis] + readAxis(report + 1 + axis * 2));
      hasMotion = true;
    }
    else if(reportId == 1 && size >= 7)
    {
      motion[0] = static_cast<int16_t>(motion[0] + readAxis(report + 1));
      motion[1] = static_cast<int16_t>(motion[1] + readAxis(report + 3));
      motion[2] = static_cast<int16_t>(motion[2] + readAxis(report + 5));
      hasMotion = true;
    }
    else if(reportId == 2 && size >= 7)
    {
      motion[3] = static_cast<int16_t>(motion[3] + readAxis(report + 1));
      motion[4] = static_cast<int16_t>(motion[4] + readAxis(report + 3));
      motion[5] = static_cast<int16_t>(motion[5] + readAxis(report + 5));
      hasMotion = true;
    }
    else if(reportId == 3 && size >= 2)
    {
      buttonMask = report[1];
      if(size >= 3)
        buttonMask |= uint32_t(report[2]) << 8;
      hasButtons = true;
    }
    else if(size == 12)
    {
      for(int axis = 0; axis < 6; ++axis)
        motion[axis] = static_cast<int16_t>(motion[axis] + readAxis(report + axis * 2));
      hasMotion = true;
    }
    else if(size >= 13)
    {
      for(int axis = 0; axis < 6; ++axis)
        motion[axis] = static_cast<int16_t>(motion[axis] + readAxis(report + 1 + axis * 2));
      hasMotion = true;
    }
    else if(size >= 2)
    {
      buttonMask = report[1];
      hasButtons = true;
    }
  }

  if(hasButtons)
    applySpaceMouseButtons(buttonMask);

  if(hasMotion)
    applySpaceMouseMotion(motion);
}

void ActiveStereoQuadBufferPresenter::applySpaceMouseMotion(const int16_t motion[6])
{
  if(!m_cameraManip)
    return;

  auto axisValue = [](int16_t value) -> double {
    constexpr int dz = 3;
    return std::abs(int(value)) <= dz ? 0.0 : double(value);
  };

  // Keep the OSG channel order. The camera coordinate system differs from the
  // SpaceMouse device frame: invert X/Z translation and all rotation outputs.
  // Y stays positive so pressing the cap down moves the model down.
  const double tx = axisValue(motion[0]);
  const double ty = axisValue(motion[2]);
  const double tz = axisValue(motion[1]);
  const double rx = axisValue(motion[3]);
  const double ry = axisValue(motion[5]);
  const double rz = axisValue(motion[4]);

  if(tx == 0.0 && ty == 0.0 && tz == 0.0 && rx == 0.0 && ry == 0.0 && rz == 0.0)
    return;

  if(!m_loggedSpaceMouseSample)
  {
    LOGI(
        "Active stereo presenter: SpaceMouse raw axes t=(%d,%d,%d) "
        "r=(%d,%d,%d), OSG channel map X=0 Y=2 Z=1 RX=3 RY=5 RZ=4.\n",
        int(motion[0]), int(motion[1]), int(motion[2]), int(motion[3]), int(motion[4]), int(motion[5]));
    m_loggedSpaceMouseSample = true;
  }

  auto camera = m_cameraManip->getCamera();

  const glm::dvec3 forward  = glm::normalize(camera.ctr - camera.eye);
  const glm::dvec3 right    = glm::normalize(glm::cross(forward, camera.up));
  const glm::dvec3 up       = glm::normalize(glm::cross(right, forward));
  const double     distance = glm::max(glm::length(camera.ctr - camera.eye), 0.001);

  // OSG mapping:
  // translate X=channel0, Y=channel2, Z=channel1; scale=(moveStep, -moveStep,
  // moveStep), moveStep=0.3. RawInput values on this device are smaller than
  // the normalized VRPN values used by OSG. Use 50x the original OSG gains (5x
  // the previous RawInput gains).
  constexpr double moveStep    = 7.5;
  constexpr double rotateScale = 0.75;
  const double     moveScale   = distance * 0.00001 * moveStep;
  const glm::dvec3 localMove(-tx * moveScale, ty * moveScale, -tz * moveScale);
  const glm::dvec3 worldMove = right * localMove.x + up * localMove.y - forward * localMove.z;
  camera.eye += worldMove;
  camera.ctr += worldMove;

  // OSG mapping:
  // rotate X=channel3, Y=channel5, Z=channel4; scale=(rotateScale,
  // -rotateScale, rotateScale).
  const double pitch = -rx * rotateScale * 0.0001;
  const double yaw   = ry * rotateScale * 0.0001;
  const double roll  = -rz * rotateScale * 0.0001;
  if(pitch != 0.0 || yaw != 0.0 || roll != 0.0)
  {
    const glm::dvec3 currentForward = glm::normalize(camera.ctr - camera.eye);
    const glm::dvec3 currentRight   = glm::normalize(glm::cross(currentForward, camera.up));
    const glm::dvec3 currentUp      = glm::normalize(glm::cross(currentRight, currentForward));

    const glm::dquat yawRot   = glm::angleAxis(yaw, currentUp);
    const glm::dquat pitchRot = glm::angleAxis(pitch, currentRight);
    const glm::dquat rollRot  = glm::angleAxis(roll, currentForward);
    const glm::dquat rot      = rollRot * yawRot * pitchRot;
    const glm::dvec3 eyeVec   = camera.eye - camera.ctr;
    camera.eye                = camera.ctr + rot * eyeVec;
    camera.up                 = glm::normalize(rot * camera.up);
  }

  m_cameraManip->setCamera(camera, true);
}

void ActiveStereoQuadBufferPresenter::applySpaceMouseButtons(uint32_t buttonMask)
{
  const uint32_t pressed = buttonMask & ~m_spaceMouseButtonMask;
  m_spaceMouseButtonMask = buttonMask;

  // OSG VRPN path uses button 0 as reset. Keep the same behavior here.
  if((pressed & 0x1U) != 0 && m_cameraManip && m_spaceMouseHomeValid)
  {
    m_cameraManip->setCamera(m_spaceMouseHomeCamera, true);
    LOGI(
        "Active stereo presenter: SpaceMouse button 0 reset camera to home "
        "position.\n");
  }
}

void ActiveStereoQuadBufferPresenter::releaseInterop()
{
  if(!m_interopGl)
    return;

  HDC   deviceContext = static_cast<HDC>(m_deviceContext);
  HGLRC glContext     = static_cast<HGLRC>(m_glContext);
  if(deviceContext == nullptr || glContext == nullptr || wglMakeCurrent(deviceContext, glContext) == FALSE)
  {
    m_interopGl.reset();
    return;
  }

  glFinish();
  for(auto& slot : m_interopGl->slots)
  {
    if(m_interopGl->deleteFramebuffers)
      m_interopGl->deleteFramebuffers(static_cast<GLsizei>(slot.framebuffers.size()), slot.framebuffers.data());
    glDeleteTextures(static_cast<GLsizei>(slot.textures.size()), slot.textures.data());
    if(m_interopGl->deleteMemoryObjects)
      m_interopGl->deleteMemoryObjects(static_cast<GLsizei>(slot.memoryObjects.size()), slot.memoryObjects.data());
    if(m_interopGl->deleteSemaphores)
    {
      if(slot.vulkanReady != 0)
        m_interopGl->deleteSemaphores(1, &slot.vulkanReady);
      if(slot.openglDone != 0)
        m_interopGl->deleteSemaphores(1, &slot.openglDone);
    }
  }

  m_interopGl.reset();
  wglMakeCurrent(nullptr, nullptr);
}

bool ActiveStereoQuadBufferPresenter::presentInterop(StereoInteropVk& interop, uint64_t frameNumber)
{
  if(m_failed || !interop.isAvailable() || !interop.hasProducedFrame(frameNumber))
    return false;

  const VkExtent2D size = interop.getSize();
  if(size.width == 0 || size.height == 0)
    return false;

  HWND windowHandle = static_cast<HWND>(m_windowHandle);
  if(windowHandle != nullptr)
  {
    pumpStereoWindowMessages(windowHandle);
    if(GetPropA(windowHandle, kCloseRequestedProp) != nullptr)
    {
      LOGI(
          "Active stereo presenter: quad-buffer window closed; disabling "
          "active stereo presentation.\n");
      if(m_closeCallback)
        m_closeCallback();
      deinit();
      m_failed = true;
      return false;
    }
  }

  if(m_windowHandle == nullptr || m_size.width != size.width || m_size.height != size.height)
  {
    if(!init(size))
      return false;
    windowHandle = static_cast<HWND>(m_windowHandle);
  }

  HDC   deviceContext = static_cast<HDC>(m_deviceContext);
  HGLRC glContext     = static_cast<HGLRC>(m_glContext);
  if(wglMakeCurrent(deviceContext, glContext) == FALSE)
    return false;

  if(!m_interopGl || m_interopGl->generation != interop.getGeneration())
  {
    if(m_interopGl)
    {
      wglMakeCurrent(nullptr, nullptr);
      releaseInterop();
      if(wglMakeCurrent(deviceContext, glContext) == FALSE)
        return false;
    }

    if(!hasOpenGlExtension("GL_EXT_memory_object") || !hasOpenGlExtension("GL_EXT_memory_object_win32")
       || !hasOpenGlExtension("GL_EXT_semaphore") || !hasOpenGlExtension("GL_EXT_semaphore_win32"))
    {
      LOGW(
          "Active stereo interop: required OpenGL external-object extensions "
          "are unavailable; using CPU fallback.\n");
      wglMakeCurrent(nullptr, nullptr);
      return false;
    }

    auto state                 = std::make_unique<InteropGlState>();
    state->createMemoryObjects = loadOpenGlFunction<PFNGLCREATEMEMORYOBJECTSEXTPROC>("glCreateMemoryObjectsEXT");
    state->deleteMemoryObjects = loadOpenGlFunction<PFNGLDELETEMEMORYOBJECTSEXTPROC>("glDeleteMemoryObjectsEXT");
    state->memoryObjectParameteriv = loadOpenGlFunction<PFNGLMEMORYOBJECTPARAMETERIVEXTPROC>("glMemoryObjectParameterivEXT");
    state->importMemoryWin32Handle = loadOpenGlFunction<PFNGLIMPORTMEMORYWIN32HANDLEEXTPROC>("glImportMemoryWin32HandleEXT");
    state->texStorageMem2D  = loadOpenGlFunction<PFNGLTEXSTORAGEMEM2DEXTPROC>("glTexStorageMem2DEXT");
    state->genSemaphores    = loadOpenGlFunction<PFNGLGENSEMAPHORESEXTPROC>("glGenSemaphoresEXT");
    state->deleteSemaphores = loadOpenGlFunction<PFNGLDELETESEMAPHORESEXTPROC>("glDeleteSemaphoresEXT");
    state->importSemaphoreWin32Handle =
        loadOpenGlFunction<PFNGLIMPORTSEMAPHOREWIN32HANDLEEXTPROC>("glImportSemaphoreWin32HandleEXT");
    state->waitSemaphore          = loadOpenGlFunction<PFNGLWAITSEMAPHOREEXTPROC>("glWaitSemaphoreEXT");
    state->signalSemaphore        = loadOpenGlFunction<PFNGLSIGNALSEMAPHOREEXTPROC>("glSignalSemaphoreEXT");
    state->genFramebuffers        = loadOpenGlFunction<PFNGLGENFRAMEBUFFERSPROC>("glGenFramebuffers");
    state->deleteFramebuffers     = loadOpenGlFunction<PFNGLDELETEFRAMEBUFFERSPROC>("glDeleteFramebuffers");
    state->bindFramebuffer        = loadOpenGlFunction<PFNGLBINDFRAMEBUFFERPROC>("glBindFramebuffer");
    state->framebufferTexture2D   = loadOpenGlFunction<PFNGLFRAMEBUFFERTEXTURE2DPROC>("glFramebufferTexture2D");
    state->checkFramebufferStatus = loadOpenGlFunction<PFNGLCHECKFRAMEBUFFERSTATUSPROC>("glCheckFramebufferStatus");
    state->blitFramebuffer        = loadOpenGlFunction<PFNGLBLITFRAMEBUFFERPROC>("glBlitFramebuffer");

    if(!state->createMemoryObjects || !state->deleteMemoryObjects || !state->memoryObjectParameteriv || !state->importMemoryWin32Handle
       || !state->texStorageMem2D || !state->genSemaphores || !state->deleteSemaphores || !state->importSemaphoreWin32Handle
       || !state->waitSemaphore || !state->signalSemaphore || !state->genFramebuffers || !state->deleteFramebuffers
       || !state->bindFramebuffer || !state->framebufferTexture2D || !state->checkFramebufferStatus || !state->blitFramebuffer)
    {
      LOGW(
          "Active stereo interop: failed to load required OpenGL entry "
          "points; using CPU fallback.\n");
      wglMakeCurrent(nullptr, nullptr);
      return false;
    }

    state->slots.resize(interop.getRingSize());
    state->generation    = interop.getGeneration();
    state->size          = size;
    bool importSucceeded = true;
    while(glGetError() != GL_NO_ERROR)
    {
    }

    for(uint32_t slotIndex = 0; slotIndex < interop.getRingSize() && importSucceeded; ++slotIndex)
    {
      auto&       slot     = state->slots[slotIndex];
      const auto& exported = interop.getExportedSlot(slotIndex);

      state->createMemoryObjects(static_cast<GLsizei>(slot.memoryObjects.size()), slot.memoryObjects.data());
      glGenTextures(static_cast<GLsizei>(slot.textures.size()), slot.textures.data());
      state->genFramebuffers(static_cast<GLsizei>(slot.framebuffers.size()), slot.framebuffers.data());

      for(uint32_t eyeIndex = 0; eyeIndex < StereoInteropVk::kEyeCount; ++eyeIndex)
      {
        const GLint dedicated = GL_TRUE;
        state->memoryObjectParameteriv(slot.memoryObjects[eyeIndex], GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
        state->importMemoryWin32Handle(slot.memoryObjects[eyeIndex], exported.eyes[eyeIndex].allocationSize,
                                       GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, exported.eyes[eyeIndex].memoryHandle);

        glBindTexture(GL_TEXTURE_2D, slot.textures[eyeIndex]);
        state->texStorageMem2D(GL_TEXTURE_2D, 1, GL_RGBA8, static_cast<GLsizei>(size.width),
                               static_cast<GLsizei>(size.height), slot.memoryObjects[eyeIndex], 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        state->bindFramebuffer(GL_READ_FRAMEBUFFER, slot.framebuffers[eyeIndex]);
        state->framebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, slot.textures[eyeIndex], 0);
        if(state->checkFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
          importSucceeded = false;
      }

      state->genSemaphores(1, &slot.vulkanReady);
      state->genSemaphores(1, &slot.openglDone);
      state->importSemaphoreWin32Handle(slot.vulkanReady, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, exported.vulkanReadyHandle);
      state->importSemaphoreWin32Handle(slot.openglDone, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, exported.openglDoneHandle);

      if(glGetError() != GL_NO_ERROR)
        importSucceeded = false;
    }
    state->bindFramebuffer(GL_READ_FRAMEBUFFER, 0);

    if(!importSucceeded)
    {
      LOGW(
          "Active stereo interop: OpenGL could not import Vulkan "
          "images/semaphores; using CPU fallback.\n");
      m_interopGl = std::move(state);
      wglMakeCurrent(nullptr, nullptr);
      releaseInterop();
      return false;
    }

    m_interopGl = std::move(state);
    LOGI(
        "Active stereo interop: Vulkan images imported directly into OpenGL; "
        "CPU readback is bypassed.\n");
  }

  const uint32_t slotIndex = static_cast<uint32_t>(frameNumber % interop.getRingSize());
  auto&          slot      = m_interopGl->slots[slotIndex];
  const std::array<GLenum, StereoInteropVk::kEyeCount> layouts = {GL_LAYOUT_GENERAL_EXT, GL_LAYOUT_GENERAL_EXT};
  m_interopGl->waitSemaphore(slot.vulkanReady, 0, nullptr, StereoInteropVk::kEyeCount, slot.textures.data(), layouts.data());

  glDisable(GL_FRAMEBUFFER_SRGB);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glViewport(0, 0, static_cast<GLsizei>(size.width), static_cast<GLsizei>(size.height));
  m_interopGl->bindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

  const std::array<GLenum, StereoInteropVk::kEyeCount> drawBuffers = {GL_BACK_LEFT, GL_BACK_RIGHT};
  for(uint32_t eyeIndex = 0; eyeIndex < StereoInteropVk::kEyeCount; ++eyeIndex)
  {
    m_interopGl->bindFramebuffer(GL_READ_FRAMEBUFFER, slot.framebuffers[eyeIndex]);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glDrawBuffer(drawBuffers[eyeIndex]);
    m_interopGl->blitFramebuffer(0, 0, static_cast<GLint>(size.width), static_cast<GLint>(size.height), 0,
                                 static_cast<GLint>(size.height), static_cast<GLint>(size.width), 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  }

  m_interopGl->signalSemaphore(slot.openglDone, 0, nullptr, StereoInteropVk::kEyeCount, slot.textures.data(), layouts.data());
  glFlush();
  SwapBuffers(deviceContext);
  interop.notifyOpenGlPresented(frameNumber);

  m_interopGl->bindFramebuffer(GL_READ_FRAMEBUFFER, 0);
  wglMakeCurrent(nullptr, nullptr);

  if(!m_loggedFirstPresent)
  {
    LOGI(
        "Active stereo presenter: GPU-shared Vulkan images -> "
        "GL_BACK_LEFT/GL_BACK_RIGHT, quad-buffer preserved.\n");
    m_loggedFirstPresent = true;
  }
  return true;
}

void ActiveStereoQuadBufferPresenter::deinit()
{
  releaseInterop();
  HDC   deviceContext = static_cast<HDC>(m_deviceContext);
  HGLRC glContext     = static_cast<HGLRC>(m_glContext);
  HWND  windowHandle  = static_cast<HWND>(m_windowHandle);

  if(glContext != nullptr)
  {
    if(deviceContext != nullptr)
      wglMakeCurrent(deviceContext, glContext);
    if(m_glTexture != 0)
    {
      glDeleteTextures(1, &m_glTexture);
      m_glTexture = 0;
    }
    m_glTextureSize  = {0, 0};
    m_glTextureFloat = false;
    if(wglGetCurrentContext() == glContext)
      wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(glContext);
    m_glContext = nullptr;
  }

  if(windowHandle != nullptr)
  {
    RemovePropA(windowHandle, kCloseRequestedProp);
    if(deviceContext != nullptr)
    {
      ReleaseDC(windowHandle, deviceContext);
      m_deviceContext = nullptr;
    }
    DestroyWindow(windowHandle);
    m_windowHandle = nullptr;
  }

  m_size               = {0, 0};
  m_failed             = false;
  m_loggedFirstPresent = false;
  m_leftMouseDown = m_middleMouseDown = m_rightMouseDown = false;
  m_rawInputRegistered                                   = false;
  m_loggedSpaceMouseSample                               = false;
}

bool ActiveStereoQuadBufferPresenter::init(const VkExtent2D& size)
{
  deinit();

  if(size.width == 0 || size.height == 0)
    return false;

  if(!registerStereoWindowClass())
  {
    LOGE(
        "Active stereo presenter: failed to register the native WGL stereo "
        "window class.\n");
    m_failed = true;
    return false;
  }

  HWND windowHandle = CreateWindowExA(0, kStereoWindowClass, "CurvedStereoGS active stereo output", WS_POPUP | WS_VISIBLE, 0, 0,
                                      static_cast<int>(size.width), static_cast<int>(size.height), nullptr, nullptr,
                                      GetModuleHandleA(nullptr), this);
  if(windowHandle == nullptr)
  {
    LOGE(
        "Active stereo presenter: failed to create the native WGL stereo "
        "window.\n");
    m_failed = true;
    return false;
  }

  HDC deviceContext = GetDC(windowHandle);
  if(deviceContext == nullptr)
  {
    LOGE(
        "Active stereo presenter: failed to get the native WGL device "
        "context.\n");
    DestroyWindow(windowHandle);
    m_failed = true;
    return false;
  }

  PIXELFORMATDESCRIPTOR stereoFormat{};
  bool                  stereoFormatIsFloat = false;
  const int             pixelFormat         = chooseStereoPixelFormat(deviceContext, stereoFormat, stereoFormatIsFloat);
  if(pixelFormat == 0)
  {
    LOGE(
        "Active stereo presenter: no Windows/WGL pixel format supports "
        "PFD_STEREO + PFD_DOUBLEBUFFER + OpenGL.\n");
    logStereoPixelFormatSummary(deviceContext);
    ReleaseDC(windowHandle, deviceContext);
    DestroyWindow(windowHandle);
    m_failed = true;
    return false;
  }

  if(SetPixelFormat(deviceContext, pixelFormat, &stereoFormat) == FALSE)
  {
    LOGE(
        "Active stereo presenter: SetPixelFormat failed for stereo pixel "
        "format %d.\n",
        pixelFormat);
    ReleaseDC(windowHandle, deviceContext);
    DestroyWindow(windowHandle);
    m_failed = true;
    return false;
  }

  HGLRC glContext = wglCreateContext(deviceContext);
  if(glContext == nullptr || wglMakeCurrent(deviceContext, glContext) == FALSE)
  {
    LOGE(
        "Active stereo presenter: failed to create or activate the native WGL "
        "OpenGL context.\n");
    if(glContext != nullptr)
      wglDeleteContext(glContext);
    ReleaseDC(windowHandle, deviceContext);
    DestroyWindow(windowHandle);
    m_failed = true;
    return false;
  }

  GLboolean hasStereo = GL_FALSE;
  glGetBooleanv(GL_STEREO, &hasStereo);
  if(hasStereo != GL_TRUE)
  {
    LOGE(
        "Active stereo presenter: WGL context was created, but "
        "glGetBooleanv(GL_STEREO) returned false.\n");
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(glContext);
    ReleaseDC(windowHandle, deviceContext);
    DestroyWindow(windowHandle);
    m_failed = true;
    return false;
  }

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_DITHER);
  glDisable(GL_BLEND);
  glDisable(GL_LIGHTING);
  glDisable(GL_FRAMEBUFFER_SRGB);
  disableFloatColorClampingIfAvailable();
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  wglMakeCurrent(nullptr, nullptr);

  m_windowHandle  = windowHandle;
  m_deviceContext = deviceContext;
  m_glContext     = glContext;
  m_size          = size;
  updateCameraWindowSize(size.width, size.height);
  registerRawInput(windowHandle);
  SetForegroundWindow(windowHandle);
  SetFocus(windowHandle);

  GLint redBits   = 0;
  GLint greenBits = 0;
  GLint blueBits  = 0;
  GLint alphaBits = 0;
  wglMakeCurrent(deviceContext, glContext);
  glGetIntegerv(GL_RED_BITS, &redBits);
  glGetIntegerv(GL_GREEN_BITS, &greenBits);
  glGetIntegerv(GL_BLUE_BITS, &blueBits);
  glGetIntegerv(GL_ALPHA_BITS, &alphaBits);
  wglMakeCurrent(nullptr, nullptr);

  LOGI(
      "Active stereo presenter: native WGL quad-buffer window created at "
      "%ux%u, pixelFormat=%d, pixelType=%s, colorBits=%u, alphaBits=%u, "
      "GL rgbaBits=%d/%d/%d/%d. "
      "Left eye -> GL_BACK_LEFT, right eye -> GL_BACK_RIGHT.\n",
      size.width, size.height, pixelFormat, stereoFormatIsFloat ? "RGBA_FLOAT" : "RGBA",
      uint32_t(stereoFormat.cColorBits), uint32_t(stereoFormat.cAlphaBits), redBits, greenBits, blueBits, alphaBits);
  return true;
}

void ActiveStereoQuadBufferPresenter::drawEye(uint32_t width, uint32_t height, unsigned int drawBuffer, const std::vector<uint8_t>& rgba)
{
  glDrawBuffer(drawBuffer);
  glClear(GL_COLOR_BUFFER_BIT);

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_DITHER);
  glDisable(GL_BLEND);
  glDisable(GL_LIGHTING);
  glDisable(GL_FRAMEBUFFER_SRGB);
  disableFloatColorClampingIfAvailable();
  glDisable(GL_TEXTURE_2D);
  glColor4f(1.0F, 1.0F, 1.0F, 1.0F);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glPixelTransferf(GL_RED_SCALE, 1.0F);
  glPixelTransferf(GL_GREEN_SCALE, 1.0F);
  glPixelTransferf(GL_BLUE_SCALE, 1.0F);
  glPixelTransferf(GL_ALPHA_SCALE, 1.0F);
  glPixelTransferf(GL_RED_BIAS, 0.0F);
  glPixelTransferf(GL_GREEN_BIAS, 0.0F);
  glPixelTransferf(GL_BLUE_BIAS, 0.0F);
  glPixelTransferf(GL_ALPHA_BIAS, 0.0F);

  glRasterPos2i(0, static_cast<GLint>(height));
  glPixelZoom(1.0F, -1.0F);
  glDrawPixels(static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
  glPixelZoom(1.0F, 1.0F);
}

void ActiveStereoQuadBufferPresenter::drawEye(uint32_t width, uint32_t height, unsigned int drawBuffer, const std::vector<float>& rgba)
{
  glDrawBuffer(drawBuffer);
  glClear(GL_COLOR_BUFFER_BIT);

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_DITHER);
  glDisable(GL_BLEND);
  glDisable(GL_LIGHTING);
  glDisable(GL_FRAMEBUFFER_SRGB);
  disableFloatColorClampingIfAvailable();
  glDisable(GL_TEXTURE_2D);
  glColor4f(1.0F, 1.0F, 1.0F, 1.0F);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glPixelTransferf(GL_RED_SCALE, 1.0F);
  glPixelTransferf(GL_GREEN_SCALE, 1.0F);
  glPixelTransferf(GL_BLUE_SCALE, 1.0F);
  glPixelTransferf(GL_ALPHA_SCALE, 1.0F);
  glPixelTransferf(GL_RED_BIAS, 0.0F);
  glPixelTransferf(GL_GREEN_BIAS, 0.0F);
  glPixelTransferf(GL_BLUE_BIAS, 0.0F);
  glPixelTransferf(GL_ALPHA_BIAS, 0.0F);

  glRasterPos2i(0, static_cast<GLint>(height));
  glPixelZoom(1.0F, -1.0F);
  glDrawPixels(static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA, GL_FLOAT, rgba.data());
  glPixelZoom(1.0F, 1.0F);
}

bool ActiveStereoQuadBufferPresenter::present(const VkExtent2D&           size,
                                              const std::vector<uint8_t>& leftRgba,
                                              const std::vector<uint8_t>& rightRgba)
{
  if(m_failed || size.width == 0 || size.height == 0)
    return false;

  const size_t eyeBytes = static_cast<size_t>(size.width) * static_cast<size_t>(size.height) * 4U;
  if(leftRgba.size() != eyeBytes || rightRgba.size() != eyeBytes)
    return false;

  HWND windowHandle = static_cast<HWND>(m_windowHandle);
  if(windowHandle != nullptr)
  {
    pumpStereoWindowMessages(windowHandle);
    if(GetPropA(windowHandle, kCloseRequestedProp) != nullptr)
    {
      LOGI(
          "Active stereo presenter: quad-buffer window closed; disabling "
          "active stereo presentation.\n");
      if(m_closeCallback)
        m_closeCallback();
      deinit();
      m_failed = true;
      return false;
    }
  }

  if(m_windowHandle == nullptr || m_size.width != size.width || m_size.height != size.height)
  {
    if(!init(size))
      return false;
    windowHandle = static_cast<HWND>(m_windowHandle);
  }

  HDC   deviceContext = static_cast<HDC>(m_deviceContext);
  HGLRC glContext     = static_cast<HGLRC>(m_glContext);
  if(wglMakeCurrent(deviceContext, glContext) == FALSE)
    return false;

  glDisable(GL_FRAMEBUFFER_SRGB);
  disableFloatColorClampingIfAvailable();
  glViewport(0, 0, static_cast<GLsizei>(size.width), static_cast<GLsizei>(size.height));
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(0.0, static_cast<double>(size.width), 0.0, static_cast<double>(size.height), -1.0, 1.0);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();

  drawEye(size.width, size.height, GL_BACK_LEFT, leftRgba);
  drawEye(size.width, size.height, GL_BACK_RIGHT, rightRgba);

  SwapBuffers(deviceContext);
  wglMakeCurrent(nullptr, nullptr);

  if(!m_loggedFirstPresent)
  {
    LOGI(
        "Active stereo presenter: presenting Vulkan display-ready RGBA8 "
        "left/right frames through native WGL OpenGL quad buffers.\n");
    m_loggedFirstPresent = true;
  }

  return true;
}

bool ActiveStereoQuadBufferPresenter::present(const VkExtent2D& size, const std::vector<float>& leftRgba, const std::vector<float>& rightRgba)
{
  if(m_failed || size.width == 0 || size.height == 0)
    return false;

  const size_t eyeFloats = static_cast<size_t>(size.width) * static_cast<size_t>(size.height) * 4U;
  if(leftRgba.size() != eyeFloats || rightRgba.size() != eyeFloats)
    return false;

  HWND windowHandle = static_cast<HWND>(m_windowHandle);
  if(windowHandle != nullptr)
  {
    pumpStereoWindowMessages(windowHandle);
    if(GetPropA(windowHandle, kCloseRequestedProp) != nullptr)
    {
      LOGI(
          "Active stereo presenter: quad-buffer window closed; disabling "
          "active stereo presentation.\n");
      if(m_closeCallback)
        m_closeCallback();
      deinit();
      m_failed = true;
      return false;
    }
  }

  if(m_windowHandle == nullptr || m_size.width != size.width || m_size.height != size.height)
  {
    if(!init(size))
      return false;
    windowHandle = static_cast<HWND>(m_windowHandle);
  }

  HDC   deviceContext = static_cast<HDC>(m_deviceContext);
  HGLRC glContext     = static_cast<HGLRC>(m_glContext);
  if(wglMakeCurrent(deviceContext, glContext) == FALSE)
    return false;

  glDisable(GL_FRAMEBUFFER_SRGB);
  disableFloatColorClampingIfAvailable();
  glViewport(0, 0, static_cast<GLsizei>(size.width), static_cast<GLsizei>(size.height));
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(0.0, static_cast<double>(size.width), 0.0, static_cast<double>(size.height), -1.0, 1.0);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();

  drawEye(size.width, size.height, GL_BACK_LEFT, leftRgba);
  drawEye(size.width, size.height, GL_BACK_RIGHT, rightRgba);

  SwapBuffers(deviceContext);
  wglMakeCurrent(nullptr, nullptr);

  if(!m_loggedFirstPresent)
  {
    LOGI(
        "Active stereo presenter: presenting Vulkan native floating-point "
        "left/right frames through native WGL OpenGL quad buffers.\n");
    m_loggedFirstPresent = true;
  }

  return true;
}

#else

void ActiveStereoQuadBufferPresenter::handleGlfwWindowClose(GLFWwindow* window)
{
  auto* presenter = static_cast<ActiveStereoQuadBufferPresenter*>(glfwGetWindowUserPointer(window));
  if(presenter != nullptr && presenter->m_closeCallback)
    presenter->m_closeCallback();
}

void ActiveStereoQuadBufferPresenter::handleGlfwKey(GLFWwindow* window, int key, int, int action, int)
{
  if(action != GLFW_PRESS)
    return;

  auto* presenter = static_cast<ActiveStereoQuadBufferPresenter*>(glfwGetWindowUserPointer(window));
  if(presenter == nullptr)
    return;

  if(key == GLFW_KEY_ESCAPE)
  {
    if(presenter->m_closeCallback)
      presenter->m_closeCallback();
    return;
  }

  if((key == GLFW_KEY_0 || key == GLFW_KEY_KP_0) && presenter->m_stereoToggleCallback)
  {
    presenter->m_stereoToggleCallback();
  }
}

void ActiveStereoQuadBufferPresenter::handleGlfwFramebufferSize(GLFWwindow* window, int width, int height)
{
  auto* presenter = static_cast<ActiveStereoQuadBufferPresenter*>(glfwGetWindowUserPointer(window));
  if(presenter != nullptr && width > 0 && height > 0)
    presenter->updateCameraWindowSize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
}

bool ActiveStereoQuadBufferPresenter::presentInterop(StereoInteropVk&, uint64_t)
{
  return false;
}

void ActiveStereoQuadBufferPresenter::releaseInterop()
{
  m_interopGl.reset();
}

void ActiveStereoQuadBufferPresenter::deinit()
{
  if(m_window != nullptr)
  {
    if(glfwGetCurrentContext() == m_window)
      glfwMakeContextCurrent(nullptr);
    glfwDestroyWindow(m_window);
    m_window = nullptr;
  }
  m_size               = {0, 0};
  m_failed             = false;
  m_loggedFirstPresent = false;
}

bool ActiveStereoQuadBufferPresenter::init(const VkExtent2D& size)
{
  deinit();

  if(size.width == 0 || size.height == 0)
    return false;

  glfwDefaultWindowHints();
  glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
  glfwWindowHint(GLFW_STEREO, GLFW_TRUE);
  glfwWindowHint(GLFW_DOUBLEBUFFER, GLFW_TRUE);
  glfwWindowHint(GLFW_RED_BITS, 8);
  glfwWindowHint(GLFW_GREEN_BITS, 8);
  glfwWindowHint(GLFW_BLUE_BITS, 8);
  glfwWindowHint(GLFW_ALPHA_BITS, 8);
  glfwWindowHint(GLFW_DEPTH_BITS, 0);
  glfwWindowHint(GLFW_STENCIL_BITS, 0);
  glfwWindowHint(GLFW_AUX_BUFFERS, 0);
  glfwWindowHint(GLFW_SAMPLES, 0);
  glfwWindowHint(GLFW_SRGB_CAPABLE, GLFW_FALSE);
  glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_FALSE);

  GLFWmonitor*       fullscreenMonitor = nullptr;
  const GLFWvidmode* fullscreenMode    = nullptr;
  int                monitorCount      = 0;
  GLFWmonitor**      monitors          = glfwGetMonitors(&monitorCount);
  for(int monitorIndex = 0; monitorIndex < monitorCount; ++monitorIndex)
  {
    const GLFWvidmode* mode = glfwGetVideoMode(monitors[monitorIndex]);
    if(mode != nullptr && mode->width == static_cast<int>(size.width) && mode->height == static_cast<int>(size.height))
    {
      fullscreenMonitor = monitors[monitorIndex];
      fullscreenMode    = mode;
      break;
    }
  }

  if(fullscreenMode != nullptr)
    glfwWindowHint(GLFW_REFRESH_RATE, fullscreenMode->refreshRate);
  else
    glfwWindowHint(GLFW_DECORATED, GLFW_TRUE);

  m_window = glfwCreateWindow(static_cast<int>(size.width), static_cast<int>(size.height), "CurvedStereoGS active stereo output",
                              fullscreenMonitor, nullptr);
  if(m_window == nullptr)
  {
    LOGE(
        "Active stereo presenter: failed to create an OpenGL quad-buffer "
        "stereo window. "
        "Enable stereo in the driver and use a display mode that exposes a "
        "stereo pixel format.\n");
    m_failed = true;
    return false;
  }

  glfwSetWindowUserPointer(m_window, this);
  glfwSetWindowCloseCallback(m_window, handleGlfwWindowClose);
  glfwSetKeyCallback(m_window, handleGlfwKey);
  glfwSetFramebufferSizeCallback(m_window, handleGlfwFramebufferSize);

  glfwMakeContextCurrent(m_window);
  if(glfwGetWindowAttrib(m_window, GLFW_STEREO) != GLFW_TRUE)
  {
    LOGE(
        "Active stereo presenter: OpenGL window was created without "
        "GLFW_STEREO support; quad-buffer output is unavailable.\n");
    glfwMakeContextCurrent(nullptr);
    deinit();
    m_failed = true;
    return false;
  }

  glfwSwapInterval(1);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_DITHER);
  glDisable(GL_BLEND);
  glDisable(GL_LIGHTING);
  glDisable(GL_FRAMEBUFFER_SRGB);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glfwMakeContextCurrent(nullptr);

  m_size = size;
  updateCameraWindowSize(size.width, size.height);
  LOGI(
      "Active stereo presenter: OpenGL quad-buffer window created at %ux%u. "
      "GLFW stereo=%d, fullscreen=%d. Left eye -> GL_BACK_LEFT, right eye -> "
      "GL_BACK_RIGHT.\n",
      size.width, size.height, glfwGetWindowAttrib(m_window, GLFW_STEREO), fullscreenMonitor != nullptr ? 1 : 0);
  return true;
}

void ActiveStereoQuadBufferPresenter::drawEye(uint32_t width, uint32_t height, unsigned int drawBuffer, const std::vector<uint8_t>& rgba)
{
  glDrawBuffer(drawBuffer);
  glClear(GL_COLOR_BUFFER_BIT);
  glRasterPos2i(0, static_cast<int>(height));
  glPixelZoom(1.0F, -1.0F);
  glDrawPixels(static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
  glPixelZoom(1.0F, 1.0F);
}

void ActiveStereoQuadBufferPresenter::drawEye(uint32_t width, uint32_t height, unsigned int drawBuffer, const std::vector<float>& rgba)
{
  glDrawBuffer(drawBuffer);
  glClear(GL_COLOR_BUFFER_BIT);
  glRasterPos2i(0, static_cast<int>(height));
  glPixelZoom(1.0F, -1.0F);
  glDrawPixels(static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA, GL_FLOAT, rgba.data());
  glPixelZoom(1.0F, 1.0F);
}

bool ActiveStereoQuadBufferPresenter::present(const VkExtent2D&           size,
                                              const std::vector<uint8_t>& leftRgba,
                                              const std::vector<uint8_t>& rightRgba)
{
  if(m_failed || size.width == 0 || size.height == 0)
    return false;

  const size_t eyeBytes = static_cast<size_t>(size.width) * static_cast<size_t>(size.height) * 4U;
  if(leftRgba.size() != eyeBytes || rightRgba.size() != eyeBytes)
    return false;

  glfwPollEvents();

  if(m_window != nullptr && glfwWindowShouldClose(m_window))
  {
    LOGI(
        "Active stereo presenter: quad-buffer window closed; disabling active "
        "stereo presentation.\n");
    if(m_closeCallback)
      m_closeCallback();
    deinit();
    m_failed = true;
    return false;
  }

  if(m_window == nullptr || m_size.width != size.width || m_size.height != size.height)
  {
    if(!init(size))
      return false;
  }

  glfwMakeContextCurrent(m_window);

  glDisable(GL_FRAMEBUFFER_SRGB);
  glViewport(0, 0, static_cast<GLsizei>(size.width), static_cast<GLsizei>(size.height));
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(0.0, static_cast<double>(size.width), 0.0, static_cast<double>(size.height), -1.0, 1.0);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();

  drawEye(size.width, size.height, GL_BACK_LEFT, leftRgba);
  drawEye(size.width, size.height, GL_BACK_RIGHT, rightRgba);

  glfwSwapBuffers(m_window);
  glfwMakeContextCurrent(nullptr);

  if(!m_loggedFirstPresent)
  {
    LOGI(
        "Active stereo presenter: presenting Vulkan display-ready RGBA8 "
        "left/right frames through OpenGL quad buffers.\n");
    m_loggedFirstPresent = true;
  }

  return true;
}

bool ActiveStereoQuadBufferPresenter::present(const VkExtent2D& size, const std::vector<float>& leftRgba, const std::vector<float>& rightRgba)
{
  if(m_failed || size.width == 0 || size.height == 0)
    return false;

  const size_t eyeFloats = static_cast<size_t>(size.width) * static_cast<size_t>(size.height) * 4U;
  if(leftRgba.size() != eyeFloats || rightRgba.size() != eyeFloats)
    return false;

  glfwPollEvents();

  if(m_window != nullptr && glfwWindowShouldClose(m_window))
  {
    LOGI(
        "Active stereo presenter: quad-buffer window closed; disabling active "
        "stereo presentation.\n");
    if(m_closeCallback)
      m_closeCallback();
    deinit();
    m_failed = true;
    return false;
  }

  if(m_window == nullptr || m_size.width != size.width || m_size.height != size.height)
  {
    if(!init(size))
      return false;
  }

  glfwMakeContextCurrent(m_window);

  glDisable(GL_FRAMEBUFFER_SRGB);
  glViewport(0, 0, static_cast<GLsizei>(size.width), static_cast<GLsizei>(size.height));
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(0.0, static_cast<double>(size.width), 0.0, static_cast<double>(size.height), -1.0, 1.0);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();

  drawEye(size.width, size.height, GL_BACK_LEFT, leftRgba);
  drawEye(size.width, size.height, GL_BACK_RIGHT, rightRgba);

  glfwSwapBuffers(m_window);
  glfwMakeContextCurrent(nullptr);

  if(!m_loggedFirstPresent)
  {
    LOGI(
        "Active stereo presenter: presenting Vulkan native floating-point "
        "left/right frames through OpenGL quad buffers.\n");
    m_loggedFirstPresent = true;
  }

  return true;
}

#endif

}  // namespace curved_stereo_gs
