#!/bin/bash

SCRIPT_REPO="https://github.com/BtbN/Vulkan-Shim-Loader.git"
SCRIPT_COMMIT="65b3936528cd92eb4ea3de485d03f858a3850484"

SCRIPT_REPO2="https://github.com/KhronosGroup/Vulkan-Headers.git"
SCRIPT_COMMIT2="v1.4.356"
SCRIPT_TAGFILTER2="v?.*.*"

ffbuild_enabled() {
    (( $(ffbuild_ffver) > 404 )) || return -1
    return 0
}

ffbuild_dl() {
    default_dl .
    echo "git-mini-clone \"$SCRIPT_REPO2\" \"$SCRIPT_COMMIT2\" Vulkan-Headers"
}

ffbuild_build() {
    mkdir build && cd build

    cmake -DCMAKE_TOOLCHAIN_FILE="$FFBUILD_CMAKE_TOOLCHAIN" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$FFBUILD_PREFIX" \
        -DVULKAN_SHIM_IMPERSONATE=ON ..
    make -j$(nproc)
    make install DESTDIR="$FFBUILD_DESTDIR"
}

ffbuild_configure() {
    # FFmpeg 6.x Vulkan AV1 仍用 Vk*MESA；现代 Vulkan-Headers 仅保留 KHR。
    # 优先读已 export 的 FFVER（get_output 子 shell 里比调用 ffbuild_ffver 更稳）。
    local ver="${FFVER:-99999999}"
    if [[ "$ver" -lt 700 ]]; then
        echo --disable-vulkan
        return 0
    fi
    echo --enable-vulkan
}

ffbuild_unconfigure() {
    (( ${FFVER:-0} >= 404 )) || return 0
    echo --disable-vulkan
}
