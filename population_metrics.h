#pragma once

#include "types.h"

#include <cstddef>
#include <cstdint>
#include <vector>

struct PopulationTractMetric {
    size_t tract_feature_idx = 0;
    double total_population = 0.0;
    double area_sq_km = 0.0;
    double population_density_per_sq_km = 0.0;
    uint32_t crime_count = 0;
    double crime_per_1000_residents = 0.0;
    bool population_valid = false;
    bool area_valid = false;
};

struct PopulationMetricsSummary {
    size_t tract_count = 0;
    size_t valid_population_tracts = 0;
    size_t crime_point_count = 0;
    size_t assigned_crime_points = 0;
    double total_population = 0.0;
    uint32_t total_crimes = 0;
};

double polygonFeatureAreaSqKm(const LayerDef::FeatureRecord& feature);
bool tryParsePopulation(const LayerDef& layer, size_t feature_idx, double* out_population);

std::vector<PopulationTractMetric> buildTractPopulationCrimeMetrics(
    const LayerDef& population_tract_layer,
    const LayerDef& crime_point_layer,
    PopulationMetricsSummary* summary = nullptr);

void applyPopulationMetricsToLayer(
    LayerDef& tract_layer,
    const std::vector<PopulationTractMetric>& metrics);
