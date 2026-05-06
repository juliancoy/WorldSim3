#pragma once

#include "app_settings.h"
#include "heatmap_render.h"
#include "layer_runtime.h"
#include "profiling.h"
#include "types.h"
#include "worldsim_app_internal.h"

#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct FrameFinalizationContext {
    ImGui_ImplVulkanH_Window* window_data = nullptr;
    std::chrono::steady_clock::time_point* last_frame_ts = nullptr;
    double* ema_frame_ms = nullptr;
    double perf_alpha = 0.12;
    std::chrono::steady_clock::time_point frame_begin;
    size_t tiles_drawn_frame = 0;
    size_t features_considered_frame = 0;
    size_t features_drawn_frame = 0;
    std::atomic<double>* perf_frame_ms_last = nullptr;
    std::atomic<double>* perf_frame_ms_avg = nullptr;
    std::atomic<double>* perf_fps_avg = nullptr;
    std::atomic<double>* prof_ui_ms_last = nullptr;
    std::atomic<double>* prof_owner_ms_last = nullptr;
    std::atomic<double>* prof_tile_ms_last = nullptr;
    std::atomic<double>* prof_layer_ms_last = nullptr;
    std::atomic<double>* prof_heatmap_ms_last = nullptr;
    std::atomic<double>* prof_overlay_ms_last = nullptr;
    std::atomic<double>* prof_present_ms_last = nullptr;
    std::atomic<size_t>* prof_tiles_drawn_last = nullptr;
    std::atomic<size_t>* prof_features_considered_last = nullptr;
    std::atomic<size_t>* prof_features_drawn_last = nullptr;
    std::atomic<size_t>* prof_retired_textures = nullptr;
    std::atomic<size_t>* prof_tile_cache_size = nullptr;
    std::atomic<size_t>* prof_heat_samples_last = nullptr;
    std::mutex* profile_mutex = nullptr;
    std::vector<ProfileFrameSample>* profile_samples = nullptr;
    size_t* profile_sample_pos = nullptr;
    size_t* profile_sample_count = nullptr;
};

struct AppShutdownContext {
    std::filesystem::path* root = nullptr;
    AppSettings* app_settings = nullptr;
    GLFWwindow* window = nullptr;
    std::vector<LayerDef>* layers = nullptr;
    bool hover_inspector_enabled = true;
    std::unordered_map<std::string, bool>* zoning_zone_enabled = nullptr;
    std::vector<bool>* layer_fill_enabled = nullptr;
    std::vector<bool>* layer_hover_enabled = nullptr;
    std::vector<bool>* layer_inspect_enabled = nullptr;
    std::vector<bool>* layer_heatmap_enabled = nullptr;
    std::vector<int>* layer_heatmap_max_zoom = nullptr;
    std::vector<bool>* layer_heatmap_use_gradient = nullptr;
    std::vector<int>* layer_heatmap_algo = nullptr;
    std::vector<bool>* layer_heatmap_use_global_settings = nullptr;
    std::vector<float>* layer_heatmap_cell_px = nullptr;
    std::vector<float>* layer_heatmap_bandwidth_px = nullptr;
    std::vector<float>* layer_heatmap_blur_sigma_px = nullptr;
    std::vector<float>* layer_heatmap_percentile_clip = nullptr;
    std::vector<bool>* layer_heatmap_zoom_adaptive_bandwidth = nullptr;
    std::vector<bool>* layer_heatmap_multires_enabled = nullptr;
    std::vector<float>* layer_heatmap_multires_blend = nullptr;
    int* heatmap_algo = nullptr;
    int* heatmap_quality_preset = nullptr;
    float* global_heat_cell_px = nullptr;
    float* heatmap_bandwidth_px = nullptr;
    float* heatmap_blur_sigma_px = nullptr;
    float* heatmap_percentile_clip = nullptr;
    bool* heatmap_zoom_adaptive_bandwidth = nullptr;
    bool* heatmap_multires_enabled = nullptr;
    float* heatmap_multires_blend = nullptr;
    std::atomic<bool>* hydration_stop = nullptr;
    std::thread* time_cube_ui_worker = nullptr;
    std::condition_variable* hydrate_req_cv = nullptr;
    std::condition_variable* tri_cv = nullptr;
    std::vector<std::thread>* hydration_workers = nullptr;
    std::thread* triangulation_worker = nullptr;
    std::thread* status_api_worker = nullptr;
    std::thread* dataset_api_worker = nullptr;
    std::thread* lan_discovery_worker = nullptr;
    TileTexture* heatmap_raster_texture = nullptr;
};

void finalizeWorldSimFrame(FrameFinalizationContext& ctx);
void shutdownWorldSimApp(AppShutdownContext& ctx);
