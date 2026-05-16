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

DuckDB should remain analytics-oriented. Parcel render geometry should move into a dedicated render sidecar cache rather than becoming a DuckDB-backed draw source.

Target render-cache flow:

```text
source parcel geometry
  -> hydration cache
  -> triangulation cache
  -> parcel render sidecar cache
  -> async upload staging
  -> retained GPU vertex/index buffers
  -> render thread
```

The first implemented step toward that boundary is a binary parcel render sidecar cache. It is chunked, versioned, signature-validated, and stores contiguous world-space vertex/index streams plus feature and chunk lookup tables. It is not yet bound into Vulkan draw buffers, but it establishes the correct storage target for parcel geometry and keeps that geometry out of DuckDB.

KISS data rule:

- DuckDB stores parcel facts, joins, and analytics.
- Parcel render sidecar stores only the geometry needed for retained rendering plus minimal lookup metadata.
- Do not duplicate parcel attribute payloads into the render sidecar unless a render-time lookup proves necessary.

The next implemented step is GPU residency for parcel geometry. Runtime now uploads:

- immutable parcel position buffer
- immutable parcel index buffer
- immutable vertex-to-parcel-slot reference buffer
- mutable per-parcel RGBA buffer

The DuckDB/query-driven filter path writes colors into that per-parcel RGBA buffer.

The next implemented step is the actual parcel fill pipeline cutover. Parcel base fills are now rendered by a dedicated Vulkan graphics pipeline inserted into the map draw stream through an ImGui draw callback. That pipeline:

- binds retained parcel position, index, vertex-to-parcel-slot, and RGBA buffers
- uses per-frame viewport and map-transform push constants
- culls work at chunk granularity on CPU before issuing draws
- keeps outlines and overlay fills on the existing CPU/ImGui path for now

This means parcel base fills no longer rebuild triangles into ImGui every frame when the GPU parcel path is active.

The next implemented step is parcel overlay fill cutover on the same retained geometry. Runtime now maintains a second mutable per-parcel RGBA buffer for overlay fills and issues a second parcel draw pass from the render tail stage. Vacancy, tax, and parcel-parameter fill overlays now use the retained parcel GPU geometry instead of CPU tessellation when the parcel GPU path is active.

The next implemented step is parcel outline cutover. The parcel render sidecar now carries explicit ring-edge topology, runtime uploads a retained line-index buffer, and the render tail stage issues a dedicated GPU line pass for parcel boundaries and selected-parcel emphasis. This avoids drawing triangle edges or rebuilding CPU polylines for parcel rings.

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
- Parcel base fills can now bypass the CPU/ImGui fill path when GPU residency and draw state are ready.
- Non-parcel polygon fills are still rebuilt into CPU scratch buffers and submitted through ImGui draw lists.
- Parcel outlines can now use a retained GPU edge path built from explicit ring-edge topology.
- Parcel overlay fills can now use a second retained GPU color stream on the same geometry.
- Outlines are submitted through ImGui polyline commands.
- Vulkan now also owns a dedicated parcel fill graphics pipeline that binds retained buffers and issues chunked indexed draws inside the map draw stream.

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

The current hydration cache is a persistence cache for CPU feature records. It avoids reparsing GeoJSON, but it is still expensive for very large parcel layers because it recreates many CPU vectors and strings. It is not the ideal format for render startup.

The target architecture should add a separate render-binary cache. This cache is downstream of hydration and triangulation, and upstream of GPU upload.

The first implemented step is a binary hydration cache at `data/cache/hydration/<layer-file>.bin`. That cache still stores CPU feature records, not final GPU buffers, but it establishes explicit binary signatures, atomic writes, and status phases. The retained render-binary cache described below is the next downstream cache layer.

The second implemented step is a binary triangulation cache at `data/cache/triangulation/<layer-file>.tri.bin`. That cache stores the per-feature triangle index vectors keyed by the same source signature and makes parcel fill readiness observable through explicit triangulation cache phases.

The third implemented step is a persistent CPU projection cache. `MapProjectionCache` is now owned outside the single frame, reused while `math_zoom` is stable, and invalidated when hydrated layer geometry is replaced. This does not yet create retained GPU buffers, but it removes one source of repeated per-frame world-coordinate reconstruction and establishes the correct invalidation boundary for later render-binary work. A dedicated CLI self-test now verifies reuse at stable zoom and invalidation on zoom change.

The fourth implemented step is budgeted triangulation-result apply on the main thread. Triangulation and cache decode still happen on workers, but the final move of triangle vectors into live `LayerDef::FeatureGeom` records no longer commits an entire large parcel layer in one frame. The drain path now applies bounded batches per frame, publishes explicit `applying_binary_cache` or `applying_built_result` phases while work remains, and marks the layer `Ready` only after the final batch lands.

The fifth implemented step is asynchronous spatial-index construction. Full-layer `LayerSpatialIndex` rebuilds no longer happen inline in the frame loop. Stable hydrated layers now publish extent snapshots into a background spatial-index job queue, the worker builds the index off-frame, and the main thread only drains completed results. Results are accepted only when their source signature and feature count still match the current hydrated layer; stale results are discarded explicitly.

The sixth implemented step is maintained layer-profile accounting. The `/profile/layers` snapshot path no longer rescans every feature in dirty layers on the main thread just to recount rings, points, triangle indices, properties, and spatial-index stats. Those counters are now maintained at the actual mutation boundaries during hydration drain, triangulation apply, and spatial-index apply, and the snapshot builder only copies the already-maintained totals.

The seventh implemented step is bounded no-index render fallback. When a large layer is hydrated but its spatial index is not ready yet, the render pass no longer falls back to scanning every feature in one frame. Large no-index layers now advance through a rolling bounded scan budget per frame using persistent cursors. This keeps raw parcel rendering responsive while the spatial index worker catches up. Heatmap recomputation for large no-index layers is deferred rather than generated from a partial feature scan.

The eighth implemented step is cached heat normalization. Heat layers no longer rebuild percentile and grouped normalization distributions every frame when filter/domain state is unchanged. The render path now reuses normalization state keyed by the stable heatmap data key plus layer index, with bounded cache retention inside `HeatmapRuntimeState`.

The ninth implemented step is retained CPU fill-geometry caching inside `MapProjectionCache`. For polygon features, the render path now caches a flattened world-space vertex stream and a prevalidated triangle-index stream per feature. This remains useful for overlays, outlines, and non-GPU polygon paths even after parcel base fills move to a dedicated Vulkan parcel pipeline.

The tenth implemented step is the dedicated parcel fill graphics pipeline. The runtime now:

- loads parcel geometry from the render sidecar into retained GPU buffers
- keeps a separate vertex-to-parcel-slot buffer
- keeps a separate mutable RGBA buffer keyed by parcel slot
- updates that RGBA buffer from the DuckDB/query filter state
- inserts a Vulkan parcel draw callback into the map draw stream
- issues indexed draws only for visible parcel chunks

This is the first real frame-path cutover away from ImGui parcel triangle submission.

The eleventh implemented step is GPU parcel overlay fills. The runtime now:

- keeps a second mutable per-parcel RGBA buffer for overlay fills
- computes parcel-parameter, vacancy, and tax overlay fills into that buffer from current filter/query state
- injects a second parcel GPU draw from the render tail stage so overlay ordering remains consistent
- skips CPU tessellated parcel overlay fills when the overlay GPU pass is active

The twelfth implemented step is GPU parcel outlines and selection emphasis. The runtime now:

- stores explicit line-index topology in the parcel render sidecar
- uploads a retained parcel line-index buffer alongside fill indices
- keeps a third mutable per-parcel RGBA buffer for outline/selection color
- injects a dedicated line-list parcel draw from the render tail stage
- skips CPU parcel polylines and selected-parcel stroke submission when the GPU outline path is active

At this point the parcel layer is off the ImGui geometry path for base fills, overlay fills, outlines, and selected-parcel emphasis.

The thirteenth implemented step is asynchronous parcel render-cache load/build. The UI/frame loop no longer loads or rebuilds the parcel render sidecar inline. Instead:

- the frame loop publishes a parcel render-cache request keyed by layer file and source signature
- a background parcel render worker loads the binary sidecar, or rebuilds it from persisted hydration and triangulation caches when needed
- the main thread only consumes completed blobs, uploads retained GPU buffers, and updates the small mutable RGBA streams

This is the first real separation between parcel render-cache IO/build work and the frame loop. GPU buffer creation and upload still happen on the main/render side in this step because the Vulkan upload path is still using the shared upload command pool and command buffer.

The fourteenth implemented step is asynchronous parcel GPU upload. The runtime now has a dedicated parcel GPU upload worker inside the Vulkan subsystem. That worker:

- owns a private Vulkan command pool and command buffer
- accepts completed `ParcelRenderCacheBlob` upload requests
- builds fully uploaded parcel GPU payloads off the frame-thread orchestration path
- publishes completed payloads back to the main thread
- lets the main thread adopt completed payloads and retire the previous live parcel buffers

This keeps Vulkan object ownership coherent while moving device-local buffer creation and transfer work off the frame-loop orchestration path. Mutable per-parcel RGBA buffer updates still happen on the main/render side in this step because those are small mapped-memory writes.

The fifteenth implemented step is generation-tracked parcel GPU buffer retirement. When a new parcel GPU payload is adopted:

- the previous live parcel GPU payload is not destroyed immediately
- it is moved into a retired payload queue with a retire-after frame serial
- retirement drains only after enough presented frames have elapsed
- forced shutdown still drains the queue synchronously

This removes the last immediate destructive replacement in the parcel GPU residency path and makes payload lifetime match frame presentation progress instead of adoption timing.

The sixteenth implemented step is session-static parcel GPU geometry. Parcel geometry is now treated as startup residency, not as a hot-swappable runtime asset:

- the first successful parcel GPU upload locks the active parcel geometry source signature for the rest of the process
- long-lived large parcel buffers are therefore allocated and populated only for startup geometry residency
- after that point, runtime parcel work is limited to the mutable per-parcel RGBA streams
- if the parcel geometry source signature changes later in the same session, the runtime keeps the existing GPU geometry and logs a restart-required warning instead of replacing live geometry in-process

This is the correct professional boundary for the current product model. DuckDB filters and query state can continue to restyle parcels by writing color, but parcel geometry itself is now session-static.

The seventeenth implemented step is a compact canonical parcel binary companion for the statewide parcel layer. `worldsim_regional_parcel_builder` now emits:

- `data/layers/regional_parcels.geojson`
- `data/layers/regional_parcels.geojson.canonical.bin`

The GeoJSON file remains the human-readable interchange and debugging artifact. The companion binary stores the hydrated parcel feature content in a dense binary layout keyed to the final GeoJSON file signature. Hydration workers can consume that binary companion directly when the normal hydration cache is absent or stale, instead of reparsing multi-gigabyte GeoJSON text.

The tenth implemented step is independent retained parcel color storage. `MapProjectionCache` now stores a per-feature style record keyed by layer, feature index, and style generation, with a feature-wide color plus a subpolygon color vector. The current feature model still represents a parcel as one polygon-with-holes, so the subpolygon vector presently defaults to one entry for polygonal parcel features. The storage boundary is now explicit, though: geometry and color are retained separately, and color storage survives pan/zoom projection churn until a real source replacement resets the cache owner.

Operationally, the supported migration path is:

- runtime hydration workers read `.bin` hydration caches, canonical parcel binaries, or source GeoJSON
- `worldsim3 --warm-hydration-cache <layer-file>` validates or rebuilds one binary hydration cache
- `worldsim3 --warm-hydration-cache-all` processes local layers that already have a binary hydration cache artifact
- triangulation workers read `.tri.bin` caches or rebuild from hydrated geometry
- `worldsim3 --warm-triangulation-cache <layer-file>` validates or rebuilds one binary triangulation cache
- `worldsim3 --warm-triangulation-cache-all` processes local binary triangulation artifacts
- the map frame reuses a persistent world-ring/world-extent projection cache until `math_zoom` or hydrated source generation changes
- large triangulation results are applied into live feature records incrementally across frames instead of a single unbounded main-thread commit
- large spatial-index rebuilds are built asynchronously from extent snapshots and applied only when their source signature and feature count still match the current layer
- layer profile snapshots now copy maintained counters instead of rescanning full layers on the frame thread
- large no-index layers use a bounded rolling fallback scan instead of a full-layer render pass while waiting for async spatial-index completion
- heat normalization distributions are cached by stable heatmap data key instead of rebuilt every frame
- polygon fill rendering reuses retained world-space fill geometry and prevalidated triangle indices instead of rebuilding those CPU buffers every frame
- parcel features now have independent retained color storage with room for per-subpolygon colors

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

A render-ready binary format will be faster than the current CPU-feature hydration cache for render startup if it avoids object reconstruction and stores data in the same contiguous layout the renderer needs. The expected win is not just smaller bytes on disk; it is fewer allocations, less pointer chasing, and less per-frame geometry preparation.

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
