#pragma once

#include "cache_io.h"
#include "parcel_unified.h"
#include "types.h"

#include <cstddef>
#include <vector>

const ParcelRenderFeatureRecord* parcelRenderFeatureRecord(const ParcelRenderCacheBlob* blob, size_t parcel_idx);
bool parcelExtent(
    const ParcelRenderCacheBlob* blob,
    size_t parcel_idx,
    const LayerDef::FeatureRecord* fg,
    LayerDef::FeatureExtent& out);
bool parcelHasGeometry(
    const ParcelRenderCacheBlob* blob,
    size_t parcel_idx,
    const LayerDef::FeatureRecord* fg);
double parcelAreaSqMFromFeature(const LayerDef::FeatureRecord& fg);
double parcelAreaSqMFromRenderBlob(const ParcelRenderCacheBlob* blob, size_t parcel_idx);
double parcelAreaSqM(
    const ParcelRenderCacheBlob* blob,
    size_t parcel_idx,
    const LayerDef::FeatureRecord& fg);
double parcelCurrentValue(const std::vector<UnifiedParcelRecord>* unified_parcels, size_t parcel_idx);
double parcelParameterValue(
    int parcel_parameter_mode,
    const std::vector<UnifiedParcelRecord>* unified_parcels,
    const ParcelRenderCacheBlob* blob,
    size_t parcel_idx,
    const LayerDef::FeatureRecord& fg);
