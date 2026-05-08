#include "heatmap_strategy_ops.h"

#include <algorithm>

void applyMultiResStrategy(
    const std::vector<HeatSample>& heat_samples,
    std::unordered_map<uint64_t, HeatBin>& global_heat_bins,
    std::unordered_map<uint64_t, HeatBin>& coarse_heat_bins,
    float origin_x,
    float origin_y,
    float global_heat_cell,
    bool heatmap_multires_enabled,
    float heatmap_multires_blend) {
    const float fine = global_heat_cell;
    const float coarse = global_heat_cell * 2.0f;
    for (const auto& s : heat_samples) {
        const int bx = (int)((s.x - origin_x) / fine);
        const int by = (int)((s.y - origin_y) / fine);
        HeatBin& b = global_heat_bins[heatPackBin(bx, by)];
        heatAccumBin(b, s, 1.0);

        const int cbx = (int)((s.x - origin_x) / coarse);
        const int cby = (int)((s.y - origin_y) / coarse);
        HeatBin& cb = coarse_heat_bins[heatPackBin(cbx, cby)];
        heatAccumBin(cb, s, 1.0);
    }

    if (!heatmap_multires_enabled) return;
    const double blend = std::clamp((double)heatmap_multires_blend, 0.0, 1.0);
    for (auto& kv : global_heat_bins) {
        int bx = 0, by = 0;
        heatUnpackBin(kv.first, bx, by);
        const int cbx = bx / 2;
        const int cby = by / 2;
        auto it = coarse_heat_bins.find(heatPackBin(cbx, cby));
        if (it != coarse_heat_bins.end()) {
            kv.second.density = kv.second.density * (1.0 - blend) + it->second.density * blend;
        }
    }
}
