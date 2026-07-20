#!/bin/bash
# Build the optional TensorRT plugin for detect_cuda.
#
# Output: libavfilter_detect_cuda_trt.so
# Runtime: place next to ffmpeg, under /usr/local/lib, or set
#          FFMPEG_DETECT_CUDA_TRT_PATH to the file or directory.
#
# Env:
#   FFMPEG_SRC        ffmpeg source tree (default: ffmpeg/ffmpeg-8.1.2)
#   FFBUILD_PREFIX    ffnvcodec prefix (default: build/release)
#   TRT_ROOT          TensorRT prefix (include/ + lib|lib64/)
#   TRT_INCLUDE       override: directory containing NvInfer.h
#   TRT_LIB           override: directory containing libnvinfer.so*
#   CUDA_ROOT         CUDA toolkit (default: /usr/local/cuda)
#   OUT_DIR           build output dir (default: $FFBUILD_PREFIX/lib)
#   CXX               C++ compiler (default: g++)
#
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
FFMPEG_SRC="${FFMPEG_SRC:-${FFMPEG_DIR:-${ROOT_DIR}/ffmpeg/ffmpeg-8.1.2}}"
FFBUILD_PREFIX="${FFBUILD_PREFIX:-${USR_DIR:-${ROOT_DIR}/build/release}}"
CUDA_ROOT="${CUDA_ROOT:-/usr/local/cuda}"
TRT_ROOT="${TRT_ROOT:-/usr/local/tensorrt}"
# Default: install into the same prefix as ffmpeg (build/release/lib)
OUT_DIR="${OUT_DIR:-${FFBUILD_PREFIX}/lib}"
INSTALL_DIR="${FFBUILD_PREFIX}/lib"
CXX="${CXX:-g++}"

SRC="${FFMPEG_SRC}/libavfilter/vf_detect_cuda_trt.cpp"
HDR_DIR="${FFMPEG_SRC}/libavfilter"
PLUGIN_NAME="libavfilter_detect_cuda_trt.so"
OUT_LIB="${OUT_DIR}/${PLUGIN_NAME}"
INSTALL_LIB="${INSTALL_DIR}/${PLUGIN_NAME}"

if [[ ! -f "${SRC}" ]]; then
  local_fallback="${ROOT_DIR}/ffmpeg/ffmpeg-8.1.2/libavfilter/vf_detect_cuda_trt.cpp"
  if [[ -f "${local_fallback}" ]]; then
    FFMPEG_SRC="${ROOT_DIR}/ffmpeg/ffmpeg-8.1.2"
    SRC="${FFMPEG_SRC}/libavfilter/vf_detect_cuda_trt.cpp"
    HDR_DIR="${FFMPEG_SRC}/libavfilter"
    echo "warning: FFMPEG_SRC corrected to ${FFMPEG_SRC}" >&2
  else
    echo "error: missing ${SRC}" >&2
    exit 1
  fi
fi

# ---- ffnvcodec ----
export PKG_CONFIG_PATH="${FFBUILD_PREFIX}/lib/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
FFNV_CFLAGS=""
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists ffnvcodec; then
  FFNV_CFLAGS="$(pkg-config --cflags ffnvcodec)"
elif [[ -f "${FFBUILD_PREFIX}/include/ffnvcodec/dynlink_loader.h" ]]; then
  FFNV_CFLAGS="-I${FFBUILD_PREFIX}/include"
else
  echo "error: ffnvcodec headers not found under ${FFBUILD_PREFIX}/include/ffnvcodec/" >&2
  exit 1
fi

# ---- TensorRT discovery ----
# Prefer explicit TRT_INCLUDE/TRT_LIB; else TRT_ROOT; else common prefixes.
find_trt_include() {
  local d
  for d in "$@"; do
    [[ -n "$d" && -f "${d}/NvInfer.h" ]] && { echo "$d"; return 0; }
  done
  return 1
}

find_trt_lib() {
  local d
  for d in "$@"; do
    [[ -z "$d" || ! -d "$d" ]] && continue
    if compgen -G "${d}/libnvinfer.so*" >/dev/null 2>&1 || \
       compgen -G "${d}/libnvinfer.so" >/dev/null 2>&1; then
      echo "$d"
      return 0
    fi
  done
  return 1
}

TRT_CANDIDATES=()
[[ -n "${TRT_ROOT:-}" ]] && TRT_CANDIDATES+=("${TRT_ROOT}")
TRT_CANDIDATES+=(
  /usr/local/tensorrt
  /usr/local/TensorRT
  /opt/tensorrt
  /usr
  "${CUDA_ROOT}"
)
# Versioned installs: /usr/local/TensorRT-10.* etc.
shopt -s nullglob
for d in /usr/local/TensorRT-* /opt/TensorRT-* /usr/local/tensorrt-*; do
  TRT_CANDIDATES+=("$d")
done
shopt -u nullglob

if [[ -z "${TRT_INCLUDE:-}" ]]; then
  TRT_INCLUDE="$(find_trt_include \
    ${TRT_ROOT:+"${TRT_ROOT}/include"} \
    "${TRT_CANDIDATES[@]/%//include}" \
    /usr/include \
    /usr/include/x86_64-linux-gnu \
    || true)"
fi

if [[ -z "${TRT_LIB:-}" ]]; then
  TRT_LIB="$(find_trt_lib \
    ${TRT_ROOT:+"${TRT_ROOT}/lib"} \
    ${TRT_ROOT:+"${TRT_ROOT}/lib64"} \
    "${TRT_CANDIDATES[@]/%//lib}" \
    "${TRT_CANDIDATES[@]/%//lib64}" \
    /usr/lib \
    /usr/lib64 \
    /usr/lib/x86_64-linux-gnu \
    || true)"
fi

# Infer TRT_ROOT for rpath / logging when only include/lib were found
if [[ -z "${TRT_ROOT:-}" && -n "${TRT_INCLUDE:-}" ]]; then
  case "${TRT_INCLUDE}" in
    */include) TRT_ROOT="${TRT_INCLUDE%/include}" ;;
    *) TRT_ROOT="${TRT_INCLUDE}" ;;
  esac
fi
TRT_ROOT="${TRT_ROOT:-/usr/local/tensorrt}"

if [[ -z "${TRT_INCLUDE:-}" || ! -f "${TRT_INCLUDE}/NvInfer.h" ]]; then
  cat >&2 <<EOF
error: TensorRT headers not found (NvInfer.h).

Default /usr/local/tensorrt/include is missing on this machine.
Set one of:
  export TRT_ROOT=/path/to/TensorRT          # expect \$TRT_ROOT/include/NvInfer.h
  export TRT_INCLUDE=/path/to/dir            # directory that contains NvInfer.h
  export TRT_LIB=/path/to/lib                # directory that contains libnvinfer.so

Quick locate:
  find /usr /opt /usr/local -name NvInfer.h 2>/dev/null | head
EOF
  exit 1
fi

if [[ -z "${TRT_LIB:-}" ]]; then
  cat >&2 <<EOF
error: TensorRT libraries not found (libnvinfer.so*).
  export TRT_LIB=/path/to/lib
  or: find /usr /opt /usr/local -name 'libnvinfer.so*' 2>/dev/null | head
EOF
  exit 1
fi

if [[ ! -f "${CUDA_ROOT}/include/cuda_runtime_api.h" ]]; then
  echo "error: CUDA headers not found under ${CUDA_ROOT}/include" >&2
  echo "  export CUDA_ROOT=/usr/local/cuda-<ver>" >&2
  exit 1
fi

mkdir -p "${OUT_DIR}" "${INSTALL_DIR}"

echo "Building ${OUT_LIB}"
echo "  FFMPEG_SRC=${FFMPEG_SRC}"
echo "  FFBUILD_PREFIX=${FFBUILD_PREFIX}"
echo "  INSTALL_DIR=${INSTALL_DIR}"
echo "  FFNV_CFLAGS=${FFNV_CFLAGS}"
echo "  TRT_ROOT=${TRT_ROOT}"
echo "  TRT_INCLUDE=${TRT_INCLUDE}"
echo "  TRT_LIB=${TRT_LIB}"
echo "  CUDA_ROOT=${CUDA_ROOT}"

# shellcheck disable=SC2086
"${CXX}" -std=c++14 -fPIC -shared -O2 \
  -Wno-deprecated-declarations \
  -I"${HDR_DIR}" \
  -I"${FFMPEG_SRC}" \
  ${FFNV_CFLAGS} \
  -I"${TRT_INCLUDE}" \
  -I"${CUDA_ROOT}/include" \
  "${SRC}" \
  -L"${TRT_LIB}" \
  -L"${CUDA_ROOT}/lib64" -L"${CUDA_ROOT}/lib" \
  -lnvinfer -lnvonnxparser -lcudart \
  -Wl,-rpath,"${TRT_LIB}:${CUDA_ROOT}/lib64" \
  -o "${OUT_LIB}"

# Always install into FFBUILD_PREFIX/lib (even if OUT_DIR was overridden)
out_abs="$(cd "$(dirname "${OUT_LIB}")" && pwd)/$(basename "${OUT_LIB}")"
inst_abs="$(cd "${INSTALL_DIR}" && pwd)/${PLUGIN_NAME}"
if [[ "${out_abs}" != "${inst_abs}" ]]; then
  cp -f "${OUT_LIB}" "${INSTALL_LIB}"
  echo "Installed: ${INSTALL_LIB}"
else
  echo "Installed: ${OUT_LIB}"
fi

echo "OK: ${INSTALL_LIB}"
echo "Runtime: place next to ffmpeg, or export FFMPEG_DETECT_CUDA_TRT_PATH=${INSTALL_LIB}"
