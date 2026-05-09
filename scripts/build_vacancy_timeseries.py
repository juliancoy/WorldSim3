#!/usr/bin/env python3
"""Build a historical vacancy time series without open-list survivorship bias.

Outputs a chart-ready yearly CSV with counts, area, and value.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Tuple


DATE_FORMATS = [
    "%Y-%m-%dT%H:%M:%SZ",
    "%Y-%m-%dT%H:%M:%S",
    "%Y-%m-%d",
    "%b %d %Y %H",
    "%b %d %Y",
    "%m/%d/%Y",
    "%m/%d/%y",
]


OPEN_DATE_FIELDS = ["DateNotice", "DateIssue", "DateIssued", "DATE", "RECORD_DATE", "CREATED_DATE"]
NOTICE_CLOSE_FIELDS = ["DateCancel", "DateAbate"]
REHAB_CLOSE_FIELDS = ["DateIssue", "DateIssued", "DATE", "RECORD_DATE", "CREATED_DATE"]

AREA_FIELDS = ["STRUCTAREA", "LOT_SIZE", "Shape__Area"]
VALUE_FIELDS = ["TAXBASE", "ARTAXBAS", "SALEPRIC", "FULLCASH"]


@dataclass
class ParcelHistory:
    open_dates: List[dt.date] = field(default_factory=list)
    close_dates: List[dt.date] = field(default_factory=list)
    area: float = 0.0
    value: float = 0.0


def normalize_blocklot(v: object) -> str:
    if v is None:
        return ""
    s = str(v).strip().upper().replace(" ", "")
    return s


def parse_number(v: object) -> float:
    if v is None:
        return 0.0
    if isinstance(v, (int, float)):
        return float(v)
    s = str(v).strip().replace(",", "")
    if not s:
        return 0.0
    try:
        return float(s)
    except ValueError:
        return 0.0


def parse_date(value: object) -> Optional[dt.date]:
    if value is None:
        return None
    s = str(value).strip()
    if not s or s.lower() == "null":
        return None

    for fmt in DATE_FORMATS:
        try:
            return dt.datetime.strptime(s, fmt).date()
        except ValueError:
            pass

    # Try ISO prefix fallback.
    if len(s) >= 10:
        head = s[:10]
        try:
            return dt.datetime.strptime(head, "%Y-%m-%d").date()
        except ValueError:
            pass

    return None


def read_geojson_features(path: Path) -> List[dict]:
    with path.open("r", encoding="utf-8") as f:
        j = json.load(f)
    return j.get("features", [])


def first_date(props: dict, fields: List[str]) -> Optional[dt.date]:
    for k in fields:
        if k in props:
            d = parse_date(props.get(k))
            if d is not None:
                return d
    return None


def load_notice_events(path: Path, parcels: Dict[str, ParcelHistory], qa: dict) -> None:
    features = read_geojson_features(path)
    qa["notice_rows"] = len(features)
    used = 0
    missing_blocklot = 0
    missing_open_date = 0
    close_events = 0

    for feat in features:
        props = feat.get("properties", {}) or {}
        blocklot = normalize_blocklot(props.get("BLOCKLOT"))
        if not blocklot:
            missing_blocklot += 1
            continue

        open_d = first_date(props, OPEN_DATE_FIELDS)
        if open_d is None:
            missing_open_date += 1
            continue

        ph = parcels.setdefault(blocklot, ParcelHistory())
        ph.open_dates.append(open_d)
        used += 1

        close_d = first_date(props, NOTICE_CLOSE_FIELDS)
        if close_d is not None:
            ph.close_dates.append(close_d)
            close_events += 1

    qa["notice_used_rows"] = used
    qa["notice_missing_blocklot_rows"] = missing_blocklot
    qa["notice_missing_open_date_rows"] = missing_open_date
    qa["notice_close_events"] = close_events


def load_rehab_events(path: Path, parcels: Dict[str, ParcelHistory], qa: dict) -> None:
    features = read_geojson_features(path)
    qa["rehab_rows"] = len(features)
    used = 0
    missing_blocklot = 0
    missing_close_date = 0

    for feat in features:
        props = feat.get("properties", {}) or {}
        blocklot = normalize_blocklot(props.get("BLOCKLOT"))
        if not blocklot:
            missing_blocklot += 1
            continue

        close_d = first_date(props, REHAB_CLOSE_FIELDS)
        if close_d is None:
            missing_close_date += 1
            continue

        ph = parcels.setdefault(blocklot, ParcelHistory())
        ph.close_dates.append(close_d)
        used += 1

    qa["rehab_used_rows"] = used
    qa["rehab_missing_blocklot_rows"] = missing_blocklot
    qa["rehab_missing_close_date_rows"] = missing_close_date


def load_parcel_attributes(path: Path, parcels: Dict[str, ParcelHistory], qa: dict) -> None:
    features = read_geojson_features(path)
    qa["real_property_rows"] = len(features)
    matched = 0

    for feat in features:
        props = feat.get("properties", {}) or {}
        blocklot = normalize_blocklot(props.get("BLOCKLOT"))
        if not blocklot:
            continue
        ph = parcels.get(blocklot)
        if ph is None:
            continue

        area = 0.0
        for k in AREA_FIELDS:
            area = parse_number(props.get(k))
            if area > 0:
                break

        value = 0.0
        for k in VALUE_FIELDS:
            value = parse_number(props.get(k))
            if value > 0:
                break

        ph.area = area
        ph.value = value
        matched += 1

    qa["real_property_matched_rows"] = matched


def timeline_for_parcel(ph: ParcelHistory) -> Tuple[List[Tuple[dt.date, int]], int]:
    events: List[Tuple[dt.date, int]] = []
    for d in ph.open_dates:
        events.append((d, +1))
    for d in ph.close_dates:
        events.append((d, -1))

    # For same day, process opens before closes.
    events.sort(key=lambda x: (x[0], -x[1]))

    state = 0
    corrected = 0
    folded: List[Tuple[dt.date, int]] = []
    for d, delta in events:
        state += delta
        if state < 0:
            corrected += 1
            state = 0
        folded.append((d, delta))
    return folded, corrected


def build_series(parcels: Dict[str, ParcelHistory], qa: dict) -> List[dict]:
    min_year: Optional[int] = None
    max_year: Optional[int] = None
    per_parcel_events: Dict[str, List[Tuple[dt.date, int]]] = {}
    corrected_states = 0

    for blocklot, ph in parcels.items():
        events, corrected = timeline_for_parcel(ph)
        if not events:
            continue
        per_parcel_events[blocklot] = events
        corrected_states += corrected
        years = [d.year for d, _ in events]
        y0, y1 = min(years), max(years)
        min_year = y0 if min_year is None else min(min_year, y0)
        max_year = y1 if max_year is None else max(max_year, y1)

    qa["parcels_with_events"] = len(per_parcel_events)
    qa["corrected_negative_state_events"] = corrected_states

    if min_year is None or max_year is None:
        return []

    rows: List[dict] = []
    active_prev = 0
    for year in range(min_year, max_year + 1):
        y_end = dt.date(year, 12, 31)
        opened_events = 0
        closed_events = 0
        opened_parcels = set()
        closed_parcels = set()

        active_count = 0
        active_area = 0.0
        active_value = 0.0

        for blocklot, evs in per_parcel_events.items():
            state = 0
            for d, delta in evs:
                if d.year == year:
                    if delta > 0:
                        opened_events += 1
                        opened_parcels.add(blocklot)
                    else:
                        closed_events += 1
                        closed_parcels.add(blocklot)
                if d <= y_end:
                    state += delta
                    if state < 0:
                        state = 0

            if state > 0:
                ph = parcels[blocklot]
                active_count += 1
                active_area += ph.area
                active_value += ph.value

        rows.append(
            {
                "year": year,
                "opened_events": opened_events,
                "closed_events": closed_events,
                "net_events": opened_events - closed_events,
                "opened_parcels": len(opened_parcels),
                "closed_parcels": len(closed_parcels),
                "active_parcels_eoy": active_count,
                "active_area_eoy": round(active_area, 2),
                "active_value_eoy": round(active_value, 2),
                "active_delta_vs_prior_year": active_count - active_prev,
            }
        )
        active_prev = active_count

    qa["series_year_min"] = min_year
    qa["series_year_max"] = max_year
    qa["series_rows"] = len(rows)
    return rows


def write_csv(path: Path, rows: List[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = [
        "year",
        "opened_events",
        "closed_events",
        "net_events",
        "opened_parcels",
        "closed_parcels",
        "active_parcels_eoy",
        "active_area_eoy",
        "active_value_eoy",
        "active_delta_vs_prior_year",
    ]
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        w.writerows(rows)


def main() -> None:
    parser = argparse.ArgumentParser(description="Build yearly vacancy time series CSV.")
    parser.add_argument("--notices", default="data/layers/vacant_building_notices.geojson")
    parser.add_argument("--rehabs", default="data/layers/vacant_building_rehabs.geojson")
    parser.add_argument("--real-property", default="data/layers/real_property_information.geojson")
    parser.add_argument("--out-csv", default="data/analytics/vacancy_timeseries_yearly.csv")
    parser.add_argument("--out-qa", default="data/analytics/vacancy_timeseries_qa.json")
    args = parser.parse_args()

    parcels: Dict[str, ParcelHistory] = {}
    qa: dict = {
        "method": "event_based_no_open_list_survivorship_bias",
        "date": dt.datetime.now(dt.timezone.utc).isoformat(),
        "assumptions": {
            "open_event": "vacant notice issue/notice date",
            "close_event": "notice cancel/abate date OR rehab issue date",
            "active_inventory": "parcel has open-close state > 0 at year end",
            "value_note": "uses point-in-time real_property_information value fields, not historical parcel reassessment series",
        },
    }

    load_notice_events(Path(args.notices), parcels, qa)
    load_rehab_events(Path(args.rehabs), parcels, qa)
    load_parcel_attributes(Path(args.real_property), parcels, qa)
    rows = build_series(parcels, qa)

    write_csv(Path(args.out_csv), rows)
    Path(args.out_qa).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out_qa).write_text(json.dumps(qa, indent=2), encoding="utf-8")

    print(f"wrote {len(rows)} rows to {args.out_csv}")
    print(f"wrote qa to {args.out_qa}")


if __name__ == "__main__":
    main()
