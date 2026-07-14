#!/bin/bash

SCRIPT_SKIP="1"

ffbuild_enabled() {
    [[ $TARGET == win* ]]
}

ffbuild_dl() {
    true
}

ffbuild_build() {
    return 0
}

ffbuild_configure() {
    echo --enable-schannel
}

ffbuild_unconfigure() {
    echo --disable-schannel
}
