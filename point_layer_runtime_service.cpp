#include "point_layer_runtime_service.h"

#include "app_utils.h"
#include "render_routing.h"
#include "worldsim_app.h"

#include "imgui.h"

#include <cstdio>
#include <cstring>

namespace {

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

uint32_t pointGlyphForLayerFeature(const LayerDef& layer, const LayerDef::FeatureRecord* fg) {
    if (fg && isLikelyCrimePointLayer(layer)) return crimePointGlyphCode(*fg);
    if (containsCaseInsensitive(layer.name, "water")) return 6u;
    if (containsCaseInsensitive(layer.name, "health")) return 5u;
    if (containsCaseInsensitive(layer.name, "school")) return 4u;
    if (containsCaseInsensitive(layer.name, "market")) return 2u;
    if (containsCaseInsensitive(layer.name, "police")) return 2u;
    if (containsCaseInsensitive(layer.name, "church")) return 3u;
    if (containsCaseInsensitive(layer.name, "industry")) return 1u;
    if (containsCaseInsensitive(layer.name, "filling")) return 1u;
    if (containsCaseInsensitive(layer.name, "event") ||
        containsCaseInsensitive(layer.subcategory, "event") ||
        containsCaseInsensitive(layer.duckdb_role, "point_event")) {
        return 6u;
    }
    switch (layer.category) {
        case LayerDef::Category::PublicHealth: return 5u;
        case LayerDef::Category::Infrastructure: return 1u;
        case LayerDef::Category::Safety: return 2u;
        case LayerDef::Category::Zoning: return 4u;
        case LayerDef::Category::Housing:
        default: return 0u;
    }
}

} // namespace

void syncPointGpuLayers(const PointLayerRuntimeSyncInput& input, PointLayerRuntimeState& state) {
    if (!input.root || !input.layers || !input.layer_states || !input.map_filter_state || !input.query_layers) {
        return;
    }

    for (size_t li = 0; li < input.layers->size() && li < input.layer_states->size(); ++li) {
        if (static_cast<int>(li) == input.crime_nibrs_layer_idx) continue;
        const LayerDef& layer = (*input.layers)[li];
        LayerRuntimeState& layer_state = (*input.layer_states)[li];
        if (!layerUsesPointGeometry(layer) ||
            !layer.enabled ||
            layer_state.status != LayerPipelineStatus::Ready ||
            layer_state.hydration_source_signature.empty()) {
            state.geometry_artifacts.erase(li);
            state.artifact_signatures.erase(li);
            clearPointLayerGpuBuffers(li);
            state.color_state_keys.erase(li);
            state.glyph_state_keys.erase(li);
            if (layer_state.geometry_artifact_class == GeometryArtifactClass::Point) {
                layer_state.geometry_source_signature.clear();
                layer_state.geometry_phase.clear();
                layer_state.geometry_loaded_from_artifact = false;
                layer_state.geometry_gpu_resident = false;
            }
            continue;
        }

        const std::filesystem::path artifact_path =
            geometryArtifactCachePathForLayerFile(
                *input.root,
                layer.file,
                GeometryArtifactClass::Point,
                layerRenderRouteArtifactName(LayerRenderRoute::PointGpu));
        layer_state.geometry_artifact_class = GeometryArtifactClass::Point;
        layer_state.geometry_artifact_path = artifact_path.string();
        const std::string& sig = layer_state.hydration_source_signature;

        auto sig_it = state.artifact_signatures.find(li);
        if (sig_it != state.artifact_signatures.end() &&
            sig_it->second == sig &&
            state.geometry_artifacts.find(li) != state.geometry_artifacts.end()) {
            layer_state.geometry_source_signature = sig;
            layer_state.geometry_phase = "artifact_validated";
            layer_state.geometry_loaded_from_artifact = true;
            layer_state.geometry_gpu_resident = pointLayerGpuBuffersResident(li);
        } else {
            PointGeometryArtifact artifact;
            if (!loadBinaryPointGeometryArtifact(artifact_path, sig, artifact) ||
                artifact.positions.size() != artifact.features.size() ||
                (!layer.features.empty() && artifact.features.size() != layer.features.size())) {
                state.geometry_artifacts.erase(li);
                state.artifact_signatures.erase(li);
                clearPointLayerGpuBuffers(li);
                state.color_state_keys.erase(li);
                state.glyph_state_keys.erase(li);
                layer_state.geometry_source_signature = sig;
                layer_state.geometry_phase = "artifact_missing";
                layer_state.geometry_loaded_from_artifact = false;
                layer_state.geometry_gpu_resident = false;
                continue;
            }

            state.geometry_artifacts[li] = std::move(artifact);
            state.artifact_signatures[li] = sig;
            layer_state.geometry_source_signature = sig;
            std::string gpu_error;
            if (ensurePointLayerGpuBuffersResident(li, state.geometry_artifacts[li], &gpu_error)) {
                layer_state.geometry_phase = "gpu_ready";
                layer_state.geometry_gpu_resident = true;
            } else {
                std::fprintf(
                    stderr,
                    "[worldsim3] Point GPU upload failed for layer %zu (%s): %s\n",
                    li,
                    layer.name.c_str(),
                    gpu_error.c_str());
                clearPointLayerGpuBuffers(li);
                state.color_state_keys.erase(li);
                state.glyph_state_keys.erase(li);
                layer_state.geometry_phase = "gpu_upload_failed";
                layer_state.geometry_gpu_resident = false;
            }
            layer_state.geometry_loaded_from_artifact = true;
        }

        if (!layer_state.geometry_gpu_resident) continue;
        if (!input.ensure_feature_render_cache) continue;

        const LayerFeatureRenderCache& cached_feature_render = input.ensure_feature_render_cache();
        uint64_t color_state_key = 1469598103934665603ULL;
        hashMix(color_state_key, input.feature_render_state_key);
        hashMix(color_state_key, li);
        hashCString(color_state_key, sig.c_str());
        hashMix(color_state_key, static_cast<uint64_t>(layer.enabled));
        hashMix(color_state_key, static_cast<uint64_t>(input.map_filter_state->enabled));
        hashMix(color_state_key, static_cast<uint64_t>(input.query_layers->size()));
        for (const auto& ql : *input.query_layers) {
            hashMix(color_state_key, static_cast<uint64_t>(ql.enabled));
            hashMix(color_state_key, static_cast<uint64_t>(ql.result_set.active));
            hashMix(color_state_key, static_cast<uint64_t>(ql.row_count));
            hashMix(color_state_key, static_cast<uint64_t>(ql.result_set.features.size()));
            for (float c : ql.color) hashF32(color_state_key, c);
        }
        hashF32(color_state_key, layer.color.x);
        hashF32(color_state_key, layer.color.y);
        hashF32(color_state_key, layer.color.z);
        hashF32(color_state_key, layer.color.w);

        if (state.color_state_keys[li] != color_state_key) {
            const size_t point_feature_count = state.geometry_artifacts[li].features.size();
            std::vector<ImU32> point_colors(point_feature_count, IM_COL32(0, 0, 0, 0));
            const ImU32 base_color = ImGui::ColorConvertFloat4ToU32(layer.color);
            for (size_t i = 0; i < point_feature_count; ++i) {
                const FeatureRenderState* render_state = findFeatureRenderState(cached_feature_render, li, i);
                ImU32 color = base_color;
                if (render_state) {
                    if (!render_state->visible) color = IM_COL32(0, 0, 0, 0);
                    else if (render_state->has_query_color) color = render_state->query_color;
                }
                point_colors[i] = color;
            }
            std::string color_error;
            if (updatePointLayerGpuColorBuffer(li, point_colors, &color_error)) {
                state.color_state_keys[li] = color_state_key;
            }
        }

        const uint64_t glyph_state_key = color_state_key ^ 0x9f8d4c53b1a2401dULL;
        if (state.glyph_state_keys[li] != glyph_state_key) {
            const size_t point_feature_count = state.geometry_artifacts[li].features.size();
            std::vector<uint32_t> glyph_codes(point_feature_count, 0u);
            for (size_t i = 0; i < point_feature_count; ++i) {
                const LayerDef::FeatureRecord* fg = i < layer.features.size() ? &layer.features[i] : nullptr;
                glyph_codes[i] = pointGlyphForLayerFeature(layer, fg);
            }
            std::string glyph_error;
            if (updatePointLayerGpuGlyphBuffer(li, glyph_codes, &glyph_error)) {
                state.glyph_state_keys[li] = glyph_state_key;
            }
        }
    }
}
