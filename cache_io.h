#pragma once

#include "types.h"

#include <filesystem>
#include <cstdint>
#include <string>
#include <vector>

struct GeometryArtifactHeader {
    uint32_t version = 1;
    uint32_t endian_marker = 0x01020304u;
    GeometryArtifactClass geometry_class = GeometryArtifactClass::Unknown;
    uint64_t feature_count = 0;
    uint64_t chunk_count = 0;
    std::string source_signature;
};

struct GeometryArtifactFeatureRecord {
    uint32_t feature_idx = 0;
    std::string feature_id;
    uint32_t vertex_offset = 0;
    uint32_t vertex_count = 0;
    uint32_t index_offset = 0;
    uint32_t index_count = 0;
    uint32_t aux_index_offset = 0;
    uint32_t aux_index_count = 0;
    float min_lon = 0.0f;
    float min_lat = 0.0f;
    float max_lon = 0.0f;
    float max_lat = 0.0f;
};

struct GeometryArtifactChunkRecord {
    uint32_t chunk_idx = 0;
    uint32_t feature_offset = 0;
    uint32_t feature_count = 0;
    uint32_t vertex_offset = 0;
    uint32_t vertex_count = 0;
    uint32_t index_offset = 0;
    uint32_t index_count = 0;
    uint32_t aux_index_offset = 0;
    uint32_t aux_index_count = 0;
    float min_lon = 0.0f;
    float min_lat = 0.0f;
    float max_lon = 0.0f;
    float max_lat = 0.0f;
};

struct PointGeometryArtifact {
    GeometryArtifactHeader header;
    std::vector<ImVec2> positions;
    std::vector<uint32_t> feature_refs;
    std::vector<GeometryArtifactFeatureRecord> features;
    std::vector<GeometryArtifactChunkRecord> chunks;
};

struct PolylineGeometryArtifact {
    GeometryArtifactHeader header;
    std::vector<ImVec2> vertices;
    std::vector<uint32_t> feature_refs;
    std::vector<uint32_t> line_indices;
    std::vector<GeometryArtifactFeatureRecord> features;
    std::vector<GeometryArtifactChunkRecord> chunks;
};

struct PolygonGeometryArtifact {
    GeometryArtifactHeader header;
    std::vector<ImVec2> vertices;
    std::vector<uint32_t> feature_refs;
    std::vector<uint32_t> fill_indices;
    std::vector<uint32_t> line_indices;
    std::vector<GeometryArtifactFeatureRecord> features;
    std::vector<GeometryArtifactChunkRecord> chunks;
};

struct ParcelRenderFeatureRecord {
    uint32_t feature_idx = 0;
    uint32_t vertex_offset = 0;
    uint32_t vertex_count = 0;
    uint32_t index_offset = 0;
    uint32_t index_count = 0;
    uint32_t line_index_offset = 0;
    uint32_t line_index_count = 0;
    float min_lon = 0.0f;
    float min_lat = 0.0f;
    float max_lon = 0.0f;
    float max_lat = 0.0f;
};

struct ParcelRenderChunkRecord {
    uint32_t chunk_idx = 0;
    uint32_t feature_offset = 0;
    uint32_t feature_count = 0;
    uint32_t vertex_offset = 0;
    uint32_t vertex_count = 0;
    uint32_t index_offset = 0;
    uint32_t index_count = 0;
    uint32_t line_index_offset = 0;
    uint32_t line_index_count = 0;
    float min_lon = 0.0f;
    float min_lat = 0.0f;
    float max_lon = 0.0f;
    float max_lat = 0.0f;
};

struct ParcelRenderCacheBlob {
    std::string source_signature;
    std::vector<ImVec2> vertices;
    std::vector<uint32_t> vertex_feature_refs;
    std::vector<uint32_t> indices;
    std::vector<uint32_t> line_indices;
    std::vector<ParcelRenderFeatureRecord> features;
    std::vector<ParcelRenderChunkRecord> chunks;
};

struct CanonicalFeatureCollectionMetadata {
    uint32_t version = 0;
    uint32_t endian_marker = 0;
    uint64_t feature_count = 0;
    std::string source_signature;
    uintmax_t file_size_bytes = 0;
};

const char* geometryArtifactClassName(GeometryArtifactClass cls);
const char* geometryArtifactFileSuffix(GeometryArtifactClass cls);
std::filesystem::path geometryArtifactCachePathForLayerFile(
    const std::filesystem::path& root,
    const std::string& layer_file,
    GeometryArtifactClass cls);
bool buildPointGeometryArtifact(
    const LayerDef& layer,
    const std::vector<LayerDef::FeatureRecord>& features,
    const std::string& sig,
    PointGeometryArtifact& out,
    size_t chunk_feature_budget = 4096);
bool loadBinaryPointGeometryArtifact(
    const std::filesystem::path& cache_path,
    const std::string& sig,
    PointGeometryArtifact& out);
void saveBinaryPointGeometryArtifact(
    const std::filesystem::path& cache_path,
    const PointGeometryArtifact& artifact);
bool buildPolylineGeometryArtifact(
    const LayerDef& layer,
    const std::vector<LayerDef::FeatureRecord>& features,
    const std::string& sig,
    PolylineGeometryArtifact& out,
    size_t chunk_feature_budget = 4096);
bool loadBinaryPolylineGeometryArtifact(
    const std::filesystem::path& cache_path,
    const std::string& sig,
    PolylineGeometryArtifact& out);
void saveBinaryPolylineGeometryArtifact(
    const std::filesystem::path& cache_path,
    const PolylineGeometryArtifact& artifact);
bool buildPolygonGeometryArtifact(
    const LayerDef& layer,
    const std::vector<LayerDef::FeatureRecord>& features,
    const std::string& sig,
    PolygonGeometryArtifact& out,
    size_t chunk_feature_budget = 4096);
bool loadBinaryPolygonGeometryArtifact(
    const std::filesystem::path& cache_path,
    const std::string& sig,
    PolygonGeometryArtifact& out);
void saveBinaryPolygonGeometryArtifact(
    const std::filesystem::path& cache_path,
    const PolygonGeometryArtifact& artifact);

std::string fileSignature(const std::filesystem::path& p);
bool resolveLayerSourceSignature(
    const std::filesystem::path& layer_path,
    std::string& out_sig,
    std::string* out_source_kind = nullptr);

bool loadBinaryHydrationCache(
    const std::filesystem::path& cache_path,
    const std::string& sig,
    std::vector<LayerDef::FeatureRecord>& out,
    std::vector<LayerDef::FeatureProperties>* out_feature_properties = nullptr);

void saveBinaryHydrationCache(
    const std::filesystem::path& cache_path,
    const std::string& sig,
    const std::vector<LayerDef::FeatureRecord>& features,
    const std::vector<LayerDef::FeatureProperties>* feature_properties = nullptr);

bool binaryHydrationCacheShouldBeCompacted(
    const std::filesystem::path& cache_path,
    const std::vector<LayerDef::FeatureRecord>& features,
    const std::vector<LayerDef::FeatureProperties>* feature_properties = nullptr);

bool buildParcelRenderCacheBlob(
    const std::vector<LayerDef::FeatureRecord>& features,
    const std::string& sig,
    ParcelRenderCacheBlob& out,
    size_t chunk_feature_budget = 4096);

bool loadBinaryParcelRenderCache(
    const std::filesystem::path& cache_path,
    const std::string& sig,
    ParcelRenderCacheBlob& out);

void saveBinaryParcelRenderCache(
    const std::filesystem::path& cache_path,
    const ParcelRenderCacheBlob& blob);

bool loadBinaryCanonicalMetadata(
    const std::filesystem::path& cache_path,
    CanonicalFeatureCollectionMetadata& out);

void saveBinaryCanonicalFeatureCollection(
    const std::filesystem::path& cache_path,
    const std::string& sig,
    const std::vector<LayerDef::FeatureRecord>& features,
    const std::vector<LayerDef::FeatureProperties>* feature_properties = nullptr);

bool loadBinaryCanonicalFeatureCollection(
    const std::filesystem::path& cache_path,
    const std::string& sig,
    std::vector<LayerDef::FeatureRecord>& out,
    std::vector<LayerDef::FeatureProperties>* out_feature_properties = nullptr);

bool loadCanonicalLayerFeatureCollection(
    const std::filesystem::path& root,
    const std::string& layer_file,
    const std::string& sig,
    std::vector<LayerDef::FeatureRecord>& out,
    std::vector<LayerDef::FeatureProperties>* out_feature_properties = nullptr);

bool loadBinaryOwnerSearchCache(
    const std::filesystem::path& cache_path,
    const std::string& parcel_sig,
    const std::string& real_property_sig,
    std::vector<std::string>& parcel_owner_search,
    std::vector<std::string>& real_property_owner_search);

void saveBinaryOwnerSearchCache(
    const std::filesystem::path& cache_path,
    const std::string& parcel_sig,
    const std::string& real_property_sig,
    const std::vector<std::string>& parcel_owner_search,
    const std::vector<std::string>& real_property_owner_search);

bool loadBinaryAddressSearchCache(
    const std::filesystem::path& cache_path,
    const std::string& parcel_sig,
    std::vector<std::string>& parcel_address_search);

void saveBinaryAddressSearchCache(
    const std::filesystem::path& cache_path,
    const std::string& parcel_sig,
    const std::vector<std::string>& parcel_address_search);
