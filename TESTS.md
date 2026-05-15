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
- It does not exercise the hydration binary cache, legacy hydration MsgPack fallback, triangulation cache, derived cache invalidation, or DuckDB rebuild freshness.

## Hydration Cache Self-Test

```bash
./build/worldsim3 --hydration-cache-selftest
```

Purpose:

- Writes a small binary hydration cache fixture.
- Loads the fixture through `loadBinaryHydrationCache()`.
- Verifies feature extents, rings, properties, and stale signature rejection.

Expected pass signal:

- Command exits `0`.
- JSON contains `"ok": true`.

Coverage:

- Binary hydration cache round trip.
- Binary cache source-signature validation.
- Basic point and polygon feature shapes.

Limitations:

- It does not benchmark large-layer cache load speed.
- It does not exercise the legacy MsgPack fallback conversion path.

## Hydration Cache Warmer

```bash
./build/worldsim3 --warm-hydration-cache regional_parcels.geojson
./build/worldsim3 --warm-hydration-cache-all
```

Purpose:

- Validates an existing binary hydration cache for one layer.
- If only a matching legacy MsgPack hydration cache exists, converts it into the binary cache.
- Verifies the written binary cache can be read back with the same source signature.

Expected pass signal:

- Command exits `0`.
- JSON contains `"ok": true`.

Notes:

- Large layers can require substantial RAM while converting from legacy MsgPack because the converter must load the legacy cache before writing binary.
- Prefer running this with the interactive app closed when converting `regional_parcels.geojson`.
- `--warm-hydration-cache-all` only processes local layers that already have a binary or legacy hydration cache artifact.

## Triangulation Cache Self-Test

```bash
./build/worldsim3 --triangulation-cache-selftest
```

Purpose:

- Writes a small binary triangulation cache fixture.
- Loads the fixture through `loadBinaryTriCache()`.
- Verifies vector round-trip, stale signature rejection, and feature-count rejection.

Expected pass signal:

- Command exits `0`.
- JSON contains `"ok": true`.

## Triangulation Cache Warmer

```bash
./build/worldsim3 --warm-triangulation-cache regional_parcels.geojson
./build/worldsim3 --warm-triangulation-cache-all
```

Purpose:

- Validates an existing binary triangulation cache for one layer.
- If only a matching legacy JSON triangulation cache exists, converts it into the binary cache.
- Bulk mode converts all discovered triangulation cache artifacts and skips stale legacy caches whose signature or feature count no longer matches the source layer.

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

## Triangulation Apply Self-Test

```bash
./build/worldsim3 --triangulation-apply-selftest
```

Purpose:

- Verifies `drainTriangulationResults()` leaves a large result partially applied after the first bounded drain.
- Verifies the intermediate status phase is one of the explicit `applying_*` phases.
- Verifies repeated drains complete the result and restore the final cache-hit phase.

Expected pass signal:

- Command exits `0`.
- JSON contains `"ok": true`.

## Spatial Index Self-Test

```bash
./build/worldsim3 --spatial-index-selftest
```

Purpose:

- Verifies completed spatial-index results are applied through the drain path.
- Verifies the built index can answer a simple bounding-box query.
- Verifies stale spatial-index results are discarded when the hydrated source signature has changed.

Expected pass signal:

- Command exits `0`.
- JSON contains `"ok": true`.

## Layer Profile Self-Test

```bash
./build/worldsim3 --layer-profile-selftest
```

Purpose:

- Verifies layer profile snapshots are built from maintained accumulators.
- Verifies the snapshot copies feature, ring, point, triangle, property, and spatial-index counters.
- Verifies the dirty bit is cleared after refresh.

Expected pass signal:

- Command exits `0`.
- JSON contains `"ok": true`.

## Render Fallback Note

The bounded no-index render fallback currently has no dedicated CLI self-test.

Current coverage comes from code inspection and the shared render-path build integration:

- `render_layer_pass.cpp` uses a rolling bounded fallback scan for large no-index layers.
- hydration replacement and cache-clear paths reset the per-layer fallback cursors.
- heatmap recomputation is deferred for large no-index layers to avoid building aggregates from partial scans.

The next appropriate automated coverage would be a focused render-pass harness that verifies cursor advancement and bounded work when `LayerSpatialIndex::built == false`.

## Heat Normalization Cache Note

The heat-normalization cache currently has no dedicated CLI self-test.

Current coverage comes from code inspection and shared render-path integration:

- `render_layer_pass.cpp` caches normalization state by heatmap data key plus layer index.
- `heatmap_runtime.cpp` clears the normalization cache when the broader heatmap runtime cache is cleared.
- cache retention is bounded to prevent unbounded growth from repeated filter changes.

The next appropriate automated coverage would be a focused render-pass or heatmap harness that verifies a second render with the same heatmap data key reuses cached normalization state instead of rebuilding it.

## DuckDB Owner Dump Integration Check

```bash
./build/worldsim3_duckdb_owner_dump --quiet --summary-only --workers 2
```

Purpose:

- Hydrates locally available layers through the headless hydration worker/cache pipeline.
- Builds parcel consolidation artifacts.
- Rebuilds `data/worldsim.duckdb`.
- Queries the owner with the most properties.
- Validates that DuckDB `unified_parcels` row count matches in-memory consolidation.

Expected pass signal:

- Command exits `0`.
- Output includes hydration counts, DuckDB rebuild status, top owner summary, and unified parcel row count.

Coverage:

- Manifest loading.
- Local layer discovery.
- Hydration worker pipeline.
- Hydration cache read/write path.
- Parcel consolidation.
- DuckDB rebuild and SQL query execution.
- Basic consistency between in-memory unified parcels and DuckDB tables.

Limitations:

- Heavyweight: it can take a while and rewrites `data/worldsim.duckdb`.
- It does not exercise the interactive frame drain path exactly; the headless runner consumes hydration batches directly.
- It does not assert same-row-count source changes or mid-run rehydration replacement semantics.

Useful variants:

```bash
./build/worldsim3_duckdb_owner_dump --workers 4 --summary-only
./build/worldsim3_duckdb_owner_dump --quiet --output /tmp/top_owner.tsv
```

## GPU Aggregate Harness

```bash
./build/worldsim3_gpu_aggregate_harness
```

Common variants:

```bash
./build/worldsim3_gpu_aggregate_harness --howard
./build/worldsim3_gpu_aggregate_harness --input data/layers/parcel.geojson --jurisdiction "Baltimore City"
./build/worldsim3_gpu_aggregate_harness --raster 512 --repeats 3 --sigma 1.5
./build/worldsim3_gpu_aggregate_harness --no-cpu-blur
```

Purpose:

- Exercises the Vulkan heatmap aggregate path.
- Compares GPU aggregate output against CPU expectations where applicable.
- Reports output pixel count and density summary.

Expected pass signal:

- Command exits `0`.
- Output includes `GPU aggregate OK`.

Coverage:

- Vulkan device setup.
- Heatmap compute shader dispatch.
- GPU aggregate readback.
- CPU/GPU aggregate consistency checks.

Limitations:

- Requires a working Vulkan runtime and physical device.
- May fail on headless systems without GPU access even if app logic is correct.

## Arkavo Connectivity Test

```bash
./build/arkavo_connectivity_test --timeout 20
```

Two-peer data-channel check:

```bash
./build/arkavo_connectivity_test --room worldsim-connectivity-test
./build/arkavo_connectivity_test --room worldsim-connectivity-test --send-peer PEER_ID --send-file PATH
```

Purpose:

- Verifies signaling, ICE, WebRTC peer setup, and optional data-channel file send.

Expected pass signal:

- For a network smoke test, command reaches connected/signaling-ready state and exits successfully.
- For a two-peer send test, the receiving peer reports the file.

Coverage:

- Arkavo signaling transport.
- WebRTC session manager setup.
- Data-channel file transfer path.

Limitations:

- Requires network access to the configured signaling URL.
- Two-peer transfer requires coordinating two running instances and a valid peer id.

## Local CI Packaging Script

```bash
scripts/ci-local.sh local
```

Purpose:

- Builds Linux packaging artifacts.
- Configures and builds Windows MinGW targets.
- Packages Windows artifacts.

Expected pass signal:

- Command exits `0`.
- `dist/` contains produced artifacts listed by the script.

Coverage:

- Broader packaging and cross-build validation.
- Confirms release-adjacent build wiring.

Limitations:

- Heavyweight.
- May install packages when `AUTO_INSTALL_DEPS=1`.
- Requires MinGW and Vulkan cross-build dependencies for the Windows path.

Safer dependency-check variant:

```bash
AUTO_INSTALL_DEPS=0 scripts/ci-local.sh local
```

## Size/Structure Checks

```bash
tools/check_file_sizes.sh
tools/check_main_size.sh
```

Purpose:

- Checks repository source/file size constraints.
- Helps catch accidental large-file or main-loop growth regressions.

Expected pass signal:

- Command exits `0`.

Coverage:

- Repository hygiene.
- File size guardrails.

Limitations:

- These are static checks, not behavioral tests.

## Current Cache Propagation Gap

The existing tests provide useful integration coverage, especially `worldsim3_duckdb_owner_dump`, but there is not yet a small deterministic regression test for cache propagation itself.

A focused cache regression should use tiny synthetic layers and assert:

- first hydration writes a cache file
- second hydration loads from cache
- rehydration replaces existing runtime features rather than appending
- same-row-count source changes invalidate derived caches by signature
- triangulation jobs preserve the hydration source signature
- DuckDB `needsRebuild()` changes when `analytics_build_info.source_signature` differs
