#pragma once

#include "app_utils.h"
#include "app_settings.h"

#include <filesystem>

struct ImGuiContext;

void drawGearPanel(
    bool* show_sources_panel,
    const std::filesystem::path& root,
    AppSettings* app_settings,
    ImGuiContext* main_imgui_context,
    ImGuiContext* queue_imgui_context,
    BootstrapProgress& bootstrap);
