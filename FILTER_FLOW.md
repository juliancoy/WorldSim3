# FILTER_FLOW

This document defines the runtime filter flow and the boundary between browse state and render state.

## 1) State Boundaries

`MapFilterState` is the single source of truth for runtime UI filters.

- Definition: [filters.h](/mnt/Cancer/worldsim3/filters.h:34)
- Owner instance: [app_main_loop.cpp](/mnt/Cancer/worldsim3/app_main_loop.cpp:710)
- Runtime evaluator: [filters.cpp](/mnt/Cancer/worldsim3/filters.cpp:218)

`MapFilterState` contains only runtime filters:

- `enabled`
- `use_date`, `year_min`, `year_max`
- `blocklot`, `status`, `address`, `owner`, `zip`
- `crime.*`
- `selected_owners`
- `event_sector_enabled`

It does not contain geography browse state.

`LayerBrowseState` is separate UI state used only to decide which layers are shown in the left column.

- Definition: [filters.h](/mnt/Cancer/worldsim3/filters.h:29)
- Left-panel owner: [left_panel.cpp](/mnt/Cancer/worldsim3/left_panel.cpp:239)
- Browse matcher: [filters.cpp](/mnt/Cancer/worldsim3/filters.cpp:199)

Rule:

- If it changes rendered/runtime behavior, it belongs in `MapFilterState`, layer enablement, or per-layer settings.
- If it only narrows the layer browser UI, it belongs in `LayerBrowseState`.

## 2) Runtime Inputs

Runtime map behavior is driven by:

1. Enabled layers
2. Per-layer settings
3. `MapFilterState`
4. Optional `FilterResultSet` / `QueryMapLayer` outputs

Runtime map behavior is not driven by left-panel geography browse state.

## 3) Evaluation Context

`FeatureFilterContext` is read-only frame context, not persistent state.

It contains:

- `map_filters`
- layer/dataset references used for joins
- result-set gates
- query overlays
- parcel-related cached lookup tables

Builder:

- [filter_context_builder.cpp](/mnt/Cancer/worldsim3/filter_context_builder.cpp:5)

## 4) Visibility Predicate

All feature visibility filtering flows through:

- `featurePassesFilters(...)` in [filters.cpp](/mnt/Cancer/worldsim3/filters.cpp:218)

High-level order:

1. Active `FilterResultSet` gates
2. Crime-layer branch, if applicable
3. Parcel-domain owner-selection gate
4. General field/date filters from `MapFilterState`
5. Layer-specific render gates outside generic filtering

Color overlays are resolved separately through `queryMapColorForFeature(...)`.

Best practice:

- Keep visibility logic centralized in `featurePassesFilters(...)`.
- Do not add geography gates to runtime filtering.

## 5) Left-Panel Geography Browse

Nation/region selection in the left panel is browse state only.

It affects:

- which layer rows are visible in the left column
- bulk left-panel actions that operate on the currently shown layer rows

It does not affect:

- map rendering
- hover/inspection
- query execution context
- map-view restoration
- runtime filter predicates

Browse persistence:

- [layer_state_io.cpp](/mnt/Cancer/worldsim3/layer_state_io.cpp:859)

## 6) Parcel-Domain Filters

Parcel-domain means layers whose manifest scale is `parcel`.

Parcel-domain filters include:

- selected owners
- owner text query
- real-property joins by normalized blocklot
- parcel overlays

Owner selection remains active even when the global filter toggle is off.

## 7) Unified Parcel Source

Parcel-specific UI and SQL should prefer `UnifiedParcelRecord` / `unified_parcels`.

Runtime builder:

- [parcel_unified.cpp](/mnt/Cancer/worldsim3/parcel_unified.cpp:45)

DuckDB table:

- `unified_parcels`

## 8) Zoning State

Zoning visibility is separate from parcel-owner and field filters.

Zoning class enablement uses:

- `zoning_zone_enabled[zoning_code]`

Derived zoning UI state is built from enabled zoning polygon layers:

- [derived_layer_caches.cpp](/mnt/Cancer/worldsim3/derived_layer_caches.cpp:137)

Current limitation:

- zoning class state is still global by raw zoning code string
- hover/inspection still targets one active zoning layer at a time

That limitation is independent of left-panel geography browse state.

## 9) Hover Model

Hover precedence is:

1. Point features
2. Parcels
3. Zoning

Implementation:

- [map_render_hover.cpp](/mnt/Cancer/worldsim3/map_render_hover.cpp:177)

Implication:

- zoning hover can still be masked by parcel hover in `All supported` mode

## 10) Draw Order

Layer draw order is intentionally grouped:

1. Non-zoning, non-parcel layers
2. Zoning layers
3. Parcel-domain layers

Implementation:

- [render_plan_builder.cpp](/mnt/Cancer/worldsim3/render_plan_builder.cpp:19)

## 11) Persistence

Runtime filter persistence should read/write `MapFilterState` only.

- Load filter state: [layer_state_io.cpp](/mnt/Cancer/worldsim3/layer_state_io.cpp:610)
- Save filter state: [layer_state_io.cpp](/mnt/Cancer/worldsim3/layer_state_io.cpp:698)

Map view persistence is global, not geography-scoped.

- Load map view state: [layer_state_io.cpp](/mnt/Cancer/worldsim3/layer_state_io.cpp:778)
- Save map view state: [layer_state_io.cpp](/mnt/Cancer/worldsim3/layer_state_io.cpp:822)

Layer-browser geography is persisted separately from runtime filters.

## 12) Adding A New UI Filter

Use this workflow:

1. Add a field to `MapFilterState`.
2. Mutate it from UI.
3. Read it inside `featurePassesFilters(...)` or a helper it calls.
4. Include it in cache keys if it changes rendered output.
5. Persist it if it should survive restart.

Do not add runtime behavior to `LayerBrowseState`.

## 13) Adding A New Browse Filter

Use this workflow only for left-column catalog narrowing:

1. Add a field to `LayerBrowseState`.
2. Use it only in layer-list visibility/bulk layer-list actions.
3. Persist it separately from runtime filter state.

Do not let browse-only state affect rendering, hover, or query snapshots.
