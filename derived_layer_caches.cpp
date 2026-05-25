#include "derived_layer_caches.h"

#include "app_utils.h"
#include "cache_io.h"
#include "duckdb_analytics.h"
#include "feature_props.h"
#include "parcel_consolidation.h"
#include "vacancy_overlay.h"

#include <algorithm>
#include <sstream>
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

    size_t active_zoning_feature_total = 0;
    for (const LayerDef& layer : layers) {
        if (!layer.enabled || !isZoningPolygonLayerForCaches(layer)) continue;
        active_zoning_feature_total += layer.features.size();
    }
    if (active_zoning_feature_total != *ctx.zoning_zone_discovered_feature_count) {
        *ctx.zoning_zone_discovered_feature_count = active_zoning_feature_total;
        ctx.zoning_zone_counts->clear();
        ctx.zoning_zone_label->clear();
        ctx.zoning_group_zones->clear();
        ctx.zoning_group_order->clear();
        std::unordered_map<std::string, bool> prev_enabled = *ctx.zoning_zone_enabled;
        ctx.zoning_zone_order->clear();
        std::unordered_set<std::string> seen_zone_keys;
        seen_zone_keys.reserve(active_zoning_feature_total / 4 + 16);
        for (const LayerDef& layer : layers) {
            if (!layer.enabled || !isZoningPolygonLayerForCaches(layer)) continue;
            for (const auto& fg : layer.features) {
                std::string zkey = zoningClassKey(fg);
                std::string zlabel = zoningClassLabel(fg);
                (*ctx.zoning_zone_counts)[zkey] += 1;
                if (seen_zone_keys.insert(zkey).second) ctx.zoning_zone_order->push_back(zkey);
                if (ctx.zoning_zone_enabled->find(zkey) == ctx.zoning_zone_enabled->end()) {
                    auto it_prev = prev_enabled.find(zkey);
                    (*ctx.zoning_zone_enabled)[zkey] = (it_prev == prev_enabled.end()) ? true : it_prev->second;
                }
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
        const auto& pfeats = layers[(size_t)ctx.parcel_layer_idx].features;
        const std::string parcel_sig = hydratedLayerSignature(layers, ctx.layer_states, ctx.parcel_layer_idx);
        if (!ctx.duckdb_analytics || !ctx.duckdb_analytics->status().last_rebuild_ok) {
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
        if (!snapshot.ok || snapshot.unified_parcels.size() != pfeats.size()) {
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
        for (size_t i = 0; i < pfeats.size(); ++i) {
            const int vac_notice = (i < ctx.parcel_vac_notice_by_feature->size()) ? (*ctx.parcel_vac_notice_by_feature)[i] : 0;
            const int vac_rehab = (i < ctx.parcel_vac_rehab_by_feature->size()) ? (*ctx.parcel_vac_rehab_by_feature)[i] : 0;
            notice_rows_matched += (size_t)std::max(vac_notice, 0);
            rehab_rows_matched += (size_t)std::max(vac_rehab, 0);
            if ((vac_notice + vac_rehab) <= 0) continue;
            matched_total++;
            if (!pfeats[i].rings.empty()) with_geometry_total++;
            if (!pfeats[i].rings.empty() && !pfeats[i].triangles.empty()) triangulated_renderable_total++;
        }
        ctx.vacant_notice_rows_matched_total->store(notice_rows_matched, std::memory_order_relaxed);
        ctx.vacant_rehab_rows_matched_total->store(rehab_rows_matched, std::memory_order_relaxed);
        ctx.vacant_parcels_matched_total->store(matched_total, std::memory_order_relaxed);
        ctx.vacant_parcels_with_geometry_total->store(with_geometry_total, std::memory_order_relaxed);
        ctx.vacant_parcels_triangulated_renderable_total->store(triangulated_renderable_total, std::memory_order_relaxed);
        const std::filesystem::path derived_path = *ctx.root / "data" / "cache" / "derived" / "parcel_vacancy_status.json";
        saveDerivedVacancyStatus(
            derived_path,
            pfeats,
            *ctx.parcel_vac_notice_by_feature,
            *ctx.parcel_vac_rehab_by_feature,
            ctx.parcel_vac_notice_by_feature->size(),
            ctx.parcel_vac_rehab_by_feature->size(),
            notice_rows_matched,
            rehab_rows_matched,
            ctx.parcel_blocklot_by_feature);
    } else {
        ctx.parcel_owner_search_by_feature->clear();
        ctx.real_property_owner_search_by_feature->clear();
        ctx.parcel_address_search_by_feature->clear();
    }
}
