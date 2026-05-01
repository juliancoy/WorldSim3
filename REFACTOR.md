# Refactor Plan: Split `main.cpp` into Functional Translation Units

## Goals
- Reduce coupling and compile times by decomposing `main.cpp` into focused modules.
- Keep behavior identical during refactor (no functional regressions).
- Preserve ability to build and run after each step.

## Proposed Target Layout
- `src/app/main.cpp`
- `src/app/app.cpp`
- `src/app/app.h`

- `src/core/types.h`
- `src/core/layer_manifest.cpp`
- `src/core/layer_manifest.h`
- `src/core/layer_state_io.cpp`
- `src/core/layer_state_io.h`
- `src/core/geo.cpp`
- `src/core/geo.h`
- `src/core/feature_props.cpp`
- `src/core/feature_props.h`

- `src/io/http_download.cpp`
- `src/io/http_download.h`
- `src/io/cache_io.cpp`
- `src/io/cache_io.h`

- `src/pipeline/hydration.cpp`
- `src/pipeline/hydration.h`
- `src/pipeline/triangulation.cpp`
- `src/pipeline/triangulation.h`
- `src/pipeline/spatial_index.cpp`
- `src/pipeline/spatial_index.h`

- `src/render/vulkan_context.cpp`
- `src/render/vulkan_context.h`
- `src/render/tile_renderer.cpp`
- `src/render/tile_renderer.h`
- `src/render/layer_renderer.cpp`
- `src/render/layer_renderer.h`

- `src/ui/layers_panel.cpp`
- `src/ui/layers_panel.h`
- `src/ui/map_view.cpp`
- `src/ui/map_view.h`
- `src/ui/parcel_tooltip.cpp`
- `src/ui/parcel_tooltip.h`

- `src/api/status_api.cpp`
- `src/api/status_api.h`
- `src/api/command_api.cpp`
- `src/api/command_api.h`

## Functional Ownership
- `core/*`: data models, manifest parsing, settings persistence, geometry helpers.
- `io/*`: network download and cache read/write.
- `pipeline/*`: hydration, triangulation, spatial indexing.
- `render/*`: Vulkan setup + tile/layer draw implementation.
- `ui/*`: ImGui windows, filters, hover/tooltips, context menus.
- `api/*`: REST endpoints, request parsing, status serialization.

## Refactor Constraints
- No behavior changes while moving code.
- Keep old symbols until call sites are migrated.
- Prefer extracting pure functions first.
- Introduce minimal shared state structs instead of more globals.

## Shared State Decomposition
Create explicit state structs before split:
- `AppConfig` (paths, constants, zoom limits)
- `RuntimeState` (zoom, center, perf counters)
- `LayerRuntime` (layers, pipeline status, caches, lookup maps)
- `RenderState` (tile cache, vulkan handles)
- `ApiState` (pending commands, status snapshots)

Pass these to modules instead of relying on file-scope statics.

## Incremental Migration Plan
1. **Type extraction**
- Move structs/enums/constants to `core/types.h`.
- Replace local duplicates/includes with shared header.

2. **Pure helper extraction**
- Move `getPropertyValue`, zoning helpers, key normalization, lon/lat conversions, and ring LOD helpers to `core/*`.
- Add unit-testable signatures where possible.

3. **Manifest and UI state I/O**
- Move `loadManifest`, `loadLayerUiState`, `saveLayerUiState`, app settings I/O to `core/*`.

4. **HTTP and cache I/O**
- Move curl downloader and hydration/tri cache read/write to `io/*`.

5. **Pipeline split**
- Move hydration worker logic and queues to `pipeline/hydration.*`.
- Move triangulation worker + cache to `pipeline/triangulation.*`.
- Move spatial index build/query to `pipeline/spatial_index.*`.

6. **REST API split**
- Move status and command handlers to `api/*`.
- Keep a small API bootstrap in app layer.

7. **Renderer split**
- Move Vulkan init/shutdown to `render/vulkan_context.*`.
- Move tile cache/load/sample to `render/tile_renderer.*`.
- Move geometry draw passes to `render/layer_renderer.*`.

8. **UI split**
- Move Layers/Controls panel to `ui/layers_panel.*`.
- Move map interactions + context menu to `ui/map_view.*`.
- Move hover tooltip composition to `ui/parcel_tooltip.*`.

9. **Thin main**
- Keep `src/app/main.cpp` only for wiring:
  - init
  - main loop tick dispatch
  - shutdown

10. **Cleanup**
- Remove dead statics and duplicate helpers.
- Enforce include boundaries and forward declarations.

## Build System Changes
- Update `CMakeLists.txt` to compile new sources explicitly.
- Introduce logical source groups (`core`, `io`, `pipeline`, `render`, `ui`, `api`).
- Keep link set unchanged initially (`Vulkan`, `GLFW`, `curl`, `imgui`, etc.).

## Validation Checklist Per Step
- Build passes (`cmake --build ...`).
- App launches successfully.
- `/status` responds.
- Parcel hover still shows full fields.
- Vacancy probe metrics unchanged.
- Zoning filters still render/filter correctly.
- Street View context menu still opens URL.

## Risk Areas
- Implicit global state dependencies (worker threads + UI).
- Thread-safety around shared maps/queues during extraction.
- Order-of-initialization bugs after moving statics.
- Rendering regressions from splitting draw-path helpers.

## Recommended First PR Scope
- Steps 1-3 only (types + pure helpers + manifest/UI state I/O).
- Keep pipeline/render/api untouched for low-risk baseline.
