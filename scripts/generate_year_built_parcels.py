#!/usr/bin/env python3
"""Build a parcel polygon choropleth layer for real-property YEAR_BUILD."""

import json
from datetime import date
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LAYERS = ROOT / "data" / "provenance" / "stored" / "world" / "earth" / "nation_state" / "us" / "state_region" / "md" / "county_city" / "baltimore_city" / "layers"
PARCEL_PATH = LAYERS / "parcel.geojson"
REAL_PROPERTY_PATH = LAYERS / "real_property_information.geojson"
OUT_PATH = LAYERS / "year_built_parcels.geojson"


def normalize_join_key(value: str) -> str:
    if value is None:
        return ""
    return "".join(ch.upper() for ch in str(value) if ch.isalnum())


def to_int(value) -> int:
    if value is None:
        return 0
    if isinstance(value, int):
        return value
    if isinstance(value, float):
        return int(value)
    s = str(value).strip().replace(",", "")
    if not s:
        return 0
    try:
        return int(float(s))
    except Exception:
        return 0


def valid_year_built(year: int) -> bool:
    return 1600 <= year <= date.today().year + 1


def main() -> int:
    if not PARCEL_PATH.exists():
        print(f"missing parcel dataset: {PARCEL_PATH}")
        return 1
    if not REAL_PROPERTY_PATH.exists():
        print(f"missing real property dataset: {REAL_PROPERTY_PATH}")
        return 1

    parcel = json.loads(PARCEL_PATH.read_text())
    real_property = json.loads(REAL_PROPERTY_PATH.read_text())

    year_by_blocklot: dict[str, int] = {}
    for feat in real_property.get("features", []):
        props = feat.get("properties", {})
        blocklot = normalize_join_key(props.get("BLOCKLOT"))
        if not blocklot:
            continue
        year = to_int(props.get("YEAR_BUILD"))
        if not valid_year_built(year):
            continue
        prev = year_by_blocklot.get(blocklot)
        if prev is None or year < prev:
            year_by_blocklot[blocklot] = year

    current_year = date.today().year
    out_features = []
    matched = 0
    for feat in parcel.get("features", []):
        props = dict(feat.get("properties", {}))
        blocklot = normalize_join_key(props.get("BLOCKLOT"))
        year = year_by_blocklot.get(blocklot, 0)
        if year > 0:
            matched += 1
        props["year_built"] = year
        props["building_age_years"] = (current_year - year) if year > 0 else 0
        props["year_built_source"] = "real_property_information.YEAR_BUILD" if year > 0 else ""
        out_features.append({
            "type": "Feature",
            "geometry": feat.get("geometry"),
            "properties": props,
        })

    out = {
        "type": "FeatureCollection",
        "name": "Year Built (Parcels)",
        "metadata": {
            "derived_from": ["parcel.geojson", "real_property_information.geojson"],
            "join_key": "normalized BLOCKLOT",
            "heatmap_field": "year_built",
            "source_field": "YEAR_BUILD",
            "generated_by": "scripts/generate_year_built_parcels.py",
        },
        "features": out_features,
    }
    OUT_PATH.write_text(json.dumps(out))
    print(f"wrote: {OUT_PATH}")
    print(f"parcel features: {len(out_features)}")
    print(f"matched year-built records: {matched}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
