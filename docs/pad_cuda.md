# pad_cuda

CUDA 补边：表达式控制画布与偏移；可选 **Fit-inside 缩放后居中**（`auto_scale`）；支持 **NV12 / P010** 等。

## 变更记录

| 日期 | 说明 |
|------|------|
| 2026-08-06 | 新增 `auto_scale=1` Fit-inside；支持 P010；文档合并为单文件 |

## 设计方案（核心功能）

1. **表达式 pad**：`w`/`h`/`x`/`y` 变量同 CPU（`iw`/`ih`/`ow`/`oh`…）
2. **`auto_scale=1`**：`scale=min(ow/iw,oh/ih)` 等比放入后 pad；求 x/y 时 `iw`/`ih` 为缩放后尺寸
3. **P010**：`ushort`/`ushort2` 拷贝与双线性缩放 kernel

| 文件 | 职责 |
|------|------|
| `vf_pad_cuda.c` | 表达式、`auto_scale`、填色、调度 |
| `vf_pad_cuda.cu` | `pad_*` / `pad_scale_*` |

`auto_scale=0`：要求画布 ≥ 输入。构建：`CONFIG_PAD_CUDA_FILTER`。

## 参数说明

| 选项 | 默认 | 常用取值 | 作用 |
|------|------|----------|------|
| `width`/`w` | `iw` | `1920` | 画布宽 |
| `height`/`h` | `ih` | `1080` | 画布高 |
| `x` / `y` | `0` | `(ow-iw)/2` / `(oh-ih)/2` | 放置偏移 |
| `color` | `black` | `black`；调试用 `blue` | 补边色 |
| `eval` | `init` | 分辨率会变用 `frame` | 求值时机 |
| `aspect` | `0` | 保持 `0` | 历史选项 |
| `auto_scale` | `0` | 多分辨率入画布用 **`1`** | Fit-inside |

表达式变量：`iw`/`ih`/`ow`/`oh`/`a`/`sar`/`dar`/`hsub`/`vsub`。  
`auto_scale=1` 求 x/y 时，`iw`/`ih` = **缩放后**尺寸。

## 用法示例

```text
# 推荐：1080p 等比居中
pad_cuda=1920:1080:(ow-iw)/2:(oh-ih)/2:color=black:auto_scale=1

# 传统 pad（输入不得大于画布）
pad_cuda=1920:1080:(ow-iw)/2:(oh-ih)/2:color=black

hevc_cuvid → pad_cuda=1920:1080:(ow-iw)/2:(oh-ih)/2:auto_scale=1 → hevc_nvenc
```

## 注意事项

- 输入分辨率不固定时务必 `auto_scale=1`
- 支持格式：`YUV420P`/`YUV444P`/`YUVA*`/`NV12`/`P010`
- 居中公式在 `auto_scale=1` 下才对「缩放后画面」正确

## 踩坑记录

| 现象 | 处理 |
|------|------|
| `Padded size < input size` | 加 `auto_scale=1` |
| 未居中 | 检查 `(ow-iw)/2` 且开 `auto_scale` |
| Unsupported format | 先 `scale_cuda=format=nv12` 或 `p010` |
