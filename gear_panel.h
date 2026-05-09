#pragma once

#include "app_utils.h"

#include <filesystem>

void drawGearPanel(bool* show_sources_panel, const std::filesystem::path& root, BootstrapProgress& bootstrap);
