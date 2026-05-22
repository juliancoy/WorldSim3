#include "gpu_profiler_tab.h"

#include "imgui.h"
#include "worldsim_app.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <numeric>
#include <sstream>
#include <string>

namespace {
struct ProcStatusSnapshot {
    size_t vm_rss_kb = 0;
    size_t vm_size_kb = 0;
    size_t vm_swap_kb = 0;
    size_t threads = 0;
};

ProcStatusSnapshot readProcStatusSnapshot() {
    ProcStatusSnapshot out;
#if defined(__linux__)
    std::ifstream in("/proc/self/status");
    std::string key;
    while (in >> key) {
        if (key == "VmRSS:") in >> out.vm_rss_kb;
        else if (key == "VmSize:") in >> out.vm_size_kb;
        else if (key == "VmSwap:") in >> out.vm_swap_kb;
        else if (key == "Threads:") in >> out.threads;
        std::string rest;
        std::getline(in, rest);
    }
#endif
    return out;
}

std::string formatBytes(uint64_t bytes) {
    static const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = (double)bytes;
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(kUnits)) {
        value /= 1024.0;
        unit++;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f %s", value, kUnits[unit]);
    return buf;
}

std::string formatKb(size_t kb) {
    return formatBytes((uint64_t)kb * 1024ULL);
}

std::vector<ProfileFrameSample> copyProfileSamples(const GpuProfilerTabContext& ctx) {
    std::vector<ProfileFrameSample> out;
    if (!ctx.profile_mutex || !ctx.profile_samples || !ctx.profile_sample_pos || !ctx.profile_sample_count) return out;
    std::lock_guard<std::mutex> lk(*ctx.profile_mutex);
    const size_t sample_count = *ctx.profile_sample_count;
    const auto& ring = *ctx.profile_samples;
    out.reserve(sample_count);
    const size_t start = sample_count < ring.size() ? 0 : *ctx.profile_sample_pos;
    for (size_t i = 0; i < sample_count; ++i) {
        out.push_back(ring[(start + i) % ring.size()]);
    }
    return out;
}

void drawMetricSummary(const char* label, const std::vector<float>& values) {
    if (values.empty()) {
        ImGui::TextDisabled("%s: no samples", label);
        return;
    }
    auto [min_it, max_it] = std::minmax_element(values.begin(), values.end());
    const double avg = std::accumulate(values.begin(), values.end(), 0.0) / (double)values.size();
    ImGui::Text("%s: last %.2f | avg %.2f | min %.2f | max %.2f", label, values.back(), avg, *min_it, *max_it);
}

template <typename Fn>
std::vector<float> collectSeries(const std::vector<ProfileFrameSample>& samples, Fn fn) {
    std::vector<float> out;
    out.reserve(samples.size());
    for (const auto& sample : samples) out.push_back((float)fn(sample));
    return out;
}
}

void drawGpuProfilerTab(const GpuProfilerTabContext& ctx) {
    ImGuiTabItemFlags tab_flags = (ctx.tab_requested && *ctx.tab_requested) ? ImGuiTabItemFlags_SetSelected : 0;
    if (!ImGui::BeginTabItem("GPU Profiler", nullptr, tab_flags)) return;
    if (ctx.tab_requested) *ctx.tab_requested = false;

    const GpuProfilerLiveSnapshot gpu = getGpuProfilerLiveSnapshot();
    const ProcStatusSnapshot proc = readProcStatusSnapshot();
    const std::vector<ProfileFrameSample> samples = copyProfileSamples(ctx);

    if (gpu.oom_active) {
        ImGui::TextColored(ImVec4(0.90f, 0.28f, 0.22f, 1.0f), "GPU memory alert active");
        if (!gpu.last_error.empty()) ImGui::TextWrapped("%s", gpu.last_error.c_str());
    } else {
        ImGui::TextDisabled("Core Vulkan does not expose nvtop-style utilization percentages here. This tab shows live Vulkan heap capacity, app-managed GPU residency, and frame timing.");
    }

    if (ImGui::Button("Reload Parcel GPU") && ctx.reload_requested) {
        *ctx.reload_requested = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear Alert")) {
        clearGpuProfilerAlertState();
    }

    ImGui::Separator();
    ImGui::Text("Device: %s", gpu.physical_device_name.empty() ? "unknown" : gpu.physical_device_name.c_str());
    ImGui::Text("Process RSS: %s | VM: %s | Swap: %s | Threads: %zu",
        formatKb(proc.vm_rss_kb).c_str(),
        formatKb(proc.vm_size_kb).c_str(),
        formatKb(proc.vm_swap_kb).c_str(),
        proc.threads);
    ImGui::Text("Tile cache entries: %zu | Retired textures: %zu", gpu.tile_cache_entries, gpu.retired_texture_count);
    ImGui::Text("Parcel GPU resident: %s | Draw active: %s | OOM generation: %llu",
        gpu.parcel_gpu_resident ? "yes" : "no",
        gpu.parcel_gpu_draw_active ? "yes" : "no",
        (unsigned long long)gpu.oom_generation);

    ImGui::Text("Parcel device-local: %s | host-visible: %s",
        formatBytes(gpu.parcel_device_local_bytes).c_str(),
        formatBytes(gpu.parcel_host_visible_bytes).c_str());
    ImGui::Text("Retired parcel device-local: %s | host-visible: %s",
        formatBytes(gpu.retired_parcel_device_local_bytes).c_str(),
        formatBytes(gpu.retired_parcel_host_visible_bytes).c_str());

    const bool gpu_splat_active = ctx.prof_heatmap_gpu_splat_active && ctx.prof_heatmap_gpu_splat_active->load(std::memory_order_relaxed);
    const bool gpu_splat_hq = ctx.prof_heatmap_high_quality && ctx.prof_heatmap_high_quality->load(std::memory_order_relaxed);
    const bool heat_tex_resident = ctx.prof_heatmap_texture_resident && ctx.prof_heatmap_texture_resident->load(std::memory_order_relaxed);
    const bool heat_async = ctx.prof_heatmap_async_inflight && ctx.prof_heatmap_async_inflight->load(std::memory_order_relaxed);
    const size_t heat_tex_cache = ctx.prof_heatmap_texture_cache_entries ? ctx.prof_heatmap_texture_cache_entries->load(std::memory_order_relaxed) : 0;
    ImGui::Text("Heatmap GPU: splat=%s | high_quality=%s | texture_resident=%s | async=%s | texture_cache=%zu",
        gpu_splat_active ? "on" : "off",
        gpu_splat_hq ? "on" : "off",
        heat_tex_resident ? "yes" : "no",
        heat_async ? "yes" : "no",
        heat_tex_cache);

    if (ImGui::CollapsingHeader("Vulkan Heaps", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const auto& heap : gpu.heaps) {
            ImGui::BulletText(
                "Heap %u: %s | %s | memory types=%u | host-visible types=%u | coherent types=%u",
                heap.index,
                formatBytes(heap.size_bytes).c_str(),
                heap.device_local ? "device-local" : "shared/host",
                heap.memory_type_count,
                heap.host_visible_type_count,
                heap.host_coherent_type_count);
        }
    }

    if (ImGui::CollapsingHeader("Recent Events", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (gpu.recent_events.empty()) {
            ImGui::TextDisabled("No recent profiler events.");
        } else {
            for (const auto& event : gpu.recent_events) {
                ImGui::BulletText("%s", event.c_str());
            }
        }
    }

    if (samples.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("No frame samples collected yet.");
        ImGui::EndTabItem();
        return;
    }

    const std::vector<float> frame_ms = collectSeries(samples, [](const ProfileFrameSample& s) { return s.frame_ms; });
    const std::vector<float> present_ms = collectSeries(samples, [](const ProfileFrameSample& s) { return s.render_present_ms; });
    const std::vector<float> layers_ms = collectSeries(samples, [](const ProfileFrameSample& s) { return s.layers_ms; });
    const std::vector<float> tiles_ms = collectSeries(samples, [](const ProfileFrameSample& s) { return s.tiles_ms; });
    const std::vector<float> heatmap_ms = collectSeries(samples, [](const ProfileFrameSample& s) { return s.heatmap_ms; });
    const std::vector<float> overlays_ms = collectSeries(samples, [](const ProfileFrameSample& s) { return s.overlays_ms; });
    const std::vector<float> features_drawn = collectSeries(samples, [](const ProfileFrameSample& s) { return (double)s.features_drawn_points; });

    ImGui::Separator();
    ImGui::Text("Frame timeline (%zu samples)", samples.size());
    drawMetricSummary("Frame ms", frame_ms);
    ImGui::PlotLines("##gpu_prof_frame_ms", frame_ms.data(), (int)frame_ms.size(), 0, nullptr, 0.0f, *std::max_element(frame_ms.begin(), frame_ms.end()) * 1.10f, ImVec2(-1.0f, 72.0f));
    drawMetricSummary("Present ms", present_ms);
    ImGui::PlotLines("##gpu_prof_present_ms", present_ms.data(), (int)present_ms.size(), 0, nullptr, 0.0f, *std::max_element(present_ms.begin(), present_ms.end()) * 1.10f, ImVec2(-1.0f, 56.0f));

    ImGui::Separator();
    ImGui::Text("Frame phases");
    drawMetricSummary("Layer pass ms", layers_ms);
    ImGui::PlotLines("##gpu_prof_layers_ms", layers_ms.data(), (int)layers_ms.size(), 0, nullptr, 0.0f, *std::max_element(layers_ms.begin(), layers_ms.end()) * 1.10f, ImVec2(-1.0f, 56.0f));
    drawMetricSummary("Tile ms", tiles_ms);
    ImGui::PlotLines("##gpu_prof_tiles_ms", tiles_ms.data(), (int)tiles_ms.size(), 0, nullptr, 0.0f, *std::max_element(tiles_ms.begin(), tiles_ms.end()) * 1.10f, ImVec2(-1.0f, 56.0f));
    drawMetricSummary("Heatmap ms", heatmap_ms);
    ImGui::PlotLines("##gpu_prof_heatmap_ms", heatmap_ms.data(), (int)heatmap_ms.size(), 0, nullptr, 0.0f, *std::max_element(heatmap_ms.begin(), heatmap_ms.end()) * 1.10f, ImVec2(-1.0f, 56.0f));
    drawMetricSummary("Overlay ms", overlays_ms);
    ImGui::PlotLines("##gpu_prof_overlays_ms", overlays_ms.data(), (int)overlays_ms.size(), 0, nullptr, 0.0f, *std::max_element(overlays_ms.begin(), overlays_ms.end()) * 1.10f, ImVec2(-1.0f, 56.0f));

    ImGui::Separator();
    ImGui::Text("Rendered feature pressure");
    drawMetricSummary("Features drawn", features_drawn);
    ImGui::PlotLines("##gpu_prof_features_drawn", features_drawn.data(), (int)features_drawn.size(), 0, nullptr, 0.0f, *std::max_element(features_drawn.begin(), features_drawn.end()) * 1.10f, ImVec2(-1.0f, 56.0f));

    ImGui::EndTabItem();
}
