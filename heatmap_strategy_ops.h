#pragma once

#include "heatmap_render.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

struct HeatBin {
    double density = 0.0;
    double color_w_sum = 0.0;
    double r_sum = 0.0;
    double g_sum = 0.0;
    double b_sum = 0.0;
    double rr_sum = 0.0;
    double gg_sum = 0.0;
    double bb_sum = 0.0;
    int gradient_votes = 0;
    int solid_votes = 0;
    std::vector<float> values;
};

uint64_t heatPackBin(int bx, int by);
void heatUnpackBin(uint64_t key, int& bx, int& by);
void heatAccumBin(HeatBin& b, const HeatSample& s, double w);

void applyGridStrategy(
    const std::vector<HeatSample>& heat_samples,
    std::unordered_map<uint64_t, HeatBin>& global_heat_bins,
    float origin_x,
    float origin_y,
    float global_heat_cell);

void applyKdeStrategy(
    const std::vector<HeatSample>& heat_samples,
    std::unordered_map<uint64_t, HeatBin>& global_heat_bins,
    float origin_x,
    float origin_y,
    float global_heat_cell,
    float sigma_px);

void applyGpuSplatBlurStrategy(
    const std::vector<HeatSample>& heat_samples,
    std::unordered_map<uint64_t, HeatBin>& global_heat_bins,
    float origin_x,
    float origin_y,
    float viewport_w,
    float viewport_h,
    float global_heat_cell,
    float blur_sigma_px);

void applyHexStrategy(
    const std::vector<HeatSample>& heat_samples,
    std::unordered_map<uint64_t, HeatBin>& global_heat_bins,
    float origin_x,
    float origin_y,
    float global_heat_cell);

void applyMultiResStrategy(
    const std::vector<HeatSample>& heat_samples,
    std::unordered_map<uint64_t, HeatBin>& global_heat_bins,
    std::unordered_map<uint64_t, HeatBin>& coarse_heat_bins,
    float origin_x,
    float origin_y,
    float global_heat_cell,
    bool heatmap_multires_enabled,
    float heatmap_multires_blend);
