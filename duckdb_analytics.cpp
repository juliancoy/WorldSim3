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
#include <optional>
#include <sstream>
#include <unordered_map>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
constexpr int kAnalyticsSchemaVersion = 6;

struct AnalyticsLayerStateRow {
    size_t layer_idx = 0;
    std::string layer_file;
    std::string source_signature;
};

fs::path analyticsSignatureSidecarPath(const fs::path& db_path) {
    return fs::path(db_path.string() + ".source_signature");
}

bool loadAnalyticsSignatureSidecar(const fs::path& db_path, std::string& out_signature) {
    std::ifstream in(analyticsSignatureSidecarPath(db_path));
    if (!in) return false;
    std::getline(in, out_signature);
    return !out_signature.empty();
}

void saveAnalyticsSignatureSidecar(const fs::path& db_path, const std::string& signature) {
    const fs::path sidecar_path = analyticsSignatureSidecarPath(db_path);
    fs::create_directories(sidecar_path.parent_path());
    const fs::path tmp_path = fs::path(sidecar_path.string() + ".tmp");
    {
        std::ofstream out(tmp_path, std::ios::binary);
        if (!out) return;
        out << signature << '\n';
        out.flush();
        if (!out.good()) {
            std::error_code remove_ec;
            fs::remove(tmp_path, remove_ec);
            return;
        }
    }

    std::error_code ec;
    fs::rename(tmp_path, sidecar_path, ec);
    if (ec) {
        ec.clear();
        fs::remove(sidecar_path, ec);
        ec.clear();
        fs::rename(tmp_path, sidecar_path, ec);
        if (ec) {
            std::error_code remove_ec;
            fs::remove(tmp_path, remove_ec);
        }
    }
}

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

bool analyticsLayerStableSignature(const fs::path& root, const LayerDef& layer, std::string& out_signature) {
    const fs::path stored_path = resolveStoredLayerPath(root, layer);
    std::string source_signature;
    if (resolveLayerSourceSignature(stored_path, source_signature, nullptr) && !source_signature.empty()) {
        out_signature = source_signature;
        return true;
    }
    return false;
}

std::string analyticsLayerBuildSignature(const fs::path& root, const LayerDef& layer) {
    std::string stable_layer_signature;
    if (analyticsLayerStableSignature(root, layer, stable_layer_signature)) {
        return stable_layer_signature;
    }
    std::ostringstream sig;
    const std::vector<fs::path> sig_paths = analyticsSourceArtifactPaths(root, layer);
    for (const auto& sig_path : sig_paths) {
        if (!fs::exists(sig_path)) continue;
        sig << sig_path.string() << "=" << fileSignature(sig_path) << "|";
    }
    return sig.str();
}

std::vector<AnalyticsLayerStateRow> analyticsCurrentLayerStates(
    const fs::path& root,
    const std::vector<LayerDef>& layers) {
    std::vector<AnalyticsLayerStateRow> out;
    for (size_t li = 0; li < layers.size(); ++li) {
        const auto& layer = layers[li];
        if (!layer.duckdb_ingest) continue;
        std::ostringstream sig;
        const std::string source_sig = analyticsLayerBuildSignature(root, layer);
        if (source_sig.empty()) continue;
        sig << source_sig
            << "|role=" << layer.duckdb_role
            << "|scale=" << layer.scale
            << "|category=" << categoryToString(layer.category)
            << "|name=" << layer.name
            << "|world=" << layer.provenance_world
            << "|nation_state=" << layer.provenance_nation_state
            << "|state_region=" << layer.provenance_state_region
            << "|county_city=" << layer.provenance_county_city;
        out.push_back(AnalyticsLayerStateRow{
            li,
            layer.file,
            sig.str()
        });
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
        const std::string layer_signature = analyticsLayerBuildSignature(root, layer);
        if (!layer_signature.empty()) sig << "sig=" << layer_signature << "|";
    }
    return sig.str();
}

bool loadStoredAnalyticsLayerStates(
    duckdb::Connection& con,
    std::vector<AnalyticsLayerStateRow>& out) {
    out.clear();
    auto res = con.Query(R"SQL(
        SELECT layer_idx, layer_file, source_signature
        FROM analytics_layer_state
        ORDER BY layer_idx, layer_file
    )SQL");
    if (!res || res->HasError()) return false;
    while (auto chunk = res->Fetch()) {
        for (idx_t row = 0; row < chunk->size(); ++row) {
            AnalyticsLayerStateRow state;
            state.layer_idx = (size_t)chunk->GetValue(0, row).GetValue<uint64_t>();
            state.layer_file = chunk->GetValue(1, row).ToString();
            state.source_signature = chunk->GetValue(2, row).ToString();
            out.push_back(std::move(state));
        }
    }
    return true;
}

bool analyticsLayerStateTableReady(duckdb::Connection& con) {
    auto res = con.Query(R"SQL(
        SELECT count(*)::BIGINT
        FROM information_schema.tables
        WHERE table_schema = 'main'
          AND table_name = 'analytics_layer_state'
    )SQL");
    return res && !res->HasError() && res->RowCount() > 0 && res->GetValue<int64_t>(0, 0) == 1;
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
        const std::string entity_id = normalizeJoinKey("entity:" + layer.file + ":" + normalizeJoinKey(acct));

        appender.BeginRow();
        appender.Append<uint64_t>((uint64_t)layer_idx);
        appender.Append<const char*>(layer.name.c_str());
        appender.Append<const char*>(layer.file.c_str());
        appender.Append<const char*>(duckdb_role.c_str());
        appender.Append<uint64_t>((uint64_t)local_feature_idx);
        appender.Append<const char*>(entity_id.c_str());
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

void appendAnalyticsLayerFeatures(
    const fs::path& root,
    duckdb::Connection& con,
    const std::vector<LayerDef>& layers,
    const std::unordered_set<std::string>* layer_file_filter,
    size_t& feature_count,
    size_t& layer_count) {
    auto appender = duckdb::Appender(con, "layer_features");
    feature_count = 0;
    layer_count = 0;
    for (size_t li = 0; li < layers.size(); ++li) {
        const auto& source_layer = layers[li];
        if (!source_layer.duckdb_ingest) continue;
        if (layer_file_filter && !layer_file_filter->contains(source_layer.file)) continue;
        if (source_layer.import_type == "socrata_csv_properties") {
            const fs::path csv_path =
                provenanceSourceArtifactPath(root, source_layer, source_layer.file + ".source.csv");
            if (!fs::exists(csv_path)) continue;
            layer_count++;
            appendSocrataHowardPropertyRows(appender, li, source_layer, csv_path, feature_count);
            continue;
        }
        LayerDef layer;
        if (!ensureAnalyticsLayerFeaturesLoaded(root, source_layer, layer)) continue;
        if (layer.features.empty()) continue;
        rebuildFeaturePropertyRegistryForLayer(layer);
        layer_count++;
        const std::string category = categoryToString(layer.category);
        const std::string duckdb_role = layer.duckdb_role.empty() ? "layer_feature" : layer.duckdb_role;
        for (size_t fi = 0; fi < layer.features.size(); ++fi) {
            const auto& fg = layer.features[fi];
            const std::string entity_id = featureEntityIdForLayerFeature(layer, fg, fi);
            const std::string blocklot = featureBlockLotJoinKey(fg);
            std::string owner = toLowerAscii(trimDisplayValue(firstDisplayProperty(
                fg, {"OWNER_1", "OWNERNME1", "OWNER", "OWNER_NAME", "AR_OWNER", "OWNER_ABBR"})));
            const std::string address = firstDisplayProperty(fg, {
                "FULLADDR", "FULL_ADDRESS", "PROPERTY_ADDRESS", "PROPERTYADDR", "PREMISEADD",
                "PREMISE_ADDRESS", "ADDRESS", "Address", "ADDR", "ADDR1", "ADDRESS1",
                "SITE_ADDR", "SITUSADDR", "LOCATION", "Location"
            });
            const std::string zipcode = firstDisplayProperty(fg, {"ZIP", "ZIPCODE", "POSTAL_CODE"});
            const std::string status = firstDisplayProperty(fg, {"STATUS", "STATE", "CASE_STATUS"});
            const std::string zoning = zoningClassKey(fg);
            const std::string jurisdiction = firstDisplayProperty(
                fg, {"jurisdiction", "JURISDICTION", "COUNTY", "COUNTYNAME", "County"});
            const double value = numericProp(fg, {"value_usd", "property_value_usd", "TAXBASE", "ARTAXBAS", "SALEPRIC"});
            const double structure_area_sqft = structureAreaSqFtProp(fg);
            const std::string feature_name = firstDisplayProperty(
                fg, {"name", "poi_name", "prmry_name", "set_name", "market_nam", "plc_st_nam", "fctry_st_n"});
            const std::string lga_name = firstDisplayProperty(fg, {"lganame"});
            const std::string ward_name = firstDisplayProperty(fg, {"wardname"});
            const std::string source_name = firstDisplayProperty(fg, {"source"});
            const std::string event_date_text = firstDisplayProperty(fg, {
                "event_date", "DateNotice", "DateIssue", "DateIssued", "Issue_Date_ISO",
                "Issue_Date", "SALEDATE", "DATE", "CREATED_DATE", "RECORD_DATE"
            });
            const std::string event_status_hint = firstDisplayProperty(fg, {"CASE_STATUS", "STATUS", "STATE"});
            int event_year_hint = 0;
            if (const std::string year_text = firstDisplayProperty(fg, {"Issue_Year", "YEAR"});
                !trimDisplayValue(year_text).empty()) {
                event_year_hint = (int)parseNumericField(year_text);
            }
            const double amount_usd_hint = numericProp(
                fg, {"amount_usd", "AMOUNT", "Amount", "COST", "TOTALCOST", "SALEPRIC", "TAXBASE", "ARTAXBAS"});

            appender.BeginRow();
            appender.Append<uint64_t>((uint64_t)li);
            appender.Append<const char*>(layer.name.c_str());
            appender.Append<const char*>(layer.file.c_str());
            appender.Append<const char*>(duckdb_role.c_str());
            appender.Append<uint64_t>((uint64_t)fi);
            appender.Append<const char*>(entity_id.c_str());
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
            feature_count++;
        }
    }
    appender.Close();
}

void rewriteUnifiedParcelsTable(
    duckdb::Connection& con,
    const std::vector<UnifiedParcelRecord>& unified_parcels) {
    auto clear_res = con.Query("DELETE FROM unified_parcels");
    if (!clear_res || clear_res->HasError()) {
        throw std::runtime_error(clear_res ? clear_res->GetError() : "failed to clear unified_parcels");
    }
    auto parcel_appender = duckdb::Appender(con, "unified_parcels");
    for (const auto& parcel : unified_parcels) {
        parcel_appender.BeginRow();
        parcel_appender.Append<uint64_t>((uint64_t)parcel.parcel_layer_idx);
        parcel_appender.Append<const char*>(parcel.parcel_entity_id.c_str());
        parcel_appender.Append<const char*>(parcel.parcel_geometry_entity_id.c_str());
        parcel_appender.Append<const char*>(parcel.blocklot.c_str());
        parcel_appender.Append<const char*>(parcel.parcel_source_file.c_str());
        parcel_appender.Append<const char*>(parcel.property_source_file.c_str());
        parcel_appender.Append<bool>(parcel.parcel_has_geometry);
        parcel_appender.Append<bool>(parcel.has_property_record);
        parcel_appender.Append<const char*>(parcel.owner.c_str());
        parcel_appender.Append<const char*>(parcel.owner_display.c_str());
        parcel_appender.Append<const char*>(parcel.address.c_str());
        parcel_appender.Append<const char*>(parcel.address_search.c_str());
        parcel_appender.Append<const char*>(parcel.zip.c_str());
        parcel_appender.Append<const char*>(parcel.status.c_str());
        parcel_appender.Append<double>(parcel.current_land);
        parcel_appender.Append<double>(parcel.current_improvements);
        parcel_appender.Append<double>(parcel.structure_area_sqft);
        parcel_appender.Append<double>(parcel.tax_base);
        parcel_appender.Append<double>(parcel.sale_price);
        parcel_appender.Append<double>(parcel.current_value);
        parcel_appender.Append<int32_t>(parcel.vacant_notice_count);
        parcel_appender.Append<int32_t>(parcel.vacant_rehab_count);
        parcel_appender.Append<int32_t>(parcel.tax_lien_count);
        parcel_appender.Append<int32_t>(parcel.tax_sale_count);
        parcel_appender.Append<double>(parcel.tax_lien_amount);
        parcel_appender.Append<double>(parcel.tax_sale_amount);
        parcel_appender.Append<double>(parcel.parcel_extent.min_lon);
        parcel_appender.Append<double>(parcel.parcel_extent.min_lat);
        parcel_appender.Append<double>(parcel.parcel_extent.max_lon);
        parcel_appender.Append<double>(parcel.parcel_extent.max_lat);
        parcel_appender.EndRow();
    }
    parcel_appender.Close();
}

void rewriteAnalyticsLayerState(
    duckdb::Connection& con,
    const std::vector<AnalyticsLayerStateRow>& states) {
    auto clear_res = con.Query("DELETE FROM analytics_layer_state");
    if (!clear_res || clear_res->HasError()) {
        throw std::runtime_error(clear_res ? clear_res->GetError() : "failed to clear analytics_layer_state");
    }
    auto appender = duckdb::Appender(con, "analytics_layer_state");
    for (const auto& state : states) {
        appender.BeginRow();
        appender.Append<uint64_t>((uint64_t)state.layer_idx);
        appender.Append<const char*>(state.layer_file.c_str());
        appender.Append<const char*>(state.source_signature.c_str());
        appender.EndRow();
    }
    appender.Close();
}

void rebuildDerivedAnalyticsObjects(
    const fs::path& root,
    duckdb::Connection& con,
    const std::vector<LayerDef>& layers,
    const std::vector<AnalyticsLayerStateRow>& current_states,
    size_t hydrated_layer_count,
    size_t hydrated_feature_count) {
    auto exec_or_throw = [&](const std::string& sql, const char* context) {
        auto res = con.Query(sql);
        if (!res || res->HasError()) {
            throw std::runtime_error(
                std::string(context) + ": " + (res ? res->GetError() : std::string("query failed")));
        }
    };

    exec_or_throw("DELETE FROM analytics_build_info", "clear analytics_build_info");
    exec_or_throw("DELETE FROM analytics_source_contributions", "clear analytics_source_contributions");
    exec_or_throw("DELETE FROM repository_sources", "clear repository_sources");
    exec_or_throw("DELETE FROM import_audit", "clear import_audit");

    {
        const std::string built_at_utc = isoNowUtc();
        const std::string source_signature = analyticsBuildSignature(root, layers);
        size_t unified_parcel_count = 0;
        size_t parcels_with_property_record = 0;
        size_t parcels_with_geometry = 0;
        if (auto count_res = con.Query(
                "SELECT count(*), "
                "coalesce(sum(CASE WHEN has_property_record THEN 1 ELSE 0 END), 0), "
                "coalesce(sum(CASE WHEN parcel_has_geometry THEN 1 ELSE 0 END), 0) "
                "FROM unified_parcels");
            count_res && !count_res->HasError() && count_res->RowCount() > 0) {
            unified_parcel_count = (size_t)count_res->GetValue<int64_t>(0, 0);
            parcels_with_property_record = (size_t)count_res->GetValue<int64_t>(1, 0);
            parcels_with_geometry = (size_t)count_res->GetValue<int64_t>(2, 0);
        }
        auto info_appender = duckdb::Appender(con, "analytics_build_info");
        info_appender.BeginRow();
        info_appender.Append<const char*>(built_at_utc.c_str());
        info_appender.Append<const char*>(source_signature.c_str());
        info_appender.Append<uint64_t>((uint64_t)hydrated_layer_count);
        info_appender.Append<uint64_t>((uint64_t)hydrated_feature_count);
        info_appender.Append<uint64_t>((uint64_t)unified_parcel_count);
        info_appender.Append<uint64_t>((uint64_t)parcels_with_property_record);
        info_appender.Append<uint64_t>((uint64_t)parcels_with_geometry);
        info_appender.EndRow();
        info_appender.Close();
    }

    {
        auto source_appender = duckdb::Appender(con, "analytics_source_contributions");
        if (auto parcel_source_res = con.Query(
                "SELECT parcel_source_file, count(*) "
                "FROM unified_parcels WHERE parcel_source_file <> '' GROUP BY 1");
            parcel_source_res && !parcel_source_res->HasError()) {
            for (idx_t i = 0; i < parcel_source_res->RowCount(); ++i) {
                source_appender.BeginRow();
                source_appender.Append<const char*>("parcel_geometry");
                source_appender.Append<const char*>(parcel_source_res->GetValue(0, i).ToString().c_str());
                source_appender.Append<uint64_t>((uint64_t)parcel_source_res->GetValue<int64_t>(1, i));
                source_appender.EndRow();
            }
        }
        if (auto property_source_res = con.Query(
                "SELECT property_source_file, count(*) "
                "FROM unified_parcels WHERE property_source_file <> '' GROUP BY 1");
            property_source_res && !property_source_res->HasError()) {
            for (idx_t i = 0; i < property_source_res->RowCount(); ++i) {
                source_appender.BeginRow();
                source_appender.Append<const char*>("property_record");
                source_appender.Append<const char*>(property_source_res->GetValue(0, i).ToString().c_str());
                source_appender.Append<uint64_t>((uint64_t)property_source_res->GetValue<int64_t>(1, i));
                source_appender.EndRow();
            }
        }
        if (auto hydrated_res = con.Query(
                "SELECT layer_file, count(*)::BIGINT FROM layer_features GROUP BY layer_file");
            hydrated_res && !hydrated_res->HasError()) {
            for (idx_t i = 0; i < hydrated_res->RowCount(); ++i) {
                source_appender.BeginRow();
                source_appender.Append<const char*>("hydrated_layer");
                source_appender.Append<const char*>(hydrated_res->GetValue(0, i).ToString().c_str());
                source_appender.Append<uint64_t>((uint64_t)hydrated_res->GetValue<int64_t>(1, i));
                source_appender.EndRow();
            }
        }
        source_appender.Close();
    }

    {
        const std::vector<fs::path> manifest_paths = repositoryManifestPaths(root);
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
                    const fs::path local_path = manifestItemOutputPath(root, item);
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
            const fs::path stored_path = resolveStoredLayerPath(root, layer);
            std::error_code ec;
            const bool stored_exists = fs::exists(stored_path, ec) && !ec;
            fs::path source_artifact_path;
            if (!layer.import_artifact_file.empty()) {
                source_artifact_path = provenanceSourceArtifactPath(root, layer, layer.import_artifact_file);
            } else if (layer.import_type == "xlsx_point_table") {
                source_artifact_path = provenanceSourceArtifactPath(root, layer, layer.file + ".source.xlsx");
            } else if (layer.import_type == "zipped_shapefile") {
                source_artifact_path = provenanceSourceArtifactPath(root, layer, layer.file + ".source.zip");
            } else if (layer.import_type == "socrata_csv_properties") {
                source_artifact_path = provenanceSourceArtifactPath(root, layer, layer.file + ".source.csv");
            }
            ec.clear();
            const bool source_exists = !source_artifact_path.empty() && fs::exists(source_artifact_path, ec) && !ec;
            ec.clear();
            const uint64_t source_size_bytes = source_exists ? (uint64_t)fs::file_size(source_artifact_path, ec) : 0ULL;
            size_t missing_name_count = 0;
            size_t missing_lga_count = 0;
            for (const auto& fg : layer.features) {
                const std::string feature_name = firstDisplayProperty(
                    fg, {"name", "poi_name", "prmry_name", "set_name", "market_nam", "plc_st_nam", "fctry_st_n"});
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

    rewriteAnalyticsLayerState(con, current_states);

    exec_or_throw("DROP TABLE IF EXISTS geography_feature_collections", "drop geography_feature_collections");
    exec_or_throw("DROP TABLE IF EXISTS parcel_events", "drop parcel_events");
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
        const std::string expected_signature = analyticsBuildSignature(root_, layers);

        std::string sidecar_signature;
        if (loadAnalyticsSignatureSidecar(db_path, sidecar_signature)) {
            return sidecar_signature != expected_signature;
        }

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
        if (stored_signature == expected_signature) {
            saveAnalyticsSignatureSidecar(db_path, stored_signature);
            return false;
        }
        return true;
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

std::string DuckDbAnalytics::buildSourceSignature() const {
    try {
        std::error_code ec;
        const fs::path db_path = status_.db_path;
        if (!fs::exists(db_path, ec) || ec) return {};
        duckdb::DuckDB db(status_.db_path);
        duckdb::Connection con(db);
        auto res = con.Query("SELECT source_signature FROM analytics_build_info LIMIT 1");
        if (!res || res->HasError() || res->RowCount() == 0) return {};
        return res->GetValue(0, 0).ToString();
    } catch (...) {
        return {};
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

    if (stale_or_missing && validateExistingCache()) {
        try {
            duckdb::DuckDB db(status_.db_path);
            duckdb::Connection con(db);
            if (analyticsLayerStateTableReady(con)) {
                std::vector<AnalyticsLayerStateRow> stored_states;
                std::vector<AnalyticsLayerStateRow> current_states = analyticsCurrentLayerStates(root_, layers);
                if (loadStoredAnalyticsLayerStates(con, stored_states)) {
                    std::unordered_map<std::string, AnalyticsLayerStateRow> stored_by_file;
                    std::unordered_map<std::string, AnalyticsLayerStateRow> current_by_file;
                    for (const auto& state : stored_states) stored_by_file[state.layer_file] = state;
                    for (const auto& state : current_states) current_by_file[state.layer_file] = state;

                    std::unordered_set<std::string> changed_files;
                    for (const auto& [file, current] : current_by_file) {
                        auto it = stored_by_file.find(file);
                        if (it == stored_by_file.end() ||
                            it->second.layer_idx != current.layer_idx ||
                            it->second.source_signature != current.source_signature) {
                            changed_files.insert(file);
                        }
                    }
                    for (const auto& [file, stored] : stored_by_file) {
                        if (!current_by_file.contains(file)) changed_files.insert(file);
                    }

                    if (changed_files.empty()) {
                        saveAnalyticsSignatureSidecar(status_.db_path, analyticsBuildSignature(root_, layers));
                        result.ok = true;
                        result.reused_existing = true;
                        result.message = status_.message;
                        return result;
                    }

                    auto exec_or_throw = [&](const std::string& sql, const char* context) {
                        auto res = con.Query(sql);
                        if (!res || res->HasError()) {
                            throw std::runtime_error(
                                std::string(context) + ": " +
                                (res ? res->GetError() : std::string("query failed")));
                        }
                    };
                    auto quoted_file_list = [&]() {
                        std::ostringstream out;
                        bool first = true;
                        for (const auto& file : changed_files) {
                            if (!first) out << ", ";
                            first = false;
                            out << "'" << sqlQuote(file) << "'";
                        }
                        return out.str();
                    };

                    exec_or_throw("BEGIN TRANSACTION", "begin incremental analytics refresh");
                    const std::string in_list = quoted_file_list();
                    if (!in_list.empty()) {
                        exec_or_throw(
                            "DELETE FROM layer_features WHERE layer_file IN (" + in_list + ")",
                            "delete stale layer_features rows");
                        exec_or_throw(
                            "DELETE FROM layer_feature_properties WHERE layer_file IN (" + in_list + ")",
                            "delete stale layer_feature_properties rows");
                    }

                    size_t changed_feature_count = 0;
                    size_t changed_layer_count = 0;
                    appendAnalyticsLayerFeatures(
                        root_,
                        con,
                        layers,
                        &changed_files,
                        changed_feature_count,
                        changed_layer_count);
                    rewriteUnifiedParcelsTable(con, unified_parcels);

                    size_t total_feature_count = 0;
                    size_t total_layer_count = 0;
                    if (auto feature_count_res = con.Query("SELECT count(*)::BIGINT FROM layer_features");
                        feature_count_res && !feature_count_res->HasError() && feature_count_res->RowCount() > 0) {
                        total_feature_count = (size_t)feature_count_res->GetValue<int64_t>(0, 0);
                    }
                    if (auto layer_count_res = con.Query("SELECT count(DISTINCT layer_idx)::BIGINT FROM layer_features");
                        layer_count_res && !layer_count_res->HasError() && layer_count_res->RowCount() > 0) {
                        total_layer_count = (size_t)layer_count_res->GetValue<int64_t>(0, 0);
                    }
                    rebuildDerivedAnalyticsObjects(
                        root_,
                        con,
                        layers,
                        current_states,
                        total_layer_count,
                        total_feature_count);
                    exec_or_throw("COMMIT", "commit incremental analytics refresh");
                    saveAnalyticsSignatureSidecar(status_.db_path, analyticsBuildSignature(root_, layers));

                    status_.last_rebuild_ok = true;
                    status_.layer_count = total_layer_count;
                    status_.feature_count = total_feature_count;
                    std::ostringstream msg;
                    msg << "DuckDB analytics incrementally refreshed: "
                        << changed_files.size() << " changed layers, "
                        << total_feature_count << " total features across "
                        << total_layer_count << " hydrated layers.";
                    status_.message = msg.str();

                    result.ok = true;
                    result.incrementally_updated = true;
                    result.message = status_.message;
                    return result;
                }
            }
        } catch (const std::exception&) {
        }
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
            std::string parcel_entity_id;
            std::string parcel_geometry_entity_id;
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
            size_t real_property_local_feature_idx = 0;
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
        exec_or_throw("DROP TABLE IF EXISTS analytics_layer_state", "drop analytics_layer_state");
        exec_or_throw(R"SQL(
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
                entity_id VARCHAR,
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
                const std::string entity_id = featureEntityIdForLayerFeature(layer, fg, fi);
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
                appender.Append<const char*>(entity_id.c_str());
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
                    row.parcel_entity_id = featureEntityIdForLayerFeature(layer, fg, fi);
                    row.parcel_geometry_entity_id = featureGeometryEntityIdForLayerFeature(layer, fg, fi);
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
                    row.real_property_local_feature_idx = fi;
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
                        (row.priority == it->second.priority &&
                         row.real_property_local_feature_idx < it->second.real_property_local_feature_idx)) {
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
        )SQL", "create unified_parcels");
        {
            auto parcel_appender = duckdb::Appender(con, "unified_parcels");
            for (const auto& parcel : parcel_rows) {
                const PropertyRecordRow* property = nullptr;
                if (!parcel.blocklot.empty()) {
                    auto it = property_by_blocklot.find(parcel.blocklot);
                    if (it != property_by_blocklot.end() &&
                        it->second.layer_file != parcel.parcel_layer_file) {
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
                parcel_appender.Append<const char*>(parcel.parcel_entity_id.c_str());
                parcel_appender.Append<const char*>(parcel.parcel_geometry_entity_id.c_str());
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
        exec_or_throw("CREATE INDEX IF NOT EXISTS idx_layer_features_entity_id ON layer_features(layer_file, entity_id)", "index layer_features entity_id");
        exec_or_throw("CREATE INDEX IF NOT EXISTS idx_layer_features_owner ON layer_features(owner)", "index layer_features owner");
        exec_or_throw("CREATE INDEX IF NOT EXISTS idx_layer_features_layer_file ON layer_features(layer_file)", "index layer_features layer_file");
        exec_or_throw("CREATE INDEX IF NOT EXISTS idx_layer_features_geography ON layer_features(provenance_nation_state, provenance_state_region)", "index layer_features geography");
        exec_or_throw("CREATE INDEX IF NOT EXISTS idx_unified_parcels_blocklot ON unified_parcels(blocklot)", "index unified_parcels blocklot");
        exec_or_throw("CREATE INDEX IF NOT EXISTS idx_unified_parcels_entity_id ON unified_parcels(parcel_entity_id)", "index unified_parcels entity id");
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
            CREATE TABLE analytics_layer_state (
                layer_idx UBIGINT,
                layer_file VARCHAR,
                source_signature VARCHAR
            )
        )SQL", "create analytics_layer_state");
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
        rewriteAnalyticsLayerState(con, analyticsCurrentLayerStates(root_, layers));
        exec_or_throw("COMMIT", "commit analytics rebuild");
        saveAnalyticsSignatureSidecar(status_.db_path, analyticsBuildSignature(root_, layers));

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
        con.Query("CREATE TEMP TABLE ui_selected_parcels(layer_idx UBIGINT, entity_id VARCHAR, blocklot VARCHAR)");
        for (const auto& parcel : selected_parcels) {
            con.Query(
                "INSERT INTO ui_selected_parcels VALUES (" +
                std::to_string((uint64_t)parcel.layer_idx) + ", " +
                "'" + sqlQuote(parcel.entity_id) + "', '" +
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
        const int entity_id_col = find_col({"entity_id"});
        const int blocklot_col = find_col({"blocklot", "block_lot"});
        const int owner_col = find_col({"owner", "owner_name"});
        std::unordered_map<std::string, size_t> feature_idx_cache;
        auto resolve_feature_idx = [&](size_t layer_idx, const std::string& entity_id) -> std::optional<size_t> {
            const std::string key = std::to_string((uint64_t)layer_idx) + "\n" + normalizeJoinKey(entity_id);
            if (key.empty()) return std::nullopt;
            if (auto it = feature_idx_cache.find(key); it != feature_idx_cache.end()) return it->second;
            const std::string lookup_sql =
                "SELECT feature_idx FROM layer_features WHERE layer_idx = " +
                std::to_string((uint64_t)layer_idx) +
                " AND entity_id = '" + sqlQuote(normalizeJoinKey(entity_id)) +
                "' LIMIT 1";
            auto lookup = con.Query(lookup_sql);
            if (!lookup || lookup->HasError()) return std::nullopt;
            if (auto chunk = lookup->Fetch()) {
                if (chunk->size() > 0) {
                    const size_t feature_idx = (size_t)chunk->GetValue(0, 0).GetValue<uint64_t>();
                    feature_idx_cache.emplace(key, feature_idx);
                    return feature_idx;
                }
            }
            return std::nullopt;
        };

        out.result_set.active = true;
        size_t scanned_rows = 0;
        while (auto chunk = result->Fetch()) {
            for (idx_t row = 0; row < chunk->size(); ++row) {
                std::vector<std::string> display_row;
                display_row.reserve(out.columns.size());
                for (idx_t col = 0; col < chunk->ColumnCount(); ++col) {
                    display_row.push_back(chunk->GetValue(col, row).ToString());
                }

                if (layer_col >= 0 &&
                    (size_t)layer_col < display_row.size() &&
                    entity_id_col >= 0 &&
                    (size_t)entity_id_col < display_row.size()) {
                    try {
                        const uint64_t layer_idx = std::stoull(display_row[(size_t)layer_col]);
                        if (const auto feature_idx = resolve_feature_idx((size_t)layer_idx, display_row[(size_t)entity_id_col])) {
                            out.result_set.layers.insert((size_t)layer_idx);
                            out.result_set.features.insert(FeatureKey{(size_t)layer_idx, *feature_idx});
                        }
                    } catch (...) {
                    }
                } else if (layer_col >= 0 && feature_col >= 0 &&
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
        SELECT pf.layer_idx, pf.entity_id, pf.blocklot
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

DuckDbQueryResult DuckDbAnalytics::queryUnifiedParcelDetail(const std::string& parcel_entity_id) const {
    const std::string key = normalizeJoinKey(parcel_entity_id);
    if (key.empty()) {
        DuckDbQueryResult out;
        out.ok = false;
        out.message = "Parcel entity ID is empty";
        return out;
    }
    std::ostringstream sql;
    sql << R"SQL(
        SELECT *
        FROM (
            SELECT
                0 AS detail_priority,
                parcel_layer_idx,
                parcel_entity_id,
                parcel_geometry_entity_id,
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
            WHERE parcel_entity_id = ')SQL" << sqlQuote(key) << R"SQL('
            UNION ALL
            SELECT
                1 AS detail_priority,
                layer_idx AS parcel_layer_idx,
                entity_id AS parcel_entity_id,
                '' AS parcel_geometry_entity_id,
                blocklot,
                layer_file AS parcel_source_file,
                '' AS property_source_file,
                true AS parcel_has_geometry,
                false AS has_property_record,
                owner,
                owner AS owner_display,
                address,
                zipcode,
                status,
                0.0 AS current_land,
                0.0 AS current_improvements,
                structure_area_sqft,
                value_usd AS tax_base,
                0.0 AS sale_price,
                value_usd AS current_value,
                0 AS vacant_notice_count,
                0 AS vacant_rehab_count,
                0 AS tax_lien_count,
                0 AS tax_sale_count,
                0.0 AS tax_lien_amount,
                0.0 AS tax_sale_amount,
                min_lon,
                min_lat,
                max_lon,
                max_lat
            FROM layer_features
            WHERE entity_id = ')SQL" << sqlQuote(key) << R"SQL('
              AND scale = 'parcel'
              AND duckdb_role = 'parcel_record'
        ) detail
        ORDER BY detail_priority ASC
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
            SELECT parcel_layer_idx, parcel_entity_id AS entity_id, blocklot, owner, address, current_value
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
                hit.entity_id = chunk->GetValue(1, row).ToString();
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

DuckDbParcelSemanticSnapshot DuckDbAnalytics::loadParcelSemanticSnapshot(size_t parcel_layer_idx) const {
    DuckDbParcelSemanticSnapshot out;
    if (!status_.last_rebuild_ok) {
        out.message = "DuckDB analytics cache is not ready";
        return out;
    }
    try {
        duckdb::DuckDB db(status_.db_path);
        duckdb::Connection con(db);

        auto signature_res = con.Query("SELECT source_signature FROM analytics_build_info LIMIT 1");
        if (!signature_res || signature_res->HasError() || signature_res->RowCount() == 0) {
            out.message = signature_res ? signature_res->GetError() : "missing analytics_build_info";
            return out;
        }
        out.source_signature = signature_res->GetValue(0, 0).ToString();

        std::ostringstream sql;
        sql << R"SQL(
            WITH ranked_unified AS (
                SELECT
                    parcel_layer_idx,
                    parcel_entity_id,
                    parcel_geometry_entity_id,
                    blocklot,
                    parcel_source_file,
                    property_source_file,
                    parcel_has_geometry,
                    has_property_record,
                    owner,
                    owner_display,
                    address,
                    address_search,
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
                    max_lat,
                    ROW_NUMBER() OVER (
                        PARTITION BY parcel_layer_idx, parcel_entity_id
                        ORDER BY blocklot, parcel_source_file, property_source_file
                    ) AS rn
                FROM unified_parcels
                WHERE parcel_layer_idx = )SQL" << parcel_layer_idx << R"SQL(
            )
            SELECT
                lf.feature_idx,
                lf.entity_id,
                COALESCE(ru.parcel_geometry_entity_id, '') AS parcel_geometry_entity_id,
                COALESCE(ru.blocklot, lf.blocklot, '') AS blocklot,
                COALESCE(ru.parcel_source_file, '') AS parcel_source_file,
                COALESCE(ru.property_source_file, '') AS property_source_file,
                COALESCE(ru.parcel_has_geometry, TRUE) AS parcel_has_geometry,
                COALESCE(ru.has_property_record, FALSE) AS has_property_record,
                COALESCE(ru.owner, lf.owner, '') AS owner,
                COALESCE(ru.owner_display, lf.owner, '') AS owner_display,
                COALESCE(ru.address, lf.address, '') AS address,
                COALESCE(ru.address_search, '') AS address_search,
                COALESCE(ru.zipcode, lf.zipcode, '') AS zipcode,
                COALESCE(ru.status, lf.status, '') AS status,
                COALESCE(ru.current_land, 0.0) AS current_land,
                COALESCE(ru.current_improvements, 0.0) AS current_improvements,
                COALESCE(ru.structure_area_sqft, lf.structure_area_sqft, 0.0) AS structure_area_sqft,
                COALESCE(ru.tax_base, 0.0) AS tax_base,
                COALESCE(ru.sale_price, 0.0) AS sale_price,
                COALESCE(ru.current_value, lf.value_usd, 0.0) AS current_value,
                COALESCE(ru.vacant_notice_count, 0) AS vacant_notice_count,
                COALESCE(ru.vacant_rehab_count, 0) AS vacant_rehab_count,
                COALESCE(ru.tax_lien_count, 0) AS tax_lien_count,
                COALESCE(ru.tax_sale_count, 0) AS tax_sale_count,
                COALESCE(ru.tax_lien_amount, 0.0) AS tax_lien_amount,
                COALESCE(ru.tax_sale_amount, 0.0) AS tax_sale_amount,
                COALESCE(ru.min_lon, lf.min_lon) AS min_lon,
                COALESCE(ru.min_lat, lf.min_lat) AS min_lat,
                COALESCE(ru.max_lon, lf.max_lon) AS max_lon,
                COALESCE(ru.max_lat, lf.max_lat) AS max_lat
            FROM layer_features lf
            LEFT JOIN ranked_unified ru
              ON ru.rn = 1
             AND ru.parcel_layer_idx = lf.layer_idx
             AND ru.parcel_entity_id = lf.entity_id
            WHERE lf.layer_idx = )SQL" << parcel_layer_idx << R"SQL(
            ORDER BY lf.feature_idx
        )SQL";

        auto result = con.Query(sql.str());
        if (!result || result->HasError()) {
            out.message = result ? result->GetError() : "parcel semantic snapshot query failed";
            return out;
        }

        while (auto chunk = result->Fetch()) {
            for (idx_t row = 0; row < chunk->size(); ++row) {
                UnifiedParcelRecord record;
                record.parcel_layer_idx = parcel_layer_idx;
                record.parcel_local_feature_idx = (size_t)chunk->GetValue(0, row).GetValue<uint64_t>();
                record.parcel_entity_id = chunk->GetValue(1, row).ToString();
                record.parcel_geometry_entity_id = chunk->GetValue(2, row).ToString();
                record.blocklot = chunk->GetValue(3, row).ToString();
                record.parcel_source_file = chunk->GetValue(4, row).ToString();
                record.property_source_file = chunk->GetValue(5, row).ToString();
                record.parcel_has_geometry = chunk->GetValue(6, row).GetValue<bool>();
                record.has_property_record = chunk->GetValue(7, row).GetValue<bool>();
                record.owner = chunk->GetValue(8, row).ToString();
                record.owner_display = chunk->GetValue(9, row).ToString();
                record.address = chunk->GetValue(10, row).ToString();
                record.address_search = chunk->GetValue(11, row).ToString();
                record.zip = chunk->GetValue(12, row).ToString();
                record.status = chunk->GetValue(13, row).ToString();
                record.current_land = chunk->GetValue(14, row).GetValue<double>();
                record.current_improvements = chunk->GetValue(15, row).GetValue<double>();
                record.structure_area_sqft = chunk->GetValue(16, row).GetValue<double>();
                record.tax_base = chunk->GetValue(17, row).GetValue<double>();
                record.sale_price = chunk->GetValue(18, row).GetValue<double>();
                record.current_value = chunk->GetValue(19, row).GetValue<double>();
                record.vacant_notice_count = chunk->GetValue(20, row).GetValue<int32_t>();
                record.vacant_rehab_count = chunk->GetValue(21, row).GetValue<int32_t>();
                record.tax_lien_count = chunk->GetValue(22, row).GetValue<int32_t>();
                record.tax_sale_count = chunk->GetValue(23, row).GetValue<int32_t>();
                record.tax_lien_amount = chunk->GetValue(24, row).GetValue<double>();
                record.tax_sale_amount = chunk->GetValue(25, row).GetValue<double>();
                record.parcel_extent.min_lon = (float)chunk->GetValue(26, row).GetValue<double>();
                record.parcel_extent.min_lat = (float)chunk->GetValue(27, row).GetValue<double>();
                record.parcel_extent.max_lon = (float)chunk->GetValue(28, row).GetValue<double>();
                record.parcel_extent.max_lat = (float)chunk->GetValue(29, row).GetValue<double>();
                if (record.owner_display.empty()) record.owner_display = record.owner;
                if (record.address_search.empty()) record.address_search = normalizeAddressSearchText(record.address);
                record.owner_search = toLowerAscii(trimDisplayValue(record.owner));

                out.parcel_blocklot_by_feature.push_back(record.blocklot);
                out.parcel_owner_search_by_feature.push_back(record.owner_search);
                out.parcel_address_search_by_feature.push_back(record.address_search);
                out.parcel_vac_notice_by_feature.push_back(record.vacant_notice_count);
                out.parcel_vac_rehab_by_feature.push_back(record.vacant_rehab_count);
                out.parcel_tax_lien_by_feature.push_back(record.tax_lien_count);
                out.parcel_tax_sale_by_feature.push_back(record.tax_sale_count);
                out.parcel_tax_lien_amount_by_feature.push_back(record.tax_lien_amount);
                out.parcel_tax_sale_amount_by_feature.push_back(record.tax_sale_amount);
                out.unified_parcels.push_back(std::move(record));
            }
        }

        out.ok = true;
    } catch (const std::exception& ex) {
        out.message = ex.what();
    }
    return out;
}
