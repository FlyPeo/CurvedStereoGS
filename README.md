# CurvedStereoGS

**曲面屏立体高斯泼溅渲染系统**（Curved-Screen Stereo Gaussian Splatting Renderer）

CurvedStereoGS 是基于 Vulkan 的 3D Gaussian Splatting 渲染项目，面向曲面显示和双目立体输出。项目在原有渲染基础上加入三段式曲面映射、Vulkan Multiview 双眼渲染、WGL 四缓冲主动立体输出，以及不依赖四缓冲显卡的 Vulkan Half-SBS 并排输出。Qt 控制台用于选择模型、标定曲面参数和切换输出模式。

代码结构与帧流程见 [架构说明](docs/architecture.md)。

## 项目结构

```text
CurvedStereoGS/
├─ src/                 渲染器 C++ 源码
│  ├─ qt/               Qt 控制台源码
│  └─ shaders/          Slang 着色器与共享头文件
├─ third_party/         构建所需的第三方源码及各自许可证
├─ cmake/               CMake 查找模块
├─ docs/                架构与控制台说明
├─ models/              本地场景目录（大型模型不上传）
└─ CMakeLists.txt        项目构建入口
```

第三方依赖的来源和版本见 [third_party/README.md](third_party/README.md)；Qt 控制台的使用说明见 [docs/qt-control-center.md](docs/qt-control-center.md)。

## 立体输出

| 模式 | 输出方式 | 要求 |
| --- | --- | --- |
| 自动 | 优先 WGL 四缓冲；不可用时切到 Vulkan 并排 | 普通 Vulkan 显卡即可运行回退路径 |
| 四缓冲 | 左右眼分别输出到 `GL_BACK_LEFT` / `GL_BACK_RIGHT` | 驱动和显示设备支持 WGL `PFD_STEREO` |
| Vulkan 并排 | Multiview 双眼图层在普通窗口中左右各占半屏 | 观看立体效果需支持 Half-SBS 的显示设备 |

按 `0` 键可在双眼视差与零视差之间切换。切换输出模式需在 Qt 控制台点击“应用并重启”。

## 构建与运行

Windows 环境需要 Visual Studio 2022、Vulkan SDK；构建 Qt 控制台还需要 Qt 6.5+ Widgets。仓库包含所需的源代码依赖，首次配置可能获取 Slang 工具链。将下面的 Qt 路径换成自己的安装位置：

```powershell
cmake -S . -B .codex-build -DDISABLE_DEFAULT_SCENE=ON -DCMAKE_PREFIX_PATH=D:/Qt/6.6.3/msvc2019_64 -DCURVED_STEREO_GS_BUILD_QT_CONTROL_CENTER=ON
cmake --build .codex-build --config Release --target CurvedStereoGS_ControlCenter --parallel 8
.\_bin\Release\CurvedStereoGS_ControlCenter.exe
```

在控制台选择 `.ply`、`.spz` 或 `.csgs` 场景文件，然后选择输出模式并启动渲染。旧 `.vkgs` 工程仍可读取。也可直接运行渲染器，例如：

```powershell
.\_bin\Release\CurvedStereoGS.exe --scene C:\path\to\scene.ply --stereoOutputMode 2
```

项目、构建目标、主渲染器和 Qt 控制台统一使用 **CurvedStereoGS** 名称。

`--stereoOutputMode` 的取值为 `0` 自动、`1` 仅 WGL 四缓冲、`2` 仅 Vulkan Half-SBS。直接构建渲染器时使用 `--target CurvedStereoGS`。仓库默认不上传大型模型、生成文件或本机运行配置；请自行将场景放入本地 `models/`，或使用绝对路径传给 `--scene`。

## 来源与许可

本项目扩展自 [NVIDIA Vulkan Gaussian Splatting 示例](https://github.com/nvpro-samples/vk_gaussian_splatting)。原始源码与第三方依赖的版权声明、许可文件保持不变。参见 [LICENSE](LICENSE) 与 [NOTICE.md](NOTICE.md)。
