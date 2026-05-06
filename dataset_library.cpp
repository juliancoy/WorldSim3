#include "dataset_library.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <functional>
#include <iomanip>
#include <sstream>
#include <unordered_map>

#include <curl/curl.h>

using json = nlohmann::json;
namespace fs = std::filesystem;

static size_t curlWriteToFile(void* ptr, size_t size, size_t nmemb, void* userdata) {
    FILE* fp = (FILE*)userdata;
    return std::fwrite(ptr, size, nmemb, fp);
}

bool downloadUrlToFile(const std::string& url, const fs::path& out_path, std::string& err) {
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
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 120L);
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
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
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

VersionedDownloadResult downloadUrlVersioned(
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
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 120L);
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

FreshnessCheckResult checkUrlFreshnessVersioned(
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

std::vector<json> readLayerManifestEntries(const fs::path& root) {
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

std::filesystem::path layerManifestPathForPhase(const fs::path& root, const std::string& phase) {
    if (phase == "all" || phase.empty()) return root / "layers_manifest.json";
    if (phase == "must-have") return root / "layers_manifest.must_have.json";
    if (phase == "nice-to-have") return root / "layers_manifest.nice_to_have.json";
    if (phase == "heavy-data") return root / "layers_manifest.heavy_data.json";
    if (phase == "capital-flows") return root / "layers_manifest.capital_flows.json";
    fs::path p(phase);
    return p.is_absolute() ? p : root / p;
}

static fs::path layerOutputDirForManifestItem(const fs::path& root, const json& item) {
    if (item.contains("directory") && item["directory"].is_string()) {
        fs::path p(item["directory"].get<std::string>());
        return p.is_absolute() ? p : root / p;
    }
    if (item.value("category", std::string()) == "capital-flows") {
        return root / "data" / "capital_flows";
    }
    return root / "data" / "layers";
}

LayerDownloadSummary downloadLayerManifestPhase(
    const fs::path& root,
    const std::string& phase,
    bool include_large,
    const std::function<void(size_t, size_t, const std::string&)>& on_progress) {
    LayerDownloadSummary summary;
    const fs::path manifest_path = layerManifestPathForPhase(root, phase);
    std::ifstream in(manifest_path);
    if (!in) {
        summary.failed = 1;
        summary.messages.push_back("manifest not found: " + manifest_path.string());
        return summary;
    }

    json items;
    try {
        in >> items;
    } catch (const std::exception& e) {
        summary.failed = 1;
        summary.messages.push_back("manifest parse failed: " + std::string(e.what()));
        return summary;
    }
    if (!items.is_array()) {
        summary.failed = 1;
        summary.messages.push_back("manifest is not an array: " + manifest_path.string());
        return summary;
    }

    summary.total = items.size();
    for (size_t i = 0; i < items.size(); ++i) {
        const json& item = items[i];
        const std::string name = item.value("name", std::string("unnamed"));
        auto report = [&](const std::string& msg) {
            summary.messages.push_back(msg);
            if (on_progress) on_progress(i + 1, items.size(), msg);
        };

        if (item.value("download", true) == false) {
            summary.skipped++;
            report("skipped " + name + ": " + item.value("reason", std::string("metadata/API/manual source")));
            continue;
        }
        if (item.value("large", false) && !include_large) {
            summary.skipped++;
            report("skipped " + name + ": large source; rerun with --include-large");
            continue;
        }
        if (!item.contains("url") || !item["url"].is_string() || !item.contains("file") || !item["file"].is_string()) {
            summary.failed++;
            report("failed " + name + ": missing url or file");
            continue;
        }

        const std::string file = item["file"].get<std::string>();
        const fs::path out_path = layerOutputDirForManifestItem(root, item) / file;
        report("downloading " + name + " -> " + out_path.string());
        VersionedDownloadResult res = downloadUrlVersioned(
            item["url"].get<std::string>(),
            out_path,
            root / "data" / "versions");
        if (res.ok) {
            summary.downloaded++;
            report((res.not_modified ? "checked " : "downloaded ") + file + ": " + res.message);
        } else {
            summary.failed++;
            report("failed " + name + ": " + res.message);
        }
    }

    return summary;
}
