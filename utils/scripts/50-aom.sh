#!/bin/bash

SCRIPT_REPO="https://aomedia.googlesource.com/aom"
SCRIPT_COMMIT="43f2b6a99000057184332c8c2dd4bedc19fdec6f"

ffbuild_depends() {
    echo base
    echo vmaf
}

ffbuild_enabled() {
    [[ $TARGET == winarm64 ]] && return -1
    return 0
}

ffbuild_build() {
    local _pdir="${ROOT_DIR}/patches/aom"
    if [[ -d "$_pdir" ]]; then
        shopt -s nullglob
        for patch in "$_pdir"/*.patch; do
            echo "Applying $patch"
            git am < "$patch"
        done
        shopt -u nullglob
    fi

    mkdir cmbuild && cd cmbuild

    # Workaround broken build system
    export CFLAGS="$CFLAGS -pthread -I${FFBUILD_PREFIX}/include/libvmaf"

    cmake -DCMAKE_TOOLCHAIN_FILE="$FFBUILD_CMAKE_TOOLCHAIN" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$FFBUILD_PREFIX" -DBUILD_SHARED_LIBS=OFF -DENABLE_EXAMPLES=NO -DENABLE_TESTS=NO -DENABLE_TOOLS=NO -DCONFIG_TUNE_VMAF=1 ..
    make -j$(nproc)
    make install DESTDIR="$FFBUILD_DESTDIR"

    echo "Requires.private: libvmaf" >> "$FFBUILD_DESTPREFIX"/lib/pkgconfig/aom.pc
}

ffbuild_configure() {
    echo --enable-libaom
}

ffbuild_unconfigure() {
    echo --disable-libaom
}
