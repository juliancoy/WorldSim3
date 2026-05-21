#include "sql_tab.h"

#include "app_utils.h"
#include "duckdb_analytics.h"
#include "layer_state_io.h"
#include "imgui.h"
#include "repeatable_filters.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sstream>
#include <string>

using json = nlohmann::json;

namespace {
constexpr size_t kQueryBufferSize = 8192;
constexpr size_t kMaxQueryHistoryEntries = 100;

void copyToQueryBuffer(char (&buffer)[kQueryBufferSize], const std::string& value) {
    std::snprintf(buffer, sizeof(buffer), "%s", value.c_str());
}

void copyLayerColor(const float src[4], float dst[4]) {
    for (int i = 0; i < 4; ++i) dst[i] = src[i];
}

std::string currentUtcTimestampIso8601() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time_now = std::chrono::system_clock::to_time_t(now);
    std::tm utc_tm{};
#if defined(_WIN32)
    gmtime_s(&utc_tm, &time_now);
#else
    gmtime_r(&time_now, &utc_tm);
#endif
    char buffer[32];
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc_tm) == 0) return {};
    return buffer;
}

QueryExecutionContextSnapshot makeQuerySnapshot(
    const MapFilterState& map_filter_state,
    const AppSettings& app_settings,
    double center_lon,
    double center_lat,
    double zoom,
    const std::vector<DuckDbSelectedParcel>& selected_parcels) {
    QueryExecutionContextSnapshot snapshot;
    snapshot.filter_enabled = map_filter_state.enabled;
    snapshot.filter_use_date = map_filter_state.use_date;
    snapshot.filter_year_min = map_filter_state.year_min;
    snapshot.filter_year_max = map_filter_state.year_max;
    snapshot.filter_blocklot = map_filter_state.blocklot;
    snapshot.filter_status = map_filter_state.status;
    snapshot.filter_address = map_filter_state.address;
    snapshot.filter_owner = map_filter_state.owner;
    snapshot.filter_zip = map_filter_state.zip;
    snapshot.crime = map_filter_state.crime;
    snapshot.selected_owners.assign(map_filter_state.selected_owners.begin(), map_filter_state.selected_owners.end());
    std::sort(snapshot.selected_owners.begin(), snapshot.selected_owners.end());
    snapshot.selected_parcel_blocklots.reserve(selected_parcels.size());
    for (const auto& parcel : selected_parcels) {
        if (!parcel.blocklot.empty()) snapshot.selected_parcel_blocklots.push_back(parcel.blocklot);
    }
    std::sort(snapshot.selected_parcel_blocklots.begin(), snapshot.selected_parcel_blocklots.end());
    snapshot.selected_parcel_blocklots.erase(
        std::unique(snapshot.selected_parcel_blocklots.begin(), snapshot.selected_parcel_blocklots.end()),
        snapshot.selected_parcel_blocklots.end());
    snapshot.event_sector_enabled = map_filter_state.event_sector_enabled;
    snapshot.center_lon = center_lon;
    snapshot.center_lat = center_lat;
    snapshot.zoom = zoom;
    snapshot.map_title_text = app_settings.map_title_text;
    snapshot.map_title_show_primary_parcel_source = app_settings.map_title_show_primary_parcel_source;
    return snapshot;
}

void appendQueryHistoryEntry(
    const std::filesystem::path& root,
    std::vector<QueryHistoryEntry>& query_history,
    const char* query_name,
    const char* query_sql,
    const float query_color[4],
    const char* mode,
    const DuckDbQueryResult& result,
    const QueryExecutionContextSnapshot& snapshot) {
    QueryHistoryEntry entry;
    entry.executed_at_utc = currentUtcTimestampIso8601();
    entry.mode = mode ? mode : "";
    entry.name = query_name ? query_name : "";
    entry.sql = query_sql ? query_sql : "";
    copyLayerColor(query_color, entry.color);
    entry.row_count = result.rows.size();
    entry.status = result.message;
    entry.snapshot = snapshot;
    query_history.push_back(std::move(entry));
    if (query_history.size() > kMaxQueryHistoryEntries) {
        query_history.erase(query_history.begin(), query_history.begin() + (ptrdiff_t)(query_history.size() - kMaxQueryHistoryEntries));
    }
    saveQueryHistoryUiState(root, query_history);
}

std::string suggestedRepeatableFilterId(const std::string& raw_name, const std::string& fallback_prefix) {
    std::string candidate;
    candidate.reserve(raw_name.size());
    for (char c : raw_name) {
        if (std::isalnum((unsigned char)c)) candidate.push_back((char)std::tolower((unsigned char)c));
        else if (c == ' ' || c == '-' || c == '.') candidate.push_back('_');
    }
    candidate = sanitizeRepeatableFilterId(candidate);
    if (candidate.empty()) candidate = fallback_prefix;
    return candidate;
}
}

void renderSqlTab(
    DuckDbAnalytics& duckdb_analytics,
    const std::vector<LayerDef>& layers,
    const std::vector<UnifiedParcelRecord>& unified_parcels,
    const MapFilterState& map_filter_state,
    const AppSettings& app_settings,
    const std::filesystem::path& root,
    double center_lon,
    double center_lat,
    double zoom,
    const std::vector<DuckDbSelectedParcel>& selected_parcels,
    std::vector<QueryMapLayer>& query_layers,
    std::vector<QueryHistoryEntry>& query_history) {
    static char query_name[96] = "Query 1";
    static char query_sql[kQueryBufferSize] =
        "SELECT parcel_layer_idx AS layer_idx, parcel_feature_idx AS feature_idx, blocklot, owner, address, current_value\n"
        "FROM unified_parcels\n"
        "WHERE owner IN (SELECT owner FROM ui_selected_owners)\n"
        "   OR blocklot IN (SELECT blocklot FROM ui_selected_parcels)\n"
        "LIMIT 5000;";
    static float query_color[4] = {1.0f, 0.48f, 0.08f, 1.0f};
    static int selected_query = -1;
    static DuckDbQueryResult last_result;
    static char save_filter_id[96] = "";
    static char save_filter_name[128] = "";
    static int save_filter_version = 1;
    static std::string save_filter_status;

    const DuckDbAnalyticsStatus& db_status = duckdb_analytics.status();
    ImGui::TextWrapped("Database: %s", db_status.db_path.c_str());
    ImGui::TextWrapped("%s", db_status.message.c_str());
    ImGui::Text("Hydrated layers indexed: %zu", db_status.layer_count);
    ImGui::Text("Features indexed: %zu", db_status.feature_count);
    if (ImGui::Button("Rebuild DuckDB Cache")) {
        duckdb_analytics.rebuild(layers, unified_parcels);
    }

    ImGui::SeparatorText("Map Query Entrypoint");
    ImGui::TextWrapped("Queries become colored map layers when they return layer_idx + feature_idx, blocklot, or owner.");
    ImGui::Text("Selected owners exposed to SQL: %zu", map_filter_state.selected_owners.size());
    ImGui::Text("Selected parcels exposed to SQL: %zu", selected_parcels.size());
    ImGui::TextDisabled("Use ui_selected_owners in SQL to query the current Owners-tab selection.");
    ImGui::TextDisabled("Use ui_selected_parcels in SQL to query the active Parcel Info selection.");
    ImGui::InputText("Name", query_name, sizeof(query_name));
    ImGui::ColorEdit4("Map Color", query_color, ImGuiColorEditFlags_NoInputs);
    ImGui::InputTextMultiline(
        "SQL",
        query_sql,
        sizeof(query_sql),
        ImVec2(-FLT_MIN, 150.0f),
        ImGuiInputTextFlags_AllowTabInput);

    const QueryExecutionContextSnapshot snapshot = makeQuerySnapshot(
        map_filter_state,
        app_settings,
        center_lon,
        center_lat,
        zoom,
        selected_parcels);

    if (ImGui::Button("Run As Colored Map Query")) {
        last_result = duckdb_analytics.executeMapQuery(query_sql, map_filter_state.selected_owners, selected_parcels, 1000);
        appendQueryHistoryEntry(root, query_history, query_name, query_sql, query_color, "map_layer", last_result, snapshot);
        if (last_result.ok) {
            QueryMapLayer layer;
            layer.enabled = true;
            layer.name = query_name[0] ? query_name : ("Query " + std::to_string(query_layers.size() + 1));
            layer.sql = query_sql;
            copyLayerColor(query_color, layer.color);
            layer.result_set = std::move(last_result.result_set);
            layer.row_count = last_result.rows.size();
            layer.status = last_result.message;
            query_layers.push_back(std::move(layer));
            selected_query = (int)query_layers.size() - 1;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Run Preview Only")) {
        last_result = duckdb_analytics.executeMapQuery(query_sql, map_filter_state.selected_owners, selected_parcels, 1000);
        appendQueryHistoryEntry(root, query_history, query_name, query_sql, query_color, "preview", last_result, snapshot);
    }
    if (!last_result.message.empty()) {
        ImGui::TextWrapped("%s", last_result.message.c_str());
    }

    ImGui::SeparatorText("Save As Repeatable Filter");
    if (save_filter_name[0] == '\0') {
        std::snprintf(save_filter_name, sizeof(save_filter_name), "%s", query_name);
    }
    if (save_filter_id[0] == '\0') {
        const std::string suggested_id = suggestedRepeatableFilterId(
            query_name[0] ? query_name : "sql_query",
            "sql_query");
        std::snprintf(save_filter_id, sizeof(save_filter_id), "%s", suggested_id.c_str());
    }
    ImGui::InputText("Filter ID", save_filter_id, sizeof(save_filter_id));
    ImGui::InputText("Filter Name", save_filter_name, sizeof(save_filter_name));
    ImGui::InputInt("Filter Version", &save_filter_version);
    if (ImGui::Button("Save Query As Repeatable Filter")) {
        QueryHistoryEntry entry;
        entry.executed_at_utc = currentUtcTimestampIso8601();
        entry.mode = "sql_tab";
        entry.name = query_name;
        entry.sql = query_sql;
        copyLayerColor(query_color, entry.color);
        entry.row_count = last_result.rows.size();
        entry.status = last_result.message;
        entry.snapshot = snapshot;
        std::string error;
        const json spec = makeSqlRepeatableFilterSpec(
            entry,
            save_filter_id,
            save_filter_name,
            std::max(save_filter_version, 1),
            "sql_tab");
        if (saveRepeatableFilterSpec(root, spec, error)) {
            save_filter_status = "Saved " + sanitizeRepeatableFilterId(save_filter_id);
        } else {
            save_filter_status = "Save failed: " + error;
        }
    }
    if (!save_filter_status.empty()) {
        ImGui::TextWrapped("%s", save_filter_status.c_str());
    }

    ImGui::SeparatorText("Active Query Layers");
    if (query_layers.empty()) {
        ImGui::TextDisabled("No query layers yet.");
    } else {
        for (size_t i = 0; i < query_layers.size(); ++i) {
            QueryMapLayer& layer = query_layers[i];
            ImGui::PushID((int)i);
            ImGui::Checkbox("##enabled", &layer.enabled);
            ImGui::SameLine();
            ImGui::ColorButton("##color", ImVec4(layer.color[0], layer.color[1], layer.color[2], layer.color[3]));
            ImGui::SameLine();
            if (ImGui::Selectable(layer.name.c_str(), selected_query == (int)i)) {
                selected_query = (int)i;
                std::snprintf(query_name, sizeof(query_name), "%s", layer.name.c_str());
                copyToQueryBuffer(query_sql, layer.sql);
                copyLayerColor(layer.color, query_color);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) {
                query_layers.erase(query_layers.begin() + (ptrdiff_t)i);
                if (selected_query >= (int)query_layers.size()) selected_query = (int)query_layers.size() - 1;
                ImGui::PopID();
                break;
            }
            ImGui::TextDisabled("%s", layer.status.c_str());
            ImGui::PopID();
        }
        if (ImGui::Button("Clear Query Layers")) {
            query_layers.clear();
            selected_query = -1;
        }
    }

    if (selected_query >= 0 && selected_query < (int)query_layers.size()) {
        QueryMapLayer& layer = query_layers[(size_t)selected_query];
        ImGui::SeparatorText("Selected Query Layer");
        if (ImGui::InputText("Layer Name", query_name, sizeof(query_name))) layer.name = query_name;
        if (ImGui::ColorEdit4("Layer Color", query_color, ImGuiColorEditFlags_NoInputs)) {
            copyLayerColor(query_color, layer.color);
        }
        if (ImGui::Button("Re-run Selected Query")) {
            last_result = duckdb_analytics.executeMapQuery(layer.sql, map_filter_state.selected_owners, selected_parcels, 1000);
            if (last_result.ok) {
                layer.result_set = std::move(last_result.result_set);
                layer.row_count = last_result.rows.size();
                layer.status = last_result.message;
            } else {
                layer.status = last_result.message;
            }
        }
    }

    ImGui::SeparatorText("Preview Rows");
    if (!last_result.columns.empty()) {
        const int column_count = std::min<int>((int)last_result.columns.size(), 8);
        if (ImGui::BeginTable("sql_preview_rows", column_count, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 180))) {
            for (int c = 0; c < column_count; ++c) ImGui::TableSetupColumn(last_result.columns[(size_t)c].c_str());
            ImGui::TableHeadersRow();
            for (const auto& row : last_result.rows) {
                ImGui::TableNextRow();
                for (int c = 0; c < column_count; ++c) {
                    ImGui::TableSetColumnIndex(c);
                    ImGui::TextUnformatted(c < (int)row.size() ? row[(size_t)c].c_str() : "");
                }
            }
            ImGui::EndTable();
        }
    } else {
        ImGui::TextDisabled("No preview rows yet.");
    }

    ImGui::SeparatorText("Available Views");
    ImGui::TextDisabled("layer_features");
    ImGui::TextDisabled("parcel_features");
    ImGui::TextDisabled("unified_parcels");
    ImGui::TextDisabled("owner_rollups");
    ImGui::TextDisabled("layer_counts");
    ImGui::SeparatorText("Example Queries");
    if (ImGui::SmallButton("Selected owners parcels")) {
        copyToQueryBuffer(query_sql,
            "SELECT parcel_layer_idx AS layer_idx, parcel_feature_idx AS feature_idx, blocklot, owner, address, current_value\n"
            "FROM unified_parcels\n"
            "WHERE owner IN (SELECT owner FROM ui_selected_owners)\n"
            "   OR blocklot IN (SELECT blocklot FROM ui_selected_parcels)\n"
            "LIMIT 5000;");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Vacant by status")) {
        copyToQueryBuffer(query_sql,
            "SELECT parcel_layer_idx AS layer_idx, parcel_feature_idx AS feature_idx, blocklot, owner, address, status, current_value\n"
            "FROM unified_parcels\n"
            "WHERE vacant_notice_count > 0 OR vacant_rehab_count > 0\n"
            "LIMIT 5000;");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Owner rollups")) {
        copyToQueryBuffer(query_sql, "SELECT * FROM owner_rollups LIMIT 25;");
    }
}

void drawSqlTab(
    DuckDbAnalytics& duckdb_analytics,
    const std::vector<LayerDef>& layers,
    const std::vector<UnifiedParcelRecord>& unified_parcels,
    const MapFilterState& map_filter_state,
    const AppSettings& app_settings,
    const std::filesystem::path& root,
    double center_lon,
    double center_lat,
    double zoom,
    const std::vector<size_t>& selected_parcel_indices,
    bool show_selected_parcel_details,
    int parcel_layer_idx,
    size_t selected_parcel_idx,
    std::vector<QueryMapLayer>& query_layers,
    std::vector<QueryHistoryEntry>& query_history) {
    if (!ImGui::BeginTabItem("SQL")) return;

    std::vector<DuckDbSelectedParcel> sql_selected_parcels;
    const bool sql_selected_parcel_valid =
        show_selected_parcel_details && !selected_parcel_indices.empty() && parcel_layer_idx >= 0 &&
        (size_t)parcel_layer_idx < layers.size() &&
        selected_parcel_idx < layers[(size_t)parcel_layer_idx].features.size();
    if (sql_selected_parcel_valid) {
        for (size_t sel_idx : selected_parcel_indices) {
            if (sel_idx >= layers[(size_t)parcel_layer_idx].features.size()) continue;
            const auto& selected = layers[(size_t)parcel_layer_idx].features[sel_idx];
            sql_selected_parcels.push_back(DuckDbSelectedParcel{
                (size_t)parcel_layer_idx,
                sel_idx,
                std::string(),
                featureBlockLotJoinKey(selected)
            });
        }
    }
    renderSqlTab(
        duckdb_analytics,
        layers,
        unified_parcels,
        map_filter_state,
        app_settings,
        root,
        center_lon,
        center_lat,
        zoom,
        sql_selected_parcels,
        query_layers,
        query_history);
    ImGui::EndTabItem();
}
