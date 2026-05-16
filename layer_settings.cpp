#include "layer_settings.h"

#include "aggregate_visualization_strategies.h"
#include "layer_import.h"
#include "render_policy.h"
#include "layer_ui_actions.h"
#include "ui_primitives.h"
#include "worldsim_app_internal.h"

#include "imgui.h"

#include <algorithm>
#include <fstream>

#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

void drawLayerDisplaySettingsPopup(LayerSettingsPopupContext& ctx) {
    if (!ctx.shared || !ctx.layer || !ImGui::BeginPopup("layer_display_settings")) return;

    LayerUiSharedContext& shared = *ctx.shared;
    LayerDef& l = *ctx.layer;
    ImGui::TextUnformatted(l.name.c_str());
    ImGui::Separator();
    const fs::path version_meta_path = shared.root / "data" / "versions" / "metadata" / (ctx.local_layer_path.filename().string() + ".json");
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

    const bool can_track_update = !l.source_url.empty() || layerHasImportSource(l);
    ImGui::BeginDisabled(!can_track_update);
    if (ImGui::Button(ctx.local_layer_exists ? "Update (versioned)" : "Download (versioned)")) {
        downloadOrUpdateLayerVersioned({ctx.shared, ctx.idx, &l, ctx.local_layer_path}, ctx.local_layer_exists);
    }
    ImGui::SameLine();
    if (ImGui::Button("Check Update")) {
        checkLayerUpdateVersioned({ctx.shared, ctx.idx, &l, ctx.local_layer_path});
    }
    ImGui::EndDisabled();
    if (!can_track_update) {
        ImGui::TextDisabled("No direct downloadable layer URL or import source available for update checks.");
        if (!l.reference_url.empty()) ImGui::TextWrapped("Reference: %s", l.reference_url.c_str());
        for (const auto& url : l.source_urls) ImGui::TextWrapped("Source: %s", url.c_str());
    }
    ImGui::Separator();

    pushButtonPalette(ButtonPalette::Destructive);
    if (ImGui::Button("Delete Local Layer File")) {
        deleteLocalLayerFile({ctx.shared, ctx.idx, &l, ctx.local_layer_path});
    }
    ImGui::PopStyleColor(buttonPaletteColorCount(ButtonPalette::Destructive));
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted("Removes local file and resets layer to undownloaded state.");
        ImGui::EndTooltip();
    }
    ImGui::Separator();

    bool fill_flag = (ctx.idx < shared.layer_fill_enabled->size()) ? (*shared.layer_fill_enabled)[ctx.idx] : true;
    if (ImGui::Checkbox("Fill polygons", &fill_flag) && ctx.idx < shared.layer_fill_enabled->size()) {
        std::lock_guard<std::mutex> lk_fill(*shared.layer_fill_mutex);
        (*shared.layer_fill_enabled)[ctx.idx] = fill_flag;
        *shared.layer_fill_state_changed = true;
    }
    bool hover_flag = (ctx.idx < shared.layer_hover_enabled->size()) ? (*shared.layer_hover_enabled)[ctx.idx] : true;
    if (ImGui::Checkbox("Hover inspector", &hover_flag) && ctx.idx < shared.layer_hover_enabled->size()) {
        (*shared.layer_hover_enabled)[ctx.idx] = hover_flag;
        *shared.layer_hover_state_changed = true;
    }
    bool inspect_flag = (ctx.idx < shared.layer_inspect_enabled->size()) ? (*shared.layer_inspect_enabled)[ctx.idx] : true;
    if (ImGui::Checkbox("Click inspect", &inspect_flag) && ctx.idx < shared.layer_inspect_enabled->size()) {
        (*shared.layer_inspect_enabled)[ctx.idx] = inspect_flag;
        *shared.layer_inspect_state_changed = true;
    }

    const bool is_value_parcel_layer = l.scale == "parcel" && !l.heatmap_field.empty();
    ImGui::SeparatorText(is_value_parcel_layer ? "Value visualization" : "Heatmap");
    const char* layer_algo_items[] = {
        "None", "KDE (Gaussian)", "GPU Splat + Blur", "LOD Geometry", "Hex Binning", "Multi-res Pyramid", "Median Choropleth"
    };
    int layer_algo_ui = 0;
    const bool layer_heatmap_on = ctx.idx < shared.layer_heatmap_enabled->size() ? (*shared.layer_heatmap_enabled)[ctx.idx] : true;
    if (layer_heatmap_on && ctx.idx < shared.layer_heatmap_algo->size()) {
        const int layer_algo = (*shared.layer_heatmap_algo)[ctx.idx] < 0 ? shared.heatmap_algo : (*shared.layer_heatmap_algo)[ctx.idx];
        layer_algo_ui = aggregateLayerUiIndexFromAlgo(layer_algo);
    }
    if (ImGui::Combo("Aggregate method", &layer_algo_ui, layer_algo_items, IM_ARRAYSIZE(layer_algo_items)) && ctx.idx < shared.layer_heatmap_algo->size()) {
        if (ctx.idx < shared.layer_heatmap_enabled->size()) (*shared.layer_heatmap_enabled)[ctx.idx] = layer_algo_ui != 0;
        if (layer_algo_ui != 0) (*shared.layer_heatmap_algo)[ctx.idx] = aggregateAlgoFromLayerUiIndex(layer_algo_ui);
        *shared.layer_heatmap_state_changed = true;
    }
    const bool aggregate_none = layer_algo_ui == 0;
    HeatmapLayerPolicyContext policy_ctx;
    policy_ctx.layers = shared.layers;
    policy_ctx.layer_heatmap_enabled = shared.layer_heatmap_enabled;
    policy_ctx.layer_heatmap_algo = shared.layer_heatmap_algo;
    policy_ctx.layer_heatmap_max_zoom = shared.layer_heatmap_max_zoom;
    policy_ctx.layer_parcel_detail_min_zoom = shared.layer_parcel_detail_min_zoom;
    policy_ctx.zoom = ctx.zoom;
    policy_ctx.heatmap_algo = shared.heatmap_algo;
    const LayerDisplayPolicy display_policy = resolveLayerDisplayPolicy(policy_ctx, ctx.idx);
    const int resolved_layer_algo = display_policy.aggregate_algo;

    if (display_policy.mode == LayerDisplayMode::ParcelChoroplethDetail) ImGui::TextDisabled("Resolved display: parcel choropleth by %s", l.heatmap_field.c_str());
    else if (display_policy.mode == LayerDisplayMode::Aggregate || display_policy.mode == LayerDisplayMode::LodGeometry) ImGui::TextDisabled("Resolved display: aggregate %s", aggregateStrategyName(resolved_layer_algo));
    else ImGui::TextDisabled("Resolved display: per-feature fill");

    if (!l.heatmap_field.empty()) {
        const char* normalize_items[] = {"Absolute clipped range", "Layer percentile", "Group/Zoning percentile"};
        int normalize_mode = ctx.idx < shared.layer_normalize_mode->size() ? (*shared.layer_normalize_mode)[ctx.idx] : 0;
        normalize_mode = std::clamp(normalize_mode, 0, (int)IM_ARRAYSIZE(normalize_items) - 1);
        if (ImGui::Combo("Normalize", &normalize_mode, normalize_items, IM_ARRAYSIZE(normalize_items)) && ctx.idx < shared.layer_normalize_mode->size()) {
            (*shared.layer_normalize_mode)[ctx.idx] = normalize_mode;
            *shared.layer_heatmap_state_changed = true;
        }
    }

    if (is_value_parcel_layer) ImGui::TextDisabled("Parcel detail draws per-parcel value fill.");
    ImGui::BeginDisabled(aggregate_none);
    ImGui::Indent();
    if (resolved_layer_algo == kAggregateKdeGaussian) {
        *shared.layer_heatmap_state_changed |= shared.heatmap_input_float_enter("KDE bandwidth", (*shared.layer_heatmap_bandwidth_px)[ctx.idx], 2.0f, 96.0f, "%.1f");
        bool adaptive = (*shared.layer_heatmap_zoom_adaptive_bandwidth)[ctx.idx];
        if (ImGui::Checkbox("Adaptive KDE bandwidth", &adaptive)) {
            (*shared.layer_heatmap_zoom_adaptive_bandwidth)[ctx.idx] = adaptive;
            *shared.layer_heatmap_state_changed = true;
        }
        *shared.heatmap_controls_active |= ImGui::IsItemActive();
        *shared.layer_heatmap_state_changed |= shared.heatmap_input_float_enter("KDE clip", (*shared.layer_heatmap_percentile_clip)[ctx.idx], 50.0f, 100.0f, "%.0f");
    } else if (resolved_layer_algo == kAggregateGpuSplatBlur) {
        *shared.layer_heatmap_state_changed |= shared.heatmap_input_float_enter("Splat radius", (*shared.layer_heatmap_bandwidth_px)[ctx.idx], 2.0f, 96.0f, "%.1f");
        *shared.layer_heatmap_state_changed |= shared.heatmap_input_float_enter("Blur sigma", (*shared.layer_heatmap_blur_sigma_px)[ctx.idx], 0.0f, 32.0f, "%.1f");
        bool adaptive = (*shared.layer_heatmap_zoom_adaptive_bandwidth)[ctx.idx];
        if (ImGui::Checkbox("Adaptive splat radius", &adaptive)) {
            (*shared.layer_heatmap_zoom_adaptive_bandwidth)[ctx.idx] = adaptive;
            *shared.layer_heatmap_state_changed = true;
        }
        if (shared.heatmap_allow_cpu_fallback) {
            bool allow_fallback = *shared.heatmap_allow_cpu_fallback;
            if (ImGui::Checkbox("Allow CPU fallback if GPU path fails", &allow_fallback)) {
                *shared.heatmap_allow_cpu_fallback = allow_fallback;
                *shared.layer_heatmap_state_changed = true;
            }
            if (!allow_fallback) {
                ImGui::TextDisabled("GPU path is required. Aggregate is skipped until GPU succeeds.");
            }
        }
        *shared.heatmap_controls_active |= ImGui::IsItemActive();
    } else if (resolved_layer_algo == kAggregateLodGeometry) {
        ImGui::TextWrapped("LOD Geometry draws simplified parcel/polygon geometry directly instead of generating heatmap cells.");
    } else if (resolved_layer_algo == kAggregateHexBinning) {
        *shared.layer_heatmap_state_changed |= shared.heatmap_input_float_enter("Hex size", (*shared.layer_heatmap_cell_px)[ctx.idx], 2.0f, 80.0f, "%.0f");
        *shared.layer_heatmap_state_changed |= shared.heatmap_input_float_enter("Hex clip", (*shared.layer_heatmap_percentile_clip)[ctx.idx], 50.0f, 100.0f, "%.0f");
    } else if (resolved_layer_algo == kAggregateMedianChoropleth) {
        *shared.layer_heatmap_state_changed |= shared.heatmap_input_float_enter("Choropleth cell size", (*shared.layer_heatmap_cell_px)[ctx.idx], 2.0f, 80.0f, "%.0f");
        *shared.layer_heatmap_state_changed |= shared.heatmap_input_float_enter("Value clip", (*shared.layer_heatmap_percentile_clip)[ctx.idx], 50.0f, 100.0f, "%.0f");
    } else {
        *shared.layer_heatmap_state_changed |= shared.heatmap_input_float_enter("Fine cell size", (*shared.layer_heatmap_cell_px)[ctx.idx], 2.0f, 80.0f, "%.0f");
        bool multires_enabled = (*shared.layer_heatmap_multires_enabled)[ctx.idx];
        if (ImGui::Checkbox("Enable pyramid blend", &multires_enabled)) {
            (*shared.layer_heatmap_multires_enabled)[ctx.idx] = multires_enabled;
            *shared.layer_heatmap_state_changed = true;
        }
        *shared.heatmap_controls_active |= ImGui::IsItemActive();
        ImGui::BeginDisabled(!multires_enabled);
        *shared.layer_heatmap_state_changed |= shared.heatmap_input_float_enter("Pyramid blend", (*shared.layer_heatmap_multires_blend)[ctx.idx], 0.0f, 1.0f, "%.2f");
        ImGui::EndDisabled();
    }
    ImGui::Unindent();

    int hz = display_policy.aggregate_max_zoom;
    if (ImGui::SliderInt("Aggregate max zoom", &hz, kMinZoom, kMaxZoom) && ctx.idx < shared.layer_heatmap_max_zoom->size()) {
        (*shared.layer_heatmap_max_zoom)[ctx.idx] = hz;
        *shared.layer_heatmap_state_changed = true;
    }
    ImGui::EndDisabled();
    if (is_value_parcel_layer) {
        int pz = display_policy.configured_parcel_detail_min_zoom;
        if (ImGui::SliderInt("Parcel detail min zoom", &pz, kMinZoom, kMaxZoom) && ctx.idx < shared.layer_parcel_detail_min_zoom->size()) {
            (*shared.layer_parcel_detail_min_zoom)[ctx.idx] = pz;
            *shared.layer_heatmap_state_changed = true;
        }
        if (display_policy.aggregate_configured &&
            display_policy.effective_parcel_detail_min_zoom != display_policy.configured_parcel_detail_min_zoom) {
            ImGui::TextDisabled("Effective parcel detail starts at zoom %d to avoid an aggregate/detail gap.", display_policy.effective_parcel_detail_min_zoom);
        }
    }
    bool use_gradient = (ctx.idx < shared.layer_heatmap_use_gradient->size()) ? (*shared.layer_heatmap_use_gradient)[ctx.idx] : true;
    if (ImGui::Checkbox("Apply gradient colors", &use_gradient) && ctx.idx < shared.layer_heatmap_use_gradient->size()) {
        (*shared.layer_heatmap_use_gradient)[ctx.idx] = use_gradient;
        *shared.layer_heatmap_state_changed = true;
    }
    ImGui::EndPopup();
}
