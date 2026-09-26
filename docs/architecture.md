# Architecture

CurvedStereoGS separates the Qt control interface from the renderer. `CurvedStereoGS_ControlCenter.exe` starts `CurvedStereoGS.exe` as a child process and passes scene, curve-calibration, eye-separation, and output-mode parameters. The renderer owns its GLFW/Vulkan window and, when selected, the WGL quad-buffer window.

## Frame path

1. Gaussian splats are loaded from `.ply` or `.spz` files and rendered by Vulkan.
2. Vulkan Multiview renders left and right eyes into two layers of the presentation image.
3. The curved-screen pass maps the virtual flat view to the calibrated three-section physical screen.
4. Output mode selects one of two presentation paths:
   - **WGL quad-buffer** shares or reads back both eye images and presents them to `GL_BACK_LEFT` and `GL_BACK_RIGHT`.
   - **Vulkan Half-SBS** blits the two layers into the left and right halves of an ordinary swapchain image. This path does not require WGL stereo pixel formats.
5. **Auto** begins with quad-buffer presentation and switches to Vulkan Half-SBS if no usable WGL stereo format is available.

The four-buffer path remains available; the Vulkan path is an alternative for machines without quad-buffer support. Half-SBS requires a compatible stereoscopic display mode for 3D viewing; on an ordinary monitor it shows two images side by side.

## Main components

| Location | Role |
| --- | --- |
| `src/main.cpp` | Startup options, output selection, frame presentation |
| `src/gaussian_splatting.cpp` | Rendering, Multiview targets, curved-screen processing |
| `src/active_stereo_presenter.cpp` | WGL quad-buffer presentation |
| `src/stereo_interop_vk.cpp` | Vulkan/OpenGL shared-image interop |
| `src/qt/qt_control_center.cpp` | Qt control panel and renderer process management |
| `src/shaders/` | Vulkan/Slang shaders and shared shader headers |
| `third_party/` | Vendored build dependencies and their license files |
| `src/curved_stereo_gs_project_*.cpp` | `.csgs` scene project serialization |

New projects are written as `.csgs`. Loading legacy `.vkgs` project files remains supported.
