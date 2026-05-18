#!/usr/bin/env python3
"""Pull Maryland DHCD housing-development layers from official iMAP services."""

from __future__ import annotations

import datetime as dt
import json
import math
import time
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "data" / "provenance" / "stored" / "world" / "earth" / "nation_state" / "us" / "state_region" / "md" / "layers"
PAGE_SIZE = 800

MULTIFAMILY_SERVICE = (
    "https://mdgeodata.md.gov/imap/rest/services/BusinessEconomy/"
    "MD_MultifamilySites/FeatureServer/0"
)
DESIGNATED_AREAS_SERVICE = (
    "https://mdgeodata.md.gov/imap/rest/services/BusinessEconomy/"
    "MD_HousingDesignatedAreas/FeatureServer"
)

LAYERS = [
    {
        "name": "MD DHCD Multifamily Sites",
        "file": "md_dhcd_multifamily_sites.geojson",
        "service": MULTIFAMILY_SERVICE,
        "kind": "state_dhcd_multifamily_sites",
    },
    {
        "name": "MD DHCD Targeted Areas",
        "file": "md_dhcd_targeted_areas.geojson",
        "service": f"{DESIGNATED_AREAS_SERVICE}/0",
        "kind": "state_dhcd_housing_designated_area",
    },
    {
        "name": "MD DHCD Qualified Census Tracts",
        "file": "md_dhcd_qualified_census_tracts.geojson",
        "service": f"{DESIGNATED_AREAS_SERVICE}/1",
        "kind": "state_dhcd_housing_designated_area",
    },
    {
        "name": "MD DHCD Communities of Opportunity",
        "file": "md_dhcd_communities_of_opportunity.geojson",
        "service": f"{DESIGNATED_AREAS_SERVICE}/2",
        "kind": "state_dhcd_housing_designated_area",
    },
    {
        "name": "MD DHCD Small Difficult Development Areas",
        "file": "md_dhcd_small_difficult_development_areas.geojson",
        "service": f"{DESIGNATED_AREAS_SERVICE}/3",
        "kind": "state_dhcd_housing_designated_area",
    },
    {
        "name": "MD DHCD Rural Areas",
        "file": "md_dhcd_rural_areas.geojson",
        "service": f"{DESIGNATED_AREAS_SERVICE}/5",
        "kind": "state_dhcd_housing_designated_area",
    },
    {
        "name": "MD DHCD BRHP Opportunity Designations",
        "file": "md_dhcd_brhp_opportunity_designations.geojson",
        "service": f"{DESIGNATED_AREAS_SERVICE}/6",
        "kind": "state_dhcd_housing_designated_area",
    },
    {
        "name": "MD DHCD Baltimore County Opportunity Areas",
        "file": "md_dhcd_baltimore_county_opportunity_areas.geojson",
        "service": f"{DESIGNATED_AREAS_SERVICE}/7",
        "kind": "state_dhcd_housing_designated_area",
    },
    {
        "name": "MD DHCD Just Communities",
        "file": "md_dhcd_just_communities.geojson",
        "service": f"{DESIGNATED_AREAS_SERVICE}/9",
        "kind": "state_dhcd_housing_designated_area",
    },
]


def fetch_json(url: str, params: dict[str, Any]) -> dict[str, Any]:
    query = urllib.parse.urlencode(params)
    req = urllib.request.Request(f"{url}?{query}", headers={"User-Agent": "worldsim3/1.0"})
    with urllib.request.urlopen(req, timeout=120) as resp:
        return json.load(resp)


def iso_date_from_arcgis(value: Any) -> str | None:
    if isinstance(value, (int, float)) and math.isfinite(value):
        return dt.datetime.fromtimestamp(value / 1000.0, tz=dt.timezone.utc).date().isoformat()
    return None


def normalize_properties(props: dict[str, Any], source_name: str) -> dict[str, Any]:
    out = dict(props)
    out["source_name"] = source_name
    out["source_agency"] = "Maryland Department of Housing and Community Development"
    for key, value in list(out.items()):
        iso = iso_date_from_arcgis(value)
        if iso:
            out[f"{key}_ISO"] = iso
    return out


def fetch_layer(spec: dict[str, str]) -> list[dict[str, Any]]:
    features: list[dict[str, Any]] = []
    metadata = fetch_json(spec["service"], {"f": "json"})
    object_id_field = metadata.get("objectIdField") or "OBJECTID"
    offset = 0
    while True:
        data = fetch_json(
            f"{spec['service']}/query",
            {
                "where": "1=1",
                "outFields": "*",
                "returnGeometry": "true",
                "outSR": 4326,
                "f": "geojson",
                "resultOffset": offset,
                "resultRecordCount": PAGE_SIZE,
                "orderByFields": f"{object_id_field} ASC",
            },
        )
        page = data.get("features") or []
        if not page:
            break
        for feature in page:
            props = normalize_properties(feature.get("properties") or {}, spec["name"])
            features.append(
                {
                    "type": "Feature",
                    "id": feature.get("id"),
                    "properties": props,
                    "geometry": feature.get("geometry"),
                }
            )
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
            "source_agency": "Maryland Department of Housing and Community Development",
            "features": features,
        }
        path = OUT_DIR / spec["file"]
        path.write_text(json.dumps(collection, separators=(",", ":")), encoding="utf-8")
        print(f"{path}: {len(features)} features")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
