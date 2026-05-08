#include "cache_io.h"

#include "memory_utils.h"

#include <fstream>

#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
struct TrimHeapOnScopeExit {
    ~TrimHeapOnScopeExit() { trimProcessHeap(); }
};
}

std::string fileSignature(const fs::path& p) {
    std::error_code ec;
    auto sz = fs::file_size(p, ec);
    auto wt = fs::last_write_time(p, ec);
    auto ticks = wt.time_since_epoch().count();
    return std::to_string((unsigned long long)sz) + "_" + std::to_string((long long)ticks);
}

bool loadHydrationCache(const fs::path& cache_path, const std::string& sig, std::vector<LayerDef::FeatureGeom>& out) {
    std::ifstream in(cache_path, std::ios::binary);
    if (!in) return false;
    bool ok = false;
    {
        TrimHeapOnScopeExit trim_on_exit;
        std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (buf.empty()) return false;
        json j;
        try {
            j = json::from_msgpack(buf);
        } catch (...) {
            return false;
        }
        if (!j.contains("version") || j["version"].get<int>() != 1) return false;
        if (!j.contains("signature") || j["signature"].get<std::string>() != sig) return false;
        if (!j.contains("features") || !j["features"].is_array()) return false;

        out.clear();
        out.reserve(j["features"].size());
        for (const auto& jf : j["features"]) {
            LayerDef::FeatureGeom fg{};
            if (!jf.contains("extent") || !jf.contains("rings")) return false;
            const auto& je = jf["extent"];
            fg.extent.min_lon = je.value("min_lon", 0.0f);
            fg.extent.min_lat = je.value("min_lat", 0.0f);
            fg.extent.max_lon = je.value("max_lon", 0.0f);
            fg.extent.max_lat = je.value("max_lat", 0.0f);
            for (const auto& jr : jf["rings"]) {
                std::vector<ImVec2> ring;
                ring.reserve(jr.size());
                for (const auto& jp : jr) {
                    if (!jp.is_array() || jp.size() < 2) continue;
                    ring.push_back(ImVec2(jp[0].get<float>(), jp[1].get<float>()));
                }
                if (!ring.empty()) fg.rings.push_back(std::move(ring));
            }
            if (jf.contains("properties") && jf["properties"].is_array()) {
                fg.properties.reserve(jf["properties"].size());
                for (const auto& kv : jf["properties"]) {
                    if (!kv.is_array() || kv.size() < 2) continue;
                    fg.properties.push_back({kv[0].get<std::string>(), kv[1].get<std::string>()});
                }
            }
            out.push_back(std::move(fg));
        }
        ok = true;
    }
    return ok;
}

void saveHydrationCache(const fs::path& cache_path, const std::string& sig, const std::vector<LayerDef::FeatureGeom>& features) {
    {
        TrimHeapOnScopeExit trim_on_exit;
        json j;
        j["version"] = 1;
        j["signature"] = sig;
        j["features"] = json::array();
        for (const auto& fg : features) {
            json jf;
            jf["extent"] = {
                {"min_lon", fg.extent.min_lon},
                {"min_lat", fg.extent.min_lat},
                {"max_lon", fg.extent.max_lon},
                {"max_lat", fg.extent.max_lat},
            };
            jf["rings"] = json::array();
            for (const auto& r : fg.rings) {
                json jr = json::array();
                for (const auto& p : r) jr.push_back({p.x, p.y});
                jf["rings"].push_back(std::move(jr));
            }
            jf["properties"] = json::array();
            for (const auto& kv : fg.properties) jf["properties"].push_back({kv.first, kv.second});
            j["features"].push_back(std::move(jf));
        }
        fs::create_directories(cache_path.parent_path());
        std::vector<uint8_t> bin = json::to_msgpack(j);
        std::ofstream out(cache_path, std::ios::binary);
        if (out) out.write((const char*)bin.data(), (std::streamsize)bin.size());
    }
}

bool loadTriCache(const fs::path& cache_path, const std::string& sig, size_t count, std::vector<std::vector<uint32_t>>& out) {
    std::ifstream in(cache_path);
    if (!in) return false;
    bool ok = false;
    {
        TrimHeapOnScopeExit trim_on_exit;
        json j;
        in >> j;
        if (!j.contains("signature") || !j.contains("triangles")) return false;
        if (j["signature"].get<std::string>() != sig) return false;
        auto arr = j["triangles"];
        if (!arr.is_array() || arr.size() != count) return false;
        out.resize(count);
        for (size_t i = 0; i < count; ++i) out[i] = arr[i].get<std::vector<uint32_t>>();
        ok = true;
    }
    return ok;
}

void saveTriCache(const fs::path& cache_path, const std::string& sig, const std::vector<std::vector<uint32_t>>& tris) {
    fs::create_directories(cache_path.parent_path());
    {
        TrimHeapOnScopeExit trim_on_exit;
        json j;
        j["signature"] = sig;
        j["triangles"] = tris;
        std::ofstream out(cache_path);
        if (out) out << j.dump();
    }
}
