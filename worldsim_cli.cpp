#include "worldsim_cli.h"

#include "app_utils.h"
#include "cache_io.h"
#include "duckdb_analytics.h"
#include "env_config.h"
#include "feature_props.h"
#include "layer_import.h"
#include "layer_geometry.h"
#include "layer_state_io.h"
#include "layer_pipeline_drain.h"
#include "map_inspection.h"
#include "map_render_selection.h"
#include "map_render_projection.h"
#include "parcel_consolidation.h"
#include "selection.h"
#include "profiling_layer_snapshot.h"
#include "render_layer_pass.h"
#include "render_plan_builder.h"
#include "render_policy.h"
#include "worldsim_dataset_bootstrap.h"
#include "worldsim_app.h"
#include "parcel_matched_layers.h"
#include "vacancy_overlay.h"

#include <duckdb.hpp>
#include <imgui_internal.h>
#include <algorithm>
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

bool isPrimaryParcelGeometryFileForCli(const std::string& file) {
    return file == "parcel.geojson" ||
           (file.size() > std::strlen("_county_parcels.geojson") &&
            file.ends_with("_county_parcels.geojson"));
}

std::unordered_map<std::string, std::vector<std::string>> readDuckDbColumnsByTable(
    const fs::path& db_path,
    const std::vector<std::string>& table_names);
std::unordered_map<std::string, uint64_t> readDuckDbCountsByLayerFile(
    duckdb::Connection& con,
    const std::string& table_name,
    const std::string& layer_column,
    const std::string& count_expr);

bool loadExistingGeometryArtifactForLayer(
    const fs::path& root,
    const std::string& file,
    GeometryArtifactClass cls,
    const std::string& sig,
    json& stats_out) {
    const fs::path artifact_path = geometryArtifactCachePathForLayerFile(root, file, cls);
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
    const bool artifact_shape_ok =
        artifact_ok &&
        artifact.vertices.size() == flattened.size() &&
        artifact.fill_indices.size() == feature.triangles.size() &&
        artifact.line_indices.size() == 16 &&
        artifact.features.size() == 1 &&
        artifact.chunks.size() == 1;

    json out = {
        {"mode", "polygon-hole-selftest"},
        {"ok", shell_point_inside && hole_point_rejected && outside_point_rejected && centroids_valid && artifact_shape_ok},
        {"shell_point_inside", shell_point_inside},
        {"hole_point_rejected", hole_point_rejected},
        {"outside_point_rejected", outside_point_rejected},
        {"triangle_index_count", feature.triangles.size()},
        {"triangle_count", centroid_count},
        {"centroids_valid", centroids_valid},
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
    const fs::path polygon_path = geometryArtifactCachePathForLayerFile(root, file, GeometryArtifactClass::Polygon);
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
        exec("CREATE TABLE parcel_events(blocklot VARCHAR, event_date VARCHAR, event_type VARCHAR, event_status VARCHAR, amount_usd DOUBLE, source_layer_name VARCHAR, source_layer_file VARCHAR)");
        exec(R"SQL(
            INSERT INTO layer_features VALUES
            (9, 'Parcels', 'parcel.geojson', 'parcel_record', 0, 'entity:a', 'parcel', 'Housing', '', '', '', '', -76.70, 39.20, -76.69, 39.21, 'BLK1', 'owner a', '1 Main', '21201', 'ACTIVE', '', '', 100000, 1200, '', '', '', '', '', '', 0, 0),
            (9, 'Parcels', 'parcel.geojson', 'parcel_record', 1, 'entity:b', 'parcel', 'Housing', '', '', '', '', -76.68, 39.20, -76.67, 39.21, 'BLK2', 'owner b', '2 Main', '21202', 'ACTIVE', '', '', 200000, 1500, '', '', '', '', '', '', 0, 0)
        )SQL");
        exec(R"SQL(
            INSERT INTO unified_parcels VALUES
            (9, 'entity:a', 'geom:a', 'BLK1', 'parcel.geojson', 'rp.geojson', TRUE, TRUE, 'owner a', 'Owner A', '1 Main', '1main', '21201', 'ACTIVE', 1000, 2000, 1200, 300000, 0, 300000, 2, 1, 3, 0, 4500, 0, -76.70, 39.20, -76.69, 39.21),
            (9, 'entity:b', 'geom:b', 'BLK2', 'parcel.geojson', '', TRUE, FALSE, 'owner b', 'Owner B', '2 Main', '2main', '21202', 'ACTIVE', 0, 0, 1500, 200000, 0, 200000, 0, 0, 0, 1, 0, 1200, -76.68, 39.20, -76.67, 39.21)
        )SQL");

        DuckDbAnalytics analytics(test_root);
        const bool cache_ok = analytics.validateExistingCache();
        const DuckDbParcelSemanticSnapshot snapshot = analytics.loadParcelSemanticSnapshot(9);
        const bool ok =
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
        exec("CREATE TABLE layer_features(layer_idx UBIGINT, layer_name VARCHAR, layer_file VARCHAR, duckdb_role VARCHAR, feature_idx UBIGINT, entity_id VARCHAR, scale VARCHAR, category VARCHAR, provenance_world VARCHAR, provenance_nation_state VARCHAR, provenance_state_region VARCHAR, provenance_county_city VARCHAR, min_lon DOUBLE, min_lat DOUBLE, max_lon DOUBLE, max_lat DOUBLE, blocklot VARCHAR, owner VARCHAR, address VARCHAR, zipcode VARCHAR, status VARCHAR, zoning VARCHAR, jurisdiction VARCHAR, value_usd DOUBLE, structure_area_sqft DOUBLE, feature_name VARCHAR, lga_name VARCHAR, ward_name VARCHAR, source_name VARCHAR, event_date_text VARCHAR, event_status_hint VARCHAR, event_year_hint INTEGER, amount_usd_hint DOUBLE)");
        exec("CREATE TABLE layer_feature_properties(layer_idx UBIGINT, layer_name VARCHAR, layer_file VARCHAR, duckdb_role VARCHAR, feature_idx UBIGINT, entity_id VARCHAR, property_key VARCHAR, property_value VARCHAR)");
        exec("CREATE TABLE unified_parcels(parcel_layer_idx UBIGINT, parcel_entity_id VARCHAR, parcel_geometry_entity_id VARCHAR, blocklot VARCHAR, parcel_source_file VARCHAR, property_source_file VARCHAR, parcel_has_geometry BOOLEAN, has_property_record BOOLEAN, owner VARCHAR, owner_display VARCHAR, address VARCHAR, address_search VARCHAR, zipcode VARCHAR, status VARCHAR, current_land DOUBLE, current_improvements DOUBLE, structure_area_sqft DOUBLE, tax_base DOUBLE, sale_price DOUBLE, current_value DOUBLE, vacant_notice_count INTEGER, vacant_rehab_count INTEGER, tax_lien_count INTEGER, tax_sale_count INTEGER, tax_lien_amount DOUBLE, tax_sale_amount DOUBLE, min_lon DOUBLE, min_lat DOUBLE, max_lon DOUBLE, max_lat DOUBLE)");
        exec("CREATE TABLE parcel_events(blocklot VARCHAR, event_date VARCHAR, event_type VARCHAR, event_status VARCHAR, amount_usd DOUBLE, source_layer_name VARCHAR, source_layer_file VARCHAR)");
        exec(R"SQL(
            INSERT INTO layer_features VALUES
            (10, 'Baltimore County Parcels', 'baltimore_county_parcels.geojson', 'parcel_record', 1, 'ENTITYCOUNTY1', 'parcel', 'Housing', '', '', '', '', -76.70, 39.20, -76.69, 39.21, 'BC-1', 'county owner', '10 County St', '21211', 'ACTIVE', '', '', 150000, 900, '', '', '', '', '', '', 0, 0)
        )SQL");

        DuckDbAnalytics analytics(test_root);
        const bool cache_ok = analytics.validateExistingCache();

        std::vector<LayerDef> layers(2);
        layers[0].file = "parcel.geojson";
        layers[0].enabled = true;
        layers[1].file = "baltimore_county_parcels.geojson";
        layers[1].enabled = true;
        layers[1].features.resize(2);
        layers[1].features[1].entity_id = "ENTITYCOUNTY1";
        layers[1].features[1].geometry_entity_id = "geom:county:1";
        layers[1].features[1].source_feature_id = "src:county:1";

        MapHoverState hover_state;
        hover_state.hovered_parcel_layer_idx = 1;
        hover_state.hovered_parcel_idx = 1;
        hover_state.hovered_parcel_entity_id = "ENTITYCOUNTY1";

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
            hovered.hit &&
            hovered.layer_idx == 1 &&
            hovered.feature_idx == 1 &&
            !hovered.entity_id.empty() &&
            detail.available &&
            detail.blocklot == "BC-1" &&
            detail.owner_display == "county owner" &&
            detail.address == "10 County St" &&
            detail.tax_lien_count == 0 &&
            click_ok &&
            selection.active_layer_idx == 1 &&
            selection.active_entity_id == hovered.entity_id &&
            opened_entity_id == hovered.entity_id;

        std::cout << json{
            {"mode", kMode},
            {"ok", ok},
            {"cache_ok", cache_ok},
            {"hover_hit", hovered.hit},
            {"hover_entity_id", hovered.entity_id},
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
    const bool current_artifact_valid = !needs_rebuild && analytics.validateExistingCache();
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
    json existing_stats;
    if (loadExistingGeometryArtifactForLayer(root, layer.file, detected_class, sig, existing_stats)) {
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
        const fs::path artifact_path = geometryArtifactCachePathForLayerFile(root, layer.file, GeometryArtifactClass::Point);
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
        const fs::path artifact_path = geometryArtifactCachePathForLayerFile(root, layer.file, GeometryArtifactClass::Polyline);
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
    const fs::path artifact_path = geometryArtifactCachePathForLayerFile(root, layer.file, GeometryArtifactClass::Polygon);
    saveBinaryPolygonGeometryArtifact(artifact_path, artifact);
    PolygonGeometryArtifact verify;
    const bool ok = loadBinaryPolygonGeometryArtifact(artifact_path, sig, verify);
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
        {"created_files", json::array({artifact_path.string()})},
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

json buildGeometryDuckDbArtifacts(const fs::path& root, int reserve_cores) {
    constexpr const char* kMode = "build-geometry-duckdb-artifacts";
    const auto started_at = std::chrono::steady_clock::now();
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
    json existing_stats;
    if (loadExistingGeometryArtifactForLayer(root, file, GeometryArtifactClass::Point, sig, existing_stats)) {
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

    const fs::path artifact_path = geometryArtifactCachePathForLayerFile(root, file, GeometryArtifactClass::Point);
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
    const fs::path artifact_path = geometryArtifactCachePathForLayerFile(root, file, GeometryArtifactClass::Point);
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
    json existing_stats;
    if (loadExistingGeometryArtifactForLayer(root, file, GeometryArtifactClass::Polyline, sig, existing_stats)) {
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

    const fs::path artifact_path = geometryArtifactCachePathForLayerFile(root, file, GeometryArtifactClass::Polyline);
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
    const fs::path artifact_path = geometryArtifactCachePathForLayerFile(root, file, GeometryArtifactClass::Polyline);
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
    json existing_stats;
    if (loadExistingGeometryArtifactForLayer(root, file, GeometryArtifactClass::Polygon, sig, existing_stats)) {
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

    const fs::path artifact_path = geometryArtifactCachePathForLayerFile(root, file, GeometryArtifactClass::Polygon);
    saveBinaryPolygonGeometryArtifact(artifact_path, artifact);
    PolygonGeometryArtifact loaded;
    const bool roundtrip_ok = loadBinaryPolygonGeometryArtifact(artifact_path, sig, loaded);

    json out = {
        {"mode", "compile-polygon-geometry"},
        {"file", file},
        {"ok", roundtrip_ok},
        {"reused_existing", false},
        {"layer_path", layer_path.string()},
        {"artifact_path", artifact_path.string()},
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
    const fs::path artifact_path = geometryArtifactCachePathForLayerFile(root, file, GeometryArtifactClass::Polygon);
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
        if (arg == "--parcel-polygon-identity-selftest") {
            options.run_parcel_polygon_identity_selftest = true;
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
        if (arg == "--duckdb-parcel-semantic-snapshot-selftest") {
            options.run_duckdb_parcel_semantic_snapshot_selftest = true;
            continue;
        }
        if (arg == "--duckdb-parcel-ingest-selftest") {
            options.run_duckdb_parcel_ingest_selftest = true;
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
        if (arg == "--build-geometry-duckdb-artifacts") {
            options.run_build_geometry_duckdb_artifacts = true;
            continue;
        }
        if (arg == "--startup-preprocess") {
            options.run_startup_preprocess = true;
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
        if (arg.rfind("--color-editor=", 0) == 0) {
            options.run_color_editor = true;
            options.color_editor_session_file = arg.substr(std::strlen("--color-editor="));
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
        << "       worldsim3 [--generate-canonical-files [all|must-have|nice-to-have|heavy-data|capital-flows|anambra-runtime|anambra-repository|extended-events|historical-high-quality|archival-research]] [--include-large]\n"
        << "       worldsim3 --rebuild-duckdb-analytics [--reserve-cores N]\n"
        << "       worldsim3 --inspect-duckdb-geography-tables\n"
        << "       worldsim3 --report-duckdb-coverage\n"
        << "       worldsim3 [--build-parcel-matched-layers|--force-build-parcel-matched-layers]\n"
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
        << "       worldsim3 --build-geometry-duckdb-artifacts [--reserve-cores N]\n"
        << "       worldsim3 --startup-preprocess [--reserve-cores N]\n"
        << "       worldsim3 --projection-cache-selftest\n"
        << "       worldsim3 --projection-fill-cache-selftest\n"
        << "       worldsim3 --projection-color-cache-selftest\n"
        << "       worldsim3 --polygon-hole-selftest\n"
        << "       worldsim3 --spatial-index-selftest\n"
        << "       worldsim3 --layer-profile-selftest\n"
        << "       worldsim3 --layer-runtime-status-selftest\n"
        << "       worldsim3 --parcel-gpu-cpu-bypass-selftest\n"
        << "       worldsim3 --render-policy-selftest\n"
        << "       worldsim3 --render-plan-selftest\n"
        << "       worldsim3 --parcel-polygon-identity-selftest\n"
        << "       worldsim3 --parcel-selection-ui-harness\n"
        << "       worldsim3 --parcel-hover-click-ui-harness\n"
        << "       worldsim3 --duckdb-parcel-semantic-snapshot-selftest\n"
        << "       worldsim3 --duckdb-parcel-ingest-selftest [--reserve-cores N]\n"
        << "       worldsim3 --vacancy-selftest\n";
}

int runWorldsimCliImmediate(const fs::path& root, const WorldsimCliOptions& options) {
    applyDotEnvEnvironment(root);
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
    if (options.run_parcel_polygon_identity_selftest) {
        return runParcelPolygonIdentitySelftest();
    }
    if (options.run_parcel_selection_ui_harness) {
        return runParcelSelectionUiHarness();
    }
    if (options.run_parcel_hover_click_ui_harness) {
        return runParcelHoverClickUiHarness(root);
    }
    if (options.run_duckdb_parcel_semantic_snapshot_selftest) {
        return runDuckDbParcelSemanticSnapshotSelftest(root);
    }
    if (options.run_duckdb_parcel_ingest_selftest) {
        return runDuckDbParcelIngestSelftest(root, options.reserve_cores_set ? options.reserve_cores : 0);
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
    if (options.run_build_geometry_duckdb_artifacts) {
        const json out = buildGeometryDuckDbArtifacts(root, options.reserve_cores_set ? options.reserve_cores : 0);
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
    if (options.run_build_parcel_matched_layers) {
        ensureParcelMatchedEventLayers(root, options.force_build_parcel_matched_layers, &std::cout);
        return 0;
    }
    return -1;
}
