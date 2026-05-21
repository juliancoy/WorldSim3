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
./build/worldsim3 --warm-parcel-render-cache parcel.geojson
./build/worldsim3 --warm-parcel-render-cache-all
./build/worldsim3 --parcel-artifact-health parcel.geojson
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
