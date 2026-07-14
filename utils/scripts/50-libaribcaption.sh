#!/bin/bash

SCRIPT_REPO="https://github.com/xqq/libaribcaption.git"
SCRIPT_COMMIT="f9d8c50fe5e51c98d101f69d74591295cb568036"

ffbuild_depends() {
    echo base
    echo fonts
    echo openssl
}

ffbuild_enabled() {
    (( $(ffbuild_ffver) > 600 )) || return -1
    return 0
}

ffbuild_build() {
    mkdir build
    cd build

    export CFLAGS="$CFLAGS -DHAVE_OPENSSL=1"
    export CXXFLAGS="$CXXFLAGS -DHAVE_OPENSSL=1"
    export PKG_CONFIG_LIBDIR="${FFBUILD_PREFIX}/lib/pkgconfig:${FFBUILD_PREFIX}/share/pkgconfig${PKG_CONFIG_LIBDIR:+:$PKG_CONFIG_LIBDIR}"
    export PKG_CONFIG_PATH="$PKG_CONFIG_LIBDIR"

    # Fontconfig/Freetype must come from our prefix (usr/), not the host.
    if ! pkg-config --exists fontconfig; then
        echo "[ERROR] fontconfig.pc 不在 PKG_CONFIG_LIBDIR=$PKG_CONFIG_LIBDIR" >&2
        echo "[ERROR] 请先成功构建 35-fontconfig（fonts）" >&2
        return 1
    fi

    cmake -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$FFBUILD_CMAKE_TOOLCHAIN" \
        -DCMAKE_INSTALL_PREFIX="$FFBUILD_PREFIX" \
        -DCMAKE_PREFIX_PATH="$FFBUILD_PREFIX" \
        -DCMAKE_FIND_ROOT_PATH="$FFBUILD_PREFIX" \
        -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH \
        -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH \
        -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=BOTH \
        -DARIBCC_SHARED_LIBRARY=OFF -DARIBCC_BUILD_TESTS=OFF -DBUILD_SHARED_LIBS=OFF \
        -DARIBCC_USE_FREETYPE=ON -DARIBCC_USE_EMBEDDED_FREETYPE=OFF \
        ..

    ninja -j$(nproc)
    DESTDIR="$FFBUILD_DESTDIR" ninja install

    local pc="$FFBUILD_DESTPREFIX/lib/pkgconfig/libaribcaption.pc"
    if [[ -f "$pc" ]]; then
        echo "Libs.private: -lstdc++ -lcrypto" >> "$pc"
    else
        echo "[WARN] libaribcaption.pc 未生成于 $pc"
    fi
}

ffbuild_configure() {
    echo --enable-libaribcaption
}

ffbuild_unconfigure() {
    (( $(ffbuild_ffver) > 600 )) || return 0
    echo --disable-libaribcaption
}
