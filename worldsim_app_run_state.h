#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "app_settings.h"
#include "layer_runtime.h"

struct WorldSimRunState {
  int argc = 0;
  char** argv = nullptr;

  std::filesystem::path root;
  AppSettings app_settings;
  std::vector<LayerDef> layers;

  bool initialized = false;
  bool running = false;
  int exit_code = 0;

  // Planned migration fields:
  // - Windowing/Vulkan handles.
  // - Worker threads and synchronization primitives.
  // - UI state and frame-local profiling data.
  // - Layer-derived caches and map-render intermediates.
  // - Shutdown/finalization context inputs.
};

