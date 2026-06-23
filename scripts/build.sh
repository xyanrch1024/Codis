#!/bin/bash
# 一键构建脚本：设置环境 + CMake configure + build
set -e

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
VCPKG_ROOT="${VCPKG_ROOT:-$HOME/vcpkg}"
LOCAL_BIN="$HOME/.local/usr/bin"
LOCAL_LIB="$HOME/.local/usr/lib/x86_64-linux-gnu"

export PATH="$LOCAL_BIN:$PATH"
export LD_LIBRARY_PATH="$LOCAL_LIB:$LD_LIBRARY_PATH"

BUILD_DIR="${1:-$PROJECT_DIR/build}"
BUILD_TYPE="${2:-Release}"

echo "=== OpenCode C++ Build ==="
echo "Project:     $PROJECT_DIR"
echo "Build dir:   $BUILD_DIR"
echo "Build type:  $BUILD_TYPE"
echo "vcpkg root:  $VCPKG_ROOT"
echo ""

cmake -B "$BUILD_DIR" -S "$PROJECT_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"

cmake --build "$BUILD_DIR" -- -j$(nproc)

echo ""
echo "Build complete: $BUILD_DIR/packages/cli/opencode"
echo "Run: $PROJECT_DIR/bin/opencode --help"
