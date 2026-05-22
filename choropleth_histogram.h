#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

struct ApproxHistogram {
    bool valid = false;
    size_t sample_count = 0;
    size_t clipped_sample_count = 0;
    double min_value = 0.0;
    double max_value = 0.0;
    double clipped_max_value = 0.0;
    double median_value = 0.0;
    double bin_width = 0.0;
    float max_bin = 0.0f;
    std::vector<float> plot_bins;
    std::vector<uint32_t> bins;
    std::vector<uint32_t> cumulative_bins;

    bool rangeValid() const {
        return valid && std::isfinite(min_value) && std::isfinite(clipped_max_value) && clipped_max_value > min_value;
    }

    float normalizeLinear(double value) const {
        if (!rangeValid()) return 0.0f;
        return std::clamp((float)((value - min_value) / (clipped_max_value - min_value)), 0.0f, 1.0f);
    }

    float normalizeApproxPercentile(double value) const {
        if (!rangeValid() || bins.empty()) return 0.0f;
        const double clamped = std::clamp(value, min_value, clipped_max_value);
        if (bin_width <= std::numeric_limits<double>::epsilon()) return 0.0f;
        const double pos = (clamped - min_value) / bin_width;
        size_t bin_idx = (size_t)std::clamp((int)std::floor(pos), 0, (int)bins.size() - 1);
        const double frac = std::clamp(pos - (double)bin_idx, 0.0, 1.0);
        const double before = bin_idx == 0 ? 0.0 : (double)cumulative_bins[bin_idx - 1];
        const double within = frac * (double)bins[bin_idx];
        const double denom = (double)std::max<size_t>(1, clipped_sample_count);
        return std::clamp((float)((before + within) / denom), 0.0f, 1.0f);
    }

    float normalizeEqualCountZones(double value, int zone_count = 8) const {
        zone_count = std::max(2, zone_count);
        const float percentile = normalizeApproxPercentile(value);
        const int zone_idx = std::clamp((int)std::floor(percentile * (float)zone_count), 0, zone_count - 1);
        return zone_count <= 1 ? 0.0f : (float)zone_idx / (float)(zone_count - 1);
    }
};

inline ApproxHistogram buildApproxHistogram(
    const std::vector<double>& values,
    float clip_pct,
    int requested_bin_count = 32) {
    ApproxHistogram hist;
    if (values.empty()) return hist;

    std::vector<double> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    hist.valid = true;
    hist.sample_count = sorted.size();
    hist.min_value = sorted.front();
    hist.max_value = sorted.back();
    hist.median_value = sorted[sorted.size() / 2];
    clip_pct = std::clamp(clip_pct, 50.0f, 100.0f);
    const size_t max_idx = sorted.size() - 1;
    const size_t clip_idx = clip_pct < 100.0f
        ? (size_t)std::clamp((int)std::floor((clip_pct / 100.0f) * (double)max_idx), 0, (int)max_idx)
        : max_idx;
    hist.clipped_max_value = std::max(hist.min_value, sorted[clip_idx]);
    hist.clipped_sample_count =
        (size_t)std::distance(sorted.begin(), std::upper_bound(sorted.begin(), sorted.end(), hist.clipped_max_value));

    const int bin_count = std::max(8, requested_bin_count);
    hist.plot_bins.assign((size_t)bin_count, 0.0f);
    hist.bins.assign((size_t)bin_count, 0);
    hist.cumulative_bins.assign((size_t)bin_count, 0);

    if (hist.clipped_max_value <= hist.min_value) {
        hist.plot_bins[0] = (float)hist.sample_count;
        hist.bins[0] = (uint32_t)hist.sample_count;
        hist.cumulative_bins[0] = (uint32_t)hist.sample_count;
        hist.max_bin = hist.plot_bins[0];
        return hist;
    }

    hist.bin_width = (hist.clipped_max_value - hist.min_value) / (double)bin_count;
    if (hist.bin_width <= std::numeric_limits<double>::epsilon()) {
        hist.plot_bins[0] = (float)hist.sample_count;
        hist.bins[0] = (uint32_t)hist.sample_count;
        hist.cumulative_bins[0] = (uint32_t)hist.sample_count;
        hist.max_bin = hist.plot_bins[0];
        return hist;
    }

    for (double raw_value : sorted) {
        const double value = std::clamp(raw_value, hist.min_value, hist.clipped_max_value);
        int bin_idx = (int)std::floor((value - hist.min_value) / hist.bin_width);
        bin_idx = std::clamp(bin_idx, 0, bin_count - 1);
        hist.plot_bins[(size_t)bin_idx] += 1.0f;
        hist.bins[(size_t)bin_idx] += 1u;
        hist.max_bin = std::max(hist.max_bin, hist.plot_bins[(size_t)bin_idx]);
    }
    uint32_t running = 0;
    for (size_t i = 0; i < hist.bins.size(); ++i) {
        running += hist.bins[i];
        hist.cumulative_bins[i] = running;
    }
    return hist;
}
