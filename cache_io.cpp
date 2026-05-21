#include "cache_io.h"

#include "app_utils.h"
#include "feature_props.h"
#include "layer_geometry.h"
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
constexpr uint32_t kHydrationBinaryVersion = 2;
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
constexpr uint32_t kCanonicalFeatureBinaryVersion = 2;
constexpr std::array<char, 8> kOwnerSearchBinaryMagic{{'W', 'S', '3', 'O', 'S', 'C', '1', '\0'}};
constexpr uint32_t kOwnerSearchBinaryVersion = 1;
constexpr std::array<char, 8> kAddressSearchBinaryMagic{{'W', 'S', '3', 'A', 'S', 'C', '1', '\0'}};
constexpr uint32_t kAddressSearchBinaryVersion = 1;
constexpr size_t kCanonicalFeatureSignatureBytes = 256;

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

bool writeHydrationProperties(
    std::ostream& out,
    const fs::path&,
    const std::vector<std::pair<std::string, std::string>>& properties) {
    if (properties.size() > std::numeric_limits<uint32_t>::max()) return false;
    bool ok = writeU32(out, static_cast<uint32_t>(properties.size()));
    for (const auto& kv : properties) {
        if (!ok) break;
        ok = writeString(out, kv.first) && writeString(out, kv.second);
    }
    return ok;
}

bool writePolylinePaths(std::ostream& out, const std::vector<std::vector<ImVec2>>& paths) {
    if (paths.size() > std::numeric_limits<uint32_t>::max()) return false;
    bool ok = writeU32(out, static_cast<uint32_t>(paths.size()));
    for (const auto& path : paths) {
        if (!ok) break;
        if (path.size() > std::numeric_limits<uint32_t>::max()) return false;
        ok = writeU32(out, static_cast<uint32_t>(path.size()));
        for (const ImVec2& p : path) {
            if (!ok) break;
            ok = writeFloat(out, p.x) && writeFloat(out, p.y);
        }
    }
    return ok;
}

bool readPolylinePaths(std::istream& in, std::vector<std::vector<ImVec2>>& paths) {
    paths.clear();
    uint32_t path_count = 0;
    if (!readU32(in, path_count) || path_count > kMaxBinaryHydrationRingsPerFeature) return false;
    paths.reserve(path_count);
    for (uint32_t pi = 0; pi < path_count; ++pi) {
        uint32_t point_count = 0;
        if (!readU32(in, point_count) || point_count > kMaxBinaryHydrationPointsPerRing) return false;
        std::vector<ImVec2> path;
        path.reserve(point_count);
        for (uint32_t vi = 0; vi < point_count; ++vi) {
            ImVec2 p;
            if (!readFloat(in, p.x) || !readFloat(in, p.y)) return false;
            path.push_back(p);
        }
        paths.push_back(std::move(path));
    }
    return true;
}

const FeaturePropertyPairs* propertiesForFeatureRecord(
    const LayerDef::FeatureRecord& fg,
    const std::vector<LayerDef::FeatureProperties>* feature_properties,
    size_t feature_idx) {
    if (feature_properties && feature_idx < feature_properties->size()) {
        return &(*feature_properties)[feature_idx].values;
    }
    return getPropertyPairs(fg);
}

struct FlattenedParcelFeature {
    std::vector<ImVec2> vertices;
    std::vector<uint32_t> indices;
    std::vector<uint32_t> line_indices;
};

bool flattenParcelFeatureForRender(const LayerDef::FeatureRecord& fg, FlattenedParcelFeature& out) {
    out.vertices.clear();
    out.indices.clear();
    out.line_indices.clear();
    if (fg.rings.empty()) return false;
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
    return !out.vertices.empty() && !out.line_indices.empty();
}
}

const char* geometryArtifactClassName(GeometryArtifactClass cls) {
    switch (cls) {
        case GeometryArtifactClass::Point: return "point";
        case GeometryArtifactClass::Polyline: return "polyline";
        case GeometryArtifactClass::Polygon: return "polygon";
        case GeometryArtifactClass::Unknown: break;
    }
    return "unknown";
}

const char* geometryArtifactFileSuffix(GeometryArtifactClass cls) {
    switch (cls) {
        case GeometryArtifactClass::Point: return ".point.bin";
        case GeometryArtifactClass::Polyline: return ".polyline.bin";
        case GeometryArtifactClass::Polygon: return ".polygon.bin";
        case GeometryArtifactClass::Unknown: break;
    }
    return ".unknown.bin";
}

std::filesystem::path geometryArtifactCachePathForLayerFile(
    const fs::path& root,
    const std::string& layer_file,
    GeometryArtifactClass cls) {
    return root / "data" / "cache" / "geometry" /
           (layerArtifactBasenameForFile(layer_file) + geometryArtifactFileSuffix(cls));
}

bool buildPointGeometryArtifact(
    const LayerDef& layer,
    const std::vector<LayerDef::FeatureRecord>& features,
    const std::string& sig,
    PointGeometryArtifact& out,
    size_t chunk_feature_budget) {
    if (chunk_feature_budget == 0 || chunk_feature_budget > std::numeric_limits<uint32_t>::max()) return false;
    out = PointGeometryArtifact{};
    out.header.geometry_class = GeometryArtifactClass::Point;
    out.header.source_signature = sig;
    out.features.reserve(features.size());
    out.positions.reserve(features.size());
    out.feature_refs.reserve(features.size());
    out.chunks.reserve((features.size() / chunk_feature_budget) + 1);

    GeometryArtifactChunkRecord current_chunk{};
    bool chunk_open = false;
    for (size_t feature_idx = 0; feature_idx < features.size(); ++feature_idx) {
        const auto& fg = features[feature_idx];
        const float lon = (fg.extent.min_lon + fg.extent.max_lon) * 0.5f;
        const float lat = (fg.extent.min_lat + fg.extent.max_lat) * 0.5f;
        if (!chunk_open || current_chunk.feature_count >= chunk_feature_budget) {
            if (chunk_open) out.chunks.push_back(current_chunk);
            current_chunk = GeometryArtifactChunkRecord{};
            current_chunk.chunk_idx = static_cast<uint32_t>(out.chunks.size());
            current_chunk.feature_offset = static_cast<uint32_t>(out.features.size());
            current_chunk.vertex_offset = static_cast<uint32_t>(out.positions.size());
            current_chunk.min_lon = fg.extent.min_lon;
            current_chunk.min_lat = fg.extent.min_lat;
            current_chunk.max_lon = fg.extent.max_lon;
            current_chunk.max_lat = fg.extent.max_lat;
            chunk_open = true;
        } else {
            current_chunk.min_lon = std::min(current_chunk.min_lon, fg.extent.min_lon);
            current_chunk.min_lat = std::min(current_chunk.min_lat, fg.extent.min_lat);
            current_chunk.max_lon = std::max(current_chunk.max_lon, fg.extent.max_lon);
            current_chunk.max_lat = std::max(current_chunk.max_lat, fg.extent.max_lat);
        }

        GeometryArtifactFeatureRecord rec{};
        rec.feature_idx = static_cast<uint32_t>(feature_idx);
        rec.feature_id = featureStableIdForLayerFeature(layer, fg, feature_idx);
        rec.vertex_offset = static_cast<uint32_t>(out.positions.size());
        rec.vertex_count = 1;
        rec.min_lon = fg.extent.min_lon;
        rec.min_lat = fg.extent.min_lat;
        rec.max_lon = fg.extent.max_lon;
        rec.max_lat = fg.extent.max_lat;
        out.positions.push_back(ImVec2(lon, lat));
        out.feature_refs.push_back(static_cast<uint32_t>(out.features.size()));
        out.features.push_back(std::move(rec));
        current_chunk.feature_count += 1;
        current_chunk.vertex_count += 1;
    }
    if (chunk_open) out.chunks.push_back(current_chunk);
    out.header.feature_count = out.features.size();
    out.header.chunk_count = out.chunks.size();
    return true;
}

bool buildPolylineGeometryArtifact(
    const LayerDef& layer,
    const std::vector<LayerDef::FeatureRecord>& features,
    const std::string& sig,
    PolylineGeometryArtifact& out,
    size_t chunk_feature_budget) {
    if (chunk_feature_budget == 0 || chunk_feature_budget > std::numeric_limits<uint32_t>::max()) return false;
    out = PolylineGeometryArtifact{};
    out.header.geometry_class = GeometryArtifactClass::Polyline;
    out.header.source_signature = sig;
    out.features.reserve(features.size());
    out.chunks.reserve((features.size() / chunk_feature_budget) + 1);

    GeometryArtifactChunkRecord current_chunk{};
    bool chunk_open = false;
    for (size_t feature_idx = 0; feature_idx < features.size(); ++feature_idx) {
        const auto& fg = features[feature_idx];
        if (fg.paths.empty()) continue;

        size_t path_vertex_count = 0;
        for (const auto& path : fg.paths) path_vertex_count += path.size();
        if (path_vertex_count == 0 || path_vertex_count > std::numeric_limits<uint32_t>::max()) continue;
        const std::vector<uint32_t> line_indices = flattenLinePathsToSegmentIndices(fg.paths);
        if (line_indices.empty()) continue;
        if (out.vertices.size() + path_vertex_count > std::numeric_limits<uint32_t>::max()) return false;
        if (out.line_indices.size() + line_indices.size() > std::numeric_limits<uint32_t>::max()) return false;

        if (!chunk_open || current_chunk.feature_count >= chunk_feature_budget) {
            if (chunk_open) out.chunks.push_back(current_chunk);
            current_chunk = GeometryArtifactChunkRecord{};
            current_chunk.chunk_idx = static_cast<uint32_t>(out.chunks.size());
            current_chunk.feature_offset = static_cast<uint32_t>(out.features.size());
            current_chunk.vertex_offset = static_cast<uint32_t>(out.vertices.size());
            current_chunk.index_offset = static_cast<uint32_t>(out.line_indices.size());
            current_chunk.min_lon = fg.extent.min_lon;
            current_chunk.min_lat = fg.extent.min_lat;
            current_chunk.max_lon = fg.extent.max_lon;
            current_chunk.max_lat = fg.extent.max_lat;
            chunk_open = true;
        } else {
            current_chunk.min_lon = std::min(current_chunk.min_lon, fg.extent.min_lon);
            current_chunk.min_lat = std::min(current_chunk.min_lat, fg.extent.min_lat);
            current_chunk.max_lon = std::max(current_chunk.max_lon, fg.extent.max_lon);
            current_chunk.max_lat = std::max(current_chunk.max_lat, fg.extent.max_lat);
        }

        GeometryArtifactFeatureRecord rec{};
        rec.feature_idx = static_cast<uint32_t>(feature_idx);
        rec.feature_id = featureStableIdForLayerFeature(layer, fg, feature_idx);
        rec.vertex_offset = static_cast<uint32_t>(out.vertices.size());
        rec.vertex_count = static_cast<uint32_t>(path_vertex_count);
        rec.index_offset = static_cast<uint32_t>(out.line_indices.size());
        rec.index_count = static_cast<uint32_t>(line_indices.size());
        rec.min_lon = fg.extent.min_lon;
        rec.min_lat = fg.extent.min_lat;
        rec.max_lon = fg.extent.max_lon;
        rec.max_lat = fg.extent.max_lat;

        const uint32_t feature_ref = static_cast<uint32_t>(out.features.size());
        for (const auto& path : fg.paths) {
            out.vertices.insert(out.vertices.end(), path.begin(), path.end());
            out.feature_refs.insert(out.feature_refs.end(), path.size(), feature_ref);
        }
        for (uint32_t idx : line_indices) out.line_indices.push_back(idx + rec.vertex_offset);
        out.features.push_back(std::move(rec));

        current_chunk.feature_count += 1;
        current_chunk.vertex_count += static_cast<uint32_t>(path_vertex_count);
        current_chunk.index_count += static_cast<uint32_t>(line_indices.size());
    }
    if (chunk_open) out.chunks.push_back(current_chunk);
    out.header.feature_count = out.features.size();
    out.header.chunk_count = out.chunks.size();
    return !out.vertices.empty() && !out.line_indices.empty();
}

bool buildPolygonGeometryArtifact(
    const LayerDef& layer,
    const std::vector<LayerDef::FeatureRecord>& features,
    const std::string& sig,
    PolygonGeometryArtifact& out,
    size_t chunk_feature_budget) {
    if (chunk_feature_budget == 0 || chunk_feature_budget > std::numeric_limits<uint32_t>::max()) return false;
    out = PolygonGeometryArtifact{};
    out.header.geometry_class = GeometryArtifactClass::Polygon;
    out.header.source_signature = sig;
    out.features.reserve(features.size());
    out.chunks.reserve((features.size() / chunk_feature_budget) + 1);

    GeometryArtifactChunkRecord current_chunk{};
    bool chunk_open = false;
    for (size_t feature_idx = 0; feature_idx < features.size(); ++feature_idx) {
        const auto& fg = features[feature_idx];
        if (fg.rings.empty()) continue;

        FlattenedParcelFeature flattened;
        if (!flattenParcelFeatureForRender(fg, flattened)) continue;
        if (out.vertices.size() + flattened.vertices.size() > std::numeric_limits<uint32_t>::max()) return false;
        if (out.fill_indices.size() + flattened.indices.size() > std::numeric_limits<uint32_t>::max()) return false;
        if (out.line_indices.size() + flattened.line_indices.size() > std::numeric_limits<uint32_t>::max()) return false;

        if (!chunk_open || current_chunk.feature_count >= chunk_feature_budget) {
            if (chunk_open) out.chunks.push_back(current_chunk);
            current_chunk = GeometryArtifactChunkRecord{};
            current_chunk.chunk_idx = static_cast<uint32_t>(out.chunks.size());
            current_chunk.feature_offset = static_cast<uint32_t>(out.features.size());
            current_chunk.vertex_offset = static_cast<uint32_t>(out.vertices.size());
            current_chunk.index_offset = static_cast<uint32_t>(out.fill_indices.size());
            current_chunk.aux_index_offset = static_cast<uint32_t>(out.line_indices.size());
            current_chunk.min_lon = fg.extent.min_lon;
            current_chunk.min_lat = fg.extent.min_lat;
            current_chunk.max_lon = fg.extent.max_lon;
            current_chunk.max_lat = fg.extent.max_lat;
            chunk_open = true;
        } else {
            current_chunk.min_lon = std::min(current_chunk.min_lon, fg.extent.min_lon);
            current_chunk.min_lat = std::min(current_chunk.min_lat, fg.extent.min_lat);
            current_chunk.max_lon = std::max(current_chunk.max_lon, fg.extent.max_lon);
            current_chunk.max_lat = std::max(current_chunk.max_lat, fg.extent.max_lat);
        }

        GeometryArtifactFeatureRecord rec{};
        rec.feature_idx = static_cast<uint32_t>(feature_idx);
        rec.feature_id = featureStableIdForLayerFeature(layer, fg, feature_idx);
        rec.vertex_offset = static_cast<uint32_t>(out.vertices.size());
        rec.vertex_count = static_cast<uint32_t>(flattened.vertices.size());
        rec.index_offset = static_cast<uint32_t>(out.fill_indices.size());
        rec.index_count = static_cast<uint32_t>(flattened.indices.size());
        rec.aux_index_offset = static_cast<uint32_t>(out.line_indices.size());
        rec.aux_index_count = static_cast<uint32_t>(flattened.line_indices.size());
        rec.min_lon = fg.extent.min_lon;
        rec.min_lat = fg.extent.min_lat;
        rec.max_lon = fg.extent.max_lon;
        rec.max_lat = fg.extent.max_lat;

        const uint32_t feature_ref = static_cast<uint32_t>(out.features.size());
        out.vertices.insert(out.vertices.end(), flattened.vertices.begin(), flattened.vertices.end());
        out.feature_refs.insert(out.feature_refs.end(), flattened.vertices.size(), feature_ref);
        for (uint32_t idx : flattened.indices) out.fill_indices.push_back(idx + rec.vertex_offset);
        for (uint32_t idx : flattened.line_indices) out.line_indices.push_back(idx + rec.vertex_offset);
        out.features.push_back(std::move(rec));

        current_chunk.feature_count += 1;
        current_chunk.vertex_count += static_cast<uint32_t>(flattened.vertices.size());
        current_chunk.index_count += static_cast<uint32_t>(flattened.indices.size());
        current_chunk.aux_index_count += static_cast<uint32_t>(flattened.line_indices.size());
    }

    if (chunk_open) out.chunks.push_back(current_chunk);
    out.header.feature_count = out.features.size();
    out.header.chunk_count = out.chunks.size();
    return !out.vertices.empty() && !out.line_indices.empty();
}

bool loadBinaryPointGeometryArtifact(
    const fs::path& cache_path,
    const std::string& sig,
    PointGeometryArtifact& out) {
    std::ifstream in(cache_path, std::ios::binary);
    if (!in) return false;
    std::array<char, 8> magic{};
    if (!readExact(in, magic.data(), magic.size())) return false;
    static constexpr std::array<char, 8> kPointArtifactMagic{{'W','S','3','P','N','T','1','\0'}};
    if (magic != kPointArtifactMagic) return false;

    GeometryArtifactHeader hdr{};
    uint32_t geometry_class = 0;
    if (!readU32(in, hdr.version) ||
        !readU32(in, hdr.endian_marker) ||
        !readString(in, hdr.source_signature) ||
        !readU32(in, geometry_class) ||
        !readU64(in, hdr.feature_count) ||
        !readU64(in, hdr.chunk_count)) {
        return false;
    }
    hdr.geometry_class = static_cast<GeometryArtifactClass>(geometry_class);
    if (hdr.version != 1 || hdr.endian_marker != 0x01020304u || hdr.geometry_class != GeometryArtifactClass::Point) return false;
    if (hdr.source_signature != sig) return false;

    uint32_t point_count = 0;
    uint32_t feature_ref_count = 0;
    if (!readU32(in, point_count) || !readU32(in, feature_ref_count) || point_count != feature_ref_count) return false;

    out = PointGeometryArtifact{};
    out.header = hdr;
    out.positions.resize(point_count);
    out.feature_refs.resize(feature_ref_count);
    out.features.resize((size_t)hdr.feature_count);
    out.chunks.resize((size_t)hdr.chunk_count);

    for (uint32_t i = 0; i < point_count; ++i) {
        if (!readFloat(in, out.positions[i].x) || !readFloat(in, out.positions[i].y)) return false;
    }
    for (uint32_t i = 0; i < feature_ref_count; ++i) {
        if (!readU32(in, out.feature_refs[i])) return false;
        if (out.feature_refs[i] >= out.features.size()) return false;
    }
    for (auto& rec : out.features) {
        uint32_t feature_id_len = 0;
        if (!readU32(in, rec.feature_idx) ||
            !readString(in, rec.feature_id) ||
            !readU32(in, rec.vertex_offset) ||
            !readU32(in, rec.vertex_count) ||
            !readU32(in, rec.index_offset) ||
            !readU32(in, rec.index_count) ||
            !readU32(in, rec.aux_index_offset) ||
            !readU32(in, rec.aux_index_count) ||
            !readFloat(in, rec.min_lon) ||
            !readFloat(in, rec.min_lat) ||
            !readFloat(in, rec.max_lon) ||
            !readFloat(in, rec.max_lat)) return false;
        (void)feature_id_len;
    }
    for (auto& rec : out.chunks) {
        if (!readU32(in, rec.chunk_idx) ||
            !readU32(in, rec.feature_offset) ||
            !readU32(in, rec.feature_count) ||
            !readU32(in, rec.vertex_offset) ||
            !readU32(in, rec.vertex_count) ||
            !readU32(in, rec.index_offset) ||
            !readU32(in, rec.index_count) ||
            !readU32(in, rec.aux_index_offset) ||
            !readU32(in, rec.aux_index_count) ||
            !readFloat(in, rec.min_lon) ||
            !readFloat(in, rec.min_lat) ||
            !readFloat(in, rec.max_lon) ||
            !readFloat(in, rec.max_lat)) return false;
    }
    return true;
}

bool loadBinaryPolylineGeometryArtifact(
    const fs::path& cache_path,
    const std::string& sig,
    PolylineGeometryArtifact& out) {
    std::ifstream in(cache_path, std::ios::binary);
    if (!in) return false;
    static constexpr std::array<char, 8> kPolylineArtifactMagic{{'W','S','3','L','I','N','1','\0'}};
    std::array<char, 8> magic{};
    if (!readExact(in, magic.data(), magic.size()) || magic != kPolylineArtifactMagic) return false;

    GeometryArtifactHeader hdr{};
    uint32_t geometry_class = 0;
    if (!readU32(in, hdr.version) ||
        !readU32(in, hdr.endian_marker) ||
        !readString(in, hdr.source_signature) ||
        !readU32(in, geometry_class) ||
        !readU64(in, hdr.feature_count) ||
        !readU64(in, hdr.chunk_count)) return false;
    hdr.geometry_class = static_cast<GeometryArtifactClass>(geometry_class);
    if (hdr.version != 1 || hdr.endian_marker != 0x01020304u || hdr.geometry_class != GeometryArtifactClass::Polyline) return false;
    if (hdr.source_signature != sig) return false;

    uint32_t vertex_count = 0;
    uint32_t feature_ref_count = 0;
    uint32_t line_index_count = 0;
    if (!readU32(in, vertex_count) ||
        !readU32(in, feature_ref_count) ||
        !readU32(in, line_index_count) ||
        vertex_count != feature_ref_count) return false;

    out = PolylineGeometryArtifact{};
    out.header = hdr;
    out.vertices.resize(vertex_count);
    out.feature_refs.resize(feature_ref_count);
    out.line_indices.resize(line_index_count);
    out.features.resize((size_t)hdr.feature_count);
    out.chunks.resize((size_t)hdr.chunk_count);

    for (uint32_t i = 0; i < vertex_count; ++i) {
        if (!readFloat(in, out.vertices[i].x) || !readFloat(in, out.vertices[i].y)) return false;
    }
    for (uint32_t i = 0; i < feature_ref_count; ++i) {
        if (!readU32(in, out.feature_refs[i]) || out.feature_refs[i] >= out.features.size()) return false;
    }
    for (uint32_t i = 0; i < line_index_count; ++i) {
        if (!readU32(in, out.line_indices[i]) || out.line_indices[i] >= out.vertices.size()) return false;
    }
    for (auto& rec : out.features) {
        if (!readU32(in, rec.feature_idx) ||
            !readString(in, rec.feature_id) ||
            !readU32(in, rec.vertex_offset) ||
            !readU32(in, rec.vertex_count) ||
            !readU32(in, rec.index_offset) ||
            !readU32(in, rec.index_count) ||
            !readU32(in, rec.aux_index_offset) ||
            !readU32(in, rec.aux_index_count) ||
            !readFloat(in, rec.min_lon) ||
            !readFloat(in, rec.min_lat) ||
            !readFloat(in, rec.max_lon) ||
            !readFloat(in, rec.max_lat)) return false;
    }
    for (auto& rec : out.chunks) {
        if (!readU32(in, rec.chunk_idx) ||
            !readU32(in, rec.feature_offset) ||
            !readU32(in, rec.feature_count) ||
            !readU32(in, rec.vertex_offset) ||
            !readU32(in, rec.vertex_count) ||
            !readU32(in, rec.index_offset) ||
            !readU32(in, rec.index_count) ||
            !readU32(in, rec.aux_index_offset) ||
            !readU32(in, rec.aux_index_count) ||
            !readFloat(in, rec.min_lon) ||
            !readFloat(in, rec.min_lat) ||
            !readFloat(in, rec.max_lon) ||
            !readFloat(in, rec.max_lat)) return false;
    }
    return true;
}

bool loadBinaryPolygonGeometryArtifact(
    const fs::path& cache_path,
    const std::string& sig,
    PolygonGeometryArtifact& out) {
    std::ifstream in(cache_path, std::ios::binary);
    if (!in) return false;
    static constexpr std::array<char, 8> kPolygonArtifactMagic{{'W','S','3','P','L','Y','1','\0'}};
    std::array<char, 8> magic{};
    if (!readExact(in, magic.data(), magic.size()) || magic != kPolygonArtifactMagic) return false;

    GeometryArtifactHeader hdr{};
    uint32_t geometry_class = 0;
    if (!readU32(in, hdr.version) ||
        !readU32(in, hdr.endian_marker) ||
        !readString(in, hdr.source_signature) ||
        !readU32(in, geometry_class) ||
        !readU64(in, hdr.feature_count) ||
        !readU64(in, hdr.chunk_count)) {
        return false;
    }
    hdr.geometry_class = static_cast<GeometryArtifactClass>(geometry_class);
    if (hdr.version != 1 || hdr.endian_marker != 0x01020304u || hdr.geometry_class != GeometryArtifactClass::Polygon) return false;
    if (hdr.source_signature != sig) return false;

    uint32_t vertex_count = 0;
    uint32_t feature_ref_count = 0;
    uint32_t fill_index_count = 0;
    uint32_t line_index_count = 0;
    if (!readU32(in, vertex_count) ||
        !readU32(in, feature_ref_count) ||
        !readU32(in, fill_index_count) ||
        !readU32(in, line_index_count) ||
        vertex_count != feature_ref_count) {
        return false;
    }

    out = PolygonGeometryArtifact{};
    out.header = hdr;
    out.vertices.resize(vertex_count);
    out.feature_refs.resize(feature_ref_count);
    out.fill_indices.resize(fill_index_count);
    out.line_indices.resize(line_index_count);
    out.features.resize((size_t)hdr.feature_count);
    out.chunks.resize((size_t)hdr.chunk_count);

    for (uint32_t i = 0; i < vertex_count; ++i) {
        if (!readFloat(in, out.vertices[i].x) || !readFloat(in, out.vertices[i].y)) return false;
    }
    for (uint32_t i = 0; i < feature_ref_count; ++i) {
        if (!readU32(in, out.feature_refs[i]) || out.feature_refs[i] >= out.features.size()) return false;
    }
    for (uint32_t i = 0; i < fill_index_count; ++i) {
        if (!readU32(in, out.fill_indices[i]) || out.fill_indices[i] >= out.vertices.size()) return false;
    }
    for (uint32_t i = 0; i < line_index_count; ++i) {
        if (!readU32(in, out.line_indices[i]) || out.line_indices[i] >= out.vertices.size()) return false;
    }
    for (auto& rec : out.features) {
        if (!readU32(in, rec.feature_idx) ||
            !readString(in, rec.feature_id) ||
            !readU32(in, rec.vertex_offset) ||
            !readU32(in, rec.vertex_count) ||
            !readU32(in, rec.index_offset) ||
            !readU32(in, rec.index_count) ||
            !readU32(in, rec.aux_index_offset) ||
            !readU32(in, rec.aux_index_count) ||
            !readFloat(in, rec.min_lon) ||
            !readFloat(in, rec.min_lat) ||
            !readFloat(in, rec.max_lon) ||
            !readFloat(in, rec.max_lat)) return false;
    }
    for (auto& rec : out.chunks) {
        if (!readU32(in, rec.chunk_idx) ||
            !readU32(in, rec.feature_offset) ||
            !readU32(in, rec.feature_count) ||
            !readU32(in, rec.vertex_offset) ||
            !readU32(in, rec.vertex_count) ||
            !readU32(in, rec.index_offset) ||
            !readU32(in, rec.index_count) ||
            !readU32(in, rec.aux_index_offset) ||
            !readU32(in, rec.aux_index_count) ||
            !readFloat(in, rec.min_lon) ||
            !readFloat(in, rec.min_lat) ||
            !readFloat(in, rec.max_lon) ||
            !readFloat(in, rec.max_lat)) return false;
    }
    return true;
}

void saveBinaryPointGeometryArtifact(
    const fs::path& cache_path,
    const PointGeometryArtifact& artifact) {
    if (!hostIsLittleEndian()) return;
    fs::create_directories(cache_path.parent_path());
    const fs::path tmp_path = tempCachePathFor(cache_path);
    static constexpr std::array<char, 8> kPointArtifactMagic{{'W','S','3','P','N','T','1','\0'}};
    bool ok = false;
    {
        std::ofstream out(tmp_path, std::ios::binary);
        if (!out) return;
        ok = writeExact(out, kPointArtifactMagic.data(), kPointArtifactMagic.size()) &&
             writeU32(out, artifact.header.version) &&
             writeU32(out, artifact.header.endian_marker) &&
             writeString(out, artifact.header.source_signature) &&
             writeU32(out, static_cast<uint32_t>(artifact.header.geometry_class)) &&
             writeU64(out, artifact.header.feature_count) &&
             writeU64(out, artifact.header.chunk_count) &&
             writeU32(out, static_cast<uint32_t>(artifact.positions.size())) &&
             writeU32(out, static_cast<uint32_t>(artifact.feature_refs.size()));
        for (const auto& p : artifact.positions) ok = ok && writeFloat(out, p.x) && writeFloat(out, p.y);
        for (uint32_t ref : artifact.feature_refs) ok = ok && writeU32(out, ref);
        for (const auto& rec : artifact.features) {
            ok = ok &&
                 writeU32(out, rec.feature_idx) &&
                 writeString(out, rec.feature_id) &&
                 writeU32(out, rec.vertex_offset) &&
                 writeU32(out, rec.vertex_count) &&
                 writeU32(out, rec.index_offset) &&
                 writeU32(out, rec.index_count) &&
                 writeU32(out, rec.aux_index_offset) &&
                 writeU32(out, rec.aux_index_count) &&
                 writeFloat(out, rec.min_lon) &&
                 writeFloat(out, rec.min_lat) &&
                 writeFloat(out, rec.max_lon) &&
                 writeFloat(out, rec.max_lat);
        }
        for (const auto& rec : artifact.chunks) {
            ok = ok &&
                 writeU32(out, rec.chunk_idx) &&
                 writeU32(out, rec.feature_offset) &&
                 writeU32(out, rec.feature_count) &&
                 writeU32(out, rec.vertex_offset) &&
                 writeU32(out, rec.vertex_count) &&
                 writeU32(out, rec.index_offset) &&
                 writeU32(out, rec.index_count) &&
                 writeU32(out, rec.aux_index_offset) &&
                 writeU32(out, rec.aux_index_count) &&
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

void saveBinaryPolylineGeometryArtifact(
    const fs::path& cache_path,
    const PolylineGeometryArtifact& artifact) {
    if (!hostIsLittleEndian()) return;
    fs::create_directories(cache_path.parent_path());
    const fs::path tmp_path = tempCachePathFor(cache_path);
    static constexpr std::array<char, 8> kPolylineArtifactMagic{{'W','S','3','L','I','N','1','\0'}};
    bool ok = false;
    {
        std::ofstream out(tmp_path, std::ios::binary);
        if (!out) return;
        ok = writeExact(out, kPolylineArtifactMagic.data(), kPolylineArtifactMagic.size()) &&
             writeU32(out, artifact.header.version) &&
             writeU32(out, artifact.header.endian_marker) &&
             writeString(out, artifact.header.source_signature) &&
             writeU32(out, static_cast<uint32_t>(artifact.header.geometry_class)) &&
             writeU64(out, artifact.header.feature_count) &&
             writeU64(out, artifact.header.chunk_count) &&
             writeU32(out, static_cast<uint32_t>(artifact.vertices.size())) &&
             writeU32(out, static_cast<uint32_t>(artifact.feature_refs.size())) &&
             writeU32(out, static_cast<uint32_t>(artifact.line_indices.size()));
        for (const auto& p : artifact.vertices) ok = ok && writeFloat(out, p.x) && writeFloat(out, p.y);
        for (uint32_t ref : artifact.feature_refs) ok = ok && writeU32(out, ref);
        for (uint32_t idx : artifact.line_indices) ok = ok && writeU32(out, idx);
        for (const auto& rec : artifact.features) {
            ok = ok &&
                 writeU32(out, rec.feature_idx) &&
                 writeString(out, rec.feature_id) &&
                 writeU32(out, rec.vertex_offset) &&
                 writeU32(out, rec.vertex_count) &&
                 writeU32(out, rec.index_offset) &&
                 writeU32(out, rec.index_count) &&
                 writeU32(out, rec.aux_index_offset) &&
                 writeU32(out, rec.aux_index_count) &&
                 writeFloat(out, rec.min_lon) &&
                 writeFloat(out, rec.min_lat) &&
                 writeFloat(out, rec.max_lon) &&
                 writeFloat(out, rec.max_lat);
        }
        for (const auto& rec : artifact.chunks) {
            ok = ok &&
                 writeU32(out, rec.chunk_idx) &&
                 writeU32(out, rec.feature_offset) &&
                 writeU32(out, rec.feature_count) &&
                 writeU32(out, rec.vertex_offset) &&
                 writeU32(out, rec.vertex_count) &&
                 writeU32(out, rec.index_offset) &&
                 writeU32(out, rec.index_count) &&
                 writeU32(out, rec.aux_index_offset) &&
                 writeU32(out, rec.aux_index_count) &&
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

void saveBinaryPolygonGeometryArtifact(
    const fs::path& cache_path,
    const PolygonGeometryArtifact& artifact) {
    if (!hostIsLittleEndian()) return;
    fs::create_directories(cache_path.parent_path());
    const fs::path tmp_path = tempCachePathFor(cache_path);
    static constexpr std::array<char, 8> kPolygonArtifactMagic{{'W','S','3','P','L','Y','1','\0'}};
    bool ok = false;
    {
        std::ofstream out(tmp_path, std::ios::binary);
        if (!out) return;
        ok = writeExact(out, kPolygonArtifactMagic.data(), kPolygonArtifactMagic.size()) &&
             writeU32(out, artifact.header.version) &&
             writeU32(out, artifact.header.endian_marker) &&
             writeString(out, artifact.header.source_signature) &&
             writeU32(out, static_cast<uint32_t>(artifact.header.geometry_class)) &&
             writeU64(out, artifact.header.feature_count) &&
             writeU64(out, artifact.header.chunk_count) &&
             writeU32(out, static_cast<uint32_t>(artifact.vertices.size())) &&
             writeU32(out, static_cast<uint32_t>(artifact.feature_refs.size())) &&
             writeU32(out, static_cast<uint32_t>(artifact.fill_indices.size())) &&
             writeU32(out, static_cast<uint32_t>(artifact.line_indices.size()));
        for (const auto& p : artifact.vertices) ok = ok && writeFloat(out, p.x) && writeFloat(out, p.y);
        for (uint32_t ref : artifact.feature_refs) ok = ok && writeU32(out, ref);
        for (uint32_t idx : artifact.fill_indices) ok = ok && writeU32(out, idx);
        for (uint32_t idx : artifact.line_indices) ok = ok && writeU32(out, idx);
        for (const auto& rec : artifact.features) {
            ok = ok &&
                 writeU32(out, rec.feature_idx) &&
                 writeString(out, rec.feature_id) &&
                 writeU32(out, rec.vertex_offset) &&
                 writeU32(out, rec.vertex_count) &&
                 writeU32(out, rec.index_offset) &&
                 writeU32(out, rec.index_count) &&
                 writeU32(out, rec.aux_index_offset) &&
                 writeU32(out, rec.aux_index_count) &&
                 writeFloat(out, rec.min_lon) &&
                 writeFloat(out, rec.min_lat) &&
                 writeFloat(out, rec.max_lon) &&
                 writeFloat(out, rec.max_lat);
        }
        for (const auto& rec : artifact.chunks) {
            ok = ok &&
                 writeU32(out, rec.chunk_idx) &&
                 writeU32(out, rec.feature_offset) &&
                 writeU32(out, rec.feature_count) &&
                 writeU32(out, rec.vertex_offset) &&
                 writeU32(out, rec.vertex_count) &&
                 writeU32(out, rec.index_offset) &&
                 writeU32(out, rec.index_count) &&
                 writeU32(out, rec.aux_index_offset) &&
                 writeU32(out, rec.aux_index_count) &&
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
    const fs::path canonical_path = fs::path(layer_path.string() + ".canonical.bin");
    CanonicalFeatureCollectionMetadata meta;
    if (!loadBinaryCanonicalMetadata(canonical_path, meta) || meta.source_signature.empty()) return false;
    out_sig = meta.source_signature;
    if (out_source_kind) *out_source_kind = "canonical_binary";
    return true;
}

bool loadBinaryHydrationCache(
    const fs::path& cache_path,
    const std::string& sig,
    std::vector<LayerDef::FeatureRecord>& out,
    std::vector<LayerDef::FeatureProperties>* out_feature_properties) {
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
        if (out_feature_properties) out_feature_properties->clear();
        out.reserve(static_cast<size_t>(feature_count));
        if (out_feature_properties) out_feature_properties->reserve(static_cast<size_t>(feature_count));
        for (uint64_t fi = 0; fi < feature_count; ++fi) {
            LayerDef::FeatureRecord fg{};
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
            if (!readPolylinePaths(in, fg.paths)) return false;

            uint32_t property_count = 0;
            if (!readU32(in, property_count) || property_count > kMaxBinaryHydrationPropertiesPerFeature) return false;
            LayerDef::FeatureProperties props;
            props.values.reserve(property_count);
            for (uint32_t pi = 0; pi < property_count; ++pi) {
                std::string key;
                std::string value;
                if (!readString(in, key) || !readString(in, value)) return false;
                props.values.push_back({std::move(key), std::move(value)});
            }
            out.push_back(std::move(fg));
            setTransientFeatureProperties(out.back(), props.values);
            if (out_feature_properties) out_feature_properties->push_back(std::move(props));
        }
        ok = true;
    }
    return ok;
}

void saveBinaryHydrationCache(
    const fs::path& cache_path,
    const std::string& sig,
    const std::vector<LayerDef::FeatureRecord>& features,
    const std::vector<LayerDef::FeatureProperties>* feature_properties) {
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
        for (size_t fi = 0; fi < features.size(); ++fi) {
            const auto& fg = features[fi];
            const FeaturePropertyPairs* props = propertiesForFeatureRecord(fg, feature_properties, fi);
            if (!ok) break;
            if (fg.rings.size() > std::numeric_limits<uint32_t>::max() ||
                fg.paths.size() > std::numeric_limits<uint32_t>::max() ||
                (props && props->size() > std::numeric_limits<uint32_t>::max())) {
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
            ok = ok && writePolylinePaths(out, fg.paths);
            ok = ok && writeHydrationProperties(out, cache_path, props ? *props : FeaturePropertyPairs{});
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
    const std::vector<LayerDef::FeatureRecord>& features,
    const std::vector<LayerDef::FeatureProperties>* feature_properties) {
    (void)cache_path;
    (void)features;
    (void)feature_properties;
    return false;
}

bool buildParcelRenderCacheBlob(
    const std::vector<LayerDef::FeatureRecord>& features,
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
    const std::vector<LayerDef::FeatureRecord>& features,
    const std::vector<LayerDef::FeatureProperties>* feature_properties) {
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

        for (size_t fi = 0; fi < features.size(); ++fi) {
            const auto& fg = features[fi];
            const FeaturePropertyPairs* props = propertiesForFeatureRecord(fg, feature_properties, fi);
            if (!ok) break;
            if (fg.rings.size() > std::numeric_limits<uint32_t>::max() ||
                fg.paths.size() > std::numeric_limits<uint32_t>::max() ||
                (props && props->size() > std::numeric_limits<uint32_t>::max())) {
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
            ok = ok && writePolylinePaths(out, fg.paths);
            ok = ok && writeU32(out, static_cast<uint32_t>(props ? props->size() : 0));
            if (props) for (const auto& kv : *props) {
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

bool loadBinaryCanonicalFeatureCollection(
    const fs::path& cache_path,
    const std::string& sig,
    std::vector<LayerDef::FeatureRecord>& out,
    std::vector<LayerDef::FeatureProperties>* out_feature_properties) {
    std::ifstream in(cache_path, std::ios::binary);
    if (!in) return false;

    CanonicalFeatureCollectionMetadata meta;
    if (!loadBinaryCanonicalMetadata(cache_path, meta) || meta.source_signature != sig) return false;
    in.clear();
    in.seekg(static_cast<std::streamoff>(8 + 4 + 4 + 8 + kCanonicalFeatureSignatureBytes), std::ios::beg);
    if (!in) return false;

    out.clear();
    if (out_feature_properties) out_feature_properties->clear();
    out.reserve(static_cast<size_t>(meta.feature_count));
    if (out_feature_properties) out_feature_properties->reserve(static_cast<size_t>(meta.feature_count));
    for (uint64_t fi = 0; fi < meta.feature_count; ++fi) {
        LayerDef::FeatureRecord fg{};
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
        if (!readPolylinePaths(in, fg.paths)) return false;

        uint32_t property_count = 0;
        if (!readU32(in, property_count) || property_count > kMaxBinaryHydrationPropertiesPerFeature) return false;
        LayerDef::FeatureProperties props;
        props.values.reserve(property_count);
        for (uint32_t pi = 0; pi < property_count; ++pi) {
            std::string key;
            std::string value;
            if (!readString(in, key) || !readString(in, value)) return false;
            props.values.push_back({std::move(key), std::move(value)});
        }
        out.push_back(std::move(fg));
        setTransientFeatureProperties(out.back(), props.values);
        if (out_feature_properties) out_feature_properties->push_back(std::move(props));
    }
    return true;
}

bool loadCanonicalLayerFeatureCollection(
    const fs::path& root,
    const std::string& layer_file,
    const std::string& sig,
    std::vector<LayerDef::FeatureRecord>& out,
    std::vector<LayerDef::FeatureProperties>* out_feature_properties) {
    return loadBinaryCanonicalFeatureCollection(
        canonicalLayerPathForFile(root, layer_file),
        sig,
        out,
        out_feature_properties);
}

bool loadBinaryOwnerSearchCache(
    const fs::path& cache_path,
    const std::string& parcel_sig,
    const std::string& real_property_sig,
    std::vector<std::string>& parcel_owner_search,
    std::vector<std::string>& real_property_owner_search) {
    std::ifstream in(cache_path, std::ios::binary);
    if (!in) return false;

    std::array<char, 8> magic{};
    uint32_t version = 0;
    uint32_t endian = 0;
    uint64_t parcel_count = 0;
    uint64_t real_property_count = 0;
    std::string stored_parcel_sig;
    std::string stored_real_property_sig;
    if (!readExact(in, magic.data(), magic.size()) ||
        magic != kOwnerSearchBinaryMagic ||
        !readU32(in, version) ||
        version != kOwnerSearchBinaryVersion ||
        !readU32(in, endian) ||
        endian != 0x01020304u ||
        !readU64(in, parcel_count) ||
        !readU64(in, real_property_count) ||
        !readString(in, stored_parcel_sig) ||
        !readString(in, stored_real_property_sig) ||
        stored_parcel_sig != parcel_sig ||
        stored_real_property_sig != real_property_sig) {
        return false;
    }
    if (parcel_count > kMaxBinaryHydrationFeatures ||
        real_property_count > kMaxBinaryHydrationFeatures) {
        return false;
    }

    parcel_owner_search.clear();
    real_property_owner_search.clear();
    parcel_owner_search.reserve(static_cast<size_t>(parcel_count));
    real_property_owner_search.reserve(static_cast<size_t>(real_property_count));

    for (uint64_t i = 0; i < parcel_count; ++i) {
        std::string value;
        if (!readString(in, value)) return false;
        parcel_owner_search.push_back(std::move(value));
    }
    for (uint64_t i = 0; i < real_property_count; ++i) {
        std::string value;
        if (!readString(in, value)) return false;
        real_property_owner_search.push_back(std::move(value));
    }
    return true;
}

void saveBinaryOwnerSearchCache(
    const fs::path& cache_path,
    const std::string& parcel_sig,
    const std::string& real_property_sig,
    const std::vector<std::string>& parcel_owner_search,
    const std::vector<std::string>& real_property_owner_search) {
    if (!hostIsLittleEndian()) return;
    if (parcel_owner_search.size() > std::numeric_limits<uint64_t>::max() ||
        real_property_owner_search.size() > std::numeric_limits<uint64_t>::max()) {
        return;
    }

    fs::create_directories(cache_path.parent_path());
    const fs::path tmp_path = tempCachePathFor(cache_path);
    bool ok = false;
    {
        TrimHeapOnScopeExit trim_on_exit;
        std::ofstream out(tmp_path, std::ios::binary);
        if (!out) return;
        ok = writeExact(out, kOwnerSearchBinaryMagic.data(), kOwnerSearchBinaryMagic.size()) &&
             writeU32(out, kOwnerSearchBinaryVersion) &&
             writeU32(out, 0x01020304u) &&
             writeU64(out, static_cast<uint64_t>(parcel_owner_search.size())) &&
             writeU64(out, static_cast<uint64_t>(real_property_owner_search.size())) &&
             writeString(out, parcel_sig) &&
             writeString(out, real_property_sig);
        for (const std::string& value : parcel_owner_search) {
            if (!ok) break;
            ok = writeString(out, value);
        }
        for (const std::string& value : real_property_owner_search) {
            if (!ok) break;
            ok = writeString(out, value);
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

bool loadBinaryAddressSearchCache(
    const fs::path& cache_path,
    const std::string& parcel_sig,
    std::vector<std::string>& parcel_address_search) {
    std::ifstream in(cache_path, std::ios::binary);
    if (!in) return false;

    std::array<char, 8> magic{};
    uint32_t version = 0;
    uint32_t endian = 0;
    uint64_t parcel_count = 0;
    std::string stored_parcel_sig;
    if (!readExact(in, magic.data(), magic.size()) ||
        magic != kAddressSearchBinaryMagic ||
        !readU32(in, version) ||
        version != kAddressSearchBinaryVersion ||
        !readU32(in, endian) ||
        endian != 0x01020304u ||
        !readU64(in, parcel_count) ||
        !readString(in, stored_parcel_sig) ||
        stored_parcel_sig != parcel_sig ||
        parcel_count > kMaxBinaryHydrationFeatures) {
        return false;
    }

    parcel_address_search.clear();
    parcel_address_search.reserve(static_cast<size_t>(parcel_count));
    for (uint64_t i = 0; i < parcel_count; ++i) {
        std::string value;
        if (!readString(in, value)) return false;
        parcel_address_search.push_back(std::move(value));
    }
    return true;
}

void saveBinaryAddressSearchCache(
    const fs::path& cache_path,
    const std::string& parcel_sig,
    const std::vector<std::string>& parcel_address_search) {
    if (!hostIsLittleEndian()) return;
    if (parcel_address_search.size() > std::numeric_limits<uint64_t>::max()) return;

    fs::create_directories(cache_path.parent_path());
    const fs::path tmp_path = tempCachePathFor(cache_path);
    bool ok = false;
    {
        TrimHeapOnScopeExit trim_on_exit;
        std::ofstream out(tmp_path, std::ios::binary);
        if (!out) return;
        ok = writeExact(out, kAddressSearchBinaryMagic.data(), kAddressSearchBinaryMagic.size()) &&
             writeU32(out, kAddressSearchBinaryVersion) &&
             writeU32(out, 0x01020304u) &&
             writeU64(out, static_cast<uint64_t>(parcel_address_search.size())) &&
             writeString(out, parcel_sig);
        for (const std::string& value : parcel_address_search) {
            if (!ok) break;
            ok = writeString(out, value);
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
