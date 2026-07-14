#!/bin/bash

SCRIPT_REPO="https://code.videolan.org/videolan/libdvdnav.git"
SCRIPT_COMMIT="9c5f2278eb5b23cdcd0575065f5d575c4e6602a4"

ffbuild_depends() {
    echo base
}

ffbuild_enabled() {
    [[ $VARIANT == lgpl* ]] && return -1
    (( $(ffbuild_ffver) >= 700 )) || return -1
    return 0
}

ffbuild_build() {
    if ! pkg-config --exists dvdread; then
        echo "[ERROR] pkg-config 找不到 dvdread（需要先成功安装 40-libdvdread）" >&2
        echo "  PKG_CONFIG_PATH=${PKG_CONFIG_PATH:-}" >&2
        echo "  PKG_CONFIG_LIBDIR=${PKG_CONFIG_LIBDIR:-}" >&2
        ls -la "${FFBUILD_PREFIX}/lib/pkgconfig"/dvd*.pc \
               "${FFBUILD_PREFIX}/lib64/pkgconfig"/dvd*.pc 2>/dev/null || true
        return 1
    fi

    # stop the static library from exporting symbols when linked into a shared lib
    sed -i 's/SUPPORT_ATTRIBUTE_VISIBILITY_DEFAULT/SUPPORT_ATTRIBUTE_VISIBILITY_DEFAULT_DISABLED/g' meson.build
    sed -i 's/-DLIBDVDCSS_EXPORTS/-DLIBDVDCSS_EXPORTS_DISABLED/g' src/meson.build

    mkdir build && cd build

    local myconf=(
        --prefix="$FFBUILD_PREFIX"
        --libdir=lib
        -Ddefault_library=static
        -Denable_docs=false
        -Denable_examples=false
    )

    if [[ $TARGET == win* || $TARGET == linux* ]]; then
        myconf+=(
            --cross-file="$CROSS_MESON"
        )
    else
        echo "Unknown target"
        return -1
    fi

    meson setup "${myconf[@]}" ..
    ninja -j$(nproc)
    DESTDIR="$FFBUILD_DESTDIR" ninja install
}

ffbuild_configure() {
    echo --enable-libdvdnav
}

ffbuild_unconfigure() {
    (( $(ffbuild_ffver) >= 700 )) || return 0
    echo --disable-libdvdnav
}
