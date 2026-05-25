#include "parcel_metrics.h"

#include <cmath>
#include <numbers>

namespace {
constexpr double kDegToMetersLat = 111320.0;

double triangleAreaSqM(const ImVec2& a, const ImVec2& b, const ImVec2& c) {
    const double lat0 = ((double)a.y + (double)b.y + (double)c.y) / 3.0;
    const double sx = kDegToMetersLat * std::cos(lat0 * std::numbers::pi / 180.0);
    const double ax = (double)a.x * sx;
    const double ay = (double)a.y * kDegToMetersLat;
    const double bx = (double)b.x * sx;
    const double by = (double)b.y * kDegToMetersLat;
    const double cx = (double)c.x * sx;
    const double cy = (double)c.y * kDegToMetersLat;
    return std::abs((bx - ax) * (cy - ay) - (cx - ax) * (by - ay)) * 0.5;
}
}

const ParcelRenderFeatureRecord* parcelRenderFeatureRecord(const ParcelRenderCacheBlob* blob, size_t parcel_idx) {
    if (!blob) return nullptr;
    if (parcel_idx < blob->features.size() && blob->features[parcel_idx].feature_idx == parcel_idx) {
        return &blob->features[parcel_idx];
    }
    for (const ParcelRenderFeatureRecord& rec : blob->features) {
        if (rec.feature_idx == parcel_idx) return &rec;
    }
    return nullptr;
}

bool parcelExtent(
    const ParcelRenderCacheBlob* blob,
    size_t parcel_idx,
    const LayerDef::FeatureRecord* fg,
    LayerDef::FeatureExtent& out) {
    if (const ParcelRenderFeatureRecord* rec = parcelRenderFeatureRecord(blob, parcel_idx)) {
        out.min_lon = rec->min_lon;
        out.min_lat = rec->min_lat;
        out.max_lon = rec->max_lon;
        out.max_lat = rec->max_lat;
        return true;
    }
    if (!fg) return false;
    out = fg->extent;
    return (out.min_lon != 0.0f || out.min_lat != 0.0f || out.max_lon != 0.0f || out.max_lat != 0.0f ||
            !fg->rings.empty() || !fg->paths.empty());
}

bool parcelHasGeometry(
    const ParcelRenderCacheBlob* blob,
    size_t parcel_idx,
    const LayerDef::FeatureRecord* fg) {
    if (parcelRenderFeatureRecord(blob, parcel_idx)) return true;
    return fg && !fg->rings.empty();
}

double parcelAreaSqMFromFeature(const LayerDef::FeatureRecord& fg) {
    if (fg.rings.empty()) return 0.0;
    double total = 0.0;
    for (const auto& ring : fg.rings) {
        if (ring.size() < 3) continue;
        double lat_sum = 0.0;
        for (const auto& p : ring) lat_sum += (double)p.y;
        const double lat0 = lat_sum / (double)ring.size();
        const double sx = kDegToMetersLat * std::cos(lat0 * std::numbers::pi / 180.0);
        double a = 0.0;
        for (size_t i = 0, n = ring.size(); i < n; ++i) {
            const auto& p = ring[i];
            const auto& q = ring[(i + 1) % n];
            a += ((double)p.x * sx) * ((double)q.y * kDegToMetersLat) -
                 ((double)q.x * sx) * ((double)p.y * kDegToMetersLat);
        }
        total += std::abs(a) * 0.5;
    }
    return total;
}

double parcelAreaSqMFromRenderBlob(const ParcelRenderCacheBlob* blob, size_t parcel_idx) {
    const ParcelRenderFeatureRecord* rec = parcelRenderFeatureRecord(blob, parcel_idx);
    if (!rec || !blob) return 0.0;
    const uint32_t end = rec->index_offset + rec->index_count;
    if (end > blob->indices.size()) return 0.0;
    double total = 0.0;
    for (uint32_t i = rec->index_offset; i + 2 < end; i += 3) {
        const uint32_t ia = blob->indices[i];
        const uint32_t ib = blob->indices[i + 1];
        const uint32_t ic = blob->indices[i + 2];
        if (ia >= blob->vertices.size() || ib >= blob->vertices.size() || ic >= blob->vertices.size()) continue;
        total += triangleAreaSqM(blob->vertices[ia], blob->vertices[ib], blob->vertices[ic]);
    }
    return total;
}

double parcelAreaSqM(
    const ParcelRenderCacheBlob* blob,
    size_t parcel_idx,
    const LayerDef::FeatureRecord& fg) {
    const double render_area = parcelAreaSqMFromRenderBlob(blob, parcel_idx);
    if (render_area > 0.0 && std::isfinite(render_area)) return render_area;
    return parcelAreaSqMFromFeature(fg);
}

double parcelCurrentValue(const std::vector<UnifiedParcelRecord>* unified_parcels, size_t parcel_idx) {
    if (!unified_parcels) return 0.0;
    if (parcel_idx >= unified_parcels->size()) return 0.0;
    return (*unified_parcels)[parcel_idx].current_value;
}

double parcelParameterValue(
    int parcel_parameter_mode,
    const std::vector<UnifiedParcelRecord>* unified_parcels,
    const ParcelRenderCacheBlob* blob,
    size_t parcel_idx,
    const LayerDef::FeatureRecord& fg) {
    switch (parcel_parameter_mode) {
        case 1:
            return parcelAreaSqM(blob, parcel_idx, fg);
        case 2:
            return parcelCurrentValue(unified_parcels, parcel_idx);
        case 3: {
            const double area = parcelAreaSqM(blob, parcel_idx, fg);
            const double value = parcelCurrentValue(unified_parcels, parcel_idx);
            if (!(area > 0.0) || !std::isfinite(area) || !(value > 0.0) || !std::isfinite(value)) return 0.0;
            return value / area;
        }
        default:
            return 0.0;
    }
}
