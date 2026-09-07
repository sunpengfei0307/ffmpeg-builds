# tonemap_cuda

本文档合并原设计说明与集成用法。

## 变更记录

| 日期 | 说明 |
|------|------|
| 2026-09-02 | FFmpeg 8 补回 NVIDIA Vulkan 对 CUDA 帧的协商（libplacebo 链路），见 [libplacebo.md](./libplacebo.md) |
| 2026-08-06 | 合并 `tonemap_cuda.md` 与 `tonemap_cuda_integration.md` 为单文件 |

## 设计方案（核心功能）

## 1. 目标与范围

在 FFmpeg 4.4.1 中实现 GPU 双向 HDR↔SDR 滤镜 `tonemap_cuda`，对标 `tonemap_opencl` 色彩数学与 VLC d3d11 Hable 观感。

```text
hevc_cuvid → fps → [hlg2pq_cuda 可选] → tonemap_cuda → scale_cuda → hevc_nvenc
```

- 输入：`AV_PIX_FMT_CUDA` + P010（PQ/HLG）  
- 输出：nv12 / p010；双向 hdr2sdr / sdr2hdr  
- 参考：`tonemap_opencl`、`VLC d3d11`  

## 2. 代码结构

| 文件 | 职责 |
|------|------|
| `vf_tonemap_cuda.c` | 选项、hw_frames、矩阵、metadata、stats 调度 |
| `vf_tonemap_cuda.cu` | 色彩管线、tone op、调光、Y/UV kernel |
| `vf_tonemap_cuda.ptx.c` | PTX 嵌入（构建生成） |

Kernel：`tonemap_cuda_stats`；`tonemap_cuda_p010_y` / `_nv12_y`；`tonemap_cuda_p010_uv` / `_nv12_uv`。

## 3. 色彩管线设计

### 3.1 HDR→SDR 流程

```text
P010 YUV → 线性 RGB → HLG/PQ 逆 + HLG OOTF
  → rgb2rgb (2020→709) → [scene_adapt] → map_hdr_to_sdr
  → gamut_fit_upper → BT.709 gamma → YUV
```

- **Y**：逐像素 + 可选 output-domain unsharp  
- **UV**：2×2 **线性 RGB 平均后再 gamma**（对齐 opencl，避免 Cb/Cr 平均渗入）

### 3.2 关键设计决策

| 决策 | 理由 |
|------|------|
| 默认 `hable`（auto） | VLC filmic；bt2390 可选 |
| 保色相 tone map | `sig=max(R,G,B)` 曲线后 RGB 等比缩放 |
| `hable(32s/peak)/hable(32)` | 32× 中间调 + 平滑白点；避免 `hable(11.2)` 硬剪白爆/黄偏红 |
| `gamut_fit_upper` | 正溢出等比缩放，非拉向亮度轴 |
| UV 无 unsharp | Cr 过冲致金黄边发红 |
| `scene_adapt` 默认关 | 混合亮暗直播场景易全黑 |
| black/clarity 轻默认 | 补 SDR 通透，不替代 tone curve |

### 3.3 Hable 标定演进

| 标定 | 公式 | 问题 |
|------|------|------|
| 峰值映射 | `hable(s)/hable(peak)` | 整体偏黑 |
| VLC 字面 | `hable(32s/peak)/hable(11.2)` | 350nit+ 硬剪，Y 剪爆 + Cr → 黄偏红 |
| **当前** | `hable(32s/peak)/hable(32)` | 中间调亮、高光平滑 |

### 3.4 init 默认（代码层）

`init_tonemap_params()`：hdr2sdr 时 exposure/contrast/saturation=1.0，black=0.010，clarity=0.08，scene_adapt=0；sdr2hdr 时 black/clarity=0，target_peak 默认 10。参数语义与推荐用法见 integration 文档。

## 4. 问题与解决记录

### 4.1 功能与架构

| 问题 | 根因 | 解决 |
|------|------|------|
| 双向 tonemap | 仅 hdr2sdr | 重构 host/kernel + sdr2hdr metadata |
| sdr2hdr 爆亮 | 线性拉伸 | 默认 gamma 扩展 |
| stats API | `cuMemsetD32` 不存在 | `cuMemsetD8Async` + `cuMemcpy2DAsync` |

### 4.2 清晰度

| 问题 | 根因 | 解决 |
|------|------|------|
| 发糊 | 4K tonemap 后 scale 抹锐化 | 先 scale 再 tonemap；output-domain unsharp |
| unsharp 无效 | 源域高通与 tone 后不对齐 | Y 平面 output unsharp |

### 4.3 色彩

| 问题 | 根因 | 解决 |
|------|------|------|
| 红发灰 | gamut 拉向 luma | `gamut_fit_upper` |
| 白发粉 | gamma 后 Cb/Cr 平均 | 线性 RGB 平均再编码 |
| LED 偏色 | 逐通道 Hable | sig 等比缩放 |
| 饱和红被洗 | neutral 阈值过宽 | `rel_chroma<0.06` |

### 4.4 亮度

| 问题 | 根因 | 解决 |
|------|------|------|
| bt2390 偏暗硬 | roll-off 过早 | 默认 hable |
| 发雾 | 缺调光 | black/clarity 链 |
| scene_adapt 全黑 | slope=0.25/avg | 默认关 |
| 白爆/黄偏红 | hable(11.2) 硬剪 | hable(32) 归一 |

### 4.5 明确不做

- UV unsharp  
- 降低 sdr2hdr `target_peak` 默认  
- 逐通道 Hable 默认  
- VLC 字面 `hable(11.2)` 默认  

## 5. 与 VLC / tonemap_opencl 对照

| 能力 | opencl | VLC d3d11 | tonemap_cuda |
|------|--------|-----------|--------------|
| 默认 tone | 可配 | Hable 32/11.2 逐通道 | Hable 32/32 保色相 |
| scene_adapt | 多帧 | 无 | 可选，默认关 |
| UV | 线性 RGB 平均 | — | 对齐 opencl |
| 后级调光 | 有限 | 无 | black/clarity/contrast |

## 6. 验证清单（开发）

- [ ] hdr2sdr HLG 4K，对比 VLC  
- [ ] 白/字幕/红蓝 LED 无色偏  
- [ ] 无白爆、黄边不泛红  
- [ ] sdr2hdr smoke + metadata  
- [ ] 非法 input tag 报错  

构建命令与直播示例见 [tonemap_cuda.md](./tonemap_cuda.md)。

## 7. 已知限制

1. SDR 无法完整覆盖 BT.2020 荧光色  
2. 4:2:0 块边界轻微色度渗入  
3. scene_adapt 单帧，无 opencl 滑动平均  
4. scene_adapt 依赖 stats kernel 额外 pass  

## 8. 变更时间线

1. 双向 tonemap + BT.2390  
2. exposure/contrast/saturation/desat/tint/unsharp  
3. output unsharp + 管线顺序  
4. gamut_fit_upper + UV 线性平均  
5. hable 默认 + 保色相  
6. black/clarity/scene_adapt + stats  
7. Hable 标定：peak → 32/11.2 → **32/32**  

## 9. 参数索引

完整说明（作用、推荐值、场景组合）见 [tonemap_cuda.md §滤镜参数完整说明](./tonemap_cuda.md#滤镜参数完整说明)。

---

| 路径 | 说明 |
|------|------|
| `.cursor/plans/tonemap_cuda.md` | 集成与使用 |
| `.cursor/plans/tonemap_cuda.md` | 本文档 |

*最后更新：2026-06-11*

## 参数说明 · 用法示例 · 注意事项 · 踩坑

全 GPU HDR↔SDR 动态范围映射：P010 CUDA 帧 → BT.709 SDR 或 PQ/HLG HDR 输出。

> 色彩管线设计、Bug 根因与标定演进详见 [tonemap_cuda.md](./tonemap_cuda.md)。

## 功能概览

| 能力 | 说明 |
|------|------|
| HDR→SDR | HLG/PQ → NV12/P010（BT.709），默认 Hable |
| SDR→HDR | BT.709 → PQ/HLG + HDR10 metadata |
| 调光 | black / clarity / contrast / desat / unsharp 等 |
| 全 GPU | 与 cuvid、scale_cuda、nvenc 串联 |

## 推荐管线

### 直播 HDR→SDR（HLG 4K）

```text
hevc_cuvid → fps → tonemap_cuda hdr2sdr → scale_cuda → hevc_nvenc
```

```bash
-filter_complex "[0:v:0]fps=fps=25,\
tonemap_cuda=direction=hdr2sdr:input_transfer=hlg:input_matrix=bt2020:input_primaries=bt2020:tonemap=hable:peak=10:t=bt709:m=bt709:p=bt709:format=nv12,\
scale_cuda=format=nv12,setdar=dar=a[vout0];..."
```

- HLG 源**不要**前置 `hlg2pq_cuda`  
- `peak=10` = 1000 nits 假定  
- 勿叠 `temperature`、`scene_adapt=1` 等偏离默认的参数  

### 4K→1080p + 锐化（先 scale 再 tonemap）

```text
scale_cuda=1920:1080:interp_algo=lanczos:format=p010 → tonemap_cuda ... unsharp=0.3
```

### PQ / SDR→HDR / 与 detect_cuda 串联

见下文「典型场景推荐组合」；detect 用法见 [detect_cuda.md](./detect_cuda.md)。

---

## 编译

```bash
./configure --enable-cuda-nvcc --enable-cuvid --enable-nvenc --enable-libnpp \
  --enable-filter=tonemap_cuda
make V=1 libavfilter/vf_tonemap_cuda.o libavfilter/vf_tonemap_cuda.ptx.o ffmpeg
ffmpeg -filters | grep tonemap_cuda
```

---

## 使用示例

### 直播转码

```bash
./ffmpeg -hwaccel cuvid -hwaccel_device 0 \
  -c:v hevc_cuvid -f live_flv -i "rtmp://..." \
  -filter_complex "[0:v:0]fps=fps=25,\
tonemap_cuda=direction=hdr2sdr:input_transfer=hlg:input_matrix=bt2020:input_primaries=bt2020:tonemap=hable:peak=10:t=bt709:m=bt709:p=bt709:format=nv12,\
scale_cuda=format=nv12,setdar=dar=a[vout0]; \
[0:a:0]volume=1.0,aresample=osr=48000[ain0]" \
  -map '[vout0]' -map '[ain0]' \
  -vcodec hevc_nvenc -gpu 0 -b:v 2000000 -r 25 -g 50 -profile:v main \
  -acodec libfdk_aac -ab 128000 -ac 2 -f flv "rtmp://..."
```

### 离线 HDR→SDR

```bash
ffmpeg -hwaccel cuda -hwaccel_output_format cuda -i input_hdr.mp4 \
  -vf "tonemap_cuda=direction=hdr2sdr:input_transfer=hlg:input_matrix=bt2020:\
input_primaries=bt2020:tonemap=hable:peak=10:t=bt709:m=bt709:p=bt709:format=nv12" \
  -c:v h264_nvenc -cq 23 out_sdr.mp4
```

### BT.2390 备选

```bash
tonemap_cuda=...:tonemap=bt2390:peak=10:param=0.8:...
```

偏暗偏硬，HLG 直播一般仍用 `hable`。

---

## 滤镜参数完整说明

参数来自 `vf_tonemap_cuda.c`。**代码默认**指 AVOption 初值或 init 后生效值；**推荐值**针对 HLG 4K 直播。

### 方向与 tone operator

| 参数 | 类型 | 代码默认 | 推荐值 | 说明 |
|------|------|----------|--------|------|
| `direction` | enum | `hdr2sdr` | `hdr2sdr` | `hdr2sdr` / `sdr2hdr` |
| `tonemap` | enum | `auto` | `hable` | auto：hdr2sdr→hable，sdr2hdr→gamma |
| `param` | float | NaN→算子默认 | 一般不设 | bt2390 默认 1.3；hable 忽略 |
| `peak` | float | 0→metadata | **`10`** | 源峰值 ×100 nits；越小越亮 |
| `target_peak` | float | 0→1/10 | hdr2sdr `1`；sdr2hdr **`10`** | 输出 mastering 峰值 |

| 算子 | 推荐场景 |
|------|----------|
| `hable` | **HLG/PQ 直播 HDR→SDR（默认）** |
| `bt2390` | 高光极冲、需更早 roll-off |
| `clip` | SDR 范围内像素须保真 |
| `gamma` 等 | 实验 / 特殊素材 |

### 输入 / 输出色彩标签

| 参数 | 别名 | 推荐值（HLG→SDR） | 说明 |
|------|------|-------------------|------|
| `input_transfer` | `it` | **`hlg`** | hlg / pq / bt709 |
| `input_matrix` | `im` | **`bt2020`** | |
| `input_primaries` | `ip` | **`bt2020`** | |
| `transfer` | `t` | **`bt709`** | |
| `matrix` | `m` | **`bt709`** | |
| `primaries` | `p` | **`bt709`** | |
| `range` | `r` | **`tv`** | limited |
| `format` | — | **`nv12`** | nv12 / p010 |

### 调光与观感（HDR→SDR）

| 参数 | 代码默认 | 生效默认 | 推荐值 | 说明 |
|------|----------|----------|--------|------|
| `exposure` | 0→1.0 | 1.0 | **1.0** | tone 前曝光 |
| `contrast` | 0→1.0 | 1.0 | **1.0** | 阴影 gamma |
| `saturation` | 0→1.0 | 1.0 | **1.0** | 全局饱和 |
| `black` | 0→0.010 | 0.010 | **0.01~0.02** | 黑位压缩 |
| `clarity` | 0→0.08 | 0.08 | **0.08~0.12** | 中间调通透 |
| `scene_adapt` | -1→0 | 0 | **0** | 直播慎用 |
| `desat` | 1.0 | 1.0 | **1.0**；冲时 **0.8~1.2** | 近中性高光去饱和 |
| `tint` | 0 | 0 | **0** | 绿/品红 ±1 |
| `temperature` | 0 | 0 | **0** | 暖/冷 trim |
| `unsharp` | 0 | 0 | **0.25~0.35** | Y 锐化；目标分辨率 tonemap |
| `pool_size` | 32 | 32 | 32 | hwframe 池 |

---

## 典型场景推荐组合

| 场景 | 参数片段 |
|------|----------|
| HLG 直播默认 | `direction=hdr2sdr:it=hlg:im=bt2020:ip=bt2020:tonemap=hable:peak=10:t=bt709:m=bt709:p=bt709:format=nv12` |
| 1080p + 锐化 | 先 `scale_cuda=1920:1080:interp_algo=lanczos:format=p010`，再 `...:unsharp=0.3` |
| 整体偏暗 | `peak=8` |
| 高光仍冲 | `peak=12` 或 `desat=1.0` |
| 发雾 | `black=0.02:clarity=0.12` |
| 固定机位 | `scene_adapt=1`（慎用） |
| PQ 强高光 | `tonemap=bt2390:param=1.0:it=pq:...` |
| SDR→HDR | `direction=sdr2hdr:it=bt709:im=bt709:ip=bt709:t=smpte2084:m=bt2020:p=bt2020:target_peak=10:format=p010` |

---

## 滤镜参数速查（简表）

| 参数 | 默认 | 说明 |
|------|------|------|
| `direction` | hdr2sdr | |
| `tonemap` | auto | |
| `peak` | metadata | HLG 推荐 10 |
| `it/im/ip` → `t/m/p` | 见上 | 色彩标签 |
| `black/clarity/scene_adapt/unsharp` | 见上 | 调光 / 锐化 |
| `pool_size` | 32 | |

完整说明见上一节 [滤镜参数完整说明](#滤镜参数完整说明)。

---

## 常见问题

| 现象 | 处理 |
|------|------|
| 整体发糊 | 先 scale 再 tonemap；`unsharp` + lanczos |
| 整体偏黑 | `peak=8` 或 `tonemap=hable` |
| 白爆、字幕黄偏红 | 确认 `hable(32)` 标定版本；可 `peak=12` / `desat` |
| 白发粉 | 略增 `desat` |
| 画面全黑 | 关 `scene_adapt` |
| 发雾 | `black=0.02:clarity=0.12` |
| sdr2hdr 过亮 | 保持默认 `gamma` + `target_peak=10` |

设计层根因见 [tonemap_cuda.md §4](./tonemap_cuda.md#4-问题与解决记录)。

---

## 相关文件

| 路径 | 说明 |
|------|------|
| `.cursor/plans/tonemap_cuda.md` | 开发设计 |
| `.cursor/plans/tonemap_cuda.md` | 本文档 |
| `doc/filters.texi` | 官方 filter 文档 |

*最后更新：2026-06-11*
