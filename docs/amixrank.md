# amixrank

多路音频混音 + 响度排序：主讲 ducking、FIFO 控延迟，并向 `mixing_cuda` PUB 响度 rank。整链路见 [mixing_cuda.md](./mixing_cuda.md)。

## 变更记录

| 日期 | 说明 |
|------|------|
| 2026-08-06 | 补充 `search` 回复字段说明：`槽:enabled:weight` |
| 2026-08-06 | `search` 支持单槽/区间：`search` / `search 1` / `search 1-4` |
| 2026-08-06 | zmq/命令：入口 WARNING 打印；失败 ERROR 详报 |
| 2026-08-06 | `set_active` 支持区间：`1-4 0`（同 `dynamic_input update`） |
| 2026-08-06 | 运行时 `set_active <槽\|区间> <0\|1>`：关=静音混出+排空 FIFO；开=join_fade；`search`；默认 `buffer_ms=200` |
| 2026-08-06 | 成片嘴型：FIFO>2×buffer 削回 buffer（声不能相对节目画面无限堆晚）；与 dynamic_input 共享 due 配合 |
| 2026-08-06 | OVERFLOW 文案：指向单路进率；足帧时勿把 `speed=` 当主因 |
| 2026-08-06 | 移除旧选项（无兼容）：`prebuffer_ms`/`target_latency_ms`/`max_latency_ms`/`trim_grace_*`/`soft_trim_ms` |
| 2026-08-06 | latency 收敛为单一 `buffer_ms`；去掉多层伺服/软砍 |
| 2026-08-06 | 异常 `amix_sync` WARNING（含 cause/fix）；正常静默 |
| 2026-08-06 | 文档独立成册 |
| 2026-08-04 | 软 duck / `join_fade_ms`；无 task_id 时 `gain_<pid>.sock` |

## 设计方案（核心功能）

1. **混音 + ducking**：input0 通常为 `anullsrc`（节目时钟，`weights=0`）；业务路接 `dynamic_input` 音频
2. **延迟策略**：单一 `buffer_ms` 抖动垫；>2× 削回以保成片嘴型；`startup_grace_ms` 内不削
3. **激励 PUB**：JSON rank → mixing SUB（主讲边框 / speaker 布局）

源文件：`libavfilter/af_amixrank.c`。

### 延迟模型（摘要）

| 参数 | 默认 | 作用 |
|------|------|------|
| `buffer_ms` | **200** | 入混垫；>**2×** 时削回 buffer（保成片嘴型） |
| `startup_grace_ms` | 10000 | 启动宽限（不削/不报 OVERFLOW） |
| `join_fade_ms` | 500 | 冷插入渐入（期间不做 FIFO 检查） |

### 成片嘴型怎么保证

| 层 | 谁 | 规则 |
|----|-----|------|
| 源内容 | `dynamic_input` | A/V **同一 media→wall due** 放出；可丢过时画面，**禁止**音频在走、画面单独 hold 导致节目轴嘴型裂 |
| 节目钟 | `color_cuda` + `anullsrc` | 画/声节拍对齐 |
| 混音延迟 | `amixrank` | 垫 `buffer_ms`；堆到 >2× 则削回，避免声相对已合成画面越堆越晚 |

### 异常日志（`amix_sync`）

| code | 含义 / 处理要点 |
|------|----------------|
| `OVERFLOW` | >2×buffer，已削回 buffer → 查该路 `dyn_sync`/突发 |
| `AUDIO_DROP` | 紧急 OOM 护栏 → 严重失衡 |

## 参数说明

| 选项 | 默认 | 常用建议 | 作用 |
|------|------|----------|------|
| `inputs` | — | = 音频 pad 数（含 anullsrc） | 输入路数 |
| `weights` | — | `0\|1\|1\|…`（时钟路 0） | 各路权重 |
| `duration` | — | 合屏常用 `longest` | 结束策略 |
| `normalize` | `1` | 合屏常用 **`0`** | 增益归一 |
| `ducking` | `1` | 保持 `1` | 主讲优先衰减其它路 |
| `duck_gain_db` | `-18` | `-12 ~ -18` | 非主讲衰减 |
| `duck_attack_ms` | `80` | `50~120` | 增益上升软过渡 |
| `duck_release_ms` | `250` | `200~400` | 衰减/重入软过渡 |
| `duck_hold_ms` | `400` | `300~600` | 主讲切换保持 |
| `join_fade_ms` | `500` | `300~800` | 冷插入渐入 |
| `buffer_ms` | `200` | **200** | 唯一缓存限制 |
| `startup_grace_ms` | `10000` | 保持默认 | 启动宽限 |
| `sync_warn_interval_ms` | `10000` | 保持 | `amix_sync` 节流 |
| `rank_endpoint` | — | 默认同 mixing / `gain_<pid>.sock` | zmq PUB |
| `rank_interval_ms` | `100` | `100` | 发布间隔 |

## 用法示例

```text
anullsrc=channel_layout=stereo:sample_rate=48000[asil];
[asil][a0][a1][a2][a3]amixrank=inputs=5:normalize=0:ducking=1:duck_gain_db=-18:rank_interval_ms=100:weights=0|1|1|1|1:buffer_ms=200[aout]
```

## 运行时命令（图内 zmq）

业务槽编号与 `dynamic_input` 一致：**1..N**（`input0=anullsrc` 不可关）。仅控音频，不联动视频 `set_active`。

| 命令 | 参数 | 行为 |
|------|------|------|
| `set_active` | `<槽\|区间> <0\|1>` | 关：不进混/rank + 排空 FIFO；开：按 `weights` 重入，`join_fade` 渐入 |
| `search` | 〔空〕/`1`/`1-4` | 查业务槽；回复见下 |

`search` 回复格式：`槽:enabled:weight`（多槽空格分隔）。

| 字段 | 含义 |
|------|------|
| 槽 | 业务插槽号 **1..N**（对应 amixrank `inputN`，不含 anullsrc） |
| enabled | `set_active` 使能：`1`=进混，`0`=已关（不进混/rank，FIFO 已排空） |
| weight | 配置权重（关路**不会**改 weights；`1:0:1` 末尾的 `1` 即权重仍为 1） |

```text
# 例：槽1已关、权重1；槽3使能、权重1
1:0:1 2:0:1 3:1:1 4:1:1
```

```bash
./asr_cmd "amixrank set_active 2 0" "$SOCK"
./asr_cmd "amixrank set_active 1-4 0" "$SOCK"
./asr_cmd "amixrank set_active 1-4 1" "$SOCK"
./asr_cmd "amixrank search" "$SOCK"
./asr_cmd "amixrank search 1" "$SOCK"
./asr_cmd "amixrank search 1-4" "$SOCK"
```

规格：[2026-08-06-amixrank-set-active-design.md](./superpowers/specs/2026-08-06-amixrank-set-active-design.md)

## 注意事项

- `inputs` / `weights` 必须与实际音频 pad 对齐
- 完整合屏模板见 [mixing_cuda.md](./mixing_cuda.md) §3

## 踩坑记录

| 现象 | 处理 |
|------|------|
| `amix_sync OVERFLOW` | 入快于 anullsrc；查编码/源速率，勿盲目加大 `buffer_ms` |
| `amix_sync AUDIO_DROP` | 严重失衡触发护栏；当 bug 查 |
| remove→update 音量泵 | 提高 `duck_hold_ms` / `join_fade_ms`；mixing `join_stable_ms` |
| mixing 无边框激励 | 确认 PUB/SUB sock 路径一致（pid vs task_id） |
