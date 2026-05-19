#include "layers_panel_ui.h"

#include "aggregate_visualization_strategies.h"
#include "app_utils.h"
#include "choropleth_histogram.h"
#include "feature_props.h"
#include "layer_settings.h"
#include "layer_ui_actions.h"
#include "map_render_utils.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <unordered_map>

namespace {
bool drawColorCircleButton(const char* id, const char* tooltip, const ImVec4& color) {
    ImGui::PushID(id);
    const float radius = 5.0f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 size(radius * 2.0f + 6.0f, radius * 2.0f + 6.0f);
    ImGui::InvisibleButton("##color_circle", size);
    const bool clicked = ImGui::IsItemClicked();
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
    return clicked;
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

double parcelAreaSqM(const LayerDef::FeatureGeom& fg) {
    if (fg.rings.empty()) return 0.0;
    constexpr double kDegToMetersLat = 111320.0;
    double total = 0.0;
    for (const auto& ring : fg.rings) {
        if (ring.size() < 3) continue;
        double lat_sum = 0.0;
        for (const auto& p : ring) lat_sum += (double)p.y;
        const double lat0 = lat_sum / (double)ring.size();
        const double sx = kDegToMetersLat * std::cos(lat0 * 3.14159265358979323846 / 180.0);
        double a = 0.0;
        for (size_t i = 0, n = ring.size(); i < n; ++i) {
            const auto& p = ring[i];
            const auto& q = ring[(i + 1) % n];
            a += ((double)p.x * sx) * ((double)q.y * kDegToMetersLat) -
                 ((double)q.x * sx) * ((double)p.y * kDegToMetersLat);
        }
        total += std::abs(a) * 0.5;
    }
    return total;
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
        const double area = parcelAreaSqM(fg);
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
        const double area = parcelAreaSqM(parcel_fg);
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
    const bool clicked = drawColorCircleButton("fill_color", "Fill color and continuous scale", layer.color);
    if (clicked && ctx.shared && ctx.shared->open_layer_color_editor) ctx.shared->open_layer_color_editor(idx, false);
    return clicked;
}

bool drawOutlineColorCircleButton(const LayersPanelUiContext& ctx, size_t idx, const LayerDef& layer) {
    const bool clicked = drawColorCircleButton("outline_color", "Outline color", layer.outline_color);
    if (clicked && ctx.shared && ctx.shared->open_layer_color_editor) ctx.shared->open_layer_color_editor(idx, true);
    return clicked;
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
    const std::filesystem::path& local_layer_path,
    bool local_layer_exists);

bool categoryHasVisibleSubdata(const LayersPanelUiContext& ctx, LayerDef::Category cat) {
    if (!ctx.shared || !ctx.shared->layers) return false;
    for (const LayerDef& layer : *ctx.shared->layers) {
        if (layer.category != cat) continue;
        if (ctx.map_filter_state && !layerMatchesSelectedGeography(layer, *ctx.map_filter_state)) continue;
        if (layer.enabled) return true;
    }
    return false;
}

bool layerVisibleInHierarchy(const LayersPanelUiContext& ctx, const LayerDef& layer) {
    return !ctx.map_filter_state || layerMatchesSelectedGeography(layer, *ctx.map_filter_state);
}

bool drawBranchVisibilityToggle(const char* id, bool visible, const char* tooltip) {
    ImGui::PushID(id);
    pushButtonPalette(visible ? ButtonPalette::ToggleOn : ButtonPalette::ToggleOff);
    const bool clicked = ImGui::SmallButton("Eye");
    ImGui::PopStyleColor(buttonPaletteColorCount(visible ? ButtonPalette::ToggleOn : ButtonPalette::ToggleOff));
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(tooltip);
        ImGui::TextDisabled("%s", visible ? "Some subdata visible" : "All subdata hidden");
        ImGui::EndTooltip();
    }
    ImGui::PopID();
    return clicked;
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
    const std::filesystem::path local_layer_path = resolveStoredLayerPath(ctx.shared->root, layer);
    const bool local_layer_exists =
        ctx.shared->local_layer_exists_cache && idx < ctx.shared->local_layer_exists_cache->size()
            ? (*ctx.shared->local_layer_exists_cache)[idx]
            : false;
    const bool active_parcel_layer_exists =
        ctx.shared->local_layer_exists_cache &&
        ctx.parcel_layer_idx >= 0 &&
        (size_t)ctx.parcel_layer_idx < ctx.shared->local_layer_exists_cache->size()
            ? (*ctx.shared->local_layer_exists_cache)[(size_t)ctx.parcel_layer_idx]
            : false;
    const bool show_download_button = !local_layer_exists && !active_parcel_layer_exists;
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
    bool selected = ctx.parcel_jurisdiction_filter_state->selected_jurisdictions.find(jurisdiction) !=
        ctx.parcel_jurisdiction_filter_state->selected_jurisdictions.end();
    if (drawIconToggleButton("show_jurisdiction", "V", selected, "Show jurisdiction in active parcel filter")) {
        setParcelJurisdictionSelected(ctx, jurisdiction, selected);
    }
    if (ImGui::SmallButton("?")) ImGui::OpenPopup("layer_display_settings");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted("Layer display settings");
        ImGui::EndTooltip();
    }

    LayerSettingsPopupContext settings_ctx =
        makeLayerSettingsPopupContext(ctx, idx, layer, local_layer_path, local_layer_exists);
    drawLayerDisplaySettingsPopup(settings_ctx);

    ImGui::SameLine();
    drawFillColorCircleButton(ctx, idx, layer);
    ImGui::SameLine(0.0f, 3.0f);
    drawOutlineColorCircleButton(ctx, idx, layer);
    ImGui::SameLine(0.0f, 6.0f);
    drawLayerNameBadge(layer.name, layer.color);
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(layer.name.c_str());
        ImGui::Separator();
        ImGui::TextWrapped("Filters the active Maryland parcel layer through DuckDB before Vulkan rendering.");
        ImGui::Text("Jurisdiction: %s", jurisdiction);
        ImGui::Text("Local: %s", local_layer_exists ? "yes" : "no");
        if (active_parcel_layer_exists) {
            ImGui::TextDisabled("Download hidden because the Maryland parcel layer is already available locally.");
        }
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
    const std::filesystem::path& local_layer_path,
    bool local_layer_exists) {
    LayerSettingsPopupContext settings_ctx;
    settings_ctx.shared = ctx.shared;
    settings_ctx.local_layer_path = local_layer_path;
    settings_ctx.idx = idx;
    settings_ctx.layer = &layer;
    settings_ctx.local_layer_exists = local_layer_exists;
    settings_ctx.zoom = ctx.zoom;
    return settings_ctx;
}

void drawLayerCategory(LayersPanelUiContext& ctx, LayerDef::Category cat, const char* label) {
    if (!ctx.shared || !ctx.shared->layers) return;
    const bool any_visible = categoryHasVisibleSubdata(ctx, cat);
    const std::string toggle_id = std::string("toggle_") + label;
    const std::string toggle_tip = std::string("Toggle all ") + label + " subdata";
    if (drawBranchVisibilityToggle(toggle_id.c_str(), any_visible, toggle_tip.c_str())) {
        setCategoryVisible(*ctx.shared, ctx.parcel_layer_idx, cat, !any_visible);
    }
    ImGui::SameLine();
    const bool open = ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
    if (!open) return;

    std::string current_subcategory;
    std::string current_scope;
    std::string current_region;
    bool current_region_open = false;
    for (size_t idx = 0; idx < ctx.shared->layers->size(); ++idx) {
        LayerDef& layer = (*ctx.shared->layers)[idx];
        if (layer.category != cat) continue;
        if (hiddenParcelParameterLayer(*ctx.shared, ctx.parcel_layer_idx, idx)) continue;
        if (!layerVisibleInHierarchy(ctx, layer)) continue;
        if (layer.subcategory != current_subcategory) {
            if (!current_region.empty() && current_region_open) ImGui::TreePop();
            current_subcategory = layer.subcategory;
            if (!current_subcategory.empty()) ImGui::SeparatorText(current_subcategory.c_str());
            current_scope.clear();
            current_region.clear();
            current_region_open = false;
        }
        if (layer.region != current_region) {
            if (!current_region.empty() && current_region_open) ImGui::TreePop();
            current_region = layer.region;
            current_region_open = false;
            if (!current_region.empty()) {
                current_region_open = ImGui::TreeNodeEx(
                    current_region.c_str(),
                    ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
                current_scope.clear();
            }
        }
        if (!current_region.empty() && !current_region_open) {
            continue;
        }
        if (drawParcelJurisdictionFilterRow(ctx, idx, layer)) {
            continue;
        }
        const char* scope_label = geographicScopeLabel(layer);
        if (scope_label != current_scope) {
            current_scope = scope_label;
            ImGui::TextDisabled("%s", current_scope.c_str());
        }

        ImGui::PushID((int)idx);
        const std::filesystem::path local_layer_path = resolveStoredLayerPath(ctx.shared->root, layer);
        const bool local_layer_exists =
            ctx.shared->local_layer_exists_cache && idx < ctx.shared->local_layer_exists_cache->size()
                ? (*ctx.shared->local_layer_exists_cache)[idx]
                : false;
        if (!local_layer_exists) {
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
        if (ImGui::SmallButton("?")) ImGui::OpenPopup("layer_display_settings");
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Layer display settings");
            ImGui::EndTooltip();
        }

        LayerSettingsPopupContext settings_ctx =
            makeLayerSettingsPopupContext(ctx, idx, layer, local_layer_path, local_layer_exists);
        drawLayerDisplaySettingsPopup(settings_ctx);

        ImGui::SameLine();
        drawFillColorCircleButton(ctx, idx, layer);
        ImGui::SameLine(0.0f, 3.0f);
        drawOutlineColorCircleButton(ctx, idx, layer);
        ImGui::SameLine(0.0f, 6.0f);
        drawLayerNameBadge(layer.name, layer.color);
        const bool row_hovered = ImGui::IsItemHovered();
        ImGui::SameLine();
        LayerRuntimeState st;
        if (ctx.shared->status_mutex && ctx.shared->layer_states) {
            std::lock_guard<std::mutex> lk(*ctx.shared->status_mutex);
            if (idx < ctx.shared->layer_states->size()) st = (*ctx.shared->layer_states)[idx];
        }
        const std::string display_status = layerRuntimeDisplayStatus(st, layer.file);
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
            if (!st.triangulation_phase.empty()) ImGui::TextDisabled("Triangulation: %s", st.triangulation_phase.c_str());
            ImGui::Text("Features: %zu", st.feature_count);
            ImGui::Text("File: %s", layer.file.c_str());
            ImGui::Text("Local: %s", local_layer_exists ? "yes" : "no");
            if (!layer.subcategory.empty()) ImGui::Text("Subcategory: %s", layer.subcategory.c_str());
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
    if (!current_region.empty() && current_region_open) ImGui::TreePop();

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
}
