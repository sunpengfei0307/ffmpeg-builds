# hlg2pq_cuda / detect_cuda Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 `ffmpeg/codes` 的 `hlg2pq_cuda`、`detect_cuda`（含 TRT 插件）迁入 FFmpeg 8.1.2，行为不变，仅做 API/构建适配。

**Architecture:** 宿主 `.c` + 设备 `.cu` 嵌入 PTX；`pq_metadata` 共享对象；TensorRT 以独立共享库 + `dlopen` loader 提供。

**Tech Stack:** FFmpeg 8.1.2 libavfilter、ffnvcodec、cuda_nvcc/cuda_llvm、TensorRT（插件）

## Global Constraints

- 源：`ffmpeg/codes/`；目标：`ffmpeg/ffmpeg-8.1.2/libavfilter/`
- 滤镜注册必须用 `const FFFilter` + `FILTER_*` 宏
- `hw_frames_ctx` 必须经 `ff_filter_link()`
- PTX 符号：`ff_vf_<name>_ptx_data` / `ff_vf_<name>_ptx_len`
- TRT 不静态链入 libavfilter

---

### Task 1: 拷贝源文件并接入构建

**Files:**
- Create: `ffmpeg/ffmpeg-8.1.2/libavfilter/pq_metadata.{c,h}`
- Create: `ffmpeg/ffmpeg-8.1.2/libavfilter/vf_hlg2pq_cuda.{c,cu}`
- Create: `ffmpeg/ffmpeg-8.1.2/libavfilter/vf_detect_cuda.{c,cu}`
- Create: `ffmpeg/ffmpeg-8.1.2/libavfilter/vf_detect_cuda_trt.{h,cpp}`
- Create: `ffmpeg/ffmpeg-8.1.2/libavfilter/vf_detect_cuda_trt_loader.c`
- Modify: `configure`, `libavfilter/Makefile`, `libavfilter/allfilters.c`

- [ ] **Step 1:** 从 `ffmpeg/codes/` 复制上述文件到 `libavfilter/`
- [ ] **Step 2:** `configure` 增加 `hlg2pq_cuda` / `detect_cuda` 的 `*_filter_deps` / `deps_any`（对齐 `pad_cuda`）
- [ ] **Step 3:** `Makefile` 增加 OBJS 行；`hlg2pq` 含 `pq_metadata.o`；`detect` 含 loader + ptx；`SKIPHEADERS` 含 `vf_detect_cuda_trt.h` 与 `pq_metadata.h`
- [ ] **Step 4:** `allfilters.c` 按字母序声明 `ff_vf_detect_cuda`、`ff_vf_hlg2pq_cuda`

### Task 2: 适配 hlg2pq_cuda 宿主 API

**Files:**
- Modify: `vf_hlg2pq_cuda.c`

- [ ] **Step 1:** `internal.h` → `filters.h`；保留 `pq_metadata.h`
- [ ] **Step 2:** PTX 改为 `ff_vf_hlg2pq_cuda_ptx_data/len`；`ff_cuda_load_module` 传 data+len
- [ ] **Step 3:** `config_props` / `filter_frame` 中 `hw_frames_ctx` 改用 `ff_filter_link`
- [ ] **Step 4:** 注册改为 `const FFFilter` + `FILTER_INPUTS/OUTPUTS` + `FILTER_SINGLE_PIXFMT`；去掉 `query_formats` 与 pad `{ NULL }`
- [ ] **Step 5:** `.cu` 不改（已用 `cuda/vector_helpers.cuh`）

### Task 3: 适配 detect_cuda 宿主 API

**Files:**
- Modify: `vf_detect_cuda.c`

- [ ] **Step 1:** 同 Task 2 的 include / FFFilter / FILTER_SINGLE_PIXFMT / pads
- [ ] **Step 2:** 全部 `hw_frames_ctx` 访问改 `FilterLink`
- [ ] **Step 3:** PTX 符号改为 `ff_vf_detect_cuda_ptx_data/len`
- [ ] **Step 4:** AVOption 常量项的 unit 改为 `.unit = "model_type"` / `"face_model_type"`
- [ ] **Step 5:** `.cu` / `.cpp` / loader / `.h` 逻辑保持；确认 `compat/w32dlfcn.h`、`SLIBSUF` 可用

### Task 4: TRT 独立插件构建

**Files:**
- Create: `ffmpeg/ffmpeg-8.1.2/libavfilter/Makefile.detect_cuda_trt` 或 `utils/scripts/build-detect-cuda-trt.sh`

- [ ] **Step 1:** 提供用 `g++`/`nvcc` 友好方式编译 `vf_detect_cuda_trt.cpp` 为 `libavfilter_detect_cuda_trt$(SLIBSUF)` 的规则/脚本（需 `-lnvinfer -lnvonnxparser -lcudart`）
- [ ] **Step 2:** 导出 C ABI：`ff_detect_trt_create/resolve_cache_path/get_info/execute/destroy`
- [ ] **Step 3:** 文档说明：插件放 ffmpeg 同目录或设 `FFMPEG_DETECT_CUDA_TRT_PATH`

### Task 5: 验证

- [ ] **Step 1:** 确认源文件编译期符号与 `pad_cuda`/`tonemap_cuda` 一致（静态检查）
- [ ] **Step 2:** 若环境可编：`ffmpeg -filters` 含 `hlg2pq_cuda`、`detect_cuda`
- [ ] **Step 3:** 提交由用户显式要求时再做（默认不 commit）
