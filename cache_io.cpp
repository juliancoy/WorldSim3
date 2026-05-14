#include "cache_io.h"

#include "memory_utils.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>

#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
struct TrimHeapOnScopeExit {
    ~TrimHeapOnScopeExit() { trimProcessHeap(); }
};

fs::path tempCachePathFor(const fs::path& cache_path) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::ostringstream name;
    name << cache_path.filename().string() << ".tmp." << now;
    return cache_path.parent_path() / name.str();
}

constexpr std::array<char, 8> kHydrationBinaryMagic{{'W', 'S', '3', 'H', 'Y', 'D', '2', '\0'}};
constexpr uint32_t kHydrationBinaryVersion = 1;
constexpr uint64_t kMaxBinaryHydrationFeatures = 5000000ull;
constexpr uint32_t kMaxBinaryHydrationRingsPerFeature = 10000u;
constexpr uint32_t kMaxBinaryHydrationPointsPerRing = 1000000u;
constexpr uint32_t kMaxBinaryHydrationPropertiesPerFeature = 10000u;
constexpr uint32_t kMaxBinaryHydrationStringBytes = 64u * 1024u * 1024u;
constexpr std::array<char, 8> kTriBinaryMagic{{'W', 'S', '3', 'T', 'R', 'I', '2', '\0'}};
constexpr uint32_t kTriBinaryVersion = 1;
constexpr uint32_t kMaxBinaryTriIndicesPerFeature = 20000000u;

bool hostIsLittleEndian() {
    const uint16_t v = 1;
    return *reinterpret_cast<const uint8_t*>(&v) == 1;
}

bool readExact(std::istream& in, void* dst, size_t n) {
    in.read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
    return bool(in);
}

bool writeExact(std::ostream& out, const void* src, size_t n) {
    out.write(static_cast<const char*>(src), static_cast<std::streamsize>(n));
    return bool(out);
}

bool readU32(std::istream& in, uint32_t& out) {
    uint8_t b[4];
    if (!readExact(in, b, sizeof(b))) return false;
    out = uint32_t(b[0]) |
          (uint32_t(b[1]) << 8) |
          (uint32_t(b[2]) << 16) |
          (uint32_t(b[3]) << 24);
    return true;
}

bool readU64(std::istream& in, uint64_t& out) {
    uint8_t b[8];
    if (!readExact(in, b, sizeof(b))) return false;
    out = uint64_t(b[0]) |
          (uint64_t(b[1]) << 8) |
          (uint64_t(b[2]) << 16) |
          (uint64_t(b[3]) << 24) |
          (uint64_t(b[4]) << 32) |
          (uint64_t(b[5]) << 40) |
          (uint64_t(b[6]) << 48) |
          (uint64_t(b[7]) << 56);
    return true;
}

bool writeU32(std::ostream& out, uint32_t v) {
    const uint8_t b[4] = {
        static_cast<uint8_t>(v & 0xffu),
        static_cast<uint8_t>((v >> 8) & 0xffu),
        static_cast<uint8_t>((v >> 16) & 0xffu),
        static_cast<uint8_t>((v >> 24) & 0xffu)
    };
    return writeExact(out, b, sizeof(b));
}

bool writeU64(std::ostream& out, uint64_t v) {
    const uint8_t b[8] = {
        static_cast<uint8_t>(v & 0xffu),
        static_cast<uint8_t>((v >> 8) & 0xffu),
        static_cast<uint8_t>((v >> 16) & 0xffu),
        static_cast<uint8_t>((v >> 24) & 0xffu),
        static_cast<uint8_t>((v >> 32) & 0xffu),
        static_cast<uint8_t>((v >> 40) & 0xffu),
        static_cast<uint8_t>((v >> 48) & 0xffu),
        static_cast<uint8_t>((v >> 56) & 0xffu)
    };
    return writeExact(out, b, sizeof(b));
}

bool readFloat(std::istream& in, float& out) {
    uint32_t bits = 0;
    if (!readU32(in, bits)) return false;
    static_assert(sizeof(float) == sizeof(uint32_t));
    std::memcpy(&out, &bits, sizeof(float));
    return true;
}

bool writeFloat(std::ostream& out, float v) {
    uint32_t bits = 0;
    static_assert(sizeof(float) == sizeof(uint32_t));
    std::memcpy(&bits, &v, sizeof(float));
    return writeU32(out, bits);
}

bool readString(std::istream& in, std::string& out) {
    uint32_t n = 0;
    if (!readU32(in, n) || n > kMaxBinaryHydrationStringBytes) return false;
    out.resize(n);
    return n == 0 || readExact(in, out.data(), n);
}

bool writeString(std::ostream& out, const std::string& s) {
    if (s.size() > std::numeric_limits<uint32_t>::max()) return false;
    if (!writeU32(out, static_cast<uint32_t>(s.size()))) return false;
    return s.empty() || writeExact(out, s.data(), s.size());
}
}

std::string fileSignature(const fs::path& p) {
    std::error_code size_ec;
    auto sz = fs::file_size(p, size_ec);
    if (size_ec) return "missing:" + p.filename().string();
    std::error_code time_ec;
    auto wt = fs::last_write_time(p, time_ec);
    if (time_ec) return std::to_string((unsigned long long)sz) + "_mtime_unavailable";
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
        const fs::path tmp_path = tempCachePathFor(cache_path);
        {
            std::ofstream out(tmp_path, std::ios::binary);
            if (!out) return;
            out.write((const char*)bin.data(), (std::streamsize)bin.size());
            out.flush();
            if (!out) {
                std::error_code remove_ec;
                fs::remove(tmp_path, remove_ec);
                return;
            }
        }
        std::error_code rename_ec;
        fs::rename(tmp_path, cache_path, rename_ec);
        if (rename_ec) {
            std::error_code remove_ec;
            fs::remove(cache_path, remove_ec);
            rename_ec.clear();
            fs::rename(tmp_path, cache_path, rename_ec);
            if (rename_ec) fs::remove(tmp_path, remove_ec);
        }
    }
}

bool loadBinaryHydrationCache(const fs::path& cache_path, const std::string& sig, std::vector<LayerDef::FeatureGeom>& out) {
    std::ifstream in(cache_path, std::ios::binary);
    if (!in) return false;
    bool ok = false;
    {
        TrimHeapOnScopeExit trim_on_exit;
        std::array<char, 8> magic{};
        if (!readExact(in, magic.data(), magic.size()) || magic != kHydrationBinaryMagic) return false;

        uint32_t version = 0;
        if (!readU32(in, version) || version != kHydrationBinaryVersion) return false;
        uint32_t endian_marker = 0;
        if (!readU32(in, endian_marker) || endian_marker != 0x01020304u) return false;

        std::string stored_sig;
        if (!readString(in, stored_sig) || stored_sig != sig) return false;

        uint64_t feature_count = 0;
        if (!readU64(in, feature_count) || feature_count > kMaxBinaryHydrationFeatures) return false;

        out.clear();
        out.reserve(static_cast<size_t>(feature_count));
        for (uint64_t fi = 0; fi < feature_count; ++fi) {
            LayerDef::FeatureGeom fg{};
            if (!readFloat(in, fg.extent.min_lon) ||
                !readFloat(in, fg.extent.min_lat) ||
                !readFloat(in, fg.extent.max_lon) ||
                !readFloat(in, fg.extent.max_lat)) {
                return false;
            }

            uint32_t ring_count = 0;
            if (!readU32(in, ring_count) || ring_count > kMaxBinaryHydrationRingsPerFeature) return false;
            fg.rings.reserve(ring_count);
            for (uint32_t ri = 0; ri < ring_count; ++ri) {
                uint32_t point_count = 0;
                if (!readU32(in, point_count) || point_count > kMaxBinaryHydrationPointsPerRing) return false;
                std::vector<ImVec2> ring;
                ring.reserve(point_count);
                for (uint32_t pi = 0; pi < point_count; ++pi) {
                    ImVec2 p;
                    if (!readFloat(in, p.x) || !readFloat(in, p.y)) return false;
                    ring.push_back(p);
                }
                fg.rings.push_back(std::move(ring));
            }

            uint32_t property_count = 0;
            if (!readU32(in, property_count) || property_count > kMaxBinaryHydrationPropertiesPerFeature) return false;
            fg.properties.reserve(property_count);
            for (uint32_t pi = 0; pi < property_count; ++pi) {
                std::string key;
                std::string value;
                if (!readString(in, key) || !readString(in, value)) return false;
                fg.properties.push_back({std::move(key), std::move(value)});
            }
            out.push_back(std::move(fg));
        }
        ok = true;
    }
    return ok;
}

void saveBinaryHydrationCache(const fs::path& cache_path, const std::string& sig, const std::vector<LayerDef::FeatureGeom>& features) {
    if (!hostIsLittleEndian()) return;
    fs::create_directories(cache_path.parent_path());
    const fs::path tmp_path = tempCachePathFor(cache_path);
    bool ok = false;
    {
        TrimHeapOnScopeExit trim_on_exit;
        std::ofstream out(tmp_path, std::ios::binary);
        if (!out) return;
        ok = writeExact(out, kHydrationBinaryMagic.data(), kHydrationBinaryMagic.size()) &&
             writeU32(out, kHydrationBinaryVersion) &&
             writeU32(out, 0x01020304u) &&
             writeString(out, sig) &&
             writeU64(out, static_cast<uint64_t>(features.size()));
        for (const auto& fg : features) {
            if (!ok) break;
            if (fg.rings.size() > std::numeric_limits<uint32_t>::max() ||
                fg.properties.size() > std::numeric_limits<uint32_t>::max()) {
                ok = false;
                break;
            }
            ok = writeFloat(out, fg.extent.min_lon) &&
                 writeFloat(out, fg.extent.min_lat) &&
                 writeFloat(out, fg.extent.max_lon) &&
                 writeFloat(out, fg.extent.max_lat) &&
                 writeU32(out, static_cast<uint32_t>(fg.rings.size()));
            for (const auto& ring : fg.rings) {
                if (!ok) break;
                if (ring.size() > std::numeric_limits<uint32_t>::max()) {
                    ok = false;
                    break;
                }
                ok = writeU32(out, static_cast<uint32_t>(ring.size()));
                for (const ImVec2& p : ring) {
                    if (!ok) break;
                    ok = writeFloat(out, p.x) && writeFloat(out, p.y);
                }
            }
            ok = ok && writeU32(out, static_cast<uint32_t>(fg.properties.size()));
            for (const auto& kv : fg.properties) {
                if (!ok) break;
                ok = writeString(out, kv.first) && writeString(out, kv.second);
            }
        }
        out.flush();
        ok = ok && bool(out);
    }
    if (!ok) {
        std::error_code remove_ec;
        fs::remove(tmp_path, remove_ec);
        return;
    }
    std::error_code rename_ec;
    fs::rename(tmp_path, cache_path, rename_ec);
    if (rename_ec) {
        std::error_code remove_ec;
        fs::remove(cache_path, remove_ec);
        rename_ec.clear();
        fs::rename(tmp_path, cache_path, rename_ec);
        if (rename_ec) fs::remove(tmp_path, remove_ec);
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

bool loadBinaryTriCache(const fs::path& cache_path, const std::string& sig, size_t count, std::vector<std::vector<uint32_t>>& out) {
    std::ifstream in(cache_path, std::ios::binary);
    if (!in) return false;
    bool ok = false;
    {
        TrimHeapOnScopeExit trim_on_exit;
        std::array<char, 8> magic{};
        if (!readExact(in, magic.data(), magic.size()) || magic != kTriBinaryMagic) return false;
        uint32_t version = 0;
        if (!readU32(in, version) || version != kTriBinaryVersion) return false;
        uint32_t endian_marker = 0;
        if (!readU32(in, endian_marker) || endian_marker != 0x01020304u) return false;

        std::string stored_sig;
        if (!readString(in, stored_sig) || stored_sig != sig) return false;

        uint64_t stored_count = 0;
        if (!readU64(in, stored_count) || stored_count != count) return false;

        out.clear();
        out.resize(count);
        for (size_t i = 0; i < count; ++i) {
            uint32_t tri_count = 0;
            if (!readU32(in, tri_count) || tri_count > kMaxBinaryTriIndicesPerFeature) return false;
            out[i].resize(tri_count);
            for (uint32_t j = 0; j < tri_count; ++j) {
                if (!readU32(in, out[i][j])) return false;
            }
        }
        ok = true;
    }
    return ok;
}

void saveBinaryTriCache(const fs::path& cache_path, const std::string& sig, const std::vector<std::vector<uint32_t>>& tris) {
    if (!hostIsLittleEndian()) return;
    fs::create_directories(cache_path.parent_path());
    const fs::path tmp_path = tempCachePathFor(cache_path);
    bool ok = false;
    {
        TrimHeapOnScopeExit trim_on_exit;
        std::ofstream out(tmp_path, std::ios::binary);
        if (!out) return;
        ok = writeExact(out, kTriBinaryMagic.data(), kTriBinaryMagic.size()) &&
             writeU32(out, kTriBinaryVersion) &&
             writeU32(out, 0x01020304u) &&
             writeString(out, sig) &&
             writeU64(out, static_cast<uint64_t>(tris.size()));
        for (const auto& one : tris) {
            if (!ok) break;
            if (one.size() > std::numeric_limits<uint32_t>::max()) {
                ok = false;
                break;
            }
            ok = writeU32(out, static_cast<uint32_t>(one.size()));
            for (uint32_t v : one) {
                if (!ok) break;
                ok = writeU32(out, v);
            }
        }
        out.flush();
        ok = ok && bool(out);
    }
    if (!ok) {
        std::error_code remove_ec;
        fs::remove(tmp_path, remove_ec);
        return;
    }
    std::error_code rename_ec;
    fs::rename(tmp_path, cache_path, rename_ec);
    if (rename_ec) {
        std::error_code remove_ec;
        fs::remove(cache_path, remove_ec);
        rename_ec.clear();
        fs::rename(tmp_path, cache_path, rename_ec);
        if (rename_ec) fs::remove(tmp_path, remove_ec);
    }
}
