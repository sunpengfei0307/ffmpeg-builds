# detect_cuda

本文档合并原设计说明与集成用法。

## 变更记录

| 日期 | 说明 |
|------|------|
| 2026-08-06 | 合并 `detect_cuda.md` 与 `detect_cuda_integration.md` 为单文件 |

## 设计方案（核心功能）

## 1. 目标与范围

在 FFmpeg 4.4.1 实现全 CUDA 人物/人脸检测滤镜 `detect_cuda`，输出 ROI side data 供 NVENC `roi_qp_map` / x264 `-roi 1`。

设计原则：

- 像素与推理全程 CUDA device memory，无 `hwdownload`  
- TensorRT 推理（非 OpenVINO/DNN backend）  
- 支持 yolov8 / yolov8_face / ssd / e2e  

## 2. 为什么需要专用 Filter

CPU 检测需 hwdownload → 推理 → hwupload，破坏直播全 GPU 管线。本滤镜在 graph 内完成预处理 → TRT → 解码 → ROI metadata → [GPU 画框]。

## 3. 总体架构

### 3.1 文件结构

| 文件 | 职责 |
|------|------|
| `vf_detect_cuda.c` | 选项、TRT 调度、NMS、ROI、画框 |
| `vf_detect_cuda.cu` | preproc、yolov8/ssd decode、draw |
| `vf_detect_cuda_trt.cpp` | ONNX→TRT、enqueueV3、yolov8_face 识别（编译为独立 `.so`） |
| `vf_detect_cuda_trt_loader.c` | 运行时 `dlopen` 加载 TRT 插件，主程序不链 TensorRT |
| `scripts/download_detect_models.sh` | 模型下载 |

### 3.2 模块分层

```text
vf_detect_cuda.c          config_props / filter_frame / ROI side data
vf_detect_cuda.cu         detect_preproc_420 / yolov8_decode / draw
vf_detect_cuda_trt_loader dlopen → libavfilter_detect_cuda_trt.so
vf_detect_cuda_trt.cpp    build_engine / try_init_yolov8_face_heads / execute
```

### 3.2.1 TensorRT 插件化（运行时加载）

**动机：** 若将 `vf_detect_cuda_trt.cpp` 静态链入 `ffmpeg`，启动时 `DT_NEEDED` 会依赖 `libnvinfer`、`libcudart`、`libcudnn` 等；在无 TensorRT/CUDA deps 的机器上 `ffmpeg -version` 即失败。

**做法：**

| 组件 | 链接方式 |
|------|----------|
| `ffmpeg` / `libavfilter.a` | 仅含 `vf_detect_cuda_trt_loader.o`，**不**链 TensorRT |
| `libavfilter_detect_cuda_trt.so` | 单独共享库，链 `-lnvinfer -lnvonnxparser -lcudart` 等 |

首次调用 `ff_detect_trt_create()` 时 loader 按序查找插件：

1. 环境变量 `FFMPEG_DETECT_CUDA_TRT_PATH`（`.so` 完整路径或目录）
2. `$prefix/lib/libavfilter_detect_cuda_trt.so`（`make install` 默认位置）
3. 可执行文件同目录（Linux `/proc/self/exe`）
4. 构建树：`./libavfilter/libavfilter_detect_cuda_trt.so`

**验证：**

```bash
ldd ./ffmpeg | grep -E 'nvinfer|nvonnx|cudart|cudnn|cublas'   # 应无输出
./ffmpeg -version                                              # 无 deps 机器可启动
ldd $prefix/lib/libavfilter_detect_cuda_trt.so                   # 插件含 TRT 依赖
```

### 3.3 模型类型与解码路径

| model_type | 输出 | 解码 |
|------------|------|------|
| `yolov8` | `[1,84,8400]` 等 | GPU |
| `yolov8_face` | 3× `[1,80,H,W]` DFL | CPU DFL |
| `ssd` | `[1,1,N,7]` | GPU |
| `e2e` | EfficientNMS | GPU |

**yakhyo yolov8n-face.onnx**：3-head DFL（80=64 bbox+1 cls+15 kpt），`try_init_yolov8_face_heads()` 自动识别，`model_type=auto|yolov8` 均可。

### 3.4 双引擎调度

- `targets=person+face`：`engine`（人）+ `face_engine`（脸），共用一次 preproc  
- `targets=face`：`engine` 或 `face_engine` 二选一  

### 3.5 TRT 缓存

`{onnx}.{WxH}.{fp16|fp32}.sm{XX}.trt{MMN}.trt`（例：`yolov8n.onnx.640x640.fp16.sm86.trt860.trt`）。

- `sm{XX}`：GPU Compute Capability（`major*10+minor`，同 SM 可跨机拷贝）
- `trt{MMN}`：TensorRT 版本标签（`getInferLibVersion()/10`，如 8601→860）
- 命中则直接加载；未命中或反序列化失败则从 ONNX 重建；旧格式 `{onnx}.{WxH}.{fp16|fp32}.trt` 不回退加载
- I/O 强制 FP32，内部可 FP16；路径解析在 `ff_detect_trt_resolve_cache_path()`，日志在 C 层 `create_trt_context()`

### 3.6 ROI 输出（格式）

`AV_FRAME_DATA_REGIONS_OF_INTEREST`：`left/top/right/bottom` + `qoffset`（AVRational，负=更高质量）。默认 face -0.25、person -0.15。NVENC 配合见 integration 文档。

### 3.7 ROI Bilateral（磨皮）

可选：`bilateral=1` 时在 draw 前对检测区域做 GPU bilateral。详见 [2026-07-28-detect-cuda-bilateral-design.md](./2026-07-28-detect-cuda-bilateral-design.md)；用法见 [integration §ROI 磨皮](./detect_cuda.md#roi-磨皮bilateral)。

- face：`skin_poly × feature_protect × skin_tone` — 皮肤多边形 + 五官椭圆 + YUV 肤色门控  
- 参数：`kpt_radius=0.12`、`feature_scale=1.35`、`skin_inset=0.03`、`skin_tone=1`、`skin_y_min=55`  
- 皮肤多边形按 face bbox 扩到额头/外颊；黑发/非肤色在 ROI 内也不磨  
- person：身体矩形 ROI，减去 face bbox（无五官保护）  
- `draw_skin=1`：绿多边形=已磨皮肤，黄 1px 框=未处理五官；检测框仍 `draw=1`（红/品红）  
- 读写分离：读 `src`、写 `dst`；支持 NV12/YUV420P/P010  

## 4. 实施步骤

1. TRT 封装层（C 接口 + C++，`enqueueV3`）  
2. CUDA preproc + yolov8/ssd GPU decode + draw  
3. 主滤镜：双引擎、targets、CPU NMS、stride  
4. yolov8_face 3-head CPU DFL decode  
5. 直播集成与 context 修复（§5.7）  

## 5. Bug 分析与解决

### 5.1 Segfault @ createInferRuntime

TRT 初始化前未 `cuCtxPushCurrent` → 在 `config_props` 中 push/pop FFmpeg context。

### 5.2 `engine option is required`（仅人脸）

旧逻辑强制 `face_engine` → 允许 `engine=` 或 `face_engine=` 指向人脸 ONNX。

### 5.3 invalid YOLOv8 output shape（人脸）

yakhyo 3-head DFL 非 `[1,84,8400]` → `DETECT_MODEL_YOLOV8_FACE` + `try_init_yolov8_face_heads()`。

### 5.4–5.5 C++ 编译：FFDetectTRTContext

struct 须在全局唯一定义，helper 放在 struct 之后；头文件 opaque pointer。

### 5.6 链接 av_log

C++ 层移除 `av_log`，日志统一在 `.c` 的 `create_trt_context()`。

### 5.7 Cask convolution execution（核心）

**根因：** `cudaSetDevice()` 绑定 primary context，与 cuvid 的 non-primary FFmpeg context 不一致；TRT 持有 FFmpeg context 内的 device pointer 在错误 context 执行。

**修复：** `ensure_cuda_ready()` 仅 `cudaFree(0)`，禁止 `cudaSetDevice`；TRT create/execute 须在 `cuCtxPushCurrent(FFmpeg cuda_ctx)` 之后。

## 6. 依赖与环境（开发）

| 组件 | 版本建议 |
|------|----------|
| CUDA | 11.8 |
| TensorRT | 8.6.x |
| GPU PTX | sm_75 / sm_86 / sm_89 |

configure 见 [detect_cuda.md §编译](./detect_cuda.md#编译)。编译期仍需 `--enable-libtensorrt` 以构建插件 `.so`；**勿**把 `-lnvinfer -lnvonnxparser` 放进 `--extra-libs`（否则 `ffmpeg` 仍会硬依赖 TensorRT）。

## 7. 已知限制与后续

| 项 | 说明 |
|----|------|
| yolov8_face DFL | CPU decode，可迁 GPU |
| landmarks | yolov8_face 解码 5 点；用于皮肤多边形 + 五官椭圆保护 |
| PTX | 改 `.cu` 需重编 `vf_detect_cuda.ptx.o` |
| TRT 缓存 | 改 ONNX/build 逻辑后手动删 `.trt` |

## 8. 关键代码路径

| 功能 | 文件 | 函数 |
|------|------|------|
| 入口 | `vf_detect_cuda.c` | `detect_cuda_filter_frame` |
| TRT 初始化 | `vf_detect_cuda.c` | `create_trt_context` |
| TRT 插件加载 | `vf_detect_cuda_trt_loader.c` | `ff_detect_trt_create`（dlopen） |
| 推理 | `vf_detect_cuda.c` | `run_detection` |
| preproc | `vf_detect_cuda.cu` | `detect_preproc_420` |
| face DFL | `vf_detect_cuda.c` | `decode_yolov8_face_host` |
| TRT 执行 | `vf_detect_cuda_trt.cpp` | `ff_detect_trt_execute` |
| context | `vf_detect_cuda_trt.cpp` | `ensure_cuda_ready` |
| ROI | `vf_detect_cuda.c` | `write_roi_side_data` |
| ROI bilateral | `vf_detect_cuda.cu` | `detect_bilateral_roi` / `build_skin_poly` / `feature_protect_weight` |
| 磨皮调试画框 | `vf_detect_cuda.cu` | `detect_draw_y` / `detect_draw_uv`（`draw_skin`） |
| 关键点解码 | `vf_detect_cuda.c` | `decode_yolov8_face_head` |

## 9. 参数与测试

- 参数表、推荐值与逐项释义：[detect_cuda.md §滤镜参数完整说明](./detect_cuda.md#滤镜参数完整说明)
- ROI 开/关双路 A/B 对比（人脸 / 人物 / 全量）：[detect_cuda.md §ROI A/B 双路对比测试](./detect_cuda.md#roi-ab-双路对比测试)

---

| 路径 | 说明 |
|------|------|
| `.cursor/plans/detect_cuda.md` | 集成与使用 |
| `.cursor/plans/detect_cuda.md` | 本文档 |

*最后更新：2026-08-04（皮肤多边形磨皮 + draw_skin 调试）*

## 参数说明 · 用法示例 · 注意事项 · 踩坑

﻿# detect_cuda 滤镜集成与使用指南

全 CUDA 管线的人物/人脸检测：GPU 预处理 → TensorRT 推理 → ROI side data → NVENC 区域编码。

> 架构设计、Bug 根因（含 Cask/context）详见 [detect_cuda.md](./detect_cuda.md)。

## 功能概览

| 能力 | 说明 |
|------|------|
| 人物检测 | YOLOv8n（COCO class 0） |
| 人脸检测 | yakhyo `yolov8n-face.onnx`（3-head DFL，自动识别） |
| 双模型 | `engine` + `face_engine` 共用一次预处理 |
| ROI 磨皮 | `bilateral=1`：皮肤多边形 + 五官保护 + 肤色门控（跳过黑发等非肤色） |
| 画框 | `draw=1` 检测框；`draw_skin=1` 调试皮肤/五官区域（分色 1px） |
| ROI | `AV_FRAME_DATA_REGIONS_OF_INTEREST` → NVENC / x264 |
| 日志 | `dump=` JSONL |

## 推荐模型

| 用途 | 文件 | model_type |
|------|------|------------|
| 人物 | `yolov8n.onnx` | `auto` / `yolov8` |
| 人脸 | `yolov8n-face.onnx` | `auto`（→ yolov8_face） |

一键下载：

```bash
cd libavfilter/scripts && ./download_detect_models.sh /models
```

手动 wget 人脸 ONNX 等详见下文「模型下载」。

---

## 模型下载

### 方式 A：脚本（推荐）

```bash
cd libavfilter/scripts
chmod +x download_detect_models.sh
./download_detect_models.sh /models
```

脚本会下载人物 ONNX 与人脸 ONNX 到指定目录（默认 `/models`），可直接用于 `detect_cuda=engine=...`。

### 方式 B：手动

**人脸（直接 ONNX）：**

```bash
wget -c -O yolov8n-face.onnx \
  https://github.com/yakhyo/yolov8-face-onnx-inference/releases/download/weights/yolov8n-face.onnx
```

**人物（pt → onnx）：**

```bash
wget -c https://github.com/ultralytics/assets/releases/download/v0.0.0/yolov8n.pt
pip3 install ultralytics onnx
python3 -c "from ultralytics import YOLO; YOLO('yolov8n.pt').export(format='onnx', imgsz=640, simplify=True, opset=12)"
```

> **导出参数说明：**
>
> - `opset=17`：推荐使用 17 及以上版本，与 TensorRT 兼容性最佳
> - `simplify=True`：简化 ONNX 模型，去除冗余节点，大幅提升转换成功率
>
> 等价 CLI：
>
> ```bash
> yolo export model=yolov8n.pt format=onnx opset=17 simplify=True
> ```

> yakhyo 人脸为 3-head DFL 格式，滤镜自动识别；技术细节见 [detect_cuda.md §3.3](./detect_cuda.md#33-模型类型与解码路径)。

---

## 编译

TensorRT 后端编译为独立插件 `libavfilter_detect_cuda_trt.so`；`ffmpeg` 主程序通过 `dlopen` 加载，**不应**在 `--extra-libs` 中链接 `-lnvinfer -lnvonnxparser`。

```bash
export CUDA_HOME=/usr/local/cuda-11.8
export TRT_ROOT=/opt/nvidia/TensorRT-8.6.1.6

./configure \
  --enable-cuda-nvcc --enable-cuvid --enable-nvenc --enable-libnpp \
  --enable-libtensorrt --enable-filter=detect_cuda \
  --extra-cflags="-I$CUDA_HOME/include -I$TRT_ROOT/include" \
  --extra-ldflags="-L$CUDA_HOME/lib64 -L$TRT_ROOT/lib"

make -j$(nproc)
make install    # 安装插件到 $prefix/lib/libavfilter_detect_cuda_trt.so

ffmpeg -filters | grep detect_cuda
ldd ./ffmpeg | grep -E 'nvinfer|nvonnx|cudart'   # 应无输出
```

**插件路径（任选其一）：**

| 方式 | 说明 |
|------|------|
| `make install` | 默认 `$prefix/lib/libavfilter_detect_cuda_trt.so` |
| 环境变量 | `export FFMPEG_DETECT_CUDA_TRT_PATH=/path/to/libavfilter_detect_cuda_trt.so` |
| 开发调试 | 构建产物 `libavfilter/libavfilter_detect_cuda_trt.so`，loader 也会搜索 `libavfilter/` 目录 |

**无 TensorRT 环境：** `ffmpeg -version` 可正常启动；仅在使用 `detect_cuda` 滤镜且找不到插件时会报错。

**有 TensorRT 环境：** 确保插件 `.so` 可加载（安装路径或 `FFMPEG_DETECT_CUDA_TRT_PATH`）；插件依赖的 `libnvinfer` 等须在 `LD_LIBRARY_PATH` 或系统库路径中。

修改 `.cu` 后重编 PTX：

```bash
cd libavfilter
nvcc -ptx -o vf_detect_cuda.ptx vf_detect_cuda.cu -arch=sm_75 --compiler-options '-fPIC'
make V=1 vf_detect_cuda.o vf_detect_cuda.ptx.o
```

**验证插件化是否生效：**

```bash
# ffmpeg 主程序不应硬依赖 TensorRT
ldd ./ffmpeg | grep -E 'nvinfer|nvonnx|cudart|cudnn|cublas'   # 应无输出

# 插件自身应含 TRT 依赖
ldd $prefix/lib/libavfilter_detect_cuda_trt.so | grep nvinfer
```

> 编译期仍需 `--enable-libtensorrt` 以构建插件 `.so`；**勿**把 `-lnvinfer -lnvonnxparser` 放进 `--extra-libs`（否则 `ffmpeg` 仍会硬依赖 TensorRT，无 deps 机器无法启动）。

---

## 推荐管线

```text
hevc_cuvid → fps → hlg2pq_cuda → scale_cuda → detect_cuda → hevc_nvenc (roi_qp_map=1)
```

| 环节 | 说明 |
|------|------|
| `hevc_cuvid` | 硬解输入，帧在 FFmpeg non-primary CUDA context |
| `hlg2pq_cuda` | HDR 源转 PQ（可选，见 hlg2pq_cuda 文档） |
| `scale_cuda` | 缩放到目标分辨率（如 1920×1080） |
| `detect_cuda` | 检测 + ROI side data + 可选画框 |
| `hevc_nvenc` | 配合 `-roi_qp_map 1` 实现区域 QP |

---

## 使用示例

### 直播单路（人脸 + ROI，基准命令）

与 A/B 对比测试除 `split` 与第二路输出外，**分辨率、码率、编码参数完全一致**：

```bash
./ffmpeg -hwaccel cuvid -hwaccel_device 0 \
  -abnormal_timeout 30 -thread_queue_size 2048 \
  -c:v hevc_cuvid -task_id 302427 -err_fps_trigger_threshold 2 \
  -f live_flv -i "rtmp://10.61.228.106/live/dongfang_udp2rtmp_src_4k" \
  -filter_complex "[0:v:0]fps=fps=25,hlg2pq_cuda,scale_cuda=1920:1080,setdar=dar=a,\
detect_cuda=face_engine=/data/sunpf/ffmpeg.4/deps/models/yolov8n-face.onnx:face_model_type=yolov8:targets=face:score=0.5:stride=1:draw=1:face_qoffset=-60[vout0]; \
[0:a:0]volume=1.0,aresample=osr=48000[ain0]" \
  -map '[vout0]' -map '[ain0]' \
  -vcodec hevc_nvenc -gpu 0 \
  -b:v 2000000 -maxrate 2000000 -bufsize 8000000 \
  -bf 0 -r 25 -g 50 -level 0.0 -profile:v main10 \
  -roi_qp_map 1 -roi_qp_mode delta -roi_qp_strength 51 \
  -spatial_aq 0 -temporal_aq 0 -b_scale_ratio 1.4 \
  -acodec libfdk_aac -ab 128000 -ac 2 \
  -f flv "rtmp://test-push.live.qiyi.domain/live/roi_effect_on"
```

人物 / 全量单路：仅替换 `detect_cuda=...` 片段（见 [ROI A/B 双路对比测试](#roi-ab-双路对比测试) 表格）。

### 离线 + libx264 ROI

```bash
ffmpeg -hwaccel cuda -hwaccel_output_format cuda -i input.mp4 \
  -vf "detect_cuda=engine=/models/yolov8n.onnx:face_engine=/models/yolov8n-face.onnx:targets=person+face:draw=0" \
  -c:v libx264 -crf 23 -roi 1 out_roi.mp4
```

双模型 `person+face` 模式下，`engine` 指向人物 ONNX，`face_engine` 指向人脸 ONNX，共用一次 GPU 预处理。

---

## ROI A/B 双路对比测试

同一路输入、**两路完全同规格编码**（1920×1080、2Mbps），仅 `-roi_qp_map 1` vs `0` 不同。

**通用结构：**

```text
detect_cuda 一次 → split=2 → 两路 hevc_nvenc（编码参数逐字相同，仅 roi_qp_map 不同）
```

| 路 | `-roi_qp_map` | 推流后缀示例 |
|----|---------------|--------------|
| ROI 开 | `1` | `*_on` |
| ROI 关 | `0` | `*_off` |

**三路公共编码参数（与单路基准命令一致，不得修改）：**

```text
scale_cuda=1920:1080
-b:v 2000000 -maxrate 2000000 -bufsize 8000000 -bf 0
-r 25 -g 50 -level 0.0 -profile:v main10 -gpu 0
-roi_qp_mode delta -roi_qp_strength 51
-spatial_aq 0 -temporal_aq 0 -b_scale_ratio 1.4
-acodec libfdk_aac -ab 128000 -ac 2
```

**三路仅 `detect_cuda=...` 不同：**

| 场景 | detect_cuda 片段 |
|------|------------------|
| 人脸 | `face_engine=.../yolov8n-face.onnx:face_model_type=yolov8:targets=face:score=0.5:stride=1:draw=1:face_qoffset=-60` |
| 人物 | `engine=.../yolov8n.onnx:model_type=yolov8:targets=person:score=0.5:stride=1:draw=1:person_qoffset=-60` |
| 全量 | `engine=.../yolov8n.onnx:face_engine=.../yolov8n-face.onnx:model_type=yolov8:face_model_type=yolov8:targets=person+face:score=0.5:stride=1:draw=1:face_qoffset=-60` |

全量相对人脸：**仅增加** `engine=.../yolov8n.onnx`、`model_type=yolov8`，`targets=face` → `targets=person+face`。

### 1. 仅人脸（face）

```bash
./ffmpeg -hwaccel cuvid -hwaccel_device 0 \
  -abnormal_timeout 30 -thread_queue_size 2048 \
  -c:v hevc_cuvid -task_id 302427 -err_fps_trigger_threshold 2 \
  -f live_flv -i "rtmp://10.61.228.106/live/dongfang_udp2rtmp_src_4k" \
  -filter_complex "\
[0:v:0]fps=fps=25,hlg2pq_cuda,scale_cuda=1920:1080,setdar=dar=a,\
detect_cuda=face_engine=/data/sunpf/ffmpeg.4/deps/models/yolov8n-face.onnx:face_model_type=yolov8:targets=face:score=0.5:stride=1:draw=1:face_qoffset=-60[vdet]; \
[vdet]split=2[vroi][vnoroi]; \
[0:a:0]volume=1.0,aresample=osr=48000,asplit=2[ain0][ain1]" \
  -map "[vroi]"  -map "[ain0]" \
  -vcodec hevc_nvenc -gpu 0 \
  -b:v 2000000 -maxrate 2000000 -bufsize 8000000 \
  -bf 0 -r 25 -g 50 -level 0.0 -profile:v main10 \
  -roi_qp_map 1 -roi_qp_mode delta -roi_qp_strength 51 \
  -spatial_aq 0 -temporal_aq 0 -b_scale_ratio 1.4 \
  -acodec libfdk_aac -ab 128000 -ac 2 \
  -f flv "rtmp://test-push.live.qiyi.domain/live/roi_face_on" \
  -map "[vnoroi]" -map "[ain1]" \
  -vcodec hevc_nvenc -gpu 0 \
  -b:v 2000000 -maxrate 2000000 -bufsize 8000000 \
  -bf 0 -r 25 -g 50 -level 0.0 -profile:v main10 \
  -roi_qp_map 0 \
  -spatial_aq 0 -temporal_aq 0 -b_scale_ratio 1.4 \
  -acodec libfdk_aac -ab 128000 -ac 2 \
  -f flv "rtmp://test-push.live.qiyi.domain/live/roi_face_off"
```

### 2. 仅人物（person）

相对 §1，仅 `detect_cuda` 换为人物模型：

```bash
./ffmpeg -hwaccel cuvid -hwaccel_device 0 \
  -abnormal_timeout 30 -thread_queue_size 2048 \
  -c:v hevc_cuvid -task_id 302427 -err_fps_trigger_threshold 2 \
  -f live_flv -i "rtmp://10.61.228.106/live/dongfang_udp2rtmp_src_4k" \
  -filter_complex "\
[0:v:0]fps=fps=25,hlg2pq_cuda,scale_cuda=1920:1080,setdar=dar=a,\
detect_cuda=engine=/data/sunpf/ffmpeg.4/deps/models/yolov8n.onnx:model_type=yolov8:targets=person:score=0.5:stride=1:draw=1:person_qoffset=-60[vdet]; \
[vdet]split=2[vroi][vnoroi]; \
[0:a:0]volume=1.0,aresample=osr=48000,asplit=2[ain0][ain1]" \
  -map "[vroi]"  -map "[ain0]" \
  -vcodec hevc_nvenc -gpu 0 \
  -b:v 2000000 -maxrate 2000000 -bufsize 8000000 \
  -bf 0 -r 25 -g 50 -level 0.0 -profile:v main10 \
  -roi_qp_map 1 -roi_qp_mode delta -roi_qp_strength 51 \
  -spatial_aq 0 -temporal_aq 0 -b_scale_ratio 1.4 \
  -acodec libfdk_aac -ab 128000 -ac 2 \
  -f flv "rtmp://test-push.live.qiyi.domain/live/roi_person_on" \
  -map "[vnoroi]" -map "[ain1]" \
  -vcodec hevc_nvenc -gpu 0 \
  -b:v 2000000 -maxrate 2000000 -bufsize 8000000 \
  -bf 0 -r 25 -g 50 -level 0.0 -profile:v main10 \
  -roi_qp_map 0 \
  -spatial_aq 0 -temporal_aq 0 -b_scale_ratio 1.4 \
  -acodec libfdk_aac -ab 128000 -ac 2 \
  -f flv "rtmp://test-push.live.qiyi.domain/live/roi_person_off"
```

### 3. 人物 + 人脸（person+face）

相对 §1 人脸命令，**编码参数不变**，`detect_cuda` 仅增加人物模型：

```bash
./ffmpeg -hwaccel cuvid -hwaccel_device 0 \
  -abnormal_timeout 30 -thread_queue_size 2048 \
  -c:v hevc_cuvid -task_id 302427 -err_fps_trigger_threshold 2 \
  -f live_flv -i "rtmp://10.61.228.106/live/dongfang_udp2rtmp_src_4k" \
  -filter_complex "\
[0:v:0]fps=fps=25,hlg2pq_cuda,scale_cuda=1920:1080,setdar=dar=a,\
detect_cuda=engine=/data/sunpf/ffmpeg.4/deps/models/yolov8n.onnx:face_engine=/data/sunpf/ffmpeg.4/deps/models/yolov8n-face.onnx:model_type=yolov8:face_model_type=yolov8:targets=person+face:score=0.5:stride=1:draw=1:face_qoffset=-60[vdet]; \
[vdet]split=2[vroi][vnoroi]; \
[0:a:0]volume=1.0,aresample=osr=48000,asplit=2[ain0][ain1]" \
  -map "[vroi]"  -map "[ain0]" \
  -vcodec hevc_nvenc -gpu 0 \
  -b:v 2000000 -maxrate 2000000 -bufsize 8000000 \
  -bf 0 -r 25 -g 50 -level 0.0 -profile:v main10 \
  -roi_qp_map 1 -roi_qp_mode delta -roi_qp_strength 51 \
  -spatial_aq 0 -temporal_aq 0 -b_scale_ratio 1.4 \
  -acodec libfdk_aac -ab 128000 -ac 2 \
  -f flv "rtmp://test-push.live.qiyi.domain/live/roi_full_on" \
  -map "[vnoroi]" -map "[ain1]" \
  -vcodec hevc_nvenc -gpu 0 \
  -b:v 2000000 -maxrate 2000000 -bufsize 8000000 \
  -bf 0 -r 25 -g 50 -level 0.0 -profile:v main10 \
  -roi_qp_map 0 \
  -spatial_aq 0 -temporal_aq 0 -b_scale_ratio 1.4 \
  -acodec libfdk_aac -ab 128000 -ac 2 \
  -f flv "rtmp://test-push.live.qiyi.domain/live/roi_full_off"
```

**对比要点：**

- 三路分辨率 **1920×1080**、码率 **2Mbps**，编码与人脸 A/B 基准一致。
- 全量相对人脸：filter 只多 `engine=.../yolov8n.onnx`、`model_type=yolov8`，`targets=person+face`。
- 两路来自同一帧 `detect_cuda`；`*_off` 路仅 `-roi_qp_map 0`。

**离线落盘：** 将末尾 `-f flv "rtmp://..."` 改为 `-f mp4 /tmp/roi_xxx_on.mp4` / `..._off.mp4` 即可。

---

## 首次运行与 TRT 缓存

首次加载 ONNX 在同目录生成：

```text
yolov8n.onnx.640x640.fp16.sm86.trt860.trt
yolov8n-face.onnx.640x640.fp16.sm86.trt860.trt
```

（`smXX` / `trtMMN` 随本机 GPU Compute Capability 与 TensorRT 版本变化；同 SM + 同 TRT 可跨机拷贝。）

- 默认 `fp16=1`，缓存名含 `fp16` 正常
- 首帧构建约 1~3 分钟，之后读缓存
- 要 FP32：`fp16=0` 并删除旧 `.trt`

常见 GPU 与 SM 对照（缓存名中的 `smXX`）：
|             GPU           | SM   |
|---------------------------|------|
| RTX 2060/2070/2080        | sm75 |
| T4                        | sm75 |
| RTX 3060/3070/3080/3090   | sm86 |
| A10 / A30                 | sm80 |
| A100                      | sm80 |
| RTX 4070/4080/4090        | sm89 |
| L4 / L20 / L40            | sm89 |
| H100                      | sm90 |

查询本机：`nvidia-smi --query-gpu=name --format=csv,noheader`（旧版无 `compute_cap` 字段时按上表对照），或用 `deviceQuery` / `cudaGetDeviceProperties`。

提前使用 `trtexec` 生成（固定 batch=1；须在目标 SM 机器上构建；静态 ONNX 勿加 `--shapes`）：

```bash
trtexec \
  --onnx=/data/LCMS/models/yolov8n.onnx \
  --saveEngine=/data/LCMS/models/yolov8n.onnx.640x640.fp16.sm89.trt860.trt \
  --fp16

trtexec \
  --onnx=/data/LCMS/models/yolov8n-face.onnx \
  --saveEngine=/data/LCMS/models/yolov8n-face.onnx.640x640.fp16.sm89.trt860.trt \
  --fp16
```

支持多 batch 推理，用以下参数替代 `--shapes`：

```bash
trtexec \
  --onnx=/data/LCMS/models/yolov8n.onnx \
  --saveEngine=/data/LCMS/models/yolov8n.onnx.640x640.fp16.sm89.trt860.trt \
  --fp16 \
  --minShapes=images:1x3x640x640 \
  --optShapes=images:8x3x640x640 \
  --maxShapes=images:16x3x640x640
```

| 参数  | 含义 |
|------|------|
| `--minShapes=images:1x3x640x640`  | 最小 batch |
| `--optShapes=images:8x3x640x640`  | 最优 batch（性能最好） |
| `--maxShapes=images:16x3x640x640` | 最大 batch |

> 缓存文件名格式：`{onnx}.{WxH}.{fp16|fp32}.sm{XX}.trt{MMN}.trt`（含 GPU SM 与 TensorRT 版本）。修改 ONNX、`input_size`、`fp16`，或换不同 SM/TRT 环境后会使用新文件名；旧格式缓存不会被加载。

---

## CUDA 上下文（cuvid 必读）

使用 `-hwaccel cuvid` 时帧在 FFmpeg 的 **non-primary CUDA context** 中。若出现 `Cask convolution execution`：

- 确认使用含 context 修复的最新代码
- **勿**在 TRT 路径调用 `cudaSetDevice()`
- 一般**无需**删 `.trt` 缓存

根因详见 [detect_cuda.md §5.7](./detect_cuda.md#57-cask-convolution-execution核心)。

**技术背景：**

| 问题 | 说明 |
|------|------|
| primary vs non-primary | `cudaSetDevice()` 绑定 primary context，与 cuvid 的 FFmpeg context 不一致 |
| 修复方式 | `ensure_cuda_ready()` 仅 `cudaFree(0)`；TRT create/execute 须在 `cuCtxPushCurrent(FFmpeg cuda_ctx)` 之后 |
| 症状 | TRT 在错误 context 执行 Cask 卷积，报 `Cask convolution execution` |

---

## ROI 与 NVENC

滤镜写入 `AV_FRAME_DATA_REGIONS_OF_INTEREST`，每个检测框含 `left/top/right/bottom` 与 `qoffset`（AVRational，负值 = 更高质量）。

| 检测类型 | 代码默认 qoffset | 直播推荐 | 含义 |
|----------|------------------|----------|------|
| face | -1.0（`face_qoffset=-100`） | -0.25（`-25`） | 人脸更高质量 |
| person | -1.0（`person_qoffset=-100`） | -0.15（`-15`） | 人物稍高质量 |

`qoffset` = 参数值 / 100。直播建议显式设 `face_qoffset=-25` / `person_qoffset=-15`，再配合 NVENC `roi_qp_strength` 控制实际 ΔQP。

**NVENC：**

```bash
-roi_qp_map 1 -roi_qp_mode delta -roi_qp_strength 24
```

直播 A/B 测试使用 `roi_qp_strength 51` 以放大 ROI 效果便于肉眼对比；生产环境可按码率与主观质量调整（常用 24~51）。

**x264：**

```bash
-roi 1
```

| 编码器 | 开关 | 说明 |
|--------|------|------|
| NVENC | `-roi_qp_map 1` | 读取 side data 中的 delta QP |
| x264 | `-roi 1` | 读取相同 ROI side data |

---

## 滤镜参数完整说明

| 参数 | 类型 | 代码默认 | 推荐值 | 说明 |
|------|------|----------|--------|------|
| `engine` | string | 必填* | 见模型章 | 主模型 `.onnx`/`.trt` |
| `face_engine` | string | 无 | 双模型必填 | 人脸模型 |
| `model_type` | enum | auto | auto | yolov8 / yolov8_face / ssd / e2e |
| `face_model_type` | enum | auto | auto | 同 model_type |
| `targets` | string | person | 按场景 | person / face / person+face（**用 + 分隔**） |
| `labels` | string | person+face | 默认即可 | ROI 类别名（**用 + 分隔**） |
| `score` | float | 0.45 | **0.5**（直播） | 置信度阈值 |
| `iou` | float | 0.45 | 0.45 | NMS IoU，0=关 |
| `max_det` | int | 64 | **32**（直播） | 单帧最大框数 |
| `input_size` | int | 640 | 640 | 须与 ONNX imgsz 一致 |
| `fp16` | bool | 1 | 1 | TRT FP16 构建 |
| `draw` | bool | 1 | **0**（上线 ROI） | GPU 画检测框 |
| `draw_skin` | bool | 0 | 调磨皮时 **1** | 画皮肤 ROI（绿）+ 未处理五官框（黄，1px） |
| `thickness` | int | 1 | 1 | 检测框线宽 1~16（不影响 `draw_skin`） |
| `person_qoffset` | int | -100 | -15 | QP 偏移 /100 |
| `face_qoffset` | int | -100 | -25 | QP 偏移 /100 |
| `stride` | int | 1 | **2~3**（直播） | 每 N 帧推理 |
| `dump` | string | 无 | 调试时 | JSONL 路径 |
| `pool_size` | int | 32 | 32 | hwframe 池 |
| `bilateral` | bool | 0 | 按需 | ROI 磨皮（默认关） |
| `bilateral_targets` | string | face | face | face / person / all |
| `sigmaS` | float | 3.0 | **3.0** | 空间 sigma |
| `sigmaR` | float | 50.0 | **50** | 值域 sigma（8-bit 量纲） |
| `window_size` | int | 9 | **9** | 邻域窗口（奇数；>15 费算力） |
| `kpt_radius` | float | 0.12 | **0.12** | 五官保护基准半径 / 脸宽 |
| `feature_scale` | float | 1.35 | **1.35** | 眼/嘴/鼻椭圆相对 `kpt_radius` 放大 |
| `skin_inset` | float | 0.03 | **0.03** | 皮肤多边形内缩（相对脸宽；偏小=磨更多额/颊） |
| `edge_soft` | float | 8 | **8** | ROI/五官边缘软过渡像素 |
| `bilateral_planes` | int | 1 | 1 | 1=仅 Y；3=Y+UV |
| `skin_tone` | bool | 1 | **1** | 只磨肤色 YUV；跳过黑发/手套等 |
| `skin_y_min` | float | 55 | **55~70** | 肤色最低亮度；越大越严（更排黑发） |

\* 仅人脸：`engine` 或 `face_engine` 二选一；双模型两者兼有。

### 典型场景推荐组合

**直播 ROI（人脸，省算力）：**

```text
face_engine=.../yolov8n-face.onnx:targets=face:score=0.5:max_det=32:draw=0:stride=2:fp16=1
```

**人脸磨皮（皮肤多边形 + 五官保护，需 yolov8_face）：**

```text
face_engine=.../yolov8n-face.onnx:targets=face:score=0.5:draw=0:bilateral=1:bilateral_targets=face:sigmaS=3:sigmaR=50:window_size=9:kpt_radius=0.12:edge_soft=8
```

可选：五官仍糊 `:feature_scale=1.5`；绿区仍偏大 `:skin_inset=0.06`；绿区还偏小 `:skin_inset=0`

**磨皮区域调试（分色描边，便于对比）：**

```text
face_engine=.../yolov8n-face.onnx:targets=face:bilateral=1:draw=1:draw_skin=1:thickness=1:sigmaS=3:sigmaR=50:window_size=9:kpt_radius=0.12:edge_soft=8
```

| 颜色 | 含义 |
|------|------|
| 红/品红 | 人脸检测框（`draw=1`） |
| 绿色多边形 | 皮肤磨皮 ROI（已处理区域边界） |
| 黄色框（1px） | 未处理五官保护区（眼 / 鼻 / 嘴） |

只看皮肤/五官、不要检测框：`draw=0:draw_skin=1`。

**离线调试（人物+人脸）：**

```text
engine=.../yolov8n.onnx:face_engine=.../yolov8n-face.onnx:targets=person+face:score=0.45:draw=1:dump=/tmp/det.jsonl
```

**高精度离线：**

```text
engine=.../yolov8n.onnx:targets=person:score=0.4:fp16=0:stride=1:max_det=64
```

**全量直播 ROI（person+face，与 A/B 测试一致）：**

```text
engine=.../yolov8n.onnx:face_engine=.../yolov8n-face.onnx:model_type=yolov8:face_model_type=yolov8:targets=person+face:score=0.5:stride=1:draw=1:face_qoffset=-60
```

### 参数详细释义

#### 模型与引擎

**`engine`** — 主检测模型路径（`.onnx` 或 `.trt`）。

- `targets=person`：须指向人物模型（如 `yolov8n.onnx`）。
- `targets=face` 且未设 `face_engine`：可指向人脸模型（如 `yolov8n-face.onnx`）。
- `targets=person+face`：须指向人物模型；人脸由 `face_engine` 提供。

首次加载 `.onnx` 时，会在同目录自动生成 TRT 缓存（如 `yolov8n.onnx.640x640.fp16.sm86.trt860.trt`，含 SM 与 TRT 版本）。

**`face_engine`** — 人脸专用模型路径，双模型模式使用。

- `targets=person+face` 时**必填**。
- `targets=face` 时可选，与 `engine` 二选一。
- 与 `engine` 共用同一次 GPU 预处理（YUV→RGB），再分别推理，避免重复预处理开销。

**`model_type` / `face_model_type`** — 指定模型**输出 tensor 格式**，决定解码路径。

| 值 | 含义 |
|----|------|
| `auto` | 根据 engine tensor shape 自动识别（推荐） |
| `yolov8` | 标准 Ultralytics 输出 `[1, 84, 8400]`，GPU 解码 |
| `yolov8_face` | yakhyo 人脸 3-head DFL `[1, 80, H, W]×3`，CPU 解码 |
| `ssd` | SSD 格式 `[1, 1, N, 7]` |
| `e2e` | 已含 NMS 的端到端 TRT engine |

`face_model_type` 仅作用于 `face_engine`；仅人脸且为 `auto` 时，会继承 `model_type`。

**`input_size`** — 模型正方形输入边长（默认 640）。

须与 ONNX 导出时的 `imgsz` 一致，否则检测框会偏移。同时影响 letterbox 缩放、TRT 缓存文件名（如 `640x640`）和推理耗时（越大越慢、小目标可能更准）。

**`fp16`** — 首次从 ONNX 构建 TRT 时是否启用 FP16 内部计算。

- `1`（默认）：更快、更省显存，缓存名含 `fp16`。
- `0`：全 FP32，更慢但更稳；需删旧 `.trt` 重建。

I/O tensor 始终为 FP32，与 FP16 内部计算不冲突。

#### 检测目标与过滤

**`targets`** — 控制启用哪些检测类别，并决定加载哪些 TRT 引擎。

**分隔符用 `+`，不要用 `,`**：`filter_complex` 里逗号是滤镜链分隔符，`targets=person,face` 会被截断；请用 `targets=person+face` 或 `targets=face+person`（顺序无关）。代码仍兼容旧写法 `,`（仅非 filtergraph 场景）。

| 值 | 加载引擎 | 典型用途 |
|----|----------|----------|
| `person` | 仅 `engine`（人物） | 全身 ROI |
| `face` | `engine` 或 `face_engine`（人脸） | 人脸清晰度 |
| `person+face` | `engine` + `face_engine` | 人物+人脸双层 ROI |

未匹配到有效值时，回退为 `person+face`。

**`labels`** — ROI / dump 日志中的**类别名称映射**，`+` 分隔（同 `targets`，勿用 `,`）。

默认 `person+face` 表示索引 0 → person、索引 1 → face。用于区分框类型、选择 `person_qoffset` / `face_qoffset`，以及 side data 里的 `label=person/face`。一般保持默认即可。

**`score`** — 置信度阈值，范围 0~1，低于此值的候选框被丢弃。

- 越高：漏检增多，误检减少。
- 越低：检出更多，误检可能增多。

直播建议 **0.5~0.6**；离线分析可用 **0.45**。

**`iou`** — CPU 端 NMS（非极大值抑制）的 IoU 阈值。

同类框 IoU 超过 `iou` 时，保留得分更高的、去掉另一个。默认 **0.45** 较均衡；设 **0** 可关 NMS。人物与人脸分别做 NMS，双模型合并后还会再做一次全局 NMS。

**`max_det`** — 单帧最多保留的检测框数量，同时决定 GPU 缓冲分配大小。

默认 64，直播 **32** 通常够用。过大占显存、ROI 写入变慢；过小在人多场景会截断部分框。超出上限时按得分排序截断。

#### 输出画框与 ROI

**`draw`** — 是否在 GPU 帧上绘制**检测框**（人脸红/品红，人物蓝）。

- `1`：画检测框，便于调试。
- `0`：不画检测框，略省 GPU；上线只要 ROI 时建议 **0**。

`stride>1` 跳帧时，会用上一帧结果继续画框（若 `draw=1`）。

**`draw_skin`** — 是否绘制磨皮调试叠加（与 `draw` 可独立开关；`draw` 或 `draw_skin` 任一为 1 即 launch 画框 kernel）。

- 绿色多边形轮廓：皮肤磨皮 ROI（由 5 点关键点推成的 9 边形，经 `skin_inset` 内缩）
- 黄色 1px 矩形：未处理五官保护区 AABB（双眼椭圆 / 鼻 / 嘴）
- 线宽固定 **1px**，不受 `thickness` 影响

用于对比「磨了哪里、五官保没保住」；上线务必 `draw_skin=0`。

**`thickness`** — **检测框**线宽（像素），范围 1~16，仅 `draw=1` 时生效。默认 **1**；`draw_skin` 始终 1px。

**`person_qoffset` / `face_qoffset`** — 写入 ROI side data 的 **QP 质量偏移**，供 NVENC `roi_qp_map` 或 x264 `-roi 1` 使用。

实际值 = 参数 / 100：

- `person_qoffset=-15` → -0.15（人物区更高质量）
- `face_qoffset=-25` → -0.25（人脸区更高质量）

负值越大，该区域编码质量越高（码率向 ROI 倾斜）。人脸默认比人物更负，体现「脸优先」。

**`dump`** — 可选 JSONL 日志路径。每帧一行，含 pts、帧号、各框 label/score/坐标。用于离线评估、调参，上线一般不设。

#### ROI 磨皮（bilateral）

需 `targets` 含 `face` 且人脸模型为 yolov8_face（带 5 点关键点：左眼、右眼、鼻、左嘴角、右嘴角）。

**权重：**

```text
weight = skin_poly_weight × feature_protect_weight × skin_tone_weight
out    = lerp(src, bilateral(src), weight)
```

| 分量 | 含义 |
|------|------|
| `skin_poly_weight` | 落在皮肤 9 边形内才磨；无关键点时回退为内缩椭圆 |
| `feature_protect_weight` | 眼/鼻/嘴椭圆 + 5 点圆内为 0（不磨）；区外 `edge_soft` 软过渡 |
| `skin_tone_weight` | YUV 肤色门控：低亮度（黑发）、非肤色色度（绿幕/黑布等）权重→0 |

**`bilateral`** — `1` 启用；在 draw 之前、对 `dst` 做 ROI bilateral（读 `src`）。`stride>1` 跳检帧仍用缓存框磨皮。

**`bilateral_targets`** — `face` / `person` / `all`（`+` 分隔亦可）。person 区域会减去相交 face bbox；五官保护只在 face 路径生效。

**`sigmaS` / `sigmaR` / `window_size`** — 双边滤波强度。通用推荐：`sigmaS=3:sigmaR=50:window_size=9`。窗口越大越慢（约 O(W²)）。

**`kpt_radius`** — 五官保护基准半径 = `kpt_radius × 脸宽`。默认 **0.12**。

**`feature_scale`** — 眼/嘴/鼻椭圆相对 `kpt_radius` 的放大系数。默认 **1.35**；眼嘴仍糊可试 **1.5**。

**`skin_inset`** — 皮肤多边形向中心内缩（相对脸宽），避发际/轮廓硬边。默认 **0.03**；绿区仍偏大可提到 **0.06~0.08**；偏小可设 **0**。

**`edge_soft`** — ROI / 五官边界软过渡像素数，默认 **8**。

**`bilateral_planes`** — `1` 仅磨 Y（推荐）；`3` 同时磨 Y+UV。

**`skin_tone`** — `1`（默认）启用肤色门控：几何 ROI 内仍跳过明显非肤色像素（额前碎发、黑发、手套、舞台绿等）。`0` 关闭（仅几何+五官保护）。

**`skin_y_min`** — 肤色最低 Y（8-bit）。默认 **55**；碎发仍被磨可提到 **65~75**；阴影脸颊被误伤可降到 **45**。

#### 性能与缓冲

**`stride`** — 推理间隔：每 N 帧才真正跑 TRT，中间帧复用上一帧结果。

- `1`：每帧推理，最准、最耗 GPU。
- `2~3`：直播常用，GPU 占用可降 50%~66%。

跳帧时仍会用上一帧框做 ROI / 磨皮；若 `draw=1` 则继续画上一帧的框。首帧或尚无结果时会强制推理。

**`pool_size`** — CUDA 输出 hwframe 池大小（8~128，默认 32）。

filter graph 缓冲较深、或与其他 CUDA 滤镜串联时，池太小可能等帧；一般默认 32 够用，极端长链路可试 48~64。

#### 参数关系速览

```text
targets          → 决定加载 engine / face_engine
input_size       → 须与模型导出一致，影响框坐标与 TRT 缓存名
score + iou      → 控制框数量与质量
max_det          → 限制最终输出上限
stride           → 用算力换实时性；ROI/磨皮仍可用上一帧 boxes
draw / draw_skin → 仅影响可视化，不影响 ROI / 磨皮结果
bilateral        → 皮肤多边形 × 五官保护；需 face + 关键点
kpt_radius / feature_scale / skin_inset → 调五官保护与磨皮范围
person/face_qoffset → 仅影响编码质量分配，不影响检测
fp16             → 仅影响 TRT 构建，改后需删 .trt 重建
```

---

## 滤镜参数速查（简表）

| 参数 | 默认 | 说明 |
|------|------|------|
| `engine` / `face_engine` | 必填* | 模型路径 |
| `targets` | person | 检测类别 |
| `score` / `iou` | 0.45 | 阈值 / NMS |
| `stride` | 1 | 跳帧 |
| `draw` / `draw_skin` | 1 / 0 | 检测框 / 磨皮区域调试 |
| `bilateral` | 0 | 皮肤磨皮 |
| `sigmaS` / `sigmaR` / `window_size` | 3 / 50 / 9 | 磨皮强度 |
| `kpt_radius` / `feature_scale` / `skin_inset` | 0.12 / 1.35 / 0.03 | 五官保护与磨皮范围 |
| `person_qoffset` / `face_qoffset` | -100 / -100（推荐 -15 / -25） | ROI QP |

完整说明见 [滤镜参数完整说明](#滤镜参数完整说明)；逐项释义见 [参数详细释义](#参数详细释义)。

---

## 常见问题

| 问题 | 处理 |
|------|------|
| `engine option is required` | 仅人脸时用 `engine=` 或 `face_engine=` |
| `invalid YOLOv8 output shape` | 用人脸 ONNX + 新版自动识别 yolov8_face |
| `Cask convolution execution` | context 修复；见上文 CUDA 章节 |
| `TensorRT plugin not found` | `make install` 或设 `FFMPEG_DETECT_CUDA_TRT_PATH` |
| `ffmpeg` 启动绑 libnvinfer | 从 `--extra-libs` 去掉 `-lnvinfer -lnvonnxparser`，重配重编 |
| 只有人物没有人脸 | 加 `face_engine=` 或 `targets=face` |
| 框位置偏移 | `input_size=640` 与导出 imgsz 一致 |
| 首次很慢 | 正在建 TRT 缓存 |
| NVENC ROI 无效 | 加 `-roi_qp_map 1` |
| x264 ROI 无效 | 加 `-roi 1` |
| 改 fp16 后行为异常 | 删除旧 `.trt` 重建 |
| 无 TensorRT 机器启动失败 | 确认未链 `-lnvinfer`；插件化编译见 §编译 |
| 改 `.cu` 后磨皮/画框行为旧 | 重编 PTX：`make -C libavfilter vf_detect_cuda.ptx.o vf_detect_cuda.o` |
| 磨皮后眼/嘴发糊 | 加大 `feature_scale`（如 1.5）或 `kpt_radius`；开 `draw_skin=1` 核对黄框是否盖住五官 |
| 磨皮范围太大（发际/轮廓） | 加大 `skin_inset`（如 0.06）；开 `draw_skin=1` 看绿色多边形 |
| 额头碎发/黑发被磨糊 | 确认 `skin_tone=1`；提高 `skin_y_min`（如 65~75） |
| `draw_skin` 颜色不分 | 确认已重编 PTX；检测框红/品红、皮肤绿、五官黄 |

---

## 相关文件

| 路径 | 说明 |
|------|------|
| `.cursor/plans/detect_cuda.md` | 开发设计 |
| `.cursor/plans/detect_cuda.md` | 本文档 |
| `libavfilter/vf_detect_cuda.c` | 主滤镜：选项、NMS、ROI、画框 |
| `libavfilter/vf_detect_cuda.cu` | CUDA preproc、decode、draw |
| `libavfilter/vf_detect_cuda_trt.cpp` | ONNX→TRT、推理（编译为独立 `.so`） |
| `libavfilter/vf_detect_cuda_trt_loader.c` | TRT 插件 dlopen loader |
| `libavfilter/libavfilter_detect_cuda_trt.so` | TensorRT 运行时插件（`make install`） |
| `libavfilter/scripts/download_detect_models.sh` | 模型下载 |
| `tools/build_detect_cuda.sh` | 构建辅助脚本 |
| `tools/detect_cuda_verify.sh` | 功能验证脚本 |

*文档更新：2026-08-04（皮肤多边形磨皮 + 五官椭圆保护；`draw_skin` 分色调试；参数表同步）*
