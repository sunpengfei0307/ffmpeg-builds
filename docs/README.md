# ffmpeg-builds（自包含本地构建）

本目录是可独立编译的 FFmpeg 构建树本地实现。

## 文档索引（快速跳转）

每个滤镜 **只保留一个** `docs/<name>.md`（含设计、变更记录、参数、示例、注意事项、踩坑）。

| 滤镜 / 模块 | 文档 |
|-------------|------|
| `mixing_cuda`（会议合屏整链路） | [mixing_cuda.md](./mixing_cuda.md) |
| 合屏导播控制台（浏览器） | [mix_console.html](./mix_console.html) |
| `dynamic_input` | [dynamic_input.md](./dynamic_input.md) |
| `color_cuda` | [color_cuda.md](./color_cuda.md) |
| `amixrank` | [amixrank.md](./amixrank.md) |
| `eq_cuda` | [eq_cuda.md](./eq_cuda.md) |
| `curves_cuda` | [curves_cuda.md](./curves_cuda.md) |
| `pad_cuda` | [pad_cuda.md](./pad_cuda.md) |
| `detect_cuda` | [detect_cuda.md](./detect_cuda.md) |
| `tonemap_cuda` | [tonemap_cuda.md](./tonemap_cuda.md) |
| `hlg2pq_cuda` | [hlg2pq_cuda.md](./hlg2pq_cuda.md) |
| libplacebo HDR→SDR（`scale_cuda` + Vulkan `hwupload`） | [libplacebo.md](./libplacebo.md) |
| 进程级 `-zmq`（滤镜增删改查、nvenc 码率、concat 切源、ffmpeg 退出） | [zmq.md](./zmq.md) |
| 进程内 HTTP 拉流（`-http_server`：HLS / DASH / `/{app}/{stream}.flv\|ts\|mp4` 内存 GOP） | [http_server.md](./http_server.md) |
| `-report` 写启动日志文件（前缀-日期时间.log） | [report.md](./report.md) |

合屏设计规格（可选深入）：[superpowers/specs/](./superpowers/specs/)


## 布局

```
build.sh
utils/
  logger.sh          # 日志
  stages.sh          # 全部编排（路径/本机依赖/工具链/下载/编依赖/编 FFmpeg）
  scripts/           # 第三方库配方
  downloads/         # 源码包缓存（可缺，构建时补下）
  host-bin/          # git-mini-clone 等（复制到 build/tools/bin，不外链工程）
ffmpeg/ffmpeg-<ver>/ # FFmpeg 源码，ver=版本参数（默认 8.1.2）；已有 configure 则不覆盖
build/
  stages/            # *.done
  caches/            # 编译工作目录
  release/           # 安装前缀（原 usr/）
  tools/{bin,opt}/   # 本机工具（PATH 优先）
  build.log
```

## 安装到正式目录

```bash
cd /data/sunpf/ffmpeg-builds
./build.sh linux64 nonfree 8.1.2
# 手动构建：PATH="/data/sunpf/ffmpeg-builds/build/tools/bin:$PATH" make -j"$(nproc)"
# 另一补丁版本会用独立目录并重新下载：
# ./build.sh linux64 nonfree 8.1.2   → ffmpeg/ffmpeg-8.1.2 + tag n8.1.2
```

## 版本与源码目录

| 参数（ADDINS） | 目录 | 缺失时下载 |
|----------------|------|------------|
| `8.1.2`（默认） | `ffmpeg/ffmpeg-8.1.2` | `git clone --branch n8.1.2` |

是否“已存在”：只看对应目录下是否有 `configure`；**不会**用 8.1.2。

## 常用环境变量

| 变量 | 含义 |
|------|------|
| `REUSE=1` | 默认，跳过已 `.done` 的依赖 |
| `FORCE_REBUILD=1` | 强制重编依赖（并禁用自动 FFMPEG_ONLY） |
| `FFMPEG_ONLY=1` | 强制只编 FFmpeg；`=0` 强制全量 |
| `EXTRA_FF_CONFIGURE=...` | 追加到最终 `./configure` 的参数 |
| `FF_CONFIGURE=...` | 覆盖基底 configure 标志（scripts 仍会再追加 `--enable-lib*`） |
| `VERBOSE=1` | 控制台同步详细日志 |

### 自动模式（默认）

直接 `./build.sh` 时会检测 `build/release`：

- **完备**（关键 `.pc` / 库齐全）→ 自动 `FFMPEG_ONLY=1`，只编 FFmpeg
- **未完备** → 走默认全量（本机依赖 → 工具链 → 下载 → 编库 → FFmpeg）

强制行为：

```bash
./build.sh                          # 自动判断
FFMPEG_ONLY=0 ./build.sh            # 即使 release 已齐也全量
FORCE_REBUILD=1 ./build.sh          # 强制重编依赖 + FFmpeg
FFMPEG_ONLY=1 ./build.sh            # 显式只编 FFmpeg
```

## 引入额外库 / 修改 configure

### 方式 A：临时追加（不改仓库）

```bash
EXTRA_FF_CONFIGURE='--enable-libfoo --disable-doc' FFMPEG_ONLY=1 ./build.sh
```

库本身需已安装到 `build/release`（含 `.pc` / `.a`），且 FFmpeg 源码支持对应 `--enable-*`。

### 方式 B：持久接入新依赖（推荐）

1. 在 `utils/scripts/` 新增配方，例如 `50-mylib.sh`（可参考 `50-libopus.sh`）：

```bash
#!/bin/bash
SCRIPT_REPO="https://example.com/mylib.git"
SCRIPT_COMMIT="<commit>"

ffbuild_enabled() { return 0; }

ffbuild_build() {
    # 编进 $FFBUILD_PREFIX（即 build/release）
    ./configure --prefix="$FFBUILD_PREFIX" --disable-shared --enable-static
    make -j"$(nproc)"
    make install DESTDIR="$FFBUILD_DESTDIR"
}

ffbuild_configure() {
    echo --enable-libmylib
}
```

2. 在 `utils/scripts/zz-final.sh` 的 `ffbuild_depends` 里加上依赖名（如 `echo mylib`），保证拓扑会编到它。

3. 完整重跑（或删掉对应 `build/stages/*.done` 后）`./build.sh`，再 `FFMPEG_ONLY=1` 迭代 FFmpeg。

`collect_ff_flags` 会扫描所有 `utils/scripts/**/*.sh` 的 `ffbuild_configure`，自动拼进最终 configure。

### 方式 C：只改基底标志

```bash
FF_CONFIGURE='--enable-gpl --enable-version3 --enable-nonfree --disable-debug --disable-network' \
  FFMPEG_ONLY=1 ./build.sh
```

scripts 收集到的 `--enable-lib*` 仍会追加在后面。

## 约束

- `ffmpeg/ffmpeg-<ver>` 中已有 `configure` 则不 clone、不删除、不覆盖（不同 ver 目录互不影响）
