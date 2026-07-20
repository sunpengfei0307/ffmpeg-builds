#!/bin/bash
# 本地自包含 FFmpeg 构建（无 Docker、无 variants/addins、无外部工程软链）
#
# 布局（ROOT_DIR = 本脚本所在目录）:
#   build.sh
#   utils/
#     logger.sh          日志
#     stages.sh          编排（路径 / 本机依赖 / 工具链 / 下载 / stage / FFmpeg）
#     scripts/           依赖构建脚本（原 scripts.d）
#     downloads/         依赖源码缓存
#     tools/          主机辅助工具（复制到 build/tools/bin，不外链）
#   ffmpeg/ffmpeg-<ver>/ FFmpeg 源码（ver=ADDINS，默认 8.1.2；已有则绝不覆盖）
#   build/
#     stages/            *.done 断点标记
#     caches/            stage 解压/编译工作目录
#     release/           安装前缀（bin lib include …）= FFBUILD_PREFIX / USR_DIR
#     tools/bin|opt/     本机工具与可选源码安装
#     build.log          完整日志
#
# 用法:
#   ./build.sh [linux64] [nonfree] [8.1.2]
#
# 环境变量:
#   REUSE=1              默认：复用已有依赖，不重下不重编已完成 stage
#   FORCE_REBUILD=1      强制重编依赖（禁用自动 FFMPEG_ONLY）
#   FFMPEG_ONLY=1        跳过本机依赖/工具链安装/下载/编库，只编 FFmpeg
#                        （未设置时：release 完备则自动开启；FFMPEG_ONLY=0 强制全量）
#   EXTRA_FF_CONFIGURE=  追加到最终 ./configure 的额外参数
#   USE_HOST_TOOLCHAIN=1 系统 gcc（默认）
#   VERBOSE=1            控制台同步详细日志
#   FFBUILD_PYTHON=...   指定主机 Python（默认优先较新版本）
#   FFMPEG_SRC           由本脚本写死为 $FFMPEG_DIR（勿依赖外部环境）
#   SKIP_DETECT_CUDA_TRT=1  跳过 detect_cuda TensorRT 插件编译
#   REQUIRE_DETECT_CUDA_TRT=1  插件编译失败则整次构建失败（默认仅警告）
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
export ROOT_DIR

DEFAULT_TARGET="linux64"
DEFAULT_VARIANT="nonfree"
# 须与目录名 ffmpeg-<ver> 中 - 后字符串一致（8.1.2 / 8.1.3 为不同目录，各自下载）
DEFAULT_ADDINS_STR="8.1.2"

# shellcheck source=/dev/null
source "${ROOT_DIR}/utils/logger.sh"
# shellcheck source=/dev/null
source "${ROOT_DIR}/utils/stages.sh"

print_help() {
  cat <<EOF
用法: $0 [目标] [变体] [版本提示...]

默认: linux64 nonfree 8.1.2

目录布局:
  utils/scripts/     依赖构建脚本
  utils/downloads/   依赖下载缓存
  utils/tools/    主机辅助工具（构建时复制到 build/tools/bin）
  ffmpeg/ffmpeg-<ver>/  FFmpeg 源码（ver=ADDINS，如 8.1.2；已有不覆盖）
  build/release/     安装前缀（FFBUILD_PREFIX / USR_DIR）
  build/stages/      stage 完成标记 (*.done)
  build/caches/      stage 工作目录
  build/tools/       本机工具链包装与可选源码工具
  build/build.log    完整日志

用法说明:
  - 版本参数须写全三位数（8.1.2 / 8.1.3），对应不同 ffmpeg/ffmpeg-<ver> 目录，各自独立下载
  - 检测：仅当 ffmpeg/ffmpeg-<ver>/configure 存在才视为已有源码，否则 git clone tag n<ver>
  - 启动时检测 build/release：完备则自动 FFMPEG_ONLY=1，否则全量编译

环境变量:
  REUSE=1              复用已编译依赖（默认）
  FORCE_REBUILD=1      强制重编依赖（禁用自动 FFMPEG_ONLY）
  FFMPEG_ONLY=1        强制只编 FFmpeg；=0 强制全量（即使 release 已齐）
  EXTRA_FF_CONFIGURE=  追加 FFmpeg configure 参数（例: --enable-libfoo）
  USE_HOST_TOOLCHAIN=1 系统 gcc（默认）
  VERBOSE=1            控制台详细输出
  FFBUILD_PYTHON=...   指定主机 Python
  SKIP_DETECT_CUDA_TRT=1       跳过 utils/build-detect-cuda-trt.sh
  REQUIRE_DETECT_CUDA_TRT=1    TensorRT 插件失败则构建失败（默认仅警告）

导入脚本:
  1) 持久接入: 在 utils/scripts/ 新增 NN-name.sh（实现 ffbuild_build + ffbuild_configure），并在 utils/scripts/zz-final.sh 的 ffbuild_depends 中声明
  2) 临时追加: EXTRA_FF_CONFIGURE='--enable-xxx --disable-yyy' ./build.sh
  3) 覆盖基底: FF_CONFIGURE='--enable-gpl ...' ./build.sh
EOF
}

parse_args() {
  if [[ $# -ge 1 && ( $1 == "-h" || $1 == "--help" ) ]]; then
    print_help
    exit 0
  fi
  TARGET="${1:-$DEFAULT_TARGET}"
  VARIANT="${2:-$DEFAULT_VARIANT}"
  if [[ $# -ge 3 ]]; then
    ADDINS_STR="${*:3}"
    ADDINS_STR="${ADDINS_STR// /-}"
  else
    ADDINS_STR="$DEFAULT_ADDINS_STR"
  fi
  if [[ "$VARIANT" == "nofree" ]]; then
    VARIANT="nonfree"
  fi
  if [[ "$TARGET" != "linux64" ]]; then
    echo "仅支持 linux64，收到: $TARGET" >&2
    exit 1
  fi
  export TARGET VARIANT ADDINS_STR
  export ADDINS="$ADDINS_STR"
  export REUSE="${REUSE:-1}"
  export USE_HOST_TOOLCHAIN="${USE_HOST_TOOLCHAIN:-1}"
  export TAR_OPTIONS="--no-xattrs --no-acls --no-selinux"

  # Base FFmpeg configure flags (scripts append --enable-lib* via collect_ff_flags)
  export FF_CONFIGURE="${FF_CONFIGURE:---enable-gpl --enable-version3 --enable-nonfree --disable-debug}"
}

# release 完备 → 自动 FFMPEG_ONLY；显式 FFMPEG_ONLY=0 / FORCE_REBUILD=1 则全量
_auto_set_ff_only() {
  local mode="${FFMPEG_ONLY-}"
  if [[ -n "${FORCE_REBUILD:-}" ]]; then
    unset FFMPEG_ONLY SKIP_DEPS || true
    _info "FORCE_REBUILD=1：全量编译依赖 + FFmpeg"
    return 0
  fi
  if [[ "$mode" == "0" ]]; then
    unset FFMPEG_ONLY SKIP_DEPS || true
    _info "FFMPEG_ONLY=0：强制全量编译"
    return 0
  fi
  if [[ -n "$mode" && "$mode" != "0" ]]; then
    FFMPEG_ONLY=1
    SKIP_DEPS=1
    export FFMPEG_ONLY SKIP_DEPS
    _info "FFMPEG_ONLY=1：只编 FFmpeg"
    return 0
  fi
  # 未显式设置：按 release 是否完备自动决定
  if _deps_ready; then
    local pc
    pc="$(_pc_count "${USR_DIR}/lib/pkgconfig")"
    FFMPEG_ONLY=1
    SKIP_DEPS=1
    export FFMPEG_ONLY SKIP_DEPS
    _ok "检测到 build/release 依赖完备（pkgconfig=${pc}），自动设置 FFMPEG_ONLY=1，跳过依赖编译"
  else
    unset FFMPEG_ONLY SKIP_DEPS || true
    local miss
    miss="$(_missing_critical_deps | tr '\n' ' ' | xargs || true)"
    _info "build/release 未完备${miss:+（缺: $miss）}，走默认全量编译"
  fi
}

main() {
  parse_args "$@"
  _log_init "$@"
  _paths_init
  _init_ff_vars
  # TRT 插件等脚本用；写死为本仓库 FFmpeg 源码树，避免外部环境指到错误路径
  export FFMPEG_SRC="$FFMPEG_DIR"
  # 必须尽早安装，否则依赖阶段 tee|while 会吞掉 Ctrl+C
  _ff_interrupt_setup
  export WORK_DIR TARGET VARIANT ADDINS_STR OUTPUT_DIR="$USR_DIR"

  _auto_set_ff_only

  _console "构建: TARGET=$TARGET VARIANT=$VARIANT ADDINS_STR=$ADDINS_STR FFVER=$FFVER"
  _console "源码: $FFMPEG_DIR"
  _console "日志: $BUILD_LOG"
  _console "前缀: $FFBUILD_PREFIX"
  _console "复用: REUSE=$REUSE  FORCE_REBUILD=${FORCE_REBUILD:-0}  FFMPEG_ONLY=${FFMPEG_ONLY:-0}"
  _console "提示: Ctrl+C 可中断并终止子进程"

  if [[ -n "${FFMPEG_ONLY:-}" ]]; then
    _banner "[FFMPEG_ONLY] 跳过 系统库 + 工具链 + 下载（依赖） + 编译（依赖）"
    stage_ffmpeg_only_prep || { _fail "FFMPEG_ONLY 环境准备"; exit 1; }
    _banner "编译 FFmpeg → build/release"
    stage_build_ffmpeg || { _fail "编译 FFmpeg 阶段"; exit 1; }
  else
    _banner "[1/5] 系统库安装"
    stage_host_deps || { _fail "系统库安装阶段"; exit 1; }

    _banner "[2/5] 准备工具链"
    stage_toolchain || { _fail "准备工具链阶段"; exit 1; }

    _banner "[3/5] 下载依赖库"
    stage_fetch_deps || { _fail "下载依赖库阶段"; exit 1; }

    _banner "[4/5] 编译依赖库 → build/release"
    stage_build_deps || { _fail "编译依赖库阶段"; exit 1; }

    _banner "[5/5] 编译 FFmpeg → build/release"
    stage_build_ffmpeg || { _fail "编译 FFmpeg 阶段"; exit 1; }
  fi

  # detect_cuda TensorRT 插件（输出到 $FFBUILD_PREFIX/lib）
  # 默认插件失败仅警告；REQUIRE_DETECT_CUDA_TRT=1 时失败则退出
  stage_build_detect_cuda_trt || { _fail "detect_cuda TRT 插件阶段"; exit 1; }

  _banner "完成"

  local pc
  pc="$(_pc_count "${USR_DIR}/lib/pkgconfig")"
  _ok "build/release  pkgconfig=${pc}"
  _ok "ffmpeg: ${USR_DIR}/bin/ffmpeg"
  if [[ -f "${USR_DIR}/lib/libavfilter_detect_cuda_trt.so" ]]; then
    _ok "detect_cuda TRT: ${USR_DIR}/lib/libavfilter_detect_cuda_trt.so"
  fi
  _info "详细日志: $BUILD_LOG"
  _log "===== build end $(date -Is) ====="
}

main "$@"
