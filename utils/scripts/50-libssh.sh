#!/bin/bash

SCRIPT_REPO="https://gitlab.com/libssh/libssh-mirror.git"
SCRIPT_COMMIT="689d7320644bbf06f77911a58d41a68e3e68675b"

ffbuild_depends() {
    echo base
    echo zlib
    echo openssl
}

ffbuild_enabled() {
    return 0
}

ffbuild_build() {
    : "${FFBUILD_PREFIX:?FFBUILD_PREFIX unset}"
    : "${FFBUILD_DESTDIR:?FFBUILD_DESTDIR unset}"

    mkdir build && cd build

    export CFLAGS="$CFLAGS -Dmd5=libssh_md5"

    # Static FFmpeg builds: disable GSSAPI — host krb5 is usually shared-only and
    # leaves undefined gss_* refs that FFmpeg configure reports as "libssh not found".
    cmake -GNinja -DCMAKE_TOOLCHAIN_FILE="$FFBUILD_CMAKE_TOOLCHAIN" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$FFBUILD_PREFIX" \
        -DCMAKE_INSTALL_LIBDIR=lib \
        -DHAVE_STRNDUP=YES -DBUILD_SHARED_LIBS=OFF -DWITH_EXAMPLES=OFF -DWITH_SERVER=OFF -DWITH_SFTP=ON -DWITH_ZLIB=ON \
        -DWITH_GSSAPI=OFF \
        ..

    # Fix compilation on windows, mingw exports the symbol, but the header only shows it for c23.
    # Since the cmake script only checks for the symbol, it succeeds. But then fails to build.
    echo '#include <stddef.h>' >> config.h
    echo 'char * strndup(const char *s, size_t c);' >> config.h

    ninja -j"$(nproc)"
    DESTDIR="$FFBUILD_DESTDIR" ninja install

    local pc="${FFBUILD_DESTPREFIX}/lib/pkgconfig/libssh.pc"
    if [[ ! -f "$pc" && -f "${FFBUILD_DESTPREFIX}/lib64/pkgconfig/libssh.pc" ]]; then
        mkdir -p "${FFBUILD_DESTPREFIX}/lib/pkgconfig"
        cp -a "${FFBUILD_DESTPREFIX}/lib64/." "${FFBUILD_DESTPREFIX}/lib/"
        pc="${FFBUILD_DESTPREFIX}/lib/pkgconfig/libssh.pc"
    fi
    [[ -f "$pc" ]] || { echo "[ERROR] libssh.pc missing after install" >&2; return 1; }

    # Drop empty/duplicate Requires.private lines cmake may leave, then append once.
    sed -i '/^Requires\.private:[[:space:]]*$/d' "$pc"
    if ! grep -q '^Requires\.private:.*libssl' "$pc"; then
        {
            echo "Requires.private: libssl libcrypto zlib"
            echo "Cflags.private: -DLIBSSH_STATIC"
            if [[ $TARGET == win* ]]; then
                echo "Libs.private: -liphlpapi -lws2_32"
            fi
            echo "Libs.private: -lpthread"
        } >> "$pc"
    elif ! grep -q 'Cflags.private:.*LIBSSH_STATIC' "$pc"; then
        echo "Cflags.private: -DLIBSSH_STATIC" >> "$pc"
    fi
}

ffbuild_configure() {
    echo --enable-libssh
}

ffbuild_unconfigure() {
    echo --disable-libssh
}
