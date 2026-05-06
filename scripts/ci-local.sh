#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

SHA_TAG="${1:-local}"
JOBS="${JOBS:-$(nproc)}"
AUTO_INSTALL_DEPS="${AUTO_INSTALL_DEPS:-1}"

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "Missing required command: $1" >&2
    exit 1
  }
}

echo "[ci-local] Using SHA tag: $SHA_TAG"
echo "[ci-local] Parallel jobs: $JOBS"

# Resolve MinGW Vulkan SDK/library hints for local cross-builds.
# Prefer explicit env vars; fallback to common locations or vcpkg x64-mingw-dynamic.
resolve_mingw_vulkan() {
  local default_inc="/usr/include"
  local default_lib_a="/usr/x86_64-w64-mingw32/lib/libvulkan-1.a"
  local default_lib_lib="/usr/x86_64-w64-mingw32/lib/vulkan-1.lib"

  MINGW_VULKAN_INCLUDE_DIR="${MINGW_VULKAN_INCLUDE_DIR:-}"
  MINGW_VULKAN_LIBRARY="${MINGW_VULKAN_LIBRARY:-}"

  if [[ -z "${MINGW_VULKAN_INCLUDE_DIR}" && -d "${default_inc}/vulkan" ]]; then
    MINGW_VULKAN_INCLUDE_DIR="${default_inc}"
  fi
  if [[ -z "${MINGW_VULKAN_LIBRARY}" && -f "${default_lib_a}" ]]; then
    MINGW_VULKAN_LIBRARY="${default_lib_a}"
  elif [[ -z "${MINGW_VULKAN_LIBRARY}" && -f "${default_lib_lib}" ]]; then
    MINGW_VULKAN_LIBRARY="${default_lib_lib}"
  fi

  if [[ -z "${MINGW_VULKAN_INCLUDE_DIR}" || -z "${MINGW_VULKAN_LIBRARY}" ]]; then
    local vcpkg_root="${VCPKG_ROOT:-}"
    local triplet="${VCPKG_TARGET_TRIPLET:-x64-mingw-dynamic}"
    if [[ -n "${vcpkg_root}" ]]; then
      local vcpkg_inc="${vcpkg_root}/installed/${triplet}/include"
      local vcpkg_lib_a="${vcpkg_root}/installed/${triplet}/lib/libvulkan-1.a"
      local vcpkg_lib_lib="${vcpkg_root}/installed/${triplet}/lib/vulkan-1.lib"
      if [[ -z "${MINGW_VULKAN_INCLUDE_DIR}" && -d "${vcpkg_inc}/vulkan" ]]; then
        MINGW_VULKAN_INCLUDE_DIR="${vcpkg_inc}"
      fi
      if [[ -z "${MINGW_VULKAN_LIBRARY}" && -f "${vcpkg_lib_a}" ]]; then
        MINGW_VULKAN_LIBRARY="${vcpkg_lib_a}"
      elif [[ -z "${MINGW_VULKAN_LIBRARY}" && -f "${vcpkg_lib_lib}" ]]; then
        MINGW_VULKAN_LIBRARY="${vcpkg_lib_lib}"
      fi
    fi
  fi

  if [[ -z "${MINGW_VULKAN_INCLUDE_DIR}" || -z "${MINGW_VULKAN_LIBRARY}" ]]; then
    echo "[ci-local] ERROR: MinGW Windows build requires Vulkan headers + loader import library." >&2
    echo "[ci-local] Missing one or both of:" >&2
    echo "  MINGW_VULKAN_INCLUDE_DIR (directory containing vulkan/vulkan.h)" >&2
    echo "  MINGW_VULKAN_LIBRARY (vulkan-1 import library for MinGW)" >&2
    echo "" >&2
    echo "[ci-local] Recommended paths:" >&2
    echo "  1) CI/Release: use GitHub Actions windows-latest (MSVC) workflow." >&2
    echo "  2) Local MinGW: install via vcpkg and export:" >&2
    echo "     export VCPKG_ROOT=/path/to/vcpkg" >&2
    echo "     \$VCPKG_ROOT/vcpkg install vulkan:x64-mingw-dynamic curl:x64-mingw-dynamic glfw3:x64-mingw-dynamic" >&2
    echo "     export MINGW_VULKAN_INCLUDE_DIR=\$VCPKG_ROOT/installed/x64-mingw-dynamic/include" >&2
    echo "     export MINGW_VULKAN_LIBRARY=\$VCPKG_ROOT/installed/x64-mingw-dynamic/lib/libvulkan-1.a" >&2
    exit 1
  fi
}

install_deps_if_needed() {
  local missing=0
  command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1 || missing=1
  command -v ninja >/dev/null 2>&1 || missing=1
  pkg-config --exists vulkan 2>/dev/null || missing=1
  pkg-config --exists glfw3 2>/dev/null || missing=1
  pkg-config --exists libcurl 2>/dev/null || missing=1

  if [[ "$missing" -eq 0 ]]; then
    return
  fi

  if [[ "$AUTO_INSTALL_DEPS" != "1" ]]; then
    echo "Missing build dependencies and AUTO_INSTALL_DEPS=0." >&2
    echo "Install manually or rerun with AUTO_INSTALL_DEPS=1." >&2
    exit 1
  fi

  echo "[ci-local] Installing required dependencies via apt"
  if command -v sudo >/dev/null 2>&1; then
    sudo apt-get update
    sudo apt-get install -y --no-install-recommends \
      build-essential \
      ninja-build \
      cmake \
      wget \
      file \
      dpkg-dev \
      zip \
      pkg-config \
      libvulkan-dev \
      libglfw3-dev \
      libcurl4-openssl-dev \
      libssl-dev \
      mingw-w64
  else
    apt-get update
    apt-get install -y --no-install-recommends \
      build-essential \
      ninja-build \
      cmake \
      wget \
      file \
      dpkg-dev \
      zip \
      pkg-config \
      libvulkan-dev \
      libglfw3-dev \
      libcurl4-openssl-dev \
      libssl-dev \
      mingw-w64
  fi
}

install_deps_if_needed
need_cmd cmake
need_cmd tar
need_cmd zip

if [[ -f "build-windows-mingw/CMakeCache.txt" ]]; then
  if ! grep -q "CMAKE_HOME_DIRECTORY:INTERNAL=${ROOT_DIR}" "build-windows-mingw/CMakeCache.txt"; then
    echo "[ci-local] Removing stale build dir: build-windows-mingw"
    rm -rf build-windows-mingw
  fi
fi

# Linux packaging (.deb + AppImage)
echo "[ci-local] Building Linux packages (.deb + AppImage)"
JOBS="$JOBS" ./scripts/package-linux.sh "$SHA_TAG"

# MinGW build
need_cmd x86_64-w64-mingw32-gcc
need_cmd x86_64-w64-mingw32-g++
need_cmd x86_64-w64-mingw32-windres

echo "[ci-local] Configuring Windows (MinGW) build"
resolve_mingw_vulkan
CC=x86_64-w64-mingw32-gcc \
CXX=x86_64-w64-mingw32-g++ \
RC=x86_64-w64-mingw32-windres \
cmake -S . -B build-windows-mingw -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
  -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ \
  -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres \
  -DVulkan_INCLUDE_DIR="${MINGW_VULKAN_INCLUDE_DIR}" \
  -DVulkan_LIBRARY="${MINGW_VULKAN_LIBRARY}"

echo "[ci-local] Building Windows (MinGW) targets"
cmake --build build-windows-mingw --config Release -j "$JOBS"

echo "[ci-local] Packaging Windows artifacts"
mkdir -p dist/windows
cp -v build-windows-mingw/worldsim3.exe dist/windows/
cp -v build-windows-mingw/arkavo_connectivity_test.exe dist/windows/
(
  cd dist
  zip -r "worldsim3-${SHA_TAG}-windows-mingw.zip" windows
)

echo "[ci-local] Done"
echo "[ci-local] Produced:"
echo "  dist/worldsim3-${SHA_TAG}.deb"
echo "  dist/worldsim3-${SHA_TAG}.AppImage"
echo "  dist/worldsim3-${SHA_TAG}-windows-mingw.zip"
