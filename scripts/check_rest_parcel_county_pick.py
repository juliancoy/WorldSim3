#!/usr/bin/env python3
import json
import sys
import time
import urllib.parse
import urllib.request


BASE_URL = "http://127.0.0.1:8787"
TARGET_LON = -76.62003707885742
TARGET_LAT = 39.437034606933594
TARGET_LAYER = 10


def get_json(path: str):
    with urllib.request.urlopen(BASE_URL + path, timeout=5) as response:
        return json.loads(response.read().decode("utf-8"))


def get(path: str):
    with urllib.request.urlopen(BASE_URL + path, timeout=5) as response:
        response.read()


def wait_for_status():
    last_error = None
    for _ in range(40):
        try:
            return get_json("/status")
        except Exception as exc:
            last_error = exc
            time.sleep(0.25)
    raise RuntimeError(f"status API not ready: {last_error}")


def move_and_status(x: int, y: int):
    get(f"/ui?action=move&x={x}&y={y}")
    time.sleep(0.15)
    return get_json("/status")


def query_selected_entity(entity_id: str):
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
    return get_json("/controls/query?limit=5&sql=" + urllib.parse.quote(sql))


def main() -> int:
    wait_for_status()
    get(f"/set_center?lon={TARGET_LON}&lat={TARGET_LAT}")
    get("/set_zoom?value=17")
    time.sleep(0.4)

    # The normal desktop instance uses the map canvas center at this point after
    # startup. Keep the REST test deterministic instead of deriving coordinates
    # from transient hover frames.
    x, y = 877, 456
    status = move_and_status(x, y)
    hover_before_click = status.get("hover", {})
    get(f"/ui?action=click&x={x}&y={y}&button=0")
    time.sleep(0.3)
    status = get_json("/status")
    hover_after_click = status.get("hover", {})

    selected_entity = hover_after_click.get("selected_parcel_entity_id", "")
    selected_feature = hover_after_click.get("selected_parcel_idx")
    query = query_selected_entity(selected_entity) if selected_entity else {"ok": False, "rows": []}
    rows = query.get("rows", [])
    row = rows[0] if rows else {}

    ok = (
        hover_before_click.get("hovered_parcel_layer_idx") == TARGET_LAYER and
        hover_after_click.get("selected_parcel_layer_idx") == TARGET_LAYER and
        hover_before_click.get("hovered_parcel_idx") == selected_feature and
        query.get("ok") is True and
        str(row.get("feature_idx", "")) == str(selected_feature) and
        bool(row.get("blocklot")) and
        row.get("blocklot") == row.get("feature_blocklot")
    )

    print(json.dumps({
        "mode": "rest-parcel-county-pick",
        "ok": ok,
        "screen": {"x": x, "y": y},
        "hovered": {
            "layer": hover_before_click.get("hovered_parcel_layer_idx"),
            "feature": hover_before_click.get("hovered_parcel_idx"),
            "entity": hover_before_click.get("hovered_parcel_entity_id"),
            "geometry": hover_before_click.get("hovered_parcel_geometry_entity_id"),
        },
        "selected": {
            "layer": hover_after_click.get("selected_parcel_layer_idx"),
            "feature": hover_after_click.get("selected_parcel_idx"),
            "entity": selected_entity,
            "geometry": hover_after_click.get("selected_parcel_geometry_entity_id"),
        },
        "query_ok": query.get("ok"),
        "query_message": query.get("message"),
        "row": row,
    }, indent=2))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
