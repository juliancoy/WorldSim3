#include "sql_tab.h"

#include "duckdb_analytics.h"
#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
constexpr size_t kQueryBufferSize = 8192;

void copyToQueryBuffer(char (&buffer)[kQueryBufferSize], const std::string& value) {
    std::snprintf(buffer, sizeof(buffer), "%s", value.c_str());
}

void copyLayerColor(const float src[4], float dst[4]) {
    for (int i = 0; i < 4; ++i) dst[i] = src[i];
}
}

void renderSqlTab(
    DuckDbAnalytics& duckdb_analytics,
    const std::vector<LayerDef>& layers,
    const std::vector<UnifiedParcelRecord>& unified_parcels,
    const MapFilterState& map_filter_state,
    const std::vector<DuckDbSelectedParcel>& selected_parcels,
    std::vector<QueryMapLayer>& query_layers) {
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

    if (ImGui::Button("Run As Colored Map Query")) {
        last_result = duckdb_analytics.executeMapQuery(query_sql, map_filter_state.selected_owners, selected_parcels, 1000);
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
    }
    if (!last_result.message.empty()) {
        ImGui::TextWrapped("%s", last_result.message.c_str());
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
