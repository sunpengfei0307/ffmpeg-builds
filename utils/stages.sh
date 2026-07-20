#!/bin/bash
# Self-contained FFmpeg local-build orchestration.
# Sourced by build.sh after utils/logger.sh. Defines functions only (no side effects on source).
#
# Layout (ROOT_DIR = project root):
#   UTILS_DIR / WORK_DIR     = $ROOT_DIR/utils
#   SCRIPTS_D                = $UTILS_DIR/scripts
#   DL_CACHE                 = $UTILS_DIR/downloads
#   HOST_BIN_SRC             = $UTILS_DIR/tools
#   FFMPEG_DIR               = $ROOT_DIR/ffmpeg/ffmpeg-${ADDINS_STR}（如 ffmpeg-8.1.2）
#   BUILD_DIR                = $ROOT_DIR/build
#   STAGES_DIR               = $BUILD_DIR/stages          (*.done only)
#   CACHE_DIR                = $BUILD_DIR/caches          (extract/build workdirs)
#   USR_DIR / FFBUILD_PREFIX = $BUILD_DIR/release
#   LOCAL_BIN                = $BUILD_DIR/tools/bin
#   LOCAL_OPT                = $BUILD_DIR/tools/opt
#
# Local-only build (no Docker). CentOS 8 / bash 4.4+.

###############################################################################
# 1. Paths
###############################################################################

_paths_init() {
  : "${ROOT_DIR:?ROOT_DIR must be set}"

  export UTILS_DIR="${ROOT_DIR}/utils"
  export WORK_DIR="$UTILS_DIR"
  export SCRIPTS_D="${UTILS_DIR}/scripts"
  export DL_CACHE="${UTILS_DIR}/downloads"
  export HOST_BIN_SRC="${UTILS_DIR}/tools"

  # ADDINS_STR 即版本号，须与目录 ffmpeg-<ver> 中 - 后字符串完全一致（不可省略补丁号）
  local _ffver="${ADDINS_STR:-8.1.2}"
  export FFMPEG_DIR="${ROOT_DIR}/ffmpeg/ffmpeg-${_ffver}"
  export BUILD_DIR="${ROOT_DIR}/build"
  export STAGES_DIR="${BUILD_DIR}/stages"
  export CACHE_DIR="${BUILD_DIR}/caches"

  export USR_DIR="${BUILD_DIR}/release"
  export FFBUILD_PREFIX="$USR_DIR"
  # 直装到绝对 prefix：DESTDIR=/ 满足 scripts 里 ${FFBUILD_DESTDIR:?}，
  # 且 / + $FFBUILD_PREFIX == $FFBUILD_PREFIX（无独立 ffdest 目录）。
  export FFBUILD_DESTDIR="/"
  export FFBUILD_DESTPREFIX="$FFBUILD_PREFIX"

  export LOCAL_ROOT="$BUILD_DIR"
  export LOCAL_BIN="${BUILD_DIR}/tools/bin"
  export LOCAL_OPT="${BUILD_DIR}/tools/opt"
  export CT_PREFIX="${LOCAL_OPT}/ct-ng"
  export TOOLCHAIN_CMAKE="${BUILD_DIR}/tools/toolchain.cmake"
  export CROSS_MESON="${BUILD_DIR}/tools/cross.meson"
  export SRC_DIR="$CACHE_DIR"
  export OUTPUT_DIR="$USR_DIR"
  export CARGO_HOME="${LOCAL_OPT}/cargo"
  export RUSTUP_HOME="${LOCAL_OPT}/rustup"

  mkdir -p "$SCRIPTS_D" "$DL_CACHE" "$HOST_BIN_SRC" \
           "${ROOT_DIR}/ffmpeg" \
           "$USR_DIR"/{bin,lib,include,share} \
           "$STAGES_DIR" "$CACHE_DIR" \
           "$LOCAL_BIN" "$LOCAL_OPT" "$CT_PREFIX" \
           "$CARGO_HOME" "$RUSTUP_HOME" \
           "$(dirname "$TOOLCHAIN_CMAKE")"

  export PATH="${LOCAL_BIN}:${HOME}/.local/bin:${PATH}"
  export PKG_CONFIG_LIBDIR="${USR_DIR}/lib/pkgconfig:${USR_DIR}/share/pkgconfig"
  export PKG_CONFIG_PATH="$PKG_CONFIG_LIBDIR"
  export PKG_CONFIG=pkg-config
}

###############################################################################
# FF vars / script API stubs
###############################################################################

_init_ff_vars() {
  export TARGET="${TARGET:-linux64}"
  export VARIANT="${VARIANT:-nonfree}"
  export ADDINS_STR="${ADDINS_STR:-8.1.2}"
  export ADDINS="${ADDINS:-$ADDINS_STR}"

  ffbuild_ffver() {
    case "$ADDINS_STR" in
      *6.0*) echo 600 ;;
      *6.1*) echo 601 ;;
      *7.0*) echo 700 ;;
      *7.1*) echo 701 ;;
      *8.0*) echo 800 ;;
      *8.1*) echo 801 ;;
      *9.0*) echo 900 ;;
      *)     echo 99999999 ;;
    esac
  }

  FFVER="$(ffbuild_ffver)"
  export FFVER

  # 由 ADDINS_STR 推导 git 引用：8.1.2 → tag n8.1.2；仅 8.1 → branch release/8.1
  _ffmpeg_git_ref() {
    local v="${ADDINS_STR}"
    if [[ "$v" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
      echo "n${v}"
    elif [[ "$v" =~ ^[0-9]+\.[0-9]+$ ]]; then
      echo "release/${v}"
    else
      echo "n8.1.2"
    fi
  }
  export GIT_BRANCH="${GIT_BRANCH:-$(_ffmpeg_git_ref)}"

  # Default stubs — scripts override when sourced
  ffbuild_depends() { :; }
  ffbuild_configure() { return 0; }
  ffbuild_unconfigure() { return 0; }
  ffbuild_cflags() { return 0; }
  ffbuild_uncflags() { return 0; }
  ffbuild_cxxflags() { return 0; }
  ffbuild_uncxxflags() { return 0; }
  ffbuild_ldexeflags() { return 0; }
  ffbuild_unldexeflags() { return 0; }
  ffbuild_ldflags() { return 0; }
  ffbuild_unldflags() { return 0; }
  ffbuild_libs() { return 0; }
  ffbuild_unlibs() { return 0; }
  ffbuild_enabled() { return 0; }

  export LICENSE_FILE="${LICENSE_FILE:-COPYING.LGPLv2.1}"
  export REPO="${REPO:-local/ffmpeg-builds}"
}

###############################################################################
# Download recipe helpers (scripts may override ffbuild_dl)
###############################################################################

default_dl() {
  echo "git-mini-clone \"$SCRIPT_REPO\" \"$SCRIPT_COMMIT\" \"$1\""
}

ffbuild_dl() {
  default_dl .
}

###############################################################################
# 2. Host deps
###############################################################################

_install_from_source() {
  local name="$1" dirname="$2"
  shift 2
  if [[ "${FORCE_HOST_SRC:-}" != "1" ]] && command -v "$name" &>/dev/null; then
    return 0
  fi
  local urls=() conf_args=()
  while [[ $# -gt 0 ]]; do
    if [[ "$1" == "--" ]]; then shift; conf_args=("$@"); break; fi
    urls+=("$1"); shift
  done
  _log "源码安装 $name → ${LOCAL_BIN}"
  (
    set -euo pipefail
    export CC=gcc CXX=g++ LD=ld AR=ar RANLIB=ranlib NM=nm
    unset CROSS_COMPILE FFBUILD_CROSS_PREFIX CFLAGS CXXFLAGS LDFLAGS || true
    local tmp prefix url ok=0
    tmp="$(mktemp -d)"
    prefix="${LOCAL_OPT}/${name}"
    trap 'rm -rf -- "$tmp"' EXIT
    cd "$tmp"
    for url in "${urls[@]}"; do
      _log "下载: $url"
      if curl -fsSL --connect-timeout 20 "$url" -o src.tgz || wget -q -T 20 "$url" -O src.tgz; then
        ok=1; break
      fi
    done
    [[ "$ok" -eq 1 ]] || exit 1
    tar -xf src.tgz
    cd "$dirname"
    ./configure --prefix="$prefix" "${conf_args[@]+"${conf_args[@]}"}"
    make -j"$(nproc)"
    make install
    mkdir -p "$LOCAL_BIN"
    local f
    for f in "$prefix"/bin/*; do
      [[ -e "$f" ]] || continue
      ln -sfn "$f" "${LOCAL_BIN}/$(basename "$f")"
    done
  ) >>"$BUILD_LOG" 2>&1
}

_setup_aclocal_path() {
  local -a dirs=()
  local d joined="" seen=":"
  # xorg-macros 等安装到 FFBUILD_PREFIX/share/aclocal
  if [[ -n "${FFBUILD_PREFIX:-}" ]]; then
    mkdir -p "${FFBUILD_PREFIX}/share/aclocal"
    dirs+=("${FFBUILD_PREFIX}/share/aclocal")
  fi
  shopt -s nullglob
  for d in "${LOCAL_OPT}"/*/share/aclocal /usr/share/aclocal /usr/local/share/aclocal; do
    [[ -d "$d" ]] || continue
    dirs+=("$d")
  done
  shopt -u nullglob
  [[ ${#dirs[@]} -eq 0 ]] && return 0
  for d in "${dirs[@]}"; do
    [[ "$seen" == *":$d:"* ]] && continue
    seen+="${d}:"
    [[ -n "$joined" ]] && joined+=":"
    joined+="$d"
  done
  export ACLOCAL_PATH="$joined"
}

_meson_ge() {
  local need_maj="$1" need_min="$2"
  local ver maj min
  ver="$(meson --version 2>/dev/null | head -n1 | grep -oE '[0-9]+\.[0-9]+' | head -n1 || true)"
  [[ -n "$ver" ]] || return 1
  maj="${ver%%.*}"
  min="${ver#*.}"
  if [[ "$maj" -gt "$need_maj" ]]; then return 0; fi
  if [[ "$maj" -eq "$need_maj" && "$min" -ge "$need_min" ]]; then return 0; fi
  return 1
}

_cmd_works() {
  local c="$1"
  command -v "$c" &>/dev/null || return 1
  "$c" --version &>/dev/null || "$c" -h &>/dev/null || return 1
  return 0
}

_scrub_broken_local_wrappers() {
  local f
  mkdir -p "$LOCAL_BIN"
  for f in meson ninja python3 python pip3; do
    [[ -e "${LOCAL_BIN}/$f" || -L "${LOCAL_BIN}/$f" ]] || continue
    if ! "${LOCAL_BIN}/$f" --version &>/dev/null && ! "${LOCAL_BIN}/$f" -h &>/dev/null; then
      _info "remove broken wrapper: ${LOCAL_BIN}/$f"
      rm -f "${LOCAL_BIN}/$f"
    fi
  done
}

_find_working_meson() {
  local cand
  local -a cands=()
  command -v meson &>/dev/null && cands+=("$(command -v meson)")
  [[ -x /usr/local/bin/meson ]] && cands+=("/usr/local/bin/meson")
  [[ -x /usr/bin/meson ]] && cands+=("/usr/bin/meson")
  [[ -x "${HOME}/.local/bin/meson" ]] && cands+=("${HOME}/.local/bin/meson")
  for cand in "${cands[@]+"${cands[@]}"}"; do
    [[ -n "$cand" && -e "$cand" ]] || continue
    [[ "$cand" == "${LOCAL_BIN}/meson" ]] && continue
    if "$cand" --version &>/dev/null; then
      echo "$cand"
      return 0
    fi
  done
  return 1
}

_python_candidates() {
  local cand
  if [[ -n "${FFBUILD_PYTHON:-}" ]]; then
    echo "$FFBUILD_PYTHON"
  fi
  if [[ -n "${PYTHON:-}" ]]; then
    echo "$PYTHON"
  fi
  for cand in \
    python3.14 python314 \
    python3.13 python313 \
    python3.12 python312 \
    python3.11 python311 \
    python3.10 python310 \
    python3.9 python39 \
    python3.8 python38 \
    python3; do
    echo "$cand"
  done
}

_find_modern_python() {
  local cand py need="${1:-10}"
  for cand in $(_python_candidates); do
    [[ -n "$cand" ]] || continue
    command -v "$cand" &>/dev/null || continue
    py="$(command -v "$cand")"
    [[ "$py" == "${LOCAL_BIN}/python3" || "$py" == "${LOCAL_BIN}/python" ]] && continue
    if "$py" -c "import sys; raise SystemExit(0 if sys.version_info[:2] >= (3, ${need}) else 1)" 2>/dev/null; then
      echo "$py"
      return 0
    fi
  done
  return 1
}

_write_meson_wrapper() {
  local py="$1"
  mkdir -p "$LOCAL_BIN"
  cat > "${LOCAL_BIN}/meson" <<EOF
#!/bin/bash
exec "$py" -m mesonbuild.mesonmain "\$@"
EOF
  chmod +x "${LOCAL_BIN}/meson"
}

_link_python_tools() {
  local py="$1"
  local user_base ninja_bin meson_sys
  mkdir -p "$LOCAL_BIN"
  user_base="$("$py" -m site --user-base 2>/dev/null || true)"
  export PATH="${user_base:+${user_base}/bin:}${LOCAL_BIN}:${HOME}/.local/bin:${PATH}"

  ln -sfn "$py" "${LOCAL_BIN}/python3"
  ln -sfn "$py" "${LOCAL_BIN}/python"
  if "$py" -m pip --version &>/dev/null; then
    printf '#!/bin/bash\nexec "%s" -m pip "$@"\n' "$py" > "${LOCAL_BIN}/pip3"
    chmod +x "${LOCAL_BIN}/pip3"
  fi

  if "$py" -c "import mesonbuild" 2>/dev/null; then
    _write_meson_wrapper "$py"
  else
    meson_sys="$(_find_working_meson || true)"
    if [[ -n "$meson_sys" ]]; then
      ln -sfn "$meson_sys" "${LOCAL_BIN}/meson"
    else
      _write_meson_wrapper "$py"
    fi
  fi

  ninja_bin="$(command -v ninja 2>/dev/null || true)"
  [[ -z "$ninja_bin" && -n "$user_base" && -x "${user_base}/bin/ninja" ]] && ninja_bin="${user_base}/bin/ninja"
  [[ -z "$ninja_bin" && -x /usr/bin/ninja ]] && ninja_bin="/usr/bin/ninja"
  if [[ -n "$ninja_bin" && "$ninja_bin" != "${LOCAL_BIN}/ninja" ]]; then
    ln -sfn "$ninja_bin" "${LOCAL_BIN}/ninja"
  fi

  export PATH="${LOCAL_BIN}:${HOME}/.local/bin:${PATH}"
  export FFBUILD_PYTHON="$py"
  hash -r 2>/dev/null || true
}

_install_python38_module() {
  _info "dnf module install Python 3.8 (fallback; prefer Python 3.14+)"
  _run "dnf-module-python38" bash -c '
    '"$SUDO"' dnf module reset -y python38 2>/dev/null || true
    '"$SUDO"' dnf module enable -y python38:3.8 2>/dev/null \
      || '"$SUDO"' dnf module enable -y python38 2>/dev/null || true
    '"$SUDO"' dnf module install -y python38 2>/dev/null \
      || '"$SUDO"' dnf module install -y python38:3.8/common 2>/dev/null \
      || '"$SUDO"' dnf install -y @python38 2>/dev/null || true
    '"$SUDO"' dnf install -y python38-pip python38-devel 2>/dev/null || true
  '
}

_ensure_python_build_modules() {
  local py cand
  local -a pys=()
  for cand in "${FFBUILD_PYTHON:-}" python3 python3.14 python314 python3.8 python38 python3.6; do
    [[ -n "$cand" ]] || continue
    command -v "$cand" &>/dev/null || continue
    py="$(command -v "$cand")"
    local seen=0 p
    for p in "${pys[@]+"${pys[@]}"}"; do
      [[ "$p" == "$py" ]] && { seen=1; break; }
    done
    [[ "$seen" -eq 1 ]] && continue
    pys+=("$py")
  done
  [[ ${#pys[@]} -eq 0 ]] && return 0

  local ok=0
  for py in "${pys[@]}"; do
    if "$py" -c "import jsonschema" 2>/dev/null; then
      _ok "jsonschema OK: $py"
      ok=1
      continue
    fi
    _info "pip install jsonschema for $py"
    "$py" -m ensurepip --upgrade >>"$BUILD_LOG" 2>&1 || true
    _run "pip-jsonschema-$(basename "$py")" "$py" -m pip install --user -U "jsonschema" "jinja2" "markupsafe" || \
      _run "pip-jsonschema2-$(basename "$py")" "$py" -m pip install -U "jsonschema" "jinja2" "markupsafe" || true
    if "$py" -c "import jsonschema" 2>/dev/null; then
      _ok "jsonschema installed: $py"
      ok=1
    fi
  done
  [[ "$ok" -eq 1 ]] || _info "jsonschema not confirmed; mbedtls will retry or GEN_FILES=OFF"
}

_ensure_modern_meson() {
  mkdir -p "$LOCAL_BIN"
  _scrub_broken_local_wrappers

  export PATH="/usr/local/bin:/usr/bin:${HOME}/.local/bin:${PATH//${LOCAL_BIN}:/}"

  local meson_sys=""
  meson_sys="$(_find_working_meson || true)"
  if [[ -n "$meson_sys" ]]; then
    local ver
    ver="$("$meson_sys" --version 2>/dev/null | head -n1 || true)"
    _info "found working meson: $meson_sys ($ver)"
    if "$meson_sys" --version &>/dev/null; then
      local maj min
      maj="$(echo "$ver" | grep -oE '[0-9]+' | head -n1)"
      min="$(echo "$ver" | grep -oE '[0-9]+\.[0-9]+' | head -n1 | cut -d. -f2)"
      if [[ -n "$maj" && ( "$maj" -gt 0 || ( "$maj" -eq 0 && "${min:-0}" -ge 64 ) || "$maj" -ge 1 ) ]]; then
        ln -sfn "$meson_sys" "${LOCAL_BIN}/meson"
      fi
    fi
  fi

  local py=""
  py="$(_find_modern_python 10 || true)"
  if [[ -z "$py" ]]; then
    py="$(_find_modern_python 7 || true)"
  fi
  if [[ -z "$py" ]]; then
    _install_python38_module
    py="$(_find_modern_python 7 || true)"
  fi
  if [[ -z "$py" ]]; then
    export PATH="${LOCAL_BIN}:/usr/local/bin:/usr/bin:${HOME}/.local/bin:${PATH}"
    hash -r 2>/dev/null || true
    if _meson_ge 0 64; then
      _ok "meson OK (system): $(meson --version | head -n1)"
      return 0
    fi
    _fail "No Python>=3.7 and no working meson. Install Python 3.14+ or meson."
    return 1
  fi

  local pyver
  pyver="$("$py" -c 'import sys; print("%d.%d.%d"%sys.version_info[:3])')"
  _info "host Python: $py ($pyver)"

  export PATH="${LOCAL_BIN}:/usr/local/bin:/usr/bin:${HOME}/.local/bin:${PATH}"
  _scrub_broken_local_wrappers

  "$py" -m ensurepip --upgrade >>"$BUILD_LOG" 2>&1 || true
  _run "pip-upgrade" "$py" -m pip install --user -U "pip" "setuptools" "wheel" || \
    _run "pip-upgrade2" "$py" -m pip install -U "pip" "setuptools" "wheel" || true

  if ! _run "pip-meson" "$py" -m pip install --user -U "meson>=1.2.0"; then
    _run "pip-meson2" "$py" -m pip install -U "meson>=1.2.0" || true
  fi

  if ! _run "pip-ninja" "$py" -m pip install --user -U "ninja"; then
    _run "dnf-ninja" "$SUDO" dnf install -y ninja-build 2>/dev/null || true
    _run "pip-ninja-wheel" "$py" -m pip install --user -U --only-binary=:all: ninja || \
      _run "pip-ninja-old" "$py" -m pip install --user -U "ninja==1.11.1" || true
  fi

  _link_python_tools "$py"
  _scrub_broken_local_wrappers
  export PATH="${LOCAL_BIN}:/usr/local/bin:/usr/bin:${HOME}/.local/bin:${PATH}"
  hash -r 2>/dev/null || true

  if ! meson --version &>/dev/null; then
    meson_sys="$(_find_working_meson || true)"
    if [[ -n "$meson_sys" ]]; then
      _info "fallback link system meson: $meson_sys"
      ln -sfn "$meson_sys" "${LOCAL_BIN}/meson"
      hash -r 2>/dev/null || true
    fi
  fi

  if ! _meson_ge 0 64; then
    _fail "Meson still < 0.64 (now: $(meson --version 2>&1 | head -n1))"
    return 1
  fi
  _ok "meson: $(meson --version | head -n1) ; python: $py"
  _ok "ninja: $(ninja --version 2>/dev/null || echo missing)"
}

_copy_host_bin_tools() {
  local f name
  mkdir -p "$LOCAL_BIN"
  [[ -d "$HOST_BIN_SRC" ]] || return 0
  shopt -s nullglob
  for f in "${HOST_BIN_SRC}"/*; do
    [[ -f "$f" ]] || continue
    name="$(basename "$f")"
    cp -f "$f" "${LOCAL_BIN}/${name}"
    chmod +x "${LOCAL_BIN}/${name}"
  done
  shopt -u nullglob
  _ok "tools → ${LOCAL_BIN}"
}

stage_host_deps() {
  SUDO=""
  [[ ${EUID:-0} -ne 0 ]] && SUDO="sudo"
  export SUDO

  if grep -qE "CentOS.*(release )?8" /etc/redhat-release 2>/dev/null; then
    _run "vault-repos" bash -c '
      shopt -s nullglob
      for rf in /etc/yum.repos.d/CentOS-*.repo /etc/yum.repos.d/CentOS-Linux-*.repo; do
        [[ -f "$rf" ]] || continue
        '"$SUDO"' sed -i -e "s/^mirrorlist=/#mirrorlist=/g" \
          -e "s|^#\?baseurl=http://mirror.centos.org|baseurl=http://vault.centos.org|g" \
          -e "s|^baseurl=http://mirror.centos.org|baseurl=http://vault.centos.org|g" "$rf" || true
      done
      '"$SUDO"' tee /etc/yum.repos.d/CentOS-PowerTools-Vault.repo >/dev/null <<EOF
[powertools-vault]
name=CentOS-8 - PowerTools (Vault)
baseurl=http://vault.centos.org/centos/8/PowerTools/\$basearch/os/
gpgcheck=0
enabled=1
EOF
      '"$SUDO"' dnf clean all -q 2>/dev/null || true
    '
  fi

  _run "enable-repos" bash -c '
    '"$SUDO"' dnf install -y dnf-plugins-core 2>/dev/null || true
    rpm -q epel-release &>/dev/null || '"$SUDO"' dnf install -y epel-release 2>/dev/null || true
    for repo in powertools PowerTools powertools-vault crb; do
      '"$SUDO"' dnf config-manager --set-enabled "$repo" 2>/dev/null || true
    done
    '"$SUDO"' dnf makecache 2>/dev/null || true
  '

  local base_pkgs=(
    git subversion curl wget xz unzip zip tar which
    gcc gcc-c++ make cmake autoconf automake libtool pkgconf
    bison flex gawk python3 python3-pip python3-devel
    patch diffutils rsync
    openssl-devel zlib-devel bzip2 bzip2-devel libstdc++-devel libstdc++-static
    ncurses-devel gettext perl nasm yasm gperf texinfo help2man ninja-build indent
    clang
    ocaml ocaml-findlib ocaml-ocamlbuild ocaml-num ocaml-num-devel
  )

  _run "dnf-pkgs" "$SUDO" dnf install -y "${base_pkgs[@]}" || \
    _run "dnf-pkgs-soft" bash -c 'for p in '"${base_pkgs[*]}"'; do '"$SUDO"' dnf install -y "$p" 2>/dev/null || true; done'

  export PATH="${LOCAL_BIN}:${HOME}/.local/bin:${PATH}"

  _ensure_modern_meson || return 1
  _ensure_python_build_modules

  command -v nasm &>/dev/null || _install_from_source nasm "nasm-2.16.03" \
    "https://www.nasm.us/pub/nasm/releasebuilds/2.16.03/nasm-2.16.03.tar.xz" || true

  if ! command -v aclocal-1.18 &>/dev/null; then
    _install_from_source aclocal-1.18 "automake-1.18.1" \
      "https://ftp.gnu.org/gnu/automake/automake-1.18.1.tar.xz" \
      "https://mirrors.kernel.org/gnu/automake/automake-1.18.1.tar.xz" || {
      _fail "automake-1.18"; return 1; }
  fi
  local ac_ver
  ac_ver="$(autoconf --version 2>/dev/null | head -n1 | grep -oE '[0-9]+\.[0-9]+' | head -n1 || true)"
  if [[ -z "$ac_ver" ]] || awk -v v="$ac_ver" 'BEGIN{ split(v,a,"."); exit !((a[1]<2) || (a[1]==2 && a[2]<71)) }'; then
    FORCE_HOST_SRC=1 _install_from_source autoconf "autoconf-2.72" \
      "https://ftp.gnu.org/gnu/autoconf/autoconf-2.72.tar.xz" || true
  fi
  if ! command -v libtoolize &>/dev/null; then
    FORCE_HOST_SRC=1 _install_from_source libtoolize "libtool-2.4.7" \
      "https://ftp.gnu.org/gnu/libtool/libtool-2.4.7.tar.xz" || {
      _fail "libtool"; return 1; }
  fi

  export PATH="${LOCAL_BIN}:${HOME}/.local/bin:${PATH}"
  _setup_aclocal_path

  # Project-local host helpers (never symlink outside project)
  _copy_host_bin_tools

  local missing=() cmd
  for cmd in gcc cmake python3 meson git svn nasm aclocal-1.18 libtoolize; do
    command -v "$cmd" &>/dev/null || missing+=("$cmd")
  done
  if [[ ${#missing[@]} -gt 0 ]]; then
    _fail "缺少命令: ${missing[*]}"
    return 1
  fi
  if ! _meson_ge 0 64; then
    _fail "Meson 仍 < 0.64（$(meson --version 2>&1 | head -n1)）"
    return 1
  fi
  _ok "本机依赖就绪"
  _log "python: ${FFBUILD_PYTHON:-$(command -v python3)} ; meson: $(meson --version 2>/dev/null | head -n1) ; ninja: $(ninja --version 2>/dev/null | head -n1)"
}

###############################################################################
# 3. Host toolchain
###############################################################################

local_env_activate_host() {
  local host_triple
  host_triple="$(gcc -dumpmachine 2>/dev/null || true)"
  [[ -n "$host_triple" ]] || host_triple="x86_64-linux-gnu"
  export FFBUILD_TOOLCHAIN="$host_triple"
  export FFBUILD_CROSS_PREFIX="${host_triple}-"

  mkdir -p "$LOCAL_BIN" "$CARGO_HOME" "$RUSTUP_HOME"
  local tool src dst
  for tool in gcc g++ cc c++ ar ranlib nm ld strip as objcopy objdump readelf strings pkg-config; do
    src="$(command -v "$tool" 2>/dev/null || true)"
    if [[ -z "$src" ]]; then
      case "$tool" in
        cc) src="$(command -v gcc || true)" ;;
        c++) src="$(command -v g++ || true)" ;;
      esac
    fi
    [[ -n "$src" ]] || continue
    dst="${LOCAL_BIN}/${host_triple}-${tool}"
    ln -sfn "$src" "$dst"
  done
  ln -sfn "$(command -v gcc-ar 2>/dev/null || command -v ar)" "${LOCAL_BIN}/${host_triple}-gcc-ar" 2>/dev/null || \
    ln -sfn "$(command -v ar)" "${LOCAL_BIN}/${host_triple}-gcc-ar"
  ln -sfn "$(command -v gcc-ranlib 2>/dev/null || command -v ranlib)" "${LOCAL_BIN}/${host_triple}-gcc-ranlib" 2>/dev/null || \
    ln -sfn "$(command -v ranlib)" "${LOCAL_BIN}/${host_triple}-gcc-ranlib"
  ln -sfn "$(command -v gcc-nm 2>/dev/null || command -v nm)" "${LOCAL_BIN}/${host_triple}-gcc-nm" 2>/dev/null || \
    ln -sfn "$(command -v nm)" "${LOCAL_BIN}/${host_triple}-gcc-nm"

  export PATH="${LOCAL_BIN}:${CARGO_HOME}/bin:${PATH}"
  export CC="${host_triple}-gcc"
  export CXX="${host_triple}-g++"
  export LD="${host_triple}-ld"
  export AR="${host_triple}-gcc-ar"
  export RANLIB="${host_triple}-gcc-ranlib"
  export NM="${host_triple}-gcc-nm"
  export FFBUILD_TARGET_FLAGS="--pkg-config=pkg-config --arch=x86_64 --target-os=linux"
  export FFBUILD_RUST_TARGET="x86_64-unknown-linux-gnu"
  echo "[INFO] 本机 triple: ${FFBUILD_TOOLCHAIN}"
  echo "[INFO] 已创建前缀工具: ${LOCAL_BIN}/${host_triple}-gcc -> $(readlink -f "${LOCAL_BIN}/${host_triple}-gcc" 2>/dev/null || readlink "${LOCAL_BIN}/${host_triple}-gcc")"
  command -v "${host_triple}-gcc" >/dev/null || {
    echo "[ERROR] ${host_triple}-gcc 不在 PATH" >&2
    return 1
  }

  local _static_flags="-static-libgcc -static-libstdc++"
  if ! echo 'int main(){return 0;}' | g++ -x c++ - -static-libgcc -static-libstdc++ -o /tmp/.ffbuild_static_cxx_test.$$ 2>/dev/null; then
    echo "[WARN] 本机无可用 libstdc++.a（请 dnf install libstdc++-static）；host 模式去掉 -static-libstdc++/-static-libgcc"
    _static_flags=""
  fi
  rm -f /tmp/.ffbuild_static_cxx_test.$$

  local _fs_flags=""
  local _gcc_maj
  _gcc_maj="$(g++ -dumpversion 2>/dev/null | cut -d. -f1 || true)"
  if [[ -n "$_gcc_maj" && "$_gcc_maj" -lt 9 ]]; then
    _fs_flags="-lstdc++fs"
    echo "[INFO] GCC ${_gcc_maj}: host cmake 链接末尾附加 -lstdc++fs"
  fi

  export CFLAGS="${_static_flags} -I${FFBUILD_PREFIX}/include -O2 -pipe -fPIC -DPIC -D_FORTIFY_SOURCE=2 -fstack-protector-strong -fstack-clash-protection -pthread"
  export CXXFLAGS="${_static_flags} -I${FFBUILD_PREFIX}/include -O2 -pipe -fPIC -DPIC -D_FORTIFY_SOURCE=2 -fstack-protector-strong -fstack-clash-protection -pthread"
  export LDFLAGS="${_static_flags} -L${FFBUILD_PREFIX}/lib -O2 -pipe -fstack-protector-strong -fstack-clash-protection -Wl,-z,relro,-z,now -pthread -lm"
  export STAGE_CFLAGS="-fvisibility=hidden -fno-semantic-interposition"
  export STAGE_CXXFLAGS="-fvisibility=hidden -fno-semantic-interposition"
  export HOST_CC=gcc HOST_CXX=g++ HOST_CFLAGS="-O2 -pipe" HOST_CXXFLAGS="-O2 -pipe"

  cat > "$TOOLCHAIN_CMAKE" <<EOF
# Native host toolchain wrappers (USE_HOST_TOOLCHAIN=1)
set(CMAKE_C_COMPILER ${host_triple}-gcc)
set(CMAKE_CXX_COMPILER ${host_triple}-g++)
set(CMAKE_RANLIB ${host_triple}-gcc-ranlib)
set(CMAKE_AR ${host_triple}-gcc-ar)
set(CMAKE_INSTALL_LIBDIR lib)
set(CMAKE_PREFIX_PATH "${FFBUILD_PREFIX}" CACHE PATH "" FORCE)
list(APPEND CMAKE_PREFIX_PATH "${FFBUILD_PREFIX}")
set(CMAKE_FIND_ROOT_PATH "${FFBUILD_PREFIX}")
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(PKG_CONFIG_EXECUTABLE "pkg-config" CACHE FILEPATH "" FORCE)
EOF
  if [[ -n "$_fs_flags" ]]; then
    cat >> "$TOOLCHAIN_CMAKE" <<EOF
# GCC < 9: std::filesystem → libstdc++fs (after static libs)
set(CMAKE_CXX_STANDARD_LIBRARIES "-lstdc++ -lm ${_fs_flags} \${CMAKE_CXX_STANDARD_LIBRARIES}")
EOF
  fi

  export PKG_CONFIG_LIBDIR="${FFBUILD_PREFIX}/lib/pkgconfig:${FFBUILD_PREFIX}/share/pkgconfig"
  export PKG_CONFIG_PATH="$PKG_CONFIG_LIBDIR"

  cat > "$CROSS_MESON" <<EOF
[binaries]
c = '${host_triple}-gcc'
cpp = '${host_triple}-g++'
ld = '${host_triple}-ld'
ar = '${host_triple}-gcc-ar'
ranlib = '${host_triple}-gcc-ranlib'
strip = '${host_triple}-strip'
pkg-config = 'pkg-config'

[properties]
pkg_config_libdir = '${FFBUILD_PREFIX}/lib/pkgconfig:${FFBUILD_PREFIX}/share/pkgconfig'

[paths]
libdir = 'lib'

[host_machine]
system = 'linux'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF

  export FFBUILD_CMAKE_TOOLCHAIN="$TOOLCHAIN_CMAKE"

  mkdir -p "$CARGO_HOME"
  cat > "${CARGO_HOME}/config.toml" <<EOF
[target.${FFBUILD_RUST_TARGET}]
linker = "${host_triple}-gcc"
ar = "${host_triple}-gcc-ar"

[target.host]
linker = "gcc"
ar = "ar"
EOF

  if [[ ! -d "${LOCAL_OPT}/implib/.git" && ! -d "${LOCAL_OPT}/implib" ]]; then
    git clone --filter=blob:none --depth=1 https://github.com/yugr/Implib.so "${LOCAL_OPT}/implib" 2>/dev/null \
      || echo "[WARN] Implib.so 克隆失败（部分 stage 可能需要）"
  fi
  # Prefer project tools gen-implib (already copied in stage_host_deps)
  if [[ -f "${HOST_BIN_SRC}/gen-implib" && ! -x "${LOCAL_BIN}/gen-implib" ]]; then
    cp -f "${HOST_BIN_SRC}/gen-implib" "${LOCAL_BIN}/gen-implib"
    chmod +x "${LOCAL_BIN}/gen-implib"
  fi

  if ! command -v rustup &>/dev/null && [[ ! -x "${CARGO_HOME}/bin/rustup" ]]; then
    echo "[INFO] 安装 rustup（本机工具链模式）"
    curl https://sh.rustup.rs -sSf | bash -s -- -y --no-modify-path || \
      echo "[WARN] rustup 安装失败；依赖 rav1e 的构建可能失败"
  fi
  export PATH="${CARGO_HOME}/bin:${PATH}"
  if command -v rustup &>/dev/null || [[ -x "${CARGO_HOME}/bin/rustup" ]]; then
    rustup default stable 2>/dev/null || rustup default nightly 2>/dev/null || true
  fi
  _ensure_cargo_c || echo "[WARN] cargo-c 未就绪（rav1e 阶段将再试或失败）"

  echo "[INFO] 本机工具链已激活: $(gcc --version | head -1)"
  echo "[INFO] 注意：产物依赖本机 glibc，可移植性弱于交叉工具链"
}

# cargo cinstall 来自 cargo-c 包（二进制名 cargo-cinstall，不是 cargo-c）
_ensure_cargo_c() {
  export CARGO_HOME="${CARGO_HOME:-${LOCAL_OPT}/cargo}"
  export RUSTUP_HOME="${RUSTUP_HOME:-${LOCAL_OPT}/rustup}"
  export PATH="${CARGO_HOME}/bin:${PATH}"
  hash -r 2>/dev/null || true

  if command -v cargo-cinstall &>/dev/null || [[ -x "${CARGO_HOME}/bin/cargo-cinstall" ]]; then
    echo "[INFO] cargo-c 已就绪: $(command -v cargo-cinstall 2>/dev/null || echo ${CARGO_HOME}/bin/cargo-cinstall)"
    return 0
  fi

  if ! command -v cargo &>/dev/null && [[ ! -x "${CARGO_HOME}/bin/cargo" ]]; then
    echo "[ERROR] 未找到 cargo，无法安装 cargo-c（rav1e 需要）" >&2
    return 1
  fi

  echo "[INFO] 安装 cargo-c（提供 cargo cinstall，rav1e 需要）…"
  # 先尝试普通安装；EL8 常缺新 openssl，再试 vendored-openssl
  if ! cargo install cargo-c; then
    echo "[INFO] cargo install cargo-c 失败，改用 --features=vendored-openssl"
    if ! cargo install cargo-c --features=vendored-openssl; then
      echo "[ERROR] cargo-c 安装失败" >&2
      return 1
    fi
  fi
  hash -r 2>/dev/null || true
  if command -v cargo-cinstall &>/dev/null || [[ -x "${CARGO_HOME}/bin/cargo-cinstall" ]]; then
    echo "[INFO] cargo-c 安装成功"
    return 0
  fi
  echo "[ERROR] cargo-c 安装后仍找不到 cargo-cinstall" >&2
  ls -la "${CARGO_HOME}/bin"/cargo-c* 2>/dev/null || true
  return 1
}

###############################################################################
# 4. Download
###############################################################################

local_download_all() {
  : "${WORK_DIR:?WORK_DIR must be set}"
  : "${DL_CACHE:?DL_CACHE must be set}"
  : "${TARGET:?TARGET must be set}"
  : "${VARIANT:?VARIANT must be set}"

  cd "$WORK_DIR"
  mkdir -p "$DL_CACHE"
  _init_ff_vars

  local _saved_CC="${CC:-}" _saved_CXX="${CXX:-}"
  unset CC CXX || true

  local STAGE STAGENAME STG DLHASH TGT WORKDIR

  shopt -s nullglob
  for STAGE in scripts/*.sh scripts/*/*.sh; do
    STAGENAME="$(basename "$STAGE" .sh)"
    echo "[INFO] download stage: $STAGENAME"
    (
      set -euo pipefail
      cd "$WORK_DIR"
      # Ensure dl helpers exist inside subshell
      default_dl() { echo "git-mini-clone \"$SCRIPT_REPO\" \"$SCRIPT_COMMIT\" \"$1\""; }
      ffbuild_dl() { default_dl .; }
      # shellcheck source=/dev/null
      source "$STAGE"
      if ! ffbuild_enabled; then
        exit 0
      fi
      STG="$(ffbuild_dl || true)"
      [[ -z "${STG:-}" ]] && exit 0
      DLHASH="$(printf '%s' "$STG" | sha256sum | awk '{print $1}')"
      TGT="${DL_CACHE}/${STAGENAME}_${DLHASH}.tar.xz"
      if [[ -f "$TGT" ]]; then
        ln -sfn "$(basename "$TGT")" "${DL_CACHE}/${STAGENAME}.tar.xz"
        exit 0
      fi
      WORKDIR="$(mktemp -d)"
      trap 'rm -rf -- "$WORKDIR"' EXIT
      cd "$WORKDIR"
      eval "$STG"
      mkdir -p "$DL_CACHE"
      local tmp_archive="${WORKDIR}/cache.tar.xz"
      tar -cJf "$tmp_archive" .
      local sz
      sz="$(stat -c%s "$tmp_archive" 2>/dev/null || stat -f%z "$tmp_archive" 2>/dev/null || echo 0)"
      if [[ "${sz:-0}" -lt 4096 ]]; then
        echo "[ERROR] download cache too small (${sz} bytes) for $STAGENAME — 不写入缓存" >&2
        exit 1
      fi
      mv -f "$tmp_archive" "$TGT"
      ln -sfn "$(basename "$TGT")" "${DL_CACHE}/${STAGENAME}.tar.xz"
    ) || {
      echo "[ERROR] download failed: $STAGENAME" >&2
      [[ -n "$_saved_CC" ]] && export CC="$_saved_CC"
      [[ -n "$_saved_CXX" ]] && export CXX="$_saved_CXX"
      return 1
    }
  done
  shopt -u nullglob

  [[ -n "$_saved_CC" ]] && export CC="$_saved_CC"
  [[ -n "$_saved_CXX" ]] && export CXX="$_saved_CXX"
}

###############################################################################
# 5. Stage runner
###############################################################################

resolvestage() {
  local name="$1"
  local -a matches=()
  local had_nullglob=0
  shopt -q nullglob && had_nullglob=1
  shopt -s nullglob

  if [[ -d "$name" ]]; then
    echo "$name"
    [[ "$had_nullglob" -eq 1 ]] || shopt -u nullglob
    return 0
  fi

  matches=(scripts/??-"${name}")
  if [[ ${#matches[@]} -gt 0 && -d "${matches[0]}" ]]; then
    echo "${matches[0]}"
    [[ "$had_nullglob" -eq 1 ]] || shopt -u nullglob
    return 0
  fi

  matches=(scripts/??-"${name}.sh")
  if [[ ${#matches[@]} -gt 0 && -f "${matches[0]}" ]]; then
    echo "${matches[0]}"
    [[ "$had_nullglob" -eq 1 ]] || shopt -u nullglob
    return 0
  fi

  [[ "$had_nullglob" -eq 1 ]] || shopt -u nullglob
  echo "scripts/??-${name}.sh"
}

resolvescript() {
  local STAGE
  STAGE="$(resolvestage "$1")"
  if [[ -d "$STAGE" ]]; then
    local -a scripts=( "$STAGE"/*.sh )
    if [[ ${#scripts[@]} -eq 0 ]]; then
      return 1
    fi
    echo "${scripts[-1]}"
  else
    echo "$STAGE"
  fi
}

get_stagedeps() {
  local name="$1"
  local STAGE SCRIPT
  local -a RESDEPS=() matches=()
  local had_nullglob=0
  shopt -q nullglob && had_nullglob=1
  shopt -s nullglob

  if [[ -d "$name" ]]; then
    STAGE="$name"
  else
    matches=(scripts/??-"${name}")
    if [[ ${#matches[@]} -gt 0 && -d "${matches[0]}" ]]; then
      STAGE="${matches[0]}"
    else
      STAGE=""
    fi
  fi

  if [[ -n "$STAGE" && -d "$STAGE" ]]; then
    local SUBSCRIPT
    for SUBSCRIPT in "$STAGE"/*.sh; do
      [[ -f "$SUBSCRIPT" ]] || continue
      # shellcheck disable=SC2207
      RESDEPS+=( $(get_stagedeps "$SUBSCRIPT") )
    done
    [[ "$had_nullglob" -eq 1 ]] || shopt -u nullglob
    if [[ ${#RESDEPS[@]} -gt 0 ]]; then
      printf '%s\n' "${RESDEPS[@]}" | sort -u
    fi
    return 0
  fi

  if [[ -f "$name" ]]; then
    SCRIPT="$name"
  else
    matches=(scripts/??-"${name}.sh")
    if [[ ${#matches[@]} -eq 0 ]]; then
      [[ "$had_nullglob" -eq 1 ]] || shopt -u nullglob
      return 0
    fi
    SCRIPT="${matches[0]}"
  fi
  [[ "$had_nullglob" -eq 1 ]] || shopt -u nullglob

  (
    set +u
    SELF="$SCRIPT"
    STAGENAME="$(basename "$SCRIPT" | sed 's/.sh$//')"
    ffbuild_depends() { :; }
    # shellcheck source=/dev/null
    source "$SCRIPT"
    ffbuild_enabled || exit 0
    ffbuild_depends
  )
}

collect_build_order() {
  local node="$1"
  local line
  [[ -v DEP_VISITED["$node"] ]] && return 0
  if [[ -v DEP_STACK["$node"] ]]; then
    echo "[ERROR] 依赖环: $node" >&2
    return 1
  fi
  DEP_STACK["$node"]=1
  while IFS= read -r line; do
    [[ -z "$line" ]] && continue
    collect_build_order "$line" || return 1
  done < <(get_stagedeps "$node")
  unset 'DEP_STACK[$node]'
  DEP_VISITED["$node"]=1
  if [[ -n "${ENTRYSCRIPT:-}" && ( "$node" == "$ENTRYSCRIPT" || "$(basename "$node")" == "$(basename "$ENTRYSCRIPT")" ) ]]; then
    return 0
  fi
  BUILD_ORDER+=( "$node" )
}

_prefix_file_count() {
  find "$FFBUILD_PREFIX" -type f 2>/dev/null | wc -l | tr -d ' '
}

_merge_lib64_under_prefix() {
  if [[ -d "${FFBUILD_PREFIX}/lib64" ]]; then
    mkdir -p "${FFBUILD_PREFIX}/lib" "${FFBUILD_PREFIX}/lib/pkgconfig"
    cp -a "${FFBUILD_PREFIX}/lib64/." "${FFBUILD_PREFIX}/lib/" 2>/dev/null || true
    if [[ -d "${FFBUILD_PREFIX}/lib64/pkgconfig" ]]; then
      mkdir -p "${FFBUILD_PREFIX}/lib/pkgconfig"
      cp -a "${FFBUILD_PREFIX}/lib64/pkgconfig/." "${FFBUILD_PREFIX}/lib/pkgconfig/" 2>/dev/null || true
    fi
  fi
}

_stage_has_expected_artifact() {
  local stage="$1"
  local line pats pat f
  while IFS= read -r line; do
    # 去掉首尾空白（heredoc 误缩进时仍能匹配）
    line="${line#"${line%%[![:space:]]*}"}"
    line="${line%"${line##*[![:space:]]}"}"
    [[ -z "$line" || "$line" == \#* ]] && continue
    [[ "${line%%|*}" == "$stage" ]] || continue
    pats="${line#*|}"
    IFS='|' read -r -a arr <<< "$pats"
    for pat in "${arr[@]}"; do
      pat="${pat#"${pat%%[![:space:]]*}"}"
      pat="${pat%"${pat##*[![:space:]]}"}"
      [[ -z "$pat" ]] && continue
      f="${FFBUILD_PREFIX}/${pat}"
      [[ -e "$f" ]] || continue
      # 残缺 .pc（仅有 Libs.private 追加行）视为无效——chromaprint 类假完成
      if [[ "$pat" == *.pc ]]; then
        if ! grep -qE '^(Name:|prefix=)' "$f" 2>/dev/null; then
          continue
        fi
      fi
      return 0
    done
    return 1
  done < <(_stage_artifact_expect)
  # No mapping → unknown stage; caller decides via file-count delta
  return 2
}

# Build one scripts/*.sh. Workdir = CACHE_DIR; .done = STAGES_DIR.
# Installs directly into FFBUILD_PREFIX (FFBUILD_DESTDIR=/，无独立 ffdest).
run_one_script() {
  local SCRIPT="$1"
  local STAGENAME SELF
  # Normalize to path relative to WORK_DIR when possible
  if [[ "$SCRIPT" == /* ]]; then
    :
  elif [[ -f "${WORK_DIR}/${SCRIPT}" ]]; then
    SCRIPT="${WORK_DIR}/${SCRIPT}"
  elif [[ -f "$SCRIPT" ]]; then
    SCRIPT="$(cd "$(dirname "$SCRIPT")" && pwd)/$(basename "$SCRIPT")"
  fi
  STAGENAME="$(basename "$SCRIPT" .sh)"
  SELF="$SCRIPT"

  if [[ -f "${STAGES_DIR}/${STAGENAME}.done" && -z "${FORCE_REBUILD:-}" ]]; then
    local skip_art_rc=0
    _stage_has_expected_artifact "$STAGENAME" || skip_art_rc=$?
    if [[ "$skip_art_rc" -eq 1 ]]; then
      echo "[WARN] $STAGENAME.done 存在但期望产物缺失，清除标记并重编" >&2
      rm -f "${STAGES_DIR}/${STAGENAME}.done"
    elif [[ "$skip_art_rc" -eq 0 ]]; then
      echo "[SKIP] $STAGENAME"
      return 0
    elif grep -qE '^SCRIPT_SKIP=' "$SCRIPT" 2>/dev/null; then
      echo "[SKIP] $STAGENAME"
      return 0
    else
      # 无产物映射的真实库 stage：不信任裸 .done（避免 libunibreak/libpng 类假完成）
      echo "[WARN] $STAGENAME.done 存在但无产物映射，清除标记并重编" >&2
      rm -f "${STAGES_DIR}/${STAGENAME}.done"
    fi
  fi

  echo "[STEP] building stage $STAGENAME"
  rm -rf "${CACHE_DIR}/${STAGENAME}"
  mkdir -p "${CACHE_DIR}/${STAGENAME}" "${FFBUILD_PREFIX}"

  local cache="${DL_CACHE}/${STAGENAME}.tar.xz"
  if [[ -f "$cache" ]]; then
    tar -xaf "$cache" -C "${CACHE_DIR}/${STAGENAME}"
  fi

  local before_files
  before_files="$(_prefix_file_count)"

  (
    set -euo pipefail
    export SELF STAGENAME
    : "${FFBUILD_PREFIX:?FFBUILD_PREFIX must be set in stage}"
    # DESTDIR=/ + absolute PREFIX → 直装到 prefix（无独立 ffdest）
    export FFBUILD_PREFIX
    export FFBUILD_DESTDIR="${FFBUILD_DESTDIR:-/}"
    export FFBUILD_DESTPREFIX="${FFBUILD_DESTPREFIX:-$FFBUILD_PREFIX}"
    export CPPFLAGS="${CPPFLAGS:-}" LDFLAGS="${LDFLAGS:-}" CFLAGS="${CFLAGS:-}" CXXFLAGS="${CXXFLAGS:-}"
    export RAW_CFLAGS="$CFLAGS" RAW_CXXFLAGS="$CXXFLAGS" RAW_LDFLAGS="$LDFLAGS"
    [[ -n "${STAGE_CFLAGS:-}" ]] && export CFLAGS="$CFLAGS $STAGE_CFLAGS"
    [[ -n "${STAGE_CXXFLAGS:-}" ]] && export CXXFLAGS="$CXXFLAGS $STAGE_CXXFLAGS"
    export CMAKE_ARGS="${CMAKE_ARGS:-} -DCMAKE_INSTALL_LIBDIR=lib"
    # 确保本 stage 的 autoreconf 能找到已安装的 xorg-macros.m4 等
    _setup_aclocal_path
    cd "${CACHE_DIR}/${STAGENAME}"
    git config --global --add safe.directory "$PWD" || true
    # shellcheck source=/dev/null
    source "$SCRIPT"
    ffbuild_enabled || exit 0
    ffbuild_build
  ) || {
    echo "[ERROR] stage 失败: $STAGENAME（保留工作目录 ${CACHE_DIR}/${STAGENAME} 便于排查）" >&2
    return 1
  }

  _merge_lib64_under_prefix
  # Dep stages must not leave bins that confuse later builds / packaging
  rm -rf "${FFBUILD_PREFIX}/bin"

  local after_files delta
  after_files="$(_prefix_file_count)"
  delta=$((after_files - before_files))

  local skip=0
  if grep -qE '^SCRIPT_SKIP=' "$SCRIPT" 2>/dev/null; then
    skip=1
  fi

  local art_rc=0
  # Capture status without tripping set -e
  _stage_has_expected_artifact "$STAGENAME" || art_rc=$?

  if [[ "$skip" -eq 1 ]]; then
    echo "[INFO] $STAGENAME SCRIPT_SKIP，标记完成（delta=${delta}）"
    touch "${STAGES_DIR}/${STAGENAME}.done"
    rm -rf "${CACHE_DIR}/${STAGENAME}"
    return 0
  fi

  # Known artifact mapping: must exist
  if [[ "$art_rc" -eq 1 ]]; then
    echo "[ERROR] $STAGENAME: 期望产物未出现在 ${FFBUILD_PREFIX}。不写 .done。" >&2
    return 1
  fi

  # No mapping: require some new files (or allow if art_rc==0 already passed)
  if [[ "$art_rc" -eq 2 && "$delta" -le 0 ]]; then
    # Extra critical checks for well-known codecs without relying solely on delta
    case "$STAGENAME" in
      50-x264)
        if [[ ! -f "${FFBUILD_PREFIX}/lib/pkgconfig/x264.pc" && ! -f "${FFBUILD_PREFIX}/lib/libx264.a" ]]; then
          echo "[ERROR] $STAGENAME: missing libx264/x264.pc。不写 .done。" >&2
          return 1
        fi
        ;;
      50-x265)
        if [[ ! -f "${FFBUILD_PREFIX}/lib/pkgconfig/x265.pc" && ! -f "${FFBUILD_PREFIX}/lib/libx265.a" ]]; then
          echo "[ERROR] $STAGENAME: missing libx265/x265.pc。不写 .done。" >&2
          return 1
        fi
        ;;
      50-davs2)
        if [[ ! -f "${FFBUILD_PREFIX}/lib/pkgconfig/davs2.pc" && ! -f "${FFBUILD_PREFIX}/lib/libdavs2.a" ]]; then
          echo "[ERROR] $STAGENAME: missing libdavs2/davs2.pc。不写 .done。" >&2
          return 1
        fi
        ;;
      50-xavs2)
        if [[ ! -f "${FFBUILD_PREFIX}/lib/pkgconfig/xavs2.pc" && ! -f "${FFBUILD_PREFIX}/lib/libxavs2.a" ]]; then
          echo "[ERROR] $STAGENAME: missing libxavs2/xavs2.pc。不写 .done。" >&2
          return 1
        fi
        ;;
      50-lcevcdec)
        if [[ ! -f "${FFBUILD_PREFIX}/lib/pkgconfig/lcevc_dec.pc" ]]; then
          echo "[ERROR] $STAGENAME: missing lcevc_dec.pc。不写 .done。" >&2
          return 1
        fi
        ;;
      50-libssh)
        if [[ ! -f "${FFBUILD_PREFIX}/lib/pkgconfig/libssh.pc" || ! -f "${FFBUILD_PREFIX}/lib/libssh.a" ]]; then
          echo "[ERROR] $STAGENAME: missing libssh。不写 .done。" >&2
          return 1
        fi
        ;;
      *)
        echo "[ERROR] $STAGENAME 未向 ${FFBUILD_PREFIX} 安装新文件（delta=${delta}）。不写 .done。" >&2
        return 1
        ;;
    esac
  fi

  # libssh GSSAPI sanity
  if [[ "$STAGENAME" == "50-libssh" && -f "${FFBUILD_PREFIX}/lib/libssh.a" ]]; then
    if nm -A "${FFBUILD_PREFIX}/lib/libssh.a" 2>/dev/null | grep -q ' U gss_'; then
      if ! grep -qE 'gssapi|krb5' "${FFBUILD_PREFIX}/lib/pkgconfig/libssh.pc" 2>/dev/null; then
        echo "[ERROR] $STAGENAME: libssh.a 含未解析 gss_*（请 WITH_GSSAPI=OFF）。不写 .done。" >&2
        return 1
      fi
    fi
  fi

  touch "${STAGES_DIR}/${STAGENAME}.done"
  local pc_now=0
  [[ -d "${FFBUILD_PREFIX}/lib/pkgconfig" ]] && \
    pc_now="$(find "${FFBUILD_PREFIX}/lib/pkgconfig" -name '*.pc' 2>/dev/null | wc -l | tr -d ' ')"
  echo "[INFO] done $STAGENAME (delta_files=${delta} total_pc=${pc_now})"

  rm -rf "${CACHE_DIR}/${STAGENAME}"
}

_stage_leaf_names() {
  local CURDEP SCRIPT STAGE STAGE_SCRIPT
  local -a leaves=()
  for CURDEP in "$@"; do
    SCRIPT="$(resolvescript "$CURDEP" || true)"
    [[ -n "${SCRIPT:-}" && -f "$SCRIPT" ]] || continue
    (
      set +u
      SELF="$SCRIPT"
      ffbuild_depends() { :; }
      # shellcheck source=/dev/null
      source "$SCRIPT"
      ffbuild_enabled
    ) || continue
    STAGE="$(resolvestage "$CURDEP")"
    if [[ -d "$STAGE" ]]; then
      shopt -s nullglob
      for STAGE_SCRIPT in "$STAGE"/*.sh; do
        leaves+=("$(basename "$STAGE_SCRIPT" .sh)")
      done
      shopt -u nullglob
    else
      leaves+=("$(basename "$STAGE" .sh)")
    fi
  done
  printf '%s\n' "${leaves[@]+"${leaves[@]}"}"
}

_stage_progress_counts() {
  local name done_n=0 pend_n=0 total=0
  for name in "$@"; do
    [[ -n "$name" ]] || continue
    total=$((total + 1))
    if [[ -f "${STAGES_DIR}/${name}.done" ]]; then
      done_n=$((done_n + 1))
    else
      pend_n=$((pend_n + 1))
    fi
  done
  echo "$done_n $pend_n $total"
}

_emit_stage_progress() {
  local label="${1:-}"
  shift
  local done_n pend_n total pct=0
  read -r done_n pend_n total < <(_stage_progress_counts "$@")
  [[ "$total" -gt 0 ]] && pct=$((done_n * 100 / total))
  if [[ -n "$label" ]]; then
    printf '[PROGRESS] %s  已完成 %s/%s  未编译 %s  (%s%%)\n' "$label" "$done_n" "$total" "$pend_n" "$pct"
  else
    printf '[PROGRESS] 已完成 %s/%s  未编译 %s  (%s%%)\n' "$done_n" "$total" "$pend_n" "$pct"
  fi
}

local_run_all_stages() {
  : "${WORK_DIR:?WORK_DIR must be set}"
  : "${TARGET:?TARGET must be set}"
  : "${VARIANT:?VARIANT must be set}"
  : "${STAGES_DIR:?STAGES_DIR must be set}"
  : "${DL_CACHE:?DL_CACHE must be set}"
  : "${FFBUILD_PREFIX:?FFBUILD_PREFIX must be set}"
  : "${CACHE_DIR:?CACHE_DIR must be set}"
  # DESTDIR=/ → 直装 PREFIX；DESTPREFIX 与 PREFIX 相同
  export FFBUILD_DESTDIR="${FFBUILD_DESTDIR:-/}"
  export FFBUILD_DESTPREFIX="${FFBUILD_DESTPREFIX:-$FFBUILD_PREFIX}"

  cd "$WORK_DIR"
  _init_ff_vars

  # 依赖阶段可能跳过 toolchain；确保 cross.meson 带上 pkg_config_libdir / libdir
  if [[ -n "${CROSS_MESON:-}" && -n "${FFBUILD_PREFIX:-}" ]]; then
    local _cc _cxx _ld _ar _ranlib _strip
    _cc="$(grep -E "^c = " "$CROSS_MESON" 2>/dev/null | head -n1 | sed -E "s/^c = '([^']+)'.*/\1/" || true)"
    _cxx="$(grep -E "^cpp = " "$CROSS_MESON" 2>/dev/null | head -n1 | sed -E "s/^cpp = '([^']+)'.*/\1/" || true)"
    _ld="$(grep -E "^ld = " "$CROSS_MESON" 2>/dev/null | head -n1 | sed -E "s/^ld = '([^']+)'.*/\1/" || true)"
    _ar="$(grep -E "^ar = " "$CROSS_MESON" 2>/dev/null | head -n1 | sed -E "s/^ar = '([^']+)'.*/\1/" || true)"
    _ranlib="$(grep -E "^ranlib = " "$CROSS_MESON" 2>/dev/null | head -n1 | sed -E "s/^ranlib = '([^']+)'.*/\1/" || true)"
    _strip="$(grep -E "^strip = " "$CROSS_MESON" 2>/dev/null | head -n1 | sed -E "s/^strip = '([^']+)'.*/\1/" || true)"
    : "${_cc:=${FFBUILD_TOOLCHAIN:-x86_64-redhat-linux}-gcc}"
    : "${_cxx:=${FFBUILD_TOOLCHAIN:-x86_64-redhat-linux}-g++}"
    : "${_ld:=${FFBUILD_TOOLCHAIN:-x86_64-redhat-linux}-ld}"
    : "${_ar:=${FFBUILD_TOOLCHAIN:-x86_64-redhat-linux}-gcc-ar}"
    : "${_ranlib:=${FFBUILD_TOOLCHAIN:-x86_64-redhat-linux}-gcc-ranlib}"
    : "${_strip:=${FFBUILD_TOOLCHAIN:-x86_64-redhat-linux}-strip}"
    cat > "$CROSS_MESON" <<EOF || true
[binaries]
c = '${_cc}'
cpp = '${_cxx}'
ld = '${_ld}'
ar = '${_ar}'
ranlib = '${_ranlib}'
strip = '${_strip}'
pkg-config = 'pkg-config'

[properties]
pkg_config_libdir = '${FFBUILD_PREFIX}/lib/pkgconfig:${FFBUILD_PREFIX}/share/pkgconfig'

[paths]
libdir = 'lib'

[host_machine]
system = 'linux'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF
  fi

  if [[ -n "${FORCE_REBUILD:-}" ]]; then
    echo "[INFO] FORCE_REBUILD=1：清除 stage 断点标记与 FFBUILD_PREFIX"
    rm -f "${STAGES_DIR}"/*.done
    rm -rf "${FFBUILD_PREFIX:?}"/*
  fi

  export TARGET VARIANT ADDINS_STR REPO FFVER

  local ENTRYSCRIPT
  ENTRYSCRIPT="$(ls -1d scripts/* 2>/dev/null | tail -n 1)"
  if [[ -z "$ENTRYSCRIPT" ]]; then
    echo "[ERROR] 未找到 scripts/* 入口" >&2
    return 1
  fi

  local -A DEP_VISITED DEP_STACK
  local -a BUILD_ORDER=()
  local built_count=0 skip_count=0
  local CURDEP SCRIPT STAGE STAGE_SCRIPT

  echo "[INFO] stage 调度入口: $ENTRYSCRIPT  TARGET=$TARGET VARIANT=$VARIANT FFVER=$FFVER"
  echo "[INFO] FFBUILD_TOOLCHAIN=${FFBUILD_TOOLCHAIN:-} CROSS_PREFIX=${FFBUILD_CROSS_PREFIX:-} CC=${CC:-}"
  echo "[INFO] FFBUILD_PREFIX=$FFBUILD_PREFIX (DESTDIR empty → direct install)"

  _setup_aclocal_path
  echo "[INFO] ACLOCAL_PATH=${ACLOCAL_PATH:-}"

  collect_build_order "$ENTRYSCRIPT" || return 1

  if [[ ${#BUILD_ORDER[@]} -eq 0 ]]; then
    echo "[ERROR] 依赖拓扑为空（入口: $ENTRYSCRIPT）。解析失败。" >&2
    return 1
  fi
  echo "[INFO] 拓扑排序完成：共 ${#BUILD_ORDER[@]} 个 stage"

  local -a STAGE_LEAVES=()
  mapfile -t STAGE_LEAVES < <(_stage_leaf_names "${BUILD_ORDER[@]}")
  if [[ ${#STAGE_LEAVES[@]} -eq 0 ]]; then
    STAGE_LEAVES=()
  fi
  _emit_stage_progress "开始" "${STAGE_LEAVES[@]+"${STAGE_LEAVES[@]}"}"

  for CURDEP in "${BUILD_ORDER[@]}"; do
    SCRIPT="$(resolvescript "$CURDEP" || true)"
    if [[ -z "${SCRIPT:-}" || ! -f "$SCRIPT" ]]; then
      echo "[WARN] 无法解析 stage 脚本: $CURDEP → '${SCRIPT:-}'，跳过"
      skip_count=$((skip_count + 1))
      continue
    fi
    (
      set +u
      SELF="$SCRIPT"
      ffbuild_depends() { :; }
      # shellcheck source=/dev/null
      source "$SCRIPT"
      ffbuild_enabled
    ) || { echo "[SKIP] disabled: $CURDEP"; skip_count=$((skip_count + 1)); continue; }

    STAGE="$(resolvestage "$CURDEP")"
    if [[ -d "$STAGE" ]]; then
      shopt -s nullglob
      local -a stage_scripts=( "$STAGE"/*.sh )
      shopt -u nullglob
      if [[ ${#stage_scripts[@]} -eq 0 ]]; then
        echo "[WARN] 目录 stage 无脚本: $STAGE"
        skip_count=$((skip_count + 1))
        continue
      fi
      # 同目录前置叶子若 .done 但产物缺失，先清掉，避免后面叶子误 SKIP 依赖
      local _pre
      for _pre in "${stage_scripts[@]}"; do
        local _pn
        _pn="$(basename "$_pre" .sh)"
        if [[ -f "${STAGES_DIR}/${_pn}.done" ]]; then
          local _prc=0
          _stage_has_expected_artifact "$_pn" || _prc=$?
          if [[ "$_prc" -eq 1 ]]; then
            echo "[WARN] 清除陈旧 ${_pn}.done（同目录依赖产物缺失）" >&2
            rm -f "${STAGES_DIR}/${_pn}.done"
          fi
        fi
      done
      for STAGE_SCRIPT in "${stage_scripts[@]}"; do
        local _leaf _was_done=0
        _leaf="$(basename "$STAGE_SCRIPT" .sh)"
        [[ -f "${STAGES_DIR}/${_leaf}.done" && -z "${FORCE_REBUILD:-}" ]] && _was_done=1
        run_one_script "$STAGE_SCRIPT" || return 1
        built_count=$((built_count + 1))
        if [[ "$_was_done" -eq 0 ]]; then
          _emit_stage_progress "$_leaf" "${STAGE_LEAVES[@]+"${STAGE_LEAVES[@]}"}"
        fi
      done
    else
      local _leaf _was_done=0
      _leaf="$(basename "$STAGE" .sh)"
      [[ -f "${STAGES_DIR}/${_leaf}.done" && -z "${FORCE_REBUILD:-}" ]] && _was_done=1
      run_one_script "$STAGE" || return 1
      built_count=$((built_count + 1))
      if [[ "$_was_done" -eq 0 ]]; then
        _emit_stage_progress "$_leaf" "${STAGE_LEAVES[@]+"${STAGE_LEAVES[@]}"}"
      fi
    fi
  done

  local done_count pc_count
  done_count="$(find "${STAGES_DIR}" -name '*.done' 2>/dev/null | wc -l | tr -d ' ')"
  pc_count=0
  [[ -d "${FFBUILD_PREFIX}/lib/pkgconfig" ]] && \
    pc_count="$(find "${FFBUILD_PREFIX}/lib/pkgconfig" -name '*.pc' 2>/dev/null | wc -l | tr -d ' ')"

  _emit_stage_progress "结束" "${STAGE_LEAVES[@]+"${STAGE_LEAVES[@]}"}"
  echo "[INFO] scripts 调度结束：本轮执行=${built_count} 禁用跳过≈${skip_count} .done=${done_count} pkgconfig=${pc_count}"
  if [[ "$pc_count" -lt 5 ]]; then
    echo "[ERROR] 依赖安装结果异常：${FFBUILD_PREFIX}/lib/pkgconfig 下 .pc 过少（${pc_count}）" >&2
    return 1
  fi
  echo "[INFO] 所有 scripts 依赖 stage 已构建完成（FFVER=${FFVER}）"
}

###############################################################################
# 6. Helpers (pkg-config / preflight / deps readiness)
###############################################################################

_pc_count() {
  local d="$1"
  [[ -d "$d" ]] || { echo 0; return; }
  find "$d" -name '*.pc' 2>/dev/null | wc -l | tr -d ' '
}

_rewrite_pc_prefix() {
  local prefix="$1"
  local pcdir="${prefix}/lib/pkgconfig"
  [[ -d "$pcdir" ]] || return 0
  local f
  find "$pcdir" -name '*.pc' -print0 2>/dev/null | while IFS= read -r -d '' f; do
    if grep -q '^prefix=' "$f" 2>/dev/null; then
      sed -i "s|^prefix=.*|prefix=${prefix}|" "$f"
    fi
    sed -i '/^Requires\.private:[[:space:]]*$/d' "$f" 2>/dev/null || true
  done
}

_preflight_ffmpeg_pkgconfig() {
  local pcdir="${USR_DIR}/lib/pkgconfig"
  export PKG_CONFIG_LIBDIR="${pcdir}:${USR_DIR}/share/pkgconfig"
  export PKG_CONFIG_PATH="$PKG_CONFIG_LIBDIR"
  unset PKG_CONFIG_SYSROOT_DIR || true

  local -a checks=(
    "libx264:x264:50-x264"
    "libx265:x265:50-x265"
    "libdavs2:davs2:50-davs2"
    "libxavs2:xavs2:50-xavs2"
    "liblcevc-dec:lcevc_dec:50-lcevcdec"
    "libssh:libssh:50-libssh"
    "libplacebo:libplacebo:50-libplacebo"
    "libass:libass:50-libass"
    "libopus:opus:50-libopus"
    "libvpx:vpx:50-libvpx"
    "libwebp:libwebp:50-libwebp"
    "libvorbis:vorbis:50-libvorbis"
    "libtheora:theora:50-libtheora"
    "libbluray:libbluray:50-libbluray"
    "libsrt:srt:50-srt"
    "libzimg:zimg:50-zimg"
    "libaom:aom:50-aom"
    "libdav1d:dav1d:50-dav1d"
    "libsvtav1:SvtAv1Enc:50-svtav1"
    "librav1e:rav1e:50-rav1e"
    "libopenh264:openh264:50-openh264"
    "libopenjpeg:libopenjp2:50-openjpeg"
    "libfdk-aac:fdk-aac:50-fdk-aac"
    "libtwolame:twolame:50-twolame"
    "libsoxr:soxr:50-soxr"
    "libzmq:libzmq:50-libzmq"
    "libvidstab:vidstab:50-vidstab"
    "libvvenc:libvvenc:50-vvenc"
    "libuavs3d:uavs3d:50-uavs3d"
    "libkvazaar:kvazaar:50-kvazaar"
    "libzvbi:zvbi-0.2:50-zvbi"
    "libaribb24:aribb24:50-libaribb24"
    "libjxl:libjxl:50-libjxl"
    "librist:librist:50-librist"
    "libvpl:vpl:50-onevpl"
    "openal:openal:50-openal"
    "liboapv:oapv:50-openapv"
    "libopencore-amrnb:opencore-amrnb:50-opencore-amr"
    "libopenmpt:libopenmpt:50-openmpt"
    "libass:libass:50-libass"
    "libunibreak:libunibreak:45-libunibreak"
    "libdvdread:dvdread:40-libdvdread"
    "libdvdnav:dvdnav:50-libdvdnav"
    "chromaprint:libchromaprint:50-chromaprint"
    "libgme:libgme:50-gme"
    "libshaderc:shaderc:50-shaderc"
    "librubberband:rubberband:50-rubberband"
    "sdl2:sdl2:50-sdl"
    "libaribcaption:libaribcaption:50-libaribcaption"
    "fontconfig:fontconfig:35-fontconfig"
    "libfreetype:freetype2:25-freetype"
    "libharfbuzz:harfbuzz:45-harfbuzz"
    "libfribidi:fribidi:25-fribidi"
    "openssl:openssl:25-openssl"
    "libxml2:libxml-2.0:25-libxml2"
    "lzma:liblzma:25-xz"
    "libvmaf:libvmaf:45-vmaf"
    "libpulse:libpulse:45-pulseaudio"
    "vulkan:vulkan:45-vulkan-loader"
    "libdrm:libdrm:40-libdrm"
    "vaapi:libva:50-libva"
    "lv2:lilv-0:99-lilv"
    "ffnvcodec:ffnvcodec:50-ffnvcodec"
  )

  local cfg="${FF_CONFIGURE:-}"
  local item flag mod stage rc=0 err
  for item in "${checks[@]}"; do
    IFS=':' read -r flag mod stage <<< "$item"
    [[ "$cfg" == *"--enable-${flag}"* ]] || continue
    if [[ -f "${pcdir}/${mod}.pc" ]]; then
      # 残缺 .pc（例如仅有 Libs.private 追加行）当作缺失
      if ! grep -qE '^(Name:|prefix=)' "${pcdir}/${mod}.pc" 2>/dev/null; then
        echo "[ERROR] preflight: ${pcdir}/${mod}.pc 内容无效（缺 Name:/prefix=）" >&2
        rm -f "${pcdir}/${mod}.pc"
        if [[ -n "$stage" ]]; then
          rm -f "${STAGES_DIR}/${stage}.done"
          echo "[INFO] 已清除无效 ${mod}.pc 与 ${stage}.done，请重跑依赖阶段" >&2
        fi
        rc=1
        continue
      fi
    elif [[ ! -f "${pcdir}/${mod}.pc" ]]; then
      echo "[ERROR] preflight: 启用了 --enable-${flag} 但缺少 ${pcdir}/${mod}.pc" >&2
      if [[ -n "$stage" ]]; then
        rm -f "${STAGES_DIR}/${stage}.done"
        echo "[INFO] 已清除 ${stage}.done，请重跑依赖阶段" >&2
      fi
      rc=1
      continue
    fi
    err="$(pkg-config --exists --print-errors --static "$mod" 2>&1)" || {
      echo "[ERROR] preflight: pkg-config --static $mod 失败: $err" >&2
      if [[ -n "$stage" ]]; then
        rm -f "${STAGES_DIR}/${stage}.done"
        echo "[INFO] 已清除 ${stage}.done，请重跑依赖阶段" >&2
      fi
      rc=1
      continue
    }
  done

  if [[ -f "${USR_DIR}/lib/libssh.a" ]]; then
    local nmout=""
    nmout="$(nm -A "${USR_DIR}/lib/libssh.a" 2>/dev/null || nm "${USR_DIR}/lib/libssh.a" 2>/dev/null || true)"
    if printf '%s' "$nmout" | grep -qE '[[:space:]]U[[:space:]]+gss_'; then
      if ! grep -qE 'gssapi|krb5' "${pcdir}/libssh.pc" 2>/dev/null; then
        echo "[ERROR] preflight: libssh.a 含未解析 gss_*（需 WITH_GSSAPI=OFF 重编）" >&2
        rm -f "${STAGES_DIR}/50-libssh.done"
        rc=1
      fi
    fi
  fi

  local -a file_checks=(
    "libmp3lame:lib/libmp3lame.a|include/lame/lame.h:50-libmp3lame"
    "libxvid:lib/libxvidcore.a|include/xvid.h:50-xvid"
    "libsnappy:lib/libsnappy.a|include/snappy-c.h:50-snappy"
    "avisynth:include/avisynth/avisynth_c.h|include/avisynth/avisynth.h:50-avisynth"
    "amf:include/AMF/core/Factory.h:50-amf"
    "frei0r:include/frei0r.h:50-frei0r"
  )
  local paths path found
  for item in "${file_checks[@]}"; do
    IFS=':' read -r flag paths stage <<< "$item"
    [[ "$cfg" == *"--enable-${flag}"* ]] || continue
    found=0
    IFS='|' read -r -a arr <<< "$paths"
    for path in "${arr[@]}"; do
      if [[ -e "${USR_DIR}/${path}" ]]; then
        found=1
        break
      fi
    done
    if [[ "$found" -eq 0 ]]; then
      echo "[ERROR] preflight: 启用了 --enable-${flag} 但缺少产物（${paths}）" >&2
      [[ -n "$stage" ]] && rm -f "${STAGES_DIR}/${stage}.done"
      rc=1
    fi
  done

  local -a pie_libs=(
    "libxavs2:xavs2.h:xavs2_api_get:50-xavs2:-lxavs2 -lpthread -lm -ldl"
    "libdavs2:davs2.h:davs2_decoder_open:50-davs2:-ldavs2 -lpthread -lm"
  )
  local hdr sym libs smoke_dir
  for item in "${pie_libs[@]}"; do
    IFS=':' read -r flag hdr sym stage libs <<< "$item"
    [[ "$cfg" == *"--enable-${flag}"* ]] || continue
    local archive=""
    case "$flag" in
      libxavs2) archive="${USR_DIR}/lib/libxavs2.a" ;;
      libdavs2) archive="${USR_DIR}/lib/libdavs2.a" ;;
    esac
    [[ -f "$archive" ]] || continue
    smoke_dir="$(mktemp -d)"
    cat >"${smoke_dir}/t.c" <<EOF
#include <stdint.h>
#include <${hdr}>
int main(void) { return (int)(intptr_t)${sym}; }
EOF
    if ! "${CC:-gcc}" -fPIE -pie -O0 -o "${smoke_dir}/t" "${smoke_dir}/t.c" \
        -I"${USR_DIR}/include" -L"${USR_DIR}/lib" \
        -Wl,--start-group ${libs} -Wl,--end-group >/dev/null 2>"${smoke_dir}/err"; then
      echo "[ERROR] preflight: ${flag} 无法链入 PIE（多为 asm 非 PIC），将重编 ${stage}" >&2
      head -n 8 "${smoke_dir}/err" >&2 || true
      rm -f "${STAGES_DIR}/${stage}.done"
      rc=1
    fi
    rm -rf "$smoke_dir"
  done

  if [[ "$rc" -eq 0 ]]; then
    echo "[INFO] preflight: 已启用依赖的 pkg-config/文件/PIE 检查通过"
  fi
  return "$rc"
}

_missing_critical_deps() {
  local -a need=()
  local n
  need+=(zlib openssl)
  if [[ "${VARIANT:-nonfree}" != lgpl* ]]; then
    need+=(x264 x265)
  fi
  if [[ "${VARIANT:-}" == nonfree* ]]; then
    need+=(fdk-aac)
  fi
  local -a missing=()
  for n in "${need[@]}"; do
    if [[ -f "${USR_DIR}/lib/pkgconfig/${n}.pc" || -f "${USR_DIR}/lib/lib${n}.a" ]]; then
      continue
    fi
    if [[ "$n" == "fdk-aac" && -f "${USR_DIR}/lib/pkgconfig/fdk-aac.pc" ]]; then
      continue
    fi
    missing+=("$n")
  done
  printf '%s\n' "${missing[@]+"${missing[@]}"}"
}

_deps_ready() {
  local pc
  pc="$(_pc_count "${USR_DIR}/lib/pkgconfig")"
  [[ "$pc" -ge 5 ]] || return 1
  local miss
  miss="$(_missing_critical_deps | tr '\n' ' ' | xargs)"
  [[ -z "$miss" ]]
}

_stage_artifact_expect() {
  # 注意：heredoc 行首禁止缩进，否则 stage 名带空格导致 scrub/SKIP 全部失效
  cat <<'EOF'
10-xorg-macros|share/aclocal/xorg-macros.m4|share/pkgconfig/xorg-macros.pc
10-xcbproto|lib/pkgconfig/xcb-proto.pc|share/pkgconfig/xcb-proto.pc
10-xproto|share/pkgconfig/xproto.pc|lib/pkgconfig/xproto.pc
10-xtrans|share/pkgconfig/xtrans.pc|lib/pkgconfig/xtrans.pc
20-zlib|lib/pkgconfig/zlib.pc|lib/libz.a
20-libiconv|lib/libiconv.a|include/iconv.h|lib/pkgconfig/libiconv.pc
20-libxau|lib/pkgconfig/xau.pc
25-openssl|lib/pkgconfig/openssl.pc|lib/pkgconfig/libssl.pc
25-xz|lib/pkgconfig/liblzma.pc
25-gmp|lib/pkgconfig/gmp.pc|lib/libgmp.a
25-libxml2|lib/pkgconfig/libxml-2.0.pc
25-libpng|lib/pkgconfig/libpng.pc|lib/libpng.a|lib/libpng16.a
25-freetype|lib/pkgconfig/freetype2.pc|share/aclocal/freetype2.m4
25-fribidi|lib/pkgconfig/fribidi.pc
25-libogg|lib/pkgconfig/ogg.pc|share/aclocal/ogg.m4
25-fftw3|lib/pkgconfig/fftw3.pc|lib/libfftw3.a
30-libxcb|lib/pkgconfig/xcb.pc
30-libpciaccess|lib/pkgconfig/pciaccess.pc
30-libdvdcss|lib/pkgconfig/libdvdcss.pc|lib/libdvdcss.a
35-fontconfig|lib/pkgconfig/fontconfig.pc
40-libx11|lib/pkgconfig/x11.pc
40-libdrm|lib/pkgconfig/libdrm.pc
40-vulkan-headers|include/vulkan/vulkan.h|share/vulkan/registry/vk.xml
40-libdvdread|lib/pkgconfig/dvdread.pc
40-mbedtls|lib/pkgconfig/mbedtls.pc|lib/pkgconfig/mbedcrypto.pc|lib/libmbedtls.a
45-harfbuzz|lib/pkgconfig/harfbuzz.pc
45-libvorbis|lib/pkgconfig/vorbis.pc|share/aclocal/vorbis.m4
45-pulseaudio|lib/pkgconfig/libpulse.pc
45-opencl|lib/pkgconfig/OpenCL.pc
45-vmaf|lib/pkgconfig/libvmaf.pc
45-vulkan-loader|lib/pkgconfig/vulkan.pc
45-libunibreak|lib/pkgconfig/libunibreak.pc|lib/libunibreak.a
45-libudfread|lib/pkgconfig/udfread.pc|lib/libudfread.a
45-libsamplerate|lib/pkgconfig/samplerate.pc|lib/libsamplerate.a
45-brotli|lib/pkgconfig/libbrotlicommon.pc|lib/pkgconfig/libbrotlidec.pc|lib/libbrotlidec.a
45-lcms2|lib/pkgconfig/lcms2.pc|lib/liblcms2.a
50-freetype|lib/pkgconfig/freetype2.pc
50-libxext|lib/pkgconfig/xext.pc
50-libxfixes|lib/pkgconfig/xfixes.pc
50-libxi|lib/pkgconfig/xi.pc
50-libxinerama|lib/pkgconfig/xinerama.pc
50-libxrender|lib/pkgconfig/xrender.pc
50-libxscrnsaver|lib/pkgconfig/xscrnsaver.pc
50-libxxf86vm|lib/pkgconfig/xxf86vm.pc
50-libva|lib/pkgconfig/libva.pc
50-libass|lib/pkgconfig/libass.pc
50-libopus|lib/pkgconfig/opus.pc
50-libvpx|lib/pkgconfig/vpx.pc
50-libmp3lame|lib/pkgconfig/lame.pc|lib/libmp3lame.a
50-libwebp|lib/pkgconfig/libwebp.pc
50-libtheora|lib/pkgconfig/theora.pc
50-libbluray|lib/pkgconfig/libbluray.pc
50-libssh|lib/pkgconfig/libssh.pc|lib/libssh.a
50-libcurl|lib/pkgconfig/libcurl.pc
50-srt|lib/pkgconfig/srt.pc
50-zimg|lib/pkgconfig/zimg.pc
50-x264|lib/pkgconfig/x264.pc|lib/libx264.a
50-x265|lib/pkgconfig/x265.pc|lib/libx265.a
50-davs2|lib/pkgconfig/davs2.pc|lib/libdavs2.a
50-xavs2|lib/pkgconfig/xavs2.pc|lib/libxavs2.a
50-lcevcdec|lib/pkgconfig/lcevc_dec.pc
50-libplacebo|lib/pkgconfig/libplacebo.pc
50-shaderc|lib/pkgconfig/shaderc.pc|lib/pkgconfig/shaderc_combined.pc
50-rubberband|lib/pkgconfig/rubberband.pc
50-sdl|lib/pkgconfig/sdl2.pc
50-libaribcaption|lib/pkgconfig/libaribcaption.pc
50-aom|lib/pkgconfig/aom.pc
50-dav1d|lib/pkgconfig/dav1d.pc
50-svtav1|lib/pkgconfig/SvtAv1Enc.pc
50-rav1e|lib/pkgconfig/rav1e.pc
50-openh264|lib/pkgconfig/openh264.pc
50-openjpeg|lib/pkgconfig/libopenjp2.pc
50-fdk-aac|lib/pkgconfig/fdk-aac.pc
50-twolame|lib/pkgconfig/twolame.pc
50-soxr|lib/pkgconfig/soxr.pc
50-snappy|lib/libsnappy.a|lib/pkgconfig/snappy.pc
50-libzmq|lib/pkgconfig/libzmq.pc
50-vidstab|lib/pkgconfig/vidstab.pc
50-vvenc|lib/pkgconfig/libvvenc.pc
50-uavs3d|lib/pkgconfig/uavs3d.pc
50-kvazaar|lib/pkgconfig/kvazaar.pc
50-xvid|lib/libxvidcore.a
50-zvbi|lib/pkgconfig/zvbi-0.2.pc
50-libaribb24|lib/pkgconfig/aribb24.pc
50-libjxl|lib/pkgconfig/libjxl.pc
50-librist|lib/pkgconfig/librist.pc
50-onevpl|lib/pkgconfig/vpl.pc
50-openal|lib/pkgconfig/openal.pc
50-openapv|lib/pkgconfig/oapv.pc
50-opencore-amr|lib/pkgconfig/opencore-amrnb.pc
50-openmpt|lib/pkgconfig/libopenmpt.pc
50-libdvdnav|lib/pkgconfig/dvdnav.pc
50-chromaprint|lib/pkgconfig/libchromaprint.pc
50-gme|lib/pkgconfig/libgme.pc
50-frei0r|lib/pkgconfig/frei0r.pc|include/frei0r.h
50-avisynth|include/avisynth/avisynth.h|include/avisynth.h
50-ffnvcodec|lib/pkgconfig/ffnvcodec.pc
50-amf|include/AMF/core/Factory.h
50-whisper|lib/pkgconfig/whisper.pc|lib/libwhisper.a
55-spirv-cross|lib/pkgconfig/spirv-cross-c-shared.pc|lib/pkgconfig/spirv-cross.pc|lib/libspirv-cross-c.a
60-libglvnd|lib/pkgconfig/gl.pc|lib/pkgconfig/opengl.pc
60-libxcursor|lib/pkgconfig/xcursor.pc
60-libxrandr|lib/pkgconfig/xrandr.pc
60-libxv|lib/pkgconfig/xv.pc
60-spirv-headers|include/spirv/unified1/spirv.h|include/spirv/1.2/spirv.h
96-lv2|lib/pkgconfig/lv2.pc
96-serd|lib/pkgconfig/serd-0.pc
96-zix|lib/pkgconfig/zix-0.pc
97-sord|lib/pkgconfig/sord-0.pc
98-sratom|lib/pkgconfig/sratom-0.pc
99-lilv|lib/pkgconfig/lilv-0.pc
EOF
}

_scrub_stale_stage_dones() {
  local line stage pats pat found
  while IFS= read -r line; do
    line="${line#"${line%%[![:space:]]*}"}"
    line="${line%"${line##*[![:space:]]}"}"
    [[ -z "$line" || "$line" == \#* ]] && continue
    stage="${line%%|*}"
    pats="${line#*|}"
    stage="${stage%"${stage##*[![:space:]]}"}"
    [[ -f "${STAGES_DIR}/${stage}.done" ]] || continue
    found=0
    IFS='|' read -r -a arr <<< "$pats"
    for pat in "${arr[@]}"; do
      pat="${pat#"${pat%%[![:space:]]*}"}"
      pat="${pat%"${pat##*[![:space:]]}"}"
      [[ -z "$pat" ]] && continue
      if [[ -e "${USR_DIR}/${pat}" ]]; then
        if [[ "$pat" == *.pc ]] && ! grep -qE '^(Name:|prefix=)' "${USR_DIR}/${pat}" 2>/dev/null; then
          continue
        fi
        found=1
        break
      fi
    done
    if [[ "$found" -eq 0 ]]; then
      rm -f "${STAGES_DIR}/${stage}.done"
      _info "清除陈旧标记 ${stage}.done（release 中无产物，将重编）"
    fi
  done < <(_stage_artifact_expect)

  if [[ -f "${STAGES_DIR}/50-libssh.done" && -f "${USR_DIR}/lib/libssh.a" ]]; then
    if nm -A "${USR_DIR}/lib/libssh.a" 2>/dev/null | grep -q ' U gss_'; then
      if ! grep -qE 'gssapi|krb5' "${USR_DIR}/lib/pkgconfig/libssh.pc" 2>/dev/null; then
        rm -f "${STAGES_DIR}/50-libssh.done"
        _info "清除 50-libssh.done（静态库含未解析 gss_*，将以 WITH_GSSAPI=OFF 重编）"
      fi
    fi
  fi

  # ffnvcodec 头版本与目标 NVENC_SDK 不一致时强制重编（避免 SKIP 后仍链到缓存）
  if [[ -f "${STAGES_DIR}/50-ffnvcodec.done" && -f "${USR_DIR}/lib/pkgconfig/ffnvcodec.pc" ]]; then
    local _nv_ver _nv_want _nv_ok=0
    _nv_ver="$(awk '/^Version:/{print $2; exit}' "${USR_DIR}/lib/pkgconfig/ffnvcodec.pc" 2>/dev/null || true)"
    _nv_want="${NVENC_SDK:-12.2}"
    case "$_nv_want" in
      13.0|13) [[ "$_nv_ver" == 13.* ]] && _nv_ok=1 ;;
      11.1|11) [[ "$_nv_ver" == 11.* ]] && _nv_ok=1 ;;
      12.2|12|*) [[ "$_nv_ver" == 12.2* ]] && _nv_ok=1 ;;
    esac
    if [[ "$_nv_ok" -eq 0 ]]; then
      rm -f "${STAGES_DIR}/50-ffnvcodec.done"
      _info "清除 50-ffnvcodec.done（ffnvcodec=${_nv_ver:-?}，期望 ${_nv_want}，将重编头并重链 FFmpeg）"
    fi
  fi
}

###############################################################################
# 7. collect_ff_flags / get_output
###############################################################################

get_output() {
  local script_abs="$1" kind="$2"
  (
    set +e
    set +u
    SELF="$script_abs"
    # shellcheck disable=SC1090
    source "$script_abs"
    if ffbuild_enabled; then
      ffbuild_"$kind" 2>/dev/null || exit 0
    else
      ffbuild_un"$kind" 2>/dev/null || exit 0
    fi
  )
}

collect_ff_flags() {
  : "${WORK_DIR:?WORK_DIR must be set}"
  : "${TARGET:?TARGET must be set}"
  : "${VARIANT:?VARIANT must be set}"

  cd "$WORK_DIR"
  _init_ff_vars

  FF_CONFIGURE="${FF_CONFIGURE:-}"
  FF_CFLAGS="${FF_CFLAGS:-}"
  FF_CXXFLAGS="${FF_CXXFLAGS:-}"
  FF_LDFLAGS="${FF_LDFLAGS:-}"
  FF_LDEXEFLAGS="${FF_LDEXEFLAGS:-}"
  FF_LIBS="${FF_LIBS:-}"

  local SCRIPT script_abs conf_piece enabled_count=0
  shopt -s nullglob
  for SCRIPT in scripts/*.sh scripts/*/*.sh; do
    [[ -f "$SCRIPT" ]] || continue
    if [[ "$SCRIPT" == /* ]]; then
      script_abs="$SCRIPT"
    else
      script_abs="${WORK_DIR}/${SCRIPT}"
    fi
    conf_piece="$(get_output "$script_abs" configure | tr '\n' ' ')"
    FF_CONFIGURE+=" ${conf_piece}"
    FF_CFLAGS+=" $(get_output "$script_abs" cflags | tr '\n' ' ')"
    FF_CXXFLAGS+=" $(get_output "$script_abs" cxxflags | tr '\n' ' ')"
    FF_LDFLAGS+=" $(get_output "$script_abs" ldflags | tr '\n' ' ')"
    FF_LDEXEFLAGS+=" $(get_output "$script_abs" ldexeflags | tr '\n' ' ')"
    FF_LIBS+=" $(get_output "$script_abs" libs | tr '\n' ' ')"
    if [[ "$conf_piece" == *"--enable-"* ]]; then
      enabled_count=$((enabled_count + 1))
    fi
  done
  shopt -u nullglob

  FF_CONFIGURE="$(echo "$FF_CONFIGURE" | xargs)"
  FF_CFLAGS="$(echo "$FF_CFLAGS" | xargs)"
  FF_CXXFLAGS="$(echo "$FF_CXXFLAGS" | xargs)"
  FF_LDFLAGS="$(echo "$FF_LDFLAGS" | xargs)"
  FF_LDEXEFLAGS="$(echo "$FF_LDEXEFLAGS" | xargs)"
  FF_LIBS="$(echo "$FF_LIBS" | xargs)"

  # FFmpeg 6.x + 现代 Vulkan-Headers：强制关 vulkan（避免 Vk*MESA 编译失败）
  if [[ "${FFVER:-99999999}" -lt 700 ]]; then
    if [[ "$FF_CONFIGURE" == *"--enable-vulkan"* ]]; then
      FF_CONFIGURE="$(echo "$FF_CONFIGURE" | sed -E 's/(^| )--enable-vulkan( |$)/ --disable-vulkan /g')"
      FF_CONFIGURE="$(echo "$FF_CONFIGURE" | xargs)"
      echo "[INFO] FFVER=$FFVER < 700：已将 --enable-vulkan 替换为 --disable-vulkan"
    elif [[ "$FF_CONFIGURE" != *"--disable-vulkan"* ]]; then
      FF_CONFIGURE="$(echo "$FF_CONFIGURE --disable-vulkan" | xargs)"
      echo "[INFO] FFVER=$FFVER < 700：追加 --disable-vulkan"
    fi
  fi

  export FF_CONFIGURE FF_CFLAGS FF_CXXFLAGS FF_LDFLAGS FF_LDEXEFLAGS FF_LIBS

  echo "[INFO] 已从 scripts 收集到 ${enabled_count} 个 --enable-* 项"
  echo "[INFO] FF_CONFIGURE=$FF_CONFIGURE"
  echo "[INFO] FF_CFLAGS=$FF_CFLAGS"
  echo "[INFO] FF_LDFLAGS=$FF_LDFLAGS"
  echo "[INFO] FF_LIBS=$FF_LIBS"

  if [[ "$enabled_count" -lt 5 ]]; then
    echo "[ERROR] 依赖库 configure 标志过少（${enabled_count}）。" >&2
    echo "[ERROR] 正常 nonfree 构建应包含大量 --enable-lib*。" >&2
    return 1
  fi
  if [[ "$FF_CONFIGURE" != *"--enable-libx264"* && "$VARIANT" != lgpl* ]]; then
    echo "[ERROR] 缺少 --enable-libx264，依赖标志收集异常。" >&2
    return 1
  fi
}

_local_assert_deps_installed() {
  : "${FFBUILD_PREFIX:?}"
  local pc_count lib_count
  pc_count=0
  lib_count=0
  [[ -d "${FFBUILD_PREFIX}/lib/pkgconfig" ]] && \
    pc_count="$(find "${FFBUILD_PREFIX}/lib/pkgconfig" -name '*.pc' 2>/dev/null | wc -l | tr -d ' ')"
  [[ -d "${FFBUILD_PREFIX}/lib" ]] && \
    lib_count="$(find "${FFBUILD_PREFIX}/lib" \( -name '*.a' -o -name '*.so*' \) 2>/dev/null | wc -l | tr -d ' ')"

  echo "[INFO] 依赖前缀检查: ${FFBUILD_PREFIX} (pc=${pc_count} libs=${lib_count})"

  if [[ "$pc_count" -lt 5 && "$lib_count" -lt 5 ]]; then
    echo "[ERROR] ${FFBUILD_PREFIX} 中几乎没有已编译的依赖库。" >&2
    echo "[ERROR] 请重新完整构建，不要使用 FFMPEG_ONLY=1，除非依赖已齐全。" >&2
    return 1
  fi

  if [[ "$VARIANT" != lgpl* ]]; then
    if [[ ! -f "${FFBUILD_PREFIX}/lib/pkgconfig/x264.pc" && ! -f "${FFBUILD_PREFIX}/lib/libx264.a" ]]; then
      echo "[ERROR] 未找到 libx264（${FFBUILD_PREFIX}）。依赖 stage 未编过或失败。" >&2
      return 1
    fi
  fi
}

###############################################################################
# 8. High-level stages
###############################################################################

stage_toolchain() {
  USE_HOST_TOOLCHAIN="${USE_HOST_TOOLCHAIN:-1}"
  export USE_HOST_TOOLCHAIN
  export WORK_DIR FFBUILD_PREFIX FFBUILD_DESTDIR FFBUILD_DESTPREFIX
  export STAGES_DIR DL_CACHE SRC_DIR CACHE_DIR
  export LOCAL_ROOT LOCAL_OPT LOCAL_BIN TOOLCHAIN_CMAKE CROSS_MESON CT_PREFIX
  export PKG_CONFIG_LIBDIR="${USR_DIR}/lib/pkgconfig:${USR_DIR}/share/pkgconfig"
  export PKG_CONFIG_PATH="$PKG_CONFIG_LIBDIR"

  if [[ -n "${USE_HOST_TOOLCHAIN:-}" ]]; then
    local_env_activate_host >>"$BUILD_LOG" 2>&1
    export FFBUILD_PREFIX="$USR_DIR"
    export FFBUILD_DESTDIR="/"
    export FFBUILD_DESTPREFIX="$FFBUILD_PREFIX"
    export CFLAGS="-I${USR_DIR}/include -O2 -pipe -fPIC -DPIC -pthread"
    export CXXFLAGS="-I${USR_DIR}/include -O2 -pipe -fPIC -DPIC -pthread"
    export LDFLAGS="-L${USR_DIR}/lib -pthread -lm"
    if echo 'int main(){return 0;}' | g++ -x c++ - -static-libgcc -static-libstdc++ -o /tmp/.ff_cxx.$$ 2>/dev/null; then
      export CFLAGS="-static-libgcc -static-libstdc++ $CFLAGS"
      export CXXFLAGS="-static-libgcc -static-libstdc++ $CXXFLAGS"
      export LDFLAGS="-static-libgcc -static-libstdc++ $LDFLAGS"
    fi
    rm -f /tmp/.ff_cxx.$$
    _ok "host gcc: $(gcc --version | head -1)"
  else
    _fail "本构建仅支持 USE_HOST_TOOLCHAIN=1（系统 gcc）"
    return 1
  fi
}

# FFMPEG_ONLY：不装本机包、不下载、不编依赖；只准备 PATH/编译器/pkg-config 后编 FFmpeg
stage_ffmpeg_only_prep() {
  export PATH="${LOCAL_BIN}:${HOME}/.local/bin:${PATH}"
  _setup_aclocal_path || true
  _copy_host_bin_tools || true
  stage_toolchain || return 1
  if ! _deps_ready; then
    local miss
    miss="$(_missing_critical_deps | tr '\n' ' ' | xargs || true)"
    _fail "FFMPEG_ONLY 要求 build/release 依赖已齐，缺少: ${miss:-?}。请先完整跑一次 ./build.sh"
    return 1
  fi
  _ok "FFMPEG_ONLY 环境就绪（跳过阶段 1–4）"
}

stage_fetch_deps() {
  if [[ -n "${SKIP_DEPS:-}" || -n "${FFMPEG_ONLY:-}" ]]; then
    _skip "下载依赖（SKIP_DEPS/FFMPEG_ONLY）"
    return 0
  fi

  # 先清陈旧 .done，再判断 pending，避免「.done 在但产物没了」仍跳过下载
  _scrub_stale_stage_dones

  # 有待重编 stage（无 .done）时不能跳过下载：否则解压空/旧缓存会缺目录
  # （例：清掉 50-ffnvcodec.done 后 REUSE 跳过下载 → cd ffnvcodec2 失败）
  local _pending=0
  if [[ -d "${STAGES_DIR:-}" && -d "${WORK_DIR:-}/scripts" ]]; then
    local _s _name
    shopt -s nullglob
    for _s in "${WORK_DIR}/scripts"/*.sh "${WORK_DIR}/scripts"/*/*.sh; do
      [[ -f "$_s" ]] || continue
      _name="$(basename "$_s" .sh)"
      if [[ ! -f "${STAGES_DIR}/${_name}.done" ]]; then
        _pending=1
        break
      fi
    done
    shopt -u nullglob
  fi

  if [[ "${REUSE:-1}" == "1" && -z "${FORCE_REBUILD:-}" ]] && _deps_ready && [[ "$_pending" -eq 0 ]]; then
    _reuse "跳过下载（release 已齐且无待重编 stage）"
    return 0
  fi
  if [[ "$_pending" -eq 1 ]]; then
    _info "存在待重编 stage，刷新 downloads 缓存"
  fi

  local miss
  miss="$(_missing_critical_deps | tr '\n' ' ' | xargs || true)"
  if [[ -n "$miss" ]]; then
    _info "需补齐关键库: $miss （已有库不重编）"
  fi
  mkdir -p "$DL_CACHE"
  if local_download_all >>"$BUILD_LOG" 2>&1; then
    _ok "依赖源码缓存 → utils/downloads"
  else
    _fail "依赖下载"
    return 1
  fi
}

stage_build_deps() {
  _scrub_stale_stage_dones

  if [[ -n "${SKIP_DEPS:-}" || -n "${FFMPEG_ONLY:-}" ]]; then
    _skip "编译依赖（SKIP_DEPS/FFMPEG_ONLY）"
    if ! _deps_ready; then
      local miss
      miss="$(_missing_critical_deps | tr '\n' ' ' | xargs || true)"
      _fail "依赖未就绪，缺少: ${miss:-?}。不要用 FFMPEG_ONLY"
      return 1
    fi
    return 0
  fi

  if [[ -z "${FORCE_REBUILD:-}" ]]; then
    unset FORCE_REBUILD || true
  fi
  local miss pc
  pc="$(_pc_count "${USR_DIR}/lib/pkgconfig")"
  miss="$(_missing_critical_deps | tr '\n' ' ' | xargs || true)"
  if [[ -n "$miss" ]]; then
    _info "补编缺失依赖: $miss（已有 .done 不重编，pkgconfig=${pc}）"
  else
    _info "继续编译未完成的 scripts stage（pkgconfig=${pc}）"
  fi

  export STAGES_DIR DL_CACHE FFBUILD_PREFIX FFBUILD_DESTDIR FFBUILD_DESTPREFIX WORK_DIR CACHE_DIR

  # rav1e 等依赖 cargo-c；工具链阶段若被跳过/静默失败，这里再确保一次
  export PATH="${CARGO_HOME:+$CARGO_HOME/bin:}${PATH}"
  if ! _ensure_cargo_c; then
    _fail "cargo-c（cargo cinstall）不可用，无法编译 rav1e"
    return 1
  fi

  local rc_file="${BUILD_DIR}/.deps_rc"
  rm -f "$rc_file"

  # 注意：旧写法 `cmd | tee | while read` 在 Ctrl+C 后 while 常以 0 退出，父脚本继续跑。
  # 这里用 pipefail + 检查 PIPESTATUS[0]，并配合 _ff_interrupt_setup。
  _filter_deps_console() {
    while IFS= read -r line || [[ -n "${line:-}" ]]; do
      [[ "${BUILD_INTERRUPTED:-0}" == "1" ]] && return 130
      case "$line" in
        "[STEP] building stage "*)
          _console "  … ${line#\[STEP\] building stage }"
          ;;
        "[SKIP] "*)
          _console "  [SKIP] ${line#\[SKIP\] }"
          ;;
        "[PROGRESS] "*)
          _console "  ${line}"
          ;;
        "[INFO] 拓扑排序完成:"*|"[INFO] stage 调度入口:"*)
          _console "  ${line#\[INFO\] }"
          ;;
        "[INFO] done "*)
          _ok "$(echo "$line" | awk '{print $3}')"
          ;;
        "[ERROR] stage 失败:"*|"[ERROR] stage 失败"*)
          _fail "${line#\[ERROR\] }"
          ;;
        "[INTERRUPT]"*)
          _console "  ${line}"
          ;;
      esac
    done
    return 0
  }

  local rc=1
  local -a pst=()
  set +e
  set -o pipefail
  (
    local_run_all_stages
    echo $? >"$rc_file"
  ) 2>&1 | tee -a "$BUILD_LOG" | _filter_deps_console
  pst=( "${PIPESTATUS[@]}" )
  set +o pipefail 2>/dev/null || true
  set -e

  # Ctrl+C：构建子进程 / tee / filter 任一段以 130/143 退出都算中断
  if [[ "${BUILD_INTERRUPTED:-0}" == "1" \
     || "${pst[0]:-0}" -eq 130 || "${pst[0]:-0}" -eq 143 \
     || "${pst[1]:-0}" -eq 130 || "${pst[1]:-0}" -eq 143 \
     || "${pst[2]:-0}" -eq 130 || "${pst[2]:-0}" -eq 143 ]]; then
    _fail "用户中断（Ctrl+C）"
    return 130
  fi

  [[ -f "$rc_file" ]] && rc="$(cat "$rc_file")"
  # 子 shell 被信号杀掉时可能写不出 rc_file
  if [[ ! -f "$rc_file" && "${pst[0]:-1}" -ne 0 ]]; then
    rc="${pst[0]}"
  fi
  if [[ "$rc" -eq 0 ]]; then
    local pc done_n
    pc="$(_pc_count "${USR_DIR}/lib/pkgconfig")"
    done_n="$(find "$STAGES_DIR" -name '*.done' 2>/dev/null | wc -l | tr -d ' ')"
    if ! _deps_ready; then
      miss="$(_missing_critical_deps | tr '\n' ' ' | xargs || true)"
      _fail "依赖阶段结束仍缺: ${miss:-?}（.done=${done_n} pc=${pc}）"
      return 1
    fi
    _ok "依赖编译完成（.done=${done_n} pkgconfig=${pc}）"
  else
    _fail "依赖编译失败（见 build.log）"
    return 1
  fi
}

# True if FFMPEG_DIR exists and is non-empty (any file, .git, or configure).
_ffmpeg_srcdir_ready() {
  local d="${1:-$FFMPEG_DIR}"
  [[ -d "$d" ]] || return 1
  [[ -f "${d}/configure" ]] && return 0
  [[ -d "${d}/.git" ]] && return 0
  # any regular file / non-empty
  local n
  n="$(find "$d" -mindepth 1 -maxdepth 2 2>/dev/null | head -n 1 || true)"
  [[ -n "$n" ]]
}

# 恢复构建所需脚本的可执行位（共享盘/Windows 同步常丢失 +x → Error 126 Permission denied）
_ffmpeg_fix_exec_bits() {
  local src="${1:?}"
  local f
  chmod u+x "${src}/configure" 2>/dev/null || true
  # OpenCL: make 调用 ./tools/source2c 生成 *.cl → *.c
  if [[ -f "${src}/tools/source2c" ]]; then
    chmod u+x "${src}/tools/source2c" || return 1
  fi
  # 其它可能被直接执行的 tools 辅助脚本（无扩展名 / .sh）
  if [[ -d "${src}/tools" ]]; then
    while IFS= read -r -d '' f; do
      chmod u+x "$f" 2>/dev/null || true
    done < <(find "${src}/tools" -maxdepth 1 -type f \( -name '*.sh' -o ! -name '*.*' \) -print0 2>/dev/null)
  fi
  if [[ -f "${src}/tools/source2c" && ! -x "${src}/tools/source2c" ]]; then
    _fail "tools/source2c 仍不可执行（检查挂载是否 noexec / 文件系统权限）"
    return 1
  fi
  _log "已恢复 FFmpeg 脚本执行权限: configure tools/source2c …"
  return 0
}

stage_build_ffmpeg() {
  export SRC_DIR="$CACHE_DIR"
  export FFMPEG_SRC_DIR="$FFMPEG_DIR"
  export OUTPUT_DIR="$USR_DIR"
  export FFBUILD_PREFIX="$USR_DIR"
  export FFBUILD_DESTDIR="/"
  export FFBUILD_DESTPREFIX="$FFBUILD_PREFIX"

  if _ffmpeg_srcdir_ready "$FFMPEG_DIR"; then
    _reuse "ffmpeg 源码已存在（$FFMPEG_DIR），不重新 clone/覆盖"
  fi

  _build_ffmpeg_into_usr
}

_build_ffmpeg_into_usr() {
  set -euo pipefail

  if ! _deps_ready; then
    local miss
    miss="$(_missing_critical_deps | tr '\n' ' ' | xargs || true)"
    _fail "依赖不完整，缺少: ${miss:-?}。请先完成依赖阶段"
    return 1
  fi
  _local_assert_deps_installed >>"$BUILD_LOG" 2>&1 || return 1
  collect_ff_flags >>"$BUILD_LOG" 2>&1 || return 1

  # 用户追加的 configure 参数（在 scripts 收集结果之后）
  if [[ -n "${EXTRA_FF_CONFIGURE:-}" ]]; then
    FF_CONFIGURE="$(echo "${FF_CONFIGURE} ${EXTRA_FF_CONFIGURE}" | xargs)"
    export FF_CONFIGURE
    _info "EXTRA_FF_CONFIGURE → $EXTRA_FF_CONFIGURE"
  fi

  local FFMPEG_SRC="$FFMPEG_DIR"
  local repo="${FFMPEG_REPO:-https://github.com/FFmpeg/FFmpeg.git}"
  # 精确版本用 tag（n8.1.2）；仅主次版本用 release/8.1。可用 GIT_BRANCH / GIT_BRANCH_OVERRIDE 覆盖
  local branch="${GIT_BRANCH:-n${ADDINS_STR}}"
  branch="${GIT_BRANCH_OVERRIDE:-$branch}"

  # 本地优先：仅当本版本目录 ffmpeg/ffmpeg-<ADDINS_STR> 已含 configure 才跳过下载
  if ! _ffmpeg_srcdir_ready "$FFMPEG_SRC"; then
    _info "重新 clone FFmpeg"
    _info "  git:  $repo"
    _info "  ref:  $branch"
    _info "  dst: $FFMPEG_SRC"
    mkdir -p "$(dirname "$FFMPEG_SRC")"
    # Empty dir may exist — remove only if truly empty / no configure
    if [[ -d "$FFMPEG_SRC" ]] && ! _ffmpeg_srcdir_ready "$FFMPEG_SRC"; then
      # safe: only wipe if no meaningful content
      find "$FFMPEG_SRC" -mindepth 1 -maxdepth 1 -exec rm -rf {} + 2>/dev/null || true
    fi
    if ! _run "ffmpeg-clone" git clone --filter=blob:none --branch="$branch" "$repo" "$FFMPEG_SRC"; then
      _fail "ffmpeg clone failed: $repo (ref=$branch → $FFMPEG_SRC)"
      return 1
    fi
  else
    _log "保留本地 FFmpeg 源码: $FFMPEG_SRC（检测到 configure，跳过下载）"
  fi

  if [[ ! -f "${FFMPEG_SRC}/configure" ]]; then
    _fail "FFmpeg 源码缺少 configure: $FFMPEG_SRC"
    return 1
  fi
 
  cd "$FFMPEG_SRC"
  export PKG_CONFIG_PATH="${USR_DIR}/lib/pkgconfig:${USR_DIR}/share/pkgconfig"
  export PKG_CONFIG_LIBDIR="$PKG_CONFIG_PATH"
 
  if ! _preflight_ffmpeg_pkgconfig >>"$BUILD_LOG" 2>&1; then
    # preflight 会清掉坏 .done；即使处于自动 FFMPEG_ONLY 也要允许补编一轮
    _info "预检失败，自动补编缺失依赖后重试"
    local _saved_ff_only="${FFMPEG_ONLY:-}" _saved_skip_deps="${SKIP_DEPS:-}"
    unset FFMPEG_ONLY SKIP_DEPS || true
    if stage_build_deps && _preflight_ffmpeg_pkgconfig >>"$BUILD_LOG" 2>&1; then
      _ok "依赖补编后预检通过"
      [[ -n "$_saved_ff_only" ]] && export FFMPEG_ONLY="$_saved_ff_only"
      [[ -n "$_saved_skip_deps" ]] && export SKIP_DEPS="$_saved_skip_deps"
    else
      [[ -n "$_saved_ff_only" ]] && export FFMPEG_ONLY="$_saved_ff_only"
      [[ -n "$_saved_skip_deps" ]] && export SKIP_DEPS="$_saved_skip_deps"
      _fail "依赖 pkg-config 预检失败（见 build.log）"
      return 1
    fi
  fi

  _info "configure FFmpeg → prefix=build/release"
  # 头文件内联工具较多，抑制 unused-function 警告
  local _ff_cflags="$FF_CFLAGS $CFLAGS -Wno-unused-function"
  local _ff_cxxflags="$FF_CXXFLAGS $CXXFLAGS -Wno-unused-function"
  # shellcheck disable=SC2086
  if ! _run "ffmpeg-configure" ./configure \
      --prefix="$USR_DIR" \
      --pkg-config-flags="--static" \
      $FFBUILD_TARGET_FLAGS \
      $FF_CONFIGURE \
      --extra-cflags="$_ff_cflags" \
      --extra-cxxflags="$_ff_cxxflags" \
      --extra-libs="$FF_LIBS" \
      --extra-ldflags="$FF_LDFLAGS $LDFLAGS" \
      --extra-ldexeflags="$FF_LDEXEFLAGS" \
      --cc="$CC" --cxx="$CXX" --ar="$AR" --ranlib="$RANLIB" --nm="$NM" \
      --extra-version="$(date +%Y%m%d)"; then
    _fail "ffmpeg configure"
    return 1
  fi

  local cfg_h=""
  for cfg_h in config.h ffbuild/config.h; do
    [[ -f "$cfg_h" ]] && break
  done
 
  _info "make FFmpeg"
  if ! _run "ffmpeg-make" make -j"$(nproc)"; then
    _fail "ffmpeg make"
    return 1
  fi
  if ! _run "ffmpeg-install" make install install-doc; then
    _fail "ffmpeg install"
    return 1
  fi

  if [[ -x "${USR_DIR}/bin/ffmpeg" ]]; then
    _ok "ffmpeg → ${USR_DIR}/bin/ffmpeg"
    "${USR_DIR}/bin/ffmpeg" -version 2>&1 | head -n 3 | while read -r L; do _info "$L"; done
  else
    _fail "build/release/bin/ffmpeg 不存在"
    return 1
  fi
}

# Optional: build detect_cuda TensorRT plugin → $FFBUILD_PREFIX/lib
# SKIP_DETECT_CUDA_TRT=1           skip
# REQUIRE_DETECT_CUDA_TRT=1        treat failure as fatal (default: warn only)
stage_build_detect_cuda_trt() {
  local script="${ROOT_DIR}/utils/build-detect-cuda-trt.sh"
  local out_so="${FFBUILD_PREFIX:-${USR_DIR}}/lib/libavfilter_detect_cuda_trt.so"

  if [[ -n "${SKIP_DETECT_CUDA_TRT:-}" ]]; then
    _skip "detect_cuda TRT 插件（SKIP_DETECT_CUDA_TRT=1）"
    return 0
  fi

  if [[ ! -f "$script" ]]; then
    _fail "缺少 $script"
    [[ -n "${REQUIRE_DETECT_CUDA_TRT:-}" ]] && return 1
    return 0
  fi

  _banner "编译 detect_cuda TensorRT 插件 → ${FFBUILD_PREFIX:-${USR_DIR}}/lib"
  export FFMPEG_SRC="${FFMPEG_SRC:-${FFMPEG_DIR}}"
  export FFBUILD_PREFIX="${FFBUILD_PREFIX:-${USR_DIR}}"
  export OUT_DIR="${FFBUILD_PREFIX}/lib"

  if _run "detect-cuda-trt" bash "$script"; then
    if [[ -f "$out_so" ]]; then
      _ok "detect_cuda TRT → $out_so"
      return 0
    fi
    _fail "脚本成功但未找到 $out_so"
  else
    _fail "detect_cuda TRT 插件编译失败（可设 TRT_ROOT / SKIP_DETECT_CUDA_TRT=1）"
  fi

  if [[ -n "${REQUIRE_DETECT_CUDA_TRT:-}" ]]; then
    return 1
  fi
  _info "已忽略 TRT 插件失败（默认不阻断整体构建；需要严格模式请设 REQUIRE_DETECT_CUDA_TRT=1）"
  return 0
}
