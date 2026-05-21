# Geometry Artifact Migration

This document defines the implementation plan for removing CPU geometry from the WorldSim3 runtime architecture.

## Progress Snapshot

Status as of `2026-05-21`.

Implemented:

- explicit compiled geometry artifact filenames:
  - `.point.bin`
  - `.polyline.bin`
  - `.polygon.bin`
- stable feature IDs threaded into DuckDB
- CLI compile/validate commands for point, polyline, and polygon artifacts
- bulk `--build-geometry-duckdb-artifacts` command
- point runtime rendering from compiled artifacts
- point hover from compiled artifacts
- crime point GPU upload from compiled artifacts
- generic polyline rendering from compiled artifacts
- generic polygon rendering from compiled artifacts
- generic render-time CPU fallback removed for point, polyline, and generic polygon layers
- zoning polygon artifact loading is active in runtime
- parcel selection overlays use the parcel render blob instead of CPU rings
- parcel hover hit-testing uses the parcel render blob instead of CPU polygon tests
- zoning hover/inspection now prefers polygon artifacts instead of CPU polygon tests
- hydrated layers now transition directly to `Ready` in artifact mode instead of entering triangulation
- the live app no longer starts a triangulation worker thread

Not implemented yet:

- GPU picking
- full parcel workflow cutover away from `LayerDef::FeatureGeom`
- deletion of hydration workers
- deletion of triangulation worker codepaths and types
- removal of CPU feature geometry as a normal runtime structure

Target outcome:

- all renderable geometry is compiled into explicit-type disk artifacts
- all interactive picking uses GPU-resident geometry
- all durable text/numeric feature data lives in DuckDB
- hydration and triangulation are deleted entirely

## Target Contracts

### Geometry Artifact Filenames

Every compiled geometry artifact must encode its geometry class in the filename:

- `data/cache/geometry/<layer-file>.point.bin`
- `data/cache/geometry/<layer-file>.polyline.bin`
- `data/cache/geometry/<layer-file>.polygon.bin`

The filename is part of the contract. Runtime code must not infer geometry class from internal payload structure alone.

### Geometry Artifact Ownership

Compiled geometry artifacts are the only persistent runtime geometry format.

They replace:

- source-shaped hydration caches
- triangulation caches
- parcel-only render sidecars

They are consumed by:

- Vulkan upload
- GPU picking
- viewport culling
- selection visualization

They are not the durable home for:

- full property bags
- large text fields
- query-oriented denormalizations

### DuckDB Ownership

DuckDB is the universal durable feature-data store.

It owns:

- normalized feature attributes
- source-specific spillover in `properties_json`
- detail-panel fields
- query/search/filter fields
- derived domain tables such as `unified_parcels`

It does not own:

- render geometry
- per-frame visibility state
- GPU upload payloads

### Stable Feature Identity

Everything depends on stable feature IDs.

Each compiled geometry record and each DuckDB row must be joinable through a stable feature identity that survives rebuilds.

Minimum requirements:

- deterministic per-source feature ID
- persisted in geometry artifacts
- persisted in DuckDB rows
- usable by GPU picking and detail lookup

Preferred shape:

- `layer_file`
- `feature_id`

Optional internal acceleration keys may still use dense integer indices, but those indices must be derived from stable IDs, not treated as canonical IDs themselves.

## Artifact Format

Each compiled geometry artifact should begin with:

- magic
- version
- endian marker
- source signature
- geometry class
- feature count
- chunk count

### `.point.bin`

Required payload:

- point positions
- feature refs
- per-feature bounds
- chunk/group bounds if chunking is used

Optional payload:

- glyph/type codes if the geometry compiler owns them

### `.polyline.bin`

Required payload:

- polyline vertex stream
- line index stream
- feature refs
- per-feature offset/count records
- per-feature bounds
- per-chunk bounds

### `.polygon.bin`

Required payload:

- polygon vertex stream
- fill index stream
- outline line index stream
- feature refs
- per-feature offset/count records
- per-feature bounds
- per-chunk bounds

Important:

- there is no separate triangulation cache
- triangulation happens inside geometry compilation and is persisted only in `.polygon.bin`

### Picking Metadata

All geometry artifacts must carry enough metadata to support GPU picking without CPU geometry reconstruction.

Minimum requirement:

- every rendered primitive can be mapped back to a stable feature ID

## Runtime Architecture

### Startup

Startup flow:

```text
source layer
  -> compiled geometry artifact exists and matches source signature
  -> runtime loads artifact metadata
  -> runtime uploads artifact payload to Vulkan
  -> layer becomes drawable and pickable
```

If the artifact is missing or stale:

- runtime schedules geometry compilation
- layer remains non-drawable until compilation succeeds
- runtime does not fall back to CPU geometry hydration

### Vulkan Residency

Runtime owns:

- device-local buffers
- host-visible mutable color/overlay buffers where needed
- per-frame visibility state
- retired GPU payload lifecycle

Runtime does not own:

- canonical geometry reconstruction
- source parsing as a fallback render path

### GPU Picking

GPU picking is required, not optional.

Phase 1:

- offscreen feature-ID picking for hover/click

Phase 2:

- rectangle selection
- lasso/multi-feature selection

Results must resolve as:

```text
picked GPU feature ref
  -> stable feature ID
  -> DuckDB row lookup
  -> UI detail / selection state
```

## DuckDB Changes

### Universal Feature Table

`layer_features` becomes the universal durable feature-data table.

Required columns:

- `layer_file`
- `feature_id`
- `layer_name`
- `duckdb_role`
- `category`
- `scale`
- provenance columns
- normalized commonly queried fields
- `properties_json`

Strong recommendation:

- make `(layer_file, feature_id)` the logical primary join key used across runtime and query systems

### Derived Tables

Derived tables such as `unified_parcels` remain allowed, but they must also preserve stable feature references back to the geometry layer.

That means derived parcel rows should retain enough identity to:

- recolor geometry
- drive selection overlays
- resolve detail rows

without CPU geometry scans.

## Migration Sequence

### Milestone 1: Artifact Contract

Deliverables:

- final binary header schema
- explicit filename convention
- stable feature ID contract
- artifact validation CLI

Exit criteria:

- artifact spec is frozen at version 1
- validation can reject bad magic, bad version, bad signature, and malformed counts

### Milestone 2: Geometry Compiler

Deliverables:

- shared geometry compiler entrypoint
- `.point.bin` output
- `.polyline.bin` output
- `.polygon.bin` output

Requirements:

- compiler performs any polygon triangulation internally
- compiler writes picking metadata
- compiler writes chunk bounds

Exit criteria:

- representative point, line, and polygon layers compile successfully

### Milestone 3: Runtime Loader/Uploader

Deliverables:

- unified geometry artifact loader
- unified Vulkan upload path by geometry class
- runtime signature tracking for compiled artifacts

Exit criteria:

- layers render from compiled artifacts only
- no source parse is needed for rendering

### Milestone 4: GPU Picking

Deliverables:

- hover picking
- click picking
- stable ID resolution to DuckDB

Exit criteria:

- current hover/click workflows work with CPU geometry disabled

### Milestone 5: DuckDB Identity Alignment

Deliverables:

- stable `feature_id` persistence in `layer_features`
- stable `feature_id` integration in derived tables
- detail panels and query tools reading through stable IDs

Exit criteria:

- selected feature -> DuckDB detail lookup works for all migrated layer classes

### Milestone 6: Consumer Migration

Consumers to migrate:

- rendering
- hover inspector
- click inspector
- selection state
- parcel recoloring/overlays
- repeatable filters
- detail panels
- derived parcel workflows

Exit criteria:

- no production workflow requires `LayerDef::FeatureGeom`

### Milestone 7: Deletion

Delete:

- hydration cache readers/writers
- triangulation cache readers/writers
- runtime CPU geometry fallback paths
- parcel-only transitional special cases that duplicate the generic artifact path
- code that assumes full source-shaped geometry is resident in RAM

Exit criteria:

- `LayerDef::FeatureGeom` is no longer part of the normal render/interaction architecture
- hydration and triangulation commands are removed from user-facing tooling

## Codebase Work Breakdown

### 1. Artifact Definition

Primary files:

- `cache_io.cpp`
- `cache_io.h`
- `DATAFLOW.md`

Tasks:

- define new artifact headers
- define load/save functions for point/polyline/polygon artifacts
- add artifact validation functions

### 2. Geometry Compiler

Primary files:

- new geometry compiler module or tool
- current parcel render artifact builder logic in `cache_io.cpp`
- import/build command paths

Tasks:

- generalize parcel render blob generation
- build line and point artifact emitters
- remove dependency on separate triangulation persistence

### 3. Runtime Loader

Primary files:

- `worldsim_app.cpp`
- `layer_workers.cpp`
- `layer_runtime.cpp`

Tasks:

- replace hydration/triangulation orchestration with geometry-artifact acquisition
- unify geometry residency state
- remove CPU geometry fallback assumptions

### 4. GPU Picking

Primary files:

- `worldsim_app.cpp`
- map interaction modules

Tasks:

- build ID-buffer or compute-based picking path
- map pick results to stable feature IDs
- integrate hover/click selection

### 5. DuckDB Alignment

Primary files:

- `duckdb_analytics.cpp`
- parcel derived-data modules

Tasks:

- persist stable feature IDs
- align detail/query paths to feature IDs
- ensure derived tables can recolor/select geometry without CPU scans

## Non-Goals

These are not goals of the target architecture:

- preserving CPU geometry as a hidden backup path
- keeping hydration for "just in case" scenarios
- keeping triangulation as a separately persisted maintenance stage
- storing full property bags inside geometry artifacts
- using DuckDB as a render geometry source

## Verification

Required verification before deletion:

- artifact validation tests for all three geometry classes
- render parity against current parcel path
- GPU picking correctness tests
- stable feature ID persistence tests
- DuckDB detail lookup parity tests
- startup performance measurements
- memory usage measurements
- large-layer stress tests
- stale artifact invalidation tests

## Final Architectural Rule

After migration:

- if geometry is needed for rendering or interaction, it must come from `.point.bin`, `.polyline.bin`, or `.polygon.bin`
- if text or numeric feature data is needed, it must come from DuckDB or an explicit derived table built for that purpose
- there is no CPU geometry persistence layer between the two
