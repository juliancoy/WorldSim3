#pragma once

#include <cstddef>
#include <string>

struct ProfileFrameSample {
    double frame_ms = 0.0;
    double ui_total_ms = 0.0;
    double event_poll_ms = 0.0;
    double imgui_new_frame_ms = 0.0;
    double frame_prelude_ms = 0.0;
    double left_panel_ms = 0.0;
    double aux_windows_ms = 0.0;
    double layer_ui_sync_ms = 0.0;
    double derived_caches_ms = 0.0;
    double feature_render_cache_ms = 0.0;
    double runtime_sync_ms = 0.0;
    double right_panel_ms = 0.0;
    double map_tab_ms = 0.0;
    double imgui_render_ms = 0.0;
    double owner_aggregate_ms = 0.0;
    double owner_filter_ms = 0.0;
    double tiles_ms = 0.0;
    double layers_ms = 0.0;
    double heatmap_ms = 0.0;
    double overlays_ms = 0.0;
    double render_present_ms = 0.0;
    size_t tiles_drawn = 0;
    size_t features_considered = 0;
    size_t features_drawn_points = 0;
    size_t owner_filter_candidates = 0;
    size_t owner_filter_matches = 0;
    size_t heat_samples = 0;
    size_t retired_textures = 0;
};

struct LayerProfileSnapshot {
    size_t index = 0;
    std::string name;
    std::string file;
    bool enabled = false;
    size_t features = 0;
    size_t rings = 0;
    size_t ring_points = 0;
    size_t triangle_indices = 0;
    size_t properties = 0;
    bool spatial_index_built = false;
    size_t spatial_index_cells = 0;
    size_t spatial_index_marks = 0;
};

struct LayerProfileAccumulator {
    size_t features = 0;
    size_t rings = 0;
    size_t ring_points = 0;
    size_t triangle_indices = 0;
    size_t properties = 0;
    bool spatial_index_built = false;
    size_t spatial_index_cells = 0;
    size_t spatial_index_marks = 0;
};
