# Run Loop Migration Notes

The legacy include split is gone. The remaining frame orchestration now lives in `app_main_loop.cpp`, and the extracted subsystems below remain in normal C++ modules:

- `profiling_layer_snapshot.h/.cpp`: refreshes `LayerProfileSnapshot` entries from layer geometry and spatial index state.
- `basemap_coverage.h/.cpp`: counts cached Baltimore-area raster tile coverage for OSM/topographic basemaps.
- `net_http_utils.h/.cpp`: owns `urlEncodeComponent`, matching the existing URL decode/network utility home.
- `lan_discovery.h/.cpp`: owns UDP LAN peer scanning, `LanPeerInfo`, and the LAN peer scan/list UI.
- `layer_download_queue.h/.cpp`: owns layer download queue path/load/persist/pending/enqueue/start/tick logic through `LayerDownloadQueueContext`.
- `api_control_commands.h/.cpp`: applies REST/API map commands, layer API commands, and remote ImGui mouse/scroll injection through `ApiControlContext`.
- `data_library_panel.h/.cpp`: owns the `Data Library` window, including search-cache row rebuild logic and local cell/tooltip/freshness display helpers.
- `app_frame_support.h/.cpp`: owns frame layout sizing, text-scale hotkey handling, and per-frame hydration/triangulation progress snapshots.
- `zoning_filters_panel.h/.cpp`: owns the `Zoning Filters` panel drawing and returns whether filter state changed.
- `basemap_panel.h/.cpp`: owns the `Basemap Layers` panel, settings saves, vector contour download, and coverage display.
- `performance_stats_panel.h/.cpp`: owns the `Performance and Stats` window UI, with explicit callbacks for Arkavo actions and cache-clear execution.
- `layer_pipeline_drain.h/.cpp`: owns the per-frame hydration queue drain and triangulation result application logic.
- `derived_layer_caches.h/.cpp`: owns zoning/real-property/vacancy/tax derived cache rebuilds, unified parcel refresh, and related counters.
- `map_frame_render.h/.cpp`: owns the map-frame render pass orchestration for render-plan assembly, layer pass execution, heatmap frame execution, tail pass execution, and related per-frame profiling writes.
- `map_frame_session.h/.cpp`: owns the per-frame parcel-jurisdiction query refresh, feature-filter construction, render-session setup, render orchestration call, and map inspection dispatch.
- `owner_aggregates.h/.cpp`: owns owner-class override load/save, owner classification, owner aggregate rebuilds, filtered owner snapshot generation, and related cache invalidation bookkeeping.
- `left_panel.h/.cpp`: owns the `Layers and Controls` window, including left-column orchestration for basemap controls, layer-category controls, global visibility/download actions, and zoning filter composition.
- `layer_ui_state_sync.h/.cpp`: owns per-frame layer UI reconciliation, including changed-layer detection, newly-enabled hydration enqueueing, dependency coordination, and filter/UI state persistence.
- `frame_prelude.h/.cpp`: owns frame-start API command application, download queue/lazy tile queue ticking, basemap coverage refresh, and the assembly of reusable per-frame download/LAN/profile contexts.
- `performance_runtime_support.h/.cpp`: owns Arkavo connect/disconnect/send actions, cache-clear execution, and `Performance and Stats` window wiring to the existing stats panel.
- `right_panel.h/.cpp`: owns the `Record Filters` window, parcel-selection helpers, DuckDB auto-rebuild gating, gradient filter setup, and right-panel tab composition.
- `map_canvas_session.h/.cpp`: owns map canvas startup, hover target resolution, basemap rendering, overlay enablement, and projection-cache setup for the map tab.
- `map_tab.h/.cpp`: owns the full `Map` window/tab orchestration, including map-canvas setup, map-frame session execution, and overlay panel popup wiring.
- `app_main_loop.cpp`: now owns the top-level `runWorldSim3App` orchestration and the per-frame main loop that composes the extracted modules.

Dependency map for moved blocks:

- Frame layout/text scale: inputs `ImGuiIO`, `ui_left_panel_frac`, `ui_right_panel_frac`, `ui_text_scale`; outputs `FrameLayout` and updated persisted panel fractions/font scale.
- Layer profile snapshot: inputs `layers`, `layer_spatial`, `layer_profile_dirty`; outputs `layer_profile_snapshot` under `layer_profile_mutex` and clears dirty flags.
- Basemap coverage: inputs `root`, target tile directory, zoom range; outputs missing/total tile counts.
- Layer download queue: inputs layer list, queue state, LAN peer context, freshness/status vectors, hydration/local-file callbacks; outputs queue JSON, async download future state, status/freshness updates.
- LAN discovery: inputs protocol version and mutable peer/status state; outputs peer list, scan status, last scan timestamp, and peer summary UI.
- API control: inputs REST command atomics, layer command state, remote UI atomics; outputs map center/zoom changes, layer command effects, ImGui input events.
- Data Library UI: inputs layer list, coordinator callbacks/state, query/download controls, row caches, freshness state, and queue callbacks; outputs the full window UI plus visible row cache updates.
- Zoning filters panel: inputs app settings, zone group/order/count/color/metadata state; outputs setting saves and a changed flag.
- Basemap panel: inputs app settings, cached coverage, basemap/lazy download states, status string; outputs settings saves, optional contour download, status/dirty updates.
- Frame/pipeline support: inputs ImGui IO, panel fractions, font scale, atomic counts, queues, and timestamps; outputs frame layout plus pipeline progress snapshots and idle timer updates.
- Performance/stats UI: inputs frame/pipeline counters, Arkavo state, LAN discovery panel context, cache-clear UI state, and explicit action callbacks; outputs only UI state mutations plus callback invocation.
- Layer pipeline drain: inputs hydrated/triangulation queues, layer/state vectors, status/profile dirty state, request flags, counters, and a heap-trim callback; outputs appended features, queued triangulation jobs, applied triangle results, and state/counter updates.
- Derived layer caches: inputs layer features, zoning metadata/state, parcel/vacancy/tax join maps, unified parcel caches, and aggregate counters; outputs rebuilt lookup tables, per-feature derived vectors, unified parcel refreshes, and dirty/match counters.
- Map-frame render orchestration: inputs render/view state, filter callbacks, heatmap settings, layer vectors, projection/runtime state, parcel/zoning derived data, and profiling atomics; outputs render-plan execution, heat samples, heatmap cache/frame work, tail-pass overlays, and frame-level render counters.
- Map-frame session: inputs DuckDB analytics, parcel-jurisdiction state, filter/query state, render/view state, map interaction state, and render/inspection callbacks; outputs refreshed parcel jurisdiction result sets, the per-frame render invocation, and inspection/selection side effects.
- Owner aggregates: inputs owner override state, unified parcel records, selected owners, parcel/tax generation counters, and layer sizes; outputs persisted overrides, rebuilt owner aggregates, filtered vacancy/value snapshots, and dirty/cache state updates.
- Left panel: inputs left-column layout, app settings, layer/freshness/runtime state, basemap cache state, parcel-jurisdiction filters, and download/hydration callbacks; outputs the full `Layers and Controls` window plus visibility/filter mutations and missing-download summary counts for the data library window.
- Layer UI state sync: inputs layer enablement snapshots, changed UI flags, dependency/filter state, and hydration callbacks; outputs updated persisted UI/filter state, newly enabled hydration requests, refreshed last-enabled snapshots, and the derived vacant-layer-active flag.
- Frame prelude: inputs frame-local map state, API command atomics, download/freshness state, queue storage, and basemap coverage caches; outputs prepared frame contexts plus completed frame-start queue/API/coverage work.
- Performance runtime support: inputs Arkavo runtime state, cache-clear toggles, layer/pipeline queues, profiling counters, and cache-reset callbacks; outputs updated Arkavo/cache UI state and invokes the existing performance panel callbacks.
- Right panel: inputs UI layout, parcel selection state, DuckDB/query/filter state, owner aggregate state, histogram/vacancy counters, hydration queue state, and tab callbacks; outputs the full `Record Filters` window plus parcel-selection and DuckDB rebuild side effects.
- Map canvas session: inputs viewport state, basemap cache/download state, hover toggle state, layer vectors, and tile profiling counters; outputs prepared map-canvas state including hover results, overlay enablement, basemap draw work, and a ready projection cache.
- Map tab: inputs layout, map/view state, render/filter state, time-cube/policy overlay state, and profiling counters; outputs the full `Map` window/tab plus map-frame execution and overlay popup UI.

Behavior intentionally preserved:

- JSON queue schema and path remain `data/layer_download_queue.json` with `schema_version` and `queued[].file`.
- LAN discovery still broadcasts `WS3_DISCOVER_V1` on port `8789` and preserves the existing socket guards and timeout behavior.
- Download fallback order remains LAN peers first, then original layer source/import source.
- ImGui labels/IDs and user-facing strings were kept unchanged.
- Atomic memory orders used by API command consumption remain `std::memory_order_relaxed`.

Remaining debt:

- The old `worldsim_app_run_loop_part*.inc` files are removed, and the Arkavo/cache-clear support plus frame-start download/coverage orchestration are now extracted. `app_main_loop.cpp` is still a large coordinator, but the remaining code is mostly true top-level sequencing and state assembly.
- Several docs still describe the older migration path and may need another cleanup pass if the refactor keeps moving.
