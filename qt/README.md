# CurvedStereoGS · Qt 控制台

`CurvedStereoGS_ControlCenter` 是 CurvedStereoGS 的 Qt 6.5+ Widgets 控制界面。它通过 `QProcess` 启动同目录的 `CurvedStereoGS.exe`，负责场景选择、曲面几何标定、双眼间距、输出分辨率和立体输出模式。新设置保存在 `CurvedStereoGS/ControlCenter`，首次启动时会读取旧版设置；修改参数后点击“应用并重启”。

渲染器继续使用自己的 Vulkan/GLFW 渲染循环。四缓冲模式通过 WGL 的 `GL_BACK_LEFT`、`GL_BACK_RIGHT` 输出；Vulkan 并排模式把 Multiview 的双眼图层直接复制到普通交换链的左右半屏，不需要 WGL 四缓冲或 CPU 回读。自动模式先尝试四缓冲，若驱动不提供 `PFD_STEREO`，便切换为并排模式。控制台关闭时会停止它启动的渲染进程。

## Windows 构建

需要 Visual Studio 2022、Vulkan SDK 和 Qt 6.5+ Widgets。示例（根据实际 Qt 安装路径调整）：

```powershell
cmake -S . -B .codex-build -DDISABLE_DEFAULT_SCENE=ON -DCMAKE_PREFIX_PATH=D:/Qt/6.6.3/msvc2019_64 -DCURVED_STEREO_GS_BUILD_QT_CONTROL_CENTER=ON
cmake --build .codex-build --config Release --target CurvedStereoGS_ControlCenter --parallel 8
```

构建目标会先编译 `CurvedStereoGS`，再把 `CurvedStereoGS_ControlCenter.exe` 和需要的 Qt 运行库部署到 `_bin/Release`。启动 `_bin/Release/CurvedStereoGS_ControlCenter.exe` 即可。未安装 Qt 时仍可单独构建和运行 `CurvedStereoGS`。

## 运行限制

- 没有指定场景时，渲染器尝试加载自身的默认模型。若默认模型不存在，仍可启动空场景，但需要手动选择 `.ply`、`.spz` 或 `.csgs` 文件查看内容。旧 `.vkgs` 工程仍可读取。
- 四缓冲主动立体仍取决于显卡驱动、显示器和 Windows WGL 是否提供 `PFD_STEREO`。Vulkan 并排模式不依赖此能力，但输出为 Half-SBS：每只眼占窗口宽度的一半，需要支持左右并排格式的立体显示设备才能看到立体效果；普通显示器上会看到两幅画面。
- 命令行可用 `--stereoOutputMode 0|1|2` 选择自动、仅 WGL 四缓冲或仅 Vulkan 并排。按 `0` 键仍可在立体视差与零视差之间切换。
- Qt 控制台与渲染器是同一项目中的两个进程。参数在启动或重启时生效，运行中相机操作仍在渲染窗口完成。
