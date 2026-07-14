#!/bin/bash

SCRIPT_REPO="https://gitlab.freedesktop.org/fontconfig/fontconfig.git"
SCRIPT_COMMIT="939e33ee473d70a790c89b624385c2c0a5875a51"

# Git tip (2.18+) requires Meson >= 1.11 → Python >= 3.7.
# CentOS/RHEL 8 ships Python 3.6 and can only run Meson ~0.61, so use a
# release tarball that still accepts Meson 0.60+.
FONTCONFIG_RELEASE_VER="2.15.0"
FONTCONFIG_RELEASE_URLS=(
    "https://www.freedesktop.org/software/fontconfig/release/fontconfig-${FONTCONFIG_RELEASE_VER}.tar.xz"
    "https://gitlab.freedesktop.org/fontconfig/fontconfig/-/archive/${FONTCONFIG_RELEASE_VER}/fontconfig-${FONTCONFIG_RELEASE_VER}.tar.gz"
)

ffbuild_depends() {
    echo base
    echo libxml2
    echo libiconv
}

ffbuild_enabled() {
    return 0
}

_fc_meson_major_minor() {
    # prints e.g. 0.61 or 1.11 ; empty on failure
    meson --version 2>/dev/null | head -n1 | grep -oE '[0-9]+\.[0-9]+' | head -n1
}

_fc_meson_too_old_for_git() {
    local ver maj min
    ver="$(_fc_meson_major_minor)"
    [[ -n "$ver" ]] || return 0
    maj="${ver%%.*}"
    min="${ver#*.}"
    # need >= 1.11 for current git tip
    if [[ "$maj" -gt 1 ]]; then
        return 1
    fi
    if [[ "$maj" -eq 1 && "$min" -ge 11 ]]; then
        return 1
    fi
    return 0
}

_fc_use_release_tarball() {
    local url ok=0 tmp srcdir
    echo "[INFO] fontconfig: Meson=$(_fc_meson_major_minor) 过旧，改用 release ${FONTCONFIG_RELEASE_VER}"
    tmp="$(mktemp -d)"
    for url in "${FONTCONFIG_RELEASE_URLS[@]}"; do
        echo "[INFO] 下载: $url"
        if curl -fsSL --connect-timeout 30 "$url" -o "$tmp/fc.tgz" \
            || wget -q -T 30 "$url" -O "$tmp/fc.tgz"; then
            ok=1
            break
        fi
        echo "[WARN] 下载失败: $url"
    done
    [[ "$ok" -eq 1 ]] || { echo "[ERROR] fontconfig release 下载失败" >&2; rm -rf "$tmp"; return 1; }
    tar -xf "$tmp/fc.tgz" -C "$tmp"
    srcdir="$(find "$tmp" -maxdepth 1 -type d -name 'fontconfig-*' | head -n1)"
    [[ -n "$srcdir" && -d "$srcdir" ]] || { echo "[ERROR] tarball 内无 fontconfig-*" >&2; rm -rf "$tmp"; return 1; }
    find . -mindepth 1 -maxdepth 1 -exec rm -rf {} +
    cp -a "$srcdir"/. .
    rm -rf "$tmp"
    [[ -f meson.build ]] || { echo "[ERROR] release 树缺少 meson.build" >&2; return 1; }
}

ffbuild_build() {
    # Local EL8 / old meson: switch to release sources before configure
    if _fc_meson_too_old_for_git; then
        _fc_use_release_tarball || return 1
    fi

    mkdir build && cd build

    local myconf=(
        --prefix="$FFBUILD_PREFIX"
        --buildtype=release
        --default-library=static
        -Ddoc=disabled
        -Dtests=disabled
    )

    # Options differ slightly across versions; probe meson.build
    if grep -q "iconv" ../meson.build 2>/dev/null; then
        myconf+=(-Diconv=enabled)
    fi
    if grep -q "xml-backend" ../meson.build 2>/dev/null; then
        myconf+=(-Dxml-backend=libxml2)
    elif grep -q "xml2" ../meson.build 2>/dev/null; then
        # older option names
        :
    fi
    if grep -q "tools" ../meson.build 2>/dev/null; then
        myconf+=(-Dtools=disabled)
    fi
    if grep -q "cache-build" ../meson.build 2>/dev/null; then
        myconf+=(-Dcache-build=disabled)
    fi

    if [[ $TARGET == linux* ]]; then
        myconf+=(
            --sysconfdir=/etc
            --localstatedir=/var
            --cross-file="$CROSS_MESON"
        )
    elif [[ $TARGET == win* ]]; then
        myconf+=(
            --cross-file="$CROSS_MESON"
        )
    else
        echo "Unknown target"
        return -1
    fi

    export PKG_CONFIG_LIBDIR="${FFBUILD_PREFIX}/lib/pkgconfig:${FFBUILD_PREFIX}/share/pkgconfig${PKG_CONFIG_LIBDIR:+:$PKG_CONFIG_LIBDIR}"
    export PKG_CONFIG_PATH="$PKG_CONFIG_LIBDIR"

    meson setup "${myconf[@]}" ..
    ninja -j"$(nproc)"
    DESTDIR="$FFBUILD_DESTDIR" ninja install

    # 本地构建共用 FFBUILD_PREFIX：不可 rm -rf share/（会删掉 xorg-macros 等）
    rm -rf "$FFBUILD_DESTPREFIX"/{var,etc}
    rm -rf "$FFBUILD_DESTPREFIX"/share/{doc,man,gtk-doc,info,fontconfig,xml,gettext}
}

ffbuild_configure() {
    echo --enable-fontconfig
}

ffbuild_unconfigure() {
    echo --disable-fontconfig
}