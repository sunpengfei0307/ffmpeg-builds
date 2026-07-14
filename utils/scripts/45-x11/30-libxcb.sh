#!/bin/bash

SCRIPT_REPO="https://gitlab.freedesktop.org/xorg/lib/libxcb.git"
SCRIPT_COMMIT="4d6e1c8fffaf811cf4d0e68ff3bc6f50e62c32c7"

ffbuild_enabled() {
    [[ $TARGET != linux* ]] && return -1
    return 0
}

ffbuild_build() {
    autoreconf -i

    local myconf=(
        --prefix="$FFBUILD_PREFIX"
        --enable-shared
        --disable-static
        --with-pic
        --disable-devel-docs
    )

    if [[ $TARGET == linux* ]]; then
        myconf+=(
            --host="$FFBUILD_TOOLCHAIN"
        )
    else
        echo "Unknown target"
        return -1
    fi

    export CFLAGS="$RAW_CFLAGS"
    export LDFLAGS="$RAW_LDFLAGS"

    ./configure "${myconf[@]}"
    make -j$(nproc)
    make install DESTDIR="$FFBUILD_DESTDIR"

    # NOTE: do NOT use ${LIBNAME%%.*} — it truncates at the first '.' in the
    # absolute path (breaks when the workspace lives under e.g. ffmpeg.8.1.2/).
    # Strip from ".so" onward instead (libxcb.so.1 → libxcb, libxcb-xv.so.0 → libxcb-xv).
    local LIBNAME BASE
    for LIBNAME in "$FFBUILD_DESTPREFIX"/lib/libxcb*.so.?; do
        [[ -e "$LIBNAME" ]] || continue
        BASE="${LIBNAME%.so*}"
        gen-implib "$LIBNAME" "${BASE}.a"
        rm -f "${BASE}".so* "${BASE}".la
    done
}

ffbuild_configure() {
    echo --enable-libxcb
}

ffbuild_unconfigure() {
    echo --disable-libxcb
}
