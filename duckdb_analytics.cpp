#include "duckdb_analytics.h"

#include "app_utils.h"
#include "cache_io.h"
#include "feature_props.h"

#include <duckdb.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
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

std::string prop(const LayerDef::FeatureGeom& fg, std::initializer_list<const char*> keys) {
    return firstDisplayProperty(fg, keys);
}

std::string propsJson(const LayerDef::FeatureGeom& fg) {
    json obj = json::object();
    for (const auto& kv : fg.properties) obj[kv.first] = kv.second;
    return obj.dump();
}

double numericProp(const LayerDef::FeatureGeom& fg, std::initializer_list<const char*> keys) {
    return parseNumericField(firstDisplayProperty(fg, keys));
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

fs::path anambraRepositoryManifestPath(const fs::path& root) {
    return root / "sources" / "world" / "earth" / "nation_state" / "ng" / "state_region" / "anambra" /
        "layers_manifest.repository.json";
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

std::string analyticsBuildSignature(const fs::path& root, const std::vector<LayerDef>& layers) {
    std::ostringstream sig;
    for (const auto& layer : layers) {
        sig << layer.file << ":runtime=" << (layer.runtime_load ? 1 : 0)
            << ":duckdb=" << (layer.duckdb_ingest ? 1 : 0)
            << ":role=" << layer.duckdb_role << "|";
        if (!layer.duckdb_ingest) continue;
        const fs::path layer_path = resolveStoredLayerPath(root, layer);
        if (!fs::exists(layer_path)) continue;
        sig << "sig=" << fileSignature(layer_path) << "|";
    }
    return sig.str();
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
              AND table_name IN ('layer_features', 'unified_parcels', 'parcel_events')
        )SQL");
        if (!table_check || table_check->HasError() || table_check->RowCount() == 0) {
            status_.last_rebuild_ok = false;
            status_.message = "DuckDB analytics cache validation failed.";
            return false;
        }
        const int64_t table_count = table_check->GetValue<int64_t>(0, 0);
        if (table_count < 3) {
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

bool DuckDbAnalytics::rebuild(const std::vector<LayerDef>& layers, const std::vector<UnifiedParcelRecord>& unified_parcels) {
    try {
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
        exec_or_throw("DROP TABLE IF EXISTS unified_parcels", "drop unified_parcels");
        exec_or_throw("DROP TABLE IF EXISTS parcel_events", "drop parcel_events");
        exec_or_throw("DROP TABLE IF EXISTS anambra_repository_sources", "drop anambra_repository_sources");
        exec_or_throw("DROP TABLE IF EXISTS geography_feature_collections", "drop geography_feature_collections");
        exec_or_throw("DROP TABLE IF EXISTS anambra_runtime_features", "drop anambra_runtime_features");
        exec_or_throw("DROP TABLE IF EXISTS anambra_runtime_lga_summary", "drop anambra_runtime_lga_summary");
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
                value_usd DOUBLE,
                feature_name VARCHAR,
                lga_name VARCHAR,
                ward_name VARCHAR,
                source_name VARCHAR,
                event_date_text VARCHAR,
                event_status_hint VARCHAR,
                event_year_hint INTEGER,
                amount_usd_hint DOUBLE,
                properties_json VARCHAR
            )
        )SQL", "create layer_features");

        auto appender = duckdb::Appender(con, "layer_features");
        size_t feature_count = 0;
        size_t layer_count = 0;
        for (size_t li = 0; li < layers.size(); ++li) {
            const auto& layer = layers[li];
            if (!layer.duckdb_ingest) continue;
            if (layer.features.empty()) continue;
            layer_count++;
            const std::string category = categoryToString(layer.category);
            const std::string duckdb_role = layer.duckdb_role.empty() ? "layer_feature" : layer.duckdb_role;
            for (size_t fi = 0; fi < layer.features.size(); ++fi) {
                const auto& fg = layer.features[fi];
                const std::string blocklot = featureBlockLotJoinKey(fg);
                std::string owner = toLowerAscii(trimDisplayValue(prop(fg, {"OWNER_1", "OWNERNME1", "OWNER", "OWNER_NAME", "AR_OWNER", "OWNER_ABBR"})));
                const std::string address = prop(fg, {
                    "FULLADDR", "FULL_ADDRESS", "PROPERTY_ADDRESS", "PROPERTYADDR", "PREMISEADD",
                    "PREMISE_ADDRESS", "ADDRESS", "Address", "ADDR", "ADDR1", "ADDRESS1",
                    "SITE_ADDR", "SITUSADDR", "LOCATION", "Location"
                });
                const std::string zipcode = prop(fg, {"ZIP", "ZIPCODE", "POSTAL_CODE"});
                const std::string status = prop(fg, {"STATUS", "STATE", "CASE_STATUS"});
                const std::string zoning = zoningClassKey(fg);
                double value = numericProp(fg, {"value_usd", "property_value_usd", "TAXBASE", "ARTAXBAS", "SALEPRIC"});
                const std::string feature_name = firstDisplayProperty(
                    fg, {"name", "poi_name", "prmry_name", "set_name", "market_nam", "plc_st_nam", "fctry_st_n"});
                const std::string lga_name = firstDisplayProperty(fg, {"lganame"});
                const std::string ward_name = firstDisplayProperty(fg, {"wardname"});
                const std::string source_name = firstDisplayProperty(fg, {"source"});
                const std::string event_date_text = prop(fg, {
                    "event_date", "DateNotice", "DateIssue", "DateIssued", "Issue_Date_ISO",
                    "Issue_Date", "SALEDATE", "DATE", "CREATED_DATE", "RECORD_DATE"
                });
                const std::string event_status_hint = prop(fg, {"CASE_STATUS", "STATUS", "STATE"});
                int event_year_hint = 0;
                if (const std::string year_text = prop(fg, {"Issue_Year", "YEAR"}); !trimDisplayValue(year_text).empty()) {
                    event_year_hint = (int)parseNumericField(year_text);
                }
                double amount_usd_hint = numericProp(
                    fg, {"amount_usd", "AMOUNT", "Amount", "COST", "TOTALCOST", "SALEPRIC", "TAXBASE", "ARTAXBAS"});

                appender.BeginRow();
                appender.Append<uint64_t>((uint64_t)li);
                appender.Append<const char*>(layer.name.c_str());
                appender.Append<const char*>(layer.file.c_str());
                appender.Append<const char*>(duckdb_role.c_str());
                appender.Append<uint64_t>((uint64_t)fi);
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
                appender.Append<double>(value);
                appender.Append<const char*>(feature_name.c_str());
                appender.Append<const char*>(lga_name.c_str());
                appender.Append<const char*>(ward_name.c_str());
                appender.Append<const char*>(source_name.c_str());
                appender.Append<const char*>(event_date_text.c_str());
                appender.Append<const char*>(event_status_hint.c_str());
                appender.Append<int32_t>(event_year_hint);
                appender.Append<double>(amount_usd_hint);
                const std::string pj = propsJson(fg);
                appender.Append<const char*>(pj.c_str());
                appender.EndRow();
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
                zipcode VARCHAR,
                status VARCHAR,
                current_land DOUBLE,
                current_improvements DOUBLE,
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
        if (!unified_parcels.empty()) {
            auto parcel_appender = duckdb::Appender(con, "unified_parcels");
            for (const auto& parcel : unified_parcels) {
                if (!parcel.parcel_geom) continue;
                parcel_appender.BeginRow();
                parcel_appender.Append<uint64_t>((uint64_t)parcel.parcel_layer_idx);
                parcel_appender.Append<uint64_t>((uint64_t)parcel.parcel_feature_idx);
                parcel_appender.Append<uint64_t>(parcel.real_property_feature_idx == (size_t)-1 ? UINT64_MAX : (uint64_t)parcel.real_property_feature_idx);
                parcel_appender.Append<const char*>(parcel.blocklot.c_str());
                parcel_appender.Append<const char*>(parcel.parcel_source_file.c_str());
                parcel_appender.Append<const char*>(parcel.property_source_file.c_str());
                parcel_appender.Append<bool>(parcel.parcel_has_geometry);
                parcel_appender.Append<bool>(parcel.has_property_record);
                parcel_appender.Append<const char*>(parcel.owner.c_str());
                parcel_appender.Append<const char*>(parcel.owner_display.c_str());
                parcel_appender.Append<const char*>(parcel.address.c_str());
                parcel_appender.Append<const char*>(parcel.zip.c_str());
                parcel_appender.Append<const char*>(parcel.status.c_str());
                parcel_appender.Append<double>(parcel.current_land);
                parcel_appender.Append<double>(parcel.current_improvements);
                parcel_appender.Append<double>(parcel.tax_base);
                parcel_appender.Append<double>(parcel.sale_price);
                parcel_appender.Append<double>(parcel.current_value);
                parcel_appender.Append<int32_t>((int32_t)parcel.vacant_notice_count);
                parcel_appender.Append<int32_t>((int32_t)parcel.vacant_rehab_count);
                parcel_appender.Append<int32_t>((int32_t)parcel.tax_lien_count);
                parcel_appender.Append<int32_t>((int32_t)parcel.tax_sale_count);
                parcel_appender.Append<double>(parcel.tax_lien_amount);
                parcel_appender.Append<double>(parcel.tax_sale_amount);
                parcel_appender.Append<double>(parcel.parcel_geom->extent.min_lon);
                parcel_appender.Append<double>(parcel.parcel_geom->extent.min_lat);
                parcel_appender.Append<double>(parcel.parcel_geom->extent.max_lon);
                parcel_appender.Append<double>(parcel.parcel_geom->extent.max_lat);
                parcel_appender.EndRow();
            }
            parcel_appender.Close();
        }

        exec_or_throw("CREATE INDEX IF NOT EXISTS idx_layer_features_blocklot ON layer_features(blocklot)", "index layer_features blocklot");
        exec_or_throw("CREATE INDEX IF NOT EXISTS idx_layer_features_owner ON layer_features(owner)", "index layer_features owner");
        exec_or_throw("CREATE INDEX IF NOT EXISTS idx_layer_features_layer_file ON layer_features(layer_file)", "index layer_features layer_file");
        exec_or_throw("CREATE INDEX IF NOT EXISTS idx_layer_features_geography ON layer_features(provenance_nation_state, provenance_state_region)", "index layer_features geography");
        exec_or_throw("CREATE INDEX IF NOT EXISTS idx_unified_parcels_blocklot ON unified_parcels(blocklot)", "index unified_parcels blocklot");
        exec_or_throw("CREATE INDEX IF NOT EXISTS idx_unified_parcels_owner ON unified_parcels(owner)", "index unified_parcels owner");
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
            CREATE TABLE anambra_repository_sources (
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
                provenance_nation_state VARCHAR,
                provenance_state_region VARCHAR
            )
        )SQL", "create anambra_repository_sources");
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
            size_t parcels_with_property_record = 0;
            size_t parcels_with_geometry = 0;
            std::unordered_map<std::string, size_t> parcel_source_counts;
            std::unordered_map<std::string, size_t> property_source_counts;
            for (const auto& parcel : unified_parcels) {
                if (parcel.has_property_record) parcels_with_property_record++;
                if (parcel.parcel_has_geometry) parcels_with_geometry++;
                if (!parcel.parcel_source_file.empty()) parcel_source_counts[parcel.parcel_source_file] += 1;
                if (!parcel.property_source_file.empty()) property_source_counts[parcel.property_source_file] += 1;
            }

            auto info_appender = duckdb::Appender(con, "analytics_build_info");
            info_appender.BeginRow();
            info_appender.Append<const char*>(built_at_utc.c_str());
            info_appender.Append<const char*>(source_signature.c_str());
            info_appender.Append<uint64_t>((uint64_t)layer_count);
            info_appender.Append<uint64_t>((uint64_t)feature_count);
            info_appender.Append<uint64_t>((uint64_t)unified_parcels.size());
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
            std::ifstream in(anambraRepositoryManifestPath(root_));
            json arr;
            if (in) {
                try {
                    in >> arr;
                } catch (...) {
                    arr = json::array();
                }
            }
            if (arr.is_array()) {
                auto anambra_appender = duckdb::Appender(con, "anambra_repository_sources");
                for (const auto& item : arr) {
                    if (!item.is_object()) continue;
                    const fs::path local_path = manifestItemOutputPath(root_, item);
                    std::error_code ec;
                    const bool local_exists = !local_path.empty() && fs::exists(local_path, ec) && !ec;
                    const uint64_t file_size_bytes =
                        local_exists ? (uint64_t)fs::file_size(local_path, ec) : 0ULL;
                    const auto& provenance = item.contains("provenance") && item["provenance"].is_object()
                        ? item["provenance"] : json::object();
                    anambra_appender.BeginRow();
                    anambra_appender.Append<const char*>(item.value("name", std::string()).c_str());
                    anambra_appender.Append<const char*>(item.value("file", std::string()).c_str());
                    anambra_appender.Append<const char*>(item.value("url", std::string()).c_str());
                    anambra_appender.Append<const char*>(item.value("source", std::string()).c_str());
                    anambra_appender.Append<const char*>(item.value("description", std::string()).c_str());
                    anambra_appender.Append<const char*>(local_path.string().c_str());
                    anambra_appender.Append<bool>(local_exists);
                    anambra_appender.Append<uint64_t>(file_size_bytes);
                    anambra_appender.Append<bool>(item.value("download", true));
                    anambra_appender.Append<const char*>(item.value("reason", std::string()).c_str());
                    anambra_appender.Append<const char*>(provenance.value("nation_state", std::string()).c_str());
                    anambra_appender.Append<const char*>(provenance.value("state_region", std::string()).c_str());
                    anambra_appender.EndRow();
                }
                anambra_appender.Close();
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
            CREATE TABLE anambra_runtime_features AS
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
                    feature_name,
                    lga_name,
                    ward_name,
                    source_name,
                    properties_json
            FROM layer_features
            WHERE provenance_nation_state = 'ng'
              AND provenance_state_region = 'anambra'
        )SQL", "create anambra_runtime_features");
        exec_or_throw(R"SQL(
            CREATE TABLE anambra_runtime_lga_summary AS
            SELECT
                layer_file,
                layer_name,
                coalesce(lga_name, '') AS lga_name,
                count(*) AS feature_count
            FROM anambra_runtime_features
            GROUP BY layer_file, layer_name, coalesce(lga_name, '')
            ORDER BY layer_file, feature_count DESC, lga_name
        )SQL", "create anambra_runtime_lga_summary");
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
                    lf.properties_json,
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
                feature_idx AS source_feature_idx,
                properties_json
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
        con.Query("CREATE TEMP TABLE ui_selected_parcels(layer_idx UBIGINT, feature_idx UBIGINT, blocklot VARCHAR)");
        for (const auto& parcel : selected_parcels) {
            con.Query(
                "INSERT INTO ui_selected_parcels VALUES (" +
                std::to_string((uint64_t)parcel.layer_idx) + ", " +
                std::to_string((uint64_t)parcel.feature_idx) + ", '" +
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
        SELECT layer_idx, feature_idx, blocklot
        FROM parcel_features
        WHERE layer_idx = )SQL" << (uint64_t)parcel_layer_idx << R"SQL(
          AND regexp_replace(upper(coalesce(
                nullif(json_extract_string(properties_json, '$.jurisdiction'), ''),
                CASE
                    WHEN layer_file = 'parcel.geojson' THEN 'Baltimore City'
                    WHEN layer_file = 'baltimore_county_parcels.geojson' THEN 'Baltimore County'
                    WHEN layer_file = 'howard_county_parcels.geojson' THEN 'Howard County'
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
        const std::string needle = "%" + sqlQuote(toLowerAscii(q)) + "%";
        const std::string compact = "%" + sqlQuote(normalizeJoinKey(q)) + "%";
        std::ostringstream sql;
        sql << R"SQL(
            SELECT parcel_layer_idx, parcel_feature_idx, blocklot, owner, address, current_value
            FROM unified_parcels
            WHERE lower(coalesce(owner, '')) LIKE ')SQL" << needle << R"SQL('
               OR lower(coalesce(owner_display, '')) LIKE ')SQL" << needle << R"SQL('
               OR lower(coalesce(address, '')) LIKE ')SQL" << needle << R"SQL('
               OR regexp_replace(upper(coalesce(blocklot, '')), '[^A-Z0-9]', '', 'g') LIKE ')SQL" << compact << R"SQL('
            LIMIT )SQL" << std::max<size_t>(max_rows * 8, 200) << ";";
        auto result = con.Query(sql.str());
        if (!result || result->HasError()) return out;
        while (auto chunk = result->Fetch()) {
            for (idx_t row = 0; row < chunk->size(); ++row) {
                DuckDbSearchHit hit;
                hit.layer_idx = (size_t)chunk->GetValue(0, row).GetValue<uint64_t>();
                hit.feature_idx = (size_t)chunk->GetValue(1, row).GetValue<uint64_t>();
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
