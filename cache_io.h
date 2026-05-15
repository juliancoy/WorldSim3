#pragma once

#include "types.h"

#include <filesystem>
#include <cstdint>
#include <string>
#include <vector>

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

std::string fileSignature(const std::filesystem::path& p);

bool loadHydrationCache(
    const std::filesystem::path& cache_path,
    const std::string& sig,
    std::vector<LayerDef::FeatureGeom>& out);

void saveHydrationCache(
    const std::filesystem::path& cache_path,
    const std::string& sig,
    const std::vector<LayerDef::FeatureGeom>& features);

bool loadBinaryHydrationCache(
    const std::filesystem::path& cache_path,
    const std::string& sig,
    std::vector<LayerDef::FeatureGeom>& out);

void saveBinaryHydrationCache(
    const std::filesystem::path& cache_path,
    const std::string& sig,
    const std::vector<LayerDef::FeatureGeom>& features);

bool loadTriCache(
    const std::filesystem::path& cache_path,
    const std::string& sig,
    size_t count,
    std::vector<std::vector<uint32_t>>& out);

void saveTriCache(
    const std::filesystem::path& cache_path,
    const std::string& sig,
    const std::vector<std::vector<uint32_t>>& tris);

bool loadBinaryTriCache(
    const std::filesystem::path& cache_path,
    const std::string& sig,
    size_t count,
    std::vector<std::vector<uint32_t>>& out);

void saveBinaryTriCache(
    const std::filesystem::path& cache_path,
    const std::string& sig,
    const std::vector<std::vector<uint32_t>>& tris);

bool buildParcelRenderCacheBlob(
    const std::vector<LayerDef::FeatureGeom>& features,
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

bool loadBinaryCanonicalFeatureCollection(
    const std::filesystem::path& cache_path,
    const std::string& sig,
    std::vector<LayerDef::FeatureGeom>& out);
