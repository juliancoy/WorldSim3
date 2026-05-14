#include "derived_layer_caches.h"

#include "app_utils.h"
#include "feature_props.h"
#include "parcel_consolidation.h"
#include "vacancy_overlay.h"

#include <algorithm>
#include <sstream>
#include <unordered_set>

namespace {
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
        !ctx.vacant_notice_rows_matched_total || !ctx.vacant_rehab_rows_matched_total ||
        !ctx.vacant_parcels_matched_total || !ctx.vacant_parcels_with_geometry_total ||
        !ctx.vacant_parcels_triangulated_renderable_total || !ctx.unified_parcels ||
        !ctx.unified_parcel_cached_size || !ctx.unified_parcel_cached_signature ||
        !ctx.unified_real_property_cached_size ||
        !ctx.unified_vacancy_generation_applied || !ctx.unified_tax_generation_applied ||
        !ctx.owner_aggregates_dirty) {
        return;
    }

    auto& layers = *ctx.layers;

    if (ctx.zoning_layer_idx >= 0 && (size_t)ctx.zoning_layer_idx < layers.size()) {
        const auto& zfeats = layers[(size_t)ctx.zoning_layer_idx].features;
        if (zfeats.size() != *ctx.zoning_zone_discovered_feature_count) {
            *ctx.zoning_zone_discovered_feature_count = zfeats.size();
            ctx.zoning_zone_counts->clear();
            ctx.zoning_zone_label->clear();
            ctx.zoning_group_zones->clear();
            ctx.zoning_group_order->clear();
            std::unordered_map<std::string, bool> prev_enabled = *ctx.zoning_zone_enabled;
            ctx.zoning_zone_order->clear();
            std::unordered_set<std::string> seen_zone_keys;
            seen_zone_keys.reserve(zfeats.size() / 4 + 16);
            for (const auto& fg : zfeats) {
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
            std::sort(ctx.zoning_zone_order->begin(), ctx.zoning_zone_order->end());
            for (const auto& zkey : *ctx.zoning_zone_order) {
                std::string g = zoningGroupKey(zkey);
                if (ctx.zoning_group_zones->find(g) == ctx.zoning_group_zones->end()) ctx.zoning_group_order->push_back(g);
                (*ctx.zoning_group_zones)[g].push_back(zkey);
            }
            std::sort(ctx.zoning_group_order->begin(), ctx.zoning_group_order->end());
        }
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
    if (ctx.vacant_notice_layer_idx >= 0) {
        const auto& feats = layers[(size_t)ctx.vacant_notice_layer_idx].features;
        const std::string sig = hydratedLayerSignature(layers, ctx.layer_states, ctx.vacant_notice_layer_idx);
        if (feats.size() != *ctx.cached_vac_notice_size || sig != *ctx.cached_vac_notice_signature) {
            ctx.vacant_notice_count_by_blocklot->clear();
            for (const auto& fg : feats) {
                std::string bl = normalizeJoinKey(getPropertyValue(fg, "BLOCKLOT"));
                if (!bl.empty()) (*ctx.vacant_notice_count_by_blocklot)[bl] += 1;
            }
            *ctx.cached_vac_notice_size = feats.size();
            *ctx.cached_vac_notice_signature = sig;
            *ctx.vacancy_maps_generation += 1;
        }
    }
    if (ctx.vacant_rehab_layer_idx >= 0) {
        const auto& feats = layers[(size_t)ctx.vacant_rehab_layer_idx].features;
        const std::string sig = hydratedLayerSignature(layers, ctx.layer_states, ctx.vacant_rehab_layer_idx);
        if (feats.size() != *ctx.cached_vac_rehab_size || sig != *ctx.cached_vac_rehab_signature) {
            ctx.vacant_rehab_count_by_blocklot->clear();
            for (const auto& fg : feats) {
                std::string bl = normalizeJoinKey(getPropertyValue(fg, "BLOCKLOT"));
                if (!bl.empty()) (*ctx.vacant_rehab_count_by_blocklot)[bl] += 1;
            }
            *ctx.cached_vac_rehab_size = feats.size();
            *ctx.cached_vac_rehab_signature = sig;
            *ctx.vacancy_maps_generation += 1;
        }
    }
    if (ctx.tax_lien_layer_idx >= 0) {
        const auto& feats = layers[(size_t)ctx.tax_lien_layer_idx].features;
        const std::string sig = hydratedLayerSignature(layers, ctx.layer_states, ctx.tax_lien_layer_idx);
        if (feats.size() != *ctx.cached_tax_lien_size || sig != *ctx.cached_tax_lien_signature) {
            ctx.tax_lien_count_by_blocklot->clear();
            ctx.tax_lien_amount_by_blocklot->clear();
            for (const auto& fg : feats) {
                std::string bl = featureBlockLotJoinKey(fg);
                if (bl.empty()) continue;
                (*ctx.tax_lien_count_by_blocklot)[bl] += 1;
                (*ctx.tax_lien_amount_by_blocklot)[bl] += parseNumericField(getPropertyValue(fg, "TOTAL_AMOUNT"));
            }
            *ctx.cached_tax_lien_size = feats.size();
            *ctx.cached_tax_lien_signature = sig;
            *ctx.tax_maps_generation += 1;
        }
    }
    if (ctx.tax_sale_layer_idx >= 0) {
        const auto& feats = layers[(size_t)ctx.tax_sale_layer_idx].features;
        const std::string sig = hydratedLayerSignature(layers, ctx.layer_states, ctx.tax_sale_layer_idx);
        if (feats.size() != *ctx.cached_tax_sale_size || sig != *ctx.cached_tax_sale_signature) {
            ctx.tax_sale_count_by_blocklot->clear();
            ctx.tax_sale_amount_by_blocklot->clear();
            for (const auto& fg : feats) {
                std::string bl = featureBlockLotJoinKey(fg);
                if (bl.empty()) continue;
                (*ctx.tax_sale_count_by_blocklot)[bl] += 1;
                double amount = parseNumericField(getPropertyValue(fg, "total_lien"));
                if (amount <= 0.0) amount = parseNumericField(getPropertyValue(fg, "total_3yea"));
                if (amount <= 0.0) amount = parseNumericField(getPropertyValue(fg, "total_tax"));
                (*ctx.tax_sale_amount_by_blocklot)[bl] += amount;
            }
            *ctx.cached_tax_sale_size = feats.size();
            *ctx.cached_tax_sale_signature = sig;
            *ctx.tax_maps_generation += 1;
        }
    }
    if (ctx.parcel_layer_idx >= 0) {
        const auto& pfeats = layers[(size_t)ctx.parcel_layer_idx].features;
        const std::string parcel_sig = hydratedLayerSignature(layers, ctx.layer_states, ctx.parcel_layer_idx);
        if (ctx.parcel_vac_notice_by_feature->size() != pfeats.size() ||
            ctx.parcel_vac_rehab_by_feature->size() != pfeats.size() ||
            *ctx.parcel_vacancy_generation_applied != *ctx.vacancy_maps_generation) {
            ctx.parcel_vac_notice_by_feature->assign(pfeats.size(), 0);
            ctx.parcel_vac_rehab_by_feature->assign(pfeats.size(), 0);
            size_t notice_rows_matched = 0;
            size_t rehab_rows_matched = 0;
            for (size_t i = 0; i < pfeats.size(); ++i) {
                std::string bl = normalizeJoinKey(getPropertyValue(pfeats[i], "BLOCKLOT"));
                auto itn = ctx.vacant_notice_count_by_blocklot->find(bl);
                if (itn != ctx.vacant_notice_count_by_blocklot->end()) {
                    (*ctx.parcel_vac_notice_by_feature)[i] = itn->second;
                    notice_rows_matched += (size_t)itn->second;
                }
                auto itr = ctx.vacant_rehab_count_by_blocklot->find(bl);
                if (itr != ctx.vacant_rehab_count_by_blocklot->end()) {
                    (*ctx.parcel_vac_rehab_by_feature)[i] = itr->second;
                    rehab_rows_matched += (size_t)itr->second;
                }
            }
            *ctx.parcel_vacancy_generation_applied = *ctx.vacancy_maps_generation;
            ctx.vacant_notice_rows_matched_total->store(notice_rows_matched, std::memory_order_relaxed);
            ctx.vacant_rehab_rows_matched_total->store(rehab_rows_matched, std::memory_order_relaxed);
            const std::filesystem::path derived_path = *ctx.root / "data" / "cache" / "derived" / "parcel_vacancy_status.json";
            saveDerivedVacancyStatus(
                derived_path,
                pfeats,
                *ctx.parcel_vac_notice_by_feature,
                *ctx.parcel_vac_rehab_by_feature,
                (size_t)*ctx.cached_vac_notice_size,
                (size_t)*ctx.cached_vac_rehab_size,
                notice_rows_matched,
                rehab_rows_matched);
            *ctx.owner_aggregates_dirty = true;
        }
        if (ctx.parcel_tax_lien_by_feature->size() != pfeats.size() ||
            ctx.parcel_tax_sale_by_feature->size() != pfeats.size() ||
            *ctx.parcel_tax_generation_applied != *ctx.tax_maps_generation) {
            ctx.parcel_tax_lien_by_feature->assign(pfeats.size(), 0);
            ctx.parcel_tax_sale_by_feature->assign(pfeats.size(), 0);
            ctx.parcel_tax_lien_amount_by_feature->assign(pfeats.size(), 0.0);
            ctx.parcel_tax_sale_amount_by_feature->assign(pfeats.size(), 0.0);
            for (size_t i = 0; i < pfeats.size(); ++i) {
                std::string bl = featureBlockLotJoinKey(pfeats[i]);
                auto it_lien = ctx.tax_lien_count_by_blocklot->find(bl);
                if (it_lien != ctx.tax_lien_count_by_blocklot->end()) {
                    (*ctx.parcel_tax_lien_by_feature)[i] = it_lien->second;
                    auto it_amt = ctx.tax_lien_amount_by_blocklot->find(bl);
                    if (it_amt != ctx.tax_lien_amount_by_blocklot->end()) (*ctx.parcel_tax_lien_amount_by_feature)[i] = it_amt->second;
                }
                auto it_sale = ctx.tax_sale_count_by_blocklot->find(bl);
                if (it_sale != ctx.tax_sale_count_by_blocklot->end()) {
                    (*ctx.parcel_tax_sale_by_feature)[i] = it_sale->second;
                    auto it_amt = ctx.tax_sale_amount_by_blocklot->find(bl);
                    if (it_amt != ctx.tax_sale_amount_by_blocklot->end()) (*ctx.parcel_tax_sale_amount_by_feature)[i] = it_amt->second;
                }
            }
            *ctx.parcel_tax_generation_applied = *ctx.tax_maps_generation;
            *ctx.owner_aggregates_dirty = true;
        }
        const size_t real_property_size_for_unified = ctx.harmonized_real_property_features->size();
        if (*ctx.unified_parcel_cached_size != pfeats.size() ||
            *ctx.unified_parcel_cached_signature != parcel_sig ||
            *ctx.unified_real_property_cached_size != real_property_size_for_unified ||
            *ctx.unified_vacancy_generation_applied != *ctx.parcel_vacancy_generation_applied ||
            *ctx.unified_tax_generation_applied != *ctx.parcel_tax_generation_applied) {
            *ctx.unified_parcels = buildUnifiedParcels(UnifiedParcelBuildRequest{
                &layers,
                ctx.parcel_layer_idx,
                ctx.real_property_layer_idx,
                ctx.harmonized_real_property_features,
                ctx.harmonized_real_property_source_files,
                ctx.real_property_by_blocklot,
                ctx.parcel_vac_notice_by_feature,
                ctx.parcel_vac_rehab_by_feature,
                ctx.parcel_tax_lien_by_feature,
                ctx.parcel_tax_sale_by_feature,
                ctx.parcel_tax_lien_amount_by_feature,
                ctx.parcel_tax_sale_amount_by_feature
            });
            *ctx.unified_parcel_cached_size = pfeats.size();
            *ctx.unified_parcel_cached_signature = parcel_sig;
            *ctx.unified_real_property_cached_size = real_property_size_for_unified;
            *ctx.unified_vacancy_generation_applied = *ctx.parcel_vacancy_generation_applied;
            *ctx.unified_tax_generation_applied = *ctx.parcel_tax_generation_applied;
            *ctx.owner_aggregates_dirty = true;
        }
        size_t matched_total = 0;
        size_t with_geometry_total = 0;
        size_t triangulated_renderable_total = 0;
        for (size_t i = 0; i < pfeats.size(); ++i) {
            const int vac_notice = (i < ctx.parcel_vac_notice_by_feature->size()) ? (*ctx.parcel_vac_notice_by_feature)[i] : 0;
            const int vac_rehab = (i < ctx.parcel_vac_rehab_by_feature->size()) ? (*ctx.parcel_vac_rehab_by_feature)[i] : 0;
            if ((vac_notice + vac_rehab) <= 0) continue;
            matched_total++;
            if (!pfeats[i].rings.empty()) with_geometry_total++;
            if (!pfeats[i].rings.empty() && !pfeats[i].triangles.empty()) triangulated_renderable_total++;
        }
        ctx.vacant_parcels_matched_total->store(matched_total, std::memory_order_relaxed);
        ctx.vacant_parcels_with_geometry_total->store(with_geometry_total, std::memory_order_relaxed);
        ctx.vacant_parcels_triangulated_renderable_total->store(triangulated_renderable_total, std::memory_order_relaxed);
    }
}
