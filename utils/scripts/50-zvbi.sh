#!/bin/bash

SCRIPT_REPO="https://github.com/zapping-vbi/zvbi"
SCRIPT_COMMIT="41477c97c8edf7a01f1594b2a95b94f0117eed21"

ffbuild_depends() {
    echo base
    echo libiconv
}

ffbuild_enabled() {
    return 0
}

ffbuild_build() {
    # EL8 gettext/autopoint is often < 0.21; zvbi configure.ac may require 0.21+.
    # We already --disable-nls, so pin AM_GNU_GETTEXT_VERSION to host gettext.
    local gt_ver
    gt_ver="$(gettext --version 2>/dev/null | head -n1 | grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?' | head -n1 || true)"
    if [[ -n "$gt_ver" && -f configure.ac ]]; then
        if grep -q 'AM_GNU_GETTEXT_VERSION' configure.ac; then
            echo "[INFO] zvbi: pin AM_GNU_GETTEXT_VERSION to host gettext ${gt_ver}"
            sed -i -E "s/AM_GNU_GETTEXT_VERSION\(\[[^]]*\]\)/AM_GNU_GETTEXT_VERSION([${gt_ver}])/" configure.ac
            sed -i -E "s/AM_GNU_GETTEXT_VERSION\([^)]*\)/AM_GNU_GETTEXT_VERSION([${gt_ver}])/" configure.ac
        fi
    fi
    # Prefer REQUIRE_VERSION if present (allows newer); still lower if needed
    if grep -q 'AM_GNU_GETTEXT_REQUIRE_VERSION' configure.ac 2>/dev/null && [[ -n "$gt_ver" ]]; then
        sed -i -E "s/AM_GNU_GETTEXT_REQUIRE_VERSION\(\[[^]]*\]\)/AM_GNU_GETTEXT_REQUIRE_VERSION([${gt_ver}])/" configure.ac || true
    fi

    # Avoid autopoint hard-fail: try AUTOPOINT=true if still too old after pin
    if ! ./autogen.sh; then
        echo "[WARN] zvbi autogen failed; retry with AUTOPOINT=true"
        AUTOPOINT=true autoreconf -fi || AUTOPOINT=true ./autogen.sh || {
            echo "[ERROR] zvbi autogen/autoreconf failed (gettext/autopoint)" >&2
            return 1
        }
    fi

    local myconf=(
        --prefix="$FFBUILD_PREFIX"
        --disable-shared
        --enable-static
        --with-pic
        --without-doxygen
        --without-x
        --disable-dvb
        --disable-bktr
        --disable-nls
        --disable-proxy
    )

    if [[ $TARGET == win* || $TARGET == linux* ]]; then
        myconf+=(
            --host="$FFBUILD_TOOLCHAIN"
        )
    else
        echo "Unknown target"
        return -1
    fi

    ./configure "${myconf[@]}"
    make -C src -j"$(nproc)"
    make -C src install DESTDIR="$FFBUILD_DESTDIR"
    make SUBDIRS=. install DESTDIR="$FFBUILD_DESTDIR"
}

ffbuild_configure() {
    echo --enable-libzvbi
}

ffbuild_unconfigure() {
    echo --disable-libzvbi
}