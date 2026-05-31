#include "layer_state_io.h"

#include "aggregate_visualization_strategies.h"
#include "app_utils.h"
#include "event_sectors.h"
#include "filters.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <unordered_set>
#include <type_traits>

#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
ImVec4 defaultOutlineColor(const ImVec4& fill) {
    return ImVec4(
        std::clamp(fill.x * 0.72f, 0.0f, 1.0f),
        std::clamp(fill.y * 0.72f, 0.0f, 1.0f),
        std::clamp(fill.z * 0.72f, 0.0f, 1.0f),
        1.0f);
}

std::string mapViewKey() {
    return "global";
}

std::string filterProfileKey() {
    return "global";
}

int manifestPriority(const fs::path& manifest_path) {
    const std::string name = manifest_path.filename().string();
    if (name == "layers_manifest.json") return 0;
    if (name == "layers_manifest.must_have.json") return 1;
    if (name == "layers_manifest.nice_to_have.json") return 2;
    if (name == "layers_manifest.heavy_data.json") return 3;
    if (name == "layers_manifest.extended_events.json") return 4;
    if (name == "layers_manifest.historical_high_quality.json") return 5;
    if (name == "layers_manifest.capital_flows.json") return 6;
    if (name == "layers_manifest.repository.json") return 7;
    if (name == "layers_manifest.archival_research.json") return 8;
    return 100;
}

std::vector<fs::path> discoverManifestPaths(const fs::path& root) {
    std::vector<fs::path> out;
    const fs::path sources_root = root / "sources" / "world";
    std::error_code ec;
    if (!fs::exists(sources_root, ec) || ec) return out;
    for (fs::recursive_directory_iterator it(sources_root, ec), end; it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file()) continue;
        const std::string name = it->path().filename().string();
        if (!name.starts_with("layers_manifest") || !name.ends_with(".json")) continue;
        out.push_back(it->path());
    }
    std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) {
        const fs::path a_parent = a.parent_path().lexically_normal();
        const fs::path b_parent = b.parent_path().lexically_normal();
        if (a_parent != b_parent) return a_parent.string() < b_parent.string();
        const int a_priority = manifestPriority(a);
        const int b_priority = manifestPriority(b);
        if (a_priority != b_priority) return a_priority < b_priority;
        return a.filename().string() < b.filename().string();
    });
    return out;
}

template <typename T>
struct LayerSettingDescriptor {
    const char* key;
    std::vector<T>* dst = nullptr;
    T fallback{};
};

template <typename T>
struct ConstLayerSettingDescriptor {
    const char* key;
    const std::vector<T>* src = nullptr;
    T fallback{};
};

template <typename T>
void loadLayerSettingsFromObject(
    const json& root_json,
    const std::vector<LayerDef>& layers,
    const std::initializer_list<LayerSettingDescriptor<T>>& settings) {
    for (const auto& setting : settings) {
        if (!setting.dst || !root_json.contains(setting.key) || !root_json[setting.key].is_object()) continue;
        const auto& obj = root_json[setting.key];
        if (setting.dst->size() < layers.size()) setting.dst->resize(layers.size(), setting.fallback);
        for (size_t i = 0; i < layers.size(); ++i) {
            const auto& file = layers[i].file;
            if constexpr (std::is_same_v<T, bool>) {
                if (obj.contains(file) && obj[file].is_boolean()) (*setting.dst)[i] = obj[file].template get<bool>();
            } else if constexpr (std::is_integral_v<T>) {
                if (obj.contains(file) && obj[file].is_number_integer()) (*setting.dst)[i] = obj[file].template get<T>();
            } else {
                if (obj.contains(file) && obj[file].is_number()) (*setting.dst)[i] = obj[file].template get<T>();
            }
        }
    }
}

template <typename T>
void saveLayerSettingsToObject(
    json& root_json,
    const std::vector<LayerDef>& layers,
    const std::initializer_list<ConstLayerSettingDescriptor<T>>& settings) {
    for (const auto& setting : settings) {
        if (!setting.src) continue;
        json obj = json::object();
        for (size_t i = 0; i < layers.size(); ++i) {
            obj[layers[i].file] = i < setting.src->size() ? (*setting.src)[i] : setting.fallback;
        }
        root_json[setting.key] = std::move(obj);
    }
}

bool parseHexLayerColor(const std::string& color_hex, ImVec4& out) {
    if ((color_hex.size() != 7 && color_hex.size() != 9) || color_hex[0] != '#') return false;
    auto hex = [&](int s) { return std::stoi(color_hex.substr(s, 2), nullptr, 16) / 255.0f; };
    out = ImVec4(hex(1), hex(3), hex(5), color_hex.size() == 9 ? hex(7) : 1.0f);
    return true;
}

std::string encodeHexLayerColor(const ImVec4& color) {
    auto chan = [](float v) {
        return std::clamp((int)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f), 0, 255);
    };
    char buf[10];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", chan(color.x), chan(color.y), chan(color.z), chan(color.w));
    return std::string(buf);
}

int findLayerIndexByFile(const std::vector<LayerDef>& layers, const std::string& file) {
    for (size_t i = 0; i < layers.size(); ++i) {
        if (layerMatchesIdentifier(layers[i], file)) return (int)i;
    }
    return -1;
}

int findFirstEnabledLayerByCategory(const std::vector<LayerDef>& layers, const std::vector<bool>* enabled, LayerDef::Category category) {
    for (size_t i = 0; i < layers.size(); ++i) {
        if (layers[i].category != category) continue;
        if (enabled && i < enabled->size() && !(*enabled)[i]) continue;
        return (int)i;
    }
    return -1;
}

int findFirstEnabledParcelLayer(const std::vector<LayerDef>& layers, const std::vector<bool>* enabled) {
    for (size_t i = 0; i < layers.size(); ++i) {
        if (layers[i].scale != "parcel") continue;
        if (enabled && i < enabled->size() && !(*enabled)[i]) continue;
        return (int)i;
    }
    return -1;
}

int findFirstEnabledPointLayer(const std::vector<LayerDef>& layers, const std::vector<bool>* enabled) {
    for (size_t i = 0; i < layers.size(); ++i) {
        if (!layerUsesPointGeometry(layers[i])) continue;
        if (enabled && i < enabled->size() && !(*enabled)[i]) continue;
        return (int)i;
    }
    return -1;
}

int deriveLegacyActiveHoverLayerIdx(
    const std::vector<LayerDef>& layers,
    int legacy_mode,
    const std::vector<bool>* enabled) {
    if (legacy_mode == 0) return -1;
    if (legacy_mode == 1) return findFirstEnabledParcelLayer(layers, enabled);
    if (legacy_mode == 2) return findFirstEnabledLayerByCategory(layers, enabled, LayerDef::Category::Zoning);
    if (enabled) {
        for (size_t i = 0; i < enabled->size() && i < layers.size(); ++i) {
            if ((*enabled)[i]) return (int)i;
        }
    }
    const int zoning_idx = findFirstEnabledLayerByCategory(layers, enabled, LayerDef::Category::Zoning);
    if (zoning_idx >= 0) return zoning_idx;
    const int point_idx = findFirstEnabledPointLayer(layers, enabled);
    if (point_idx >= 0) return point_idx;
    return findFirstEnabledLayerByCategory(layers, enabled, LayerDef::Category::Housing);
}

int deriveLegacyActiveClickLayerIdx(const std::vector<LayerDef>& layers, const std::vector<bool>* enabled) {
    const int parcel_idx = findFirstEnabledParcelLayer(layers, enabled);
    if (parcel_idx >= 0) return parcel_idx;
    return findFirstEnabledLayerByCategory(layers, enabled, LayerDef::Category::Zoning);
}

int legacyHoverModeForActiveLayer(const std::vector<LayerDef>& layers, const int* active_hover_layer_idx) {
    if (!active_hover_layer_idx || *active_hover_layer_idx < 0 || (size_t)*active_hover_layer_idx >= layers.size()) return 0;
    const LayerDef& layer = layers[(size_t)*active_hover_layer_idx];
    if (layer.scale == "parcel") return 1;
    if (layer.category == LayerDef::Category::Zoning) return 2;
    return 3;
}
}

static void copyToBuffer(const std::string& src, char* dst, size_t dst_size) {
    if (!dst || dst_size == 0) return;
    std::strncpy(dst, src.c_str(), dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static LayerDef::Category inferCategory(const std::string& layer_name) {
    std::string s = layer_name;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    const char* zoning_keywords[] = {
        "zoning"
    };
    for (const char* k : zoning_keywords) {
        if (s.find(k) != std::string::npos) return LayerDef::Category::Zoning;
    }
    const char* health_keywords[] = {
        "health", "life expectancy", "asthma", "diabetes", "obesity", "stroke", "depression", "svi", "vulnerability"
    };
    for (const char* k : health_keywords) {
        if (s.find(k) != std::string::npos) return LayerDef::Category::PublicHealth;
    }
    const char* infra_keywords[] = {
        "water", "drain", "sewer", "storm", "route", "stop", "bus", "parking", "transit", "street"
    };
    for (const char* k : infra_keywords) {
        if (s.find(k) != std::string::npos) return LayerDef::Category::Infrastructure;
    }
    return LayerDef::Category::Housing;
}

static LayerDef::Category parseCategory(const json& v, const std::string& layer_name) {
    if (v.contains("category") && v["category"].is_string()) {
        std::string c = v["category"].get<std::string>();
        std::transform(c.begin(), c.end(), c.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
        if (c == "housing") return LayerDef::Category::Housing;
        if (c == "public_health" || c == "public-health" || c == "publichealth") return LayerDef::Category::PublicHealth;
        if (c == "infrastructure") return LayerDef::Category::Infrastructure;
        if (c == "zoning") return LayerDef::Category::Zoning;
        if (c == "safety") return LayerDef::Category::Safety;
    }
    return inferCategory(layer_name);
}

static void appendManifestEntries(
    const fs::path& manifest_path,
    const fs::path& root,
    std::unordered_set<std::string>& seen_files,
    std::vector<LayerDef>& layers,
    bool include_non_runtime) {
    std::ifstream in(manifest_path);
    if (!in) return;
    json arr;
    try {
        in >> arr;
    } catch (...) {
        return;
    }
    if (!arr.is_array()) return;
    for (size_t i = 0; i < arr.size(); ++i) {
        if (!arr[i].contains("file") || !arr[i]["file"].is_string()) continue;
        const std::string file = arr[i]["file"].get<std::string>();
        if (file.empty() || seen_files.count(file)) continue;
        if (!include_non_runtime &&
            arr[i].contains("runtime_load") &&
            arr[i]["runtime_load"].is_boolean() &&
            !arr[i]["runtime_load"].get<bool>()) {
            continue;
        }

        LayerDef ld;
        ld.name = arr[i].value("name", file);
        ld.logical_id = arr[i].value("id", defaultLayerLogicalIdForFile(file));
        ld.file = file;
        ld.source_url = arr[i].contains("url") ? arr[i]["url"].get<std::string>() : "";
        ld.reference_url = arr[i].contains("reference_url") ? arr[i]["reference_url"].get<std::string>() : "";
        if (arr[i].contains("source_urls") && arr[i]["source_urls"].is_array()) {
            for (const auto& v : arr[i]["source_urls"]) {
                if (v.is_string()) ld.source_urls.push_back(v.get<std::string>());
            }
        }
        if (arr[i].contains("import") && arr[i]["import"].is_object()) {
            const auto& import = arr[i]["import"];
            ld.import_type = import.value("type", std::string());
            ld.import_url = import.value("url", std::string());
            ld.import_source_crs = import.value("source_crs", std::string());
            ld.import_shapefile = import.value("shapefile", std::string());
            ld.import_service_url = import.value("service_url", std::string());
            ld.import_where = import.value("where", std::string());
            ld.import_normalizer = import.value("normalizer", std::string());
            ld.import_query = import.value("query", std::string());
            ld.import_table = import.value("table", std::string());
            ld.import_year = import.value("year", std::string());
            ld.import_survey = import.value("survey", std::string());
            ld.import_sheet_name = import.value("sheet_name", std::string());
            ld.import_item_path = import.value("item_path", std::string());
            ld.import_lon_field = import.value("lon_field", std::string());
            ld.import_lat_field = import.value("lat_field", std::string());
            ld.import_artifact_file = import.value("artifact_file", std::string());
        }
        if (arr[i].contains("provenance") && arr[i]["provenance"].is_object()) {
            const auto& provenance = arr[i]["provenance"];
            ld.provenance_world = provenance.value("world", std::string());
            ld.provenance_nation_state = provenance.value("nation_state", std::string());
            ld.provenance_state_region = provenance.value("state_region", std::string());
            ld.provenance_county_city = provenance.value("county_city", std::string());
        }
        ld.description = arr[i].contains("description") ? arr[i]["description"].get<std::string>() : "";
        ld.heatmap_field = arr[i].contains("heatmap_field") ? arr[i]["heatmap_field"].get<std::string>() : "";
        ld.subcategory = arr[i].contains("subcategory") ? arr[i]["subcategory"].get<std::string>() : "";
        ld.region = arr[i].contains("region") ? arr[i]["region"].get<std::string>() : "";
        ld.scale = arr[i].contains("scale") ? arr[i]["scale"].get<std::string>() : "";
        ld.duckdb_role = arr[i].contains("duckdb_role") ? arr[i]["duckdb_role"].get<std::string>() : "";
        std::string c = arr[i].value("color", std::string("#999999"));
        parseHexLayerColor(c, ld.color);
        ld.outline_color = defaultOutlineColor(ld.color);
        ld.enabled = arr[i].contains("default_enabled") ? arr[i]["default_enabled"].get<bool>() : false;
        ld.runtime_load = arr[i].contains("runtime_load") ? arr[i]["runtime_load"].get<bool>() : true;
        ld.duckdb_ingest = arr[i].contains("duckdb_ingest") ? arr[i]["duckdb_ingest"].get<bool>() : true;
        ld.category = parseCategory(arr[i], ld.name);
        refreshLayerGeometryUsageCache(ld);
        layers.push_back(std::move(ld));
        seen_files.insert(file);
    }
}

std::vector<LayerDef> loadManifest(const fs::path& root, bool include_non_runtime) {
    std::vector<LayerDef> layers;
    std::unordered_set<std::string> seen_files;
    const std::vector<fs::path> manifest_paths = discoverManifestPaths(root);
    for (const auto& manifest_path : manifest_paths) {
        appendManifestEntries(manifest_path, root, seen_files, layers, include_non_runtime);
    }
    return layers;
}

void loadLayerUiState(
    const fs::path& root,
    std::vector<LayerDef>& layers,
    bool& hover_inspector_enabled,
    int* active_hover_layer_idx,
    int* active_click_layer_idx,
    int* parcel_parameter_mode,
    std::unordered_map<std::string, bool>* zoning_zone_enabled,
    std::vector<bool>* layer_fill_enabled,
    std::vector<bool>* layer_hover_enabled,
    std::vector<bool>* layer_inspect_enabled,
    std::vector<bool>* layer_heatmap_enabled,
    std::vector<int>* layer_heatmap_max_zoom,
    std::vector<int>* layer_parcel_detail_min_zoom,
    std::vector<bool>* layer_heatmap_use_gradient,
    std::vector<float>* layer_choropleth_gamma,
    std::vector<int>* layer_heatmap_algo,
    std::vector<int>* layer_normalize_mode,
    std::vector<float>* layer_heatmap_cell_px,
    std::vector<float>* layer_heatmap_bandwidth_px,
    std::vector<float>* layer_heatmap_blur_sigma_px,
    std::vector<float>* layer_heatmap_percentile_clip,
    std::vector<bool>* layer_heatmap_zoom_adaptive_bandwidth,
    std::vector<bool>* layer_heatmap_multires_enabled,
    std::vector<float>* layer_heatmap_multires_blend,
    int* heatmap_algo,
    int* heatmap_quality_preset,
    float* heatmap_cell_px,
    float* heatmap_bandwidth_px,
    float* heatmap_blur_sigma_px,
    float* heatmap_percentile_clip,
    bool* heatmap_zoom_adaptive_bandwidth,
    bool* heatmap_multires_enabled,
    float* heatmap_multires_blend) {
    std::ifstream in(root / "data" / "layer_ui_state.json");
    if (!in) return;
    json j;
    try {
        in >> j;
    } catch (...) {
        return;
    }
    int legacy_hover_mode = 3;
    bool have_legacy_hover_mode = false;
    if (j.contains("hover_inspector_mode") && j["hover_inspector_mode"].is_number_integer()) {
        legacy_hover_mode = std::clamp(j["hover_inspector_mode"].get<int>(), 0, 3);
        hover_inspector_enabled = legacy_hover_mode != 0;
        have_legacy_hover_mode = true;
    } else if (j.contains("hover_inspector_enabled") && j["hover_inspector_enabled"].is_boolean()) {
        hover_inspector_enabled = j["hover_inspector_enabled"].get<bool>();
        legacy_hover_mode = hover_inspector_enabled ? 3 : 0;
    } else if (j.contains("parcel_hover_enabled") && j["parcel_hover_enabled"].is_boolean()) {
        hover_inspector_enabled = j["parcel_hover_enabled"].get<bool>();
        legacy_hover_mode = hover_inspector_enabled ? 3 : 0;
    }
    if (parcel_parameter_mode && j.contains("parcel_parameter_mode") && j["parcel_parameter_mode"].is_number_integer()) {
        *parcel_parameter_mode = std::clamp(j["parcel_parameter_mode"].get<int>(), 0, 3);
    }
    if (j.contains("layers") && j["layers"].is_object()) {
        const auto& obj = j["layers"];
        for (auto& l : layers) {
            if (obj.contains(l.file) && obj[l.file].is_boolean()) {
                l.enabled = obj[l.file].get<bool>();
            }
        }
    }
    if (j.contains("layer_colors") && j["layer_colors"].is_object()) {
        const auto& obj = j["layer_colors"];
        for (auto& l : layers) {
            auto it = obj.find(l.file);
            if (it != obj.end() && it->is_string()) parseHexLayerColor(it->get<std::string>(), l.color);
        }
    }
    if (j.contains("layer_outline_colors") && j["layer_outline_colors"].is_object()) {
        const auto& obj = j["layer_outline_colors"];
        for (auto& l : layers) {
            auto it = obj.find(l.file);
            if (it != obj.end() && it->is_string() && parseHexLayerColor(it->get<std::string>(), l.outline_color)) continue;
            l.outline_color = defaultOutlineColor(l.color);
        }
    } else {
        for (auto& l : layers) l.outline_color = defaultOutlineColor(l.color);
    }
    if (zoning_zone_enabled && j.contains("zoning_zones") && j["zoning_zones"].is_object()) {
        zoning_zone_enabled->clear();
        for (auto it = j["zoning_zones"].begin(); it != j["zoning_zones"].end(); ++it) {
            if (it.value().is_boolean()) (*zoning_zone_enabled)[it.key()] = it.value().get<bool>();
        }
    }
    loadLayerSettingsFromObject<bool>(j, layers, {
        {"layer_fills", layer_fill_enabled, true},
        {"layer_hovers", layer_hover_enabled, true},
        {"layer_inspects", layer_inspect_enabled, true},
        {"layer_heatmaps", layer_heatmap_enabled, true},
        {"layer_heatmap_use_gradient", layer_heatmap_use_gradient, true},
        {"layer_heatmap_zoom_adaptive_bandwidth", layer_heatmap_zoom_adaptive_bandwidth, true},
        {"layer_heatmap_multires_enabled", layer_heatmap_multires_enabled, true},
    });
    if (layer_fill_enabled && layer_heatmap_use_gradient) {
        const size_t count = std::min({layers.size(), layer_fill_enabled->size(), layer_heatmap_use_gradient->size()});
        for (size_t i = 0; i < count; ++i) {
            if (!layers[i].heatmap_field.empty() && (*layer_heatmap_use_gradient)[i]) {
                (*layer_fill_enabled)[i] = true;
            }
        }
    }
    loadLayerSettingsFromObject<int>(j, layers, {
        {"layer_heatmap_max_zoom", layer_heatmap_max_zoom, 13},
        {"layer_parcel_detail_min_zoom", layer_parcel_detail_min_zoom, 14},
        {"layer_heatmap_algo", layer_heatmap_algo, -1},
        {"layer_normalize_mode", layer_normalize_mode, 0},
    });
    if (layer_normalize_mode) {
        for (int& mode : *layer_normalize_mode) mode = std::clamp(mode, 0, 3);
    }
    loadLayerSettingsFromObject<float>(j, layers, {
        {"layer_heatmap_cell_px", layer_heatmap_cell_px, 24.0f},
        {"layer_heatmap_bandwidth_px", layer_heatmap_bandwidth_px, 18.0f},
        {"layer_heatmap_blur_sigma_px", layer_heatmap_blur_sigma_px, 6.0f},
        {"layer_heatmap_percentile_clip", layer_heatmap_percentile_clip, 95.0f},
        {"layer_choropleth_gamma", layer_choropleth_gamma, 1.0f},
        {"layer_heatmap_multires_blend", layer_heatmap_multires_blend, 0.5f},
    });
    if (layer_choropleth_gamma) {
        for (float& gamma : *layer_choropleth_gamma) gamma = std::clamp(gamma, 0.10f, 5.0f);
    }
    if (j.contains("heatmap_settings") && j["heatmap_settings"].is_object()) {
        const auto& hs = j["heatmap_settings"];
        if (heatmap_algo && hs.contains("algo") && hs["algo"].is_number_integer()) *heatmap_algo = hs["algo"].get<int>();
        if (heatmap_quality_preset && hs.contains("quality_preset") && hs["quality_preset"].is_number_integer()) *heatmap_quality_preset = hs["quality_preset"].get<int>();
        if (heatmap_cell_px && hs.contains("cell_px") && hs["cell_px"].is_number()) *heatmap_cell_px = hs["cell_px"].get<float>();
        if (heatmap_bandwidth_px && hs.contains("bandwidth_px") && hs["bandwidth_px"].is_number()) *heatmap_bandwidth_px = hs["bandwidth_px"].get<float>();
        if (heatmap_blur_sigma_px && hs.contains("blur_sigma_px") && hs["blur_sigma_px"].is_number()) *heatmap_blur_sigma_px = hs["blur_sigma_px"].get<float>();
        if (heatmap_percentile_clip && hs.contains("percentile_clip") && hs["percentile_clip"].is_number()) *heatmap_percentile_clip = hs["percentile_clip"].get<float>();
        if (heatmap_zoom_adaptive_bandwidth && hs.contains("zoom_adaptive_bandwidth") && hs["zoom_adaptive_bandwidth"].is_boolean()) *heatmap_zoom_adaptive_bandwidth = hs["zoom_adaptive_bandwidth"].get<bool>();
        if (heatmap_multires_enabled && hs.contains("multires_enabled") && hs["multires_enabled"].is_boolean()) *heatmap_multires_enabled = hs["multires_enabled"].get<bool>();
        if (heatmap_multires_blend && hs.contains("multires_blend") && hs["multires_blend"].is_number()) *heatmap_multires_blend = hs["multires_blend"].get<float>();
    }

    if (active_hover_layer_idx) {
        *active_hover_layer_idx = -1;
        if (j.contains("active_hover_layer") && j["active_hover_layer"].is_string()) {
            *active_hover_layer_idx = findLayerIndexByFile(layers, j["active_hover_layer"].get<std::string>());
        } else if (hover_inspector_enabled || have_legacy_hover_mode) {
            *active_hover_layer_idx = deriveLegacyActiveHoverLayerIdx(layers, legacy_hover_mode, layer_hover_enabled);
        }
        if (!hover_inspector_enabled) *active_hover_layer_idx = -1;
    }
    if (active_click_layer_idx) {
        *active_click_layer_idx = -1;
        if (j.contains("active_click_layer") && j["active_click_layer"].is_string()) {
            *active_click_layer_idx = findLayerIndexByFile(layers, j["active_click_layer"].get<std::string>());
        } else {
            *active_click_layer_idx = deriveLegacyActiveClickLayerIdx(layers, layer_inspect_enabled);
        }
    }
}

void saveLayerUiState(
    const fs::path& root,
    const std::vector<LayerDef>& layers,
    bool hover_inspector_enabled,
    const int* active_hover_layer_idx,
    const int* active_click_layer_idx,
    const int* parcel_parameter_mode,
    const std::unordered_map<std::string, bool>* zoning_zone_enabled,
    const std::vector<bool>* layer_fill_enabled,
    const std::vector<bool>* layer_hover_enabled,
    const std::vector<bool>* layer_inspect_enabled,
    const std::vector<bool>* layer_heatmap_enabled,
    const std::vector<int>* layer_heatmap_max_zoom,
    const std::vector<int>* layer_parcel_detail_min_zoom,
    const std::vector<bool>* layer_heatmap_use_gradient,
    const std::vector<float>* layer_choropleth_gamma,
    const std::vector<int>* layer_heatmap_algo,
    const std::vector<int>* layer_normalize_mode,
    const std::vector<float>* layer_heatmap_cell_px,
    const std::vector<float>* layer_heatmap_bandwidth_px,
    const std::vector<float>* layer_heatmap_blur_sigma_px,
    const std::vector<float>* layer_heatmap_percentile_clip,
    const std::vector<bool>* layer_heatmap_zoom_adaptive_bandwidth,
    const std::vector<bool>* layer_heatmap_multires_enabled,
    const std::vector<float>* layer_heatmap_multires_blend,
    const int* heatmap_algo,
    const int* heatmap_quality_preset,
    const float* heatmap_cell_px,
    const float* heatmap_bandwidth_px,
    const float* heatmap_blur_sigma_px,
    const float* heatmap_percentile_clip,
    const bool* heatmap_zoom_adaptive_bandwidth,
    const bool* heatmap_multires_enabled,
    const float* heatmap_multires_blend) {
    fs::create_directories(root / "data");
    json j;
    j["hover_inspector_enabled"] = hover_inspector_enabled;
    j["hover_inspector_mode"] = legacyHoverModeForActiveLayer(layers, active_hover_layer_idx);
    if (active_hover_layer_idx && *active_hover_layer_idx >= 0 && (size_t)*active_hover_layer_idx < layers.size()) {
        j["active_hover_layer"] = layers[(size_t)*active_hover_layer_idx].file;
    }
    if (active_click_layer_idx && *active_click_layer_idx >= 0 && (size_t)*active_click_layer_idx < layers.size()) {
        j["active_click_layer"] = layers[(size_t)*active_click_layer_idx].file;
    }
    if (parcel_parameter_mode) j["parcel_parameter_mode"] = std::clamp(*parcel_parameter_mode, 0, 3);
    json flags = json::object();
    for (const auto& l : layers) flags[l.file] = l.enabled;
    j["layers"] = flags;
    json fill_colors = json::object();
    json outline_colors = json::object();
    for (const auto& l : layers) {
        fill_colors[l.file] = encodeHexLayerColor(l.color);
        outline_colors[l.file] = encodeHexLayerColor(l.outline_color);
    }
    j["layer_colors"] = std::move(fill_colors);
    j["layer_outline_colors"] = std::move(outline_colors);
    if (zoning_zone_enabled) {
        json zf = json::object();
        for (const auto& kv : *zoning_zone_enabled) zf[kv.first] = kv.second;
        j["zoning_zones"] = std::move(zf);
    }
    saveLayerSettingsToObject<bool>(j, layers, {
        {"layer_fills", layer_fill_enabled, true},
        {"layer_hovers", layer_hover_enabled, true},
        {"layer_inspects", layer_inspect_enabled, true},
        {"layer_heatmaps", layer_heatmap_enabled, true},
        {"layer_heatmap_use_gradient", layer_heatmap_use_gradient, true},
        {"layer_heatmap_zoom_adaptive_bandwidth", layer_heatmap_zoom_adaptive_bandwidth, true},
        {"layer_heatmap_multires_enabled", layer_heatmap_multires_enabled, true},
    });
    saveLayerSettingsToObject<int>(j, layers, {
        {"layer_heatmap_max_zoom", layer_heatmap_max_zoom, 13},
        {"layer_parcel_detail_min_zoom", layer_parcel_detail_min_zoom, kParcelChoroplethMinZoom},
        {"layer_heatmap_algo", layer_heatmap_algo, -1},
        {"layer_normalize_mode", layer_normalize_mode, 0},
    });
    saveLayerSettingsToObject<float>(j, layers, {
        {"layer_heatmap_cell_px", layer_heatmap_cell_px, 24.0f},
        {"layer_heatmap_bandwidth_px", layer_heatmap_bandwidth_px, 18.0f},
        {"layer_heatmap_blur_sigma_px", layer_heatmap_blur_sigma_px, 6.0f},
        {"layer_heatmap_percentile_clip", layer_heatmap_percentile_clip, 95.0f},
        {"layer_choropleth_gamma", layer_choropleth_gamma, 1.0f},
        {"layer_heatmap_multires_blend", layer_heatmap_multires_blend, 0.5f},
    });
    if (heatmap_algo || heatmap_quality_preset || heatmap_cell_px || heatmap_bandwidth_px || heatmap_blur_sigma_px ||
        heatmap_percentile_clip || heatmap_zoom_adaptive_bandwidth || heatmap_multires_enabled || heatmap_multires_blend) {
        json hs = json::object();
        if (heatmap_algo) hs["algo"] = *heatmap_algo;
        if (heatmap_quality_preset) hs["quality_preset"] = *heatmap_quality_preset;
        if (heatmap_cell_px) hs["cell_px"] = *heatmap_cell_px;
        if (heatmap_bandwidth_px) hs["bandwidth_px"] = *heatmap_bandwidth_px;
        if (heatmap_blur_sigma_px) hs["blur_sigma_px"] = *heatmap_blur_sigma_px;
        if (heatmap_percentile_clip) hs["percentile_clip"] = *heatmap_percentile_clip;
        if (heatmap_zoom_adaptive_bandwidth) hs["zoom_adaptive_bandwidth"] = *heatmap_zoom_adaptive_bandwidth;
        if (heatmap_multires_enabled) hs["multires_enabled"] = *heatmap_multires_enabled;
        if (heatmap_multires_blend) hs["multires_blend"] = *heatmap_multires_blend;
        j["heatmap_settings"] = std::move(hs);
    }
    std::ofstream out(root / "data" / "layer_ui_state.json");
    if (out) out << j.dump(2);
}

void loadFilterUiState(
    const fs::path& root,
    bool* filter_enabled,
    bool* filter_use_date,
    int* filter_year_min,
    int* filter_year_max,
    char* filter_blocklot,
    size_t filter_blocklot_size,
    char* filter_status,
    size_t filter_status_size,
    char* filter_address,
    size_t filter_address_size,
    char* filter_owner,
    size_t filter_owner_size,
    char* filter_zip,
    size_t filter_zip_size,
    bool* crime_filter_enabled,
    bool* crime_filter_homicide,
    bool* crime_filter_robbery,
    bool* crime_filter_assault,
    bool* crime_filter_burglary,
    bool* crime_filter_theft,
    bool* crime_filter_auto_theft,
    bool* crime_filter_drug,
    bool* crime_filter_shooting,
    bool* crime_filter_use_year,
    int* crime_year_min,
    int* crime_year_max,
    char* owner_search_query,
    size_t owner_search_query_size,
    std::unordered_set<std::string>* selected_owners,
    std::unordered_map<std::string, bool>* event_sector_enabled) {
    std::ifstream in(root / "data" / "layer_ui_state.json");
    if (!in) return;
    json j;
    try {
        in >> j;
    } catch (...) {
        return;
    }
    if (!j.contains("filters") || !j["filters"].is_object()) return;
    const json& persisted_filters = j["filters"];
    const json* active_filters = &persisted_filters;
    const std::string profile_key = filterProfileKey();
    if (j.contains("filter_profiles") && j["filter_profiles"].is_object()) {
        const json& profiles = j["filter_profiles"];
        if (profiles.contains(profile_key) && profiles[profile_key].is_object()) active_filters = &profiles[profile_key];
    }
    const json& f = *active_filters;
    if (filter_enabled && f.contains("enabled") && f["enabled"].is_boolean()) *filter_enabled = f["enabled"].get<bool>();
    if (filter_use_date && f.contains("use_date") && f["use_date"].is_boolean()) *filter_use_date = f["use_date"].get<bool>();
    if (filter_year_min && f.contains("year_min") && f["year_min"].is_number_integer()) *filter_year_min = f["year_min"].get<int>();
    if (filter_year_max && f.contains("year_max") && f["year_max"].is_number_integer()) *filter_year_max = f["year_max"].get<int>();
    if (f.contains("blocklot") && f["blocklot"].is_string()) copyToBuffer(f["blocklot"].get<std::string>(), filter_blocklot, filter_blocklot_size);
    if (f.contains("status") && f["status"].is_string()) copyToBuffer(f["status"].get<std::string>(), filter_status, filter_status_size);
    if (f.contains("address") && f["address"].is_string()) copyToBuffer(f["address"].get<std::string>(), filter_address, filter_address_size);
    if (f.contains("owner") && f["owner"].is_string()) copyToBuffer(f["owner"].get<std::string>(), filter_owner, filter_owner_size);
    if (f.contains("zip") && f["zip"].is_string()) copyToBuffer(f["zip"].get<std::string>(), filter_zip, filter_zip_size);
    if (f.contains("owner_search_query") && f["owner_search_query"].is_string()) {
        copyToBuffer(f["owner_search_query"].get<std::string>(), owner_search_query, owner_search_query_size);
    }
    if (crime_filter_enabled && f.contains("crime_enabled") && f["crime_enabled"].is_boolean()) *crime_filter_enabled = f["crime_enabled"].get<bool>();
    if (crime_filter_homicide && f.contains("crime_homicide") && f["crime_homicide"].is_boolean()) *crime_filter_homicide = f["crime_homicide"].get<bool>();
    if (crime_filter_robbery && f.contains("crime_robbery") && f["crime_robbery"].is_boolean()) *crime_filter_robbery = f["crime_robbery"].get<bool>();
    if (crime_filter_assault && f.contains("crime_assault") && f["crime_assault"].is_boolean()) *crime_filter_assault = f["crime_assault"].get<bool>();
    if (crime_filter_burglary && f.contains("crime_burglary") && f["crime_burglary"].is_boolean()) *crime_filter_burglary = f["crime_burglary"].get<bool>();
    if (crime_filter_theft && f.contains("crime_theft") && f["crime_theft"].is_boolean()) *crime_filter_theft = f["crime_theft"].get<bool>();
    if (crime_filter_auto_theft && f.contains("crime_auto_theft") && f["crime_auto_theft"].is_boolean()) *crime_filter_auto_theft = f["crime_auto_theft"].get<bool>();
    if (crime_filter_drug && f.contains("crime_drug") && f["crime_drug"].is_boolean()) *crime_filter_drug = f["crime_drug"].get<bool>();
    if (crime_filter_shooting && f.contains("crime_shooting") && f["crime_shooting"].is_boolean()) *crime_filter_shooting = f["crime_shooting"].get<bool>();
    if (crime_filter_use_year && f.contains("crime_use_year") && f["crime_use_year"].is_boolean()) *crime_filter_use_year = f["crime_use_year"].get<bool>();
    if (crime_year_min && f.contains("crime_year_min") && f["crime_year_min"].is_number_integer()) *crime_year_min = f["crime_year_min"].get<int>();
    if (crime_year_max && f.contains("crime_year_max") && f["crime_year_max"].is_number_integer()) *crime_year_max = f["crime_year_max"].get<int>();
    if (selected_owners && f.contains("selected_owners") && f["selected_owners"].is_array()) {
        selected_owners->clear();
        for (const auto& v : f["selected_owners"]) {
            if (v.is_string()) selected_owners->insert(v.get<std::string>());
        }
    }
    if (event_sector_enabled && f.contains("event_sector_enabled") && f["event_sector_enabled"].is_object()) {
        event_sector_enabled->clear();
        for (auto it = f["event_sector_enabled"].begin(); it != f["event_sector_enabled"].end(); ++it) {
            if (it.value().is_boolean()) (*event_sector_enabled)[it.key()] = it.value().get<bool>();
        }
    }
    if (event_sector_enabled) ensureCommunitySectorFilterDefaults(*event_sector_enabled);
}

void saveFilterUiState(
    const fs::path& root,
    bool filter_enabled,
    bool filter_use_date,
    int filter_year_min,
    int filter_year_max,
    const char* filter_blocklot,
    const char* filter_status,
    const char* filter_address,
    const char* filter_owner,
    const char* filter_zip,
    bool crime_filter_enabled,
    bool crime_filter_homicide,
    bool crime_filter_robbery,
    bool crime_filter_assault,
    bool crime_filter_burglary,
    bool crime_filter_theft,
    bool crime_filter_auto_theft,
    bool crime_filter_drug,
    bool crime_filter_shooting,
    bool crime_filter_use_year,
    int crime_year_min,
    int crime_year_max,
    const char* owner_search_query,
    const std::unordered_set<std::string>& selected_owners,
    const std::unordered_map<std::string, bool>& event_sector_enabled) {
    fs::create_directories(root / "data");
    json j = json::object();
    {
        std::ifstream in(root / "data" / "layer_ui_state.json");
        if (in) {
            try {
                in >> j;
            } catch (...) {
                j = json::object();
            }
        }
    }
    json f = json::object();
    f["enabled"] = filter_enabled;
    f["use_date"] = filter_use_date;
    f["year_min"] = filter_year_min;
    f["year_max"] = filter_year_max;
    f["blocklot"] = filter_blocklot ? filter_blocklot : "";
    f["status"] = filter_status ? filter_status : "";
    f["address"] = filter_address ? filter_address : "";
    f["owner"] = filter_owner ? filter_owner : "";
    f["zip"] = filter_zip ? filter_zip : "";
    f["crime_enabled"] = crime_filter_enabled;
    f["crime_homicide"] = crime_filter_homicide;
    f["crime_robbery"] = crime_filter_robbery;
    f["crime_assault"] = crime_filter_assault;
    f["crime_burglary"] = crime_filter_burglary;
    f["crime_theft"] = crime_filter_theft;
    f["crime_auto_theft"] = crime_filter_auto_theft;
    f["crime_drug"] = crime_filter_drug;
    f["crime_shooting"] = crime_filter_shooting;
    f["crime_use_year"] = crime_filter_use_year;
    f["crime_year_min"] = crime_year_min;
    f["crime_year_max"] = crime_year_max;
    if (owner_search_query) {
        f["owner_search_query"] = owner_search_query;
    } else if (j.contains("filters") && j["filters"].is_object()) {
        f["owner_search_query"] = j["filters"].value("owner_search_query", std::string());
    } else {
        f["owner_search_query"] = "";
    }
    json owners = json::array();
    for (const auto& owner : selected_owners) owners.push_back(owner);
    f["selected_owners"] = std::move(owners);
    json sectors = json::object();
    for (const auto& kv : event_sector_enabled) sectors[kv.first] = kv.second;
    f["event_sector_enabled"] = std::move(sectors);
    j["filters"] = f;
    if (!j.contains("filter_profiles") || !j["filter_profiles"].is_object()) j["filter_profiles"] = json::object();
    j["filter_profiles"][filterProfileKey()] = f;
    std::ofstream out(root / "data" / "layer_ui_state.json");
    if (out) out << j.dump(2);
}

void loadMapUiState(
    const fs::path& root,
    double* center_lon,
    double* center_lat,
    double* zoom,
    std::string* selected_parcel_entity_id,
    std::vector<std::string>* selected_parcel_entity_ids) {
    std::ifstream in(root / "data" / "layer_ui_state.json");
    if (!in) return;
    json j;
    try {
        in >> j;
    } catch (...) {
        return;
    }
    const json* map_view = nullptr;
    const std::string key = mapViewKey();
    if (j.contains("map_views") && j["map_views"].is_object()) {
        const json& map_views = j["map_views"];
        if (map_views.contains(key) && map_views[key].is_object()) map_view = &map_views[key];
    }
    if (!map_view && j.contains("map_view") && j["map_view"].is_object()) {
        map_view = &j["map_view"];
    }
    if (map_view) {
        const json& v = *map_view;
        if (center_lon && v.contains("center_lon") && v["center_lon"].is_number()) *center_lon = v["center_lon"].get<double>();
        if (center_lat && v.contains("center_lat") && v["center_lat"].is_number()) *center_lat = v["center_lat"].get<double>();
        if (zoom && v.contains("zoom") && v["zoom"].is_number()) *zoom = v["zoom"].get<double>();
    }
    if (j.contains("parcel_selection") && j["parcel_selection"].is_object()) {
        const json& s = j["parcel_selection"];
        if (selected_parcel_entity_id) {
            selected_parcel_entity_id->clear();
            if (s.contains("active_entity_id") && s["active_entity_id"].is_string()) {
                *selected_parcel_entity_id = s["active_entity_id"].get<std::string>();
            }
        }
        if (selected_parcel_entity_ids) {
            selected_parcel_entity_ids->clear();
            if (s.contains("entity_ids") && s["entity_ids"].is_array()) {
                for (const auto& v : s["entity_ids"]) {
                    if (v.is_string()) selected_parcel_entity_ids->push_back(v.get<std::string>());
                }
            }
        }
    }
}

void saveMapUiState(
    const fs::path& root,
    double center_lon,
    double center_lat,
    double zoom,
    const std::string& selected_parcel_entity_id,
    const std::vector<std::string>& selected_parcel_entity_ids) {
    fs::create_directories(root / "data");
    json j = json::object();
    {
        std::ifstream in(root / "data" / "layer_ui_state.json");
        if (in) {
            try {
                in >> j;
            } catch (...) {
                j = json::object();
            }
        }
    }
    const json map_view = {
        {"center_lon", center_lon},
        {"center_lat", center_lat},
        {"zoom", zoom}
    };
    j["map_view"] = map_view;
    if (!j.contains("map_views") || !j["map_views"].is_object()) j["map_views"] = json::object();
    j["map_views"][mapViewKey()] = map_view;
    json selection = json::object();
    selection["active_entity_id"] = selected_parcel_entity_id.empty() ? json(nullptr) : json(selected_parcel_entity_id);
    selection["entity_ids"] = json::array();
    for (const std::string& entity_id : selected_parcel_entity_ids) selection["entity_ids"].push_back(entity_id);
    j["parcel_selection"] = std::move(selection);
    std::ofstream out(root / "data" / "layer_ui_state.json");
    if (out) out << j.dump(2);
}

void loadLayerBrowseUiState(
    const fs::path& root,
    std::string* selected_nation_state,
    std::string* selected_state_region) {
    std::ifstream in(root / "data" / "layer_ui_state.json");
    if (!in) return;
    json j;
    try {
        in >> j;
    } catch (...) {
        return;
    }
    if (j.contains("layer_browser") && j["layer_browser"].is_object()) {
        const json& b = j["layer_browser"];
        if (selected_nation_state && b.contains("selected_nation_state") && b["selected_nation_state"].is_string()) {
            *selected_nation_state = b["selected_nation_state"].get<std::string>();
        }
        if (selected_state_region && b.contains("selected_state_region") && b["selected_state_region"].is_string()) {
            *selected_state_region = b["selected_state_region"].get<std::string>();
        }
        return;
    }
    if (j.contains("filters") && j["filters"].is_object()) {
        const json& f = j["filters"];
        if (selected_nation_state && f.contains("selected_nation_state") && f["selected_nation_state"].is_string()) {
            *selected_nation_state = f["selected_nation_state"].get<std::string>();
        }
        if (selected_state_region && f.contains("selected_state_region") && f["selected_state_region"].is_string()) {
            *selected_state_region = f["selected_state_region"].get<std::string>();
        }
    }
}

void saveLayerBrowseUiState(
    const fs::path& root,
    const std::string* selected_nation_state,
    const std::string* selected_state_region) {
    fs::create_directories(root / "data");
    json j = json::object();
    {
        std::ifstream in(root / "data" / "layer_ui_state.json");
        if (in) {
            try {
                in >> j;
            } catch (...) {
                j = json::object();
            }
        }
    }
    j["layer_browser"] = {
        {"selected_nation_state", selected_nation_state ? *selected_nation_state : "us"},
        {"selected_state_region", selected_state_region ? *selected_state_region : "md"}
    };
    std::ofstream out(root / "data" / "layer_ui_state.json");
    if (out) out << j.dump(2);
}

namespace {
json crimeFilterStateJson(const CrimeFilterState& crime) {
    return {
        {"enabled", crime.enabled},
        {"homicide", crime.homicide},
        {"robbery", crime.robbery},
        {"assault", crime.assault},
        {"burglary", crime.burglary},
        {"theft", crime.theft},
        {"auto_theft", crime.auto_theft},
        {"drug", crime.drug},
        {"shooting", crime.shooting},
        {"use_year", crime.use_year},
        {"year_min", crime.year_min},
        {"year_max", crime.year_max}
    };
}

void loadCrimeFilterStateFromJson(const json& j, CrimeFilterState& crime) {
    if (!j.is_object()) return;
    if (j.contains("enabled") && j["enabled"].is_boolean()) crime.enabled = j["enabled"].get<bool>();
    if (j.contains("homicide") && j["homicide"].is_boolean()) crime.homicide = j["homicide"].get<bool>();
    if (j.contains("robbery") && j["robbery"].is_boolean()) crime.robbery = j["robbery"].get<bool>();
    if (j.contains("assault") && j["assault"].is_boolean()) crime.assault = j["assault"].get<bool>();
    if (j.contains("burglary") && j["burglary"].is_boolean()) crime.burglary = j["burglary"].get<bool>();
    if (j.contains("theft") && j["theft"].is_boolean()) crime.theft = j["theft"].get<bool>();
    if (j.contains("auto_theft") && j["auto_theft"].is_boolean()) crime.auto_theft = j["auto_theft"].get<bool>();
    if (j.contains("drug") && j["drug"].is_boolean()) crime.drug = j["drug"].get<bool>();
    if (j.contains("shooting") && j["shooting"].is_boolean()) crime.shooting = j["shooting"].get<bool>();
    if (j.contains("use_year") && j["use_year"].is_boolean()) crime.use_year = j["use_year"].get<bool>();
    if (j.contains("year_min") && j["year_min"].is_number_integer()) crime.year_min = j["year_min"].get<int>();
    if (j.contains("year_max") && j["year_max"].is_number_integer()) crime.year_max = j["year_max"].get<int>();
}

json queryHistoryEntryJson(const QueryHistoryEntry& entry) {
    json selected_owners = json::array();
    for (const auto& owner : entry.snapshot.selected_owners) selected_owners.push_back(owner);
    json selected_parcel_blocklots = json::array();
    for (const auto& blocklot : entry.snapshot.selected_parcel_blocklots) selected_parcel_blocklots.push_back(blocklot);
    json event_sectors = json::object();
    for (const auto& kv : entry.snapshot.event_sector_enabled) event_sectors[kv.first] = kv.second;
    return {
        {"executed_at_utc", entry.executed_at_utc},
        {"mode", entry.mode},
        {"name", entry.name},
        {"sql", entry.sql},
        {"color", json::array({entry.color[0], entry.color[1], entry.color[2], entry.color[3]})},
        {"row_count", entry.row_count},
        {"status", entry.status},
        {"snapshot", {
            {"filter_enabled", entry.snapshot.filter_enabled},
            {"filter_use_date", entry.snapshot.filter_use_date},
            {"filter_year_min", entry.snapshot.filter_year_min},
            {"filter_year_max", entry.snapshot.filter_year_max},
            {"filter_blocklot", entry.snapshot.filter_blocklot},
            {"filter_status", entry.snapshot.filter_status},
            {"filter_address", entry.snapshot.filter_address},
            {"filter_owner", entry.snapshot.filter_owner},
            {"filter_zip", entry.snapshot.filter_zip},
            {"crime", crimeFilterStateJson(entry.snapshot.crime)},
            {"selected_owners", std::move(selected_owners)},
            {"selected_parcel_blocklots", std::move(selected_parcel_blocklots)},
            {"event_sector_enabled", std::move(event_sectors)},
            {"center_lon", entry.snapshot.center_lon},
            {"center_lat", entry.snapshot.center_lat},
            {"zoom", entry.snapshot.zoom},
            {"map_title_text", entry.snapshot.map_title_text},
            {"map_title_show_primary_parcel_source", entry.snapshot.map_title_show_primary_parcel_source},
            {"map_title_source_layer_file", entry.snapshot.map_title_source_layer_file}
        }}
    };
}

void loadQueryHistoryEntryFromJson(const json& j, QueryHistoryEntry& entry) {
    if (!j.is_object()) return;
    if (j.contains("executed_at_utc") && j["executed_at_utc"].is_string()) entry.executed_at_utc = j["executed_at_utc"].get<std::string>();
    if (j.contains("mode") && j["mode"].is_string()) entry.mode = j["mode"].get<std::string>();
    if (j.contains("name") && j["name"].is_string()) entry.name = j["name"].get<std::string>();
    if (j.contains("sql") && j["sql"].is_string()) entry.sql = j["sql"].get<std::string>();
    if (j.contains("color") && j["color"].is_array()) {
        for (int i = 0; i < 4 && i < (int)j["color"].size(); ++i) {
            if (j["color"][i].is_number()) entry.color[i] = j["color"][i].get<float>();
        }
    }
    if (j.contains("row_count") && j["row_count"].is_number_unsigned()) entry.row_count = j["row_count"].get<size_t>();
    if (j.contains("status") && j["status"].is_string()) entry.status = j["status"].get<std::string>();
    if (!j.contains("snapshot") || !j["snapshot"].is_object()) return;
    const json& s = j["snapshot"];
    if (s.contains("filter_enabled") && s["filter_enabled"].is_boolean()) entry.snapshot.filter_enabled = s["filter_enabled"].get<bool>();
    if (s.contains("filter_use_date") && s["filter_use_date"].is_boolean()) entry.snapshot.filter_use_date = s["filter_use_date"].get<bool>();
    if (s.contains("filter_year_min") && s["filter_year_min"].is_number_integer()) entry.snapshot.filter_year_min = s["filter_year_min"].get<int>();
    if (s.contains("filter_year_max") && s["filter_year_max"].is_number_integer()) entry.snapshot.filter_year_max = s["filter_year_max"].get<int>();
    if (s.contains("filter_blocklot") && s["filter_blocklot"].is_string()) entry.snapshot.filter_blocklot = s["filter_blocklot"].get<std::string>();
    if (s.contains("filter_status") && s["filter_status"].is_string()) entry.snapshot.filter_status = s["filter_status"].get<std::string>();
    if (s.contains("filter_address") && s["filter_address"].is_string()) entry.snapshot.filter_address = s["filter_address"].get<std::string>();
    if (s.contains("filter_owner") && s["filter_owner"].is_string()) entry.snapshot.filter_owner = s["filter_owner"].get<std::string>();
    if (s.contains("filter_zip") && s["filter_zip"].is_string()) entry.snapshot.filter_zip = s["filter_zip"].get<std::string>();
    if (s.contains("crime")) loadCrimeFilterStateFromJson(s["crime"], entry.snapshot.crime);
    if (s.contains("selected_owners") && s["selected_owners"].is_array()) {
        entry.snapshot.selected_owners.clear();
        for (const auto& owner : s["selected_owners"]) {
            if (owner.is_string()) entry.snapshot.selected_owners.push_back(owner.get<std::string>());
        }
    }
    if (s.contains("selected_parcel_blocklots") && s["selected_parcel_blocklots"].is_array()) {
        entry.snapshot.selected_parcel_blocklots.clear();
        for (const auto& blocklot : s["selected_parcel_blocklots"]) {
            if (blocklot.is_string()) entry.snapshot.selected_parcel_blocklots.push_back(blocklot.get<std::string>());
        }
    }
    if (s.contains("event_sector_enabled") && s["event_sector_enabled"].is_object()) {
        entry.snapshot.event_sector_enabled.clear();
        for (auto it = s["event_sector_enabled"].begin(); it != s["event_sector_enabled"].end(); ++it) {
            if (it.value().is_boolean()) entry.snapshot.event_sector_enabled[it.key()] = it.value().get<bool>();
        }
    }
    if (s.contains("center_lon") && s["center_lon"].is_number()) entry.snapshot.center_lon = s["center_lon"].get<double>();
    if (s.contains("center_lat") && s["center_lat"].is_number()) entry.snapshot.center_lat = s["center_lat"].get<double>();
    if (s.contains("zoom") && s["zoom"].is_number()) entry.snapshot.zoom = s["zoom"].get<double>();
    if (s.contains("map_title_text") && s["map_title_text"].is_string()) entry.snapshot.map_title_text = s["map_title_text"].get<std::string>();
    if (s.contains("map_title_show_primary_parcel_source") && s["map_title_show_primary_parcel_source"].is_boolean()) {
        entry.snapshot.map_title_show_primary_parcel_source = s["map_title_show_primary_parcel_source"].get<bool>();
    }
    if (s.contains("map_title_source_layer_file") && s["map_title_source_layer_file"].is_string()) {
        entry.snapshot.map_title_source_layer_file = s["map_title_source_layer_file"].get<std::string>();
    }
}
}

void loadQueryHistoryUiState(
    const fs::path& root,
    std::vector<QueryHistoryEntry>* query_history) {
    if (!query_history) return;
    std::ifstream in(root / "data" / "layer_ui_state.json");
    if (!in) return;
    json j;
    try {
        in >> j;
    } catch (...) {
        return;
    }
    if (!j.contains("query_history") || !j["query_history"].is_array()) return;
    query_history->clear();
    for (const auto& item : j["query_history"]) {
        QueryHistoryEntry entry;
        loadQueryHistoryEntryFromJson(item, entry);
        if (!entry.sql.empty()) query_history->push_back(std::move(entry));
    }
}

void saveQueryHistoryUiState(
    const fs::path& root,
    const std::vector<QueryHistoryEntry>& query_history) {
    fs::create_directories(root / "data");
    json j = json::object();
    {
        std::ifstream in(root / "data" / "layer_ui_state.json");
        if (in) {
            try {
                in >> j;
            } catch (...) {
                j = json::object();
            }
        }
    }
    json out_history = json::array();
    for (const auto& entry : query_history) out_history.push_back(queryHistoryEntryJson(entry));
    j["query_history"] = std::move(out_history);
    std::ofstream out(root / "data" / "layer_ui_state.json");
    if (out) out << j.dump(2);
}
