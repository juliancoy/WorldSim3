#pragma once

#include "data_library_coordinator.h"
#include "dataset_library.h"
#include "imgui.h"
#include "types.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

struct DataLibrarySearchCacheContext {
    const std::vector<LayerDef>* layers = nullptr;
    const char* query_buffer = nullptr;
    std::string* cached_query = nullptr;
    size_t* cached_layer_count = nullptr;
    std::vector<size_t>* visible_rows = nullptr;
    size_t* cache_rebuilds = nullptr;
};

void updateDataLibraryVisibleRows(DataLibrarySearchCacheContext& ctx);
void drawDataLibraryCell(const std::string& value, size_t max_chars = 64);
void drawDataLibraryTooltip(const LayerDef& layer);
const char* dataFreshnessLabel(FreshnessState state);
ImVec4 dataFreshnessColor(FreshnessState state);

struct DataLibraryUiContext {
    const std::filesystem::path* root = nullptr;
    std::vector<LayerDef>* layers = nullptr;
    DataLibraryCoordinatorContext* coordinator = nullptr;
    bool* show_data_library = nullptr;
    char* query_buffer = nullptr;
    size_t query_buffer_size = 0;
    int* download_phase = nullptr;
    bool* include_large = nullptr;
    std::string* cached_query = nullptr;
    size_t* cached_layer_count = nullptr;
    std::vector<size_t>* visible_rows = nullptr;
    size_t* cache_rebuilds = nullptr;
    size_t* rendered_rows_last = nullptr;
    std::function<bool(size_t)> enqueue_layer_download_request;
    std::function<size_t()> queue_all_missing_layer_downloads;
    size_t downloadable_missing_layer_count = 0;
    size_t queueable_missing_layer_count = 0;
};

void drawDataLibraryWindow(DataLibraryUiContext& ctx);
