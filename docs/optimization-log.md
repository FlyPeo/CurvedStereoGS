# 渲染管线优化记录

按时间顺序记录每次性能优化的背景、思路、实现与实测结果，便于回溯与复测。

---

## 优化 #1：排序时域复用 + 帧缓冲降带宽（2026-10-17）

- **测试环境**：NVIDIA GeForce RTX 4070 SUPER，静态场景 `models/bicycle.ply`，1920×1080，默认曲面立体主线路（Multiview + 三段式 Warp），300 帧 / 次，取 269 个常稳采样帧（自动跳过前 30 帧热身）。
- **旧版二进制**：`_bin/Release/CurvedStereoGS_baseline_pretune.exe`（优化前 + 帧时统计，保留用于复测）。

### 问题

帧路径解剖（`GaussianSplatting::onRender` → `renderHybridPipeline`）发现四处浪费：

1. **每帧无条件全量排序**（`processSortingOnGPU`，gaussian_splatting.cpp）：距离/剔除 compute + vrdx 全量 radix sort 每帧执行，相机静止、画面完全不变时也照跑。排序键实际只由中心眼视图/投影矩阵决定（`dist.comp.slang:58`），这是可复用的充分条件，但代码没有利用。
2. **帧缓冲 RGBA32F**：`prmRender.colorFormat` 默认 `R32G32B32A32_SFLOAT`（parameters.h），COLOR_MAIN/AUX1 每像素 16 字节；每帧每层约 4 次全屏触碰（光栅写 AUX1、Warp 读 AUX1、历史读写 MAIN），显示输出用不满 32 位精度。
3. **动态序列每帧全量重写**：`animateDynamicSplats` 只要序列打开就对全部 splat 做 lerp 重写，混合参数不变时结果恒同，属重复功。
4. **每帧宽 barrier**：`readBackIndirectParametersIfNeeded` 用 `ALL_COMMANDS→TRANSFER` 的全管线屏障为 16 字节的统计回读让路，阻碍 GPU 异步重叠。

### 思路

- 排序键 = f(视图矩阵, 投影) ⇒ **排序结果的有效性可以显式跟踪**：视图矩阵、FOV、透明剔除开关均未变，且没有事件改写 splat 位置（动态序列、场景加载、gizmo 拖拽）时，跳过整段"间接参数重置 + 距离计算 + radix sort"，复用 GPU 上现存的排序索引与间接绘制参数，只补一个近零成本的可见性屏障。
- 显示路径不需要 32 位浮点累积 ⇒ 帧缓冲降为 16F，全屏带宽近乎减半；HDR 余量（65504）与 2^-11 相对精度足够，且 `.csgs` 工程加载本就支持 16F 路径。
- 混合参数（关键帧对、alpha、数量）与上帧相同 ⇒ dispatch 结果恒同，可整体跳过，并连带不触发排序失效。
- 统计回读的屏障只需覆盖该缓冲的实际写入者（距离 compute + 间接参数重置的 transfer）。

### 方法

| 改动 | 位置 | 说明 |
| --- | --- | --- |
| 排序时域复用 | `renderHybridPipeline`（gaussian_splatting.cpp:897 附近） | 缓存 `视图矩阵(memcmp) + FOV + 透明剔除开关`，叠加脏标记 `m_sortDirty`；跳过时补 `COMPUTE\|TRANSFER → COMPUTE\|MESH\|VERTEX\|DRAW_INDIRECT` 可见性屏障（跨提交读取符合 Vulkan 语义） |
| 失效事件接线 | `gaussian_splatting_app.cpp onPreRender`（ply 加载完成）、`openDynamicSequence`、`animateDynamicSplats`（实际 dispatch 时）、`m_helpers.transform.isDragging()` | 新增公开方法 `markSplatSortDirty()` |
| 帧缓冲 16F | `parameters.h` `colorFormat` 默认值 | `R32G32B32A32 → R16G16B16A16_SFLOAT` |
| 动态 lerp 早退 | `animateDynamicSplats` | 缓存 `(n, alpha, k0, k1)`，全等则跳过 dispatch 与置脏 |
| 回读屏障收窄 | `readBackIndirectParametersIfNeeded` | `ALL_COMMANDS → COMPUTE\|TRANSFER` |
| 帧时统计（测量工具） | `onRender` 累计、`onDetach` 输出 | 退出时打印 `Frame timing: mean/p50/p95/p99/max`，自动跳过前 30 帧，天然排除启动着色器编译时间 |

### 结果

| 指标 | 旧版 | 新版 | 变化 |
| --- | --- | --- | --- |
| **p50 帧时** | 7.610 ms (131 fps) | **2.140 ms (467 fps)** | **↓ 72%** |
| p95 帧时 | 7.741 ms | 5.676 ms | ↓ 27% |
| p99 帧时 | 22.312 ms | 5.807 ms | ↓ 74% |
| mean | 21.397 ms | 16.986 ms | ↓ 21% |
| max | 3859.9 ms | 3962.9 ms | 持平 |

- p50 降幅（约 5.5ms）与"每帧全量排序"的估算成本吻合；p99 与 p95 几乎持平说明新版帧耗时分布平稳。
- mean/max 被两版共有的异步加载尖峰（约 3.9 秒的场景上传 hitch）污染，属启动噪声，对比以 p50/p95/p99 为准。如需消除，可把统计热身从 30 帧提高到 60 帧。
- 动态场景（如 `assets/basketball_canonical.ply`）位置每帧变化、排序必须执行，预期收益主要来自 16F 带宽，幅度小得多。

### 复测命令（各约 15 秒）

```powershell
.\_bin\Release\CurvedStereoGS_baseline_pretune.exe --scene .\models\bicycle.ply --size 1920 1080 --headless 1 --headlessFrameCount 300 | Select-String "Frame timing"
.\_bin\Release\CurvedStereoGS.exe --scene .\models\bicycle.ply --size 1920 1080 --headless 1 --headlessFrameCount 300 | Select-String "Frame timing"
```

### 踩坑：`--headless` + `--benchmark` 会无限渲染

main.cpp:630-633：headless 与 benchmarkMode 同时开启时 `headlessFrameCount` 被强制改为 `UINT32_MAX`（设计上由基准序列器脚本负责 close）。**不带序列器脚本传 `--benchmark 1` 会导致无限渲染**——表现为 GPU 满载、CPU 空闲、永不退出，极易误判为"卡死/编译慢"。自动化测量请只用 `--headless 1 --headlessFrameCount N`，不要加 `--benchmark`。

### 后续可做

- 排序键量化为 16 位（vrdx 键宽支持待确认），进一步减半排序流量。
- 动态场景的排序摊销（局部重排代替全排）。
- Slang 编译磁盘缓存，消除每次启动约 1-2 分钟的固定编译成本。→ **已在优化 #2 完成**

---

## 优化 #2：Slang 着色器 SPIR-V 磁盘缓存 + 便携运行包（2026-10-18）

### 问题

渲染器每次启动通过 nvslang 从源码全量编译约 17 个着色器（约 3.4-4.2 秒，优化 #1 之前更长），是启动"白屏"的主体；nvslang 无编译缓存选项，且 `updateSlangMacros` 的宏集合（约 50 个）随渲染模式变化，无法简单按文件缓存。

另发现一个**既有的隐性浪费与崩溃窗口**：运行中可能残留待处理的 `m_requestUpdateShaders` 请求（日志可见旧版单次运行有 3 次 "Shaders updated"），退出时 `reset() → processUpdateRequests(true)` 会对即将关闭的进程做一次完整的"反初始化 → 重编译 → 管线重建"——纯浪费最多 4 秒，且与新增的缓存写盘组合后表现为**偶发退出段错误**（1080p/300 帧配置下约 2/3 概率）。

### 思路与方法

- **缓存 key = 全部着色器源码摘要 + 文件名 + 宏集合**：对 `getShaderDirs()` 下所有 `.slang/.h` 文件字节做 FNV-1a 64 位摘要（懒计算一次），任何源码或共享头文件的改动都会使整个缓存失效；宏集合进入 key 保证不同渲染模式命中不同条目。命中则直接读 SPIR-V 建 `VkShaderModule`，未命中则编译并写盘。缓存目录为 `<exe目录>/shader_cache/`，随程序便携。
- **teardown 守卫**：新增 `m_shuttingDown` 标志，`onDetach` 置位；`processUpdateRequests()` 在此状态下直接丢弃待编译请求——退出路径不再重建任何管线。
- **便携布局**：`getShaderDirs()` 增加 `<exe目录>/CurvedStereoGS_files/nvshaders` 搜索路径（`CurvedStereoGS_files/shaders` 原已支持），使着色器源码可脱离源码树随包分发。

### 结果

| 场景 | 冷缓存 | 热缓存 |
| --- | --- | --- |
| 着色器阶段 | 3439-4160 ms（17 个全编译） | **8-16 ms（17 个全部读盘）** |
| bicycle 300 帧总启动+运行 | 7 s | **2-3 s** |

- 崩溃修复后 1080p/300 帧配置 **8/8 次干净退出**（修复前同配置约 2/3 概率段错误）。
- 便携包 `bin/CurvedStereoGS_Portable/`：exe + DLL + `CurvedStereoGS_files/{shaders,nvshaders}` + 预热完成的 `shader_cache/`，冷启动 7 s、热启动 **3 s**（含加载与渲染），完全脱离源码树运行。

### 复测命令

```powershell
cd bin\CurvedStereoGS_Portable
.\CurvedStereoGS.exe --scene ..\..\models\bicycle.ply --size 1280 720 --headless 1 --headlessFrameCount 300 | Select-String "Shaders updated"
```

### 注意

- headless 帧数设置过少（如 40 帧）时，进程可能在异步加载线程完成前就退出，导致着色器延迟编译未触发——验证缓存请用 ≥300 帧。
- 场景文件不随包分发：把 `models/` 放进包内或用绝对路径传 `--scene`。
