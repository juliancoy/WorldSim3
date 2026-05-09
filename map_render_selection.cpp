#include "map_render_selection.h"

#include "geo.h"

void renderSelectedParcelOutlines(const MapSelectionRenderContext& ctx) {
    if (!ctx.draw || !ctx.layers || !ctx.selected_parcel_indices || !ctx.projection || !ctx.project_world) return;
    if (ctx.parcel_layer_idx < 0 || (size_t)ctx.parcel_layer_idx >= ctx.layers->size()) return;
    if (ctx.selected_parcel_indices->empty()) return;

    const ImU32 selected_outline = IM_COL32(255, 230, 0, 255);
    const auto& parcel_layer = (*ctx.layers)[(size_t)ctx.parcel_layer_idx];
    for (size_t idx : *ctx.selected_parcel_indices) {
        if (idx >= parcel_layer.features.size()) continue;
        const auto& fg = parcel_layer.features[idx];
        if (!fg.rings.empty()) {
            const auto& world_rings = ctx.projection->getWorldRings((size_t)ctx.parcel_layer_idx, (uint32_t)idx, fg);
            for (const auto& r : world_rings) {
                ctx.projection->appendWorldRingLine(r, 1);
                const auto& line = ctx.projection->scratchLine();
                ctx.draw->AddPolyline(line.data(), (int)line.size(), selected_outline, ImDrawFlags_Closed, 2.5f);
            }
        } else {
            ImVec2 pw = lonLatToWorldPx(fg.extent.min_lon, fg.extent.min_lat, ctx.math_zoom);
            ImVec2 ps = ctx.project_world(pw);
            if (ps.x >= ctx.origin.x && ps.x <= ctx.origin.x + ctx.size.x &&
                ps.y >= ctx.origin.y && ps.y <= ctx.origin.y + ctx.size.y) {
                ctx.draw->AddCircle(ps, 8.0f, selected_outline, 0, 2.5f);
            }
        }
    }
}
