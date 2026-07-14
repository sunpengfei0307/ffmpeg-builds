#!/bin/bash

SCRIPT_REPO="https://github.com/FFTW/fftw3.git"
SCRIPT_COMMIT="93ed4c786934aec9946f8dda4b4e3eb08f8be41c"

# Official release (pre-generated codelets). Prefer this on CentOS/RHEL 8 where
# ocaml-num is often missing even if ocamlbuild is installed.
FFTW_RELEASE_VER="3.3.10"
FFTW_RELEASE_URLS=(
    "https://www.fftw.org/fftw-${FFTW_RELEASE_VER}.tar.gz"
    "https://fftw.org/fftw-${FFTW_RELEASE_VER}.tar.gz"
    "https://github.com/FFTW/fftw3/releases/download/fftw-${FFTW_RELEASE_VER}/fftw-${FFTW_RELEASE_VER}.tar.gz"
)

ffbuild_enabled() {
    return 0
}

_fftw_have_codelets() {
    [[ -f dft/scalar/codelets/n1_2.c ]]
}

# Full OCaml toolchain needed to generate codelets from git (incl. findlib package num).
_fftw_have_ocaml() {
    command -v ocamlbuild &>/dev/null || return 1
    command -v ocamlfind &>/dev/null || return 1
    ocamlfind query num &>/dev/null || return 1
    return 0
}

# Download + extract official tarball into current stage dir (overwrite git tree).
_fftw_use_release_tarball() {
    local url ok=0 tmp
    echo "[INFO] FFTW: 使用官方 release ${FFTW_RELEASE_VER}（无需 OCaml / ocaml-num）"
    tmp="$(mktemp -d)"
    for url in "${FFTW_RELEASE_URLS[@]}"; do
        echo "[INFO] 下载: $url"
        if curl -fsSL --connect-timeout 30 "$url" -o "$tmp/fftw.tgz" \
            || wget -q -T 30 "$url" -O "$tmp/fftw.tgz"; then
            ok=1
            break
        fi
        echo "[WARN] 下载失败: $url"
    done
    [[ "$ok" -eq 1 ]] || { echo "[ERROR] FFTW release 下载失败" >&2; rm -rf "$tmp"; return 1; }
    tar -xzf "$tmp/fftw.tgz" -C "$tmp"
    local srcdir
    srcdir="$(find "$tmp" -maxdepth 1 -type d -name 'fftw-*' | head -n1)"
    [[ -n "$srcdir" && -d "$srcdir" ]] || { echo "[ERROR] tarball 内无 fftw-* 目录" >&2; rm -rf "$tmp"; return 1; }
    find . -mindepth 1 -maxdepth 1 -exec rm -rf {} +
    cp -a "$srcdir"/. .
    rm -rf "$tmp"
    _fftw_have_codelets || { echo "[ERROR] release 包仍缺少 n1_2.c" >&2; return 1; }
}

ffbuild_build() {
    local myconf=(
        --prefix="$FFBUILD_PREFIX"
        --disable-shared
        --enable-static
        --disable-fortran
        --disable-doc
        --with-our-malloc
        --enable-threads
        --with-combined-threads
        --with-incoming-stack-boundary=2
    )

    if [[ $TARGET != *arm64 ]]; then
        myconf+=(
            --enable-sse2
            --enable-avx
            --enable-avx2
        )
    fi

    if [[ $TARGET == win* || $TARGET == linux* ]]; then
        myconf+=(
            --host="$FFBUILD_TOOLCHAIN"
        )
    else
        echo "Unknown target"
        return -1
    fi

    # Prefer release tarball unless codelets already present OR user forces git
    # with a complete OCaml+num toolchain (FFTW_USE_GIT=1).
    local use_git_bootstrap=0
    if _fftw_have_codelets; then
        echo "[INFO] FFTW: 已有预生成 codelets，直接 configure/make"
        use_git_bootstrap=0
    elif [[ "${FFTW_USE_GIT:-}" == "1" ]] && _fftw_have_ocaml; then
        echo "[INFO] FFTW: FFTW_USE_GIT=1 且 OCaml+num 齐全，走 git bootstrap"
        use_git_bootstrap=1
    else
        if [[ "${FFTW_USE_GIT:-}" == "1" ]]; then
            echo "[WARN] FFTW_USE_GIT=1 但 ocamlfind 无 num 包（需 ocaml-num / ocaml-num-devel），改用 release tarball"
        elif command -v ocamlbuild &>/dev/null && ! ocamlfind query num &>/dev/null; then
            echo "[WARN] 已装 ocamlbuild 但缺 findlib 包 num，改用 release tarball"
        fi
        _fftw_use_release_tarball || return 1
        use_git_bootstrap=0
    fi

    if [[ "$use_git_bootstrap" -eq 1 ]]; then
        sed -i 's/-libs nums/-use-ocamlfind -package num/' genfft/Makefile.am
        sed -i 's/-pkgs num/-use-ocamlfind -package num/' genfft/Makefile.am || true
        sed -i 's/windows.h/process.h/' configure.ac
        myconf+=(--enable-maintainer-mode)
        ./bootstrap.sh "${myconf[@]}"
    else
        if [[ -f configure.ac ]] && grep -q 'windows.h' configure.ac 2>/dev/null; then
            sed -i 's/windows.h/process.h/' configure.ac || true
        fi
        if [[ ! -x ./configure ]]; then
            echo "[ERROR] release 树缺少可执行 configure" >&2
            return 1
        fi
        ./configure "${myconf[@]}"
    fi

    make -j"$(nproc)"
    make install DESTDIR="$FFBUILD_DESTDIR"
}