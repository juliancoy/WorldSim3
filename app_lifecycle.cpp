#include "app_lifecycle.h"

#include "layer_state_io.h"

#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_vulkan.h"

#include <algorithm>
#include <chrono>
#include <curl/curl.h>

void finalizeWorldSimFrame(FrameFinalizationContext& ctx) {
    ImGui::Render();
    ImDrawData* draw_data = ImGui::GetDrawData();
    if (draw_data->DisplaySize.x > 0.0f && draw_data->DisplaySize.y > 0.0f) {
        ctx.window_data->ClearValue.color.float32[0] = 0.95f;
        ctx.window_data->ClearValue.color.float32[1] = 0.95f;
        ctx.window_data->ClearValue.color.float32[2] = 0.96f;
        ctx.window_data->ClearValue.color.float32[3] = 1.00f;
        const auto present_prof_begin = std::chrono::steady_clock::now();
        FrameRender(ctx.window_data, draw_data);
        FramePresent(ctx.window_data);
        ctx.prof_present_ms_last->store(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - present_prof_begin).count(),
            std::memory_order_relaxed);
    }
    auto now_frame = std::chrono::steady_clock::now();
    double frame_ms = std::chrono::duration<double, std::milli>(now_frame - *ctx.last_frame_ts).count();
    *ctx.last_frame_ts = now_frame;
    if (*ctx.ema_frame_ms <= 0.0) *ctx.ema_frame_ms = frame_ms;
    else *ctx.ema_frame_ms = (1.0 - ctx.perf_alpha) * *ctx.ema_frame_ms + ctx.perf_alpha * frame_ms;
    double fps = (*ctx.ema_frame_ms > 0.0) ? (1000.0 / *ctx.ema_frame_ms) : 0.0;
    ctx.perf_frame_ms_last->store(frame_ms, std::memory_order_relaxed);
    ctx.perf_frame_ms_avg->store(*ctx.ema_frame_ms, std::memory_order_relaxed);
    ctx.perf_fps_avg->store(fps, std::memory_order_relaxed);
    ctx.prof_ui_ms_last->store(
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ctx.frame_begin).count(),
        std::memory_order_relaxed);
    ctx.prof_tiles_drawn_last->store(ctx.tiles_drawn_frame, std::memory_order_relaxed);
    ctx.prof_features_considered_last->store(ctx.features_considered_frame, std::memory_order_relaxed);
    ctx.prof_features_drawn_last->store(ctx.features_drawn_frame, std::memory_order_relaxed);
    ctx.prof_retired_textures->store(g_RetiredTextures.size(), std::memory_order_relaxed);
    ctx.prof_tile_cache_size->store(g_TileCache.size(), std::memory_order_relaxed);

    ProfileFrameSample sample;
    sample.frame_ms = frame_ms;
    sample.ui_total_ms = ctx.prof_ui_ms_last->load(std::memory_order_relaxed);
    sample.owner_aggregate_ms = ctx.prof_owner_ms_last->load(std::memory_order_relaxed);
    sample.tiles_ms = ctx.prof_tile_ms_last->load(std::memory_order_relaxed);
    sample.layers_ms = ctx.prof_layer_ms_last->load(std::memory_order_relaxed);
    sample.heatmap_ms = ctx.prof_heatmap_ms_last->load(std::memory_order_relaxed);
    sample.overlays_ms = ctx.prof_overlay_ms_last->load(std::memory_order_relaxed);
    sample.render_present_ms = ctx.prof_present_ms_last->load(std::memory_order_relaxed);
    sample.tiles_drawn = ctx.tiles_drawn_frame;
    sample.features_considered = ctx.features_considered_frame;
    sample.features_drawn_points = ctx.features_drawn_frame;
    sample.heat_samples = ctx.prof_heat_samples_last->load(std::memory_order_relaxed);
    sample.retired_textures = g_RetiredTextures.size();
    std::lock_guard<std::mutex> lk(*ctx.profile_mutex);
    (*ctx.profile_samples)[*ctx.profile_sample_pos] = sample;
    *ctx.profile_sample_pos = (*ctx.profile_sample_pos + 1) % ctx.profile_samples->size();
    *ctx.profile_sample_count = std::min(*ctx.profile_sample_count + 1, ctx.profile_samples->size());
}

void shutdownWorldSimApp(AppShutdownContext& ctx) {
    vkDeviceWaitIdle(g_Device);
    saveLayerUiState(
        *ctx.root,
        *ctx.layers,
        ctx.hover_inspector_enabled,
        ctx.hover_inspector_mode,
        ctx.zoning_zone_enabled,
        ctx.layer_fill_enabled,
        ctx.layer_hover_enabled,
        ctx.layer_inspect_enabled,
        ctx.layer_heatmap_enabled,
        ctx.layer_heatmap_max_zoom,
        ctx.layer_parcel_detail_min_zoom,
        ctx.layer_heatmap_use_gradient,
        ctx.layer_heatmap_algo,
        ctx.layer_normalize_mode,
        ctx.layer_heatmap_cell_px,
        ctx.layer_heatmap_bandwidth_px,
        ctx.layer_heatmap_blur_sigma_px,
        ctx.layer_heatmap_percentile_clip,
        ctx.layer_heatmap_zoom_adaptive_bandwidth,
        ctx.layer_heatmap_multires_enabled,
        ctx.layer_heatmap_multires_blend,
        ctx.heatmap_algo,
        ctx.heatmap_quality_preset,
        ctx.global_heat_cell_px,
        ctx.heatmap_bandwidth_px,
        ctx.heatmap_blur_sigma_px,
        ctx.heatmap_percentile_clip,
        ctx.heatmap_zoom_adaptive_bandwidth,
        ctx.heatmap_multires_enabled,
        ctx.heatmap_multires_blend,
        ctx.heatmap_allow_cpu_fallback);
    ctx.app_settings->vulkan_validation_enabled = g_EnableValidationLayers;
    saveAppSettings(*ctx.root, *ctx.app_settings);
    ctx.hydration_stop->store(true, std::memory_order_relaxed);
    if (ctx.time_cube_ui_worker->joinable()) ctx.time_cube_ui_worker->join();
    ctx.hydrate_req_cv->notify_all();
    ctx.tri_cv->notify_all();
    for (auto& t : *ctx.hydration_workers) if (t.joinable()) t.join();
    if (ctx.triangulation_worker->joinable()) ctx.triangulation_worker->join();
    if (ctx.status_api_worker->joinable()) ctx.status_api_worker->join();
    if (ctx.dataset_api_worker->joinable()) ctx.dataset_api_worker->join();
    if (ctx.lan_discovery_worker->joinable()) ctx.lan_discovery_worker->join();
    destroyTileTextureNow(*ctx.heatmap_raster_texture);
    drainRetiredTextures(true);
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    CleanupVulkanWindow();
    CleanupVulkan();
    glfwDestroyWindow(ctx.window);
    glfwTerminate();
    curl_global_cleanup();
}
