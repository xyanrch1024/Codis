#!/usr/bin/env bash
# Codis 一键安装脚本
# 用法: curl -fsSL https://codis.chat/install | bash
# 从 GitHub Releases 下载预编译二进制，安装到 /usr/local/bin（可覆盖）。
set -euo pipefail

REPO="${CODIS_REPO:-xyanrch1024/Codis}"
INSTALL_DIR="${CODIS_INSTALL_DIR:-/usr/local/bin}"

# ---- 平台/架构检测 ----
case "$(uname -s)-$(uname -m)" in
  Linux-x86_64|Linux-amd64*)  OS=linux;  ARCH=x64 ;;
  Linux-aarch64|Linux-arm64*) OS=linux;  ARCH=arm64 ;;
  Darwin-x86_64|Darwin-amd64*) OS=macos; ARCH=x64 ;;
  Darwin-arm64)               OS=macos;  ARCH=arm64 ;;
  *)
    echo "error: unsupported platform $(uname -s)-$(uname -m)" >&2
    exit 1 ;;
esac

ASSET="codis-${OS}-${ARCH}.tar.gz"
URL="https://github.com/${REPO}/releases/latest/download/${ASSET}"

echo "==> Codis installer ($OS/$ARCH)"
echo "==> Downloading ${URL}"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

curl -fsSL --retry 3 "$URL" -o "$TMP/codis.tar.gz"
tar -xzf "$TMP/codis.tar.gz" -C "$TMP"

if [ ! -x "$TMP/codis" ] || [ ! -x "$TMP/codis-server" ]; then
  echo "error: release archive missing codis / codis-server binaries" >&2
  ls -la "$TMP" >&2
  exit 1
fi

# ---- 安装（必要时 sudo）----
mkdir -p "$INSTALL_DIR" 2>/dev/null || { echo "==> Need sudo for $INSTALL_DIR"; sudo mkdir -p "$INSTALL_DIR"; }

for bin in codis codis-server; do
  if [ -w "$INSTALL_DIR" ]; then
    install -m 0755 "$TMP/$bin" "$INSTALL_DIR/$bin"
  else
    echo "==> Need sudo to install $bin"
    sudo install -m 0755 "$TMP/$bin" "$INSTALL_DIR/$bin"
  fi
done

echo
cat <<'EOF'
██████╗ ██████╗ ██████╗ ██╗███████╗
  ██╔════╝██╔═══██╗██╔══██╗██║██╔════╝
  ██║     ██║   ██║██████╔╝██║███████╗
  ██║     ██║   ██║██╔══██╗██║╚════██║
  ╚██████╗╚██████╔╝██║  ██║██║███████║
   ╚═════╝ ╚═════╝ ╚═╝  ╚═╝╚═╝╚══════╝
EOF
echo
echo "==> Codis installed to $INSTALL_DIR"
echo
echo "    启动（自动拉起服务端）:"
echo "      export GLM_API_KEY=\"your-api-key\""
echo "      $INSTALL_DIR/codis"
echo
echo "    继续上次会话 / 指定模型:"
echo "      $INSTALL_DIR/codis -c"
echo "      $INSTALL_DIR/codis -m glm-4.5-flash"
echo
echo "    升级: 重新执行本脚本即可（安装到当前路径）"