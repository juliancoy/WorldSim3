#include "gradient_tab.h"

#include "aggregate_visualization_strategies.h"
#include "imgui.h"
#include "map_render_utils.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <iomanip>
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
}

void drawGradientTab(const GradientTabContext& ctx) {
    if (!ImGui::BeginTabItem("Gradient")) return;

    ImGui::TextUnformatted("Gradient Scaling");
    ImGui::Separator();
    ImGui::TextWrapped("Numeric layer colors use a blue-to-yellow-to-red gradient. Effective ranges are percentile-clipped so extreme outliers do not flatten normal parcel colors.");
    ImGui::Separator();

    int gradient_layer_count = 0;
    ImGui::BeginChild("gradient_scale_layers", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    if (ctx.layers) {
        for (size_t li = 0; li < ctx.layers->size(); ++li) {
            const auto& layer = (*ctx.layers)[li];
            if (!layer.enabled || layer.heatmap_field.empty()) continue;
            std::vector<float> values;
            values.reserve(layer.features.size());
            for (size_t fi = 0; fi < layer.features.size(); ++fi) {
                const auto& fg = layer.features[fi];
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
                ImGui::Text("Field: %s", layer.heatmap_field.c_str());
                ImGui::Text("Resolved display: %s", resolved.c_str());
                ImGui::Text("Samples: %zu", values.size());
                ImGui::Text("Clip: %.0f%%", clip_pct);
                ImGui::Text("Blue floor: %s", formatGradientValue(layer.heatmap_field, effective_min).c_str());
                ImGui::Text("Yellow midpoint: %s", formatGradientValue(layer.heatmap_field, (effective_min + effective_max) * 0.5).c_str());
                ImGui::Text("Red ceiling: %s", formatGradientValue(layer.heatmap_field, effective_max).c_str());
                ImGui::TextDisabled("Median: %s", formatGradientValue(layer.heatmap_field, median).c_str());
                ImGui::TextDisabled("Raw max: %s", formatGradientValue(layer.heatmap_field, raw_max).c_str());
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
