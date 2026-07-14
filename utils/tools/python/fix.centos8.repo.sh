# 1. 备份原有系统源文件
mkdir -p /etc/yum.repos.d/bak
mv /etc/yum.repos.d/CentOS-*.repo /etc/yum.repos.d/bak/

# 2. 写入完整的 Vault 源配置（含 PowerTools，适配编译场景）
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

# 3. 清理旧缓存并重建元数据
dnf clean all
dnf makecache
dnf repolist
