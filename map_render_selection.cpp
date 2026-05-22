#include "map_render_selection.h"

#include "geo.h"

namespace {
ImVec2 projectArtifactLonLat(const MapSelectionRenderContext& ctx, const ImVec2& lonlat) {
    return ctx.project_world(lonLatToWorldPx(lonlat.x, lonlat.y, ctx.math_zoom));
}

const ParcelRenderFeatureRecord* parcelRenderFeature(const MapSelectionRenderContext& ctx, size_t parcel_idx) {
    if (!ctx.parcel_render_blob) return nullptr;
    if (parcel_idx < ctx.parcel_render_blob->features.size() &&
        ctx.parcel_render_blob->features[parcel_idx].feature_idx == parcel_idx) {
        return &ctx.parcel_render_blob->features[parcel_idx];
    }
    for (const ParcelRenderFeatureRecord& rec : ctx.parcel_render_blob->features) {
        if (rec.feature_idx == parcel_idx) return &rec;
    }
    return nullptr;
}

void drawParcelSelectionArtifactFill(
    const MapSelectionRenderContext& ctx,
    const ParcelRenderFeatureRecord& rec,
    ImU32 fill) {
    const uint32_t end = rec.index_offset + rec.index_count;
    if (!ctx.parcel_render_blob || end > ctx.parcel_render_blob->indices.size()) return;
    for (uint32_t i = rec.index_offset; i + 2 < end; i += 3) {
        const uint32_t ia = ctx.parcel_render_blob->indices[i];
        const uint32_t ib = ctx.parcel_render_blob->indices[i + 1];
        const uint32_t ic = ctx.parcel_render_blob->indices[i + 2];
        if (ia >= ctx.parcel_render_blob->vertices.size() ||
            ib >= ctx.parcel_render_blob->vertices.size() ||
            ic >= ctx.parcel_render_blob->vertices.size()) continue;
        ctx.draw->AddTriangleFilled(
            projectArtifactLonLat(ctx, ctx.parcel_render_blob->vertices[ia]),
            projectArtifactLonLat(ctx, ctx.parcel_render_blob->vertices[ib]),
            projectArtifactLonLat(ctx, ctx.parcel_render_blob->vertices[ic]),
            fill);
    }
}

void drawParcelSelectionArtifactOutline(
    const MapSelectionRenderContext& ctx,
    const ParcelRenderFeatureRecord& rec,
    ImU32 outline,
    float thickness) {
    const uint32_t end = rec.line_index_offset + rec.line_index_count;
    if (!ctx.parcel_render_blob || end > ctx.parcel_render_blob->line_indices.size()) return;
    for (uint32_t i = rec.line_index_offset; i + 1 < end; i += 2) {
        const uint32_t ia = ctx.parcel_render_blob->line_indices[i];
        const uint32_t ib = ctx.parcel_render_blob->line_indices[i + 1];
        if (ia >= ctx.parcel_render_blob->vertices.size() || ib >= ctx.parcel_render_blob->vertices.size()) continue;
        ctx.draw->AddLine(
            projectArtifactLonLat(ctx, ctx.parcel_render_blob->vertices[ia]),
            projectArtifactLonLat(ctx, ctx.parcel_render_blob->vertices[ib]),
            outline,
            thickness);
    }
}
} // namespace

void renderSelectedParcelOutlines(const MapSelectionRenderContext& ctx) {
    if (!ctx.draw || !ctx.layers || !ctx.selected_parcel_indices || !ctx.projection || !ctx.project_world) return;
    if (ctx.parcel_layer_idx < 0 || (size_t)ctx.parcel_layer_idx >= ctx.layers->size()) return;
    if (ctx.selected_parcel_indices->empty()) return;

    const ImU32 selected_fill = IM_COL32(255, 230, 0, 108);
    const ImU32 selected_outline_halo = IM_COL32(32, 24, 0, 255);
    const ImU32 selected_outline = IM_COL32(255, 240, 64, 255);
    const auto& parcel_layer = (*ctx.layers)[(size_t)ctx.parcel_layer_idx];
    if (!parcel_layer.enabled) return;

    for (size_t idx : *ctx.selected_parcel_indices) {
        if (idx >= parcel_layer.features.size()) continue;
        const ParcelRenderFeatureRecord* rec = parcelRenderFeature(ctx, idx);
        if (!rec) continue;
        drawParcelSelectionArtifactFill(ctx, *rec, selected_fill);
        drawParcelSelectionArtifactOutline(ctx, *rec, selected_outline_halo, 6.0f);
        drawParcelSelectionArtifactOutline(ctx, *rec, selected_outline, 3.0f);
    }
}
