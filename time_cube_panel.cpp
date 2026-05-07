#include "time_cube_panel.h"

#include "feature_props.h"
#include "thread_utils.h"

#include "imgui.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

void drawTimeCubeTab(TimeCubePanelContext& ctx) {
    auto& time_cube_service = *ctx.service;
    auto& layers = *ctx.layers;
    auto& time_cube_ui_result = *ctx.result;
    auto& time_cube_ui_loaded = *ctx.loaded;
    auto& time_cube_ui_status = *ctx.status;
    auto& time_cube_ui_mutex = *ctx.mutex;
    auto& time_cube_ui_worker = *ctx.worker;
    auto& time_cube_ui_running = *ctx.running;
    auto& time_cube_ui_done = *ctx.done;
    auto& time_cube_selected = *ctx.selected;
    auto& time_cube_year_min = *ctx.year_min;
    auto& time_cube_year_max = *ctx.year_max;
    auto& time_cube_normalize_mode = *ctx.normalize_mode;
    auto& time_cube_show_excluded = *ctx.show_excluded;

    if (ImGui::BeginTabItem("Time Cube")) {
        if (time_cube_ui_done.load(std::memory_order_relaxed)) {
            if (time_cube_ui_worker.joinable()) time_cube_ui_worker.join();
            time_cube_ui_done.store(false, std::memory_order_relaxed);
            time_cube_ui_running.store(false, std::memory_order_relaxed);
        }
        auto lightweight_layers_copy = [&]() {
            std::vector<LayerDef> copy;
            copy.reserve(layers.size());
            for (const auto& l : layers) {
                LayerDef row;
                row.name = l.name;
                row.file = l.file;
                row.source_url = l.source_url;
                row.description = l.description;
                row.heatmap_field = l.heatmap_field;
                row.subcategory = l.subcategory;
                row.scale = l.scale;
                row.color = l.color;
                row.enabled = l.enabled;
                row.category = l.category;
                copy.push_back(std::move(row));
            }
            return copy;
        };
        auto start_time_cube_job = [&](bool rebuild) {
            if (time_cube_ui_running.load(std::memory_order_relaxed)) return;
            if (time_cube_ui_worker.joinable()) time_cube_ui_worker.join();
            TimeCubeQuery q;
            q.year_from = 1900;
            q.year_to = 2100;
            q.rebuild = rebuild;
            std::vector<LayerDef> layer_meta = lightweight_layers_copy();
            {
                std::lock_guard<std::mutex> lk(time_cube_ui_mutex);
                time_cube_ui_running.store(true, std::memory_order_relaxed);
                time_cube_ui_done.store(false, std::memory_order_relaxed);
                time_cube_ui_status = rebuild ? "Rebuilding indexes..." : "Refreshing indexes...";
            }
            time_cube_ui_worker = std::thread([&, q, layer_meta = std::move(layer_meta), rebuild]() mutable {
                setCurrentThreadName("ws3-time-cube");
                TimeCubeResult result = time_cube_service.query(layer_meta, q);
                {
                    std::lock_guard<std::mutex> lk(time_cube_ui_mutex);
                    time_cube_ui_result = std::move(result);
                    time_cube_ui_loaded = true;
                    time_cube_ui_status = std::string(rebuild ? "Rebuilt " : "Loaded ") + std::to_string(time_cube_ui_result.datasets.size()) + " dataset indexes";
                    time_cube_ui_done.store(true, std::memory_order_relaxed);
                }
            });
        };
        TimeCubeResult cube_view;
        bool cube_loaded = false;
        bool cube_running = false;
        std::string cube_status;
        {
            std::lock_guard<std::mutex> lk(time_cube_ui_mutex);
            cube_view = time_cube_ui_result;
            cube_loaded = time_cube_ui_loaded;
            cube_running = time_cube_ui_running.load(std::memory_order_relaxed);
            cube_status = time_cube_ui_status;
        }
        ImGui::TextUnformatted("Time-Series Cube");
        ImGui::Separator();
        ImGui::TextWrapped("A time cube standardizes event histories and annual snapshots as dataset x year x grain, with schema-declared measures.");
        ImGui::BeginDisabled(cube_running);
        if (ImGui::Button(cube_loaded ? "Refresh Indexes" : "Build Indexes")) start_time_cube_job(false);
        ImGui::SameLine();
        if (ImGui::Button("Rebuild Indexes")) start_time_cube_job(true);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", cube_status.c_str());
        ImGui::TextDisabled("REST endpoint: http://127.0.0.1:8787/time_cube");
        ImGui::TextDisabled("REST rebuild: /time_cube?rebuild=1");
        if (cube_running) ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(-1, 0), "Index job running");
        if (!cube_loaded) {
            ImGui::Separator();
            ImGui::TextWrapped("Press Build Indexes. The UI remains responsive; cached indexes are stored under data/cache/time_cube and invalidated by dataset and schema signatures.");
        } else {
            size_t recommended_count = 0;
            size_t local_count = 0;
            size_t matched_total = 0;
            int data_year_min = 2101;
            int data_year_max = 1899;
            for (const auto& d : cube_view.datasets) {
                if (d.local) local_count++;
                if (d.recommended) recommended_count++;
                matched_total += d.matched_records;
                for (const auto& yc : d.years) {
                    data_year_min = std::min(data_year_min, yc.first);
                    data_year_max = std::max(data_year_max, yc.first);
                }
            }
            if (data_year_min <= data_year_max) {
                time_cube_year_min = std::clamp(time_cube_year_min, data_year_min, data_year_max);
                time_cube_year_max = std::clamp(time_cube_year_max, data_year_min, data_year_max);
                if (time_cube_year_min > time_cube_year_max) std::swap(time_cube_year_min, time_cube_year_max);
            }
            if (time_cube_selected.size() != cube_view.datasets.size()) time_cube_selected.assign(cube_view.datasets.size(), true);
            if (ImGui::BeginTabBar("time_cube_workspace_tabs")) {
                if (ImGui::BeginTabItem("Overview")) {
                    ImGui::Text("Time-ready datasets: %zu / %zu", recommended_count, cube_view.datasets.size());
                    ImGui::Text("Local datasets: %zu", local_count);
                    ImGui::Text("Matched records: %zu", matched_total);
                    if (data_year_min <= data_year_max) ImGui::Text("Covered years: %d-%d", data_year_min, data_year_max);
                    ImGui::SeparatorText("How to use this");
                    ImGui::TextWrapped("Use Datasets to choose series, Timeline to compare them, and Schema to audit exactly how each dataset is assigned to time.");
                    ImGui::TextWrapped("Use raw counts for operational volume, index-to-100 for trend comparison, and percent-of-max for shape comparison.");
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Timeline")) {
                    if (data_year_min <= data_year_max) {
                        ImGui::SliderInt("Year Min", &time_cube_year_min, data_year_min, data_year_max);
                        ImGui::SliderInt("Year Max", &time_cube_year_max, data_year_min, data_year_max);
                        if (time_cube_year_min > time_cube_year_max) std::swap(time_cube_year_min, time_cube_year_max);
                    }
                    const char* modes[] = {"Raw count/value", "Index first year = 100", "Percent of max"};
                    ImGui::Combo("Normalize", &time_cube_normalize_mode, modes, IM_ARRAYSIZE(modes));
                    ImVec2 plot_pos = ImGui::GetCursorScreenPos();
                    ImVec2 plot_size = ImGui::GetContentRegionAvail();
                    plot_size.y = std::min(plot_size.y, 360.0f);
                    if (plot_size.x < 240.0f) plot_size.x = 240.0f;
                    if (plot_size.y < 220.0f) plot_size.y = 220.0f;
                    ImGui::InvisibleButton("time_cube_timeline_plot", plot_size);
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    dl->AddRectFilled(plot_pos, ImVec2(plot_pos.x + plot_size.x, plot_pos.y + plot_size.y), IM_COL32(248, 249, 246, 255));
                    dl->AddRect(plot_pos, ImVec2(plot_pos.x + plot_size.x, plot_pos.y + plot_size.y), IM_COL32(90, 96, 102, 160));
                    const float left_pad = 54.0f;
                    const float bottom_pad = 28.0f;
                    const float top_pad = 14.0f;
                    const float right_pad = 18.0f;
                    ImVec2 p0(plot_pos.x + left_pad, plot_pos.y + top_pad);
                    ImVec2 p1(plot_pos.x + plot_size.x - right_pad, plot_pos.y + plot_size.y - bottom_pad);
                    dl->AddLine(ImVec2(p0.x, p1.y), p1, IM_COL32(60, 64, 68, 180), 1.0f);
                    dl->AddLine(ImVec2(p0.x, p0.y), ImVec2(p0.x, p1.y), IM_COL32(60, 64, 68, 180), 1.0f);
                    struct PlotSeries { std::string label; ImU32 color; std::vector<std::pair<int, float>> points; };
                    std::vector<PlotSeries> series;
                    float global_max = 1.0f;
                    for (size_t di = 0; di < cube_view.datasets.size(); ++di) {
                        const auto& d = cube_view.datasets[di];
                        if (!d.recommended || di >= time_cube_selected.size() || !time_cube_selected[di]) continue;
                        PlotSeries s;
                        s.label = d.name;
                        s.color = ImGui::ColorConvertFloat4ToU32(colorFromStableKey(d.file));
                        float first = 0.0f;
                        float local_max = 0.0f;
                        for (const auto& yc : d.years) {
                            if (yc.first < time_cube_year_min || yc.first > time_cube_year_max) continue;
                            local_max = std::max(local_max, (float)yc.second);
                            if (first <= 0.0f && yc.second > 0) first = (float)yc.second;
                        }
                        for (const auto& yc : d.years) {
                            if (yc.first < time_cube_year_min || yc.first > time_cube_year_max) continue;
                            float v = (float)yc.second;
                            if (time_cube_normalize_mode == 1 && first > 0.0f) v = (v / first) * 100.0f;
                            if (time_cube_normalize_mode == 2 && local_max > 0.0f) v = (v / local_max) * 100.0f;
                            global_max = std::max(global_max, v);
                            s.points.push_back({yc.first, v});
                        }
                        if (!s.points.empty()) series.push_back(std::move(s));
                    }
                    if (series.empty()) {
                        dl->AddText(ImVec2(p0.x + 12.0f, p0.y + 12.0f), IM_COL32(120, 80, 50, 255), "Select one or more time-ready datasets.");
                    } else {
                        const int span = std::max(1, time_cube_year_max - time_cube_year_min);
                        for (int g = 0; g <= 4; ++g) {
                            float t = (float)g / 4.0f;
                            float y = p1.y - t * (p1.y - p0.y);
                            dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), IM_COL32(120, 126, 132, 45), 1.0f);
                        }
                        for (const auto& s : series) {
                            for (size_t pi = 1; pi < s.points.size(); ++pi) {
                                auto project = [&](std::pair<int, float> pt) {
                                    float x = p0.x + ((float)(pt.first - time_cube_year_min) / (float)span) * (p1.x - p0.x);
                                    float y = p1.y - (pt.second / global_max) * (p1.y - p0.y);
                                    return ImVec2(x, y);
                                };
                                dl->AddLine(project(s.points[pi - 1]), project(s.points[pi]), s.color, 2.0f);
                            }
                        }
                    }
                    ImGui::SeparatorText("Legend");
                    int legend_count = 0;
                    for (size_t di = 0; di < cube_view.datasets.size() && legend_count < 12; ++di) {
                        const auto& d = cube_view.datasets[di];
                        if (!d.recommended || di >= time_cube_selected.size() || !time_cube_selected[di]) continue;
                        ImGui::ColorButton(("##legend" + d.file).c_str(), colorFromStableKey(d.file), ImGuiColorEditFlags_NoTooltip, ImVec2(10, 10));
                        ImGui::SameLine();
                        ImGui::Text("%s (%s)", d.name.c_str(), d.measure.c_str());
                        legend_count++;
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Datasets")) {
                    ImGui::Checkbox("Show excluded datasets", &time_cube_show_excluded);
                    ImGui::SameLine();
                    if (ImGui::Button("Select Time-Ready")) {
                        for (size_t i = 0; i < cube_view.datasets.size() && i < time_cube_selected.size(); ++i) time_cube_selected[i] = cube_view.datasets[i].recommended;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Clear Selection")) {
                        std::fill(time_cube_selected.begin(), time_cube_selected.end(), false);
                    }
                    if (ImGui::BeginTable("time_cube_dataset_table", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0, 360))) {
                        ImGui::TableSetupColumn("On");
                        ImGui::TableSetupColumn("Dataset");
                        ImGui::TableSetupColumn("Mode");
                        ImGui::TableSetupColumn("Grain");
                        ImGui::TableSetupColumn("Records");
                        ImGui::TableSetupColumn("Years");
                        ImGui::TableSetupColumn("Measure");
                        ImGui::TableSetupColumn("State");
                        ImGui::TableHeadersRow();
                        for (size_t di = 0; di < cube_view.datasets.size(); ++di) {
                            const auto& d = cube_view.datasets[di];
                            if (!time_cube_show_excluded && !d.recommended) continue;
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::BeginDisabled(!d.recommended);
                            bool selected = time_cube_selected[di];
                            if (ImGui::Checkbox(("##tcsel" + d.file).c_str(), &selected)) time_cube_selected[di] = selected;
                            ImGui::EndDisabled();
                            ImGui::TableSetColumnIndex(1);
                            ImGui::TextWrapped("%s", d.name.c_str());
                            ImGui::TextDisabled("%s", d.file.c_str());
                            ImGui::TableSetColumnIndex(2);
                            ImGui::TextUnformatted(d.time_mode.empty() ? "-" : d.time_mode.c_str());
                            ImGui::TableSetColumnIndex(3);
                            ImGui::TextUnformatted(d.grain.empty() ? "-" : d.grain.c_str());
                            ImGui::TableSetColumnIndex(4);
                            ImGui::Text("%zu / %zu", d.matched_records, d.feature_count);
                            ImGui::TableSetColumnIndex(5);
                            if (!d.years.empty()) ImGui::Text("%d-%d", d.years.front().first, d.years.back().first);
                            else ImGui::TextDisabled("-");
                            ImGui::TableSetColumnIndex(6);
                            ImGui::TextWrapped("%s", d.measure.empty() ? d.declared_date_field.c_str() : d.measure.c_str());
                            ImGui::TableSetColumnIndex(7);
                            if (d.recommended) ImGui::TextColored(ImVec4(0.20f, 0.62f, 0.25f, 1.0f), "Time-ready");
                            else ImGui::TextWrapped("%s", d.exclusion_reason.empty() ? "Excluded" : d.exclusion_reason.c_str());
                        }
                        ImGui::EndTable();
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Schema")) {
                    ImGui::TextDisabled("Authoritative rules: data/schemas/time_cube_datasets.json");
                    if (ImGui::BeginTable("time_cube_schema_table", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0, 360))) {
                        ImGui::TableSetupColumn("Dataset");
                        ImGui::TableSetupColumn("Mode");
                        ImGui::TableSetupColumn("Date Field");
                        ImGui::TableSetupColumn("Snapshot");
                        ImGui::TableSetupColumn("Grain");
                        ImGui::TableSetupColumn("Measure / Reason");
                        ImGui::TableHeadersRow();
                        for (const auto& d : cube_view.datasets) {
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0); ImGui::TextWrapped("%s", d.file.c_str());
                            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(d.time_mode.empty() ? "-" : d.time_mode.c_str());
                            ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(d.declared_date_field.empty() ? "-" : d.declared_date_field.c_str());
                            ImGui::TableSetColumnIndex(3); if (d.snapshot_year >= 0) ImGui::Text("%d", d.snapshot_year); else ImGui::TextDisabled("-");
                            ImGui::TableSetColumnIndex(4); ImGui::TextUnformatted(d.grain.empty() ? "-" : d.grain.c_str());
                            ImGui::TableSetColumnIndex(5); ImGui::TextWrapped("%s", d.recommended ? d.measure.c_str() : d.exclusion_reason.c_str());
                        }
                        ImGui::EndTable();
                    }
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        ImGui::EndTabItem();
    }
}
