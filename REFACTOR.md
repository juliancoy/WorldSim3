# WorldSim3 Refactor Plan: Target Service Architecture

This document describes the intended service decomposition for the app shell and render backend. The transition starts from `worldsim_app.cpp` and `app_main_loop.cpp`, but the target architecture is expressed in terms of service ownership rather than temporary file boundaries.

## Refactor Goals

1. Reduce both files to thin orchestration units.
2. Split by runtime ownership, not by arbitrary line chunks.
3. Replace broad file-static/global state with explicit service contexts.
4. Keep existing behavior and public entry points stable during extraction.
5. Make the next extractions reviewable in small PRs.

## Target End State

End state:

1. The application shell is a thin coordinator that sequences startup preprocess, runtime bootstrap, background services, the main loop service, and shutdown.
2. The render backend is a thin facade over dedicated rendering services.
3. Vulkan, GPU residency, GPU picking, tile textures, startup preprocess, runtime assembly, and shutdown all have explicit service ownership.
4. Cross-service state is passed through typed runtime contexts rather than large file-static/global surfaces.

## Proposed Services

### Render backend services

1. `vulkan_context_service.{h,cpp}`
   Owns instance/device/queue/descriptor pool/sampler/command pool setup and teardown.

2. `frame_present_service.{h,cpp}`
   Owns `SetupVulkanWindow`, `FrameRender`, `FramePresent`, `FrameRenderSecondary`, `FramePresentSecondary`, and swapchain screenshot capture.

3. `parcel_gpu_service.{h,cpp}`
   Owns parcel residency, color uploads, draw state, draw callbacks, upload worker, retired payload draining, and residency/profiler status publishing.

4. `zoning_gpu_service.{h,cpp}`
   Owns zoning layer residency, descriptor sets, outline indirect compute, color buffers, and zoning draw callbacks.

5. `point_layer_gpu_service.{h,cpp}`
   Owns point layer and crime point residency, pipelines, descriptor sets, glyph/color uploads, and draw callbacks.

6. `polyline_gpu_service.{h,cpp}`
   Owns polyline layer residency, descriptors, pipelines, color uploads, and draw callbacks.

7. `gpu_pick_service.{h,cpp}`
   Owns offscreen pick render pass/resources, point pick staging buffers, and polygon/point pick execution.

8. `tile_texture_service.{h,cpp}`
   Owns texture upload, mip generation, descriptor finalization, tile cache LRU, cache eviction, and retired texture draining.

### App shell services

1. `startup_preprocess_service.{h,cpp}`
   Owns `StartupPreprocessPlan`, subprocess execution, CLI mode, and preprocess UI window.

2. `app_window_service.{h,cpp}`
   Owns primary/secondary GLFW+ImGui window creation, context setup, font upload, and callback wiring for the download queue window.

3. `app_runtime_bootstrap.{h,cpp}`
   Owns initial runtime assembly currently done inline in `runWorldSim3App()`: layer loading, registry/index discovery, UI state load, derived state initialization, and default selections.

4. `background_services_bootstrap.{h,cpp}`
   Owns hydration workers, spatial worker, parcel render cache worker, status API worker, dataset API worker, and LAN discovery worker.

5. `app_runtime_state.{h,cpp}`
   Defines the state aggregates that are currently hundreds of locals inside `runWorldSim3App()`.

6. `main_loop_service.{h,cpp}`
   Owns the top-level while-loop sequencing and delegates to existing extracted modules such as `frame_prelude`, `map_tab`, `left_panel`, `right_panel`, and `layer_ui_state_sync`.

7. `app_shutdown_service.{h,cpp}`
   Owns stop flags, worker joins, queue drains, final persistence, and Vulkan/UI teardown ordering.

## Recommended State Aggregates

The first high-value move is not another utility function. It is introducing explicit state structs so service boundaries have something stable to receive.

Create these aggregates before or alongside the service moves:

1. `RenderBackendState`
   Holds Vulkan globals, swapchain flags, upload command resources, screenshot state, and queue mutexes.

2. `ParcelGpuRuntime`
   Holds parcel buffers, draw state, upload worker state, retired payloads, and GPU profiler alert/status state.

3. `LayerGpuRuntime`
   Holds zoning, point, polyline, and crime layer GPU maps and per-layer draw state.

4. `TileRuntime`
   Holds tile cache, retired textures, tile sampler, and cache policy counters.

5. `StartupRuntime`
   Holds preprocess plan state, subprocess log lines, and preprocess UI progress state.

6. `AppRuntimeState`
   Holds the large mutable app state currently allocated as locals in `runWorldSim3App()`.

7. `BackgroundServiceHandles`
   Holds worker threads, stop flags, queues, mutexes, and futures that need coordinated shutdown.

## Extraction Order

### Phase 1: Stabilize State Boundaries

1. Introduce the runtime aggregates listed above.
2. Move existing globals/locals into those structs without changing behavior.
3. Pass those structs into existing helper functions.

This is the lowest-risk step and makes later file moves mechanical.

### Phase 2: Move startup preprocess behind `startup_preprocess_service`

Move out:

1. `StartupPreprocessIssue`
2. `StartupPreprocessPlan`
3. `StartupPreprocessRunResult`
4. `inspectStartupPreprocessPlan()`-related helpers
5. `runStartupPreprocessWindow()`
6. `runStartupPreprocessCli()`

Result:

The app coordinator keeps only the decision point for whether preprocess must run.

### Phase 3: Move window/bootstrap behind `app_window_service`

Move out:

1. Main GLFW window sizing/creation
2. Main ImGui Vulkan initialization
3. Download queue window creation and callback wiring
4. Font upload boilerplate for both windows

Result:

Window/bootstrap details disappear from the top-level app coordinator.

### Phase 4: Move worker startup behind `background_services_bootstrap`

Move out:

1. Hydration worker startup
2. Spatial worker startup
3. Parcel render cache worker
4. Status API worker
5. Dataset API worker
6. LAN discovery worker

Result:

Thread ownership is centralized and shutdown becomes tractable.

### Phase 5: Move `tile_texture_service` and `gpu_pick_service`

These two are relatively self-contained backend services and can move before the parcel/zoning draw path.

### Phase 6: Move `parcel_gpu_service`

Move out parcel-specific residency, color upload, upload worker, draw state, and callbacks as one service. Do not split parcel upload worker from parcel draw ownership; they share lifecycle and buffer state.

### Phase 7: Move `zoning_gpu_service`

Keep zoning outline indirect compute with zoning draw ownership. It should not live in a generic compute helper because its inputs are zoning-specific buffers and feature metadata.

### Phase 8: Move `point_layer_gpu_service` and `polyline_gpu_service`

These services mirror the already-established map/layer architecture and can share a small amount of internal helper code through a private header if needed.

### Phase 9: Move `frame_present_service`

Once the GPU draw services are out, frame/present code can depend on a stable render backend context rather than transitional globals.

### Phase 10: Reduce the app shell to orchestration

At this point the top-level runner should mostly do:

1. Parse CLI / immediate commands
2. Load settings
3. Run startup preprocess gate
4. Build app runtime
5. Start background services
6. Run main loop service
7. Run shutdown service

## Why This Split

This split follows the intended ownership seams for the steady-state app:

1. Parcel, zoning, point, and polyline rendering have distinct residency, color, and draw-state lifecycles.
2. GPU picking has its own render pass and staging resources.
3. Tile caching has its own lifecycle and memory-retirement policy.
4. Startup preprocess already acts like a separate mode.
5. Background workers form a service cluster with shared stop/join semantics.
6. Existing modules such as `map_frame_session`, `map_tab`, `frame_prelude`, and `layer_ui_state_sync` already point toward service-style boundaries.

## Concrete PR Slices

1. PR 1: Refresh `REFACTOR.md`, add runtime aggregate structs, no logic changes.
2. PR 2: Extract `startup_preprocess_service`.
3. PR 3: Extract `app_window_service` and `app_shutdown_service`.
4. PR 4: Extract `background_services_bootstrap` and thread handle structs.
5. PR 5: Extract `tile_texture_service`.
6. PR 6: Extract `gpu_pick_service`.
7. PR 7: Extract `parcel_gpu_service`.
8. PR 8: Extract `zoning_gpu_service`.
9. PR 9: Extract `point_layer_gpu_service` and `polyline_gpu_service`.
10. PR 10: Extract `frame_present_service` and leave the render backend facade/orchestrator only.

## Guardrails

1. Preserve the existing public API in `worldsim_app.h` until the moves are complete.
2. Prefer move-only PRs before behavior changes.
3. Keep the shutdown order identical while thread/service ownership is moving.
4. Do not merge service boundaries that share only helper code; extract helpers later if duplication survives.
5. Keep `docs/RUN_LOOP_MIGRATION.md` aligned as each phase lands so the migration notes do not drift again.

## Verification Per Phase

1. `cmake -S . -B build`
2. `cmake --build build -j`
3. Launch smoke test: `./build/worldsim3`
4. Exercise startup preprocess mode if touched
5. Exercise map render, parcel/zoning rendering, and hover/pick behavior if GPU services are touched
6. Exercise download queue, status API, and LAN discovery if background services are touched

## Success Criteria

1. App-shell responsibilities are owned by dedicated startup/bootstrap/loop/shutdown services.
2. Render-backend responsibilities are owned by dedicated Vulkan/GPU/tile/pick/frame services.
3. Service ownership is explicit enough that new work lands in service modules rather than temporary coordinator files.
4. Runtime shutdown and worker lifecycle are explicit and reviewable.
