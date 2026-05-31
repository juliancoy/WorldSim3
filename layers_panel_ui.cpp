#include "layers_panel_ui.h"

#include "aggregate_visualization_strategies.h"
#include "app_utils.h"
#include "choropleth_histogram.h"
#include "feature_props.h"
#include "layer_settings.h"
#include "layer_ui_actions.h"
#include "map_render_utils.h"
#include "parcel_metrics.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <functional>
#include <iomanip>
#include <sstream>
#include <unordered_map>

namespace {
struct ClipboardToastState {
    std::string message;
    double expires_at = 0.0;
};

ClipboardToastState& clipboardToastState() {
    static ClipboardToastState state;
    return state;
}

void copyLayerNameToClipboard(const std::string& layer_name) {
    if (layer_name.empty()) return;
    ImGui::SetClipboardText(layer_name.c_str());
    ClipboardToastState& toast = clipboardToastState();
    toast.message = "Copied to Clipboard!";
    toast.expires_at = ImGui::GetTime() + 1.2;
}

void drawClipboardToastOverlay() {
    ClipboardToastState& toast = clipboardToastState();
    if (toast.message.empty()) return;
    const double now = ImGui::GetTime();
    if (now >= toast.expires_at) {
        toast.message.clear();
        return;
    }

    const ImVec2 window_pos = ImGui::GetWindowPos();
    const ImVec2 window_size = ImGui::GetWindowSize();
    const ImVec2 padding(12.0f, 8.0f);
    const ImVec2 text_size = ImGui::CalcTextSize(toast.message.c_str());
    const ImVec2 box_size(text_size.x + padding.x * 2.0f, text_size.y + padding.y * 2.0f);
    const ImVec2 box_min(
        window_pos.x + window_size.x - box_size.x - 18.0f,
        window_pos.y + 18.0f);
    const ImVec2 box_max(box_min.x + box_size.x, box_min.y + box_size.y);

    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(box_min, box_max, IM_COL32(18, 24, 20, 230), 6.0f);
    draw->AddRect(box_min, box_max, IM_COL32(96, 180, 120, 255), 6.0f, 0, 1.5f);
    draw->AddText(ImVec2(box_min.x + padding.x, box_min.y + padding.y), IM_COL32(232, 245, 236, 255), toast.message.c_str());
}

struct ColorCircleButtonResult {
    bool left_clicked = false;
    bool right_clicked = false;
};

ColorCircleButtonResult drawColorCircleButton(const char* id, const char* tooltip, const ImVec4& color) {
    ImGui::PushID(id);
    const float radius = 5.0f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size(radius * 2.0f + 6.0f, radius * 2.0f + 6.0f);
    ImGui::InvisibleButton("##color_circle", size);
    ColorCircleButtonResult result;
    result.left_clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    result.right_clicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 center(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f);
    draw->AddCircleFilled(center, radius, ImGui::ColorConvertFloat4ToU32(color), 16);
    draw->AddCircle(center, radius + 1.0f, IM_COL32(18, 22, 26, 235), 16, 1.5f);
    if (hovered) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(tooltip);
        ImGui::EndTooltip();
    }
    ImGui::PopID();
    return result;
}

bool alphaNearlyEquals(float a, float b) {
    return std::abs(a - b) <= 0.02f;
}

float nextOutlineAlphaForRightClick(ImGuiID button_id, float current_alpha) {
    static std::unordered_map<ImGuiID, int> cycle_index_by_button;
    constexpr std::array<float, 3> kCycle = {0.0f, 1.0f, 0.5f};

    auto reset_cycle_index = [&]() {
        if (alphaNearlyEquals(current_alpha, kCycle[0])) return 0;
        if (alphaNearlyEquals(current_alpha, kCycle[1])) return 1;
        if (alphaNearlyEquals(current_alpha, kCycle[2])) return 2;
        return -1;
    };

    auto it = cycle_index_by_button.find(button_id);
    if (it == cycle_index_by_button.end()) {
        if (alphaNearlyEquals(current_alpha, 1.0f)) {
            cycle_index_by_button[button_id] = 0;
            return kCycle[0];
        }
        const int reset_index = reset_cycle_index();
        const int next_index = reset_index >= 0 ? (reset_index + 1) % (int)kCycle.size() : 0;
        cycle_index_by_button[button_id] = next_index;
        return kCycle[(size_t)next_index];
    }

    const int reset_index = reset_cycle_index();
    if (reset_index >= 0 && !alphaNearlyEquals(current_alpha, kCycle[(size_t)it->second])) {
        it->second = reset_index;
    }
    it->second = (it->second + 1) % (int)kCycle.size();
    return kCycle[(size_t)it->second];
}

bool layerSupportsHoverSelection(const LayersPanelUiContext& ctx, size_t idx) {
    return ctx.shared &&
        ctx.shared->layer_hover_enabled &&
        ctx.active_hover_layer_idx &&
        idx < ctx.shared->layer_hover_enabled->size();
}

bool layerIsSelectedHoverTarget(const LayersPanelUiContext& ctx, size_t idx) {
    return layerSupportsHoverSelection(ctx, idx) && *ctx.active_hover_layer_idx == (int)idx;
}

void selectHoverTargetLayer(LayersPanelUiContext& ctx, size_t idx) {
    if (!layerSupportsHoverSelection(ctx, idx)) return;
    if (*ctx.active_hover_layer_idx == (int)idx) {
        *ctx.active_hover_layer_idx = -1;
        if (ctx.shared->layer_hover_state_changed) *ctx.shared->layer_hover_state_changed = true;
        return;
    }
    (*ctx.shared->layer_hover_enabled)[idx] = true;
    *ctx.active_hover_layer_idx = (int)idx;
    if (ctx.shared->layer_hover_state_changed) *ctx.shared->layer_hover_state_changed = true;
}

bool layerSupportsClickSelection(const LayersPanelUiContext& ctx, size_t idx) {
    if (!ctx.shared ||
        !ctx.shared->layers ||
        !ctx.shared->layer_inspect_enabled ||
        !ctx.active_click_layer_idx ||
        idx >= ctx.shared->layers->size()) {
        return false;
    }
    const LayerDef& layer = (*ctx.shared->layers)[idx];
    const bool parcel_interaction_layer =
        layer.enabled &&
        layer.scale == "parcel" &&
        !layerUsesPointGeometry(layer) &&
        !layerUsesPolylineGeometry(layer);
    return ctx.shared &&
        (parcel_interaction_layer || (int)idx == ctx.zoning_layer_idx);
}

bool layerIsSelectedClickTarget(const LayersPanelUiContext& ctx, size_t idx) {
    return layerSupportsClickSelection(ctx, idx) &&
        ctx.active_click_layer_idx &&
        *ctx.active_click_layer_idx == (int)idx;
}

void selectClickTargetLayer(LayersPanelUiContext& ctx, size_t idx) {
    if (!layerSupportsClickSelection(ctx, idx) || idx >= ctx.shared->layer_inspect_enabled->size()) return;
    (*ctx.shared->layer_inspect_enabled)[idx] = true;
    if (ctx.active_click_layer_idx) *ctx.active_click_layer_idx = (int)idx;
    if (ctx.shared->layer_inspect_state_changed) *ctx.shared->layer_inspect_state_changed = true;
}

std::string compactMoney(double value) {
    std::ostringstream os;
    if (value >= 1000000000.0) os << "$" << std::fixed << std::setprecision(1) << (value / 1000000000.0) << "B";
    else if (value >= 1000000.0) os << "$" << std::fixed << std::setprecision(1) << (value / 1000000.0) << "M";
    else if (value >= 1000.0) os << "$" << std::fixed << std::setprecision(0) << (value / 1000.0) << "K";
    else os << "$" << std::fixed << std::setprecision(0) << value;
    return os.str();
}

std::string compactNumber(double value) {
    std::ostringstream os;
    if (std::abs(value) >= 1000000000.0) os << std::fixed << std::setprecision(1) << (value / 1000000000.0) << "B";
    else if (std::abs(value) >= 1000000.0) os << std::fixed << std::setprecision(1) << (value / 1000000.0) << "M";
    else if (std::abs(value) >= 1000.0) os << std::fixed << std::setprecision(0) << (value / 1000.0) << "K";
    else os << std::fixed << std::setprecision(2) << value;
    return os.str();
}

std::string formatContinuousValue(std::string field, double value) {
    std::transform(field.begin(), field.end(), field.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    if (field.find("usd") != std::string::npos ||
        field.find("value") != std::string::npos ||
        field.find("amount") != std::string::npos ||
        field.find("cost") != std::string::npos ||
        field.find("price") != std::string::npos) {
        return compactMoney(value);
    }
    return compactNumber(value);
}

int parcelContinuousControlLayerIndex(const LayersPanelUiContext& ctx) {
    if (!ctx.shared || !ctx.shared->layers || ctx.parcel_layer_idx < 0) return -1;
    const int parameter_mode = ctx.shared->parcel_parameter_mode ? *ctx.shared->parcel_parameter_mode : 0;
    if (parameter_mode == 1) return ctx.parcel_layer_idx;
    if (parameter_mode == 2 || parameter_mode == 3) return findLayerFile(*ctx.shared, "property_value_parcels.geojson");
    for (size_t i = 0; i < ctx.shared->layers->size(); ++i) {
        if ((int)i == ctx.parcel_layer_idx) continue;
        const LayerDef& layer = (*ctx.shared->layers)[i];
        if (layer.scale == "parcel" && !layer.heatmap_field.empty() && layer.enabled) return (int)i;
    }
    return -1;
}

void drawHistogramPreview(const std::string& id, const std::string& field, const ApproxHistogram& hist, int normalize_mode, float gamma) {
    if (!hist.valid || hist.sample_count == 0 || hist.plot_bins.empty()) {
        ImGui::TextDisabled("No numeric samples loaded for this continuous source yet.");
        return;
    }
    ImGui::Text("Samples: %zu", hist.sample_count);
    ImGui::Text("Range: %s to %s", formatContinuousValue(field, hist.min_value).c_str(), formatContinuousValue(field, hist.max_value).c_str());
    ImGui::Text("Median: %s", formatContinuousValue(field, hist.median_value).c_str());
    ImGui::TextDisabled("Red ceiling after clip: %s", formatContinuousValue(field, hist.clipped_max_value).c_str());
    ImGui::InvisibleButton(id.c_str(), ImVec2(220.0f, 90.0f));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    draw->AddRectFilled(min, max, IM_COL32(18, 22, 28, 255), 4.0f);
    draw->AddRect(min, max, IM_COL32(70, 76, 84, 255), 4.0f);
    const size_t n = hist.plot_bins.size();
    if (n == 0 || hist.max_bin <= 0.0f) return;
    const float bin_w = (max.x - min.x) / (float)n;
    for (size_t i = 0; i < n; ++i) {
        const float h = std::clamp(hist.plot_bins[i] / (hist.max_bin * 1.05f), 0.0f, 1.0f);
        const float x0 = min.x + bin_w * (float)i;
        const float x1 = x0 + std::max(1.0f, bin_w - 1.0f);
        const float y1 = max.y - 1.0f;
        const float y0 = y1 - h * (max.y - min.y - 2.0f);
        const double value_center = hist.min_value + ((double)i + 0.5) * hist.bin_width;
        float t = 0.0f;
        if (normalize_mode == 0) t = hist.normalizeLinear(value_center);
        else if (normalize_mode == 3) t = hist.normalizeEqualCountZones(value_center);
        else t = hist.normalizeApproxPercentile(value_center);
        t = applyPowerGamma(t, gamma);
        draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), ImGui::ColorConvertFloat4ToU32(heatColor(t)));
    }
}

std::vector<double> collectNumericLayerValues(const LayerDef& layer) {
    std::vector<double> values;
    if (layer.heatmap_field.empty()) return values;
    values.reserve(layer.features.size());
    for (const auto& fg : layer.features) {
        float v = 0.0f;
        if (tryGetFeaturePropertyFloat(fg, layer.heatmap_field, v) && std::isfinite(v)) values.push_back((double)v);
    }
    return values;
}

std::vector<double> collectParcelAreaValues(const LayerDef& layer) {
    std::vector<double> values;
    values.reserve(layer.features.size());
    for (const auto& fg : layer.features) {
        if (fg.rings.empty()) continue;
        const double area = parcelAreaSqMFromFeature(fg);
        if (area > 0.0 && std::isfinite(area)) values.push_back(area);
    }
    return values;
}

std::vector<double> collectParcelValuePerAreaValues(
    const LayerDef& parcel_layer,
    const LayerDef& property_value_layer) {
    std::vector<double> values;
    const size_t n = std::min(parcel_layer.features.size(), property_value_layer.features.size());
    values.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const auto& parcel_fg = parcel_layer.features[i];
        if (parcel_fg.rings.empty()) continue;
        const double area = parcelAreaSqMFromFeature(parcel_fg);
        if (!(area > 0.0) || !std::isfinite(area)) continue;
        float v = 0.0f;
        if (!tryGetFeaturePropertyFloat(property_value_layer.features[i], property_value_layer.heatmap_field, v) || !std::isfinite(v) || v <= 0.0f) continue;
        values.push_back((double)v / area);
    }
    return values;
}

bool drawOutlineColorEditor(ImVec4& color) {
    bool changed = false;
    float rgba[4] = {color.x, color.y, color.z, color.w};
    if (ImGui::ColorPicker4("Outline color", rgba, ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_NoSidePreview)) {
        color = ImVec4(rgba[0], rgba[1], rgba[2], rgba[3]);
        changed = true;
    }
    return changed;
}

bool drawFillColorEditor(LayersPanelUiContext& ctx, size_t idx, LayerDef& layer) {
    bool changed = false;

    float rgba[4] = {layer.color.x, layer.color.y, layer.color.z, layer.color.w};
    if (ImGui::ColorPicker4("Static color", rgba, ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_NoSidePreview)) {
        layer.color = ImVec4(rgba[0], rgba[1], rgba[2], rgba[3]);
        changed = true;
    }

    const bool is_parcel_root_row = (int)idx == ctx.parcel_layer_idx;
    const bool has_layer_continuous = !layer.heatmap_field.empty();
    const int active_parcel_layer_idx = is_parcel_root_row ? parcelContinuousControlLayerIndex(ctx) : -1;
    const bool parcel_area_active = is_parcel_root_row && ctx.shared && ctx.shared->parcel_parameter_mode && *ctx.shared->parcel_parameter_mode == 1;
    const bool parcel_value_active = is_parcel_root_row && ctx.shared && ctx.shared->parcel_parameter_mode && *ctx.shared->parcel_parameter_mode == 2;
    const bool parcel_value_per_area_active = is_parcel_root_row && ctx.shared && ctx.shared->parcel_parameter_mode && *ctx.shared->parcel_parameter_mode == 3;
    const bool gradient_active =
        has_layer_continuous &&
        ctx.shared &&
        ctx.shared->layer_heatmap_use_gradient &&
        idx < ctx.shared->layer_heatmap_use_gradient->size()
            ? (*ctx.shared->layer_heatmap_use_gradient)[idx]
            : false;

    if (is_parcel_root_row || has_layer_continuous) {
        ImGui::SeparatorText("Color Mode");
    }

    if (has_layer_continuous) {
        bool static_selected = !gradient_active;
        if (ImGui::RadioButton("Static##static_color_mode", static_selected)) {
            if (ctx.shared && ctx.shared->layer_heatmap_use_gradient && idx < ctx.shared->layer_heatmap_use_gradient->size()) {
                (*ctx.shared->layer_heatmap_use_gradient)[idx] = false;
                if (ctx.shared->layer_heatmap_state_changed) *ctx.shared->layer_heatmap_state_changed = true;
            }
        }
        bool continuous_selected = gradient_active;
        if (ImGui::RadioButton(("Continuous: " + layer.heatmap_field + "##continuous_mode").c_str(), continuous_selected)) {
            if (ctx.shared && ctx.shared->layer_heatmap_use_gradient && idx < ctx.shared->layer_heatmap_use_gradient->size()) {
                (*ctx.shared->layer_heatmap_use_gradient)[idx] = true;
                if (ctx.shared->layer_heatmap_state_changed) *ctx.shared->layer_heatmap_state_changed = true;
            }
            if (ctx.shared && ctx.shared->layer_fill_enabled && idx < ctx.shared->layer_fill_enabled->size()) {
                (*ctx.shared->layer_fill_enabled)[idx] = true;
                if (ctx.shared->layer_fill_state_changed) *ctx.shared->layer_fill_state_changed = true;
            }
        }
    }

    if (is_parcel_root_row && ctx.shared && ctx.shared->layers) {
        const bool parcel_static = !parcel_area_active && !parcel_value_per_area_active && active_parcel_layer_idx < 0;
        if (ImGui::RadioButton("Static##parcel_static_color_mode", parcel_static)) {
            setParcelParameterMode(*ctx.shared, 0);
        }
        if (ImGui::RadioButton("Continuous: parcel area##parcel_area_color_mode", parcel_area_active)) {
            setParcelParameterMode(*ctx.shared, 1);
        }
        if (ImGui::RadioButton("Continuous: value per area##parcel_value_per_area_color_mode", parcel_value_per_area_active)) {
            activateParcelValuePerAreaMode(*ctx.shared);
        }
        for (size_t i = 0; i < ctx.shared->layers->size(); ++i) {
            LayerDef& parameter_layer = (*ctx.shared->layers)[i];
            const bool is_parameter_layer = ctx.shared->layer_registry
                ? ctx.shared->layer_registry->isParcelHeatmapLayer(i)
                : (parameter_layer.scale == "parcel" && !parameter_layer.heatmap_field.empty());
            if (!is_parameter_layer || (int)i == ctx.parcel_layer_idx) continue;
            const bool selected =
                (int)i == active_parcel_layer_idx ||
                ((parcel_value_active || parcel_value_per_area_active) && parameter_layer.file == "property_value_parcels.geojson");
            std::string option_label = "Continuous: " + parameter_layer.name + "##parcel_parameter_layer_" + std::to_string(i);
            if (ImGui::RadioButton(option_label.c_str(), selected)) {
                activateParameterLayer(*ctx.shared, (int)i);
                if (ctx.shared->layer_heatmap_use_gradient && i < ctx.shared->layer_heatmap_use_gradient->size()) {
                    (*ctx.shared->layer_heatmap_use_gradient)[i] = true;
                }
            }
        }
    }

    int control_layer_idx = -1;
    std::string histogram_field;
    std::vector<double> histogram_values;
    if (is_parcel_root_row) {
        if (parcel_area_active) {
            control_layer_idx = ctx.parcel_layer_idx;
            histogram_field = "parcel_area_sq_m";
            histogram_values = collectParcelAreaValues(layer);
        } else if (parcel_value_per_area_active && active_parcel_layer_idx >= 0 && (size_t)active_parcel_layer_idx < ctx.shared->layers->size()) {
            control_layer_idx = active_parcel_layer_idx;
            const LayerDef& active_layer = (*ctx.shared->layers)[(size_t)active_parcel_layer_idx];
            histogram_field = "value_per_area";
            histogram_values = collectParcelValuePerAreaValues(layer, active_layer);
        } else if (active_parcel_layer_idx >= 0 && (size_t)active_parcel_layer_idx < ctx.shared->layers->size()) {
            control_layer_idx = active_parcel_layer_idx;
            const LayerDef& active_layer = (*ctx.shared->layers)[(size_t)active_parcel_layer_idx];
            histogram_field = active_layer.heatmap_field;
            histogram_values = collectNumericLayerValues(active_layer);
        }
    } else if (has_layer_continuous && gradient_active) {
        control_layer_idx = (int)idx;
        histogram_field = layer.heatmap_field;
        histogram_values = collectNumericLayerValues(layer);
    }

    if (control_layer_idx >= 0 && ctx.shared) {
        ImGui::SeparatorText("Continuous Scale");
        if (ctx.shared->layer_normalize_mode && (size_t)control_layer_idx < ctx.shared->layer_normalize_mode->size()) {
            const char* normalize_items[] = {"Absolute clipped range", "Histogram percentile", "Group/Zoning percentile", "Equal-count color bands"};
            int normalize_mode = (*ctx.shared->layer_normalize_mode)[(size_t)control_layer_idx];
            normalize_mode = std::clamp(normalize_mode, 0, (int)IM_ARRAYSIZE(normalize_items) - 1);
            if (ImGui::Combo("Normalize", &normalize_mode, normalize_items, (int)IM_ARRAYSIZE(normalize_items))) {
                (*ctx.shared->layer_normalize_mode)[(size_t)control_layer_idx] = normalize_mode;
                if (ctx.shared->layer_heatmap_state_changed) *ctx.shared->layer_heatmap_state_changed = true;
            }
        }
        if (ctx.shared->layer_heatmap_percentile_clip && (size_t)control_layer_idx < ctx.shared->layer_heatmap_percentile_clip->size()) {
            float clip = (*ctx.shared->layer_heatmap_percentile_clip)[(size_t)control_layer_idx];
            if (ImGui::SliderFloat("Clip", &clip, 50.0f, 100.0f, "%.0f%%")) {
                (*ctx.shared->layer_heatmap_percentile_clip)[(size_t)control_layer_idx] = clip;
                if (ctx.shared->layer_heatmap_state_changed) *ctx.shared->layer_heatmap_state_changed = true;
            }
        }
        if (ctx.shared->layer_choropleth_gamma && (size_t)control_layer_idx < ctx.shared->layer_choropleth_gamma->size()) {
            float gamma = (*ctx.shared->layer_choropleth_gamma)[(size_t)control_layer_idx];
            if (ImGui::SliderFloat("Gamma", &gamma, 0.10f, 5.0f, "%.2f")) {
                (*ctx.shared->layer_choropleth_gamma)[(size_t)control_layer_idx] = gamma;
                if (ctx.shared->layer_heatmap_state_changed) *ctx.shared->layer_heatmap_state_changed = true;
            }
        }
        const float clip = ctx.shared->layer_heatmap_percentile_clip && (size_t)control_layer_idx < ctx.shared->layer_heatmap_percentile_clip->size()
            ? (*ctx.shared->layer_heatmap_percentile_clip)[(size_t)control_layer_idx]
            : 100.0f;
        const int normalize_mode = ctx.shared->layer_normalize_mode && (size_t)control_layer_idx < ctx.shared->layer_normalize_mode->size()
            ? std::clamp((*ctx.shared->layer_normalize_mode)[(size_t)control_layer_idx], 0, 3)
            : 0;
        const float gamma = ctx.shared->layer_choropleth_gamma && (size_t)control_layer_idx < ctx.shared->layer_choropleth_gamma->size()
            ? (*ctx.shared->layer_choropleth_gamma)[(size_t)control_layer_idx]
            : 1.0f;
        drawHistogramPreview("##continuous_histogram", histogram_field, buildApproxHistogram(histogram_values, clip), normalize_mode, gamma);
    }
    return changed;
}

bool drawFillColorCircleButton(const LayersPanelUiContext& ctx, size_t idx, const LayerDef& layer) {
    const ColorCircleButtonResult result = drawColorCircleButton("fill_color", "Fill color and continuous scale", layer.color);
    if (result.left_clicked && ctx.shared && ctx.shared->open_layer_color_editor) ctx.shared->open_layer_color_editor(idx, false);
    return result.left_clicked;
}

bool drawOutlineColorCircleButton(const LayersPanelUiContext& ctx, size_t idx, LayerDef& layer) {
    const ImGuiID button_id = ImGui::GetID("outline_color");
    const ColorCircleButtonResult result = drawColorCircleButton(
        "outline_color",
        "Outline color\nLeft click: open editor\nRight click: cycle alpha 0%, 100%, 50%",
        layer.outline_color);
    if (result.right_clicked) {
        layer.outline_color.w = nextOutlineAlphaForRightClick(button_id, layer.outline_color.w);
        if (ctx.shared && ctx.shared->layer_heatmap_state_changed) *ctx.shared->layer_heatmap_state_changed = true;
    }
    if (result.left_clicked && ctx.shared && ctx.shared->open_layer_color_editor) ctx.shared->open_layer_color_editor(idx, true);
    return result.left_clicked || result.right_clicked;
}

const char* geographicScopeLabel(const LayerDef& layer) {
    if (layer.scale == "parcel") return "Parcel / land unit";
    if (layer.scale == "building") return "Building / address";
    if (layerUsesPointGeometry(layer)) return "Point records";
    if (layer.scale == "tract") return "Census tract";
    if (layer.scale == "csa") return "Community / CSA";
    if (layer.scale == "regional") return "Regional / jurisdiction";
    if (!layer.scale.empty()) return layer.scale.c_str();
    return "Area / boundary";
}

const char* parcelJurisdictionForLayer(const LayerDef& layer) {
    static thread_local std::string jurisdiction;
    jurisdiction.clear();
    if (layer.name.size() > 16 && layer.name.ends_with(" County Parcels")) {
        jurisdiction = layer.name.substr(0, layer.name.size() - 8);
        return jurisdiction.c_str();
    }
    if (layer.name == "Baltimore City Parcels") {
        jurisdiction = layer.name.substr(0, layer.name.size() - 8);
        return jurisdiction.c_str();
    }
    return "";
}

LayerSettingsPopupContext makeLayerSettingsPopupContext(
    LayersPanelUiContext& ctx,
    size_t idx,
    LayerDef& layer,
    bool local_layer_exists);

bool drawParcelJurisdictionFilterRow(LayersPanelUiContext& ctx, size_t idx, LayerDef& layer);

bool layerVisibleInHierarchy(const LayersPanelUiContext& ctx, const LayerDef& layer);

bool categoryHasVisibleSubdata(const LayersPanelUiContext& ctx, LayerDef::Category cat) {
    if (!ctx.shared || !ctx.shared->layers) return false;
    for (const LayerDef& layer : *ctx.shared->layers) {
        if (layer.category != cat) continue;
        if (!layerVisibleInHierarchy(ctx, layer)) continue;
        if (layer.enabled) return true;
    }
    return false;
}

bool layerMatchesSearch(const LayersPanelUiContext& ctx, const LayerDef& layer) {
    const std::string query = ctx.layer_search_query ? trimDisplayValue(ctx.layer_search_query) : "";
    if (query.empty()) return true;
    const std::string scope = geographicScopeLabel(layer);
    return fuzzyTextMatches(layer.name, query) ||
           fuzzyTextMatches(layer.description, query) ||
           fuzzyTextMatches(layer.subcategory, query) ||
           fuzzyTextMatches(layer.region, query) ||
           fuzzyTextMatches(layerLogicalId(layer), query) ||
           fuzzyTextMatches(layer.file, query) ||
           fuzzyTextMatches(scope, query);
}

bool layerVisibleInHierarchy(const LayersPanelUiContext& ctx, const LayerDef& layer) {
    if (ctx.layer_browse_state && !layerMatchesBrowseGeography(layer, *ctx.layer_browse_state)) return false;
    return layerMatchesSearch(ctx, layer);
}

struct GroupControlState {
    bool has_rows = false;
    bool any_visible = false;
    bool hover_supported = false;
    bool hover_selected = false;
    bool click_supported = false;
    bool click_selected = false;
    int first_hover_idx = -1;
    int first_click_idx = -1;
};

struct SourceHierarchyNode {
    std::string key;
    std::string label;
    std::vector<size_t> layer_indices;
    std::vector<size_t> descendant_layer_indices;
    std::vector<SourceHierarchyNode> children;
};

using LayerGroupMatcher = std::function<bool(size_t, const LayerDef&)>;

GroupControlState buildGroupControlState(const LayersPanelUiContext& ctx, const LayerGroupMatcher& matches) {
    GroupControlState state;
    if (!ctx.shared || !ctx.shared->layers) return state;
    for (size_t idx = 0; idx < ctx.shared->layers->size(); ++idx) {
        const LayerDef& layer = (*ctx.shared->layers)[idx];
        if (!matches(idx, layer)) continue;
        state.has_rows = true;
        state.any_visible = state.any_visible || layer.enabled;
        if (layerSupportsHoverSelection(ctx, idx)) {
            state.hover_supported = true;
            if (state.first_hover_idx < 0) state.first_hover_idx = (int)idx;
            state.hover_selected = state.hover_selected || layerIsSelectedHoverTarget(ctx, idx);
        }
        if (layerSupportsClickSelection(ctx, idx)) {
            state.click_supported = true;
            if (state.first_click_idx < 0) state.first_click_idx = (int)idx;
            state.click_selected = state.click_selected || layerIsSelectedClickTarget(ctx, idx);
        }
    }
    return state;
}

void setGroupVisibility(const LayersPanelUiContext& ctx, const LayerGroupMatcher& matches, bool enabled) {
    if (!ctx.shared || !ctx.shared->layers) return;
    bool heatmap_changed = false;
    for (size_t idx = 0; idx < ctx.shared->layers->size(); ++idx) {
        LayerDef& layer = (*ctx.shared->layers)[idx];
        if (!matches(idx, layer)) continue;
        layer.enabled = enabled;
        if (!enabled && ctx.shared->layer_heatmap_enabled && idx < ctx.shared->layer_heatmap_enabled->size()) {
            (*ctx.shared->layer_heatmap_enabled)[idx] = false;
            heatmap_changed = true;
        }
    }
    if (heatmap_changed && ctx.shared->layer_heatmap_state_changed) *ctx.shared->layer_heatmap_state_changed = true;
}

void setGroupHoverTarget(LayersPanelUiContext& ctx, const LayerGroupMatcher& matches) {
    if (!ctx.shared || !ctx.shared->layers || !ctx.shared->layer_hover_enabled) return;
    int first_idx = -1;
    bool active_idx_in_group = false;
    for (size_t idx = 0; idx < ctx.shared->layers->size(); ++idx) {
        LayerDef& layer = (*ctx.shared->layers)[idx];
        if (!matches(idx, layer) || !layerSupportsHoverSelection(ctx, idx)) continue;
        (*ctx.shared->layer_hover_enabled)[idx] = true;
        active_idx_in_group = active_idx_in_group || (ctx.active_hover_layer_idx && *ctx.active_hover_layer_idx == (int)idx);
        if (first_idx < 0) first_idx = (int)idx;
    }
    if (first_idx >= 0 && ctx.active_hover_layer_idx) {
        *ctx.active_hover_layer_idx = active_idx_in_group ? -1 : first_idx;
        if (ctx.shared->layer_hover_state_changed) *ctx.shared->layer_hover_state_changed = true;
    }
}

void setGroupClickTarget(LayersPanelUiContext& ctx, const LayerGroupMatcher& matches) {
    if (!ctx.shared || !ctx.shared->layers || !ctx.shared->layer_inspect_enabled) return;
    int first_idx = -1;
    for (size_t idx = 0; idx < ctx.shared->layers->size(); ++idx) {
        LayerDef& layer = (*ctx.shared->layers)[idx];
        if (!matches(idx, layer) || !layerSupportsClickSelection(ctx, idx)) continue;
        if (idx < ctx.shared->layer_inspect_enabled->size()) {
            (*ctx.shared->layer_inspect_enabled)[idx] = true;
        }
        if (first_idx < 0) first_idx = (int)idx;
    }
    if (first_idx >= 0 && ctx.active_click_layer_idx) {
        *ctx.active_click_layer_idx = first_idx;
        if (ctx.shared->layer_inspect_state_changed) *ctx.shared->layer_inspect_state_changed = true;
    }
}

bool drawGroupControls(
    LayersPanelUiContext& ctx,
    const char* id,
    const GroupControlState& state,
    const LayerGroupMatcher& matches,
    const char* visibility_tooltip,
    const char* hover_tooltip,
    const char* click_tooltip) {
    ImGui::PushID(id);
    if (state.hover_supported) {
        const bool selected = state.hover_selected;
        if (ImGui::RadioButton("##hover_target", selected)) setGroupHoverTarget(ctx, matches);
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(hover_tooltip);
            ImGui::EndTooltip();
        }
        ImGui::SameLine();
    }
    if (state.click_supported) {
        const bool selected = state.click_selected;
        if (ImGui::RadioButton("##click_target", selected)) setGroupClickTarget(ctx, matches);
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(click_tooltip);
            ImGui::EndTooltip();
        }
        ImGui::SameLine();
    }
    bool visible = state.any_visible;
    const bool changed = drawIconToggleButton("show", "V", visible, visibility_tooltip);
    if (changed) setGroupVisibility(ctx, matches, visible);
    ImGui::PopID();
    return changed;
}

std::string humanizeSourceHierarchyValue(const std::string& value) {
    if (value.empty()) return {};
    if (value == "earth") return "Earth";
    if (value == "us") return "US";
    if (value.size() <= 3) {
        std::string out = value;
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
            return (char)std::toupper(c);
        });
        return out;
    }
    std::string out;
    out.reserve(value.size());
    bool uppercase_next = true;
    for (char ch : value) {
        if (ch == '_' || ch == '-') {
            out.push_back(' ');
            uppercase_next = true;
            continue;
        }
        if (uppercase_next) out.push_back((char)std::toupper((unsigned char)ch));
        else out.push_back(ch);
        uppercase_next = false;
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> sourceHierarchySegments(const LayerDef& layer) {
    std::vector<std::pair<std::string, std::string>> out;
    auto push_segment = [&](const char* level, const std::string& value) {
        if (value.empty()) return;
        out.push_back({
            std::string(level) + ":" + value,
            humanizeSourceHierarchyValue(value)
        });
    };
    push_segment("world", layer.provenance_world);
    push_segment("nation_state", layer.provenance_nation_state);
    push_segment("state_region", layer.provenance_state_region);
    push_segment("county_city", layer.provenance_county_city);
    return out;
}

void insertLayerIntoSourceHierarchy(
    SourceHierarchyNode& root,
    const std::vector<std::pair<std::string, std::string>>& segments,
    size_t layer_idx) {
    root.descendant_layer_indices.push_back(layer_idx);
    SourceHierarchyNode* node = &root;
    for (const auto& [segment_key, segment_label] : segments) {
        auto it = std::find_if(node->children.begin(), node->children.end(), [&](const SourceHierarchyNode& child) {
            return child.key == segment_key;
        });
        if (it == node->children.end()) {
            node->children.push_back(SourceHierarchyNode{segment_key, segment_label});
            it = node->children.end() - 1;
        }
        it->descendant_layer_indices.push_back(layer_idx);
        node = &(*it);
    }
    node->layer_indices.push_back(layer_idx);
}

LayerGroupMatcher matcherForLayerIndices(const std::vector<size_t>& indices) {
    return [indices](size_t idx, const LayerDef&) {
        return std::find(indices.begin(), indices.end(), idx) != indices.end();
    };
}

void drawStandardLayerRow(LayersPanelUiContext& ctx, size_t idx, LayerDef& layer) {
    ImGui::PushID((int)idx);
    const bool local_layer_exists =
        ctx.shared->local_layer_exists_cache && idx < ctx.shared->local_layer_exists_cache->size()
            ? (*ctx.shared->local_layer_exists_cache)[idx]
            : false;
    const bool download_pending =
        ctx.shared->layer_download_pending ? ctx.shared->layer_download_pending(idx) : false;
    if (!local_layer_exists && !download_pending) {
        pushButtonPalette(ButtonPalette::Download);
        const bool can_download = ctx.shared->layer_registry
            ? ctx.shared->layer_registry->canDownload(idx)
            : (!layer.source_url.empty() || !layer.import_type.empty());
        const bool has_source_metadata = ctx.shared->layer_registry
            ? ctx.shared->layer_registry->hasSourceMetadata(idx)
            : (can_download || !layer.reference_url.empty() || !layer.source_urls.empty());
        if (ImGui::SmallButton("D")) {
            if (can_download && ctx.shared->enqueue_layer_download_request) {
                ctx.shared->enqueue_layer_download_request(idx);
            } else if (ctx.shared->data_library_status_msg) {
                *ctx.shared->data_library_status_msg = has_source_metadata
                    ? "No direct downloadable GeoJSON URL for " + layer.file + "; see layer tooltip for source URLs."
                    : "No source URL for " + layer.file;
            }
        }
        ImGui::PopStyleColor(buttonPaletteColorCount(ButtonPalette::Download));
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Download missing dataset");
            ImGui::TextDisabled(
                "%s",
                can_download ? (layer.source_url.empty() ? "Import source available" : "Direct download URL available") :
                (has_source_metadata ? "Source URLs documented; no direct app download URL" : "No source URL in manifest"));
            ImGui::EndTooltip();
        }
        ImGui::SameLine();
    }
    if (!local_layer_exists && download_pending) {
        ImGui::TextDisabled("Q");
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Layer download is queued or active");
            ImGui::EndTooltip();
        }
        ImGui::SameLine();
    }

    const bool hover_selected = layerIsSelectedHoverTarget(ctx, idx);
    if (layerSupportsHoverSelection(ctx, idx)) {
        if (ImGui::RadioButton("##hover_target", hover_selected)) {
            selectHoverTargetLayer(ctx, idx);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Set active hover inspector target");
            ImGui::EndTooltip();
        }
        ImGui::SameLine();
    }
    const bool click_selected = layerIsSelectedClickTarget(ctx, idx);
    if (layerSupportsClickSelection(ctx, idx)) {
        if (ImGui::RadioButton("##click_target", click_selected)) {
            selectClickTargetLayer(ctx, idx);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Set active click action target");
            ImGui::EndTooltip();
        }
        ImGui::SameLine();
    }
    drawIconToggleButton("show", "V", layer.enabled, "Show layer");
    if (ImGui::SmallButton("?")) ImGui::OpenPopup("layer_display_settings");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted("Layer display settings");
        ImGui::EndTooltip();
    }

    LayerSettingsPopupContext settings_ctx =
        makeLayerSettingsPopupContext(ctx, idx, layer, local_layer_exists);
    drawLayerDisplaySettingsPopup(settings_ctx);

    ImGui::SameLine();
    drawFillColorCircleButton(ctx, idx, layer);
    ImGui::SameLine(0.0f, 3.0f);
    drawOutlineColorCircleButton(ctx, idx, layer);
    ImGui::SameLine(0.0f, 6.0f);
    drawLayerNameBadge(layer.name, layer.color);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) copyLayerNameToClipboard(layer.name);
    const bool row_hovered = ImGui::IsItemHovered();
    ImGui::SameLine();
    LayerRuntimeState st;
    if (ctx.shared->status_mutex && ctx.shared->layer_states) {
        std::lock_guard<std::mutex> lk(*ctx.shared->status_mutex);
        if (idx < ctx.shared->layer_states->size()) st = (*ctx.shared->layer_states)[idx];
    }
    const std::string display_status = layerRuntimeDisplayStatus(st, layerLogicalId(layer));
    if (st.status == LayerPipelineStatus::Failed) {
        ImGui::TextColored(ImVec4(0.85f, 0.35f, 0.2f, 1.0f), "[%s]", display_status.c_str());
    } else {
        ImGui::TextDisabled("[%s | %zu]", display_status.c_str(), st.feature_count);
    }
    const bool status_hovered = ImGui::IsItemHovered();
    if (row_hovered || status_hovered) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(layer.name.c_str());
        ImGui::Separator();
        ImGui::Text("Category: %s", categoryToString(layer.category));
        ImGui::Text("Status: %s", display_status.c_str());
        ImGui::TextDisabled("Pipeline: %s", statusToString(st.status));
        if (!st.hydration_phase.empty()) ImGui::TextDisabled("Hydration: %s", st.hydration_phase.c_str());
        ImGui::Text("Features: %zu", st.feature_count);
        ImGui::Text("Layer ID: %s", layerLogicalId(layer).c_str());
        ImGui::TextDisabled("Storage Key: %s", layer.file.c_str());
        ImGui::Text("Local: %s", local_layer_exists ? "yes" : "no");
        if (!layer.scale.empty()) ImGui::Text("Scale: %s", layer.scale.c_str());
        if (!layer.heatmap_field.empty()) ImGui::Text("Heatmap Field: %s", layer.heatmap_field.c_str());
        if (!layer.description.empty()) ImGui::TextWrapped("Description: %s", layer.description.c_str());
        if (!layer.source_url.empty()) ImGui::TextWrapped("Download URL: %s", layer.source_url.c_str());
        if (!layer.reference_url.empty()) ImGui::TextWrapped("Reference: %s", layer.reference_url.c_str());
        if (!layer.source_urls.empty()) {
            ImGui::SeparatorText("Source URLs");
            for (const auto& url : layer.source_urls) ImGui::TextWrapped("%s", url.c_str());
        }
        if (!st.error.empty()) {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.85f, 0.35f, 0.2f, 1.0f), "Error: %s", st.error.c_str());
        }
        ImGui::EndTooltip();
    }
    ImGui::PopID();
}

void drawSourceHierarchyNode(LayersPanelUiContext& ctx, const SourceHierarchyNode& node) {
    const LayerGroupMatcher matches = matcherForLayerIndices(node.descendant_layer_indices);
    const GroupControlState state = buildGroupControlState(ctx, matches);
    const std::string visibility_tip = std::string("Show or hide all layers in ") + node.label;
    const std::string hover_tip = std::string("Set hover action for the first supported layer in ") + node.label;
    const std::string click_tip = std::string("Set click action for the first supported layer in ") + node.label;
    drawGroupControls(
        ctx,
        node.key.c_str(),
        state,
        matches,
        visibility_tip.c_str(),
        hover_tip.c_str(),
        click_tip.c_str());
    const bool open = ImGui::TreeNodeEx(node.label.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
    if (!open) return;

    for (const auto& child : node.children) {
        drawSourceHierarchyNode(ctx, child);
    }
    for (size_t idx : node.layer_indices) {
        LayerDef& layer = (*ctx.shared->layers)[idx];
        if (drawParcelJurisdictionFilterRow(ctx, idx, layer)) continue;
        drawStandardLayerRow(ctx, idx, layer);
    }
    ImGui::TreePop();
}

void setParcelJurisdictionSelected(LayersPanelUiContext& ctx, const char* jurisdiction, bool selected) {
    if (!ctx.parcel_jurisdiction_filter_state || !jurisdiction || jurisdiction[0] == '\0') return;
    if (selected) ctx.parcel_jurisdiction_filter_state->selected_jurisdictions.insert(jurisdiction);
    else ctx.parcel_jurisdiction_filter_state->selected_jurisdictions.erase(jurisdiction);
    ctx.parcel_jurisdiction_filter_state->dirty = true;
}

bool drawParcelJurisdictionFilterRow(LayersPanelUiContext& ctx, size_t idx, LayerDef& layer) {
    if (!ctx.parcel_jurisdiction_filter_state || layer.region != "Maryland" || layer.scale != "parcel" || (int)idx == ctx.parcel_layer_idx) {
        return false;
    }
    const char* jurisdiction = parcelJurisdictionForLayer(layer);
    if (jurisdiction[0] == '\0') return false;

    ImGui::PushID((int)idx);
    const bool local_layer_exists =
        ctx.shared->local_layer_exists_cache && idx < ctx.shared->local_layer_exists_cache->size()
            ? (*ctx.shared->local_layer_exists_cache)[idx]
            : false;
    const bool download_pending =
        ctx.shared->layer_download_pending ? ctx.shared->layer_download_pending(idx) : false;
    const bool show_download_button = !local_layer_exists && !download_pending;
    if (show_download_button) {
        pushButtonPalette(ButtonPalette::Download);
        const bool can_download = ctx.shared->layer_registry
            ? ctx.shared->layer_registry->canDownload(idx)
            : (!layer.source_url.empty() || !layer.import_type.empty());
        const bool has_source_metadata = ctx.shared->layer_registry
            ? ctx.shared->layer_registry->hasSourceMetadata(idx)
            : (can_download || !layer.reference_url.empty() || !layer.source_urls.empty());
        if (ImGui::SmallButton("D")) {
            if (can_download && ctx.shared->enqueue_layer_download_request) {
                ctx.shared->enqueue_layer_download_request(idx);
            } else if (ctx.shared->data_library_status_msg) {
                *ctx.shared->data_library_status_msg = has_source_metadata
                    ? "No direct downloadable GeoJSON URL for " + layer.file + "; see layer tooltip for source URLs."
                    : "No source URL for " + layer.file;
            }
        }
        ImGui::PopStyleColor(buttonPaletteColorCount(ButtonPalette::Download));
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Download missing dataset");
            ImGui::TextDisabled(
                "%s",
                can_download ? (layer.source_url.empty() ? "Import source available" : "Direct download URL available") :
                (has_source_metadata ? "Source URLs documented; no direct app download URL" : "No source URL in manifest"));
            ImGui::EndTooltip();
        }
        ImGui::SameLine();
    }
    drawIconToggleButton("show", "V", layer.enabled, "Show layer");
    bool selected = ctx.parcel_jurisdiction_filter_state->selected_jurisdictions.find(jurisdiction) !=
        ctx.parcel_jurisdiction_filter_state->selected_jurisdictions.end();
    if (drawIconToggleButton("show_jurisdiction", "J", selected, "Include jurisdiction in active parcel filter")) {
        setParcelJurisdictionSelected(ctx, jurisdiction, selected);
    }
    if (ImGui::SmallButton("?")) ImGui::OpenPopup("layer_display_settings");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted("Layer display settings");
        ImGui::EndTooltip();
    }

    LayerSettingsPopupContext settings_ctx =
        makeLayerSettingsPopupContext(ctx, idx, layer, local_layer_exists);
    drawLayerDisplaySettingsPopup(settings_ctx);

    ImGui::SameLine();
    drawFillColorCircleButton(ctx, idx, layer);
    ImGui::SameLine(0.0f, 3.0f);
    drawOutlineColorCircleButton(ctx, idx, layer);
    ImGui::SameLine(0.0f, 6.0f);
    drawLayerNameBadge(layer.name, layer.color);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) copyLayerNameToClipboard(layer.name);
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(layer.name.c_str());
        ImGui::Separator();
        ImGui::TextWrapped("County parcel rows have separate layer visibility and parcel-jurisdiction filter state.");
        ImGui::Text("Jurisdiction: %s", jurisdiction);
        ImGui::Text("Layer visible: %s", layer.enabled ? "yes" : "no");
        ImGui::Text("Jurisdiction filter enabled: %s", selected ? "yes" : "no");
        ImGui::Text("Local: %s", local_layer_exists ? "yes" : "no");
        ImGui::Text("Download queued/active: %s", download_pending ? "yes" : "no");
        if (!ctx.parcel_jurisdiction_filter_state->status.empty()) {
            ImGui::TextDisabled("%s", ctx.parcel_jurisdiction_filter_state->status.c_str());
        }
        ImGui::EndTooltip();
    }
    ImGui::PopID();
    return true;
}


void drawCrimeFilters(LayersPanelUiContext& ctx) {
    if (!ctx.shared || !ctx.shared->layers || !ctx.crime_filter_enabled || !ctx.crime_filter_use_year ||
        !ctx.crime_year_min || !ctx.crime_year_max || !ctx.crime_breakdown) {
        return;
    }

    ImGui::SeparatorText("Crime Filters");
    ImGui::Checkbox("Enable Crime Filter", ctx.crime_filter_enabled);
    ImGui::Checkbox("Filter Crime Year", ctx.crime_filter_use_year);
    ImGui::BeginDisabled(!*ctx.crime_filter_use_year);
    ImGui::SliderInt("Crime Year Min", ctx.crime_year_min, 1900, 2100);
    ImGui::SliderInt("Crime Year Max", ctx.crime_year_max, 1900, 2100);
    if (*ctx.crime_year_min > *ctx.crime_year_max) std::swap(*ctx.crime_year_min, *ctx.crime_year_max);
    ImGui::EndDisabled();
    ImGui::Checkbox("Homicide", ctx.crime_filter_homicide); ImGui::SameLine();
    ImGui::Checkbox("Robbery", ctx.crime_filter_robbery);
    ImGui::Checkbox("Assault", ctx.crime_filter_assault); ImGui::SameLine();
    ImGui::Checkbox("Burglary", ctx.crime_filter_burglary);
    ImGui::Checkbox("Theft/Larceny", ctx.crime_filter_theft); ImGui::SameLine();
    ImGui::Checkbox("Auto Theft", ctx.crime_filter_auto_theft);
    ImGui::Checkbox("Drug/Narcotic", ctx.crime_filter_drug); ImGui::SameLine();
    ImGui::Checkbox("Shooting", ctx.crime_filter_shooting);
    if (ImGui::Button("Clear Crime Filters")) {
        *ctx.crime_filter_homicide = false;
        *ctx.crime_filter_robbery = false;
        *ctx.crime_filter_assault = false;
        *ctx.crime_filter_burglary = false;
        *ctx.crime_filter_theft = false;
        *ctx.crime_filter_auto_theft = false;
        *ctx.crime_filter_drug = false;
        *ctx.crime_filter_shooting = false;
        *ctx.crime_filter_use_year = false;
        *ctx.crime_filter_enabled = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh Crime Breakdown")) {
        std::unordered_map<std::string, int> counts;
        auto add_layer_counts = [&](int idx) {
            if (idx < 0 || (size_t)idx >= ctx.shared->layers->size()) return;
            for (const auto& fg : (*ctx.shared->layers)[(size_t)idx].features) {
                const std::string desc = toLowerAscii(getPropertyValue(fg, "Description"));
                const std::string code = toLowerAscii(getPropertyValue(fg, "CrimeCode"));
                const std::string dt = getPropertyValue(fg, "CrimeDateTime");
                if (*ctx.crime_filter_enabled) {
                    if (*ctx.crime_filter_use_year) {
                        int yr = extractYearMaybe(dt);
                        if (yr < 0 || yr < *ctx.crime_year_min || yr > *ctx.crime_year_max) continue;
                    }
                    const bool any_type =
                        *ctx.crime_filter_homicide || *ctx.crime_filter_robbery || *ctx.crime_filter_assault ||
                        *ctx.crime_filter_burglary || *ctx.crime_filter_theft || *ctx.crime_filter_auto_theft ||
                        *ctx.crime_filter_drug || *ctx.crime_filter_shooting;
                    if (any_type) {
                        auto has = [&](const char* s) { return desc.find(s) != std::string::npos || code.find(s) != std::string::npos; };
                        bool ok = false;
                        if (*ctx.crime_filter_homicide && (has("homicide") || has("murder"))) ok = true;
                        if (*ctx.crime_filter_robbery && has("robbery")) ok = true;
                        if (*ctx.crime_filter_assault && (has("assault") || has("aggravated assault"))) ok = true;
                        if (*ctx.crime_filter_burglary && has("burglary")) ok = true;
                        if (*ctx.crime_filter_theft && (has("larceny") || has("theft"))) ok = true;
                        if (*ctx.crime_filter_auto_theft && (has("motor vehicle theft") || has("auto theft") || has("vehicle theft"))) ok = true;
                        if (*ctx.crime_filter_drug && (has("drug") || has("narcotic"))) ok = true;
                        if (*ctx.crime_filter_shooting && has("shooting")) ok = true;
                        if (!ok) continue;
                    }
                }
                std::string label = trimDisplayValue(getPropertyValue(fg, "Description"));
                if (label.empty()) label = trimDisplayValue(getPropertyValue(fg, "CrimeCode"));
                if (label.empty()) label = "(unknown)";
                counts[label] += 1;
            }
        };
        add_layer_counts(ctx.crime_nibrs_layer_idx);
        ctx.crime_breakdown->clear();
        ctx.crime_breakdown->reserve(counts.size());
        for (auto& kv : counts) ctx.crime_breakdown->push_back(kv);
        std::sort(ctx.crime_breakdown->begin(), ctx.crime_breakdown->end(), [](const auto& a, const auto& b) {
            return a.second > b.second;
        });
    }
    ImGui::Text("Breakdown Rows: %zu", ctx.crime_breakdown->size());
}

LayerSettingsPopupContext makeLayerSettingsPopupContext(
    LayersPanelUiContext& ctx,
    size_t idx,
    LayerDef& layer,
    bool local_layer_exists) {
    LayerSettingsPopupContext settings_ctx;
    settings_ctx.shared = ctx.shared;
    settings_ctx.active_hover_layer_idx = ctx.active_hover_layer_idx;
    settings_ctx.active_click_layer_idx = ctx.active_click_layer_idx;
    settings_ctx.idx = idx;
    settings_ctx.layer = &layer;
    settings_ctx.local_layer_exists = local_layer_exists;
    settings_ctx.zoom = ctx.zoom;
    return settings_ctx;
}

void drawLayerCategory(LayersPanelUiContext& ctx, LayerDef::Category cat, const char* label) {
    if (!ctx.shared || !ctx.shared->layers) return;
    const LayerGroupMatcher category_matches = [&](size_t idx, const LayerDef& layer) {
        return layer.category == cat &&
            !hiddenParcelParameterLayer(*ctx.shared, ctx.parcel_layer_idx, idx) &&
            layerVisibleInHierarchy(ctx, layer);
    };
    const GroupControlState category_state = buildGroupControlState(ctx, category_matches);
    if (!category_state.has_rows) return;
    const std::string category_id = std::string("category_") + label;
    const std::string category_visibility_tip = std::string("Show or hide all layers in ") + label;
    const std::string category_hover_tip = std::string("Set hover action for the first supported layer in ") + label;
    const std::string category_click_tip = std::string("Set click action for the first supported layer in ") + label;
    drawGroupControls(
        ctx,
        category_id.c_str(),
        category_state,
        category_matches,
        category_visibility_tip.c_str(),
        category_hover_tip.c_str(),
        category_click_tip.c_str());
    const bool open = ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
    if (!open) return;
    SourceHierarchyNode root;
    root.key = std::string("source_root_") + label;
    root.label = label;
    for (size_t idx = 0; idx < ctx.shared->layers->size(); ++idx) {
        LayerDef& layer = (*ctx.shared->layers)[idx];
        if (layer.category != cat) continue;
        if (hiddenParcelParameterLayer(*ctx.shared, ctx.parcel_layer_idx, idx)) continue;
        if (!layerVisibleInHierarchy(ctx, layer)) continue;
        insertLayerIntoSourceHierarchy(root, sourceHierarchySegments(layer), idx);
    }
    for (const auto& child : root.children) {
        drawSourceHierarchyNode(ctx, child);
    }
    for (size_t idx : root.layer_indices) {
        LayerDef& layer = (*ctx.shared->layers)[idx];
        if (drawParcelJurisdictionFilterRow(ctx, idx, layer)) continue;
        drawStandardLayerRow(ctx, idx, layer);
    }

    if (cat == LayerDef::Category::Safety) drawCrimeFilters(ctx);
    ImGui::TreePop();
}
}

bool drawLayerColorEditorContents(LayersPanelUiContext& ctx, size_t idx, LayerColorEditorTarget target) {
    if (!ctx.shared || !ctx.shared->layers || idx >= ctx.shared->layers->size()) return false;
    LayerDef& layer = (*ctx.shared->layers)[idx];
    switch (target) {
        case LayerColorEditorTarget::Fill:
            return drawFillColorEditor(ctx, idx, layer);
        case LayerColorEditorTarget::Outline:
            return drawOutlineColorEditor(layer.outline_color);
    }
    return false;
}

void drawLayerCategoriesPanel(LayersPanelUiContext& ctx) {
    drawLayerCategory(ctx, LayerDef::Category::Housing, "Housing");
    drawLayerCategory(ctx, LayerDef::Category::PublicHealth, "Public Health");
    drawLayerCategory(ctx, LayerDef::Category::Safety, "Safety");
    drawLayerCategory(ctx, LayerDef::Category::Infrastructure, "Infrastructure");
    drawLayerCategory(ctx, LayerDef::Category::Zoning, "Zoning");
    drawClipboardToastOverlay();
}
