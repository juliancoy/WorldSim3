# FILTER_FLOW

This document defines the runtime filter flow and the boundary between browse state and render state.

## 1) State Boundaries

`MapFilterState` is the single source of truth for generic runtime UI filters.

- Definition: [filters.h](/mnt/Cancer/worldsim3/filters.h:34)
- Owner instance: app runtime state / main-loop service context
- Runtime evaluator: [filters.cpp](/mnt/Cancer/worldsim3/filters.cpp:218)

`MapFilterState` contains generic runtime filters:

- `enabled`
- `use_date`, `year_min`, `year_max`
- `blocklot`, `status`, `address`, `owner`, `zip`
- `crime.*`
- `selected_owners`
- `event_sector_enabled`

It does not contain geography browse state or parcel-jurisdiction runtime state.

`ParcelJurisdictionFilterState` is separate runtime UI state for Maryland parcel-jurisdiction gating.

- Definition: [filters.h](/mnt/Cancer/worldsim3/filters.h:120)
- Frame refresh: [map_frame_session.cpp](/mnt/Cancer/worldsim3/map_frame_session.cpp:8)
- Frame wiring into `FeatureFilterContext`: [map_frame_session.cpp](/mnt/Cancer/worldsim3/map_frame_session.cpp:41)

It is not browse state. It affects rendered parcel visibility through a derived `FilterResultSet`.

`LayerDef.enabled` is the canonical layer on/off flag.

- Persisted layer UI state: [layer_state_io.cpp](/mnt/Cancer/worldsim3/layer_state_io.cpp:403)
- Render gate: [render_layer_pass.cpp](/mnt/Cancer/worldsim3/render_layer_pass.cpp:1151)
- Status API exposure: [status_api.cpp](/mnt/Cancer/worldsim3/status_api.cpp:927)

`LayerBrowseState` is separate UI state used only to decide which layers are shown in the left column.

- Definition: [filters.h](/mnt/Cancer/worldsim3/filters.h:29)
- Left-panel owner: [left_panel.cpp](/mnt/Cancer/worldsim3/left_panel.cpp:239)
- Browse matcher: [filters.cpp](/mnt/Cancer/worldsim3/filters.cpp:199)

Rules:

- If it changes generic rendered/runtime filtering behavior, it belongs in `MapFilterState`.
- If it is a specialized render-domain gate that compiles into a `FilterResultSet`, it may live outside `MapFilterState`, but it must be documented and wired through `FeatureFilterContext`.
- If it turns an entire layer on or off, it belongs in layer enablement (`LayerDef.enabled`).
- If it only narrows the layer browser UI, it belongs in `LayerBrowseState`.

## 2) Runtime Inputs

Runtime map behavior is driven by:

1. Enabled layers
2. Per-layer settings
3. `MapFilterState`
4. Parcel-jurisdiction runtime state compiled into `FilterResultSet`
5. Optional `FilterResultSet` / `QueryMapLayer` outputs

Runtime map behavior is not driven by left-panel geography browse state.

## 3) Evaluation Context

`FeatureFilterContext` is read-only frame context, not persistent state.

It contains:

- `map_filters`
- layer/dataset references used for joins
- result-set gates, including parcel-jurisdiction and text-query gates
- query overlays
- parcel-related cached lookup tables

Builder:

- [filter_context_builder.cpp](/mnt/Cancer/worldsim3/filter_context_builder.cpp:5)

## 4) Visibility Predicate

All feature visibility filtering flows through:

- `featurePassesFilters(...)` in [filters.cpp](/mnt/Cancer/worldsim3/filters.cpp:218)

All layer draw attempts still pass through the top-level layer enablement gate first:

- `if (!l.enabled) continue;` in [render_layer_pass.cpp](/mnt/Cancer/worldsim3/render_layer_pass.cpp:1153)

High-level order:

1. Active `FilterResultSet` gates
2. Crime-layer branch, if applicable
3. Parcel-domain owner-selection gate
4. General field/date filters from `MapFilterState`
5. Layer-specific render gates outside generic filtering

Implication:

- `layer.enabled` is necessary but not sufficient for visible output.
- An enabled layer may still draw nothing if hydration is incomplete, if all features fail runtime/result-set filters, or if an alternate draw path suppresses raw-source rendering.

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

- parcel-jurisdiction selection compiled into `FilterResultSet`
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

Generic runtime filter persistence should read/write `MapFilterState`.

- Load filter state: [layer_state_io.cpp](/mnt/Cancer/worldsim3/layer_state_io.cpp:610)
- Save filter state: [layer_state_io.cpp](/mnt/Cancer/worldsim3/layer_state_io.cpp:698)

Layer enablement is persisted separately from `MapFilterState` as layer UI state.

- Load layer UI state: [layer_state_io.cpp](/mnt/Cancer/worldsim3/layer_state_io.cpp:368)
- Save layer UI state: [layer_state_io.cpp](/mnt/Cancer/worldsim3/layer_state_io.cpp:523)

Current limitation:

- parcel-jurisdiction runtime state is not persisted alongside `MapFilterState`
- if persistence is added later, document it explicitly as separate runtime filter state

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

Exception:

- If the filter is a specialized UI workflow that compiles to `FilterResultSet` and does not fit cleanly in `MapFilterState`, keep it as separate runtime state, document it here, and wire it through `FeatureFilterContext`.

## 13) Adding A New Browse Filter

Use this workflow only for left-column catalog narrowing:

1. Add a field to `LayerBrowseState`.
2. Use it only in layer-list visibility/bulk layer-list actions.
3. Persist it separately from runtime filter state.

Do not let browse-only state affect rendering, hover, or query snapshots.
