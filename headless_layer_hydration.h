#pragma once

#include "types.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

struct HeadlessLayerHydrationOptions {
    unsigned int worker_count = 1;
    bool verbose = true;
};

struct HeadlessLayerHydrationFailure {
    size_t layer_index = 0;
    std::string layer_file;
    std::string error;
};

struct HeadlessLayerHydrationSummary {
    size_t local_layer_count = 0;
    size_t requested_layer_count = 0;
    size_t hydrated_layer_count = 0;
    size_t failed_layer_count = 0;
    size_t skipped_missing_layer_count = 0;
    size_t total_feature_count = 0;
    double elapsed_ms = 0.0;
    std::vector<size_t> requested_indices;
    std::vector<HeadlessLayerHydrationFailure> failures;
};

bool hydrateLocalLayersHeadless(
    const std::filesystem::path& root,
    std::vector<LayerDef>& layers,
    const HeadlessLayerHydrationOptions& options,
    HeadlessLayerHydrationSummary& summary);
