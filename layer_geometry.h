#pragma once

#include "types.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

std::string jsonValueToString(const nlohmann::json& v);
std::vector<LayerDef::FeatureGeom> extractFeatureGeoms(const nlohmann::json& geom);
std::vector<LayerDef::FeatureGeom> loadLayerPointsFromFile(const std::filesystem::path& full_path);
bool pointInRing(const std::vector<ImVec2>& ring, float x, float y);
bool pointInFeature(const LayerDef::FeatureGeom& fg, float lon, float lat);
int lodRingStepForZoom(int zoom);
bool implausibleHydrationCache(const std::string& file, size_t feature_count);
void hydrateLayerBatches(
    const std::filesystem::path& full_path,
    size_t batch_size,
    const std::atomic<bool>& stop_flag,
    const std::function<bool()>& should_continue,
    const std::function<void(std::vector<LayerDef::FeatureGeom>&&, bool, bool, const std::string&)>& emit);
void loadLayerPoints(LayerDef& layer, const std::filesystem::path& root);
std::vector<uint32_t> triangulateRings(const std::vector<std::vector<ImVec2>>& rings);
void appendRingScreenPointsLod(
    const std::vector<ImVec2>& ring,
    int ring_step,
    int math_zoom,
    const ImVec2& center_world,
    const ImVec2& origin,
    const ImVec2& size,
    double zoom_scale,
    std::vector<ImVec2>& out);
void appendWorldRingScreenPointsLod(
    const std::vector<ImVec2>& world_ring,
    int ring_step,
    const ImVec2& center_world,
    const ImVec2& origin,
    const ImVec2& size,
    double zoom_scale,
    std::vector<ImVec2>& out);
