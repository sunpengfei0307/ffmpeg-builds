#!/bin/bash

SCRIPT_SKIP="1"

ffbuild_enabled() {
    [[ $TARGET == linux* ]]
}

ffbuild_dl() {
    true
}

ffbuild_build() {
    return 0
}

ffbuild_ldexeflags() {
    echo '-pie'

    if [[ $VARIANT == *shared* ]]; then
        # Can't escape escape hell
        echo -Wl,-rpath='\\\\\\\$\\\$ORIGIN'
        echo -Wl,-rpath='\\\\\\\$\\\$ORIGIN/../lib'
    fi
}
