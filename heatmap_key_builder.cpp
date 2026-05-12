#include "heatmap_key_builder.h"

#include "aggregate_visualization_strategies.h"
#include "cache_io.h"
#include "map_render_utils.h"
#include "worldsim_app_internal.h"

#include <algorithm>

uint64_t buildHeatmapKey(const HeatmapKeyBuilderContext& ctx, bool include_view_state) {
    uint64_t key = 1469598103934665603ULL;
    hashCombineU64(key, include_view_state ? 1ULL : 0ULL);
    if (include_view_state) {
        hashCombineU64(key, (uint64_t)ctx.zoom);
        hashCombineU64(key, (uint64_t)ctx.math_zoom);
    }
    hashCombineFloat(key, ctx.global_heat_cell_px);
    hashCombineU64(key, (uint64_t)ctx.heatmap_algo);
    hashCombineU64(key, (uint64_t)ctx.effective_heatmap_quality_preset);
    hashCombineFloat(key, ctx.heatmap_bandwidth_px);
    hashCombineFloat(key, ctx.heatmap_blur_sigma_px);
    hashCombineFloat(key, ctx.heatmap_percentile_clip);
    hashCombineU64(key, ctx.heatmap_zoom_adaptive_bandwidth ? 1ULL : 0ULL);
    hashCombineU64(key, ctx.heatmap_multires_enabled ? 1ULL : 0ULL);
    hashCombineFloat(key, ctx.heatmap_multires_blend);
    hashCombineU64(key, ctx.heatmap_allow_cpu_fallback ? 1ULL : 0ULL);
    hashCombineU64(key, ctx.filter_enabled ? 1ULL : 0ULL);
    for (const char* p = ctx.filter_blocklot; *p; ++p) hashCombineU64(key, (uint64_t)(unsigned char)(*p));
    for (const char* p = ctx.filter_status; *p; ++p) hashCombineU64(key, (uint64_t)(unsigned char)(*p));
    for (const char* p = ctx.filter_address; *p; ++p) hashCombineU64(key, (uint64_t)(unsigned char)(*p));
    for (const char* p = ctx.filter_owner; *p; ++p) hashCombineU64(key, (uint64_t)(unsigned char)(*p));
    if (ctx.selected_owners && !ctx.selected_owners->empty()) {
        std::vector<std::string> selected_owner_list(ctx.selected_owners->begin(), ctx.selected_owners->end());
        std::sort(selected_owner_list.begin(), selected_owner_list.end());
        for (const auto& owner : selected_owner_list) {
            hashCombineU64(key, (uint64_t)owner.size());
            for (unsigned char ch : owner) hashCombineU64(key, (uint64_t)ch);
        }
    }
    for (const char* p = ctx.filter_zip; *p; ++p) hashCombineU64(key, (uint64_t)(unsigned char)(*p));
    hashCombineU64(key, ctx.filter_use_date ? 1ULL : 0ULL);
    hashCombineU64(key, (uint64_t)ctx.filter_year_min);
    hashCombineU64(key, (uint64_t)ctx.filter_year_max);
    hashCombineU64(key, ctx.crime_filter_enabled ? 1ULL : 0ULL);
    hashCombineU64(key, ctx.crime_filter_use_year ? 1ULL : 0ULL);
    hashCombineU64(key, (uint64_t)ctx.crime_year_min);
    hashCombineU64(key, (uint64_t)ctx.crime_year_max);
    hashCombineU64(key, ctx.crime_filter_homicide ? 1ULL : 0ULL);
    hashCombineU64(key, ctx.crime_filter_robbery ? 1ULL : 0ULL);
    hashCombineU64(key, ctx.crime_filter_assault ? 1ULL : 0ULL);
    hashCombineU64(key, ctx.crime_filter_burglary ? 1ULL : 0ULL);
    hashCombineU64(key, ctx.crime_filter_theft ? 1ULL : 0ULL);
    hashCombineU64(key, ctx.crime_filter_auto_theft ? 1ULL : 0ULL);
    hashCombineU64(key, ctx.crime_filter_drug ? 1ULL : 0ULL);
    hashCombineU64(key, ctx.crime_filter_shooting ? 1ULL : 0ULL);
    hashCombineU64(key, (uint64_t)(ctx.query_layers ? ctx.query_layers->size() : 0));
    if (ctx.query_layers) {
        for (const QueryMapLayer& query_layer : *ctx.query_layers) {
            hashCombineU64(key, query_layer.enabled ? 1ULL : 0ULL);
            for (float channel : query_layer.color) hashCombineFloat(key, channel);
            hashCombineU64(key, (uint64_t)query_layer.result_set.features.size());
            hashCombineU64(key, (uint64_t)query_layer.result_set.blocklots.size());
            hashCombineU64(key, (uint64_t)query_layer.result_set.owners.size());
            for (unsigned char ch : query_layer.name) hashCombineU64(key, (uint64_t)ch);
        }
    }
    if (ctx.layers && ctx.layer_uses_heatmap_aggregate) {
        for (size_t i = 0; i < ctx.layers->size(); ++i) {
            const bool layer_contributes_heatmap = (*ctx.layer_uses_heatmap_aggregate)(i);
            hashCombineU64(key, layer_contributes_heatmap ? 1ULL : 0ULL);
            if (!layer_contributes_heatmap) continue;
            hashCombineU64(key, (uint64_t)((ctx.layer_heatmap_max_zoom && i < ctx.layer_heatmap_max_zoom->size()) ? (*ctx.layer_heatmap_max_zoom)[i] : 13));
            hashCombineU64(key, (uint64_t)((ctx.layer_parcel_detail_min_zoom && i < ctx.layer_parcel_detail_min_zoom->size()) ? (*ctx.layer_parcel_detail_min_zoom)[i] : kParcelChoroplethMinZoom));
            hashCombineU64(key, (ctx.layer_heatmap_use_gradient && i < ctx.layer_heatmap_use_gradient->size() && (*ctx.layer_heatmap_use_gradient)[i]) ? 1ULL : 0ULL);
            hashCombineU64(key, (uint64_t)((ctx.layer_heatmap_algo && i < ctx.layer_heatmap_algo->size()) ? ((*ctx.layer_heatmap_algo)[i] + 1) : 0));
            hashCombineU64(key, (uint64_t)((ctx.layer_normalize_mode && i < ctx.layer_normalize_mode->size()) ? (*ctx.layer_normalize_mode)[i] : 0));
            hashCombineFloat(key, ctx.layer_heatmap_cell_px && i < ctx.layer_heatmap_cell_px->size() ? (*ctx.layer_heatmap_cell_px)[i] : ctx.global_heat_cell_px);
            hashCombineFloat(key, ctx.layer_heatmap_bandwidth_px && i < ctx.layer_heatmap_bandwidth_px->size() ? (*ctx.layer_heatmap_bandwidth_px)[i] : ctx.heatmap_bandwidth_px);
            hashCombineFloat(key, ctx.layer_heatmap_blur_sigma_px && i < ctx.layer_heatmap_blur_sigma_px->size() ? (*ctx.layer_heatmap_blur_sigma_px)[i] : ctx.heatmap_blur_sigma_px);
            hashCombineFloat(key, ctx.layer_heatmap_percentile_clip && i < ctx.layer_heatmap_percentile_clip->size() ? (*ctx.layer_heatmap_percentile_clip)[i] : ctx.heatmap_percentile_clip);
            hashCombineU64(key, (ctx.layer_heatmap_zoom_adaptive_bandwidth && i < ctx.layer_heatmap_zoom_adaptive_bandwidth->size() && (*ctx.layer_heatmap_zoom_adaptive_bandwidth)[i]) ? 1ULL : 0ULL);
            hashCombineU64(key, (ctx.layer_heatmap_multires_enabled && i < ctx.layer_heatmap_multires_enabled->size() && (*ctx.layer_heatmap_multires_enabled)[i]) ? 1ULL : 0ULL);
            hashCombineFloat(key, ctx.layer_heatmap_multires_blend && i < ctx.layer_heatmap_multires_blend->size() ? (*ctx.layer_heatmap_multires_blend)[i] : ctx.heatmap_multires_blend);
            hashCombineU64(key, (uint64_t)(*ctx.layers)[i].features.size());
            const std::string sig = fileSignature(ctx.root / "data" / "layers" / (*ctx.layers)[i].file);
            for (unsigned char ch : sig) hashCombineU64(key, (uint64_t)ch);
        }
    }
    return key;
}
