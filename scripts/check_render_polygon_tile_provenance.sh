#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

WORLD_SIM_BIN="${WORLD_SIM_BIN:-./build/worldsim3}"
if [[ ! -x "$WORLD_SIM_BIN" ]]; then
  echo "Missing executable: $WORLD_SIM_BIN" >&2
  exit 2
fi

TMP_VALIDATE="$(mktemp)"
TMP_RENDER_ONE="$(mktemp)"
TMP_RENDER_TWO="$(mktemp)"
trap 'rm -f "$TMP_VALIDATE" "$TMP_RENDER_ONE" "$TMP_RENDER_TWO"' EXIT

"$WORLD_SIM_BIN" --validate-polygon-geometry parcel.geojson >"$TMP_VALIDATE"
"$WORLD_SIM_BIN" --render-polygon-tile parcel.geojson 14 4821 6140 >"$TMP_RENDER_ONE"

python3 - "$TMP_VALIDATE" "$TMP_RENDER_ONE" <<'PY'
import json
import os
import sys

with open(sys.argv[1], "r", encoding="utf-8") as fh:
    validate = json.load(fh)
with open(sys.argv[2], "r", encoding="utf-8") as fh:
    render = json.load(fh)

assert validate["ok"] is True
assert render["ok"] is True
assert render["derived_cache"] is True
assert render["artifact_validated"] is True
assert render["artifact_source_signature"] == validate["source_signature"]
assert render["source_signature"] == validate["source_signature"]
assert render["cache_key"]["source_signature"] == render["source_signature"]
assert render["cache_key"]["render_route"] == render["render_path"]
assert render["tile_path"] == render["output_path"]
assert os.path.exists(render["tile_path"])

print(render["tile_path"])
PY

TILE_PATH="$(python3 - "$TMP_RENDER_ONE" <<'PY'
import json
import sys
with open(sys.argv[1], "r", encoding="utf-8") as fh:
    render = json.load(fh)
print(render["tile_path"])
PY
)"

rm -f "$TILE_PATH"
"$WORLD_SIM_BIN" --render-polygon-tile parcel.geojson 14 4821 6140 >"$TMP_RENDER_TWO"

python3 - "$TMP_RENDER_ONE" "$TMP_RENDER_TWO" "$TILE_PATH" <<'PY'
import json
import os
import sys

with open(sys.argv[1], "r", encoding="utf-8") as fh:
    first = json.load(fh)
with open(sys.argv[2], "r", encoding="utf-8") as fh:
    second = json.load(fh)
tile_path = sys.argv[3]

assert second["ok"] is True
assert second["tile_path"] == first["tile_path"]
assert second["source_signature"] == first["source_signature"]
assert second["cache_key"] == first["cache_key"]
assert os.path.exists(tile_path), tile_path

print(json.dumps({
    "ok": True,
    "tile_path": tile_path,
    "source_signature": second["source_signature"]
}, indent=2))
PY
