#pragma once

#include <filesystem>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "app_settings.h"
#include "dataset_library.h"
#include "heatmap_runtime.h"
#include "layer_registry.h"
#include "layer_runtime.h"

struct DataLibraryRuntimeState {
  std::string status_msg;
  std::string cached_query;
  size_t cached_layer_count = 0;
  std::vector<size_t> visible_rows;
  size_t cache_rebuilds = 0;
  size_t rendered_rows_last = 0;
  std::vector<FreshnessState> freshness_state;
  std::vector<std::string> freshness_msg;
  std::vector<bool> local_layer_exists_cache;
  int download_phase = 0;
  bool include_large = false;
  bool bulk_inflight = false;
  std::mutex bulk_mutex;
  std::string bulk_progress;
  std::future<LayerDownloadSummary> bulk_future;
};

struct WorldSimRunState {
  int argc = 0;
  char** argv = nullptr;

  std::filesystem::path root;
  AppSettings app_settings;
  std::vector<LayerDef> layers;
  LayerRegistry layer_registry;
  DataLibraryRuntimeState data_library;
  HeatmapRuntimeState heatmap;

  bool initialized = false;
  bool running = false;
  int exit_code = 0;

  // Planned migration fields:
  // - Windowing/Vulkan handles.
  // - Worker threads and synchronization primitives.
  // - Remaining UI state and frame-local profiling data.
  // - Layer-derived caches and map-render intermediates.
  // - Shutdown/finalization context inputs.
};
