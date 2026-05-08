#!/usr/bin/env python3
"""Join point/event layers to parcel polygons by BLOCKLOT and emit parcel layers."""

from __future__ import annotations

import json
from collections import defaultdict
from pathlib import Path
from typing import Any

DATA_DIR = Path("data/layers")
PARCEL_FILE = DATA_DIR / "parcel.geojson"

SPECS = [
    {
        "source": "open_notices_vacant.geojson",
        "output": "open_notices_vacant_parcels.geojson",
        "name": "Open Notices - Vacant Code Parcels",
        "event_kind": "open_notice_vacant",
        "date_fields": ["Issue_Date_ISO", "Issue_Year"],
        "id_fields": ["ObjectID"],
        "summary_fields": ["Notice_Type", "Council_District", "Councilperson", "Impact_Investment_Area", "Neighborhood"],
    },
    {
        "source": "open_notices_exterior.geojson",
        "output": "open_notices_exterior_parcels.geojson",
        "name": "Open Notices - Exterior Code Parcels",
        "event_kind": "open_notice_exterior",
        "date_fields": ["Issue_Date_ISO", "Issue_Year"],
        "id_fields": ["ObjectID"],
        "summary_fields": ["Notice_Type", "Council_District", "Councilperson", "Impact_Investment_Area", "Neighborhood"],
    },
    {
        "source": "open_notices_interior.geojson",
        "output": "open_notices_interior_parcels.geojson",
        "name": "Open Notices - Interior Code Parcels",
        "event_kind": "open_notice_interior",
        "date_fields": ["Issue_Date_ISO", "Issue_Year"],
        "id_fields": ["ObjectID"],
        "summary_fields": ["Notice_Type", "Council_District", "Councilperson", "Impact_Investment_Area", "Neighborhood"],
    },
    {
        "source": "open_notices_interior_exterior.geojson",
        "output": "open_notices_interior_exterior_parcels.geojson",
        "name": "Open Notices - Interior/Exterior Code Parcels",
        "event_kind": "open_notice_interior_exterior",
        "date_fields": ["Issue_Date_ISO", "Issue_Year"],
        "id_fields": ["ObjectID"],
        "summary_fields": ["Notice_Type", "Council_District", "Councilperson", "Impact_Investment_Area", "Neighborhood"],
    },
    {
        "source": "open_notices_other.geojson",
        "output": "open_notices_other_parcels.geojson",
        "name": "Open Notices - Other Code Parcels",
        "event_kind": "open_notice_other",
        "date_fields": ["Issue_Date_ISO", "Issue_Year"],
        "id_fields": ["ObjectID"],
        "summary_fields": ["Notice_Type", "Council_District", "Councilperson", "Impact_Investment_Area", "Neighborhood"],
    },
    {
        "source": "codemap_recently_rehabbed_vacant_buildings.geojson",
        "output": "codemap_recently_rehabbed_vacant_building_parcels.geojson",
        "name": "CodeMap Recently Rehabbed Vacant Building Parcels",
        "event_kind": "codemap_recent_rehab",
        "date_fields": ["DateIssue_ISO"],
        "id_fields": ["OBJECTID", "PermitNum"],
        "summary_fields": ["PermitNum", "VBN", "csm_exist_use", "csm_prop_use", "Council_District", "Neighborhood", "Typology2017"],
    },
]


def norm_key(value: Any) -> str:
    if value is None:
        return ""
    return "".join(str(value).upper().split())


def first_present(props: dict[str, Any], fields: list[str]) -> Any:
    for field in fields:
        value = props.get(field)
        if value is not None and str(value).strip() != "":
            return value
    return None


def source_blocklot(props: dict[str, Any]) -> str:
    value = first_present(props, ["BLOCKLOT", "BlockLot", "blocklot", "Block_Lot", "BLOCK_LOT"])
    if value is not None:
        return norm_key(value)
    block = first_present(props, ["Block", "BLOCK", "block"])
    lot = first_present(props, ["Lot", "LOT", "lot"])
    if block is not None and lot is not None:
        return norm_key(f"{block}{lot}")
    return ""


def merge_events(parcel_feature: dict[str, Any], events: list[dict[str, Any]], spec: dict[str, Any]) -> dict[str, Any]:
    parcel_props = parcel_feature.get("properties") or {}
    out_props = dict(parcel_props)
    out_props["matched_layer"] = spec["source"]
    out_props["matched_event_kind"] = spec["event_kind"]
    out_props["matched_event_count"] = len(events)

    dates: list[str] = []
    ids: list[str] = []
    for event in events:
        props = event.get("properties") or {}
        date_value = first_present(props, spec["date_fields"])
        if date_value is not None:
            dates.append(str(date_value))
        id_parts = []
        for field in spec["id_fields"]:
            value = props.get(field)
            if value is not None and str(value).strip() != "":
                id_parts.append(str(value).strip())
        if id_parts:
            ids.append(" / ".join(id_parts))

    if dates:
        dates_sorted = sorted(set(dates))
        out_props["matched_first_date"] = dates_sorted[0]
        out_props["matched_latest_date"] = dates_sorted[-1]
    if ids:
        unique_ids = []
        seen = set()
        for value in ids:
            if value in seen:
                continue
            seen.add(value)
            unique_ids.append(value)
        out_props["matched_first_id"] = unique_ids[0]
        out_props["matched_ids_sample"] = "; ".join(unique_ids[:5])

    for field in spec["summary_fields"]:
        values = []
        seen = set()
        for event in events:
            value = (event.get("properties") or {}).get(field)
            if value is None or str(value).strip() == "":
                continue
            text = str(value).strip()
            if text in seen:
                continue
            seen.add(text)
            values.append(text)
        if values:
            safe = field.replace("/", "_").replace(" ", "_")
            out_props[f"matched_{safe}_sample"] = "; ".join(values[:5])

    return {
        "type": "Feature",
        "properties": out_props,
        "geometry": parcel_feature.get("geometry"),
    }


def main() -> int:
    parcels = json.load(open(PARCEL_FILE, encoding="utf-8")).get("features", [])
    parcel_by_key: dict[str, dict[str, Any]] = {}
    for parcel in parcels:
        key = source_blocklot(parcel.get("properties") or {})
        if key and key not in parcel_by_key:
            parcel_by_key[key] = parcel

    for spec in SPECS:
        source_path = DATA_DIR / spec["source"]
        source = json.load(open(source_path, encoding="utf-8"))
        events_by_key: dict[str, list[dict[str, Any]]] = defaultdict(list)
        missing_key = 0
        for event in source.get("features", []):
            key = source_blocklot(event.get("properties") or {})
            if not key:
                missing_key += 1
                continue
            events_by_key[key].append(event)

        out_features = []
        unmatched_events = 0
        for key, events in events_by_key.items():
            parcel = parcel_by_key.get(key)
            if not parcel:
                unmatched_events += len(events)
                continue
            out_features.append(merge_events(parcel, events, spec))

        collection = {
            "type": "FeatureCollection",
            "name": spec["name"],
            "source": spec["source"],
            "join_key": "BLOCKLOT",
            "source_feature_count": len(source.get("features", [])),
            "matched_parcel_count": len(out_features),
            "unmatched_source_event_count": unmatched_events,
            "missing_source_key_count": missing_key,
            "features": out_features,
        }
        out_path = DATA_DIR / spec["output"]
        out_path.write_text(json.dumps(collection, separators=(",", ":")), encoding="utf-8")
        print(
            f"{out_path}: parcels={len(out_features)} source_events={len(source.get('features', []))} "
            f"unmatched_events={unmatched_events} missing_key={missing_key}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
