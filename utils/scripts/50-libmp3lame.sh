#!/bin/bash

# Prefer official release tarball. SVN trunk often fails / produces empty caches
# on restricted networks; release 3.100 already ships configure.
SCRIPT_REPO="https://svn.code.sf.net/p/lame/svn/trunk/lame"
SCRIPT_REV="6531"

LAME_RELEASE_VER="3.100"
LAME_RELEASE_URLS=(
    "https://downloads.sourceforge.net/project/lame/lame/${LAME_RELEASE_VER}/lame-${LAME_RELEASE_VER}.tar.gz"
    "https://sourceforge.net/projects/lame/files/lame/${LAME_RELEASE_VER}/lame-${LAME_RELEASE_VER}.tar.gz/download"
    "https://downloads.sourceforge.net/lame/lame-${LAME_RELEASE_VER}.tar.gz"
)

ffbuild_depends() {
    echo base
    echo libiconv
}

ffbuild_enabled() {
    return 0
}

ffbuild_dl() {
    # Keep a download recipe for cache hashing; actual fetch may use release in dockerbuild.
    echo "retry-tool sh -c \"rm -rf lame && ( svn checkout '${SCRIPT_REPO}@${SCRIPT_REV}' lame || true )\""
}

_lame_use_release() {
    local url ok=0 tmp srcdir
    echo "[INFO] libmp3lame: 使用官方 release ${LAME_RELEASE_VER}（避免 SVN 空树）"
    tmp="$(mktemp -d)"
    for url in "${LAME_RELEASE_URLS[@]}"; do
        echo "[INFO] 下载: $url"
        if curl -fsSL --connect-timeout 30 -L "$url" -o "$tmp/lame.tgz" \
            || wget -q -T 30 -O "$tmp/lame.tgz" "$url"; then
            # reject tiny/HTML error pages
            if [[ -s "$tmp/lame.tgz" ]] && [[ "$(stat -c%s "$tmp/lame.tgz" 2>/dev/null || stat -f%z "$tmp/lame.tgz")" -gt 100000 ]]; then
                ok=1
                break
            fi
        fi
        echo "[WARN] 下载失败或文件过小: $url"
    done
    [[ "$ok" -eq 1 ]] || { echo "[ERROR] LAME release 下载失败" >&2; rm -rf "$tmp"; return 1; }
    tar -xzf "$tmp/lame.tgz" -C "$tmp"
    srcdir="$(find "$tmp" -maxdepth 1 -type d -name 'lame-*' | head -n1)"
    [[ -n "$srcdir" && -d "$srcdir" ]] || { echo "[ERROR] tarball 内无 lame-*" >&2; rm -rf "$tmp"; return 1; }
    find . -mindepth 1 -maxdepth 1 -exec rm -rf {} +
    cp -a "$srcdir"/. .
    rm -rf "$tmp"
    [[ -f configure || -f configure.in || -f configure.ac ]] || {
        echo "[ERROR] release 树缺少 configure" >&2
        return 1
    }
}

ffbuild_build() {
    # Empty/corrupt SVN extract (common when cache is a 160-byte stub)
    if [[ ! -f configure.ac && ! -f configure.in && ! -f configure ]]; then
        _lame_use_release || return 1
    fi

    if [[ ! -x ./configure ]]; then
        if [[ -f configure.ac || -f configure.in ]]; then
            autoreconf -fi || autoreconf -i || true
        fi
    fi
    if [[ ! -x ./configure && ! -f ./configure ]]; then
        echo "[INFO] 工作树仍无 configure，改下 release"
        _lame_use_release || return 1
    fi

    local myconf=(
        --prefix="$FFBUILD_PREFIX"
        --disable-shared
        --enable-static
        --enable-nasm
        --disable-gtktest
        --disable-cpml
        --disable-frontend
        --disable-decoder
    )

    if [[ $TARGET == win* || $TARGET == linux* ]]; then
        myconf+=(
            --host="$FFBUILD_TOOLCHAIN"
        )
    else
        echo "Unknown target"
        return -1
    fi

    export CFLAGS="$CFLAGS -DNDEBUG -Wno-error=incompatible-pointer-types"

    ./configure "${myconf[@]}"
    make -j"$(nproc)"
    make install DESTDIR="$FFBUILD_DESTDIR"

    # Upstream lame often omits .pc; FFmpeg uses headers+lib, but keep a .pc for tooling.
    local dest_pc="${FFBUILD_DESTDIR}${FFBUILD_PREFIX}/lib/pkgconfig/lame.pc"
    if [[ ! -f "$dest_pc" && -f "${FFBUILD_DESTDIR}${FFBUILD_PREFIX}/lib/libmp3lame.a" ]]; then
        mkdir -p "$(dirname "$dest_pc")"
        cat >"$dest_pc" <<EOF
prefix=${FFBUILD_PREFIX}
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: lame
Description: lame MP3 encoder library
Version: ${LAME_RELEASE_VER}
Libs: -L\${libdir} -lmp3lame
Libs.private: -lm
Cflags: -I\${includedir}
EOF
    fi
}

ffbuild_configure() {
    echo --enable-libmp3lame
}

ffbuild_unconfigure() {
    echo --disable-libmp3lame
}