#include "gradient_tab.h"

#include "aggregate_visualization_strategies.h"
#include "imgui.h"
#include "map_render_utils.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <iomanip>
#include <limits>
#include <numbers>
#include <sstream>

namespace {
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

std::string formatGradientValue(const std::string& field, double value) {
    std::string lower_field = field;
    std::transform(lower_field.begin(), lower_field.end(), lower_field.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    if (lower_field.find("usd") != std::string::npos ||
        lower_field.find("value") != std::string::npos ||
        lower_field.find("amount") != std::string::npos ||
        lower_field.find("cost") != std::string::npos ||
        lower_field.find("price") != std::string::npos) {
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
        const double sx = kDegToMetersLat * std::cos(lat0 * std::numbers::pi / 180.0);
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

double parcelParameterValue(const GradientTabContext& ctx, size_t parcel_idx, const LayerDef::FeatureGeom& fg) {
    if (ctx.parcel_parameter_mode == 1) return parcelAreaSqM(fg);
    if (ctx.parcel_parameter_mode == 2 && ctx.unified_parcels) {
        const UnifiedParcelRecord* rec = unifiedParcelAt(*ctx.unified_parcels, parcel_idx);
        return rec ? rec->current_value : 0.0;
    }
    return 0.0;
}

void drawPowerLegend(const std::string& field, float min_v, float max_v, float gamma) {
    const float stops[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    for (float stop : stops) {
        const float source_t = std::pow(stop, 1.0f / std::clamp(gamma, 0.10f, 5.0f));
        const float value = min_v + (max_v - min_v) * source_t;
        ImGui::ColorButton(
            (std::string("##legend_") + field + std::to_string(stop)).c_str(),
            heatColor(stop),
            ImGuiColorEditFlags_NoTooltip,
            ImVec2(18, 12));
        ImGui::SameLine();
        ImGui::Text("%3.0f%% color: %s", stop * 100.0f, formatGradientValue(field, value).c_str());
    }
}
}

void drawGradientTab(const GradientTabContext& ctx) {
    if (!ImGui::BeginTabItem("Gradient")) return;

    ImGui::TextUnformatted("Gradient Scaling");
    ImGui::Separator();
    ImGui::TextWrapped("Numeric layer colors use a blue-to-yellow-to-red gradient. Effective ranges are percentile-clipped so extreme outliers do not flatten normal parcel colors.");
    ImGui::Separator();

    int gradient_layer_count = 0;
    ImGui::BeginChild("gradient_scale_layers", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    if (ctx.layers && ctx.parcel_parameter_mode > 0 && ctx.parcel_layer_idx >= 0 && (size_t)ctx.parcel_layer_idx < ctx.layers->size()) {
        const auto& parcel_layer = (*ctx.layers)[(size_t)ctx.parcel_layer_idx];
        std::vector<float> values;
        values.reserve(parcel_layer.features.size());
        for (size_t fi = 0; fi < parcel_layer.features.size(); ++fi) {
            const auto& fg = parcel_layer.features[fi];
            if (ctx.feature_passes_filters && !ctx.feature_passes_filters((size_t)ctx.parcel_layer_idx, fi, fg)) continue;
            const double v = parcelParameterValue(ctx, fi, fg);
            if (v > 0.0 && std::isfinite(v)) values.push_back((float)v);
        }
        if (!values.empty()) {
            gradient_layer_count++;
            std::sort(values.begin(), values.end());
            const float min_v = values.front();
            const float max_v = values.back();
            const std::string field = ctx.parcel_parameter_mode == 1 ? "parcel_area_sq_m" : "current_value";
            float gamma = ctx.layer_choropleth_gamma && (size_t)ctx.parcel_layer_idx < ctx.layer_choropleth_gamma->size()
                ? (*ctx.layer_choropleth_gamma)[(size_t)ctx.parcel_layer_idx]
                : 1.0f;
            if (ImGui::CollapsingHeader("Active Parcel Choropleth", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Text("Layer: %s", parcel_layer.name.c_str());
                ImGui::Text("Field: %s", field.c_str());
                ImGui::Text("Filtered samples: %zu", values.size());
                ImGui::Text("Blue floor: %s", formatGradientValue(field, min_v).c_str());
                ImGui::Text("Red ceiling: %s", formatGradientValue(field, max_v).c_str());
                if (ImGui::SliderFloat("Gamma##active_parcel_gamma", &gamma, 0.10f, 5.0f, "%.2f") &&
                    ctx.layer_choropleth_gamma && (size_t)ctx.parcel_layer_idx < ctx.layer_choropleth_gamma->size()) {
                    (*ctx.layer_choropleth_gamma)[(size_t)ctx.parcel_layer_idx] = gamma;
                    if (ctx.layer_heatmap_state_changed) *ctx.layer_heatmap_state_changed = true;
                }
                drawPowerLegend(field, min_v, max_v, gamma);
            }
        }
    }
    if (ctx.layers) {
        for (size_t li = 0; li < ctx.layers->size(); ++li) {
            const auto& layer = (*ctx.layers)[li];
            if (!layer.enabled || layer.heatmap_field.empty()) continue;
            std::vector<float> values;
            values.reserve(layer.features.size());
            for (size_t fi = 0; fi < layer.features.size(); ++fi) {
                const auto& fg = layer.features[fi];
                if (ctx.feature_passes_filters && !ctx.feature_passes_filters(li, fi, fg)) continue;
                float v = 0.0f;
                if (tryGetFeaturePropertyFloat(fg, layer.heatmap_field, v)) values.push_back(v);
            }
            if (values.empty()) continue;
            gradient_layer_count++;
            std::sort(values.begin(), values.end());
            const float clip_pct = std::clamp(
                ctx.layer_heatmap_percentile_clip && li < ctx.layer_heatmap_percentile_clip->size()
                    ? (*ctx.layer_heatmap_percentile_clip)[li]
                    : ctx.heatmap_percentile_clip,
                50.0f,
                100.0f);
            const size_t max_idx = values.size() - 1;
            const size_t clip_idx = clip_pct < 100.0f
                ? (size_t)std::clamp((int)std::floor((clip_pct / 100.0f) * (double)max_idx), 0, (int)max_idx)
                : max_idx;
            const float raw_min = values.front();
            const float raw_max = values.back();
            const float effective_min = raw_min;
            const float effective_max = std::max(effective_min, values[clip_idx]);
            const float median = values[max_idx / 2];
            const bool layer_heatmap_on = ctx.layer_heatmap_enabled && li < ctx.layer_heatmap_enabled->size() && (*ctx.layer_heatmap_enabled)[li];
            const int aggregate_max_zoom = ctx.layer_heatmap_max_zoom && li < ctx.layer_heatmap_max_zoom->size() ? (*ctx.layer_heatmap_max_zoom)[li] : 13;
            const int detail_min_zoom = ctx.layer_parcel_detail_min_zoom && li < ctx.layer_parcel_detail_min_zoom->size()
                ? (*ctx.layer_parcel_detail_min_zoom)[li]
                : kParcelChoroplethMinZoom;
            const int aggregate_algo = (ctx.layer_heatmap_algo && li < ctx.layer_heatmap_algo->size() && (*ctx.layer_heatmap_algo)[li] >= 0)
                ? (*ctx.layer_heatmap_algo)[li]
                : ctx.heatmap_algo;
            const bool parcel_choropleth_now =
                layer.scale == "parcel" &&
                !layer.heatmap_field.empty() &&
                ctx.zoom >= detail_min_zoom;
            const bool aggregate_now =
                layer_heatmap_on &&
                ctx.zoom <= aggregate_max_zoom &&
                !parcel_choropleth_now &&
                isHeatmapAggregateMethod(aggregate_algo);
            std::string resolved = "per-feature fill";
            if (parcel_choropleth_now) resolved = "parcel choropleth";
            else if (aggregate_now) resolved = std::string("aggregate ") + aggregateStrategyName(aggregate_algo);

            if (ImGui::CollapsingHeader(layer.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                float gamma = ctx.layer_choropleth_gamma && li < ctx.layer_choropleth_gamma->size()
                    ? (*ctx.layer_choropleth_gamma)[li]
                    : 1.0f;
                ImGui::Text("Field: %s", layer.heatmap_field.c_str());
                ImGui::Text("Resolved display: %s", resolved.c_str());
                ImGui::Text("Filtered samples: %zu", values.size());
                ImGui::Text("Clip: %.0f%%", clip_pct);
                ImGui::Text("Blue floor: %s", formatGradientValue(layer.heatmap_field, effective_min).c_str());
                ImGui::Text("Yellow midpoint: %s", formatGradientValue(layer.heatmap_field, (effective_min + effective_max) * 0.5).c_str());
                ImGui::Text("Red ceiling: %s", formatGradientValue(layer.heatmap_field, effective_max).c_str());
                ImGui::TextDisabled("Median: %s", formatGradientValue(layer.heatmap_field, median).c_str());
                ImGui::TextDisabled("Raw max: %s", formatGradientValue(layer.heatmap_field, raw_max).c_str());
                if (ImGui::SliderFloat(("Gamma##gamma_" + std::to_string(li)).c_str(), &gamma, 0.10f, 5.0f, "%.2f") &&
                    ctx.layer_choropleth_gamma && li < ctx.layer_choropleth_gamma->size()) {
                    (*ctx.layer_choropleth_gamma)[li] = gamma;
                    if (ctx.layer_heatmap_state_changed) *ctx.layer_heatmap_state_changed = true;
                }
                drawPowerLegend(layer.heatmap_field, effective_min, effective_max, gamma);
                if (raw_max > effective_max) {
                    ImGui::TextWrapped("Values above the red ceiling are clamped to red. This prevents outliers from making ordinary parcels all blue.");
                }
                if (layer.scale == "parcel") {
                    ImGui::TextDisabled("Aggregate max zoom: %d | Parcel detail min zoom: %d", aggregate_max_zoom, detail_min_zoom);
                } else {
                    ImGui::TextDisabled("Aggregate max zoom: %d", aggregate_max_zoom);
                }
            }
        }
    }
    if (gradient_layer_count == 0) {
        ImGui::TextDisabled("No enabled numeric gradient layers are loaded.");
        ImGui::TextWrapped("Enable a layer with a heatmap field, such as Property Value (Parcels), to see its current gradient scale.");
    }
    ImGui::EndChild();
    ImGui::EndTabItem();
}
