# Monitor / SEI / NVENC ROI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 6.1.1 日志 Post/sei_copy 与 4.4 NVENC ROI 迁入 FFmpeg 8.1.2，分层：`spfutils` / `utils_stream` / `ffmpeg_monitor` / `ffmpeg_sei`。

**Architecture:** ZMQ 仅存于 `spfutils.h`（`IZeromq`）；SEI/NALU/poster publish 在 `utils_stream`；业务薄模块挂 fftools；ROI 公共逻辑在 `nvenc.c`，h264/hevc/av1 共用。

**Tech Stack:** FFmpeg 8.1.2、libzmq（`50-libzmq.sh`）、ffnvcodec

## Global Constraints

- 业务/nvenc 禁止直接 `zmq_*`
- 命名：`ffmpeg_monitor` / `ffmpeg_sei`（无 iqiyi 中间段）
- av1 必须有 ROI 选项与公共路径
- 不整文件覆盖 `ffmpeg.c`

---

### Task 1: 拷贝公共头与依赖

**Files:** Create under `ffmpeg/ffmpeg-8.1.2/fftools/` from 6.1.1: `header.h`, `spfutils.h`, `utils_stream.h`, `uthash.h`, `yyjson.c`, `yyjson.h`

- [x] Copy files
- [ ] Fix 8.1.2 API breaks in `utils_stream.h`（如 `channel_layout` → `ch_layout` 等，按编译错误修）
- [x] Makefile 后续统一加对象

### Task 2: NVENC ROI + b_scale_ratio（h264/hevc/av1）

**Files:** `libavcodec/nvenc.h`, `nvenc.c`, `nvenc_h264.c`, `nvenc_hevc.c`, `nvenc_av1.c`；参考 4.4

- [x] 扩展 `NvencContext` 字段
- [x] `nvenc_setup_roi_qp_map` + RC 中应用 `b_scale_ratio`
- [x] 三套 options 表增加选项
- [x] 送帧路径调用 ROI setup

### Task 3: ffmpeg_monitor + CLI/print_report

**Files:** `ffmpeg_monitor.c/.h`, `ffmpeg_opt.c`, `ffmpeg.c`, `ffmpeg.h`, `Makefile`

- [x] abnormal_timeout / zombine_monitor / poster init
- [x] 挂 print_report / transcode
- [x] 链 libzmq

### Task 4: ffmpeg_sei + demux/mux sei_copy

**Files:** `ffmpeg_sei.c/.h`, `ffmpeg_demux.c`, `ffmpeg_mux.c`, `ffmpeg_mux_init.c`, `ffmpeg.h`

- [x] usr_sei_dict 字段与建表
- [x] demux 提取 / mux 插入
- [ ] 可选 SEI SUB 线程

### Task 5: 构建与冒烟

- [x] `fftools/Makefile` + configure/extralibs
- [ ] 编译通过；静态检查无裸 `zmq_*`（nvenc/monitor/sei）
