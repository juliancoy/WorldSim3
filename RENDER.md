# Rendering

This document describes how map rendering is organized and what each layer type is expected to provide. It is intended as a contract for future render changes, especially around filters, GPU buffers, and choropleth color generation.

## Frame Pipeline

The main map path is:

1. `drawMapTabWindow(...)` builds a `MapFrameSessionContext`.
2. `runMapFrameSession(...)` refreshes filter state and builds a `RenderFrameOrchestrationContext`.
3. `orchestrateMapFrameRender(...)` resolves display policy, heatmap cache keys, render plan, layer pass, heatmap pass, and tail overlays.
4. `runRenderLayerPass(...)` draws normal per-feature layers or queues GPU-backed primary draws.
5. `runHeatmapFramePass(...)` draws aggregate heatmap output.
6. `runRenderTailPass(...)` draws parcel source overlays and late overlays.

Rendering should consume prepared state. It should not perform expensive data discovery, file scanning, or full filter evaluation in the frame loop.

## Startup Readiness

Rendering assumes compiled geometry artifacts and DuckDB semantics are already current. Startup may inspect artifact readiness, but interactive launch must not run preprocessing before the main map UI and status API are alive.

The main renderer does not perform preprocessing, rebuild missing geometry, or silently fall back to CPU feature records. Missing or stale artifacts are prepared explicitly with `worldsim3 --startup-preprocess` or `worldsim3 --build-geometry-duckdb-artifacts`.

## Filter And Query Color Contract

Filter evaluation produces `LayerFeatureRenderCache`:

- `FeatureRenderState::visible` says whether the feature should participate in rendering.
- `FeatureRenderState::has_query_color` and `query_color` carry SQL/query-map overlay color.
- `LayerFeatureRenderCache::state_key` is derived from map filters, active result sets, and query layers.

The cache is built by `ensureLayerFeatureRenderCache(...)` from `FeatureFilterContext`. The render path should use `findFeatureRenderState(...)` or the callbacks wired through `MapFrameSessionContext`, not call `featurePassesFilters(...)` or `queryMapColorForFeature(...)` directly from draw loops.

The key rule is:

Filters run when filter/query inputs change. Frames draw cached visibility and cached RGBA.

## Display Policy

`resolveLayerDisplayPolicy(...)` maps a layer to one of these modes:

- `PerFeature`: normal feature rendering.
- `Aggregate`: heatmap or aggregate texture rendering.
- `LodGeometry`: GPU/LOD geometry rendering.
- `PointCluster`: clustered point rendering.
- `ParcelChoroplethDetail`: parcel-scale continuous field rendering at detail zoom.

A layer with `scale == "parcel"` and a non-empty `heatmap_field` is a value parcel layer. If aggregate rendering is configured, the policy uses aggregate mode up to `aggregate_max_zoom`, then switches to detailed parcel choropleth at the effective parcel detail zoom.

## Layer Types

### Point Layers

Point layers are identified by `layerUsesPointGeometry(layer)`.

Preferred path:

- Load or validate a `PointGeometryArtifact`.
- Upload positions/features to point GPU buffers.
- Upload one color per feature from `LayerFeatureRenderCache`.
- Upload glyph codes separately.
- Configure point GPU draw state each frame from the current viewport.

Fallback path:

- CPU render uses spatial candidates when available.
- Features are filtered through cached `feature_passes_filters`.
- Point glyph classification should be cached per layer where possible.

Point colors follow this priority:

1. Transparent if cached visibility is false.
2. Query-map color if `has_query_color`.
3. Layer base color.

Crime point layers are a specialized point path with their own GPU buffer set and crime glyph logic.

### Polyline Layers

Polyline layers are identified by `layerUsesPolylineGeometry(layer)`.

Preferred path:

- Load or validate a `PolylineGeometryArtifact`.
- Upload geometry to polyline GPU buffers.
- Upload one color per feature from `LayerFeatureRenderCache`.
- Configure polyline GPU draw state from the current viewport.

Fallback path:

- There is no frame-loop CPU polyline geometry fallback for normal map rendering.
- If the compiled artifact or GPU buffers are missing, the layer is non-drawable until geometry is compiled and uploaded.
- Debug/inspection UI may draw small ad hoc line snippets, but that is outside the primary layer renderer.

### Polygon Layers

General polygon layers render as filled and/or outlined features from `PolygonGeometryArtifact` uploaded to GPU buffers. Frame rendering must not tessellate rings, project rings, or draw polygon outlines through ImGui. If a polygon artifact or its GPU buffers are missing, the layer is non-drawable until the geometry pipeline produces the artifact and the runtime uploads it.

Color priority:

1. Choropleth/heat color if the layer has a valid `heatmap_field` and gradient rendering is enabled.
2. Zoning zone color for zoning polygon layers.
3. Query-map color from `LayerFeatureRenderCache`.
4. Layer base color.

Polygon fill must respect `layer_fill_enabled` and `map_polygon_fill_opacity` where applicable. Tessellated fill indices are part of the compiled geometry artifact; they are not generated in the frame loop.

Polygon outlines use the compiled line-index artifact. The renderer keeps per-feature bounds and line ranges in GPU storage buffers alongside the vertex/index buffers, uploads one outline RGBA value per feature when filter/query/style state changes, and dispatches compute before the render pass to populate indirect draw commands for the current viewport. Camera movement updates push constants and dispatches GPU command generation; it must not rebuild geometry, re-run filters, or construct polygon draw ranges on the CPU.

### Parcel Layers

The main parcel layer has a dedicated GPU path using `ParcelRenderCacheBlob`.

GPU resident data:

- Parcel vertices and indices.
- Base color buffer.
- Overlay color buffer.
- Outline color buffer.

Base parcel colors are rebuilt only when their color/filter key changes. The base color buffer uses cached visibility and cached query color, plus value choropleth output when a parcel parameter mode requires it.

Overlay colors are separate from base colors. Vacancy, tax, area, value, and selected-parcel overlays write to the overlay buffer. Outlines are computed from base/overlay/selection state and uploaded separately.

There is no normal frame-loop CPU parcel geometry fallback. Parcel overlays use GPU color buffers over the resident parcel geometry. If GPU parcel geometry is unavailable, parcel geometry remains non-drawable until the resident buffers are restored.

### Zoning Layers

Zoning layers are polygon layers with category `Zoning` or zoning-like names/files. Zoning color comes from `zoning_zone_color` keyed by `zoningClassKey(feature)`.

The zoning GPU path stores base fill and outline colors per feature. Zone enabled/disabled state and zone colors are part of the color state key. Disabled zones should produce transparent feature colors rather than being filtered inside draw loops.

The outline path should submit GPU-generated indirect line draws. Each command slot references a feature's resident line-index range; transparent or out-of-view features are written with zero instances by compute.

### Raw Source Layers

Some layers act as raw sources for derived overlays, such as vacancy notice, vacancy rehab, tax lien, and tax sale inputs. `RenderPlan` decides when these source layers should be skipped as standalone map layers because their data is being rendered through parcel overlays.

Raw source layers should not be drawn twice: once as raw features and once as parcel-derived overlays.

### Aggregate Layers

Aggregate layers use `HeatSample` generation and `runHeatmapFramePass(...)`.

Aggregate cache keys include:

- Layer and query-layer state.
- Filter state.
- Zoom/view state when the aggregate is view-dependent.
- Heatmap algorithm and quality settings.
- Per-layer normalization, gamma, percentile clipping, and gradient settings.

High-quality GPU aggregate modes should reuse cached aggregate textures while panning whenever the data key is unchanged.

### LOD Geometry

LOD geometry layers use precomputed geometry artifacts and a GPU draw path. They must not rebuild or redraw retained line/polygon geometry from CPU feature records during normal rendering.

## Choropleth Contract

The codebase spells this as choropleth. It means per-feature color is derived from a numeric value and mapped through a color ramp.

A layer participates in choropleth rendering when:

- `LayerDef::heatmap_field` is non-empty.
- The feature has a numeric property with that exact key.
- The layer display policy resolves to per-feature/detail rendering, or an aggregate mode that consumes heat samples.

The field contract:

- `heatmap_field` names a feature property, not a display label.
- The property value must parse as a finite float.
- Missing, non-numeric, or non-finite values do not produce a choropleth color.
- Filters apply before normalization. Hidden features must not affect min/max, percentile rank, equal-count zones, or aggregate samples.

Normalization is controlled by `layer_normalize_mode`:

- `0`: linear min/max over visible numeric values.
- `1`: percentile rank over visible numeric values.
- `2`: grouped percentile rank, falling back to global percentile when the group has too few samples.
- `3`: equal-count zones derived from percentile rank.

`layer_heatmap_percentile_clip` limits the high end of linear normalization. `layer_choropleth_gamma` applies `pow(t, gamma)` after normalization. `heatColor(t)` maps normalized `t` to the blue-yellow-red ramp.

Polygon and parcel fill opacity is applied after color selection. Query-map colors should override base layer color, but continuous choropleth color takes priority when the feature has a valid choropleth value in the active choropleth mode.

## GPU Residency Guidelines

Keep stable geometry on the GPU when it has a reusable artifact:

- Parcel triangles, outlines, and feature records.
- Point positions and feature records.
- Polyline vertex/index data.
- Polygon fill and outline artifacts.
- Per-feature color/glyph buffers that change only when filter/query/style keys change.
- Per-frame indirect draw-command buffers generated on the GPU from cached feature visibility/color and viewport bounds.

Do not move short-lived control state to the GPU. Filter evaluation, SQL result-set matching, owner/address text matching, and property normalization are CPU responsibilities. Their output should be compact GPU-friendly buffers: visibility as transparent colors, per-feature RGBA, glyph IDs, and aggregate textures. Camera movement updates draw-state uniforms and GPU command-generation inputs; it must not reproject retained geometry on the CPU.

## Cache Invalidation Rules

Recompute `LayerFeatureRenderCache` when:

- `MapFilterState` changes.
- Active filter result sets change.
- Query layers are added, removed, recolored, enabled, disabled, or receive new results.
- Layer feature counts change.

Reupload GPU color buffers when:

- The feature render cache key changes.
- Layer base color or opacity changes.
- Choropleth mode, normalization mode, percentile clip, or gamma changes.
- Overlay sources or selected parcels change.
- Zone colors or zone enabled state changes.

Reupload geometry buffers when:

- The layer hydration source signature changes.
- The geometry artifact is missing, invalid, or no longer matches the feature count.
- The GPU buffer for that layer has been cleared or lost.

Do not rebuild geometry, filters, or color buffers just because the camera panned. Camera changes should update draw state and projection, not data state.

## Implementation Boundaries

`filters.*` owns filter/query result evaluation and cached render visibility/query color.

`render_policy.*` owns display-mode selection.

`map_frame_session.*` wires persistent app state into render callbacks.

`map_frame_render.*` orchestrates the frame.

`render_layer_pass.*` handles primary layer drawing and sample collection.

`render_tail_pass.*` and `map_render_overlays.*` handle late parcel overlays.

`app_main_loop.cpp` currently owns several persistent GPU color/geometry buffers and their state keys. New render work should prefer moving reusable state behind smaller services when that reduces coupling, but it must preserve the same invalidation contracts.
