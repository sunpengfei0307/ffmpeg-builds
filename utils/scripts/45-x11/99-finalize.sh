#!/bin/bash

SCRIPT_SKIP="1"

ffbuild_enabled() {
    [[ $TARGET != linux* ]] && return -1
    return 0
}

ffbuild_dl() {
    return 0
}

ffbuild_build() {
    rm "$FFBUILD_DESTPREFIX"/lib/lib*.so* || true
    rm "$FFBUILD_DESTPREFIX"/lib/*.la || true
}

ffbuild_libs() {
    echo -ldl
}
