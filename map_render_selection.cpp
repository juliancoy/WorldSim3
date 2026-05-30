#include "map_render_selection.h"

#include "geo.h"

namespace {
ImVec2 projectArtifactLonLat(const MapSelectionRenderContext& ctx, const ImVec2& lonlat) {
    return ctx.project_world(lonLatToWorldPx(lonlat.x, lonlat.y, ctx.math_zoom));
}

void drawParcelSelectionArtifactFill(
    const MapSelectionRenderContext& ctx,
    const ParcelRenderFeatureRecord& rec,
    ImU32 fill);
void drawParcelSelectionArtifactOutline(
    const MapSelectionRenderContext& ctx,
    const ParcelRenderFeatureRecord& rec,
    ImU32 outline,
    float thickness);
bool drawPolygonSelectionArtifactFill(
    const MapSelectionRenderContext& ctx,
    const PolygonGeometryArtifact& artifact,
    const GeometryArtifactFeatureRecord& rec,
    ImU32 fill);
bool drawPolygonSelectionArtifactOutline(
    const MapSelectionRenderContext& ctx,
    const PolygonGeometryArtifact& artifact,
    const GeometryArtifactFeatureRecord& rec,
    ImU32 outline,
    float thickness);

bool drawParcelSelectionByEntityId(
    const MapSelectionRenderContext& ctx,
    const std::string& entity_id,
    ImU32 fill,
    ImU32 outline_halo,
    ImU32 outline) {
    if (!ctx.parcel_render_blob || entity_id.empty()) return false;
    bool drew = false;
    for (const ParcelRenderFeatureRecord& rec : ctx.parcel_render_blob->features) {
        if (rec.entity_id != entity_id) continue;
        drawParcelSelectionArtifactFill(ctx, rec, fill);
        drawParcelSelectionArtifactOutline(ctx, rec, outline_halo, 6.0f);
        drawParcelSelectionArtifactOutline(ctx, rec, outline, 3.0f);
        drew = true;
    }
    return drew;
}

bool drawParcelSelectionByGeometryEntityId(
    const MapSelectionRenderContext& ctx,
    const std::string& geometry_entity_id,
    ImU32 fill,
    ImU32 outline_halo,
    ImU32 outline) {
    if (!ctx.parcel_render_blob || geometry_entity_id.empty()) return false;
    bool drew = false;
    for (const ParcelRenderFeatureRecord& rec : ctx.parcel_render_blob->features) {
        if (rec.geometry_entity_id != geometry_entity_id && rec.entity_id != geometry_entity_id) continue;
        drawParcelSelectionArtifactFill(ctx, rec, fill);
        drawParcelSelectionArtifactOutline(ctx, rec, outline_halo, 6.0f);
        drawParcelSelectionArtifactOutline(ctx, rec, outline, 3.0f);
        drew = true;
    }
    return drew;
}

bool drawParcelSelectionByFeatureIdx(
    const MapSelectionRenderContext& ctx,
    size_t feature_idx,
    ImU32 fill,
    ImU32 outline_halo,
    ImU32 outline) {
    if (!ctx.parcel_render_blob || feature_idx >= ctx.parcel_render_blob->features.size()) return false;
    const ParcelRenderFeatureRecord& rec = ctx.parcel_render_blob->features[feature_idx];
    if (rec.feature_idx != feature_idx) return false;
    drawParcelSelectionArtifactFill(ctx, rec, fill);
    drawParcelSelectionArtifactOutline(ctx, rec, outline_halo, 6.0f);
    drawParcelSelectionArtifactOutline(ctx, rec, outline, 3.0f);
    return true;
}

void drawParcelSelectionArtifactFill(
    const MapSelectionRenderContext& ctx,
    const ParcelRenderFeatureRecord& rec,
    ImU32 fill) {
    const uint32_t end = rec.index_offset + rec.index_count;
    if (!ctx.parcel_render_blob || end > ctx.parcel_render_blob->indices.size()) return;
    const ImDrawListFlags saved_flags = ctx.draw->Flags;
    ctx.draw->Flags &= ~ImDrawListFlags_AntiAliasedFill;
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
    ctx.draw->Flags = saved_flags;
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

bool drawPolygonSelectionByEntityId(
    const MapSelectionRenderContext& ctx,
    const PolygonGeometryArtifact& artifact,
    const std::string& entity_id,
    ImU32 fill,
    ImU32 outline_halo,
    ImU32 outline) {
    if (entity_id.empty()) return false;
    bool drew = false;
    for (const GeometryArtifactFeatureRecord& rec : artifact.features) {
        if (rec.entity_id != entity_id) continue;
        const bool filled = drawPolygonSelectionArtifactFill(ctx, artifact, rec, fill);
        const bool halo = drawPolygonSelectionArtifactOutline(ctx, artifact, rec, outline_halo, 6.0f);
        const bool line = drawPolygonSelectionArtifactOutline(ctx, artifact, rec, outline, 3.0f);
        drew = drew || filled || halo || line;
    }
    return drew;
}

bool drawPolygonSelectionByGeometryEntityId(
    const MapSelectionRenderContext& ctx,
    const PolygonGeometryArtifact& artifact,
    const std::string& geometry_entity_id,
    ImU32 fill,
    ImU32 outline_halo,
    ImU32 outline) {
    if (geometry_entity_id.empty()) return false;
    bool drew = false;
    for (const GeometryArtifactFeatureRecord& rec : artifact.features) {
        if (rec.geometry_entity_id != geometry_entity_id && rec.entity_id != geometry_entity_id) continue;
        const bool filled = drawPolygonSelectionArtifactFill(ctx, artifact, rec, fill);
        const bool halo = drawPolygonSelectionArtifactOutline(ctx, artifact, rec, outline_halo, 6.0f);
        const bool line = drawPolygonSelectionArtifactOutline(ctx, artifact, rec, outline, 3.0f);
        drew = drew || filled || halo || line;
    }
    return drew;
}

bool drawPolygonSelectionByFeatureIdx(
    const MapSelectionRenderContext& ctx,
    const PolygonGeometryArtifact& artifact,
    size_t feature_idx,
    ImU32 fill,
    ImU32 outline_halo,
    ImU32 outline) {
    if (feature_idx >= artifact.features.size()) return false;
    const GeometryArtifactFeatureRecord& rec = artifact.features[feature_idx];
    if (rec.feature_idx != feature_idx) return false;
    const bool filled = drawPolygonSelectionArtifactFill(ctx, artifact, rec, fill);
    const bool halo = drawPolygonSelectionArtifactOutline(ctx, artifact, rec, outline_halo, 6.0f);
    const bool line = drawPolygonSelectionArtifactOutline(ctx, artifact, rec, outline, 3.0f);
    return filled || halo || line;
}

bool drawPolygonSelectionArtifactFill(
    const MapSelectionRenderContext& ctx,
    const PolygonGeometryArtifact& artifact,
    const GeometryArtifactFeatureRecord& rec,
    ImU32 fill) {
    const uint32_t end = rec.index_offset + rec.index_count;
    if (rec.index_count == 0 || end > artifact.fill_indices.size()) return false;
    bool drew = false;
    const ImDrawListFlags saved_flags = ctx.draw->Flags;
    ctx.draw->Flags &= ~ImDrawListFlags_AntiAliasedFill;
    for (uint32_t i = rec.index_offset; i + 2 < end; i += 3) {
        const uint32_t ia = artifact.fill_indices[i];
        const uint32_t ib = artifact.fill_indices[i + 1];
        const uint32_t ic = artifact.fill_indices[i + 2];
        if (ia >= artifact.vertices.size() || ib >= artifact.vertices.size() || ic >= artifact.vertices.size()) continue;
        ctx.draw->AddTriangleFilled(
            projectArtifactLonLat(ctx, artifact.vertices[ia]),
            projectArtifactLonLat(ctx, artifact.vertices[ib]),
            projectArtifactLonLat(ctx, artifact.vertices[ic]),
            fill);
        drew = true;
    }
    ctx.draw->Flags = saved_flags;
    return drew;
}

bool drawPolygonSelectionArtifactOutline(
    const MapSelectionRenderContext& ctx,
    const PolygonGeometryArtifact& artifact,
    const GeometryArtifactFeatureRecord& rec,
    ImU32 outline,
    float thickness) {
    const uint32_t end = rec.aux_index_offset + rec.aux_index_count;
    if (rec.aux_index_count == 0 || end > artifact.line_indices.size()) return false;
    bool drew = false;
    for (uint32_t i = rec.aux_index_offset; i + 1 < end; i += 2) {
        const uint32_t ia = artifact.line_indices[i];
        const uint32_t ib = artifact.line_indices[i + 1];
        if (ia >= artifact.vertices.size() || ib >= artifact.vertices.size()) continue;
        ctx.draw->AddLine(
            projectArtifactLonLat(ctx, artifact.vertices[ia]),
            projectArtifactLonLat(ctx, artifact.vertices[ib]),
            outline,
            thickness);
        drew = true;
    }
    return drew;
}

} // namespace

void renderSelectedParcelOutlines(const MapSelectionRenderContext& ctx) {
    if (!ctx.draw || !ctx.layers || !ctx.parcel_selection || !ctx.project_world) return;
    if (ctx.parcel_selection->refs.empty()) return;

    const ImU32 selected_fill = IM_COL32(255, 230, 0, 108);
    const ImU32 selected_outline_halo = IM_COL32(32, 24, 0, 255);
    const ImU32 selected_outline = IM_COL32(255, 240, 64, 255);

    for (const ParcelSelectionRef& ref : ctx.parcel_selection->refs) {
        if (ref.layer_idx < 0 || (size_t)ref.layer_idx >= ctx.layers->size()) continue;
        const auto& parcel_layer = (*ctx.layers)[(size_t)ref.layer_idx];
        if (!parcel_layer.enabled || ref.entity_id.empty()) continue;
        const std::string geometry_entity_id =
            ref.geometry_entity_id.empty() ? ref.entity_id : ref.geometry_entity_id;

        bool drew = false;
        if (ctx.polygon_geometry_artifacts) {
            auto it = ctx.polygon_geometry_artifacts->find((size_t)ref.layer_idx);
            if (it != ctx.polygon_geometry_artifacts->end()) {
                if (ref.feature_idx != (size_t)-1) {
                    drew = drawPolygonSelectionByFeatureIdx(
                        ctx,
                        it->second,
                        ref.feature_idx,
                        selected_fill,
                        selected_outline_halo,
                        selected_outline);
                }
                if (!drew) {
                    drew = drawPolygonSelectionByGeometryEntityId(
                        ctx,
                        it->second,
                        geometry_entity_id,
                        selected_fill,
                        selected_outline_halo,
                        selected_outline);
                }
            }
        }
        if (!drew && ref.layer_idx == ctx.parcel_layer_idx) {
            if (ref.feature_idx != (size_t)-1) {
                drew = drawParcelSelectionByFeatureIdx(
                    ctx,
                    ref.feature_idx,
                    selected_fill,
                    selected_outline_halo,
                    selected_outline);
            }
            if (!drew) {
                drawParcelSelectionByGeometryEntityId(
                    ctx,
                    geometry_entity_id,
                    selected_fill,
                    selected_outline_halo,
                    selected_outline);
            }
        }
    }
}
