#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

WORLD_SIM_BIN="${WORLD_SIM_BIN:-./build/worldsim3}"
if [[ ! -x "$WORLD_SIM_BIN" ]]; then
  echo "Missing executable: $WORLD_SIM_BIN" >&2
  exit 2
fi

TMP_JSON="$(mktemp)"
trap 'rm -f "$TMP_JSON"' EXIT

"$WORLD_SIM_BIN" --render-polygon-tile parcel.geojson 14 4821 6140 >"$TMP_JSON"

python3 - "$TMP_JSON" <<'PY'
import json
import os
import sys

with open(sys.argv[1], "r", encoding="utf-8") as fh:
    payload = json.load(fh)

assert payload["mode"] == "render-polygon-tile"
assert payload["ok"] is True
assert payload["file"] == "parcel.geojson"
assert payload["render_path"] == "parcel_polygon_gpu"
assert payload["source_signature"], "missing source_signature"
assert payload["style_key"], "missing style_key"
assert payload["tile"] == {"z": 14, "x": 4821, "y": 6140}

output_path = payload["output_path"]
assert os.path.exists(output_path), f"missing output tile: {output_path}"
assert output_path.endswith("/z14/4821_6140.ppm"), output_path
assert "/render_tiles/" in output_path, output_path
assert "/parcel.geojson/" in output_path, output_path
assert "/parcel_polygon_gpu/" in output_path, output_path
assert f"/source_{payload['source_signature'].replace('/', '_').replace(' ', '_')}" in output_path or "/source_" in output_path
assert f"/style_{payload['style_key']}" in output_path, output_path

with open(output_path, "rb") as fh:
    magic = fh.read(2)
assert magic == b"P6", magic

print(json.dumps({
    "ok": True,
    "output_path": output_path,
    "style_key": payload["style_key"]
}, indent=2))
PY
