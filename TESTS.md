# WorldSim3 Tests and Verification Commands

This file lists the test-style binaries, harnesses, and local verification commands available in this repository.

## Build Verification

```bash
cmake --build build -j2
```

Purpose:

- Compiles the main app and configured helper binaries.
- Catches C++ API drift, missing includes, bad signatures, link errors, and shader target wiring.

Expected pass signal:

- Command exits `0`.
- Final output links `worldsim3` and any changed helper binaries.

Notes:

- Use a larger `-j` value if the machine has enough memory.
- This is the fastest broad check after C++ changes.

## Vacancy Self-Test

```bash
./build/worldsim3 --vacancy-selftest
```

Purpose:

- Loads parcel, vacant building notice, and vacant rehab source layers.
- Verifies blocklot matching between vacancy records and parcel geometries.
- Emits a JSON summary with row counts, matched/unmatched counts, and geometry availability.

Expected pass signal:

- Command exits `0`.
- JSON contains `"ok": true`.

Coverage:

- Source layer availability.
- Vacancy join keys.
- Parcel geometry presence.
- Basic end-to-end vacancy overlay assumptions.

Limitations:

- It reads source files directly.
- It does not exercise compiled geometry artifacts, derived cache invalidation, or DuckDB rebuild freshness.

## Polygon Hole Selftest

```bash
./build/worldsim3 --polygon-hole-selftest
```

Purpose:

- Verifies polygon-with-hole behavior across hit testing and parcel render blob flattening.
- Exercises polygon fill generation through the current in-process geometry build path.

Expected pass signal:

- Command exits `0`.
- JSON contains `"ok": true`.

## Projection Cache Self-Test

```bash
./build/worldsim3 --projection-cache-selftest
./build/worldsim3 --projection-fill-cache-selftest
./build/worldsim3 --projection-color-cache-selftest
```

Purpose:

- Verifies `MapProjectionCache` fills on first use.
- Verifies world-ring/world-extent reuse when `math_zoom` is unchanged.
- Verifies cache invalidation and rebuild when `math_zoom` changes.

Expected pass signal:

- Command exits `0`.
- JSON contains `"ok": true`.

Additional fill-cache coverage:

- `--projection-fill-cache-selftest` verifies retained world-space fill geometry is built on first use.
- Verifies invalid triangle indices are discarded once during cache build.
- Verifies the cached fill geometry is reused across frame-projection changes at stable `math_zoom`.
- Verifies the fill cache is invalidated and rebuilt when `math_zoom` changes.

Additional color-cache coverage:

- `--projection-color-cache-selftest` verifies per-feature retained color storage is absent before first write.
- Verifies style-key mismatch produces a cache miss.
- Verifies overwriting the same parcel feature with a new style key replaces the retained feature color and subpolygon color vector.
- Verifies color storage survives zoom/projection cache invalidation because it is independent from world-geometry caches.

## Parcel Geometry Artifact Checks

```bash
./build/worldsim3 --compile-polygon-geometry parcel.geojson
./build/worldsim3 --validate-polygon-geometry parcel.geojson
./build/worldsim3 --parcel-artifact-health parcel.geojson
./build/worldsim3 --parcel-polygon-identity-selftest
./build/worldsim3 --parcel-selection-ui-harness
./build/worldsim3 --parcel-hover-click-ui-harness
./build/worldsim3 --duckdb-parcel-semantic-snapshot-selftest
```

Purpose:

- Verifies the parcel polygon geometry artifact can be compiled from the current parcel-prep pipeline.
- Verifies the compiled polygon artifact passes header/signature validation.
- Verifies parcel artifact health across canonical binary, polygon geometry artifact, and DuckDB analytics.

Expected pass signal:

- Command exits `0`.
- JSON contains `"ok": true`.

Additional coverage:

- `--parcel-artifact-health <layer>` performs a lightweight header/size/signature audit of the canonical binary, polygon geometry artifact, and DuckDB artifact without loading full feature bodies.
- Polygon artifact validation still covers the feature/chunk tables and the fill/line index buffers used by the GPU-side parcel fill and outline paths.
- `--parcel-polygon-identity-selftest` verifies parcel polygon artifacts are accepted only when every feature carries `entity_id`, and that artifact-to-runtime parcel blob conversion rejects missing identity instead of falling back to CPU feature records.
- `--parcel-selection-ui-harness` verifies selected-parcel highlighting is drawn from resident artifact identity only, and that a single selected `entity_id` highlights every matching geometry record for both the primary parcel blob and county polygon artifacts.
- `--parcel-hover-click-ui-harness` verifies parcel hover resolves stable parcel identity, county-parcel hover detail can fall back through DuckDB when no in-memory unified parcel row is present, and click selection opens/selects the resolved parcel entity.
- `--duckdb-parcel-semantic-snapshot-selftest` verifies the runtime parcel semantic snapshot is loaded from `layer_features` and `unified_parcels` in DuckDB, producing per-feature parcel counts/search fields without scanning canonical property bags.

Current additional verification:

- `cmake --build build --target worldsim3 -j1` now also verifies the parcel fill vertex and fragment shaders compile to SPIR-V and the dedicated parcel Vulkan pipeline code links cleanly.
- The same build now verifies the retained parcel overlay GPU color-buffer path, retained parcel outline GPU color-buffer path, and the parcel fill/overlay/outline draw callbacks compile and link.
- The same build now verifies the asynchronous parcel render worker wiring compiles and links, including shutdown/join handling and sidecar request/result plumbing.
- The same build now verifies the asynchronous parcel GPU upload worker wiring compiles and links, including worker-owned Vulkan upload context creation, stale-result discard, payload adoption, and shutdown/join handling.
- The same build now verifies generation-tracked parcel GPU retirement wiring compiles and links, including retire-after-frame tracking, per-frame drain hooks, and forced shutdown drain.
- The same build now verifies the session-static parcel geometry residency policy compiles and links: startup upload remains supported, while later in-process parcel source signature changes are handled as restart-required instead of live geometry replacement.

## Baltimore Region Generated Data Checks

```bash
./scripts/check_baltimore_region_generated_data.sh parcels-required
./scripts/check_baltimore_region_generated_data.sh zoning-audit
./scripts/check_baltimore_region_generated_data.sh zoning-required
```

Purpose:

- Verifies high-profile Baltimore-region parcel and zoning artifact matrices that are important for visible county-scale regression coverage.
- Provides a non-gating audit mode when an operator wants a full zoning report without failing a larger run.

Expected pass signal:

- `parcels-required` exits `0`.
- `zoning-required` exits `0`.
- `zoning-audit` reports pass/fail by layer and exits `0`.

Current ctest coverage:

- `ctest -R worldsim3_baltimore_region_parcel_artifact_matrix --output-on-failure`
- `ctest -R worldsim3_baltimore_region_zoning_artifact_matrix --output-on-failure`

Notes:

- The parcel and zoning matrices are intentionally narrow: they enforce the visible Baltimore-region layers that have already proven to be high-value regression targets.
- `zoning-audit` remains useful when you want the same report shape without failing a broader operator run.

## Raster Tile Contract Check

```bash
./build/worldsim3 --render-polygon-tile parcel.geojson 14 4821 6140
ctest -R worldsim3_render_polygon_tile_contract --output-on-failure
ctest -R worldsim3_render_polygon_tile_provenance --output-on-failure
ctest -R worldsim3_render_polygon_tile_runtime_policy --output-on-failure
```

Purpose:

- Verifies the offline raster tile product described in `professional render.md` is emitted as a derived cache artifact rather than an alternate source of truth.
- Verifies the command exposes the render-route and cache-key provenance needed for professional debugability.
- Verifies the runtime zoom policy is conservative and explicit:
  zoomed out raster, mid zoom raster base plus vector outline, high zoom vector only.

Expected pass signal:

- The command exits `0` and writes a PPM tile under `data/cache/render_tiles/...`.
- The contract test verifies the JSON output includes:
- The runtime policy test verifies:
  - generic polygon `z10 -> raster_only`
  - generic polygon `z12 -> raster_base_vector_outline`
  - generic polygon `z14 -> vector_only`
  - heatmap, zoning, filtered, and query-driven polygon states stay `vector_only`
  - active parcel GPU stays `vector_only`
  - county parcel polygon fallback can use `raster_only`
  - `render_path`
  - `source_signature`
  - `style_key`
  - `tile_path`
  - a cache-keyed output path matching the requested `z/x/y`
- The provenance test verifies the tile is explicitly reported as a derived cache artifact, tied to the validated artifact/source signature, and can be deleted and regenerated at the same cache-keyed path.

Notes:

- This is a contract test for provenance and cache-key shape, not yet a full vector/raster equivalence test.

## Generated Data Coverage Matrix

This section describes how strong current test coverage is for generated data paths, not just whether individual commands exist.

Coverage labels:

- `Well covered`: exercised by dedicated self-tests plus artifact or semantic verification.
- `Partially covered`: some validation exists, but it is uneven, indirect, or limited to headers/artifacts/buildability.
- `Effectively untested`: little or no repeatable verification beyond ad hoc manual inspection.

### Well Covered

- `Primary parcel canonical + geometry + DuckDB ingest path`
  Commands: `--validate-canonical-parcel-binary`, `--parcel-artifact-health`, `--compile-polygon-geometry`, `--validate-polygon-geometry`, `--duckdb-parcel-semantic-snapshot-selftest`, `--duckdb-parcel-ingest-selftest`, `--parcel-polygon-identity-selftest`.
  Why: this is the strongest generated-data path in the repo. It has artifact validation, semantic verification, ingest verification, and UI harness coverage.

- `Parcel interaction data derived from generated artifacts`
  Commands: `--parcel-selection-ui-harness`, `--parcel-hover-click-ui-harness`.
  Why: these tests verify that generated parcel artifacts remain usable for selection, hover, identity resolution, and county fallback behavior.

- `Parcel vacancy/tax overlay assumptions on generated parcel joins`
  Commands: `--vacancy-selftest`, `--duckdb-parcel-semantic-snapshot-selftest`.
  Why: these do not fully cover rebuild freshness, but they do exercise the generated parcel join assumptions that downstream overlays depend on.

### Partially Covered

- `Generic polygon geometry artifacts, including zoning-style layers`
  Commands: `--compile-polygon-geometry <layer>`, `--validate-polygon-geometry <layer>`, build verification.
  Why: the repo can compile and validate polygon artifacts, but coverage is layer-by-layer and not enforced as a complete required set. Recent manual verification showed multiple zoning layers with missing or stale compiled polygon artifacts.

- `Point and polyline generated geometry artifacts`
  Commands: `--compile-point-geometry`, `--validate-point-geometry`, `--compile-polyline-geometry`, `--validate-polyline-geometry`.
  Why: the command surface exists, but there is no comparable end-to-end matrix showing that all intended generated point and polyline layers are routinely materialized, validated, and renderable.

- `Startup preprocess generated outputs`
  Commands: `--build-geometry-duckdb-artifacts`, `--startup-preprocess`.
  Why: these can generate required runtime artifacts, but the repo does not yet document a dedicated self-test that proves startup preprocess deterministically produces the full required artifact set without runtime surprises.

- `Runtime GPU renderability of generated artifacts`
  Commands: build verification, parcel UI harnesses, manual status/screenshot API checks.
  Why: some generated data paths are proven renderable in practice, especially parcels, but most layer families are not covered by a repeatable automated render regression suite.

### Effectively Untested

- `Whole-repo generated data completeness as a single deterministic contract`
  Gap: there is no one-command test that materializes the expected generated data set, validates every required artifact, and fails on any missing/stale member.

- `Generated data isolation and reproducibility under concurrent workflows`
  Gap: tests and runtime currently share mutable state such as `data/worldsim.duckdb` and cached artifacts. Operationally this means a long-running test or app instance can affect another run.

- `Visual regression coverage for all generated layer families`
  Gap: parcel-region screenshot sweeps are feasible and were run manually, but there is no checked-in automated visual regression suite for parcels, zoning, point layers, and polylines together.

- `Semantic fixture-based validation for most non-parcel generated layers`
  Gap: outside the parcel pipeline, most generated data paths do not yet have fixture-driven correctness assertions that verify expected counts, identities, joins, or styling behavior.

## Current Professional Assessment

Generated-data testing is:

- `Robust` for the core parcel pipeline.
- `Moderate` for generated artifacts in general.
- `Not yet robust` across all generated data classes and render paths.

The biggest current gap is not the absence of commands. The gap is that coverage is uneven across layer families, especially outside parcels, and the repo does not yet enforce one repeatable generated-data contract for the full Baltimore-region target set.
