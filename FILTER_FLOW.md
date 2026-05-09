# FILTER_FLOW

This document defines the filter pipeline, the source of truth, and how UI-created filters should participate in map rendering.

## 1) Source Of Truth

`MapFilterState` is the single source of truth for map filters created by UI elements.

- Definition: [filters.h](/home/julian/Documents/worldsim3/filters.h:11)
- Owner instance: [worldsim_app_run.cpp](/home/julian/Documents/worldsim3/worldsim_app_run.cpp:511)
- Runtime evaluator: [filters.cpp](/home/julian/Documents/worldsim3/filters.cpp:60)

UI tabs should mutate fields on `MapFilterState`; they should not create parallel filter state that the renderer also has to know about.

Current `MapFilterState` fields:

- Global map filter toggle: `enabled`
- Date filter: `use_date`, `year_min`, `year_max`
- Field filters: `blocklot`, `status`, `address`, `owner`, `zip`
- Crime filters: `crime.enabled`, per-crime toggles, `crime.use_year`, `crime.year_min`, `crime.year_max`
- Owner selection: `selected_owners`

The current run loop keeps reference aliases like `filter_enabled` and `selected_owners` so existing ImGui code remains readable, but storage still lives in `map_filter_state`.

## 2) Evaluation Context

`FeatureFilterContext` is not filter state. It is the read-only evaluation context passed to filter functions for one frame.

It contains:

- Pointer to the SSOT: `map_filters`
- Dataset references needed for joins: layers, real-property blocklot index, vacancy vectors
- Layer identity indexes: parcel, real-property, crime layers
- Optional canonical result set: `result_set`

Frame wiring:

- [worldsim_app_run_loop_part4.inc](/home/julian/Documents/worldsim3/worldsim_app_run_loop_part4.inc:16)

Rule: if a filter is created by a UI element, put it in `MapFilterState`. If a filter is produced by an engine/query operation, put its canonical output in `FilterResultSet`.

## 3) Canonical Filter Outputs

`FilterResultSet` is the bridge for non-UI filters such as SQL query results.

It can carry:

- `features`: exact `(layer_idx, feature_idx)` identities
- `blocklots`: parcel-domain identities
- `owners`: normalized owner identities

Renderer code should not parse SQL rows or UI widget state directly. SQL and other engines should convert their output into `FilterResultSet`; the renderer continues to call `featurePassesFilters(...)`.

When `FilterResultSet::active` is true and all identity sets are empty, no features pass that result-set gate. That represents a query/filter that returned zero matches.

`QueryMapLayer` wraps a `FilterResultSet` with a user-visible name, enabled flag, SQL text, and RGBA color. Active query layers do not replace the base filter SSOT; they are color overlays resolved during rendering.

SQL selection tables exposed to queries:

- `ui_selected_owners(owner)`: owners selected in the Owners tab
- `ui_selected_parcels(layer_idx, feature_idx, blocklot)`: active Parcel Info selection

## 4) Runtime Predicate

All feature visibility filtering flows through:

- `featurePassesFilters(...)` in [filters.cpp](/home/julian/Documents/worldsim3/filters.cpp:60)

The renderer wraps this as `feature_passes_filters(...)` and uses it consistently for direct layer drawing, heatmap sampling, and parcel overlays.

Pipeline:

1. Optional `FilterResultSet` gate
2. Crime-layer branch, if current layer is a crime layer
3. Parcel-domain owner selection gate
4. General field/date filters from `MapFilterState`
5. Query color overlay resolution from active `QueryMapLayer` objects
6. Layer-specific render gates outside generic filtering

## 5) Crime Layers

Crime layers use crime-specific filters only.

If both `MapFilterState::enabled` and `MapFilterState::crime.enabled` are false, crime features pass.

If crime filtering is active, the evaluator applies:

- Optional crime year range
- Selected crime categories

## 6) Parcel-Domain Filters

Parcel-domain means any layer whose manifest scale is `parcel`:

- `layers[layer_idx].scale == "parcel"`

This intentionally includes more than the base parcel polygon layer: real property information, vacancy, rehab, tax, and other parcel-renderable layers all participate when they are configured as parcel scale.

Parcel-domain filters:

- Selected owners
- Owner text query
- Real-property joins by normalized blocklot
- Parcel overlays

Owner selection remains active even when the global filter toggle is off. That lets the Owners tab act as a direct selection/highlight mechanism.

## 6.1) Unified Parcel Source

Parcel-specific UI and SQL should use `UnifiedParcelRecord` / `unified_parcels` as the canonical parcel source.

The unified record combines:

- Base parcel geometry and feature identity
- Real Property Information joined by normalized blocklot
- Owner, address, zip, and status
- Current assessed value fields: current land, current improvements, tax base, sale price, and computed current value
- Vacancy notice/rehab counts
- Tax lien/sale counts and amounts

Runtime builder:

- [parcel_unified.cpp](/home/julian/Documents/worldsim3/parcel_unified.cpp:45)

DuckDB table:

- `unified_parcels`

Use `unified_parcels` for parcel queries when possible. Use `parcel_features` only when you need raw layer-row records for all parcel-scale layers.

## 7) Zoning Filters

Zoning visibility is separate from owner and parcel field filters.

Zoning uses its own class toggle map:

- `zoning_zone_enabled[zoning_code]`

This prevents parcel owner filters from hiding zoning geometry. Zoning visibility is applied after generic feature filtering in the zoning draw paths.

## 8) Draw Order

Layer draw order is intentionally grouped:

1. Non-zoning, non-parcel layers
2. Zoning layer
3. Parcel-domain layers

This keeps parcel-related geometry above zoning fills.

Implementation:

- [worldsim_app_run_loop_part4.inc](/home/julian/Documents/worldsim3/worldsim_app_run_loop_part4.inc:320)

## 9) Persistence

Filter persistence should read/write the SSOT fields from `MapFilterState`, including `selected_owners`.

Current persistence functions still use field-level parameters for compatibility:

- Load: [layer_state_io.cpp](/home/julian/Documents/worldsim3/layer_state_io.cpp:428)
- Save: [layer_state_io.cpp](/home/julian/Documents/worldsim3/layer_state_io.cpp:501)

The app passes references backed by `map_filter_state`, so persisted data still restores into the SSOT.

## 10) Adding A New UI Filter

Use this workflow:

1. Add a field to `MapFilterState` in [filters.h](/home/julian/Documents/worldsim3/filters.h:11).
2. Render a UI control that mutates that field.
3. Read that field inside `featurePassesFilters(...)` or a helper in [filters.cpp](/home/julian/Documents/worldsim3/filters.cpp:60).
4. Include it in heatmap/cache keys if it changes rendered output.
5. Persist it if users expect it to survive restart.

Do not add renderer-local filter variables. They create ambiguous precedence and make map/query behavior diverge.

## 11) Adding A SQL Filter

Use this workflow:

1. Execute SQL through the SQL tab or analytics layer.
2. Include at least one canonical identity in the query result: `layer_idx + feature_idx`, `blocklot`, or `owner`.
3. Convert query rows to `FilterResultSet` identities.
4. Store the result in a `QueryMapLayer` with its own color.
5. Attach query layers to `FeatureFilterContext::query_layers`.
6. Let the renderer call `queryMapColorForFeature(...)` to color matching map elements.

Do not make SQL directly toggle layer visibility or mutate ad-hoc UI fields unless the SQL result is explicitly being saved as a UI filter.
