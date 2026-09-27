#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${ROOT}/build"

mkdir -p "${ROOT}/bin"
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"

cmake -S "${ROOT}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build "${BUILD_DIR}" -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)"

echo "Build complete: ${ROOT}/bin/libvlua.so"
if [ -x "${ROOT}/bin/vlua" ]; then
  echo "Tool built:    ${ROOT}/bin/vlua"
fi
echo "Lua 5.3.6:     ${BUILD_DIR}/lua/lua53"
