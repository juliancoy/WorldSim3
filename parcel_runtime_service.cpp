#include "parcel_runtime_service.h"

#include "app_utils.h"
#include "choropleth_histogram.h"
#include "map_render_utils.h"
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

void clearParcelRuntimeState(ParcelRuntimeState& state, bool clear_blob) {
    clearParcelGpuBuffers();
    state.uploaded_signature.clear();
    state.render_requested_signature.clear();
    state.gpu_upload_requested_signature.clear();
    state.filter_state_key = 0;
    state.overlay_state_key = 0;
    state.outline_state_key = 0;
    state.last_base_colors.clear();
    state.last_overlay_colors.clear();
    state.last_outline_colors.clear();
    if (clear_blob) state.render_blob = ParcelRenderCacheBlob{};
}

bool layerEnabledAt(const std::vector<LayerDef>& layers, int idx) {
    return idx >= 0 && static_cast<size_t>(idx) < layers.size() && layers[static_cast<size_t>(idx)].enabled;
}

bool layerFillEnabledAt(const std::vector<bool>* enabled, int idx) {
    return enabled && idx >= 0 && static_cast<size_t>(idx) < enabled->size() && (*enabled)[static_cast<size_t>(idx)];
}

int valueAt(const std::vector<int>* values, size_t idx) {
    return values && idx < values->size() ? (*values)[idx] : 0;
}

std::string parcelEntityIdForRenderFeature(const ParcelRenderFeatureRecord& rec) {
    return rec.entity_id;
}

} // namespace

ImU32 resolveParcelRuntimeBaseFillColor(
    const FeatureRenderState* render_state,
    ImU32 base_color,
    float base_opacity,
    bool value_color_valid,
    ImU32 value_color) {
    if (render_state && !render_state->visible) return IM_COL32(0, 0, 0, 0);
    if (render_state && render_state->has_query_color) return render_state->query_color;
    if (value_color_valid) return mapPolygonFillColor(value_color, base_opacity);
    return mapPolygonFillColor(base_color, base_opacity);
}

ImU32 resolveParcelRuntimeOverlayColor(
    const FeatureRenderState* render_state,
    ImU32 domain_overlay_color) {
    if (render_state && !render_state->visible) return IM_COL32(0, 0, 0, 0);
    if (render_state && render_state->has_query_color) return render_state->query_color;
    return domain_overlay_color;
}

void syncParcelGpuLayer(const ParcelRuntimeSyncInput& input, ParcelRuntimeState& state) {
    if (!input.root || !input.app_settings || !input.layers || !input.layer_states ||
        !input.layer_choropleth_gamma || !input.layer_heatmap_percentile_clip ||
        !input.layer_normalize_mode || !input.layer_fill_enabled || !input.map_filter_state ||
        !input.parcel_jurisdiction_result_set || !input.query_layers || !input.unified_parcels ||
        !input.selected_parcel_id_set ||
        !input.gpu_profiler_reload_requested || !input.ensure_feature_render_cache ||
        !input.parcel_area_sq_m) {
        return;
    }

    if (input.parcel_layer_idx < 0 ||
        static_cast<size_t>(input.parcel_layer_idx) >= input.layers->size() ||
        static_cast<size_t>(input.parcel_layer_idx) >= input.layer_states->size()) {
        return;
    }

    const LayerDef& parcel_layer = (*input.layers)[static_cast<size_t>(input.parcel_layer_idx)];
    LayerRuntimeState& parcel_state = (*input.layer_states)[static_cast<size_t>(input.parcel_layer_idx)];

    if (*input.gpu_profiler_reload_requested) {
        clearGpuProfilerAlertState();
        clearParcelRuntimeState(state, false);
        *input.gpu_profiler_reload_requested = false;
    }

    const bool parcel_ready =
        parcel_state.status == LayerPipelineStatus::Ready &&
        !parcel_state.hydration_source_signature.empty();
    if (!parcel_ready) {
        parcel_state.geometry_gpu_resident = false;
        parcel_state.geometry_gpu_pick_ready = false;
        if (state.geometry_locked_signature.empty()) {
            clearParcelRuntimeState(state, true);
        }
        return;
    }

    const std::string& sig = parcel_state.hydration_source_signature;
    const LayerRenderRoute render_route = classifyLayerRenderRoute(
        static_cast<size_t>(input.parcel_layer_idx),
        parcel_layer,
        input.parcel_layer_idx);
    const std::filesystem::path artifact_path =
        geometryArtifactCachePathForLayerFile(
            *input.root,
            parcel_layer.file,
            GeometryArtifactClass::Polygon,
            layerRenderRouteArtifactName(render_route));
    const bool parcel_geometry_refresh_allowed =
        state.geometry_locked_signature.empty() || sig == state.geometry_locked_signature;
    if (!parcel_geometry_refresh_allowed) {
        if (state.geometry_restart_required_signature != sig) {
            std::fprintf(
                stderr,
                "[worldsim3] Parcel geometry source changed from %s to %s after startup. "
                "Parcel GPU geometry is session-static; restart required for geometry refresh.\n",
                state.geometry_locked_signature.c_str(),
                sig.c_str());
                state.geometry_restart_required_signature = sig;
        }
    } else if (state.render_requested_signature != sig && state.uploaded_signature != sig) {
        PolygonGeometryArtifact artifact;
        ParcelRenderCacheBlob blob;
        std::string blob_error;
        if (!loadBinaryPolygonGeometryArtifact(artifact_path, sig, artifact) ||
            !buildParcelRenderCacheBlobFromPolygonArtifact(artifact, blob, &blob_error)) {
            std::fprintf(
                stderr,
                "[worldsim3] Parcel polygon geometry artifact missing or invalid: %s%s%s\n",
                artifact_path.string().c_str(),
                blob_error.empty() ? "" : " error=",
                blob_error.empty() ? "" : blob_error.c_str());
            recordGpuProfilerEvent("parcel polygon artifact unavailable");
            return;
        }
        state.render_blob = std::move(blob);
        state.render_requested_signature = sig;
        if (state.gpu_upload_requested_signature != sig) {
            std::string gpu_error;
            if (requestParcelGpuUpload(state.render_blob, &gpu_error)) {
                recordGpuProfilerEvent("parcel GPU upload requested");
                state.gpu_upload_requested_signature = sig;
            } else {
                std::fprintf(stderr, "[worldsim3] Parcel GPU upload request failed: %s\n", gpu_error.c_str());
                recordGpuProfilerEvent("parcel GPU upload request failed");
            }
        }
    }

    if (parcel_geometry_refresh_allowed &&
        state.render_requested_signature == sig &&
        state.uploaded_signature != sig &&
        !state.render_blob.features.empty() &&
        state.gpu_upload_requested_signature != sig) {
        std::string gpu_error;
        if (requestParcelGpuUpload(state.render_blob, &gpu_error)) {
            recordGpuProfilerEvent("parcel GPU upload requested");
            state.gpu_upload_requested_signature = sig;
        } else {
            std::fprintf(stderr, "[worldsim3] Parcel GPU upload request failed: %s\n", gpu_error.c_str());
            recordGpuProfilerEvent("parcel GPU upload request failed");
        }
    }

    if (parcel_geometry_refresh_allowed) {
        std::string adopted_signature;
        std::string upload_error;
        if (drainParcelGpuUploadResults(&sig, &adopted_signature, &upload_error)) {
            if (adopted_signature == sig) {
                state.uploaded_signature = sig;
                if (state.geometry_locked_signature.empty()) state.geometry_locked_signature = sig;
                state.geometry_restart_required_signature.clear();
                state.gpu_upload_requested_signature.clear();
                state.filter_state_key = 0;
                state.overlay_state_key = 0;
                state.outline_state_key = 0;
                state.last_base_colors.assign(state.render_blob.features.size(), IM_COL32(0, 0, 0, 0));
                state.last_overlay_colors.assign(state.render_blob.features.size(), IM_COL32(0, 0, 0, 0));
                state.last_outline_colors.assign(state.render_blob.features.size(), IM_COL32(0, 0, 0, 0));
            }
        } else if (!upload_error.empty()) {
            std::fprintf(stderr, "[worldsim3] Parcel GPU upload failed: %s\n", upload_error.c_str());
            recordGpuProfilerEvent("parcel GPU upload failed");
            state.gpu_upload_requested_signature.clear();
        }
    }

    if (state.uploaded_signature != sig || !parcel_geometry_refresh_allowed || state.render_blob.features.empty()) {
        return;
    }

    parcel_state.geometry_gpu_resident = true;
    parcel_state.geometry_gpu_pick_ready = true;
    parcel_state.geometry_phase = "gpu_ready";

    uint64_t color_state_key = 1469598103934665603ULL;
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
    hashMix(color_state_key, static_cast<uint64_t>(input.parcel_jurisdiction_result_set->active));
    hashMix(color_state_key, static_cast<uint64_t>(input.parcel_jurisdiction_result_set->features.size()));
    hashMix(color_state_key, static_cast<uint64_t>(input.parcel_jurisdiction_result_set->blocklots.size()));
    hashMix(color_state_key, static_cast<uint64_t>(input.parcel_jurisdiction_result_set->owners.size()));
    hashMix(color_state_key, static_cast<uint64_t>(input.query_layers->size()));
    for (const auto& ql : *input.query_layers) {
        hashMix(color_state_key, static_cast<uint64_t>(ql.enabled));
        hashMix(color_state_key, static_cast<uint64_t>(ql.result_set.active));
        hashMix(color_state_key, static_cast<uint64_t>(ql.row_count));
        hashMix(color_state_key, static_cast<uint64_t>(ql.result_set.features.size()));
        hashMix(color_state_key, static_cast<uint64_t>(ql.result_set.blocklots.size()));
        hashMix(color_state_key, static_cast<uint64_t>(ql.result_set.owners.size()));
        for (float c : ql.color) hashF32(color_state_key, c);
        for (float c : ql.outline_color) hashF32(color_state_key, c);
    }

    uint64_t overlay_state_key = color_state_key;
    hashMix(overlay_state_key, static_cast<uint64_t>(input.parcel_parameter_mode));
    hashMix(color_state_key, static_cast<uint64_t>(input.parcel_parameter_mode));
    uint32_t gamma_bits = 0;
    if (static_cast<size_t>(input.parcel_layer_idx) < input.layer_choropleth_gamma->size()) {
        std::memcpy(&gamma_bits, &(*input.layer_choropleth_gamma)[static_cast<size_t>(input.parcel_layer_idx)], sizeof(gamma_bits));
    }
    uint32_t polygon_fill_opacity_bits = 0;
    std::memcpy(&polygon_fill_opacity_bits, &input.app_settings->map_polygon_fill_opacity, sizeof(polygon_fill_opacity_bits));
    hashMix(overlay_state_key, gamma_bits);
    hashMix(color_state_key, gamma_bits);
    hashMix(color_state_key, polygon_fill_opacity_bits);
    hashMix(overlay_state_key, static_cast<uint64_t>(input.layer_fill_enabled->size()));
    hashMix(overlay_state_key, static_cast<uint64_t>(layerFillEnabledAt(input.layer_fill_enabled, input.vacant_notice_layer_idx)));
    hashMix(overlay_state_key, static_cast<uint64_t>(layerFillEnabledAt(input.layer_fill_enabled, input.vacant_rehab_layer_idx)));
    hashMix(overlay_state_key, static_cast<uint64_t>(layerFillEnabledAt(input.layer_fill_enabled, input.tax_lien_layer_idx)));
    hashMix(overlay_state_key, static_cast<uint64_t>(layerFillEnabledAt(input.layer_fill_enabled, input.tax_sale_layer_idx)));
    hashMix(overlay_state_key, static_cast<uint64_t>(layerEnabledAt(*input.layers, input.parcel_layer_idx)));
    hashMix(overlay_state_key, static_cast<uint64_t>(layerEnabledAt(*input.layers, input.vacant_notice_layer_idx)));
    hashMix(overlay_state_key, static_cast<uint64_t>(layerEnabledAt(*input.layers, input.vacant_rehab_layer_idx)));
    hashMix(overlay_state_key, static_cast<uint64_t>(layerEnabledAt(*input.layers, input.tax_lien_layer_idx)));
    hashMix(overlay_state_key, static_cast<uint64_t>(layerEnabledAt(*input.layers, input.tax_sale_layer_idx)));
    hashMix(overlay_state_key, static_cast<uint64_t>(input.selected_parcel_id_set->size()));
    for (const std::string& selected_id : *input.selected_parcel_id_set) {
        for (unsigned char ch : selected_id) hashMix(overlay_state_key, ch);
    }
    uint64_t outline_state_key = overlay_state_key;

    const LayerFeatureRenderCache& cached_feature_render = input.ensure_feature_render_cache();
    auto query_style_for_feature = [&](uint32_t feature_idx, ImU32& out_fill, ImU32& out_outline) {
        if (!input.query_layers) return false;
        const FeatureKey key{static_cast<size_t>(input.parcel_layer_idx), static_cast<size_t>(feature_idx)};
        for (auto it = input.query_layers->rbegin(); it != input.query_layers->rend(); ++it) {
            if (!it->enabled || !it->result_set.active) continue;
            if (it->result_set.features.find(key) == it->result_set.features.end()) continue;
            out_fill = ImGui::ColorConvertFloat4ToU32(ImVec4(it->color[0], it->color[1], it->color[2], it->color[3]));
            out_outline = ImGui::ColorConvertFloat4ToU32(ImVec4(
                it->outline_color[0],
                it->outline_color[1],
                it->outline_color[2],
                it->outline_color[3]));
            return true;
        }
        return false;
    };
    hashMix(color_state_key, input.feature_render_state_key);
    hashMix(overlay_state_key, input.feature_render_state_key);
    hashMix(outline_state_key, input.feature_render_state_key);
    hashMix(color_state_key, static_cast<uint64_t>(layerEnabledAt(*input.layers, input.parcel_layer_idx)));
    hashMix(color_state_key, static_cast<uint64_t>(layerFillEnabledAt(input.layer_fill_enabled, input.parcel_layer_idx)));
    hashF32(color_state_key, parcel_layer.color.x);
    hashF32(color_state_key, parcel_layer.color.y);
    hashF32(color_state_key, parcel_layer.color.z);
    hashF32(color_state_key, parcel_layer.color.w);

    if (color_state_key != state.filter_state_key) {
        std::vector<ImU32> parcel_colors(state.render_blob.features.size(), IM_COL32(0, 0, 0, 0));
        const ImU32 base_color = ImGui::ColorConvertFloat4ToU32(parcel_layer.color);
        const bool value_choropleth_enabled =
            (input.parcel_parameter_mode == 2 || input.parcel_parameter_mode == 3) &&
            layerEnabledAt(*input.layers, input.parcel_layer_idx);
        const size_t parcel_value_control_layer_idx =
            input.property_value_layer_idx >= 0 ? static_cast<size_t>(input.property_value_layer_idx)
                                                : static_cast<size_t>(input.parcel_layer_idx);
        const float parcel_gamma =
            parcel_value_control_layer_idx < input.layer_choropleth_gamma->size()
                ? (*input.layer_choropleth_gamma)[parcel_value_control_layer_idx]
                : 1.0f;
        const float parcel_clip =
            parcel_value_control_layer_idx < input.layer_heatmap_percentile_clip->size()
                ? (*input.layer_heatmap_percentile_clip)[parcel_value_control_layer_idx]
                : 100.0f;
        const int parcel_normalize_mode =
            parcel_value_control_layer_idx < input.layer_normalize_mode->size()
                ? std::clamp((*input.layer_normalize_mode)[parcel_value_control_layer_idx], 0, 3)
                : 1;
        auto current_value_at = [&](size_t parcel_idx) -> double {
            if (parcel_idx >= input.unified_parcels->size()) return 0.0;
            return (*input.unified_parcels)[parcel_idx].current_value;
        };
        auto current_value_per_area_at = [&](size_t parcel_idx) -> double {
            const double area = input.parcel_area_sq_m(parcel_idx);
            if (!(area > 0.0) || !std::isfinite(area)) return 0.0;
            const double value = current_value_at(parcel_idx);
            return value > 0.0 && std::isfinite(value) ? value / area : 0.0;
        };
        std::vector<double> value_samples;
        if (value_choropleth_enabled) {
            value_samples.reserve(state.render_blob.features.size());
            for (const ParcelRenderFeatureRecord& rec : state.render_blob.features) {
                const uint32_t feature_idx = rec.feature_idx;
                const FeatureRenderState* render_state =
                    findFeatureRenderState(cached_feature_render, static_cast<size_t>(input.parcel_layer_idx), feature_idx);
                if (render_state && !render_state->visible) continue;
                const double v = input.parcel_parameter_mode == 3 ? current_value_per_area_at(feature_idx) : current_value_at(feature_idx);
                if (v > 0.0 && std::isfinite(v)) value_samples.push_back(v);
            }
        }
        const ApproxHistogram value_hist = buildApproxHistogram(value_samples, parcel_clip);
        const bool value_range_valid = value_choropleth_enabled && value_hist.rangeValid();
        for (size_t i = 0; i < state.render_blob.features.size(); ++i) {
            const uint32_t feature_idx = state.render_blob.features[i].feature_idx;
            const FeatureRenderState* render_state =
                findFeatureRenderState(cached_feature_render, static_cast<size_t>(input.parcel_layer_idx), feature_idx);
            ImU32 query_fill = IM_COL32(0, 0, 0, 0);
            ImU32 query_outline = IM_COL32(0, 0, 0, 0);
            const bool has_direct_query_style = query_style_for_feature(feature_idx, query_fill, query_outline);
            bool value_color_valid_for_feature = false;
            ImU32 value_color_for_feature = IM_COL32(0, 0, 0, 0);
            if (value_range_valid) {
                const double v = input.parcel_parameter_mode == 3 ? current_value_per_area_at(feature_idx) : current_value_at(feature_idx);
                if (v > 0.0 && std::isfinite(v)) {
                    const float normalized =
                        parcel_normalize_mode == 0 ? value_hist.normalizeLinear(v)
                        : (parcel_normalize_mode == 3 ? value_hist.normalizeEqualCountZones(v) : value_hist.normalizeApproxPercentile(v));
                    const float t = applyPowerGamma(normalized, parcel_gamma);
                    value_color_for_feature = ImGui::ColorConvertFloat4ToU32(heatColor(t));
                    value_color_valid_for_feature = true;
                }
            }
            parcel_colors[i] = has_direct_query_style
                ? query_fill
                : resolveParcelRuntimeBaseFillColor(
                    render_state,
                    base_color,
                    input.app_settings->map_polygon_fill_opacity,
                    value_color_valid_for_feature,
                    value_color_for_feature);
        }
        std::string color_error;
        if (updateParcelGpuColorBuffer(parcel_colors, &color_error)) {
            state.filter_state_key = color_state_key;
            state.last_base_colors = parcel_colors;
        }
    }

    const ImVec4 lien_c =
        layerEnabledAt(*input.layers, input.tax_lien_layer_idx)
            ? (*input.layers)[static_cast<size_t>(input.tax_lien_layer_idx)].color
            : ImVec4(0.95f, 0.55f, 0.1f, 1.0f);
    const ImVec4 sale_c =
        layerEnabledAt(*input.layers, input.tax_sale_layer_idx)
            ? (*input.layers)[static_cast<size_t>(input.tax_sale_layer_idx)].color
            : ImVec4(0.85f, 0.2f, 0.1f, 1.0f);
    const ImVec4 notice_c =
        layerEnabledAt(*input.layers, input.vacant_notice_layer_idx)
            ? (*input.layers)[static_cast<size_t>(input.vacant_notice_layer_idx)].color
            : ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
    const ImVec4 rehab_c =
        layerEnabledAt(*input.layers, input.vacant_rehab_layer_idx)
            ? (*input.layers)[static_cast<size_t>(input.vacant_rehab_layer_idx)].color
            : ImVec4(0.0f, 1.0f, 1.0f, 1.0f);
    hashF32(overlay_state_key, notice_c.x);
    hashF32(overlay_state_key, notice_c.y);
    hashF32(overlay_state_key, notice_c.z);
    hashF32(overlay_state_key, notice_c.w);
    hashF32(overlay_state_key, rehab_c.x);
    hashF32(overlay_state_key, rehab_c.y);
    hashF32(overlay_state_key, rehab_c.z);
    hashF32(overlay_state_key, rehab_c.w);
    hashF32(overlay_state_key, lien_c.x);
    hashF32(overlay_state_key, lien_c.y);
    hashF32(overlay_state_key, lien_c.z);
    hashF32(overlay_state_key, lien_c.w);
    hashF32(overlay_state_key, sale_c.x);
    hashF32(overlay_state_key, sale_c.y);
    hashF32(overlay_state_key, sale_c.z);
    hashF32(overlay_state_key, sale_c.w);
    outline_state_key = overlay_state_key;

    if (overlay_state_key != state.overlay_state_key) {
        auto parameter_value = [&](size_t parcel_idx) -> double {
            switch (input.parcel_parameter_mode) {
                case 1: return input.parcel_area_sq_m(parcel_idx);
                case 2: {
                    if (parcel_idx >= input.unified_parcels->size()) return 0.0;
                    return (*input.unified_parcels)[parcel_idx].current_value;
                }
                case 3: {
                    const double area = input.parcel_area_sq_m(parcel_idx);
                    if (parcel_idx >= input.unified_parcels->size() || !(area > 0.0) || !std::isfinite(area)) return 0.0;
                    const double value = (*input.unified_parcels)[parcel_idx].current_value;
                    return value > 0.0 && std::isfinite(value) ? value / area : 0.0;
                }
                default: return 0.0;
            }
        };
        std::vector<ImU32> overlay_colors(state.render_blob.features.size(), IM_COL32(0, 0, 0, 0));
        const bool parameter_fill_enabled = input.parcel_parameter_mode == 1 && layerEnabledAt(*input.layers, input.parcel_layer_idx);
        const bool vacancy_fill_enabled =
            (layerEnabledAt(*input.layers, input.vacant_notice_layer_idx) ||
             layerEnabledAt(*input.layers, input.vacant_rehab_layer_idx)) &&
            (layerFillEnabledAt(input.layer_fill_enabled, input.vacant_notice_layer_idx) ||
             layerFillEnabledAt(input.layer_fill_enabled, input.vacant_rehab_layer_idx));
        const bool tax_fill_enabled =
            (layerEnabledAt(*input.layers, input.tax_lien_layer_idx) ||
             layerEnabledAt(*input.layers, input.tax_sale_layer_idx)) &&
            (layerFillEnabledAt(input.layer_fill_enabled, input.tax_lien_layer_idx) ||
             layerFillEnabledAt(input.layer_fill_enabled, input.tax_sale_layer_idx));
        std::vector<double> parameter_samples;
        if (parameter_fill_enabled) {
            parameter_samples.reserve(state.render_blob.features.size());
            for (const ParcelRenderFeatureRecord& rec : state.render_blob.features) {
                const uint32_t feature_idx = rec.feature_idx;
                const FeatureRenderState* render_state =
                    findFeatureRenderState(cached_feature_render, static_cast<size_t>(input.parcel_layer_idx), feature_idx);
                if (render_state && !render_state->visible) continue;
                const double v = parameter_value(feature_idx);
                if (v > 0.0 && std::isfinite(v)) parameter_samples.push_back(v);
            }
        }
        const float parameter_clip =
            static_cast<size_t>(input.parcel_layer_idx) < input.layer_heatmap_percentile_clip->size()
                ? (*input.layer_heatmap_percentile_clip)[static_cast<size_t>(input.parcel_layer_idx)]
                : 100.0f;
        const int parameter_normalize_mode =
            static_cast<size_t>(input.parcel_layer_idx) < input.layer_normalize_mode->size()
                ? std::clamp((*input.layer_normalize_mode)[static_cast<size_t>(input.parcel_layer_idx)], 0, 3)
                : 1;
        const ApproxHistogram parameter_hist = buildApproxHistogram(parameter_samples, parameter_clip);
        const bool parameter_range_valid = parameter_fill_enabled && parameter_hist.rangeValid();
        const ImU32 selected_overlay = IM_COL32(255, 230, 0, 112);
        const float parcel_gamma =
            static_cast<size_t>(input.parcel_layer_idx) < input.layer_choropleth_gamma->size()
                ? (*input.layer_choropleth_gamma)[static_cast<size_t>(input.parcel_layer_idx)]
                : 1.0f;
        for (size_t i = 0; i < state.render_blob.features.size(); ++i) {
            const uint32_t feature_idx = state.render_blob.features[i].feature_idx;
            const FeatureRenderState* render_state =
                findFeatureRenderState(cached_feature_render, static_cast<size_t>(input.parcel_layer_idx), feature_idx);
            if (render_state && !render_state->visible) continue;
            ImU32 query_fill = IM_COL32(0, 0, 0, 0);
            ImU32 query_outline = IM_COL32(0, 0, 0, 0);
            const bool has_direct_query_style = query_style_for_feature(feature_idx, query_fill, query_outline);
            ImU32 overlay = IM_COL32(0, 0, 0, 0);
            if (parameter_range_valid) {
                const double v = parameter_value(feature_idx);
                if (v > 0.0 && std::isfinite(v)) {
                    const float normalized =
                        parameter_normalize_mode == 0 ? parameter_hist.normalizeLinear(v)
                        : (parameter_normalize_mode == 3 ? parameter_hist.normalizeEqualCountZones(v) : parameter_hist.normalizeApproxPercentile(v));
                    const float t = applyPowerGamma(normalized, parcel_gamma);
                    overlay = colorWithAlpha(heatColor(t), 150);
                }
            }
            const int vac_notice = valueAt(input.parcel_vac_notice_by_feature, feature_idx);
            const int vac_rehab = valueAt(input.parcel_vac_rehab_by_feature, feature_idx);
            const int vac_weight = overlayWeight(
                layerEnabledAt(*input.layers, input.vacant_notice_layer_idx),
                vac_notice,
                layerEnabledAt(*input.layers, input.vacant_rehab_layer_idx),
                vac_rehab);
            if (vacancy_fill_enabled && vac_weight > 0) {
                const int alpha = scaledOverlayAlpha(120, 18, 120, 230, vac_weight);
                const ImVec4 vac_base = blendVacancyColor(notice_c, rehab_c, vac_notice, vac_rehab);
                overlay = colorWithAlpha(vac_base, alpha);
            }
            const int lien_count = valueAt(input.parcel_tax_lien_by_feature, feature_idx);
            const int sale_count = valueAt(input.parcel_tax_sale_by_feature, feature_idx);
            const int tax_weight = overlayWeight(
                layerEnabledAt(*input.layers, input.tax_lien_layer_idx),
                lien_count,
                layerEnabledAt(*input.layers, input.tax_sale_layer_idx),
                sale_count);
            if (tax_fill_enabled && tax_weight > 0) {
                const int alpha = scaledOverlayAlpha(90, 10, 90, 210, tax_weight);
                const ImVec4 tax_base = blendTaxColor(
                    lien_c,
                    sale_c,
                    layerEnabledAt(*input.layers, input.tax_lien_layer_idx),
                    layerEnabledAt(*input.layers, input.tax_sale_layer_idx),
                    lien_count,
                    sale_count);
                overlay = colorWithAlpha(tax_base, alpha);
            }
            const std::string parcel_entity_id =
                parcelEntityIdForRenderFeature(state.render_blob.features[i]);
            if (input.selected_parcel_id_set->find(parcel_entity_id) != input.selected_parcel_id_set->end()) {
                overlay = selected_overlay;
            } else if (has_direct_query_style) {
                overlay = query_fill;
            } else {
                overlay = resolveParcelRuntimeOverlayColor(render_state, overlay);
            }
            overlay_colors[i] = overlay;
        }
        std::string overlay_error;
        if (updateParcelGpuOverlayColorBuffer(overlay_colors, &overlay_error)) {
            state.overlay_state_key = overlay_state_key;
            state.last_overlay_colors = overlay_colors;
        }
    }

    if (outline_state_key != state.outline_state_key) {
        std::vector<ImU32> outline_colors(state.render_blob.features.size(), IM_COL32(0, 0, 0, 0));
        const ImU32 selected_outline = IM_COL32(255, 240, 64, 255);
        for (size_t i = 0; i < state.render_blob.features.size(); ++i) {
            const uint32_t feature_idx = state.render_blob.features[i].feature_idx;
            const FeatureRenderState* render_state =
                findFeatureRenderState(cached_feature_render, static_cast<size_t>(input.parcel_layer_idx), feature_idx);
            if (render_state && !render_state->visible) continue;
            ImU32 query_fill = IM_COL32(0, 0, 0, 0);
            ImU32 query_outline = IM_COL32(0, 0, 0, 0);
            const bool has_direct_query_style = query_style_for_feature(feature_idx, query_fill, query_outline);
            const std::string parcel_entity_id =
                parcelEntityIdForRenderFeature(state.render_blob.features[i]);
            if (input.selected_parcel_id_set->find(parcel_entity_id) != input.selected_parcel_id_set->end()) {
                outline_colors[i] = selected_outline;
                continue;
            }
            if (render_state && render_state->has_query_outline_color) {
                outline_colors[i] = render_state->query_outline_color;
                continue;
            }
            if (has_direct_query_style) {
                outline_colors[i] = query_outline;
                continue;
            }
            ImU32 outline =
                i < state.last_base_colors.size()
                    ? state.last_base_colors[i]
                    : ImGui::ColorConvertFloat4ToU32(parcel_layer.color);
            const ImU32 overlay_fill =
                i < state.last_overlay_colors.size()
                    ? state.last_overlay_colors[i]
                    : IM_COL32(0, 0, 0, 0);
            if ((overlay_fill >> 24) != 0) {
                const ImVec4 overlay_v = ImGui::ColorConvertU32ToFloat4(overlay_fill);
                outline = colorWithAlpha(darkenColor(overlay_v, 0.60f), 235);
            } else {
                ImVec4 base_v = ImGui::ColorConvertU32ToFloat4(outline);
                outline = colorWithAlpha(darkenColor(base_v, 0.72f), 220);
            }
            outline_colors[i] = outline;
        }
        std::string outline_error;
        if (updateParcelGpuOutlineColorBuffer(outline_colors, &outline_error)) {
            state.outline_state_key = outline_state_key;
        }
    }
}
