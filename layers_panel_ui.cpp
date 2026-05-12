#include "layers_panel_ui.h"

#include "aggregate_visualization_strategies.h"
#include "app_utils.h"
#include "feature_props.h"
#include "layer_settings.h"
#include "layer_ui_actions.h"

#include "imgui.h"

#include <algorithm>
#include <unordered_map>

namespace {
const char* geographicScopeLabel(const LayerDef& layer) {
    if (layer.scale == "parcel") return "Parcel / land unit";
    if (layer.scale == "building") return "Building / address";
    if (layer.scale == "point") return "Point records";
    if (layer.scale == "tract") return "Census tract";
    if (layer.scale == "csa") return "Community / CSA";
    if (layer.scale == "regional") return "Regional / jurisdiction";
    if (!layer.scale.empty()) return layer.scale.c_str();
    return "Area / boundary";
}

void drawBooleanParameterToggle(LayersPanelUiContext& ctx, const char* label, const char* file) {
    if (!ctx.shared || !ctx.shared->layers) return;
    const int li = findLayerFile(*ctx.shared, file);
    if (li < 0) return;
    bool enabled = (*ctx.shared->layers)[(size_t)li].enabled;
    if (ImGui::Checkbox(label, &enabled)) {
        (*ctx.shared->layers)[(size_t)li].enabled = enabled;
        if (enabled && ctx.shared->enqueue_hydration) ctx.shared->enqueue_hydration((size_t)li, true);
    }
}

void drawParcelParameterPopup(LayersPanelUiContext& ctx) {
    if (!ctx.shared) return;
    if (!ImGui::BeginPopup("parcel_parameter_popup")) return;
    ImGui::TextUnformatted("Parcel Parameters");
    ImGui::SeparatorText("Choropleth");
    const int mode = ctx.shared->parcel_parameter_mode ? *ctx.shared->parcel_parameter_mode : 0;
    if (ImGui::RadioButton("None##parcel_parameter_none", mode == 0)) {
        setParcelParameterMode(*ctx.shared, 0);
    }
    if (ImGui::RadioButton("Area##parcel_parameter_area", mode == 1)) {
        setParcelParameterMode(*ctx.shared, 1);
    }
    if (ImGui::RadioButton("Current value##parcel_parameter_value", mode == 2)) {
        setParcelParameterMode(*ctx.shared, 2);
    }
    if (ctx.shared->layers) {
        for (size_t i = 0; i < ctx.shared->layers->size(); ++i) {
            LayerDef& layer = (*ctx.shared->layers)[i];
            const bool is_parameter_layer = ctx.shared->layer_registry
                ? ctx.shared->layer_registry->isParcelHeatmapLayer(i)
                : (layer.scale == "parcel" && !layer.heatmap_field.empty());
            if (!is_parameter_layer) continue;
            bool selected = layer.enabled && (!ctx.shared->parcel_parameter_mode || *ctx.shared->parcel_parameter_mode == 0);
            std::string label = layer.name + "##parcel_parameter_layer_" + std::to_string(i);
            if (ImGui::RadioButton(label.c_str(), selected)) activateParameterLayer(*ctx.shared, (int)i);
        }
    }

    ImGui::SeparatorText("Boolean Indicators");
    drawBooleanParameterToggle(ctx, "FTA / other notice", "open_notices_other_parcels.geojson");
    drawBooleanParameterToggle(ctx, "Vacant notice", "vacant_building_notices.geojson");
    drawBooleanParameterToggle(ctx, "Rehab", "vacant_building_rehabs.geojson");
    drawBooleanParameterToggle(ctx, "Tax lien", "tax_lien_certificate_sale_properties.geojson");
    drawBooleanParameterToggle(ctx, "Tax sale", "tax_sale_list_2021.geojson");
    drawBooleanParameterToggle(ctx, "Recently rehabbed VBN", "codemap_recently_rehabbed_vbn_parcels.geojson");
    ImGui::EndPopup();
}

void drawCrimeFilters(LayersPanelUiContext& ctx) {
    if (!ctx.shared || !ctx.shared->layers || !ctx.crime_filter_enabled || !ctx.crime_filter_use_year ||
        !ctx.crime_year_min || !ctx.crime_year_max || !ctx.crime_breakdown) {
        return;
    }

    ImGui::SeparatorText("Crime Filters");
    ImGui::Checkbox("Enable Crime Filter", ctx.crime_filter_enabled);
    ImGui::Checkbox("Filter Crime Year", ctx.crime_filter_use_year);
    ImGui::BeginDisabled(!*ctx.crime_filter_use_year);
    ImGui::SliderInt("Crime Year Min", ctx.crime_year_min, 1900, 2100);
    ImGui::SliderInt("Crime Year Max", ctx.crime_year_max, 1900, 2100);
    if (*ctx.crime_year_min > *ctx.crime_year_max) std::swap(*ctx.crime_year_min, *ctx.crime_year_max);
    ImGui::EndDisabled();
    ImGui::Checkbox("Homicide", ctx.crime_filter_homicide); ImGui::SameLine();
    ImGui::Checkbox("Robbery", ctx.crime_filter_robbery);
    ImGui::Checkbox("Assault", ctx.crime_filter_assault); ImGui::SameLine();
    ImGui::Checkbox("Burglary", ctx.crime_filter_burglary);
    ImGui::Checkbox("Theft/Larceny", ctx.crime_filter_theft); ImGui::SameLine();
    ImGui::Checkbox("Auto Theft", ctx.crime_filter_auto_theft);
    ImGui::Checkbox("Drug/Narcotic", ctx.crime_filter_drug); ImGui::SameLine();
    ImGui::Checkbox("Shooting", ctx.crime_filter_shooting);
    if (ImGui::Button("Clear Crime Filters")) {
        *ctx.crime_filter_homicide = false;
        *ctx.crime_filter_robbery = false;
        *ctx.crime_filter_assault = false;
        *ctx.crime_filter_burglary = false;
        *ctx.crime_filter_theft = false;
        *ctx.crime_filter_auto_theft = false;
        *ctx.crime_filter_drug = false;
        *ctx.crime_filter_shooting = false;
        *ctx.crime_filter_use_year = false;
        *ctx.crime_filter_enabled = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh Crime Breakdown")) {
        std::unordered_map<std::string, int> counts;
        auto add_layer_counts = [&](int idx) {
            if (idx < 0 || (size_t)idx >= ctx.shared->layers->size()) return;
            for (const auto& fg : (*ctx.shared->layers)[(size_t)idx].features) {
                const std::string desc = toLowerAscii(getPropertyValue(fg, "Description"));
                const std::string code = toLowerAscii(getPropertyValue(fg, "CrimeCode"));
                const std::string dt = getPropertyValue(fg, "CrimeDateTime");
                if (*ctx.crime_filter_enabled) {
                    if (*ctx.crime_filter_use_year) {
                        int yr = extractYearMaybe(dt);
                        if (yr < 0 || yr < *ctx.crime_year_min || yr > *ctx.crime_year_max) continue;
                    }
                    const bool any_type =
                        *ctx.crime_filter_homicide || *ctx.crime_filter_robbery || *ctx.crime_filter_assault ||
                        *ctx.crime_filter_burglary || *ctx.crime_filter_theft || *ctx.crime_filter_auto_theft ||
                        *ctx.crime_filter_drug || *ctx.crime_filter_shooting;
                    if (any_type) {
                        auto has = [&](const char* s) { return desc.find(s) != std::string::npos || code.find(s) != std::string::npos; };
                        bool ok = false;
                        if (*ctx.crime_filter_homicide && (has("homicide") || has("murder"))) ok = true;
                        if (*ctx.crime_filter_robbery && has("robbery")) ok = true;
                        if (*ctx.crime_filter_assault && (has("assault") || has("aggravated assault"))) ok = true;
                        if (*ctx.crime_filter_burglary && has("burglary")) ok = true;
                        if (*ctx.crime_filter_theft && (has("larceny") || has("theft"))) ok = true;
                        if (*ctx.crime_filter_auto_theft && (has("motor vehicle theft") || has("auto theft") || has("vehicle theft"))) ok = true;
                        if (*ctx.crime_filter_drug && (has("drug") || has("narcotic"))) ok = true;
                        if (*ctx.crime_filter_shooting && has("shooting")) ok = true;
                        if (!ok) continue;
                    }
                }
                std::string label = trimDisplayValue(getPropertyValue(fg, "Description"));
                if (label.empty()) label = trimDisplayValue(getPropertyValue(fg, "CrimeCode"));
                if (label.empty()) label = "(unknown)";
                counts[label] += 1;
            }
        };
        add_layer_counts(ctx.crime_nibrs_layer_idx);
        add_layer_counts(ctx.crime_legacy_layer_idx);
        ctx.crime_breakdown->clear();
        ctx.crime_breakdown->reserve(counts.size());
        for (auto& kv : counts) ctx.crime_breakdown->push_back(kv);
        std::sort(ctx.crime_breakdown->begin(), ctx.crime_breakdown->end(), [](const auto& a, const auto& b) {
            return a.second > b.second;
        });
    }
    ImGui::Text("Breakdown Rows: %zu", ctx.crime_breakdown->size());
}

LayerSettingsPopupContext makeLayerSettingsPopupContext(
    LayersPanelUiContext& ctx,
    size_t idx,
    LayerDef& layer,
    const std::filesystem::path& local_layer_path,
    bool local_layer_exists) {
    LayerSettingsPopupContext settings_ctx;
    settings_ctx.shared = ctx.shared;
    settings_ctx.local_layer_path = local_layer_path;
    settings_ctx.idx = idx;
    settings_ctx.layer = &layer;
    settings_ctx.local_layer_exists = local_layer_exists;
    settings_ctx.zoom = ctx.zoom;
    return settings_ctx;
}

void drawLayerCategory(LayersPanelUiContext& ctx, LayerDef::Category cat, const char* label) {
    if (!ctx.shared || !ctx.shared->layers || !ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen)) return;
    std::string show_id = std::string("Show ") + label;
    std::string hide_id = std::string("Hide ") + label;
    if (ImGui::Button(show_id.c_str())) {
        setCategoryVisible(*ctx.shared, ctx.parcel_layer_idx, cat, true);
    }
    ImGui::SameLine();
    if (ImGui::Button(hide_id.c_str())) {
        setCategoryVisible(*ctx.shared, ctx.parcel_layer_idx, cat, false);
    }

    std::string current_subcategory;
    std::string current_scope;
    for (size_t idx = 0; idx < ctx.shared->layers->size(); ++idx) {
        LayerDef& layer = (*ctx.shared->layers)[idx];
        if (layer.category != cat) continue;
        if (hiddenParcelParameterLayer(*ctx.shared, ctx.parcel_layer_idx, idx)) continue;
        if (layer.subcategory != current_subcategory) {
            current_subcategory = layer.subcategory;
            if (!current_subcategory.empty()) ImGui::SeparatorText(current_subcategory.c_str());
            current_scope.clear();
        }
        const char* scope_label = geographicScopeLabel(layer);
        if (scope_label != current_scope) {
            current_scope = scope_label;
            ImGui::TextDisabled("%s", current_scope.c_str());
        }

        ImGui::PushID((int)idx);
        const std::filesystem::path local_layer_path = ctx.shared->root / "data" / "layers" / layer.file;
        const bool local_layer_exists =
            ctx.shared->local_layer_exists_cache && idx < ctx.shared->local_layer_exists_cache->size()
                ? (*ctx.shared->local_layer_exists_cache)[idx]
                : false;
        if (!local_layer_exists) {
            pushButtonPalette(ButtonPalette::Download);
            const bool can_download = ctx.shared->layer_registry
                ? ctx.shared->layer_registry->canDownload(idx)
                : (!layer.source_url.empty() || !layer.import_type.empty());
            const bool has_source_metadata = ctx.shared->layer_registry
                ? ctx.shared->layer_registry->hasSourceMetadata(idx)
                : (can_download || !layer.reference_url.empty() || !layer.source_urls.empty());
            if (ImGui::SmallButton("D")) {
                if (can_download && ctx.shared->enqueue_layer_download_request) {
                    ctx.shared->enqueue_layer_download_request(idx);
                } else if (ctx.shared->data_library_status_msg) {
                    *ctx.shared->data_library_status_msg = has_source_metadata
                        ? "No direct downloadable GeoJSON URL for " + layer.file + "; see layer tooltip for source URLs."
                        : "No source URL for " + layer.file;
                }
            }
            ImGui::PopStyleColor(buttonPaletteColorCount(ButtonPalette::Download));
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted("Download missing dataset");
                ImGui::TextDisabled(
                    "%s",
                    can_download ? (layer.source_url.empty() ? "Import source available" : "Direct download URL available") :
                    (has_source_metadata ? "Source URLs documented; no direct app download URL" : "No source URL in manifest"));
                ImGui::EndTooltip();
            }
            ImGui::SameLine();
        }

        drawIconToggleButton("show", "V", layer.enabled, "Show layer");
        if (ctx.parcel_layer_idx >= 0 && (int)idx == ctx.parcel_layer_idx) {
            if (ImGui::SmallButton("P")) ImGui::OpenPopup("parcel_parameter_popup");
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted("Parcel parameters");
                ImGui::EndTooltip();
            }
            drawParcelParameterPopup(ctx);
            ImGui::SameLine();
        }
        if (ImGui::SmallButton("?")) ImGui::OpenPopup("layer_display_settings");
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Layer display settings");
            ImGui::EndTooltip();
        }

        LayerSettingsPopupContext settings_ctx =
            makeLayerSettingsPopupContext(ctx, idx, layer, local_layer_path, local_layer_exists);
        drawLayerDisplaySettingsPopup(settings_ctx);

        ImGui::SameLine();
        drawLayerNameBadge(layer.name, layer.color);
        const bool row_hovered = ImGui::IsItemHovered();
        ImGui::SameLine();
        LayerRuntimeState st;
        if (ctx.shared->status_mutex && ctx.shared->layer_states) {
            std::lock_guard<std::mutex> lk(*ctx.shared->status_mutex);
            if (idx < ctx.shared->layer_states->size()) st = (*ctx.shared->layer_states)[idx];
        }
        if (st.status == LayerPipelineStatus::Failed) {
            ImGui::TextColored(ImVec4(0.85f, 0.35f, 0.2f, 1.0f), "[%s]", statusToString(st.status));
        } else {
            ImGui::TextDisabled("[%s | %zu]", statusToString(st.status), st.feature_count);
        }
        const bool status_hovered = ImGui::IsItemHovered();
        if (row_hovered || status_hovered) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(layer.name.c_str());
            ImGui::Separator();
            ImGui::Text("Category: %s", categoryToString(layer.category));
            ImGui::Text("Status: %s", statusToString(st.status));
            ImGui::Text("Features: %zu", st.feature_count);
            ImGui::Text("File: %s", layer.file.c_str());
            ImGui::Text("Local: %s", local_layer_exists ? "yes" : "no");
            if (!layer.subcategory.empty()) ImGui::Text("Subcategory: %s", layer.subcategory.c_str());
            if (!layer.scale.empty()) ImGui::Text("Scale: %s", layer.scale.c_str());
            if (!layer.heatmap_field.empty()) ImGui::Text("Heatmap Field: %s", layer.heatmap_field.c_str());
            if (!layer.description.empty()) ImGui::TextWrapped("Description: %s", layer.description.c_str());
            if (!layer.source_url.empty()) ImGui::TextWrapped("Download URL: %s", layer.source_url.c_str());
            if (!layer.reference_url.empty()) ImGui::TextWrapped("Reference: %s", layer.reference_url.c_str());
            if (!layer.source_urls.empty()) {
                ImGui::SeparatorText("Source URLs");
                for (const auto& url : layer.source_urls) ImGui::TextWrapped("%s", url.c_str());
            }
            if (!st.error.empty()) {
                ImGui::Separator();
                ImGui::TextColored(ImVec4(0.85f, 0.35f, 0.2f, 1.0f), "Error: %s", st.error.c_str());
            }
            ImGui::EndTooltip();
        }
        ImGui::PopID();
    }

    if (cat == LayerDef::Category::Safety) drawCrimeFilters(ctx);
}
}

void drawLayerCategoriesPanel(LayersPanelUiContext& ctx) {
    drawLayerCategory(ctx, LayerDef::Category::Housing, "Housing");
    drawLayerCategory(ctx, LayerDef::Category::PublicHealth, "Public Health");
    drawLayerCategory(ctx, LayerDef::Category::Safety, "Safety");
    drawLayerCategory(ctx, LayerDef::Category::Infrastructure, "Infrastructure");
    drawLayerCategory(ctx, LayerDef::Category::Zoning, "Zoning");
}
