#include "heatmap_strategy_ops.h"

#include <algorithm>
#include <cmath>

void applyHexStrategy(
    const std::vector<HeatSample>& heat_samples,
    std::unordered_map<uint64_t, HeatBin>& global_heat_bins,
    float origin_x,
    float origin_y,
    float global_heat_cell) {
    const float hex_w = global_heat_cell;
    const float hex_h = std::max(1.0f, hex_w * 0.8660254f);
    for (const auto& s : heat_samples) {
        const float gx = (s.x - origin_x) / hex_w;
        const float gy = (s.y - origin_y) / hex_h;
        const int q = (int)std::round(gx - gy * 0.5f);
        const int r = (int)std::round(gy);
        HeatBin& b = global_heat_bins[heatPackBin(q, r)];
        heatAccumBin(b, s, 1.0);
    }
}
