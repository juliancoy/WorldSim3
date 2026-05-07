#!/usr/bin/env python3
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LAYERS = ROOT / "data" / "layers"
PARCEL_PATH = LAYERS / "parcel.geojson"
REAL_PROPERTY_PATH = LAYERS / "real_property_information.geojson"
OUT_PATH = LAYERS / "property_value_parcels.geojson"


def normalize_join_key(value: str) -> str:
    if value is None:
        return ""
    return "".join(ch.upper() for ch in str(value) if ch.isalnum())


def to_float(value) -> float:
    if value is None:
        return 0.0
    if isinstance(value, (int, float)):
        return float(value)
    s = str(value).strip().replace(",", "").replace("$", "")
    if not s:
        return 0.0
    try:
        return float(s)
    except Exception:
        return 0.0


def select_value(props: dict) -> tuple[float, str]:
    tax_base = to_float(props.get("TAXBASE"))
    if tax_base <= 0.0:
        tax_base = to_float(props.get("ARTAXBAS"))
    curr_land = to_float(props.get("CURRLAND"))
    curr_impr = to_float(props.get("CURRIMPR"))
    sale_pric = to_float(props.get("SALEPRIC"))
    if tax_base > 0.0:
        return tax_base, "tax_base"
    if (curr_land + curr_impr) > 0.0:
        return curr_land + curr_impr, "curr_land_plus_impr"
    if sale_pric > 0.0:
        return sale_pric, "sale_price"
    return 0.0, ""


def main() -> int:
    if not PARCEL_PATH.exists():
        print(f"missing parcel dataset: {PARCEL_PATH}")
        return 1
    if not REAL_PROPERTY_PATH.exists():
        print(f"missing real property dataset: {REAL_PROPERTY_PATH}")
        return 1

    parcel = json.loads(PARCEL_PATH.read_text())
    rp = json.loads(REAL_PROPERTY_PATH.read_text())

    value_by_blocklot: dict[str, tuple[float, str]] = {}
    for feat in rp.get("features", []):
        props = feat.get("properties", {})
        blocklot = normalize_join_key(props.get("BLOCKLOT"))
        if not blocklot:
            continue
        value, source = select_value(props)
        if value <= 0.0:
            continue
        prev = value_by_blocklot.get(blocklot)
        if prev is None or value > prev[0]:
            value_by_blocklot[blocklot] = (value, source)

    out_features = []
    matched = 0
    for feat in parcel.get("features", []):
        props = dict(feat.get("properties", {}))
        blocklot = normalize_join_key(props.get("BLOCKLOT"))
        value, source = value_by_blocklot.get(blocklot, (0.0, ""))
        if value > 0.0:
            matched += 1
        props["value_usd"] = value
        props["value_source"] = source
        out_features.append(
            {
                "type": "Feature",
                "geometry": feat.get("geometry"),
                "properties": props,
            }
        )

    out = {"type": "FeatureCollection", "features": out_features}
    OUT_PATH.write_text(json.dumps(out))
    print(f"wrote: {OUT_PATH}")
    print(f"parcel features: {len(out_features)}")
    print(f"matched value records: {matched}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
