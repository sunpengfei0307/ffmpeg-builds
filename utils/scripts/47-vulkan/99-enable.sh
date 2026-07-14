#!/bin/bash

SCRIPT_SKIP="1"

ffbuild_enabled() {
    (( $(ffbuild_ffver) > 404 )) || return -1
    return 0
}

ffbuild_dl() {
    true
}

ffbuild_build() {
    return 0
}
