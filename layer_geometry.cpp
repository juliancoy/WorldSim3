#include "layer_geometry.h"

#include "geo.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <optional>

#include "earcut.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

std::string jsonValueToString(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number_unsigned()) return std::to_string(v.get<unsigned long long>());
    if (v.is_number_float()) return std::to_string(v.get<double>());
    if (v.is_null()) return "";
    return v.dump();
}

std::vector<LayerDef::FeatureGeom> extractFeatureGeoms(const json& geom) {
    std::vector<LayerDef::FeatureGeom> out;
    if (!geom.contains("type") || !geom.contains("coordinates")) return out;
    const std::string t = geom["type"].get<std::string>();

    auto build_from_polygon = [](const json& poly_coords) -> std::optional<LayerDef::FeatureGeom> {
        LayerDef::FeatureGeom fg{};
        bool has = false;
        auto expand = [&](double lon, double lat) {
            if (!has) {
                fg.extent.min_lon = fg.extent.max_lon = (float)lon;
                fg.extent.min_lat = fg.extent.max_lat = (float)lat;
                has = true;
                return;
            }
            fg.extent.min_lon = std::min(fg.extent.min_lon, (float)lon);
            fg.extent.min_lat = std::min(fg.extent.min_lat, (float)lat);
            fg.extent.max_lon = std::max(fg.extent.max_lon, (float)lon);
            fg.extent.max_lat = std::max(fg.extent.max_lat, (float)lat);
        };

        for (const auto& ring_json : poly_coords) {
            std::vector<ImVec2> ring;
            for (const auto& p : ring_json) {
                if (p.size() < 2) continue;
                double lon = p[0].get<double>();
                double lat = p[1].get<double>();
                ring.push_back(ImVec2((float)lon, (float)lat));
                expand(lon, lat);
            }
            if (ring.size() >= 3) fg.rings.push_back(std::move(ring));
        }
        if (!has || fg.rings.empty()) return std::nullopt;

        return fg;
    };

    if (t == "Polygon") {
        auto fg = build_from_polygon(geom["coordinates"]);
        if (fg) out.push_back(std::move(*fg));
    } else if (t == "MultiPolygon") {
        for (const auto& poly : geom["coordinates"]) {
            auto fg = build_from_polygon(poly);
            if (fg) out.push_back(std::move(*fg));
        }
    } else if (t == "Point") {
        const auto& c = geom["coordinates"];
        if (c.is_array() && c.size() >= 2) {
            LayerDef::FeatureGeom fg{};
            double lon = c[0].get<double>();
            double lat = c[1].get<double>();
            fg.extent.min_lon = fg.extent.max_lon = (float)lon;
            fg.extent.min_lat = fg.extent.max_lat = (float)lat;
            out.push_back(std::move(fg));
        }
    } else if (t == "MultiPoint") {
        for (const auto& c : geom["coordinates"]) {
            if (!c.is_array() || c.size() < 2) continue;
            LayerDef::FeatureGeom fg{};
            double lon = c[0].get<double>();
            double lat = c[1].get<double>();
            fg.extent.min_lon = fg.extent.max_lon = (float)lon;
            fg.extent.min_lat = fg.extent.max_lat = (float)lat;
            out.push_back(std::move(fg));
        }
    }
    return out;
}

std::vector<LayerDef::FeatureGeom> loadLayerPointsFromFile(const fs::path& full_path) {
    std::vector<LayerDef::FeatureGeom> features;
    std::ifstream in(full_path);
    if (!in) return features;
    json j;
    in >> j;
    if (!j.contains("features")) return features;
    for (auto& f : j["features"]) {
        if (!f.contains("geometry")) continue;
        auto geoms = extractFeatureGeoms(f["geometry"]);
        std::vector<std::pair<std::string, std::string>> props;
        if (f.contains("properties") && f["properties"].is_object()) {
            props.reserve(f["properties"].size());
            for (auto it = f["properties"].begin(); it != f["properties"].end(); ++it) {
                props.push_back({it.key(), jsonValueToString(it.value())});
            }
        }
        for (auto& g : geoms) {
            g.properties = props;
            features.push_back(std::move(g));
        }
    }
    return features;
}

bool pointInRing(const std::vector<ImVec2>& ring, float x, float y) {
    bool inside = false;
    const size_t n = ring.size();
    if (n < 3) return false;
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const float xi = ring[i].x, yi = ring[i].y;
        const float xj = ring[j].x, yj = ring[j].y;
        const bool intersect = ((yi > y) != (yj > y)) &&
                               (x < (xj - xi) * (y - yi) / ((yj - yi) == 0.0f ? 1e-9f : (yj - yi)) + xi);
        if (intersect) inside = !inside;
    }
    return inside;
}

bool pointInFeature(const LayerDef::FeatureGeom& fg, float lon, float lat) {
    if (fg.rings.empty()) return false;
    if (!pointInRing(fg.rings[0], lon, lat)) return false;
    for (size_t i = 1; i < fg.rings.size(); ++i) {
        if (pointInRing(fg.rings[i], lon, lat)) return false;
    }
    return true;
}

int lodRingStepForZoom(int zoom) {
    if (zoom <= 10) return 8;
    if (zoom == 11) return 6;
    if (zoom == 12) return 4;
    if (zoom == 13) return 2;
    return 1;
}

bool implausibleHydrationCache(const std::string& file, size_t feature_count) {
    // These guards reject previously observed duplicated cache payloads. They are
    // intentionally loose so legitimate source updates still load from cache.
    if (file == "parcel.geojson") return feature_count > 300000;
    if (file == "zoning.geojson") return feature_count > 10000;
    if (file == "vacant_building_notices.geojson") return feature_count > 20000;
    if (file == "vacant_building_rehabs.geojson") return feature_count > 20000;
    return false;
}

void hydrateLayerBatches(
    const fs::path& full_path,
    size_t batch_size,
    const std::atomic<bool>& stop_flag,
    const std::function<bool()>& should_continue,
    const std::function<void(std::vector<LayerDef::FeatureGeom>&&, bool, bool, const std::string&)>& emit) {
    auto stream_features = [&](std::function<bool(json&&)> on_feature, std::string& err) -> bool {
        std::ifstream in(full_path, std::ios::binary);
        if (!in) {
            err = "failed to open layer file";
            return false;
        }
        std::string token;
        token.reserve(10);
        bool in_string = false;
        bool escape = false;
        bool found_features_key = false;
        bool in_features_array = false;
        bool collecting_feature = false;
        int obj_depth = 0;
        std::string feature_buf;
        feature_buf.reserve(65536);

        char ch = 0;
        while (in.get(ch)) {
            if (!in_features_array) {
                if (!found_features_key) {
                    if (!in_string) {
                        if (ch == '"') {
                            in_string = true;
                            token.clear();
                        }
                    } else {
                        if (escape) {
                            token.push_back(ch);
                            escape = false;
                        } else if (ch == '\\') {
                            escape = true;
                        } else if (ch == '"') {
                            in_string = false;
                            if (token == "features") found_features_key = true;
                        } else {
                            token.push_back(ch);
                        }
                    }
                } else {
                    if (ch == '[') {
                        in_features_array = true;
                    }
                }
                continue;
            }

            if (!collecting_feature) {
                if (ch == '{') {
                    collecting_feature = true;
                    obj_depth = 1;
                    in_string = false;
                    escape = false;
                    feature_buf.clear();
                    feature_buf.push_back(ch);
                } else if (ch == ']') {
                    return true;
                }
                continue;
            }

            feature_buf.push_back(ch);
            if (in_string) {
                if (escape) escape = false;
                else if (ch == '\\') escape = true;
                else if (ch == '"') in_string = false;
                continue;
            }
            if (ch == '"') {
                in_string = true;
                continue;
            }
            if (ch == '{') obj_depth++;
            else if (ch == '}') {
                obj_depth--;
                if (obj_depth == 0) {
                    try {
                        json f = json::parse(feature_buf);
                        if (!on_feature(std::move(f))) return true;
                    } catch (const std::exception& e) {
                        err = std::string("feature parse failed: ") + e.what();
                        return false;
                    }
                    collecting_feature = false;
                }
            }
        }
        err = found_features_key ? "invalid geojson: unterminated features array" : "invalid geojson: missing features array";
        return false;
    };

    std::vector<LayerDef::FeatureGeom> batch;
    batch.reserve(batch_size);
    std::string stream_err;
    bool stream_aborted = false;
    const bool ok = stream_features([&](json&& f) -> bool {
        if (stop_flag.load(std::memory_order_relaxed) || !should_continue()) {
            stream_aborted = true;
            return false;
        }
        if (!f.contains("geometry")) return true;
        std::vector<std::pair<std::string, std::string>> props;
        if (f.contains("properties") && f["properties"].is_object()) {
            props.reserve(f["properties"].size());
            for (auto it = f["properties"].begin(); it != f["properties"].end(); ++it) {
                if (it.value().is_null()) continue;
                if (it.value().is_string()) props.push_back({it.key(), it.value().get<std::string>()});
                else props.push_back({it.key(), it.value().dump()});
            }
        }
        auto geoms = extractFeatureGeoms(f["geometry"]);
        for (auto& g : geoms) {
            if (stop_flag.load(std::memory_order_relaxed) || !should_continue()) {
                stream_aborted = true;
                return false;
            }
            g.properties = props;
            batch.push_back(std::move(g));
            if (batch.size() >= batch_size) {
                emit(std::move(batch), false, false, "");
                batch.clear();
                batch.reserve(batch_size);
            }
        }
        return true;
    }, stream_err);
    if (!ok) {
        emit({}, true, true, stream_err.empty() ? "feature streaming failed" : stream_err);
        return;
    }
    if (stream_aborted) return;
    if (!batch.empty()) emit(std::move(batch), false, false, "");
    emit({}, true, false, "");
}

void loadLayerPoints(LayerDef& layer, const fs::path& root) {
    layer.features = loadLayerPointsFromFile(root / "data" / "layers" / layer.file);
}

std::vector<uint32_t> triangulateRings(const std::vector<std::vector<ImVec2>>& rings) {
    using Pt = std::array<double, 2>;
    std::vector<std::vector<Pt>> poly;
    poly.reserve(rings.size());
    for (const auto& ring : rings) {
        std::vector<Pt> rp;
        rp.reserve(ring.size());
        for (const ImVec2& p : ring) rp.push_back(Pt{(double)p.x, (double)p.y});
        poly.push_back(std::move(rp));
    }
    return mapbox::earcut<uint32_t>(poly);
}

void appendRingScreenPointsLod(
    const std::vector<ImVec2>& ring,
    int ring_step,
    int math_zoom,
    const ImVec2& center_world,
    const ImVec2& origin,
    const ImVec2& size,
    double zoom_scale,
    std::vector<ImVec2>& out) {
    if (ring.empty()) return;
    const int step = std::max(1, ring_step);
    const size_t n = ring.size();
    if (step == 1 || n <= 4) {
        for (const ImVec2& ll : ring) {
            ImVec2 pw = lonLatToWorldPx(ll.x, ll.y, math_zoom);
            out.push_back(worldToScreen(pw, center_world, origin, size, zoom_scale));
        }
        return;
    }
    out.reserve(out.size() + (n / (size_t)step) + 2);
    for (size_t i = 0; i < n; i += (size_t)step) {
        ImVec2 pw = lonLatToWorldPx(ring[i].x, ring[i].y, math_zoom);
        out.push_back(worldToScreen(pw, center_world, origin, size, zoom_scale));
    }
    if ((n - 1) % (size_t)step != 0) {
        ImVec2 pw = lonLatToWorldPx(ring.back().x, ring.back().y, math_zoom);
        out.push_back(worldToScreen(pw, center_world, origin, size, zoom_scale));
    }
}

void appendWorldRingScreenPointsLod(
    const std::vector<ImVec2>& world_ring,
    int ring_step,
    const ImVec2& center_world,
    const ImVec2& origin,
    const ImVec2& size,
    double zoom_scale,
    std::vector<ImVec2>& out) {
    if (world_ring.empty()) return;
    const int step = std::max(1, ring_step);
    const size_t n = world_ring.size();
    if (step == 1 || n <= 4) {
        for (const ImVec2& wp : world_ring) out.push_back(worldToScreen(wp, center_world, origin, size, zoom_scale));
        return;
    }
    out.reserve(out.size() + (n / (size_t)step) + 2);
    for (size_t i = 0; i < n; i += (size_t)step) out.push_back(worldToScreen(world_ring[i], center_world, origin, size, zoom_scale));
    if ((n - 1) % (size_t)step != 0) out.push_back(worldToScreen(world_ring.back(), center_world, origin, size, zoom_scale));
}


