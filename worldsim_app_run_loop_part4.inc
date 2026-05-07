                crime_filter_homicide || crime_filter_robbery || crime_filter_assault ||
                crime_filter_burglary || crime_filter_theft || crime_filter_auto_theft || crime_filter_drug || crime_filter_shooting;
            if (!any_type) return true;
            auto has = [&](const char* s) { return desc.find(s) != std::string::npos || code.find(s) != std::string::npos; };
            bool ok = false;
            if (crime_filter_homicide && (has("homicide") || has("murder"))) ok = true;
            if (crime_filter_robbery && has("robbery")) ok = true;
            if (crime_filter_assault && (has("assault") || has("aggravated assault"))) ok = true;
            if (crime_filter_burglary && has("burglary")) ok = true;
            if (crime_filter_theft && (has("larceny") || has("theft"))) ok = true;
            if (crime_filter_auto_theft && (has("motor vehicle theft") || has("auto theft") || has("vehicle theft"))) ok = true;
            if (crime_filter_drug && (has("drug") || has("narcotic"))) ok = true;
            if (crime_filter_shooting && has("shooting")) ok = true;
            return ok;
        };
        auto feature_passes_filters = [&](size_t layer_idx, size_t feature_idx, const LayerDef::FeatureGeom& fg) -> bool {
            if (is_crime_layer(layer_idx)) {
                if (!filter_enabled && !crime_filter_enabled) return true;
                return crime_feature_matches(fg);
            }
            const bool selected_owner_filter_active =
                !selected_owners.empty() &&
                parcel_layer_idx >= 0 &&
                (int)layer_idx == parcel_layer_idx;
            if (!filter_enabled && !selected_owner_filter_active) return true;
            const LayerDef::FeatureGeom* rp_join = nullptr;
            if (parcel_layer_idx >= 0 &&
                (int)layer_idx == parcel_layer_idx &&
                real_property_layer_idx >= 0 &&
                (size_t)real_property_layer_idx < layers.size()) {
                std::string bl = normalizeJoinKey(first_prop(fg, {"BLOCKLOT", "BLOCK_LOT", "LOT"}));
                if (!bl.empty()) {
                    auto itrp = real_property_by_blocklot.find(bl);
                    if (itrp != real_property_by_blocklot.end() &&
                        itrp->second < layers[(size_t)real_property_layer_idx].features.size()) {
                        rp_join = &layers[(size_t)real_property_layer_idx].features[itrp->second];
                    }
                }
            }
            if (selected_owner_filter_active) {
                std::string owner = owner_name_for(rp_join);
                if (owner.empty()) owner = owner_name_for(&fg);
                if (owner.empty() || selected_owners.find(owner) == selected_owners.end()) return false;
            }
            if (!filter_enabled) return true;
            if (filter_use_date) {
                std::string ds = first_prop(fg, {"RECORD_DATE", "RECORDDATE", "DATE", "CREATED_DATE", "ISSUE_DATE", "DateNotice", "DateIssue", "DateIssued", "DateCancel", "DateAbate"});
                if (ds.empty() && rp_join) ds = first_prop(*rp_join, {"RECORD_DATE", "RECORDDATE", "DATE", "CREATED_DATE", "ISSUE_DATE", "DateNotice", "DateIssue", "DateIssued", "DateCancel", "DateAbate"});
                if (ds.empty()) return false;
                int yr = extractYearMaybe(ds);
                if (yr < 0 || yr < filter_year_min || yr > filter_year_max) return false;
            }
            const std::string q_blocklot(filter_blocklot);
            const std::string q_status(filter_status);
            const std::string q_address(filter_address);
            const std::string q_owner(filter_owner);
            const std::string q_zip(filter_zip);
            if (!q_blocklot.empty()) {
                std::string bl = first_prop(fg, {"BLOCKLOT", "BLOCK_LOT", "LOT"});
                if (!containsCaseInsensitive(bl, q_blocklot)) return false;
            }
            if (!q_status.empty()) {
                std::string st = first_prop(fg, {"STATUS", "STATE", "CASE_STATUS"});
                if (st.empty() && rp_join) st = first_prop(*rp_join, {"STATUS", "STATE", "CASE_STATUS"});
                if (st.empty() && parcel_layer_idx >= 0 && (int)layer_idx == parcel_layer_idx) {
                    const int vn = (feature_idx < parcel_vac_notice_by_feature.size()) ? parcel_vac_notice_by_feature[feature_idx] : 0;
                    const int vr = (feature_idx < parcel_vac_rehab_by_feature.size()) ? parcel_vac_rehab_by_feature[feature_idx] : 0;
                    st = (vn + vr) > 0 ? "vacant" : "occupied";
                }
                if (!containsCaseInsensitive(st, q_status)) return false;
            }
            if (!q_address.empty()) {
                std::string ad = first_prop(fg, {"FULLADDR", "PROPERTY_ADDRESS", "PREMISEADD", "ADDRESS", "Address", "ADDR"});
                if (ad.empty() && rp_join) ad = first_prop(*rp_join, {"FULLADDR", "PROPERTY_ADDRESS", "PREMISEADD", "ADDRESS", "Address", "ADDR"});
                if (!containsCaseInsensitive(ad, q_address)) return false;
            }
            if (!q_owner.empty()) {
                std::string ow = first_prop(fg, {"OWNER_1", "OWNER_2", "OWNER_3", "OWNERNME1", "OWNER", "OWNER_NAME", "OWNER_ABBR", "AR_OWNER"});
                if (ow.empty() && rp_join) ow = first_prop(*rp_join, {"OWNER_1", "OWNER_2", "OWNER_3", "OWNERNME1", "OWNER", "OWNER_NAME", "OWNER_ABBR", "AR_OWNER"});
                if (!containsCaseInsensitive(ow, q_owner)) return false;
            }
            if (!q_zip.empty()) {
                std::string zp = first_prop(fg, {"ZIP", "ZIPCODE", "POSTAL_CODE"});
                if (zp.empty() && rp_join) zp = first_prop(*rp_join, {"ZIP", "ZIPCODE", "POSTAL_CODE"});
                if (!containsCaseInsensitive(zp, q_zip)) return false;
            }
            return true;
        };
        const float global_heat_cell = std::max(2.0f, global_heat_cell_px);
        std::vector<HeatSample> heat_samples;
        auto resolve_layer_aggregate_algo = [&](size_t layer_idx) {
            return (layer_idx < layer_heatmap_algo.size() && layer_heatmap_algo[layer_idx] >= 0)
                ? layer_heatmap_algo[layer_idx]
                : heatmap_algo;
        };
        auto layer_uses_parcel_choropleth_detail = [&](size_t layer_idx) {
            if (layer_idx >= layers.size()) return false;
            const auto& layer = layers[layer_idx];
            const int min_zoom =
                layer_idx < layer_parcel_detail_min_zoom.size()
                    ? layer_parcel_detail_min_zoom[layer_idx]
                    : kParcelChoroplethMinZoom;
            return zoom >= min_zoom &&
                   layer.scale == "parcel" &&
                   !layer.heatmap_field.empty();
        };
        auto layer_uses_heatmap_aggregate = [&](size_t layer_idx) {
            if (layer_idx >= layers.size() ||
                !layers[layer_idx].enabled ||
                layer_idx >= layer_heatmap_enabled.size() ||
                layer_idx >= layer_heatmap_max_zoom.size() ||
                !layer_heatmap_enabled[layer_idx] ||
                zoom > layer_heatmap_max_zoom[layer_idx]) {
                return false;
            }
            if (layer_uses_parcel_choropleth_detail(layer_idx)) return false;
            const int aggregate_algo = resolve_layer_aggregate_algo(layer_idx);
            return aggregate_algo != kAggregateLodGeometry && isHeatmapAggregateMethod(aggregate_algo);
        };
        auto layer_uses_lod_geometry = [&](size_t layer_idx) {
            if (layer_idx >= layers.size() ||
                !layers[layer_idx].enabled ||
                layer_idx >= layer_heatmap_enabled.size() ||
                layer_idx >= layer_heatmap_max_zoom.size() ||
                !layer_heatmap_enabled[layer_idx] ||
                zoom > layer_heatmap_max_zoom[layer_idx]) {
                return false;
            }
            if (layer_uses_parcel_choropleth_detail(layer_idx)) return false;
            return resolve_layer_aggregate_algo(layer_idx) == kAggregateLodGeometry;
        };
        auto resolve_layer_heat_settings = [&](size_t layer_idx, HeatSample& hs) {
            hs.layer = (int)layer_idx;
            hs.algo = resolve_layer_aggregate_algo(layer_idx);
            hs.cell_px = std::max(2.0f, layer_idx < layer_heatmap_cell_px.size() ? layer_heatmap_cell_px[layer_idx] : global_heat_cell_px);
            hs.bandwidth_px = std::max(1.0f, layer_idx < layer_heatmap_bandwidth_px.size() ? layer_heatmap_bandwidth_px[layer_idx] : heatmap_bandwidth_px);
            hs.blur_sigma_px = std::max(0.0f, layer_idx < layer_heatmap_blur_sigma_px.size() ? layer_heatmap_blur_sigma_px[layer_idx] : heatmap_blur_sigma_px);
            hs.percentile_clip = std::clamp(layer_idx < layer_heatmap_percentile_clip.size() ? layer_heatmap_percentile_clip[layer_idx] : heatmap_percentile_clip, 50.0f, 100.0f);
            hs.zoom_adaptive_bandwidth = layer_idx < layer_heatmap_zoom_adaptive_bandwidth.size() ? layer_heatmap_zoom_adaptive_bandwidth[layer_idx] : heatmap_zoom_adaptive_bandwidth;
            hs.multires_enabled = layer_idx < layer_heatmap_multires_enabled.size() ? layer_heatmap_multires_enabled[layer_idx] : heatmap_multires_enabled;
            hs.multires_blend = std::clamp(layer_idx < layer_heatmap_multires_blend.size() ? layer_heatmap_multires_blend[layer_idx] : heatmap_multires_blend, 0.0f, 1.0f);
        };
        bool smooth_only_heatmap = true;
        bool any_active_heatmap = false;
        bool any_active_gpu_splat = false;
        for (size_t i = 0; i < layers.size(); ++i) {
            if (!layer_uses_heatmap_aggregate(i)) continue;
            any_active_heatmap = true;
            const int aggregate_algo = resolve_layer_aggregate_algo(i);
            if (aggregate_algo == kAggregateGpuSplatBlur) any_active_gpu_splat = true;
            if (!isSmoothHeatmapAggregateMethod(aggregate_algo)) smooth_only_heatmap = false;
        }
        if (!any_active_heatmap) smooth_only_heatmap = false;
        uint64_t heatmap_key = 1469598103934665603ULL;
        hashCombineU64(heatmap_key, (uint64_t)zoom);
        hashCombineU64(heatmap_key, (uint64_t)math_zoom);
        hashCombineFloat(heatmap_key, global_heat_cell_px);
        hashCombineU64(heatmap_key, (uint64_t)heatmap_algo);
        hashCombineU64(heatmap_key, (uint64_t)heatmap_quality_preset);
        hashCombineFloat(heatmap_key, heatmap_bandwidth_px);
        hashCombineFloat(heatmap_key, heatmap_blur_sigma_px);
        hashCombineFloat(heatmap_key, heatmap_percentile_clip);
        hashCombineU64(heatmap_key, heatmap_zoom_adaptive_bandwidth ? 1ULL : 0ULL);
        hashCombineU64(heatmap_key, heatmap_multires_enabled ? 1ULL : 0ULL);
        hashCombineFloat(heatmap_key, heatmap_multires_blend);
        hashCombineU64(heatmap_key, filter_enabled ? 1ULL : 0ULL);
        for (const char* p = filter_blocklot; *p; ++p) hashCombineU64(heatmap_key, (uint64_t)(unsigned char)(*p));
        for (const char* p = filter_status; *p; ++p) hashCombineU64(heatmap_key, (uint64_t)(unsigned char)(*p));
        for (const char* p = filter_address; *p; ++p) hashCombineU64(heatmap_key, (uint64_t)(unsigned char)(*p));
        for (const char* p = filter_owner; *p; ++p) hashCombineU64(heatmap_key, (uint64_t)(unsigned char)(*p));
        if (!selected_owners.empty()) {
            std::vector<std::string> selected_owner_list(selected_owners.begin(), selected_owners.end());
            std::sort(selected_owner_list.begin(), selected_owner_list.end());
            for (const auto& owner : selected_owner_list) {
                hashCombineU64(heatmap_key, (uint64_t)owner.size());
                for (unsigned char ch : owner) hashCombineU64(heatmap_key, (uint64_t)ch);
            }
        }
        for (const char* p = filter_zip; *p; ++p) hashCombineU64(heatmap_key, (uint64_t)(unsigned char)(*p));
        hashCombineU64(heatmap_key, filter_use_date ? 1ULL : 0ULL);
        hashCombineU64(heatmap_key, (uint64_t)filter_year_min);
        hashCombineU64(heatmap_key, (uint64_t)filter_year_max);
        hashCombineU64(heatmap_key, crime_filter_enabled ? 1ULL : 0ULL);
        hashCombineU64(heatmap_key, crime_filter_use_year ? 1ULL : 0ULL);
        hashCombineU64(heatmap_key, (uint64_t)crime_year_min);
        hashCombineU64(heatmap_key, (uint64_t)crime_year_max);
        hashCombineU64(heatmap_key, crime_filter_homicide ? 1ULL : 0ULL);
        hashCombineU64(heatmap_key, crime_filter_robbery ? 1ULL : 0ULL);
        hashCombineU64(heatmap_key, crime_filter_assault ? 1ULL : 0ULL);
        hashCombineU64(heatmap_key, crime_filter_burglary ? 1ULL : 0ULL);
        hashCombineU64(heatmap_key, crime_filter_theft ? 1ULL : 0ULL);
        hashCombineU64(heatmap_key, crime_filter_auto_theft ? 1ULL : 0ULL);
        hashCombineU64(heatmap_key, crime_filter_drug ? 1ULL : 0ULL);
        hashCombineU64(heatmap_key, crime_filter_shooting ? 1ULL : 0ULL);
        for (size_t i = 0; i < layers.size(); ++i) {
            hashCombineU64(heatmap_key, layers[i].enabled ? 1ULL : 0ULL);
            hashCombineU64(heatmap_key, (i < layer_heatmap_enabled.size() && layer_heatmap_enabled[i]) ? 1ULL : 0ULL);
            hashCombineU64(heatmap_key, (uint64_t)((i < layer_heatmap_max_zoom.size()) ? layer_heatmap_max_zoom[i] : 13));
            hashCombineU64(heatmap_key, (uint64_t)((i < layer_parcel_detail_min_zoom.size()) ? layer_parcel_detail_min_zoom[i] : kParcelChoroplethMinZoom));
            hashCombineU64(heatmap_key, (i < layer_heatmap_use_gradient.size() && layer_heatmap_use_gradient[i]) ? 1ULL : 0ULL);
            hashCombineU64(heatmap_key, (uint64_t)((i < layer_heatmap_algo.size()) ? (layer_heatmap_algo[i] + 1) : 0));
            hashCombineU64(heatmap_key, (uint64_t)((i < layer_normalize_mode.size()) ? layer_normalize_mode[i] : 0));
            hashCombineFloat(heatmap_key, i < layer_heatmap_cell_px.size() ? layer_heatmap_cell_px[i] : global_heat_cell_px);
            hashCombineFloat(heatmap_key, i < layer_heatmap_bandwidth_px.size() ? layer_heatmap_bandwidth_px[i] : heatmap_bandwidth_px);
            hashCombineFloat(heatmap_key, i < layer_heatmap_blur_sigma_px.size() ? layer_heatmap_blur_sigma_px[i] : heatmap_blur_sigma_px);
            hashCombineFloat(heatmap_key, i < layer_heatmap_percentile_clip.size() ? layer_heatmap_percentile_clip[i] : heatmap_percentile_clip);
            hashCombineU64(heatmap_key, (i < layer_heatmap_zoom_adaptive_bandwidth.size() && layer_heatmap_zoom_adaptive_bandwidth[i]) ? 1ULL : 0ULL);
            hashCombineU64(heatmap_key, (i < layer_heatmap_multires_enabled.size() && layer_heatmap_multires_enabled[i]) ? 1ULL : 0ULL);
            hashCombineFloat(heatmap_key, i < layer_heatmap_multires_blend.size() ? layer_heatmap_multires_blend[i] : heatmap_multires_blend);
            hashCombineU64(heatmap_key, (uint64_t)layers[i].features.size());
        }
        if (heatmap_async_inflight && heatmap_async_future.valid() &&
            heatmap_async_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            const auto heat_prof_begin = std::chrono::steady_clock::now();
            auto done = heatmap_async_future.get();
            heatmap_async_inflight = false;
            heatmap_pending_key = 0;
            CachedAggregateTexture aggregate_entry;
            aggregate_entry.cells = std::move(done.second.cells);
            aggregate_entry.last_used_frame = ++heatmap_texture_cache_frame;
            bool aggregate_has_texture = false;
            if (done.second.has_raster && done.second.raster.w > 0 && done.second.raster.h > 0 && !done.second.raster.rgba.empty()) {
                aggregate_entry.raster = done.second.raster;
                aggregate_has_texture = uploadRgbaTexture(
                    done.second.raster.rgba.data(),
                    (uint32_t)done.second.raster.w,
                    (uint32_t)done.second.raster.h,
                    aggregate_entry.texture);
            }
            auto old_cached_aggregate = heatmap_texture_cache.find(done.first);
            if (old_cached_aggregate != heatmap_texture_cache.end()) {
                destroyTileTexture(old_cached_aggregate->second.texture);
                heatmap_texture_cache.erase(old_cached_aggregate);
            }
            heatmap_texture_cache.emplace(done.first, std::move(aggregate_entry));
            prune_heatmap_texture_cache();
            if (done.first == heatmap_key) {
                const auto current_cached_aggregate = heatmap_texture_cache.find(done.first);
                if (current_cached_aggregate != heatmap_texture_cache.end()) {
                    heatmap_cached_cells = current_cached_aggregate->second.cells;
                    heatmap_cached_raster_meta = current_cached_aggregate->second.raster;
                    heatmap_raster_cache_key = aggregate_has_texture ? done.first : 0;
                    heatmap_raster_texture_valid = aggregate_has_texture;
                }
                if (!aggregate_has_texture && !smooth_only_heatmap) {
                    destroyTileTexture(heatmap_raster_texture);
                    heatmap_raster_texture_valid = false;
                    heatmap_cached_raster_meta = {};
                    heatmap_raster_cache_key = 0;
                }
                heatmap_cache_key = done.first;
                heatmap_cache_valid = true;
            }
            prof_heatmap_ms_last.store(prof_ms_since(heat_prof_begin), std::memory_order_relaxed);
        }
        if (!any_active_heatmap) {
            heatmap_cached_cells.clear();
            heatmap_cache_valid = false;
            heatmap_cache_key = 0;
            heatmap_pending_key = 0;
            if (heatmap_raster_texture_valid) destroyTileTexture(heatmap_raster_texture);
            heatmap_raster_texture_valid = false;
            heatmap_cached_raster_meta = {};
            heatmap_raster_cache_key = 0;
        }
        const CachedAggregateTexture* cached_aggregate_for_key = nullptr;
        if (any_active_heatmap) {
            auto cached_it = heatmap_texture_cache.find(heatmap_key);
            if (cached_it != heatmap_texture_cache.end()) {
                cached_it->second.last_used_frame = ++heatmap_texture_cache_frame;
                cached_aggregate_for_key = &cached_it->second;
                heatmap_cache_key = heatmap_key;
                heatmap_cache_valid = true;
            }
        }
        const bool can_use_cached_heatmap =
            any_active_heatmap &&
            (cached_aggregate_for_key != nullptr || (heatmap_cache_valid && heatmap_cache_key == heatmap_key));
        const bool aggregate_generation_pending = heatmap_async_inflight && heatmap_pending_key != 0;
        bool should_recompute_heatmap =
            any_active_heatmap &&
            !can_use_cached_heatmap &&
            !aggregate_generation_pending &&
            !heatmap_controls_active;
        const auto layer_prof_begin = std::chrono::steady_clock::now();
        const RawSourceLayerPolicy raw_source_layer_policy{
            vacant_notice_layer_idx,
            vacant_rehab_layer_idx,
            tax_lien_layer_idx,
            tax_sale_layer_idx
        };
        const bool vacant_notice_overlay_enabled =
            vacant_notice_enabled &&
            !(vacant_notice_layer_idx >= 0 &&
              (layer_uses_heatmap_aggregate((size_t)vacant_notice_layer_idx) ||
               layer_uses_lod_geometry((size_t)vacant_notice_layer_idx)));
        const bool vacant_rehab_overlay_enabled =
            vacant_rehab_enabled &&
            !(vacant_rehab_layer_idx >= 0 &&
              (layer_uses_heatmap_aggregate((size_t)vacant_rehab_layer_idx) ||
               layer_uses_lod_geometry((size_t)vacant_rehab_layer_idx)));
        const bool tax_lien_overlay_enabled =
            tax_lien_enabled &&
            !(tax_lien_layer_idx >= 0 &&
              (layer_uses_heatmap_aggregate((size_t)tax_lien_layer_idx) ||
               layer_uses_lod_geometry((size_t)tax_lien_layer_idx)));
        const bool tax_sale_overlay_enabled =
            tax_sale_enabled &&
            !(tax_sale_layer_idx >= 0 &&
              (layer_uses_heatmap_aggregate((size_t)tax_sale_layer_idx) ||
               layer_uses_lod_geometry((size_t)tax_sale_layer_idx)));
        for (size_t layer_idx = 0; layer_idx < layers.size(); ++layer_idx) {
            auto& l = layers[layer_idx];
            if (!l.enabled) continue;
            const bool layer_uses_heatmap_for_cache = layer_uses_heatmap_aggregate(layer_idx);
            const bool layer_uses_lod_for_draw = layer_uses_lod_geometry(layer_idx);
            if (shouldSkipRawSourceLayer(layer_idx, raw_source_layer_policy) &&
                !layer_uses_heatmap_for_cache &&
                !layer_uses_lod_for_draw) continue;
            const bool is_zoning_layer = ((int)layer_idx == zoning_layer_idx);
            const bool is_heat_layer = !l.heatmap_field.empty();
            float heat_min = std::numeric_limits<float>::infinity();
            float heat_max = -std::numeric_limits<float>::infinity();
            std::vector<float> heat_values;
            std::unordered_map<std::string, std::vector<float>> heat_values_by_group;
            auto normalization_group_key = [&](const LayerDef::FeatureGeom& fg) {
                std::string key = first_prop(fg, {
                    "ZONECODE", "ZONING", "ZONE", "zoning", "zoning_group",
                    "group_key", "LANDUSE", "LAND_USE", "USE", "CATEGORY", "category"
                });
                return normalizeJoinKey(key);
            };
            if (is_heat_layer) {
                heat_values.reserve(l.features.size());
                for (size_t fi = 0; fi < l.features.size(); ++fi) {
                    const auto& fg = l.features[fi];
                    if (!feature_passes_filters(layer_idx, fi, fg)) continue;
                    float v = 0.0f;
                    if (!tryGetFeaturePropertyFloat(fg, l.heatmap_field, v)) continue;
                    heat_values.push_back(v);
                    const std::string group_key = normalization_group_key(fg);
                    if (!group_key.empty()) heat_values_by_group[group_key].push_back(v);
                    heat_min = std::min(heat_min, v);
                    heat_max = std::max(heat_max, v);
                }
                if (!heat_values.empty()) {
                    const float clip_pct = std::clamp(
                        layer_idx < layer_heatmap_percentile_clip.size()
                            ? layer_heatmap_percentile_clip[layer_idx]
                            : heatmap_percentile_clip,
                        50.0f,
                        100.0f);
                    std::sort(heat_values.begin(), heat_values.end());
                    heat_min = heat_values.front();
                    if (clip_pct < 100.0f) {
                        const size_t max_idx = heat_values.size() - 1;
                        const size_t kth = (size_t)std::clamp(
                            (int)std::floor((clip_pct / 100.0f) * (double)max_idx),
                            0,
                            (int)max_idx);
                        heat_max = std::max(heat_min, heat_values[kth]);
                    } else {
                        heat_max = heat_values.back();
                    }
                }
                for (auto& kv : heat_values_by_group) std::sort(kv.second.begin(), kv.second.end());
            }
            const bool heat_range_valid = std::isfinite(heat_min) && std::isfinite(heat_max) && heat_max > heat_min;
            const int normalize_mode =
                layer_idx < layer_normalize_mode.size()
                    ? std::clamp(layer_normalize_mode[layer_idx], 0, 2)
                    : 0;
            auto percentile_rank = [](const std::vector<float>& sorted_values, float value) {
                if (sorted_values.size() <= 1) return 0.0f;
                const auto it = std::lower_bound(sorted_values.begin(), sorted_values.end(), value);
                const size_t rank = (size_t)std::distance(sorted_values.begin(), it);
                return std::clamp((float)rank / (float)(sorted_values.size() - 1), 0.0f, 1.0f);
            };
            auto normalized_heat_t = [&](const LayerDef::FeatureGeom& fg, float value, float& out_t) {
                if (heat_values.empty()) return false;
                if (normalize_mode == 1) {
                    out_t = percentile_rank(heat_values, value);
                    return true;
                }
                if (normalize_mode == 2) {
                    const std::string group_key = normalization_group_key(fg);
                    auto git = heat_values_by_group.find(group_key);
                    if (git != heat_values_by_group.end() && git->second.size() >= 5) {
                        out_t = percentile_rank(git->second, value);
                    } else {
                        out_t = percentile_rank(heat_values, value);
                    }
                    return true;
                }
                if (!heat_range_valid) return false;
                out_t = std::clamp((value - heat_min) / (heat_max - heat_min), 0.0f, 1.0f);
                return true;
            };
            auto point_in_world_rings = [](const std::vector<std::vector<ImVec2>>& rings, float x, float y) {
                if (rings.empty() || !pointInRing(rings[0], x, y)) return false;
                for (size_t ri = 1; ri < rings.size(); ++ri) {
                    if (pointInRing(rings[ri], x, y)) return false;
                }
                return true;
            };
            auto add_heat_samples_for_feature = [&](
                size_t sample_layer_idx,
                uint32_t feature_idx,
                const LayerDef::FeatureGeom& fg,
                const ImVec2& p0w,
                const ImVec2& p1w,
                ImU32 feature_c,
                float feature_heat_value,
                float feature_normalized_value,
                bool feature_heat_value_valid) {
                HeatSample base;
                base.lon = (fg.extent.min_lon + fg.extent.max_lon) * 0.5f;
                base.lat = (fg.extent.min_lat + fg.extent.max_lat) * 0.5f;
                base.color = ImGui::ColorConvertU32ToFloat4(feature_c);
                base.value = normalize_mode == 0 ? feature_heat_value : feature_normalized_value;
                base.has_value = feature_heat_value_valid;
                base.prefer_gradient =
                    sample_layer_idx < layer_heatmap_use_gradient.size()
                        ? layer_heatmap_use_gradient[sample_layer_idx]
                        : true;
                resolve_layer_heat_settings(sample_layer_idx, base);

                const int aggregate_algo = resolve_layer_aggregate_algo(sample_layer_idx);
                const bool area_choropleth =
                    aggregate_algo == kAggregateMedianChoropleth &&
                    feature_heat_value_valid &&
                    !fg.rings.empty();
                if (area_choropleth) {
                    const auto& world_rings = get_world_rings(sample_layer_idx, feature_idx, fg);
                    const float cell = std::max(2.0f, base.cell_px);
                    const float min_x = std::min(p0w.x, p1w.x);
                    const float max_x = std::max(p0w.x, p1w.x);
                    const float min_y = std::min(p0w.y, p1w.y);
                    const float max_y = std::max(p0w.y, p1w.y);
                    const int bx0 = (int)std::floor(min_x / cell);
                    const int bx1 = (int)std::floor(max_x / cell);
                    const int by0 = (int)std::floor(min_y / cell);
                    const int by1 = (int)std::floor(max_y / cell);
                    const size_t before = heat_samples.size();
                    for (int by = by0; by <= by1; ++by) {
                        const float cy = ((float)by + 0.5f) * cell;
                        for (int bx = bx0; bx <= bx1; ++bx) {
                            const float cx = ((float)bx + 0.5f) * cell;
                            if (!point_in_world_rings(world_rings, cx, cy)) continue;
                            HeatSample hs = base;
                            hs.x = cx;
                            hs.y = cy;
                            heat_samples.push_back(hs);
                        }
                    }
                    if (heat_samples.size() > before) return;
                }

                HeatSample hs = base;
                hs.x = (p0w.x + p1w.x) * 0.5f;
                hs.y = (p0w.y + p1w.y) * 0.5f;
                heat_samples.push_back(hs);
            };
            ImU32 c = ImGui::ColorConvertFloat4ToU32(l.color);
            bool have_candidates = !should_recompute_heatmap || !layer_uses_heatmap_for_cache;
            if (have_candidates) {
                have_candidates = queryLayerSpatialIndex(
                    layer_spatial[layer_idx], view_min_lon, view_min_lat, view_max_lon, view_max_lat, render_candidates);
            }
            if (have_candidates) {
                for (uint32_t fidx : render_candidates) {
                    prof_features_considered_frame++;
                    const bool layer_uses_heatmap = layer_uses_heatmap_aggregate(layer_idx);
                    if (smooth_only_heatmap && should_recompute_heatmap && layer_uses_heatmap && fidx % 2 != 0) continue;
                    if (fidx >= l.features.size()) continue;
                    auto& fg = l.features[(size_t)fidx];
                    if (!feature_passes_filters(layer_idx, (size_t)fidx, fg)) continue;
                    ImU32 feature_c = c;
                    float feature_heat_value = 0.0f;
                    float feature_normalized_value = 0.0f;
                    bool feature_heat_value_valid = false;
                    if (is_heat_layer) {
                        feature_heat_value_valid = tryGetFeaturePropertyFloat(fg, l.heatmap_field, feature_heat_value);
                        if (feature_heat_value_valid &&
                            normalized_heat_t(fg, feature_heat_value, feature_normalized_value)) {
                            feature_c = ImGui::ColorConvertFloat4ToU32(heatColor(feature_normalized_value));
                        }
                    }
                    if (is_zoning_layer) {
                        const std::string zkey = zoningClassKey(fg);
                        auto it_en = zoning_zone_enabled.find(zkey);
                        if (it_en != zoning_zone_enabled.end() && !it_en->second) continue;
                        auto it_col = zoning_zone_color.find(zkey);
                        if (it_col != zoning_zone_color.end()) feature_c = ImGui::ColorConvertFloat4ToU32(it_col->second);
                    }
                    const auto& ex = fg.extent;
                    const auto& pww = get_world_extent(layer_idx, fidx, fg);
                    ImVec2 p0w = pww.first;
                    ImVec2 p1w = pww.second;
                    ImVec2 a = project_world(p0w);
                    ImVec2 b = project_world(p1w);
                    ImVec2 p0(std::min(a.x, b.x), std::min(a.y, b.y));
                    ImVec2 p1(std::max(a.x, b.x), std::max(a.y, b.y));
                    if (p1.x < origin.x || p0.x > origin.x + size.x || p1.y < origin.y || p0.y > origin.y + size.y) continue;

                    if (layer_uses_heatmap) {
                        if (can_use_cached_heatmap) continue;
                        add_heat_samples_for_feature(
                            layer_idx,
                            fidx,
                            fg,
                            p0w,
                            p1w,
                            feature_c,
                            feature_heat_value,
                            feature_normalized_value,
                            feature_heat_value_valid);
                        continue;
                    }

                    if (!fg.rings.empty()) {
                        const auto& world_rings = get_world_rings(layer_idx, fidx, fg);
                        const bool fill_enabled_for_layer = layer_idx < layer_fill_enabled.size() && layer_fill_enabled[layer_idx];
                        if (fill_enabled_for_layer && should_fill_layer_polygon(layer_idx)) {
                            ImU32 fill = (feature_c & 0x00FFFFFF) | (170u << 24);
                            draw_tessellated_fill(fg, world_rings, fill);
                        }

                        for (const auto& r : world_rings) {
                            append_world_ring_line(r, layer_uses_lod_for_draw ? lod_ring_step : 1);
                            const auto& line = projection_cache.scratchLine();
                            draw->AddPolyline(line.data(), (int)line.size(), feature_c, ImDrawFlags_Closed, 1.0f);
                        }
                    } else {
                        ImVec2 pw = lonLatToWorldPx(fg.extent.min_lon, fg.extent.min_lat, math_zoom);
                        ImVec2 ps = project_world(pw);
                        if (ps.x >= origin.x && ps.x <= origin.x + size.x && ps.y >= origin.y && ps.y <= origin.y + size.y) {
                            float r = std::clamp((float)(2.0 * zoom_scale + 1.5), 2.0f, 6.0f);
                            draw->AddCircleFilled(ps, r, feature_c);
                            prof_features_drawn_frame++;
                        }
                    }

                }
                continue;
            }
            const size_t smooth_sample_stride =
                (smooth_only_heatmap && should_recompute_heatmap && layer_uses_heatmap_for_cache && l.features.size() > kMaxSmoothHeatSamplesPerLayer)
                    ? std::max<size_t>(1, (l.features.size() + kMaxSmoothHeatSamplesPerLayer - 1) / kMaxSmoothHeatSamplesPerLayer)
                    : 1;
            for (auto& fg : l.features) {
                prof_features_considered_frame++;
                size_t fi = (size_t)(&fg - &l.features[0]);
                if (!feature_passes_filters(layer_idx, fi, fg)) continue;
                ImU32 feature_c = c;
                float feature_heat_value = 0.0f;
                float feature_normalized_value = 0.0f;
                bool feature_heat_value_valid = false;
                if (is_heat_layer) {
                    feature_heat_value_valid = tryGetFeaturePropertyFloat(fg, l.heatmap_field, feature_heat_value);
                    if (feature_heat_value_valid &&
                        normalized_heat_t(fg, feature_heat_value, feature_normalized_value)) {
                        feature_c = ImGui::ColorConvertFloat4ToU32(heatColor(feature_normalized_value));
                    }
                }
                if (is_zoning_layer) {
                    const std::string zkey = zoningClassKey(fg);
                    auto it_en = zoning_zone_enabled.find(zkey);
                    if (it_en != zoning_zone_enabled.end() && !it_en->second) continue;
                    auto it_col = zoning_zone_color.find(zkey);
                    if (it_col != zoning_zone_color.end()) feature_c = ImGui::ColorConvertFloat4ToU32(it_col->second);
                }
                const auto& ex = fg.extent;
                const auto& pww = get_world_extent(layer_idx, (uint32_t)fi, fg);
                ImVec2 p0w = pww.first;
                ImVec2 p1w = pww.second;
                ImVec2 a = project_world(p0w);
                ImVec2 b = project_world(p1w);
                ImVec2 p0(std::min(a.x, b.x), std::min(a.y, b.y));
                ImVec2 p1(std::max(a.x, b.x), std::max(a.y, b.y));
                const bool layer_uses_heatmap = layer_uses_heatmap_aggregate(layer_idx);
                if (!layer_uses_heatmap && (p1.x < origin.x || p0.x > origin.x + size.x || p1.y < origin.y || p0.y > origin.y + size.y)) continue;
                if (layer_uses_heatmap) {
                    if (can_use_cached_heatmap) continue;
                    if (smooth_sample_stride > 1 && (fi % smooth_sample_stride) != 0) continue;
                    add_heat_samples_for_feature(
                        layer_idx,
                        (uint32_t)fi,
                        fg,
                        p0w,
                        p1w,
                        feature_c,
                        feature_heat_value,
                        feature_normalized_value,
                        feature_heat_value_valid);
                    continue;
                }

                if (!fg.rings.empty()) {
                    const auto& world_rings = get_world_rings(layer_idx, (uint32_t)fi, fg);
                    const bool fill_enabled_for_layer = layer_idx < layer_fill_enabled.size() && layer_fill_enabled[layer_idx];
                    if (fill_enabled_for_layer && should_fill_layer_polygon(layer_idx)) {
                        ImU32 fill = (feature_c & 0x00FFFFFF) | (170u << 24);
                        draw_tessellated_fill(fg, world_rings, fill);
                    }

                    // Project vacant point datasets onto parcel polygons instead of drawing point markers.
                    if (layer_idx == (size_t)parcel_layer_idx && (vacant_notice_overlay_enabled || vacant_rehab_overlay_enabled)) {
                        int vac_notice = 0;
                        int vac_rehab = 0;
                        if (fi < parcel_vac_notice_by_feature.size()) vac_notice = parcel_vac_notice_by_feature[fi];
                        if (fi < parcel_vac_rehab_by_feature.size()) vac_rehab = parcel_vac_rehab_by_feature[fi];

                        const int weight = overlayWeight(vacant_notice_overlay_enabled, vac_notice, vacant_rehab_overlay_enabled, vac_rehab);
                        if (weight > 0) {
                            const bool notice_fill = vacant_notice_overlay_enabled &&
                                vacant_notice_layer_idx >= 0 &&
                                (size_t)vacant_notice_layer_idx < layer_fill_enabled.size() &&
                                layer_fill_enabled[(size_t)vacant_notice_layer_idx];
                            const bool rehab_fill = vacant_rehab_overlay_enabled &&
                                vacant_rehab_layer_idx >= 0 &&
                                (size_t)vacant_rehab_layer_idx < layer_fill_enabled.size() &&
                                layer_fill_enabled[(size_t)vacant_rehab_layer_idx];
                            if ((notice_fill || rehab_fill) && should_fill_layer_polygon((size_t)parcel_layer_idx)) {
                                const int alpha = scaledOverlayAlpha(95, 16, 95, 220, weight);
                                ImVec4 vac_base = blendVacancyColor(vacancy_notice_color, vacancy_rehab_color, vac_notice, vac_rehab);
                                ImU32 vac_fill = colorWithAlpha(vac_base, alpha);
                                draw_tessellated_fill(fg, world_rings, vac_fill);
                            }
                        }
                    }

                    for (const auto& r : world_rings) {
                        append_world_ring_line(r, layer_uses_lod_for_draw ? lod_ring_step : 1);
                        const auto& line = projection_cache.scratchLine();
                        draw->AddPolyline(line.data(), (int)line.size(), feature_c, ImDrawFlags_Closed, 1.0f);
                    }
                } else {
                    ImVec2 pw = lonLatToWorldPx(fg.extent.min_lon, fg.extent.min_lat, math_zoom);
                    ImVec2 ps = project_world(pw);
                    if (ps.x >= origin.x && ps.x <= origin.x + size.x && ps.y >= origin.y && ps.y <= origin.y + size.y) {
                        float r = std::clamp((float)(2.0 * zoom_scale + 1.5), 2.0f, 6.0f);
                        draw->AddCircleFilled(ps, r, feature_c);
                        prof_features_drawn_frame++;
                    }
                }

            }
        }
        prof_layer_ms_last.store(prof_ms_since(layer_prof_begin), std::memory_order_relaxed);
        prof_heat_samples_last.store(heat_samples.size(), std::memory_order_relaxed);
        if (should_recompute_heatmap && heat_samples.empty()) {
            heatmap_cached_cells.clear();
            heatmap_cache_key = heatmap_key;
            heatmap_cache_valid = true;
            should_recompute_heatmap = false;
        }
        if (should_recompute_heatmap && !heatmap_async_inflight) {
            const auto heat_prof_begin = std::chrono::steady_clock::now();
            const auto samples_copy = heat_samples;
            const uint64_t key_copy = heatmap_key;
            const int zoom_copy = zoom;
            const float vw = std::max(1.0f, size.x);
            const float vh = std::max(1.0f, size.y);
            heatmap_async_future = std::async(std::launch::async, [=]() mutable -> std::pair<uint64_t, HeatmapRenderData> {
                return buildHeatmapRenderData(
                    key_copy,
                    samples_copy,
                    0.0f,
                    0.0f,
                    view_min_lon,
                    view_min_lat,
                    view_max_lon,
                    view_max_lat,
                    vw,
                    vh,
                    zoom_copy,
                    kMaxZoom,
                    kSmoothHeatRasterBasePx,
                    kSmoothHeatRasterMaxPx);
            });
            heatmap_async_inflight = true;
            heatmap_pending_key = key_copy;
            if (smooth_only_heatmap) should_recompute_heatmap = false;
        } else if (heatmap_async_inflight) {
            should_recompute_heatmap = false;
        }
        std::vector<CachedHeatCell> frame_heat_cells;
        if (should_recompute_heatmap) {
            frame_heat_cells = buildImmediateHeatmapCells(
                heat_samples,
                0.0f,
                0.0f,
                size.x,
                size.y,
                zoom,
                kMaxZoom,
                heatmap_algo,
                global_heat_cell,
                heatmap_bandwidth_px,
                heatmap_blur_sigma_px,
                heatmap_percentile_clip,
                heatmap_zoom_adaptive_bandwidth,
                heatmap_multires_enabled,
                heatmap_multires_blend);
            heatmap_cached_cells = frame_heat_cells;
            heatmap_cache_key = heatmap_key;
            heatmap_cache_valid = true;
        }
        const std::vector<CachedHeatCell> empty_heat_cells;
        const std::vector<CachedHeatCell>& draw_cells =
            cached_aggregate_for_key ? cached_aggregate_for_key->cells :
            (can_use_cached_heatmap ? heatmap_cached_cells :
            (should_recompute_heatmap ? frame_heat_cells : empty_heat_cells));
        const bool cached_aggregate_has_texture =
            cached_aggregate_for_key && cached_aggregate_for_key->texture.descriptor;
        MapHeatmapDrawContext heatmap_draw_ctx;
        heatmap_draw_ctx.draw = draw;
        heatmap_draw_ctx.math_zoom = math_zoom;
        heatmap_draw_ctx.smooth_only_heatmap = any_active_heatmap && smooth_only_heatmap;
        heatmap_draw_ctx.heatmap_async_inflight = any_active_heatmap && heatmap_async_inflight;
        heatmap_draw_ctx.heatmap_raster_texture_valid =
            any_active_heatmap && (cached_aggregate_has_texture || heatmap_raster_texture_valid);
        heatmap_draw_ctx.heatmap_raster_cache_key = cached_aggregate_has_texture ? heatmap_key : heatmap_raster_cache_key;
        heatmap_draw_ctx.heatmap_key = heatmap_key;
        heatmap_draw_ctx.heatmap_raster_texture =
            cached_aggregate_has_texture ? &cached_aggregate_for_key->texture : &heatmap_raster_texture;
        heatmap_draw_ctx.heatmap_raster =
            cached_aggregate_for_key ? &cached_aggregate_for_key->raster : &heatmap_cached_raster_meta;
        heatmap_draw_ctx.draw_cells = &draw_cells;
        heatmap_draw_ctx.project_world = project_world;
        drawHeatmapPass(heatmap_draw_ctx);
        if (any_active_gpu_splat && heatmap_async_inflight && !can_use_cached_heatmap) {
            const char* loading_label = "Generating Aggregate";
            const ImVec2 text_size = ImGui::CalcTextSize(loading_label);
            const float pad_x = 12.0f;
            const float pad_y = 7.0f;
            const ImVec2 box_min(origin.x + 12.0f, origin.y + 12.0f);
            const ImVec2 box_max(box_min.x + text_size.x + pad_x * 2.0f, box_min.y + text_size.y + pad_y * 2.0f);
            draw->AddRectFilled(box_min, box_max, IM_COL32(17, 24, 32, 220), 8.0f);
            draw->AddRect(box_min, box_max, IM_COL32(255, 255, 255, 80), 8.0f);
            draw->AddText(ImVec2(box_min.x + pad_x, box_min.y + pad_y), IM_COL32(245, 248, 250, 245), loading_label);
        }

        const auto overlay_prof_begin = std::chrono::steady_clock::now();
        MapRenderContext overlay_ctx;
        overlay_ctx.draw = draw;
        overlay_ctx.origin = origin;
        overlay_ctx.size = size;
        overlay_ctx.layers = &layers;
        overlay_ctx.parcel_layer_idx = parcel_layer_idx >= 0 ? (size_t)parcel_layer_idx : (size_t)-1;
        overlay_ctx.vacant_notice_enabled = vacant_notice_overlay_enabled;
        overlay_ctx.vacant_rehab_enabled = vacant_rehab_overlay_enabled;
        overlay_ctx.tax_lien_enabled = tax_lien_overlay_enabled;
        overlay_ctx.tax_sale_enabled = tax_sale_overlay_enabled;
        overlay_ctx.vacant_notice_layer_idx = vacant_notice_layer_idx;
        overlay_ctx.vacant_rehab_layer_idx = vacant_rehab_layer_idx;
        overlay_ctx.tax_lien_layer_idx = tax_lien_layer_idx;
        overlay_ctx.tax_sale_layer_idx = tax_sale_layer_idx;
        overlay_ctx.vacancy_notice_color = vacancy_notice_color;
        overlay_ctx.vacancy_rehab_color = vacancy_rehab_color;
        overlay_ctx.layer_fill_enabled = &layer_fill_enabled;
        overlay_ctx.parcel_vac_notice_by_feature = &parcel_vac_notice_by_feature;
        overlay_ctx.parcel_vac_rehab_by_feature = &parcel_vac_rehab_by_feature;
        overlay_ctx.parcel_tax_lien_by_feature = &parcel_tax_lien_by_feature;
        overlay_ctx.parcel_tax_sale_by_feature = &parcel_tax_sale_by_feature;
        overlay_ctx.feature_passes_filters = feature_passes_filters;
        overlay_ctx.should_fill_layer_polygon = should_fill_layer_polygon;
        overlay_ctx.projection = &projection_cache;
        const MapOverlayResult overlay_result = renderParcelSourceOverlays(overlay_ctx);
        visible_vacant_parcels_last_frame.store(overlay_result.visible_vacant_parcels, std::memory_order_relaxed);
        prof_overlay_ms_last.store(prof_ms_since(overlay_prof_begin), std::memory_order_relaxed);

        const MapFillStats& fill_stats = projection_cache.fillStats();
        render_fill_attempts_last_frame.store(fill_stats.attempts, std::memory_order_relaxed);
        render_fill_success_last_frame.store(fill_stats.success, std::memory_order_relaxed);
        render_fill_no_triangles_last_frame.store(fill_stats.no_triangles, std::memory_order_relaxed);
        render_fill_bad_indices_last_frame.store(fill_stats.bad_indices, std::memory_order_relaxed);

        {
            char zoom_label[32];
            std::snprintf(zoom_label, sizeof(zoom_label), "Zoom %d", zoom);
            const ImVec2 text_size = ImGui::CalcTextSize(zoom_label);
            const float pad_x = 10.0f;
            const float pad_y = 6.0f;
            const ImVec2 box_max(origin.x + size.x - 12.0f, origin.y + size.y - 12.0f);
            const ImVec2 box_min(box_max.x - text_size.x - pad_x * 2.0f, box_max.y - text_size.y - pad_y * 2.0f);
            draw->AddRectFilled(box_min, box_max, IM_COL32(17, 24, 32, 205), 7.0f);
            draw->AddRect(box_min, box_max, IM_COL32(255, 255, 255, 70), 7.0f);
            draw->AddText(ImVec2(box_min.x + pad_x, box_min.y + pad_y), IM_COL32(245, 248, 250, 245), zoom_label);
        }

        if (map_hovered && parcel_inspect_active && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hovered_parcel != nullptr) {
            show_selected_parcel_details = true;
            selected_parcel_idx = hovered_parcel_idx;
            show_selected_zone_details = false;
            selected_zone_idx = (size_t)-1;
        } else if (map_hovered && zoning_inspect_active && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hovered_zone != nullptr) {
            show_selected_zone_details = true;
            selected_zone_idx = hovered_zone_idx;
            show_selected_parcel_details = false;
            selected_parcel_idx = (size_t)-1;
        }

        if (parcel_hover_active && map_hovered && hovered_parcel) {
            std::string blocklot_raw = getPropertyValue(*hovered_parcel, "BLOCKLOT");
            std::string blocklot = normalizeJoinKey(blocklot_raw);
            int vac_notice = (hovered_parcel_idx < parcel_vac_notice_by_feature.size()) ? parcel_vac_notice_by_feature[hovered_parcel_idx] : 0;
            int vac_rehab = (hovered_parcel_idx < parcel_vac_rehab_by_feature.size()) ? parcel_vac_rehab_by_feature[hovered_parcel_idx] : 0;
            int tax_lien = (hovered_parcel_idx < parcel_tax_lien_by_feature.size()) ? parcel_tax_lien_by_feature[hovered_parcel_idx] : 0;
            int tax_sale = (hovered_parcel_idx < parcel_tax_sale_by_feature.size()) ? parcel_tax_sale_by_feature[hovered_parcel_idx] : 0;
            double tax_lien_amount = (hovered_parcel_idx < parcel_tax_lien_amount_by_feature.size()) ? parcel_tax_lien_amount_by_feature[hovered_parcel_idx] : 0.0;
            double tax_sale_amount = (hovered_parcel_idx < parcel_tax_sale_amount_by_feature.size()) ? parcel_tax_sale_amount_by_feature[hovered_parcel_idx] : 0.0;
            const LayerDef::FeatureGeom* hovered_zoning = hovered_zone;
            if (zoning_layer_idx >= 0 && (size_t)zoning_layer_idx < layers.size()) {
                const float qlon = (hovered_parcel->extent.min_lon + hovered_parcel->extent.max_lon) * 0.5f;
                const float qlat = (hovered_parcel->extent.min_lat + hovered_parcel->extent.max_lat) * 0.5f;
                std::vector<uint32_t> zoning_candidates;
                bool have_zoning_candidates = false;
                if ((size_t)zoning_layer_idx < layer_spatial.size() && layer_spatial[(size_t)zoning_layer_idx].built) {
                    have_zoning_candidates = queryLayerSpatialIndex(
                        layer_spatial[(size_t)zoning_layer_idx], qlon, qlat, qlon, qlat, zoning_candidates);
                }
                if (have_zoning_candidates) {
                    const auto& zfeats = layers[(size_t)zoning_layer_idx].features;
                    for (uint32_t zi : zoning_candidates) {
                        if (zi >= zfeats.size()) continue;
                        const auto& zf = zfeats[zi];
                        if (qlon >= zf.extent.min_lon && qlon <= zf.extent.max_lon &&
                            qlat >= zf.extent.min_lat && qlat <= zf.extent.max_lat &&
                            pointInFeature(zf, qlon, qlat)) {
                            hovered_zoning = &zf;
                            break;
                        }
                    }
                }
            }

            ImGui::SetNextWindowSize(ImVec2(460.0f, 0.0f), ImGuiCond_Always);
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(440.0f);
            ImGui::TextUnformatted("Parcel Details");
            ImGui::Separator();
            ImGui::Text("BLOCKLOT: %s", blocklot_raw.empty() ? "(none)" : blocklot_raw.c_str());
            ImGui::Text("Vacant Notices: %d", vac_notice);
            ImGui::Text("Vacant Rehab Records: %d", vac_rehab);
            ImGui::Text("Tax Lien Certificate Records: %d", tax_lien);
            if (tax_lien > 0) ImGui::Text("Tax Lien Total Amount: $%.2f", tax_lien_amount);
            ImGui::Text("Tax Sale 2021 Records: %d", tax_sale);
            if (tax_sale > 0) ImGui::Text("Tax Sale Total Lien: $%.2f", tax_sale_amount);

            const LayerDef::FeatureGeom* hovered_rp = real_property_for_parcel(*hovered_parcel);
            draw_real_property_summary(hovered_rp);

            ImGui::Separator();
            if (hovered_zoning) {
                std::string zone_key = zoningClassKey(*hovered_zoning);
                std::string zone_label = zoningClassLabel(*hovered_zoning);
                std::string zone_description;
                auto meta_it = zoning_metadata.find(zone_key);
                if (meta_it != zoning_metadata.end()) {
                    if (!meta_it->second.label.empty()) zone_label = meta_it->second.label;
                    zone_description = meta_it->second.description;
                }
                const char* display_zone = !zone_label.empty() ? zone_label.c_str() : (zone_key.empty() ? "(unlabeled)" : zone_key.c_str());
                ImGui::SetWindowFontScale(1.45f);
                ImGui::TextWrapped("%s", display_zone);
                ImGui::SetWindowFontScale(1.0f);
                if (!zone_description.empty()) ImGui::TextWrapped("%s", zone_description.c_str());
            } else if (zoning_layer_idx >= 0) {
                ImGui::TextDisabled("Zoning: no intersecting zoning polygon found.");
            }
            ImGui::Separator();
            draw_feature_properties("All Parcel Geometry Fields", *hovered_parcel);
            if (hovered_rp) {
                ImGui::Separator();
                draw_feature_properties("All Real Property Fields", *hovered_rp);
            }
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        if (zoning_hover_active && map_hovered && !(parcel_hover_active && hovered_parcel) && hovered_zone) {
            drawZoningHoverTooltip(*hovered_zone, zoning_metadata);
        }
        ImGui::EndTabItem();
        }
        TimeCubePanelContext time_cube_panel_ctx;
        time_cube_panel_ctx.service = &time_cube_service;
        time_cube_panel_ctx.layers = &layers;
        time_cube_panel_ctx.result = &time_cube_ui_result;
        time_cube_panel_ctx.loaded = &time_cube_ui_loaded;
        time_cube_panel_ctx.status = &time_cube_ui_status;
        time_cube_panel_ctx.mutex = &time_cube_ui_mutex;
        time_cube_panel_ctx.worker = &time_cube_ui_worker;
        time_cube_panel_ctx.running = &time_cube_ui_running;
        time_cube_panel_ctx.done = &time_cube_ui_done;
        time_cube_panel_ctx.selected = &time_cube_selected;
        time_cube_panel_ctx.year_min = &time_cube_year_min;
        time_cube_panel_ctx.year_max = &time_cube_year_max;
        time_cube_panel_ctx.normalize_mode = &time_cube_normalize_mode;
        time_cube_panel_ctx.show_excluded = &time_cube_show_excluded;
        drawTimeCubeTab(time_cube_panel_ctx);
        PolicyPanelContext policy_panel_ctx;
        policy_panel_ctx.hierarchy = &policy_hierarchy;
        policy_panel_ctx.hierarchy_loaded = policy_hierarchy_loaded;
        policy_panel_ctx.hierarchy_error = &policy_hierarchy_error;
        policy_panel_ctx.query = policy_hierarchy_query;
        policy_panel_ctx.query_capacity = sizeof(policy_hierarchy_query);
        policy_panel_ctx.scope = &policy_hierarchy_scope;
        policy_panel_ctx.roster = &public_servant_roster;
        policy_panel_ctx.people_pay_cached_query = &people_pay_cached_query;
        policy_panel_ctx.people_pay_cached_scope = &people_pay_cached_scope;
        policy_panel_ctx.people_pay_cache_matched_count = &people_pay_cache_matched_count;
        policy_panel_ctx.people_pay_visible_rows = &people_pay_visible_rows;
        policy_panel_ctx.people_pay_cache_rebuilds = &people_pay_cache_rebuilds;
        policy_panel_ctx.people_pay_rendered_rows_last = &people_pay_rendered_rows_last;
        policy_panel_ctx.viz_root = &policy_viz_root;
        policy_panel_ctx.viz_cached_query = &policy_viz_cached_query;
        policy_panel_ctx.viz_cached_scope = &policy_viz_cached_scope;
        policy_panel_ctx.viz_cached_metric = &policy_viz_cached_metric;
        policy_panel_ctx.viz_metric = &policy_viz_metric;
        policy_panel_ctx.viz_cache_rebuilds = &policy_viz_cache_rebuilds;
        policy_panel_ctx.viz_node_count = &policy_viz_node_count;
        drawPolicyHierarchyTab(policy_panel_ctx);
        drawVisualModelTabs(layers.size());
        ImGui::EndTabBar();
        }

        ImGui::End();

        FrameFinalizationContext frame_finalization_ctx;
        frame_finalization_ctx.window_data = wd;
        frame_finalization_ctx.last_frame_ts = &last_frame_ts;
        frame_finalization_ctx.ema_frame_ms = &ema_frame_ms;
        frame_finalization_ctx.perf_alpha = kPerfAlpha;
        frame_finalization_ctx.frame_begin = prof_frame_begin;
        frame_finalization_ctx.tiles_drawn_frame = prof_tiles_drawn_frame;
        frame_finalization_ctx.features_considered_frame = prof_features_considered_frame;
        frame_finalization_ctx.features_drawn_frame = prof_features_drawn_frame;
        frame_finalization_ctx.perf_frame_ms_last = &perf_frame_ms_last;
        frame_finalization_ctx.perf_frame_ms_avg = &perf_frame_ms_avg;
        frame_finalization_ctx.perf_fps_avg = &perf_fps_avg;
        frame_finalization_ctx.prof_ui_ms_last = &prof_ui_ms_last;
        frame_finalization_ctx.prof_owner_ms_last = &prof_owner_ms_last;
        frame_finalization_ctx.prof_tile_ms_last = &prof_tile_ms_last;
        frame_finalization_ctx.prof_layer_ms_last = &prof_layer_ms_last;
        frame_finalization_ctx.prof_heatmap_ms_last = &prof_heatmap_ms_last;
        frame_finalization_ctx.prof_overlay_ms_last = &prof_overlay_ms_last;
        frame_finalization_ctx.prof_present_ms_last = &prof_present_ms_last;
        frame_finalization_ctx.prof_tiles_drawn_last = &prof_tiles_drawn_last;
        frame_finalization_ctx.prof_features_considered_last = &prof_features_considered_last;
        frame_finalization_ctx.prof_features_drawn_last = &prof_features_drawn_last;
        frame_finalization_ctx.prof_retired_textures = &prof_retired_textures;
        frame_finalization_ctx.prof_tile_cache_size = &prof_tile_cache_size;
        frame_finalization_ctx.prof_heat_samples_last = &prof_heat_samples_last;
        frame_finalization_ctx.profile_mutex = &profile_mutex;
        frame_finalization_ctx.profile_samples = &profile_samples;
        frame_finalization_ctx.profile_sample_pos = &profile_sample_pos;
        frame_finalization_ctx.profile_sample_count = &profile_sample_count;
        finalizeWorldSimFrame(frame_finalization_ctx);
    }
