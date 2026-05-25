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
