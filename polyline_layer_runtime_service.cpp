#include "polyline_layer_runtime_service.h"

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

} // namespace

void syncPolylineGpuLayers(const PolylineLayerRuntimeSyncInput& input, PolylineLayerRuntimeState& state) {
    if (!input.root || !input.layers || !input.layer_states || !input.map_filter_state || !input.query_layers) {
        return;
    }

    for (size_t li = 0; li < input.layers->size() && li < input.layer_states->size(); ++li) {
        const LayerDef& layer = (*input.layers)[li];
        LayerRuntimeState& layer_state = (*input.layer_states)[li];
        if (!layerUsesPolylineGeometry(layer) ||
            !layer.enabled ||
            layer_state.status != LayerPipelineStatus::Ready ||
            layer_state.hydration_source_signature.empty()) {
            state.geometry_artifacts.erase(li);
            state.artifact_signatures.erase(li);
            clearPolylineLayerGpuBuffers(li);
            state.color_state_keys.erase(li);
            if (layer_state.geometry_artifact_class == GeometryArtifactClass::Polyline) {
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
                GeometryArtifactClass::Polyline,
                layerRenderRouteArtifactName(LayerRenderRoute::PolylineGpu));
        layer_state.geometry_artifact_class = GeometryArtifactClass::Polyline;
        layer_state.geometry_artifact_path = artifact_path.string();
        const std::string& sig = layer_state.hydration_source_signature;
        auto sig_it = state.artifact_signatures.find(li);
        if (sig_it != state.artifact_signatures.end() &&
            sig_it->second == sig &&
            state.geometry_artifacts.find(li) != state.geometry_artifacts.end()) {
            layer_state.geometry_source_signature = sig;
            layer_state.geometry_phase = "artifact_validated";
            layer_state.geometry_loaded_from_artifact = true;
            layer_state.geometry_gpu_resident = polylineLayerGpuBuffersResident(li);
        } else {
            PolylineGeometryArtifact artifact;
            if (!loadBinaryPolylineGeometryArtifact(artifact_path, sig, artifact) ||
                (!layer.features.empty() && artifact.features.size() != layer.features.size())) {
                state.geometry_artifacts.erase(li);
                state.artifact_signatures.erase(li);
                clearPolylineLayerGpuBuffers(li);
                state.color_state_keys.erase(li);
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
            if (ensurePolylineLayerGpuBuffersResident(li, state.geometry_artifacts[li], &gpu_error)) {
                layer_state.geometry_phase = "gpu_ready";
                layer_state.geometry_gpu_resident = true;
            } else {
                std::fprintf(
                    stderr,
                    "[worldsim3] Polyline GPU upload failed for layer %zu (%s): %s\n",
                    li,
                    layer.name.c_str(),
                    gpu_error.c_str());
                clearPolylineLayerGpuBuffers(li);
                state.color_state_keys.erase(li);
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
            const size_t line_feature_count = state.geometry_artifacts[li].features.size();
            std::vector<ImU32> line_colors(line_feature_count, IM_COL32(0, 0, 0, 0));
            const ImU32 base_color = ImGui::ColorConvertFloat4ToU32(layer.color);
            for (size_t i = 0; i < line_feature_count; ++i) {
                const FeatureRenderState* render_state = findFeatureRenderState(cached_feature_render, li, i);
                ImU32 color = base_color;
                if (render_state) {
                    if (!render_state->visible) color = IM_COL32(0, 0, 0, 0);
                    else if (render_state->has_query_color) color = render_state->query_color;
                }
                line_colors[i] = color;
            }
            std::string color_error;
            if (updatePolylineLayerGpuColorBuffer(li, line_colors, &color_error)) {
                state.color_state_keys[li] = color_state_key;
            }
        }
    }
}
