#include "basemap_panel.h"

#include "dataset_library.h"
#include "imgui.h"

#include <atomic>

void drawBasemapPanel(BasemapPanelContext& ctx) {
    if (!ctx.root || !ctx.app_settings || !ctx.basemap_download || !ctx.lazy_tile_download ||
        !ctx.data_library_status_msg || !ctx.basemap_coverage_dirty) {
        return;
    }
    if (!ImGui::CollapsingHeader("Basemap Layers", ImGuiTreeNodeFlags_DefaultOpen)) return;

    const size_t osm_missing = ctx.osm_missing_tiles_cached;
    const size_t osm_total = ctx.osm_total_tiles_cached;
    const size_t topo_missing = ctx.topo_missing_tiles_cached;
    const size_t topo_total = ctx.topo_total_tiles_cached;
    const bool topo_tiles_available = ctx.topo_tiles_available_cached;
    const bool topo_vector_available = ctx.topo_vector_available_cached;
    const bool topo_any_available = topo_tiles_available || topo_vector_available;
    bool osm_enabled_ui = ctx.app_settings->basemap_osm_enabled;
    if (ImGui::Checkbox("OpenStreetMap", &osm_enabled_ui)) {
        ctx.app_settings->basemap_osm_enabled = osm_enabled_ui;
        saveAppSettings(*ctx.root, *ctx.app_settings);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("?##osm_display_settings")) ImGui::OpenPopup("osm_display_settings");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted("Layer display settings");
        ImGui::EndTooltip();
    }
    if (ImGui::BeginPopup("osm_display_settings")) {
        ImGui::TextUnformatted("OpenStreetMap");
        ImGui::Separator();
        if (ImGui::SliderFloat("Opacity##osm", &ctx.app_settings->basemap_osm_opacity, 0.0f, 1.0f, "%.2f")) {
            saveAppSettings(*ctx.root, *ctx.app_settings);
        }
        if (ImGui::Checkbox("Greyscale underlying map##osm", &ctx.app_settings->grayscale_basemap)) {
            saveAppSettings(*ctx.root, *ctx.app_settings);
        }
        ImGui::TextDisabled("Tile source: data/tiles");
        ImGui::TextDisabled("Visible tiles are fetched lazily and cached on disk.");
        ImGui::EndPopup();
    }
    bool topo_enabled_ui = ctx.app_settings->basemap_topographic_enabled;
    if (ImGui::Checkbox("Topographic", &topo_enabled_ui)) {
        ctx.app_settings->basemap_topographic_enabled = topo_enabled_ui;
        saveAppSettings(*ctx.root, *ctx.app_settings);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!topo_any_available);
    if (ImGui::SmallButton("?##topo_display_settings")) ImGui::OpenPopup("topo_display_settings");
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted("Layer display settings");
        ImGui::EndTooltip();
    }
    if (ImGui::BeginPopup("topo_display_settings")) {
        ImGui::TextUnformatted("Topographic");
        ImGui::Separator();
        if (ImGui::SliderFloat("Opacity##topo", &ctx.app_settings->basemap_topographic_opacity, 0.0f, 1.0f, "%.2f")) {
            saveAppSettings(*ctx.root, *ctx.app_settings);
        }
        if (ImGui::Checkbox("Use vector representation##topo_vector", &ctx.app_settings->topo_vector_enabled)) {
            saveAppSettings(*ctx.root, *ctx.app_settings);
        }
        if (ImGui::Checkbox("Greyscale underlying map##topo", &ctx.app_settings->grayscale_basemap)) {
            saveAppSettings(*ctx.root, *ctx.app_settings);
        }
        ImGui::TextDisabled("Tile source: data/tiles_topo");
        ImGui::TextDisabled("Vector source: data/tiles_topo_vector.geojson");
        ImGui::TextDisabled("Raster tiles are fetched lazily and cached on disk.");
        if (!topo_vector_available) {
            if (ImGui::SmallButton("D##topo_vector_geojson")) {
                const std::string url =
                    "https://carto.nationalmap.gov/arcgis/rest/services/contours/MapServer/14/query"
                    "?where=1%3D1&outFields=CONTOURELE&f=geojson"
                    "&geometry=-76.75,39.18,-76.45,39.40"
                    "&geometryType=esriGeometryEnvelope&inSR=4326&spatialRel=esriSpatialRelIntersects";
                std::string err;
                const std::filesystem::path out = *ctx.root / "data" / "tiles_topo_vector.geojson";
                if (downloadUrlToFile(url, out, err)) {
                    *ctx.data_library_status_msg = "Downloaded topographic vector source.";
                    *ctx.basemap_coverage_dirty = true;
                } else {
                    *ctx.data_library_status_msg = std::string("Topographic vector download failed: ") + err;
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted("Download vector contour GeoJSON for the Baltimore area");
                ImGui::EndTooltip();
            }
        }
        ImGui::EndPopup();
    }
    bool satellite_enabled_ui = ctx.app_settings->basemap_satellite_enabled;
    if (ImGui::Checkbox("Satellite", &satellite_enabled_ui)) {
        ctx.app_settings->basemap_satellite_enabled = satellite_enabled_ui;
        saveAppSettings(*ctx.root, *ctx.app_settings);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("?##satellite_display_settings")) ImGui::OpenPopup("satellite_display_settings");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted("Layer display settings");
        ImGui::EndTooltip();
    }
    if (ImGui::BeginPopup("satellite_display_settings")) {
        ImGui::TextUnformatted("Satellite");
        ImGui::Separator();
        if (ImGui::SliderFloat("Opacity##satellite", &ctx.app_settings->basemap_satellite_opacity, 0.0f, 1.0f, "%.2f")) {
            saveAppSettings(*ctx.root, *ctx.app_settings);
        }
        if (ImGui::Checkbox("Greyscale underlying map##satellite", &ctx.app_settings->grayscale_basemap)) {
            saveAppSettings(*ctx.root, *ctx.app_settings);
        }
        ImGui::TextDisabled("Tile source: data/tiles_satellite");
        ImGui::TextDisabled("Raster tiles are fetched lazily up to native z%d and cached on disk.", ctx.max_satellite_native_tile_zoom);
        ImGui::EndPopup();
    }
    if (!topo_tiles_available && !topo_vector_available) {
        ImGui::TextDisabled("Topographic source unavailable: missing data/tiles_topo (or data/tiles_topographic) and data/tiles_topo_vector.geojson.");
        ImGui::TextDisabled("Raster tiles will be fetched lazily; use ? -> D only for vector contours.");
        if (ctx.basemap_download->inflight || !ctx.basemap_download->queue.empty()) {
            ImGui::TextDisabled("A basemap download job is active or queued. See Download Queue window.");
        }
    }
    if (ctx.app_settings->topo_vector_enabled && !topo_vector_available) {
        ImGui::TextDisabled("Topographic vector source unavailable (data/tiles_topo_vector.geojson).");
    }
    if (!ctx.app_settings->basemap_osm_enabled &&
        !ctx.app_settings->basemap_topographic_enabled &&
        !ctx.app_settings->basemap_satellite_enabled) {
        ImGui::TextDisabled("No basemap source enabled.");
    }
    if (osm_total > 0) {
        ImGui::TextDisabled(
            "OSM coverage: %zu/%zu tiles present (z%d-z%d)",
            (size_t)(osm_total - osm_missing),
            osm_total,
            ctx.min_zoom,
            ctx.max_native_tile_zoom);
    }
    if (topo_total > 0) {
        ImGui::TextDisabled(
            "Topo coverage: %zu/%zu tiles present (z%d-z%d)",
            (size_t)(topo_total - topo_missing),
            topo_total,
            ctx.min_zoom,
            ctx.max_native_tile_zoom);
    }
    if (ctx.basemap_download->inflight) {
        ImGui::Separator();
        ImGui::TextDisabled("Basemap download is active. See the Download Queue window.");
    }
    ImGui::TextDisabled(
        "Lazy cache this run: %zu downloaded, %zu queued",
        ctx.lazy_tile_download->downloaded.load(std::memory_order_relaxed),
        ctx.lazy_tile_download->queue.size());
}
