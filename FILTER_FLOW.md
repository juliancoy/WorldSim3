# FILTER_FLOW

This document describes how filtering works in the map pipeline and how different filter systems combine.

## 1) Filter Sources (UI State)

Main filter state is collected in the left panel:

- Global toggle: `filter_enabled`
- Date filter: `filter_use_date`, `filter_year_min`, `filter_year_max`
- Field filters: `filter_blocklot`, `filter_status`, `filter_address`, `filter_owner`, `filter_zip`
- Crime filter toggle and type toggles: `crime_filter_enabled` + per-crime booleans
- Owner selection set: `selected_owners` (from Owners tab)
- Zoning visibility map: `zoning_zone_enabled[zoning_code]`

Primary UI wiring:

- [worldsim_app_run_loop_part3.inc](/home/julian/Documents/worldsim3/worldsim_app_run_loop_part3.inc:346)
- [worldsim_app_run_loop_part1.inc](/home/julian/Documents/worldsim3/worldsim_app_run_loop_part1.inc:991)

## 2) Core Runtime Predicate: `feature_passes_filters`

All map feature filtering flows through `feature_passes_filters(layer_idx, feature_idx, fg)` in:

- [worldsim_app_run_loop_part4.inc](/home/julian/Documents/worldsim3/worldsim_app_run_loop_part4.inc:23)

### 2.1 Crime layers branch

If the current layer is a crime layer, filtering uses crime logic only:

- If both `filter_enabled` and `crime_filter_enabled` are off, pass all crime features.
- Otherwise, evaluate crime-specific year/type matching (`crime_feature_matches`).

### 2.2 Non-crime branch

For all other layers:

1. Determine parcel-domain membership with `is_parcel_related_layer(layer_idx)`:
- any layer whose manifest/config scale is `\"parcel\"` (`layers[layer_idx].scale == \"parcel\"`)

2. Owner selection filter (`selected_owners`) is applied only when:
- layer is parcel-related, and
- owners are selected, and
- real property layer is available.

3. If `filter_enabled` is off, only the owner-selection gate above may still apply.

4. If `filter_enabled` is on, apply field/date filters:
- Date (`filter_use_date`)
- Block/Lot
- Status
- Address (normalized token matching)
- Owner text query (`filter_owner`) only for parcel-related layers
- ZIP

Important join behavior:

- For many checks, the system may join a feature to real-property by normalized block/lot (`rp_join`) and fall back to joined properties.

## 3) Zoning Filters Are Separate From Owner/Field Filters

Zoning has an additional class-visibility gate:

- `zoning_zone_enabled[zkey]`

Even if `feature_passes_filters` passes, zoning can still be hidden by class toggle.

Where applied:

- Spatial-index draw path: [worldsim_app_run_loop_part4.inc](/home/julian/Documents/worldsim3/worldsim_app_run_loop_part4.inc:590)
- Full-scan draw path: [worldsim_app_run_loop_part4.inc](/home/julian/Documents/worldsim3/worldsim_app_run_loop_part4.inc:667)

## 4) Draw Order and Visual Stacking

Layer traversal is reordered into `draw_layer_order`:

1. Non-zoning, non-parcel-related layers
2. Zoning layer
3. Parcel-related layers

So parcel-family layers always render above zoning.

Implementation:

- [worldsim_app_run_loop_part4.inc](/home/julian/Documents/worldsim3/worldsim_app_run_loop_part4.inc:389)

## 5) Overlays Reuse the Same Filter Predicate

Parcel source overlays (vacancy/tax outlines/fills projected on parcel polygons) call `feature_passes_filters` for parcel features.

So when parcel filters exclude a parcel, overlays for that parcel are excluded too.

Implementation:

- [map_render_overlays.cpp](/home/julian/Documents/worldsim3/map_render_overlays.cpp:45)
- [map_render_overlays.cpp](/home/julian/Documents/worldsim3/map_render_overlays.cpp:57)
- [map_render_overlays.cpp](/home/julian/Documents/worldsim3/map_render_overlays.cpp:84)

## 6) Practical Precedence Summary

For a feature to render, all relevant gates must pass:

1. Layer enabled and present in draw traversal
2. `feature_passes_filters(...)` returns true
3. Layer-specific gates (for zoning: `zoning_zone_enabled`)
4. Geometry/screen-space checks
5. Fill-mode checks for polygon fill paths (`layer_fill_enabled`, `should_fill_layer_polygon`)

For parcel overlays, an additional requirement is that the parcel has nonzero derived overlay weight (vacancy/tax signal).

## 7) Notes on Current Separation

Current behavior intentionally separates concerns:

- Owner selection (`selected_owners`) is parcel-domain only (all `scale == \"parcel\"` layers).
- Owner text filter (`filter_owner`) is parcel-domain only (all `scale == \"parcel\"` layers).
- Zoning class toggles are zoning-only.
- Crime filtering is crime-layer-specific.

This avoids cross-domain suppression (for example, owner filters hiding zoning geometry).
