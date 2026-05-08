#include "heatmap_strategy_ops.h"

#include <algorithm>
#include <cmath>

void applyKdeStrategy(
    const std::vector<HeatSample>& heat_samples,
    std::unordered_map<uint64_t, HeatBin>& global_heat_bins,
    float origin_x,
    float origin_y,
    float global_heat_cell,
    float sigma_px) {
    const float sigma = std::max(1.0f, sigma_px);
    const int radius = std::max(1, (int)std::ceil((3.0f * sigma) / global_heat_cell));
    const float two_sigma2 = 2.0f * sigma * sigma;
    for (const auto& s : heat_samples) {
        const int cbx = (int)((s.x - origin_x) / global_heat_cell);
        const int cby = (int)((s.y - origin_y) / global_heat_cell);
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                const float cx = origin_x + (cbx + dx + 0.5f) * global_heat_cell;
                const float cy = origin_y + (cby + dy + 0.5f) * global_heat_cell;
                const float d2 = (cx - s.x) * (cx - s.x) + (cy - s.y) * (cy - s.y);
                const double w = std::exp(-(double)d2 / (double)two_sigma2);
                if (w < 1e-5) continue;
                HeatBin& b = global_heat_bins[heatPackBin(cbx + dx, cby + dy)];
                heatAccumBin(b, s, w);
            }
        }
    }
}
