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
        auto try_get_prop_float = [&](const LayerDef::FeatureGeom& fg, const std::string& key, float& out) -> bool {
            if (key.empty()) return false;
            for (const auto& kv : fg.properties) {
                if (kv.first != key) continue;
                char* end = nullptr;
                const float v = std::strtof(kv.second.c_str(), &end);
                if (end == kv.second.c_str() || (end && *end != '\0')) return false;
                if (!std::isfinite(v)) return false;
                out = v;
                return true;
            }
            return false;
        };
        auto heat_color = [&](float t) -> ImVec4 {
            t = std::clamp(t, 0.0f, 1.0f);
            const ImVec4 cold(0.12f, 0.35f, 0.75f, 1.0f);
            const ImVec4 mid(0.98f, 0.83f, 0.26f, 1.0f);
            const ImVec4 hot(0.82f, 0.14f, 0.12f, 1.0f);
            auto lerp = [](const ImVec4& a, const ImVec4& b, float u) {
                return ImVec4(
                    a.x + (b.x - a.x) * u,
                    a.y + (b.y - a.y) * u,
                    a.z + (b.z - a.z) * u,
                    a.w + (b.w - a.w) * u);
            };
            if (t < 0.5f) return lerp(cold, mid, t * 2.0f);
            return lerp(mid, hot, (t - 0.5f) * 2.0f);
        };
        auto hash_combine_u64 = [](uint64_t& seed, uint64_t v) {
            seed ^= v + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
        };
        auto hash_combine_f = [&](uint64_t& seed, float v) {
            uint32_t bits = 0;
            std::memcpy(&bits, &v, sizeof(bits));
            hash_combine_u64(seed, (uint64_t)bits);
        };
        auto hash_combine_qd = [&](uint64_t& seed, double v, double scale) {
            if (!std::isfinite(v)) {
                hash_combine_u64(seed, 0xffffffffffffffffULL);
                return;
            }
            const int64_t q = (int64_t)std::llround(v * scale);
            hash_combine_u64(seed, (uint64_t)q);
        };
        const float global_heat_cell = std::max(4.0f, global_heat_cell_px);
        std::vector<HeatSample> heat_samples;
        auto resolve_layer_heat_settings = [&](size_t layer_idx, HeatSample& hs) {
            const bool use_global = layer_idx >= layer_heatmap_use_global_settings.size() || layer_heatmap_use_global_settings[layer_idx];
            hs.layer = (int)layer_idx;
            hs.algo = (layer_idx < layer_heatmap_algo.size() && layer_heatmap_algo[layer_idx] >= 0) ? layer_heatmap_algo[layer_idx] : heatmap_algo;
            hs.cell_px = std::max(4.0f, use_global || layer_idx >= layer_heatmap_cell_px.size() ? global_heat_cell_px : layer_heatmap_cell_px[layer_idx]);
            hs.bandwidth_px = std::max(1.0f, use_global || layer_idx >= layer_heatmap_bandwidth_px.size() ? heatmap_bandwidth_px : layer_heatmap_bandwidth_px[layer_idx]);
            hs.blur_sigma_px = std::max(0.0f, use_global || layer_idx >= layer_heatmap_blur_sigma_px.size() ? heatmap_blur_sigma_px : layer_heatmap_blur_sigma_px[layer_idx]);
            hs.percentile_clip = std::clamp(use_global || layer_idx >= layer_heatmap_percentile_clip.size() ? heatmap_percentile_clip : layer_heatmap_percentile_clip[layer_idx], 50.0f, 100.0f);
            hs.zoom_adaptive_bandwidth = use_global || layer_idx >= layer_heatmap_zoom_adaptive_bandwidth.size() ? heatmap_zoom_adaptive_bandwidth : layer_heatmap_zoom_adaptive_bandwidth[layer_idx];
            hs.multires_enabled = use_global || layer_idx >= layer_heatmap_multires_enabled.size() ? heatmap_multires_enabled : layer_heatmap_multires_enabled[layer_idx];
            hs.multires_blend = std::clamp(use_global || layer_idx >= layer_heatmap_multires_blend.size() ? heatmap_multires_blend : layer_heatmap_multires_blend[layer_idx], 0.0f, 1.0f);
        };
        bool smooth_only_heatmap = true;
        bool any_active_heatmap = false;
        for (size_t i = 0; i < layers.size(); ++i) {
            const bool active_heatmap =
                layers[i].enabled &&
                i < layer_heatmap_enabled.size() &&
                i < layer_heatmap_max_zoom.size() &&
                layer_heatmap_enabled[i] &&
                zoom <= layer_heatmap_max_zoom[i];
            if (!active_heatmap) continue;
            any_active_heatmap = true;
            const int algo = (i < layer_heatmap_algo.size() && layer_heatmap_algo[i] >= 0) ? layer_heatmap_algo[i] : heatmap_algo;
            if (algo != 1 && algo != 2 && algo != 4) smooth_only_heatmap = false;
        }
        if (!any_active_heatmap) smooth_only_heatmap = false;
        uint64_t heatmap_key = 1469598103934665603ULL;
        hash_combine_u64(heatmap_key, (uint64_t)zoom);
        hash_combine_u64(heatmap_key, (uint64_t)math_zoom);
        hash_combine_qd(heatmap_key, center_lon, 10000000.0);
        hash_combine_qd(heatmap_key, center_lat, 10000000.0);
        hash_combine_qd(heatmap_key, view_min_lon, 10000000.0);
        hash_combine_qd(heatmap_key, view_min_lat, 10000000.0);
        hash_combine_qd(heatmap_key, view_max_lon, 10000000.0);
        hash_combine_qd(heatmap_key, view_max_lat, 10000000.0);
        hash_combine_u64(heatmap_key, (uint64_t)std::max(1, (int)std::lround(size.x)));
        hash_combine_u64(heatmap_key, (uint64_t)std::max(1, (int)std::lround(size.y)));
        hash_combine_f(heatmap_key, global_heat_cell_px);
        hash_combine_u64(heatmap_key, (uint64_t)heatmap_algo);
        hash_combine_u64(heatmap_key, (uint64_t)heatmap_quality_preset);
        hash_combine_f(heatmap_key, heatmap_bandwidth_px);
        hash_combine_f(heatmap_key, heatmap_blur_sigma_px);
        hash_combine_f(heatmap_key, heatmap_percentile_clip);
        hash_combine_u64(heatmap_key, heatmap_zoom_adaptive_bandwidth ? 1ULL : 0ULL);
        hash_combine_u64(heatmap_key, heatmap_multires_enabled ? 1ULL : 0ULL);
        hash_combine_f(heatmap_key, heatmap_multires_blend);
        hash_combine_u64(heatmap_key, filter_enabled ? 1ULL : 0ULL);
        for (const char* p = filter_blocklot; *p; ++p) hash_combine_u64(heatmap_key, (uint64_t)(unsigned char)(*p));
        for (const char* p = filter_status; *p; ++p) hash_combine_u64(heatmap_key, (uint64_t)(unsigned char)(*p));
        for (const char* p = filter_address; *p; ++p) hash_combine_u64(heatmap_key, (uint64_t)(unsigned char)(*p));
        for (const char* p = filter_owner; *p; ++p) hash_combine_u64(heatmap_key, (uint64_t)(unsigned char)(*p));
        if (!selected_owners.empty()) {
            std::vector<std::string> selected_owner_list(selected_owners.begin(), selected_owners.end());
            std::sort(selected_owner_list.begin(), selected_owner_list.end());
            for (const auto& owner : selected_owner_list) {
                hash_combine_u64(heatmap_key, (uint64_t)owner.size());
                for (unsigned char ch : owner) hash_combine_u64(heatmap_key, (uint64_t)ch);
            }
        }
        for (const char* p = filter_zip; *p; ++p) hash_combine_u64(heatmap_key, (uint64_t)(unsigned char)(*p));
        hash_combine_u64(heatmap_key, filter_use_date ? 1ULL : 0ULL);
        hash_combine_u64(heatmap_key, (uint64_t)filter_year_min);
        hash_combine_u64(heatmap_key, (uint64_t)filter_year_max);
        hash_combine_u64(heatmap_key, crime_filter_enabled ? 1ULL : 0ULL);
        hash_combine_u64(heatmap_key, crime_filter_use_year ? 1ULL : 0ULL);
        hash_combine_u64(heatmap_key, (uint64_t)crime_year_min);
        hash_combine_u64(heatmap_key, (uint64_t)crime_year_max);
        hash_combine_u64(heatmap_key, crime_filter_homicide ? 1ULL : 0ULL);
        hash_combine_u64(heatmap_key, crime_filter_robbery ? 1ULL : 0ULL);
        hash_combine_u64(heatmap_key, crime_filter_assault ? 1ULL : 0ULL);
        hash_combine_u64(heatmap_key, crime_filter_burglary ? 1ULL : 0ULL);
        hash_combine_u64(heatmap_key, crime_filter_theft ? 1ULL : 0ULL);
        hash_combine_u64(heatmap_key, crime_filter_auto_theft ? 1ULL : 0ULL);
        hash_combine_u64(heatmap_key, crime_filter_drug ? 1ULL : 0ULL);
        hash_combine_u64(heatmap_key, crime_filter_shooting ? 1ULL : 0ULL);
        for (size_t i = 0; i < layers.size(); ++i) {
            hash_combine_u64(heatmap_key, layers[i].enabled ? 1ULL : 0ULL);
            hash_combine_u64(heatmap_key, (i < layer_heatmap_enabled.size() && layer_heatmap_enabled[i]) ? 1ULL : 0ULL);
            hash_combine_u64(heatmap_key, (uint64_t)((i < layer_heatmap_max_zoom.size()) ? layer_heatmap_max_zoom[i] : 13));
            hash_combine_u64(heatmap_key, (i < layer_heatmap_use_gradient.size() && layer_heatmap_use_gradient[i]) ? 1ULL : 0ULL);
            hash_combine_u64(heatmap_key, (uint64_t)((i < layer_heatmap_algo.size()) ? (layer_heatmap_algo[i] + 1) : 0));
            const bool use_global = i >= layer_heatmap_use_global_settings.size() || layer_heatmap_use_global_settings[i];
            hash_combine_u64(heatmap_key, use_global ? 1ULL : 0ULL);
            if (!use_global) {
                hash_combine_f(heatmap_key, i < layer_heatmap_cell_px.size() ? layer_heatmap_cell_px[i] : global_heat_cell_px);
                hash_combine_f(heatmap_key, i < layer_heatmap_bandwidth_px.size() ? layer_heatmap_bandwidth_px[i] : heatmap_bandwidth_px);
                hash_combine_f(heatmap_key, i < layer_heatmap_blur_sigma_px.size() ? layer_heatmap_blur_sigma_px[i] : heatmap_blur_sigma_px);
                hash_combine_f(heatmap_key, i < layer_heatmap_percentile_clip.size() ? layer_heatmap_percentile_clip[i] : heatmap_percentile_clip);
                hash_combine_u64(heatmap_key, (i < layer_heatmap_zoom_adaptive_bandwidth.size() && layer_heatmap_zoom_adaptive_bandwidth[i]) ? 1ULL : 0ULL);
                hash_combine_u64(heatmap_key, (i < layer_heatmap_multires_enabled.size() && layer_heatmap_multires_enabled[i]) ? 1ULL : 0ULL);
                hash_combine_f(heatmap_key, i < layer_heatmap_multires_blend.size() ? layer_heatmap_multires_blend[i] : heatmap_multires_blend);
            }
            hash_combine_u64(heatmap_key, (uint64_t)layers[i].features.size());
        }
        if (heatmap_async_inflight && heatmap_async_future.valid() &&
            heatmap_async_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            const auto heat_prof_begin = std::chrono::steady_clock::now();
            auto done = heatmap_async_future.get();
            heatmap_async_inflight = false;
            if (done.first == heatmap_key) {
                heatmap_cached_cells = std::move(done.second.cells);
                if (done.second.has_raster && done.second.raster.w > 0 && done.second.raster.h > 0 && !done.second.raster.rgba.empty()) {
                    heatmap_cached_raster_meta = done.second.raster;
                    heatmap_raster_texture_valid = uploadRgbaTexture(
                        done.second.raster.rgba.data(),
                        (uint32_t)done.second.raster.w,
                        (uint32_t)done.second.raster.h,
                        heatmap_raster_texture);
                    if (heatmap_raster_texture_valid) heatmap_raster_cache_key = done.first;
                } else {
                    if (!smooth_only_heatmap) {
                        destroyTileTexture(heatmap_raster_texture);
                        heatmap_raster_texture_valid = false;
                        heatmap_cached_raster_meta = {};
                        heatmap_raster_cache_key = 0;
                    }
                }
                heatmap_cache_key = done.first;
                heatmap_cache_valid = true;
            }
            prof_heatmap_ms_last.store(prof_ms_since(heat_prof_begin), std::memory_order_relaxed);
        }
        const bool can_use_cached_heatmap = heatmap_cache_valid && heatmap_cache_key == heatmap_key;
        bool should_recompute_heatmap = (!can_use_cached_heatmap) && (!heatmap_controls_active);
        const auto layer_prof_begin = std::chrono::steady_clock::now();
        for (size_t layer_idx = 0; layer_idx < layers.size(); ++layer_idx) {
            auto& l = layers[layer_idx];
            if (!l.enabled) continue;
            const bool project_to_parcels_only =
                (vacant_notice_layer_idx >= 0 && (int)layer_idx == vacant_notice_layer_idx) ||
                (vacant_rehab_layer_idx >= 0 && (int)layer_idx == vacant_rehab_layer_idx) ||
                (tax_lien_layer_idx >= 0 && (int)layer_idx == tax_lien_layer_idx) ||
                (tax_sale_layer_idx >= 0 && (int)layer_idx == tax_sale_layer_idx);
            if (project_to_parcels_only) continue;
            const bool is_zoning_layer = ((int)layer_idx == zoning_layer_idx);
            const bool is_heat_layer = !l.heatmap_field.empty();
            float heat_min = std::numeric_limits<float>::infinity();
            float heat_max = -std::numeric_limits<float>::infinity();
            if (is_heat_layer) {
                for (size_t fi = 0; fi < l.features.size(); ++fi) {
                    const auto& fg = l.features[fi];
                    if (!feature_passes_filters(layer_idx, fi, fg)) continue;
                    float v = 0.0f;
                    if (!try_get_prop_float(fg, l.heatmap_field, v)) continue;
                    heat_min = std::min(heat_min, v);
                    heat_max = std::max(heat_max, v);
                }
            }
            const bool heat_range_valid = std::isfinite(heat_min) && std::isfinite(heat_max) && heat_max > heat_min;
            ImU32 c = ImGui::ColorConvertFloat4ToU32(l.color);
            const bool layer_uses_heatmap_for_cache =
                layer_idx < layer_heatmap_enabled.size() &&
                layer_idx < layer_heatmap_max_zoom.size() &&
                layer_heatmap_enabled[layer_idx] &&
                zoom <= layer_heatmap_max_zoom[layer_idx];
            bool have_candidates = !should_recompute_heatmap || !layer_uses_heatmap_for_cache;
            if (have_candidates) {
                have_candidates = queryLayerSpatialIndex(
                    layer_spatial[layer_idx], view_min_lon, view_min_lat, view_max_lon, view_max_lat, render_candidates);
            }
            if (have_candidates) {
                for (uint32_t fidx : render_candidates) {
                    prof_features_considered_frame++;
                    const bool layer_uses_heatmap =
                        layer_idx < layer_heatmap_enabled.size() &&
                        layer_idx < layer_heatmap_max_zoom.size() &&
                        layer_heatmap_enabled[layer_idx] &&
                        zoom <= layer_heatmap_max_zoom[layer_idx];
                    if (smooth_only_heatmap && should_recompute_heatmap && layer_uses_heatmap && fidx % 2 != 0) continue;
                    if (fidx >= l.features.size()) continue;
                    auto& fg = l.features[(size_t)fidx];
                    if (!feature_passes_filters(layer_idx, (size_t)fidx, fg)) continue;
                    ImU32 feature_c = c;
                    if (is_heat_layer) {
                        float v = 0.0f;
                        if (try_get_prop_float(fg, l.heatmap_field, v) && heat_range_valid) {
                            const float t = (v - heat_min) / (heat_max - heat_min);
                            feature_c = ImGui::ColorConvertFloat4ToU32(heat_color(t));
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
                        const ImVec2 center((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
                        HeatSample hs;
                        hs.x = center.x;
                        hs.y = center.y;
                        hs.lon = (fg.extent.min_lon + fg.extent.max_lon) * 0.5f;
                        hs.lat = (fg.extent.min_lat + fg.extent.max_lat) * 0.5f;
                        hs.color = ImGui::ColorConvertU32ToFloat4(feature_c);
                        hs.prefer_gradient = layer_idx < layer_heatmap_use_gradient.size() ? layer_heatmap_use_gradient[layer_idx] : true;
                        resolve_layer_heat_settings(layer_idx, hs);
                        heat_samples.push_back(hs);
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
                            append_world_ring_line(r);
                            draw->AddPolyline(scratch_line.data(), (int)scratch_line.size(), feature_c, ImDrawFlags_Closed, 1.0f);
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
                if (is_heat_layer) {
                    float v = 0.0f;
                    if (try_get_prop_float(fg, l.heatmap_field, v) && heat_range_valid) {
                        const float t = (v - heat_min) / (heat_max - heat_min);
                        feature_c = ImGui::ColorConvertFloat4ToU32(heat_color(t));
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
                const bool layer_uses_heatmap =
                    layer_idx < layer_heatmap_enabled.size() &&
                    layer_idx < layer_heatmap_max_zoom.size() &&
                    layer_heatmap_enabled[layer_idx] &&
                    zoom <= layer_heatmap_max_zoom[layer_idx];
                if (!layer_uses_heatmap && (p1.x < origin.x || p0.x > origin.x + size.x || p1.y < origin.y || p0.y > origin.y + size.y)) continue;
                if (layer_uses_heatmap) {
                    if (can_use_cached_heatmap) continue;
                    if (smooth_sample_stride > 1 && (fi % smooth_sample_stride) != 0) continue;
                    const ImVec2 center((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
                    HeatSample hs;
                    hs.x = center.x;
                    hs.y = center.y;
                    hs.lon = (fg.extent.min_lon + fg.extent.max_lon) * 0.5f;
                    hs.lat = (fg.extent.min_lat + fg.extent.max_lat) * 0.5f;
                    hs.color = ImGui::ColorConvertU32ToFloat4(feature_c);
                    hs.prefer_gradient = layer_idx < layer_heatmap_use_gradient.size() ? layer_heatmap_use_gradient[layer_idx] : true;
                    resolve_layer_heat_settings(layer_idx, hs);
                    heat_samples.push_back(hs);
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
                    if (layer_idx == (size_t)parcel_layer_idx && (vacant_notice_enabled || vacant_rehab_enabled)) {
                        int vac_notice = 0;
                        int vac_rehab = 0;
                        if (fi < parcel_vac_notice_by_feature.size()) vac_notice = parcel_vac_notice_by_feature[fi];
                        if (fi < parcel_vac_rehab_by_feature.size()) vac_rehab = parcel_vac_rehab_by_feature[fi];

                        int weight = 0;
                        if (vacant_notice_enabled) weight += vac_notice;
                        if (vacant_rehab_enabled) weight += vac_rehab;
                        if (weight > 0) {
                            const bool notice_fill = vacant_notice_enabled &&
                                vacant_notice_layer_idx >= 0 &&
                                (size_t)vacant_notice_layer_idx < layer_fill_enabled.size() &&
                                layer_fill_enabled[(size_t)vacant_notice_layer_idx];
                            const bool rehab_fill = vacant_rehab_enabled &&
                                vacant_rehab_layer_idx >= 0 &&
                                (size_t)vacant_rehab_layer_idx < layer_fill_enabled.size() &&
                                layer_fill_enabled[(size_t)vacant_rehab_layer_idx];
                            if ((notice_fill || rehab_fill) && should_fill_layer_polygon((size_t)parcel_layer_idx)) {
                                const int alpha = std::clamp(95 + weight * 16, 95, 220);
                                ImU32 vac_fill = color_with_alpha(vacancy_base_color(vac_notice, vac_rehab), alpha);
                                draw_tessellated_fill(fg, world_rings, vac_fill);
                            }
                        }
                    }

                    for (const auto& r : world_rings) {
                        append_world_ring_line(r);
                        draw->AddPolyline(scratch_line.data(), (int)scratch_line.size(), feature_c, ImDrawFlags_Closed, 1.0f);
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
        if (should_recompute_heatmap && !heatmap_async_inflight) {
            const auto heat_prof_begin = std::chrono::steady_clock::now();
            const auto samples_copy = heat_samples;
            const uint64_t key_copy = heatmap_key;
            const float cell_copy = global_heat_cell;
            const float bw_copy = heatmap_bandwidth_px;
            const float blur_copy = heatmap_blur_sigma_px;
            const float clip_copy = heatmap_percentile_clip;
            const bool adaptive_copy = heatmap_zoom_adaptive_bandwidth;
            const bool multires_enabled_copy = heatmap_multires_enabled;
            const float multires_blend_copy = heatmap_multires_blend;
            const int zoom_copy = zoom;
            const float ox = origin.x;
            const float oy = origin.y;
            const float vw = std::max(1.0f, size.x);
            const float vh = std::max(1.0f, size.y);
            heatmap_async_future = std::async(std::launch::async, [=]() mutable -> std::pair<uint64_t, HeatmapRenderData> {
                return buildHeatmapRenderData(
                    key_copy,
                    samples_copy,
                    ox,
                    oy,
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
            if (smooth_only_heatmap) should_recompute_heatmap = false;
        } else if (heatmap_async_inflight) {
            should_recompute_heatmap = false;
        }
        std::vector<CachedHeatCell> frame_heat_cells;
        if (should_recompute_heatmap) {
            frame_heat_cells = buildImmediateHeatmapCells(
                heat_samples,
                origin.x,
                origin.y,
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
        const std::vector<CachedHeatCell>& draw_cells =
            can_use_cached_heatmap ? heatmap_cached_cells :
            (should_recompute_heatmap ? frame_heat_cells : heatmap_cached_cells);
        const bool draw_stale_raster_while_rebuilding =
            smooth_only_heatmap &&
            heatmap_async_inflight &&
            heatmap_raster_texture_valid;
        const bool draw_current_raster =
            heatmap_raster_texture_valid &&
            heatmap_raster_cache_key == heatmap_key;
        const bool drawing_heatmap_raster =
            (draw_current_raster || draw_stale_raster_while_rebuilding) &&
            heatmap_raster_texture.descriptor;
        if (drawing_heatmap_raster) {
            ImVec2 raster_nw = lonLatToWorldPx(heatmap_cached_raster_meta.min_lon, heatmap_cached_raster_meta.max_lat, math_zoom);
            ImVec2 raster_se = lonLatToWorldPx(heatmap_cached_raster_meta.max_lon, heatmap_cached_raster_meta.min_lat, math_zoom);
            ImVec2 rp0 = project_world(raster_nw);
            ImVec2 rp1 = project_world(raster_se);
            draw->AddImage((ImTextureID)heatmap_raster_texture.descriptor, rp0, rp1);
        }
        const bool suppress_vector_heat_cells = smooth_only_heatmap && drawing_heatmap_raster;
        if (!suppress_vector_heat_cells) for (const auto& c : draw_cells) {
            if (c.is_hex) {
                ImVec2 pts[6] = {
                    ImVec2(c.cx - c.hw, c.cy),
                    ImVec2(c.cx - c.hw * 0.5f, c.cy - c.hh),
                    ImVec2(c.cx + c.hw * 0.5f, c.cy - c.hh),
                    ImVec2(c.cx + c.hw, c.cy),
                    ImVec2(c.cx + c.hw * 0.5f, c.cy + c.hh),
                    ImVec2(c.cx - c.hw * 0.5f, c.cy + c.hh),
                };
                draw->AddConvexPolyFilled(pts, 6, c.fill);
                if (c.draw_outline) draw->AddPolyline(pts, 6, c.outline, ImDrawFlags_Closed, 1.0f);
            } else {
                draw->AddRectFilled(ImVec2(c.x0, c.y0), ImVec2(c.x1, c.y1), c.fill);
                if (c.draw_outline) draw->AddRect(ImVec2(c.x0, c.y0), ImVec2(c.x1, c.y1), c.outline, 0.0f, 0, 1.0f);
            }
        }

        const auto overlay_prof_begin = std::chrono::steady_clock::now();
        // Render vacant overlays as a final top pass so they remain visible regardless of layer order.
        size_t visible_vacant_parcels_counter = 0;
        if ((vacant_notice_enabled || vacant_rehab_enabled) && parcel_layer_idx >= 0) {
            auto& parcel_layer = layers[(size_t)parcel_layer_idx];
            for (size_t i = 0; i < parcel_layer.features.size(); ++i) {
                auto& fg = parcel_layer.features[i];
                if (!feature_passes_filters((size_t)parcel_layer_idx, i, fg)) continue;
                const auto& ex = fg.extent;
                const auto& pww = get_world_extent((size_t)parcel_layer_idx, (uint32_t)i, fg);
                ImVec2 p0w = pww.first;
                ImVec2 p1w = pww.second;
                ImVec2 a = project_world(p0w);
                ImVec2 b = project_world(p1w);
                ImVec2 p0(std::min(a.x, b.x), std::min(a.y, b.y));
                ImVec2 p1(std::max(a.x, b.x), std::max(a.y, b.y));
                if (p1.x < origin.x || p0.x > origin.x + size.x || p1.y < origin.y || p0.y > origin.y + size.y) continue;
                if (fg.rings.empty()) continue;

                int vac_notice = (i < parcel_vac_notice_by_feature.size()) ? parcel_vac_notice_by_feature[i] : 0;
                int vac_rehab = (i < parcel_vac_rehab_by_feature.size()) ? parcel_vac_rehab_by_feature[i] : 0;

                int weight = 0;
                if (vacant_notice_enabled) weight += vac_notice;
                if (vacant_rehab_enabled) weight += vac_rehab;
                if (weight <= 0) continue;
                visible_vacant_parcels_counter++;

                const auto& world_rings = get_world_rings((size_t)parcel_layer_idx, (uint32_t)i, fg);
                const int alpha = std::clamp(120 + weight * 18, 120, 230);
                ImVec4 vac_base = vacancy_base_color(vac_notice, vac_rehab);
                ImU32 vac_fill = color_with_alpha(vac_base, alpha);
                ImU32 vac_outline = color_with_alpha(darken_color(vac_base, 0.62f), 235);
                const bool notice_fill = vacant_notice_layer_idx >= 0 &&
                    (size_t)vacant_notice_layer_idx < layer_fill_enabled.size() &&
                    layer_fill_enabled[(size_t)vacant_notice_layer_idx];
                const bool rehab_fill = vacant_rehab_layer_idx >= 0 &&
                    (size_t)vacant_rehab_layer_idx < layer_fill_enabled.size() &&
                    layer_fill_enabled[(size_t)vacant_rehab_layer_idx];
                if ((notice_fill || rehab_fill) && should_fill_layer_polygon((size_t)parcel_layer_idx)) {
                    draw_tessellated_fill(fg, world_rings, vac_fill);
                }
                for (const auto& r : world_rings) {
                    append_world_ring_line(r);
                    if (!scratch_line.empty()) draw->AddPolyline(scratch_line.data(), (int)scratch_line.size(), vac_outline, ImDrawFlags_Closed, 2.0f);
                }
            }
        }
        visible_vacant_parcels_last_frame.store(visible_vacant_parcels_counter, std::memory_order_relaxed);
        prof_overlay_ms_last.store(prof_ms_since(overlay_prof_begin), std::memory_order_relaxed);

        // Render tax source records on matched parcel geometry, not as raw point dots.
        if ((tax_lien_enabled || tax_sale_enabled) && parcel_layer_idx >= 0) {
            auto& parcel_layer = layers[(size_t)parcel_layer_idx];
            for (size_t i = 0; i < parcel_layer.features.size(); ++i) {
                auto& fg = parcel_layer.features[i];
                if (!feature_passes_filters((size_t)parcel_layer_idx, i, fg)) continue;
                const auto& pww = get_world_extent((size_t)parcel_layer_idx, (uint32_t)i, fg);
                ImVec2 a = project_world(pww.first);
                ImVec2 b = project_world(pww.second);
                ImVec2 p0(std::min(a.x, b.x), std::min(a.y, b.y));
                ImVec2 p1(std::max(a.x, b.x), std::max(a.y, b.y));
                if (p1.x < origin.x || p0.x > origin.x + size.x || p1.y < origin.y || p0.y > origin.y + size.y) continue;
                if (fg.rings.empty()) continue;

                const int lien_count = (i < parcel_tax_lien_by_feature.size()) ? parcel_tax_lien_by_feature[i] : 0;
                const int sale_count = (i < parcel_tax_sale_by_feature.size()) ? parcel_tax_sale_by_feature[i] : 0;
                int weight = 0;
                if (tax_lien_enabled) weight += lien_count;
                if (tax_sale_enabled) weight += sale_count;
                if (weight <= 0) continue;

                ImVec4 lien_c = (tax_lien_layer_idx >= 0) ? layers[(size_t)tax_lien_layer_idx].color : ImVec4(0.95f, 0.55f, 0.1f, 1.0f);
                ImVec4 sale_c = (tax_sale_layer_idx >= 0) ? layers[(size_t)tax_sale_layer_idx].color : ImVec4(0.85f, 0.2f, 0.1f, 1.0f);
                ImVec4 tax_base = lien_c;
                if (tax_lien_enabled && tax_sale_enabled && lien_count > 0 && sale_count > 0) {
                    tax_base = ImVec4((lien_c.x + sale_c.x) * 0.5f,
                                      (lien_c.y + sale_c.y) * 0.5f,
                                      (lien_c.z + sale_c.z) * 0.5f,
                                      1.0f);
                } else if (tax_sale_enabled && sale_count > 0) {
                    tax_base = sale_c;
                }
                const auto& world_rings = get_world_rings((size_t)parcel_layer_idx, (uint32_t)i, fg);
                const bool lien_fill = tax_lien_enabled &&
                    tax_lien_layer_idx >= 0 &&
                    (size_t)tax_lien_layer_idx < layer_fill_enabled.size() &&
                    layer_fill_enabled[(size_t)tax_lien_layer_idx];
                const bool sale_fill = tax_sale_enabled &&
                    tax_sale_layer_idx >= 0 &&
                    (size_t)tax_sale_layer_idx < layer_fill_enabled.size() &&
                    layer_fill_enabled[(size_t)tax_sale_layer_idx];
                if ((lien_fill || sale_fill) && should_fill_layer_polygon((size_t)parcel_layer_idx)) {
                    const int alpha = std::clamp(90 + weight * 10, 90, 210);
                    draw_tessellated_fill(fg, world_rings, color_with_alpha(tax_base, alpha));
                }
                ImU32 tax_outline = color_with_alpha(darken_color(tax_base, 0.58f), 240);
                for (const auto& r : world_rings) {
                    append_world_ring_line(r);
                    if (!scratch_line.empty()) draw->AddPolyline(scratch_line.data(), (int)scratch_line.size(), tax_outline, ImDrawFlags_Closed, 2.0f);
                }
            }
        }

        render_fill_attempts_last_frame.store(fill_attempts_frame, std::memory_order_relaxed);
        render_fill_success_last_frame.store(fill_success_frame, std::memory_order_relaxed);
        render_fill_no_triangles_last_frame.store(fill_no_triangles_frame, std::memory_order_relaxed);
        render_fill_bad_indices_last_frame.store(fill_bad_indices_frame, std::memory_order_relaxed);

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

            ImGui::BeginTooltip();
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
                ImGui::Text("Zoning: %s", zone_key.empty() ? "(available, unlabeled)" : zone_key.c_str());
                if (!zone_label.empty() && zone_label != zone_key) {
                    ImGui::TextWrapped("Zoning Label: %s", zone_label.c_str());
                }
                if (!zone_description.empty()) {
                    ImGui::TextWrapped("Description: %s", zone_description.c_str());
                }
                ImGui::TextUnformatted("Zoning Fields");
                for (const auto& kv : hovered_zoning->properties) {
                    if (kv.second.empty()) continue;
                    ImGui::TextWrapped("%s: %s", kv.first.c_str(), kv.second.c_str());
                }
            } else if (zoning_layer_idx >= 0) {
                ImGui::TextDisabled("Zoning: no intersecting zoning polygon found.");
            }
            ImGui::Separator();
            draw_feature_properties("All Parcel Geometry Fields", *hovered_parcel);
            if (hovered_rp) {
                ImGui::Separator();
                draw_feature_properties("All Real Property Fields", *hovered_rp);
            }
            ImGui::EndTooltip();
        }
        if (zoning_hover_active && map_hovered && !hovered_parcel && hovered_zone) {
            std::string zone_key = zoningClassKey(*hovered_zone);
            std::string zone_label = zoningClassLabel(*hovered_zone);
            std::string zone_description;
            auto meta_it = zoning_metadata.find(zone_key);
            if (meta_it != zoning_metadata.end()) {
                if (!meta_it->second.label.empty()) zone_label = meta_it->second.label;
                zone_description = meta_it->second.description;
            }
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Zoning Details");
            ImGui::Separator();
            ImGui::Text("Feature: %zu", hovered_zone_idx);
            ImGui::Text("Zone: %s", zone_key.empty() ? "(unlabeled)" : zone_key.c_str());
            if (!zone_label.empty() && zone_label != zone_key) {
                ImGui::TextWrapped("Label: %s", zone_label.c_str());
            }
            if (!zone_description.empty()) {
                ImGui::TextWrapped("Description: %s", zone_description.c_str());
            }
            ImGui::Separator();
            ImGui::TextUnformatted("All Zone Fields");
            for (const auto& kv : hovered_zone->properties) {
                if (kv.second.empty()) continue;
                ImGui::TextWrapped("%s: %s", kv.first.c_str(), kv.second.c_str());
            }
            ImGui::EndTooltip();
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

