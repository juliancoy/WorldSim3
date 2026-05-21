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

## Parcel Render Cache Self-Test

```bash
./build/worldsim3 --parcel-render-cache-selftest
./build/worldsim3 --warm-parcel-render-cache regional_parcels.geojson
./build/worldsim3 --warm-parcel-render-cache-all
./build/worldsim3 --parcel-artifact-health regional_parcels.geojson
```

Purpose:

- Verifies a parcel render sidecar blob can be built from the current parcel-prep pipeline.
- Verifies the binary sidecar round-trips contiguous vertex/index data, line-index topology, plus feature/chunk tables.
- Verifies a stale source signature is rejected.

Expected pass signal:

- Command exits `0`.
- JSON contains `"ok": true`.

Additional warmer coverage:

- `--warm-parcel-render-cache <layer>` verifies a real layer can be converted into a binary parcel render sidecar.
- `--warm-parcel-render-cache-all` attempts the same conversion for every layer with the required inputs.
- `--parcel-artifact-health <layer>` performs a lightweight header/size/signature audit of the canonical binary, parcel render sidecar, and DuckDB artifact without loading full feature bodies.
- The parcel render self-test also verifies vertex-to-parcel-slot references and line-index topology round-trip, which are the lookups used by the GPU-side parcel fill and outline paths.

Current additional verification:

- `cmake --build build --target worldsim3 -j1` now also verifies the parcel fill vertex and fragment shaders compile to SPIR-V and the dedicated parcel Vulkan pipeline code links cleanly.
- The same build now verifies the retained parcel overlay GPU color-buffer path, retained parcel outline GPU color-buffer path, and the parcel fill/overlay/outline draw callbacks compile and link.
- The same build now verifies the asynchronous parcel render worker wiring compiles and links, including shutdown/join handling and sidecar request/result plumbing.
- The same build now verifies the asynchronous parcel GPU upload worker wiring compiles and links, including worker-owned Vulkan upload context creation, stale-result discard, payload adoption, and shutdown/join handling.
- The same build now verifies generation-tracked parcel GPU retirement wiring compiles and links, including retire-after-frame tracking, per-frame drain hooks, and forced shutdown drain.
- The same build now verifies the session-static parcel geometry residency policy compiles and links: startup upload remains supported, while later in-process parcel source signature changes are handled as restart-required instead of live geometry replacement.
- The same build now verifies the canonical parcel binary path compiles and links: the regional parcel builder emits only `regional_parcels.geojson.canonical.bin`, and the parcel-prep path can load that artifact directly through the normal source-signature validation path.
- There is not yet a dedicated headless harness that asserts parcel GPU draw callback output on a live Vulkan frame.
- The current parcel GPU cutover is therefore covered by build integration plus the parcel-render-sidecar round-trip tests.

## Spatial Index Self-Test

```bash
./build/worldsim3 --spatial-index-selftest
```

Purpose:

- Verifies completed spatial-index results are applied through the drain path.
- Verifies the built index can answer a simple bounding-box query.
- Verifies stale spatial-index results are discarded when the source signature has changed.

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
- source replacement and cache-clear paths reset the per-layer fallback cursors.
- heatmap recomputation is deferred for large no-index layers to avoid building aggregates from partial scans.

The next appropriate automated coverage would be a focused render-pass harness that verifies cursor advancement and bounded work when `LayerSpatialIndex::built == false`.

## Render Policy Self-Test

```bash
./build/worldsim3 --render-policy-selftest
```

Purpose:

- Verifies value parcel layers cannot enter an aggregate/detail display gap when aggregate max zoom is lower than parcel detail min zoom.
- Verifies a layer configured with aggregate max zoom 9 switches to parcel detail at zoom 10 rather than disappearing through zoom 12.
- Verifies aggregate max zoom is ignored when aggregate mode is `None`.

Expected pass signal:

- Command exits `0`.
- JSON contains `"ok": true`.

## Heat Normalization Cache Note

The heat-normalization cache currently has no dedicated CLI self-test.

Current coverage comes from code inspection and shared render-path integration:

- `render_layer_pass.cpp` caches normalization state by heatmap data key plus layer index.
- `heatmap_runtime.cpp` clears the normalization cache when the broader heatmap runtime cache is cleared.
- cache retention is bounded to prevent unbounded growth from repeated filter changes.

The next appropriate automated coverage would be a focused render-pass or heatmap harness that verifies a second render with the same heatmap data key reuses cached normalization state instead of rebuilding it.

## GPU Aggregate Harness

```bash
./build/worldsim3_gpu_aggregate_harness
```

Common variants:

```bash
./build/worldsim3_gpu_aggregate_harness --howard
./build/worldsim3_gpu_aggregate_harness --input data/world/earth/nation_state/us/state_region/md/county_city/baltimore_city/layers/parcel.geojson --jurisdiction "Baltimore City"
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

- first geometry-cache build writes a cache file
- second geometry-cache load reuses that cache
- same-source reload replaces existing runtime features rather than appending
- same-row-count source changes invalidate derived caches by signature
- geometry build jobs preserve the source signature
- DuckDB `needsRebuild()` changes when `analytics_build_info.source_signature` differs
