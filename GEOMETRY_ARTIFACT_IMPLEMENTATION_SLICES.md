# Geometry Artifact Implementation Slices

This document breaks the migration into incremental implementation slices.

Use this with:

- [GEOMETRY_ARTIFACT_MIGRATION.md](/mnt/Cancer/worldsim3/GEOMETRY_ARTIFACT_MIGRATION.md)
- [GEOMETRY_ARTIFACT_CHECKLIST.md](/mnt/Cancer/worldsim3/GEOMETRY_ARTIFACT_CHECKLIST.md)

This file owns slice ordering and slice completion, not the full narrative status summary.

- For the current implementation snapshot, use [GEOMETRY_ARTIFACT_MIGRATION.md](/mnt/Cancer/worldsim3/GEOMETRY_ARTIFACT_MIGRATION.md).
- Use this document to decide landing order and remaining slice boundaries.

## Current Status

Status as of `2026-05-21`.

Completed:

- Slice 1: artifact naming and runtime contracts
- Slice 2: stable feature IDs into DuckDB and runtime contracts
- Slice 3: point artifact format and CLI
- Slice 6: polyline artifact format and CLI
- Slice 8: polygon artifact format and CLI

Substantially implemented but not complete:

- Slice 4: point runtime loader and draw path
- Slice 7: polyline runtime loader and generic draw path
- Slice 9: generic polygon runtime draw path
- Slice 11: parcel workflow cutover
- Slice 12: runtime status and CLI cleanup

Still open:

- Slice 5: GPU picking for points
- Slice 10: GPU picking for polygons
- Slice 13: delete CPU geometry runtime paths
- Slice 14: final simplification pass

## Sequencing Rules

- Each slice should leave the app buildable.
- Each slice should preserve current behavior unless the slice explicitly changes a user-visible workflow.
- New compiled geometry paths can land in parallel with old CPU geometry paths at first.
- Hydration and triangulation should only be deleted after every runtime consumer is cut over.
- Stable feature ID work must land early, because all later slices depend on it.

## Slice 1: Artifact Naming And Runtime Contracts

Status:

- complete

Goal:

- Introduce the new architecture vocabulary without changing rendering behavior.

Files:

- [types.h](/mnt/Cancer/worldsim3/types.h)
- [cache_io.h](/mnt/Cancer/worldsim3/cache_io.h)
- [layer_runtime.h](/mnt/Cancer/worldsim3/layer_runtime.h)
- [layer_runtime.cpp](/mnt/Cancer/worldsim3/layer_runtime.cpp)
- [DATAFLOW.md](/mnt/Cancer/worldsim3/DATAFLOW.md)
- [GEOMETRY_ARTIFACT_MIGRATION.md](/mnt/Cancer/worldsim3/GEOMETRY_ARTIFACT_MIGRATION.md)
- [GEOMETRY_ARTIFACT_CHECKLIST.md](/mnt/Cancer/worldsim3/GEOMETRY_ARTIFACT_CHECKLIST.md)

Changes:

- Add explicit geometry-class artifact naming helpers.
- Add runtime state fields for compiled geometry artifacts and stable feature IDs.
- Add non-invasive status terminology for artifact acquisition and GPU readiness.
- Do not remove old cache paths yet.

Exit:

- Code compiles.
- No functional change.

## Slice 2: Stable Feature IDs End To End

Status:

- complete

Goal:

- Make stable feature identity explicit before touching picking or DuckDB joins.

Files:

- [types.h](/mnt/Cancer/worldsim3/types.h)
- [layer_import.h](/mnt/Cancer/worldsim3/layer_import.h)
- [layer_import.cpp](/mnt/Cancer/worldsim3/layer_import.cpp)
- [duckdb_analytics.h](/mnt/Cancer/worldsim3/duckdb_analytics.h)
- [duckdb_analytics.cpp](/mnt/Cancer/worldsim3/duckdb_analytics.cpp)
- [parcel_consolidation.h](/mnt/Cancer/worldsim3/parcel_consolidation.h)
- [parcel_consolidation.cpp](/mnt/Cancer/worldsim3/parcel_consolidation.cpp)

Changes:

- Define stable `feature_id`.
- Persist `feature_id` into `layer_features`.
- Thread `feature_id` into derived parcel records where needed.
- Keep current runtime geometry behavior unchanged.

Exit:

- DuckDB rows can be resolved by `layer_file + feature_id`.

## Slice 3: Point Geometry Artifact Format

Status:

- complete

Goal:

- Land the first generic compiled geometry format on the simplest geometry class.

Files:

- [cache_io.h](/mnt/Cancer/worldsim3/cache_io.h)
- [cache_io.cpp](/mnt/Cancer/worldsim3/cache_io.cpp)
- [layer_import.cpp](/mnt/Cancer/worldsim3/layer_import.cpp)
- [worldsim_cli.h](/mnt/Cancer/worldsim3/worldsim_cli.h)
- [worldsim_cli.cpp](/mnt/Cancer/worldsim3/worldsim_cli.cpp)

Changes:

- Implement `.point.bin` header/load/save/validate.
- Add compiler path from point source layers to `.point.bin`.
- Add CLI compile/validate commands for point artifacts.

Exit:

- Representative point layers compile and validate.

## Slice 4: Point Runtime Loader And Upload Path

Status:

- partial

Goal:

- Prove the runtime can draw from compiled artifacts without CPU geometry fallback for points.

Files:

- [layer_workers.h](/mnt/Cancer/worldsim3/layer_workers.h)
- [layer_workers.cpp](/mnt/Cancer/worldsim3/layer_workers.cpp)
- [app_main_loop.cpp](/mnt/Cancer/worldsim3/app_main_loop.cpp)
- [worldsim_app.h](/mnt/Cancer/worldsim3/worldsim_app.h)
- [worldsim_app.cpp](/mnt/Cancer/worldsim3/worldsim_app.cpp)
- [map_render_hover.cpp](/mnt/Cancer/worldsim3/map_render_hover.cpp)
- [map_render_selection.cpp](/mnt/Cancer/worldsim3/map_render_selection.cpp)

Changes:

- Add point artifact acquisition.
- Add point Vulkan upload from `.point.bin`.
- Keep existing point CPU path behind fallback during this slice if needed.

Exit:

- Point layers can render from `.point.bin`.
- Point hover also uses `.point.bin`.
- Point interaction still does not use GPU picking.

## Slice 5: GPU Hover/Click Picking For Points

Status:

- not started

Goal:

- Cut the first interaction path over to GPU picking.

Files:

- [worldsim_app.cpp](/mnt/Cancer/worldsim3/worldsim_app.cpp)
- [map_render_hover.h](/mnt/Cancer/worldsim3/map_render_hover.h)
- [map_render_hover.cpp](/mnt/Cancer/worldsim3/map_render_hover.cpp)
- [map_inspection.h](/mnt/Cancer/worldsim3/map_inspection.h)
- [map_inspection.cpp](/mnt/Cancer/worldsim3/map_inspection.cpp)
- [selection.h](/mnt/Cancer/worldsim3/selection.h)
- [selection.cpp](/mnt/Cancer/worldsim3/selection.cpp)

Changes:

- Implement GPU picking for hover/click on point layers.
- Resolve picked features through stable IDs and DuckDB.

Exit:

- Point hover/click works with CPU geometry disabled.

## Slice 6: Polyline Geometry Artifact Format

Status:

- complete

Goal:

- Add the second generic geometry artifact class.

Files:

- [cache_io.h](/mnt/Cancer/worldsim3/cache_io.h)
- [cache_io.cpp](/mnt/Cancer/worldsim3/cache_io.cpp)
- [layer_import.cpp](/mnt/Cancer/worldsim3/layer_import.cpp)
- [worldsim_cli.cpp](/mnt/Cancer/worldsim3/worldsim_cli.cpp)

Changes:

- Implement `.polyline.bin` header/load/save/validate.
- Add compiler path from line source layers to `.polyline.bin`.

Exit:

- Representative polyline layers compile and validate.

## Slice 7: Polyline Runtime Loader, Upload, And Picking

Status:

- partial

Goal:

- Move polyline render and hover/click to compiled geometry.

Files:

- [layer_workers.cpp](/mnt/Cancer/worldsim3/layer_workers.cpp)
- [app_main_loop.cpp](/mnt/Cancer/worldsim3/app_main_loop.cpp)
- [worldsim_app.cpp](/mnt/Cancer/worldsim3/worldsim_app.cpp)
- [map_render_hover.cpp](/mnt/Cancer/worldsim3/map_render_hover.cpp)
- [map_render_selection.cpp](/mnt/Cancer/worldsim3/map_render_selection.cpp)

Changes:

- Add `.polyline.bin` runtime acquisition and upload.
- Add GPU picking support for polyline layers.

Exit:

- Polyline layers render from compiled artifacts.
- Polyline picking is still not implemented.

## Slice 8: Polygon Geometry Artifact Format

Status:

- complete

Goal:

- Generalize the parcel render blob into the final polygon artifact shape.

Files:

- [cache_io.h](/mnt/Cancer/worldsim3/cache_io.h)
- [cache_io.cpp](/mnt/Cancer/worldsim3/cache_io.cpp)
- [layer_import.cpp](/mnt/Cancer/worldsim3/layer_import.cpp)
- [worldsim_cli.cpp](/mnt/Cancer/worldsim3/worldsim_cli.cpp)

Changes:

- Implement `.polygon.bin` header/load/save/validate.
- Move polygon triangulation into compilation only.
- Emit fill and outline streams in `.polygon.bin`.
- Preserve parcel-specific capabilities through the generic artifact shape.

Exit:

- Polygon layers compile to `.polygon.bin`.

## Slice 9: Polygon Runtime Loader And Generic Polygon Upload

Status:

- partial

Goal:

- Move polygon drawing to the generic compiled artifact path.

Files:

- [layer_workers.cpp](/mnt/Cancer/worldsim3/layer_workers.cpp)
- [worldsim_app.cpp](/mnt/Cancer/worldsim3/worldsim_app.cpp)
- [map_render_layers.h](/mnt/Cancer/worldsim3/map_render_layers.h)
- [map_render_layers.cpp](/mnt/Cancer/worldsim3/map_render_layers.cpp)
- [map_render_overlays.cpp](/mnt/Cancer/worldsim3/map_render_overlays.cpp)

Changes:

- Upload polygon buffers from `.polygon.bin`.
- Replace parcel-only generic geometry assumptions where possible.
- Keep parcel business logic, but not parcel-only geometry persistence logic.

Exit:

- Generic polygon rendering uses `.polygon.bin`.
- Parcel and zoning still retain specialized runtime paths.

## Slice 10: GPU Picking For Polygons

Status:

- not started

Goal:

- Remove CPU polygon hit testing.

Files:

- [worldsim_app.cpp](/mnt/Cancer/worldsim3/worldsim_app.cpp)
- [map_render_hover.cpp](/mnt/Cancer/worldsim3/map_render_hover.cpp)
- [map_render_selection.cpp](/mnt/Cancer/worldsim3/map_render_selection.cpp)
- [map_inspection.cpp](/mnt/Cancer/worldsim3/map_inspection.cpp)
- [selection.cpp](/mnt/Cancer/worldsim3/selection.cpp)

Changes:

- Add polygon GPU hover/click picking.
- Add box selection or a first multi-feature selection path.
- Resolve selected features through stable IDs and DuckDB.

Exit:

- Polygon hit testing no longer requires CPU geometry.
- Parcel hover/click state no longer stores parcel `FeatureRecord*`, but the generic picked-feature-ID path is still incomplete.

## Slice 11: Derived Parcel Workflow Cutover

Status:

- partial

Goal:

- Make parcel workflows independent of CPU geometry residency.

Files:

- [parcel_consolidation.cpp](/mnt/Cancer/worldsim3/parcel_consolidation.cpp)
- [duckdb_analytics.cpp](/mnt/Cancer/worldsim3/duckdb_analytics.cpp)
- [status_api.cpp](/mnt/Cancer/worldsim3/status_api.cpp)
- [status_api_context_builder.cpp](/mnt/Cancer/worldsim3/status_api_context_builder.cpp)
- parcel-related UI modules that read selected parcel detail

Changes:

- Re-key parcel workflows around stable feature IDs and derived tables.
- Ensure detail panels, recoloring, and overlays resolve through DuckDB/derived facts.
- Prefer unified parcel extents and blocklot-based lookup over parcel feature geometry where possible.

Exit:

- Parcel overlays and selection visualization are artifact-aware.
- Parcel business workflows still have some `LayerDef::FeatureRecord` fallbacks, but they are no longer the default path.

## Slice 12: Runtime Status And CLI Cleanup

Status:

- partial

Goal:

- Remove user-visible references to hydration and triangulation.

Files:

- [layer_runtime.cpp](/mnt/Cancer/worldsim3/layer_runtime.cpp)
- [status_api.cpp](/mnt/Cancer/worldsim3/status_api.cpp)
- [worldsim_cli.cpp](/mnt/Cancer/worldsim3/worldsim_cli.cpp)
- docs files

Changes:

- Replace old status terms with artifact/gpu terms.
- Remove hydration/triangulation maintenance commands.
- Add geometry-artifact health commands only.

Exit:

- Bulk artifact tooling exists.
- Runtime/status output still contains hydration and triangulation terminology in places.

## Slice 13: Delete CPU Geometry Runtime Paths

Status:

- not started

Goal:

- Remove the old architecture once all consumers are migrated.

Files:

- [layer_workers.cpp](/mnt/Cancer/worldsim3/layer_workers.cpp)
- [cache_io.cpp](/mnt/Cancer/worldsim3/cache_io.cpp)
- [types.h](/mnt/Cancer/worldsim3/types.h)
- [app_main_loop.cpp](/mnt/Cancer/worldsim3/app_main_loop.cpp)
- [worldsim_app.cpp](/mnt/Cancer/worldsim3/worldsim_app.cpp)
- any remaining consumers of `LayerDef::FeatureRecord`

Changes:

- Delete hydration cache code.
- Delete triangulation cache code.
- Delete CPU geometry fallback paths.
- Remove `LayerDef::FeatureRecord` from normal runtime usage.

Exit:

- No production path can render or interact through CPU geometry.

## Slice 14: Final Simplification Pass

Status:

- not started

Goal:

- Remove transitional naming and parcel-specific artifact special cases that are now generic.

Files:

- [worldsim_app.cpp](/mnt/Cancer/worldsim3/worldsim_app.cpp)
- [cache_io.cpp](/mnt/Cancer/worldsim3/cache_io.cpp)
- [layer_runtime.cpp](/mnt/Cancer/worldsim3/layer_runtime.cpp)
- docs files

Changes:

- Rename generic systems that still carry parcel-era names.
- Remove transitional compatibility branches.
- Tighten docs to describe only the final architecture.

Exit:

- The codebase no longer reflects the old hydration/triangulation model in naming or behavior.

## Parallelism Guidance

Safe parallel slices after Slice 2:

- point artifact format work and DuckDB stable-ID enrichment
- polyline artifact format work and status/docs cleanup

Safe parallel slices after Slice 8:

- polygon runtime upload work
- GPU picking UI integration
- parcel workflow cutover

Avoid parallel overlap on:

- [cache_io.cpp](/mnt/Cancer/worldsim3/cache_io.cpp)
- [worldsim_app.cpp](/mnt/Cancer/worldsim3/worldsim_app.cpp)
- [layer_workers.cpp](/mnt/Cancer/worldsim3/layer_workers.cpp)

unless the write scopes are explicitly partitioned.

## Go/No-Go Gates

Do not start Slice 13 until:

- Slice 5 is complete for points
- Slice 7 is complete for polylines
- Slice 10 is complete for polygons
- Slice 11 is complete for parcel workflows
- verification in [GEOMETRY_ARTIFACT_CHECKLIST.md](/mnt/Cancer/worldsim3/GEOMETRY_ARTIFACT_CHECKLIST.md) is substantially green

Do not call the migration complete until:

- hydration is deleted
- triangulation is deleted
- CPU hit testing is deleted
- compiled geometry artifacts are the only render-interaction geometry path
