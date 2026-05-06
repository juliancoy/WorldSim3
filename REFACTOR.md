# main.cpp Refactor Plan

Goal: break `main.cpp` into focused modules until it is under 2000 lines without changing runtime behavior or committing generated data.

## 1. Freeze Behavior

- Build the current baseline before each extraction.
- Keep smoke checks for startup, `/status`, `/profile`, layer settings load/save, and heatmap settings.
- Do not combine behavior changes with file-move refactors.

## 2. Define Module Boundaries

Extract by ownership, not by line ranges:

- `app_settings.{h,cpp}`: persistent app settings, grayscale, validation, UI defaults.
- `status_api.{h,cpp}`: HTTP control/status/profile endpoints.
- `profiling.{h,cpp}`: frame profiler, layer profile snapshots, process metrics.
- `tile_cache.{h,cpp}`: tile cache, retired Vulkan textures, tile download/decode queue.
- `heatmap.{h,cpp}`: aggregate method config, raster generation, KDE/splat/grid/hex/multires code.
- `layer_runtime.{h,cpp}`: hydration queues, triangulation jobs/results, layer pipeline state.
- `map_render.{h,cpp}`: map draw loop helpers, coordinate transforms, layer drawing helpers.
- `dataset_library.{h,cpp}`: downloads, version metadata, diff artifacts, freshness checks.
- `owners_panel.{h,cpp}`: owner aggregation/cache/table logic.
- `vacancy_tax_overlay.{h,cpp}`: vacancy/tax joins and overlay state.
- `zoning.{h,cpp}`: zoning class/group discovery, filters, colors.
- `lan_services.{h,cpp}`: LAN dataset serving, peer discovery, Arkavo integration UI glue.
- `ui_panels.{h,cpp}`: ImGui panels after state boundaries are clear.

## 3. Introduce Shared Context Types

Create `app_context.h` with grouped state structs:

- `ViewState`
- `LayerUiState`
- `RuntimeQueues`
- `RenderStats`
- `HeatmapState`
- `OverlayState`
- `NetworkState`

Keep raw global Vulkan objects isolated until later to avoid destabilizing rendering.

## 4. Extract Pure/Low-Risk Code First

Move static helpers with minimal dependencies:

- URL decoding.
- File hash/signature helpers.
- GeoJSON diff artifact writing.
- App settings load/save.
- Profile sample structs and summarizers.

This reduces size without changing control flow.

## 5. Extract Status/Profile API

Move the HTTP server worker into `status_api.cpp`.

Pass a compact `StatusApiContext` containing:

- Atomics for view/perf counters.
- Mutex-protected layer profile snapshots.
- REST command queues.
- Layer status snapshots.

Verify:

- `GET /status`
- `GET /profile`
- `GET /profile/layers`
- `GET /profile/reset`

## 6. Extract Profiling

Move profiler ownership into `profiling.{h,cpp}`:

- `ProfileFrameSample`
- `LayerProfileSnapshot`
- Rolling frame ring buffer.
- Layer snapshot refresh.
- Process metrics.
- JSON serialization.

Replace direct `main.cpp` mutation with methods:

- `profiler.recordFrame(sample)`
- `profiler.markLayerDirty(i)`
- `profiler.refreshLayerSnapshots(layers, layer_spatial)`
- `profiler.toJson()`

## 7. Extract Dataset Library

Move download/version/freshness/diff code and related structs into `dataset_library.{h,cpp}`.

Main should keep only:

- UI calls.
- High-level state.
- User-triggered actions.

Verify no `data/` outputs are staged or committed.

## 8. Extract Heatmap

Move heatmap ownership into `heatmap.{h,cpp}`:

- Aggregate method enum/config.
- Per-layer effective settings.
- Raster generation.
- Async job key/result types.
- KDE, splat, grid, hex, and multires algorithms.

Keep Vulkan texture upload in `main.cpp` initially if needed. After stable, move it behind a `HeatmapRenderer` abstraction.

## 9. Extract Layer Pipeline

Move hydration and triangulation into `layer_runtime.{h,cpp}`.

Expose:

- `enqueueHydration(index, required)`
- `drainHydrated()`
- `drainTriangulated()`
- `stopWorkers()`

This touches threading, so do it only after API/profiling extraction is stable.

## 10. Extract Overlays

Move vacancy/tax joins and overlay state into `vacancy_tax_overlay.{h,cpp}`.

Move zoning discovery/filter state into `zoning.{h,cpp}`.

Keep draw calls in `main.cpp` at first. Move draw helpers only after state ownership is stable.

## 11. Extract Tile Cache

Move tile cache ownership into `tile_cache.{h,cpp}`:

- LRU cache.
- Decode workers.
- Retired texture draining.
- Tile request state.
- Vulkan tile texture lifecycle.

Keep Vulkan handles explicit in a context object. Validate no descriptor-set lifetime regressions.

## 12. Extract UI Panels And Render Helpers

Move large ImGui panels into focused files:

- `layers_panel.cpp`
- `library_panel.cpp`
- `models_panel.cpp`
- `network_panel.cpp`

Move map drawing helpers after state ownership is clear.

## 13. Add Line Count Gate

Add `tools/check_main_size.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail
lines=$(wc -l < main.cpp)
if (( lines > 2000 )); then
  echo "main.cpp has ${lines} lines; limit is 2000" >&2
  exit 1
fi
```

Run it before committing the final phase.

## 14. Commit Strategy

- One commit per extraction phase.
- Every commit must build.
- Do not mix formatting-only changes with behavior-preserving moves.
- Never stage `data/`, `build/`, generated datasets, binaries, or local cache files.

## 15. Target End State

- `main.cpp` is under 2000 lines.
- `main.cpp` owns only startup, Vulkan setup, top-level loop orchestration, and shutdown.
- Domain code lives in named modules.
- Profiling endpoints remain available throughout the refactor.
