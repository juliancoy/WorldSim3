#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct AppSettings {
    bool dark_mode = false;
    bool vulkan_validation_enabled = false;
    bool grayscale_basemap = false;
    bool basemap_osm_enabled = true;
    bool basemap_topographic_enabled = false;
    bool basemap_satellite_enabled = false;
    bool basemap_dark_satellite_enabled = false;
    bool basemap_night_satellite_enabled = false;
    float basemap_osm_opacity = 1.0f;
    float basemap_topographic_opacity = 1.0f;
    float basemap_satellite_opacity = 1.0f;
    float basemap_dark_satellite_opacity = 1.0f;
    float basemap_night_satellite_opacity = 1.0f;
    float map_polygon_fill_opacity = 170.0f / 255.0f;
    float map_polygon_outline_thickness = 2.0f;
    double zoom_step = 0.25;
    bool smooth_scroll_zoom = true;
    double scroll_zoom_distance = 0.25;
    double scroll_zoom_duration_s = 0.16;
    double alt_zoom_number_multiplier = 3.0;
    bool road_label_click_mode = false;
    float road_label_pick_tolerance_px = 12.0f;
    float road_label_size_scale = 1.0f;
    bool road_label_angle_along_road = true;
    bool road_label_avoid_overlap = true;
    int road_label_collision_mode = 1;
    float road_label_separation_px = 8.0f;
    bool topo_vector_enabled = false;
    bool zoning_use_simcity_colors = true;
    int reserve_cpu_cores = 0;
    std::string map_title_text;
    bool map_title_show_primary_parcel_source = false;
    std::string map_title_source_layer_file;
    std::vector<std::string> map_title_source_layer_files;
    bool map_title_all_caps = false;
    bool map_legend_show_overlay = false;
    int map_legend_overlay_position = 0;
    double ui_left_panel_frac = 0.34;
    double ui_right_panel_frac = 0.24;
    bool av_record_audio = true;
    int av_output_width = 1920;
    int av_output_height = 1080;
    int av_framerate = 60;
    int av_video_bitrate_mbps = 12;
    std::string av_audio_source;
    std::string av_encoder_name;
    std::string last_project_path;
};

AppSettings loadAppSettings(const std::filesystem::path& root, const AppSettings& defaults);
void saveAppSettings(const std::filesystem::path& root, const AppSettings& settings);
