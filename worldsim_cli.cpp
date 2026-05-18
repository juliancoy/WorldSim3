#include "worldsim_cli.h"

#include "app_utils.h"
#include "cache_io.h"
#include "duckdb_analytics.h"
#include "headless_layer_hydration.h"
#include "layer_geometry.h"
#include "layer_state_io.h"
#include "layer_pipeline_drain.h"
#include "map_render_projection.h"
#include "parcel_consolidation.h"
#include "profiling_layer_snapshot.h"
#include "render_layer_pass.h"
#include "render_plan_builder.h"
#include "render_policy.h"
#include "worldsim_dataset_bootstrap.h"
#include "worldsim_app.h"
#include "parcel_matched_layers.h"
#include "vacancy_overlay.h"

#include <duckdb.hpp>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <deque>
#include <fstream>
#include <iostream>
#include <thread>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
bool nearlyEqual(float a, float b) {
    return std::fabs(a - b) <= 0.00001f;
}

bool isBareLayerFilename(const std::string& file) {
    return !file.empty() &&
           file.find('/') == std::string::npos &&
           file.find('\\') == std::string::npos;
}

std::string canonicalBinaryPathForLayerFile(const std::string& file) {
    return file + ".canonical.bin";
}

bool collectHydratedFeaturesFromGeoJson(
    const fs::path& layer_path,
    std::vector<LayerDef::FeatureGeom>& out,
    std::string& error) {
    std::atomic<bool> stop{false};
    out.clear();
    hydrateLayerBatches(
        layer_path,
        2048,
        stop,
        []() { return true; },
        [&](std::vector<LayerDef::FeatureGeom>&& chunk, bool done, bool failed, const std::string& err) {
            if (!chunk.empty()) {
                out.insert(
                    out.end(),
                    std::make_move_iterator(chunk.begin()),
                    std::make_move_iterator(chunk.end()));
            }
            if (failed) error = err;
            if (done && error.empty()) error.clear();
        });
    return error.empty();
}

bool ensureHydrationCacheReady(
    const fs::path& root,
    const std::string& file,
    const std::string& sig,
    std::vector<LayerDef::FeatureGeom>& features,
    std::string& source_used,
    std::string& error) {
    const fs::path layer_path = resolveStoredLayerPathForFile(root, file);
    const fs::path binary_path = root / "data" / "cache" / "hydration" / (file + ".bin");
    const fs::path canonical_path = layer_path.parent_path() / canonicalBinaryPathForLayerFile(file);

    if (loadBinaryHydrationCache(binary_path, sig, features)) {
        source_used = "binary";
        return true;
    }
    if (loadBinaryCanonicalFeatureCollection(canonical_path, sig, features)) {
        saveBinaryHydrationCache(binary_path, sig, features);
        source_used = "canonical_binary";
        return true;
    }
    if (fs::exists(layer_path)) {
        if (!collectHydratedFeaturesFromGeoJson(layer_path, features, error)) return false;
        saveBinaryHydrationCache(binary_path, sig, features);
        source_used = "geojson";
        return true;
    }

    error = "no readable source geojson, canonical binary, or hydration cache";
    return false;
}

int runHydrationCacheSelftest(const fs::path& root) {
    std::vector<LayerDef::FeatureGeom> features;
    LayerDef::FeatureGeom polygon;
    polygon.extent.min_lon = -76.7f;
    polygon.extent.min_lat = 39.2f;
    polygon.extent.max_lon = -76.6f;
    polygon.extent.max_lat = 39.3f;
    polygon.rings.push_back({
        ImVec2(-76.7f, 39.2f),
        ImVec2(-76.6f, 39.2f),
        ImVec2(-76.6f, 39.3f),
        ImVec2(-76.7f, 39.3f)
    });
    polygon.properties.push_back({"source_parcel_id", "test-1"});
    polygon.properties.push_back({"owner", "Example Owner"});
    features.push_back(std::move(polygon));

    LayerDef::FeatureGeom point;
    point.extent.min_lon = point.extent.max_lon = -76.65f;
    point.extent.min_lat = point.extent.max_lat = 39.25f;
    point.properties.push_back({"kind", "point"});
    features.push_back(std::move(point));

    const fs::path test_dir = root / "data" / "cache" / "selftest";
    const fs::path cache_path = test_dir / "hydration_cache_selftest.bin";
    const std::string sig = "selftest_2";
    saveBinaryHydrationCache(cache_path, sig, features);

    std::vector<LayerDef::FeatureGeom> loaded;
    const bool loaded_ok = loadBinaryHydrationCache(cache_path, sig, loaded);
    bool same = loaded_ok && loaded.size() == features.size();
    if (same) {
        same = loaded[0].rings.size() == 1 &&
               loaded[0].rings[0].size() == 4 &&
               loaded[0].properties.size() == 2 &&
               loaded[0].properties[0].first == "source_parcel_id" &&
               loaded[0].properties[0].second == "test-1" &&
               loaded[1].rings.empty() &&
               loaded[1].properties.size() == 1 &&
               nearlyEqual(loaded[0].extent.min_lon, features[0].extent.min_lon) &&
               nearlyEqual(loaded[0].rings[0][2].x, features[0].rings[0][2].x);
    }

    std::vector<LayerDef::FeatureGeom> stale_loaded;
    const bool stale_rejected = !loadBinaryHydrationCache(cache_path, "wrong_signature", stale_loaded);

    const fs::path regional_cache_path = test_dir / "regional_parcels.geojson.bin";
    std::vector<LayerDef::FeatureGeom> regional_features = features;
    for (int i = 0; i < 32; ++i) {
        regional_features[0].properties.push_back({"raw_county_field_to_drop_" + std::to_string(i), "large raw value"});
    }
    saveBinaryHydrationCache(regional_cache_path, sig, regional_features);
    std::vector<LayerDef::FeatureGeom> regional_loaded;
    const bool regional_loaded_ok = loadBinaryHydrationCache(regional_cache_path, sig, regional_loaded);
    bool regional_compacted = regional_loaded_ok && regional_loaded.size() == regional_features.size();
    if (regional_compacted) {
        const bool should_rewrite = binaryHydrationCacheShouldBeCompacted(regional_cache_path, regional_features);
        const bool should_not_rewrite = !binaryHydrationCacheShouldBeCompacted(regional_cache_path, regional_loaded);
        regional_compacted =
            should_rewrite &&
            should_not_rewrite &&
            regional_loaded[0].properties.size() == 2 &&
            regional_loaded[0].properties[0].first == "source_parcel_id" &&
            regional_loaded[0].properties[1].first == "owner";
    }

    std::error_code ec;
    fs::remove(cache_path, ec);
    fs::remove(regional_cache_path, ec);
    fs::remove(test_dir, ec);

    json out = {
        {"mode", "hydration-cache-selftest"},
        {"ok", same && stale_rejected && regional_compacted},
        {"loaded", loaded_ok},
        {"regional_compacted", regional_compacted},
        {"roundtrip_features", loaded.size()},
        {"stale_signature_rejected", stale_rejected}
    };
    std::cout << out.dump(2) << '\n';
    return (same && stale_rejected && regional_compacted) ? 0 : 1;
}

int runTriangulationCacheSelftest(const fs::path& root) {
    std::vector<std::vector<uint32_t>> tris = {
        {0, 1, 2, 0, 2, 3},
        {},
        {5, 6, 7}
    };
    const fs::path test_dir = root / "data" / "cache" / "selftest";
    const fs::path cache_path = test_dir / "triangulation_cache_selftest.bin";
    const std::string sig = "tri_selftest_1";
    saveBinaryTriCache(cache_path, sig, tris);

    std::vector<std::vector<uint32_t>> loaded;
    const bool loaded_ok = loadBinaryTriCache(cache_path, sig, tris.size(), loaded);
    const bool same = loaded_ok && loaded == tris;

    std::vector<std::vector<uint32_t>> stale_loaded;
    const bool stale_rejected = !loadBinaryTriCache(cache_path, "wrong_signature", tris.size(), stale_loaded);
    std::vector<std::vector<uint32_t>> wrong_count_loaded;
    const bool wrong_count_rejected = !loadBinaryTriCache(cache_path, sig, tris.size() + 1, wrong_count_loaded);

    std::error_code ec;
    fs::remove(cache_path, ec);
    fs::remove(test_dir, ec);

    json out = {
        {"mode", "triangulation-cache-selftest"},
        {"ok", same && stale_rejected && wrong_count_rejected},
        {"loaded", loaded_ok},
        {"roundtrip_feature_vectors", loaded.size()},
        {"stale_signature_rejected", stale_rejected},
        {"wrong_count_rejected", wrong_count_rejected}
    };
    std::cout << out.dump(2) << '\n';
    return (same && stale_rejected && wrong_count_rejected) ? 0 : 1;
}

int runProjectionCacheSelftest() {
    LayerDef::FeatureGeom feature;
    feature.extent.min_lon = -76.7f;
    feature.extent.min_lat = 39.2f;
    feature.extent.max_lon = -76.6f;
    feature.extent.max_lat = 39.3f;
    feature.rings.push_back({
        ImVec2(-76.7f, 39.2f),
        ImVec2(-76.6f, 39.2f),
        ImVec2(-76.6f, 39.3f),
        ImVec2(-76.7f, 39.3f)
    });

    MapProjectionCache cache(12, 1, [](const ImVec2& p) { return p; });
    const auto& world_rings_first = cache.getWorldRings(0, 0, feature);
    const auto& world_extent_first = cache.getWorldExtent(0, 0, feature);
    const size_t rings_after_first = cache.cachedWorldRingEntries();
    const size_t extents_after_first = cache.cachedWorldExtentEntries();
    const ImVec2 first_ring_point = world_rings_first[0][0];
    const ImVec2 first_extent_min = world_extent_first.first;

    const auto* rings_ptr_first = &world_rings_first;
    const auto* extent_ptr_first = &world_extent_first;

    cache.updateFrameProjection(12, 1, [](const ImVec2& p) { return ImVec2(p.x + 10.0f, p.y + 20.0f); });
    const auto& world_rings_second = cache.getWorldRings(0, 0, feature);
    const auto& world_extent_second = cache.getWorldExtent(0, 0, feature);
    const bool reused_same_zoom =
        rings_ptr_first == &world_rings_second &&
        extent_ptr_first == &world_extent_second &&
        cache.cachedWorldRingEntries() == rings_after_first &&
        cache.cachedWorldExtentEntries() == extents_after_first;

    cache.updateFrameProjection(13, 1, [](const ImVec2& p) { return p; });
    const bool cleared_on_zoom_change =
        cache.cachedWorldRingEntries() == 0 &&
        cache.cachedWorldExtentEntries() == 0;
    const auto& world_rings_third = cache.getWorldRings(0, 0, feature);
    const auto& world_extent_third = cache.getWorldExtent(0, 0, feature);
    const bool rebuilt_after_zoom_change =
        cache.cachedWorldRingEntries() == 1 &&
        cache.cachedWorldExtentEntries() == 1 &&
        !nearlyEqual(world_rings_third[0][0].x, first_ring_point.x) &&
        !nearlyEqual(world_extent_third.first.x, first_extent_min.x);

    json out = {
        {"mode", "projection-cache-selftest"},
        {"ok", rings_after_first == 1 && extents_after_first == 1 && reused_same_zoom && cleared_on_zoom_change && rebuilt_after_zoom_change},
        {"rings_after_first", rings_after_first},
        {"extents_after_first", extents_after_first},
        {"reused_same_zoom", reused_same_zoom},
        {"cleared_on_zoom_change", cleared_on_zoom_change},
        {"rebuilt_after_zoom_change", rebuilt_after_zoom_change}
    };
    std::cout << out.dump(2) << '\n';
    return out["ok"].get<bool>() ? 0 : 1;
}

int runProjectionFillCacheSelftest() {
    LayerDef::FeatureGeom feature;
    feature.extent.min_lon = -76.7f;
    feature.extent.min_lat = 39.2f;
    feature.extent.max_lon = -76.6f;
    feature.extent.max_lat = 39.3f;
    feature.rings.push_back({
        ImVec2(-76.7f, 39.2f),
        ImVec2(-76.6f, 39.2f),
        ImVec2(-76.6f, 39.3f),
        ImVec2(-76.7f, 39.3f)
    });
    feature.triangles = {0, 1, 2, 0, 2, 3, 9, 10, 11};

    MapProjectionCache cache(12, 1, [](const ImVec2& p) { return ImVec2(p.x + 100.0f, p.y - 50.0f); });
    const auto& fill_first = cache.getWorldFillGeometry(0, 0, feature);
    const auto* fill_ptr_first = &fill_first;
    const bool first_ok =
        cache.cachedWorldFillEntries() == 1 &&
        fill_first.vertices.size() == 4 &&
        fill_first.triangle_indices == std::vector<uint32_t>({0, 1, 2, 0, 2, 3});

    const auto& fill_second = cache.getWorldFillGeometry(0, 0, feature);
    const bool reused_same_zoom =
        fill_ptr_first == &fill_second &&
        cache.cachedWorldFillEntries() == 1;

    cache.updateFrameProjection(12, 1, [](const ImVec2& p) { return p; });
    const auto& fill_same_zoom_projection_change = cache.getWorldFillGeometry(0, 0, feature);
    const bool retained_across_projection_change =
        &fill_same_zoom_projection_change == fill_ptr_first &&
        cache.cachedWorldFillEntries() == 1;

    cache.updateFrameProjection(13, 1, [](const ImVec2& p) { return p; });
    const bool cleared_on_zoom_change = cache.cachedWorldFillEntries() == 0;
    const auto& fill_third = cache.getWorldFillGeometry(0, 0, feature);
    const bool rebuilt_after_zoom_change =
        cache.cachedWorldFillEntries() == 1 &&
        fill_third.vertices.size() == 4 &&
        fill_third.triangle_indices == std::vector<uint32_t>({0, 1, 2, 0, 2, 3});

    json out = {
        {"mode", "projection-fill-cache-selftest"},
        {"ok", first_ok && reused_same_zoom && retained_across_projection_change && cleared_on_zoom_change && rebuilt_after_zoom_change},
        {"first_ok", first_ok},
        {"reused_same_zoom", reused_same_zoom},
        {"retained_across_projection_change", retained_across_projection_change},
        {"cleared_on_zoom_change", cleared_on_zoom_change},
        {"rebuilt_after_zoom_change", rebuilt_after_zoom_change}
    };
    std::cout << out.dump(2) << '\n';
    return out["ok"].get<bool>() ? 0 : 1;
}

int runProjectionColorCacheSelftest() {
    MapProjectionCache cache(12, 1, [](const ImVec2& p) { return p; });
    const uint64_t style_key_a = 0xabc123ull;
    const uint64_t style_key_b = 0xdef456ull;
    const ImU32 color_a = IM_COL32(10, 20, 30, 255);
    const ImU32 color_b = IM_COL32(200, 150, 100, 255);

    const bool missing_before =
        cache.findFeatureColorStorage(0, 0, style_key_a) == nullptr;
    cache.storeFeatureColorStorage(0, 0, style_key_a, color_a, 1);
    const CachedFeatureColorStorage* first = cache.findFeatureColorStorage(0, 0, style_key_a);
    const bool first_ok =
        first &&
        first->feature_color == color_a &&
        first->polygon_colors.size() == 1 &&
        first->polygon_colors[0] == color_a &&
        cache.cachedFeatureColorEntries() == 1;

    const bool style_miss =
        cache.findFeatureColorStorage(0, 0, style_key_b) == nullptr;
    cache.storeFeatureColorStorage(0, 0, style_key_b, color_b, 3);
    const CachedFeatureColorStorage* second = cache.findFeatureColorStorage(0, 0, style_key_b);
    const bool overwrite_ok =
        second &&
        second->feature_color == color_b &&
        second->polygon_colors.size() == 3 &&
        second->polygon_colors[0] == color_b &&
        second->polygon_colors[2] == color_b &&
        cache.cachedFeatureColorEntries() == 1;

    cache.updateFrameProjection(13, 1, [](const ImVec2& p) { return p; });
    const CachedFeatureColorStorage* after_zoom = cache.findFeatureColorStorage(0, 0, style_key_b);
    const bool retained_after_zoom =
        after_zoom &&
        after_zoom->feature_color == color_b &&
        after_zoom->polygon_colors.size() == 3;

    json out = {
        {"mode", "projection-color-cache-selftest"},
        {"ok", missing_before && first_ok && style_miss && overwrite_ok && retained_after_zoom},
        {"missing_before", missing_before},
        {"first_ok", first_ok},
        {"style_miss", style_miss},
        {"overwrite_ok", overwrite_ok},
        {"retained_after_zoom", retained_after_zoom}
    };
    std::cout << out.dump(2) << '\n';
    return out["ok"].get<bool>() ? 0 : 1;
}

int runPolygonHoleSelftest() {
    LayerDef::FeatureGeom feature;
    feature.extent.min_lon = 0.0f;
    feature.extent.min_lat = 0.0f;
    feature.extent.max_lon = 10.0f;
    feature.extent.max_lat = 10.0f;
    feature.rings.push_back({
        ImVec2(0.0f, 0.0f),
        ImVec2(10.0f, 0.0f),
        ImVec2(10.0f, 10.0f),
        ImVec2(0.0f, 10.0f)
    });
    feature.rings.push_back({
        ImVec2(3.0f, 3.0f),
        ImVec2(3.0f, 7.0f),
        ImVec2(7.0f, 7.0f),
        ImVec2(7.0f, 3.0f)
    });
    feature.triangles = triangulateRings(feature.rings);

    const bool shell_point_inside = pointInFeature(feature, 1.0f, 1.0f);
    const bool hole_point_rejected = !pointInFeature(feature, 5.0f, 5.0f);
    const bool outside_point_rejected = !pointInFeature(feature, 12.0f, 5.0f);

    std::vector<ImVec2> flattened;
    for (const auto& ring : feature.rings) {
        flattened.insert(flattened.end(), ring.begin(), ring.end());
    }

    bool centroids_valid = !feature.triangles.empty();
    size_t centroid_count = 0;
    for (size_t ti = 0; ti + 2 < feature.triangles.size(); ti += 3) {
        const uint32_t ia = feature.triangles[ti + 0];
        const uint32_t ib = feature.triangles[ti + 1];
        const uint32_t ic = feature.triangles[ti + 2];
        if (ia >= flattened.size() || ib >= flattened.size() || ic >= flattened.size()) {
            centroids_valid = false;
            break;
        }
        const ImVec2& a = flattened[ia];
        const ImVec2& b = flattened[ib];
        const ImVec2& c = flattened[ic];
        const float cx = (a.x + b.x + c.x) / 3.0f;
        const float cy = (a.y + b.y + c.y) / 3.0f;
        if (pointInRing(feature.rings[1], cx, cy) || !pointInFeature(feature, cx, cy)) {
            centroids_valid = false;
            break;
        }
        ++centroid_count;
    }

    ParcelRenderCacheBlob blob;
    const bool render_blob_ok = buildParcelRenderCacheBlob({feature}, "polygon_hole_selftest_sig", blob, 64);
    const bool render_blob_shape_ok =
        render_blob_ok &&
        blob.vertices.size() == flattened.size() &&
        blob.indices.size() == feature.triangles.size() &&
        blob.line_indices.size() == 16 &&
        blob.features.size() == 1 &&
        blob.chunks.size() == 1;

    json out = {
        {"mode", "polygon-hole-selftest"},
        {"ok", shell_point_inside && hole_point_rejected && outside_point_rejected && centroids_valid && render_blob_shape_ok},
        {"shell_point_inside", shell_point_inside},
        {"hole_point_rejected", hole_point_rejected},
        {"outside_point_rejected", outside_point_rejected},
        {"triangle_index_count", feature.triangles.size()},
        {"triangle_count", centroid_count},
        {"centroids_valid", centroids_valid},
        {"render_blob_ok", render_blob_shape_ok}
    };
    std::cout << out.dump(2) << '\n';
    return out["ok"].get<bool>() ? 0 : 1;
}

int runParcelRenderCacheSelftest(const fs::path& root) {
    std::vector<LayerDef::FeatureGeom> features;

    LayerDef::FeatureGeom a;
    a.extent.min_lon = -76.7f;
    a.extent.min_lat = 39.2f;
    a.extent.max_lon = -76.6f;
    a.extent.max_lat = 39.3f;
    a.rings.push_back({
        ImVec2(-76.7f, 39.2f),
        ImVec2(-76.6f, 39.2f),
        ImVec2(-76.6f, 39.3f),
        ImVec2(-76.7f, 39.3f)
    });
    a.triangles = {0, 1, 2, 0, 2, 3};
    features.push_back(a);

    LayerDef::FeatureGeom b;
    b.extent.min_lon = -76.5f;
    b.extent.min_lat = 39.1f;
    b.extent.max_lon = -76.4f;
    b.extent.max_lat = 39.2f;
    b.rings.push_back({
        ImVec2(-76.5f, 39.1f),
        ImVec2(-76.4f, 39.1f),
        ImVec2(-76.4f, 39.2f),
        ImVec2(-76.5f, 39.2f)
    });
    b.triangles = {0, 1, 2, 0, 2, 3};
    features.push_back(b);

    LayerDef::FeatureGeom skipped;
    skipped.extent.min_lon = -76.3f;
    skipped.extent.min_lat = 39.0f;
    skipped.extent.max_lon = -76.2f;
    skipped.extent.max_lat = 39.1f;
    skipped.rings.push_back({
        ImVec2(-76.3f, 39.0f),
        ImVec2(-76.2f, 39.0f),
        ImVec2(-76.2f, 39.1f),
        ImVec2(-76.3f, 39.1f)
    });
    features.push_back(skipped);

    const std::string sig = "parcel_render_selftest_sig";
    ParcelRenderCacheBlob blob;
    const bool built = buildParcelRenderCacheBlob(features, sig, blob, 1);
    const fs::path test_dir = root / "data" / "cache" / "selftest";
    const fs::path cache_path = test_dir / "parcel_render_cache_selftest.bin";
    if (built) saveBinaryParcelRenderCache(cache_path, blob);

    ParcelRenderCacheBlob loaded;
    const bool loaded_ok = built && loadBinaryParcelRenderCache(cache_path, sig, loaded);
    ParcelRenderCacheBlob stale;
    const bool stale_rejected = !loadBinaryParcelRenderCache(cache_path, "wrong_sig", stale);
    const bool same =
        loaded_ok &&
        loaded.source_signature == sig &&
        loaded.vertices.size() == 8 &&
        loaded.vertex_feature_refs.size() == 8 &&
        loaded.indices.size() == 12 &&
        loaded.line_indices.size() == 16 &&
        loaded.features.size() == 2 &&
        loaded.chunks.size() == 2 &&
        loaded.features[0].feature_idx == 0 &&
        loaded.features[1].feature_idx == 1 &&
        loaded.chunks[0].feature_count == 1 &&
        loaded.chunks[1].feature_count == 1 &&
        loaded.features[0].line_index_count == 8 &&
        loaded.features[1].line_index_count == 8 &&
        loaded.vertex_feature_refs[0] == 0 &&
        loaded.vertex_feature_refs[4] == 1 &&
        loaded.indices[0] == 0 &&
        loaded.indices[6] == 4 &&
        loaded.line_indices[0] == 0 &&
        loaded.line_indices[8] == 4;

    std::error_code ec;
    fs::remove(cache_path, ec);
    fs::remove(test_dir, ec);

    json out = {
        {"mode", "parcel-render-cache-selftest"},
        {"ok", built && same && stale_rejected},
        {"built", built},
        {"loaded", loaded_ok},
        {"vertex_count", loaded.vertices.size()},
        {"vertex_feature_ref_count", loaded.vertex_feature_refs.size()},
        {"index_count", loaded.indices.size()},
        {"line_index_count", loaded.line_indices.size()},
        {"feature_records", loaded.features.size()},
        {"chunk_records", loaded.chunks.size()},
        {"stale_signature_rejected", stale_rejected}
    };
    std::cout << out.dump(2) << '\n';
    return out["ok"].get<bool>() ? 0 : 1;
}

int runTriangulationApplySelftest() {
    std::vector<LayerDef> layers(1);
    layers[0].file = "tri_apply_selftest.geojson";
    layers[0].features.resize(10000);
    std::vector<LayerRuntimeState> layer_states(1);
    std::vector<bool> layer_profile_dirty(1, false);
    std::deque<TriResult> tri_results;
    std::mutex tri_mutex;
    std::mutex status_mutex;
    std::atomic<size_t> triangulated_count{0};

    TriResult tr;
    tr.index = 0;
    tr.source_signature = "tri_apply_sig";
    tr.loaded_from_cache = true;
    tr.loaded_from_binary_cache = true;
    tr.triangles_per_feature.resize(layers[0].features.size(), {0, 1, 2, 0, 2, 3});
    tri_results.push_back(std::move(tr));

    LayerPipelineDrainContext ctx;
    ctx.layers = &layers;
    ctx.tri_results = &tri_results;
    ctx.tri_mutex = &tri_mutex;
    ctx.layer_states = &layer_states;
    ctx.status_mutex = &status_mutex;
    ctx.layer_profile_dirty = &layer_profile_dirty;
    ctx.triangulated_count = &triangulated_count;

    drainTriangulationResults(ctx);
    const bool first_pass_partial =
        !tri_results.empty() &&
        tri_results.front().apply_offset > 0 &&
        tri_results.front().apply_offset < tri_results.front().triangles_per_feature.size() &&
        layer_states[0].status == LayerPipelineStatus::Triangulating &&
        layer_states[0].triangulation_phase == "applying_binary_cache" &&
        triangulated_count.load(std::memory_order_relaxed) == 0;

    size_t passes = 1;
    while (!tri_results.empty() && passes < 16) {
        drainTriangulationResults(ctx);
        ++passes;
    }

    const std::vector<uint32_t> expected{0, 1, 2, 0, 2, 3};
    const bool complete =
        tri_results.empty() &&
        layer_states[0].status == LayerPipelineStatus::Ready &&
        layer_states[0].triangulation_phase == "binary_cache_hit" &&
        layer_states[0].triangulation_loaded_from_cache &&
        triangulated_count.load(std::memory_order_relaxed) == 1 &&
        layer_profile_dirty[0] &&
        layers[0].features[0].triangles == expected &&
        layers[0].features.back().triangles == expected;

    json out = {
        {"mode", "triangulation-apply-selftest"},
        {"ok", first_pass_partial && complete},
        {"first_pass_partial", first_pass_partial},
        {"passes", passes},
        {"complete", complete}
    };
    std::cout << out.dump(2) << '\n';
    return out["ok"].get<bool>() ? 0 : 1;
}

int runSpatialIndexSelftest() {
    std::vector<LayerDef> layers(1);
    layers[0].features.resize(3);
    layers[0].features[0].extent = {-76.70f, 39.20f, -76.69f, 39.21f};
    layers[0].features[1].extent = {-76.68f, 39.22f, -76.67f, 39.23f};
    layers[0].features[2].extent = {-76.40f, 39.50f, -76.39f, 39.51f};

    std::vector<LayerRuntimeState> layer_states(1);
    layer_states[0].hydration_source_signature = "sig_1";
    layer_states[0].spatial_index_phase = "queued";
    std::vector<LayerSpatialIndex> layer_spatial(1);
    std::vector<bool> layer_profile_dirty(1, false);
    std::deque<SpatialIndexResult> spatial_results;
    std::mutex spatial_mutex;
    std::mutex status_mutex;

    std::vector<LayerDef::FeatureExtent> extents;
    for (const auto& fg : layers[0].features) extents.push_back(fg.extent);

    SpatialIndexResult ok_result;
    ok_result.index = 0;
    ok_result.source_signature = "sig_1";
    ok_result.feature_count = extents.size();
    buildLayerSpatialIndexForExtents(extents, ok_result.spatial_index);
    spatial_results.push_back(std::move(ok_result));

    LayerPipelineDrainContext ctx;
    ctx.layers = &layers;
    ctx.spatial_results = &spatial_results;
    ctx.spatial_mutex = &spatial_mutex;
    ctx.layer_states = &layer_states;
    ctx.layer_spatial = &layer_spatial;
    ctx.status_mutex = &status_mutex;
    ctx.layer_profile_dirty = &layer_profile_dirty;

    drainSpatialIndexResults(ctx);
    std::vector<uint32_t> hits;
    const bool query_ok = queryLayerSpatialIndex(layer_spatial[0], -76.705f, 39.195f, -76.665f, 39.235f, hits);
    const bool applied =
        layer_spatial[0].built &&
        layer_spatial[0].feature_count_built == extents.size() &&
        layer_states[0].spatial_index_phase == "ready" &&
        layer_states[0].spatial_index_source_signature == "sig_1" &&
        layer_profile_dirty[0] &&
        query_ok &&
        hits.size() == 2;

    layer_profile_dirty[0] = false;
    layer_states[0].hydration_source_signature = "sig_2";
    layer_states[0].spatial_index_phase = "queued";
    SpatialIndexResult stale_result;
    stale_result.index = 0;
    stale_result.source_signature = "sig_1";
    stale_result.feature_count = extents.size();
    buildLayerSpatialIndexForExtents(extents, stale_result.spatial_index);
    spatial_results.push_back(std::move(stale_result));
    drainSpatialIndexResults(ctx);
    const bool stale_discarded =
        layer_states[0].spatial_index_phase == "stale_discarded" &&
        !layer_profile_dirty[0] &&
        layer_spatial[0].feature_count_built == extents.size();

    json out = {
        {"mode", "spatial-index-selftest"},
        {"ok", applied && stale_discarded},
        {"applied", applied},
        {"stale_discarded", stale_discarded},
        {"query_hits", hits.size()}
    };
    std::cout << out.dump(2) << '\n';
    return out["ok"].get<bool>() ? 0 : 1;
}

int runLayerProfileSelftest() {
    std::vector<LayerDef> layers(1);
    layers[0].name = "Profile Layer";
    layers[0].file = "profile_layer.geojson";
    layers[0].enabled = true;

    std::vector<LayerProfileAccumulator> accumulators(1);
    accumulators[0].features = 7;
    accumulators[0].rings = 5;
    accumulators[0].ring_points = 21;
    accumulators[0].triangle_indices = 18;
    accumulators[0].properties = 11;
    accumulators[0].spatial_index_built = true;
    accumulators[0].spatial_index_cells = 64;
    accumulators[0].spatial_index_marks = 7;

    std::vector<bool> layer_profile_dirty(1, true);
    std::vector<LayerProfileSnapshot> layer_profile_snapshot(1);
    std::mutex layer_profile_mutex;

    LayerProfileSnapshotRefreshContext ctx;
    ctx.layers = &layers;
    ctx.layer_profile_accumulators = &accumulators;
    ctx.layer_profile_dirty = &layer_profile_dirty;
    ctx.layer_profile_snapshot = &layer_profile_snapshot;
    ctx.layer_profile_mutex = &layer_profile_mutex;
    refreshLayerProfileSnapshot(ctx);

    const auto& snap = layer_profile_snapshot[0];
    const bool ok =
        !layer_profile_dirty[0] &&
        snap.name == "Profile Layer" &&
        snap.file == "profile_layer.geojson" &&
        snap.enabled &&
        snap.features == 7 &&
        snap.rings == 5 &&
        snap.ring_points == 21 &&
        snap.triangle_indices == 18 &&
        snap.properties == 11 &&
        snap.spatial_index_built &&
        snap.spatial_index_cells == 64 &&
        snap.spatial_index_marks == 7;

    json out = {
        {"mode", "layer-profile-selftest"},
        {"ok", ok},
        {"features", snap.features},
        {"triangle_indices", snap.triangle_indices},
        {"spatial_index_cells", snap.spatial_index_cells}
    };
    std::cout << out.dump(2) << '\n';
    return ok ? 0 : 1;
}

int runLayerRuntimeStatusSelftest() {
    LayerRuntimeState hydration_cache;
    hydration_cache.status = LayerPipelineStatus::Hydrating;
    hydration_cache.hydration_phase = "loading_binary_cache";

    LayerRuntimeState canonical_binary;
    canonical_binary.status = LayerPipelineStatus::Hydrating;
    canonical_binary.hydration_phase = "loading_canonical_binary_source";

    LayerRuntimeState source_geojson;
    source_geojson.status = LayerPipelineStatus::Hydrating;
    source_geojson.hydration_phase = "parsing_source_cache_missing";

    LayerRuntimeState tri_cache;
    tri_cache.status = LayerPipelineStatus::Triangulating;
    tri_cache.triangulation_phase = "loading_binary_cache";

    LayerRuntimeState ready;
    ready.status = LayerPipelineStatus::Ready;

    const std::string file = "regional_parcels.geojson";
    const bool ok =
        layerRuntimeDisplayStatus(hydration_cache, file) == "reading regional_parcels.geojson.bin" &&
        layerRuntimeDisplayStatus(canonical_binary, file) == "reading regional_parcels.geojson.canonical.bin" &&
        layerRuntimeDisplayStatus(source_geojson, file) == "reading regional_parcels.geojson" &&
        layerRuntimeDisplayStatus(tri_cache, file) == "reading regional_parcels.geojson.tri.bin" &&
        layerRuntimeDisplayStatus(ready, file) == "ready";

    json out = {
        {"mode", "layer-runtime-status-selftest"},
        {"ok", ok},
        {"hydration_cache", layerRuntimeDisplayStatus(hydration_cache, file)},
        {"canonical_binary", layerRuntimeDisplayStatus(canonical_binary, file)},
        {"source_geojson", layerRuntimeDisplayStatus(source_geojson, file)},
        {"triangulation_cache", layerRuntimeDisplayStatus(tri_cache, file)},
        {"ready", layerRuntimeDisplayStatus(ready, file)}
    };
    std::cout << out.dump(2) << '\n';
    return ok ? 0 : 1;
}

int runParcelGpuCpuBypassSelftest() {
    const bool plain_gpu_bypasses =
        shouldBypassCpuParcelFeaturePass(true, false, false, false);
    const bool inactive_gpu_does_not_bypass =
        !shouldBypassCpuParcelFeaturePass(false, false, false, false);
    const bool heatmap_recompute_keeps_cpu =
        !shouldBypassCpuParcelFeaturePass(true, true, false, true);
    const bool cached_heatmap_bypasses =
        shouldBypassCpuParcelFeaturePass(true, true, false, false);
    const bool lod_keeps_cpu =
        !shouldBypassCpuParcelFeaturePass(true, false, true, false);
    const bool lod_heatmap_keeps_cpu =
        !shouldBypassCpuParcelFeaturePass(true, true, true, false);

    const bool ok =
        plain_gpu_bypasses &&
        inactive_gpu_does_not_bypass &&
        heatmap_recompute_keeps_cpu &&
        cached_heatmap_bypasses &&
        lod_keeps_cpu &&
        lod_heatmap_keeps_cpu;

    json out = {
        {"mode", "parcel-gpu-cpu-bypass-selftest"},
        {"ok", ok},
        {"plain_gpu_bypasses", plain_gpu_bypasses},
        {"inactive_gpu_does_not_bypass", inactive_gpu_does_not_bypass},
        {"heatmap_recompute_keeps_cpu", heatmap_recompute_keeps_cpu},
        {"cached_heatmap_bypasses", cached_heatmap_bypasses},
        {"lod_keeps_cpu", lod_keeps_cpu},
        {"lod_heatmap_keeps_cpu", lod_heatmap_keeps_cpu}
    };
    std::cout << out.dump(2) << '\n';
    return ok ? 0 : 1;
}

int runRenderPolicySelftest() {
    std::vector<LayerDef> layers(1);
    layers[0].enabled = true;
    layers[0].scale = "parcel";
    layers[0].heatmap_field = "assessed_value";

    std::vector<bool> heatmap_enabled{true};
    std::vector<int> heatmap_algo{kAggregateMedianChoropleth};
    std::vector<int> heatmap_max_zoom{9};
    std::vector<int> parcel_detail_min_zoom{13};

    HeatmapLayerPolicyContext ctx;
    ctx.layers = &layers;
    ctx.layer_heatmap_enabled = &heatmap_enabled;
    ctx.layer_heatmap_algo = &heatmap_algo;
    ctx.layer_heatmap_max_zoom = &heatmap_max_zoom;
    ctx.layer_parcel_detail_min_zoom = &parcel_detail_min_zoom;
    ctx.heatmap_algo = kAggregateGridBinning;

    auto display_at_zoom = [&](int zoom) {
        ctx.zoom = zoom;
        return resolveLayerDisplayPolicy(ctx, 0);
    };

    const LayerDisplayPolicy display_9 = display_at_zoom(9);
    const LayerDisplayPolicy display_10 = display_at_zoom(10);
    const LayerDisplayPolicy display_12 = display_at_zoom(12);
    const LayerDisplayPolicy display_13 = display_at_zoom(13);
    const bool aggregate_at_9 = display_9.mode == LayerDisplayMode::Aggregate;
    const bool detail_off_at_9 = display_9.mode != LayerDisplayMode::ParcelChoroplethDetail;
    const int effective_detail_min_zoom = display_10.effective_parcel_detail_min_zoom;
    const bool detail_at_10 = display_10.mode == LayerDisplayMode::ParcelChoroplethDetail;
    const bool aggregate_off_at_10 = display_10.mode != LayerDisplayMode::Aggregate;
    const bool detail_at_12 = display_12.mode == LayerDisplayMode::ParcelChoroplethDetail;
    const bool detail_at_13 = display_13.mode == LayerDisplayMode::ParcelChoroplethDetail;

    heatmap_enabled[0] = false;
    const LayerDisplayPolicy aggregate_none_display_10 = display_at_zoom(10);
    const LayerDisplayPolicy aggregate_none_display_13 = display_at_zoom(13);
    const bool aggregate_none_ignores_max_zoom =
        aggregate_none_display_10.effective_parcel_detail_min_zoom == 13 &&
        aggregate_none_display_10.mode != LayerDisplayMode::ParcelChoroplethDetail &&
        aggregate_none_display_13.mode == LayerDisplayMode::ParcelChoroplethDetail;

    const bool ok =
        aggregate_at_9 &&
        detail_off_at_9 &&
        effective_detail_min_zoom == 10 &&
        detail_at_10 &&
        aggregate_off_at_10 &&
        detail_at_12 &&
        detail_at_13 &&
        aggregate_none_ignores_max_zoom;

    json out = {
        {"mode", "render-policy-selftest"},
        {"ok", ok},
        {"aggregate_at_9", aggregate_at_9},
        {"detail_off_at_9", detail_off_at_9},
        {"effective_detail_min_zoom", effective_detail_min_zoom},
        {"detail_at_10", detail_at_10},
        {"aggregate_off_at_10", aggregate_off_at_10},
        {"detail_at_12", detail_at_12},
        {"detail_at_13", detail_at_13},
        {"aggregate_none_ignores_max_zoom", aggregate_none_ignores_max_zoom}
    };
    std::cout << out.dump(2) << '\n';
    return ok ? 0 : 1;
}

int runRenderPlanSelftest() {
    std::vector<LayerDef> layers(4);
    layers[0].file = "basemap_context.geojson";
    layers[1].file = "regional_parcels.geojson";
    layers[1].scale = "parcel";
    layers[2].file = "zoning.geojson";
    layers[2].category = LayerDef::Category::Zoning;
    layers[2].scale = "parcel";
    layers[3].file = "parcel_query_overlay.geojson";
    layers[3].scale = "parcel";

    RenderPlanBuilderContext ctx;
    ctx.layers = &layers;
    ctx.zoning_layer_idx = 2;
    ctx.is_parcel_related_layer = [](size_t layer_idx) {
        return layer_idx == 1 || layer_idx == 3;
    };
    ctx.layer_uses_heatmap_aggregate = [](size_t) { return false; };
    ctx.layer_uses_lod_geometry = [](size_t) { return false; };

    const RenderPlan plan = buildRenderPlan(ctx);
    auto order_pos = [&](size_t layer_idx) {
        auto it = std::find(plan.draw_layer_order.begin(), plan.draw_layer_order.end(), layer_idx);
        return it == plan.draw_layer_order.end()
            ? plan.draw_layer_order.size()
            : (size_t)std::distance(plan.draw_layer_order.begin(), it);
    };
    const size_t zoning_pos = order_pos(2);
    const size_t parcel_pos = order_pos(1);
    const size_t query_pos = order_pos(3);
    const bool zoning_before_parcel_infill = zoning_pos < parcel_pos && zoning_pos < query_pos;
    const bool all_layers_present = plan.draw_layer_order.size() == layers.size();
    const bool ok = all_layers_present && zoning_before_parcel_infill;

    json out = {
        {"mode", "render-plan-selftest"},
        {"ok", ok},
        {"draw_layer_order", plan.draw_layer_order},
        {"zoning_before_parcel_infill", zoning_before_parcel_infill},
        {"all_layers_present", all_layers_present}
    };
    std::cout << out.dump(2) << '\n';
    return ok ? 0 : 1;
}

std::optional<size_t> readBinaryTriCacheCount(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    char magic[8];
    if (!in.read(magic, sizeof(magic))) return std::nullopt;
    const std::string expected("WS3TRI2", 7);
    if (std::string(magic, magic + 7) != expected) return std::nullopt;
    uint32_t version = 0;
    uint32_t endian = 0;
    auto read_u32 = [&](uint32_t& out) {
        unsigned char b[4];
        if (!in.read(reinterpret_cast<char*>(b), sizeof(b))) return false;
        out = uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
        return true;
    };
    auto read_u64 = [&](uint64_t& out) {
        unsigned char b[8];
        if (!in.read(reinterpret_cast<char*>(b), sizeof(b))) return false;
        out = uint64_t(b[0]) |
              (uint64_t(b[1]) << 8) |
              (uint64_t(b[2]) << 16) |
              (uint64_t(b[3]) << 24) |
              (uint64_t(b[4]) << 32) |
              (uint64_t(b[5]) << 40) |
              (uint64_t(b[6]) << 48) |
              (uint64_t(b[7]) << 56);
        return true;
    };
    if (!read_u32(version) || !read_u32(endian) || version != 1 || endian != 0x01020304u) return std::nullopt;
    uint32_t sig_len = 0;
    if (!read_u32(sig_len)) return std::nullopt;
    in.seekg(sig_len, std::ios::cur);
    if (!in) return std::nullopt;
    uint64_t count = 0;
    if (!read_u64(count) || count > std::numeric_limits<size_t>::max()) return std::nullopt;
    return static_cast<size_t>(count);
}

struct BinaryCacheHeader {
    bool ok = false;
    uint32_t version = 0;
    std::string source_signature;
    uint64_t count = 0;
    uint32_t vertices = 0;
    uint32_t indices = 0;
    uint32_t line_indices = 0;
    uint32_t features = 0;
    uint32_t chunks = 0;
    uintmax_t file_size_bytes = 0;
    std::string error;
};

bool readCliU32(std::istream& in, uint32_t& out) {
    unsigned char b[4];
    if (!in.read(reinterpret_cast<char*>(b), sizeof(b))) return false;
    out = uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
    return true;
}

bool readCliU64(std::istream& in, uint64_t& out) {
    unsigned char b[8];
    if (!in.read(reinterpret_cast<char*>(b), sizeof(b))) return false;
    out = uint64_t(b[0]) |
          (uint64_t(b[1]) << 8) |
          (uint64_t(b[2]) << 16) |
          (uint64_t(b[3]) << 24) |
          (uint64_t(b[4]) << 32) |
          (uint64_t(b[5]) << 40) |
          (uint64_t(b[6]) << 48) |
          (uint64_t(b[7]) << 56);
    return true;
}

bool readCliString(std::istream& in, std::string& out) {
    uint32_t n = 0;
    if (!readCliU32(in, n) || n > 64u * 1024u * 1024u) return false;
    out.resize(n);
    return n == 0 || bool(in.read(out.data(), static_cast<std::streamsize>(n)));
}

uintmax_t fileSizeOrZero(const fs::path& path) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(path, ec);
    return ec ? 0 : size;
}

BinaryCacheHeader readHydrationCacheHeader(const fs::path& path) {
    BinaryCacheHeader out;
    out.file_size_bytes = fileSizeOrZero(path);
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        out.error = "missing";
        return out;
    }
    char magic[8];
    if (!in.read(magic, sizeof(magic)) || std::string(magic, magic + 7) != "WS3HYD2") {
        out.error = "bad_magic";
        return out;
    }
    uint32_t endian = 0;
    if (!readCliU32(in, out.version) || !readCliU32(in, endian) || out.version != 1 || endian != 0x01020304u) {
        out.error = "bad_header";
        return out;
    }
    if (!readCliString(in, out.source_signature) || !readCliU64(in, out.count)) {
        out.error = "bad_metadata";
        return out;
    }
    out.ok = true;
    return out;
}

BinaryCacheHeader readTriCacheHeader(const fs::path& path) {
    BinaryCacheHeader out;
    out.file_size_bytes = fileSizeOrZero(path);
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        out.error = "missing";
        return out;
    }
    char magic[8];
    if (!in.read(magic, sizeof(magic)) || std::string(magic, magic + 7) != "WS3TRI2") {
        out.error = "bad_magic";
        return out;
    }
    uint32_t endian = 0;
    if (!readCliU32(in, out.version) || !readCliU32(in, endian) || out.version != 1 || endian != 0x01020304u) {
        out.error = "bad_header";
        return out;
    }
    if (!readCliString(in, out.source_signature) || !readCliU64(in, out.count)) {
        out.error = "bad_metadata";
        return out;
    }
    out.ok = true;
    return out;
}

BinaryCacheHeader readParcelRenderCacheHeader(const fs::path& path) {
    BinaryCacheHeader out;
    out.file_size_bytes = fileSizeOrZero(path);
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        out.error = "missing";
        return out;
    }
    char magic[8];
    if (!in.read(magic, sizeof(magic)) || std::string(magic, magic + 7) != "WS3PRD1") {
        out.error = "bad_magic";
        return out;
    }
    uint32_t endian = 0;
    if (!readCliU32(in, out.version) || !readCliU32(in, endian) || out.version != 2 || endian != 0x01020304u) {
        out.error = "bad_header";
        return out;
    }
    if (!readCliString(in, out.source_signature) ||
        !readCliU32(in, out.vertices) ||
        !readCliU32(in, out.indices) ||
        !readCliU32(in, out.line_indices) ||
        !readCliU32(in, out.features) ||
        !readCliU32(in, out.chunks)) {
        out.error = "bad_metadata";
        return out;
    }
    out.count = out.features;
    out.ok = true;
    return out;
}

json binaryHeaderJson(const BinaryCacheHeader& h, const std::string& expected_sig, uint64_t expected_count) {
    const bool signature_match = h.ok && h.source_signature == expected_sig;
    const bool count_match = h.ok && (expected_count == 0 || h.count == expected_count);
    json out = {
        {"ok", h.ok},
        {"file_size_bytes", h.file_size_bytes},
        {"source_signature", h.source_signature},
        {"signature_match", signature_match},
        {"count", h.count},
        {"count_match", count_match}
    };
    if (h.version != 0) out["version"] = h.version;
    if (!h.error.empty()) out["error"] = h.error;
    if (h.vertices != 0 || h.indices != 0 || h.features != 0 || h.chunks != 0) {
        out["vertices"] = h.vertices;
        out["indices"] = h.indices;
        out["line_indices"] = h.line_indices;
        out["features"] = h.features;
        out["chunks"] = h.chunks;
    }
    return out;
}

int parcelArtifactHealth(const fs::path& root, std::string file) {
    if (file.empty()) file = "regional_parcels.geojson";
    if (!isBareLayerFilename(file)) {
        std::cout << json{
            {"mode", "parcel-artifact-health"},
            {"file", file},
            {"ok", false},
            {"error", "requires a layer filename, not a path"}
        }.dump(2) << '\n';
        return 2;
    }

    const fs::path layer_path = resolveStoredLayerPathForFile(root, file);
    const fs::path canonical_path = layer_path.parent_path() / canonicalBinaryPathForLayerFile(file);
    const fs::path hydration_path = root / "data" / "cache" / "hydration" / (file + ".bin");
    const fs::path tri_path = root / "data" / "cache" / "triangulation" / (file + ".tri.bin");
    const fs::path render_path = root / "data" / "cache" / "render" / (file + ".parcel-render.bin");
    const fs::path duckdb_path = root / "data" / "worldsim.duckdb";

    std::string resolved_sig;
    std::string resolved_kind;
    const bool resolved = resolveLayerSourceSignature(layer_path, resolved_sig, &resolved_kind);

    CanonicalFeatureCollectionMetadata canonical_meta;
    const bool canonical_ok = loadBinaryCanonicalMetadata(canonical_path, canonical_meta);
    const uint64_t expected_count = canonical_ok ? canonical_meta.feature_count : 0;
    const std::string expected_sig = resolved ? resolved_sig : canonical_meta.source_signature;

    const BinaryCacheHeader hydration = readHydrationCacheHeader(hydration_path);
    const BinaryCacheHeader tri = readTriCacheHeader(tri_path);
    const BinaryCacheHeader render = readParcelRenderCacheHeader(render_path);

    std::error_code geojson_ec;
    const bool geojson_present = fs::exists(layer_path, geojson_ec) && !geojson_ec;
    const uintmax_t geojson_size = geojson_present ? fileSizeOrZero(layer_path) : 0;
    const bool duckdb_present = fs::exists(duckdb_path, geojson_ec) && !geojson_ec;
    const uintmax_t duckdb_size = duckdb_present ? fileSizeOrZero(duckdb_path) : 0;

    json recommendations = json::array();
    if (!canonical_ok) recommendations.push_back("build canonical parcel binary");
    if (!hydration.ok || hydration.source_signature != expected_sig || (expected_count != 0 && hydration.count != expected_count)) {
        recommendations.push_back("run --warm-hydration-cache " + file);
    }
    if (!tri.ok || tri.source_signature != expected_sig || (expected_count != 0 && tri.count != expected_count)) {
        recommendations.push_back("run --warm-triangulation-cache " + file);
    }
    if (!render.ok || render.source_signature != expected_sig) {
        recommendations.push_back("run --warm-parcel-render-cache " + file);
    }
    if (!duckdb_present) recommendations.push_back("rebuild DuckDB analytics cache");
    if (geojson_present && resolved_kind == "canonical_binary") {
        recommendations.push_back("GeoJSON is optional/debug for this layer; normal runtime uses canonical binary signature");
    }

    const bool ok =
        resolved &&
        canonical_ok &&
        hydration.ok && hydration.source_signature == expected_sig && (expected_count == 0 || hydration.count == expected_count) &&
        tri.ok && tri.source_signature == expected_sig && (expected_count == 0 || tri.count == expected_count) &&
        render.ok && render.source_signature == expected_sig &&
        duckdb_present;

    json out = {
        {"mode", "parcel-artifact-health"},
        {"file", file},
        {"ok", ok},
        {"resolved_source_signature", expected_sig},
        {"resolved_source_kind", resolved_kind},
        {"geojson", {
            {"present", geojson_present},
            {"optional_when_canonical_present", canonical_ok && file == "regional_parcels.geojson"},
            {"file_size_bytes", geojson_size}
        }},
        {"canonical_binary", {
            {"ok", canonical_ok},
            {"path", canonical_path.string()},
            {"file_size_bytes", canonical_ok ? canonical_meta.file_size_bytes : fileSizeOrZero(canonical_path)},
            {"source_signature", canonical_meta.source_signature},
            {"signature_match", canonical_ok && canonical_meta.source_signature == expected_sig},
            {"feature_count", canonical_meta.feature_count}
        }},
        {"hydration_cache", binaryHeaderJson(hydration, expected_sig, expected_count)},
        {"triangulation_cache", binaryHeaderJson(tri, expected_sig, expected_count)},
        {"parcel_render_cache", binaryHeaderJson(render, expected_sig, 0)},
        {"duckdb", {
            {"present", duckdb_present},
            {"path", duckdb_path.string()},
            {"file_size_bytes", duckdb_size}
        }},
        {"recommendations", std::move(recommendations)}
    };
    std::cout << out.dump(2) << '\n';
    return ok ? 0 : 1;
}

int runCanonicalParcelBinarySelftest(const fs::path& root) {
    std::vector<LayerDef::FeatureGeom> features;
    LayerDef::FeatureGeom polygon;
    polygon.extent.min_lon = -76.7f;
    polygon.extent.min_lat = 39.2f;
    polygon.extent.max_lon = -76.6f;
    polygon.extent.max_lat = 39.3f;
    polygon.rings.push_back({
        ImVec2(-76.7f, 39.2f),
        ImVec2(-76.6f, 39.2f),
        ImVec2(-76.6f, 39.3f),
        ImVec2(-76.7f, 39.3f)
    });
    polygon.properties.push_back({"regional_parcel_id", "BaltimoreCity:TEST123"});
    polygon.properties.push_back({"owner", "Canonical Parcel Test"});
    features.push_back(std::move(polygon));

    const fs::path test_dir = root / "data" / "cache" / "selftest";
    const fs::path cache_path = test_dir / "regional_parcels.geojson.canonical.bin";
    const fs::path resolver_geojson_path = test_dir / "regional_parcels.geojson";
    const fs::path resolver_canonical_path = test_dir / "regional_parcels.geojson.canonical.bin";
    const std::string sig = "canonical_selftest_sig";
    saveBinaryCanonicalFeatureCollection(cache_path, sig, features);

    CanonicalFeatureCollectionMetadata meta;
    const bool meta_ok = loadBinaryCanonicalMetadata(cache_path, meta);

    std::vector<LayerDef::FeatureGeom> loaded;
    const bool loaded_ok = loadBinaryCanonicalFeatureCollection(cache_path, sig, loaded);
    const bool stale_rejected = !loadBinaryCanonicalFeatureCollection(cache_path, "wrong_signature", loaded);

    {
        fs::create_directories(test_dir);
        std::ofstream geojson_out(resolver_geojson_path);
        geojson_out << "{\"type\":\"FeatureCollection\",\"features\":[]}";
    }
    std::string resolved_sig;
    std::string resolved_kind;
    const bool resolver_prefers_canonical =
        resolveLayerSourceSignature(resolver_geojson_path, resolved_sig, &resolved_kind) &&
        resolved_sig == sig &&
        resolved_kind == "canonical_binary";

    const bool roundtrip_ok =
        loaded_ok &&
        loaded.size() == 1 &&
        loaded[0].rings.size() == 1 &&
        loaded[0].rings[0].size() == 4 &&
        loaded[0].properties.size() == 2 &&
        loaded[0].properties[0].first == "regional_parcel_id" &&
        loaded[0].properties[0].second == "BaltimoreCity:TEST123" &&
        nearlyEqual(loaded[0].extent.max_lat, 39.3f);

    std::error_code ec;
    fs::remove(cache_path, ec);
    fs::remove(resolver_geojson_path, ec);
    fs::remove(resolver_canonical_path, ec);
    fs::remove(test_dir, ec);

    const bool ok =
        meta_ok &&
        meta.version == 1 &&
        meta.endian_marker == 0x01020304u &&
        meta.feature_count == 1 &&
        meta.source_signature == sig &&
        roundtrip_ok &&
        stale_rejected &&
        resolver_prefers_canonical;

    json out = {
        {"mode", "canonical-parcel-binary-selftest"},
        {"ok", ok},
        {"metadata_loaded", meta_ok},
        {"feature_count", meta.feature_count},
        {"source_signature", meta.source_signature},
        {"roundtrip_ok", roundtrip_ok},
        {"stale_signature_rejected", stale_rejected},
        {"resolver_prefers_canonical", resolver_prefers_canonical}
    };
    std::cout << out.dump(2) << '\n';
    return ok ? 0 : 1;
}

int inspectCanonicalParcelBinary(const fs::path& root, std::string file) {
    if (file.empty()) file = "regional_parcels.geojson";
    if (!isBareLayerFilename(file)) {
        std::cout << json{
            {"mode", "inspect-canonical-parcel-binary"},
            {"file", file},
            {"ok", false},
            {"error", "requires a layer filename, not a path"}
        }.dump(2) << '\n';
        return 2;
    }

    const fs::path canonical_path = resolveStoredLayerPathForFile(root, file).parent_path() / canonicalBinaryPathForLayerFile(file);
    CanonicalFeatureCollectionMetadata meta;
    if (!loadBinaryCanonicalMetadata(canonical_path, meta)) {
        std::cout << json{
            {"mode", "inspect-canonical-parcel-binary"},
            {"file", file},
            {"ok", false},
            {"canonical_path", canonical_path.string()},
            {"error", "failed to read canonical parcel binary metadata"}
        }.dump(2) << '\n';
        return 1;
    }

    const double bytes_per_feature =
        meta.feature_count == 0 ? 0.0 : (double)meta.file_size_bytes / (double)meta.feature_count;
    std::cout << json{
        {"mode", "inspect-canonical-parcel-binary"},
        {"file", file},
        {"ok", true},
        {"canonical_path", canonical_path.string()},
        {"version", meta.version},
        {"endian_marker", meta.endian_marker},
        {"source_signature", meta.source_signature},
        {"feature_count", meta.feature_count},
        {"file_size_bytes", meta.file_size_bytes},
        {"bytes_per_feature", bytes_per_feature}
    }.dump(2) << '\n';
    return 0;
}

int validateCanonicalParcelBinary(const fs::path& root, std::string file) {
    if (file.empty()) file = "regional_parcels.geojson";
    if (!isBareLayerFilename(file)) {
        std::cout << json{
            {"mode", "validate-canonical-parcel-binary"},
            {"file", file},
            {"ok", false},
            {"error", "requires a layer filename, not a path"}
        }.dump(2) << '\n';
        return 2;
    }

    const fs::path layer_path = resolveStoredLayerPathForFile(root, file);
    if (!fs::exists(layer_path)) {
        std::cout << json{
            {"mode", "validate-canonical-parcel-binary"},
            {"file", file},
            {"ok", false},
            {"error", "source geojson is required for comparison validation"}
        }.dump(2) << '\n';
        return 1;
    }

    std::string sig;
    std::string sig_source_kind;
    if (!resolveLayerSourceSignature(layer_path, sig, &sig_source_kind)) {
        std::cout << json{
            {"mode", "validate-canonical-parcel-binary"},
            {"file", file},
            {"ok", false},
            {"error", "failed to resolve source signature"}
        }.dump(2) << '\n';
        return 1;
    }

    std::vector<LayerDef::FeatureGeom> geojson_features;
    std::string geojson_error;
    const bool geojson_ok = collectHydratedFeaturesFromGeoJson(layer_path, geojson_features, geojson_error);
    std::vector<LayerDef::FeatureGeom> canonical_features;
    const fs::path canonical_path = resolveStoredLayerPathForFile(root, file).parent_path() / canonicalBinaryPathForLayerFile(file);
    const bool canonical_ok = loadBinaryCanonicalFeatureCollection(canonical_path, sig, canonical_features);
    CanonicalFeatureCollectionMetadata meta;
    const bool meta_ok = loadBinaryCanonicalMetadata(canonical_path, meta);

    bool representative_match = false;
    if (geojson_ok && canonical_ok && !geojson_features.empty() && !canonical_features.empty()) {
        representative_match =
            geojson_features[0].rings.size() == canonical_features[0].rings.size() &&
            geojson_features[0].properties.size() == canonical_features[0].properties.size() &&
            nearlyEqual(geojson_features[0].extent.min_lon, canonical_features[0].extent.min_lon) &&
            nearlyEqual(geojson_features[0].extent.max_lat, canonical_features[0].extent.max_lat);
    }

    const bool ok =
        geojson_ok &&
        canonical_ok &&
        meta_ok &&
        meta.source_signature == sig &&
        geojson_features.size() == canonical_features.size() &&
        (geojson_features.empty() || representative_match);

    std::cout << json{
        {"mode", "validate-canonical-parcel-binary"},
        {"file", file},
        {"ok", ok},
        {"source_signature", sig},
        {"source_signature_kind", sig_source_kind},
        {"canonical_source_signature", meta.source_signature},
        {"geojson_features", geojson_features.size()},
        {"canonical_features", canonical_features.size()},
        {"representative_match", representative_match},
        {"geojson_error", geojson_error}
    }.dump(2) << '\n';
    return ok ? 0 : 1;
}

json warmHydrationCacheOne(const fs::path& root, const std::string& file, int& exit_code) {
    exit_code = 0;
    if (!isBareLayerFilename(file)) {
        exit_code = 2;
        return {
            {"mode", "warm-hydration-cache"},
            {"file", file},
            {"ok", false},
            {"error", "requires a layer filename, not a path"}
        };
    }
    const fs::path layer_path = resolveStoredLayerPathForFile(root, file);
    const fs::path binary_path = root / "data" / "cache" / "hydration" / (file + ".bin");
    std::string sig;
    std::string sig_source_kind;
    if (!resolveLayerSourceSignature(layer_path, sig, &sig_source_kind)) {
        exit_code = 1;
        return {
            {"mode", "warm-hydration-cache"},
            {"file", file},
            {"ok", false},
            {"error", "failed to resolve source signature"}
        };
    }

    std::vector<LayerDef::FeatureGeom> features;
    std::string source_used;
    std::string error;
    if (!ensureHydrationCacheReady(root, file, sig, features, source_used, error)) {
        exit_code = 1;
        return {
            {"mode", "warm-hydration-cache"},
            {"file", file},
            {"ok", false},
            {"source_signature", sig},
            {"source_signature_kind", sig_source_kind},
            {"error", error}
        };
    }
    if (binaryHydrationCacheShouldBeCompacted(binary_path, features)) {
        saveBinaryHydrationCache(binary_path, sig, features);
        source_used += "+compacted";
    }

    std::vector<LayerDef::FeatureGeom> verify;
    const bool verify_ok = loadBinaryHydrationCache(binary_path, sig, verify);
    const bool ok = verify_ok && verify.size() == features.size();
    if (!ok) exit_code = 1;
    return {
        {"mode", "warm-hydration-cache"},
        {"file", file},
        {"ok", ok},
        {"source_signature", sig},
        {"source_signature_kind", sig_source_kind},
        {"source", source_used},
        {"features", features.size()},
        {"verified_features", verify.size()},
        {"binary_cache", binary_path.string()}
    };
}

int warmHydrationCache(const fs::path& root, const std::string& file) {
    int exit_code = 0;
    const json out = warmHydrationCacheOne(root, file, exit_code);
    std::cout << out.dump(2) << '\n';
    return exit_code;
}

int warmHydrationCacheAll(const fs::path& root) {
    const fs::path layers_dir = root / "data" / "provenance" / "stored";
    std::error_code ec;
    if (!fs::exists(layers_dir, ec)) {
        std::cerr << "layers directory missing: " << layers_dir << '\n';
        return 2;
    }

    std::vector<std::string> candidates;
    for (const auto& entry : fs::directory_iterator(layers_dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        std::string layer_file;
        if (entry.path().extension() == ".geojson") {
            layer_file = name;
        } else if (name.ends_with(".geojson.canonical.bin")) {
            layer_file = name.substr(0, name.size() - std::strlen(".canonical.bin"));
        } else {
            continue;
        }
        const fs::path binary_path = root / "data" / "cache" / "hydration" / (layer_file + ".bin");
        if (fs::exists(binary_path)) candidates.push_back(layer_file);
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

    json results = json::array();
    size_t ok_count = 0;
    size_t failed_count = 0;
    for (const std::string& file : candidates) {
        int one_exit = 0;
        json one = warmHydrationCacheOne(root, file, one_exit);
        results.push_back(one);
        if (one_exit == 0) ok_count += 1;
        else failed_count += 1;
    }

    json out = {
        {"mode", "warm-hydration-cache-all"},
        {"ok", failed_count == 0},
        {"candidate_count", candidates.size()},
        {"ok_count", ok_count},
        {"failed_count", failed_count},
        {"results", std::move(results)}
    };
    std::cout << out.dump(2) << '\n';
    return failed_count == 0 ? 0 : 1;
}

int rebuildDuckDbAnalyticsCli(const fs::path& root, int reserve_cores) {
    std::vector<LayerDef> layers = loadManifest(root);
    const unsigned int hw = std::max(1u, std::thread::hardware_concurrency());
    const unsigned int worker_count = std::max(1u, hw > (unsigned int)std::max(0, reserve_cores) ? hw - (unsigned int)std::max(0, reserve_cores) : 1u);

    HeadlessLayerHydrationSummary hydration_summary;
    const bool hydration_ok = hydrateLocalLayersHeadless(
        root,
        layers,
        HeadlessLayerHydrationOptions{worker_count, true},
        hydration_summary);

    WorldsimLayerIndices indices = detectWorldsimLayerIndices(root, layers);
    ParcelConsolidationArtifacts artifacts = buildParcelConsolidationArtifacts(root, layers, indices);

    DuckDbAnalytics analytics(root);
    const bool rebuild_ok = hydration_ok && analytics.rebuild(layers, artifacts.unified_parcels);

    json out = {
        {"mode", "rebuild-duckdb-analytics"},
        {"ok", rebuild_ok},
        {"worker_count", worker_count},
        {"hydration", {
            {"ok", hydration_ok},
            {"local_layer_count", hydration_summary.local_layer_count},
            {"requested_layer_count", hydration_summary.requested_layer_count},
            {"hydrated_layer_count", hydration_summary.hydrated_layer_count},
            {"failed_layer_count", hydration_summary.failed_layer_count},
            {"skipped_missing_layer_count", hydration_summary.skipped_missing_layer_count},
            {"total_feature_count", hydration_summary.total_feature_count},
            {"elapsed_ms", hydration_summary.elapsed_ms}
        }},
        {"unified_parcels", artifacts.unified_parcels.size()},
        {"duckdb", {
            {"available", analytics.status().available},
            {"last_rebuild_ok", analytics.status().last_rebuild_ok},
            {"layer_count", analytics.status().layer_count},
            {"feature_count", analytics.status().feature_count},
            {"db_path", analytics.status().db_path},
            {"message", analytics.status().message}
        }}
    };
    if (!hydration_summary.failures.empty()) {
        json failures = json::array();
        for (const auto& failure : hydration_summary.failures) {
            failures.push_back({
                {"layer_index", failure.layer_index},
                {"layer_file", failure.layer_file},
                {"error", failure.error}
            });
        }
        out["hydration"]["failures"] = std::move(failures);
    }

    std::cout << out.dump(2) << '\n';
    return rebuild_ok ? 0 : 1;
}

int inspectDuckDbGeographyTablesCli(const fs::path& root) {
    json out = {
        {"mode", "inspect-duckdb-geography-tables"},
        {"db_path", (root / "data" / "worldsim.duckdb").string()}
    };
    try {
        duckdb::DuckDB db(root / "data" / "worldsim.duckdb");
        duckdb::Connection con(db);

        auto table_rows = con.Query(R"SQL(
            SELECT table_name
            FROM information_schema.tables
            WHERE table_schema = 'main'
            ORDER BY table_name
        )SQL");
        if (!table_rows || table_rows->HasError()) {
            out["ok"] = false;
            out["error"] = table_rows ? table_rows->GetError() : "failed to query information_schema.tables";
            std::cout << out.dump(2) << '\n';
            return 1;
        }

        json tables = json::array();
        std::unordered_set<std::string> table_names;
        for (size_t i = 0; i < (size_t)table_rows->RowCount(); ++i) {
            const std::string name = table_rows->GetValue(0, i).ToString();
            tables.push_back(name);
            table_names.insert(name);
        }
        out["tables"] = std::move(tables);

        auto count_one = [&](const std::string& sql) -> json {
            auto res = con.Query(sql);
            if (!res || res->HasError()) {
                json j = json::object();
                j["ok"] = false;
                j["error"] = res ? res->GetError() : "query failed";
                return j;
            }
            json j = json::object();
            j["ok"] = true;
            j["count"] = res->GetValue<int64_t>(0, 0);
            return j;
        };

        out["table_presence"] = {
            {"geography_feature_collections", table_names.contains("geography_feature_collections")},
            {"anambra_runtime_features", table_names.contains("anambra_runtime_features")},
            {"anambra_runtime_lga_summary", table_names.contains("anambra_runtime_lga_summary")},
            {"import_audit", table_names.contains("import_audit")},
            {"layer_features", table_names.contains("layer_features")}
        };

        out["base_counts"] = {
            {"layer_features_anambra", count_one(
                "SELECT count(*)::BIGINT FROM layer_features "
                "WHERE provenance_nation_state = 'ng' AND provenance_state_region = 'anambra'")},
            {"import_audit_anambra", count_one(
                "SELECT count(*)::BIGINT FROM import_audit "
                "WHERE provenance_nation_state = 'ng' AND provenance_state_region = 'anambra'")}
        };

        const char* runtime_features_sql = R"SQL(
            SELECT count(*)::BIGINT
            FROM (
                SELECT
                    layer_file,
                    layer_name,
                    duckdb_role,
                    category,
                    feature_idx,
                    min_lon,
                    min_lat,
                    max_lon,
                    max_lat,
                    coalesce(
                        json_extract_string(properties_json, '$.name'),
                        json_extract_string(properties_json, '$.poi_name'),
                        json_extract_string(properties_json, '$.prmry_name'),
                        json_extract_string(properties_json, '$.set_name'),
                        json_extract_string(properties_json, '$.market_nam'),
                        json_extract_string(properties_json, '$.plc_st_nam'),
                        json_extract_string(properties_json, '$.fctry_st_n')
                    ) AS feature_name,
                    json_extract_string(properties_json, '$.lganame') AS lga_name,
                    json_extract_string(properties_json, '$.wardname') AS ward_name,
                    json_extract_string(properties_json, '$.source') AS source_name,
                    properties_json
                FROM layer_features
                WHERE provenance_nation_state = 'ng'
                  AND provenance_state_region = 'anambra'
            ) t
        )SQL";
        const char* lga_summary_sql = R"SQL(
            SELECT count(*)::BIGINT
            FROM (
                SELECT
                    layer_file,
                    layer_name,
                    coalesce(json_extract_string(properties_json, '$.lganame'), '') AS lga_name,
                    count(*) AS feature_count
                FROM layer_features
                WHERE provenance_nation_state = 'ng'
                  AND provenance_state_region = 'anambra'
                GROUP BY layer_file, layer_name, coalesce(json_extract_string(properties_json, '$.lganame'), '')
            ) t
        )SQL";

        out["derivation_diagnostics"] = {
            {"anambra_runtime_features_query", count_one(runtime_features_sql)},
            {"anambra_runtime_lga_summary_query", count_one(lga_summary_sql)}
        };

        if (table_names.contains("anambra_runtime_features")) {
            out["materialized_counts"] = {
                {"anambra_runtime_features", count_one("SELECT count(*)::BIGINT FROM anambra_runtime_features")}
            };
        }
        if (table_names.contains("anambra_runtime_lga_summary")) {
            out["materialized_counts"]["anambra_runtime_lga_summary"] =
                count_one("SELECT count(*)::BIGINT FROM anambra_runtime_lga_summary");
        }

        out["ok"] = true;
        std::cout << out.dump(2) << '\n';
        return 0;
    } catch (const std::exception& e) {
        out["ok"] = false;
        out["error"] = e.what();
        std::cout << out.dump(2) << '\n';
        return 1;
    }
}

json warmTriangulationCacheOne(const fs::path& root, const std::string& file, int& exit_code) {
    exit_code = 0;
    if (!isBareLayerFilename(file)) {
        exit_code = 2;
        return {
            {"mode", "warm-triangulation-cache"},
            {"file", file},
            {"ok", false},
            {"error", "requires a layer filename, not a path"}
        };
    }
    const fs::path layer_path = resolveStoredLayerPathForFile(root, file);
    const fs::path binary_path = root / "data" / "cache" / "triangulation" / (file + ".tri.bin");
    std::string sig;
    std::string sig_source_kind;
    if (!resolveLayerSourceSignature(layer_path, sig, &sig_source_kind)) {
        exit_code = 1;
        return {
            {"mode", "warm-triangulation-cache"},
            {"file", file},
            {"ok", false},
            {"error", "failed to resolve source signature"}
        };
    }

    std::vector<std::vector<uint32_t>> tris;
    if (fs::exists(binary_path)) {
        const auto count = readBinaryTriCacheCount(binary_path);
        if (count && loadBinaryTriCache(binary_path, sig, *count, tris)) {
            return {
                {"mode", "warm-triangulation-cache"},
                {"file", file},
                {"ok", true},
                {"source_signature", sig},
                {"source", "binary"},
                {"feature_vectors", tris.size()},
                {"binary_cache", binary_path.string()}
            };
        }
    }

    size_t hydrated_feature_count = 0;
    std::vector<LayerDef::FeatureGeom> features;
    std::string hydration_source;
    std::string hydration_error;
    if (!ensureHydrationCacheReady(root, file, sig, features, hydration_source, hydration_error)) {
        exit_code = 1;
        return {
            {"mode", "warm-triangulation-cache"},
            {"file", file},
            {"ok", false},
            {"source_signature", sig},
            {"source_signature_kind", sig_source_kind},
            {"error", hydration_error}
        };
    }
    hydrated_feature_count = features.size();
    tris.resize(features.size());
    for (size_t i = 0; i < features.size(); ++i) {
        if (!features[i].rings.empty()) tris[i] = triangulateRings(features[i].rings);
    }
    const std::string source_used = hydration_source == "binary" ? "hydration_binary" : ("hydration_" + hydration_source);
    saveBinaryTriCache(binary_path, sig, tris);
    std::vector<std::vector<uint32_t>> verify;
    const bool verify_ok = loadBinaryTriCache(binary_path, sig, tris.size(), verify);
    const bool ok = verify_ok && verify == tris;
    if (!ok) exit_code = 1;
    return {
        {"mode", "warm-triangulation-cache"},
        {"file", file},
        {"ok", ok},
        {"source_signature", sig},
        {"source_signature_kind", sig_source_kind},
        {"source", source_used},
        {"hydrated_features", hydrated_feature_count},
        {"feature_vectors", tris.size()},
        {"verified_feature_vectors", verify.size()},
        {"binary_cache", binary_path.string()}
    };
}

int warmTriangulationCache(const fs::path& root, const std::string& file) {
    int exit_code = 0;
    const json out = warmTriangulationCacheOne(root, file, exit_code);
    std::cout << out.dump(2) << '\n';
    return exit_code;
}

int warmTriangulationCacheAll(const fs::path& root) {
    const fs::path tri_dir = root / "data" / "cache" / "triangulation";
    std::error_code ec;
    if (!fs::exists(tri_dir, ec)) {
        std::cerr << "triangulation cache directory missing: " << tri_dir << '\n';
        return 2;
    }
    std::vector<std::string> candidates;
    for (const auto& entry : fs::directory_iterator(tri_dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        if (name.ends_with(".tri.bin")) {
            candidates.push_back(name.substr(0, name.size() - std::strlen(".tri.bin")));
        }
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

    json results = json::array();
    size_t ok_count = 0;
    size_t failed_count = 0;
    for (const std::string& file : candidates) {
        int one_exit = 0;
        json one = warmTriangulationCacheOne(root, file, one_exit);
        results.push_back(one);
        if (one_exit == 0) ok_count += 1;
        else failed_count += 1;
    }

    json out = {
        {"mode", "warm-triangulation-cache-all"},
        {"ok", failed_count == 0},
        {"candidate_count", candidates.size()},
        {"ok_count", ok_count},
        {"failed_count", failed_count},
        {"results", std::move(results)}
    };
    std::cout << out.dump(2) << '\n';
    return failed_count == 0 ? 0 : 1;
}

json warmParcelRenderCacheOne(const fs::path& root, const std::string& file, int& exit_code) {
    exit_code = 0;
    if (!isBareLayerFilename(file)) {
        exit_code = 2;
        return {
            {"mode", "warm-parcel-render-cache"},
            {"file", file},
            {"ok", false},
            {"error", "requires a layer filename, not a path"}
        };
    }

    const fs::path layer_path = resolveStoredLayerPathForFile(root, file);
    const fs::path hydration_path = root / "data" / "cache" / "hydration" / (file + ".bin");
    const fs::path tri_path = root / "data" / "cache" / "triangulation" / (file + ".tri.bin");
    const fs::path render_path = root / "data" / "cache" / "render" / (file + ".parcel-render.bin");
    std::string sig;
    std::string sig_source_kind;
    if (!resolveLayerSourceSignature(layer_path, sig, &sig_source_kind)) {
        exit_code = 1;
        return {
            {"mode", "warm-parcel-render-cache"},
            {"file", file},
            {"ok", false},
            {"error", "failed to resolve source signature"}
        };
    }

    std::vector<LayerDef::FeatureGeom> features;
    if (!loadBinaryHydrationCache(hydration_path, sig, features)) {
        int warm_exit = 0;
        json warmed = warmHydrationCacheOne(root, file, warm_exit);
        if (warm_exit != 0 || !loadBinaryHydrationCache(hydration_path, sig, features)) {
            exit_code = 1;
            return {
                {"mode", "warm-parcel-render-cache"},
                {"file", file},
                {"ok", false},
                {"source_signature", sig},
                {"source_signature_kind", sig_source_kind},
                {"error", warmed.value("error", std::string("no valid binary hydration cache"))}
            };
        }
    }

    std::vector<std::vector<uint32_t>> tris;
    if (!loadBinaryTriCache(tri_path, sig, features.size(), tris)) {
        int warm_exit = 0;
        json warmed = warmTriangulationCacheOne(root, file, warm_exit);
        if (warm_exit != 0 || !loadBinaryTriCache(tri_path, sig, features.size(), tris)) {
            exit_code = 1;
            return {
                {"mode", "warm-parcel-render-cache"},
                {"file", file},
                {"ok", false},
                {"source_signature", sig},
                {"source_signature_kind", sig_source_kind},
                {"hydrated_features", features.size()},
                {"error", warmed.value("error", std::string("no valid binary triangulation cache"))}
            };
        }
    }

    const size_t feature_count = std::min(features.size(), tris.size());
    for (size_t i = 0; i < feature_count; ++i) features[i].triangles = std::move(tris[i]);

    ParcelRenderCacheBlob blob;
    const bool built = buildParcelRenderCacheBlob(features, sig, blob);
    if (!built) {
        exit_code = 1;
        return {
            {"mode", "warm-parcel-render-cache"},
            {"file", file},
            {"ok", false},
            {"source_signature", sig},
            {"hydrated_features", features.size()},
            {"error", "failed to build parcel render cache blob"}
        };
    }

    saveBinaryParcelRenderCache(render_path, blob);
    ParcelRenderCacheBlob verify;
    const bool verify_ok = loadBinaryParcelRenderCache(render_path, sig, verify);
    const bool ok =
        verify_ok &&
        verify.source_signature == sig &&
        verify.vertices.size() == blob.vertices.size() &&
        verify.indices.size() == blob.indices.size() &&
        verify.features.size() == blob.features.size() &&
        verify.chunks.size() == blob.chunks.size();
    if (!ok) exit_code = 1;

    return {
        {"mode", "warm-parcel-render-cache"},
        {"file", file},
        {"ok", ok},
        {"source_signature", sig},
        {"source_signature_kind", sig_source_kind},
        {"hydrated_features", features.size()},
        {"render_features", blob.features.size()},
        {"vertices", blob.vertices.size()},
        {"indices", blob.indices.size()},
        {"chunks", blob.chunks.size()},
        {"cache_path", render_path.string()}
    };
}

int warmParcelRenderCache(const fs::path& root, const std::string& file) {
    int exit_code = 0;
    const json out = warmParcelRenderCacheOne(root, file, exit_code);
    std::cout << out.dump(2) << '\n';
    return exit_code;
}

int warmParcelRenderCacheAll(const fs::path& root) {
    const fs::path tri_dir = root / "data" / "cache" / "triangulation";
    std::error_code ec;
    if (!fs::exists(tri_dir, ec)) {
        std::cerr << "triangulation cache directory missing: " << tri_dir << '\n';
        return 2;
    }

    std::vector<std::string> candidates;
    for (const auto& entry : fs::directory_iterator(tri_dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        if (name.ends_with(".tri.bin")) candidates.push_back(name.substr(0, name.size() - std::strlen(".tri.bin")));
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

    json results = json::array();
    size_t ok_count = 0;
    size_t failed_count = 0;
    for (const std::string& file : candidates) {
        int one_exit = 0;
        json one = warmParcelRenderCacheOne(root, file, one_exit);
        results.push_back(one);
        if (one_exit == 0) ok_count += 1;
        else failed_count += 1;
    }

    json out = {
        {"mode", "warm-parcel-render-cache-all"},
        {"ok", failed_count == 0},
        {"candidate_count", candidates.size()},
        {"ok_count", ok_count},
        {"failed_count", failed_count},
        {"results", std::move(results)}
    };
    std::cout << out.dump(2) << '\n';
    return failed_count == 0 ? 0 : 1;
}

json skippedWarmStep(const std::string& mode, const std::string& file, const std::string& reason) {
    return {
        {"mode", mode},
        {"file", file},
        {"ok", false},
        {"skipped", true},
        {"reason", reason}
    };
}

json warmParcelRuntimeStackOne(const fs::path& root, std::string file, int& exit_code) {
    if (file.empty()) file = "regional_parcels.geojson";
    exit_code = 0;
    if (!isBareLayerFilename(file)) {
        exit_code = 2;
        return {
            {"mode", "warm-parcel-runtime-stack"},
            {"file", file},
            {"ok", false},
            {"error", "requires a layer filename, not a path"}
        };
    }

    int hydration_exit = 0;
    json hydration = warmHydrationCacheOne(root, file, hydration_exit);

    int tri_exit = 0;
    json triangulation = hydration_exit == 0
        ? warmTriangulationCacheOne(root, file, tri_exit)
        : skippedWarmStep("warm-triangulation-cache", file, "hydration cache warm failed");

    int render_exit = 0;
    json render = (hydration_exit == 0 && tri_exit == 0)
        ? warmParcelRenderCacheOne(root, file, render_exit)
        : skippedWarmStep("warm-parcel-render-cache", file, "upstream cache warm failed");

    const bool ok = hydration_exit == 0 && tri_exit == 0 && render_exit == 0;
    exit_code = ok ? 0 : 1;
    return {
        {"mode", "warm-parcel-runtime-stack"},
        {"file", file},
        {"ok", ok},
        {"hydration_cache", std::move(hydration)},
        {"triangulation_cache", std::move(triangulation)},
        {"parcel_render_cache", std::move(render)},
        {"canonical_binary", {
            {"rebuilt", false},
            {"reason", "canonical binary rebuild/export remains explicit"}
        }},
        {"duckdb", {
            {"rebuilt", false},
            {"reason", "DuckDB analytics rebuild remains explicit and is not required for parcel geometry rendering"}
        }},
        {"health_check", "run --parcel-artifact-health " + file}
    };
}

int warmParcelRuntimeStack(const fs::path& root, const std::string& file) {
    int exit_code = 0;
    const json out = warmParcelRuntimeStackOne(root, file, exit_code);
    std::cout << out.dump(2) << '\n';
    return exit_code;
}
}

WorldsimCliOptions parseWorldsimCliOptions(int argc, char** argv) {
    WorldsimCliOptions options;
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--vacancy-selftest") {
            options.run_vacancy_selftest = true;
            continue;
        }
        if (arg == "--hydration-cache-selftest") {
            options.run_cache_selftest = true;
            continue;
        }
        if (arg == "--triangulation-cache-selftest") {
            options.run_triangulation_cache_selftest = true;
            continue;
        }
        if (arg == "--projection-cache-selftest") {
            options.run_projection_cache_selftest = true;
            continue;
        }
        if (arg == "--projection-fill-cache-selftest") {
            options.run_projection_fill_cache_selftest = true;
            continue;
        }
        if (arg == "--projection-color-cache-selftest") {
            options.run_projection_color_cache_selftest = true;
            continue;
        }
        if (arg == "--polygon-hole-selftest") {
            options.run_polygon_hole_selftest = true;
            continue;
        }
        if (arg == "--parcel-render-cache-selftest") {
            options.run_parcel_render_cache_selftest = true;
            continue;
        }
        if (arg == "--triangulation-apply-selftest") {
            options.run_triangulation_apply_selftest = true;
            continue;
        }
        if (arg == "--spatial-index-selftest") {
            options.run_spatial_index_selftest = true;
            continue;
        }
        if (arg == "--layer-profile-selftest") {
            options.run_layer_profile_selftest = true;
            continue;
        }
        if (arg == "--layer-runtime-status-selftest") {
            options.run_layer_runtime_status_selftest = true;
            continue;
        }
        if (arg == "--parcel-gpu-cpu-bypass-selftest") {
            options.run_parcel_gpu_cpu_bypass_selftest = true;
            continue;
        }
        if (arg == "--render-policy-selftest") {
            options.run_render_policy_selftest = true;
            continue;
        }
        if (arg == "--render-plan-selftest") {
            options.run_render_plan_selftest = true;
            continue;
        }
        if (arg == "--canonical-parcel-binary-selftest") {
            options.run_canonical_parcel_binary_selftest = true;
            continue;
        }
        if (arg == "--inspect-canonical-parcel-binary") {
            options.run_inspect_canonical_parcel_binary = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') options.canonical_parcel_binary_file = argv[++i];
            continue;
        }
        if (arg == "--validate-canonical-parcel-binary") {
            options.run_validate_canonical_parcel_binary = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') options.canonical_parcel_binary_file = argv[++i];
            continue;
        }
        if (arg == "--parcel-artifact-health") {
            options.run_parcel_artifact_health = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') options.canonical_parcel_binary_file = argv[++i];
            continue;
        }
        if (arg == "--warm-parcel-runtime-stack") {
            options.run_warm_parcel_runtime_stack = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') options.warm_parcel_runtime_stack_file = argv[++i];
            continue;
        }
        if (arg == "--warm-hydration-cache") {
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                options.run_warm_hydration_cache = true;
                options.warm_hydration_cache_file = argv[++i];
            }
            continue;
        }
        if (arg == "--warm-hydration-cache-all") {
            options.run_warm_hydration_cache_all = true;
            continue;
        }
        if (arg == "--warm-triangulation-cache") {
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                options.run_warm_triangulation_cache = true;
                options.warm_triangulation_cache_file = argv[++i];
            }
            continue;
        }
        if (arg == "--warm-triangulation-cache-all") {
            options.run_warm_triangulation_cache_all = true;
            continue;
        }
        if (arg == "--warm-parcel-render-cache") {
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                options.run_warm_parcel_render_cache = true;
                options.warm_parcel_render_cache_file = argv[++i];
            }
            continue;
        }
        if (arg == "--warm-parcel-render-cache-all") {
            options.run_warm_parcel_render_cache_all = true;
            continue;
        }
        if (arg.rfind("--warm-hydration-cache=", 0) == 0) {
            options.run_warm_hydration_cache = true;
            options.warm_hydration_cache_file = arg.substr(std::strlen("--warm-hydration-cache="));
            continue;
        }
        if (arg.rfind("--warm-triangulation-cache=", 0) == 0) {
            options.run_warm_triangulation_cache = true;
            options.warm_triangulation_cache_file = arg.substr(std::strlen("--warm-triangulation-cache="));
            continue;
        }
        if (arg.rfind("--warm-parcel-render-cache=", 0) == 0) {
            options.run_warm_parcel_render_cache = true;
            options.warm_parcel_render_cache_file = arg.substr(std::strlen("--warm-parcel-render-cache="));
            continue;
        }
        if (arg.rfind("--warm-parcel-runtime-stack=", 0) == 0) {
            options.run_warm_parcel_runtime_stack = true;
            options.warm_parcel_runtime_stack_file = arg.substr(std::strlen("--warm-parcel-runtime-stack="));
            continue;
        }
        if (arg.rfind("--inspect-canonical-parcel-binary=", 0) == 0) {
            options.run_inspect_canonical_parcel_binary = true;
            options.canonical_parcel_binary_file = arg.substr(std::strlen("--inspect-canonical-parcel-binary="));
            continue;
        }
        if (arg.rfind("--validate-canonical-parcel-binary=", 0) == 0) {
            options.run_validate_canonical_parcel_binary = true;
            options.canonical_parcel_binary_file = arg.substr(std::strlen("--validate-canonical-parcel-binary="));
            continue;
        }
        if (arg.rfind("--parcel-artifact-health=", 0) == 0) {
            options.run_parcel_artifact_health = true;
            options.canonical_parcel_binary_file = arg.substr(std::strlen("--parcel-artifact-health="));
            continue;
        }
        if (arg == "--download-layers") {
            options.run_download_layers = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                options.download_phase = argv[++i];
            } else {
                options.download_phase = "all";
            }
            continue;
        }
        if (arg == "--rebuild-duckdb-analytics") {
            options.run_rebuild_duckdb_analytics = true;
            continue;
        }
        if (arg == "--inspect-duckdb-geography-tables") {
            options.run_inspect_duckdb_geography_tables = true;
            continue;
        }
        if (arg == "--build-parcel-matched-layers") {
            options.run_build_parcel_matched_layers = true;
            continue;
        }
        if (arg == "--force-build-parcel-matched-layers") {
            options.run_build_parcel_matched_layers = true;
            options.force_build_parcel_matched_layers = true;
            continue;
        }
        if (arg.rfind("--download-layers=", 0) == 0) {
            options.run_download_layers = true;
            options.download_phase = arg.substr(std::strlen("--download-layers="));
            continue;
        }
        if (arg == "--include-large") {
            options.include_large_downloads = true;
            continue;
        }
        if (arg == "--reserve-one-core") {
            options.reserve_cores = 1;
            options.reserve_cores_set = true;
            continue;
        }
        if (arg == "--reserve-cores") {
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                options.reserve_cores = std::max(0, std::atoi(argv[++i]));
                options.reserve_cores_set = true;
            }
            continue;
        }
        if (arg.rfind("--reserve-cores=", 0) == 0) {
            options.reserve_cores = std::max(0, std::atoi(arg.c_str() + std::strlen("--reserve-cores=")));
            options.reserve_cores_set = true;
            continue;
        }
        if (arg == "--help" || arg == "-h") {
            options.show_help = true;
            continue;
        }
    }
    return options;
}

void printWorldsimUsage() {
    std::cout
        << "Usage: worldsim3 [--reserve-one-core|--reserve-cores N]\n"
        << "       worldsim3 [--download-layers [all|must-have|nice-to-have|heavy-data|capital-flows|anambra-runtime|anambra-repository|extended-events|historical-high-quality|archival-research]] [--include-large]\n"
        << "       worldsim3 --rebuild-duckdb-analytics [--reserve-cores N]\n"
        << "       worldsim3 --inspect-duckdb-geography-tables\n"
        << "       worldsim3 [--build-parcel-matched-layers|--force-build-parcel-matched-layers]\n"
        << "       worldsim3 --warm-hydration-cache LAYER_FILE\n"
        << "       worldsim3 --warm-hydration-cache-all\n"
        << "       worldsim3 --warm-triangulation-cache LAYER_FILE\n"
        << "       worldsim3 --warm-triangulation-cache-all\n"
        << "       worldsim3 --warm-parcel-render-cache LAYER_FILE\n"
        << "       worldsim3 --warm-parcel-render-cache-all\n"
        << "       worldsim3 --warm-parcel-runtime-stack [LAYER_FILE]\n"
        << "       worldsim3 --canonical-parcel-binary-selftest\n"
        << "       worldsim3 --inspect-canonical-parcel-binary [LAYER_FILE]\n"
        << "       worldsim3 --validate-canonical-parcel-binary [LAYER_FILE]\n"
        << "       worldsim3 --parcel-artifact-health [LAYER_FILE]\n"
        << "       worldsim3 --hydration-cache-selftest\n"
        << "       worldsim3 --triangulation-cache-selftest\n"
        << "       worldsim3 --projection-cache-selftest\n"
        << "       worldsim3 --projection-fill-cache-selftest\n"
        << "       worldsim3 --projection-color-cache-selftest\n"
        << "       worldsim3 --polygon-hole-selftest\n"
        << "       worldsim3 --parcel-render-cache-selftest\n"
        << "       worldsim3 --triangulation-apply-selftest\n"
        << "       worldsim3 --spatial-index-selftest\n"
        << "       worldsim3 --layer-profile-selftest\n"
        << "       worldsim3 --layer-runtime-status-selftest\n"
        << "       worldsim3 --parcel-gpu-cpu-bypass-selftest\n"
        << "       worldsim3 --render-policy-selftest\n"
        << "       worldsim3 --render-plan-selftest\n"
        << "       worldsim3 --vacancy-selftest\n";
}

int runWorldsimCliImmediate(const fs::path& root, const WorldsimCliOptions& options) {
    if (options.show_help) {
        printWorldsimUsage();
        return 0;
    }
    if (options.run_vacancy_selftest) {
        return runVacancySelftest(root);
    }
    if (options.run_cache_selftest) {
        return runHydrationCacheSelftest(root);
    }
    if (options.run_triangulation_cache_selftest) {
        return runTriangulationCacheSelftest(root);
    }
    if (options.run_projection_cache_selftest) {
        return runProjectionCacheSelftest();
    }
    if (options.run_projection_fill_cache_selftest) {
        return runProjectionFillCacheSelftest();
    }
    if (options.run_projection_color_cache_selftest) {
        return runProjectionColorCacheSelftest();
    }
    if (options.run_polygon_hole_selftest) {
        return runPolygonHoleSelftest();
    }
    if (options.run_parcel_render_cache_selftest) {
        return runParcelRenderCacheSelftest(root);
    }
    if (options.run_triangulation_apply_selftest) {
        return runTriangulationApplySelftest();
    }
    if (options.run_spatial_index_selftest) {
        return runSpatialIndexSelftest();
    }
    if (options.run_layer_profile_selftest) {
        return runLayerProfileSelftest();
    }
    if (options.run_layer_runtime_status_selftest) {
        return runLayerRuntimeStatusSelftest();
    }
    if (options.run_parcel_gpu_cpu_bypass_selftest) {
        return runParcelGpuCpuBypassSelftest();
    }
    if (options.run_render_policy_selftest) {
        return runRenderPolicySelftest();
    }
    if (options.run_render_plan_selftest) {
        return runRenderPlanSelftest();
    }
    if (options.run_canonical_parcel_binary_selftest) {
        return runCanonicalParcelBinarySelftest(root);
    }
    if (options.run_inspect_canonical_parcel_binary) {
        return inspectCanonicalParcelBinary(root, options.canonical_parcel_binary_file);
    }
    if (options.run_validate_canonical_parcel_binary) {
        return validateCanonicalParcelBinary(root, options.canonical_parcel_binary_file);
    }
    if (options.run_parcel_artifact_health) {
        return parcelArtifactHealth(root, options.canonical_parcel_binary_file);
    }
    if (options.run_warm_parcel_runtime_stack) {
        return warmParcelRuntimeStack(root, options.warm_parcel_runtime_stack_file);
    }
    if (options.run_warm_hydration_cache) {
        return warmHydrationCache(root, options.warm_hydration_cache_file);
    }
    if (options.run_warm_hydration_cache_all) {
        return warmHydrationCacheAll(root);
    }
    if (options.run_warm_triangulation_cache) {
        return warmTriangulationCache(root, options.warm_triangulation_cache_file);
    }
    if (options.run_warm_triangulation_cache_all) {
        return warmTriangulationCacheAll(root);
    }
    if (options.run_warm_parcel_render_cache) {
        return warmParcelRenderCache(root, options.warm_parcel_render_cache_file);
    }
    if (options.run_warm_parcel_render_cache_all) {
        return warmParcelRenderCacheAll(root);
    }
    if (options.run_download_layers) {
        return runLayerDownloadCli(
            root,
            options.download_phase.empty() ? "all" : options.download_phase,
            options.include_large_downloads);
    }
    if (options.run_rebuild_duckdb_analytics) {
        return rebuildDuckDbAnalyticsCli(root, options.reserve_cores_set ? options.reserve_cores : 0);
    }
    if (options.run_inspect_duckdb_geography_tables) {
        return inspectDuckDbGeographyTablesCli(root);
    }
    if (options.run_build_parcel_matched_layers) {
        ensureParcelMatchedEventLayers(root, options.force_build_parcel_matched_layers, &std::cout);
        return 0;
    }
    return -1;
}
