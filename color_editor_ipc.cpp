#include "color_editor_ipc.h"

#include <fstream>

#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
json colorToJson(const ImVec4& color) {
    return json::array({color.x, color.y, color.z, color.w});
}

ImVec4 colorFromJson(const json& j, const ImVec4& fallback) {
    if (!j.is_array() || j.size() != 4) return fallback;
    if (!j[0].is_number() || !j[1].is_number() || !j[2].is_number() || !j[3].is_number()) return fallback;
    return ImVec4(
        j[0].get<float>(),
        j[1].get<float>(),
        j[2].get<float>(),
        j[3].get<float>());
}

json histogramToJson(const ColorEditorHistogramSnapshot& histogram) {
    json j;
    j["valid"] = histogram.valid;
    j["sample_count"] = histogram.sample_count;
    j["min_value"] = histogram.min_value;
    j["max_value"] = histogram.max_value;
    j["clipped_max_value"] = histogram.clipped_max_value;
    j["median_value"] = histogram.median_value;
    j["bin_width"] = histogram.bin_width;
    j["max_bin"] = histogram.max_bin;
    j["plot_bins"] = histogram.plot_bins;
    j["bins"] = histogram.bins;
    j["cumulative_bins"] = histogram.cumulative_bins;
    return j;
}

ColorEditorHistogramSnapshot histogramFromJson(const json& j) {
    ColorEditorHistogramSnapshot histogram;
    if (!j.is_object()) return histogram;
    histogram.valid = j.value("valid", false);
    histogram.sample_count = j.value("sample_count", (size_t)0);
    histogram.min_value = j.value("min_value", 0.0);
    histogram.max_value = j.value("max_value", 0.0);
    histogram.clipped_max_value = j.value("clipped_max_value", 0.0);
    histogram.median_value = j.value("median_value", 0.0);
    histogram.bin_width = j.value("bin_width", 0.0);
    histogram.max_bin = j.value("max_bin", 0.0f);
    if (j.contains("plot_bins") && j["plot_bins"].is_array()) {
        histogram.plot_bins = j["plot_bins"].get<std::vector<float>>();
    }
    if (j.contains("bins") && j["bins"].is_array()) {
        histogram.bins = j["bins"].get<std::vector<uint32_t>>();
    }
    if (j.contains("cumulative_bins") && j["cumulative_bins"].is_array()) {
        histogram.cumulative_bins = j["cumulative_bins"].get<std::vector<uint32_t>>();
    }
    return histogram;
}

json optionToJson(const ColorEditorOptionSnapshot& option) {
    json j;
    j["id"] = option.id;
    j["label"] = option.label;
    j["field_label"] = option.field_label;
    j["control_layer_idx"] = option.control_layer_idx;
    j["normalize_mode"] = option.normalize_mode;
    j["percentile_clip"] = option.percentile_clip;
    j["gamma"] = option.gamma;
    j["histogram"] = histogramToJson(option.histogram);
    return j;
}

ColorEditorOptionSnapshot optionFromJson(const json& j) {
    ColorEditorOptionSnapshot option;
    if (!j.is_object()) return option;
    option.id = j.value("id", "");
    option.label = j.value("label", "");
    option.field_label = j.value("field_label", "");
    option.control_layer_idx = j.value("control_layer_idx", -1);
    option.normalize_mode = j.value("normalize_mode", 0);
    option.percentile_clip = j.value("percentile_clip", 100.0f);
    option.gamma = j.value("gamma", 1.0f);
    if (j.contains("histogram")) option.histogram = histogramFromJson(j["histogram"]);
    return option;
}

template <typename T>
bool saveJsonAtomic(const fs::path& path, const T& payload) {
    const fs::path tmp = path.string() + ".tmp";
    fs::create_directories(path.parent_path());
    std::ofstream out(tmp);
    if (!out) return false;
    out << payload.dump(2);
    out.close();
    if (!out.good()) return false;
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        ec.clear();
        fs::rename(tmp, path, ec);
        if (ec) return false;
    }
    return true;
}
}

bool saveColorEditorSnapshot(const fs::path& path, const ColorEditorSnapshot& snapshot) {
    json j;
    j["revision"] = snapshot.revision;
    j["command_path"] = snapshot.command_path.string();
    j["layer_idx"] = snapshot.layer_idx;
    j["outline_target"] = snapshot.outline_target;
    j["dark_mode"] = snapshot.dark_mode;
    j["supports_continuous"] = snapshot.supports_continuous;
    j["layer_name"] = snapshot.layer_name;
    j["layer_file"] = snapshot.layer_file;
    j["fill_color"] = colorToJson(snapshot.fill_color);
    j["outline_color"] = colorToJson(snapshot.outline_color);
    j["selected_option_id"] = snapshot.selected_option_id;
    j["options"] = json::array();
    for (const auto& option : snapshot.options) j["options"].push_back(optionToJson(option));
    return saveJsonAtomic(path, j);
}

bool loadColorEditorSnapshot(const fs::path& path, ColorEditorSnapshot& snapshot) {
    std::ifstream in(path);
    if (!in) return false;
    json j;
    try {
        in >> j;
    } catch (...) {
        return false;
    }
    snapshot = ColorEditorSnapshot{};
    snapshot.revision = j.value("revision", (uint64_t)0);
    snapshot.command_path = j.value("command_path", "");
    snapshot.layer_idx = j.value("layer_idx", -1);
    snapshot.outline_target = j.value("outline_target", false);
    snapshot.dark_mode = j.value("dark_mode", false);
    snapshot.supports_continuous = j.value("supports_continuous", false);
    snapshot.layer_name = j.value("layer_name", "");
    snapshot.layer_file = j.value("layer_file", "");
    if (j.contains("fill_color")) snapshot.fill_color = colorFromJson(j["fill_color"], snapshot.fill_color);
    if (j.contains("outline_color")) snapshot.outline_color = colorFromJson(j["outline_color"], snapshot.outline_color);
    snapshot.selected_option_id = j.value("selected_option_id", "static");
    if (j.contains("options") && j["options"].is_array()) {
        for (const auto& option_json : j["options"]) snapshot.options.push_back(optionFromJson(option_json));
    }
    return true;
}

bool saveColorEditorCommand(const fs::path& path, const ColorEditorCommand& command) {
    json j;
    j["seq"] = command.seq;
    j["type"] = command.type;
    j["layer_idx"] = command.layer_idx;
    j["control_layer_idx"] = command.control_layer_idx;
    j["option_id"] = command.option_id;
    j["color"] = colorToJson(command.color);
    j["normalize_mode"] = command.normalize_mode;
    j["percentile_clip"] = command.percentile_clip;
    j["gamma"] = command.gamma;
    return saveJsonAtomic(path, j);
}

bool loadColorEditorCommand(const fs::path& path, ColorEditorCommand& command) {
    std::ifstream in(path);
    if (!in) return false;
    json j;
    try {
        in >> j;
    } catch (...) {
        return false;
    }
    command = ColorEditorCommand{};
    command.seq = j.value("seq", (uint64_t)0);
    command.type = j.value("type", "");
    command.layer_idx = j.value("layer_idx", -1);
    command.control_layer_idx = j.value("control_layer_idx", -1);
    command.option_id = j.value("option_id", "");
    if (j.contains("color")) command.color = colorFromJson(j["color"], command.color);
    command.normalize_mode = j.value("normalize_mode", 0);
    command.percentile_clip = j.value("percentile_clip", 100.0f);
    command.gamma = j.value("gamma", 1.0f);
    return true;
}
