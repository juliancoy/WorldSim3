#!/usr/bin/env python3
import json
import math
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "sources/world/earth/nation_state/us/state_region/md/county_city/baltimore_city/layers/crime_nibrs_group_a_2022_present.geojson"
OUT_DIR = ROOT / "data/test_outputs"
OUT_JPG = OUT_DIR / "nibrs_crime_aggregate_3x3.jpg"

TILE = 360
PAD = 18
LABEL_H = 34
INNER = 304
BG = (14, 19, 24)
PANEL = (22, 30, 38)
TEXT = (238, 243, 247)


AGGREGATES = [
    ("Grid Binning", "grid"),
    ("KDE Gaussian", "kde"),
    ("GPU Splat + Blur", "gpu_blur"),
    ("GPU Splat Hue", "gpu_hue"),
    ("Hex Binning", "hex"),
    ("Multi-res Pyramid", "multires"),
    ("LOD Geometry", "lod"),
    ("Median Choropleth", "median"),
    ("Point Clustering", "cluster"),
]


def load_points():
    with SOURCE.open() as f:
        data = json.load(f)
    pts = []
    rejected = 0
    for feature in data.get("features", []):
        geom = feature.get("geometry") or {}
        coords = geom.get("coordinates")
        if not isinstance(coords, list) or len(coords) < 2:
            rejected += 1
            continue
        lon, lat = float(coords[0]), float(coords[1])
        if not math.isfinite(lon) or not math.isfinite(lat):
            rejected += 1
            continue
        if not (-180 <= lon <= 180 and -90 <= lat <= 90):
            rejected += 1
            continue
        if abs(lon) < 1.0e-5 and abs(lat) < 1.0e-5:
            rejected += 1
            continue
        pts.append((lon, lat))
    if not pts:
        raise RuntimeError("no valid NIBRS points")
    arr = np.asarray(pts, dtype=np.float64)
    return arr, rejected


def heat_colors(norm, hue=False):
    norm = np.clip(norm, 0.0, 1.0)
    if hue:
        r = (50 + 205 * norm).astype(np.uint8)
        g = (80 + 90 * np.sqrt(norm)).astype(np.uint8)
        b = (210 - 180 * norm).astype(np.uint8)
    else:
        r = np.where(norm < 0.5, 30 + 430 * norm, 245 - 50 * (norm - 0.5)).astype(np.uint8)
        g = np.where(norm < 0.5, 86 + 300 * norm, 236 - 350 * (norm - 0.5)).astype(np.uint8)
        b = np.where(norm < 0.5, 190 - 220 * norm, 80 - 90 * (norm - 0.5)).astype(np.uint8)
    a = np.where(norm > 0, 38 + 198 * np.sqrt(norm), 0).astype(np.uint8)
    return np.dstack([r, g, b, a])


def histogram(points, bounds, size, bins):
    min_lon, min_lat, max_lon, max_lat = bounds
    x = np.clip(((points[:, 0] - min_lon) / (max_lon - min_lon) * bins).astype(np.int32), 0, bins - 1)
    y = np.clip(((max_lat - points[:, 1]) / (max_lat - min_lat) * bins).astype(np.int32), 0, bins - 1)
    h = np.zeros((bins, bins), dtype=np.float32)
    np.add.at(h, (y, x), 1.0)
    img = Image.fromarray(np.clip(h / max(1.0, h.max()) * 255, 0, 255).astype(np.uint8), "L")
    return img.resize((size, size), Image.Resampling.BILINEAR), h


def colorize_density(density_img, blur=0.0, hue=False):
    if blur > 0:
        density_img = density_img.filter(ImageFilter.GaussianBlur(blur))
    arr = np.asarray(density_img, dtype=np.float32) / 255.0
    if arr.max() > 0:
        arr = arr / max(arr.max(), 1e-6)
    rgba = heat_colors(arr, hue=hue)
    return Image.fromarray(rgba, "RGBA")


def make_panel(points, bounds, label, kind):
    panel = Image.new("RGB", (TILE, TILE), PANEL)
    draw = ImageDraw.Draw(panel)
    x0 = (TILE - INNER) // 2
    y0 = LABEL_H + 8
    draw.rectangle((x0 - 1, y0 - 1, x0 + INNER, y0 + INNER), fill=(8, 12, 16), outline=(66, 83, 96))

    if kind in {"grid", "median"}:
        density, h = histogram(points, bounds, INNER, 42 if kind == "grid" else 24)
        if kind == "median":
            arr = np.asarray(density, dtype=np.uint8)
            arr = (arr // 42) * 42
            density = Image.fromarray(arr, "L")
        layer = colorize_density(density, 0.0, False)
    elif kind == "kde":
        density, _ = histogram(points, bounds, INNER, 180)
        layer = colorize_density(density, 4.5, False)
    elif kind == "gpu_blur":
        density, _ = histogram(points, bounds, INNER, 220)
        layer = colorize_density(density, 7.0, False)
    elif kind == "gpu_hue":
        density, _ = histogram(points, bounds, INNER, 220)
        layer = colorize_density(density, 7.0, True)
    elif kind == "multires":
        fine, _ = histogram(points, bounds, INNER, 180)
        coarse, _ = histogram(points, bounds, INNER, 42)
        fine_arr = np.asarray(fine.filter(ImageFilter.GaussianBlur(3.0)), dtype=np.float32)
        coarse_arr = np.asarray(coarse.filter(ImageFilter.GaussianBlur(10.0)), dtype=np.float32)
        mix = np.clip(fine_arr * 0.65 + coarse_arr * 0.35, 0, 255).astype(np.uint8)
        layer = colorize_density(Image.fromarray(mix, "L"), 0.0, False)
    elif kind == "hex":
        layer = Image.new("RGBA", (INNER, INNER), (0, 0, 0, 0))
        d = ImageDraw.Draw(layer)
        density, h = histogram(points, bounds, INNER, 30)
        hmax = max(1.0, float(h.max()))
        step = INNER / h.shape[0]
        for yy in range(h.shape[0]):
            for xx in range(h.shape[1]):
                v = h[yy, xx]
                if v <= 0:
                    continue
                t = min(1.0, float(v / hmax))
                cx = (xx + 0.5 + 0.5 * (yy % 2)) * step
                cy = (yy + 0.5) * step
                r = step * 0.46
                pts = [(cx + math.cos(a) * r, cy + math.sin(a) * r) for a in [0, 1.047, 2.094, 3.142, 4.189, 5.236]]
                color = tuple(heat_colors(np.array([[t]], dtype=np.float32))[0, 0])
                d.polygon(pts, fill=color)
    elif kind == "cluster":
        density, h = histogram(points, bounds, INNER, 22)
        layer = Image.new("RGBA", (INNER, INNER), (0, 0, 0, 0))
        d = ImageDraw.Draw(layer)
        hmax = max(1.0, float(h.max()))
        step = INNER / h.shape[0]
        for yy in range(h.shape[0]):
            for xx in range(h.shape[1]):
                v = h[yy, xx]
                if v <= 0:
                    continue
                t = min(1.0, float(v / hmax))
                r = 4 + 14 * math.sqrt(t)
                cx = (xx + 0.5) * step
                cy = (yy + 0.5) * step
                color = tuple(heat_colors(np.array([[t]], dtype=np.float32), hue=True)[0, 0])
                d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=color, outline=(255, 255, 255, 120))
    else:
        layer = Image.new("RGBA", (INNER, INNER), (0, 0, 0, 0))
        d = ImageDraw.Draw(layer)
        stride = max(1, len(points) // 22000)
        min_lon, min_lat, max_lon, max_lat = bounds
        for lon, lat in points[::stride]:
            x = (lon - min_lon) / (max_lon - min_lon) * INNER
            y = (max_lat - lat) / (max_lat - min_lat) * INNER
            d.point((x, y), fill=(230, 75, 55, 150))

    panel.paste(layer.convert("RGB"), (x0, y0), layer if layer.mode == "RGBA" else None)
    try:
        font = ImageFont.truetype("DejaVuSans-Bold.ttf", 16)
        small = ImageFont.truetype("DejaVuSans.ttf", 11)
    except Exception:
        font = small = None
    draw.text((14, 10), label, fill=TEXT, font=font)
    draw.text((14, TILE - 22), "NIBRS Group A Crime Data", fill=(166, 181, 191), font=small)
    return panel


def main():
    points, rejected = load_points()
    min_lon, min_lat = points.min(axis=0)
    max_lon, max_lat = points.max(axis=0)
    pad_lon = (max_lon - min_lon) * 0.05
    pad_lat = (max_lat - min_lat) * 0.05
    bounds = (min_lon - pad_lon, min_lat - pad_lat, max_lon + pad_lon, max_lat + pad_lat)

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    sheet = Image.new("RGB", (TILE * 3, TILE * 3), BG)
    for i, (label, kind) in enumerate(AGGREGATES):
        panel = make_panel(points, bounds, label, kind)
        sheet.paste(panel, ((i % 3) * TILE, (i // 3) * TILE))
    sheet.save(OUT_JPG, "JPEG", quality=94, optimize=True)
    print(json.dumps({
        "ok": True,
        "output": str(OUT_JPG),
        "points": int(len(points)),
        "rejected_null_island_or_invalid": rejected,
        "bounds": {
            "min_lon": float(min_lon),
            "min_lat": float(min_lat),
            "max_lon": float(max_lon),
            "max_lat": float(max_lat),
        },
        "aggregates": [name for name, _ in AGGREGATES],
    }, indent=2))


if __name__ == "__main__":
    main()
