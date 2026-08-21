# eq_cuda

CUDA 亮度/对比度/伽马/饱和度调节。参数与 CPU `eq` 相同，并支持 **NV12 / P010**。

## 变更记录

| 日期 | 说明 |
|------|------|
| 2026-08-06 | 新增 `eq_cuda`：选项对齐 CPU `eq`；支持 NV12/YUV420P/YUV444P/P010；文档合并为单文件 |

## 设计方案（核心功能）

1. **参数语义对齐 CPU `eq`**：`contrast` / `brightness` / `saturation` / `gamma` / `gamma_r|g|b` / `gamma_weight` / `eval` / `process_command`
2. **格式扩展**：在 CPU 仅 8bit 之外，增加 **P010（10bit 左对齐）**，服务 HDR 全 GPU 链路
3. **主机建 LUT + GPU 查表**：8bit LUT256；10bit LUT1024；Y/U/V 分平面（NV12/P010 用 packed UV kernel）

| 文件 | 职责 |
|------|------|
| `libavfilter/vf_eq_cuda.c` | 表达式、LUT、上传、调度 |
| `libavfilter/vf_eq_cuda.cu` | `eq_uchar` / `eq_uchar2` / `eq_ushort` / `eq_ushort2` |

算法：

```text
v = contrast*(i/max - 0.5) + 0.5 + brightness
v = v*(1-gw) + pow(v, 1/gamma)*gw
```

构建：`CONFIG_EQ_CUDA_FILTER`（需 **GPL** + `ffnvcodec` + `cuda_nvcc|cuda_llvm`）。

## 参数说明

| 选项 | 默认 | 硬裁剪 | 常用安全区间 | 作用 |
|------|------|--------|--------------|------|
| `contrast` | `1.0` | [-1000, 1000] | **0.85 ~ 1.25** | 对比度；主要 Y |
| `brightness` | `0.0` | [-1, 1] | **-0.08 ~ 0.08** | 亮度偏移；Y |
| `saturation` | `1.0` | [0, 3] | **0.85 ~ 1.25** | 饱和度；U/V |
| `gamma` | `1.0` | [0.1, 10] | **0.85 ~ 1.20** | 主伽马 |
| `gamma_r` / `gamma_g` / `gamma_b` | `1.0` | [0.1, 10] | **0.90 ~ 1.10** | 通道伽马权重 |
| `gamma_weight` | `1.0` | [0, 1] | **0.5 ~ 1.0**（常保持 1） | 线性与伽马混合比 |
| `eval` | `init` | `init`/`frame` | 固定参数用 **`init`** | 何时求表达式 |

表达式变量：`n` / `r` / `t`。HDR 建议把安全区间再收一档。

- **contrast**：轻度 `1.05~1.15`；`>1.4` 易死黑死白
- **brightness**：微调 `±0.02~0.05`
- **saturation**：补色 `1.05~1.15`；`0`=灰度
- **gamma**：`>1` 中间调变亮；Y 实际为 `gamma*gamma_g`
- **gamma_r/g/b**：校色用，映射同 CPU（Y=`gamma*gamma_g`，U=`sqrt(gamma_b/gamma_g)`，V=`sqrt(gamma_r/gamma_g)`）

## 用法示例

```text
# 轻微提神
eq_cuda=contrast=1.08:brightness=0.02:saturation=1.08

# 暗场略提亮
eq_cuda=gamma=1.12:brightness=0.01

# SDR
h264_cuvid → eq_cuda=contrast=1.1:brightness=0.02 → hevc_nvenc

# HDR P010
hevc_cuvid → eq_cuda=contrast=1.05:gamma=1.1 → scale_cuda → hevc_nvenc
```

## 注意事项

- 一次只动 1~2 个参数；先 contrast/brightness，再 saturation，最后 gamma
- 硬裁剪远大于观感安全范围，以「常用安全区间」为准
- 需 `--enable-gpl`

## 踩坑记录

| 现象 | 原因/处理 |
|------|-----------|
| Unsupported format | 仅 NV12/YUV420P/YUV444P/P010；其它先 `scale_cuda=format=` |
| 无效果 | 参数为 identity；或 `eval=frame` 表达式未依赖 `t`/`n` |
| 链接失败 | 未开 GPL |
