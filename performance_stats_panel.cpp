#include "performance_stats_panel.h"

void drawPerformanceStatsPanel(PerformanceStatsUiContext& ctx) {
    if (!ctx.arkavo_room_id || !ctx.arkavo_status || !ctx.arkavo_err ||
        !ctx.cache_clear || !ctx.lan_panel) {
        return;
    }

    ImGui::SetNextWindowPos(ImVec2(ctx.layout_margin, ctx.layout_margin + ctx.main_panel_h + ctx.layout_gap), ImGuiCond_Always);
    ImGui::SetNextWindowSize(
        ImVec2(ctx.left_panel_w, std::max(0.0f, ctx.layout_h - ctx.main_panel_h - ctx.layout_margin * 2.0f - ctx.layout_gap)),
        ImGuiCond_Always);
    ImGui::Begin("Performance and Stats", nullptr, ImGuiWindowFlags_NoCollapse);
    ImGui::Text(
        "Enabled layers ready: %zu / %zu (%.1f%%)",
        ctx.enabled_ready_now,
        ctx.enabled_layer_count,
        ctx.enabled_ready_frac * 100.0f);
    ImGui::ProgressBar(ctx.enabled_ready_frac, ImVec2(-1.0f, 0.0f));
    if (ctx.enabled_layer_count > 0 &&
        ctx.enabled_hydrated_now == ctx.enabled_layer_count &&
        ctx.enabled_ready_now < ctx.enabled_layer_count) {
        ImGui::TextDisabled("Enabled layer cache load complete; render preparation still running.");
    }
    ImGui::TextDisabled(
        "Enabled layers hydrated: %zu / %zu (%.1f%%)",
        ctx.enabled_hydrated_now,
        ctx.enabled_layer_count,
        ctx.enabled_hydrated_frac * 100.0f);
    ImGui::TextDisabled(
        "Global cached/loaded layers: %zu / %zu (%.1f%%) | Global ready layers: %zu / %zu (%.1f%%)",
        ctx.hydrated_now,
        ctx.layer_count,
        ctx.hydrated_frac * 100.0f,
        ctx.triangulated_now,
        ctx.layer_count,
        ctx.tri_frac * 100.0f);
    ImGui::TextDisabled("Background layer queue: %zu | Tri queue: %zu | elapsed: %.1fs", ctx.hydrated_pending, ctx.tri_pending, ctx.elapsed_s);
    ImGui::TextDisabled("Frame: %.2f ms (last %.2f) | FPS: %.1f", ctx.perf_frame_ms_avg, ctx.perf_frame_ms_last, ctx.perf_fps_avg);
    ImGui::TextDisabled(
        "UI rows: Data Library %zu/%zu (rebuilds %zu) | People & Pay %zu/%zu (rebuilds %zu)",
        ctx.data_library_rendered_rows_last,
        ctx.data_library_visible_rows,
        ctx.data_library_cache_rebuilds,
        ctx.people_pay_rendered_rows_last,
        ctx.people_pay_visible_rows,
        ctx.people_pay_cache_rebuilds);
    ImGui::TextDisabled(
        "Fill: %zu ok / %zu attempts | no tris %zu | bad idx %zu",
        ctx.render_fill_success_last_frame,
        ctx.render_fill_attempts_last_frame,
        ctx.render_fill_no_triangles_last_frame,
        ctx.render_fill_bad_indices_last_frame);
    ImGui::TextDisabled("API: http://127.0.0.1:8787/status");
    ImGui::TextDisabled("LAN Data API: http://0.0.0.0:8788/datasets");
    ImGui::TextDisabled("P2P signaling: /p2p/register /p2p/publish /p2p/poll");
    ImGui::SeparatorText("Arkavo Native Realtime");
    ImGui::InputText("Room ID", ctx.arkavo_room_id, ctx.arkavo_room_id_size);
    if (!ctx.arkavo_connected) {
        if (ImGui::Button("Connect Arkavo") && ctx.on_connect_arkavo) ctx.on_connect_arkavo();
    } else {
        if (ImGui::Button("Disconnect Arkavo") && ctx.on_disconnect_arkavo) ctx.on_disconnect_arkavo();
        ImGui::SameLine();
        ImGui::TextDisabled("connected=%s", ctx.arkavo_connected ? "yes" : "no");
        ImGui::TextDisabled("self peer: %s", ctx.arkavo_self_peer_id.empty() ? "(none)" : ctx.arkavo_self_peer_id.c_str());
        ImGui::TextDisabled("tracked peers: %zu", ctx.arkavo_tracked_peers);
        ImGui::TextDisabled("open data channels: %zu", ctx.arkavo_open_peers.size());
        ImGui::InputText("Send Peer", ctx.arkavo_send_peer, ctx.arkavo_send_peer_size);
        ImGui::InputText("Send File Path", ctx.arkavo_send_path, ctx.arkavo_send_path_size);
        if (ImGui::Button("Send Arkavo File") && ctx.on_send_arkavo_file) ctx.on_send_arkavo_file();
        if (!ctx.arkavo_open_peers.empty()) {
            ImGui::TextDisabled("open peers:");
            for (const auto& p : ctx.arkavo_open_peers) ImGui::TextDisabled("%s", p.c_str());
        }
    }
    ImGui::TextDisabled("status: %s", ctx.arkavo_status->c_str());
    if (!ctx.arkavo_err->empty()) ImGui::TextColored(ImVec4(0.85f, 0.35f, 0.2f, 1.0f), "arkavo: %s", ctx.arkavo_err->c_str());
    drawLanDiscoveryPanel(*ctx.lan_panel);
    if (ctx.enabled_hydrated_now < ctx.enabled_layer_count && ctx.hydrate_idle_s > 15.0) {
        ImGui::TextColored(ImVec4(0.85f, 0.35f, 0.2f, 1.0f), "Enabled-layer cache loading has not advanced for %.1fs", ctx.hydrate_idle_s);
    }
    if (ctx.triangulated_now < ctx.layer_count && ctx.tri_idle_s > 15.0) {
        ImGui::TextColored(ImVec4(0.85f, 0.35f, 0.2f, 1.0f), "Triangulation has not advanced for %.1fs", ctx.tri_idle_s);
    }
    ImGui::Separator();
    ImGui::TextDisabled("Tile cache: %zu / %zu", ctx.tile_cache_size, ctx.max_tile_cache);
    if (ImGui::TreeNode("Cache Clear")) {
        if (ImGui::Checkbox("All cache categories", &ctx.cache_clear->clear_cache_all)) {
            ctx.cache_clear->clear_cache_hydration = ctx.cache_clear->clear_cache_all;
            ctx.cache_clear->clear_cache_triangulation = ctx.cache_clear->clear_cache_all;
            ctx.cache_clear->clear_cache_derived = ctx.cache_clear->clear_cache_all;
            ctx.cache_clear->clear_cache_heatmap_memory = ctx.cache_clear->clear_cache_all;
            ctx.cache_clear->clear_cache_heatmap_disk = ctx.cache_clear->clear_cache_all;
            ctx.cache_clear->clear_cache_tile_memory = ctx.cache_clear->clear_cache_all;
            ctx.cache_clear->clear_cache_tile_disk_presence = ctx.cache_clear->clear_cache_all;
        }
        if (ImGui::BeginChild("cache_clear_children", ImVec2(0, 190), false, ImGuiWindowFlags_HorizontalScrollbar)) {
            if (ImGui::TreeNode("Data cache")) {
                ImGui::Checkbox("Hydration cache (on-disk)", &ctx.cache_clear->clear_cache_hydration);
                ImGui::Checkbox("Triangulation cache (on-disk)", &ctx.cache_clear->clear_cache_triangulation);
                ImGui::Checkbox("Derived cache (on-disk)", &ctx.cache_clear->clear_cache_derived);
                ImGui::Checkbox("Heatmap aggregate cache (on-disk)", &ctx.cache_clear->clear_cache_heatmap_disk);
                ImGui::TreePop();
            }
            if (ImGui::TreeNode("Runtime cache")) {
                ImGui::Checkbox("Heatmap aggregate cache (textures + raster meta)", &ctx.cache_clear->clear_cache_heatmap_memory);
                ImGui::Checkbox("Tile cache (GPU textures + queue + lru)", &ctx.cache_clear->clear_cache_tile_memory);
                ImGui::Checkbox("Tile existence memoization", &ctx.cache_clear->clear_cache_tile_disk_presence);
                ImGui::TreePop();
            }
            ImGui::EndChild();
        }
        const bool has_any_selected =
            ctx.cache_clear->clear_cache_hydration ||
            ctx.cache_clear->clear_cache_triangulation ||
            ctx.cache_clear->clear_cache_derived ||
            ctx.cache_clear->clear_cache_heatmap_memory ||
            ctx.cache_clear->clear_cache_heatmap_disk ||
            ctx.cache_clear->clear_cache_tile_memory ||
            ctx.cache_clear->clear_cache_tile_disk_presence;
        const bool all_children_selected =
            ctx.cache_clear->clear_cache_hydration &&
            ctx.cache_clear->clear_cache_triangulation &&
            ctx.cache_clear->clear_cache_derived &&
            ctx.cache_clear->clear_cache_heatmap_memory &&
            ctx.cache_clear->clear_cache_heatmap_disk &&
            ctx.cache_clear->clear_cache_tile_memory &&
            ctx.cache_clear->clear_cache_tile_disk_presence;
        ctx.cache_clear->clear_cache_all = has_any_selected && all_children_selected;
        ImGui::SeparatorText("Selected scopes");
        if (ImGui::Button("Clear Cache") && ctx.on_clear_cache) ctx.on_clear_cache();
        ImGui::TreePop();
    }
    if (!ctx.cache_clear->last_cache_clear_msg.empty()) ImGui::TextDisabled("%s", ctx.cache_clear->last_cache_clear_msg.c_str());
    ImGui::End();
}
