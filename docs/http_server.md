# 进程内 HTTP 拉流（`-http_server`）

HLS / DASH 仍写盘再 GET。HTTP-FLV / HTTP-TS / HTTP-fMP4 走**内存 GOP**：路径为 `/{app}/{stream}.flv|.ts|.mp4`（默认 `/live/live.flv`），从最近闭合 GOP 的 I 帧起播。


| 模式                 | 路径                                 | 行为                                 |
| ------------------ | ---------------------------------- | ---------------------------------- |
| `-http_live 1`（默认） | `/{app}/{stream}.flv` `.ts` `.mp4` | 内存 GOP 直播                          |
| `-http_live 1`     | 其它（`index.m3u8` 等）                 | `http_root` 下文件                    |
| `-http_live 0`     | 任意                                 | 只按 `http_root` **点播下载**完整文件，不走 GOP |



| 输出        | 磁盘                              | 拉流（默认 app/stream = `live`/`live`）     |
| --------- | ------------------------------- | ------------------------------------- |
| HLS       | `http_root` 下 `index.m3u8` + 切片 | `http://<host>:8091/index.m3u8`       |
| DASH      | `http_root` 下 `index.mpd` + 切片  | `http://<host>:8091/index.mpd`        |
| HTTP-FLV  | 不需要                             | `http://<host>:8091/live/ls_cctv.flv` |
| HTTP-TS   | 不需要                             | `http://<host>:8091/live/ls_cctv.ts`  |
| HTTP-fMP4 | 不需要                             | `http://<host>:8091/live/ls_cctv.mp4` |


多路输出用重复的 `-http_live_bind app/stream:file`。HLS/DASH **不受** `-http_live` 影响，始终按 `http_root` 文件 GET。只拉直播时 `-http_root` 可省略。

设计说明：[GOP 起播](./superpowers/specs/2026-09-02-http-live-gop-design.md) · [ZLM 并发架构](./superpowers/specs/2026-09-07-http-server-zlm-arch-design.md)

## 变更记录


| 日期         | 说明                                                                                                              |
| ---------- | --------------------------------------------------------------------------------------------------------------- |
| 2026-09-18 | 从 ffmpeg.4 `262cc35` 回移植：fMP4 `separate_moof+negative_cts_offsets`（不再用 cmaf 单轨）；TS `mpegts_copyts` + AAC priming 音频 PTS 后移；MP4 视频 priming 对齐 AAC delay；IDR 后 flush moof；TS/fMP4 起播一律等 KEY；OpenSSL 1.0.2/1.1 兼容 |
| 2026-09-14 | FFmpeg 8 `-re`：只按最慢那路限速（对齐上游 `de6bcf5`）。8.1 原先对每路分别 `sleep`，ZLM/MP4 音视频交错差时超前的音频会拖住 demux，视频 `speed` 掉到 0.7x 并刷 `Resumed reading … lag`；4/6 无此 catch-up 逻辑所以不明显。仅 `ffmpeg-8.1.2` |
| 2026-09-09 | `-http_server` 内存 GOP + `-stream_loop` 切齐 A/V 终点已迁到 `ffmpeg4.4.1.iqiyi`（`write_packet` 旁路推送；4.4 无 `pkt->time_base`/`ch_layout`/`nal.h`，已改用 `ost->st`/`par->channels`/`ff_avc_parse_nal_units_buf`） |
| 2026-09-08 | `-stream_loop`：切点取音视频 `pts+duration` 墙钟 min，丢掉较长一轨的尾巴；`duration` 用 `AV_TIME_BASE` 累加。容器有 stream duration 则第一圈就切，否则第一圈测量、第二圈起切。 |
| 2026-09-08 | copy（尤其 `-c:v copy -c:a aac`）比整路转码更容易花屏/音画漂：视频走源时间戳、音频走编码器时间戳；GOP 按容器 KEY 切（FLV 的 KEY 不一定是 IDR）；copy 命令常缺 `-f live_flv`（无 `AVFMT_TS_DISCONT`，跳跃 dts 会被丢掉）。8.x 已无 `-async` |
| 2026-09-08 | copy 拉 `.mp4` 报 `Packet duration: -N / dts: …` 和 `pts has no value`：音频包与上一包 `dts+duration` 重叠（AAC priming）。写入前补 pts、把重叠 dts 后移，不再丢包 |
| 2026-09-08 | 连接时报 `nonzero dts` / `dts 14848>=14848`：懒 mux 把当前包写进 GOP 后又写一遍；empty_moov 后首包 dts 不是 0。已改为 enable 后不再重复写、`delay_moov` 并把 ftyp/moov 拆进 header。copy 是 AVCC、转码是 Annex B：按起始码/长度前缀转换；TS 走 `h264_mp4toannexb`（IDR 补 SPS/PPS）。TS 起播从 GOP 缓存出，不再干等下一个 I |
| 2026-09-08 | HTTP-fMP4 改为 CMAF 逐帧 fragment（`frag_every_frame`+负 CTS），起播只从 ring 里最后一个 IDR 盒子；修复 `.mp4` 的 `Invalid NAL unit 0` 与 copy 卡顿（`.flv`/`.ts` 不受影响） |
| 2026-09-08 | 分钟统计改为单行一条（无 `{}`），先换行再打，避免和进度行粘在一起 |
| 2026-09-08 | 每分钟打一路简洁流统计；`GET /api/streams` JSON；`GET|POST /api/kick` 踢单个/批量拉流客户端 |
| 2026-09-08 | 追上 ring（`rpos==wpos`）不再误判 stale，消除连续 `client lagged, skip GOP` |
| 2026-09-07 | I/O 改为每核 EventWorker + `SO_REUSEPORT` + 每连接协程；媒体只传 refcount 指针；明文文件 `sendfile`。`-http_workers` 默认 0=核数 |
| 2026-09-02 | 白名单拒绝回 HTTP 403（队列满回 503），不再静默断 TCP；ffplay 可立刻退出                                                                |
| 2026-09-02 | `http:` / `http-live:` / `http-server:` 操作与异常日志前缀 `([YYYY-MM-DD HH:MM:SS,mmm]`，不依赖 `-task_id`                   |
| 2026-09-02 | HTTP-FLV/MP4：ADTS 剥成裸 AAC（aac_adtstoasc）；修复 `Malformed AAC bitstream`                                           |
| 2026-09-02 | HTTP-TS：裸 AAC 补 AudioSpecificConfig，mpegts 才能打 ADTS；修复 `AAC bitstream not in ADTS format and extradata missing` |
| 2026-09-02 | FLV/MP4 按封装把 Annex B 转成 AVCC（不再自制 extra[0]=1）；TS 仍走 Annex B。修复 Invalid NAL unit size                            |
| 2026-09-02 | 有 GOP 立刻起播（不等 extradata/下一 I）；connect/disconnect 成对日志                                                           |
| 2026-09-02 | `/live.flv` `/live.ts` `/live.mp4` 改为内存 GOP + 每连接 mux，从最近闭合 GOP 的 I 帧起播                                         |
| 2026-09-02 | HTTP-TS / HTTP-fMP4 改为 `-http_server` 写 `live.ts` / `live.mp4` 并跟读                                              |
| 2026-09-02 | 补全 HLS / DASH / HTTP-TS / HTTP-fMP4 完整启动命令（含 SRT 转码示例）                                                          |
| 2026-09-02 | 新增 `-http_server` / `-http_root`                                                                                |




## 1. 输出命令（复制即用）

下面共用同一路 SRT 输入与滤镜，均加 `-http_server http://0.0.0.0:8091`。

先建目录（仅 HLS/DASH 需要）：`mkdir -p /data/LCMS/hls/123456 /data/LCMS/dash/123456`

### 1.1 HLS fMP4（`-http_server`）

同时可拉 `http://<host>:8091/live/live.flv`（无 `-http_live_bind` 时默认 `/live/live` 绑 file 0）。

```bash
./ffmpeg \
  -http_server http://0.0.0.0:8091 \
  -http_root /data/LCMS/hls/123456 \
  -i "srt://36.155.98.77:10016" \
  -filter_complex "[0:v:0]format=pix_fmts=yuv420p,yadif=mode=0,fps=fps=25,split=1[vin0];[vin0]scale=1920:1080[vout0];[0:a:0]azmq=b=ipc\\\:///data/LCMS/sock/27026_filter.sock,volume=1.0,aresample=osr=48000,asplit=1[ain0]" \
  -map '[vout0]' -map '[ain0]' \
  -vcodec h264_nvenc -acodec libfdk_aac \
  -b:v 10000000 -bf 2 -r 25 -g 50.0 -level 0.0 -profile:v high \
  -gpu 0 -spatial-aq 1 -b_scale_ratio 1.4 \
  -ab 128000 -ac 2 \
  -f hls -hls_time 2 -hls_list_size 8 \
    -hls_flags delete_segments+independent_segments \
    -hls_segment_type fmp4 \
    /data/LCMS/hls/123456/index.m3u8
```



### 1.2 HLS MPEG-TS（`-http_server`）

与 1.1 相同，只改切片类型（或不写 `-hls_segment_type`，默认 TS）：

```bash
./ffmpeg \
  -http_server http://0.0.0.0:8091 \
  -http_root /data/LCMS/hls/123456 \
  -i "srt://36.155.98.77:10016" \
  -filter_complex "[0:v:0]format=pix_fmts=yuv420p,yadif=mode=0,fps=fps=25,split=1[vin0];[vin0]scale=1920:1080[vout0];[0:a:0]azmq=b=ipc\\\:///data/LCMS/sock/27026_filter.sock,volume=1.0,aresample=osr=48000,asplit=1[ain0]" \
  -map '[vout0]' -map '[ain0]' \
  -vcodec h264_nvenc -acodec libfdk_aac \
  -b:v 10000000 -bf 2 -r 25 -g 50.0 -level 0.0 -profile:v high \
  -gpu 0 -spatial-aq 1 -b_scale_ratio 1.4 \
  -ab 128000 -ac 2 \
  -f hls -hls_time 2 -hls_list_size 8 \
    -hls_flags delete_segments+independent_segments \
    -hls_segment_type mpegts \
    /data/LCMS/hls/123456/index.m3u8
```



### 1.3 DASH（`-http_server`）

```bash
./ffmpeg \
  -http_server http://0.0.0.0:8091 \
  -http_root /data/LCMS/dash/123456 \
  -i "srt://36.155.98.77:10016" \
  -filter_complex "[0:v:0]format=pix_fmts=yuv420p,yadif=mode=0,fps=fps=25,split=1[vin0];[vin0]scale=1920:1080[vout0];[0:a:0]azmq=b=ipc\\\:///data/LCMS/sock/27026_filter.sock,volume=1.0,aresample=osr=48000,asplit=1[ain0]" \
  -map '[vout0]' -map '[ain0]' \
  -vcodec h264_nvenc -acodec libfdk_aac \
  -b:v 10000000 -bf 2 -r 25 -g 50.0 -level 0.0 -profile:v high \
  -gpu 0 -spatial-aq 1 -b_scale_ratio 1.4 \
  -ab 128000 -ac 2 \
  -f dash -seg_duration 2 -window_size 8 \
    /data/LCMS/dash/123456/index.mpd
```



### 1.4 HTTP-FLV / HTTP-TS / HTTP-fMP4（内存 GOP）

不写磁盘直播文件。默认立刻推**当前最新 1 个 GOP**（building，已从最近 I 起）再跟实时，快起播。`?lowdelay=1` 不推缓存，等下一帧 I 再出。

单路（默认 `/live/live.flv`；自定义流名用 `-http_live_bind`）：

```bash
./ffmpeg \
  -http_server http://0.0.0.0:8091 \
  -http_live_bind live/ls_cctv:0 \
  -i "srt://36.155.98.77:10016" \
  -filter_complex "[0:v:0]format=pix_fmts=yuv420p,yadif=mode=0,fps=fps=25,split=1[vin0];[vin0]scale=1920:1080[vout0];[0:a:0]azmq=b=ipc\\\:///data/LCMS/sock/27026_filter.sock,volume=1.0,aresample=osr=48000,asplit=1[ain0]" \
  -map '[vout0]' -map '[ain0]' \
  -vcodec h264_nvenc -acodec libfdk_aac \
  -b:v 10000000 -bf 2 -r 25 -g 50.0 -level 0.0 -profile:v high \
  -gpu 0 -spatial-aq 1 -b_scale_ratio 1.4 \
  -ab 128000 -ac 2 \
  -f null -
```

一进多出（每路输出绑一个流名）：

```bash
./ffmpeg -http_server http://0.0.0.0:8091 \
  -http_live_bind live/ls_cctv_1080p:0 \
  -http_live_bind live/ls_cctv_720p:1 \
  -i "srt://..." \
  -map 0:v -map 0:a -c:v h264_nvenc -s 1920x1080 -g 50 -c:a aac -f flv rtmp://.../ls_cctv_1080p \
  -map 0:v -map 0:a -c:v h264_nvenc -s 1280x720  -g 50 -c:a aac -f flv rtmp://.../ls_cctv_720p
```

拉流：

- 快起播：`http://<host>:8091/live/ls_cctv_1080p.flv`
- 低延迟（等下一 I）：`http://<host>:8091/live/ls_cctv_1080p.flv?lowdelay=1`
- `.ts` / `.mp4` 同样：`/live/ls_cctv_720p.ts`

HTTPS + 鉴权 + 白名单：

```bash
./ffmpeg \
  -http_server https://0.0.0.0:8091 \
  -http_cert /data/LCMS/certs/server.crt \
  -http_key  /data/LCMS/certs/server.key \
  -http_token SECRET \
  -http_allow 10.0.0.0/8,127.0.0.1 \
  -http_live_bind live/ls_cctv:0 \
  ...
```

拉流：`https://<host>:8091/live/ls_cctv.flv?token=SECRET`（或 `Authorization: Bearer SECRET` / HTTP Basic）。

不写 `-http_live_bind` 时默认为 `/live/live.flv` 绑 file 0。

### 1.5 点播（`-http_live 0`）

关掉内存 GOP 后，所有 GET 都是 `http_root` 下的**完整文件下载**（带 `Content-Length`，可 Range），不跟读、不直播。例如已落盘的 `vod.mp4`：

```bash
./ffmpeg -http_server http://0.0.0.0:8091 -http_root /data/LCMS/vod -http_live 0 \
  -i in.mp4 -c copy /data/LCMS/vod/vod.mp4
```

拉流：`http://<host>:8091/vod.mp4`。`index.m3u8` / `index.mpd` / `.m4s` 无论 `-http_live` 开或关都按文件 GET。

## 2. `-http_server` 选项


| 选项                                   | 含义                                                         |
| ------------------------------------ | ---------------------------------------------------------- |
| `-http_server http://0.0.0.0:8091`   | 进程内 bind。`https://` 需 `-http_cert` + `-http_key`           |
| `-http_root DIR`                     | 文档根。省略时用第一个本地 `.m3u8` / `.mpd` 输出目录；只拉直播可不设                |
| `-http_cert PATH` / `-http_key PATH` | HTTPS 证书与私钥（PEM，证书可用 chain）                                |
| `-http_auth user:password`           | HTTP Basic 拉流鉴权                                            |
| `-http_token TOKEN`                  | `?token=` 或 `Authorization: Bearer`                        |
| `-http_allow CIDR`                   | IP 白名单，可重复或逗号分隔；不设则允许所有地址                                  |
| `-http_workers N`                    | EventWorker 数。`0`（默认）= 进程可用核数；每核一个 epoll + 同端口 `SO_REUSEPORT` |
| `-http_live 1`                       | 默认开：已 bind 的 `/{app}/{stream}.flv                          |
| `-http_live_bind app/stream[:N]`     | 可重复。把 URL `/{app}/{stream}` 绑到第 N 个输出文件（省略 N 则按出现顺序 0,1,…） |


行为：

- 直播：`/{app}/{stream}.flv` → `video/x-flv`；`.ts` → `video/mp2t`；`.mp4` → `video/mp4`；无 `Content-Length`
- 默认：立刻推**最新 1 个 GOP**（当前 building，从最近 I 起）再跟实时
- `?lowdelay=1`：不推缓存，等到下一帧视频 I 再出
- HLS/DASH（`.m3u8` `.mpd` `.m4s`）永不走 GOP
- HLS/DASH MIME：`.m3u8` → `application/vnd.apple.mpegurl`；`.mpd` → `application/dash+xml`；`.m4s/.mp4` → `video/mp4`；切片 `.ts` → `video/mp2t`
- 播放列表 `Cache-Control: no-cache`；切片短缓存
- CORS：`Access-Control-Allow-Origin: *`（API 另允许 POST）
- 每分钟一条 INFO：`http-live: streams N, stream app/name, h264, aac, gop Xs, fps N, clients N, …`
- 统计 / 踢流：`GET /api/streams`、`GET|POST /api/kick`（见 §3）；鉴权与拉流相同
- 文件 GET 支持简单 `Range`（206）；直播路径不走 Range
- `GET /` 先试 `index.m3u8`，没有再试 `index.mpd`
- 拒绝路径 `..`；app/stream 不得含 `/`；不提供目录列表
- HTTPS：`-http_server https://0.0.0.0:8091 -http_cert cert.pem -http_key key.pem`（TLS 1.2+）
- 鉴权：未配置则不检查；配置后 GET/HEAD 必须带 token 或 Basic（OPTIONS 不鉴权）
- 白名单：`-http_allow` 按源 IP 过滤；不在名单内先读完请求再回 **HTTP 403 Forbidden**（不再静默关 TCP，否则 ffplay 会一直等响应头）。HTTPS 会先完成 TLS 再 403
- 起播日志：每行前缀 `([2026-09-02 15:34:11,367]`（本地时分秒+毫秒，无 `-task_id` 也会打），随后 `http: [ip:port +Nms] accept/tls/GET/auth/route`，再 `http-live: [ip] gop ready / http 200 / mux header / streaming / first media`

极致 IO 把 `-http_root` 指到 `/dev/shm/...`。音量运行时：`./asr_cmd "volume volume 0.5" ipc:///data/LCMS/sock/27026_filter.sock`（图内 `azmq`）。

## 3. 统计 API 与踢流

鉴权与拉流相同：配了 `-http_token` / `-http_auth` 就必须带。白名单同样生效。

| 方法 | 路径 | 作用 |
|------|------|------|
| `GET` / `HEAD` | `/api/streams` 或 `/api/stat` | 当前直播路 JSON（字段对齐运维台：编码/分辨率/fps/GOP/码率/声道/输入输出/时长/协议/客户端数） |
| `GET` / `POST` | `/api/kick` | 踢该路**全部拉流客户端**（不杀 ffmpeg 进程、不关推流） |

踢流查询参数（任选一种）：

| 参数 | 示例 | 含义 |
|------|------|------|
| `app` + `stream` | `?app=live&stream=ls_test_1080p` | 单路 |
| `stream` | `?stream=ls_a,ls_b` | 按流名匹配（可跨 app）；逗号批量 |
| `streams` / `spec` | `?streams=live/ls_a,live/ls_b` | `app/stream` 列表 |
| `all=1` | `?all=1` | 踢当前所有路的拉流客户端 |

```bash
# 统计
curl -s "http://127.0.0.1:8091/api/streams?token=SECRET"

# 踢单路
curl -s "http://127.0.0.1:8091/api/kick?app=live&stream=ls_test_1080p&token=SECRET"

# 批量
curl -s "http://127.0.0.1:8091/api/kick?streams=live/ls_a,live/ls_b&token=SECRET"

# 全部拉流
curl -s -X POST "http://127.0.0.1:8091/api/kick?all=1&token=SECRET"
```

`/api/streams` 示例：

```json
{
  "code": 0,
  "streams": 1,
  "data": [{
    "app": "live",
    "stream": "ls_test_1080p",
    "schema": "live/ls_test_1080p",
    "video_codec": "H264",
    "width": 1920,
    "height": 1080,
    "fps": 25.0,
    "gop": 2.0,
    "video_bitrate": 4550000,
    "audio_codec": "AAC",
    "sample_rate": 48000,
    "channels": 2,
    "audio_bitrate": 128000,
    "bitrate_in": 4678000,
    "bitrate_out": 9230000,
    "bytes_in": 112721920,
    "bytes_out": 225443840,
    "status": "active",
    "duration": 724,
    "duration_str": "12m 4s",
    "clients": 2,
    "clients_flv": 2,
    "clients_ts": 0,
    "clients_mp4": 0,
    "protocols": ["flv"]
  }]
}
```

码率单位均为 **bps**；`status` 为 `active` / `wait` / `idle`。踢流成功：`{"code":0,"kicked":2,"streams":["live/ls_a"]}`（`kicked` 为当时在线客户端数）。未找到流：HTTP 404 且 `code=-2`。

也可走进程 `-zmq`（回包较短，多路时可能被截断，运维请用 HTTP）：

```bash
./asr_cmd "ffmpeg streams" "$CMD_SOCK"
./asr_cmd "ffmpeg kick live/ls_test_1080p" "$CMD_SOCK"
./asr_cmd "ffmpeg kick live/ls_a,live/ls_b" "$CMD_SOCK"
./asr_cmd "ffmpeg kick all" "$CMD_SOCK"
```

## 4. 合屏第二路（RTMP + HLS / live 预览）

主输出仍推 RTMP。加 `-http_server -http_live_bind live/stream_out:0` 后即可拉 `/live/stream_out.flv`。需要浏览器 HLS 时再加第二路 `-f hls`。完整 filtergraph 见 [mixing_cuda.md](./mixing_cuda.md) §3.6。

```bash
./ffmpeg -task_id 123456 \
  -zmq ipc:///data/LCMS/sock/123456_cmd.sock \
  -http_server http://0.0.0.0:8091 \
  -http_live_bind live/stream_out:0 \
  -http_token SECRET \
  -http_allow 10.0.0.0/8,127.0.0.1 \
  -http_root /data/LCMS/hls/123456 \
  -init_hw_device cuda=hw:0 -filter_hw_device hw \
  -filter_complex '...' \
  -map '[vout]' -map '[aout]' \
  -c:v h264_nvenc -preset p4 -b:v 6000k -maxrate 6000k -bufsize 12000k -b_scale_ratio 1.4 \
  -c:a aac -b:a 128k \
  -f flv rtmp://test-push.live.qiyi.domain/live/stream_out \
  -map '[vout]' -map '[aout]' \
  -c:v h264_nvenc -preset p4 -b:v 2500k \nodelay
  -c:a aac \
  -f hls -hls_time 2 -hls_list_size 8 \
    -hls_flags delete_segments+independent_segments \
    -hls_segment_type fmp4 \
    /data/LCMS/hls/123456/index.m3u8
```

拉流：`http://host:8091/live/stream_out.flv?token=SECRET`（主路 GOP）。两路都预览：

```text
-http_live_bind live/stream_out:0 -http_live_bind live/preview:1
```

则 `/live/preview.flv` 是 2500k 那路；HLS 仍是 `http://nodelayhost:8091/index.m3u8`。

只出 HLS、不推 RTMP：去掉 `-f flv ...` 那一路即可。

## 注意事项

- 对外请用 `https://` + `-http_cert/key`，并加 `-http_token` 或 `-http_auth`，以及 `-http_allow`
- 同时在线直播路数不再受 32 条线程限制；`-http_workers` 是 EventWorker 数（默认=核数）。受 `ulimit -n` 和每连接 64KB 协程栈限制
- 明文文件 GET 走 `sendfile`；HTTPS 文件仍 `SSL_write`。直播路径按格式懒 mux，同格式观众共享 ring。播放器拿到的是 **FLV tag / TS / fMP4 fragment**，不是 GOP 列表里的原始 `AVPacket`。包格式按 NALU 头判断：`00 00 00 01`/`00 00 01` 为 Annex B（转码常见），4 字节长度前缀为 AVCC（`-c copy` 常见）。`.flv`/`.mp4` 要 AVCC；`.ts` 要 Annex B，copy 时用 `h264_mp4toannexb` 并在 IDR 前插入 SPS/PPS。HTTP-fMP4 用 `delay_moov`+逐帧 CMAF，header 始终带 ftyp+moov
- HLS 是写盘再 GET；live 务必 `delete_segments` + 小 `hls_list_size`，避免盘满。
- `/{app}/{stream}.*` 不涨盘。`-http_live 0` 只提供已写完的文件，不会跟读增长文件。
- 启动日志应有：`([时间] http-live: GOP /live/ls_cctv.flv|.ts|.mp4` 以及 `([时间] http-server: http|https … workers=N reuseport=1 stack=64k auth=on|off`。之后每分钟一条：`http-live: streams N, stream app/name, h264, aac, gop Xs, fps N, clients N`
- 拉流应立刻看到：`([时间] http: [ip:port +Nms] connect` → `GET` → `route live` → `http-live: connect` / `gop ready wait=0ms cache=building extra=N (no wait extradata/next-IDR)` → `first media`。断开：`http-live: disconnect reason=…` 和 `http: disconnect reason=… bytes=…`。时间格式与进度行 `get_fmttime()` 相同：`([YYYY-MM-DD HH:MM:SS,mmm]`
- 有 GOP 时 `wait=` 应为 0。不再等 H.264 extradata 或下一个闭合 GOP。按 NALU 头分流：Annex B（转码）→ `.flv`/`.mp4` 转 AVCC、`.ts` 原样；AVCC（copy）→ `.flv`/`.mp4` 原样、`.ts` 走 `h264_mp4toannexb`（IDR 补 SPS/PPS）
- AAC 按封装转：GOP 保持编码器原包。`.ts` 要 ADTS（裸帧则补 ASC 让 mpegts 加头）；`.flv` / `.mp4` 要 ASC + 裸帧（ADTS 则剥 7/9 字节头，等同 `aac_adtstoasc`）
- 404：根目录与 muxer 输出路径不一致，或切片尚未写出（等 1～2 个 `hls_time`）；直播 app/stream 写错也会 404（不会误进 GOP）
- 直播路径返回 503：还在等第一帧视频或第一个 I 帧（约 1 个 `-g` 间隔）；有缓存 GOP 时不应再等
- HTTP 预览要稳：优先整路转码（`fps`/`aresample` + 编码器出 IDR）。`-c:v copy` 保留源 GOP/时间戳；再配 `-c:a aac` 是**两条时钟**。RTMP 入请加 `-f live_flv`（与转码命令一致），不要依赖 8.x 已删除的 `-async`
- 实现已同时在 `ffmpeg-8.1.2` 与 `ffmpeg4.4.1.iqiyi`；命令与路径相同
- `-stream_loop` 串文件：每圈按音视频结束时刻的 **min** 切掉较长一轨，再用 `AV_TIME_BASE` 把同一段时长加到下一圈。容器标明 duration 时第一圈就切；否则第一圈整段读完才量出切点。时间戳是 `原始戳 + n*T`，不会回到 0。`-c:a aac` 的编码器 priming 仍可能让预览端微漂
- FFmpeg 8 `-re` / `-readrate`：按文件里**最慢那路**的 DTS 对齐墙钟，不再对每路分别 `sleep`。ZLM 录的 MP4 往往音频成簇、视频成簇，旧 8.1 会越跑越慢。网络卡住后默认仍以 `readrate×1.05` 追赶；若还想更快追上可用 `-readrate_catchup 10`



## 踩坑


| 现象                                                           | 处理                                                                                |
| ------------------------------------------------------------ | --------------------------------------------------------------------------------- |
| 浏览器跨域失败                                                      | 确认走的是 `-http_server`（自带 CORS），不是裸文件或其它端口                                          |
| 直播 404                                                       | 路径必须是 `/{app}/{stream}.flv`（默认 `/live/live.flv`），不是旧的 `/live.flv`                 |
| 想下文件却一直挂着                                                    | 加 `-http_live 0`，走点播；默认 1 会把匹配的 app/stream 当直播                                    |
| 起播仍要 4～5s                                                    | 看 `gop ready wait=`：有缓存应为 0。`cache=none` 才是在等第一帧 I。断开看 `disconnect reason=`       |
| 预览码率不对                                                       | 用 `-http_live_bind live/name:N` 绑到对应输出                                            |
| 401 Unauthorized                                             | 加 `?token=` 或 Basic/`Authorization: Bearer`，与 `-http_token`/`-http_auth` 一致       |
| 403 Forbidden / `deny whitelist`                             | 源 IP 不在 `-http_allow`。应立刻看到 `http 403 Forbidden`；旧版本静默断 TCP 会让 ffplay 一直挂着        |
| `/api/streams` 401                                           | 与拉流相同，加 `?token=` 或 Basic |
| 踢流后客户端立刻断开 `reason=kicked`                             | 预期；只踢拉流，不停止编码。批量用逗号或 `all=1` |
| 503 Service Unavailable                                      | GOP 未就绪（等第一帧 I）或直播关闭；不再有 HWM=128 队列满 |
| https 起不来                                                    | 缺 `-http_cert`/`-http_key`，或 PEM 不匹配；需 OpenSSL 构建                                 |
| 慢客户端 `reason=lagged`                                        | 真落后才会 skip（`rpos` 被覆盖）。追上（`rpos==wpos`）不再误报；旧版本会连续刷这条 |
| 一直 503                                                       | 编码还没出第一帧视频；或 `?lowdelay=1` 在等下一 I                                                 |
| `Malformed AAC bitstream` / `aac_adtstoasc`（FLV/MP4）         | GOP 里是 ADTS。进程需带本次剥 ADTS 的修复；`.ts` 保持 ADTS                                        |
| `AAC bitstream not in ADTS format and extradata missing`（TS） | 裸 AAC 且缺 ASC。进程需带补 extra 的修复；`.flv`/`.mp4` 要 ASC+裸帧，`.ts` 要 ADTS                  |
| `Packet duration: -N / dts: …` + `pts has no value`（copy 拉 `.mp4`，多为音频 stream 1） | copy 的 AAC 包会和上一包 `dts+duration` 重叠，movenc 把 pts 清掉。需带本次写包前校正时间戳的进程 |
| `Track N starts with a nonzero dts` / `non monotonically increasing dts`（连上 `.mp4` 时） | 旧进程 `empty_moov` 后首包 dts 不是 0；懒 mux 把 GOP 里当前包写了两遍。需带 `delay_moov` + enable 后不再重复写的进程 |
| copy 比转码更容易花屏 / 卡顿 / 音画不同步（`-c:v copy -c:a aac`） | 不是纯 copy：视频保留源 dts/GOP，音频解码再编码。RTMP 请加 `-f live_flv`（`AVFMT_TS_DISCONT`），否则跳跃 dts 会被丢掉。FLV 的 KEY 可能是非 IDR 的 I 帧，GOP 从这里切会起播花屏。稳妥做法：两边都转码，或 `-c copy` 且音频也不重编。8.x 无 `-async` |
| `-stream_loop` / 文件读到尾再循环，时间长了音画漂 | 旧进程用 `max_pts-min_pts` 跨 timebase 累加，两轨长度不同则接点空洞/重叠。现进程按 `min(音视频 pts+dur)` 切掉长轨尾巴，`duration` 用 `AV_TIME_BASE`。无容器 duration 时第一圈接点仍可能不齐。copy+音频重编、HTTP `live_sanitize_ts` 单边推音频仍可能漂 |
| FFmpeg 8 `-re -c copy` 推 RTMP/`-f null` 刷 `Resumed reading at pts … rate 1.050 after a lag`，`speed` 不足、中途卡顿；4/6 正常 | 8.1 对每路 DTS 分别 sleep，交错差的 MP4 上超前轨拖住落后轨，lag 只增不减。需带本次「只按最慢路限速」的 8.1.2。4.4/6.1 没有 catch-up 时钟。若仍偶发 lag（出口阻塞）再加 `-readrate_catchup 10` |
| copy 比转码更容易花屏 / `.ts` 首次或切换格式像没缓存、出流慢 | copy 是 AVCC，转码是 Annex B。旧进程把 AVCC 原样打进 TS（缺起始码和 SPS/PPS），且 TS 干等下一个 I。需带本次按 NALU 头转换 + `h264_mp4toannexb` + TS 从 GOP 缓存起播的进程 |
| `Invalid NAL unit 0` / `no frame` / `log2_max_frame_num_minus4=31` / `missing picture`（`.mp4`/`.flv` 花屏，`.ts` 稍好） | GOP 缓存的是编码器包；播放器读的是 mux 后的 ring。TS 能靠 `0x47` 再同步。旧进程把 P 帧 fMP4 盒子标成关键帧、或从 oldest 半包起播、或 Annex B 当 AVCC 写入。需带本次 fragment 延后标 key + 无 IDR 则等待 + acc 回写的进程 |
| `Invalid NAL unit size` / `sps_id out of range`（FLV/MP4）     | 包是 Annex B、头却标成 AVCC。拉 `.ts` 正常而 `.flv`/`.mp4` 刷 NAL 错即此问题；进程需带本次 Annex B→AVCC 修复 |
| bind 失败                                                      | 端口占用，或未 `--enable-network`                                                        |
| HLS 一直 404                                                   | `-http_root` 与 `-f hls` 路径不是同一目录                                                  |
