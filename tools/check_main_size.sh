#!/usr/bin/env bash
set -euo pipefail
lines=$(wc -l < main.cpp)
if (( lines > 2000 )); then
  echo "main.cpp has ${lines} lines; limit is 2000" >&2
  exit 1
fi
