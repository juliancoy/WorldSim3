#!/usr/bin/env python3
import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request


DEFAULT_BASE_URL = "http://127.0.0.1:8787"
COUNTY_PARCEL_FILE = "baltimore_county_parcels.geojson"
COUNTY_ZONING_FILE = "baltimore_county_zoning.geojson"
TARGET_LON = -76.62003707885742
TARGET_LAT = 39.437034606933594
TARGET_ZOOM = 17
TARGET_SCREEN_POINTS = (
    (877, 456),
    (860, 456),
    (894, 456),
    (877, 440),
    (877, 472),
    (845, 440),
    (910, 472),
)


def request_json(base_url: str, path: str, timeout: float = 5.0):
    with urllib.request.urlopen(base_url + path, timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8"))


def request_ok(base_url: str, path: str, timeout: float = 5.0):
    with urllib.request.urlopen(base_url + path, timeout=timeout) as response:
        response.read()


def wait_for_status(base_url: str, timeout_s: float):
    deadline = time.time() + timeout_s
    last_error = None
    while time.time() < deadline:
        try:
            return request_json(base_url, "/status", timeout=2.0)
        except Exception as exc:
            last_error = exc
            time.sleep(0.25)
    raise RuntimeError(f"status API not ready after {timeout_s:.1f}s: {last_error}")


def profile_pid(base_url: str):
    try:
        return int(request_json(base_url, "/profile", timeout=2.0).get("process", {}).get("pid", -1))
    except Exception:
        return -1


def wait_for_spawned_status(base_url: str, expected_pid: int, timeout_s: float):
    deadline = time.time() + timeout_s
    last_pid = -1
    last_error = None
    while time.time() < deadline:
        if expected_pid <= 0:
            raise RuntimeError("spawned process exited before REST API became ready")
        if profile_pid(base_url) == expected_pid:
            return request_json(base_url, "/status", timeout=2.0)
        last_pid = profile_pid(base_url)
        last_error = f"REST pid {last_pid}, expected spawned pid {expected_pid}"
        time.sleep(0.25)
    raise RuntimeError(f"spawned REST API not ready after {timeout_s:.1f}s: {last_error}")


def layer_by_file(status: dict, layer_file: str):
    for layer in status.get("layers", []):
        if layer.get("file") == layer_file:
            return layer
    return None


def wait_for_enabled_layers_ready(base_url: str, files, timeout_s: float):
    deadline = time.time() + timeout_s
    last_status = None
    while time.time() < deadline:
        last_status = request_json(base_url, "/status", timeout=5.0)
        ready = True
        for layer_file in files:
            layer = layer_by_file(last_status, layer_file)
            ready = ready and bool(layer) and bool(layer.get("enabled")) and bool(layer.get("ready"))
            ready = ready and bool(layer.get("geometry_gpu_resident")) and bool(layer.get("geometry_gpu_pick_ready"))
        perf = last_status.get("perf", {})
        ready = ready and float(perf.get("fps_avg", 0.0)) > 0.0
        if ready:
            return last_status
        time.sleep(0.5)
    raise RuntimeError("enabled layer readiness timed out: " + json.dumps({
        "enabled_layers_ready": (last_status or {}).get("enabled_layers_ready"),
        "enabled_layers_total": (last_status or {}).get("enabled_layers_total"),
        "layers": [
            {
                "file": layer_file,
                "status": (layer_by_file(last_status or {}, layer_file) or {}).get("status"),
                "ready": (layer_by_file(last_status or {}, layer_file) or {}).get("ready"),
                "gpu_resident": (layer_by_file(last_status or {}, layer_file) or {}).get("geometry_gpu_resident"),
                "pick_ready": (layer_by_file(last_status or {}, layer_file) or {}).get("geometry_gpu_pick_ready"),
            }
            for layer_file in files
        ],
        "perf": (last_status or {}).get("perf"),
    }, indent=2))


def configure_scene(base_url: str):
    status = request_json(base_url, "/status", timeout=5.0)
    for layer in status.get("layers", []):
        if layer.get("enabled"):
            request_ok(base_url, "/set_layer?file=" + urllib.parse.quote(layer.get("file", "")) + "&enabled=0")
    request_ok(base_url, "/set_layer?file=" + urllib.parse.quote(COUNTY_ZONING_FILE) + "&enabled=1")
    request_ok(base_url, "/set_layer?file=" + urllib.parse.quote(COUNTY_PARCEL_FILE) + "&enabled=1")
    request_ok(base_url, f"/set_center?lon={TARGET_LON}&lat={TARGET_LAT}")
    request_ok(base_url, f"/set_zoom?value={TARGET_ZOOM}")


def query_selected_entity(base_url: str, entity_id: str):
    escaped = entity_id.replace("'", "''")
    sql = f"""
        SELECT up.parcel_layer_idx, pf.feature_idx, up.parcel_entity_id,
               up.parcel_geometry_entity_id, up.blocklot, up.address,
               up.has_property_record, pf.blocklot AS feature_blocklot,
               pf.address AS feature_address
        FROM unified_parcels up
        JOIN parcel_features pf
          ON pf.layer_idx = up.parcel_layer_idx
         AND pf.entity_id = up.parcel_entity_id
        WHERE up.parcel_entity_id = '{escaped}'
        LIMIT 5
    """
    return request_json(base_url, "/controls/query?limit=5&sql=" + urllib.parse.quote(sql), timeout=10.0)


def click_county_parcel(base_url: str, target_layer: int):
    attempts = []
    for x, y in TARGET_SCREEN_POINTS:
        request_ok(base_url, f"/ui?action=move&x={x}&y={y}")
        time.sleep(0.2)
        before = request_json(base_url, "/status", timeout=5.0)
        request_ok(base_url, f"/ui?action=click&x={x}&y={y}&button=0")
        time.sleep(0.4)
        after = request_json(base_url, "/status", timeout=5.0)
        hover = after.get("hover", {})
        attempts.append({
            "x": x,
            "y": y,
            "hover_layer": before.get("hover", {}).get("hovered_parcel_layer_idx"),
            "hover_feature": before.get("hover", {}).get("hovered_parcel_idx"),
            "selected_layer": hover.get("selected_parcel_layer_idx"),
            "selected_feature": hover.get("selected_parcel_idx"),
            "selected_entity": hover.get("selected_parcel_entity_id"),
        })
        if hover.get("selected_parcel_layer_idx") == target_layer and hover.get("selected_parcel_entity_id"):
            return after, attempts
    raise RuntimeError("could not select Baltimore County parcel: " + json.dumps(attempts, indent=2))


def cpu_seconds(profile: dict) -> float:
    process = profile.get("process", {})
    return float(process.get("user_cpu_seconds", 0.0)) + float(process.get("system_cpu_seconds", 0.0))


def summarize_perf(base_url: str, sample_seconds: float):
    request_ok(base_url, "/profile/reset")
    time.sleep(0.5)
    before = request_json(base_url, "/profile", timeout=5.0)
    start_cpu = cpu_seconds(before)
    start = time.time()
    time.sleep(sample_seconds)
    after = request_json(base_url, "/profile", timeout=5.0)
    elapsed = max(0.001, time.time() - start)
    cpu_pct = ((cpu_seconds(after) - start_cpu) / elapsed) * 100.0
    frame = after.get("frame", {})
    history = after.get("history", {}).get("frame_ms", {})
    phases = after.get("phases_ms_last", {})
    return {
        "fps_avg": float(frame.get("fps_avg", 0.0)),
        "frame_ms_avg": float(frame.get("frame_ms_avg", 0.0)),
        "frame_ms_p95": float(history.get("p95", 0.0)),
        "sample_count": after.get("history", {}).get("sample_count", 0),
        "cpu_pct": cpu_pct,
        "process": after.get("process", {}),
        "phases_ms_last": phases,
    }


def start_worldsim(worldsim_bin: str):
    log = tempfile.NamedTemporaryFile(prefix="worldsim3-rest-perf-", suffix=".log", delete=False)
    proc = subprocess.Popen(
        [worldsim_bin],
        cwd=os.getcwd(),
        stdout=log,
        stderr=subprocess.STDOUT,
    )
    return proc, log.name


def stop_worldsim(proc):
    if not proc or proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=8)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)


def main() -> int:
    parser = argparse.ArgumentParser(description="REST-driven Baltimore County parcel app performance smoke test")
    parser.add_argument("--base-url", default=DEFAULT_BASE_URL)
    parser.add_argument("--worldsim-bin", default=os.environ.get("WORLD_SIM_BIN", "./build/worldsim3"))
    parser.add_argument("--use-running", action="store_true", help="use an existing REST instance instead of spawning one")
    parser.add_argument("--startup-timeout", type=float, default=45.0)
    parser.add_argument("--ready-timeout", type=float, default=90.0)
    parser.add_argument("--sample-seconds", type=float, default=4.0)
    parser.add_argument("--min-fps", type=float, default=20.0)
    parser.add_argument("--max-frame-ms", type=float, default=50.0)
    parser.add_argument("--max-frame-p95-ms", type=float, default=80.0)
    parser.add_argument("--max-cpu-pct", type=float, default=250.0)
    args = parser.parse_args()

    proc = None
    log_path = None
    try:
        if args.use_running:
            wait_for_status(args.base_url, args.startup_timeout)
        else:
            proc, log_path = start_worldsim(args.worldsim_bin)
            wait_for_spawned_status(args.base_url, proc.pid, args.startup_timeout)

        configure_scene(args.base_url)
        status = wait_for_enabled_layers_ready(
            args.base_url,
            (COUNTY_ZONING_FILE, COUNTY_PARCEL_FILE),
            args.ready_timeout,
        )
        parcel_layer = layer_by_file(status, COUNTY_PARCEL_FILE)
        if not parcel_layer:
            raise RuntimeError(f"missing target layer: {COUNTY_PARCEL_FILE}")
        target_layer_idx = int(parcel_layer["index"])

        selection_status, click_attempts = click_county_parcel(args.base_url, target_layer_idx)
        hover = selection_status.get("hover", {})
        selected_entity = hover.get("selected_parcel_entity_id", "")
        query = query_selected_entity(args.base_url, selected_entity) if selected_entity else {"ok": False, "rows": []}
        rows = query.get("rows", [])
        row = rows[0] if rows else {}

        perf = summarize_perf(args.base_url, args.sample_seconds)
        ok = (
            query.get("ok") is True and
            bool(row.get("blocklot")) and
            str(row.get("feature_idx", "")) == str(hover.get("selected_parcel_idx")) and
            perf["fps_avg"] >= args.min_fps and
            0.0 < perf["frame_ms_avg"] <= args.max_frame_ms and
            0.0 < perf["frame_ms_p95"] <= args.max_frame_p95_ms and
            perf["cpu_pct"] <= args.max_cpu_pct
        )
        out = {
            "mode": "rest-parcel-county-perf",
            "ok": ok,
            "thresholds": {
                "min_fps": args.min_fps,
                "max_frame_ms": args.max_frame_ms,
                "max_frame_p95_ms": args.max_frame_p95_ms,
                "max_cpu_pct": args.max_cpu_pct,
            },
            "target_layer": {
                "file": COUNTY_PARCEL_FILE,
                "index": target_layer_idx,
                "features": parcel_layer.get("features"),
            },
            "selected": {
                "layer": hover.get("selected_parcel_layer_idx"),
                "feature": hover.get("selected_parcel_idx"),
                "entity": selected_entity,
                "geometry": hover.get("selected_parcel_geometry_entity_id"),
            },
            "query_ok": query.get("ok"),
            "query_message": query.get("message"),
            "row": row,
            "performance": perf,
            "click_attempts": click_attempts,
            "spawned_log": log_path,
        }
        print(json.dumps(out, indent=2))
        return 0 if ok else 1
    except Exception as exc:
        print(json.dumps({
            "mode": "rest-parcel-county-perf",
            "ok": False,
            "error": str(exc),
            "spawned_log": log_path,
        }, indent=2), file=sys.stderr)
        return 1
    finally:
        if not args.use_running:
            stop_worldsim(proc)


if __name__ == "__main__":
    sys.exit(main())
