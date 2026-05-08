                    {
                        std::lock_guard<std::mutex> lk(data_library_bulk_mutex);
                        progress = data_library_bulk_progress;
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", progress.empty() ? "Downloading..." : progress.c_str());
                }
                size_t downloaded_count = 0;
                for (size_t i = 0; i < local_layer_exists_cache.size(); ++i) {
                    if (local_layer_exists_cache[i]) downloaded_count++;
                }
                ImGui::Text("Downloaded: %zu / %zu", downloaded_count, layers.size());
                ImGui::TextDisabled("Version metadata: data/versions/metadata | snapshots: data/versions/snapshots | diffs: data/versions/diffs");
                if (!data_library_status_msg.empty()) ImGui::TextWrapped("%s", data_library_status_msg.c_str());
                ImGui::Separator();

                const std::string query = trimDisplayValue(data_library_query);
                if (query != data_library_cached_query || data_library_cached_layer_count != layers.size()) {
                    data_library_cached_query = query;
                    data_library_cached_layer_count = layers.size();
                    data_library_visible_rows.clear();
                    data_library_visible_rows.reserve(layers.size());
                    for (size_t i = 0; i < layers.size(); ++i) {
                        const auto& l = layers[i];
                        const bool hit =
                            query.empty() ||
                            containsCaseInsensitive(l.name, query) ||
                            containsCaseInsensitive(l.file, query) ||
                            containsCaseInsensitive(categoryToString(l.category), query) ||
                            containsCaseInsensitive(l.subcategory, query) ||
                            containsCaseInsensitive(l.description, query);
                        if (hit) data_library_visible_rows.push_back(i);
                    }
                    data_library_cache_rebuilds++;
                }

                data_library_rendered_rows_last = 0;
                if (ImGui::BeginTable("data_library_table", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0, 0))) {
                    ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 94.0f);
                    ImGui::TableSetupColumn("Freshness", ImGuiTableColumnFlags_WidthFixed, 142.0f);
                    ImGui::TableSetupColumn("Enabled", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                    ImGui::TableSetupColumn("Name");
                    ImGui::TableSetupColumn("File");
                    ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 110.0f);
                    ImGui::TableSetupColumn("Subcategory");
                    ImGui::TableHeadersRow();
                    ImGuiListClipper clipper;
                    clipper.Begin(static_cast<int>(data_library_visible_rows.size()));
                    while (clipper.Step()) {
                        for (int display_index = clipper.DisplayStart; display_index < clipper.DisplayEnd; ++display_index) {
                            const size_t i = data_library_visible_rows[static_cast<size_t>(display_index)];
                            auto& l = layers[i];
                            const fs::path local_path = root / "data" / "layers" / l.file;
                            const bool local_exists = i < local_layer_exists_cache.size() ? local_layer_exists_cache[i] : false;
                            data_library_rendered_rows_last++;
                            ImGui::PushID((int)i);
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            if (!local_exists) {
                                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.22f, 0.12f, 1.0f));
                                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.30f, 0.14f, 1.0f));
                                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.46f, 0.18f, 0.08f, 1.0f));
                                if (ImGui::SmallButton("Download")) {
                                    if (l.source_url.empty()) {
                                        data_library_status_msg = "No source URL for " + l.file;
                                    } else {
                                        enqueue_layer_download_request(i);
                                    }
                                }
                                ImGui::PopStyleColor(3);
                            } else if (!l.source_url.empty()) {
                                if (data_freshness_state[i] == FreshnessState::UpdateAvailable) {
                                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.74f, 0.44f, 0.12f, 1.0f));
                                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.88f, 0.53f, 0.14f, 1.0f));
                                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.62f, 0.36f, 0.10f, 1.0f));
                                    if (ImGui::SmallButton("Update")) {
                                        enqueue_layer_download_request(i);
                                    }
                                    ImGui::PopStyleColor(3);
                                } else if (ImGui::SmallButton("Check")) {
                                    FreshnessCheckResult cr = checkUrlFreshnessVersioned(l.source_url, local_path, root / "data" / "versions");
                                    data_freshness_state[i] = cr.state;
                                    data_freshness_msg[i] = cr.message;
                                    data_library_status_msg = "Checked " + l.file + ": " + cr.message;
                                }
                            } else {
                                ImGui::TextDisabled("-");
                            }
                            ImGui::TableSetColumnIndex(1);
                            ImGui::ColorButton("##freshness_dot", freshness_color(data_freshness_state[i]), ImGuiColorEditFlags_NoTooltip, ImVec2(10, 10));
                            ImGui::SameLine();
                            ImGui::TextDisabled("%s", freshness_label(data_freshness_state[i]));
                            if (!data_freshness_msg[i].empty() && ImGui::IsItemHovered()) {
                                ImGui::BeginTooltip();
                                ImGui::TextUnformatted(data_freshness_msg[i].c_str());
                                ImGui::EndTooltip();
                            }
                            ImGui::TableSetColumnIndex(2);
                            ImGui::Checkbox("##enable", &l.enabled);
                            ImGui::TableSetColumnIndex(3);
                            ImGui::PushStyleColor(ImGuiCol_Text, l.color);
                            draw_library_cell(l.name, 72);
                            ImGui::PopStyleColor();
                            draw_library_tooltip(l);
                            ImGui::TableSetColumnIndex(4);
                            draw_library_cell(l.file, 72);
                            ImGui::TableSetColumnIndex(5);
                            draw_library_cell(categoryToString(l.category), 32);
                            ImGui::TableSetColumnIndex(6);
                            draw_library_cell(l.subcategory, 56);
                            ImGui::PopID();
                        }
                    }
                    ImGui::EndTable();
                }
                ImGui::TextDisabled("Matched rows: %zu | Rendered this frame: %zu | Cache rebuilds: %zu",
                                    data_library_visible_rows.size(), data_library_rendered_rows_last, data_library_cache_rebuilds);
            }
            ImGui::End();
        }
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(layout_margin, layout_margin + main_panel_h + layout_gap), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(left_panel_w, std::max(0.0f, layout_h - main_panel_h - layout_margin * 2.0f - layout_gap)), ImGuiCond_Always);
        ImGui::Begin("Performance and Stats", nullptr, ImGuiWindowFlags_NoCollapse);
        ImGui::Text("Hydration: %zu / %zu (%.1f%%)", hydrated_now, layers.size(), hydrated_frac * 100.0f);
        ImGui::ProgressBar(hydrated_frac, ImVec2(-1.0f, 0.0f));
        ImGui::Text("Triangulation: %zu / %zu (%.1f%%)", triangulated_now, layers.size(), tri_frac * 100.0f);
        ImGui::ProgressBar(tri_frac, ImVec2(-1.0f, 0.0f));
        ImGui::TextDisabled("Hydration queue: %zu | Tri queue: %zu | elapsed: %.1fs", hydrated_pending, tri_pending, elapsed_s);
        ImGui::TextDisabled("Frame: %.2f ms (last %.2f) | FPS: %.1f",
                            perf_frame_ms_avg.load(std::memory_order_relaxed),
                            perf_frame_ms_last.load(std::memory_order_relaxed),
                            perf_fps_avg.load(std::memory_order_relaxed));
        ImGui::TextDisabled("UI rows: Data Library %zu/%zu (rebuilds %zu) | People & Pay %zu/%zu (rebuilds %zu)",
                            data_library_rendered_rows_last,
                            data_library_visible_rows.size(),
                            data_library_cache_rebuilds,
                            people_pay_rendered_rows_last,
                            people_pay_visible_rows.size(),
                            people_pay_cache_rebuilds);
        ImGui::TextDisabled("Fill: %zu ok / %zu attempts | no tris %zu | bad idx %zu",
                            render_fill_success_last_frame.load(std::memory_order_relaxed),
                            render_fill_attempts_last_frame.load(std::memory_order_relaxed),
                            render_fill_no_triangles_last_frame.load(std::memory_order_relaxed),
                            render_fill_bad_indices_last_frame.load(std::memory_order_relaxed));
        ImGui::TextDisabled("API: http://127.0.0.1:8787/status");
        ImGui::TextDisabled("LAN Data API: http://0.0.0.0:8788/datasets");
        ImGui::TextDisabled("P2P signaling: /p2p/register /p2p/publish /p2p/poll");
        ImGui::SeparatorText("Arkavo Native Realtime");
        ImGui::InputText("Room ID", arkavo_room_id, sizeof(arkavo_room_id));
        if (!arkavo_client) {
            if (ImGui::Button("Connect Arkavo")) {
                ArkavoRealtimeClient::Config cfg;
                cfg.room_id = trimDisplayValue(arkavo_room_id);
                cfg.signaling_url = "wss://signaling.arkavo.org/";
                auto transport = std::make_unique<ArkavoSignalingTransportCurl>();
                arkavo_client = std::make_unique<ArkavoRealtimeClient>(cfg, std::move(transport));
                arkavo_rtc = std::make_unique<ArkavoRtcSessionManager>(*arkavo_client);
                arkavo_rtc->on_log = [&](const std::string& m) { arkavo_status = m; };
                arkavo_rtc->on_error = [&](const std::string& e) { arkavo_err = e; };
                arkavo_rtc->on_file_received = [&](const std::string& peer, const std::filesystem::path& p) {
                    arkavo_status = "received file from " + peer + ": " + p.string();
                };
                arkavo_client->on_log = [&](const std::string& m) { arkavo_status = m; };
                arkavo_client->on_error = [&](const std::string& e) { arkavo_err = e; };
                arkavo_client->on_peer_should_connect = [&](const std::string& peer_id, bool initiator) {
                    if (arkavo_rtc) arkavo_rtc->connectPeer(peer_id, initiator);
                };
                arkavo_client->on_peer_left = [&](const std::string& peer_id) {
                    if (arkavo_rtc) arkavo_rtc->removePeer(peer_id);
                };
                arkavo_client->on_signal_payload = [&](const std::string& peer_id, const nlohmann::json& payload) {
                    if (arkavo_rtc) arkavo_rtc->handleSignal(peer_id, payload);
                };
                std::string err;
                if (!arkavo_client->start(err)) {
                    arkavo_err = err;
                    arkavo_status = "connect failed";
                } else {
                    arkavo_status = "connecting";
                }
            }
        } else {
            if (ImGui::Button("Disconnect Arkavo")) {
                if (arkavo_rtc) arkavo_rtc->closeAll();
                arkavo_client->stop();
                arkavo_rtc.reset();
                arkavo_client.reset();
                arkavo_status = "disconnected";
            }
            ImGui::SameLine();
            ImGui::TextDisabled("connected=%s", arkavo_client->isConnected() ? "yes" : "no");
            ImGui::TextDisabled("self peer: %s", arkavo_client->selfPeerId().empty() ? "(none)" : arkavo_client->selfPeerId().c_str());
            ImGui::TextDisabled("tracked peers: %zu", arkavo_client->peers().size());
            if (arkavo_rtc) {
                auto open_peers = arkavo_rtc->connectedPeers();
                ImGui::TextDisabled("open data channels: %zu", open_peers.size());
                ImGui::InputText("Send Peer", arkavo_send_peer, sizeof(arkavo_send_peer));
                ImGui::InputText("Send File Path", arkavo_send_path, sizeof(arkavo_send_path));
                if (ImGui::Button("Send Arkavo File")) {
                    std::string err;
                    if (!arkavo_rtc->sendFile(trimDisplayValue(arkavo_send_peer), trimDisplayValue(arkavo_send_path), err)) {
                        arkavo_err = err;
                    } else {
                        arkavo_status = "file send queued";
                    }
                }
                if (!open_peers.empty()) {
                    ImGui::TextDisabled("open peers:");
                    for (const auto& p : open_peers) ImGui::TextDisabled("%s", p.c_str());
                }
            }
        }
        ImGui::TextDisabled("status: %s", arkavo_status.c_str());
        if (!arkavo_err.empty()) ImGui::TextColored(ImVec4(0.85f, 0.35f, 0.2f, 1.0f), "arkavo: %s", arkavo_err.c_str());
        if (ImGui::Button("Scan LAN Peers")) (void)scan_lan_peers(700, true);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", lan_scan_status.c_str());
        if (!lan_peers.empty()) {
            ImGui::BeginChild("lan_peer_list", ImVec2(0, 90), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
            for (const auto& p : lan_peers) {
                ImGui::Text("%s:%d | program %s %s | protocol %d %s",
                    p.ip.c_str(),
                    p.dataset_port,
                    p.app_name.empty() ? "(unknown)" : p.app_name.c_str(),
                    p.app_version.empty() ? "(unknown)" : p.app_version.c_str(),
                    p.protocol_version,
                    p.protocol_match ? "[OK]" : "[MISMATCH]");
            }
            ImGui::EndChild();
            std::unordered_map<std::string, int> program_counts;
            for (const auto& p : lan_peers) {
                const std::string name = p.app_name.empty() ? "(unknown)" : p.app_name;
                const std::string ver = p.app_version.empty() ? "(unknown)" : p.app_version;
                program_counts[name + " " + ver]++;
            }
            ImGui::TextDisabled("Peer Programs");
            ImGui::BeginChild("lan_program_list", ImVec2(0, 70), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
            for (const auto& kv : program_counts) {
                ImGui::Text("%s | peers %d", kv.first.c_str(), kv.second);
            }
            ImGui::EndChild();
        }
        if (hydrated_now < layers.size() && hydrate_idle_s > 15.0) {
            ImGui::TextColored(ImVec4(0.85f, 0.35f, 0.2f, 1.0f), "Hydration has not advanced for %.1fs", hydrate_idle_s);
        }
        if (triangulated_now < layers.size() && tri_idle_s > 15.0) {
            ImGui::TextColored(ImVec4(0.85f, 0.35f, 0.2f, 1.0f), "Triangulation has not advanced for %.1fs", tri_idle_s);
        }
        ImGui::Separator();
        ImGui::TextDisabled("Tile cache: %zu / %zu", g_TileCache.size(), kMaxTileCache);
        if (ImGui::Button("Clear Cache")) {
            size_t removed_files = 0;
            std::error_code ec;
            auto clear_tree = [&](const fs::path& p) {
                if (fs::exists(p, ec)) removed_files += fs::remove_all(p, ec);
            };
            clear_tree(root / "data" / "cache" / "hydration");
            clear_tree(root / "data" / "cache" / "triangulation");
            clear_tree(root / "data" / "cache" / "derived");
            fs::create_directories(root / "data" / "cache" / "hydration", ec);
            fs::create_directories(root / "data" / "cache" / "triangulation", ec);
            fs::create_directories(root / "data" / "cache" / "derived", ec);

            for (auto& kv : g_TileCache) destroyTileTexture(kv.second.tex);
            g_TileCache.clear();
            g_TileLRU.clear();
            clearTileDiskPresenceCache();

            {
                std::lock_guard<std::mutex> lk(hydrated_mutex);
                hydrated_queue.clear();
            }
            {
                std::lock_guard<std::mutex> lk(tri_mutex);
                tri_jobs.clear();
                tri_results.clear();
            }
            {
                std::lock_guard<std::mutex> lk(hydrate_req_mutex);
                hydrate_requests.clear();
                std::fill(hydration_requested.begin(), hydration_requested.end(), false);
                std::fill(hydration_required.begin(), hydration_required.end(), false);
            }

            hydrated_count.store(0, std::memory_order_relaxed);
            triangulated_count.store(0, std::memory_order_relaxed);
            cached_real_property_size = 0;
            cached_vac_notice_size = 0;
            cached_vac_rehab_size = 0;
            cached_tax_lien_size = 0;
            cached_tax_sale_size = 0;
            vacancy_maps_generation = 0;
            parcel_vacancy_generation_applied = -1;
            tax_maps_generation = 0;
            parcel_tax_generation_applied = -1;
            releaseContainerStorage(parcel_vac_notice_by_feature);
            releaseContainerStorage(parcel_vac_rehab_by_feature);
            releaseContainerStorage(parcel_tax_lien_by_feature);
            releaseContainerStorage(parcel_tax_sale_by_feature);
            releaseContainerStorage(parcel_tax_lien_amount_by_feature);
            releaseContainerStorage(parcel_tax_sale_amount_by_feature);
            releaseContainerStorage(vacant_notice_count_by_blocklot);
            releaseContainerStorage(vacant_rehab_count_by_blocklot);
            releaseContainerStorage(tax_lien_count_by_blocklot);
            releaseContainerStorage(tax_lien_amount_by_blocklot);
            releaseContainerStorage(tax_sale_count_by_blocklot);
            releaseContainerStorage(tax_sale_amount_by_blocklot);
            releaseContainerStorage(real_property_by_blocklot);
            releaseContainerStorage(zoning_zone_label);
            releaseContainerStorage(zoning_zone_counts);
            releaseContainerStorage(zoning_zone_order);
            releaseContainerStorage(zoning_group_zones);
            releaseContainerStorage(zoning_group_order);
            zoning_zone_discovered_feature_count = 0;
            visible_vacant_parcels_last_frame.store(0, std::memory_order_relaxed);
            vacant_parcels_matched_total.store(0, std::memory_order_relaxed);
            vacant_parcels_with_geometry_total.store(0, std::memory_order_relaxed);
            vacant_parcels_triangulated_renderable_total.store(0, std::memory_order_relaxed);

            for (size_t i = 0; i < layers.size(); ++i) {
                releaseContainerStorage(layers[i].features);
                layer_spatial[i] = LayerSpatialIndex{};
                if (i < layer_profile_dirty.size()) layer_profile_dirty[i] = true;
                std::lock_guard<std::mutex> lk(status_mutex);
                if (i < layer_states.size()) {
                    layer_states[i].status = LayerPipelineStatus::Queued;
                    layer_states[i].feature_count = 0;
                    layer_states[i].error.clear();
                }
            }

            for (size_t i = 0; i < layers.size(); ++i) {
                if (layers[i].enabled) enqueue_hydration(i);
            }
            trimProcessHeap();
            last_cache_clear_msg = "Cache cleared; rehydrating enabled layers.";
        }
        if (!last_cache_clear_msg.empty()) ImGui::TextDisabled("%s", last_cache_clear_msg.c_str());
        ImGui::End();
        bool ui_state_changed =
            (hover_inspector_mode != last_hover_inspector_mode) ||
            zoning_filters_changed ||
            layer_fill_state_changed ||
            layer_hover_state_changed ||
            layer_inspect_state_changed ||
            layer_heatmap_state_changed ||
            heatmap_settings_state_changed;
        std::vector<size_t> newly_enabled;
        if (last_enabled_state.size() == layers.size()) {
            for (size_t i = 0; i < layers.size(); ++i) {
                if (layers[i].enabled != last_enabled_state[i]) {
                    ui_state_changed = true;
                    if (i < layer_profile_dirty.size()) layer_profile_dirty[i] = true;
                    if (layers[i].enabled && !last_enabled_state[i]) newly_enabled.push_back(i);
                }
            }
        } else {
            ui_state_changed = true;
            for (size_t i = 0; i < layers.size(); ++i) {
                if (layers[i].enabled) newly_enabled.push_back(i);
            }
        }
        if (!newly_enabled.empty()) {
            for (size_t i : newly_enabled) {
                if (!layer_data_available(i)) enqueue_hydration(i);
            }
        }
        // Vacant overlays depend on parcel geometry even when parcel visibility is off.
        const bool vacant_layer_active =
            (vacant_notice_layer_idx >= 0 && layers[(size_t)vacant_notice_layer_idx].enabled) ||
            (vacant_rehab_layer_idx >= 0 && layers[(size_t)vacant_rehab_layer_idx].enabled);
        if (vacant_layer_active && parcel_layer_idx >= 0) {
            bool parcel_ready = false;
            {
                std::lock_guard<std::mutex> lk(status_mutex);
                if ((size_t)parcel_layer_idx < layer_states.size()) {
                    LayerPipelineStatus st = layer_states[(size_t)parcel_layer_idx].status;
                    parcel_ready = (st == LayerPipelineStatus::Hydrated ||
                                    st == LayerPipelineStatus::TriQueued ||
                                    st == LayerPipelineStatus::Triangulating ||
                                    st == LayerPipelineStatus::Ready);
                }
            }
            if (!parcel_ready) {
                enqueue_hydration((size_t)parcel_layer_idx, true);
            }
        }
        if (real_property_layer_idx >= 0 && (size_t)real_property_layer_idx < layers.size()) {
            bool real_property_ready = false;
            {
                std::lock_guard<std::mutex> lk(status_mutex);
                if ((size_t)real_property_layer_idx < layer_states.size()) {
                    LayerPipelineStatus st = layer_states[(size_t)real_property_layer_idx].status;
                    real_property_ready = (st == LayerPipelineStatus::Hydrated ||
                                           st == LayerPipelineStatus::TriQueued ||
                                           st == LayerPipelineStatus::Triangulating ||
                                           st == LayerPipelineStatus::Ready);
                }
            }
            const bool filter_join_needed =
                filter_enabled ||
                (parcel_layer_idx >= 0 && layers[(size_t)parcel_layer_idx].enabled) ||
                vacant_layer_active ||
                filter_owner[0] != '\0' ||
                filter_address[0] != '\0' ||
                filter_zip[0] != '\0';
            if (filter_join_needed && !real_property_ready) {
                enqueue_hydration((size_t)real_property_layer_idx, true);
            }
        }
        const bool tax_layer_active =
            (tax_lien_layer_idx >= 0 && layers[(size_t)tax_lien_layer_idx].enabled) ||
            (tax_sale_layer_idx >= 0 && layers[(size_t)tax_sale_layer_idx].enabled);
        if (tax_layer_active && parcel_layer_idx >= 0) {
            bool parcel_ready = false;
            {
                std::lock_guard<std::mutex> lk(status_mutex);
                if ((size_t)parcel_layer_idx < layer_states.size()) {
                    LayerPipelineStatus st = layer_states[(size_t)parcel_layer_idx].status;
                    parcel_ready = (st == LayerPipelineStatus::Hydrated ||
                                    st == LayerPipelineStatus::TriQueued ||
                                    st == LayerPipelineStatus::Triangulating ||
                                    st == LayerPipelineStatus::Ready);
                }
            }
            if (!parcel_ready) enqueue_hydration((size_t)parcel_layer_idx, true);
        }
        if (zoning_layer_idx >= 0 && (size_t)zoning_layer_idx < layers.size()) {
            bool zoning_ready = false;
            {
                std::lock_guard<std::mutex> lk(status_mutex);
                if ((size_t)zoning_layer_idx < layer_states.size()) {
                    LayerPipelineStatus st = layer_states[(size_t)zoning_layer_idx].status;
                    zoning_ready = (st == LayerPipelineStatus::Hydrated ||
                                    st == LayerPipelineStatus::TriQueued ||
                                    st == LayerPipelineStatus::Triangulating ||
                                    st == LayerPipelineStatus::Ready);
                }
            }
            if (layers[(size_t)zoning_layer_idx].enabled && !zoning_ready) {
                enqueue_hydration((size_t)zoning_layer_idx);
            }
        }
        if (ui_state_changed) {
            saveLayerUiState(
                root,
                layers,
                hover_inspector_enabled,
                &hover_inspector_mode,
                &zoning_zone_enabled,
                &layer_fill_enabled,
                &layer_hover_enabled,
                &layer_inspect_enabled,
                &layer_heatmap_enabled,
                &layer_heatmap_max_zoom,
                &layer_parcel_detail_min_zoom,
                &layer_heatmap_use_gradient,
                &layer_heatmap_algo,
                &layer_normalize_mode,
                &layer_heatmap_cell_px,
                &layer_heatmap_bandwidth_px,
                &layer_heatmap_blur_sigma_px,
                &layer_heatmap_percentile_clip,
                &layer_heatmap_zoom_adaptive_bandwidth,
                &layer_heatmap_multires_enabled,
                &layer_heatmap_multires_blend,
                &heatmap_algo,
                &heatmap_quality_preset,
                &global_heat_cell_px,
                &heatmap_bandwidth_px,
                &heatmap_blur_sigma_px,
                &heatmap_percentile_clip,
                &heatmap_zoom_adaptive_bandwidth,
                &heatmap_multires_enabled,
                &heatmap_multires_blend,
                &heatmap_allow_cpu_fallback);
            last_enabled_state.clear();
            last_enabled_state.reserve(layers.size());
            for (const auto& l : layers) last_enabled_state.push_back(l.enabled);
            last_hover_inspector_mode = hover_inspector_mode;
        }
        {
            std::lock_guard<std::mutex> lk(hydrated_mutex);
            while (!hydrated_queue.empty()) {
                HydratedLayer ready = std::move(hydrated_queue.front());
                hydrated_queue.pop_front();
                if (ready.index < layers.size()) {
                    if (!ready.features.empty()) {
                        auto& dst = layers[ready.index].features;
                        dst.insert(
                            dst.end(),
                            std::make_move_iterator(ready.features.begin()),
                            std::make_move_iterator(ready.features.end()));
                        if (ready.index < layer_profile_dirty.size()) layer_profile_dirty[ready.index] = true;
                        std::lock_guard<std::mutex> lk3(status_mutex);
                        if (ready.index < layer_states.size()) {
                            layer_states[ready.index].status = LayerPipelineStatus::Hydrating;
                            layer_states[ready.index].feature_count = layers[ready.index].features.size();
                        }
                    }
                    if (ready.failed) {
                        {
                            std::lock_guard<std::mutex> lk4(hydrate_req_mutex);
                            if (ready.index < hydration_requested.size()) hydration_requested[ready.index] = false;
                        }
                        std::lock_guard<std::mutex> lk3(status_mutex);
                        if (ready.index < layer_states.size()) {
                            layer_states[ready.index].status = LayerPipelineStatus::Failed;
                            layer_states[ready.index].error = ready.error;
                        }
                        continue;
                    }
                    if (!ready.done) continue;
                    {
                        std::lock_guard<std::mutex> lk4(hydrate_req_mutex);
                        if (ready.index < hydration_requested.size()) hydration_requested[ready.index] = false;
                    }
                    hydrated_count.fetch_add(1, std::memory_order_relaxed);
                    {
                        fs::path layer_path = root / "data" / "layers" / layers[ready.index].file;
                        fs::path cache_path = root / "data" / "cache" / "hydration" / (layers[ready.index].file + ".msgpack");
                        saveHydrationCache(cache_path, fileSignature(layer_path), layers[ready.index].features);
                    }
                    {
                        std::lock_guard<std::mutex> lk3(status_mutex);
                        if (ready.index < layer_states.size()) {
                            layer_states[ready.index].status = LayerPipelineStatus::Hydrated;
                            layer_states[ready.index].feature_count = layers[ready.index].features.size();
                            layer_states[ready.index].error.clear();
                        }
                    }
                    TriJob tj;
                    tj.index = ready.index;
                    tj.file = layers[ready.index].file;
                    tj.rings_per_feature.reserve(layers[ready.index].features.size());
                    for (const auto& fg : layers[ready.index].features) tj.rings_per_feature.push_back(fg.rings);
                    std::lock_guard<std::mutex> lk2(tri_mutex);
                    {
                        std::lock_guard<std::mutex> lk3(status_mutex);
                        if (ready.index < layer_states.size()) layer_states[ready.index].status = LayerPipelineStatus::TriQueued;
                    }
                    const bool parcel_dep_priority =
                        vacant_layer_active && parcel_layer_idx >= 0 && (int)ready.index == parcel_layer_idx;
                    if (layers[ready.index].enabled || parcel_dep_priority) {
                        tri_jobs.push_front(std::move(tj));
                    } else {
                        tri_jobs.push_back(std::move(tj));
                    }
                    tri_cv.notify_one();
                    trimProcessHeap();
                }
            }
        }
        {
            std::lock_guard<std::mutex> lk(tri_mutex);
            while (!tri_results.empty()) {
                TriResult tr = std::move(tri_results.front());
                tri_results.pop_front();
                if (tr.index < layers.size()) {
                    if (tr.ok) {
                        auto& fs = layers[tr.index].features;
                        size_t n = std::min(fs.size(), tr.triangles_per_feature.size());
                        for (size_t i = 0; i < n; ++i) fs[i].triangles = std::move(tr.triangles_per_feature[i]);
                        if (tr.index < layer_profile_dirty.size()) layer_profile_dirty[tr.index] = true;
                        triangulated_count.fetch_add(1, std::memory_order_relaxed);
                        std::lock_guard<std::mutex> lk3(status_mutex);
                        if (tr.index < layer_states.size()) layer_states[tr.index].status = LayerPipelineStatus::Ready;
                        trimProcessHeap();
                    } else {
                        std::lock_guard<std::mutex> lk3(status_mutex);
                        if (tr.index < layer_states.size()) {
                            layer_states[tr.index].status = LayerPipelineStatus::Failed;
                            layer_states[tr.index].error = tr.error;
                        }
                    }
                }
            }
        }

        if (zoning_layer_idx >= 0 && (size_t)zoning_layer_idx < layers.size()) {
            const auto& zfeats = layers[(size_t)zoning_layer_idx].features;
            if (zfeats.size() != zoning_zone_discovered_feature_count) {
                zoning_zone_discovered_feature_count = zfeats.size();
                zoning_zone_counts.clear();
                zoning_zone_label.clear();
                zoning_group_zones.clear();
                zoning_group_order.clear();
                std::unordered_map<std::string, bool> prev_enabled = zoning_zone_enabled;
                zoning_zone_order.clear();
                std::unordered_set<std::string> seen_zone_keys;
                seen_zone_keys.reserve(zfeats.size() / 4 + 16);
                for (const auto& fg : zfeats) {
                    std::string zkey = zoningClassKey(fg);
                    std::string zlabel = zoningClassLabel(fg);
                    zoning_zone_counts[zkey] += 1;
                    if (seen_zone_keys.insert(zkey).second) zoning_zone_order.push_back(zkey);
                    if (zoning_zone_enabled.find(zkey) == zoning_zone_enabled.end()) {
                        auto it_prev = prev_enabled.find(zkey);
                        zoning_zone_enabled[zkey] = (it_prev == prev_enabled.end()) ? true : it_prev->second;
                    }
                    auto meta_it = zoning_metadata.find(zkey);
                    if (zoning_zone_label.find(zkey) == zoning_zone_label.end()) {
                        zoning_zone_label[zkey] =
                            (meta_it != zoning_metadata.end() && !meta_it->second.label.empty()) ? meta_it->second.label : zlabel;
                    }
                    if (zoning_zone_color.find(zkey) == zoning_zone_color.end()) {
                        if (app_settings.zoning_use_simcity_colors) {
                            zoning_zone_color[zkey] = zoningShadeVariant(zoningColorFromConvention(zkey), zkey);
                        } else {
                            const ImVec4 base_color =
                                (meta_it != zoning_metadata.end() && meta_it->second.has_color) ? meta_it->second.color : colorFromStableKey(zkey);
                            zoning_zone_color[zkey] = zoningShadeVariant(base_color, zkey);
                        }
                    }
                }
                std::sort(zoning_zone_order.begin(), zoning_zone_order.end());
                for (const auto& zkey : zoning_zone_order) {
                    std::string g = zoningGroupKey(zkey);
                    if (zoning_group_zones.find(g) == zoning_group_zones.end()) zoning_group_order.push_back(g);
                    zoning_group_zones[g].push_back(zkey);
                }
                std::sort(zoning_group_order.begin(), zoning_group_order.end());
            }
        }

        if (real_property_layer_idx >= 0) {
            const auto& feats = layers[(size_t)real_property_layer_idx].features;
            if (feats.size() != cached_real_property_size) {
                real_property_by_blocklot.clear();
                for (size_t i = 0; i < feats.size(); ++i) {
                    std::string bl = featureBlockLotJoinKey(feats[i]);
                    if (!bl.empty() && real_property_by_blocklot.find(bl) == real_property_by_blocklot.end()) {
                        real_property_by_blocklot[bl] = i;
                    }
                }
                cached_real_property_size = feats.size();
                owner_aggregates_dirty = true;
            }
        }
        if (vacant_notice_layer_idx >= 0) {
            const auto& feats = layers[(size_t)vacant_notice_layer_idx].features;
            if (feats.size() != cached_vac_notice_size) {
                vacant_notice_count_by_blocklot.clear();
                for (const auto& fg : feats) {
                    std::string bl = normalizeJoinKey(getPropertyValue(fg, "BLOCKLOT"));
                    if (!bl.empty()) vacant_notice_count_by_blocklot[bl] += 1;
                }
                cached_vac_notice_size = feats.size();
                vacancy_maps_generation += 1;
            }
        }
        if (vacant_rehab_layer_idx >= 0) {
            const auto& feats = layers[(size_t)vacant_rehab_layer_idx].features;
            if (feats.size() != cached_vac_rehab_size) {
                vacant_rehab_count_by_blocklot.clear();
                for (const auto& fg : feats) {
                    std::string bl = normalizeJoinKey(getPropertyValue(fg, "BLOCKLOT"));
                    if (!bl.empty()) vacant_rehab_count_by_blocklot[bl] += 1;
                }
                cached_vac_rehab_size = feats.size();
                vacancy_maps_generation += 1;
            }
        }
        if (tax_lien_layer_idx >= 0) {
            const auto& feats = layers[(size_t)tax_lien_layer_idx].features;
            if (feats.size() != cached_tax_lien_size) {
                tax_lien_count_by_blocklot.clear();
                tax_lien_amount_by_blocklot.clear();
                for (const auto& fg : feats) {
                    std::string bl = featureBlockLotJoinKey(fg);
                    if (bl.empty()) continue;
                    tax_lien_count_by_blocklot[bl] += 1;
                    tax_lien_amount_by_blocklot[bl] += parseNumericField(getPropertyValue(fg, "TOTAL_AMOUNT"));
                }
                cached_tax_lien_size = feats.size();
                tax_maps_generation += 1;
            }
        }
        if (tax_sale_layer_idx >= 0) {
            const auto& feats = layers[(size_t)tax_sale_layer_idx].features;
            if (feats.size() != cached_tax_sale_size) {
                tax_sale_count_by_blocklot.clear();
                tax_sale_amount_by_blocklot.clear();
                for (const auto& fg : feats) {
                    std::string bl = featureBlockLotJoinKey(fg);
                    if (bl.empty()) continue;
                    tax_sale_count_by_blocklot[bl] += 1;
                    double amount = parseNumericField(getPropertyValue(fg, "total_lien"));
                    if (amount <= 0.0) amount = parseNumericField(getPropertyValue(fg, "total_3yea"));
                    if (amount <= 0.0) amount = parseNumericField(getPropertyValue(fg, "total_tax"));
                    tax_sale_amount_by_blocklot[bl] += amount;
                }
                cached_tax_sale_size = feats.size();
                tax_maps_generation += 1;
            }
        }
        if (parcel_layer_idx >= 0) {
            const auto& pfeats = layers[(size_t)parcel_layer_idx].features;
            if (parcel_vac_notice_by_feature.size() != pfeats.size() ||
                parcel_vac_rehab_by_feature.size() != pfeats.size() ||
                parcel_vacancy_generation_applied != vacancy_maps_generation) {
                parcel_vac_notice_by_feature.assign(pfeats.size(), 0);
                parcel_vac_rehab_by_feature.assign(pfeats.size(), 0);
                size_t notice_rows_matched = 0;
                size_t rehab_rows_matched = 0;
                for (size_t i = 0; i < pfeats.size(); ++i) {
                    std::string bl = normalizeJoinKey(getPropertyValue(pfeats[i], "BLOCKLOT"));
                    auto itn = vacant_notice_count_by_blocklot.find(bl);
                    if (itn != vacant_notice_count_by_blocklot.end()) {
                        parcel_vac_notice_by_feature[i] = itn->second;
                        notice_rows_matched += (size_t)itn->second;
                    }
                    auto itr = vacant_rehab_count_by_blocklot.find(bl);
                    if (itr != vacant_rehab_count_by_blocklot.end()) {
                        parcel_vac_rehab_by_feature[i] = itr->second;
                        rehab_rows_matched += (size_t)itr->second;
                    }
                }
                parcel_vacancy_generation_applied = vacancy_maps_generation;
                vacant_notice_rows_matched_total.store(notice_rows_matched, std::memory_order_relaxed);
                vacant_rehab_rows_matched_total.store(rehab_rows_matched, std::memory_order_relaxed);
                const fs::path derived_path = root / "data" / "cache" / "derived" / "parcel_vacancy_status.json";
                saveDerivedVacancyStatus(
                    derived_path,
                    pfeats,
                    parcel_vac_notice_by_feature,
                    parcel_vac_rehab_by_feature,
                    (size_t)cached_vac_notice_size,
                    (size_t)cached_vac_rehab_size,
                    notice_rows_matched,
                    rehab_rows_matched);
                owner_aggregates_dirty = true;
            }
            if (parcel_tax_lien_by_feature.size() != pfeats.size() ||
                parcel_tax_sale_by_feature.size() != pfeats.size() ||
                parcel_tax_generation_applied != tax_maps_generation) {
                parcel_tax_lien_by_feature.assign(pfeats.size(), 0);
                parcel_tax_sale_by_feature.assign(pfeats.size(), 0);
                parcel_tax_lien_amount_by_feature.assign(pfeats.size(), 0.0);
                parcel_tax_sale_amount_by_feature.assign(pfeats.size(), 0.0);
                for (size_t i = 0; i < pfeats.size(); ++i) {
                    std::string bl = featureBlockLotJoinKey(pfeats[i]);
                    auto it_lien = tax_lien_count_by_blocklot.find(bl);
                    if (it_lien != tax_lien_count_by_blocklot.end()) {
                        parcel_tax_lien_by_feature[i] = it_lien->second;
                        auto it_amt = tax_lien_amount_by_blocklot.find(bl);
                        if (it_amt != tax_lien_amount_by_blocklot.end()) parcel_tax_lien_amount_by_feature[i] = it_amt->second;
                    }
                    auto it_sale = tax_sale_count_by_blocklot.find(bl);
                    if (it_sale != tax_sale_count_by_blocklot.end()) {
                        parcel_tax_sale_by_feature[i] = it_sale->second;
                        auto it_amt = tax_sale_amount_by_blocklot.find(bl);
                        if (it_amt != tax_sale_amount_by_blocklot.end()) parcel_tax_sale_amount_by_feature[i] = it_amt->second;
                    }
                }
                parcel_tax_generation_applied = tax_maps_generation;
                owner_aggregates_dirty = true;
            }
            size_t matched_total = 0;
            size_t with_geometry_total = 0;
            size_t triangulated_renderable_total = 0;
            for (size_t i = 0; i < pfeats.size(); ++i) {
                const int vac_notice = (i < parcel_vac_notice_by_feature.size()) ? parcel_vac_notice_by_feature[i] : 0;
                const int vac_rehab = (i < parcel_vac_rehab_by_feature.size()) ? parcel_vac_rehab_by_feature[i] : 0;
                if ((vac_notice + vac_rehab) <= 0) continue;
                matched_total++;
                if (!pfeats[i].rings.empty()) with_geometry_total++;
                if (!pfeats[i].rings.empty() && !pfeats[i].triangles.empty()) triangulated_renderable_total++;
            }
            vacant_parcels_matched_total.store(matched_total, std::memory_order_relaxed);
            vacant_parcels_with_geometry_total.store(with_geometry_total, std::memory_order_relaxed);
            vacant_parcels_triangulated_renderable_total.store(triangulated_renderable_total, std::memory_order_relaxed);
        }
        for (size_t li = 0; li < layers.size(); ++li) {
            LayerPipelineStatus st = LayerPipelineStatus::Queued;
            {
                std::lock_guard<std::mutex> lk(status_mutex);
                if (li < layer_states.size()) st = layer_states[li].status;
            }
            const bool stable =
                st == LayerPipelineStatus::Hydrated ||
                st == LayerPipelineStatus::TriQueued ||
                st == LayerPipelineStatus::Triangulating ||
                st == LayerPipelineStatus::Ready;
            if (!stable) continue;
            if (!layer_spatial[li].built || layer_spatial[li].feature_count_built != layers[li].features.size()) {
                buildLayerSpatialIndex(layers[li], layer_spatial[li]);
                if (li < layer_profile_dirty.size()) layer_profile_dirty[li] = true;
            }
        }
        refresh_layer_profile_snapshot();

        auto real_property_for_parcel = [&](const LayerDef::FeatureGeom& parcel) -> const LayerDef::FeatureGeom* {
            if (real_property_layer_idx < 0 || (size_t)real_property_layer_idx >= layers.size()) return nullptr;
            std::string blocklot = featureBlockLotJoinKey(parcel);
            if (blocklot.empty()) return nullptr;
            auto itrp = real_property_by_blocklot.find(blocklot);
            if (itrp == real_property_by_blocklot.end()) return nullptr;
            const auto& rp_layer = layers[(size_t)real_property_layer_idx];
            if (itrp->second >= rp_layer.features.size()) return nullptr;
            return &rp_layer.features[itrp->second];
        };
        auto text_prop = [](const char* label, const std::string& value) {
            if (!value.empty()) ImGui::TextWrapped("%s: %s", label, value.c_str());
        };
        auto money_prop = [](const char* label, const std::string& value) {
            if (value.empty()) return;
            const double amount = parseNumericField(value);
            if (amount > 0.0) ImGui::TextWrapped("%s: $%.2f", label, amount);
            else ImGui::TextWrapped("%s: %s", label, value.c_str());
        };
        auto draw_real_property_summary = [&](const LayerDef::FeatureGeom* rp) {
            ImGui::Separator();
            ImGui::TextUnformatted("Ownership / Assessment");
            if (!rp) {
                ImGui::TextDisabled("No matching Real Property Information record loaded for this parcel.");
                ImGui::TextDisabled("Source expected: data/layers/real_property_information.geojson");
                return;
            }
            const std::string property_address = firstDisplayProperty(*rp, {
                "FULLADDR", "FULL_ADDRESS", "PROPERTY_ADDRESS", "PROPERTYADDR", "PREMISEADD",
                "PREMISE_ADDRESS", "ADDRESS", "Address", "ADDR", "ADDR1", "ADDRESS1",
                "SITE_ADDR", "SITUSADDR", "LOCATION", "Location"
            });
            if (!property_address.empty()) {
                ImGui::TextDisabled("Property Address");
                ImGui::SetWindowFontScale(1.22f);
                ImGui::TextWrapped("%s", property_address.c_str());
                ImGui::SetWindowFontScale(1.0f);
                ImGui::Separator();
            }
            text_prop("Owner 1", firstDisplayProperty(*rp, {"OWNER_1", "OWNERNME1", "OWNER", "OWNER_NAME"}));
            text_prop("Owner 2", firstDisplayProperty(*rp, {"OWNER_2"}));
            text_prop("Owner 3", firstDisplayProperty(*rp, {"OWNER_3"}));
            text_prop("Owner Abbrev", firstDisplayProperty(*rp, {"OWNER_ABBR"}));
            text_prop("Mailing Address", firstDisplayProperty(*rp, {"MAILTOADD"}));
            text_prop("Use Group", firstDisplayProperty(*rp, {"USEGROUP"}));
            text_prop("DHCD Use", firstDisplayProperty(*rp, {"DHCDUSE1"}));
            text_prop("Zone Code", firstDisplayProperty(*rp, {"ZONECODE"}));
            text_prop("Year Built", firstDisplayProperty(*rp, {"YEAR_BUILD"}));
            text_prop("Lot Size", firstDisplayProperty(*rp, {"LOT_SIZE"}));
            text_prop("Structure Area", firstDisplayProperty(*rp, {"STRUCTAREA"}));
            money_prop("Current Land", firstDisplayProperty(*rp, {"CURRLAND"}));
            money_prop("Current Improvements", firstDisplayProperty(*rp, {"CURRIMPR"}));
            money_prop("Tax Base", firstDisplayProperty(*rp, {"TAXBASE", "ARTAXBAS"}));
            money_prop("City Tax", firstDisplayProperty(*rp, {"CITY_TAX"}));
            money_prop("State Tax", firstDisplayProperty(*rp, {"STATETAX"}));
            money_prop("Sale Price", firstDisplayProperty(*rp, {"SALEPRIC"}));
            text_prop("Sale Date", firstDisplayProperty(*rp, {"SALEDATE"}));
            const std::string deed_book = firstDisplayProperty(*rp, {"DEEDBOOK"});
