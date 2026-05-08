#include "layer_state_io.h"

#include "aggregate_visualization_strategies.h"

#include <algorithm>
#include <cctype>
#include <fstream>

#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

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
        ld.heatmap_field = arr[i].contains("heatmap_field") ? arr[i]["heatmap_field"].get<std::string>() : "";
        ld.subcategory = arr[i].contains("subcategory") ? arr[i]["subcategory"].get<std::string>() : "";
        ld.scale = arr[i].contains("scale") ? arr[i]["scale"].get<std::string>() : "";
        std::string c = arr[i]["color"].get<std::string>();
        auto hex = [&](int s) { return std::stoi(c.substr(s, 2), nullptr, 16) / 255.0f; };
        ld.color = ImVec4(hex(1), hex(3), hex(5), 1.0f);
        ld.enabled = arr[i].contains("default_enabled") ? arr[i]["default_enabled"].get<bool>() : (i < 4);
        ld.category = parseCategory(arr[i], ld.name);
        layers.push_back(std::move(ld));
    }
    return layers;
}

void loadLayerUiState(
    const fs::path& root,
    std::vector<LayerDef>& layers,
    bool& hover_inspector_enabled,
    int* hover_inspector_mode,
    std::unordered_map<std::string, bool>* zoning_zone_enabled,
    std::vector<bool>* layer_fill_enabled,
    std::vector<bool>* layer_hover_enabled,
    std::vector<bool>* layer_inspect_enabled,
    std::vector<bool>* layer_heatmap_enabled,
    std::vector<int>* layer_heatmap_max_zoom,
    std::vector<int>* layer_parcel_detail_min_zoom,
    std::vector<bool>* layer_heatmap_use_gradient,
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
    float* heatmap_multires_blend,
    bool* heatmap_allow_cpu_fallback) {
    std::ifstream in(root / "data" / "layer_ui_state.json");
    if (!in) return;
    json j;
    try {
        in >> j;
    } catch (...) {
        return;
    }
    bool have_mode = false;
    if (hover_inspector_mode && j.contains("hover_inspector_mode") && j["hover_inspector_mode"].is_number_integer()) {
        *hover_inspector_mode = std::clamp(j["hover_inspector_mode"].get<int>(), 0, 3);
        hover_inspector_enabled = *hover_inspector_mode != 0;
        have_mode = true;
    }
    if (!have_mode) {
        if (j.contains("hover_inspector_enabled") && j["hover_inspector_enabled"].is_boolean()) {
            hover_inspector_enabled = j["hover_inspector_enabled"].get<bool>();
        } else if (j.contains("parcel_hover_enabled") && j["parcel_hover_enabled"].is_boolean()) {
            hover_inspector_enabled = j["parcel_hover_enabled"].get<bool>();
        }
        if (hover_inspector_mode) *hover_inspector_mode = hover_inspector_enabled ? 3 : 0;
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
    if (layer_heatmap_enabled && j.contains("layer_heatmaps") && j["layer_heatmaps"].is_object()) {
        const auto& obj = j["layer_heatmaps"];
        if (layer_heatmap_enabled->size() < layers.size()) layer_heatmap_enabled->resize(layers.size(), true);
        for (size_t i = 0; i < layers.size(); ++i) {
            if (obj.contains(layers[i].file) && obj[layers[i].file].is_boolean()) {
                (*layer_heatmap_enabled)[i] = obj[layers[i].file].get<bool>();
            }
        }
    }
    if (layer_heatmap_max_zoom && j.contains("layer_heatmap_max_zoom") && j["layer_heatmap_max_zoom"].is_object()) {
        const auto& obj = j["layer_heatmap_max_zoom"];
        if (layer_heatmap_max_zoom->size() < layers.size()) layer_heatmap_max_zoom->resize(layers.size(), 13);
        for (size_t i = 0; i < layers.size(); ++i) {
            if (obj.contains(layers[i].file) && obj[layers[i].file].is_number_integer()) {
                (*layer_heatmap_max_zoom)[i] = obj[layers[i].file].get<int>();
            }
        }
    }
    if (layer_parcel_detail_min_zoom && j.contains("layer_parcel_detail_min_zoom") && j["layer_parcel_detail_min_zoom"].is_object()) {
        const auto& obj = j["layer_parcel_detail_min_zoom"];
        if (layer_parcel_detail_min_zoom->size() < layers.size()) layer_parcel_detail_min_zoom->resize(layers.size(), 14);
        for (size_t i = 0; i < layers.size(); ++i) {
            if (obj.contains(layers[i].file) && obj[layers[i].file].is_number_integer()) {
                (*layer_parcel_detail_min_zoom)[i] = obj[layers[i].file].get<int>();
            }
        }
    }
    if (layer_heatmap_use_gradient && j.contains("layer_heatmap_use_gradient") && j["layer_heatmap_use_gradient"].is_object()) {
        const auto& obj = j["layer_heatmap_use_gradient"];
        if (layer_heatmap_use_gradient->size() < layers.size()) layer_heatmap_use_gradient->resize(layers.size(), true);
        for (size_t i = 0; i < layers.size(); ++i) {
            if (obj.contains(layers[i].file) && obj[layers[i].file].is_boolean()) {
                (*layer_heatmap_use_gradient)[i] = obj[layers[i].file].get<bool>();
            }
        }
    }
    if (layer_heatmap_algo && j.contains("layer_heatmap_algo") && j["layer_heatmap_algo"].is_object()) {
        const auto& obj = j["layer_heatmap_algo"];
        if (layer_heatmap_algo->size() < layers.size()) layer_heatmap_algo->resize(layers.size(), -1);
        for (size_t i = 0; i < layers.size(); ++i) {
            if (obj.contains(layers[i].file) && obj[layers[i].file].is_number_integer()) {
                (*layer_heatmap_algo)[i] = obj[layers[i].file].get<int>();
            }
        }
    }
    if (layer_normalize_mode && j.contains("layer_normalize_mode") && j["layer_normalize_mode"].is_object()) {
        const auto& obj = j["layer_normalize_mode"];
        if (layer_normalize_mode->size() < layers.size()) layer_normalize_mode->resize(layers.size(), 0);
        for (size_t i = 0; i < layers.size(); ++i) {
            if (obj.contains(layers[i].file) && obj[layers[i].file].is_number_integer()) {
                (*layer_normalize_mode)[i] = std::clamp(obj[layers[i].file].get<int>(), 0, 2);
            }
        }
    }
    auto load_float_layer_setting = [&](std::vector<float>* dst, const char* key, float fallback) {
        if (!dst || !j.contains(key) || !j[key].is_object()) return;
        const auto& obj = j[key];
        if (dst->size() < layers.size()) dst->resize(layers.size(), fallback);
        for (size_t i = 0; i < layers.size(); ++i) {
            if (obj.contains(layers[i].file) && obj[layers[i].file].is_number()) {
                (*dst)[i] = obj[layers[i].file].get<float>();
            }
        }
    };
    auto load_bool_layer_setting = [&](std::vector<bool>* dst, const char* key, bool fallback) {
        if (!dst || !j.contains(key) || !j[key].is_object()) return;
        const auto& obj = j[key];
        if (dst->size() < layers.size()) dst->resize(layers.size(), fallback);
        for (size_t i = 0; i < layers.size(); ++i) {
            if (obj.contains(layers[i].file) && obj[layers[i].file].is_boolean()) {
                (*dst)[i] = obj[layers[i].file].get<bool>();
            }
        }
    };
    load_float_layer_setting(layer_heatmap_cell_px, "layer_heatmap_cell_px", 24.0f);
    load_float_layer_setting(layer_heatmap_bandwidth_px, "layer_heatmap_bandwidth_px", 18.0f);
    load_float_layer_setting(layer_heatmap_blur_sigma_px, "layer_heatmap_blur_sigma_px", 6.0f);
    load_float_layer_setting(layer_heatmap_percentile_clip, "layer_heatmap_percentile_clip", 95.0f);
    load_bool_layer_setting(layer_heatmap_zoom_adaptive_bandwidth, "layer_heatmap_zoom_adaptive_bandwidth", true);
    load_bool_layer_setting(layer_heatmap_multires_enabled, "layer_heatmap_multires_enabled", true);
    load_float_layer_setting(layer_heatmap_multires_blend, "layer_heatmap_multires_blend", 0.5f);
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
        if (heatmap_allow_cpu_fallback && hs.contains("allow_cpu_fallback") && hs["allow_cpu_fallback"].is_boolean()) *heatmap_allow_cpu_fallback = hs["allow_cpu_fallback"].get<bool>();
    }
}

void saveLayerUiState(
    const fs::path& root,
    const std::vector<LayerDef>& layers,
    bool hover_inspector_enabled,
    const int* hover_inspector_mode,
    const std::unordered_map<std::string, bool>* zoning_zone_enabled,
    const std::vector<bool>* layer_fill_enabled,
    const std::vector<bool>* layer_hover_enabled,
    const std::vector<bool>* layer_inspect_enabled,
    const std::vector<bool>* layer_heatmap_enabled,
    const std::vector<int>* layer_heatmap_max_zoom,
    const std::vector<int>* layer_parcel_detail_min_zoom,
    const std::vector<bool>* layer_heatmap_use_gradient,
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
    const float* heatmap_multires_blend,
    const bool* heatmap_allow_cpu_fallback) {
    fs::create_directories(root / "data");
    json j;
    j["hover_inspector_enabled"] = hover_inspector_enabled;
    if (hover_inspector_mode) j["hover_inspector_mode"] = std::clamp(*hover_inspector_mode, 0, 3);
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
    if (layer_heatmap_enabled) {
        json h = json::object();
        for (size_t i = 0; i < layers.size(); ++i) {
            h[layers[i].file] = i < layer_heatmap_enabled->size() ? (*layer_heatmap_enabled)[i] : true;
        }
        j["layer_heatmaps"] = std::move(h);
    }
    if (layer_heatmap_max_zoom) {
        json hz = json::object();
        for (size_t i = 0; i < layers.size(); ++i) {
            hz[layers[i].file] = i < layer_heatmap_max_zoom->size() ? (*layer_heatmap_max_zoom)[i] : 13;
        }
        j["layer_heatmap_max_zoom"] = std::move(hz);
    }
    if (layer_parcel_detail_min_zoom) {
        json pz = json::object();
        for (size_t i = 0; i < layers.size(); ++i) {
            pz[layers[i].file] = i < layer_parcel_detail_min_zoom->size() ? (*layer_parcel_detail_min_zoom)[i] : kParcelChoroplethMinZoom;
        }
        j["layer_parcel_detail_min_zoom"] = std::move(pz);
    }
    if (layer_heatmap_use_gradient) {
        json hg = json::object();
        for (size_t i = 0; i < layers.size(); ++i) {
            hg[layers[i].file] = i < layer_heatmap_use_gradient->size() ? (*layer_heatmap_use_gradient)[i] : true;
        }
        j["layer_heatmap_use_gradient"] = std::move(hg);
    }
    if (layer_heatmap_algo) {
        json ha = json::object();
        for (size_t i = 0; i < layers.size(); ++i) {
            ha[layers[i].file] = i < layer_heatmap_algo->size() ? (*layer_heatmap_algo)[i] : -1;
        }
        j["layer_heatmap_algo"] = std::move(ha);
    }
    if (layer_normalize_mode) {
        json nm = json::object();
        for (size_t i = 0; i < layers.size(); ++i) {
            nm[layers[i].file] = i < layer_normalize_mode->size() ? (*layer_normalize_mode)[i] : 0;
        }
        j["layer_normalize_mode"] = std::move(nm);
    }
    auto save_float_layer_setting = [&](const std::vector<float>* src, const char* key, float fallback) {
        if (!src) return;
        json obj = json::object();
        for (size_t i = 0; i < layers.size(); ++i) {
            obj[layers[i].file] = i < src->size() ? (*src)[i] : fallback;
        }
        j[key] = std::move(obj);
    };
    auto save_bool_layer_setting = [&](const std::vector<bool>* src, const char* key, bool fallback) {
        if (!src) return;
        json obj = json::object();
        for (size_t i = 0; i < layers.size(); ++i) {
            obj[layers[i].file] = i < src->size() ? (*src)[i] : fallback;
        }
        j[key] = std::move(obj);
    };
    save_float_layer_setting(layer_heatmap_cell_px, "layer_heatmap_cell_px", 24.0f);
    save_float_layer_setting(layer_heatmap_bandwidth_px, "layer_heatmap_bandwidth_px", 18.0f);
    save_float_layer_setting(layer_heatmap_blur_sigma_px, "layer_heatmap_blur_sigma_px", 6.0f);
    save_float_layer_setting(layer_heatmap_percentile_clip, "layer_heatmap_percentile_clip", 95.0f);
    save_bool_layer_setting(layer_heatmap_zoom_adaptive_bandwidth, "layer_heatmap_zoom_adaptive_bandwidth", true);
    save_bool_layer_setting(layer_heatmap_multires_enabled, "layer_heatmap_multires_enabled", true);
    save_float_layer_setting(layer_heatmap_multires_blend, "layer_heatmap_multires_blend", 0.5f);
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
        if (heatmap_allow_cpu_fallback) hs["allow_cpu_fallback"] = *heatmap_allow_cpu_fallback;
        j["heatmap_settings"] = std::move(hs);
    }
    std::ofstream out(root / "data" / "layer_ui_state.json");
    if (out) out << j.dump(2);
}
