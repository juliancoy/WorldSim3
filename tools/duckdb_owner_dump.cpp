#include "app_utils.h"
#include "cpu_affinity.h"
#include "duckdb_analytics.h"
#include "headless_layer_hydration.h"
#include "layer_state_io.h"
#include "memory_utils.h"
#include "parcel_consolidation.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
struct OwnerDumpOptions {
    bool show_help = false;
    unsigned int workers = 0;
    int reserve_cores = 0;
    bool verbose = true;
    bool summary_only = false;
    fs::path output_path;
};

std::optional<unsigned int> parseUnsigned(const char* value) {
    if (!value || !*value) return std::nullopt;
    char* end = nullptr;
    unsigned long parsed = std::strtoul(value, &end, 10);
    if (!end || *end != '\0' || parsed > std::numeric_limits<unsigned int>::max()) return std::nullopt;
    return (unsigned int)parsed;
}

std::optional<int> parseInt(const char* value) {
    if (!value || !*value) return std::nullopt;
    char* end = nullptr;
    long parsed = std::strtol(value, &end, 10);
    if (!end || *end != '\0' || parsed < std::numeric_limits<int>::min() || parsed > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return (int)parsed;
}

void printUsage() {
    std::cout
        << "Usage: worldsim3_duckdb_owner_dump [--workers N] [--reserve-cores N] [--quiet]\n"
        << "                                  [--summary-only] [--output PATH]\n"
        << "  Rebuilds the initial consolidated DuckDB parcel view from local layer files,\n"
        << "  finds the owner with the most properties, and prints all of that owner's\n"
        << "  unified parcel rows.\n";
}

bool parseOptions(int argc, char** argv, OwnerDumpOptions& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            options.show_help = true;
            return true;
        }
        if (arg == "--quiet") {
            options.verbose = false;
            continue;
        }
        if (arg == "--summary-only") {
            options.summary_only = true;
            continue;
        }
        if (arg == "--output") {
            if (i + 1 >= argc) {
                std::cerr << "--output requires a path\n";
                return false;
            }
            options.output_path = argv[++i];
            continue;
        }
        if (arg.rfind("--output=", 0) == 0) {
            options.output_path = arg.substr(std::string("--output=").size());
            continue;
        }
        if (arg == "--workers") {
            if (i + 1 >= argc) {
                std::cerr << "--workers requires a value\n";
                return false;
            }
            auto parsed = parseUnsigned(argv[++i]);
            if (!parsed || *parsed == 0) {
                std::cerr << "Invalid --workers value\n";
                return false;
            }
            options.workers = *parsed;
            continue;
        }
        if (arg.rfind("--workers=", 0) == 0) {
            auto parsed = parseUnsigned(arg.c_str() + std::string("--workers=").size());
            if (!parsed || *parsed == 0) {
                std::cerr << "Invalid --workers value\n";
                return false;
            }
            options.workers = *parsed;
            continue;
        }
        if (arg == "--reserve-cores") {
            if (i + 1 >= argc) {
                std::cerr << "--reserve-cores requires a value\n";
                return false;
            }
            auto parsed = parseInt(argv[++i]);
            if (!parsed || *parsed < 0) {
                std::cerr << "Invalid --reserve-cores value\n";
                return false;
            }
            options.reserve_cores = *parsed;
            continue;
        }
        if (arg.rfind("--reserve-cores=", 0) == 0) {
            auto parsed = parseInt(arg.c_str() + std::string("--reserve-cores=").size());
            if (!parsed || *parsed < 0) {
                std::cerr << "Invalid --reserve-cores value\n";
                return false;
            }
            options.reserve_cores = *parsed;
            continue;
        }
        std::cerr << "Unknown argument: " << arg << '\n';
        return false;
    }
    return true;
}

unsigned int chooseWorkerCount(unsigned int requested) {
    if (requested > 0) return requested;
    const unsigned int hw = std::max(1u, std::thread::hardware_concurrency());
    return std::min(4u, hw);
}

void printQueryResult(const DuckDbQueryResult& result) {
    for (size_t i = 0; i < result.columns.size(); ++i) {
        if (i > 0) std::cout << '\t';
        std::cout << result.columns[i];
    }
    std::cout << '\n';
    for (const auto& row : result.rows) {
        for (size_t i = 0; i < row.size(); ++i) {
            if (i > 0) std::cout << '\t';
            std::cout << row[i];
        }
        std::cout << '\n';
    }
}

void writeQueryResultTsv(std::ostream& out, const DuckDbQueryResult& result) {
    for (size_t i = 0; i < result.columns.size(); ++i) {
        if (i > 0) out << '\t';
        out << result.columns[i];
    }
    out << '\n';
    for (const auto& row : result.rows) {
        for (size_t i = 0; i < row.size(); ++i) {
            if (i > 0) out << '\t';
            out << row[i];
        }
        out << '\n';
    }
}

std::string joinStrings(const std::set<std::string>& values) {
    std::ostringstream out;
    bool first = true;
    for (const auto& value : values) {
        if (!first) out << ", ";
        out << value;
        first = false;
    }
    return out.str();
}

std::string diagnoseZeroFeatureLayer(const fs::path& path) {
    std::ifstream in(path);
    if (!in) return "unable to open source file for diagnosis";
    json source;
    try {
        in >> source;
    } catch (const std::exception& e) {
        return std::string("source file is not valid JSON: ") + e.what();
    }
    if (!source.contains("features") || !source["features"].is_array()) {
        return "source file does not contain a GeoJSON features array";
    }
    const auto& features = source["features"];
    if (features.empty()) return "source file has an empty features array";

    std::set<std::string> geometry_types;
    size_t null_geometry_count = 0;
    for (const auto& feature : features) {
        if (!feature.contains("geometry") || feature["geometry"].is_null()) {
            null_geometry_count += 1;
            continue;
        }
        const auto& geometry = feature["geometry"];
        if (!geometry.contains("type") || !geometry["type"].is_string()) {
            null_geometry_count += 1;
            continue;
        }
        geometry_types.insert(geometry["type"].get<std::string>());
    }

    if (geometry_types.empty()) return "source features have no usable geometry objects";
    const bool all_supported = std::all_of(geometry_types.begin(), geometry_types.end(), [](const std::string& type) {
        return type == "Point" || type == "MultiPoint" || type == "Polygon" || type == "MultiPolygon";
    });
    if (!all_supported) {
        return "unsupported geometry types present: " + joinStrings(geometry_types) +
               " (supported: Point, MultiPoint, Polygon, MultiPolygon)";
    }
    if (null_geometry_count > 0) {
        return "all supported geometry rows were skipped; some source features have null or malformed geometry";
    }
    return "supported geometry types were present, but no features were extracted";
}
}

int main(int argc, char** argv) {
    OwnerDumpOptions options;
    if (!parseOptions(argc, argv, options)) return 1;
    if (options.show_help) {
        printUsage();
        return 0;
    }

    configureProcessAllocatorForWorldSim();

    const fs::path root = resolveAppRoot(fs::current_path(), argc > 0 ? argv[0] : nullptr);
    const unsigned int worker_count = chooseWorkerCount(options.workers);
    std::cerr << "Resolving app root: " << root << '\n';
    std::cerr << "Hydration workers: " << worker_count << '\n';

    if (options.reserve_cores > 0) {
        std::string affinity_message;
        if (applyReservedCpuCores(options.reserve_cores, affinity_message)) {
            if (!affinity_message.empty()) std::cerr << affinity_message << '\n';
        } else if (!affinity_message.empty()) {
            std::cerr << "CPU affinity: " << affinity_message << '\n';
        }
    }

    std::vector<LayerDef> layers = loadManifest(root);
    if (layers.empty()) {
        std::cerr << "Failed to load layers manifest from " << root << '\n';
        return 1;
    }

    std::cerr << "Hydrating locally available layers using worker/cache pipeline...\n";
    HeadlessLayerHydrationSummary hydration_summary;
    const auto hydration_started_at = std::chrono::steady_clock::now();
    if (!hydrateLocalLayersHeadless(
            root,
            layers,
            HeadlessLayerHydrationOptions{worker_count, options.verbose},
            hydration_summary)) {
        std::cerr << "Hydration failed for " << hydration_summary.failed_layer_count << " layer(s).\n";
        for (const auto& failure : hydration_summary.failures) {
            std::cerr << "  " << failure.layer_file << ": " << failure.error << '\n';
        }
        return 1;
    }
    const double hydration_wall_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - hydration_started_at).count();
    std::vector<std::pair<std::string, std::string>> zero_feature_diagnostics;
    for (size_t idx : hydration_summary.requested_indices) {
        if (idx >= layers.size() || !layers[idx].features.empty()) continue;
        zero_feature_diagnostics.push_back({
            layers[idx].file,
            diagnoseZeroFeatureLayer(resolveStoredLayerPath(root, layers[idx]))
        });
    }

    const WorldsimLayerIndices layer_indices = detectWorldsimLayerIndices(root, layers);
    if (layer_indices.parcel_layer_idx < 0) {
        std::cerr << "Could not identify the parcel layer in the manifest.\n";
        return 1;
    }

    std::cerr << "Building parcel consolidation artifacts...\n";
    ParcelConsolidationArtifacts artifacts = buildParcelConsolidationArtifacts(root, layers, layer_indices);
    if (artifacts.unified_parcels.empty()) {
        std::cerr << "Unified parcel consolidation produced no rows. Parcel features loaded: "
                  << layers[(size_t)layer_indices.parcel_layer_idx].features.size() << '\n';
        return 1;
    }

    std::cerr << "Rebuilding DuckDB analytics cache...\n";
    DuckDbAnalytics duckdb(root);
    const auto rebuild_started_at = std::chrono::steady_clock::now();
    if (!duckdb.rebuild(layers, artifacts.unified_parcels)) {
        std::cerr << duckdb.status().message << '\n';
        return 1;
    }
    const double rebuild_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - rebuild_started_at).count();

    std::cerr << "Querying top owner...\n";
    const DuckDbQueryResult top_owner = duckdb.executeMapQuery(
        R"SQL(
            SELECT
                owner,
                min(owner_display) AS owner_display,
                count(*) AS property_count,
                sum(current_value) AS total_current_value
            FROM unified_parcels
            WHERE owner IS NOT NULL AND owner <> ''
            GROUP BY owner
            ORDER BY property_count DESC, total_current_value DESC
            LIMIT 1
        )SQL",
        {},
        {});
    if (!top_owner.ok) {
        std::cerr << top_owner.message << '\n';
        return 1;
    }
    if (top_owner.rows.empty()) {
        std::cerr << "No owners found in unified_parcels.\n";
        return 1;
    }

    const std::string owner_key = top_owner.rows[0][0];
    const std::string owner_display = top_owner.rows[0][1];
    const size_t property_count = (size_t)std::stoull(top_owner.rows[0][2]);

    const DuckDbQueryResult unified_count = duckdb.executeMapQuery(
        "SELECT count(*) AS parcel_count FROM unified_parcels",
        {},
        {});
    if (!unified_count.ok || unified_count.rows.empty()) {
        std::cerr << "Failed to validate unified_parcels row count: "
                  << (unified_count.ok ? "no rows returned" : unified_count.message) << '\n';
        return 1;
    }
    const size_t unified_row_count = (size_t)std::stoull(unified_count.rows[0][0]);
    if (unified_row_count != artifacts.unified_parcels.size()) {
        std::cerr << "Unified parcel row count mismatch: DuckDB has " << unified_row_count
                  << ", in-memory consolidation has " << artifacts.unified_parcels.size() << '\n';
        return 1;
    }

    std::cerr << "Printing owner properties...\n";
    const DuckDbQueryResult owner_properties = duckdb.executeMapQuery(
        "SELECT * FROM unified_parcels WHERE owner = '" + owner_key + "' "
        "ORDER BY current_value DESC, address ASC, blocklot ASC",
        {},
        {},
        std::max<size_t>(property_count, 1));
    if (!owner_properties.ok) {
        std::cerr << owner_properties.message << '\n';
        return 1;
    }
    if (owner_properties.rows.size() != property_count) {
        std::cerr << "Owner property count mismatch: summary query returned " << property_count
                  << ", detail query returned " << owner_properties.rows.size() << '\n';
        return 1;
    }

    std::cout << "Root: " << root << '\n';
    std::cout << "Hydration workers: " << worker_count << '\n';
    std::cout << "Local layers hydrated: " << hydration_summary.hydrated_layer_count
              << "/" << hydration_summary.requested_layer_count << '\n';
    std::cout << "Missing local layers skipped: " << hydration_summary.skipped_missing_layer_count << '\n';
    std::cout << "Hydrated features: " << hydration_summary.total_feature_count << '\n';
    std::cout << "Hydration time (ms): " << hydration_wall_ms << '\n';
    std::cout << "DuckDB rebuild time (ms): " << rebuild_ms << '\n';
    std::cout << duckdb.status().message << '\n';
    std::cout << "Unified parcels: " << unified_row_count << '\n';
    std::cout << "Top owner: " << owner_display << " [" << owner_key << "]\n";
    std::cout << "Property count: " << property_count << '\n';
    if (!zero_feature_diagnostics.empty()) {
        std::cout << "Zero-feature local layers: " << zero_feature_diagnostics.size() << '\n';
        for (const auto& item : zero_feature_diagnostics) {
            std::cout << "  " << item.first << ": " << item.second << '\n';
        }
    }
    std::cout << '\n';

    if (!options.output_path.empty()) {
        std::error_code ec;
        if (options.output_path.has_parent_path()) fs::create_directories(options.output_path.parent_path(), ec);
        std::ofstream out(options.output_path);
        if (!out) {
            std::cerr << "Failed to open output path: " << options.output_path << '\n';
            return 1;
        }
        writeQueryResultTsv(out, owner_properties);
        std::cout << "Owner properties written to: " << options.output_path << '\n';
    }
    if (!options.summary_only) printQueryResult(owner_properties);
    return 0;
}
