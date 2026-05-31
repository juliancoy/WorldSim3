#include "derived_layer_caches.h"

#include "app_utils.h"
#include "cache_io.h"
#include "duckdb_analytics.h"
#include "feature_props.h"
#include "parcel_consolidation.h"
#include "render_routing.h"
#include "vacancy_overlay.h"

#include <algorithm>
#include <initializer_list>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace {
bool isZoningPolygonLayerForCaches(const LayerDef& layer) {
    if (layerUsesPointGeometry(layer)) return false;
    if (layer.category == LayerDef::Category::Zoning) return true;
    return containsCaseInsensitive(layer.file, "zoning") ||
           containsCaseInsensitive(layer.name, "zoning");
}

std::string hydratedLayerSignature(
    const std::vector<LayerDef>& layers,
    const std::vector<LayerRuntimeState>* layer_states,
    int layer_idx) {
    if (layer_idx < 0 || (size_t)layer_idx >= layers.size()) return "missing";
    if (layer_states && (size_t)layer_idx < layer_states->size() &&
        !(*layer_states)[(size_t)layer_idx].hydration_source_signature.empty()) {
        return (*layer_states)[(size_t)layer_idx].hydration_source_signature;
    }
    std::ostringstream fallback;
    fallback << "runtime:" << layers[(size_t)layer_idx].file << ":"
             << layers[(size_t)layer_idx].features.size();
    return fallback.str();
}

void appendLayerInputsSignature(
    std::string& out,
    const std::vector<LayerDef>& layers,
    const std::vector<LayerRuntimeState>* layer_states,
    int layer_idx) {
    out += "|";
    out += std::to_string(layer_idx);
    out += ":";
    if (layer_idx < 0 || (size_t)layer_idx >= layers.size()) {
        out += "missing";
        return;
    }
    out += hydratedLayerSignature(layers, layer_states, layer_idx);
    out += ":";
    out += std::to_string(layers[(size_t)layer_idx].features.size());
}

std::string derivedLayerRefreshInputsSignature(const DerivedLayerCachesContext& ctx) {
    std::string sig;
    sig.reserve(512);
    const auto& layers = *ctx.layers;
    sig += "parcel:";
    appendLayerInputsSignature(sig, layers, ctx.layer_states, ctx.parcel_layer_idx);
    sig += "|rp:";
    appendLayerInputsSignature(sig, layers, ctx.layer_states, ctx.real_property_layer_idx);
    sig += "|z:";
    for (size_t i = 0; i < layers.size(); ++i) {
        if (!layers[i].enabled || !isZoningPolygonLayerForCaches(layers[i])) continue;
        appendLayerInputsSignature(sig, layers, ctx.layer_states, (int)i);
    }
    sig += "|vn:";
    appendLayerInputsSignature(sig, layers, ctx.layer_states, ctx.vacant_notice_layer_idx);
    sig += "|vr:";
    appendLayerInputsSignature(sig, layers, ctx.layer_states, ctx.vacant_rehab_layer_idx);
    sig += "|tl:";
    appendLayerInputsSignature(sig, layers, ctx.layer_states, ctx.tax_lien_layer_idx);
    sig += "|ts:";
    appendLayerInputsSignature(sig, layers, ctx.layer_states, ctx.tax_sale_layer_idx);
    sig += "|zsim:";
    sig += ctx.app_settings->zoning_use_simcity_colors ? "1" : "0";
    sig += "|duckdb:";
    if (ctx.duckdb_analytics && ctx.duckdb_analytics->status().last_rebuild_ok) {
        const std::string duckdb_sig = ctx.duckdb_analytics->buildSourceSignature();
        sig += duckdb_sig.empty() ? "ready" : duckdb_sig;
    } else {
        sig += "unavailable";
    }
    return sig;
}

std::string ownerSearchValueFor(const LayerDef::FeatureRecord& fg) {
    return toLowerAscii(trimDisplayValue(firstDisplayProperty(fg, {
        "owner", "owner_name",
        "OWNER_1", "OWNER_2", "OWNER_3",
        "OWNERNME1", "OWNER", "OWNER_NAME",
        "AR_OWNER", "OWNER_ABBR"
    })));
}

bool ensureParcelRenderBlobForSemanticSync(
    const DerivedLayerCachesContext& ctx,
    const LayerDef& parcel_layer,
    const std::string& source_signature,
    std::string* message) {
    if (!ctx.root || !ctx.parcel_render_blob) {
        if (message) *message = "parcel render blob unavailable";
        return false;
    }
    if (ctx.parcel_render_blob->source_signature == source_signature &&
        !ctx.parcel_render_blob->features.empty()) {
        return true;
    }
    const LayerRenderRoute render_route = classifyLayerRenderRoute(
        static_cast<size_t>(ctx.parcel_layer_idx),
        parcel_layer,
        ctx.parcel_layer_idx);
    const std::filesystem::path artifact_path =
        geometryArtifactCachePathForLayerFile(
            *ctx.root,
            parcel_layer.file,
            GeometryArtifactClass::Polygon,
            layerRenderRouteArtifactName(render_route));
    PolygonGeometryArtifact artifact;
    ParcelRenderCacheBlob blob;
    std::string blob_error;
    if (!loadBinaryPolygonGeometryArtifact(artifact_path, source_signature, artifact) ||
        !buildParcelRenderCacheBlobFromPolygonArtifact(artifact, blob, &blob_error)) {
        if (message) {
            std::ostringstream ss;
            ss << artifact_path.string();
            if (!blob_error.empty()) ss << " error=" << blob_error;
            *message = ss.str();
        }
        return false;
    }
    *ctx.parcel_render_blob = std::move(blob);
    if (message) message->clear();
    return true;
}

const std::vector<LayerDef::FeatureRecord>* zoningFeaturesForCaches(
    const DerivedLayerCachesContext& ctx,
    size_t layer_idx,
    std::unordered_map<size_t, std::vector<LayerDef::FeatureRecord>>& fallback_features,
    std::unordered_map<size_t, std::vector<LayerDef::FeatureProperties>>& fallback_properties) {
    if (!ctx.layers || layer_idx >= ctx.layers->size()) return nullptr;
    const LayerDef& layer = (*ctx.layers)[layer_idx];
    if (!layer.features.empty()) return &layer.features;
    if (!ctx.root || !ctx.layer_states || layer_idx >= ctx.layer_states->size()) return nullptr;
    const std::string& sig = (*ctx.layer_states)[layer_idx].hydration_source_signature;
    if (sig.empty()) return nullptr;
    auto existing = fallback_features.find(layer_idx);
    if (existing != fallback_features.end()) return &existing->second;

    std::vector<LayerDef::FeatureRecord> features;
    std::vector<LayerDef::FeatureProperties> properties;
    if (!loadCanonicalLayerFeatureCollection(*ctx.root, layer.file, sig, features, &properties) ||
        features.empty()) {
        return nullptr;
    }
    fallback_properties[layer_idx] = std::move(properties);
    auto inserted = fallback_features.emplace(layer_idx, std::move(features));
    return &inserted.first->second;
}

const FeaturePropertyPairs* featurePropertiesForCaches(
    const LayerDef& layer,
    const std::vector<LayerDef::FeatureProperties>* fallback_properties,
    size_t feature_idx) {
    if (feature_idx < layer.feature_properties.size()) return &layer.feature_properties[feature_idx].values;
    if (fallback_properties && feature_idx < fallback_properties->size()) return &(*fallback_properties)[feature_idx].values;
    return nullptr;
}

std::string firstFeaturePropertyForCaches(
    const LayerDef& layer,
    const std::vector<LayerDef::FeatureProperties>* fallback_properties,
    const LayerDef::FeatureRecord& fg,
    size_t feature_idx,
    std::initializer_list<const char*> keys) {
    if (const FeaturePropertyPairs* props = featurePropertiesForCaches(layer, fallback_properties, feature_idx)) {
        for (const char* key : keys) {
            if (!key) continue;
            for (const auto& kv : *props) {
                if (kv.first == key) return kv.second;
            }
        }
    }
    return getFirstPropertyValue(fg, keys);
}

std::string zoningClassKeyForCaches(
    const LayerDef& layer,
    const std::vector<LayerDef::FeatureProperties>* fallback_properties,
    const LayerDef::FeatureRecord& fg,
    size_t feature_idx) {
    std::string z = firstFeaturePropertyForCaches(layer, fallback_properties, fg, feature_idx, {
        "Zoning", "Label", "ZoningLabel", "ZONING", "ZONED", "ZONE",
        "ZONE_CLASS", "ZONE_DIST", "CLASS", "DISTRICT", "Type", "TYPE", "DIST_CODE"
    });
    return z.empty() ? "UNSPECIFIED" : z;
}

std::string zoningClassLabelForCaches(
    const LayerDef& layer,
    const std::vector<LayerDef::FeatureProperties>* fallback_properties,
    const LayerDef::FeatureRecord& fg,
    size_t feature_idx) {
    std::string z = firstFeaturePropertyForCaches(layer, fallback_properties, fg, feature_idx, {
        "Label", "ZONING", "ZONED", "ZONE", "ZONE_CLASS", "ZONE_DIST",
        "CLASS", "DISTRICT", "Type", "TYPE", "DIST_CODE"
    });
    return z.empty() ? "UNSPECIFIED" : z;
}
}

void refreshDerivedLayerCaches(DerivedLayerCachesContext& ctx) {
    if (!ctx.root || !ctx.layers || !ctx.app_settings || !ctx.zoning_metadata ||
        !ctx.zoning_zone_enabled || !ctx.zoning_zone_color || !ctx.zoning_zone_label ||
        !ctx.zoning_zone_order || !ctx.zoning_zone_counts || !ctx.zoning_group_zones ||
        !ctx.zoning_group_order || !ctx.zoning_zone_discovered_feature_count ||
        !ctx.real_property_by_blocklot || !ctx.harmonized_real_property_features ||
        !ctx.harmonized_real_property_source_files || !ctx.harmonized_real_property_signature || !ctx.cached_real_property_size ||
        !ctx.cached_vac_notice_size || !ctx.cached_vac_notice_signature ||
        !ctx.cached_vac_rehab_size || !ctx.cached_vac_rehab_signature ||
        !ctx.cached_tax_lien_size || !ctx.cached_tax_lien_signature ||
        !ctx.cached_tax_sale_size || !ctx.cached_tax_sale_signature ||
        !ctx.vacant_notice_count_by_blocklot || !ctx.vacant_rehab_count_by_blocklot ||
        !ctx.tax_lien_count_by_blocklot || !ctx.tax_lien_amount_by_blocklot ||
        !ctx.tax_sale_count_by_blocklot || !ctx.tax_sale_amount_by_blocklot ||
        !ctx.vacancy_maps_generation || !ctx.parcel_vacancy_generation_applied ||
        !ctx.tax_maps_generation || !ctx.parcel_tax_generation_applied ||
        !ctx.parcel_vac_notice_by_feature || !ctx.parcel_vac_rehab_by_feature ||
        !ctx.parcel_tax_lien_by_feature || !ctx.parcel_tax_sale_by_feature ||
        !ctx.parcel_tax_lien_amount_by_feature || !ctx.parcel_tax_sale_amount_by_feature ||
        !ctx.parcel_blocklot_by_feature || !ctx.parcel_blocklot_cached_signature ||
        !ctx.vacant_notice_rows_matched_total || !ctx.vacant_rehab_rows_matched_total ||
        !ctx.vacant_parcels_matched_total || !ctx.vacant_parcels_with_geometry_total ||
        !ctx.vacant_parcels_triangulated_renderable_total || !ctx.unified_parcels ||
        !ctx.parcel_owner_search_by_feature || !ctx.real_property_owner_search_by_feature ||
        !ctx.parcel_address_search_by_feature ||
        !ctx.unified_parcel_cached_size || !ctx.unified_parcel_cached_signature ||
        !ctx.last_refresh_inputs_signature ||
        !ctx.unified_real_property_cached_size ||
        !ctx.unified_vacancy_generation_applied || !ctx.unified_tax_generation_applied ||
        !ctx.owner_aggregates_dirty) {
        return;
    }

    auto& layers = *ctx.layers;
    const std::string refresh_inputs_signature = derivedLayerRefreshInputsSignature(ctx);
    if (*ctx.last_refresh_inputs_signature == refresh_inputs_signature) return;
    *ctx.last_refresh_inputs_signature = refresh_inputs_signature;

    std::unordered_map<size_t, std::vector<LayerDef::FeatureRecord>> zoning_fallback_features;
    std::unordered_map<size_t, std::vector<LayerDef::FeatureProperties>> zoning_fallback_properties;
    size_t active_zoning_feature_total = 0;
    for (size_t layer_idx = 0; layer_idx < layers.size(); ++layer_idx) {
        const LayerDef& layer = layers[layer_idx];
        if (!layer.enabled || !isZoningPolygonLayerForCaches(layer)) continue;
        const std::vector<LayerDef::FeatureRecord>* zoning_features =
            zoningFeaturesForCaches(ctx, layer_idx, zoning_fallback_features, zoning_fallback_properties);
        if (zoning_features) active_zoning_feature_total += zoning_features->size();
    }
    {
        *ctx.zoning_zone_discovered_feature_count = active_zoning_feature_total;
        ctx.zoning_zone_counts->clear();
        ctx.zoning_zone_label->clear();
        ctx.zoning_group_zones->clear();
        ctx.zoning_group_order->clear();
        ctx.zoning_zone_order->clear();
        std::unordered_set<std::string> seen_zone_keys;
        seen_zone_keys.reserve(active_zoning_feature_total / 4 + 16);
        for (size_t layer_idx = 0; layer_idx < layers.size(); ++layer_idx) {
            const LayerDef& layer = layers[layer_idx];
            if (!layer.enabled || !isZoningPolygonLayerForCaches(layer)) continue;
            const std::vector<LayerDef::FeatureRecord>* zoning_features =
                zoningFeaturesForCaches(ctx, layer_idx, zoning_fallback_features, zoning_fallback_properties);
            if (!zoning_features) continue;
            const auto props_it = zoning_fallback_properties.find(layer_idx);
            const std::vector<LayerDef::FeatureProperties>* zoning_properties =
                props_it == zoning_fallback_properties.end() ? nullptr : &props_it->second;
            for (size_t feature_idx = 0; feature_idx < zoning_features->size(); ++feature_idx) {
                const auto& fg = (*zoning_features)[feature_idx];
                std::string zkey = zoningClassKeyForCaches(layer, zoning_properties, fg, feature_idx);
                std::string zlabel = zoningClassLabelForCaches(layer, zoning_properties, fg, feature_idx);
                (*ctx.zoning_zone_counts)[zkey] += 1;
                if (seen_zone_keys.insert(zkey).second) ctx.zoning_zone_order->push_back(zkey);
                (*ctx.zoning_zone_enabled)[zkey] = true;
                auto meta_it = ctx.zoning_metadata->find(zkey);
                if (ctx.zoning_zone_label->find(zkey) == ctx.zoning_zone_label->end()) {
                    (*ctx.zoning_zone_label)[zkey] =
                        (meta_it != ctx.zoning_metadata->end() && !meta_it->second.label.empty()) ? meta_it->second.label : zlabel;
                }
                if (ctx.zoning_zone_color->find(zkey) == ctx.zoning_zone_color->end()) {
                    if (ctx.app_settings->zoning_use_simcity_colors) {
                        (*ctx.zoning_zone_color)[zkey] = zoningShadeVariant(zoningColorFromConvention(zkey), zkey);
                    } else {
                        const ImVec4 base_color =
                            (meta_it != ctx.zoning_metadata->end() && meta_it->second.has_color) ? meta_it->second.color : colorFromStableKey(zkey);
                        (*ctx.zoning_zone_color)[zkey] = zoningShadeVariant(base_color, zkey);
                    }
                }
            }
        }
        std::sort(ctx.zoning_zone_order->begin(), ctx.zoning_zone_order->end());
        for (const auto& zkey : *ctx.zoning_zone_order) {
            std::string g = zoningGroupKey(zkey);
            if (ctx.zoning_group_zones->find(g) == ctx.zoning_group_zones->end()) ctx.zoning_group_order->push_back(g);
            (*ctx.zoning_group_zones)[g].push_back(zkey);
        }
        std::sort(ctx.zoning_group_order->begin(), ctx.zoning_group_order->end());
    }

    {
        const std::string real_property_signature =
            computeHarmonizedRealPropertySignature(*ctx.root, layers, ctx.real_property_layer_idx);
        if (real_property_signature != *ctx.harmonized_real_property_signature) {
            rebuildHarmonizedRealPropertyFeatures(
                *ctx.root,
                layers,
                ctx.real_property_layer_idx,
                *ctx.harmonized_real_property_features,
                *ctx.harmonized_real_property_source_files,
                *ctx.real_property_by_blocklot);
            *ctx.harmonized_real_property_signature = real_property_signature;
            *ctx.cached_real_property_size = ctx.harmonized_real_property_features->size();
            *ctx.owner_aggregates_dirty = true;
        }
    }
    ctx.real_property_owner_search_by_feature->clear();
    ctx.real_property_owner_search_by_feature->reserve(ctx.harmonized_real_property_features->size());
    for (const LayerDef::FeatureRecord& fg : *ctx.harmonized_real_property_features) {
        ctx.real_property_owner_search_by_feature->push_back(ownerSearchValueFor(fg));
    }

    if (ctx.parcel_layer_idx >= 0) {
        const auto& parcel_layer = layers[(size_t)ctx.parcel_layer_idx];
        const std::string parcel_sig = hydratedLayerSignature(layers, ctx.layer_states, ctx.parcel_layer_idx);
        std::string parcel_render_blob_error;
        const bool parcel_render_blob_ready =
            ensureParcelRenderBlobForSemanticSync(ctx, parcel_layer, parcel_sig, &parcel_render_blob_error);
        const size_t parcel_feature_count =
            (ctx.parcel_render_blob && !ctx.parcel_render_blob->features.empty())
                ? ctx.parcel_render_blob->features.size()
                : parcel_layer.features.size();
        if (!ctx.duckdb_analytics || !ctx.duckdb_analytics->status().last_rebuild_ok) {
            std::fprintf(
                stderr,
                "[worldsim3] parcel semantics source=duckdb ready=0 db=%s reason=%s\n",
                (ctx.duckdb_analytics ? ctx.duckdb_analytics->status().db_path.c_str() : ""),
                (ctx.duckdb_analytics ? ctx.duckdb_analytics->status().message.c_str() : "DuckDB analytics unavailable"));
            ctx.parcel_blocklot_by_feature->clear();
            ctx.parcel_vac_notice_by_feature->clear();
            ctx.parcel_vac_rehab_by_feature->clear();
            ctx.parcel_tax_lien_by_feature->clear();
            ctx.parcel_tax_sale_by_feature->clear();
            ctx.parcel_tax_lien_amount_by_feature->clear();
            ctx.parcel_tax_sale_amount_by_feature->clear();
            ctx.unified_parcels->clear();
            ctx.parcel_owner_search_by_feature->clear();
            ctx.parcel_address_search_by_feature->clear();
            ctx.vacant_notice_rows_matched_total->store(0, std::memory_order_relaxed);
            ctx.vacant_rehab_rows_matched_total->store(0, std::memory_order_relaxed);
            ctx.vacant_parcels_matched_total->store(0, std::memory_order_relaxed);
            ctx.vacant_parcels_with_geometry_total->store(0, std::memory_order_relaxed);
            ctx.vacant_parcels_triangulated_renderable_total->store(0, std::memory_order_relaxed);
            return;
        }

        const DuckDbParcelSemanticSnapshot snapshot =
            ctx.duckdb_analytics->loadParcelSemanticSnapshot((size_t)ctx.parcel_layer_idx);
        const std::string parcel_semantic_sig = parcel_sig + "|" + snapshot.source_signature;
        if (!snapshot.ok || snapshot.unified_parcels.size() != parcel_feature_count) {
            std::fprintf(
                stderr,
                "[worldsim3] parcel semantics source=duckdb ready=1 snapshot_ok=%d rows=%zu expected=%zu layer_rows=%zu render_rows=%zu render_blob_ready=%d db=%s message=%s%s%s\n",
                snapshot.ok ? 1 : 0,
                snapshot.unified_parcels.size(),
                parcel_feature_count,
                parcel_layer.features.size(),
                ctx.parcel_render_blob ? ctx.parcel_render_blob->features.size() : 0,
                parcel_render_blob_ready ? 1 : 0,
                ctx.duckdb_analytics->status().db_path.c_str(),
                snapshot.message.c_str(),
                parcel_render_blob_error.empty() ? "" : " render_blob_error=",
                parcel_render_blob_error.empty() ? "" : parcel_render_blob_error.c_str());
            ctx.parcel_blocklot_by_feature->clear();
            ctx.parcel_vac_notice_by_feature->clear();
            ctx.parcel_vac_rehab_by_feature->clear();
            ctx.parcel_tax_lien_by_feature->clear();
            ctx.parcel_tax_sale_by_feature->clear();
            ctx.parcel_tax_lien_amount_by_feature->clear();
            ctx.parcel_tax_sale_amount_by_feature->clear();
            ctx.unified_parcels->clear();
            ctx.parcel_owner_search_by_feature->clear();
            ctx.parcel_address_search_by_feature->clear();
            ctx.vacant_notice_rows_matched_total->store(0, std::memory_order_relaxed);
            ctx.vacant_rehab_rows_matched_total->store(0, std::memory_order_relaxed);
            ctx.vacant_parcels_matched_total->store(0, std::memory_order_relaxed);
            ctx.vacant_parcels_with_geometry_total->store(0, std::memory_order_relaxed);
            ctx.vacant_parcels_triangulated_renderable_total->store(0, std::memory_order_relaxed);
            return;
        }

        if (*ctx.unified_parcel_cached_signature != parcel_semantic_sig ||
            *ctx.unified_parcel_cached_size != snapshot.unified_parcels.size()) {
            std::fprintf(
                stderr,
                "[worldsim3] parcel semantics source=duckdb ready=1 snapshot_ok=1 rows=%zu expected=%zu layer_rows=%zu render_rows=%zu render_blob_ready=%d db=%s signature=%s\n",
                snapshot.unified_parcels.size(),
                parcel_feature_count,
                parcel_layer.features.size(),
                ctx.parcel_render_blob ? ctx.parcel_render_blob->features.size() : 0,
                parcel_render_blob_ready ? 1 : 0,
                ctx.duckdb_analytics->status().db_path.c_str(),
                snapshot.source_signature.c_str());
            *ctx.parcel_blocklot_by_feature = snapshot.parcel_blocklot_by_feature;
            *ctx.parcel_vac_notice_by_feature = snapshot.parcel_vac_notice_by_feature;
            *ctx.parcel_vac_rehab_by_feature = snapshot.parcel_vac_rehab_by_feature;
            *ctx.parcel_tax_lien_by_feature = snapshot.parcel_tax_lien_by_feature;
            *ctx.parcel_tax_sale_by_feature = snapshot.parcel_tax_sale_by_feature;
            *ctx.parcel_tax_lien_amount_by_feature = snapshot.parcel_tax_lien_amount_by_feature;
            *ctx.parcel_tax_sale_amount_by_feature = snapshot.parcel_tax_sale_amount_by_feature;
            *ctx.parcel_owner_search_by_feature = snapshot.parcel_owner_search_by_feature;
            *ctx.parcel_address_search_by_feature = snapshot.parcel_address_search_by_feature;
            *ctx.unified_parcels = snapshot.unified_parcels;

            for (UnifiedParcelRecord& row : *ctx.unified_parcels) {
                row.real_property_layer_idx = ctx.real_property_layer_idx;
                row.real_property_local_feature_idx = (size_t)-1;
                if (!row.blocklot.empty()) {
                    auto it = ctx.real_property_by_blocklot->find(row.blocklot);
                    if (it != ctx.real_property_by_blocklot->end()) {
                        row.real_property_local_feature_idx = it->second;
                        row.has_property_record = true;
                        if (ctx.harmonized_real_property_source_files &&
                            it->second < ctx.harmonized_real_property_source_files->size() &&
                            row.property_source_file.empty()) {
                            row.property_source_file = (*ctx.harmonized_real_property_source_files)[it->second];
                        }
                    }
                }
            }

            *ctx.cached_vac_notice_size = snapshot.unified_parcels.size();
            *ctx.cached_vac_rehab_size = snapshot.unified_parcels.size();
            *ctx.cached_tax_lien_size = snapshot.unified_parcels.size();
            *ctx.cached_tax_sale_size = snapshot.unified_parcels.size();
            *ctx.cached_vac_notice_signature = parcel_semantic_sig;
            *ctx.cached_vac_rehab_signature = parcel_semantic_sig;
            *ctx.cached_tax_lien_signature = parcel_semantic_sig;
            *ctx.cached_tax_sale_signature = parcel_semantic_sig;
            *ctx.parcel_blocklot_cached_signature = parcel_semantic_sig;
            *ctx.unified_parcel_cached_size = snapshot.unified_parcels.size();
            *ctx.unified_parcel_cached_signature = parcel_semantic_sig;
            *ctx.unified_real_property_cached_size = ctx.harmonized_real_property_features->size();
            *ctx.vacancy_maps_generation += 1;
            *ctx.tax_maps_generation += 1;
            *ctx.parcel_vacancy_generation_applied = *ctx.vacancy_maps_generation;
            *ctx.parcel_tax_generation_applied = *ctx.tax_maps_generation;
            *ctx.unified_vacancy_generation_applied = *ctx.parcel_vacancy_generation_applied;
            *ctx.unified_tax_generation_applied = *ctx.parcel_tax_generation_applied;
            *ctx.owner_aggregates_dirty = true;
        }

        size_t notice_rows_matched = 0;
        size_t rehab_rows_matched = 0;
        size_t matched_total = 0;
        size_t with_geometry_total = 0;
        size_t triangulated_renderable_total = 0;
        for (size_t i = 0; i < parcel_feature_count; ++i) {
            const int vac_notice = (i < ctx.parcel_vac_notice_by_feature->size()) ? (*ctx.parcel_vac_notice_by_feature)[i] : 0;
            const int vac_rehab = (i < ctx.parcel_vac_rehab_by_feature->size()) ? (*ctx.parcel_vac_rehab_by_feature)[i] : 0;
            notice_rows_matched += (size_t)std::max(vac_notice, 0);
            rehab_rows_matched += (size_t)std::max(vac_rehab, 0);
            if ((vac_notice + vac_rehab) <= 0) continue;
            matched_total++;
            const bool has_render_geometry =
                ctx.parcel_render_blob &&
                i < ctx.parcel_render_blob->features.size() &&
                ctx.parcel_render_blob->features[i].vertex_count > 0;
            const bool has_render_triangles =
                ctx.parcel_render_blob &&
                i < ctx.parcel_render_blob->features.size() &&
                ctx.parcel_render_blob->features[i].index_count > 0;
            const bool has_layer_geometry =
                i < parcel_layer.features.size() && !parcel_layer.features[i].rings.empty();
            const bool has_layer_triangles =
                i < parcel_layer.features.size() && !parcel_layer.features[i].triangles.empty();
            if (has_render_geometry || has_layer_geometry) with_geometry_total++;
            if (has_render_triangles || has_layer_triangles) triangulated_renderable_total++;
        }
        ctx.vacant_notice_rows_matched_total->store(notice_rows_matched, std::memory_order_relaxed);
        ctx.vacant_rehab_rows_matched_total->store(rehab_rows_matched, std::memory_order_relaxed);
        ctx.vacant_parcels_matched_total->store(matched_total, std::memory_order_relaxed);
        ctx.vacant_parcels_with_geometry_total->store(with_geometry_total, std::memory_order_relaxed);
        ctx.vacant_parcels_triangulated_renderable_total->store(triangulated_renderable_total, std::memory_order_relaxed);
        if (parcel_layer.features.size() == parcel_feature_count) {
            const std::filesystem::path derived_path = *ctx.root / "data" / "cache" / "derived" / "parcel_vacancy_status.json";
            saveDerivedVacancyStatus(
                derived_path,
                parcel_layer.features,
                *ctx.parcel_vac_notice_by_feature,
                *ctx.parcel_vac_rehab_by_feature,
                ctx.parcel_vac_notice_by_feature->size(),
                ctx.parcel_vac_rehab_by_feature->size(),
                notice_rows_matched,
                rehab_rows_matched,
                ctx.parcel_blocklot_by_feature);
        }
    } else {
        ctx.parcel_owner_search_by_feature->clear();
        ctx.real_property_owner_search_by_feature->clear();
        ctx.parcel_address_search_by_feature->clear();
    }
}
