# color_cuda

CUDA 纯色底图源（类 CPU `color`）。会议合屏中作 `mixing_cuda` 的 `bg_input` 节目时钟。整链路见 [mixing_cuda.md](./mixing_cuda.md)。

## 变更记录

| 日期 | 说明 |
|------|------|
| 2026-08-06 | `format=nv12\|p010`；表驱动校验；文档独立成册 |
| 2026-08-04 | 合屏建议与业务同 `r`，防侧窗启动卡/黑 |

## 设计方案（核心功能）

1. **恒定 CUDA 底色帧**：无 `hwdownload`，可驱动 FrameSync
2. **`format=nv12|p010`**：与 `mixing_cuda` / NVDEC 一致（mixing **不**做 8↔10 互转）
3. **元数据**：`colorspace` / `color_trc` / `out_range` 可显式或按 format 推断

| 文件 | 职责 |
|------|------|
| `vsrc_color_cuda.c` | 选项、hwframes、元数据 |
| `vsrc_color_cuda.cu` | 按 format 填 Y/UV |

`bg_input=0` 时本源驱动出帧；业务断流只漏底色。构建：`CONFIG_COLOR_CUDA_FILTER`。

## 参数说明

| 选项 | 默认 | 常用建议 | 作用 |
|------|------|----------|------|
| `color`/`c` | `black` | 会议底 `0x19271F` / `0x645f55` | 填充色 |
| `size`/`s` | `1920x1080` | 与 mixing 同画幅 | 输出尺寸 |
| `rate`/`r` | `60` | **与业务同 fps**，如 `25` | 节目时钟帧率 |
| `duration`/`d` | `-1` | 直播保持默认 | 时长 |
| `sar` | `1` | `1` | 采样宽高比 |
| `format` | `nv12` | SDR=`nv12`；HDR=`p010` | sw_format |
| `device` | `0` | 无全局 hwdevice 时用 | CUDA 设备 |
| `colorspace`/`space` | auto | SDR bt709；P010 bt2020nc | 矩阵 |
| `color_trc` | auto | SDR bt709；PQ/HLG 按链路 | 传输特性 |
| `out_range` | limited | 直播 **`tv`/`limited`** | range |

**`r` 务必与 `dynamic_input` / 业务目标 fps 一致**（默认 60 偏高，会议请写 `r=25`）。

## 用法示例

```text
# SDR 会议底板
color_cuda=c=0x19271F:s=1920x1080:r=25:format=nv12

# 合屏片段
color_cuda=c=0x645f55:s=1920x1080:r=25[bg];
[bg][v0][v1]...mixing_cuda=inputs=5:bg_input=0:...

# HDR
color_cuda=c=0x101010:s=1920x1080:r=25:format=p010:colorspace=bt2020nc:color_trc=smpte2084
```

## 注意事项

- `s`/`r`/`format` 须与 mixing 及业务 NVDEC 一致
- 运行时可通过 zmq/`process_command` 改 `color`

## 踩坑记录

| 现象 | 处理 |
|------|------|
| 侧窗启动卡/黑 | `r` 与业务一致；见 mixing `hold_stall_ms` |
| format mismatch | 底色/业务/mixing 的 nv12↔p010 不一致 |
| 无 CUDA 设备 | `-init_hw_device cuda` 或 `device=` |
