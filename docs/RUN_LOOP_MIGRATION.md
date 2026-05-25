# Run Loop Target

This document describes the intended app-shell structure for the main runtime loop.

The target is a thin top-level runner that sequences services. It should not be the long-term owner of startup preprocess details, window/bootstrap boilerplate, large runtime-state assembly, background worker wiring, or low-level shutdown ordering.

## Target App Shell

The top-level runner should do only this:

1. Parse CLI and immediate command modes.
2. Load persistent app settings.
3. Run startup preprocess gating.
4. Build runtime state through bootstrap services.
5. Start background services.
6. Enter the main loop service.
7. Run orderly shutdown.

## Target Services

### Startup and bootstrap

- `startup_preprocess_service.{h,cpp}`: readiness inspection, CLI mode, subprocess execution, and preprocess UI.
- `app_window_service.{h,cpp}`: GLFW window creation, ImGui/Vulkan window bootstrap, font upload, and secondary window callback wiring.
- `app_runtime_bootstrap.{h,cpp}`: layer loading, registry/index discovery, persisted UI state load, derived-state initialization, and default selection setup.

### Runtime and background work

- `app_runtime_state.{h,cpp}`: typed runtime aggregates for filters, downloads, profiling, ownership, zoning, parcel-derived state, and UI/session state.
- `background_services_bootstrap.{h,cpp}`: hydration workers, spatial worker, parcel render cache worker, status API worker, dataset API worker, and LAN discovery worker.
- `app_shutdown_service.{h,cpp}`: stop flags, joins, persistence, queue drains, and teardown ordering.

### Frame loop

- `main_loop_service.{h,cpp}`: owns the main `while` loop and delegates frame work to the already-extracted modules.
- `frame_prelude.{h,cpp}`: frame-start command application, queue ticking, coverage refresh, and per-frame support contexts.
- `map_tab.{h,cpp}`: map window/tab orchestration.
- `map_frame_session.{h,cpp}`: frame-local map/filter/query/render session wiring.
- `map_frame_render.{h,cpp}`: frame render orchestration.
- `left_panel.{h,cpp}` and `right_panel.{h,cpp}`: side-panel composition.
- `layer_ui_state_sync.{h,cpp}`: per-frame UI/persistence reconciliation.

## Runtime State Boundaries

The app shell should pass structured state, not hundreds of locals.

Recommended aggregates:

1. `AppRuntimeState`
2. `BackgroundServiceHandles`
3. `StartupRuntime`
4. `AppWindowRuntime`
5. `ApiCommandRuntime`
6. `DownloadRuntime`
7. `ProfilingRuntime`

The exact names can evolve, but the boundary should remain:

- bootstrap creates state
- services mutate only the state they own
- the main loop consumes typed contexts, not free-floating globals

## Service Contracts

### Startup preprocess contract

- Startup preprocess determines whether geometry/DuckDB artifacts are ready before the main UI starts.
- The main loop never performs startup artifact compilation itself.

### Window/bootstrap contract

- Window creation, font upload, and callback wiring are not frame-loop responsibilities.
- The main loop receives prepared windows and render contexts.

### Background services contract

- Worker startup and worker shutdown are symmetric and centrally owned.
- The main loop may drain completed work, but it does not own thread construction policy.

### Frame-loop contract

- Per-frame sequencing consumes prepared runtime state.
- The frame loop should not assemble the application's entire state graph inline.
- The frame loop coordinates modules; it does not absorb their responsibilities.

## Design Rule

When new app-shell work is added, it should land in one of these target services first:

1. startup preprocess
2. window/bootstrap
3. runtime bootstrap/state
4. background services
5. main loop sequencing
6. shutdown

If a change does not clearly belong to one of those services, define the boundary before expanding the coordinator.
