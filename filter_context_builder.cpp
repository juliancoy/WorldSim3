#include "filter_context_builder.h"

#include "app_utils.h"

FeatureFilterContext makeFeatureFilterContext(const FeatureFilterContextFactoryInput& input) {
    FeatureFilterContext ctx;
    ctx.layers = input.layers;
    ctx.map_filters = input.map_filters;
    if (input.map_filters) {
        ctx.selected_nation_state_normalized = normalizeGeographyToken(input.map_filters->selected_nation_state);
        ctx.selected_state_region_normalized = normalizeGeographyToken(input.map_filters->selected_state_region);
        ctx.selected_county_city_normalized = normalizeGeographyToken(input.map_filters->selected_county_city);
    }
    ctx.result_set = input.result_set;
    ctx.query_layers = input.query_layers;
    ctx.real_property_by_blocklot = input.real_property_by_blocklot;
    ctx.parcel_vac_notice_by_feature = input.parcel_vac_notice_by_feature;
    ctx.parcel_vac_rehab_by_feature = input.parcel_vac_rehab_by_feature;
    ctx.real_property_layer_idx = input.real_property_layer_idx;
    ctx.parcel_layer_idx = input.parcel_layer_idx;
    ctx.crime_nibrs_layer_idx = input.crime_nibrs_layer_idx;
    return ctx;
}
