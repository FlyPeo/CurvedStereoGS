# 自研边界说明（Attribution & Contribution Boundary）

本项目基于 NVIDIA 的开源示例 **vk_gaussian_splatting**（nvpro_core2 生态，Apache 2.0 许可）深度扩展而成。
依照许可证要求，原版权声明、`LICENSE` 与 `NOTICE.md` 均予保留；第三方依赖（nvpro_core2、spz、vrdx、
miniply 等）的许可证见 `third_party/` 各自目录。本文档明确划分上游基础与本项目的自研增量，
供代码审阅、论文写作与求职面试时引用。

## 一、上游基础（NVIDIA vk_gaussian_splatting 提供）

- Vulkan 1.4 上下文、nvapp 应用框架与元素体系（`third_party/nvpro_core2/`）
- 3DGS 基础光栅管线骨架：顶点/网格着色器路径的原始实现、EWA 协方差投影与 splat 绘制的基线逻辑
- GPU 基数排序（vrdx）的接入方式、基础 GBuffer/描述符管理框架
- RTX 光线追踪管线基线、DLSS 集成点（本项目默认关闭）
- PlyLoaderAsync 异步加载框架

## 二、自研增量（本项目核心贡献）

### 1. 曲面屏三段式立体投影模型（全部自研）

- 三段式（平面-柱面-平面）物理屏参数化模型与标定参数体系（`src/parameters.h`）
- 主机侧离轴视锥推导 `computeCurvedScreenViewProjection` 与逐眼物理头位偏移
- GPU 侧逐像素反投影 Warp `src/shaders/post.comp.slang`（270 行，含柱面段射线求交、
  时域累积、立体显示映射）——上游不存在任何曲面校正代码

### 2. 双目 Multiview 立体渲染扩展（全部自研）

- 双层渲染目标（arrayLayers=2 / viewMask 0x3）的 GBuffer 与管线改造
- FrameInfo UBO 的 `stereo*` 系列字段与着色器端 `getStereoEyeIndex` 逐眼矩阵选择
  （`src/shaders/shaderio.h`，20 处立体相关扩展）
- `STEREO_MULTIVIEW_POST` 宏特化体系：同一套着色器覆盖曲面/平面 × 立体/单目四种模式

### 3. 跨 API 立体呈现链路（全部自研，约 3200 行）

- `src/active_stereo_presenter.cpp`（1785 行）：WGL 四缓冲主动立体窗口、
  `PFD_STEREO` 像素格式探测、GPU 共享与 CPU 回退双路径呈现
- `src/stereo_interop_vk.*`：Vulkan→OpenGL 外部内存 + 二值信号量互操作、三槽环形缓冲
- Half-SBS 并排输出与四缓冲失败时的**自动降级**机制
- `src/stereo_frame_stats.*`：三条呈现路径的帧级 p50/p95/p99 统计与 CSV 落盘

### 4. 动态 4DGS 序列播放（全部自研）

- `src/dynamic_splat_sequence.*`（385 行）：`.dgs` 关键帧池、2 GiB 预算滑动窗口 VRAM 流播
- `src/shaders/dynamic_animate.comp.slang`：关键帧插值 compute pass（先于排序执行）
- Dynamic3DGaussians 训练输出 → `.dgs` 的导出工具链（`tools/`）

### 5. 工程化与工具链（全部自研）

- Qt 6 控制台（`src/qt/qt_control_center.cpp`，685 行）：独立进程管理渲染器、
  渲染方式/立体模式双开关、标定参数持久化
- `.csgs` 场景工程格式的读写器（`src/curved_stereo_gs_project_*.cpp`）
- `src/hardware_support.*`：能力探测与**多厂商适配**（GPU_VENDOR 宏、mesh shader per-primitive
  回退、RTX 可选门控）
- 交互修复（Viewport 窗口接入 trackball/WASD）、ESC 退出、模式感知立体热键

### 6. 性能优化（全部自研，含实测数据）

- 排序时域复用（相机静止跳过全量排序）+ RGBA16F 帧缓冲：静态场景 p50 帧时
  **7.61ms → 2.14ms（-72%）**，见 `docs/optimization-log.md`
- Slang SPIR-V 磁盘缓存（源码哈希+宏集合为 key）：着色器启动 **3439ms → 8ms**
- 退出期 teardown 重编译守卫（修复偶发段错误）
- 便携运行包布局（`bin/CurvedStereoGS_Portable/`）与模型自动识别加载

## 三、量化概览

| 维度 | 数量 |
| --- | --- |
| 全新自研模块（不含对上游文件的修改） | 约 6100 行 C++ + 300 行 Slang |
| 其中呈现链路 + 控制台 + 动态序列 | 约 3200 + 685 + 385 行 |
| 上游骨架文件内的功能性改造 | Multiview 目标、排序复用、缓存、模式开关等（`gaussian_splatting.*`、`main.cpp`） |
| 实测性能成果 | 启动着色器阶段 -99.7%，静态帧时 p50 -72% |

## 四、表述规范（简历 / 论文 / 面试）

- ✅ 推荐：「基于 NVIDIA 开源 vk_gaussian_splatting 框架（Apache 2.0），自研曲面三段式立体
  投影、跨 API 四缓冲呈现链路与动态 4DGS 流播，并将静态帧时 p50 降低 72%」
- ✅ 推荐：「在上游 raster 骨架上扩展 Multiview 双目标与逐像素反投影 Warp」
- ❌ 禁止：「从零实现 3DGS 渲染器」（与事实不符，且上游代码可被比对）
