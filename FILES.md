# Files

## Entry And App Shell

- `main.cpp`: Minimal executable entry point. Delegates startup to `runWorldSim3App`.
- `worldsim_app.h`: Public declaration for the application runner.
- `worldsim_app.cpp`: Vulkan, swapchain, texture upload, tile-cache, screenshot, and frame present/render implementation.
- `worldsim_app_internal.h`: Internal declarations shared between the Vulkan implementation and the app run loop.
- `worldsim_app_run.cpp`: Compiled translation unit for `runWorldSim3App`, containing the full run-loop implementation.
- `worldsim_app_run_state.h`: Declaration-only shared run-state struct used for ongoing run-loop modularization.
- `app_lifecycle.cpp`: Compiled frame finalization and shutdown implementation, including profiling sample capture, settings persistence, worker joins, and renderer cleanup.
- `app_lifecycle.h`: Typed lifecycle contexts and lifecycle function declarations.

## App Settings And Utilities

- `app_settings.cpp`: Loads and saves persistent app-level settings such as validation and grayscale basemap mode.
- `app_settings.h`: App settings data structure and settings I/O declarations.
- `app_utils.cpp`: Shared app helpers for status text, tile math, text parsing, property formatting, TODO parsing, and URL opening.
- `app_utils.h`: Shared utility declarations and `BootstrapProgress`.
- `profiling.h`: Profiling sample and layer-profile data structures used by UI and status endpoints.
- `screenshot_state.h`: Shared screenshot request/result state synchronized between HTTP API and render thread.

## Data And Layer Runtime

- `types.h`: Core layer, feature, category, and geometry data types.
- `dataset_library.cpp`: Layer manifest loading and geographic hierarchy assembly for datasets.
- `dataset_library.h`: Dataset library and hierarchy declarations.
- `layer_runtime.cpp`: Layer pipeline status formatting and runtime state helpers.
- `layer_runtime.h`: Layer pipeline status/state declarations.
- `layer_workers.cpp`: Background hydration and triangulation worker implementations.
- `layer_workers.h`: Worker context and worker startup declarations.
- `layer_geometry.cpp`: Spatial index building/querying and world-geometry cache helpers.
- `layer_geometry.h`: Layer spatial index and cached geometry declarations.
- `layer_state_io.cpp`: Persistent per-layer UI state load/save implementation.
- `layer_state_io.h`: Per-layer UI state I/O declarations.
- `cache_io.cpp`: Hydration cache serialization helpers.
- `cache_io.h`: Hydration cache declarations.

## Map Data Semantics

- `feature_props.cpp`: Feature property normalization, stable coloring, and property access helpers.
- `feature_props.h`: Feature property helper declarations.
- `geo.cpp`: Geographic projection helpers.
- `geo.h`: Geo helper declarations.
- `zoning.cpp`: Zoning class metadata, labels, grouping, and self-test integration.
- `zoning.h`: Zoning helper declarations.
- `vacancy_overlay.cpp`: Vacancy/tax overlay color logic, derived vacancy cache writing, and vacancy self-test.
- `vacancy_overlay.h`: Vacancy overlay declarations.

## Heatmaps And Panels

- `heatmap_render.cpp`: Heatmap cell/raster generation for grid, KDE, splat/blur, hex, and multires methods.
- `heatmap_render.h`: Heatmap sample, cached-cell, raster, and render-data declarations.
- `time_cube.cpp`: Time Cube service, schema-aware indexing, caching, and JSON serialization.
- `time_cube.h`: Time Cube query/result/service declarations.
- `time_cube_panel.cpp`: ImGui Time Cube panel and async index-job UI.
- `time_cube_panel.h`: Time Cube panel context and draw declaration.
- `policy_panel.cpp`: ImGui policy hierarchy, people/pay table, treemap, sunburst, and source/pay schedule UI.
- `policy_panel.h`: Policy panel context, roster row, and visualization node declarations.
- `model_tabs_panel.cpp`: Lightweight analytical model tabs such as graph model, star schema, spatial index, uncertainty, risk, causal panel, and scenarios.
- `model_tabs_panel.h`: Visual model tab draw declaration.

## APIs And Networking

- `status_api.cpp`: Local status/control HTTP API, profiling endpoints, screenshot endpoint, layer controls, and Time Cube endpoint.
- `status_api.h`: Status API context and worker declaration.
- `dataset_lan_api.cpp`: LAN dataset API, P2P mailbox endpoints, and UDP discovery responder.
- `dataset_lan_api.h`: Dataset/LAN API context and worker declarations.
- `net_http_utils.cpp`: Small HTTP response helper implementation.
- `net_http_utils.h`: HTTP helper declaration.
- `arkavo_realtime_client.cpp`: Arkavo signaling client lifecycle, peer tracking, and message callbacks.
- `arkavo_realtime_client.h`: Arkavo realtime client API.
- `arkavo_signaling_transport_curl.cpp`: CURL-backed Arkavo signaling transport.
- `arkavo_signaling_transport_curl.h`: Arkavo signaling transport declaration.
- `arkavo_rtc_session_manager.cpp`: WebRTC session and data-channel management for peer file transfer.
- `arkavo_rtc_session_manager.h`: RTC session manager API.
- `arkavo_connectivity_test.cpp`: Standalone connectivity test executable for Arkavo signaling/RTC components.

## Documentation And Plans

- `README.md`: Project overview and usage documentation.
- `TODO.md`: Working checklist used by the in-app gear/source panel.
- `NEW_VISUAL_MODELS.md`: Visual model and heatmap implementation plan/options.
- `REFACTOR.md`: Refactor plan and progress notes for decomposing the original monolithic app.
- `layers_plan.md`: Dataset/layer planning notes.
- `FILES.md`: This file; describes the purpose of each authored top-level file.

## Third Party And Generated-Like Assets

- `stb_image.h`: Vendored image loader used for map tile PNG loading.

## Tooling

- `tools/check_main_size.sh`: Guard that verifies `main.cpp` stays below the configured line limit.
- `tools/check_file_sizes.sh`: Guard that verifies authored source/header files stay below 2000 lines, excluding build/data/vendor files.
