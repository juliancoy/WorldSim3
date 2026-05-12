#pragma once

#include "heatmap_render.h"

#include <string>
#include <vector>

// Attempts GPU aggregate generation for smooth heatmap algorithms.
// Returns true on success and fills out_density/out_cr/out_cg/out_cb/out_cw/out_gv/out_sv.
// Returns false when GPU path is unavailable or fails.
bool buildGpuSplatAggregate(
    const std::vector<HeatSample>& group,
    int rw,
    int rh,
    float raster_min_lon,
    float raster_min_lat,
    float raster_max_lon,
    float raster_max_lat,
    float sigma_r,
    std::vector<float>& out_density,
    std::vector<float>& out_cr,
    std::vector<float>& out_cg,
    std::vector<float>& out_cb,
    std::vector<float>& out_cw,
    std::vector<float>& out_gv,
    std::vector<float>& out_sv,
    std::string* error);

void shutdownGpuSplatAggregate();
