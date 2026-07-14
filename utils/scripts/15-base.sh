#!/bin/bash

SCRIPT_SKIP="1"

ffbuild_depends() {
    echo mingw
    echo mingw-std-threads
    echo xorg-macros
}

ffbuild_enabled() {
    return 0
}

ffbuild_dl() {
    return 0
}

ffbuild_build() {
    return 0
}

ffbuild_ldexeflags() {
    return 0
}
