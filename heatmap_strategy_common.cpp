#include "heatmap_strategy_ops.h"

uint64_t heatPackBin(int bx, int by) {
    return ((uint64_t)(uint32_t)bx << 32) | (uint32_t)by;
}

void heatUnpackBin(uint64_t key, int& bx, int& by) {
    bx = (int)(uint32_t)(key >> 32);
    by = (int)(uint32_t)(key & 0xffffffffu);
}

void heatAccumBin(HeatBin& b, const HeatSample& s, double w) {
    b.density += w;
    b.color_w_sum += w;
    b.r_sum += s.color.x * w;
    b.g_sum += s.color.y * w;
    b.b_sum += s.color.z * w;
    b.rr_sum += s.color.x * s.color.x * w;
    b.gg_sum += s.color.y * s.color.y * w;
    b.bb_sum += s.color.z * s.color.z * w;
    if (s.prefer_gradient) b.gradient_votes++;
    else b.solid_votes++;
    if (s.has_value) b.values.push_back(s.value);
}
