#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_vulkan.h"
#include "layer_state_io.h"
#include "feature_props.h"
#include "geo.h"
#include "cache_io.h"

#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <nlohmann/json.hpp>
#include <curl/curl.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <list>
#include <optional>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>
#include <functional>
#include <array>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <deque>
#include <sstream>
#include <iomanip>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include "earcut.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

struct HydratedLayer {
    size_t index = 0;
    std::vector<LayerDef::FeatureGeom> features;
    bool done = false;
    bool failed = false;
    std::string error;
};
struct TriJob {
    size_t index = 0;
    std::string file;
    std::vector<std::vector<std::vector<ImVec2>>> rings_per_feature;
};
struct TriResult {
    size_t index = 0;
    std::vector<std::vector<uint32_t>> triangles_per_feature;
    bool ok = true;
    std::string error;
};

enum class LayerPipelineStatus {
    Queued,
    Hydrating,
    Hydrated,
    TriQueued,
    Triangulating,
    Ready,
    Failed
};

struct LayerRuntimeState {
    LayerPipelineStatus status = LayerPipelineStatus::Queued;
    size_t feature_count = 0;
    std::string error;
};

struct LayerSpatialIndex {
    bool built = false;
    size_t feature_count_built = 0;
    float min_lon = 0.0f;
    float min_lat = 0.0f;
    float max_lon = 0.0f;
    float max_lat = 0.0f;
    int nx = 0;
    int ny = 0;
    std::vector<std::vector<uint32_t>> cells;
    std::vector<uint32_t> marks;
    uint32_t mark_id = 1;
};

struct LayerGeometryCache {
    int zoom = -1;
    size_t feature_count = 0;
    std::unordered_map<uint32_t, std::vector<std::vector<ImVec2>>> world_rings_by_feature;
};

static std::vector<LayerDef::FeatureGeom> loadLayerPointsFromFile(const fs::path& full_path);

struct BootstrapProgress {
    std::atomic<bool> running{false};
    std::atomic<bool> done{false};
    std::atomic<bool> failed{false};
    std::atomic<int> phase{0}; // 0=init,1=layers,2=tiles,3=done
    std::atomic<size_t> done_items{0};
    std::atomic<size_t> total_items{0};
    std::string status;
    std::string error;
    std::mutex msg_mutex;
};

static const char* statusToString(LayerPipelineStatus s) {
    switch (s) {
        case LayerPipelineStatus::Queued: return "queued";
        case LayerPipelineStatus::Hydrating: return "hydrating";
        case LayerPipelineStatus::Hydrated: return "hydrated";
        case LayerPipelineStatus::TriQueued: return "tri_queued";
        case LayerPipelineStatus::Triangulating: return "triangulating";
        case LayerPipelineStatus::Ready: return "ready";
        case LayerPipelineStatus::Failed: return "failed";
    }
    return "unknown";
}

static void buildLayerSpatialIndex(const LayerDef& layer, LayerSpatialIndex& si) {
    si = LayerSpatialIndex{};
    const size_t n = layer.features.size();
    if (n == 0) return;
    float min_lon = layer.features[0].extent.min_lon;
    float min_lat = layer.features[0].extent.min_lat;
    float max_lon = layer.features[0].extent.max_lon;
    float max_lat = layer.features[0].extent.max_lat;
    for (const auto& fg : layer.features) {
        min_lon = std::min(min_lon, fg.extent.min_lon);
        min_lat = std::min(min_lat, fg.extent.min_lat);
        max_lon = std::max(max_lon, fg.extent.max_lon);
        max_lat = std::max(max_lat, fg.extent.max_lat);
    }
    si.min_lon = min_lon;
    si.min_lat = min_lat;
    si.max_lon = max_lon;
    si.max_lat = max_lat;
    const double span_lon = std::max(1e-6, (double)(max_lon - min_lon));
    const double span_lat = std::max(1e-6, (double)(max_lat - min_lat));
    const double approx = std::sqrt((double)n / 14.0);
    si.nx = std::clamp((int)approx, 24, 140);
    si.ny = std::clamp((int)approx, 24, 140);
    si.cells.resize((size_t)si.nx * (size_t)si.ny);
    si.marks.assign(n, 0u);

    auto cell_x = [&](float lon) {
        int x = (int)(((double)(lon - min_lon) / span_lon) * si.nx);
        return std::clamp(x, 0, si.nx - 1);
    };
    auto cell_y = [&](float lat) {
        int y = (int)(((double)(lat - min_lat) / span_lat) * si.ny);
        return std::clamp(y, 0, si.ny - 1);
    };

    for (size_t i = 0; i < n; ++i) {
        const auto& ex = layer.features[i].extent;
        const int x0 = cell_x(ex.min_lon);
        const int x1 = cell_x(ex.max_lon);
        const int y0 = cell_y(ex.min_lat);
        const int y1 = cell_y(ex.max_lat);
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                si.cells[(size_t)y * (size_t)si.nx + (size_t)x].push_back((uint32_t)i);
            }
        }
    }
    si.feature_count_built = n;
    si.built = true;
}

static bool queryLayerSpatialIndex(
    LayerSpatialIndex& si,
    float q_min_lon, float q_min_lat, float q_max_lon, float q_max_lat,
    std::vector<uint32_t>& out) {
    if (!si.built || si.nx <= 0 || si.ny <= 0) return false;
    if (q_max_lon < si.min_lon || q_min_lon > si.max_lon || q_max_lat < si.min_lat || q_min_lat > si.max_lat) {
        out.clear();
        return true;
    }
    const double span_lon = std::max(1e-6, (double)(si.max_lon - si.min_lon));
    const double span_lat = std::max(1e-6, (double)(si.max_lat - si.min_lat));
    auto cell_x = [&](float lon) {
        int x = (int)(((double)(lon - si.min_lon) / span_lon) * si.nx);
        return std::clamp(x, 0, si.nx - 1);
    };
    auto cell_y = [&](float lat) {
        int y = (int)(((double)(lat - si.min_lat) / span_lat) * si.ny);
        return std::clamp(y, 0, si.ny - 1);
    };

    int x0 = cell_x(q_min_lon), x1 = cell_x(q_max_lon);
    int y0 = cell_y(q_min_lat), y1 = cell_y(q_max_lat);
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    if (++si.mark_id == 0) {
        std::fill(si.marks.begin(), si.marks.end(), 0u);
        si.mark_id = 1;
    }
    out.clear();
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const auto& cell = si.cells[(size_t)y * (size_t)si.nx + (size_t)x];
            for (uint32_t idx : cell) {
                if (idx >= si.marks.size()) continue;
                if (si.marks[idx] == si.mark_id) continue;
                si.marks[idx] = si.mark_id;
                out.push_back(idx);
            }
        }
    }
    return true;
}

static void saveDerivedVacancyStatus(
    const fs::path& out_path,
    const std::vector<LayerDef::FeatureGeom>& parcel_features,
    const std::vector<int>& notice_counts,
    const std::vector<int>& rehab_counts,
    size_t vacant_notice_rows_total,
    size_t vacant_rehab_rows_total,
    size_t vacant_notice_rows_matched,
    size_t vacant_rehab_rows_matched) {
    if (parcel_features.size() != notice_counts.size() || parcel_features.size() != rehab_counts.size()) return;
    json j;
    j["schema_version"] = 1;
    j["parcel_feature_count"] = parcel_features.size();
    j["vacant_notice_rows_total"] = vacant_notice_rows_total;
    j["vacant_rehab_rows_total"] = vacant_rehab_rows_total;
    j["vacant_notice_rows_matched"] = vacant_notice_rows_matched;
    j["vacant_rehab_rows_matched"] = vacant_rehab_rows_matched;
    j["vacant_notice_rows_unmatched"] = vacant_notice_rows_total - std::min(vacant_notice_rows_total, vacant_notice_rows_matched);
    j["vacant_rehab_rows_unmatched"] = vacant_rehab_rows_total - std::min(vacant_rehab_rows_total, vacant_rehab_rows_matched);
    j["entries"] = json::array();
    auto local_norm = [](const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char ch : s) {
            unsigned char u = (unsigned char)ch;
            if (std::isalnum(u)) out.push_back((char)std::toupper(u));
        }
        return out;
    };
    auto local_prop = [](const LayerDef::FeatureGeom& fg, const char* key) {
        for (const auto& kv : fg.properties) {
            if (kv.first == key) return kv.second;
        }
        return std::string{};
    };

    for (size_t i = 0; i < parcel_features.size(); ++i) {
        int n = notice_counts[i];
        int r = rehab_counts[i];
        int w = n + r;
        if (w <= 0) continue;
        std::string bl = local_norm(local_prop(parcel_features[i], "BLOCKLOT"));
        j["entries"].push_back({
            {"feature_index", i},
            {"blocklot_norm", bl},
            {"vacant_notice_count", n},
            {"vacant_rehab_count", r},
            {"vacancy_weight", w}
        });
    }
    fs::create_directories(out_path.parent_path());
    std::ofstream out(out_path);
    if (out) out << j.dump();
}

static const char* categoryToString(LayerDef::Category c) {
    switch (c) {
        case LayerDef::Category::Housing: return "housing";
        case LayerDef::Category::Infrastructure: return "infrastructure";
    }
    return "unknown";
}

static bool writeAll(int fd, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, data + sent, len - sent);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

static size_t curlWriteToFile(void* ptr, size_t size, size_t nmemb, void* userdata) {
    FILE* fp = (FILE*)userdata;
    return std::fwrite(ptr, size, nmemb, fp);
}

static bool downloadUrlToFile(const std::string& url, const fs::path& out_path, std::string& err) {
    fs::create_directories(out_path.parent_path());
    fs::path tmp = out_path;
    tmp += ".part";
    FILE* fp = std::fopen(tmp.string().c_str(), "wb");
    if (!fp) {
        err = "failed to open output file";
        return false;
    }
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::fclose(fp);
        err = "curl init failed";
        return false;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "BaltimoreVulkanMap/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteToFile);
    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);
    std::fclose(fp);
    if (rc != CURLE_OK || code < 200 || code >= 300) {
        std::error_code ec;
        fs::remove(tmp, ec);
        err = std::string("http failed: ") + curl_easy_strerror(rc) + " code=" + std::to_string(code);
        return false;
    }
    std::error_code ec;
    fs::rename(tmp, out_path, ec);
    if (ec) {
        err = "rename failed: " + ec.message();
        return false;
    }
    return true;
}

static std::vector<json> readLayerManifestEntries(const fs::path& root) {
    std::ifstream in(root / "layers_manifest.json");
    if (!in) in.open(root / "scripts" / "layers_manifest.json");
    if (!in) return {};
    json arr;
    in >> arr;
    std::vector<json> out;
    out.reserve(arr.size());
    for (const auto& e : arr) out.push_back(e);
    return out;
}

static bool loadAppSettingsValidation(const fs::path& root, bool default_value) {
    std::ifstream in(root / "data" / "app_settings.json");
    if (!in) return default_value;
    json j;
    try {
        in >> j;
    } catch (...) {
        return default_value;
    }
    if (j.contains("vulkan_validation_enabled") && j["vulkan_validation_enabled"].is_boolean()) {
        return j["vulkan_validation_enabled"].get<bool>();
    }
    return default_value;
}

static void saveAppSettings(const fs::path& root, bool validation_enabled) {
    fs::create_directories(root / "data");
    json j;
    j["vulkan_validation_enabled"] = validation_enabled;
    std::ofstream out(root / "data" / "app_settings.json");
    if (out) out << j.dump(2);
}

static std::pair<int, int> deg2num(double lat_deg, double lon_deg, int zoom) {
    double lat_rad = lat_deg * M_PI / 180.0;
    double n = std::pow(2.0, zoom);
    int xtile = (int)((lon_deg + 180.0) / 360.0 * n);
    int ytile = (int)((1.0 - std::asinh(std::tan(lat_rad)) / M_PI) / 2.0 * n);
    return {xtile, ytile};
}

static void setBootstrapStatus(BootstrapProgress& bp, const std::string& s) {
    std::lock_guard<std::mutex> lk(bp.msg_mutex);
    bp.status = s;
}

static std::string jsonValueToString(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number_unsigned()) return std::to_string(v.get<unsigned long long>());
    if (v.is_number_float()) return std::to_string(v.get<double>());
    if (v.is_null()) return "";
    return v.dump();
}

static void openUrlInBrowser(const std::string& url) {
#if defined(_WIN32)
    std::string cmd = "start \"\" \"" + url + "\"";
#elif defined(__APPLE__)
    std::string cmd = "open \"" + url + "\"";
#else
    std::string cmd = "xdg-open \"" + url + "\"";
#endif
    int rc = std::system(cmd.c_str());
    (void)rc;
}

static int runVacancySelftest(const fs::path& root) {
    const fs::path parcel_path = root / "data" / "layers" / "parcel.geojson";
    const fs::path notice_path = root / "data" / "layers" / "vacant_building_notices.geojson";
    const fs::path rehab_path = root / "data" / "layers" / "vacant_building_rehabs.geojson";

    json out;
    out["mode"] = "vacancy-selftest";
    out["root"] = root.string();
    out["paths"] = {
        {"parcel", parcel_path.string()},
        {"vacant_notices", notice_path.string()},
        {"vacant_rehabs", rehab_path.string()}
    };

    if (!fs::exists(parcel_path) || !fs::exists(notice_path) || !fs::exists(rehab_path)) {
        out["ok"] = false;
        out["error"] = "required layer file missing";
        out["exists"] = {
            {"parcel", fs::exists(parcel_path)},
            {"vacant_notices", fs::exists(notice_path)},
            {"vacant_rehabs", fs::exists(rehab_path)}
        };
        std::printf("%s\n", out.dump(2).c_str());
        return 2;
    }

    auto parcels = loadLayerPointsFromFile(parcel_path);
    auto notices = loadLayerPointsFromFile(notice_path);
    auto rehabs = loadLayerPointsFromFile(rehab_path);

    std::unordered_map<std::string, std::vector<size_t>> parcel_by_blocklot;
    parcel_by_blocklot.reserve(parcels.size());
    for (size_t i = 0; i < parcels.size(); ++i) {
        std::string bl = normalizeJoinKey(getPropertyValue(parcels[i], "BLOCKLOT"));
        if (!bl.empty()) parcel_by_blocklot[bl].push_back(i);
    }

    std::vector<int> notice_by_parcel(parcels.size(), 0);
    std::vector<int> rehab_by_parcel(parcels.size(), 0);
    size_t notice_rows_matched = 0, notice_rows_unmatched = 0, notice_rows_missing_key = 0;
    size_t rehab_rows_matched = 0, rehab_rows_unmatched = 0, rehab_rows_missing_key = 0;

    for (const auto& fg : notices) {
        std::string bl = normalizeJoinKey(getPropertyValue(fg, "BLOCKLOT"));
        if (bl.empty()) {
            notice_rows_missing_key++;
            continue;
        }
        auto it = parcel_by_blocklot.find(bl);
        if (it == parcel_by_blocklot.end()) {
            notice_rows_unmatched++;
            continue;
        }
        notice_rows_matched++;
        for (size_t idx : it->second) notice_by_parcel[idx] += 1;
    }

    for (const auto& fg : rehabs) {
        std::string bl = normalizeJoinKey(getPropertyValue(fg, "BLOCKLOT"));
        if (bl.empty()) {
            rehab_rows_missing_key++;
            continue;
        }
        auto it = parcel_by_blocklot.find(bl);
        if (it == parcel_by_blocklot.end()) {
            rehab_rows_unmatched++;
            continue;
        }
        rehab_rows_matched++;
        for (size_t idx : it->second) rehab_by_parcel[idx] += 1;
    }

    size_t parcels_with_notice = 0, parcels_with_rehab = 0, parcels_with_any = 0;
    size_t parcels_with_geometry = 0, parcels_with_polygon_rings = 0, parcels_with_nonzero_extent = 0;
    size_t matched_parcels_with_polygon_geometry = 0;
    int max_notice = 0, max_rehab = 0, max_weight = 0;
    for (size_t i = 0; i < parcels.size(); ++i) {
        int n = notice_by_parcel[i];
        int r = rehab_by_parcel[i];
        int w = n + r;
        const auto& ex = parcels[i].extent;
        const bool nonzero_extent = (ex.max_lon > ex.min_lon) && (ex.max_lat > ex.min_lat);
        const bool has_poly = !parcels[i].rings.empty();
        const bool has_geom = has_poly || nonzero_extent;
        if (has_geom) parcels_with_geometry++;
        if (has_poly) parcels_with_polygon_rings++;
        if (nonzero_extent) parcels_with_nonzero_extent++;
        if (n > 0) parcels_with_notice++;
        if (r > 0) parcels_with_rehab++;
        if (w > 0) parcels_with_any++;
        if (w > 0 && has_poly) matched_parcels_with_polygon_geometry++;
        max_notice = std::max(max_notice, n);
        max_rehab = std::max(max_rehab, r);
        max_weight = std::max(max_weight, w);
    }

    json counts = json::object();
    counts["parcel_features"] = parcels.size();
    counts["vacant_notice_rows"] = notices.size();
    counts["vacant_rehab_rows"] = rehabs.size();
    counts["parcel_unique_blocklot_keys"] = parcel_by_blocklot.size();
    counts["notice_rows_matched"] = notice_rows_matched;
    counts["notice_rows_unmatched"] = notice_rows_unmatched;
    counts["notice_rows_missing_key"] = notice_rows_missing_key;
    counts["rehab_rows_matched"] = rehab_rows_matched;
    counts["rehab_rows_unmatched"] = rehab_rows_unmatched;
    counts["rehab_rows_missing_key"] = rehab_rows_missing_key;
    counts["parcels_with_notice"] = parcels_with_notice;
    counts["parcels_with_rehab"] = parcels_with_rehab;
    counts["parcels_with_any_vacancy"] = parcels_with_any;
    counts["parcels_with_geometry"] = parcels_with_geometry;
    counts["parcels_with_polygon_rings"] = parcels_with_polygon_rings;
    counts["parcels_with_nonzero_extent"] = parcels_with_nonzero_extent;
    counts["matched_parcels_with_polygon_geometry"] = matched_parcels_with_polygon_geometry;
    counts["geometry_attainable"] = (matched_parcels_with_polygon_geometry > 0);
    counts["max_notice_count_on_parcel"] = max_notice;
    counts["max_rehab_count_on_parcel"] = max_rehab;
    counts["max_total_vacancy_weight_on_parcel"] = max_weight;
    out["counts"] = std::move(counts);
    out["ok"] = (parcels_with_any > 0) &&
                (notice_rows_matched + rehab_rows_matched > 0) &&
                (matched_parcels_with_polygon_geometry > 0);
    std::printf("%s\n", out.dump(2).c_str());
    return out["ok"].get<bool>() ? 0 : 3;
}

struct TileTexture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSet descriptor = VK_NULL_HANDLE;
    uint32_t mip_levels = 1;
};

struct TileCacheEntry {
    TileTexture tex;
    std::list<std::string>::iterator lru_it;
};

static constexpr int kMinZoom = 11;
static constexpr int kMaxZoom = 64;
static constexpr int kMaxNativeTileZoom = 18;
static constexpr int kMaxInternalMathZoom = 22;
static constexpr size_t kMaxTileCache = 320;

static VkAllocationCallbacks* g_Allocator = nullptr;
static VkInstance g_Instance = VK_NULL_HANDLE;
static VkPhysicalDevice g_PhysicalDevice = VK_NULL_HANDLE;
static VkDevice g_Device = VK_NULL_HANDLE;
static uint32_t g_QueueFamily = (uint32_t)-1;
static VkQueue g_Queue = VK_NULL_HANDLE;
static VkDescriptorPool g_DescriptorPool = VK_NULL_HANDLE;
static VkSampler g_TileSampler = VK_NULL_HANDLE;
static VkCommandPool g_UploadCommandPool = VK_NULL_HANDLE;
static VkCommandBuffer g_UploadCommandBuffer = VK_NULL_HANDLE;
static VkDebugUtilsMessengerEXT g_DebugUtilsMessenger = VK_NULL_HANDLE;

static ImGui_ImplVulkanH_Window g_MainWindowData;
static int g_MinImageCount = 2;
static bool g_SwapChainRebuild = false;

static std::unordered_map<std::string, TileCacheEntry> g_TileCache;
static std::list<std::string> g_TileLRU;
static bool g_EnableValidationLayers = false;

static void check_vk_result(VkResult err) {
    if (err == 0) return;
    std::fprintf(stderr, "[vulkan] VkResult=%d\n", err);
    if (err < 0) std::abort();
}

static VKAPI_ATTR VkBool32 VKAPI_CALL DebugUtilsCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT* callback_data,
    void* user_data) {
    (void)severity;
    (void)type;
    (void)user_data;
    std::fprintf(stderr, "[vulkan validation] %s\n", callback_data->pMessage);
    return VK_FALSE;
}

static bool IsInstanceLayerAvailable(const char* layer_name) {
    uint32_t layer_count = 0;
    vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
    std::vector<VkLayerProperties> layers(layer_count);
    vkEnumerateInstanceLayerProperties(&layer_count, layers.data());
    for (const auto& layer : layers) {
        if (std::strcmp(layer.layerName, layer_name) == 0) return true;
    }
    return false;
}

static bool IsInstanceExtensionAvailable(const char* ext_name) {
    uint32_t ext_count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> exts(ext_count);
    vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, exts.data());
    for (const auto& ext : exts) {
        if (std::strcmp(ext.extensionName, ext_name) == 0) return true;
    }
    return false;
}

static void CreateDebugUtilsMessenger() {
    if (!g_EnableValidationLayers) return;
    auto create_fn = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_Instance, "vkCreateDebugUtilsMessengerEXT");
    if (!create_fn) return;
    VkDebugUtilsMessengerCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = DebugUtilsCallback;
    check_vk_result(create_fn(g_Instance, &info, g_Allocator, &g_DebugUtilsMessenger));
}

static uint32_t findMemoryType(uint32_t type_filter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++) {
        if ((type_filter & (1u << i)) && (mem.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    std::abort();
}

static void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& memory) {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check_vk_result(vkCreateBuffer(g_Device, &info, g_Allocator, &buffer));

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_Device, buffer, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = findMemoryType(req.memoryTypeBits, properties);
    check_vk_result(vkAllocateMemory(g_Device, &alloc, g_Allocator, &memory));
    check_vk_result(vkBindBufferMemory(g_Device, buffer, memory, 0));
}

static uint32_t calcMipLevels(uint32_t w, uint32_t h) {
    uint32_t levels = 1;
    while (w > 1 || h > 1) {
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
        levels++;
    }
    return levels;
}

static void createImage(uint32_t width, uint32_t height, uint32_t mip_levels, VkImage& image, VkDeviceMemory& memory) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.extent = {width, height, 1};
    info.mipLevels = mip_levels;
    info.arrayLayers = 1;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check_vk_result(vkCreateImage(g_Device, &info, g_Allocator, &image));

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g_Device, image, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check_vk_result(vkAllocateMemory(g_Device, &alloc, g_Allocator, &memory));
    check_vk_result(vkBindImageMemory(g_Device, image, memory, 0));
}

static VkImageView createImageView(VkImage image, uint32_t mip_levels) {
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    info.subresourceRange.levelCount = mip_levels;
    info.subresourceRange.layerCount = 1;
    VkImageView out = VK_NULL_HANDLE;
    check_vk_result(vkCreateImageView(g_Device, &info, g_Allocator, &out));
    return out;
}

static void transitionImage(VkCommandBuffer cmd, VkImage image, uint32_t base_level, uint32_t level_count, VkImageLayout old_layout, VkImageLayout new_layout) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = base_level;
    barrier.subresourceRange.levelCount = level_count;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags src_stage;
    VkPipelineStageFlags dst_stage;
    if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED && new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

static void submitUploadCommands(std::function<void(VkCommandBuffer)> record) {
    check_vk_result(vkResetCommandPool(g_Device, g_UploadCommandPool, 0));

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk_result(vkBeginCommandBuffer(g_UploadCommandBuffer, &begin));
    record(g_UploadCommandBuffer);
    check_vk_result(vkEndCommandBuffer(g_UploadCommandBuffer));

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &g_UploadCommandBuffer;
    check_vk_result(vkQueueSubmit(g_Queue, 1, &submit, VK_NULL_HANDLE));
    check_vk_result(vkQueueWaitIdle(g_Queue));
}

static void destroyTileTexture(TileTexture& tex) {
    if (tex.descriptor) ImGui_ImplVulkan_RemoveTexture(tex.descriptor);
    if (tex.view) vkDestroyImageView(g_Device, tex.view, g_Allocator);
    if (tex.image) vkDestroyImage(g_Device, tex.image, g_Allocator);
    if (tex.memory) vkFreeMemory(g_Device, tex.memory, g_Allocator);
    tex = {};
}

static void touchLRU(const std::string& key) {
    auto it = g_TileCache.find(key);
    if (it == g_TileCache.end()) return;
    g_TileLRU.erase(it->second.lru_it);
    g_TileLRU.push_front(key);
    it->second.lru_it = g_TileLRU.begin();
}

static void evictIfNeeded() {
    while (g_TileCache.size() > kMaxTileCache) {
        const std::string key = g_TileLRU.back();
        g_TileLRU.pop_back();
        auto it = g_TileCache.find(key);
        if (it != g_TileCache.end()) {
            destroyTileTexture(it->second.tex);
            g_TileCache.erase(it);
        }
    }
}

static bool loadTileTexture(const fs::path& tile_path, const std::string& key) {
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load(tile_path.string().c_str(), &w, &h, &channels, STBI_rgb_alpha);
    if (!pixels) return false;

    const uint32_t mip_levels = calcMipLevels((uint32_t)w, (uint32_t)h);
    std::vector<std::vector<unsigned char>> mip_data;
    mip_data.reserve(mip_levels);
    mip_data.emplace_back((size_t)w * (size_t)h * 4);
    std::memcpy(mip_data[0].data(), pixels, mip_data[0].size());
    stbi_image_free(pixels);

    uint32_t mw = (uint32_t)w;
    uint32_t mh = (uint32_t)h;
    for (uint32_t m = 1; m < mip_levels; ++m) {
        uint32_t nw = std::max(1u, mw / 2);
        uint32_t nh = std::max(1u, mh / 2);
        std::vector<unsigned char> out((size_t)nw * (size_t)nh * 4);
        for (uint32_t y = 0; y < nh; ++y) {
            for (uint32_t x = 0; x < nw; ++x) {
                int acc[4] = {0, 0, 0, 0};
                for (uint32_t ky = 0; ky < 2; ++ky) {
                    for (uint32_t kx = 0; kx < 2; ++kx) {
                        uint32_t sx = std::min(mw - 1, x * 2 + kx);
                        uint32_t sy = std::min(mh - 1, y * 2 + ky);
                        size_t si = ((size_t)sy * mw + sx) * 4;
                        acc[0] += mip_data[m - 1][si + 0];
                        acc[1] += mip_data[m - 1][si + 1];
                        acc[2] += mip_data[m - 1][si + 2];
                        acc[3] += mip_data[m - 1][si + 3];
                    }
                }
                size_t di = ((size_t)y * nw + x) * 4;
                out[di + 0] = (unsigned char)(acc[0] / 4);
                out[di + 1] = (unsigned char)(acc[1] / 4);
                out[di + 2] = (unsigned char)(acc[2] / 4);
                out[di + 3] = (unsigned char)(acc[3] / 4);
            }
        }
        mip_data.emplace_back(std::move(out));
        mw = nw;
        mh = nh;
    }

    VkDeviceSize size = 0;
    for (const auto& m : mip_data) size += (VkDeviceSize)m.size();
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging, staging_mem);

    void* mapped = nullptr;
    check_vk_result(vkMapMemory(g_Device, staging_mem, 0, size, 0, &mapped));
    unsigned char* dst = static_cast<unsigned char*>(mapped);
    std::vector<VkBufferImageCopy> regions;
    regions.reserve(mip_levels);
    VkDeviceSize offset = 0;
    mw = (uint32_t)w;
    mh = (uint32_t)h;
    for (uint32_t m = 0; m < mip_levels; ++m) {
        std::memcpy(dst + offset, mip_data[m].data(), mip_data[m].size());
        VkBufferImageCopy region{};
        region.bufferOffset = offset;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = m;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {mw, mh, 1};
        regions.push_back(region);
        offset += (VkDeviceSize)mip_data[m].size();
        mw = std::max(1u, mw / 2);
        mh = std::max(1u, mh / 2);
    }
    vkUnmapMemory(g_Device, staging_mem);

    TileTexture tex;
    tex.mip_levels = mip_levels;
    createImage((uint32_t)w, (uint32_t)h, mip_levels, tex.image, tex.memory);

    submitUploadCommands([&](VkCommandBuffer cmd) {
        transitionImage(cmd, tex.image, 0, mip_levels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyBufferToImage(cmd, staging, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)regions.size(), regions.data());
        transitionImage(cmd, tex.image, 0, mip_levels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });

    vkDestroyBuffer(g_Device, staging, g_Allocator);
    vkFreeMemory(g_Device, staging_mem, g_Allocator);

    tex.view = createImageView(tex.image, mip_levels);
    tex.descriptor = ImGui_ImplVulkan_AddTexture(g_TileSampler, tex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    g_TileLRU.push_front(key);
    g_TileCache.emplace(key, TileCacheEntry{tex, g_TileLRU.begin()});
    evictIfNeeded();
    return true;
}

static TileTexture* getTileTexture(const fs::path& root, int z, int x, int y) {
    const std::string key = std::to_string(z) + "/" + std::to_string(x) + "/" + std::to_string(y);
    auto it = g_TileCache.find(key);
    if (it != g_TileCache.end()) {
        touchLRU(key);
        return &it->second.tex;
    }

    const fs::path tile_path = root / "data" / "tiles" / std::to_string(z) / std::to_string(x) / (std::to_string(y) + ".png");
    if (!fs::exists(tile_path)) return nullptr;
    if (!loadTileTexture(tile_path, key)) return nullptr;

    auto loaded = g_TileCache.find(key);
    return loaded == g_TileCache.end() ? nullptr : &loaded->second.tex;
}

struct TileSample {
    TileTexture* tex = nullptr;
    ImVec2 uv0 = ImVec2(0.0f, 0.0f);
    ImVec2 uv1 = ImVec2(1.0f, 1.0f);
};

static TileSample getTileSample(const fs::path& root, int z, int x, int y) {
    if (z <= kMaxNativeTileZoom) {
        return TileSample{getTileTexture(root, z, x, y), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f)};
    }

    const int dz = z - kMaxNativeTileZoom;
    const int scale = 1 << dz;
    const int parent_x = x / scale;
    const int parent_y = y / scale;
    const int ox = x % scale;
    const int oy = y % scale;

    TileTexture* parent = getTileTexture(root, kMaxNativeTileZoom, parent_x, parent_y);
    if (!parent) return {};

    const float step = 1.0f / (float)scale;
    ImVec2 uv0((float)ox * step, (float)oy * step);
    ImVec2 uv1(uv0.x + step, uv0.y + step);
    return TileSample{parent, uv0, uv1};
}

static std::vector<LayerDef::FeatureGeom> extractFeatureGeoms(const json& geom) {
    std::vector<LayerDef::FeatureGeom> out;
    if (!geom.contains("type") || !geom.contains("coordinates")) return out;
    const std::string t = geom["type"].get<std::string>();

    auto build_from_polygon = [](const json& poly_coords) -> std::optional<LayerDef::FeatureGeom> {
        LayerDef::FeatureGeom fg{};
        bool has = false;
        auto expand = [&](double lon, double lat) {
            if (!has) {
                fg.extent.min_lon = fg.extent.max_lon = (float)lon;
                fg.extent.min_lat = fg.extent.max_lat = (float)lat;
                has = true;
                return;
            }
            fg.extent.min_lon = std::min(fg.extent.min_lon, (float)lon);
            fg.extent.min_lat = std::min(fg.extent.min_lat, (float)lat);
            fg.extent.max_lon = std::max(fg.extent.max_lon, (float)lon);
            fg.extent.max_lat = std::max(fg.extent.max_lat, (float)lat);
        };

        for (const auto& ring_json : poly_coords) {
            std::vector<ImVec2> ring;
            for (const auto& p : ring_json) {
                if (p.size() < 2) continue;
                double lon = p[0].get<double>();
                double lat = p[1].get<double>();
                ring.push_back(ImVec2((float)lon, (float)lat));
                expand(lon, lat);
            }
            if (ring.size() >= 3) fg.rings.push_back(std::move(ring));
        }
        if (!has || fg.rings.empty()) return std::nullopt;

        return fg;
    };

    if (t == "Polygon") {
        auto fg = build_from_polygon(geom["coordinates"]);
        if (fg) out.push_back(std::move(*fg));
    } else if (t == "MultiPolygon") {
        for (const auto& poly : geom["coordinates"]) {
            auto fg = build_from_polygon(poly);
            if (fg) out.push_back(std::move(*fg));
        }
    } else if (t == "Point") {
        const auto& c = geom["coordinates"];
        if (c.is_array() && c.size() >= 2) {
            LayerDef::FeatureGeom fg{};
            double lon = c[0].get<double>();
            double lat = c[1].get<double>();
            fg.extent.min_lon = fg.extent.max_lon = (float)lon;
            fg.extent.min_lat = fg.extent.max_lat = (float)lat;
            out.push_back(std::move(fg));
        }
    } else if (t == "MultiPoint") {
        for (const auto& c : geom["coordinates"]) {
            if (!c.is_array() || c.size() < 2) continue;
            LayerDef::FeatureGeom fg{};
            double lon = c[0].get<double>();
            double lat = c[1].get<double>();
            fg.extent.min_lon = fg.extent.max_lon = (float)lon;
            fg.extent.min_lat = fg.extent.max_lat = (float)lat;
            out.push_back(std::move(fg));
        }
    }
    return out;
}

static std::vector<LayerDef::FeatureGeom> loadLayerPointsFromFile(const fs::path& full_path) {
    std::vector<LayerDef::FeatureGeom> features;
    std::ifstream in(full_path);
    if (!in) return features;
    json j;
    in >> j;
    if (!j.contains("features")) return features;
    for (auto& f : j["features"]) {
        if (!f.contains("geometry")) continue;
        auto geoms = extractFeatureGeoms(f["geometry"]);
        std::vector<std::pair<std::string, std::string>> props;
        if (f.contains("properties") && f["properties"].is_object()) {
            props.reserve(f["properties"].size());
            for (auto it = f["properties"].begin(); it != f["properties"].end(); ++it) {
                props.push_back({it.key(), jsonValueToString(it.value())});
            }
        }
        for (auto& g : geoms) {
            g.properties = props;
            features.push_back(std::move(g));
        }
    }
    return features;
}

static bool pointInRing(const std::vector<ImVec2>& ring, float x, float y) {
    bool inside = false;
    const size_t n = ring.size();
    if (n < 3) return false;
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const float xi = ring[i].x, yi = ring[i].y;
        const float xj = ring[j].x, yj = ring[j].y;
        const bool intersect = ((yi > y) != (yj > y)) &&
                               (x < (xj - xi) * (y - yi) / ((yj - yi) == 0.0f ? 1e-9f : (yj - yi)) + xi);
        if (intersect) inside = !inside;
    }
    return inside;
}

static bool pointInFeature(const LayerDef::FeatureGeom& fg, float lon, float lat) {
    if (fg.rings.empty()) return false;
    if (!pointInRing(fg.rings[0], lon, lat)) return false;
    for (size_t i = 1; i < fg.rings.size(); ++i) {
        if (pointInRing(fg.rings[i], lon, lat)) return false;
    }
    return true;
}

static int lodRingStepForZoom(int zoom) {
    if (zoom <= 12) return 8;
    if (zoom == 13) return 6;
    if (zoom == 14) return 4;
    if (zoom == 15) return 3;
    if (zoom == 16) return 2;
    return 1;
}

static void hydrateLayerBatches(
    const fs::path& full_path,
    size_t batch_size,
    const std::atomic<bool>& stop_flag,
    const std::function<bool()>& should_continue,
    const std::function<void(std::vector<LayerDef::FeatureGeom>&&, bool, bool, const std::string&)>& emit) {
    std::ifstream in(full_path);
    if (!in) {
        emit({}, true, true, "failed to open layer file");
        return;
    }
    json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        emit({}, true, true, e.what());
        return;
    }
    if (!j.contains("features") || !j["features"].is_array()) {
        emit({}, true, true, "invalid geojson: missing features array");
        return;
    }

    std::vector<LayerDef::FeatureGeom> batch;
    batch.reserve(batch_size);
    for (auto& f : j["features"]) {
        if (stop_flag.load(std::memory_order_relaxed) || !should_continue()) return;
        if (!f.contains("geometry")) continue;
        std::vector<std::pair<std::string, std::string>> props;
        if (f.contains("properties") && f["properties"].is_object()) {
            props.reserve(f["properties"].size());
            for (auto it = f["properties"].begin(); it != f["properties"].end(); ++it) {
                if (it.value().is_null()) continue;
                if (it.value().is_string()) props.push_back({it.key(), it.value().get<std::string>()});
                else props.push_back({it.key(), it.value().dump()});
            }
        }
        auto geoms = extractFeatureGeoms(f["geometry"]);
        for (auto& g : geoms) {
            if (stop_flag.load(std::memory_order_relaxed) || !should_continue()) return;
            g.properties = props;
            batch.push_back(std::move(g));
            if (batch.size() >= batch_size) {
                emit(std::move(batch), false, false, "");
                batch.clear();
                batch.reserve(batch_size);
            }
        }
    }
    if (!batch.empty()) emit(std::move(batch), false, false, "");
    emit({}, true, false, "");
}

static void loadLayerPoints(LayerDef& layer, const fs::path& root) {
    layer.features = loadLayerPointsFromFile(root / "data" / "layers" / layer.file);
}

static std::vector<uint32_t> triangulateRings(const std::vector<std::vector<ImVec2>>& rings) {
    using Pt = std::array<double, 2>;
    std::vector<std::vector<Pt>> poly;
    poly.reserve(rings.size());
    for (const auto& ring : rings) {
        std::vector<Pt> rp;
        rp.reserve(ring.size());
        for (const ImVec2& p : ring) rp.push_back(Pt{(double)p.x, (double)p.y});
        poly.push_back(std::move(rp));
    }
    return mapbox::earcut<uint32_t>(poly);
}

static void appendRingScreenPointsLod(
    const std::vector<ImVec2>& ring,
    int ring_step,
    int math_zoom,
    const ImVec2& center_world,
    const ImVec2& origin,
    const ImVec2& size,
    double zoom_scale,
    std::vector<ImVec2>& out) {
    if (ring.empty()) return;
    const int step = std::max(1, ring_step);
    const size_t n = ring.size();
    if (step == 1 || n <= 4) {
        for (const ImVec2& ll : ring) {
            ImVec2 pw = lonLatToWorldPx(ll.x, ll.y, math_zoom);
            out.push_back(worldToScreen(pw, center_world, origin, size, zoom_scale));
        }
        return;
    }
    out.reserve(out.size() + (n / (size_t)step) + 2);
    for (size_t i = 0; i < n; i += (size_t)step) {
        ImVec2 pw = lonLatToWorldPx(ring[i].x, ring[i].y, math_zoom);
        out.push_back(worldToScreen(pw, center_world, origin, size, zoom_scale));
    }
    if ((n - 1) % (size_t)step != 0) {
        ImVec2 pw = lonLatToWorldPx(ring.back().x, ring.back().y, math_zoom);
        out.push_back(worldToScreen(pw, center_world, origin, size, zoom_scale));
    }
}

static void appendWorldRingScreenPointsLod(
    const std::vector<ImVec2>& world_ring,
    int ring_step,
    const ImVec2& center_world,
    const ImVec2& origin,
    const ImVec2& size,
    double zoom_scale,
    std::vector<ImVec2>& out) {
    if (world_ring.empty()) return;
    const int step = std::max(1, ring_step);
    const size_t n = world_ring.size();
    if (step == 1 || n <= 4) {
        for (const ImVec2& wp : world_ring) out.push_back(worldToScreen(wp, center_world, origin, size, zoom_scale));
        return;
    }
    out.reserve(out.size() + (n / (size_t)step) + 2);
    for (size_t i = 0; i < n; i += (size_t)step) out.push_back(worldToScreen(world_ring[i], center_world, origin, size, zoom_scale));
    if ((n - 1) % (size_t)step != 0) out.push_back(worldToScreen(world_ring.back(), center_world, origin, size, zoom_scale));
}

static void SetupVulkan(const char** extensions, uint32_t extensions_count) {
    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "BaltimoreVulkanMap";
    app_info.apiVersion = VK_API_VERSION_1_1;

    std::vector<const char*> enabled_extensions;
    enabled_extensions.reserve(extensions_count + 1);
    for (uint32_t i = 0; i < extensions_count; ++i) enabled_extensions.push_back(extensions[i]);

    std::vector<const char*> enabled_layers;
    if (g_EnableValidationLayers && IsInstanceLayerAvailable("VK_LAYER_KHRONOS_validation")) {
        enabled_layers.push_back("VK_LAYER_KHRONOS_validation");
        if (IsInstanceExtensionAvailable(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            enabled_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
    } else {
        g_EnableValidationLayers = false;
    }

    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;
    create_info.enabledExtensionCount = (uint32_t)enabled_extensions.size();
    create_info.ppEnabledExtensionNames = enabled_extensions.data();
    create_info.enabledLayerCount = (uint32_t)enabled_layers.size();
    create_info.ppEnabledLayerNames = enabled_layers.empty() ? nullptr : enabled_layers.data();
    check_vk_result(vkCreateInstance(&create_info, g_Allocator, &g_Instance));
    CreateDebugUtilsMessenger();

    uint32_t gpu_count = 0;
    check_vk_result(vkEnumeratePhysicalDevices(g_Instance, &gpu_count, nullptr));
    std::vector<VkPhysicalDevice> gpus(gpu_count);
    check_vk_result(vkEnumeratePhysicalDevices(g_Instance, &gpu_count, gpus.data()));
    g_PhysicalDevice = gpus[0];

    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_PhysicalDevice, &count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(count);
    vkGetPhysicalDeviceQueueFamilyProperties(g_PhysicalDevice, &count, queues.data());
    for (uint32_t i = 0; i < count; i++) {
        if (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            g_QueueFamily = i;
            break;
        }
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = g_QueueFamily;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;

    const char* device_extensions[] = {"VK_KHR_swapchain"};
    VkPhysicalDeviceFeatures available_features{};
    vkGetPhysicalDeviceFeatures(g_PhysicalDevice, &available_features);
    VkPhysicalDeviceFeatures enabled_features{};
    enabled_features.samplerAnisotropy = available_features.samplerAnisotropy;
    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = 1;
    device_info.ppEnabledExtensionNames = device_extensions;
    device_info.pEnabledFeatures = &enabled_features;
    check_vk_result(vkCreateDevice(g_PhysicalDevice, &device_info, g_Allocator, &g_Device));
    vkGetDeviceQueue(g_Device, g_QueueFamily, 0, &g_Queue);

    VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLER, 1024},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4096},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2048},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2048},
    };
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 8192;
    pool_info.poolSizeCount = (uint32_t)IM_ARRAYSIZE(pool_sizes);
    pool_info.pPoolSizes = pool_sizes;
    check_vk_result(vkCreateDescriptorPool(g_Device, &pool_info, g_Allocator, &g_DescriptorPool));

    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler.minLod = 0.0f;
    sampler.maxLod = 16.0f;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(g_PhysicalDevice, &props);
    sampler.anisotropyEnable = available_features.samplerAnisotropy ? VK_TRUE : VK_FALSE;
    sampler.maxAnisotropy = available_features.samplerAnisotropy ? std::min(8.0f, props.limits.maxSamplerAnisotropy) : 1.0f;
    check_vk_result(vkCreateSampler(g_Device, &sampler, g_Allocator, &g_TileSampler));

    VkCommandPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.queueFamilyIndex = g_QueueFamily;
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    check_vk_result(vkCreateCommandPool(g_Device, &pool, g_Allocator, &g_UploadCommandPool));

    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = g_UploadCommandPool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    check_vk_result(vkAllocateCommandBuffers(g_Device, &alloc, &g_UploadCommandBuffer));
}

static void SetupVulkanWindow(ImGui_ImplVulkanH_Window* wd, VkSurfaceKHR surface, int width, int height) {
    wd->Surface = surface;
    VkBool32 res;
    vkGetPhysicalDeviceSurfaceSupportKHR(g_PhysicalDevice, g_QueueFamily, wd->Surface, &res);
    if (res != VK_TRUE) std::abort();

    const VkFormat request_surface_image_format[] = {
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_B8G8R8_UNORM,
        VK_FORMAT_R8G8B8_UNORM,
    };
    const VkColorSpaceKHR request_surface_color_space = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
    wd->SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(
        g_PhysicalDevice, wd->Surface, request_surface_image_format,
        (size_t)IM_ARRAYSIZE(request_surface_image_format),
        request_surface_color_space);

    const VkPresentModeKHR present_modes[] = {
        VK_PRESENT_MODE_MAILBOX_KHR,
        VK_PRESENT_MODE_IMMEDIATE_KHR,
        VK_PRESENT_MODE_FIFO_KHR,
    };
    wd->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(
        g_PhysicalDevice, wd->Surface, &present_modes[0], IM_ARRAYSIZE(present_modes));

    ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, wd, g_QueueFamily, g_Allocator, width, height, g_MinImageCount);
}

static void CleanupTileCache() {
    for (auto& kv : g_TileCache) destroyTileTexture(kv.second.tex);
    g_TileCache.clear();
    g_TileLRU.clear();
}

static void CleanupVulkan() {
    CleanupTileCache();
    if (g_UploadCommandPool) vkDestroyCommandPool(g_Device, g_UploadCommandPool, g_Allocator);
    if (g_TileSampler) vkDestroySampler(g_Device, g_TileSampler, g_Allocator);
    vkDestroyDescriptorPool(g_Device, g_DescriptorPool, g_Allocator);
    vkDestroyDevice(g_Device, g_Allocator);
    if (g_DebugUtilsMessenger != VK_NULL_HANDLE) {
        auto destroy_fn = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_Instance, "vkDestroyDebugUtilsMessengerEXT");
        if (destroy_fn) destroy_fn(g_Instance, g_DebugUtilsMessenger, g_Allocator);
    }
    vkDestroyInstance(g_Instance, g_Allocator);
}

static void CleanupVulkanWindow() {
    ImGui_ImplVulkanH_DestroyWindow(g_Instance, g_Device, &g_MainWindowData, g_Allocator);
}

static void FrameRender(ImGui_ImplVulkanH_Window* wd, ImDrawData* draw_data) {
    VkSemaphore image_acquired_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    VkResult err = vkAcquireNextImageKHR(g_Device, wd->Swapchain, UINT64_MAX, image_acquired_semaphore, VK_NULL_HANDLE, &wd->FrameIndex);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
        g_SwapChainRebuild = true;
        return;
    }
    check_vk_result(err);

    ImGui_ImplVulkanH_Frame* fd = &wd->Frames[wd->FrameIndex];
    check_vk_result(vkWaitForFences(g_Device, 1, &fd->Fence, VK_TRUE, UINT64_MAX));
    check_vk_result(vkResetFences(g_Device, 1, &fd->Fence));
    check_vk_result(vkResetCommandPool(g_Device, fd->CommandPool, 0));

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk_result(vkBeginCommandBuffer(fd->CommandBuffer, &begin));

    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = wd->RenderPass;
    rp.framebuffer = fd->Framebuffer;
    rp.renderArea.extent.width = wd->Width;
    rp.renderArea.extent.height = wd->Height;
    rp.clearValueCount = 1;
    rp.pClearValues = &wd->ClearValue;
    vkCmdBeginRenderPass(fd->CommandBuffer, &rp, VK_SUBPASS_CONTENTS_INLINE);

    ImGui_ImplVulkan_RenderDrawData(draw_data, fd->CommandBuffer);
    vkCmdEndRenderPass(fd->CommandBuffer);
    check_vk_result(vkEndCommandBuffer(fd->CommandBuffer));

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &image_acquired_semaphore;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &fd->CommandBuffer;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &render_complete_semaphore;
    check_vk_result(vkQueueSubmit(g_Queue, 1, &submit, fd->Fence));
}

static void FramePresent(ImGui_ImplVulkanH_Window* wd) {
    if (g_SwapChainRebuild) return;
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    VkPresentInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &render_complete_semaphore;
    info.swapchainCount = 1;
    info.pSwapchains = &wd->Swapchain;
    info.pImageIndices = &wd->FrameIndex;
    VkResult err = vkQueuePresentKHR(g_Queue, &info);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
        g_SwapChainRebuild = true;
        return;
    }
    check_vk_result(err);
    wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->SemaphoreCount;
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--vacancy-selftest") {
            return runVacancySelftest(fs::current_path());
        }
    }
    fs::path root = fs::current_path();
    g_EnableValidationLayers = loadAppSettingsValidation(root, g_EnableValidationLayers);
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(1600, 1000, "Baltimore Vulkan Map", nullptr, nullptr);

    uint32_t extensions_count = 0;
    const char** extensions = glfwGetRequiredInstanceExtensions(&extensions_count);
    SetupVulkan(extensions, extensions_count);

    VkSurfaceKHR surface;
    VkResult err = glfwCreateWindowSurface(g_Instance, window, g_Allocator, &surface);
    if (err != VK_SUCCESS) return 1;

    int w, h;
    glfwGetFramebufferSize(window, &w, &h);
    ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
    SetupVulkanWindow(wd, surface, w, h);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsLight();

    ImGui_ImplGlfw_InitForVulkan(window, true);
    ImGui_ImplVulkan_InitInfo init_info{};
    init_info.Instance = g_Instance;
    init_info.PhysicalDevice = g_PhysicalDevice;
    init_info.Device = g_Device;
    init_info.QueueFamily = g_QueueFamily;
    init_info.Queue = g_Queue;
    init_info.DescriptorPool = g_DescriptorPool;
    init_info.RenderPass = wd->RenderPass;
    init_info.MinImageCount = g_MinImageCount;
    init_info.ImageCount = wd->ImageCount;
    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.CheckVkResultFn = check_vk_result;
    ImGui_ImplVulkan_Init(&init_info);

    {
        VkCommandPool command_pool = wd->Frames[wd->FrameIndex].CommandPool;
        VkCommandBuffer command_buffer = wd->Frames[wd->FrameIndex].CommandBuffer;
        check_vk_result(vkResetCommandPool(g_Device, command_pool, 0));
        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk_result(vkBeginCommandBuffer(command_buffer, &begin_info));
        ImGui_ImplVulkan_CreateFontsTexture();
        check_vk_result(vkEndCommandBuffer(command_buffer));
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command_buffer;
        check_vk_result(vkQueueSubmit(g_Queue, 1, &submit, VK_NULL_HANDLE));
        check_vk_result(vkDeviceWaitIdle(g_Device));
        ImGui_ImplVulkan_DestroyFontsTexture();
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    BootstrapProgress bootstrap;
    bootstrap.running.store(true, std::memory_order_relaxed);
    setBootstrapStatus(bootstrap, "Checking required data...");
    std::thread bootstrap_worker([&]() {
        auto fail = [&](const std::string& msg) {
            std::lock_guard<std::mutex> lk(bootstrap.msg_mutex);
            bootstrap.failed.store(true, std::memory_order_relaxed);
            bootstrap.error = msg;
            bootstrap.running.store(false, std::memory_order_relaxed);
            bootstrap.done.store(true, std::memory_order_relaxed);
        };
        try {
            // 1) Layers
            bootstrap.phase.store(1, std::memory_order_relaxed);
            auto manifest = readLayerManifestEntries(root);
            if (manifest.empty()) {
                fail("layers_manifest.json not found or invalid");
                return;
            }
            std::vector<std::pair<std::string, fs::path>> missing_layers;
            for (const auto& e : manifest) {
                if (!e.contains("file") || !e.contains("url")) continue;
                fs::path out = root / "data" / "layers" / e["file"].get<std::string>();
                if (!fs::exists(out)) missing_layers.push_back({e["url"].get<std::string>(), out});
            }
            bootstrap.total_items.store(missing_layers.size(), std::memory_order_relaxed);
            bootstrap.done_items.store(0, std::memory_order_relaxed);
            for (size_t i = 0; i < missing_layers.size(); ++i) {
                setBootstrapStatus(bootstrap, "Downloading layer " + std::to_string(i + 1) + "/" + std::to_string(missing_layers.size()));
                std::string err_s;
                if (!downloadUrlToFile(missing_layers[i].first, missing_layers[i].second, err_s)) {
                    fail("Layer download failed: " + err_s);
                    return;
                }
                bootstrap.done_items.fetch_add(1, std::memory_order_relaxed);
            }

            // 2) Tiles
            bootstrap.phase.store(2, std::memory_order_relaxed);
            constexpr double MIN_LON = -76.72, MIN_LAT = 39.20, MAX_LON = -76.50, MAX_LAT = 39.38;
            constexpr int MIN_ZOOM = 11, MAX_ZOOM = 18;
            std::vector<std::tuple<int, int, int>> missing_tiles;
            for (int z = MIN_ZOOM; z <= MAX_ZOOM; ++z) {
                auto [x0, y1] = deg2num(MIN_LAT, MIN_LON, z);
                auto [x1, y0] = deg2num(MAX_LAT, MAX_LON, z);
                int x_min = std::min(x0, x1), x_max = std::max(x0, x1);
                int y_min = std::min(y0, y1), y_max = std::max(y0, y1);
                for (int x = x_min; x <= x_max; ++x) {
                    for (int y = y_min; y <= y_max; ++y) {
                        fs::path out = root / "data" / "tiles" / std::to_string(z) / std::to_string(x) / (std::to_string(y) + ".png");
                        if (!fs::exists(out)) missing_tiles.emplace_back(z, x, y);
                    }
                }
            }
            bootstrap.total_items.store(missing_tiles.size(), std::memory_order_relaxed);
            bootstrap.done_items.store(0, std::memory_order_relaxed);
            for (size_t i = 0; i < missing_tiles.size(); ++i) {
                auto [z, x, y] = missing_tiles[i];
                setBootstrapStatus(bootstrap, "Downloading tile " + std::to_string(i + 1) + "/" + std::to_string(missing_tiles.size()));
                fs::path out = root / "data" / "tiles" / std::to_string(z) / std::to_string(x) / (std::to_string(y) + ".png");
                std::string url = "https://tile.openstreetmap.org/" + std::to_string(z) + "/" + std::to_string(x) + "/" + std::to_string(y) + ".png";
                std::string err_s;
                if (!downloadUrlToFile(url, out, err_s)) {
                    // Keep going for transient/provider failures; data loader can continue with partial tiles.
                }
                bootstrap.done_items.fetch_add(1, std::memory_order_relaxed);
            }

            bootstrap.phase.store(3, std::memory_order_relaxed);
            setBootstrapStatus(bootstrap, "Data check complete.");
            bootstrap.running.store(false, std::memory_order_relaxed);
            bootstrap.done.store(true, std::memory_order_relaxed);
        } catch (const std::exception& e) {
            fail(std::string("Bootstrap exception: ") + e.what());
        }
    });

    while (bootstrap.running.load(std::memory_order_relaxed) && !glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (g_SwapChainRebuild) {
            glfwGetFramebufferSize(window, &w, &h);
            if (w > 0 && h > 0) {
                ImGui_ImplVulkan_SetMinImageCount(g_MinImageCount);
                ImGui_ImplVulkanH_CreateOrResizeWindow(
                    g_Instance, g_PhysicalDevice, g_Device, &g_MainWindowData, g_QueueFamily, g_Allocator, w, h, g_MinImageCount);
                g_MainWindowData.FrameIndex = 0;
                g_SwapChainRebuild = false;
            }
        }
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2((float)w * 0.5f - 280.0f, (float)h * 0.5f - 90.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(560, 180), ImGuiCond_Always);
        ImGui::Begin("Startup Data Check", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
        int ph = bootstrap.phase.load(std::memory_order_relaxed);
        const char* phase_name = ph == 1 ? "Layers" : (ph == 2 ? "Tiles" : (ph == 3 ? "Done" : "Init"));
        size_t done = bootstrap.done_items.load(std::memory_order_relaxed);
        size_t total = bootstrap.total_items.load(std::memory_order_relaxed);
        double pct = total ? (100.0 * (double)done / (double)total) : 100.0;
        std::string status;
        {
            std::lock_guard<std::mutex> lk(bootstrap.msg_mutex);
            status = bootstrap.status;
        }
        ImGui::Text("Phase: %s", phase_name);
        ImGui::ProgressBar((float)(total ? ((double)done / (double)total) : 1.0), ImVec2(-1.0f, 0.0f));
        ImGui::Text("%zu / %zu (%.1f%%)", done, total, pct);
        ImGui::Separator();
        ImGui::TextWrapped("%s", status.c_str());
        ImGui::End();

        ImGui::Render();
        ImDrawData* draw_data = ImGui::GetDrawData();
        if (draw_data->DisplaySize.x > 0.0f && draw_data->DisplaySize.y > 0.0f) {
            wd->ClearValue.color.float32[0] = 0.95f;
            wd->ClearValue.color.float32[1] = 0.95f;
            wd->ClearValue.color.float32[2] = 0.96f;
            wd->ClearValue.color.float32[3] = 1.0f;
            FrameRender(wd, draw_data);
            FramePresent(wd);
        }
    }
    if (bootstrap_worker.joinable()) bootstrap_worker.join();
    if (bootstrap.failed.load(std::memory_order_relaxed)) {
        std::string err_msg;
        {
            std::lock_guard<std::mutex> lk(bootstrap.msg_mutex);
            err_msg = bootstrap.error;
        }
        std::fprintf(stderr, "bootstrap failed: %s\n", err_msg.c_str());
    }

    auto layers = loadManifest(root);
    int parcel_layer_idx = -1;
    int real_property_layer_idx = -1;
    int vacant_notice_layer_idx = -1;
    int vacant_rehab_layer_idx = -1;
    int zoning_layer_idx = -1;
    for (size_t i = 0; i < layers.size(); ++i) {
        if (layers[i].file == "parcel.geojson") parcel_layer_idx = (int)i;
        else if (layers[i].file == "real_property_information.geojson") real_property_layer_idx = (int)i;
        else if (layers[i].file == "vacant_building_notices.geojson") vacant_notice_layer_idx = (int)i;
        else if (layers[i].file == "vacant_building_rehabs.geojson") vacant_rehab_layer_idx = (int)i;
        else if (layers[i].file == "zoning.geojson") zoning_layer_idx = (int)i;
    }
    std::unordered_map<std::string, size_t> real_property_by_blocklot;
    std::unordered_map<std::string, int> vacant_notice_count_by_blocklot;
    std::unordered_map<std::string, int> vacant_rehab_count_by_blocklot;
    std::unordered_map<std::string, bool> zoning_zone_enabled;
    std::unordered_map<std::string, ImVec4> zoning_zone_color;
    std::unordered_map<std::string, std::string> zoning_zone_label;
    std::vector<std::string> zoning_zone_order;
    std::unordered_map<std::string, size_t> zoning_zone_counts;
    std::unordered_map<std::string, std::vector<std::string>> zoning_group_zones;
    std::vector<std::string> zoning_group_order;
    size_t zoning_zone_discovered_feature_count = 0;
    std::vector<int> parcel_vac_notice_by_feature;
    std::vector<int> parcel_vac_rehab_by_feature;
    int vacancy_maps_generation = 0;
    int parcel_vacancy_generation_applied = -1;
    size_t cached_real_property_size = 0;
    size_t cached_vac_notice_size = 0;
    size_t cached_vac_rehab_size = 0;
    std::mutex hydrated_mutex;
    std::deque<HydratedLayer> hydrated_queue;
    std::mutex hydrate_req_mutex;
    std::condition_variable hydrate_req_cv;
    std::deque<size_t> hydrate_requests;
    std::mutex tri_mutex;
    std::condition_variable tri_cv;
    std::deque<TriJob> tri_jobs;
    std::deque<TriResult> tri_results;
    std::atomic<bool> hydration_stop{false};
    std::atomic<size_t> hydrated_count{0};
    std::atomic<size_t> triangulated_count{0};
    std::atomic<double> perf_frame_ms_avg{0.0};
    std::atomic<double> perf_frame_ms_last{0.0};
    std::atomic<double> perf_fps_avg{0.0};
    std::atomic<size_t> visible_vacant_parcels_last_frame{0};
    std::atomic<size_t> vacant_parcels_matched_total{0};
    std::atomic<size_t> vacant_parcels_with_geometry_total{0};
    std::atomic<size_t> vacant_parcels_triangulated_renderable_total{0};
    std::atomic<int> current_zoom_state{12};
    std::atomic<double> current_lon_state{-76.6122};
    std::atomic<double> current_lat_state{39.2904};
    std::atomic<int> api_zoom_cmd{-1};
    std::atomic<double> api_lon_cmd{std::numeric_limits<double>::quiet_NaN()};
    std::atomic<double> api_lat_cmd{std::numeric_limits<double>::quiet_NaN()};
    std::mutex api_layer_mutex;
    std::unordered_map<std::string, bool> api_layer_enable_cmds;
    std::mutex status_mutex;
    std::vector<LayerRuntimeState> layer_states(layers.size());
    std::vector<LayerSpatialIndex> layer_spatial(layers.size());
    std::vector<LayerGeometryCache> layer_geom_cache(layers.size());
    std::vector<uint32_t> render_candidates;
    auto hydration_started_at = std::chrono::steady_clock::now();
    auto last_hydration_progress_at = hydration_started_at;
    auto last_tri_progress_at = hydration_started_at;
    size_t last_hydrated_seen = 0;
    size_t last_triangulated_seen = 0;

    std::vector<bool> hydration_requested(layers.size(), false);
    for (size_t i = 0; i < layers.size(); ++i) {
        if (layers[i].enabled) {
            hydrate_requests.push_back(i);
            hydration_requested[i] = true;
        }
    }
    auto enqueue_hydration = [&](size_t idx) {
        if (idx >= layers.size()) return;
        std::lock_guard<std::mutex> lk(hydrate_req_mutex);
        if (!hydration_requested[idx]) {
            hydrate_requests.push_back(idx);
            hydration_requested[idx] = true;
            std::lock_guard<std::mutex> lk2(status_mutex);
            if (idx < layer_states.size()) layer_states[idx].status = LayerPipelineStatus::Queued;
        }
        hydrate_req_cv.notify_one();
    };

    std::vector<std::thread> hydration_workers;
    const unsigned int hw = std::max(1u, std::thread::hardware_concurrency());
    const unsigned int hydration_worker_count = std::min(4u, hw);
    for (unsigned int wi = 0; wi < hydration_worker_count; ++wi) {
        hydration_workers.emplace_back([&]() {
            constexpr size_t kHydrationBatchSize = 350;
            while (!hydration_stop.load(std::memory_order_relaxed)) {
                size_t i = 0;
                {
                    std::unique_lock<std::mutex> lk(hydrate_req_mutex);
                    hydrate_req_cv.wait_for(lk, std::chrono::milliseconds(100), [&]() {
                        return hydration_stop.load(std::memory_order_relaxed) || !hydrate_requests.empty();
                    });
                    if (hydration_stop.load(std::memory_order_relaxed)) break;
                    if (hydrate_requests.empty()) continue;
                    i = hydrate_requests.front();
                    hydrate_requests.pop_front();
                }
                if (i >= layers.size() || !layers[i].enabled) continue;
                {
                    std::lock_guard<std::mutex> lk(status_mutex);
                    if (i < layer_states.size()) layer_states[i].status = LayerPipelineStatus::Hydrating;
                }

                const fs::path layer_path = root / "data" / "layers" / layers[i].file;
                const fs::path cache_path = root / "data" / "cache" / "hydration" / (layers[i].file + ".msgpack");
                const std::string sig = fileSignature(layer_path);
                std::vector<LayerDef::FeatureGeom> cached_features;
                if (loadHydrationCache(cache_path, sig, cached_features)) {
                    // Treat empty cache as stale/corrupt and fall back to source parse.
                    if (!cached_features.empty()) {
                        for (size_t off = 0; off < cached_features.size(); off += kHydrationBatchSize) {
                            if (hydration_stop.load(std::memory_order_relaxed) || !layers[i].enabled) break;
                            size_t end = std::min(cached_features.size(), off + kHydrationBatchSize);
                            std::vector<LayerDef::FeatureGeom> chunk;
                            chunk.reserve(end - off);
                            for (size_t k = off; k < end; ++k) chunk.push_back(std::move(cached_features[k]));
                            std::lock_guard<std::mutex> lk(hydrated_mutex);
                            hydrated_queue.push_back(HydratedLayer{i, std::move(chunk), false, false, ""});
                        }
                        std::lock_guard<std::mutex> lk(hydrated_mutex);
                        hydrated_queue.push_back(HydratedLayer{i, {}, true, false, ""});
                        continue;
                    }
                }

                hydrateLayerBatches(
                    layer_path, kHydrationBatchSize, hydration_stop,
                    [&]() { return i < layers.size() && layers[i].enabled; },
                    [&](std::vector<LayerDef::FeatureGeom>&& chunk, bool done, bool failed, const std::string& error) {
                        std::lock_guard<std::mutex> lk(hydrated_mutex);
                        hydrated_queue.push_back(HydratedLayer{i, std::move(chunk), done, failed, error});
                    });
            }
        });
    }

    std::thread triangulation_worker([&]() {
        while (!hydration_stop.load(std::memory_order_relaxed)) {
            TriJob job;
            {
                std::unique_lock<std::mutex> lk(tri_mutex);
                tri_cv.wait_for(lk, std::chrono::milliseconds(100), [&]() {
                    return hydration_stop.load(std::memory_order_relaxed) || !tri_jobs.empty();
                });
                if (hydration_stop.load(std::memory_order_relaxed)) break;
                if (tri_jobs.empty()) continue;
                    job = std::move(tri_jobs.front());
                    tri_jobs.pop_front();
            }
            {
                std::lock_guard<std::mutex> lk(status_mutex);
                if (job.index < layer_states.size()) layer_states[job.index].status = LayerPipelineStatus::Triangulating;
            }

            fs::path layer_path = root / "data" / "layers" / job.file;
            fs::path cache_path = root / "data" / "cache" / "triangulation" / (job.file + ".tri.json");
            std::string sig = fileSignature(layer_path);
            TriResult result;
            result.index = job.index;
            try {
                if (!loadTriCache(cache_path, sig, job.rings_per_feature.size(), result.triangles_per_feature)) {
                    result.triangles_per_feature.resize(job.rings_per_feature.size());
                    for (size_t i = 0; i < job.rings_per_feature.size(); ++i) {
                        if (!job.rings_per_feature[i].empty()) {
                            result.triangles_per_feature[i] = triangulateRings(job.rings_per_feature[i]);
                        }
                    }
                    saveTriCache(cache_path, sig, result.triangles_per_feature);
                }
                result.ok = true;
            } catch (const std::exception& e) {
                result.ok = false;
                result.error = e.what();
            }
            {
                std::lock_guard<std::mutex> lk(tri_mutex);
                tri_results.push_back(std::move(result));
            }
        }
    });

    std::thread status_api_worker([&]() {
        int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0) return;
        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(8787);
        if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
            close(server_fd);
            return;
        }
        if (listen(server_fd, 8) != 0) {
            close(server_fd);
            return;
        }

        while (!hydration_stop.load(std::memory_order_relaxed)) {
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(server_fd, &readfds);
            timeval tv{};
            tv.tv_sec = 1;
            int sel = select(server_fd + 1, &readfds, nullptr, nullptr, &tv);
            if (sel <= 0) continue;
            int client_fd = accept(server_fd, nullptr, nullptr);
            if (client_fd < 0) continue;

            char buf[1024];
            ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
            if (n <= 0) {
                close(client_fd);
                continue;
            }
            buf[n] = '\0';
            std::string req(buf);
            size_t method_sp = req.find(' ');
            size_t path_sp = method_sp == std::string::npos ? std::string::npos : req.find(' ', method_sp + 1);
            std::string path_q = (method_sp != std::string::npos && path_sp != std::string::npos)
                ? req.substr(method_sp + 1, path_sp - method_sp - 1)
                : "/";
            auto split_q = [&](const std::string& pq) {
                size_t q = pq.find('?');
                return std::make_pair(pq.substr(0, q), q == std::string::npos ? std::string() : pq.substr(q + 1));
            };
            auto [path, query] = split_q(path_q);
            auto get_q = [&](const std::string& key) -> std::string {
                size_t pos = 0;
                while (pos < query.size()) {
                    size_t amp = query.find('&', pos);
                    std::string kv = query.substr(pos, (amp == std::string::npos ? query.size() : amp) - pos);
                    size_t eq = kv.find('=');
                    std::string k = eq == std::string::npos ? kv : kv.substr(0, eq);
                    if (k == key) return eq == std::string::npos ? std::string() : kv.substr(eq + 1);
                    if (amp == std::string::npos) break;
                    pos = amp + 1;
                }
                return {};
            };

            if (path == "/set_zoom") {
                std::string v = get_q("value");
                if (!v.empty()) api_zoom_cmd.store(std::stoi(v), std::memory_order_relaxed);
                const char* body = "{\"ok\":true}";
                std::ostringstream os;
                os << "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " << std::strlen(body)
                   << "\r\nConnection: close\r\n\r\n" << body;
                std::string resp = os.str();
                (void)writeAll(client_fd, resp.data(), resp.size());
            } else if (path == "/set_center") {
                std::string lon = get_q("lon");
                std::string lat = get_q("lat");
                if (!lon.empty()) api_lon_cmd.store(std::stod(lon), std::memory_order_relaxed);
                if (!lat.empty()) api_lat_cmd.store(std::stod(lat), std::memory_order_relaxed);
                const char* body = "{\"ok\":true}";
                std::ostringstream os;
                os << "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " << std::strlen(body)
                   << "\r\nConnection: close\r\n\r\n" << body;
                std::string resp = os.str();
                (void)writeAll(client_fd, resp.data(), resp.size());
            } else if (path == "/set_layer") {
                std::string file = get_q("file");
                std::string enabled = get_q("enabled");
                if (!file.empty() && !enabled.empty()) {
                    bool en = (enabled == "1" || enabled == "true" || enabled == "on");
                    std::lock_guard<std::mutex> lk(api_layer_mutex);
                    api_layer_enable_cmds[file] = en;
                }
                const char* body = "{\"ok\":true}";
                std::ostringstream os;
                os << "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " << std::strlen(body)
                   << "\r\nConnection: close\r\n\r\n" << body;
                std::string resp = os.str();
                (void)writeAll(client_fd, resp.data(), resp.size());
            } else if (path == "/status") {
                std::vector<LayerRuntimeState> states_copy;
                {
                    std::lock_guard<std::mutex> lk(status_mutex);
                    states_copy = layer_states;
                }
                const auto now = std::chrono::steady_clock::now();
                const double elapsed_s = std::max(0.001, std::chrono::duration<double>(now - hydration_started_at).count());
                size_t feature_total = 0;
                for (const auto& st : states_copy) feature_total += st.feature_count;
                json out;
                out["schema_version"] = 2;
                out["layers_total"] = layers.size();
                out["hydrated"] = hydrated_count.load(std::memory_order_relaxed);
                out["triangulated"] = triangulated_count.load(std::memory_order_relaxed);
                const double total_layers = layers.empty() ? 1.0 : (double)layers.size();
                out["hydration_pct"] = ((double)out["hydrated"].get<size_t>() / total_layers) * 100.0;
                out["triangulation_pct"] = ((double)out["triangulated"].get<size_t>() / total_layers) * 100.0;
                out["elapsed_seconds"] = elapsed_s;
                out["hydrated_layers_per_min"] = ((double)out["hydrated"].get<size_t>() / elapsed_s) * 60.0;
                out["triangulated_layers_per_min"] = ((double)out["triangulated"].get<size_t>() / elapsed_s) * 60.0;
                out["hydrated_features_total"] = feature_total;
                out["hydrated_features_per_sec"] = (double)feature_total / elapsed_s;
                out["tile_cache_size"] = g_TileCache.size();
                out["tile_cache_max"] = kMaxTileCache;
                out["view"] = {
                    {"zoom", current_zoom_state.load(std::memory_order_relaxed)},
                    {"center_lon", current_lon_state.load(std::memory_order_relaxed)},
                    {"center_lat", current_lat_state.load(std::memory_order_relaxed)},
                    {"visible_vacant_parcels_last_frame", visible_vacant_parcels_last_frame.load(std::memory_order_relaxed)}
                };
                out["vacancy_probe"] = {
                    {"matched_total", vacant_parcels_matched_total.load(std::memory_order_relaxed)},
                    {"with_geometry_total", vacant_parcels_with_geometry_total.load(std::memory_order_relaxed)},
                    {"triangulated_renderable_total", vacant_parcels_triangulated_renderable_total.load(std::memory_order_relaxed)}
                };
                out["perf"] = {
                    {"frame_ms_avg", perf_frame_ms_avg.load(std::memory_order_relaxed)},
                    {"frame_ms_last", perf_frame_ms_last.load(std::memory_order_relaxed)},
                    {"fps_avg", perf_fps_avg.load(std::memory_order_relaxed)}
                };
                json status_counts = json::object();
                out["layers"] = json::array();
                for (size_t i = 0; i < states_copy.size(); ++i) {
                    const auto& st = states_copy[i];
                    const char* s = statusToString(st.status);
                    status_counts[s] = status_counts.value(s, 0) + 1;
                    out["layers"].push_back({
                        {"index", i},
                        {"name", i < layers.size() ? layers[i].name : std::string()},
                        {"file", i < layers.size() ? layers[i].file : std::string()},
                        {"enabled", i < layers.size() ? layers[i].enabled : false},
                        {"status", s},
                        {"features", st.feature_count},
                        {"hydrated", st.status != LayerPipelineStatus::Queued && st.status != LayerPipelineStatus::Hydrating && st.status != LayerPipelineStatus::Failed},
                        {"triangulated", st.status == LayerPipelineStatus::Ready},
                        {"error", st.error}
                    });
                }
                out["status_counts"] = status_counts;
                std::string body = out.dump();
                std::ostringstream os;
                os << "HTTP/1.1 200 OK\r\n"
                   << "Content-Type: application/json\r\n"
                   << "Content-Length: " << body.size() << "\r\n"
                   << "Connection: close\r\n\r\n"
                   << body;
                std::string resp = os.str();
                (void)writeAll(client_fd, resp.data(), resp.size());
            } else if (path == "/vacancy_probe") {
                json out;
                out["ok"] = true;
                out["matched_total"] = vacant_parcels_matched_total.load(std::memory_order_relaxed);
                out["with_geometry_total"] = vacant_parcels_with_geometry_total.load(std::memory_order_relaxed);
                out["triangulated_renderable_total"] = vacant_parcels_triangulated_renderable_total.load(std::memory_order_relaxed);
                out["visible_last_frame"] = visible_vacant_parcels_last_frame.load(std::memory_order_relaxed);
                std::string body = out.dump();
                std::ostringstream os;
                os << "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " << body.size()
                   << "\r\nConnection: close\r\n\r\n" << body;
                std::string resp = os.str();
                (void)writeAll(client_fd, resp.data(), resp.size());
            } else {
                const char* body = "{\"error\":\"not found\"}";
                std::ostringstream os;
                os << "HTTP/1.1 404 Not Found\r\n"
                   << "Content-Type: application/json\r\n"
                   << "Content-Length: " << std::strlen(body) << "\r\n"
                   << "Connection: close\r\n\r\n"
                   << body;
                std::string resp = os.str();
                (void)writeAll(client_fd, resp.data(), resp.size());
            }
            close(client_fd);
        }
        close(server_fd);
    });

    int zoom = 12;
    float center_lon = -76.6122f;
    float center_lat = 39.2904f;
    bool parcel_hover_enabled = true;
    loadLayerUiState(root, layers, parcel_hover_enabled, &zoning_zone_enabled);
    std::vector<bool> last_enabled_state;
    last_enabled_state.reserve(layers.size());
    for (const auto& l : layers) last_enabled_state.push_back(l.enabled);
    bool last_parcel_hover_enabled = parcel_hover_enabled;
    auto last_frame_ts = std::chrono::steady_clock::now();
    double ema_frame_ms = 0.0;
    constexpr double kPerfAlpha = 0.12;
    std::string last_cache_clear_msg;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        if (g_SwapChainRebuild) {
            glfwGetFramebufferSize(window, &w, &h);
            if (w > 0 && h > 0) {
                ImGui_ImplVulkan_SetMinImageCount(g_MinImageCount);
                ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, &g_MainWindowData, g_QueueFamily, g_Allocator, w, h, g_MinImageCount);
                g_MainWindowData.FrameIndex = 0;
                g_SwapChainRebuild = false;
            }
        }

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        // Apply REST control commands on main thread.
        {
            int zc = api_zoom_cmd.exchange(-1, std::memory_order_relaxed);
            if (zc >= kMinZoom && zc <= kMaxZoom) zoom = zc;
            double lonc = api_lon_cmd.exchange(std::numeric_limits<double>::quiet_NaN(), std::memory_order_relaxed);
            double latc = api_lat_cmd.exchange(std::numeric_limits<double>::quiet_NaN(), std::memory_order_relaxed);
            if (!std::isnan(lonc)) center_lon = (float)lonc;
            if (!std::isnan(latc)) center_lat = std::clamp((float)latc, -85.0f, 85.0f);
            std::lock_guard<std::mutex> lk(api_layer_mutex);
            for (const auto& kv : api_layer_enable_cmds) {
                for (auto& l : layers) {
                    if (l.file == kv.first) l.enabled = kv.second;
                }
            }
            api_layer_enable_cmds.clear();
        }
        current_zoom_state.store(zoom, std::memory_order_relaxed);
        current_lon_state.store(center_lon, std::memory_order_relaxed);
        current_lat_state.store(center_lat, std::memory_order_relaxed);

        ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(420, 760), ImGuiCond_Always);
        ImGui::Begin("Layers and Controls");
        ImGui::Text("Vulkan map + Vulkan UI");
        ImGui::SliderInt("Zoom", &zoom, kMinZoom, kMaxZoom);
        ImGui::SliderFloat("Center Lon", &center_lon, -76.75f, -76.45f, "%.4f");
        ImGui::SliderFloat("Center Lat", &center_lat, 39.18f, 39.40f, "%.4f");
        ImGui::Checkbox("Enable Parcel Hover Inspector", &parcel_hover_enabled);
        bool validation_ui = g_EnableValidationLayers;
        if (ImGui::Checkbox("Vulkan Validation (restart required)", &validation_ui)) {
            g_EnableValidationLayers = validation_ui;
            saveAppSettings(root, g_EnableValidationLayers);
        }
        ImGui::Separator();

        if (ImGui::Button("Show All")) {
            for (auto& l : layers) l.enabled = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Hide All")) {
            for (auto& l : layers) l.enabled = false;
        }

        auto draw_category = [&](LayerDef::Category cat, const char* label) {
            if (!ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen)) return;
            std::string show_id = std::string("Show ") + label;
            std::string hide_id = std::string("Hide ") + label;
            if (ImGui::Button(show_id.c_str())) {
                for (auto& l : layers) if (l.category == cat) l.enabled = true;
            }
            ImGui::SameLine();
            if (ImGui::Button(hide_id.c_str())) {
                for (auto& l : layers) if (l.category == cat) l.enabled = false;
            }
            for (auto& l : layers) {
                size_t idx = (size_t)(&l - &layers[0]);
                if (l.category != cat) continue;
                ImGui::PushStyleColor(ImGuiCol_Text, l.color);
                ImGui::Checkbox(l.name.c_str(), &l.enabled);
                ImGui::PopStyleColor();
                const bool row_hovered = ImGui::IsItemHovered();
                ImGui::SameLine();
                LayerRuntimeState st;
                {
                    std::lock_guard<std::mutex> lk(status_mutex);
                    if (idx < layer_states.size()) st = layer_states[idx];
                }
                if (st.status == LayerPipelineStatus::Failed) {
                    ImGui::TextColored(ImVec4(0.85f, 0.35f, 0.2f, 1.0f), "[%s]", statusToString(st.status));
                } else {
                    ImGui::TextDisabled("[%s | %zu]", statusToString(st.status), st.feature_count);
                }
                const bool status_hovered = ImGui::IsItemHovered();
                if (row_hovered || status_hovered) {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted(l.name.c_str());
                    ImGui::Separator();
                    ImGui::Text("Category: %s", categoryToString(l.category));
                    ImGui::Text("Status: %s", statusToString(st.status));
                    ImGui::Text("Features: %zu", st.feature_count);
                    ImGui::Text("File: %s", l.file.c_str());
                    if (!l.description.empty()) ImGui::TextWrapped("Description: %s", l.description.c_str());
                    if (!l.source_url.empty()) ImGui::TextWrapped("Source: %s", l.source_url.c_str());
                    if (!st.error.empty()) {
                        ImGui::Separator();
                        ImGui::TextColored(ImVec4(0.85f, 0.35f, 0.2f, 1.0f), "Error: %s", st.error.c_str());
                    }
                    ImGui::EndTooltip();
                }
            }
        };

        draw_category(LayerDef::Category::Housing, "Housing");
        draw_category(LayerDef::Category::Infrastructure, "Infrastructure");
        bool zoning_filters_changed = false;
        if (zoning_layer_idx >= 0 && ImGui::CollapsingHeader("Zoning Filters", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::Button("Show All Zones")) {
                for (auto& kv : zoning_zone_enabled) {
                    if (!kv.second) zoning_filters_changed = true;
                    kv.second = true;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Hide All Zones")) {
                for (auto& kv : zoning_zone_enabled) {
                    if (kv.second) zoning_filters_changed = true;
                    kv.second = false;
                }
            }
            if (zoning_zone_order.empty()) {
                ImGui::TextDisabled("Zoning classes will appear after zoning layer hydrates.");
            } else {
                for (const auto& gkey : zoning_group_order) {
                    auto git = zoning_group_zones.find(gkey);
                    if (git == zoning_group_zones.end()) continue;
                    const auto& zones = git->second;
                    size_t enabled_count = 0;
                    for (const auto& z : zones) if (zoning_zone_enabled[z]) enabled_count++;
                    bool group_enabled = enabled_count == zones.size() && !zones.empty();
                    bool group_partial = enabled_count > 0 && enabled_count < zones.size();

                    if (group_partial) ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(1.0f, 0.75f, 0.1f, 1.0f));
                    std::string gcb = gkey + "##group_enabled";
                    bool group_next = group_enabled;
                    if (ImGui::Checkbox(gcb.c_str(), &group_next)) {
                        for (const auto& z : zones) {
                            if (zoning_zone_enabled[z] != group_next) zoning_filters_changed = true;
                            zoning_zone_enabled[z] = group_next;
                        }
                    }
                    if (group_partial) ImGui::PopStyleColor();
                    ImGui::SameLine();
                    std::string gheader = gkey + " Zones (" + std::to_string(enabled_count) + "/" + std::to_string(zones.size()) + ")";
                    if (ImGui::TreeNodeEx(gheader.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                        for (const auto& zkey : zones) {
                            ImVec4 zc = zoning_zone_color[zkey];
                            ImGui::ColorButton((std::string("##zclr_") + zkey).c_str(), zc, ImGuiColorEditFlags_NoTooltip, ImVec2(12, 12));
                            ImGui::SameLine();
                            bool enabled = zoning_zone_enabled[zkey];
                            std::string display = zkey;
                            auto lit = zoning_zone_label.find(zkey);
                            if (lit != zoning_zone_label.end() && !lit->second.empty() && lit->second != zkey) {
                                display += " - " + lit->second;
                            }
                            std::string label = display + " (" + std::to_string(zoning_zone_counts[zkey]) + ")";
                            if (ImGui::Checkbox(label.c_str(), &enabled)) {
                                zoning_zone_enabled[zkey] = enabled;
                                zoning_filters_changed = true;
                            }
                        }
                        ImGui::TreePop();
                    }
                }
            }
        }

        const size_t hydrated_now = hydrated_count.load(std::memory_order_relaxed);
        const size_t triangulated_now = triangulated_count.load(std::memory_order_relaxed);
        if (hydrated_now > last_hydrated_seen) {
            last_hydrated_seen = hydrated_now;
            last_hydration_progress_at = std::chrono::steady_clock::now();
        }
        if (triangulated_now > last_triangulated_seen) {
            last_triangulated_seen = triangulated_now;
            last_tri_progress_at = std::chrono::steady_clock::now();
        }

        size_t hydrated_pending = 0;
        {
            std::lock_guard<std::mutex> lk(hydrated_mutex);
            hydrated_pending = hydrated_queue.size();
        }
        size_t tri_pending = 0;
        {
            std::lock_guard<std::mutex> lk(tri_mutex);
            tri_pending = tri_jobs.size();
        }

        const float hydrated_frac = layers.empty() ? 1.0f : (float)hydrated_now / (float)layers.size();
        const float tri_frac = layers.empty() ? 1.0f : (float)triangulated_now / (float)layers.size();
        const auto now = std::chrono::steady_clock::now();
        const double elapsed_s = std::chrono::duration<double>(now - hydration_started_at).count();
        const double hydrate_idle_s = std::chrono::duration<double>(now - last_hydration_progress_at).count();
        const double tri_idle_s = std::chrono::duration<double>(now - last_tri_progress_at).count();

        ImGui::SeparatorText("Background Load Progress");
        ImGui::Text("Hydration: %zu / %zu (%.1f%%)", hydrated_now, layers.size(), hydrated_frac * 100.0f);
        ImGui::ProgressBar(hydrated_frac, ImVec2(-1.0f, 0.0f));
        ImGui::Text("Triangulation: %zu / %zu (%.1f%%)", triangulated_now, layers.size(), tri_frac * 100.0f);
        ImGui::ProgressBar(tri_frac, ImVec2(-1.0f, 0.0f));
        ImGui::TextDisabled("Hydration queue: %zu | Tri queue: %zu | elapsed: %.1fs", hydrated_pending, tri_pending, elapsed_s);
        ImGui::TextDisabled("Frame: %.2f ms (last %.2f) | FPS: %.1f",
                            perf_frame_ms_avg.load(std::memory_order_relaxed),
                            perf_frame_ms_last.load(std::memory_order_relaxed),
                            perf_fps_avg.load(std::memory_order_relaxed));
        ImGui::TextDisabled("API: http://127.0.0.1:8787/status");
        if (hydrated_now < layers.size() && hydrate_idle_s > 15.0) {
            ImGui::TextColored(ImVec4(0.85f, 0.35f, 0.2f, 1.0f), "Hydration has not advanced for %.1fs", hydrate_idle_s);
        }
        if (triangulated_now < layers.size() && tri_idle_s > 15.0) {
            ImGui::TextColored(ImVec4(0.85f, 0.35f, 0.2f, 1.0f), "Triangulation has not advanced for %.1fs", tri_idle_s);
        }
        ImGui::Separator();
        ImGui::TextDisabled("Tile cache: %zu / %zu", g_TileCache.size(), kMaxTileCache);
        if (ImGui::Button("Clear Cache")) {
            size_t removed_files = 0;
            std::error_code ec;
            auto clear_tree = [&](const fs::path& p) {
                if (fs::exists(p, ec)) removed_files += fs::remove_all(p, ec);
            };
            clear_tree(root / "data" / "cache" / "hydration");
            clear_tree(root / "data" / "cache" / "triangulation");
            clear_tree(root / "data" / "cache" / "derived");
            fs::create_directories(root / "data" / "cache" / "hydration", ec);
            fs::create_directories(root / "data" / "cache" / "triangulation", ec);
            fs::create_directories(root / "data" / "cache" / "derived", ec);

            for (auto& kv : g_TileCache) destroyTileTexture(kv.second.tex);
            g_TileCache.clear();
            g_TileLRU.clear();

            {
                std::lock_guard<std::mutex> lk(hydrated_mutex);
                hydrated_queue.clear();
            }
            {
                std::lock_guard<std::mutex> lk(tri_mutex);
                tri_jobs.clear();
                tri_results.clear();
            }
            {
                std::lock_guard<std::mutex> lk(hydrate_req_mutex);
                hydrate_requests.clear();
                std::fill(hydration_requested.begin(), hydration_requested.end(), false);
            }

            hydrated_count.store(0, std::memory_order_relaxed);
            triangulated_count.store(0, std::memory_order_relaxed);
            cached_real_property_size = 0;
            cached_vac_notice_size = 0;
            cached_vac_rehab_size = 0;
            vacancy_maps_generation = 0;
            parcel_vacancy_generation_applied = -1;
            parcel_vac_notice_by_feature.clear();
            parcel_vac_rehab_by_feature.clear();
            vacant_notice_count_by_blocklot.clear();
            vacant_rehab_count_by_blocklot.clear();
            real_property_by_blocklot.clear();
            zoning_zone_label.clear();
            zoning_zone_counts.clear();
            zoning_zone_order.clear();
            zoning_group_zones.clear();
            zoning_group_order.clear();
            zoning_zone_discovered_feature_count = 0;
            visible_vacant_parcels_last_frame.store(0, std::memory_order_relaxed);
            vacant_parcels_matched_total.store(0, std::memory_order_relaxed);
            vacant_parcels_with_geometry_total.store(0, std::memory_order_relaxed);
            vacant_parcels_triangulated_renderable_total.store(0, std::memory_order_relaxed);

            for (size_t i = 0; i < layers.size(); ++i) {
                layers[i].features.clear();
                layer_spatial[i] = LayerSpatialIndex{};
                layer_geom_cache[i] = LayerGeometryCache{};
                std::lock_guard<std::mutex> lk(status_mutex);
                if (i < layer_states.size()) {
                    layer_states[i].status = LayerPipelineStatus::Queued;
                    layer_states[i].feature_count = 0;
                    layer_states[i].error.clear();
                }
            }

            for (size_t i = 0; i < layers.size(); ++i) {
                if (layers[i].enabled) enqueue_hydration(i);
            }
            last_cache_clear_msg = "Cache cleared; rehydrating enabled layers.";
        }
        if (!last_cache_clear_msg.empty()) ImGui::TextDisabled("%s", last_cache_clear_msg.c_str());
        ImGui::End();
        bool ui_state_changed = (parcel_hover_enabled != last_parcel_hover_enabled) || zoning_filters_changed;
        std::vector<size_t> newly_enabled;
        if (last_enabled_state.size() == layers.size()) {
            for (size_t i = 0; i < layers.size(); ++i) {
                if (layers[i].enabled != last_enabled_state[i]) {
                    ui_state_changed = true;
                    if (layers[i].enabled && !last_enabled_state[i]) newly_enabled.push_back(i);
                }
            }
        } else {
            ui_state_changed = true;
            for (size_t i = 0; i < layers.size(); ++i) {
                if (layers[i].enabled) newly_enabled.push_back(i);
            }
        }
        if (!newly_enabled.empty()) {
            for (size_t i : newly_enabled) enqueue_hydration(i);
        }
        // Vacant overlays depend on parcel geometry even when parcel visibility is off.
        const bool vacant_layer_active =
            (vacant_notice_layer_idx >= 0 && layers[(size_t)vacant_notice_layer_idx].enabled) ||
            (vacant_rehab_layer_idx >= 0 && layers[(size_t)vacant_rehab_layer_idx].enabled);
        if (vacant_layer_active && parcel_layer_idx >= 0) {
            bool parcel_ready = false;
            {
                std::lock_guard<std::mutex> lk(status_mutex);
                if ((size_t)parcel_layer_idx < layer_states.size()) {
                    LayerPipelineStatus st = layer_states[(size_t)parcel_layer_idx].status;
                    parcel_ready = (st == LayerPipelineStatus::Hydrated ||
                                    st == LayerPipelineStatus::TriQueued ||
                                    st == LayerPipelineStatus::Triangulating ||
                                    st == LayerPipelineStatus::Ready);
                }
            }
            if (!parcel_ready && layers[(size_t)parcel_layer_idx].features.empty()) {
                enqueue_hydration((size_t)parcel_layer_idx);
            }
        }
        if (parcel_hover_enabled && zoning_layer_idx >= 0 && layers[(size_t)zoning_layer_idx].features.empty()) {
            enqueue_hydration((size_t)zoning_layer_idx);
        }
        if (ui_state_changed) {
            saveLayerUiState(root, layers, parcel_hover_enabled, &zoning_zone_enabled);
            last_enabled_state.clear();
            last_enabled_state.reserve(layers.size());
            for (const auto& l : layers) last_enabled_state.push_back(l.enabled);
            last_parcel_hover_enabled = parcel_hover_enabled;
        }
        {
            std::lock_guard<std::mutex> lk(hydrated_mutex);
            while (!hydrated_queue.empty()) {
                HydratedLayer ready = std::move(hydrated_queue.front());
                hydrated_queue.pop_front();
                if (ready.index < layers.size()) {
                    if (!ready.features.empty()) {
                        auto& dst = layers[ready.index].features;
                        dst.insert(
                            dst.end(),
                            std::make_move_iterator(ready.features.begin()),
                            std::make_move_iterator(ready.features.end()));
                        std::lock_guard<std::mutex> lk3(status_mutex);
                        if (ready.index < layer_states.size()) {
                            layer_states[ready.index].status = LayerPipelineStatus::Hydrating;
                            layer_states[ready.index].feature_count = layers[ready.index].features.size();
                        }
                    }
                    if (ready.failed) {
                        {
                            std::lock_guard<std::mutex> lk4(hydrate_req_mutex);
                            if (ready.index < hydration_requested.size()) hydration_requested[ready.index] = false;
                        }
                        std::lock_guard<std::mutex> lk3(status_mutex);
                        if (ready.index < layer_states.size()) {
                            layer_states[ready.index].status = LayerPipelineStatus::Failed;
                            layer_states[ready.index].error = ready.error;
                        }
                        continue;
                    }
                    if (!ready.done) continue;
                    {
                        std::lock_guard<std::mutex> lk4(hydrate_req_mutex);
                        if (ready.index < hydration_requested.size()) hydration_requested[ready.index] = false;
                    }
                    hydrated_count.fetch_add(1, std::memory_order_relaxed);
                    {
                        fs::path layer_path = root / "data" / "layers" / layers[ready.index].file;
                        fs::path cache_path = root / "data" / "cache" / "hydration" / (layers[ready.index].file + ".msgpack");
                        saveHydrationCache(cache_path, fileSignature(layer_path), layers[ready.index].features);
                    }
                    {
                        std::lock_guard<std::mutex> lk3(status_mutex);
                        if (ready.index < layer_states.size()) {
                            layer_states[ready.index].status = LayerPipelineStatus::Hydrated;
                            layer_states[ready.index].feature_count = layers[ready.index].features.size();
                            layer_states[ready.index].error.clear();
                        }
                    }
                    TriJob tj;
                    tj.index = ready.index;
                    tj.file = layers[ready.index].file;
                    tj.rings_per_feature.reserve(layers[ready.index].features.size());
                    for (const auto& fg : layers[ready.index].features) tj.rings_per_feature.push_back(fg.rings);
                    std::lock_guard<std::mutex> lk2(tri_mutex);
                    {
                        std::lock_guard<std::mutex> lk3(status_mutex);
                        if (ready.index < layer_states.size()) layer_states[ready.index].status = LayerPipelineStatus::TriQueued;
                    }
                    if (layers[ready.index].enabled) {
                        tri_jobs.push_front(std::move(tj));
                    } else {
                        tri_jobs.push_back(std::move(tj));
                    }
                    tri_cv.notify_one();
                }
            }
        }
        {
            std::lock_guard<std::mutex> lk(tri_mutex);
            while (!tri_results.empty()) {
                TriResult tr = std::move(tri_results.front());
                tri_results.pop_front();
                if (tr.index < layers.size()) {
                    if (tr.ok) {
                        auto& fs = layers[tr.index].features;
                        size_t n = std::min(fs.size(), tr.triangles_per_feature.size());
                        for (size_t i = 0; i < n; ++i) fs[i].triangles = std::move(tr.triangles_per_feature[i]);
                        triangulated_count.fetch_add(1, std::memory_order_relaxed);
                        std::lock_guard<std::mutex> lk3(status_mutex);
                        if (tr.index < layer_states.size()) layer_states[tr.index].status = LayerPipelineStatus::Ready;
                    } else {
                        std::lock_guard<std::mutex> lk3(status_mutex);
                        if (tr.index < layer_states.size()) {
                            layer_states[tr.index].status = LayerPipelineStatus::Failed;
                            layer_states[tr.index].error = tr.error;
                        }
                    }
                }
            }
        }

        if (zoning_layer_idx >= 0 && (size_t)zoning_layer_idx < layers.size()) {
            const auto& zfeats = layers[(size_t)zoning_layer_idx].features;
            if (zfeats.size() != zoning_zone_discovered_feature_count) {
                zoning_zone_discovered_feature_count = zfeats.size();
                zoning_zone_counts.clear();
                zoning_zone_label.clear();
                zoning_group_zones.clear();
                zoning_group_order.clear();
                std::unordered_map<std::string, bool> prev_enabled = zoning_zone_enabled;
                zoning_zone_order.clear();
                for (const auto& fg : zfeats) {
                    std::string zkey = zoningClassKey(fg);
                    std::string zlabel = zoningClassLabel(fg);
                    zoning_zone_counts[zkey] += 1;
                    if (zoning_zone_enabled.find(zkey) == zoning_zone_enabled.end()) {
                        auto it_prev = prev_enabled.find(zkey);
                        zoning_zone_enabled[zkey] = (it_prev == prev_enabled.end()) ? true : it_prev->second;
                        zoning_zone_order.push_back(zkey);
                    }
                    if (zoning_zone_label.find(zkey) == zoning_zone_label.end()) zoning_zone_label[zkey] = zlabel;
                    if (zoning_zone_color.find(zkey) == zoning_zone_color.end()) {
                        zoning_zone_color[zkey] = colorFromStableKey(zkey);
                    }
                }
                std::sort(zoning_zone_order.begin(), zoning_zone_order.end());
                for (const auto& zkey : zoning_zone_order) {
                    std::string g = zoningGroupKey(zkey);
                    if (zoning_group_zones.find(g) == zoning_group_zones.end()) zoning_group_order.push_back(g);
                    zoning_group_zones[g].push_back(zkey);
                }
                std::sort(zoning_group_order.begin(), zoning_group_order.end());
            }
        }

        if (real_property_layer_idx >= 0) {
            const auto& feats = layers[(size_t)real_property_layer_idx].features;
            if (feats.size() != cached_real_property_size) {
                real_property_by_blocklot.clear();
                for (size_t i = 0; i < feats.size(); ++i) {
                    std::string bl = normalizeJoinKey(getPropertyValue(feats[i], "BLOCKLOT"));
                    if (!bl.empty() && real_property_by_blocklot.find(bl) == real_property_by_blocklot.end()) {
                        real_property_by_blocklot[bl] = i;
                    }
                }
                cached_real_property_size = feats.size();
            }
        }
        if (vacant_notice_layer_idx >= 0) {
            const auto& feats = layers[(size_t)vacant_notice_layer_idx].features;
            if (feats.size() != cached_vac_notice_size) {
                vacant_notice_count_by_blocklot.clear();
                for (const auto& fg : feats) {
                    std::string bl = normalizeJoinKey(getPropertyValue(fg, "BLOCKLOT"));
                    if (!bl.empty()) vacant_notice_count_by_blocklot[bl] += 1;
                }
                cached_vac_notice_size = feats.size();
                vacancy_maps_generation += 1;
            }
        }
        if (vacant_rehab_layer_idx >= 0) {
            const auto& feats = layers[(size_t)vacant_rehab_layer_idx].features;
            if (feats.size() != cached_vac_rehab_size) {
                vacant_rehab_count_by_blocklot.clear();
                for (const auto& fg : feats) {
                    std::string bl = normalizeJoinKey(getPropertyValue(fg, "BLOCKLOT"));
                    if (!bl.empty()) vacant_rehab_count_by_blocklot[bl] += 1;
                }
                cached_vac_rehab_size = feats.size();
                vacancy_maps_generation += 1;
            }
        }
        if (parcel_layer_idx >= 0) {
            const auto& pfeats = layers[(size_t)parcel_layer_idx].features;
            if (parcel_vac_notice_by_feature.size() != pfeats.size() ||
                parcel_vac_rehab_by_feature.size() != pfeats.size() ||
                parcel_vacancy_generation_applied != vacancy_maps_generation) {
                parcel_vac_notice_by_feature.assign(pfeats.size(), 0);
                parcel_vac_rehab_by_feature.assign(pfeats.size(), 0);
                size_t notice_rows_matched = 0;
                size_t rehab_rows_matched = 0;
                for (size_t i = 0; i < pfeats.size(); ++i) {
                    std::string bl = normalizeJoinKey(getPropertyValue(pfeats[i], "BLOCKLOT"));
                    auto itn = vacant_notice_count_by_blocklot.find(bl);
                    if (itn != vacant_notice_count_by_blocklot.end()) {
                        parcel_vac_notice_by_feature[i] = itn->second;
                        notice_rows_matched += (size_t)itn->second;
                    }
                    auto itr = vacant_rehab_count_by_blocklot.find(bl);
                    if (itr != vacant_rehab_count_by_blocklot.end()) {
                        parcel_vac_rehab_by_feature[i] = itr->second;
                        rehab_rows_matched += (size_t)itr->second;
                    }
                }
                parcel_vacancy_generation_applied = vacancy_maps_generation;
                const fs::path derived_path = root / "data" / "cache" / "derived" / "parcel_vacancy_status.json";
                saveDerivedVacancyStatus(
                    derived_path,
                    pfeats,
                    parcel_vac_notice_by_feature,
                    parcel_vac_rehab_by_feature,
                    (size_t)cached_vac_notice_size,
                    (size_t)cached_vac_rehab_size,
                    notice_rows_matched,
                    rehab_rows_matched);
            }
            size_t matched_total = 0;
            size_t with_geometry_total = 0;
            size_t triangulated_renderable_total = 0;
            for (size_t i = 0; i < pfeats.size(); ++i) {
                const int vac_notice = (i < parcel_vac_notice_by_feature.size()) ? parcel_vac_notice_by_feature[i] : 0;
                const int vac_rehab = (i < parcel_vac_rehab_by_feature.size()) ? parcel_vac_rehab_by_feature[i] : 0;
                if ((vac_notice + vac_rehab) <= 0) continue;
                matched_total++;
                if (!pfeats[i].rings.empty()) with_geometry_total++;
                if (!pfeats[i].rings.empty() && !pfeats[i].triangles.empty()) triangulated_renderable_total++;
            }
            vacant_parcels_matched_total.store(matched_total, std::memory_order_relaxed);
            vacant_parcels_with_geometry_total.store(with_geometry_total, std::memory_order_relaxed);
            vacant_parcels_triangulated_renderable_total.store(triangulated_renderable_total, std::memory_order_relaxed);
        }
        for (size_t li = 0; li < layers.size(); ++li) {
            LayerPipelineStatus st = LayerPipelineStatus::Queued;
            {
                std::lock_guard<std::mutex> lk(status_mutex);
                if (li < layer_states.size()) st = layer_states[li].status;
            }
            const bool stable =
                st == LayerPipelineStatus::Hydrated ||
                st == LayerPipelineStatus::TriQueued ||
                st == LayerPipelineStatus::Triangulating ||
                st == LayerPipelineStatus::Ready;
            if (!stable) continue;
            if (!layer_spatial[li].built || layer_spatial[li].feature_count_built != layers[li].features.size()) {
                buildLayerSpatialIndex(layers[li], layer_spatial[li]);
            }
            if (layer_geom_cache[li].feature_count != layers[li].features.size()) {
                layer_geom_cache[li].feature_count = layers[li].features.size();
                layer_geom_cache[li].zoom = -1;
                layer_geom_cache[li].world_rings_by_feature.clear();
            }
        }

        ImGui::SetNextWindowPos(ImVec2(440, 12), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2((float)w - 452.0f, (float)h - 24.0f), ImGuiCond_Always);
        ImGui::Begin("Map", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImVec2 size = ImGui::GetContentRegionAvail();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImGui::InvisibleButton("map_canvas_input", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        const bool map_hovered = ImGui::IsItemHovered();
        const bool map_active = ImGui::IsItemActive();

        draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(242, 246, 250, 255));

        const int math_zoom = std::min(zoom, kMaxInternalMathZoom);
        const double zoom_scale = std::ldexp(1.0, zoom - math_zoom);
        ImVec2 center_world = lonLatToWorldPx(center_lon, center_lat, math_zoom);

        if (map_hovered) {
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f) {
                int next_zoom = std::clamp(zoom + (wheel > 0 ? 1 : -1), kMinZoom, kMaxZoom);
                if (next_zoom != zoom) {
                    const ImVec2 mouse = ImGui::GetIO().MousePos;
                    ImVec2 mouse_world = ImVec2(
                        center_world.x + (float)((mouse.x - (origin.x + size.x * 0.5f)) / zoom_scale),
                        center_world.y + (float)((mouse.y - (origin.y + size.y * 0.5f)) / zoom_scale));
                    ImVec2 ll = worldPxToLonLat(mouse_world, math_zoom);
                    zoom = next_zoom;
                    const int next_math_zoom = std::min(zoom, kMaxInternalMathZoom);
                    const double next_zoom_scale = std::ldexp(1.0, zoom - next_math_zoom);
                    ImVec2 mouse_world_new = lonLatToWorldPx(ll.x, ll.y, next_math_zoom);
                    center_world = ImVec2(
                        mouse_world_new.x - (float)((mouse.x - (origin.x + size.x * 0.5f)) / next_zoom_scale),
                        mouse_world_new.y - (float)((mouse.y - (origin.y + size.y * 0.5f)) / next_zoom_scale));
                    ImVec2 cll = worldPxToLonLat(center_world, next_math_zoom);
                    center_lon = cll.x;
                    center_lat = cll.y;
                }
            }

            if (map_active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
                ImVec2 d = ImGui::GetIO().MouseDelta;
                center_world.x -= (float)(d.x / zoom_scale);
                center_world.y -= (float)(d.y / zoom_scale);
                ImVec2 ll = worldPxToLonLat(center_world, math_zoom);
                center_lon = ll.x;
                center_lat = std::clamp(ll.y, -85.0f, 85.0f);
            }
        }

        center_world = lonLatToWorldPx(center_lon, center_lat, math_zoom);
        const ImVec2 mouse_screen = ImGui::GetIO().MousePos;
        const ImVec2 mouse_world(
            center_world.x + (float)((mouse_screen.x - (origin.x + size.x * 0.5f)) / zoom_scale),
            center_world.y + (float)((mouse_screen.y - (origin.y + size.y * 0.5f)) / zoom_scale));
        const ImVec2 mouse_ll = worldPxToLonLat(mouse_world, math_zoom);
        static ImVec2 context_ll(0.0f, 0.0f);
        if (map_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            context_ll = mouse_ll;
            ImGui::OpenPopup("map_context_menu");
        }
        if (ImGui::BeginPopup("map_context_menu")) {
            ImGui::Text("Lon: %.6f", context_ll.x);
            ImGui::Text("Lat: %.6f", context_ll.y);
            ImGui::Separator();
            if (ImGui::MenuItem("Open Google Maps Street View")) {
                std::ostringstream url;
                url << std::fixed << std::setprecision(7)
                    << "https://www.google.com/maps/@?api=1&map_action=pano&viewpoint="
                    << context_ll.y << "," << context_ll.x;
                openUrlInBrowser(url.str());
            }
            ImGui::EndPopup();
        }
        const LayerDef::FeatureGeom* hovered_parcel = nullptr;
        size_t hovered_parcel_idx = (size_t)-1;
        const double half_w_world = (size.x * 0.5) / zoom_scale;
        const double half_h_world = (size.y * 0.5) / zoom_scale;
        ImVec2 ll_a = worldPxToLonLat(ImVec2(center_world.x - (float)half_w_world, center_world.y - (float)half_h_world), math_zoom);
        ImVec2 ll_b = worldPxToLonLat(ImVec2(center_world.x + (float)half_w_world, center_world.y + (float)half_h_world), math_zoom);
        const float view_min_lon = std::min(ll_a.x, ll_b.x);
        const float view_max_lon = std::max(ll_a.x, ll_b.x);
        const float view_min_lat = std::min(ll_a.y, ll_b.y);
        const float view_max_lat = std::max(ll_a.y, ll_b.y);
        int min_x = (int)std::floor((center_world.x - half_w_world) / 256.0) - 1;
        int max_x = (int)std::floor((center_world.x + half_w_world) / 256.0) + 1;
        int min_y = (int)std::floor((center_world.y - half_h_world) / 256.0) - 1;
        int max_y = (int)std::floor((center_world.y + half_h_world) / 256.0) + 1;
        const int period = 1 << math_zoom;
        const int max_tile = period - 1;

        for (int tx = min_x; tx <= max_x; ++tx) {
            for (int ty = min_y; ty <= max_y; ++ty) {
                int wrapped_x = tx;
                while (wrapped_x < 0) wrapped_x += period;
                while (wrapped_x > max_tile) wrapped_x -= period;
                if (ty < 0 || ty > max_tile) continue;

                TileSample sample = getTileSample(root, math_zoom, wrapped_x, ty);
                if (!sample.tex) continue;

                ImVec2 tile_world((float)(tx * 256), (float)(ty * 256));
                ImVec2 p0 = worldToScreen(tile_world, center_world, origin, size, zoom_scale);
                ImVec2 p1(p0.x + (float)(256.0 * zoom_scale), p0.y + (float)(256.0 * zoom_scale));
                draw->AddImage((ImTextureID)sample.tex->descriptor, p0, p1, sample.uv0, sample.uv1);
            }
        }

        bool vacant_notice_enabled = vacant_notice_layer_idx >= 0 && layers[(size_t)vacant_notice_layer_idx].enabled;
        bool vacant_rehab_enabled = vacant_rehab_layer_idx >= 0 && layers[(size_t)vacant_rehab_layer_idx].enabled;
        auto color_with_alpha = [](const ImVec4& c, int alpha) -> ImU32 {
            const int r = std::clamp((int)std::lround(c.x * 255.0f), 0, 255);
            const int g = std::clamp((int)std::lround(c.y * 255.0f), 0, 255);
            const int b = std::clamp((int)std::lround(c.z * 255.0f), 0, 255);
            return IM_COL32(r, g, b, alpha);
        };
        auto darken_color = [](const ImVec4& c, float mul) -> ImVec4 {
            return ImVec4(std::clamp(c.x * mul, 0.0f, 1.0f),
                          std::clamp(c.y * mul, 0.0f, 1.0f),
                          std::clamp(c.z * mul, 0.0f, 1.0f), 1.0f);
        };
        auto vacancy_base_color = [&](int vac_notice, int vac_rehab) -> ImVec4 {
            const ImVec4 notice_c = (vacant_notice_layer_idx >= 0) ? layers[(size_t)vacant_notice_layer_idx].color : ImVec4(1, 0, 0, 1);
            const ImVec4 rehab_c = (vacant_rehab_layer_idx >= 0) ? layers[(size_t)vacant_rehab_layer_idx].color : ImVec4(0, 1, 1, 1);
            if (vac_notice > 0 && vac_rehab > 0) {
                return ImVec4((notice_c.x + rehab_c.x) * 0.5f,
                              (notice_c.y + rehab_c.y) * 0.5f,
                              (notice_c.z + rehab_c.z) * 0.5f, 1.0f);
            }
            if (vac_rehab > 0) return rehab_c;
            return notice_c;
        };
        const int ring_step = lodRingStepForZoom(math_zoom);
        auto get_world_rings = [&](size_t layer_idx, uint32_t feature_idx, const LayerDef::FeatureGeom& fg)
            -> const std::vector<std::vector<ImVec2>>& {
            auto& cache = layer_geom_cache[layer_idx];
            if (cache.zoom != math_zoom) {
                cache.zoom = math_zoom;
                cache.world_rings_by_feature.clear();
            }
            auto it = cache.world_rings_by_feature.find(feature_idx);
            if (it != cache.world_rings_by_feature.end()) return it->second;
            std::vector<std::vector<ImVec2>> wr;
            wr.reserve(fg.rings.size());
            for (const auto& r : fg.rings) {
                std::vector<ImVec2> rr;
                rr.reserve(r.size());
                for (const ImVec2& ll : r) rr.push_back(lonLatToWorldPx(ll.x, ll.y, math_zoom));
                wr.push_back(std::move(rr));
            }
            auto [ins_it, _] = cache.world_rings_by_feature.emplace(feature_idx, std::move(wr));
            return ins_it->second;
        };
        for (size_t layer_idx = 0; layer_idx < layers.size(); ++layer_idx) {
            auto& l = layers[layer_idx];
            if (!l.enabled) continue;
            const bool is_zoning_layer = ((int)layer_idx == zoning_layer_idx);
            ImU32 c = ImGui::ColorConvertFloat4ToU32(l.color);
            bool have_candidates = queryLayerSpatialIndex(
                layer_spatial[layer_idx], view_min_lon, view_min_lat, view_max_lon, view_max_lat, render_candidates);
            if (have_candidates) {
                for (uint32_t fidx : render_candidates) {
                    if (fidx >= l.features.size()) continue;
                    auto& fg = l.features[(size_t)fidx];
                    ImU32 feature_c = c;
                    if (is_zoning_layer) {
                        const std::string zkey = zoningClassKey(fg);
                        auto it_en = zoning_zone_enabled.find(zkey);
                        if (it_en != zoning_zone_enabled.end() && !it_en->second) continue;
                        auto it_col = zoning_zone_color.find(zkey);
                        if (it_col != zoning_zone_color.end()) feature_c = ImGui::ColorConvertFloat4ToU32(it_col->second);
                    }
                    const auto& ex = fg.extent;
                    ImVec2 p0w = lonLatToWorldPx(ex.min_lon, ex.max_lat, math_zoom);
                    ImVec2 p1w = lonLatToWorldPx(ex.max_lon, ex.min_lat, math_zoom);
                    ImVec2 a = worldToScreen(p0w, center_world, origin, size, zoom_scale);
                    ImVec2 b = worldToScreen(p1w, center_world, origin, size, zoom_scale);
                    ImVec2 p0(std::min(a.x, b.x), std::min(a.y, b.y));
                    ImVec2 p1(std::max(a.x, b.x), std::max(a.y, b.y));
                    if (p1.x < origin.x || p0.x > origin.x + size.x || p1.y < origin.y || p0.y > origin.y + size.y) continue;

                    if (!fg.rings.empty()) {
                        const auto& world_rings = get_world_rings(layer_idx, fidx, fg);
                        std::vector<ImVec2> verts;
                        size_t total = 0;
                        for (const auto& r : world_rings) total += r.size();
                        verts.reserve(total);
                        for (const auto& r : world_rings) {
                            for (const ImVec2& wp : r) verts.push_back(worldToScreen(wp, center_world, origin, size, zoom_scale));
                        }

                        if (!fg.triangles.empty()) {
                            ImU32 fill = (feature_c & 0x00FFFFFF) | (60u << 24);
                            draw->PrimReserve((int)fg.triangles.size(), (int)verts.size());
                            unsigned int base = draw->_VtxCurrentIdx;
                            for (const ImVec2& v : verts) draw->PrimWriteVtx(v, ImVec2(0, 0), fill);
                            for (uint32_t idx : fg.triangles) draw->PrimWriteIdx((ImDrawIdx)(base + idx));
                        }

                        for (const auto& r : world_rings) {
                            std::vector<ImVec2> line;
                            appendWorldRingScreenPointsLod(r, ring_step, center_world, origin, size, zoom_scale, line);
                            draw->AddPolyline(line.data(), (int)line.size(), feature_c, ImDrawFlags_Closed, 1.0f);
                        }
                    } else {
                        if ((int)layer_idx == vacant_notice_layer_idx || (int)layer_idx == vacant_rehab_layer_idx) continue;
                        ImVec2 pw = lonLatToWorldPx(fg.extent.min_lon, fg.extent.min_lat, math_zoom);
                        ImVec2 ps = worldToScreen(pw, center_world, origin, size, zoom_scale);
                        if (ps.x >= origin.x && ps.x <= origin.x + size.x && ps.y >= origin.y && ps.y <= origin.y + size.y) {
                            float r = std::clamp((float)(2.0 * zoom_scale + 1.5), 2.0f, 6.0f);
                            draw->AddCircleFilled(ps, r, feature_c);
                        }
                    }

                    if (parcel_hover_enabled && map_hovered && hovered_parcel == nullptr && parcel_layer_idx >= 0 &&
                        (int)layer_idx == parcel_layer_idx) {
                        if (mouse_ll.x >= fg.extent.min_lon && mouse_ll.x <= fg.extent.max_lon &&
                            mouse_ll.y >= fg.extent.min_lat && mouse_ll.y <= fg.extent.max_lat &&
                            pointInFeature(fg, mouse_ll.x, mouse_ll.y)) {
                            hovered_parcel = &fg;
                            hovered_parcel_idx = (size_t)fidx;
                        }
                    }
                }
                continue;
            }
            for (auto& fg : l.features) {
                size_t fi = (size_t)(&fg - &l.features[0]);
                ImU32 feature_c = c;
                if (is_zoning_layer) {
                    const std::string zkey = zoningClassKey(fg);
                    auto it_en = zoning_zone_enabled.find(zkey);
                    if (it_en != zoning_zone_enabled.end() && !it_en->second) continue;
                    auto it_col = zoning_zone_color.find(zkey);
                    if (it_col != zoning_zone_color.end()) feature_c = ImGui::ColorConvertFloat4ToU32(it_col->second);
                }
                const auto& ex = fg.extent;
                ImVec2 p0w = lonLatToWorldPx(ex.min_lon, ex.max_lat, math_zoom);
                ImVec2 p1w = lonLatToWorldPx(ex.max_lon, ex.min_lat, math_zoom);
                ImVec2 a = worldToScreen(p0w, center_world, origin, size, zoom_scale);
                ImVec2 b = worldToScreen(p1w, center_world, origin, size, zoom_scale);
                ImVec2 p0(std::min(a.x, b.x), std::min(a.y, b.y));
                ImVec2 p1(std::max(a.x, b.x), std::max(a.y, b.y));
                if (p1.x < origin.x || p0.x > origin.x + size.x || p1.y < origin.y || p0.y > origin.y + size.y) continue;

                if (!fg.rings.empty()) {
                    const auto& world_rings = get_world_rings(layer_idx, (uint32_t)fi, fg);
                    std::vector<ImVec2> verts;
                    size_t total = 0;
                    for (const auto& r : world_rings) total += r.size();
                    verts.reserve(total);
                    for (const auto& r : world_rings) {
                        for (const ImVec2& wp : r) verts.push_back(worldToScreen(wp, center_world, origin, size, zoom_scale));
                    }

                    if (!fg.triangles.empty()) {
                        ImU32 fill = (feature_c & 0x00FFFFFF) | (60u << 24);
                        draw->PrimReserve((int)fg.triangles.size(), (int)verts.size());
                        unsigned int base = draw->_VtxCurrentIdx;
                        for (const ImVec2& v : verts) draw->PrimWriteVtx(v, ImVec2(0, 0), fill);
                        for (uint32_t idx : fg.triangles) draw->PrimWriteIdx((ImDrawIdx)(base + idx));
                    }

                    // Project vacant point datasets onto parcel polygons instead of drawing point markers.
                    if (layer_idx == (size_t)parcel_layer_idx && (vacant_notice_enabled || vacant_rehab_enabled)) {
                        int vac_notice = 0;
                        int vac_rehab = 0;
                        if (fi < parcel_vac_notice_by_feature.size()) vac_notice = parcel_vac_notice_by_feature[fi];
                        if (fi < parcel_vac_rehab_by_feature.size()) vac_rehab = parcel_vac_rehab_by_feature[fi];

                        int weight = 0;
                        if (vacant_notice_enabled) weight += vac_notice;
                        if (vacant_rehab_enabled) weight += vac_rehab;
                        if (weight > 0 && !fg.triangles.empty()) {
                            const int alpha = std::clamp(45 + weight * 12, 45, 170);
                            ImU32 vac_fill = color_with_alpha(vacancy_base_color(vac_notice, vac_rehab), alpha);
                            draw->PrimReserve((int)fg.triangles.size(), (int)verts.size());
                            unsigned int base = draw->_VtxCurrentIdx;
                            for (const ImVec2& v : verts) draw->PrimWriteVtx(v, ImVec2(0, 0), vac_fill);
                            for (uint32_t idx : fg.triangles) draw->PrimWriteIdx((ImDrawIdx)(base + idx));
                        }
                    }

                    for (const auto& r : world_rings) {
                        std::vector<ImVec2> line;
                        appendWorldRingScreenPointsLod(r, ring_step, center_world, origin, size, zoom_scale, line);
                        draw->AddPolyline(line.data(), (int)line.size(), feature_c, ImDrawFlags_Closed, 1.0f);
                    }
                } else {
                    // Vacant datasets are point sources used for parcel matching; suppress raw point dot rendering.
                    if (layer_idx == (size_t)vacant_notice_layer_idx || layer_idx == (size_t)vacant_rehab_layer_idx) continue;
                    ImVec2 pw = lonLatToWorldPx(fg.extent.min_lon, fg.extent.min_lat, math_zoom);
                    ImVec2 ps = worldToScreen(pw, center_world, origin, size, zoom_scale);
                    if (ps.x >= origin.x && ps.x <= origin.x + size.x && ps.y >= origin.y && ps.y <= origin.y + size.y) {
                        float r = std::clamp((float)(2.0 * zoom_scale + 1.5), 2.0f, 6.0f);
                        draw->AddCircleFilled(ps, r, feature_c);
                    }
                }

                if (parcel_hover_enabled && map_hovered && hovered_parcel == nullptr && parcel_layer_idx >= 0 &&
                    (&l - &layers[0]) == parcel_layer_idx) {
                    if (mouse_ll.x >= fg.extent.min_lon && mouse_ll.x <= fg.extent.max_lon &&
                        mouse_ll.y >= fg.extent.min_lat && mouse_ll.y <= fg.extent.max_lat &&
                        pointInFeature(fg, mouse_ll.x, mouse_ll.y)) {
                        hovered_parcel = &fg;
                        hovered_parcel_idx = fi;
                    }
                }
            }
        }

        // Render vacant overlays as a final top pass so they remain visible regardless of layer order.
        size_t visible_vacant_parcels_counter = 0;
        if ((vacant_notice_enabled || vacant_rehab_enabled) && parcel_layer_idx >= 0) {
            auto& parcel_layer = layers[(size_t)parcel_layer_idx];
            for (size_t i = 0; i < parcel_layer.features.size(); ++i) {
                auto& fg = parcel_layer.features[i];
                const auto& ex = fg.extent;
                ImVec2 p0w = lonLatToWorldPx(ex.min_lon, ex.max_lat, math_zoom);
                ImVec2 p1w = lonLatToWorldPx(ex.max_lon, ex.min_lat, math_zoom);
                ImVec2 a = worldToScreen(p0w, center_world, origin, size, zoom_scale);
                ImVec2 b = worldToScreen(p1w, center_world, origin, size, zoom_scale);
                ImVec2 p0(std::min(a.x, b.x), std::min(a.y, b.y));
                ImVec2 p1(std::max(a.x, b.x), std::max(a.y, b.y));
                if (p1.x < origin.x || p0.x > origin.x + size.x || p1.y < origin.y || p0.y > origin.y + size.y) continue;
                if (fg.rings.empty()) continue;

                int vac_notice = (i < parcel_vac_notice_by_feature.size()) ? parcel_vac_notice_by_feature[i] : 0;
                int vac_rehab = (i < parcel_vac_rehab_by_feature.size()) ? parcel_vac_rehab_by_feature[i] : 0;

                int weight = 0;
                if (vacant_notice_enabled) weight += vac_notice;
                if (vacant_rehab_enabled) weight += vac_rehab;
                if (weight <= 0) continue;
                visible_vacant_parcels_counter++;

                const auto& world_rings = get_world_rings((size_t)parcel_layer_idx, (uint32_t)i, fg);
                std::vector<ImVec2> verts;
                size_t total = 0;
                for (const auto& r : world_rings) total += r.size();
                verts.reserve(total);
                for (const auto& r : world_rings) {
                    for (const ImVec2& wp : r) verts.push_back(worldToScreen(wp, center_world, origin, size, zoom_scale));
                }
                const int alpha = std::clamp(70 + weight * 14, 70, 200);
                ImVec4 vac_base = vacancy_base_color(vac_notice, vac_rehab);
                ImU32 vac_fill = color_with_alpha(vac_base, alpha);
                ImU32 vac_outline = color_with_alpha(darken_color(vac_base, 0.62f), 235);
                if (!fg.triangles.empty()) {
                    draw->PrimReserve((int)fg.triangles.size(), (int)verts.size());
                    unsigned int base = draw->_VtxCurrentIdx;
                    for (const ImVec2& v : verts) draw->PrimWriteVtx(v, ImVec2(0, 0), vac_fill);
                    for (uint32_t idx : fg.triangles) draw->PrimWriteIdx((ImDrawIdx)(base + idx));
                }
                for (const auto& r : world_rings) {
                    std::vector<ImVec2> line;
                    appendWorldRingScreenPointsLod(r, ring_step, center_world, origin, size, zoom_scale, line);
                    if (!line.empty()) draw->AddPolyline(line.data(), (int)line.size(), vac_outline, ImDrawFlags_Closed, 2.0f);
                }
            }
        }
        visible_vacant_parcels_last_frame.store(visible_vacant_parcels_counter, std::memory_order_relaxed);

        if (parcel_hover_enabled && map_hovered && hovered_parcel) {
            std::string blocklot_raw = getPropertyValue(*hovered_parcel, "BLOCKLOT");
            std::string blocklot = normalizeJoinKey(blocklot_raw);
            int vac_notice = (hovered_parcel_idx < parcel_vac_notice_by_feature.size()) ? parcel_vac_notice_by_feature[hovered_parcel_idx] : 0;
            int vac_rehab = (hovered_parcel_idx < parcel_vac_rehab_by_feature.size()) ? parcel_vac_rehab_by_feature[hovered_parcel_idx] : 0;
            const LayerDef::FeatureGeom* hovered_zoning = nullptr;
            if (zoning_layer_idx >= 0 && (size_t)zoning_layer_idx < layers.size()) {
                const float qlon = (hovered_parcel->extent.min_lon + hovered_parcel->extent.max_lon) * 0.5f;
                const float qlat = (hovered_parcel->extent.min_lat + hovered_parcel->extent.max_lat) * 0.5f;
                std::vector<uint32_t> zoning_candidates;
                bool have_zoning_candidates = false;
                if ((size_t)zoning_layer_idx < layer_spatial.size() && layer_spatial[(size_t)zoning_layer_idx].built) {
                    have_zoning_candidates = queryLayerSpatialIndex(
                        layer_spatial[(size_t)zoning_layer_idx], qlon, qlat, qlon, qlat, zoning_candidates);
                }
                if (have_zoning_candidates) {
                    const auto& zfeats = layers[(size_t)zoning_layer_idx].features;
                    for (uint32_t zi : zoning_candidates) {
                        if (zi >= zfeats.size()) continue;
                        const auto& zf = zfeats[zi];
                        if (qlon >= zf.extent.min_lon && qlon <= zf.extent.max_lon &&
                            qlat >= zf.extent.min_lat && qlat <= zf.extent.max_lat &&
                            pointInFeature(zf, qlon, qlat)) {
                            hovered_zoning = &zf;
                            break;
                        }
                    }
                }
            }

            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Parcel Details");
            ImGui::Separator();
            ImGui::Text("BLOCKLOT: %s", blocklot_raw.empty() ? "(none)" : blocklot_raw.c_str());
            ImGui::Text("Vacant Notices: %d", vac_notice);
            ImGui::Text("Vacant Rehab Records: %d", vac_rehab);

            if (real_property_layer_idx >= 0 && !blocklot.empty()) {
                auto itrp = real_property_by_blocklot.find(blocklot);
                if (itrp != real_property_by_blocklot.end()) {
                    const auto& rp = layers[(size_t)real_property_layer_idx].features[itrp->second];
                    std::string owner = getPropertyValue(rp, "OWNERNME1");
                    if (owner.empty()) owner = getPropertyValue(rp, "OWNER");
                    if (owner.empty()) owner = getPropertyValue(rp, "OWNER_NAME");
                    std::string addr = getPropertyValue(rp, "PROPERTY_ADDRESS");
                    if (addr.empty()) addr = getPropertyValue(rp, "PREMISEADD");
                    if (!owner.empty()) ImGui::TextWrapped("Owner: %s", owner.c_str());
                    if (!addr.empty()) ImGui::TextWrapped("Property Address: %s", addr.c_str());
                }
            }

            ImGui::Separator();
            if (hovered_zoning) {
                std::string zone_label = zoningClassKey(*hovered_zoning);
                std::string zone_desc = zoningClassLabel(*hovered_zoning);
                ImGui::Text("Zoning: %s", zone_label.empty() ? "(available, unlabeled)" : zone_label.c_str());
                if (!zone_desc.empty() && zone_desc != zone_label) {
                    ImGui::TextWrapped("Zoning Label: %s", zone_desc.c_str());
                }
                ImGui::TextUnformatted("Zoning Fields");
                for (const auto& kv : hovered_zoning->properties) {
                    if (kv.second.empty()) continue;
                    ImGui::TextWrapped("%s: %s", kv.first.c_str(), kv.second.c_str());
                }
            } else if (zoning_layer_idx >= 0) {
                ImGui::TextDisabled("Zoning: no intersecting zoning polygon found.");
            }
            ImGui::Separator();
            ImGui::TextUnformatted("All Parcel Fields");
            for (const auto& kv : hovered_parcel->properties) {
                if (kv.second.empty()) continue;
                ImGui::TextWrapped("%s: %s", kv.first.c_str(), kv.second.c_str());
            }
            ImGui::EndTooltip();
        }

        ImGui::End();

        ImGui::Render();
        ImDrawData* draw_data = ImGui::GetDrawData();
        if (draw_data->DisplaySize.x > 0.0f && draw_data->DisplaySize.y > 0.0f) {
            wd->ClearValue.color.float32[0] = 0.95f;
            wd->ClearValue.color.float32[1] = 0.95f;
            wd->ClearValue.color.float32[2] = 0.96f;
            wd->ClearValue.color.float32[3] = 1.00f;
            FrameRender(wd, draw_data);
            FramePresent(wd);
        }

        auto now_frame = std::chrono::steady_clock::now();
        double frame_ms = std::chrono::duration<double, std::milli>(now_frame - last_frame_ts).count();
        last_frame_ts = now_frame;
        if (ema_frame_ms <= 0.0) ema_frame_ms = frame_ms;
        else ema_frame_ms = (1.0 - kPerfAlpha) * ema_frame_ms + kPerfAlpha * frame_ms;
        double fps = (ema_frame_ms > 0.0) ? (1000.0 / ema_frame_ms) : 0.0;
        perf_frame_ms_last.store(frame_ms, std::memory_order_relaxed);
        perf_frame_ms_avg.store(ema_frame_ms, std::memory_order_relaxed);
        perf_fps_avg.store(fps, std::memory_order_relaxed);
    }

    vkDeviceWaitIdle(g_Device);
    saveLayerUiState(root, layers, parcel_hover_enabled, &zoning_zone_enabled);
    saveAppSettings(root, g_EnableValidationLayers);
    hydration_stop.store(true, std::memory_order_relaxed);
    hydrate_req_cv.notify_all();
    tri_cv.notify_all();
    for (auto& t : hydration_workers) if (t.joinable()) t.join();
    if (triangulation_worker.joinable()) triangulation_worker.join();
    if (status_api_worker.joinable()) status_api_worker.join();
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    CleanupVulkanWindow();
    CleanupVulkan();
    glfwDestroyWindow(window);
    glfwTerminate();
    curl_global_cleanup();
    return 0;
}
