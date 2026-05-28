#include "zoning_runtime_service.h"

#include "app_utils.h"
#include "feature_props.h"
#include "render_routing.h"
#include "worldsim_app.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

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

bool buildPolygonGpuBlob(const PolygonGeometryArtifact& artifact, ParcelRenderCacheBlob& out) {
    out = ParcelRenderCacheBlob{};
    out.source_signature = artifact.header.source_signature;
    out.vertices = artifact.vertices;
    out.vertex_feature_refs = artifact.feature_refs;
    out.indices = artifact.fill_indices;
    if (out.indices.empty() && !out.vertices.empty()) out.indices.push_back(0);
    out.line_indices = artifact.line_indices;
    out.features.reserve(artifact.features.size());
    for (const GeometryArtifactFeatureRecord& rec : artifact.features) {
        ParcelRenderFeatureRecord dst;
        dst.feature_idx = rec.feature_idx;
        dst.vertex_offset = rec.vertex_offset;
        dst.vertex_count = rec.vertex_count;
        dst.index_offset = rec.index_offset;
        dst.index_count = rec.index_count;
        dst.line_index_offset = rec.aux_index_offset;
        dst.line_index_count = rec.aux_index_count;
        dst.min_lon = rec.min_lon;
        dst.min_lat = rec.min_lat;
        dst.max_lon = rec.max_lon;
        dst.max_lat = rec.max_lat;
        out.features.push_back(dst);
    }
    out.chunks.reserve(artifact.chunks.size());
    for (const GeometryArtifactChunkRecord& rec : artifact.chunks) {
        ParcelRenderChunkRecord dst;
        dst.chunk_idx = rec.chunk_idx;
        dst.feature_offset = rec.feature_offset;
        dst.feature_count = rec.feature_count;
        dst.vertex_offset = rec.vertex_offset;
        dst.vertex_count = rec.vertex_count;
        dst.index_offset = rec.index_offset;
        dst.index_count = rec.index_count;
        dst.line_index_offset = rec.aux_index_offset;
        dst.line_index_count = rec.aux_index_count;
        dst.min_lon = rec.min_lon;
        dst.min_lat = rec.min_lat;
        dst.max_lon = rec.max_lon;
        dst.max_lat = rec.max_lat;
        out.chunks.push_back(dst);
    }
    return
        !out.vertices.empty() &&
        !out.vertex_feature_refs.empty() &&
        !out.indices.empty() &&
        !out.line_indices.empty() &&
        !out.features.empty();
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
            }
            continue;
        }

        const std::string zoning_signature =
            std::to_string(li) + ":" + layer_state.hydration_source_signature;
        auto failed_it = state.failed_signatures.find(li);
        if (failed_it != state.failed_signatures.end() && failed_it->second == zoning_signature) {
            layer_state.geometry_gpu_resident = false;
            continue;
        }
        if (state.uploaded_signatures[li] != zoning_signature) {
            ParcelRenderCacheBlob blob;
            const std::filesystem::path artifact_path =
                geometryArtifactCachePathForLayerFile(
                    *input.root,
                    layer.file,
                    GeometryArtifactClass::Polygon,
                    layerRenderRouteArtifactName(render_route));
            PolygonGeometryArtifact artifact;
            if (!loadBinaryPolygonGeometryArtifact(artifact_path, layer_state.hydration_source_signature, artifact) ||
                (!layer.features.empty() && artifact.features.size() != layer.features.size()) ||
                !buildPolygonGpuBlob(artifact, blob)) {
                std::fprintf(
                    stderr,
                    "[worldsim3][parcel-pick] artifact-invalid layer=%zu file=%s path=%s layer_features=%zu artifact_features=%zu signature=%s\n",
                    li,
                    layer.file.c_str(),
                    artifact_path.string().c_str(),
                    layer.features.size(),
                    artifact.features.size(),
                    layer_state.hydration_source_signature.c_str());
                clearZoningLayerState(li, state);
                state.failed_signatures[li] = zoning_signature;
                layer_state.geometry_gpu_resident = false;
                continue;
            }
            blob.source_signature = zoning_signature;
            std::string zoning_error;
            if (ensureZoningGpuBuffersResident(li, blob, &zoning_error)) {
                state.render_blobs[li] = std::move(blob);
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
                continue;
            }
        }

        auto blob_it = state.render_blobs.find(li);
        if (blob_it == state.render_blobs.end() || blob_it->second.features.empty()) {
            layer_state.geometry_gpu_resident = false;
            continue;
        }
        const ParcelRenderCacheBlob& blob = blob_it->second;
        layer_state.geometry_gpu_resident = true;
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
        }
        hashMix(color_state_key, static_cast<uint64_t>(input.zoning_zone_enabled->size()));
        for (const auto& [zone_key, enabled] : *input.zoning_zone_enabled) {
            for (unsigned char ch : zone_key) hashMix(color_state_key, ch);
            hashMix(color_state_key, static_cast<uint64_t>(enabled));
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
        hashF32(color_state_key, layer.color.x);
        hashF32(color_state_key, layer.color.y);
        hashF32(color_state_key, layer.color.z);
        hashF32(color_state_key, layer.color.w);
        hashF32(color_state_key, input.app_settings->map_polygon_fill_opacity);
        uint64_t outline_state_key = color_state_key;
        hashF32(outline_state_key, layer.outline_color.x);
        hashF32(outline_state_key, layer.outline_color.y);
        hashF32(outline_state_key, layer.outline_color.z);
        hashF32(outline_state_key, layer.outline_color.w);

        if (state.color_state_keys[li] != color_state_key) {
            std::vector<ImU32> zoning_colors(blob.features.size(), IM_COL32(0, 0, 0, 0));
            const ImU32 base_color = ImGui::ColorConvertFloat4ToU32(layer.color);
            for (size_t i = 0; i < blob.features.size(); ++i) {
                const uint32_t feature_idx = blob.features[i].feature_idx;
                if (!(li < input.layer_fill_enabled->size() && (*input.layer_fill_enabled)[li])) continue;
                const FeatureRenderState* render_state =
                    findFeatureRenderState(cached_feature_render, li, static_cast<size_t>(feature_idx));
                if (render_state && !render_state->visible) continue;
                ImU32 color = base_color;
                if (static_cast<size_t>(feature_idx) < layer.features.size() && isZoningPolygonLayer(layer)) {
                    const LayerDef::FeatureRecord& fg = layer.features[static_cast<size_t>(feature_idx)];
                    const std::string zkey = zoningClassKey(fg);
                    auto it_col = input.zoning_zone_color->find(zkey);
                    if (it_col != input.zoning_zone_color->end()) {
                        color = ImGui::ColorConvertFloat4ToU32(it_col->second);
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
                outline_colors[i] = outline_color;
            }
            std::string outline_error;
            if (updateZoningGpuOutlineColorBuffer(li, outline_colors, &outline_error)) {
                state.outline_state_keys[li] = outline_state_key;
                state.last_outline_colors[li] = std::move(outline_colors);
            }
        }
    }
}
