#!/bin/bash

SCRIPT_REPO="https://github.com/FFmpeg/nv-codec-headers.git"
SCRIPT_COMMIT="15ee32753c92faddbabbff11676779618fc6db7e"

# n12.2.72.0：匹配驱动 550 / NVENC API 12.2（CUDA 12.4 常见环境）
# sdk/13.0 要求驱动 ≥570，运行时报 Required: 13.0 Found: 12.2
SCRIPT_REPO2="https://github.com/FFmpeg/nv-codec-headers.git"
SCRIPT_COMMIT2="c69278340ab1d5559c7d7bf0edf615dc33ddbba7"
SCRIPT_BRANCH2="n12.2.72.0"

SCRIPT_REPO3="https://github.com/FFmpeg/nv-codec-headers.git"
SCRIPT_COMMIT3="afae1834257b919848c5deb21a17c7355616b1ee"
SCRIPT_BRANCH3="sdk/11.1"

# 可选：NVENC_SDK=13.0 强制用最新头（Blackwell 硬件平台，需驱动 ≥570）
#        NVENC_SDK=12.4 强制用 12.4（默认，适配 Turing 及更低硬件平台）
#        NVENC_SDK=11.1 强制用 11.1（Pascal 及更低硬件平台）
ffbuild_enabled() {
    [[ $TARGET == winarm64 ]] && (( $(ffbuild_ffver) <= 801 )) && return -1
    (( $(ffbuild_ffver) >= 404 )) || return -1
    return 0
}

ffbuild_dl() {
    default_dl ffnvcodec
    echo "git-mini-clone \"$SCRIPT_REPO2\" \"$SCRIPT_COMMIT2\" ffnvcodec2"
    echo "git-mini-clone \"$SCRIPT_REPO3\" \"$SCRIPT_COMMIT3\" ffnvcodec3"
}

ffbuild_build() {
    local sdk="${NVENC_SDK:-}"
    case "$sdk" in
        13.0|13) cd ffnvcodec ;;
        12.4|12) cd ffnvcodec2 ;;
        11.1|11) cd ffnvcodec3 ;;
        "")
            # FFmpeg 8.x 默认 12.2，兼容 Driver 550；更新需 NVENC_SDK=13.0
            if (( $FFVER < 800 )); then
                cd ffnvcodec3
            else
                cd ffnvcodec2
            fi
            ;;
        *)
            echo "[ERROR] 未知 NVENC_SDK=$sdk（可用: 11.1 / 12.2 / 13.0）" >&2
            return 1
            ;;
    esac

    make PREFIX="$FFBUILD_PREFIX" DESTDIR="$FFBUILD_DESTDIR" install
}

# cuda_llvm needs clang (as nvcc). Host-gcc EL8 builds often lack it;
# ffnvcodec headers alone still enable NVENC/NVDEC via --enable-ffnvcodec.
_ffnvcodec_have_cuda_llvm() {
    command -v clang >/dev/null 2>&1 || return 1
    # Minimal probe: clang must accept --cuda-gpu-arch (CUDA toolchain / clang CUDA)
    clang --help 2>&1 | grep -q -- '--cuda-gpu-arch' || return 1
    return 0
}

ffbuild_configure() {
    if _ffnvcodec_have_cuda_llvm; then
        echo --enable-ffnvcodec --enable-cuda-llvm
    else
        echo "[INFO] ffnvcodec: no usable clang for cuda_llvm; enable ffnvcodec only" >&2
        echo --enable-ffnvcodec --disable-cuda-llvm
    fi
}

ffbuild_unconfigure() {
    echo --disable-ffnvcodec --disable-cuda-llvm
}

ffbuild_cflags() {
    return 0
}

ffbuild_ldflags() {
    return 0
}
