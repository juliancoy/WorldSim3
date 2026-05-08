#!/usr/bin/env python3
"""Build derived CodeMap rehab-success proxy layers from DHCD Datamax services."""

from __future__ import annotations

import datetime as dt
import json
import math
import sys
import time
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any

OUT_DIR = Path("data/layers")
PAGE_SIZE = 2000

LAYERS = [
    {
        "service": "https://egis.baltimorecity.gov/egis/rest/services/Housing/dmxLandPlanning/MapServer/38",
        "file": "codemap_recently_rehabbed_vacant_buildings.geojson",
        "name": "CodeMap Recently Rehabbed Vacant Buildings",
        "kind": "point",
    },
    {
        "service": "https://egis.baltimorecity.gov/egis/rest/services/Housing/dmxLandPlanning/MapServer/27",
        "file": "codemap_recently_rehabbed_vbn_parcels.geojson",
        "name": "CodeMap Recently Rehabbed VBN Parcels",
        "kind": "parcel_polygon",
    },
]


def fetch_json(url: str, params: dict[str, Any]) -> dict[str, Any]:
    query = urllib.parse.urlencode(params)
    with urllib.request.urlopen(f"{url}?{query}", timeout=90) as resp:
        return json.load(resp)


def iso_date_from_arcgis(value: Any) -> str | None:
    if value is None:
        return None
    if isinstance(value, (int, float)) and math.isfinite(value):
        return dt.datetime.fromtimestamp(value / 1000.0, tz=dt.timezone.utc).date().isoformat()
    return None


def normalize_props(props: dict[str, Any]) -> dict[str, Any]:
    out = dict(props)
    blocklot = out.get("BlockLot") or out.get("BLOCKLOT") or out.get("blocklot")
    if blocklot is not None:
        out["BLOCKLOT"] = str(blocklot).strip()
    fulladdr = out.get("FULLADDR") or out.get("Address") or out.get("address")
    if fulladdr is not None:
        out["Address"] = str(fulladdr).strip()
    neighbor = out.get("NEIGHBOR") or out.get("Neighborhood") or out.get("neighborhood")
    if neighbor is not None:
        out["Neighborhood"] = str(neighbor).strip()
    date_issue_iso = iso_date_from_arcgis(out.get("DateIssue")) or iso_date_from_arcgis(out.get("csm_issued_date"))
    if date_issue_iso:
        out["DateIssue_ISO"] = date_issue_iso
    out["rehab_success_proxy"] = "recently_rehabbed_vacant_building_since_2022"
    return out


def fetch_layer(spec: dict[str, str]) -> list[dict[str, Any]]:
    service = spec["service"]
    features: list[dict[str, Any]] = []
    offset = 0
    while True:
        data = fetch_json(
            f"{service}/query",
            {
                "where": "1=1",
                "outFields": "*",
                "returnGeometry": "true",
                "outSR": 4326,
                "f": "geojson",
                "resultOffset": offset,
                "resultRecordCount": PAGE_SIZE,
                "orderByFields": "OBJECTID ASC",
            },
        )
        page = data.get("features") or []
        if not page:
            break
        for feature in page:
            props = normalize_props(feature.get("properties") or {})
            features.append({"type": "Feature", "properties": props, "geometry": feature.get("geometry")})
        if len(page) < PAGE_SIZE:
            break
        offset += len(page)
        time.sleep(0.05)
    return features


def main() -> int:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for spec in LAYERS:
        features = fetch_layer(spec)
        collection = {
            "type": "FeatureCollection",
            "name": spec["name"],
            "source": spec["service"],
            "source_kind": spec["kind"],
            "features": features,
        }
        path = OUT_DIR / spec["file"]
        path.write_text(json.dumps(collection, separators=(",", ":")), encoding="utf-8")
        missing_blocklot = sum(1 for f in features if not (f.get("properties") or {}).get("BLOCKLOT"))
        print(f"{path}: {len(features)} features, missing BLOCKLOT={missing_blocklot}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
