# 进程级 ZMQ（`-zmq`）

进程内一条 REP，把命令派给 **已实现 `process_command` 的** filter / codec / format / protocol / fftools。  
图内 `zmq` / `azmq` **不改、不替代**；合屏可以只开 `-zmq`，不必再串图内 `zmq`。

## 变更记录

| 日期 | 说明 |
|------|------|
| 2026-08-20 | 全量命令手册：合屏三滤镜增删改查 / enable、volume、nvenc 批量与分路码率、concat `src` 切源、ffmpeg 日志与退出；concat demuxer 接入 `-zmq` |
| 2026-08-20 | 明确：不挂图内 `zmq` 时，`-zmq` 仍可给所有滤镜发命令 |
| 2026-08-19 | 合屏完整启动示例：图内 `*_filter.sock` + `-zmq` `*_cmd.sock` |
| 2026-08-17 | 新增 `-zmq`；opt-in `process_command`；nvenc 支持 bitrate / maxrate / bufsize / b_scale_ratio |

## 和滤镜 zmq 的关系

| | 图内 `zmq`/`azmq` | 进程 `-zmq` |
|--|-------------------|-------------|
| 启动 | filtergraph 里串滤镜 | `ffmpeg -zmq ipc:///path.sock ...` |
| 作用域 | **该图** | 整个 ffmpeg 进程（多输入 / 多输出） |
| 老命令 | `volume volume 0.5` 打到 filter sock | 同样格式也可打到 `-zmq` sock |
| 未实现回调 | 该滤镜 `ENOSYS` | 该 codec/format/protocol `ENOSYS`，其它组件不受影响 |
| 滤镜 REP | 带回 `process_command` 真实返回值 | 滤镜命令回的是「已投递」；结果打在日志 `Command reply for stream N` |

合屏滤镜命令可以只打 `-zmq` sock。图内 sock 仅在老客户端仍连 `*_filter.sock` 时需要。滤镜细节另见 [mixing_cuda.md](./mixing_cuda.md) §7。

## 启动

合屏完整模板与 [mixing_cuda.md](./mixing_cuda.md) §3.6 相同：

```bash
./ffmpeg -task_id 123456 \
  -zmq ipc:///data/LCMS/sock/123456_cmd.sock \
  -init_hw_device cuda=hw:0 -filter_hw_device hw \
  -filter_complex '
color_cuda=c=0x19271F:s=1920x1080:r=25[bg];
dynamic_input=outputs=16:enable_audio=1:live=1:s=1920x1080:r=25:sample_rate=48000:initial_urls=rtmp\\\://test-push.live.qiyi.domain/live/ls_src_dp_h264_1080p\\\,rtmp\\\://test-push.live.qiyi.domain/live/ls_src_zp_h264_1080p\\\,rtmp\\\://test-push.live.qiyi.domain/live/ls_src_zp_h264_1080p\\\,rtmp\\\://test-push.live.qiyi.domain/live/ls_src_zp_h264_1080p[v0][v1][v2][v3][v4][v5][v6][v7][v8][v9][v10][v11][v12][v13][v14][v15][a0][a1][a2][a3][a4][a5][a6][a7][a8][a9][a10][a11][a12][a13][a14][a15];
[bg][v0][v1][v2][v3][v4][v5][v6][v7][v8][v9][v10][v11][v12][v13][v14][v15]mixing_cuda=inputs=17:s=1920x1080:format=nv12:layout_mode=speaker:speaker_layout=obs:bg_input=0:stall_ms=800:border_px=2[vout];
anullsrc=r=48000:cl=stereo[asil];
[asil][a0][a1][a2][a3][a4][a5][a6][a7][a8][a9][a10][a11][a12][a13][a14][a15]amixrank=inputs=17:duration=longest:normalize=0:ducking=1:duck_gain_db=-18:rank_interval_ms=100:weights=0|1|1|1|1|1|1|1|1|1|1|1|1|1|1|1|1[aout]
' \
  -map '[vout]' -map '[aout]' \
  -c:v h264_nvenc -preset p4 -b:v 6000k -maxrate 6000k -bufsize 12000k -b_scale_ratio 1.4 \
  -r 25 -g 50 \
  -c:a aac -b:a 128k -ar 48000 -ac 2 \
  -f flv rtmp://test-push.live.qiyi.domain/live/stream_out
```

可选：在 `color_cuda=...` 后串 `,zmq=b=ipc\\\:///data/LCMS/sock/123456_filter.sock`，给仍打 filter sock 的客户端用。音量链可再挂 `volume`（见下文）。

| sock | 用途 |
|------|------|
| `123456_cmd.sock` | `-zmq`（建议必加）：滤镜 + NVENC + concat + `ffmpeg quit` |
| `123456_filter.sock` | 图内 zmq（**可选**） |

下文示例默认：

```bash
CMD_SOCK="ipc:///data/LCMS/sock/123456_cmd.sock"
URL="rtmp://test-push.live.qiyi.domain/live/ls_src_zp_h264_1080p"
./asr_cmd "<target> <command> [arg…]" "$CMD_SOCK"
```

## 报文

与 `zmqsend` / 图内 zmq 相同：

```text
<target> <command> [arg…]
```

无前缀：先当滤镜；若名字正好是当前路上的编码器（如 `h264_nvenc`）则走 codec；若是当前路上的 demuxer（如 `concat`）则走输入格式。

| target | 含义 |
|--------|------|
| `dynamic_input` / `mixing_cuda` / `amixrank` / `volume` / `all` | 所有 simple + complex 图里同名滤镜 |
| `filter:volume` / `fg:0:volume` | 显式滤镜；`fg:N` 指定第 N 张图 |
| `enc:0` | 输出文件 0 上**所有** encoder |
| `enc:0:v:0` | 输出文件 0、第 0 路**视频** encoder |
| `enc:1:a:0` | 输出文件 1、第 0 路音频 encoder |
| `enc:h264_nvenc` / `h264_nvenc` | 所有该名字的 encoder |
| `dec:0:v:0` / `dec:0:a:0` | 输入文件 0 的指定 decoder（钩子在；当前解码器未实现命令则 `ENOSYS`） |
| `concat` / `if:0` / `demux:0` | concat 等输入 demuxer |
| `of:0` / `mux:0` | 输出 muxer（未实现则 `ENOSYS`） |
| `proto:of:0` / `proto:if:1` | 对应文件的 URL 协议（未实现则 `ENOSYS`） |
| `ffmpeg` | fftools：`quit` / `loglevel` |

未实现 `process_command` 的组件返回 `ENOSYS`，不会另 bind sock。

---

## 1. 合屏三滤镜：增删改查 / enable

槽号约定（与 [mixing_cuda.md](./mixing_cuda.md) §7.1 相同）：

| 编号 | 含义 |
|------|------|
| **0** | 底色 / `color_cuda`（`bg_input`），**不参与** `dynamic_input` 命令 |
| **1..N** | 业务槽，三滤镜同号 |

三滤镜 `set_active` **互不联动**：关一路应对同槽发齐（或用 `search` 对账）。Pad 数启动预留，运行时不增减、不重编号。

### 1.1 `dynamic_input`（源：增 / 删 / 改 / 查 / 开关）

| 命令 | 参数 | 行为 |
|------|------|------|
| `search` | 〔空〕/`2`/`1-3` | 查槽；回复 `槽:url:状态:active` |
| `update` | `<槽\|区间> [url…]` | **增 / 改**：打开成功再热挂；区间 URL 少则复用最后一个 |
| `remove` | `<槽\|区间> [url]` | **删**：软拆除（线程闲置，槽号不变） |
| `purge` / `purage` | （无） | 软拆除全部业务槽 |
| `set_active` | `<槽\|区间> <0\|1>` | **disable / enable**（1=出帧，0=关） |
| `live` | `0\|1` | 运行时改 `live` |

```bash
# 查
./asr_cmd "dynamic_input search" "$CMD_SOCK"
./asr_cmd "dynamic_input search 2" "$CMD_SOCK"
./asr_cmd "dynamic_input search 1-4" "$CMD_SOCK"

# 增 / 改（空槽 update = 插入；已有槽 = 换源）
./asr_cmd "dynamic_input update 5 $URL" "$CMD_SOCK"
./asr_cmd "dynamic_input update 5-8 $URL" "$CMD_SOCK"
./asr_cmd "dynamic_input update 1-4 $URL1 $URL2 $URL3 $URL4" "$CMD_SOCK"
./asr_cmd "dynamic_input update 1-4 $URL1 $URL2" "$CMD_SOCK"   # → u1,u2,u2,u2

# 删
./asr_cmd "dynamic_input remove 5" "$CMD_SOCK"
./asr_cmd "dynamic_input remove 1-3" "$CMD_SOCK"
./asr_cmd "dynamic_input purge" "$CMD_SOCK"

# enable / disable
./asr_cmd "dynamic_input set_active 2 0" "$CMD_SOCK"
./asr_cmd "dynamic_input set_active 2 1" "$CMD_SOCK"
./asr_cmd "dynamic_input set_active 1-4 0" "$CMD_SOCK"
```

废弃：`add` / `switch` / `cmd_*.sock`。`remove` 后再上人必须 `update` **到原槽号**。

### 1.2 `mixing_cuda`（画面：查 / 开关 / 布局）

| 命令 | 参数 | 行为 |
|------|------|------|
| `search` | 〔空〕/`1`/`1-4` | `mode=… speaker=… switch=…` + `槽:active:holding:have_frame` |
| `set_active` | `<槽\|区间> <0\|1>` | 该格 enable / disable（不联动音频） |
| `layout_mode` | `fixed`/`adapt`/`adaptive` | 热切布局套；`speaker` 映射为 adapt |
| `layout` | JSON 整行 | 热改几何（fixed/adapt） |
| `reload` | （无） | 重读 `layout_file` 或 task 落盘 |
| `active_speaker` | `<pad>` | 手动主讲（`speaker_switch=0` 时） |
| `speaker_switch` | `0\|1` | 1=跟 amixrank 激励；0=仅手动 |
| `speaker_layout` | `obs`/`grid` | 主讲样式 |
| `slot_style` | `<pad> border_px=N radius_px=N border_color=C` | 单路边框 |
| `trans_effect` / `trans_ms` | 见 mixing §4.5 / §6 | 格子切换特效 |
| `tile_border_px` / `radius_px` / `tile_border_color` / `fill` / `border_color` | 运行时选项 | 经 `ff_filter_process_command` |

```bash
# 查
./asr_cmd "mixing_cuda search" "$CMD_SOCK"
./asr_cmd "mixing_cuda search 1-4" "$CMD_SOCK"

# enable / disable
./asr_cmd "mixing_cuda set_active 2 0" "$CMD_SOCK"
./asr_cmd "mixing_cuda set_active 2 1" "$CMD_SOCK"
./asr_cmd "mixing_cuda set_active 1-4 0" "$CMD_SOCK"

# 布局 / 主讲
./asr_cmd "mixing_cuda layout_mode adapt" "$CMD_SOCK"
./asr_cmd "mixing_cuda layout_mode fixed" "$CMD_SOCK"
./asr_cmd "mixing_cuda speaker_switch 0" "$CMD_SOCK"
./asr_cmd "mixing_cuda active_speaker 2" "$CMD_SOCK"
./asr_cmd "mixing_cuda speaker_switch 1" "$CMD_SOCK"
./asr_cmd "mixing_cuda speaker_layout obs" "$CMD_SOCK"
./asr_cmd "mixing_cuda reload" "$CMD_SOCK"
./asr_cmd "mixing_cuda trans_effect slide_left" "$CMD_SOCK"
./asr_cmd "mixing_cuda trans_ms 800" "$CMD_SOCK"
./asr_cmd "mixing_cuda tile_border_px 2" "$CMD_SOCK"
./asr_cmd "mixing_cuda slot_style 1 border_px=3 radius_px=12 border_color=#FF6644" "$CMD_SOCK"
```

`layout` JSON 示例见 [mixing_cuda.md](./mixing_cuda.md) §7.3。

### 1.3 `amixrank`（混音：查 / 开关）

业务槽 **1..N**（`input0=anullsrc` 不可关）。**只控音频**。

| 命令 | 参数 | 行为 |
|------|------|------|
| `search` | 〔空〕/`1`/`1-4` | `槽:enabled:weight` |
| `set_active` | `<槽\|区间> <0\|1>` | disable：不进混/rank + 排空 FIFO；enable：按 weights 重入 |
| `buffer_ms` / `weights` / `normalize` / `ducking` / `duck_gain_db` / `duck_*_ms` / `join_fade_ms` | 运行时选项 | 见 [amixrank.md](./amixrank.md) |

```bash
./asr_cmd "amixrank search" "$CMD_SOCK"
./asr_cmd "amixrank search 1-4" "$CMD_SOCK"
./asr_cmd "amixrank set_active 2 0" "$CMD_SOCK"
./asr_cmd "amixrank set_active 2 1" "$CMD_SOCK"
./asr_cmd "amixrank set_active 1-4 0" "$CMD_SOCK"
./asr_cmd "amixrank buffer_ms 200" "$CMD_SOCK"
./asr_cmd "amixrank ducking 1" "$CMD_SOCK"
./asr_cmd "amixrank duck_gain_db -18" "$CMD_SOCK"
```

同槽音画一起关（推荐）：

```bash
./asr_cmd "dynamic_input set_active 2 0" "$CMD_SOCK"
./asr_cmd "mixing_cuda set_active 2 0" "$CMD_SOCK"
./asr_cmd "amixrank set_active 2 0" "$CMD_SOCK"
```

---

## 2. `volume`（音量 / 静音）

图里要有 `volume` 滤镜，例如 `[aout]volume=1.0[aout2]`，`-map` 改成 `[aout2]`。命令名与滤镜名相同：

```bash
./asr_cmd "volume volume 1.0" "$CMD_SOCK"     # 原量
./asr_cmd "volume volume 0.5" "$CMD_SOCK"     # -6 dB 量级（线性增益）
./asr_cmd "volume volume 0" "$CMD_SOCK"       # 静音（disable 音量）
./asr_cmd "filter:volume volume 0.8" "$CMD_SOCK"
```

多路输出时用实例名（如 `Parsed_volume_0`）或 `fg:N:volume` 指定图。

---

## 3. `color_cuda`（底板色）

```bash
./asr_cmd "color_cuda color 0x19271F" "$CMD_SOCK"
./asr_cmd "color_cuda c 0x645f55" "$CMD_SOCK"
```

---

## 4. 编码器：批量 / 特定路码率（nvenc）

已实现：`h264_nvenc` / `hevc_nvenc` / `av1_nvenc`。下一帧 `reconfig_encoder()` 生效。

| command | 说明 |
|---------|------|
| `bitrate` / `b` | 平均码率（**bps 整数**）；并按 `b_scale_ratio` 更新 maxrate/bufsize |
| `maxrate` | `rc_max_rate` |
| `bufsize` | `rc_buffer_size` |
| `b_scale_ratio` / `vb_scale_ratio` | 与 6.x `nv264_cmd_url` 相同 |

**必须打 `-zmq` sock。** 图内 filter sock 到不了 encoder。

```bash
# —— 批量：当前所有 h264_nvenc ——
./asr_cmd "h264_nvenc bitrate 4000000" "$CMD_SOCK"
./asr_cmd "enc:h264_nvenc bitrate 4000000" "$CMD_SOCK"
./asr_cmd "enc:0 bitrate 4000000" "$CMD_SOCK"          # 输出文件 0 上全部 encoder

# —— 特定路：输出文件 0、第 0 路视频 ——
./asr_cmd "enc:0:v:0 bitrate 4000000" "$CMD_SOCK"
./asr_cmd "enc:0:v:0 maxrate 6000000" "$CMD_SOCK"
./asr_cmd "enc:0:v:0 bufsize 12000000" "$CMD_SOCK"
./asr_cmd "enc:0:v:0 b_scale_ratio 1.4" "$CMD_SOCK"

# —— 双输出：主路 6M、预览路 2M ——
./asr_cmd "enc:0:v:0 bitrate 6000000" "$CMD_SOCK"
./asr_cmd "enc:1:v:0 bitrate 2000000" "$CMD_SOCK"
```

码率单位是 **bps**（`4000000` = 4000k），不要写 `4000k`。

### decoder

`dec:0:v:0` / `dec:0:a:0` 路径已通，**当前没有解码器实现 `process_command`**，会回 `ENOSYS`。改码率请走 encoder。

---

## 5. concat 切源

concat demuxer 已实现 `process_command`：`src <url>` 写入 `try_url` 并 `waiting=1`，由监控线程打开成功后再热切。

```bash
# 输入 0 是 concat 时（推荐显式下标）
./asr_cmd "if:0 src rtmp://test-push.live.qiyi.domain/live/ls_src_dp_h264_1080p" "$CMD_SOCK"
./asr_cmd "demux:0 src $URL" "$CMD_SOCK"

# 当前进程里有 concat 输入时，无前缀也可
./asr_cmd "concat src $URL" "$CMD_SOCK"
```

启动示例（concat 当输入，仍用进程 `-zmq`，不必再开 concat 自己的 sock）：

```bash
./ffmpeg -zmq ipc:///data/LCMS/sock/123456_cmd.sock \
  -f concat -safe 0 -i playlist.ffconcat \
  ...
```

老用法仍可用 demuxer 选项 `concat_cmd=`（默认占位 `ipc://cmd_xxxx.sock`，生产请改成真路径或干脆只用 `-zmq`）。老 sock 报文同样是 `concat src <url>`。

---

## 6. ffmpeg 进程动作

| command | 参数 | 行为 |
|---------|------|------|
| `quit` / `q` / `exit` | （无） | 结束转码（同 Ctrl-C 的退出请求） |
| `loglevel` | `quiet`/`panic`/`fatal`/`error`/`warning`/`info`/`verbose`/`debug`/`trace` 或数字 | `av_log_set_level` |

```bash
./asr_cmd "ffmpeg loglevel warning" "$CMD_SOCK"
./asr_cmd "ffmpeg loglevel debug" "$CMD_SOCK"
./asr_cmd "ffmpeg loglevel 24" "$CMD_SOCK"     # 数字亦可
./asr_cmd "ffmpeg quit" "$CMD_SOCK"
```

---

## 7. muxer / protocol

`of:0` / `proto:of:0` 钩子在，**当前 flv/rtmp 等未实现 `process_command`**，回 `ENOSYS`。要加命令时在对应 `FFOutputFormat` / `URLProtocol` 上挂回调即可，broker 不用改。

---

## 给组件作者

与滤镜相同：有回调才解析命令。

```c
// libavcodec/codec_internal.h  FFCodec
int (*process_command)(AVCodecContext *avctx, const char *cmd, const char *arg,
                       char *res, int res_len, int flags);
```

format / protocol 同名钩子分别在 `libavformat/mux.h`、`demux.h`、`url.h`。

公开入口：`avcodec_process_command()`、`avformat_process_command()`、`ffurl_process_command()`。
