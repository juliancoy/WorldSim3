#include "worldsim_cli.h"

#include "cache_io.h"
#include "map_render_projection.h"
#include "worldsim_dataset_bootstrap.h"
#include "worldsim_app.h"
#include "parcel_matched_layers.h"
#include "vacancy_overlay.h"

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <deque>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
bool nearlyEqual(float a, float b) {
    return std::fabs(a - b) <= 0.00001f;
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

    std::error_code ec;
    fs::remove(cache_path, ec);
    fs::remove(test_dir, ec);

    json out = {
        {"mode", "hydration-cache-selftest"},
        {"ok", same && stale_rejected},
        {"loaded", loaded_ok},
        {"roundtrip_features", loaded.size()},
        {"stale_signature_rejected", stale_rejected}
    };
    std::cout << out.dump(2) << '\n';
    return (same && stale_rejected) ? 0 : 1;
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

bool isBareLayerFilename(const std::string& file) {
    return !file.empty() &&
           file.find('/') == std::string::npos &&
           file.find('\\') == std::string::npos;
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

json warmHydrationCacheOne(const fs::path& root, const std::string& file, int& exit_code) {
    exit_code = 0;
    if (file.empty() || file.find('/') != std::string::npos || file.find('\\') != std::string::npos) {
        exit_code = 2;
        return {
            {"mode", "warm-hydration-cache"},
            {"file", file},
            {"ok", false},
            {"error", "requires a layer filename, not a path"}
        };
    }
    const fs::path layer_path = root / "data" / "layers" / file;
    const fs::path msgpack_path = root / "data" / "cache" / "hydration" / (file + ".msgpack");
    const fs::path binary_path = root / "data" / "cache" / "hydration" / (file + ".bin");
    const std::string sig = fileSignature(layer_path);

    std::vector<LayerDef::FeatureGeom> features;
    const bool binary_valid = loadBinaryHydrationCache(binary_path, sig, features);
    if (binary_valid) {
        return {
            {"mode", "warm-hydration-cache"},
            {"file", file},
            {"ok", true},
            {"source_signature", sig},
            {"source", "binary"},
            {"features", features.size()},
            {"binary_cache", binary_path.string()}
        };
    }

    features.clear();
    const bool msgpack_valid = loadHydrationCache(msgpack_path, sig, features);
    if (!msgpack_valid) {
        exit_code = 1;
        return {
            {"mode", "warm-hydration-cache"},
            {"file", file},
            {"ok", false},
            {"source_signature", sig},
            {"error", "no valid binary or legacy msgpack hydration cache"}
        };
    }

    saveBinaryHydrationCache(binary_path, sig, features);
    std::vector<LayerDef::FeatureGeom> verify;
    const bool verify_ok = loadBinaryHydrationCache(binary_path, sig, verify);
    const bool ok = verify_ok && verify.size() == features.size();
    if (!ok) exit_code = 1;
    return {
        {"mode", "warm-hydration-cache"},
        {"file", file},
        {"ok", ok},
        {"source_signature", sig},
        {"source", "msgpack"},
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
    const fs::path layers_dir = root / "data" / "layers";
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
        if (entry.path().extension() != ".geojson") continue;
        const fs::path msgpack_path = root / "data" / "cache" / "hydration" / (name + ".msgpack");
        const fs::path binary_path = root / "data" / "cache" / "hydration" / (name + ".bin");
        if (fs::exists(binary_path) || fs::exists(msgpack_path)) candidates.push_back(name);
    }
    std::sort(candidates.begin(), candidates.end());

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
    const fs::path layer_path = root / "data" / "layers" / file;
    const std::string sig = fileSignature(layer_path);
    const fs::path json_path = root / "data" / "cache" / "triangulation" / (file + ".tri.json");
    const fs::path binary_path = root / "data" / "cache" / "triangulation" / (file + ".tri.bin");

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

    if (!fs::exists(json_path)) {
        exit_code = 1;
        return {
            {"mode", "warm-triangulation-cache"},
            {"file", file},
            {"ok", false},
            {"source_signature", sig},
            {"error", "no valid binary or legacy json triangulation cache"}
        };
    }

    std::ifstream in(json_path);
    if (!in) {
        exit_code = 1;
        return {
            {"mode", "warm-triangulation-cache"},
            {"file", file},
            {"ok", false},
            {"source_signature", sig},
            {"error", "failed to open legacy json triangulation cache"}
        };
    }
    json j;
    try {
        in >> j;
    } catch (...) {
        exit_code = 1;
        return {
            {"mode", "warm-triangulation-cache"},
            {"file", file},
            {"ok", false},
            {"source_signature", sig},
            {"error", "legacy json triangulation cache parse failed"}
        };
    }
    if (!j.contains("signature") || !j.contains("triangles") ||
        j["signature"].get<std::string>() != sig || !j["triangles"].is_array()) {
        exit_code = 1;
        return {
            {"mode", "warm-triangulation-cache"},
            {"file", file},
            {"ok", false},
            {"source_signature", sig},
            {"error", "legacy json triangulation cache signature/count invalid"}
        };
    }
    tris = j["triangles"].get<std::vector<std::vector<uint32_t>>>();
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
        {"source", "json"},
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
        if (name.ends_with(".tri.json")) {
            candidates.push_back(name.substr(0, name.size() - std::strlen(".tri.json")));
        } else if (name.ends_with(".tri.bin")) {
            candidates.push_back(name.substr(0, name.size() - std::strlen(".tri.bin")));
        }
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

    json results = json::array();
    size_t ok_count = 0;
    size_t failed_count = 0;
    size_t skipped_count = 0;
    for (const std::string& file : candidates) {
        int one_exit = 0;
        json one = warmTriangulationCacheOne(root, file, one_exit);
        results.push_back(one);
        const std::string error = one.value("error", std::string());
        const bool stale_legacy_cache =
            error == "legacy json triangulation cache signature/count invalid";
        if (one_exit == 0) ok_count += 1;
        else if (stale_legacy_cache) skipped_count += 1;
        else failed_count += 1;
    }

    json out = {
        {"mode", "warm-triangulation-cache-all"},
        {"ok", failed_count == 0},
        {"candidate_count", candidates.size()},
        {"ok_count", ok_count},
        {"skipped_count", skipped_count},
        {"failed_count", failed_count},
        {"results", std::move(results)}
    };
    std::cout << out.dump(2) << '\n';
    return failed_count == 0 ? 0 : 1;
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
        if (arg == "--download-layers") {
            options.run_download_layers = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                options.download_phase = argv[++i];
            } else {
                options.download_phase = "all";
            }
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
        << "       worldsim3 [--download-layers [all|must-have|nice-to-have|heavy-data|capital-flows|extended-events|historical-high-quality|archival-research]] [--include-large]\n"
        << "       worldsim3 [--build-parcel-matched-layers|--force-build-parcel-matched-layers]\n"
        << "       worldsim3 --warm-hydration-cache LAYER_FILE\n"
        << "       worldsim3 --warm-hydration-cache-all\n"
        << "       worldsim3 --warm-triangulation-cache LAYER_FILE\n"
        << "       worldsim3 --warm-triangulation-cache-all\n"
        << "       worldsim3 --hydration-cache-selftest\n"
        << "       worldsim3 --triangulation-cache-selftest\n"
        << "       worldsim3 --projection-cache-selftest\n"
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
    if (options.run_download_layers) {
        return runLayerDownloadCli(
            root,
            options.download_phase.empty() ? "all" : options.download_phase,
            options.include_large_downloads);
    }
    if (options.run_build_parcel_matched_layers) {
        ensureParcelMatchedEventLayers(root, options.force_build_parcel_matched_layers, &std::cout);
        return 0;
    }
    return -1;
}
