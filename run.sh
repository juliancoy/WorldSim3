#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT_DIR"

if [[ "${WORLD_SIM3_PRELOAD_DATA:-0}" == "1" ]]; then
  python3 download_layers.py
  python3 download_tiles.py
fi

cmake -S . -B build-cmake
cmake --build build-cmake -j

./build-cmake/worldsim3
