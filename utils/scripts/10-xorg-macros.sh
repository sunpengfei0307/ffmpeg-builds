#!/bin/bash

SCRIPT_REPO="https://gitlab.freedesktop.org/xorg/util/macros.git"
SCRIPT_COMMIT="a9d71e3fd8e6758b70be31c586921bbbcd2a8449"

ffbuild_depends() {
    return 0
}

ffbuild_enabled() {
    [[ $TARGET != linux* ]] && return -1
    return 0
}

ffbuild_build() {
    autoreconf -i
    ./configure --prefix="$FFBUILD_PREFIX"
    make -j"$(nproc)"
    make install DESTDIR="$FFBUILD_DESTDIR"
}
