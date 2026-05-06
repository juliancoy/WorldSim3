# WorldSim3 Refactor Plan: Keep Code Files Under 2000 Lines

## Objective

Refactor the codebase so every **first-party** code file is under 2000 lines, while preserving runtime behavior.

- Scope includes: `*.cpp`, `*.h`, `*.hpp`, `*.inc`, and Python tooling scripts.
- Third-party vendored code is tracked separately (see `stb_image.h` policy below).

## Implementation Progress

Completed so far:

1. `worldsim_cli.{h,cpp}` extracted from `worldsim_app_run.cpp` for CLI parsing/dispatch.
2. `worldsim_dataset_bootstrap.{h,cpp}` extracted for preload/download bootstrap behavior.
3. `worldsim_bootstrap.{h,cpp}` extracted for key layer index discovery.
4. `worldsim_app_run.cpp` reduced below 2000 lines using incremental extraction and include-unit decomposition.

## Current Over-Limit Files

Based on `python countlines.py` output:

1. `stb_image.h` (7988 lines) - vendored third-party file; do not hand-edit.

All first-party C++ and Python files are currently under 2000 lines.

## Refactor Policy (Best Practice)

1. Preserve behavior first; move code before changing logic.
2. Split by domain ownership, not arbitrary line chunks.
3. Minimize global state exposure by introducing context structs.
4. Keep public headers narrow; prefer internal/private headers for implementation details.
5. Add/keep automated guardrails (`tools/check_file_sizes.sh`) in CI.
6. Treat third-party code separately from first-party size policy.

## Third-Party File Policy (`stb_image.h`)

`stb_image.h` is vendored external code. Best practice is:

1. Keep it unmodified and pinned to a known upstream revision.
2. Move it under a clear vendor path (`third_party/stb/stb_image.h`) if desired.
3. Exclude vendored files from first-party line limits (already done in `tools/check_file_sizes.sh`).

This avoids carrying a custom fork and reduces upgrade risk.

## High-Level Extraction Strategy

Primary split target: `worldsim_app_run.cpp`.

### Phase 1: App Entry and Bootstrap

Move CLI parsing and startup/bootstrap orchestration out of `worldsim_app_run.cpp`.

### Phase 2: Frame Pipeline Decomposition

Split frame-loop responsibilities into dedicated units (input, simulation, render pass orchestration).

### Phase 3: UI Panel Orchestration

Move tab/panel wiring and per-panel state adapters into separate module files.

### Phase 4: Networking and Service Coordination

Extract realtime/session/LAN orchestration code paths from the run loop.

### Phase 5: Runtime Integration Cleanup

Centralize mutable runtime state into structured contexts and reduce cross-file coupling.

## New Files To Create

The following files should be introduced (professional naming, single-responsibility boundaries):

1. `worldsim_cli.h`
2. `worldsim_cli.cpp`
3. `worldsim_bootstrap.h`
4. `worldsim_bootstrap.cpp`
5. `worldsim_runtime_context.h`
6. `worldsim_frame_loop.h`
7. `worldsim_frame_loop.cpp`
8. `worldsim_render_loop.h`
9. `worldsim_render_loop.cpp`
10. `worldsim_ui_coordinator.h`
11. `worldsim_ui_coordinator.cpp`
12. `worldsim_network_coordinator.h`
13. `worldsim_network_coordinator.cpp`
14. `worldsim_dataset_bootstrap.h`
15. `worldsim_dataset_bootstrap.cpp`
16. `worldsim_command_dispatch.h`
17. `worldsim_command_dispatch.cpp`
18. `worldsim_shutdown.h`
19. `worldsim_shutdown.cpp`

## Existing Files Impacted By This Refactor

### Core build/runtime

1. `CMakeLists.txt` (add new compilation units)
2. `worldsim_app_run.cpp` (major reduction; orchestrator-only end state)
3. `worldsim_app.cpp` (ownership handoff for helper implementations, include cleanup)
4. `worldsim_app_internal.h` (replace broad globals/includes with refined contexts)
5. `worldsim_app.h` (public API remains small; verify unchanged contract)
6. `main.cpp` (no behavior change expected; include/path updates only if needed)

### App lifecycle and utilities

7. `app_lifecycle.cpp`
8. `app_lifecycle.h`
9. `app_utils.cpp`
10. `app_utils.h`
11. `app_settings.cpp`
12. `app_settings.h`

### Rendering/map/layers integration touchpoints

13. `layer_runtime.cpp`
14. `layer_runtime.h`
15. `layer_workers.cpp`
16. `layer_workers.h`
17. `layer_geometry.cpp`
18. `layer_geometry.h`
19. `heatmap_render.cpp`
20. `heatmap_render.h`
21. `time_cube.cpp`
22. `time_cube.h`
23. `time_cube_panel.cpp`
24. `time_cube_panel.h`

### Data and API/service integration

25. `dataset_library.cpp`
26. `dataset_library.h`
27. `dataset_lan_api.cpp`
28. `dataset_lan_api.h`
29. `status_api.cpp`
30. `status_api.h`
31. `net_http_utils.cpp`
32. `net_http_utils.h`
33. `cache_io.cpp`
34. `cache_io.h`

### UI/domain panel integration

35. `policy_panel.cpp`
36. `policy_panel.h`
37. `model_tabs_panel.cpp`
38. `model_tabs_panel.h`
39. `vacancy_overlay.cpp`
40. `vacancy_overlay.h`
41. `zoning.cpp`
42. `zoning.h`

### Realtime/session stack integration

43. `arkavo_realtime_client.cpp`
44. `arkavo_realtime_client.h`
45. `arkavo_rtc_session_manager.cpp`
46. `arkavo_rtc_session_manager.h`
47. `arkavo_signaling_transport_curl.cpp`
48. `arkavo_signaling_transport_curl.h`

### Optional vendor path cleanup

49. `stb_image.h` (only path/include relocation if moved under `third_party/`; no content edits)

## Target End State

1. All first-party code files are under 2000 lines.
2. `worldsim_app_run.cpp` becomes a thin orchestrator (target: 800-1500 lines).
3. App runtime responsibilities are split across bounded coordinator modules listed above.
4. Third-party vendored files remain excluded from first-party size policy.
5. `tools/check_file_sizes.sh` stays green locally and in CI.

## Verification Checklist Per Refactor PR

1. `cmake -S . -B build`
2. `cmake --build build -j`
3. `tools/check_file_sizes.sh`
4. Launch smoke test: `./build/worldsim3`
5. API smoke: `GET /status`, `GET /profile`
6. UI smoke: map render, layer toggles, policy panel, time cube panel

## Delivery Plan (PR Slicing)

1. PR 1: Add runtime context and CLI/bootstrap modules; no behavior changes.
2. PR 2: Extract frame/render loop coordinators.
3. PR 3: Extract UI coordinator and command dispatch.
4. PR 4: Extract network/dataset bootstrap/shutdown coordinators.
5. PR 5: Final cleanup, include minimization, and line-limit enforcement confirmation.

This sequence keeps risk controlled and every step reviewable.
