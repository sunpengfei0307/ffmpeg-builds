# draw_text_cuda

在 CUDA 帧上叠文字。启动参数给固定文案；进程 `-zmq` 可改文字、字号、字距、字体、加粗/下划线，以及背景框的位置、宽高、颜色、透明度和行数。

## 变更记录


| 日期         | 说明                                                               |
| ---------- | ---------------------------------------------------------------- |
| 2026-10-09 | 未指定 `font` 时使用内置微软雅黑（`msyh.ttc`），不再查找系统 DejaVu/Noto              |
| 2026-10-09 | 补上 `libavutil/mem.h`，避免库内编译把 `av_malloc` 当成隐式声明并以 `-Werror` 失败   |
| 2026-10-08 | 初版：FreeType 光栅化后 GPU 混合；PTS 队列；`timeline` 在文字前加 `[HH:MM:SS.mmm]` |


## 设计方案（核心功能）

1. FreeType 把文字画成 RGBA 小图，再在 GPU 上按 alpha 叠到 YUV / RGB
2. 像素格式与 `scale_cuda` 相同，至少覆盖 `yuv420p` / `nv12` / `p010`
3. 文字按行保留，最多 `lines` 行，超出丢掉最旧的一行
4. PTS 规则（单位是秒）：
  - `pts<=0` 或省略：立刻压入当前字幕
  - 文字 PTS **早于**当前已输出画面：先缓存，下一帧若仍不晚于画面就上屏
  - 文字 PTS **晚于**当前画面：丢弃，不预排未来字幕
5. `timeline=1` 时，在整段文字最前面加上当前帧时间戳，格式 `[01:01:01.532]`（时:分:秒.毫秒，毫秒 3 位）


| 文件                     | 职责                                       |
| ---------------------- | ---------------------------------------- |
| `vf_draw_text_cuda.c`  | 选项、FreeType、PTS 队列、命令                    |
| `vf_draw_text_cuda.cu` | 把 RGBA 叠到 Y/UV 或打包 RGB                   |
| `draw_text_msyh.ttc`   | 内置微软雅黑，由 `vf_draw_text_cuda_msyh.S` 打进滤镜 |


构建：`CONFIG_DRAW_TEXT_CUDA_FILTER`（依赖 CUDA 与 libfreetype）。

未写 `font` 时用内置微软雅黑，不依赖系统字体目录。指定 `font` 则改用该文件。`fontindex` 是字体集里的字面，默认 `0`。

## 参数说明


| 选项                     | 默认          | 常用取值               | 作用                         |
| ---------------------- | ----------- | ------------------ | -------------------------- |
| `text`                 | 空           | 启动文案               | 固定文字，`\n` 换行               |
| `font`                 | 空           | 字体文件路径             | 空则用内置微软雅黑                  |
| `fontindex`            | `0`         | `.ttc` 里的字面        | 字体集索引                      |
| `fontsize`             | `36`        | `48`               | 字号，像素，8–512                |
| `spacing` / `interval` | `0`         | `2`                | 字距，像素，可为负                  |
| `line_spacing`         | `4`         | `8`                | 行距                         |
| `bold`                 | `0`         | `1`                | 合成加粗                       |
| `underline`            | `0`         | `1`                | 下划线                        |
| `fontcolor` / `color`  | `white`     | `white`、`0xFFCC00` | 字色                         |
| `alpha`                | `1`         | `0.9`              | 文字不透明度                     |
| `box`                  | `1`         | `0` 关掉底            | 背景框                        |
| `boxcolor`             | `black@0.6` | `black@0.4`        | 背景色，可带透明度                  |
| `boxalpha`             | `1`         | `0.8`              | 再乘一层背景透明度                  |
| `x` / `y`              | `24`        | 贴左上或右下             | 框的左上角，可为负                  |
| `w` / `h`              | `0`         | `640` / `0`        | 框宽高。`0` 按文字收缩。`w>0` 时按宽度换行 |
| `boxpad`               | `8`         | `12`               | 框内边距                       |
| `lines`                | `1`         | `3`                | 最多保留几行                     |
| `timeline`             | `0`         | `1`                | 文字前加帧时间戳                   |


颜色范围未标明时按电视范围（limited）换算；`color_range=jpeg` 时按全幅。

## 用法示例

固定字幕：

```text
hwupload_cuda,draw_text_cuda=text=直播中:fontsize=48:x=48:y=48
```

打开时间戳烧录：

```text
draw_text_cuda=timeline=1:fontsize=32:x=24:y=24:box=1
```

画面时间 1 小时 1 分 1.532 秒时，文字前是 `[01:01:01.532]`。没有正文时只显示这一段。

运行时（进程 `-zmq`，目标名 `draw_text_cuda`）：

```text
draw_text_cuda text 现在这句立刻上屏
draw_text_cuda text pts=12.5 这句只在画面已经走到 12.5 秒及以后才保留
draw_text_cuda text pts=-1 也是立刻上屏
draw_text_cuda fontsize 64
draw_text_cuda spacing 2
draw_text_cuda bold 1
draw_text_cuda underline 1
draw_text_cuda font /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf
draw_text_cuda x 80
draw_text_cuda y 900
draw_text_cuda w 800
draw_text_cuda h 160
draw_text_cuda box 1
draw_text_cuda boxcolor black@0.5
draw_text_cuda boxalpha 0.8
draw_text_cuda lines 3
draw_text_cuda timeline 1
draw_text_cuda clear
```

`text pts=秒 正文` 里秒是相对流时间戳的秒，不是系统时间。`pts` 晚于当前已经输出的画面会被丢掉。多行用 `\n`。新的一句追加到行尾，超过 `lines` 后丢掉最旧的一行。

## 注意事项

- 必须接在 CUDA 帧后面
- 换 `font` / `fontsize` / `bold` 会重新光栅化；只改 `x`/`y` 不会
- 时间戳来自帧 PTS，不是墙上时钟。PTS 无效时显示 `[00:00:00.000]`
- 彩色 emoji 字体不会按彩字绘制，请用普通轮廓字体
