#include "layer_state_io.h"

#include <algorithm>
#include <cctype>
#include <fstream>

#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

static LayerDef::Category inferCategory(const std::string& layer_name) {
    std::string s = layer_name;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    const char* infra_keywords[] = {
        "water", "drain", "sewer", "storm", "zoning", "route", "stop", "bus", "parking", "transit", "street"
    };
    for (const char* k : infra_keywords) {
        if (s.find(k) != std::string::npos) return LayerDef::Category::Infrastructure;
    }
    return LayerDef::Category::Housing;
}

std::vector<LayerDef> loadManifest(const fs::path& root) {
    std::vector<LayerDef> layers;
    std::ifstream in(root / "layers_manifest.json");
    if (!in) {
        in.open(root / "scripts" / "layers_manifest.json");
    }
    if (!in) return layers;
    json arr;
    in >> arr;
    for (size_t i = 0; i < arr.size(); ++i) {
        LayerDef ld;
        ld.name = arr[i]["name"].get<std::string>();
        ld.file = arr[i]["file"].get<std::string>();
        ld.source_url = arr[i].contains("url") ? arr[i]["url"].get<std::string>() : "";
        ld.description = arr[i].contains("description") ? arr[i]["description"].get<std::string>() : "";
        std::string c = arr[i]["color"].get<std::string>();
        auto hex = [&](int s) { return std::stoi(c.substr(s, 2), nullptr, 16) / 255.0f; };
        ld.color = ImVec4(hex(1), hex(3), hex(5), 1.0f);
        ld.enabled = i < 4;
        ld.category = inferCategory(ld.name);
        layers.push_back(std::move(ld));
    }
    return layers;
}

void loadLayerUiState(
    const fs::path& root,
    std::vector<LayerDef>& layers,
    bool& hover_inspector_enabled,
    std::unordered_map<std::string, bool>* zoning_zone_enabled,
    std::vector<bool>* layer_fill_enabled,
    std::vector<bool>* layer_hover_enabled,
    std::vector<bool>* layer_inspect_enabled) {
    std::ifstream in(root / "data" / "layer_ui_state.json");
    if (!in) return;
    json j;
    try {
        in >> j;
    } catch (...) {
        return;
    }
    if (j.contains("hover_inspector_enabled") && j["hover_inspector_enabled"].is_boolean()) {
        hover_inspector_enabled = j["hover_inspector_enabled"].get<bool>();
    } else if (j.contains("parcel_hover_enabled") && j["parcel_hover_enabled"].is_boolean()) {
        hover_inspector_enabled = j["parcel_hover_enabled"].get<bool>();
    }
    if (j.contains("layers") && j["layers"].is_object()) {
        const auto& obj = j["layers"];
        for (auto& l : layers) {
            if (obj.contains(l.file) && obj[l.file].is_boolean()) {
                l.enabled = obj[l.file].get<bool>();
            }
        }
    }
    if (zoning_zone_enabled && j.contains("zoning_zones") && j["zoning_zones"].is_object()) {
        zoning_zone_enabled->clear();
        for (auto it = j["zoning_zones"].begin(); it != j["zoning_zones"].end(); ++it) {
            if (it.value().is_boolean()) (*zoning_zone_enabled)[it.key()] = it.value().get<bool>();
        }
    }
    if (layer_fill_enabled && j.contains("layer_fills") && j["layer_fills"].is_object()) {
        const auto& obj = j["layer_fills"];
        if (layer_fill_enabled->size() < layers.size()) layer_fill_enabled->resize(layers.size(), true);
        for (size_t i = 0; i < layers.size(); ++i) {
            if (obj.contains(layers[i].file) && obj[layers[i].file].is_boolean()) {
                (*layer_fill_enabled)[i] = obj[layers[i].file].get<bool>();
            }
        }
    }
    if (layer_hover_enabled && j.contains("layer_hovers") && j["layer_hovers"].is_object()) {
        const auto& obj = j["layer_hovers"];
        if (layer_hover_enabled->size() < layers.size()) layer_hover_enabled->resize(layers.size(), true);
        for (size_t i = 0; i < layers.size(); ++i) {
            if (obj.contains(layers[i].file) && obj[layers[i].file].is_boolean()) {
                (*layer_hover_enabled)[i] = obj[layers[i].file].get<bool>();
            }
        }
    }
    if (layer_inspect_enabled && j.contains("layer_inspects") && j["layer_inspects"].is_object()) {
        const auto& obj = j["layer_inspects"];
        if (layer_inspect_enabled->size() < layers.size()) layer_inspect_enabled->resize(layers.size(), true);
        for (size_t i = 0; i < layers.size(); ++i) {
            if (obj.contains(layers[i].file) && obj[layers[i].file].is_boolean()) {
                (*layer_inspect_enabled)[i] = obj[layers[i].file].get<bool>();
            }
        }
    }
}

void saveLayerUiState(
    const fs::path& root,
    const std::vector<LayerDef>& layers,
    bool hover_inspector_enabled,
    const std::unordered_map<std::string, bool>* zoning_zone_enabled,
    const std::vector<bool>* layer_fill_enabled,
    const std::vector<bool>* layer_hover_enabled,
    const std::vector<bool>* layer_inspect_enabled) {
    fs::create_directories(root / "data");
    json j;
    j["hover_inspector_enabled"] = hover_inspector_enabled;
    json flags = json::object();
    for (const auto& l : layers) flags[l.file] = l.enabled;
    j["layers"] = flags;
    if (zoning_zone_enabled) {
        json zf = json::object();
        for (const auto& kv : *zoning_zone_enabled) zf[kv.first] = kv.second;
        j["zoning_zones"] = std::move(zf);
    }
    if (layer_fill_enabled) {
        json fills = json::object();
        for (size_t i = 0; i < layers.size(); ++i) {
            fills[layers[i].file] = i < layer_fill_enabled->size() ? (*layer_fill_enabled)[i] : true;
        }
        j["layer_fills"] = std::move(fills);
    }
    if (layer_hover_enabled) {
        json hovers = json::object();
        for (size_t i = 0; i < layers.size(); ++i) {
            hovers[layers[i].file] = i < layer_hover_enabled->size() ? (*layer_hover_enabled)[i] : true;
        }
        j["layer_hovers"] = std::move(hovers);
    }
    if (layer_inspect_enabled) {
        json inspects = json::object();
        for (size_t i = 0; i < layers.size(); ++i) {
            inspects[layers[i].file] = i < layer_inspect_enabled->size() ? (*layer_inspect_enabled)[i] : true;
        }
        j["layer_inspects"] = std::move(inspects);
    }
    std::ofstream out(root / "data" / "layer_ui_state.json");
    if (out) out << j.dump(2);
}
