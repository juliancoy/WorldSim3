#include "app_ui_state_bootstrap.h"

#include "app_utils.h"
#include "event_sectors.h"
#include "layer_state_io.h"
#include "worldsim_app_internal.h"

#include <algorithm>

namespace {
bool isZoningPolygonLayer(const LayerDef& layer) {
    if (layerUsesPointGeometry(layer)) return false;
    if (layer.category == LayerDef::Category::Zoning) return true;
    const std::string file_lower = toLowerAscii(layer.file);
    const std::string name_lower = toLowerAscii(layer.name);
    return file_lower.find("zoning") != std::string::npos ||
           name_lower.find("zoning") != std::string::npos;
}
}  // namespace

void loadPersistedAppUiState(AppUiStateBootstrapInput& input) {
    if (!input.root || !input.layers || !input.layer_browse_state || !input.map_filter_state ||
        !input.query_history || !input.center_lon || !input.center_lat || !input.zoom ||
        !input.zoning_layer_idx || !input.parcel_selection) {
        return;
    }

    loadLayerBrowseUiState(
        *input.root,
        &input.layer_browse_state->selected_nation_state,
        &input.layer_browse_state->selected_state_region);

    loadFilterUiState(
        *input.root,
        &input.map_filter_state->enabled,
        &input.map_filter_state->use_date,
        &input.map_filter_state->year_min,
        &input.map_filter_state->year_max,
        input.map_filter_state->blocklot,
        sizeof(input.map_filter_state->blocklot),
        input.map_filter_state->status,
        sizeof(input.map_filter_state->status),
        input.map_filter_state->address,
        sizeof(input.map_filter_state->address),
        input.map_filter_state->owner,
        sizeof(input.map_filter_state->owner),
        input.map_filter_state->zip,
        sizeof(input.map_filter_state->zip),
        &input.map_filter_state->crime.enabled,
        &input.map_filter_state->crime.homicide,
        &input.map_filter_state->crime.robbery,
        &input.map_filter_state->crime.assault,
        &input.map_filter_state->crime.burglary,
        &input.map_filter_state->crime.theft,
        &input.map_filter_state->crime.auto_theft,
        &input.map_filter_state->crime.drug,
        &input.map_filter_state->crime.shooting,
        &input.map_filter_state->crime.use_year,
        &input.map_filter_state->crime.year_min,
        &input.map_filter_state->crime.year_max,
        input.owner_search_query,
        input.owner_search_query_size,
        &input.map_filter_state->selected_owners,
        &input.map_filter_state->event_sector_enabled);

    loadQueryHistoryUiState(*input.root, input.query_history);
    ensureCommunitySectorFilterDefaults(input.map_filter_state->event_sector_enabled);

    loadMapUiState(
        *input.root,
        input.center_lon,
        input.center_lat,
        input.zoom,
        &input.parcel_selection->active_entity_id,
        &input.parcel_selection->entity_ids);

    *input.zoning_layer_idx = resolveActiveZoningLayerIndex(
        *input.layers,
        *input.zoning_layer_idx,
        input.fallback_zoning_layer_idx);

    *input.zoom = std::clamp(*input.zoom, (double)kMinZoom, (double)kMaxZoom);
    *input.center_lat = std::clamp(*input.center_lat, -85.0, 85.0);

    input.parcel_selection->entity_id_set.clear();
    for (const std::string& entity_id : input.parcel_selection->entity_ids) {
        input.parcel_selection->entity_id_set.insert(entity_id);
    }
    input.parcel_selection->show_details =
        !input.parcel_selection->entity_ids.empty() && !input.parcel_selection->active_entity_id.empty();
}

int resolveActiveZoningLayerIndex(
    const std::vector<LayerDef>& layers,
    int zoning_layer_idx,
    int fallback_zoning_layer_idx) {
    if (zoning_layer_idx >= 0 && (size_t)zoning_layer_idx < layers.size() &&
        layers[(size_t)zoning_layer_idx].enabled && isZoningPolygonLayer(layers[(size_t)zoning_layer_idx])) {
        return zoning_layer_idx;
    }
    for (size_t i = 0; i < layers.size(); ++i) {
        if (layers[i].enabled && isZoningPolygonLayer(layers[i])) return (int)i;
    }
    return fallback_zoning_layer_idx;
}
