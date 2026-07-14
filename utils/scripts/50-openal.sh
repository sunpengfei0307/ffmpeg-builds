#!/bin/bash

SCRIPT_REPO="https://github.com/kcat/openal-soft.git"
# Recent master needs C++20. On GCC<11 pin to 1.23.1 (last pre-C++20-friendly release).
SCRIPT_COMMIT="93358135d92f3aa2672ed95f9c6bb04f8482d578"
_openal_gcc_maj="$(gcc -dumpversion 2>/dev/null | cut -d. -f1 || true)"
if [[ -n "${_openal_gcc_maj:-}" && "${_openal_gcc_maj}" -lt 11 ]]; then
    SCRIPT_COMMIT="1.23.1"
fi
unset _openal_gcc_maj

ffbuild_enabled() {
    (( $(ffbuild_ffver) > 501 )) || return -1
    return 0
}

ffbuild_build() {
    if [[ ! -f CMakeLists.txt ]]; then
        echo "[ERROR] openal source missing CMakeLists.txt" >&2
        return 1
    fi
    if grep -rqE '#include <(bit|span|concepts|ranges)>' common 2>/dev/null; then
        local gcc_maj
        gcc_maj="$(${CXX:-g++} -dumpversion 2>/dev/null | cut -d. -f1 || true)"
        if [[ -n "$gcc_maj" && "$gcc_maj" -lt 11 ]]; then
            echo "[ERROR] Cached openal needs C++20; GCC=${gcc_maj}." >&2
            echo "[ERROR] Run: rm -f dep/downloads/50-openal*.tar.xz .build/stages/50-openal.done" >&2
            echo "[ERROR] Then rebuild to fetch openal-soft 1.23.1" >&2
            return 1
        fi
    fi

    # openal-soft 1.23.1 uses cmake_minimum_required(VERSION 3.0.2); new CMake rejects <3.5
    if grep -qE 'cmake_minimum_required\s*\(\s*VERSION\s+3\.[0-4]' CMakeLists.txt 2>/dev/null; then
        echo "[INFO] bump cmake_minimum_required to 3.5 for modern CMake"
        sed -i -E 's/cmake_minimum_required\s*\(\s*VERSION\s+3\.[0-4][^)]*\)/cmake_minimum_required(VERSION 3.5)/' CMakeLists.txt
    fi

    rm -rf cm_build
    mkdir cm_build && cd cm_build

    export CFLAGS="$CFLAGS -include stdlib.h"
    export CXXFLAGS="$CXXFLAGS -include cstdlib"

    cmake -DCMAKE_TOOLCHAIN_FILE="$FFBUILD_CMAKE_TOOLCHAIN" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$FFBUILD_PREFIX" \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DLIBTYPE=STATIC -DALSOFT_UTILS=OFF -DALSOFT_EXAMPLES=OFF  ..
    make -j"$(nproc)"
    make install DESTDIR="$FFBUILD_DESTDIR"

    echo "Libs.private: -lstdc++" >> "$FFBUILD_DESTPREFIX"/lib/pkgconfig/openal.pc

    if [[ $TARGET == win* ]]; then
        echo "Libs.private: -lole32 -luuid" >> "$FFBUILD_DESTPREFIX"/lib/pkgconfig/openal.pc
    fi
}

ffbuild_configure() {
    echo --enable-openal
}

ffbuild_unconfigure() {
    echo --disable-openal
}