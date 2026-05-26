#include "crime_point_runtime_service.h"

#include "app_utils.h"
#include "render_routing.h"
#include "worldsim_app.h"

#include "imgui.h"

#include <cstdio>
#include <cstring>

namespace {

void resetCrimePointRuntimeState(LayerRuntimeState& layer_state, CrimePointRuntimeState& state) {
    layer_state.geometry_source_signature.clear();
    layer_state.geometry_phase.clear();
    layer_state.geometry_loaded_from_artifact = false;
    layer_state.geometry_gpu_resident = false;
    clearCrimePointGpuBuffers();
    clearCrimePointGpuDrawState();
    state.uploaded_signature.clear();
    state.color_state_key = 0;
    state.artifact = PointGeometryArtifact{};
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

} // namespace

void syncCrimePointGpuLayer(const CrimePointRuntimeSyncInput& input, CrimePointRuntimeState& state) {
    if (!input.root || !input.layers || !input.layer_states || !input.map_filter_state || !input.query_layers) {
        return;
    }
    if (input.crime_nibrs_layer_idx < 0 ||
        static_cast<size_t>(input.crime_nibrs_layer_idx) >= input.layers->size() ||
        static_cast<size_t>(input.crime_nibrs_layer_idx) >= input.layer_states->size()) {
        clearCrimePointGpuBuffers();
        clearCrimePointGpuDrawState();
        state.uploaded_signature.clear();
        state.color_state_key = 0;
        state.artifact = PointGeometryArtifact{};
        return;
    }

    const size_t layer_idx = static_cast<size_t>(input.crime_nibrs_layer_idx);
    const LayerDef& crime_layer = (*input.layers)[layer_idx];
    LayerRuntimeState& crime_state = (*input.layer_states)[layer_idx];
    const std::filesystem::path crime_artifact_path =
        geometryArtifactCachePathForLayerFile(
            *input.root,
            crime_layer.file,
            GeometryArtifactClass::Point,
            layerRenderRouteArtifactName(LayerRenderRoute::PointGpu));
    crime_state.geometry_artifact_class = GeometryArtifactClass::Point;
    crime_state.geometry_artifact_path = crime_artifact_path.string();

    const bool crime_ready =
        crime_layer.enabled &&
        crime_state.status == LayerPipelineStatus::Ready &&
        !crime_state.hydration_source_signature.empty();
    if (!crime_ready) {
        resetCrimePointRuntimeState(crime_state, state);
        return;
    }

    const std::string& sig = crime_state.hydration_source_signature;
    if (state.uploaded_signature != sig) {
        PointGeometryArtifact point_artifact;
        std::vector<uint32_t> point_glyphs;
        if (!loadBinaryPointGeometryArtifact(crime_artifact_path, sig, point_artifact)) {
            crime_state.geometry_source_signature = sig;
            crime_state.geometry_phase = "artifact_missing";
            crime_state.geometry_loaded_from_artifact = false;
            crime_state.geometry_gpu_resident = false;
            std::fprintf(
                stderr,
                "[worldsim3] Crime point geometry artifact load failed for %s (%s)\n",
                crime_layer.file.c_str(),
                crime_artifact_path.string().c_str());
            resetCrimePointRuntimeState(crime_state, state);
            return;
        }
        if (point_artifact.positions.size() != point_artifact.features.size() ||
            (!crime_layer.features.empty() && point_artifact.features.size() != crime_layer.features.size())) {
            crime_state.geometry_source_signature = sig;
            crime_state.geometry_phase = "artifact_feature_mismatch";
            crime_state.geometry_loaded_from_artifact = false;
            crime_state.geometry_gpu_resident = false;
            std::fprintf(
                stderr,
                "[worldsim3] Crime point geometry artifact feature mismatch for %s: artifact=%zu runtime=%zu\n",
                crime_layer.file.c_str(),
                point_artifact.features.size(),
                crime_layer.features.size());
            resetCrimePointRuntimeState(crime_state, state);
            return;
        }

        point_glyphs.reserve(point_artifact.features.size());
        for (size_t i = 0; i < point_artifact.features.size(); ++i) {
            if (i < crime_layer.features.size()) point_glyphs.push_back(crimePointGlyphCode(crime_layer.features[i]));
            else point_glyphs.push_back(0u);
        }

        std::string gpu_error;
        if (ensureCrimePointGpuBuffersResident(sig, point_artifact.positions, &gpu_error) &&
            updateCrimePointGpuGlyphBuffer(point_glyphs, &gpu_error)) {
            crime_state.geometry_source_signature = sig;
            crime_state.geometry_phase = "gpu_ready";
            crime_state.geometry_loaded_from_artifact = true;
            crime_state.geometry_gpu_resident = true;
            state.artifact = std::move(point_artifact);
            state.uploaded_signature = sig;
            state.color_state_key = 0;
        } else {
            crime_state.geometry_source_signature = sig;
            crime_state.geometry_phase = "gpu_upload_failed";
            crime_state.geometry_loaded_from_artifact = false;
            crime_state.geometry_gpu_resident = false;
            std::fprintf(stderr, "[worldsim3] Crime point GPU upload failed: %s\n", gpu_error.c_str());
            clearCrimePointGpuBuffers();
            clearCrimePointGpuDrawState();
            state.uploaded_signature.clear();
            state.artifact = PointGeometryArtifact{};
            return;
        }
    }

    if (state.uploaded_signature != sig ||
        state.artifact.positions.size() != state.artifact.features.size()) {
        return;
    }

    crime_state.geometry_source_signature = sig;
    crime_state.geometry_phase = "gpu_ready";
    crime_state.geometry_loaded_from_artifact = true;
    crime_state.geometry_gpu_resident = true;

    uint64_t color_state_key = 1469598103934665603ULL;
    hashMix(color_state_key, layer_idx);
    hashMix(color_state_key, static_cast<uint64_t>(crime_layer.enabled));
    hashMix(color_state_key, static_cast<uint64_t>(input.map_filter_state->enabled));
    hashMix(color_state_key, static_cast<uint64_t>(input.map_filter_state->crime.enabled));
    hashMix(color_state_key, static_cast<uint64_t>(input.map_filter_state->crime.homicide));
    hashMix(color_state_key, static_cast<uint64_t>(input.map_filter_state->crime.robbery));
    hashMix(color_state_key, static_cast<uint64_t>(input.map_filter_state->crime.assault));
    hashMix(color_state_key, static_cast<uint64_t>(input.map_filter_state->crime.burglary));
    hashMix(color_state_key, static_cast<uint64_t>(input.map_filter_state->crime.theft));
    hashMix(color_state_key, static_cast<uint64_t>(input.map_filter_state->crime.auto_theft));
    hashMix(color_state_key, static_cast<uint64_t>(input.map_filter_state->crime.drug));
    hashMix(color_state_key, static_cast<uint64_t>(input.map_filter_state->crime.shooting));
    hashMix(color_state_key, static_cast<uint64_t>(input.map_filter_state->crime.use_year));
    hashMix(color_state_key, static_cast<uint64_t>(input.map_filter_state->crime.year_min));
    hashMix(color_state_key, static_cast<uint64_t>(input.map_filter_state->crime.year_max));
    hashMix(color_state_key, static_cast<uint64_t>(input.query_layers->size()));
    for (const auto& ql : *input.query_layers) {
        hashMix(color_state_key, static_cast<uint64_t>(ql.enabled));
        hashMix(color_state_key, static_cast<uint64_t>(ql.result_set.active));
        hashMix(color_state_key, static_cast<uint64_t>(ql.row_count));
        hashMix(color_state_key, static_cast<uint64_t>(ql.result_set.features.size()));
        for (float c : ql.color) hashF32(color_state_key, c);
    }
    hashMix(color_state_key, input.feature_render_state_key);

    if (color_state_key == state.color_state_key) return;
    if (!input.ensure_feature_render_cache) return;

    const LayerFeatureRenderCache& cached_feature_render = input.ensure_feature_render_cache();
    const size_t crime_feature_count = state.artifact.features.size();
    std::vector<ImU32> point_colors(crime_feature_count, IM_COL32(0, 0, 0, 0));
    const ImU32 base_color = ImGui::ColorConvertFloat4ToU32(crime_layer.color);
    for (size_t i = 0; i < crime_feature_count; ++i) {
        const FeatureRenderState* render_state = findFeatureRenderState(cached_feature_render, layer_idx, i);
        ImU32 color = base_color;
        if (render_state) {
            if (!render_state->visible) color = IM_COL32(0, 0, 0, 0);
            else if (render_state->has_query_color) color = render_state->query_color;
        }
        point_colors[i] = color;
    }
    std::string color_error;
    if (updateCrimePointGpuColorBuffer(point_colors, &color_error)) {
        state.color_state_key = color_state_key;
    }
}
