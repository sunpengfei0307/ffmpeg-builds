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

设计说明：[superpowers/specs/2026-09-02-http-live-gop-design.md](./superpowers/specs/2026-09-02-http-live-gop-design.md)

## 变更记录


| 日期         | 说明                                                                                                              |
| ---------- | --------------------------------------------------------------------------------------------------------------- |
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
| `-http_workers N`                    | worker 线程数（默认 32，队列 HWM=128）                               |
| `-http_live 1`                       | 默认开：已 bind 的 `/{app}/{stream}.flv                          |
| `-http_live_bind app/stream[:N]`     | 可重复。把 URL `/{app}/{stream}` 绑到第 N 个输出文件（省略 N 则按出现顺序 0,1,…） |


行为：

- 直播：`/{app}/{stream}.flv` → `video/x-flv`；`.ts` → `video/mp2t`；`.mp4` → `video/mp4`；无 `Content-Length`
- 默认：立刻推**最新 1 个 GOP**（当前 building，从最近 I 起）再跟实时
- `?lowdelay=1`：不推缓存，等到下一帧视频 I 再出
- HLS/DASH（`.m3u8` `.mpd` `.m4s`）永不走 GOP
- HLS/DASH MIME：`.m3u8` → `application/vnd.apple.mpegurl`；`.mpd` → `application/dash+xml`；`.m4s/.mp4` → `video/mp4`；切片 `.ts` → `video/mp2t`
- 播放列表 `Cache-Control: no-cache`；切片短缓存
- CORS：`Access-Control-Allow-Origin: *`
- 文件 GET 支持简单 `Range`（206）；直播路径不走 Range
- `GET /` 先试 `index.m3u8`，没有再试 `index.mpd`
- 拒绝路径 `..`；app/stream 不得含 `/`；不提供目录列表
- HTTPS：`-http_server https://0.0.0.0:8091 -http_cert cert.pem -http_key key.pem`（TLS 1.2+）
- 鉴权：未配置则不检查；配置后 GET/HEAD 必须带 token 或 Basic（OPTIONS 不鉴权）
- 白名单：`-http_allow` 按源 IP 过滤；不在名单内先读完请求再回 **HTTP 403 Forbidden**（不再静默关 TCP，否则 ffplay 会一直等响应头）。HTTPS 会先完成 TLS 再 403
- 起播日志：每行前缀 `([2026-09-02 15:34:11,367]`（本地时分秒+毫秒，无 `-task_id` 也会打），随后 `http: [ip:port +Nms] accept/tls/GET/auth/route`，再 `http-live: [ip] gop ready / http 200 / mux header / streaming / first media`

极致 IO 把 `-http_root` 指到 `/dev/shm/...`。音量运行时：`./asr_cmd "volume volume 0.5" ipc:///data/LCMS/sock/27026_filter.sock`（图内 `azmq`）。

## 3. 合屏第二路（RTMP + HLS / live 预览）

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
- 同时在线直播路数受 `-http_workers` 限制（默认 32）；HLS 切片请求也占用 worker
- HLS 是写盘再 GET；live 务必 `delete_segments` + 小 `hls_list_size`，避免盘满。
- `/{app}/{stream}.*` 不涨盘。`-http_live 0` 只提供已写完的文件，不会跟读增长文件。
- 启动日志应有：`([时间] http-live: GOP /live/ls_cctv.flv|.ts|.mp4` 以及 `([时间] http-server: http|https … workers=N auth=on|off`
- 拉流应立刻看到：`([时间] http: [ip:port +Nms] connect` → `GET` → `route live` → `http-live: connect` / `gop ready wait=0ms cache=building extra=N (no wait extradata/next-IDR)` → `first media`。断开：`http-live: disconnect reason=…` 和 `http: disconnect reason=… bytes=…`。时间格式与进度行 `get_fmttime()` 相同：`([YYYY-MM-DD HH:MM:SS,mmm]`
- 有 GOP 时 `wait=` 应为 0。不再等 H.264 extradata 或下一个闭合 GOP。NVENC 包是 Annex B：`.ts` 原样 mux；`.flv` / `.mp4` 在本连接 mux 时转成 AVCC（长度前缀 + AVCDecoderConfigurationRecord），不要把起始码当 NAL 长度写入
- AAC 按封装转：GOP 保持编码器原包。`.ts` 要 ADTS（裸帧则补 ASC 让 mpegts 加头）；`.flv` / `.mp4` 要 ASC + 裸帧（ADTS 则剥 7/9 字节头，等同 `aac_adtstoasc`）
- 404：根目录与 muxer 输出路径不一致，或切片尚未写出（等 1～2 个 `hls_time`）；直播 app/stream 写错也会 404（不会误进 GOP）
- 直播路径返回 503：还在等第一帧视频或第一个 I 帧（约 1 个 `-g` 间隔）；有缓存 GOP 时不应再等



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
| 503 Service Unavailable                                      | worker 队列满（默认 128）；日志 `overload … 503`                                            |
| https 起不来                                                    | 缺 `-http_cert`/`-http_key`，或 PEM 不匹配；需 OpenSSL 构建                                 |
| 一直 503                                                       | 编码还没出第一帧视频；或 `?lowdelay=1` 在等下一 I                                                 |
| `Malformed AAC bitstream` / `aac_adtstoasc`（FLV/MP4）         | GOP 里是 ADTS。进程需带本次剥 ADTS 的修复；`.ts` 保持 ADTS                                        |
| `AAC bitstream not in ADTS format and extradata missing`（TS） | 裸 AAC 且缺 ASC。进程需带补 extra 的修复；`.flv`/`.mp4` 要 ASC+裸帧，`.ts` 要 ADTS                  |
| `Invalid NAL unit size` / `sps_id out of range`（FLV/MP4）     | 包是 Annex B、头却标成 AVCC。拉 `.ts` 正常而 `.flv`/`.mp4` 刷 NAL 错即此问题；进程需带本次 Annex B→AVCC 修复 |
| bind 失败                                                      | 端口占用，或未 `--enable-network`                                                        |
| HLS 一直 404                                                   | `-http_root` 与 `-f hls` 路径不是同一目录                                                  |
