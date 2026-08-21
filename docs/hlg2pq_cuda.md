# hlg2pq_cuda

本文档合并原设计说明与集成用法。

## 变更记录

| 日期 | 说明 |
|------|------|
| 2026-08-06 | 合并 `hlg2pq_cuda.md` 与 `hlg2pq_cuda_integration.md` 为单文件 |

## 设计方案（核心功能）

## 1. 目标与范围

在 FFmpeg 4.4.1 中实现纯 GPU 的 HLG→PQ HDR 转码链路：

```text
hevc_cuvid/NVDEC -> CUDA frame -> hlg2pq_cuda -> scale_cuda -> hevc_nvenc -> flv/rtmp
```

核心要求：

- 像素全程在 CUDA device memory，不引入 `hwdownload`。
- CUDA kernel 完成真实 HLG→PQ 像素转换（非仅改 metadata）。
- 补全 PQ/HDR10 frame side data 与 HEVC NVENC SEI。
- 与 `scale_cuda`、`hevc_nvenc` 串联。

第一版支持：

- 输入/输出：`AV_PIX_FMT_CUDA` + `sw_format=P010`
- 色彩：HLG / BT.2020 NCL / limited range → PQ / BT.2020 NCL / limited range

## 2. 为什么需要专用 CUDA Filter

HLG→PQ 须在 RGB/线性光域处理：

1. P010 limited YCbCr 归一化  
2. BT.2020 NCL YCbCr → RGB  
3. HLG OETF 逆 + OOTF  
4. PQ EOTF 逆  
5. RGB → YCbCr 写回 P010  

只改 metadata 会导致下游按 PQ 解释 HLG 像素，且缺 Mastering/CLL 时播放器显示异常。

## 3. 总体设计

### 3.1 Filter 架构

| 文件 | 职责 |
|------|------|
| `libavfilter/vf_hlg2pq_cuda.c` | hw_frames、metadata、NVENC 侧 SEI 数据源 |
| `libavfilter/vf_hlg2pq_cuda.cu` | `hlg2pq_y` / `hlg2pq_uv` kernel |

构建：`vf_hlg2pq_cuda.cu → .ptx → .ptx.c → .ptx.o`，`configure` 依赖 `ffnvcodec` + `cuda_nvcc|cuda_llvm`。

### 3.2 Pixel Kernel

双 kernel：

- `hlg2pq_y`：每 luma 像素完整 HLG→PQ  
- `hlg2pq_uv`：2×2 luma 各自 HLG→PQ 后平均 PQ 域 Cb/Cr（优于单点采样，减少色偏）

P010 **10bit 左对齐**（读写 `>>6` / `<<6`），不可当满 16bit 使用。

### 3.3 Metadata

输出帧：

```c
color_trc       = SMPTE2084
color_primaries = BT2020
colorspace      = BT2020_NCL
color_range     = MPEG (limited)
```

Side data：Mastering Display Metadata、Content Light Level；峰值由选项 `peak_luminance`（默认 1000 nits）驱动。

### 3.4 NVENC SEI

本仓库 NVENC 头文件无新版 mastering API，采用 `NV_ENC_SEI_PAYLOAD` 动态注入：

- SEI 137：Mastering Display Colour Volume  
- SEI 144：Content Light Level  

注入条件：`frame->color_trc == SMPTE2084`（滤镜输出帧级 metadata，非仅 `avctx` 初始化期标签）。

## 4. 实施步骤

1. 以 `scale_cuda` hwframe 模式新增 filter 骨架（query/config/filter_frame）。  
2. 实现 HLG/PQ 色彩 kernel + P010 对齐。  
3. 输出帧写 PQ metadata，清除源 HLG side data。  
4. `nvenc.c` 按 frame side data 注入 HDR10 SEI。  
5. 直播链路排障：`-bf 2` + `scale_cuda` 阻塞 → stream sync 修复（§5）。

## 5. Bug 分析与解决

### 5.1 现象

1080p 链路加入 `hlg2pq_cuda` 后：

- `scale_cuda=1920:-2` 后编码阻塞  
- `-bf 0` 或 `-loglevel debug` 可恢复  
- 无本滤镜时正常  

### 5.2 排除项

- 非格式不支持（输出 P010，`scale_cuda` 有 `Convert_p010le_p010le`）  
- 非单纯算力不足（debug 更慢却恢复，说明是时序/生命周期）  

### 5.3 根因

`cuLaunchKernel` 异步；原实现在 kernel 后立即销毁 texture、pop context、交出 `dst`。`-bf 2` 时 NVENC 持帧加深，与下游 resize 叠加产生 race。debug 日志改变调度时序，属偶然规避。

### 5.4 修复

```c
ret = CHECK_CU(cu->cuStreamSynchronize(s->cu_stream));
```

并在输出 `AVHWFramesContext` 设置 `initial_pool_size = pool_size`（默认 32），减少直播启动期帧池抖动。

## 6. 验证清单（开发）

- [ ] `ffmpeg -filters | grep hlg2pq_cuda`  
- [ ] 纯 GPU 链路输出 PQ metadata（ffprobe）  
- [ ] `-bf 2 -loglevel info/warning` 不再阻塞  
- [ ] `scale_cuda=1920:-2` 与 `3840:2160` 回归  

验证命令与直播示例见 [hlg2pq_cuda.md](./hlg2pq_cuda.md)。

## 7. 参数索引

完整说明（作用、推荐值、场景组合）见 [hlg2pq_cuda.md §滤镜参数完整说明](./hlg2pq_cuda.md#滤镜参数完整说明)。

## 8. 已知限制与后续

| 项 | 说明 |
|----|------|
| 格式 | 当前 P010；P016/YUV420P10/BT2020_CL 未支持 |
| MaxCLL/FALL | 静态默认值，非逐帧 GPU 统计 |
| 性能 | 可与 scale 融合为 `hlg2pq_scale_cuda` 减少一轮读写 |

## 9. 核心结论

三件事须同时正确：

1. 像素真实 HLG→PQ  
2. PQ/HDR10 metadata 传到 frame 与 HEVC SEI  
3. CUDA 异步与 NVENC B 帧持帧的生命周期须闭合（`cuStreamSynchronize` + 足够 `pool_size`）

---

## 相关文档

| 路径 | 说明 |
|------|------|
| `.cursor/plans/hlg2pq_cuda.md` | 集成与使用 |
| `.cursor/plans/hlg2pq_cuda.md` | 本文档 |

*最后更新：2026-06-11*

## 参数说明 · 用法示例 · 注意事项 · 踩坑

纯 GPU 的 HLG→PQ HDR 色彩转换：P010 CUDA 帧内完成 OETF/OOTF 变换，并补全 PQ/HDR10 元数据与 NVENC SEI。

> 架构设计、异步 Bug 根因与修复详见 [hlg2pq_cuda.md](./hlg2pq_cuda.md)。

## 功能概览

| 能力 | 说明 |
|------|------|
| HLG→PQ | BT.2020 P010 limited → PQ P010 limited |
| 元数据 | 输出 `color_trc=smpte2084` + Mastering Display + Content Light Level |
| NVENC | HEVC 码流注入 SEI type 137/144（依赖 frame side data） |
| 全 GPU | 与 `scale_cuda`、`hevc_nvenc` 串联，无 `hwdownload` |

## 推荐管线

### HDR 转码（HLG 源保持 HDR，改 PQ 标签 + 像素）

```text
hevc_cuvid → fps → hlg2pq_cuda → scale_cuda → hevc_nvenc (main10) → flv/rtmp
```

```bash
-filter_complex "[0:v:0]fps=fps=25,hlg2pq_cuda=pool_size=32,split=1[vin0];\
[vin0]scale_cuda=1920:-2,setdar=dar=a[vout0];..."
```

编码侧建议：

```bash
-profile:v main10 -pix_fmt p010le \
-color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc
```

### 与 detect_cuda / tonemap_cuda 的关系

| 下游需求 | 是否前置 hlg2pq_cuda |
|----------|----------------------|
| 继续 HDR（PQ）链路 + ROI 检测 | **是** |
| HDR→SDR 直播（tonemap_cuda） | **否** — HLG 源直连 tonemap 色彩更准 |

---

## 编译

```bash
./configure --enable-cuda-nvcc --enable-cuvid --enable-nvenc --enable-libnpp \
  --enable-filter=hlg2pq_cuda
make -j$(nproc)
ffmpeg -filters | grep hlg2pq_cuda
```

修改 `vf_hlg2pq_cuda.cu` 后：

```bash
make V=1 libavfilter/vf_hlg2pq_cuda.o libavfilter/vf_hlg2pq_cuda.ptx.o ffmpeg
```

---

## 使用示例

### 直播（保留 B 帧）

修复 stream sync 后，可正常使用 `-bf 2`：

```bash
./ffmpeg -hwaccel cuvid -hwaccel_device 0 \
  -c:v hevc_cuvid -f live_flv -i "rtmp://..." \
  -filter_complex "[0:v:0]fps=fps=25,hlg2pq_cuda=pool_size=32,split=1[vin0];\
[vin0]scale_cuda=1920:-2,setdar=dar=a[vout0]; \
[0:a:0]volume=1.0,aresample=osr=48000[ain0]" \
  -map '[vout0]' -map '[ain0]' \
  -vcodec hevc_nvenc -gpu 0 -bf 2 \
  -b:v 2000000 -r 25 -g 50 -profile:v main10 \
  -acodec libfdk_aac -ab 128000 -ac 2 \
  -f flv "rtmp://..."
```

### 离线文件

```bash
ffmpeg -hwaccel cuda -hwaccel_output_format cuda -i input_hlg.mp4 \
  -vf "hlg2pq_cuda=pool_size=32,scale_cuda=1920:-2" \
  -c:v hevc_nvenc -profile:v main10 -pix_fmt p010le \
  -color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc \
  output_pq.mp4
```

### 验证 metadata

```bash
ffprobe -v error -select_streams v:0 \
  -show_streams -show_frames -read_intervals "%+#1" output_pq.mp4
```

应确认：`color_transfer=smpte2084`、`Mastering display metadata`、`Content light level metadata`。

---

## 滤镜参数完整说明

| 参数 | 类型 | 代码默认 | 推荐值 | 说明 |
|------|------|----------|--------|------|
| `peak_luminance` | float | 1000.0 | **1000** | PQ mastering 峰值（nits），写入 Mastering/CLL metadata 并传入 kernel |
| `pool_size` | int | 32 | **32**（直播） | CUDA 输出 hwframe 池 [8,128] |

输入须为 HLG/BT.2020 P010 CUDA 帧；输出色彩固定 PQ/BT.2020（无 transfer/matrix 选项）。

---

## 典型场景推荐组合

**标准直播（1080p 缩放）：**

```text
hlg2pq_cuda=pool_size=32 → scale_cuda=1920:-2
```

**4K 直通 PQ（不缩放）：**

```text
hlg2pq_cuda=pool_size=32
```

**启动期仍偶发卡顿（运行层补强，非代码替代）：**

```text
-surfaces 32 -delay 32
```

---

## 常见问题

| 现象 | 处理 |
|------|------|
| 加滤镜后 `-bf 2` 卡住 | 确认已含 `cuStreamSynchronize` 修复；勿用 `-bf 0` 作为长期方案 |
| `-loglevel debug` 才正常 | 时序 race 症状，同上 |
| 播放器 PQ 发白/无 HDR 标识 | 检查 ffprobe 是否有 HDR10 side data；NVENC 需 HEVC main10 |
| 只改 metadata 不转像素 | 必须用本滤镜，不能只设 `-color_trc smpte2084` |
| P010 缩放色偏 | 确认 kernel 使用 10bit 左对齐读写（见设计文档 §3.2） |

根因分析见 [hlg2pq_cuda.md §5](./hlg2pq_cuda.md#5-bug-分析与解决)。

---

## 相关文件

| 路径 | 说明 |
|------|------|
| `.cursor/plans/hlg2pq_cuda.md` | 开发设计与 Bug 总结 |
| `.cursor/plans/hlg2pq_cuda.md` | 本文档 |
| `libavfilter/vf_hlg2pq_cuda.c` / `.cu` | 实现 |

---

*最后更新：2026-06-11*
