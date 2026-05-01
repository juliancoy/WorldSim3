#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT_DIR"

python3 download_layers.py
python3 download_tiles.py

cmake -S . -B build
cmake --build build -j

./build/bmore_vulkan
