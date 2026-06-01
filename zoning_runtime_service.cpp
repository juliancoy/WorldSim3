#include "zoning_runtime_service.h"

#include "app_utils.h"
#include "feature_props.h"
#include "heat_normalization.h"
#include "map_render_utils.h"
#include "render_routing.h"
#include "worldsim_app.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>

namespace {

ImU32 mapPolygonFillColor(ImU32 color, float opacity) {
    const uint32_t src_alpha = (color >> 24) & 0xFFu;
    if (src_alpha == 0) return color;
    const uint32_t alpha = static_cast<uint32_t>(std::clamp(
        static_cast<int>(std::lround(static_cast<float>(src_alpha) * std::clamp(opacity, 0.0f, 1.0f))),
        0,
        255));
    return (color & 0x00FFFFFFu) | (alpha << 24);
}

void hashMix(uint64_t& h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
}

void hashCString(uint64_t& h, const char* s) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s ? s : "");
    while (*p) {
        hashMix(h, *p);
        ++p;
    }
}

void hashF32(uint64_t& h, float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    hashMix(h, bits);
}

bool isZoningPolygonLayer(const LayerDef& layer) {
    if (layerUsesPointGeometry(layer)) return false;
    if (layer.category == LayerDef::Category::Zoning) return true;
    const std::string file_lower = toLowerAscii(layer.file);
    const std::string name_lower = toLowerAscii(layer.name);
    return file_lower.find("zoning") != std::string::npos ||
           name_lower.find("zoning") != std::string::npos;
}

std::string polygonNormalizationGroupKey(const LayerDef::FeatureRecord& fg) {
    return normalizeJoinKey(getFirstPropertyValue(fg, {
        "ZONECODE", "ZONING", "ZONE", "zoning", "zoning_group",
        "group_key", "LANDUSE", "LAND_USE", "USE", "CATEGORY", "category"
    }));
}

void clearZoningLayerState(size_t layer_idx, ZoningRuntimeState& state) {
    clearZoningGpuBuffers(layer_idx);
    clearZoningGpuDrawState(layer_idx);
    state.uploaded_signatures.erase(layer_idx);
    state.color_state_keys.erase(layer_idx);
    state.outline_state_keys.erase(layer_idx);
    state.last_base_colors.erase(layer_idx);
    state.last_outline_colors.erase(layer_idx);
    state.render_blobs.erase(layer_idx);
    state.color_features.erase(layer_idx);
    state.color_feature_properties.erase(layer_idx);
}

const FeaturePropertyPairs* featurePropertiesForColoring(
    const LayerDef& layer,
    const std::vector<LayerDef::FeatureProperties>* fallback_properties,
    size_t feature_idx) {
    if (feature_idx < layer.feature_properties.size()) return &layer.feature_properties[feature_idx].values;
    if (fallback_properties && feature_idx < fallback_properties->size()) return &(*fallback_properties)[feature_idx].values;
    return nullptr;
}

std::string firstFeaturePropertyForColoring(
    const LayerDef& layer,
    const std::vector<LayerDef::FeatureProperties>* fallback_properties,
    const LayerDef::FeatureRecord& fg,
    size_t feature_idx,
    std::initializer_list<const char*> keys) {
    if (const FeaturePropertyPairs* props = featurePropertiesForColoring(layer, fallback_properties, feature_idx)) {
        for (const char* key : keys) {
            if (!key) continue;
            for (const auto& kv : *props) {
                if (kv.first == key) return kv.second;
            }
        }
    }
    return getFirstPropertyValue(fg, keys);
}

std::string zoningClassKeyForColoring(
    const LayerDef& layer,
    const std::vector<LayerDef::FeatureProperties>* fallback_properties,
    const LayerDef::FeatureRecord& fg,
    size_t feature_idx) {
    if (const FeaturePropertyPairs* props = featurePropertiesForColoring(layer, fallback_properties, feature_idx)) {
        return zoningClassKeyFromPropertyPairs(*props);
    }
    std::string z = getFirstPropertyValue(fg, {
        "GENZONE", "GENZONE_CAT",
        "Zoning", "Label", "ZoningLabel", "ZONING", "ZONED", "ZONE",
        "ZONE_CLASS", "ZONE_DIST", "CLASS", "DISTRICT", "Type", "TYPE", "DIST_CODE"
    });
    return z.empty() ? "UNSPECIFIED" : z;
}

bool tryGetFeaturePropertyFloatForColoring(
    const LayerDef& layer,
    const std::vector<LayerDef::FeatureProperties>* fallback_properties,
    const LayerDef::FeatureRecord& fg,
    size_t feature_idx,
    const std::string& key,
    float& out) {
    if (const FeaturePropertyPairs* props = featurePropertiesForColoring(layer, fallback_properties, feature_idx)) {
        for (const auto& kv : *props) {
            if (kv.first != key) continue;
            char* end = nullptr;
            const float v = std::strtof(kv.second.c_str(), &end);
            if (end == kv.second.c_str() || (end && *end != '\0') || !std::isfinite(v)) return false;
            out = v;
            return true;
        }
    }
    return tryGetFeaturePropertyFloat(fg, key, out);
}

HeatNormalizationState buildColoringHeatNormalizationState(
    const LayerDef& layer,
    const std::vector<LayerDef::FeatureRecord>* fallback_features,
    const std::vector<LayerDef::FeatureProperties>* fallback_properties,
    size_t layer_idx,
    const std::vector<int>& layer_normalize_mode,
    const std::vector<float>& layer_heatmap_percentile_clip,
    float heatmap_percentile_clip,
    const LayerFeatureRenderCache& cached_feature_render) {
    HeatNormalizationState state;
    state.heat_min = std::numeric_limits<float>::infinity();
    state.heat_max = -std::numeric_limits<float>::infinity();
    state.normalize_mode =
        layer_idx < layer_normalize_mode.size()
            ? std::clamp(layer_normalize_mode[layer_idx], 0, 3)
            : 0;
    const std::vector<LayerDef::FeatureRecord>* features = !layer.features.empty() ? &layer.features : fallback_features;
    if (!features || layer.heatmap_field.empty()) return state;

    state.heat_values.reserve(features->size());
    for (size_t fi = 0; fi < features->size(); ++fi) {
        const FeatureRenderState* render_state = findFeatureRenderState(cached_feature_render, layer_idx, fi);
        if (render_state && !render_state->visible) continue;
        const LayerDef::FeatureRecord& fg = (*features)[fi];
        float v = 0.0f;
        if (!tryGetFeaturePropertyFloatForColoring(layer, fallback_properties, fg, fi, layer.heatmap_field, v)) continue;
        state.heat_values.push_back(v);
        const std::string group_key = polygonNormalizationGroupKey(fg);
        if (!group_key.empty()) state.heat_values_by_group[group_key].push_back(v);
        state.heat_min = std::min(state.heat_min, v);
        state.heat_max = std::max(state.heat_max, v);
    }
    if (!state.heat_values.empty()) {
        const float clip_pct = std::clamp(
            layer_idx < layer_heatmap_percentile_clip.size()
                ? layer_heatmap_percentile_clip[layer_idx]
                : heatmap_percentile_clip,
            50.0f,
            100.0f);
        std::sort(state.heat_values.begin(), state.heat_values.end());
        state.heat_min = state.heat_values.front();
        if (clip_pct < 100.0f) {
            const size_t max_idx = state.heat_values.size() - 1;
            const size_t kth = (size_t)std::clamp(
                (int)std::floor((clip_pct / 100.0f) * (double)max_idx),
                0,
                (int)max_idx);
            state.heat_max = std::max(state.heat_min, state.heat_values[kth]);
        } else {
            state.heat_max = state.heat_values.back();
        }
    }
    for (auto& kv : state.heat_values_by_group) std::sort(kv.second.begin(), kv.second.end());
    state.heat_range_valid =
        std::isfinite(state.heat_min) && std::isfinite(state.heat_max) && state.heat_max > state.heat_min;
    return state;
}

} // namespace

void syncZoningGpuLayers(const ZoningRuntimeSyncInput& input, ZoningRuntimeState& state) {
    if (!input.root || !input.app_settings || !input.layers || !input.layer_states ||
        !input.map_filter_state || !input.query_layers || !input.zoning_zone_enabled ||
        !input.zoning_zone_color || !input.layer_fill_enabled) {
        return;
    }

    for (size_t li = 0; li < input.layers->size() && li < input.layer_states->size(); ++li) {
        const LayerDef& layer = (*input.layers)[li];
        LayerRuntimeState& layer_state = (*input.layer_states)[li];
        const LayerRenderRoute render_route =
            classifyLayerRenderRoute(li, layer, input.parcel_layer_idx);
        const bool polygon_gpu_route =
            render_route == LayerRenderRoute::GenericPolygonGpu ||
            render_route == LayerRenderRoute::ParcelPolygonGpu;
        const bool polygon_gpu_ready =
            layer.enabled &&
            polygon_gpu_route &&
            layer_state.status == LayerPipelineStatus::Ready &&
            !layer_state.hydration_source_signature.empty();
        if (!polygon_gpu_ready) {
            clearZoningLayerState(li, state);
            state.failed_signatures.erase(li);
            if (layer_state.geometry_artifact_class == GeometryArtifactClass::Polygon) {
                layer_state.geometry_gpu_resident = false;
                layer_state.geometry_gpu_pick_ready = false;
            }
            continue;
        }

        const std::string zoning_signature =
            std::to_string(li) + ":" + layer_state.hydration_source_signature;
        auto failed_it = state.failed_signatures.find(li);
        if (failed_it != state.failed_signatures.end() && failed_it->second == zoning_signature) {
            layer_state.geometry_gpu_resident = false;
            layer_state.geometry_gpu_pick_ready = false;
            continue;
        }
        if (state.uploaded_signatures[li] != zoning_signature) {
            ParcelRenderCacheBlob blob;
            state.color_features.erase(li);
            state.color_feature_properties.erase(li);
            const std::filesystem::path artifact_path =
                geometryArtifactCachePathForLayerFile(
                    *input.root,
                    layer.file,
                    GeometryArtifactClass::Polygon,
                    layerRenderRouteArtifactName(render_route));
            PolygonGeometryArtifact artifact;
            std::string artifact_error;
            if (!loadBinaryPolygonGeometryArtifact(artifact_path, layer_state.hydration_source_signature, artifact) ||
                (!layer.features.empty() && artifact.features.size() != layer.features.size()) ||
                !buildParcelRenderCacheBlobFromPolygonArtifact(artifact, blob, &artifact_error)) {
                std::fprintf(
                    stderr,
                    "[worldsim3][parcel-pick] artifact-invalid layer=%zu file=%s path=%s layer_features=%zu artifact_features=%zu signature=%s detail=%s\n",
                    li,
                    layer.file.c_str(),
                    artifact_path.string().c_str(),
                    layer.features.size(),
                    artifact.features.size(),
                    layer_state.hydration_source_signature.c_str(),
                    artifact_error.c_str());
                clearZoningLayerState(li, state);
                state.failed_signatures[li] = zoning_signature;
                layer_state.geometry_gpu_resident = false;
                layer_state.geometry_gpu_pick_ready = false;
                continue;
            }
            blob.source_signature = zoning_signature;
            std::string zoning_error;
            if (ensureZoningGpuBuffersResident(li, blob, &zoning_error)) {
                state.render_blobs[li] = std::move(blob);
                if (!layer.heatmap_field.empty() || isZoningPolygonLayer(layer)) {
                    std::vector<LayerDef::FeatureRecord> color_features;
                    std::vector<LayerDef::FeatureProperties> color_properties;
                    if (loadCanonicalLayerFeatureCollection(
                            *input.root,
                            layer.file,
                            layer_state.hydration_source_signature,
                            color_features,
                            &color_properties) &&
                        color_features.size() == state.render_blobs[li].features.size()) {
                        state.color_features[li] = std::move(color_features);
                        state.color_feature_properties[li] = std::move(color_properties);
                    }
                }
                state.uploaded_signatures[li] = zoning_signature;
                state.failed_signatures.erase(li);
                state.color_state_keys.erase(li);
                state.outline_state_keys.erase(li);
                state.last_base_colors[li].assign(state.render_blobs[li].features.size(), IM_COL32(0, 0, 0, 0));
                state.last_outline_colors[li].assign(state.render_blobs[li].features.size(), IM_COL32(0, 0, 0, 0));
            } else {
                std::fprintf(
                    stderr,
                    "[worldsim3] Polygon GPU upload failed for layer %zu (%s): %s\n",
                    li,
                    layer.name.c_str(),
                    zoning_error.c_str());
                clearZoningLayerState(li, state);
                state.failed_signatures[li] = zoning_signature;
                layer_state.geometry_gpu_resident = false;
                layer_state.geometry_gpu_pick_ready = false;
                continue;
            }
        }

        auto blob_it = state.render_blobs.find(li);
        if (blob_it == state.render_blobs.end() || blob_it->second.features.empty()) {
            layer_state.geometry_gpu_resident = false;
            layer_state.geometry_gpu_pick_ready = false;
            continue;
        }
        const ParcelRenderCacheBlob& blob = blob_it->second;
        layer_state.geometry_gpu_resident = true;
        layer_state.geometry_gpu_pick_ready = true;
        if (!input.ensure_feature_render_cache) continue;

        const LayerFeatureRenderCache& cached_feature_render = input.ensure_feature_render_cache();
        uint64_t color_state_key = 1469598103934665603ULL;
        hashMix(color_state_key, input.feature_render_state_key);
        hashCString(color_state_key, zoning_signature.c_str());
        hashMix(color_state_key, static_cast<uint64_t>(input.map_filter_state->enabled));
        hashMix(color_state_key, static_cast<uint64_t>(input.map_filter_state->use_date));
        hashMix(color_state_key, static_cast<uint64_t>(input.map_filter_state->year_min));
        hashMix(color_state_key, static_cast<uint64_t>(input.map_filter_state->year_max));
        hashCString(color_state_key, input.map_filter_state->blocklot);
        hashCString(color_state_key, input.map_filter_state->status);
        hashCString(color_state_key, input.map_filter_state->address);
        hashCString(color_state_key, input.map_filter_state->owner);
        hashCString(color_state_key, input.map_filter_state->zip);
        hashMix(color_state_key, static_cast<uint64_t>(input.map_filter_state->selected_owners.size()));
        for (const auto& owner : input.map_filter_state->selected_owners) {
            for (unsigned char ch : owner) hashMix(color_state_key, ch);
        }
        hashMix(color_state_key, static_cast<uint64_t>(input.query_layers->size()));
        for (const auto& ql : *input.query_layers) {
            hashMix(color_state_key, static_cast<uint64_t>(ql.enabled));
            hashMix(color_state_key, static_cast<uint64_t>(ql.result_set.active));
            hashMix(color_state_key, static_cast<uint64_t>(ql.row_count));
            hashMix(color_state_key, static_cast<uint64_t>(ql.result_set.features.size()));
            for (float c : ql.color) hashF32(color_state_key, c);
            for (float c : ql.outline_color) hashF32(color_state_key, c);
        }
        hashMix(color_state_key, static_cast<uint64_t>(input.zoning_zone_color->size()));
        for (const auto& [zone_key, color] : *input.zoning_zone_color) {
            for (unsigned char ch : zone_key) hashMix(color_state_key, ch);
            hashF32(color_state_key, color.x);
            hashF32(color_state_key, color.y);
            hashF32(color_state_key, color.z);
            hashF32(color_state_key, color.w);
        }
        hashMix(
            color_state_key,
            static_cast<uint64_t>(li < input.layer_fill_enabled->size() ? (*input.layer_fill_enabled)[li] : false));
        hashMix(
            color_state_key,
            static_cast<uint64_t>(
                input.layer_heatmap_use_gradient &&
                li < input.layer_heatmap_use_gradient->size() &&
                (*input.layer_heatmap_use_gradient)[li]));
        hashMix(
            color_state_key,
            static_cast<uint64_t>(
                input.layer_normalize_mode && li < input.layer_normalize_mode->size()
                    ? (*input.layer_normalize_mode)[li]
                    : 0));
        hashF32(
            color_state_key,
            input.layer_choropleth_gamma && li < input.layer_choropleth_gamma->size()
                ? (*input.layer_choropleth_gamma)[li]
                : 1.0f);
        hashF32(
            color_state_key,
            input.layer_heatmap_percentile_clip && li < input.layer_heatmap_percentile_clip->size()
                ? (*input.layer_heatmap_percentile_clip)[li]
                : input.heatmap_percentile_clip);
        hashCString(color_state_key, layer.heatmap_field.c_str());
        hashF32(color_state_key, layer.color.x);
        hashF32(color_state_key, layer.color.y);
        hashF32(color_state_key, layer.color.z);
        hashF32(color_state_key, layer.color.w);
        hashMix(color_state_key, static_cast<uint64_t>(input.app_settings->zoning_use_simcity_colors));
        hashF32(color_state_key, input.app_settings->map_polygon_fill_opacity);
        uint64_t outline_state_key = color_state_key;
        hashF32(outline_state_key, layer.outline_color.x);
        hashF32(outline_state_key, layer.outline_color.y);
        hashF32(outline_state_key, layer.outline_color.z);
        hashF32(outline_state_key, layer.outline_color.w);

        if (state.color_state_keys[li] != color_state_key) {
            std::vector<ImU32> zoning_colors(blob.features.size(), IM_COL32(0, 0, 0, 0));
            const ImU32 base_color = ImGui::ColorConvertFloat4ToU32(layer.color);
            const bool use_gradient =
                !layer.heatmap_field.empty() &&
                input.layer_heatmap_use_gradient &&
                li < input.layer_heatmap_use_gradient->size() &&
                (*input.layer_heatmap_use_gradient)[li];
            std::vector<int> default_normalize_mode;
            std::vector<float> default_percentile_clip;
            const std::vector<int>& normalize_mode =
                input.layer_normalize_mode ? *input.layer_normalize_mode : default_normalize_mode;
            const std::vector<float>& percentile_clip =
                input.layer_heatmap_percentile_clip ? *input.layer_heatmap_percentile_clip : default_percentile_clip;
            const auto fallback_features_it = state.color_features.find(li);
            const auto fallback_properties_it = state.color_feature_properties.find(li);
            const std::vector<LayerDef::FeatureRecord>* fallback_features =
                fallback_features_it != state.color_features.end() ? &fallback_features_it->second : nullptr;
            const std::vector<LayerDef::FeatureProperties>* fallback_properties =
                fallback_properties_it != state.color_feature_properties.end() ? &fallback_properties_it->second : nullptr;
            const HeatNormalizationState heat_normalization = use_gradient
                ? buildColoringHeatNormalizationState(
                    layer,
                    fallback_features,
                    fallback_properties,
                    li,
                    normalize_mode,
                    percentile_clip,
                    input.heatmap_percentile_clip,
                    cached_feature_render)
                : HeatNormalizationState{};
            const float gamma =
                input.layer_choropleth_gamma && li < input.layer_choropleth_gamma->size()
                    ? (*input.layer_choropleth_gamma)[li]
                    : 1.0f;
            for (size_t i = 0; i < blob.features.size(); ++i) {
                const uint32_t feature_idx = blob.features[i].feature_idx;
                if (!(li < input.layer_fill_enabled->size() && (*input.layer_fill_enabled)[li])) continue;
                const FeatureRenderState* render_state =
                    findFeatureRenderState(cached_feature_render, li, static_cast<size_t>(feature_idx));
                if (render_state && !render_state->visible) continue;
                ImU32 color = base_color;
                const bool has_live_feature = static_cast<size_t>(feature_idx) < layer.features.size();
                const bool has_fallback_feature =
                    fallback_features && static_cast<size_t>(feature_idx) < fallback_features->size();
                if ((has_live_feature || has_fallback_feature) && use_gradient) {
                    const LayerDef::FeatureRecord& fg = has_live_feature
                        ? layer.features[static_cast<size_t>(feature_idx)]
                        : (*fallback_features)[static_cast<size_t>(feature_idx)];
                    float value = 0.0f;
                    float t = 0.0f;
                    if (tryGetFeaturePropertyFloatForColoring(
                            layer,
                            fallback_properties,
                            fg,
                            static_cast<size_t>(feature_idx),
                            layer.heatmap_field,
                            value) &&
                        heat_normalization.normalizedValue(fg, value, polygonNormalizationGroupKey, t)) {
                        color = ImGui::ColorConvertFloat4ToU32(heatColor(applyPowerGamma(t, gamma)));
                    }
                } else if (isZoningPolygonLayer(layer) && (has_live_feature || has_fallback_feature)) {
                    const LayerDef::FeatureRecord& fg = has_live_feature
                        ? layer.features[static_cast<size_t>(feature_idx)]
                        : (*fallback_features)[static_cast<size_t>(feature_idx)];
                    const std::string zkey =
                        zoningClassKeyForColoring(layer, fallback_properties, fg, static_cast<size_t>(feature_idx));
                    auto it_col = input.zoning_zone_color->find(zkey);
                    if (it_col != input.zoning_zone_color->end()) {
                        color = ImGui::ColorConvertFloat4ToU32(it_col->second);
                    } else if (input.app_settings->zoning_use_simcity_colors) {
                        color = ImGui::ColorConvertFloat4ToU32(
                            zoningShadeVariant(zoningColorFromConvention(zkey), zkey));
                    } else {
                        color = ImGui::ColorConvertFloat4ToU32(
                            zoningShadeVariant(colorFromStableKey(zkey), zkey));
                    }
                }
                if (render_state && render_state->has_query_color) color = render_state->query_color;
                zoning_colors[i] = mapPolygonFillColor(color, input.app_settings->map_polygon_fill_opacity);
            }
            std::string color_error;
            if (updateZoningGpuColorBuffer(li, zoning_colors, &color_error)) {
                state.color_state_keys[li] = color_state_key;
                state.last_base_colors[li] = std::move(zoning_colors);
            }
        }

        if (state.outline_state_keys[li] != outline_state_key) {
            std::vector<ImU32> outline_colors(blob.features.size(), IM_COL32(0, 0, 0, 0));
            const ImU32 outline_color = ImGui::ColorConvertFloat4ToU32(layer.outline_color);
            for (size_t i = 0; i < blob.features.size(); ++i) {
                const uint32_t feature_idx = blob.features[i].feature_idx;
                const FeatureRenderState* render_state =
                    findFeatureRenderState(cached_feature_render, li, static_cast<size_t>(feature_idx));
                if (render_state && !render_state->visible) continue;
                outline_colors[i] = render_state && render_state->has_query_outline_color
                    ? render_state->query_outline_color
                    : outline_color;
            }
            std::string outline_error;
            if (updateZoningGpuOutlineColorBuffer(li, outline_colors, &outline_error)) {
                state.outline_state_keys[li] = outline_state_key;
                state.last_outline_colors[li] = std::move(outline_colors);
            }
        }
    }
}
