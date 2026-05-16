# WorldSim3 Data Flow

This document describes how layer data moves from source files into runtime memory, persistent caches, derived records, and DuckDB analytics.

## Source Identity

Every source layer file under `data/layers/` is identified by `fileSignature(path)` from `cache_io.cpp`.

The signature is:

- source file size
- source file modification time

If the source file is missing, the signature is a `missing:<filename>` sentinel, so an old cache file is not accepted as a substitute for absent source data.

This signature is the invalidation key shared by hydration, triangulation, derived caches, and DuckDB analytics.

## Disk-Persisted Artifact Overview

WorldSim3 persists four broad classes of disk artifacts:

- Authoritative or source-adjacent data under `data/layers/`, `data/imports/`, and `data/inbox/`.
- Rebuildable runtime caches under `data/cache/`.
- Runtime/user state JSON files under `data/`.
- Analytics exports and SQL cache files under `data/analytics/` and `data/worldsim.duckdb`.

The startup rule is intentionally simple:

- Geometry startup reads only geometry-oriented artifacts.
- DuckDB is for explicit query/search/detail workflows, not for render readiness.
- Expensive analytics rebuilds are user-initiated, not automatic frame-loop work.

The main artifacts are:

| Path | Kind | Producer | Consumer | Notes |
| --- | --- | --- | --- | --- |
| `data/layers/*.geojson` | Source/interchange layer data | Download queue, import tools, builders, scripts | Hydration workers, layer registry, derived builders | The normal source of truth for most map layers. File size and mtime form the source signature. |
| `data/layers/*.geojson.part` | In-progress layer download | Layer download queue | Download finalization only | Temporary artifact; should not be accepted as a layer source. |
| `data/imports/*.source.*` | Raw imported/downloaded upstream payloads | Dataset download/import tools | Builders and audit/debug workflows | Preserves upstream ZIP/CSV payloads used to generate normalized GeoJSON layers. |
| `data/inbox/**` | Manual drop-zone inputs | User or external process | Builder scripts/tools | Used for datasets that are copied in manually, such as HUD PIT files. |
| `data/layers/regional_parcels.geojson.canonical.bin` | Canonical parcel binary source companion | Regional parcel builder | Hydration workers, headless hydration, validation CLI | Signature-bound compact parcel source. Lets statewide parcels hydrate without reparsing the 7 GB GeoJSON when the binary hydration cache is missing. |
| `data/cache/hydration/<layer-file>.bin` | Primary hydrated CPU feature cache | Hydration workers, warm CLI | Hydration workers, parcel render cache warmer | Rebuildable cache of `LayerDef::FeatureGeom` records. Startup still streams this into RAM because render filters, hit testing, derived joins, DuckDB, and spatial indexing use CPU layer objects. |
| `data/cache/hydration/*.tmp.*` | In-progress hydration cache write | Hydration cache writer | None after successful rename | Temporary artifact. A completed write atomically renames to `.bin`; leftover temp files can be deleted when no writer is running. |
| `data/cache/triangulation/<layer-file>.tri.bin` | Primary triangulation cache | Triangulation worker, warm/render CLI | Triangulation worker, parcel render sidecar builder | Stores per-feature triangle index vectors keyed by source signature and feature count. |
| `data/cache/render/<layer-file>.parcel-render.bin` | Retained parcel render sidecar | Parcel render worker, warm CLI | Vulkan parcel upload path | Stores render-shaped parcel positions, fill/line indices, vertex-to-feature refs, feature spans, and chunk bounds. Does not store property bags. |
| `data/cache/aggregate/<hex-key>.raster.bin` | Heatmap aggregate raster cache | Heatmap runtime | Heatmap runtime | Rebuildable raster cache keyed by heatmap/view/filter settings. Runtime texture objects are separate in-memory state. |
| `data/cache/derived/parcel_vacancy_status.json` | Derived parcel status cache | Derived cache refresh | Derived cache refresh and parcel styling | Stores derived parcel vacancy status records. Invalidated by propagated source signatures/generations. |
| `data/cache/screenshots/*` | User screenshots | Screenshot capture path | User/debug workflows | Output artifacts, not inputs to the layer pipeline. |
| `data/worldsim.duckdb` | DuckDB analytics cache | Explicit DuckDB analytics rebuild | Query/search/right-panel analytics | SQL-ready analytics cache built from hydrated runtime layers and unified parcel records. It is not a geometry hydration or render cache and is not rebuilt automatically during startup. |
| `data/analytics/*` | Offline analytics exports | Scripts | User/audit workflows | Example: vacancy timeseries CSV/QA JSON. Not used for normal startup hydration. |
| `data/tiles/<z>/<x>/<y>.png` | OSM raster tile cache | Basemap lazy downloader or preseeded data | Basemap renderer | On-disk basemap PNGs. Disk presence is memoized in memory and can be cleared without deleting PNGs. |
| `data/tiles_topo*/<z>/<x>/<y>.png` | Topographic raster tile cache | Basemap lazy downloader or preseeded data | Basemap renderer | Topographic raster tiles. `data/tiles_topographic` may be used as an alternate/preferred topo source when present. |
| `data/tiles_satellite/<z>/<x>/<y>.png` | Satellite raster tile cache | Basemap lazy downloader or preseeded data | Basemap renderer | Satellite raster tiles, fetched lazily up to the configured native zoom. The dark satellite basemap is a render-time transform over this same cache and does not create a separate tile artifact. |
| `data/tiles_satellite_night/<z>/<x>/<y>.png` | Night satellite raster tile cache | Basemap lazy downloader or preseeded data | Basemap renderer | Night-lights basemap tiles, fetched lazily from the Earth-at-Night source up to its native z7 and displayed only as low/mid-zoom context. High zoom uses dark satellite detail instead of overzooming pixelated night lights. |
| `data/tiles_topo_vector.geojson` | Topographic vector layer | Topo download/build path | Basemap renderer | Optional vector contour/topographic representation. |
| `data/lazy_tile_queue.json` | Persisted lazy tile request queue | Basemap tile queue | Basemap tile queue | Keeps pending visible tile downloads across runs. |
| `data/download_queue.json` | Basemap/data-library download queue | Download queue | Download queue | Persists queued generic downloads. |
| `data/layer_download_queue.json` | Layer download queue | Layer download queue | Layer download queue | Persists queued layer refresh/download work by layer file. |
| `data/layer_ui_state.json` | Layer UI/filter/render state | Layer UI state sync | Startup state load, layer UI | Persists enabled flags, colors, opacity, heatmap settings, filters, and related layer controls. |
| `data/app_settings.json` | App-level settings | Settings UI and shutdown | Startup settings load | Persists Vulkan validation setting, CPU core reservation, basemap toggles/opacities, zoning color mode, and similar app controls. |

Files under `data/cache/` are rebuildable and should be treated as performance artifacts. Files under `data/layers/`, `data/imports/`, and `data/inbox/` are source or source-adjacent artifacts and should not be cleared as routine cache cleanup unless the intended result is to force reimport or redownload.

## Startup Layer Scheduling

On startup, `app_main_loop.cpp` enqueues every enabled layer for hydration.

That does not mean every source GeoJSON is reparsed. The hydration worker first checks `data/cache/hydration/<layer-file>.bin`. For `regional_parcels.geojson`, it can also load `data/layers/regional_parcels.geojson.canonical.bin`, which is a compact canonical parcel binary companion emitted by the regional parcel builder. If one of these binary sources exists, matches the source signature, and passes sanity checks, the worker streams structured features into runtime memory. If not, it parses the source layer and writes a new hydration cache when the layer is cacheable.

Hydration always repopulates `layers[i].features` in RAM because filters, hit testing, derived joins, spatial indexing, and optional analytics rebuilds operate on runtime layer objects.

For large parcel layers, hydration alone does not mean "ready to draw." Runtime readiness is split into:

- hydration: CPU feature records and bounds are available
- spatial index: viewport queries and hit-test acceleration are available
- triangulation apply: fill triangle vectors have been copied from cache/build results into runtime records
- render sidecar/GPU upload: retained parcel buffers are available for the Vulkan path

The frame loop should not compensate for a missing large parcel render sidecar by scanning millions of CPU parcel features. Statewide parcels are either drawn through the retained GPU parcel path or skipped until that path is available.

For `regional_parcels.geojson`, the import path is now allowed to materialize missing official Maryland parcel staging layers before the regional builder runs. The builder prefers the Maryland Planning parcel catalog for statewide county coverage, while keeping Baltimore County and Howard County on their better county-native feeds. The builder now writes both the canonical GeoJSON layer and a signature-bound compact binary companion in the same pass.

## Hydration Cache

Persistent path:

```text
data/cache/hydration/<layer-file>.bin
```

The `.bin` cache stores the hydrated CPU feature records in an explicit little-endian binary layout.

Writer:

```text
layer_workers.cpp -> saveBinaryHydrationCache()
```

Reader:

```text
layer_workers.cpp -> loadBinaryHydrationCache()
```

A hydration cache stores:

- cache format version
- source signature
- feature extents
- geometry rings
- feature properties

For `regional_parcels.geojson.bin`, the writer keeps only runtime-critical parcel identity/join fields such as jurisdiction, source file, parcel IDs, blocklot keys, and canonical owner/address/value summaries. It intentionally drops the full raw county property bag from the hydration cache. Full parcel/property detail belongs in DuckDB and derived unified parcel records; the hydration cache remains the CPU geometry and hit-test source.

Successful hydrations write cache files by default, including large layers such as `regional_parcels.geojson`. Set `WORLD_SIM3_DISABLE_LARGE_LAYER_CACHE=1` to suppress writes for layers over 300 MB when disk churn is more important than future startup speed.

Hydration cache writes are staged through a temporary file and then renamed into place. A crash during cache serialization should leave either the old cache or no accepted cache, not a partially written target file.

When a layer is rehydrated, the first hydration batch is marked as replacing existing runtime data. `layer_pipeline_drain.cpp` clears old `layers[i].features`, clears the spatial index, resets triangle provenance, and then appends the new batches. This prevents old and new source data from being mixed when a layer is downloaded or refreshed while the app is running.

Runtime state records:

- `hydration_source_signature`
- `hydration_loaded_from_cache`
- `hydration_phase`

`hydration_phase` distinguishes `loading_binary_cache`, `loading_canonical_binary_source`, `binary_cache_hit_queueing`, `canonical_binary_queueing`, source parsing, stale cache, rejected cache, and completed cache hits. This prevents a large cache decode from looking identical to a GeoJSON source parse.

## Triangulation Cache

Persistent path:

```text
data/cache/triangulation/<layer-file>.tri.bin
```

The `.tri.bin` cache is the primary format. It stores per-feature triangle index vectors in an explicit binary layout and avoids loading a large JSON array tree on startup.

Writer:

```text
layer_workers.cpp -> saveBinaryTriCache()
```

Reader:

```text
layer_workers.cpp -> loadBinaryTriCache()
```

Triangulation jobs inherit the hydration source signature from the hydrated layer. The triangulation cache is accepted only when that signature and the feature count match. Results carry the same signature back into runtime state as `triangulation_source_signature`.

Runtime state records:

- `triangulation_phase`
- `triangulation_loaded_from_cache`

This keeps fill geometry coupled to the exact source geometry that produced the hydrated features.

## Derived Runtime Caches

Derived caches are rebuilt in `derived_layer_caches.cpp`.

The key derived data includes:

- harmonized real-property records
- vacant notice counts by blocklot
- vacant rehab counts by blocklot
- tax lien counts and amounts by blocklot
- tax sale counts and amounts by blocklot
- parcel-level vacancy/tax arrays
- unified parcel records

Invalidation uses propagated hydration source signatures, not only feature counts. This matters when a source file changes but keeps the same number of rows.

The unified parcel cache is invalidated when any of these change:

- parcel layer source signature
- parcel feature count
- harmonized real-property size
- parcel vacancy generation
- parcel tax generation

The derived disk artifact currently written by this path is:

```text
data/cache/derived/parcel_vacancy_status.json
```

## DuckDB Analytics Cache

Persistent path:

```text
data/worldsim.duckdb
```

Writer:

```text
duckdb_analytics.cpp -> DuckDbAnalytics::rebuild()
```

Reader:

```text
duckdb_analytics.cpp -> DuckDbAnalytics::executeMapQuery()
```

DuckDB is an analytics cache, not the render hydration cache. It is built from already hydrated runtime layers plus unified parcel records.

DuckDB stores extracted feature attributes in `layer_features` and parcel-level property/detail fields in `unified_parcels`. This is the intended home for searchable owner/address/value/detail data. It is not the source used to hydrate CPU geometry at startup.

The database stores `analytics_build_info.source_signature`, which is the combined signature of available source files. `DuckDbAnalytics::needsRebuild()` compares that stored signature to the current source signature instead of relying on database mtime. This avoids false freshness decisions when file timestamps move or a database is copied.

DuckDB rebuild is intentionally explicit. The SQL tab exposes a rebuild button, and command-line/offline tools may also rebuild it. The frame loop does not automatically rebuild `data/worldsim.duckdb`, because a full rebuild can write multiple gigabytes and can stall the UI while parcels are still becoming render-ready.

If DuckDB is missing or stale:

- map rendering and parcel hydration still proceed from geometry caches
- DuckDB-backed search/detail/query features report that the cache is unavailable or stale
- the user can rebuild the analytics cache when interactive startup is no longer on the critical path

## Clear Cache Behavior

The Performance panel exposes separate clear scopes:

- hydration disk cache
- triangulation disk cache
- derived disk cache
- heatmap aggregate disk cache
- heatmap runtime cache
- tile runtime cache
- tile disk-presence memoization

Clearing hydration data also clears runtime layer features, spatial indexes, queued hydration/triangulation work, runtime provenance signatures, and derived cache state, then re-enqueues enabled layers.

Clearing only triangulation keeps hydrated features and schedules fresh triangulation using the hydrated layer source signature.

## Important Distinction

Seeing hydration progress on each startup is expected. It means runtime layer objects are being populated. If the hydration cache is valid, this path reads binary cache files instead of reparsing GeoJSON.

Seeing `triangulating` after a triangulation cache hit is also expected for large parcel layers. In that phase the binary cache has been accepted, but cached triangle vectors are still being applied into runtime feature records in bounded batches.

DuckDB persists SQL-ready analytics tables, but it does not hydrate map-renderable layer geometry directly and should not run on the startup critical path.

## Parcel Render Sidecar Cache

Parcel polygon geometry should not use DuckDB as its primary render storage target. The correct target is a dedicated binary sidecar shaped for retained rendering resources.

Current binary sidecar support:

```text
data/cache/render/<layer-file>.parcel-render.bin
```

The sidecar stores:

- source signature
- contiguous world-space vertex stream
- contiguous vertex-to-render-feature reference stream
- contiguous triangle index stream
- contiguous line index stream for parcel ring edges
- per-feature offset/count records
- per-chunk offset/count and bounds records

It deliberately does not store the full parcel property bags. The sidecar follows a KISS rule: geometry and the smallest lookup metadata needed for rendering stay in the render cache; parcel attributes remain in runtime layer records and DuckDB analytics tables.

Sidecar production is now asynchronous at runtime:

- the frame loop requests a sidecar by `layer_file + source_signature`
- a background parcel render worker tries to load `data/cache/render/<layer-file>.parcel-render.bin`
- on a miss, that worker rebuilds the sidecar from persisted hydration and triangulation caches instead of touching live frame-owned layer state
- the main thread consumes only completed sidecar blobs

The render sidecar is the artifact that should make statewide parcel drawing fast. Until it is loaded and uploaded, the renderer avoids a full CPU fallback scan for statewide parcels. This keeps the UI responsive while the sidecar/GPU path catches up.

GPU upload is now asynchronous too:

- the main thread submits a completed `ParcelRenderCacheBlob` to the parcel GPU upload worker
- the upload worker owns a private Vulkan command pool and command buffer
- that worker builds a fully uploaded parcel GPU payload off the frame-loop orchestration path
- the main thread adopts only completed payloads, keyed by source signature, and discards stale upload results
- previously live parcel GPU payloads are retired against a presented-frame serial and drained later instead of being destroyed immediately on adoption

Parcel geometry residency is now session-static:

- the first successful parcel GPU upload locks the parcel geometry source signature for the rest of the process
- later source-signature changes do not trigger in-process parcel geometry replacement
- instead, the runtime keeps the existing long-lived parcel geometry buffers and logs that restart is required for geometry refresh
- only the per-parcel RGBA buffers remain mutable during steady-state runtime

At runtime, Vulkan uploads the sidecar into separate GPU buffers:

- parcel positions
- parcel triangle indices
- parcel line indices
- vertex-to-parcel-slot references
- parcel RGBA colors
- parcel overlay RGBA colors
- parcel outline RGBA colors

The parcel RGBA buffers are mutable. DuckDB/query-driven filter state writes base parcel colors into one buffer and parcel overlay fills into a second buffer without rebuilding parcel geometry.

When the map viewport is active, runtime also derives a per-frame parcel draw state from:

- current map viewport origin and size
- current `math_zoom`, `zoom_scale`, and `center_world`
- current visible lon/lat bounds

That state is used to:

- cull parcel render chunks on CPU by chunk bounds
- bind the retained parcel GPU buffers
- issue indexed parcel base-fill draws through a dedicated Vulkan parcel pipeline inserted into the map draw stream
- issue indexed parcel overlay-fill draws from the render tail stage on the same retained geometry
- issue indexed parcel outline draws from the render tail stage using explicit line topology

Current boundary:

- parcel base fills use the dedicated Vulkan pipeline
- parcel overlay fills use the dedicated Vulkan pipeline with a second color buffer
- parcel outlines and selected-parcel emphasis use the dedicated Vulkan line path with a third color buffer
- parcel visibility filtering is represented by RGBA updates, with transparent parcels discarded in the fragment stage

This cache is intended to become the source for asynchronous GPU upload. It is not a literal disk-to-GPU zero-copy draw buffer. The file remains a durable cache artifact, while Vulkan buffers remain runtime-owned GPU resources.
