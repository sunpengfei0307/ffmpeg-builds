#!/bin/bash

SCRIPT_REPO="https://github.com/xiph/rav1e.git"
SCRIPT_COMMIT="564ae3b0007ae2b06893fd7166bf88c5a84c5b63"

ffbuild_enabled() {
    [[ $TARGET == win32 ]] && return -1
    return 0
}

ffbuild_build() {
    export CARGO_HOME="${CARGO_HOME:-${LOCAL_OPT}/cargo}"
    export RUSTUP_HOME="${RUSTUP_HOME:-${LOCAL_OPT}/rustup}"
    export PATH="${CARGO_HOME}/bin:${PATH}"

    if ! command -v cargo-cinstall &>/dev/null && [[ ! -x "${CARGO_HOME}/bin/cargo-cinstall" ]]; then
        echo "[INFO] rav1e: 缺少 cargo-cinstall，尝试安装 cargo-c…"
        if ! cargo install cargo-c; then
            cargo install cargo-c --features=vendored-openssl || {
                echo "[ERROR] 需要 cargo-c（cargo install cargo-c）才能执行 cargo cinstall" >&2
                return 1
            }
        fi
        hash -r 2>/dev/null || true
    fi
    if ! command -v cargo-cinstall &>/dev/null && [[ ! -x "${CARGO_HOME}/bin/cargo-cinstall" ]]; then
        echo "[ERROR] 仍找不到 cargo-cinstall，无法构建 rav1e" >&2
        return 1
    fi

    local myconf=(
        --prefix="${FFBUILD_PREFIX}"
        --destdir="${FFBUILD_DESTDIR}"
        --target="${FFBUILD_RUST_TARGET}"
        --library-type=staticlib
        --crt-static
        --release
    )

    # Pulls in target-libs for host tool builds otherwise.
    # Luckily no target libraries are needed.
    unset PKG_CONFIG_LIBDIR

    # The pinned version is broken, and upstream does not react
    cargo update cc

    export "AR_${FFBUILD_RUST_TARGET//-/_}"="${AR}"
    export "RANLIB_${FFBUILD_RUST_TARGET//-/_}"="${RANLIB}"
    export "NM_${FFBUILD_RUST_TARGET//-/_}"="${NM}"
    export "LD_${FFBUILD_RUST_TARGET//-/_}"="${LD}"
    export "CC_${FFBUILD_RUST_TARGET//-/_}"="${CC}"
    export "CXX_${FFBUILD_RUST_TARGET//-/_}"="${CXX}"
    export "LD_${FFBUILD_RUST_TARGET//-/_}"="${LD}"
    export "CFLAGS_${FFBUILD_RUST_TARGET//-/_}"="${CFLAGS}"
    export "CXXFLAGS_${FFBUILD_RUST_TARGET//-/_}"="${CXXFLAGS}"
    export "LDFLAGS_${FFBUILD_RUST_TARGET//-/_}"="${LDFLAGS}"
    unset AR RANLIB NM CC CXX LD CFLAGS CXXFLAGS LDFLAGS

    cargo cinstall -v "${myconf[@]}"

    shopt -s nullglob
    local ravlibs=( "${FFBUILD_DESTPREFIX}"/lib/*rav1e* )
    shopt -u nullglob
    if [[ ${#ravlibs[@]} -eq 0 ]]; then
        echo "[ERROR] rav1e 安装后未找到 ${FFBUILD_DESTPREFIX}/lib/*rav1e*" >&2
        return 1
    fi
    chmod 644 "${ravlibs[@]}"
}

ffbuild_configure() {
    echo --enable-librav1e
}

ffbuild_unconfigure() {
    echo --disable-librav1e
}
