#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_vulkan.h"
#include "layer_state_io.h"
#include "feature_props.h"
#include "geo.h"
#include "cache_io.h"
#include "arkavo_realtime_client.h"
#include "arkavo_signaling_transport_curl.h"
#include "arkavo_rtc_session_manager.h"
#include "time_cube.h"

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
#include <future>
#include <deque>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <cctype>
#include <cfloat>
#include <random>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#else
#include <sys/socket.h>
#include <sys/resource.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#endif
#include "earcut.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;
static constexpr const char* kAppVersion = "0.1.0";
static constexpr int kProtocolVersion = 1;

struct ProfileFrameSample {
    double frame_ms = 0.0;
    double ui_total_ms = 0.0;
    double owner_aggregate_ms = 0.0;
    double tiles_ms = 0.0;
    double layers_ms = 0.0;
    double heatmap_ms = 0.0;
    double overlays_ms = 0.0;
    double render_present_ms = 0.0;
    size_t tiles_drawn = 0;
    size_t features_considered = 0;
    size_t features_drawn_points = 0;
    size_t heat_samples = 0;
    size_t retired_textures = 0;
};

struct LayerProfileSnapshot {
    size_t index = 0;
    std::string name;
    std::string file;
    bool enabled = false;
    size_t features = 0;
    size_t rings = 0;
    size_t ring_points = 0;
    size_t triangle_indices = 0;
    size_t properties = 0;
    bool spatial_index_built = false;
    size_t spatial_index_cells = 0;
    size_t spatial_index_marks = 0;
};

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
        case LayerDef::Category::PublicHealth: return "public_health";
        case LayerDef::Category::Infrastructure: return "infrastructure";
        case LayerDef::Category::Zoning: return "zoning";
        case LayerDef::Category::Safety: return "safety";
    }
    return "unknown";
}

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static bool ensureSocketRuntimeInitialized() {
#ifdef _WIN32
    static bool initialized = false;
    static bool ok = false;
    if (!initialized) {
        WSADATA wsa{};
        ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
        initialized = true;
    }
    return ok;
#else
    return true;
#endif
}

static int netClose(int fd) {
#ifdef _WIN32
    return closesocket((SOCKET)fd);
#else
    return close(fd);
#endif
}

static ssize_t netRead(int fd, void* buf, size_t len) {
#ifdef _WIN32
    return (ssize_t)recv((SOCKET)fd, (char*)buf, (int)len, 0);
#else
    return read(fd, buf, len);
#endif
}

static ssize_t netWrite(int fd, const char* data, size_t len) {
#ifdef _WIN32
    return (ssize_t)send((SOCKET)fd, data, (int)len, 0);
#else
    return write(fd, data, len);
#endif
}

static bool writeAll(int fd, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = netWrite(fd, data + sent, len - sent);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

static std::string urlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    auto hexv = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+' ) out.push_back(' ');
        else if (s[i] == '%' && i + 2 < s.size()) {
            int a = hexv(s[i + 1]);
            int b = hexv(s[i + 2]);
            if (a >= 0 && b >= 0) {
                out.push_back((char)((a << 4) | b));
                i += 2;
            } else out.push_back(s[i]);
        } else out.push_back(s[i]);
    }
    return out;
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

struct VersionedDownloadResult {
    bool ok = false;
    bool not_modified = false;
    bool changed = false;
    std::string message;
};

enum class FreshnessState {
    Unknown,
    UpToDate,
    UpdateAvailable,
    NotTrackable,
    Error
};

struct FreshnessCheckResult {
    FreshnessState state = FreshnessState::Unknown;
    bool ok = false;
    std::string message;
};

static uint64_t fnv1a64File(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return 0;
    uint64_t h = 1469598103934665603ull;
    char buf[1 << 15];
    while (in.good()) {
        in.read(buf, sizeof(buf));
        std::streamsize n = in.gcount();
        for (std::streamsize i = 0; i < n; ++i) {
            h ^= (uint8_t)buf[i];
            h *= 1099511628211ull;
        }
    }
    return h;
}

static std::string toHexU64(uint64_t v) {
    std::ostringstream os;
    os << std::hex << std::setw(16) << std::setfill('0') << v;
    return os.str();
}

static std::string isoNowUtcCompact() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &tm);
    return std::string(buf);
}

static std::string featureKey(const json& f, size_t idx) {
    if (f.contains("id")) {
        if (f["id"].is_string()) return f["id"].get<std::string>();
        if (f["id"].is_number_integer()) return std::to_string(f["id"].get<long long>());
    }
    if (f.contains("properties") && f["properties"].is_object()) {
        const auto& p = f["properties"];
        const char* keys[] = {"OBJECTID", "objectid", "id", "ID", "FIPS", "GEOID", "BLOCKLOT", "blocklot"};
        for (const char* k : keys) {
            if (!p.contains(k)) continue;
            if (p[k].is_string()) return p[k].get<std::string>();
            if (p[k].is_number_integer()) return std::to_string(p[k].get<long long>());
            if (p[k].is_number_float()) return std::to_string(p[k].get<double>());
        }
    }
    return std::string("__idx_") + std::to_string(idx);
}

static void writeGeoJsonDiffArtifact(
    const fs::path& old_path,
    const fs::path& new_path,
    const fs::path& diff_path,
    const std::string& old_hash,
    const std::string& new_hash) {
    std::ifstream old_in(old_path), new_in(new_path);
    if (!old_in || !new_in) return;
    json old_j, new_j;
    try {
        old_in >> old_j;
        new_in >> new_j;
    } catch (...) {
        return;
    }
    if (!old_j.contains("features") || !new_j.contains("features") ||
        !old_j["features"].is_array() || !new_j["features"].is_array()) {
        return;
    }
    std::unordered_map<std::string, std::string> old_map;
    std::unordered_map<std::string, std::string> new_map;
    size_t i = 0;
    for (const auto& f : old_j["features"]) old_map[featureKey(f, i++)] = f.dump();
    i = 0;
    for (const auto& f : new_j["features"]) new_map[featureKey(f, i++)] = f.dump();

    std::vector<std::string> added, removed, changed;
    added.reserve(64); removed.reserve(64); changed.reserve(64);
    size_t added_n = 0, removed_n = 0, changed_n = 0;
    for (const auto& kv : new_map) {
        auto it = old_map.find(kv.first);
        if (it == old_map.end()) {
            added_n++;
            if (added.size() < 32) added.push_back(kv.first);
        } else if (it->second != kv.second) {
            changed_n++;
            if (changed.size() < 32) changed.push_back(kv.first);
        }
    }
    for (const auto& kv : old_map) {
        if (new_map.find(kv.first) == new_map.end()) {
            removed_n++;
            if (removed.size() < 32) removed.push_back(kv.first);
        }
    }
    json out{
        {"schema_version", 1},
        {"created_at", isoNowUtcCompact()},
        {"old_hash", old_hash},
        {"new_hash", new_hash},
        {"counts", {
            {"old_features", old_map.size()},
            {"new_features", new_map.size()},
            {"added", added_n},
            {"removed", removed_n},
            {"changed", changed_n}
        }},
        {"sample_keys", {
            {"added", added},
            {"removed", removed},
            {"changed", changed}
        }}
    };
    fs::create_directories(diff_path.parent_path());
    std::ofstream diff_out(diff_path);
    if (diff_out) diff_out << out.dump(2);
}

struct HeaderCapture {
    std::string etag;
    std::string last_modified;
};

static size_t curlHeaderCapture(void* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t n = size * nmemb;
    if (!userdata || n == 0) return n;
    HeaderCapture* hc = (HeaderCapture*)userdata;
    std::string line((const char*)ptr, n);
    auto lower = line;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    auto trim = [](std::string s) {
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || std::isspace((unsigned char)s.back()))) s.pop_back();
        size_t i = 0;
        while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
        return s.substr(i);
    };
    if (lower.rfind("etag:", 0) == 0) hc->etag = trim(line.substr(5));
    if (lower.rfind("last-modified:", 0) == 0) hc->last_modified = trim(line.substr(14));
    return n;
}

static VersionedDownloadResult downloadUrlVersioned(
    const std::string& url,
    const fs::path& out_path,
    const fs::path& versions_root) {
    VersionedDownloadResult res;
    const fs::path meta_path = versions_root / "metadata" / (out_path.filename().string() + ".json");
    fs::create_directories(meta_path.parent_path());
    json meta = json::object();
    {
        std::ifstream in(meta_path);
        if (in) {
            try { in >> meta; } catch (...) { meta = json::object(); }
        }
    }
    std::string prev_etag = meta.value("etag", std::string());
    std::string prev_lm = meta.value("last_modified", std::string());

    fs::create_directories(out_path.parent_path());
    fs::path tmp = out_path;
    tmp += ".part";
    FILE* fp = std::fopen(tmp.string().c_str(), "wb");
    if (!fp) {
        res.message = "failed to open output file";
        return res;
    }
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::fclose(fp);
        res.message = "curl init failed";
        return res;
    }
    struct curl_slist* hdrs = nullptr;
    if (!prev_etag.empty()) hdrs = curl_slist_append(hdrs, ("If-None-Match: " + prev_etag).c_str());
    if (!prev_lm.empty()) hdrs = curl_slist_append(hdrs, ("If-Modified-Since: " + prev_lm).c_str());
    HeaderCapture hc{};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "BaltimoreVulkanMap/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 180L);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteToFile);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curlHeaderCapture);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &hc);
    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_off_t content_length = -1;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &content_length);
    curl_easy_cleanup(curl);
    if (hdrs) curl_slist_free_all(hdrs);
    std::fclose(fp);

    if (rc != CURLE_OK) {
        std::error_code ec;
        fs::remove(tmp, ec);
        res.message = std::string("http failed: ") + curl_easy_strerror(rc);
        return res;
    }
    if (code == 304) {
        std::error_code ec;
        fs::remove(tmp, ec);
        meta["checked_at"] = isoNowUtcCompact();
        std::ofstream mo(meta_path);
        if (mo) mo << meta.dump(2);
        res.ok = true;
        res.not_modified = true;
        res.message = "not modified";
        return res;
    }
    if (code < 200 || code >= 300) {
        std::error_code ec;
        fs::remove(tmp, ec);
        res.message = "http code " + std::to_string(code);
        return res;
    }

    const bool had_old = fs::exists(out_path);
    fs::path old_snapshot_tmp = versions_root / "tmp" / (out_path.filename().string() + ".old");
    fs::create_directories(old_snapshot_tmp.parent_path());
    if (had_old) {
        std::error_code ec;
        fs::copy_file(out_path, old_snapshot_tmp, fs::copy_options::overwrite_existing, ec);
    }
    std::error_code ec;
    fs::rename(tmp, out_path, ec);
    if (ec) {
        res.message = "rename failed: " + ec.message();
        return res;
    }

    const uint64_t new_h = fnv1a64File(out_path);
    const std::string new_hash = toHexU64(new_h);
    const std::string prev_hash = meta.value("content_hash", std::string());
    const bool changed = !prev_hash.empty() ? (new_hash != prev_hash) : true;
    const std::string stamp = isoNowUtcCompact();

    if (changed) {
        const fs::path snap_dir = versions_root / "snapshots" / out_path.filename().string();
        fs::create_directories(snap_dir);
        const fs::path snap_path = snap_dir / (stamp + "_" + new_hash + out_path.extension().string());
        std::error_code ec2;
        fs::copy_file(out_path, snap_path, fs::copy_options::overwrite_existing, ec2);
        if (had_old && fs::exists(old_snapshot_tmp)) {
            const uint64_t old_h = fnv1a64File(old_snapshot_tmp);
            const std::string old_hash = toHexU64(old_h);
            if (out_path.extension() == ".geojson") {
                const fs::path diff_path = versions_root / "diffs" / out_path.filename().string() /
                    (stamp + "_" + old_hash + "_to_" + new_hash + ".json");
                writeGeoJsonDiffArtifact(old_snapshot_tmp, out_path, diff_path, old_hash, new_hash);
            }
        }
    }
    if (fs::exists(old_snapshot_tmp)) {
        std::error_code ec3;
        fs::remove(old_snapshot_tmp, ec3);
    }
    meta["url"] = url;
    meta["file"] = out_path.filename().string();
    meta["etag"] = hc.etag.empty() ? prev_etag : hc.etag;
    meta["last_modified"] = hc.last_modified.empty() ? prev_lm : hc.last_modified;
    meta["status_code"] = code;
    meta["content_length"] = (long long)content_length;
    meta["content_hash"] = new_hash;
    meta["fetched_at"] = stamp;
    meta["checked_at"] = stamp;
    meta["size_bytes"] = (long long)fs::file_size(out_path);
    meta["version_counter"] = meta.value("version_counter", 0) + (changed ? 1 : 0);
    std::ofstream mo(meta_path);
    if (mo) mo << meta.dump(2);

    res.ok = true;
    res.changed = changed;
    res.message = changed ? ("updated hash=" + new_hash) : "no content change";
    return res;
}

static FreshnessCheckResult checkUrlFreshnessVersioned(
    const std::string& url,
    const fs::path& out_path,
    const fs::path& versions_root) {
    FreshnessCheckResult r;
    if (url.empty()) {
        r.state = FreshnessState::NotTrackable;
        r.ok = false;
        r.message = "no source URL";
        return r;
    }
    const fs::path meta_path = versions_root / "metadata" / (out_path.filename().string() + ".json");
    json meta = json::object();
    {
        std::ifstream in(meta_path);
        if (in) {
            try { in >> meta; } catch (...) { meta = json::object(); }
        }
    }
    std::string prev_etag = meta.value("etag", std::string());
    std::string prev_lm = meta.value("last_modified", std::string());

    CURL* curl = curl_easy_init();
    if (!curl) {
        r.state = FreshnessState::Error;
        r.message = "curl init failed";
        return r;
    }
    struct curl_slist* hdrs = nullptr;
    if (!prev_etag.empty()) hdrs = curl_slist_append(hdrs, ("If-None-Match: " + prev_etag).c_str());
    if (!prev_lm.empty()) hdrs = curl_slist_append(hdrs, ("If-Modified-Since: " + prev_lm).c_str());
    HeaderCapture hc{};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "BaltimoreVulkanMap/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curlHeaderCapture);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &hc);
    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);
    if (hdrs) curl_slist_free_all(hdrs);
    meta["checked_at"] = isoNowUtcCompact();
    if (!hc.etag.empty()) meta["etag"] = hc.etag;
    if (!hc.last_modified.empty()) meta["last_modified"] = hc.last_modified;
    fs::create_directories(meta_path.parent_path());
    std::ofstream mo(meta_path);
    if (mo) mo << meta.dump(2);

    if (rc != CURLE_OK) {
        r.state = FreshnessState::Error;
        r.message = std::string("http failed: ") + curl_easy_strerror(rc);
        return r;
    }
    if (code == 304) {
        r.state = FreshnessState::UpToDate;
        r.ok = true;
        r.message = "up-to-date (304)";
        return r;
    }
    if (code >= 200 && code < 300) {
        if (!prev_etag.empty() || !prev_lm.empty()) {
            r.state = FreshnessState::UpdateAvailable;
            r.ok = true;
            r.message = "update available";
        } else {
            r.state = FreshnessState::Unknown;
            r.ok = true;
            r.message = "tracked; baseline unknown";
        }
        return r;
    }
    r.state = FreshnessState::Error;
    r.message = "http code " + std::to_string(code);
    return r;
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

struct AppSettings {
    bool vulkan_validation_enabled = false;
    bool grayscale_basemap = false;
};

static AppSettings loadAppSettings(const fs::path& root, const AppSettings& defaults) {
    std::ifstream in(root / "data" / "app_settings.json");
    if (!in) return defaults;
    AppSettings out = defaults;
    json j;
    try {
        in >> j;
    } catch (...) {
        return defaults;
    }
    if (j.contains("vulkan_validation_enabled") && j["vulkan_validation_enabled"].is_boolean()) {
        out.vulkan_validation_enabled = j["vulkan_validation_enabled"].get<bool>();
    }
    if (j.contains("grayscale_basemap") && j["grayscale_basemap"].is_boolean()) {
        out.grayscale_basemap = j["grayscale_basemap"].get<bool>();
    }
    return out;
}

static void saveAppSettings(const fs::path& root, const AppSettings& settings) {
    fs::create_directories(root / "data");
    json j;
    j["vulkan_validation_enabled"] = settings.vulkan_validation_enabled;
    j["grayscale_basemap"] = settings.grayscale_basemap;
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
static constexpr size_t kMaxSmoothHeatSamplesPerLayer = 50000;
static constexpr int kSmoothHeatRasterBasePx = 1536;
static constexpr int kSmoothHeatRasterMaxPx = 2048;

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
static std::vector<TileTexture> g_RetiredTextures;
static int g_TextureRetireFrames = 0;

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

static void destroyTileTextureNow(TileTexture& tex) {
    if (tex.descriptor) ImGui_ImplVulkan_RemoveTexture(tex.descriptor);
    if (tex.view) vkDestroyImageView(g_Device, tex.view, g_Allocator);
    if (tex.image) vkDestroyImage(g_Device, tex.image, g_Allocator);
    if (tex.memory) vkFreeMemory(g_Device, tex.memory, g_Allocator);
    tex = {};
}

static void destroyTileTexture(TileTexture& tex) {
    if (!tex.descriptor && !tex.view && !tex.image && !tex.memory) return;
    g_RetiredTextures.push_back(tex);
    g_TextureRetireFrames = 8;
    tex = {};
}

static void drainRetiredTextures(bool force = false) {
    if (force) {
        if (g_Device != VK_NULL_HANDLE) check_vk_result(vkDeviceWaitIdle(g_Device));
        for (auto& tex : g_RetiredTextures) destroyTileTextureNow(tex);
        g_RetiredTextures.clear();
        g_TextureRetireFrames = 0;
        return;
    }
    if (g_RetiredTextures.empty()) return;
    if (g_TextureRetireFrames > 0) {
        g_TextureRetireFrames--;
        return;
    }
    for (auto& tex : g_RetiredTextures) destroyTileTextureNow(tex);
    g_RetiredTextures.clear();
}

static bool uploadRgbaTexture(const unsigned char* pixels, uint32_t w, uint32_t h, TileTexture& tex) {
    if (!pixels || w == 0 || h == 0) return false;
    destroyTileTexture(tex);

    const VkDeviceSize size = (VkDeviceSize)w * (VkDeviceSize)h * 4;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging, staging_mem);

    void* mapped = nullptr;
    check_vk_result(vkMapMemory(g_Device, staging_mem, 0, size, 0, &mapped));
    std::memcpy(mapped, pixels, (size_t)size);
    vkUnmapMemory(g_Device, staging_mem);

    tex.mip_levels = 1;
    createImage(w, h, tex.mip_levels, tex.image, tex.memory);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {w, h, 1};

    submitUploadCommands([&](VkCommandBuffer cmd) {
        transitionImage(cmd, tex.image, 0, tex.mip_levels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyBufferToImage(cmd, staging, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        transitionImage(cmd, tex.image, 0, tex.mip_levels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });

    vkDestroyBuffer(g_Device, staging, g_Allocator);
    vkFreeMemory(g_Device, staging_mem, g_Allocator);

    tex.view = createImageView(tex.image, tex.mip_levels);
    tex.descriptor = ImGui_ImplVulkan_AddTexture(g_TileSampler, tex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return tex.descriptor != VK_NULL_HANDLE;
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
    if (g_Device != VK_NULL_HANDLE) check_vk_result(vkDeviceWaitIdle(g_Device));
    for (auto& kv : g_TileCache) destroyTileTextureNow(kv.second.tex);
    g_TileCache.clear();
    g_TileLRU.clear();
    drainRetiredTextures(true);
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
    AppSettings app_settings;
    app_settings.vulkan_validation_enabled = g_EnableValidationLayers;
    app_settings = loadAppSettings(root, app_settings);
    g_EnableValidationLayers = app_settings.vulkan_validation_enabled;
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
    bootstrap.running.store(false, std::memory_order_relaxed);
    bootstrap.done.store(true, std::memory_order_relaxed);
    bootstrap.phase.store(3, std::memory_order_relaxed);
    setBootstrapStatus(bootstrap, "Startup download disabled. Use Data Library to fetch missing datasets.");

    auto layers = loadManifest(root);
    int parcel_layer_idx = -1;
    int real_property_layer_idx = -1;
    int vacant_notice_layer_idx = -1;
    int vacant_rehab_layer_idx = -1;
    int tax_lien_layer_idx = -1;
    int tax_sale_layer_idx = -1;
    int zoning_layer_idx = -1;
    int crime_nibrs_layer_idx = -1;
    int crime_legacy_layer_idx = -1;
    for (size_t i = 0; i < layers.size(); ++i) {
        if (layers[i].file == "parcel.geojson") parcel_layer_idx = (int)i;
        else if (layers[i].file == "real_property_information.geojson") real_property_layer_idx = (int)i;
        else if (layers[i].file == "vacant_building_notices.geojson") vacant_notice_layer_idx = (int)i;
        else if (layers[i].file == "vacant_building_rehabs.geojson") vacant_rehab_layer_idx = (int)i;
        else if (layers[i].file == "tax_lien_certificate_sale_properties.geojson") tax_lien_layer_idx = (int)i;
        else if (layers[i].file == "tax_sale_list_2021.geojson") tax_sale_layer_idx = (int)i;
        else if (layers[i].file == "zoning.geojson") zoning_layer_idx = (int)i;
        else if (layers[i].file == "crime_nibrs_group_a_2022_present.geojson") crime_nibrs_layer_idx = (int)i;
        else if (layers[i].file == "crime_part_1_legacy_srs.geojson") crime_legacy_layer_idx = (int)i;
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
    std::atomic<double> prof_ui_ms_last{0.0};
    std::atomic<double> prof_owner_ms_last{0.0};
    std::atomic<double> prof_tile_ms_last{0.0};
    std::atomic<double> prof_layer_ms_last{0.0};
    std::atomic<double> prof_heatmap_ms_last{0.0};
    std::atomic<double> prof_overlay_ms_last{0.0};
    std::atomic<double> prof_present_ms_last{0.0};
    std::atomic<size_t> prof_tiles_drawn_last{0};
    std::atomic<size_t> prof_features_considered_last{0};
    std::atomic<size_t> prof_features_drawn_last{0};
    std::atomic<size_t> prof_heat_samples_last{0};
    std::atomic<size_t> prof_retired_textures{0};
    std::atomic<size_t> prof_tile_cache_size{0};
    std::mutex profile_mutex;
    std::vector<ProfileFrameSample> profile_samples(600);
    size_t profile_sample_pos = 0;
    size_t profile_sample_count = 0;
    uint64_t profile_reset_generation = 0;
    std::mutex layer_profile_mutex;
    std::vector<LayerProfileSnapshot> layer_profile_snapshot(layers.size());
    std::vector<bool> layer_profile_dirty(layers.size(), true);
    std::atomic<size_t> render_fill_attempts_last_frame{0};
    std::atomic<size_t> render_fill_success_last_frame{0};
    std::atomic<size_t> render_fill_no_triangles_last_frame{0};
    std::atomic<size_t> render_fill_bad_indices_last_frame{0};
    std::atomic<size_t> visible_vacant_parcels_last_frame{0};
    std::atomic<size_t> vacant_parcels_matched_total{0};
    std::atomic<size_t> vacant_parcels_with_geometry_total{0};
    std::atomic<size_t> vacant_parcels_triangulated_renderable_total{0};
    std::atomic<size_t> vacant_notice_rows_matched_total{0};
    std::atomic<size_t> vacant_rehab_rows_matched_total{0};
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
    std::vector<bool> layer_heatmap_enabled(layers.size(), true);
    std::vector<int> layer_heatmap_max_zoom(layers.size(), 13);
    std::vector<bool> layer_heatmap_use_gradient(layers.size(), true);
    std::vector<int> layer_heatmap_algo(layers.size(), -1);
    std::vector<bool> layer_heatmap_use_global_settings(layers.size(), true);
    std::vector<float> layer_heatmap_cell_px(layers.size(), 24.0f);
    std::vector<float> layer_heatmap_bandwidth_px(layers.size(), 18.0f);
    std::vector<float> layer_heatmap_blur_sigma_px(layers.size(), 6.0f);
    std::vector<float> layer_heatmap_percentile_clip(layers.size(), 95.0f);
    std::vector<bool> layer_heatmap_zoom_adaptive_bandwidth(layers.size(), true);
    std::vector<bool> layer_heatmap_multires_enabled(layers.size(), true);
    std::vector<float> layer_heatmap_multires_blend(layers.size(), 0.5f);
    // Heatmap runtime settings (loaded/saved via layer_ui_state.json)
    float global_heat_cell_px = 24.0f;
    int heatmap_algo = 0; // 0=grid,1=kde,2=gpu_splat_blur,3=hex,4=multires
    int heatmap_quality_preset = 1; // 0=fast,1=balanced,2=high
    float heatmap_bandwidth_px = 18.0f;
    float heatmap_blur_sigma_px = 6.0f;
    float heatmap_percentile_clip = 95.0f;
    bool heatmap_zoom_adaptive_bandwidth = true;
    bool heatmap_multires_enabled = true;
    float heatmap_multires_blend = 0.5f;
    struct CachedHeatCell {
        bool is_hex = false;
        bool draw_outline = true;
        float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
        float cx = 0.0f, cy = 0.0f, hw = 0.0f, hh = 0.0f;
        ImU32 fill = 0;
        ImU32 outline = 0;
    };
    struct HeatmapRaster {
        int w = 0;
        int h = 0;
        float min_lon = 0.0f;
        float min_lat = 0.0f;
        float max_lon = 0.0f;
        float max_lat = 0.0f;
        std::vector<unsigned char> rgba;
    };
    struct HeatmapRenderData {
        std::vector<CachedHeatCell> cells;
        HeatmapRaster raster;
        bool has_raster = false;
    };
    std::vector<CachedHeatCell> heatmap_cached_cells;
    TileTexture heatmap_raster_texture;
    bool heatmap_raster_texture_valid = false;
    HeatmapRaster heatmap_cached_raster_meta;
    uint64_t heatmap_raster_cache_key = 0;
    uint64_t heatmap_cache_key = 0;
    bool heatmap_cache_valid = false;
    std::future<std::pair<uint64_t, HeatmapRenderData>> heatmap_async_future;
    bool heatmap_async_inflight = false;
    bool hover_inspector_enabled = true;
    loadLayerUiState(
        root,
        layers,
        hover_inspector_enabled,
        &zoning_zone_enabled,
        &layer_fill_enabled,
        &layer_hover_enabled,
        &layer_inspect_enabled,
        &layer_heatmap_enabled,
        &layer_heatmap_max_zoom,
        &layer_heatmap_use_gradient,
        &layer_heatmap_algo,
        &layer_heatmap_use_global_settings,
        &layer_heatmap_cell_px,
        &layer_heatmap_bandwidth_px,
        &layer_heatmap_blur_sigma_px,
        &layer_heatmap_percentile_clip,
        &layer_heatmap_zoom_adaptive_bandwidth,
        &layer_heatmap_multires_enabled,
        &layer_heatmap_multires_blend,
        &heatmap_algo,
        &heatmap_quality_preset,
        &global_heat_cell_px,
        &heatmap_bandwidth_px,
        &heatmap_blur_sigma_px,
        &heatmap_percentile_clip,
        &heatmap_zoom_adaptive_bandwidth,
        &heatmap_multires_enabled,
        &heatmap_multires_blend);
    TimeCubeService time_cube_service(root);
    std::mutex status_mutex;
    std::vector<LayerRuntimeState> layer_states(layers.size());
    std::vector<LayerSpatialIndex> layer_spatial(layers.size());
    std::vector<uint32_t> render_candidates;
    struct OwnerAggregate {
        std::string owner;
        size_t property_count = 0;
        double area_m2 = 0.0;
        double value_usd = 0.0;
    };
    std::vector<OwnerAggregate> owner_aggregates;
    bool owner_aggregates_dirty = true;
    int owner_sort_mode = 0; // 0=count,1=area,2=value
    int owner_sorted_mode = -1;
    size_t owner_cached_parcel_size = (size_t)-1;
    size_t owner_cached_real_property_size = (size_t)-1;
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
        if (!ensureSocketRuntimeInitialized()) return;
        int server_fd = (int)::socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0) return;
        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(8787);
        if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
            netClose(server_fd);
            return;
        }
        if (listen(server_fd, 8) != 0) {
            netClose(server_fd);
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
            int client_fd = (int)accept(server_fd, nullptr, nullptr);
            if (client_fd < 0) continue;

            char buf[1024];
            ssize_t n = netRead(client_fd, buf, sizeof(buf) - 1);
            if (n <= 0) {
                netClose(client_fd);
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
            auto send_json = [&](int code, const char* reason, const json& out) {
                std::string body = out.dump();
                std::ostringstream os;
                os << "HTTP/1.1 " << code << " " << reason << "\r\n"
                   << "Content-Type: application/json\r\n"
                   << "Content-Length: " << body.size() << "\r\n"
                   << "Connection: close\r\n\r\n"
                   << body;
                std::string resp = os.str();
                (void)writeAll(client_fd, resp.data(), resp.size());
            };
            auto build_layer_profile = [&]() {
                json layer_list = json::array();
                size_t total_features = 0;
                size_t total_rings = 0;
                size_t total_points = 0;
                size_t total_tri_indices = 0;
                size_t total_properties = 0;
                std::lock_guard<std::mutex> profile_lk(layer_profile_mutex);
                for (const auto& layer : layer_profile_snapshot) {
                    total_features += layer.features;
                    total_rings += layer.rings;
                    total_points += layer.ring_points;
                    total_tri_indices += layer.triangle_indices;
                    total_properties += layer.properties;
                    layer_list.push_back({
                        {"index", layer.index},
                        {"name", layer.name},
                        {"file", layer.file},
                        {"enabled", layer.enabled},
                        {"features", layer.features},
                        {"rings", layer.rings},
                        {"ring_points", layer.ring_points},
                        {"triangle_indices", layer.triangle_indices},
                        {"properties", layer.properties},
                        {"spatial_index_built", layer.spatial_index_built},
                        {"spatial_index_cells", layer.spatial_index_cells},
                        {"spatial_index_marks", layer.spatial_index_marks}
                    });
                }
                json out;
                out["layers"] = std::move(layer_list);
                out["totals"] = {
                    {"features", total_features},
                    {"rings", total_rings},
                    {"ring_points", total_points},
                    {"triangle_indices", total_tri_indices},
                    {"properties", total_properties}
                };
                return out;
            };
            auto summarize_ms = [](const std::vector<double>& values) {
                json out;
                out["count"] = values.size();
                if (values.empty()) {
                    out["avg"] = 0.0;
                    out["p50"] = 0.0;
                    out["p95"] = 0.0;
                    out["max"] = 0.0;
                    return out;
                }
                std::vector<double> sorted = values;
                std::sort(sorted.begin(), sorted.end());
                double sum = 0.0;
                for (double v : sorted) sum += v;
                auto pct = [&](double p) {
                    size_t idx = (size_t)std::floor((p * (double)(sorted.size() - 1)) + 0.5);
                    return sorted[std::min(idx, sorted.size() - 1)];
                };
                out["avg"] = sum / (double)sorted.size();
                out["p50"] = pct(0.50);
                out["p95"] = pct(0.95);
                out["max"] = sorted.back();
                return out;
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
            } else if (path == "/time_cube") {
                auto parse_year_q = [&](const char* key, int fallback) {
                    const std::string raw = get_q(key);
                    if (raw.empty()) return fallback;
                    try {
                        return std::clamp(std::stoi(raw), 1900, 2100);
                    } catch (...) {
                        return fallback;
                    }
                };
                const std::string layer_filter = urlDecode(get_q("layer"));
                int year_from = parse_year_q("from", 1900);
                int year_to = parse_year_q("to", 2100);
                if (year_from > year_to) std::swap(year_from, year_to);
                const std::string enabled_q = toLowerAscii(urlDecode(get_q("enabled")));
                const bool enabled_only = (enabled_q == "1" || enabled_q == "true" || enabled_q == "on" || enabled_q == "yes");
                const std::string rebuild_q = toLowerAscii(urlDecode(get_q("rebuild")));
                const bool rebuild = (rebuild_q == "1" || rebuild_q == "true" || rebuild_q == "on" || rebuild_q == "yes");
                TimeCubeQuery cube_query;
                cube_query.layer_filter = layer_filter;
                cube_query.year_from = year_from;
                cube_query.year_to = year_to;
                cube_query.enabled_only = enabled_only;
                cube_query.rebuild = rebuild;
                json out = time_cube_service.toJson(time_cube_service.query(layers, cube_query));

                std::string body = out.dump();
                std::ostringstream os;
                os << "HTTP/1.1 200 OK\r\n"
                   << "Content-Type: application/json\r\n"
                   << "Content-Length: " << body.size() << "\r\n"
                   << "Connection: close\r\n\r\n"
                   << body;
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
                out["app_version"] = kAppVersion;
                out["protocol_version"] = kProtocolVersion;
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
                out["tile_cache_size"] = prof_tile_cache_size.load(std::memory_order_relaxed);
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
            } else if (path == "/profile" || path == "/profile/layers" || path == "/profile/reset") {
                if (path == "/profile/reset") {
                    {
                        std::lock_guard<std::mutex> lk(profile_mutex);
                        profile_sample_pos = 0;
                        profile_sample_count = 0;
                        profile_reset_generation++;
                    }
                    send_json(200, "OK", {{"ok", true}, {"profile_generation", profile_reset_generation}});
                    netClose(client_fd);
                    continue;
                }
                if (path == "/profile/layers") {
                    json out = build_layer_profile();
                    out["ok"] = true;
                    send_json(200, "OK", out);
                    netClose(client_fd);
                    continue;
                }
                double user_cpu_seconds = 0.0;
                double system_cpu_seconds = 0.0;
                long long max_rss_kb = 0;
                long long voluntary_context_switches = 0;
                long long involuntary_context_switches = 0;
#ifndef _WIN32
                struct rusage ru{};
                getrusage(RUSAGE_SELF, &ru);
                auto tv_sec_d = [](const timeval& tv) {
                    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
                };
                user_cpu_seconds = tv_sec_d(ru.ru_utime);
                system_cpu_seconds = tv_sec_d(ru.ru_stime);
                max_rss_kb = (long long)ru.ru_maxrss;
                voluntary_context_switches = (long long)ru.ru_nvcsw;
                involuntary_context_switches = (long long)ru.ru_nivcsw;
#endif
                size_t vm_rss_kb = 0;
                size_t threads = 0;
#ifndef _WIN32
                {
                    std::ifstream status_in("/proc/self/status");
                    std::string key;
                    while (status_in >> key) {
                        if (key == "VmRSS:") {
                            status_in >> vm_rss_kb;
                        } else if (key == "Threads:") {
                            status_in >> threads;
                        }
                        std::string rest;
                        std::getline(status_in, rest);
                    }
                }
#endif
                std::vector<ProfileFrameSample> samples;
                uint64_t profile_generation = 0;
                {
                    std::lock_guard<std::mutex> lk(profile_mutex);
                    samples.reserve(profile_sample_count);
                    const size_t start = profile_sample_count < profile_samples.size() ? 0 : profile_sample_pos;
                    for (size_t i = 0; i < profile_sample_count; ++i) {
                        samples.push_back(profile_samples[(start + i) % profile_samples.size()]);
                    }
                    profile_generation = profile_reset_generation;
                }
                auto phase_values = [&](double ProfileFrameSample::*field) {
                    std::vector<double> values;
                    values.reserve(samples.size());
                    for (const auto& s : samples) values.push_back(s.*field);
                    return values;
                };
                json out;
                out["ok"] = true;
                out["profile_generation"] = profile_generation;
                out["process"] = {
                    {"pid", (int)getpid()},
                    {"vm_rss_kb", vm_rss_kb},
                    {"threads", threads},
                    {"user_cpu_seconds", user_cpu_seconds},
                    {"system_cpu_seconds", system_cpu_seconds},
                    {"max_rss_kb", max_rss_kb},
                    {"voluntary_context_switches", voluntary_context_switches},
                    {"involuntary_context_switches", involuntary_context_switches}
                };
                out["frame"] = {
                    {"frame_ms_avg", perf_frame_ms_avg.load(std::memory_order_relaxed)},
                    {"frame_ms_last", perf_frame_ms_last.load(std::memory_order_relaxed)},
                    {"fps_avg", perf_fps_avg.load(std::memory_order_relaxed)}
                };
                out["history"] = {
                    {"capacity_frames", profile_samples.size()},
                    {"sample_count", samples.size()},
                    {"frame_ms", summarize_ms(phase_values(&ProfileFrameSample::frame_ms))},
                    {"ui_total_ms", summarize_ms(phase_values(&ProfileFrameSample::ui_total_ms))},
                    {"owner_aggregate_ms", summarize_ms(phase_values(&ProfileFrameSample::owner_aggregate_ms))},
                    {"tiles_ms", summarize_ms(phase_values(&ProfileFrameSample::tiles_ms))},
                    {"layers_ms", summarize_ms(phase_values(&ProfileFrameSample::layers_ms))},
                    {"heatmap_ms", summarize_ms(phase_values(&ProfileFrameSample::heatmap_ms))},
                    {"overlays_ms", summarize_ms(phase_values(&ProfileFrameSample::overlays_ms))},
                    {"render_present_ms", summarize_ms(phase_values(&ProfileFrameSample::render_present_ms))}
                };
                out["phases_ms_last"] = {
                    {"ui_total", prof_ui_ms_last.load(std::memory_order_relaxed)},
                    {"owner_aggregate", prof_owner_ms_last.load(std::memory_order_relaxed)},
                    {"tiles", prof_tile_ms_last.load(std::memory_order_relaxed)},
                    {"layers", prof_layer_ms_last.load(std::memory_order_relaxed)},
                    {"heatmap", prof_heatmap_ms_last.load(std::memory_order_relaxed)},
                    {"overlays", prof_overlay_ms_last.load(std::memory_order_relaxed)},
                    {"render_present", prof_present_ms_last.load(std::memory_order_relaxed)}
                };
                out["counts_last_frame"] = {
                    {"tiles_drawn", prof_tiles_drawn_last.load(std::memory_order_relaxed)},
                    {"features_considered", prof_features_considered_last.load(std::memory_order_relaxed)},
                    {"features_drawn_points", prof_features_drawn_last.load(std::memory_order_relaxed)},
                    {"heat_samples", prof_heat_samples_last.load(std::memory_order_relaxed)},
                    {"retired_textures", prof_retired_textures.load(std::memory_order_relaxed)},
                    {"tile_cache_size", prof_tile_cache_size.load(std::memory_order_relaxed)},
                    {"tile_cache_max", kMaxTileCache}
                };
                json layer_profile = build_layer_profile();
                out["layers"] = std::move(layer_profile["layers"]);
                out["totals"] = std::move(layer_profile["totals"]);
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
            netClose(client_fd);
        }
        netClose(server_fd);
    });
    std::mutex p2p_mutex;
    std::unordered_map<std::string, std::vector<json>> p2p_mailbox;
    std::thread dataset_api_worker([&]() {
        if (!ensureSocketRuntimeInitialized()) return;
        int server_fd = (int)::socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0) return;
        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(8788);
        if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
            netClose(server_fd);
            return;
        }
        if (listen(server_fd, 16) != 0) {
            netClose(server_fd);
            return;
        }
        auto parse_q = [](const std::string& query, const std::string& key) -> std::string {
            size_t pos = 0;
            while (pos < query.size()) {
                size_t amp = query.find('&', pos);
                std::string kv = query.substr(pos, (amp == std::string::npos ? query.size() : amp) - pos);
                size_t eq = kv.find('=');
                std::string k = eq == std::string::npos ? kv : kv.substr(0, eq);
                if (k == key) return eq == std::string::npos ? std::string() : urlDecode(kv.substr(eq + 1));
                if (amp == std::string::npos) break;
                pos = amp + 1;
            }
            return {};
        };
        auto send_json = [&](int cfd, int code, const json& j) {
            const std::string body = j.dump();
            const char* reason = code == 200 ? "OK" : (code == 404 ? "Not Found" : "Bad Request");
            std::ostringstream os;
            os << "HTTP/1.1 " << code << " " << reason << "\r\n"
               << "Content-Type: application/json\r\n"
               << "Access-Control-Allow-Origin: *\r\n"
               << "Content-Length: " << body.size() << "\r\n"
               << "Connection: close\r\n\r\n" << body;
            const std::string resp = os.str();
            (void)writeAll(cfd, resp.data(), resp.size());
        };
        while (!hydration_stop.load(std::memory_order_relaxed)) {
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(server_fd, &readfds);
            timeval tv{};
            tv.tv_sec = 1;
            int sel = select(server_fd + 1, &readfds, nullptr, nullptr, &tv);
            if (sel <= 0) continue;
            int client_fd = (int)accept(server_fd, nullptr, nullptr);
            if (client_fd < 0) continue;

            char buf[4096];
            ssize_t n = netRead(client_fd, buf, sizeof(buf) - 1);
            if (n <= 0) {
                netClose(client_fd);
                continue;
            }
            buf[n] = '\0';
            std::string req(buf);
            size_t method_sp = req.find(' ');
            size_t path_sp = method_sp == std::string::npos ? std::string::npos : req.find(' ', method_sp + 1);
            std::string path_q = (method_sp != std::string::npos && path_sp != std::string::npos)
                ? req.substr(method_sp + 1, path_sp - method_sp - 1)
                : "/";
            size_t qpos = path_q.find('?');
            std::string path = path_q.substr(0, qpos);
            std::string query = qpos == std::string::npos ? std::string() : path_q.substr(qpos + 1);

            if (path == "/datasets") {
                json out;
                out["ok"] = true;
                out["app_version"] = kAppVersion;
                out["protocol_version"] = kProtocolVersion;
                out["root"] = (root / "data").string();
                out["files"] = json::array();
                std::error_code ec;
                for (fs::recursive_directory_iterator it(root / "data", ec), end; it != end && !ec; it.increment(ec)) {
                    if (ec || !it->is_regular_file()) continue;
                    const fs::path p = it->path();
                    json row;
                    row["path"] = fs::relative(p, root).string();
                    row["size_bytes"] = (uint64_t)it->file_size();
                    row["mtime_unix"] = (int64_t)std::chrono::duration_cast<std::chrono::seconds>(
                        it->last_write_time().time_since_epoch()).count();
                    out["files"].push_back(std::move(row));
                }
                send_json(client_fd, 200, out);
            } else if (path == "/dataset/file") {
                const std::string rel = parse_q(query, "path");
                fs::path p = root / rel;
                std::error_code ec;
                p = fs::weakly_canonical(p, ec);
                const fs::path data_root = fs::weakly_canonical(root / "data", ec);
                bool ok_path = !ec && !rel.empty() &&
                    p.string().find(data_root.string()) == 0 &&
                    fs::exists(p) && fs::is_regular_file(p);
                if (!ok_path) {
                    send_json(client_fd, 404, json{{"ok", false}, {"error", "file not found"}});
                } else {
                    std::ifstream in(p, std::ios::binary);
                    if (!in) {
                        send_json(client_fd, 404, json{{"ok", false}, {"error", "file open failed"}});
                    } else {
                        const uint64_t fsz = (uint64_t)fs::file_size(p);
                        std::ostringstream hs;
                        hs << "HTTP/1.1 200 OK\r\n"
                           << "Content-Type: application/octet-stream\r\n"
                           << "Access-Control-Allow-Origin: *\r\n"
                           << "Content-Length: " << fsz << "\r\n"
                           << "Connection: close\r\n\r\n";
                        const std::string hdr = hs.str();
                        if (writeAll(client_fd, hdr.data(), hdr.size())) {
                            char fbuf[1 << 15];
                            while (in.good()) {
                                in.read(fbuf, sizeof(fbuf));
                                std::streamsize got = in.gcount();
                                if (got > 0 && !writeAll(client_fd, fbuf, (size_t)got)) break;
                            }
                        }
                    }
                }
            } else if (path == "/p2p/register") {
                const std::string peer = parse_q(query, "peer");
                if (peer.empty()) send_json(client_fd, 400, json{{"ok", false}, {"error", "missing peer"}});
                else {
                    std::lock_guard<std::mutex> lk(p2p_mutex);
                    p2p_mailbox[peer];
                    send_json(client_fd, 200, json{{"ok", true}, {"peer", peer}});
                }
            } else if (path == "/p2p/publish") {
                const std::string to = parse_q(query, "to");
                const std::string from = parse_q(query, "from");
                const std::string type = parse_q(query, "type");
                const std::string payload = parse_q(query, "payload");
                if (to.empty() || from.empty() || type.empty()) {
                    send_json(client_fd, 400, json{{"ok", false}, {"error", "missing to/from/type"}});
                } else {
                    json msg{
                        {"from", from},
                        {"type", type},
                        {"payload", payload},
                        {"ts_unix", std::time(nullptr)}
                    };
                    std::lock_guard<std::mutex> lk(p2p_mutex);
                    p2p_mailbox[to].push_back(std::move(msg));
                    send_json(client_fd, 200, json{{"ok", true}});
                }
            } else if (path == "/p2p/poll") {
                const std::string peer = parse_q(query, "peer");
                if (peer.empty()) send_json(client_fd, 400, json{{"ok", false}, {"error", "missing peer"}});
                else {
                    json out;
                    out["ok"] = true;
                    out["peer"] = peer;
                    out["messages"] = json::array();
                    {
                        std::lock_guard<std::mutex> lk(p2p_mutex);
                        auto it = p2p_mailbox.find(peer);
                        if (it != p2p_mailbox.end()) {
                            out["messages"] = it->second;
                            it->second.clear();
                        }
                    }
                    send_json(client_fd, 200, out);
                }
            } else {
                send_json(client_fd, 404, json{{"ok", false}, {"error", "not found"}});
            }
            netClose(client_fd);
        }
        netClose(server_fd);
    });
    std::thread lan_discovery_worker([&]() {
        if (!ensureSocketRuntimeInitialized()) return;
        int sock = (int)::socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) return;
        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(8789);
        if (bind(sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
            netClose(sock);
            return;
        }
        while (!hydration_stop.load(std::memory_order_relaxed)) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(sock, &rfds);
            timeval tv{};
            tv.tv_sec = 1;
            int sel = select(sock + 1, &rfds, nullptr, nullptr, &tv);
            if (sel <= 0) continue;
            sockaddr_in src{};
            #ifdef _WIN32
            int slen = sizeof(src);
            #else
            socklen_t slen = sizeof(src);
            #endif
            char buf[2048];
            ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0, (sockaddr*)&src, &slen);
            if (n <= 0) continue;
            buf[n] = '\0';
            std::string msg(buf);
            if (msg.find("WS3_DISCOVER_V1") != 0) continue;
            char ipbuf[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &src.sin_addr, ipbuf, sizeof(ipbuf));
            json out{
                {"ok", true},
                {"app", "worldsim3"},
                {"app_version", kAppVersion},
                {"protocol_version", kProtocolVersion},
                {"status_port", 8787},
                {"dataset_port", 8788},
                {"discovery_port", 8789},
                {"ip", std::string(ipbuf)}
            };
            std::string body = out.dump();
            (void)sendto(sock, body.data(), body.size(), 0, (sockaddr*)&src, slen);
        }
        netClose(sock);
    });

    int zoom = 12;
    double center_lon = -76.6122;
    double center_lat = 39.2904;
    std::vector<bool> last_enabled_state;
    last_enabled_state.reserve(layers.size());
    for (const auto& l : layers) last_enabled_state.push_back(l.enabled);
    bool last_hover_inspector_enabled = hover_inspector_enabled;
    bool show_sources_panel = false;
    bool show_data_library = false;
    char data_library_query[128] = "";
    std::string data_library_status_msg;
    std::string data_library_cached_query;
    size_t data_library_cached_layer_count = 0;
    std::vector<size_t> data_library_visible_rows;
    size_t data_library_cache_rebuilds = 0;
    size_t data_library_rendered_rows_last = 0;
    std::vector<FreshnessState> data_freshness_state(layers.size(), FreshnessState::Unknown);
    std::vector<std::string> data_freshness_msg(layers.size(), "");
    bool filter_enabled = false;
    bool filter_use_date = false;
    int filter_year_min = 2000;
    int filter_year_max = 2026;
    char filter_blocklot[64] = "";
    char filter_status[64] = "";
    char filter_address[96] = "";
    char filter_owner[96] = "";
    char filter_zip[24] = "";
    bool crime_filter_enabled = false;
    bool crime_filter_homicide = false;
    bool crime_filter_robbery = false;
    bool crime_filter_assault = false;
    bool crime_filter_burglary = false;
    bool crime_filter_theft = false;
    bool crime_filter_auto_theft = false;
    bool crime_filter_drug = false;
    bool crime_filter_shooting = false;
    bool crime_filter_use_year = false;
    int crime_year_min = 2022;
    int crime_year_max = 2026;
    std::vector<std::pair<std::string, int>> crime_breakdown;
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
    TimeCubeResult time_cube_ui_result;
    bool time_cube_ui_loaded = false;
    std::string time_cube_ui_status = "Not indexed in this session";
    std::mutex time_cube_ui_mutex;
    std::thread time_cube_ui_worker;
    std::atomic<bool> time_cube_ui_running{false};
    std::atomic<bool> time_cube_ui_done{false};
    std::vector<bool> time_cube_selected(layers.size(), true);
    int time_cube_year_min = 2020;
    int time_cube_year_max = 2026;
    int time_cube_normalize_mode = 0; // 0=raw,1=index first year to 100,2=percent of max
    bool time_cube_show_excluded = false;
    json policy_hierarchy;
    bool policy_hierarchy_loaded = false;
    std::string policy_hierarchy_error;
    {
        std::ifstream in(root / "data" / "government" / "government_hierarchy_and_pay_2026.json");
        if (in) {
            try {
                in >> policy_hierarchy;
                policy_hierarchy_loaded = true;
            } catch (const std::exception& e) {
                policy_hierarchy_error = e.what();
            }
        } else {
            policy_hierarchy_error = "missing data/government/government_hierarchy_and_pay_2026.json";
        }
    }
    char policy_hierarchy_query[128] = "";
    int policy_hierarchy_scope = 0; // 0=all,1=maryland,2=baltimore,3=federal
    struct PublicServantRosterRow {
        std::string source_id;
        std::string source_type;
        std::string jurisdiction;
        std::string employer;
        std::string agency;
        std::string person_name;
        std::string role_title;
        std::string pay_grade;
        std::string annual_salary;
        std::string gross_pay;
        std::string fiscal_year;
        std::string source_url;
        std::string provenance_note;
    };
    std::vector<PublicServantRosterRow> public_servant_roster;
    {
        std::ifstream in(root / "data" / "public_servants" / "normalized_public_servants.jsonl");
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            try {
                json row = json::parse(line);
                PublicServantRosterRow out;
                out.source_id = row.value("source_id", "");
                out.source_type = row.value("source_type", "");
                out.jurisdiction = row.value("jurisdiction", "");
                out.employer = row.value("employer", "");
                out.agency = row.value("agency", "");
                out.person_name = row.value("person_name", "");
                out.role_title = row.value("role_title", "");
                out.pay_grade = row.value("pay_grade", "");
                out.annual_salary = row.value("annual_salary", "");
                out.gross_pay = row.value("gross_pay", "");
                out.fiscal_year = row.value("fiscal_year", "");
                out.source_url = row.value("source_url", "");
                out.provenance_note = row.value("provenance_note", "");
                public_servant_roster.push_back(std::move(out));
            } catch (...) {
            }
        }
    }
    std::string people_pay_cached_query;
    int people_pay_cached_scope = -1;
    size_t people_pay_cache_matched_count = 0;
    std::vector<size_t> people_pay_visible_rows;
    size_t people_pay_cache_rebuilds = 0;
    size_t people_pay_rendered_rows_last = 0;
    struct PolicyVizNode {
        std::string label;
        size_t personnel = 0;
        double pay_total = 0.0;
        double value = 0.0;
        std::vector<PolicyVizNode> children;
    };
    PolicyVizNode policy_viz_root;
    std::string policy_viz_cached_query;
    int policy_viz_cached_scope = -1;
    int policy_viz_cached_metric = -1;
    int policy_viz_metric = 0; // 0=personnel count, 1=pay total
    size_t policy_viz_cache_rebuilds = 0;
    size_t policy_viz_node_count = 0;
    struct LanPeerInfo {
        std::string ip;
        std::string app_version;
        int protocol_version = 0;
        int dataset_port = 0;
        bool protocol_match = false;
    };
    std::vector<LanPeerInfo> lan_peers;
    std::string lan_scan_status = "Not scanned";
    char arkavo_room_id[128] = "worldsim-default-room";
    std::string arkavo_status = "idle";
    std::string arkavo_err;
    std::unique_ptr<ArkavoRealtimeClient> arkavo_client;
    std::unique_ptr<ArkavoRtcSessionManager> arkavo_rtc;
    char arkavo_send_peer[160] = "";
    char arkavo_send_path[512] = "";

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
        drainRetiredTextures();
        const auto prof_frame_begin = std::chrono::steady_clock::now();
        auto prof_ms_since = [](std::chrono::steady_clock::time_point t) {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
        };
        size_t prof_tiles_drawn_frame = 0;
        size_t prof_features_considered_frame = 0;
        size_t prof_features_drawn_frame = 0;
        bool layer_fill_state_changed = false;
        bool layer_hover_state_changed = false;
        bool layer_inspect_state_changed = false;
        bool layer_heatmap_state_changed = false;
        bool heatmap_settings_state_changed = false;
        bool heatmap_controls_active = false;
        auto heatmap_input_float_enter = [&](const char* label, float& value, float min_value, float max_value, const char* format) {
            static std::unordered_map<ImGuiID, float> drafts;
            static std::unordered_map<ImGuiID, bool> was_active;
            const ImGuiID id = ImGui::GetID(label);
            if (!was_active[id]) drafts[id] = value;
            float draft = drafts[id];
            const bool committed = ImGui::InputFloat(label, &draft, 0.0f, 0.0f, format, ImGuiInputTextFlags_EnterReturnsTrue);
            const bool active = ImGui::IsItemActive();
            heatmap_controls_active |= active;
            if (committed) {
                value = std::clamp(draft, min_value, max_value);
                drafts[id] = value;
                was_active[id] = active;
                return true;
            }
            drafts[id] = active ? draft : value;
            was_active[id] = active;
            return false;
        };
        auto refresh_layer_profile_snapshot = [&]() {
            bool any_dirty = false;
            for (bool dirty : layer_profile_dirty) {
                if (dirty) {
                    any_dirty = true;
                    break;
                }
            }
            if (!any_dirty) return;
            std::vector<LayerProfileSnapshot> updates;
            updates.reserve(layers.size());
            for (size_t i = 0; i < layers.size(); ++i) {
                if (i >= layer_profile_dirty.size() || !layer_profile_dirty[i]) continue;
                const auto& layer = layers[i];
                LayerProfileSnapshot s;
                s.index = i;
                s.name = layer.name;
                s.file = layer.file;
                s.enabled = layer.enabled;
                s.features = layer.features.size();
                for (const auto& fg : layer.features) {
                    s.rings += fg.rings.size();
                    s.triangle_indices += fg.triangles.size();
                    s.properties += fg.properties.size();
                    for (const auto& r : fg.rings) s.ring_points += r.size();
                }
                if (i < layer_spatial.size()) {
                    s.spatial_index_built = layer_spatial[i].built;
                    s.spatial_index_cells = layer_spatial[i].cells.size();
                    s.spatial_index_marks = layer_spatial[i].marks.size();
                }
                updates.push_back(std::move(s));
            }
            {
                std::lock_guard<std::mutex> lk(layer_profile_mutex);
                for (const auto& s : updates) {
                    if (s.index < layer_profile_snapshot.size()) layer_profile_snapshot[s.index] = s;
                }
            }
            std::fill(layer_profile_dirty.begin(), layer_profile_dirty.end(), false);
        };
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
                for (size_t i = 0; i < layers.size(); ++i) {
                    if (layers[i].file == kv.first) {
                        layers[i].enabled = kv.second;
                        if (i < layer_profile_dirty.size()) layer_profile_dirty[i] = true;
                    }
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
        if (ImGui::Button("Library")) show_data_library = !show_data_library;
        ImGui::SameLine();
        ImGui::Text("Vulkan map + Vulkan UI");
        size_t local_layer_count = 0;
        for (const auto& l : layers) {
            if (fs::exists(root / "data" / "layers" / l.file)) local_layer_count++;
        }
        ImGui::TextDisabled("Local data: %zu/%zu", local_layer_count, layers.size());
        ImGui::SliderInt("Zoom", &zoom, kMinZoom, kMaxZoom);
        const double lon_min = -76.75, lon_max = -76.45;
        const double lat_min = 39.18, lat_max = 39.40;
        ImGui::SliderScalar("Center Lon", ImGuiDataType_Double, &center_lon, &lon_min, &lon_max, "%.6f");
        ImGui::SliderScalar("Center Lat", ImGuiDataType_Double, &center_lat, &lat_min, &lat_max, "%.6f");
        ImGui::Checkbox("Enable Hover Inspector", &hover_inspector_enabled);
        ImGui::SeparatorText("Heatmap");
        const char* heatmap_algo_items[] = {
            "Grid Binning",
            "KDE (Gaussian)",
            "GPU Splat + Blur",
            "Hex Binning",
            "Multi-res Pyramid"
        };
        const char* heatmap_quality_items[] = {"Fast", "Balanced", "High"};
        int heatmap_quality_prev = heatmap_quality_preset;
        heatmap_settings_state_changed |= ImGui::Combo("Heatmap Quality", &heatmap_quality_preset, heatmap_quality_items, IM_ARRAYSIZE(heatmap_quality_items));
        heatmap_controls_active |= ImGui::IsItemActive();
        if (heatmap_quality_preset != heatmap_quality_prev) {
            if (heatmap_quality_preset == 0) {
                global_heat_cell_px = 36.0f;
                heatmap_bandwidth_px = 10.0f;
                heatmap_blur_sigma_px = 3.0f;
                heatmap_percentile_clip = 92.0f;
                heatmap_multires_enabled = false;
            } else if (heatmap_quality_preset == 1) {
                global_heat_cell_px = 24.0f;
                heatmap_bandwidth_px = 18.0f;
                heatmap_blur_sigma_px = 6.0f;
                heatmap_percentile_clip = 95.0f;
                heatmap_multires_enabled = true;
                heatmap_multires_blend = 0.5f;
            } else {
                global_heat_cell_px = 14.0f;
                heatmap_bandwidth_px = 26.0f;
                heatmap_blur_sigma_px = 10.0f;
                heatmap_percentile_clip = 97.0f;
                heatmap_multires_enabled = true;
                heatmap_multires_blend = 0.65f;
            }
            heatmap_settings_state_changed = true;
        }
        heatmap_settings_state_changed |= ImGui::Combo("Heatmap Algorithm", &heatmap_algo, heatmap_algo_items, IM_ARRAYSIZE(heatmap_algo_items));
        ImGui::SameLine();
        if (ImGui::SmallButton("Apply Global To All")) {
            for (size_t i = 0; i < layer_heatmap_algo.size(); ++i) {
                if (i < layer_heatmap_enabled.size()) layer_heatmap_enabled[i] = true;
                layer_heatmap_algo[i] = -1; // Use global method
                if (i < layer_heatmap_use_global_settings.size()) layer_heatmap_use_global_settings[i] = true;
            }
            layer_heatmap_state_changed = true;
            heatmap_settings_state_changed = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Clear per-layer overrides and force all heatmap layers to use global settings.");
            ImGui::EndTooltip();
        }
        heatmap_controls_active |= ImGui::IsItemActive();
        heatmap_settings_state_changed |= heatmap_input_float_enter("Heatmap Cell (px)", global_heat_cell_px, 8.0f, 80.0f, "%.0f");
        heatmap_settings_state_changed |= heatmap_input_float_enter("Bandwidth (px)", heatmap_bandwidth_px, 2.0f, 96.0f, "%.1f");
        heatmap_settings_state_changed |= heatmap_input_float_enter("Blur Sigma (px)", heatmap_blur_sigma_px, 0.0f, 32.0f, "%.1f");
        heatmap_settings_state_changed |= heatmap_input_float_enter("Normalization Clip (%)", heatmap_percentile_clip, 50.0f, 100.0f, "%.0f");
        heatmap_settings_state_changed |= ImGui::Checkbox("Zoom-adaptive bandwidth", &heatmap_zoom_adaptive_bandwidth);
        heatmap_controls_active |= ImGui::IsItemActive();
        heatmap_settings_state_changed |= ImGui::Checkbox("Enable multi-res blending", &heatmap_multires_enabled);
        heatmap_controls_active |= ImGui::IsItemActive();
        ImGui::BeginDisabled(!heatmap_multires_enabled);
        heatmap_settings_state_changed |= heatmap_input_float_enter("Multi-res blend", heatmap_multires_blend, 0.0f, 1.0f, "%.2f");
        heatmap_controls_active |= ImGui::IsItemActive();
        ImGui::EndDisabled();
        bool validation_ui = g_EnableValidationLayers;
        if (ImGui::Checkbox("Vulkan Validation (restart required)", &validation_ui)) {
            g_EnableValidationLayers = validation_ui;
            app_settings.vulkan_validation_enabled = g_EnableValidationLayers;
            saveAppSettings(root, app_settings);
        }
        if (ImGui::Checkbox("Greyscale underlying map", &app_settings.grayscale_basemap)) {
            saveAppSettings(root, app_settings);
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
            std::string current_subcategory;
            for (auto& l : layers) {
                size_t idx = (size_t)(&l - &layers[0]);
                if (l.category != cat) continue;
                if (l.subcategory != current_subcategory) {
                    current_subcategory = l.subcategory;
                    if (!current_subcategory.empty()) ImGui::SeparatorText(current_subcategory.c_str());
                }
                ImGui::PushID((int)idx);
                const fs::path local_layer_path = root / "data" / "layers" / l.file;
                const bool local_layer_exists = fs::exists(local_layer_path);
                if (!local_layer_exists) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.22f, 0.12f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.30f, 0.14f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.46f, 0.18f, 0.08f, 1.0f));
                    bool can_download = !l.source_url.empty();
                    if (ImGui::SmallButton("D")) {
                        if (can_download) {
                            VersionedDownloadResult vd = downloadUrlVersioned(
                                l.source_url,
                                local_layer_path,
                                root / "data" / "versions");
                            if (vd.ok) {
                                data_library_status_msg = (vd.not_modified ? "Checked " : "Downloaded/updated ") + l.file + " (" + vd.message + ")";
                                data_freshness_state[idx] = FreshnessState::UpToDate;
                                data_freshness_msg[idx] = vd.message;
                                enqueue_hydration(idx, true);
                            } else {
                                data_library_status_msg = "Download failed for " + l.file + ": " + vd.message;
                                data_freshness_state[idx] = FreshnessState::Error;
                                data_freshness_msg[idx] = vd.message;
                            }
                        } else {
                            data_library_status_msg = "No source URL for " + l.file;
                        }
                    }
                    ImGui::PopStyleColor(3);
                    if (ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        ImGui::TextUnformatted("Download missing dataset");
                        ImGui::TextDisabled("%s", can_download ? "Source available" : "No source URL in manifest");
                        ImGui::EndTooltip();
                    }
                    ImGui::SameLine();
                }
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
                if (ImGui::SmallButton("⚙")) ImGui::OpenPopup("layer_display_settings");
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted("Layer display settings");
                    ImGui::EndTooltip();
                }
                if (ImGui::BeginPopup("layer_display_settings")) {
                    ImGui::TextUnformatted(l.name.c_str());
                    ImGui::Separator();

                    bool fill_flag = (idx < layer_fill_enabled.size()) ? layer_fill_enabled[idx] : true;
                    if (ImGui::Checkbox("Fill polygons", &fill_flag) && idx < layer_fill_enabled.size()) {
                        std::lock_guard<std::mutex> lk_fill(layer_fill_mutex);
                        layer_fill_enabled[idx] = fill_flag;
                        layer_fill_state_changed = true;
                    }
                    bool hover_flag = (idx < layer_hover_enabled.size()) ? layer_hover_enabled[idx] : true;
                    if (ImGui::Checkbox("Hover inspector", &hover_flag) && idx < layer_hover_enabled.size()) {
                        layer_hover_enabled[idx] = hover_flag;
                        layer_hover_state_changed = true;
                    }
                    bool inspect_flag = (idx < layer_inspect_enabled.size()) ? layer_inspect_enabled[idx] : true;
                    if (ImGui::Checkbox("Click inspect", &inspect_flag) && idx < layer_inspect_enabled.size()) {
                        layer_inspect_enabled[idx] = inspect_flag;
                        layer_inspect_state_changed = true;
                    }

                    ImGui::SeparatorText("Heatmap");
                    const char* layer_algo_items[] = {
                        "None",
                        "Use Global",
                        "Grid Binning",
                        "KDE (Gaussian)",
                        "GPU Splat + Blur",
                        "Hex Binning",
                        "Multi-res Pyramid"
                    };
                    int layer_algo_ui = 0;
                    const bool layer_heatmap_on = idx < layer_heatmap_enabled.size() ? layer_heatmap_enabled[idx] : true;
                    if (layer_heatmap_on && idx < layer_heatmap_algo.size()) layer_algo_ui = std::clamp(layer_heatmap_algo[idx] + 2, 1, 6);
                    if (ImGui::Combo("Aggregate method", &layer_algo_ui, layer_algo_items, IM_ARRAYSIZE(layer_algo_items)) && idx < layer_heatmap_algo.size()) {
                        if (idx < layer_heatmap_enabled.size()) layer_heatmap_enabled[idx] = layer_algo_ui != 0;
                        if (layer_algo_ui != 0) layer_heatmap_algo[idx] = layer_algo_ui - 2;
                        layer_heatmap_state_changed = true;
                    }
                    const bool aggregate_none = layer_algo_ui == 0;
                    const int resolved_layer_algo = (layer_algo_ui <= 1) ? heatmap_algo : (layer_algo_ui - 2);
                    auto sync_layer_heatmap_defaults = [&](size_t layer_i) {
                        if (layer_i >= layer_heatmap_cell_px.size()) return;
                        layer_heatmap_cell_px[layer_i] = global_heat_cell_px;
                        layer_heatmap_bandwidth_px[layer_i] = heatmap_bandwidth_px;
                        layer_heatmap_blur_sigma_px[layer_i] = heatmap_blur_sigma_px;
                        layer_heatmap_percentile_clip[layer_i] = heatmap_percentile_clip;
                        layer_heatmap_zoom_adaptive_bandwidth[layer_i] = heatmap_zoom_adaptive_bandwidth;
                        layer_heatmap_multires_enabled[layer_i] = heatmap_multires_enabled;
                        layer_heatmap_multires_blend[layer_i] = heatmap_multires_blend;
                    };
                    ImGui::BeginDisabled(aggregate_none);
                    bool use_global_method_settings = idx < layer_heatmap_use_global_settings.size() ? layer_heatmap_use_global_settings[idx] : true;
                    if (ImGui::Checkbox("Use global method settings", &use_global_method_settings) && idx < layer_heatmap_use_global_settings.size()) {
                        layer_heatmap_use_global_settings[idx] = use_global_method_settings;
                        if (!use_global_method_settings) sync_layer_heatmap_defaults(idx);
                        layer_heatmap_state_changed = true;
                    }
                    if (use_global_method_settings) sync_layer_heatmap_defaults(idx);
                    ImGui::BeginDisabled(use_global_method_settings);
                    ImGui::Indent();
                    if (resolved_layer_algo == 0) {
                        layer_heatmap_state_changed |= heatmap_input_float_enter("Grid cell size", layer_heatmap_cell_px[idx], 8.0f, 80.0f, "%.0f");
                        layer_heatmap_state_changed |= heatmap_input_float_enter("Grid clip", layer_heatmap_percentile_clip[idx], 50.0f, 100.0f, "%.0f");
                    } else if (resolved_layer_algo == 1) {
                        layer_heatmap_state_changed |= heatmap_input_float_enter("KDE bandwidth", layer_heatmap_bandwidth_px[idx], 2.0f, 96.0f, "%.1f");
                        bool adaptive = layer_heatmap_zoom_adaptive_bandwidth[idx];
                        if (ImGui::Checkbox("Adaptive KDE bandwidth", &adaptive)) {
                            layer_heatmap_zoom_adaptive_bandwidth[idx] = adaptive;
                            layer_heatmap_state_changed = true;
                        }
                        heatmap_controls_active |= ImGui::IsItemActive();
                        layer_heatmap_state_changed |= heatmap_input_float_enter("KDE clip", layer_heatmap_percentile_clip[idx], 50.0f, 100.0f, "%.0f");
                    } else if (resolved_layer_algo == 2) {
                        layer_heatmap_state_changed |= heatmap_input_float_enter("Splat radius", layer_heatmap_bandwidth_px[idx], 2.0f, 96.0f, "%.1f");
                        layer_heatmap_state_changed |= heatmap_input_float_enter("Blur sigma", layer_heatmap_blur_sigma_px[idx], 0.0f, 32.0f, "%.1f");
                        bool adaptive = layer_heatmap_zoom_adaptive_bandwidth[idx];
                        if (ImGui::Checkbox("Adaptive splat radius", &adaptive)) {
                            layer_heatmap_zoom_adaptive_bandwidth[idx] = adaptive;
                            layer_heatmap_state_changed = true;
                        }
                        heatmap_controls_active |= ImGui::IsItemActive();
                    } else if (resolved_layer_algo == 3) {
                        layer_heatmap_state_changed |= heatmap_input_float_enter("Hex size", layer_heatmap_cell_px[idx], 8.0f, 80.0f, "%.0f");
                        layer_heatmap_state_changed |= heatmap_input_float_enter("Hex clip", layer_heatmap_percentile_clip[idx], 50.0f, 100.0f, "%.0f");
                    } else {
                        layer_heatmap_state_changed |= heatmap_input_float_enter("Fine cell size", layer_heatmap_cell_px[idx], 8.0f, 80.0f, "%.0f");
                        bool multires_enabled = layer_heatmap_multires_enabled[idx];
                        if (ImGui::Checkbox("Enable pyramid blend", &multires_enabled)) {
                            layer_heatmap_multires_enabled[idx] = multires_enabled;
                            layer_heatmap_state_changed = true;
                        }
                        heatmap_controls_active |= ImGui::IsItemActive();
                        ImGui::BeginDisabled(!multires_enabled);
                        layer_heatmap_state_changed |= heatmap_input_float_enter("Pyramid blend", layer_heatmap_multires_blend[idx], 0.0f, 1.0f, "%.2f");
                        ImGui::EndDisabled();
                    }
                    ImGui::Unindent();
                    ImGui::EndDisabled();
                    int hz = (idx < layer_heatmap_max_zoom.size()) ? layer_heatmap_max_zoom[idx] : 13;
                    if (ImGui::SliderInt("Heatmap max zoom", &hz, kMinZoom, kMaxZoom) && idx < layer_heatmap_max_zoom.size()) {
                        layer_heatmap_max_zoom[idx] = hz;
                        layer_heatmap_state_changed = true;
                    }
                    bool use_gradient = (idx < layer_heatmap_use_gradient.size()) ? layer_heatmap_use_gradient[idx] : true;
                    if (ImGui::Checkbox("Apply gradient colors", &use_gradient) && idx < layer_heatmap_use_gradient.size()) {
                        layer_heatmap_use_gradient[idx] = use_gradient;
                        layer_heatmap_state_changed = true;
                    }
                    ImGui::EndDisabled();
                    ImGui::EndPopup();
                }
                ImGui::SameLine();
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
                    ImGui::Text("Local: %s", local_layer_exists ? "yes" : "no");
                    if (!l.subcategory.empty()) ImGui::Text("Subcategory: %s", l.subcategory.c_str());
                    if (!l.scale.empty()) ImGui::Text("Scale: %s", l.scale.c_str());
                    if (!l.heatmap_field.empty()) ImGui::Text("Heatmap Field: %s", l.heatmap_field.c_str());
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
            if (cat == LayerDef::Category::Safety) {
                ImGui::SeparatorText("Crime Filters");
                ImGui::Checkbox("Enable Crime Filter", &crime_filter_enabled);
                ImGui::Checkbox("Filter Crime Year", &crime_filter_use_year);
                ImGui::BeginDisabled(!crime_filter_use_year);
                ImGui::SliderInt("Crime Year Min", &crime_year_min, 1900, 2100);
                ImGui::SliderInt("Crime Year Max", &crime_year_max, 1900, 2100);
                if (crime_year_min > crime_year_max) std::swap(crime_year_min, crime_year_max);
                ImGui::EndDisabled();
                ImGui::Checkbox("Homicide", &crime_filter_homicide); ImGui::SameLine();
                ImGui::Checkbox("Robbery", &crime_filter_robbery);
                ImGui::Checkbox("Assault", &crime_filter_assault); ImGui::SameLine();
                ImGui::Checkbox("Burglary", &crime_filter_burglary);
                ImGui::Checkbox("Theft/Larceny", &crime_filter_theft); ImGui::SameLine();
                ImGui::Checkbox("Auto Theft", &crime_filter_auto_theft);
                ImGui::Checkbox("Drug/Narcotic", &crime_filter_drug); ImGui::SameLine();
                ImGui::Checkbox("Shooting", &crime_filter_shooting);
                if (ImGui::Button("Clear Crime Filters")) {
                    crime_filter_homicide = false;
                    crime_filter_robbery = false;
                    crime_filter_assault = false;
                    crime_filter_burglary = false;
                    crime_filter_theft = false;
                    crime_filter_auto_theft = false;
                    crime_filter_drug = false;
                    crime_filter_shooting = false;
                    crime_filter_use_year = false;
                    crime_filter_enabled = false;
                }
                ImGui::SameLine();
                if (ImGui::Button("Refresh Crime Breakdown")) {
                    std::unordered_map<std::string, int> counts;
                    auto add_layer_counts = [&](int idx) {
                        if (idx < 0 || (size_t)idx >= layers.size()) return;
                        for (const auto& fg : layers[(size_t)idx].features) {
                            const std::string desc = toLowerAscii(getPropertyValue(fg, "Description"));
                            const std::string code = toLowerAscii(getPropertyValue(fg, "CrimeCode"));
                            const std::string dt = getPropertyValue(fg, "CrimeDateTime");
                            if (crime_filter_enabled) {
                                if (crime_filter_use_year) {
                                    int yr = extractYearMaybe(dt);
                                    if (yr < 0 || yr < crime_year_min || yr > crime_year_max) continue;
                                }
                            const bool any_type =
                                crime_filter_homicide || crime_filter_robbery || crime_filter_assault ||
                                crime_filter_burglary || crime_filter_theft || crime_filter_auto_theft || crime_filter_drug || crime_filter_shooting;
                                if (any_type) {
                                    auto has = [&](const char* s) { return desc.find(s) != std::string::npos || code.find(s) != std::string::npos; };
                                    bool ok = false;
                                    if (crime_filter_homicide && (has("homicide") || has("murder"))) ok = true;
                                    if (crime_filter_robbery && has("robbery")) ok = true;
                                    if (crime_filter_assault && (has("assault") || has("aggravated assault"))) ok = true;
                                    if (crime_filter_burglary && has("burglary")) ok = true;
                                    if (crime_filter_theft && (has("larceny") || has("theft"))) ok = true;
                                    if (crime_filter_auto_theft && (has("motor vehicle theft") || has("auto theft") || has("vehicle theft"))) ok = true;
                                    if (crime_filter_drug && (has("drug") || has("narcotic"))) ok = true;
                                    if (crime_filter_shooting && has("shooting")) ok = true;
                                    if (!ok) continue;
                                }
                            }
                            std::string label = trimDisplayValue(getPropertyValue(fg, "Description"));
                            if (label.empty()) label = trimDisplayValue(getPropertyValue(fg, "CrimeCode"));
                            if (label.empty()) label = "(unknown)";
                            counts[label] += 1;
                        }
                    };
                    add_layer_counts(crime_nibrs_layer_idx);
                    add_layer_counts(crime_legacy_layer_idx);
                    crime_breakdown.clear();
                    crime_breakdown.reserve(counts.size());
                    for (auto& kv : counts) crime_breakdown.push_back(kv);
                    std::sort(crime_breakdown.begin(), crime_breakdown.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
                }
                ImGui::Text("Breakdown Rows: %zu", crime_breakdown.size());
            }
        };

        draw_category(LayerDef::Category::Housing, "Housing");
        draw_category(LayerDef::Category::PublicHealth, "Public Health");
        draw_category(LayerDef::Category::Safety, "Safety");
        draw_category(LayerDef::Category::Infrastructure, "Infrastructure");
        draw_category(LayerDef::Category::Zoning, "Zoning");
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
                    if (ImGui::BeginTabItem("Heatmap")) {
                        const char* heatmap_algo_items[] = {
                            "Grid Binning",
                            "KDE (Gaussian)",
                            "GPU Splat + Blur",
                            "Hex Binning",
                            "Multi-res Pyramid"
                        };
                        const char* heatmap_quality_items[] = {"Fast", "Balanced", "High"};
                        int heatmap_quality_prev = heatmap_quality_preset;
                        heatmap_settings_state_changed |= ImGui::Combo("Quality", &heatmap_quality_preset, heatmap_quality_items, IM_ARRAYSIZE(heatmap_quality_items));
                        heatmap_controls_active |= ImGui::IsItemActive();
                        if (heatmap_quality_preset != heatmap_quality_prev) {
                            if (heatmap_quality_preset == 0) {
                                global_heat_cell_px = 36.0f;
                                heatmap_bandwidth_px = 10.0f;
                                heatmap_blur_sigma_px = 3.0f;
                                heatmap_percentile_clip = 92.0f;
                                heatmap_multires_enabled = false;
                            } else if (heatmap_quality_preset == 1) {
                                global_heat_cell_px = 24.0f;
                                heatmap_bandwidth_px = 18.0f;
                                heatmap_blur_sigma_px = 6.0f;
                                heatmap_percentile_clip = 95.0f;
                                heatmap_multires_enabled = true;
                                heatmap_multires_blend = 0.5f;
                            } else {
                                global_heat_cell_px = 14.0f;
                                heatmap_bandwidth_px = 26.0f;
                                heatmap_blur_sigma_px = 10.0f;
                                heatmap_percentile_clip = 97.0f;
                                heatmap_multires_enabled = true;
                                heatmap_multires_blend = 0.65f;
                            }
                            heatmap_settings_state_changed = true;
                        }
                        heatmap_settings_state_changed |= ImGui::Combo("Aggregate Method", &heatmap_algo, heatmap_algo_items, IM_ARRAYSIZE(heatmap_algo_items));
                        heatmap_controls_active |= ImGui::IsItemActive();
                        heatmap_settings_state_changed |= heatmap_input_float_enter("Cell Size", global_heat_cell_px, 8.0f, 80.0f, "%.0f");
                        heatmap_settings_state_changed |= heatmap_input_float_enter("Bandwidth", heatmap_bandwidth_px, 2.0f, 96.0f, "%.1f");
                        heatmap_settings_state_changed |= heatmap_input_float_enter("Blur Sigma", heatmap_blur_sigma_px, 0.0f, 32.0f, "%.1f");
                        heatmap_settings_state_changed |= heatmap_input_float_enter("Clip", heatmap_percentile_clip, 50.0f, 100.0f, "%.0f");
                        heatmap_settings_state_changed |= ImGui::Checkbox("Zoom-adaptive bandwidth", &heatmap_zoom_adaptive_bandwidth);
                        heatmap_controls_active |= ImGui::IsItemActive();
                        heatmap_settings_state_changed |= ImGui::Checkbox("Multi-res blending", &heatmap_multires_enabled);
                        heatmap_controls_active |= ImGui::IsItemActive();
                        ImGui::BeginDisabled(!heatmap_multires_enabled);
                        heatmap_settings_state_changed |= heatmap_input_float_enter("Multi-res Blend", heatmap_multires_blend, 0.0f, 1.0f, "%.2f");
                        ImGui::EndDisabled();
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
            }
            ImGui::End();
        }
        if (show_data_library) {
            ImGui::SetNextWindowSize(ImVec2(860, 560), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Data Library", &show_data_library, ImGuiWindowFlags_NoCollapse)) {
                auto draw_library_cell = [](const std::string& value, size_t max_chars = 64) {
                    const std::string full = value.empty() ? std::string("-") : value;
                    std::string shown = full;
                    if (shown.size() > max_chars && max_chars > 3) shown = shown.substr(0, max_chars - 3) + "...";
                    ImGui::TextUnformatted(shown.c_str());
                    if (shown.size() != full.size() && ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 80.0f);
                        ImGui::TextUnformatted(full.c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::EndTooltip();
                    }
                };
                auto draw_library_tooltip = [](const LayerDef& l) {
                    if (!ImGui::IsItemHovered()) return;
                    if (l.description.empty() && l.source_url.empty()) return;
                    ImGui::BeginTooltip();
                    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 90.0f);
                    if (!l.description.empty()) ImGui::TextUnformatted(l.description.c_str());
                    if (!l.description.empty() && !l.source_url.empty()) ImGui::Separator();
                    if (!l.source_url.empty()) ImGui::TextUnformatted(l.source_url.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                };
                auto freshness_label = [](FreshnessState s) -> const char* {
                    switch (s) {
                        case FreshnessState::UpToDate: return "Up-to-date";
                        case FreshnessState::UpdateAvailable: return "Update available";
                        case FreshnessState::NotTrackable: return "Not trackable";
                        case FreshnessState::Error: return "Check error";
                        case FreshnessState::Unknown: default: return "Unknown";
                    }
                };
                auto freshness_color = [](FreshnessState s) -> ImVec4 {
                    switch (s) {
                        case FreshnessState::UpToDate: return ImVec4(0.20f, 0.62f, 0.25f, 1.0f);
                        case FreshnessState::UpdateAvailable: return ImVec4(0.80f, 0.50f, 0.10f, 1.0f);
                        case FreshnessState::NotTrackable: return ImVec4(0.45f, 0.45f, 0.45f, 1.0f);
                        case FreshnessState::Error: return ImVec4(0.75f, 0.22f, 0.16f, 1.0f);
                        case FreshnessState::Unknown: default: return ImVec4(0.30f, 0.45f, 0.72f, 1.0f);
                    }
                };
                ImGui::InputTextWithHint("##data_library_query", "Search by name, file, category, subcategory...", data_library_query, sizeof(data_library_query));
                ImGui::SameLine();
                if (ImGui::Button("Clear")) data_library_query[0] = '\0';
                ImGui::SameLine();
                if (ImGui::Button("Check All Updates")) {
                    size_t checked = 0;
                    size_t updates = 0;
                    for (size_t i = 0; i < layers.size(); ++i) {
                        const auto& l = layers[i];
                        const fs::path local_path = root / "data" / "layers" / l.file;
                        if (!fs::exists(local_path) || l.source_url.empty()) {
                            data_freshness_state[i] = l.source_url.empty() ? FreshnessState::NotTrackable : FreshnessState::Unknown;
                            data_freshness_msg[i] = l.source_url.empty() ? "no source URL" : "not downloaded";
                            continue;
                        }
                        FreshnessCheckResult cr = checkUrlFreshnessVersioned(l.source_url, local_path, root / "data" / "versions");
                        data_freshness_state[i] = cr.state;
                        data_freshness_msg[i] = cr.message;
                        checked++;
                        if (cr.state == FreshnessState::UpdateAvailable) updates++;
                    }
                    data_library_status_msg = "Checked " + std::to_string(checked) + " datasets; updates available: " + std::to_string(updates);
                }
                size_t downloaded_count = 0;
                for (const auto& l : layers) {
                    if (fs::exists(root / "data" / "layers" / l.file)) downloaded_count++;
                }
                ImGui::Text("Downloaded: %zu / %zu", downloaded_count, layers.size());
                ImGui::TextDisabled("Version metadata: data/versions/metadata | snapshots: data/versions/snapshots | diffs: data/versions/diffs");
                if (!data_library_status_msg.empty()) ImGui::TextWrapped("%s", data_library_status_msg.c_str());
                ImGui::Separator();

                const std::string query = trimDisplayValue(data_library_query);
                if (query != data_library_cached_query || data_library_cached_layer_count != layers.size()) {
                    data_library_cached_query = query;
                    data_library_cached_layer_count = layers.size();
                    data_library_visible_rows.clear();
                    data_library_visible_rows.reserve(layers.size());
                    for (size_t i = 0; i < layers.size(); ++i) {
                        const auto& l = layers[i];
                        const bool hit =
                            query.empty() ||
                            containsCaseInsensitive(l.name, query) ||
                            containsCaseInsensitive(l.file, query) ||
                            containsCaseInsensitive(categoryToString(l.category), query) ||
                            containsCaseInsensitive(l.subcategory, query) ||
                            containsCaseInsensitive(l.description, query);
                        if (hit) data_library_visible_rows.push_back(i);
                    }
                    data_library_cache_rebuilds++;
                }

                data_library_rendered_rows_last = 0;
                if (ImGui::BeginTable("data_library_table", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0, 0))) {
                    ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 94.0f);
                    ImGui::TableSetupColumn("Freshness", ImGuiTableColumnFlags_WidthFixed, 142.0f);
                    ImGui::TableSetupColumn("Enabled", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                    ImGui::TableSetupColumn("Name");
                    ImGui::TableSetupColumn("File");
                    ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 110.0f);
                    ImGui::TableSetupColumn("Subcategory");
                    ImGui::TableHeadersRow();
                    ImGuiListClipper clipper;
                    clipper.Begin(static_cast<int>(data_library_visible_rows.size()));
                    while (clipper.Step()) {
                        for (int display_index = clipper.DisplayStart; display_index < clipper.DisplayEnd; ++display_index) {
                            const size_t i = data_library_visible_rows[static_cast<size_t>(display_index)];
                            auto& l = layers[i];
                            const fs::path local_path = root / "data" / "layers" / l.file;
                            const bool local_exists = fs::exists(local_path);
                            data_library_rendered_rows_last++;
                            ImGui::PushID((int)i);
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            if (!local_exists) {
                                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.22f, 0.12f, 1.0f));
                                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.30f, 0.14f, 1.0f));
                                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.46f, 0.18f, 0.08f, 1.0f));
                                if (ImGui::SmallButton("Download")) {
                                    if (l.source_url.empty()) {
                                        data_library_status_msg = "No source URL for " + l.file;
                                    } else {
                                        VersionedDownloadResult vd = downloadUrlVersioned(
                                            l.source_url,
                                            local_path,
                                            root / "data" / "versions");
                                        if (vd.ok) {
                                            data_library_status_msg = (vd.not_modified ? "Checked " : "Downloaded/updated ") + l.file + " (" + vd.message + ")";
                                            data_freshness_state[i] = FreshnessState::UpToDate;
                                            data_freshness_msg[i] = vd.message;
                                            enqueue_hydration(i, true);
                                        } else {
                                            data_library_status_msg = "Download failed for " + l.file + ": " + vd.message;
                                            data_freshness_state[i] = FreshnessState::Error;
                                            data_freshness_msg[i] = vd.message;
                                        }
                                    }
                                }
                                ImGui::PopStyleColor(3);
                            } else if (!l.source_url.empty()) {
                                if (data_freshness_state[i] == FreshnessState::UpdateAvailable) {
                                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.74f, 0.44f, 0.12f, 1.0f));
                                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.88f, 0.53f, 0.14f, 1.0f));
                                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.62f, 0.36f, 0.10f, 1.0f));
                                    if (ImGui::SmallButton("Update")) {
                                        VersionedDownloadResult vd = downloadUrlVersioned(
                                            l.source_url,
                                            local_path,
                                            root / "data" / "versions");
                                        if (vd.ok) {
                                            data_freshness_state[i] = FreshnessState::UpToDate;
                                            data_freshness_msg[i] = vd.message;
                                            data_library_status_msg = "Updated " + l.file + " (" + vd.message + ")";
                                            enqueue_hydration(i, true);
                                        } else {
                                            data_freshness_state[i] = FreshnessState::Error;
                                            data_freshness_msg[i] = vd.message;
                                            data_library_status_msg = "Update failed for " + l.file + ": " + vd.message;
                                        }
                                    }
                                    ImGui::PopStyleColor(3);
                                } else if (ImGui::SmallButton("Check")) {
                                    FreshnessCheckResult cr = checkUrlFreshnessVersioned(l.source_url, local_path, root / "data" / "versions");
                                    data_freshness_state[i] = cr.state;
                                    data_freshness_msg[i] = cr.message;
                                    data_library_status_msg = "Checked " + l.file + ": " + cr.message;
                                }
                            } else {
                                ImGui::TextDisabled("-");
                            }
                            ImGui::TableSetColumnIndex(1);
                            ImGui::ColorButton("##freshness_dot", freshness_color(data_freshness_state[i]), ImGuiColorEditFlags_NoTooltip, ImVec2(10, 10));
                            ImGui::SameLine();
                            ImGui::TextDisabled("%s", freshness_label(data_freshness_state[i]));
                            if (!data_freshness_msg[i].empty() && ImGui::IsItemHovered()) {
                                ImGui::BeginTooltip();
                                ImGui::TextUnformatted(data_freshness_msg[i].c_str());
                                ImGui::EndTooltip();
                            }
                            ImGui::TableSetColumnIndex(2);
                            ImGui::Checkbox("##enable", &l.enabled);
                            ImGui::TableSetColumnIndex(3);
                            ImGui::PushStyleColor(ImGuiCol_Text, l.color);
                            draw_library_cell(l.name, 72);
                            ImGui::PopStyleColor();
                            draw_library_tooltip(l);
                            ImGui::TableSetColumnIndex(4);
                            draw_library_cell(l.file, 72);
                            ImGui::TableSetColumnIndex(5);
                            draw_library_cell(categoryToString(l.category), 32);
                            ImGui::TableSetColumnIndex(6);
                            draw_library_cell(l.subcategory, 56);
                            ImGui::PopID();
                        }
                    }
                    ImGui::EndTable();
                }
                ImGui::TextDisabled("Matched rows: %zu | Rendered this frame: %zu | Cache rebuilds: %zu",
                                    data_library_visible_rows.size(), data_library_rendered_rows_last, data_library_cache_rebuilds);
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
        ImGui::TextDisabled("UI rows: Data Library %zu/%zu (rebuilds %zu) | People & Pay %zu/%zu (rebuilds %zu)",
                            data_library_rendered_rows_last,
                            data_library_visible_rows.size(),
                            data_library_cache_rebuilds,
                            people_pay_rendered_rows_last,
                            people_pay_visible_rows.size(),
                            people_pay_cache_rebuilds);
        ImGui::TextDisabled("Fill: %zu ok / %zu attempts | no tris %zu | bad idx %zu",
                            render_fill_success_last_frame.load(std::memory_order_relaxed),
                            render_fill_attempts_last_frame.load(std::memory_order_relaxed),
                            render_fill_no_triangles_last_frame.load(std::memory_order_relaxed),
                            render_fill_bad_indices_last_frame.load(std::memory_order_relaxed));
        ImGui::TextDisabled("API: http://127.0.0.1:8787/status");
        ImGui::TextDisabled("LAN Data API: http://0.0.0.0:8788/datasets");
        ImGui::TextDisabled("P2P signaling: /p2p/register /p2p/publish /p2p/poll");
        ImGui::SeparatorText("Arkavo Native Realtime");
        ImGui::InputText("Room ID", arkavo_room_id, sizeof(arkavo_room_id));
        if (!arkavo_client) {
            if (ImGui::Button("Connect Arkavo")) {
                ArkavoRealtimeClient::Config cfg;
                cfg.room_id = trimDisplayValue(arkavo_room_id);
                cfg.signaling_url = "wss://signaling.arkavo.org/";
                auto transport = std::make_unique<ArkavoSignalingTransportCurl>();
                arkavo_client = std::make_unique<ArkavoRealtimeClient>(cfg, std::move(transport));
                arkavo_rtc = std::make_unique<ArkavoRtcSessionManager>(*arkavo_client);
                arkavo_rtc->on_log = [&](const std::string& m) { arkavo_status = m; };
                arkavo_rtc->on_error = [&](const std::string& e) { arkavo_err = e; };
                arkavo_rtc->on_file_received = [&](const std::string& peer, const std::filesystem::path& p) {
                    arkavo_status = "received file from " + peer + ": " + p.string();
                };
                arkavo_client->on_log = [&](const std::string& m) { arkavo_status = m; };
                arkavo_client->on_error = [&](const std::string& e) { arkavo_err = e; };
                arkavo_client->on_peer_should_connect = [&](const std::string& peer_id, bool initiator) {
                    if (arkavo_rtc) arkavo_rtc->connectPeer(peer_id, initiator);
                };
                arkavo_client->on_peer_left = [&](const std::string& peer_id) {
                    if (arkavo_rtc) arkavo_rtc->removePeer(peer_id);
                };
                arkavo_client->on_signal_payload = [&](const std::string& peer_id, const nlohmann::json& payload) {
                    if (arkavo_rtc) arkavo_rtc->handleSignal(peer_id, payload);
                };
                std::string err;
                if (!arkavo_client->start(err)) {
                    arkavo_err = err;
                    arkavo_status = "connect failed";
                } else {
                    arkavo_status = "connecting";
                }
            }
        } else {
            if (ImGui::Button("Disconnect Arkavo")) {
                if (arkavo_rtc) arkavo_rtc->closeAll();
                arkavo_client->stop();
                arkavo_rtc.reset();
                arkavo_client.reset();
                arkavo_status = "disconnected";
            }
            ImGui::SameLine();
            ImGui::TextDisabled("connected=%s", arkavo_client->isConnected() ? "yes" : "no");
            ImGui::TextDisabled("self peer: %s", arkavo_client->selfPeerId().empty() ? "(none)" : arkavo_client->selfPeerId().c_str());
            ImGui::TextDisabled("tracked peers: %zu", arkavo_client->peers().size());
            if (arkavo_rtc) {
                auto open_peers = arkavo_rtc->connectedPeers();
                ImGui::TextDisabled("open data channels: %zu", open_peers.size());
                ImGui::InputText("Send Peer", arkavo_send_peer, sizeof(arkavo_send_peer));
                ImGui::InputText("Send File Path", arkavo_send_path, sizeof(arkavo_send_path));
                if (ImGui::Button("Send Arkavo File")) {
                    std::string err;
                    if (!arkavo_rtc->sendFile(trimDisplayValue(arkavo_send_peer), trimDisplayValue(arkavo_send_path), err)) {
                        arkavo_err = err;
                    } else {
                        arkavo_status = "file send queued";
                    }
                }
                if (!open_peers.empty()) {
                    ImGui::TextDisabled("open peers:");
                    for (const auto& p : open_peers) ImGui::TextDisabled("%s", p.c_str());
                }
            }
        }
        ImGui::TextDisabled("status: %s", arkavo_status.c_str());
        if (!arkavo_err.empty()) ImGui::TextColored(ImVec4(0.85f, 0.35f, 0.2f, 1.0f), "arkavo: %s", arkavo_err.c_str());
        if (ImGui::Button("Scan LAN Peers")) {
            lan_peers.clear();
            if (!ensureSocketRuntimeInitialized()) continue;
            int s = (int)::socket(AF_INET, SOCK_DGRAM, 0);
            if (s < 0) {
                lan_scan_status = "Scan failed: socket error";
            } else {
                int on = 1;
                setsockopt(s, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
                timeval rcv_to{};
                rcv_to.tv_sec = 0;
                rcv_to.tv_usec = 700000;
                setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &rcv_to, sizeof(rcv_to));
                sockaddr_in dst{};
                dst.sin_family = AF_INET;
                dst.sin_port = htons(8789);
                dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);
                const char* probe = "WS3_DISCOVER_V1";
                (void)sendto(s, probe, std::strlen(probe), 0, (sockaddr*)&dst, sizeof(dst));
                std::unordered_set<std::string> seen;
                for (;;) {
                    sockaddr_in src{};
                    #ifdef _WIN32
                    int slen = sizeof(src);
                    #else
                    socklen_t slen = sizeof(src);
                    #endif
                    char rbuf[4096];
                    ssize_t rn = recvfrom(s, rbuf, sizeof(rbuf) - 1, 0, (sockaddr*)&src, &slen);
                    if (rn <= 0) break;
                    rbuf[rn] = '\0';
                    json jr = json::parse(std::string(rbuf), nullptr, false);
                    if (jr.is_discarded()) continue;
                    char ipbuf[INET_ADDRSTRLEN] = {0};
                    inet_ntop(AF_INET, &src.sin_addr, ipbuf, sizeof(ipbuf));
                    const std::string ip(ipbuf);
                    if (seen.count(ip)) continue;
                    seen.insert(ip);
                    LanPeerInfo p;
                    p.ip = ip;
                    p.app_version = jr.value("app_version", "");
                    p.protocol_version = jr.value("protocol_version", 0);
                    p.dataset_port = jr.value("dataset_port", 8788);
                    p.protocol_match = (p.protocol_version == kProtocolVersion);
                    lan_peers.push_back(std::move(p));
                }
                netClose(s);
                size_t compatible = 0;
                for (const auto& p : lan_peers) if (p.protocol_match) compatible++;
                lan_scan_status = "Peers: " + std::to_string(lan_peers.size()) +
                                  " | Compatible protocol: " + std::to_string(compatible);
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", lan_scan_status.c_str());
        if (!lan_peers.empty()) {
            ImGui::BeginChild("lan_peer_list", ImVec2(0, 90), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
            for (const auto& p : lan_peers) {
                ImGui::Text("%s:%d | app %s | protocol %d %s",
                    p.ip.c_str(),
                    p.dataset_port,
                    p.app_version.empty() ? "(unknown)" : p.app_version.c_str(),
                    p.protocol_version,
                    p.protocol_match ? "[OK]" : "[MISMATCH]");
            }
            ImGui::EndChild();
        }
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
                if (i < layer_profile_dirty.size()) layer_profile_dirty[i] = true;
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
            layer_inspect_state_changed ||
            layer_heatmap_state_changed ||
            heatmap_settings_state_changed;
        std::vector<size_t> newly_enabled;
        if (last_enabled_state.size() == layers.size()) {
            for (size_t i = 0; i < layers.size(); ++i) {
                if (layers[i].enabled != last_enabled_state[i]) {
                    ui_state_changed = true;
                    if (i < layer_profile_dirty.size()) layer_profile_dirty[i] = true;
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
                &layer_inspect_enabled,
                &layer_heatmap_enabled,
                &layer_heatmap_max_zoom,
                &layer_heatmap_use_gradient,
                &layer_heatmap_algo,
                &layer_heatmap_use_global_settings,
                &layer_heatmap_cell_px,
                &layer_heatmap_bandwidth_px,
                &layer_heatmap_blur_sigma_px,
                &layer_heatmap_percentile_clip,
                &layer_heatmap_zoom_adaptive_bandwidth,
                &layer_heatmap_multires_enabled,
                &layer_heatmap_multires_blend,
                &heatmap_algo,
                &heatmap_quality_preset,
                &global_heat_cell_px,
                &heatmap_bandwidth_px,
                &heatmap_blur_sigma_px,
                &heatmap_percentile_clip,
                &heatmap_zoom_adaptive_bandwidth,
                &heatmap_multires_enabled,
                &heatmap_multires_blend);
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
                        if (ready.index < layer_profile_dirty.size()) layer_profile_dirty[ready.index] = true;
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
                        if (tr.index < layer_profile_dirty.size()) layer_profile_dirty[tr.index] = true;
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
                owner_aggregates_dirty = true;
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
                vacant_notice_rows_matched_total.store(notice_rows_matched, std::memory_order_relaxed);
                vacant_rehab_rows_matched_total.store(rehab_rows_matched, std::memory_order_relaxed);
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
                owner_aggregates_dirty = true;
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
                owner_aggregates_dirty = true;
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
                if (li < layer_profile_dirty.size()) layer_profile_dirty[li] = true;
            }
        }
        refresh_layer_profile_snapshot();

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
        auto parcel_area_sq_m = [&](const LayerDef::FeatureGeom& fg) -> double {
            if (fg.rings.empty()) return 0.0;
            const double deg_to_m_lat = 111320.0;
            double total = 0.0;
            for (const auto& ring : fg.rings) {
                if (ring.size() < 3) continue;
                double lat_sum = 0.0;
                for (const auto& p : ring) lat_sum += (double)p.y;
                const double lat0 = lat_sum / (double)ring.size();
                const double cos_lat = std::cos(lat0 * M_PI / 180.0);
                const double sx = deg_to_m_lat * cos_lat;
                const double sy = deg_to_m_lat;
                double a = 0.0;
                for (size_t i = 0, n = ring.size(); i < n; ++i) {
                    const auto& p = ring[i];
                    const auto& q = ring[(i + 1) % n];
                    const double px = (double)p.x * sx;
                    const double py = (double)p.y * sy;
                    const double qx = (double)q.x * sx;
                    const double qy = (double)q.y * sy;
                    a += (px * qy - qx * py);
                }
                total += std::abs(a) * 0.5;
            }
            return total;
        };
        auto owner_name_for = [&](const LayerDef::FeatureGeom* rp) -> std::string {
            if (!rp) return "";
            std::string o = firstDisplayProperty(*rp, {"OWNER_1", "OWNERNME1", "OWNER", "OWNER_NAME", "AR_OWNER", "OWNER_ABBR"});
            return toLowerAscii(trimDisplayValue(o));
        };
        auto owner_value_for = [&](const LayerDef::FeatureGeom* rp) -> double {
            if (!rp) return 0.0;
            auto v = [&](std::initializer_list<const char*> keys) -> double {
                return parseNumericField(firstDisplayProperty(*rp, keys));
            };
            double tax_base = v({"TAXBASE", "ARTAXBAS"});
            double curr_land = v({"CURRLAND"});
            double curr_impr = v({"CURRIMPR"});
            double sale = v({"SALEPRIC"});
            if (tax_base > 0.0) return tax_base;
            if (curr_land + curr_impr > 0.0) return curr_land + curr_impr;
            return std::max(0.0, sale);
        };
        if (parcel_layer_idx >= 0 && (size_t)parcel_layer_idx < layers.size() &&
            layers[(size_t)parcel_layer_idx].features.size() != owner_cached_parcel_size) {
            owner_cached_parcel_size = layers[(size_t)parcel_layer_idx].features.size();
            owner_aggregates_dirty = true;
        }
        if (real_property_layer_idx >= 0 && (size_t)real_property_layer_idx < layers.size() &&
            layers[(size_t)real_property_layer_idx].features.size() != owner_cached_real_property_size) {
            owner_cached_real_property_size = layers[(size_t)real_property_layer_idx].features.size();
            owner_aggregates_dirty = true;
        }
        if (owner_aggregates_dirty && parcel_layer_idx >= 0 && (size_t)parcel_layer_idx < layers.size()) {
            const auto owner_prof_begin = std::chrono::steady_clock::now();
            std::unordered_map<std::string, OwnerAggregate> acc;
            const auto& parcels = layers[(size_t)parcel_layer_idx].features;
            const bool parcel_data_ready = !parcels.empty();
            const bool real_property_data_ready =
                real_property_layer_idx >= 0 &&
                (size_t)real_property_layer_idx < layers.size() &&
                !layers[(size_t)real_property_layer_idx].features.empty();
            if (!parcel_data_ready) {
                owner_sorted_mode = -1;
            } else {
            for (const auto& pf : parcels) {
                const LayerDef::FeatureGeom* rp = real_property_for_parcel(pf);
                std::string owner = owner_name_for(rp);
                if (owner.empty()) owner = owner_name_for(&pf);
                if (owner.empty()) continue;
                auto& row = acc[owner];
                if (row.owner.empty()) row.owner = owner;
                row.property_count += 1;
                row.area_m2 += parcel_area_sq_m(pf);
                row.value_usd += owner_value_for(rp);
            }
            owner_aggregates.clear();
            owner_aggregates.reserve(acc.size());
            for (auto& kv : acc) owner_aggregates.push_back(std::move(kv.second));
            owner_aggregates_dirty = !real_property_data_ready && owner_aggregates.empty();
            owner_sorted_mode = -1;
            }
            prof_owner_ms_last.store(prof_ms_since(owner_prof_begin), std::memory_order_relaxed);
        }

        const float right_panel_w = 360.0f;
        ImGui::SetNextWindowPos(ImVec2((float)w - right_panel_w - 12.0f, 12.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(right_panel_w, (float)h - 24.0f), ImGuiCond_Always);
        ImGui::Begin("Record Filters", nullptr, ImGuiWindowFlags_NoCollapse);
        if (ImGui::BeginTabBar("right_tabs")) {
        if (ImGui::BeginTabItem("Filters")) {
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
        ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Vacancy-Parcel")) {
            const size_t notices_total = (size_t)cached_vac_notice_size;
            const size_t rehabs_total = (size_t)cached_vac_rehab_size;
            const size_t notices_matched = vacant_notice_rows_matched_total.load(std::memory_order_relaxed);
            const size_t rehabs_matched = vacant_rehab_rows_matched_total.load(std::memory_order_relaxed);
            const size_t parcels_matched = vacant_parcels_matched_total.load(std::memory_order_relaxed);
            const size_t parcels_geom = vacant_parcels_with_geometry_total.load(std::memory_order_relaxed);
            ImGui::TextUnformatted("Vacant -> Parcel Join Quality");
            ImGui::Separator();
            ImGui::Text("Vacant notice records: %zu", notices_total);
            ImGui::Text("Matched to parcel: %zu (%.1f%%)", notices_matched, notices_total ? (100.0 * (double)notices_matched / (double)notices_total) : 0.0);
            ImGui::Text("Unmatched notices: %zu", notices_total >= notices_matched ? (notices_total - notices_matched) : 0);
            ImGui::Separator();
            ImGui::Text("Vacant rehab records: %zu", rehabs_total);
            ImGui::Text("Matched to parcel: %zu (%.1f%%)", rehabs_matched, rehabs_total ? (100.0 * (double)rehabs_matched / (double)rehabs_total) : 0.0);
            ImGui::Text("Unmatched rehabs: %zu", rehabs_total >= rehabs_matched ? (rehabs_total - rehabs_matched) : 0);
            ImGui::Separator();
            ImGui::Text("Parcels with vacancy evidence: %zu", parcels_matched);
            ImGui::Text("Those with parcel geometry: %zu", parcels_geom);
            ImGui::TextWrapped("Map styling uses parcel-level derived status. Raw vacant points are treated as child records.");
        ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Owners")) {
            ImGui::TextUnformatted("Owner Rankings");
            ImGui::Separator();
            const char* sort_items[] = {"# Properties", "Area Owned", "Value Owned"};
            ImGui::Combo("Sort By", &owner_sort_mode, sort_items, IM_ARRAYSIZE(sort_items));
            if (owner_sorted_mode != owner_sort_mode) {
                std::stable_sort(owner_aggregates.begin(), owner_aggregates.end(), [&](const OwnerAggregate& a, const OwnerAggregate& b) {
                    auto tie_break = [&]() {
                        if (a.property_count != b.property_count) return a.property_count > b.property_count;
                        if (std::abs(a.area_m2 - b.area_m2) > 0.5) return a.area_m2 > b.area_m2;
                        if (std::abs(a.value_usd - b.value_usd) > 0.5) return a.value_usd > b.value_usd;
                        return a.owner < b.owner;
                    };
                    if (owner_sort_mode == 1 && std::abs(a.area_m2 - b.area_m2) > 0.5) return a.area_m2 > b.area_m2;
                    if (owner_sort_mode == 2 && std::abs(a.value_usd - b.value_usd) > 0.5) return a.value_usd > b.value_usd;
                    if (owner_sort_mode == 0 && a.property_count != b.property_count) return a.property_count > b.property_count;
                    return tie_break();
                });
                owner_sorted_mode = owner_sort_mode;
            }
            ImGui::Text("Owners: %zu", owner_aggregates.size());
            if (owner_aggregates.empty()) {
                const size_t parcel_count =
                    (parcel_layer_idx >= 0 && (size_t)parcel_layer_idx < layers.size()) ? layers[(size_t)parcel_layer_idx].features.size() : 0;
                const size_t real_property_count =
                    (real_property_layer_idx >= 0 && (size_t)real_property_layer_idx < layers.size()) ? layers[(size_t)real_property_layer_idx].features.size() : 0;
                ImGui::TextDisabled("Waiting for owner data: parcels=%zu real_property=%zu", parcel_count, real_property_count);
            }
            ImGui::BeginChild("owner_rankings", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
            const size_t max_rows = std::min<size_t>(500, owner_aggregates.size());
            for (size_t i = 0; i < max_rows; ++i) {
                const auto& r = owner_aggregates[i];
                ImGui::Text("%zu) %s", i + 1, r.owner.c_str());
                ImGui::TextDisabled("properties: %zu | area: %.0f m^2 | value: $%.0f", r.property_count, r.area_m2, r.value_usd);
                ImGui::Separator();
            }
            ImGui::EndChild();
        ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
        }
        ImGui::End();

        const float map_x = 440.0f;
        const float map_w = std::max(260.0f, (float)w - map_x - right_panel_w - 24.0f);
        ImGui::SetNextWindowPos(ImVec2(map_x, 12), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(map_w, (float)h - 24.0f), ImGuiCond_Always);
        ImGui::Begin("Map", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        if (ImGui::BeginTabBar("main_view_tabs")) {
        if (ImGui::BeginTabItem("Map")) {
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

        const auto tile_prof_begin = std::chrono::steady_clock::now();
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
                draw->AddImage(
                    (ImTextureID)sample.tex->descriptor,
                    p0,
                    p1,
                    sample.uv0,
                    sample.uv1,
                    app_settings.grayscale_basemap ? IM_COL32(178, 178, 178, 255) : IM_COL32_WHITE);
                if (app_settings.grayscale_basemap) {
                    draw->AddRectFilled(p0, p1, IM_COL32(244, 244, 244, 78));
                }
                prof_tiles_drawn_frame++;
            }
        }
        prof_tile_ms_last.store(prof_ms_since(tile_prof_begin), std::memory_order_relaxed);

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
        std::vector<std::vector<ImVec2>> scratch_world_rings;
        std::pair<ImVec2, ImVec2> scratch_world_extent;
        auto get_world_rings = [&](size_t, uint32_t, const LayerDef::FeatureGeom& fg)
            -> const std::vector<std::vector<ImVec2>>& {
            scratch_world_rings.clear();
            scratch_world_rings.reserve(fg.rings.size());
            for (const auto& r : fg.rings) {
                std::vector<ImVec2> rr;
                rr.reserve(r.size());
                for (const ImVec2& ll : r) rr.push_back(lonLatToWorldPx(ll.x, ll.y, math_zoom));
                scratch_world_rings.push_back(std::move(rr));
            }
            return scratch_world_rings;
        };
        auto get_world_extent = [&](size_t, uint32_t, const LayerDef::FeatureGeom& fg)
            -> const std::pair<ImVec2, ImVec2>& {
            ImVec2 p0w = lonLatToWorldPx(fg.extent.min_lon, fg.extent.max_lat, math_zoom);
            ImVec2 p1w = lonLatToWorldPx(fg.extent.max_lon, fg.extent.min_lat, math_zoom);
            scratch_world_extent = std::make_pair(p0w, p1w);
            return scratch_world_extent;
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
        auto is_crime_layer = [&](size_t layer_idx) -> bool {
            return (crime_nibrs_layer_idx >= 0 && (int)layer_idx == crime_nibrs_layer_idx) ||
                   (crime_legacy_layer_idx >= 0 && (int)layer_idx == crime_legacy_layer_idx);
        };
        auto crime_feature_matches = [&](const LayerDef::FeatureGeom& fg) -> bool {
            if (!crime_filter_enabled) return true;
            const std::string desc = toLowerAscii(first_prop(fg, {"Description", "description", "OFFENSE", "UCRDescription"}));
            const std::string code = toLowerAscii(first_prop(fg, {"CrimeCode", "UCR_CODE", "UCRCode"}));
            const std::string dt = first_prop(fg, {"CrimeDateTime", "CrimeDate", "DATE", "RECORD_DATE"});
            if (crime_filter_use_year) {
                int yr = extractYearMaybe(dt);
                if (yr < 0 || yr < crime_year_min || yr > crime_year_max) return false;
            }
            const bool any_type =
                crime_filter_homicide || crime_filter_robbery || crime_filter_assault ||
                crime_filter_burglary || crime_filter_theft || crime_filter_auto_theft || crime_filter_drug || crime_filter_shooting;
            if (!any_type) return true;
            auto has = [&](const char* s) { return desc.find(s) != std::string::npos || code.find(s) != std::string::npos; };
            bool ok = false;
            if (crime_filter_homicide && (has("homicide") || has("murder"))) ok = true;
            if (crime_filter_robbery && has("robbery")) ok = true;
            if (crime_filter_assault && (has("assault") || has("aggravated assault"))) ok = true;
            if (crime_filter_burglary && has("burglary")) ok = true;
            if (crime_filter_theft && (has("larceny") || has("theft"))) ok = true;
            if (crime_filter_auto_theft && (has("motor vehicle theft") || has("auto theft") || has("vehicle theft"))) ok = true;
            if (crime_filter_drug && (has("drug") || has("narcotic"))) ok = true;
            if (crime_filter_shooting && has("shooting")) ok = true;
            return ok;
        };
        auto feature_passes_filters = [&](size_t layer_idx, size_t feature_idx, const LayerDef::FeatureGeom& fg) -> bool {
            if (is_crime_layer(layer_idx)) {
                if (!filter_enabled && !crime_filter_enabled) return true;
                return crime_feature_matches(fg);
            }
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
        auto try_get_prop_float = [&](const LayerDef::FeatureGeom& fg, const std::string& key, float& out) -> bool {
            if (key.empty()) return false;
            for (const auto& kv : fg.properties) {
                if (kv.first != key) continue;
                char* end = nullptr;
                const float v = std::strtof(kv.second.c_str(), &end);
                if (end == kv.second.c_str() || (end && *end != '\0')) return false;
                if (!std::isfinite(v)) return false;
                out = v;
                return true;
            }
            return false;
        };
        auto heat_color = [&](float t) -> ImVec4 {
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
        };
        auto hash_combine_u64 = [](uint64_t& seed, uint64_t v) {
            seed ^= v + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
        };
        auto hash_combine_f = [&](uint64_t& seed, float v) {
            uint32_t bits = 0;
            std::memcpy(&bits, &v, sizeof(bits));
            hash_combine_u64(seed, (uint64_t)bits);
        };
        const float global_heat_cell = std::max(4.0f, global_heat_cell_px);
        struct HeatSample {
            int layer = -1;
            float x = 0.0f;
            float y = 0.0f;
            float lon = 0.0f;
            float lat = 0.0f;
            ImVec4 color = ImVec4(1, 1, 1, 1);
            bool prefer_gradient = true;
            int algo = 0;
            float cell_px = 24.0f;
            float bandwidth_px = 18.0f;
            float blur_sigma_px = 6.0f;
            float percentile_clip = 95.0f;
            bool zoom_adaptive_bandwidth = true;
            bool multires_enabled = true;
            float multires_blend = 0.5f;
        };
        std::vector<HeatSample> heat_samples;
        auto resolve_layer_heat_settings = [&](size_t layer_idx, HeatSample& hs) {
            const bool use_global = layer_idx >= layer_heatmap_use_global_settings.size() || layer_heatmap_use_global_settings[layer_idx];
            hs.layer = (int)layer_idx;
            hs.algo = (layer_idx < layer_heatmap_algo.size() && layer_heatmap_algo[layer_idx] >= 0) ? layer_heatmap_algo[layer_idx] : heatmap_algo;
            hs.cell_px = std::max(4.0f, use_global || layer_idx >= layer_heatmap_cell_px.size() ? global_heat_cell_px : layer_heatmap_cell_px[layer_idx]);
            hs.bandwidth_px = std::max(1.0f, use_global || layer_idx >= layer_heatmap_bandwidth_px.size() ? heatmap_bandwidth_px : layer_heatmap_bandwidth_px[layer_idx]);
            hs.blur_sigma_px = std::max(0.0f, use_global || layer_idx >= layer_heatmap_blur_sigma_px.size() ? heatmap_blur_sigma_px : layer_heatmap_blur_sigma_px[layer_idx]);
            hs.percentile_clip = std::clamp(use_global || layer_idx >= layer_heatmap_percentile_clip.size() ? heatmap_percentile_clip : layer_heatmap_percentile_clip[layer_idx], 50.0f, 100.0f);
            hs.zoom_adaptive_bandwidth = use_global || layer_idx >= layer_heatmap_zoom_adaptive_bandwidth.size() ? heatmap_zoom_adaptive_bandwidth : layer_heatmap_zoom_adaptive_bandwidth[layer_idx];
            hs.multires_enabled = use_global || layer_idx >= layer_heatmap_multires_enabled.size() ? heatmap_multires_enabled : layer_heatmap_multires_enabled[layer_idx];
            hs.multires_blend = std::clamp(use_global || layer_idx >= layer_heatmap_multires_blend.size() ? heatmap_multires_blend : layer_heatmap_multires_blend[layer_idx], 0.0f, 1.0f);
        };
        bool smooth_only_heatmap = true;
        bool any_active_heatmap = false;
        for (size_t i = 0; i < layers.size(); ++i) {
            const bool active_heatmap =
                layers[i].enabled &&
                i < layer_heatmap_enabled.size() &&
                i < layer_heatmap_max_zoom.size() &&
                layer_heatmap_enabled[i] &&
                zoom <= layer_heatmap_max_zoom[i];
            if (!active_heatmap) continue;
            any_active_heatmap = true;
            const int algo = (i < layer_heatmap_algo.size() && layer_heatmap_algo[i] >= 0) ? layer_heatmap_algo[i] : heatmap_algo;
            if (algo != 1 && algo != 2 && algo != 4) smooth_only_heatmap = false;
        }
        if (!any_active_heatmap) smooth_only_heatmap = false;
        uint64_t heatmap_key = 1469598103934665603ULL;
        if (!smooth_only_heatmap) hash_combine_u64(heatmap_key, (uint64_t)zoom);
        hash_combine_f(heatmap_key, global_heat_cell_px);
        hash_combine_u64(heatmap_key, (uint64_t)heatmap_algo);
        hash_combine_u64(heatmap_key, (uint64_t)heatmap_quality_preset);
        hash_combine_f(heatmap_key, heatmap_bandwidth_px);
        hash_combine_f(heatmap_key, heatmap_blur_sigma_px);
        hash_combine_f(heatmap_key, heatmap_percentile_clip);
        hash_combine_u64(heatmap_key, heatmap_zoom_adaptive_bandwidth ? 1ULL : 0ULL);
        hash_combine_u64(heatmap_key, heatmap_multires_enabled ? 1ULL : 0ULL);
        hash_combine_f(heatmap_key, heatmap_multires_blend);
        hash_combine_u64(heatmap_key, filter_enabled ? 1ULL : 0ULL);
        for (const char* p = filter_blocklot; *p; ++p) hash_combine_u64(heatmap_key, (uint64_t)(unsigned char)(*p));
        for (const char* p = filter_status; *p; ++p) hash_combine_u64(heatmap_key, (uint64_t)(unsigned char)(*p));
        for (const char* p = filter_address; *p; ++p) hash_combine_u64(heatmap_key, (uint64_t)(unsigned char)(*p));
        for (const char* p = filter_owner; *p; ++p) hash_combine_u64(heatmap_key, (uint64_t)(unsigned char)(*p));
        for (const char* p = filter_zip; *p; ++p) hash_combine_u64(heatmap_key, (uint64_t)(unsigned char)(*p));
        hash_combine_u64(heatmap_key, filter_use_date ? 1ULL : 0ULL);
        hash_combine_u64(heatmap_key, (uint64_t)filter_year_min);
        hash_combine_u64(heatmap_key, (uint64_t)filter_year_max);
        hash_combine_u64(heatmap_key, crime_filter_enabled ? 1ULL : 0ULL);
        hash_combine_u64(heatmap_key, crime_filter_use_year ? 1ULL : 0ULL);
        hash_combine_u64(heatmap_key, (uint64_t)crime_year_min);
        hash_combine_u64(heatmap_key, (uint64_t)crime_year_max);
        hash_combine_u64(heatmap_key, crime_filter_homicide ? 1ULL : 0ULL);
        hash_combine_u64(heatmap_key, crime_filter_robbery ? 1ULL : 0ULL);
        hash_combine_u64(heatmap_key, crime_filter_assault ? 1ULL : 0ULL);
        hash_combine_u64(heatmap_key, crime_filter_burglary ? 1ULL : 0ULL);
        hash_combine_u64(heatmap_key, crime_filter_theft ? 1ULL : 0ULL);
        hash_combine_u64(heatmap_key, crime_filter_auto_theft ? 1ULL : 0ULL);
        hash_combine_u64(heatmap_key, crime_filter_drug ? 1ULL : 0ULL);
        hash_combine_u64(heatmap_key, crime_filter_shooting ? 1ULL : 0ULL);
        for (size_t i = 0; i < layers.size(); ++i) {
            hash_combine_u64(heatmap_key, layers[i].enabled ? 1ULL : 0ULL);
            hash_combine_u64(heatmap_key, (i < layer_heatmap_enabled.size() && layer_heatmap_enabled[i]) ? 1ULL : 0ULL);
            hash_combine_u64(heatmap_key, (uint64_t)((i < layer_heatmap_max_zoom.size()) ? layer_heatmap_max_zoom[i] : 13));
            hash_combine_u64(heatmap_key, (i < layer_heatmap_use_gradient.size() && layer_heatmap_use_gradient[i]) ? 1ULL : 0ULL);
            hash_combine_u64(heatmap_key, (uint64_t)((i < layer_heatmap_algo.size()) ? (layer_heatmap_algo[i] + 1) : 0));
            const bool use_global = i >= layer_heatmap_use_global_settings.size() || layer_heatmap_use_global_settings[i];
            hash_combine_u64(heatmap_key, use_global ? 1ULL : 0ULL);
            if (!use_global) {
                hash_combine_f(heatmap_key, i < layer_heatmap_cell_px.size() ? layer_heatmap_cell_px[i] : global_heat_cell_px);
                hash_combine_f(heatmap_key, i < layer_heatmap_bandwidth_px.size() ? layer_heatmap_bandwidth_px[i] : heatmap_bandwidth_px);
                hash_combine_f(heatmap_key, i < layer_heatmap_blur_sigma_px.size() ? layer_heatmap_blur_sigma_px[i] : heatmap_blur_sigma_px);
                hash_combine_f(heatmap_key, i < layer_heatmap_percentile_clip.size() ? layer_heatmap_percentile_clip[i] : heatmap_percentile_clip);
                hash_combine_u64(heatmap_key, (i < layer_heatmap_zoom_adaptive_bandwidth.size() && layer_heatmap_zoom_adaptive_bandwidth[i]) ? 1ULL : 0ULL);
                hash_combine_u64(heatmap_key, (i < layer_heatmap_multires_enabled.size() && layer_heatmap_multires_enabled[i]) ? 1ULL : 0ULL);
                hash_combine_f(heatmap_key, i < layer_heatmap_multires_blend.size() ? layer_heatmap_multires_blend[i] : heatmap_multires_blend);
            }
            hash_combine_u64(heatmap_key, (uint64_t)layers[i].features.size());
        }
        if (heatmap_async_inflight && heatmap_async_future.valid() &&
            heatmap_async_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            const auto heat_prof_begin = std::chrono::steady_clock::now();
            auto done = heatmap_async_future.get();
            heatmap_async_inflight = false;
            if (done.first == heatmap_key) {
                heatmap_cached_cells = std::move(done.second.cells);
                if (done.second.has_raster && done.second.raster.w > 0 && done.second.raster.h > 0 && !done.second.raster.rgba.empty()) {
                    heatmap_cached_raster_meta = done.second.raster;
                    heatmap_raster_texture_valid = uploadRgbaTexture(
                        done.second.raster.rgba.data(),
                        (uint32_t)done.second.raster.w,
                        (uint32_t)done.second.raster.h,
                        heatmap_raster_texture);
                    if (heatmap_raster_texture_valid) heatmap_raster_cache_key = done.first;
                } else {
                    if (!smooth_only_heatmap) {
                        destroyTileTexture(heatmap_raster_texture);
                        heatmap_raster_texture_valid = false;
                        heatmap_cached_raster_meta = {};
                        heatmap_raster_cache_key = 0;
                    }
                }
                heatmap_cache_key = done.first;
                heatmap_cache_valid = true;
            }
            prof_heatmap_ms_last.store(prof_ms_since(heat_prof_begin), std::memory_order_relaxed);
        }
        const bool can_use_cached_heatmap = heatmap_cache_valid && heatmap_cache_key == heatmap_key;
        bool should_recompute_heatmap = (!can_use_cached_heatmap) && (!heatmap_controls_active);
        const auto layer_prof_begin = std::chrono::steady_clock::now();
        for (size_t layer_idx = 0; layer_idx < layers.size(); ++layer_idx) {
            auto& l = layers[layer_idx];
            if (!l.enabled) continue;
            const bool is_zoning_layer = ((int)layer_idx == zoning_layer_idx);
            const bool is_heat_layer = !l.heatmap_field.empty();
            float heat_min = std::numeric_limits<float>::infinity();
            float heat_max = -std::numeric_limits<float>::infinity();
            if (is_heat_layer) {
                for (size_t fi = 0; fi < l.features.size(); ++fi) {
                    const auto& fg = l.features[fi];
                    if (!feature_passes_filters(layer_idx, fi, fg)) continue;
                    float v = 0.0f;
                    if (!try_get_prop_float(fg, l.heatmap_field, v)) continue;
                    heat_min = std::min(heat_min, v);
                    heat_max = std::max(heat_max, v);
                }
            }
            const bool heat_range_valid = std::isfinite(heat_min) && std::isfinite(heat_max) && heat_max > heat_min;
            ImU32 c = ImGui::ColorConvertFloat4ToU32(l.color);
            const bool layer_uses_heatmap_for_cache =
                layer_idx < layer_heatmap_enabled.size() &&
                layer_idx < layer_heatmap_max_zoom.size() &&
                layer_heatmap_enabled[layer_idx] &&
                zoom <= layer_heatmap_max_zoom[layer_idx];
            bool have_candidates = !should_recompute_heatmap || !layer_uses_heatmap_for_cache;
            if (have_candidates) {
                have_candidates = queryLayerSpatialIndex(
                    layer_spatial[layer_idx], view_min_lon, view_min_lat, view_max_lon, view_max_lat, render_candidates);
            }
            if (have_candidates) {
                for (uint32_t fidx : render_candidates) {
                    prof_features_considered_frame++;
                    const bool layer_uses_heatmap =
                        layer_idx < layer_heatmap_enabled.size() &&
                        layer_idx < layer_heatmap_max_zoom.size() &&
                        layer_heatmap_enabled[layer_idx] &&
                        zoom <= layer_heatmap_max_zoom[layer_idx];
                    if (smooth_only_heatmap && should_recompute_heatmap && layer_uses_heatmap && fidx % 2 != 0) continue;
                    if (fidx >= l.features.size()) continue;
                    auto& fg = l.features[(size_t)fidx];
                    if (!feature_passes_filters(layer_idx, (size_t)fidx, fg)) continue;
                    ImU32 feature_c = c;
                    if (is_heat_layer) {
                        float v = 0.0f;
                        if (try_get_prop_float(fg, l.heatmap_field, v) && heat_range_valid) {
                            const float t = (v - heat_min) / (heat_max - heat_min);
                            feature_c = ImGui::ColorConvertFloat4ToU32(heat_color(t));
                        }
                    }
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

                    if (layer_uses_heatmap) {
                        if (can_use_cached_heatmap) continue;
                        const ImVec2 center((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
                        HeatSample hs;
                        hs.x = center.x;
                        hs.y = center.y;
                        hs.lon = (fg.extent.min_lon + fg.extent.max_lon) * 0.5f;
                        hs.lat = (fg.extent.min_lat + fg.extent.max_lat) * 0.5f;
                        hs.color = ImGui::ColorConvertU32ToFloat4(feature_c);
                        hs.prefer_gradient = layer_idx < layer_heatmap_use_gradient.size() ? layer_heatmap_use_gradient[layer_idx] : true;
                        resolve_layer_heat_settings(layer_idx, hs);
                        heat_samples.push_back(hs);
                        continue;
                    }

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
                        ImVec2 pw = lonLatToWorldPx(fg.extent.min_lon, fg.extent.min_lat, math_zoom);
                        ImVec2 ps = project_world(pw);
                        if (ps.x >= origin.x && ps.x <= origin.x + size.x && ps.y >= origin.y && ps.y <= origin.y + size.y) {
                            if ((int)layer_idx == vacant_notice_layer_idx || (int)layer_idx == vacant_rehab_layer_idx ||
                                (int)layer_idx == tax_lien_layer_idx || (int)layer_idx == tax_sale_layer_idx) {
                                continue;
                            }
                            float r = std::clamp((float)(2.0 * zoom_scale + 1.5), 2.0f, 6.0f);
                            draw->AddCircleFilled(ps, r, feature_c);
                            prof_features_drawn_frame++;
                        }
                    }

                }
                continue;
            }
            const size_t smooth_sample_stride =
                (smooth_only_heatmap && should_recompute_heatmap && layer_uses_heatmap_for_cache && l.features.size() > kMaxSmoothHeatSamplesPerLayer)
                    ? std::max<size_t>(1, (l.features.size() + kMaxSmoothHeatSamplesPerLayer - 1) / kMaxSmoothHeatSamplesPerLayer)
                    : 1;
            for (auto& fg : l.features) {
                prof_features_considered_frame++;
                size_t fi = (size_t)(&fg - &l.features[0]);
                if (!feature_passes_filters(layer_idx, fi, fg)) continue;
                ImU32 feature_c = c;
                if (is_heat_layer) {
                    float v = 0.0f;
                    if (try_get_prop_float(fg, l.heatmap_field, v) && heat_range_valid) {
                        const float t = (v - heat_min) / (heat_max - heat_min);
                        feature_c = ImGui::ColorConvertFloat4ToU32(heat_color(t));
                    }
                }
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
                const bool layer_uses_heatmap =
                    layer_idx < layer_heatmap_enabled.size() &&
                    layer_idx < layer_heatmap_max_zoom.size() &&
                    layer_heatmap_enabled[layer_idx] &&
                    zoom <= layer_heatmap_max_zoom[layer_idx];
                if (!layer_uses_heatmap && (p1.x < origin.x || p0.x > origin.x + size.x || p1.y < origin.y || p0.y > origin.y + size.y)) continue;
                if (layer_uses_heatmap) {
                    if (can_use_cached_heatmap) continue;
                    if (smooth_sample_stride > 1 && (fi % smooth_sample_stride) != 0) continue;
                    const ImVec2 center((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
                    HeatSample hs;
                    hs.x = center.x;
                    hs.y = center.y;
                    hs.lon = (fg.extent.min_lon + fg.extent.max_lon) * 0.5f;
                    hs.lat = (fg.extent.min_lat + fg.extent.max_lat) * 0.5f;
                    hs.color = ImGui::ColorConvertU32ToFloat4(feature_c);
                    hs.prefer_gradient = layer_idx < layer_heatmap_use_gradient.size() ? layer_heatmap_use_gradient[layer_idx] : true;
                    resolve_layer_heat_settings(layer_idx, hs);
                    heat_samples.push_back(hs);
                    continue;
                }

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
                    ImVec2 pw = lonLatToWorldPx(fg.extent.min_lon, fg.extent.min_lat, math_zoom);
                    ImVec2 ps = project_world(pw);
                    if (ps.x >= origin.x && ps.x <= origin.x + size.x && ps.y >= origin.y && ps.y <= origin.y + size.y) {
                        // Vacant/tax source datasets are point feeds; optionally aggregate vacants into heatmap.
                        if (layer_idx == (size_t)vacant_notice_layer_idx || layer_idx == (size_t)vacant_rehab_layer_idx ||
                            layer_idx == (size_t)tax_lien_layer_idx || layer_idx == (size_t)tax_sale_layer_idx) {
                            continue;
                        }
                        float r = std::clamp((float)(2.0 * zoom_scale + 1.5), 2.0f, 6.0f);
                        draw->AddCircleFilled(ps, r, feature_c);
                        prof_features_drawn_frame++;
                    }
                }

            }
        }
        prof_layer_ms_last.store(prof_ms_since(layer_prof_begin), std::memory_order_relaxed);
        prof_heat_samples_last.store(heat_samples.size(), std::memory_order_relaxed);
        if (should_recompute_heatmap && !heatmap_async_inflight) {
            const auto heat_prof_begin = std::chrono::steady_clock::now();
            const auto samples_copy = heat_samples;
            const uint64_t key_copy = heatmap_key;
            const float cell_copy = global_heat_cell;
            const float bw_copy = heatmap_bandwidth_px;
            const float blur_copy = heatmap_blur_sigma_px;
            const float clip_copy = heatmap_percentile_clip;
            const bool adaptive_copy = heatmap_zoom_adaptive_bandwidth;
            const bool multires_enabled_copy = heatmap_multires_enabled;
            const float multires_blend_copy = heatmap_multires_blend;
            const int zoom_copy = zoom;
            const float ox = origin.x;
            const float oy = origin.y;
            const float vw = std::max(1.0f, size.x);
            const float vh = std::max(1.0f, size.y);
            heatmap_async_future = std::async(std::launch::async, [=]() -> std::pair<uint64_t, HeatmapRenderData> {
                struct Bin {
                    double d = 0.0, w = 0.0, r = 0.0, g = 0.0, b = 0.0;
                    int gv = 0, sv = 0;
                    bool hex = false;
                };
                auto key = [](int x, int y) -> uint64_t { return ((uint64_t)(uint32_t)x << 32) | (uint32_t)y; };
                auto add = [&](std::unordered_map<uint64_t, Bin>& bins, int bx, int by, const HeatSample& s, double w, bool hex = false) {
                    Bin& b = bins[key(bx, by)];
                    b.d += w; b.w += w; b.r += s.color.x * w; b.g += s.color.y * w; b.b += s.color.z * w;
                    if (s.prefer_gradient) b.gv++; else b.sv++;
                    b.hex = b.hex || hex;
                };
                auto build_group = [&](const std::vector<HeatSample>& group) {
                    std::vector<CachedHeatCell> out;
                    if (group.empty()) return out;
                    const HeatSample& settings = group.front();
                    const float cell = std::max(4.0f, settings.cell_px);
                    const float zf = 1.0f;
                    const float bw = std::max(1.0f, settings.bandwidth_px * zf);
                    const float blur = std::max(0.0f, settings.blur_sigma_px);
                    std::unordered_map<uint64_t, Bin> bins;
                    std::unordered_map<uint64_t, Bin> coarse;
                    auto kde_add = [&](const HeatSample& s, float sigma_px, double ws) {
                        const float sigma = std::max(1.0f, sigma_px);
                        const int radius = std::max(1, (int)std::ceil((3.0f * sigma) / cell));
                        const int cbx = (int)((s.x - ox) / cell), cby = (int)((s.y - oy) / cell);
                        const float two = 2.0f * sigma * sigma;
                        for (int dy = -radius; dy <= radius; ++dy) for (int dx = -radius; dx <= radius; ++dx) {
                            const float cx = ox + (cbx + dx + 0.5f) * cell;
                            const float cy = oy + (cby + dy + 0.5f) * cell;
                            const float d2 = (cx - s.x) * (cx - s.x) + (cy - s.y) * (cy - s.y);
                            const double w = std::exp(-(double)d2 / (double)two) * ws;
                            if (w < 1e-5) continue;
                            add(bins, cbx + dx, cby + dy, s, w);
                        }
                    };
                    for (const auto& s : group) {
                        const int algo = std::clamp(s.algo, 0, 4);
                        if (algo == 0) {
                            add(bins, (int)((s.x - ox) / cell), (int)((s.y - oy) / cell), s, 1.0);
                        } else if (algo == 1) {
                            kde_add(s, bw, 1.0);
                        } else if (algo == 2) {
                            kde_add(s, std::sqrt(bw * bw + blur * blur), 1.0);
                        } else if (algo == 3) {
                            const float hh = std::max(1.0f, cell * 0.8660254f);
                            const float gx = (s.x - ox) / cell, gy = (s.y - oy) / hh;
                            add(bins, (int)std::round(gx - gy * 0.5f), (int)std::round(gy), s, 1.0, true);
                        } else {
                            const float coarse_cell = cell * 2.0f;
                            add(bins, (int)((s.x - ox) / cell), (int)((s.y - oy) / cell), s, 1.0);
                            add(coarse, (int)((s.x - ox) / coarse_cell), (int)((s.y - oy) / coarse_cell), s, 1.0);
                        }
                    }
                    if (settings.multires_enabled && !coarse.empty()) {
                        const double blend = std::clamp((double)settings.multires_blend, 0.0, 1.0);
                        for (auto& kv : bins) {
                            int bx = (int)(uint32_t)(kv.first >> 32), by = (int)(uint32_t)(kv.first & 0xffffffffu);
                            auto it = coarse.find(key(bx / 2, by / 2));
                            if (it != coarse.end()) kv.second.d = kv.second.d * (1.0 - blend) + it->second.d * blend;
                        }
                    }
                    double maxd = 0.0;
                    std::vector<double> vals; vals.reserve(bins.size());
                    for (const auto& kv : bins) vals.push_back(kv.second.d);
                    if (!vals.empty()) {
                        maxd = *std::max_element(vals.begin(), vals.end());
                        const double pct = std::clamp((double)settings.percentile_clip, 50.0, 100.0);
                        if (pct < 100.0) {
                            size_t k = (size_t)std::clamp((int)std::floor((pct / 100.0) * (double)(vals.size() - 1)), 0, (int)vals.size() - 1);
                            std::nth_element(vals.begin(), vals.begin() + k, vals.end());
                            maxd = std::max(1e-6, vals[k]);
                        }
                    }
                    if (maxd <= 0.0) return out;
                    out.reserve(bins.size());
                    for (const auto& kv : bins) {
                        const int bx = (int)(uint32_t)(kv.first >> 32), by = (int)(uint32_t)(kv.first & 0xffffffffu);
                        const Bin& b = kv.second;
                        const float t = std::clamp((float)(b.d / maxd), 0.0f, 1.0f);
                        const float iw = (b.w > 0.0) ? (float)(1.0 / b.w) : 0.0f;
                        const ImVec4 base(std::clamp((float)(b.r * iw), 0.0f, 1.0f), std::clamp((float)(b.g * iw), 0.0f, 1.0f), std::clamp((float)(b.b * iw), 0.0f, 1.0f), 1.0f);
                        const bool grad = b.gv >= b.sv;
                        ImVec4 c = grad ? ImVec4(0.12f + 0.70f * t, 0.35f + 0.30f * t, 0.75f - 0.62f * t, 0.62f)
                                        : ImVec4(base.x * (0.30f + 0.70f * t), base.y * (0.30f + 0.70f * t), base.z * (0.30f + 0.70f * t), 0.22f + 0.58f * t);
                        const int algo = std::clamp(settings.algo, 0, 4);
                        const bool smooth_surface = algo == 1 || algo == 2 || algo == 4;
                        CachedHeatCell cc;
                        cc.draw_outline = !smooth_surface;
                        cc.fill = ImGui::ColorConvertFloat4ToU32(c);
                        cc.outline = smooth_surface ? 0 : ImGui::ColorConvertFloat4ToU32(ImVec4(c.x * 0.75f, c.y * 0.75f, c.z * 0.75f, 0.85f));
                        if (b.hex) {
                            const float hh = std::max(1.0f, cell * 0.8660254f);
                            cc.is_hex = true;
                            cc.cx = ox + ((float)bx + (float)by * 0.5f + 0.5f) * cell;
                            cc.cy = oy + ((float)by + 0.5f) * hh;
                            cc.hw = cell * 0.5f;
                            cc.hh = hh * 0.5f;
                        } else {
                            const float overlap = smooth_surface ? std::max(0.75f, cell * 0.035f) : 0.0f;
                            cc.is_hex = false;
                            cc.x0 = ox + (float)bx * cell - overlap; cc.y0 = oy + (float)by * cell - overlap;
                            cc.x1 = cc.x0 + cell + overlap * 2.0f; cc.y1 = cc.y0 + cell + overlap * 2.0f;
                        }
                        out.push_back(cc);
                    }
                    return out;
                };
                std::unordered_map<int, std::vector<HeatSample>> by_layer;
                for (const auto& s : samples_copy) by_layer[s.layer].push_back(s);
                HeatmapRenderData out;
                float raster_min_lon = std::numeric_limits<float>::infinity();
                float raster_min_lat = std::numeric_limits<float>::infinity();
                float raster_max_lon = -std::numeric_limits<float>::infinity();
                float raster_max_lat = -std::numeric_limits<float>::infinity();
                for (const auto& s : samples_copy) {
                    const int algo = std::clamp(s.algo, 0, 4);
                    if (algo != 1 && algo != 2 && algo != 4) continue;
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
                const int rw = std::clamp((int)std::lround((float)kSmoothHeatRasterBasePx * std::sqrt(aspect)), 384, kSmoothHeatRasterMaxPx);
                const int rh = std::clamp((int)std::lround((float)rw / aspect), 384, kSmoothHeatRasterMaxPx);
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
                    const float density_scale = 1.0f;
                    const float zf = 1.0f;
                    float sigma = std::max(1.0f, settings.bandwidth_px * zf);
                    if (settings.algo == 2) sigma = std::sqrt(sigma * sigma + settings.blur_sigma_px * settings.blur_sigma_px);
                    if (settings.algo == 4 && settings.multires_enabled) sigma *= 1.0f + std::clamp(settings.multires_blend, 0.0f, 1.0f);
                    const float sigma_r = std::max(1.0f, sigma * density_scale);
                    const int radius = std::max(1, (int)std::ceil(3.0f * sigma_r));
                    const float two = 2.0f * sigma_r * sigma_r;
                    std::vector<float> density((size_t)rw * (size_t)rh, 0.0f);
                    std::vector<float> cr(density.size(), 0.0f), cg(density.size(), 0.0f), cb(density.size(), 0.0f), cw(density.size(), 0.0f);
                    int gradient_votes = 0;
                    int solid_votes = 0;
                    for (const auto& s : group) {
                        if (s.prefer_gradient) gradient_votes++; else solid_votes++;
                        const float px = (s.lon - raster_min_lon) * sx;
                        const float py = (raster_max_lat - s.lat) * sy;
                        const int cx0 = (int)std::floor(px);
                        const int cy0 = (int)std::floor(py);
                        for (int yy = cy0 - radius; yy <= cy0 + radius; ++yy) {
                            if (yy < 0 || yy >= rh) continue;
                            for (int xx = cx0 - radius; xx <= cx0 + radius; ++xx) {
                                if (xx < 0 || xx >= rw) continue;
                                const float dx = ((float)xx + 0.5f) - px;
                                const float dy = ((float)yy + 0.5f) - py;
                                const float d2 = dx * dx + dy * dy;
                                const float w = std::exp(-d2 / two);
                                if (w < 1e-5) continue;
                                const size_t i = (size_t)yy * (size_t)rw + (size_t)xx;
                                density[i] += w;
                                cr[i] += s.color.x * w;
                                cg[i] += s.color.y * w;
                                cb[i] += s.color.z * w;
                                cw[i] += w;
                            }
                        }
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
                    const bool use_gradient = gradient_votes >= solid_votes;
                    for (int y = 0; y < rh; ++y) {
                        for (int x = 0; x < rw; ++x) {
                            const size_t i = (size_t)y * (size_t)rw + (size_t)x;
                            if (density[i] <= 1e-6) continue;
                            const float t = std::clamp((float)(density[i] / maxd), 0.0f, 1.0f);
                            const float iw = cw[i] > 0.0f ? 1.0f / cw[i] : 0.0f;
                            const ImVec4 base(std::clamp((float)cr[i] * iw, 0.0f, 1.0f), std::clamp((float)cg[i] * iw, 0.0f, 1.0f), std::clamp((float)cb[i] * iw, 0.0f, 1.0f), 1.0f);
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
                    const int algo = std::clamp(kv.second.front().algo, 0, 4);
                    if (algo == 1 || algo == 2 || algo == 4) {
                        build_raster_group(kv.second);
                    } else {
                        std::vector<CachedHeatCell> group_cells = build_group(kv.second);
                        out.cells.insert(out.cells.end(), group_cells.begin(), group_cells.end());
                    }
                }
                return {key_copy, std::move(out)};
            });
            heatmap_async_inflight = true;
            should_recompute_heatmap = false;
            prof_heatmap_ms_last.store(prof_ms_since(heat_prof_begin), std::memory_order_relaxed);
        } else if (heatmap_async_inflight) {
            should_recompute_heatmap = false;
        }
        std::vector<CachedHeatCell> frame_heat_cells;
        if (should_recompute_heatmap) {
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
        };

        std::unordered_map<uint64_t, HeatBin> global_heat_bins;
        std::unordered_map<uint64_t, HeatBin> coarse_heat_bins;
        double max_density = 0.0;
        const float zoom_factor = heatmap_zoom_adaptive_bandwidth ? std::clamp(1.0f + 0.12f * (float)(kMaxZoom - zoom), 1.0f, 3.0f) : 1.0f;
        const float bw_px = std::max(1.0f, heatmap_bandwidth_px * zoom_factor);
        const float blur_sigma = std::max(0.0f, heatmap_blur_sigma_px);

        auto add_grid_sample = [&](const HeatSample& s, float cell, double w) {
            const int bx = (int)((s.x - origin.x) / cell);
            const int by = (int)((s.y - origin.y) / cell);
            HeatBin& b = global_heat_bins[pack_bin(bx, by)];
            accum_bin(b, s, w);
        };
        auto add_kde_sample = [&](const HeatSample& s, float cell, float sigma_px, double wscale) {
            const float sigma = std::max(1.0f, sigma_px);
            const int radius = std::max(1, (int)std::ceil((3.0f * sigma) / cell));
            const int cbx = (int)((s.x - origin.x) / cell);
            const int cby = (int)((s.y - origin.y) / cell);
            const float two_sigma2 = 2.0f * sigma * sigma;
            for (int dy = -radius; dy <= radius; ++dy) {
                for (int dx = -radius; dx <= radius; ++dx) {
                    const float cx = origin.x + (cbx + dx + 0.5f) * cell;
                    const float cy = origin.y + (cby + dy + 0.5f) * cell;
                    const float d2 = (cx - s.x) * (cx - s.x) + (cy - s.y) * (cy - s.y);
                    const double w = std::exp(-(double)d2 / (double)two_sigma2) * wscale;
                    if (w < 1e-5) continue;
                    HeatBin& b = global_heat_bins[pack_bin(cbx + dx, cby + dy)];
                    accum_bin(b, s, w);
                }
            }
        };

        if (heatmap_algo == 0) {
            for (const auto& s : heat_samples) add_grid_sample(s, global_heat_cell, 1.0);
        } else if (heatmap_algo == 1) {
            for (const auto& s : heat_samples) add_kde_sample(s, global_heat_cell, bw_px, 1.0);
        } else if (heatmap_algo == 2) {
            // Explicit splat grid + separable blur pipeline (CPU implementation of the same model).
            if (!heat_samples.empty()) {
                const int grid_w = std::max(1, (int)std::ceil(size.x / global_heat_cell));
                const int grid_h = std::max(1, (int)std::ceil(size.y / global_heat_cell));
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
                    const int bx = (int)((s.x - origin.x) / global_heat_cell);
                    const int by = (int)((s.y - origin.y) / global_heat_cell);
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
        } else if (heatmap_algo == 3) {
            const float hex_w = global_heat_cell;
            const float hex_h = std::max(1.0f, hex_w * 0.8660254f);
            for (const auto& s : heat_samples) {
                const float gx = (s.x - origin.x) / hex_w;
                const float gy = (s.y - origin.y) / hex_h;
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
                const int bx = (int)((s.x - origin.x) / coarse);
                const int by = (int)((s.y - origin.y) / coarse);
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

        if (!global_heat_bins.empty()) {
            std::vector<double> dens;
            dens.reserve(global_heat_bins.size());
            for (const auto& kv : global_heat_bins) dens.push_back(kv.second.density);
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

        if (!global_heat_bins.empty() && max_density > 0.0) {
            for (const auto& kv : global_heat_bins) {
                int bx = 0, by = 0;
                unpack_bin(kv.first, bx, by);
                const HeatBin& bin = kv.second;
                const float t = std::clamp((float)(bin.density / max_density), 0.0f, 1.0f);
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
                if (monochrome_bin || !prefer_gradient) {
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
                    const ImVec4 hc = heat_color(t);
                    fill = ImGui::ColorConvertFloat4ToU32(ImVec4(hc.x, hc.y, hc.z, 0.62f));
                    outline = ImGui::ColorConvertFloat4ToU32(ImVec4(hc.x * 0.75f, hc.y * 0.75f, hc.z * 0.75f, 0.85f));
                }

                if (heatmap_algo == 3) {
                    const float hex_w = global_heat_cell;
                    const float hex_h = std::max(1.0f, hex_w * 0.8660254f);
                    const float cx = origin.x + ((float)bx + (float)by * 0.5f + 0.5f) * hex_w;
                    const float cy = origin.y + ((float)by + 0.5f) * hex_h;
                    CachedHeatCell c;
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
                    const bool smooth_surface = heatmap_algo == 1 || heatmap_algo == 2 || heatmap_algo == 4;
                    const float overlap = smooth_surface ? std::max(0.75f, global_heat_cell * 0.035f) : 0.0f;
                    const float x0 = origin.x + (float)bx * global_heat_cell;
                    const float y0 = origin.y + (float)by * global_heat_cell;
                    const float x1 = x0 + global_heat_cell;
                    const float y1 = y0 + global_heat_cell;
                    CachedHeatCell c;
                    c.is_hex = false;
                    c.draw_outline = !smooth_surface;
                    c.x0 = x0 - overlap; c.y0 = y0 - overlap; c.x1 = x1 + overlap; c.y1 = y1 + overlap;
                    c.fill = fill;
                    c.outline = smooth_surface ? 0 : outline;
                    frame_heat_cells.push_back(c);
                }
            }
        }
        heatmap_cached_cells = frame_heat_cells;
        heatmap_cache_key = heatmap_key;
        heatmap_cache_valid = true;
        }
        const std::vector<CachedHeatCell>& draw_cells =
            can_use_cached_heatmap ? heatmap_cached_cells :
            (should_recompute_heatmap ? frame_heat_cells : heatmap_cached_cells);
        const bool draw_stale_raster_while_rebuilding =
            smooth_only_heatmap &&
            heatmap_async_inflight &&
            heatmap_raster_texture_valid;
        const bool draw_current_raster =
            heatmap_raster_texture_valid &&
            heatmap_raster_cache_key == heatmap_key;
        if ((draw_current_raster || draw_stale_raster_while_rebuilding) && heatmap_raster_texture.descriptor) {
            ImVec2 raster_nw = lonLatToWorldPx(heatmap_cached_raster_meta.min_lon, heatmap_cached_raster_meta.max_lat, math_zoom);
            ImVec2 raster_se = lonLatToWorldPx(heatmap_cached_raster_meta.max_lon, heatmap_cached_raster_meta.min_lat, math_zoom);
            ImVec2 rp0 = project_world(raster_nw);
            ImVec2 rp1 = project_world(raster_se);
            draw->AddImage((ImTextureID)heatmap_raster_texture.descriptor, rp0, rp1);
        }
        for (const auto& c : draw_cells) {
            if (c.is_hex) {
                ImVec2 pts[6] = {
                    ImVec2(c.cx - c.hw, c.cy),
                    ImVec2(c.cx - c.hw * 0.5f, c.cy - c.hh),
                    ImVec2(c.cx + c.hw * 0.5f, c.cy - c.hh),
                    ImVec2(c.cx + c.hw, c.cy),
                    ImVec2(c.cx + c.hw * 0.5f, c.cy + c.hh),
                    ImVec2(c.cx - c.hw * 0.5f, c.cy + c.hh),
                };
                draw->AddConvexPolyFilled(pts, 6, c.fill);
                if (c.draw_outline) draw->AddPolyline(pts, 6, c.outline, ImDrawFlags_Closed, 1.0f);
            } else {
                draw->AddRectFilled(ImVec2(c.x0, c.y0), ImVec2(c.x1, c.y1), c.fill);
                if (c.draw_outline) draw->AddRect(ImVec2(c.x0, c.y0), ImVec2(c.x1, c.y1), c.outline, 0.0f, 0, 1.0f);
            }
        }

        const auto overlay_prof_begin = std::chrono::steady_clock::now();
        // Render vacant overlays as a final top pass so they remain visible regardless of layer order.
        size_t visible_vacant_parcels_counter = 0;
        const bool parcel_uses_heatmap =
            parcel_layer_idx >= 0 &&
            (size_t)parcel_layer_idx < layer_heatmap_enabled.size() &&
            (size_t)parcel_layer_idx < layer_heatmap_max_zoom.size() &&
            layer_heatmap_enabled[(size_t)parcel_layer_idx] &&
            zoom <= layer_heatmap_max_zoom[(size_t)parcel_layer_idx];
        if (!parcel_uses_heatmap && (vacant_notice_enabled || vacant_rehab_enabled) && parcel_layer_idx >= 0) {
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
        prof_overlay_ms_last.store(prof_ms_since(overlay_prof_begin), std::memory_order_relaxed);

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

        {
            char zoom_label[32];
            std::snprintf(zoom_label, sizeof(zoom_label), "Zoom %d", zoom);
            const ImVec2 text_size = ImGui::CalcTextSize(zoom_label);
            const float pad_x = 10.0f;
            const float pad_y = 6.0f;
            const ImVec2 box_max(origin.x + size.x - 12.0f, origin.y + size.y - 12.0f);
            const ImVec2 box_min(box_max.x - text_size.x - pad_x * 2.0f, box_max.y - text_size.y - pad_y * 2.0f);
            draw->AddRectFilled(box_min, box_max, IM_COL32(17, 24, 32, 205), 7.0f);
            draw->AddRect(box_min, box_max, IM_COL32(255, 255, 255, 70), 7.0f);
            draw->AddText(ImVec2(box_min.x + pad_x, box_min.y + pad_y), IM_COL32(245, 248, 250, 245), zoom_label);
        }

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
        ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Time Cube")) {
            if (time_cube_ui_done.load(std::memory_order_relaxed)) {
                if (time_cube_ui_worker.joinable()) time_cube_ui_worker.join();
                time_cube_ui_done.store(false, std::memory_order_relaxed);
                time_cube_ui_running.store(false, std::memory_order_relaxed);
            }
            auto lightweight_layers_copy = [&]() {
                std::vector<LayerDef> copy;
                copy.reserve(layers.size());
                for (const auto& l : layers) {
                    LayerDef row;
                    row.name = l.name;
                    row.file = l.file;
                    row.source_url = l.source_url;
                    row.description = l.description;
                    row.heatmap_field = l.heatmap_field;
                    row.subcategory = l.subcategory;
                    row.scale = l.scale;
                    row.color = l.color;
                    row.enabled = l.enabled;
                    row.category = l.category;
                    copy.push_back(std::move(row));
                }
                return copy;
            };
            auto start_time_cube_job = [&](bool rebuild) {
                if (time_cube_ui_running.load(std::memory_order_relaxed)) return;
                if (time_cube_ui_worker.joinable()) time_cube_ui_worker.join();
                TimeCubeQuery q;
                q.year_from = 1900;
                q.year_to = 2100;
                q.rebuild = rebuild;
                std::vector<LayerDef> layer_meta = lightweight_layers_copy();
                {
                    std::lock_guard<std::mutex> lk(time_cube_ui_mutex);
                    time_cube_ui_running.store(true, std::memory_order_relaxed);
                    time_cube_ui_done.store(false, std::memory_order_relaxed);
                    time_cube_ui_status = rebuild ? "Rebuilding indexes..." : "Refreshing indexes...";
                }
                time_cube_ui_worker = std::thread([&, q, layer_meta = std::move(layer_meta), rebuild]() mutable {
                    TimeCubeResult result = time_cube_service.query(layer_meta, q);
                    {
                        std::lock_guard<std::mutex> lk(time_cube_ui_mutex);
                        time_cube_ui_result = std::move(result);
                        time_cube_ui_loaded = true;
                        time_cube_ui_status = std::string(rebuild ? "Rebuilt " : "Loaded ") + std::to_string(time_cube_ui_result.datasets.size()) + " dataset indexes";
                        time_cube_ui_done.store(true, std::memory_order_relaxed);
                    }
                });
            };
            TimeCubeResult cube_view;
            bool cube_loaded = false;
            bool cube_running = false;
            std::string cube_status;
            {
                std::lock_guard<std::mutex> lk(time_cube_ui_mutex);
                cube_view = time_cube_ui_result;
                cube_loaded = time_cube_ui_loaded;
                cube_running = time_cube_ui_running.load(std::memory_order_relaxed);
                cube_status = time_cube_ui_status;
            }
            ImGui::TextUnformatted("Time-Series Cube");
            ImGui::Separator();
            ImGui::TextWrapped("A time cube standardizes event histories and annual snapshots as dataset x year x grain, with schema-declared measures.");
            ImGui::BeginDisabled(cube_running);
            if (ImGui::Button(cube_loaded ? "Refresh Indexes" : "Build Indexes")) start_time_cube_job(false);
            ImGui::SameLine();
            if (ImGui::Button("Rebuild Indexes")) start_time_cube_job(true);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", cube_status.c_str());
            ImGui::TextDisabled("REST endpoint: http://127.0.0.1:8787/time_cube");
            ImGui::TextDisabled("REST rebuild: /time_cube?rebuild=1");
            if (cube_running) ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(-1, 0), "Index job running");
            if (!cube_loaded) {
                ImGui::Separator();
                ImGui::TextWrapped("Press Build Indexes. The UI remains responsive; cached indexes are stored under data/cache/time_cube and invalidated by dataset and schema signatures.");
            } else {
                size_t recommended_count = 0;
                size_t local_count = 0;
                size_t matched_total = 0;
                int data_year_min = 2101;
                int data_year_max = 1899;
                for (const auto& d : cube_view.datasets) {
                    if (d.local) local_count++;
                    if (d.recommended) recommended_count++;
                    matched_total += d.matched_records;
                    for (const auto& yc : d.years) {
                        data_year_min = std::min(data_year_min, yc.first);
                        data_year_max = std::max(data_year_max, yc.first);
                    }
                }
                if (data_year_min <= data_year_max) {
                    time_cube_year_min = std::clamp(time_cube_year_min, data_year_min, data_year_max);
                    time_cube_year_max = std::clamp(time_cube_year_max, data_year_min, data_year_max);
                    if (time_cube_year_min > time_cube_year_max) std::swap(time_cube_year_min, time_cube_year_max);
                }
                if (time_cube_selected.size() != cube_view.datasets.size()) time_cube_selected.assign(cube_view.datasets.size(), true);
                if (ImGui::BeginTabBar("time_cube_workspace_tabs")) {
                    if (ImGui::BeginTabItem("Overview")) {
                        ImGui::Text("Time-ready datasets: %zu / %zu", recommended_count, cube_view.datasets.size());
                        ImGui::Text("Local datasets: %zu", local_count);
                        ImGui::Text("Matched records: %zu", matched_total);
                        if (data_year_min <= data_year_max) ImGui::Text("Covered years: %d-%d", data_year_min, data_year_max);
                        ImGui::SeparatorText("How to use this");
                        ImGui::TextWrapped("Use Datasets to choose series, Timeline to compare them, and Schema to audit exactly how each dataset is assigned to time.");
                        ImGui::TextWrapped("Use raw counts for operational volume, index-to-100 for trend comparison, and percent-of-max for shape comparison.");
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Timeline")) {
                        if (data_year_min <= data_year_max) {
                            ImGui::SliderInt("Year Min", &time_cube_year_min, data_year_min, data_year_max);
                            ImGui::SliderInt("Year Max", &time_cube_year_max, data_year_min, data_year_max);
                            if (time_cube_year_min > time_cube_year_max) std::swap(time_cube_year_min, time_cube_year_max);
                        }
                        const char* modes[] = {"Raw count/value", "Index first year = 100", "Percent of max"};
                        ImGui::Combo("Normalize", &time_cube_normalize_mode, modes, IM_ARRAYSIZE(modes));
                        ImVec2 plot_pos = ImGui::GetCursorScreenPos();
                        ImVec2 plot_size = ImGui::GetContentRegionAvail();
                        plot_size.y = std::min(plot_size.y, 360.0f);
                        if (plot_size.x < 240.0f) plot_size.x = 240.0f;
                        if (plot_size.y < 220.0f) plot_size.y = 220.0f;
                        ImGui::InvisibleButton("time_cube_timeline_plot", plot_size);
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        dl->AddRectFilled(plot_pos, ImVec2(plot_pos.x + plot_size.x, plot_pos.y + plot_size.y), IM_COL32(248, 249, 246, 255));
                        dl->AddRect(plot_pos, ImVec2(plot_pos.x + plot_size.x, plot_pos.y + plot_size.y), IM_COL32(90, 96, 102, 160));
                        const float left_pad = 54.0f;
                        const float bottom_pad = 28.0f;
                        const float top_pad = 14.0f;
                        const float right_pad = 18.0f;
                        ImVec2 p0(plot_pos.x + left_pad, plot_pos.y + top_pad);
                        ImVec2 p1(plot_pos.x + plot_size.x - right_pad, plot_pos.y + plot_size.y - bottom_pad);
                        dl->AddLine(ImVec2(p0.x, p1.y), p1, IM_COL32(60, 64, 68, 180), 1.0f);
                        dl->AddLine(ImVec2(p0.x, p0.y), ImVec2(p0.x, p1.y), IM_COL32(60, 64, 68, 180), 1.0f);
                        struct PlotSeries { std::string label; ImU32 color; std::vector<std::pair<int, float>> points; };
                        std::vector<PlotSeries> series;
                        float global_max = 1.0f;
                        for (size_t di = 0; di < cube_view.datasets.size(); ++di) {
                            const auto& d = cube_view.datasets[di];
                            if (!d.recommended || di >= time_cube_selected.size() || !time_cube_selected[di]) continue;
                            PlotSeries s;
                            s.label = d.name;
                            s.color = ImGui::ColorConvertFloat4ToU32(colorFromStableKey(d.file));
                            float first = 0.0f;
                            float local_max = 0.0f;
                            for (const auto& yc : d.years) {
                                if (yc.first < time_cube_year_min || yc.first > time_cube_year_max) continue;
                                local_max = std::max(local_max, (float)yc.second);
                                if (first <= 0.0f && yc.second > 0) first = (float)yc.second;
                            }
                            for (const auto& yc : d.years) {
                                if (yc.first < time_cube_year_min || yc.first > time_cube_year_max) continue;
                                float v = (float)yc.second;
                                if (time_cube_normalize_mode == 1 && first > 0.0f) v = (v / first) * 100.0f;
                                if (time_cube_normalize_mode == 2 && local_max > 0.0f) v = (v / local_max) * 100.0f;
                                global_max = std::max(global_max, v);
                                s.points.push_back({yc.first, v});
                            }
                            if (!s.points.empty()) series.push_back(std::move(s));
                        }
                        if (series.empty()) {
                            dl->AddText(ImVec2(p0.x + 12.0f, p0.y + 12.0f), IM_COL32(120, 80, 50, 255), "Select one or more time-ready datasets.");
                        } else {
                            const int span = std::max(1, time_cube_year_max - time_cube_year_min);
                            for (int g = 0; g <= 4; ++g) {
                                float t = (float)g / 4.0f;
                                float y = p1.y - t * (p1.y - p0.y);
                                dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), IM_COL32(120, 126, 132, 45), 1.0f);
                            }
                            for (const auto& s : series) {
                                for (size_t pi = 1; pi < s.points.size(); ++pi) {
                                    auto project = [&](std::pair<int, float> pt) {
                                        float x = p0.x + ((float)(pt.first - time_cube_year_min) / (float)span) * (p1.x - p0.x);
                                        float y = p1.y - (pt.second / global_max) * (p1.y - p0.y);
                                        return ImVec2(x, y);
                                    };
                                    dl->AddLine(project(s.points[pi - 1]), project(s.points[pi]), s.color, 2.0f);
                                }
                            }
                        }
                        ImGui::SeparatorText("Legend");
                        int legend_count = 0;
                        for (size_t di = 0; di < cube_view.datasets.size() && legend_count < 12; ++di) {
                            const auto& d = cube_view.datasets[di];
                            if (!d.recommended || di >= time_cube_selected.size() || !time_cube_selected[di]) continue;
                            ImGui::ColorButton(("##legend" + d.file).c_str(), colorFromStableKey(d.file), ImGuiColorEditFlags_NoTooltip, ImVec2(10, 10));
                            ImGui::SameLine();
                            ImGui::Text("%s (%s)", d.name.c_str(), d.measure.c_str());
                            legend_count++;
                        }
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Datasets")) {
                        ImGui::Checkbox("Show excluded datasets", &time_cube_show_excluded);
                        ImGui::SameLine();
                        if (ImGui::Button("Select Time-Ready")) {
                            for (size_t i = 0; i < cube_view.datasets.size() && i < time_cube_selected.size(); ++i) time_cube_selected[i] = cube_view.datasets[i].recommended;
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Clear Selection")) {
                            std::fill(time_cube_selected.begin(), time_cube_selected.end(), false);
                        }
                        if (ImGui::BeginTable("time_cube_dataset_table", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0, 360))) {
                            ImGui::TableSetupColumn("On");
                            ImGui::TableSetupColumn("Dataset");
                            ImGui::TableSetupColumn("Mode");
                            ImGui::TableSetupColumn("Grain");
                            ImGui::TableSetupColumn("Records");
                            ImGui::TableSetupColumn("Years");
                            ImGui::TableSetupColumn("Measure");
                            ImGui::TableSetupColumn("State");
                            ImGui::TableHeadersRow();
                            for (size_t di = 0; di < cube_view.datasets.size(); ++di) {
                                const auto& d = cube_view.datasets[di];
                                if (!time_cube_show_excluded && !d.recommended) continue;
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                ImGui::BeginDisabled(!d.recommended);
                                bool selected = time_cube_selected[di];
                                if (ImGui::Checkbox(("##tcsel" + d.file).c_str(), &selected)) time_cube_selected[di] = selected;
                                ImGui::EndDisabled();
                                ImGui::TableSetColumnIndex(1);
                                ImGui::TextWrapped("%s", d.name.c_str());
                                ImGui::TextDisabled("%s", d.file.c_str());
                                ImGui::TableSetColumnIndex(2);
                                ImGui::TextUnformatted(d.time_mode.empty() ? "-" : d.time_mode.c_str());
                                ImGui::TableSetColumnIndex(3);
                                ImGui::TextUnformatted(d.grain.empty() ? "-" : d.grain.c_str());
                                ImGui::TableSetColumnIndex(4);
                                ImGui::Text("%zu / %zu", d.matched_records, d.feature_count);
                                ImGui::TableSetColumnIndex(5);
                                if (!d.years.empty()) ImGui::Text("%d-%d", d.years.front().first, d.years.back().first);
                                else ImGui::TextDisabled("-");
                                ImGui::TableSetColumnIndex(6);
                                ImGui::TextWrapped("%s", d.measure.empty() ? d.declared_date_field.c_str() : d.measure.c_str());
                                ImGui::TableSetColumnIndex(7);
                                if (d.recommended) ImGui::TextColored(ImVec4(0.20f, 0.62f, 0.25f, 1.0f), "Time-ready");
                                else ImGui::TextWrapped("%s", d.exclusion_reason.empty() ? "Excluded" : d.exclusion_reason.c_str());
                            }
                            ImGui::EndTable();
                        }
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Schema")) {
                        ImGui::TextDisabled("Authoritative rules: data/schemas/time_cube_datasets.json");
                        if (ImGui::BeginTable("time_cube_schema_table", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0, 360))) {
                            ImGui::TableSetupColumn("Dataset");
                            ImGui::TableSetupColumn("Mode");
                            ImGui::TableSetupColumn("Date Field");
                            ImGui::TableSetupColumn("Snapshot");
                            ImGui::TableSetupColumn("Grain");
                            ImGui::TableSetupColumn("Measure / Reason");
                            ImGui::TableHeadersRow();
                            for (const auto& d : cube_view.datasets) {
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0); ImGui::TextWrapped("%s", d.file.c_str());
                                ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(d.time_mode.empty() ? "-" : d.time_mode.c_str());
                                ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(d.declared_date_field.empty() ? "-" : d.declared_date_field.c_str());
                                ImGui::TableSetColumnIndex(3); if (d.snapshot_year >= 0) ImGui::Text("%d", d.snapshot_year); else ImGui::TextDisabled("-");
                                ImGui::TableSetColumnIndex(4); ImGui::TextUnformatted(d.grain.empty() ? "-" : d.grain.c_str());
                                ImGui::TableSetColumnIndex(5); ImGui::TextWrapped("%s", d.recommended ? d.measure.c_str() : d.exclusion_reason.c_str());
                            }
                            ImGui::EndTable();
                        }
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Graph Model")) {
            ImGui::TextUnformatted("Entity Graph");
            ImGui::Separator();
            ImGui::TextWrapped("Nodes: agency, program, vendor, parcel, tract, award.");
            ImGui::TextWrapped("Edges: funds, regulates, located_in, owns, serves.");
            ImGui::TextDisabled("Hierarchy source: data/government/government_hierarchy_and_pay_2026.json");
            ImGui::TextDisabled("Use for cross-system dependency mapping and path queries.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Star Schema")) {
            ImGui::TextUnformatted("Analytical Star Schema");
            ImGui::Separator();
            ImGui::TextWrapped("Fact tables: incidents, permits, vacancies, tax events, mortgage activity, public health prevalence.");
            ImGui::TextWrapped("Dimensions: time, geography, organization, category, status.");
            ImGui::TextDisabled("Recommended keys: blocklot_id, tract_fips, date_id, agency_id.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Spatial Index")) {
            ImGui::TextUnformatted("Spatial Index Artifacts");
            ImGui::Separator();
            ImGui::TextWrapped("Generate H3/S2 multiresolution tiling views for fast joins and heatmaps.");
            ImGui::Text("Loaded map layers: %zu", layers.size());
            ImGui::TextDisabled("Recommended resolutions: neighborhood, tract, parcel-adjacent.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Uncertainty")) {
            ImGui::TextUnformatted("Uncertainty-Aware Views");
            ImGui::Separator();
            ImGui::TextWrapped("Track confidence intervals, modeled-estimate flags, and data-quality scores per metric.");
            ImGui::TextDisabled("Critical for CDC PLACES and other modeled prevalence datasets.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Change Log")) {
            ImGui::TextUnformatted("Dataset Change Log");
            ImGui::Separator();
            ImGui::TextWrapped("Snapshot diffs by refresh date: added, removed, changed features and properties.");
            ImGui::TextDisabled("Use for reproducibility and auditability of map-derived decisions.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Policy Hierarchy")) {
            ImGui::TextUnformatted("Policy and Authority Hierarchy");
            ImGui::Separator();
            ImGui::TextWrapped("Data-backed view of Maryland, Baltimore, and salient federal authority structures with federal pay schedule references.");
            ImGui::TextDisabled("Source: data/government/government_hierarchy_and_pay_2026.json");
            if (!policy_hierarchy_loaded) {
                ImGui::TextColored(ImVec4(0.75f, 0.22f, 0.16f, 1.0f), "Could not load hierarchy: %s", policy_hierarchy_error.c_str());
                ImGui::EndTabItem();
            } else {
                ImGui::InputTextWithHint("##policy_hierarchy_query", "Search agency, position, section, branch, schedule...", policy_hierarchy_query, sizeof(policy_hierarchy_query));
                ImGui::SameLine();
                if (ImGui::Button("Clear")) policy_hierarchy_query[0] = '\0';
                const char* scopes[] = {"All", "Maryland", "Baltimore", "Federal"};
                ImGui::Combo("Scope", &policy_hierarchy_scope, scopes, IM_ARRAYSIZE(scopes));
                const std::string policy_query = trimDisplayValue(policy_hierarchy_query);
                auto item_matches = [&](const std::string& a, const std::string& b = {}, const std::string& c = {}, const std::string& d = {}) {
                    if (policy_query.empty()) return true;
                    return containsCaseInsensitive(a, policy_query) ||
                           containsCaseInsensitive(b, policy_query) ||
                           containsCaseInsensitive(c, policy_query) ||
                           containsCaseInsensitive(d, policy_query);
                };
                auto shorten_for_ui = [](const std::string& value, size_t max_chars = 72) {
                    if (value.size() <= max_chars || max_chars <= 3) return value;
                    return value.substr(0, max_chars - 3) + "...";
                };
                auto draw_clipped_text = [&](const std::string& value, size_t max_chars = 72) {
                    const std::string full = value.empty() ? std::string("-") : value;
                    const std::string shown = shorten_for_ui(full, max_chars);
                    ImGui::TextUnformatted(shown.c_str());
                    if (shown.size() != full.size() && ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 80.0f);
                        ImGui::TextUnformatted(full.c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::EndTooltip();
                    }
                };
                auto draw_source_chip = [&](const std::string& value) {
                    if (value.empty()) return;
                    ImGui::TextDisabled("%s", shorten_for_ui(value, 64).c_str());
                    if (ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 80.0f);
                        ImGui::TextUnformatted(value.c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::EndTooltip();
                    }
                };
                auto roster_row_matches_query = [&](const PublicServantRosterRow& row) {
                    if (policy_query.empty()) return true;
                    return containsCaseInsensitive(row.jurisdiction, policy_query) ||
                           containsCaseInsensitive(row.employer, policy_query) ||
                           containsCaseInsensitive(row.person_name, policy_query) ||
                           containsCaseInsensitive(row.agency, policy_query) ||
                           containsCaseInsensitive(row.role_title, policy_query) ||
                           containsCaseInsensitive(row.pay_grade, policy_query) ||
                           containsCaseInsensitive(row.annual_salary, policy_query) ||
                           containsCaseInsensitive(row.gross_pay, policy_query) ||
                           containsCaseInsensitive(row.fiscal_year, policy_query) ||
                           containsCaseInsensitive(row.source_id, policy_query);
                };
                auto parse_pay_amount = [](const std::string& value) {
                    std::string cleaned;
                    cleaned.reserve(value.size());
                    bool seen_digit = false;
                    for (char c : value) {
                        if ((c >= '0' && c <= '9') || c == '.' || c == '-') {
                            cleaned.push_back(c);
                            if (c >= '0' && c <= '9') seen_digit = true;
                        }
                    }
                    if (!seen_digit) return 0.0;
                    try { return std::stod(cleaned); } catch (...) { return 0.0; }
                };
                auto format_money_compact = [](double value) {
                    std::ostringstream os;
                    if (value >= 1000000000.0) os << "$" << std::fixed << std::setprecision(1) << (value / 1000000000.0) << "B";
                    else if (value >= 1000000.0) os << "$" << std::fixed << std::setprecision(1) << (value / 1000000.0) << "M";
                    else if (value >= 1000.0) os << "$" << std::fixed << std::setprecision(1) << (value / 1000.0) << "K";
                    else os << "$" << std::fixed << std::setprecision(0) << value;
                    return os.str();
                };
                auto sort_policy_viz = [&](auto&& self, PolicyVizNode& node) -> void {
                    for (auto& child : node.children) self(self, child);
                    std::sort(node.children.begin(), node.children.end(), [](const PolicyVizNode& a, const PolicyVizNode& b) {
                        if (a.value == b.value) return a.label < b.label;
                        return a.value > b.value;
                    });
                };
                auto count_policy_viz = [&](auto&& self, const PolicyVizNode& node) -> size_t {
                    size_t n = 1;
                    for (const auto& child : node.children) n += self(self, child);
                    return n;
                };
                auto rebuild_policy_viz = [&]() {
                    struct Agg {
                        std::string label;
                        std::string parent;
                        size_t personnel = 0;
                        double pay_total = 0.0;
                    };
                    std::unordered_map<std::string, Agg> aggs;
                    auto clean_label = [](const std::string& value, const char* fallback) {
                        std::string trimmed = trimDisplayValue(value);
                        return trimmed.empty() ? std::string(fallback) : trimmed;
                    };
                    auto add_agg = [&](const std::string& key, const std::string& parent, const std::string& label, double pay) {
                        Agg& agg = aggs[key];
                        if (agg.label.empty()) agg.label = label;
                        if (agg.parent.empty()) agg.parent = parent;
                        agg.personnel++;
                        agg.pay_total += pay;
                    };
                    for (const auto& row : public_servant_roster) {
                        if (policy_hierarchy_scope == 1 && row.jurisdiction != "Maryland") continue;
                        if (policy_hierarchy_scope == 2 && row.jurisdiction != "Baltimore City") continue;
                        if (policy_hierarchy_scope == 3 && row.jurisdiction != "Federal") continue;
                        if (!roster_row_matches_query(row)) continue;
                        const std::string jurisdiction = clean_label(row.jurisdiction, "Unknown Jurisdiction");
                        const std::string employer = clean_label(row.employer, "Unknown Employer");
                        const std::string agency = clean_label(row.agency, "Unknown Agency");
                        const std::string role = clean_label(row.role_title, "Unknown Role");
                        double pay = parse_pay_amount(row.annual_salary);
                        if (pay <= 0.0) pay = parse_pay_amount(row.gross_pay);
                        const std::string k_j = jurisdiction;
                        const std::string k_e = k_j + "\x1f" + employer;
                        const std::string k_a = k_e + "\x1f" + agency;
                        const std::string k_r = k_a + "\x1f" + role;
                        add_agg(k_j, "", jurisdiction, pay);
                        add_agg(k_e, k_j, employer, pay);
                        add_agg(k_a, k_e, agency, pay);
                        add_agg(k_r, k_a, role, pay);
                    }
                    std::unordered_map<std::string, std::vector<std::string>> children_by_parent;
                    for (const auto& kv : aggs) children_by_parent[kv.second.parent].push_back(kv.first);
                    auto build_node = [&](auto&& self, const std::string& key) -> PolicyVizNode {
                        const Agg& agg = aggs[key];
                        PolicyVizNode node;
                        node.label = agg.label;
                        node.personnel = agg.personnel;
                        node.pay_total = agg.pay_total;
                        node.value = (policy_viz_metric == 0) ? static_cast<double>(agg.personnel) : agg.pay_total;
                        for (const std::string& child_key : children_by_parent[key]) node.children.push_back(self(self, child_key));
                        return node;
                    };
                    policy_viz_root = {};
                    policy_viz_root.label = "Public Sector";
                    for (const std::string& key : children_by_parent[""]) policy_viz_root.children.push_back(build_node(build_node, key));
                    for (const auto& child : policy_viz_root.children) {
                        policy_viz_root.personnel += child.personnel;
                        policy_viz_root.pay_total += child.pay_total;
                    }
                    policy_viz_root.value = (policy_viz_metric == 0) ? static_cast<double>(policy_viz_root.personnel) : policy_viz_root.pay_total;
                    sort_policy_viz(sort_policy_viz, policy_viz_root);
                    policy_viz_node_count = count_policy_viz(count_policy_viz, policy_viz_root);
                    policy_viz_cached_query = policy_query;
                    policy_viz_cached_scope = policy_hierarchy_scope;
                    policy_viz_cached_metric = policy_viz_metric;
                    policy_viz_cache_rebuilds++;
                };
                auto ensure_policy_viz = [&]() {
                    if (policy_viz_cached_query != policy_query ||
                        policy_viz_cached_scope != policy_hierarchy_scope ||
                        policy_viz_cached_metric != policy_viz_metric) {
                        rebuild_policy_viz();
                    }
                };
                auto value_label = [&]() {
                    return policy_viz_metric == 0 ? std::string("personnel") : std::string("pay");
                };
                auto node_value_label = [&](const PolicyVizNode& node) {
                    if (policy_viz_metric == 0) return std::to_string(node.personnel) + " people";
                    return format_money_compact(node.pay_total);
                };
                auto draw_policy_tooltip = [&](const PolicyVizNode& node) {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted(node.label.c_str());
                    ImGui::Separator();
                    ImGui::Text("Personnel rows: %zu", node.personnel);
                    ImGui::Text("Parsed pay total: %s", format_money_compact(node.pay_total).c_str());
                    ImGui::Text("Children: %zu", node.children.size());
                    ImGui::EndTooltip();
                };
                auto draw_treemap_node = [&](auto&& self, ImDrawList* dl, const PolicyVizNode& node, ImVec2 minp, ImVec2 maxp, int depth) -> void {
                    if (maxp.x - minp.x < 2.0f || maxp.y - minp.y < 2.0f || node.value <= 0.0) return;
                    const ImU32 base = ImGui::ColorConvertFloat4ToU32(colorFromStableKey(node.label));
                    const float shade = std::clamp(0.92f - depth * 0.07f, 0.55f, 0.92f);
                    ImVec4 c = ImGui::ColorConvertU32ToFloat4(base);
                    c.x *= shade; c.y *= shade; c.z *= shade; c.w = 0.92f;
                    dl->AddRectFilled(minp, maxp, ImGui::ColorConvertFloat4ToU32(c));
                    dl->AddRect(minp, maxp, IM_COL32(255, 255, 255, 210));
                    const float w = maxp.x - minp.x;
                    const float h = maxp.y - minp.y;
                    if (w > 86.0f && h > 34.0f) {
                        const std::string label = shorten_for_ui(node.label, static_cast<size_t>(std::max(10.0f, w / 7.5f)));
                        dl->AddText(ImVec2(minp.x + 5.0f, minp.y + 4.0f), IM_COL32(24, 28, 32, 255), label.c_str());
                        if (h > 52.0f) {
                            const std::string v = node_value_label(node);
                            dl->AddText(ImVec2(minp.x + 5.0f, minp.y + 21.0f), IM_COL32(40, 45, 50, 230), v.c_str());
                        }
                    }
                    ImGui::SetCursorScreenPos(minp);
                    ImGui::PushID(&node);
                    ImGui::InvisibleButton("treemap_node", ImVec2(w, h));
                    if (ImGui::IsItemHovered()) draw_policy_tooltip(node);
                    ImGui::PopID();
                    if (node.children.empty() || depth >= 5 || w < 40.0f || h < 30.0f) return;
                    const float pad = 3.0f;
                    ImVec2 inner_min(minp.x + pad, minp.y + ((h > 58.0f) ? 38.0f : pad));
                    ImVec2 inner_max(maxp.x - pad, maxp.y - pad);
                    if (inner_max.x <= inner_min.x || inner_max.y <= inner_min.y) return;
                    const double total = std::max(0.000001, node.value);
                    float cursor = ((inner_max.x - inner_min.x) >= (inner_max.y - inner_min.y)) ? inner_min.x : inner_min.y;
                    const bool split_x = (inner_max.x - inner_min.x) >= (inner_max.y - inner_min.y);
                    size_t drawn = 0;
                    for (const auto& child : node.children) {
                        if (child.value <= 0.0) continue;
                        drawn++;
                        if (drawn > 80) break;
                        const float frac = static_cast<float>(child.value / total);
                        if (split_x) {
                            const float nx = (drawn == node.children.size()) ? inner_max.x : std::min(inner_max.x, cursor + (inner_max.x - inner_min.x) * frac);
                            self(self, dl, child, ImVec2(cursor, inner_min.y), ImVec2(nx, inner_max.y), depth + 1);
                            cursor = nx;
                        } else {
                            const float ny = (drawn == node.children.size()) ? inner_max.y : std::min(inner_max.y, cursor + (inner_max.y - inner_min.y) * frac);
                            self(self, dl, child, ImVec2(inner_min.x, cursor), ImVec2(inner_max.x, ny), depth + 1);
                            cursor = ny;
                        }
                        if ((split_x && cursor >= inner_max.x - 1.0f) || (!split_x && cursor >= inner_max.y - 1.0f)) break;
                    }
                };
                auto draw_arc_segment = [](ImDrawList* dl, ImVec2 center, float r0, float r1, float a0, float a1, ImU32 color) {
                    if (a1 <= a0 || r1 <= r0) return;
                    const int segments = std::clamp(static_cast<int>((a1 - a0) * r1 / 6.0f), 8, 96);
                    std::vector<ImVec2> pts;
                    pts.reserve(static_cast<size_t>(segments * 2 + 2));
                    for (int i = 0; i <= segments; ++i) {
                        const float t = a0 + (a1 - a0) * (static_cast<float>(i) / static_cast<float>(segments));
                        pts.push_back(ImVec2(center.x + std::cos(t) * r1, center.y + std::sin(t) * r1));
                    }
                    for (int i = segments; i >= 0; --i) {
                        const float t = a0 + (a1 - a0) * (static_cast<float>(i) / static_cast<float>(segments));
                        pts.push_back(ImVec2(center.x + std::cos(t) * r0, center.y + std::sin(t) * r0));
                    }
                    dl->AddConvexPolyFilled(pts.data(), static_cast<int>(pts.size()), color);
                    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), IM_COL32(255, 255, 255, 190), ImDrawFlags_Closed, 1.0f);
                };
                auto draw_sunburst_node = [&](auto&& self, ImDrawList* dl, const PolicyVizNode& node, ImVec2 center, float a0, float a1, float ring, float ring_w, int depth) -> void {
                    if (node.value <= 0.0 || a1 <= a0 || depth > 5) return;
                    const float r0 = ring + depth * ring_w;
                    const float r1 = r0 + ring_w - 2.0f;
                    const ImU32 color = ImGui::ColorConvertFloat4ToU32(colorFromStableKey(node.label));
                    draw_arc_segment(dl, center, r0, r1, a0, a1, color);
                    const float mid = (a0 + a1) * 0.5f;
                    const float mx = center.x + std::cos(mid) * ((r0 + r1) * 0.5f);
                    const float my = center.y + std::sin(mid) * ((r0 + r1) * 0.5f);
                    ImGui::SetCursorScreenPos(ImVec2(mx - 6.0f, my - 6.0f));
                    ImGui::PushID(&node);
                    ImGui::InvisibleButton("sunburst_node", ImVec2(12.0f, 12.0f));
                    if (ImGui::IsItemHovered()) draw_policy_tooltip(node);
                    ImGui::PopID();
                    if ((a1 - a0) > 0.22f && depth <= 1) {
                        const std::string label = shorten_for_ui(node.label, 18);
                        dl->AddText(ImVec2(mx + 4.0f, my - 7.0f), IM_COL32(25, 28, 32, 230), label.c_str());
                    }
                    if (node.children.empty()) return;
                    float cursor = a0;
                    const double total = std::max(0.000001, node.value);
                    size_t drawn = 0;
                    for (const auto& child : node.children) {
                        if (child.value <= 0.0) continue;
                        drawn++;
                        if (drawn > 96) break;
                        const float na = cursor + (a1 - a0) * static_cast<float>(child.value / total);
                        self(self, dl, child, center, cursor, na, ring, ring_w, depth + 1);
                        cursor = na;
                    }
                };
                auto count_section_items = [&](const char* jurisdiction) {
                    size_t total = 0;
                    const json& node = policy_hierarchy[jurisdiction];
                    if (!node.contains("sections") || !node["sections"].is_object()) return total;
                    for (auto it = node["sections"].begin(); it != node["sections"].end(); ++it) {
                        if (it.value().is_array()) total += it.value().size();
                    }
                    return total;
                };
                const size_t maryland_total = count_section_items("maryland");
                const size_t baltimore_total = count_section_items("baltimore");
                const size_t federal_positions = policy_hierarchy["federal"].value("salient_positions", json::array()).size();
                if (ImGui::BeginTabBar("policy_hierarchy_tabs")) {
                    if (ImGui::BeginTabItem("Overview")) {
                        ImGui::Text("Generated: %s", policy_hierarchy.value("generated_at", "unknown").c_str());
                        ImGui::Text("Maryland entries: %zu", maryland_total);
                        ImGui::Text("Baltimore entries: %zu", baltimore_total);
                        ImGui::Text("Federal salient positions: %zu", federal_positions);
                        ImGui::SeparatorText("Use");
                        ImGui::TextWrapped("Use Hierarchy to find agencies/offices, Federal Positions to inspect role-to-pay references, and Pay Schedule to see Executive Schedule levels.");
                        ImGui::TextWrapped("This is an authority/reference model, not proof of reporting lines. Treat links and pay references as source-backed metadata.");
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Treemap")) {
                        ensure_policy_viz();
                        ImGui::TextDisabled("Nested boxes: jurisdiction -> employer -> agency -> role. Area is proportional to selected metric.");
                        ImGui::RadioButton("Personnel count", &policy_viz_metric, 0);
                        ImGui::SameLine();
                        ImGui::RadioButton("Parsed pay total", &policy_viz_metric, 1);
                        ensure_policy_viz();
                        ImGui::SameLine();
                        ImGui::TextDisabled("Metric: %s | nodes: %zu | rebuilds: %zu", value_label().c_str(), policy_viz_node_count, policy_viz_cache_rebuilds);
                        const ImVec2 canvas_size(-1.0f, 520.0f);
                        ImGui::BeginChild("policy_treemap_canvas", canvas_size, true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        const ImVec2 p0 = ImGui::GetCursorScreenPos();
                        const ImVec2 avail = ImGui::GetContentRegionAvail();
                        const ImVec2 p1(p0.x + std::max(100.0f, avail.x), p0.y + std::max(100.0f, avail.y));
                        dl->AddRectFilled(p0, p1, IM_COL32(246, 248, 242, 255));
                        if (policy_viz_root.value <= 0.0 || policy_viz_root.children.empty()) {
                            dl->AddText(ImVec2(p0.x + 16.0f, p0.y + 16.0f), IM_COL32(120, 80, 50, 255), "No matching personnel/pay rows for this scope and search.");
                        } else {
                            draw_treemap_node(draw_treemap_node, dl, policy_viz_root, ImVec2(p0.x + 8.0f, p0.y + 8.0f), ImVec2(p1.x - 8.0f, p1.y - 8.0f), 0);
                        }
                        ImGui::SetCursorScreenPos(p1);
                        ImGui::EndChild();
                        ImGui::TextDisabled("Pay totals are parsed from annual salary first, then gross pay. Rows without numeric pay still count for personnel.");
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Sunburst")) {
                        ensure_policy_viz();
                        ImGui::TextDisabled("Radial hierarchy: center=root, outer rings descend through jurisdiction, employer, agency, and role.");
                        ImGui::RadioButton("Personnel count", &policy_viz_metric, 0);
                        ImGui::SameLine();
                        ImGui::RadioButton("Parsed pay total", &policy_viz_metric, 1);
                        ensure_policy_viz();
                        ImGui::SameLine();
                        ImGui::TextDisabled("Metric: %s | nodes: %zu | rebuilds: %zu", value_label().c_str(), policy_viz_node_count, policy_viz_cache_rebuilds);
                        ImGui::BeginChild("policy_sunburst_canvas", ImVec2(0, 560), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        const ImVec2 p0 = ImGui::GetCursorScreenPos();
                        const ImVec2 avail = ImGui::GetContentRegionAvail();
                        const ImVec2 p1(p0.x + std::max(100.0f, avail.x), p0.y + std::max(100.0f, avail.y));
                        dl->AddRectFilled(p0, p1, IM_COL32(247, 246, 240, 255));
                        const ImVec2 center((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
                        const float max_r = std::min(p1.x - p0.x, p1.y - p0.y) * 0.46f;
                        if (policy_viz_root.value <= 0.0 || policy_viz_root.children.empty()) {
                            dl->AddText(ImVec2(p0.x + 16.0f, p0.y + 16.0f), IM_COL32(120, 80, 50, 255), "No matching personnel/pay rows for this scope and search.");
                        } else {
                            dl->AddCircleFilled(center, max_r * 0.10f, IM_COL32(55, 65, 60, 255), 48);
                            const std::string root_label = node_value_label(policy_viz_root);
                            dl->AddText(ImVec2(center.x - 42.0f, center.y - 7.0f), IM_COL32(255, 255, 255, 245), root_label.c_str());
                            constexpr float kPi = 3.14159265358979323846f;
                            draw_sunburst_node(draw_sunburst_node, dl, policy_viz_root, center, -kPi * 0.5f, kPi * 1.5f, max_r * 0.12f, max_r * 0.17f, 0);
                        }
                        ImGui::SetCursorScreenPos(p1);
                        ImGui::EndChild();
                        ImGui::TextDisabled("Hover labels are currently anchored on segment centroids; use Treemap for denser inspection.");
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Hierarchy")) {
                        ImGui::TextDisabled("Shape: jurisdiction -> section/branch -> office or position. Search keeps matching branches visible.");
                        ImGui::BeginChild("policy_hierarchy_tree", ImVec2(0, 420), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
                        auto section_has_match = [&](const std::string& section, const json& rows, const std::string& jurisdiction) {
                            if (policy_query.empty()) return true;
                            if (item_matches(section, jurisdiction)) return true;
                            if (!rows.is_array()) return false;
                            for (const auto& row : rows) {
                                if (item_matches(row.value("name", ""), row.value("url", ""), section, jurisdiction)) return true;
                            }
                            return false;
                        };
                        auto draw_jurisdiction_tree = [&](const char* key, const char* label) {
                            if ((policy_hierarchy_scope == 1 && std::string(key) != "maryland") ||
                                (policy_hierarchy_scope == 2 && std::string(key) != "baltimore") ||
                                policy_hierarchy_scope == 3) return;
                            const json& node = policy_hierarchy[key];
                            if (!node.contains("sections") || !node["sections"].is_object()) return;
                            size_t visible_sections = 0;
                            for (auto sec = node["sections"].begin(); sec != node["sections"].end(); ++sec) {
                                if (section_has_match(sec.key(), sec.value(), label)) visible_sections++;
                            }
                            if (visible_sections == 0 && !policy_query.empty()) return;
                            if (!policy_query.empty()) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                            if (ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen)) {
                                ImGui::TextDisabled("Source:");
                                ImGui::SameLine();
                                draw_source_chip(node.value("source", ""));
                                for (auto sec = node["sections"].begin(); sec != node["sections"].end(); ++sec) {
                                    if (!sec.value().is_array()) continue;
                                    if (!section_has_match(sec.key(), sec.value(), label)) continue;
                                    std::string section_label = sec.key() + " (" + std::to_string(sec.value().size()) + ")";
                                    if (!policy_query.empty()) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                                    if (ImGui::TreeNode(section_label.c_str())) {
                                        for (const auto& row : sec.value()) {
                                            const std::string name = row.value("name", "");
                                            const std::string url = row.value("url", "");
                                            if (!item_matches(sec.key(), name, label, url)) continue;
                                            ImGui::BulletText("%s", name.c_str());
                                            if (!url.empty()) {
                                                ImGui::SameLine();
                                                draw_source_chip(url);
                                            }
                                        }
                                        ImGui::TreePop();
                                    }
                                }
                                ImGui::TreePop();
                            }
                        };
                        auto draw_federal_tree = [&]() {
                            if (policy_hierarchy_scope == 1 || policy_hierarchy_scope == 2) return;
                            const json rows = policy_hierarchy["federal"].value("salient_positions", json::array());
                            std::unordered_map<std::string, std::vector<json>> by_branch;
                            for (const auto& row : rows) {
                                const std::string pos = row.value("position", "");
                                const std::string branch = row.value("branch", "Federal");
                                const std::string schedule = row.value("pay_schedule", "");
                                const std::string ref = row.value("pay_reference", "");
                                if (!item_matches(pos, branch, schedule, ref)) continue;
                                by_branch[branch].push_back(row);
                            }
                            if (by_branch.empty() && !policy_query.empty()) return;
                            if (!policy_query.empty()) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                            if (ImGui::TreeNodeEx("Federal", ImGuiTreeNodeFlags_DefaultOpen)) {
                                for (auto& kv : by_branch) {
                                    std::string branch_label = kv.first + " (" + std::to_string(kv.second.size()) + ")";
                                    if (!policy_query.empty()) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                                    if (ImGui::TreeNode(branch_label.c_str())) {
                                        for (const auto& row : kv.second) {
                                            ImGui::BulletText("%s", row.value("position", "").c_str());
                                            ImGui::Indent();
                                            ImGui::TextDisabled("Pay: %s", row.value("pay_schedule", "").c_str());
                                            ImGui::TextDisabled("Reference:");
                                            ImGui::SameLine();
                                            draw_source_chip(row.value("pay_reference", ""));
                                            ImGui::Unindent();
                                        }
                                        ImGui::TreePop();
                                    }
                                }
                                ImGui::TreePop();
                            }
                        };
                        draw_jurisdiction_tree("maryland", "Maryland");
                        draw_jurisdiction_tree("baltimore", "Baltimore");
                        draw_federal_tree();
                        ImGui::EndChild();
                        ImGui::SeparatorText("Search Results Table");
                        auto draw_jurisdiction = [&](const char* key, const char* label) {
                            if ((policy_hierarchy_scope == 1 && std::string(key) != "maryland") ||
                                (policy_hierarchy_scope == 2 && std::string(key) != "baltimore") ||
                                policy_hierarchy_scope == 3) return;
                            const json& node = policy_hierarchy[key];
                            if (!node.contains("sections") || !node["sections"].is_object()) return;
                            if (!ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen)) return;
                            ImGui::TextDisabled("Source:");
                            ImGui::SameLine();
                            draw_source_chip(node.value("source", ""));
                            if (ImGui::BeginTable((std::string("policy_table_") + key).c_str(), 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0, 260))) {
                                ImGui::TableSetupColumn("Section");
                                ImGui::TableSetupColumn("Name");
                                ImGui::TableSetupColumn("Jurisdiction");
                                ImGui::TableSetupColumn("URL");
                                ImGui::TableHeadersRow();
                                for (auto sec = node["sections"].begin(); sec != node["sections"].end(); ++sec) {
                                    if (!sec.value().is_array()) continue;
                                    for (const auto& row : sec.value()) {
                                        const std::string name = row.value("name", "");
                                        const std::string url = row.value("url", "");
                                        if (!item_matches(sec.key(), name, label, url)) continue;
                                        ImGui::TableNextRow();
                                        ImGui::TableSetColumnIndex(0); draw_clipped_text(sec.key(), 48);
                                        ImGui::TableSetColumnIndex(1); draw_clipped_text(name, 72);
                                        ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(label);
                                        ImGui::TableSetColumnIndex(3); draw_clipped_text(url, 80);
                                    }
                                }
                                ImGui::EndTable();
                            }
                        };
                        draw_jurisdiction("maryland", "Maryland");
                        draw_jurisdiction("baltimore", "Baltimore");
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Federal Positions")) {
                        if (policy_hierarchy_scope == 1 || policy_hierarchy_scope == 2) {
                            ImGui::TextDisabled("Federal rows hidden by current scope.");
                        } else if (ImGui::BeginTable("federal_positions_table", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0, 420))) {
                            ImGui::TableSetupColumn("Position");
                            ImGui::TableSetupColumn("Branch");
                            ImGui::TableSetupColumn("Pay Schedule");
                            ImGui::TableSetupColumn("Reference");
                            ImGui::TableHeadersRow();
                            const json rows = policy_hierarchy["federal"].value("salient_positions", json::array());
                            for (const auto& row : rows) {
                                const std::string pos = row.value("position", "");
                                const std::string branch = row.value("branch", "");
                                const std::string schedule = row.value("pay_schedule", "");
                                const std::string ref = row.value("pay_reference", "");
                                if (!item_matches(pos, branch, schedule, ref)) continue;
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0); draw_clipped_text(pos, 72);
                                ImGui::TableSetColumnIndex(1); draw_clipped_text(branch, 48);
                                ImGui::TableSetColumnIndex(2); draw_clipped_text(schedule, 48);
                                ImGui::TableSetColumnIndex(3); draw_clipped_text(ref, 80);
                            }
                            ImGui::EndTable();
                        }
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("People & Pay")) {
                        ImGui::TextWrapped("Fetched public-sector roster. Search is capped for UI responsiveness; empty search shows the first matching rows by scope.");
                        ImGui::Text("Loaded roster rows: %zu", public_servant_roster.size());
                        ImGui::TextDisabled("Normalized files: data/public_servants/normalized_public_servants.jsonl and .csv");
                        constexpr size_t kPeoplePayDisplayCap = 5000;
                        if (people_pay_cached_query != policy_query || people_pay_cached_scope != policy_hierarchy_scope) {
                            people_pay_cached_query = policy_query;
                            people_pay_cached_scope = policy_hierarchy_scope;
                            people_pay_cache_matched_count = 0;
                            people_pay_visible_rows.clear();
                            people_pay_visible_rows.reserve(std::min(public_servant_roster.size(), kPeoplePayDisplayCap));
                            for (size_t row_index = 0; row_index < public_servant_roster.size(); ++row_index) {
                                const auto& row = public_servant_roster[row_index];
                                if (policy_hierarchy_scope == 1 && row.jurisdiction != "Maryland") continue;
                                if (policy_hierarchy_scope == 2 && row.jurisdiction != "Baltimore City") continue;
                                if (policy_hierarchy_scope == 3 && row.jurisdiction != "Federal") continue;
                                if (!roster_row_matches_query(row)) continue;
                                people_pay_cache_matched_count++;
                                if (people_pay_visible_rows.size() < kPeoplePayDisplayCap) people_pay_visible_rows.push_back(row_index);
                            }
                            people_pay_cache_rebuilds++;
                        }
                        people_pay_rendered_rows_last = 0;
                        if (ImGui::BeginTable("people_pay_table", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0, 440))) {
                            ImGui::TableSetupColumn("Jurisdiction");
                            ImGui::TableSetupColumn("Person");
                            ImGui::TableSetupColumn("Employer");
                            ImGui::TableSetupColumn("Agency");
                            ImGui::TableSetupColumn("Role");
                            ImGui::TableSetupColumn("Pay Grade");
                            ImGui::TableSetupColumn("Annual / Gross");
                            ImGui::TableSetupColumn("FY / Source");
                            ImGui::TableHeadersRow();
                            ImGuiListClipper clipper;
                            clipper.Begin(static_cast<int>(people_pay_visible_rows.size()));
                            while (clipper.Step()) {
                                for (int display_index = clipper.DisplayStart; display_index < clipper.DisplayEnd; ++display_index) {
                                    const auto& row = public_servant_roster[people_pay_visible_rows[static_cast<size_t>(display_index)]];
                                    const std::string person = row.person_name.empty() ? "(not in source)" : row.person_name;
                                    const std::string grade = row.pay_grade.empty() ? "-" : row.pay_grade;
                                    const std::string salary = "Annual: " + (row.annual_salary.empty() ? std::string("-") : row.annual_salary) +
                                                               " | Gross: " + (row.gross_pay.empty() ? std::string("-") : row.gross_pay);
                                    const std::string source = row.fiscal_year + " | " + row.source_id;
                                    people_pay_rendered_rows_last++;
                                    ImGui::TableNextRow();
                                    ImGui::TableSetColumnIndex(0); draw_clipped_text(row.jurisdiction, 32);
                                    ImGui::TableSetColumnIndex(1); draw_clipped_text(person, 48);
                                    ImGui::TableSetColumnIndex(2); draw_clipped_text(row.employer, 56);
                                    ImGui::TableSetColumnIndex(3); draw_clipped_text(row.agency, 56);
                                    ImGui::TableSetColumnIndex(4); draw_clipped_text(row.role_title, 72);
                                    ImGui::TableSetColumnIndex(5); draw_clipped_text(grade, 32);
                                    ImGui::TableSetColumnIndex(6); draw_clipped_text(salary, 64);
                                    ImGui::TableSetColumnIndex(7); draw_clipped_text(source, 72);
                                }
                            }
                            ImGui::EndTable();
                        }
                        ImGui::TextDisabled("Matched rows: %zu | Cached rows: %zu | Rendered this frame: %zu | Cap: %zu | Cache rebuilds: %zu",
                                            people_pay_cache_matched_count, people_pay_visible_rows.size(), people_pay_rendered_rows_last,
                                            kPeoplePayDisplayCap, people_pay_cache_rebuilds);
                        ImGui::TextDisabled("Official bulk named salary source currently fetched: Baltimore City. Maryland state/university official pages fetched are salary schedules/structures, not bulk named rosters.");
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Pay Schedule")) {
                        const json pay = policy_hierarchy["federal"].value("pay_schedules", json::object());
                        const json exec = pay.value("executive_schedule", json::object());
                        ImGui::Text("Executive Schedule Year: %d", exec.value("year", 0));
                        ImGui::TextWrapped("Note: %s", exec.value("note", "").c_str());
                        ImGui::TextWrapped("Source: %s", exec.value("source", "").c_str());
                        if (ImGui::BeginTable("executive_schedule_table", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable)) {
                            ImGui::TableSetupColumn("Level");
                            ImGui::TableSetupColumn("Annual Pay");
                            ImGui::TableHeadersRow();
                            const json levels = exec.value("levels", json::object());
                            for (auto it = levels.begin(); it != levels.end(); ++it) {
                                const std::string level = it.key();
                                const std::string amount = it.value().is_number() ? std::to_string(it.value().get<int>()) : it.value().dump();
                                if (!item_matches(level, amount)) continue;
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(level.c_str());
                                ImGui::TableSetColumnIndex(1); ImGui::Text("$%s", amount.c_str());
                            }
                            ImGui::EndTable();
                        }
                        const json gs = pay.value("general_schedule", json::object());
                        ImGui::SeparatorText("General Schedule");
                        ImGui::Text("Year: %d | XML table count: %zu", gs.value("year", 0), gs.value("xml_tables", json::array()).size());
                        ImGui::TextWrapped("Note: %s", gs.value("note", "").c_str());
                        ImGui::TextWrapped("Source: %s", gs.value("source", "").c_str());
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Sources")) {
                        ImGui::TextWrapped("Maryland: %s", policy_hierarchy["maryland"].value("source", "").c_str());
                        ImGui::TextWrapped("Baltimore: %s", policy_hierarchy["baltimore"].value("source", "").c_str());
                        const json sources = policy_hierarchy["federal"].value("sources", json::array());
                        for (const auto& s : sources) {
                            if (s.is_string()) ImGui::TextWrapped("Federal: %s", s.get<std::string>().c_str());
                            else ImGui::TextWrapped("Federal: %s", s.dump().c_str());
                        }
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Risk Scorecards")) {
            ImGui::TextUnformatted("Risk / Composite Scorecards");
            ImGui::Separator();
            ImGui::TextWrapped("Build transparent weighted indicators by tract/CSA with drill-down to source metrics.");
            ImGui::TextDisabled("Publish weights and normalization method per scorecard version.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Causal Panel")) {
            ImGui::TextUnformatted("Causal-Ready Panel");
            ImGui::Separator();
            ImGui::TextWrapped("Longitudinal panel by tract with lagged covariates for intervention evaluation.");
            ImGui::TextDisabled("Recommended fields: treatment flag, pre/post windows, controls, lag features.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Scenarios")) {
            ImGui::TextUnformatted("Scenario Model");
            ImGui::Separator();
            ImGui::TextWrapped("Store baseline plus intervention assumptions and projected outcomes.");
            ImGui::TextDisabled("Use multiple scenario branches for policy alternatives and sensitivity checks.");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
        }

        ImGui::End();

        ImGui::Render();
        ImDrawData* draw_data = ImGui::GetDrawData();
        if (draw_data->DisplaySize.x > 0.0f && draw_data->DisplaySize.y > 0.0f) {
            wd->ClearValue.color.float32[0] = 0.95f;
            wd->ClearValue.color.float32[1] = 0.95f;
            wd->ClearValue.color.float32[2] = 0.96f;
            wd->ClearValue.color.float32[3] = 1.00f;
            const auto present_prof_begin = std::chrono::steady_clock::now();
            FrameRender(wd, draw_data);
            FramePresent(wd);
            prof_present_ms_last.store(prof_ms_since(present_prof_begin), std::memory_order_relaxed);
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
        prof_ui_ms_last.store(prof_ms_since(prof_frame_begin), std::memory_order_relaxed);
        prof_tiles_drawn_last.store(prof_tiles_drawn_frame, std::memory_order_relaxed);
        prof_features_considered_last.store(prof_features_considered_frame, std::memory_order_relaxed);
        prof_features_drawn_last.store(prof_features_drawn_frame, std::memory_order_relaxed);
        prof_retired_textures.store(g_RetiredTextures.size(), std::memory_order_relaxed);
        prof_tile_cache_size.store(g_TileCache.size(), std::memory_order_relaxed);
        {
            ProfileFrameSample sample;
            sample.frame_ms = frame_ms;
            sample.ui_total_ms = prof_ui_ms_last.load(std::memory_order_relaxed);
            sample.owner_aggregate_ms = prof_owner_ms_last.load(std::memory_order_relaxed);
            sample.tiles_ms = prof_tile_ms_last.load(std::memory_order_relaxed);
            sample.layers_ms = prof_layer_ms_last.load(std::memory_order_relaxed);
            sample.heatmap_ms = prof_heatmap_ms_last.load(std::memory_order_relaxed);
            sample.overlays_ms = prof_overlay_ms_last.load(std::memory_order_relaxed);
            sample.render_present_ms = prof_present_ms_last.load(std::memory_order_relaxed);
            sample.tiles_drawn = prof_tiles_drawn_frame;
            sample.features_considered = prof_features_considered_frame;
            sample.features_drawn_points = prof_features_drawn_frame;
            sample.heat_samples = prof_heat_samples_last.load(std::memory_order_relaxed);
            sample.retired_textures = g_RetiredTextures.size();
            std::lock_guard<std::mutex> lk(profile_mutex);
            profile_samples[profile_sample_pos] = sample;
            profile_sample_pos = (profile_sample_pos + 1) % profile_samples.size();
            profile_sample_count = std::min(profile_sample_count + 1, profile_samples.size());
        }
    }

    vkDeviceWaitIdle(g_Device);
    saveLayerUiState(
        root,
        layers,
        hover_inspector_enabled,
        &zoning_zone_enabled,
        &layer_fill_enabled,
        &layer_hover_enabled,
        &layer_inspect_enabled,
        &layer_heatmap_enabled,
        &layer_heatmap_max_zoom,
        &layer_heatmap_use_gradient,
        &layer_heatmap_algo,
        &layer_heatmap_use_global_settings,
        &layer_heatmap_cell_px,
        &layer_heatmap_bandwidth_px,
        &layer_heatmap_blur_sigma_px,
        &layer_heatmap_percentile_clip,
        &layer_heatmap_zoom_adaptive_bandwidth,
        &layer_heatmap_multires_enabled,
        &layer_heatmap_multires_blend,
        &heatmap_algo,
        &heatmap_quality_preset,
        &global_heat_cell_px,
        &heatmap_bandwidth_px,
        &heatmap_blur_sigma_px,
        &heatmap_percentile_clip,
        &heatmap_zoom_adaptive_bandwidth,
        &heatmap_multires_enabled,
        &heatmap_multires_blend);
    app_settings.vulkan_validation_enabled = g_EnableValidationLayers;
    saveAppSettings(root, app_settings);
    hydration_stop.store(true, std::memory_order_relaxed);
    if (time_cube_ui_worker.joinable()) time_cube_ui_worker.join();
    hydrate_req_cv.notify_all();
    tri_cv.notify_all();
    for (auto& t : hydration_workers) if (t.joinable()) t.join();
    if (triangulation_worker.joinable()) triangulation_worker.join();
    if (status_api_worker.joinable()) status_api_worker.join();
    if (dataset_api_worker.joinable()) dataset_api_worker.join();
    if (lan_discovery_worker.joinable()) lan_discovery_worker.join();
    destroyTileTextureNow(heatmap_raster_texture);
    drainRetiredTextures(true);
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
