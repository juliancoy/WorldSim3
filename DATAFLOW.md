# WorldSim3 Data Flow

This document describes how layer data moves from source files into runtime memory, persistent caches, derived records, and DuckDB analytics.

## Source Identity

Every source layer file under `data/layers/` is identified by `fileSignature(path)` from `cache_io.cpp`.

The signature is:

- source file size
- source file modification time

If the source file is missing, the signature is a `missing:<filename>` sentinel, so an old cache file is not accepted as a substitute for absent source data.

This signature is the invalidation key shared by hydration, triangulation, derived caches, and DuckDB analytics.

## Startup Layer Scheduling

On startup, `app_main_loop.cpp` enqueues every enabled layer for hydration.

That does not mean every source GeoJSON is reparsed. The hydration worker first checks `data/cache/hydration/<layer-file>.bin`, then falls back to `data/cache/hydration/<layer-file>.msgpack` for older caches. If a cache exists, matches the source signature, and passes sanity checks, the worker streams cached features into runtime memory. If not, it parses the source layer and writes a new hydration cache when the layer is cacheable.

Hydration always repopulates `layers[i].features` in RAM because render code, filters, hit testing, derived joins, and DuckDB rebuilds operate on runtime layer objects.

For `regional_parcels.geojson`, the import path is now allowed to materialize missing official Maryland parcel staging layers before the regional builder runs. The builder prefers the Maryland Planning parcel catalog for statewide county coverage, while keeping Baltimore County and Howard County on their better county-native feeds.

## Hydration Cache

Persistent path:

```text
data/cache/hydration/<layer-file>.bin
data/cache/hydration/<layer-file>.msgpack
```

The `.bin` cache is the primary format. It stores the hydrated CPU feature records in an explicit little-endian binary layout and avoids the large MsgPack object-graph decode cost on startup.

The `.msgpack` cache is a legacy fallback. If a matching MsgPack cache exists and the binary cache does not, the worker may load MsgPack once, then write the binary cache for later launches.

Writer:

```text
layer_workers.cpp -> saveBinaryHydrationCache()
```

Reader:

```text
layer_workers.cpp -> loadBinaryHydrationCache()
layer_workers.cpp -> loadHydrationCache() legacy fallback
```

A hydration cache stores:

- cache format version
- source signature
- feature extents
- geometry rings
- feature properties

Successful hydrations write cache files by default, including large layers such as `regional_parcels.geojson`. Set `WORLD_SIM3_DISABLE_LARGE_LAYER_CACHE=1` to suppress writes for layers over 300 MB when disk churn is more important than future startup speed.

Hydration cache writes are staged through a temporary file and then renamed into place. A crash during cache serialization should leave either the old cache or no accepted cache, not a partially written target file.

When a layer is rehydrated, the first hydration batch is marked as replacing existing runtime data. `layer_pipeline_drain.cpp` clears old `layers[i].features`, clears the spatial index, resets triangle provenance, and then appends the new batches. This prevents old and new source data from being mixed when a layer is downloaded or refreshed while the app is running.

Runtime state records:

- `hydration_source_signature`
- `hydration_loaded_from_cache`
- `hydration_phase`

`hydration_phase` distinguishes `loading_binary_cache`, `loading_msgpack_cache`, `binary_cache_hit_queueing`, `msgpack_cache_hit_queueing`, source parsing, stale cache, rejected cache, and completed cache hits. This prevents a large cache decode from looking identical to a GeoJSON source parse.

## Triangulation Cache

Persistent path:

```text
data/cache/triangulation/<layer-file>.tri.bin
data/cache/triangulation/<layer-file>.tri.json
```

The `.tri.bin` cache is the primary format. It stores per-feature triangle index vectors in an explicit binary layout and avoids loading a large JSON array tree on startup.

The `.tri.json` cache is a legacy fallback. If a matching JSON cache exists and the binary cache does not, the worker may load JSON once, then write the binary cache for later launches.

Writer:

```text
layer_workers.cpp -> saveBinaryTriCache()
```

Reader:

```text
layer_workers.cpp -> loadBinaryTriCache()
layer_workers.cpp -> loadTriCache() legacy fallback
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

The database stores `analytics_build_info.source_signature`, which is the combined signature of available source files. `DuckDbAnalytics::needsRebuild()` compares that stored signature to the current source signature instead of relying on database mtime. This avoids false freshness decisions when file timestamps move or a database is copied.

Auto-rebuild waits until:

- hydration requests are idle
- the hydrated queue is empty
- unified parcels exist

When a hydrated layer's source signature changes after DuckDB has already been checked, the hydration drain marks DuckDB auto-rebuild as unchecked. The right panel can then rebuild DuckDB after the new runtime data and derived records settle.

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

Seeing hydration progress on each startup is expected. It means runtime layer objects are being populated. If the hydration cache is valid, this path reads binary or legacy MsgPack cache files instead of reparsing GeoJSON.

DuckDB persists SQL-ready analytics tables, but it does not currently hydrate map-renderable layer geometry directly.
