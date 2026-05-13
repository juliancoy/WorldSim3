#include "data_library_panel.h"

#include "app_utils.h"
#include "ui_primitives.h"

void updateDataLibraryVisibleRows(DataLibrarySearchCacheContext& ctx) {
    if (!ctx.layers || !ctx.query_buffer || !ctx.cached_query || !ctx.cached_layer_count ||
        !ctx.visible_rows || !ctx.cache_rebuilds) {
        return;
    }

    const std::string query = trimDisplayValue(ctx.query_buffer);
    if (query == *ctx.cached_query && *ctx.cached_layer_count == ctx.layers->size()) return;

    *ctx.cached_query = query;
    *ctx.cached_layer_count = ctx.layers->size();
    ctx.visible_rows->clear();
    ctx.visible_rows->reserve(ctx.layers->size());
    for (size_t i = 0; i < ctx.layers->size(); ++i) {
        const auto& layer = (*ctx.layers)[i];
        const bool hit =
            query.empty() ||
            containsCaseInsensitive(layer.name, query) ||
            containsCaseInsensitive(layer.file, query) ||
            containsCaseInsensitive(categoryToString(layer.category), query) ||
            containsCaseInsensitive(layer.subcategory, query) ||
            containsCaseInsensitive(layer.description, query);
        if (hit) ctx.visible_rows->push_back(i);
    }
    (*ctx.cache_rebuilds)++;
}

void drawDataLibraryCell(const std::string& value, size_t max_chars) {
    const std::string full = value.empty() ? std::string("-") : value;
    std::string shown = full;
    if (shown.size() > max_chars && max_chars > 3) shown = shown.substr(0, max_chars - 3) + "...";
    ImGui::TextUnformatted(shown.c_str());
    if (shown.size() != full.size() && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 80.0f);
        ImGui::TextUnformatted(full.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void drawDataLibraryTooltip(const LayerDef& layer) {
    if (!ImGui::IsItemHovered()) return;
    if (layer.description.empty() && layer.source_url.empty() && layer.reference_url.empty() && layer.source_urls.empty()) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 90.0f);
    if (!layer.description.empty()) ImGui::TextUnformatted(layer.description.c_str());
    if (!layer.description.empty() && (!layer.source_url.empty() || !layer.reference_url.empty() || !layer.source_urls.empty())) ImGui::Separator();
    if (!layer.source_url.empty()) ImGui::Text("Download URL: %s", layer.source_url.c_str());
    if (!layer.reference_url.empty()) ImGui::Text("Reference: %s", layer.reference_url.c_str());
    for (const auto& url : layer.source_urls) ImGui::Text("Source: %s", url.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

const char* dataFreshnessLabel(FreshnessState state) {
    switch (state) {
        case FreshnessState::UpToDate: return "Up-to-date";
        case FreshnessState::UpdateAvailable: return "Update available";
        case FreshnessState::NotTrackable: return "Not trackable";
        case FreshnessState::Error: return "Check error";
        case FreshnessState::Unknown: default: return "Unknown";
    }
}

ImVec4 dataFreshnessColor(FreshnessState state) {
    switch (state) {
        case FreshnessState::UpToDate: return ImVec4(0.20f, 0.62f, 0.25f, 1.0f);
        case FreshnessState::UpdateAvailable: return ImVec4(0.80f, 0.50f, 0.10f, 1.0f);
        case FreshnessState::NotTrackable: return ImVec4(0.45f, 0.45f, 0.45f, 1.0f);
        case FreshnessState::Error: return ImVec4(0.75f, 0.22f, 0.16f, 1.0f);
        case FreshnessState::Unknown: default: return ImVec4(0.30f, 0.45f, 0.72f, 1.0f);
    }
}

void drawDataLibraryWindow(DataLibraryUiContext& ctx) {
    if (!ctx.root || !ctx.layers || !ctx.coordinator || !ctx.show_data_library || !ctx.query_buffer ||
        !ctx.download_phase || !ctx.include_large || !ctx.cached_query || !ctx.cached_layer_count ||
        !ctx.visible_rows || !ctx.cache_rebuilds || !ctx.rendered_rows_last) {
        return;
    }

    if (!*ctx.show_data_library) return;

    ImGui::SetNextWindowSize(ImVec2(860, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Data Library", ctx.show_data_library, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    const char* download_phases[] = {"must-have", "nice-to-have", "heavy-data", "all", "capital-flows"};
    finalizeDataLibraryBulkDownloadIfReady(*ctx.coordinator);
    ImGui::InputTextWithHint("##data_library_query", "Search by name, file, category, subcategory...", ctx.query_buffer, ctx.query_buffer_size);
    ImGui::SameLine();
    if (ImGui::Button("Clear")) ctx.query_buffer[0] = '\0';
    ImGui::SameLine();
    if (ImGui::Button("Check All Updates")) {
        checkAllDataLibraryUpdates(*ctx.coordinator);
    }
    ImGui::Separator();
    ImGui::SetNextItemWidth(150.0f);
    ImGui::Combo("Download phase", ctx.download_phase, download_phases, IM_ARRAYSIZE(download_phases));
    ImGui::SameLine();
    ImGui::Checkbox("Include large", ctx.include_large);
    ImGui::SameLine();
    if (ImGui::Button("Rescan Local Files")) {
        rescanDataLibraryLocalFiles(*ctx.coordinator);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(ctx.queueable_missing_layer_count == 0);
    if (ImGui::Button("Download All##data_library") && ctx.queue_all_missing_layer_downloads) {
        ctx.queue_all_missing_layer_downloads();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::BeginTooltip();
        ImGui::Text("Queue all missing layer datasets with source URLs");
        ImGui::TextDisabled("Missing downloadable: %zu", ctx.downloadable_missing_layer_count);
        ImGui::TextDisabled("Queueable now: %zu", ctx.queueable_missing_layer_count);
        ImGui::EndTooltip();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(ctx.coordinator->data_library_bulk_inflight && *ctx.coordinator->data_library_bulk_inflight);
    if (ImGui::Button("Download Phase")) {
        startDataLibraryPhaseDownload(*ctx.coordinator, download_phases[*ctx.download_phase], *ctx.include_large);
    }
    ImGui::EndDisabled();
    if (ctx.coordinator->data_library_bulk_inflight && *ctx.coordinator->data_library_bulk_inflight) {
        std::string progress;
        {
            std::lock_guard<std::mutex> lk(*ctx.coordinator->data_library_bulk_mutex);
            progress = *ctx.coordinator->data_library_bulk_progress;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", progress.empty() ? "Downloading..." : progress.c_str());
    }

    size_t downloaded_count = 0;
    for (size_t i = 0; i < ctx.coordinator->local_layer_exists_cache->size(); ++i) {
        if ((*ctx.coordinator->local_layer_exists_cache)[i]) downloaded_count++;
    }
    ImGui::Text("Downloaded: %zu / %zu", downloaded_count, ctx.layers->size());
    ImGui::TextDisabled("Version metadata: data/versions/metadata | snapshots: data/versions/snapshots | diffs: data/versions/diffs");
    if (ctx.coordinator->data_library_status_msg && !ctx.coordinator->data_library_status_msg->empty()) {
        ImGui::TextWrapped("%s", ctx.coordinator->data_library_status_msg->c_str());
    }
    ImGui::Separator();

    DataLibrarySearchCacheContext data_library_search_ctx;
    data_library_search_ctx.layers = ctx.layers;
    data_library_search_ctx.query_buffer = ctx.query_buffer;
    data_library_search_ctx.cached_query = ctx.cached_query;
    data_library_search_ctx.cached_layer_count = ctx.cached_layer_count;
    data_library_search_ctx.visible_rows = ctx.visible_rows;
    data_library_search_ctx.cache_rebuilds = ctx.cache_rebuilds;
    updateDataLibraryVisibleRows(data_library_search_ctx);

    *ctx.rendered_rows_last = 0;
    if (ImGui::BeginTable("data_library_table", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0, 0))) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 94.0f);
        ImGui::TableSetupColumn("Freshness", ImGuiTableColumnFlags_WidthFixed, 142.0f);
        ImGui::TableSetupColumn("Enabled", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("File");
        ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Subcategory");
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(ctx.visible_rows->size()));
        while (clipper.Step()) {
            for (int display_index = clipper.DisplayStart; display_index < clipper.DisplayEnd; ++display_index) {
                const size_t i = (*ctx.visible_rows)[static_cast<size_t>(display_index)];
                auto& layer = (*ctx.layers)[i];
                const std::filesystem::path local_path = *ctx.root / "data" / "layers" / layer.file;
                const bool local_exists =
                    i < ctx.coordinator->local_layer_exists_cache->size() ? (*ctx.coordinator->local_layer_exists_cache)[i] : false;
                (*ctx.rendered_rows_last)++;
                ImGui::PushID((int)i);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (!local_exists) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.22f, 0.12f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.30f, 0.14f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.46f, 0.18f, 0.08f, 1.0f));
                    if (ImGui::SmallButton("Download")) {
                        if (layer.source_url.empty()) {
                            *ctx.coordinator->data_library_status_msg = "No source URL for " + layer.file;
                        } else if (ctx.enqueue_layer_download_request) {
                            ctx.enqueue_layer_download_request(i);
                        }
                    }
                    ImGui::PopStyleColor(3);
                } else if (!layer.source_url.empty()) {
                    if ((*ctx.coordinator->data_freshness_state)[i] == FreshnessState::UpdateAvailable) {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.74f, 0.44f, 0.12f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.88f, 0.53f, 0.14f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.62f, 0.36f, 0.10f, 1.0f));
                        if (ImGui::SmallButton("Update") && ctx.enqueue_layer_download_request) {
                            ctx.enqueue_layer_download_request(i);
                        }
                        ImGui::PopStyleColor(3);
                    } else if (ImGui::SmallButton("Check")) {
                        FreshnessCheckResult cr = checkUrlFreshnessVersioned(layer.source_url, local_path, *ctx.root / "data" / "versions");
                        (*ctx.coordinator->data_freshness_state)[i] = cr.state;
                        (*ctx.coordinator->data_freshness_msg)[i] = cr.message;
                        *ctx.coordinator->data_library_status_msg = "Checked " + layer.file + ": " + cr.message;
                    }
                } else {
                    ImGui::TextDisabled("-");
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::ColorButton("##freshness_dot", dataFreshnessColor((*ctx.coordinator->data_freshness_state)[i]), ImGuiColorEditFlags_NoTooltip, ImVec2(10, 10));
                ImGui::SameLine();
                ImGui::TextDisabled("%s", dataFreshnessLabel((*ctx.coordinator->data_freshness_state)[i]));
                if (!(*ctx.coordinator->data_freshness_msg)[i].empty() && ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted((*ctx.coordinator->data_freshness_msg)[i].c_str());
                    ImGui::EndTooltip();
                }
                ImGui::TableSetColumnIndex(2);
                ImGui::Checkbox("##enable", &layer.enabled);
                ImGui::TableSetColumnIndex(3);
                drawLayerNameBadge(layer.name, layer.color, 72);
                drawDataLibraryTooltip(layer);
                ImGui::TableSetColumnIndex(4);
                drawDataLibraryCell(layer.file, 72);
                ImGui::TableSetColumnIndex(5);
                drawDataLibraryCell(categoryToString(layer.category), 32);
                ImGui::TableSetColumnIndex(6);
                drawDataLibraryCell(layer.subcategory, 56);
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled(
        "Matched rows: %zu | Rendered this frame: %zu | Cache rebuilds: %zu",
        ctx.visible_rows->size(),
        *ctx.rendered_rows_last,
        *ctx.cache_rebuilds);
    ImGui::End();
}
