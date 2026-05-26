#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

WORLD_SIM_BIN="${WORLD_SIM_BIN:-./build/worldsim3}"

MODE="${1:-}"
if [[ -z "$MODE" ]]; then
  echo "Usage: $0 <parcels-required|zoning-audit>" >&2
  exit 2
fi

if [[ ! -x "$WORLD_SIM_BIN" ]]; then
  echo "Missing executable: $WORLD_SIM_BIN" >&2
  exit 2
fi

parcel_layers=(
  "parcel.geojson"
  "baltimore_county_parcels.geojson"
  "howard_county_parcels.geojson"
  "anne_arundel_county_parcels.geojson"
  "harford_county_parcels.geojson"
  "carroll_county_parcels.geojson"
  "prince_georges_county_parcels.geojson"
  "cecil_county_parcels.geojson"
  "frederick_county_parcels.geojson"
  "montgomery_county_parcels.geojson"
  "charles_county_parcels.geojson"
  "calvert_county_parcels.geojson"
  "queen_annes_county_parcels.geojson"
  "talbot_county_parcels.geojson"
  "kent_county_parcels.geojson"
  "caroline_county_parcels.geojson"
)

zoning_layers=(
  "zoning.geojson"
  "baltimore_county_zoning.geojson"
  "howard_county_zoning.geojson"
  "anne_arundel_county_zoning.geojson"
  "prince_georges_county_zoning.geojson"
  "montgomery_county_zoning.geojson"
)

case "$MODE" in
  parcels-required)
    layers=("${parcel_layers[@]}")
    failure_label="required"
    ;;
  zoning-audit|zoning-required)
    layers=("${zoning_layers[@]}")
    if [[ "$MODE" == "zoning-required" ]]; then
      failure_label="required"
    else
      failure_label="audit"
    fi
    ;;
  *)
    echo "Unknown mode: $MODE" >&2
    exit 2
    ;;
esac

tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT

ok_count=0
failed_count=0

for layer in "${layers[@]}"; do
  out_path="$tmp_dir/${layer//\//_}.json"
  layer_status=0
  if ! "$WORLD_SIM_BIN" --validate-polygon-geometry "$layer" >"$out_path"; then
    layer_status=$?
  fi
  if python3 - "$out_path" "$layer_status" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as fh:
    payload = json.load(fh)

if payload.get("ok"):
    print(f"PASS {payload.get('file')} features={payload.get('features', 0)} chunks={payload.get('chunks', 0)}")
    sys.exit(0)

error = payload.get("error", "unknown error")
returncode = int(sys.argv[2])
print(f"FAIL {payload.get('file')} error={error} returncode={returncode}")
sys.exit(1)
PY
  then
    ((ok_count += 1))
  else
    ((failed_count += 1))
  fi
done

echo "Summary mode=$MODE ok=$ok_count failed=$failed_count"

if [[ "$failure_label" == "required" && "$failed_count" -ne 0 ]]; then
  exit 1
fi

exit 0
