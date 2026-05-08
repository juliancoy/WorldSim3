#include "heatmap_strategy_ops.h"

#include <algorithm>
#include <cmath>

void applyGpuSplatBlurStrategy(
    const std::vector<HeatSample>& heat_samples,
    std::unordered_map<uint64_t, HeatBin>& global_heat_bins,
    float origin_x,
    float origin_y,
    float viewport_w,
    float viewport_h,
    float global_heat_cell,
    float blur_sigma_px) {
    if (heat_samples.empty()) return;

    const int grid_w = std::max(1, (int)std::ceil(viewport_w / global_heat_cell));
    const int grid_h = std::max(1, (int)std::ceil(viewport_h / global_heat_cell));
    const size_t n = (size_t)grid_w * (size_t)grid_h;
    std::vector<float> dens(n, 0.0f);
    std::vector<float> rsum(n, 0.0f);
    std::vector<float> gsum(n, 0.0f);
    std::vector<float> bsum(n, 0.0f);
    std::vector<float> wsum(n, 0.0f);
    std::vector<float> grad_votes(n, 0.0f);
    std::vector<float> solid_votes(n, 0.0f);
    auto idx2 = [&](int x, int y) -> size_t { return (size_t)y * (size_t)grid_w + (size_t)x; };

    for (const auto& s : heat_samples) {
        const int bx = (int)((s.x - origin_x) / global_heat_cell);
        const int by = (int)((s.y - origin_y) / global_heat_cell);
        if (bx < 0 || by < 0 || bx >= grid_w || by >= grid_h) continue;
        const size_t ii = idx2(bx, by);
        dens[ii] += 1.0f;
        rsum[ii] += s.color.x;
        gsum[ii] += s.color.y;
        bsum[ii] += s.color.z;
        wsum[ii] += 1.0f;
        if (s.prefer_gradient) grad_votes[ii] += 1.0f;
        else solid_votes[ii] += 1.0f;
    }

    auto blur_1d = [&](std::vector<float>& src, int w, int h, float sigma, bool horizontal) {
        if (sigma <= 0.05f) return;
        const int radius = std::max(1, (int)std::ceil(3.0f * sigma));
        std::vector<float> kernel((size_t)(radius * 2 + 1), 0.0f);
        float ksum = 0.0f;
        for (int i = -radius; i <= radius; ++i) {
            const float v = std::exp(-(float)(i * i) / (2.0f * sigma * sigma));
            kernel[(size_t)(i + radius)] = v;
            ksum += v;
        }
        if (ksum > 0.0f) for (float& v : kernel) v /= ksum;
        std::vector<float> out(src.size(), 0.0f);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                float acc = 0.0f;
                for (int k = -radius; k <= radius; ++k) {
                    const int sx = horizontal ? std::clamp(x + k, 0, w - 1) : x;
                    const int sy = horizontal ? y : std::clamp(y + k, 0, h - 1);
                    acc += src[idx2(sx, sy)] * kernel[(size_t)(k + radius)];
                }
                out[idx2(x, y)] = acc;
            }
        }
        src.swap(out);
    };

    const float sigma_cells = std::max(0.0f, blur_sigma_px / global_heat_cell);
    blur_1d(dens, grid_w, grid_h, sigma_cells, true);
    blur_1d(dens, grid_w, grid_h, sigma_cells, false);
    blur_1d(rsum, grid_w, grid_h, sigma_cells, true);
    blur_1d(rsum, grid_w, grid_h, sigma_cells, false);
    blur_1d(gsum, grid_w, grid_h, sigma_cells, true);
    blur_1d(gsum, grid_w, grid_h, sigma_cells, false);
    blur_1d(bsum, grid_w, grid_h, sigma_cells, true);
    blur_1d(bsum, grid_w, grid_h, sigma_cells, false);
    blur_1d(wsum, grid_w, grid_h, sigma_cells, true);
    blur_1d(wsum, grid_w, grid_h, sigma_cells, false);
    blur_1d(grad_votes, grid_w, grid_h, sigma_cells, true);
    blur_1d(grad_votes, grid_w, grid_h, sigma_cells, false);
    blur_1d(solid_votes, grid_w, grid_h, sigma_cells, true);
    blur_1d(solid_votes, grid_w, grid_h, sigma_cells, false);

    for (int y = 0; y < grid_h; ++y) {
        for (int x = 0; x < grid_w; ++x) {
            const size_t ii = idx2(x, y);
            const double d = dens[ii];
            if (d <= 1e-6) continue;
            HeatBin b;
            b.density = d;
            b.color_w_sum = std::max(1e-6, (double)wsum[ii]);
            b.r_sum = rsum[ii];
            b.g_sum = gsum[ii];
            b.b_sum = bsum[ii];
            const double invw = 1.0 / b.color_w_sum;
            const double rm = b.r_sum * invw;
            const double gm = b.g_sum * invw;
            const double bm = b.b_sum * invw;
            b.rr_sum = rm * rm * b.color_w_sum;
            b.gg_sum = gm * gm * b.color_w_sum;
            b.bb_sum = bm * bm * b.color_w_sum;
            b.gradient_votes = (int)std::round(grad_votes[ii]);
            b.solid_votes = (int)std::round(solid_votes[ii]);
            global_heat_bins[heatPackBin(x, y)] = b;
        }
    }
}
