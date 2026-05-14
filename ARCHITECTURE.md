# WorldSim3 Target Architecture

This document describes the professional target architecture for keeping the app responsive while large geospatial data, cache hydration, derived joins, aggregation, and rendering continue in the background.

The current application already has worker threads for hydration, triangulation, status APIs, dataset APIs, and some async heatmap work. The remaining problem is that expensive work still propagates into the UI/render frame. The target is to make the frame loop consume completed snapshots, never perform large rebuilds directly.

## Goals

- Keep UI input responsive during hydration, triangulation, aggregation, and DuckDB rebuilds.
- Keep rendering stable by drawing the latest complete render snapshot.
- Move expensive data mutation and rebuild work off the UI/render frame.
- Make cache, layer, derived, and render state transitions explicit and observable.
- Preserve correctness: no partial layer state, mixed source signatures, or stale derived records.

## Non-Goals

- Rewriting all rendering at once.
- Removing existing worker code.
- Replacing DuckDB as the analytics cache in the first step.
- Making every operation fully real-time. Some jobs can remain slow as long as they do not block the frame.

## Thread Model

### UI Thread

Owns:

- ImGui input and panels
- user settings edits
- current interaction state
- lightweight status/progress display

Must not do:

- GeoJSON parsing
- hydration cache serialization/deserialization
- triangulation
- spatial index rebuilds for large layers
- owner aggregate rebuilds
- DuckDB rebuilds
- full-layer filtering over hundreds of thousands of features

### Render Thread

Owns:

- drawing the current frame
- consuming immutable render snapshots
- issuing draw commands
- presenting GPU resources already made available to it

Must not block on:

- hydration completion
- aggregate generation
- DuckDB
- cache writes
- large CPU-side geometry rebuilds

The render thread should draw the latest complete `RenderSnapshot`. If a newer snapshot is not ready, it should keep drawing the previous one.

### Data Workers

Own:

- layer hydration
- hydration cache reads/writes
- triangulation
- spatial index construction
- derived vacancy/tax/unified parcel joins
- owner aggregate rebuilds
- DuckDB analytics rebuilds

Workers publish completed immutable outputs through explicit queues or snapshot swaps.

### GPU/Aggregate Workers

Own:

- heatmap sample aggregation
- GPU aggregate texture generation
- CPU fallback aggregate generation
- aggregate cache writes
- texture staging work where possible

The render path should consume completed aggregate textures. It should not wait for an aggregate texture to be generated.

## Snapshot Boundary

The core boundary should be an immutable snapshot:

```cpp
struct RenderSnapshot {
    uint64_t generation = 0;
    std::vector<LayerRenderSnapshot> layers;
    DerivedOverlaySnapshot overlays;
    OwnerAggregateSnapshot owners;
    HeatmapTextureSnapshot heatmaps;
    SelectionSnapshot selection;
};
```

The exact fields can evolve, but the contract should stay stable:

- snapshots are immutable after publication
- publication is atomic from the render thread's point of view
- the render thread never observes half-applied hydration or derived data
- each snapshot carries source signatures/generations for provenance

Recommended ownership:

```cpp
std::atomic<std::shared_ptr<const RenderSnapshot>> current_render_snapshot;
```

Workers build a new snapshot or snapshot delta off-thread, then atomically publish it. The render/UI frame reads the pointer once at frame start and uses that stable view for the whole frame.

## Data Flow

Target flow:

```text
source files
  -> hydration worker
  -> hydration cache
  -> hydrated layer records
  -> triangulation worker
  -> triangulation cache
  -> spatial index worker
  -> derived data workers
  -> render snapshot builder
  -> atomic snapshot publish
  -> render thread
```

DuckDB flow:

```text
hydrated layers + unified parcels
  -> DuckDB rebuild worker
  -> data/worldsim.duckdb
  -> SQL/query UI
```

DuckDB should remain analytics-oriented unless a separate decision is made to use it as a renderable geometry store.

## Frame Budget

The frame loop should have explicit budgets:

- queue draining: bounded by time and item count
- status copying: bounded and shallow
- rendering: only visible candidates from existing snapshots
- UI panels: virtualized lists for large tables

Suggested starting targets:

- UI input latency: under 50 ms during background work
- typical frame under load: under 100 ms
- idle frame: under 16-33 ms depending on VSync/display target
- no single UI/render frame should process an entire large layer

## Job System

Introduce a small job abstraction before a large framework:

```cpp
struct JobToken {
    uint64_t generation = 0;
    std::atomic<bool>* cancelled = nullptr;
};

struct JobResult {
    uint64_t generation = 0;
    bool ok = false;
    std::string error;
};
```

Required behavior:

- Jobs carry the generation/signature they were built from.
- Newer settings or source changes cancel obsolete jobs.
- Results are discarded if their generation is stale.
- Long-running jobs periodically check cancellation.
- Rebuilds are coalesced so repeated UI changes do not queue redundant work.

## Dirty Flags and Generations

Use explicit generations instead of scattered boolean dirtiness:

- `layer_source_generation`
- `hydration_generation`
- `triangulation_generation`
- `spatial_index_generation`
- `derived_generation`
- `filter_generation`
- `render_snapshot_generation`
- `duckdb_generation`

Each derived artifact records the generations it consumed. An artifact is valid only when its consumed generations match current upstream generations.

## Rendering Policy

Rendering should decide from snapshot metadata:

- draw per-feature geometry
- draw LOD geometry
- draw aggregate texture
- draw stale aggregate texture while rebuild is pending
- draw loading/progress overlay only when useful

Important rule:

If aggregate method is `None`, the layer should continue using per-feature rendering. It must not silently auto-enable aggregate rendering because the layer is large.

## Rendering Architecture

Parcel-level rendering and aggregate-level rendering should be treated as separate pipelines. They have different correctness requirements, cache boundaries, GPU resource shapes, and interaction behavior.

### Parcel-Level Rendering

Current state:

- Parcel geometry is not rendered through a zero-copy GPU path.
- Parcel features are hydrated into CPU `LayerDef::FeatureGeom` records.
- The render frame queries the CPU spatial index for visible feature candidates.
- Ring coordinates are projected on CPU into world/screen-space caches.
- Polygon fills are rebuilt into CPU scratch buffers and submitted through ImGui draw lists.
- Outlines are submitted through ImGui polyline commands.
- Vulkan eventually renders the ImGui command buffers, but the parcel vertex/index data is CPU-generated and copied through ImGui first.

Target state:

- Parcel geometry should become a retained GPU geometry path.
- Hydration and triangulation workers should produce immutable parcel render batches keyed by layer, tile or chunk, source signature, style generation, and LOD generation.
- A GPU upload worker should build vertex/index buffers once per valid generation.
- The render frame should bind existing buffers and issue draw calls for visible chunks.
- Per-frame CPU work should be limited to visibility selection, uniforms, small style/filter buffers, and command submission.
- Parcel picking and metadata lookup should remain CPU-accessible through immutable feature metadata, not by reading geometry back from the GPU.
- Stale parcel buffers may continue rendering while newer hydration, triangulation, filtering, or style jobs complete.

The parcel-level target is not strict zero-copy from source file to GPU. GeoJSON parsing, projection preparation, triangulation, simplification, metadata indexing, and picking support are CPU responsibilities. The target is zero-copy on the frame path: once a valid parcel GPU buffer exists, the render frame should not rebuild or recopy parcel vertices each frame.

The professional path should not send hydrated GeoJSON-style feature records directly to the GPU. It should compile parcel data into render-specific buffers first. The GPU receives retained vertex/index buffers and small per-frame uniform/style/filter buffers. CPU feature metadata remains available for search, selection, picking, joins, and status reporting.

Parcel-level buffer contract:

- Buffers carry `source_signature`, `hydration_generation`, `triangulation_generation`, `style_generation`, and `lod_generation`.
- Buffers are immutable after publication.
- Buffers are retired only after the render thread is done with them.
- Stale buffers are discarded when their source signature no longer matches the layer source.
- Missing or rebuilding buffers degrade to the previous complete buffer, an LOD buffer, or outline-only rendering.

Parcel-level data flow:

```text
source file
  -> hydration worker
  -> CPU feature metadata + rings
  -> triangulation/simplification worker
  -> chunk/tile render batch builder
  -> render-binary cache
  -> GPU upload worker
  -> retained parcel vertex/index buffers
  -> render snapshot
  -> render thread draw calls
```

### Render Binary Format

The current hydration cache is a persistence cache for CPU feature records. It avoids reparsing GeoJSON, but it is still expensive for very large parcel layers because it decodes a large MsgPack object graph and recreates many CPU vectors and strings. It is not the ideal format for render startup.

The target architecture should add a separate render-binary cache. This cache is downstream of hydration and triangulation, and upstream of GPU upload.

The first implemented step is a binary hydration cache at `data/cache/hydration/<layer-file>.bin`. That cache still stores CPU feature records, not final GPU buffers, but it removes the largest MsgPack decode cost and establishes explicit binary signatures, atomic writes, and status phases. The retained render-binary cache described below is the next downstream cache layer.

The second implemented step is a binary triangulation cache at `data/cache/triangulation/<layer-file>.tri.bin`. That cache stores the per-feature triangle index vectors keyed by the same source signature. It removes the JSON triangulation decode cost and makes parcel fill readiness observable through explicit triangulation cache phases.

The third implemented step is a persistent CPU projection cache. `MapProjectionCache` is now owned outside the single frame, reused while `math_zoom` is stable, and invalidated when hydrated layer geometry is replaced. This does not yet create retained GPU buffers, but it removes one source of repeated per-frame world-coordinate reconstruction and establishes the correct invalidation boundary for later render-binary work.

Operationally, the supported migration path is:

- runtime hydration workers prefer `.bin`
- legacy `.msgpack` is read only as a fallback
- `worldsim3 --warm-hydration-cache <layer-file>` converts one legacy cache to binary
- `worldsim3 --warm-hydration-cache-all` converts all currently cacheable local layers that already have a cache artifact
- triangulation workers prefer `.tri.bin`
- legacy `.tri.json` is read only as a fallback
- `worldsim3 --warm-triangulation-cache <layer-file>` converts one legacy triangulation cache to binary
- `worldsim3 --warm-triangulation-cache-all` converts all currently cacheable local triangulation artifacts and skips stale legacy caches
- the map frame reuses a persistent world-ring/world-extent projection cache until `math_zoom` or hydrated source generation changes

This keeps the architecture migration incremental. Teams can materialize the faster binary hydration layer without waiting for the later retained-GPU-buffer work.

Recommended parcel render-binary contents:

- fixed header with schema version, source signature, feature count, chunk count, coordinate space, and bounds
- chunk table with byte offsets, feature ranges, world-space bounds, LOD level, vertex count, and index count
- contiguous vertex buffers, preferably already in the GPU vertex layout
- contiguous index buffers, using `uint32_t` unless a chunk can safely use `uint16_t`
- compact feature-id mapping from draw primitive to parcel feature id
- optional compact style/filter attribute columns
- separate metadata offsets for owner, address, parcel id, and other strings

This format should be chunked so startup can load the visible chunks first and defer the rest. It should be memory-mappable where practical, but the render frame should still not parse or validate large structures.

Binary format requirements:

- source signature must match the source layer
- schema version must match the renderer
- byte order and alignment must be explicit
- chunks must be independently loadable
- failed or partial writes must never replace the last valid cache
- cache writes must be atomic
- GPU upload should happen on a worker or upload queue, not inside the UI frame

A different binary format will be faster than the current MsgPack hydration cache for render startup if it avoids object reconstruction and stores data in the same contiguous layout the renderer needs. The expected win is not just smaller bytes on disk; it is fewer allocations, less pointer chasing, less JSON-shaped decoding, and less per-frame geometry preparation.

Strict disk-to-GPU zero-copy is not the target. Vulkan normally uploads device-local buffers through staging memory or a dedicated transfer path. The professional target is direct enough for the frame loop: render-ready binary chunks are loaded, uploaded asynchronously into retained GPU buffers, and then reused without rebuilding or recopying during normal frames.

### Aggregate-Level Rendering

Current state:

- Aggregate heatmaps are separate from individual parcel geometry.
- Aggregate jobs collect or bin CPU heat samples, optionally using the experimental GPU splat path.
- Completed aggregate rasters are uploaded as textures.
- Texture upload currently uses a staging buffer and a CPU `memcpy`, so it is not strict zero-copy.
- Cached aggregate textures can be reused by key, and stale aggregate results can be preserved while new work is pending.

Target state:

- Aggregate rendering should remain a texture/raster pipeline, separate from parcel mesh rendering.
- Aggregate jobs should run off-frame and publish completed `AggregateTextureSnapshot` resources.
- The render frame should only draw the latest complete texture for the active aggregate key.
- Pan, zoom, filter, style, and algorithm changes should create explicit aggregate generations.
- Obsolete aggregate jobs should be cancellable or discarded by generation.
- Previous aggregate textures should stay visible until the replacement texture is complete.
- Aggregate cache keys should include the upstream source signatures, filter generation, view/raster parameters, algorithm, and style parameters.

Aggregate-level target is also not strict zero-copy in the general case. CPU-generated rasters and disk-cached rasters require staging uploads. The professional target is frame-path isolation: aggregate generation and texture upload must not block UI input or parcel-level rendering.

Aggregate-level data flow:

```text
render snapshot + filters + view parameters
  -> aggregate job
  -> CPU bins or GPU splat/compute
  -> aggregate raster/texture
  -> aggregate cache
  -> AggregateTextureSnapshot
  -> render thread textured draw
```

### Separation Rules

- Selecting aggregate method `None` must never disable parcel-level rendering.
- Parcel-level rendering should continue independently when aggregate textures are absent, stale, rebuilding, or disabled.
- Aggregate-level rendering should not mutate parcel geometry buffers.
- Parcel metadata and selection should be based on parcel-level feature identity, not aggregate pixels.
- Status APIs should report parcel buffer readiness separately from aggregate texture readiness.
- Performance profiling should separate parcel CPU culling/projection/upload/draw from aggregate sample collection/generation/upload/draw.

## Cache Policy

Cache behavior should be boring and predictable:

- valid cache hits are used regardless of layer size
- stale cache entries are rebuilt after successful parse
- cache writes are atomic
- cache metadata includes source signature and feature count
- cache provenance is visible through status/debug APIs
- env vars may disable cache writes, but normal correctness should not require env vars

## Status and Observability

Expose enough state to diagnose stalls without attaching a debugger:

- current snapshot generation
- active jobs by type
- queue depths
- cache hits/misses/stale/rebuilt counts
- per-layer source signature
- per-layer hydration source: cache or source parse
- per-layer triangulation cache hit/miss
- latest snapshot build time
- dropped stale job results
- frame phase timings

The existing `/status`, `/profile`, and `/profile/layers` endpoints are good places to grow this.

## Migration Plan

### Phase 1: Stop Frame Blocking

- Keep existing rendering code.
- Move owner aggregate rebuilds off the frame.
- Move large spatial index builds off the frame.
- Render stale data while workers rebuild.
- Add job generations and stale-result discard.

### Phase 2: Snapshot Render Inputs

- Introduce `RenderSnapshot`.
- Publish immutable layer render data after hydration/triangulation/index completion.
- Make the render frame read one snapshot pointer at frame start.
- Move derived vacancy/tax overlay arrays into the snapshot.

### Phase 3: Async Aggregates

- Treat heatmap/aggregate textures as snapshot resources.
- Generate aggregate textures off-frame.
- Keep drawing the previous aggregate texture until the new one is ready.
- Add cancellation for obsolete pan/zoom/filter aggregate jobs.

### Phase 4: DuckDB and Query Isolation

- Move DuckDB rebuild into a background job.
- Make SQL/query UI show current database generation and rebuild status.
- Prevent query execution from blocking the frame.

### Phase 5: Formal Regression Tests

Add deterministic tests for:

- hydration cache hit/miss/rebuild
- large-layer cache rebuild behavior
- source signature propagation
- rehydration replacement semantics
- stale job result discard
- snapshot atomicity
- aggregate `None` preserving per-feature rendering

## Success Criteria

The architecture is working when:

- panning/zooming stays responsive during hydration
- large layer hydration no longer causes multi-second UI frames
- DuckDB rebuilds never freeze the UI
- owner aggregate rebuilds never freeze the UI
- render snapshots never contain mixed source generations
- status endpoints can explain what background work is active
- restarting after a successful large-layer hydration uses the cache instead of reparsing source
