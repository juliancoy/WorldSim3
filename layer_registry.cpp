#include "layer_registry.h"

#include "layer_import.h"

namespace fs = std::filesystem;

LayerRegistry::LayerRegistry(const fs::path& root, const std::vector<LayerDef>& layers) {
    refresh(root, layers);
}

void LayerRegistry::refresh(const fs::path& root, const std::vector<LayerDef>& layers) {
    layers_ = &layers;
    indices_ = {};
    const bool regional_parcels_available =
        fs::exists(root / "data" / "layers" / "regional_parcels.geojson") ||
        fs::exists(root / "data" / "layers" / "regional_parcels.geojson.canonical.bin");
    const bool regional_real_property_available = fs::exists(root / "data" / "layers" / "regional_real_property.geojson");
    for (size_t i = 0; i < layers.size(); ++i) {
        if (layers[i].file == "regional_parcels.geojson" && regional_parcels_available) indices_.parcel_layer_idx = (int)i;
        else if (layers[i].file == "parcel.geojson" && indices_.parcel_layer_idx < 0) indices_.parcel_layer_idx = (int)i;
        else if (layers[i].file == "regional_real_property.geojson" && regional_real_property_available) indices_.real_property_layer_idx = (int)i;
        else if (layers[i].file == "real_property_information.geojson" && indices_.real_property_layer_idx < 0) indices_.real_property_layer_idx = (int)i;
        else if (layers[i].file == "vacant_building_notices.geojson") indices_.vacant_notice_layer_idx = (int)i;
        else if (layers[i].file == "vacant_building_rehabs.geojson") indices_.vacant_rehab_layer_idx = (int)i;
        else if (layers[i].file == "tax_lien_certificate_sale_properties.geojson") indices_.tax_lien_layer_idx = (int)i;
        else if (layers[i].file == "tax_sale_list_2021.geojson") indices_.tax_sale_layer_idx = (int)i;
        else if (layers[i].file == "zoning.geojson") indices_.zoning_layer_idx = (int)i;
        else if (layers[i].file == "crime_nibrs_group_a_2022_present.geojson") indices_.crime_nibrs_layer_idx = (int)i;
    }
}

int LayerRegistry::findLayerByFile(std::string_view file) const {
    if (!layers_) return -1;
    for (size_t i = 0; i < layers_->size(); ++i) {
        if ((*layers_)[i].file == file) return (int)i;
    }
    return -1;
}

int LayerRegistry::findLayerByName(std::string_view name) const {
    if (!layers_) return -1;
    for (size_t i = 0; i < layers_->size(); ++i) {
        if ((*layers_)[i].name == name) return (int)i;
    }
    return -1;
}

int LayerRegistry::findLayerByFileOrName(std::string_view key) const {
    int idx = findLayerByFile(key);
    return idx >= 0 ? idx : findLayerByName(key);
}

bool LayerRegistry::hasLayer(size_t idx) const {
    return layers_ && idx < layers_->size();
}

bool LayerRegistry::canDownload(size_t idx) const {
    return hasLayer(idx) && (!(*layers_)[idx].source_url.empty() || layerHasImportSource((*layers_)[idx]));
}

bool LayerRegistry::hasSourceMetadata(size_t idx) const {
    return hasLayer(idx) && (canDownload(idx) || !(*layers_)[idx].reference_url.empty() || !(*layers_)[idx].source_urls.empty());
}

bool LayerRegistry::isParcelHeatmapLayer(size_t idx) const {
    return hasLayer(idx) && (*layers_)[idx].scale == "parcel" && !(*layers_)[idx].heatmap_field.empty();
}

bool LayerRegistry::isHiddenParcelGeometryLayer(size_t idx) const {
    return hasLayer(idx) &&
           indices_.parcel_layer_idx >= 0 &&
           (int)idx != indices_.parcel_layer_idx &&
           (*layers_)[idx].scale == "parcel" &&
           (*layers_)[idx].region.empty();
}
