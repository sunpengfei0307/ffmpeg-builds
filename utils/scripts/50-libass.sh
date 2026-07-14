#!/bin/bash

SCRIPT_REPO="https://github.com/libass/libass.git"
SCRIPT_COMMIT="4a05d8127f525943ebf45fdc6497c9e665947f0d"

ffbuild_depends() {
    echo base
    echo fonts
    echo fribidi
    echo libiconv
    echo libunibreak
}

ffbuild_enabled() {
    return 0
}

_libass_meson_ok() {
    local ver maj min
    ver="$(meson --version 2>/dev/null | head -n1 | grep -oE '[0-9]+\.[0-9]+' | head -n1 || true)"
    [[ -n "$ver" ]] || return 1
    maj="${ver%%.*}"
    min="${ver#*.}"
    [[ "$maj" -gt 0 ]] && return 0
    [[ "$maj" -eq 0 && "$min" -ge 64 ]] && return 0
    return 1
}

ffbuild_build() {
    export PKG_CONFIG_LIBDIR="${FFBUILD_PREFIX}/lib/pkgconfig:${FFBUILD_PREFIX}/share/pkgconfig${PKG_CONFIG_LIBDIR:+:$PKG_CONFIG_LIBDIR}"
    export PKG_CONFIG_PATH="$PKG_CONFIG_LIBDIR"
    export CFLAGS="$CFLAGS -Dread_file=libass_internal_read_file"

    if ! pkg-config --exists libunibreak; then
        echo "[ERROR] pkg-config 找不到 libunibreak（需先成功安装 45-libunibreak）" >&2
        echo "  PKG_CONFIG_PATH=${PKG_CONFIG_PATH:-}" >&2
        ls -la "${FFBUILD_PREFIX}/lib/pkgconfig"/libunibreak*.pc 2>/dev/null || true
        return 1
    fi

    if _libass_meson_ok; then
        mkdir build && cd build
        local myconf=(
            --prefix="$FFBUILD_PREFIX"
            --libdir=lib
            --buildtype=release
            --default-library=static
            -Dtest=disabled
            -Dcompare=disabled
            -Dprofile=disabled
            -Dfuzz=disabled
            -Dcheckasm=disabled
            -Dfontconfig=enabled
            -Dasm=enabled
            -Dlibunibreak=enabled
        )
        if [[ $TARGET == win* ]]; then
            myconf+=(-Ddirectwrite=enabled --cross-file="$CROSS_MESON")
        elif [[ $TARGET == linux* ]]; then
            myconf+=(--cross-file="$CROSS_MESON")
        else
            echo "Unknown target"; return -1
        fi
        meson setup "${myconf[@]}" ..
        ninja -j$(nproc)
        DESTDIR="$FFBUILD_DESTDIR" ninja install
        return 0
    fi

    # Fallback: autotools (works with older Meson/Python on EL8)
    echo "[INFO] libass: Meson=$(meson --version 2>/dev/null | head -n1) < 0.64，改用 autotools"
    ./autogen.sh
    local myconf=(
        --prefix="$FFBUILD_PREFIX"
        --disable-shared
        --enable-static
        --enable-libunibreak
    )
    if [[ $TARGET == win* || $TARGET == linux* ]]; then
        myconf+=(--host="$FFBUILD_TOOLCHAIN")
    else
        echo "Unknown target"; return -1
    fi
    ./configure "${myconf[@]}"
    make -j$(nproc)
    make install DESTDIR="$FFBUILD_DESTDIR"
}

ffbuild_configure() {
    echo --enable-libass
}

ffbuild_unconfigure() {
    echo --disable-libass
}