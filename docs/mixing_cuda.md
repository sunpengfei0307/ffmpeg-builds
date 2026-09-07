# mixing_cuda（OBS 式会议合屏）

三层职责：动态源（`[dynamic_input](./dynamic_input.md)`）、视频合屏（`mixing_cuda`）、音频混音（`[amixrank](./amixrank.md)`）；底板常用 `[color_cuda](./color_cuda.md)`、anullsrc。

### 变更记录


| 日期  | 说明  |
| --- | --- |
| 2026-09-07 | `-abnormal_timeout` 对视频 stream copy 不杀进程（无解码帧可判） |
| 2026-09-02 | `-report 前缀` 按启动时间写独立日志文件，见 [report.md](./report.md) |
| 2026-09-02 | HTTP 白名单拒绝回 403（不再静默断连），见 [http_server.md](./http_server.md) |
| 2026-09-02 | HTTP 操作/异常日志带 `([时间]` 前缀，见 [http_server.md](./http_server.md) |
| 2026-09-02 | HTTP-FLV/MP4 剥 AAC ADTS，见 [http_server.md](./http_server.md) |
| 2026-09-02 | HTTP-TS 裸 AAC 补 ASC extra，见 [http_server.md](./http_server.md) |
| 2026-09-02 | HTTP-FLV/MP4 Annex B→AVCC，见 [http_server.md](./http_server.md) |
| 2026-09-02 | HTTP 起播不等 extradata；connect/disconnect 成对日志，见 [http_server.md](./http_server.md) |
| 2026-09-02 | HTTP 拉流 accept/起播加速（`req=`/`wait=` 日志），见 [http_server.md](./http_server.md) |
| 2026-09-02 | `-http_live_bind` 多输出绑定；`?lowdelay=1`；HLS/DASH 不受 `-http_live` 影响，见 [http_server.md](./http_server.md) |
| 2026-08-20 | 图内 `zmq` 可选：有 `-zmq` 即可给所有滤镜发命令；全量命令手册见 [zmq.md](./zmq.md) |
| 2026-08-19 | 新版启动：图内 `123456_filter.sock` + 进程 `-zmq` `123456_cmd.sock`；§3.6 / §7 示例同步 |
| 2026-08-17 | 进程级 `-zmq` 与图内 zmq/azmq 并存，合屏命令仍走图内 sock，见 [zmq.md](./zmq.md) |


---

## 1. 架构总览

```text
                    ┌─────────────────────────────────────────────┐
  RTMP × N ────────►│ dynamic_input                                │
                    │  v0..vN (CUDA NV12)  a0..aN (PCM)           │
                    └──────────┬──────────────────┬───────────────┘
                               │                  │
         color_cuda ──►[bg]    │                  │
                               ▼                  ▼
                    ┌──────────────────┐   ┌─────────────────────┐
                    │ mixing_cuda      │   │ anullsrc + amixrank │
                    │ 布局 / 合屏 / 边框│   │ 时钟 + 响度 + duck  │
                    └────────┬─────────┘   └──────────┬──────────┘
                             │                        │
                             └──────────┬─────────────┘
                                        ▼
                              NVENC + AAC → FLV/RTMP
```


| 组件              | 职责                        | 时钟                                      |
| --------------- | ------------------------- | --------------------------------------- |
| `dynamic_input` | 多路 RTMP 拉流、重连、占位帧、音视频 pad | 视频可按墙钟 `r` 节流；音频随下游 `frame_wanted`      |
| `mixing_cuda`   | CUDA 合屏、布局、主讲边框、断流填底      | `bg_input`**（color_cuda）** 驱动 FrameSync |
| `amixrank`      | 响度排序、ducking、FIFO 硬顶控延迟   | `anullsrc`**（input0）**                  |


跨滤镜激励：`amixrank` PUB → `mixing_cuda` SUB。sock：`-task_id`/`FFMPEG_TASK_ID` → `gain_<N>.sock`，**未传则默认** `gain_<pid>.sock`。
滤镜命令：图内 `zmq`（`*_filter.sock`）**或**进程 `-zmq`（`*_cmd.sock`）均可；编码器码率 / `ffmpeg quit` 只能走 `-zmq`。见 §7、[zmq.md](./zmq.md)。

---



## 2. 依赖与确认

- `--enable-ffnvcodec`，且 `cuda_nvcc` 或 `cuda_llvm`
- 滤镜：`dynamic_input` / `mixing_cuda` / `amixrank`
- 激励通信：`--enable-libzmq`（可选；无 zmq 时 `sendcmd` 注入 `active_speaker`，ducking 仍可用）
- 输入/输出视频：**NV12 CUDA**

```bash
./ffmpeg -hide_banner -filters | grep -E 'dynamic_input|mixing_cuda|amixrank'
```

缺滤镜时在构建树：

```bash
cd /data/sunpf/ffmpeg-builds
FFMPEG_ONLY=1 ./build.sh
```

`config_components.h` 中应有 `#define CONFIG_DYNAMIC_INPUT_FILTER 1`。

---



## 3. 端到端用法（推荐模板）



### 3.1 fixed 布局（JSON 宫格）

```text
./ffmpeg -task_id 123456 \
  -report /data/LCMS/log/123456_mix \
  -zmq ipc:///data/LCMS/sock/123456_cmd.sock \
  -init_hw_device cuda=hw:0 -filter_hw_device hw \
  -filter_complex "
color_cuda=c=0x645f55:s=1920x1080:r=25,zmq=b=ipc\\\:///data/LCMS/sock/123456_filter.sock[bg];
dynamic_input=outputs=4:enable_audio=1:s=1920x1080:r=25:sample_rate=48000:playout_delay_ms=40:av_wait_ms=6000:pts_jump_s=10:initial_urls=rtmp\\://u0\\,rtmp\\://u1\\,rtmp\\://u2\\,rtmp\\://u3[v0][v1][v2][v3][a0][a1][a2][a3];
[bg][v0][v1][v2][v3]mixing_cuda=inputs=5:s=1920x1080:layout_mode=fixed:layout_file=./layout.json:bg_input=0:stall_ms=800:border_px=4:trans_effect=slide_right:trans_ms=2000[vout];
anullsrc=r=48000:cl=stereo[asil];
[asil][a0][a1][a2][a3]amixrank=inputs=5:normalize=0:ducking=1:duck_gain_db=-18:rank_interval_ms=100:weights=0|1|1|1|1:buffer_ms=200[aout]
" \
  -map '[vout]' -map '[aout]' \
  -c:v h264_nvenc -preset p4 -b:v 6000k -maxrate 6000k -bufsize 12000k -b_scale_ratio 1.4 \
  -r 25 -g 50 \
  -c:a aac -b:a 128k -ar 48000 -ac 2 \
  -f flv rtmp://out/live/stream
```



### 3.2 speaker 布局（内置主讲 + 缩略图）

`layout_file` **不会**生效（启动打 WARNING）。几何由 `speaker_layout=obs|grid` 决定。图内命令挂在 `color_cuda` 后的 `zmq`（见 3.1 / 3.6），不必再单独串在 mixing 后面。

```text
[bg][v0][v1][v2][v3]mixing_cuda=inputs=5:s=1920x1080:layout_mode=speaker:speaker_layout=obs:bg_input=0:stall_ms=800:border_px=4:trans_effect=slide_right:trans_ms=2000[vout]
```



### 3.3 adaptive 布局（按存活路数自动宫格）

```text
[bg][v0][v1][v2][v3]mixing_cuda=inputs=5:s=1920x1080:layout_mode=adaptive:bg_input=0:stall_ms=800[vout]
```



### 3.4 关键约定

- **时钟挂底色**：`bg_input=0` → 只有 `color_cuda` 驱动出帧；业务 RTMP 全是 secondary，断流只漏底色，不卡死整图
- **背景不进宫格**：`bg_input` 只作整幅底板。默认 2×2 / JSON 业务槽写 `input:1..4`（图为 `[bg][v0]…`）
- **同源音画**：A/V **同一 PTS 时间线**有序放出（`av_wait_ms=6000` 等对端、`pts_jump_s=10` 纠跳跃、`playout_delay_ms=40`）；输入正常时尽量保持源帧序
- **音频 weights**：用 `weights=0|1|1|1|1`（避免空格转义被吃成全 0）。input0=`anullsrc` 只作时钟
- **激励 sock**：`-task_id N` / `FFMPEG_TASK_ID` → `gain_<N>.sock`；都未设则 `gain_<pid>.sock`；也可 `rank_endpoint=`
- **命令 sock**：进程 `-zmq` → `ipc:///data/LCMS/sock/<task>_cmd.sock`（滤镜 + NVENC + quit）；图内 `zmq` → `*_filter.sock`（可选，老客户端）。见 §7、[zmq.md](./zmq.md)
- **持帧防闪**：解码间隙重复上一帧（`lavfi.dyn_hold` / 同 PTS）；持帧用 `hold_stall_ms`，硬断流用 `stall_ms`；`color_cuda` 建议与业务同 `r`
- **格子切换特效**：见 **§4.5**；同 URL `unchanged` 不播
- **换源编码/分辨率**：`update`/重连会重新 probe + 开解码器（H.264↔H.265 等），有 CUDA 设备时优先 NVDEC 出 CUDA 帧；`r` **是输出节拍，不会随输入 25↔50 自动改**（`color_cuda` 建议同 `r`）
- **三处** `s=`：`color_cuda`=底板尺寸；`mixing_cuda`=最终画幅；`dynamic_input`=各 pad 的 CUDA 帧池/链路声明尺寸（占位帧、软上传），**不强制缩放业务画面**（NVDEC 帧保持源分辨率，由 mixing 按格 fit）。建议与 mixing 同值以免链路协商异常。
- `format=nv12|p010`：`color_cuda` 与 `mixing_cuda` 均支持；mixing **不互转** 8/10bit——底色、业务 NVDEC、mixing 的 `format` 必须一致（SDR 用 `nv12`，10bit/HDR 链路用 `p010`）。实现上通过 `supported_formats[]` + `format_is_supported()` 校验（同 `scale_cuda`/`pad_cuda` 风格）。
- **不要**把 `primary_input` 指到业务 RTMP 路



### 3.5 路数上限与扩容（pad 预留）

FFmpeg 滤镜图的 **pad 数在建图时固定**，运行中不能用 zmq「再加一路 pad」。


| 限制                        | 现状                              |
| ------------------------- | ------------------------------- |
| `dynamic_input` `outputs` | 启动选项，最大 **32**（`DYN_INPUT_MAX`） |
| `mixing_cuda` `inputs`    | 启动选项，最大 **32**（含 `bg_input`）    |
| `amixrank` `inputs`       | 须与音频 pad 数一致（含 `anullsrc`）      |


**实用做法（推荐）**：按峰值预留 pad（常见 16 路业务，见 §3.6）；空槽 `remove`，需要时再 `update`。

**硬上限 32**：`DYN_INPUT_MAX` / `MIXING_CUDA_MAX_INPUTS`。再大需改宏重编。  
**不能指望**：运行时 `outputs++` / 动态 `ff_append_inpad`。

---



### 3.6 16 路业务模板（预留 16 pad，启动拉 4 路）

预留 16 个业务 pad + 1 路底色；启动拉 4 路 URL（1 导播 + 3 嘉宾），其余槽空着，用 `update` 随时填。  
布局用 `layout_mode=speaker`（主讲套）或 `adaptive`（equal 宫格套）。内置几何见 `vf_mixing_cuda_layout.c`。

两条控制 sock（可并存）。**只加 `-zmq`、不挂图内 `zmq` 也可以**给 `dynamic_input` / `mixing_cuda` / `amixrank` 发命令（进程 broker 注入滤镜线程，与键盘 `c` 相同）。图内 sock 留给仍打 `*_filter.sock` 的老客户端。

| sock | 来源 | 用途 |
|------|------|------|
| `ipc:///data/LCMS/sock/123456_filter.sock` | 图内 `zmq`（可选） | `dynamic_input` / `mixing_cuda` / `amixrank` |
| `ipc:///data/LCMS/sock/123456_cmd.sock` | `-zmq`（建议必加） | 滤镜命令 + NVENC 码率 + `ffmpeg quit` |

```text
./ffmpeg -task_id 123456 \
  -zmq ipc:///data/LCMS/sock/123456_cmd.sock \
  -init_hw_device cuda=hw:0 -filter_hw_device hw \
  -filter_complex '
color_cuda=c=0x19271F:s=1920x1080:r=25,zmq=b=ipc\\\:///data/LCMS/sock/123456_filter.sock[bg];
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

要点：


| 项              | 值                                  |
| -------------- | ---------------------------------- |
| 业务 pad         | `outputs=16` → 槽 **1..16**         |
| mixing         | `inputs=17`（bg+16），`bg_input=0`    |
| amixrank       | `inputs=17`，`weights=0` + 16 个 `1` |
| `initial_urls` | **4 路**（槽 1..4）；5..16 空，后续 `update` |
| 布局             | `speaker`=主讲套；`adaptive`=equal 宫格套 |
| 硬上限            | 32 路业务（`outputs=32` / `inputs=33`） |


实时控制（启动后）：

```bash
TASK=123456
FILTER_SOCK="ipc:///data/LCMS/sock/${TASK}_filter.sock"
CMD_SOCK="ipc:///data/LCMS/sock/${TASK}_cmd.sock"
URL="rtmp://test-push.live.qiyi.domain/live/ls_src_zp_h264_1080p"

# —— 滤镜（图内 zmq；也可打到 $CMD_SOCK）——
./asr_cmd "dynamic_input search" "$FILTER_SOCK"
./asr_cmd "dynamic_input update 5 $URL" "$FILTER_SOCK"
./asr_cmd "dynamic_input update 5-8 $URL" "$FILTER_SOCK"
./asr_cmd "dynamic_input remove 5" "$FILTER_SOCK"
./asr_cmd "dynamic_input set_active 2 0" "$FILTER_SOCK"
./asr_cmd "mixing_cuda search" "$FILTER_SOCK"
./asr_cmd "mixing_cuda set_active 1 1" "$FILTER_SOCK"
./asr_cmd "amixrank search" "$FILTER_SOCK"
./asr_cmd "amixrank set_active 2 0" "$FILTER_SOCK"

# —— 编码器 / 进程（必须 -zmq / $CMD_SOCK）——
./asr_cmd "h264_nvenc bitrate 4000000" "$CMD_SOCK"
./asr_cmd "enc:0:v:0 b_scale_ratio 1.4" "$CMD_SOCK"
./asr_cmd "ffmpeg loglevel warning" "$CMD_SOCK"
./asr_cmd "ffmpeg quit" "$CMD_SOCK"
```



### 3.7 可选：进程内 HTTP 预览（不替代 RTMP）

主输出仍是 §3.6 的 `flv`/`rtmp`。加 `-http_server` 即可拉 `/{app}/{stream}.flv`（无 bind 时默认 `/live/live.flv`）。自定义流名用 `-http_live_bind live/stream_out:0`。需要浏览器 HLS 时再加第二路 `-f hls`。HTTPS/鉴权见 [http_server.md](./http_server.md) §1.4 / §2 / §3。

```bash
./ffmpeg -task_id 123456 \
  -zmq ipc:///data/LCMS/sock/123456_cmd.sock \
  -http_server http://0.0.0.0:8080 \
  -http_root /data/LCMS/hls/123456 \
  ... 同上 filter_complex / map / nvenc / aac ... \
  -f flv rtmp://test-push.live.qiyi.domain/live/stream_out \
  -map '[vout]' -map '[aout]' \
  -c:v h264_nvenc -preset p4 -b:v 2500k \
  -c:a aac \
  -f hls -hls_time 2 -hls_list_size 8 \
    -hls_flags delete_segments+independent_segments \
    -hls_segment_type fmp4 \
    /data/LCMS/hls/123456/index.m3u8
```

拉流：`http://host:8080/live/live.flv`（主路 GOP；`?lowdelay=1` 等下一 I）、`http://host:8080/index.m3u8`（HLS，不受 `-http_live` 影响）。多码率用 `-http_live_bind live/name:N`。



## 4. 布局设计



### 4.1 layout_mode 与 layout_file / 落盘


| `layout_mode`        | 几何来源                                                                    | `layout_file` / zmq `layout` |
| -------------------- | ----------------------------------------------------------------------- | ---------------------------- |
| `fixed`              | **严格** JSON：格绑 `input` 槽；无流 fill/hold，不挤格                               | **加载并落盘**                    |
| `adapt` / `adaptive` | JSON 模板：活路>格数**丢弃多余**；活路<格数**收缩**为 N 个最大格；无 JSON 时 equal（激励开则可回退内置主讲几何） | **加载并落盘**                    |
| `speaker`（废弃）        | 映射为 `adapt` + WARNING                                                   | 同 adapt                      |


**落盘**：解析成功 → `/data/LCMS/layouts/<task_id>.layout`（`task_id=` 或 `FFMPEG_TASK_ID`）。下次启动无可用 layout 时自动恢复。

`speaker_switch=1`：主讲放到面积最大（并列则更靠前）的宫格；`fixed` 下不改 `input` 绑定，只高亮。`0`：仅手动 `active_speaker`。

### 4.2 fixed：Layout JSON

```json
{
  "canvas": { "w": 1920, "h": 1080 },
  "bg": "0x101010",
  "on_disconnect": "fill",
  "videos": [
    { "input": 1, "x": 0,  "y": 0,  "w": 50, "h": 50, "z": 0, "visible": 1, "border_px": 2, "radius_px": 8, "border_color": "#5b8def" },
    { "input": 2, "x": 50, "y": 0,  "w": 50, "h": 50, "z": 1, "visible": 1, "border_px": 2, "radius_px": 8, "border_color": "#5b8def" },
    { "input": 3, "x": 0,  "y": 50, "w": 50, "h": 50, "z": 2, "visible": 1 },
    { "input": 4, "x": 50, "y": 50, "w": 50, "h": 50, "z": 3, "visible": 1 }
  ]
}
```


| 字段                     | 含义                                 |
| ---------------------- | ---------------------------------- |
| `x/y/w/h`              | 相对画幅 **0–100%**；像素换算后按偶数对齐（`& ~1`） |
| `z`                    | 叠放顺序（大者在上）                         |
| `visible`              | 0=不画                               |
| `on_disconnect`        | `fill` 断流露底；`collapse` 宫格回退        |
| `input`                | **filter 输入下标**，不是业务路 0..N-1       |
| `border_px`            | 本路边框宽（缺省用 `tile_border_px`）        |
| `radius_px` / `radius` | 本路圆角（缺省用 `radius_px` 滤镜选项）         |
| `border_color`         | 本路边框色（缺省用 `tile_border_color`）     |


图为 `[bg][v0][v1][v2][v3]` 且 `bg_input=0` 时，宫格必须写 `1,2,3,4`。误写 `0,1,2,3` 会把 bg 占进第一格；滤镜会告警并把 `input>=bg_input` 整体 +1 纠正。

无 JSON / JSON 非法时：跳过 `bg_input` 的默认 2×2 业务宫格，日志：

`mixing_cuda: layout invalid (…) — using default N-slot business grid`

### 4.3 speaker：内置几何

存活业务路中，响度最高（或 `active_speaker`）占主窗。主讲挂掉会**立即**提升下一位，不会留下空槽。

**1080p 基准（其他分辨率按 1920×1080 比例缩放）：**


| 区域  | 尺寸           | 说明                |
| --- | ------------ | ----------------- |
| 主画面 | **1432×806** | 水平/垂直居中于画布        |
| 副画面 | **468×256**  | 固定比例，不压扁；侧栏最多 3 路 |
| 间距  | **16px**     | 主↔副横向、副↔副纵向       |


```
speaker_layout=obs（n≤4）            speaker_layout=obs(n>4) / grid
┌──────────────────┬────┐           ┌────────────────────────┐
│                  │ T1 │           │                        │
│   主 1432×806    ├16px┤           │        主 1432×806      │
│                  │ T2 │           │                        │
│                  ├────┤           ├────┬────┬────┬─────────┤
│                  │ T3 │           │ T1 │ T2 │ T3 │   …     │
└──────────────────┴────┘           └────┴────┴────┴─────────┘
  侧栏最多 3 路 468×256               底部横栏（保 468:256）+ 缝 16
```

- 仅 1 路存活 → 居中 1432×806
- **obs**：副屏 ≤3（总人数 ≤4）右侧竖栏；更多自动切底部横栏
- **grid**：始终底部横栏
- 侧栏：副屏固定 468×256，顶与主画面齐；3 路时底亦基本对齐（800 vs 806）
- 底栏：路数多时等比缩小，不拉坏宽高比
- 格缝 / 外圈露底图（`bg` / `fill`）
- `border_px` / `border_color`：主讲**高亮**描边（0=关）；每路描边/圆角见 `tile_border_`* / `radius_px` 与 layout 字段
- 实现：`vf_mixing_cuda_layout.c` → `ff_mixing_layout_speaker`



### 4.4 adaptive：活跃宫格

按当前存活业务路数（1–32）自动选 **equal 宫格**：行列使 `cols/rows` 最接近 16:9，外圈 PAD=8、格间 GAP=8；失活/断流不占格（`fill`）。实现：`ff_mixing_layout_equal`。

---



### 4.5 格子切换特效（fade / slide / fly）

格子**重新出现有效画面**时播放全局切换特效（设计摘要：[2026-08-04-tile-transition-effects-design.md](superpowers/specs/2026-08-04-tile-transition-effects-design.md)）。

**触发**


| 场景                                 | 是否播放         |
| ---------------------------------- | ------------ |
| `update` 热切换到新 URL                 | 是（首帧新画面）     |
| 冷插入 / 断流重连恢复                       | 是            |
| 同 URL `unchanged`（跳过 open）         | 否            |
| `trans_effect=none` 或 `trans_ms=0` | 否（立刻切，与现网一致） |


**叠法**：底层 = 该格旧 `hold`（无 hold 则露底色）→ 上层按进度画新画面。

**参数（全局，非单次 update）**


| 选项             | 默认            | 取值                                                                                                                            |
| -------------- | ------------- | ----------------------------------------------------------------------------------------------------------------------------- |
| `trans_effect` | `slide_right` | `none` / `fade` / `slide_left` / `slide_right` / `slide_up` / `slide_down` / `fly_left` / `fly_right` / `fly_up` / `fly_down` |
| `trans_ms`     | `2000`        | `0`–`5000`（毫秒）                                                                                                                |


- **fade**：新旧交叉淡入
- **slide_***：新画面从格外滑入（无淡入）
- **fly_***：滑入 + 淡入

滤镜参数示例：

```text
mixing_cuda=...:trans_effect=slide_left:trans_ms=800
```

运行时（图内 `zmq`）：

```bash
./asr_cmd "mixing_cuda trans_effect fly_left" "$SOCK"
./asr_cmd "mixing_cuda trans_ms 1000" "$SOCK"
./asr_cmd "mixing_cuda trans_effect none" "$SOCK"   # 关闭
```

成功切换时应见日志：

```text
dynamic_input: content_gen=N on vX
mixing_cuda: tile transition start input=... gen=N effect=... ms=... under=0|1
```

**注意**：改 `.cu` 后须 `FFMPEG_ONLY=1 ./build.sh` 重新生成 PTX，否则新 kernel 加载失败。

---



## 5. 音频设计（amixrank 延迟策略）



### 5.1 模型

```text
anullsrc ──► input0（时钟，weight=0，不进混音）
a0..aN   ──► input1..（业务；空 FIFO = 静音，不堵编码）
```

音画同步分两层：


| 层级      | 机制                              | 作用                         |
| ------- | ------------------------------- | -------------------------- |
| **节目轴** | `color_cuda` + `anullsrc` 同墙钟速率 | 最终 `[vout]`/`[aout]` 节拍对齐  |
| **业务轴** | 同路 A/V **共享 PTS 时间线** + 有序小队列   | 输入正常时按源帧序放出，音画对齐；一侧迟到则等/削齐 |


**音频先到数秒、视频后到（或相反）**：源时间戳仍同步时，不能按各自 `wall_arrival` 放行。做法：

1. 先到的一侧**先缓冲**，最多等 `av_wait_ms`（默认 **6000**）
2. 对端到达后按 **已修正的 media PTS** 取公共起点 `start = max(a_pts, v_pts)`，丢掉更早的音频
3. 再按 `sync_wall + (pts - start) + playout_delay` 放出
  → 日志：`AV primed ... start_pts=... drained_audio=N`

视频可先出画（等音频）；**有视频轨时音频在 primed 前不放**。

**PTS 坏点 / 整体跳 10s+**：与 FFmpeg `dts_delta_threshold`（默认 10s）同类——在写入播放时间线前对每路 URL 做共享 `ts_offset` 修正，再进 mixing/amixrank 的放行逻辑。日志：

`timestamp discontinuity ... delta=...s, new offset=...s (pts_jump_s=10.0)`

写出到下游的 PTS 仍是节目轴（`media_frame_idx` / 采样计数），不受源跳跃影响。


| 参数                 | 默认        | 在哪            | 作用                  |
| ------------------ | --------- | ------------- | ------------------- |
| `av_wait_ms`       | **6000**  | dynamic_input | 等对端 A/V 的最长时间       |
| `playout_delay_ms` | **40**    | dynamic_input | PTS 对齐后的小抖动垫        |
| `pts_jump_s`       | **10**    | dynamic_input | 超过则修正 PTS 跳跃（`0`=关） |
| `buffer_ms`        | **200**   | amixrank      | 唯一音频缓存；>2× 削回保嘴型    |
| `startup_grace_ms` | **10000** | amixrank      | 启动宽限（不报 OVERFLOW）   |


```text
dynamic_input=...:playout_delay_ms=40:av_wait_ms=6000:pts_jump_s=10
amixrank=...:buffer_ms=200
```

**启动** `speed<1`：实时模式下编码爬到 1x 属正常，只要输出 `fps` 够即可。此阶段 RTMP 仍按实时灌 FIFO，旧逻辑用过小硬 trim 会单独扔掉音频而视频也在晚，造成唇偏。现在宽限期内允许更大 backlog（A/V 一起晚）；唇同步仍靠 `dynamic_input` 的 PTS/`playout_delay`。

**稳态**：`buffer_ms≈200` 吸收抖动；>2× 削回 buffer。若持续 `OVERFLOW`/`AUDIO_DROP`，说明入出失衡——加大 buffer 只会推迟问题。

**同步主从（2026-08-06）**：

- 节目钟：`color_cuda` + `anullsrc`
- 单路：`dynamic_input` **音频主导**，画面跟本路音频 PTS（可丢旧画/持帧，尽量不丢音频）
- `|A−V|≤80ms`：**不打印**；仅异常实时 `dyn_sync` / `mix_sync` / `amix_sync`（含 `cause:` / `fix:`）
- 详 [av-balance spec](./superpowers/specs/2026-08-06-av-balance-sync-report-design.md)



### 5.2 FFmpeg 默认音画同步（对照）

常见转码：PTS 对齐 + demux 层 `dts_delta_threshold` 纠跳跃 + 可选 `aresample=async`。多路分滤镜混叠时，`dynamic_input` 内做 per-URL PTS 修正与 A/V prime，节目轴仍靠 `color_cuda` + `anullsrc`。

### 5.3 ducking

- `rank[0]` 满增益；其余 × `duck_gain_db`（默认 −18 dB ≈ 幅度 12.6%，**不会静音**）
- `normalize=1`：增益和 >1 时整体归一防削顶；会议场景常用 `normalize=0` + 手动 weights



### 5.4 响度 JSON 契约

amixrank PUB / mixing SUB：

```json
{"rank":[2,0,1],"levels":[-40.0,-35.0,-20.0]}
```

- `rank`：响度降序的 **input 下标**
- `levels[i]`：**input i** 的 dBFS（按输入下标，不是 rank 序）

---



## 6. 选项速查



### mixing_cuda


| 选项                       | 默认          | 说明                                                        |
| ------------------------ | ----------- | --------------------------------------------------------- |
| `inputs`                 | 8           | 最大 32（含 bg）                                               |
| `s`                      | 1920x1080   | 输出画幅                                                      |
| `format`                 | nv12        | CUDA `sw_format`：**nv12** 或 **p010**；所有输入须相同              |
| `layout_mode`            | fixed       | `fixed` / `adapt`（`adaptive`）；`speaker` 废弃→adapt（**可热改**） |
| `layout_file` / `layout` | —           | **fixed 与 adapt 均可用**；成功后落盘 task layout                   |
| `speaker_layout`         | obs         | `obs` / `grid`（adapt 无 JSON 且激励开时的内置回退）                   |
| `rank_endpoint`          | —           | zmq SUB                                                   |
| `speaker_hold_ms`        | 500         | 主讲切换保持                                                    |
| `join_stable_ms`         | 300         | adaptive/speaker 冷插入布局防抖                                  |
| `speaker_switch`         | **1**       | `1`=激励开：主讲进最大/最前格；`0`=仅手动 `active_speaker`                |
| `speaker_switch_db`      | 6           | 响度差阈值（仅 `speaker_switch=1`）                               |
| `bg_input`               | 0           | 背景板 **且 FrameSync 时钟**                                    |
| `primary_input`          | 0           | 仅 `bg_input=-1` 时作时钟；勿指业务路                                |
| `stall_ms`               | 800         | 非持帧时无新帧超时（硬断流）                                            |
| `hold_stall_ms`          | 3000        | 持帧/同 PTS 重复时的超时（防侧窗误黑）                                    |
| `border_px`              | 4           | **主讲高亮**边框宽（0=关；可叠在本路边框上加厚并改色）                            |
| `border_color`           | 0x00FF88    | 主讲高亮边框色                                                   |
| `tile_border_px`         | 0           | 默认每路边框宽（0=关；layout/`slot_style` 可覆盖）                      |
| `tile_border_color`      | 0x5B8DEF    | 默认每路边框色                                                   |
| `radius_px`              | 0           | 默认每路圆角（0=直角；layout/`slot_style` 可覆盖）                      |
| `trans_effect`           | slide_right | 格子换源特效，见 §4.5（`none`/`fade`/`slide_*`/`fly_*`）            |
| `trans_ms`               | 2000        | 特效时长 ms（0–5000；0=关）                                       |
| `sync_warn_interval_ms`  | 10000       | `mix_sync` 告警节流                                           |
| `fill`                   | 暗灰          | 清屏底色                                                      |




### dynamic_input


| 选项                      | 默认             | 说明                        |
| ----------------------- | -------------- | ------------------------- |
| `outputs`               | 8              | 每类媒体 pad 数（最大 32）         |
| `enable_audio`          | 1              | 追加同源 `a0..aN`             |
| `sample_rate`           | 48000          | 音频输出采样率                   |
| `initial_urls`          | —              | 逗号分隔初始 URL（填入插槽 1..N）     |
| `s` / `r`               | 1920x1080 / 25 | 输出尺寸与**输出节拍**（不自动跟输入 fps） |
| `live`                  | 1              | 视频墙钟节流；`0`=尽快             |
| `playout_delay_ms`      | 40             | PTS 对齐后的小持有；`0`=对齐后立刻放    |
| `av_wait_ms`            | 6000           | 一侧先到时等对端的最长时间             |
| `pts_jump_s`            | 10             | PTS 跳跃修正阈值（秒）；`0`=关闭      |
| `reconnect_ms`          | 2000           | 定时重连间隔                    |
| `sync_warn_av_ms`       | 80             | `                         |
| `sync_warn_interval_ms` | 10000          | `dyn_sync` 节流             |




### amixrank


| 选项                      | 默认    | 说明                       |
| ----------------------- | ----- | ------------------------ |
| `inputs`                | —     | 输入路数                     |
| `weights`               | —     | 推荐 `0                    |
| `rank_endpoint`         | —     | zmq PUB bind             |
| `rank_interval_ms`      | 100   | 发布间隔                     |
| `ducking`               | 1     | 主讲优先衰减其它路                |
| `duck_gain_db`          | -18   | 非主讲衰减                    |
| `duck_attack_ms`        | 80    | 增益上升软过渡                  |
| `duck_release_ms`       | 250   | 衰减/重入软过渡                 |
| `duck_hold_ms`          | 400   | 主讲切换保持，抑音量泵              |
| `join_fade_ms`          | 500   | 冷插入/重入渐入（期间不 trim/servo） |
| `buffer_ms`             | 200   | 唯一缓存；>2× 削回 + OVERFLOW   |
| `startup_grace_ms`      | 10000 | 启动宽限                     |
| `sync_warn_interval_ms` | 10000 | `amix_sync` 节流           |
| `normalize`             | 1     | 增益归一                     |


---



## 7. 运行时命令（统一图内 zmq）

合屏/动态源等滤镜命令仍走图内 `zmq`/`azmq`，用法不变。进程级 `ffmpeg -zmq` 是额外的一条 REP（编码器码率等），见 [zmq.md](./zmq.md)，互不替换。

所有可写命令走 **一条** 官方 `zmq`（视频链）REP，消息格式：

```text
<目标滤镜> <命令> [参数…]
```

`f_zmq` 已支持「命令后整行作参数」，因此 `dynamic_input update 1 rtmp://…` 无需再额外加引号。


| 通道 | Socket | 协议 | 对象 |
|------|--------|------|------|
| **滤镜命令** | `ipc:///data/LCMS/sock/<task>_filter.sock` | REP：`目标 命令 参数` | `dynamic_input` / `mixing_cuda` / `amixrank` |
| **进程命令** | `ipc:///data/LCMS/sock/<task>_cmd.sock`（`-zmq`） | 同上 | NVENC / fftools；也可转发滤镜命令，见 [zmq.md](./zmq.md) |
| **激励** | `gain_<task_id\|pid>.sock` | PUB JSON（只读） | `amixrank` → `mixing_cuda` |


每条命令：`zmq` 与目标滤镜均以 **WARNING** 实时打印；解析失败/不支持/参数错以 **ERROR** 详报（含 `cmd`/`args`/`av_err2str`）。解析失败也会回 REP，避免客户端挂死。

图内挂载（可选；挂在 `color_cuda` 后即可覆盖整图）：

```text
color_cuda=...:r=25,zmq=b=ipc\\\:///data/LCMS/sock/123456_filter.sock[bg];
```

进程级（**不依赖**上图 sock；滤镜命令也可打到这条）：

```text
-zmq ipc:///data/LCMS/sock/123456_cmd.sock
```



### 7.1 插槽编号


| 编号       | 含义                                                        |
| -------- | --------------------------------------------------------- |
| **0**    | 底色 / `color_cuda`（`bg_input`）— **不参与** `dynamic_input` 命令 |
| **1..N** | `dynamic_input` 第 1..N 个业务插槽（pad `v0`/`a0` …）             |


**三滤镜固定对应（推荐建图** `[bg][v0..]mixing` **+** `[asil][a0..]amixrank`**）**：


| 业务槽 K  | `dynamic_input`                       | `mixing_cuda`                   | `amixrank`                            |
| ------ | ------------------------------------- | ------------------------------- | ------------------------------------- |
| K=1..N | `srcs[K-1]` → pad `v{K-1}` / `a{K-1}` | filter input **K**（input0=`bg`） | filter input **K**（input0=`anullsrc`） |


要点：

1. **Pad 数启动预留、运行时不增减、不重编号。** `remove` / `purge` 只是软拆除（槽变空、线程闲置），**不会**把后面的槽往前挪。
2. 再上人必须 `update` **到原槽号**（或明确指定的空槽），**禁止** purge 后按存活顺序重新 `update 1、2、3…` 挤号——那会把「流身份」换槽，看起来像「mixing/amixrank 对不上」。
3. `layout_mode=adaptive` / `speaker` / `on_disconnect=collapse` 会按**存活路**重排画面，**屏上第 k 格 ≠ 业务槽 k**；需要位置稳定请用 `fixed` + `on_disconnect=fill`。
4. 三滤镜 `set_active` **互不联动**：`dynamic_input remove` 不会自动关 mixing/amixrank 同槽；控制面应对同槽发齐，或用 `search` 三路对账。

对账示例：

```bash
./asr_cmd "dynamic_input search" "$SOCK"   # 槽:url:状态:active
./asr_cmd "mixing_cuda search" "$SOCK"     # 槽:active:holding:have_frame
./asr_cmd "amixrank search" "$SOCK"        # 槽:enabled:weight
```



### 7.2 dynamic_input


| 命令                 | 参数            | 行为                             |
| ------------------ | ------------- | ------------------------------ |
| `search`           | 〔空〕/`2`/`1-3` | 查插槽；无参=全部。回复 `槽:url:状态:active` |
| `update`           | `<槽           | 区间> [url…]`                    |
| `remove`           | `<槽           | 区间> [url]`                     |
| `purge` / `purage` | （无）           | 软拆除全部业务插槽                      |
| `set_active`       | `<槽           | 区间> <0                         |
| `live`             | `0            | 1`                             |


> **支持的业务路径：**`remove` **→ 空等任意时长 →** `update` **再插入。**  
> `remove` 为软拆除（保 demux 线程空闲，等同断流）；稍后 `update` 复用该线程冷插入（先打开成功再挂上），音视频渐入。勿再硬杀进程式拆槽。

```bash
TASK=123456
SOCK="ipc:///data/LCMS/sock/${TASK}_filter.sock"
URL="rtmp://test-push.live.qiyi.domain/live/ls_src_zp_h264_1080p"

./asr_cmd "dynamic_input search" "$SOCK"
./asr_cmd "dynamic_input search 2" "$SOCK"
./asr_cmd "dynamic_input update 1 $URL" "$SOCK"
./asr_cmd "dynamic_input update 1-4 $URL" "$SOCK"                          # 四槽同一 URL
./asr_cmd "dynamic_input update 1-4 $URL1 $URL2 $URL3 $URL4" "$SOCK"      # 一一对应
./asr_cmd "dynamic_input update 1-4 $URL1 $URL2" "$SOCK"                  # → u1,u2,u2,u2
./asr_cmd "dynamic_input remove 3 $URL" "$SOCK"
./asr_cmd "dynamic_input remove 1-3" "$SOCK"
./asr_cmd "dynamic_input purage" "$SOCK"
./asr_cmd "dynamic_input set_active 2 0" "$SOCK"
./asr_cmd "dynamic_input set_active 1-4 0" "$SOCK"
```

```bash
python3 scripts/mix_zmq_cmd.py --url "$SOCK" --raw "dynamic_input search"
python3 scripts/mix_zmq_cmd.py --url "$SOCK" --raw "dynamic_input update 1 $URL"
```



### 7.3 mixing_cuda


| 命令               | 参数                                             | 作用                                          |
| ---------------- | ---------------------------------------------- | ------------------------------------------- |
| `search`         | 〔空〕/`1`/`1-4`                                  | 查业务槽；回复见下                                   |
| `layout_mode`    | `fixed`/`adapt`/`adaptive`                     | **热切换**；`speaker` 废弃→adapt；会重载已缓存/文件 layout |
| `layout`         | JSON 字符串                                       | **fixed/adapt 均可**；成功后落盘 task layout        |
| `slot_style`     | `<pad> border_px=N radius_px=N border_color=C` | 改**单路**边框/圆角/颜色（pad=业务输入下标，非 bg）            |
| `reload`         | 无                                              | 重读 `layout_file` 或 task 落盘路径                |
| `set_active`     | `<槽                                            | 区间> <0                                      |
| `active_speaker` | `<pad>`                                        | 手动主讲 pad；`speaker_switch=0` 时专用             |
| `speaker_switch` | `0`/`1`                                        | `1`=激励+主讲进最大格；`0`=仅 `active_speaker`        |
| `speaker_layout` | `obs` / `grid`                                 | 主讲样式                                        |
| `primary`        | `<pad>`                                        | FrameSync 时钟（勿指业务 RTMP）                     |
| `trans_effect`   | 见 §6                                           | 运行时改切换特效                                    |
| `trans_ms`       | `0`–`5000`                                     | 运行时改特效时长                                    |


`search` 回复示例：

```text
mode=speaker speaker=1 switch=1 1:1:0:1 2:1:1:1 3:1:1:1 4:1:0:1 5:1:0:0 …
```

**全局字段**


| 字段         | 含义                                                               |
| ---------- | ---------------------------------------------------------------- |
| `mode=`    | 当前 `layout_mode`：`fixed` / `adaptive` / `speaker`                |
| `speaker=` | 当前主讲 pad 下标                                                      |
| `switch=`  | `speaker_switch`：`1`=跟 amixrank 音频激励切主讲；`0`=仅手动 `active_speaker` |


**每槽**（空格分隔）：`槽:active:holding:have_frame`


| 字段         | 含义                                      |
| ---------- | --------------------------------------- |
| 槽          | 业务插槽 **1..N**                           |
| active     | `set_active`：`1`=开，`0`=关                |
| holding    | `1`=正在持帧（`dyn_hold` / 同 PTS 重复）；`0`=非持帧 |
| have_frame | `1`=有可画帧；`0`=当前无帧（空槽常见）                 |


解读例：`2:1:1:1` = 槽2 使能、正在 hold、有画面；`5:1:0:0` = 槽5 使能、未 hold、无帧；`1:1:0:1` = 槽1 使能、非 hold、有画面。

```bash
./asr_cmd "mixing_cuda search" "$SOCK"
./asr_cmd "mixing_cuda search 1" "$SOCK"
./asr_cmd "mixing_cuda search 1-4" "$SOCK"
./asr_cmd "mixing_cuda layout_mode speaker" "$SOCK"
./asr_cmd "mixing_cuda speaker_switch 0" "$SOCK"          # 关闭音频激励联动
./asr_cmd "mixing_cuda active_speaker 2" "$SOCK"          # 仅 speaker 模式
./asr_cmd "mixing_cuda speaker_switch 1" "$SOCK"          # 恢复跟 amixrank
./asr_cmd "mixing_cuda layout_mode fixed" "$SOCK"
./asr_cmd "mixing_cuda set_active 2 0" "$SOCK"
./asr_cmd "mixing_cuda set_active 1-4 0" "$SOCK"
./asr_cmd "mixing_cuda speaker_layout obs" "$SOCK"
./asr_cmd "mixing_cuda reload" "$SOCK"
./asr_cmd "mixing_cuda trans_effect slide_left" "$SOCK"
./asr_cmd "mixing_cuda trans_ms 800" "$SOCK"
```



#### 4 画面 `layout` 热改示例（`layout_mode=fixed`）

前提：图为 `[bg][v0][v1][v2][v3]`，`bg_input=0`，JSON 里业务格写 `input:1..4`。  
`f_zmq` 把 `layout` 后整行当 JSON，可用单引号包住整条命令。

```bash
TASK=123456
SOCK="ipc:///data/LCMS/sock/${TASK}_filter.sock"
```

**例 1 — 均等 2×2**

```bash
./asr_cmd 'mixing_cuda layout {"canvas":{"w":1920,"h":1080},"videos":[{"input":1,"x":0,"y":0,"w":50,"h":50,"z":0,"visible":1},{"input":2,"x":50,"y":0,"w":50,"h":50,"z":1,"visible":1},{"input":3,"x":0,"y":50,"w":50,"h":50,"z":2,"visible":1},{"input":4,"x":50,"y":50,"w":50,"h":50,"z":3,"visible":1}]}' "$SOCK"
```

```text
┌─────┬─────┐
│  1  │  2  │
├─────┼─────┤
│  3  │  4  │
└─────┴─────┘
```

**例 2 — 左主讲 ~75%(<=4路) + 右三路竖条**

```bash
./asr_cmd 'mixing_cuda layout {"canvas":{"w":1920,"h":1080}," videos":[{"input":1,"x":0,"y":0,"w":70,"h":100,"z":0,"visible":1},{"input":2,"x":70,"y":0,"w":30,"h":33.33,"z":1,"visible":1},{"input":3,"x":70,"y":33.33,"w":30,"h":33.33,"z":2,"visible":1},{"input":4,"x":70,"y":66.66,"w":30,"h":33.34,"z":3,"visible":1}]}' "$SOCK"
```

```text
┌──────────┬────┐
│          │ 2  │
│    1     ├────┤
│          │ 3  │
│          ├────┤
│          │ 4  │
└──────────┴────┘
```

**例 3 — 上排两大窗 + 下排两小窗**

```bash
./asr_cmd 'mixing_cuda layout {"canvas":{"w":1920,"h":1080},"videos":[{"input":1,"x":0,"y":0,"w":50,"h":66,"z":0,"visible":1},{"input":2,"x":50,"y":0,"w":50,"h":66,"z":1,"visible":1},{"input":3,"x":0,"y":66,"w":50,"h":34,"z":2,"visible":1},{"input":4,"x":50,"y":66,"w":50,"h":34,"z":3,"visible":1}]}' "$SOCK"
```

```text
┌───────┬───────┐
│   1   │   2   │  ← 高 66%
├───────┼───────┤
│   3   │   4   │  ← 高 34%
└───────┴───────┘
```

也可用文件：改 `layout_file` 后 `./asr_cmd "mixing_cuda reload" "$SOCK"`（须启动时配了 `layout_file=`）。

#### 单路边框 / 圆角（`slot_style`）

```bash
# 默认每路边框（启动或热改 AVOption）
./asr_cmd "mixing_cuda tile_border_px 2" "$SOCK"
./asr_cmd "mixing_cuda radius_px 8" "$SOCK"
./asr_cmd "mixing_cuda tile_border_color 0x5B8DEF" "$SOCK"

# 只改 pad=1（[bg][v0]... 时 v0）
./asr_cmd "mixing_cuda slot_style 1 border_px=3 radius_px=12 border_color=#FF6644" "$SOCK"
```

导播台「导出 layout JSON」会写入每路 `border_px` / `radius_px` / `border_color`，且宽高按输出分辨率**偶数就近**对齐。

### 7.4 amixrank

激励自动 PUB 到 `gain_N.sock`。选项与使能经同一 `zmq`。业务槽 **1..N**（与 `dynamic_input` 同号；`input0=anullsrc` 不可关）。**仅音频**，不联动视频 `set_active`。


| 命令                        | 参数            | 行为                                |
| ------------------------- | ------------- | --------------------------------- |
| `set_active`              | `<槽           | 区间> <0                            |
| `search`                  | 〔空〕/`1`/`1-4` | 见下；详 [amixrank.md](./amixrank.md) |
| `buffer_ms` / `weights` 等 | 见 §6          | 运行时改选项                            |


`search` 回复：`槽:enabled:weight`（空格分隔）。


| 字段      | 含义                                    |
| ------- | ------------------------------------- |
| 槽       | 业务插槽 **1..N**                         |
| enabled | `1`=进混，`0`=`set_active` 已关            |
| weight  | 配置权重（关路不改 weights；`1:0:1` 末尾 `1` 即权重） |


```bash
./asr_cmd "amixrank set_active 2 0" "$SOCK"
./asr_cmd "amixrank set_active 1-4 0" "$SOCK"
./asr_cmd "amixrank set_active 1-4 1" "$SOCK"
./asr_cmd "amixrank search" "$SOCK"
./asr_cmd "amixrank search 1" "$SOCK"
./asr_cmd "amixrank search 1-4" "$SOCK"
./asr_cmd "amixrank buffer_ms 200" "$SOCK"
```



### 7.5 脚本 / 导播台

`scripts/mix_zmq_cmd.py`（需 `pip install pyzmq`），请显式 `--url` 指向图内 `zmq` sock（`*_filter.sock`）。编码器码率用 `--url` 指向 `-zmq` sock（`*_cmd.sock`）。

控制面槽操作推荐用 `[scripts/mix_slot_cmd.py](../scripts/mix_slot_cmd.py)`（`update`/`remove`/`purge` 自动三联发 `set_active`）。

浏览器导播台原型：[mix_console.html](./mix_console.html)；产品侧已接入 Godot Admin「内容 → 导播管理」（/mixctrl）：

- **预览成本**：预览区各框**直拉源 URL**（浏览器可播的 http(s)/mp4/图片直接播；`rtmp`/`srt` 需预览代理或转 HLS）；**不合流**。推流/上屏后由服务端按布局做唯一 `mixing_cuda` 合流，避免双混流。
- **控制**：字幕/台标/布局只改预览；【手动转场】后上屏。混音混屏按**当前场景各路输入** V/A 分控（无单独「主输出」列）；**语音激励**按钮 ↔ `mixing_cuda speaker_switch`（开=主讲进最大格；关=不跟激励）。默认槽边框色高级灰 `#8b93a3`。预览槽位**未开变换也可点选**（仅展示左侧机位属性；拖/缩放需开「预览区变换」）。
- **转场**：Godot 工作台用 Element Plus；效果+时长与手动转场一栏，**场景轮播 / 定时转场**用 Tab 切换（`YYYY-MM-DD HH:mm`→场景）；自动转场列表空时隐藏；停止推流/录制需确认；混音/转场可左右拖分栏。
- **布局 / 画幅**：12 个快速模板；**横屏 16:9 / 竖屏 9:16** 切换（背景画布、推流分辨率、布局小图同步适配；竖屏下主讲类布局改为上大下三）。各输入占位在布局槽内 **letterbox**：横屏源固定 16:9、竖屏源固定 9:16（不拉伸填槽）。选中源可改输入画幅。推流锁参；暗色监视器底。
- **预览变换**：开启后选中源可拖位置、8 向手柄改大小（**默认锁定纵横比**，Shift 解除）；选中不重绘 `<video>`；四边像素间距标尺（按推流分辨率换算）。



### 7.6 进程 `-zmq`（编码器 / fftools）

须启动带 `-zmq ipc:///data/LCMS/sock/<task>_cmd.sock`。报文格式与图内 zmq 相同。

```bash
CMD_SOCK="ipc:///data/LCMS/sock/123456_cmd.sock"
./asr_cmd "h264_nvenc bitrate 4000000" "$CMD_SOCK"
./asr_cmd "enc:0:v:0 maxrate 6000000" "$CMD_SOCK"
./asr_cmd "enc:0:v:0 bufsize 12000000" "$CMD_SOCK"
./asr_cmd "enc:0:v:0 b_scale_ratio 1.4" "$CMD_SOCK"
./asr_cmd "concat src rtmp://host/live/u1" "$CMD_SOCK"
./asr_cmd "volume volume 0.5" "$CMD_SOCK"
./asr_cmd "ffmpeg loglevel warning" "$CMD_SOCK"
./asr_cmd "ffmpeg quit" "$CMD_SOCK"
```

滤镜增删改查 / enable、分路码率、concat 切源的全量示例见 [zmq.md](./zmq.md)。未实现 `process_command` 的 codec/format/protocol 回 `ENOSYS`。

---



## 8. 行为要点与排障


| 现象                                    | 含义 / 处理                                                                                                                                                                           |
| ------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 无声                                    | 按序查：① `weights=[0 1 1 1 1]` ② `opened … audio=1` / `media recovered … (audio)` ③ `first real audio on inputN`；有 recovered 仍无声 → 见附录 **B.2「live 卡住音频」**；`has no audio stream`=源无音轨 |
| `amix_sync … OVERFLOW` / `AUDIO_DROP` | 超 `buffer_ms`（未丢）/ 紧急丢声；**勿靠加大 buffer 掩盖**；查 `speed`/`fps` 与 `dyn_sync`                                                                                                           |
| `dyn_sync … AV_SKEW` / `DROP_A`       | 源 A/V 偏 >80ms 或音频队列丢；查推流时钟/下游卡住；同跳后仍齐则不应告警                                                                                                                                        |
| `mix_sync … NO_REAL` / `EMPTY_SPIKE`  | 曾 live 后短时内大量 empty（非 hold）；查 dynamic_input 重连/解码。无输入持帧、remove/reconnect hold、长空闲空槽：**不告警**（解码堵看 `dyn_sync`）                                                                      |
| `timestamp discontinuity`             | 源 PTS 跳变已修正（`pts_jump_s`）；偶发正常                                                                                                                                                    |
| `AV primed` / `AV wait timeout`       | 同源 A/V 对齐完成 / 超时单边启动                                                                                                                                                              |
| `audio queue full`（dynamic_input）     | 下游未及时取音频；偶发可忽略，持续则查编码滞后                                                                                                                                                           |
| speaker + layout_file                 | 预期忽略 JSON；改 `layout_mode=fixed` 才用文件                                                                                                                                              |
| 左上角只剩底色                               | JSON `input` 含了 `bg_input`；应写业务下标 1..N                                                                                                                                            |
| 一路卡顿                                  | secondary 不阻塞 primary；只影响该 pad                                                                                                                                                    |
| RTMP 未恢复                              | 视频 `lavfi.dyn_empty` 占位；音频不推占位（amixrank 空 FIFO=静音）；不发 EOF，可重连                                                                                                                     |
| 断流/重连后一路画面冻住                          | 见附录 **B.3**；持帧勿再被当成 live（`lavfi.dyn_hold`）                                                                                                                                        |
| update 换源无淡入/滑入/飞入                    | 见附录 **B.7**；查 `content_gen`/`tile transition start` 日志；同 URL `unchanged` 不播；旧二进制误绑每帧 `cur_seq` 会导致特效永远 t≈0                                                                        |
| `-c copy` 却被 `-abnormal_timeout` 杀掉 | 旧版本按解码帧/循环卡住杀 copy。现对视频 stream copy 跳过僵尸超时和 10s 丢帧退出；转码任务仍生效 |


其它：

- **单流隔离**：一路卡顿只影响该 pad
- **CUevent**：compose 用事件流水线，避免每帧 `cuStreamSynchronize`
- **无图内 zmq**：只要启动了 `-zmq`，滤镜命令仍可打到 `*_cmd.sock`；两条都没有才不能热控流/布局（`sendcmd` 仍可注入 `active_speaker`；amixrank ducking 仍可用）
- `live=1`：只约束**视频**墙钟；音频按下游 `frame_wanted` 独立出帧（见 B.2）

---



## 附录 A：旧方案 concat + amix（兼容）

仍可用 concat `try_self` + `mixing_cuda` fixed + `amix`（无激励自动切主讲）：

```text
-f concat -try_self 1 -concat_cmd ipc:///tmp/concat0.sock -safe 0 -i rtmp://main0
...
color_cuda=c=0x101010:s=1920x1080:r=25[bg];
[bg][0:v][1:v][2:v][3:v] mixing_cuda=inputs=5:layout_mode=fixed:layout_file=/tmp/layout.json:stall_ms=800[vout];
anullsrc=r=48000:cl=stereo[asil];
[asil][0:a][1:a][2:a][3:a] amix=inputs=5:weights='1 1 1 1 0':normalize=0[aout]
```

热切备流：向对应 `concat_cmd` 发 `concat src rtmp://backup`。新会议场景请优先用正文三件套。

---



## 附录 B：问题与方案（踩坑记录）



### B.1 布局


| 问题                                              | 原因                                        | 方案                                                          |
| ----------------------------------------------- | ----------------------------------------- | ----------------------------------------------------------- |
| `layout_mode=speaker:layout_file=...` 不按 JSON 排 | speaker/adaptive **忽略** JSON 几何，只用内置主讲/宫格 | 要用 JSON → `layout_mode=fixed`；否则去掉 `layout_file`（会 WARNING） |
| JSON 非法 / 缺 `videos[]`                          | fixed 无法建槽                                | WARNING + 自动回退默认业务宫格（跳过 `bg_input`）                         |
| 左上角只剩底色 / bg 进第一格                               | JSON `input` 写成了 `0..3`（含背景板）             | 图为 `[bg][v0]…` 时写 `1..4`；滤镜会告警并尝试 +1 纠正                     |
| speaker 主讲挂掉侧栏“消失”                              | 死主讲仍占大窗、缩略图高度按错                           | 已修：主讲挂掉立即提升；solo → 100% 全屏                                  |




### B.2 音频 / 唇同步


| 问题                                            | 原因                                                                                                                  | 方案                                                                                          |
| --------------------------------------------- | ------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------- |
| **完全无声（有画面、有** `media recovered (audio)`**）** | 旧实现把 **音频也卡在视频** `live=1` **墙钟**上：每秒最多 ≈25×1024 采样（≪48kHz），amixrank 大部分时间混空 FIFO → 听感静音；单槽 `queued_audio` 还会丢掉未取走的帧 | **已修**：墙钟只约束视频；音频按 `frame_wanted` 独立出帧 + 多帧 `audio_q`；重编后应见 `first real audio on inputN`    |
| 完全无声（无 `first real audio`）                    | `weights` 空格转义被吃成全 0；或源无音轨；或二进制未含上述修复                                                                               | 用 `weights=0                                                                                |
| amixrank 永远静音（曾推占位）                           | 旧逻辑对断流音频推 `lavfi.dyn_empty` 静音填满 FIFO，真实音频再也进不来                                                                     | **已修**：音频**不推**静音占位；空 FIFO 即静音；amixrank 丢弃 secondary 上残留的 `dyn_empty`                       |
| ducking 以为会静音                                 | `-18dB` 只是变小（约 12.6% 幅度）                                                                                            | 正常；真正静音查 weights / 音轨 / live 节流 / FIFO                                                      |
| `trimmed …` 启动卡一下 / 稳态约每分钟一顿                  | 启动：旧硬 trim 过猛；稳态：FIFO 堆到硬顶再砍 20ms                                                                                   | **已修**：grace + join fade；稳态 `target_latency_ms=100` 伺服；硬顶仍周期出现 → 查入出时钟，勿加大 `max_latency` 掩盖 |
| 软追赶 catch-up                                  | 时间压缩听感怪                                                                                                             | **已去掉**；勿再开                                                                                 |
| 节目轴 vs 业务轴搞混                                  | `color`/`anullsrc` 只保证输出节拍                                                                                          | 同路唇同步靠 `dynamic_input` PTS prime + 修正                                                       |
| 音频先到几秒、视频后到                                   | 墙钟到达不同，源 PTS 仍同步                                                                                                    | `av_wait_ms=6000` 缓冲等待 + `max(a,v)` 削齐；音频 primed 前不放                                        |
| `playout_delay` 很大（200ms）发闷                   | 等长持有会叠端到端延迟                                                                                                         | 默认 **40ms**（只作小垫，不是主同步）                                                                     |
| 偶发坏 PTS / 跳 10s+                              | 放行时间线被带飞（卡住或倾倒）                                                                                                     | `pts_jump_s=10` 共享 offset 修正（类 FFmpeg）；看 `timestamp discontinuity` 日志                       |
| 改完只有底色、没业务画面                                  | 单槽最新帧 + delay 等待互相顶掉 → 永不放行                                                                                         | 已改**有序 video FIFO**，与音频同一 PTS due 规则，保帧序+唇同步                                                |
| 单路声音时有时无                                      | PTS 时间线相对墙钟超前 → 音频空等；amixrank 短欠载就 disarm 再垫缓冲                                                                      | PTS due 超前 >80ms 且已过 arrival+delay 则**重锚**；amixrank 仅长空洞(~500ms) 才 disarm                   |
| 编码 `speed<1` 启动期                              | 实时模式爬到 1x 正常；旧逻辑仍按 100ms 硬 trim                                                                                     | 宽限内允许 backlog（A/V 一起晚）；稳态仍持续 trim / `audio queue full` → 再降码率/负载                            |


无声排查顺序：

```text
weights=[0 1 1 1 1]  →  opened…audio=1 / media recovered(audio)
                      →  first real audio on inputN
                      →  仍无声：确认已重编含「音频解耦视频墙钟」的修复
```



### B.3 视频时钟 / 稳定性


| 问题                | 原因                                                                                                | 方案                                                                                                                              |
| ----------------- | ------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------- |
| 整图卡死              | FrameSync 被业务 RTMP 当 primary                                                                      | `bg_input=0` **+ color_cuda** 做时钟                                                                                               |
| 闪几下停              | `primary_input` 指到业务路                                                                             | 勿指 RTMP pad                                                                                                                     |
| 解码间隙闪黑            | 插了 empty 占位                                                                                       | 持上一帧；仅断流清空后露底                                                                                                                   |
| **断流重连后某一路画面卡死**  | 无新解码时仍发 `cur_hw`，节目 PTS 每 tick 递增 → mixing 一直刷新 `last_arrival`，`stall_ms` 永不生效                    | **已修**：持帧标 `lavfi.dyn_hold`；mixing 照常 blit 但不刷新 arrival；超超时后按断流处理，真帧恢复后自动回格                                                     |
| **侧窗启动即卡 / 中间黑**  | `dyn_hold`+`stall_ms=800` 过严；`color_cuda r=50` 而业务 `r=25` 时 FrameSync 同 PTS 重复被当成断流；video_q 未及时排空 | **已修**：`hold_stall_ms=3000`；同 PTS 当 hold；积压强制放行；**建议** `color_cuda` **与** `dynamic_input` **同 r（如均 25）**；三路同 URL 易抢流，中间黑多为该路未出画 |
| `color_cuda r=50` | 背景时钟 50fps 带动整图 compose                                                                           | 业务 `r=25` 时侧窗易误判 stall；**会议场景建议 r=25**                                                                                          |
| `live=1` 音频也变慢/无声 | 误以为 live 同时节流音视频                                                                                  | **live 只节流视频**；音频独立（见 B.2）                                                                                                      |




### B.4 命令行 / 构建


| 问题                                                       | 原因                                                    | 方案                                                   |
| -------------------------------------------------------- | ----------------------------------------------------- | ---------------------------------------------------- |
| `initial_urls` 协议/`:` 被吃掉、多 URL 合并不对                     | bash 单引号 filtergraph 里，`:` 与 `,` 仍需对 lavfi 转义；反斜杠层数不够 | 推荐：`rtmp\\\://host/path\\\,rtmp\\\://host/path`（见下例） |
| `weights=0 1 1 1 1` 变成全 0                                | shell/转义把空格后内容丢掉                                      | **始终**写 `weights=0                                   |
| 改完源码仍无声/旧行为                                              | 跑的不是刚编的二进制                                            | `FFMPEG_ONLY=1 ./build.sh`，用安装前缀或 `./ffmpeg` 确认路径    |
| 编译：`void value not ignored`（`av_channel_layout_default`） | 本树该 API 返回 `void`，不能 `ret = …`                        | 直接调用、不接返回值（已按此修复）                                    |


`initial_urls` **转义示例**（bash，与第 3 节模板一致）：

```bash
initial_urls=rtmp\\\://test-push.live.qiyi.domain/live/ls_src_zp_h264_1080p\\\,rtmp\\\://test-push.live.qiyi.domain/live/ls_src_zp_h264_1080p
```

少写 `\` 时常见现象：URL 解析失败、只识别一路、或打开报错后一直重连。

### B.5 推荐默认（会议合屏）

```text
dynamic_input=...:playout_delay_ms=40:av_wait_ms=6000:pts_jump_s=10:live=1:enable_audio=1
mixing_cuda=...:layout_mode=speaker:speaker_layout=obs:bg_input=0:stall_ms=800:border_px=2
color_cuda=...:r=25,zmq=b=ipc:///data/LCMS/sock/<task>_filter.sock[bg]
-zmq ipc:///data/LCMS/sock/<task>_cmd.sock
-c:v h264_nvenc ... -b_scale_ratio 1.4
amixrank=...:weights=0|1|1|1|1:ducking=1:duck_gain_db=-18:normalize=0
```

关键日志（正常应陆续出现）：

1. `dynamic_input: ... playout_delay_ms=40 av_wait_ms=6000 pts_jump_s=10.0`
2. `amixrank: ... buffer_ms=200 ... weights=[0 1 1 1 1]`
3. `dynamic_input: opened ... audio=1` / `media recovered ... (audio)`
4. `dynamic_input: AV primed ...`
5. `amixrank: first real audio on inputN`
6. 换源时：`dynamic_input: content_gen=N on vX` → `mixing_cuda: tile transition start ...`

异常时对照本附录与第 8 节排障表。

### B.6 控制面（命令发不通）


| 问题                          | 原因                                                     | 方案                                                                                                     |
| --------------------------- | ------------------------------------------------------ | ------------------------------------------------------------------------------------------------------ |
| 流/布局命令无响应                   | 图里未挂 `zmq` **且**未加 `-zmq`，或 sock 路径不一致                | 见 **§7**；有 `-zmq` 时打 `*_cmd.sock` 即可；`dynamic_input` **已无**专用 `cmd_*.sock` |
| NVENC 码率命令无响应                 | 未加 `-zmq`，或打到了 filter sock                               | 用 `*_cmd.sock`：`h264_nvenc bitrate …`；见 **§7.6**、[zmq.md](./zmq.md) |
| `update` 失败旧流不变             | 新 URL 打开失败                                             | 查源；日志 `update ... aborted, open failed`；成功才挂槽                                                          |
| 批量 `update` 卡一下/花屏          | 在滤镜线程同步 open / 先拆再开                                    | **已修**：旁路并行 open（`queued async open`→`async open done`→`staged for hot-swap`）；同 URL `unchanged`；切换保留末帧 |
| `remove`/`update` 槽号无效      | 用了 0 或超出 1..N                                          | **0=底色不参与**；业务槽从 **1** 起                                                                               |
| 布局 JSON 无效                  | `layout_mode≠fixed` 或 JSON 坏                           | fixed + 合法 JSON；否则 WARNING                                                                             |
| `remove` 后再 `update` 卡顿/音量晃 | 旧实现硬拆线程 + ducking 硬切                                   | **软拆除+冷插入**（保线程）；`join_fade_ms` / `join_stable_ms`；日志应见 `soft-removed` / `cold insert`                 |
| purge/remove 后再上人，音画槽「对不上」  | ①控制面挤到低号槽；②adaptive/speaker 视觉压缩；③三滤镜 `set_active` 未对齐 | **槽号=稳定身份**（见 **§7.1**）：原槽 `update`；固定布局用 `fixed`+`fill`；同槽对齐三路 `search`/`set_active`                  |




### B.7 格子切换特效


| 问题                               | 原因                                                                | 方案                                                                                              |
| -------------------------------- | ----------------------------------------------------------------- | ----------------------------------------------------------------------------------------------- |
| `update` **后看不出 fade/slide/fly** | 旧实现把 `lavfi.dyn_gen` 绑到每帧递增的 `cur_seq` → 过渡每帧被重置到 t≈0（新画面几乎始终在格外） | **已修**：独立 `content_gen`，仅在 update/冷插入/重连后首帧新画面递增；重编后应见 `content_gen=` 与 `tile transition start` |
| 同 URL 切换无特效                      | `unchanged` 跳过 open，不 bump `content_gen`                          | 预期行为；换不同 URL 或先 `remove` 再 `update`                                                             |
| `trans_effect=slide_left` 仍像硬切   | 二进制未含修复，或 `trans_ms=0`/`none`                                     | 确认 `FFMPEG_ONLY=1 ./build.sh`；查启动参数与 zmq 是否改掉                                                   |
| 改 `.cu` 后 filter 起不来 / kernel 缺失 | PTX 未随 `.cu` 重编                                                   | 重新 `FFMPEG_ONLY=1 ./build.sh`                                                                   |
| 想关闭特效                            | —                                                                 | `trans_effect=none` 或 `trans_ms=0`（滤镜参数或 zmq）                                                   |
| 换 264↔265 / 分辨率后黑屏或旧解码器          | 未重新 open                                                          | `update`/重连会重新 probe；日志 `opened ... (codec=h264                                                 |
| 输入 50fps 输出仍 25                  | `r` 是**输出节拍**，不跟输入自动变                                             | 同时改 `dynamic_input` 与 `color_cuda` 的 `r`（会议建议一致）                                                |
