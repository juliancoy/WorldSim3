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
        FeatureFilterContext filter_ctx;
        filter_ctx.layers = &layers;
        filter_ctx.map_filters = &map_filter_state;
        filter_ctx.real_property_by_blocklot = &real_property_by_blocklot;
        filter_ctx.parcel_vac_notice_by_feature = &parcel_vac_notice_by_feature;
        filter_ctx.parcel_vac_rehab_by_feature = &parcel_vac_rehab_by_feature;
        filter_ctx.real_property_layer_idx = real_property_layer_idx;
        filter_ctx.parcel_layer_idx = parcel_layer_idx;
        filter_ctx.crime_nibrs_layer_idx = crime_nibrs_layer_idx;
        filter_ctx.crime_legacy_layer_idx = crime_legacy_layer_idx;
        filter_ctx.query_layers = &query_layers;
        auto feature_passes_filters = [&](size_t layer_idx, size_t feature_idx, const LayerDef::FeatureGeom& fg) -> bool {
            return featurePassesFilters(filter_ctx, layer_idx, feature_idx, fg);
        };
        auto query_map_color = [&](size_t layer_idx, size_t feature_idx, const LayerDef::FeatureGeom& fg, ImU32& out_color) -> bool {
            float query_color[4] = {0, 0, 0, 0};
            if (!queryMapColorForFeature(filter_ctx, layer_idx, feature_idx, fg, query_color)) return false;
            out_color = ImGui::ColorConvertFloat4ToU32(ImVec4(query_color[0], query_color[1], query_color[2], query_color[3]));
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
            hs.allow_cpu_fallback = heatmap_allow_cpu_fallback;
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
        const int requested_heatmap_quality_preset = std::clamp(heatmap_quality_preset, 0, 2);
        const int effective_heatmap_quality_preset =
            any_active_gpu_splat && !heatmap_allow_cpu_fallback ? 2 : requested_heatmap_quality_preset;
        const bool high_quality_gpu_aggregate =
            any_active_gpu_splat &&
            !heatmap_allow_cpu_fallback;
        const int smooth_heat_raster_base_px =
            effective_heatmap_quality_preset == 0 ? 1024 :
            effective_heatmap_quality_preset == 1 ? kSmoothHeatRasterBasePx :
            2048;
        const int smooth_heat_raster_max_px =
            effective_heatmap_quality_preset == 0 ? 1536 :
            effective_heatmap_quality_preset == 1 ? kSmoothHeatRasterMaxPx :
            3072;
        auto build_heatmap_key = [&](bool include_view_state) {
            uint64_t key = 1469598103934665603ULL;
            hashCombineU64(key, include_view_state ? 1ULL : 0ULL);
            if (include_view_state) {
                hashCombineU64(key, (uint64_t)zoom);
                hashCombineU64(key, (uint64_t)math_zoom);
            }
            hashCombineFloat(key, global_heat_cell_px);
            hashCombineU64(key, (uint64_t)heatmap_algo);
            hashCombineU64(key, (uint64_t)effective_heatmap_quality_preset);
            hashCombineFloat(key, heatmap_bandwidth_px);
            hashCombineFloat(key, heatmap_blur_sigma_px);
            hashCombineFloat(key, heatmap_percentile_clip);
            hashCombineU64(key, heatmap_zoom_adaptive_bandwidth ? 1ULL : 0ULL);
            hashCombineU64(key, heatmap_multires_enabled ? 1ULL : 0ULL);
            hashCombineFloat(key, heatmap_multires_blend);
            hashCombineU64(key, heatmap_allow_cpu_fallback ? 1ULL : 0ULL);
            hashCombineU64(key, filter_enabled ? 1ULL : 0ULL);
            for (const char* p = filter_blocklot; *p; ++p) hashCombineU64(key, (uint64_t)(unsigned char)(*p));
            for (const char* p = filter_status; *p; ++p) hashCombineU64(key, (uint64_t)(unsigned char)(*p));
            for (const char* p = filter_address; *p; ++p) hashCombineU64(key, (uint64_t)(unsigned char)(*p));
            for (const char* p = filter_owner; *p; ++p) hashCombineU64(key, (uint64_t)(unsigned char)(*p));
            if (!selected_owners.empty()) {
                std::vector<std::string> selected_owner_list(selected_owners.begin(), selected_owners.end());
                std::sort(selected_owner_list.begin(), selected_owner_list.end());
                for (const auto& owner : selected_owner_list) {
                    hashCombineU64(key, (uint64_t)owner.size());
                    for (unsigned char ch : owner) hashCombineU64(key, (uint64_t)ch);
                }
            }
            for (const char* p = filter_zip; *p; ++p) hashCombineU64(key, (uint64_t)(unsigned char)(*p));
            hashCombineU64(key, filter_use_date ? 1ULL : 0ULL);
            hashCombineU64(key, (uint64_t)filter_year_min);
            hashCombineU64(key, (uint64_t)filter_year_max);
            hashCombineU64(key, crime_filter_enabled ? 1ULL : 0ULL);
            hashCombineU64(key, crime_filter_use_year ? 1ULL : 0ULL);
            hashCombineU64(key, (uint64_t)crime_year_min);
            hashCombineU64(key, (uint64_t)crime_year_max);
            hashCombineU64(key, crime_filter_homicide ? 1ULL : 0ULL);
            hashCombineU64(key, crime_filter_robbery ? 1ULL : 0ULL);
            hashCombineU64(key, crime_filter_assault ? 1ULL : 0ULL);
            hashCombineU64(key, crime_filter_burglary ? 1ULL : 0ULL);
            hashCombineU64(key, crime_filter_theft ? 1ULL : 0ULL);
            hashCombineU64(key, crime_filter_auto_theft ? 1ULL : 0ULL);
            hashCombineU64(key, crime_filter_drug ? 1ULL : 0ULL);
            hashCombineU64(key, crime_filter_shooting ? 1ULL : 0ULL);
            hashCombineU64(key, (uint64_t)query_layers.size());
            for (const QueryMapLayer& query_layer : query_layers) {
                hashCombineU64(key, query_layer.enabled ? 1ULL : 0ULL);
                for (float channel : query_layer.color) hashCombineFloat(key, channel);
                hashCombineU64(key, (uint64_t)query_layer.result_set.features.size());
                hashCombineU64(key, (uint64_t)query_layer.result_set.blocklots.size());
                hashCombineU64(key, (uint64_t)query_layer.result_set.owners.size());
                for (unsigned char ch : query_layer.name) hashCombineU64(key, (uint64_t)ch);
            }
            for (size_t i = 0; i < layers.size(); ++i) {
                const bool layer_contributes_heatmap = layer_uses_heatmap_aggregate(i);
                hashCombineU64(key, layer_contributes_heatmap ? 1ULL : 0ULL);
                if (!layer_contributes_heatmap) continue;
                hashCombineU64(key, (uint64_t)((i < layer_heatmap_max_zoom.size()) ? layer_heatmap_max_zoom[i] : 13));
                hashCombineU64(key, (uint64_t)((i < layer_parcel_detail_min_zoom.size()) ? layer_parcel_detail_min_zoom[i] : kParcelChoroplethMinZoom));
                hashCombineU64(key, (i < layer_heatmap_use_gradient.size() && layer_heatmap_use_gradient[i]) ? 1ULL : 0ULL);
                hashCombineU64(key, (uint64_t)((i < layer_heatmap_algo.size()) ? (layer_heatmap_algo[i] + 1) : 0));
                hashCombineU64(key, (uint64_t)((i < layer_normalize_mode.size()) ? layer_normalize_mode[i] : 0));
                hashCombineFloat(key, i < layer_heatmap_cell_px.size() ? layer_heatmap_cell_px[i] : global_heat_cell_px);
                hashCombineFloat(key, i < layer_heatmap_bandwidth_px.size() ? layer_heatmap_bandwidth_px[i] : heatmap_bandwidth_px);
                hashCombineFloat(key, i < layer_heatmap_blur_sigma_px.size() ? layer_heatmap_blur_sigma_px[i] : heatmap_blur_sigma_px);
                hashCombineFloat(key, i < layer_heatmap_percentile_clip.size() ? layer_heatmap_percentile_clip[i] : heatmap_percentile_clip);
                hashCombineU64(key, (i < layer_heatmap_zoom_adaptive_bandwidth.size() && layer_heatmap_zoom_adaptive_bandwidth[i]) ? 1ULL : 0ULL);
                hashCombineU64(key, (i < layer_heatmap_multires_enabled.size() && layer_heatmap_multires_enabled[i]) ? 1ULL : 0ULL);
                hashCombineFloat(key, i < layer_heatmap_multires_blend.size() ? layer_heatmap_multires_blend[i] : heatmap_multires_blend);
                hashCombineU64(key, (uint64_t)layers[i].features.size());
                const std::string sig = fileSignature(root / "data" / "layers" / layers[i].file);
                for (unsigned char ch : sig) hashCombineU64(key, (uint64_t)ch);
            }
            return key;
        };
        const uint64_t heatmap_view_key = build_heatmap_key(true);
        const uint64_t heatmap_data_key = build_heatmap_key(false);
        const uint64_t heatmap_key = high_quality_gpu_aggregate ? heatmap_data_key : heatmap_view_key;
        auto aggregate_cache_path = [&](uint64_t key) {
            std::ostringstream name;
            name << std::hex << key << ".raster.bin";
            return root / "data" / "cache" / "aggregate" / name.str();
        };
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
                if (!high_quality_gpu_aggregate) {
                    saveHeatmapRasterCache(aggregate_cache_path(done.first), done.first, done.second.raster);
                }
                aggregate_entry.raster = done.second.raster;
                aggregate_has_texture = uploadRgbaTexture(
                    done.second.raster.rgba.data(),
                    (uint32_t)done.second.raster.w,
                    (uint32_t)done.second.raster.h,
                    aggregate_entry.texture);
            }
            const bool aggregate_has_content = aggregate_has_texture || !aggregate_entry.cells.empty();
            if (aggregate_has_content || !any_active_heatmap) {
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
            } else if (!heatmap_async_inflight && !high_quality_gpu_aggregate) {
                HeatmapRaster disk_raster;
                if (loadHeatmapRasterCache(aggregate_cache_path(heatmap_key), heatmap_key, disk_raster)) {
                    CachedAggregateTexture disk_entry;
                    disk_entry.raster = std::move(disk_raster);
                    disk_entry.last_used_frame = ++heatmap_texture_cache_frame;
                    if (uploadRgbaTexture(
                            disk_entry.raster.rgba.data(),
                            (uint32_t)disk_entry.raster.w,
                            (uint32_t)disk_entry.raster.h,
                            disk_entry.texture)) {
                        auto inserted = heatmap_texture_cache.emplace(heatmap_key, std::move(disk_entry));
                        prune_heatmap_texture_cache();
                        auto current = heatmap_texture_cache.find(heatmap_key);
                        if (current != heatmap_texture_cache.end()) {
                            cached_aggregate_for_key = &current->second;
                            heatmap_cache_key = heatmap_key;
                            heatmap_cache_valid = true;
                        }
                    }
                }
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
        std::vector<size_t> draw_layer_order;
        draw_layer_order.reserve(layers.size());
        std::vector<size_t> parcel_related_draw_layers;
        parcel_related_draw_layers.reserve(5);
        size_t zoning_draw_layer = (size_t)-1;
        for (size_t layer_idx = 0; layer_idx < layers.size(); ++layer_idx) {
            if ((int)layer_idx == zoning_layer_idx) {
                zoning_draw_layer = layer_idx;
                continue;
            }
            if (isParcelRelatedLayer(filter_ctx, layer_idx)) {
                parcel_related_draw_layers.push_back(layer_idx);
                continue;
            }
            draw_layer_order.push_back(layer_idx);
        }
        if (zoning_draw_layer != (size_t)-1) draw_layer_order.push_back(zoning_draw_layer);
        draw_layer_order.insert(
            draw_layer_order.end(),
            parcel_related_draw_layers.begin(),
            parcel_related_draw_layers.end());
        for (size_t layer_idx : draw_layer_order) {
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
            if (high_quality_gpu_aggregate && should_recompute_heatmap && layer_uses_heatmap_for_cache) have_candidates = false;
            if (have_candidates) {
                have_candidates = queryLayerSpatialIndex(
                    layer_spatial[layer_idx], view_min_lon, view_min_lat, view_max_lon, view_max_lat, render_candidates);
            }
            if (have_candidates) {
                for (uint32_t fidx : render_candidates) {
                    prof_features_considered_frame++;
                    const bool layer_uses_heatmap = layer_uses_heatmap_aggregate(layer_idx);
                    if (!high_quality_gpu_aggregate && smooth_only_heatmap && should_recompute_heatmap && layer_uses_heatmap && fidx % 2 != 0) continue;
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
                    query_map_color(layer_idx, (size_t)fidx, fg, feature_c);
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
                (!high_quality_gpu_aggregate && smooth_only_heatmap && should_recompute_heatmap && layer_uses_heatmap_for_cache && l.features.size() > kMaxSmoothHeatSamplesPerLayer)
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
                query_map_color(layer_idx, fi, fg, feature_c);
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
                    smooth_heat_raster_base_px,
                    smooth_heat_raster_max_px);
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
        const CachedAggregateTexture* draw_cached_aggregate = cached_aggregate_for_key;
        uint64_t draw_cached_aggregate_key = heatmap_key;
        if (!draw_cached_aggregate && heatmap_async_inflight && heatmap_cache_valid && heatmap_cache_key != 0) {
            auto stale_it = heatmap_texture_cache.find(heatmap_cache_key);
            if (stale_it != heatmap_texture_cache.end() && stale_it->second.texture.descriptor) {
                stale_it->second.last_used_frame = ++heatmap_texture_cache_frame;
                draw_cached_aggregate = &stale_it->second;
                draw_cached_aggregate_key = heatmap_cache_key;
            }
        }
        const std::vector<CachedHeatCell> empty_heat_cells;
        const std::vector<CachedHeatCell>& draw_cells =
            draw_cached_aggregate ? draw_cached_aggregate->cells :
            (can_use_cached_heatmap ? heatmap_cached_cells :
            (should_recompute_heatmap ? frame_heat_cells : empty_heat_cells));
        const bool cached_aggregate_has_texture =
            draw_cached_aggregate && draw_cached_aggregate->texture.descriptor;
        MapHeatmapDrawContext heatmap_draw_ctx;
        heatmap_draw_ctx.draw = draw;
        heatmap_draw_ctx.math_zoom = math_zoom;
        heatmap_draw_ctx.smooth_only_heatmap = any_active_heatmap && smooth_only_heatmap;
        heatmap_draw_ctx.heatmap_async_inflight = any_active_heatmap && heatmap_async_inflight;
        heatmap_draw_ctx.heatmap_raster_texture_valid =
            any_active_heatmap && (cached_aggregate_has_texture || heatmap_raster_texture_valid);
        heatmap_draw_ctx.heatmap_raster_cache_key = cached_aggregate_has_texture ? draw_cached_aggregate_key : heatmap_raster_cache_key;
        heatmap_draw_ctx.heatmap_key = cached_aggregate_has_texture ? draw_cached_aggregate_key : heatmap_key;
        heatmap_draw_ctx.heatmap_raster_texture =
            cached_aggregate_has_texture ? &draw_cached_aggregate->texture : &heatmap_raster_texture;
        heatmap_draw_ctx.heatmap_raster =
            draw_cached_aggregate ? &draw_cached_aggregate->raster : &heatmap_cached_raster_meta;
        heatmap_draw_ctx.draw_cells = &draw_cells;
        heatmap_draw_ctx.project_world = project_world;
        prof_heatmap_gpu_splat_active.store(any_active_gpu_splat, std::memory_order_relaxed);
        prof_heatmap_high_quality.store(high_quality_gpu_aggregate, std::memory_order_relaxed);
        prof_heatmap_cache_valid.store(can_use_cached_heatmap, std::memory_order_relaxed);
        prof_heatmap_texture_resident.store(
            heatmap_draw_ctx.heatmap_raster_texture_valid &&
            heatmap_draw_ctx.heatmap_raster_texture &&
            heatmap_draw_ctx.heatmap_raster_texture->descriptor,
            std::memory_order_relaxed);
        prof_heatmap_async_inflight.store(heatmap_async_inflight, std::memory_order_relaxed);
        prof_heatmap_cache_key.store(heatmap_cache_valid ? heatmap_cache_key : 0, std::memory_order_relaxed);
        prof_heatmap_texture_cache_entries.store(heatmap_texture_cache.size(), std::memory_order_relaxed);
        drawHeatmapPass(heatmap_draw_ctx);
        if (any_active_gpu_splat && heatmap_async_inflight && !can_use_cached_heatmap) {
            drawMapStatusBadge(draw, origin, "Generating Aggregate");
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
        renderSelectedParcelOutlines(MapSelectionRenderContext{
            draw,
            origin,
            size,
            &layers,
            parcel_layer_idx,
            &selected_parcel_indices,
            math_zoom,
            &projection_cache,
            project_world
        });

        const MapFillStats& fill_stats = projection_cache.fillStats();
        render_fill_attempts_last_frame.store(fill_stats.attempts, std::memory_order_relaxed);
        render_fill_success_last_frame.store(fill_stats.success, std::memory_order_relaxed);
        render_fill_no_triangles_last_frame.store(fill_stats.no_triangles, std::memory_order_relaxed);
        render_fill_bad_indices_last_frame.store(fill_stats.bad_indices, std::memory_order_relaxed);

        drawMapZoomBadge(draw, origin, size, zoom);

        handleMapInspection(MapInspectionContext{
            map_hovered,
            parcel_hover_active,
            parcel_inspect_active,
            zoning_hover_active,
            zoning_inspect_active,
            parcel_layer_idx,
            zoning_layer_idx,
            &layers,
            &layer_spatial,
            &zoning_metadata,
            &parcel_selection,
            [&](size_t idx) { openElementParcelPage(element_info_state, idx); },
            &show_selected_zone_details,
            &selected_zone_idx,
            &hover_state,
            &parcel_vac_notice_by_feature,
            &parcel_vac_rehab_by_feature,
            &parcel_tax_lien_by_feature,
            &parcel_tax_sale_by_feature,
            &parcel_tax_lien_amount_by_feature,
            &parcel_tax_sale_amount_by_feature,
            real_property_for_parcel
        });

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
        drawMapOverlayPanelsPopup(origin, size, time_cube_panel_ctx, policy_panel_ctx, layers.size());

        ImGui::EndTabItem();
        }
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

        if (download_queue_window && !glfwWindowShouldClose(download_queue_window)) {
            if (download_queue_swapchain_rebuild) {
                glfwGetFramebufferSize(download_queue_window, &dq_w, &dq_h);
                if (dq_w > 0 && dq_h > 0) {
                    ImGui::SetCurrentContext(download_queue_imgui_context);
                    ImGui_ImplVulkan_SetMinImageCount(g_MinImageCount);
                    ImGui_ImplVulkanH_CreateOrResizeWindow(
                        g_Instance,
                        g_PhysicalDevice,
                        g_Device,
                        &download_queue_wd,
                        g_QueueFamily,
                        g_Allocator,
                        dq_w,
                        dq_h,
                        g_MinImageCount);
                    download_queue_wd.FrameIndex = 0;
                    download_queue_swapchain_rebuild = false;
                }
            }
            if (!download_queue_swapchain_rebuild) {
                ImGui::SetCurrentContext(download_queue_imgui_context);
                ImGui_ImplVulkan_NewFrame();
                ImGui_ImplGlfw_NewFrame();
                ImGui::NewFrame();
                ImGuiIO& queue_io = ImGui::GetIO();
                queue_io.FontGlobalScale = ui_text_scale;
                ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
                ImGui::SetNextWindowSize(queue_io.DisplaySize, ImGuiCond_Always);
                if (ImGui::Begin(
                        "Download Queue",
                        nullptr,
                        ImGuiWindowFlags_NoDecoration |
                            ImGuiWindowFlags_NoMove |
                            ImGuiWindowFlags_NoResize |
                            ImGuiWindowFlags_NoCollapse)) {
                    drawBasemapDownloadQueueSection(basemap_download, root, data_library_status_msg, resolve_download_label);
                    drawLazyTileDownloadQueueSection(lazy_tile_download);
                    ImGui::SeparatorText("Layer Data");
                    if (layer_download_inflight && layer_download_active_idx < layers.size()) {
                        ImGui::Text("Active: %s", layers[layer_download_active_idx].name.c_str());
                        ImGui::TextDisabled("%s", layers[layer_download_active_idx].file.c_str());
                    } else {
                        ImGui::TextDisabled("No active layer download");
                    }
                    if (layer_download_queue.empty()) {
                        ImGui::TextDisabled("No queued layer downloads");
                    } else {
                        for (size_t i = 0; i < layer_download_queue.size(); ++i) {
                            const size_t idx = layer_download_queue[i];
                            if (idx < layers.size()) ImGui::BulletText("%zu. %s", i + 1, layers[idx].name.c_str());
                        }
                    }
                    if (!layer_download_last_event.empty()) ImGui::TextDisabled("Last queue event: %s", layer_download_last_event.c_str());
                }
                ImGui::End();
                ImGui::Render();
                ImDrawData* queue_draw_data = ImGui::GetDrawData();
                if (queue_draw_data->DisplaySize.x > 0.0f && queue_draw_data->DisplaySize.y > 0.0f) {
                    download_queue_wd.ClearValue.color.float32[0] = 0.95f;
                    download_queue_wd.ClearValue.color.float32[1] = 0.95f;
                    download_queue_wd.ClearValue.color.float32[2] = 0.96f;
                    download_queue_wd.ClearValue.color.float32[3] = 1.00f;
                    FrameRenderSecondary(&download_queue_wd, queue_draw_data, download_queue_swapchain_rebuild);
                    FramePresentSecondary(&download_queue_wd, download_queue_swapchain_rebuild);
                }
                ImGui::SetCurrentContext(main_imgui_context);
            }
        }
    }
