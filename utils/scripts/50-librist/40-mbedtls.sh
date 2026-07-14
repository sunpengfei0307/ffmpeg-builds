#!/bin/bash

SCRIPT_REPO="https://github.com/ARMmbed/mbedtls.git"
SCRIPT_COMMIT="v4.1.0"
SCRIPT_TAGFILTER="v4.*"

ffbuild_enabled() {
    return 0
}

ffbuild_dl() {
    default_dl .
    echo "git submodule update --init --recursive --depth=1"
}

ffbuild_build() {
    # mbedtls 4.x GEN_FILES needs jsonschema
    local py gen_files=ON
    py="${FFBUILD_PYTHON:-}"
    if [[ -z "$py" ]]; then
        py="$(command -v python3.14 2>/dev/null || command -v python314 2>/dev/null || command -v python3.8 2>/dev/null || command -v python3)"
    fi
    if ! "$py" -c "import jsonschema" 2>/dev/null; then
        echo "[INFO] mbedtls: pip install jsonschema ($py)"
        "$py" -m pip install --user -U jsonschema 2>/dev/null || "$py" -m pip install -U jsonschema 2>/dev/null || true
    fi
    if ! "$py" -c "import jsonschema" 2>/dev/null; then
        echo "[WARN] jsonschema missing; using -DGEN_FILES=OFF"
        gen_files=OFF
    fi

    if [[ $TARGET == win32 ]]; then
        "$py" scripts/config.py unset MBEDTLS_AESNI_C
    fi

    rm -rf build
    mkdir build && cd build

    cmake -DCMAKE_TOOLCHAIN_FILE="$FFBUILD_CMAKE_TOOLCHAIN" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$FFBUILD_PREFIX" \
        -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DGEN_FILES="$gen_files" \
        -DUSE_STATIC_MBEDTLS_LIBRARY=ON -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DINSTALL_MBEDTLS_HEADERS=ON \
        -DPython3_EXECUTABLE="$py" \
        ..
    make -j$(nproc)
    make install DESTDIR="$FFBUILD_DESTDIR"

    if [[ $TARGET == win* ]]; then
        echo "Libs.private: -lws2_32 -lbcrypt -lwinmm -lgdi32" >> "$FFBUILD_DESTPREFIX"/lib/pkgconfig/mbedcrypto.pc
    fi
}

