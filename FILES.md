# Files

## Entry And App Shell

- `main.cpp`: Minimal executable entry point. Delegates startup to `runWorldSim3App`.
- `worldsim_app.h`: Public declaration for the application runner.
- `worldsim_app.cpp`: Vulkan, swapchain, texture upload, tile-cache, screenshot, and frame present/render implementation.
- `worldsim_app_internal.h`: Internal declarations shared between the Vulkan implementation and the app run loop.
- `app_main_loop.cpp`: Compiled translation unit for `runWorldSim3App`, containing app state setup, the main frame loop, and top-level orchestration of the extracted runtime/UI/render modules.
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
- `selection.cpp`: Parcel multi-selection state mutation helpers.
- `selection.h`: Parcel selection state and helper declarations.
- `tiles.cpp`: Lazy persistent basemap tile download queue and tile-cache path helpers.
- `tiles.h`: Tile queue state and lazy tile helper declarations.
- `gear_panel.cpp`: ImGui gear/source panel with TODO work tabs, skipped download reporting, and heatmap settings guidance.
- `gear_panel.h`: Gear panel draw declaration.

## Data And Layer Runtime

- `types.h`: Core layer, feature, category, and geometry data types.
- `dataset_library.cpp`: Layer manifest loading and geographic hierarchy assembly for datasets.
- `dataset_library.h`: Dataset library and hierarchy declarations.
- `layer_import.cpp`: C++ source importer for non-GeoJSON layer sources, including zipped shapefile extraction, CRS conversion for county parcel imports, and Socrata CSV property-record conversion.
- `layer_import.h`: Layer import source detection and download/import entry points.
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
- `model_tabs_panel.cpp`: Orchestrates lightweight analytical model tabs such as graph model, star schema, spatial index, uncertainty, risk, causal panel, and scenarios.
- `model_tabs_panel.h`: Visual model tab draw declaration.
- `map_viewport.cpp`: ImGui map canvas input, camera zoom/pan math, viewport bounds calculation, and map context menu.
- `map_viewport.h`: Map viewport context/frame types and canvas setup declaration.
- `map_render_basemap.cpp`: Basemap render pass for OSM/topographic/satellite tiles, topo vector fallback, lazy tile requests, and basemap warning overlay.
- `map_render_basemap.h`: Basemap render context/result declarations.
- `map_render_hud.cpp`: Small map HUD badges such as zoom and aggregate-loading status.
- `map_render_hud.h`: Map HUD draw declarations.
- `map_render_selection.cpp`: Selected parcel outline render pass.
- `map_render_selection.h`: Selected parcel outline render context and declaration.
- `map_inspection.cpp`: Map click inspection behavior and parcel/zoning hover tooltip rendering.
- `map_inspection.h`: Map inspection context and handler declaration.
- `map_overlay_panels.cpp`: Map overlay popup shell for Time Cube, policy hierarchy, and visual model tabs.
- `map_overlay_panels.h`: Overlay popup draw declaration.
- `graph_model_tab.cpp`: ImGui Graph Model tab implementation.
- `graph_model_tab.h`: Graph Model tab draw declaration.
- `star_schema_tab.cpp`: ImGui Star Schema tab implementation.
- `star_schema_tab.h`: Star Schema tab draw declaration.
- `spatial_index_tab.cpp`: ImGui Spatial Index tab implementation.
- `spatial_index_tab.h`: Spatial Index tab draw declaration.
- `uncertainty_tab.cpp`: ImGui Uncertainty tab implementation.
- `uncertainty_tab.h`: Uncertainty tab draw declaration.
- `risk_scorecards_tab.cpp`: ImGui Risk Scorecards tab implementation.
- `risk_scorecards_tab.h`: Risk Scorecards tab draw declaration.
- `causal_panel_tab.cpp`: ImGui Causal Panel tab implementation.
- `causal_panel_tab.h`: Causal Panel tab draw declaration.
- `scenarios_tab.cpp`: ImGui Scenarios tab implementation.
- `scenarios_tab.h`: Scenarios tab draw declaration.
- `change_log_tab.cpp`: ImGui Change Log tab implementation.
- `change_log_tab.h`: Change Log tab draw declaration.
- `sql_tab.cpp`: ImGui SQL tab, DuckDB query editor, query execution controls, and selected-parcel context table.
- `sql_tab.h`: SQL tab context and draw declaration.
- `owner_info.cpp`: ImGui Owner Info tab, owner hyperlink rendering, and owner page navigation helpers.
- `owner_info.h`: Owner Info UI state, context, and draw declarations.
- `filters_tab.cpp`: ImGui Filters tab, address locate flow, zoning detail display, field filters, and record-year histogram UI.
- `filters_tab.h`: Filters tab context, address locate result type, and draw declaration.
- `parcel_info_tab.cpp`: ImGui Parcel Info tab and parcel real-property summary UI.
- `parcel_info_tab.h`: Parcel Info tab context and draw declaration.
- `vacancy_parcel_tab.cpp`: ImGui Vacancy-Parcel join quality tab.
- `vacancy_parcel_tab.h`: Vacancy-Parcel tab context and draw declaration.
- `gradient_tab.cpp`: ImGui Gradient scaling diagnostics tab.
- `gradient_tab.h`: Gradient tab context and draw declaration.
- `owners_tab.cpp`: ImGui Owners ranking/search/classification tab.
- `owners_tab.h`: Owner aggregate data types, filtered aggregate snapshot, Owners tab context, and draw declaration.

## UI Tab Extraction Status

Most record/analysis tabs now have their own `.cpp`.

- Extracted to dedicated `.cpp/.h`: `Filters`, `Parcel Info`, `SQL`, `Vacancy-Parcel`, `Gradient`, `Owner Info`, `Owners`, `Time Cube`, `Policy Hierarchy`, `Graph Model`, `Star Schema`, `Spatial Index`, `Uncertainty`, `Risk Scorecards`, `Causal Panel`, `Scenarios`, and `Change Log`.
- Partially extracted: `Map` viewport/input, basemap pass, HUD badges, selected outlines, inspection/tooltips, and overlay popup shell all live in dedicated modules.
- The `Map` tab shell and core layer/heatmap render orchestration now compose through `map_tab.cpp`, `map_frame_session.cpp`, and `map_frame_render.cpp`, with rendering split across `map_render_basemap`, `map_render_projection`, `map_render_hover`, `map_render_layers`, `map_render_overlays`, `map_render_heatmap_pass`, `map_render_selection`, `map_render_hud`, `map_inspection`, `map_overlay_panels`, and `map_render_utils`.
- Gear/source panel tabs live in `gear_panel.cpp/.h`.

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
- `worldsim_regional_parcel_builder`: Normalizes Baltimore City, Baltimore County, Howard County, and future county parcel/property GeoJSON inputs into `data/layers/regional_parcels.geojson`.
- `data/layers/regional_parcels.geojson`: Generated canonical regional parcel layer used by the app when present; rebuild from official source downloads rather than editing manually.
