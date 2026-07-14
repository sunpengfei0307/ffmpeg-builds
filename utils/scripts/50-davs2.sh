#!/bin/bash

SCRIPT_REPO="https://github.com/pkuvcl/davs2.git"
SCRIPT_COMMIT="b41cf117452e2d73d827f02d3e30aa20f1c721ac"

ffbuild_enabled() {
    [[ $VARIANT == lgpl* ]] && return -1
    [[ $TARGET == win32 ]] && return -1
    # davs2 aarch64 support is broken
    [[ $TARGET == *arm64 ]] && return -1
    return 0
}

ffbuild_dl() {
    default_dl .
    echo "git fetch --unshallow"
}

ffbuild_build() {
    : "${FFBUILD_PREFIX:?FFBUILD_PREFIX unset}"
    : "${FFBUILD_DESTDIR:?FFBUILD_DESTDIR unset}"

    cd build/linux

    # FFmpeg links with -pie; keep C/asm PIC (same class of failure as xavs2).
    local myconf=(
        --disable-cli
        --enable-pic
        --extra-asflags="-DPIC"
        --extra-cflags="-fPIC -DPIC"
        --prefix="$FFBUILD_PREFIX"
    )
    export CFLAGS="${CFLAGS:-} -fPIC -DPIC"
    export ASFLAGS="${ASFLAGS:-} -DPIC"

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

    # Host wrappers: davs2/x264 configure may call ${triple}-strings
    if [[ -n "${FFBUILD_CROSS_PREFIX:-}" ]] && ! command -v "${FFBUILD_CROSS_PREFIX}strings" >/dev/null 2>&1; then
        local _strings _bindir
        _strings="$(command -v strings 2>/dev/null || true)"
        _bindir="$(dirname "$(command -v "${FFBUILD_CROSS_PREFIX}gcc" 2>/dev/null || echo /usr/bin)")"
        if [[ -n "$_strings" && -d "$_bindir" ]]; then
            ln -sfn "$_strings" "${_bindir}/${FFBUILD_CROSS_PREFIX}strings"
        fi
    fi

    echo "[INFO] davs2 configure prefix=$FFBUILD_PREFIX"
    if ! ./configure "${myconf[@]}"; then
        echo "[ERROR] davs2 configure failed" >&2
        return 1
    fi
    if [[ -f config.mak ]]; then
        sed -i -E "s|^prefix=.*|prefix=$FFBUILD_PREFIX|" config.mak || true
        sed -i -E "s|^libdir=.*|libdir=$FFBUILD_PREFIX/lib|" config.mak || true
        sed -i -E "s|^includedir=.*|includedir=$FFBUILD_PREFIX/include|" config.mak || true
    fi

    make -j"$(nproc)"
    make install DESTDIR="$FFBUILD_DESTDIR"

    local dest_lib="${FFBUILD_DESTDIR}${FFBUILD_PREFIX}/lib/libdavs2.a"
    local dest_pc="${FFBUILD_DESTDIR}${FFBUILD_PREFIX}/lib/pkgconfig/davs2.pc"
    local dest_inc="${FFBUILD_DESTDIR}${FFBUILD_PREFIX}/include"

    if [[ ! -f "$dest_lib" ]]; then
        echo "[WARN] davs2: lib missing after make install; installing manually"
        mkdir -p "${FFBUILD_DESTDIR}${FFBUILD_PREFIX}/lib/pkgconfig" "$dest_inc"
        local built
        built="$(find . -name 'libdavs2.a' -type f 2>/dev/null | head -n1)"
        [[ -n "$built" ]] || { echo "[ERROR] libdavs2.a not built" >&2; return 1; }
        cp -f "$built" "${FFBUILD_DESTDIR}${FFBUILD_PREFIX}/lib/libdavs2.a"
        # headers
        if [[ -f ../../davs2.h ]]; then
            cp -f ../../davs2.h "$dest_inc/"
        elif [[ -f davs2.h ]]; then
            cp -f davs2.h "$dest_inc/"
        else
            find ../.. -name 'davs2.h' -type f 2>/dev/null | head -n1 | xargs -I{} cp -f {} "$dest_inc/" || true
        fi
        if [[ -f davs2.pc ]]; then
            cp -f davs2.pc "$dest_pc"
            sed -i -E "s|^prefix=.*|prefix=${FFBUILD_PREFIX}|" "$dest_pc"
        elif [[ -f ../../davs2.pc ]]; then
            cp -f ../../davs2.pc "$dest_pc"
            sed -i -E "s|^prefix=.*|prefix=${FFBUILD_PREFIX}|" "$dest_pc"
        else
            cat >"$dest_pc" <<EOF
prefix=${FFBUILD_PREFIX}
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: davs2
Description: AVS2 (IEEE 1857.4) decoder library
Version: 1.6.0
Libs: -L\${libdir} -ldavs2
Libs.private: -lm -lpthread
Cflags: -I\${includedir}
EOF
        fi
    fi

    if [[ ! -f "$dest_lib" || ! -f "$dest_pc" ]]; then
        echo "[ERROR] davs2: still missing lib/pc under DESTPREFIX" >&2
        find "${FFBUILD_DESTDIR}" -type f 2>/dev/null | head -n 40 >&2 || true
        return 1
    fi
}

ffbuild_configure() {
    echo --enable-libdavs2
}

ffbuild_unconfigure() {
    echo --disable-libdavs2
}
