# 动态 4DGS 渲染扩展设计

> 目标：让 CurvedStereoGS 从"静态场景查看器"升级为"曲面主动立体屏上的实时动态内容
> 播放器"。本文给出开源方案调研、路线对比、选定策略、资产格式 spec 与集成清单。
> 状态：Phase 1 序列播放与 Dynamic3DGaussians 训练相机初始视角已接入渲染器和 Qt 控制面板。
> 当前样例使用 basketball（150 帧、30 fps）；静态外观属性取第 0 帧，时间变化写入逐帧位置。
> 训练相机只用于启动定位，之后由用户自由控制视角。

## 1. 目标与结论

- 渲染对象从单个静态高斯场扩展为随时间变化的高斯场（动态 3DGS / 4DGS）。
- 保持既有管线下游不变：排序（vrdx）、Multiview 双眼光栅、曲面 Warp、立体呈现
  （四缓冲 / Half-SBS / 降级链）全部复用。
- 核心架构决策一句话：**"animate-then-render"**——在排序之前插入一个动画求值阶段，
  把当前时刻的高斯位置写入现有 position 缓冲；下游对"高斯在动"完全无感。
- 性能底线：每显示帧的动画求值 < 0.5 ms（60 万高斯），稳态零主机→设备大拷贝。

## 2. 开源方案调研

### 2.1 训练侧（资产生产，Python 生态）

| 方案 | 表示 | 对渲染端的输出 | 适配度 |
| --- | --- | --- | --- |
| [4DGaussians](https://github.com/hustvl/4DGaussians)（CVPR'24, hustvl） | canonical 3DGS + HexPlane 式 4D 特征网格 + 小 MLP 解码（位置/旋转/缩放形变） | canonical 高斯 +（网格，MLP）权重；官方口径 82 FPS @800×800 (RTX 3090) | **路线②首选**：权重导出后推理可完全 GPU 化 |
| [SpacetimeGaussians](https://github.com/oppo-us-research/SpacetimeGaussians)（CVPR'24, STG） | 每高斯时间不透明度 α(t) + 多项式运动/旋转 | 每高斯运动参数（无渲染时 MLP） | **路线③原型**：属性可在 shader 内解析求值 |
| [Dynamic3DGaussians](https://github.com/JonathonLuiten/Dynamic3DGaussians)（Deformable-3DGS） | 每时间步高斯 + 跨帧对应（特征空间刚性正则做跟踪） | 逐帧对齐的高斯序列 | **路线①首选**：逐帧导出 PLY 即可播放 |
| [fudan-zvg/4d-gaussian-splatting](https://github.com/fudan-zvg/4d-gaussian-splatting)（ICLR'24） | 原生 4D 高斯 | 类似 ② | 备选 |
| [GSCodec Studio](https://arxiv.org/html/2506.01822v1) | 4D 高斯压缩框架 + 4D 播放器 | 码流 | 压缩与流式参考（较重） |

### 2.2 渲染侧参考

- [aras-p/UnityGaussianSplatting](https://github.com/aras-p/UnityGaussianSplatting)（MIT）：
  分块打包、属性量化（位置 fp16/旋转 smallest-three 16bit）、GPU 双调排序、compute 裁剪
  ——静态属性的打包与量化策略照此借鉴。
- **PlayCanvas 4D flipbook**（WebGL2/WebGPU 逐帧翻页播放）：验证"逐帧序列播放"路线在生
  产上可行，与我们路线①同构。
- UE5 插件生态已支持 STG / keyframe-4DGS / Fudan 4DGS 的引擎内实时渲染——印证"训练
  Python 侧 / 渲染引擎侧分离"是行业共识。

## 3. 路线对比（渲染端集成视角）

| 路线 | 时间进入管线的方式 | 每帧 CPU→GPU 拷贝 | 存储 | 实现量 | 备注 |
| --- | --- | --- | --- | --- | --- |
| ① 序列播放 | 关键帧缓冲切换 + 插值 | 0（关键帧常驻 VRAM） | T × 位置（fp32，窗口池上限 2 GiB） | 小 | 已接入渲染循环 |
| ② 形变场推理 | compute pass：特征采样+MLP → 当帧属性 | 0（权重常驻） | canonical + 权重（~几十 MB） | 中 | 简历含金量最高 |
| ③ STG 解析 | vertex/mesh shader 内由 t 解析 | 0 | 运动参数并入静态属性 | 大 | 依赖训练导出格式 |

存储硬约束（bicycle 60.3 万高斯）：全量属性 ~250 B/高斯 → 150 MB/帧；300 帧全量 =
45 GB（显存不可行）/ 流式 4.5 GB/s（NVMe 不可行）。**因此"动态子集"是第一设计决策**：
形变类方法只动 position（+rot/scale），SH 留在 canonical——position-only fp16 =
3.6 MB/帧，300 帧 = 1.1 GB 显存（窗口化后更低）。

## 4. 选定策略：animate-then-render（两阶段）

**插入点**：现有帧路径为 `加载 → 排序 → Multiview 光栅 → 曲面 Warp → 呈现`。
在排序之前插入 `animate` 阶段：把 t 时刻的位置写入现有 position 缓冲。下游（排序键
计算、光栅、Warp、呈现）对动态完全无感——排序本来每帧每眼就要跑（深度随时间变化），
无需任何改动。

```
加载(静态属性, canonical) ──┐
                            ├─► [animate compute] ─► position 缓冲 ─► 排序 ─► Multiview 光栅 ─► Warp ─► 呈现
关键帧池(VRAM) ────────────┘
```

### Phase 1（关键帧插值，已实现）

- 资产：`.dgs` 打包（见第 5 节）+ 现有静态加载路径读 canonical 属性。
- VRAM 关键帧池：全部或窗口化的逐帧位置缓冲（fp16）。
- animate pass：`pos = lerp(P[k], P[k+1], α)`，α 为帧内插值系数。Slang 核心 ~10 行
  （见第 6 节），一个 compute pipeline。
- 时钟：使用单调时钟按 `.dgs` 的 FPS 循环播放；在 `onRender` 的排序之前插入 GPU
  compute pass，按相邻帧位置做线性插值。
- 训练视角：从 Dynamic3DGaussians 的 `train_meta.json` 读取 `w2c` 和 `k`，将 OpenCV
  RDF 相机位姿转换到渲染器 RUB 坐标；按相机索引设置首帧初始视角，之后由相机操纵器接管。

### Phase 2（形变场推理，路线②）

- 从 4DGaussians checkpoint 导出：canonical .ply + 4D 特征网格（按层存 3D 纹理数组，
  t 为连续层坐标）+ 解码 MLP 权重 flat binary。
- animate pass 换成：特征采样（相邻两层 trilinear）+ 小 MLP 解码 → 位置/旋转/缩放。
- 推理量级：60 万高斯 × 特征采样 + 2×64 MLP ≈ 数 GFLOP/帧 → **<1 ms**（用
  StereoFrameStats 的口径习惯单独计时）。
- 连续时间输入，无关键帧跳变——对立体观看的舒适度是实打实的收益。

### Phase 3（可选，STG 解析运动）

- 运动多项式参数并入静态属性，位置在 vertex/mesh shader 内解析求值，animate pass 消
  失；依赖训练侧导出运动参数，作为远期项。

## 5. `.dgs` 资产格式（v1 spec）

```
Header（64 字节定长）
  magic        : 4s   = 'DGS1'
  version      : u16  = 1
  flags        : u16  = bit0: 位置 fp16（0 则 fp32）
  frameCount   : u32
  splatCount   : u32
  fps          : f32
  reserved     : u32 × 11（0）

帧偏移表    : frameCount × u64（每帧块相对文件头的字节偏移，支持随机访问/窗口预取）
帧块        : 每帧 = splatCount × 3 个紧密排列的标量（f16 | f32），不填充 vec3 步长
```

- **坐标系契约**：canonical PLY 保持原始 RDF（训练坐标，Y 向下）——静态加载路径
  会对它做 RDF->RUB 翻转（ply_loader_async convertCoordinates，flip=(1,-1,-1)）；
  .dgs 位置流必须与**加载后**的缓冲区同空间（RUB），exporter 负责翻转。
  运行时打开序列时会对比两者包围盒（日志 "coordinate space"），centers 相距
  超过半幅即告警——这是镜像/错位类错误的第一防线。
- 静态属性（SH/缩放/旋转/不透明度）不走 .dgs——由现有 .ply/.csgs 静态路径加载
  （canonical）。动态子集只有位置；rot/scale 动态化在 v2 的格式扩展位（flags bit1）。
- 帧块独立可寻址：窗口预取只需要 offset 表 + 一次 pread。

## 6. animate 计算着色器（Phase 1 核心，供集成）

```slang
StructuredBuffer<float> keyframePool; // 紧密排列的关键帧 XYZ 标量
RWStructuredBuffer<float> outPos;      // 紧密排列的中心缓冲 XYZ 标量
CBUF { uint prevFloatOffset; uint nextFloatOffset; float alpha; uint splatCount; }

[numthreads(256, 1, 1)]
void animateMain(uint3 tid : SV_DispatchThreadID)
{
    const uint i = tid.x;
    if (i >= splatCount) return;
    const uint baseA = prevFloatOffset + 3 * i;
    const uint baseB = nextFloatOffset + 3 * i;
    outPos[3 * i + 0] = lerp(keyframePool[baseA + 0], keyframePool[baseB + 0], alpha);
    outPos[3 * i + 1] = lerp(keyframePool[baseA + 1], keyframePool[baseB + 1], alpha);
    outPos[3 * i + 2] = lerp(keyframePool[baseA + 2], keyframePool[baseB + 2], alpha);
}
```

位置缓冲与 `.dgs` 帧块都是 12 字节一个 XYZ。Vulkan SPIR-V 会将 `StructuredBuffer<float3>`
数组步长设为 16 字节，因此着色器用标量缓冲按 `3 * splatIndex` 显式寻址，避免错读和越界写。

Phase 2 时本 kernel 的 lerp 换成"特征纹理数组采样 + MLP 解码"，输出契约不变。

## 7. 集成清单（映射到现有代码）

| # | 改动 | 文件 | 量级 |
| --- | --- | --- | --- |
| 1 | `.dgs` 解析、时间池与时钟 | `src/dynamic_splat_sequence.*` | 已实现 |
| 2 | `--dynamicSequence` 参数及同名 `.dgs` 自动配对 | `src/main.cpp` | 已实现 |
| 3 | 训练相机元数据加载、坐标变换和初始视角定位 | `src/camera_set.h`, `src/gaussian_splatting.cpp` | 已实现 |
| 4 | animate compute pass 在排序前更新共享高斯位置 | `src/gaussian_splatting.cpp`, `src/shaders/dynamic_animate.comp.slang` | 已实现 |
| 5 | Qt 场景联动、序列路径和训练相机索引 | `src/qt/qt_control_center.cpp` | 已实现 |
| 6 | 形变场权重推理与更完整的时间变化外观属性 | 后续扩展 | 未实现 |

集成原则：`DynamicSplatSequence` 只负责"资产解析 + 关键帧池 + 时钟"，不持有渲染管
线；animate pass 归渲染器（与排序/光栅管线同处）。

## 8. 关键设计决策

1. **双眼一致性**：animate 每显示帧只求值一次，写入共享 position 缓冲；Multiview 两层
   读取同一份数据——两眼内容错位（立体眩晕的根源）在结构上不可能发生。这是"先形变后
   渲染"顺序的直接收益，也是与 99% 不考虑立体的 4DGS 实现的差异点。
2. **动态子集**：只有 position 进 .dgs（SH/缩放/旋转留 canonical）。依据：形变类方法
   的形变量集中在这三者；全量属性不可存储也不可流式（第 3 节算术）。v2 若需 rot/scale
   动态，flags bit1 预留。
3. **排序共享旋钮**：两眼视轴差约 2°，默认每眼各排序（正确性优先）；提供"共享单眼排
   序"参数（性能优先，近景混合顺序有可感知差异的折衷）——与现有每帧排序的关系是复用
   而非新增。
4. **时钟解耦**：动画时间与显示帧率、内容 fps 三者解耦；t 连续采样。Phase 2 的形变网
   络吃连续 t，无关键帧跳变。
5. **背景/前景分批**：静态背景高斯单独成批，跳过 animate pass（60 万高斯里通常只有前
   景在动）——省掉无效推理与拷贝。
6. **显存窗口**：帧数超出预算时按 offset 表做滑动窗口 + 预取（v1 同步预取，v2 换异步
   ——`ply_loader_async` 的既有模式可复用）。

## 9. 里程碑

| 里程碑 | 内容 | 工作量 | 验收 |
| --- | --- | --- | --- |
| M1 | 转换工具 + 关键帧池 + 时钟 | 已完成 | `.dgs` 可读取并按时间索引关键帧 |
| M2 | animate pipeline 与训练相机初始视角接入渲染器 | 已完成 | 动态序列播放，初始视角取自所选训练相机 |
| M3 | 4DGaussians 导出脚本 + Phase 2 推理 | 3–5 天 | 形变场路径替换 lerp，帧率不降 |
| M4 | Qt 时间轴 + 窗口化异步预取 | 2 天 | 长序列拖动流畅 |

## 10. 风险与缓解

- **显存不足**：fp16 + 窗口化（第 5/8 节）；窗口大小做成参数。
- **首帧加载慢**：.dgs 顺序读 + offset 表随机访问；M4 换异步。
- **排序开销翻倍担忧**：排序本来每帧每眼执行，动态不增加次数；共享单眼排序旋钮可再省
  一次（第 8.3）。
- **简历/文档一致性**：本特性落地前，简历不得出现"动态 4DGS/形变场"表述；落地后按
  M3 的实测数字更新（遵守 performance.md 的口径纪律）。

## 11. 参考

- 4DGaussians（CVPR'24）: https://github.com/hustvl/4DGaussians · 项目页:
  https://guanjunwu.github.io/4dgs （82 FPS @800×800, RTX 3090）
- Spacetime Gaussian Feature Splatting（CVPR'24）: https://arxiv.org/abs/2312.16812 ·
  代码: https://github.com/oppo-us-research/SpacetimeGaussians
- Dynamic3DGaussians（Deformable-3DGS）: https://github.com/JonathonLuiten/Dynamic3DGaussians
- fudan-zvg/4d-gaussian-splatting（ICLR'24）: https://github.com/fudan-zvg/4d-gaussian-splatting
- GSCodec Studio（4D 压缩与播放）: https://arxiv.org/html/2506.01822v1
- UnityGaussianSplatting（打包/量化/排序参考，MIT）: https://github.com/aras-p/UnityGaussianSplatting
- awesome-dynamic-scene-representations: https://github.com/yklcs/awesome-dynamic-scene-representations
