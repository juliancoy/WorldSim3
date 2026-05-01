#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT_DIR"

usage() {
  cat <<USAGE
Usage: ./build.sh [--asan] [--clean] [--build-dir DIR]

Options:
  --asan            Enable AddressSanitizer + UndefinedBehaviorSanitizer (gated opt-in)
  --clean           Remove build directory before configure
  --build-dir DIR   Build directory (default: build-ninja)
  -h, --help        Show this help
USAGE
}

BUILD_DIR="build-ninja"
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

if [[ $CLEAN -eq 1 ]]; then
  rm -rf "$BUILD_DIR"
fi

if [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  current_gen="$(grep -E '^CMAKE_GENERATOR:INTERNAL=' "$BUILD_DIR/CMakeCache.txt" | sed 's/^[^=]*=//')"
  if [[ "$current_gen" != "Ninja" ]]; then
    echo "Build directory '$BUILD_DIR' uses generator '$current_gen', expected 'Ninja'." >&2
    echo "Re-run with --clean or choose a different --build-dir." >&2
    exit 1
  fi
fi

CMAKE_ARGS=(
  -S .
  -B "$BUILD_DIR"
  -G Ninja
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
)

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

# Compatibility path for older commands: ./build/bmore_vulkan
mkdir -p build
ln -sfn "../$BUILD_DIR/bmore_vulkan" build/bmore_vulkan
