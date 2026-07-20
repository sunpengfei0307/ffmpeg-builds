# RTMP/FLV 国内数字 CodecID 兼容（参考 6.1.1.iqiyi）

日期：2026-07-20  
状态：已实现  
范围：`ffmpeg-8.1.2` 的 `libavformat/flv.h` / `flvdec.c` / `flvenc.c`

## 背景

国内 CDN / 旧推拉流仍大量使用数字 CodecID；Enhanced RTMP FourCC 为可选。

| 类型 | CodecID |
|------|---------|
| HEVC | 12 |
| AV1  | 13 |
| VP8  | 14 |
| VP9  | 15 |
| Opus | SoundFormat=9（与 Enhanced ExHeader 同值） |

## 行为

### Demux（始终开启）

- 识别视频数字 ID：12/13/14/15
- SoundFormat=9：peek 后续 FourCC；已知 Enhanced FourCC 则走 Enhanced，否则按 legacy Opus
- legacy AV1/VP8/VP9 读取 composition time

### Mux（默认数字 ID，对齐 6.1.1）

- **默认**：写国内数字 CodecID / Opus=9
- `-flvflags ext_header`：改写 Enhanced RTMP FourCC（`hvc1` / `av01` / `Opus` 等）

```bash
# 默认：国内 CDN 数字 ID
ffmpeg -i in.mp4 -c:v libx265 -c:a libopus -f flv rtmp://cdn/live/stream

# Enhanced FourCC
ffmpeg -i in.mp4 -c:v libx265 -c:a libopus \
  -f flv -flvflags ext_header \
  -rtmp_enhanced_codecs hvc1,Opus \
  rtmp://cdn/live/stream
```
