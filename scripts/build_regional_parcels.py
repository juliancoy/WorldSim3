#!/usr/bin/env python3
"""Normalize county parcel GeoJSON files into one regional parcel layer.

Inputs can be any GeoJSON FeatureCollection with polygon/multipolygon geometry.
The script copies source attributes and adds canonical fields consumed by the app:

  jurisdiction, source_file, source_parcel_id, account_id, blocklot, address,
  owner, land_value, improvement_value, current_value, sale_price, sale_date,
  year_built, sdat_link

Example:
  python3 scripts/build_regional_parcels.py \
    --input BaltimoreCity:data/layers/parcel.geojson \
    --input BaltimoreCounty:data/inbox/baltimore_county/parcels.geojson \
    --input HowardCounty:data/inbox/howard_county/parcels.geojson
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any, Iterable

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "data" / "layers" / "regional_parcels.geojson"

FIELD_ALIASES = {
    "source_parcel_id": [
        "source_parcel_id", "ACCOUNTID", "ACCOUNT_ID", "ACCTID", "ACCT_ID", "ACCOUNT", "ACCOUNTNO",
        "PARCELID", "PARCEL_ID", "PARCELNO", "PIN", "MAP_PARCEL", "MAPLOT", "OBJECTID", "OBJECTID_1",
    ],
    "account_id": ["account_id", "ACCOUNTID", "ACCOUNT_ID", "ACCTID", "ACCT_ID", "ACCOUNT", "ACCOUNTNO", "SDAT_ACCOUNT"],
    "blocklot": ["blocklot", "BLOCKLOT", "BLOCK_LOT", "BlockLot", "PIN", "PARCELID", "PARCEL_ID", "MAPLOT"],
    "address": [
        "address", "FULLADDR", "FULL_ADDRESS", "PROPERTY_ADDRESS", "PROPERTYADDR", "PREMISEADD",
        "PREMISE_ADDRESS", "ADDRESS", "Address", "ADDR", "ADDR1", "ADDRESS1", "SITE_ADDR",
        "SITUSADDR", "LOCATION", "Location", "SITUS_ADDRESS",
    ],
    "owner": ["owner", "owner_name", "OWNER_1", "OWNERNME1", "OWNER", "OWNER_NAME", "AR_OWNER", "OWNER_ABBR", "OWNNAME"],
    "land_value": ["land_value", "CURRLAND", "LAND_VALUE", "LANDVAL", "LAND_VAL"],
    "improvement_value": ["improvement_value", "CURRIMPR", "IMPR_VALUE", "IMPROVEMENT_VALUE", "IMPROVEVAL", "BLDG_VALUE"],
    "current_value": ["current_value", "TAXBASE", "ARTAXBAS", "TOTAL_VALUE", "TOTALVAL", "ASSD_VALUE", "ASSESSMENT", "MARKET_VALUE"],
    "sale_price": ["sale_price", "SALEPRIC", "SALE_PRICE", "SALEAMT", "SALE_AMOUNT"],
    "sale_date": ["sale_date", "SALEDATE", "SALE_DATE", "LAST_SALE_DATE"],
    "year_built": ["year_built", "YEAR_BUILD", "YEARBUILT", "YR_BUILT", "BUILT", "BLDG_YEAR"],
    "sdat_link": ["sdat_link", "SDATLINK", "SDAT_LINK", "PROPERTY_LINK"],
}


def norm_key(value: Any) -> str:
    return re.sub(r"[^0-9A-Za-z]", "", str(value or "")).upper()


def clean_text(value: Any) -> str:
    if value is None:
        return ""
    s = str(value).strip()
    return " ".join(s.split())


def first_prop(props: dict[str, Any], keys: Iterable[str]) -> Any:
    lowered = {k.lower(): k for k in props}
    for key in keys:
        if key in props and props[key] not in (None, ""):
            return props[key]
        actual = lowered.get(key.lower())
        if actual and props[actual] not in (None, ""):
            return props[actual]
    return ""


def parse_number(value: Any) -> float:
    if value is None or value == "":
        return 0.0
    if isinstance(value, (int, float)):
        return float(value)
    s = re.sub(r"[^0-9.\-]", "", str(value))
    if s in ("", "-", "."):
        return 0.0
    try:
        return float(s)
    except ValueError:
        return 0.0


def canonical_props(jurisdiction: str, source_file: Path, props: dict[str, Any], property_props: dict[str, Any] | None = None) -> dict[str, Any]:
    merged = dict(props)
    if property_props:
        for k, v in property_props.items():
            if k not in merged or merged[k] in (None, ""):
                merged[k] = v
    out = dict(merged)
    out["jurisdiction"] = jurisdiction
    out["source_file"] = source_file.name
    for field, aliases in FIELD_ALIASES.items():
        raw = first_prop(merged, aliases)
        if field in {"land_value", "improvement_value", "current_value", "sale_price"}:
            out[field] = parse_number(raw)
        elif field == "year_built":
            n = parse_number(raw)
            out[field] = int(n) if n > 0 else 0
        else:
            out[field] = clean_text(raw)
    if not out["blocklot"]:
        out["blocklot"] = out["source_parcel_id"] or out["account_id"]
    out["regional_parcel_id"] = f"{jurisdiction}:{norm_key(out['blocklot'] or out['source_parcel_id'] or out['account_id'])}"
    return out


def read_features(path: Path) -> list[dict[str, Any]]:
    with path.open("r", encoding="utf-8") as f:
        data = json.load(f)
    if data.get("type") != "FeatureCollection" or not isinstance(data.get("features"), list):
        raise SystemExit(f"{path} is not a GeoJSON FeatureCollection")
    return data["features"]


def parse_input(value: str) -> tuple[str, Path]:
    if ":" not in value:
        raise argparse.ArgumentTypeError("inputs must be Jurisdiction:path.geojson")
    jurisdiction, path = value.split(":", 1)
    jurisdiction = jurisdiction.strip()
    if not jurisdiction:
        raise argparse.ArgumentTypeError("jurisdiction is required")
    p = Path(path).expanduser()
    if not p.is_absolute():
        p = ROOT / p
    return jurisdiction, p


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", action="append", type=parse_input, required=True, help="Jurisdiction:path.geojson")
    parser.add_argument("--property-input", action="append", type=parse_input, default=[], help="Optional Jurisdiction:path.geojson property/assessment records joined by normalized parcel key")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    out_path = args.output if args.output.is_absolute() else ROOT / args.output
    property_indexes: dict[str, dict[str, dict[str, Any]]] = {}
    for jurisdiction, path in args.property_input:
        idx: dict[str, dict[str, Any]] = {}
        for feature in read_features(path):
            props = feature.get("properties") or {}
            if not isinstance(props, dict):
                continue
            key_props = canonical_props(jurisdiction, path, props)
            key = norm_key(key_props.get("blocklot") or key_props.get("source_parcel_id") or key_props.get("account_id"))
            if key and key not in idx:
                idx[key] = props
        property_indexes[jurisdiction] = idx

    features: list[dict[str, Any]] = []
    counts: dict[str, int] = {}
    for jurisdiction, path in args.input:
        source_features = read_features(path)
        counts[jurisdiction] = len(source_features)
        for feature in source_features:
            if not isinstance(feature, dict):
                continue
            geometry = feature.get("geometry")
            if not geometry:
                continue
            props = feature.get("properties") or {}
            if not isinstance(props, dict):
                props = {}
            base_key_props = canonical_props(jurisdiction, path, props)
            lookup_key = norm_key(base_key_props.get("blocklot") or base_key_props.get("source_parcel_id") or base_key_props.get("account_id"))
            joined_props = property_indexes.get(jurisdiction, {}).get(lookup_key)
            regional_props = canonical_props(jurisdiction, path, props, joined_props)
            features.append({
                "type": "Feature",
                "id": regional_props["regional_parcel_id"],
                "properties": regional_props,
                "geometry": geometry,
            })

    collection = {
        "type": "FeatureCollection",
        "name": "regional_parcels",
        "metadata": {
            "schema_version": 1,
            "generated_by": "scripts/build_regional_parcels.py",
            "source_counts": counts,
            "canonical_fields": list(FIELD_ALIASES.keys()) + ["jurisdiction", "source_file", "regional_parcel_id"],
        },
        "features": features,
    }
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("w", encoding="utf-8") as f:
        json.dump(collection, f, separators=(",", ":"))
    print(f"wrote {out_path} with {len(features)} features")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
