#!/bin/bash

SCRIPT_REPO="https://chromium.googlesource.com/webm/libwebp"
SCRIPT_COMMIT="733c91e461c18cf1127c9ed0a80dccbcfed599d3"

ffbuild_enabled() {
    return 0
}

ffbuild_build() {
    # Remove broken internal library that depends on things we disable
    sed -i '/libanim_util/d' examples/Makefile.am

    ./autogen.sh

    local myconf=(
        --prefix="$FFBUILD_PREFIX"
        --disable-shared
        --enable-static
        --with-pic
        --enable-libwebpmux
        --enable-libwebpdemux
        --disable-libwebpextras
        --disable-sdl
        --disable-gl
        --disable-png
        --disable-jpeg
        --disable-tiff
        --disable-gif
    )

    # AVX2 intrinsics need GCC>=11; GCC 8 leaves undefined refs when linking tools
    local gcc_maj
    gcc_maj="$(${CC:-gcc} -dumpversion 2>/dev/null | cut -d. -f1 || true)"
    if [[ -n "$gcc_maj" && "$gcc_maj" -lt 11 ]]; then
        echo "[INFO] GCC ${gcc_maj}: libwebp --disable-avx2"
        myconf+=( --disable-avx2 )
    fi

    if [[ $TARGET == win* || $TARGET == linux* ]]; then
        myconf+=(
            --host="$FFBUILD_TOOLCHAIN"
        )
    else
        echo "Unknown target"
        return -1
    fi

    ./configure "${myconf[@]}"
    make -j"$(nproc)"
    make install DESTDIR="$FFBUILD_DESTDIR"
}

ffbuild_configure() {
    echo --enable-libwebp
}

ffbuild_unconfigure() {
    echo --disable-libwebp
}

