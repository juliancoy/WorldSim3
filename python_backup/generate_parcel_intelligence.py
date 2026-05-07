#!/usr/bin/env python3
import json
import math
from collections import Counter, defaultdict
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LAYERS = ROOT / "data" / "layers"
OUT_PATH = LAYERS / "parcel_intelligence.geojson"


def load_geojson(name: str):
    p = LAYERS / name
    if not p.exists():
        return {"type": "FeatureCollection", "features": []}
    return json.loads(p.read_text())


def norm_key(v) -> str:
    if v is None:
        return ""
    return "".join(ch.upper() for ch in str(v) if ch.isalnum())


def to_float(v) -> float:
    if v is None:
        return 0.0
    if isinstance(v, (int, float)):
        return float(v)
    s = str(v).strip().replace(",", "").replace("$", "")
    if not s:
        return 0.0
    try:
        return float(s)
    except Exception:
        return 0.0


def parse_dt(v):
    if v is None:
        return None
    if isinstance(v, (int, float)):
        # ArcGIS milliseconds epoch in many datasets.
        if v > 10_000_000_000:
            return datetime.fromtimestamp(float(v) / 1000.0, tz=timezone.utc)
        return datetime.fromtimestamp(float(v), tz=timezone.utc)
    s = str(v).strip()
    if not s:
        return None
    for fmt in ("%Y-%m-%d", "%m/%d/%Y", "%Y/%m/%d", "%m/%d/%y", "%Y-%m-%dT%H:%M:%S", "%Y-%m-%d %H:%M:%S"):
        try:
            return datetime.strptime(s[:19], fmt).replace(tzinfo=timezone.utc)
        except Exception:
            pass
    return None


def pick_date(props: dict, keys):
    for k in keys:
        d = parse_dt(props.get(k))
        if d:
            return d
    return None


def pick_value(props: dict, keys):
    for k in keys:
        x = to_float(props.get(k))
        if x > 0:
            return x
    return 0.0


def point_in_ring(ring, x, y):
    inside = False
    n = len(ring)
    if n < 3:
        return False
    j = n - 1
    for i in range(n):
        xi, yi = ring[i][0], ring[i][1]
        xj, yj = ring[j][0], ring[j][1]
        if ((yi > y) != (yj > y)):
            denom = (yj - yi) if (yj - yi) != 0 else 1e-12
            x_cross = (xj - xi) * (y - yi) / denom + xi
            if x < x_cross:
                inside = not inside
        j = i
    return inside


def point_in_polygon_rings(rings, x, y):
    if not rings:
        return False
    if not point_in_ring(rings[0], x, y):
        return False
    for hole in rings[1:]:
        if point_in_ring(hole, x, y):
            return False
    return True


def main() -> int:
    parcel = load_geojson("parcel.geojson")
    rp = load_geojson("real_property_information.geojson")
    permits = load_geojson("housing_building_permits_2019_present.geojson")
    vac_notice = load_geojson("vacant_building_notices.geojson")
    vac_rehab = load_geojson("vacant_building_rehabs.geojson")
    tax_lien = load_geojson("tax_lien_certificate_sale_properties.geojson")
    tax_sale = load_geojson("tax_sale_list_2021.geojson")
    foreclosure = load_geojson("foreclosure_filings.geojson")
    recv_open = load_geojson("receivership_filed_open.geojson")
    recv_settled = load_geojson("receivership_settled.geojson")
    cip = load_geojson("cip_fy14_20_projects_point.geojson")

    now = datetime.now(timezone.utc)
    d90 = 90
    d365 = 365
    d1825 = 365 * 5

    # Real-property joins and owner concentration.
    rp_by_blocklot = {}
    owner_by_blocklot = {}
    owner_portfolio = Counter()
    for f in rp.get("features", []):
        p = f.get("properties", {})
        bl = norm_key(p.get("BLOCKLOT") or p.get("PIN"))
        if not bl:
            continue
        tax_base = to_float(p.get("TAXBASE")) or to_float(p.get("ARTAXBAS"))
        value = tax_base if tax_base > 0 else (to_float(p.get("CURRLAND")) + to_float(p.get("CURRIMPR")))
        if value <= 0:
            value = to_float(p.get("SALEPRIC"))
        if bl not in rp_by_blocklot or value > rp_by_blocklot[bl]["property_value_usd"]:
            rp_by_blocklot[bl] = {
                "property_value_usd": value,
                "tax_base_usd": tax_base,
                "sale_price_usd": to_float(p.get("SALEPRIC")),
                "owner_name": str(p.get("OWNER_1") or p.get("OWNERNME1") or "").strip(),
            }
        owner = rp_by_blocklot[bl]["owner_name"]
        if owner:
            owner_by_blocklot[bl] = owner

    for bl, owner in owner_by_blocklot.items():
        owner_portfolio[owner] += 1

    # Per-parcel event buckets by blocklot.
    events = defaultdict(list)
    permits_by_blocklot = defaultdict(list)
    lien_amount_by_blocklot = Counter()
    receivership_open_dates = defaultdict(list)
    receivership_settled_dates = defaultdict(list)

    def add_event(bl, typ, dt=None, amount=0.0):
        if not bl:
            return
        events[bl].append((typ, dt, amount))

    for f in vac_notice.get("features", []):
        p = f.get("properties", {})
        bl = norm_key(p.get("BLOCKLOT"))
        dt = pick_date(p, ["DateNotice", "DateAbate", "DateCancel"])
        add_event(bl, "vac_notice", dt, 0.0)

    for f in vac_rehab.get("features", []):
        p = f.get("properties", {})
        bl = norm_key(p.get("BLOCKLOT"))
        dt = pick_date(p, ["DateIssue", "DateIssued"])
        add_event(bl, "vac_rehab", dt, 0.0)

    for f in permits.get("features", []):
        p = f.get("properties", {})
        bl = norm_key(p.get("BLOCKLOT"))
        dt = pick_date(p, ["IssuedDate"])
        cost = to_float(p.get("Cost"))
        permits_by_blocklot[bl].append((dt, cost))
        add_event(bl, "permit", dt, cost)

    for f in tax_lien.get("features", []):
        p = f.get("properties", {})
        bl = norm_key((p.get("BLOCK") or "")) + norm_key((p.get("LOT") or ""))
        dt = pick_date(p, ["REDEMPTION_DATE"])
        amt = pick_value(p, ["TOTAL_AMOUNT", "LIENS"])
        lien_amount_by_blocklot[bl] += amt
        add_event(bl, "tax_lien", dt, amt)

    for f in tax_sale.get("features", []):
        p = f.get("properties", {})
        bl = norm_key((p.get("block") or "")) + norm_key((p.get("lot") or ""))
        dt = pick_date(p, ["deed_date", "when_sold"])
        amt = pick_value(p, ["total_lien", "total_li_1", "total_3yea"])
        add_event(bl, "tax_sale", dt, amt)

    for f in foreclosure.get("features", []):
        p = f.get("properties", {})
        bl = norm_key(p.get("BLOCKLOT"))
        dt = pick_date(p, ["Date"])
        add_event(bl, "foreclosure", dt, 0.0)

    for f in recv_open.get("features", []):
        p = f.get("properties", {})
        bl = norm_key(p.get("BLOCKLOT"))
        dt = pick_date(p, ["DateFiled"])
        receivership_open_dates[bl].append(dt)
        add_event(bl, "receivership_open", dt, 0.0)

    for f in recv_settled.get("features", []):
        p = f.get("properties", {})
        bl = norm_key(p.get("BLOCKLOT"))
        dt = pick_date(p, ["DateFiled", "DateAuction"])
        receivership_settled_dates[bl].append(dt)
        add_event(bl, "receivership_settled", dt, 0.0)

    # Spatial assign CIP points to parcels with a lightweight bbox grid.
    parcels = parcel.get("features", [])
    grid = defaultdict(list)
    cell = 0.005  # degrees
    parcel_bbox = []
    parcel_rings = []
    parcel_blocklot = []

    for idx, f in enumerate(parcels):
        geom = f.get("geometry") or {}
        props = f.get("properties", {})
        bl = norm_key(props.get("BLOCKLOT") or props.get("PIN"))
        parcel_blocklot.append(bl)
        coords = geom.get("coordinates") or []
        if geom.get("type") == "MultiPolygon":
            rings = []
            for poly in coords:
                if isinstance(poly, list):
                    rings.extend(poly)
        else:
            rings = coords
        # Normalize to rings [[x,y],...]
        if rings and isinstance(rings[0][0], (int, float)):
            rings = [rings]
        flat = [pt for ring in rings for pt in ring] if rings else []
        if not flat:
            parcel_bbox.append(None)
            parcel_rings.append([])
            continue
        xs = [p[0] for p in flat]
        ys = [p[1] for p in flat]
        b = (min(xs), min(ys), max(xs), max(ys))
        parcel_bbox.append(b)
        parcel_rings.append(rings)
        gx0, gy0 = int(b[0] / cell), int(b[1] / cell)
        gx1, gy1 = int(b[2] / cell), int(b[3] / cell)
        for gx in range(gx0, gx1 + 1):
            for gy in range(gy0, gy1 + 1):
                grid[(gx, gy)].append(idx)

    cip_by_blocklot = Counter()
    for f in cip.get("features", []):
        geom = f.get("geometry") or {}
        props = f.get("properties", {})
        if geom.get("type") != "Point":
            continue
        xy = geom.get("coordinates") or []
        if len(xy) < 2:
            continue
        x, y = xy[0], xy[1]
        gx, gy = int(x / cell), int(y / cell)
        amt = pick_value(
            props,
            ["Totals_1", "City_Bond_Funds", "City_General_Funds", "Revenue_Loans", "Utility_Funds", "Federal_Funds", "State_Funds"],
        )
        for idx in grid.get((gx, gy), []):
            b = parcel_bbox[idx]
            if not b:
                continue
            if not (b[0] <= x <= b[2] and b[1] <= y <= b[3]):
                continue
            if point_in_polygon_rings(parcel_rings[idx], x, y):
                bl = parcel_blocklot[idx]
                if bl:
                    cip_by_blocklot[bl] += amt
                break

    # Distress per owner.
    owner_distressed = Counter()
    for bl, evs in events.items():
        owner = owner_by_blocklot.get(bl, "")
        if not owner:
            continue
        severe = any(t in {"vac_notice", "tax_lien", "tax_sale", "foreclosure", "receivership_open"} for t, _, _ in evs)
        if severe:
            owner_distressed[owner] += 1

    out_features = []
    group_vals = defaultdict(lambda: {"value": [], "risk": [], "inv": []})

    # First pass compute metrics.
    per_feature_metrics = []
    for f in parcels:
        props = dict(f.get("properties", {}))
        bl = norm_key(props.get("BLOCKLOT") or props.get("PIN"))
        rpv = rp_by_blocklot.get(bl, {})
        value = float(rpv.get("property_value_usd", 0.0))
        tax_base = float(rpv.get("tax_base_usd", 0.0))
        sale_price = float(rpv.get("sale_price_usd", 0.0))
        owner = rpv.get("owner_name", "")
        owner_count = owner_portfolio.get(owner, 0)
        owner_dist_rate = (owner_distressed.get(owner, 0) / owner_count) if owner_count > 0 else 0.0

        evs = events.get(bl, [])
        ev_types = set()
        latest = None
        first_vac = None
        last_permit = None
        permit_1y = 0.0
        permit_5y = 0.0
        ev90 = 0
        ev1y = 0
        for t, dt, amt in evs:
            ev_types.add(t)
            if dt:
                if latest is None or dt > latest:
                    latest = dt
                age = (now - dt).days
                if age <= d90:
                    ev90 += 1
                if age <= d365:
                    ev1y += 1
                if t in {"vac_notice", "vac_rehab"}:
                    if first_vac is None or dt < first_vac:
                        first_vac = dt
                if t == "permit":
                    if last_permit is None or dt > last_permit:
                        last_permit = dt
                    if age <= d365:
                        permit_1y += amt
                    if age <= d1825:
                        permit_5y += amt

        cip_amt = float(cip_by_blocklot.get(bl, 0.0))
        invest_1y = permit_1y  # CIP lacks exact timestamp here.
        invest_5y = permit_5y + cip_amt

        lien_amt = float(lien_amount_by_blocklot.get(bl, 0.0))
        lien_to_value = (lien_amt / value) if value > 0 else 0.0
        sale_to_assessed = (sale_price / tax_base) if sale_price > 0 and tax_base > 0 else 0.0

        vac_days = (now - first_vac).days if first_vac else -1
        days_since_permit = (now - last_permit).days if last_permit else -1

        ro = receivership_open_dates.get(bl, [])
        rs = receivership_settled_dates.get(bl, [])
        rec_open_days = -1
        if ro:
            latest_open = max([d for d in ro if d] or [None])
            if latest_open:
                settled_after = [d for d in rs if d and d >= latest_open]
                if settled_after:
                    rec_open_days = (min(settled_after) - latest_open).days
                else:
                    rec_open_days = (now - latest_open).days

        # Composite scores.
        distress_count = sum(1 for t, _, _ in evs if t in {"vac_notice", "tax_lien", "tax_sale", "foreclosure", "receivership_open"})
        risk_score = (
            min(40.0, distress_count * 8.0)
            + min(20.0, lien_to_value * 100.0)
            + (10.0 if vac_days > 365 else 0.0)
            + (8.0 if days_since_permit > 365 * 2 else 0.0)
            + min(12.0, owner_dist_rate * 40.0)
            + min(10.0, ev1y * 1.5)
        )
        risk_score = max(0.0, min(100.0, risk_score))
        if risk_score >= 75:
            risk_band = "critical"
        elif risk_score >= 55:
            risk_band = "high"
        elif risk_score >= 30:
            risk_band = "moderate"
        else:
            risk_band = "low"

        momentum = min(100.0, math.log10(1.0 + invest_5y) * 18.0 + min(20.0, ev1y * 2.0))

        group_key = str(props.get("BLOCKNUM") or "")[:4] or "UNGROUPED"
        group_vals[group_key]["value"].append(value)
        group_vals[group_key]["risk"].append(risk_score)
        group_vals[group_key]["inv"].append(invest_5y)

        metrics = {
            "property_value_usd": value,
            "owner_name": owner,
            "owner_portfolio_count": owner_count,
            "owner_distress_rate": owner_dist_rate,
            "vacancy_days_est": vac_days,
            "days_since_last_permit": days_since_permit,
            "tax_lien_amount_usd": lien_amt,
            "lien_to_value_ratio": lien_to_value,
            "sale_to_assessed_ratio": sale_to_assessed,
            "receivership_open_days": rec_open_days,
            "events_90d": ev90,
            "events_1y": ev1y,
            "event_diversity_index": len(ev_types),
            "investment_1y_usd": invest_1y,
            "investment_5y_usd": invest_5y,
            "momentum_score": momentum,
            "risk_score": risk_score,
            "risk_band": risk_band,
            "latest_event_date": latest.date().isoformat() if latest else "",
            "vacancy_event_first_date": first_vac.date().isoformat() if first_vac else "",
            "risk_drivers": ",".join(sorted(ev_types)),
            "cip_allocation_usd": cip_amt,
        }
        per_feature_metrics.append((props, f.get("geometry"), group_key, metrics))

    # Group z-scores (relative baselines by block group proxy).
    group_stats = {}
    for g, vals in group_vals.items():
        group_stats[g] = {}
        for k in ("value", "risk", "inv"):
            arr = vals[k]
            if not arr:
                group_stats[g][k] = (0.0, 1.0)
                continue
            m = sum(arr) / len(arr)
            var = sum((x - m) ** 2 for x in arr) / max(1, len(arr))
            sd = math.sqrt(var) if var > 1e-12 else 1.0
            group_stats[g][k] = (m, sd)

    for props, geom, gk, m in per_feature_metrics:
        vm, vs = group_stats[gk]["value"]
        rm, rs = group_stats[gk]["risk"]
        im, isd = group_stats[gk]["inv"]
        m["value_z_group"] = (m["property_value_usd"] - vm) / vs
        m["distress_z_group"] = (m["risk_score"] - rm) / rs
        m["investment_z_group"] = (m["investment_5y_usd"] - im) / isd
        m["group_key"] = gk
        outp = dict(props)
        outp.update(m)
        out_features.append({"type": "Feature", "geometry": geom, "properties": outp})

    OUT_PATH.write_text(json.dumps({"type": "FeatureCollection", "features": out_features}))
    print(f"wrote: {OUT_PATH}")
    print(f"features: {len(out_features)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
