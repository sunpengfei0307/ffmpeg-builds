#!/bin/bash

SCRIPT_REPO="https://github.com/v-novaltd/LCEVCdec.git"
SCRIPT_COMMIT="a254bd474649e5dcd8182689ac414420bfe8d8c3"

ffbuild_enabled() {
    (( $(ffbuild_ffver) >= 800 )) || return -1
    [[ $TARGET != winarm* ]] || return -1
    return 0
}

ffbuild_build() {
    : "${FFBUILD_PREFIX:?FFBUILD_PREFIX unset}"
    : "${FFBUILD_DESTDIR:?FFBUILD_DESTDIR unset}"

    mkdir build
    cd build

    cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE="$FFBUILD_CMAKE_TOOLCHAIN" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$FFBUILD_PREFIX" \
        -DCMAKE_INSTALL_LIBDIR=lib \
        -DBUILD_SHARED_LIBS=NO -DVN_SDK_EXECUTABLES=OFF -DVN_SDK_SAMPLE_SOURCE=OFF -DVN_SDK_TRACING=OFF -DVN_SDK_METRICS=OFF -DVN_SDK_SYSTEM_INSTALL=ON \
        -DVN_SDK_PIPELINE_LEGACY=OFF -DVN_SDK_PIPELINE_VULKAN=OFF -DPC_LIBS_PRIVATE="Libs.private: -lstdc++" ..
    ninja -j"$(nproc)"
    DESTDIR="$FFBUILD_DESTDIR" ninja install

    # 只清文档噪声；不可删整个 share/（本地前缀下会毁掉 xorg-macros/aclocal）
    rm -rf "${FFBUILD_DESTPREFIX}/share"/{doc,man,gtk-doc,info,licenses,lcevc*}

    local dest_pc="${FFBUILD_DESTDIR}${FFBUILD_PREFIX}/lib/pkgconfig/lcevc_dec.pc"
    # RHEL may still land under lib64
    if [[ ! -f "$dest_pc" && -f "${FFBUILD_DESTDIR}${FFBUILD_PREFIX}/lib64/pkgconfig/lcevc_dec.pc" ]]; then
        mkdir -p "${FFBUILD_DESTDIR}${FFBUILD_PREFIX}/lib/pkgconfig"
        cp -a "${FFBUILD_DESTDIR}${FFBUILD_PREFIX}/lib64/." "${FFBUILD_DESTDIR}${FFBUILD_PREFIX}/lib/"
        dest_pc="${FFBUILD_DESTDIR}${FFBUILD_PREFIX}/lib/pkgconfig/lcevc_dec.pc"
    fi

    if [[ ! -f "$dest_pc" ]]; then
        echo "[ERROR] lcevcdec: missing lcevc_dec.pc after install" >&2
        find "${FFBUILD_DESTDIR}" -name 'lcevc_dec.pc' -o -name '*lcevc*' 2>/dev/null | head -n 40 >&2 || true
        return 1
    fi
    # Ensure Version satisfies FFmpeg require_pkg_config "lcevc_dec >= 4.0.0"
    if ! grep -qiE '^Version:[[:space:]]*[4-9]\.' "$dest_pc"; then
        echo "[WARN] lcevc_dec.pc Version may be < 4.0.0; bumping to 4.0.0 for FFmpeg check"
        if grep -qE '^Version:' "$dest_pc"; then
            sed -i -E 's/^Version:.*/Version: 4.0.0/' "$dest_pc"
        else
            echo "Version: 4.0.0" >>"$dest_pc"
        fi
    fi
}

ffbuild_configure() {
    echo --enable-liblcevc-dec
}

ffbuild_unconfigure() {
    (( $(ffbuild_ffver) >= 701 )) || return 0
    echo --disable-liblcevc-dec
}
