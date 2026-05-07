#!/usr/bin/env python3
import argparse
import math
import pathlib
import random
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed

ROOT = pathlib.Path(__file__).resolve().parents[1]s[1]
OUT = ROOT / "data" / "tiles"

MIN_LON, MIN_LAT = -76.72, 39.20
MAX_LON, MAX_LAT = -76.50, 39.38
DEFAULT_MIN_ZOOM = 11
DEFAULT_MAX_ZOOM = 18
DEFAULT_WORKERS = 2
DEFAULT_TIMEOUT = 30.0
DEFAULT_RETRIES = 4
DEFAULT_PROGRESS_EVERY = 250
DEFAULT_BACKOFF_BASE = 0.75


def deg2num(lat_deg, lon_deg, zoom):
    lat_rad = math.radians(lat_deg)
    n = 2.0 ** zoom
    xtile = int((lon_deg + 180.0) / 360.0 * n)
    ytile = int((1.0 - math.asinh(math.tan(lat_rad)) / math.pi) / 2.0 * n)
    return xtile, ytile


def fetch(url, dst, timeout):
    req = urllib.request.Request(url, headers={"User-Agent": "BaltimoreVulkanMap/1.0"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        dst.write_bytes(r.read())


def fetch_with_retry(url, dst, timeout, retries, backoff_base):
    last_exc = None
    for attempt in range(1, retries + 1):
        try:
            fetch(url, dst, timeout)
            return True, None
        except Exception as e:
            last_exc = e
            if attempt == retries:
                break
            delay = backoff_base * (2 ** (attempt - 1))
            delay *= (0.85 + random.random() * 0.30)
            time.sleep(delay)
    return False, last_exc


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--min-zoom", type=int, default=DEFAULT_MIN_ZOOM)
    parser.add_argument("--max-zoom", type=int, default=DEFAULT_MAX_ZOOM)
    parser.add_argument("--workers", type=int, default=DEFAULT_WORKERS)
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    parser.add_argument("--retries", type=int, default=DEFAULT_RETRIES)
    parser.add_argument("--progress-every", type=int, default=DEFAULT_PROGRESS_EVERY)
    parser.add_argument("--backoff-base", type=float, default=DEFAULT_BACKOFF_BASE)
    args = parser.parse_args()

    if args.workers < 1:
        raise ValueError("--workers must be >= 1")
    if args.retries < 1:
        raise ValueError("--retries must be >= 1")
    if args.progress_every < 1:
        raise ValueError("--progress-every must be >= 1")

    zooms = list(range(args.min_zoom, args.max_zoom + 1))

    OUT.mkdir(parents=True, exist_ok=True)

    zoom_ranges = {}
    total_tiles = 0
    for z in zooms:
        x0, y1 = deg2num(MIN_LAT, MIN_LON, z)
        x1, y0 = deg2num(MAX_LAT, MAX_LON, z)
        x_min, x_max = min(x0, x1), max(x0, x1)
        y_min, y_max = min(y0, y1), max(y0, y1)
        zoom_total = (x_max - x_min + 1) * (y_max - y_min + 1)
        zoom_ranges[z] = (x_min, x_max, y_min, y_max, zoom_total)
        total_tiles += zoom_total

    print(f"total tiles in range: {total_tiles}")
    print(
        f"settings: workers={args.workers} timeout={args.timeout}s "
        f"retries={args.retries} progress_every={args.progress_every}"
    )

    done = 0
    downloaded = 0
    skipped = 0
    failed = 0
    started_at = time.time()

    for z in zooms:
        x_min, x_max, y_min, y_max, zoom_total = zoom_ranges[z]
        zoom_done = 0
        zoom_downloaded = 0
        zoom_failed = 0
        zoom_skipped = 0
        to_download = []

        print(f"zoom {z}: {zoom_total} tiles")
        for x in range(x_min, x_max + 1):
            d = OUT / str(z) / str(x)
            d.mkdir(parents=True, exist_ok=True)
            for y in range(y_min, y_max + 1):
                f = d / f"{y}.png"
                if f.exists():
                    done += 1
                    zoom_done += 1
                    skipped += 1
                    zoom_skipped += 1
                    continue
                url = f"https://tile.openstreetmap.org/{z}/{x}/{y}.png"
                to_download.append((x, y, url, f))

        if not to_download:
            print(f"zoom {z}: no missing tiles")
        else:
            with ThreadPoolExecutor(max_workers=args.workers) as pool:
                futures = {
                    pool.submit(
                        fetch_with_retry,
                        url,
                        f,
                        args.timeout,
                        args.retries,
                        args.backoff_base,
                    ): (x, y)
                    for (x, y, url, f) in to_download
                }

                for fut in as_completed(futures):
                    x, y = futures[fut]
                    ok, err = fut.result()
                    done += 1
                    zoom_done += 1

                    if ok:
                        downloaded += 1
                        zoom_downloaded += 1
                    else:
                        failed += 1
                        zoom_failed += 1
                        pct = (done / total_tiles) * 100.0 if total_tiles else 100.0
                        print(
                            f"[{done}/{total_tiles} {pct:6.2f}%] "
                            f"z{z} failed {x}/{y}: {err}"
                        )

                    if done % args.progress_every == 0 or done == total_tiles:
                        pct = (done / total_tiles) * 100.0 if total_tiles else 100.0
                        elapsed = max(time.time() - started_at, 0.001)
                        rate = done / elapsed
                        remaining = max(total_tiles - done, 0)
                        eta_s = int(remaining / rate) if rate > 0 else -1
                        print(
                            f"[{done}/{total_tiles} {pct:6.2f}%] "
                            f"downloaded={downloaded} skipped={skipped} failed={failed} "
                            f"rate={rate:.1f} tiles/s eta={eta_s}s"
                        )

        zoom_pct = (zoom_done / zoom_total) * 100.0 if zoom_total else 100.0
        total_pct = (done / total_tiles) * 100.0 if total_tiles else 100.0
        print(
            f"zoom {z} complete: {zoom_done}/{zoom_total} ({zoom_pct:.2f}%) "
            f"downloaded={zoom_downloaded} skipped={zoom_skipped} failed={zoom_failed} "
            f"overall={done}/{total_tiles} ({total_pct:.2f}%)"
        )

    elapsed = max(time.time() - started_at, 0.001)
    print(
        f"summary: downloaded={downloaded} skipped={skipped} failed={failed} "
        f"elapsed={elapsed:.1f}s avg_rate={done / elapsed:.1f} tiles/s"
    )


if __name__ == "__main__":
    main()
