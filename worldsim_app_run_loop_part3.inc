            const std::string deed_page = firstDisplayProperty(*rp, {"DEEDPAGE"});
            text_prop("Deed", deed_book.empty() ? "" : deed_book + (deed_page.empty() ? "" : " / " + deed_page));
            text_prop("SDAT Link", firstDisplayProperty(*rp, {"SDATLINK"}));
            ImGui::TextDisabled("Source: Local property records when available");
        };
        auto draw_feature_properties = [&](const char* title, const LayerDef::FeatureGeom& fg) {
            ImGui::TextUnformatted(title);
            for (const auto& kv : fg.properties) {
                std::string v = trimDisplayValue(kv.second);
                if (v.empty()) continue;
                ImGui::TextWrapped("%s: %s", kv.first.c_str(), v.c_str());
            }
        };
        auto parcel_area_sq_m = [&](const LayerDef::FeatureGeom& fg) -> double {
            if (fg.rings.empty()) return 0.0;
            const double deg_to_m_lat = 111320.0;
            double total = 0.0;
            for (const auto& ring : fg.rings) {
                if (ring.size() < 3) continue;
                double lat_sum = 0.0;
                for (const auto& p : ring) lat_sum += (double)p.y;
                const double lat0 = lat_sum / (double)ring.size();
                const double cos_lat = std::cos(lat0 * std::numbers::pi / 180.0);
                const double sx = deg_to_m_lat * cos_lat;
                const double sy = deg_to_m_lat;
                double a = 0.0;
                for (size_t i = 0, n = ring.size(); i < n; ++i) {
                    const auto& p = ring[i];
                    const auto& q = ring[(i + 1) % n];
                    const double px = (double)p.x * sx;
                    const double py = (double)p.y * sy;
                    const double qx = (double)q.x * sx;
                    const double qy = (double)q.y * sy;
                    a += (px * qy - qx * py);
                }
                total += std::abs(a) * 0.5;
            }
            return total;
        };
        auto owner_name_for = [&](const LayerDef::FeatureGeom* rp) -> std::string {
            if (!rp) return "";
            std::string o = firstDisplayProperty(*rp, {"OWNER_1", "OWNERNME1", "OWNER", "OWNER_NAME", "AR_OWNER", "OWNER_ABBR"});
            return toLowerAscii(trimDisplayValue(o));
        };
        auto owner_class_items = [&]() -> const std::vector<std::pair<std::string, std::string>>& {
            static const std::vector<std::pair<std::string, std::string>> kItems = {
                {"all", "All"},
                {"government", "Government"},
                {"individual", "Individual"},
                {"receivership", "Receivership"},
                {"development_corp", "Development Corp"},
                {"nonprofit", "Nonprofit"},
                {"trust_or_estate", "Trust/Estate"},
                {"financial_institution", "Financial Institution"},
                {"llc_corporate", "LLC/Corporate"},
                {"unknown", "Unknown"},
            };
            return kItems;
        };
        auto owner_class_label = [&](const std::string& key) -> std::string {
            for (const auto& kv : owner_class_items()) if (kv.first == key) return kv.second;
            return "Unknown";
        };
        auto contains_any = [&](const std::string& s, std::initializer_list<const char*> terms) -> bool {
            for (const char* t : terms) if (s.find(t) != std::string::npos) return true;
            return false;
        };
        auto classify_owner = [&](const std::string& owner) -> std::string {
            if (owner.empty()) return "unknown";
            const std::string& s = owner;
            if (contains_any(s, {"receiver", "receivership", "trustee", "court appointed", "special receiver"})) return "receivership";
            if (contains_any(s, {"city of ", "county", "state of ", "housing authority", "dept of", "department of", "u.s.", " us ", "usa"})) return "government";
            if (contains_any(s, {"development corp", "community development", "cdc ", " cdc", "redevelopment"})) return "development_corp";
            if (contains_any(s, {"church", "ministries", "foundation", "nonprofit", "charitable", "association"})) return "nonprofit";
            if (contains_any(s, {"trust", "estate"})) return "trust_or_estate";
            if (contains_any(s, {"bank", "credit union", "mortgage", "federal national", "fannie mae", "freddie mac", "loan"})) return "financial_institution";
            if (contains_any(s, {" llc", "llc ", "inc", "corp", "company", " co ", " ltd", "lp", "llp", "properties", "holdings"})) return "llc_corporate";
            int alpha_tokens = 0;
            bool has_non_alpha_token = false;
            std::string token;
            for (char ch : s) {
                if (std::isalpha((unsigned char)ch)) token.push_back(ch);
                else if (!token.empty()) {
                    alpha_tokens++;
                    token.clear();
                } else if (!std::isspace((unsigned char)ch)) {
                    has_non_alpha_token = true;
                }
            }
            if (!token.empty()) alpha_tokens++;
            if (!has_non_alpha_token && alpha_tokens >= 2 && alpha_tokens <= 4) return "individual";
            return "unknown";
        };
        if (!owner_class_overrides_loaded) {
            owner_class_overrides_loaded = true;
            owner_class_overrides.clear();
            std::ifstream in(root / "data" / "owner_class_overrides.json");
            if (in) {
                try {
                    json oj;
                    in >> oj;
                    if (oj.is_object()) {
                        for (auto it = oj.begin(); it != oj.end(); ++it) {
                            if (it.value().is_string()) owner_class_overrides[it.key()] = it.value().get<std::string>();
                        }
                    }
                } catch (...) {
                }
            }
        }
        if (owner_class_overrides_dirty) {
            owner_class_overrides_dirty = false;
            std::filesystem::create_directories(root / "data");
            std::ofstream out(root / "data" / "owner_class_overrides.json");
            if (out) {
                json oj = json::object();
                for (const auto& kv : owner_class_overrides) oj[kv.first] = kv.second;
                out << oj.dump(2);
            }
        }
        auto owner_value_for = [&](const LayerDef::FeatureGeom* rp) -> double {
            if (!rp) return 0.0;
            auto v = [&](std::initializer_list<const char*> keys) -> double {
                return parseNumericField(firstDisplayProperty(*rp, keys));
            };
            double tax_base = v({"TAXBASE", "ARTAXBAS"});
            double curr_land = v({"CURRLAND"});
            double curr_impr = v({"CURRIMPR"});
            double sale = v({"SALEPRIC"});
            if (tax_base > 0.0) return tax_base;
            if (curr_land + curr_impr > 0.0) return curr_land + curr_impr;
            return std::max(0.0, sale);
        };
        if (parcel_layer_idx >= 0 && (size_t)parcel_layer_idx < layers.size() &&
            layers[(size_t)parcel_layer_idx].features.size() != owner_cached_parcel_size) {
            owner_cached_parcel_size = layers[(size_t)parcel_layer_idx].features.size();
            owner_aggregates_dirty = true;
        }
        if (real_property_layer_idx >= 0 && (size_t)real_property_layer_idx < layers.size() &&
            layers[(size_t)real_property_layer_idx].features.size() != owner_cached_real_property_size) {
            owner_cached_real_property_size = layers[(size_t)real_property_layer_idx].features.size();
            owner_aggregates_dirty = true;
        }
        if (owner_aggregates_dirty && parcel_layer_idx >= 0 && (size_t)parcel_layer_idx < layers.size()) {
            const auto owner_prof_begin = std::chrono::steady_clock::now();
            std::unordered_map<std::string, OwnerAggregate> acc;
            const auto& parcels = layers[(size_t)parcel_layer_idx].features;
            const bool parcel_data_ready = !parcels.empty();
            const bool real_property_data_ready =
                real_property_layer_idx >= 0 &&
                (size_t)real_property_layer_idx < layers.size() &&
                !layers[(size_t)real_property_layer_idx].features.empty();
            if (!parcel_data_ready) {
                owner_sorted_mode = -1;
            } else {
            for (const auto& parcel_record : unified_parcels) {
                const LayerDef::FeatureGeom* pf = parcel_record.parcel_geom;
                if (!pf) continue;
                std::string owner = parcel_record.owner;
                if (owner.empty()) continue;
                auto& row = acc[owner];
                if (row.owner.empty()) row.owner = owner;
                if (row.owner_class.empty()) {
                    auto oit = owner_class_overrides.find(owner);
                    row.owner_class = oit != owner_class_overrides.end() ? oit->second : classify_owner(owner);
                }
                row.property_count += 1;
                row.area_m2 += parcel_area_sq_m(*pf);
                row.value_usd += parcel_record.current_value;
            }
            owner_aggregates.clear();
            owner_aggregates.reserve(acc.size());
            for (auto& kv : acc) owner_aggregates.push_back(std::move(kv.second));
            if (!selected_owners.empty()) {
                std::unordered_set<std::string> owners_present;
                owners_present.reserve(owner_aggregates.size());
                for (const auto& row : owner_aggregates) owners_present.insert(row.owner);
                for (auto it = selected_owners.begin(); it != selected_owners.end();) {
                    if (owners_present.find(*it) == owners_present.end()) it = selected_owners.erase(it);
                    else ++it;
                }
            }
            owner_aggregates_dirty = !real_property_data_ready && owner_aggregates.empty();
            owner_sorted_mode = -1;
            }
            prof_owner_ms_last.store(prof_ms_since(owner_prof_begin), std::memory_order_relaxed);
        }
        if (!selected_owners.empty()) {
            std::vector<std::string> selected_owner_list(selected_owners.begin(), selected_owners.end());
            std::sort(selected_owner_list.begin(), selected_owner_list.end());
            uint64_t selection_key = 1469598103934665603ULL;
            for (const auto& owner : selected_owner_list) {
                selection_key ^= (uint64_t)owner.size();
                selection_key *= 1099511628211ULL;
                for (unsigned char ch : owner) {
                    selection_key ^= (uint64_t)ch;
                    selection_key *= 1099511628211ULL;
                }
            }
            uint64_t data_key = 1469598103934665603ULL;
            data_key ^= (uint64_t)owner_aggregates.size(); data_key *= 1099511628211ULL;
            data_key ^= (uint64_t)owner_cached_parcel_size; data_key *= 1099511628211ULL;
            data_key ^= (uint64_t)owner_cached_real_property_size; data_key *= 1099511628211ULL;
            data_key ^= (uint64_t)(parcel_vacancy_generation_applied + 3); data_key *= 1099511628211ULL;
            data_key ^= (uint64_t)(parcel_tax_generation_applied + 7); data_key *= 1099511628211ULL;
            if (!filtered_aggregate_snapshot.valid ||
                filtered_aggregate_snapshot.selection_key != selection_key ||
                filtered_aggregate_snapshot.data_key != data_key) {
                filtered_aggregate_snapshot = {};
                filtered_aggregate_snapshot.valid = true;
                filtered_aggregate_snapshot.selection_key = selection_key;
                filtered_aggregate_snapshot.data_key = data_key;
                for (const auto& row : owner_aggregates) {
                    if (selected_owners.find(row.owner) == selected_owners.end()) continue;
                    filtered_aggregate_snapshot.owner_property_count += row.property_count;
                    filtered_aggregate_snapshot.owner_area_m2 += row.area_m2;
                    filtered_aggregate_snapshot.owner_value_usd += row.value_usd;
                }
                if (parcel_layer_idx >= 0 && (size_t)parcel_layer_idx < layers.size()) {
                    for (const auto& parcel_record : unified_parcels) {
                        std::string owner = parcel_record.owner;
                        if (owner.empty() || selected_owners.find(owner) == selected_owners.end()) continue;
                        const int vac_notice = parcel_record.vacant_notice_count;
                        const int vac_rehab = parcel_record.vacant_rehab_count;
                        if ((vac_notice + vac_rehab) <= 0) continue;
                        filtered_aggregate_snapshot.vacancy_parcels_matched++;
                        if (parcel_record.parcel_geom && !parcel_record.parcel_geom->rings.empty()) filtered_aggregate_snapshot.vacancy_parcels_with_geometry++;
                    }
                }
            }
        } else {
            filtered_aggregate_snapshot = {};
        }

        ImGui::SetNextWindowPos(ImVec2(layout_w - right_panel_w - layout_margin, layout_margin), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(right_panel_w, main_panel_h), ImGuiCond_Always);
        ImGui::Begin("Record Filters", nullptr, ImGuiWindowFlags_NoCollapse);
        auto clear_parcel_selection = [&]() {
            clearParcelSelection(parcel_selection);
        };
        auto select_parcel_idx = [&](size_t idx, bool append_toggle) -> bool {
            if (parcel_layer_idx < 0 || (size_t)parcel_layer_idx >= layers.size()) return false;
            const auto& parcel_layer = layers[(size_t)parcel_layer_idx];
            if (!selectParcel(parcel_selection, idx, parcel_layer.features.size(), append_toggle)) return false;
            openElementParcelPage(element_info_state, idx);
            show_selected_zone_details = false;
            selected_zone_idx = (size_t)-1;
            return true;
        };
        if (parcel_layer_idx >= 0 && (size_t)parcel_layer_idx < layers.size()) {
            pruneParcelSelection(parcel_selection, layers[(size_t)parcel_layer_idx].features.size());
        } else {
            clear_parcel_selection();
        }
        if (ImGui::BeginTabBar("right_tabs")) {
        drawFiltersTab(FiltersTabContext{
            &map_filter_state,
            &layers,
            &zoning_metadata,
            &selected_parcel_index_set,
            parcel_layer_idx,
            zoning_layer_idx,
            &show_selected_parcel_details,
            &show_selected_zone_details,
            &selected_zone_idx,
            &center_lon,
            &center_lat,
            &zoom,
            &address_locate_status,
            &address_locate_matches,
            &record_year_hist,
            &record_year_hist_plot,
            &hist_feature_counts,
            &hist_enabled,
            &hist_dirty,
            &record_year_hist_max_bin,
            &record_year_nonzero_min,
            &record_year_nonzero_max,
            &record_year_nonzero_total,
            &selected_record_year,
            &selected_record_year_dirty,
            &selected_record_year_total,
            &selected_record_year_samples,
            clear_parcel_selection,
            select_parcel_idx,
            real_property_for_parcel
        });
        drawSqlTab(
            duckdb_analytics,
            layers,
            unified_parcels,
            map_filter_state,
            selected_parcel_indices,
            show_selected_parcel_details,
            parcel_layer_idx,
            selected_parcel_idx,
            query_layers);
        drawVacancyParcelTab(VacancyParcelTabContext{
            (size_t)cached_vac_notice_size,
            (size_t)cached_vac_rehab_size,
            vacant_notice_rows_matched_total.load(std::memory_order_relaxed),
            vacant_rehab_rows_matched_total.load(std::memory_order_relaxed),
            vacant_parcels_matched_total.load(std::memory_order_relaxed),
            vacant_parcels_with_geometry_total.load(std::memory_order_relaxed),
            !selected_owners.empty(),
            &filtered_aggregate_snapshot
        });
        drawGradientTab(GradientTabContext{
            &layers,
            &layer_heatmap_enabled,
            &layer_heatmap_max_zoom,
            &layer_parcel_detail_min_zoom,
            &layer_heatmap_algo,
            &layer_heatmap_percentile_clip,
            heatmap_percentile_clip,
            heatmap_algo,
            zoom
        });
        drawElementInfoTab(OwnerInfoTabContext{
            &element_info_state,
            &layers,
            parcel_layer_idx,
            &unified_parcels,
            &selected_parcel_index_set,
            &selected_parcel_indices,
            show_selected_parcel_details,
            vacant_notice_layer_idx,
            vacant_rehab_layer_idx,
            tax_lien_layer_idx,
            tax_sale_layer_idx,
            &center_lon,
            &center_lat,
            &zoom,
            clear_parcel_selection,
            select_parcel_idx,
            real_property_for_parcel
        });
        drawOwnersTab(OwnersTabContext{
            &owner_aggregates,
            &selected_owners,
            &owner_class_overrides,
            &filtered_aggregate_snapshot,
            &element_info_state,
            &layers,
            parcel_layer_idx,
            real_property_layer_idx,
            &owner_sort_mode,
            &owner_sorted_mode,
            &owner_class_filter_mode,
            &owner_class_assign_mode,
            &selected_owner_anchor,
            &owner_class_overrides_dirty,
            &owner_aggregates_dirty,
            owner_search_query,
            sizeof(owner_search_query),
            &owner_class_items()
        });
        ImGui::EndTabBar();
        }
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(map_x, layout_margin), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(map_w, main_panel_h), ImGuiCond_Always);
        ImGui::Begin("Map", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        if (ImGui::BeginTabBar("main_view_tabs")) {
        if (ImGui::BeginTabItem("Map")) {
        const MapViewportFrame map_viewport = beginMapViewportCanvas(MapViewportContext{
            &center_lon,
            &center_lat,
            &zoom,
            kMinZoom,
            kMaxZoom,
            kMaxInternalMathZoom
        });
        ImVec2 origin = map_viewport.origin;
        ImVec2 size = map_viewport.size;
        ImDrawList* draw = map_viewport.draw;
        const bool map_hovered = map_viewport.hovered;
        const bool map_active = map_viewport.active;
        const int math_zoom = map_viewport.math_zoom;
        const double zoom_scale = map_viewport.zoom_scale;
        ImVec2 center_world = map_viewport.center_world;
        auto project_world = [&](const ImVec2& wp) -> ImVec2 { return map_viewport.projectWorld(wp); };
        const ImVec2 mouse_ll = map_viewport.mouse_ll;
        const double half_w_world = (size.x * 0.5) / zoom_scale;
        const double half_h_world = (size.y * 0.5) / zoom_scale;
        const float view_min_lon = map_viewport.view_min_lon;
        const float view_max_lon = map_viewport.view_max_lon;
        const float view_min_lat = map_viewport.view_min_lat;
        const float view_max_lat = map_viewport.view_max_lat;
        auto layer_hover_active = [&](int idx) -> bool {
            return idx >= 0 && (size_t)idx < layer_hover_enabled.size() && layer_hover_enabled[(size_t)idx];
        };
        auto layer_inspect_active = [&](int idx) -> bool {
            return idx >= 0 && (size_t)idx < layer_inspect_enabled.size() && layer_inspect_enabled[(size_t)idx];
        };
        const bool hover_parcels_enabled = (hover_inspector_mode == 1 || hover_inspector_mode == 3);
        const bool hover_zoning_enabled = (hover_inspector_mode == 2 || hover_inspector_mode == 3);
        hover_inspector_enabled = (hover_inspector_mode != 0);
        const bool parcel_hover_active = hover_parcels_enabled && layer_hover_active(parcel_layer_idx);
        const bool parcel_inspect_active = layer_inspect_active(parcel_layer_idx);
        const bool zoning_hover_active = hover_zoning_enabled && layer_hover_active(zoning_layer_idx);
        const bool zoning_inspect_active = layer_inspect_active(zoning_layer_idx);

        MapHoverQuery hover_query;
        hover_query.map_hovered = map_hovered;
        hover_query.parcel_hover_active = parcel_hover_active;
        hover_query.parcel_inspect_active = parcel_inspect_active;
        hover_query.zoning_hover_active = zoning_hover_active;
        hover_query.zoning_inspect_active = zoning_inspect_active;
        hover_query.parcel_layer_idx = parcel_layer_idx;
        hover_query.zoning_layer_idx = zoning_layer_idx;
        hover_query.layers = &layers;
        hover_query.layer_spatial = &layer_spatial;
        hover_query.mouse_ll = mouse_ll;
        hover_query.view_min_lon = view_min_lon;
        hover_query.view_max_lon = view_max_lon;
        hover_query.view_min_lat = view_min_lat;
        hover_query.view_max_lat = view_max_lat;
        const MapHoverState hover_state = findMapHoverTargets(hover_query);
        const LayerDef::FeatureGeom* hovered_parcel = hover_state.hovered_parcel;
        size_t hovered_parcel_idx = hover_state.hovered_parcel_idx;
        const LayerDef::FeatureGeom* hovered_zone = hover_state.hovered_zone;
        size_t hovered_zone_idx = hover_state.hovered_zone_idx;
        const auto tile_prof_begin = std::chrono::steady_clock::now();
        const MapBasemapRenderResult basemap_result = renderMapBasemap(MapBasemapRenderContext{
            draw,
            origin,
            size,
            center_world,
            math_zoom,
            zoom_scale,
            &root,
            &app_settings,
            &lazy_tile_download,
            &topo_tiles_available_cached,
            &topo_vector_available_cached,
            &basemap_source_has_any_files_cached,
            &tile_root_dir_cached,
            &basemap_availability_last_check,
            kMaxNativeTileZoom,
            project_world
        });
        prof_tiles_drawn_frame += basemap_result.tiles_drawn;
        prof_tile_ms_last.store(prof_ms_since(tile_prof_begin), std::memory_order_relaxed);

        bool vacant_notice_enabled = vacant_notice_layer_idx >= 0 && layers[(size_t)vacant_notice_layer_idx].enabled;
        bool vacant_rehab_enabled = vacant_rehab_layer_idx >= 0 && layers[(size_t)vacant_rehab_layer_idx].enabled;
        bool tax_lien_enabled = tax_lien_layer_idx >= 0 && layers[(size_t)tax_lien_layer_idx].enabled;
        bool tax_sale_enabled = tax_sale_layer_idx >= 0 && layers[(size_t)tax_sale_layer_idx].enabled;
        const ImVec4 vacancy_notice_color = (vacant_notice_layer_idx >= 0) ? layers[(size_t)vacant_notice_layer_idx].color : ImVec4(1, 0, 0, 1);
        const ImVec4 vacancy_rehab_color = (vacant_rehab_layer_idx >= 0) ? layers[(size_t)vacant_rehab_layer_idx].color : ImVec4(0, 1, 1, 1);
        const int lod_ring_step = lodRingStepForZoom(math_zoom);
        const bool allow_parcel_scale_fill = math_zoom >= 14;
        auto is_parcel_scale_layer = [&](size_t layer_idx) -> bool {
            return layer_idx < layers.size() && layers[layer_idx].scale == "parcel";
        };
        auto should_fill_layer_polygon = [&](size_t layer_idx) -> bool {
            // Large-area layers still need low-zoom fill; parcel-scale fills are the expensive case.
            return !is_parcel_scale_layer(layer_idx) || allow_parcel_scale_fill;
        };
        MapProjectionCache projection_cache(math_zoom, 1, project_world);
        projection_cache.reserveWorldRings(8192);
        auto get_world_rings = [&](size_t layer_idx, uint32_t feature_idx, const LayerDef::FeatureGeom& fg)
            -> const std::vector<std::vector<ImVec2>>& {
            return projection_cache.getWorldRings(layer_idx, feature_idx, fg);
        };
        auto get_world_extent = [&](size_t layer_idx, uint32_t feature_idx, const LayerDef::FeatureGeom& fg)
            -> const std::pair<ImVec2, ImVec2>& {
            return projection_cache.getWorldExtent(layer_idx, feature_idx, fg);
        };
        auto append_world_ring_line = [&](const std::vector<ImVec2>& world_ring, int ring_step = 1) {
            projection_cache.appendWorldRingLine(world_ring, ring_step);
        };
        auto draw_tessellated_fill = [&](const LayerDef::FeatureGeom& fg,
                                         const std::vector<std::vector<ImVec2>>& world_rings,
                                         ImU32 fill_color) -> bool {
            return projection_cache.drawTessellatedFill(draw, fg, world_rings, fill_color);
        };
        auto first_prop = [&](const LayerDef::FeatureGeom& fg, std::initializer_list<const char*> keys) {
            for (const char* k : keys) {
                std::string v = getPropertyValue(fg, k);
                if (!v.empty()) return v;
            }
            return std::string();
        };
        auto is_crime_layer = [&](size_t layer_idx) -> bool {
            return (crime_nibrs_layer_idx >= 0 && (int)layer_idx == crime_nibrs_layer_idx) ||
                   (crime_legacy_layer_idx >= 0 && (int)layer_idx == crime_legacy_layer_idx);
        };
        auto crime_feature_matches = [&](const LayerDef::FeatureGeom& fg) -> bool {
            if (!crime_filter_enabled) return true;
            const std::string desc = toLowerAscii(first_prop(fg, {"Description", "description", "OFFENSE", "UCRDescription"}));
            const std::string code = toLowerAscii(first_prop(fg, {"CrimeCode", "UCR_CODE", "UCRCode"}));
            const std::string dt = first_prop(fg, {"CrimeDateTime", "CrimeDate", "DATE", "RECORD_DATE"});
            if (crime_filter_use_year) {
                int yr = extractYearMaybe(dt);
                if (yr < 0 || yr < crime_year_min || yr > crime_year_max) return false;
            }
            const bool any_type =
