#!/bin/bash

SCRIPT_REPO="https://github.com/pkuvcl/xavs2.git"
SCRIPT_COMMIT="eae1e8b9d12468059bdd7dee893508e470fa83d8"

ffbuild_enabled() {
    [[ $VARIANT == lgpl* ]] && return -1
    [[ $TARGET == win32 ]] && return -1
    # xavs2 aarch64 support is broken
    [[ $TARGET == *arm64 ]] && return -1
    return 0
}

ffbuild_dl() {
    echo "git clone \"$SCRIPT_REPO\" . && git checkout \"$SCRIPT_COMMIT\""
}

ffbuild_build() {
    : "${FFBUILD_PREFIX:?FFBUILD_PREFIX unset}"
    : "${FFBUILD_DESTDIR:?FFBUILD_DESTDIR unset}"

    cd build/linux

    # FFmpeg links with -pie; xavs2 asm must be PIC or configure reports "xavs2 not found".
    local myconf=(
        --disable-cli
        --enable-static
        --enable-pic
        --disable-avs
        --disable-swscale
        --disable-lavf
        --disable-ffms
        --disable-gpac
        --disable-lsmash
        --extra-asflags="-DPIC -w-macro-params-legacy"
        --extra-cflags="-fPIC -DPIC -Wno-error=incompatible-pointer-types"
        --prefix="$FFBUILD_PREFIX"
    )

    if [[ $TARGET == win* || $TARGET == linux* ]]; then
        myconf+=(
            --host="$FFBUILD_TOOLCHAIN"
            --cross-prefix="$FFBUILD_CROSS_PREFIX"
        )
    else
        echo "Unknown target"
        return -1
    fi

    # Work around configure endian check failing on modern gcc/binutils.
    # Assumes all supported archs are little endian.
    sed -i -e 's/EGIB/bss/g' -e 's/naidnePF/bss/g' configure

    # Host wrappers: endian/strings probes
    if [[ -n "${FFBUILD_CROSS_PREFIX:-}" ]] && ! command -v "${FFBUILD_CROSS_PREFIX}strings" >/dev/null 2>&1; then
        local _strings _bindir
        _strings="$(command -v strings 2>/dev/null || true)"
        _bindir="$(dirname "$(command -v "${FFBUILD_CROSS_PREFIX}gcc" 2>/dev/null || echo /usr/bin)")"
        if [[ -n "$_strings" && -d "$_bindir" ]]; then
            ln -sfn "$_strings" "${_bindir}/${FFBUILD_CROSS_PREFIX}strings"
        fi
    fi

    export CFLAGS="${CFLAGS:-} -fPIC -DPIC"
    export ASFLAGS="${ASFLAGS:-} -DPIC"

    echo "[INFO] xavs2 configure prefix=$FFBUILD_PREFIX (force PIC for PIE ffmpeg)"
    if ! ./configure "${myconf[@]}"; then
        echo "[ERROR] xavs2 configure failed" >&2
        return 1
    fi
    if [[ -f config.mak ]]; then
        sed -i -E "s|^prefix=.*|prefix=$FFBUILD_PREFIX|" config.mak || true
        # Ensure PIC flags stuck in generated makefile
        if ! grep -qE '(^| )-fPIC( |$)' config.mak 2>/dev/null; then
            echo "CFLAGS += -fPIC -DPIC" >> config.mak
            echo "ASFLAGS += -DPIC" >> config.mak
        fi
    fi

    make -j"$(nproc)"
    make install DESTDIR="$FFBUILD_DESTDIR"

    local dest_lib="${FFBUILD_DESTDIR}${FFBUILD_PREFIX}/lib/libxavs2.a"
    local dest_pc="${FFBUILD_DESTDIR}${FFBUILD_PREFIX}/lib/pkgconfig/xavs2.pc"
    if [[ ! -f "$dest_lib" ]]; then
        echo "[ERROR] xavs2: libxavs2.a missing after install" >&2
        find "${FFBUILD_DESTDIR}" -name '*xavs2*' 2>/dev/null | head -n 40 >&2 || true
        return 1
    fi
    if [[ ! -f "$dest_pc" ]]; then
        mkdir -p "$(dirname "$dest_pc")"
        cat >"$dest_pc" <<EOF
prefix=${FFBUILD_PREFIX}
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: xavs2
Description: AVS2 (IEEE 1857.4) encoder library
Version: 1.3.0
Libs: -L\${libdir} -lxavs2 -lpthread -lm -ldl
Cflags: -I\${includedir}
EOF
    fi

    # Smoke: archive must link into a PIE binary (matches FFmpeg --extra-ldexeflags=-pie)
    local smoke
    smoke="$(mktemp -d)"
    cat >"$smoke/t.c" <<'EOF'
#include <stdint.h>
#include <xavs2.h>
int main(void) { return (int)(intptr_t)xavs2_api_get; }
EOF
    if ! "${CC:-gcc}" -fPIE -pie -O0 -o "$smoke/t" "$smoke/t.c" \
        -I"${FFBUILD_DESTDIR}${FFBUILD_PREFIX}/include" \
        -L"${FFBUILD_DESTDIR}${FFBUILD_PREFIX}/lib" \
        -Wl,--start-group -lxavs2 -lpthread -lm -ldl -Wl,--end-group 2>"$smoke/err"; then
        echo "[ERROR] xavs2 PIE link smoke failed (asm likely non-PIC):" >&2
        cat "$smoke/err" >&2 || true
        rm -rf "$smoke"
        return 1
    fi
    rm -rf "$smoke"
    echo "[INFO] xavs2 PIE link smoke OK"
}

ffbuild_configure() {
    echo --enable-libxavs2
}

ffbuild_unconfigure() {
    echo --disable-libxavs2
}
