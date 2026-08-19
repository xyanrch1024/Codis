#!/usr/bin/env bash
# Codis one-click installer
# Usage: curl -fsSL https://codis.chat/install | bash
# Downloads prebuilt binaries from GitHub Releases and installs them to /usr/local/bin (overwritable).
#
# Uninstall: curl -fsSL https://codis.chat/install | bash -s -- --uninstall
set -euo pipefail

REPO="${CODIS_REPO:-xyanrch1024/Codis}"
INSTALL_DIR="${CODIS_INSTALL_DIR:-/usr/local/bin}"
# Config dir: defaults to <bin parent>/etc/codis, i.e. /usr/local/etc/codis
CONFIG_DIR="${CODIS_CONFIG_DIR:-$(dirname "$INSTALL_DIR")/etc/codis}"

# ---- Argument parsing ----
UNINSTALL=false
case "${1:-}" in
  --uninstall|-u|uninstall) UNINSTALL=true ;;
esac

# ---- Uninstall flow ----
if [ "$UNINSTALL" = true ]; then
  echo "==> Codis uninstaller (removing from $INSTALL_DIR)"
  removed=false
  for bin in codis codis-server; do
    if [ -e "$INSTALL_DIR/$bin" ]; then
      if [ -w "$INSTALL_DIR" ]; then
        rm -f "$INSTALL_DIR/$bin"
      else
        echo "==> Need sudo to remove $bin"
        sudo rm -f "$INSTALL_DIR/$bin"
      fi
      echo "    removed $INSTALL_DIR/$bin"
      removed=true
    fi
  done
  if [ -e "$CONFIG_DIR/config.toml" ]; then
    if [ -w "$(dirname "$CONFIG_DIR")" ]; then
      rm -f "$CONFIG_DIR/config.toml"
    else
      echo "==> Need sudo to remove config"
      sudo rm -f "$CONFIG_DIR/config.toml"
    fi
    echo "    removed $CONFIG_DIR/config.toml"
    rmdir "$CONFIG_DIR" 2>/dev/null || true
    removed=true
  fi
  if [ "$removed" = false ]; then
    echo "==> No Codis binaries or config found (nothing to remove)"
  else
    echo "==> Codis uninstalled"
  fi
  exit 0
fi

# ---- Platform/arch detection ----
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

# ---- Install (with sudo if needed) ----
mkdir -p "$INSTALL_DIR" 2>/dev/null || { echo "==> Need sudo for $INSTALL_DIR"; sudo mkdir -p "$INSTALL_DIR"; }

for bin in codis codis-server; do
  if [ -w "$INSTALL_DIR" ]; then
    install -m 0755 "$TMP/$bin" "$INSTALL_DIR/$bin"
  else
    echo "==> Need sudo to install $bin"
    sudo install -m 0755 "$TMP/$bin" "$INSTALL_DIR/$bin"
  fi
done

# ---- Install config (skip if exists, to avoid overwriting user config) ----
if [ -f "$TMP/config.toml" ]; then
  mkdir -p "$CONFIG_DIR" 2>/dev/null || { echo "==> Need sudo for $CONFIG_DIR"; sudo mkdir -p "$CONFIG_DIR"; }
  if [ -e "$CONFIG_DIR/config.toml" ]; then
    echo "==> Config exists, keeping $CONFIG_DIR/config.toml"
  else
    if [ -w "$CONFIG_DIR" ]; then
      install -m 0644 "$TMP/config.toml" "$CONFIG_DIR/config.toml"
    else
      echo "==> Need sudo to install config"
      sudo install -m 0644 "$TMP/config.toml" "$CONFIG_DIR/config.toml"
    fi
    echo "==> Config installed to $CONFIG_DIR/config.toml"
  fi
else
  echo "==> No config.toml in release archive, skipping"
fi

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
echo "==> Config at $CONFIG_DIR/config.toml"
echo
echo "    Start (auto-launches the server):"
echo "      export GLM_API_KEY=\"your-api-key\""
echo "      $INSTALL_DIR/codis"
echo
echo "    Resume last session / pick a model:"
echo "      $INSTALL_DIR/codis -c"
echo "      $INSTALL_DIR/codis -m glm-4.5-flash"
echo
echo "    Configure:"
echo "      Config file: $CONFIG_DIR/config.toml"
echo "      Set your API keys via env vars (see config.toml [providers] api_key_env):"
echo "        export OPENAI_API_KEY=\"sk-...\""
echo "        export DEEPSEEK_API_KEY=\"sk-...\""
echo "        export GLM_API_KEY=\"your-api-key\""
echo "      Edit $CONFIG_DIR/config.toml to change providers, models, permissions,"
echo "      websearch backend, MCP servers, etc. (it is preserved on upgrade)."
echo
echo "    Upgrade: re-run this script (installs into the current path)"
echo "    Uninstall: curl -fsSL https://codis.chat/install | bash -s -- --uninstall"