#include "worldsim_bootstrap.h"

#include <filesystem>

namespace fs = std::filesystem;

WorldsimLayerIndices findWorldsimLayerIndices(const std::vector<LayerDef>& layers) {
    WorldsimLayerIndices out;
    const bool regional_parcels_available = fs::exists(fs::current_path() / "data" / "layers" / "regional_parcels.geojson");
    const bool regional_real_property_available = fs::exists(fs::current_path() / "data" / "layers" / "regional_real_property.geojson");
    for (size_t i = 0; i < layers.size(); ++i) {
        if (layers[i].file == "regional_parcels.geojson" && regional_parcels_available) out.parcel_layer_idx = (int)i;
        else if (layers[i].file == "parcel.geojson" && out.parcel_layer_idx < 0) out.parcel_layer_idx = (int)i;
        else if (layers[i].file == "regional_real_property.geojson" && regional_real_property_available) out.real_property_layer_idx = (int)i;
        else if (layers[i].file == "real_property_information.geojson" && out.real_property_layer_idx < 0) out.real_property_layer_idx = (int)i;
        else if (layers[i].file == "vacant_building_notices.geojson") out.vacant_notice_layer_idx = (int)i;
        else if (layers[i].file == "vacant_building_rehabs.geojson") out.vacant_rehab_layer_idx = (int)i;
        else if (layers[i].file == "tax_lien_certificate_sale_properties.geojson") out.tax_lien_layer_idx = (int)i;
        else if (layers[i].file == "tax_sale_list_2021.geojson") out.tax_sale_layer_idx = (int)i;
        else if (layers[i].file == "zoning.geojson") out.zoning_layer_idx = (int)i;
        else if (layers[i].file == "crime_nibrs_group_a_2022_present.geojson") out.crime_nibrs_layer_idx = (int)i;
        else if (layers[i].file == "crime_part_1_legacy_srs.geojson") out.crime_legacy_layer_idx = (int)i;
    }
    return out;
}
