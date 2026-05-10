#include "duckdb_analytics.h"

#include "app_utils.h"
#include "feature_props.h"

#include <duckdb.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
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
}

DuckDbAnalytics::DuckDbAnalytics(std::filesystem::path root)
    : root_(std::move(root)) {
    status_.db_path = (root_ / "data" / "worldsim.duckdb").string();
    status_.available = true;
    status_.message = "DuckDB analytics cache not built yet.";
}

bool DuckDbAnalytics::needsRebuild(const std::vector<LayerDef>& layers) const {
    std::error_code ec;
    const fs::path db_path = status_.db_path;
    if (!fs::exists(db_path, ec) || ec) return true;
    const fs::file_time_type db_time = fs::last_write_time(db_path, ec);
    if (ec) return true;

    for (const auto& layer : layers) {
        if (layer.file.empty()) continue;
        const fs::path layer_path = root_ / "data" / "layers" / layer.file;
        ec.clear();
        if (!fs::exists(layer_path, ec) || ec) continue;
        const fs::file_time_type layer_time = fs::last_write_time(layer_path, ec);
        if (!ec && layer_time > db_time) return true;
    }
    return false;
}

bool DuckDbAnalytics::rebuild(const std::vector<LayerDef>& layers, const std::vector<UnifiedParcelRecord>& unified_parcels) {
    try {
        std::filesystem::create_directories(root_ / "data");
        duckdb::DuckDB db(status_.db_path);
        duckdb::Connection con(db);

        con.Query("BEGIN TRANSACTION");
        con.Query("DROP TABLE IF EXISTS layer_features");
        con.Query("DROP TABLE IF EXISTS unified_parcels");
        con.Query("DROP TABLE IF EXISTS parcel_events");
        con.Query(R"SQL(
            CREATE TABLE layer_features (
                layer_idx UBIGINT,
                layer_name VARCHAR,
                layer_file VARCHAR,
                feature_idx UBIGINT,
                scale VARCHAR,
                category VARCHAR,
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
                properties_json VARCHAR
            )
        )SQL");

        auto appender = duckdb::Appender(con, "layer_features");
        size_t feature_count = 0;
        size_t layer_count = 0;
        for (size_t li = 0; li < layers.size(); ++li) {
            const auto& layer = layers[li];
            if (layer.features.empty()) continue;
            layer_count++;
            const std::string category = categoryToString(layer.category);
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

                appender.BeginRow();
                appender.Append<uint64_t>((uint64_t)li);
                appender.Append<const char*>(layer.name.c_str());
                appender.Append<const char*>(layer.file.c_str());
                appender.Append<uint64_t>((uint64_t)fi);
                appender.Append<const char*>(layer.scale.c_str());
                appender.Append<const char*>(category.c_str());
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
                const std::string pj = propsJson(fg);
                appender.Append<const char*>(pj.c_str());
                appender.EndRow();
                feature_count++;
            }
        }
        appender.Close();

        con.Query(R"SQL(
            CREATE TABLE unified_parcels (
                parcel_layer_idx UBIGINT,
                parcel_feature_idx UBIGINT,
                real_property_feature_idx UBIGINT,
                blocklot VARCHAR,
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
        )SQL");
        if (!unified_parcels.empty()) {
            auto parcel_appender = duckdb::Appender(con, "unified_parcels");
            for (const auto& parcel : unified_parcels) {
                if (!parcel.parcel_geom) continue;
                parcel_appender.BeginRow();
                parcel_appender.Append<uint64_t>((uint64_t)parcel.parcel_layer_idx);
                parcel_appender.Append<uint64_t>((uint64_t)parcel.parcel_feature_idx);
                parcel_appender.Append<uint64_t>(parcel.real_property_feature_idx == (size_t)-1 ? UINT64_MAX : (uint64_t)parcel.real_property_feature_idx);
                parcel_appender.Append<const char*>(parcel.blocklot.c_str());
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

        con.Query("CREATE INDEX IF NOT EXISTS idx_layer_features_blocklot ON layer_features(blocklot)");
        con.Query("CREATE INDEX IF NOT EXISTS idx_layer_features_owner ON layer_features(owner)");
        con.Query("CREATE INDEX IF NOT EXISTS idx_layer_features_layer_file ON layer_features(layer_file)");
        con.Query("CREATE INDEX IF NOT EXISTS idx_unified_parcels_blocklot ON unified_parcels(blocklot)");
        con.Query("CREATE INDEX IF NOT EXISTS idx_unified_parcels_owner ON unified_parcels(owner)");
        con.Query(R"SQL(
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
                    lf.feature_idx,
                    lf.category,
                    lf.value_usd,
                    lf.properties_json,
                    coalesce(
                        try_strptime(json_extract_string(lf.properties_json, '$.event_date'), '%Y-%m-%dT%H:%M:%SZ'),
                        try_strptime(json_extract_string(lf.properties_json, '$.event_date'), '%Y-%m-%d'),
                        try_strptime(json_extract_string(lf.properties_json, '$.DateNotice'), '%Y-%m-%dT%H:%M:%SZ'),
                        try_strptime(json_extract_string(lf.properties_json, '$.DateNotice'), '%Y-%m-%d'),
                        try_strptime(json_extract_string(lf.properties_json, '$.DateIssue'), '%Y-%m-%dT%H:%M:%SZ'),
                        try_strptime(json_extract_string(lf.properties_json, '$.DateIssue'), '%Y-%m-%d'),
                        try_strptime(json_extract_string(lf.properties_json, '$.DateIssued'), '%Y-%m-%dT%H:%M:%SZ'),
                        try_strptime(json_extract_string(lf.properties_json, '$.DateIssued'), '%Y-%m-%d'),
                        try_strptime(json_extract_string(lf.properties_json, '$.Issue_Date_ISO'), '%Y-%m-%dT%H:%M:%SZ'),
                        try_strptime(json_extract_string(lf.properties_json, '$.Issue_Date_ISO'), '%Y-%m-%d'),
                        try_strptime(json_extract_string(lf.properties_json, '$.Issue_Date'), '%Y-%m-%dT%H:%M:%SZ'),
                        try_strptime(json_extract_string(lf.properties_json, '$.Issue_Date'), '%Y-%m-%d'),
                        try_strptime(json_extract_string(lf.properties_json, '$.SALEDATE'), '%Y-%m-%dT%H:%M:%SZ'),
                        try_strptime(json_extract_string(lf.properties_json, '$.SALEDATE'), '%Y-%m-%d'),
                        try_strptime(json_extract_string(lf.properties_json, '$.DATE'), '%Y-%m-%dT%H:%M:%SZ'),
                        try_strptime(json_extract_string(lf.properties_json, '$.DATE'), '%Y-%m-%d'),
                        try_strptime(json_extract_string(lf.properties_json, '$.CREATED_DATE'), '%Y-%m-%dT%H:%M:%SZ'),
                        try_strptime(json_extract_string(lf.properties_json, '$.CREATED_DATE'), '%Y-%m-%d'),
                        try_strptime(json_extract_string(lf.properties_json, '$.RECORD_DATE'), '%Y-%m-%dT%H:%M:%SZ'),
                        try_strptime(json_extract_string(lf.properties_json, '$.RECORD_DATE'), '%Y-%m-%d')
                    ) AS parsed_event_ts
                FROM layer_features lf
                WHERE lf.scale = 'parcel' AND lf.blocklot IS NOT NULL AND lf.blocklot <> ''
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
                    ELSE 'parcel_event'
                END AS event_type,
                coalesce(
                    nullif(json_extract_string(properties_json, '$.CASE_STATUS'), ''),
                    nullif(json_extract_string(properties_json, '$.STATUS'), ''),
                    nullif(json_extract_string(properties_json, '$.STATE'), ''),
                    feature_status
                ) AS event_status,
                cast(parsed_event_ts AS DATE) AS event_date,
                coalesce(
                    try_cast(json_extract_string(properties_json, '$.Issue_Year') AS INTEGER),
                    try_cast(json_extract_string(properties_json, '$.YEAR') AS INTEGER),
                    try_cast(strftime(parsed_event_ts, '%Y') AS INTEGER)
                ) AS event_year,
                coalesce(
                    nullif(value_usd, 0),
                    try_cast(json_extract_string(properties_json, '$.amount_usd') AS DOUBLE),
                    try_cast(json_extract_string(properties_json, '$.AMOUNT') AS DOUBLE),
                    try_cast(json_extract_string(properties_json, '$.Amount') AS DOUBLE),
                    try_cast(json_extract_string(properties_json, '$.COST') AS DOUBLE),
                    try_cast(json_extract_string(properties_json, '$.TOTALCOST') AS DOUBLE),
                    try_cast(json_extract_string(properties_json, '$.SALEPRIC') AS DOUBLE),
                    try_cast(json_extract_string(properties_json, '$.TAXBASE') AS DOUBLE),
                    try_cast(json_extract_string(properties_json, '$.ARTAXBAS') AS DOUBLE)
                ) AS amount_usd,
                layer_file AS source_layer_file,
                layer_name AS source_layer_name,
                feature_idx AS source_feature_idx,
                properties_json
            FROM base
        )SQL");
        con.Query("CREATE INDEX IF NOT EXISTS idx_parcel_events_blocklot ON parcel_events(blocklot)");
        con.Query("CREATE INDEX IF NOT EXISTS idx_parcel_events_event_year ON parcel_events(event_year)");
        con.Query("CREATE INDEX IF NOT EXISTS idx_parcel_events_event_type ON parcel_events(event_type)");
        con.Query(R"SQL(
            CREATE OR REPLACE VIEW parcel_features AS
            SELECT *
            FROM layer_features
            WHERE scale = 'parcel'
        )SQL");
        con.Query(R"SQL(
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
        )SQL");
        con.Query(R"SQL(
            CREATE OR REPLACE VIEW layer_counts AS
            SELECT layer_file, layer_name, scale, category, count(*) AS feature_count
            FROM layer_features
            GROUP BY layer_file, layer_name, scale, category
            ORDER BY feature_count DESC
        )SQL");
        con.Query("COMMIT");

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
