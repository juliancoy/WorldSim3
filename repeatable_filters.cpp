#include "repeatable_filters.h"

#include "app_utils.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
std::string sqlQuote(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('\'');
    for (char c : value) {
        if (c == '\'') out += "''";
        else out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

std::string jsonValueToSqlLiteral(const json& value) {
    if (value.is_boolean()) return value.get<bool>() ? "TRUE" : "FALSE";
    if (value.is_number_integer()) return std::to_string(value.get<long long>());
    if (value.is_number_unsigned()) return std::to_string(value.get<unsigned long long>());
    if (value.is_number_float()) {
        std::ostringstream os;
        os << value.get<double>();
        return os.str();
    }
    if (value.is_string()) return sqlQuote(value.get<std::string>());
    return sqlQuote(value.dump());
}

bool isSupportedRepeatableFilterEntity(const std::string& entity) {
    return entity == "parcel" || entity == "parcel_property";
}

std::string repeatableFilterExecutionMode(const json& spec) {
    if (spec.contains("execution") && spec["execution"].is_object()) {
        const std::string mode = toLowerAscii(spec["execution"].value("mode", ""));
        if (!mode.empty()) return mode;
    }
    if (spec.contains("sql") && spec["sql"].is_string()) return "sql";
    return "conditions";
}

bool validateRepeatableFilterCondition(const json& cond, std::string& error) {
    if (!cond.is_object()) {
        error = "condition must be an object";
        return false;
    }
    const std::string field = cond.value("field", "");
    const std::string op = toLowerAscii(cond.value("op", ""));
    if (field.empty() || op.empty()) {
        error = "condition requires field and op";
        return false;
    }
    auto field_it = repeatableFilterFieldSpecs().find(field);
    if (field_it == repeatableFilterFieldSpecs().end()) {
        error = "unsupported field: " + field;
        return false;
    }
    const auto type = field_it->second.type;
    const bool uses_values = op == "in";
    const bool uses_value =
        op == "=" || op == "!=" || op == ">" || op == ">=" || op == "<" || op == "<=" ||
        op == "contains" || op == "starts_with";
    if (!uses_values && !uses_value) {
        error = "unsupported operator: " + op;
        return false;
    }
    if (uses_values) {
        if (!cond.contains("values") || !cond["values"].is_array() || cond["values"].empty()) {
            error = "operator 'in' requires a non-empty values array";
            return false;
        }
    } else if (!cond.contains("value")) {
        error = "operator requires value";
        return false;
    }
    if (type == RepeatableFilterFieldSpec::Type::Boolean && !(op == "=" || op == "!=")) {
        error = "boolean field only supports = and !=";
        return false;
    }
    if (type == RepeatableFilterFieldSpec::Type::Number && (op == "contains" || op == "starts_with")) {
        error = "numeric field does not support text operators";
        return false;
    }
    if (type == RepeatableFilterFieldSpec::Type::Text &&
        (op == ">" || op == ">=" || op == "<" || op == "<=")) {
        error = "text field does not support numeric comparison operators";
        return false;
    }
    return true;
}

std::string repeatableFilterConditionSql(const json& cond) {
    const std::string field = cond.at("field").get<std::string>();
    const std::string op = toLowerAscii(cond.at("op").get<std::string>());
    const auto& field_spec = repeatableFilterFieldSpecs().at(field);
    const std::string column = field_spec.column;
    if (op == "contains") {
        return column + " ILIKE " + sqlQuote("%" + cond.at("value").get<std::string>() + "%");
    }
    if (op == "starts_with") {
        return column + " ILIKE " + sqlQuote(cond.at("value").get<std::string>() + "%");
    }
    if (op == "in") {
        std::vector<std::string> values;
        values.reserve(cond.at("values").size());
        for (const auto& v : cond.at("values")) values.push_back(jsonValueToSqlLiteral(v));
        std::ostringstream os;
        os << column << " IN (";
        for (size_t i = 0; i < values.size(); ++i) {
            if (i) os << ", ";
            os << values[i];
        }
        os << ")";
        return os.str();
    }
    return column + " " + op + " " + jsonValueToSqlLiteral(cond.at("value"));
}

json colorJson(const float color[4]) {
    return {
        {"r", color[0]},
        {"g", color[1]},
        {"b", color[2]},
        {"a", color[3]}
    };
}
}

std::string sanitizeRepeatableFilterId(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        if (std::isalnum((unsigned char)c) || c == '_' || c == '-') out.push_back(c);
    }
    return out;
}

fs::path repeatableFilterDir(const fs::path& root) {
    return root / "data" / "filters";
}

fs::path repeatableFilterPath(const fs::path& root, const std::string& id) {
    return repeatableFilterDir(root) / (id + ".json");
}

const std::unordered_map<std::string, RepeatableFilterFieldSpec>& repeatableFilterFieldSpecs() {
    static const std::unordered_map<std::string, RepeatableFilterFieldSpec> specs = {
        {"blocklot", {"blocklot", RepeatableFilterFieldSpec::Type::Text}},
        {"owner", {"owner", RepeatableFilterFieldSpec::Type::Text}},
        {"owner_display", {"owner_display", RepeatableFilterFieldSpec::Type::Text}},
        {"address", {"address", RepeatableFilterFieldSpec::Type::Text}},
        {"zip", {"zipcode", RepeatableFilterFieldSpec::Type::Text}},
        {"zipcode", {"zipcode", RepeatableFilterFieldSpec::Type::Text}},
        {"status", {"status", RepeatableFilterFieldSpec::Type::Text}},
        {"parcel_source_file", {"parcel_source_file", RepeatableFilterFieldSpec::Type::Text}},
        {"property_source_file", {"property_source_file", RepeatableFilterFieldSpec::Type::Text}},
        {"has_property_record", {"has_property_record", RepeatableFilterFieldSpec::Type::Boolean}},
        {"current_land", {"current_land", RepeatableFilterFieldSpec::Type::Number}},
        {"current_improvements", {"current_improvements", RepeatableFilterFieldSpec::Type::Number}},
        {"tax_base", {"tax_base", RepeatableFilterFieldSpec::Type::Number}},
        {"sale_price", {"sale_price", RepeatableFilterFieldSpec::Type::Number}},
        {"current_value", {"current_value", RepeatableFilterFieldSpec::Type::Number}},
        {"vacant_notice_count", {"vacant_notice_count", RepeatableFilterFieldSpec::Type::Number}},
        {"vacant_rehab_count", {"vacant_rehab_count", RepeatableFilterFieldSpec::Type::Number}},
        {"tax_lien_count", {"tax_lien_count", RepeatableFilterFieldSpec::Type::Number}},
        {"tax_sale_count", {"tax_sale_count", RepeatableFilterFieldSpec::Type::Number}},
        {"foreclosure_filing_count", {"foreclosure_filing_count", RepeatableFilterFieldSpec::Type::Number}},
        {"open_receivership_count", {"open_receivership_count", RepeatableFilterFieldSpec::Type::Number}},
        {"auction_count", {"auction_count", RepeatableFilterFieldSpec::Type::Number}},
        {"tax_lien_amount", {"tax_lien_amount", RepeatableFilterFieldSpec::Type::Number}},
        {"tax_sale_amount", {"tax_sale_amount", RepeatableFilterFieldSpec::Type::Number}}
    };
    return specs;
}

bool validateRepeatableFilterSpec(json spec, std::string& error) {
    if (!spec.is_object()) {
        error = "request body must be a JSON object";
        return false;
    }
    const std::string id = sanitizeRepeatableFilterId(spec.value("id", ""));
    if (id.empty()) {
        error = "filter id is required and must use [A-Za-z0-9_-]";
        return false;
    }
    const std::string entity = spec.value("entity", "");
    if (!isSupportedRepeatableFilterEntity(entity)) {
        error = "supported entities are 'parcel' and 'parcel_property'";
        return false;
    }
    if (!spec.contains("version") || !spec["version"].is_number_integer() || spec["version"].get<int>() <= 0) {
        error = "version must be a positive integer";
        return false;
    }
    const std::string mode = repeatableFilterExecutionMode(spec);
    if (mode == "conditions") {
        if (!spec.contains("conditions") || !spec["conditions"].is_array()) {
            error = "conditions mode requires a conditions array";
            return false;
        }
        for (const auto& cond : spec["conditions"]) {
            if (!validateRepeatableFilterCondition(cond, error)) return false;
        }
        return true;
    }
    if (mode == "sql") {
        if (!spec.contains("sql") || !spec["sql"].is_string() || trimDisplayValue(spec["sql"].get<std::string>()).empty()) {
            error = "sql mode requires a non-empty sql string";
            return false;
        }
        return true;
    }
    error = "execution.mode must be 'conditions' or 'sql'";
    return false;
}

std::string repeatableFilterSql(const json& spec) {
    if (repeatableFilterExecutionMode(spec) == "sql") {
        return spec.value("sql", "");
    }
    std::ostringstream where;
    bool first = true;
    for (const auto& cond : spec.at("conditions")) {
        if (!first) where << " AND ";
        first = false;
        where << "(" << repeatableFilterConditionSql(cond) << ")";
    }
    std::ostringstream sql;
    sql << "SELECT "
        << "parcel_layer_idx AS layer_idx, "
        << "parcel_entity_id AS entity_id, "
        << "blocklot, owner, owner_display, address, current_value, "
        << "has_property_record, parcel_source_file, property_source_file "
        << "FROM unified_parcels";
    if (!first) sql << " WHERE " << where.str();
    sql << " ORDER BY parcel_entity_id";
    return sql.str();
}

json repeatableFilterSummary(const json& spec) {
    json out = {
        {"id", spec.value("id", "")},
        {"name", spec.value("name", "")},
        {"entity", spec.value("entity", "")},
        {"version", spec.value("version", 0)},
        {"execution_mode", repeatableFilterExecutionMode(spec)},
        {"condition_count", spec.contains("conditions") && spec["conditions"].is_array() ? spec["conditions"].size() : 0}
    };
    if (spec.contains("presentation") && spec["presentation"].is_object()) {
        out["presentation"] = spec["presentation"];
    }
    return out;
}

json loadRepeatableFilterSpec(const fs::path& root, const std::string& id) {
    const fs::path path = repeatableFilterPath(root, sanitizeRepeatableFilterId(id));
    std::ifstream in(path);
    if (!in) return json();
    json spec;
    in >> spec;
    return spec;
}

std::vector<json> loadAllRepeatableFilterSpecs(const fs::path& root) {
    std::vector<json> specs;
    const fs::path dir = repeatableFilterDir(root);
    std::error_code ec;
    if (!fs::exists(dir, ec) || ec) return specs;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".json") continue;
        try {
            std::ifstream in(entry.path());
            json spec;
            in >> spec;
            specs.push_back(std::move(spec));
        } catch (...) {
        }
    }
    std::sort(specs.begin(), specs.end(), [](const json& a, const json& b) {
        return a.value("id", "") < b.value("id", "");
    });
    return specs;
}

bool saveRepeatableFilterSpec(const fs::path& root, json spec, std::string& error) {
    if (!validateRepeatableFilterSpec(spec, error)) return false;
    const std::string id = sanitizeRepeatableFilterId(spec.value("id", ""));
    spec["id"] = id;
    fs::create_directories(repeatableFilterDir(root));
    const fs::path path = repeatableFilterPath(root, id);
    const fs::path tmp = fs::path(path.string() + ".tmp");
    try {
        std::ofstream out(tmp);
        if (!out) {
            error = "failed to open temp file for write";
            return false;
        }
        out << spec.dump(2) << '\n';
        out.close();
        std::error_code ec;
        fs::rename(tmp, path, ec);
        if (ec) {
            fs::remove(path, ec);
            ec.clear();
            fs::rename(tmp, path, ec);
            if (ec) {
                error = "failed to replace filter definition";
                return false;
            }
        }
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    return true;
}

bool deleteRepeatableFilterSpec(const fs::path& root, const std::string& id) {
    std::error_code ec;
    return fs::remove(repeatableFilterPath(root, sanitizeRepeatableFilterId(id)), ec);
}

json queryExecutionSnapshotJson(const QueryExecutionContextSnapshot& snapshot) {
    json selected_owners = json::array();
    for (const auto& owner : snapshot.selected_owners) selected_owners.push_back(owner);
    json selected_parcel_blocklots = json::array();
    for (const auto& blocklot : snapshot.selected_parcel_blocklots) selected_parcel_blocklots.push_back(blocklot);
    json event_sector_enabled = json::object();
    for (const auto& kv : snapshot.event_sector_enabled) event_sector_enabled[kv.first] = kv.second;
    return {
        {"filter_enabled", snapshot.filter_enabled},
        {"filter_use_date", snapshot.filter_use_date},
        {"filter_year_min", snapshot.filter_year_min},
        {"filter_year_max", snapshot.filter_year_max},
        {"filter_blocklot", snapshot.filter_blocklot},
        {"filter_status", snapshot.filter_status},
        {"filter_address", snapshot.filter_address},
        {"filter_owner", snapshot.filter_owner},
        {"filter_zip", snapshot.filter_zip},
        {"crime", {
            {"enabled", snapshot.crime.enabled},
            {"homicide", snapshot.crime.homicide},
            {"robbery", snapshot.crime.robbery},
            {"assault", snapshot.crime.assault},
            {"burglary", snapshot.crime.burglary},
            {"theft", snapshot.crime.theft},
            {"auto_theft", snapshot.crime.auto_theft},
            {"drug", snapshot.crime.drug},
            {"shooting", snapshot.crime.shooting},
            {"use_year", snapshot.crime.use_year},
            {"year_min", snapshot.crime.year_min},
            {"year_max", snapshot.crime.year_max}
        }},
        {"selected_owners", std::move(selected_owners)},
        {"selected_parcel_blocklots", std::move(selected_parcel_blocklots)},
        {"event_sector_enabled", std::move(event_sector_enabled)},
        {"center_lon", snapshot.center_lon},
        {"center_lat", snapshot.center_lat},
        {"zoom", snapshot.zoom},
        {"map_title_text", snapshot.map_title_text},
        {"map_title_show_primary_parcel_source", snapshot.map_title_show_primary_parcel_source},
        {"map_title_source_layer_file", snapshot.map_title_source_layer_file},
        {"map_title_source_layer_files", snapshot.map_title_source_layer_files}
    };
}

json queryHistoryEntryJson(const QueryHistoryEntry& entry) {
    return {
        {"executed_at_utc", entry.executed_at_utc},
        {"mode", entry.mode},
        {"name", entry.name},
        {"sql", entry.sql},
        {"color", colorJson(entry.color)},
        {"row_count", entry.row_count},
        {"status", entry.status},
        {"snapshot", queryExecutionSnapshotJson(entry.snapshot)}
    };
}

json makeSqlRepeatableFilterSpec(
    const QueryHistoryEntry& entry,
    const std::string& id,
    const std::string& name,
    int version,
    const std::string& source_kind) {
    json spec = {
        {"id", sanitizeRepeatableFilterId(id)},
        {"name", name},
        {"entity", "parcel_property"},
        {"version", version},
        {"execution", {
            {"mode", "sql"},
            {"source", source_kind}
        }},
        {"sql", entry.sql},
        {"presentation", {
            {"name", entry.name},
            {"color", colorJson(entry.color)}
        }},
        {"replay_context", queryHistoryEntryJson(entry)}
    };
    return spec;
}
