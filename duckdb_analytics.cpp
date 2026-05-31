#include "duckdb_analytics.h"

#include "app_utils.h"
#include "cache_io.h"
#include "feature_props.h"

#include <duckdb.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <unordered_map>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
constexpr int kAnalyticsSchemaVersion = 13;
constexpr size_t kMaxEventDetailChars = 4096;
constexpr size_t kMaxEventMetadataChars = 8192;

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

std::string queryResultCell(
    const DuckDbQueryResult& result,
    const std::vector<std::string>& row,
    const char* column) {
    for (size_t i = 0; i < result.columns.size() && i < row.size(); ++i) {
        if (result.columns[i] == column) return row[i];
    }
    return {};
}

double queryResultDouble(
    const DuckDbQueryResult& result,
    const std::vector<std::string>& row,
    const char* column) {
    const std::string value = trimDisplayValue(queryResultCell(result, row, column));
    if (value.empty() || value == "NULL") return 0.0;
    char* end = nullptr;
    const double parsed = std::strtod(value.c_str(), &end);
    return end != value.c_str() && std::isfinite(parsed) ? parsed : 0.0;
}

int queryResultInt(
    const DuckDbQueryResult& result,
    const std::vector<std::string>& row,
    const char* column) {
    return static_cast<int>(std::lround(queryResultDouble(result, row, column)));
}

size_t queryResultSize(
    const DuckDbQueryResult& result,
    const std::vector<std::string>& row,
    const char* column) {
    const std::string value = trimDisplayValue(queryResultCell(result, row, column));
    if (value.empty() || value == "NULL") return 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    return end != value.c_str() ? static_cast<size_t>(parsed) : 0;
}

bool queryResultBool(
    const DuckDbQueryResult& result,
    const std::vector<std::string>& row,
    const char* column) {
    const std::string value = normalizeJoinKey(queryResultCell(result, row, column));
    return value == "1" || value == "TRUE" || value == "T" || value == "YES";
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

std::string trimFirstPropertyValue(
    const LayerDef& layer,
    size_t feature_idx,
    std::initializer_list<const char*> keys) {
    return trimDisplayValue(getFirstPropertyValue(layer, feature_idx, keys));
}

void appendDetailLine(std::string& out, const char* label, const std::string& value) {
    const std::string trimmed = trimDisplayValue(value);
    if (trimmed.empty()) return;
    if (!out.empty()) out += '\n';
    out += label;
    out += ": ";
    out += trimmed;
}

void appendMetadataValue(json& out, const char* key, const std::string& value) {
    const std::string trimmed = trimDisplayValue(value);
    if (!trimmed.empty()) out[key] = trimmed;
}

std::string truncateUtf8Safe(std::string value, size_t max_chars) {
    if (value.size() <= max_chars) return value;
    value.resize(max_chars);
    return value;
}

std::string eventTitleHintForFeature(const LayerDef& layer, size_t feature_idx) {
    const std::string explicit_title = trimFirstPropertyValue(layer, feature_idx, {
        "Description", "DESCRIPTION", "description", "desc", "DESC",
        "Project_Title", "ProjectName", "Project_Name", "Name", "name",
        "ViolationType", "Violation_Type", "Type", "TYPE", "CaseType", "CASE_TYPE"
    });
    if (!explicit_title.empty()) return explicit_title;

    const std::string permit_type = trimFirstPropertyValue(layer, feature_idx, {
        "PermitType", "PERMITTYPE", "Permit_Type", "RecordType", "RECORD_TYPE"
    });
    const std::string permit_number = trimFirstPropertyValue(layer, feature_idx, {
        "PermitNum", "PermitNumber", "PERMITNUM", "PERMIT_NUMBER", "CaseNumber", "CASE_NUMBER", "CASE_NUM"
    });
    if (!permit_type.empty() && !permit_number.empty()) return permit_type + " " + permit_number;
    if (!permit_type.empty()) return permit_type;
    if (!permit_number.empty()) return permit_number;

    return trimFirstPropertyValue(layer, feature_idx, {
        "name", "poi_name", "prmry_name", "set_name", "market_nam", "plc_st_nam", "fctry_st_n"
    });
}

std::string eventDetailHintForFeature(const LayerDef& layer, size_t feature_idx) {
    std::string detail;
    appendDetailLine(detail, "Description", trimFirstPropertyValue(layer, feature_idx, {
        "Description", "DESCRIPTION", "description", "desc", "DESC"
    }));
    appendDetailLine(detail, "Existing Use", trimFirstPropertyValue(layer, feature_idx, {
        "ExistingUse", "EXISTINGUSE", "Existing_Use"
    }));
    appendDetailLine(detail, "Proposed Use", trimFirstPropertyValue(layer, feature_idx, {
        "ProposedUse", "PROPOSEDUSE", "Proposed_Use"
    }));
    appendDetailLine(detail, "Permit Type", trimFirstPropertyValue(layer, feature_idx, {
        "PermitType", "PERMITTYPE", "Permit_Type", "RecordType", "RECORD_TYPE"
    }));
    appendDetailLine(detail, "Permit Number", trimFirstPropertyValue(layer, feature_idx, {
        "PermitNum", "PermitNumber", "PERMITNUM", "PERMIT_NUMBER", "CaseNumber", "CASE_NUMBER", "CASE_NUM"
    }));
    appendDetailLine(detail, "Neighborhood", trimFirstPropertyValue(layer, feature_idx, {
        "Neighborhood", "NEIGHBORHOOD"
    }));
    appendDetailLine(detail, "Applicant", trimFirstPropertyValue(layer, feature_idx, {
        "Applicant", "APPLICANT", "ApplicantName"
    }));
    appendDetailLine(detail, "Contractor", trimFirstPropertyValue(layer, feature_idx, {
        "Contractor", "CONTRACTOR", "ContractorName"
    }));
    appendDetailLine(detail, "Disposition", trimFirstPropertyValue(layer, feature_idx, {
        "Disposition", "DISPOSITION", "Result", "RESULT"
    }));
    return truncateUtf8Safe(detail, kMaxEventDetailChars);
}

std::string eventMetadataJsonForFeature(const LayerDef& layer, size_t feature_idx) {
    json metadata = json::object();
    appendMetadataValue(metadata, "description", trimFirstPropertyValue(layer, feature_idx, {
        "Description", "DESCRIPTION", "description", "desc", "DESC"
    }));
    appendMetadataValue(metadata, "existing_use", trimFirstPropertyValue(layer, feature_idx, {
        "ExistingUse", "EXISTINGUSE", "Existing_Use"
    }));
    appendMetadataValue(metadata, "proposed_use", trimFirstPropertyValue(layer, feature_idx, {
        "ProposedUse", "PROPOSEDUSE", "Proposed_Use"
    }));
    appendMetadataValue(metadata, "permit_type", trimFirstPropertyValue(layer, feature_idx, {
        "PermitType", "PERMITTYPE", "Permit_Type", "RecordType", "RECORD_TYPE"
    }));
    appendMetadataValue(metadata, "permit_number", trimFirstPropertyValue(layer, feature_idx, {
        "PermitNum", "PermitNumber", "PERMITNUM", "PERMIT_NUMBER", "CaseNumber", "CASE_NUMBER", "CASE_NUM"
    }));
    appendMetadataValue(metadata, "project_title", trimFirstPropertyValue(layer, feature_idx, {
        "Project_Title", "ProjectName", "Project_Name", "Name", "name"
    }));
    appendMetadataValue(metadata, "applicant", trimFirstPropertyValue(layer, feature_idx, {
        "Applicant", "APPLICANT", "ApplicantName"
    }));
    appendMetadataValue(metadata, "contractor", trimFirstPropertyValue(layer, feature_idx, {
        "Contractor", "CONTRACTOR", "ContractorName"
    }));
    appendMetadataValue(metadata, "neighborhood", trimFirstPropertyValue(layer, feature_idx, {
        "Neighborhood", "NEIGHBORHOOD"
    }));
    appendMetadataValue(metadata, "disposition", trimFirstPropertyValue(layer, feature_idx, {
        "Disposition", "DISPOSITION", "Result", "RESULT"
    }));
    appendMetadataValue(metadata, "source_address", trimFirstPropertyValue(layer, feature_idx, {
        "FULLADDR", "FULL_ADDRESS", "PROPERTY_ADDRESS", "PROPERTYADDR", "PREMISEADD",
        "PREMISE_ADDRESS", "ADDRESS", "Address", "ADDR", "ADDR1", "ADDRESS1",
        "SITE_ADDR", "SITUSADDR", "LOCATION", "Location"
    }));
    if (metadata.empty()) return {};
    return truncateUtf8Safe(metadata.dump(), kMaxEventMetadataChars);
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
        appender.Append<const char*>("");
        appender.Append<const char*>("");
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
                "event_date", "DateNotice", "DateIssue", "DateIssued", "IssuedDate",
                "Issue_Date_ISO", "Issue_Date", "DateFiled", "DateAuction",
                "DateDemoFinished", "ReleasedToContractor", "SALEDATE", "DATE",
                "CREATED_DATE", "RECORD_DATE"
            });
            const std::string event_status_hint = firstDisplayProperty(fg, {"CASE_STATUS", "STATUS", "STATE"});
            const std::string event_title_hint = eventTitleHintForFeature(layer, fi);
            const std::string event_detail_hint = eventDetailHintForFeature(layer, fi);
            const std::string event_metadata_json = eventMetadataJsonForFeature(layer, fi);
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
            appender.Append<const char*>(event_title_hint.c_str());
            appender.Append<const char*>(event_detail_hint.c_str());
            appender.Append<const char*>(event_metadata_json.c_str());
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

void rebuildSpatialSearchArtifacts(duckdb::Connection& con) {
    auto exec_or_throw = [&](const std::string& sql, const char* context) {
        auto res = con.Query(sql);
        if (!res || res->HasError()) {
            throw std::runtime_error(
                std::string(context) + ": " + (res ? res->GetError() : std::string("query failed")));
        }
    };

    exec_or_throw("DROP TABLE IF EXISTS layer_feature_bboxes", "drop layer_feature_bboxes");
    exec_or_throw("DROP TABLE IF EXISTS layer_bboxes", "drop layer_bboxes");
    exec_or_throw(R"SQL(
        CREATE TABLE layer_feature_bboxes AS
        SELECT
            layer_idx,
            layer_file,
            layer_name,
            duckdb_role,
            feature_idx,
            entity_id,
            scale,
            category,
            provenance_world,
            provenance_nation_state,
            provenance_state_region,
            provenance_county_city,
            min_lon,
            min_lat,
            max_lon,
            max_lat,
            (min_lon + max_lon) / 2.0 AS center_lon,
            (min_lat + max_lat) / 2.0 AS center_lat
        FROM layer_features
        WHERE min_lon < max_lon
          AND min_lat < max_lat
    )SQL", "create layer_feature_bboxes");
    exec_or_throw(R"SQL(
        CREATE TABLE layer_bboxes AS
        SELECT
            layer_idx,
            layer_file,
            layer_name,
            duckdb_role,
            scale,
            category,
            provenance_world,
            provenance_nation_state,
            provenance_state_region,
            provenance_county_city,
            count(*) AS feature_count,
            min(min_lon) AS min_lon,
            min(min_lat) AS min_lat,
            max(max_lon) AS max_lon,
            max(max_lat) AS max_lat
        FROM layer_feature_bboxes
        GROUP BY
            layer_idx,
            layer_file,
            layer_name,
            duckdb_role,
            scale,
            category,
            provenance_world,
            provenance_nation_state,
            provenance_state_region,
            provenance_county_city
    )SQL", "create layer_bboxes");
    exec_or_throw("CREATE INDEX IF NOT EXISTS idx_layer_feature_bboxes_layer_feature ON layer_feature_bboxes(layer_idx, feature_idx)", "index layer_feature_bboxes layer feature");
    exec_or_throw("CREATE INDEX IF NOT EXISTS idx_layer_feature_bboxes_entity ON layer_feature_bboxes(layer_idx, entity_id)", "index layer_feature_bboxes entity");
    exec_or_throw("CREATE INDEX IF NOT EXISTS idx_layer_feature_bboxes_extent ON layer_feature_bboxes(min_lon, min_lat, max_lon, max_lat)", "index layer_feature_bboxes extent");
    exec_or_throw("CREATE INDEX IF NOT EXISTS idx_layer_bboxes_layer ON layer_bboxes(layer_idx)", "index layer_bboxes layer");

    exec_or_throw("DROP TABLE IF EXISTS parcel_zone_memberships", "drop parcel_zone_memberships");
    exec_or_throw(R"SQL(
        CREATE TABLE parcel_zone_memberships (
            parcel_layer_idx UBIGINT,
            parcel_entity_id VARCHAR,
            parcel_geometry_entity_id VARCHAR,
            blocklot VARCHAR,
            zone_layer_idx UBIGINT,
            zone_layer_file VARCHAR,
            zone_layer_name VARCHAR,
            zone_feature_idx UBIGINT,
            zone_entity_id VARCHAR,
            zone_key VARCHAR,
            zone_label VARCHAR,
            relation VARCHAR,
            parcel_centroid_lon DOUBLE,
            parcel_centroid_lat DOUBLE,
            overlap_area DOUBLE,
            overlap_ratio DOUBLE,
            source_signature VARCHAR
        )
    )SQL", "create parcel_zone_memberships");
    exec_or_throw(
        "CREATE INDEX IF NOT EXISTS idx_parcel_zone_memberships_parcel "
        "ON parcel_zone_memberships(parcel_layer_idx, parcel_entity_id)",
        "index parcel_zone_memberships parcel");
    exec_or_throw(
        "CREATE INDEX IF NOT EXISTS idx_parcel_zone_memberships_zone "
        "ON parcel_zone_memberships(zone_layer_idx, zone_entity_id)",
        "index parcel_zone_memberships zone");
    exec_or_throw(R"SQL(
        CREATE OR REPLACE VIEW parcel_zone_membership_summary AS
        SELECT
            parcel_layer_idx,
            parcel_entity_id,
            count(*) AS zone_membership_count,
            count(DISTINCT zone_layer_idx) AS zone_layer_count
        FROM parcel_zone_memberships
        GROUP BY parcel_layer_idx, parcel_entity_id
    )SQL", "create parcel_zone_membership_summary view");
}

void refreshUnifiedParcelEventRollups(duckdb::Connection& con) {
    auto exec_or_throw = [&](const std::string& sql, const char* context) {
        auto res = con.Query(sql);
        if (!res || res->HasError()) {
            throw std::runtime_error(
                std::string(context) + ": " + (res ? res->GetError() : std::string("query failed")));
        }
    };
    exec_or_throw(R"SQL(
        UPDATE unified_parcels
        SET
            vacant_notice_count = COALESCE((
                SELECT count(*)::INTEGER
                FROM parcel_events pe
                WHERE pe.blocklot = unified_parcels.blocklot
                  AND pe.event_type = 'vacant_notice'
            ), 0),
            vacant_rehab_count = COALESCE((
                SELECT count(*)::INTEGER
                FROM parcel_events pe
                WHERE pe.blocklot = unified_parcels.blocklot
                  AND pe.event_type = 'vacant_rehab'
            ), 0),
            tax_lien_count = COALESCE((
                SELECT count(*)::INTEGER
                FROM parcel_events pe
                WHERE pe.blocklot = unified_parcels.blocklot
                  AND pe.event_type = 'tax_lien'
            ), 0),
            tax_sale_count = COALESCE((
                SELECT count(*)::INTEGER
                FROM parcel_events pe
                WHERE pe.blocklot = unified_parcels.blocklot
                  AND pe.event_type = 'tax_sale'
            ), 0),
            tax_lien_amount = COALESCE((
                SELECT sum(COALESCE(pe.amount_usd, 0.0))
                FROM parcel_events pe
                WHERE pe.blocklot = unified_parcels.blocklot
                  AND pe.event_type = 'tax_lien'
            ), 0.0),
            tax_sale_amount = COALESCE((
                SELECT sum(COALESCE(pe.amount_usd, 0.0))
                FROM parcel_events pe
                WHERE pe.blocklot = unified_parcels.blocklot
                  AND pe.event_type = 'tax_sale'
            ), 0.0)
    )SQL", "refresh unified parcel event rollups");
}

void rebuildParcelRelationshipArtifacts(duckdb::Connection& con) {
    auto exec_or_throw = [&](const std::string& sql, const char* context) {
        auto res = con.Query(sql);
        if (!res || res->HasError()) {
            throw std::runtime_error(
                std::string(context) + ": " + (res ? res->GetError() : std::string("query failed")));
        }
    };

    exec_or_throw("DROP TABLE IF EXISTS parcel_relationships", "drop parcel_relationships");
    exec_or_throw("DROP TABLE IF EXISTS parcel_related_features", "drop parcel_related_features");
    exec_or_throw("DROP TABLE IF EXISTS parcel_related_events", "drop parcel_related_events");
    exec_or_throw("DROP TABLE IF EXISTS parcel_property_records", "drop parcel_property_records");

    exec_or_throw(R"SQL(
        CREATE TABLE parcel_property_records AS
        WITH valid_blocklots AS (
            SELECT up.blocklot
            FROM unified_parcels up
            JOIN layer_features lf
              ON lf.blocklot = up.blocklot
            WHERE up.blocklot IS NOT NULL AND up.blocklot <> ''
              AND up.blocklot NOT IN ('ROW', 'UNK', 'NOID', 'UNKNOWN', 'NOTLOCATED', 'CONDO', 'MEDIAN', 'PRIVATERW')
              AND lf.duckdb_role = 'parcel_record'
            GROUP BY up.blocklot
            HAVING count(DISTINCT up.parcel_entity_id) <= 25
               AND count(DISTINCT lf.entity_id) <= 50
        )
        SELECT
            up.parcel_layer_idx,
            up.parcel_entity_id,
            up.parcel_geometry_entity_id,
            up.blocklot,
            lf.layer_idx AS property_layer_idx,
            lf.entity_id AS property_entity_id,
            lf.feature_idx AS property_feature_idx,
            lf.layer_file AS property_layer_file,
            lf.layer_name AS property_layer_name,
            lf.owner,
            lf.address,
            lf.zipcode,
            lf.status,
            lf.value_usd,
            lf.structure_area_sqft,
            lf.min_lon,
            lf.min_lat,
            lf.max_lon,
            lf.max_lat,
            'blocklot_exact' AS match_method,
            1.0::DOUBLE AS confidence
        FROM unified_parcels up
        JOIN valid_blocklots vb
          ON vb.blocklot = up.blocklot
        JOIN layer_features lf
          ON lf.blocklot = up.blocklot
        WHERE lf.duckdb_role = 'parcel_record'
          AND NOT (lf.layer_idx = up.parcel_layer_idx AND lf.entity_id = up.parcel_entity_id)
    )SQL", "create parcel_property_records");

    exec_or_throw(R"SQL(
        CREATE TABLE parcel_related_events AS
        WITH valid_blocklots AS (
            SELECT up.blocklot
            FROM unified_parcels up
            JOIN parcel_events pe
              ON pe.blocklot = up.blocklot
            WHERE up.blocklot IS NOT NULL AND up.blocklot <> ''
              AND up.blocklot NOT IN ('ROW', 'UNK', 'NOID', 'UNKNOWN', 'NOTLOCATED', 'CONDO', 'MEDIAN', 'PRIVATERW')
            GROUP BY up.blocklot
            HAVING count(DISTINCT up.parcel_entity_id) <= 25
               AND count(*) <= 500
        )
        SELECT
            up.parcel_layer_idx,
            up.parcel_entity_id,
            up.parcel_geometry_entity_id,
            up.blocklot,
            pe.event_id,
            pe.event_type,
            pe.event_label,
            pe.event_status,
            pe.event_date,
            pe.event_year,
            pe.event_title,
            pe.event_detail,
            pe.amount_usd,
            pe.source_layer_file,
            pe.source_layer_name,
            pe.source_feature_idx,
            'blocklot_exact' AS match_method,
            1.0::DOUBLE AS confidence
        FROM unified_parcels up
        JOIN valid_blocklots vb
          ON vb.blocklot = up.blocklot
        JOIN parcel_events pe
          ON pe.blocklot = up.blocklot
    )SQL", "create parcel_related_events");

    exec_or_throw(R"SQL(
        CREATE TABLE parcel_related_features AS
        WITH valid_blocklots AS (
            SELECT up.blocklot
            FROM unified_parcels up
            JOIN layer_features lf
              ON lf.blocklot = up.blocklot
            WHERE up.blocklot IS NOT NULL AND up.blocklot <> ''
              AND up.blocklot NOT IN ('ROW', 'UNK', 'NOID', 'UNKNOWN', 'NOTLOCATED', 'CONDO', 'MEDIAN', 'PRIVATERW')
              AND lf.duckdb_role NOT IN ('parcel_record', 'parcel_event')
            GROUP BY up.blocklot
            HAVING count(DISTINCT up.parcel_entity_id) <= 25
               AND count(DISTINCT lf.entity_id) <= 100
        )
        SELECT
            up.parcel_layer_idx,
            up.parcel_entity_id,
            up.parcel_geometry_entity_id,
            up.blocklot,
            lf.layer_idx AS related_layer_idx,
            lf.entity_id AS related_entity_id,
            lf.feature_idx AS related_feature_idx,
            lf.layer_file AS related_layer_file,
            lf.layer_name AS related_layer_name,
            lf.duckdb_role AS relation_type,
            lf.category,
            lf.owner,
            lf.address,
            lf.zipcode,
            lf.status,
            lf.value_usd,
            lf.feature_name,
            lf.event_date_text,
            lf.event_status_hint,
            lf.event_title_hint,
            lf.event_detail_hint,
            lf.amount_usd_hint,
            lf.min_lon,
            lf.min_lat,
            lf.max_lon,
            lf.max_lat,
            'blocklot_exact' AS match_method,
            1.0::DOUBLE AS confidence
        FROM unified_parcels up
        JOIN valid_blocklots vb
          ON vb.blocklot = up.blocklot
        JOIN layer_features lf
          ON lf.blocklot = up.blocklot
        WHERE lf.duckdb_role NOT IN ('parcel_record', 'parcel_event')
    )SQL", "create parcel_related_features");

    exec_or_throw(R"SQL(
        CREATE TABLE parcel_relationships AS
        SELECT
            'property_record' AS relation_type,
            'property_record' AS relation_subtype,
            match_method,
            confidence,
            parcel_layer_idx,
            parcel_entity_id,
            blocklot,
            'parcel_property_records' AS related_table,
            property_layer_idx AS related_layer_idx,
            property_entity_id AS related_entity_id,
            property_feature_idx AS related_feature_idx,
            NULL::UBIGINT AS related_event_id,
            property_layer_file AS source_layer_file,
            property_layer_name AS source_layer_name,
            coalesce(nullif(address, ''), nullif(owner, ''), property_layer_name) AS label,
            status AS detail,
            NULL::DATE AS event_date,
            value_usd AS amount_usd
        FROM parcel_property_records
        UNION ALL
        SELECT
            'parcel_event' AS relation_type,
            event_type AS relation_subtype,
            match_method,
            confidence,
            parcel_layer_idx,
            parcel_entity_id,
            blocklot,
            'parcel_related_events' AS related_table,
            NULL::UBIGINT AS related_layer_idx,
            NULL::VARCHAR AS related_entity_id,
            source_feature_idx AS related_feature_idx,
            event_id AS related_event_id,
            source_layer_file,
            source_layer_name,
            coalesce(nullif(event_title, ''), nullif(event_label, ''), event_type) AS label,
            coalesce(nullif(event_detail, ''), event_status) AS detail,
            event_date,
            amount_usd
        FROM parcel_related_events
        UNION ALL
        SELECT
            relation_type,
            coalesce(nullif(category, ''), relation_type) AS relation_subtype,
            match_method,
            confidence,
            parcel_layer_idx,
            parcel_entity_id,
            blocklot,
            'parcel_related_features' AS related_table,
            related_layer_idx,
            related_entity_id,
            related_feature_idx,
            NULL::UBIGINT AS related_event_id,
            related_layer_file AS source_layer_file,
            related_layer_name AS source_layer_name,
            coalesce(nullif(event_title_hint, ''), nullif(feature_name, ''), nullif(address, ''), related_layer_name) AS label,
            coalesce(nullif(event_detail_hint, ''), nullif(event_status_hint, ''), status) AS detail,
            try_cast(event_date_text AS DATE) AS event_date,
            coalesce(nullif(amount_usd_hint, 0), nullif(value_usd, 0)) AS amount_usd
        FROM parcel_related_features
    )SQL", "create parcel_relationships");

    exec_or_throw("CREATE INDEX IF NOT EXISTS idx_parcel_property_records_parcel ON parcel_property_records(parcel_entity_id)", "index parcel_property_records parcel");
    exec_or_throw("CREATE INDEX IF NOT EXISTS idx_parcel_property_records_blocklot ON parcel_property_records(blocklot)", "index parcel_property_records blocklot");
    exec_or_throw("CREATE INDEX IF NOT EXISTS idx_parcel_related_events_parcel ON parcel_related_events(parcel_entity_id)", "index parcel_related_events parcel");
    exec_or_throw("CREATE INDEX IF NOT EXISTS idx_parcel_related_events_blocklot ON parcel_related_events(blocklot)", "index parcel_related_events blocklot");
    exec_or_throw("CREATE INDEX IF NOT EXISTS idx_parcel_related_features_parcel ON parcel_related_features(parcel_entity_id)", "index parcel_related_features parcel");
    exec_or_throw("CREATE INDEX IF NOT EXISTS idx_parcel_related_features_blocklot ON parcel_related_features(blocklot)", "index parcel_related_features blocklot");
    exec_or_throw("CREATE INDEX IF NOT EXISTS idx_parcel_relationships_parcel ON parcel_relationships(parcel_entity_id)", "index parcel_relationships parcel");
    exec_or_throw("CREATE INDEX IF NOT EXISTS idx_parcel_relationships_type ON parcel_relationships(relation_type, relation_subtype)", "index parcel_relationships type");

    exec_or_throw(R"SQL(
        CREATE OR REPLACE VIEW parcel_relationship_summary AS
        SELECT
            parcel_layer_idx,
            parcel_entity_id,
            blocklot,
            relation_type,
            relation_subtype,
            count(*) AS related_count,
            max(confidence) AS max_confidence
        FROM parcel_relationships
        GROUP BY parcel_layer_idx, parcel_entity_id, blocklot, relation_type, relation_subtype
    )SQL", "create parcel_relationship_summary view");
}

void createEmptyParcelRelationshipArtifacts(duckdb::Connection& con) {
    auto exec_or_throw = [&](const std::string& sql, const char* context) {
        auto res = con.Query(sql);
        if (!res || res->HasError()) {
            throw std::runtime_error(
                std::string(context) + ": " + (res ? res->GetError() : std::string("query failed")));
        }
    };
    exec_or_throw(R"SQL(
        CREATE TABLE IF NOT EXISTS parcel_property_records (
            parcel_layer_idx UBIGINT,
            parcel_entity_id VARCHAR,
            parcel_geometry_entity_id VARCHAR,
            blocklot VARCHAR,
            property_layer_idx UBIGINT,
            property_entity_id VARCHAR,
            property_feature_idx UBIGINT,
            property_layer_file VARCHAR,
            property_layer_name VARCHAR,
            owner VARCHAR,
            address VARCHAR,
            zipcode VARCHAR,
            status VARCHAR,
            value_usd DOUBLE,
            structure_area_sqft DOUBLE,
            min_lon DOUBLE,
            min_lat DOUBLE,
            max_lon DOUBLE,
            max_lat DOUBLE,
            match_method VARCHAR,
            confidence DOUBLE
        )
    )SQL", "create empty parcel_property_records");
    exec_or_throw(R"SQL(
        CREATE TABLE IF NOT EXISTS parcel_related_events (
            parcel_layer_idx UBIGINT,
            parcel_entity_id VARCHAR,
            parcel_geometry_entity_id VARCHAR,
            blocklot VARCHAR,
            event_id UBIGINT,
            event_type VARCHAR,
            event_label VARCHAR,
            event_status VARCHAR,
            event_date DATE,
            event_year INTEGER,
            event_title VARCHAR,
            event_detail VARCHAR,
            amount_usd DOUBLE,
            source_layer_file VARCHAR,
            source_layer_name VARCHAR,
            source_feature_idx UBIGINT,
            match_method VARCHAR,
            confidence DOUBLE
        )
    )SQL", "create empty parcel_related_events");
    exec_or_throw(R"SQL(
        CREATE TABLE IF NOT EXISTS parcel_related_features (
            parcel_layer_idx UBIGINT,
            parcel_entity_id VARCHAR,
            parcel_geometry_entity_id VARCHAR,
            blocklot VARCHAR,
            related_layer_idx UBIGINT,
            related_entity_id VARCHAR,
            related_feature_idx UBIGINT,
            related_layer_file VARCHAR,
            related_layer_name VARCHAR,
            relation_type VARCHAR,
            category VARCHAR,
            owner VARCHAR,
            address VARCHAR,
            zipcode VARCHAR,
            status VARCHAR,
            value_usd DOUBLE,
            feature_name VARCHAR,
            event_date_text VARCHAR,
            event_status_hint VARCHAR,
            event_title_hint VARCHAR,
            event_detail_hint VARCHAR,
            amount_usd_hint DOUBLE,
            min_lon DOUBLE,
            min_lat DOUBLE,
            max_lon DOUBLE,
            max_lat DOUBLE,
            match_method VARCHAR,
            confidence DOUBLE
        )
    )SQL", "create empty parcel_related_features");
    exec_or_throw(R"SQL(
        CREATE TABLE IF NOT EXISTS parcel_relationships (
            relation_type VARCHAR,
            relation_subtype VARCHAR,
            match_method VARCHAR,
            confidence DOUBLE,
            parcel_layer_idx UBIGINT,
            parcel_entity_id VARCHAR,
            blocklot VARCHAR,
            related_table VARCHAR,
            related_layer_idx UBIGINT,
            related_entity_id VARCHAR,
            related_feature_idx UBIGINT,
            related_event_id UBIGINT,
            source_layer_file VARCHAR,
            source_layer_name VARCHAR,
            label VARCHAR,
            detail VARCHAR,
            event_date DATE,
            amount_usd DOUBLE
        )
    )SQL", "create empty parcel_relationships");
    exec_or_throw(R"SQL(
        CREATE OR REPLACE VIEW parcel_relationship_summary AS
        SELECT
            parcel_layer_idx,
            parcel_entity_id,
            blocklot,
            relation_type,
            relation_subtype,
            count(*) AS related_count,
            max(confidence) AS max_confidence
        FROM parcel_relationships
        GROUP BY parcel_layer_idx, parcel_entity_id, blocklot, relation_type, relation_subtype
    )SQL", "create empty parcel_relationship_summary view");
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
    const std::string build_source_signature = analyticsBuildSignature(root, layers);

    {
        const std::string built_at_utc = isoNowUtc();
        const std::string source_signature = build_source_signature;
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
    rebuildSpatialSearchArtifacts(con);
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
                lf.event_title_hint,
                lf.event_detail_hint,
                lf.event_metadata_json,
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
                WHEN lower(layer_file) IN ('vacant_building_notices.geojson', 'open_notices_vacant.geojson') THEN 'vacant_notice'
                WHEN lower(layer_file) LIKE '%open_notices%vacant%' THEN 'vacant_notice'
                WHEN lower(layer_name) LIKE '%open notices%' AND lower(layer_name) LIKE '%vacant%' THEN 'vacant_notice'
                WHEN lower(layer_file) = 'vacant_building_rehabs.geojson' THEN 'vacant_rehab'
                WHEN lower(layer_file) LIKE '%tax_lien%' THEN 'tax_lien'
                WHEN lower(layer_file) LIKE '%tax_sale%' THEN 'tax_sale'
                WHEN lower(layer_file) LIKE '%building_permits%' THEN 'building_permit'
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
            CASE
                WHEN lower(layer_file) LIKE '%building_permits%' THEN 'Building Permit'
                WHEN lower(layer_file) IN ('vacant_building_notices.geojson', 'open_notices_vacant.geojson') THEN 'Vacant Notice'
                WHEN lower(layer_file) = 'vacant_building_rehabs.geojson' THEN 'Vacant Rehab'
                WHEN lower(layer_file) LIKE '%tax_lien%' THEN 'Tax Lien'
                WHEN lower(layer_file) LIKE '%tax_sale%' THEN 'Tax Sale'
                WHEN lower(layer_file) LIKE '%open_bid_list_vacants_to_value%' THEN 'Vacants to Value Bid'
                WHEN nullif(event_title_hint, '') IS NOT NULL THEN event_title_hint
                ELSE replace(
                    CASE
                        WHEN lower(layer_name) LIKE '%vacant%' THEN 'vacancy_related'
                        WHEN lower(category) = 'housing' THEN 'housing'
                        WHEN lower(category) = 'permits' THEN 'permit'
                        WHEN lower(category) = 'taxes' THEN 'tax'
                        ELSE duckdb_role
                    END,
                    '_', ' ')
            END AS event_label,
            nullif(event_title_hint, '') AS event_title,
            nullif(event_detail_hint, '') AS event_detail,
            nullif(event_metadata_json, '') AS event_metadata_json,
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
    refreshUnifiedParcelEventRollups(con);
    rebuildParcelRelationshipArtifacts(con);
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
    validateExistingCache();
}

bool DuckDbAnalytics::ensureReady() {
    return status_.last_rebuild_ok || validateExistingCache();
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
        {
            std::lock_guard<std::mutex> lk(parcel_detail_cache_mutex_);
            parcel_detail_cache_.clear();
        }
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
              AND table_name IN (
                  'layer_features',
                  'layer_feature_properties',
                  'unified_parcels',
                  'parcel_events',
                  'layer_feature_bboxes',
                  'layer_bboxes',
                  'parcel_zone_memberships'
              )
        )SQL");
        if (!table_check || table_check->HasError() || table_check->RowCount() == 0) {
            status_.last_rebuild_ok = false;
            status_.message = "DuckDB analytics cache validation failed.";
            return false;
        }
        const int64_t table_count = table_check->GetValue<int64_t>(0, 0);
        if (table_count < 7) {
            status_.last_rebuild_ok = false;
            status_.message = "DuckDB analytics cache is incomplete.";
            return false;
        }
        auto relationship_table_res = con.Query(R"SQL(
            SELECT count(*)::BIGINT
            FROM information_schema.tables
            WHERE table_schema = 'main'
              AND table_name IN (
                  'parcel_property_records',
                  'parcel_related_events',
                  'parcel_related_features',
                  'parcel_relationships'
              )
        )SQL");
        const int64_t relationship_table_count =
            relationship_table_res && !relationship_table_res->HasError() && relationship_table_res->RowCount() > 0
                ? relationship_table_res->GetValue<int64_t>(0, 0)
                : 0;
        if (relationship_table_count < 4) {
            auto event_schema_res = con.Query(R"SQL(
                SELECT count(*)::BIGINT
                FROM information_schema.columns
                WHERE table_schema = 'main'
                  AND table_name = 'parcel_events'
                  AND column_name IN (
                      'event_id',
                      'event_label',
                      'event_title',
                      'event_detail',
                      'event_metadata_json',
                      'source_feature_idx'
                  )
            )SQL");
            const bool full_event_schema =
                event_schema_res &&
                !event_schema_res->HasError() &&
                event_schema_res->RowCount() > 0 &&
                event_schema_res->GetValue<int64_t>(0, 0) == 6;
            if (full_event_schema) rebuildParcelRelationshipArtifacts(con);
            else createEmptyParcelRelationshipArtifacts(con);
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
        auto parcel_table_res = con.Query(R"SQL(
            SELECT count(*)::BIGINT
            FROM information_schema.tables
            WHERE table_schema = 'main'
              AND table_name IN ('parcel_features', 'unified_parcels')
        )SQL");
        const bool has_full_parcel_tables =
            parcel_table_res &&
            !parcel_table_res->HasError() &&
            parcel_table_res->RowCount() > 0 &&
            parcel_table_res->GetValue<int64_t>(0, 0) == 2;
        if (has_full_parcel_tables) {
            auto parcel_contract_res = con.Query(R"SQL(
            SELECT
                (SELECT count(*)::BIGINT
                 FROM parcel_features
                 WHERE layer_file = 'parcel.geojson'
                    OR layer_file LIKE '%_county_parcels.geojson') AS primary_parcel_features,
                (SELECT count(*)::BIGINT FROM unified_parcels) AS unified_parcels
        )SQL");
            if (!parcel_contract_res || parcel_contract_res->HasError() || parcel_contract_res->RowCount() == 0) {
                status_.last_rebuild_ok = false;
                status_.message = "DuckDB analytics cache validation failed: parcel contract query failed.";
                return false;
            }
            const int64_t primary_parcel_features = parcel_contract_res->GetValue<int64_t>(0, 0);
            const int64_t unified_parcel_rows = parcel_contract_res->GetValue<int64_t>(1, 0);
            if (primary_parcel_features > 0 && unified_parcel_rows < primary_parcel_features) {
                status_.last_rebuild_ok = false;
                std::ostringstream parcel_msg;
                parcel_msg << "DuckDB analytics cache validation failed: unified_parcels has "
                           << unified_parcel_rows << " rows for "
                           << primary_parcel_features << " primary parcel features.";
                status_.message = parcel_msg.str();
                return false;
            }
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

                    std::string sidecar_signature;
                    const std::string expected_signature = analyticsBuildSignature(root_, layers);
                    const bool build_signature_current =
                        loadAnalyticsSignatureSidecar(status_.db_path, sidecar_signature) &&
                        sidecar_signature == expected_signature;
                    if (changed_files.empty() && build_signature_current) {
                        auto exec_or_throw = [&](const std::string& sql, const char* context) {
                            auto res = con.Query(sql);
                            if (!res || res->HasError()) {
                                throw std::runtime_error(
                                    std::string(context) + ": " +
                                    (res ? res->GetError() : std::string("query failed")));
                            }
                        };
                        exec_or_throw("BEGIN TRANSACTION", "begin derived analytics refresh");
                        exec_or_throw("DROP TABLE IF EXISTS parcel_events", "drop parcel_events");
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
                                    lf.event_title_hint,
                                    lf.event_detail_hint,
                                    lf.event_metadata_json,
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
                                    WHEN lower(layer_file) IN ('vacant_building_notices.geojson', 'open_notices_vacant.geojson') THEN 'vacant_notice'
                                    WHEN lower(layer_file) LIKE '%open_notices%vacant%' THEN 'vacant_notice'
                                    WHEN lower(layer_name) LIKE '%open notices%' AND lower(layer_name) LIKE '%vacant%' THEN 'vacant_notice'
                                    WHEN lower(layer_file) = 'vacant_building_rehabs.geojson' THEN 'vacant_rehab'
                                    WHEN lower(layer_file) LIKE '%tax_lien%' THEN 'tax_lien'
                                    WHEN lower(layer_file) LIKE '%tax_sale%' THEN 'tax_sale'
                                    WHEN lower(layer_file) LIKE '%building_permits%' THEN 'building_permit'
                                    WHEN lower(layer_file) LIKE '%open_bid_list_vacants_to_value%' THEN 'vacants_to_value_bid'
                                    WHEN lower(layer_name) LIKE '%vacant%' THEN 'vacancy_related'
                                    WHEN lower(category) = 'housing' THEN 'housing'
                                    WHEN lower(category) = 'permits' THEN 'permit'
                                    WHEN lower(category) = 'taxes' THEN 'tax'
                                    ELSE duckdb_role
                                END AS event_type,
                                coalesce(nullif(event_status_hint, ''), feature_status) AS event_status,
                                cast(parsed_event_ts AS DATE) AS event_date,
                                coalesce(
                                    nullif(event_year_hint, 0),
                                    try_cast(strftime(parsed_event_ts, '%Y') AS INTEGER)
                                ) AS event_year,
                                CASE
                                    WHEN lower(layer_file) LIKE '%building_permits%' THEN 'Building Permit'
                                    WHEN lower(layer_file) IN ('vacant_building_notices.geojson', 'open_notices_vacant.geojson') THEN 'Vacant Notice'
                                    WHEN lower(layer_file) = 'vacant_building_rehabs.geojson' THEN 'Vacant Rehab'
                                    WHEN lower(layer_file) LIKE '%tax_lien%' THEN 'Tax Lien'
                                    WHEN lower(layer_file) LIKE '%tax_sale%' THEN 'Tax Sale'
                                    WHEN lower(layer_file) LIKE '%open_bid_list_vacants_to_value%' THEN 'Vacants to Value Bid'
                                    WHEN nullif(event_title_hint, '') IS NOT NULL THEN event_title_hint
                                    ELSE replace(
                                        CASE
                                            WHEN lower(layer_name) LIKE '%vacant%' THEN 'vacancy_related'
                                            WHEN lower(category) = 'housing' THEN 'housing'
                                            WHEN lower(category) = 'permits' THEN 'permit'
                                            WHEN lower(category) = 'taxes' THEN 'tax'
                                            ELSE duckdb_role
                                        END,
                                        '_', ' ')
                                END AS event_label,
                                nullif(event_title_hint, '') AS event_title,
                                nullif(event_detail_hint, '') AS event_detail,
                                nullif(event_metadata_json, '') AS event_metadata_json,
                                coalesce(nullif(value_usd, 0), nullif(amount_usd_hint, 0)) AS amount_usd,
                                layer_file AS source_layer_file,
                                layer_name AS source_layer_name,
                                feature_idx AS source_feature_idx
                            FROM base
                        )SQL", "create parcel_events");
                        exec_or_throw("CREATE INDEX IF NOT EXISTS idx_parcel_events_blocklot ON parcel_events(blocklot)", "index parcel_events blocklot");
                        exec_or_throw("CREATE INDEX IF NOT EXISTS idx_parcel_events_event_year ON parcel_events(event_year)", "index parcel_events event_year");
                        exec_or_throw("CREATE INDEX IF NOT EXISTS idx_parcel_events_event_type ON parcel_events(event_type)", "index parcel_events event_type");
                        refreshUnifiedParcelEventRollups(con);
                        rebuildParcelRelationshipArtifacts(con);
                        exec_or_throw(
                            "UPDATE analytics_build_info SET source_signature = '" +
                                sqlQuote(analyticsBuildSignature(root_, layers)) + "'",
                            "update analytics build signature");
                        exec_or_throw("COMMIT", "commit derived analytics refresh");
                        saveAnalyticsSignatureSidecar(status_.db_path, analyticsBuildSignature(root_, layers));
                        result.ok = true;
                        result.incrementally_updated = true;
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
        {
            std::lock_guard<std::mutex> lk(parcel_detail_cache_mutex_);
            parcel_detail_cache_.clear();
        }
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
        exec_or_throw("DROP TABLE IF EXISTS parcel_zone_memberships", "drop parcel_zone_memberships");
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
                event_title_hint VARCHAR,
                event_detail_hint VARCHAR,
                event_metadata_json VARCHAR,
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
                    "event_date", "DateNotice", "DateIssue", "DateIssued", "IssuedDate",
                    "Issue_Date_ISO", "Issue_Date", "DateFiled", "DateAuction",
                    "DateDemoFinished", "ReleasedToContractor", "SALEDATE", "DATE",
                    "CREATED_DATE", "RECORD_DATE"
                });
                const std::string event_status_hint = firstDisplayProperty(fg, {"CASE_STATUS", "STATUS", "STATE"});
                const std::string event_title_hint = eventTitleHintForFeature(layer, fi);
                const std::string event_detail_hint = eventDetailHintForFeature(layer, fi);
                const std::string event_metadata_json = eventMetadataJsonForFeature(layer, fi);
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
                appender.Append<const char*>(event_title_hint.c_str());
                appender.Append<const char*>(event_detail_hint.c_str());
                appender.Append<const char*>(event_metadata_json.c_str());
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
        const std::string full_rebuild_source_signature = analyticsBuildSignature(root_, layers);
        rebuildSpatialSearchArtifacts(con);
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
            const std::string source_signature = full_rebuild_source_signature;
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
                    lf.event_title_hint,
                    lf.event_detail_hint,
                    lf.event_metadata_json,
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
                    WHEN lower(layer_file) IN ('vacant_building_notices.geojson', 'open_notices_vacant.geojson') THEN 'vacant_notice'
                    WHEN lower(layer_file) LIKE '%open_notices%vacant%' THEN 'vacant_notice'
                    WHEN lower(layer_name) LIKE '%open notices%' AND lower(layer_name) LIKE '%vacant%' THEN 'vacant_notice'
                WHEN lower(layer_file) = 'vacant_building_rehabs.geojson' THEN 'vacant_rehab'
                WHEN lower(layer_file) LIKE '%tax_lien%' THEN 'tax_lien'
                WHEN lower(layer_file) LIKE '%tax_sale%' THEN 'tax_sale'
                WHEN lower(layer_file) LIKE '%building_permits%' THEN 'building_permit'
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
                CASE
                    WHEN lower(layer_file) LIKE '%building_permits%' THEN 'Building Permit'
                    WHEN lower(layer_file) IN ('vacant_building_notices.geojson', 'open_notices_vacant.geojson') THEN 'Vacant Notice'
                    WHEN lower(layer_file) = 'vacant_building_rehabs.geojson' THEN 'Vacant Rehab'
                    WHEN lower(layer_file) LIKE '%tax_lien%' THEN 'Tax Lien'
                    WHEN lower(layer_file) LIKE '%tax_sale%' THEN 'Tax Sale'
                    WHEN lower(layer_file) LIKE '%open_bid_list_vacants_to_value%' THEN 'Vacants to Value Bid'
                    WHEN nullif(event_title_hint, '') IS NOT NULL THEN event_title_hint
                    ELSE replace(
                        CASE
                            WHEN lower(layer_name) LIKE '%vacant%' THEN 'vacancy_related'
                            WHEN lower(category) = 'housing' THEN 'housing'
                            WHEN lower(category) = 'permits' THEN 'permit'
                            WHEN lower(category) = 'taxes' THEN 'tax'
                            ELSE duckdb_role
                        END,
                        '_', ' ')
                END AS event_label,
                nullif(event_title_hint, '') AS event_title,
                nullif(event_detail_hint, '') AS event_detail,
                nullif(event_metadata_json, '') AS event_metadata_json,
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
        refreshUnifiedParcelEventRollups(con);
        rebuildParcelRelationshipArtifacts(con);
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
        const int layer_col = find_col({"layer_idx", "layer", "parcel_layer_idx"});
        const int feature_col = find_col({"feature_idx", "feature", "parcel_feature_idx"});
        const int entity_id_col = find_col({"entity_id", "parcel_entity_id", "parcel_geometry_entity_id"});
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
                } else if (layer_col >= 0 &&
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
    {
        std::lock_guard<std::mutex> lk(parcel_detail_cache_mutex_);
        auto it = parcel_detail_cache_.find(key);
        if (it != parcel_detail_cache_.end()) return it->second;
    }

    auto unified_detail_sql = [&](const char* column) {
        std::ostringstream sql;
        sql << R"SQL(
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
            WHERE )SQL" << column << R"SQL( = ')SQL" << sqlQuote(key) << R"SQL('
            LIMIT 1
        )SQL";
        return sql.str();
    };

    DuckDbQueryResult result = executeMapQuery(unified_detail_sql("parcel_entity_id"), {}, {}, 1);
    if (result.ok && result.rows.empty()) {
        result = executeMapQuery(unified_detail_sql("parcel_geometry_entity_id"), {}, {}, 1);
    }

    {
        std::lock_guard<std::mutex> lk(parcel_detail_cache_mutex_);
        parcel_detail_cache_[key] = result;
    }
    return result;
}

DuckDbUnifiedParcelDetail DuckDbAnalytics::queryUnifiedParcelDetailRecord(const std::string& parcel_entity_id) const {
    DuckDbUnifiedParcelDetail out;
    DuckDbQueryResult detail = queryUnifiedParcelDetail(parcel_entity_id);
    out.ok = detail.ok;
    out.message = detail.message;
    if (!detail.ok || detail.rows.empty()) return out;

    const auto& row = detail.rows.front();
    UnifiedParcelRecord rec;
    rec.parcel_layer_idx = queryResultSize(detail, row, "parcel_layer_idx");
    rec.parcel_entity_id = queryResultCell(detail, row, "parcel_entity_id");
    rec.parcel_geometry_entity_id = queryResultCell(detail, row, "parcel_geometry_entity_id");
    rec.blocklot = queryResultCell(detail, row, "blocklot");
    rec.parcel_source_file = queryResultCell(detail, row, "parcel_source_file");
    rec.property_source_file = queryResultCell(detail, row, "property_source_file");
    rec.parcel_has_geometry = queryResultBool(detail, row, "parcel_has_geometry");
    rec.has_property_record = queryResultBool(detail, row, "has_property_record");
    rec.owner = queryResultCell(detail, row, "owner");
    rec.owner_display = queryResultCell(detail, row, "owner_display");
    rec.address = queryResultCell(detail, row, "address");
    rec.zip = queryResultCell(detail, row, "zipcode");
    rec.status = queryResultCell(detail, row, "status");
    rec.current_land = queryResultDouble(detail, row, "current_land");
    rec.current_improvements = queryResultDouble(detail, row, "current_improvements");
    rec.structure_area_sqft = queryResultDouble(detail, row, "structure_area_sqft");
    rec.tax_base = queryResultDouble(detail, row, "tax_base");
    rec.sale_price = queryResultDouble(detail, row, "sale_price");
    rec.current_value = queryResultDouble(detail, row, "current_value");
    rec.vacant_notice_count = queryResultInt(detail, row, "vacant_notice_count");
    rec.vacant_rehab_count = queryResultInt(detail, row, "vacant_rehab_count");
    rec.tax_lien_count = queryResultInt(detail, row, "tax_lien_count");
    rec.tax_sale_count = queryResultInt(detail, row, "tax_sale_count");
    rec.tax_lien_amount = queryResultDouble(detail, row, "tax_lien_amount");
    rec.tax_sale_amount = queryResultDouble(detail, row, "tax_sale_amount");
    rec.parcel_extent.min_lon = static_cast<float>(queryResultDouble(detail, row, "min_lon"));
    rec.parcel_extent.min_lat = static_cast<float>(queryResultDouble(detail, row, "min_lat"));
    rec.parcel_extent.max_lon = static_cast<float>(queryResultDouble(detail, row, "max_lon"));
    rec.parcel_extent.max_lat = static_cast<float>(queryResultDouble(detail, row, "max_lat"));

    out.ok = true;
    out.found = true;
    out.record = std::move(rec);
    return out;
}

DuckDbUnifiedParcelDetail DuckDbAnalytics::queryUnifiedParcelDetailRecordByLayerFeature(
    size_t parcel_layer_idx,
    size_t parcel_feature_idx) const {
    DuckDbUnifiedParcelDetail out;
    const std::string cache_key =
        "layer_feature:" + std::to_string(parcel_layer_idx) + ":" + std::to_string(parcel_feature_idx);
    {
        std::lock_guard<std::mutex> lk(parcel_detail_cache_mutex_);
        auto it = parcel_detail_cache_.find(cache_key);
        if (it != parcel_detail_cache_.end()) {
            const DuckDbQueryResult& cached = it->second;
            out.ok = cached.ok;
            out.message = cached.message;
            if (!cached.ok || cached.rows.empty()) return out;

            const auto& row = cached.rows.front();
            UnifiedParcelRecord rec;
            rec.parcel_layer_idx = queryResultSize(cached, row, "parcel_layer_idx");
            rec.parcel_entity_id = queryResultCell(cached, row, "parcel_entity_id");
            rec.parcel_geometry_entity_id = queryResultCell(cached, row, "parcel_geometry_entity_id");
            rec.blocklot = queryResultCell(cached, row, "blocklot");
            rec.parcel_source_file = queryResultCell(cached, row, "parcel_source_file");
            rec.property_source_file = queryResultCell(cached, row, "property_source_file");
            rec.parcel_has_geometry = queryResultBool(cached, row, "parcel_has_geometry");
            rec.has_property_record = queryResultBool(cached, row, "has_property_record");
            rec.owner = queryResultCell(cached, row, "owner");
            rec.owner_display = queryResultCell(cached, row, "owner_display");
            rec.address = queryResultCell(cached, row, "address");
            rec.zip = queryResultCell(cached, row, "zipcode");
            rec.status = queryResultCell(cached, row, "status");
            rec.current_land = queryResultDouble(cached, row, "current_land");
            rec.current_improvements = queryResultDouble(cached, row, "current_improvements");
            rec.structure_area_sqft = queryResultDouble(cached, row, "structure_area_sqft");
            rec.tax_base = queryResultDouble(cached, row, "tax_base");
            rec.sale_price = queryResultDouble(cached, row, "sale_price");
            rec.current_value = queryResultDouble(cached, row, "current_value");
            rec.vacant_notice_count = queryResultInt(cached, row, "vacant_notice_count");
            rec.vacant_rehab_count = queryResultInt(cached, row, "vacant_rehab_count");
            rec.tax_lien_count = queryResultInt(cached, row, "tax_lien_count");
            rec.tax_sale_count = queryResultInt(cached, row, "tax_sale_count");
            rec.tax_lien_amount = queryResultDouble(cached, row, "tax_lien_amount");
            rec.tax_sale_amount = queryResultDouble(cached, row, "tax_sale_amount");
            rec.parcel_extent.min_lon = static_cast<float>(queryResultDouble(cached, row, "min_lon"));
            rec.parcel_extent.min_lat = static_cast<float>(queryResultDouble(cached, row, "min_lat"));
            rec.parcel_extent.max_lon = static_cast<float>(queryResultDouble(cached, row, "max_lon"));
            rec.parcel_extent.max_lat = static_cast<float>(queryResultDouble(cached, row, "max_lat"));

            out.found = true;
            out.record = std::move(rec);
            return out;
        }
    }

    std::ostringstream sql;
    sql << R"SQL(
        SELECT
            up.parcel_layer_idx,
            up.parcel_entity_id,
            up.parcel_geometry_entity_id,
            up.blocklot,
            up.parcel_source_file,
            up.property_source_file,
            up.parcel_has_geometry,
            up.has_property_record,
            up.owner,
            up.owner_display,
            up.address,
            up.zipcode,
            up.status,
            up.current_land,
            up.current_improvements,
            up.structure_area_sqft,
            up.tax_base,
            up.sale_price,
            up.current_value,
            up.vacant_notice_count,
            up.vacant_rehab_count,
            up.tax_lien_count,
            up.tax_sale_count,
            up.tax_lien_amount,
            up.tax_sale_amount,
            up.min_lon,
            up.min_lat,
            up.max_lon,
            up.max_lat
        FROM layer_features lf
        JOIN unified_parcels up
          ON up.parcel_layer_idx = lf.layer_idx
         AND up.parcel_entity_id = lf.entity_id
        WHERE lf.layer_idx = )SQL" << (uint64_t)parcel_layer_idx << R"SQL(
          AND lf.feature_idx = )SQL" << (uint64_t)parcel_feature_idx << R"SQL(
        LIMIT 1
    )SQL";

    const DuckDbQueryResult detail = executeMapQuery(sql.str(), {}, {}, 1);
    {
        std::lock_guard<std::mutex> lk(parcel_detail_cache_mutex_);
        parcel_detail_cache_[cache_key] = detail;
    }
    out.ok = detail.ok;
    out.message = detail.message;
    if (!detail.ok || detail.rows.empty()) return out;

    const auto& row = detail.rows.front();
    UnifiedParcelRecord rec;
    rec.parcel_layer_idx = queryResultSize(detail, row, "parcel_layer_idx");
    rec.parcel_entity_id = queryResultCell(detail, row, "parcel_entity_id");
    rec.parcel_geometry_entity_id = queryResultCell(detail, row, "parcel_geometry_entity_id");
    rec.blocklot = queryResultCell(detail, row, "blocklot");
    rec.parcel_source_file = queryResultCell(detail, row, "parcel_source_file");
    rec.property_source_file = queryResultCell(detail, row, "property_source_file");
    rec.parcel_has_geometry = queryResultBool(detail, row, "parcel_has_geometry");
    rec.has_property_record = queryResultBool(detail, row, "has_property_record");
    rec.owner = queryResultCell(detail, row, "owner");
    rec.owner_display = queryResultCell(detail, row, "owner_display");
    rec.address = queryResultCell(detail, row, "address");
    rec.zip = queryResultCell(detail, row, "zipcode");
    rec.status = queryResultCell(detail, row, "status");
    rec.current_land = queryResultDouble(detail, row, "current_land");
    rec.current_improvements = queryResultDouble(detail, row, "current_improvements");
    rec.structure_area_sqft = queryResultDouble(detail, row, "structure_area_sqft");
    rec.tax_base = queryResultDouble(detail, row, "tax_base");
    rec.sale_price = queryResultDouble(detail, row, "sale_price");
    rec.current_value = queryResultDouble(detail, row, "current_value");
    rec.vacant_notice_count = queryResultInt(detail, row, "vacant_notice_count");
    rec.vacant_rehab_count = queryResultInt(detail, row, "vacant_rehab_count");
    rec.tax_lien_count = queryResultInt(detail, row, "tax_lien_count");
    rec.tax_sale_count = queryResultInt(detail, row, "tax_sale_count");
    rec.tax_lien_amount = queryResultDouble(detail, row, "tax_lien_amount");
    rec.tax_sale_amount = queryResultDouble(detail, row, "tax_sale_amount");
    rec.parcel_extent.min_lon = static_cast<float>(queryResultDouble(detail, row, "min_lon"));
    rec.parcel_extent.min_lat = static_cast<float>(queryResultDouble(detail, row, "min_lat"));
    rec.parcel_extent.max_lon = static_cast<float>(queryResultDouble(detail, row, "max_lon"));
    rec.parcel_extent.max_lat = static_cast<float>(queryResultDouble(detail, row, "max_lat"));

    out.ok = true;
    out.found = true;
    out.record = std::move(rec);
    return out;
}

DuckDbQueryResult DuckDbAnalytics::queryParcelRelationships(
    const std::string& parcel_entity_id,
    size_t max_rows) const {
    const std::string key = normalizeJoinKey(parcel_entity_id);
    if (key.empty()) {
        DuckDbQueryResult out;
        out.ok = false;
        out.message = "Parcel entity ID is empty";
        return out;
    }

    std::ostringstream sql;
    sql << R"SQL(
        SELECT
            relation_type,
            relation_subtype,
            match_method,
            confidence,
            blocklot,
            related_table,
            related_layer_idx,
            related_entity_id,
            related_feature_idx,
            related_event_id,
            source_layer_file,
            source_layer_name,
            label,
            detail,
            event_date,
            amount_usd
        FROM parcel_relationships
        WHERE parcel_entity_id = ')SQL" << sqlQuote(key) << R"SQL('
        ORDER BY
            CASE relation_type
                WHEN 'property_record' THEN 0
                WHEN 'parcel_event' THEN 1
                ELSE 2
            END,
            event_date DESC NULLS LAST,
            source_layer_file ASC,
            label ASC
    )SQL";
    return executeMapQuery(sql.str(), {}, {}, max_rows);
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
            event_label,
            event_title,
            event_detail,
            event_metadata_json,
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
                record.owner_search = normalizeFuzzySearchText(
                    record.owner_display.empty() ? record.owner : record.owner_display);

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
