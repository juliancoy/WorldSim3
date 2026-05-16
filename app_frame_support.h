#pragma once

#include "app_lifecycle.h"
#include "download_queue.h"
#include "imgui.h"
#include "layer_runtime.h"
#include "tiles.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

struct FrameLayout {
    float layout_w = 1.0f;
    float layout_h = 1.0f;
    float layout_margin = 12.0f;
    float layout_gap = 8.0f;
    float content_w = 1.0f;
    float left_panel_w = 1.0f;
    float right_panel_w = 1.0f;
    float map_w = 1.0f;
    float map_x = 1.0f;
    float main_panel_h = 1.0f;
};

FrameLayout updateFrameLayoutAndTextScale(
    ImGuiIO& frame_io,
    std::atomic<double>& ui_left_panel_frac,
    std::atomic<double>& ui_right_panel_frac,
    float& ui_text_scale);

struct PipelineProgressContext {
    size_t layer_count = 0;
    std::atomic<size_t>* hydrated_count = nullptr;
    std::atomic<size_t>* triangulated_count = nullptr;
    size_t* last_hydrated_seen = nullptr;
    size_t* last_triangulated_seen = nullptr;
    std::chrono::steady_clock::time_point* last_hydration_progress_at = nullptr;
    std::chrono::steady_clock::time_point* last_tri_progress_at = nullptr;
    std::chrono::steady_clock::time_point hydration_started_at{};
    std::mutex* hydrated_mutex = nullptr;
    std::deque<HydratedLayer>* hydrated_queue = nullptr;
    std::mutex* tri_mutex = nullptr;
    std::deque<TriJob>* tri_jobs = nullptr;
};

struct PipelineProgressSnapshot {
    size_t hydrated_now = 0;
    size_t triangulated_now = 0;
    size_t hydrated_pending = 0;
    size_t tri_pending = 0;
    float hydrated_frac = 1.0f;
    float tri_frac = 1.0f;
    double elapsed_s = 0.0;
    double hydrate_idle_s = 0.0;
    double tri_idle_s = 0.0;
};

PipelineProgressSnapshot updatePipelineProgress(PipelineProgressContext& ctx);

struct FrameSupportFinalizationContext {
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
    bool dark_mode = false;
};

void finalizeFrameSupport(const FrameSupportFinalizationContext& ctx);

struct SecondaryDownloadQueueWindowContext {
    GLFWwindow* window = nullptr;
    bool* swapchain_rebuild = nullptr;
    int* framebuffer_w = nullptr;
    int* framebuffer_h = nullptr;
    ImGuiContext* queue_imgui_context = nullptr;
    ImGuiContext* main_imgui_context = nullptr;
    ImGui_ImplVulkanH_Window* window_data = nullptr;
    float ui_text_scale = 1.0f;
    DownloadQueueState* basemap_download = nullptr;
    const std::filesystem::path* root = nullptr;
    std::string* data_library_status_msg = nullptr;
    std::function<std::string(const std::string&)> resolve_download_label;
    LazyTileDownloadState* lazy_tile_download = nullptr;
    bool layer_download_inflight = false;
    size_t layer_download_active_idx = (size_t)-1;
    const std::vector<LayerDef>* layers = nullptr;
    const std::deque<size_t>* layer_download_queue = nullptr;
    const std::string* layer_download_last_event = nullptr;
    bool dark_mode = false;
};

void renderSecondaryDownloadQueueWindow(const SecondaryDownloadQueueWindowContext& ctx);
