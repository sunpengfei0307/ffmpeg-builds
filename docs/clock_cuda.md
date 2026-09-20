# clock_cuda

CUDA 页面时钟源（类 `color_cuda`）。输出 `AV_PIX_FMT_CUDA` 的 nv12/p010，可直接作 `mixing_cuda` 的 `bg_input`，或单独编码成时钟节目。

## 变更记录


| 日期         | 说明                                                                                  |
| ---------- | ----------------------------------------------------------------------------------- |
| 2026-09-20 | `font`/`fontweight`/`fontsize`/`datesize`；默认 regular；等宽 + PTS 走秒 |
| 2026-09-18 | 初版：`size`/`rate`/`layout`；数字钟默认含日期+星期+ISO 周+秒；`layout=time` 仅时间；`layout=analog` 指针钟 |


## 设计方案（核心功能）

1. **CUDA 源**：与 `color_cuda` 相同的 hwframes / 出帧模型，数字钟按秒复用 GPU 帧，指针钟每帧重绘
2. `format=nv12|p010`：与 mixing / NVENC 对齐
3. **布局**：
  - `digital`（默认）：翻页卡片数字钟 + 日期「2026年9月18日 星期五」
  - `time`：只显示大号时间，不显示日期和周数
  - `analog`：指针钟；默认日期行在表盘下方


| 文件                         | 职责                   |
| -------------------------- | -------------------- |
| `vsrc_clock_cuda.c`        | 选项、日期排版、hwframes     |
| `vsrc_clock_cuda.cu`       | 底色填充、字形 blit、指针钟 SDF |
| `vsrc_clock_cuda_glyphs.h` | 预烘焙数字 + 中文日期字形图集     |


构建：`CONFIG_CLOCK_CUDA_FILTER`。字形预烘焙 `segoe` / `calibri` / `mono` 四种粗细；`0-9` 等宽格。

## 参数说明


| 选项                                       | 默认             | 常用建议                          | 作用         |
| ---------------------------------------- | -------------- | ----------------------------- | ---------- |
| `size`/`s`                               | `1920x1080`    | 与 mixing 同画幅                  | 输出尺寸       |
| `rate`/`r`                               | `25`           | **与业务同 fps**                  | 节目时钟帧率     |
| `re`                                     | `1`            | 直播 `1`；尽快出帧 `0`              | 按 `r` 实时节流   |
| `layout`                                 | `digital`      | `digital` / `time` / `analog` | 页面布局       |
| `show_date`                              | `1`            | `0` 关闭日期                      | 中文年月日      |
| `show_week`                              | `0`            | `1` 追加「第N周」                   | ISO 周       |
| `show_seconds`                           | `1`            | `0` 则 `HH:MM` / 无秒针           | 秒          |
| `font` / `fontstyle`                     | `segoe`        | `segoe` / `calibri` / `mono`  | 时分秒字体风格    |
| `fontweight` / `weight`                  | `regular`      | `light` / `regular` / `medium` / `bold` | 时分秒粗细 |
| `fontsize`                               | `0`（约画面高 30%） | 如 `280`、`320`                 | 时分秒像素高度    |
| `datesize`                               | `0`（自动）        | 如 `48`                         | 日期行像素高度    |
| `color`/`c`                              | `white`        | 浅灰/白                          | 前景         |
| `bgcolor`/`bg`                           | `black`        | 会议底色                          | 背景         |
| `format`                                 | `nv12`         | SDR=`nv12`；HDR=`p010`         | sw_format  |
| `duration`/`d`                           | `-1`           | 直播保持默认                        | 时长         |
| `device`                                 | `0`            | 无全局 hwdevice 时用               | CUDA 设备    |
| `colorspace` / `color_trc` / `out_range` | 同 `color_cuda` | SDR bt709 tv                  | 元数据        |


`layout=time` 会忽略 `show_date`/`show_week`（始终不画日期行）。

## 用法示例

```text
# 默认：segoe + regular，字号自动
clock_cuda=s=1920x1080:r=25

# 细字 / 中等 / 粗体
clock_cuda=s=1920x1080:r=25:font=segoe:fontweight=light
clock_cuda=s=1920x1080:r=25:font=segoe:fontweight=medium:fontsize=280
clock_cuda=s=1920x1080:r=25:font=calibri:fontweight=bold:fontsize=320

# 等宽数字
clock_cuda=s=1920x1080:r=25:font=mono:fontweight=regular

# 只显示时间
clock_cuda=s=1920x1080:r=25:layout=time

# 指针钟（带日期）
clock_cuda=s=1920x1080:r=25:layout=analog

# 合屏底板
clock_cuda=s=1920x1080:r=25:format=nv12[bg];
[bg][v0][v1]...mixing_cuda=inputs=5:bg_input=0:...

# HDR
clock_cuda=s=1920x1080:r=25:format=p010:colorspace=bt2020nc:color_trc=smpte2084
```

推流测试（对齐现有 `hevc_nvenc` + `libfdk_aac` + `live_flv` 命令，输入改为 `clock_cuda`）：

```bash
# 默认 digital：日期+星期+周+HH:MM:SS
./ffmpeg -init_hw_device cuda=hw:0 -filter_hw_device hw \
  -abnormal_timeout 30 \
  -filter_complex "clock_cuda=s=1920x1080:r=25:format=nv12:layout=digital,setdar=dar=a[vout0];anullsrc=r=48000:cl=stereo,volume=1.0,aresample=osr=48000,asplit=1[ain0]" \
  -map '[vout0]' -map '[ain0]' \
  -vcodec hevc_nvenc -acodec libfdk_aac -b:v 10000000 -bf 2 -r 25 -g 50.0 \
  -level 0.0 -profile:v main10 -gpu 0 -spatial_aq 1 -temporal_aq 1 -b_scale_ratio 1.4 \
  -ab 128000 -ac 2 -f flv "rtmp://test-push.live.qiyi.domain/live/ls_color_1080p25"

# 只显示时间：layout=time
# 指针钟：layout=analog
```

运行时可通过 zmq/`process_command` 改 `layout` / `font` / `fontweight` / `fontsize` / `datesize` / `color` / `bgcolor`。

## 注意事项

- `s`/`r`/`format` 须与 mixing 及业务 NVDEC 一致
- 日期为机器本地时区；周数为 ISO-8601（周一起、含 1 月 4 日的为第 1 周）
- 字形图集仅覆盖数字与日期行常用汉字（年月日星期一二三四五六第周、）
