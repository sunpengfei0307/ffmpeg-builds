# libplacebo HDR→SDR（CUDA + Vulkan）

`hevc_cuvid` → `scale_cuda` → `hwupload=derive_device=vulkan` → `libplacebo` → `hwdownload` → `h264_nvenc`。

## 变更记录

| 日期 | 说明 |
|------|------|
| 2026-09-02 | 对齐 FFmpeg 6：NVIDIA Vulkan 约束重新列出 `AV_PIX_FMT_CUDA`，否则 8 会在 `scale_cuda` 与 `hwupload` 之间插入软件 `auto_scale` 并报 ENOSYS |

## 原因

FFmpeg 6（本仓库 `ffmpeg-6.1.1.iqiyi`）在 `vulkan_frames_get_constraints()` 里，NVIDIA 设备会把 `AV_PIX_FMT_CUDA` 加进 `valid_sw_formats`。`hwupload` 用这份列表当输入格式，因此 `scale_cuda`（输出 `cuda`）能直接接到 `hwupload=derive_device=vulkan`。

FFmpeg 8 上游去掉了这一条。图协商时 CUDA 与 Vulkan/软件格式对不上，自动插入 `scale`（`auto_scale_0`），软件 scale 不能吃 CUDA 帧：

```text
Impossible to convert between the formats supported by the filter
'Parsed_scale_cuda_2' and the filter 'auto_scale_0'
src: cuda
dst: yuv420p ...
Function not implemented (-38)
```

传输层本身仍支持 CUDA↔Vulkan（`vulkan_transfer_data_from_cuda`）。缺的是协商阶段的格式列表。

## 命令（与 6 相同）

```bash
./ffmpeg -hwaccel cuda -hwaccel_output_format cuda \
  -init_hw_device cuda=cu:0 -init_hw_device vulkan=vk@cu \
  -hwaccel_device cu -filter_hw_device vk \
  -c:v hevc_cuvid -i "rtmp://..." \
  -filter_complex "[0:v:0]setparams=color_primaries=bt2020:colorspace=bt2020nc:color_trc=smpte2084,fps=fps=50,scale_cuda=w=1920:h=1080:format=yuv444p16le,hwupload=derive_device=vulkan,libplacebo=...:format=yuv420p,hwdownload,format=yuv420p,...[vout0];..." \
  -map '[vout0]' -c:v h264_nvenc ...
```

## 注意事项

- 必须 `-init_hw_device vulkan=vk@cu`（从 CUDA 派生 Vulkan）且 `-filter_hw_device vk`
- NVIDIA 卡（vendor `0x10de`）；其它 GPU 不会把 CUDA 列入 Vulkan 约束
- 需要 CUDA 与 Vulkan 外部内存/信号量扩展（Linux：FD memory + FD semaphore）

## 踩坑

| 现象 | 处理 |
|------|------|
| `Parsed_scale_cuda_*` → `auto_scale_*`，`src: cuda` | 旧 8 二进制缺 CUDA 约束；需带本次 `hwcontext_vulkan.c` 修复 |
| `A hardware device reference is required` | 缺 `-filter_hw_device vk` 或未 `init_hw_device vulkan` |
| 图能连上但 `Failed to upload frame` | CUDA–Vulkan interop 扩展未开，或 Vulkan 不是从该 CUDA 设备派生 |
