#!/bin/bash

set -ex
# 卸载系统版本.
dnf remove cmake
# 安装最新版本.
/bin/cp -a cmake-4.4.0-rc3-linux-x86_64/* /usr/local/
echo 'export PATH="/usr/local/bin:$PATH"' >> /etc/profile.d/devtools.sh
sudo chmod +x /etc/profile.d/devtools.sh
source /etc/profile.d/devtools.sh

cmake --version
