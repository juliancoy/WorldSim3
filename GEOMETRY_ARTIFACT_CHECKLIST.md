# Geometry Artifact Checklist

This checklist maps the geometry-artifact migration to the current codebase.

Use this together with [GEOMETRY_ARTIFACT_MIGRATION.md](/mnt/Cancer/worldsim3/GEOMETRY_ARTIFACT_MIGRATION.md).

## Phase 1: Contracts And Schemas

### [types.h](/mnt/Cancer/worldsim3/types.h)

- [ ] Introduce runtime types for compiled geometry artifacts by class:
  - [ ] `PointGeometryArtifact`
  - [ ] `PolylineGeometryArtifact`
  - [ ] `PolygonGeometryArtifact`
- [ ] Introduce stable feature ID types used across runtime and DuckDB.
- [ ] Add runtime state that tracks artifact signature, geometry class, and GPU residency.
- [ ] Mark `LayerDef::FeatureGeom` for removal from normal runtime paths.

Exit:

- [ ] No new runtime subsystem depends on `LayerDef::FeatureGeom`.

### [cache_io.h](/mnt/Cancer/worldsim3/cache_io.h)

- [ ] Declare load/save/validate APIs for:
  - [ ] `.point.bin`
  - [ ] `.polyline.bin`
  - [ ] `.polygon.bin`
- [ ] Declare shared artifact-header structs and validation metadata.
- [ ] Declare filename helpers that produce explicit-type names.

Exit:

- [ ] No public cache API mentions hydration or triangulation.

### [cache_io.cpp](/mnt/Cancer/worldsim3/cache_io.cpp)

- [ ] Implement the new artifact headers.
- [ ] Implement save/load for `.point.bin`.
- [ ] Implement save/load for `.polyline.bin`.
- [ ] Implement save/load for `.polygon.bin`.
- [ ] Implement artifact validation helpers.
- [ ] Move polygon triangulation persistence into `.polygon.bin` generation only.
- [ ] Delete or deprecate:
  - [ ] hydration cache readers/writers
  - [ ] triangulation cache readers/writers
  - [ ] parcel-only render sidecar persistence as a special case

Exit:

- [ ] `cache_io.cpp` has one compiled-geometry family, not three geometry cache concepts.

## Phase 2: Geometry Compiler

### [layer_import.h](/mnt/Cancer/worldsim3/layer_import.h)

- [ ] Add declarations for geometry compilation entrypoints.
- [ ] Separate source normalization from compiled geometry emission.

### [layer_import.cpp](/mnt/Cancer/worldsim3/layer_import.cpp)

- [ ] Build a source-layer -> compiled-geometry path for point layers.
- [ ] Build a source-layer -> compiled-geometry path for polyline layers.
- [ ] Build a source-layer -> compiled-geometry path for polygon layers.
- [ ] Ensure polygon compilation emits fill and outline data directly.
- [ ] Ensure geometry compilation emits stable feature IDs.
- [ ] Ensure geometry compilation emits picking metadata and chunk bounds.
- [ ] Remove geometry-build dependencies on hydration and triangulation caches.

Exit:

- [ ] A source layer can compile directly to explicit-type geometry artifacts with no intermediate hydration/triangulation persistence.

### [worldsim_cli.h](/mnt/Cancer/worldsim3/worldsim_cli.h)

- [ ] Add options for geometry compile/validate/warm commands.
- [ ] Remove declarations for hydration/triangulation warm paths after cutover.

### [worldsim_cli.cpp](/mnt/Cancer/worldsim3/worldsim_cli.cpp)

- [ ] Add artifact validation commands by geometry class.
- [ ] Add geometry compile/warm commands by layer file.
- [ ] Add artifact-health reporting for `.point.bin`, `.polyline.bin`, `.polygon.bin`.
- [ ] Remove user-facing hydration commands.
- [ ] Remove user-facing triangulation commands.
- [ ] Replace parcel-only special-case health reporting with generic geometry-artifact health reporting.

Exit:

- [ ] CLI exposes compiled-geometry workflows only.

## Phase 3: Runtime Geometry Acquisition

### [layer_workers.h](/mnt/Cancer/worldsim3/layer_workers.h)

- [ ] Replace hydration/triangulation worker contracts with geometry-artifact acquisition contracts.
- [ ] Define worker job/result types around explicit geometry classes.

### [layer_workers.cpp](/mnt/Cancer/worldsim3/layer_workers.cpp)

- [ ] Replace hydration worker logic with geometry-artifact acquisition.
- [ ] Replace triangulation worker logic with geometry-artifact acquisition or geometry compile scheduling.
- [ ] Load only compiled geometry artifacts into runtime staging structures.
- [ ] Remove source-parse fallback from normal render readiness.
- [ ] Remove runtime writes of hydration caches.
- [ ] Remove runtime writes of triangulation caches.

Exit:

- [ ] `layer_workers.cpp` contains no production hydration pipeline.
- [ ] `layer_workers.cpp` contains no production triangulation pipeline.

### [layer_runtime.h](/mnt/Cancer/worldsim3/layer_runtime.h)

- [ ] Rename runtime phases/status fields away from hydration/triangulation vocabulary.
- [ ] Add explicit geometry-artifact readiness phases:
  - [ ] artifact_missing
  - [ ] artifact_loading
  - [ ] artifact_validated
  - [ ] gpu_upload_pending
  - [ ] gpu_ready
  - [ ] gpu_pick_ready

### [layer_runtime.cpp](/mnt/Cancer/worldsim3/layer_runtime.cpp)

- [ ] Replace display-status strings that mention hydration/triangulation with geometry-artifact states.
- [ ] Remove parcel-only wording like “render blob mode” once the generic path exists.

Exit:

- [ ] Runtime statuses describe compiled geometry, not CPU geometry staging.

### [app_main_loop.cpp](/mnt/Cancer/worldsim3/app_main_loop.cpp)

- [ ] Replace startup hydration queueing with geometry-artifact acquisition queueing.
- [ ] Ensure non-drawable behavior when artifacts are missing is explicit and cheap.
- [ ] Remove assumptions that enabled layers become renderable by filling `layers[i].features`.

Exit:

- [ ] Startup never reparses source geometry on the critical render path.

## Phase 4: Vulkan Upload Paths

### [worldsim_app.h](/mnt/Cancer/worldsim3/worldsim_app.h)

- [ ] Expose runtime interfaces for loading/uploading compiled geometry artifacts by class.
- [ ] Expose runtime interfaces for GPU picking result lookup.

### [worldsim_app.cpp](/mnt/Cancer/worldsim3/worldsim_app.cpp)

- [ ] Generalize parcel GPU upload payload logic into:
  - [ ] point upload path
  - [ ] polyline upload path
  - [ ] polygon upload path
- [ ] Remove parcel-only naming where it represents generic geometry concerns.
- [ ] Keep per-feature color/overlay mutability without rebuilding geometry.
- [ ] Add artifact-class-aware residency bookkeeping.
- [ ] Add a generic GPU picking path:
  - [ ] ID-buffer or compute-based hover/click picking
  - [ ] selection picking support
- [ ] Ensure GPU picking resolves to stable feature IDs, not CPU geometry indices.

Exit:

- [ ] Rendering and picking operate entirely from compiled geometry artifacts and GPU state.

## Phase 5: Map Interaction Consumers

### [map_render_layers.h](/mnt/Cancer/worldsim3/map_render_layers.h)

- [ ] Update interfaces to use artifact-backed draw state, not `LayerDef::FeatureGeom`.

### [map_render_layers.cpp](/mnt/Cancer/worldsim3/map_render_layers.cpp)

- [ ] Remove assumptions that raw layer geometry is CPU-resident for drawing.

### [map_render_hover.h](/mnt/Cancer/worldsim3/map_render_hover.h)

- [ ] Reframe hover contracts around picked feature IDs.

### [map_render_hover.cpp](/mnt/Cancer/worldsim3/map_render_hover.cpp)

- [ ] Replace CPU geometry hover detection with GPU pick result handling.

### [map_render_selection.h](/mnt/Cancer/worldsim3/map_render_selection.h)

- [ ] Reframe selection contracts around stable feature IDs.

### [map_render_selection.cpp](/mnt/Cancer/worldsim3/map_render_selection.cpp)

- [ ] Replace CPU geometry selection scans with GPU selection result handling.

### [map_inspection.h](/mnt/Cancer/worldsim3/map_inspection.h)

- [ ] Reframe inspection data loading around feature ID + DuckDB lookup.

### [map_inspection.cpp](/mnt/Cancer/worldsim3/map_inspection.cpp)

- [ ] Remove CPU geometry dependency for feature detail resolution.

### [selection.h](/mnt/Cancer/worldsim3/selection.h)

- [ ] Replace selection identity based on layer index + feature index alone with stable feature IDs.

### [selection.cpp](/mnt/Cancer/worldsim3/selection.cpp)

- [ ] Migrate selection persistence and state updates to stable feature IDs.

Exit:

- [ ] Hover, click, and selection require no CPU geometry access.

## Phase 6: DuckDB Alignment

### [duckdb_analytics.h](/mnt/Cancer/worldsim3/duckdb_analytics.h)

- [ ] Add stable feature ID APIs.
- [ ] Add lookup helpers keyed by `layer_file + feature_id`.

### [duckdb_analytics.cpp](/mnt/Cancer/worldsim3/duckdb_analytics.cpp)

- [ ] Add `feature_id` to `layer_features`.
- [ ] Ensure `feature_id` is persisted for all geometry classes.
- [ ] Ensure derived tables preserve geometry linkage through stable IDs.
- [ ] Remove rebuild assumptions that depend on session-only CPU geometry.
- [ ] Move any remaining geometry-side durable attributes into DuckDB.

Exit:

- [ ] Feature detail lookup from a GPU-picked feature works through DuckDB for all migrated layers.

### [parcel_consolidation.h](/mnt/Cancer/worldsim3/parcel_consolidation.h)

- [ ] Ensure parcel-derived records retain stable references to geometry features.

### [parcel_consolidation.cpp](/mnt/Cancer/worldsim3/parcel_consolidation.cpp)

- [ ] Remove reliance on source-shaped CPU parcel geometry for derived joins where not strictly necessary.
- [ ] Ensure parcel recolor/selection workflows can resolve through stable IDs and DuckDB-derived tables.

Exit:

- [ ] Parcel workflows can recolor/select without scanning CPU geometry.

## Phase 7: API And Status Surfaces

### [status_api.h](/mnt/Cancer/worldsim3/status_api.h)

- [ ] Rename or extend status payloads to report compiled geometry artifact health, GPU readiness, and picking readiness.

### [status_api.cpp](/mnt/Cancer/worldsim3/status_api.cpp)

- [ ] Remove hydration/triangulation-centric reporting.
- [ ] Report artifact class, artifact signature, GPU residency, and pickability.

### [status_api_context_builder.h](/mnt/Cancer/worldsim3/status_api_context_builder.h)

- [ ] Update exposed layer/runtime fields to match the compiled-geometry architecture.

### [status_api_context_builder.cpp](/mnt/Cancer/worldsim3/status_api_context_builder.cpp)

- [ ] Remove assumptions that runtime feature vectors are the main geometry readiness signal.

Exit:

- [ ] Status surfaces describe artifact + GPU state, not CPU geometry staging.

## Phase 8: Documentation And Cleanup

### [DATAFLOW.md](/mnt/Cancer/worldsim3/DATAFLOW.md)

- [ ] Keep aligned with actual implementation milestones.
- [ ] Remove remaining transitional wording once cutover completes.

### [GEOMETRY_ARTIFACT_MIGRATION.md](/mnt/Cancer/worldsim3/GEOMETRY_ARTIFACT_MIGRATION.md)

- [ ] Update milestone status as work lands.
- [ ] Mark deletion milestone complete only after old paths are removed.

### [GEOMETRY_ARTIFACT_CHECKLIST.md](/mnt/Cancer/worldsim3/GEOMETRY_ARTIFACT_CHECKLIST.md)

- [ ] Check off tasks as files are migrated.
- [ ] Add file-level notes when scope changes.

## Deletion Checklist

### CPU Geometry Infrastructure

- [ ] Remove `LayerDef::FeatureGeom` from normal runtime ownership.
- [ ] Remove production codepaths that require full source-shaped geometry in RAM.
- [ ] Remove CPU hit-testing paths.
- [ ] Remove CPU selection scan paths.

### Cache Infrastructure

- [ ] Delete hydration cache file naming and helpers.
- [ ] Delete triangulation cache file naming and helpers.
- [ ] Delete hydration/triangulation clear-cache UI paths.
- [ ] Delete hydration/triangulation CLI commands.

### Parcel Transitional Special Cases

- [ ] Remove parcel-only artifact naming as the generic geometry path takes over.
- [ ] Remove parcel-only runtime state names where they encode generic geometry concepts.
- [ ] Keep parcel-specific business logic only where it is truly domain-specific.

## Verification Checklist

- [ ] `.point.bin` validation tests exist.
- [ ] `.polyline.bin` validation tests exist.
- [ ] `.polygon.bin` validation tests exist.
- [ ] GPU hover picking works without CPU geometry.
- [ ] GPU click picking works without CPU geometry.
- [ ] Selection workflows work without CPU geometry.
- [ ] DuckDB detail lookup works from stable feature IDs.
- [ ] Parcel recoloring works from derived data + stable IDs.
- [ ] Startup avoids source-geometry parsing on the render critical path.
- [ ] Large-layer rendering works with CPU geometry disabled.

## Completion Rule

This migration is complete only when:

- all render and interaction geometry comes from `.point.bin`, `.polyline.bin`, or `.polygon.bin`
- all durable text/numeric feature data comes from DuckDB or explicit derived tables
- hydration is gone
- triangulation is gone
- no production workflow requires CPU geometry residency
