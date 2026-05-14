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
- It does not exercise persistent frame-to-frame projection-cache reuse.

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
