#pragma once

#include "types.h"

#include <filesystem>
#include <string_view>
#include <vector>

struct WorldsimLayerIndices {
    int parcel_layer_idx = -1;
    int real_property_layer_idx = -1;
    int vacant_notice_layer_idx = -1;
    int vacant_rehab_layer_idx = -1;
    int tax_lien_layer_idx = -1;
    int tax_sale_layer_idx = -1;
    int zoning_layer_idx = -1;
    int crime_nibrs_layer_idx = -1;
};

class LayerRegistry {
public:
    LayerRegistry() = default;
    LayerRegistry(const std::filesystem::path& root, const std::vector<LayerDef>& layers);

    void refresh(const std::filesystem::path& root, const std::vector<LayerDef>& layers);

    const WorldsimLayerIndices& indices() const { return indices_; }

    int findLayerByFile(std::string_view file) const;
    int findLayerByName(std::string_view name) const;
    int findLayerByFileOrName(std::string_view key) const;

    bool hasLayer(size_t idx) const;
    bool canDownload(size_t idx) const;
    bool hasSourceMetadata(size_t idx) const;
    bool isHiddenParcelGeometryLayer(size_t idx) const;
    bool isParcelHeatmapLayer(size_t idx) const;

private:
    const std::vector<LayerDef>* layers_ = nullptr;
    WorldsimLayerIndices indices_;
};
