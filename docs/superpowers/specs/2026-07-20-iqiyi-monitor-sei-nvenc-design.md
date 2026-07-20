# 日志 Post / SEI / NVENC ROI 迁移设计规格

日期：2026-07-20  
状态：已确认  
范围：迁入 FFmpeg 8.1.2；参考 `ffmpeg-6.1.1.iqiyi`（公共抽象/Post/sei_copy）与 `ffmpeg4.4.1.iqiyi`（NVENC ROI、`b_scale_ratio`）

## 背景与目标

在 8.1.2 还原生产侧能力：

1. 写日志（`spfutils` Logger / msg/err）
2. `abnormal_timeout` 僵尸监控 + 异步 report
3. ZMQ 状态 Post（`avs_poster` / `status_publish`）
4. `sei_copy`（demux 提取 → mux 插入）
5. NVENC：`roi_qp_map` / `roi_qp_mode` / `roi_qp_strength` / `b_scale_ratio`（**含 h264 / hevc / av1**）

分层硬约束：

- 多模块公用能力（含全部 `zmq_*`）→ `spfutils.h`
- 依赖 libav* 的抽象 → `utils_stream.h`
- 业务/编码文件禁止直接调用 `zmq_*`

## 参考优先级

| 主题 | 主参考 |
|------|--------|
| `IZeromq`、Logger、`ITimer`、头文件结构 | 6.1.1 `fftools/spfutils.h` |
| `AVSPoster` / status Post / sei_copy / SEI NALU | 6.1.1 `utils_stream.h` + demux/mux |
| NVENC ROI / `b_scale_ratio` / 动态码率命令 | 4.4 `nvenc*.c/h` |
| 挂点适配 | 8.1.2 模块化 fftools（Scheduler 架构） |

## 1. 分层与文件布局

```
fftools/
  header.h
  spfutils.h                 # Logger、ITimer、线程/Queue、IZeromq（唯一 ZMQ 入口）
  utils_stream.h             # AV* 辅助、SEI NALU、AVSPoster、SEIPacket、htable SEI 缓存 API
  uthash.h / yyjson.{c,h}    # 随 6.1.1 拷贝（若 spfutils 依赖）
  ffmpeg_monitor.c/.h        # abnormal、zombine_monitor、poster 生命周期、print_report 衔接
  ffmpeg_sei.c/.h            # SEI parser 线程（可选）+ demux/mux 可调用入口

libavcodec/
  nvenc.h / nvenc.c
  nvenc_h264.c / nvenc_hevc.c / nvenc_av1.c   # 均暴露 ROI 选项并走公共 ROI 路径
```

命名说明：业务模块使用 `ffmpeg_monitor` / `ffmpeg_sei`（**不含**中间段 `iqiyi`）。

### 职责边界

| 层 | 放什么 | 禁止 |
|----|--------|------|
| `spfutils.h` | Logger；`IZeromq`（create/delete/config、`zmqbuf_send/recv`）；`ITimer`；线程/Queue | `AV*`、业务 payload |
| `utils_stream.h` | `AVStrErr`；SEI 构建（含 `build_sei_nalu_packet_copy`）；`SEIPacket`；`AVSPoster` create/update/publish；`parse_sei_nalus` / `add_sei2cacher` 等 | 直接 `zmq_*`（只调 `IZeromq`） |
| `ffmpeg_monitor.c` | 选项驱动的初始化；挂 `print_report`/`transcode`；僵尸线程 | 第二套 ZMQ |
| `ffmpeg_sei.c` | SEI SUB 线程、与 demux/mux 钩子 | 第二套 ZMQ / 重复 NALU 实现 |
| `nvenc*` | ROI map、`b_scale_ratio`、可选动态命令（经 `IZeromq`） | 直接 `zmq_*` |

可改进（相对 6.1.1）：`create/update/publish_avs_poster` 从巨型 `ffmpeg.c` 下沉到 `utils_stream`；业务只调 `poster_tick()` 一类入口。

## 2. 8.1.2 挂接点

| 能力 | 挂点 | 调用 |
|------|------|------|
| CLI | `ffmpeg_opt.c`：`abnormal_timeout`、`avs_poster_zmq_url`、`task_id`、`copy_sei` 等 | 初始化 monitor/sei；`-task_id N` 未指定 URL 时默认 `ipc:///data/LCMS/sock/N_running_status.sock` |
| 状态 Post | `ffmpeg.c`：`print_report` / `transcode`；`ITimer` + `async_printer` | `utils_stream` poster API |
| 僵尸监控 | `transcode` 启动 monitor；循环刷新 `last_update_time` | `ffmpeg_monitor` |
| sei_copy 提取 | `ffmpeg_demux.c` 视频 pkt | `parse_sei_nalus` / `add_sei2cacher` |
| sei_copy 插入 | `ffmpeg_mux.c` 写视频 pkt 前 | `htable_search_near` + `build_sei_nalu_packet_copy` |
| OST 字段 | `ffmpeg.h`：`usr_sei_dict`、`min_sei_pts`；`ffmpeg_mux_init` 建表 | |
| 构建 | `fftools/Makefile` 增加对象；`EXTRALIBS-ffmpeg` 链 libzmq（`utils/scripts/50-libzmq.sh` 已有） | |

## 3. NVENC（h264 / hevc / av1）

三套编码器选项表均增加（语义对齐 4.4；默认可按 4.4）：

| 选项 | 默认 | 行为 |
|------|------|------|
| `roi_qp_map` | 0 | 启用则消费 `AV_FRAME_DATA_REGIONS_OF_INTEREST` |
| `roi_qp_mode` | `delta`（`emphasis` / `delta`） | 设置 `qpMapMode` |
| `roi_qp_strength` | 24（命令行可用 20） | `qoffset` → 块 QP 缩放 |
| `b_scale_ratio` | 1.4 | VBR：`maxBitRate≈(r-0.1)*avg`，`vbvBufferSize≈r*avg`；≤0.1 → 重置 1.4 |
| `nv264_cmd_url`（可选） | — | 动态码率/`vb_scale_ratio`；经 `IZeromq` SUB |

实现要点：

- ROI 核心逻辑放在 `nvenc.c` 公共函数（如 `nvenc_setup_roi_qp_map`），h264/hevc/**av1** 共用
- 受 SDK 宏约束时用 `NVENC_HAVE_QP_MAP_MODE` 等守卫；av1 与 h264/hevc 同一选项面，底层按 codec 能力填充 `qpDeltaMap`
- detect_cuda 等滤镜写出的 ROI side data 可直接被上述选项消费

## 4. 数据流（摘要）

```
demux 视频 pkt → parse_sei_nalus → usr_sei_dict
                      ↓
mux 写 pkt 前 ← build_sei_nalu_packet_copy ← htable_search_near

print_report / ITimer
  → poster_update(统计)
  → poster_publish → IZeromq PUB → ipc/..._running_status.sock

detect_cuda / 其它 ROI 源
  → AV_FRAME_DATA_REGIONS_OF_INTEREST
  → nvenc_* (roi_qp_map=1) → qpDeltaMap
```

## 5. 错误处理与成功标准

| 场景 | 行为 |
|------|------|
| `abnormal_timeout>0` 且主循环超时 | 强制退出（对齐 6.1.1 zombine_monitor） |
| 10s 丢帧率持续恶化 / 帧数过低 | `print_report` 返回失败 → async_printer 退出 |
| ZMQ PUB/SUB 失败 | `err` 日志；不静默吞掉 |
| `roi_qp_map=0` | 忽略 ROI side data（可限次提示） |
| av1 设备/SDK 不支持 QP map | 明确 `AVERROR`/`ENOTSUP` 或降级 + 日志（实现时按 SDK 探测二选一，默认硬失败以免静默无效） |

成功标准：

- CLI 可配置 abnormal / poster URL / sei；状态经 ZMQ 投递
- sei_copy demux→mux 行为对齐 6.1.1
- h264/hevc/**av1** 均支持 `-roi_qp_map 1 -roi_qp_mode delta -roi_qp_strength 20` 与 `b_scale_ratio`
- 业务与 nvenc 源码无裸 `zmq_*`

## 非目标

- 不整文件覆盖 8.1.2 `ffmpeg.c`
- 不把 ZMQ 实现散落到 monitor/sei/nvenc
- 不强制改动现网 `status_publish` query 字段名（保持兼容；内部可重构）

## 实现顺序（建议）

1. 拷贝并整理 `header.h` / `spfutils.h`（含 `IZeromq`）/ `utils_stream.h` / 依赖头
2. `ffmpeg_monitor` + opt/print_report/transcode 挂接
3. `ffmpeg_sei` + demux/mux sei_copy
4. NVENC 公共 ROI + h264/hevc/av1 选项
5. 构建接入 libzmq 与 Makefile
6. 联调：poster、sei_copy、ROI 管线（含 detect_cuda→nvenc）
