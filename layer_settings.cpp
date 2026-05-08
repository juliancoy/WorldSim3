#include "layer_settings.h"

#include "aggregate_visualization_strategies.h"
#include "dataset_library.h"
#include "worldsim_app_internal.h"

#include "imgui.h"

#include <algorithm>
#include <fstream>

#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

void drawLayerDisplaySettingsPopup(LayerSettingsPopupContext& ctx) {
    if (!ImGui::BeginPopup("layer_display_settings")) return;

    LayerDef& l = *ctx.layer;
    ImGui::TextUnformatted(l.name.c_str());
    ImGui::Separator();
    const fs::path version_meta_path = ctx.root / "data" / "versions" / "metadata" / (ctx.local_layer_path.filename().string() + ".json");
    json version_meta = json::object();
    {
        std::ifstream in(version_meta_path);
        if (in) {
            try { in >> version_meta; } catch (...) { version_meta = json::object(); }
        }
    }
    const std::string meta_hash = version_meta.value("content_hash", std::string());
    const std::string meta_fetched_at = version_meta.value("fetched_at", std::string());
    const std::string meta_checked_at = version_meta.value("checked_at", std::string());
    const std::string meta_last_modified = version_meta.value("last_modified", std::string());
    const std::string meta_etag = version_meta.value("etag", std::string());

    ImGui::TextDisabled("Version hash: %s", meta_hash.empty() ? "-" : meta_hash.c_str());
    ImGui::TextDisabled("Fetched at: %s", meta_fetched_at.empty() ? "-" : meta_fetched_at.c_str());
    ImGui::TextDisabled("Last checked: %s", meta_checked_at.empty() ? "-" : meta_checked_at.c_str());
    if (!meta_last_modified.empty()) ImGui::TextDisabled("Source last-modified: %s", meta_last_modified.c_str());
    if (!meta_etag.empty()) ImGui::TextDisabled("Source etag: %s", meta_etag.c_str());

    const bool can_track_update = !l.source_url.empty();
    ImGui::BeginDisabled(!can_track_update);
    if (ImGui::Button(ctx.local_layer_exists ? "Update (versioned)" : "Download (versioned)")) {
        VersionedDownloadResult vd = downloadUrlVersioned(
            l.source_url,
            ctx.local_layer_path,
            ctx.root / "data" / "versions");
        if (vd.ok) {
            *ctx.data_library_status_msg = (vd.not_modified ? "Checked " : "Downloaded/updated ") + l.file + " (" + vd.message + ")";
            (*ctx.data_freshness_state)[ctx.idx] = FreshnessState::UpToDate;
            (*ctx.data_freshness_msg)[ctx.idx] = vd.message;
            ctx.mark_local_layer_exists(ctx.idx, true);
            ctx.enqueue_hydration(ctx.idx, true);
        } else {
            *ctx.data_library_status_msg = "Update failed for " + l.file + ": " + vd.message;
            (*ctx.data_freshness_state)[ctx.idx] = FreshnessState::Error;
            (*ctx.data_freshness_msg)[ctx.idx] = vd.message;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Check Update")) {
        FreshnessCheckResult cr = checkUrlFreshnessVersioned(
            l.source_url,
            ctx.local_layer_path,
            ctx.root / "data" / "versions");
        (*ctx.data_freshness_state)[ctx.idx] = cr.state;
        (*ctx.data_freshness_msg)[ctx.idx] = cr.message;
        *ctx.data_library_status_msg = "Checked " + l.file + ": " + cr.message;
    }
    ImGui::EndDisabled();
    if (!can_track_update) ImGui::TextDisabled("No source URL available for update checks.");
    ImGui::Separator();

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.16f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.24f, 0.20f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.44f, 0.12f, 0.10f, 1.0f));
    if (ImGui::Button("Delete Local Layer File")) {
        std::error_code rm_ec;
        const bool removed = fs::remove(ctx.local_layer_path, rm_ec);
        if (removed || !fs::exists(ctx.local_layer_path)) {
            l.enabled = false;
            l.features.clear();
            if (ctx.idx < ctx.layer_spatial->size()) (*ctx.layer_spatial)[ctx.idx] = LayerSpatialIndex{};
            {
                std::lock_guard<std::mutex> lk_status(*ctx.status_mutex);
                if (ctx.idx < ctx.layer_states->size()) {
                    (*ctx.layer_states)[ctx.idx].status = LayerPipelineStatus::Queued;
                    (*ctx.layer_states)[ctx.idx].feature_count = 0;
                    (*ctx.layer_states)[ctx.idx].error.clear();
                }
            }
            ctx.mark_local_layer_exists(ctx.idx, false);
            if (ctx.idx < ctx.data_freshness_state->size()) {
                (*ctx.data_freshness_state)[ctx.idx] = l.source_url.empty() ? FreshnessState::NotTrackable : FreshnessState::Unknown;
            }
            if (ctx.idx < ctx.data_freshness_msg->size()) {
                (*ctx.data_freshness_msg)[ctx.idx] = l.source_url.empty() ? "no source URL" : "not downloaded";
            }
            *ctx.data_library_status_msg = "Deleted local layer file: " + l.file;
        } else {
            *ctx.data_library_status_msg = "Failed to delete " + l.file + ": " + rm_ec.message();
        }
    }
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted("Removes local file and resets layer to undownloaded state.");
        ImGui::EndTooltip();
    }
    ImGui::Separator();

    bool fill_flag = (ctx.idx < ctx.layer_fill_enabled->size()) ? (*ctx.layer_fill_enabled)[ctx.idx] : true;
    if (ImGui::Checkbox("Fill polygons", &fill_flag) && ctx.idx < ctx.layer_fill_enabled->size()) {
        std::lock_guard<std::mutex> lk_fill(*ctx.layer_fill_mutex);
        (*ctx.layer_fill_enabled)[ctx.idx] = fill_flag;
        *ctx.layer_fill_state_changed = true;
    }
    bool hover_flag = (ctx.idx < ctx.layer_hover_enabled->size()) ? (*ctx.layer_hover_enabled)[ctx.idx] : true;
    if (ImGui::Checkbox("Hover inspector", &hover_flag) && ctx.idx < ctx.layer_hover_enabled->size()) {
        (*ctx.layer_hover_enabled)[ctx.idx] = hover_flag;
        *ctx.layer_hover_state_changed = true;
    }
    bool inspect_flag = (ctx.idx < ctx.layer_inspect_enabled->size()) ? (*ctx.layer_inspect_enabled)[ctx.idx] : true;
    if (ImGui::Checkbox("Click inspect", &inspect_flag) && ctx.idx < ctx.layer_inspect_enabled->size()) {
        (*ctx.layer_inspect_enabled)[ctx.idx] = inspect_flag;
        *ctx.layer_inspect_state_changed = true;
    }

    const bool is_value_parcel_layer = l.scale == "parcel" && !l.heatmap_field.empty();
    ImGui::SeparatorText(is_value_parcel_layer ? "Value visualization" : "Heatmap");
    const char* layer_algo_items[] = {
        "None", "KDE (Gaussian)", "GPU Splat + Blur", "LOD Geometry", "Hex Binning", "Multi-res Pyramid", "Median Choropleth"
    };
    int layer_algo_ui = 0;
    const bool layer_heatmap_on = ctx.idx < ctx.layer_heatmap_enabled->size() ? (*ctx.layer_heatmap_enabled)[ctx.idx] : true;
    if (layer_heatmap_on && ctx.idx < ctx.layer_heatmap_algo->size()) {
        const int layer_algo = (*ctx.layer_heatmap_algo)[ctx.idx] < 0 ? ctx.heatmap_algo : (*ctx.layer_heatmap_algo)[ctx.idx];
        layer_algo_ui = aggregateLayerUiIndexFromAlgo(layer_algo);
    }
    if (ImGui::Combo("Aggregate method", &layer_algo_ui, layer_algo_items, IM_ARRAYSIZE(layer_algo_items)) && ctx.idx < ctx.layer_heatmap_algo->size()) {
        if (ctx.idx < ctx.layer_heatmap_enabled->size()) (*ctx.layer_heatmap_enabled)[ctx.idx] = layer_algo_ui != 0;
        if (layer_algo_ui != 0) (*ctx.layer_heatmap_algo)[ctx.idx] = aggregateAlgoFromLayerUiIndex(layer_algo_ui);
        *ctx.layer_heatmap_state_changed = true;
    }
    const bool aggregate_none = layer_algo_ui == 0;
    const int resolved_layer_algo = aggregate_none ? ctx.heatmap_algo : aggregateAlgoFromLayerUiIndex(layer_algo_ui);
    const int aggregate_max_zoom = ctx.idx < ctx.layer_heatmap_max_zoom->size() ? (*ctx.layer_heatmap_max_zoom)[ctx.idx] : 13;
    const int detail_min_zoom = ctx.idx < ctx.layer_parcel_detail_min_zoom->size() ? (*ctx.layer_parcel_detail_min_zoom)[ctx.idx] : kParcelChoroplethMinZoom;
    const bool aggregate_active_now = !aggregate_none && ctx.zoom <= aggregate_max_zoom && !(is_value_parcel_layer && ctx.zoom >= detail_min_zoom);

    if (aggregate_none) ImGui::TextDisabled("Resolved display: per-feature fill");
    else if (is_value_parcel_layer && ctx.zoom >= detail_min_zoom) ImGui::TextDisabled("Resolved display: parcel choropleth by %s", l.heatmap_field.c_str());
    else if (aggregate_active_now) ImGui::TextDisabled("Resolved display: aggregate %s", aggregateStrategyName(resolved_layer_algo));
    else ImGui::TextDisabled("Resolved display: per-feature fill");

    if (!l.heatmap_field.empty()) {
        const char* normalize_items[] = {"Absolute clipped range", "Layer percentile", "Group/Zoning percentile"};
        int normalize_mode = ctx.idx < ctx.layer_normalize_mode->size() ? (*ctx.layer_normalize_mode)[ctx.idx] : 0;
        normalize_mode = std::clamp(normalize_mode, 0, (int)IM_ARRAYSIZE(normalize_items) - 1);
        if (ImGui::Combo("Normalize", &normalize_mode, normalize_items, IM_ARRAYSIZE(normalize_items)) && ctx.idx < ctx.layer_normalize_mode->size()) {
            (*ctx.layer_normalize_mode)[ctx.idx] = normalize_mode;
            *ctx.layer_heatmap_state_changed = true;
        }
    }

    ImGui::BeginDisabled(aggregate_none);
    if (is_value_parcel_layer) ImGui::TextDisabled("Parcel detail draws per-parcel value fill.");
    ImGui::Indent();
    if (resolved_layer_algo == kAggregateKdeGaussian) {
        *ctx.layer_heatmap_state_changed |= ctx.heatmap_input_float_enter("KDE bandwidth", (*ctx.layer_heatmap_bandwidth_px)[ctx.idx], 2.0f, 96.0f, "%.1f");
        bool adaptive = (*ctx.layer_heatmap_zoom_adaptive_bandwidth)[ctx.idx];
        if (ImGui::Checkbox("Adaptive KDE bandwidth", &adaptive)) {
            (*ctx.layer_heatmap_zoom_adaptive_bandwidth)[ctx.idx] = adaptive;
            *ctx.layer_heatmap_state_changed = true;
        }
        *ctx.heatmap_controls_active |= ImGui::IsItemActive();
        *ctx.layer_heatmap_state_changed |= ctx.heatmap_input_float_enter("KDE clip", (*ctx.layer_heatmap_percentile_clip)[ctx.idx], 50.0f, 100.0f, "%.0f");
    } else if (resolved_layer_algo == kAggregateGpuSplatBlur) {
        *ctx.layer_heatmap_state_changed |= ctx.heatmap_input_float_enter("Splat radius", (*ctx.layer_heatmap_bandwidth_px)[ctx.idx], 2.0f, 96.0f, "%.1f");
        *ctx.layer_heatmap_state_changed |= ctx.heatmap_input_float_enter("Blur sigma", (*ctx.layer_heatmap_blur_sigma_px)[ctx.idx], 0.0f, 32.0f, "%.1f");
        bool adaptive = (*ctx.layer_heatmap_zoom_adaptive_bandwidth)[ctx.idx];
        if (ImGui::Checkbox("Adaptive splat radius", &adaptive)) {
            (*ctx.layer_heatmap_zoom_adaptive_bandwidth)[ctx.idx] = adaptive;
            *ctx.layer_heatmap_state_changed = true;
        }
        *ctx.heatmap_controls_active |= ImGui::IsItemActive();
    } else if (resolved_layer_algo == kAggregateLodGeometry) {
        ImGui::TextWrapped("LOD Geometry draws simplified parcel/polygon geometry directly instead of generating heatmap cells.");
    } else if (resolved_layer_algo == kAggregateHexBinning) {
        *ctx.layer_heatmap_state_changed |= ctx.heatmap_input_float_enter("Hex size", (*ctx.layer_heatmap_cell_px)[ctx.idx], 2.0f, 80.0f, "%.0f");
        *ctx.layer_heatmap_state_changed |= ctx.heatmap_input_float_enter("Hex clip", (*ctx.layer_heatmap_percentile_clip)[ctx.idx], 50.0f, 100.0f, "%.0f");
    } else if (resolved_layer_algo == kAggregateMedianChoropleth) {
        *ctx.layer_heatmap_state_changed |= ctx.heatmap_input_float_enter("Choropleth cell size", (*ctx.layer_heatmap_cell_px)[ctx.idx], 2.0f, 80.0f, "%.0f");
        *ctx.layer_heatmap_state_changed |= ctx.heatmap_input_float_enter("Value clip", (*ctx.layer_heatmap_percentile_clip)[ctx.idx], 50.0f, 100.0f, "%.0f");
    } else {
        *ctx.layer_heatmap_state_changed |= ctx.heatmap_input_float_enter("Fine cell size", (*ctx.layer_heatmap_cell_px)[ctx.idx], 2.0f, 80.0f, "%.0f");
        bool multires_enabled = (*ctx.layer_heatmap_multires_enabled)[ctx.idx];
        if (ImGui::Checkbox("Enable pyramid blend", &multires_enabled)) {
            (*ctx.layer_heatmap_multires_enabled)[ctx.idx] = multires_enabled;
            *ctx.layer_heatmap_state_changed = true;
        }
        *ctx.heatmap_controls_active |= ImGui::IsItemActive();
        ImGui::BeginDisabled(!multires_enabled);
        *ctx.layer_heatmap_state_changed |= ctx.heatmap_input_float_enter("Pyramid blend", (*ctx.layer_heatmap_multires_blend)[ctx.idx], 0.0f, 1.0f, "%.2f");
        ImGui::EndDisabled();
    }
    ImGui::Unindent();

    int hz = aggregate_max_zoom;
    if (ImGui::SliderInt("Aggregate max zoom", &hz, kMinZoom, kMaxZoom) && ctx.idx < ctx.layer_heatmap_max_zoom->size()) {
        (*ctx.layer_heatmap_max_zoom)[ctx.idx] = hz;
        *ctx.layer_heatmap_state_changed = true;
    }
    if (is_value_parcel_layer) {
        int pz = ctx.idx < ctx.layer_parcel_detail_min_zoom->size() ? (*ctx.layer_parcel_detail_min_zoom)[ctx.idx] : kParcelChoroplethMinZoom;
        if (ImGui::SliderInt("Parcel detail min zoom", &pz, kMinZoom, kMaxZoom) && ctx.idx < ctx.layer_parcel_detail_min_zoom->size()) {
            (*ctx.layer_parcel_detail_min_zoom)[ctx.idx] = pz;
            *ctx.layer_heatmap_state_changed = true;
        }
    }
    bool use_gradient = (ctx.idx < ctx.layer_heatmap_use_gradient->size()) ? (*ctx.layer_heatmap_use_gradient)[ctx.idx] : true;
    if (ImGui::Checkbox("Apply gradient colors", &use_gradient) && ctx.idx < ctx.layer_heatmap_use_gradient->size()) {
        (*ctx.layer_heatmap_use_gradient)[ctx.idx] = use_gradient;
        *ctx.layer_heatmap_state_changed = true;
    }
    ImGui::EndDisabled();
    ImGui::EndPopup();
}
