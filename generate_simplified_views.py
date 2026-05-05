#!/usr/bin/env python3
import argparse
import json
import pathlib
from collections import Counter
from datetime import datetime, timezone
from typing import Any

ROOT = pathlib.Path(__file__).resolve().parent
LAYERS_DIR = ROOT / "data" / "layers"
OUT_DIR = ROOT / "data" / "models"


def summarize_feature_properties(features: list[dict[str, Any]], sample_size: int = 3) -> dict[str, Any]:
    key_counts = Counter()
    type_counts: dict[str, Counter] = {}
    samples = []

    for i, f in enumerate(features):
        props = f.get("properties") or {}
        for k, v in props.items():
            key_counts[k] += 1
            t = type(v).__name__
            type_counts.setdefault(k, Counter())[t] += 1
        if len(samples) < sample_size:
            samples.append(props)

    top_keys = [k for k, _ in key_counts.most_common(25)]
    inferred_types = {
        k: dict(type_counts[k].most_common(3))
        for k in top_keys
    }
    return {
        "property_coverage": {k: c for k, c in key_counts.most_common(25)},
        "property_types": inferred_types,
        "sample_properties": samples,
    }


def geometry_kind(geom: dict[str, Any]) -> str:
    t = geom.get("type", "Unknown")
    return str(t)


def bounds_from_coordinates(coords: Any, bbox: list[float]) -> None:
    if isinstance(coords, (list, tuple)):
        if len(coords) >= 2 and all(isinstance(x, (int, float)) for x in coords[:2]):
            lon = float(coords[0])
            lat = float(coords[1])
            bbox[0] = min(bbox[0], lon)
            bbox[1] = min(bbox[1], lat)
            bbox[2] = max(bbox[2], lon)
            bbox[3] = max(bbox[3], lat)
            return
        for c in coords:
            bounds_from_coordinates(c, bbox)


def summarize_geojson(path: pathlib.Path) -> dict[str, Any]:
    raw = json.loads(path.read_text())
    features = raw.get("features") or []
    geom_counts = Counter()
    bbox = [180.0, 90.0, -180.0, -90.0]

    for f in features:
        geom = f.get("geometry") or {}
        kind = geometry_kind(geom)
        geom_counts[kind] += 1
        bounds_from_coordinates(geom.get("coordinates"), bbox)

    has_valid_bbox = bbox[0] <= bbox[2] and bbox[1] <= bbox[3]
    summary = {
        "dataset": path.name,
        "feature_count": len(features),
        "geometry_counts": dict(geom_counts),
        "bbox": bbox if has_valid_bbox else None,
    }
    summary.update(summarize_feature_properties(features))
    return summary


def write_views(summaries: list[dict[str, Any]], out_dir: pathlib.Path) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    compact = {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "dataset_count": len(summaries),
        "datasets": [
            {
                "dataset": s["dataset"],
                "feature_count": s["feature_count"],
                "geometry_counts": s["geometry_counts"],
                "bbox": s["bbox"],
            }
            for s in summaries
        ],
    }
    detailed = {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "dataset_count": len(summaries),
        "datasets": summaries,
    }
    (out_dir / "simplified_views.compact.json").write_text(json.dumps(compact, indent=2))
    (out_dir / "simplified_views.detailed.json").write_text(json.dumps(detailed, indent=2))


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate simplified model representations for local GeoJSON datasets")
    parser.add_argument("--layers-dir", default=str(LAYERS_DIR), help="GeoJSON directory")
    parser.add_argument("--out-dir", default=str(OUT_DIR), help="Output directory")
    parser.add_argument("--limit", type=int, default=0, help="Optional max dataset count")
    args = parser.parse_args()

    layers_dir = pathlib.Path(args.layers_dir)
    if not layers_dir.exists():
        print(f"layers directory not found: {layers_dir}")
        return 1

    files = sorted([p for p in layers_dir.glob("*.geojson") if p.is_file()])
    if args.limit > 0:
        files = files[: args.limit]

    summaries = []
    for p in files:
        try:
            summaries.append(summarize_geojson(p))
        except Exception as exc:  # noqa: BLE001
            summaries.append({
                "dataset": p.name,
                "error": str(exc),
            })

    out_dir = pathlib.Path(args.out_dir)
    write_views(summaries, out_dir)
    print(f"Processed {len(files)} datasets")
    print(f"Wrote {out_dir / 'simplified_views.compact.json'}")
    print(f"Wrote {out_dir / 'simplified_views.detailed.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
