#pragma once

#include "dataset_library.h"
#include "types.h"

#include <filesystem>
#include <string>
#include <vector>

bool layerHasImportSource(const LayerDef& layer);
VersionedDownloadResult downloadOrImportLayer(
    const LayerDef& layer,
    const std::filesystem::path& out_path,
    const std::filesystem::path& root,
    const DownloadProgressCallback& on_progress = {});
std::vector<std::filesystem::path> layerLocalSsotArtifactPaths(
    const std::filesystem::path& root,
    const LayerDef& layer);
bool layerHasLocalSsotSource(
    const std::filesystem::path& root,
    const LayerDef& layer);
bool loadLayerFeaturesFromLocalSsotSource(
    const std::filesystem::path& root,
    const LayerDef& layer,
    std::vector<LayerDef::FeatureRecord>& features,
    std::vector<LayerDef::FeatureProperties>& feature_properties,
    std::string& source_used,
    std::string& error);
bool layerHasLocalImportArtifact(
    const std::filesystem::path& root,
    const LayerDef& layer,
    std::filesystem::path* out_path = nullptr);
bool loadLayerFeaturesFromLocalImportArtifact(
    const std::filesystem::path& root,
    const LayerDef& layer,
    std::vector<LayerDef::FeatureRecord>& features,
    std::vector<LayerDef::FeatureProperties>& feature_properties,
    std::string& source_used,
    std::string& error);
