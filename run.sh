#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT_DIR"

if [[ "${WORLD_SIM3_PRELOAD_DATA:-0}" == "1" ]]; then
  export WORLD_SIM3_PRELOAD_DATA=1
  export WORLD_SIM3_PRELOAD_PHASE="${WORLD_SIM3_PRELOAD_PHASE:-all}"
fi

cmake -S . -B build-cmake
cmake --build build-cmake -j

./build-cmake/worldsim3
