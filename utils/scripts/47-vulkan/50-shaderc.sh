#!/bin/bash

SCRIPT_REPO="https://github.com/google/shaderc.git"
SCRIPT_COMMIT="49a8724d561c13db22b52f99f2a0e2707a9a9e3c"

ffbuild_enabled() {
    (( $(ffbuild_ffver) > 404 )) || return -1
    return 0
}

ffbuild_dl() {
    default_dl .
    echo "./utils/git-sync-deps || exit $?"
}

ffbuild_build() {
    # GCC 8: std::filesystem → -lstdc++fs. Must appear AFTER static libs on the
    # link line (GNU ld left-to-right). CMAKE_EXE_LINKER_FLAGS / LDFLAGS put it
    # too early — use CMAKE_CXX_STANDARD_LIBRARIES (end of link) instead.
    local fs_ldflags=""
    local gcc_maj
    gcc_maj="$(${CXX:-g++} -dumpversion 2>/dev/null | cut -d. -f1 || true)"
    if [[ -z "$gcc_maj" ]]; then
        gcc_maj="$(g++ -dumpversion 2>/dev/null | cut -d. -f1 || true)"
    fi
    if [[ -n "$gcc_maj" && "$gcc_maj" -lt 9 ]]; then
        fs_ldflags="-lstdc++fs"
        echo "[INFO] GCC ${gcc_maj}: shaderc/glslc 链接末尾附加 -lstdc++fs"
    fi

    rm -rf build
    mkdir build && cd build

    # Main pass: libraries + .pc only. Skip glslc here (built in native_build).
    # SHADERC_SKIP_EXECUTABLES avoids the GCC8 filesystem link failure on glslc.
    cmake -GNinja -DCMAKE_TOOLCHAIN_FILE="$FFBUILD_CMAKE_TOOLCHAIN" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$FFBUILD_PREFIX" \
        -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON \
        -DSHADERC_SKIP_EXECUTABLES=ON \
        -DENABLE_EXCEPTIONS=ON -DENABLE_GLSLANG_BINARIES=OFF -DSPIRV_SKIP_EXECUTABLES=ON \
        -DSPIRV_TOOLS_BUILD_STATIC=ON -DBUILD_SHARED_LIBS=OFF \
        ..
    ninja -j$(nproc)

    # Stage into a temp tree, then copy (not hardlink) into FFBUILD_DESTDIR.
    # cp -al + rm staging is unsafe across filesystems and can leave empty installs.
    local staging
    staging="$(mktemp -d "${TMPDIR:-/tmp}/shaderc-stage.XXXXXX")"
    export DESTDIR="$staging"
    ninja install

    if [[ $TARGET == win* ]]; then
        rm -rf "${DESTDIR}${FFBUILD_PREFIX}"/bin
        rm -f "${DESTDIR}${FFBUILD_PREFIX}"/lib/*.dll.a
    elif [[ $TARGET == linux* ]]; then
        rm -rf "${DESTDIR}${FFBUILD_PREFIX}"/bin
        rm -f "${DESTDIR}${FFBUILD_PREFIX}"/lib/*.so*
    else
        echo "Unknown target"
        rm -rf "$staging"
        return -1
    fi

    mkdir -p "$FFBUILD_DESTDIR"
    cp -a "$DESTDIR"/. "$FFBUILD_DESTDIR"/
    rm -rf "$staging"
    unset DESTDIR

    mkdir -p "$FFBUILD_DESTPREFIX"/lib/pkgconfig

    # for some reason, this does not get installed...
    cp -f libshaderc_util/libshaderc_util.a "$FFBUILD_DESTPREFIX"/lib/

    if [[ ! -f "$FFBUILD_DESTPREFIX"/lib/pkgconfig/shaderc_combined.pc ]]; then
        echo "[ERROR] shaderc_combined.pc 未安装到 ${FFBUILD_DESTPREFIX}/lib/pkgconfig" >&2
        return 1
    fi

    echo "Libs: -lstdc++ ${fs_ldflags}" >> "$FFBUILD_DESTPREFIX"/lib/pkgconfig/shaderc_combined.pc
    if [[ -f "$FFBUILD_DESTPREFIX"/lib/pkgconfig/shaderc_static.pc ]]; then
        echo "Libs: -lstdc++ ${fs_ldflags}" >> "$FFBUILD_DESTPREFIX"/lib/pkgconfig/shaderc_static.pc
    fi

    cp -f "$FFBUILD_DESTPREFIX"/lib/pkgconfig/shaderc_combined.pc \
        "$FFBUILD_DESTPREFIX"/lib/pkgconfig/shaderc.pc

    rm -rf ../native_build
    mkdir ../native_build && cd ../native_build

    unset CC CXX CFLAGS CXXFLAGS LD LDFLAGS AR RANLIB NM DLLTOOL PKG_CONFIG_LIBDIR LIBS
    # Put -lstdc++fs at link END (after .a). Do not use LDFLAGS / EXE_LINKER_FLAGS.
    local -a native_cmake=(
        -GNinja
        -DCMAKE_BUILD_TYPE=Release
        -DSHADERC_SKIP_TESTS=ON
        -DSHADERC_SKIP_EXAMPLES=ON
        -DSHADERC_SKIP_COPYRIGHT_CHECK=ON
        -DENABLE_EXCEPTIONS=ON
        -DSPIRV_TOOLS_BUILD_STATIC=ON
        -DBUILD_SHARED_LIBS=OFF
    )
    gcc_maj="$(g++ -dumpversion 2>/dev/null | cut -d. -f1 || true)"
    if [[ -n "$gcc_maj" && "$gcc_maj" -lt 9 ]]; then
        # Keep default -lstdc++/-lm; only append filesystem.
        native_cmake+=( -DCMAKE_CXX_STANDARD_LIBRARIES="-lstdc++ -lm -lstdc++fs" )
        echo "[INFO] GCC ${gcc_maj}: native glslc 链接末尾 -lstdc++fs (CXX_STANDARD_LIBRARIES)"
    fi
    cmake "${native_cmake[@]}" ..
    if ! ninja -j$(nproc) glslc/glslc; then
        echo "[WARN] glslc cmake 链接失败，改用 g++ --start-group 重链" >&2
        g++ -O3 -DNDEBUG -o glslc/glslc \
            glslc/CMakeFiles/glslc_exe.dir/src/main.cc.o \
            -Wl,--start-group \
            glslc/libglslc.a \
            libshaderc_util/libshaderc_util.a \
            libshaderc/libshaderc.a \
            libshaderc_util/libshaderc_util.a \
            third_party/glslang/SPIRV/libSPIRV.a \
            third_party/glslang/glslang/libglslang.a \
            third_party/spirv-tools/source/opt/libSPIRV-Tools-opt.a \
            third_party/spirv-tools/source/libSPIRV-Tools.a \
            -lrt -lstdc++fs \
            -Wl,--end-group
    fi

    # Docker 镜像写 /opt/glslc；本机优先写 LOCAL_BIN，并尽量落到 /opt/glslc
    if [[ -n "${LOCAL_BIN:-}" ]]; then
        mkdir -p "$LOCAL_BIN"
        cp glslc/glslc "${LOCAL_BIN}/glslc"
        chmod +x "${LOCAL_BIN}/glslc"
    fi
    if [[ -w /opt ]] || [[ -w /opt/glslc ]] 2>/dev/null; then
        cp glslc/glslc /opt/glslc
    elif command -v sudo &>/dev/null && sudo -n true 2>/dev/null; then
        sudo cp glslc/glslc /opt/glslc
    elif [[ -n "${LOCAL_BIN:-}" ]]; then
        echo "[WARN] 无法写入 /opt/glslc，已安装到 ${LOCAL_BIN}/glslc（请确保 PATH 含 LOCAL_BIN）"
    else
        cp glslc/glslc /opt/glslc
    fi
}

ffbuild_configure() {
    echo --enable-libshaderc
}

ffbuild_unconfigure() {
    (( $(ffbuild_ffver) > 404 )) || return 0
    echo --disable-libshaderc
}
