# tonemap_cuda / crop_cuda 设计规格

日期：2026-07-17  
状态：待实现  
范围：FFmpeg 8.1.2 `libavfilter` 内建 CUDA 滤镜（本仓构建树）

## 背景与目标

本仓沿用 FFmpeg 原生 CUDA 滤镜模式（宿主 `.c` + 设备 `.cu` → 嵌入 PTX），无独立运行时插件框架。现有参考：`colorspace_cuda`、`pad_cuda`、`scale_cuda`；算法/API 参考：`tonemap`、`tonemap_opencl`、`crop`。

目标：新增两个 HW-frame-aware 滤镜，使 NVDEC→滤镜→NVENC 链路可在 GPU 上完成裁剪与双向色调映射，无需 `hwdownload`。

## 已确认需求

| 项 | 选择 |
|----|------|
| 架构 | 双独立滤镜（对齐现有 CUDA 滤镜，非公共底座重构、非挂到 scale/colorspace） |
| API 完整度 | 完整：OpenCL 级 tonemap 色彩标签 + crop 表达式/`process_command` |
| 方向 | `hdr2sdr` 与 `sdr2hdr` 对称支持；`mode=hdr2sdr\|sdr2hdr\|auto` |
| 格式 | 首版仅 `nv12`、`p010` |
| 算法 | 对齐 CPU 全套：`none/linear/gamma/clip/reinhard/hable/mobius` |
| `sdr2hdr` 难逆算法 | 按算法区分：`gamma` 可逆则逆；`clip`/`mobius` 硬失败；其余近似逆 |
| crop | 物理裁剪（新 CUDA surface）；非仅改 `crop_*` 元数据 |

## 1. 架构与数据流

### 文件布局

```
libavfilter/
  vf_tonemap_cuda.c      # 选项、方向解析、帧池、元数据、kernel 调度
  vf_tonemap_cuda.cu     # YUV↔RGB、OETF/EOTF、(逆)tonemap、色域
  vf_crop_cuda.c         # 表达式、对齐、process_command、ROI 调度
  vf_crop_cuda.cu        # 按平面 ROI 拷贝到新 surface
```

### 构建接入

与 `pad_cuda` / `colorspace_cuda` 相同：

- `configure`：`*_cuda_filter_deps="ffnvcodec"`，`deps_any="cuda_nvcc cuda_llvm"`
- `Makefile`：`vf_*.o` + `vf_*.ptx.o` + `cuda/load_helper.o`
- `allfilters.c`：声明 `ff_vf_tonemap_cuda` / `ff_vf_crop_cuda`（`filter_list.c` 由构建生成）
- 本仓：依赖 `50-ffnvcodec.sh` 的 ffnvcodec +（可用时）`cuda_llvm`；必要时显式 `--enable-filter=tonemap_cuda,crop_cuda`

### 典型管线

```
NVDEC(CUDA)
  → crop_cuda=w:h:x:y
  → tonemap_cuda=mode=hdr2sdr:tonemap=hable:format=nv12
  → scale_cuda / NVENC

NVDEC(CUDA)
  → tonemap_cuda=mode=sdr2hdr:tonemap=hable:t=smpte2084:format=p010
  → NVENC(HEVC 10-bit)
```

### 运行时约定

- 输入：`AV_PIX_FMT_CUDA` + 合法 `hw_frames_ctx`
- `sw_format`：仅 `NV12` / `P010`
- 使用 `AVCUDADeviceContext` 的 context / stream；`ff_cuda_load_module` 加载嵌入 PTX
- 标志：`FF_FILTER_FLAG_HWFRAME_AWARE`
- 生命周期对齐现有滤镜：`init` → `config_props` → `filter_frame` → `uninit`

**tonemap_cuda 数据流**

1. `config_props`：解析 `mode` → 建输出帧池 → 加载 kernel  
2. `filter_frame`：push ctx → launch → 更新色彩标签与 HDR side data → pop → 下传  
3. 设备端单 pass：`YUV → linear RGB → tonemap/inverse → 输出 transfer → YUV`

**crop_cuda 数据流**

1. `config_props`：解析 `w/h` → 建更小输出帧池 → 加载 copy kernel  
2. `filter_frame`：每帧求 `x/y` → 色度对齐 → ROI copy → 更新尺寸/SAR → 下传  
3. 不做零拷贝-only 路径（CPU `crop` 已覆盖逻辑裁剪）

## 2. API / 选项与方向规则

### tonemap_cuda

| 选项 | 说明 | 默认 |
|------|------|------|
| `mode` | `hdr2sdr` / `sdr2hdr` / `auto` | `auto` |
| `tonemap` | `none` `linear` `gamma` `clip` `reinhard` `hable` `mobius` | `hable` |
| `param` | 算法参数（语义对齐 CPU `tonemap`） | 按算法 |
| `desat` | 去饱和（对齐 OpenCL） | `0.5` |
| `peak` | 信号峰值；`0` 时从帧侧数据 / `ff_determine_signal_peak` 推断 | `0` |
| `threshold` | 场景检测阈值（对齐 OpenCL） | `0.2` |
| `transfer` / `t` | 输出 transfer | 按 mode |
| `matrix` / `m` | 输出 matrix：`bt709` `bt2020` | 按 mode |
| `primaries` / `p` | 输出 primaries：`bt709` `bt2020` | 按 mode |
| `range` / `r` | `tv`/`limited` / `pc`/`full` | 输入已标注则保持，否则 `tv` |
| `format` | 输出 `sw_format`：`nv12` / `p010` / `same` / 未指定 | 未指定：按 mode 偏好；`same`：强制保持输入 `sw_format` |

**`mode=auto`**

- 输入 transfer ∈ `{smpte2084, arib-std-b67}` → `hdr2sdr`
- 输入 transfer 为 SDR 族（如 `bt709`、`bt2020-10`、`iec61966-2-1`）→ `sdr2hdr`
- 无法判断 → `AVERROR(EINVAL)`，要求显式 `mode=`

**mode 默认输出标签**

- `hdr2sdr`：`t=bt709, m=bt709, p=bt709`；`format` 未指定 → `nv12`；`format=same` → 保持输入
- `sdr2hdr`：`t=smpte2084, m=bt2020, p=bt2020`；`format` 未指定 → `p010`；`format=same` → 保持输入

**`sdr2hdr` 算法可用性**

- 可逆/近似逆：`none`、`linear`、`hable`、`reinhard`；`gamma` 在参数可解析逆时启用
- 硬失败：`clip`、`mobius`，以及不可逆的 `gamma` → `AVERROR(EINVAL)` + 明确日志

**HDR 元数据**

- `hdr2sdr`：更新/降级 Mastering、Content Light（对齐 OpenCL/CPU）
- `sdr2hdr`：按 `peak` 写入合理 Mastering/CLL；缺失时默认峰值 1000 nits

**CPU `param` 默认规则（保持一致）**

- `gamma`：未设 `param` → `1.8`
- `reinhard`：输入参数转换为 `(1 - param) / param`
- `mobius`：未设 `param` → `0.3`
- 其余未设 → `1.0`

### crop_cuda（对齐 `vf_crop`）

| 选项 | 默认 |
|------|------|
| `out_w` / `w` | `iw` |
| `out_h` / `h` | `ih` |
| `x` | `(in_w-out_w)/2` |
| `y` | `(in_h-out_h)/2` |
| `keep_aspect` | `0` |
| `exact` | `0` |

表达式变量：`iw` `ih` `ow` `oh` `a` `sar` `dar` `hsub` `vsub` `x` `y` `n` `t`。  
支持 `process_command` 动态修改 `w/h/x/y`。  
`exact=0` 时按 NV12/P010 色度子采样对齐。

## 3. 设备端算法与错误处理

### tonemap_cuda kernels

建议入口：

- `tonemap_hdr2sdr_nv12` / `tonemap_hdr2sdr_p010`
- `tonemap_sdr2hdr_nv12` / `tonemap_sdr2hdr_p010`

单像素路径：

1. 读 Y/UV → 归一化 → YUV→RGB（输入 matrix）
2. 逆光电转换：HDR 用 PQ/HLG EOTF；SDR 用 BT.709/sRGB OETF 逆
3. （可选）primaries 转到工作色域（通常 BT.2020 linear）
4. 应用 tonemap / inverse（`peak`、`param`、`desat`）
5. 输出 transfer 编码 + 输出 matrix → 写回 Y/UV
6. `range` 做 limited/full 映射

实现要点：

- 正向曲线对齐 `vf_tonemap.c` / `opencl/tonemap.cl`
- `sdr2hdr`：`linear` 按 peak 缩放；`hable`/`reinhard` 用解析或 Newton 近似逆（固定迭代上限）；`gamma` 用解析逆
- `clip`/`mobius` 不进 kernel，宿主在配置/开跑前失败
- 常量经 kernel 参数或小常量缓冲下发，避免每帧重编 PTX

### crop_cuda kernels

- 每平面：`dst(x,y) = src(x+ox, y+oy)`；UV 按 `hsub/vsub` 缩放 ROI
- 参考 `pad_cuda` 的 pitch/`CUdeviceptr`，纯 copy（不使用 texture）
- 宿主夹紧 `x/y`，保证 ROI 完全在帧内（对齐 CPU `crop`）

### 错误处理

| 场景 | 行为 |
|------|------|
| 无 `hw_frames_ctx` / 非 CUDA | `AVERROR(EINVAL)` |
| `sw_format` 非 nv12/p010 | `AVERROR(ENOTSUP)` |
| `mode=auto` 无法推断 | `AVERROR(EINVAL)`，提示显式 `mode` |
| `sdr2hdr` + `clip`/`mobius`/不可逆 `gamma` | `AVERROR(EINVAL)` + 算法名 |
| 显式 `hdr2sdr` 但输入已是 SDR | warning，仍按配置执行 |
| crop 表达式非法 / 尺寸 ≤ 0 | `AVERROR(EINVAL)` |
| CUDA API 失败 | `FF_CUDA_CHECK_DL` → 负错误码 |
| PTX/kernel 缺失 | init/config 失败 |

## 4. 测试计划与成功标准

### 功能

- `ffmpeg -filters` 列出 `tonemap_cuda`、`crop_cuda`
- HDR10(P010) → `mode=hdr2sdr:tonemap=hable:format=nv12`：亮度合理，标签 BT.709
- SDR(NV12) → `mode=sdr2hdr:tonemap=hable:t=smpte2084:format=p010`：P010 + PQ + Mastering/CLL
- `mode=auto` 对 PQ/HLG 与 BT.709 方向正确
- `tonemap=clip|mobius` + `sdr2hdr` → 明确失败
- `crop_cuda=iw/2:ih/2` 尺寸减半；`x/y` 与 `n`/`t` 动态裁剪
- `process_command` 改 crop 参数生效
- 与 `scale_cuda` / NVENC 串联无强制下载上传

### 回归对照

- 同参数与 `tonemap_opencl`（若可用）或 CPU `tonemap` + `hwupload_cuda` 做 PSNR/视觉抽样（允许 GPU 浮点差）
- `crop_cuda` 与 CPU `crop` + 重新 upload 的 ROI 像素一致（对齐后）

### 负面用例

- 非 CUDA 帧、不支持格式、非法表达式、缺 GPU/PTX

### 成功标准

- 双向 tonemap 与物理 crop 均在 GPU 完成
- API/错误行为符合第 2、3 节
- 在启用 `cuda_llvm` 或 `cuda_nvcc` 的本仓构建中可编入发行包

## 非目标（首版不做）

- 公共 CUDA 滤镜底座重构
- 扩展 `scale_cuda`/`colorspace_cuda` 选项替代独立滤镜
- 超出 `nv12`/`p010` 的格式
- 仅元数据裁剪的 `crop_cuda` 零拷贝模式
- VAAPI 风格无算法选项的驱动 tonemap

## 参考实现

- 宿主/PTX：`vf_colorspace_cuda.c`、`vf_pad_cuda.c`、`vf_scale_cuda.c`
- 算法/API：`vf_tonemap.c`、`vf_tonemap_opencl.c`、`opencl/tonemap.cl`
- 裁剪语义：`vf_crop.c`
- PTX 加载：`cuda/load_helper.c` → `ff_cuda_load_module`
- 构建配方：`utils/scripts/50-ffnvcodec.sh`
