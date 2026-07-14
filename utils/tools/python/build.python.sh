#!/bin/bash
set -euo pipefail

# ========== 配置项 ==========
PYTHON_VERSION="${1:-3.14.6}"
SRC_DIR="/usr/local/src"
INSTALL_PREFIX="/usr/local"
PYTHON_BIN="${INSTALL_PREFIX}/bin/python${PYTHON_VERSION%.*}"
PIP_BIN="${INSTALL_PREFIX}/bin/pip${PYTHON_VERSION%.*}"
BUILD_MARKER=".build_success"

# 确保 /usr/local/bin 优先于系统路径
export PATH="${INSTALL_PREFIX}/bin:$PATH"

# Python 编译参数
CONFIGURE_ARGS=(
    --prefix="${INSTALL_PREFIX}"
    --with-system-ffi
    --with-computed-gotos
    --enable-loadable-sqlite-extensions
    --enable-optimizations
    --with-ensurepip=install
    --enable-shared=no
)
# ============================

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

info()  { echo -e "${GREEN}[INFO]${NC} $1"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; exit 1; }
step()  { echo -e "${BLUE}[STEP]${NC} $1"; }

# 必须 root 运行
if [[ $EUID -ne 0 ]]; then
    error "请使用 root 用户或 sudo 执行本脚本"
fi

# ========== 1. CentOS 8 源自动修复 ==========
fix_centos8_repo() {
    if ! grep -q "CentOS Linux release 8" /etc/redhat-release 2>/dev/null; then
        info "非 CentOS 8 系统，跳过源修复"
        return 0
    fi

    if [[ -f /etc/yum.repos.d/CentOS-Vault.repo ]]; then
        info "检测到已配置 Vault 源，跳过重复修复"
        return 0
    fi

    step "修复 CentOS 8 官方源失效，切换至 Vault 归档源"
    mkdir -p /etc/yum.repos.d/bak
    mv /etc/yum.repos.d/CentOS-*.repo /etc/yum.repos.d/bak/ 2>/dev/null || true

    cat > /etc/yum.repos.d/CentOS-Vault.repo << 'EOF'
[BaseOS]
name=CentOS-8.5.2111 - BaseOS
baseurl=http://vault.centos.org/8.5.2111/BaseOS/$basearch/os/
gpgcheck=1
gpgkey=file:///etc/pki/rpm-gpg/RPM-GPG-KEY-centosofficial
enabled=1

[AppStream]
name=CentOS-8.5.2111 - AppStream
baseurl=http://vault.centos.org/8.5.2111/AppStream/$basearch/os/
gpgcheck=1
gpgkey=file:///etc/pki/rpm-gpg/RPM-GPG-KEY-centosofficial
enabled=1

[PowerTools]
name=CentOS-8.5.2111 - PowerTools
baseurl=http://vault.centos.org/8.5.2111/PowerTools/$basearch/os/
gpgcheck=1
gpgkey=file:///etc/pki/rpm-gpg/RPM-GPG-KEY-centosofficial
enabled=1

[extras]
name=CentOS-8.5.2111 - Extras
baseurl=http://vault.centos.org/8.5.2111/extras/$basearch/os/
gpgcheck=1
gpgkey=file:///etc/pki/rpm-gpg/RPM-GPG-KEY-centosofficial
enabled=1
EOF

    dnf clean all -q
    dnf makecache -q
    info "CentOS 8 源修复完成，已启用 PowerTools 仓库"
}

# ========== 2. 安装编译依赖 ==========
install_build_deps() {
    step "安装 Python 编译核心依赖与基础开发工具"
    dnf install -y -q wget yum-utils make gcc \
        openssl-devel bzip2-devel libffi-devel zlib-devel \
        sqlite-devel readline-devel ncurses-devel xz-devel gdbm-devel
    info "系统依赖安装完成"
}

# ========== 3. 编译安装 Python（保留源码 + 增量判断） ==========
build_python() {
    local tarball="Python-${PYTHON_VERSION}.tgz"
    local src_path="${SRC_DIR}/Python-${PYTHON_VERSION}"
    local download_url="https://www.python.org/ftp/python/${PYTHON_VERSION}/${tarball}"
    local marker_file="${src_path}/${BUILD_MARKER}"

    mkdir -p "${SRC_DIR}"

    # 已编译完成则直接跳过
    if [[ -f "${marker_file}" && -x "${PYTHON_BIN}" ]]; then
        info "Python ${PYTHON_VERSION} 已编译完成，跳过编译步骤"
        return 0
    fi

    # 下载源码包（已存在则跳过）
    step "下载 Python ${PYTHON_VERSION} 源码"
    if [[ ! -f "${SRC_DIR}/${tarball}" ]]; then
        wget -q "${download_url}" -O "${SRC_DIR}/${tarball}"
    else
        info "源码包已存在，跳过下载"
    fi

    # 解压源码（目录已存在则跳过）
    step "解压源码"
    if [[ ! -d "${src_path}" ]]; then
        tar -xzf "${SRC_DIR}/${tarball}" -C "${SRC_DIR}"
    else
        info "源码目录已存在，跳过解压"
    fi

    # 编译安装
    step "编译安装 Python ${PYTHON_VERSION}（PGO 优化，首次运行耗时较久）"
    cd "${src_path}"
    ./configure "${CONFIGURE_ARGS[@]}"
    make -j"$(nproc)"
    make altinstall

    # 标记编译成功
    touch "${marker_file}"
    info "Python ${PYTHON_VERSION} 编译安装完成，源码已保留在 ${src_path}"
}

# ========== 4. 升级 pip 并安装构建工具（全量抑制root警告） ==========
install_python_tools() {
    local PIP_OPTS="--root-user-action=ignore -q"

    step "升级 pip、setuptools、wheel 至最新版"
    "${PIP_BIN}" install --upgrade pip setuptools wheel ${PIP_OPTS}

    step "安装/升级 meson 和 ninja 构建工具"
    "${PIP_BIN}" install --upgrade meson ninja ${PIP_OPTS}

    # 卸载系统旧版 ninja，避免路径冲突
    if rpm -q ninja-build &>/dev/null; then
        warn "检测到系统旧版 ninja-build，已卸载以避免路径冲突"
        dnf remove -y -q ninja-build
    fi

    info "Python 构建工具安装完成"
}

# ========== 5. 结果验证（修正调用方式） ==========
verify_result() {
    echo ""
    echo "========================================"
    echo "  整套编译环境部署完成"
    echo "========================================"
    echo -e "Python 路径: ${GREEN}${PYTHON_BIN}${NC}"
    echo -e "Python 版本: ${GREEN}$(${PYTHON_BIN} --version | awk '{print $2}')${NC}"
    echo -e "源码保留路径: ${GREEN}${SRC_DIR}/Python-${PYTHON_VERSION}${NC}"
    echo -e "pip 版本:   ${GREEN}$(${PIP_BIN} --version | awk '{print $2}')${NC}"
    echo -e "meson 路径: ${GREEN}$(which meson)${NC}"
    echo -e "meson 版本: ${GREEN}$(meson --version)${NC}"
    echo -e "ninja 路径: ${GREEN}$(which ninja)${NC}"
    echo -e "ninja 版本: ${GREEN}$(ninja --version)${NC}"
    echo ""
    warn "采用 altinstall 模式，不修改系统默认 python3，不影响 yum/dnf"
    warn "工具已安装至 /usr/local/bin，已优先于系统路径生效"
    echo ""
    info "后续 FFmpeg 编译前请确保环境变量包含："
    echo "  export PATH=/usr/local/bin:\$PATH"
}

# ========== 主流程 ==========
main() {
    echo "========================================"
    echo "  CentOS 8 Python 编译环境一键部署"
    echo "  内置源修复 + Python ${PYTHON_VERSION} + 构建工具"
    echo "  保留源码 · 增量编译 · 重复运行免重编"
    echo "========================================"

    fix_centos8_repo
    install_build_deps
    build_python
    install_python_tools
    verify_result
}

main "$@"
