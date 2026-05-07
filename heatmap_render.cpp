#include "heatmap_render.h"

#include "aggregate_visualization_strategies.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <limits>
#include <unordered_map>

ImVec4 defaultHeatColor(float t);

std::pair<uint64_t, HeatmapRenderData> buildHeatmapRenderData(
    uint64_t key,
    std::vector<HeatSample> samples,
    float origin_x,
    float origin_y,
    float view_min_lon,
    float view_min_lat,
    float view_max_lon,
    float view_max_lat,
    float viewport_w,
    float viewport_h,
    int zoom,
    int max_zoom,
    int raster_base_px,
    int raster_max_px) {
                struct Bin {
                    double d = 0.0, w = 0.0, r = 0.0, g = 0.0, b = 0.0;
                    int gv = 0, sv = 0;
                    bool hex = false;
                    std::vector<float> values;
                };
                auto cell_key = [](int x, int y) -> uint64_t { return ((uint64_t)(uint32_t)x << 32) | (uint32_t)y; };
                auto add = [&](std::unordered_map<uint64_t, Bin>& bins, int bx, int by, const HeatSample& s, double w, bool hex = false) {
                    Bin& b = bins[cell_key(bx, by)];
                    b.d += w; b.w += w; b.r += s.color.x * w; b.g += s.color.y * w; b.b += s.color.z * w;
                    if (s.prefer_gradient) b.gv++; else b.sv++;
                    b.hex = b.hex || hex;
                    if (s.has_value) b.values.push_back(s.value);
                };
                auto build_group = [&](const std::vector<HeatSample>& group) {
                    std::vector<CachedHeatCell> out;
                    if (group.empty()) return out;
                    const HeatSample& settings = group.front();
                    const float cell = std::max(2.0f, settings.cell_px);
                    const float zf = 1.0f;
                    const float bw = std::max(1.0f, settings.bandwidth_px * zf);
                    const float blur = std::max(0.0f, settings.blur_sigma_px);
                    std::unordered_map<uint64_t, Bin> bins;
                    std::unordered_map<uint64_t, Bin> coarse;
                    auto kde_add = [&](const HeatSample& s, float sigma_px, double ws) {
                        const float sigma = std::max(1.0f, sigma_px);
                        const int radius = std::max(1, (int)std::ceil((3.0f * sigma) / cell));
                        const int cbx = (int)((s.x - origin_x) / cell), cby = (int)((s.y - origin_y) / cell);
                        const float two = 2.0f * sigma * sigma;
                        for (int dy = -radius; dy <= radius; ++dy) for (int dx = -radius; dx <= radius; ++dx) {
                            const float cx = origin_x + (cbx + dx + 0.5f) * cell;
                            const float cy = origin_y + (cby + dy + 0.5f) * cell;
                            const float d2 = (cx - s.x) * (cx - s.x) + (cy - s.y) * (cy - s.y);
                            const double w = std::exp(-(double)d2 / (double)two) * ws;
                            if (w < 1e-5) continue;
                            add(bins, cbx + dx, cby + dy, s, w);
                        }
                    };
                    for (const auto& s : group) {
                        const int algo = s.algo;
                        if (algo == kAggregateGridBinning || algo == kAggregateMedianChoropleth) {
                            add(bins, (int)((s.x - origin_x) / cell), (int)((s.y - origin_y) / cell), s, 1.0);
                        } else if (algo == kAggregateKdeGaussian) {
                            kde_add(s, bw, 1.0);
                        } else if (algo == kAggregateGpuSplatBlur) {
                            kde_add(s, std::sqrt(bw * bw + blur * blur), 1.0);
                        } else if (algo == kAggregateHexBinning) {
                            const float hh = std::max(1.0f, cell * 0.8660254f);
                            const float gx = (s.x - origin_x) / cell, gy = (s.y - origin_y) / hh;
                            add(bins, (int)std::round(gx - gy * 0.5f), (int)std::round(gy), s, 1.0, true);
                        } else {
                            const float coarse_cell = cell * 2.0f;
                            add(bins, (int)((s.x - origin_x) / cell), (int)((s.y - origin_y) / cell), s, 1.0);
                            add(coarse, (int)((s.x - origin_x) / coarse_cell), (int)((s.y - origin_y) / coarse_cell), s, 1.0);
                        }
                    }
                    const bool median_choropleth = settings.algo == kAggregateMedianChoropleth;
                    if (settings.multires_enabled && !coarse.empty()) {
                        const double blend = std::clamp((double)settings.multires_blend, 0.0, 1.0);
                        for (auto& kv : bins) {
                            int bx = (int)(uint32_t)(kv.first >> 32), by = (int)(uint32_t)(kv.first & 0xffffffffu);
                            auto it = coarse.find(cell_key(bx / 2, by / 2));
                            if (it != coarse.end()) kv.second.d = kv.second.d * (1.0 - blend) + it->second.d * blend;
                        }
                    }
                    double maxd = 0.0;
                    std::vector<double> vals; vals.reserve(bins.size());
                    if (median_choropleth) {
                        for (auto& kv : bins) {
                            auto& bin_values = kv.second.values;
                            if (bin_values.empty()) continue;
                            const size_t mid = bin_values.size() / 2;
                            std::nth_element(bin_values.begin(), bin_values.begin() + mid, bin_values.end());
                            vals.push_back(bin_values[mid]);
                        }
                    } else {
                        for (const auto& kv : bins) vals.push_back(kv.second.d);
                    }
                    if (!vals.empty()) {
                        maxd = *std::max_element(vals.begin(), vals.end());
                        const double pct = std::clamp((double)settings.percentile_clip, 50.0, 100.0);
                        if (pct < 100.0) {
                            size_t k = (size_t)std::clamp((int)std::floor((pct / 100.0) * (double)(vals.size() - 1)), 0, (int)vals.size() - 1);
                            std::nth_element(vals.begin(), vals.begin() + k, vals.end());
                            maxd = std::max(1e-6, vals[k]);
                        }
                    }
                    if (!median_choropleth && maxd <= 0.0) return out;
                    double mind = 0.0;
                    if (median_choropleth && !vals.empty()) mind = *std::min_element(vals.begin(), vals.end());
                    out.reserve(bins.size());
                    for (const auto& kv : bins) {
                        const int bx = (int)(uint32_t)(kv.first >> 32), by = (int)(uint32_t)(kv.first & 0xffffffffu);
                        const Bin& b = kv.second;
                        if (median_choropleth && b.values.empty()) continue;
                        const double metric = median_choropleth ? (double)b.values[b.values.size() / 2] : b.d;
                        const float t = median_choropleth
                            ? std::clamp((float)((metric - mind) / std::max(1e-6, maxd - mind)), 0.0f, 1.0f)
                            : std::clamp((float)(metric / maxd), 0.0f, 1.0f);
                        const float iw = (b.w > 0.0) ? (float)(1.0 / b.w) : 0.0f;
                        const ImVec4 base(std::clamp((float)(b.r * iw), 0.0f, 1.0f), std::clamp((float)(b.g * iw), 0.0f, 1.0f), std::clamp((float)(b.b * iw), 0.0f, 1.0f), 1.0f);
                        const bool grad = b.gv >= b.sv;
                        ImVec4 c = grad ? ImVec4(0.12f + 0.70f * t, 0.35f + 0.30f * t, 0.75f - 0.62f * t, 0.62f)
                                        : ImVec4(base.x * (0.30f + 0.70f * t), base.y * (0.30f + 0.70f * t), base.z * (0.30f + 0.70f * t), 0.22f + 0.58f * t);
                        if (median_choropleth) {
                            const ImVec4 hc = defaultHeatColor(t);
                            c = ImVec4(hc.x, hc.y, hc.z, 0.76f);
                        }
                        const int algo = settings.algo;
                        const bool smooth_surface =
                            algo == kAggregateKdeGaussian ||
                            algo == kAggregateGpuSplatBlur ||
                            algo == kAggregateMultiResPyramid;
                        CachedHeatCell cc;
                        cc.world_space = true;
                        cc.draw_outline = !smooth_surface;
                        cc.fill = ImGui::ColorConvertFloat4ToU32(c);
                        cc.outline = smooth_surface ? 0 : ImGui::ColorConvertFloat4ToU32(ImVec4(c.x * 0.75f, c.y * 0.75f, c.z * 0.75f, 0.85f));
                        if (b.hex) {
                            const float hh = std::max(1.0f, cell * 0.8660254f);
                            cc.is_hex = true;
                            cc.cx = origin_x + ((float)bx + (float)by * 0.5f + 0.5f) * cell;
                            cc.cy = origin_y + ((float)by + 0.5f) * hh;
                            cc.hw = cell * 0.5f;
                            cc.hh = hh * 0.5f;
                        } else {
                            const float overlap = smooth_surface ? std::max(0.75f, cell * 0.035f) : 0.0f;
                            cc.is_hex = false;
                            cc.x0 = origin_x + (float)bx * cell - overlap; cc.y0 = origin_y + (float)by * cell - overlap;
                            cc.x1 = cc.x0 + cell + overlap * 2.0f; cc.y1 = cc.y0 + cell + overlap * 2.0f;
                        }
                        out.push_back(cc);
                    }
                    return out;
                };
                std::unordered_map<int, std::vector<HeatSample>> by_layer;
                for (const auto& s : samples) by_layer[s.layer].push_back(s);
                HeatmapRenderData out;
                float raster_min_lon = std::numeric_limits<float>::infinity();
                float raster_min_lat = std::numeric_limits<float>::infinity();
                float raster_max_lon = -std::numeric_limits<float>::infinity();
                float raster_max_lat = -std::numeric_limits<float>::infinity();
                for (const auto& s : samples) {
                    const int algo = s.algo;
                    if (algo != kAggregateKdeGaussian &&
                        algo != kAggregateGpuSplatBlur &&
                        algo != kAggregateMultiResPyramid) continue;
                    raster_min_lon = std::min(raster_min_lon, s.lon);
                    raster_max_lon = std::max(raster_max_lon, s.lon);
                    raster_min_lat = std::min(raster_min_lat, s.lat);
                    raster_max_lat = std::max(raster_max_lat, s.lat);
                }
                if (!std::isfinite(raster_min_lon) || raster_max_lon <= raster_min_lon) {
                    raster_min_lon = view_min_lon; raster_max_lon = view_max_lon;
                }
                if (!std::isfinite(raster_min_lat) || raster_max_lat <= raster_min_lat) {
                    raster_min_lat = view_min_lat; raster_max_lat = view_max_lat;
                }
                const float lon_pad = std::max(0.0005f, (raster_max_lon - raster_min_lon) * 0.08f);
                const float lat_pad = std::max(0.0005f, (raster_max_lat - raster_min_lat) * 0.08f);
                raster_min_lon -= lon_pad;
                raster_max_lon += lon_pad;
                raster_min_lat = std::max(-85.0f, raster_min_lat - lat_pad);
                raster_max_lat = std::min(85.0f, raster_max_lat + lat_pad);
                const float aspect = std::clamp((raster_max_lon - raster_min_lon) / std::max(0.0001f, raster_max_lat - raster_min_lat), 0.35f, 2.85f);
                const int rw = std::clamp((int)std::lround((float)raster_base_px * std::sqrt(aspect)), 384, raster_max_px);
                const int rh = std::clamp((int)std::lround((float)rw / aspect), 384, raster_max_px);
                out.raster.w = rw;
                out.raster.h = rh;
                out.raster.min_lon = raster_min_lon;
                out.raster.min_lat = raster_min_lat;
                out.raster.max_lon = raster_max_lon;
                out.raster.max_lat = raster_max_lat;
                out.raster.rgba.assign((size_t)rw * (size_t)rh * 4, 0);
                auto blend_rgba = [&](int x, int y, const ImVec4& src) {
                    if (x < 0 || y < 0 || x >= rw || y >= rh || src.w <= 0.0f) return;
                    const size_t i = ((size_t)y * (size_t)rw + (size_t)x) * 4;
                    const float da = out.raster.rgba[i + 3] / 255.0f;
                    const float sa = std::clamp(src.w, 0.0f, 1.0f);
                    const float oa = sa + da * (1.0f - sa);
                    if (oa <= 0.0f) return;
                    const float dr = out.raster.rgba[i + 0] / 255.0f;
                    const float dg = out.raster.rgba[i + 1] / 255.0f;
                    const float db = out.raster.rgba[i + 2] / 255.0f;
                    const float orr = (src.x * sa + dr * da * (1.0f - sa)) / oa;
                    const float ogg = (src.y * sa + dg * da * (1.0f - sa)) / oa;
                    const float obb = (src.z * sa + db * da * (1.0f - sa)) / oa;
                    out.raster.rgba[i + 0] = (unsigned char)std::clamp((int)std::lround(orr * 255.0f), 0, 255);
                    out.raster.rgba[i + 1] = (unsigned char)std::clamp((int)std::lround(ogg * 255.0f), 0, 255);
                    out.raster.rgba[i + 2] = (unsigned char)std::clamp((int)std::lround(obb * 255.0f), 0, 255);
                    out.raster.rgba[i + 3] = (unsigned char)std::clamp((int)std::lround(oa * 255.0f), 0, 255);
                };
                auto build_raster_group = [&](const std::vector<HeatSample>& group) {
                    if (group.empty()) return;
                    const HeatSample& settings = group.front();
                    const float sx = (float)rw / std::max(0.0001f, raster_max_lon - raster_min_lon);
                    const float sy = (float)rh / std::max(0.0001f, raster_max_lat - raster_min_lat);
                    const float screen_span_x =
                        viewport_w * (raster_max_lon - raster_min_lon) /
                        std::max(0.0001f, view_max_lon - view_min_lon);
                    const float screen_span_y =
                        viewport_h * (raster_max_lat - raster_min_lat) /
                        std::max(0.0001f, view_max_lat - view_min_lat);
                    const float screen_px_per_raster_px = std::max(
                        0.0001f,
                        0.5f * (screen_span_x / std::max(1, rw) + screen_span_y / std::max(1, rh)));
                    const float zf = settings.zoom_adaptive_bandwidth
                        ? std::clamp(1.0f + 0.12f * (float)(max_zoom - zoom), 1.0f, 3.0f)
                        : 1.0f;
                    float sigma = std::max(1.0f, settings.bandwidth_px * zf);
                    if (settings.algo == kAggregateGpuSplatBlur) sigma = std::sqrt(sigma * sigma + settings.blur_sigma_px * settings.blur_sigma_px);
                    if (settings.algo == kAggregateMultiResPyramid && settings.multires_enabled) sigma *= 1.0f + std::clamp(settings.multires_blend, 0.0f, 1.0f);
                    const float sigma_r = std::max(1.0f, sigma / screen_px_per_raster_px);
                    std::vector<float> density((size_t)rw * (size_t)rh, 0.0f);
                    std::vector<float> cr(density.size(), 0.0f), cg(density.size(), 0.0f), cb(density.size(), 0.0f), cw(density.size(), 0.0f);
                    std::vector<float> gv(density.size(), 0.0f), sv(density.size(), 0.0f);
                    auto idx = [&](int x, int y) -> size_t { return (size_t)y * (size_t)rw + (size_t)x; };
                    for (const auto& s : group) {
                        const float px = (s.lon - raster_min_lon) * sx;
                        const float py = (raster_max_lat - s.lat) * sy;
                        const int x = std::clamp((int)std::floor(px), 0, rw - 1);
                        const int y = std::clamp((int)std::floor(py), 0, rh - 1);
                        const size_t i = idx(x, y);
                        density[i] += 1.0f;
                        cr[i] += s.color.x;
                        cg[i] += s.color.y;
                        cb[i] += s.color.z;
                        cw[i] += 1.0f;
                        if (s.prefer_gradient) gv[i] += 1.0f; else sv[i] += 1.0f;
                    }
                    auto blur_field = [&](std::vector<float>& src, bool horizontal) {
                        if (sigma_r <= 0.05f) return;
                        const int radius = std::max(1, (int)std::ceil(3.0f * sigma_r));
                        std::vector<float> kernel((size_t)radius * 2 + 1, 0.0f);
                        float ksum = 0.0f;
                        for (int k = -radius; k <= radius; ++k) {
                            const float v = std::exp(-(float)(k * k) / (2.0f * sigma_r * sigma_r));
                            kernel[(size_t)(k + radius)] = v;
                            ksum += v;
                        }
                        if (ksum > 0.0f) for (float& v : kernel) v /= ksum;
                        std::vector<float> dst(src.size(), 0.0f);
                        for (int y = 0; y < rh; ++y) {
                            for (int x = 0; x < rw; ++x) {
                                float acc = 0.0f;
                                for (int k = -radius; k <= radius; ++k) {
                                    const int sx2 = horizontal ? std::clamp(x + k, 0, rw - 1) : x;
                                    const int sy2 = horizontal ? y : std::clamp(y + k, 0, rh - 1);
                                    acc += src[idx(sx2, sy2)] * kernel[(size_t)(k + radius)];
                                }
                                dst[idx(x, y)] = acc;
                            }
                        }
                        src.swap(dst);
                    };
                    for (auto* field : {&density, &cr, &cg, &cb, &cw, &gv, &sv}) {
                        blur_field(*field, true);
                        blur_field(*field, false);
                    }
                    std::vector<float> vals;
                    vals.reserve(density.size());
                    for (float d : density) if (d > 1e-6f) vals.push_back(d);
                    if (vals.empty()) return;
                    float maxd = *std::max_element(vals.begin(), vals.end());
                    const float pct = std::clamp(settings.percentile_clip, 50.0f, 100.0f);
                    if (pct < 100.0) {
                        const size_t k = (size_t)std::clamp((int)std::floor((pct / 100.0f) * (float)(vals.size() - 1)), 0, (int)vals.size() - 1);
                        std::nth_element(vals.begin(), vals.begin() + k, vals.end());
                        maxd = std::max(1e-6f, vals[k]);
                    }
                    for (int y = 0; y < rh; ++y) {
                        for (int x = 0; x < rw; ++x) {
                            const size_t i = (size_t)y * (size_t)rw + (size_t)x;
                            if (density[i] <= 1e-6) continue;
                            const float t = std::clamp((float)(density[i] / maxd), 0.0f, 1.0f);
                            const float iw = cw[i] > 0.0f ? 1.0f / cw[i] : 0.0f;
                            const ImVec4 base(std::clamp((float)cr[i] * iw, 0.0f, 1.0f), std::clamp((float)cg[i] * iw, 0.0f, 1.0f), std::clamp((float)cb[i] * iw, 0.0f, 1.0f), 1.0f);
                            const bool use_gradient = gv[i] >= sv[i];
                            const ImVec4 src = use_gradient
                                ? ImVec4(0.12f + 0.70f * t, 0.35f + 0.30f * t, 0.75f - 0.62f * t, 0.62f * t)
                                : ImVec4(base.x * (0.30f + 0.70f * t), base.y * (0.30f + 0.70f * t), base.z * (0.30f + 0.70f * t), 0.58f * t);
                            blend_rgba(x, y, src);
                        }
                    }
                    out.has_raster = true;
                };
                for (const auto& kv : by_layer) {
                    if (kv.second.empty()) continue;
                    const int algo = kv.second.front().algo;
                    if (algo == kAggregateKdeGaussian ||
                        algo == kAggregateGpuSplatBlur ||
                        algo == kAggregateMultiResPyramid) {
                        build_raster_group(kv.second);
                    } else {
                        std::vector<CachedHeatCell> group_cells = build_group(kv.second);
                        out.cells.insert(out.cells.end(), group_cells.begin(), group_cells.end());
                    }
                }
                return {key, std::move(out)};
}

ImVec4 defaultHeatColor(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    const ImVec4 cold(0.12f, 0.35f, 0.75f, 1.0f);
    const ImVec4 mid(0.98f, 0.83f, 0.26f, 1.0f);
    const ImVec4 hot(0.82f, 0.14f, 0.12f, 1.0f);
    auto lerp = [](const ImVec4& a, const ImVec4& b, float u) {
        return ImVec4(
            a.x + (b.x - a.x) * u,
            a.y + (b.y - a.y) * u,
            a.z + (b.z - a.z) * u,
            a.w + (b.w - a.w) * u);
    };
    if (t < 0.5f) return lerp(cold, mid, t * 2.0f);
    return lerp(mid, hot, (t - 0.5f) * 2.0f);
}

static std::vector<CachedHeatCell> buildImmediateHeatmapCellsForSettings(
    const std::vector<HeatSample>& heat_samples,
    float origin_x,
    float origin_y,
    float viewport_w,
    float viewport_h,
    int zoom,
    int max_zoom,
    int heatmap_algo,
    float global_heat_cell,
    float heatmap_bandwidth_px,
    float heatmap_blur_sigma_px,
    float heatmap_percentile_clip,
    bool heatmap_zoom_adaptive_bandwidth,
    bool heatmap_multires_enabled,
    float heatmap_multires_blend) {
    std::vector<CachedHeatCell> frame_heat_cells;
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
auto pack_bin = [](int bx, int by) -> uint64_t {
        return ((uint64_t)(uint32_t)bx << 32) | (uint32_t)by;
    };
auto unpack_bin = [](uint64_t key, int& bx, int& by) {
        bx = (int)(uint32_t)(key >> 32);
        by = (int)(uint32_t)(key & 0xffffffffu);
    };
auto accum_bin = [&](HeatBin& b, const HeatSample& s, double w) {
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
    };

std::unordered_map<uint64_t, HeatBin> global_heat_bins;
std::unordered_map<uint64_t, HeatBin> coarse_heat_bins;
double max_density = 0.0;
const float zoom_factor = heatmap_zoom_adaptive_bandwidth ? std::clamp(1.0f + 0.12f * (float)(max_zoom - zoom), 1.0f, 3.0f) : 1.0f;
const float bw_px = std::max(1.0f, heatmap_bandwidth_px * zoom_factor);
const float blur_sigma = std::max(0.0f, heatmap_blur_sigma_px);

auto add_grid_sample = [&](const HeatSample& s, float cell, double w) {
        const int bx = (int)((s.x - origin_x) / cell);
        const int by = (int)((s.y - origin_y) / cell);
        HeatBin& b = global_heat_bins[pack_bin(bx, by)];
        accum_bin(b, s, w);
    };
auto add_kde_sample = [&](const HeatSample& s, float cell, float sigma_px, double wscale) {
        const float sigma = std::max(1.0f, sigma_px);
        const int radius = std::max(1, (int)std::ceil((3.0f * sigma) / cell));
        const int cbx = (int)((s.x - origin_x) / cell);
        const int cby = (int)((s.y - origin_y) / cell);
        const float two_sigma2 = 2.0f * sigma * sigma;
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                const float cx = origin_x + (cbx + dx + 0.5f) * cell;
                const float cy = origin_y + (cby + dy + 0.5f) * cell;
                const float d2 = (cx - s.x) * (cx - s.x) + (cy - s.y) * (cy - s.y);
                const double w = std::exp(-(double)d2 / (double)two_sigma2) * wscale;
                if (w < 1e-5) continue;
                HeatBin& b = global_heat_bins[pack_bin(cbx + dx, cby + dy)];
                accum_bin(b, s, w);
            }
        }
    };

if (heatmap_algo == kAggregateGridBinning || heatmap_algo == kAggregateMedianChoropleth) {
        for (const auto& s : heat_samples) add_grid_sample(s, global_heat_cell, 1.0);
} else if (heatmap_algo == kAggregateKdeGaussian) {
        for (const auto& s : heat_samples) add_kde_sample(s, global_heat_cell, bw_px, 1.0);
} else if (heatmap_algo == kAggregateGpuSplatBlur) {
        // Explicit splat grid + separable blur pipeline (CPU implementation of the same model).
        if (!heat_samples.empty()) {
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

            // Splat pass.
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

            const float sigma_cells = std::max(0.0f, blur_sigma / global_heat_cell);
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

            // Import blurred fields into generic bins.
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
                    global_heat_bins[pack_bin(x, y)] = b;
                }
            }
        }
} else if (heatmap_algo == kAggregateHexBinning) {
        const float hex_w = global_heat_cell;
        const float hex_h = std::max(1.0f, hex_w * 0.8660254f);
        for (const auto& s : heat_samples) {
            const float gx = (s.x - origin_x) / hex_w;
            const float gy = (s.y - origin_y) / hex_h;
            const int q = (int)std::round(gx - gy * 0.5f);
            const int r = (int)std::round(gy);
            HeatBin& b = global_heat_bins[pack_bin(q, r)];
            accum_bin(b, s, 1.0);
        }
} else {
        const float fine = global_heat_cell;
        const float coarse = global_heat_cell * 2.0f;
        for (const auto& s : heat_samples) {
            add_grid_sample(s, fine, 1.0);
            const int bx = (int)((s.x - origin_x) / coarse);
            const int by = (int)((s.y - origin_y) / coarse);
            HeatBin& b = coarse_heat_bins[pack_bin(bx, by)];
            accum_bin(b, s, 1.0);
        }
        if (heatmap_multires_enabled) {
            const double blend = std::clamp((double)heatmap_multires_blend, 0.0, 1.0);
            for (auto& kv : global_heat_bins) {
                int bx = 0, by = 0;
                unpack_bin(kv.first, bx, by);
                const int cbx = bx / 2;
                const int cby = by / 2;
                auto it = coarse_heat_bins.find(pack_bin(cbx, cby));
                if (it != coarse_heat_bins.end()) {
                    kv.second.density = kv.second.density * (1.0 - blend) + it->second.density * blend;
                }
            }
        }
    }

const bool median_choropleth = heatmap_algo == kAggregateMedianChoropleth;
if (!global_heat_bins.empty()) {
        std::vector<double> dens;
        dens.reserve(global_heat_bins.size());
        if (median_choropleth) {
            for (auto& kv : global_heat_bins) {
                auto& values = kv.second.values;
                if (values.empty()) continue;
                const size_t mid = values.size() / 2;
                std::nth_element(values.begin(), values.begin() + mid, values.end());
                dens.push_back(values[mid]);
            }
        } else {
            for (const auto& kv : global_heat_bins) dens.push_back(kv.second.density);
        }
        if (!dens.empty()) {
            max_density = *std::max_element(dens.begin(), dens.end());
            const double pct = std::clamp((double)heatmap_percentile_clip, 50.0, 100.0);
            if (pct < 100.0) {
                const size_t kth = (size_t)std::clamp((int)std::floor((pct / 100.0) * (double)(dens.size() - 1)), 0, (int)dens.size() - 1);
                std::nth_element(dens.begin(), dens.begin() + kth, dens.end());
                max_density = std::max(1e-6, dens[kth]);
            }
        }
    }

    double min_density = 0.0;
    if (median_choropleth && !global_heat_bins.empty()) {
        bool have_value = false;
        for (const auto& kv : global_heat_bins) {
            if (kv.second.values.empty()) continue;
            const double v = kv.second.values[kv.second.values.size() / 2];
            min_density = have_value ? std::min(min_density, v) : v;
            have_value = true;
        }
    }

    if (!global_heat_bins.empty() && (median_choropleth || max_density > 0.0)) {
        for (const auto& kv : global_heat_bins) {
            int bx = 0, by = 0;
            unpack_bin(kv.first, bx, by);
            const HeatBin& bin = kv.second;
            if (median_choropleth && bin.values.empty()) continue;
            const double metric = median_choropleth ? (double)bin.values[bin.values.size() / 2] : bin.density;
            const float t = median_choropleth
                ? std::clamp((float)((metric - min_density) / std::max(1e-6, max_density - min_density)), 0.0f, 1.0f)
                : std::clamp((float)(metric / max_density), 0.0f, 1.0f);
            const double inv_n = (bin.color_w_sum > 0.0) ? (1.0 / bin.color_w_sum) : 0.0;
            const double r_mean = bin.r_sum * inv_n;
            const double g_mean = bin.g_sum * inv_n;
            const double b_mean = bin.b_sum * inv_n;
            const double r_var = std::max(0.0, bin.rr_sum * inv_n - r_mean * r_mean);
            const double g_var = std::max(0.0, bin.gg_sum * inv_n - g_mean * g_mean);
            const double b_var = std::max(0.0, bin.bb_sum * inv_n - b_mean * b_mean);
            const double avg_var = (r_var + g_var + b_var) / 3.0;
            const bool monochrome_bin = avg_var < 0.0015;
            const bool prefer_gradient = bin.gradient_votes >= bin.solid_votes;

            ImU32 fill = 0;
            ImU32 outline = 0;
            if (median_choropleth) {
                const ImVec4 hc = defaultHeatColor(t);
                fill = ImGui::ColorConvertFloat4ToU32(ImVec4(hc.x, hc.y, hc.z, 0.76f));
                outline = ImGui::ColorConvertFloat4ToU32(ImVec4(hc.x * 0.72f, hc.y * 0.72f, hc.z * 0.72f, 0.88f));
            } else if (monochrome_bin || !prefer_gradient) {
                const ImVec4 base(
                    std::clamp((float)r_mean, 0.0f, 1.0f),
                    std::clamp((float)g_mean, 0.0f, 1.0f),
                    std::clamp((float)b_mean, 0.0f, 1.0f),
                    1.0f);
                const float shade = 0.30f + 0.70f * t;
                const ImVec4 ramp(
                    std::clamp(base.x * shade, 0.0f, 1.0f),
                    std::clamp(base.y * shade, 0.0f, 1.0f),
                    std::clamp(base.z * shade, 0.0f, 1.0f),
                    0.22f + 0.58f * t);
                fill = ImGui::ColorConvertFloat4ToU32(ramp);
                outline = ImGui::ColorConvertFloat4ToU32(ImVec4(
                    std::clamp(ramp.x * 0.70f, 0.0f, 1.0f),
                    std::clamp(ramp.y * 0.70f, 0.0f, 1.0f),
                    std::clamp(ramp.z * 0.70f, 0.0f, 1.0f),
                    0.82f));
        } else {
                const ImVec4 hc = defaultHeatColor(t);
                fill = ImGui::ColorConvertFloat4ToU32(ImVec4(hc.x, hc.y, hc.z, 0.62f));
                outline = ImGui::ColorConvertFloat4ToU32(ImVec4(hc.x * 0.75f, hc.y * 0.75f, hc.z * 0.75f, 0.85f));
            }

        if (heatmap_algo == kAggregateHexBinning) {
                const float hex_w = global_heat_cell;
                const float hex_h = std::max(1.0f, hex_w * 0.8660254f);
                const float cx = origin_x + ((float)bx + (float)by * 0.5f + 0.5f) * hex_w;
                const float cy = origin_y + ((float)by + 0.5f) * hex_h;
                CachedHeatCell c;
                c.world_space = true;
                c.is_hex = true;
                c.draw_outline = true;
                c.cx = cx;
                c.cy = cy;
                c.hw = hex_w * 0.5f;
                c.hh = hex_h * 0.5f;
                c.fill = fill;
                c.outline = outline;
                frame_heat_cells.push_back(c);
        } else {
                const bool smooth_surface =
                    heatmap_algo == kAggregateKdeGaussian ||
                    heatmap_algo == kAggregateGpuSplatBlur ||
                    heatmap_algo == kAggregateMultiResPyramid;
                const float overlap = smooth_surface ? std::max(0.75f, global_heat_cell * 0.035f) : 0.0f;
                const float x0 = origin_x + (float)bx * global_heat_cell;
                const float y0 = origin_y + (float)by * global_heat_cell;
                const float x1 = x0 + global_heat_cell;
                const float y1 = y0 + global_heat_cell;
                CachedHeatCell c;
                c.world_space = true;
                c.is_hex = false;
                c.draw_outline = !smooth_surface;
                c.x0 = x0 - overlap; c.y0 = y0 - overlap; c.x1 = x1 + overlap; c.y1 = y1 + overlap;
                c.fill = fill;
                c.outline = smooth_surface ? 0 : outline;
                frame_heat_cells.push_back(c);
            }
        }
    }
    return frame_heat_cells;
}

std::vector<CachedHeatCell> buildImmediateHeatmapCells(
    const std::vector<HeatSample>& heat_samples,
    float origin_x,
    float origin_y,
    float viewport_w,
    float viewport_h,
    int zoom,
    int max_zoom,
    int heatmap_algo,
    float global_heat_cell,
    float heatmap_bandwidth_px,
    float heatmap_blur_sigma_px,
    float heatmap_percentile_clip,
    bool heatmap_zoom_adaptive_bandwidth,
    bool heatmap_multires_enabled,
    float heatmap_multires_blend) {
    std::vector<CachedHeatCell> out;
    if (heat_samples.empty()) return out;

    std::unordered_map<int, std::vector<HeatSample>> by_layer;
    for (const auto& sample : heat_samples) {
        by_layer[sample.layer].push_back(sample);
    }

    for (const auto& kv : by_layer) {
        if (kv.second.empty()) continue;
        const HeatSample& settings = kv.second.front();
        std::vector<CachedHeatCell> cells = buildImmediateHeatmapCellsForSettings(
            kv.second,
            origin_x,
            origin_y,
            viewport_w,
            viewport_h,
            zoom,
            max_zoom,
            settings.algo,
            settings.cell_px,
            settings.bandwidth_px,
            settings.blur_sigma_px,
            settings.percentile_clip,
            settings.zoom_adaptive_bandwidth,
            settings.multires_enabled,
            settings.multires_blend);
        out.insert(out.end(), cells.begin(), cells.end());
    }

    if (out.empty()) {
        return buildImmediateHeatmapCellsForSettings(
            heat_samples,
            origin_x,
            origin_y,
            viewport_w,
            viewport_h,
            zoom,
            max_zoom,
            heatmap_algo,
            global_heat_cell,
            heatmap_bandwidth_px,
            heatmap_blur_sigma_px,
            heatmap_percentile_clip,
            heatmap_zoom_adaptive_bandwidth,
            heatmap_multires_enabled,
            heatmap_multires_blend);
    }
    return out;
}
