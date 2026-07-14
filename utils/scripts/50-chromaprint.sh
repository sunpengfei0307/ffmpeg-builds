#!/bin/bash

SCRIPT_REPO="https://github.com/acoustid/chromaprint.git"
SCRIPT_COMMIT="ab48115481c14873eb870e7a88334550c68d36c1"

ffbuild_depends() {
    echo base
    echo fftw3
}

ffbuild_enabled() {
    (( $(ffbuild_ffver) >= 600 )) || return -1
    return 0
}

ffbuild_build() {
    mkdir build && cd build

    # RHEL/CentOS 默认 lib64；强制 lib，避免 .pc 装到 lib64 而脚本往 lib/ 追加残缺文件
    cmake -DCMAKE_TOOLCHAIN_FILE="$FFBUILD_CMAKE_TOOLCHAIN" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$FFBUILD_PREFIX" \
        -DCMAKE_INSTALL_LIBDIR=lib \
        -DBUILD_SHARED_LIBS=OFF \
        -DBUILD_TOOLS=OFF \
        -DBUILD_TESTS=OFF \
        -DFFT_LIB=fftw3 \
        ..
    make -j$(nproc)
    make install DESTDIR="$FFBUILD_DESTDIR"

    local pc="${FFBUILD_DESTPREFIX}/lib/pkgconfig/libchromaprint.pc"
    mkdir -p "$(dirname "$pc")"
    if [[ ! -f "$pc" ]] || ! grep -qE '^(Name:|prefix=)' "$pc"; then
        if [[ -f "${FFBUILD_DESTPREFIX}/lib64/pkgconfig/libchromaprint.pc" ]]; then
            cp -f "${FFBUILD_DESTPREFIX}/lib64/pkgconfig/libchromaprint.pc" "$pc"
        else
            echo "[ERROR] chromaprint 未安装有效的 libchromaprint.pc" >&2
            find "${FFBUILD_DESTPREFIX}" -name '*chromaprint*' 2>/dev/null | head -n 40 >&2 || true
            return 1
        fi
    fi

    # 避免重复追加
    grep -q 'Libs.private:.*fftw3' "$pc" || echo "Libs.private: -lfftw3 -lstdc++" >> "$pc"
    grep -q 'Cflags.private:.*CHROMAPRINT_NODLL' "$pc" || echo "Cflags.private: -DCHROMAPRINT_NODLL" >> "$pc"
}

ffbuild_configure() {
    echo --enable-chromaprint
}

ffbuild_unconfigure() {
    echo --disable-chromaprint
}
