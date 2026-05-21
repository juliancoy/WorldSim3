#include "duckdb_analytics.h"

#include "app_utils.h"
#include "cache_io.h"
#include "feature_props.h"

#include <duckdb.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
constexpr int kAnalyticsSchemaVersion = 5;

std::string sqlQuote(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char ch : s) {
        if (ch == '\'') out += "''";
        else out.push_back(ch);
    }
    return out;
}

std::string lowerName(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    return s;
}

std::vector<uint8_t> readBinaryFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("failed to open " + path.string());
    in.seekg(0, std::ios::end);
    const auto n = in.tellg();
    in.seekg(0);
    std::vector<uint8_t> out((size_t)n);
    if (!out.empty()) in.read((char*)out.data(), (std::streamsize)out.size());
    return out;
}

std::vector<std::vector<std::string>> parseCsvRows(const std::vector<uint8_t>& bytes) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string field;
    bool quoted = false;
    for (size_t i = 0; i < bytes.size(); ++i) {
        const char ch = (char)bytes[i];
        if (quoted) {
            if (ch == '"') {
                if (i + 1 < bytes.size() && (char)bytes[i + 1] == '"') {
                    field.push_back('"');
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                field.push_back(ch);
            }
            continue;
        }
        if (ch == '"') {
            quoted = true;
        } else if (ch == ',') {
            row.push_back(std::move(field));
            field.clear();
        } else if (ch == '\n') {
            row.push_back(std::move(field));
            field.clear();
            if (!row.empty() && !row.back().empty() && row.back().back() == '\r') row.back().pop_back();
            rows.push_back(std::move(row));
            row.clear();
        } else {
            field.push_back(ch);
        }
    }
    if (!field.empty() || !row.empty()) {
        row.push_back(std::move(field));
        if (!row.empty() && !row.back().empty() && row.back().back() == '\r') row.back().pop_back();
        rows.push_back(std::move(row));
    }
    return rows;
}

double numericProp(const LayerDef::FeatureRecord& fg, std::initializer_list<const char*> keys) {
    return parseNumericField(firstDisplayProperty(fg, keys));
}

double structureAreaSqFtProp(const LayerDef::FeatureRecord& fg) {
    return numericProp(fg, {"structure_area_sqft", "STRUCTAREA", "structarea", "BLDG_AREA", "GROSS_AREA", "LIVING_AREA"});
}

bool isPrimaryParcelGeometryFile(const std::string& file) {
    return file == "parcel.geojson" ||
           (file.size() > std::strlen("_county_parcels.geojson") &&
            file.ends_with("_county_parcels.geojson"));
}

bool ensureAnalyticsLayerFeaturesLoaded(
    const fs::path& root,
    const LayerDef& source_layer,
    LayerDef& analytics_layer) {
    analytics_layer = source_layer;
    if (!analytics_layer.features.empty()) return true;
    if (!layerRuntimeSourceMaterializedForFile(root, analytics_layer.file)) return false;
    const fs::path layer_path = resolveStoredLayerPath(root, analytics_layer);
    std::string sig;
    if (!resolveLayerSourceSignature(layer_path, sig, nullptr)) return false;
    return loadCanonicalLayerFeatureCollection(
        root,
        analytics_layer.file,
        sig,
        analytics_layer.features,
        &analytics_layer.feature_properties);
}

fs::path manifestItemOutputPath(const fs::path& root, const json& item) {
    if (item.contains("directory") && item["directory"].is_string() &&
        item.contains("file") && item["file"].is_string()) {
        fs::path dir(item["directory"].get<std::string>());
        if (!dir.is_absolute()) dir = root / dir;
        return dir / item["file"].get<std::string>();
    }
    if (item.contains("provenance") && item["provenance"].is_object() &&
        item.contains("file") && item["file"].is_string()) {
        LayerDef layer;
        layer.file = item["file"].get<std::string>();
        const auto& provenance = item["provenance"];
        layer.provenance_world = provenance.value("world", std::string());
        layer.provenance_nation_state = provenance.value("nation_state", std::string());
        layer.provenance_state_region = provenance.value("state_region", std::string());
        layer.provenance_county_city = provenance.value("county_city", std::string());
        return provenanceStoredLayerPath(root, layer);
    }
    return {};
}

std::vector<fs::path> repositoryManifestPaths(const fs::path& root) {
    std::vector<fs::path> out;
    std::error_code ec;
    const fs::path manifest_root = root / "sources" / "world";
    if (!fs::exists(manifest_root, ec) || ec) return out;
    for (fs::recursive_directory_iterator it(manifest_root, ec), end; it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file()) continue;
        if (it->path().filename() == "layers_manifest.repository.json") out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string isoNowUtc() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &now_time);
#else
    gmtime_r(&now_time, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

std::vector<fs::path> analyticsSourceArtifactPaths(const fs::path& root, const LayerDef& layer) {
    std::vector<fs::path> out;
    auto append = [&](const fs::path& path) {
        if (std::find(out.begin(), out.end(), path) == out.end()) out.push_back(path);
    };
    if (!layer.source_url.empty() && layer.file.ends_with(".geojson")) {
        append(provenanceSourceArtifactPath(root, layer, layer.file));
        return out;
    }
    if (layer.import_type == "arcgis_feature_layer") {
        append(provenanceSourceArtifactPath(root, layer, layer.file));
        return out;
    }
    if (layer.import_type == "census_acs_tract_demographics") {
        append(provenanceSourceArtifactPath(root, layer, layer.file));
        if (!layer.import_artifact_file.empty()) {
            append(provenanceSourceArtifactPath(root, layer, layer.import_artifact_file));
        } else {
            append(provenanceSourceArtifactPath(root, layer, fs::path(layer.file).stem().string() + ".acs.json"));
            append(provenanceSourceArtifactPath(root, layer, layer.file + ".acs.json"));
        }
        return out;
    }
    if (!layer.import_artifact_file.empty()) {
        append(provenanceSourceArtifactPath(root, layer, layer.import_artifact_file));
        return out;
    }
    if (layer.import_type == "socrata_csv_properties") {
        append(provenanceSourceArtifactPath(root, layer, layer.file + ".source.csv"));
    } else if (layer.import_type == "xlsx_point_table") {
        append(provenanceSourceArtifactPath(root, layer, fs::path(layer.file).stem().string() + ".xlsx"));
        append(provenanceSourceArtifactPath(root, layer, layer.file + ".xlsx"));
        append(provenanceSourceArtifactPath(root, layer, layer.file + ".source.xlsx"));
    } else if (layer.import_type == "json_point_feed" || layer.import_type == "overpass_json_point_feed") {
        append(provenanceSourceArtifactPath(root, layer, fs::path(layer.file).stem().string() + ".json"));
        append(provenanceSourceArtifactPath(root, layer, layer.file + ".json"));
        append(provenanceSourceArtifactPath(root, layer, layer.file + ".source.json"));
    } else if (layer.import_type == "zipped_shapefile") {
        append(provenanceSourceArtifactPath(root, layer, layer.file + ".source.zip"));
        append(provenanceSourceArtifactPath(root, layer, fs::path(layer.file).stem().string() + ".zip"));
    }
    return out;
}

std::string analyticsBuildSignature(const fs::path& root, const std::vector<LayerDef>& layers) {
    std::ostringstream sig;
    sig << "analytics_schema_v" << kAnalyticsSchemaVersion << "|";
    for (const auto& layer : layers) {
        sig << layer.file << ":runtime=" << (layer.runtime_load ? 1 : 0)
            << ":duckdb=" << (layer.duckdb_ingest ? 1 : 0)
            << ":role=" << layer.duckdb_role << "|";
        if (!layer.duckdb_ingest) continue;
        const std::vector<fs::path> sig_paths = analyticsSourceArtifactPaths(root, layer);
        for (const auto& sig_path : sig_paths) {
            if (!fs::exists(sig_path)) continue;
            sig << "sig=" << fileSignature(sig_path) << "|";
        }
    }
    return sig.str();
}

void appendSocrataHowardPropertyRows(
    duckdb::Appender& appender,
    size_t layer_idx,
    const LayerDef& layer,
    const fs::path& csv_path,
    size_t& feature_count) {
    const auto rows = parseCsvRows(readBinaryFile(csv_path));
    if (rows.empty()) return;
    std::unordered_map<std::string, size_t> col;
    for (size_t i = 0; i < rows.front().size(); ++i) col[rows.front()[i]] = i;
    auto get = [&](const std::vector<std::string>& row, const char* name) -> std::string {
        auto it = col.find(name);
        if (it == col.end() || it->second >= row.size()) return {};
        return row[it->second];
    };
    const std::string category = categoryToString(layer.category);
    const std::string duckdb_role = layer.duckdb_role.empty() ? "layer_feature" : layer.duckdb_role;
    size_t local_feature_idx = 0;
    for (size_t r = 1; r < rows.size(); ++r) {
        const auto& row = rows[r];
        const std::string acct = get(row, "account_id_mdp_field_acctid");
        if (acct.empty()) continue;
        const std::string address = get(row, "mdp_street_address_mdp_field_address");
        const double land_value = parseNumericField(get(row, "current_cycle_data_land_value_mdp_field_names_nfmlndvl_curlndvl_and_sallndvl_sdat_field_164"));
        const double improvement_value = parseNumericField(get(row, "current_cycle_data_improvements_value_mdp_field_names_nfmimpvl_curimpvl_and_salimpvl_sdat_field_165"));
        const double current_value = parseNumericField(get(row, "current_assessment_year_total_assessment_sdat_field_172"));
        const double structure_area_sqft = 0.0;
        const std::string feature_id = layer.file + ":" + acct;

        appender.BeginRow();
        appender.Append<uint64_t>((uint64_t)layer_idx);
        appender.Append<const char*>(layer.name.c_str());
        appender.Append<const char*>(layer.file.c_str());
        appender.Append<const char*>(duckdb_role.c_str());
        appender.Append<uint64_t>((uint64_t)local_feature_idx);
        appender.Append<const char*>(feature_id.c_str());
        appender.Append<const char*>(layer.scale.c_str());
        appender.Append<const char*>(category.c_str());
        appender.Append<const char*>(layer.provenance_world.c_str());
        appender.Append<const char*>(layer.provenance_nation_state.c_str());
        appender.Append<const char*>(layer.provenance_state_region.c_str());
        appender.Append<const char*>(layer.provenance_county_city.c_str());
        appender.Append<double>(0.0);
        appender.Append<double>(0.0);
        appender.Append<double>(0.0);
        appender.Append<double>(0.0);
        appender.Append<const char*>(acct.c_str());
        appender.Append<const char*>("");
        appender.Append<const char*>(address.c_str());
        appender.Append<const char*>(get(row, "mdp_street_address_zip_code_mdp_field_zipcode").c_str());
        appender.Append<const char*>("");
        appender.Append<const char*>("");
        appender.Append<const char*>("Howard County");
        appender.Append<double>(current_value > 0.0 ? current_value : (land_value + improvement_value));
        appender.Append<double>(structure_area_sqft);
        appender.Append<const char*>("");
        appender.Append<const char*>("");
        appender.Append<const char*>("");
        appender.Append<const char*>("Maryland Real Property Assessments");
        appender.Append<const char*>(get(row, "sales_segment_1_transfer_date_yyyy_mm_dd_mdp_field_tradate_sdat_field_89").c_str());
        appender.Append<const char*>("");
        appender.Append<int32_t>((int32_t)parseNumericField(get(row, "c_a_m_a_system_data_year_built_yyyy_mdp_field_yearblt_sdat_field_235")));
        appender.Append<double>(parseNumericField(get(row, "sales_segment_1_consideration_mdp_field_considr1_sdat_field_90")));
        appender.EndRow();
        ++local_feature_idx;
        ++feature_count;
    }
}
}

DuckDbAnalytics::DuckDbAnalytics(std::filesystem::path root)
    : root_(std::move(root)) {
    status_.db_path = (root_ / "data" / "worldsim.duckdb").string();
    status_.available = true;
    status_.message = "DuckDB analytics cache not built yet.";
}

bool DuckDbAnalytics::needsRebuild(const std::vector<LayerDef>& layers) const {
    try {
        std::error_code ec;
        const fs::path db_path = status_.db_path;
        if (!fs::exists(db_path, ec) || ec) return true;

        duckdb::DuckDB db(status_.db_path);
        duckdb::Connection con(db);
        auto signature_res = con.Query(R"SQL(
            SELECT source_signature
            FROM analytics_build_info
            ORDER BY built_at_utc DESC
            LIMIT 1
        )SQL");
        if (!signature_res || signature_res->HasError() || signature_res->RowCount() == 0) {
            return true;
        }
        const std::string stored_signature = signature_res->GetValue(0, 0).ToString();
        return stored_signature != analyticsBuildSignature(root_, layers);
    } catch (...) {
        return true;
    }
}

bool DuckDbAnalytics::validateExistingCache() {
    try {
        std::error_code ec;
        const fs::path db_path = status_.db_path;
        if (!fs::exists(db_path, ec) || ec) {
            status_.last_rebuild_ok = false;
            status_.message = "DuckDB analytics cache not built yet.";
            return false;
        }

        duckdb::DuckDB db(status_.db_path);
        duckdb::Connection con(db);

        auto table_check = con.Query(R"SQL(
            SELECT count(*)::BIGINT
            FROM information_schema.tables
            WHERE table_schema = 'main'
              AND table_name IN ('layer_features', 'layer_feature_properties', 'unified_parcels', 'parcel_events')
        )SQL");
        if (!table_check || table_check->HasError() || table_check->RowCount() == 0) {
            status_.last_rebuild_ok = false;
            status_.message = "DuckDB analytics cache validation failed.";
            return false;
        }
        const int64_t table_count = table_check->GetValue<int64_t>(0, 0);
        if (table_count < 4) {
            status_.last_rebuild_ok = false;
            status_.message = "DuckDB analytics cache is incomplete.";
            return false;
        }

        auto feature_count_res = con.Query("SELECT count(*)::BIGINT FROM layer_features");
        auto layer_count_res = con.Query("SELECT count(DISTINCT layer_idx)::BIGINT FROM layer_features");
        if (!feature_count_res || feature_count_res->HasError() ||
            !layer_count_res || layer_count_res->HasError() ||
            feature_count_res->RowCount() == 0 || layer_count_res->RowCount() == 0) {
            status_.last_rebuild_ok = false;
            status_.message = "DuckDB analytics cache validation failed.";
            return false;
        }

        status_.last_rebuild_ok = true;
        status_.feature_count = (size_t)feature_count_res->GetValue<int64_t>(0, 0);
        status_.layer_count = (size_t)layer_count_res->GetValue<int64_t>(0, 0);
        std::ostringstream msg;
        msg << "DuckDB analytics cache ready: " << status_.feature_count
            << " features across " << status_.layer_count << " hydrated layers.";
        status_.message = msg.str();
        return true;
    } catch (const std::exception& e) {
        status_.last_rebuild_ok = false;
        status_.message = std::string("DuckDB cache validation failed: ") + e.what();
        return false;
    }
}

DuckDbArtifactEnsureResult DuckDbAnalytics::ensureCurrentArtifact(
    const std::vector<LayerDef>& layers,
    const std::vector<UnifiedParcelRecord>& unified_parcels) {
    DuckDbArtifactEnsureResult result;
    const bool stale_or_missing = needsRebuild(layers);
    result.invalidated = stale_or_missing;

    if (!stale_or_missing && validateExistingCache()) {
        result.ok = true;
        result.reused_existing = true;
        result.message = status_.message;
        return result;
    }

    result.invalidated = true;
    result.rebuilt = true;
    result.ok = rebuild(layers, unified_parcels);
    result.message = status_.message;
    return result;
}

bool DuckDbAnalytics::rebuild(const std::vector<LayerDef>& layers, const std::vector<UnifiedParcelRecord>& unified_parcels) {
    try {
        struct ParcelAnalyticsRow {
            size_t parcel_layer_idx = 0;
            size_t parcel_feature_idx = 0;
            std::string parcel_layer_file;
            std::string blocklot;
            std::string parcel_source_file;
            std::string owner;
            std::string owner_display;
            std::string address;
            std::string zipcode;
            std::string status;
            double current_land = 0.0;
            double current_improvements = 0.0;
            double structure_area_sqft = 0.0;
            double current_value = 0.0;
            double sale_price = 0.0;
            double min_lon = 0.0;
            double min_lat = 0.0;
            double max_lon = 0.0;
            double max_lat = 0.0;
        };
        struct PropertyRecordRow {
            size_t real_property_feature_idx = 0;
            std::string layer_file;
            std::string property_source_file;
            std::string owner;
            std::string owner_display;
            std::string address;
            std::string zipcode;
            std::string status;
            double current_land = 0.0;
            double current_improvements = 0.0;
            double structure_area_sqft = 0.0;
            double current_value = 0.0;
            double sale_price = 0.0;
            int priority = 10;
        };
        std::filesystem::create_directories(root_ / "data");
        duckdb::DuckDB db(status_.db_path);
        duckdb::Connection con(db);
        auto exec_or_throw = [&](const std::string& sql, const char* context) {
            auto res = con.Query(sql);
            if (!res || res->HasError()) {
                throw std::runtime_error(
                    std::string(context) + ": " + (res ? res->GetError() : std::string("query failed")));
            }
        };

        exec_or_throw("BEGIN TRANSACTION", "begin transaction");
        exec_or_throw("DROP TABLE IF EXISTS layer_features", "drop layer_features");
        exec_or_throw("DROP TABLE IF EXISTS layer_feature_properties", "drop layer_feature_properties");
        exec_or_throw("DROP TABLE IF EXISTS unified_parcels", "drop unified_parcels");
        exec_or_throw("DROP TABLE IF EXISTS parcel_events", "drop parcel_events");
        exec_or_throw("DROP TABLE IF EXISTS repository_sources", "drop repository_sources");
        exec_or_throw("DROP TABLE IF EXISTS geography_feature_collections", "drop geography_feature_collections");
        exec_or_throw("DROP TABLE IF EXISTS anambra_repository_sources", "drop legacy anambra_repository_sources");
        exec_or_throw("DROP TABLE IF EXISTS anambra_runtime_features", "drop legacy anambra_runtime_features");
        exec_or_throw("DROP TABLE IF EXISTS anambra_runtime_lga_summary", "drop legacy anambra_runtime_lga_summary");
        exec_or_throw("DROP TABLE IF EXISTS import_audit", "drop import_audit");
        exec_or_throw("DROP TABLE IF EXISTS analytics_build_info", "drop analytics_build_info");
        exec_or_throw("DROP TABLE IF EXISTS analytics_source_contributions", "drop analytics_source_contributions");
        exec_or_throw(R"SQL(
            CREATE TABLE layer_features (
                layer_idx UBIGINT,
                layer_name VARCHAR,
                layer_file VARCHAR,
                duckdb_role VARCHAR,
                feature_idx UBIGINT,
                feature_id VARCHAR,
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
        )SQL", "create layer_features");

        // Compatibility placeholder only. Default DuckDB builds must not copy
        // full feature property bags; selected semantic columns belong in
        // layer_features or typed derived tables.
        exec_or_throw(R"SQL(
            CREATE TABLE layer_feature_properties (
                layer_idx UBIGINT,
                layer_name VARCHAR,
                layer_file VARCHAR,
                duckdb_role VARCHAR,
                feature_idx UBIGINT,
                feature_id VARCHAR,
                property_key VARCHAR,
                property_value VARCHAR
            )
        )SQL", "create layer_feature_properties");

        auto appender = duckdb::Appender(con, "layer_features");
        std::vector<ParcelAnalyticsRow> parcel_rows;
        std::unordered_map<std::string, PropertyRecordRow> property_by_blocklot;
        size_t feature_count = 0;
        size_t layer_count = 0;
        for (size_t li = 0; li < layers.size(); ++li) {
            const auto& source_layer = layers[li];
            if (!source_layer.duckdb_ingest) continue;
            if (source_layer.import_type == "socrata_csv_properties") {
                const fs::path csv_path = provenanceSourceArtifactPath(root_, source_layer, source_layer.file + ".source.csv");
                if (!fs::exists(csv_path)) continue;
                layer_count++;
                appendSocrataHowardPropertyRows(appender, li, source_layer, csv_path, feature_count);
                continue;
            }
            LayerDef layer;
            if (!ensureAnalyticsLayerFeaturesLoaded(root_, source_layer, layer)) continue;
            if (layer.features.empty()) continue;
            rebuildFeaturePropertyRegistryForLayer(layer);
            layer_count++;
            const std::string category = categoryToString(layer.category);
            const std::string duckdb_role = layer.duckdb_role.empty() ? "layer_feature" : layer.duckdb_role;
            for (size_t fi = 0; fi < layer.features.size(); ++fi) {
                const auto& fg = layer.features[fi];
                const std::string feature_id = featureStableIdForLayerFeature(layer, fg, fi);
                const std::string blocklot = featureBlockLotJoinKey(fg);
                std::string owner = toLowerAscii(trimDisplayValue(firstDisplayProperty(fg, {"OWNER_1", "OWNERNME1", "OWNER", "OWNER_NAME", "AR_OWNER", "OWNER_ABBR"})));
                const std::string address = firstDisplayProperty(fg, {
                    "FULLADDR", "FULL_ADDRESS", "PROPERTY_ADDRESS", "PROPERTYADDR", "PREMISEADD",
                    "PREMISE_ADDRESS", "ADDRESS", "Address", "ADDR", "ADDR1", "ADDRESS1",
                    "SITE_ADDR", "SITUSADDR", "LOCATION", "Location"
                });
                const std::string zipcode = firstDisplayProperty(fg, {"ZIP", "ZIPCODE", "POSTAL_CODE"});
                const std::string status = firstDisplayProperty(fg, {"STATUS", "STATE", "CASE_STATUS"});
                const std::string zoning = zoningClassKey(fg);
                const std::string jurisdiction = firstDisplayProperty(fg, {"jurisdiction", "JURISDICTION", "COUNTY", "COUNTYNAME", "County"});
                double value = numericProp(fg, {"value_usd", "property_value_usd", "TAXBASE", "ARTAXBAS", "SALEPRIC"});
                const double structure_area_sqft = structureAreaSqFtProp(fg);
                const std::string feature_name = firstDisplayProperty(
                    fg, {"name", "poi_name", "prmry_name", "set_name", "market_nam", "plc_st_nam", "fctry_st_n"});
                const std::string lga_name = firstDisplayProperty(fg, {"lganame"});
                const std::string ward_name = firstDisplayProperty(fg, {"wardname"});
                const std::string source_name = firstDisplayProperty(fg, {"source"});
                const std::string source_file = [&]() {
                    std::string source = trimDisplayValue(firstDisplayProperty(fg, {"source_file", "SOURCE_FILE", "source", "SOURCE"}));
                    return source.empty() ? layer.file : source;
                }();
                const std::string event_date_text = firstDisplayProperty(fg, {
                    "event_date", "DateNotice", "DateIssue", "DateIssued", "Issue_Date_ISO",
                    "Issue_Date", "SALEDATE", "DATE", "CREATED_DATE", "RECORD_DATE"
                });
                const std::string event_status_hint = firstDisplayProperty(fg, {"CASE_STATUS", "STATUS", "STATE"});
                int event_year_hint = 0;
                if (const std::string year_text = firstDisplayProperty(fg, {"Issue_Year", "YEAR"}); !trimDisplayValue(year_text).empty()) {
                    event_year_hint = (int)parseNumericField(year_text);
                }
                double amount_usd_hint = numericProp(
                    fg, {"amount_usd", "AMOUNT", "Amount", "COST", "TOTALCOST", "SALEPRIC", "TAXBASE", "ARTAXBAS"});
                const double land_value = numericProp(fg, {"land_value", "CURRLAND"});
                const double improvement_value = numericProp(fg, {"improvement_value", "CURRIMPR"});
                const double parcel_current_value = numericProp(fg, {"current_value", "tax_base", "TAXBASE", "ARTAXBAS"});
                const double sale_price = numericProp(fg, {"sale_price", "SALEPRIC"});
                const std::string owner_display = trimDisplayValue(firstDisplayProperty(
                    fg, {"owner", "owner_name", "OWNER_1", "OWNERNME1", "OWNER", "OWNER_NAME", "AR_OWNER", "OWNER_ABBR"}));

                appender.BeginRow();
                appender.Append<uint64_t>((uint64_t)li);
                appender.Append<const char*>(layer.name.c_str());
                appender.Append<const char*>(layer.file.c_str());
                appender.Append<const char*>(duckdb_role.c_str());
                appender.Append<uint64_t>((uint64_t)fi);
                appender.Append<const char*>(feature_id.c_str());
                appender.Append<const char*>(layer.scale.c_str());
                appender.Append<const char*>(category.c_str());
                appender.Append<const char*>(layer.provenance_world.c_str());
                appender.Append<const char*>(layer.provenance_nation_state.c_str());
                appender.Append<const char*>(layer.provenance_state_region.c_str());
                appender.Append<const char*>(layer.provenance_county_city.c_str());
                appender.Append<double>(fg.extent.min_lon);
                appender.Append<double>(fg.extent.min_lat);
                appender.Append<double>(fg.extent.max_lon);
                appender.Append<double>(fg.extent.max_lat);
                appender.Append<const char*>(blocklot.c_str());
                appender.Append<const char*>(owner.c_str());
                appender.Append<const char*>(address.c_str());
                appender.Append<const char*>(zipcode.c_str());
                appender.Append<const char*>(status.c_str());
                appender.Append<const char*>(zoning.c_str());
                appender.Append<const char*>(jurisdiction.c_str());
                appender.Append<double>(value);
                appender.Append<double>(structure_area_sqft);
                appender.Append<const char*>(feature_name.c_str());
                appender.Append<const char*>(lga_name.c_str());
                appender.Append<const char*>(ward_name.c_str());
                appender.Append<const char*>(source_name.c_str());
                appender.Append<const char*>(event_date_text.c_str());
                appender.Append<const char*>(event_status_hint.c_str());
                appender.Append<int32_t>(event_year_hint);
                appender.Append<double>(amount_usd_hint);
                appender.EndRow();

                if (isPrimaryParcelGeometryFile(layer.file)) {
                    ParcelAnalyticsRow row;
                    row.parcel_layer_idx = li;
                    row.parcel_feature_idx = fi;
                    row.parcel_layer_file = layer.file;
                    row.blocklot = blocklot;
                    row.parcel_source_file = source_file;
                    row.owner = owner;
                    row.owner_display = owner_display;
                    row.address = address;
                    row.zipcode = zipcode;
                    row.status = status;
                    row.current_land = land_value;
                    row.current_improvements = improvement_value;
                    row.structure_area_sqft = structure_area_sqft;
                    row.current_value = parcel_current_value > 0.0 ? parcel_current_value : value;
                    row.sale_price = sale_price;
                    row.min_lon = fg.extent.min_lon;
                    row.min_lat = fg.extent.min_lat;
                    row.max_lon = fg.extent.max_lon;
                    row.max_lat = fg.extent.max_lat;
                    parcel_rows.push_back(std::move(row));
                }
                if (duckdb_role == "parcel_record" &&
                    !isPrimaryParcelGeometryFile(layer.file) &&
                    !blocklot.empty()) {
                    PropertyRecordRow row;
                    row.real_property_feature_idx = fi;
                    row.layer_file = layer.file;
                    row.property_source_file = source_file;
                    row.owner = owner;
                    row.owner_display = owner_display;
                    row.address = address;
                    row.zipcode = zipcode;
                    row.status = status;
                    row.current_land = land_value;
                    row.current_improvements = improvement_value;
                    row.structure_area_sqft = structure_area_sqft;
                    row.current_value = parcel_current_value > 0.0 ? parcel_current_value : value;
                    row.sale_price = sale_price;
                    if (layer.file == "regional_real_property.geojson") row.priority = 0;
                    else if (layer.file == "real_property_information.geojson") row.priority = 1;
                    else if (layer.file == "howard_county_real_property_assessments.geojson") row.priority = 2;
                    auto it = property_by_blocklot.find(blocklot);
                    if (it == property_by_blocklot.end() ||
                        row.priority < it->second.priority ||
                        (row.priority == it->second.priority && row.real_property_feature_idx < it->second.real_property_feature_idx)) {
                        property_by_blocklot[blocklot] = std::move(row);
                    }
                }

                feature_count++;
            }
        }
        appender.Close();

        exec_or_throw(R"SQL(
            CREATE TABLE unified_parcels (
                parcel_layer_idx UBIGINT,
                parcel_feature_idx UBIGINT,
                real_property_feature_idx UBIGINT,
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
        )SQL", "create unified_parcels");
        {
            auto parcel_appender = duckdb::Appender(con, "unified_parcels");
            for (const auto& parcel : parcel_rows) {
                const PropertyRecordRow* property = nullptr;
                if (!parcel.blocklot.empty()) {
                    auto it = property_by_blocklot.find(parcel.blocklot);
                    if (it != property_by_blocklot.end() &&
                        (it->second.layer_file != parcel.parcel_layer_file ||
                         it->second.real_property_feature_idx != parcel.parcel_feature_idx)) {
                        property = &it->second;
                    }
                }
                const std::string owner = property && !property->owner.empty() ? property->owner : parcel.owner;
                const std::string owner_display = property && !property->owner_display.empty() ? property->owner_display : parcel.owner_display;
                const std::string address = property && !property->address.empty() ? property->address : parcel.address;
                const std::string zipcode = property && !property->zipcode.empty() ? property->zipcode : parcel.zipcode;
                const std::string status = property && !property->status.empty() ? property->status : parcel.status;
                const double current_land = property && property->current_land > 0.0 ? property->current_land : parcel.current_land;
                const double current_improvements = property && property->current_improvements > 0.0 ? property->current_improvements : parcel.current_improvements;
                const double structure_area_sqft = property && property->structure_area_sqft > 0.0 ? property->structure_area_sqft : parcel.structure_area_sqft;
                const double tax_base = property && property->current_value > 0.0 ? property->current_value : parcel.current_value;
                const double sale_price = property && property->sale_price > 0.0 ? property->sale_price : parcel.sale_price;
                double current_value = tax_base;
                if (current_value <= 0.0 && current_land + current_improvements > 0.0) current_value = current_land + current_improvements;
                if (current_value <= 0.0) current_value = sale_price;

                parcel_appender.BeginRow();
                parcel_appender.Append<uint64_t>((uint64_t)parcel.parcel_layer_idx);
                parcel_appender.Append<uint64_t>((uint64_t)parcel.parcel_feature_idx);
                parcel_appender.Append<uint64_t>(property ? (uint64_t)property->real_property_feature_idx : UINT64_MAX);
                parcel_appender.Append<const char*>(parcel.blocklot.c_str());
                parcel_appender.Append<const char*>(parcel.parcel_source_file.c_str());
                parcel_appender.Append<const char*>(property ? property->property_source_file.c_str() : "");
                parcel_appender.Append<bool>(true);
                parcel_appender.Append<bool>(property != nullptr);
                parcel_appender.Append<const char*>(owner.c_str());
                parcel_appender.Append<const char*>(owner_display.c_str());
                parcel_appender.Append<const char*>(address.c_str());
                parcel_appender.Append<const char*>(normalizeAddressSearchText(address).c_str());
                parcel_appender.Append<const char*>(zipcode.c_str());
                parcel_appender.Append<const char*>(status.c_str());
                parcel_appender.Append<double>(current_land);
                parcel_appender.Append<double>(current_improvements);
                parcel_appender.Append<double>(structure_area_sqft);
                parcel_appender.Append<double>(tax_base);
                parcel_appender.Append<double>(sale_price);
                parcel_appender.Append<double>(current_value);
                parcel_appender.Append<int32_t>(0);
                parcel_appender.Append<int32_t>(0);
                parcel_appender.Append<int32_t>(0);
                parcel_appender.Append<int32_t>(0);
                parcel_appender.Append<double>(0.0);
                parcel_appender.Append<double>(0.0);
                parcel_appender.Append<double>(parcel.min_lon);
                parcel_appender.Append<double>(parcel.min_lat);
                parcel_appender.Append<double>(parcel.max_lon);
                parcel_appender.Append<double>(parcel.max_lat);
                parcel_appender.EndRow();
            }
            parcel_appender.Close();
        }

        exec_or_throw("CREATE INDEX IF NOT EXISTS idx_layer_features_blocklot ON layer_features(blocklot)", "index layer_features blocklot");
        exec_or_throw("CREATE INDEX IF NOT EXISTS idx_layer_features_feature_id ON layer_features(layer_file, feature_id)", "index layer_features feature_id");
        exec_or_throw("CREATE INDEX IF NOT EXISTS idx_layer_features_owner ON layer_features(owner)", "index layer_features owner");
        exec_or_throw("CREATE INDEX IF NOT EXISTS idx_layer_features_layer_file ON layer_features(layer_file)", "index layer_features layer_file");
        exec_or_throw("CREATE INDEX IF NOT EXISTS idx_layer_features_geography ON layer_features(provenance_nation_state, provenance_state_region)", "index layer_features geography");
        exec_or_throw("CREATE INDEX IF NOT EXISTS idx_unified_parcels_blocklot ON unified_parcels(blocklot)", "index unified_parcels blocklot");
        exec_or_throw("CREATE INDEX IF NOT EXISTS idx_unified_parcels_owner ON unified_parcels(owner)", "index unified_parcels owner");
        exec_or_throw("CREATE INDEX IF NOT EXISTS idx_unified_parcels_address_search ON unified_parcels(address_search)", "index unified_parcels address_search");
        exec_or_throw(R"SQL(
            CREATE TABLE analytics_build_info (
                built_at_utc VARCHAR,
                source_signature VARCHAR,
                hydrated_layer_count UBIGINT,
                hydrated_feature_count UBIGINT,
                unified_parcel_count UBIGINT,
                unified_parcels_with_property_record UBIGINT,
                unified_parcels_with_geometry UBIGINT
            )
        )SQL", "create analytics_build_info");
        exec_or_throw(R"SQL(
            CREATE TABLE analytics_source_contributions (
                source_role VARCHAR,
                source_file VARCHAR,
                row_count UBIGINT
            )
        )SQL", "create analytics_source_contributions");
        exec_or_throw(R"SQL(
            CREATE TABLE repository_sources (
                manifest_path VARCHAR,
                name VARCHAR,
                file VARCHAR,
                source_url VARCHAR,
                source_name VARCHAR,
                description VARCHAR,
                local_path VARCHAR,
                local_exists BOOLEAN,
                file_size_bytes UBIGINT,
                downloadable BOOLEAN,
                reason VARCHAR,
                provenance_world VARCHAR,
                provenance_nation_state VARCHAR,
                provenance_state_region VARCHAR,
                provenance_county_city VARCHAR
            )
        )SQL", "create repository_sources");
        exec_or_throw(R"SQL(
            CREATE TABLE import_audit (
                layer_file VARCHAR,
                layer_name VARCHAR,
                import_type VARCHAR,
                duckdb_role VARCHAR,
                provenance_nation_state VARCHAR,
                provenance_state_region VARCHAR,
                source_artifact_path VARCHAR,
                source_artifact_exists BOOLEAN,
                source_artifact_size_bytes UBIGINT,
                stored_layer_path VARCHAR,
                stored_layer_exists BOOLEAN,
                feature_count UBIGINT,
                missing_name_count UBIGINT,
                missing_lga_count UBIGINT
            )
        )SQL", "create import_audit");
        {
            const std::string built_at_utc = isoNowUtc();
            const std::string source_signature = analyticsBuildSignature(root_, layers);
            std::unordered_map<std::string, size_t> parcel_source_counts;
            std::unordered_map<std::string, size_t> property_source_counts;
            size_t unified_parcel_count = 0;
            size_t parcels_with_property_record = 0;
            size_t parcels_with_geometry = 0;
            if (auto count_res = con.Query("SELECT count(*), coalesce(sum(CASE WHEN has_property_record THEN 1 ELSE 0 END), 0), coalesce(sum(CASE WHEN parcel_has_geometry THEN 1 ELSE 0 END), 0) FROM unified_parcels"); count_res && !count_res->HasError() && count_res->RowCount() > 0) {
                unified_parcel_count = (size_t)count_res->GetValue<int64_t>(0, 0);
                parcels_with_property_record = (size_t)count_res->GetValue<int64_t>(1, 0);
                parcels_with_geometry = (size_t)count_res->GetValue<int64_t>(2, 0);
            }
            if (auto parcel_source_res = con.Query("SELECT parcel_source_file, count(*) FROM unified_parcels WHERE parcel_source_file <> '' GROUP BY 1"); parcel_source_res && !parcel_source_res->HasError()) {
                for (idx_t i = 0; i < parcel_source_res->RowCount(); ++i) {
                    parcel_source_counts[parcel_source_res->GetValue(0, i).ToString()] = (size_t)parcel_source_res->GetValue<int64_t>(1, i);
                }
            }
            if (auto property_source_res = con.Query("SELECT property_source_file, count(*) FROM unified_parcels WHERE property_source_file <> '' GROUP BY 1"); property_source_res && !property_source_res->HasError()) {
                for (idx_t i = 0; i < property_source_res->RowCount(); ++i) {
                    property_source_counts[property_source_res->GetValue(0, i).ToString()] = (size_t)property_source_res->GetValue<int64_t>(1, i);
                }
            }

            auto info_appender = duckdb::Appender(con, "analytics_build_info");
            info_appender.BeginRow();
            info_appender.Append<const char*>(built_at_utc.c_str());
            info_appender.Append<const char*>(source_signature.c_str());
            info_appender.Append<uint64_t>((uint64_t)layer_count);
            info_appender.Append<uint64_t>((uint64_t)feature_count);
            info_appender.Append<uint64_t>((uint64_t)unified_parcel_count);
            info_appender.Append<uint64_t>((uint64_t)parcels_with_property_record);
            info_appender.Append<uint64_t>((uint64_t)parcels_with_geometry);
            info_appender.EndRow();
            info_appender.Close();

            auto source_appender = duckdb::Appender(con, "analytics_source_contributions");
            for (const auto& [source_file, row_count] : parcel_source_counts) {
                source_appender.BeginRow();
                source_appender.Append<const char*>("parcel_geometry");
                source_appender.Append<const char*>(source_file.c_str());
                source_appender.Append<uint64_t>((uint64_t)row_count);
                source_appender.EndRow();
            }
            for (const auto& [source_file, row_count] : property_source_counts) {
                source_appender.BeginRow();
                source_appender.Append<const char*>("property_record");
                source_appender.Append<const char*>(source_file.c_str());
                source_appender.Append<uint64_t>((uint64_t)row_count);
                source_appender.EndRow();
            }
            for (const auto& layer : layers) {
                if (layer.features.empty()) continue;
                source_appender.BeginRow();
                source_appender.Append<const char*>("hydrated_layer");
                source_appender.Append<const char*>(layer.file.c_str());
                source_appender.Append<uint64_t>((uint64_t)layer.features.size());
                source_appender.EndRow();
            }
            source_appender.Close();
        }
        {
            const std::vector<fs::path> manifest_paths = repositoryManifestPaths(root_);
            if (!manifest_paths.empty()) {
                auto repository_appender = duckdb::Appender(con, "repository_sources");
                for (const auto& manifest_path : manifest_paths) {
                    std::ifstream in(manifest_path);
                    json arr;
                    if (in) {
                        try {
                            in >> arr;
                        } catch (...) {
                            arr = json::array();
                        }
                    }
                    if (!arr.is_array()) continue;
                    for (const auto& item : arr) {
                        if (!item.is_object()) continue;
                        const fs::path local_path = manifestItemOutputPath(root_, item);
                        std::error_code ec;
                        const bool local_exists = !local_path.empty() && fs::exists(local_path, ec) && !ec;
                        const uint64_t file_size_bytes =
                            local_exists ? (uint64_t)fs::file_size(local_path, ec) : 0ULL;
                        const auto& provenance = item.contains("provenance") && item["provenance"].is_object()
                            ? item["provenance"] : json::object();
                        repository_appender.BeginRow();
                        repository_appender.Append<const char*>(manifest_path.string().c_str());
                        repository_appender.Append<const char*>(item.value("name", std::string()).c_str());
                        repository_appender.Append<const char*>(item.value("file", std::string()).c_str());
                        repository_appender.Append<const char*>(item.value("url", std::string()).c_str());
                        repository_appender.Append<const char*>(item.value("source", std::string()).c_str());
                        repository_appender.Append<const char*>(item.value("description", std::string()).c_str());
                        repository_appender.Append<const char*>(local_path.string().c_str());
                        repository_appender.Append<bool>(local_exists);
                        repository_appender.Append<uint64_t>(file_size_bytes);
                        repository_appender.Append<bool>(item.value("download", true));
                        repository_appender.Append<const char*>(item.value("reason", std::string()).c_str());
                        repository_appender.Append<const char*>(provenance.value("world", std::string()).c_str());
                        repository_appender.Append<const char*>(provenance.value("nation_state", std::string()).c_str());
                        repository_appender.Append<const char*>(provenance.value("state_region", std::string()).c_str());
                        repository_appender.Append<const char*>(provenance.value("county_city", std::string()).c_str());
                        repository_appender.EndRow();
                    }
                }
                repository_appender.Close();
            }
        }
        {
            auto import_audit_appender = duckdb::Appender(con, "import_audit");
            for (const auto& layer : layers) {
                if (layer.import_type.empty()) continue;
                const fs::path stored_path = resolveStoredLayerPath(root_, layer);
                std::error_code ec;
                const bool stored_exists = fs::exists(stored_path, ec) && !ec;
                fs::path source_artifact_path;
                if (!layer.import_artifact_file.empty()) {
                    source_artifact_path = provenanceSourceArtifactPath(root_, layer, layer.import_artifact_file);
                } else if (layer.import_type == "xlsx_point_table") {
                    source_artifact_path = provenanceSourceArtifactPath(root_, layer, layer.file + ".source.xlsx");
                } else if (layer.import_type == "zipped_shapefile") {
                    source_artifact_path = provenanceSourceArtifactPath(root_, layer, layer.file + ".source.zip");
                } else if (layer.import_type == "socrata_csv_properties") {
                    source_artifact_path = provenanceSourceArtifactPath(root_, layer, layer.file + ".source.csv");
                }
                ec.clear();
                const bool source_exists = !source_artifact_path.empty() && fs::exists(source_artifact_path, ec) && !ec;
                ec.clear();
                const uint64_t source_size_bytes = source_exists ? (uint64_t)fs::file_size(source_artifact_path, ec) : 0ULL;
                size_t missing_name_count = 0;
                size_t missing_lga_count = 0;
                for (const auto& fg : layer.features) {
                    const std::string feature_name = firstDisplayProperty(
                        fg,
                        {"name", "poi_name", "prmry_name", "set_name", "market_nam", "plc_st_nam", "fctry_st_n"});
                    if (trimDisplayValue(feature_name).empty()) missing_name_count++;
                    const std::string lga_name = firstDisplayProperty(fg, {"lganame"});
                    if (trimDisplayValue(lga_name).empty()) missing_lga_count++;
                }
                import_audit_appender.BeginRow();
                import_audit_appender.Append<const char*>(layer.file.c_str());
                import_audit_appender.Append<const char*>(layer.name.c_str());
                import_audit_appender.Append<const char*>(layer.import_type.c_str());
                import_audit_appender.Append<const char*>(layer.duckdb_role.c_str());
                import_audit_appender.Append<const char*>(layer.provenance_nation_state.c_str());
                import_audit_appender.Append<const char*>(layer.provenance_state_region.c_str());
                import_audit_appender.Append<const char*>(source_artifact_path.string().c_str());
                import_audit_appender.Append<bool>(source_exists);
                import_audit_appender.Append<uint64_t>(source_size_bytes);
                import_audit_appender.Append<const char*>(stored_path.string().c_str());
                import_audit_appender.Append<bool>(stored_exists);
                import_audit_appender.Append<uint64_t>((uint64_t)layer.features.size());
                import_audit_appender.Append<uint64_t>((uint64_t)missing_name_count);
                import_audit_appender.Append<uint64_t>((uint64_t)missing_lga_count);
                import_audit_appender.EndRow();
            }
            import_audit_appender.Close();
        }
        exec_or_throw(R"SQL(
            CREATE TABLE geography_feature_collections AS
            SELECT
                provenance_world,
                provenance_nation_state,
                provenance_state_region,
                provenance_county_city,
                layer_file,
                layer_name,
                duckdb_role,
                scale,
                category,
                count(*) AS feature_count
            FROM layer_features
            GROUP BY
                provenance_world,
                provenance_nation_state,
                provenance_state_region,
                provenance_county_city,
                layer_file,
                layer_name,
                duckdb_role,
                scale,
                category
        )SQL", "create geography_feature_collections");
        exec_or_throw(R"SQL(
            CREATE TABLE parcel_events AS
            WITH base AS (
                SELECT
                    lf.blocklot,
                    lf.owner,
                    lf.address,
                    lf.zipcode,
                    lf.status AS feature_status,
                    lf.layer_file,
                    lf.layer_name,
                    lf.duckdb_role,
                    lf.feature_idx,
                    lf.category,
                    lf.value_usd,
                    lf.event_date_text,
                    lf.event_status_hint,
                    lf.event_year_hint,
                    lf.amount_usd_hint,
                    coalesce(
                        try_strptime(lf.event_date_text, '%Y-%m-%dT%H:%M:%SZ'),
                        try_strptime(lf.event_date_text, '%Y-%m-%d')
                    ) AS parsed_event_ts
                FROM layer_features lf
                WHERE lf.duckdb_role = 'parcel_event' AND lf.blocklot IS NOT NULL AND lf.blocklot <> ''
            )
            SELECT
                row_number() OVER () AS event_id,
                blocklot,
                owner,
                address,
                zipcode,
                CASE
                    WHEN lower(layer_file) = 'vacant_building_notices.geojson' THEN 'vacant_notice'
                    WHEN lower(layer_file) = 'vacant_building_rehabs.geojson' THEN 'vacant_rehab'
                    WHEN lower(layer_file) LIKE '%tax_lien%' THEN 'tax_lien'
                    WHEN lower(layer_file) LIKE '%tax_sale%' THEN 'tax_sale'
                    WHEN lower(layer_file) LIKE '%open_bid_list_vacants_to_value%' THEN 'vacants_to_value_bid'
                    WHEN lower(layer_name) LIKE '%vacant%' THEN 'vacancy_related'
                    WHEN lower(category) = 'housing' THEN 'housing'
                    WHEN lower(category) = 'permits' THEN 'permit'
                    WHEN lower(category) = 'taxes' THEN 'tax'
                    ELSE duckdb_role
                END AS event_type,
                coalesce(
                    nullif(event_status_hint, ''),
                    feature_status
                ) AS event_status,
                cast(parsed_event_ts AS DATE) AS event_date,
                coalesce(
                    nullif(event_year_hint, 0),
                    try_cast(strftime(parsed_event_ts, '%Y') AS INTEGER)
                ) AS event_year,
                coalesce(
                    nullif(value_usd, 0),
                    nullif(amount_usd_hint, 0)
                ) AS amount_usd,
                layer_file AS source_layer_file,
                layer_name AS source_layer_name,
                feature_idx AS source_feature_idx
            FROM base
        )SQL", "create parcel_events");
        exec_or_throw("CREATE INDEX IF NOT EXISTS idx_parcel_events_blocklot ON parcel_events(blocklot)", "index parcel_events blocklot");
        exec_or_throw("CREATE INDEX IF NOT EXISTS idx_parcel_events_event_year ON parcel_events(event_year)", "index parcel_events event_year");
        exec_or_throw("CREATE INDEX IF NOT EXISTS idx_parcel_events_event_type ON parcel_events(event_type)", "index parcel_events event_type");
        exec_or_throw(R"SQL(
            CREATE OR REPLACE VIEW parcel_features AS
            SELECT *
            FROM layer_features
            WHERE scale = 'parcel'
        )SQL", "create parcel_features view");
        exec_or_throw(R"SQL(
            CREATE OR REPLACE VIEW owner_rollups AS
            SELECT
                owner,
                count(*) AS property_count,
                sum(current_value) AS value_usd,
                min(min_lon) AS min_lon,
                min(min_lat) AS min_lat,
                max(max_lon) AS max_lon,
                max(max_lat) AS max_lat
            FROM unified_parcels
            WHERE owner IS NOT NULL AND owner <> ''
            GROUP BY owner
            ORDER BY property_count DESC, value_usd DESC
        )SQL", "create owner_rollups view");
        exec_or_throw(R"SQL(
            CREATE OR REPLACE VIEW layer_counts AS
            SELECT layer_file, layer_name, duckdb_role, scale, category, count(*) AS feature_count
            FROM layer_features
            GROUP BY layer_file, layer_name, duckdb_role, scale, category
            ORDER BY feature_count DESC
        )SQL", "create layer_counts view");
        exec_or_throw(R"SQL(
            CREATE OR REPLACE VIEW analytics_property_source_rollup AS
            SELECT source_file, row_count
            FROM analytics_source_contributions
            WHERE source_role = 'property_record'
            ORDER BY row_count DESC, source_file ASC
        )SQL", "create analytics_property_source_rollup view");
        exec_or_throw("COMMIT", "commit analytics rebuild");

        status_.last_rebuild_ok = true;
        status_.layer_count = layer_count;
        status_.feature_count = feature_count;
        std::ostringstream msg;
        msg << "DuckDB analytics rebuilt: " << feature_count << " features across " << layer_count << " hydrated layers.";
        status_.message = msg.str();
        return true;
    } catch (const std::exception& e) {
        status_.last_rebuild_ok = false;
        status_.message = std::string("DuckDB rebuild failed: ") + e.what();
        return false;
    }
}

DuckDbQueryResult DuckDbAnalytics::executeMapQuery(
    const std::string& sql,
    const std::unordered_set<std::string>& selected_owners,
    const std::vector<DuckDbSelectedParcel>& selected_parcels,
    size_t max_rows) const {
    DuckDbQueryResult out;
    try {
        duckdb::DuckDB db(status_.db_path);
        duckdb::Connection con(db);

        con.Query("CREATE TEMP TABLE ui_selected_owners(owner VARCHAR)");
        for (const auto& owner : selected_owners) {
            con.Query("INSERT INTO ui_selected_owners VALUES ('" + sqlQuote(owner) + "')");
        }
        con.Query("CREATE TEMP TABLE ui_selected_parcels(layer_idx UBIGINT, feature_idx UBIGINT, feature_id VARCHAR, blocklot VARCHAR)");
        for (const auto& parcel : selected_parcels) {
            con.Query(
                "INSERT INTO ui_selected_parcels VALUES (" +
                std::to_string((uint64_t)parcel.layer_idx) + ", " +
                std::to_string((uint64_t)parcel.feature_idx) + ", '" +
                sqlQuote(parcel.feature_id) + "', '" +
                sqlQuote(parcel.blocklot) + "')");
        }

        auto result = con.Query(sql);
        if (!result || result->HasError()) {
            out.message = result ? result->GetError() : "DuckDB query failed.";
            return out;
        }

        out.ok = true;
        out.columns = result->names;
        std::unordered_map<std::string, size_t> col_index;
        for (size_t i = 0; i < out.columns.size(); ++i) {
            col_index[lowerName(out.columns[i])] = i;
        }

        auto find_col = [&](std::initializer_list<const char*> names) -> int {
            for (const char* name : names) {
                auto it = col_index.find(lowerName(name));
                if (it != col_index.end()) return (int)it->second;
            }
            return -1;
        };
        const int layer_col = find_col({"layer_idx", "layer"});
        const int feature_col = find_col({"feature_idx", "feature"});
        const int blocklot_col = find_col({"blocklot", "block_lot"});
        const int owner_col = find_col({"owner", "owner_name"});

        out.result_set.active = true;
        size_t scanned_rows = 0;
        while (auto chunk = result->Fetch()) {
            for (idx_t row = 0; row < chunk->size(); ++row) {
                std::vector<std::string> display_row;
                display_row.reserve(out.columns.size());
                for (idx_t col = 0; col < chunk->ColumnCount(); ++col) {
                    display_row.push_back(chunk->GetValue(col, row).ToString());
                }

                if (layer_col >= 0 && feature_col >= 0 &&
                    (size_t)layer_col < display_row.size() &&
                    (size_t)feature_col < display_row.size()) {
                    try {
                        const uint64_t layer_idx = std::stoull(display_row[(size_t)layer_col]);
                        const uint64_t feature_idx = std::stoull(display_row[(size_t)feature_col]);
                        out.result_set.layers.insert((size_t)layer_idx);
                        out.result_set.features.insert(FeatureKey{(size_t)layer_idx, (size_t)feature_idx});
                    } catch (...) {
                    }
                }
                if (blocklot_col >= 0 && (size_t)blocklot_col < display_row.size()) {
                    const std::string blocklot = normalizeJoinKey(display_row[(size_t)blocklot_col]);
                    if (!blocklot.empty()) out.result_set.blocklots.insert(blocklot);
                }
                if (owner_col >= 0 && (size_t)owner_col < display_row.size()) {
                    const std::string owner = toLowerAscii(trimDisplayValue(display_row[(size_t)owner_col]));
                    if (!owner.empty()) out.result_set.owners.insert(owner);
                }

                scanned_rows++;
                if (out.rows.size() < max_rows) out.rows.push_back(std::move(display_row));
            }
        }

        std::ostringstream msg;
        msg << "Query returned " << scanned_rows << " rows";
        if (out.rows.size() < scanned_rows) msg << " (" << out.rows.size() << " shown)";
        msg << ". Map identities: "
            << out.result_set.features.size() << " features, "
            << out.result_set.blocklots.size() << " blocklots, "
            << out.result_set.owners.size() << " owners.";
        out.message = msg.str();
        return out;
    } catch (const std::exception& e) {
        out.message = std::string("DuckDB query failed: ") + e.what();
        return out;
    }
}

DuckDbQueryResult DuckDbAnalytics::queryParcelJurisdictions(
    size_t parcel_layer_idx,
    const std::unordered_set<std::string>& jurisdictions,
    size_t max_rows) const {
    if (jurisdictions.empty()) {
        DuckDbQueryResult out;
        out.ok = true;
        out.result_set.active = true;
        out.message = "No parcel jurisdictions selected. Map identities: 0 features.";
        return out;
    }

    std::vector<std::string> sorted_jurisdictions;
    sorted_jurisdictions.reserve(jurisdictions.size());
    for (const auto& jurisdiction : jurisdictions) {
        const std::string normalized = normalizeJoinKey(jurisdiction);
        if (!normalized.empty()) sorted_jurisdictions.push_back(normalized);
    }
    std::sort(sorted_jurisdictions.begin(), sorted_jurisdictions.end());
    sorted_jurisdictions.erase(std::unique(sorted_jurisdictions.begin(), sorted_jurisdictions.end()), sorted_jurisdictions.end());
    if (sorted_jurisdictions.empty()) {
        DuckDbQueryResult out;
        out.ok = true;
        out.result_set.active = true;
        out.message = "No valid parcel jurisdictions selected. Map identities: 0 features.";
        return out;
    }
    std::ostringstream in_clause;
    for (size_t i = 0; i < sorted_jurisdictions.size(); ++i) {
        if (i > 0) in_clause << ", ";
        in_clause << "'" << sqlQuote(sorted_jurisdictions[i]) << "'";
    }

    std::ostringstream sql;
    sql << R"SQL(
        SELECT pf.layer_idx, pf.feature_idx, pf.blocklot
        FROM parcel_features pf
        WHERE pf.layer_idx = )SQL" << (uint64_t)parcel_layer_idx << R"SQL(
          AND regexp_replace(upper(coalesce(
                nullif(pf.jurisdiction, ''),
                CASE
                    WHEN pf.layer_file = 'parcel.geojson' THEN 'Baltimore City'
                    WHEN pf.layer_file = 'baltimore_county_parcels.geojson' THEN 'Baltimore County'
                    WHEN pf.layer_file = 'howard_county_parcels.geojson' THEN 'Howard County'
                    ELSE ''
                END
              )), '[^A-Z0-9]', '', 'g') IN ()SQL" << in_clause.str() << R"SQL()
    )SQL";
    return executeMapQuery(sql.str(), {}, {}, max_rows);
}

DuckDbQueryResult DuckDbAnalytics::queryUnifiedParcelDetail(
    size_t parcel_layer_idx,
    size_t parcel_feature_idx) const {
    std::ostringstream sql;
    sql << R"SQL(
        SELECT
            parcel_layer_idx,
            parcel_feature_idx,
            blocklot,
            parcel_source_file,
            property_source_file,
            parcel_has_geometry,
            has_property_record,
            owner,
            owner_display,
            address,
            zipcode,
            status,
            current_land,
            current_improvements,
            structure_area_sqft,
            tax_base,
            sale_price,
            current_value,
            vacant_notice_count,
            vacant_rehab_count,
            tax_lien_count,
            tax_sale_count,
            tax_lien_amount,
            tax_sale_amount,
            min_lon,
            min_lat,
            max_lon,
            max_lat
        FROM unified_parcels
        WHERE parcel_layer_idx = )SQL" << (uint64_t)parcel_layer_idx << R"SQL(
          AND parcel_feature_idx = )SQL" << (uint64_t)parcel_feature_idx << R"SQL(
        LIMIT 1
    )SQL";
    return executeMapQuery(sql.str(), {}, {}, 1);
}

DuckDbQueryResult DuckDbAnalytics::queryParcelEvents(
    const std::string& blocklot,
    size_t max_rows) const {
    const std::string key = trimDisplayValue(blocklot);
    if (key.empty()) {
        DuckDbQueryResult out;
        out.ok = false;
        out.message = "BLOCKLOT is empty";
        return out;
    }
    std::ostringstream sql;
    sql << R"SQL(
        SELECT
            event_type,
            event_status,
            cast(event_date AS VARCHAR) AS event_date,
            cast(event_year AS VARCHAR) AS event_year,
            cast(amount_usd AS VARCHAR) AS amount_usd,
            source_layer_name,
            source_layer_file,
            source_feature_idx
        FROM parcel_events
        WHERE blocklot = ')SQL" << sqlQuote(key) << R"SQL('
        ORDER BY
            coalesce(event_date, DATE '0001-01-01') DESC,
            coalesce(event_year, 0) DESC,
            event_type ASC,
            source_layer_name ASC
        LIMIT )SQL" << std::max<size_t>(1, max_rows) << R"SQL(
    )SQL";
    return executeMapQuery(sql.str(), {}, {}, max_rows);
}

std::vector<DuckDbSearchHit> DuckDbAnalytics::searchParcels(const std::string& query, size_t max_rows) const {
    std::vector<DuckDbSearchHit> out;
    const std::string q = trimDisplayValue(query);
    if (q.empty()) return out;
    try {
        duckdb::DuckDB db(status_.db_path);
        duckdb::Connection con(db);
        const std::string normalized_address_query = normalizeAddressSearchText(q);
        const std::string needle = "%" + sqlQuote(toLowerAscii(q)) + "%";
        const std::string address_needle = "%" + sqlQuote(normalized_address_query) + "%";
        const std::string compact = "%" + sqlQuote(normalizeJoinKey(q)) + "%";
        std::ostringstream sql;
        sql << R"SQL(
            SELECT parcel_layer_idx, parcel_feature_idx, blocklot, owner, address, current_value
            FROM unified_parcels
            WHERE lower(coalesce(owner, '')) LIKE ')SQL" << needle << R"SQL('
               OR lower(coalesce(owner_display, '')) LIKE ')SQL" << needle << R"SQL('
               OR coalesce(address_search, '') LIKE ')SQL" << address_needle << R"SQL('
               OR regexp_replace(upper(coalesce(blocklot, '')), '[^A-Z0-9]', '', 'g') LIKE ')SQL" << compact << R"SQL('
            LIMIT )SQL" << std::max<size_t>(max_rows * 8, 200) << ";";
        auto result = con.Query(sql.str());
        if (!result || result->HasError()) return out;
        while (auto chunk = result->Fetch()) {
            for (idx_t row = 0; row < chunk->size(); ++row) {
                DuckDbSearchHit hit;
                hit.layer_idx = (size_t)chunk->GetValue(0, row).GetValue<uint64_t>();
                hit.feature_idx = (size_t)chunk->GetValue(1, row).GetValue<uint64_t>();
                hit.feature_id.clear();
                hit.blocklot = chunk->GetValue(2, row).ToString();
                hit.owner = chunk->GetValue(3, row).ToString();
                hit.address = chunk->GetValue(4, row).ToString();
                hit.current_value = chunk->GetValue(5, row).GetValue<double>();
                hit.score = std::max({
                    fuzzyTextScore(hit.owner, q),
                    fuzzyTextScore(hit.address, q),
                    fuzzyTextScore(hit.blocklot, q)
                });
                if (hit.score > 0) out.push_back(std::move(hit));
            }
        }
        std::stable_sort(out.begin(), out.end(), [](const DuckDbSearchHit& a, const DuckDbSearchHit& b) {
            if (a.score != b.score) return a.score > b.score;
            if (std::abs(a.current_value - b.current_value) > 0.5) return a.current_value > b.current_value;
            return a.blocklot < b.blocklot;
        });
        if (out.size() > max_rows) out.resize(max_rows);
    } catch (...) {
    }
    return out;
}
