#include "worldsim_cli.h"

#include "aggregate_debug.h"

#include "app_settings.h"
#include "app_utils.h"
#include "beps_screening.h"
#include "cache_io.h"
#include "crime_point_runtime_service.h"
#include "duckdb_analytics.h"
#include "derived_layer_caches.h"
#include "env_config.h"
#include "feature_props.h"
#include "heatmap_key_builder.h"
#include "heatmap_runtime.h"
#include "layer_import.h"
#include "layer_geometry.h"
#include "layer_registry.h"
#include "layer_state_io.h"
#include "layer_pipeline_drain.h"
#include "map_inspection.h"
#include "map_render_selection.h"
#include "map_render_projection.h"
#include "parcel_consolidation.h"
#include "population_metrics.h"
#include "selection.h"
#include "status_api.h"
#include "profiling_layer_snapshot.h"
#include "render_layer_pass.h"
#include "render_plan_builder.h"
#include "render_policy.h"
#include "render_routing.h"
#include "render_tile_cache.h"
#include "worldsim_dataset_bootstrap.h"
#include "worldsim_app.h"
#include "parcel_matched_layers.h"
#include "parcel_runtime_service.h"
#include "vacancy_overlay.h"

#include <duckdb.hpp>
#include <imgui_internal.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <deque>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
bool nearlyEqual(float a, float b) {
    return std::fabs(a - b) <= 0.00001f;
}

uint64_t memoryBytesFromEnvOrDefault(const char* env_name, uint64_t default_mb) {
    const char* raw = std::getenv(env_name);
    if (!raw || !*raw) return default_mb * 1024ull * 1024ull;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(raw, &end, 10);
    if (end == raw || parsed == 0ull) return default_mb * 1024ull * 1024ull;
    return parsed * 1024ull * 1024ull;
}

std::optional<uint64_t> readMemAvailableBytes() {
    std::ifstream in("/proc/meminfo");
    if (!in) return std::nullopt;
    std::string key;
    uint64_t value_kb = 0;
    std::string unit;
    while (in >> key >> value_kb >> unit) {
        if (key == "MemAvailable:") return value_kb * 1024ull;
    }
    return std::nullopt;
}

unsigned int memoryCappedWorkerCount(
    unsigned int cpu_worker_count,
    uint64_t bytes_per_job,
    uint64_t reserve_bytes,
    std::string* reason = nullptr) {
    if (reason) reason->clear();
    if (cpu_worker_count <= 1u) return 1u;
    const std::optional<uint64_t> mem_available = readMemAvailableBytes();
    if (!mem_available.has_value()) {
        if (reason) *reason = "mem_available_unavailable";
        return cpu_worker_count;
    }
    const uint64_t available = *mem_available;
    if (available <= reserve_bytes + bytes_per_job) {
        if (reason) {
            *reason = "mem_available=" + std::to_string(available / (1024ull * 1024ull)) +
                      "MB reserve=" + std::to_string(reserve_bytes / (1024ull * 1024ull)) +
                      "MB bytes_per_job=" + std::to_string(bytes_per_job / (1024ull * 1024ull)) + "MB";
        }
        return 1u;
    }
    const uint64_t usable = available - reserve_bytes;
    const uint64_t memory_workers = std::max<uint64_t>(1ull, usable / bytes_per_job);
    const unsigned int capped = static_cast<unsigned int>(std::min<uint64_t>(cpu_worker_count, memory_workers));
    if (reason) {
        *reason = "mem_available=" + std::to_string(available / (1024ull * 1024ull)) +
                  "MB reserve=" + std::to_string(reserve_bytes / (1024ull * 1024ull)) +
                  "MB bytes_per_job=" + std::to_string(bytes_per_job / (1024ull * 1024ull)) +
                  "MB memory_workers=" + std::to_string(memory_workers);
    }
    return std::max(1u, capped);
}

bool isBareLayerFilename(const std::string& file) {
    return !file.empty() &&
           file.find('/') == std::string::npos &&
           file.find('\\') == std::string::npos;
}

std::string cliSqlQuote(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const char c : value) {
        if (c == '\'') out += "''";
        else out.push_back(c);
    }
    return out;
}

bool isPrimaryParcelGeometryFileForCli(const std::string& file) {
    return file == "parcel.geojson" ||
           (file.size() > std::strlen("_county_parcels.geojson") &&
            file.ends_with("_county_parcels.geojson"));
}

json resetRebuildableArtifacts(const fs::path& root) {
    const std::vector<fs::path> targets = {
        root / "data" / "cache" / "geometry",
        root / "data" / "cache" / "render",
        root / "data" / "cache" / "render_tiles",
        root / "data" / "worldsim.duckdb",
        root / "data" / "worldsim.duckdb.wal",
        root / "data" / "worldsim.duckdb.source_signature"
    };

    json removed = json::array();
    json missing = json::array();
    json errors = json::array();
    bool ok = true;

    for (const fs::path& target : targets) {
        std::error_code ec;
        const bool exists = fs::exists(target, ec);
        if (ec) {
            ok = false;
            errors.push_back({
                {"path", target.string()},
                {"error", ec.message()}
            });
            continue;
        }
        if (!exists) {
            missing.push_back(target.string());
            continue;
        }
        const uintmax_t removed_count = fs::is_directory(target, ec)
            ? fs::remove_all(target, ec)
            : (fs::remove(target, ec) ? 1u : 0u);
        if (ec) {
            ok = false;
            errors.push_back({
                {"path", target.string()},
                {"error", ec.message()}
            });
            continue;
        }
        removed.push_back({
            {"path", target.string()},
            {"entries_removed", removed_count}
        });
    }

    return {
        {"ok", ok},
        {"removed", std::move(removed)},
        {"missing", std::move(missing)},
        {"errors", std::move(errors)}
    };
}

std::unordered_map<std::string, std::vector<std::string>> readDuckDbColumnsByTable(
    const fs::path& db_path,
    const std::vector<std::string>& table_names);
std::unordered_map<std::string, uint64_t> readDuckDbCountsByLayerFile(
    duckdb::Connection& con,
    const std::string& table_name,
    const std::string& layer_column,
    const std::string& count_expr);

LayerRenderRoute cliRenderRouteForLayer(
    const fs::path& root,
    const std::vector<LayerDef>& layers,
    const LayerDef& layer) {
    int layer_idx = -1;
    for (size_t i = 0; i < layers.size(); ++i) {
        if (&layers[i] == &layer || layers[i].file == layer.file) {
            layer_idx = static_cast<int>(i);
            break;
        }
    }
    LayerRegistry registry;
    registry.refresh(root, layers);
    return classifyLayerRenderRoute(
        layer_idx >= 0 ? static_cast<size_t>(layer_idx) : 0,
        layer,
        registry.indices().parcel_layer_idx);
}

std::vector<const char*> cliGeometryArtifactRouteNamesForLayer(
    const fs::path& root,
    const std::vector<LayerDef>& layers,
    const LayerDef& layer,
    GeometryArtifactClass cls) {
    if (cls == GeometryArtifactClass::Point) {
        return {layerRenderRouteArtifactName(LayerRenderRoute::PointGpu)};
    }
    if (cls == GeometryArtifactClass::Polyline) {
        return {layerRenderRouteArtifactName(LayerRenderRoute::PolylineGpu)};
    }
    if (cls == GeometryArtifactClass::Polygon && isOperationalParcelRenderLayer(layer)) {
        return {
            layerRenderRouteArtifactName(LayerRenderRoute::ParcelPolygonGpu),
            layerRenderRouteArtifactName(LayerRenderRoute::ParcelGpu)
        };
    }
    const LayerRenderRoute route = cliRenderRouteForLayer(root, layers, layer);
    return {layerRenderRouteArtifactName(route)};
}

bool loadExistingGeometryArtifactForLayer(
    const fs::path& root,
    const std::string& file,
    GeometryArtifactClass cls,
    const std::string& sig,
    json& stats_out,
    std::string_view render_path = {}) {
    const fs::path artifact_path = render_path.empty()
        ? geometryArtifactCachePathForLayerFile(root, file, cls)
        : geometryArtifactCachePathForLayerFile(root, file, cls, render_path);
    switch (cls) {
        case GeometryArtifactClass::Point: {
            PointGeometryArtifact artifact;
            if (!loadBinaryPointGeometryArtifact(artifact_path, sig, artifact)) return false;
            stats_out = {
                {"artifact_path", artifact_path.string()},
                {"feature_count", artifact.features.size()},
                {"vertex_count", artifact.positions.size()},
                {"chunk_count", artifact.chunks.size()}
            };
            return true;
        }
        case GeometryArtifactClass::Polyline: {
            PolylineGeometryArtifact artifact;
            if (!loadBinaryPolylineGeometryArtifact(artifact_path, sig, artifact)) return false;
            stats_out = {
                {"artifact_path", artifact_path.string()},
                {"feature_count", artifact.features.size()},
                {"vertex_count", artifact.vertices.size()},
                {"line_index_count", artifact.line_indices.size()},
                {"chunk_count", artifact.chunks.size()}
            };
            return true;
        }
        case GeometryArtifactClass::Polygon: {
            PolygonGeometryArtifact artifact;
            if (!loadBinaryPolygonGeometryArtifact(artifact_path, sig, artifact)) return false;
            stats_out = {
                {"artifact_path", artifact_path.string()},
                {"feature_count", artifact.features.size()},
                {"vertex_count", artifact.vertices.size()},
                {"fill_index_count", artifact.fill_indices.size()},
                {"line_index_count", artifact.line_indices.size()},
                {"chunk_count", artifact.chunks.size()}
            };
            return true;
        }
        case GeometryArtifactClass::Unknown:
            return false;
    }
    return false;
}

GeometryArtifactClass detectGeometryArtifactClass(
    const LayerDef& layer,
    const std::vector<LayerDef::FeatureRecord>& features) {
    bool saw_point = false;
    bool saw_polyline = false;
    bool saw_polygon = false;
    for (const auto& fg : features) {
        if (!fg.rings.empty()) saw_polygon = true;
        else if (!fg.paths.empty()) saw_polyline = true;
        else saw_point = true;
    }
    if (saw_polygon) return GeometryArtifactClass::Polygon;
    if (saw_polyline) return GeometryArtifactClass::Polyline;
    if (saw_point) return GeometryArtifactClass::Point;
    if (layerUsesPointGeometry(layer)) return GeometryArtifactClass::Point;
    if (layerUsesPolylineGeometry(layer)) return GeometryArtifactClass::Polyline;
    return GeometryArtifactClass::Polygon;
}

void emitCliProgress(
    const char* mode,
    const std::string& phase,
    const std::string& detail) {
    std::cerr << "[" << mode << "] " << phase;
    if (!detail.empty()) std::cerr << ": " << detail;
    std::cerr << std::endl;
}

std::string formatElapsedMs(double elapsed_ms) {
    const long long rounded_ms = static_cast<long long>(elapsed_ms + 0.5);
    return std::to_string(rounded_ms) + "ms";
}

void emitCanonicalLayerProgress(
    const char* mode,
    size_t index,
    size_t total,
    const std::string& layer_id,
    const std::string& status,
    const std::string& detail = {}) {
    std::ostringstream out;
    out << "[" << mode << "] "
        << "[" << index << "/" << total << "] "
        << layer_id << ": " << status;
    if (!detail.empty()) out << " " << detail;
    std::cerr << out.str() << std::endl;
}

std::string canonicalLayerSourceSummary(const LayerDef& layer) {
    if (!layer.source_url.empty()) return layer.source_url;
    if (!layer.import_url.empty()) return layer.import_url;
    if (!layer.import_service_url.empty()) return layer.import_service_url;
    if (!layer.source_urls.empty()) return layer.source_urls.front();
    if (!layer.import_type.empty()) return "import:" + layer.import_type;
    return "source:unknown";
}

json deprecatedRegionalParcelsAliasError(const char* mode, const std::string& file) {
    return {
        {"mode", mode},
        {"file", file},
        {"ok", false},
        {"error", "regional_parcels has been removed; use direct parcel layer ids such as parcel.geojson"}
    };
}

struct LocalLayerLoadFailure {
    size_t layer_index = 0;
    std::string layer_file;
    std::string error;
};

struct LocalLayerLoadSummary {
    size_t local_layer_count = 0;
    size_t requested_layer_count = 0;
    size_t loaded_layer_count = 0;
    size_t failed_layer_count = 0;
    size_t skipped_missing_layer_count = 0;
    size_t total_feature_count = 0;
    double elapsed_ms = 0.0;
    std::vector<size_t> requested_indices;
    std::vector<LocalLayerLoadFailure> failures;
};

bool loadLocalLayerFeatures(
    const fs::path& root,
    const LayerDef& layer,
    std::vector<LayerDef::FeatureRecord>& features,
    std::vector<LayerDef::FeatureProperties>* feature_properties,
    std::string& source_used,
    std::string& error) {
    const fs::path layer_path = resolveStoredLayerPath(root, layer);
    std::string sig;
    if (resolveLayerSourceSignature(layer_path, sig, nullptr) &&
        loadCanonicalLayerFeatureCollection(root, layer.file, sig, features, feature_properties)) {
        source_used = "canonical_binary";
        return true;
    }
    if (layer.import_type == "census_acs_tract_demographics") {
        std::vector<LayerDef::FeatureProperties> loaded_feature_properties;
        if (loadLayerFeaturesFromLocalSsotSource(
                root,
                layer,
                features,
                loaded_feature_properties,
                source_used,
                error)) {
            if (feature_properties) *feature_properties = std::move(loaded_feature_properties);
            return true;
        }
    }
    std::vector<LayerDef::FeatureProperties> loaded_feature_properties;
    if (loadLayerFeaturesFromLocalImportArtifact(
            root,
            layer,
            features,
            loaded_feature_properties,
            source_used,
            error)) {
        if (feature_properties) *feature_properties = std::move(loaded_feature_properties);
        return true;
    }
    if (error.empty()) error = "no readable canonical layer binary or local import artifact";
    return false;
}

bool loadLocalLayerFeatures(
    const fs::path& root,
    const std::string& file,
    const std::string&,
    std::vector<LayerDef::FeatureRecord>& features,
    std::vector<LayerDef::FeatureProperties>* feature_properties,
    std::string& source_used,
    std::string& error) {
    LayerDef layer;
    layer.file = file;
    return loadLocalLayerFeatures(root, layer, features, feature_properties, source_used, error);
}

bool layerHasLocalAnalyticsSource(const fs::path& root, const LayerDef& layer) {
    if (layerRuntimeSourceMaterialized(root, layer)) return true;
    return layerHasLocalImportArtifact(root, layer);
}

bool loadDuckDbLayerFeatures(
    const fs::path& root,
    const LayerDef& layer,
    std::vector<LayerDef::FeatureRecord>& features,
    std::vector<LayerDef::FeatureProperties>* feature_properties,
    std::string& source_used,
    std::string& error) {
    std::vector<LayerDef::FeatureProperties> loaded_feature_properties;
    if (!loadLayerFeaturesFromLocalSsotSource(
            root, layer, features, loaded_feature_properties, source_used, error)) {
        return false;
    }
    if (feature_properties) *feature_properties = std::move(loaded_feature_properties);
    return true;
}

bool loadLocalLayersForCli(
    const fs::path& root,
    std::vector<LayerDef>& layers,
    bool verbose,
    LocalLayerLoadSummary& summary,
    bool ssot_only = false,
    const std::vector<size_t>* requested_indices = nullptr) {
    summary = {};
    std::vector<size_t> local_indices;
    local_indices.reserve(requested_indices ? requested_indices->size() : layers.size());
    const auto consider_index = [&](size_t i) {
        if (i >= layers.size()) return;
        const bool exists = ssot_only
            ? layerHasLocalSsotSource(root, layers[i])
            : layerHasLocalAnalyticsSource(root, layers[i]);
        if (!exists) {
            summary.skipped_missing_layer_count += 1;
            return;
        }
        summary.local_layer_count += 1;
        local_indices.push_back(i);
    };
    if (requested_indices) {
        for (size_t i : *requested_indices) consider_index(i);
    } else {
        for (size_t i = 0; i < layers.size(); ++i) consider_index(i);
    }
    summary.requested_indices = local_indices;
    summary.requested_layer_count = local_indices.size();
    const auto started_at = std::chrono::steady_clock::now();
    for (size_t idx : local_indices) {
        std::vector<LayerDef::FeatureRecord> features;
        std::vector<LayerDef::FeatureProperties> feature_properties;
        std::string source_used;
        std::string error;
        const bool loaded = ssot_only
            ? loadDuckDbLayerFeatures(root, layers[idx], features, &feature_properties, source_used, error)
            : loadLocalLayerFeatures(root, layers[idx], features, &feature_properties, source_used, error);
        if (!loaded) {
            summary.failed_layer_count += 1;
            summary.failures.push_back({idx, layers[idx].file, error.empty() ? "failed to load source features" : error});
            if (verbose) {
                std::cerr << "  failed " << layers[idx].file << ": "
                          << (error.empty() ? "failed to load source features" : error) << '\n';
            }
            continue;
        }
        layers[idx].features = std::move(features);
        layers[idx].feature_properties = std::move(feature_properties);
        refreshLayerGeometryUsageCache(layers[idx]);
        rebuildFeaturePropertyRegistryForLayer(layers[idx]);
        summary.loaded_layer_count += 1;
        summary.total_feature_count += layers[idx].features.size();
        if (verbose) {
            std::cerr << "  [" << summary.loaded_layer_count + summary.failed_layer_count
                      << "/" << summary.requested_layer_count << "] "
                      << layers[idx].file << " -> " << layers[idx].features.size()
                      << " features via " << source_used << '\n';
        }
    }
    summary.elapsed_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started_at).count();
    return summary.failed_layer_count == 0;
}

std::vector<size_t> parcelConsolidationInputLayerIndices(const WorldsimLayerIndices& indices) {
    std::vector<size_t> out;
    out.reserve(6);
    auto append = [&](int layer_idx) {
        if (layer_idx < 0) return;
        const size_t idx = (size_t)layer_idx;
        if (std::find(out.begin(), out.end(), idx) == out.end()) out.push_back(idx);
    };
    append(indices.parcel_layer_idx);
    append(indices.real_property_layer_idx);
    append(indices.vacant_notice_layer_idx);
    append(indices.vacant_rehab_layer_idx);
    append(indices.tax_lien_layer_idx);
    append(indices.tax_sale_layer_idx);
    std::sort(out.begin(), out.end());
    return out;
}

void releaseLoadedLayerFeatures(
    std::vector<LayerDef>& layers,
    const std::unordered_set<size_t>* keep_indices = nullptr) {
    for (size_t i = 0; i < layers.size(); ++i) {
        if (keep_indices && keep_indices->contains(i)) continue;
        layers[i].features.clear();
        layers[i].feature_properties.clear();
        layers[i].features.shrink_to_fit();
        layers[i].feature_properties.shrink_to_fit();
        refreshLayerGeometryUsageCache(layers[i]);
        rebuildFeaturePropertyRegistryForLayer(layers[i]);
    }
}

LayerDef layerFromManifestItemForCli(const json& item) {
    LayerDef layer;
    layer.name = item.value("name", std::string("unnamed"));
    layer.logical_id = item.value("id", defaultLayerLogicalIdForFile(item.value("file", std::string())));
    layer.file = item.value("file", std::string());
    if (item.contains("url") && item["url"].is_string()) layer.source_url = item["url"].get<std::string>();
    if (item.contains("import") && item["import"].is_object()) {
        const auto& import = item["import"];
        layer.import_type = import.value("type", std::string());
        layer.import_url = import.value("url", std::string());
        layer.import_source_crs = import.value("source_crs", std::string());
        layer.import_shapefile = import.value("shapefile", std::string());
        layer.import_service_url = import.value("service_url", std::string());
        layer.import_normalizer = import.value("normalizer", std::string());
        layer.import_sheet_name = import.value("sheet_name", std::string());
        layer.import_lon_field = import.value("lon_field", std::string());
        layer.import_lat_field = import.value("lat_field", std::string());
        layer.import_artifact_file = import.value("artifact_file", std::string());
        layer.import_item_path = import.value("item_path", std::string());
        layer.import_table = import.value("table", std::string());
        layer.import_year = import.value("year", std::string());
        layer.import_survey = import.value("survey", std::string());
        layer.import_where = import.value("where", std::string());
        layer.import_query = import.value("query", std::string());
    }
    if (item.contains("provenance") && item["provenance"].is_object()) {
        const auto& provenance = item["provenance"];
        layer.provenance_world = provenance.value("world", std::string());
        layer.provenance_nation_state = provenance.value("nation_state", std::string());
        layer.provenance_state_region = provenance.value("state_region", std::string());
        layer.provenance_county_city = provenance.value("county_city", std::string());
    }
    return layer;
}

const LayerDef* findManifestLayerByIdentifier(const std::vector<LayerDef>& layers, const std::string& key) {
    for (const auto& candidate : layers) {
        if (layerMatchesIdentifier(candidate, key)) return &candidate;
    }
    return nullptr;
}

bool deprecatedRegionalParcelsAlias(const std::string& key) {
    return key == "regional_parcels.geojson" || key == "regional_parcels";
}

std::string resolveLayerStorageKey(const fs::path& root, const std::string& key) {
    std::vector<LayerDef> layers = loadManifest(root);
    if (const LayerDef* layer = findManifestLayerByIdentifier(layers, key)) return layer->file;
    return key;
}

int generateCanonicalFilesCli(const fs::path& root, const std::string& phase, bool include_large, int reserve_cores) {
    constexpr const char* kMode = "generate-canonical-files";
    const auto started_at = std::chrono::steady_clock::now();
    const std::string selected_phase = phase.empty() ? "all" : phase;
    const fs::path manifest_path = layerManifestPathForPhase(root, selected_phase);
    std::ifstream in(manifest_path);
    if (!in) {
        std::cerr << "manifest not found: " << manifest_path << "\n";
        return 1;
    }

    json items_json;
    try {
        in >> items_json;
    } catch (const std::exception& e) {
        std::cerr << "manifest parse failed: " << e.what() << "\n";
        return 1;
    }
    if (!items_json.is_array()) {
        std::cerr << "manifest is not an array: " << manifest_path << "\n";
        return 1;
    }

    struct CanonicalJob {
        size_t index = 0;
        LayerDef layer;
        bool is_large = false;
    };
    struct CanonicalJobResult {
        enum class Status {
            Generated,
            Skipped,
            Failed
        };
        Status status = Status::Skipped;
        std::string file;
        std::string layer_id;
        std::string message;
        fs::path canonical_path;
        uint64_t feature_count = 0;
    };

    std::vector<CanonicalJob> jobs;
    jobs.reserve(items_json.size());
    size_t considered = 0;
    for (const auto& item : items_json) {
        if (!item.is_object()) continue;
        LayerDef layer = layerFromManifestItemForCli(item);
        if (layer.file.empty()) continue;
        CanonicalJob job;
        job.index = ++considered;
        job.layer = std::move(layer);
        job.is_large = item.value("large", false);
        jobs.push_back(std::move(job));
    }

    const unsigned int hw = std::max(1u, std::thread::hardware_concurrency());
    const unsigned int cpu_worker_count = std::max(
        1u,
        hw > (unsigned int)std::max(0, reserve_cores) ? hw - (unsigned int)std::max(0, reserve_cores) : 1u);
    const uint64_t bytes_per_job = memoryBytesFromEnvOrDefault("WORLDSIM_CANONICAL_JOB_MEMORY_MB", 1536ull);
    const uint64_t reserve_bytes = memoryBytesFromEnvOrDefault("WORLDSIM_CANONICAL_MEMORY_HEADROOM_MB", 2048ull);
    std::string memory_backpressure_reason;
    const unsigned int worker_count =
        memoryCappedWorkerCount(cpu_worker_count, bytes_per_job, reserve_bytes, &memory_backpressure_reason);

    emitCliProgress(
        kMode,
        "start",
        "phase=" + selected_phase +
            " include_large=" + std::string(include_large ? "true" : "false") +
            " manifest_path=" + manifest_path.string() +
            " manifest_items=" + std::to_string(items_json.size()) +
            " jobs=" + std::to_string(jobs.size()) +
            " cpu_worker_count=" + std::to_string(cpu_worker_count) +
            " worker_count=" + std::to_string(worker_count) +
            " reserve_cores=" + std::to_string(std::max(0, reserve_cores)) +
            " memory_backpressure=" + memory_backpressure_reason);

    size_t generated = 0;
    size_t skipped = 0;
    size_t failed = 0;
    std::vector<std::string> failures;
    std::vector<CanonicalJobResult> results(jobs.size());
    size_t next_job = 0;
    std::mutex work_mutex;
    std::mutex result_mutex;
    std::mutex progress_mutex;

    auto run_job = [&](const CanonicalJob& job) -> CanonicalJobResult {
        CanonicalJobResult result;
        result.file = job.layer.file;
        result.layer_id = layerLogicalId(job.layer);

        {
            std::lock_guard<std::mutex> lock(progress_mutex);
            emitCanonicalLayerProgress(kMode, job.index, jobs.size(), result.layer_id, "waiting");
        }

        if (job.is_large && !include_large) {
            result.status = CanonicalJobResult::Status::Skipped;
            result.message = "large-source rerun_with=--include-large";
            std::lock_guard<std::mutex> lock(progress_mutex);
            emitCanonicalLayerProgress(kMode, job.index, jobs.size(), result.layer_id, "skipped", result.message);
            return result;
        }

        const fs::path canonical_path = canonicalLayerPathForFile(root, job.layer.file);
        result.canonical_path = canonical_path;

        {
            std::lock_guard<std::mutex> lock(progress_mutex);
            emitCanonicalLayerProgress(kMode, job.index, jobs.size(), result.layer_id, "checking");
        }

        if (!fs::exists(canonical_path)) {
            if (!job.layer.source_url.empty() || layerHasImportSource(job.layer)) {
                const fs::path deprecated_layer_path = resolveStoredLayerPath(root, job.layer);
                {
                    std::lock_guard<std::mutex> lock(progress_mutex);
                    emitCanonicalLayerProgress(
                        kMode,
                        job.index,
                        jobs.size(),
                        result.layer_id,
                        "pulling fs layer",
                        "from=" + canonicalLayerSourceSummary(job.layer));
                }
                const VersionedDownloadResult res = downloadOrImportLayer(job.layer, deprecated_layer_path, root);
                if (!res.ok) {
                    result.status = CanonicalJobResult::Status::Failed;
                    result.message = res.message;
                    std::lock_guard<std::mutex> lock(progress_mutex);
                    emitCanonicalLayerProgress(kMode, job.index, jobs.size(), result.layer_id, "error", res.message);
                    return result;
                }
                {
                    std::lock_guard<std::mutex> lock(progress_mutex);
                    emitCanonicalLayerProgress(
                        kMode,
                        job.index,
                        jobs.size(),
                        result.layer_id,
                        "download complete",
                        res.message);
                }
            }
        }

        if (!fs::exists(canonical_path)) {
            result.status = CanonicalJobResult::Status::Skipped;
            result.message = "no canonical artifact materialized";
            std::lock_guard<std::mutex> lock(progress_mutex);
            emitCanonicalLayerProgress(kMode, job.index, jobs.size(), result.layer_id, "skipped", result.message);
            return result;
        }

        {
            std::lock_guard<std::mutex> lock(progress_mutex);
            emitCanonicalLayerProgress(kMode, job.index, jobs.size(), result.layer_id, "verifying");
        }

        CanonicalFeatureCollectionMetadata meta;
        if (!loadBinaryCanonicalMetadata(canonical_path, meta) ||
            meta.source_signature.empty() ||
            meta.feature_count == 0) {
            result.status = CanonicalJobResult::Status::Failed;
            result.message = "canonical metadata verification failed";
            std::lock_guard<std::mutex> lock(progress_mutex);
            emitCanonicalLayerProgress(kMode, job.index, jobs.size(), result.layer_id, "error", result.message);
            return result;
        }

        if (meta.version != kCanonicalFeatureBinaryVersion) {
            std::vector<LayerDef::FeatureRecord> features;
            std::vector<LayerDef::FeatureProperties> feature_properties;
            if (!loadBinaryCanonicalFeatureCollection(canonical_path, meta.source_signature, features, &feature_properties)) {
                result.status = CanonicalJobResult::Status::Failed;
                result.message = "canonical rewrite load failed";
                std::lock_guard<std::mutex> lock(progress_mutex);
                emitCanonicalLayerProgress(kMode, job.index, jobs.size(), result.layer_id, "error", result.message);
                return result;
            }
            saveBinaryCanonicalFeatureCollection(canonical_path, meta.source_signature, features, &feature_properties);
            if (!loadBinaryCanonicalMetadata(canonical_path, meta) ||
                meta.version != kCanonicalFeatureBinaryVersion ||
                meta.source_signature.empty() ||
                meta.feature_count == 0) {
                result.status = CanonicalJobResult::Status::Failed;
                result.message = "canonical rewrite verification failed";
                std::lock_guard<std::mutex> lock(progress_mutex);
                emitCanonicalLayerProgress(kMode, job.index, jobs.size(), result.layer_id, "error", result.message);
                return result;
            }
        }

        result.status = CanonicalJobResult::Status::Generated;
        result.feature_count = meta.feature_count;
        result.message = canonical_path.string();
        {
            std::lock_guard<std::mutex> lock(progress_mutex);
            emitCanonicalLayerProgress(
                kMode,
                job.index,
                jobs.size(),
                result.layer_id,
                "done",
                "features=" + std::to_string(meta.feature_count));
        }
        return result;
    };

    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (unsigned int worker_idx = 0; worker_idx < worker_count; ++worker_idx) {
        workers.emplace_back([&]() {
            for (;;) {
                CanonicalJob job;
                {
                    std::lock_guard<std::mutex> lock(work_mutex);
                    if (next_job >= jobs.size()) break;
                    job = jobs[next_job++];
                }
                CanonicalJobResult result = run_job(job);
                {
                    std::lock_guard<std::mutex> lock(result_mutex);
                    results[job.index - 1] = std::move(result);
                }
            }
        });
    }
    for (auto& worker : workers) {
        if (worker.joinable()) worker.join();
    }

    for (const auto& result : results) {
        switch (result.status) {
            case CanonicalJobResult::Status::Generated:
                generated++;
                std::cout << "done " << result.layer_id << " -> " << result.canonical_path.string()
                          << " (" << result.feature_count << " features)\n";
                break;
            case CanonicalJobResult::Status::Skipped:
                skipped++;
                std::cout << "skip " << result.layer_id << " (" << result.message << ")\n";
                break;
            case CanonicalJobResult::Status::Failed:
                failed++;
                failures.push_back(result.layer_id + ": " + result.message);
                std::cout << "fail " << result.layer_id << " (" << result.message << ")\n";
                break;
        }
    }

    const double elapsed_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started_at).count();
    emitCliProgress(
        kMode,
        "complete",
        "ok=" + std::string(failed == 0 ? "true" : "false") +
            " considered=" + std::to_string(considered) +
            " generated=" + std::to_string(generated) +
            " skipped=" + std::to_string(skipped) +
            " failed=" + std::to_string(failed) +
            " worker_count=" + std::to_string(worker_count) +
            " elapsed=" + formatElapsedMs(elapsed_ms));

    json out = {
        {"mode", "generate-canonical-files"},
        {"phase", selected_phase},
        {"manifest_path", manifest_path.string()},
        {"cpu_worker_count", cpu_worker_count},
        {"worker_count", worker_count},
        {"memory_backpressure", memory_backpressure_reason},
        {"considered", considered},
        {"generated", generated},
        {"skipped", skipped},
        {"failed", failed},
        {"elapsed_ms", elapsed_ms},
        {"ok", failed == 0}
    };
    if (!failures.empty()) out["failures"] = failures;
    std::cout << out.dump(2) << '\n';
    return failed == 0 ? 0 : 1;
}

LayerDef::FeatureRecord makePopulationSelftestSquare(float min_lon, float min_lat, float max_lon, float max_lat) {
    LayerDef::FeatureRecord feature;
    feature.extent.min_lon = min_lon;
    feature.extent.min_lat = min_lat;
    feature.extent.max_lon = max_lon;
    feature.extent.max_lat = max_lat;
    feature.rings.push_back({
        ImVec2(min_lon, min_lat),
        ImVec2(max_lon, min_lat),
        ImVec2(max_lon, max_lat),
        ImVec2(min_lon, max_lat),
        ImVec2(min_lon, min_lat)
    });
    return feature;
}

LayerDef::FeatureRecord makePopulationSelftestPoint(float lon, float lat) {
    LayerDef::FeatureRecord feature;
    feature.extent.min_lon = lon;
    feature.extent.max_lon = lon;
    feature.extent.min_lat = lat;
    feature.extent.max_lat = lat;
    return feature;
}

int runPopulationMetricsSelftest() {
    LayerDef tracts;
    tracts.name = "Population Tracts Selftest";
    tracts.file = "population_tracts_selftest.geojson";
    tracts.features = {
        makePopulationSelftestSquare(0.0f, 0.0f, 1.0f, 1.0f),
        makePopulationSelftestSquare(1.0f, 0.0f, 2.0f, 1.0f)
    };
    tracts.feature_properties = {
        {{{"total_population", "1,000"}}},
        {{{"B01003_001E", "500"}}}
    };

    LayerDef crimes;
    crimes.name = "Crime Points Selftest";
    crimes.file = "crime_points_selftest.geojson";
    crimes.features = {
        makePopulationSelftestPoint(0.25f, 0.25f),
        makePopulationSelftestPoint(0.75f, 0.75f),
        makePopulationSelftestPoint(1.50f, 0.50f),
        makePopulationSelftestPoint(3.00f, 3.00f)
    };

    PopulationMetricsSummary summary;
    std::vector<PopulationTractMetric> metrics =
        buildTractPopulationCrimeMetrics(tracts, crimes, &summary);
    applyPopulationMetricsToLayer(tracts, metrics);

    const bool counts_ok =
        metrics.size() == 2 &&
        metrics[0].crime_count == 2 &&
        metrics[1].crime_count == 1 &&
        summary.assigned_crime_points == 3 &&
        summary.total_crimes == 4 &&
        summary.valid_population_tracts == 2;
    const bool rates_ok =
        std::fabs(metrics[0].crime_per_1000_residents - 2.0) <= 0.00001 &&
        std::fabs(metrics[1].crime_per_1000_residents - 2.0) <= 0.00001 &&
        metrics[0].population_density_per_sq_km > 0.0 &&
        metrics[1].population_density_per_sq_km > 0.0;
    const bool properties_ok =
        getPropertyValue(tracts, 0, "population_algorithm") == "tract_population_density_and_crime_rate" &&
        getPropertyValue(tracts, 0, "crime_count") == "2" &&
        getPropertyValue(tracts, 1, "crime_count") == "1";

    json out = {
        {"mode", "population-metrics-selftest"},
        {"ok", counts_ok && rates_ok && properties_ok},
        {"tract_count", summary.tract_count},
        {"valid_population_tracts", summary.valid_population_tracts},
        {"crime_point_count", summary.crime_point_count},
        {"assigned_crime_points", summary.assigned_crime_points},
        {"total_population", summary.total_population},
        {"total_crimes", summary.total_crimes},
        {"tract_0_crime_per_1000", metrics.empty() ? 0.0 : metrics[0].crime_per_1000_residents},
        {"tract_1_crime_per_1000", metrics.size() < 2 ? 0.0 : metrics[1].crime_per_1000_residents},
        {"counts_ok", counts_ok},
        {"rates_ok", rates_ok},
        {"properties_ok", properties_ok}
    };
    std::cout << out.dump(2) << '\n';
    return out["ok"].get<bool>() ? 0 : 1;
}

json featurePropertiesToJson(const LayerDef::FeatureProperties& properties) {
    json out = json::object();
    for (const auto& kv : properties.values) out[kv.first] = kv.second;
    return out;
}

json polygonFeatureGeometryToJson(const LayerDef::FeatureRecord& feature) {
    json rings = json::array();
    for (const auto& ring : feature.rings) {
        json coords = json::array();
        for (const ImVec2& p : ring) coords.push_back({p.x, p.y});
        rings.push_back(std::move(coords));
    }
    return {
        {"type", "Polygon"},
        {"coordinates", std::move(rings)}
    };
}

json featureGeometryToJson(const LayerDef::FeatureRecord& feature) {
    if (!feature.rings.empty()) return polygonFeatureGeometryToJson(feature);
    return {
        {"type", "Point"},
        {"coordinates", {feature.extent.min_lon, feature.extent.min_lat}}
    };
}

LayerDef::FeatureProperties makeProperties(std::initializer_list<std::pair<std::string, std::string>> values) {
    LayerDef::FeatureProperties props;
    props.values.assign(values.begin(), values.end());
    return props;
}

double parseNumberForCli(std::string value) {
    value.erase(std::remove(value.begin(), value.end(), ','), value.end());
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c) {
        return std::isspace(c);
    }), value.end());
    if (value.empty()) return 0.0;
    char* end = nullptr;
    const double parsed = std::strtod(value.c_str(), &end);
    if (end == value.c_str() || !std::isfinite(parsed)) return 0.0;
    return parsed;
}

bool loadLatestSnapshotGeoJsonLayer(
    const fs::path& root,
    const std::string& file,
    std::vector<LayerDef::FeatureRecord>& features,
    std::vector<LayerDef::FeatureProperties>& feature_properties,
    std::string& source_used,
    std::string& error) {
    const fs::path snapshot_dir = root / "data" / "versions" / "snapshots" / file;
    std::error_code ec;
    if (!fs::exists(snapshot_dir, ec) || ec) {
        error = "snapshot directory not found";
        return false;
    }
    fs::path latest;
    for (const auto& entry : fs::directory_iterator(snapshot_dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        const fs::path path = entry.path();
        if (path.extension() != ".geojson") continue;
        if (latest.empty() || path.filename().string() > latest.filename().string()) latest = path;
    }
    if (latest.empty()) {
        error = "no snapshot geojson found";
        return false;
    }
    try {
        features = loadLayerPointsFromFile(latest, &feature_properties);
        source_used = "snapshot_geojson:" + latest.filename().string();
        return !features.empty();
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

int runBepsScreeningSelftest() {
    LayerDef properties;
    properties.name = "BEPS Property Selftest";
    properties.file = "beps_properties_selftest.geojson";
    properties.features = {
        makePopulationSelftestPoint(-76.61f, 39.29f),
        makePopulationSelftestPoint(-76.62f, 39.30f),
        makePopulationSelftestPoint(-76.63f, 39.31f),
        makePopulationSelftestPoint(-76.64f, 39.32f)
    };
    properties.feature_properties = {
        makeProperties({{"BLOCKLOT", "0001 001"}, {"FULLADDR", "1 MARKET ST"}, {"OWNER_1", "MARKET LLC"}, {"USEGROUP", "C"}, {"STRUCTAREA", "40000"}, {"DWELUNIT", "0"}}),
        makeProperties({{"BLOCKLOT", "0001 002"}, {"FULLADDR", "2 APARTMENT ST"}, {"OWNER_1", "APARTMENTS LLC"}, {"USEGROUP", "R"}, {"STRUCTAREA", "50000"}, {"DWELUNIT", "24"}}),
        makeProperties({{"BLOCKLOT", "0001 003"}, {"FULLADDR", "3 SMALL ST"}, {"OWNER_1", "SMALL LLC"}, {"USEGROUP", "C"}, {"STRUCTAREA", "34000"}, {"DWELUNIT", "0"}}),
        makeProperties({{"BLOCKLOT", "0001 004"}, {"FULLADDR", "4 FEDERAL ST"}, {"OWNER_1", "UNITED STATES OF AMERICA"}, {"USEGROUP", "C"}, {"STRUCTAREA", "90000"}, {"DWELUNIT", "0"}})
    };

    LayerDef parcels;
    parcels.name = "BEPS Parcel Selftest";
    parcels.file = "beps_parcels_selftest.geojson";
    parcels.features = {
        makePopulationSelftestSquare(-76.611f, 39.289f, -76.609f, 39.291f),
        makePopulationSelftestSquare(-76.621f, 39.299f, -76.619f, 39.301f)
    };
    parcels.feature_properties = {
        makeProperties({{"BLOCKLOT", "0001 001"}}),
        makeProperties({{"BLOCKLOT", "0001 002"}})
    };

    BepsScreeningSummary summary;
    std::vector<BepsCandidate> candidates = screenMarylandBepsCandidates(properties, &parcels, &summary);
    LayerDef output;
    output.features.resize(candidates.size());
    applyBepsCandidateProperties(output, candidates);

    const bool count_ok =
        candidates.size() == 2 &&
        summary.candidate_count == 2 &&
        summary.matched_parcel_count == 2 &&
        summary.high_confidence_count == 2;
    const bool candidate_ok =
        candidates[0].commercial_like &&
        candidates[1].multifamily_like &&
        candidates[0].gross_floor_area_sqft == 40000.0 &&
        candidates[1].dwelling_units == 24;
    const bool exclusions_ok =
        std::none_of(candidates.begin(), candidates.end(), [](const BepsCandidate& candidate) {
            return candidate.address == "3 SMALL ST" || candidate.address == "4 FEDERAL ST";
        });
    const bool properties_ok =
        output.feature_properties.size() == 2 &&
        getPropertyValue(output, 0, "beps_might_be_covered") == "true" &&
        getPropertyValue(output, 1, "beps_confidence") == "high";

    json out = {
        {"mode", "beps-screening-selftest"},
        {"ok", count_ok && candidate_ok && exclusions_ok && properties_ok},
        {"candidate_count", summary.candidate_count},
        {"matched_parcel_count", summary.matched_parcel_count},
        {"high_confidence_count", summary.high_confidence_count},
        {"count_ok", count_ok},
        {"candidate_ok", candidate_ok},
        {"exclusions_ok", exclusions_ok},
        {"properties_ok", properties_ok}
    };
    std::cout << out.dump(2) << '\n';
    return out["ok"].get<bool>() ? 0 : 1;
}

int runBuildPopulationMetricsCli(const fs::path& root) {
    constexpr const char* kPopulationLayerFile = "cdc_places_total_population_baltimore_tracts.geojson";
    constexpr const char* kCrimeLayerFile = "crime_nibrs_group_a_2022_present.geojson";

    std::vector<LayerDef> manifest_layers = loadManifest(root);
    const LayerDef* population_manifest_layer = findManifestLayerByIdentifier(manifest_layers, kPopulationLayerFile);
    const LayerDef* crime_manifest_layer = findManifestLayerByIdentifier(manifest_layers, kCrimeLayerFile);
    if (!population_manifest_layer || !crime_manifest_layer) {
        json out = {
            {"mode", "build-population-metrics"},
            {"ok", false},
            {"error", "required population or crime layer is missing from manifest"},
            {"population_layer", kPopulationLayerFile},
            {"crime_layer", kCrimeLayerFile}
        };
        std::cout << out.dump(2) << '\n';
        return 1;
    }

    LayerDef population_layer = *population_manifest_layer;
    LayerDef crime_layer = *crime_manifest_layer;
    std::string population_source;
    std::string crime_source;
    std::string population_error;
    std::string crime_error;
    if (!loadLocalLayerFeatures(
            root,
            population_layer,
            population_layer.features,
            &population_layer.feature_properties,
            population_source,
            population_error) ||
        !loadLocalLayerFeatures(
            root,
            crime_layer,
            crime_layer.features,
            &crime_layer.feature_properties,
            crime_source,
            crime_error)) {
        json out = {
            {"mode", "build-population-metrics"},
            {"ok", false},
            {"population_source", population_source},
            {"crime_source", crime_source},
            {"population_error", population_error},
            {"crime_error", crime_error}
        };
        std::cout << out.dump(2) << '\n';
        return 1;
    }

    PopulationMetricsSummary summary;
    std::vector<PopulationTractMetric> metrics =
        buildTractPopulationCrimeMetrics(population_layer, crime_layer, &summary);
    applyPopulationMetricsToLayer(population_layer, metrics);

    json features = json::array();
    for (size_t i = 0; i < population_layer.features.size(); ++i) {
        if (population_layer.features[i].rings.empty()) continue;
        features.push_back({
            {"type", "Feature"},
            {"properties", i < population_layer.feature_properties.size()
                ? featurePropertiesToJson(population_layer.feature_properties[i])
                : json::object()},
            {"geometry", polygonFeatureGeometryToJson(population_layer.features[i])}
        });
    }

    json collection = {
        {"type", "FeatureCollection"},
        {"name", "population_metrics_baltimore_tracts"},
        {"properties", {
            {"algorithm", "tract_population_density_and_crime_rate"},
            {"population_layer", kPopulationLayerFile},
            {"crime_layer", kCrimeLayerFile},
            {"population_source", population_source},
            {"crime_source", crime_source},
            {"tract_count", summary.tract_count},
            {"valid_population_tracts", summary.valid_population_tracts},
            {"crime_point_count", summary.crime_point_count},
            {"assigned_crime_points", summary.assigned_crime_points},
            {"total_population", summary.total_population},
            {"total_crimes", summary.total_crimes}
        }},
        {"features", std::move(features)}
    };

    const fs::path output_path = root / "data" / "derived" / "population_metrics_baltimore_tracts.geojson";
    std::error_code ec;
    fs::create_directories(output_path.parent_path(), ec);
    if (ec) {
        json out = {
            {"mode", "build-population-metrics"},
            {"ok", false},
            {"error", ec.message()},
            {"output_path", output_path.string()}
        };
        std::cout << out.dump(2) << '\n';
        return 1;
    }
    std::ofstream out_file(output_path);
    if (!out_file) {
        json out = {
            {"mode", "build-population-metrics"},
            {"ok", false},
            {"error", "failed to open output file"},
            {"output_path", output_path.string()}
        };
        std::cout << out.dump(2) << '\n';
        return 1;
    }
    out_file << collection.dump(2) << '\n';

    json out = {
        {"mode", "build-population-metrics"},
        {"ok", true},
        {"output_path", output_path.string()},
        {"population_source", population_source},
        {"crime_source", crime_source},
        {"tract_count", summary.tract_count},
        {"valid_population_tracts", summary.valid_population_tracts},
        {"crime_point_count", summary.crime_point_count},
        {"assigned_crime_points", summary.assigned_crime_points},
        {"total_population", summary.total_population},
        {"total_crimes", summary.total_crimes}
    };
    std::cout << out.dump(2) << '\n';
    return 0;
}

int runBuildBepsCandidatesCli(const fs::path& root) {
    constexpr const char* kOfficialBepsLayerFile = "maryland_beps_covered_buildings.geojson";
    constexpr const char* kPropertyLayerFile = "real_property_information.geojson";
    constexpr const char* kParcelLayerFile = "parcel.geojson";

    std::vector<LayerDef> manifest_layers = loadManifest(root);
    const LayerDef* official_manifest_layer = findManifestLayerByIdentifier(manifest_layers, kOfficialBepsLayerFile);
    if (official_manifest_layer) {
        LayerDef official_layer = *official_manifest_layer;
        std::string official_source;
        std::string official_error;
        if (loadLocalLayerFeatures(
                root,
                official_layer,
                official_layer.features,
                &official_layer.feature_properties,
                official_source,
                official_error)) {
            json features = json::array();
            size_t area_threshold_count = 0;
            for (size_t i = 0; i < official_layer.features.size(); ++i) {
                json props = i < official_layer.feature_properties.size()
                    ? featurePropertiesToJson(official_layer.feature_properties[i])
                    : json::object();
                const double mde_sf = parseNumberForCli(props.value("MDE_SF", std::string()));
                if (mde_sf >= 35000.0) area_threshold_count += 1;
                props["beps_source"] = "official_mde_imap";
                props["beps_might_be_covered"] = "true";
                props["beps_confidence"] = "official_source";
                props["gross_floor_area_sqft"] = mde_sf;
                props["address"] = props.value("FIRST_ADDR", std::string());
                props["city"] = props.value("FIRST_CITY", std::string());
                props["county"] = props.value("JURSCODE", std::string());
                props["zip_code"] = props.value("FIRST_ZIPC", std::string());
                props["primary_use_type"] = props.value("ESPM_Prope", std::string());
                props["ubid"] = props.value("UBID_Combi", std::string());
                features.push_back({
                    {"type", "Feature"},
                    {"properties", std::move(props)},
                    {"geometry", featureGeometryToJson(official_layer.features[i])}
                });
            }

            json collection = {
                {"type", "FeatureCollection"},
                {"name", "maryland_beps_candidate_parcels"},
                {"properties", {
                    {"algorithm", "official_mde_beps_covered_buildings"},
                    {"source_layer", kOfficialBepsLayerFile},
                    {"source", official_source},
                    {"candidate_count", official_layer.features.size()},
                    {"area_threshold_count", area_threshold_count},
                    {"fallback_used", false},
                    {"disclaimer", "Official MDE/MD iMAP BEPS covered-buildings screening layer; confirm final obligations against COMAR and MDE reporting guidance."}
                }},
                {"features", std::move(features)}
            };

            const fs::path output_path = root / "data" / "derived" / "maryland_beps_candidate_parcels.geojson";
            std::error_code ec;
            fs::create_directories(output_path.parent_path(), ec);
            if (ec) {
                json out = {{"mode", "build-beps-candidates"}, {"ok", false}, {"error", ec.message()}};
                std::cout << out.dump(2) << '\n';
                return 1;
            }
            std::ofstream out_file(output_path);
            if (!out_file) {
                json out = {{"mode", "build-beps-candidates"}, {"ok", false}, {"error", "failed to open output file"}};
                std::cout << out.dump(2) << '\n';
                return 1;
            }
            out_file << collection.dump(2) << '\n';

            json out = {
                {"mode", "build-beps-candidates"},
                {"ok", true},
                {"source", "official_mde_imap"},
                {"source_layer", kOfficialBepsLayerFile},
                {"source_used", official_source},
                {"output_path", output_path.string()},
                {"candidate_count", official_layer.features.size()},
                {"area_threshold_count", area_threshold_count},
                {"fallback_used", false}
            };
            std::cout << out.dump(2) << '\n';
            return 0;
        }
    }

    const LayerDef* property_manifest_layer = findManifestLayerByIdentifier(manifest_layers, kPropertyLayerFile);
    const LayerDef* parcel_manifest_layer = findManifestLayerByIdentifier(manifest_layers, kParcelLayerFile);
    if (!property_manifest_layer) {
        json out = {
            {"mode", "build-beps-candidates"},
            {"ok", false},
            {"error", "required property layer is missing from manifest"},
            {"property_layer", kPropertyLayerFile}
        };
        std::cout << out.dump(2) << '\n';
        return 1;
    }

    LayerDef property_layer = *property_manifest_layer;
    LayerDef parcel_layer = parcel_manifest_layer ? *parcel_manifest_layer : LayerDef{};
    std::string property_source;
    std::string parcel_source;
    std::string property_error;
    std::string parcel_error;
    if (!loadLocalLayerFeatures(
            root,
            property_layer,
            property_layer.features,
            &property_layer.feature_properties,
            property_source,
            property_error)) {
        json out = {
            {"mode", "build-beps-candidates"},
            {"ok", false},
            {"property_source", property_source},
            {"property_error", property_error}
        };
        std::cout << out.dump(2) << '\n';
        return 1;
    }
    {
        std::vector<LayerDef::FeatureRecord> snapshot_features;
        std::vector<LayerDef::FeatureProperties> snapshot_properties;
        std::string snapshot_source;
        std::string snapshot_error;
        if (loadLatestSnapshotGeoJsonLayer(
                root,
                kPropertyLayerFile,
                snapshot_features,
                snapshot_properties,
                snapshot_source,
                snapshot_error)) {
            property_layer.features = std::move(snapshot_features);
            property_layer.feature_properties = std::move(snapshot_properties);
            property_source = snapshot_source;
        }
    }

    LayerDef* parcel_layer_ptr = nullptr;
    if (parcel_manifest_layer &&
        loadLocalLayerFeatures(root, parcel_layer, parcel_layer.features, &parcel_layer.feature_properties, parcel_source, parcel_error)) {
        parcel_layer_ptr = &parcel_layer;
    }

    BepsScreeningSummary summary;
    std::vector<BepsCandidate> candidates =
        screenMarylandBepsCandidates(property_layer, parcel_layer_ptr, &summary);
    if (candidates.empty()) {
        std::vector<LayerDef::FeatureRecord> snapshot_features;
        std::vector<LayerDef::FeatureProperties> snapshot_properties;
        std::string snapshot_source;
        std::string snapshot_error;
        if (loadLatestSnapshotGeoJsonLayer(
                root,
                kPropertyLayerFile,
                snapshot_features,
                snapshot_properties,
                snapshot_source,
                snapshot_error)) {
            property_layer.features = std::move(snapshot_features);
            property_layer.feature_properties = std::move(snapshot_properties);
            property_source = snapshot_source;
            candidates = screenMarylandBepsCandidates(property_layer, parcel_layer_ptr, &summary);
        }
    }
    if (!candidates.empty() && summary.matched_parcel_count == 0) {
        std::vector<LayerDef::FeatureRecord> snapshot_features;
        std::vector<LayerDef::FeatureProperties> snapshot_properties;
        std::string snapshot_source;
        std::string snapshot_error;
        if (loadLatestSnapshotGeoJsonLayer(
                root,
                kParcelLayerFile,
                snapshot_features,
                snapshot_properties,
                snapshot_source,
                snapshot_error)) {
            parcel_layer.features = std::move(snapshot_features);
            parcel_layer.feature_properties = std::move(snapshot_properties);
            parcel_source = snapshot_source;
            parcel_layer_ptr = &parcel_layer;
            candidates = screenMarylandBepsCandidates(property_layer, parcel_layer_ptr, &summary);
        }
    }

    LayerDef output_layer;
    output_layer.name = "Maryland BEPS Candidate Parcels";
    output_layer.file = "maryland_beps_candidate_parcels.geojson";
    output_layer.features.reserve(candidates.size());
    for (const BepsCandidate& candidate : candidates) {
        if (parcel_layer_ptr && candidate.parcel_feature_idx < parcel_layer_ptr->features.size()) {
            output_layer.features.push_back(parcel_layer_ptr->features[candidate.parcel_feature_idx]);
        } else if (candidate.property_feature_idx < property_layer.features.size()) {
            output_layer.features.push_back(property_layer.features[candidate.property_feature_idx]);
        }
    }
    applyBepsCandidateProperties(output_layer, candidates);

    json features = json::array();
    for (size_t i = 0; i < output_layer.features.size(); ++i) {
        features.push_back({
            {"type", "Feature"},
            {"properties", i < output_layer.feature_properties.size()
                ? featurePropertiesToJson(output_layer.feature_properties[i])
                : json::object()},
            {"geometry", featureGeometryToJson(output_layer.features[i])}
        });
    }

    json collection = {
        {"type", "FeatureCollection"},
        {"name", "maryland_beps_candidate_parcels"},
        {"properties", {
            {"algorithm", "comar_26_28_01_parcel_candidate_screen"},
            {"disclaimer", "Screening output only; COMAR coverage requires owner/building facts not fully present in parcel assessment data."},
            {"rule_basis", "COMAR 26.28.01 Building Energy Performance Standards; area threshold screen uses gross floor area >= 35000 sq ft."},
            {"property_layer", kPropertyLayerFile},
            {"parcel_layer", kParcelLayerFile},
            {"property_source", property_source},
            {"parcel_source", parcel_source},
            {"property_record_count", summary.property_record_count},
            {"candidate_count", summary.candidate_count},
            {"matched_parcel_count", summary.matched_parcel_count},
            {"high_confidence_count", summary.high_confidence_count},
            {"needs_review_count", summary.needs_review_count}
        }},
        {"features", std::move(features)}
    };

    const fs::path output_path = root / "data" / "derived" / "maryland_beps_candidate_parcels.geojson";
    std::error_code ec;
    fs::create_directories(output_path.parent_path(), ec);
    if (ec) {
        json out = {{"mode", "build-beps-candidates"}, {"ok", false}, {"error", ec.message()}};
        std::cout << out.dump(2) << '\n';
        return 1;
    }
    std::ofstream out_file(output_path);
    if (!out_file) {
        json out = {{"mode", "build-beps-candidates"}, {"ok", false}, {"error", "failed to open output file"}};
        std::cout << out.dump(2) << '\n';
        return 1;
    }
    out_file << collection.dump(2) << '\n';

    json out = {
        {"mode", "build-beps-candidates"},
        {"ok", true},
        {"output_path", output_path.string()},
        {"property_source", property_source},
        {"parcel_source", parcel_source},
        {"property_record_count", summary.property_record_count},
        {"candidate_count", summary.candidate_count},
        {"matched_parcel_count", summary.matched_parcel_count},
        {"high_confidence_count", summary.high_confidence_count},
        {"needs_review_count", summary.needs_review_count},
        {"parcel_match_warning", parcel_layer_ptr ? "" : parcel_error}
    };
    std::cout << out.dump(2) << '\n';
    return 0;
}

int runProjectionCacheSelftest() {
    LayerDef::FeatureRecord feature;
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
    LayerDef::FeatureRecord feature;
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
    LayerDef::FeatureRecord feature;
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
    feature.triangles = {
        0, 1, 7,
        0, 7, 4,
        1, 2, 6,
        1, 6, 7,
        2, 3, 5,
        2, 5, 6,
        3, 0, 4,
        3, 4, 5
    };

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

    LayerDef layer;
    layer.file = "polygon_hole_selftest.geojson";
    PolygonGeometryArtifact artifact;
    const bool artifact_ok = buildPolygonGeometryArtifact(layer, {feature}, "polygon_hole_selftest_sig", artifact, 64);
    bool artifact_centroids_valid = artifact_ok;
    size_t artifact_triangle_count = 0;
    for (size_t ti = 0; ti + 2 < artifact.fill_indices.size(); ti += 3) {
        const uint32_t ia = artifact.fill_indices[ti + 0];
        const uint32_t ib = artifact.fill_indices[ti + 1];
        const uint32_t ic = artifact.fill_indices[ti + 2];
        if (ia >= artifact.vertices.size() || ib >= artifact.vertices.size() || ic >= artifact.vertices.size()) {
            artifact_centroids_valid = false;
            break;
        }
        const ImVec2& a = artifact.vertices[ia];
        const ImVec2& b = artifact.vertices[ib];
        const ImVec2& c = artifact.vertices[ic];
        const float cx = (a.x + b.x + c.x) / 3.0f;
        const float cy = (a.y + b.y + c.y) / 3.0f;
        if (pointInRing(feature.rings[1], cx, cy) || !pointInFeature(feature, cx, cy)) {
            artifact_centroids_valid = false;
            break;
        }
        ++artifact_triangle_count;
    }
    const bool artifact_shape_ok =
        artifact_ok &&
        artifact.vertices.size() == flattened.size() &&
        artifact.line_indices.size() == 16 &&
        artifact.features.size() == 1 &&
        artifact.chunks.size() == 1 &&
        artifact_centroids_valid;

    json out = {
        {"mode", "polygon-hole-selftest"},
        {"ok", shell_point_inside && hole_point_rejected && outside_point_rejected && centroids_valid && artifact_shape_ok},
        {"shell_point_inside", shell_point_inside},
        {"hole_point_rejected", hole_point_rejected},
        {"outside_point_rejected", outside_point_rejected},
        {"triangle_index_count", feature.triangles.size()},
        {"triangle_count", centroid_count},
        {"centroids_valid", centroids_valid},
        {"polygon_artifact_triangle_count", artifact_triangle_count},
        {"polygon_artifact_centroids_valid", artifact_centroids_valid},
        {"polygon_artifact_ok", artifact_shape_ok}
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

    LayerRuntimeState missing_canonical_source;
    missing_canonical_source.status = LayerPipelineStatus::Hydrating;
    missing_canonical_source.hydration_phase = "canonical_source_cache_missing";

    LayerRuntimeState ready;
    ready.status = LayerPipelineStatus::Ready;

    const std::string file = "parcel.geojson";
    const bool ok =
        layerRuntimeDisplayStatus(hydration_cache, file) == "reading hydration cache" &&
        layerRuntimeDisplayStatus(canonical_binary, file) == "reading canonical layer binary" &&
        layerRuntimeDisplayStatus(missing_canonical_source, file) == "canonical layer binary missing" &&
        layerRuntimeDisplayStatus(ready, file) == "ready via compiled geometry artifact";

    json out = {
        {"mode", "layer-runtime-status-selftest"},
        {"ok", ok},
        {"hydration_cache", layerRuntimeDisplayStatus(hydration_cache, file)},
        {"canonical_binary", layerRuntimeDisplayStatus(canonical_binary, file)},
        {"missing_canonical_binary", layerRuntimeDisplayStatus(missing_canonical_source, file)},
        {"ready", layerRuntimeDisplayStatus(ready, file)}
    };
    std::cout << out.dump(2) << '\n';
    return ok ? 0 : 1;
}

int runStatusApiParcelDebugSelftest() {
    HoverDebugState hover;
    hover.map_hovered = true;
    hover.mouse_screen_x = 321.0f;
    hover.mouse_screen_y = 123.0f;
    hover.mouse_lon = -76.6122f;
    hover.mouse_lat = 39.2904f;
    hover.hovered_parcel = true;
    hover.hovered_parcel_layer_idx = 9;
    hover.hovered_parcel_idx = 63045;
    hover.hovered_parcel_entity_id = "ENTITY_PARCEL";
    hover.hovered_parcel_geometry_entity_id = "GEOMETRY_PARCEL";
    hover.inspect_parcel = true;
    hover.inspect_parcel_layer_idx = 10;
    hover.inspect_parcel_idx = 24173;
    hover.inspect_parcel_entity_id = "ENTITY_COUNTY";
    hover.inspect_parcel_geometry_entity_id = "GEOMETRY_COUNTY";
    hover.selected_parcel = true;
    hover.selected_parcel_layer_idx = 10;
    hover.selected_parcel_idx = 24173;
    hover.selected_parcel_entity_id = "CANONICAL_COUNTY";
    hover.selected_parcel_geometry_entity_id = "GEOMETRY_COUNTY";
    hover.selected_parcel_count = 1;

    const json out_json = buildHoverDebugStatusJson(hover);
    const bool ok =
        out_json.value("map_hovered", false) &&
        out_json.value("hovered_parcel", false) &&
        out_json.value("hovered_parcel_layer_idx", -1) == 9 &&
        out_json.value("hovered_parcel_idx", (uint64_t)0) == 63045u &&
        out_json.value("hovered_parcel_entity_id", std::string()) == "ENTITY_PARCEL" &&
        out_json.value("hovered_parcel_geometry_entity_id", std::string()) == "GEOMETRY_PARCEL" &&
        out_json.value("inspect_parcel", false) &&
        out_json.value("inspect_parcel_layer_idx", -1) == 10 &&
        out_json.value("inspect_parcel_idx", (uint64_t)0) == 24173u &&
        out_json.value("inspect_parcel_entity_id", std::string()) == "ENTITY_COUNTY" &&
        out_json.value("inspect_parcel_geometry_entity_id", std::string()) == "GEOMETRY_COUNTY" &&
        out_json.value("selected_parcel", false) &&
        out_json.value("selected_parcel_layer_idx", -1) == 10 &&
        out_json.value("selected_parcel_idx", (uint64_t)0) == 24173u &&
        out_json.value("selected_parcel_entity_id", std::string()) == "CANONICAL_COUNTY" &&
        out_json.value("selected_parcel_geometry_entity_id", std::string()) == "GEOMETRY_COUNTY" &&
        out_json.value("selected_parcel_count", (uint64_t)0) == 1u;

    std::cout << json{
        {"mode", "status-api-parcel-debug-selftest"},
        {"ok", ok},
        {"hover", out_json}
    }.dump(2) << '\n';
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
    const bool crime_plain_gpu_draws =
        shouldUseCrimePointPrimaryGpuDraw(true, false, false, false);
    const bool crime_heatmap_keeps_cpu_samples =
        !shouldUseCrimePointPrimaryGpuDraw(true, true, false, false);
    const bool crime_lod_keeps_cpu =
        !shouldUseCrimePointPrimaryGpuDraw(true, false, true, false);
    const bool crime_cluster_keeps_cpu =
        !shouldUseCrimePointPrimaryGpuDraw(true, false, false, true);
    const bool inactive_crime_gpu_does_not_draw =
        !shouldUseCrimePointPrimaryGpuDraw(false, false, false, false);
    const bool aggregate_label_names_layer =
        aggregateGenerationStatusLabel({"NIBRS Group A Crime Data (2022-Present)"}) ==
        "Generating aggregate: NIBRS Group A Crime Data (2022-Present)";
    const bool aggregate_label_summarizes_many_layers =
        aggregateGenerationStatusLabel({"Layer A", "Layer B", "Layer C"}) ==
        "Generating aggregate: Layer A, Layer B +1 more";
    LayerDef stable_aggregate_layer;
    stable_aggregate_layer.file = "stable_aggregate_points.geojson";
    stable_aggregate_layer.name = "Stable Aggregate Points";
    stable_aggregate_layer.enabled = true;
    stable_aggregate_layer.scale = "point";
    stable_aggregate_layer.features.resize(10);
    std::vector<LayerDef> stable_aggregate_layers{stable_aggregate_layer};
    std::function<bool(size_t)> stable_layer_contributes = [](size_t layer_idx) { return layer_idx == 0; };
    HeatmapKeyBuilderContext stable_key_ctx;
    stable_key_ctx.root = fs::temp_directory_path() / "worldsim3_stable_aggregate_key_selftest";
    stable_key_ctx.layers = &stable_aggregate_layers;
    stable_key_ctx.layer_uses_heatmap_aggregate = &stable_layer_contributes;
    stable_key_ctx.heatmap_algo = kAggregateGpuSplatBlur;
    stable_key_ctx.effective_heatmap_quality_preset = 2;
    stable_key_ctx.global_heat_cell_px = 24.0f;
    stable_key_ctx.heatmap_bandwidth_px = 18.0f;
    stable_key_ctx.heatmap_blur_sigma_px = 6.0f;
    stable_key_ctx.heatmap_percentile_clip = 95.0f;
    stable_key_ctx.heatmap_zoom_adaptive_bandwidth = true;
    stable_key_ctx.heatmap_multires_enabled = true;
    stable_key_ctx.heatmap_multires_blend = 0.5f;
    stable_key_ctx.zoom = 11;
    stable_key_ctx.math_zoom = 11;
    const uint64_t stable_view_key_11 = buildHeatmapKey(stable_key_ctx, true);
    const uint64_t stable_data_key_11 = buildHeatmapKey(stable_key_ctx, false);
    stable_key_ctx.zoom = 13;
    stable_key_ctx.math_zoom = 13;
    const uint64_t stable_view_key_13 = buildHeatmapKey(stable_key_ctx, true);
    const uint64_t stable_data_key_13 = buildHeatmapKey(stable_key_ctx, false);
    const bool stable_image_aggregate_key_ignores_zoom =
        stable_view_key_11 != stable_view_key_13 &&
        stable_data_key_11 == stable_data_key_13 &&
        selectHeatmapAggregateKey(stable_view_key_11, stable_data_key_11, true) ==
            selectHeatmapAggregateKey(stable_view_key_13, stable_data_key_13, true);
    const bool non_image_aggregate_key_remains_view_scoped =
        selectHeatmapAggregateKey(stable_view_key_11, stable_data_key_11, false) !=
        selectHeatmapAggregateKey(stable_view_key_13, stable_data_key_13, false);
    HeatSample valid_heat_sample;
    valid_heat_sample.layer = 0;
    valid_heat_sample.lon = -76.61f;
    valid_heat_sample.lat = 39.29f;
    valid_heat_sample.x = 128.0f;
    valid_heat_sample.y = 128.0f;
    valid_heat_sample.algo = kAggregateKdeGaussian;
    valid_heat_sample.color = ImVec4(1.0f, 0.2f, 0.1f, 1.0f);
    HeatSample null_island_sample = valid_heat_sample;
    null_island_sample.lon = 5.6843419e-14f;
    null_island_sample.lat = 5.6843419e-14f;
    const auto filtered_heatmap = buildHeatmapRenderData(
        0x12345678ULL,
        {valid_heat_sample, null_island_sample},
        0.0f,
        0.0f,
        -76.72f,
        39.18f,
        -76.48f,
        39.42f,
        512.0f,
        512.0f,
        11,
        19,
        384,
        512);
    const bool aggregate_filters_null_island_bounds =
        filtered_heatmap.second.has_raster &&
        filtered_heatmap.second.raster.min_lon < -76.0f &&
        filtered_heatmap.second.raster.max_lon < -76.0f &&
        filtered_heatmap.second.raster.min_lat > 39.0f &&
        filtered_heatmap.second.raster.max_lat > 39.0f;

    const fs::path heatmap_cache_selftest_root =
        fs::temp_directory_path() / "worldsim3_empty_heatmap_cache_selftest";
    std::error_code ec;
    fs::remove_all(heatmap_cache_selftest_root, ec);
    fs::create_directories(heatmap_cache_selftest_root / "data" / "cache" / "aggregate", ec);
    HeatmapRuntimeState empty_cache_state;
    empty_cache_state.cache_key = 0x777ULL;
    empty_cache_state.cache_valid = true;
    empty_cache_state.texture_cache.emplace(0x777ULL, CachedAggregateTexture{});
    const auto empty_cache_lookup = prepareHeatmapAggregateCache(
        heatmap_cache_selftest_root,
        empty_cache_state,
        true,
        true,
        0x777ULL);
    const bool empty_heatmap_cache_is_not_usable =
        !empty_cache_lookup.can_use_cached_heatmap &&
        empty_cache_state.texture_cache.find(0x777ULL) == empty_cache_state.texture_cache.end() &&
        !empty_cache_state.cache_valid;

    const fs::path raster_cache_path = heatmap_cache_selftest_root / "data" / "cache" / "aggregate" / "shot_hash_selftest.raster.bin";
    HeatmapRaster cache_raster;
    cache_raster.w = 2;
    cache_raster.h = 2;
    cache_raster.min_lon = -76.7f;
    cache_raster.min_lat = 39.1f;
    cache_raster.max_lon = -76.5f;
    cache_raster.max_lat = 39.4f;
    cache_raster.rgba = {
        255, 0, 0, 128,
        0, 255, 0, 128,
        0, 0, 255, 128,
        255, 255, 0, 128
    };
    const uint64_t expected_shot_hash = heatmapRasterShotHash(cache_raster);
    saveHeatmapRasterCache(raster_cache_path, 0xabcULL, cache_raster);
    HeatmapRaster loaded_cache_raster;
    const bool aggregate_disk_cache_loads_with_shot_hash =
        loadHeatmapRasterCache(raster_cache_path, 0xabcULL, loaded_cache_raster) &&
        loaded_cache_raster.shot_hash == expected_shot_hash &&
        loaded_cache_raster.rgba == cache_raster.rgba;
    HeatmapRaster wrong_key_cache_raster;
    const bool aggregate_disk_cache_rejects_wrong_key =
        !loadHeatmapRasterCache(raster_cache_path, 0xabdULL, wrong_key_cache_raster);
    {
        std::fstream corrupt(raster_cache_path, std::ios::in | std::ios::out | std::ios::binary);
        if (corrupt) {
            corrupt.seekg(-1, std::ios::end);
            char byte = 0;
            corrupt.read(&byte, 1);
            corrupt.clear();
            corrupt.seekp(-1, std::ios::end);
            byte ^= 0x1;
            corrupt.write(&byte, 1);
        }
    }
    HeatmapRaster corrupted_cache_raster;
    const bool aggregate_disk_cache_rejects_shot_hash_mismatch =
        !loadHeatmapRasterCache(raster_cache_path, 0xabcULL, corrupted_cache_raster);

    HeatmapRuntimeState empty_samples_state;
    empty_samples_state.cache_key = 0x778ULL;
    empty_samples_state.cache_valid = true;
    std::vector<HeatSample> empty_heat_samples;
    HeatmapFramePassContext empty_samples_ctx;
    empty_samples_ctx.runtime = &empty_samples_state;
    empty_samples_ctx.heat_samples = &empty_heat_samples;
    empty_samples_ctx.should_recompute_heatmap = true;
    empty_samples_ctx.heatmap_key = 0x778ULL;
    empty_samples_ctx.aggregate_generation_label = "Generating aggregate: selftest";
    runHeatmapFramePass(empty_samples_ctx);
    const bool empty_samples_do_not_create_valid_cache =
        !empty_samples_state.cache_valid &&
        empty_samples_state.cache_key == 0;

    HeatmapRuntimeState inactive_heatmap_state;
    inactive_heatmap_state.cache_key = 0x779ULL;
    inactive_heatmap_state.cache_valid = true;
    CachedAggregateTexture inactive_cached_aggregate;
    inactive_cached_aggregate.cells.push_back(CachedHeatCell{});
    inactive_heatmap_state.texture_cache.emplace(0x779ULL, std::move(inactive_cached_aggregate));
    const auto inactive_cache_lookup = prepareHeatmapAggregateCache(
        heatmap_cache_selftest_root,
        inactive_heatmap_state,
        false,
        true,
        0x779ULL);
    const bool inactive_heatmap_preserves_last_aggregate =
        !inactive_cache_lookup.can_use_cached_heatmap &&
        inactive_heatmap_state.cache_valid &&
        inactive_heatmap_state.cache_key == 0x779ULL &&
        inactive_heatmap_state.texture_cache.find(0x779ULL) != inactive_heatmap_state.texture_cache.end();
    const auto reactivated_cache_lookup = prepareHeatmapAggregateCache(
        heatmap_cache_selftest_root,
        inactive_heatmap_state,
        true,
        true,
        0x779ULL);
    const bool reactivated_heatmap_reuses_preserved_aggregate =
        reactivated_cache_lookup.can_use_cached_heatmap &&
        reactivated_cache_lookup.cached_aggregate_for_key != nullptr &&
        inactive_heatmap_state.cache_valid &&
        inactive_heatmap_state.cache_key == 0x779ULL;

    LayerDef::FeatureRecord null_extent_point;
    null_extent_point.extent.min_lon = 5.6843419e-14f;
    null_extent_point.extent.max_lon = 5.6843419e-14f;
    null_extent_point.extent.min_lat = 5.6843419e-14f;
    null_extent_point.extent.max_lat = 5.6843419e-14f;
    std::unordered_map<size_t, PointGeometryArtifact> point_anchor_artifacts;
    PointGeometryArtifact point_anchor_artifact;
    point_anchor_artifact.positions.push_back(ImVec2(-76.61f, 39.29f));
    point_anchor_artifacts.emplace(0, std::move(point_anchor_artifact));
    RenderLayerPassContext point_anchor_ctx;
    point_anchor_ctx.point_geometry_artifacts = &point_anchor_artifacts;
    float aggregate_anchor_lon = 0.0f;
    float aggregate_anchor_lat = 0.0f;
    const bool point_artifact_anchor_overrides_null_extent =
        aggregateSampleAnchorLonLatForFeature(
            point_anchor_ctx,
            0,
            0,
            null_extent_point,
            aggregate_anchor_lon,
            aggregate_anchor_lat) &&
        nearlyEqual(aggregate_anchor_lon, -76.61f) &&
        nearlyEqual(aggregate_anchor_lat, 39.29f);

    RenderLayerPassContext no_point_anchor_ctx;
    float rejected_anchor_lon = 0.0f;
    float rejected_anchor_lat = 0.0f;
    const bool null_extent_without_point_artifact_rejected =
        !aggregateSampleAnchorLonLatForFeature(
            no_point_anchor_ctx,
            0,
            0,
            null_extent_point,
            rejected_anchor_lon,
            rejected_anchor_lat);

    CrimePointRuntimeState crime_sampling_state;
    crime_sampling_state.uploaded_signature = "crime_sig";
    crime_sampling_state.artifact.positions.push_back(ImVec2(-76.62f, 39.30f));
    GeometryArtifactFeatureRecord crime_feature_rec;
    crime_feature_rec.feature_idx = 0;
    crime_sampling_state.artifact.features.push_back(crime_feature_rec);
    std::unordered_map<size_t, PointGeometryArtifact> published_point_artifacts;
    std::unordered_map<size_t, std::string> published_point_signatures;
    publishCrimePointArtifactForAggregateSampling(
        93,
        crime_sampling_state,
        published_point_artifacts,
        &published_point_signatures);
    const bool crime_point_artifact_published_for_aggregate_sampling =
        published_point_artifacts.find(93) != published_point_artifacts.end() &&
        published_point_signatures[93] == "crime_sig";
    published_point_artifacts[93].positions[0] = ImVec2(-75.0f, 38.0f);
    publishCrimePointArtifactForAggregateSampling(
        93,
        crime_sampling_state,
        published_point_artifacts,
        &published_point_signatures);
    const bool crime_point_artifact_publish_skips_same_signature =
        published_point_artifacts.find(93) != published_point_artifacts.end() &&
        nearlyEqual(published_point_artifacts[93].positions[0].x, -75.0f) &&
        nearlyEqual(published_point_artifacts[93].positions[0].y, 38.0f);
    crime_sampling_state.uploaded_signature = "crime_sig_2";
    publishCrimePointArtifactForAggregateSampling(
        93,
        crime_sampling_state,
        published_point_artifacts,
        &published_point_signatures);
    const bool crime_point_artifact_publish_updates_new_signature =
        published_point_artifacts.find(93) != published_point_artifacts.end() &&
        published_point_signatures[93] == "crime_sig_2" &&
        nearlyEqual(published_point_artifacts[93].positions[0].x, -76.62f) &&
        nearlyEqual(published_point_artifacts[93].positions[0].y, 39.30f);
    crime_sampling_state.uploaded_signature.clear();
    publishCrimePointArtifactForAggregateSampling(
        93,
        crime_sampling_state,
        published_point_artifacts,
        &published_point_signatures);
    const bool crime_point_artifact_unpublished_when_not_ready =
        published_point_artifacts.find(93) == published_point_artifacts.end() &&
        published_point_signatures.find(93) == published_point_signatures.end();

    LayerDef artifact_only_point_layer;
    artifact_only_point_layer.file = "artifact_only_points.geojson";
    artifact_only_point_layer.name = "Artifact Only Points";
    artifact_only_point_layer.enabled = true;
    artifact_only_point_layer.scale = "point";
    artifact_only_point_layer.duckdb_role = "point_event";
    artifact_only_point_layer.color = ImVec4(1.0f, 0.3f, 0.1f, 1.0f);
    std::vector<LayerDef> artifact_only_layers{artifact_only_point_layer};
    std::vector<bool> artifact_only_heatmap_enabled{true};
    std::vector<int> artifact_only_heatmap_algo{kAggregateGpuSplatBlur};
    std::vector<int> artifact_only_heatmap_max_zoom{13};
    std::vector<int> artifact_only_detail_zoom{14};
    std::vector<float> artifact_only_cell_px{24.0f};
    std::vector<float> artifact_only_bandwidth_px{18.0f};
    std::vector<float> artifact_only_blur_px{6.0f};
    std::vector<float> artifact_only_percentile{95.0f};
    std::vector<bool> artifact_only_adaptive{true};
    std::vector<bool> artifact_only_multires{true};
    std::vector<float> artifact_only_blend{0.5f};
    std::vector<bool> artifact_only_gradient{true};
    std::vector<float> artifact_only_gamma{1.0f};
    std::vector<int> artifact_only_normalize{0};
    HeatmapLayerPolicyContext artifact_only_policy;
    artifact_only_policy.layers = &artifact_only_layers;
    artifact_only_policy.layer_heatmap_enabled = &artifact_only_heatmap_enabled;
    artifact_only_policy.layer_heatmap_algo = &artifact_only_heatmap_algo;
    artifact_only_policy.layer_heatmap_max_zoom = &artifact_only_heatmap_max_zoom;
    artifact_only_policy.layer_parcel_detail_min_zoom = &artifact_only_detail_zoom;
    artifact_only_policy.layer_heatmap_cell_px = &artifact_only_cell_px;
    artifact_only_policy.layer_heatmap_bandwidth_px = &artifact_only_bandwidth_px;
    artifact_only_policy.layer_heatmap_blur_sigma_px = &artifact_only_blur_px;
    artifact_only_policy.layer_heatmap_percentile_clip = &artifact_only_percentile;
    artifact_only_policy.layer_heatmap_zoom_adaptive_bandwidth = &artifact_only_adaptive;
    artifact_only_policy.layer_heatmap_multires_enabled = &artifact_only_multires;
    artifact_only_policy.layer_heatmap_multires_blend = &artifact_only_blend;
    artifact_only_policy.zoom = 11;
    artifact_only_policy.heatmap_algo = kAggregateGpuSplatBlur;
    artifact_only_policy.global_heat_cell_px = 24.0f;
    artifact_only_policy.heatmap_bandwidth_px = 18.0f;
    artifact_only_policy.heatmap_blur_sigma_px = 6.0f;
    artifact_only_policy.heatmap_percentile_clip = 95.0f;
    artifact_only_policy.heatmap_zoom_adaptive_bandwidth = true;
    artifact_only_policy.heatmap_multires_enabled = true;
    artifact_only_policy.heatmap_multires_blend = 0.5f;
    PointGeometryArtifact artifact_only_points;
    artifact_only_points.positions.push_back(ImVec2(-76.63f, 39.28f));
    artifact_only_points.feature_refs.push_back(0);
    GeometryArtifactFeatureRecord artifact_only_rec;
    artifact_only_rec.feature_idx = 0;
    artifact_only_points.features.push_back(artifact_only_rec);
    std::unordered_map<size_t, PointGeometryArtifact> artifact_only_point_map;
    artifact_only_point_map.emplace(0, std::move(artifact_only_points));
    RenderPlan artifact_only_plan;
    artifact_only_plan.draw_layer_order.push_back(0);
    std::vector<HeatSample> artifact_only_samples;
    RenderLayerPassContext artifact_only_ctx;
    artifact_only_ctx.should_recompute_heatmap = true;
    artifact_only_ctx.high_quality_gpu_aggregate = true;
    artifact_only_ctx.smooth_only_heatmap = true;
    artifact_only_ctx.math_zoom = 11;
    artifact_only_ctx.layers = &artifact_only_layers;
    artifact_only_ctx.point_geometry_artifacts = &artifact_only_point_map;
    artifact_only_ctx.layer_heatmap_use_gradient = &artifact_only_gradient;
    artifact_only_ctx.layer_choropleth_gamma = &artifact_only_gamma;
    artifact_only_ctx.layer_normalize_mode = &artifact_only_normalize;
    artifact_only_ctx.layer_heatmap_percentile_clip = &artifact_only_percentile;
    artifact_only_ctx.heatmap_policy = &artifact_only_policy;
    artifact_only_ctx.render_plan = &artifact_only_plan;
    artifact_only_ctx.heat_samples = &artifact_only_samples;
    artifact_only_ctx.layer_passes_filters = [](size_t) { return true; };
    artifact_only_ctx.feature_passes_filters = [](size_t, size_t, const LayerDef::FeatureRecord&) { return true; };
    runRenderLayerPass(artifact_only_ctx);
    const bool high_quality_point_aggregate_samples_artifact_without_features =
        artifact_only_samples.size() == 1 &&
        nearlyEqual(artifact_only_samples[0].lon, -76.63f) &&
        nearlyEqual(artifact_only_samples[0].lat, 39.28f);
    artifact_only_samples.clear();
    artifact_only_heatmap_algo[0] = kAggregateKdeGaussian;
    artifact_only_policy.heatmap_algo = kAggregateKdeGaussian;
    artifact_only_ctx.high_quality_gpu_aggregate = false;
    runRenderLayerPass(artifact_only_ctx);
    const bool standard_point_aggregate_samples_artifact_without_features =
        artifact_only_samples.size() == 1 &&
        nearlyEqual(artifact_only_samples[0].lon, -76.63f) &&
        nearlyEqual(artifact_only_samples[0].lat, 39.28f);

    const bool ok =
        plain_gpu_bypasses &&
        inactive_gpu_does_not_bypass &&
        heatmap_recompute_keeps_cpu &&
        cached_heatmap_bypasses &&
        lod_keeps_cpu &&
        lod_heatmap_keeps_cpu &&
        crime_plain_gpu_draws &&
        crime_heatmap_keeps_cpu_samples &&
        crime_lod_keeps_cpu &&
        crime_cluster_keeps_cpu &&
        inactive_crime_gpu_does_not_draw &&
        aggregate_label_names_layer &&
        aggregate_label_summarizes_many_layers &&
        stable_image_aggregate_key_ignores_zoom &&
        non_image_aggregate_key_remains_view_scoped &&
        aggregate_filters_null_island_bounds &&
        empty_heatmap_cache_is_not_usable &&
        aggregate_disk_cache_loads_with_shot_hash &&
        aggregate_disk_cache_rejects_wrong_key &&
        aggregate_disk_cache_rejects_shot_hash_mismatch &&
        empty_samples_do_not_create_valid_cache &&
        inactive_heatmap_preserves_last_aggregate &&
        reactivated_heatmap_reuses_preserved_aggregate &&
        point_artifact_anchor_overrides_null_extent &&
        null_extent_without_point_artifact_rejected &&
        crime_point_artifact_published_for_aggregate_sampling &&
        crime_point_artifact_publish_skips_same_signature &&
        crime_point_artifact_publish_updates_new_signature &&
        crime_point_artifact_unpublished_when_not_ready &&
        high_quality_point_aggregate_samples_artifact_without_features &&
        standard_point_aggregate_samples_artifact_without_features;

    json out = {
        {"mode", "parcel-gpu-cpu-bypass-selftest"},
        {"ok", ok},
        {"plain_gpu_bypasses", plain_gpu_bypasses},
        {"inactive_gpu_does_not_bypass", inactive_gpu_does_not_bypass},
        {"heatmap_recompute_keeps_cpu", heatmap_recompute_keeps_cpu},
        {"cached_heatmap_bypasses", cached_heatmap_bypasses},
        {"lod_keeps_cpu", lod_keeps_cpu},
        {"lod_heatmap_keeps_cpu", lod_heatmap_keeps_cpu},
        {"crime_plain_gpu_draws", crime_plain_gpu_draws},
        {"crime_heatmap_keeps_cpu_samples", crime_heatmap_keeps_cpu_samples},
        {"crime_lod_keeps_cpu", crime_lod_keeps_cpu},
        {"crime_cluster_keeps_cpu", crime_cluster_keeps_cpu},
        {"inactive_crime_gpu_does_not_draw", inactive_crime_gpu_does_not_draw},
        {"aggregate_label_names_layer", aggregate_label_names_layer},
        {"aggregate_label_summarizes_many_layers", aggregate_label_summarizes_many_layers},
        {"stable_image_aggregate_key_ignores_zoom", stable_image_aggregate_key_ignores_zoom},
        {"non_image_aggregate_key_remains_view_scoped", non_image_aggregate_key_remains_view_scoped},
        {"aggregate_filters_null_island_bounds", aggregate_filters_null_island_bounds},
        {"empty_heatmap_cache_is_not_usable", empty_heatmap_cache_is_not_usable},
        {"aggregate_disk_cache_loads_with_shot_hash", aggregate_disk_cache_loads_with_shot_hash},
        {"aggregate_disk_cache_rejects_wrong_key", aggregate_disk_cache_rejects_wrong_key},
        {"aggregate_disk_cache_rejects_shot_hash_mismatch", aggregate_disk_cache_rejects_shot_hash_mismatch},
        {"empty_samples_do_not_create_valid_cache", empty_samples_do_not_create_valid_cache},
        {"inactive_heatmap_preserves_last_aggregate", inactive_heatmap_preserves_last_aggregate},
        {"reactivated_heatmap_reuses_preserved_aggregate", reactivated_heatmap_reuses_preserved_aggregate},
        {"point_artifact_anchor_overrides_null_extent", point_artifact_anchor_overrides_null_extent},
        {"null_extent_without_point_artifact_rejected", null_extent_without_point_artifact_rejected},
        {"crime_point_artifact_published_for_aggregate_sampling", crime_point_artifact_published_for_aggregate_sampling},
        {"crime_point_artifact_publish_skips_same_signature", crime_point_artifact_publish_skips_same_signature},
        {"crime_point_artifact_publish_updates_new_signature", crime_point_artifact_publish_updates_new_signature},
        {"crime_point_artifact_unpublished_when_not_ready", crime_point_artifact_unpublished_when_not_ready},
        {"high_quality_point_aggregate_samples_artifact_without_features", high_quality_point_aggregate_samples_artifact_without_features},
        {"standard_point_aggregate_samples_artifact_without_features", standard_point_aggregate_samples_artifact_without_features}
    };
    std::cout << out.dump(2) << '\n';
    return ok ? 0 : 1;
}

int runVulkanDeviceLossSelftest() {
    g_VulkanDeviceLost.store(false, std::memory_order_relaxed);
    const bool returned_ok = check_vk_result_allow_device_loss(VK_ERROR_DEVICE_LOST);
    const bool flag_set = g_VulkanDeviceLost.load(std::memory_order_relaxed);
    TileTexture descriptor_only_texture;
    descriptor_only_texture.descriptor = reinterpret_cast<VkDescriptorSet>(static_cast<uintptr_t>(0x1));
    destroyTileTextureNow(descriptor_only_texture);
    const bool descriptor_cleanup_after_device_loss_ok =
        descriptor_only_texture.descriptor == VK_NULL_HANDLE &&
        descriptor_only_texture.view == VK_NULL_HANDLE &&
        descriptor_only_texture.image == VK_NULL_HANDLE &&
        descriptor_only_texture.memory == VK_NULL_HANDLE;
    g_VulkanDeviceLost.store(false, std::memory_order_relaxed);
    const bool ok = !returned_ok && flag_set && descriptor_cleanup_after_device_loss_ok;
    json out = {
        {"mode", "vulkan-device-loss-selftest"},
        {"ok", ok},
        {"returned_ok", returned_ok},
        {"flag_set", flag_set},
        {"descriptor_cleanup_after_device_loss_ok", descriptor_cleanup_after_device_loss_ok}
    };
    std::cout << out.dump(2) << '\n';
    return ok ? 0 : 1;
}

int runParcelQueryLayerStyleSelftest() {
    FeatureRenderState query_state;
    query_state.visible = true;
    query_state.has_query_color = true;
    query_state.query_color = IM_COL32(0, 255, 0, 255);
    const ImU32 base_blue = IM_COL32(120, 160, 180, 255);
    const ImU32 value_red = IM_COL32(255, 0, 0, 255);
    const ImU32 query_overrides_value = resolveParcelRuntimeBaseFillColor(
        &query_state,
        base_blue,
        0.05f,
        true,
        value_red);

    FeatureRenderState hidden_query_state = query_state;
    hidden_query_state.visible = false;
    const ImU32 hidden_color = resolveParcelRuntimeBaseFillColor(
        &hidden_query_state,
        base_blue,
        1.0f,
        true,
        value_red);

    FeatureRenderState no_query_state;
    no_query_state.visible = true;
    const ImU32 value_without_query = resolveParcelRuntimeBaseFillColor(
        &no_query_state,
        base_blue,
        0.5f,
        true,
        value_red);
    const ImU32 base_without_query = resolveParcelRuntimeBaseFillColor(
        &no_query_state,
        base_blue,
        0.5f,
        false,
        IM_COL32(0, 0, 0, 0));
    const ImU32 query_overlay = resolveParcelRuntimeOverlayColor(&query_state, value_red);
    const ImU32 domain_overlay = resolveParcelRuntimeOverlayColor(&no_query_state, value_red);

    const bool ok =
        query_overrides_value == IM_COL32(0, 255, 0, 255) &&
        hidden_color == IM_COL32(0, 0, 0, 0) &&
        ((value_without_query >> 24) & 0xFFu) == 128u &&
        ((base_without_query >> 24) & 0xFFu) == 128u &&
        query_overlay == IM_COL32(0, 255, 0, 255) &&
        domain_overlay == value_red;
    json out = {
        {"mode", "parcel-query-layer-style-selftest"},
        {"ok", ok},
        {"query_overrides_value", query_overrides_value == IM_COL32(0, 255, 0, 255)},
        {"query_alpha_preserved", ((query_overrides_value >> 24) & 0xFFu) == 255u},
        {"hidden_query_transparent", hidden_color == IM_COL32(0, 0, 0, 0)},
        {"value_without_query_uses_base_opacity", ((value_without_query >> 24) & 0xFFu) == 128u},
        {"base_without_query_uses_base_opacity", ((base_without_query >> 24) & 0xFFu) == 128u},
        {"query_overlay_overrides_domain_overlay", query_overlay == IM_COL32(0, 255, 0, 255)},
        {"domain_overlay_preserved_without_query", domain_overlay == value_red}
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

int runRenderPolygonTileRuntimePolicySelftest() {
    LayerDef generic_polygon;
    generic_polygon.enabled = true;
    generic_polygon.file = "generic.geojson";

    LayerDef heatmap_polygon = generic_polygon;
    heatmap_polygon.file = "heat.geojson";
    heatmap_polygon.heatmap_field = "value";

    LayerDef zoning_polygon = generic_polygon;
    zoning_polygon.file = "zoning.geojson";
    zoning_polygon.subcategory = "zoning";

    LayerDef active_parcel = generic_polygon;
    active_parcel.file = "parcel.geojson";
    active_parcel.scale = "parcel";
    active_parcel.duckdb_role = "parcel_record";

    auto mode_name = [](const LayerDef& layer,
                        LayerRenderRoute route,
                        int zoom,
                        bool filter_state_active,
                        bool query_state_active) {
        PolygonRasterTilePolicyContext ctx;
        ctx.layer = &layer;
        ctx.render_route = route;
        ctx.zoom = zoom;
        ctx.fill_enabled = true;
        ctx.filter_state_active = filter_state_active;
        ctx.query_state_active = query_state_active;
        switch (resolvePolygonRasterTileMode(ctx)) {
            case PolygonRasterTileMode::VectorOnly: return std::string("vector_only");
            case PolygonRasterTileMode::RasterOnly: return std::string("raster_only");
            case PolygonRasterTileMode::RasterBaseVectorOutline: return std::string("raster_base_vector_outline");
        }
        return std::string("vector_only");
    };

    const std::string generic_polygon_zoom_10 =
        mode_name(generic_polygon, LayerRenderRoute::GenericPolygonGpu, 10, false, false);
    const std::string generic_polygon_zoom_12 =
        mode_name(generic_polygon, LayerRenderRoute::GenericPolygonGpu, 12, false, false);
    const std::string generic_polygon_zoom_14 =
        mode_name(generic_polygon, LayerRenderRoute::GenericPolygonGpu, 14, false, false);
    const std::string heatmap_polygon_zoom_10 =
        mode_name(heatmap_polygon, LayerRenderRoute::GenericPolygonGpu, 10, false, false);
    const std::string zoning_polygon_zoom_10 =
        mode_name(zoning_polygon, LayerRenderRoute::GenericPolygonGpu, 10, false, false);
    const std::string filtered_polygon_zoom_10 =
        mode_name(generic_polygon, LayerRenderRoute::GenericPolygonGpu, 10, true, false);
    const std::string query_polygon_zoom_10 =
        mode_name(generic_polygon, LayerRenderRoute::GenericPolygonGpu, 10, false, true);
    const std::string active_parcel_zoom_10 =
        mode_name(active_parcel, LayerRenderRoute::ParcelPolygonGpu, 10, false, false);
    const std::string county_parcel_zoom_10 =
        mode_name(active_parcel, LayerRenderRoute::ParcelPolygonGpu, 10, false, false);

    const bool ok =
        generic_polygon_zoom_10 == "raster_only" &&
        generic_polygon_zoom_12 == "raster_base_vector_outline" &&
        generic_polygon_zoom_14 == "vector_only" &&
        heatmap_polygon_zoom_10 == "vector_only" &&
        zoning_polygon_zoom_10 == "vector_only" &&
        filtered_polygon_zoom_10 == "vector_only" &&
        query_polygon_zoom_10 == "vector_only" &&
        active_parcel_zoom_10 == "raster_only" &&
        county_parcel_zoom_10 == "raster_only";

    json out = {
        {"mode", "render-polygon-tile-runtime-policy-selftest"},
        {"ok", ok},
        {"generic_polygon_zoom_10", generic_polygon_zoom_10},
        {"generic_polygon_zoom_12", generic_polygon_zoom_12},
        {"generic_polygon_zoom_14", generic_polygon_zoom_14},
        {"heatmap_polygon_zoom_10", heatmap_polygon_zoom_10},
        {"zoning_polygon_zoom_10", zoning_polygon_zoom_10},
        {"filtered_polygon_zoom_10", filtered_polygon_zoom_10},
        {"query_polygon_zoom_10", query_polygon_zoom_10},
        {"active_parcel_zoom_10", active_parcel_zoom_10},
        {"county_parcel_zoom_10", county_parcel_zoom_10}
    };
    std::cout << out.dump(2) << '\n';
    return ok ? 0 : 1;
}

int runRenderPlanSelftest() {
    std::vector<LayerDef> layers(4);
    layers[0].file = "basemap_context.geojson";
    layers[1].file = "large_parcels.layer";
    layers[1].logical_id = "large_parcels";
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
    if (file.empty()) file = "parcel.geojson";
    if (deprecatedRegionalParcelsAlias(file)) {
        std::cout << deprecatedRegionalParcelsAliasError("parcel-artifact-health", file).dump(2) << '\n';
        return 2;
    }
    if (!isBareLayerFilename(file)) {
        std::cout << json{
            {"mode", "parcel-artifact-health"},
            {"file", file},
            {"ok", false},
            {"error", "requires a layer filename, not a path"}
        }.dump(2) << '\n';
        return 2;
    }

    const std::string storage_key = resolveLayerStorageKey(root, file);
    const fs::path layer_path = resolveStoredLayerPathForFile(root, storage_key);
    const fs::path canonical_path = canonicalLayerPathForFile(root, storage_key);
    std::vector<LayerDef> manifest_layers = loadManifest(root);
    const LayerDef* manifest_layer = nullptr;
    for (const auto& candidate : manifest_layers) {
        if (candidate.file == storage_key) {
            manifest_layer = &candidate;
            break;
        }
    }
    const char* polygon_render_path = manifest_layer
        ? layerRenderRouteArtifactName(cliRenderRouteForLayer(root, manifest_layers, *manifest_layer))
        : layerRenderRouteArtifactName(LayerRenderRoute::ParcelPolygonGpu);
    const fs::path polygon_path = geometryArtifactCachePathForLayerFile(
        root,
        file,
        GeometryArtifactClass::Polygon,
        polygon_render_path);
    const fs::path duckdb_path = root / "data" / "worldsim.duckdb";

    std::string resolved_sig;
    std::string resolved_kind;
    const bool resolved = resolveLayerSourceSignature(layer_path, resolved_sig, &resolved_kind);

    CanonicalFeatureCollectionMetadata canonical_meta;
    const bool canonical_ok = loadBinaryCanonicalMetadata(canonical_path, canonical_meta);
    const uint64_t expected_count = canonical_ok ? canonical_meta.feature_count : 0;
    const std::string expected_sig = resolved ? resolved_sig : canonical_meta.source_signature;

    uint64_t polygon_feature_count = 0;
    const bool polygon_ok = validateBinaryPolygonGeometryArtifactHeader(
        polygon_path,
        expected_sig,
        &polygon_feature_count);

    std::error_code legacy_artifact_ec;
    const bool legacy_layer_artifact_present = fs::exists(layer_path, legacy_artifact_ec) && !legacy_artifact_ec;
    const uintmax_t legacy_layer_artifact_size = legacy_layer_artifact_present ? fileSizeOrZero(layer_path) : 0;
    const bool duckdb_present = fs::exists(duckdb_path, legacy_artifact_ec) && !legacy_artifact_ec;
    const uintmax_t duckdb_size = duckdb_present ? fileSizeOrZero(duckdb_path) : 0;

    json recommendations = json::array();
    if (!canonical_ok) recommendations.push_back("build canonical parcel binary");
    if (!polygon_ok) recommendations.push_back("compile polygon geometry artifact");
    if (!duckdb_present) recommendations.push_back("rebuild DuckDB analytics cache");
    if (legacy_layer_artifact_present) {
        recommendations.push_back("remove stale legacy stored-layer artifact; normal runtime uses canonical binary only");
    }

    const bool ok =
        resolved &&
        canonical_ok &&
        polygon_ok &&
        duckdb_present;

    json out = {
        {"mode", "parcel-artifact-health"},
        {"file", file},
        {"ok", ok},
        {"resolved_source_signature", expected_sig},
        {"resolved_source_kind", resolved_kind},
        {"legacy_layer_artifact", {
            {"present", legacy_layer_artifact_present},
            {"path", layer_path.string()},
            {"file_size_bytes", legacy_layer_artifact_size}
        }},
        {"canonical_binary", {
            {"ok", canonical_ok},
            {"path", canonical_path.string()},
            {"file_size_bytes", canonical_ok ? canonical_meta.file_size_bytes : fileSizeOrZero(canonical_path)},
            {"source_signature", canonical_meta.source_signature},
            {"signature_match", canonical_ok && canonical_meta.source_signature == expected_sig},
            {"feature_count", canonical_meta.feature_count}
        }},
        {"polygon_geometry_artifact", {
            {"ok", polygon_ok},
            {"path", polygon_path.string()},
            {"file_size_bytes", fileSizeOrZero(polygon_path)},
            {"source_signature", expected_sig},
            {"signature_match", polygon_ok},
            {"feature_count", polygon_feature_count}
        }},
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

int runRenderRoutingSelftest(const fs::path& root) {
    LayerDef city_parcel;
    city_parcel.file = "parcel.geojson";
    city_parcel.scale = "parcel";
    city_parcel.duckdb_role = "parcel_record";

    LayerDef county_parcel;
    county_parcel.file = "baltimore_county_parcels.geojson";
    county_parcel.scale = "parcel";
    county_parcel.duckdb_role = "parcel_record";

    LayerDef point_layer;
    point_layer.file = "points.geojson";
    point_layer.import_lon_field = "lon";
    point_layer.import_lat_field = "lat";

    LayerDef polyline_layer;
    polyline_layer.file = "roads.geojson";
    polyline_layer.import_type = "polyline";

    LayerDef polygon_layer;
    polygon_layer.file = "chap_district.geojson";

    struct Case {
        const char* label = "";
        size_t layer_idx = 0;
        int active_parcel_idx = -1;
        LayerDef layer;
        LayerRenderRoute expected_route = LayerRenderRoute::GenericPolygonGpu;
        GeometryArtifactClass artifact_class = GeometryArtifactClass::Polygon;
        const char* expected_artifact_token = "";
    };

    std::vector<Case> cases = {
        {"city_parcel", 0, 0, city_parcel, LayerRenderRoute::ParcelPolygonGpu, GeometryArtifactClass::Polygon, ".parcel_polygon_gpu.polygon.bin"},
        {"county_parcel", 1, 0, county_parcel, LayerRenderRoute::ParcelPolygonGpu, GeometryArtifactClass::Polygon, ".parcel_polygon_gpu.polygon.bin"},
        {"point", 2, 0, point_layer, LayerRenderRoute::PointGpu, GeometryArtifactClass::Point, ".point_gpu.point.bin"},
        {"polyline", 3, 0, polyline_layer, LayerRenderRoute::PolylineGpu, GeometryArtifactClass::Polyline, ".polyline_gpu.polyline.bin"},
        {"generic_polygon", 4, 0, polygon_layer, LayerRenderRoute::GenericPolygonGpu, GeometryArtifactClass::Polygon, ".generic_polygon_gpu.polygon.bin"}
    };

    json rows = json::array();
    bool ok = true;
    for (const Case& c : cases) {
        const LayerRenderRoute actual_route =
            classifyLayerRenderRoute(c.layer_idx, c.layer, c.active_parcel_idx);
        const char* route_artifact_name = layerRenderRouteArtifactName(actual_route);
        const fs::path artifact_path = geometryArtifactCachePathForLayerFile(
            root,
            c.layer.file,
            c.artifact_class,
            route_artifact_name);
        const bool route_ok = actual_route == c.expected_route;
        const bool filename_ok =
            artifact_path.filename().string().find(c.expected_artifact_token) != std::string::npos;
        ok = ok && route_ok && filename_ok;
        rows.push_back({
            {"case", c.label},
            {"ok", route_ok && filename_ok},
            {"route", layerRenderRouteArtifactName(actual_route)},
            {"route_display_ready", layerRenderRouteName(actual_route, true)},
            {"reason", layerRenderRouteReason(actual_route)},
            {"artifact_path", artifact_path.string()},
            {"route_ok", route_ok},
            {"filename_ok", filename_ok}
        });
    }

    json out = {
        {"mode", "render-routing-selftest"},
        {"ok", ok},
        {"cases", std::move(rows)}
    };
    std::cout << out.dump(2) << '\n';
    return ok ? 0 : 1;
}

struct TilePoint {
    float x = 0.0f;
    float y = 0.0f;
};

double clampWebMercatorLat(double lat) {
    return std::clamp(lat, -85.05112878, 85.05112878);
}

TilePoint lonLatToTilePixel(double lon, double lat, int z, int x, int y, int tile_size) {
    constexpr double kPi = 3.14159265358979323846;
    const double n = std::ldexp(1.0, z);
    const double lat_rad = clampWebMercatorLat(lat) * kPi / 180.0;
    const double tile_x = (lon + 180.0) / 360.0 * n;
    const double tile_y =
        (1.0 - std::log(std::tan(lat_rad) + 1.0 / std::cos(lat_rad)) / kPi) / 2.0 * n;
    return {
        static_cast<float>((tile_x - static_cast<double>(x)) * tile_size),
        static_cast<float>((tile_y - static_cast<double>(y)) * tile_size)
    };
}

float tileEdge(const TilePoint& a, const TilePoint& b, const TilePoint& p) {
    return (p.x - a.x) * (b.y - a.y) - (p.y - a.y) * (b.x - a.x);
}

uint8_t layerColorByte(float value, uint8_t fallback) {
    if (!std::isfinite(value) || value <= 0.0f) return fallback;
    return static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(value * 255.0f)), 0, 255));
}

struct RasterTileStats {
    size_t triangles_considered = 0;
    size_t triangles_drawn = 0;
    size_t pixels_written = 0;
};

void fillRasterTriangle(
    std::vector<uint8_t>& rgb,
    int tile_size,
    const TilePoint& a,
    const TilePoint& b,
    const TilePoint& c,
    const std::array<uint8_t, 3>& fill,
    float alpha,
    RasterTileStats& stats) {
    const float area = tileEdge(a, b, c);
    if (std::fabs(area) < 0.00001f) return;

    const float min_x_f = std::min({a.x, b.x, c.x});
    const float max_x_f = std::max({a.x, b.x, c.x});
    const float min_y_f = std::min({a.y, b.y, c.y});
    const float max_y_f = std::max({a.y, b.y, c.y});
    if (max_x_f < 0.0f || max_y_f < 0.0f ||
        min_x_f >= static_cast<float>(tile_size) ||
        min_y_f >= static_cast<float>(tile_size)) {
        return;
    }

    const int min_x = std::clamp(static_cast<int>(std::floor(min_x_f)), 0, tile_size - 1);
    const int max_x = std::clamp(static_cast<int>(std::ceil(max_x_f)), 0, tile_size - 1);
    const int min_y = std::clamp(static_cast<int>(std::floor(min_y_f)), 0, tile_size - 1);
    const int max_y = std::clamp(static_cast<int>(std::ceil(max_y_f)), 0, tile_size - 1);
    size_t pixels = 0;

    for (int py = min_y; py <= max_y; ++py) {
        for (int px = min_x; px <= max_x; ++px) {
            const TilePoint p{static_cast<float>(px) + 0.5f, static_cast<float>(py) + 0.5f};
            const float e0 = tileEdge(a, b, p);
            const float e1 = tileEdge(b, c, p);
            const float e2 = tileEdge(c, a, p);
            const bool inside = area > 0.0f
                ? (e0 >= 0.0f && e1 >= 0.0f && e2 >= 0.0f)
                : (e0 <= 0.0f && e1 <= 0.0f && e2 <= 0.0f);
            if (!inside) continue;

            const size_t offset = static_cast<size_t>((py * tile_size + px) * 3);
            for (int channel = 0; channel < 3; ++channel) {
                const float blended = static_cast<float>(fill[(size_t)channel]) * alpha +
                                      static_cast<float>(rgb[offset + (size_t)channel]) * (1.0f - alpha);
                rgb[offset + (size_t)channel] =
                    static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(blended)), 0, 255));
            }
            pixels += 1;
        }
    }

    if (pixels > 0) {
        stats.triangles_drawn += 1;
        stats.pixels_written += pixels;
    }
}

bool writePpmImage(const fs::path& path, int width, int height, const std::vector<uint8_t>& rgb) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return false;
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out << "P6\n" << width << ' ' << height << "\n255\n";
    out.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
    return static_cast<bool>(out);
}

int runRenderTileCacheSelftest(const fs::path& root) {
    RenderTileCacheKey key;
    key.layer_file = "baltimore county/parcels.geojson";
    key.render_route = layerRenderRouteArtifactName(LayerRenderRoute::ParcelPolygonGpu);
    key.source_signature = "123 / stale?";
    key.style_key = "parcel fill:v1";
    key.z = 15;
    key.x = 9409;
    key.y = 12505;
    const fs::path path = renderTileCachePath(root, key);
    const std::string s = path.string();
    const bool ok =
        s.find("render_tiles") != std::string::npos &&
        s.find("baltimore_county_parcels.geojson") != std::string::npos &&
        s.find("parcel_polygon_gpu") != std::string::npos &&
        s.find("source_123___stale_") != std::string::npos &&
        s.find("style_parcel_fill_v1") != std::string::npos &&
        s.find("/z15/9409_12505.ppm") != std::string::npos;
    json out = {
        {"mode", "render-tile-cache-selftest"},
        {"ok", ok},
        {"path", path.string()}
    };
    std::cout << out.dump(2) << '\n';
    return ok ? 0 : 1;
}

int renderPolygonTile(const fs::path& root, const WorldsimCliOptions& options) {
    const std::string& file = options.render_polygon_tile_file;
    if (file.empty() || !isBareLayerFilename(file)) {
        json out = {
            {"mode", "render-polygon-tile"},
            {"ok", false},
            {"error", file.empty() ? "missing layer file" : "expected bare layer filename"}
        };
        std::cout << out.dump(2) << '\n';
        return 1;
    }
    if (options.render_tile_z < 0 || options.render_tile_x < 0 || options.render_tile_y < 0 ||
        options.render_tile_z > 30) {
        json out = {
            {"mode", "render-polygon-tile"},
            {"file", file},
            {"ok", false},
            {"error", "expected non-negative z/x/y with z <= 30"}
        };
        std::cout << out.dump(2) << '\n';
        return 1;
    }

    const std::vector<LayerDef> layers = loadManifest(root);
    const LayerDef* layer = nullptr;
    for (const LayerDef& candidate : layers) {
        if (candidate.file == file) {
            layer = &candidate;
            break;
        }
    }
    if (!layer) {
        json out = {{"mode", "render-polygon-tile"}, {"file", file}, {"ok", false}, {"error", "layer file not found in manifest"}};
        std::cout << out.dump(2) << '\n';
        return 1;
    }
    if (layerUsesPointGeometry(*layer) || layerUsesPolylineGeometry(*layer)) {
        json out = {{"mode", "render-polygon-tile"}, {"file", file}, {"ok", false}, {"error", "layer is not polygon geometry"}};
        std::cout << out.dump(2) << '\n';
        return 1;
    }

    const fs::path layer_path = resolveStoredLayerPath(root, *layer);
    std::string sig;
    if (!resolveLayerSourceSignature(layer_path, sig, nullptr)) {
        json out = {{"mode", "render-polygon-tile"}, {"file", file}, {"ok", false}, {"layer_path", layer_path.string()}, {"error", "failed to resolve layer source signature"}};
        std::cout << out.dump(2) << '\n';
        return 1;
    }

    const LayerRenderRoute route = cliRenderRouteForLayer(root, layers, *layer);
    const char* route_name = layerRenderRouteArtifactName(route);
    const fs::path artifact_path =
        geometryArtifactCachePathForLayerFile(root, file, GeometryArtifactClass::Polygon, route_name);
    PolygonGeometryArtifact artifact;
    if (!loadBinaryPolygonGeometryArtifact(artifact_path, sig, artifact)) {
        json out = {
            {"mode", "render-polygon-tile"},
            {"file", file},
            {"ok", false},
            {"layer_path", layer_path.string()},
            {"artifact_path", artifact_path.string()},
            {"render_path", route_name},
            {"source_signature", sig},
            {"error", "compiled route-aware polygon artifact missing, stale, or invalid"}
        };
        std::cout << out.dump(2) << '\n';
        return 1;
    }

    constexpr int kTileSize = 256;
    std::vector<uint8_t> rgb(static_cast<size_t>(kTileSize * kTileSize * 3), 255);
    const AppSettings app_settings = loadAppSettings(root, AppSettings{});
    const std::array<uint8_t, 3> fill = {
        layerColorByte(layer->color.x, 110),
        layerColorByte(layer->color.y, 168),
        layerColorByte(layer->color.z, 104)
    };
    const float alpha = std::clamp(
        (layer->color.w > 0.0f ? layer->color.w : 0.58f) * std::clamp(app_settings.map_polygon_fill_opacity, 0.0f, 1.0f),
        0.05f,
        1.0f);
    RasterTileStats stats;

    for (size_t i = 0; i + 2 < artifact.fill_indices.size(); i += 3) {
        stats.triangles_considered += 1;
        const uint32_t ia = artifact.fill_indices[i + 0];
        const uint32_t ib = artifact.fill_indices[i + 1];
        const uint32_t ic = artifact.fill_indices[i + 2];
        if (ia >= artifact.vertices.size() || ib >= artifact.vertices.size() || ic >= artifact.vertices.size()) {
            continue;
        }
        const ImVec2& va = artifact.vertices[ia];
        const ImVec2& vb = artifact.vertices[ib];
        const ImVec2& vc = artifact.vertices[ic];
        const TilePoint a = lonLatToTilePixel(va.x, va.y, options.render_tile_z, options.render_tile_x, options.render_tile_y, kTileSize);
        const TilePoint b = lonLatToTilePixel(vb.x, vb.y, options.render_tile_z, options.render_tile_x, options.render_tile_y, kTileSize);
        const TilePoint c = lonLatToTilePixel(vc.x, vc.y, options.render_tile_z, options.render_tile_x, options.render_tile_y, kTileSize);
        fillRasterTriangle(rgb, kTileSize, a, b, c, fill, alpha, stats);
    }

    const std::string style_key = renderPolygonFillStyleKey(*layer, app_settings.map_polygon_fill_opacity);
    RenderTileCacheKey key;
    key.layer_file = file;
    key.render_route = route_name;
    key.source_signature = sig;
    key.style_key = style_key;
    key.z = options.render_tile_z;
    key.x = options.render_tile_x;
    key.y = options.render_tile_y;
    key.extension = "ppm";
    const fs::path output_path = renderTileCachePath(root, key);
    const bool written = writePpmImage(output_path, kTileSize, kTileSize, rgb);
    json out = {
        {"mode", "render-polygon-tile"},
        {"file", file},
        {"ok", written},
        {"derived_cache", true},
        {"artifact_validated", true},
        {"layer_path", layer_path.string()},
        {"artifact_path", artifact_path.string()},
        {"artifact_source_signature", sig},
        {"output_path", output_path.string()},
        {"tile_path", output_path.string()},
        {"render_path", route_name},
        {"source_signature", sig},
        {"style_key", style_key},
        {"cache_key", {
            {"layer_file", key.layer_file},
            {"render_route", key.render_route},
            {"source_signature", key.source_signature},
            {"style_key", key.style_key},
            {"z", key.z},
            {"x", key.x},
            {"y", key.y},
            {"extension", key.extension}
        }},
        {"tile", {{"z", options.render_tile_z}, {"x", options.render_tile_x}, {"y", options.render_tile_y}}},
        {"vertices", artifact.vertices.size()},
        {"fill_indices", artifact.fill_indices.size()},
        {"triangles_considered", stats.triangles_considered},
        {"triangles_drawn", stats.triangles_drawn},
        {"pixels_written", stats.pixels_written}
    };
    if (!written) out["error"] = "failed to write render tile";
    std::cout << out.dump(2) << '\n';
    return written ? 0 : 1;
}

int runCanonicalParcelBinarySelftest(const fs::path& root) {
    std::vector<LayerDef::FeatureRecord> features;
    std::vector<LayerDef::FeatureProperties> feature_properties;
    LayerDef::FeatureRecord polygon;
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
    features.push_back(std::move(polygon));
    feature_properties.push_back({{{"regional_parcel_id", "BaltimoreCity:TEST123"}, {"owner", "Canonical Parcel Test"}}});

    const fs::path test_dir = root / "data" / "cache" / "selftest";
    const fs::path cache_path = test_dir / "parcel.geojson.canonical.bin";
    const fs::path resolver_canonical_path = test_dir / "parcel.geojson.canonical.bin";
    const std::string sig = "canonical_selftest_sig";
    saveBinaryCanonicalFeatureCollection(cache_path, sig, features, &feature_properties);

    CanonicalFeatureCollectionMetadata meta;
    const bool meta_ok = loadBinaryCanonicalMetadata(cache_path, meta);

    std::vector<LayerDef::FeatureRecord> loaded;
    std::vector<LayerDef::FeatureProperties> loaded_properties;
    const bool loaded_ok = loadBinaryCanonicalFeatureCollection(cache_path, sig, loaded, &loaded_properties);
    const bool stale_rejected = !loadBinaryCanonicalFeatureCollection(cache_path, "wrong_signature", loaded);

    std::string resolved_sig;
    std::string resolved_kind;
    const bool resolver_loads_canonical =
        resolveLayerSourceSignature(test_dir / "parcel.geojson", resolved_sig, &resolved_kind) &&
        resolved_sig == sig &&
        resolved_kind == "canonical_binary";

    const bool roundtrip_ok =
        loaded_ok &&
        loaded.size() == 1 &&
        loaded[0].rings.size() == 1 &&
        loaded[0].rings[0].size() == 4 &&
        loaded_properties.size() == 1 &&
        loaded_properties[0].values.size() == 2 &&
        loaded_properties[0].values[0].first == "regional_parcel_id" &&
        loaded_properties[0].values[0].second == "BaltimoreCity:TEST123" &&
        nearlyEqual(loaded[0].extent.max_lat, 39.3f);

    std::error_code ec;
    fs::remove(cache_path, ec);
    fs::remove(resolver_canonical_path, ec);
    fs::remove(test_dir, ec);

    const bool ok =
        meta_ok &&
        meta.version == 5 &&
        meta.endian_marker == 0x01020304u &&
        meta.feature_count == 1 &&
        meta.source_signature == sig &&
        roundtrip_ok &&
        stale_rejected &&
        resolver_loads_canonical;

    json out = {
        {"mode", "canonical-parcel-binary-selftest"},
        {"ok", ok},
        {"metadata_loaded", meta_ok},
        {"feature_count", meta.feature_count},
        {"source_signature", meta.source_signature},
        {"roundtrip_ok", roundtrip_ok},
        {"stale_signature_rejected", stale_rejected},
        {"resolver_loads_canonical", resolver_loads_canonical}
    };
    std::cout << out.dump(2) << '\n';
    return ok ? 0 : 1;
}

int runParcelPolygonIdentitySelftest() {
    PolygonGeometryArtifact valid_artifact;
    valid_artifact.header.version = kPolygonGeometryArtifactVersion;
    valid_artifact.header.geometry_class = GeometryArtifactClass::Polygon;
    valid_artifact.header.feature_count = 1;
    valid_artifact.header.chunk_count = 1;
    valid_artifact.header.source_signature = "parcel_polygon_identity_selftest_sig";
    valid_artifact.vertices = {
        ImVec2(-76.70f, 39.20f),
        ImVec2(-76.69f, 39.20f),
        ImVec2(-76.69f, 39.21f),
        ImVec2(-76.70f, 39.21f)
    };
    valid_artifact.feature_refs = {0, 0, 0, 0};
    valid_artifact.fill_indices = {0, 1, 2, 0, 2, 3};
    valid_artifact.line_indices = {0, 1, 1, 2, 2, 3, 3, 0};
    GeometryArtifactFeatureRecord valid_feature;
    valid_feature.feature_idx = 0;
    valid_feature.entity_id = "entity:parcel:test-1";
    valid_feature.geometry_entity_id = "geometry:parcel:test-1";
    valid_feature.source_feature_id = "parcel:test-1";
    valid_feature.source_primary_key = "test-1";
    valid_feature.vertex_offset = 0;
    valid_feature.vertex_count = 4;
    valid_feature.index_offset = 0;
    valid_feature.index_count = 6;
    valid_feature.aux_index_offset = 0;
    valid_feature.aux_index_count = 8;
    valid_feature.min_lon = -76.70f;
    valid_feature.min_lat = 39.20f;
    valid_feature.max_lon = -76.69f;
    valid_feature.max_lat = 39.21f;
    valid_artifact.features.push_back(valid_feature);
    GeometryArtifactChunkRecord valid_chunk;
    valid_chunk.chunk_idx = 0;
    valid_chunk.feature_offset = 0;
    valid_chunk.feature_count = 1;
    valid_chunk.vertex_offset = 0;
    valid_chunk.vertex_count = 4;
    valid_chunk.index_offset = 0;
    valid_chunk.index_count = 6;
    valid_chunk.aux_index_offset = 0;
    valid_chunk.aux_index_count = 8;
    valid_chunk.min_lon = -76.70f;
    valid_chunk.min_lat = 39.20f;
    valid_chunk.max_lon = -76.69f;
    valid_chunk.max_lat = 39.21f;
    valid_artifact.chunks.push_back(valid_chunk);

    ParcelRenderCacheBlob valid_blob;
    std::string valid_error;
    const bool valid_ok =
        buildParcelRenderCacheBlobFromPolygonArtifact(valid_artifact, valid_blob, &valid_error) &&
        valid_error.empty() &&
        valid_blob.features.size() == 1 &&
        valid_blob.features[0].entity_id == valid_feature.entity_id &&
        valid_blob.features[0].geometry_entity_id == valid_feature.geometry_entity_id &&
        valid_blob.features[0].source_feature_id == valid_feature.source_feature_id &&
        valid_blob.features[0].source_primary_key == valid_feature.source_primary_key;

    PolygonGeometryArtifact invalid_artifact = valid_artifact;
    invalid_artifact.features[0].entity_id.clear();
    ParcelRenderCacheBlob invalid_blob;
    std::string invalid_error;
    const bool invalid_rejected =
        !buildParcelRenderCacheBlobFromPolygonArtifact(invalid_artifact, invalid_blob, &invalid_error) &&
        invalid_blob.features.empty() &&
        invalid_error.find("missing entity_id") != std::string::npos;

    const bool ok = valid_ok && invalid_rejected;
    json out = {
        {"mode", "parcel-polygon-identity-selftest"},
        {"ok", ok},
        {"valid_ok", valid_ok},
        {"valid_feature_count", valid_blob.features.size()},
        {"invalid_rejected", invalid_rejected},
        {"invalid_error", invalid_error}
    };
    std::cout << out.dump(2) << '\n';
    return ok ? 0 : 1;
}

int runPolygonArtifactResilienceSelftest() {
    std::error_code ec;
    const fs::path test_root = fs::temp_directory_path(ec) / "worldsim3_polygon_artifact_resilience_selftest";
    fs::create_directories(test_root / "data" / "cache" / "geometry", ec);

    LayerDef layer;
    layer.file = "polygon_artifact_resilience.geojson";
    layer.logical_id = "polygon_artifact_resilience";

    LayerDef::FeatureRecord feature;
    feature.extent.min_lon = -76.7000f;
    feature.extent.min_lat = 39.2000f;
    feature.extent.max_lon = -76.6990f;
    feature.extent.max_lat = 39.2010f;
    feature.rings = {{
        ImVec2(-76.7000f, 39.2000f),
        ImVec2(-76.6990f, 39.2000f),
        ImVec2(-76.6990f, 39.2010f),
        ImVec2(-76.7000f, 39.2010f),
        ImVec2(-76.7000f, 39.2000f)
    }};
    feature.triangles = {0, 1, 2, 0, 2, 2};
    layer.features.push_back(feature);

    const std::string sig = "polygon_artifact_resilience_selftest_sig";
    PolygonGeometryArtifact rebuilt_artifact;
    const bool build_ok = buildPolygonGeometryArtifact(layer, layer.features, sig, rebuilt_artifact, 64);
    bool rebuilt_indices_in_range = true;
    for (uint32_t idx : rebuilt_artifact.fill_indices) {
        if (idx >= 4) {
            rebuilt_indices_in_range = false;
            break;
        }
    }
    const bool triangles_rebuilt =
        build_ok &&
        rebuilt_artifact.fill_indices.size() == 6 &&
        rebuilt_indices_in_range &&
        rebuilt_artifact.fill_indices != layer.features[0].triangles;

    const fs::path artifact_path = geometryArtifactCachePathForLayerFile(
        test_root,
        layer.file,
        GeometryArtifactClass::Polygon,
        layerRenderRouteArtifactName(LayerRenderRoute::GenericPolygonGpu));
    saveBinaryPolygonGeometryArtifact(artifact_path, rebuilt_artifact);

    PolygonGeometryArtifact loaded_artifact;
    const bool roundtrip_ok = loadBinaryPolygonGeometryArtifact(artifact_path, sig, loaded_artifact);

    PolygonGeometryArtifact invalid_artifact = rebuilt_artifact;
    invalid_artifact.features[0].vertex_count = 99;
    saveBinaryPolygonGeometryArtifact(artifact_path, invalid_artifact);
    PolygonGeometryArtifact invalid_loaded_artifact;
    const bool invalid_rejected = !loadBinaryPolygonGeometryArtifact(artifact_path, sig, invalid_loaded_artifact);

    fs::remove(artifact_path, ec);
    fs::remove(test_root / "data" / "cache" / "geometry", ec);
    fs::remove(test_root / "data" / "cache", ec);
    fs::remove(test_root / "data", ec);
    fs::remove(test_root, ec);

    const bool ok = build_ok && triangles_rebuilt && roundtrip_ok && invalid_rejected;
    std::cout << json{
        {"mode", "polygon-artifact-resilience-selftest"},
        {"ok", ok},
        {"build_ok", build_ok},
        {"triangles_rebuilt", triangles_rebuilt},
        {"roundtrip_ok", roundtrip_ok},
        {"invalid_rejected", invalid_rejected}
    }.dump(2) << '\n';
    return ok ? 0 : 1;
}

int runParcelPolygonFeatureIdxSelftest() {
    std::error_code ec;
    const fs::path test_root = fs::temp_directory_path(ec) / "worldsim3_parcel_polygon_feature_idx_selftest";
    fs::create_directories(test_root / "data" / "cache" / "geometry", ec);

    LayerDef layer;
    layer.file = "parcel_polygon_feature_idx_selftest.geojson";
    layer.logical_id = "parcel_polygon_feature_idx_selftest";
    layer.scale = "parcel";
    layer.duckdb_role = "parcel_record";

    LayerDef::FeatureRecord feature_a;
    feature_a.extent.min_lon = -76.7000f;
    feature_a.extent.min_lat = 39.2000f;
    feature_a.extent.max_lon = -76.6990f;
    feature_a.extent.max_lat = 39.2010f;
    feature_a.rings = {{
        ImVec2(-76.7000f, 39.2000f),
        ImVec2(-76.6990f, 39.2000f),
        ImVec2(-76.6990f, 39.2010f),
        ImVec2(-76.7000f, 39.2010f),
        ImVec2(-76.7000f, 39.2000f)
    }};
    ensureFeatureTriangles(feature_a);

    LayerDef::FeatureRecord feature_b = feature_a;
    feature_b.extent.min_lon = -76.6985f;
    feature_b.extent.max_lon = -76.6975f;
    for (ImVec2& p : feature_b.rings[0]) p.x += 0.0015f;
    ensureFeatureTriangles(feature_b);

    layer.features.push_back(feature_a);
    layer.features.push_back(feature_b);

    const std::string sig = "parcel_polygon_feature_idx_selftest_sig";
    PolygonGeometryArtifact valid_artifact;
    const bool build_ok = buildPolygonGeometryArtifact(layer, layer.features, sig, valid_artifact, 64);
    const fs::path artifact_path = geometryArtifactCachePathForLayerFile(
        test_root,
        layer.file,
        GeometryArtifactClass::Polygon,
        layerRenderRouteArtifactName(LayerRenderRoute::ParcelPolygonGpu));

    bool valid_indices_ok = build_ok && valid_artifact.features.size() == 2;
    if (valid_indices_ok) {
        for (size_t i = 0; i < valid_artifact.features.size(); ++i) {
            if (valid_artifact.features[i].feature_idx != i) {
                valid_indices_ok = false;
                break;
            }
        }
    }

    if (build_ok) saveBinaryPolygonGeometryArtifact(artifact_path, valid_artifact);
    PolygonGeometryArtifact roundtrip_artifact;
    const bool roundtrip_ok = build_ok && loadBinaryPolygonGeometryArtifact(artifact_path, sig, roundtrip_artifact);

    PolygonGeometryArtifact invalid_artifact = valid_artifact;
    if (!invalid_artifact.features.empty()) {
        invalid_artifact.features[0].feature_idx = 9999u;
    }
    saveBinaryPolygonGeometryArtifact(artifact_path, invalid_artifact);
    PolygonGeometryArtifact invalid_loaded_artifact;
    const bool invalid_rejected = !loadBinaryPolygonGeometryArtifact(artifact_path, sig, invalid_loaded_artifact);

    fs::remove(artifact_path, ec);
    fs::remove(test_root / "data" / "cache" / "geometry", ec);
    fs::remove(test_root / "data" / "cache", ec);
    fs::remove(test_root / "data", ec);
    fs::remove(test_root, ec);

    const bool ok = build_ok && valid_indices_ok && roundtrip_ok && invalid_rejected;
    std::cout << json{
        {"mode", "parcel-polygon-feature-idx-selftest"},
        {"ok", ok},
        {"build_ok", build_ok},
        {"valid_indices_ok", valid_indices_ok},
        {"roundtrip_ok", roundtrip_ok},
        {"invalid_rejected", invalid_rejected}
    }.dump(2) << '\n';
    return ok ? 0 : 1;
}

int runParcelSelectionUiHarness() {
    IMGUI_CHECKVERSION();
    ImGuiContext* imgui = ImGui::CreateContext();
    if (!imgui) {
        std::cout << json{
            {"mode", "parcel-selection-ui-harness"},
            {"ok", false},
            {"error", "failed to create ImGui context"}
        }.dump(2) << '\n';
        return 1;
    }

    int exit_code = 1;
    try {
        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->AddFontDefault();
        unsigned char* font_pixels = nullptr;
        int font_width = 0;
        int font_height = 0;
        io.Fonts->GetTexDataAsRGBA32(&font_pixels, &font_width, &font_height);
        io.DisplaySize = ImVec2(1024.0f, 768.0f);
        ImGui::NewFrame();
        ImDrawList* draw = ImGui::GetBackgroundDrawList();
        if (!draw) {
            std::cout << json{
                {"mode", "parcel-selection-ui-harness"},
                {"ok", false},
                {"error", "failed to acquire ImGui background draw list"}
            }.dump(2) << '\n';
            ImGui::EndFrame();
            ImGui::DestroyContext(imgui);
            return 1;
        }

        std::vector<LayerDef> layers(2);
        layers[0].file = "parcel.geojson";
        layers[0].name = "Primary Parcels";
        layers[0].enabled = true;
        layers[1].file = "baltimore_county_parcels.geojson";
        layers[1].name = "Baltimore County Parcels";
        layers[1].enabled = true;

        ParcelRenderCacheBlob parcel_blob;
        parcel_blob.source_signature = "parcel_selection_ui_harness_sig";
        parcel_blob.vertices = {
            ImVec2(-76.7000f, 39.2000f),
            ImVec2(-76.6990f, 39.2000f),
            ImVec2(-76.6990f, 39.2010f),
            ImVec2(-76.7000f, 39.2010f),
            ImVec2(-76.6985f, 39.2000f),
            ImVec2(-76.6975f, 39.2000f),
            ImVec2(-76.6975f, 39.2010f),
            ImVec2(-76.6985f, 39.2010f)
        };
        parcel_blob.indices = {
            0, 1, 2, 0, 2, 3,
            4, 5, 6, 4, 6, 7
        };
        parcel_blob.line_indices = {
            0, 1, 1, 2, 2, 3, 3, 0,
            4, 5, 5, 6, 6, 7, 7, 4
        };
        parcel_blob.features = {
            ParcelRenderFeatureRecord{
                0,
                "entity:parcel:selection-primary",
                "geometry:parcel:selection-primary:0",
                "source:parcel:selection-primary:0",
                "pk:parcel:selection-primary:0",
                0, 4, 0, 6, 0, 8,
                -76.7000f, 39.2000f, -76.6990f, 39.2010f
            },
            ParcelRenderFeatureRecord{
                1,
                "entity:parcel:selection-primary",
                "geometry:parcel:selection-primary:1",
                "source:parcel:selection-primary:1",
                "pk:parcel:selection-primary:1",
                4, 4, 6, 6, 8, 8,
                -76.6985f, 39.2000f, -76.6975f, 39.2010f
            }
        };

        PolygonGeometryArtifact county_artifact;
        county_artifact.header.version = kPolygonGeometryArtifactVersion;
        county_artifact.header.geometry_class = GeometryArtifactClass::Polygon;
        county_artifact.header.feature_count = 2;
        county_artifact.header.chunk_count = 1;
        county_artifact.header.source_signature = "parcel_selection_ui_harness_sig";
        county_artifact.vertices = {
            ImVec2(-76.6960f, 39.2000f),
            ImVec2(-76.6950f, 39.2000f),
            ImVec2(-76.6950f, 39.2010f),
            ImVec2(-76.6960f, 39.2010f),
            ImVec2(-76.6945f, 39.2000f),
            ImVec2(-76.6935f, 39.2000f),
            ImVec2(-76.6935f, 39.2010f),
            ImVec2(-76.6945f, 39.2010f)
        };
        county_artifact.fill_indices = {
            0, 1, 2, 0, 2, 3,
            4, 5, 6, 4, 6, 7
        };
        county_artifact.line_indices = {
            0, 1, 1, 2, 2, 3, 3, 0,
            4, 5, 5, 6, 6, 7, 7, 4
        };
        county_artifact.features = {
            GeometryArtifactFeatureRecord{
                0,
                "entity:parcel:selection-county",
                "geometry:parcel:selection-county:0",
                "source:parcel:selection-county:0",
                "pk:parcel:selection-county:0",
                0, 4, 0, 6, 0, 8,
                -76.6960f, 39.2000f, -76.6950f, 39.2010f
            },
            GeometryArtifactFeatureRecord{
                1,
                "entity:parcel:selection-county",
                "geometry:parcel:selection-county:1",
                "source:parcel:selection-county:1",
                "pk:parcel:selection-county:1",
                4, 4, 6, 6, 8, 8,
                -76.6945f, 39.2000f, -76.6935f, 39.2010f
            }
        };
        county_artifact.chunks = {
            GeometryArtifactChunkRecord{
                0, 0, 2, 0, 8, 0, 12, 0, 16,
                -76.6960f, 39.2000f, -76.6935f, 39.2010f
            }
        };
        std::unordered_map<size_t, PolygonGeometryArtifact> polygon_artifacts;
        polygon_artifacts.emplace(1u, county_artifact);

        ParcelSelectionState selection;
        const auto identity_project = [](const ImVec2& world) { return world; };

        MapSelectionRenderContext ctx;
        ctx.draw = draw;
        ctx.origin = ImVec2(0.0f, 0.0f);
        ctx.size = ImVec2(1024.0f, 768.0f);
        ctx.layers = &layers;
        ctx.polygon_geometry_artifacts = &polygon_artifacts;
        ctx.parcel_render_blob = &parcel_blob;
        ctx.parcel_layer_idx = 0;
        ctx.parcel_selection = &selection;
        ctx.math_zoom = 16;
        ctx.projection = nullptr;
        ctx.project_world = identity_project;

        const int baseline_vtx = draw->VtxBuffer.Size;
        const int baseline_idx = draw->IdxBuffer.Size;
        const bool primary_selected = selectParcel(selection, 0, "entity:parcel:selection-primary", false);
        renderSelectedParcelOutlines(ctx);
        const int primary_vtx = draw->VtxBuffer.Size - baseline_vtx;
        const int primary_idx = draw->IdxBuffer.Size - baseline_idx;

        clearParcelSelection(selection);
        const int before_county_vtx = draw->VtxBuffer.Size;
        const int before_county_idx = draw->IdxBuffer.Size;
        const bool county_selected = selectParcel(selection, 1, "entity:parcel:selection-county", false);
        renderSelectedParcelOutlines(ctx);
        const int county_vtx = draw->VtxBuffer.Size - before_county_vtx;
        const int county_idx = draw->IdxBuffer.Size - before_county_idx;

        clearParcelSelection(selection);
        const int before_missing_vtx = draw->VtxBuffer.Size;
        const int before_missing_idx = draw->IdxBuffer.Size;
        const bool missing_selected = selectParcel(selection, 1, "entity:parcel:missing", false);
        renderSelectedParcelOutlines(ctx);
        const int missing_vtx = draw->VtxBuffer.Size - before_missing_vtx;
        const int missing_idx = draw->IdxBuffer.Size - before_missing_idx;

        const bool primary_drew_both_geometries = primary_selected && primary_vtx >= 24 && primary_idx >= 72;
        const bool county_drew_both_geometries = county_selected && county_vtx >= 24 && county_idx >= 72;
        const bool missing_drew_nothing = missing_selected && missing_vtx == 0 && missing_idx == 0;
        const bool ok = primary_drew_both_geometries && county_drew_both_geometries && missing_drew_nothing;

        json out = {
            {"mode", "parcel-selection-ui-harness"},
            {"ok", ok},
            {"primary_selected", primary_selected},
            {"primary_vertices_drawn", primary_vtx},
            {"primary_indices_drawn", primary_idx},
            {"county_selected", county_selected},
            {"county_vertices_drawn", county_vtx},
            {"county_indices_drawn", county_idx},
            {"missing_selected", missing_selected},
            {"missing_vertices_drawn", missing_vtx},
            {"missing_indices_drawn", missing_idx}
        };
        std::cout << out.dump(2) << '\n';
        ImGui::EndFrame();
        exit_code = ok ? 0 : 1;
    } catch (const std::exception& ex) {
        ImGui::EndFrame();
        std::cout << json{
            {"mode", "parcel-selection-ui-harness"},
            {"ok", false},
            {"error", ex.what()}
        }.dump(2) << '\n';
        exit_code = 1;
    }

    ImGui::DestroyContext(imgui);
    return exit_code;
}

int runDuckDbParcelSemanticSnapshotSelftest(const fs::path& root) {
    constexpr const char* kMode = "duckdb-parcel-semantic-snapshot-selftest";
    const fs::path test_root = root / "data" / "cache" / "selftest" / "duckdb_parcel_semantic_snapshot";
    std::error_code ec;
    fs::remove_all(test_root, ec);
    fs::create_directories(test_root / "data", ec);
    const fs::path db_path = test_root / "data" / "worldsim.duckdb";

    try {
        duckdb::DuckDB db(db_path.string());
        duckdb::Connection con(db);
        auto exec = [&](const char* sql) {
            auto res = con.Query(sql);
            if (!res || res->HasError()) {
                throw std::runtime_error(res ? res->GetError() : "query failed");
            }
        };
        exec("CREATE TABLE analytics_build_info(source_signature VARCHAR)");
        exec("INSERT INTO analytics_build_info VALUES ('snapshot_sig_v1')");
        exec(R"SQL(
            CREATE TABLE layer_features (
                layer_idx UBIGINT,
                layer_name VARCHAR,
                layer_file VARCHAR,
                duckdb_role VARCHAR,
                feature_idx UBIGINT,
                entity_id VARCHAR,
                scale VARCHAR,
                category VARCHAR,
                provenance_world VARCHAR,
                provenance_nation_state VARCHAR,
                provenance_state_region VARCHAR,
                provenance_county_city VARCHAR,
                min_lon DOUBLE,
                min_lat DOUBLE,
                max_lon DOUBLE,
                max_lat DOUBLE,
                blocklot VARCHAR,
                owner VARCHAR,
                address VARCHAR,
                zipcode VARCHAR,
                status VARCHAR,
                zoning VARCHAR,
                jurisdiction VARCHAR,
                value_usd DOUBLE,
                structure_area_sqft DOUBLE,
                feature_name VARCHAR,
                lga_name VARCHAR,
                ward_name VARCHAR,
                source_name VARCHAR,
                event_date_text VARCHAR,
                event_status_hint VARCHAR,
                event_title_hint VARCHAR,
                event_detail_hint VARCHAR,
                event_metadata_json VARCHAR,
                event_year_hint INTEGER,
                amount_usd_hint DOUBLE
            )
        )SQL");
        exec(R"SQL(
            CREATE TABLE layer_feature_properties (
                layer_idx UBIGINT,
                layer_name VARCHAR,
                layer_file VARCHAR,
                duckdb_role VARCHAR,
                feature_idx UBIGINT,
                entity_id VARCHAR,
                property_key VARCHAR,
                property_value VARCHAR
            )
        )SQL");
        exec(R"SQL(
            CREATE TABLE unified_parcels (
                parcel_layer_idx UBIGINT,
                parcel_entity_id VARCHAR,
                parcel_geometry_entity_id VARCHAR,
                blocklot VARCHAR,
                parcel_source_file VARCHAR,
                property_source_file VARCHAR,
                parcel_has_geometry BOOLEAN,
                has_property_record BOOLEAN,
                owner VARCHAR,
                owner_display VARCHAR,
                address VARCHAR,
                address_search VARCHAR,
                zipcode VARCHAR,
                status VARCHAR,
                current_land DOUBLE,
                current_improvements DOUBLE,
                structure_area_sqft DOUBLE,
                tax_base DOUBLE,
                sale_price DOUBLE,
                current_value DOUBLE,
                vacant_notice_count INTEGER,
                vacant_rehab_count INTEGER,
                tax_lien_count INTEGER,
                tax_sale_count INTEGER,
                tax_lien_amount DOUBLE,
                tax_sale_amount DOUBLE,
                min_lon DOUBLE,
                min_lat DOUBLE,
                max_lon DOUBLE,
                max_lat DOUBLE
            )
        )SQL");
        exec("CREATE TABLE parcel_events(blocklot VARCHAR, event_date VARCHAR, event_type VARCHAR, event_label VARCHAR, event_title VARCHAR, event_detail VARCHAR, event_metadata_json VARCHAR, event_status VARCHAR, amount_usd DOUBLE, source_layer_name VARCHAR, source_layer_file VARCHAR)");
        exec("CREATE TABLE layer_feature_bboxes(layer_idx UBIGINT, layer_file VARCHAR, layer_name VARCHAR, duckdb_role VARCHAR, feature_idx UBIGINT, entity_id VARCHAR, scale VARCHAR, category VARCHAR, provenance_world VARCHAR, provenance_nation_state VARCHAR, provenance_state_region VARCHAR, provenance_county_city VARCHAR, min_lon DOUBLE, min_lat DOUBLE, max_lon DOUBLE, max_lat DOUBLE, center_lon DOUBLE, center_lat DOUBLE)");
        exec("CREATE TABLE layer_bboxes(layer_idx UBIGINT, layer_file VARCHAR, layer_name VARCHAR, duckdb_role VARCHAR, scale VARCHAR, category VARCHAR, provenance_world VARCHAR, provenance_nation_state VARCHAR, provenance_state_region VARCHAR, provenance_county_city VARCHAR, feature_count UBIGINT, min_lon DOUBLE, min_lat DOUBLE, max_lon DOUBLE, max_lat DOUBLE)");
        exec("CREATE TABLE parcel_zone_memberships(parcel_layer_idx UBIGINT, parcel_entity_id VARCHAR, parcel_geometry_entity_id VARCHAR, blocklot VARCHAR, zone_layer_idx UBIGINT, zone_layer_file VARCHAR, zone_layer_name VARCHAR, zone_feature_idx UBIGINT, zone_entity_id VARCHAR, zone_key VARCHAR, zone_label VARCHAR, relation VARCHAR, parcel_centroid_lon DOUBLE, parcel_centroid_lat DOUBLE, overlap_area DOUBLE, overlap_ratio DOUBLE, source_signature VARCHAR)");
        exec(R"SQL(
            INSERT INTO layer_features VALUES
            (9, 'Parcels', 'parcel.geojson', 'parcel_record', 0, 'entity:a', 'parcel', 'Housing', '', '', '', '', -76.70, 39.20, -76.69, 39.21, 'BLK1', 'owner a', '1 Main', '21201', 'ACTIVE', '', '', 100000, 1200, '', '', '', '', '', '', '', '', '', 0, 0),
            (9, 'Parcels', 'parcel.geojson', 'parcel_record', 1, 'entity:b', 'parcel', 'Housing', '', '', '', '', -76.68, 39.20, -76.67, 39.21, 'BLK2', 'owner b', '2 Main', '21202', 'ACTIVE', '', '', 200000, 1500, '', '', '', '', '', '', '', '', '', 0, 0)
        )SQL");
        exec(R"SQL(
            INSERT INTO unified_parcels VALUES
            (9, 'entity:a', 'geom:a', 'BLK1', 'parcel.geojson', 'rp.geojson', TRUE, TRUE, 'owner a', 'Owner A', '1 Main', '1main', '21201', 'ACTIVE', 1000, 2000, 1200, 300000, 0, 300000, 2, 1, 3, 0, 4500, 0, -76.70, 39.20, -76.69, 39.21),
            (9, 'entity:b', 'geom:b', 'BLK2', 'parcel.geojson', '', TRUE, FALSE, 'owner b', 'Owner B', '2 Main', '2main', '21202', 'ACTIVE', 0, 0, 1500, 200000, 0, 200000, 0, 0, 0, 1, 0, 1200, -76.68, 39.20, -76.67, 39.21)
        )SQL");

        DuckDbAnalytics analytics(test_root);
        const bool cache_ok = analytics.status().last_rebuild_ok;
        const DuckDbParcelSemanticSnapshot snapshot = analytics.loadParcelSemanticSnapshot(9);
        const bool canonical_owner_ok =
            canonicalOwnerName("Mayor and City Council of Baltimore") == "mayor city council baltimore" &&
            canonicalOwnerName("Mayor City Council of Baltimore") == "mayor city council baltimore";
        const bool ok =
            canonical_owner_ok &&
            cache_ok &&
            snapshot.ok &&
            snapshot.source_signature == "snapshot_sig_v1" &&
            snapshot.unified_parcels.size() == 2 &&
            snapshot.parcel_blocklot_by_feature.size() == 2 &&
            snapshot.parcel_blocklot_by_feature[0] == "BLK1" &&
            snapshot.parcel_vac_notice_by_feature[0] == 2 &&
            snapshot.parcel_tax_lien_by_feature[0] == 3 &&
            snapshot.parcel_tax_sale_by_feature[1] == 1 &&
            snapshot.parcel_tax_sale_amount_by_feature[1] == 1200.0 &&
            snapshot.parcel_owner_search_by_feature[0] == "owner a" &&
            snapshot.parcel_address_search_by_feature[1] == "2main" &&
            snapshot.unified_parcels[0].parcel_geometry_entity_id == "geom:a" &&
            snapshot.unified_parcels[0].has_property_record &&
            snapshot.unified_parcels[1].blocklot == "BLK2";

        std::cout << json{
            {"mode", kMode},
            {"ok", ok},
            {"canonical_owner_ok", canonical_owner_ok},
            {"cache_ok", cache_ok},
            {"snapshot_ok", snapshot.ok},
            {"source_signature", snapshot.source_signature},
            {"feature_count", snapshot.unified_parcels.size()}
        }.dump(2) << '\n';
        return ok ? 0 : 1;
    } catch (const std::exception& ex) {
        std::cout << json{
            {"mode", kMode},
            {"ok", false},
            {"error", ex.what()}
        }.dump(2) << '\n';
        return 1;
    }
}

int runParcelHoverClickUiHarness(const fs::path& root) {
    constexpr const char* kMode = "parcel-hover-click-ui-harness";
    const fs::path test_root = root / "data" / "cache" / "selftest" / "parcel_hover_click";
    std::error_code ec;
    fs::remove_all(test_root, ec);
    fs::create_directories(test_root / "data", ec);
    const fs::path db_path = test_root / "data" / "worldsim.duckdb";

    try {
        duckdb::DuckDB db(db_path.string());
        duckdb::Connection con(db);
        auto exec = [&](const char* sql) {
            auto res = con.Query(sql);
            if (!res || res->HasError()) {
                throw std::runtime_error(res ? res->GetError() : "query failed");
            }
        };
        exec("CREATE TABLE layer_features(layer_idx UBIGINT, layer_name VARCHAR, layer_file VARCHAR, duckdb_role VARCHAR, feature_idx UBIGINT, entity_id VARCHAR, scale VARCHAR, category VARCHAR, provenance_world VARCHAR, provenance_nation_state VARCHAR, provenance_state_region VARCHAR, provenance_county_city VARCHAR, min_lon DOUBLE, min_lat DOUBLE, max_lon DOUBLE, max_lat DOUBLE, blocklot VARCHAR, owner VARCHAR, address VARCHAR, zipcode VARCHAR, status VARCHAR, zoning VARCHAR, jurisdiction VARCHAR, value_usd DOUBLE, structure_area_sqft DOUBLE, feature_name VARCHAR, lga_name VARCHAR, ward_name VARCHAR, source_name VARCHAR, event_date_text VARCHAR, event_status_hint VARCHAR, event_title_hint VARCHAR, event_detail_hint VARCHAR, event_metadata_json VARCHAR, event_year_hint INTEGER, amount_usd_hint DOUBLE)");
        exec("CREATE TABLE layer_feature_properties(layer_idx UBIGINT, layer_name VARCHAR, layer_file VARCHAR, duckdb_role VARCHAR, feature_idx UBIGINT, entity_id VARCHAR, property_key VARCHAR, property_value VARCHAR)");
        exec("CREATE TABLE unified_parcels(parcel_layer_idx UBIGINT, parcel_entity_id VARCHAR, parcel_geometry_entity_id VARCHAR, blocklot VARCHAR, parcel_source_file VARCHAR, property_source_file VARCHAR, parcel_has_geometry BOOLEAN, has_property_record BOOLEAN, owner VARCHAR, owner_display VARCHAR, address VARCHAR, address_search VARCHAR, zipcode VARCHAR, status VARCHAR, current_land DOUBLE, current_improvements DOUBLE, structure_area_sqft DOUBLE, tax_base DOUBLE, sale_price DOUBLE, current_value DOUBLE, vacant_notice_count INTEGER, vacant_rehab_count INTEGER, tax_lien_count INTEGER, tax_sale_count INTEGER, tax_lien_amount DOUBLE, tax_sale_amount DOUBLE, min_lon DOUBLE, min_lat DOUBLE, max_lon DOUBLE, max_lat DOUBLE)");
        exec("CREATE TABLE parcel_events(blocklot VARCHAR, event_date VARCHAR, event_type VARCHAR, event_label VARCHAR, event_title VARCHAR, event_detail VARCHAR, event_metadata_json VARCHAR, event_status VARCHAR, amount_usd DOUBLE, source_layer_name VARCHAR, source_layer_file VARCHAR)");
        exec("CREATE TABLE layer_feature_bboxes(layer_idx UBIGINT, layer_file VARCHAR, layer_name VARCHAR, duckdb_role VARCHAR, feature_idx UBIGINT, entity_id VARCHAR, scale VARCHAR, category VARCHAR, provenance_world VARCHAR, provenance_nation_state VARCHAR, provenance_state_region VARCHAR, provenance_county_city VARCHAR, min_lon DOUBLE, min_lat DOUBLE, max_lon DOUBLE, max_lat DOUBLE, center_lon DOUBLE, center_lat DOUBLE)");
        exec("CREATE TABLE layer_bboxes(layer_idx UBIGINT, layer_file VARCHAR, layer_name VARCHAR, duckdb_role VARCHAR, scale VARCHAR, category VARCHAR, provenance_world VARCHAR, provenance_nation_state VARCHAR, provenance_state_region VARCHAR, provenance_county_city VARCHAR, feature_count UBIGINT, min_lon DOUBLE, min_lat DOUBLE, max_lon DOUBLE, max_lat DOUBLE)");
        exec("CREATE TABLE parcel_zone_memberships(parcel_layer_idx UBIGINT, parcel_entity_id VARCHAR, parcel_geometry_entity_id VARCHAR, blocklot VARCHAR, zone_layer_idx UBIGINT, zone_layer_file VARCHAR, zone_layer_name VARCHAR, zone_feature_idx UBIGINT, zone_entity_id VARCHAR, zone_key VARCHAR, zone_label VARCHAR, relation VARCHAR, parcel_centroid_lon DOUBLE, parcel_centroid_lat DOUBLE, overlap_area DOUBLE, overlap_ratio DOUBLE, source_signature VARCHAR)");
        exec("CREATE TABLE analytics_build_info(source_signature VARCHAR)");
        exec("INSERT INTO analytics_build_info VALUES ('parcel_hover_click_ui_sig_v1')");
        exec(R"SQL(
            INSERT INTO layer_features VALUES
            (0, 'Primary Parcels', 'parcel.geojson', 'parcel_record', 0, 'CANONICALBC0', 'parcel', 'Housing', '', '', '', '', -76.71, 39.20, -76.70, 39.21, 'BC-0', 'owner zero', '8 County St', '21210', 'ACTIVE', '', '', 125000, 800, '', '', '', '', '', '', '', '', '', 0, 0),
            (0, 'Primary Parcels', 'parcel.geojson', 'parcel_record', 1, 'CANONICALBC1', 'parcel', 'Housing', '', '', '', '', -76.70, 39.20, -76.69, 39.21, 'BC-1', 'county owner', '10 County St', '21211', 'ACTIVE', '', '', 150000, 900, '', '', '', '', '', '', '', '', '', 0, 0)
        )SQL");
        exec(R"SQL(
            INSERT INTO unified_parcels VALUES
            (0, 'CANONICALBC0', 'GEOMETRYBC0', 'BC-0', 'parcel.geojson', 'rp.geojson', true, true, 'owner zero', 'Owner Zero', '8 County St', '8countyst', '21210', 'ACTIVE', 70000, 55000, 800, 125000, 0, 125000, 0, 0, 0, 0, 0, 0, -76.71, 39.20, -76.70, 39.21),
            (0, 'CANONICALBC1', 'GEOMETRYBC1', 'BC-1', 'parcel.geojson', 'rp.geojson', true, true, 'county owner', 'County Owner', '10 County St', '10countyst', '21211', 'ACTIVE', 100000, 50000, 900, 150000, 0, 150000, 0, 0, 0, 0, 0, 0, -76.70, 39.20, -76.69, 39.21)
        )SQL");

        DuckDbAnalytics analytics(test_root);
        const bool cache_ok = analytics.status().last_rebuild_ok;

        std::vector<LayerDef> layers(1);
        layers[0].file = "parcel.geojson";
        layers[0].name = "Primary Parcels";
        layers[0].scale = "parcel";
        layers[0].duckdb_role = "parcel_record";
        layers[0].enabled = true;

        std::vector<LayerDef::FeatureRecord> artifact_features(2);
        artifact_features[0].entity_id = "SYNTHETIC0";
        artifact_features[0].geometry_entity_id = "SYNTHETICGEOM0";
        artifact_features[0].extent.min_lon = -76.71f;
        artifact_features[0].extent.min_lat = 39.20f;
        artifact_features[0].extent.max_lon = -76.70f;
        artifact_features[0].extent.max_lat = 39.21f;
        artifact_features[0].rings = {{
            ImVec2(-76.71f, 39.20f),
            ImVec2(-76.70f, 39.20f),
            ImVec2(-76.70f, 39.21f),
            ImVec2(-76.71f, 39.21f),
            ImVec2(-76.71f, 39.20f)
        }};
        ensureFeatureTriangles(artifact_features[0]);

        artifact_features[1].entity_id = "SYNTHETIC1";
        artifact_features[1].geometry_entity_id = "SYNTHETICGEOM1";
        artifact_features[1].extent.min_lon = -76.70f;
        artifact_features[1].extent.min_lat = 39.20f;
        artifact_features[1].extent.max_lon = -76.69f;
        artifact_features[1].extent.max_lat = 39.21f;
        artifact_features[1].rings = {{
            ImVec2(-76.70f, 39.20f),
            ImVec2(-76.69f, 39.20f),
            ImVec2(-76.69f, 39.21f),
            ImVec2(-76.70f, 39.21f),
            ImVec2(-76.70f, 39.20f)
        }};
        ensureFeatureTriangles(artifact_features[1]);

        const std::string parcel_sig = "parcel_hover_click_ui_sig_v1";
        PolygonGeometryArtifact artifact;
        const bool artifact_ok = buildPolygonGeometryArtifact(layers[0], artifact_features, parcel_sig, artifact, 64);
        const fs::path artifact_path = geometryArtifactCachePathForLayerFile(
            test_root,
            layers[0].file,
            GeometryArtifactClass::Polygon,
            layerRenderRouteArtifactName(LayerRenderRoute::ParcelPolygonGpu));
        fs::create_directories(artifact_path.parent_path(), ec);
        if (artifact_ok) {
            saveBinaryPolygonGeometryArtifact(artifact_path, artifact);
        }

        std::vector<LayerRuntimeState> layer_states(1);
        layer_states[0].status = LayerPipelineStatus::Ready;
        layer_states[0].hydration_source_signature = parcel_sig;

        AppSettings app_settings;
        std::unordered_map<std::string, ZoneMetadata> zoning_metadata;
        std::unordered_map<std::string, bool> zoning_zone_enabled;
        std::unordered_map<std::string, ImVec4> zoning_zone_color;
        std::unordered_map<std::string, std::string> zoning_zone_label;
        std::vector<std::string> zoning_zone_order;
        std::unordered_map<std::string, size_t> zoning_zone_counts;
        std::unordered_map<std::string, std::vector<std::string>> zoning_group_zones;
        std::vector<std::string> zoning_group_order;
        size_t zoning_zone_discovered_feature_count = 0;
        std::unordered_map<std::string, size_t> real_property_by_blocklot;
        std::vector<LayerDef::FeatureRecord> harmonized_real_property_features;
        std::vector<std::string> harmonized_real_property_source_files;
        std::string harmonized_real_property_signature;
        size_t cached_real_property_size = 0;
        size_t cached_vac_notice_size = 0;
        std::string cached_vac_notice_signature;
        size_t cached_vac_rehab_size = 0;
        std::string cached_vac_rehab_signature;
        size_t cached_tax_lien_size = 0;
        std::string cached_tax_lien_signature;
        size_t cached_tax_sale_size = 0;
        std::string cached_tax_sale_signature;
        std::unordered_map<std::string, int> vacant_notice_count_by_blocklot;
        std::unordered_map<std::string, int> vacant_rehab_count_by_blocklot;
        std::unordered_map<std::string, int> tax_lien_count_by_blocklot;
        std::unordered_map<std::string, double> tax_lien_amount_by_blocklot;
        std::unordered_map<std::string, int> tax_sale_count_by_blocklot;
        std::unordered_map<std::string, double> tax_sale_amount_by_blocklot;
        int vacancy_maps_generation = 0;
        int parcel_vacancy_generation_applied = 0;
        int tax_maps_generation = 0;
        int parcel_tax_generation_applied = 0;
        std::vector<int> parcel_vac_notice_by_feature;
        std::vector<int> parcel_vac_rehab_by_feature;
        std::vector<int> parcel_tax_lien_by_feature;
        std::vector<int> parcel_tax_sale_by_feature;
        std::vector<double> parcel_tax_lien_amount_by_feature;
        std::vector<double> parcel_tax_sale_amount_by_feature;
        std::vector<std::string> parcel_blocklot_by_feature;
        std::string parcel_blocklot_cached_signature;
        std::atomic<size_t> vacant_notice_rows_matched_total{0};
        std::atomic<size_t> vacant_rehab_rows_matched_total{0};
        std::atomic<size_t> vacant_parcels_matched_total{0};
        std::atomic<size_t> vacant_parcels_with_geometry_total{0};
        std::atomic<size_t> vacant_parcels_triangulated_renderable_total{0};
        std::vector<UnifiedParcelRecord> unified_parcels;
        std::vector<std::string> parcel_owner_search_by_feature;
        std::vector<std::string> real_property_owner_search_by_feature;
        std::vector<std::string> parcel_address_search_by_feature;
        size_t unified_parcel_cached_size = 0;
        std::string unified_parcel_cached_signature;
        std::string last_refresh_inputs_signature;
        size_t unified_real_property_cached_size = 0;
        int unified_vacancy_generation_applied = 0;
        int unified_tax_generation_applied = 0;
        bool owner_aggregates_dirty = false;
        ParcelRenderCacheBlob render_blob;

        DerivedLayerCachesContext caches_ctx;
        caches_ctx.root = &test_root;
        caches_ctx.layers = &layers;
        caches_ctx.layer_states = &layer_states;
        caches_ctx.app_settings = &app_settings;
        caches_ctx.duckdb_analytics = &analytics;
        caches_ctx.zoning_layer_idx = -1;
        caches_ctx.real_property_layer_idx = -1;
        caches_ctx.vacant_notice_layer_idx = -1;
        caches_ctx.vacant_rehab_layer_idx = -1;
        caches_ctx.tax_lien_layer_idx = -1;
        caches_ctx.tax_sale_layer_idx = -1;
        caches_ctx.parcel_layer_idx = 0;
        caches_ctx.parcel_render_blob = &render_blob;
        caches_ctx.zoning_metadata = &zoning_metadata;
        caches_ctx.zoning_zone_enabled = &zoning_zone_enabled;
        caches_ctx.zoning_zone_color = &zoning_zone_color;
        caches_ctx.zoning_zone_label = &zoning_zone_label;
        caches_ctx.zoning_zone_order = &zoning_zone_order;
        caches_ctx.zoning_zone_counts = &zoning_zone_counts;
        caches_ctx.zoning_group_zones = &zoning_group_zones;
        caches_ctx.zoning_group_order = &zoning_group_order;
        caches_ctx.zoning_zone_discovered_feature_count = &zoning_zone_discovered_feature_count;
        caches_ctx.real_property_by_blocklot = &real_property_by_blocklot;
        caches_ctx.harmonized_real_property_features = &harmonized_real_property_features;
        caches_ctx.harmonized_real_property_source_files = &harmonized_real_property_source_files;
        caches_ctx.harmonized_real_property_signature = &harmonized_real_property_signature;
        caches_ctx.cached_real_property_size = &cached_real_property_size;
        caches_ctx.cached_vac_notice_size = &cached_vac_notice_size;
        caches_ctx.cached_vac_notice_signature = &cached_vac_notice_signature;
        caches_ctx.cached_vac_rehab_size = &cached_vac_rehab_size;
        caches_ctx.cached_vac_rehab_signature = &cached_vac_rehab_signature;
        caches_ctx.cached_tax_lien_size = &cached_tax_lien_size;
        caches_ctx.cached_tax_lien_signature = &cached_tax_lien_signature;
        caches_ctx.cached_tax_sale_size = &cached_tax_sale_size;
        caches_ctx.cached_tax_sale_signature = &cached_tax_sale_signature;
        caches_ctx.vacant_notice_count_by_blocklot = &vacant_notice_count_by_blocklot;
        caches_ctx.vacant_rehab_count_by_blocklot = &vacant_rehab_count_by_blocklot;
        caches_ctx.tax_lien_count_by_blocklot = &tax_lien_count_by_blocklot;
        caches_ctx.tax_lien_amount_by_blocklot = &tax_lien_amount_by_blocklot;
        caches_ctx.tax_sale_count_by_blocklot = &tax_sale_count_by_blocklot;
        caches_ctx.tax_sale_amount_by_blocklot = &tax_sale_amount_by_blocklot;
        caches_ctx.vacancy_maps_generation = &vacancy_maps_generation;
        caches_ctx.parcel_vacancy_generation_applied = &parcel_vacancy_generation_applied;
        caches_ctx.tax_maps_generation = &tax_maps_generation;
        caches_ctx.parcel_tax_generation_applied = &parcel_tax_generation_applied;
        caches_ctx.parcel_vac_notice_by_feature = &parcel_vac_notice_by_feature;
        caches_ctx.parcel_vac_rehab_by_feature = &parcel_vac_rehab_by_feature;
        caches_ctx.parcel_tax_lien_by_feature = &parcel_tax_lien_by_feature;
        caches_ctx.parcel_tax_sale_by_feature = &parcel_tax_sale_by_feature;
        caches_ctx.parcel_tax_lien_amount_by_feature = &parcel_tax_lien_amount_by_feature;
        caches_ctx.parcel_tax_sale_amount_by_feature = &parcel_tax_sale_amount_by_feature;
        caches_ctx.parcel_blocklot_by_feature = &parcel_blocklot_by_feature;
        caches_ctx.parcel_blocklot_cached_signature = &parcel_blocklot_cached_signature;
        caches_ctx.vacant_notice_rows_matched_total = &vacant_notice_rows_matched_total;
        caches_ctx.vacant_rehab_rows_matched_total = &vacant_rehab_rows_matched_total;
        caches_ctx.vacant_parcels_matched_total = &vacant_parcels_matched_total;
        caches_ctx.vacant_parcels_with_geometry_total = &vacant_parcels_with_geometry_total;
        caches_ctx.vacant_parcels_triangulated_renderable_total = &vacant_parcels_triangulated_renderable_total;
        caches_ctx.unified_parcels = &unified_parcels;
        caches_ctx.parcel_owner_search_by_feature = &parcel_owner_search_by_feature;
        caches_ctx.real_property_owner_search_by_feature = &real_property_owner_search_by_feature;
        caches_ctx.parcel_address_search_by_feature = &parcel_address_search_by_feature;
        caches_ctx.unified_parcel_cached_size = &unified_parcel_cached_size;
        caches_ctx.unified_parcel_cached_signature = &unified_parcel_cached_signature;
        caches_ctx.last_refresh_inputs_signature = &last_refresh_inputs_signature;
        caches_ctx.unified_real_property_cached_size = &unified_real_property_cached_size;
        caches_ctx.unified_vacancy_generation_applied = &unified_vacancy_generation_applied;
        caches_ctx.unified_tax_generation_applied = &unified_tax_generation_applied;
        caches_ctx.owner_aggregates_dirty = &owner_aggregates_dirty;
        refreshDerivedLayerCaches(caches_ctx);

        MapHoverState hover_state;
        hover_state.hovered_parcel_layer_idx = 0;
        hover_state.hovered_parcel_idx = 1;
        hover_state.inspect_parcel_layer_idx = 0;
        hover_state.inspect_parcel_idx = 1;

        ParcelSelectionState selection;
        std::string opened_entity_id;
        MapInspectionContext ctx;
        ctx.map_hovered = true;
        ctx.parcel_hover_active = true;
        ctx.parcel_inspect_active = true;
        ctx.parcel_layer_idx = 0;
        ctx.layers = &layers;
        ctx.unified_parcels = &unified_parcels;
        ctx.duckdb_analytics = &analytics;
        ctx.parcel_selection = &selection;
        ctx.open_parcel_element = [&](const std::string& entity_id) {
            opened_entity_id = entity_id;
        };
        ctx.hover_state = &hover_state;

        const ParcelHoverResolution hovered = resolveHoveredParcel(ctx);
        const ParcelHoverDetail detail = resolveParcelHoverDetail(ctx, hovered);
        const bool click_ok = applyParcelClickSelection(ctx, hovered, false);

        const bool ok =
            cache_ok &&
            artifact_ok &&
            render_blob.features.size() == 2 &&
            unified_parcels.size() == 2 &&
            hovered.hit &&
            hovered.layer_idx == 0 &&
            hovered.feature_idx == 1 &&
            hovered.unified_record != nullptr &&
            hovered.entity_id == "CANONICALBC1" &&
            detail.available &&
            detail.parcel_entity_id == "CANONICALBC1" &&
            detail.blocklot == "BC-1" &&
            detail.owner_display == "County Owner" &&
            detail.address == "10 County St" &&
            detail.tax_lien_count == 0 &&
            click_ok &&
            selection.active_layer_idx == 0 &&
            selection.active_entity_id == "CANONICALBC1" &&
            selection.refs.size() == 1 &&
            selection.refs[0].feature_idx == 1 &&
            selection.refs[0].geometry_entity_id == "GEOMETRYBC1" &&
            opened_entity_id == "CANONICALBC1";

        std::cout << json{
            {"mode", kMode},
            {"ok", ok},
            {"cache_ok", cache_ok},
            {"artifact_ok", artifact_ok},
            {"render_blob_features", render_blob.features.size()},
            {"unified_parcels", unified_parcels.size()},
            {"hover_hit", hovered.hit},
            {"hover_entity_id", hovered.entity_id},
            {"detail_entity_id", detail.parcel_entity_id},
            {"detail_available", detail.available},
            {"detail_blocklot", detail.blocklot},
            {"click_ok", click_ok},
            {"selected_feature_idx", selection.refs.empty() ? (uint64_t)-1 : (uint64_t)selection.refs[0].feature_idx},
            {"selected_geometry_entity_id", selection.refs.empty() ? "" : selection.refs[0].geometry_entity_id},
            {"selected_entity_id", selection.active_entity_id}
        }.dump(2) << '\n';
        return ok ? 0 : 1;
    } catch (const std::exception& ex) {
        std::cout << json{
            {"mode", kMode},
            {"ok", false},
            {"error", ex.what()}
        }.dump(2) << '\n';
        return 1;
    }
}

int runParcelHoverClickPickSelftest(const fs::path& root) {
    constexpr const char* kMode = "parcel-hover-click-pick-selftest";
    const fs::path test_root = root / "data" / "cache" / "selftest" / "parcel_hover_click_pick";
    std::error_code ec;
    fs::remove_all(test_root, ec);
    fs::create_directories(test_root / "data", ec);
    const fs::path db_path = test_root / "data" / "worldsim.duckdb";

    try {
        duckdb::DuckDB db(db_path.string());
        duckdb::Connection con(db);
        auto exec = [&](const char* sql) {
            auto res = con.Query(sql);
            if (!res || res->HasError()) {
                throw std::runtime_error(res ? res->GetError() : "query failed");
            }
        };
        exec("CREATE TABLE layer_features(layer_idx UBIGINT, layer_name VARCHAR, layer_file VARCHAR, duckdb_role VARCHAR, feature_idx UBIGINT, entity_id VARCHAR, scale VARCHAR, category VARCHAR, provenance_world VARCHAR, provenance_nation_state VARCHAR, provenance_state_region VARCHAR, provenance_county_city VARCHAR, min_lon DOUBLE, min_lat DOUBLE, max_lon DOUBLE, max_lat DOUBLE, blocklot VARCHAR, owner VARCHAR, address VARCHAR, zipcode VARCHAR, status VARCHAR, zoning VARCHAR, jurisdiction VARCHAR, value_usd DOUBLE, structure_area_sqft DOUBLE, feature_name VARCHAR, lga_name VARCHAR, ward_name VARCHAR, source_name VARCHAR, event_date_text VARCHAR, event_status_hint VARCHAR, event_title_hint VARCHAR, event_detail_hint VARCHAR, event_metadata_json VARCHAR, event_year_hint INTEGER, amount_usd_hint DOUBLE)");
        exec("CREATE TABLE layer_feature_properties(layer_idx UBIGINT, layer_name VARCHAR, layer_file VARCHAR, duckdb_role VARCHAR, feature_idx UBIGINT, entity_id VARCHAR, property_key VARCHAR, property_value VARCHAR)");
        exec("CREATE TABLE unified_parcels(parcel_layer_idx UBIGINT, parcel_entity_id VARCHAR, parcel_geometry_entity_id VARCHAR, blocklot VARCHAR, parcel_source_file VARCHAR, property_source_file VARCHAR, parcel_has_geometry BOOLEAN, has_property_record BOOLEAN, owner VARCHAR, owner_display VARCHAR, address VARCHAR, address_search VARCHAR, zipcode VARCHAR, status VARCHAR, current_land DOUBLE, current_improvements DOUBLE, structure_area_sqft DOUBLE, tax_base DOUBLE, sale_price DOUBLE, current_value DOUBLE, vacant_notice_count INTEGER, vacant_rehab_count INTEGER, tax_lien_count INTEGER, tax_sale_count INTEGER, tax_lien_amount DOUBLE, tax_sale_amount DOUBLE, min_lon DOUBLE, min_lat DOUBLE, max_lon DOUBLE, max_lat DOUBLE)");
        exec("CREATE TABLE parcel_events(blocklot VARCHAR, event_date VARCHAR, event_type VARCHAR, event_label VARCHAR, event_title VARCHAR, event_detail VARCHAR, event_metadata_json VARCHAR, event_status VARCHAR, amount_usd DOUBLE, source_layer_name VARCHAR, source_layer_file VARCHAR)");
        exec("CREATE TABLE layer_feature_bboxes(layer_idx UBIGINT, layer_file VARCHAR, layer_name VARCHAR, duckdb_role VARCHAR, feature_idx UBIGINT, entity_id VARCHAR, scale VARCHAR, category VARCHAR, provenance_world VARCHAR, provenance_nation_state VARCHAR, provenance_state_region VARCHAR, provenance_county_city VARCHAR, min_lon DOUBLE, min_lat DOUBLE, max_lon DOUBLE, max_lat DOUBLE, center_lon DOUBLE, center_lat DOUBLE)");
        exec("CREATE TABLE layer_bboxes(layer_idx UBIGINT, layer_file VARCHAR, layer_name VARCHAR, duckdb_role VARCHAR, scale VARCHAR, category VARCHAR, provenance_world VARCHAR, provenance_nation_state VARCHAR, provenance_state_region VARCHAR, provenance_county_city VARCHAR, feature_count UBIGINT, min_lon DOUBLE, min_lat DOUBLE, max_lon DOUBLE, max_lat DOUBLE)");
        exec("CREATE TABLE parcel_zone_memberships(parcel_layer_idx UBIGINT, parcel_entity_id VARCHAR, parcel_geometry_entity_id VARCHAR, blocklot VARCHAR, zone_layer_idx UBIGINT, zone_layer_file VARCHAR, zone_layer_name VARCHAR, zone_feature_idx UBIGINT, zone_entity_id VARCHAR, zone_key VARCHAR, zone_label VARCHAR, relation VARCHAR, parcel_centroid_lon DOUBLE, parcel_centroid_lat DOUBLE, overlap_area DOUBLE, overlap_ratio DOUBLE, source_signature VARCHAR)");
        exec(R"SQL(
            INSERT INTO layer_features VALUES
            (1, 'County Parcel Test', 'county_parcel_test.geojson', 'parcel_record', 0, 'ENTITYCOUNTY1', 'parcel', 'Housing', '', '', '', '', -76.7000, 39.2000, -76.6900, 39.2100, 'BC-1', 'county owner', '10 County St', '21211', 'ACTIVE', '', '', 150000, 900, '', '', '', '', '', '', '', '', '', 0, 0)
        )SQL");

        DuckDbAnalytics analytics(test_root);
        const bool cache_ok = analytics.status().last_rebuild_ok;

        std::vector<LayerDef> layers(2);
        layers[0].file = "parcel.geojson";
        layers[0].name = "Primary Parcels";
        layers[0].scale = "parcel";
        layers[0].duckdb_role = "parcel_record";
        layers[0].enabled = true;

        LayerDef::FeatureRecord primary_feature;
        primary_feature.extent.min_lon = -76.6800f;
        primary_feature.extent.min_lat = 39.2000f;
        primary_feature.extent.max_lon = -76.6700f;
        primary_feature.extent.max_lat = 39.2100f;
        primary_feature.rings = {{
            ImVec2(-76.6800f, 39.2000f),
            ImVec2(-76.6700f, 39.2000f),
            ImVec2(-76.6700f, 39.2100f),
            ImVec2(-76.6800f, 39.2100f),
            ImVec2(-76.6800f, 39.2000f)
        }};
        primary_feature.entity_id = "ENTITYPRIMARY0";
        ensureFeatureTriangles(primary_feature);
        layers[0].features.push_back(primary_feature);

        layers[1].file = "county_parcel_test.geojson";
        layers[1].name = "County Parcel Test";
        layers[1].scale = "parcel";
        layers[1].duckdb_role = "parcel_record";
        layers[1].enabled = true;

        LayerDef::FeatureRecord county_feature;
        county_feature.extent.min_lon = -76.7000f;
        county_feature.extent.min_lat = 39.2000f;
        county_feature.extent.max_lon = -76.6900f;
        county_feature.extent.max_lat = 39.2100f;
        county_feature.rings = {{
            ImVec2(-76.7000f, 39.2000f),
            ImVec2(-76.6900f, 39.2000f),
            ImVec2(-76.6900f, 39.2100f),
            ImVec2(-76.7000f, 39.2100f),
            ImVec2(-76.7000f, 39.2000f)
        }};
        county_feature.entity_id = "ENTITYCOUNTY1";
        county_feature.geometry_entity_id = "GEOMCOUNTY1";
        county_feature.source_feature_id = "SRCCOUNTY1";
        ensureFeatureTriangles(county_feature);
        layers[1].features.push_back(county_feature);

        PolygonGeometryArtifact county_artifact;
        const bool artifact_ok = buildPolygonGeometryArtifact(layers[1], layers[1].features, "county_pick_sig", county_artifact, 64);
        std::unordered_map<size_t, PolygonGeometryArtifact> polygon_artifacts;
        polygon_artifacts.emplace(1u, county_artifact);

        std::vector<LayerSpatialIndex> layer_spatial(2);
        buildLayerSpatialIndex(layers[0], layer_spatial[0]);
        buildLayerSpatialIndex(layers[1], layer_spatial[1]);

        std::vector<bool> layer_hover_enabled(2, true);
        std::vector<bool> layer_inspect_enabled(2, true);

        MapHoverQuery hover_query;
        hover_query.map_hovered = true;
        hover_query.parcel_hover_active = true;
        hover_query.parcel_inspect_active = true;
        hover_query.active_hover_layer_idx = 1;
        hover_query.active_click_layer_idx = 1;
        hover_query.parcel_layer_idx = 0;
        hover_query.layers = &layers;
        hover_query.polygon_geometry_artifacts = &polygon_artifacts;
        hover_query.layer_spatial = &layer_spatial;
        hover_query.layer_hover_enabled = &layer_hover_enabled;
        hover_query.layer_inspect_enabled = &layer_inspect_enabled;
        hover_query.mouse_ll = ImVec2(-76.6950f, 39.2050f);

        const MapHoverState hover_state = findMapHoverTargets(hover_query);

        ParcelSelectionState selection;
        std::string opened_entity_id;
        MapInspectionContext ctx;
        ctx.map_hovered = true;
        ctx.parcel_hover_active = true;
        ctx.parcel_inspect_active = true;
        ctx.parcel_layer_idx = 0;
        ctx.layers = &layers;
        ctx.unified_parcels = nullptr;
        ctx.duckdb_analytics = &analytics;
        ctx.polygon_geometry_artifacts = &polygon_artifacts;
        ctx.layer_spatial = &layer_spatial;
        ctx.parcel_selection = &selection;
        ctx.open_parcel_element = [&](const std::string& entity_id) {
            opened_entity_id = entity_id;
        };
        ctx.hover_state = &hover_state;

        const ParcelHoverResolution hovered = resolveHoveredParcel(ctx);
        const ParcelHoverResolution inspect = resolveInspectParcel(ctx);
        const ParcelHoverDetail detail = resolveParcelHoverDetail(ctx, inspect);
        const bool click_ok = applyParcelClickSelection(ctx, inspect, false);

        const bool ok =
            cache_ok &&
            artifact_ok &&
            hover_state.hovered_parcel_layer_idx < 0 &&
            hover_state.inspect_parcel_layer_idx < 0 &&
            hover_state.hovered_parcel_idx == (size_t)-1 &&
            hover_state.inspect_parcel_idx == (size_t)-1 &&
            hover_state.hovered_parcel_entity_id.empty() &&
            hover_state.inspect_parcel_entity_id.empty() &&
            !hovered.hit &&
            !inspect.hit &&
            !detail.available &&
            !click_ok &&
            selection.active_layer_idx < 0 &&
            selection.active_entity_id.empty() &&
            opened_entity_id.empty();

        std::cout << json{
            {"mode", kMode},
            {"ok", ok},
            {"cache_ok", cache_ok},
            {"artifact_ok", artifact_ok},
            {"gpu_only_no_cpu_fallback", ok},
            {"hover_layer_idx", hover_state.hovered_parcel_layer_idx},
            {"inspect_layer_idx", hover_state.inspect_parcel_layer_idx},
            {"hover_feature_idx", hover_state.hovered_parcel_idx},
            {"inspect_feature_idx", hover_state.inspect_parcel_idx},
            {"hover_entity_id", hover_state.hovered_parcel_entity_id},
            {"inspect_entity_id", hover_state.inspect_parcel_entity_id},
            {"detail_available", detail.available},
            {"detail_blocklot", detail.blocklot},
            {"click_ok", click_ok},
            {"selected_entity_id", selection.active_entity_id}
        }.dump(2) << '\n';
        return ok ? 0 : 1;
    } catch (const std::exception& ex) {
        std::cout << json{
            {"mode", kMode},
            {"ok", false},
            {"error", ex.what()}
        }.dump(2) << '\n';
        return 1;
    }
}

int verifyParcelDuckDbKeys(const fs::path& root) {
    constexpr const char* kMode = "verify-parcel-duckdb-keys";
    try {
        const fs::path db_path = root / "data" / "worldsim.duckdb";
        duckdb::DuckDB db(db_path.string());
        duckdb::Connection con(db);
        auto scalar_u64 = [&](const std::string& sql) -> uint64_t {
            auto res = con.Query(sql);
            if (!res || res->HasError() || res->RowCount() == 0) {
                throw std::runtime_error(res ? res->GetError() : "query failed");
            }
            return res->GetValue(0, 0).GetValue<uint64_t>();
        };

        const uint64_t parcel_layer_features = scalar_u64(R"SQL(
            SELECT count(*)
            FROM layer_features
            WHERE scale = 'parcel'
              AND duckdb_role = 'parcel_record'
              AND layer_idx IN (SELECT DISTINCT parcel_layer_idx FROM unified_parcels)
        )SQL");
        const uint64_t unified_parcels = scalar_u64(
            "SELECT count(*) FROM unified_parcels");
        const uint64_t missing_entity_matches = scalar_u64(R"SQL(
            SELECT count(*)
            FROM layer_features lf
            LEFT JOIN unified_parcels up
              ON up.parcel_layer_idx = lf.layer_idx
             AND up.parcel_entity_id = lf.entity_id
            WHERE lf.scale = 'parcel'
              AND lf.duckdb_role = 'parcel_record'
              AND lf.layer_idx IN (SELECT DISTINCT parcel_layer_idx FROM unified_parcels)
              AND up.parcel_entity_id IS NULL
        )SQL");
        const uint64_t duplicate_unified_entities = scalar_u64(R"SQL(
            SELECT count(*)
            FROM (
                SELECT parcel_layer_idx, parcel_entity_id, count(*) AS n
                FROM unified_parcels
                GROUP BY 1, 2
                HAVING count(*) > 1
            )
        )SQL");
        const uint64_t missing_property_records = scalar_u64(R"SQL(
            SELECT count(*)
            FROM unified_parcels
            WHERE NOT has_property_record
        )SQL");
        const uint64_t layer_feature_index_gaps = scalar_u64(R"SQL(
            SELECT count(*)
            FROM (
                SELECT layer_idx
                FROM layer_features
                WHERE scale = 'parcel'
                  AND duckdb_role = 'parcel_record'
                  AND layer_idx IN (SELECT DISTINCT parcel_layer_idx FROM unified_parcels)
                GROUP BY layer_idx
                HAVING NOT (
                    min(feature_idx) = 0
                    AND max(feature_idx) + 1 = count(*)
                    AND count(DISTINCT feature_idx) = count(*)
                )
            )
        )SQL");
        const uint64_t parcel_zone_membership_tables = scalar_u64(R"SQL(
            SELECT count(*)
            FROM information_schema.tables
            WHERE table_schema = 'main'
              AND table_name = 'parcel_zone_memberships'
        )SQL");
        const uint64_t layer_feature_bbox_tables = scalar_u64(R"SQL(
            SELECT count(*)
            FROM information_schema.tables
            WHERE table_schema = 'main'
              AND table_name = 'layer_feature_bboxes'
        )SQL");
        const uint64_t layer_bbox_tables = scalar_u64(R"SQL(
            SELECT count(*)
            FROM information_schema.tables
            WHERE table_schema = 'main'
              AND table_name = 'layer_bboxes'
        )SQL");
        const uint64_t layer_feature_bboxes = layer_feature_bbox_tables > 0
            ? scalar_u64("SELECT count(*) FROM layer_feature_bboxes")
            : 0;
        const uint64_t layer_bboxes = layer_bbox_tables > 0
            ? scalar_u64("SELECT count(*) FROM layer_bboxes")
            : 0;
        const uint64_t parcel_zone_memberships = parcel_zone_membership_tables > 0
            ? scalar_u64("SELECT count(*) FROM parcel_zone_memberships")
            : 0;
        const uint64_t parcel_zone_membership_bad_refs = parcel_zone_membership_tables > 0
            ? scalar_u64(R"SQL(
                SELECT count(*)
                FROM parcel_zone_memberships pzm
                LEFT JOIN unified_parcels up
                  ON up.parcel_layer_idx = pzm.parcel_layer_idx
                 AND up.parcel_entity_id = pzm.parcel_entity_id
                LEFT JOIN layer_features zf
                  ON zf.layer_idx = pzm.zone_layer_idx
                 AND zf.entity_id = pzm.zone_entity_id
                WHERE up.parcel_entity_id IS NULL
                   OR zf.entity_id IS NULL
            )SQL")
            : 0;
        const uint64_t open_vacant_notice_events = scalar_u64(R"SQL(
            SELECT count(*)
            FROM parcel_events
            WHERE event_type = 'vacant_notice'
              AND lower(source_layer_file) LIKE '%open_notices%vacant%'
        )SQL");
        const uint64_t vacant_notice_rollup_mismatches = scalar_u64(R"SQL(
            SELECT count(*)
            FROM unified_parcels up
            WHERE up.vacant_notice_count <> COALESCE((
                SELECT count(*)::INTEGER
                FROM parcel_events pe
                WHERE pe.blocklot = up.blocklot
                  AND pe.event_type = 'vacant_notice'
            ), 0)
        )SQL");
        const uint64_t vacant_rehab_rollup_mismatches = scalar_u64(R"SQL(
            SELECT count(*)
            FROM unified_parcels up
            WHERE up.vacant_rehab_count <> COALESCE((
                SELECT count(*)::INTEGER
                FROM parcel_events pe
                WHERE pe.blocklot = up.blocklot
                  AND pe.event_type = 'vacant_rehab'
            ), 0)
        )SQL");

        const bool ok =
            parcel_layer_features > 0 &&
            parcel_layer_features == unified_parcels &&
            missing_entity_matches == 0 &&
            duplicate_unified_entities == 0 &&
            layer_feature_index_gaps == 0 &&
            layer_feature_bbox_tables == 1 &&
            layer_bbox_tables == 1 &&
            layer_feature_bboxes > 0 &&
            layer_bboxes > 0 &&
            parcel_zone_membership_tables == 1 &&
            parcel_zone_membership_bad_refs == 0 &&
            vacant_notice_rollup_mismatches == 0 &&
            vacant_rehab_rollup_mismatches == 0;
        std::cout << json{
            {"mode", kMode},
            {"ok", ok},
            {"db_path", db_path.string()},
            {"parcel_layer_features", parcel_layer_features},
            {"unified_parcels", unified_parcels},
            {"missing_entity_matches", missing_entity_matches},
            {"duplicate_unified_entities", duplicate_unified_entities},
            {"missing_property_records", missing_property_records},
            {"layer_feature_index_gap_count", layer_feature_index_gaps},
            {"layer_feature_bboxes", layer_feature_bboxes},
            {"layer_bboxes", layer_bboxes},
            {"parcel_zone_memberships", parcel_zone_memberships},
            {"parcel_zone_membership_bad_refs", parcel_zone_membership_bad_refs},
            {"open_vacant_notice_events", open_vacant_notice_events},
            {"vacant_notice_rollup_mismatches", vacant_notice_rollup_mismatches},
            {"vacant_rehab_rollup_mismatches", vacant_rehab_rollup_mismatches}
        }.dump(2) << '\n';
        return ok ? 0 : 1;
    } catch (const std::exception& ex) {
        std::cout << json{
            {"mode", kMode},
            {"ok", false},
            {"error", ex.what()}
        }.dump(2) << '\n';
        return 1;
    }
}

int buildParcelZoneMembershipsCli(const fs::path& root, const WorldsimCliOptions& options) {
    constexpr const char* kMode = "build-parcel-zone-memberships";
    const auto started = std::chrono::steady_clock::now();
    try {
        const fs::path db_path = root / "data" / "worldsim.duckdb";
        duckdb::DuckDB db(db_path.string());
        duckdb::Connection con(db);

        auto query_or_throw = [&](const std::string& sql, const char* context) {
            auto res = con.Query(sql);
            if (!res || res->HasError()) {
                throw std::runtime_error(
                    std::string(context) + ": " + (res ? res->GetError() : std::string("query failed")));
            }
            return res;
        };
        auto exec = [&](const std::string& sql, const char* context) {
            (void)query_or_throw(sql, context);
        };
        auto scalar_u64 = [&](const std::string& sql, const char* context) -> uint64_t {
            auto res = query_or_throw(sql, context);
            if (res->RowCount() == 0) throw std::runtime_error(std::string(context) + ": empty result");
            return res->GetValue(0, 0).GetValue<uint64_t>();
        };
        auto scalar_str = [&](const std::string& sql, const char* context) -> std::string {
            auto res = query_or_throw(sql, context);
            if (res->RowCount() == 0 || res->GetValue(0, 0).IsNull()) return {};
            return res->GetValue(0, 0).ToString();
        };
        auto table_exists = [&](const char* table_name) -> bool {
            return scalar_u64(
                "SELECT count(*) FROM information_schema.tables "
                "WHERE table_schema = 'main' AND table_name = '" + std::string(table_name) + "'",
                table_name) > 0;
        };

        const std::array<const char*, 5> required_tables = {
            "layer_features",
            "unified_parcels",
            "layer_feature_bboxes",
            "layer_bboxes",
            "parcel_zone_memberships"
        };
        json missing = json::array();
        for (const char* table_name : required_tables) {
            if (!table_exists(table_name)) missing.push_back(table_name);
        }
        if (!missing.empty()) {
            std::cout << json{
                {"mode", kMode},
                {"ok", false},
                {"db_path", db_path.string()},
                {"missing_tables", missing},
                {"message", "Run --rebuild-duckdb-analytics before building parcel-zone memberships."}
            }.dump(2) << '\n';
            return 1;
        }

        const std::string source_signature = table_exists("analytics_build_info")
            ? scalar_str("SELECT coalesce(max(source_signature), '') FROM analytics_build_info", "analytics source signature")
            : std::string();

        std::ostringstream candidate_sql;
        candidate_sql << R"SQL(
            SELECT
                layer_idx,
                layer_file,
                layer_name,
                coalesce(scale, '') AS scale,
                coalesce(category, '') AS category,
                feature_count
            FROM layer_bboxes
            WHERE coalesce(scale, '') <> 'parcel'
              AND (
                  lower(coalesce(category, '')) LIKE '%zoning%'
               OR lower(coalesce(category, '')) LIKE '%zone%'
               OR lower(coalesce(layer_file, '')) LIKE '%zoning%'
               OR lower(coalesce(layer_file, '')) LIKE '%zone%'
               OR lower(coalesce(layer_file, '')) LIKE '%district%'
               OR lower(coalesce(layer_file, '')) LIKE '%tract%'
               OR lower(coalesce(layer_file, '')) LIKE '%neighborhood%'
               OR lower(coalesce(layer_name, '')) LIKE '%zoning%'
               OR lower(coalesce(layer_name, '')) LIKE '%zone%'
               OR lower(coalesce(layer_name, '')) LIKE '%district%'
               OR lower(coalesce(layer_name, '')) LIKE '%tract%'
               OR lower(coalesce(layer_name, '')) LIKE '%neighborhood%'
              )
        )SQL";
        if (!options.parcel_zone_membership_zone_layer_file.empty()) {
            candidate_sql << " AND layer_file = '"
                          << cliSqlQuote(options.parcel_zone_membership_zone_layer_file)
                          << "'";
        }
        candidate_sql << " ORDER BY layer_idx";

        struct ZoneLayerCandidate {
            uint64_t layer_idx = 0;
            std::string layer_file;
            std::string layer_name;
            std::string scale;
            std::string category;
            uint64_t feature_count = 0;
        };
        std::vector<ZoneLayerCandidate> candidates;
        auto candidate_res = query_or_throw(candidate_sql.str(), "select zone layer candidates");
        for (idx_t row = 0; row < candidate_res->RowCount(); ++row) {
            ZoneLayerCandidate c;
            c.layer_idx = candidate_res->GetValue<uint64_t>(0, row);
            c.layer_file = candidate_res->GetValue(1, row).ToString();
            c.layer_name = candidate_res->GetValue(2, row).ToString();
            c.scale = candidate_res->GetValue(3, row).ToString();
            c.category = candidate_res->GetValue(4, row).ToString();
            c.feature_count = candidate_res->GetValue<uint64_t>(5, row);
            candidates.push_back(std::move(c));
        }

        if (candidates.empty()) {
            const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            std::cout << json{
                {"mode", kMode},
                {"ok", true},
                {"changed", false},
                {"db_path", db_path.string()},
                {"relation", "centroid_in_bbox"},
                {"zone_layer_file_filter", options.parcel_zone_membership_zone_layer_file},
                {"candidate_zone_layers", 0},
                {"inserted_memberships", 0},
                {"processed_layers", json::array()},
                {"elapsed_ms", elapsed_ms}
            }.dump(2) << '\n';
            return 0;
        }

        exec("BEGIN TRANSACTION", "begin parcel-zone membership build");
        exec("DELETE FROM parcel_zone_memberships", "clear parcel_zone_memberships");

        uint64_t total_inserted = 0;
        json processed = json::array();
        for (const ZoneLayerCandidate& c : candidates) {
            const uint64_t before = scalar_u64("SELECT count(*) FROM parcel_zone_memberships", "count memberships before layer");
            std::ostringstream insert_sql;
            insert_sql << R"SQL(
                INSERT INTO parcel_zone_memberships (
                    parcel_layer_idx,
                    parcel_entity_id,
                    parcel_geometry_entity_id,
                    blocklot,
                    zone_layer_idx,
                    zone_layer_file,
                    zone_layer_name,
                    zone_feature_idx,
                    zone_entity_id,
                    zone_key,
                    zone_label,
                    relation,
                    parcel_centroid_lon,
                    parcel_centroid_lat,
                    overlap_area,
                    overlap_ratio,
                    source_signature
                )
                WITH parcels AS (
                    SELECT
                        up.parcel_layer_idx,
                        up.parcel_entity_id,
                        up.parcel_geometry_entity_id,
                        up.blocklot,
                        lfb.center_lon AS parcel_centroid_lon,
                        lfb.center_lat AS parcel_centroid_lat,
                        coalesce(lfb.provenance_state_region, '') AS provenance_state_region,
                        coalesce(lfb.provenance_county_city, '') AS provenance_county_city
                    FROM unified_parcels up
                    JOIN layer_feature_bboxes lfb
                      ON lfb.layer_idx = up.parcel_layer_idx
                     AND lfb.entity_id = up.parcel_entity_id
                    WHERE lfb.scale = 'parcel'
                      AND lfb.duckdb_role = 'parcel_record'
                ),
                zones AS (
                    SELECT
                        zfb.layer_idx,
                        zfb.layer_file,
                        zfb.layer_name,
                        zfb.feature_idx,
                        zfb.entity_id,
                        coalesce(lf.zoning, '') AS zoning,
                        coalesce(lf.feature_name, '') AS feature_name,
                        zfb.min_lon,
                        zfb.min_lat,
                        zfb.max_lon,
                        zfb.max_lat,
                        coalesce(zfb.provenance_state_region, '') AS provenance_state_region,
                        coalesce(zfb.provenance_county_city, '') AS provenance_county_city
                    FROM layer_feature_bboxes zfb
                    JOIN layer_features lf
                      ON lf.layer_idx = zfb.layer_idx
                     AND lf.feature_idx = zfb.feature_idx
                    WHERE zfb.layer_idx = )SQL" << c.layer_idx << R"SQL(
                )
                SELECT
                    p.parcel_layer_idx,
                    p.parcel_entity_id,
                    p.parcel_geometry_entity_id,
                    p.blocklot,
                    z.layer_idx,
                    z.layer_file,
                    z.layer_name,
                    z.feature_idx,
                    z.entity_id,
                    coalesce(nullif(z.zoning, ''), z.entity_id) AS zone_key,
                    coalesce(nullif(z.feature_name, ''), nullif(z.zoning, ''), z.layer_name) AS zone_label,
                    'centroid_in_bbox' AS relation,
                    p.parcel_centroid_lon,
                    p.parcel_centroid_lat,
                    NULL::DOUBLE AS overlap_area,
                    NULL::DOUBLE AS overlap_ratio,
                    ')SQL" << cliSqlQuote(source_signature) << R"SQL(' AS source_signature
                FROM parcels p
                JOIN zones z
                  ON p.parcel_centroid_lon BETWEEN z.min_lon AND z.max_lon
                 AND p.parcel_centroid_lat BETWEEN z.min_lat AND z.max_lat
                 AND (
                        z.provenance_county_city = ''
                     OR p.provenance_county_city = ''
                     OR z.provenance_county_city = p.provenance_county_city
                     OR z.provenance_state_region = p.provenance_state_region
                 )
            )SQL";
            exec(insert_sql.str(), "insert parcel-zone memberships for zone layer");
            const uint64_t after = scalar_u64("SELECT count(*) FROM parcel_zone_memberships", "count memberships after layer");
            const uint64_t inserted = after >= before ? after - before : 0;
            total_inserted += inserted;
            processed.push_back(json{
                {"layer_idx", c.layer_idx},
                {"layer_file", c.layer_file},
                {"layer_name", c.layer_name},
                {"feature_count", c.feature_count},
                {"inserted_memberships", inserted}
            });
        }

        exec("CREATE INDEX IF NOT EXISTS idx_parcel_zone_memberships_parcel ON parcel_zone_memberships(parcel_layer_idx, parcel_entity_id)", "index parcel_zone_memberships parcel");
        exec("CREATE INDEX IF NOT EXISTS idx_parcel_zone_memberships_zone ON parcel_zone_memberships(zone_layer_idx, zone_entity_id)", "index parcel_zone_memberships zone");
        exec("COMMIT", "commit parcel-zone membership build");

        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        std::cout << json{
            {"mode", kMode},
            {"ok", true},
            {"db_path", db_path.string()},
            {"relation", "centroid_in_bbox"},
            {"zone_layer_file_filter", options.parcel_zone_membership_zone_layer_file},
            {"candidate_zone_layers", candidates.size()},
            {"inserted_memberships", total_inserted},
            {"processed_layers", processed},
            {"elapsed_ms", elapsed_ms}
        }.dump(2) << '\n';
        return 0;
    } catch (const std::exception& ex) {
        std::cout << json{
            {"mode", kMode},
            {"ok", false},
            {"error", ex.what()}
        }.dump(2) << '\n';
        return 1;
    }
}

int inspectCanonicalParcelBinary(const fs::path& root, std::string file) {
    if (file.empty()) file = "parcel.geojson";
    if (deprecatedRegionalParcelsAlias(file)) {
        std::cout << deprecatedRegionalParcelsAliasError("inspect-canonical-parcel-binary", file).dump(2) << '\n';
        return 2;
    }
    if (!isBareLayerFilename(file)) {
        std::cout << json{
            {"mode", "inspect-canonical-parcel-binary"},
            {"file", file},
            {"ok", false},
            {"error", "requires a layer filename, not a path"}
        }.dump(2) << '\n';
        return 2;
    }

    const fs::path canonical_path = canonicalLayerPathForFile(root, resolveLayerStorageKey(root, file));
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
    if (file.empty()) file = "parcel.geojson";
    if (deprecatedRegionalParcelsAlias(file)) {
        std::cout << deprecatedRegionalParcelsAliasError("validate-canonical-parcel-binary", file).dump(2) << '\n';
        return 2;
    }
    if (!isBareLayerFilename(file)) {
        std::cout << json{
            {"mode", "validate-canonical-parcel-binary"},
            {"file", file},
            {"ok", false},
            {"error", "requires a layer filename, not a path"}
        }.dump(2) << '\n';
        return 2;
    }

    const std::string storage_key = resolveLayerStorageKey(root, file);
    const fs::path layer_path = resolveStoredLayerPathForFile(root, storage_key);
    if (!layerRuntimeSourceMaterializedForFile(root, storage_key)) {
        std::cout << json{
            {"mode", "validate-canonical-parcel-binary"},
            {"file", file},
            {"ok", false},
            {"error", "canonical layer binary is required for validation"}
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

    std::vector<LayerDef::FeatureRecord> canonical_features;
    std::vector<LayerDef::FeatureProperties> canonical_feature_properties;
    const fs::path canonical_path = canonicalLayerPathForFile(root, storage_key);
    const bool canonical_ok =
        loadBinaryCanonicalFeatureCollection(canonical_path, sig, canonical_features, &canonical_feature_properties);
    CanonicalFeatureCollectionMetadata meta;
    const bool meta_ok = loadBinaryCanonicalMetadata(canonical_path, meta);

    bool representative_match = false;
    if (canonical_ok && !canonical_features.empty() && !canonical_feature_properties.empty()) {
        const FeaturePropertyPairs* canonical_props = getPropertyPairs(canonical_features[0]);
        representative_match =
            canonical_props &&
            canonical_props->size() == canonical_feature_properties[0].values.size() &&
            (canonical_features[0].rings.empty() || nearlyEqual(canonical_features[0].extent.min_lon, canonical_features[0].extent.min_lon)) &&
            (canonical_features[0].rings.empty() || nearlyEqual(canonical_features[0].extent.max_lat, canonical_features[0].extent.max_lat));
    }

    const bool ok =
        canonical_ok &&
        meta_ok &&
        meta.source_signature == sig &&
        meta.feature_count == canonical_features.size() &&
        (canonical_features.empty() || representative_match);

    std::cout << json{
        {"mode", "validate-canonical-parcel-binary"},
        {"file", file},
        {"ok", ok},
        {"source_signature", sig},
        {"source_signature_kind", sig_source_kind},
        {"canonical_source_signature", meta.source_signature},
        {"canonical_features", canonical_features.size()},
        {"canonical_property_rows", canonical_feature_properties.size()},
        {"representative_match", representative_match}
    }.dump(2) << '\n';
    return ok ? 0 : 1;
}

int rebuildDuckDbAnalyticsCli(const fs::path& root, int reserve_cores) {
    constexpr const char* kMode = "rebuild-duckdb-analytics";
    const auto started_at = std::chrono::steady_clock::now();
    std::vector<LayerDef> layers = loadManifest(root);
    const unsigned int hw = std::max(1u, std::thread::hardware_concurrency());
    const unsigned int worker_count = std::max(1u, hw > (unsigned int)std::max(0, reserve_cores) ? hw - (unsigned int)std::max(0, reserve_cores) : 1u);
    emitCliProgress(
        kMode,
        "start",
        "manifest_layers=" + std::to_string(layers.size()) +
            " worker_count=" + std::to_string(worker_count) +
            " reserve_cores=" + std::to_string(std::max(0, reserve_cores)));
    DuckDbAnalytics analytics(root);
    const bool needs_rebuild = analytics.needsRebuild(layers);
    const bool current_artifact_valid =
        !needs_rebuild &&
        (analytics.status().last_rebuild_ok || analytics.validateExistingCache());
    emitCliProgress(
        kMode,
        "check",
        "needs_rebuild=" + std::string(needs_rebuild ? "true" : "false") +
            " current_artifact_valid=" + std::string(current_artifact_valid ? "true" : "false") +
            " db_path=" + analytics.status().db_path);
    if (current_artifact_valid) {
        const double elapsed_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started_at).count();
        emitCliProgress(
            kMode,
            "complete",
            "ok=true reused_existing=true elapsed=" + formatElapsedMs(elapsed_ms));
        json out = {
            {"mode", "rebuild-duckdb-analytics"},
            {"ok", true},
            {"worker_count", worker_count},
            {"reused_existing", true},
            {"elapsed_ms", elapsed_ms},
            {"source_load", {
                {"ok", true},
                {"skipped", true}
            }},
            {"unified_parcels", 0},
            {"duckdb", {
                {"reused_existing", true},
                {"available", analytics.status().available},
                {"last_rebuild_ok", analytics.status().last_rebuild_ok},
                {"layer_count", analytics.status().layer_count},
                {"feature_count", analytics.status().feature_count},
                {"db_path", analytics.status().db_path},
                {"message", "DuckDB analytics cache is current; rebuild skipped."}
            }}
        };
        std::cout << out.dump(2) << '\n';
        return 0;
    }

    const std::vector<size_t> parcel_input_indices = parcelConsolidationInputLayerIndices(detectWorldsimLayerIndices(root, layers));
    LocalLayerLoadSummary load_summary;
    emitCliProgress(
        kMode,
        "load",
        "loading parcel-consolidation inputs only layer_count=" + std::to_string(parcel_input_indices.size()));
    const bool load_ok = loadLocalLayersForCli(root, layers, true, load_summary, true, &parcel_input_indices);
    emitCliProgress(
        kMode,
        "load-complete",
        "requested=" + std::to_string(load_summary.requested_layer_count) +
            " loaded=" + std::to_string(load_summary.loaded_layer_count) +
            " failed=" + std::to_string(load_summary.failed_layer_count) +
            " skipped_missing=" + std::to_string(load_summary.skipped_missing_layer_count) +
            " features=" + std::to_string(load_summary.total_feature_count) +
            " elapsed=" + formatElapsedMs(load_summary.elapsed_ms));

    emitCliProgress(kMode, "parcel-consolidation", "building parcel consolidation artifacts");
    WorldsimLayerIndices indices = detectWorldsimLayerIndices(root, layers);
    ParcelConsolidationArtifacts artifacts = buildParcelConsolidationArtifacts(root, layers, indices);
    emitCliProgress(
        kMode,
        "duckdb",
        "ensuring analytics database artifact from unified_parcels=" + std::to_string(artifacts.unified_parcels.size()));

    const DuckDbArtifactEnsureResult duckdb_result =
        analytics.ensureCurrentArtifact(layers, artifacts.unified_parcels);
    const bool reused_existing = duckdb_result.reused_existing;
    const bool rebuild_ok = duckdb_result.ok;
    const double elapsed_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started_at).count();
    emitCliProgress(
        kMode,
        "complete",
        "ok=" + std::string(rebuild_ok ? "true" : "false") +
            " reused_existing=" + std::string(reused_existing ? "true" : "false") +
            " incrementally_updated=" + std::string(duckdb_result.incrementally_updated ? "true" : "false") +
            " rebuilt=" + std::string(duckdb_result.rebuilt ? "true" : "false") +
            " invalidated=" + std::string(duckdb_result.invalidated ? "true" : "false") +
            " elapsed=" + formatElapsedMs(elapsed_ms) +
            " db_path=" + analytics.status().db_path);

    json out = {
        {"mode", "rebuild-duckdb-analytics"},
        {"ok", rebuild_ok},
        {"worker_count", worker_count},
        {"reused_existing", reused_existing},
        {"elapsed_ms", elapsed_ms},
        {"source_load", {
            {"ok", load_ok},
            {"local_layer_count", load_summary.local_layer_count},
            {"requested_layer_count", load_summary.requested_layer_count},
            {"loaded_layer_count", load_summary.loaded_layer_count},
            {"failed_layer_count", load_summary.failed_layer_count},
            {"skipped_missing_layer_count", load_summary.skipped_missing_layer_count},
            {"total_feature_count", load_summary.total_feature_count},
            {"elapsed_ms", load_summary.elapsed_ms}
        }},
        {"unified_parcels", artifacts.unified_parcels.size()},
        {"duckdb", {
            {"reused_existing", reused_existing},
            {"incrementally_updated", duckdb_result.incrementally_updated},
            {"rebuilt", duckdb_result.rebuilt},
            {"invalidated", duckdb_result.invalidated},
            {"available", analytics.status().available},
            {"last_rebuild_ok", analytics.status().last_rebuild_ok},
            {"layer_count", analytics.status().layer_count},
            {"feature_count", analytics.status().feature_count},
            {"db_path", analytics.status().db_path},
            {"message", analytics.status().message}
        }}
    };
    if (!load_summary.failures.empty()) {
        json failures = json::array();
        for (const auto& failure : load_summary.failures) {
            failures.push_back({
                {"layer_index", failure.layer_index},
                {"layer_file", failure.layer_file},
                {"error", failure.error}
            });
        }
        out["source_load"]["failures"] = std::move(failures);
    }

    std::cout << out.dump(2) << '\n';
    return rebuild_ok ? 0 : 1;
}

int runDuckDbParcelIngestSelftest(const fs::path& root, int reserve_cores) {
    constexpr const char* kMode = "duckdb-parcel-ingest-selftest";
    const auto started_at = std::chrono::steady_clock::now();
    json out = {
        {"mode", kMode},
        {"ok", false}
    };

    std::vector<LayerDef> layers = loadManifest(root, true);
    LocalLayerLoadSummary load_summary;
    const bool load_ok = loadLocalLayersForCli(root, layers, false, load_summary, true);
    out["source_load"] = {
        {"ok", load_ok},
        {"local_layer_count", load_summary.local_layer_count},
        {"requested_layer_count", load_summary.requested_layer_count},
        {"loaded_layer_count", load_summary.loaded_layer_count},
        {"failed_layer_count", load_summary.failed_layer_count},
        {"skipped_missing_layer_count", load_summary.skipped_missing_layer_count},
        {"total_feature_count", load_summary.total_feature_count},
        {"elapsed_ms", load_summary.elapsed_ms}
    };
    if (!load_ok) {
        json failures = json::array();
        for (const auto& failure : load_summary.failures) {
            failures.push_back({
                {"layer_index", failure.layer_index},
                {"layer_file", failure.layer_file},
                {"error", failure.error}
            });
        }
        out["source_load"]["failures"] = std::move(failures);
        out["elapsed_ms"] = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started_at).count();
        std::cout << out.dump(2) << '\n';
        return 1;
    }

    size_t expected_total = 0;
    size_t expected_primary_layer_count = 0;
    std::unordered_map<size_t, uint64_t> expected_by_layer_idx;
    std::unordered_map<size_t, std::string> layer_file_by_idx;
    for (size_t i = 0; i < layers.size(); ++i) {
        if (!isPrimaryParcelGeometryFileForCli(layers[i].file)) continue;
        expected_primary_layer_count += 1;
        expected_total += layers[i].features.size();
        expected_by_layer_idx[i] = (uint64_t)layers[i].features.size();
        layer_file_by_idx[i] = layers[i].file;
    }
    out["expected_primary_parcel_layers"] = expected_primary_layer_count;
    out["expected_primary_parcel_rows"] = expected_total;

    WorldsimLayerIndices indices = detectWorldsimLayerIndices(root, layers);
    ParcelConsolidationArtifacts artifacts = buildParcelConsolidationArtifacts(root, layers, indices);
    out["in_memory_unified_parcels"] = artifacts.unified_parcels.size();

    DuckDbAnalytics analytics(root);
    const bool rebuild_ok = analytics.rebuild(layers, artifacts.unified_parcels);
    out["duckdb"] = {
        {"ok", rebuild_ok},
        {"available", analytics.status().available},
        {"last_rebuild_ok", analytics.status().last_rebuild_ok},
        {"layer_count", analytics.status().layer_count},
        {"feature_count", analytics.status().feature_count},
        {"db_path", analytics.status().db_path},
        {"message", analytics.status().message}
    };
    if (!rebuild_ok) {
        out["elapsed_ms"] = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started_at).count();
        std::cout << out.dump(2) << '\n';
        return 1;
    }

    json failures = json::array();
    try {
        duckdb::DuckDB db(root / "data" / "worldsim.duckdb", nullptr);
        duckdb::Connection con(db);

        auto query_or_throw = [&](const std::string& sql, const char* context) {
            auto res = con.Query(sql);
            if (!res || res->HasError()) {
                throw std::runtime_error(
                    std::string(context) + ": " + (res ? res->GetError() : std::string("query failed")));
            }
            return res;
        };

        const auto unified_total_res = query_or_throw(
            "SELECT count(*) FROM unified_parcels",
            "count unified_parcels");
        const uint64_t unified_total = unified_total_res->GetValue<uint64_t>(0, 0);

        std::unordered_map<size_t, uint64_t> actual_by_layer_idx;
        const auto per_layer_res = query_or_throw(
            "SELECT parcel_layer_idx, count(*) FROM unified_parcels GROUP BY 1",
            "group unified_parcels by parcel layer");
        for (idx_t row = 0; row < per_layer_res->RowCount(); ++row) {
            const size_t layer_idx = (size_t)per_layer_res->GetValue<uint64_t>(0, row);
            const uint64_t count = per_layer_res->GetValue<uint64_t>(1, row);
            actual_by_layer_idx[layer_idx] = count;
        }

        const auto leaked_rows_res = query_or_throw(R"SQL(
            SELECT count(*)
            FROM unified_parcels up
            JOIN layer_features lf
              ON lf.layer_idx = up.parcel_layer_idx
             AND lf.entity_id = up.parcel_entity_id
            WHERE NOT (
                lf.layer_file = 'parcel.geojson' OR
                lf.layer_file LIKE '%_county_parcels.geojson'
            )
        )SQL", "check unified_parcels leakage");
        const uint64_t leaked_rows = leaked_rows_res->GetValue<uint64_t>(0, 0);

        const auto property_attachment_res = query_or_throw(
            "SELECT count(*), coalesce(sum(CASE WHEN has_property_record THEN 1 ELSE 0 END), 0) FROM unified_parcels",
            "check property attachment coverage");
        const uint64_t property_attachment_total = property_attachment_res->GetValue<uint64_t>(0, 0);
        const uint64_t property_attachment_with_record = property_attachment_res->GetValue<uint64_t>(1, 0);

        const auto parcel_record_res = query_or_throw(R"SQL(
            SELECT count(*)
            FROM layer_features
            WHERE duckdb_role = 'parcel_record'
              AND NOT (
                layer_file = 'parcel.geojson' OR
                layer_file LIKE '%_county_parcels.geojson'
              )
        )SQL", "count parcel record rows");
        const uint64_t parcel_record_rows = parcel_record_res->GetValue<uint64_t>(0, 0);

        const auto structure_source_res = query_or_throw(
            "SELECT count(*) FROM layer_features WHERE layer_file = 'real_property_information.geojson' AND structure_area_sqft > 0",
            "count source structure sqft rows");
        const uint64_t structure_source_rows = structure_source_res->GetValue<uint64_t>(0, 0);

        const auto structure_unified_res = query_or_throw(
            "SELECT count(*) FROM unified_parcels WHERE structure_area_sqft > 0",
            "count unified structure sqft rows");
        const uint64_t structure_unified_rows = structure_unified_res->GetValue<uint64_t>(0, 0);

        json per_layer = json::array();
        for (const auto& [layer_idx, expected_count] : expected_by_layer_idx) {
            const uint64_t actual_count = actual_by_layer_idx.count(layer_idx) ? actual_by_layer_idx[layer_idx] : 0;
            const bool match = expected_count == actual_count;
            per_layer.push_back({
                {"layer_idx", layer_idx},
                {"layer_file", layer_file_by_idx[layer_idx]},
                {"expected_count", expected_count},
                {"actual_count", actual_count},
                {"match", match}
            });
            if (!match) {
                failures.push_back({
                    {"type", "layer_count_mismatch"},
                    {"layer_idx", layer_idx},
                    {"layer_file", layer_file_by_idx[layer_idx]},
                    {"expected_count", expected_count},
                    {"actual_count", actual_count}
                });
            }
        }
        for (const auto& [layer_idx, actual_count] : actual_by_layer_idx) {
            if (expected_by_layer_idx.count(layer_idx)) continue;
            failures.push_back({
                {"type", "unexpected_parcel_layer_idx"},
                {"layer_idx", layer_idx},
                {"actual_count", actual_count}
            });
        }
        if (expected_primary_layer_count != 24) {
            failures.push_back({
                {"type", "unexpected_primary_parcel_layer_count"},
                {"expected", 24},
                {"actual", expected_primary_layer_count}
            });
        }
        if (unified_total != expected_total) {
            failures.push_back({
                {"type", "unified_total_mismatch"},
                {"expected_total", expected_total},
                {"actual_total", unified_total}
            });
        }
        if (property_attachment_total != unified_total) {
            failures.push_back({
                {"type", "property_attachment_total_mismatch"},
                {"expected_total", unified_total},
                {"actual_total", property_attachment_total}
            });
        }
        if (leaked_rows != 0) {
            failures.push_back({
                {"type", "non_primary_layer_leakage"},
                {"rows", leaked_rows}
            });
        }
        if (parcel_record_rows > 0 && property_attachment_with_record == 0) {
            failures.push_back({
                {"type", "missing_property_attachments"},
                {"parcel_record_rows", parcel_record_rows},
                {"attached_rows", property_attachment_with_record}
            });
        }
        if (structure_source_rows > 0 && structure_unified_rows == 0) {
            failures.push_back({
                {"type", "missing_structure_area_propagation"},
                {"source_rows", structure_source_rows},
                {"unified_rows", structure_unified_rows}
            });
        }

        out["verification"] = {
            {"expected_primary_parcel_layers", expected_primary_layer_count},
            {"expected_total", expected_total},
            {"unified_total", unified_total},
            {"leaked_rows", leaked_rows},
            {"property_attachment_total", property_attachment_total},
            {"property_attachment_with_record", property_attachment_with_record},
            {"parcel_record_rows", parcel_record_rows},
            {"structure_source_rows", structure_source_rows},
            {"structure_unified_rows", structure_unified_rows},
            {"per_layer", std::move(per_layer)}
        };
    } catch (const std::exception& e) {
        failures.push_back({
            {"type", "verification_query_failed"},
            {"error", e.what()}
        });
    }

    out["failures"] = failures;
    out["ok"] = failures.empty();
    out["elapsed_ms"] = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started_at).count();
    std::cout << out.dump(2) << '\n';
    return failures.empty() ? 0 : 1;
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
            {"repository_sources", table_names.contains("repository_sources")},
            {"import_audit", table_names.contains("import_audit")},
            {"layer_features", table_names.contains("layer_features")},
            {"layer_feature_properties", table_names.contains("layer_feature_properties")}
        };

        out["base_counts"] = {
            {"layer_features_geographic_rows", count_one(
                "SELECT count(*)::BIGINT FROM layer_features "
                "WHERE coalesce(provenance_nation_state, '') <> '' "
                "   OR coalesce(provenance_state_region, '') <> '' "
                "   OR coalesce(provenance_county_city, '') <> ''")},
            {"import_audit_geographic_rows", count_one(
                "SELECT count(*)::BIGINT FROM import_audit "
                "WHERE coalesce(provenance_nation_state, '') <> '' "
                "   OR coalesce(provenance_state_region, '') <> ''")},
            {"repository_sources", count_one("SELECT count(*)::BIGINT FROM repository_sources")}
        };

        out["derivation_diagnostics"] = {
            {"geography_feature_collections_query", count_one(
                "SELECT count(*)::BIGINT FROM ("
                "  SELECT provenance_world, provenance_nation_state, provenance_state_region, provenance_county_city, "
                "         layer_file, layer_name, duckdb_role, scale, category, count(*) AS feature_count "
                "  FROM layer_features "
                "  GROUP BY provenance_world, provenance_nation_state, provenance_state_region, provenance_county_city, "
                "           layer_file, layer_name, duckdb_role, scale, category"
                ") t")}
        };

        if (table_names.contains("geography_feature_collections")) {
            out["materialized_counts"] = {
                {"geography_feature_collections", count_one("SELECT count(*)::BIGINT FROM geography_feature_collections")}
            };
        }
        if (table_names.contains("repository_sources")) {
            out["materialized_counts"]["repository_sources"] =
                count_one("SELECT count(*)::BIGINT FROM repository_sources");
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

int reportDuckDbCoverageCli(const fs::path& root) {
    json out = {
        {"mode", "report-duckdb-coverage"},
        {"db_path", (root / "data" / "worldsim.duckdb").string()}
    };

    const std::vector<LayerDef> layers = loadManifest(root);
    out["manifest_layer_count"] = layers.size();

    std::unordered_map<std::string, uint64_t> layer_feature_counts;
    std::unordered_map<std::string, uint64_t> layer_property_counts;
    std::unordered_map<std::string, uint64_t> import_audit_counts;
    std::unordered_map<std::string, uint64_t> geography_counts;
    std::unordered_map<std::string, uint64_t> parcel_event_counts;
    std::unordered_map<std::string, std::vector<std::string>> columns_by_table;
    json table_presence = json::object();
    bool db_ok = false;

    try {
        const fs::path db_path = root / "data" / "worldsim.duckdb";
        std::error_code ec;
        if (fs::exists(db_path, ec) && !ec) {
            duckdb::DuckDB db(db_path);
            duckdb::Connection con(db);
            const std::vector<std::string> table_names = {
                "layer_features",
                "layer_feature_properties",
                "unified_parcels",
                "parcel_events",
                "import_audit",
                "geography_feature_collections",
                "analytics_build_info"
            };
            columns_by_table = readDuckDbColumnsByTable(db_path, table_names);
            for (const auto& table_name : table_names) {
                table_presence[table_name] = columns_by_table.contains(table_name) && !columns_by_table[table_name].empty();
            }
            layer_feature_counts = readDuckDbCountsByLayerFile(con, "layer_features", "layer_file", "count(*)");
            layer_property_counts = readDuckDbCountsByLayerFile(con, "layer_feature_properties", "layer_file", "count(*)");
            import_audit_counts = readDuckDbCountsByLayerFile(con, "import_audit", "layer_file", "count(*)");
            geography_counts = readDuckDbCountsByLayerFile(con, "geography_feature_collections", "layer_file", "sum(feature_count)");
            parcel_event_counts = readDuckDbCountsByLayerFile(con, "parcel_events", "source_layer_file", "count(*)");
            db_ok = true;
        }
    } catch (const std::exception& e) {
        out["db_error"] = e.what();
    }

    size_t ingest_enabled = 0;
    size_t ingest_enabled_local = 0;
    size_t excluded_ingest_false = 0;
    size_t excluded_missing_local = 0;
    size_t ingested_in_db = 0;

    json layer_rows = json::array();
    for (const auto& layer : layers) {
        const fs::path source_path = resolveStoredLayerPath(root, layer);
        const fs::path canonical_path = canonicalLayerPathForFile(root, layer.file);
        std::error_code ec;
        const bool source_exists = fs::exists(source_path, ec) && !ec;
        ec.clear();
        const bool canonical_exists = fs::exists(canonical_path, ec) && !ec;

        if (layer.duckdb_ingest) ingest_enabled++;
        else excluded_ingest_false++;
        if (layer.duckdb_ingest && canonical_exists) ingest_enabled_local++;

        const uint64_t feature_rows = layer_feature_counts.contains(layer.file) ? layer_feature_counts[layer.file] : 0;
        const uint64_t property_rows = layer_property_counts.contains(layer.file) ? layer_property_counts[layer.file] : 0;
        const uint64_t import_rows = import_audit_counts.contains(layer.file) ? import_audit_counts[layer.file] : 0;
        const uint64_t geography_rows = geography_counts.contains(layer.file) ? geography_counts[layer.file] : 0;
        const uint64_t event_rows = parcel_event_counts.contains(layer.file) ? parcel_event_counts[layer.file] : 0;
        if (feature_rows > 0) ingested_in_db++;

        std::string status;
        std::string reason;
        if (!layer.duckdb_ingest) {
            status = "excluded";
            reason = "duckdb_ingest=false";
        } else if (!canonical_exists) {
            status = "eligible_missing_local";
            reason = "canonical source not materialized";
            excluded_missing_local++;
        } else if (!db_ok) {
            status = "eligible_db_unavailable";
            reason = "DuckDB cache not present or unreadable";
        } else if (feature_rows == 0) {
            status = "eligible_not_in_db";
            reason = "no layer_features rows present";
        } else {
            status = "ingested";
        }

        layer_rows.push_back({
            {"file", layer.file},
            {"name", layer.name},
            {"duckdb_ingest", layer.duckdb_ingest},
            {"duckdb_role", layer.duckdb_role},
            {"runtime_load", layer.runtime_load},
            {"source_path", source_path.string()},
            {"canonical_path", canonical_path.string()},
            {"source_exists", source_exists},
            {"canonical_exists", canonical_exists},
            {"status", status},
            {"reason", reason},
            {"duckdb_rows", {
                {"layer_features", feature_rows},
                {"layer_feature_properties", property_rows},
                {"import_audit", import_rows},
                {"geography_feature_collections", geography_rows},
                {"parcel_events", event_rows}
            }}
        });
    }

    out["db_ok"] = db_ok;
    out["table_presence"] = std::move(table_presence);
    out["summary"] = {
        {"ingest_enabled_layers", ingest_enabled},
        {"ingest_enabled_local_layers", ingest_enabled_local},
        {"ingested_layers_in_db", ingested_in_db},
        {"excluded_duckdb_ingest_false", excluded_ingest_false},
        {"excluded_missing_local", excluded_missing_local}
    };
    if (db_ok) {
        uint64_t total_property_rows = 0;
        for (const auto& kv : layer_property_counts) total_property_rows += kv.second;
        out["summary"]["layer_feature_property_rows"] = total_property_rows;
        out["table_columns"] = columns_by_table;
    }
    out["layers"] = std::move(layer_rows);
    std::cout << out.dump(2) << '\n';
    return 0;
}

json buildGeometryArtifactForLoadedLayer(
    const fs::path& root,
    const LayerDef& layer,
    const std::vector<LayerDef::FeatureRecord>& features,
    int& exit_code) {
    exit_code = 0;
    const fs::path layer_path = resolveStoredLayerPath(root, layer);
    std::string sig;
    std::string sig_source_kind;
    if (!resolveLayerSourceSignature(layer_path, sig, &sig_source_kind)) {
        const bool source_exists = layerRuntimeSourceMaterialized(root, layer);
        if (source_exists) exit_code = 1;
        return {
            {"layer_file", layer.file},
            {"layer_name", layer.name},
            {"ok", false},
            {"skipped", !source_exists},
            {"reason", !source_exists ? "canonical layer binary is not materialized in this workspace" : std::string()},
            {"error", source_exists ? "failed to resolve source signature" : std::string()},
            {"source_path", canonicalLayerPathForFile(root, layer.file).string()}
        };
    }
    if (features.empty()) {
        return {
            {"layer_file", layer.file},
            {"layer_name", layer.name},
            {"ok", false},
            {"skipped", true},
            {"reason", "no source features"},
            {"source_path", layer_path.string()},
            {"source_signature", sig},
            {"source_signature_kind", sig_source_kind}
        };
    }

    const GeometryArtifactClass detected_class = detectGeometryArtifactClass(layer, features);
    const std::vector<const char*> render_paths =
        cliGeometryArtifactRouteNamesForLayer(root, loadManifest(root), layer, detected_class);
    json existing_stats;
    if (render_paths.size() == 1 &&
        loadExistingGeometryArtifactForLayer(root, layer.file, detected_class, sig, existing_stats, render_paths.front())) {
        return {
            {"layer_file", layer.file},
            {"layer_name", layer.name},
            {"geometry_class", geometryArtifactClassName(detected_class)},
            {"ok", true},
            {"reused_existing", true},
            {"source_path", layer_path.string()},
            {"source_signature", sig},
            {"source_signature_kind", sig_source_kind},
            {"created_files", json::array({existing_stats["artifact_path"]})},
            {"feature_count", existing_stats.value("feature_count", 0)},
            {"vertex_count", existing_stats.value("vertex_count", 0)},
            {"fill_index_count", existing_stats.value("fill_index_count", 0)},
            {"line_index_count", existing_stats.value("line_index_count", 0)},
            {"chunk_count", existing_stats.value("chunk_count", 0)},
            {"error", std::string()}
        };
    }

    if (detected_class == GeometryArtifactClass::Point) {
        PointGeometryArtifact artifact;
        if (!buildPointGeometryArtifact(layer, features, sig, artifact)) {
            exit_code = 1;
            return {
                {"layer_file", layer.file},
                {"layer_name", layer.name},
                {"geometry_class", "point"},
                {"ok", false},
                {"error", "failed to build point geometry artifact"},
                {"source_path", layer_path.string()},
                {"source_signature", sig},
                {"source_signature_kind", sig_source_kind}
            };
        }
        const fs::path artifact_path = geometryArtifactCachePathForLayerFile(
            root,
            layer.file,
            GeometryArtifactClass::Point,
            render_paths.front());
        saveBinaryPointGeometryArtifact(artifact_path, artifact);
        PointGeometryArtifact verify;
        const bool ok = loadBinaryPointGeometryArtifact(artifact_path, sig, verify);
        if (!ok) exit_code = 1;
        return {
            {"layer_file", layer.file},
            {"layer_name", layer.name},
            {"geometry_class", "point"},
            {"ok", ok},
            {"reused_existing", false},
            {"source_path", layer_path.string()},
            {"source_signature", sig},
            {"source_signature_kind", sig_source_kind},
            {"created_files", json::array({artifact_path.string()})},
            {"feature_count", artifact.features.size()},
            {"vertex_count", artifact.positions.size()},
            {"chunk_count", artifact.chunks.size()},
            {"error", ok ? std::string() : std::string("artifact validation failed after write")}
        };
    }

    if (detected_class == GeometryArtifactClass::Polyline) {
        PolylineGeometryArtifact artifact;
        if (!buildPolylineGeometryArtifact(layer, features, sig, artifact)) {
            exit_code = 1;
            return {
                {"layer_file", layer.file},
                {"layer_name", layer.name},
                {"geometry_class", "polyline"},
                {"ok", false},
                {"error", "failed to build polyline geometry artifact"},
                {"source_path", layer_path.string()},
                {"source_signature", sig},
                {"source_signature_kind", sig_source_kind}
            };
        }
        const fs::path artifact_path = geometryArtifactCachePathForLayerFile(
            root,
            layer.file,
            GeometryArtifactClass::Polyline,
            render_paths.front());
        saveBinaryPolylineGeometryArtifact(artifact_path, artifact);
        PolylineGeometryArtifact verify;
        const bool ok = loadBinaryPolylineGeometryArtifact(artifact_path, sig, verify);
        if (!ok) exit_code = 1;
        return {
            {"layer_file", layer.file},
            {"layer_name", layer.name},
            {"geometry_class", "polyline"},
            {"ok", ok},
            {"reused_existing", false},
            {"source_path", layer_path.string()},
            {"source_signature", sig},
            {"source_signature_kind", sig_source_kind},
            {"created_files", json::array({artifact_path.string()})},
            {"feature_count", artifact.features.size()},
            {"vertex_count", artifact.vertices.size()},
            {"line_index_count", artifact.line_indices.size()},
            {"chunk_count", artifact.chunks.size()},
            {"error", ok ? std::string() : std::string("artifact validation failed after write")}
        };
    }

    PolygonGeometryArtifact artifact;
    if (!buildPolygonGeometryArtifact(layer, features, sig, artifact)) {
        exit_code = 1;
        return {
            {"layer_file", layer.file},
            {"layer_name", layer.name},
            {"geometry_class", "polygon"},
            {"ok", false},
            {"error", "failed to build polygon geometry artifact"},
            {"source_path", layer_path.string()},
            {"source_signature", sig},
            {"source_signature_kind", sig_source_kind}
        };
    }
    json created_files = json::array();
    bool ok = true;
    for (const char* render_path : render_paths) {
        const fs::path artifact_path = geometryArtifactCachePathForLayerFile(
            root,
            layer.file,
            GeometryArtifactClass::Polygon,
            render_path);
        saveBinaryPolygonGeometryArtifact(artifact_path, artifact);
        PolygonGeometryArtifact verify;
        const bool one_ok = loadBinaryPolygonGeometryArtifact(artifact_path, sig, verify);
        ok = ok && one_ok;
        created_files.push_back(artifact_path.string());
    }
    if (!ok) exit_code = 1;
    return {
        {"layer_file", layer.file},
        {"layer_name", layer.name},
        {"geometry_class", "polygon"},
        {"ok", ok},
        {"reused_existing", false},
        {"source_path", layer_path.string()},
        {"source_signature", sig},
        {"source_signature_kind", sig_source_kind},
        {"created_files", std::move(created_files)},
        {"feature_count", artifact.features.size()},
        {"vertex_count", artifact.vertices.size()},
        {"fill_index_count", artifact.fill_indices.size()},
        {"line_index_count", artifact.line_indices.size()},
        {"chunk_count", artifact.chunks.size()},
        {"error", ok ? std::string() : std::string("artifact validation failed after write")}
    };
}

std::unordered_map<std::string, std::vector<std::string>> readDuckDbColumnsByTable(
    const fs::path& db_path,
    const std::vector<std::string>& table_names) {
    std::unordered_map<std::string, std::vector<std::string>> out;
    if (table_names.empty()) return out;
    duckdb::DuckDB db(db_path);
    duckdb::Connection con(db);
    std::string in_list;
    for (size_t i = 0; i < table_names.size(); ++i) {
        if (i) in_list += ", ";
        in_list += "'" + table_names[i] + "'";
    }
    auto res = con.Query(
        "SELECT table_name, column_name "
        "FROM information_schema.columns "
        "WHERE table_schema = 'main' AND table_name IN (" + in_list + ") "
        "ORDER BY table_name, ordinal_position");
    if (!res || res->HasError()) return out;
    for (auto& name : table_names) out[name] = {};
    for (size_t row = 0; row < (size_t)res->RowCount(); ++row) {
        const std::string table_name = res->GetValue(0, row).ToString();
        const std::string column_name = res->GetValue(1, row).ToString();
        out[table_name].push_back(column_name);
    }
    return out;
}

std::unordered_map<std::string, uint64_t> readDuckDbCountsByLayerFile(
    duckdb::Connection& con,
    const std::string& table_name,
    const std::string& layer_column,
    const std::string& count_expr) {
    std::unordered_map<std::string, uint64_t> out;
    auto res = con.Query(
        "SELECT " + layer_column + ", " + count_expr + "::BIGINT "
        "FROM " + table_name + " "
        "GROUP BY " + layer_column);
    if (!res || res->HasError()) return out;
    for (size_t row = 0; row < (size_t)res->RowCount(); ++row) {
        out[res->GetValue(0, row).ToString()] = res->GetValue<uint64_t>(1, row);
    }
    return out;
}

std::unordered_map<std::string, std::unordered_map<std::string, uint64_t>> readDuckDbSourceContributionCounts(
    duckdb::Connection& con) {
    std::unordered_map<std::string, std::unordered_map<std::string, uint64_t>> out;
    auto res = con.Query(
        "SELECT source_role, source_file, row_count::BIGINT "
        "FROM analytics_source_contributions");
    if (!res || res->HasError()) return out;
    for (size_t row = 0; row < (size_t)res->RowCount(); ++row) {
        const std::string role = res->GetValue(0, row).ToString();
        const std::string file = res->GetValue(1, row).ToString();
        out[file][role] = res->GetValue<uint64_t>(2, row);
    }
    return out;
}

std::unordered_map<std::string, std::unordered_map<std::string, uint64_t>> readUnifiedParcelContributionCounts(
    duckdb::Connection& con) {
    std::unordered_map<std::string, std::unordered_map<std::string, uint64_t>> out;
    auto res = con.Query(
        "SELECT source_role, source_file, row_count::BIGINT FROM ("
        "  SELECT 'parcel_geometry' AS source_role, parcel_source_file AS source_file, count(*) AS row_count "
        "  FROM unified_parcels WHERE parcel_source_file IS NOT NULL AND parcel_source_file <> '' "
        "  GROUP BY parcel_source_file "
        "  UNION ALL "
        "  SELECT 'property_record' AS source_role, property_source_file AS source_file, count(*) AS row_count "
        "  FROM unified_parcels WHERE property_source_file IS NOT NULL AND property_source_file <> '' "
        "  GROUP BY property_source_file"
        ")");
    if (!res || res->HasError()) return out;
    for (size_t row = 0; row < (size_t)res->RowCount(); ++row) {
        const std::string role = res->GetValue(0, row).ToString();
        const std::string file = res->GetValue(1, row).ToString();
        out[file][role] = res->GetValue<uint64_t>(2, row);
    }
    return out;
}

json buildGeometryDuckDbArtifacts(const fs::path& root, int reserve_cores, bool rebuild_all_artifacts_from_scratch) {
    constexpr const char* kMode = "build-geometry-duckdb-artifacts";
    const auto started_at = std::chrono::steady_clock::now();
    json reset_result = json::object();
    if (rebuild_all_artifacts_from_scratch) {
        emitCliProgress(kMode, "reset", "deleting rebuildable geometry and duckdb artifacts before full rebuild");
        reset_result = resetRebuildableArtifacts(root);
        emitCliProgress(
            kMode,
            "reset-complete",
            "ok=" + std::string(reset_result.value("ok", false) ? "true" : "false") +
                " removed=" + std::to_string(reset_result.value("removed", json::array()).size()) +
                " missing=" + std::to_string(reset_result.value("missing", json::array()).size()) +
                " errors=" + std::to_string(reset_result.value("errors", json::array()).size()));
        if (!reset_result.value("ok", false)) {
            return {
                {"mode", kMode},
                {"ok", false},
                {"from_scratch", true},
                {"reset", std::move(reset_result)},
                {"error", "failed to delete one or more rebuildable artifacts"}
            };
        }
    }
    std::vector<LayerDef> layers = loadManifest(root);
    const WorldsimLayerIndices indices = detectWorldsimLayerIndices(root, layers);
    const unsigned int hw = std::max(1u, std::thread::hardware_concurrency());
    const unsigned int worker_count = std::max(1u, hw > (unsigned int)std::max(0, reserve_cores) ? hw - (unsigned int)std::max(0, reserve_cores) : 1u);
    emitCliProgress(
        kMode,
        "start",
        "manifest_layers=" + std::to_string(layers.size()) +
            " worker_count=" + std::to_string(worker_count) +
            " reserve_cores=" + std::to_string(std::max(0, reserve_cores)));

    LocalLayerLoadSummary load_summary;
    emitCliProgress(kMode, "load", "loading local materialized layers");
    const bool load_ok = loadLocalLayersForCli(root, layers, true, load_summary);
    emitCliProgress(
        kMode,
        "load-complete",
        "requested=" + std::to_string(load_summary.requested_layer_count) +
            " loaded=" + std::to_string(load_summary.loaded_layer_count) +
            " failed=" + std::to_string(load_summary.failed_layer_count) +
            " skipped_missing=" + std::to_string(load_summary.skipped_missing_layer_count) +
            " features=" + std::to_string(load_summary.total_feature_count) +
            " elapsed=" + formatElapsedMs(load_summary.elapsed_ms));

    json geometry_results = json::array();
    std::unordered_map<std::string, json> geometry_result_by_file;
    size_t geometry_ok_count = 0;
    size_t geometry_failed_count = 0;
    size_t geometry_skipped_count = 0;
    emitCliProgress(
        kMode,
        "geometry",
        "building compiled geometry artifacts for " + std::to_string(layers.size()) + " manifest layers");
    for (size_t li = 0; li < layers.size(); ++li) {
        int one_exit = 0;
        json one = buildGeometryArtifactForLoadedLayer(root, layers[li], layers[li].features, one_exit);
        geometry_result_by_file[layers[li].file] = one;
        geometry_results.push_back(one);
        if (one.value("skipped", false)) geometry_skipped_count += 1;
        else if (one_exit == 0) geometry_ok_count += 1;
        else geometry_failed_count += 1;
        const std::string status =
            one.value("skipped", false) ? "skipped" : (one_exit == 0 ? "ok" : "failed");
        std::string detail =
            std::to_string(li + 1) + "/" + std::to_string(layers.size()) +
            " " + layers[li].file +
            " status=" + status;
        const std::string geometry_class = one.value("geometry_class", std::string());
        if (!geometry_class.empty()) detail += " class=" + geometry_class;
        const std::string reason = one.value("reason", std::string());
        if (!reason.empty()) detail += " reason=" + reason;
        const std::string error = one.value("error", std::string());
        if (!error.empty()) detail += " error=" + error;
        emitCliProgress(kMode, "geometry", detail);
    }
    emitCliProgress(
        kMode,
        "geometry-complete",
        "ok=" + std::to_string(geometry_ok_count) +
            " failed=" + std::to_string(geometry_failed_count) +
            " skipped=" + std::to_string(geometry_skipped_count));

    const std::vector<size_t> parcel_input_indices = parcelConsolidationInputLayerIndices(indices);
    std::unordered_set<size_t> parcel_input_keep(parcel_input_indices.begin(), parcel_input_indices.end());
    emitCliProgress(
        kMode,
        "memory",
        "releasing non-parcel-consolidation feature bodies before DuckDB stage keep_layers=" +
            std::to_string(parcel_input_keep.size()));
    releaseLoadedLayerFeatures(layers, &parcel_input_keep);

    emitCliProgress(kMode, "duckdb", "building parcel consolidation artifacts");
    ParcelConsolidationArtifacts artifacts = buildParcelConsolidationArtifacts(root, layers, indices);
    emitCliProgress(
        kMode,
        "duckdb",
        "ensuring analytics database artifact from unified_parcels=" + std::to_string(artifacts.unified_parcels.size()));

    DuckDbAnalytics analytics(root);
    const DuckDbArtifactEnsureResult duckdb_result =
        analytics.ensureCurrentArtifact(layers, artifacts.unified_parcels);
    const bool duckdb_reused_existing = duckdb_result.reused_existing;
    const bool duckdb_ok = duckdb_result.ok;
    emitCliProgress(
        kMode,
        "duckdb-complete",
        "ok=" + std::string(duckdb_ok ? "true" : "false") +
            " reused_existing=" + std::string(duckdb_reused_existing ? "true" : "false") +
            " incrementally_updated=" + std::string(duckdb_result.incrementally_updated ? "true" : "false") +
            " rebuilt=" + std::string(duckdb_result.rebuilt ? "true" : "false") +
            " invalidated=" + std::string(duckdb_result.invalidated ? "true" : "false") +
            " db_path=" + analytics.status().db_path +
            " message=" + analytics.status().message);

    const fs::path db_path = root / "data" / "worldsim.duckdb";
        const std::vector<std::string> duckdb_table_names = {
            "layer_features",
            "layer_feature_properties",
            "unified_parcels",
            "parcel_events",
            "analytics_build_info",
            "analytics_source_contributions",
            "repository_sources",
            "import_audit",
            "geography_feature_collections",
            "parcel_features",
            "owner_rollups",
            "layer_counts"
        };
    std::unordered_map<std::string, std::vector<std::string>> columns_by_table;
    std::unordered_map<std::string, uint64_t> layer_feature_counts;
    std::unordered_map<std::string, uint64_t> layer_property_counts;
    std::unordered_map<std::string, uint64_t> import_audit_counts;
    std::unordered_map<std::string, uint64_t> geography_collection_counts;
    std::unordered_map<std::string, uint64_t> parcel_event_counts;
    std::unordered_map<std::string, std::unordered_map<std::string, uint64_t>> source_contributions;
    std::unordered_map<std::string, std::unordered_map<std::string, uint64_t>> unified_parcel_contributions;

    if (duckdb_ok) {
        columns_by_table = readDuckDbColumnsByTable(db_path, duckdb_table_names);
        duckdb::DuckDB db(db_path);
        duckdb::Connection con(db);
        layer_feature_counts = readDuckDbCountsByLayerFile(con, "layer_features", "layer_file", "count(*)");
        layer_property_counts = readDuckDbCountsByLayerFile(con, "layer_feature_properties", "layer_file", "count(*)");
        import_audit_counts = readDuckDbCountsByLayerFile(con, "import_audit", "layer_file", "count(*)");
        geography_collection_counts = readDuckDbCountsByLayerFile(con, "geography_feature_collections", "layer_file", "sum(feature_count)");
        parcel_event_counts = readDuckDbCountsByLayerFile(con, "parcel_events", "source_layer_file", "count(*)");
        source_contributions = readDuckDbSourceContributionCounts(con);
        unified_parcel_contributions = readUnifiedParcelContributionCounts(con);
    }

    json duckdb_tables = json::array();
    for (const auto& table_name : duckdb_table_names) {
        duckdb_tables.push_back({
            {"table", table_name},
            {"columns", columns_by_table.contains(table_name) ? json(columns_by_table[table_name]) : json::array()}
        });
    }

    json layer_outputs = json::array();
    for (const auto& layer : layers) {
        json duckdb_outputs = json::array();
        if (auto it = layer_feature_counts.find(layer.file); it != layer_feature_counts.end()) {
            duckdb_outputs.push_back({
                {"table", "layer_features"},
                {"rows_created", it->second},
                {"columns", columns_by_table["layer_features"]}
            });
        }
        if (auto it = layer_property_counts.find(layer.file); it != layer_property_counts.end()) {
            duckdb_outputs.push_back({
                {"table", "layer_feature_properties"},
                {"rows_created", it->second},
                {"columns", columns_by_table["layer_feature_properties"]}
            });
        }
        if (auto it = import_audit_counts.find(layer.file); it != import_audit_counts.end()) {
            duckdb_outputs.push_back({
                {"table", "import_audit"},
                {"rows_created", it->second},
                {"columns", columns_by_table["import_audit"]}
            });
        }
        if (auto it = geography_collection_counts.find(layer.file); it != geography_collection_counts.end()) {
            duckdb_outputs.push_back({
                {"table", "geography_feature_collections"},
                {"rows_created", it->second},
                {"columns", columns_by_table["geography_feature_collections"]}
            });
        }
        if (auto it = parcel_event_counts.find(layer.file); it != parcel_event_counts.end()) {
            duckdb_outputs.push_back({
                {"table", "parcel_events"},
                {"rows_created", it->second},
                {"columns", columns_by_table["parcel_events"]}
            });
        }
        if (auto it = source_contributions.find(layer.file); it != source_contributions.end()) {
            for (const auto& [role, row_count] : it->second) {
                duckdb_outputs.push_back({
                    {"table", "analytics_source_contributions"},
                    {"source_role", role},
                    {"rows_created", row_count},
                    {"columns", columns_by_table["analytics_source_contributions"]}
                });
            }
        }
        if (auto it = unified_parcel_contributions.find(layer.file); it != unified_parcel_contributions.end()) {
            for (const auto& [role, row_count] : it->second) {
                duckdb_outputs.push_back({
                    {"table", "unified_parcels"},
                    {"source_role", role},
                    {"rows_created", row_count},
                    {"columns", columns_by_table["unified_parcels"]}
                });
            }
        }

        layer_outputs.push_back({
            {"layer_file", layer.file},
            {"layer_name", layer.name},
            {"source_path", resolveStoredLayerPath(root, layer).string()},
            {"geometry_class", geometry_result_by_file.contains(layer.file)
                ? std::string(geometry_result_by_file[layer.file].value("geometry_class", "unknown"))
                : std::string("unknown")},
            {"geometry_output", geometry_result_by_file.contains(layer.file) ? geometry_result_by_file[layer.file] : json::object()},
            {"duckdb_outputs", std::move(duckdb_outputs)}
        });
    }

    const double total_elapsed_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started_at).count();
    emitCliProgress(
        kMode,
        "complete",
        "ok=" + std::string((load_ok && geometry_failed_count == 0 && duckdb_ok) ? "true" : "false") +
            " elapsed=" + formatElapsedMs(total_elapsed_ms));

    json source_load = {
        {"ok", load_ok},
        {"local_layer_count", load_summary.local_layer_count},
        {"requested_layer_count", load_summary.requested_layer_count},
        {"loaded_layer_count", load_summary.loaded_layer_count},
        {"failed_layer_count", load_summary.failed_layer_count},
        {"skipped_missing_layer_count", load_summary.skipped_missing_layer_count},
        {"total_feature_count", load_summary.total_feature_count},
        {"elapsed_ms", load_summary.elapsed_ms}
    };
    if (!load_summary.failures.empty()) {
        json failures = json::array();
        for (const auto& failure : load_summary.failures) {
            failures.push_back({
                {"layer_index", failure.layer_index},
                {"layer_file", failure.layer_file},
                {"error", failure.error}
            });
        }
        source_load["failures"] = std::move(failures);
    }

    return {
        {"mode", "build-geometry-duckdb-artifacts"},
        {"ok", load_ok && geometry_failed_count == 0 && duckdb_ok},
        {"from_scratch", rebuild_all_artifacts_from_scratch},
        {"reset", std::move(reset_result)},
        {"worker_count", worker_count},
        {"source_load", std::move(source_load)},
        {"geometry", {
            {"ok_count", geometry_ok_count},
            {"failed_count", geometry_failed_count},
            {"skipped_count", geometry_skipped_count},
            {"results", std::move(geometry_results)}
        }},
        {"duckdb", {
            {"ok", duckdb_ok},
            {"reused_existing", duckdb_reused_existing},
            {"incrementally_updated", duckdb_result.incrementally_updated},
            {"rebuilt", duckdb_result.rebuilt},
            {"invalidated", duckdb_result.invalidated},
            {"db_path", db_path.string()},
            {"available", analytics.status().available},
            {"last_rebuild_ok", analytics.status().last_rebuild_ok},
            {"layer_count", analytics.status().layer_count},
            {"feature_count", analytics.status().feature_count},
            {"message", analytics.status().message},
            {"tables", std::move(duckdb_tables)}
        }},
        {"layers", std::move(layer_outputs)}
    };
}

int compilePointGeometryArtifact(const fs::path& root, std::string file) {
    if (file.empty()) {
        json out = {
            {"mode", "compile-point-geometry"},
            {"ok", false},
            {"error", "missing layer file"}
        };
        std::cout << out.dump(2) << '\n';
        return 1;
    }
    if (!isBareLayerFilename(file)) {
        json out = {
            {"mode", "compile-point-geometry"},
            {"file", file},
            {"ok", false},
            {"error", "expected bare layer filename"}
        };
        std::cout << out.dump(2) << '\n';
        return 1;
    }

    const std::vector<LayerDef> layers = loadManifest(root);
    const LayerDef* layer = nullptr;
    for (const auto& candidate : layers) {
        if (candidate.file == file) {
            layer = &candidate;
            break;
        }
    }
    if (!layer) {
        json out = {
            {"mode", "compile-point-geometry"},
            {"file", file},
            {"ok", false},
            {"error", "layer file not found in manifest"}
        };
        std::cout << out.dump(2) << '\n';
        return 1;
    }
    if (!layerUsesPointGeometry(*layer)) {
        json out = {
            {"mode", "compile-point-geometry"},
            {"file", file},
            {"ok", false},
            {"error", "layer does not use point geometry"}
        };
        std::cout << out.dump(2) << '\n';
        return 1;
    }

    const fs::path layer_path = resolveStoredLayerPath(root, *layer);
    std::string sig;
    std::string source_used;
    std::vector<LayerDef::FeatureRecord> features;
    std::string error;
    if (!resolveLayerSourceSignature(layer_path, sig, nullptr) ||
        !loadLocalLayerFeatures(root, *layer, features, nullptr, source_used, error)) {
        json out = {{"mode", "compile-point-geometry"}, {"file", file}, {"ok", false}, {"layer_path", canonicalLayerPathForFile(root, file).string()}, {"error", error.empty() ? "failed to load canonical layer features" : error}};
        std::cout << out.dump(2) << '\n';
        return 1;
    }
    if (detectGeometryArtifactClass(*layer, features) != GeometryArtifactClass::Point) {
        json out = {{"mode", "compile-point-geometry"}, {"file", file}, {"ok", false}, {"layer_path", layer_path.string()}, {"error", "layer source geometry is not point"}};
        std::cout << out.dump(2) << '\n';
        return 1;
    }
    const char* render_path = layerRenderRouteArtifactName(LayerRenderRoute::PointGpu);
    json existing_stats;
    if (loadExistingGeometryArtifactForLayer(root, file, GeometryArtifactClass::Point, sig, existing_stats, render_path)) {
        json out = {
            {"mode", "compile-point-geometry"},
            {"file", file},
            {"ok", true},
            {"reused_existing", true},
            {"layer_path", layer_path.string()},
            {"artifact_path", existing_stats["artifact_path"]},
            {"source_signature", sig},
            {"features", existing_stats["feature_count"]},
            {"points", existing_stats["vertex_count"]},
            {"chunks", existing_stats["chunk_count"]}
        };
        std::cout << out.dump(2) << '\n';
        return 0;
    }
    PointGeometryArtifact artifact;
    const bool built = buildPointGeometryArtifact(*layer, features, sig, artifact);
    if (!built) {
        json out = {
            {"mode", "compile-point-geometry"},
            {"file", file},
            {"ok", false},
            {"layer_path", layer_path.string()},
            {"error", "failed to build point geometry artifact"}
        };
        std::cout << out.dump(2) << '\n';
        return 1;
    }

    const fs::path artifact_path =
        geometryArtifactCachePathForLayerFile(root, file, GeometryArtifactClass::Point, render_path);
    saveBinaryPointGeometryArtifact(artifact_path, artifact);
    PointGeometryArtifact loaded;
    const bool roundtrip_ok = loadBinaryPointGeometryArtifact(artifact_path, sig, loaded);

    json out = {
        {"mode", "compile-point-geometry"},
        {"file", file},
        {"ok", roundtrip_ok},
        {"reused_existing", false},
        {"layer_path", layer_path.string()},
        {"artifact_path", artifact_path.string()},
        {"source_signature", sig},
        {"features", artifact.features.size()},
        {"points", artifact.positions.size()},
        {"chunks", artifact.chunks.size()}
    };
    if (!roundtrip_ok) out["error"] = "artifact failed validation after write";
    std::cout << out.dump(2) << '\n';
    return roundtrip_ok ? 0 : 1;
}

int validatePointGeometryArtifact(const fs::path& root, std::string file) {
    if (file.empty()) {
        json out = {
            {"mode", "validate-point-geometry"},
            {"ok", false},
            {"error", "missing layer file"}
        };
        std::cout << out.dump(2) << '\n';
        return 1;
    }
    if (!isBareLayerFilename(file)) {
        json out = {
            {"mode", "validate-point-geometry"},
            {"file", file},
            {"ok", false},
            {"error", "expected bare layer filename"}
        };
        std::cout << out.dump(2) << '\n';
        return 1;
    }

    const std::vector<LayerDef> layers = loadManifest(root);
    const LayerDef* layer = nullptr;
    for (const auto& candidate : layers) {
        if (candidate.file == file) {
            layer = &candidate;
            break;
        }
    }
    if (!layer) {
        json out = {
            {"mode", "validate-point-geometry"},
            {"file", file},
            {"ok", false},
            {"error", "layer file not found in manifest"}
        };
        std::cout << out.dump(2) << '\n';
        return 1;
    }
    if (!layerUsesPointGeometry(*layer)) {
        json out = {
            {"mode", "validate-point-geometry"},
            {"file", file},
            {"ok", false},
            {"error", "layer does not use point geometry"}
        };
        std::cout << out.dump(2) << '\n';
        return 1;
    }

    const fs::path layer_path = resolveStoredLayerPath(root, *layer);
    std::string sig;
    if (!resolveLayerSourceSignature(layer_path, sig, nullptr)) {
        json out = {{"mode", "validate-point-geometry"}, {"file", file}, {"ok", false}, {"layer_path", canonicalLayerPathForFile(root, file).string()}, {"error", "failed to resolve canonical source signature"}};
        std::cout << out.dump(2) << '\n';
        return 1;
    }
    const fs::path artifact_path = geometryArtifactCachePathForLayerFile(
        root,
        file,
        GeometryArtifactClass::Point,
        layerRenderRouteArtifactName(LayerRenderRoute::PointGpu));
    PointGeometryArtifact artifact;
    const bool ok = loadBinaryPointGeometryArtifact(artifact_path, sig, artifact);

    json out = {
        {"mode", "validate-point-geometry"},
        {"file", file},
        {"ok", ok},
        {"layer_path", layer_path.string()},
        {"artifact_path", artifact_path.string()},
        {"source_signature", sig},
        {"artifact_class", geometryArtifactClassName(artifact.header.geometry_class)},
        {"features", artifact.features.size()},
        {"points", artifact.positions.size()},
        {"chunks", artifact.chunks.size()}
    };
    if (!ok) out["error"] = "compiled point geometry artifact missing, stale, or invalid";
    std::cout << out.dump(2) << '\n';
    return ok ? 0 : 1;
}

int compilePolylineGeometryArtifact(const fs::path& root, std::string file) {
    if (file.empty()) {
        json out = {{"mode", "compile-polyline-geometry"}, {"ok", false}, {"error", "missing layer file"}};
        std::cout << out.dump(2) << '\n';
        return 1;
    }
    if (!isBareLayerFilename(file)) {
        json out = {{"mode", "compile-polyline-geometry"}, {"file", file}, {"ok", false}, {"error", "expected bare layer filename"}};
        std::cout << out.dump(2) << '\n';
        return 1;
    }

    const std::vector<LayerDef> layers = loadManifest(root);
    const LayerDef* layer = nullptr;
    for (const auto& candidate : layers) {
        if (candidate.file == file) {
            layer = &candidate;
            break;
        }
    }
    if (!layer) {
        json out = {{"mode", "compile-polyline-geometry"}, {"file", file}, {"ok", false}, {"error", "layer file not found in manifest"}};
        std::cout << out.dump(2) << '\n';
        return 1;
    }
    if (!layerUsesPolylineGeometry(*layer)) {
        json out = {{"mode", "compile-polyline-geometry"}, {"file", file}, {"ok", false}, {"error", "layer does not use polyline geometry"}};
        std::cout << out.dump(2) << '\n';
        return 1;
    }

    const fs::path layer_path = resolveStoredLayerPath(root, *layer);
    std::string sig;
    std::string source_used;
    std::vector<LayerDef::FeatureRecord> features;
    std::string error;
    if (!resolveLayerSourceSignature(layer_path, sig, nullptr) ||
        !loadLocalLayerFeatures(root, *layer, features, nullptr, source_used, error)) {
        json out = {{"mode", "compile-polyline-geometry"}, {"file", file}, {"ok", false}, {"layer_path", canonicalLayerPathForFile(root, file).string()}, {"error", error.empty() ? "failed to load canonical layer features" : error}};
        std::cout << out.dump(2) << '\n';
        return 1;
    }
    if (detectGeometryArtifactClass(*layer, features) != GeometryArtifactClass::Polyline) {
        json out = {{"mode", "compile-polyline-geometry"}, {"file", file}, {"ok", false}, {"layer_path", layer_path.string()}, {"error", "layer source geometry is not polyline"}};
        std::cout << out.dump(2) << '\n';
        return 1;
    }
    const char* render_path = layerRenderRouteArtifactName(LayerRenderRoute::PolylineGpu);
    json existing_stats;
    if (loadExistingGeometryArtifactForLayer(root, file, GeometryArtifactClass::Polyline, sig, existing_stats, render_path)) {
        json out = {
            {"mode", "compile-polyline-geometry"},
            {"file", file},
            {"ok", true},
            {"reused_existing", true},
            {"layer_path", layer_path.string()},
            {"artifact_path", existing_stats["artifact_path"]},
            {"source_signature", sig},
            {"features", existing_stats["feature_count"]},
            {"vertices", existing_stats["vertex_count"]},
            {"line_indices", existing_stats["line_index_count"]},
            {"chunks", existing_stats["chunk_count"]}
        };
        std::cout << out.dump(2) << '\n';
        return 0;
    }
    PolylineGeometryArtifact artifact;
    const bool built = buildPolylineGeometryArtifact(*layer, features, sig, artifact);
    if (!built) {
        json out = {{"mode", "compile-polyline-geometry"}, {"file", file}, {"ok", false}, {"layer_path", layer_path.string()}, {"error", "failed to build polyline geometry artifact"}};
        std::cout << out.dump(2) << '\n';
        return 1;
    }

    const fs::path artifact_path =
        geometryArtifactCachePathForLayerFile(root, file, GeometryArtifactClass::Polyline, render_path);
    saveBinaryPolylineGeometryArtifact(artifact_path, artifact);
    PolylineGeometryArtifact loaded;
    const bool roundtrip_ok = loadBinaryPolylineGeometryArtifact(artifact_path, sig, loaded);
    json out = {
        {"mode", "compile-polyline-geometry"},
        {"file", file},
        {"ok", roundtrip_ok},
        {"reused_existing", false},
        {"layer_path", layer_path.string()},
        {"artifact_path", artifact_path.string()},
        {"source_signature", sig},
        {"features", artifact.features.size()},
        {"vertices", artifact.vertices.size()},
        {"line_indices", artifact.line_indices.size()},
        {"chunks", artifact.chunks.size()}
    };
    if (!roundtrip_ok) out["error"] = "artifact failed validation after write";
    std::cout << out.dump(2) << '\n';
    return roundtrip_ok ? 0 : 1;
}

int validatePolylineGeometryArtifact(const fs::path& root, std::string file) {
    if (file.empty()) {
        json out = {{"mode", "validate-polyline-geometry"}, {"ok", false}, {"error", "missing layer file"}};
        std::cout << out.dump(2) << '\n';
        return 1;
    }
    if (!isBareLayerFilename(file)) {
        json out = {{"mode", "validate-polyline-geometry"}, {"file", file}, {"ok", false}, {"error", "expected bare layer filename"}};
        std::cout << out.dump(2) << '\n';
        return 1;
    }

    const std::vector<LayerDef> layers = loadManifest(root);
    const LayerDef* layer = nullptr;
    for (const auto& candidate : layers) {
        if (candidate.file == file) {
            layer = &candidate;
            break;
        }
    }
    if (!layer) {
        json out = {{"mode", "validate-polyline-geometry"}, {"file", file}, {"ok", false}, {"error", "layer file not found in manifest"}};
        std::cout << out.dump(2) << '\n';
        return 1;
    }
    if (!layerUsesPolylineGeometry(*layer)) {
        json out = {{"mode", "validate-polyline-geometry"}, {"file", file}, {"ok", false}, {"error", "layer does not use polyline geometry"}};
        std::cout << out.dump(2) << '\n';
        return 1;
    }

    const fs::path layer_path = resolveStoredLayerPath(root, *layer);
    std::string sig;
    if (!resolveLayerSourceSignature(layer_path, sig, nullptr)) {
        json out = {{"mode", "validate-polyline-geometry"}, {"file", file}, {"ok", false}, {"layer_path", canonicalLayerPathForFile(root, file).string()}, {"error", "failed to resolve canonical source signature"}};
        std::cout << out.dump(2) << '\n';
        return 1;
    }
    const fs::path artifact_path = geometryArtifactCachePathForLayerFile(
        root,
        file,
        GeometryArtifactClass::Polyline,
        layerRenderRouteArtifactName(LayerRenderRoute::PolylineGpu));
    PolylineGeometryArtifact artifact;
    const bool ok = loadBinaryPolylineGeometryArtifact(artifact_path, sig, artifact);
    json out = {
        {"mode", "validate-polyline-geometry"},
        {"file", file},
        {"ok", ok},
        {"layer_path", layer_path.string()},
        {"artifact_path", artifact_path.string()},
        {"source_signature", sig},
        {"artifact_class", geometryArtifactClassName(artifact.header.geometry_class)},
        {"features", artifact.features.size()},
        {"vertices", artifact.vertices.size()},
        {"line_indices", artifact.line_indices.size()},
        {"chunks", artifact.chunks.size()}
    };
    if (!ok) out["error"] = "compiled polyline geometry artifact missing, stale, or invalid";
    std::cout << out.dump(2) << '\n';
    return ok ? 0 : 1;
}

int compilePolygonGeometryArtifact(const fs::path& root, std::string file) {
    if (file.empty()) {
        json out = {
            {"mode", "compile-polygon-geometry"},
            {"ok", false},
            {"error", "missing layer file"}
        };
        std::cout << out.dump(2) << '\n';
        return 1;
    }
    if (!isBareLayerFilename(file)) {
        json out = {
            {"mode", "compile-polygon-geometry"},
            {"file", file},
            {"ok", false},
            {"error", "expected bare layer filename"}
        };
        std::cout << out.dump(2) << '\n';
        return 1;
    }

    const std::vector<LayerDef> layers = loadManifest(root);
    const LayerDef* layer = nullptr;
    for (const auto& candidate : layers) {
        if (candidate.file == file) {
            layer = &candidate;
            break;
        }
    }
    if (!layer) {
        json out = {
            {"mode", "compile-polygon-geometry"},
            {"file", file},
            {"ok", false},
            {"error", "layer file not found in manifest"}
        };
        std::cout << out.dump(2) << '\n';
        return 1;
    }
    if (layerUsesPointGeometry(*layer)) {
        json out = {
            {"mode", "compile-polygon-geometry"},
            {"file", file},
            {"ok", false},
            {"error", "layer uses point geometry"}
        };
        std::cout << out.dump(2) << '\n';
        return 1;
    }

    const fs::path layer_path = resolveStoredLayerPath(root, *layer);
    std::string sig;
    std::string source_used;
    std::vector<LayerDef::FeatureRecord> features;
    std::string error;
    if (!resolveLayerSourceSignature(layer_path, sig, nullptr) ||
        !loadLocalLayerFeatures(root, *layer, features, nullptr, source_used, error)) {
        json out = {{"mode", "compile-polygon-geometry"}, {"file", file}, {"ok", false}, {"layer_path", canonicalLayerPathForFile(root, file).string()}, {"error", error.empty() ? "failed to load canonical layer features" : error}};
        std::cout << out.dump(2) << '\n';
        return 1;
    }
    if (detectGeometryArtifactClass(*layer, features) != GeometryArtifactClass::Polygon) {
        json out = {{"mode", "compile-polygon-geometry"}, {"file", file}, {"ok", false}, {"layer_path", layer_path.string()}, {"error", "layer source geometry is not polygon"}};
        std::cout << out.dump(2) << '\n';
        return 1;
    }
    const std::vector<const char*> render_paths =
        cliGeometryArtifactRouteNamesForLayer(root, layers, *layer, GeometryArtifactClass::Polygon);
    json existing_stats;
    if (render_paths.size() == 1 &&
        loadExistingGeometryArtifactForLayer(root, file, GeometryArtifactClass::Polygon, sig, existing_stats, render_paths.front())) {
        json out = {
            {"mode", "compile-polygon-geometry"},
            {"file", file},
            {"ok", true},
            {"reused_existing", true},
            {"layer_path", layer_path.string()},
            {"artifact_path", existing_stats["artifact_path"]},
            {"source_signature", sig},
            {"features", existing_stats["feature_count"]},
            {"vertices", existing_stats["vertex_count"]},
            {"fill_indices", existing_stats["fill_index_count"]},
            {"line_indices", existing_stats["line_index_count"]},
            {"chunks", existing_stats["chunk_count"]}
        };
        std::cout << out.dump(2) << '\n';
        return 0;
    }
    PolygonGeometryArtifact artifact;
    const bool built = buildPolygonGeometryArtifact(*layer, features, sig, artifact);
    if (!built) {
        json out = {
            {"mode", "compile-polygon-geometry"},
            {"file", file},
            {"ok", false},
            {"layer_path", layer_path.string()},
            {"error", "failed to build polygon geometry artifact"}
        };
        std::cout << out.dump(2) << '\n';
        return 1;
    }

    json created_files = json::array();
    bool roundtrip_ok = true;
    for (const char* render_path : render_paths) {
        const fs::path artifact_path =
            geometryArtifactCachePathForLayerFile(root, file, GeometryArtifactClass::Polygon, render_path);
        saveBinaryPolygonGeometryArtifact(artifact_path, artifact);
        PolygonGeometryArtifact loaded;
        const bool one_ok = loadBinaryPolygonGeometryArtifact(artifact_path, sig, loaded);
        roundtrip_ok = roundtrip_ok && one_ok;
        created_files.push_back(artifact_path.string());
    }

    json out = {
        {"mode", "compile-polygon-geometry"},
        {"file", file},
        {"ok", roundtrip_ok},
        {"reused_existing", false},
        {"layer_path", layer_path.string()},
        {"artifact_path", created_files.empty() ? std::string() : created_files.front().get<std::string>()},
        {"artifact_paths", created_files},
        {"source_signature", sig},
        {"features", artifact.features.size()},
        {"vertices", artifact.vertices.size()},
        {"fill_indices", artifact.fill_indices.size()},
        {"line_indices", artifact.line_indices.size()},
        {"chunks", artifact.chunks.size()}
    };
    if (!roundtrip_ok) out["error"] = "artifact failed validation after write";
    std::cout << out.dump(2) << '\n';
    return roundtrip_ok ? 0 : 1;
}

int compileParcelPolygonGeometryArtifacts(const fs::path& root) {
    std::vector<LayerDef> layers = loadManifest(root);
    json results = json::array();
    size_t candidate_count = 0;
    size_t ok_count = 0;
    size_t failed_count = 0;
    size_t skipped_count = 0;

    for (LayerDef& layer : layers) {
        if (!isPrimaryParcelGeometryFileForCli(layer.file)) continue;
        candidate_count += 1;

        const fs::path layer_path = resolveStoredLayerPath(root, layer);
        std::string sig;
        std::string source_kind;
        if (!resolveLayerSourceSignature(layer_path, sig, &source_kind)) {
            skipped_count += 1;
            results.push_back({
                {"layer_file", layer.file},
                {"layer_name", layer.name},
                {"ok", false},
                {"skipped", true},
                {"reason", "canonical layer binary is not materialized"},
                {"source_path", canonicalLayerPathForFile(root, layer.file).string()}
            });
            continue;
        }

        std::vector<LayerDef::FeatureRecord> features;
        std::string source_used;
        std::string error;
        if (!loadLocalLayerFeatures(root, layer.file, sig, features, nullptr, source_used, error)) {
            failed_count += 1;
            results.push_back({
                {"layer_file", layer.file},
                {"layer_name", layer.name},
                {"ok", false},
                {"skipped", false},
                {"error", error.empty() ? "failed to load canonical layer features" : error},
                {"source_path", canonicalLayerPathForFile(root, layer.file).string()},
                {"source_signature", sig},
                {"source_signature_kind", source_kind}
            });
            continue;
        }

        int one_exit = 0;
        json one = buildGeometryArtifactForLoadedLayer(root, layer, features, one_exit);
        results.push_back(one);
        if (one.value("skipped", false)) skipped_count += 1;
        else if (one_exit == 0) ok_count += 1;
        else failed_count += 1;
    }

    json out = {
        {"mode", "compile-parcel-polygon-geometry-artifacts"},
        {"ok", failed_count == 0},
        {"candidate_count", candidate_count},
        {"ok_count", ok_count},
        {"failed_count", failed_count},
        {"skipped_count", skipped_count},
        {"results", std::move(results)}
    };
    std::cout << out.dump(2) << '\n';
    return failed_count == 0 ? 0 : 1;
}

int validatePolygonGeometryArtifact(const fs::path& root, std::string file) {
    if (file.empty()) {
        json out = {
            {"mode", "validate-polygon-geometry"},
            {"ok", false},
            {"error", "missing layer file"}
        };
        std::cout << out.dump(2) << '\n';
        return 1;
    }
    if (!isBareLayerFilename(file)) {
        json out = {
            {"mode", "validate-polygon-geometry"},
            {"file", file},
            {"ok", false},
            {"error", "expected bare layer filename"}
        };
        std::cout << out.dump(2) << '\n';
        return 1;
    }

    const std::vector<LayerDef> layers = loadManifest(root);
    const LayerDef* layer = nullptr;
    for (const auto& candidate : layers) {
        if (candidate.file == file) {
            layer = &candidate;
            break;
        }
    }
    if (!layer) {
        json out = {
            {"mode", "validate-polygon-geometry"},
            {"file", file},
            {"ok", false},
            {"error", "layer file not found in manifest"}
        };
        std::cout << out.dump(2) << '\n';
        return 1;
    }
    if (layerUsesPointGeometry(*layer)) {
        json out = {
            {"mode", "validate-polygon-geometry"},
            {"file", file},
            {"ok", false},
            {"error", "layer uses point geometry"}
        };
        std::cout << out.dump(2) << '\n';
        return 1;
    }

    const fs::path layer_path = resolveStoredLayerPath(root, *layer);
    std::string sig;
    if (!resolveLayerSourceSignature(layer_path, sig, nullptr)) {
        json out = {{"mode", "validate-polygon-geometry"}, {"file", file}, {"ok", false}, {"layer_path", canonicalLayerPathForFile(root, file).string()}, {"error", "failed to resolve canonical source signature"}};
        std::cout << out.dump(2) << '\n';
        return 1;
    }
    const LayerRenderRoute render_route = cliRenderRouteForLayer(root, layers, *layer);
    const fs::path artifact_path = geometryArtifactCachePathForLayerFile(
        root,
        file,
        GeometryArtifactClass::Polygon,
        layerRenderRouteArtifactName(render_route));
    PolygonGeometryArtifact artifact;
    const bool ok = loadBinaryPolygonGeometryArtifact(artifact_path, sig, artifact);

    json out = {
        {"mode", "validate-polygon-geometry"},
        {"file", file},
        {"ok", ok},
        {"layer_path", layer_path.string()},
        {"artifact_path", artifact_path.string()},
        {"source_signature", sig},
        {"artifact_class", geometryArtifactClassName(artifact.header.geometry_class)},
        {"features", artifact.features.size()},
        {"vertices", artifact.vertices.size()},
        {"fill_indices", artifact.fill_indices.size()},
        {"line_indices", artifact.line_indices.size()},
        {"chunks", artifact.chunks.size()}
    };
    if (!ok) out["error"] = "compiled polygon geometry artifact missing, stale, or invalid";
    std::cout << out.dump(2) << '\n';
    return ok ? 0 : 1;
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
        if (arg == "--status-api-parcel-debug-selftest") {
            options.run_status_api_parcel_debug_selftest = true;
            continue;
        }
        if (arg == "--parcel-gpu-cpu-bypass-selftest") {
            options.run_parcel_gpu_cpu_bypass_selftest = true;
            continue;
        }
        if (arg == "--parcel-query-layer-style-selftest") {
            options.run_parcel_query_layer_style_selftest = true;
            continue;
        }
        if (arg == "--vulkan-device-loss-selftest") {
            options.run_vulkan_device_loss_selftest = true;
            continue;
        }
        if (arg == "--render-routing-selftest") {
            options.run_render_routing_selftest = true;
            continue;
        }
        if (arg == "--render-tile-cache-selftest") {
            options.run_render_tile_cache_selftest = true;
            continue;
        }
        if (arg == "--render-polygon-tile-runtime-policy-selftest") {
            options.run_render_polygon_tile_runtime_policy_selftest = true;
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
        if (arg == "--parcel-polygon-identity-selftest") {
            options.run_parcel_polygon_identity_selftest = true;
            continue;
        }
        if (arg == "--polygon-artifact-resilience-selftest") {
            options.run_polygon_artifact_resilience_selftest = true;
            continue;
        }
        if (arg == "--parcel-polygon-feature-idx-selftest") {
            options.run_parcel_polygon_feature_idx_selftest = true;
            continue;
        }
        if (arg == "--parcel-hover-click-pick-selftest") {
            options.run_parcel_hover_click_pick_selftest = true;
            continue;
        }
        if (arg == "--parcel-selection-ui-harness") {
            options.run_parcel_selection_ui_harness = true;
            continue;
        }
        if (arg == "--parcel-hover-click-ui-harness") {
            options.run_parcel_hover_click_ui_harness = true;
            continue;
        }
        if (arg == "--verify-parcel-duckdb-keys") {
            options.run_verify_parcel_duckdb_keys = true;
            continue;
        }
        if (arg == "--duckdb-parcel-semantic-snapshot-selftest") {
            options.run_duckdb_parcel_semantic_snapshot_selftest = true;
            continue;
        }
        if (arg == "--duckdb-parcel-ingest-selftest") {
            options.run_duckdb_parcel_ingest_selftest = true;
            continue;
        }
        if (arg == "--population-metrics-selftest") {
            options.run_population_metrics_selftest = true;
            continue;
        }
        if (arg == "--beps-screening-selftest") {
            options.run_beps_screening_selftest = true;
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
        if (arg == "--compile-point-geometry") {
            options.run_compile_point_geometry = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') options.point_geometry_file = argv[++i];
            continue;
        }
        if (arg == "--validate-point-geometry") {
            options.run_validate_point_geometry = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') options.point_geometry_file = argv[++i];
            continue;
        }
        if (arg == "--compile-polyline-geometry") {
            options.run_compile_polyline_geometry = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') options.polyline_geometry_file = argv[++i];
            continue;
        }
        if (arg == "--validate-polyline-geometry") {
            options.run_validate_polyline_geometry = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') options.polyline_geometry_file = argv[++i];
            continue;
        }
        if (arg == "--compile-polygon-geometry") {
            options.run_compile_polygon_geometry = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') options.polygon_geometry_file = argv[++i];
            continue;
        }
        if (arg == "--compile-parcel-polygon-geometry-artifacts" ||
            arg == "--compile-county-parcel-geometry") {
            options.run_compile_parcel_polygon_geometry_artifacts = true;
            continue;
        }
        if (arg == "--validate-polygon-geometry") {
            options.run_validate_polygon_geometry = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') options.polygon_geometry_file = argv[++i];
            continue;
        }
        if (arg == "--render-polygon-tile") {
            options.run_render_polygon_tile = true;
            if (i + 4 < argc) {
                options.render_polygon_tile_file = argv[++i];
                options.render_tile_z = std::atoi(argv[++i]);
                options.render_tile_x = std::atoi(argv[++i]);
                options.render_tile_y = std::atoi(argv[++i]);
            }
            continue;
        }
        if (arg == "--build-geometry-duckdb-artifacts") {
            options.run_build_geometry_duckdb_artifacts = true;
            continue;
        }
        if (arg == "--startup-preprocess") {
            options.run_startup_preprocess = true;
            continue;
        }
        if (arg == "--from-scratch" || arg == "--rebuild-all-artifacts-from-scratch") {
            options.rebuild_all_artifacts_from_scratch = true;
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
        if (arg.rfind("--compile-point-geometry=", 0) == 0) {
            options.run_compile_point_geometry = true;
            options.point_geometry_file = arg.substr(std::strlen("--compile-point-geometry="));
            continue;
        }
        if (arg.rfind("--validate-point-geometry=", 0) == 0) {
            options.run_validate_point_geometry = true;
            options.point_geometry_file = arg.substr(std::strlen("--validate-point-geometry="));
            continue;
        }
        if (arg.rfind("--compile-polyline-geometry=", 0) == 0) {
            options.run_compile_polyline_geometry = true;
            options.polyline_geometry_file = arg.substr(std::strlen("--compile-polyline-geometry="));
            continue;
        }
        if (arg.rfind("--validate-polyline-geometry=", 0) == 0) {
            options.run_validate_polyline_geometry = true;
            options.polyline_geometry_file = arg.substr(std::strlen("--validate-polyline-geometry="));
            continue;
        }
        if (arg.rfind("--compile-polygon-geometry=", 0) == 0) {
            options.run_compile_polygon_geometry = true;
            options.polygon_geometry_file = arg.substr(std::strlen("--compile-polygon-geometry="));
            continue;
        }
        if (arg.rfind("--validate-polygon-geometry=", 0) == 0) {
            options.run_validate_polygon_geometry = true;
            options.polygon_geometry_file = arg.substr(std::strlen("--validate-polygon-geometry="));
            continue;
        }
        if (arg.rfind("--render-polygon-tile=", 0) == 0) {
            options.run_render_polygon_tile = true;
            options.render_polygon_tile_file = arg.substr(std::strlen("--render-polygon-tile="));
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
        if (arg == "--generate-canonical-files") {
            options.run_generate_canonical_files = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                options.generate_canonical_phase = argv[++i];
            } else {
                options.generate_canonical_phase = "all";
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
        if (arg == "--report-duckdb-coverage") {
            options.run_report_duckdb_coverage = true;
            continue;
        }
        if (arg == "--build-parcel-matched-layers") {
            options.run_build_parcel_matched_layers = true;
            continue;
        }
        if (arg == "--build-parcel-zone-memberships") {
            options.run_build_parcel_zone_memberships = true;
            continue;
        }
        if (arg == "--zone-layer-file" || arg == "--parcel-zone-layer-file") {
            options.run_build_parcel_zone_memberships = true;
            if (i + 1 < argc) options.parcel_zone_membership_zone_layer_file = argv[++i];
            continue;
        }
        if (arg == "--build-population-metrics") {
            options.run_build_population_metrics = true;
            continue;
        }
        if (arg == "--build-beps-candidates") {
            options.run_build_beps_candidates = true;
            continue;
        }
        if (arg == "--color-editor") {
            options.run_color_editor = true;
            if (i + 1 < argc) options.color_editor_session_file = argv[++i];
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
        if (arg.rfind("--generate-canonical-files=", 0) == 0) {
            options.run_generate_canonical_files = true;
            options.generate_canonical_phase = arg.substr(std::strlen("--generate-canonical-files="));
            continue;
        }
        if (arg.rfind("--zone-layer-file=", 0) == 0) {
            options.run_build_parcel_zone_memberships = true;
            options.parcel_zone_membership_zone_layer_file = arg.substr(std::strlen("--zone-layer-file="));
            continue;
        }
        if (arg.rfind("--parcel-zone-layer-file=", 0) == 0) {
            options.run_build_parcel_zone_memberships = true;
            options.parcel_zone_membership_zone_layer_file = arg.substr(std::strlen("--parcel-zone-layer-file="));
            continue;
        }
        if (arg.rfind("--color-editor=", 0) == 0) {
            options.run_color_editor = true;
            options.color_editor_session_file = arg.substr(std::strlen("--color-editor="));
            continue;
        }
        if (arg == "--include-large") {
            options.include_large_downloads = true;
            continue;
        }
        if (arg == "--debug-gpu-aggregate") {
            options.debug_gpu_aggregate = true;
            setWorldsimGpuAggregateDebug(true);
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
        << "       worldsim3 [--download-layers [all|must-have|nice-to-have|heavy-data|beps|capital-flows|anambra-runtime|anambra-repository|extended-events|historical-high-quality|archival-research]] [--include-large]\n"
        << "       worldsim3 [--generate-canonical-files [all|must-have|nice-to-have|heavy-data|beps|capital-flows|anambra-runtime|anambra-repository|extended-events|historical-high-quality|archival-research]] [--include-large]\n"
        << "       worldsim3 --rebuild-duckdb-analytics [--reserve-cores N]\n"
        << "       worldsim3 --inspect-duckdb-geography-tables\n"
        << "       worldsim3 --report-duckdb-coverage\n"
        << "       worldsim3 [--build-parcel-matched-layers|--force-build-parcel-matched-layers]\n"
        << "       worldsim3 --build-parcel-zone-memberships [--zone-layer-file LAYER_FILE]\n"
        << "       worldsim3 --build-population-metrics\n"
        << "       worldsim3 --build-beps-candidates\n"
        << "       worldsim3 [--debug-gpu-aggregate]\n"
        << "       worldsim3 --canonical-parcel-binary-selftest\n"
        << "       worldsim3 --inspect-canonical-parcel-binary [LAYER_FILE]\n"
        << "       worldsim3 --validate-canonical-parcel-binary [LAYER_FILE]\n"
        << "       worldsim3 --parcel-artifact-health [LAYER_FILE]\n"
        << "       worldsim3 --compile-point-geometry LAYER_FILE\n"
        << "       worldsim3 --validate-point-geometry LAYER_FILE\n"
        << "       worldsim3 --compile-polyline-geometry LAYER_FILE\n"
        << "       worldsim3 --validate-polyline-geometry LAYER_FILE\n"
        << "       worldsim3 --compile-polygon-geometry LAYER_FILE\n"
        << "       worldsim3 --compile-parcel-polygon-geometry-artifacts\n"
        << "       worldsim3 --validate-polygon-geometry LAYER_FILE\n"
        << "       worldsim3 --render-polygon-tile LAYER_FILE Z X Y\n"
        << "       worldsim3 --build-geometry-duckdb-artifacts [--from-scratch] [--reserve-cores N]\n"
        << "       worldsim3 --startup-preprocess [--from-scratch] [--reserve-cores N]\n"
        << "       worldsim3 --projection-cache-selftest\n"
        << "       worldsim3 --projection-fill-cache-selftest\n"
        << "       worldsim3 --projection-color-cache-selftest\n"
        << "       worldsim3 --polygon-hole-selftest\n"
        << "       worldsim3 --spatial-index-selftest\n"
        << "       worldsim3 --layer-profile-selftest\n"
        << "       worldsim3 --layer-runtime-status-selftest\n"
        << "       worldsim3 --status-api-parcel-debug-selftest\n"
        << "       worldsim3 --parcel-gpu-cpu-bypass-selftest\n"
        << "       worldsim3 --parcel-query-layer-style-selftest\n"
        << "       worldsim3 --vulkan-device-loss-selftest\n"
        << "       worldsim3 --render-routing-selftest\n"
        << "       worldsim3 --render-tile-cache-selftest\n"
        << "       worldsim3 --render-polygon-tile-runtime-policy-selftest\n"
        << "       worldsim3 --render-policy-selftest\n"
        << "       worldsim3 --render-plan-selftest\n"
        << "       worldsim3 --parcel-polygon-identity-selftest\n"
        << "       worldsim3 --polygon-artifact-resilience-selftest\n"
        << "       worldsim3 --parcel-polygon-feature-idx-selftest\n"
        << "       worldsim3 --parcel-hover-click-pick-selftest\n"
        << "       worldsim3 --parcel-selection-ui-harness\n"
        << "       worldsim3 --parcel-hover-click-ui-harness\n"
        << "       worldsim3 --verify-parcel-duckdb-keys\n"
        << "       worldsim3 --duckdb-parcel-semantic-snapshot-selftest\n"
        << "       worldsim3 --duckdb-parcel-ingest-selftest [--reserve-cores N]\n"
        << "       worldsim3 --population-metrics-selftest\n"
        << "       worldsim3 --beps-screening-selftest\n"
        << "       worldsim3 --vacancy-selftest\n";
}

int runWorldsimCliImmediate(const fs::path& root, const WorldsimCliOptions& options) {
    applyDotEnvEnvironment(root);
    if (options.debug_gpu_aggregate) setWorldsimGpuAggregateDebug(true);
    if (options.show_help) {
        printWorldsimUsage();
        return 0;
    }
    if (options.run_vacancy_selftest) {
        return runVacancySelftest(root);
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
    if (options.run_spatial_index_selftest) {
        return runSpatialIndexSelftest();
    }
    if (options.run_layer_profile_selftest) {
        return runLayerProfileSelftest();
    }
    if (options.run_layer_runtime_status_selftest) {
        return runLayerRuntimeStatusSelftest();
    }
    if (options.run_status_api_parcel_debug_selftest) {
        return runStatusApiParcelDebugSelftest();
    }
    if (options.run_parcel_gpu_cpu_bypass_selftest) {
        return runParcelGpuCpuBypassSelftest();
    }
    if (options.run_parcel_query_layer_style_selftest) {
        return runParcelQueryLayerStyleSelftest();
    }
    if (options.run_vulkan_device_loss_selftest) {
        return runVulkanDeviceLossSelftest();
    }
    if (options.run_render_routing_selftest) {
        return runRenderRoutingSelftest(root);
    }
    if (options.run_render_tile_cache_selftest) {
        return runRenderTileCacheSelftest(root);
    }
    if (options.run_render_polygon_tile_runtime_policy_selftest) {
        return runRenderPolygonTileRuntimePolicySelftest();
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
    if (options.run_parcel_polygon_identity_selftest) {
        return runParcelPolygonIdentitySelftest();
    }
    if (options.run_polygon_artifact_resilience_selftest) {
        return runPolygonArtifactResilienceSelftest();
    }
    if (options.run_parcel_polygon_feature_idx_selftest) {
        return runParcelPolygonFeatureIdxSelftest();
    }
    if (options.run_parcel_hover_click_pick_selftest) {
        return runParcelHoverClickPickSelftest(root);
    }
    if (options.run_parcel_selection_ui_harness) {
        return runParcelSelectionUiHarness();
    }
    if (options.run_parcel_hover_click_ui_harness) {
        return runParcelHoverClickUiHarness(root);
    }
    if (options.run_verify_parcel_duckdb_keys) {
        return verifyParcelDuckDbKeys(root);
    }
    if (options.run_duckdb_parcel_semantic_snapshot_selftest) {
        return runDuckDbParcelSemanticSnapshotSelftest(root);
    }
    if (options.run_duckdb_parcel_ingest_selftest) {
        return runDuckDbParcelIngestSelftest(root, options.reserve_cores_set ? options.reserve_cores : 0);
    }
    if (options.run_population_metrics_selftest) {
        return runPopulationMetricsSelftest();
    }
    if (options.run_beps_screening_selftest) {
        return runBepsScreeningSelftest();
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
    if (options.run_compile_point_geometry) {
        return compilePointGeometryArtifact(root, options.point_geometry_file);
    }
    if (options.run_validate_point_geometry) {
        return validatePointGeometryArtifact(root, options.point_geometry_file);
    }
    if (options.run_compile_polyline_geometry) {
        return compilePolylineGeometryArtifact(root, options.polyline_geometry_file);
    }
    if (options.run_validate_polyline_geometry) {
        return validatePolylineGeometryArtifact(root, options.polyline_geometry_file);
    }
    if (options.run_compile_polygon_geometry) {
        return compilePolygonGeometryArtifact(root, options.polygon_geometry_file);
    }
    if (options.run_compile_parcel_polygon_geometry_artifacts) {
        return compileParcelPolygonGeometryArtifacts(root);
    }
    if (options.run_validate_polygon_geometry) {
        return validatePolygonGeometryArtifact(root, options.polygon_geometry_file);
    }
    if (options.run_render_polygon_tile) {
        return renderPolygonTile(root, options);
    }
    if (options.run_build_geometry_duckdb_artifacts) {
        const json out = buildGeometryDuckDbArtifacts(
            root,
            options.reserve_cores_set ? options.reserve_cores : 0,
            options.rebuild_all_artifacts_from_scratch);
        std::cout << out.dump(2) << '\n';
        return out.value("ok", false) ? 0 : 1;
    }
    if (options.run_download_layers) {
        return runLayerDownloadCli(
            root,
            options.download_phase.empty() ? "all" : options.download_phase,
            options.include_large_downloads);
    }
    if (options.run_generate_canonical_files) {
        return generateCanonicalFilesCli(
            root,
            options.generate_canonical_phase.empty() ? "all" : options.generate_canonical_phase,
            options.include_large_downloads,
            options.reserve_cores_set ? options.reserve_cores : 0);
    }
    if (options.run_rebuild_duckdb_analytics) {
        return rebuildDuckDbAnalyticsCli(root, options.reserve_cores_set ? options.reserve_cores : 0);
    }
    if (options.run_inspect_duckdb_geography_tables) {
        return inspectDuckDbGeographyTablesCli(root);
    }
    if (options.run_report_duckdb_coverage) {
        return reportDuckDbCoverageCli(root);
    }
    if (options.run_build_parcel_zone_memberships) {
        return buildParcelZoneMembershipsCli(root, options);
    }
    if (options.run_build_population_metrics) {
        return runBuildPopulationMetricsCli(root);
    }
    if (options.run_build_beps_candidates) {
        return runBuildBepsCandidatesCli(root);
    }
    if (options.run_build_parcel_matched_layers) {
        ensureParcelMatchedEventLayers(root, options.force_build_parcel_matched_layers, &std::cout);
        return 0;
    }
    return -1;
}
