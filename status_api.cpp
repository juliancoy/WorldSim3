#include "status_api.h"

#include "app_utils.h"
#include "memory_utils.h"
#include "net_http_utils.h"
#include "thread_utils.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#if !defined(_WIN32)
#include <malloc.h>
#include <sys/resource.h>
#else
#include <process.h>
#endif

#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
struct ResourceUsageSnapshot {
    double user_cpu_seconds = 0.0;
    double system_cpu_seconds = 0.0;
    long long max_rss_kb = 0;
    long long voluntary_context_switches = 0;
    long long involuntary_context_switches = 0;
};

struct ProcMemorySnapshot {
    size_t vm_size_kb = 0;
    size_t vm_rss_kb = 0;
    size_t vm_hwm_kb = 0;
    size_t vm_data_kb = 0;
    size_t vm_swap_kb = 0;
    size_t threads = 0;
    size_t smaps_rss_kb = 0;
    size_t smaps_pss_kb = 0;
    size_t private_dirty_kb = 0;
    size_t anonymous_kb = 0;
    size_t swap_kb = 0;
};

ResourceUsageSnapshot readResourceUsage() {
    ResourceUsageSnapshot out;
#if !defined(_WIN32)
    struct rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    auto tv_sec_d = [](const timeval& tv) {
        return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
    };
    out.user_cpu_seconds = tv_sec_d(ru.ru_utime);
    out.system_cpu_seconds = tv_sec_d(ru.ru_stime);
    out.max_rss_kb = (long long)ru.ru_maxrss;
    out.voluntary_context_switches = (long long)ru.ru_nvcsw;
    out.involuntary_context_switches = (long long)ru.ru_nivcsw;
#endif
    return out;
}

ProcMemorySnapshot readProcMemory() {
    ProcMemorySnapshot out;
#if !defined(_WIN32)
    {
        std::ifstream status_in("/proc/self/status");
        std::string key;
        while (status_in >> key) {
            if (key == "VmSize:") status_in >> out.vm_size_kb;
            else if (key == "VmRSS:") status_in >> out.vm_rss_kb;
            else if (key == "VmHWM:") status_in >> out.vm_hwm_kb;
            else if (key == "VmData:") status_in >> out.vm_data_kb;
            else if (key == "VmSwap:") status_in >> out.vm_swap_kb;
            else if (key == "Threads:") status_in >> out.threads;
            std::string rest;
            std::getline(status_in, rest);
        }
    }
    {
        std::ifstream smaps_in("/proc/self/smaps_rollup");
        std::string key;
        while (smaps_in >> key) {
            if (key == "Rss:") smaps_in >> out.smaps_rss_kb;
            else if (key == "Pss:") smaps_in >> out.smaps_pss_kb;
            else if (key == "Private_Dirty:") smaps_in >> out.private_dirty_kb;
            else if (key == "Anonymous:") smaps_in >> out.anonymous_kb;
            else if (key == "Swap:") smaps_in >> out.swap_kb;
            std::string rest;
            std::getline(smaps_in, rest);
        }
    }
#endif
    return out;
}

std::string readTextFileIfExists(const std::string& path) {
    std::ifstream in(path);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int currentProcessId() {
#if defined(_WIN32)
    return _getpid();
#else
    return getpid();
#endif
}
}

std::thread startStatusApiWorker(StatusApiContext ctx) {
    return std::thread([ctx]() mutable {
        setCurrentThreadName("ws3-status-api");

        auto& hydration_stop = *ctx.stop;
        auto& layers = *ctx.layers;
        auto& time_cube_service = *ctx.time_cube_service;
        auto& g_ScreenshotState = *ctx.screenshot;
        auto& status_mutex = *ctx.status_mutex;
        auto& layer_states = *ctx.layer_states;
        auto& layer_fill_mutex = *ctx.layer_fill_mutex;
        auto& layer_fill_enabled = *ctx.layer_fill_enabled;
        auto& layer_hover_enabled = *ctx.layer_hover_enabled;
        auto& layer_inspect_enabled = *ctx.layer_inspect_enabled;
        auto& layer_heatmap_enabled = *ctx.layer_heatmap_enabled;
        auto& hydration_started_at = *ctx.hydration_started_at;
        auto& hydrated_count = *ctx.hydrated_count;
        auto& triangulated_count = *ctx.triangulated_count;
        auto& prof_tile_cache_size = *ctx.prof_tile_cache_size;
        auto& current_zoom_state = *ctx.current_zoom_state;
        auto& current_lon_state = *ctx.current_lon_state;
        auto& current_lat_state = *ctx.current_lat_state;
        auto& visible_vacant_parcels_last_frame = *ctx.visible_vacant_parcels_last_frame;
        auto& vacant_parcels_matched_total = *ctx.vacant_parcels_matched_total;
        auto& vacant_parcels_with_geometry_total = *ctx.vacant_parcels_with_geometry_total;
        auto& vacant_parcels_triangulated_renderable_total = *ctx.vacant_parcels_triangulated_renderable_total;
        auto& perf_frame_ms_avg = *ctx.perf_frame_ms_avg;
        auto& perf_frame_ms_last = *ctx.perf_frame_ms_last;
        auto& perf_fps_avg = *ctx.perf_fps_avg;
        auto& ui_left_panel_frac = *ctx.ui_left_panel_frac;
        auto& ui_right_panel_frac = *ctx.ui_right_panel_frac;
        auto& render_fill_attempts_last_frame = *ctx.render_fill_attempts_last_frame;
        auto& render_fill_success_last_frame = *ctx.render_fill_success_last_frame;
        auto& render_fill_no_triangles_last_frame = *ctx.render_fill_no_triangles_last_frame;
        auto& render_fill_bad_indices_last_frame = *ctx.render_fill_bad_indices_last_frame;
        auto& api_layer_mutex = *ctx.api_layer_mutex;
        auto& api_layer_enable_cmds = *ctx.api_layer_enable_cmds;
        auto& api_layer_fill_cmds = *ctx.api_layer_fill_cmds;
        auto& api_layer_download_cmds = *ctx.api_layer_download_cmds;
        auto& api_zoom_cmd = *ctx.api_zoom_cmd;
        auto& api_lon_cmd = *ctx.api_lon_cmd;
        auto& api_lat_cmd = *ctx.api_lat_cmd;
        auto& api_ui_cmd_seq = *ctx.api_ui_cmd_seq;
        auto& api_ui_cmd_kind = *ctx.api_ui_cmd_kind;
        auto& api_ui_cmd_x = *ctx.api_ui_cmd_x;
        auto& api_ui_cmd_y = *ctx.api_ui_cmd_y;
        auto& api_ui_cmd_button = *ctx.api_ui_cmd_button;
        auto& api_ui_cmd_scroll_y = *ctx.api_ui_cmd_scroll_y;
        auto& layer_profile_mutex = *ctx.layer_profile_mutex;
        auto& layer_profile_snapshot = *ctx.layer_profile_snapshot;
        auto& profile_mutex = *ctx.profile_mutex;
        auto& profile_samples = *ctx.profile_samples;
        auto& profile_sample_pos = *ctx.profile_sample_pos;
        auto& profile_sample_count = *ctx.profile_sample_count;
        auto& profile_reset_generation = *ctx.profile_reset_generation;
        auto& prof_ui_ms_last = *ctx.prof_ui_ms_last;
        auto& prof_owner_ms_last = *ctx.prof_owner_ms_last;
        auto& prof_tile_ms_last = *ctx.prof_tile_ms_last;
        auto& prof_layer_ms_last = *ctx.prof_layer_ms_last;
        auto& prof_heatmap_ms_last = *ctx.prof_heatmap_ms_last;
        auto& prof_overlay_ms_last = *ctx.prof_overlay_ms_last;
        auto& prof_present_ms_last = *ctx.prof_present_ms_last;
        auto& prof_tiles_drawn_last = *ctx.prof_tiles_drawn_last;
        auto& prof_features_considered_last = *ctx.prof_features_considered_last;
        auto& prof_features_drawn_last = *ctx.prof_features_drawn_last;
        auto& prof_heat_samples_last = *ctx.prof_heat_samples_last;
        auto& prof_retired_textures = *ctx.prof_retired_textures;
        const char* kAppVersion = ctx.app_version;
        const int kProtocolVersion = ctx.protocol_version;
        const size_t kMaxTileCache = ctx.tile_cache_max;

        if (!initNetworkSockets()) return;
        NetSocket server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd == kInvalidNetSocket) return;
        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

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
            int sel = select(static_cast<int>(server_fd + 1), &readfds, nullptr, nullptr, &tv);
            if (sel <= 0) continue;
            NetSocket client_fd = accept(server_fd, nullptr, nullptr);
            if (client_fd == kInvalidNetSocket) continue;

            char buf[1024];
            NetSSize n = netRead(client_fd, buf, sizeof(buf) - 1);
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
            auto url_decode = [](const std::string& in) -> std::string {
                std::string out;
                out.reserve(in.size());
                for (size_t i = 0; i < in.size(); ++i) {
                    const char ch = in[i];
                    if (ch == '+') {
                        out.push_back(' ');
                        continue;
                    }
                    if (ch == '%' && i + 2 < in.size()) {
                        auto hex = [](char c) -> int {
                            if (c >= '0' && c <= '9') return c - '0';
                            if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
                            if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
                            return -1;
                        };
                        const int hi = hex(in[i + 1]);
                        const int lo = hex(in[i + 2]);
                        if (hi >= 0 && lo >= 0) {
                            out.push_back((char)((hi << 4) | lo));
                            i += 2;
                            continue;
                        }
                    }
                    out.push_back(ch);
                }
                return out;
            };
            auto get_q = [&](const std::string& key) -> std::string {
                size_t pos = 0;
                while (pos < query.size()) {
                    size_t amp = query.find('&', pos);
                    std::string kv = query.substr(pos, (amp == std::string::npos ? query.size() : amp) - pos);
                    size_t eq = kv.find('=');
                    std::string k = eq == std::string::npos ? kv : kv.substr(0, eq);
                    k = url_decode(k);
                    if (k == key) {
                        if (eq == std::string::npos) return {};
                        return url_decode(kv.substr(eq + 1));
                    }
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
            auto build_memory_profile = [&]() {
                constexpr size_t kImVec2Bytes = sizeof(ImVec2);
                constexpr size_t kRingVectorBytes = sizeof(std::vector<ImVec2>);
                constexpr size_t kFeatureGeomBytes = sizeof(LayerDef::FeatureGeom);
                constexpr size_t kPropertyPairBytes = sizeof(std::pair<std::string, std::string>);
                constexpr size_t kTriangleIndexBytes = sizeof(uint32_t);

                auto lower_bound_bytes = [&](const LayerProfileSnapshot& layer) -> size_t {
                    size_t bytes = 0;
                    bytes += layer.features * kFeatureGeomBytes;
                    bytes += layer.rings * kRingVectorBytes;
                    bytes += layer.ring_points * kImVec2Bytes;
                    bytes += layer.triangle_indices * kTriangleIndexBytes;
                    bytes += layer.properties * kPropertyPairBytes;
                    bytes += layer.spatial_index_cells * sizeof(std::vector<uint32_t>);
                    bytes += layer.spatial_index_marks * sizeof(uint32_t);
                    return bytes;
                };

                json layer_list = json::array();
                size_t total_lower_bound_bytes = 0;
                size_t total_features = 0;
                size_t total_rings = 0;
                size_t total_points = 0;
                size_t total_tri_indices = 0;
                size_t total_properties = 0;
                {
                    std::lock_guard<std::mutex> profile_lk(layer_profile_mutex);
                    for (const auto& layer : layer_profile_snapshot) {
                        const size_t bytes = lower_bound_bytes(layer);
                        total_lower_bound_bytes += bytes;
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
                            {"spatial_index_cells", layer.spatial_index_cells},
                            {"spatial_index_marks", layer.spatial_index_marks},
                            {"estimated_lower_bound_bytes", bytes},
                            {"estimated_lower_bound_mb", (double)bytes / (1024.0 * 1024.0)}
                        });
                    }
                }

                const ProcMemorySnapshot mem = readProcMemory();
                const ResourceUsageSnapshot ru = readResourceUsage();
                json out;
                out["ok"] = true;
                out["note"] = "Layer estimates are lower bounds: string character allocations, vector capacity slack, allocator arenas, JSON/cache transients, and GPU allocations are not fully attributable here.";
                out["process"] = {
                    {"pid", currentProcessId()},
                    {"vm_size_kb", mem.vm_size_kb},
                    {"vm_rss_kb", mem.vm_rss_kb},
                    {"vm_hwm_kb", mem.vm_hwm_kb},
                    {"vm_data_kb", mem.vm_data_kb},
                    {"vm_swap_kb", mem.vm_swap_kb},
                    {"threads", mem.threads},
                    {"max_rss_kb", ru.max_rss_kb}
                };
                out["smaps_rollup"] = {
                    {"rss_kb", mem.smaps_rss_kb},
                    {"pss_kb", mem.smaps_pss_kb},
                    {"private_dirty_kb", mem.private_dirty_kb},
                    {"anonymous_kb", mem.anonymous_kb},
                    {"swap_kb", mem.swap_kb}
                };
#if !defined(_WIN32)
                const struct mallinfo2 mi = mallinfo2();
                out["allocator"] = {
                    {"arena_bytes", (size_t)mi.arena},
                    {"ordblks", (size_t)mi.ordblks},
                    {"hblkhd_bytes", (size_t)mi.hblkhd},
                    {"uordblks_bytes", (size_t)mi.uordblks},
                    {"fordblks_bytes", (size_t)mi.fordblks},
                    {"keepcost_bytes", (size_t)mi.keepcost}
                };
#endif
                out["sizeof"] = {
                    {"FeatureGeom", kFeatureGeomBytes},
                    {"ring_vector", kRingVectorBytes},
                    {"ImVec2", kImVec2Bytes},
                    {"property_pair_string_string", kPropertyPairBytes},
                    {"triangle_index", kTriangleIndexBytes}
                };
                out["layers"] = std::move(layer_list);
                out["totals"] = {
                    {"features", total_features},
                    {"rings", total_rings},
                    {"ring_points", total_points},
                    {"triangle_indices", total_tri_indices},
                    {"properties", total_properties},
                    {"estimated_lower_bound_bytes", total_lower_bound_bytes},
                    {"estimated_lower_bound_mb", (double)total_lower_bound_bytes / (1024.0 * 1024.0)}
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
            } else if (path == "/set_ui") {
                const std::string left = get_q("left_panel_frac");
                const std::string right = get_q("right_panel_frac");
                if (!left.empty()) {
                    ui_left_panel_frac.store(std::clamp(std::stod(left), 0.08, 0.70), std::memory_order_relaxed);
                }
                if (!right.empty()) {
                    ui_right_panel_frac.store(std::clamp(std::stod(right), 0.08, 0.50), std::memory_order_relaxed);
                }
                send_json(200, "OK", {
                    {"ok", true},
                    {"layout", {
                        {"left_panel_frac", ui_left_panel_frac.load(std::memory_order_relaxed)},
                        {"right_panel_frac", ui_right_panel_frac.load(std::memory_order_relaxed)}
                    }}
                });
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
            } else if (path == "/memory") {
                send_json(200, "OK", build_memory_profile());
            } else if (path == "/memory/trim") {
                json before = build_memory_profile();
                const ProcessMemoryTrimResult trim = trimProcessHeap();
                json after = build_memory_profile();
                send_json(200, "OK", {
                    {"ok", true},
                    {"trim", {
                        {"supported", trim.supported},
                        {"attempted", trim.attempted},
                        {"released", trim.released}
                    }},
                    {"before", std::move(before)},
                    {"after", std::move(after)}
                });
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
                const ResourceUsageSnapshot ru = readResourceUsage();
                size_t vm_rss_kb = 0;
                size_t threads = 0;
#if !defined(_WIN32)
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
                    {"pid", currentProcessId()},
                    {"vm_rss_kb", vm_rss_kb},
                    {"threads", threads},
                    {"user_cpu_seconds", ru.user_cpu_seconds},
                    {"system_cpu_seconds", ru.system_cpu_seconds},
                    {"max_rss_kb", ru.max_rss_kb},
                    {"voluntary_context_switches", ru.voluntary_context_switches},
                    {"involuntary_context_switches", ru.involuntary_context_switches}
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
                out["heatmap_aggregate"] = {
                    {"gpu_splat_active", ctx.prof_heatmap_gpu_splat_active && ctx.prof_heatmap_gpu_splat_active->load(std::memory_order_relaxed)},
                    {"high_quality", ctx.prof_heatmap_high_quality && ctx.prof_heatmap_high_quality->load(std::memory_order_relaxed)},
                    {"cache_valid", ctx.prof_heatmap_cache_valid && ctx.prof_heatmap_cache_valid->load(std::memory_order_relaxed)},
                    {"texture_resident", ctx.prof_heatmap_texture_resident && ctx.prof_heatmap_texture_resident->load(std::memory_order_relaxed)},
                    {"async_inflight", ctx.prof_heatmap_async_inflight && ctx.prof_heatmap_async_inflight->load(std::memory_order_relaxed)},
                    {"cache_key", ctx.prof_heatmap_cache_key ? ctx.prof_heatmap_cache_key->load(std::memory_order_relaxed) : 0},
                    {"texture_cache_entries", ctx.prof_heatmap_texture_cache_entries ? ctx.prof_heatmap_texture_cache_entries->load(std::memory_order_relaxed) : 0}
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
            } else if (path == "/ui") {
                json out;
                out["ok"] = true;
                out["note"] = "Live layer UI checkbox state. Selected parcel/zone detail state is render-thread local and is not exposed here.";
                const std::string include_hierarchy = get_q("include_hierarchy");
                const std::string action = get_q("action");
                if (!action.empty()) {
                    try {
                        if (action == "click") {
                            const double x = std::stod(get_q("x"));
                            const double y = std::stod(get_q("y"));
                            const std::string braw = get_q("button");
                            const int button = braw.empty() ? 0 : std::clamp(std::stoi(braw), 0, 4);
                            api_ui_cmd_x.store(x, std::memory_order_relaxed);
                            api_ui_cmd_y.store(y, std::memory_order_relaxed);
                            api_ui_cmd_button.store(button, std::memory_order_relaxed);
                            api_ui_cmd_kind.store(1, std::memory_order_relaxed);
                            api_ui_cmd_seq.fetch_add(1, std::memory_order_relaxed);
                            out["action"] = {{"type", "click"}, {"x", x}, {"y", y}, {"button", button}};
                        } else if (action == "move") {
                            const double x = std::stod(get_q("x"));
                            const double y = std::stod(get_q("y"));
                            api_ui_cmd_x.store(x, std::memory_order_relaxed);
                            api_ui_cmd_y.store(y, std::memory_order_relaxed);
                            api_ui_cmd_kind.store(2, std::memory_order_relaxed);
                            api_ui_cmd_seq.fetch_add(1, std::memory_order_relaxed);
                            out["action"] = {{"type", "move"}, {"x", x}, {"y", y}};
                        } else if (action == "scroll") {
                            const std::string draw = get_q("dy");
                            const double dy = draw.empty() ? 0.0 : std::stod(draw);
                            api_ui_cmd_scroll_y.store(dy, std::memory_order_relaxed);
                            api_ui_cmd_kind.store(3, std::memory_order_relaxed);
                            api_ui_cmd_seq.fetch_add(1, std::memory_order_relaxed);
                            out["action"] = {{"type", "scroll"}, {"dy", dy}};
                        } else if (action == "download_layer") {
                            std::string file = get_q("file");
                            auto lower = [](std::string s) {
                                for (char& c : s) c = (char)std::tolower((unsigned char)c);
                                return s;
                            };
                            if (file.empty()) {
                                const std::string idx_raw = get_q("index");
                                if (!idx_raw.empty()) {
                                    const int idx = std::stoi(idx_raw);
                                    if (idx >= 0 && (size_t)idx < layers.size()) file = layers[(size_t)idx].file;
                                }
                            }
                            if (file.empty()) {
                                const std::string name = get_q("name");
                                if (!name.empty()) {
                                    for (const auto& l : layers) {
                                        if (l.name == name) {
                                            file = l.file;
                                            break;
                                        }
                                    }
                                }
                            }
                            if (file.empty()) {
                                out["ok"] = false;
                                out["error"] = "download_layer requires one of: file, index, or name";
                            } else {
                                size_t matched = layers.size();
                                const std::string needle = lower(file);
                                for (size_t i = 0; i < layers.size(); ++i) {
                                    if (lower(layers[i].file) == needle || lower(layers[i].name) == needle) {
                                        matched = i;
                                        break;
                                    }
                                }
                                if (matched == layers.size()) {
                                    out["ok"] = false;
                                    out["error"] = std::string("download_layer target not found: ") + file;
                                } else {
                                    file = layers[matched].file;
                                    const auto& layer = layers[matched];
                                    if (layer.source_url.empty()) {
                                        out["ok"] = false;
                                        out["error"] = "layer has no source URL";
                                    } else {
                                        std::lock_guard<std::mutex> lk(api_layer_mutex);
                                        api_layer_download_cmds.push_back(file);
                                    }
                                    out["action"] = {
                                        {"type", "download_layer"},
                                        {"file", file},
                                        {"index", matched},
                                        {"name", layer.name},
                                        {"has_source_url", !layer.source_url.empty()},
                                        {"enqueued", !layer.source_url.empty()}
                                    };
                                }
                            }
                        } else {
                            out["ok"] = false;
                            out["error"] = "unsupported action; use click|move|scroll|download_layer";
                        }
                    } catch (...) {
                        out["ok"] = false;
                        out["error"] = "invalid ui action parameters";
                    }
                }
                out["view"] = {
                    {"zoom", current_zoom_state.load(std::memory_order_relaxed)},
                    {"center_lon", current_lon_state.load(std::memory_order_relaxed)},
                    {"center_lat", current_lat_state.load(std::memory_order_relaxed)}
                };
                out["layout"] = {
                    {"left_panel_frac", ui_left_panel_frac.load(std::memory_order_relaxed)},
                    {"right_panel_frac", ui_right_panel_frac.load(std::memory_order_relaxed)}
                };
                {
                    std::lock_guard<std::mutex> lk(layer_fill_mutex);
                    json layer_ui = json::array();
                    std::unordered_map<std::string, json> by_category;
                    for (size_t i = 0; i < layers.size(); ++i) {
                        const auto& l = layers[i];
                        const std::string category = categoryToString(l.category);
                        by_category[category]["name"] = category;
                        if (!by_category[category].contains("children")) by_category[category]["children"] = json::array();
                        by_category[category]["children"].push_back({
                            {"id", l.file},
                            {"label", l.name},
                            {"kind", "layer"},
                            {"index", i},
                            {"downloadable", !l.source_url.empty()}
                        });
                        layer_ui.push_back({
                            {"index", i},
                            {"file", l.file},
                            {"name", l.name},
                            {"enabled", l.enabled},
                            {"fill_enabled", i < layer_fill_enabled.size() ? layer_fill_enabled[i] : true},
                            {"hover_enabled", i < layer_hover_enabled.size() ? layer_hover_enabled[i] : true},
                            {"inspect_enabled", i < layer_inspect_enabled.size() ? layer_inspect_enabled[i] : true},
                            {"heatmap_enabled", i < layer_heatmap_enabled.size() ? layer_heatmap_enabled[i] : false}
                        });
                    }
                    out["layers"] = std::move(layer_ui);
                    if (include_hierarchy == "1" || include_hierarchy == "true" || include_hierarchy.empty()) {
                        json cats = json::array();
                        for (const auto& cat : {"Housing", "Public Health", "Safety", "Infrastructure", "Zoning"}) {
                            auto it = by_category.find(cat);
                            if (it != by_category.end()) cats.push_back(it->second);
                        }
                        out["ui_hierarchy"] = {
                            {"windows", json::array({
                                {{"id", "layers_controls"}, {"label", "Layers and Controls"}},
                                {{"id", "map"}, {"label", "Map"}},
                                {{"id", "record_filters"}, {"label", "Record Filters"}},
                                {{"id", "download_queue"}, {"label", "Download Queue"}, {"conditional", true}},
                                {{"id", "gear_panel"}, {"label", "Gear Panel"}, {"conditional", true}}
                            })},
                            {"layers_and_controls", {
                                {"sections", json::array({
                                    "Basemap controls",
                                    "Basemap Layers",
                                    "Housing",
                                    "Public Health",
                                    "Safety",
                                    "Infrastructure",
                                    "Zoning",
                                    "Zoning Filters",
                                    "Performance and Stats"
                                })},
                                {"categories", std::move(cats)},
                                {"row_controls", json::array({"D", "V", "⚙"})}
                            }},
                            {"record_filters_tabs", json::array({"Filters", "Vacancy-Parcel", "Gradient", "Owners"})}
                        };
                        const std::string md = readTextFileIfExists("UI_HIERARCHY.md");
                        if (!md.empty()) out["ui_hierarchy_markdown"] = md;
                    }
                }
                out["vacancy"] = {
                    {"matched_total", vacant_parcels_matched_total.load(std::memory_order_relaxed)},
                    {"with_geometry_total", vacant_parcels_with_geometry_total.load(std::memory_order_relaxed)},
                    {"triangulated_renderable_total", vacant_parcels_triangulated_renderable_total.load(std::memory_order_relaxed)},
                    {"visible_last_frame", visible_vacant_parcels_last_frame.load(std::memory_order_relaxed)}
                };
                std::string body = out.dump();
                std::ostringstream os;
                os << "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " << body.size()
                   << "\r\nConnection: close\r\n\r\n" << body;
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
                const std::string native_q = toLowerAscii(urlDecode(get_q("native")));
                const std::string fullres_q = toLowerAscii(urlDecode(get_q("fullres")));
                const std::string raw_q = toLowerAscii(urlDecode(get_q("raw")));
                const bool request_native =
                    native_q == "1" || native_q == "true" || native_q == "yes" || native_q == "on" ||
                    fullres_q == "1" || fullres_q == "true" || fullres_q == "yes" || fullres_q == "on" ||
                    raw_q == "1" || raw_q == "true" || raw_q == "yes" || raw_q == "on";
                uint64_t req_id = 0;
                {
                    std::lock_guard<std::mutex> lk(g_ScreenshotState.mutex);
                    g_ScreenshotState.req_id += 1;
                    req_id = g_ScreenshotState.req_id;
                    g_ScreenshotState.pending = true;
                    g_ScreenshotState.request_native = request_native;
                    g_ScreenshotState.ok = false;
                    g_ScreenshotState.path.clear();
                    g_ScreenshotState.error.clear();
                    g_ScreenshotState.native_width = 0;
                    g_ScreenshotState.native_height = 0;
                    g_ScreenshotState.logical_width = 0;
                    g_ScreenshotState.logical_height = 0;
                    g_ScreenshotState.output_width = 0;
                    g_ScreenshotState.output_height = 0;
                    g_ScreenshotState.framebuffer_scale_x = 1.0f;
                    g_ScreenshotState.framebuffer_scale_y = 1.0f;
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
                out["native_requested"] = request_native;
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
                        out["native_width"] = g_ScreenshotState.native_width;
                        out["native_height"] = g_ScreenshotState.native_height;
                        out["logical_width"] = g_ScreenshotState.logical_width;
                        out["logical_height"] = g_ScreenshotState.logical_height;
                        out["output_width"] = g_ScreenshotState.output_width;
                        out["output_height"] = g_ScreenshotState.output_height;
                        out["framebuffer_scale_x"] = g_ScreenshotState.framebuffer_scale_x;
                        out["framebuffer_scale_y"] = g_ScreenshotState.framebuffer_scale_y;
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
}
