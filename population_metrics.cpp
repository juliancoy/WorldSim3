#include "population_metrics.h"

#include "feature_props.h"
#include "layer_geometry.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <string>

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kKmPerDegreeLat = 110.574;
constexpr double kKmPerDegreeLonAtEquator = 111.320;

double ringMeanLatitude(const std::vector<ImVec2>& ring) {
    if (ring.empty()) return 0.0;
    double sum = 0.0;
    for (const ImVec2& p : ring) sum += p.y;
    return sum / static_cast<double>(ring.size());
}

double ringSignedAreaSqKm(const std::vector<ImVec2>& ring) {
    if (ring.size() < 3) return 0.0;
    const double mean_lat_rad = ringMeanLatitude(ring) * kPi / 180.0;
    const double km_per_lon = kKmPerDegreeLonAtEquator * std::cos(mean_lat_rad);
    double area2 = 0.0;
    for (size_t i = 0; i < ring.size(); ++i) {
        const ImVec2& a = ring[i];
        const ImVec2& b = ring[(i + 1) % ring.size()];
        const double ax = a.x * km_per_lon;
        const double ay = a.y * kKmPerDegreeLat;
        const double bx = b.x * km_per_lon;
        const double by = b.y * kKmPerDegreeLat;
        area2 += ax * by - bx * ay;
    }
    return area2 * 0.5;
}

bool parsePositiveDouble(std::string value, double* out) {
    value.erase(std::remove(value.begin(), value.end(), ','), value.end());
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c) {
        return std::isspace(c);
    }), value.end());
    if (value.empty()) return false;
    char* end = nullptr;
    const double parsed = std::strtod(value.c_str(), &end);
    if (end == value.c_str() || !std::isfinite(parsed) || parsed < 0.0) return false;
    if (out) *out = parsed;
    return true;
}

void upsertProperty(
    LayerDef::FeatureProperties& properties,
    const std::string& key,
    const std::string& value) {
    for (auto& kv : properties.values) {
        if (kv.first == key) {
            kv.second = value;
            return;
        }
    }
    properties.values.push_back({key, value});
}

bool featureExtentContains(const LayerDef::FeatureRecord& feature, float lon, float lat) {
    return lon >= feature.extent.min_lon &&
           lon <= feature.extent.max_lon &&
           lat >= feature.extent.min_lat &&
           lat <= feature.extent.max_lat;
}
}

double polygonFeatureAreaSqKm(const LayerDef::FeatureRecord& feature) {
    if (feature.rings.empty()) return 0.0;
    double area = 0.0;
    for (size_t i = 0; i < feature.rings.size(); ++i) {
        const double ring_area = std::fabs(ringSignedAreaSqKm(feature.rings[i]));
        area += (i == 0) ? ring_area : -ring_area;
    }
    return std::max(0.0, area);
}

bool tryParsePopulation(const LayerDef& layer, size_t feature_idx, double* out_population) {
    static constexpr std::initializer_list<const char*> kPopulationKeys = {
        "total_population",
        "TotalPopulation",
        "TOTAL_POPULATION",
        "population",
        "POPULATION",
        "TotalPop",
        "total_pop",
        "B01003_001E",
        "B02001_001E",
        "B03003_001E"
    };
    const std::string value = getFirstPropertyValue(layer, feature_idx, kPopulationKeys);
    return parsePositiveDouble(value, out_population);
}

std::vector<PopulationTractMetric> buildTractPopulationCrimeMetrics(
    const LayerDef& population_tract_layer,
    const LayerDef& crime_point_layer,
    PopulationMetricsSummary* summary) {
    std::vector<PopulationTractMetric> metrics;
    metrics.reserve(population_tract_layer.features.size());

    PopulationMetricsSummary local_summary;
    local_summary.tract_count = population_tract_layer.features.size();
    local_summary.crime_point_count = crime_point_layer.features.size();

    for (size_t i = 0; i < population_tract_layer.features.size(); ++i) {
        PopulationTractMetric metric;
        metric.tract_feature_idx = i;
        metric.area_sq_km = polygonFeatureAreaSqKm(population_tract_layer.features[i]);
        metric.area_valid = metric.area_sq_km > 0.0;

        double population = 0.0;
        if (tryParsePopulation(population_tract_layer, i, &population)) {
            metric.total_population = population;
            metric.population_valid = true;
            local_summary.valid_population_tracts += 1;
            local_summary.total_population += population;
            if (metric.area_valid) {
                metric.population_density_per_sq_km = population / metric.area_sq_km;
            }
        }
        metrics.push_back(metric);
    }

    for (const LayerDef::FeatureRecord& crime : crime_point_layer.features) {
        const float lon = crime.extent.min_lon;
        const float lat = crime.extent.min_lat;
        local_summary.total_crimes += 1;
        for (size_t i = 0; i < population_tract_layer.features.size(); ++i) {
            const LayerDef::FeatureRecord& tract = population_tract_layer.features[i];
            if (!featureExtentContains(tract, lon, lat)) continue;
            if (!pointInFeature(tract, lon, lat)) continue;
            metrics[i].crime_count += 1;
            local_summary.assigned_crime_points += 1;
            break;
        }
    }

    for (PopulationTractMetric& metric : metrics) {
        if (metric.population_valid && metric.total_population > 0.0) {
            metric.crime_per_1000_residents =
                static_cast<double>(metric.crime_count) * 1000.0 / metric.total_population;
        }
    }

    if (summary) *summary = local_summary;
    return metrics;
}

void applyPopulationMetricsToLayer(
    LayerDef& tract_layer,
    const std::vector<PopulationTractMetric>& metrics) {
    if (tract_layer.feature_properties.size() < tract_layer.features.size()) {
        tract_layer.feature_properties.resize(tract_layer.features.size());
    }
    for (const PopulationTractMetric& metric : metrics) {
        if (metric.tract_feature_idx >= tract_layer.feature_properties.size()) continue;
        LayerDef::FeatureProperties& properties = tract_layer.feature_properties[metric.tract_feature_idx];
        upsertProperty(properties, "population_algorithm", "tract_population_density_and_crime_rate");
        upsertProperty(properties, "total_population", std::to_string(metric.total_population));
        upsertProperty(properties, "area_sq_km", std::to_string(metric.area_sq_km));
        upsertProperty(properties, "population_density_per_sq_km", std::to_string(metric.population_density_per_sq_km));
        upsertProperty(properties, "crime_count", std::to_string(metric.crime_count));
        upsertProperty(properties, "crime_per_1000_residents", std::to_string(metric.crime_per_1000_residents));
    }
}
