#include "app_frame_support.h"

#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_vulkan.h"
#include "worldsim_app_internal.h"

#include <algorithm>
#include <cmath>

FrameLayout updateFrameLayoutAndTextScale(
    ImGuiIO& frame_io,
    std::atomic<double>& ui_left_panel_frac,
    std::atomic<double>& ui_right_panel_frac,
    float& ui_text_scale) {
    FrameLayout layout;
    layout.layout_w = std::max(1.0f, frame_io.DisplaySize.x);
    layout.layout_h = std::max(1.0f, frame_io.DisplaySize.y);
    layout.content_w = std::max(1.0f, layout.layout_w - layout.layout_margin * 2.0f - layout.layout_gap * 2.0f);
    double left_frac = std::clamp(ui_left_panel_frac.load(std::memory_order_relaxed), 0.08, 0.70);
    double right_frac = std::clamp(ui_right_panel_frac.load(std::memory_order_relaxed), 0.08, 0.50);
    if (left_frac + right_frac > 0.92) {
        const double scale = 0.92 / (left_frac + right_frac);
        left_frac *= scale;
        right_frac *= scale;
    }
    ui_left_panel_frac.store(left_frac, std::memory_order_relaxed);
    ui_right_panel_frac.store(right_frac, std::memory_order_relaxed);
    layout.left_panel_w = layout.content_w * (float)left_frac;
    layout.right_panel_w = layout.content_w * (float)right_frac;
    layout.map_w = std::max(1.0f, layout.content_w - layout.left_panel_w - layout.right_panel_w);
    layout.map_x = layout.left_panel_w + layout.layout_margin + layout.layout_gap;
    layout.main_panel_h = std::max(0.0f, layout.layout_h - layout.layout_margin * 2.0f);

    const bool text_scale_up =
        frame_io.KeyCtrl &&
        (ImGui::IsKeyPressed(ImGuiKey_Equal, false) ||
         ImGui::IsKeyPressed(ImGuiKey_KeypadAdd, false));
    const bool text_scale_down =
        frame_io.KeyCtrl &&
        (ImGui::IsKeyPressed(ImGuiKey_Minus, false) ||
         ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, false));
    if (text_scale_up || text_scale_down) {
        ui_text_scale = std::clamp(ui_text_scale + (text_scale_up ? 0.10f : -0.10f), 0.70f, 1.80f);
        frame_io.FontGlobalScale = ui_text_scale;
    } else if (std::abs(frame_io.FontGlobalScale - ui_text_scale) > 0.001f) {
        frame_io.FontGlobalScale = ui_text_scale;
    }
    return layout;
}

PipelineProgressSnapshot updatePipelineProgress(PipelineProgressContext& ctx) {
    PipelineProgressSnapshot out;
    if (!ctx.hydrated_count || !ctx.triangulated_count || !ctx.last_hydrated_seen ||
        !ctx.last_triangulated_seen || !ctx.last_hydration_progress_at ||
        !ctx.last_tri_progress_at || !ctx.hydrated_mutex || !ctx.hydrated_queue ||
        !ctx.tri_mutex || !ctx.tri_jobs) {
        return out;
    }

    out.hydrated_now = ctx.hydrated_count->load(std::memory_order_relaxed);
    out.triangulated_now = ctx.triangulated_count->load(std::memory_order_relaxed);
    if (out.hydrated_now > *ctx.last_hydrated_seen) {
        *ctx.last_hydrated_seen = out.hydrated_now;
        *ctx.last_hydration_progress_at = std::chrono::steady_clock::now();
    }
    if (out.triangulated_now > *ctx.last_triangulated_seen) {
        *ctx.last_triangulated_seen = out.triangulated_now;
        *ctx.last_tri_progress_at = std::chrono::steady_clock::now();
    }

    {
        std::lock_guard<std::mutex> lk(*ctx.hydrated_mutex);
        out.hydrated_pending = ctx.hydrated_queue->size();
    }
    {
        std::lock_guard<std::mutex> lk(*ctx.tri_mutex);
        out.tri_pending = ctx.tri_jobs->size();
    }

    out.hydrated_frac = ctx.layer_count == 0 ? 1.0f : (float)out.hydrated_now / (float)ctx.layer_count;
    out.tri_frac = ctx.layer_count == 0 ? 1.0f : (float)out.triangulated_now / (float)ctx.layer_count;
    const auto now = std::chrono::steady_clock::now();
    out.elapsed_s = std::chrono::duration<double>(now - ctx.hydration_started_at).count();
    out.hydrate_idle_s = std::chrono::duration<double>(now - *ctx.last_hydration_progress_at).count();
    out.tri_idle_s = std::chrono::duration<double>(now - *ctx.last_tri_progress_at).count();
    return out;
}

void finalizeFrameSupport(const FrameSupportFinalizationContext& ctx) {
    FrameFinalizationContext frame_ctx;
    frame_ctx.window_data = ctx.window_data;
    frame_ctx.last_frame_ts = ctx.last_frame_ts;
    frame_ctx.ema_frame_ms = ctx.ema_frame_ms;
    frame_ctx.perf_alpha = ctx.perf_alpha;
    frame_ctx.frame_begin = ctx.frame_begin;
    frame_ctx.tiles_drawn_frame = ctx.tiles_drawn_frame;
    frame_ctx.features_considered_frame = ctx.features_considered_frame;
    frame_ctx.features_drawn_frame = ctx.features_drawn_frame;
    frame_ctx.perf_frame_ms_last = ctx.perf_frame_ms_last;
    frame_ctx.perf_frame_ms_avg = ctx.perf_frame_ms_avg;
    frame_ctx.perf_fps_avg = ctx.perf_fps_avg;
    frame_ctx.prof_ui_ms_last = ctx.prof_ui_ms_last;
    frame_ctx.prof_owner_ms_last = ctx.prof_owner_ms_last;
    frame_ctx.prof_tile_ms_last = ctx.prof_tile_ms_last;
    frame_ctx.prof_layer_ms_last = ctx.prof_layer_ms_last;
    frame_ctx.prof_heatmap_ms_last = ctx.prof_heatmap_ms_last;
    frame_ctx.prof_overlay_ms_last = ctx.prof_overlay_ms_last;
    frame_ctx.prof_present_ms_last = ctx.prof_present_ms_last;
    frame_ctx.prof_tiles_drawn_last = ctx.prof_tiles_drawn_last;
    frame_ctx.prof_features_considered_last = ctx.prof_features_considered_last;
    frame_ctx.prof_features_drawn_last = ctx.prof_features_drawn_last;
    frame_ctx.prof_retired_textures = ctx.prof_retired_textures;
    frame_ctx.prof_tile_cache_size = ctx.prof_tile_cache_size;
    frame_ctx.prof_heat_samples_last = ctx.prof_heat_samples_last;
    frame_ctx.profile_mutex = ctx.profile_mutex;
    frame_ctx.profile_samples = ctx.profile_samples;
    frame_ctx.profile_sample_pos = ctx.profile_sample_pos;
    frame_ctx.profile_sample_count = ctx.profile_sample_count;
    frame_ctx.dark_mode = ctx.dark_mode;
    finalizeWorldSimFrame(frame_ctx);
}

void renderSecondaryDownloadQueueWindow(const SecondaryDownloadQueueWindowContext& ctx) {
    if (!ctx.window || !ctx.swapchain_rebuild || !ctx.framebuffer_w || !ctx.framebuffer_h ||
        !ctx.queue_imgui_context || !ctx.main_imgui_context || !ctx.window_data || !ctx.basemap_download ||
        !ctx.root || !ctx.data_library_status_msg || !ctx.lazy_tile_download || !ctx.layers ||
        !ctx.layer_download_queue || !ctx.layer_download_last_event) {
        return;
    }
    if (glfwWindowShouldClose(ctx.window)) return;

    if (*ctx.swapchain_rebuild) {
        glfwGetFramebufferSize(ctx.window, ctx.framebuffer_w, ctx.framebuffer_h);
        if (*ctx.framebuffer_w > 0 && *ctx.framebuffer_h > 0) {
            ImGui::SetCurrentContext(ctx.queue_imgui_context);
            std::lock_guard<std::mutex> qlk(g_QueueSubmitMutex);
            check_vk_result(vkDeviceWaitIdle(g_Device));
            ImGui_ImplVulkan_SetMinImageCount(g_MinImageCount);
            ImGui_ImplVulkanH_CreateOrResizeWindow(
                g_Instance,
                g_PhysicalDevice,
                g_Device,
                ctx.window_data,
                g_QueueFamily,
                g_Allocator,
                *ctx.framebuffer_w,
                *ctx.framebuffer_h,
                g_MinImageCount);
            ctx.window_data->FrameIndex = 0;
            *ctx.swapchain_rebuild = false;
        }
    }
    if (*ctx.swapchain_rebuild) return;

    ImGui::SetCurrentContext(ctx.queue_imgui_context);
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ImGuiIO& queue_io = ImGui::GetIO();
    queue_io.FontGlobalScale = ctx.ui_text_scale;
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(queue_io.DisplaySize, ImGuiCond_Always);
    if (ImGui::Begin(
            "Download Queue",
            nullptr,
            ImGuiWindowFlags_NoDecoration |
                ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoCollapse)) {
        drawBasemapDownloadQueueSection(*ctx.basemap_download, *ctx.root, *ctx.data_library_status_msg, ctx.resolve_download_label);
        drawLazyTileDownloadQueueSection(*ctx.lazy_tile_download);
        ImGui::SeparatorText("Layer Data");
        if (ctx.layer_download_inflight && ctx.layer_download_active_idx < ctx.layers->size()) {
            ImGui::Text("Active: %s", (*ctx.layers)[ctx.layer_download_active_idx].name.c_str());
            ImGui::TextDisabled("%s", (*ctx.layers)[ctx.layer_download_active_idx].file.c_str());
        } else {
            ImGui::TextDisabled("No active layer download");
        }
        if (ctx.layer_download_queue->empty()) {
            ImGui::TextDisabled("No queued layer downloads");
        } else {
            for (size_t i = 0; i < ctx.layer_download_queue->size(); ++i) {
                const size_t idx = (*ctx.layer_download_queue)[i];
                if (idx < ctx.layers->size()) ImGui::BulletText("%zu. %s", i + 1, (*ctx.layers)[idx].name.c_str());
            }
        }
        if (!ctx.layer_download_last_event->empty()) {
            ImGui::TextDisabled("Last queue event: %s", ctx.layer_download_last_event->c_str());
        }
    }
    ImGui::End();
    ImGui::Render();
    ImDrawData* queue_draw_data = ImGui::GetDrawData();
    if (queue_draw_data->DisplaySize.x > 0.0f && queue_draw_data->DisplaySize.y > 0.0f) {
        ctx.window_data->ClearValue.color.float32[0] = ctx.dark_mode ? 0.020f : 0.95f;
        ctx.window_data->ClearValue.color.float32[1] = ctx.dark_mode ? 0.026f : 0.95f;
        ctx.window_data->ClearValue.color.float32[2] = ctx.dark_mode ? 0.034f : 0.96f;
        ctx.window_data->ClearValue.color.float32[3] = 1.00f;
        FrameRenderSecondary(ctx.window_data, queue_draw_data, *ctx.swapchain_rebuild);
        FramePresentSecondary(ctx.window_data, *ctx.swapchain_rebuild);
    }
    ImGui::SetCurrentContext(ctx.main_imgui_context);
}
