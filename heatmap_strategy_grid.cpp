#include "heatmap_strategy_ops.h"

void applyGridStrategy(
    const std::vector<HeatSample>& heat_samples,
    std::unordered_map<uint64_t, HeatBin>& global_heat_bins,
    float origin_x,
    float origin_y,
    float global_heat_cell) {
    for (const auto& s : heat_samples) {
        const int bx = (int)((s.x - origin_x) / global_heat_cell);
        const int by = (int)((s.y - origin_y) / global_heat_cell);
        HeatBin& b = global_heat_bins[heatPackBin(bx, by)];
        heatAccumBin(b, s, 1.0);
    }
}
