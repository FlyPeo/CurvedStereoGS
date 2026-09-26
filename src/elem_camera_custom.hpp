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
    if(!manip) return;

    manip->updateAnim();

    // If ImGui is capturing mouse for another window/widget, ignore.
    if(ImGui::GetIO().WantCaptureMouse)
    {
      return;
    }

    nvutils::CameraManipulator::Inputs inputs;
    inputs.lmb   = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    inputs.rmb   = ImGui::IsMouseDown(ImGuiMouseButton_Right);
    inputs.mmb   = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
    inputs.ctrl  = ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl);
    inputs.shift = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift);
    inputs.alt   = ImGui::IsKeyDown(ImGuiKey_LeftAlt) || ImGui::IsKeyDown(ImGuiKey_RightAlt);
    ImVec2 mousePos = ImGui::GetMousePos();

    if(ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))
    {
      manip->setMousePosition({mousePos.x, mousePos.y});
    }

    if(ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0F) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 1.0F) || ImGui::IsMouseDragging(ImGuiMouseButton_Right, 1.0F))
    {
      manip->mouseMove({mousePos.x, mousePos.y}, inputs);
    }

    if(ImGui::GetIO().MouseWheel != 0.0F)
    {
      manip->wheel(ImGui::GetIO().MouseWheel * -10.0f, inputs);
    }
  }

private:
  std::function<bool()> m_disableCallback;
};

}  // namespace curved_stereo_gs
