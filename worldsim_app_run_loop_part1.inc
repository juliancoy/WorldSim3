    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        int fb_w = 0;
        int fb_h = 0;
        glfwGetFramebufferSize(window, &fb_w, &fb_h);
        if (fb_w > 0 && fb_h > 0 && (fb_w != g_MainWindowData.Width || fb_h != g_MainWindowData.Height)) {
            w = fb_w;
            h = fb_h;
            g_SwapChainRebuild = true;
        }

        if (g_SwapChainRebuild) {
            glfwGetFramebufferSize(window, &w, &h);
            if (w > 0 && h > 0) {
                ImGui_ImplVulkan_SetMinImageCount(g_MinImageCount);
                ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, &g_MainWindowData, g_QueueFamily, g_Allocator, w, h, g_MinImageCount);
                g_MainWindowData.FrameIndex = 0;
                g_SwapChainRebuild = false;
            }
        }

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGuiIO& frame_io = ImGui::GetIO();
        const float layout_w = std::max(1.0f, frame_io.DisplaySize.x);
        const float layout_h = std::max(1.0f, frame_io.DisplaySize.y);
        constexpr float layout_margin = 12.0f;
        constexpr float layout_gap = 8.0f;
        const float content_w = std::max(1.0f, layout_w - layout_margin * 2.0f - layout_gap * 2.0f);
        double left_frac = std::clamp(ui_left_panel_frac.load(std::memory_order_relaxed), 0.08, 0.70);
        double right_frac = std::clamp(ui_right_panel_frac.load(std::memory_order_relaxed), 0.08, 0.50);
        if (left_frac + right_frac > 0.92) {
            const double scale = 0.92 / (left_frac + right_frac);
            left_frac *= scale;
            right_frac *= scale;
        }
        ui_left_panel_frac.store(left_frac, std::memory_order_relaxed);
        ui_right_panel_frac.store(right_frac, std::memory_order_relaxed);
        const float left_panel_w = content_w * (float)left_frac;
        const float right_panel_w = content_w * (float)right_frac;
        const float map_w = std::max(1.0f, content_w - left_panel_w - right_panel_w);
        const float map_x = left_panel_w + layout_margin + layout_gap;
        const float main_panel_h = std::max(0.0f, layout_h - layout_margin * 2.0f);
        const bool text_scale_up =
            frame_io.KeyCtrl &&
            (ImGui::IsKeyPressed(ImGuiKey_Equal, false) ||
             ImGui::IsKeyPressed(ImGuiKey_KeypadAdd, false));
        const bool text_scale_down =
            frame_io.KeyCtrl &&
            (ImGui::IsKeyPressed(ImGuiKey_Minus, false) ||
             ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, false));
        if (text_scale_up || text_scale_down) {
            ui_text_scale = std::clamp(ui_text_scale + (text_scale_up ? 0.10f : -0.10f), 0.70f, 1.80f);
            frame_io.FontGlobalScale = ui_text_scale;
        } else if (std::abs(frame_io.FontGlobalScale - ui_text_scale) > 0.001f) {
            frame_io.FontGlobalScale = ui_text_scale;
        }
        drainRetiredTextures();
        const auto prof_frame_begin = std::chrono::steady_clock::now();
        auto prof_ms_since = [](std::chrono::steady_clock::time_point t) {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
        };
        size_t prof_tiles_drawn_frame = 0;
        size_t prof_features_considered_frame = 0;
        size_t prof_features_drawn_frame = 0;
        bool layer_fill_state_changed = false;
        bool layer_hover_state_changed = false;
        bool layer_inspect_state_changed = false;
        bool layer_heatmap_state_changed = false;
        bool heatmap_settings_state_changed = false;
        bool heatmap_controls_active = false;
        auto heatmap_input_float_enter = [&](const char* label, float& value, float min_value, float max_value, const char* format) {
            static std::unordered_map<ImGuiID, float> drafts;
            static std::unordered_map<ImGuiID, bool> was_active;
            const ImGuiID id = ImGui::GetID(label);
            if (!was_active[id]) drafts[id] = value;
            float draft = drafts[id];
            const bool committed = ImGui::InputFloat(label, &draft, 0.0f, 0.0f, format, ImGuiInputTextFlags_EnterReturnsTrue);
            const bool active = ImGui::IsItemActive();
            heatmap_controls_active |= active;
            if (committed) {
                value = std::clamp(draft, min_value, max_value);
                drafts[id] = value;
                was_active[id] = active;
                return true;
            }
            drafts[id] = active ? draft : value;
            was_active[id] = active;
            return false;
        };
        auto refresh_layer_profile_snapshot = [&]() {
            bool any_dirty = false;
            for (bool dirty : layer_profile_dirty) {
                if (dirty) {
                    any_dirty = true;
                    break;
                }
            }
            if (!any_dirty) return;
            std::vector<LayerProfileSnapshot> updates;
            updates.reserve(layers.size());
            for (size_t i = 0; i < layers.size(); ++i) {
                if (i >= layer_profile_dirty.size() || !layer_profile_dirty[i]) continue;
                const auto& layer = layers[i];
                LayerProfileSnapshot s;
                s.index = i;
                s.name = layer.name;
                s.file = layer.file;
                s.enabled = layer.enabled;
                s.features = layer.features.size();
                for (const auto& fg : layer.features) {
                    s.rings += fg.rings.size();
                    s.triangle_indices += fg.triangles.size();
                    s.properties += fg.properties.size();
                    for (const auto& r : fg.rings) s.ring_points += r.size();
                }
                if (i < layer_spatial.size()) {
                    s.spatial_index_built = layer_spatial[i].built;
                    s.spatial_index_cells = layer_spatial[i].cells.size();
                    s.spatial_index_marks = layer_spatial[i].marks.size();
                }
                updates.push_back(std::move(s));
            }
            {
                std::lock_guard<std::mutex> lk(layer_profile_mutex);
                for (const auto& s : updates) {
                    if (s.index < layer_profile_snapshot.size()) layer_profile_snapshot[s.index] = s;
                }
            }
            std::fill(layer_profile_dirty.begin(), layer_profile_dirty.end(), false);
        };
        // Apply REST control commands on main thread.
        {
            int zc = api_zoom_cmd.exchange(-1, std::memory_order_relaxed);
            if (zc >= kMinZoom && zc <= kMaxZoom) zoom = zc;
            double lonc = api_lon_cmd.exchange(std::numeric_limits<double>::quiet_NaN(), std::memory_order_relaxed);
            double latc = api_lat_cmd.exchange(std::numeric_limits<double>::quiet_NaN(), std::memory_order_relaxed);
            if (!std::isnan(lonc)) center_lon = lonc;
            if (!std::isnan(latc)) center_lat = std::clamp(latc, -85.0, 85.0);
            std::lock_guard<std::mutex> lk(api_layer_mutex);
            for (const auto& kv : api_layer_enable_cmds) {
                for (size_t i = 0; i < layers.size(); ++i) {
                    if (layers[i].file == kv.first) {
                        layers[i].enabled = kv.second;
                        if (i < layer_profile_dirty.size()) layer_profile_dirty[i] = true;
                    }
                }
            }
            api_layer_enable_cmds.clear();
            if (!api_layer_fill_cmds.empty()) {
                std::lock_guard<std::mutex> lk_fill(layer_fill_mutex);
                for (const auto& kv : api_layer_fill_cmds) {
                    for (size_t i = 0; i < layers.size(); ++i) {
                        if (layers[i].file == kv.first && i < layer_fill_enabled.size()) {
                            if (layer_fill_enabled[i] != kv.second) {
                                layer_fill_enabled[i] = kv.second;
                                layer_fill_state_changed = true;
                            }
                        }
                    }
                }
                api_layer_fill_cmds.clear();
            }
        }
        current_zoom_state.store(zoom, std::memory_order_relaxed);
        current_lon_state.store(center_lon, std::memory_order_relaxed);
        current_lat_state.store(center_lat, std::memory_order_relaxed);
        auto start_basemap_download = [&](const std::string& target_dir, const std::string& url_tmpl) {
            basemap_download_inflight = true;
            basemap_download_label = target_dir;
            basemap_download_started_at = std::chrono::steady_clock::now();
            basemap_download_stop_requested.store(false, std::memory_order_relaxed);
            basemap_download_stopped.store(false, std::memory_order_relaxed);
            basemap_download_done.store(0, std::memory_order_relaxed);
            basemap_download_total.store(0, std::memory_order_relaxed);
            basemap_download_downloaded.store(0, std::memory_order_relaxed);
            basemap_download_skipped.store(0, std::memory_order_relaxed);
            basemap_download_failed.store(0, std::memory_order_relaxed);
            basemap_download_bytes.store(0, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lk(basemap_download_mutex);
                basemap_download_progress = "Preparing download...";
            }
            data_library_status_msg = "Basemap download started (" + target_dir + ")";
            basemap_download_future = std::async(std::launch::async, [&, root, target_dir, url_tmpl]() {
                const fs::path base = root / "data" / target_dir;
                fs::create_directories(base);
                const double min_lon = -76.75;
                const double max_lon = -76.45;
                const double min_lat = 39.18;
                const double max_lat = 39.40;
                size_t total = 0;
                for (int z = 11; z <= 18; ++z) {
                    auto [x0, y_max] = deg2num(min_lat, min_lon, z);
                    auto [x1, y_min] = deg2num(max_lat, max_lon, z);
                    if (x0 > x1) std::swap(x0, x1);
                    if (y_min > y_max) std::swap(y_min, y_max);
                    total += (size_t)(x1 - x0 + 1) * (size_t)(y_max - y_min + 1);
                }
                basemap_download_total.store(total, std::memory_order_relaxed);
                size_t done = 0;
                for (int z = 11; z <= 18; ++z) {
                    if (basemap_download_stop_requested.load(std::memory_order_relaxed)) {
                        basemap_download_stopped.store(true, std::memory_order_relaxed);
                        break;
                    }
                    auto [x0, y_max] = deg2num(min_lat, min_lon, z);
                    auto [x1, y_min] = deg2num(max_lat, max_lon, z);
                    if (x0 > x1) std::swap(x0, x1);
                    if (y_min > y_max) std::swap(y_min, y_max);
                    for (int x = x0; x <= x1; ++x) {
                        if (basemap_download_stop_requested.load(std::memory_order_relaxed)) {
                            basemap_download_stopped.store(true, std::memory_order_relaxed);
                            break;
                        }
                        for (int y = y_min; y <= y_max; ++y) {
                            if (basemap_download_stop_requested.load(std::memory_order_relaxed)) {
                                basemap_download_stopped.store(true, std::memory_order_relaxed);
                                break;
                            }
                            fs::path out = base / std::to_string(z) / std::to_string(x) / (std::to_string(y) + ".png");
                            std::error_code ec;
                            if (fs::exists(out, ec) && !ec) {
                                basemap_download_skipped.fetch_add(1, std::memory_order_relaxed);
                            } else {
                                std::string url = url_tmpl;
                                auto replace_all = [&](const std::string& from, const std::string& to) {
                                    size_t pos = 0;
                                    while ((pos = url.find(from, pos)) != std::string::npos) {
                                        url.replace(pos, from.size(), to);
                                        pos += to.size();
                                    }
                                };
                                replace_all("{z}", std::to_string(z));
                                replace_all("{x}", std::to_string(x));
                                replace_all("{y}", std::to_string(y));
                                std::string err;
                                if (downloadUrlToFile(url, out, err)) {
                                    basemap_download_downloaded.fetch_add(1, std::memory_order_relaxed);
                                    std::error_code sec;
                                    const auto sz = fs::file_size(out, sec);
                                    if (!sec) basemap_download_bytes.fetch_add((size_t)sz, std::memory_order_relaxed);
                                } else {
                                    basemap_download_failed.fetch_add(1, std::memory_order_relaxed);
                                }
                            }
                            done++;
                            basemap_download_done.store(done, std::memory_order_relaxed);
                            if (done % 32 == 0 || done == total) {
                                std::lock_guard<std::mutex> lk(basemap_download_mutex);
                                basemap_download_progress =
                                    "Tiles " + std::to_string(done) + "/" + std::to_string(total) +
                                    " | downloaded " + std::to_string(basemap_download_downloaded.load(std::memory_order_relaxed)) +
                                    ", skipped " + std::to_string(basemap_download_skipped.load(std::memory_order_relaxed)) +
                                    ", failed " + std::to_string(basemap_download_failed.load(std::memory_order_relaxed));
                            }
                        }
                        if (basemap_download_stopped.load(std::memory_order_relaxed)) break;
                    }
                    if (basemap_download_stopped.load(std::memory_order_relaxed)) break;
                }
            });
        };
        auto request_basemap_download = [&](const std::string& target_dir, const std::string& url_tmpl) {
            if (basemap_download_inflight) {
                basemap_download_queue.push_back({target_dir, url_tmpl});
                data_library_status_msg = "Queued basemap download (" + target_dir + "). Queue size: " + std::to_string(basemap_download_queue.size());
                return;
            }
            start_basemap_download(target_dir, url_tmpl);
        };
        if (basemap_download_inflight && basemap_download_future.valid() &&
            basemap_download_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            basemap_download_future.get();
            basemap_download_inflight = false;
            const bool stopped = basemap_download_stopped.load(std::memory_order_relaxed);
            data_library_status_msg = std::string(stopped ? "Basemap download stopped (" : "Basemap download (") + basemap_download_label + "): " +
                std::to_string(basemap_download_downloaded.load(std::memory_order_relaxed)) + " downloaded, " +
                std::to_string(basemap_download_skipped.load(std::memory_order_relaxed)) + " skipped, " +
                std::to_string(basemap_download_failed.load(std::memory_order_relaxed)) + " failed";
            if (!stopped && !basemap_download_queue.empty()) {
                const auto next = basemap_download_queue.front();
                basemap_download_queue.pop_front();
                start_basemap_download(next.first, next.second);
            } else if (stopped) {
                basemap_download_queue.clear();
            }
        }
        ImGui::SetNextWindowPos(ImVec2(layout_margin, layout_margin), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(left_panel_w, main_panel_h), ImGuiCond_Always);
        ImGui::Begin("Layers and Controls", nullptr, ImGuiWindowFlags_NoCollapse);
        if (ImGui::Button("Gear")) show_sources_panel = !show_sources_panel;
        ImGui::SameLine();
        if (ImGui::Button("Library")) show_data_library = !show_data_library;
        ImGui::SameLine();
        ImGui::Text("Vulkan map + Vulkan UI");
        size_t local_layer_count = 0;
        for (size_t i = 0; i < local_layer_exists_cache.size(); ++i) {
            if (local_layer_exists_cache[i]) local_layer_count++;
        }
        ImGui::TextDisabled("Local data: %zu/%zu", local_layer_count, layers.size());
        ImGui::SliderInt("Zoom", &zoom, kMinZoom, kMaxZoom);
        const double lon_min = -76.75, lon_max = -76.45;
        const double lat_min = 39.18, lat_max = 39.40;
        ImGui::SliderScalar("Center Lon", ImGuiDataType_Double, &center_lon, &lon_min, &lon_max, "%.6f");
        ImGui::SliderScalar("Center Lat", ImGuiDataType_Double, &center_lat, &lat_min, &lat_max, "%.6f");
        ImGui::Checkbox("Enable Hover Inspector", &hover_inspector_enabled);
        ImGui::SeparatorText("Heatmap");
        ImGui::TextDisabled("Global heatmap controls removed.");
        ImGui::TextDisabled("Use each layer's settings (⚙) to configure aggregate method and heatmap parameters.");
        bool validation_ui = g_EnableValidationLayers;
        if (ImGui::Checkbox("Vulkan Validation (restart required)", &validation_ui)) {
            g_EnableValidationLayers = validation_ui;
            app_settings.vulkan_validation_enabled = g_EnableValidationLayers;
            saveAppSettings(root, app_settings);
        }
        const bool topo_tiles_available =
            fs::exists(root / "data" / "tiles_topo") ||
            fs::exists(root / "data" / "tiles_topographic");
        const char* basemap_options[] = {"OpenStreetMap", "Topographic"};
        int basemap_style_ui = std::clamp(app_settings.basemap_style, 0, 1);
        ImGui::BeginDisabled(!topo_tiles_available);
        if (ImGui::Combo("Underlying map source", &basemap_style_ui, basemap_options, IM_ARRAYSIZE(basemap_options))) {
            if (!topo_tiles_available && basemap_style_ui == 1) basemap_style_ui = 0;
            app_settings.basemap_style = basemap_style_ui;
            saveAppSettings(root, app_settings);
        }
        ImGui::EndDisabled();
        if (!topo_tiles_available) {
            ImGui::TextDisabled("Topographic tiles unavailable (expected: data/tiles_topo or data/tiles_topographic).");
        }
        int reserve_cpu_cores_ui = std::clamp(app_settings.reserve_cpu_cores, 0, 8);
        if (ImGui::SliderInt("Reserve CPU cores (restart required)", &reserve_cpu_cores_ui, 0, 8)) {
            app_settings.reserve_cpu_cores = reserve_cpu_cores_ui;
            saveAppSettings(root, app_settings);
        }
        ImGui::Separator();

        if (ImGui::Button("Show All")) {
            for (auto& l : layers) l.enabled = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Hide All")) {
            for (auto& l : layers) l.enabled = false;
        }

        if (ImGui::CollapsingHeader("Basemap Layers", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto dir_has_any_file = [&](const fs::path& p) {
                std::error_code ec;
                if (!fs::exists(p, ec) || ec) return false;
                for (fs::recursive_directory_iterator it(p, ec), end; !ec && it != end; it.increment(ec)) {
                    if (it->is_regular_file(ec)) return true;
                }
                return false;
            };
            auto count_missing_tiles = [&](const std::string& target_dir) -> std::pair<size_t, size_t> {
                const fs::path base = root / "data" / target_dir;
                const double min_lon = -76.75;
                const double max_lon = -76.45;
                const double min_lat = 39.18;
                const double max_lat = 39.40;
                size_t missing = 0;
                size_t total = 0;
                for (int z = 11; z <= 18; ++z) {
                    auto [x0, y_max] = deg2num(min_lat, min_lon, z);
                    auto [x1, y_min] = deg2num(max_lat, max_lon, z);
                    if (x0 > x1) std::swap(x0, x1);
                    if (y_min > y_max) std::swap(y_min, y_max);
                    for (int x = x0; x <= x1; ++x) {
                        for (int y = y_min; y <= y_max; ++y) {
                            total++;
                            std::error_code ec;
                            fs::path out = base / std::to_string(z) / std::to_string(x) / (std::to_string(y) + ".png");
                            if (!(fs::exists(out, ec) && !ec)) missing++;
                        }
                    }
                }
                return {missing, total};
            };
            const auto [osm_missing, osm_total] = count_missing_tiles("tiles");
            const auto [topo_missing, topo_total] = count_missing_tiles("tiles_topo");
            const bool osm_tiles_available = dir_has_any_file(root / "data" / "tiles");
            const bool topo_tiles_available = dir_has_any_file(root / "data" / "tiles_topo") || dir_has_any_file(root / "data" / "tiles_topographic");
            const bool osm_incomplete = osm_total > 0 && osm_missing > 0;
            const bool topo_incomplete = topo_total > 0 && topo_missing > 0;
            int basemap_style_ui = std::clamp(app_settings.basemap_style, 0, 1);
            if (!osm_tiles_available || osm_incomplete) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.22f, 0.12f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.30f, 0.14f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.46f, 0.18f, 0.08f, 1.0f));
                if (ImGui::SmallButton("D##osm_basemap")) {
                    request_basemap_download("tiles", "https://tile.openstreetmap.org/{z}/{x}/{y}.png");
                }
                ImGui::PopStyleColor(3);
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted("Download missing OpenStreetMap tiles");
                    ImGui::EndTooltip();
                }
                ImGui::SameLine();
            }
            if (ImGui::RadioButton("OpenStreetMap", basemap_style_ui == 0)) {
                app_settings.basemap_style = 0;
                saveAppSettings(root, app_settings);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("?##osm_display_settings")) ImGui::OpenPopup("osm_display_settings");
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted("Layer display settings");
                ImGui::EndTooltip();
            }
            if (ImGui::BeginPopup("osm_display_settings")) {
                ImGui::TextUnformatted("OpenStreetMap");
                ImGui::Separator();
                if (ImGui::Checkbox("Greyscale underlying map##osm", &app_settings.grayscale_basemap)) {
                    saveAppSettings(root, app_settings);
                }
                ImGui::TextDisabled("Tile source: data/tiles");
                ImGui::EndPopup();
            }
            if (!topo_tiles_available || topo_incomplete) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.22f, 0.12f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.30f, 0.14f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.46f, 0.18f, 0.08f, 1.0f));
                if (ImGui::SmallButton("D##topo_basemap")) {
                    request_basemap_download("tiles_topo", "https://tile.opentopomap.org/{z}/{x}/{y}.png");
                }
                ImGui::PopStyleColor(3);
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted("Download missing Topographic tiles");
                    ImGui::EndTooltip();
                }
                ImGui::SameLine();
            }
            ImGui::BeginDisabled(!topo_tiles_available);
            if (ImGui::RadioButton("Topographic", basemap_style_ui == 1)) {
                app_settings.basemap_style = 1;
                saveAppSettings(root, app_settings);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!topo_tiles_available);
            if (ImGui::SmallButton("?##topo_display_settings")) ImGui::OpenPopup("topo_display_settings");
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted("Layer display settings");
                ImGui::EndTooltip();
            }
            if (ImGui::BeginPopup("topo_display_settings")) {
                ImGui::TextUnformatted("Topographic");
                ImGui::Separator();
                if (ImGui::Checkbox("Greyscale underlying map##topo", &app_settings.grayscale_basemap)) {
                    saveAppSettings(root, app_settings);
                }
                ImGui::TextDisabled("Tile source: data/tiles_topo");
                ImGui::EndPopup();
            }
            if (!topo_tiles_available) {
                ImGui::TextDisabled("Topographic tiles unavailable.");
            }
            if (osm_total > 0) {
                ImGui::TextDisabled("OSM coverage: %zu/%zu tiles present (z11-z18)", (size_t)(osm_total - osm_missing), osm_total);
            }
            if (topo_total > 0) {
                ImGui::TextDisabled("Topo coverage: %zu/%zu tiles present (z11-z18)", (size_t)(topo_total - topo_missing), topo_total);
            }
            if (basemap_download_inflight) {
                const size_t done = basemap_download_done.load(std::memory_order_relaxed);
                const size_t total = std::max<size_t>(1, basemap_download_total.load(std::memory_order_relaxed));
                const float frac = std::clamp((float)done / (float)total, 0.0f, 1.0f);
                const double elapsed_s = std::max(0.001, std::chrono::duration<double>(std::chrono::steady_clock::now() - basemap_download_started_at).count());
                const double tiles_per_s = (double)done / elapsed_s;
                const double bytes_per_s = (double)basemap_download_bytes.load(std::memory_order_relaxed) / elapsed_s;
                const size_t remaining = (done < total) ? (total - done) : 0;
                const double eta_s = tiles_per_s > 0.001 ? ((double)remaining / tiles_per_s) : 0.0;
                std::string progress_line;
                {
                    std::lock_guard<std::mutex> lk(basemap_download_mutex);
                    progress_line = basemap_download_progress;
                }
                ImGui::Separator();
                ImGui::TextUnformatted("Basemap download in progress...");
                ImGui::ProgressBar(frac, ImVec2(-1.0f, 0.0f));
                ImGui::Text("Speed: %.2f tiles/s | %.2f MB/s | ETA: %.0fs", tiles_per_s, bytes_per_s / (1024.0 * 1024.0), eta_s);
                if (!basemap_download_queue.empty()) ImGui::TextDisabled("Queued requests: %zu", basemap_download_queue.size());
                if (ImGui::Button("Stop Basemap Jobs")) {
                    basemap_download_stop_requested.store(true, std::memory_order_relaxed);
                    basemap_download_queue.clear();
                    data_library_status_msg = "Stop requested for basemap downloads";
                }
                if (!progress_line.empty()) ImGui::TextWrapped("%s", progress_line.c_str());
            }
        }

        if (basemap_download_inflight) {
            ImGui::SetNextWindowSize(ImVec2(460.0f, 0.0f), ImGuiCond_Always);
            if (ImGui::Begin("Basemap Download", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
                const size_t done = basemap_download_done.load(std::memory_order_relaxed);
                const size_t total = std::max<size_t>(1, basemap_download_total.load(std::memory_order_relaxed));
                const float frac = std::clamp((float)done / (float)total, 0.0f, 1.0f);
                const double elapsed_s = std::max(0.001, std::chrono::duration<double>(std::chrono::steady_clock::now() - basemap_download_started_at).count());
                const double tiles_per_s = (double)done / elapsed_s;
                const double bytes_per_s = (double)basemap_download_bytes.load(std::memory_order_relaxed) / elapsed_s;
                const size_t remaining = (done < total) ? (total - done) : 0;
                const double eta_s = tiles_per_s > 0.001 ? ((double)remaining / tiles_per_s) : 0.0;
                std::string progress_line;
                {
                    std::lock_guard<std::mutex> lk(basemap_download_mutex);
                    progress_line = basemap_download_progress;
                }
                ImGui::Text("Source: %s", basemap_download_label.c_str());
                ImGui::ProgressBar(frac, ImVec2(420.0f, 0.0f));
                ImGui::Text("Done: %zu / %zu", done, total);
                ImGui::Text("Downloaded: %zu | Skipped: %zu | Failed: %zu",
                            basemap_download_downloaded.load(std::memory_order_relaxed),
                            basemap_download_skipped.load(std::memory_order_relaxed),
                            basemap_download_failed.load(std::memory_order_relaxed));
                ImGui::Text("Speed: %.2f tiles/s | %.2f MB/s | ETA: %.0fs", tiles_per_s, bytes_per_s / (1024.0 * 1024.0), eta_s);
                ImGui::Text("Bytes downloaded: %.2f MB", (double)basemap_download_bytes.load(std::memory_order_relaxed) / (1024.0 * 1024.0));
                if (!basemap_download_queue.empty()) ImGui::TextDisabled("Queued requests: %zu", basemap_download_queue.size());
                if (ImGui::Button("Stop Basemap Jobs##window")) {
                    basemap_download_stop_requested.store(true, std::memory_order_relaxed);
                    basemap_download_queue.clear();
                    data_library_status_msg = "Stop requested for basemap downloads";
                }
                if (!progress_line.empty()) ImGui::TextWrapped("%s", progress_line.c_str());
            }
            ImGui::End();
        }

        auto draw_category = [&](LayerDef::Category cat, const char* label) {
            if (!ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen)) return;
            std::string show_id = std::string("Show ") + label;
            std::string hide_id = std::string("Hide ") + label;
            if (ImGui::Button(show_id.c_str())) {
                for (auto& l : layers) if (l.category == cat) l.enabled = true;
            }
            ImGui::SameLine();
            if (ImGui::Button(hide_id.c_str())) {
                for (auto& l : layers) if (l.category == cat) l.enabled = false;
            }
            std::string current_subcategory;
            for (auto& l : layers) {
                size_t idx = (size_t)(&l - &layers[0]);
                if (l.category != cat) continue;
                if (l.subcategory != current_subcategory) {
                    current_subcategory = l.subcategory;
                    if (!current_subcategory.empty()) ImGui::SeparatorText(current_subcategory.c_str());
                }
                ImGui::PushID((int)idx);
                const fs::path local_layer_path = root / "data" / "layers" / l.file;
                const bool local_layer_exists = idx < local_layer_exists_cache.size() ? local_layer_exists_cache[idx] : false;
                if (!local_layer_exists) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.22f, 0.12f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.30f, 0.14f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.46f, 0.18f, 0.08f, 1.0f));
                    bool can_download = !l.source_url.empty();
                    if (ImGui::SmallButton("D")) {
                        if (can_download) {
                            VersionedDownloadResult vd = downloadUrlVersioned(
                                l.source_url,
                                local_layer_path,
                                root / "data" / "versions");
                            if (vd.ok) {
                                data_library_status_msg = (vd.not_modified ? "Checked " : "Downloaded/updated ") + l.file + " (" + vd.message + ")";
                                data_freshness_state[idx] = FreshnessState::UpToDate;
                                data_freshness_msg[idx] = vd.message;
                                mark_local_layer_exists(idx, true);
                                enqueue_hydration(idx, true);
                            } else {
                                data_library_status_msg = "Download failed for " + l.file + ": " + vd.message;
                                data_freshness_state[idx] = FreshnessState::Error;
                                data_freshness_msg[idx] = vd.message;
                            }
                        } else {
                            data_library_status_msg = "No source URL for " + l.file;
                        }
                    }
                    ImGui::PopStyleColor(3);
                    if (ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        ImGui::TextUnformatted("Download missing dataset");
                        ImGui::TextDisabled("%s", can_download ? "Source available" : "No source URL in manifest");
                        ImGui::EndTooltip();
                    }
                    ImGui::SameLine();
                }
                auto icon_toggle = [&](const char* id, const char* icon, bool& value, const char* tip) -> bool {
                    ImGui::PushID(id);
                    if (value) {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.45f, 0.78f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.55f, 0.92f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.12f, 0.34f, 0.62f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                    } else {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.16f, 0.16f, 0.22f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.32f, 0.32f, 0.32f, 0.55f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.42f, 0.42f, 0.42f, 0.75f));
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.55f, 1.0f));
                    }
                    bool changed = false;
                    if (ImGui::SmallButton(icon)) {
                        value = !value;
                        changed = true;
                    }
                    ImGui::PopStyleColor(4);
                    if (ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        ImGui::TextUnformatted(tip);
                        ImGui::TextDisabled("%s", value ? "Enabled" : "Disabled");
                        ImGui::EndTooltip();
                    }
                    ImGui::PopID();
                    ImGui::SameLine();
                    return changed;
                };

                icon_toggle("show", "V", l.enabled, "Show layer");
                if (ImGui::SmallButton("⚙")) ImGui::OpenPopup("layer_display_settings");
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted("Layer display settings");
                    ImGui::EndTooltip();
                }
                LayerSettingsPopupContext layer_settings_ctx;
                layer_settings_ctx.root = root;
                layer_settings_ctx.local_layer_path = local_layer_path;
                layer_settings_ctx.idx = idx;
                layer_settings_ctx.layer = &l;
                layer_settings_ctx.local_layer_exists = local_layer_exists;
                layer_settings_ctx.zoom = zoom;
                layer_settings_ctx.data_library_status_msg = &data_library_status_msg;
                layer_settings_ctx.data_freshness_state = &data_freshness_state;
                layer_settings_ctx.data_freshness_msg = &data_freshness_msg;
                layer_settings_ctx.mark_local_layer_exists = [&](size_t i, bool exists) { mark_local_layer_exists(i, exists); };
                layer_settings_ctx.enqueue_hydration = [&](size_t i, bool required) { enqueue_hydration(i, required); };
                layer_settings_ctx.layer_spatial = &layer_spatial;
                layer_settings_ctx.layer_states = &layer_states;
                layer_settings_ctx.status_mutex = &status_mutex;
                layer_settings_ctx.layer_fill_enabled = &layer_fill_enabled;
                layer_settings_ctx.layer_hover_enabled = &layer_hover_enabled;
                layer_settings_ctx.layer_inspect_enabled = &layer_inspect_enabled;
                layer_settings_ctx.layer_heatmap_enabled = &layer_heatmap_enabled;
                layer_settings_ctx.layer_heatmap_algo = &layer_heatmap_algo;
                layer_settings_ctx.layer_heatmap_max_zoom = &layer_heatmap_max_zoom;
                layer_settings_ctx.layer_parcel_detail_min_zoom = &layer_parcel_detail_min_zoom;
                layer_settings_ctx.layer_normalize_mode = &layer_normalize_mode;
                layer_settings_ctx.layer_heatmap_cell_px = &layer_heatmap_cell_px;
                layer_settings_ctx.layer_heatmap_bandwidth_px = &layer_heatmap_bandwidth_px;
                layer_settings_ctx.layer_heatmap_blur_sigma_px = &layer_heatmap_blur_sigma_px;
                layer_settings_ctx.layer_heatmap_percentile_clip = &layer_heatmap_percentile_clip;
                layer_settings_ctx.layer_heatmap_multires_blend = &layer_heatmap_multires_blend;
                layer_settings_ctx.layer_heatmap_zoom_adaptive_bandwidth = &layer_heatmap_zoom_adaptive_bandwidth;
                layer_settings_ctx.layer_heatmap_multires_enabled = &layer_heatmap_multires_enabled;
                layer_settings_ctx.layer_heatmap_use_gradient = &layer_heatmap_use_gradient;
                layer_settings_ctx.heatmap_algo = heatmap_algo;
                layer_settings_ctx.layer_fill_mutex = &layer_fill_mutex;
                layer_settings_ctx.layer_fill_state_changed = &layer_fill_state_changed;
                layer_settings_ctx.layer_hover_state_changed = &layer_hover_state_changed;
                layer_settings_ctx.layer_inspect_state_changed = &layer_inspect_state_changed;
                layer_settings_ctx.layer_heatmap_state_changed = &layer_heatmap_state_changed;
                layer_settings_ctx.heatmap_controls_active = &heatmap_controls_active;
                layer_settings_ctx.heatmap_input_float_enter =
                    [&](const char* label, float& value, float min_value, float max_value, const char* format) {
                        return heatmap_input_float_enter(label, value, min_value, max_value, format);
                    };
                drawLayerDisplaySettingsPopup(layer_settings_ctx);
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, l.color);
                ImGui::TextUnformatted(l.name.c_str());
                ImGui::PopStyleColor();
                const bool row_hovered = ImGui::IsItemHovered();
                ImGui::SameLine();
                LayerRuntimeState st;
                {
                    std::lock_guard<std::mutex> lk(status_mutex);
                    if (idx < layer_states.size()) st = layer_states[idx];
                }
                if (st.status == LayerPipelineStatus::Failed) {
                    ImGui::TextColored(ImVec4(0.85f, 0.35f, 0.2f, 1.0f), "[%s]", statusToString(st.status));
                } else {
                    ImGui::TextDisabled("[%s | %zu]", statusToString(st.status), st.feature_count);
                }
                const bool status_hovered = ImGui::IsItemHovered();
                if (row_hovered || status_hovered) {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted(l.name.c_str());
                    ImGui::Separator();
                    ImGui::Text("Category: %s", categoryToString(l.category));
                    ImGui::Text("Status: %s", statusToString(st.status));
                    ImGui::Text("Features: %zu", st.feature_count);
                    ImGui::Text("File: %s", l.file.c_str());
                    ImGui::Text("Local: %s", local_layer_exists ? "yes" : "no");
                    if (!l.subcategory.empty()) ImGui::Text("Subcategory: %s", l.subcategory.c_str());
                    if (!l.scale.empty()) ImGui::Text("Scale: %s", l.scale.c_str());
                    if (!l.heatmap_field.empty()) ImGui::Text("Heatmap Field: %s", l.heatmap_field.c_str());
                    if (!l.description.empty()) ImGui::TextWrapped("Description: %s", l.description.c_str());
                    if (!l.source_url.empty()) ImGui::TextWrapped("Source: %s", l.source_url.c_str());
                    if (!st.error.empty()) {
                        ImGui::Separator();
                        ImGui::TextColored(ImVec4(0.85f, 0.35f, 0.2f, 1.0f), "Error: %s", st.error.c_str());
                    }
                    ImGui::EndTooltip();
                }
                ImGui::PopID();
            }
            if (cat == LayerDef::Category::Safety) {
                ImGui::SeparatorText("Crime Filters");
                ImGui::Checkbox("Enable Crime Filter", &crime_filter_enabled);
                ImGui::Checkbox("Filter Crime Year", &crime_filter_use_year);
                ImGui::BeginDisabled(!crime_filter_use_year);
                ImGui::SliderInt("Crime Year Min", &crime_year_min, 1900, 2100);
                ImGui::SliderInt("Crime Year Max", &crime_year_max, 1900, 2100);
                if (crime_year_min > crime_year_max) std::swap(crime_year_min, crime_year_max);
                ImGui::EndDisabled();
                ImGui::Checkbox("Homicide", &crime_filter_homicide); ImGui::SameLine();
                ImGui::Checkbox("Robbery", &crime_filter_robbery);
                ImGui::Checkbox("Assault", &crime_filter_assault); ImGui::SameLine();
                ImGui::Checkbox("Burglary", &crime_filter_burglary);
                ImGui::Checkbox("Theft/Larceny", &crime_filter_theft); ImGui::SameLine();
                ImGui::Checkbox("Auto Theft", &crime_filter_auto_theft);
                ImGui::Checkbox("Drug/Narcotic", &crime_filter_drug); ImGui::SameLine();
                ImGui::Checkbox("Shooting", &crime_filter_shooting);
                if (ImGui::Button("Clear Crime Filters")) {
                    crime_filter_homicide = false;
                    crime_filter_robbery = false;
                    crime_filter_assault = false;
                    crime_filter_burglary = false;
                    crime_filter_theft = false;
                    crime_filter_auto_theft = false;
                    crime_filter_drug = false;
                    crime_filter_shooting = false;
                    crime_filter_use_year = false;
                    crime_filter_enabled = false;
                }
                ImGui::SameLine();
                if (ImGui::Button("Refresh Crime Breakdown")) {
                    std::unordered_map<std::string, int> counts;
                    auto add_layer_counts = [&](int idx) {
                        if (idx < 0 || (size_t)idx >= layers.size()) return;
                        for (const auto& fg : layers[(size_t)idx].features) {
                            const std::string desc = toLowerAscii(getPropertyValue(fg, "Description"));
                            const std::string code = toLowerAscii(getPropertyValue(fg, "CrimeCode"));
                            const std::string dt = getPropertyValue(fg, "CrimeDateTime");
                            if (crime_filter_enabled) {
                                if (crime_filter_use_year) {
                                    int yr = extractYearMaybe(dt);
                                    if (yr < 0 || yr < crime_year_min || yr > crime_year_max) continue;
                                }
                            const bool any_type =
                                crime_filter_homicide || crime_filter_robbery || crime_filter_assault ||
                                crime_filter_burglary || crime_filter_theft || crime_filter_auto_theft || crime_filter_drug || crime_filter_shooting;
                                if (any_type) {
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
                                    if (!ok) continue;
                                }
                            }
                            std::string label = trimDisplayValue(getPropertyValue(fg, "Description"));
                            if (label.empty()) label = trimDisplayValue(getPropertyValue(fg, "CrimeCode"));
                            if (label.empty()) label = "(unknown)";
                            counts[label] += 1;
                        }
                    };
                    add_layer_counts(crime_nibrs_layer_idx);
                    add_layer_counts(crime_legacy_layer_idx);
                    crime_breakdown.clear();
                    crime_breakdown.reserve(counts.size());
                    for (auto& kv : counts) crime_breakdown.push_back(kv);
                    std::sort(crime_breakdown.begin(), crime_breakdown.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
                }
                ImGui::Text("Breakdown Rows: %zu", crime_breakdown.size());
            }
        };

        draw_category(LayerDef::Category::Housing, "Housing");
        draw_category(LayerDef::Category::PublicHealth, "Public Health");
        draw_category(LayerDef::Category::Safety, "Safety");
        draw_category(LayerDef::Category::Infrastructure, "Infrastructure");
        draw_category(LayerDef::Category::Zoning, "Zoning");
        bool zoning_filters_changed = false;
        if (zoning_layer_idx >= 0 && ImGui::CollapsingHeader("Zoning Filters", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::Button("Show All Zones")) {
                for (auto& kv : zoning_zone_enabled) {
                    if (!kv.second) zoning_filters_changed = true;
                    kv.second = true;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Hide All Zones")) {
                for (auto& kv : zoning_zone_enabled) {
                    if (kv.second) zoning_filters_changed = true;
                    kv.second = false;
                }
            }
            if (zoning_zone_order.empty()) {
                ImGui::TextDisabled("Zoning classes will appear after zoning layer hydrates.");
            } else {
                for (const auto& gkey : zoning_group_order) {
                    auto git = zoning_group_zones.find(gkey);
                    if (git == zoning_group_zones.end()) continue;
                    const auto& zones = git->second;
                    size_t enabled_count = 0;
                    for (const auto& z : zones) if (zoning_zone_enabled[z]) enabled_count++;
                    bool group_enabled = enabled_count == zones.size() && !zones.empty();
                    bool group_partial = enabled_count > 0 && enabled_count < zones.size();

                    if (group_partial) ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(1.0f, 0.75f, 0.1f, 1.0f));
                    std::string gcb = gkey + "##group_enabled";
                    bool group_next = group_enabled;
                    if (ImGui::Checkbox(gcb.c_str(), &group_next)) {
                        for (const auto& z : zones) {
                            if (zoning_zone_enabled[z] != group_next) zoning_filters_changed = true;
                            zoning_zone_enabled[z] = group_next;
                        }
                    }
                    if (group_partial) ImGui::PopStyleColor();
                    ImGui::SameLine();
                    std::string gheader = gkey + " Zones (" + std::to_string(enabled_count) + "/" + std::to_string(zones.size()) + ")";
                    if (ImGui::TreeNodeEx(gheader.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                        for (const auto& zkey : zones) {
                            ImVec4 zc = zoning_zone_color[zkey];
                            ImGui::ColorButton((std::string("##zclr_") + zkey).c_str(), zc, ImGuiColorEditFlags_NoTooltip, ImVec2(12, 12));
                            ImGui::SameLine();
                            bool enabled = zoning_zone_enabled[zkey];
	                            std::string display = zkey;
	                            auto lit = zoning_zone_label.find(zkey);
	                            if (lit != zoning_zone_label.end() && !lit->second.empty() && lit->second != zkey) {
	                                display += " - " + lit->second;
	                            }
	                            std::string label = display + " (" + std::to_string(zoning_zone_counts[zkey]) + ")";
	                            if (ImGui::Checkbox(label.c_str(), &enabled)) {
	                                zoning_zone_enabled[zkey] = enabled;
	                                zoning_filters_changed = true;
	                            }
	                            if (ImGui::IsItemHovered()) {
	                                ImGui::SetNextWindowSize(ImVec2(460.0f, 0.0f), ImGuiCond_Always);
	                                ImGui::BeginTooltip();
	                                ImGui::PushTextWrapPos(440.0f);
	                                ImGui::TextUnformatted(zkey.c_str());
	                                auto mit = zoning_metadata.find(zkey);
	                                if (mit != zoning_metadata.end()) {
	                                    if (!mit->second.label.empty()) ImGui::TextWrapped("%s", mit->second.label.c_str());
	                                    if (!mit->second.description.empty()) {
	                                        ImGui::Separator();
	                                        ImGui::TextWrapped("%s", mit->second.description.c_str());
	                                    }
	                                }
	                                ImGui::PopTextWrapPos();
	                                ImGui::EndTooltip();
	                            }
	                        }
                        ImGui::TreePop();
                    }
                }
            }
        }

        const size_t hydrated_now = hydrated_count.load(std::memory_order_relaxed);
        const size_t triangulated_now = triangulated_count.load(std::memory_order_relaxed);
        if (hydrated_now > last_hydrated_seen) {
            last_hydrated_seen = hydrated_now;
            last_hydration_progress_at = std::chrono::steady_clock::now();
        }
        if (triangulated_now > last_triangulated_seen) {
            last_triangulated_seen = triangulated_now;
            last_tri_progress_at = std::chrono::steady_clock::now();
        }

        size_t hydrated_pending = 0;
        {
            std::lock_guard<std::mutex> lk(hydrated_mutex);
            hydrated_pending = hydrated_queue.size();
        }
        size_t tri_pending = 0;
        {
            std::lock_guard<std::mutex> lk(tri_mutex);
            tri_pending = tri_jobs.size();
        }

        const float hydrated_frac = layers.empty() ? 1.0f : (float)hydrated_now / (float)layers.size();
        const float tri_frac = layers.empty() ? 1.0f : (float)triangulated_now / (float)layers.size();
        const auto now = std::chrono::steady_clock::now();
        const double elapsed_s = std::chrono::duration<double>(now - hydration_started_at).count();
        const double hydrate_idle_s = std::chrono::duration<double>(now - last_hydration_progress_at).count();
        const double tri_idle_s = std::chrono::duration<double>(now - last_tri_progress_at).count();

        if (show_sources_panel) {
            ImGui::SetNextWindowSize(ImVec2(540, 420), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Gear Panel", &show_sources_panel, ImGuiWindowFlags_NoCollapse)) {
                if (ImGui::BeginTabBar("gear_tabs")) {
                    if (ImGui::BeginTabItem("Sources")) {
                        std::vector<std::string> past_work;
                        std::vector<std::string> future_work;
                        std::vector<std::string> skipped_layer_files;
                        std::string todo_text = readTextFile(root / "TODO.md");
                        collectTodoWork(todo_text, past_work, future_work);
                        {
                            std::lock_guard<std::mutex> lk(bootstrap.msg_mutex);
                            skipped_layer_files = bootstrap.skipped_layer_files;
                        }
                        size_t skipped_layers = bootstrap.skipped_layers.load(std::memory_order_relaxed);
                        size_t skipped_tiles = bootstrap.skipped_tiles.load(std::memory_order_relaxed);
                        ImGui::Text("Past: %zu", past_work.size());
                        ImGui::SameLine();
                        ImGui::Text("Future: %zu", future_work.size());
                        ImGui::SameLine();
                        ImGui::Text("Skipped: %zuL/%zuT", skipped_layers, skipped_tiles);
                        ImGui::Separator();
                        if (ImGui::BeginTabBar("work_tabs")) {
                            if (ImGui::BeginTabItem("Past Work")) {
                                ImGui::BeginChild("past_work_scroll", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
                                if (past_work.empty()) ImGui::TextDisabled("No completed checklist items found in TODO.md");
                                else for (const auto& item : past_work) ImGui::BulletText("%s", item.c_str());
                                ImGui::EndChild();
                                ImGui::EndTabItem();
                            }
                            if (ImGui::BeginTabItem("Future Work")) {
                                ImGui::BeginChild("future_work_scroll", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
                                if (future_work.empty()) ImGui::TextDisabled("No pending items found in TODO.md");
                                else for (const auto& item : future_work) ImGui::BulletText("%s", item.c_str());
                                ImGui::EndChild();
                                ImGui::EndTabItem();
                            }
                            if (ImGui::BeginTabItem("Skipped This Run")) {
                                ImGui::BeginChild("skipped_work_scroll", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
                                ImGui::Text("Layers already present (not downloaded this run): %zu", skipped_layers);
                                ImGui::Text("Tiles already present (not downloaded this run): %zu", skipped_tiles);
                                ImGui::Separator();
                                if (skipped_layer_files.empty()) ImGui::TextDisabled("No skipped layers recorded for this run.");
                                else {
                                    ImGui::TextUnformatted("Skipped layer files:");
                                    for (const auto& f : skipped_layer_files) ImGui::BulletText("%s", f.c_str());
                                }
                                ImGui::EndChild();
                                ImGui::EndTabItem();
                            }
                            ImGui::EndTabBar();
                        }
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Heatmap")) {
                        ImGui::TextDisabled("Heatmap controls are per-layer only.");
                        ImGui::TextWrapped("Open a layer's settings (⚙) to edit aggregate method, bandwidth, blur, clip, adaptive bandwidth, and multi-res settings.");
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
            }
            ImGui::End();
        }
        if (show_data_library) {
            ImGui::SetNextWindowSize(ImVec2(860, 560), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Data Library", &show_data_library, ImGuiWindowFlags_NoCollapse)) {
                auto draw_library_cell = [](const std::string& value, size_t max_chars = 64) {
                    const std::string full = value.empty() ? std::string("-") : value;
                    std::string shown = full;
                    if (shown.size() > max_chars && max_chars > 3) shown = shown.substr(0, max_chars - 3) + "...";
                    ImGui::TextUnformatted(shown.c_str());
                    if (shown.size() != full.size() && ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 80.0f);
                        ImGui::TextUnformatted(full.c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::EndTooltip();
                    }
                };
                auto draw_library_tooltip = [](const LayerDef& l) {
                    if (!ImGui::IsItemHovered()) return;
                    if (l.description.empty() && l.source_url.empty()) return;
                    ImGui::BeginTooltip();
                    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 90.0f);
                    if (!l.description.empty()) ImGui::TextUnformatted(l.description.c_str());
                    if (!l.description.empty() && !l.source_url.empty()) ImGui::Separator();
                    if (!l.source_url.empty()) ImGui::TextUnformatted(l.source_url.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                };
                auto freshness_label = [](FreshnessState s) -> const char* {
                    switch (s) {
                        case FreshnessState::UpToDate: return "Up-to-date";
                        case FreshnessState::UpdateAvailable: return "Update available";
                        case FreshnessState::NotTrackable: return "Not trackable";
                        case FreshnessState::Error: return "Check error";
                        case FreshnessState::Unknown: default: return "Unknown";
                    }
                };
                auto freshness_color = [](FreshnessState s) -> ImVec4 {
                    switch (s) {
                        case FreshnessState::UpToDate: return ImVec4(0.20f, 0.62f, 0.25f, 1.0f);
                        case FreshnessState::UpdateAvailable: return ImVec4(0.80f, 0.50f, 0.10f, 1.0f);
                        case FreshnessState::NotTrackable: return ImVec4(0.45f, 0.45f, 0.45f, 1.0f);
                        case FreshnessState::Error: return ImVec4(0.75f, 0.22f, 0.16f, 1.0f);
                        case FreshnessState::Unknown: default: return ImVec4(0.30f, 0.45f, 0.72f, 1.0f);
                    }
                };
                const char* download_phases[] = {"must-have", "nice-to-have", "heavy-data", "all", "capital-flows"};
                if (data_library_bulk_inflight && data_library_bulk_future.valid() &&
                    data_library_bulk_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                    LayerDownloadSummary summary = data_library_bulk_future.get();
                    data_library_bulk_inflight = false;
                    refresh_local_layer_exists_cache();
                    data_library_status_msg = "Downloaded phase: " + std::to_string(summary.downloaded) +
                        " fetched/checked, " + std::to_string(summary.skipped) +
                        " skipped, " + std::to_string(summary.failed) + " failed";
                    {
                        std::lock_guard<std::mutex> lk(data_library_bulk_mutex);
                        data_library_bulk_progress.clear();
                    }
                    for (size_t i = 0; i < layers.size(); ++i) {
                        if (i < local_layer_exists_cache.size() && local_layer_exists_cache[i]) {
                            data_freshness_state[i] = FreshnessState::UpToDate;
                            if (data_freshness_msg[i].empty() || data_freshness_msg[i] == "not downloaded") {
                                data_freshness_msg[i] = "downloaded";
                            }
                            enqueue_hydration(i, true);
                        }
                    }
                }
                ImGui::InputTextWithHint("##data_library_query", "Search by name, file, category, subcategory...", data_library_query, sizeof(data_library_query));
                ImGui::SameLine();
                if (ImGui::Button("Clear")) data_library_query[0] = '\0';
                ImGui::SameLine();
                if (ImGui::Button("Check All Updates")) {
                    size_t checked = 0;
                    size_t updates = 0;
                    for (size_t i = 0; i < layers.size(); ++i) {
                        const auto& l = layers[i];
                        const bool local_exists = i < local_layer_exists_cache.size() ? local_layer_exists_cache[i] : false;
                        const fs::path local_path = root / "data" / "layers" / l.file;
                        if (!local_exists || l.source_url.empty()) {
                            data_freshness_state[i] = l.source_url.empty() ? FreshnessState::NotTrackable : FreshnessState::Unknown;
                            data_freshness_msg[i] = l.source_url.empty() ? "no source URL" : "not downloaded";
                            continue;
                        }
                        FreshnessCheckResult cr = checkUrlFreshnessVersioned(l.source_url, local_path, root / "data" / "versions");
                        data_freshness_state[i] = cr.state;
                        data_freshness_msg[i] = cr.message;
                        checked++;
                        if (cr.state == FreshnessState::UpdateAvailable) updates++;
                    }
                    data_library_status_msg = "Checked " + std::to_string(checked) + " datasets; updates available: " + std::to_string(updates);
                }
                ImGui::Separator();
                ImGui::SetNextItemWidth(150.0f);
                ImGui::Combo("Download phase", &data_library_download_phase, download_phases, IM_ARRAYSIZE(download_phases));
                ImGui::SameLine();
                ImGui::Checkbox("Include large", &data_library_include_large);
                ImGui::SameLine();
                if (ImGui::Button("Rescan Local Files")) {
                    refresh_local_layer_exists_cache();
                    data_library_status_msg = "Rescanned local layer files";
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(data_library_bulk_inflight);
                if (ImGui::Button("Download Phase")) {
                    const std::string phase = download_phases[data_library_download_phase];
                    const bool include_large = data_library_include_large;
                    {
                        std::lock_guard<std::mutex> lk(data_library_bulk_mutex);
                        data_library_bulk_progress = "Starting " + phase + " download";
                    }
                    data_library_status_msg = "Downloading " + phase + " in background";
                    data_library_bulk_inflight = true;
                    data_library_bulk_future = std::async(std::launch::async, [root, phase, include_large, &data_library_bulk_mutex, &data_library_bulk_progress]() {
                        setCurrentThreadName("ws3-data-fetch");
                        return downloadLayerManifestPhase(
                            root,
                            phase,
                            include_large,
                            [&data_library_bulk_mutex, &data_library_bulk_progress](size_t i, size_t total, const std::string& msg) {
                                std::lock_guard<std::mutex> lk(data_library_bulk_mutex);
                                data_library_bulk_progress = "[" + std::to_string(i) + "/" + std::to_string(total) + "] " + msg;
                            });
                    });
                }
                ImGui::EndDisabled();
                if (data_library_bulk_inflight) {
                    std::string progress;
