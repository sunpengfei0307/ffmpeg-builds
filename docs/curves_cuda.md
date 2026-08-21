# curves_cuda

CUDA RGB 曲线调色：直接吃 **NV12/P010**，内部 YUV↔RGB 后查表写回；选项与 CPU `curves` 一致。

## 变更记录

| 日期 | 说明 |
|------|------|
| 2026-08-06 | 新增 `curves_cuda`：NV12/P010 内部转 RGB；文档合并为单文件 |

## 设计方案（核心功能）

1. **选项对齐 CPU**：`preset` / `master` / `red|green|blue` / `all` / `psfile` / `plot` / `interp`
2. **输入 NV12/P010**：无需外部 `scale_cuda` 转 RGB；输出 `sw_format` 不变
3. **主机建 LUT + 融合 kernel**：BT.709 YUV↔RGB（尊重 `color_range`）；master 合成同 CPU

| 文件 | 职责 |
|------|------|
| `vf_curves_cuda.c` | 控制点/preset/psfile、插值、LUT 上传 |
| `vf_curves_cuda.cu` | `curves_nv12` / `curves_p010` |

LUT 深度：NV12→256，P010→1024。构建：`CONFIG_CURVES_CUDA_FILTER`。

## 参数说明

| 选项 | 默认 | 常用建议 | 作用 |
|------|------|----------|------|
| `preset` | `none` | 直播：`medium_contrast` / `lighter` / `darker` | 内置风格 |
| `master` / `m` | — | 2~4 点；中间点偏离对角线 ≤0.15 | 主曲线 |
| `red`/`green`/`blue` | — | 中间点偏离 ≤0.08 | 分通道 |
| `all` | — | 同 master 幅度 | 复制到 R/G/B |
| `interp` | `natural` | 保守用 `pchip` | 插值 |
| `psfile` / `plot` | — | 调试用 | PS 文件 / gnuplot |

控制点：`x/y`，x,y∈[0,1]，x 严格递增。

### preset

| preset | 观感 | 直播默认 |
|--------|------|----------|
| `none` / `lighter` / `darker` / `linear_contrast` | 轻调整 | 较安全 |
| `medium_contrast` | 中等对比 | **常用** |
| `increase_contrast` / `strong_contrast` | 强对比 | 慎用 |
| `vintage` / `cross_process` / `negative` / `color_negative` | 风格/特效 | 非默认 |

## 用法示例

```text
# 整体提亮且不偏色（只动 master）
curves_cuda=master='0/0 0.4/0.5 1/1'

# 更轻提亮
curves_cuda=master='0/0 0.5/0.55 1/1'

# 安全增强对比
curves_cuda=preset=medium_contrast

# 轻 S
curves_cuda=master='0/0 0.25/0.22 0.75/0.78 1/1'

hevc_cuvid → curves_cuda=preset=medium_contrast → hevc_nvenc
```

## 注意事项

- **不偏色**：只设 `master=` 或 `preset=lighter`；勿同时改 r/g/b
- 有 `master` 时：`out = master(channel(in))`
- 矩阵固定 BT.709；与 CPU 软件 RGB 观感可能略有差异（YUV 往返）

## 踩坑记录

| 现象 | 处理 |
|------|------|
| Unsupported format | 仅 NV12/P010 |
| 解析失败 | x 未递增或超出 [0,1] |
| 偏色严重 | 勿叠 r/g/b；检查 `color_range` |
