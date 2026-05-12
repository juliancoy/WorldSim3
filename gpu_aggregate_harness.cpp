#include "heatmap_gpu_aggregate.h"
#include "worldsim_app_internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

VkAllocationCallbacks* g_Allocator = nullptr;
VkInstance g_Instance = VK_NULL_HANDLE;
VkPhysicalDevice g_PhysicalDevice = VK_NULL_HANDLE;
VkDevice g_Device = VK_NULL_HANDLE;
uint32_t g_QueueFamily = (uint32_t)-1;
VkQueue g_Queue = VK_NULL_HANDLE;
std::mutex g_QueueSubmitMutex;
VkDescriptorPool g_DescriptorPool = VK_NULL_HANDLE;
VkSampler g_TileSampler = VK_NULL_HANDLE;
ImGui_ImplVulkanH_Window g_MainWindowData{};
int g_MinImageCount = 2;
bool g_SwapChainRebuild = false;
std::unordered_map<std::string, TileCacheEntry> g_TileCache;
std::list<std::string> g_TileLRU;
bool g_EnableValidationLayers = false;
std::vector<TileTexture> g_RetiredTextures;
ScreenshotRequestState g_ScreenshotState;

void check_vk_result(VkResult err) {
    if (err == VK_SUCCESS) return;
    std::fprintf(stderr, "[harness][vulkan] VkResult=%d\n", (int)err);
    std::abort();
}

void SetupVulkanWindow(ImGui_ImplVulkanH_Window*, VkSurfaceKHR, int, int) {}
void CleanupVulkanWindow() {}
void FrameRender(ImGui_ImplVulkanH_Window*, ImDrawData*) {}
void FramePresent(ImGui_ImplVulkanH_Window*) {}
void FrameRenderSecondary(ImGui_ImplVulkanH_Window*, ImDrawData*, bool&) {}
void FramePresentSecondary(ImGui_ImplVulkanH_Window*, bool&) {}
void drainRetiredTextures(bool) {}
void destroyTileTexture(TileTexture&) {}
void destroyTileTextureNow(TileTexture&) {}
bool uploadRgbaTexture(const unsigned char*, uint32_t, uint32_t, TileTexture&) { return false; }
TileSample getTileSample(const std::filesystem::path&, const std::string&, int, int, int, int) { return {}; }
const std::vector<std::vector<ImVec2>>& getTopoVectorLines(const std::filesystem::path&) {
    static const std::vector<std::vector<ImVec2>> empty;
    return empty;
}

static void setup_vulkan_minimal() {
    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "ws3-gpu-aggregate-harness";
    app_info.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app_info;
    check_vk_result(vkCreateInstance(&ici, g_Allocator, &g_Instance));

    uint32_t gpu_count = 0;
    check_vk_result(vkEnumeratePhysicalDevices(g_Instance, &gpu_count, nullptr));
    if (gpu_count == 0) {
        std::fprintf(stderr, "[harness] No Vulkan physical devices found\n");
        std::abort();
    }
    std::vector<VkPhysicalDevice> gpus(gpu_count);
    check_vk_result(vkEnumeratePhysicalDevices(g_Instance, &gpu_count, gpus.data()));
    g_PhysicalDevice = gpus[0];

    uint32_t qf_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_PhysicalDevice, &qf_count, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qf_count);
    vkGetPhysicalDeviceQueueFamilyProperties(g_PhysicalDevice, &qf_count, qfs.data());
    for (uint32_t i = 0; i < qf_count; ++i) {
        if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            g_QueueFamily = i;
            break;
        }
    }
    if (g_QueueFamily == (uint32_t)-1) {
        std::fprintf(stderr, "[harness] No graphics queue family found\n");
        std::abort();
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = g_QueueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    check_vk_result(vkCreateDevice(g_PhysicalDevice, &dci, g_Allocator, &g_Device));
    vkGetDeviceQueue(g_Device, g_QueueFamily, 0, &g_Queue);
}

struct BenchConfig {
    std::filesystem::path input = "data/layers/regional_parcels.geojson";
    std::string jurisdiction = "HowardCounty";
    int raster_w = 2048;
    int raster_h = 2048;
    int repeats = 5;
    float sigma_r = 6.0f;
    bool include_cpu_blur = true;
};

struct Bounds {
    float min_lon = std::numeric_limits<float>::max();
    float min_lat = std::numeric_limits<float>::max();
    float max_lon = -std::numeric_limits<float>::max();
    float max_lat = -std::numeric_limits<float>::max();
    bool valid = false;
};

static double now_ms() {
    using clock = std::chrono::steady_clock;
    static const auto start = clock::now();
    return std::chrono::duration<double, std::milli>(clock::now() - start).count();
}

static std::string prop_string(const json& props, const char* key) {
    auto it = props.find(key);
    if (it == props.end() || it->is_null()) return {};
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<long long>());
    if (it->is_number_unsigned()) return std::to_string(it->get<unsigned long long>());
    if (it->is_number_float()) return std::to_string(it->get<double>());
    return {};
}

static bool parse_float_prop(const json& props, const char* key, float& out) {
    auto it = props.find(key);
    if (it == props.end() || it->is_null()) return false;
    try {
        if (it->is_number()) {
            out = it->get<float>();
            return true;
        }
        if (it->is_string()) {
            std::string s = it->get<std::string>();
            std::string filtered;
            filtered.reserve(s.size());
            for (unsigned char c : s) {
                if (std::isdigit(c) || c == '.' || c == '-') filtered.push_back((char)c);
            }
            if (filtered.empty()) return false;
            out = std::stof(filtered);
            return true;
        }
    } catch (...) {
    }
    return false;
}

static void expand_bounds(Bounds& b, float lon, float lat) {
    if (!std::isfinite(lon) || !std::isfinite(lat)) return;
    b.min_lon = std::min(b.min_lon, lon);
    b.min_lat = std::min(b.min_lat, lat);
    b.max_lon = std::max(b.max_lon, lon);
    b.max_lat = std::max(b.max_lat, lat);
    b.valid = true;
}

static void scan_coords(const json& coords, Bounds& b) {
    if (!coords.is_array() || coords.empty()) return;
    if (coords.size() >= 2 && coords[0].is_number() && coords[1].is_number()) {
        expand_bounds(b, coords[0].get<float>(), coords[1].get<float>());
        return;
    }
    for (const auto& child : coords) scan_coords(child, b);
}

static std::vector<HeatSample> load_howard_samples(const BenchConfig& cfg, Bounds& out_bounds) {
    std::ifstream in(cfg.input);
    if (!in) throw std::runtime_error("failed to open " + cfg.input.string());
    json fc;
    in >> fc;
    if (!fc.contains("features") || !fc["features"].is_array()) {
        throw std::runtime_error("input is not a GeoJSON FeatureCollection");
    }

    std::vector<HeatSample> samples;
    samples.reserve(120000);
    float min_value = std::numeric_limits<float>::max();
    float max_value = -std::numeric_limits<float>::max();

    for (const auto& feature : fc["features"]) {
        const json props = feature.value("properties", json::object());
        const std::string jur = prop_string(props, "jurisdiction");
        if (!cfg.jurisdiction.empty() && jur != cfg.jurisdiction && jur != "Howard County") continue;
        if (!feature.contains("geometry") || feature["geometry"].is_null()) continue;
        const auto& geom = feature["geometry"];
        if (!geom.contains("coordinates")) continue;
        Bounds fb;
        scan_coords(geom["coordinates"], fb);
        if (!fb.valid) continue;
        expand_bounds(out_bounds, fb.min_lon, fb.min_lat);
        expand_bounds(out_bounds, fb.max_lon, fb.max_lat);

        float value = 0.0f;
        const bool has_value =
            parse_float_prop(props, "current_value", value) ||
            parse_float_prop(props, "TOTAL_VALUE", value) ||
            parse_float_prop(props, "NFMTTLVL", value);
        if (has_value) {
            min_value = std::min(min_value, value);
            max_value = std::max(max_value, value);
        }

        HeatSample s;
        s.lon = (fb.min_lon + fb.max_lon) * 0.5f;
        s.lat = (fb.min_lat + fb.max_lat) * 0.5f;
        s.has_value = has_value;
        s.value = value;
        s.prefer_gradient = true;
        s.algo = 2;
        samples.push_back(s);
    }

    const bool value_range = std::isfinite(min_value) && std::isfinite(max_value) && max_value > min_value;
    for (auto& s : samples) {
        const float t = value_range && s.has_value ? std::clamp((s.value - min_value) / (max_value - min_value), 0.0f, 1.0f) : 0.5f;
        s.color = ImVec4(0.15f + 0.85f * t, 0.25f + 0.45f * (1.0f - t), 0.95f - 0.65f * t, 1.0f);
    }
    return samples;
}

static void blur_field(std::vector<float>& src, int w, int h, float sigma, bool horizontal) {
    if (sigma <= 0.05f) return;
    const int radius = std::max(1, (int)std::ceil(3.0f * sigma));
    std::vector<float> kernel((size_t)radius * 2 + 1, 0.0f);
    float ksum = 0.0f;
    for (int k = -radius; k <= radius; ++k) {
        const float v = std::exp(-(float)(k * k) / (2.0f * sigma * sigma));
        kernel[(size_t)(k + radius)] = v;
        ksum += v;
    }
    if (ksum > 0.0f) for (float& v : kernel) v /= ksum;
    std::vector<float> dst(src.size(), 0.0f);
    auto idx = [&](int x, int y) { return (size_t)y * (size_t)w + (size_t)x; };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float acc = 0.0f;
            for (int k = -radius; k <= radius; ++k) {
                const int sx = horizontal ? std::clamp(x + k, 0, w - 1) : x;
                const int sy = horizontal ? y : std::clamp(y + k, 0, h - 1);
                acc += src[idx(sx, sy)] * kernel[(size_t)(k + radius)];
            }
            dst[idx(x, y)] = acc;
        }
    }
    src.swap(dst);
}

static int run_howard_bench(const BenchConfig& cfg) {
    Bounds bounds;
    const double load_begin = now_ms();
    std::vector<HeatSample> samples = load_howard_samples(cfg, bounds);
    const double load_ms = now_ms() - load_begin;
    if (samples.empty() || !bounds.valid) {
        std::fprintf(stderr, "[harness] no matching samples in %s\n", cfg.input.string().c_str());
        return 2;
    }

    setup_vulkan_minimal();
    std::vector<double> gpu_ms;
    std::vector<double> blur_ms;
    gpu_ms.reserve((size_t)cfg.repeats);
    blur_ms.reserve((size_t)cfg.repeats);
    double last_sum_density = 0.0;
    double last_max_density = 0.0;

    for (int r = 0; r < cfg.repeats; ++r) {
        std::vector<float> density, cr, cg, cb, cw, gv, sv;
        std::string err;
        const double gpu_begin = now_ms();
        const bool ok = buildGpuSplatAggregate(
            samples,
            cfg.raster_w,
            cfg.raster_h,
            bounds.min_lon,
            bounds.min_lat,
            bounds.max_lon,
            bounds.max_lat,
            cfg.sigma_r,
            density,
            cr,
            cg,
            cb,
            cw,
            gv,
            sv,
            &err);
        const double one_gpu_ms = now_ms() - gpu_begin;
        if (!ok) {
            std::fprintf(stderr, "[harness] GPU aggregate FAILED: %s\n", err.c_str());
            CleanupVulkan();
            return 3;
        }
        gpu_ms.push_back(one_gpu_ms);

        const double blur_begin = now_ms();
        if (cfg.include_cpu_blur) {
            for (auto* field : {&density, &cr, &cg, &cb, &cw, &gv, &sv}) {
                blur_field(*field, cfg.raster_w, cfg.raster_h, cfg.sigma_r, true);
                blur_field(*field, cfg.raster_w, cfg.raster_h, cfg.sigma_r, false);
            }
        }
        blur_ms.push_back(now_ms() - blur_begin);

        last_sum_density = 0.0;
        last_max_density = 0.0;
        for (float d : density) {
            last_sum_density += d;
            last_max_density = std::max(last_max_density, (double)d);
        }
    }

    auto avg = [](const std::vector<double>& xs) {
        double sum = 0.0;
        for (double x : xs) sum += x;
        return xs.empty() ? 0.0 : sum / (double)xs.size();
    };
    auto minv = [](const std::vector<double>& xs) {
        return xs.empty() ? 0.0 : *std::min_element(xs.begin(), xs.end());
    };
    auto maxv = [](const std::vector<double>& xs) {
        return xs.empty() ? 0.0 : *std::max_element(xs.begin(), xs.end());
    };

    std::printf("{\n");
    std::printf("  \"input\": \"%s\",\n", cfg.input.string().c_str());
    std::printf("  \"jurisdiction\": \"%s\",\n", cfg.jurisdiction.c_str());
    std::printf("  \"samples\": %zu,\n", samples.size());
    std::printf("  \"extent\": {\"min_lon\": %.8f, \"min_lat\": %.8f, \"max_lon\": %.8f, \"max_lat\": %.8f},\n",
                bounds.min_lon, bounds.min_lat, bounds.max_lon, bounds.max_lat);
    std::printf("  \"raster\": {\"w\": %d, \"h\": %d, \"pixels\": %zu, \"sigma_r\": %.3f},\n",
                cfg.raster_w, cfg.raster_h, (size_t)cfg.raster_w * (size_t)cfg.raster_h, cfg.sigma_r);
    std::printf("  \"load_ms\": %.3f,\n", load_ms);
    std::printf("  \"gpu_ms\": {\"avg\": %.3f, \"min\": %.3f, \"max\": %.3f},\n", avg(gpu_ms), minv(gpu_ms), maxv(gpu_ms));
    std::printf("  \"cpu_blur_ms\": {\"avg\": %.3f, \"min\": %.3f, \"max\": %.3f},\n", avg(blur_ms), minv(blur_ms), maxv(blur_ms));
    std::printf("  \"total_aggregate_ms\": {\"avg\": %.3f},\n", avg(gpu_ms) + avg(blur_ms));
    std::printf("  \"last_density\": {\"sum\": %.3f, \"max\": %.3f}\n", last_sum_density, last_max_density);
    std::printf("}\n");

    CleanupVulkan();
    return 0;
}

static BenchConfig parse_args(int argc, char** argv) {
    BenchConfig cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto need = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--howard") {
        } else if (arg == "--input") {
            cfg.input = need("--input");
        } else if (arg == "--jurisdiction") {
            cfg.jurisdiction = need("--jurisdiction");
        } else if (arg == "--raster") {
            cfg.raster_w = cfg.raster_h = std::stoi(need("--raster"));
        } else if (arg == "--width") {
            cfg.raster_w = std::stoi(need("--width"));
        } else if (arg == "--height") {
            cfg.raster_h = std::stoi(need("--height"));
        } else if (arg == "--repeats") {
            cfg.repeats = std::max(1, std::stoi(need("--repeats")));
        } else if (arg == "--sigma") {
            cfg.sigma_r = std::max(0.1f, std::stof(need("--sigma")));
        } else if (arg == "--no-cpu-blur") {
            cfg.include_cpu_blur = false;
        } else if (arg == "--help" || arg == "-h") {
            std::printf("Usage: worldsim3_gpu_aggregate_harness [--howard] [--input PATH] [--jurisdiction NAME] [--raster N] [--repeats N] [--sigma N] [--no-cpu-blur]\n");
            std::exit(0);
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            std::exit(2);
        }
    }
    return cfg;
}

void SetupVulkan(const char**, uint32_t) { setup_vulkan_minimal(); }

void CleanupVulkan() {
    if (g_Device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(g_Device);
        shutdownGpuSplatAggregate();
        vkDestroyDevice(g_Device, g_Allocator);
        g_Device = VK_NULL_HANDLE;
    }
    if (g_Instance != VK_NULL_HANDLE) {
        vkDestroyInstance(g_Instance, g_Allocator);
        g_Instance = VK_NULL_HANDLE;
    }
}

int main(int argc, char** argv) {
#if defined(__linux__)
    setenv("WS3_ENABLE_EXPERIMENTAL_GPU_AGGREGATE", "1", 1);
#endif

    if (argc > 1) {
        return run_howard_bench(parse_args(argc, argv));
    }

    setup_vulkan_minimal();

    constexpr int rw = 512;
    constexpr int rh = 512;
    std::vector<HeatSample> samples;
    samples.reserve(20000);
    for (int i = 0; i < 20000; ++i) {
        const float t = (float)i / 20000.0f;
        HeatSample s;
        s.lon = -76.8f + 0.4f * t;
        s.lat = 39.1f + 0.35f * std::fabs(std::sin(t * 31.4159f));
        s.color = ImVec4(0.2f + 0.8f * t, 0.3f, 0.9f - 0.6f * t, 1.0f);
        s.prefer_gradient = true;
        s.algo = 2;
        samples.push_back(s);
    }

    std::vector<float> density, cr, cg, cb, cw, gv, sv;
    std::string err;
    const bool ok = buildGpuSplatAggregate(
        samples,
        rw,
        rh,
        -76.9f,
        39.0f,
        -76.3f,
        39.6f,
        6.0f,
        density,
        cr,
        cg,
        cb,
        cw,
        gv,
        sv,
        &err);

    if (!ok) {
        std::fprintf(stderr, "[harness] GPU aggregate FAILED: %s\n", err.c_str());
        CleanupVulkan();
        return 2;
    }

    double sum_density = 0.0;
    double max_density = 0.0;
    for (float d : density) {
        sum_density += d;
        max_density = std::max(max_density, (double)d);
    }
    std::printf("[harness] GPU aggregate OK\n");
    std::printf("[harness] output pixels=%zu sum_density=%.3f max_density=%.3f\n", density.size(), sum_density, max_density);

    CleanupVulkan();
    return 0;
}
