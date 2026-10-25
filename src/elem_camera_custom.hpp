/*
 * Custom ElementCamera that can be disabled during comparison slider drag
 */

#pragma once

#include <nvapp/elem_camera.hpp>
#include <functional>

namespace curved_stereo_gs {

class ElementCameraCustom : public nvapp::ElementCamera
{
public:
  ElementCameraCustom(std::shared_ptr<nvutils::CameraManipulator> camera = nullptr)
      : ElementCamera(camera)
  {
  }

  // Set a callback to check if camera updates should be disabled
  void setDisableCallback(std::function<bool()> callback) { m_disableCallback = callback; }

  void onUIRender() override
  {
    if(m_disableCallback && m_disableCallback())
    {
      return;
    }

    auto manip = getCameraManipulator();
    if(!manip)
      return;

    // Reuse nvapp's viewport-aware camera controls: left-drag trackball orbit,
    // mouse pan/dolly, wheel zoom, and WASD translation (Shift/Ctrl adjust speed).
    nvapp::ElementCamera::updateCamera(manip, ImGui::FindWindowByName("Viewport"));
  }

private:
  std::function<bool()> m_disableCallback;
};

}  // namespace curved_stereo_gs
