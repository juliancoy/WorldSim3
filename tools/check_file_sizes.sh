#!/usr/bin/env bash
set -euo pipefail
limit=2000
failed=0
while IFS= read -r -d '' file; do
  case "$file" in
    ./build/*|./build-*/*|./data/*|./stb_image.h|./earcut.hpp) continue ;;
  esac
  lines=$(wc -l < "$file")
  if (( lines > limit )); then
    echo "$file has ${lines} lines; limit is ${limit}" >&2
    failed=1
  fi
done < <(find . -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' -o -name '*.inc' \) -print0)
exit "$failed"
