# dynamic_input

多路动态拉流：独立 demux/decode、重连、持帧、音视频 pad；视频优先 NVDEC→CUDA。整链路见 [mixing_cuda.md](./mixing_cuda.md)。

## 变更记录

| 日期 | 说明 |
|------|------|
| 2026-08-06 | 成片嘴型：恢复 A/V **共享 due** 放行；仅丢落后于已放音频的旧画，禁止“声走、画 hold”拆节目轴 |
| 2026-08-06 | `|A−V|≤80ms` 静默；异常 `dyn_sync` WARNING（cause/fix） |
| 2026-08-06 | 文档独立成册（自合屏文档拆分参数/命令要点） |
| 2026-08-04 | 统一图内 zmq：`search`/`update`/`remove`/`purge`；废弃 `cmd_*.sock` 与 `add`/`switch` |
| 2026-08-04 | `update` 旁路并行 open；槽区间批量；remove 软拆除+持帧 |
| 2026-08-05 | 换源 264↔265 探测；`r` 为输出节拍不跟输入自动变 |

## 设计方案（核心功能）

1. **多 pad 输出**：`v0..vN-1`（可选 `a0..aN-1`）；命令插槽 **1..N**（0=底色）
2. **直播节拍**：`live=1` 时视频按墙钟 `r` 节流；`s=` 只声明帧池/占位，**不强制缩放**业务分辨率
3. **单路同步（成片嘴型）**：prime 后 A/V 走**同一 media→wall due**；可丢明显落后于已放音频的旧画；**不得**在音频继续出的同时单独 hold 视频（会把嘴型拆开）
4. **控制面**：图内统一 zmq；`update` 先 probe+打开成功再热替换

源文件：`libavfilter/vf_dynamic_input.c`。路数上限 32。成片对齐见 [amixrank.md](./amixrank.md)「成片嘴型怎么保证」。

## 参数说明

| 选项 | 默认 | 常用建议 | 作用 |
|------|------|----------|------|
| `outputs` | `8` | 按峰值预留（1–32） | 每类媒体 pad 数 |
| `size`/`s` | `1920x1080` | 与 mixing/color 同值 | 帧池/占位声明 |
| `r` | `25` | 与 `color_cuda` 同 fps | **输出节拍** |
| `sw_format` | `nv12` | 与 mixing `format` 一致 | 硬件 sw_format |
| `initial_urls` | — | 逗号分隔 | 启动填入插槽 1..N |
| `device` | `0` | — | CUDA 设备 |
| `enable_audio` | `1` | 合屏保持 `1` | 是否输出音频 pad |
| `sample_rate` | `48000` | **48000** | 音频采样率 |
| `reconnect_ms` | `2000` | `1000~3000` | 重连间隔 |
| `live` | `1` | 直播=`1` | 视频墙钟节流 |
| `playout_delay_ms` | `40` | **40~80** | PTS 对齐后抖动垫 |
| `av_wait_ms` | `6000` | **3000~8000** | 等对端 A/V |
| `pts_jump_s` | `10` | 保持 **10** | PTS 跳跃修正阈值（秒） |
| `sync_warn_av_ms` | `80` | **80** | `\|A−V\|` 超过才 `dyn_sync` 告警；以内静默 |
| `sync_warn_interval_ms` | `10000` | 保持 | 同 pad 告警节流 |

`outputs` 启动定死，运行时不能加 pad。唇同步关键：音频主导 + `playout_delay_ms` / `av_wait_ms`。

### 异常日志（`dyn_sync`）

正常不同步打印。仅 WARNING，模板：`dyn_sync: padN IMBALANCE code=... | cause: ... | fix: ...`

| code | 含义 |
|------|------|
| `AV_SKEW` | 源 A/V media PTS 偏差 > `sync_warn_av_ms` |
| `DROP_A` | 音频队列溢出丢帧（严重，违反音频优先） |
| `DROP_V` / `V_CATCHUP_HEAVY` | 视频队列满或为跟音频大量丢旧画 |

## 用法示例

```text
# 4 路会议
dynamic_input=outputs=4:enable_audio=1:live=1:s=1920x1080:r=25:sample_rate=48000:playout_delay_ms=40:av_wait_ms=6000:pts_jump_s=10:initial_urls=rtmp://u0\,rtmp://u1\,rtmp://u2\,rtmp://u3

# 16 路预留、只开 1 路
dynamic_input=outputs=16:enable_audio=1:live=1:s=1920x1080:r=25:sample_rate=48000:initial_urls=rtmp://host/live/u0
```

### 运行时命令（经统一 zmq）

```text
dynamic_input search
dynamic_input update 2 rtmp://host/live/u2
dynamic_input update 3-6 rtmp://host/live/u3
dynamic_input remove 2
dynamic_input purge
```

完整命令约定与模板见 [mixing_cuda.md](./mixing_cuda.md) §3 / §7。

## 注意事项

- 插槽 0 是底色，不走本滤镜命令
- `r` 不随输入 25↔50 自动改变
- 废弃：`cmd_*.sock`、`add`/`switch`

## 踩坑记录

| 现象 | 处理 |
|------|------|
| 侧窗黑/卡 | `r` 与 color 一致；mixing `hold_stall_ms` |
| 音画偏 | 看 `dyn_sync AV_SKEW`；同跳后仍齐则正常；勿靠加大 amixrank `buffer_ms` 掩盖 |
| `DROP_A` | 下游卡住/编码慢；保音频，查 backpressure |
| 换源花屏 | 用现行 `update`（先开再换） |
| 格式不符 | `sw_format` 与 mixing `format` 一致 |
