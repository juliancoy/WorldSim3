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
#include <unordered_set>
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
#include <cctype>
#include <cfloat>
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
    std::unordered_map<uint32_t, std::pair<ImVec2, ImVec2>> world_extent_by_feature;
};

static std::vector<LayerDef::FeatureGeom> loadLayerPointsFromFile(const fs::path& full_path);

struct BootstrapProgress {
    std::atomic<bool> running{false};
    std::atomic<bool> done{false};
    std::atomic<bool> failed{false};
    std::atomic<int> phase{0}; // 0=init,1=layers,2=tiles,3=done
    std::atomic<size_t> done_items{0};
    std::atomic<size_t> total_items{0};
    std::atomic<size_t> skipped_layers{0};
    std::atomic<size_t> skipped_tiles{0};
    std::vector<std::string> skipped_layer_files;
    std::string status;
    std::string error;
    std::mutex msg_mutex;
};

struct ZoneMetadata {
    std::string label;
    std::string description;
    std::string color_hex;
    ImVec4 color = ImVec4(0, 0, 0, 1);
    bool has_color = false;
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

struct ScreenshotRequestState {
    std::mutex mutex;
    std::condition_variable cv;
    bool pending = false;
    uint64_t req_id = 0;
    uint64_t done_id = 0;
    bool ok = false;
    std::string path;
    std::string error;
};

static ScreenshotRequestState g_ScreenshotState;

static bool writePpmRgb(
    const fs::path& out_path,
    const uint8_t* pixels,
    uint32_t width,
    uint32_t height,
    size_t row_pitch,
    VkFormat fmt,
    std::string& err) {
    std::ofstream out(out_path, std::ios::binary);
    if (!out) {
        err = "failed to open screenshot output";
        return false;
    }
    out << "P6\n" << width << " " << height << "\n255\n";
    const bool bgra = (fmt == VK_FORMAT_B8G8R8A8_UNORM || fmt == VK_FORMAT_B8G8R8A8_SRGB);
    const bool rgba = (fmt == VK_FORMAT_R8G8B8A8_UNORM || fmt == VK_FORMAT_R8G8B8A8_SRGB);
    if (!bgra && !rgba) {
        err = "unsupported swapchain format for screenshot";
        return false;
    }
    std::vector<uint8_t> line;
    line.resize((size_t)width * 3);
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* src = pixels + (size_t)y * row_pitch;
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* px = src + (size_t)x * 4;
            uint8_t r = rgba ? px[0] : px[2];
            uint8_t g = px[1];
            uint8_t b = rgba ? px[2] : px[0];
            size_t i = (size_t)x * 3;
            line[i + 0] = r;
            line[i + 1] = g;
            line[i + 2] = b;
        }
        out.write(reinterpret_cast<const char*>(line.data()), (std::streamsize)line.size());
    }
    if (!out.good()) {
        err = "failed writing screenshot file";
        return false;
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

static bool parseHexColor(const std::string& hex, ImVec4& out) {
    if (hex.size() != 7 || hex[0] != '#') return false;
    auto val = [&](size_t off) -> int {
        try {
            return std::stoi(hex.substr(off, 2), nullptr, 16);
        } catch (...) {
            return -1;
        }
    };
    int r = val(1), g = val(3), b = val(5);
    if (r < 0 || g < 0 || b < 0) return false;
    out = ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
    return true;
}

static std::unordered_map<std::string, ZoneMetadata> loadZoneMetadata(const fs::path& root) {
    std::unordered_map<std::string, ZoneMetadata> out;
    std::ifstream in(root / "data" / "zoning_classes.json");
    if (!in) in.open(root / "zoning_classes.json");
    if (!in) return out;
    json j;
    try {
        in >> j;
    } catch (...) {
        return out;
    }
    if (!j.contains("zones") || !j["zones"].is_object()) return out;
    for (auto it = j["zones"].begin(); it != j["zones"].end(); ++it) {
        if (!it.value().is_object()) continue;
        ZoneMetadata meta;
        const auto& v = it.value();
        if (v.contains("label") && v["label"].is_string()) meta.label = v["label"].get<std::string>();
        if (v.contains("description") && v["description"].is_string()) meta.description = v["description"].get<std::string>();
        if (v.contains("color") && v["color"].is_string()) {
            meta.color_hex = v["color"].get<std::string>();
            meta.has_color = parseHexColor(meta.color_hex, meta.color);
        }
        out[it.key()] = std::move(meta);
    }
    return out;
}

static std::string readTextFile(const fs::path& p) {
    std::ifstream in(p);
    if (!in) return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void collectTodoWork(const std::string& todo_text, std::vector<std::string>& past, std::vector<std::string>& future) {
    std::istringstream in(todo_text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("- [x]", 0) == 0 || line.rfind("- [X]", 0) == 0) {
            past.push_back(line.substr(5));
            continue;
        }
        if (line.rfind("- ", 0) == 0) {
            future.push_back(line.substr(2));
            continue;
        }
    }
}

static std::string toLowerAscii(std::string s) {
    for (char& ch : s) ch = (char)std::tolower((unsigned char)ch);
    return s;
}

static bool containsCaseInsensitive(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    const std::string h = toLowerAscii(haystack);
    const std::string n = toLowerAscii(needle);
    return h.find(n) != std::string::npos;
}

static int extractYearMaybe(const std::string& s) {
    for (size_t i = 0; i + 3 < s.size(); ++i) {
        if (!std::isdigit((unsigned char)s[i]) ||
            !std::isdigit((unsigned char)s[i + 1]) ||
            !std::isdigit((unsigned char)s[i + 2]) ||
            !std::isdigit((unsigned char)s[i + 3])) continue;
        const int y = (s[i] - '0') * 1000 + (s[i + 1] - '0') * 100 + (s[i + 2] - '0') * 10 + (s[i + 3] - '0');
        if (y >= 1900 && y <= 2100) return y;
    }
    return -1;
}

static double parseNumericField(const std::string& s) {
    std::string cleaned;
    cleaned.reserve(s.size());
    for (char ch : s) {
        if (std::isdigit((unsigned char)ch) || ch == '.' || ch == '-' || ch == '+') cleaned.push_back(ch);
    }
    if (cleaned.empty() || cleaned == "-" || cleaned == "+") return 0.0;
    try {
        return std::stod(cleaned);
    } catch (...) {
        return 0.0;
    }
}

static std::string trimDisplayValue(std::string s) {
    auto is_ws = [](unsigned char ch) { return std::isspace(ch) != 0; };
    while (!s.empty() && is_ws((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && is_ws((unsigned char)s.back())) s.pop_back();
    return s;
}

static std::string firstDisplayProperty(const LayerDef::FeatureGeom& fg, std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        std::string v = trimDisplayValue(getPropertyValue(fg, key));
        if (!v.empty()) return v;
    }
    return "";
}

static std::string blockLotJoinKeyFromParts(const std::string& block, const std::string& lot) {
    std::string b = normalizeJoinKey(block);
    std::string l = normalizeJoinKey(lot);
    if (b.empty() || l.empty()) return "";
    return b + l;
}

static std::string featureBlockLotJoinKey(const LayerDef::FeatureGeom& fg) {
    std::string bl = normalizeJoinKey(getPropertyValue(fg, "BLOCKLOT"));
    if (!bl.empty()) return bl;
    bl = normalizeJoinKey(getPropertyValue(fg, "PIN"));
    if (!bl.empty()) return bl;
    bl = normalizeJoinKey(getPropertyValue(fg, "pin"));
    if (!bl.empty()) return bl;
    bl = blockLotJoinKeyFromParts(getPropertyValue(fg, "BLOCK"), getPropertyValue(fg, "LOT"));
    if (!bl.empty()) return bl;
    return blockLotJoinKeyFromParts(getPropertyValue(fg, "block"), getPropertyValue(fg, "lot"));
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
    if (zoom <= 10) return 8;
    if (zoom == 11) return 6;
    if (zoom == 12) return 4;
    if (zoom == 13) return 2;
    return 1;
}

static bool implausibleHydrationCache(const std::string& file, size_t feature_count) {
    // These guards reject previously observed duplicated cache payloads. They are
    // intentionally loose so legitimate source updates still load from cache.
    if (file == "parcel.geojson") return feature_count > 300000;
    if (file == "zoning.geojson") return feature_count > 10000;
    if (file == "vacant_building_notices.geojson") return feature_count > 20000;
    if (file == "vacant_building_rehabs.geojson") return feature_count > 20000;
    return false;
}

static void hydrateLayerBatches(
    const fs::path& full_path,
    size_t batch_size,
    const std::atomic<bool>& stop_flag,
    const std::function<bool()>& should_continue,
    const std::function<void(std::vector<LayerDef::FeatureGeom>&&, bool, bool, const std::string&)>& emit) {
    auto stream_features = [&](std::function<bool(json&&)> on_feature, std::string& err) -> bool {
        std::ifstream in(full_path, std::ios::binary);
        if (!in) {
            err = "failed to open layer file";
            return false;
        }
        std::string token;
        token.reserve(10);
        bool in_string = false;
        bool escape = false;
        bool found_features_key = false;
        bool in_features_array = false;
        bool collecting_feature = false;
        int obj_depth = 0;
        std::string feature_buf;
        feature_buf.reserve(65536);

        char ch = 0;
        while (in.get(ch)) {
            if (!in_features_array) {
                if (!found_features_key) {
                    if (!in_string) {
                        if (ch == '"') {
                            in_string = true;
                            token.clear();
                        }
                    } else {
                        if (escape) {
                            token.push_back(ch);
                            escape = false;
                        } else if (ch == '\\') {
                            escape = true;
                        } else if (ch == '"') {
                            in_string = false;
                            if (token == "features") found_features_key = true;
                        } else {
                            token.push_back(ch);
                        }
                    }
                } else {
                    if (ch == '[') {
                        in_features_array = true;
                    }
                }
                continue;
            }

            if (!collecting_feature) {
                if (ch == '{') {
                    collecting_feature = true;
                    obj_depth = 1;
                    in_string = false;
                    escape = false;
                    feature_buf.clear();
                    feature_buf.push_back(ch);
                } else if (ch == ']') {
                    return true;
                }
                continue;
            }

            feature_buf.push_back(ch);
            if (in_string) {
                if (escape) escape = false;
                else if (ch == '\\') escape = true;
                else if (ch == '"') in_string = false;
                continue;
            }
            if (ch == '"') {
                in_string = true;
                continue;
            }
            if (ch == '{') obj_depth++;
            else if (ch == '}') {
                obj_depth--;
                if (obj_depth == 0) {
                    try {
                        json f = json::parse(feature_buf);
                        if (!on_feature(std::move(f))) return true;
                    } catch (const std::exception& e) {
                        err = std::string("feature parse failed: ") + e.what();
                        return false;
                    }
                    collecting_feature = false;
                }
            }
        }
        err = found_features_key ? "invalid geojson: unterminated features array" : "invalid geojson: missing features array";
        return false;
    };

    std::vector<LayerDef::FeatureGeom> batch;
    batch.reserve(batch_size);
    std::string stream_err;
    bool stream_aborted = false;
    const bool ok = stream_features([&](json&& f) -> bool {
        if (stop_flag.load(std::memory_order_relaxed) || !should_continue()) {
            stream_aborted = true;
            return false;
        }
        if (!f.contains("geometry")) return true;
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
            if (stop_flag.load(std::memory_order_relaxed) || !should_continue()) {
                stream_aborted = true;
                return false;
            }
            g.properties = props;
            batch.push_back(std::move(g));
            if (batch.size() >= batch_size) {
                emit(std::move(batch), false, false, "");
                batch.clear();
                batch.reserve(batch_size);
            }
        }
        return true;
    }, stream_err);
    if (!ok) {
        emit({}, true, true, stream_err.empty() ? "feature streaming failed" : stream_err);
        return;
    }
    if (stream_aborted) return;
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

    uint64_t shot_req_id = 0;
    {
        std::lock_guard<std::mutex> lk(g_ScreenshotState.mutex);
        if (g_ScreenshotState.pending) shot_req_id = g_ScreenshotState.req_id;
    }
    if (shot_req_id == 0) return;

    auto complete_screenshot = [&](bool ok, const std::string& path, const std::string& error) {
        std::lock_guard<std::mutex> lk(g_ScreenshotState.mutex);
        if (g_ScreenshotState.pending && g_ScreenshotState.req_id == shot_req_id) {
            g_ScreenshotState.pending = false;
            g_ScreenshotState.done_id = shot_req_id;
            g_ScreenshotState.ok = ok;
            g_ScreenshotState.path = path;
            g_ScreenshotState.error = error;
            g_ScreenshotState.cv.notify_all();
        }
    };

    check_vk_result(vkWaitForFences(g_Device, 1, &fd->Fence, VK_TRUE, UINT64_MAX));

    VkImage src_image = fd->Backbuffer;
    if (src_image == VK_NULL_HANDLE || wd->Width == 0 || wd->Height == 0) {
        complete_screenshot(false, "", "invalid backbuffer");
        return;
    }

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    const uint32_t width = wd->Width;
    const uint32_t height = wd->Height;
    const VkDeviceSize image_size = (VkDeviceSize)width * (VkDeviceSize)height * 4;
    createBuffer(
        image_size,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        staging,
        staging_mem);

    VkCommandPool cap_pool = VK_NULL_HANDLE;
    VkCommandBuffer cap_cmd = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = g_QueueFamily;
    check_vk_result(vkCreateCommandPool(g_Device, &pool_info, g_Allocator, &cap_pool));
    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = cap_pool;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;
    check_vk_result(vkAllocateCommandBuffers(g_Device, &alloc_info, &cap_cmd));

    VkCommandBufferBeginInfo begin2{};
    begin2.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin2.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk_result(vkBeginCommandBuffer(cap_cmd, &begin2));

    VkImageMemoryBarrier to_src{};
    to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_src.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_src.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.image = src_image;
    to_src.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_src.subresourceRange.levelCount = 1;
    to_src.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(
        cap_cmd,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &to_src);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cap_cmd, src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &region);

    VkImageMemoryBarrier back_to_present{};
    back_to_present.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    back_to_present.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back_to_present.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    back_to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back_to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    back_to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    back_to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    back_to_present.image = src_image;
    back_to_present.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    back_to_present.subresourceRange.levelCount = 1;
    back_to_present.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(
        cap_cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &back_to_present);
    check_vk_result(vkEndCommandBuffer(cap_cmd));

    VkSubmitInfo cap_submit{};
    cap_submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    cap_submit.commandBufferCount = 1;
    cap_submit.pCommandBuffers = &cap_cmd;
    check_vk_result(vkQueueSubmit(g_Queue, 1, &cap_submit, VK_NULL_HANDLE));
    check_vk_result(vkQueueWaitIdle(g_Queue));

    std::string out_path;
    std::string capture_err;
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(g_Device, staging, &req);
    void* mapped = nullptr;
    check_vk_result(vkMapMemory(g_Device, staging_mem, 0, req.size, 0, &mapped));
    const fs::path shot_dir = fs::current_path() / "data" / "cache" / "screenshots";
    std::error_code ec;
    fs::create_directories(shot_dir, ec);
    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count();
    fs::path out_file = shot_dir / ("shot_" + std::to_string(ts) + ".ppm");
    const bool ok = writePpmRgb(
        out_file,
        static_cast<const uint8_t*>(mapped),
        width,
        height,
        (size_t)width * 4,
        wd->SurfaceFormat.format,
        capture_err);
    vkUnmapMemory(g_Device, staging_mem);
    if (ok) out_path = out_file.string();

    vkFreeCommandBuffers(g_Device, cap_pool, 1, &cap_cmd);
    vkDestroyCommandPool(g_Device, cap_pool, g_Allocator);
    vkDestroyBuffer(g_Device, staging, g_Allocator);
    vkFreeMemory(g_Device, staging_mem, g_Allocator);

    complete_screenshot(ok, out_path, capture_err);
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
            std::vector<std::string> present_layers;
            for (const auto& e : manifest) {
                if (!e.contains("file") || !e.contains("url")) continue;
                fs::path out = root / "data" / "layers" / e["file"].get<std::string>();
                if (!fs::exists(out)) missing_layers.push_back({e["url"].get<std::string>(), out});
                else present_layers.push_back(out.filename().string());
            }
            bootstrap.skipped_layers.store(present_layers.size(), std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lk(bootstrap.msg_mutex);
                bootstrap.skipped_layer_files = std::move(present_layers);
            }
            bootstrap.total_items.store(missing_layers.size(), std::memory_order_relaxed);
            bootstrap.done_items.store(0, std::memory_order_relaxed);
            if (!missing_layers.empty()) {
                const unsigned int hw = std::max(1u, std::thread::hardware_concurrency());
                const size_t worker_count = std::min<size_t>(6, std::max<size_t>(2, hw));
                setBootstrapStatus(
                    bootstrap,
                    "Downloading layers in parallel (" + std::to_string(worker_count) + " workers)...");

                std::atomic<size_t> next_idx{0};
                std::atomic<bool> layer_failed{false};
                std::mutex layer_err_mutex;
                std::string layer_err;
                std::vector<std::thread> layer_workers;
                layer_workers.reserve(worker_count);

                for (size_t wi = 0; wi < worker_count; ++wi) {
                    layer_workers.emplace_back([&]() {
                        while (true) {
                            if (layer_failed.load(std::memory_order_relaxed)) break;
                            const size_t idx = next_idx.fetch_add(1, std::memory_order_relaxed);
                            if (idx >= missing_layers.size()) break;
                            std::string err_s;
                            if (!downloadUrlToFile(missing_layers[idx].first, missing_layers[idx].second, err_s)) {
                                const bool first_fail = !layer_failed.exchange(true, std::memory_order_relaxed);
                                if (first_fail) {
                                    std::lock_guard<std::mutex> lk(layer_err_mutex);
                                    layer_err = "Layer download failed (" + missing_layers[idx].second.filename().string() + "): " + err_s;
                                }
                                break;
                            }
                            bootstrap.done_items.fetch_add(1, std::memory_order_relaxed);
                        }
                    });
                }
                for (auto& t : layer_workers) if (t.joinable()) t.join();
                if (layer_failed.load(std::memory_order_relaxed)) {
                    fail(layer_err.empty() ? "Layer download failed." : layer_err);
                    return;
                }
            }

            // 2) Tiles
            bootstrap.phase.store(2, std::memory_order_relaxed);
            constexpr double MIN_LON = -76.72, MIN_LAT = 39.20, MAX_LON = -76.50, MAX_LAT = 39.38;
            constexpr int MIN_ZOOM = 11, MAX_ZOOM = 18;
            std::vector<std::tuple<int, int, int>> missing_tiles;
            size_t total_tiles_in_range = 0;
            for (int z = MIN_ZOOM; z <= MAX_ZOOM; ++z) {
                auto [x0, y1] = deg2num(MIN_LAT, MIN_LON, z);
                auto [x1, y0] = deg2num(MAX_LAT, MAX_LON, z);
                int x_min = std::min(x0, x1), x_max = std::max(x0, x1);
                int y_min = std::min(y0, y1), y_max = std::max(y0, y1);
                for (int x = x_min; x <= x_max; ++x) {
                    for (int y = y_min; y <= y_max; ++y) {
                        total_tiles_in_range++;
                        fs::path out = root / "data" / "tiles" / std::to_string(z) / std::to_string(x) / (std::to_string(y) + ".png");
                        if (!fs::exists(out)) missing_tiles.emplace_back(z, x, y);
                    }
                }
            }
            bootstrap.skipped_tiles.store(total_tiles_in_range - missing_tiles.size(), std::memory_order_relaxed);
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

        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2((float)w, (float)h), ImGuiCond_Always);
        ImGui::Begin(
            "Startup Data Check",
            nullptr,
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBringToFrontOnFocus);

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float margin_x = std::max(24.0f, avail.x * 0.05f);
        const float margin_y = std::max(20.0f, avail.y * 0.05f);
        const float card_w = std::max(640.0f, avail.x - margin_x * 2.0f);
        const float card_h = std::max(280.0f, avail.y - margin_y * 2.0f);
        ImGui::SetCursorPos(ImVec2((avail.x - card_w) * 0.5f, (avail.y - card_h) * 0.5f));
        ImGui::BeginChild("startup_progress_card", ImVec2(card_w, card_h), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
        int ph = bootstrap.phase.load(std::memory_order_relaxed);
        const char* phase_name = ph == 1 ? "Layers" : (ph == 2 ? "Tiles" : (ph == 3 ? "Done" : "Init"));
        size_t done = bootstrap.done_items.load(std::memory_order_relaxed);
        size_t total = bootstrap.total_items.load(std::memory_order_relaxed);
        size_t skipped_layers = bootstrap.skipped_layers.load(std::memory_order_relaxed);
        size_t skipped_tiles = bootstrap.skipped_tiles.load(std::memory_order_relaxed);
        double pct = total ? (100.0 * (double)done / (double)total) : 100.0;
        std::string status;
        {
            std::lock_guard<std::mutex> lk(bootstrap.msg_mutex);
            status = bootstrap.status;
        }
        ImGui::Text("Startup Data Check");
        ImGui::Separator();
        auto draw_step_row = [&](const char* label, bool complete, bool active) {
            ImVec4 bg = ImVec4(0.24f, 0.24f, 0.24f, 0.90f);
            ImVec4 fg = ImVec4(0.88f, 0.88f, 0.88f, 1.0f);
            if (complete) {
                bg = ImVec4(0.18f, 0.47f, 0.24f, 0.96f);
                fg = ImVec4(0.94f, 1.00f, 0.94f, 1.0f);
            } else if (active) {
                bg = ImVec4(0.40f, 0.35f, 0.16f, 0.96f);
                fg = ImVec4(1.00f, 0.98f, 0.84f, 1.0f);
            }
            ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
            const std::string row_id = std::string("startup_step_") + label;
            if (ImGui::BeginChild(row_id.c_str(), ImVec2(-1.0f, 28.0f), ImGuiChildFlags_Border)) {
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, fg);
                ImGui::TextUnformatted(label);
                ImGui::PopStyleColor();
            }
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        };
        draw_step_row("1) Layer Download", ph > 1, ph == 1);
        draw_step_row("2) Tile Download", ph > 2, ph == 2);
        draw_step_row("3) Ready", ph >= 3, ph == 3);
        ImGui::Spacing();
        ImGui::Text("Phase: %s", phase_name);
        ImGui::ProgressBar((float)(total ? ((double)done / (double)total) : 1.0), ImVec2(-1.0f, 12.0f));
        ImGui::Text("%zu / %zu (%.1f%%)", done, total, pct);
        if (ph == 1) ImGui::Text("Already present layers: %zu", skipped_layers);
        if (ph == 2) ImGui::Text("Already present tiles: %zu", skipped_tiles);
        ImGui::Separator();
        ImGui::TextWrapped("%s", status.c_str());
        ImGui::EndChild();
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
    int tax_lien_layer_idx = -1;
    int tax_sale_layer_idx = -1;
    int zoning_layer_idx = -1;
    for (size_t i = 0; i < layers.size(); ++i) {
        if (layers[i].file == "parcel.geojson") parcel_layer_idx = (int)i;
        else if (layers[i].file == "real_property_information.geojson") real_property_layer_idx = (int)i;
        else if (layers[i].file == "vacant_building_notices.geojson") vacant_notice_layer_idx = (int)i;
        else if (layers[i].file == "vacant_building_rehabs.geojson") vacant_rehab_layer_idx = (int)i;
        else if (layers[i].file == "tax_lien_certificate_sale_properties.geojson") tax_lien_layer_idx = (int)i;
        else if (layers[i].file == "tax_sale_list_2021.geojson") tax_sale_layer_idx = (int)i;
        else if (layers[i].file == "zoning.geojson") zoning_layer_idx = (int)i;
    }
    std::unordered_map<std::string, size_t> real_property_by_blocklot;
    std::unordered_map<std::string, int> vacant_notice_count_by_blocklot;
    std::unordered_map<std::string, int> vacant_rehab_count_by_blocklot;
    std::unordered_map<std::string, int> tax_lien_count_by_blocklot;
    std::unordered_map<std::string, double> tax_lien_amount_by_blocklot;
    std::unordered_map<std::string, int> tax_sale_count_by_blocklot;
    std::unordered_map<std::string, double> tax_sale_amount_by_blocklot;
    std::unordered_map<std::string, bool> zoning_zone_enabled;
    std::unordered_map<std::string, ImVec4> zoning_zone_color;
    std::unordered_map<std::string, std::string> zoning_zone_label;
    std::unordered_map<std::string, ZoneMetadata> zoning_metadata = loadZoneMetadata(root);
    std::vector<std::string> zoning_zone_order;
    std::unordered_map<std::string, size_t> zoning_zone_counts;
    std::unordered_map<std::string, std::vector<std::string>> zoning_group_zones;
    std::vector<std::string> zoning_group_order;
    size_t zoning_zone_discovered_feature_count = 0;
    std::vector<int> parcel_vac_notice_by_feature;
    std::vector<int> parcel_vac_rehab_by_feature;
    std::vector<int> parcel_tax_lien_by_feature;
    std::vector<int> parcel_tax_sale_by_feature;
    std::vector<double> parcel_tax_lien_amount_by_feature;
    std::vector<double> parcel_tax_sale_amount_by_feature;
    int vacancy_maps_generation = 0;
    int parcel_vacancy_generation_applied = -1;
    int tax_maps_generation = 0;
    int parcel_tax_generation_applied = -1;
    size_t cached_real_property_size = 0;
    size_t cached_vac_notice_size = 0;
    size_t cached_vac_rehab_size = 0;
    size_t cached_tax_lien_size = 0;
    size_t cached_tax_sale_size = 0;
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
    std::atomic<size_t> render_fill_attempts_last_frame{0};
    std::atomic<size_t> render_fill_success_last_frame{0};
    std::atomic<size_t> render_fill_no_triangles_last_frame{0};
    std::atomic<size_t> render_fill_bad_indices_last_frame{0};
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
    std::unordered_map<std::string, bool> api_layer_fill_cmds;
    std::mutex layer_fill_mutex;
    std::vector<bool> layer_fill_enabled(layers.size(), true);
    std::vector<bool> layer_hover_enabled(layers.size(), true);
    std::vector<bool> layer_inspect_enabled(layers.size(), true);
    bool hover_inspector_enabled = true;
    loadLayerUiState(
        root,
        layers,
        hover_inspector_enabled,
        &zoning_zone_enabled,
        &layer_fill_enabled,
        &layer_hover_enabled,
        &layer_inspect_enabled);
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
    std::vector<bool> hydration_required(layers.size(), false);
    for (size_t i = 0; i < layers.size(); ++i) {
        if (layers[i].enabled) {
            hydrate_requests.push_back(i);
            hydration_requested[i] = true;
        }
    }
    auto is_parcel_priority_index = [&](size_t idx) -> bool {
        if (parcel_layer_idx < 0) return false;
        if ((int)idx == parcel_layer_idx) return true;
        const bool vac_active =
            (vacant_notice_layer_idx >= 0 && layers[(size_t)vacant_notice_layer_idx].enabled) ||
            (vacant_rehab_layer_idx >= 0 && layers[(size_t)vacant_rehab_layer_idx].enabled);
        return vac_active && (int)idx == parcel_layer_idx;
    };
    auto enqueue_hydration = [&](size_t idx, bool required = false) {
        if (idx >= layers.size()) return;
        std::lock_guard<std::mutex> lk(hydrate_req_mutex);
        if (required) hydration_required[idx] = true;
        if (!hydration_requested[idx]) {
            if (is_parcel_priority_index(idx)) hydrate_requests.push_front(idx);
            else hydrate_requests.push_back(idx);
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
                bool required = false;
                {
                    std::lock_guard<std::mutex> lk(hydrate_req_mutex);
                    required = i < hydration_required.size() && hydration_required[i];
                }
                if (i >= layers.size() || (!layers[i].enabled && !required)) continue;
                {
                    std::lock_guard<std::mutex> lk(status_mutex);
                    if (i < layer_states.size()) layer_states[i].status = LayerPipelineStatus::Hydrating;
                }

                const fs::path layer_path = root / "data" / "layers" / layers[i].file;
                const fs::path cache_path = root / "data" / "cache" / "hydration" / (layers[i].file + ".msgpack");
                const std::string sig = fileSignature(layer_path);
                std::vector<LayerDef::FeatureGeom> cached_features;
                if (loadHydrationCache(cache_path, sig, cached_features)) {
                    const bool suspicious_cache =
                        cached_features.empty() ||
                        implausibleHydrationCache(layers[i].file, cached_features.size()) ||
                        (fs::exists(layer_path) &&
                         fs::file_size(layer_path) > 1024 * 1024 &&
                         cached_features.size() < 32);
                    if (!suspicious_cache) {
                        for (size_t off = 0; off < cached_features.size(); off += kHydrationBatchSize) {
                            if (hydration_stop.load(std::memory_order_relaxed) || (!layers[i].enabled && !required)) break;
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
                    } else {
                        std::error_code ec;
                        fs::remove(cache_path, ec);
                    }
                }

                hydrateLayerBatches(
                    layer_path, kHydrationBatchSize, hydration_stop,
                    [&]() { return i < layers.size() && (layers[i].enabled || required); },
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
            } else if (path == "/set_fill") {
                std::string file = get_q("file");
                std::string enabled = get_q("enabled");
                if (!file.empty() && !enabled.empty()) {
                    bool en = (enabled == "1" || enabled == "true" || enabled == "on");
                    std::lock_guard<std::mutex> lk(api_layer_mutex);
                    api_layer_fill_cmds[file] = en;
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
                std::vector<bool> fill_copy;
                {
                    std::lock_guard<std::mutex> lk(layer_fill_mutex);
                    fill_copy = layer_fill_enabled;
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
                out["render_fill"] = {
                    {"attempts_last_frame", render_fill_attempts_last_frame.load(std::memory_order_relaxed)},
                    {"success_last_frame", render_fill_success_last_frame.load(std::memory_order_relaxed)},
                    {"no_triangles_last_frame", render_fill_no_triangles_last_frame.load(std::memory_order_relaxed)},
                    {"bad_indices_last_frame", render_fill_bad_indices_last_frame.load(std::memory_order_relaxed)}
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
                        {"fill_enabled", i < fill_copy.size() ? fill_copy[i] : true},
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
            } else if (path == "/screenshot") {
                uint64_t req_id = 0;
                {
                    std::lock_guard<std::mutex> lk(g_ScreenshotState.mutex);
                    g_ScreenshotState.req_id += 1;
                    req_id = g_ScreenshotState.req_id;
                    g_ScreenshotState.pending = true;
                    g_ScreenshotState.ok = false;
                    g_ScreenshotState.path.clear();
                    g_ScreenshotState.error.clear();
                }
                bool ready = false;
                {
                    std::unique_lock<std::mutex> lk(g_ScreenshotState.mutex);
                    ready = g_ScreenshotState.cv.wait_for(
                        lk,
                        std::chrono::seconds(5),
                        [&]() { return g_ScreenshotState.done_id >= req_id; });
                }
                json out;
                out["supported"] = true;
                out["format"] = "ppm";
                if (!ready) {
                    out["ok"] = false;
                    out["error"] = "timed out waiting for render-thread capture";
                    std::string body = out.dump();
                    std::ostringstream os;
                    os << "HTTP/1.1 504 Gateway Timeout\r\nContent-Type: application/json\r\nContent-Length: " << body.size()
                       << "\r\nConnection: close\r\n\r\n" << body;
                    std::string resp = os.str();
                    (void)writeAll(client_fd, resp.data(), resp.size());
                } else {
                    {
                        std::lock_guard<std::mutex> lk(g_ScreenshotState.mutex);
                        out["ok"] = g_ScreenshotState.ok;
                        out["path"] = g_ScreenshotState.path;
                        out["error"] = g_ScreenshotState.error;
                    }
                    std::string body = out.dump();
                    std::ostringstream os;
                    os << "HTTP/1.1 " << (out["ok"].get<bool>() ? "200 OK" : "500 Internal Server Error")
                       << "\r\nContent-Type: application/json\r\nContent-Length: " << body.size()
                       << "\r\nConnection: close\r\n\r\n" << body;
                    std::string resp = os.str();
                    (void)writeAll(client_fd, resp.data(), resp.size());
                }
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
    double center_lon = -76.6122;
    double center_lat = 39.2904;
    std::vector<bool> last_enabled_state;
    last_enabled_state.reserve(layers.size());
    for (const auto& l : layers) last_enabled_state.push_back(l.enabled);
    bool last_hover_inspector_enabled = hover_inspector_enabled;
    bool show_sources_panel = false;
    bool filter_enabled = false;
    bool filter_use_date = false;
    int filter_year_min = 2000;
    int filter_year_max = 2026;
    char filter_blocklot[64] = "";
    char filter_status[64] = "";
    char filter_address[96] = "";
    char filter_owner[96] = "";
    char filter_zip[24] = "";
    std::vector<int> record_year_hist(201, 0); // 1900..2100
    std::vector<float> record_year_hist_plot(201, 0.0f);
    std::vector<size_t> hist_feature_counts(layers.size(), 0);
    std::vector<bool> hist_enabled(layers.size(), false);
    bool hist_dirty = true;
    float record_year_hist_max_bin = 1.0f;
    int record_year_nonzero_min = 1900;
    int record_year_nonzero_max = 2100;
    int record_year_nonzero_total = 0;
    int selected_record_year = -1;
    bool selected_record_year_dirty = true;
    int selected_record_year_total = 0;
    std::vector<std::string> selected_record_year_samples;
    bool show_selected_parcel_details = false;
    size_t selected_parcel_idx = (size_t)-1;
    bool show_selected_zone_details = false;
    size_t selected_zone_idx = (size_t)-1;
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
        bool layer_fill_state_changed = false;
        bool layer_hover_state_changed = false;
        bool layer_inspect_state_changed = false;
        // Apply REST control commands on main thread.
        {
            int zc = api_zoom_cmd.exchange(-1, std::memory_order_relaxed);
            if (zc >= kMinZoom && zc <= kMaxZoom) zoom = zc;
            double lonc = api_lon_cmd.exchange(std::numeric_limits<double>::quiet_NaN(), std::memory_order_relaxed);
            double latc = api_lat_cmd.exchange(std::numeric_limits<double>::quiet_NaN(), std::memory_order_relaxed);
            if (!std::isnan(lonc)) center_lon = lonc;
            if (!std::isnan(latc)) center_lat = std::clamp(latc, -85.0, 85.0);
            std::lock_guard<std::mutex> lk(api_layer_mutex);
            for (const auto& kv : api_layer_enable_cmds) {
                for (auto& l : layers) {
                    if (l.file == kv.first) l.enabled = kv.second;
                }
            }
            api_layer_enable_cmds.clear();
            if (!api_layer_fill_cmds.empty()) {
                std::lock_guard<std::mutex> lk_fill(layer_fill_mutex);
                for (const auto& kv : api_layer_fill_cmds) {
                    for (size_t i = 0; i < layers.size(); ++i) {
                        if (layers[i].file == kv.first && i < layer_fill_enabled.size()) {
                            if (layer_fill_enabled[i] != kv.second) {
                                layer_fill_enabled[i] = kv.second;
                                layer_fill_state_changed = true;
                            }
                        }
                    }
                }
                api_layer_fill_cmds.clear();
            }
        }
        current_zoom_state.store(zoom, std::memory_order_relaxed);
        current_lon_state.store(center_lon, std::memory_order_relaxed);
        current_lat_state.store(center_lat, std::memory_order_relaxed);

        ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(420, 760), ImGuiCond_Always);
        ImGui::Begin("Layers and Controls");
        if (ImGui::Button("Gear")) show_sources_panel = !show_sources_panel;
        ImGui::SameLine();
        ImGui::Text("Vulkan map + Vulkan UI");
        ImGui::SliderInt("Zoom", &zoom, kMinZoom, kMaxZoom);
        const double lon_min = -76.75, lon_max = -76.45;
        const double lat_min = 39.18, lat_max = 39.40;
        ImGui::SliderScalar("Center Lon", ImGuiDataType_Double, &center_lon, &lon_min, &lon_max, "%.6f");
        ImGui::SliderScalar("Center Lat", ImGuiDataType_Double, &center_lat, &lat_min, &lat_max, "%.6f");
        ImGui::Checkbox("Enable Hover Inspector", &hover_inspector_enabled);
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
                ImGui::PushID((int)idx);
                auto icon_toggle = [&](const char* id, const char* icon, bool& value, const char* tip) -> bool {
                    ImGui::PushID(id);
                    if (value) {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.45f, 0.78f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.55f, 0.92f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.12f, 0.34f, 0.62f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                    } else {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.16f, 0.16f, 0.22f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.32f, 0.32f, 0.32f, 0.55f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.42f, 0.42f, 0.42f, 0.75f));
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.55f, 1.0f));
                    }
                    bool changed = false;
                    if (ImGui::SmallButton(icon)) {
                        value = !value;
                        changed = true;
                    }
                    ImGui::PopStyleColor(4);
                    if (ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        ImGui::TextUnformatted(tip);
                        ImGui::TextDisabled("%s", value ? "Enabled" : "Disabled");
                        ImGui::EndTooltip();
                    }
                    ImGui::PopID();
                    ImGui::SameLine();
                    return changed;
                };

                icon_toggle("show", "V", l.enabled, "Show layer");
                bool fill_flag = (idx < layer_fill_enabled.size()) ? layer_fill_enabled[idx] : true;
                if (icon_toggle("fill", "F", fill_flag, "Fill polygons") && idx < layer_fill_enabled.size()) {
                    std::lock_guard<std::mutex> lk_fill(layer_fill_mutex);
                    layer_fill_enabled[idx] = fill_flag;
                    layer_fill_state_changed = true;
                }
                bool hover_flag = (idx < layer_hover_enabled.size()) ? layer_hover_enabled[idx] : true;
                if (icon_toggle("hover", "H", hover_flag, "Hover inspector") && idx < layer_hover_enabled.size()) {
                    layer_hover_enabled[idx] = hover_flag;
                    layer_hover_state_changed = true;
                }
                bool inspect_flag = (idx < layer_inspect_enabled.size()) ? layer_inspect_enabled[idx] : true;
                if (icon_toggle("inspect", "I", inspect_flag, "Click to inspect") && idx < layer_inspect_enabled.size()) {
                    layer_inspect_enabled[idx] = inspect_flag;
                    layer_inspect_state_changed = true;
                }
                ImGui::PushStyleColor(ImGuiCol_Text, l.color);
                ImGui::TextUnformatted(l.name.c_str());
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
                ImGui::PopID();
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
	                            if (ImGui::IsItemHovered()) {
	                                ImGui::BeginTooltip();
	                                ImGui::TextUnformatted(zkey.c_str());
	                                auto mit = zoning_metadata.find(zkey);
	                                if (mit != zoning_metadata.end()) {
	                                    if (!mit->second.label.empty()) ImGui::TextWrapped("%s", mit->second.label.c_str());
	                                    if (!mit->second.description.empty()) {
	                                        ImGui::Separator();
	                                        ImGui::TextWrapped("%s", mit->second.description.c_str());
	                                    }
	                                }
	                                ImGui::EndTooltip();
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

        if (show_sources_panel) {
            ImGui::SetNextWindowSize(ImVec2(540, 420), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Gear Panel", &show_sources_panel, ImGuiWindowFlags_NoCollapse)) {
                if (ImGui::BeginTabBar("gear_tabs")) {
                    if (ImGui::BeginTabItem("Sources")) {
                        std::vector<std::string> past_work;
                        std::vector<std::string> future_work;
                        std::vector<std::string> skipped_layer_files;
                        std::string todo_text = readTextFile(root / "TODO.md");
                        collectTodoWork(todo_text, past_work, future_work);
                        {
                            std::lock_guard<std::mutex> lk(bootstrap.msg_mutex);
                            skipped_layer_files = bootstrap.skipped_layer_files;
                        }
                        size_t skipped_layers = bootstrap.skipped_layers.load(std::memory_order_relaxed);
                        size_t skipped_tiles = bootstrap.skipped_tiles.load(std::memory_order_relaxed);
                        ImGui::Text("Past: %zu", past_work.size());
                        ImGui::SameLine();
                        ImGui::Text("Future: %zu", future_work.size());
                        ImGui::SameLine();
                        ImGui::Text("Skipped: %zuL/%zuT", skipped_layers, skipped_tiles);
                        ImGui::Separator();
                        if (ImGui::BeginTabBar("work_tabs")) {
                            if (ImGui::BeginTabItem("Past Work")) {
                                ImGui::BeginChild("past_work_scroll", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
                                if (past_work.empty()) ImGui::TextDisabled("No completed checklist items found in TODO.md");
                                else for (const auto& item : past_work) ImGui::BulletText("%s", item.c_str());
                                ImGui::EndChild();
                                ImGui::EndTabItem();
                            }
                            if (ImGui::BeginTabItem("Future Work")) {
                                ImGui::BeginChild("future_work_scroll", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
                                if (future_work.empty()) ImGui::TextDisabled("No pending items found in TODO.md");
                                else for (const auto& item : future_work) ImGui::BulletText("%s", item.c_str());
                                ImGui::EndChild();
                                ImGui::EndTabItem();
                            }
                            if (ImGui::BeginTabItem("Skipped This Run")) {
                                ImGui::BeginChild("skipped_work_scroll", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
                                ImGui::Text("Layers already present (not downloaded this run): %zu", skipped_layers);
                                ImGui::Text("Tiles already present (not downloaded this run): %zu", skipped_tiles);
                                ImGui::Separator();
                                if (skipped_layer_files.empty()) ImGui::TextDisabled("No skipped layers recorded for this run.");
                                else {
                                    ImGui::TextUnformatted("Skipped layer files:");
                                    for (const auto& f : skipped_layer_files) ImGui::BulletText("%s", f.c_str());
                                }
                                ImGui::EndChild();
                                ImGui::EndTabItem();
                            }
                            ImGui::EndTabBar();
                        }
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
            }
            ImGui::End();
        }
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(12, 784), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(420, (float)h - 796.0f), ImGuiCond_Always);
        ImGui::Begin("Performance and Stats", nullptr, ImGuiWindowFlags_NoCollapse);
        ImGui::Text("Hydration: %zu / %zu (%.1f%%)", hydrated_now, layers.size(), hydrated_frac * 100.0f);
        ImGui::ProgressBar(hydrated_frac, ImVec2(-1.0f, 0.0f));
        ImGui::Text("Triangulation: %zu / %zu (%.1f%%)", triangulated_now, layers.size(), tri_frac * 100.0f);
        ImGui::ProgressBar(tri_frac, ImVec2(-1.0f, 0.0f));
        ImGui::TextDisabled("Hydration queue: %zu | Tri queue: %zu | elapsed: %.1fs", hydrated_pending, tri_pending, elapsed_s);
        ImGui::TextDisabled("Frame: %.2f ms (last %.2f) | FPS: %.1f",
                            perf_frame_ms_avg.load(std::memory_order_relaxed),
                            perf_frame_ms_last.load(std::memory_order_relaxed),
                            perf_fps_avg.load(std::memory_order_relaxed));
        ImGui::TextDisabled("Fill: %zu ok / %zu attempts | no tris %zu | bad idx %zu",
                            render_fill_success_last_frame.load(std::memory_order_relaxed),
                            render_fill_attempts_last_frame.load(std::memory_order_relaxed),
                            render_fill_no_triangles_last_frame.load(std::memory_order_relaxed),
                            render_fill_bad_indices_last_frame.load(std::memory_order_relaxed));
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
                std::fill(hydration_required.begin(), hydration_required.end(), false);
            }

            hydrated_count.store(0, std::memory_order_relaxed);
            triangulated_count.store(0, std::memory_order_relaxed);
            cached_real_property_size = 0;
            cached_vac_notice_size = 0;
            cached_vac_rehab_size = 0;
            cached_tax_lien_size = 0;
            cached_tax_sale_size = 0;
            vacancy_maps_generation = 0;
            parcel_vacancy_generation_applied = -1;
            tax_maps_generation = 0;
            parcel_tax_generation_applied = -1;
            parcel_vac_notice_by_feature.clear();
            parcel_vac_rehab_by_feature.clear();
            parcel_tax_lien_by_feature.clear();
            parcel_tax_sale_by_feature.clear();
            parcel_tax_lien_amount_by_feature.clear();
            parcel_tax_sale_amount_by_feature.clear();
            vacant_notice_count_by_blocklot.clear();
            vacant_rehab_count_by_blocklot.clear();
            tax_lien_count_by_blocklot.clear();
            tax_lien_amount_by_blocklot.clear();
            tax_sale_count_by_blocklot.clear();
            tax_sale_amount_by_blocklot.clear();
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
        bool ui_state_changed =
            (hover_inspector_enabled != last_hover_inspector_enabled) ||
            zoning_filters_changed ||
            layer_fill_state_changed ||
            layer_hover_state_changed ||
            layer_inspect_state_changed;
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
            if (!parcel_ready) {
                enqueue_hydration((size_t)parcel_layer_idx, true);
            }
        }
        if (real_property_layer_idx >= 0 && (size_t)real_property_layer_idx < layers.size()) {
            bool real_property_ready = false;
            {
                std::lock_guard<std::mutex> lk(status_mutex);
                if ((size_t)real_property_layer_idx < layer_states.size()) {
                    LayerPipelineStatus st = layer_states[(size_t)real_property_layer_idx].status;
                    real_property_ready = (st == LayerPipelineStatus::Hydrated ||
                                           st == LayerPipelineStatus::TriQueued ||
                                           st == LayerPipelineStatus::Triangulating ||
                                           st == LayerPipelineStatus::Ready);
                }
            }
            const bool filter_join_needed =
                filter_enabled ||
                (parcel_layer_idx >= 0 && layers[(size_t)parcel_layer_idx].enabled) ||
                vacant_layer_active ||
                filter_owner[0] != '\0' ||
                filter_address[0] != '\0' ||
                filter_zip[0] != '\0';
            if (filter_join_needed && !real_property_ready) {
                enqueue_hydration((size_t)real_property_layer_idx, true);
            }
        }
        const bool tax_layer_active =
            (tax_lien_layer_idx >= 0 && layers[(size_t)tax_lien_layer_idx].enabled) ||
            (tax_sale_layer_idx >= 0 && layers[(size_t)tax_sale_layer_idx].enabled);
        if (tax_layer_active && parcel_layer_idx >= 0) {
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
            if (!parcel_ready) enqueue_hydration((size_t)parcel_layer_idx, true);
        }
        if (zoning_layer_idx >= 0 && (size_t)zoning_layer_idx < layers.size()) {
            bool zoning_ready = false;
            {
                std::lock_guard<std::mutex> lk(status_mutex);
                if ((size_t)zoning_layer_idx < layer_states.size()) {
                    LayerPipelineStatus st = layer_states[(size_t)zoning_layer_idx].status;
                    zoning_ready = (st == LayerPipelineStatus::Hydrated ||
                                    st == LayerPipelineStatus::TriQueued ||
                                    st == LayerPipelineStatus::Triangulating ||
                                    st == LayerPipelineStatus::Ready);
                }
            }
            if (layers[(size_t)zoning_layer_idx].enabled && !zoning_ready) {
                enqueue_hydration((size_t)zoning_layer_idx);
            }
        }
        if (ui_state_changed) {
            saveLayerUiState(
                root,
                layers,
                hover_inspector_enabled,
                &zoning_zone_enabled,
                &layer_fill_enabled,
                &layer_hover_enabled,
                &layer_inspect_enabled);
            last_enabled_state.clear();
            last_enabled_state.reserve(layers.size());
            for (const auto& l : layers) last_enabled_state.push_back(l.enabled);
            last_hover_inspector_enabled = hover_inspector_enabled;
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
                    const bool parcel_dep_priority =
                        vacant_layer_active && parcel_layer_idx >= 0 && (int)ready.index == parcel_layer_idx;
                    if (layers[ready.index].enabled || parcel_dep_priority) {
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
                std::unordered_set<std::string> seen_zone_keys;
                seen_zone_keys.reserve(zfeats.size() / 4 + 16);
                for (const auto& fg : zfeats) {
                    std::string zkey = zoningClassKey(fg);
                    std::string zlabel = zoningClassLabel(fg);
                    zoning_zone_counts[zkey] += 1;
                    if (seen_zone_keys.insert(zkey).second) zoning_zone_order.push_back(zkey);
                    if (zoning_zone_enabled.find(zkey) == zoning_zone_enabled.end()) {
                        auto it_prev = prev_enabled.find(zkey);
                        zoning_zone_enabled[zkey] = (it_prev == prev_enabled.end()) ? true : it_prev->second;
                    }
                    auto meta_it = zoning_metadata.find(zkey);
                    if (zoning_zone_label.find(zkey) == zoning_zone_label.end()) {
                        zoning_zone_label[zkey] =
                            (meta_it != zoning_metadata.end() && !meta_it->second.label.empty()) ? meta_it->second.label : zlabel;
                    }
                    if (zoning_zone_color.find(zkey) == zoning_zone_color.end()) {
                        zoning_zone_color[zkey] =
                            (meta_it != zoning_metadata.end() && meta_it->second.has_color) ? meta_it->second.color : colorFromStableKey(zkey);
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
                    std::string bl = featureBlockLotJoinKey(feats[i]);
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
        if (tax_lien_layer_idx >= 0) {
            const auto& feats = layers[(size_t)tax_lien_layer_idx].features;
            if (feats.size() != cached_tax_lien_size) {
                tax_lien_count_by_blocklot.clear();
                tax_lien_amount_by_blocklot.clear();
                for (const auto& fg : feats) {
                    std::string bl = featureBlockLotJoinKey(fg);
                    if (bl.empty()) continue;
                    tax_lien_count_by_blocklot[bl] += 1;
                    tax_lien_amount_by_blocklot[bl] += parseNumericField(getPropertyValue(fg, "TOTAL_AMOUNT"));
                }
                cached_tax_lien_size = feats.size();
                tax_maps_generation += 1;
            }
        }
        if (tax_sale_layer_idx >= 0) {
            const auto& feats = layers[(size_t)tax_sale_layer_idx].features;
            if (feats.size() != cached_tax_sale_size) {
                tax_sale_count_by_blocklot.clear();
                tax_sale_amount_by_blocklot.clear();
                for (const auto& fg : feats) {
                    std::string bl = featureBlockLotJoinKey(fg);
                    if (bl.empty()) continue;
                    tax_sale_count_by_blocklot[bl] += 1;
                    double amount = parseNumericField(getPropertyValue(fg, "total_lien"));
                    if (amount <= 0.0) amount = parseNumericField(getPropertyValue(fg, "total_3yea"));
                    if (amount <= 0.0) amount = parseNumericField(getPropertyValue(fg, "total_tax"));
                    tax_sale_amount_by_blocklot[bl] += amount;
                }
                cached_tax_sale_size = feats.size();
                tax_maps_generation += 1;
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
            if (parcel_tax_lien_by_feature.size() != pfeats.size() ||
                parcel_tax_sale_by_feature.size() != pfeats.size() ||
                parcel_tax_generation_applied != tax_maps_generation) {
                parcel_tax_lien_by_feature.assign(pfeats.size(), 0);
                parcel_tax_sale_by_feature.assign(pfeats.size(), 0);
                parcel_tax_lien_amount_by_feature.assign(pfeats.size(), 0.0);
                parcel_tax_sale_amount_by_feature.assign(pfeats.size(), 0.0);
                for (size_t i = 0; i < pfeats.size(); ++i) {
                    std::string bl = featureBlockLotJoinKey(pfeats[i]);
                    auto it_lien = tax_lien_count_by_blocklot.find(bl);
                    if (it_lien != tax_lien_count_by_blocklot.end()) {
                        parcel_tax_lien_by_feature[i] = it_lien->second;
                        auto it_amt = tax_lien_amount_by_blocklot.find(bl);
                        if (it_amt != tax_lien_amount_by_blocklot.end()) parcel_tax_lien_amount_by_feature[i] = it_amt->second;
                    }
                    auto it_sale = tax_sale_count_by_blocklot.find(bl);
                    if (it_sale != tax_sale_count_by_blocklot.end()) {
                        parcel_tax_sale_by_feature[i] = it_sale->second;
                        auto it_amt = tax_sale_amount_by_blocklot.find(bl);
                        if (it_amt != tax_sale_amount_by_blocklot.end()) parcel_tax_sale_amount_by_feature[i] = it_amt->second;
                    }
                }
                parcel_tax_generation_applied = tax_maps_generation;
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
                layer_geom_cache[li].world_extent_by_feature.clear();
            }
        }

        auto real_property_for_parcel = [&](const LayerDef::FeatureGeom& parcel) -> const LayerDef::FeatureGeom* {
            if (real_property_layer_idx < 0 || (size_t)real_property_layer_idx >= layers.size()) return nullptr;
            std::string blocklot = featureBlockLotJoinKey(parcel);
            if (blocklot.empty()) return nullptr;
            auto itrp = real_property_by_blocklot.find(blocklot);
            if (itrp == real_property_by_blocklot.end()) return nullptr;
            const auto& rp_layer = layers[(size_t)real_property_layer_idx];
            if (itrp->second >= rp_layer.features.size()) return nullptr;
            return &rp_layer.features[itrp->second];
        };
        auto text_prop = [](const char* label, const std::string& value) {
            if (!value.empty()) ImGui::TextWrapped("%s: %s", label, value.c_str());
        };
        auto money_prop = [](const char* label, const std::string& value) {
            if (value.empty()) return;
            const double amount = parseNumericField(value);
            if (amount > 0.0) ImGui::TextWrapped("%s: $%.2f", label, amount);
            else ImGui::TextWrapped("%s: %s", label, value.c_str());
        };
        auto draw_real_property_summary = [&](const LayerDef::FeatureGeom* rp) {
            ImGui::Separator();
            ImGui::TextUnformatted("Ownership / Assessment");
            if (!rp) {
                ImGui::TextDisabled("No matching Real Property Information record loaded for this parcel.");
                ImGui::TextDisabled("Source expected: data/layers/real_property_information.geojson");
                return;
            }
            text_prop("Owner 1", firstDisplayProperty(*rp, {"OWNER_1", "OWNERNME1", "OWNER", "OWNER_NAME"}));
            text_prop("Owner 2", firstDisplayProperty(*rp, {"OWNER_2"}));
            text_prop("Owner 3", firstDisplayProperty(*rp, {"OWNER_3"}));
            text_prop("Owner Abbrev", firstDisplayProperty(*rp, {"OWNER_ABBR"}));
            text_prop("Mailing Address", firstDisplayProperty(*rp, {"MAILTOADD"}));
            text_prop("Property Address", firstDisplayProperty(*rp, {"FULLADDR", "PROPERTY_ADDRESS", "PREMISEADD", "ADDRESS"}));
            text_prop("Use Group", firstDisplayProperty(*rp, {"USEGROUP"}));
            text_prop("DHCD Use", firstDisplayProperty(*rp, {"DHCDUSE1"}));
            text_prop("Zone Code", firstDisplayProperty(*rp, {"ZONECODE"}));
            text_prop("Year Built", firstDisplayProperty(*rp, {"YEAR_BUILD"}));
            text_prop("Lot Size", firstDisplayProperty(*rp, {"LOT_SIZE"}));
            text_prop("Structure Area", firstDisplayProperty(*rp, {"STRUCTAREA"}));
            money_prop("Current Land", firstDisplayProperty(*rp, {"CURRLAND"}));
            money_prop("Current Improvements", firstDisplayProperty(*rp, {"CURRIMPR"}));
            money_prop("Tax Base", firstDisplayProperty(*rp, {"TAXBASE", "ARTAXBAS"}));
            money_prop("City Tax", firstDisplayProperty(*rp, {"CITY_TAX"}));
            money_prop("State Tax", firstDisplayProperty(*rp, {"STATETAX"}));
            money_prop("Sale Price", firstDisplayProperty(*rp, {"SALEPRIC"}));
            text_prop("Sale Date", firstDisplayProperty(*rp, {"SALEDATE"}));
            const std::string deed_book = firstDisplayProperty(*rp, {"DEEDBOOK"});
            const std::string deed_page = firstDisplayProperty(*rp, {"DEEDPAGE"});
            text_prop("Deed", deed_book.empty() ? "" : deed_book + (deed_page.empty() ? "" : " / " + deed_page));
            text_prop("SDAT Link", firstDisplayProperty(*rp, {"SDATLINK"}));
            ImGui::TextDisabled("Source: Baltimore Real Property Information (data/layers/real_property_information.geojson)");
        };
        auto draw_feature_properties = [&](const char* title, const LayerDef::FeatureGeom& fg) {
            ImGui::TextUnformatted(title);
            for (const auto& kv : fg.properties) {
                std::string v = trimDisplayValue(kv.second);
                if (v.empty()) continue;
                ImGui::TextWrapped("%s: %s", kv.first.c_str(), v.c_str());
            }
        };

        const float right_panel_w = 360.0f;
        ImGui::SetNextWindowPos(ImVec2((float)w - right_panel_w - 12.0f, 12.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(right_panel_w, (float)h - 24.0f), ImGuiCond_Always);
        ImGui::Begin("Record Filters", nullptr, ImGuiWindowFlags_NoCollapse);
        const bool selected_valid =
            show_selected_parcel_details && parcel_layer_idx >= 0 &&
            (size_t)parcel_layer_idx < layers.size() &&
            selected_parcel_idx < layers[(size_t)parcel_layer_idx].features.size();
        const bool selected_zone_valid =
            show_selected_zone_details && zoning_layer_idx >= 0 &&
            (size_t)zoning_layer_idx < layers.size() &&
            selected_zone_idx < layers[(size_t)zoning_layer_idx].features.size();
        if (selected_valid) {
            const auto& selected = layers[(size_t)parcel_layer_idx].features[selected_parcel_idx];
            if (ImGui::Button("Back To Filters")) {
                show_selected_parcel_details = false;
                selected_parcel_idx = (size_t)-1;
            }
            ImGui::Separator();
            std::string blocklot_raw = getPropertyValue(selected, "BLOCKLOT");
            std::string blocklot = normalizeJoinKey(blocklot_raw);
            int vac_notice = (selected_parcel_idx < parcel_vac_notice_by_feature.size()) ? parcel_vac_notice_by_feature[selected_parcel_idx] : 0;
            int vac_rehab = (selected_parcel_idx < parcel_vac_rehab_by_feature.size()) ? parcel_vac_rehab_by_feature[selected_parcel_idx] : 0;
            int tax_lien = (selected_parcel_idx < parcel_tax_lien_by_feature.size()) ? parcel_tax_lien_by_feature[selected_parcel_idx] : 0;
            int tax_sale = (selected_parcel_idx < parcel_tax_sale_by_feature.size()) ? parcel_tax_sale_by_feature[selected_parcel_idx] : 0;
            double tax_lien_amount = (selected_parcel_idx < parcel_tax_lien_amount_by_feature.size()) ? parcel_tax_lien_amount_by_feature[selected_parcel_idx] : 0.0;
            double tax_sale_amount = (selected_parcel_idx < parcel_tax_sale_amount_by_feature.size()) ? parcel_tax_sale_amount_by_feature[selected_parcel_idx] : 0.0;
            ImGui::TextUnformatted("Parcel Details");
            ImGui::Separator();
            ImGui::Text("BLOCKLOT: %s", blocklot_raw.empty() ? "(none)" : blocklot_raw.c_str());
            ImGui::Text("Vacant Notices: %d", vac_notice);
            ImGui::Text("Vacant Rehab Records: %d", vac_rehab);
            ImGui::Text("Tax Lien Certificate Records: %d", tax_lien);
            if (tax_lien > 0) ImGui::Text("Tax Lien Total Amount: $%.2f", tax_lien_amount);
            ImGui::Text("Tax Sale 2021 Records: %d", tax_sale);
            if (tax_sale > 0) ImGui::Text("Tax Sale Total Lien: $%.2f", tax_sale_amount);
            const LayerDef::FeatureGeom* selected_rp = real_property_for_parcel(selected);
            draw_real_property_summary(selected_rp);
            ImGui::Separator();
            ImGui::BeginChild("selected_parcel_fields", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
            draw_feature_properties("All Parcel Geometry Fields", selected);
            if (selected_rp) {
                ImGui::Separator();
                draw_feature_properties("All Real Property Fields", *selected_rp);
            }
            ImGui::EndChild();
        } else if (selected_zone_valid) {
            const auto& selected = layers[(size_t)zoning_layer_idx].features[selected_zone_idx];
            if (ImGui::Button("Back To Filters")) {
                show_selected_zone_details = false;
                selected_zone_idx = (size_t)-1;
            }
            ImGui::Separator();
            std::string zone_key = zoningClassKey(selected);
            std::string zone_label = zoningClassLabel(selected);
            std::string zone_description;
            auto meta_it = zoning_metadata.find(zone_key);
            if (meta_it != zoning_metadata.end()) {
                if (!meta_it->second.label.empty()) zone_label = meta_it->second.label;
                zone_description = meta_it->second.description;
            }
            ImVec4 zone_color = colorFromStableKey(zone_key);
            auto it_col = zoning_zone_color.find(zone_key);
            if (it_col != zoning_zone_color.end()) zone_color = it_col->second;
            ImGui::TextUnformatted("Zoning Details");
            ImGui::Separator();
            ImGui::ColorButton("##selected_zone_color", zone_color, ImGuiColorEditFlags_NoTooltip, ImVec2(18, 18));
            ImGui::SameLine();
            ImGui::Text("Zone: %s", zone_key.empty() ? "(unlabeled)" : zone_key.c_str());
            if (!zone_label.empty() && zone_label != zone_key) {
                ImGui::TextWrapped("Label: %s", zone_label.c_str());
            }
            ImGui::TextWrapped("Description: %s", zone_description.empty() ? "No description available." : zone_description.c_str());
            ImGui::Separator();
            ImGui::TextUnformatted("All Zone Fields");
            ImGui::BeginChild("selected_zone_fields", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
            for (const auto& kv : selected.properties) {
                if (kv.second.empty()) continue;
                ImGui::TextWrapped("%s: %s", kv.first.c_str(), kv.second.c_str());
            }
            ImGui::EndChild();
        } else {
            if (show_selected_parcel_details && !selected_valid) {
                show_selected_parcel_details = false;
                selected_parcel_idx = (size_t)-1;
            }
            if (show_selected_zone_details && !selected_zone_valid) {
                show_selected_zone_details = false;
                selected_zone_idx = (size_t)-1;
            }
            ImGui::Checkbox("Enable Filters", &filter_enabled);
            ImGui::Checkbox("Filter By Record Date", &filter_use_date);
            ImGui::BeginDisabled(!filter_enabled || !filter_use_date);
            ImGui::SliderInt("Year Min", &filter_year_min, 1900, 2100);
            ImGui::SliderInt("Year Max", &filter_year_max, 1900, 2100);
            if (filter_year_min > filter_year_max) std::swap(filter_year_min, filter_year_max);
            ImGui::EndDisabled();
            ImGui::SeparatorText("Common Fields");
            ImGui::InputText("Block/Lot", filter_blocklot, sizeof(filter_blocklot));
            ImGui::InputText("Status", filter_status, sizeof(filter_status));
            ImGui::InputText("Address", filter_address, sizeof(filter_address));
            ImGui::InputText("Owner", filter_owner, sizeof(filter_owner));
            ImGui::InputText("ZIP", filter_zip, sizeof(filter_zip));
            if (ImGui::Button("Clear Field Filters")) {
                filter_blocklot[0] = '\0';
                filter_status[0] = '\0';
                filter_address[0] = '\0';
                filter_owner[0] = '\0';
                filter_zip[0] = '\0';
                filter_use_date = false;
            }
            ImGui::SeparatorText("Record Year Histogram");
            auto first_prop_hist = [&](const LayerDef::FeatureGeom& fg, std::initializer_list<const char*> keys) {
                for (const char* k : keys) {
                    std::string v = getPropertyValue(fg, k);
                    if (!v.empty()) return v;
                }
                return std::string();
            };
            if (hist_feature_counts.size() != layers.size()) {
                hist_feature_counts.assign(layers.size(), 0);
                hist_enabled.assign(layers.size(), false);
                hist_dirty = true;
            }
            for (size_t i = 0; i < layers.size(); ++i) {
                const size_t fc = layers[i].features.size();
                if (hist_feature_counts[i] != fc || hist_enabled[i] != layers[i].enabled) {
                    hist_feature_counts[i] = fc;
                    hist_enabled[i] = layers[i].enabled;
                    hist_dirty = true;
                }
            }
            if (hist_dirty) {
                std::fill(record_year_hist.begin(), record_year_hist.end(), 0);
                for (size_t li = 0; li < layers.size(); ++li) {
                    if (!layers[li].enabled) continue;
                    for (const auto& fg : layers[li].features) {
                        std::string ds = first_prop_hist(fg, {"RECORD_DATE", "RECORDDATE", "DATE", "CREATED_DATE", "ISSUE_DATE", "DateNotice", "DateIssue", "DateIssued", "DateCancel", "DateAbate"});
                        if (ds.empty()) continue;
                        int y = extractYearMaybe(ds);
                        if (y < 1900 || y > 2100) continue;
                        record_year_hist[(size_t)(y - 1900)]++;
                    }
                }
                float max_bin = 1.0f;
                int nz_min = 2101;
                int nz_max = 1899;
                int nz_total = 0;
                for (size_t i = 0; i < record_year_hist.size(); ++i) {
                    record_year_hist_plot[i] = (float)record_year_hist[i];
                    if (record_year_hist[i] > 0) {
                        int y = 1900 + (int)i;
                        nz_min = std::min(nz_min, y);
                        nz_max = std::max(nz_max, y);
                        nz_total += record_year_hist[i];
                    }
                    if (record_year_hist_plot[i] > max_bin) max_bin = record_year_hist_plot[i];
                }
                record_year_hist_max_bin = max_bin;
                if (nz_min <= nz_max) {
                    record_year_nonzero_min = nz_min;
                    record_year_nonzero_max = nz_max;
                } else {
                    record_year_nonzero_min = 1900;
                    record_year_nonzero_max = 2100;
                }
                record_year_nonzero_total = nz_total;
                selected_record_year_dirty = true;
                hist_dirty = false;
            }
            ImGui::TextDisabled("Enabled-layer records by year");
            if (record_year_nonzero_total <= 0) {
                ImGui::TextDisabled("No recognized date fields found in currently enabled layers.");
            } else {
                ImGui::Text("Range: %d-%d  Total: %d  Peak: %.0f",
                            record_year_nonzero_min,
                            record_year_nonzero_max,
                            record_year_nonzero_total,
                            record_year_hist_max_bin);
                const int plot_offset = std::max(0, record_year_nonzero_min - 1900);
                const int plot_count = std::max(1, record_year_nonzero_max - record_year_nonzero_min + 1);
                ImGui::PlotHistogram(
                    "##record_year_hist",
                    record_year_hist_plot.data() + plot_offset,
                    plot_count,
                    0,
                    nullptr,
                    0.0f,
                    record_year_hist_max_bin * 1.05f,
                    ImVec2(-1.0f, 140.0f));
            }
            ImGui::BeginChild("year_hist_list", ImVec2(0, 130), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
            for (int y = record_year_nonzero_min; y <= record_year_nonzero_max; ++y) {
                int c = record_year_hist[(size_t)(y - 1900)];
                if (c <= 0) continue;
                char label[64];
                std::snprintf(label, sizeof(label), "%d: %d records", y, c);
                if (ImGui::Selectable(label, selected_record_year == y)) {
                    selected_record_year = y;
                    selected_record_year_dirty = true;
                }
            }
            ImGui::EndChild();
            if (selected_record_year >= 1900 && selected_record_year <= 2100) {
                if (record_year_hist[(size_t)(selected_record_year - 1900)] <= 0) {
                    selected_record_year = -1;
                    selected_record_year_samples.clear();
                    selected_record_year_total = 0;
                    selected_record_year_dirty = false;
                }
            }
            if (selected_record_year >= 1900 && selected_record_year <= 2100 && selected_record_year_dirty) {
                constexpr size_t kMaxYearSamples = 8;
                selected_record_year_samples.clear();
                selected_record_year_total = 0;
                for (size_t li = 0; li < layers.size(); ++li) {
                    if (!layers[li].enabled) continue;
                    for (const auto& fg : layers[li].features) {
                        std::string ds = first_prop_hist(fg, {"RECORD_DATE", "RECORDDATE", "DATE", "CREATED_DATE", "ISSUE_DATE", "DateNotice", "DateIssue", "DateIssued", "DateCancel", "DateAbate"});
                        if (ds.empty() || extractYearMaybe(ds) != selected_record_year) continue;
                        selected_record_year_total++;
                        if (selected_record_year_samples.size() >= kMaxYearSamples) continue;
                        std::string blocklot = first_prop_hist(fg, {"BLOCKLOT", "BLOCK_LOT", "LOT"});
                        std::string address = first_prop_hist(fg, {"FULLADDR", "PROPERTY_ADDRESS", "PREMISEADD", "ADDRESS", "Address", "ADDR"});
                        std::string owner = first_prop_hist(fg, {"OWNER_1", "OWNER_2", "OWNER_3", "OWNERNME1", "OWNER", "OWNER_NAME", "OWNER_ABBR", "AR_OWNER"});
                        std::string status = first_prop_hist(fg, {"STATUS", "STATE", "CASE_STATUS"});
                        std::ostringstream row;
                        row << layers[li].name << " | " << ds;
                        if (!blocklot.empty()) row << " | BL " << blocklot;
                        if (!address.empty()) row << " | " << address;
                        if (!owner.empty()) row << " | " << owner;
                        if (!status.empty()) row << " | " << status;
                        selected_record_year_samples.push_back(row.str());
                    }
                }
                selected_record_year_dirty = false;
            }
            if (selected_record_year >= 1900 && selected_record_year <= 2100) {
                ImGui::SeparatorText("Selected Year Records");
                ImGui::Text("%d: showing %zu of %d records",
                            selected_record_year,
                            selected_record_year_samples.size(),
                            selected_record_year_total);
                ImGui::BeginChild("selected_year_records", ImVec2(0, 170), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
                if (selected_record_year_samples.empty()) {
                    ImGui::TextDisabled("No sample records available for this year.");
                } else {
                    for (const std::string& row : selected_record_year_samples) {
                        ImGui::TextWrapped("%s", row.c_str());
                        ImGui::Separator();
                    }
                }
                ImGui::EndChild();
            }
        }
        ImGui::End();

        const float map_x = 440.0f;
        const float map_w = std::max(260.0f, (float)w - map_x - right_panel_w - 24.0f);
        ImGui::SetNextWindowPos(ImVec2(map_x, 12), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(map_w, (float)h - 24.0f), ImGuiCond_Always);
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
        auto wrap_world_x = [&](double x, int mz) -> double {
            const double period = 256.0 * (double)(1u << mz);
            x = std::fmod(x, period);
            if (x < 0.0) x += period;
            return x;
        };
        ImVec2 center_world = lonLatToWorldPx(center_lon, center_lat, math_zoom);
        center_world.x = (float)wrap_world_x((double)center_world.x, math_zoom);

        if (map_hovered) {
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f) {
                int next_zoom = std::clamp(zoom + (wheel > 0 ? 1 : -1), kMinZoom, kMaxZoom);
                if (next_zoom != zoom) {
                    const ImVec2 mouse = ImGui::GetIO().MousePos;
                    ImVec2 mouse_world = ImVec2(
                        center_world.x + (float)((mouse.x - (origin.x + size.x * 0.5f)) / zoom_scale),
                        center_world.y + (float)((mouse.y - (origin.y + size.y * 0.5f)) / zoom_scale));
                    mouse_world.x = (float)wrap_world_x((double)mouse_world.x, math_zoom);
                    ImVec2 ll = worldPxToLonLat(mouse_world, math_zoom);
                    zoom = next_zoom;
                    const int next_math_zoom = std::min(zoom, kMaxInternalMathZoom);
                    const double next_zoom_scale = std::ldexp(1.0, zoom - next_math_zoom);
                    ImVec2 mouse_world_new = lonLatToWorldPx(ll.x, ll.y, next_math_zoom);
                    center_world = ImVec2(
                        mouse_world_new.x - (float)((mouse.x - (origin.x + size.x * 0.5f)) / next_zoom_scale),
                        mouse_world_new.y - (float)((mouse.y - (origin.y + size.y * 0.5f)) / next_zoom_scale));
                    center_world.x = (float)wrap_world_x((double)center_world.x, next_math_zoom);
                    ImVec2 cll = worldPxToLonLat(center_world, next_math_zoom);
                    center_lon = cll.x;
                    center_lat = std::clamp((double)cll.y, -85.0, 85.0);
                }
            }

            if (map_active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
                ImVec2 d = ImGui::GetIO().MouseDelta;
                center_world.x -= (float)(d.x / zoom_scale);
                center_world.y -= (float)(d.y / zoom_scale);
                center_world.x = (float)wrap_world_x((double)center_world.x, math_zoom);
                ImVec2 ll = worldPxToLonLat(center_world, math_zoom);
                center_lon = ll.x;
                center_lat = std::clamp((double)ll.y, -85.0, 85.0);
            }
        }

        center_world = lonLatToWorldPx(center_lon, center_lat, math_zoom);
        center_world.x = (float)wrap_world_x((double)center_world.x, math_zoom);
        const float screen_cx = origin.x + size.x * 0.5f;
        const float screen_cy = origin.y + size.y * 0.5f;
        const float zsf = (float)zoom_scale;
        auto project_world = [&](const ImVec2& wp) -> ImVec2 {
            return ImVec2(
                screen_cx + (wp.x - center_world.x) * zsf,
                screen_cy + (wp.y - center_world.y) * zsf);
        };
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
        const LayerDef::FeatureGeom* hovered_zone = nullptr;
        size_t hovered_zone_idx = (size_t)-1;
        const double half_w_world = (size.x * 0.5) / zoom_scale;
        const double half_h_world = (size.y * 0.5) / zoom_scale;
        ImVec2 ll_a = worldPxToLonLat(ImVec2(center_world.x - (float)half_w_world, center_world.y - (float)half_h_world), math_zoom);
        ImVec2 ll_b = worldPxToLonLat(ImVec2(center_world.x + (float)half_w_world, center_world.y + (float)half_h_world), math_zoom);
        const float view_min_lon = std::min(ll_a.x, ll_b.x);
        const float view_max_lon = std::max(ll_a.x, ll_b.x);
        const float view_min_lat = std::min(ll_a.y, ll_b.y);
        const float view_max_lat = std::max(ll_a.y, ll_b.y);
        auto layer_hover_active = [&](int idx) -> bool {
            return idx >= 0 && (size_t)idx < layer_hover_enabled.size() && layer_hover_enabled[(size_t)idx];
        };
        auto layer_inspect_active = [&](int idx) -> bool {
            return idx >= 0 && (size_t)idx < layer_inspect_enabled.size() && layer_inspect_enabled[(size_t)idx];
        };
        const bool parcel_hover_active = hover_inspector_enabled && layer_hover_active(parcel_layer_idx);
        const bool parcel_inspect_active = layer_inspect_active(parcel_layer_idx);
        const bool zoning_hover_active = hover_inspector_enabled && layer_hover_active(zoning_layer_idx);
        const bool zoning_inspect_active = layer_inspect_active(zoning_layer_idx);

        if (map_hovered && (parcel_hover_active || parcel_inspect_active) && parcel_layer_idx >= 0) {
            const size_t pli = (size_t)parcel_layer_idx;
            if (pli < layers.size() && pli < layer_spatial.size() && layer_spatial[pli].built) {
                std::vector<uint32_t> hover_candidates;
                if (queryLayerSpatialIndex(
                        layer_spatial[pli],
                        mouse_ll.x,
                        mouse_ll.y,
                        mouse_ll.x,
                        mouse_ll.y,
                        hover_candidates)) {
                    float best_area = std::numeric_limits<float>::infinity();
                    for (uint32_t fidx : hover_candidates) {
                        if (fidx >= layers[pli].features.size()) continue;
                        const auto& fg = layers[pli].features[(size_t)fidx];
                        if (fg.rings.empty()) continue;
                        if (fg.extent.max_lon < view_min_lon || fg.extent.min_lon > view_max_lon ||
                            fg.extent.max_lat < view_min_lat || fg.extent.min_lat > view_max_lat) {
                            continue;
                        }
                        if (mouse_ll.x < fg.extent.min_lon || mouse_ll.x > fg.extent.max_lon ||
                            mouse_ll.y < fg.extent.min_lat || mouse_ll.y > fg.extent.max_lat) {
                            continue;
                        }
                        if (!pointInFeature(fg, mouse_ll.x, mouse_ll.y)) continue;
                        const float area = std::max(0.0f, fg.extent.max_lon - fg.extent.min_lon) *
                                           std::max(0.0f, fg.extent.max_lat - fg.extent.min_lat);
                        if (area < best_area) {
                            best_area = area;
                            hovered_parcel = &fg;
                            hovered_parcel_idx = (size_t)fidx;
                        }
                    }
                }
            }
        }
        if (map_hovered && (zoning_hover_active || zoning_inspect_active) && zoning_layer_idx >= 0 && (size_t)zoning_layer_idx < layers.size()) {
            const size_t zli = (size_t)zoning_layer_idx;
            if (zli < layer_spatial.size() && layer_spatial[zli].built) {
                std::vector<uint32_t> zone_candidates;
                if (queryLayerSpatialIndex(
                        layer_spatial[zli],
                        mouse_ll.x,
                        mouse_ll.y,
                        mouse_ll.x,
                        mouse_ll.y,
                        zone_candidates)) {
                    float best_area = std::numeric_limits<float>::infinity();
                    const auto& zfeats = layers[zli].features;
                    for (uint32_t zidx : zone_candidates) {
                        if (zidx >= zfeats.size()) continue;
                        const auto& zf = zfeats[zidx];
                        if (zf.rings.empty()) continue;
                        if (mouse_ll.x < zf.extent.min_lon || mouse_ll.x > zf.extent.max_lon ||
                            mouse_ll.y < zf.extent.min_lat || mouse_ll.y > zf.extent.max_lat) {
                            continue;
                        }
                        if (!pointInFeature(zf, mouse_ll.x, mouse_ll.y)) continue;
                        const float area = std::max(0.0f, zf.extent.max_lon - zf.extent.min_lon) *
                                           std::max(0.0f, zf.extent.max_lat - zf.extent.min_lat);
                        if (area < best_area) {
                            best_area = area;
                            hovered_zone = &zf;
                            hovered_zone_idx = (size_t)zidx;
                        }
                    }
                }
            }
        }
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
                ImVec2 p0 = project_world(tile_world);
                ImVec2 p1(p0.x + (float)(256.0 * zoom_scale), p0.y + (float)(256.0 * zoom_scale));
                draw->AddImage((ImTextureID)sample.tex->descriptor, p0, p1, sample.uv0, sample.uv1);
            }
        }

        bool vacant_notice_enabled = vacant_notice_layer_idx >= 0 && layers[(size_t)vacant_notice_layer_idx].enabled;
        bool vacant_rehab_enabled = vacant_rehab_layer_idx >= 0 && layers[(size_t)vacant_rehab_layer_idx].enabled;
        bool tax_lien_enabled = tax_lien_layer_idx >= 0 && layers[(size_t)tax_lien_layer_idx].enabled;
        bool tax_sale_enabled = tax_sale_layer_idx >= 0 && layers[(size_t)tax_sale_layer_idx].enabled;
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
        const bool allow_parcel_scale_fill = math_zoom >= 14;
        auto is_parcel_scale_layer = [&](size_t layer_idx) -> bool {
            return ((int)layer_idx == parcel_layer_idx ||
                    (int)layer_idx == real_property_layer_idx ||
                    (int)layer_idx == vacant_notice_layer_idx ||
                    (int)layer_idx == vacant_rehab_layer_idx ||
                    (int)layer_idx == tax_lien_layer_idx ||
                    (int)layer_idx == tax_sale_layer_idx);
        };
        auto should_fill_layer_polygon = [&](size_t layer_idx) -> bool {
            // Large-area layers still need low-zoom fill; parcel-scale fills are the expensive case.
            return !is_parcel_scale_layer(layer_idx) || allow_parcel_scale_fill;
        };
        auto get_world_rings = [&](size_t layer_idx, uint32_t feature_idx, const LayerDef::FeatureGeom& fg)
            -> const std::vector<std::vector<ImVec2>>& {
            auto& cache = layer_geom_cache[layer_idx];
            if (cache.zoom != math_zoom) {
                cache.zoom = math_zoom;
                cache.world_rings_by_feature.clear();
                cache.world_extent_by_feature.clear();
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
        auto get_world_extent = [&](size_t layer_idx, uint32_t feature_idx, const LayerDef::FeatureGeom& fg)
            -> const std::pair<ImVec2, ImVec2>& {
            auto& cache = layer_geom_cache[layer_idx];
            if (cache.zoom != math_zoom) {
                cache.zoom = math_zoom;
                cache.world_rings_by_feature.clear();
                cache.world_extent_by_feature.clear();
            }
            auto it = cache.world_extent_by_feature.find(feature_idx);
            if (it != cache.world_extent_by_feature.end()) return it->second;
            ImVec2 p0w = lonLatToWorldPx(fg.extent.min_lon, fg.extent.max_lat, math_zoom);
            ImVec2 p1w = lonLatToWorldPx(fg.extent.max_lon, fg.extent.min_lat, math_zoom);
            auto [ins_it, _] = cache.world_extent_by_feature.emplace(feature_idx, std::make_pair(p0w, p1w));
            return ins_it->second;
        };
        std::vector<ImVec2> scratch_fill_verts;
        scratch_fill_verts.reserve(4096);
        std::vector<uint32_t> scratch_fill_indices;
        scratch_fill_indices.reserve(12288);
        std::vector<ImVec2> scratch_line;
        scratch_line.reserve(1024);
        size_t fill_attempts_frame = 0;
        size_t fill_success_frame = 0;
        size_t fill_no_triangles_frame = 0;
        size_t fill_bad_indices_frame = 0;
        auto project_world_rings_for_fill = [&](const std::vector<std::vector<ImVec2>>& world_rings) -> size_t {
            size_t total = 0;
            for (const auto& r : world_rings) total += r.size();
            scratch_fill_verts.clear();
            scratch_fill_verts.reserve(total);
            for (const auto& r : world_rings) {
                for (const ImVec2& wp : r) scratch_fill_verts.push_back(project_world(wp));
            }
            return total;
        };
        auto append_world_ring_line = [&](const std::vector<ImVec2>& world_ring) {
            scratch_line.clear();
            if (world_ring.empty()) return;
            const int step = std::max(1, ring_step);
            const size_t n = world_ring.size();
            if (step == 1 || n <= 4) {
                for (const ImVec2& wp : world_ring) scratch_line.push_back(project_world(wp));
            } else {
                scratch_line.reserve((n / (size_t)step) + 2);
                for (size_t i = 0; i < n; i += (size_t)step) scratch_line.push_back(project_world(world_ring[i]));
                if ((n - 1) % (size_t)step != 0) scratch_line.push_back(project_world(world_ring.back()));
            }
        };
        auto draw_tessellated_fill = [&](const LayerDef::FeatureGeom& fg,
                                         const std::vector<std::vector<ImVec2>>& world_rings,
                                         ImU32 fill_color) -> bool {
            fill_attempts_frame++;
            if (fg.triangles.empty()) {
                fill_no_triangles_frame++;
                return false;
            }
            const size_t vcount = project_world_rings_for_fill(world_rings);
            if (vcount < 3) return false;

            scratch_fill_indices.clear();
            scratch_fill_indices.reserve(fg.triangles.size());
            for (size_t ti = 0; ti + 2 < fg.triangles.size(); ti += 3) {
                const uint32_t a = fg.triangles[ti + 0];
                const uint32_t b = fg.triangles[ti + 1];
                const uint32_t cidx = fg.triangles[ti + 2];
                if (a < vcount && b < vcount && cidx < vcount) {
                    scratch_fill_indices.push_back(a);
                    scratch_fill_indices.push_back(b);
                    scratch_fill_indices.push_back(cidx);
                }
            }
            if (scratch_fill_indices.empty()) {
                fill_bad_indices_frame++;
                return false;
            }

            const ImDrawListFlags old_flags = draw->Flags;
            draw->Flags &= ~ImDrawListFlags_AntiAliasedFill;
            for (size_t ii = 0; ii + 2 < scratch_fill_indices.size(); ii += 3) {
                draw->AddTriangleFilled(
                    scratch_fill_verts[scratch_fill_indices[ii + 0]],
                    scratch_fill_verts[scratch_fill_indices[ii + 1]],
                    scratch_fill_verts[scratch_fill_indices[ii + 2]],
                    fill_color);
            }
            draw->Flags = old_flags;
            fill_success_frame++;
            return true;
        };
        auto first_prop = [&](const LayerDef::FeatureGeom& fg, std::initializer_list<const char*> keys) {
            for (const char* k : keys) {
                std::string v = getPropertyValue(fg, k);
                if (!v.empty()) return v;
            }
            return std::string();
        };
        auto feature_passes_filters = [&](size_t layer_idx, size_t feature_idx, const LayerDef::FeatureGeom& fg) -> bool {
            if (!filter_enabled) return true;
            const LayerDef::FeatureGeom* rp_join = nullptr;
            if (parcel_layer_idx >= 0 &&
                (int)layer_idx == parcel_layer_idx &&
                real_property_layer_idx >= 0 &&
                (size_t)real_property_layer_idx < layers.size()) {
                std::string bl = normalizeJoinKey(first_prop(fg, {"BLOCKLOT", "BLOCK_LOT", "LOT"}));
                if (!bl.empty()) {
                    auto itrp = real_property_by_blocklot.find(bl);
                    if (itrp != real_property_by_blocklot.end() &&
                        itrp->second < layers[(size_t)real_property_layer_idx].features.size()) {
                        rp_join = &layers[(size_t)real_property_layer_idx].features[itrp->second];
                    }
                }
            }
            if (filter_use_date) {
                std::string ds = first_prop(fg, {"RECORD_DATE", "RECORDDATE", "DATE", "CREATED_DATE", "ISSUE_DATE", "DateNotice", "DateIssue", "DateIssued", "DateCancel", "DateAbate"});
                if (ds.empty() && rp_join) ds = first_prop(*rp_join, {"RECORD_DATE", "RECORDDATE", "DATE", "CREATED_DATE", "ISSUE_DATE", "DateNotice", "DateIssue", "DateIssued", "DateCancel", "DateAbate"});
                if (ds.empty()) return false;
                int yr = extractYearMaybe(ds);
                if (yr < 0 || yr < filter_year_min || yr > filter_year_max) return false;
            }
            const std::string q_blocklot(filter_blocklot);
            const std::string q_status(filter_status);
            const std::string q_address(filter_address);
            const std::string q_owner(filter_owner);
            const std::string q_zip(filter_zip);
            if (!q_blocklot.empty()) {
                std::string bl = first_prop(fg, {"BLOCKLOT", "BLOCK_LOT", "LOT"});
                if (!containsCaseInsensitive(bl, q_blocklot)) return false;
            }
            if (!q_status.empty()) {
                std::string st = first_prop(fg, {"STATUS", "STATE", "CASE_STATUS"});
                if (st.empty() && rp_join) st = first_prop(*rp_join, {"STATUS", "STATE", "CASE_STATUS"});
                if (st.empty() && parcel_layer_idx >= 0 && (int)layer_idx == parcel_layer_idx) {
                    const int vn = (feature_idx < parcel_vac_notice_by_feature.size()) ? parcel_vac_notice_by_feature[feature_idx] : 0;
                    const int vr = (feature_idx < parcel_vac_rehab_by_feature.size()) ? parcel_vac_rehab_by_feature[feature_idx] : 0;
                    st = (vn + vr) > 0 ? "vacant" : "occupied";
                }
                if (!containsCaseInsensitive(st, q_status)) return false;
            }
            if (!q_address.empty()) {
                std::string ad = first_prop(fg, {"FULLADDR", "PROPERTY_ADDRESS", "PREMISEADD", "ADDRESS", "Address", "ADDR"});
                if (ad.empty() && rp_join) ad = first_prop(*rp_join, {"FULLADDR", "PROPERTY_ADDRESS", "PREMISEADD", "ADDRESS", "Address", "ADDR"});
                if (!containsCaseInsensitive(ad, q_address)) return false;
            }
            if (!q_owner.empty()) {
                std::string ow = first_prop(fg, {"OWNER_1", "OWNER_2", "OWNER_3", "OWNERNME1", "OWNER", "OWNER_NAME", "OWNER_ABBR", "AR_OWNER"});
                if (ow.empty() && rp_join) ow = first_prop(*rp_join, {"OWNER_1", "OWNER_2", "OWNER_3", "OWNERNME1", "OWNER", "OWNER_NAME", "OWNER_ABBR", "AR_OWNER"});
                if (!containsCaseInsensitive(ow, q_owner)) return false;
            }
            if (!q_zip.empty()) {
                std::string zp = first_prop(fg, {"ZIP", "ZIPCODE", "POSTAL_CODE"});
                if (zp.empty() && rp_join) zp = first_prop(*rp_join, {"ZIP", "ZIPCODE", "POSTAL_CODE"});
                if (!containsCaseInsensitive(zp, q_zip)) return false;
            }
            return true;
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
                    if (!feature_passes_filters(layer_idx, (size_t)fidx, fg)) continue;
                    ImU32 feature_c = c;
                    if (is_zoning_layer) {
                        const std::string zkey = zoningClassKey(fg);
                        auto it_en = zoning_zone_enabled.find(zkey);
                        if (it_en != zoning_zone_enabled.end() && !it_en->second) continue;
                        auto it_col = zoning_zone_color.find(zkey);
                        if (it_col != zoning_zone_color.end()) feature_c = ImGui::ColorConvertFloat4ToU32(it_col->second);
                    }
                    const auto& ex = fg.extent;
                    const auto& pww = get_world_extent(layer_idx, fidx, fg);
                    ImVec2 p0w = pww.first;
                    ImVec2 p1w = pww.second;
                    ImVec2 a = project_world(p0w);
                    ImVec2 b = project_world(p1w);
                    ImVec2 p0(std::min(a.x, b.x), std::min(a.y, b.y));
                    ImVec2 p1(std::max(a.x, b.x), std::max(a.y, b.y));
                    if (p1.x < origin.x || p0.x > origin.x + size.x || p1.y < origin.y || p0.y > origin.y + size.y) continue;

                    if (!fg.rings.empty()) {
                        const auto& world_rings = get_world_rings(layer_idx, fidx, fg);
                        const bool fill_enabled_for_layer = layer_idx < layer_fill_enabled.size() && layer_fill_enabled[layer_idx];
                        if (fill_enabled_for_layer && should_fill_layer_polygon(layer_idx)) {
                            ImU32 fill = (feature_c & 0x00FFFFFF) | (170u << 24);
                            draw_tessellated_fill(fg, world_rings, fill);
                        }

                        for (const auto& r : world_rings) {
                            append_world_ring_line(r);
                            draw->AddPolyline(scratch_line.data(), (int)scratch_line.size(), feature_c, ImDrawFlags_Closed, 1.0f);
                        }
                    } else {
                        if ((int)layer_idx == vacant_notice_layer_idx || (int)layer_idx == vacant_rehab_layer_idx ||
                            (int)layer_idx == tax_lien_layer_idx || (int)layer_idx == tax_sale_layer_idx) continue;
                        ImVec2 pw = lonLatToWorldPx(fg.extent.min_lon, fg.extent.min_lat, math_zoom);
                        ImVec2 ps = project_world(pw);
                        if (ps.x >= origin.x && ps.x <= origin.x + size.x && ps.y >= origin.y && ps.y <= origin.y + size.y) {
                            float r = std::clamp((float)(2.0 * zoom_scale + 1.5), 2.0f, 6.0f);
                            draw->AddCircleFilled(ps, r, feature_c);
                        }
                    }

                }
                continue;
            }
            for (auto& fg : l.features) {
                size_t fi = (size_t)(&fg - &l.features[0]);
                if (!feature_passes_filters(layer_idx, fi, fg)) continue;
                ImU32 feature_c = c;
                if (is_zoning_layer) {
                    const std::string zkey = zoningClassKey(fg);
                    auto it_en = zoning_zone_enabled.find(zkey);
                    if (it_en != zoning_zone_enabled.end() && !it_en->second) continue;
                    auto it_col = zoning_zone_color.find(zkey);
                    if (it_col != zoning_zone_color.end()) feature_c = ImGui::ColorConvertFloat4ToU32(it_col->second);
                }
                const auto& ex = fg.extent;
                const auto& pww = get_world_extent(layer_idx, (uint32_t)fi, fg);
                ImVec2 p0w = pww.first;
                ImVec2 p1w = pww.second;
                ImVec2 a = project_world(p0w);
                ImVec2 b = project_world(p1w);
                ImVec2 p0(std::min(a.x, b.x), std::min(a.y, b.y));
                ImVec2 p1(std::max(a.x, b.x), std::max(a.y, b.y));
                if (p1.x < origin.x || p0.x > origin.x + size.x || p1.y < origin.y || p0.y > origin.y + size.y) continue;

                if (!fg.rings.empty()) {
                    const auto& world_rings = get_world_rings(layer_idx, (uint32_t)fi, fg);
                    const bool fill_enabled_for_layer = layer_idx < layer_fill_enabled.size() && layer_fill_enabled[layer_idx];
                    if (fill_enabled_for_layer && should_fill_layer_polygon(layer_idx)) {
                        ImU32 fill = (feature_c & 0x00FFFFFF) | (170u << 24);
                        draw_tessellated_fill(fg, world_rings, fill);
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
                        if (weight > 0) {
                            const bool notice_fill = vacant_notice_enabled &&
                                vacant_notice_layer_idx >= 0 &&
                                (size_t)vacant_notice_layer_idx < layer_fill_enabled.size() &&
                                layer_fill_enabled[(size_t)vacant_notice_layer_idx];
                            const bool rehab_fill = vacant_rehab_enabled &&
                                vacant_rehab_layer_idx >= 0 &&
                                (size_t)vacant_rehab_layer_idx < layer_fill_enabled.size() &&
                                layer_fill_enabled[(size_t)vacant_rehab_layer_idx];
                            if ((notice_fill || rehab_fill) && should_fill_layer_polygon((size_t)parcel_layer_idx)) {
                                const int alpha = std::clamp(95 + weight * 16, 95, 220);
                                ImU32 vac_fill = color_with_alpha(vacancy_base_color(vac_notice, vac_rehab), alpha);
                                draw_tessellated_fill(fg, world_rings, vac_fill);
                            }
                        }
                    }

                    for (const auto& r : world_rings) {
                        append_world_ring_line(r);
                        draw->AddPolyline(scratch_line.data(), (int)scratch_line.size(), feature_c, ImDrawFlags_Closed, 1.0f);
                    }
                } else {
                    // Vacant datasets are point sources used for parcel matching; suppress raw point dot rendering.
                    if (layer_idx == (size_t)vacant_notice_layer_idx || layer_idx == (size_t)vacant_rehab_layer_idx ||
                        layer_idx == (size_t)tax_lien_layer_idx || layer_idx == (size_t)tax_sale_layer_idx) continue;
                    ImVec2 pw = lonLatToWorldPx(fg.extent.min_lon, fg.extent.min_lat, math_zoom);
                    ImVec2 ps = project_world(pw);
                    if (ps.x >= origin.x && ps.x <= origin.x + size.x && ps.y >= origin.y && ps.y <= origin.y + size.y) {
                        float r = std::clamp((float)(2.0 * zoom_scale + 1.5), 2.0f, 6.0f);
                        draw->AddCircleFilled(ps, r, feature_c);
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
                if (!feature_passes_filters((size_t)parcel_layer_idx, i, fg)) continue;
                const auto& ex = fg.extent;
                const auto& pww = get_world_extent((size_t)parcel_layer_idx, (uint32_t)i, fg);
                ImVec2 p0w = pww.first;
                ImVec2 p1w = pww.second;
                ImVec2 a = project_world(p0w);
                ImVec2 b = project_world(p1w);
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
                const int alpha = std::clamp(120 + weight * 18, 120, 230);
                ImVec4 vac_base = vacancy_base_color(vac_notice, vac_rehab);
                ImU32 vac_fill = color_with_alpha(vac_base, alpha);
                ImU32 vac_outline = color_with_alpha(darken_color(vac_base, 0.62f), 235);
                const bool notice_fill = vacant_notice_layer_idx >= 0 &&
                    (size_t)vacant_notice_layer_idx < layer_fill_enabled.size() &&
                    layer_fill_enabled[(size_t)vacant_notice_layer_idx];
                const bool rehab_fill = vacant_rehab_layer_idx >= 0 &&
                    (size_t)vacant_rehab_layer_idx < layer_fill_enabled.size() &&
                    layer_fill_enabled[(size_t)vacant_rehab_layer_idx];
                if ((notice_fill || rehab_fill) && should_fill_layer_polygon((size_t)parcel_layer_idx)) {
                    draw_tessellated_fill(fg, world_rings, vac_fill);
                }
                for (const auto& r : world_rings) {
                    append_world_ring_line(r);
                    if (!scratch_line.empty()) draw->AddPolyline(scratch_line.data(), (int)scratch_line.size(), vac_outline, ImDrawFlags_Closed, 2.0f);
                }
            }
        }
        visible_vacant_parcels_last_frame.store(visible_vacant_parcels_counter, std::memory_order_relaxed);

        // Render tax source records on matched parcel geometry, not as raw point dots.
        if ((tax_lien_enabled || tax_sale_enabled) && parcel_layer_idx >= 0) {
            auto& parcel_layer = layers[(size_t)parcel_layer_idx];
            for (size_t i = 0; i < parcel_layer.features.size(); ++i) {
                auto& fg = parcel_layer.features[i];
                if (!feature_passes_filters((size_t)parcel_layer_idx, i, fg)) continue;
                const auto& pww = get_world_extent((size_t)parcel_layer_idx, (uint32_t)i, fg);
                ImVec2 a = project_world(pww.first);
                ImVec2 b = project_world(pww.second);
                ImVec2 p0(std::min(a.x, b.x), std::min(a.y, b.y));
                ImVec2 p1(std::max(a.x, b.x), std::max(a.y, b.y));
                if (p1.x < origin.x || p0.x > origin.x + size.x || p1.y < origin.y || p0.y > origin.y + size.y) continue;
                if (fg.rings.empty()) continue;

                const int lien_count = (i < parcel_tax_lien_by_feature.size()) ? parcel_tax_lien_by_feature[i] : 0;
                const int sale_count = (i < parcel_tax_sale_by_feature.size()) ? parcel_tax_sale_by_feature[i] : 0;
                int weight = 0;
                if (tax_lien_enabled) weight += lien_count;
                if (tax_sale_enabled) weight += sale_count;
                if (weight <= 0) continue;

                ImVec4 lien_c = (tax_lien_layer_idx >= 0) ? layers[(size_t)tax_lien_layer_idx].color : ImVec4(0.95f, 0.55f, 0.1f, 1.0f);
                ImVec4 sale_c = (tax_sale_layer_idx >= 0) ? layers[(size_t)tax_sale_layer_idx].color : ImVec4(0.85f, 0.2f, 0.1f, 1.0f);
                ImVec4 tax_base = lien_c;
                if (tax_lien_enabled && tax_sale_enabled && lien_count > 0 && sale_count > 0) {
                    tax_base = ImVec4((lien_c.x + sale_c.x) * 0.5f,
                                      (lien_c.y + sale_c.y) * 0.5f,
                                      (lien_c.z + sale_c.z) * 0.5f,
                                      1.0f);
                } else if (tax_sale_enabled && sale_count > 0) {
                    tax_base = sale_c;
                }
                const auto& world_rings = get_world_rings((size_t)parcel_layer_idx, (uint32_t)i, fg);
                const bool lien_fill = tax_lien_enabled &&
                    tax_lien_layer_idx >= 0 &&
                    (size_t)tax_lien_layer_idx < layer_fill_enabled.size() &&
                    layer_fill_enabled[(size_t)tax_lien_layer_idx];
                const bool sale_fill = tax_sale_enabled &&
                    tax_sale_layer_idx >= 0 &&
                    (size_t)tax_sale_layer_idx < layer_fill_enabled.size() &&
                    layer_fill_enabled[(size_t)tax_sale_layer_idx];
                if ((lien_fill || sale_fill) && should_fill_layer_polygon((size_t)parcel_layer_idx)) {
                    const int alpha = std::clamp(90 + weight * 10, 90, 210);
                    draw_tessellated_fill(fg, world_rings, color_with_alpha(tax_base, alpha));
                }
                ImU32 tax_outline = color_with_alpha(darken_color(tax_base, 0.58f), 240);
                for (const auto& r : world_rings) {
                    append_world_ring_line(r);
                    if (!scratch_line.empty()) draw->AddPolyline(scratch_line.data(), (int)scratch_line.size(), tax_outline, ImDrawFlags_Closed, 2.0f);
                }
            }
        }

        render_fill_attempts_last_frame.store(fill_attempts_frame, std::memory_order_relaxed);
        render_fill_success_last_frame.store(fill_success_frame, std::memory_order_relaxed);
        render_fill_no_triangles_last_frame.store(fill_no_triangles_frame, std::memory_order_relaxed);
        render_fill_bad_indices_last_frame.store(fill_bad_indices_frame, std::memory_order_relaxed);

        if (map_hovered && parcel_inspect_active && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hovered_parcel != nullptr) {
            show_selected_parcel_details = true;
            selected_parcel_idx = hovered_parcel_idx;
            show_selected_zone_details = false;
            selected_zone_idx = (size_t)-1;
        } else if (map_hovered && zoning_inspect_active && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hovered_zone != nullptr) {
            show_selected_zone_details = true;
            selected_zone_idx = hovered_zone_idx;
            show_selected_parcel_details = false;
            selected_parcel_idx = (size_t)-1;
        }

        if (parcel_hover_active && map_hovered && hovered_parcel) {
            std::string blocklot_raw = getPropertyValue(*hovered_parcel, "BLOCKLOT");
            std::string blocklot = normalizeJoinKey(blocklot_raw);
            int vac_notice = (hovered_parcel_idx < parcel_vac_notice_by_feature.size()) ? parcel_vac_notice_by_feature[hovered_parcel_idx] : 0;
            int vac_rehab = (hovered_parcel_idx < parcel_vac_rehab_by_feature.size()) ? parcel_vac_rehab_by_feature[hovered_parcel_idx] : 0;
            int tax_lien = (hovered_parcel_idx < parcel_tax_lien_by_feature.size()) ? parcel_tax_lien_by_feature[hovered_parcel_idx] : 0;
            int tax_sale = (hovered_parcel_idx < parcel_tax_sale_by_feature.size()) ? parcel_tax_sale_by_feature[hovered_parcel_idx] : 0;
            double tax_lien_amount = (hovered_parcel_idx < parcel_tax_lien_amount_by_feature.size()) ? parcel_tax_lien_amount_by_feature[hovered_parcel_idx] : 0.0;
            double tax_sale_amount = (hovered_parcel_idx < parcel_tax_sale_amount_by_feature.size()) ? parcel_tax_sale_amount_by_feature[hovered_parcel_idx] : 0.0;
            const LayerDef::FeatureGeom* hovered_zoning = hovered_zone;
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
            ImGui::Text("Tax Lien Certificate Records: %d", tax_lien);
            if (tax_lien > 0) ImGui::Text("Tax Lien Total Amount: $%.2f", tax_lien_amount);
            ImGui::Text("Tax Sale 2021 Records: %d", tax_sale);
            if (tax_sale > 0) ImGui::Text("Tax Sale Total Lien: $%.2f", tax_sale_amount);

            const LayerDef::FeatureGeom* hovered_rp = real_property_for_parcel(*hovered_parcel);
            draw_real_property_summary(hovered_rp);

            ImGui::Separator();
            if (hovered_zoning) {
                std::string zone_key = zoningClassKey(*hovered_zoning);
                std::string zone_label = zoningClassLabel(*hovered_zoning);
                std::string zone_description;
                auto meta_it = zoning_metadata.find(zone_key);
                if (meta_it != zoning_metadata.end()) {
                    if (!meta_it->second.label.empty()) zone_label = meta_it->second.label;
                    zone_description = meta_it->second.description;
                }
                ImGui::Text("Zoning: %s", zone_key.empty() ? "(available, unlabeled)" : zone_key.c_str());
                if (!zone_label.empty() && zone_label != zone_key) {
                    ImGui::TextWrapped("Zoning Label: %s", zone_label.c_str());
                }
                if (!zone_description.empty()) {
                    ImGui::TextWrapped("Description: %s", zone_description.c_str());
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
            draw_feature_properties("All Parcel Geometry Fields", *hovered_parcel);
            if (hovered_rp) {
                ImGui::Separator();
                draw_feature_properties("All Real Property Fields", *hovered_rp);
            }
            ImGui::EndTooltip();
        }
        if (zoning_hover_active && map_hovered && !hovered_parcel && hovered_zone) {
            std::string zone_key = zoningClassKey(*hovered_zone);
            std::string zone_label = zoningClassLabel(*hovered_zone);
            std::string zone_description;
            auto meta_it = zoning_metadata.find(zone_key);
            if (meta_it != zoning_metadata.end()) {
                if (!meta_it->second.label.empty()) zone_label = meta_it->second.label;
                zone_description = meta_it->second.description;
            }
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Zoning Details");
            ImGui::Separator();
            ImGui::Text("Feature: %zu", hovered_zone_idx);
            ImGui::Text("Zone: %s", zone_key.empty() ? "(unlabeled)" : zone_key.c_str());
            if (!zone_label.empty() && zone_label != zone_key) {
                ImGui::TextWrapped("Label: %s", zone_label.c_str());
            }
            if (!zone_description.empty()) {
                ImGui::TextWrapped("Description: %s", zone_description.c_str());
            }
            ImGui::Separator();
            ImGui::TextUnformatted("All Zone Fields");
            for (const auto& kv : hovered_zone->properties) {
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
    saveLayerUiState(
        root,
        layers,
        hover_inspector_enabled,
        &zoning_zone_enabled,
        &layer_fill_enabled,
        &layer_hover_enabled,
        &layer_inspect_enabled);
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
