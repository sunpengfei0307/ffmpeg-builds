#!/bin/bash

SCRIPT_REPO="https://code.videolan.org/videolan/libplacebo.git"
SCRIPT_COMMIT="05ac2cca6571c04d06369a26825d207781b73f32"

ffbuild_depends() {
    echo base
    echo vulkan
}

ffbuild_enabled() {
    (( $(ffbuild_ffver) > 600 )) || return -1
    return 0
}

ffbuild_dl() {
    default_dl .
    # Avoid --filter=blob:none: shallow submodule can leave glad empty
    echo "git submodule update --init --recursive --depth=1"
}

ffbuild_build() {
    # Re-fetch submodules if cache missed glad
    if [[ -f .gitmodules ]] && [[ ! -d 3rdparty/glad && ! -d src/opengl/include/glad ]]; then
        echo "[INFO] libplacebo: git submodule update --init (glad)"
        git submodule update --init --recursive --depth=1 || \
            git submodule update --init --recursive || true
    fi

    local myconf=(
        --prefix="$FFBUILD_PREFIX"
        --buildtype=release
        --default-library=static
        -Dvulkan=enabled
        -Dvk-proc-addr=enabled
        -Dvulkan-registry="$FFBUILD_PREFIX"/share/vulkan/registry/vk.xml
        -Dshaderc=enabled
        -Dglslang=disabled
        -Ddemos=false
        -Dtests=false
        -Dbench=false
        -Dfuzz=false
    )

    # FFmpeg mainly uses Vulkan; disable OpenGL if glad still missing
    if [[ ! -d 3rdparty/glad && ! -d src/opengl/include/glad ]]; then
        echo "[WARN] libplacebo: glad missing, -Dopengl=disabled"
        myconf+=( -Dopengl=disabled )
    fi

    if [[ $TARGET == win* ]]; then
        myconf+=(
            -Dd3d11=enabled
        )
    fi

    if [[ $TARGET == win* || $TARGET == linux* ]]; then
        myconf+=(
            --cross-file="$CROSS_MESON"
        )
    else
        echo "Unknown target"
        return -1
    fi

    rm -rf build
    mkdir build && cd build

    meson setup "${myconf[@]}" ..
    ninja -j"$(nproc)"
    DESTDIR="$FFBUILD_DESTDIR" ninja install

    echo "Libs.private: -lstdc++" >> "$FFBUILD_DESTPREFIX"/lib/pkgconfig/libplacebo.pc
}

ffbuild_configure() {
    echo --enable-libplacebo
}

ffbuild_unconfigure() {
    (( $(ffbuild_ffver) >= 500 )) || return 0
    echo --disable-libplacebo
}

