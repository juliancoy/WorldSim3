#include "worldsim_dataset_bootstrap.h"

#include "app_utils.h"
#include "dataset_library.h"
#include "layer_import.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <thread>
#include <ctime>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
struct CliDownloadItem {
    std::string name;
    std::string file;
    bool skipped = false;
    std::string skip_reason;
    bool done = false;
    bool ok = false;
    bool not_modified = false;
    std::string message;
    bool exists_before = false;
    bool exists_after = false;
    bool materialized_before = false;
    bool materialized_after = false;
    bool hash_verified = false;
    std::string hash_state;
    std::string output_path;
    std::string canonical_path;
    std::string source_artifact_path;
    uint64_t bytes_now = 0;
    uint64_t bytes_total = 0;
    double ewma_bps = 0.0;
    std::chrono::steady_clock::time_point last_sample_at{};
    uint64_t last_sample_bytes = 0;
};

static uint64_t fnv1a64File(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return 0;
    uint64_t h = 1469598103934665603ull;
    char buf[1 << 15];
    while (in.good()) {
        in.read(buf, sizeof(buf));
        const std::streamsize n = in.gcount();
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

static std::string metadataHashForOutput(const fs::path& root, const fs::path& out_path) {
    const fs::path meta_path = root / "data" / "versions" / "metadata" / (out_path.filename().string() + ".json");
    std::ifstream in(meta_path);
    if (!in) return {};
    json meta;
    try {
        in >> meta;
    } catch (...) {
        return {};
    }
    if (!meta.is_object()) return {};
    return meta.value("content_hash", std::string());
}

static fs::path canonicalOutputPathForLayerFile(const fs::path& out_path, const std::string& file) {
    return out_path.parent_path() / (layerArtifactBasenameForFile(file) + ".canonical.bin");
}

static bool layerOutputMaterialized(const fs::path& root, const fs::path& out_path, const std::string& file) {
    std::error_code ec;
    if (fs::exists(out_path, ec) && !ec) return true;
    ec.clear();
    if (fs::exists(canonicalOutputPathForLayerFile(out_path, file), ec) && !ec) return true;
    ec.clear();
    const fs::path legacy_path = root / "data" / "layers" / file;
    if (fs::exists(legacy_path, ec) && !ec) return true;
    ec.clear();
    return fs::exists(canonicalOutputPathForLayerFile(legacy_path, file), ec) && !ec;
}

static fs::path layerOutputDirForManifestItem(const fs::path& root, const json& item) {
    if (item.contains("directory") && item["directory"].is_string()) {
        fs::path p(item["directory"].get<std::string>());
        return p.is_absolute() ? p : root / p;
    }
    if (item.contains("provenance") && item["provenance"].is_object()) {
        const auto& provenance = item["provenance"];
        LayerDef layer;
        layer.file = item.value("file", std::string());
        layer.provenance_world = provenance.value("world", std::string());
        layer.provenance_nation_state = provenance.value("nation_state", std::string());
        layer.provenance_state_region = provenance.value("state_region", std::string());
        layer.provenance_county_city = provenance.value("county_city", std::string());
        return provenanceStoredLayerPath(root, layer).parent_path();
    }
    if (item.value("category", std::string()) == "capital-flows") {
        return root / "data" / "capital_flows";
    }
    return root / "data" / "provenance" / "stored" / "world" / "earth" / "layers";
}

static LayerDef layerFromManifestItem(const json& item) {
    LayerDef layer;
    layer.name = item.value("name", std::string("unnamed"));
    layer.file = item.value("file", std::string());
    if (item.contains("url") && item["url"].is_string()) layer.source_url = item["url"].get<std::string>();
    if (item.contains("import") && item["import"].is_object()) {
        const auto& import = item["import"];
        layer.import_type = import.value("type", std::string());
        layer.import_url = import.value("url", std::string());
        layer.import_source_crs = import.value("source_crs", std::string());
        layer.import_shapefile = import.value("shapefile", std::string());
        layer.import_service_url = import.value("service_url", std::string());
        layer.import_where = import.value("where", std::string());
        layer.import_normalizer = import.value("normalizer", std::string());
        layer.import_query = import.value("query", std::string());
        layer.import_table = import.value("table", std::string());
        layer.import_year = import.value("year", std::string());
        layer.import_survey = import.value("survey", std::string());
        layer.import_sheet_name = import.value("sheet_name", std::string());
        layer.import_lon_field = import.value("lon_field", std::string());
        layer.import_lat_field = import.value("lat_field", std::string());
        layer.import_artifact_file = import.value("artifact_file", std::string());
        layer.import_item_path = import.value("item_path", std::string());
    }
    if (item.contains("provenance") && item["provenance"].is_object()) {
        const auto& provenance = item["provenance"];
        layer.provenance_world = provenance.value("world", std::string());
        layer.provenance_nation_state = provenance.value("nation_state", std::string());
        layer.provenance_state_region = provenance.value("state_region", std::string());
        layer.provenance_county_city = provenance.value("county_city", std::string());
    }
    return layer;
}

static fs::path importSourceArtifactPathForManifestItem(const fs::path& root, const json& item) {
    if (!item.contains("import") || !item["import"].is_object()) return {};
    const auto& import = item["import"];
    const std::string import_type = import.value("type", std::string());
    if (import_type.empty()) return {};
    LayerDef layer = layerFromManifestItem(item);
    const std::string file = item.value("file", std::string());
    if (file.empty()) return {};
    std::string artifact_file = import.value("artifact_file", std::string());
    if (artifact_file.empty() && import_type == "socrata_csv_properties") {
        artifact_file = file + ".source.csv";
    }
    if (artifact_file.empty()) return {};
    return provenanceSourceArtifactPath(root, layer, artifact_file);
}

static bool importSourceArtifactMaterialized(const fs::path& root, const json& item, fs::path* out_path = nullptr) {
    const fs::path artifact_path = importSourceArtifactPathForManifestItem(root, item);
    if (out_path) *out_path = artifact_path;
    if (artifact_path.empty()) return false;
    std::error_code ec;
    return fs::exists(artifact_path, ec) && !ec;
}

static std::string renderBar(float p, int width) {
    const int filled = std::clamp((int)std::lround(p * (double)width), 0, width);
    std::string out;
    out.reserve((size_t)width);
    for (int i = 0; i < width; ++i) out.push_back(i < filled ? '=' : ' ');
    return out;
}

static std::string humanBps(double bps) {
    const char* units[] = {"B/s", "KB/s", "MB/s", "GB/s"};
    int u = 0;
    while (bps >= 1024.0 && u < 3) { bps /= 1024.0; ++u; }
    std::ostringstream os;
    os << std::fixed << std::setprecision(u == 0 ? 0 : 1) << bps << ' ' << units[u];
    return os.str();
}

static std::string isoNowUtcCompact() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &tm);
    return std::string(buf);
}
}

int runLayerDownloadCli(const fs::path& root, const std::string& phase, bool include_large) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    const std::string selected_phase = phase.empty() ? "all" : phase;
    LayerDownloadSummary summary;
    std::cout << "Using app root: " << root.string() << "\n";
    std::cout << "Using manifest phase: " << selected_phase << "\n";

    const fs::path manifest_path = layerManifestPathForPhase(root, selected_phase);
    std::ifstream in(manifest_path);
    if (!in) {
        curl_global_cleanup();
        std::cerr << "manifest not found: " << manifest_path << "\n";
        return 1;
    }
    json items_json;
    try {
        in >> items_json;
    } catch (const std::exception& e) {
        curl_global_cleanup();
        std::cerr << "manifest parse failed: " << e.what() << "\n";
        return 1;
    }
    if (!items_json.is_array()) {
        curl_global_cleanup();
        std::cerr << "manifest is not an array: " << manifest_path << "\n";
        return 1;
    }

    std::vector<CliDownloadItem> items;
    std::vector<LayerDef> layers;
    std::vector<fs::path> out_paths;
    std::vector<size_t> work_item_indices;
    items.reserve(items_json.size());
    layers.reserve(items_json.size());
    out_paths.reserve(items_json.size());
    for (const auto& item : items_json) {
        CliDownloadItem c;
        c.name = item.value("name", std::string("unnamed"));
        c.file = item.value("file", std::string());
        const fs::path out_path = c.file.empty() ? fs::path() : layerOutputDirForManifestItem(root, item) / c.file;
        if (!out_path.empty()) {
            c.output_path = out_path.string();
            c.canonical_path = canonicalOutputPathForLayerFile(out_path, c.file).string();
            c.materialized_before = layerOutputMaterialized(root, out_path, c.file);
        }
        fs::path source_artifact_path;
        const bool source_artifact_before = importSourceArtifactMaterialized(root, item, &source_artifact_path);
        if (!source_artifact_path.empty()) c.source_artifact_path = source_artifact_path.string();
        c.materialized_before = c.materialized_before || source_artifact_before;
        const bool has_import = item.contains("import") && item["import"].is_object();
        const bool has_url = item.contains("url") && item["url"].is_string();
        if (item.value("download", true) == false && !has_import) {
            c.skipped = true;
            c.skip_reason = item.value("reason", std::string("metadata/API/manual source"));
            summary.skipped++;
        } else if (item.value("large", false) && !include_large) {
            c.skipped = true;
            c.skip_reason = "large source; rerun with --include-large";
            summary.skipped++;
        } else if ((!has_url && !has_import) || c.file.empty()) {
            c.done = true;
            c.ok = false;
            c.message = "missing url/import or file";
            summary.failed++;
        } else {
            layers.push_back(layerFromManifestItem(item));
            out_paths.push_back(out_path);
            std::error_code ec;
            c.exists_before = fs::exists(out_path, ec) && !ec;
            if (c.exists_before) {
                const uint64_t h = fnv1a64File(out_path);
                const std::string local_hash = h == 0 ? std::string() : toHexU64(h);
                const std::string meta_hash = metadataHashForOutput(root, out_path);
                if (!local_hash.empty() && !meta_hash.empty()) {
                    c.hash_verified = (local_hash == meta_hash);
                    c.hash_state = c.hash_verified ? "hash-ok" : "hash-mismatch";
                } else if (!local_hash.empty()) {
                    c.hash_state = "hash-local-only";
                } else {
                    c.hash_state = "hash-unavailable";
                }
            } else {
                c.hash_state = "missing";
            }
            work_item_indices.push_back(items.size());
        }
        items.push_back(std::move(c));
    }
    summary.total = items.size();

    std::mutex mu;
    size_t done_count = summary.skipped + summary.failed;
    size_t active = 0;
    size_t next_work = 0;
    struct Task { size_t slot; std::future<VersionedDownloadResult> fut; };
    std::vector<Task> running;
    const size_t max_parallel = std::clamp<size_t>(std::max(1u, std::thread::hardware_concurrency()) / 2, 2, 8);
    const auto started_at = std::chrono::steady_clock::now();
    while (done_count < items.size()) {
        while (running.size() < max_parallel && next_work < layers.size()) {
            const size_t item_idx = work_item_indices[next_work];
            if (item_idx >= items.size()) { next_work++; continue; }
            items[item_idx].last_sample_at = std::chrono::steady_clock::now();
            items[item_idx].last_sample_bytes = 0;
            items[item_idx].bytes_now = 0;
            items[item_idx].bytes_total = 0;
            const LayerDef layer = layers[next_work];
            const fs::path out = out_paths[next_work];
            running.push_back(Task{
                next_work,
                std::async(std::launch::async, [&, layer, out, item_idx]() {
                    return downloadOrImportLayer(layer, out, root, [&, item_idx](uint64_t now, uint64_t total) {
                        std::lock_guard<std::mutex> lk(mu);
                        auto& it = items[item_idx];
                        const auto t = std::chrono::steady_clock::now();
                        if (it.last_sample_at.time_since_epoch().count() != 0 && now >= it.last_sample_bytes) {
                            const double dt = std::chrono::duration<double>(t - it.last_sample_at).count();
                            const uint64_t db = now - it.last_sample_bytes;
                            if (dt >= 0.20 && db > 0) {
                                const double inst = (double)db / dt;
                                constexpr double kAlpha = 0.22;
                                it.ewma_bps = it.ewma_bps <= 0.0 ? inst : (kAlpha * inst + (1.0 - kAlpha) * it.ewma_bps);
                            }
                        }
                        it.last_sample_at = t;
                        it.last_sample_bytes = now;
                        it.bytes_now = now;
                        it.bytes_total = total;
                    });
                })
            });
            active++;
            next_work++;
        }

        for (size_t i = 0; i < running.size();) {
            if (running[i].fut.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) { ++i; continue; }
            const size_t slot = running[i].slot;
            const size_t item_idx = work_item_indices[slot];
            VersionedDownloadResult res = running[i].fut.get();
            if (item_idx < items.size()) {
                std::lock_guard<std::mutex> lk(mu);
                auto& it = items[item_idx];
                it.done = true;
                it.ok = res.ok;
                it.not_modified = res.not_modified;
                it.message = res.message;
                std::error_code ec;
                it.exists_after = fs::exists(out_paths[slot], ec) && !ec;
                if (it.exists_after && !it.hash_verified) {
                    const uint64_t h = fnv1a64File(out_paths[slot]);
                    const std::string local_hash = h == 0 ? std::string() : toHexU64(h);
                    const std::string meta_hash = metadataHashForOutput(root, out_paths[slot]);
                    if (!local_hash.empty() && !meta_hash.empty()) {
                        it.hash_verified = (local_hash == meta_hash);
                        it.hash_state = it.hash_verified ? "hash-ok" : "hash-mismatch";
                    } else if (!local_hash.empty()) {
                        it.hash_state = "hash-local-only";
                    }
                }
                if (res.ok) summary.downloaded++;
                else summary.failed++;
                done_count++;
                active--;
            }
            running.erase(running.begin() + (std::ptrdiff_t)i);
        }

        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_at).count();
        std::cout << "\033[2J\033[H";
        std::cout << "worldsim3 layer pull  phase=" << selected_phase << "  elapsed=" << std::fixed << std::setprecision(1) << elapsed << "s\n";
        std::cout << "overall: done " << done_count << "/" << items.size()
                  << "  active " << active
                  << "  downloaded " << summary.downloaded
                  << "  skipped " << summary.skipped
                  << "  failed " << summary.failed << "\n\n";
        for (size_t i = 0; i < items.size(); ++i) {
            const auto& it = items[i];
            if (it.skipped) {
                std::cout << it.file << ": skipped (" << it.skip_reason << ")\n";
                continue;
            }
            if (it.done) {
                std::cout << it.file << ": " << (it.ok ? (it.not_modified ? "up-to-date" : "done") : "failed")
                          << " - " << it.message
                          << " | exists:" << (it.exists_after ? "yes" : "no")
                          << " | " << it.hash_state << "\n";
                continue;
            }
            float p = (it.bytes_total > 0) ? std::clamp((float)it.bytes_now / (float)it.bytes_total, 0.0f, 0.99f) : 0.0f;
            std::string eta = "--:--";
            if (it.bytes_total > 0 && it.ewma_bps > 1024.0 && it.bytes_now < it.bytes_total) {
                const double s = (double)(it.bytes_total - it.bytes_now) / it.ewma_bps;
                const int sec = std::max(0, (int)std::lround(s));
                std::ostringstream eos;
                eos << std::setw(2) << std::setfill('0') << (sec / 60) << ":" << std::setw(2) << (sec % 60);
                eta = eos.str();
            }
            std::cout << it.file << ": [" << renderBar(p, 24) << "] "
                      << std::setw(3) << (int)std::lround(p * 100.0f) << "%  "
                      << std::setw(10) << humanBps(it.ewma_bps) << "  eta " << eta
                      << "  " << (it.bytes_total > 0 ? (std::to_string(it.bytes_now / 1024) + "/" + std::to_string(it.bytes_total / 1024) + "KB") : "size ?")
                      << "  exists:" << (it.exists_before ? "yes" : "no")
                      << "\n";
        }
        std::cout.flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }

    const std::string run_stamp = isoNowUtcCompact();
    size_t audit_materialized = 0;
    size_t audit_missing = 0;
    size_t audit_required_missing = 0;
    std::vector<std::string> audit_missing_files;
    std::vector<std::string> audit_required_missing_files;
    json records = json::array();
    for (auto& it : items) {
        if (!it.output_path.empty()) {
            const fs::path out_path(it.output_path);
            std::error_code ec;
            it.exists_after = fs::exists(out_path, ec) && !ec;
            it.materialized_after = layerOutputMaterialized(root, out_path, it.file);
        }
        if (!it.source_artifact_path.empty()) {
            std::error_code ec;
            it.materialized_after = it.materialized_after || (fs::exists(it.source_artifact_path, ec) && !ec);
        }
        if (!it.file.empty()) {
            if (it.materialized_after) {
                ++audit_materialized;
            } else {
                ++audit_missing;
                audit_missing_files.push_back(it.file);
                if (!it.skipped) {
                    ++audit_required_missing;
                    audit_required_missing_files.push_back(it.file);
                }
            }
        }
        records.push_back({
            {"name", it.name},
            {"file", it.file},
            {"skipped", it.skipped},
            {"skip_reason", it.skip_reason},
            {"done", it.done},
            {"ok", it.ok},
            {"not_modified", it.not_modified},
            {"message", it.message},
            {"exists_before", it.exists_before},
            {"exists_after", it.exists_after},
            {"materialized_before", it.materialized_before},
            {"materialized_after", it.materialized_after},
            {"hash_verified", it.hash_verified},
            {"hash_state", it.hash_state},
            {"output_path", it.output_path},
            {"canonical_path", it.canonical_path},
            {"source_artifact_path", it.source_artifact_path}
        });
    }
    const fs::path report_path = root / "data" / "versions" / "reports" / ("layer_download_run_" + run_stamp + ".json");
    fs::create_directories(report_path.parent_path());
    json report = {
        {"schema_version", 1},
        {"created_at", run_stamp},
        {"phase", selected_phase},
        {"include_large", include_large},
        {"summary", {
            {"downloaded", summary.downloaded},
            {"skipped", summary.skipped},
            {"failed", summary.failed},
            {"total", summary.total},
            {"audit_materialized", audit_materialized},
            {"audit_missing", audit_missing},
            {"audit_required_missing", audit_required_missing}
        }},
        {"audit_missing_files", audit_missing_files},
        {"audit_required_missing_files", audit_required_missing_files},
        {"records", std::move(records)}
    };
    {
        std::ofstream out(report_path);
        if (out) out << report.dump(2);
    }
    curl_global_cleanup();
    std::cout << "\nDone. downloaded=" << summary.downloaded
              << " skipped=" << summary.skipped
              << " failed=" << summary.failed
              << " total=" << summary.total
              << " audit_materialized=" << audit_materialized
              << " audit_missing=" << audit_missing
              << " audit_required_missing=" << audit_required_missing << "\n";
    if (!audit_missing_files.empty()) {
        std::cout << "Audit missing files:";
        const size_t n = std::min<size_t>(audit_missing_files.size(), 20);
        for (size_t i = 0; i < n; ++i) std::cout << " " << audit_missing_files[i];
        if (audit_missing_files.size() > n) std::cout << " ...";
        std::cout << "\n";
    }
    if (!audit_required_missing_files.empty()) {
        std::cout << "Audit required missing files:";
        const size_t n = std::min<size_t>(audit_required_missing_files.size(), 20);
        for (size_t i = 0; i < n; ++i) std::cout << " " << audit_required_missing_files[i];
        if (audit_required_missing_files.size() > n) std::cout << " ...";
        std::cout << "\n";
    }
    std::cout << "Report: " << report_path.string() << "\n";
    return summary.failed == 0 && audit_required_missing == 0 ? 0 : 1;
}

bool envEnabled(const char* name) {
    const char* value = std::getenv(name);
    if (!value || !*value) return false;
    std::string s = toLowerAscii(value);
    return s != "0" && s != "false" && s != "no" && s != "off";
}

void preloadLayersFromEnvironment(const fs::path& root) {
    if (!envEnabled("WORLD_SIM3_PRELOAD_DATA")) return;

    const char* phase_env = std::getenv("WORLD_SIM3_PRELOAD_PHASE");
    const std::string preload_phase = (phase_env && *phase_env) ? std::string(phase_env) : "all";
    LayerDownloadSummary summary = downloadLayerManifestPhase(
        root,
        preload_phase,
        envEnabled("WORLD_SIM3_INCLUDE_LARGE"),
        [](size_t i, size_t total, const std::string& msg) {
            std::cout << "[preload " << i << "/" << total << "] " << msg << "\n";
        });
    if (summary.failed > 0) {
        std::cerr << "Preload completed with " << summary.failed << " failed downloads.\n";
    }
}
