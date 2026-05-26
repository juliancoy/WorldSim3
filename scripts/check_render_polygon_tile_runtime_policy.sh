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

"$WORLD_SIM_BIN" --render-polygon-tile-runtime-policy-selftest >"$TMP_JSON"

python3 - "$TMP_JSON" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as fh:
    payload = json.load(fh)

assert payload["mode"] == "render-polygon-tile-runtime-policy-selftest"
assert payload["ok"] is True
assert payload["generic_polygon_zoom_10"] == "raster_only"
assert payload["generic_polygon_zoom_12"] == "raster_base_vector_outline"
assert payload["generic_polygon_zoom_14"] == "vector_only"
assert payload["heatmap_polygon_zoom_10"] == "vector_only"
assert payload["zoning_polygon_zoom_10"] == "vector_only"
assert payload["filtered_polygon_zoom_10"] == "vector_only"
assert payload["query_polygon_zoom_10"] == "vector_only"
assert payload["active_parcel_zoom_10"] == "vector_only"
assert payload["county_parcel_zoom_10"] == "raster_only"

print(json.dumps(payload, indent=2))
PY
