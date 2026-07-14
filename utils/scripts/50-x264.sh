#!/bin/bash

SCRIPT_REPO="https://code.videolan.org/videolan/x264.git"
SCRIPT_COMMIT="0480cb05fa188d37ae87e8f4fd8f1aea3711f7ee"

ffbuild_enabled() {
    [[ $VARIANT == lgpl* ]] && return -1
    return 0
}

ffbuild_build() {
    : "${FFBUILD_PREFIX:?FFBUILD_PREFIX unset}"
    : "${FFBUILD_DESTDIR:?FFBUILD_DESTDIR unset}"

    local myconf=(
        --disable-cli
        --enable-static
        --enable-pic
        --disable-lavf
        --disable-swscale
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

    echo "[INFO] x264 configure prefix=$FFBUILD_PREFIX destdir=$FFBUILD_DESTDIR cross-prefix=${FFBUILD_CROSS_PREFIX:-}"
    # Host wrappers must provide ${triple}-strings (endian test); create if missing.
    if [[ -n "${FFBUILD_CROSS_PREFIX:-}" ]] && ! command -v "${FFBUILD_CROSS_PREFIX}strings" >/dev/null 2>&1; then
        local _strings _bindir
        _strings="$(command -v strings 2>/dev/null || true)"
        _bindir="$(dirname "$(command -v "${FFBUILD_CROSS_PREFIX}gcc" 2>/dev/null || echo /usr/bin)")"
        if [[ -n "$_strings" && -d "$_bindir" ]]; then
            ln -sfn "$_strings" "${_bindir}/${FFBUILD_CROSS_PREFIX}strings"
            echo "[INFO] x264: linked ${_bindir}/${FFBUILD_CROSS_PREFIX}strings -> $_strings"
        fi
    fi
    if ! ./configure "${myconf[@]}"; then
        echo "[ERROR] x264 configure failed (often missing ${FFBUILD_CROSS_PREFIX}strings)" >&2
        return 1
    fi
    [[ -f config.mak ]] || { echo "[ERROR] x264: no config.mak after configure" >&2; return 1; }

    # x264 may ignore --prefix under some host/cross combos; force config.mak.
    sed -i -E "s|^prefix=.*|prefix=$FFBUILD_PREFIX|" config.mak
    sed -i -E "s|^libdir=.*|libdir=$FFBUILD_PREFIX/lib|" config.mak
    sed -i -E "s|^includedir=.*|includedir=$FFBUILD_PREFIX/include|" config.mak
    if grep -qE '^prefix=/usr/local/?$' config.mak 2>/dev/null; then
        echo "[ERROR] x264 configure used prefix=/usr/local; FFBUILD_PREFIX was ignored" >&2
        return 1
    fi
    if ! grep -qE "^prefix=${FFBUILD_PREFIX}$" config.mak 2>/dev/null; then
        echo "[ERROR] x264 config.mak prefix mismatch (want $FFBUILD_PREFIX)" >&2
        grep -E '^(prefix|libdir|includedir)=' config.mak >&2 || true
        return 1
    fi

    make -j"$(nproc)"
    make install DESTDIR="$FFBUILD_DESTDIR"

    local dest_lib="${FFBUILD_DESTDIR}${FFBUILD_PREFIX}/lib/libx264.a"
    local dest_pc="${FFBUILD_DESTDIR}${FFBUILD_PREFIX}/lib/pkgconfig/x264.pc"
    local dest_inc="${FFBUILD_DESTDIR}${FFBUILD_PREFIX}/include"

    # Manual fallback: install only built the CLI under /usr/local before.
    if [[ ! -f "$dest_lib" ]]; then
        echo "[WARN] x264: lib missing after make install; installing lib/headers/pc manually"
        mkdir -p "${FFBUILD_DESTDIR}${FFBUILD_PREFIX}/lib/pkgconfig" "$dest_inc"
        [[ -f libx264.a ]] || { echo "[ERROR] libx264.a not built" >&2; return 1; }
        cp -f libx264.a "${FFBUILD_DESTDIR}${FFBUILD_PREFIX}/lib/"
        cp -f x264.h x264_config.h "$dest_inc/"
        if [[ -f x264.pc ]]; then
            cp -f x264.pc "$dest_pc"
            # rewrite prefix inside .pc if needed
            sed -i -E "s|^prefix=.*|prefix=${FFBUILD_PREFIX}|" "$dest_pc"
        else
            cat >"$dest_pc" <<EOF
prefix=${FFBUILD_PREFIX}
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: x264
Description: H.264 (MPEG4 AVC) encoder library
Version: 0.164.0
Libs: -L\${libdir} -lx264
Libs.private: -lm -lpthread -ldl
Cflags: -I\${includedir}
EOF
        fi
    fi

    if [[ ! -f "$dest_lib" || ! -f "$dest_pc" ]]; then
        echo "[ERROR] x264: still missing lib/pc under DESTPREFIX after install" >&2
        echo "[ERROR] expected: $dest_lib and $dest_pc" >&2
        find "${FFBUILD_DESTDIR}" -type f 2>/dev/null | head -n 50 >&2 || true
        return 1
    fi
}

ffbuild_configure() {
    echo --enable-libx264
}

ffbuild_unconfigure() {
    echo --disable-libx264
}

ffbuild_cflags() {
    return 0
}

ffbuild_ldflags() {
    return 0
}
