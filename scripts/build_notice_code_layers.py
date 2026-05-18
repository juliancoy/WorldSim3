#!/usr/bin/env python3
"""Build derived Baltimore open-notice layers split by Notice_Type."""

from __future__ import annotations

import json
import math
import sys
import time
import urllib.parse
import urllib.request
from pathlib import Path

SOURCE_URL = "https://services1.arcgis.com/UWYHeuuJISiGmgXx/ArcGIS/rest/services/OpenNoticesDashboard/FeatureServer/0"
OUT_DIR = Path("data/world/earth/nation_state/us/state_region/md/county_city/baltimore_city/layers")
PAGE_SIZE = 2000

NOTICE_LAYERS = {
    "Exterior": {
        "file": "open_notices_exterior.geojson",
        "name": "Open Notices - Exterior Code",
    },
    "Interior": {
        "file": "open_notices_interior.geojson",
        "name": "Open Notices - Interior Code",
    },
    "Interior/Exterior": {
        "file": "open_notices_interior_exterior.geojson",
        "name": "Open Notices - Interior/Exterior Code",
    },
    "Other": {
        "file": "open_notices_other.geojson",
        "name": "Open Notices - Other Code",
    },
    "Vacant": {
        "file": "open_notices_vacant.geojson",
        "name": "Open Notices - Vacant Code",
    },
}


def fetch_json(url: str, params: dict[str, object]) -> dict:
    query = urllib.parse.urlencode(params)
    with urllib.request.urlopen(f"{url}?{query}", timeout=60) as resp:
        return json.load(resp)


def fetch_all_features() -> list[dict]:
    features: list[dict] = []
    offset = 0
    while True:
        data = fetch_json(
            f"{SOURCE_URL}/query",
            {
                "where": "1=1",
                "outFields": "*",
                "returnGeometry": "true",
                "outSR": 4326,
                "f": "geojson",
                "resultOffset": offset,
                "resultRecordCount": PAGE_SIZE,
            },
        )
        page = data.get("features", [])
        if not page:
            break
        features.extend(page)
        if len(page) < PAGE_SIZE:
            break
        offset += len(page)
        time.sleep(0.05)
    return features


def normalize_feature(feature: dict) -> dict:
    props = dict(feature.get("properties") or {})
    # Normalize parcel key casing to match the rest of the app's parcel-linked layers.
    if "Blocklot" in props and "BLOCKLOT" not in props:
        props["BLOCKLOT"] = props.get("Blocklot")
    if "Issue_Date" in props and props.get("Issue_Date") is not None:
        v = props["Issue_Date"]
        if isinstance(v, (int, float)) and math.isfinite(v):
            # ArcGIS GeoJSON usually emits epoch milliseconds for date fields.
            import datetime as _dt
            props["Issue_Date_ISO"] = _dt.datetime.fromtimestamp(v / 1000.0, tz=_dt.timezone.utc).isoformat().replace("+00:00", "Z")
    return {
        "type": "Feature",
        "properties": props,
        "geometry": feature.get("geometry"),
    }


def main() -> int:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    features = [normalize_feature(f) for f in fetch_all_features()]
    by_type: dict[str, list[dict]] = {key: [] for key in NOTICE_LAYERS}
    unknown: dict[str, int] = {}
    for feature in features:
        notice_type = (feature.get("properties") or {}).get("Notice_Type")
        if notice_type in by_type:
            by_type[notice_type].append(feature)
        else:
            unknown[str(notice_type)] = unknown.get(str(notice_type), 0) + 1

    for notice_type, spec in NOTICE_LAYERS.items():
        collection = {
            "type": "FeatureCollection",
            "name": spec["name"],
            "source": SOURCE_URL,
            "notice_type": notice_type,
            "features": by_type[notice_type],
        }
        path = OUT_DIR / spec["file"]
        path.write_text(json.dumps(collection, separators=(",", ":")), encoding="utf-8")
        print(f"{path}: {len(by_type[notice_type])} features")

    if unknown:
        print(f"Unknown Notice_Type values: {unknown}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
