#pragma once

#include "types.h"

#include <filesystem>
#include <string>
#include <vector>

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
