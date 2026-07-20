# hlg2pq_cuda / detect_cuda 迁移设计规格

日期：2026-07-20  
状态：已实现（方案 A）  
范围：将 `ffmpeg/codes` 中的滤镜迁入 FFmpeg 8.1.2 `libavfilter`

## 背景与目标

源码位于 `ffmpeg/codes/`（旧版 `AVFilter` API）。目标树为 `ffmpeg/ffmpeg-8.1.2`，已有 `pad_cuda` / `tonemap_cuda` / `crop_cuda` 等 CUDA 滤镜范式（`FFFilter` + 嵌入 PTX）。

目标：完整迁入 `hlg2pq_cuda`、`detect_cuda`（含 TensorRT 独立插件），算法与选项行为与 codes 一致；仅做 8.1.2 API / 构建兼容。

## 已确认需求

| 项 | 选择 |
|----|------|
| 架构 | 双独立滤镜 + 共享 `pq_metadata` |
| TensorRT | 方案 A：独立 `libavfilter_detect_cuda_trt` 插件 + `dlopen` loader |
| PQ 元数据 | 使用 `codes/pq_metadata.c` / `.h`（非内联重写） |
| 算法 | 完全仿照 codes，不改 kernel / TRT 逻辑 |

## 1. 文件布局

```
libavfilter/
  pq_metadata.h / pq_metadata.c          # 共享 PQ/HDR10 元数据
  vf_hlg2pq_cuda.c / vf_hlg2pq_cuda.cu
  vf_detect_cuda.c / vf_detect_cuda.cu
  vf_detect_cuda_trt.h
  vf_detect_cuda_trt_loader.c            # 编入 libavfilter
  vf_detect_cuda_trt.cpp                 # 独立共享库，不链入 libavfilter
```

## 2. 构建接入

### hlg2pq_cuda

- `configure`：`hlg2pq_cuda_filter_deps="ffnvcodec"`，`deps_any="cuda_nvcc cuda_llvm"`
- `Makefile`：`vf_hlg2pq_cuda.o` + `vf_hlg2pq_cuda.ptx.o` + `cuda/load_helper.o` + `pq_metadata.o`
- `allfilters.c`：`extern const FFFilter ff_vf_hlg2pq_cuda`

### detect_cuda

- `configure`：`detect_cuda_filter_deps="ffnvcodec"`，`deps_any="cuda_nvcc cuda_llvm"`（另需 `libdl`/`LoadLibrary` 以 `dlopen`）
- `Makefile`：`vf_detect_cuda.o` + `vf_detect_cuda.ptx.o` + `vf_detect_cuda_trt_loader.o` + `cuda/load_helper.o`
- `SKIPHEADERS`：`vf_detect_cuda_trt.h`
- `allfilters.c`：`extern const FFFilter ff_vf_detect_cuda`
- TRT 插件：独立规则/脚本编译 `vf_detect_cuda_trt.cpp` → `libavfilter_detect_cuda_trt$(SLIBSUF)`，依赖系统 TensorRT + CUDA runtime；运行时由 loader 查找（可执行文件旁、`FFMPEG_DETECT_CUDA_TRT_PATH`、系统 lib 目录）

### pq_metadata

- 作为 `hlg2pq_cuda`（及将来其它滤镜）的普通 `.o` 依赖对象编入

## 3. 8.1.2 API 适配清单

| codes（旧） | 8.1.2（新） |
|-------------|-------------|
| `#include "internal.h"` | `#include "filters.h"` |
| `AVFilter ff_vf_*` | `const FFFilter ff_vf_*`，字段在 `.p.*` |
| `.query_formats = ...` | `FILTER_SINGLE_PIXFMT(AV_PIX_FMT_CUDA)` |
| `.inputs` / `.outputs` 数组含 `{ NULL }` | `FILTER_INPUTS` / `FILTER_OUTPUTS`，无 NULL 哨兵 |
| `inlink->hw_frames_ctx` | `ff_filter_link(inlink)->hw_frames_ctx` |
| `outlink->hw_frames_ctx = ...` | `ff_filter_link(outlink)->hw_frames_ctx = ...` |
| `extern const char vf_*_ptx[]` + `strlen` | `ff_vf_*_ptx_data[]` + `ff_vf_*_ptx_len` |
| AVOption 末尾 `"unit"` 字符串 | `.unit = "unit"` |
| `av_hwframe_get_buffer(outlink->hw_frames_ctx, ...)` | 使用 `FilterLink *` 上的 `hw_frames_ctx` |

设备端 `.cu` / TRT `.cpp` 逻辑保持不变；`vector_helpers.cuh` 已存在于 `libavfilter/cuda/`。

## 4. 行为（保持 codes）

### hlg2pq_cuda

- 输入：`AV_PIX_FMT_CUDA`，`sw_format=P010`
- 选项：`peak_luminance`（默认 1000）、`pool_size`（默认 32）、`repair_metadata`（默认 1）
- `repair_metadata=1`（默认）：已是标准 PQ 的帧跳过转换，内部走与 `repair_pq_metadata` 相同的 `REPAIR_MISSING` 补全 MDM/CLL；HLG 帧仍转换并 `FORCE_ALL`
- HLG 路径允许 primaries/matrix 为 unspecified（由 metadata 补全）

### repair_pq_metadata

- 独立 metadata-only 滤镜（软/硬帧均可）
- 选项：`enable`（默认 1）、`peak_luminance`（默认 1000）
- 对已是 PQ/BT.2020 且缺 MDM/CLL 的帧调用 `ff_frame_apply_pq_hdr_metadata(..., REPAIR_MISSING, ...)`

### detect_cuda

- 输入：CUDA hwframes，`NV12` / `P010` / `YUV420P`
- TensorRT 人物/人脸检测、可选画框、`AV_FRAME_DATA_REGIONS_OF_INTEREST`
- 双引擎、`stride`、loader `dlopen` 插件 —— 行为不变

## 5. 错误处理与成功标准

- 无 `hw_frames_ctx` / 非支持 `sw_format` / 非 HLG 输入：与 codes 相同错误码
- TRT 插件缺失：config/首帧加载失败并提示查找路径
- 成功：`ffmpeg -filters` 列出两滤镜；PTX 可编译；有 TRT 时 detect 可加载插件；hlg2pq 输出 PQ 色标与 MDM/CLL

## 非目标

- 不改 YOLO/TRT 模型格式或检测算法
- 不把 TRT 静态链入 `libavfilter`
- 不合并进 `tonemap_cuda`
