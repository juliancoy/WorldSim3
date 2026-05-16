#include "layer_runtime.h"

#include <algorithm>
#include <cmath>

namespace {
std::string artifactReadStatus(const char* prefix, const std::string& layer_file, const char* suffix = "") {
    if (layer_file.empty()) return prefix;
    return std::string("reading ") + layer_file + suffix;
}
}

const char* statusToString(LayerPipelineStatus s) {
    switch (s) {
        case LayerPipelineStatus::Queued: return "queued";
        case LayerPipelineStatus::Hydrating: return "hydrating";
        case LayerPipelineStatus::Hydrated: return "hydrated";
        case LayerPipelineStatus::TriQueued: return "tri_queued";
        case LayerPipelineStatus::Triangulating: return "triangulating";
        case LayerPipelineStatus::Ready: return "ready";
        case LayerPipelineStatus::Failed: return "failed";
    }
    return "unknown";
}

std::string layerRuntimeDisplayStatus(const LayerRuntimeState& state, const std::string& layer_file) {
    if (state.status == LayerPipelineStatus::Hydrating) {
        if (state.hydration_phase == "loading_binary_cache" ||
            state.hydration_phase == "binary_cache_hit_queueing" ||
            state.hydration_phase == "cache_hit") {
            return artifactReadStatus("reading hydration cache", layer_file, ".bin");
        }
        if (state.hydration_phase == "loading_canonical_binary_source" ||
            state.hydration_phase == "canonical_binary_queueing") {
            return artifactReadStatus("reading canonical parcel binary", layer_file, ".canonical.bin");
        }
        if (state.hydration_phase == "parsing_source_cache_disabled" ||
            state.hydration_phase == "parsing_source_cache_missing" ||
            state.hydration_phase == "parsing_source_cache_miss_or_stale" ||
            state.hydration_phase == "source_parse") {
            return artifactReadStatus("reading source GeoJSON", layer_file);
        }
        if (state.hydration_phase == "parsing_source_cache_rejected") {
            return "rebuilding from source GeoJSON";
        }
        if (state.hydration_phase == "loading_canonical_binary_source_failed") {
            return "canonical binary read failed";
        }
    }
    if (state.status == LayerPipelineStatus::Triangulating) {
        if (state.triangulation_phase == "loading_binary_cache" ||
            state.triangulation_phase == "binary_cache_hit") {
            return artifactReadStatus("reading triangulation cache", layer_file, ".tri.bin");
        }
        if (state.triangulation_phase == "building_source_triangles" ||
            state.triangulation_phase == "building_cache_missing") {
            return "building triangulation cache";
        }
    }
    if (state.status == LayerPipelineStatus::TriQueued) return "queued for triangulation";
    return statusToString(state.status);
}

void buildLayerSpatialIndex(const LayerDef& layer, LayerSpatialIndex& si) {
    std::vector<LayerDef::FeatureExtent> feature_extents;
    feature_extents.reserve(layer.features.size());
    for (const auto& fg : layer.features) feature_extents.push_back(fg.extent);
    buildLayerSpatialIndexForExtents(feature_extents, si);
}

void buildLayerSpatialIndexForExtents(
    const std::vector<LayerDef::FeatureExtent>& feature_extents,
    LayerSpatialIndex& si) {
    si = LayerSpatialIndex{};
    const size_t n = feature_extents.size();
    if (n == 0) return;
    float min_lon = feature_extents[0].min_lon;
    float min_lat = feature_extents[0].min_lat;
    float max_lon = feature_extents[0].max_lon;
    float max_lat = feature_extents[0].max_lat;
    for (const auto& ex : feature_extents) {
        min_lon = std::min(min_lon, ex.min_lon);
        min_lat = std::min(min_lat, ex.min_lat);
        max_lon = std::max(max_lon, ex.max_lon);
        max_lat = std::max(max_lat, ex.max_lat);
    }
    si.min_lon = min_lon;
    si.min_lat = min_lat;
    si.max_lon = max_lon;
    si.max_lat = max_lat;
    const double span_lon = std::max(1e-6, (double)(max_lon - min_lon));
    const double span_lat = std::max(1e-6, (double)(max_lat - min_lat));
    const double approx = std::sqrt((double)n / 14.0);
    si.nx = std::clamp((int)approx, 24, 140);
    si.ny = std::clamp((int)approx, 24, 140);
    si.cells.resize((size_t)si.nx * (size_t)si.ny);
    si.marks.assign(n, 0u);

    auto cell_x = [&](float lon) {
        int x = (int)(((double)(lon - min_lon) / span_lon) * si.nx);
        return std::clamp(x, 0, si.nx - 1);
    };
    auto cell_y = [&](float lat) {
        int y = (int)(((double)(lat - min_lat) / span_lat) * si.ny);
        return std::clamp(y, 0, si.ny - 1);
    };

    for (size_t i = 0; i < n; ++i) {
        const auto& ex = feature_extents[i];
        const int x0 = cell_x(ex.min_lon);
        const int x1 = cell_x(ex.max_lon);
        const int y0 = cell_y(ex.min_lat);
        const int y1 = cell_y(ex.max_lat);
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                si.cells[(size_t)y * (size_t)si.nx + (size_t)x].push_back((uint32_t)i);
            }
        }
    }
    si.feature_count_built = n;
    si.built = true;
}

bool queryLayerSpatialIndex(
    LayerSpatialIndex& si,
    float q_min_lon,
    float q_min_lat,
    float q_max_lon,
    float q_max_lat,
    std::vector<uint32_t>& out) {
    if (!si.built || si.nx <= 0 || si.ny <= 0) return false;
    if (q_max_lon < si.min_lon || q_min_lon > si.max_lon || q_max_lat < si.min_lat || q_min_lat > si.max_lat) {
        out.clear();
        return true;
    }
    const double span_lon = std::max(1e-6, (double)(si.max_lon - si.min_lon));
    const double span_lat = std::max(1e-6, (double)(si.max_lat - si.min_lat));
    auto cell_x = [&](float lon) {
        int x = (int)(((double)(lon - si.min_lon) / span_lon) * si.nx);
        return std::clamp(x, 0, si.nx - 1);
    };
    auto cell_y = [&](float lat) {
        int y = (int)(((double)(lat - si.min_lat) / span_lat) * si.ny);
        return std::clamp(y, 0, si.ny - 1);
    };

    int x0 = cell_x(q_min_lon), x1 = cell_x(q_max_lon);
    int y0 = cell_y(q_min_lat), y1 = cell_y(q_max_lat);
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    if (++si.mark_id == 0) {
        std::fill(si.marks.begin(), si.marks.end(), 0u);
        si.mark_id = 1;
    }
    out.clear();
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const auto& cell = si.cells[(size_t)y * (size_t)si.nx + (size_t)x];
            for (uint32_t idx : cell) {
                if (idx >= si.marks.size()) continue;
                if (si.marks[idx] == si.mark_id) continue;
                si.marks[idx] = si.mark_id;
                out.push_back(idx);
            }
        }
    }
    return true;
}
