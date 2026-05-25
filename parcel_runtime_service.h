#pragma once

#include "app_settings.h"
#include "cache_io.h"
#include "filters.h"
#include "layer_runtime.h"
#include "parcel_unified.h"

#include "imgui.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

struct ParcelRuntimeState {
    std::string uploaded_signature;
    std::string geometry_locked_signature;
    std::string geometry_restart_required_signature;
    std::string render_requested_signature;
    std::string gpu_upload_requested_signature;
    uint64_t filter_state_key = 0;
    uint64_t overlay_state_key = 0;
    uint64_t outline_state_key = 0;
    std::vector<ImU32> last_base_colors;
    std::vector<ImU32> last_overlay_colors;
    std::vector<ImU32> last_outline_colors;
    ParcelRenderCacheBlob render_blob;
};

struct ParcelRuntimeSyncInput {
    const std::filesystem::path* root = nullptr;
    const AppSettings* app_settings = nullptr;
    std::vector<LayerDef>* layers = nullptr;
    std::vector<LayerRuntimeState>* layer_states = nullptr;
    int parcel_layer_idx = -1;
    int property_value_layer_idx = -1;
    int vacant_notice_layer_idx = -1;
    int vacant_rehab_layer_idx = -1;
    int tax_lien_layer_idx = -1;
    int tax_sale_layer_idx = -1;
    int parcel_parameter_mode = 0;
    const std::vector<float>* layer_choropleth_gamma = nullptr;
    const std::vector<float>* layer_heatmap_percentile_clip = nullptr;
    const std::vector<int>* layer_normalize_mode = nullptr;
    const std::vector<bool>* layer_fill_enabled = nullptr;
    const MapFilterState* map_filter_state = nullptr;
    const FilterResultSet* parcel_jurisdiction_result_set = nullptr;
    const std::vector<QueryMapLayer>* query_layers = nullptr;
    const std::vector<UnifiedParcelRecord>* unified_parcels = nullptr;
    const std::vector<int>* parcel_vac_notice_by_feature = nullptr;
    const std::vector<int>* parcel_vac_rehab_by_feature = nullptr;
    const std::vector<int>* parcel_tax_lien_by_feature = nullptr;
    const std::vector<int>* parcel_tax_sale_by_feature = nullptr;
    const std::unordered_set<std::string>* selected_parcel_id_set = nullptr;
    bool* gpu_profiler_reload_requested = nullptr;
    uint64_t feature_render_state_key = 0;
    std::function<const LayerFeatureRenderCache&()> ensure_feature_render_cache;
    std::function<double(size_t)> parcel_area_sq_m;
};

void syncParcelGpuLayer(const ParcelRuntimeSyncInput& input, ParcelRuntimeState& state);
