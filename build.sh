#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT_DIR"

install_packages() {
  local missing_name="$1"
  shift
  local packages=("$@")
  local installer=()

  if [[ "$(id -u)" -ne 0 ]]; then
    if ! command -v sudo >/dev/null 2>&1; then
      echo "[build.sh] $missing_name is required, but sudo is not available to install it." >&2
      exit 1
    fi
    installer=(sudo)
  fi

  if command -v apt-get >/dev/null 2>&1; then
    "${installer[@]}" apt-get update
    "${installer[@]}" apt-get install -y "${packages[@]}"
  elif command -v dnf >/dev/null 2>&1; then
    "${installer[@]}" dnf install -y "${packages[@]}"
  elif command -v yum >/dev/null 2>&1; then
    "${installer[@]}" yum install -y "${packages[@]}"
  elif command -v pacman >/dev/null 2>&1; then
    "${installer[@]}" pacman -Sy --noconfirm "${packages[@]}"
  elif command -v zypper >/dev/null 2>&1; then
    "${installer[@]}" zypper --non-interactive install "${packages[@]}"
  elif command -v brew >/dev/null 2>&1; then
    brew install "${packages[@]}"
  else
    echo "[build.sh] $missing_name is required, but no supported package manager was found." >&2
    exit 1
  fi
}

install_required_build_tools() {
  local packages=()

  if command -v apt-get >/dev/null 2>&1 && command -v dpkg-query >/dev/null 2>&1; then
    local apt_packages=(
      cmake
      g++
      ninja-build
      python3
      libvulkan-dev
      vulkan-tools
      libcurl4-openssl-dev
      libssl-dev
      libglfw3-dev
      xorg-dev
      libwayland-dev
    )

    for package in "${apt_packages[@]}"; do
      if ! dpkg-query -W -f='${Status}' "$package" 2>/dev/null | grep -q "install ok installed"; then
        packages+=("$package")
      fi
    done
  else
    if ! command -v cmake >/dev/null 2>&1; then
      packages+=(cmake)
    fi

    if ! command -v ninja >/dev/null 2>&1; then
      packages+=(ninja)
    fi
  fi

  if [[ ${#packages[@]} -eq 0 ]]; then
    return
  fi

  echo "[build.sh] Missing build dependencies: ${packages[*]}; attempting to install them."
  install_packages "build dependencies" "${packages[@]}"

  if ! command -v cmake >/dev/null 2>&1; then
    echo "[build.sh] cmake installation finished, but cmake is still not on PATH." >&2
    exit 1
  fi

  if ! command -v ninja >/dev/null 2>&1; then
    echo "[build.sh] ninja installation finished, but ninja is still not on PATH." >&2
    exit 1
  fi
}

usage() {
  cat <<USAGE
Usage: ./build.sh [--asan] [--clean] [--build-dir DIR]

Options:
  --asan            Enable AddressSanitizer + UndefinedBehaviorSanitizer (gated opt-in)
  --clean           Remove build directory before configure
  --build-dir DIR   Build directory (default: build)
  -h, --help        Show this help
USAGE
}

BUILD_DIR="build"
ENABLE_ASAN=0
CLEAN=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --asan)
      ENABLE_ASAN=1
      shift
      ;;
    --clean)
      CLEAN=1
      shift
      ;;
    --build-dir)
      BUILD_DIR="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

install_required_build_tools

if [[ $CLEAN -eq 1 ]]; then
  rm -rf "$BUILD_DIR"
fi

if [[ -f "$BUILD_DIR/CMakeCache.txt" ]] && grep -Eq '^(CMAKE_MAKE_PROGRAM|Vulkan_INCLUDE_DIR|Vulkan_LIBRARY|CURL_INCLUDE_DIR|CURL_LIBRARY|OPENSSL_CRYPTO_LIBRARY|OPENSSL_INCLUDE_DIR|OPENSSL_SSL_LIBRARY):.*NOTFOUND' "$BUILD_DIR/CMakeCache.txt"; then
  echo "[build.sh] Removing stale CMake cache with missing build dependencies."
  rm -f "$BUILD_DIR/CMakeCache.txt"
  rm -rf "$BUILD_DIR/CMakeFiles"
fi

EXTRA_CMAKE_IGNORE_PREFIX_PATH=()
GLFW3_DIR_OVERRIDE=""
LOCAL_GLFW_CONFIG_DIR="${HOME}/.local/lib/cmake/glfw3"
if [[ -d "$LOCAL_GLFW_CONFIG_DIR" ]] &&
   grep -Rqs 'lib/libglfw3\.a' "$LOCAL_GLFW_CONFIG_DIR" &&
   [[ ! -f "${HOME}/.local/lib/libglfw3.a" ]]; then
  echo "[build.sh] Ignoring stale GLFW package in ${LOCAL_GLFW_CONFIG_DIR}; using system package or FetchContent instead."
  EXTRA_CMAKE_IGNORE_PREFIX_PATH+=("${HOME}/.local")
  for glfw_config_dir in \
    "/usr/lib/$(gcc -print-multiarch 2>/dev/null || true)/cmake/glfw3" \
    /usr/lib/*/cmake/glfw3 \
    /usr/lib/cmake/glfw3 \
    /usr/local/lib/cmake/glfw3
  do
    if [[ -f "${glfw_config_dir}/glfw3Config.cmake" ]]; then
      GLFW3_DIR_OVERRIDE="$glfw_config_dir"
      break
    fi
  done
fi

if [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  current_gen="$(grep -E '^CMAKE_GENERATOR:INTERNAL=' "$BUILD_DIR/CMakeCache.txt" | sed 's/^[^=]*=//')"
  if [[ "$current_gen" != "Ninja" ]]; then
    if [[ "$BUILD_DIR" == "build" && "$current_gen" == "Unix Makefiles" ]]; then
      if [[ ! -e "build-cmake" ]]; then
        echo "[build.sh] Moving existing Unix Makefiles build tree: build -> build-cmake"
        mv build build-cmake
      fi
    else
    echo "Build directory '$BUILD_DIR' uses generator '$current_gen', expected 'Ninja'." >&2
    echo "Re-run with --clean or choose a different --build-dir." >&2
    exit 1
    fi
  fi
fi

CMAKE_ARGS=(
  -S .
  -B "$BUILD_DIR"
  -G Ninja
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
)

if [[ ${#EXTRA_CMAKE_IGNORE_PREFIX_PATH[@]} -gt 0 ]]; then
  IFS=';'
  CMAKE_ARGS+=("-DCMAKE_IGNORE_PREFIX_PATH=${EXTRA_CMAKE_IGNORE_PREFIX_PATH[*]}")
  unset IFS
fi

if [[ -n "$GLFW3_DIR_OVERRIDE" ]]; then
  CMAKE_ARGS+=("-Dglfw3_DIR=${GLFW3_DIR_OVERRIDE}")
fi

if [[ $ENABLE_ASAN -eq 1 ]]; then
  SAN_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
  CMAKE_ARGS+=(
    -DCMAKE_BUILD_TYPE=Debug
    "-DCMAKE_C_FLAGS=${SAN_FLAGS}"
    "-DCMAKE_CXX_FLAGS=${SAN_FLAGS}"
    "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined"
  )
  echo "[build.sh] ASan/UBSan ENABLED"
else
  echo "[build.sh] ASan/UBSan disabled (use --asan to enable)"
fi

cmake "${CMAKE_ARGS[@]}"
cmake --build "$BUILD_DIR"
