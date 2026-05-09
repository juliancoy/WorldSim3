#!/usr/bin/env python3
"""Build CoC-level homelessness trend layer by joining HUD PIT table to CoC geometry.

Inputs:
- CoC geometry GeoJSON (default: data/layers/hud_coc_boundaries_maryland.geojson)
- HUD PIT workbook/csv
  - explicit: --pit-table /path/to/file
  - drop-zone default: data/inbox/hud_pit/

Output:
- data/layers/hud_pit_homelessness_2007_2024_by_coc.geojson
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Iterable

import pandas as pd


def normalize_coc(v: object) -> str:
    s = str(v or "").strip().upper().replace(" ", "")
    if not s:
        return ""
    m = re.match(r"^([A-Z]{2})[-_]?([0-9]{3})$", s)
    if m:
        return f"{m.group(1)}-{m.group(2)}"
    return s


def load_tabular(path: Path, sheet: str | None) -> pd.DataFrame:
    suffix = path.suffix.lower()
    if suffix == ".csv":
        return pd.read_csv(path)
    if suffix == ".xlsx":
        return pd.read_excel(path, sheet_name=sheet)
    if suffix == ".xlsb":
        try:
            return pd.read_excel(path, engine="pyxlsb", sheet_name=sheet)
        except Exception as e:  # noqa: BLE001
            raise RuntimeError(
                f"Failed reading xlsb: {e}. Install dependency with: python3 -m pip install pyxlsb"
            ) from e
    raise RuntimeError(f"Unsupported input format: {path}")


def find_col(df: pd.DataFrame, candidates: Iterable[str]) -> str | None:
    lookup = {c.lower().strip(): c for c in df.columns}
    for want in candidates:
        if want.lower() in lookup:
            return lookup[want.lower()]
    for c in df.columns:
        cl = c.lower().strip()
        for want in candidates:
            if want.lower() in cl:
                return c
    return None


def extract_year_columns(df: pd.DataFrame) -> list[tuple[int, str]]:
    out: list[tuple[int, str]] = []
    for c in df.columns:
        s = str(c).strip()
        m = re.search(r"(20[0-9]{2})", s)
        if m:
            y = int(m.group(1))
            if 2000 <= y <= 2035:
                out.append((y, c))
    # keep unique by year, first match wins
    seen: set[int] = set()
    uniq: list[tuple[int, str]] = []
    for y, c in sorted(out, key=lambda t: t[0]):
        if y in seen:
            continue
        seen.add(y)
        uniq.append((y, c))
    return uniq


def parse_pit(df: pd.DataFrame) -> pd.DataFrame:
    coc_col = find_col(df, ["COCNUM", "CoC Number", "CoC", "HudNum"])
    if not coc_col:
        raise RuntimeError(f"Could not find CoC identifier column. Columns: {list(df.columns)}")

    year_cols = extract_year_columns(df)
    if len(year_cols) < 3:
        raise RuntimeError(f"Could not identify PIT year columns. Columns: {list(df.columns)}")

    keep_cols = [coc_col] + [c for _, c in year_cols]
    d = df[keep_cols].copy()
    d = d.rename(columns={coc_col: "COCNUM"})
    d["COCNUM"] = d["COCNUM"].map(normalize_coc)
    d = d[d["COCNUM"] != ""]

    # Wide table expected. Aggregate duplicate rows by CoC and sum numeric year columns.
    for _, c in year_cols:
        d[c] = pd.to_numeric(d[c], errors="coerce").fillna(0)
    agg_map = {c: "sum" for _, c in year_cols}
    d = d.groupby("COCNUM", as_index=False).agg(agg_map)

    # Rename to PIT_YYYY_Total fields expected by map layer.
    rename = {col: f"PIT_{year}_Total" for year, col in year_cols}
    d = d.rename(columns=rename)

    years = sorted(y for y, _ in year_cols)
    if years:
        d["PIT_First_Year"] = years[0]
        d["PIT_Last_Year"] = years[-1]
        d["PIT_Change_Abs"] = d.get(f"PIT_{years[-1]}_Total", 0) - d.get(f"PIT_{years[0]}_Total", 0)
        base = d.get(f"PIT_{years[0]}_Total", 0).replace(0, pd.NA)
        d["PIT_Change_Pct"] = ((d.get(f"PIT_{years[-1]}_Total", 0) - d.get(f"PIT_{years[0]}_Total", 0)) / base) * 100.0
    return d


def resolve_pit_path(explicit: str | None) -> Path:
    if explicit:
        return Path(explicit)
    inbox = Path("data/inbox/hud_pit")
    candidates = []
    for ext in (".xlsb", ".xlsx", ".csv"):
        candidates.extend(inbox.glob(f"*{ext}"))
    if not candidates:
        raise SystemExit(
            "No PIT file found. Drag and drop one file into data/inbox/hud_pit/ "
            "or pass --pit-table."
        )
    # Prefer newest file for drag-and-drop workflow.
    candidates.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    return candidates[0]


def main() -> int:
    ap = argparse.ArgumentParser(description="Build HUD PIT CoC homelessness trend GeoJSON layer")
    ap.add_argument("--coc-geojson", default="data/layers/hud_coc_boundaries_maryland.geojson")
    ap.add_argument("--pit-table", default=None, help="Local path to HUD PIT table (.xlsb/.xlsx/.csv)")
    ap.add_argument("--sheet", default=None, help="Optional sheet name for Excel input")
    ap.add_argument("--out", default="data/layers/hud_pit_homelessness_2007_2024_by_coc.geojson")
    args = ap.parse_args()

    coc_path = Path(args.coc_geojson)
    pit_path = resolve_pit_path(args.pit_table)
    out_path = Path(args.out)

    if not coc_path.exists():
        raise SystemExit(f"Missing CoC geometry: {coc_path}")
    if not pit_path.exists():
        raise SystemExit(f"Missing PIT source table: {pit_path}")

    df = load_tabular(pit_path, args.sheet)
    pit = parse_pit(df)

    fc = json.loads(coc_path.read_text(encoding="utf-8"))
    features = fc.get("features", [])
    matched = 0
    for f in features:
        p = f.get("properties", {})
        coc = normalize_coc(p.get("COCNUM") or p.get("HudNum"))
        hit = pit[pit["COCNUM"] == coc]
        if hit.empty:
            continue
        row = hit.iloc[0].to_dict()
        for k, v in row.items():
            if k == "COCNUM":
                continue
            if pd.isna(v):
                p[k] = None
            elif isinstance(v, (int, float)):
                p[k] = float(v) if isinstance(v, float) else int(v)
            else:
                p[k] = v
        matched += 1

    fc["name"] = "HUD PIT Homelessness Trends by CoC (2007-2024)"
    fc["source"] = str(pit_path)
    fc["join_key"] = "COCNUM"
    fc["matched_features"] = matched
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(fc, separators=(",", ":")), encoding="utf-8")

    print(f"rows in PIT table: {len(pit)}")
    print(f"matched CoC features: {matched} / {len(features)}")
    print(f"source PIT file: {pit_path}")
    print(f"wrote: {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
