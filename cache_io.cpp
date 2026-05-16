#include "cache_io.h"

#include "memory_utils.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>

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
constexpr std::array<char, 8> kParcelRenderBinaryMagic{{'W', 'S', '3', 'P', 'R', 'D', '1', '\0'}};
constexpr uint32_t kParcelRenderBinaryVersion = 2;
constexpr uint32_t kMaxParcelRenderVertices = 400000000u;
constexpr uint32_t kMaxParcelRenderIndices = 1200000000u;
constexpr uint32_t kMaxParcelRenderFeatures = 10000000u;
constexpr uint32_t kMaxParcelRenderChunks = 100000u;
constexpr std::array<char, 8> kCanonicalFeatureBinaryMagic{{'W', 'S', '3', 'C', 'A', 'N', '1', '\0'}};
constexpr uint32_t kCanonicalFeatureBinaryVersion = 1;
constexpr size_t kCanonicalFeatureSignatureBytes = 256;
constexpr size_t kRegionalParcelCompactionPropertyThreshold = 24;

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

bool isRegionalParcelHydrationCachePath(const fs::path& cache_path) {
    return cache_path.filename().string() == "regional_parcels.geojson.bin";
}

bool keepRegionalParcelRuntimeProperty(const std::string& key) {
    static const std::array<const char*, 31> keep_keys{{
        "jurisdiction",
        "source_file",
        "regional_parcel_id",
        "source_parcel_id",
        "account_id",
        "blocklot",
        "BLOCKLOT",
        "BLOCK_LOT",
        "BlockLot",
        "PIN",
        "pin",
        "BLOCK",
        "LOT",
        "block",
        "lot",
        "address",
        "owner",
        "owner_name",
        "land_value",
        "improvement_value",
        "current_value",
        "sale_price",
        "sale_date",
        "year_built",
        "sdat_link",
        "FULLADDR",
        "PROPERTY_ADDRESS",
        "OWNER_1",
        "OWNERNME1",
        "TAXBASE",
        "SDATLINK"
    }};
    return std::find(keep_keys.begin(), keep_keys.end(), key) != keep_keys.end();
}

uint32_t hydrationPropertyWriteCount(
    const fs::path& cache_path,
    const std::vector<std::pair<std::string, std::string>>& properties) {
    if (!isRegionalParcelHydrationCachePath(cache_path)) {
        return static_cast<uint32_t>(properties.size());
    }
    uint32_t count = 0;
    for (const auto& kv : properties) {
        if (keepRegionalParcelRuntimeProperty(kv.first)) ++count;
    }
    return count;
}

bool writeHydrationProperties(
    std::ostream& out,
    const fs::path& cache_path,
    const std::vector<std::pair<std::string, std::string>>& properties) {
    if (properties.size() > std::numeric_limits<uint32_t>::max()) return false;
    const bool slim_regional_parcels = isRegionalParcelHydrationCachePath(cache_path);
    const uint32_t count = hydrationPropertyWriteCount(cache_path, properties);
    bool ok = writeU32(out, count);
    for (const auto& kv : properties) {
        if (!ok) break;
        if (slim_regional_parcels && !keepRegionalParcelRuntimeProperty(kv.first)) continue;
        ok = writeString(out, kv.first) && writeString(out, kv.second);
    }
    return ok;
}

struct FlattenedParcelFeature {
    std::vector<ImVec2> vertices;
    std::vector<uint32_t> indices;
    std::vector<uint32_t> line_indices;
};

bool flattenParcelFeatureForRender(const LayerDef::FeatureGeom& fg, FlattenedParcelFeature& out) {
    out.vertices.clear();
    out.indices.clear();
    out.line_indices.clear();
    if (fg.rings.empty() || fg.triangles.empty()) return false;
    size_t total_points = 0;
    for (const auto& ring : fg.rings) total_points += ring.size();
    if (total_points == 0 || total_points > std::numeric_limits<uint32_t>::max()) return false;
    out.vertices.reserve(total_points);
    size_t ring_vertex_offset = 0;
    for (const auto& ring : fg.rings) {
        out.vertices.insert(out.vertices.end(), ring.begin(), ring.end());
        if (ring.size() >= 2) {
            out.line_indices.reserve(out.line_indices.size() + ring.size() * 2);
            for (size_t i = 0; i < ring.size(); ++i) {
                const uint32_t a = static_cast<uint32_t>(ring_vertex_offset + i);
                const uint32_t b = static_cast<uint32_t>(ring_vertex_offset + ((i + 1) % ring.size()));
                out.line_indices.push_back(a);
                out.line_indices.push_back(b);
            }
        }
        ring_vertex_offset += ring.size();
    }
    out.indices.reserve(fg.triangles.size());
    for (size_t ti = 0; ti + 2 < fg.triangles.size(); ti += 3) {
        const uint32_t a = fg.triangles[ti + 0];
        const uint32_t b = fg.triangles[ti + 1];
        const uint32_t c = fg.triangles[ti + 2];
        if (a >= out.vertices.size() || b >= out.vertices.size() || c >= out.vertices.size()) continue;
        out.indices.push_back(a);
        out.indices.push_back(b);
        out.indices.push_back(c);
    }
    return !out.vertices.empty() && !out.indices.empty() && !out.line_indices.empty();
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

bool loadBinaryCanonicalMetadata(const fs::path& cache_path, CanonicalFeatureCollectionMetadata& out) {
    std::ifstream in(cache_path, std::ios::binary);
    if (!in) return false;

    std::array<char, 8> magic{};
    if (!readExact(in, magic.data(), magic.size()) || magic != kCanonicalFeatureBinaryMagic) return false;

    CanonicalFeatureCollectionMetadata meta{};
    if (!readU32(in, meta.version) || meta.version != kCanonicalFeatureBinaryVersion) return false;
    if (!readU32(in, meta.endian_marker) || meta.endian_marker != 0x01020304u) return false;
    if (!readU64(in, meta.feature_count) || meta.feature_count > kMaxBinaryHydrationFeatures) return false;

    std::array<char, kCanonicalFeatureSignatureBytes> sig_buf{};
    if (!readExact(in, sig_buf.data(), sig_buf.size())) return false;
    meta.source_signature.assign(sig_buf.data(), std::find(sig_buf.begin(), sig_buf.end(), '\0'));

    std::error_code size_ec;
    meta.file_size_bytes = fs::file_size(cache_path, size_ec);
    if (size_ec) meta.file_size_bytes = 0;

    out = std::move(meta);
    return true;
}

bool resolveLayerSourceSignature(const fs::path& layer_path, std::string& out_sig, std::string* out_source_kind) {
    std::error_code exists_ec;
    if (fs::exists(layer_path, exists_ec) && !exists_ec) {
        out_sig = fileSignature(layer_path);
        if (out_source_kind) *out_source_kind = "geojson";
        return true;
    }

    const fs::path canonical_path = fs::path(layer_path.string() + ".canonical.bin");
    CanonicalFeatureCollectionMetadata meta;
    if (!loadBinaryCanonicalMetadata(canonical_path, meta) || meta.source_signature.empty()) return false;
    out_sig = meta.source_signature;
    if (out_source_kind) *out_source_kind = "canonical_binary";
    return true;
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
            ok = ok && writeHydrationProperties(out, cache_path, fg.properties);
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

bool binaryHydrationCacheShouldBeCompacted(
    const fs::path& cache_path,
    const std::vector<LayerDef::FeatureGeom>& features) {
    if (!isRegionalParcelHydrationCachePath(cache_path)) return false;
    for (const auto& fg : features) {
        if (fg.properties.size() > kRegionalParcelCompactionPropertyThreshold) return true;
    }
    return false;
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

bool buildParcelRenderCacheBlob(
    const std::vector<LayerDef::FeatureGeom>& features,
    const std::string& sig,
    ParcelRenderCacheBlob& out,
    size_t chunk_feature_budget) {
    if (chunk_feature_budget == 0 || chunk_feature_budget > std::numeric_limits<uint32_t>::max()) return false;
    out = ParcelRenderCacheBlob{};
    out.source_signature = sig;
    out.features.reserve(features.size());
    out.chunks.reserve((features.size() / chunk_feature_budget) + 1);

    FlattenedParcelFeature flattened;
    ParcelRenderChunkRecord current_chunk{};
    bool chunk_open = false;
    for (size_t feature_idx = 0; feature_idx < features.size(); ++feature_idx) {
        if (!flattenParcelFeatureForRender(features[feature_idx], flattened)) continue;
        if (out.vertices.size() + flattened.vertices.size() > kMaxParcelRenderVertices) return false;
        if (out.indices.size() + flattened.indices.size() > kMaxParcelRenderIndices) return false;
        if (out.line_indices.size() + flattened.line_indices.size() > kMaxParcelRenderIndices) return false;
        if (out.features.size() >= kMaxParcelRenderFeatures) return false;

        if (!chunk_open || current_chunk.feature_count >= chunk_feature_budget) {
            if (chunk_open) out.chunks.push_back(current_chunk);
            current_chunk = ParcelRenderChunkRecord{};
            current_chunk.chunk_idx = static_cast<uint32_t>(out.chunks.size());
            current_chunk.feature_offset = static_cast<uint32_t>(out.features.size());
            current_chunk.vertex_offset = static_cast<uint32_t>(out.vertices.size());
            current_chunk.index_offset = static_cast<uint32_t>(out.indices.size());
            current_chunk.line_index_offset = static_cast<uint32_t>(out.line_indices.size());
            current_chunk.min_lon = features[feature_idx].extent.min_lon;
            current_chunk.min_lat = features[feature_idx].extent.min_lat;
            current_chunk.max_lon = features[feature_idx].extent.max_lon;
            current_chunk.max_lat = features[feature_idx].extent.max_lat;
            chunk_open = true;
        } else {
            current_chunk.min_lon = std::min(current_chunk.min_lon, features[feature_idx].extent.min_lon);
            current_chunk.min_lat = std::min(current_chunk.min_lat, features[feature_idx].extent.min_lat);
            current_chunk.max_lon = std::max(current_chunk.max_lon, features[feature_idx].extent.max_lon);
            current_chunk.max_lat = std::max(current_chunk.max_lat, features[feature_idx].extent.max_lat);
        }

        ParcelRenderFeatureRecord rec{};
        rec.feature_idx = static_cast<uint32_t>(feature_idx);
        rec.vertex_offset = static_cast<uint32_t>(out.vertices.size());
        rec.vertex_count = static_cast<uint32_t>(flattened.vertices.size());
        rec.index_offset = static_cast<uint32_t>(out.indices.size());
        rec.index_count = static_cast<uint32_t>(flattened.indices.size());
        rec.line_index_offset = static_cast<uint32_t>(out.line_indices.size());
        rec.line_index_count = static_cast<uint32_t>(flattened.line_indices.size());
        rec.min_lon = features[feature_idx].extent.min_lon;
        rec.min_lat = features[feature_idx].extent.min_lat;
        rec.max_lon = features[feature_idx].extent.max_lon;
        rec.max_lat = features[feature_idx].extent.max_lat;

        out.vertices.insert(out.vertices.end(), flattened.vertices.begin(), flattened.vertices.end());
        out.vertex_feature_refs.insert(
            out.vertex_feature_refs.end(),
            flattened.vertices.size(),
            static_cast<uint32_t>(out.features.size()));
        for (uint32_t idx : flattened.indices) out.indices.push_back(idx + rec.vertex_offset);
        for (uint32_t idx : flattened.line_indices) out.line_indices.push_back(idx + rec.vertex_offset);
        out.features.push_back(rec);
        current_chunk.feature_count += 1;
        current_chunk.vertex_count += rec.vertex_count;
        current_chunk.index_count += rec.index_count;
        current_chunk.line_index_count += rec.line_index_count;
    }
    if (chunk_open) out.chunks.push_back(current_chunk);
    if (out.chunks.size() > kMaxParcelRenderChunks) return false;
    return true;
}

bool loadBinaryParcelRenderCache(const fs::path& cache_path, const std::string& sig, ParcelRenderCacheBlob& out) {
    std::ifstream in(cache_path, std::ios::binary);
    if (!in) return false;
    std::array<char, 8> magic{};
    if (!readExact(in, magic.data(), magic.size()) || magic != kParcelRenderBinaryMagic) return false;
    uint32_t version = 0;
    if (!readU32(in, version) || version != kParcelRenderBinaryVersion) return false;
    uint32_t endian_marker = 0;
    if (!readU32(in, endian_marker) || endian_marker != 0x01020304u) return false;
    std::string stored_sig;
    if (!readString(in, stored_sig) || stored_sig != sig) return false;

    uint32_t vertex_count = 0;
    uint32_t index_count = 0;
    uint32_t line_index_count = 0;
    uint32_t feature_count = 0;
    uint32_t chunk_count = 0;
    if (!readU32(in, vertex_count) || vertex_count > kMaxParcelRenderVertices) return false;
    if (!readU32(in, index_count) || index_count > kMaxParcelRenderIndices) return false;
    if (!readU32(in, line_index_count) || line_index_count > kMaxParcelRenderIndices) return false;
    if (!readU32(in, feature_count) || feature_count > kMaxParcelRenderFeatures) return false;
    if (!readU32(in, chunk_count) || chunk_count > kMaxParcelRenderChunks) return false;

    out = ParcelRenderCacheBlob{};
    out.source_signature = std::move(stored_sig);
    out.vertices.resize(vertex_count);
    out.vertex_feature_refs.resize(vertex_count);
    out.indices.resize(index_count);
    out.line_indices.resize(line_index_count);
    out.features.resize(feature_count);
    out.chunks.resize(chunk_count);

    for (uint32_t i = 0; i < vertex_count; ++i) {
        if (!readFloat(in, out.vertices[i].x) || !readFloat(in, out.vertices[i].y)) return false;
    }
    for (uint32_t i = 0; i < vertex_count; ++i) {
        if (!readU32(in, out.vertex_feature_refs[i]) || out.vertex_feature_refs[i] >= feature_count) return false;
    }
    for (uint32_t i = 0; i < index_count; ++i) {
        if (!readU32(in, out.indices[i]) || out.indices[i] >= vertex_count) return false;
    }
    for (uint32_t i = 0; i < line_index_count; ++i) {
        if (!readU32(in, out.line_indices[i]) || out.line_indices[i] >= vertex_count) return false;
    }
    for (uint32_t i = 0; i < feature_count; ++i) {
        auto& rec = out.features[i];
        if (!readU32(in, rec.feature_idx) ||
            !readU32(in, rec.vertex_offset) ||
            !readU32(in, rec.vertex_count) ||
            !readU32(in, rec.index_offset) ||
            !readU32(in, rec.index_count) ||
            !readU32(in, rec.line_index_offset) ||
            !readU32(in, rec.line_index_count) ||
            !readFloat(in, rec.min_lon) ||
            !readFloat(in, rec.min_lat) ||
            !readFloat(in, rec.max_lon) ||
            !readFloat(in, rec.max_lat)) {
            return false;
        }
        if (rec.vertex_offset + rec.vertex_count > vertex_count) return false;
        if (rec.index_offset + rec.index_count > index_count) return false;
        if (rec.line_index_offset + rec.line_index_count > line_index_count) return false;
    }
    for (uint32_t i = 0; i < chunk_count; ++i) {
        auto& rec = out.chunks[i];
        if (!readU32(in, rec.chunk_idx) ||
            !readU32(in, rec.feature_offset) ||
            !readU32(in, rec.feature_count) ||
            !readU32(in, rec.vertex_offset) ||
            !readU32(in, rec.vertex_count) ||
            !readU32(in, rec.index_offset) ||
            !readU32(in, rec.index_count) ||
            !readU32(in, rec.line_index_offset) ||
            !readU32(in, rec.line_index_count) ||
            !readFloat(in, rec.min_lon) ||
            !readFloat(in, rec.min_lat) ||
            !readFloat(in, rec.max_lon) ||
            !readFloat(in, rec.max_lat)) {
            return false;
        }
        if (rec.feature_offset + rec.feature_count > feature_count) return false;
        if (rec.vertex_offset + rec.vertex_count > vertex_count) return false;
        if (rec.index_offset + rec.index_count > index_count) return false;
        if (rec.line_index_offset + rec.line_index_count > line_index_count) return false;
    }
    return true;
}

void saveBinaryParcelRenderCache(const fs::path& cache_path, const ParcelRenderCacheBlob& blob) {
    if (!hostIsLittleEndian()) return;
    if (blob.vertices.size() > std::numeric_limits<uint32_t>::max() ||
        blob.vertex_feature_refs.size() != blob.vertices.size() ||
        blob.indices.size() > std::numeric_limits<uint32_t>::max() ||
        blob.line_indices.size() > std::numeric_limits<uint32_t>::max() ||
        blob.features.size() > std::numeric_limits<uint32_t>::max() ||
        blob.chunks.size() > std::numeric_limits<uint32_t>::max()) {
        return;
    }
    fs::create_directories(cache_path.parent_path());
    const fs::path tmp_path = tempCachePathFor(cache_path);
    bool ok = false;
    {
        std::ofstream out(tmp_path, std::ios::binary);
        if (!out) return;
        ok = writeExact(out, kParcelRenderBinaryMagic.data(), kParcelRenderBinaryMagic.size()) &&
             writeU32(out, kParcelRenderBinaryVersion) &&
             writeU32(out, 0x01020304u) &&
             writeString(out, blob.source_signature) &&
             writeU32(out, static_cast<uint32_t>(blob.vertices.size())) &&
             writeU32(out, static_cast<uint32_t>(blob.indices.size())) &&
             writeU32(out, static_cast<uint32_t>(blob.line_indices.size())) &&
             writeU32(out, static_cast<uint32_t>(blob.features.size())) &&
             writeU32(out, static_cast<uint32_t>(blob.chunks.size()));
        for (const auto& v : blob.vertices) ok = ok && writeFloat(out, v.x) && writeFloat(out, v.y);
        for (uint32_t ref : blob.vertex_feature_refs) ok = ok && writeU32(out, ref);
        for (uint32_t i : blob.indices) ok = ok && writeU32(out, i);
        for (uint32_t i : blob.line_indices) ok = ok && writeU32(out, i);
        for (const auto& rec : blob.features) {
            ok = ok &&
                 writeU32(out, rec.feature_idx) &&
                 writeU32(out, rec.vertex_offset) &&
                 writeU32(out, rec.vertex_count) &&
                 writeU32(out, rec.index_offset) &&
                 writeU32(out, rec.index_count) &&
                 writeU32(out, rec.line_index_offset) &&
                 writeU32(out, rec.line_index_count) &&
                 writeFloat(out, rec.min_lon) &&
                 writeFloat(out, rec.min_lat) &&
                 writeFloat(out, rec.max_lon) &&
                 writeFloat(out, rec.max_lat);
        }
        for (const auto& rec : blob.chunks) {
            ok = ok &&
                 writeU32(out, rec.chunk_idx) &&
                 writeU32(out, rec.feature_offset) &&
                 writeU32(out, rec.feature_count) &&
                 writeU32(out, rec.vertex_offset) &&
                 writeU32(out, rec.vertex_count) &&
                 writeU32(out, rec.index_offset) &&
                 writeU32(out, rec.index_count) &&
                 writeU32(out, rec.line_index_offset) &&
                 writeU32(out, rec.line_index_count) &&
                 writeFloat(out, rec.min_lon) &&
                 writeFloat(out, rec.min_lat) &&
                 writeFloat(out, rec.max_lon) &&
                 writeFloat(out, rec.max_lat);
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

void saveBinaryCanonicalFeatureCollection(
    const fs::path& cache_path,
    const std::string& sig,
    const std::vector<LayerDef::FeatureGeom>& features) {
    if (!hostIsLittleEndian()) return;
    fs::create_directories(cache_path.parent_path());
    const fs::path tmp_path = tempCachePathFor(cache_path);
    bool ok = false;
    {
        TrimHeapOnScopeExit trim_on_exit;
        std::ofstream out(tmp_path, std::ios::binary);
        if (!out) return;
        ok = writeExact(out, kCanonicalFeatureBinaryMagic.data(), kCanonicalFeatureBinaryMagic.size()) &&
             writeU32(out, kCanonicalFeatureBinaryVersion) &&
             writeU32(out, 0x01020304u) &&
             writeU64(out, static_cast<uint64_t>(features.size()));

        std::array<char, kCanonicalFeatureSignatureBytes> sig_buf{};
        const size_t sig_bytes = std::min(sig.size(), sig_buf.size() - 1);
        std::memcpy(sig_buf.data(), sig.data(), sig_bytes);
        ok = ok && writeExact(out, sig_buf.data(), sig_buf.size());

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

bool loadBinaryCanonicalFeatureCollection(const fs::path& cache_path, const std::string& sig, std::vector<LayerDef::FeatureGeom>& out) {
    std::ifstream in(cache_path, std::ios::binary);
    if (!in) return false;

    CanonicalFeatureCollectionMetadata meta;
    if (!loadBinaryCanonicalMetadata(cache_path, meta) || meta.source_signature != sig) return false;
    in.clear();
    in.seekg(static_cast<std::streamoff>(8 + 4 + 4 + 8 + kCanonicalFeatureSignatureBytes), std::ios::beg);
    if (!in) return false;

    out.clear();
    out.reserve(static_cast<size_t>(meta.feature_count));
    for (uint64_t fi = 0; fi < meta.feature_count; ++fi) {
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
    return true;
}
