#pragma once

#include "imgui.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct ColorEditorHistogramSnapshot {
    bool valid = false;
    size_t sample_count = 0;
    double min_value = 0.0;
    double max_value = 0.0;
    double clipped_max_value = 0.0;
    double median_value = 0.0;
    double bin_width = 0.0;
    float max_bin = 0.0f;
    std::vector<float> plot_bins;
    std::vector<uint32_t> bins;
    std::vector<uint32_t> cumulative_bins;
};

struct ColorEditorOptionSnapshot {
    std::string id;
    std::string label;
    std::string field_label;
    int control_layer_idx = -1;
    int normalize_mode = 0;
    float percentile_clip = 100.0f;
    float gamma = 1.0f;
    ColorEditorHistogramSnapshot histogram;
};

struct ColorEditorSnapshot {
    uint64_t revision = 0;
    std::filesystem::path command_path;
    int layer_idx = -1;
    bool outline_target = false;
    bool dark_mode = false;
    bool supports_continuous = false;
    std::string layer_name;
    std::string layer_file;
    ImVec4 fill_color = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    ImVec4 outline_color = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    std::string selected_option_id = "static";
    std::vector<ColorEditorOptionSnapshot> options;
};

struct ColorEditorCommand {
    uint64_t seq = 0;
    std::string type;
    int layer_idx = -1;
    int control_layer_idx = -1;
    std::string option_id;
    ImVec4 color = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    int normalize_mode = 0;
    float percentile_clip = 100.0f;
    float gamma = 1.0f;
};

bool saveColorEditorSnapshot(const std::filesystem::path& path, const ColorEditorSnapshot& snapshot);
bool loadColorEditorSnapshot(const std::filesystem::path& path, ColorEditorSnapshot& snapshot);

bool saveColorEditorCommand(const std::filesystem::path& path, const ColorEditorCommand& command);
bool loadColorEditorCommand(const std::filesystem::path& path, ColorEditorCommand& command);
