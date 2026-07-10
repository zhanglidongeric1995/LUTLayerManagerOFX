#!/usr/bin/env bash
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
"$DIR/scripts/install_macos_user.sh"
echo
echo "安装完成。请完全退出并重新打开 DaVinci Resolve。"
read -r -p "按回车键关闭窗口..." _
