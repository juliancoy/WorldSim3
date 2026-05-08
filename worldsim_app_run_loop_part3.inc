            const std::string deed_page = firstDisplayProperty(*rp, {"DEEDPAGE"});
            text_prop("Deed", deed_book.empty() ? "" : deed_book + (deed_page.empty() ? "" : " / " + deed_page));
            text_prop("SDAT Link", firstDisplayProperty(*rp, {"SDATLINK"}));
            ImGui::TextDisabled("Source: Baltimore Real Property Information (data/layers/real_property_information.geojson)");
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
            for (const auto& pf : parcels) {
                const LayerDef::FeatureGeom* rp = real_property_for_parcel(pf);
                std::string owner = owner_name_for(rp);
                if (owner.empty()) owner = owner_name_for(&pf);
                if (owner.empty()) continue;
                auto& row = acc[owner];
                if (row.owner.empty()) row.owner = owner;
                row.property_count += 1;
                row.area_m2 += parcel_area_sq_m(pf);
                row.value_usd += owner_value_for(rp);
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
                    const auto& pfeats = layers[(size_t)parcel_layer_idx].features;
                    for (size_t i = 0; i < pfeats.size(); ++i) {
                        const LayerDef::FeatureGeom* rp = real_property_for_parcel(pfeats[i]);
                        std::string owner = owner_name_for(rp);
                        if (owner.empty()) owner = owner_name_for(&pfeats[i]);
                        if (owner.empty() || selected_owners.find(owner) == selected_owners.end()) continue;
                        const int vac_notice = (i < parcel_vac_notice_by_feature.size()) ? parcel_vac_notice_by_feature[i] : 0;
                        const int vac_rehab = (i < parcel_vac_rehab_by_feature.size()) ? parcel_vac_rehab_by_feature[i] : 0;
                        if ((vac_notice + vac_rehab) <= 0) continue;
                        filtered_aggregate_snapshot.vacancy_parcels_matched++;
                        if (!pfeats[i].rings.empty()) filtered_aggregate_snapshot.vacancy_parcels_with_geometry++;
                    }
                }
            }
        } else {
            filtered_aggregate_snapshot = {};
        }

        const float right_panel_w = 360.0f;
        ImGui::SetNextWindowPos(ImVec2((float)w - right_panel_w - 12.0f, 12.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(right_panel_w, (float)h - 24.0f), ImGuiCond_Always);
        ImGui::Begin("Record Filters", nullptr, ImGuiWindowFlags_NoCollapse);
        if (ImGui::BeginTabBar("right_tabs")) {
        if (ImGui::BeginTabItem("Filters")) {
        const bool selected_valid =
            show_selected_parcel_details && parcel_layer_idx >= 0 &&
            (size_t)parcel_layer_idx < layers.size() &&
            selected_parcel_idx < layers[(size_t)parcel_layer_idx].features.size();
        const bool selected_zone_valid =
            show_selected_zone_details && zoning_layer_idx >= 0 &&
            (size_t)zoning_layer_idx < layers.size() &&
            selected_zone_idx < layers[(size_t)zoning_layer_idx].features.size();
        if (selected_valid) {
            const auto& selected = layers[(size_t)parcel_layer_idx].features[selected_parcel_idx];
            if (ImGui::Button("Back To Filters")) {
                show_selected_parcel_details = false;
                selected_parcel_idx = (size_t)-1;
            }
            ImGui::Separator();
            std::string blocklot_raw = getPropertyValue(selected, "BLOCKLOT");
            std::string blocklot = normalizeJoinKey(blocklot_raw);
            int vac_notice = (selected_parcel_idx < parcel_vac_notice_by_feature.size()) ? parcel_vac_notice_by_feature[selected_parcel_idx] : 0;
            int vac_rehab = (selected_parcel_idx < parcel_vac_rehab_by_feature.size()) ? parcel_vac_rehab_by_feature[selected_parcel_idx] : 0;
            int tax_lien = (selected_parcel_idx < parcel_tax_lien_by_feature.size()) ? parcel_tax_lien_by_feature[selected_parcel_idx] : 0;
            int tax_sale = (selected_parcel_idx < parcel_tax_sale_by_feature.size()) ? parcel_tax_sale_by_feature[selected_parcel_idx] : 0;
            double tax_lien_amount = (selected_parcel_idx < parcel_tax_lien_amount_by_feature.size()) ? parcel_tax_lien_amount_by_feature[selected_parcel_idx] : 0.0;
            double tax_sale_amount = (selected_parcel_idx < parcel_tax_sale_amount_by_feature.size()) ? parcel_tax_sale_amount_by_feature[selected_parcel_idx] : 0.0;
            ImGui::TextUnformatted("Parcel Details");
            ImGui::Separator();
            const LayerDef::FeatureGeom* selected_rp = real_property_for_parcel(selected);
            const std::vector<ParcelTimelineEvent> parcel_events = buildParcelTimeline(ParcelTimelineRequest{
                &layers,
                &selected,
                selected_rp,
                vacant_notice_layer_idx,
                vacant_rehab_layer_idx,
                tax_lien_layer_idx,
                tax_sale_layer_idx
            });
            ImGui::BeginChild("selected_parcel_fields", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
            if (ImGui::CollapsingHeader("Summary", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Text("BLOCKLOT: %s", blocklot_raw.empty() ? "(none)" : blocklot_raw.c_str());
                ImGui::Text("Vacant Notices: %d", vac_notice);
                ImGui::Text("Vacant Rehab Records: %d", vac_rehab);
                ImGui::Text("Tax Lien Certificate Records: %d", tax_lien);
                if (tax_lien > 0) ImGui::Text("Tax Lien Total Amount: $%.2f", tax_lien_amount);
                ImGui::Text("Tax Sale 2021 Records: %d", tax_sale);
                if (tax_sale > 0) ImGui::Text("Tax Sale Total Lien: $%.2f", tax_sale_amount);
                draw_real_property_summary(selected_rp);
            }
            if (ImGui::CollapsingHeader("Event Timeline", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (parcel_events.empty()) {
                    ImGui::TextDisabled("No related parcel events found.");
                } else {
                    for (const auto& ev : parcel_events) {
                        ImGui::Separator();
                        if (!ev.date.empty()) {
                            ImGui::TextColored(ImVec4(1.0f, 0.86f, 0.36f, 1.0f), "Date: %s", ev.date.c_str());
                        } else {
                            ImGui::TextColored(ImVec4(0.82f, 0.84f, 0.88f, 1.0f), "Date unavailable");
                        }
                        ImGui::TextUnformatted(ev.event_type.c_str());
                        if (!ev.status.empty()) ImGui::TextWrapped("Status: %s", ev.status.c_str());
                        if (!ev.amount.empty()) ImGui::TextWrapped("Amount: %s", ev.amount.c_str());
                        ImGui::TextDisabled("Source: %s", ev.source_layer.c_str());
                    }
                }
            }
            if (ImGui::CollapsingHeader("Parcel Geometry Fields")) {
                draw_feature_properties("All Parcel Geometry Fields", selected);
            }
            if (selected_rp && ImGui::CollapsingHeader("Real Property Fields")) {
                draw_feature_properties("All Real Property Fields", *selected_rp);
            }
            if (ImGui::CollapsingHeader("Quick Stats")) {
                ImGui::Text("Total Events: %d", (int)parcel_events.size());
                int with_amount = 0;
                for (const auto& ev : parcel_events) {
                    if (!ev.amount.empty()) with_amount++;
                }
                ImGui::Text("Events With Amounts: %d", with_amount);
            }
            ImGui::EndChild();
        } else if (selected_zone_valid) {
            const auto& selected = layers[(size_t)zoning_layer_idx].features[selected_zone_idx];
            if (ImGui::Button("Back To Filters")) {
                show_selected_zone_details = false;
                selected_zone_idx = (size_t)-1;
            }
            ImGui::Separator();
            std::string zone_key = zoningClassKey(selected);
            std::string zone_label = zoningClassLabel(selected);
            auto meta_it = zoning_metadata.find(zone_key);
            if (meta_it != zoning_metadata.end()) {
                if (!meta_it->second.label.empty()) zone_label = meta_it->second.label;
            }
            std::string zone_description = zoningDescription(selected, zoning_metadata);
            const char* display_zone = !zone_label.empty() ? zone_label.c_str() : (zone_key.empty() ? "(unlabeled)" : zone_key.c_str());
            ImGui::SetWindowFontScale(1.6f);
            ImGui::TextWrapped("%s", display_zone);
            ImGui::SetWindowFontScale(1.0f);
            ImGui::Separator();
            ImGui::TextWrapped("%s", zone_description.empty() ? "No description available." : zone_description.c_str());
        } else {
            if (show_selected_parcel_details && !selected_valid) {
                show_selected_parcel_details = false;
                selected_parcel_idx = (size_t)-1;
            }
            if (show_selected_zone_details && !selected_zone_valid) {
                show_selected_zone_details = false;
                selected_zone_idx = (size_t)-1;
            }
            ImGui::Checkbox("Enable Filters", &filter_enabled);
            ImGui::Checkbox("Filter By Record Date", &filter_use_date);
            ImGui::BeginDisabled(!filter_enabled || !filter_use_date);
            ImGui::SliderInt("Year Min", &filter_year_min, 1900, 2100);
            ImGui::SliderInt("Year Max", &filter_year_max, 1900, 2100);
            if (filter_year_min > filter_year_max) std::swap(filter_year_min, filter_year_max);
            ImGui::EndDisabled();
            ImGui::SeparatorText("Common Fields");
            ImGui::InputText("Block/Lot", filter_blocklot, sizeof(filter_blocklot));
            ImGui::InputText("Status", filter_status, sizeof(filter_status));
            ImGui::InputText("Address", filter_address, sizeof(filter_address));
            ImGui::InputText("Owner", filter_owner, sizeof(filter_owner));
            ImGui::InputText("ZIP", filter_zip, sizeof(filter_zip));
            if (ImGui::Button("Clear Field Filters")) {
                filter_blocklot[0] = '\0';
                filter_status[0] = '\0';
                filter_address[0] = '\0';
                filter_owner[0] = '\0';
                filter_zip[0] = '\0';
                filter_use_date = false;
            }
            ImGui::SeparatorText("Record Year Histogram");
            auto first_prop_hist = [&](const LayerDef::FeatureGeom& fg, std::initializer_list<const char*> keys) {
                for (const char* k : keys) {
                    std::string v = getPropertyValue(fg, k);
                    if (!v.empty()) return v;
                }
                return std::string();
            };
            if (hist_feature_counts.size() != layers.size()) {
                hist_feature_counts.assign(layers.size(), 0);
                hist_enabled.assign(layers.size(), false);
                hist_dirty = true;
            }
            for (size_t i = 0; i < layers.size(); ++i) {
                const size_t fc = layers[i].features.size();
                if (hist_feature_counts[i] != fc || hist_enabled[i] != layers[i].enabled) {
                    hist_feature_counts[i] = fc;
                    hist_enabled[i] = layers[i].enabled;
                    hist_dirty = true;
                }
            }
            if (hist_dirty) {
                std::fill(record_year_hist.begin(), record_year_hist.end(), 0);
                for (size_t li = 0; li < layers.size(); ++li) {
                    if (!layers[li].enabled) continue;
                    for (const auto& fg : layers[li].features) {
                        std::string ds = first_prop_hist(fg, {"RECORD_DATE", "RECORDDATE", "DATE", "CREATED_DATE", "ISSUE_DATE", "DateNotice", "DateIssue", "DateIssued", "DateCancel", "DateAbate"});
                        if (ds.empty()) continue;
                        int y = extractYearMaybe(ds);
                        if (y < 1900 || y > 2100) continue;
                        record_year_hist[(size_t)(y - 1900)]++;
                    }
                }
                float max_bin = 1.0f;
                int nz_min = 2101;
                int nz_max = 1899;
                int nz_total = 0;
                for (size_t i = 0; i < record_year_hist.size(); ++i) {
                    record_year_hist_plot[i] = (float)record_year_hist[i];
                    if (record_year_hist[i] > 0) {
                        int y = 1900 + (int)i;
                        nz_min = std::min(nz_min, y);
                        nz_max = std::max(nz_max, y);
                        nz_total += record_year_hist[i];
                    }
                    if (record_year_hist_plot[i] > max_bin) max_bin = record_year_hist_plot[i];
                }
                record_year_hist_max_bin = max_bin;
                if (nz_min <= nz_max) {
                    record_year_nonzero_min = nz_min;
                    record_year_nonzero_max = nz_max;
                } else {
                    record_year_nonzero_min = 1900;
                    record_year_nonzero_max = 2100;
                }
                record_year_nonzero_total = nz_total;
                selected_record_year_dirty = true;
                hist_dirty = false;
            }
            ImGui::TextDisabled("Enabled-layer records by year");
            if (record_year_nonzero_total <= 0) {
                ImGui::TextDisabled("No recognized date fields found in currently enabled layers.");
            } else {
                ImGui::Text("Range: %d-%d  Total: %d  Peak: %.0f",
                            record_year_nonzero_min,
                            record_year_nonzero_max,
                            record_year_nonzero_total,
                            record_year_hist_max_bin);
                const int plot_offset = std::max(0, record_year_nonzero_min - 1900);
                const int plot_count = std::max(1, record_year_nonzero_max - record_year_nonzero_min + 1);
                ImGui::PlotHistogram(
                    "##record_year_hist",
                    record_year_hist_plot.data() + plot_offset,
                    plot_count,
                    0,
                    nullptr,
                    0.0f,
                    record_year_hist_max_bin * 1.05f,
                    ImVec2(-1.0f, 140.0f));
            }
            ImGui::BeginChild("year_hist_list", ImVec2(0, 130), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
            for (int y = record_year_nonzero_min; y <= record_year_nonzero_max; ++y) {
                int c = record_year_hist[(size_t)(y - 1900)];
                if (c <= 0) continue;
                char label[64];
                std::snprintf(label, sizeof(label), "%d: %d records", y, c);
                if (ImGui::Selectable(label, selected_record_year == y)) {
                    selected_record_year = y;
                    selected_record_year_dirty = true;
                }
            }
            ImGui::EndChild();
            if (selected_record_year >= 1900 && selected_record_year <= 2100) {
                if (record_year_hist[(size_t)(selected_record_year - 1900)] <= 0) {
                    selected_record_year = -1;
                    selected_record_year_samples.clear();
                    selected_record_year_total = 0;
                    selected_record_year_dirty = false;
                }
            }
            if (selected_record_year >= 1900 && selected_record_year <= 2100 && selected_record_year_dirty) {
                constexpr size_t kMaxYearSamples = 8;
                selected_record_year_samples.clear();
                selected_record_year_total = 0;
                for (size_t li = 0; li < layers.size(); ++li) {
                    if (!layers[li].enabled) continue;
                    for (const auto& fg : layers[li].features) {
                        std::string ds = first_prop_hist(fg, {"RECORD_DATE", "RECORDDATE", "DATE", "CREATED_DATE", "ISSUE_DATE", "DateNotice", "DateIssue", "DateIssued", "DateCancel", "DateAbate"});
                        if (ds.empty() || extractYearMaybe(ds) != selected_record_year) continue;
                        selected_record_year_total++;
                        if (selected_record_year_samples.size() >= kMaxYearSamples) continue;
                        std::string blocklot = first_prop_hist(fg, {"BLOCKLOT", "BLOCK_LOT", "LOT"});
                        std::string address = first_prop_hist(fg, {"FULLADDR", "PROPERTY_ADDRESS", "PREMISEADD", "ADDRESS", "Address", "ADDR"});
                        std::string owner = first_prop_hist(fg, {"OWNER_1", "OWNER_2", "OWNER_3", "OWNERNME1", "OWNER", "OWNER_NAME", "OWNER_ABBR", "AR_OWNER"});
                        std::string status = first_prop_hist(fg, {"STATUS", "STATE", "CASE_STATUS"});
                        std::ostringstream row;
                        row << layers[li].name << " | " << ds;
                        if (!blocklot.empty()) row << " | BL " << blocklot;
                        if (!address.empty()) row << " | " << address;
                        if (!owner.empty()) row << " | " << owner;
                        if (!status.empty()) row << " | " << status;
                        selected_record_year_samples.push_back(row.str());
                    }
                }
                selected_record_year_dirty = false;
            }
            if (selected_record_year >= 1900 && selected_record_year <= 2100) {
                ImGui::SeparatorText("Selected Year Records");
                ImGui::Text("%d: showing %zu of %d records",
                            selected_record_year,
                            selected_record_year_samples.size(),
                            selected_record_year_total);
                ImGui::BeginChild("selected_year_records", ImVec2(0, 170), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
                if (selected_record_year_samples.empty()) {
                    ImGui::TextDisabled("No sample records available for this year.");
                } else {
                    for (const std::string& row : selected_record_year_samples) {
                        ImGui::TextWrapped("%s", row.c_str());
                        ImGui::Separator();
                    }
                }
                ImGui::EndChild();
            }
        }
        ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Vacancy-Parcel")) {
            const size_t notices_total = (size_t)cached_vac_notice_size;
            const size_t rehabs_total = (size_t)cached_vac_rehab_size;
            const size_t notices_matched = vacant_notice_rows_matched_total.load(std::memory_order_relaxed);
            const size_t rehabs_matched = vacant_rehab_rows_matched_total.load(std::memory_order_relaxed);
            size_t parcels_matched = vacant_parcels_matched_total.load(std::memory_order_relaxed);
            size_t parcels_geom = vacant_parcels_with_geometry_total.load(std::memory_order_relaxed);
            if (!selected_owners.empty() && filtered_aggregate_snapshot.valid) {
                parcels_matched = filtered_aggregate_snapshot.vacancy_parcels_matched;
                parcels_geom = filtered_aggregate_snapshot.vacancy_parcels_with_geometry;
            }
            ImGui::TextUnformatted("Vacant -> Parcel Join Quality");
            ImGui::Separator();
            ImGui::Text("Vacant notice records: %zu", notices_total);
            ImGui::Text("Matched to parcel: %zu (%.1f%%)", notices_matched, notices_total ? (100.0 * (double)notices_matched / (double)notices_total) : 0.0);
            ImGui::Text("Unmatched notices: %zu", notices_total >= notices_matched ? (notices_total - notices_matched) : 0);
            ImGui::Separator();
            ImGui::Text("Vacant rehab records: %zu", rehabs_total);
            ImGui::Text("Matched to parcel: %zu (%.1f%%)", rehabs_matched, rehabs_total ? (100.0 * (double)rehabs_matched / (double)rehabs_total) : 0.0);
            ImGui::Text("Unmatched rehabs: %zu", rehabs_total >= rehabs_matched ? (rehabs_total - rehabs_matched) : 0);
            ImGui::Separator();
            ImGui::Text("Parcels with vacancy evidence: %zu", parcels_matched);
            ImGui::Text("Those with parcel geometry: %zu", parcels_geom);
            if (!selected_owners.empty()) ImGui::TextDisabled("Counts scoped to selected owners.");
            ImGui::TextWrapped("Map styling uses parcel-level derived status. Raw vacant points are treated as child records.");
        ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Gradient")) {
            ImGui::TextUnformatted("Gradient Scaling");
            ImGui::Separator();
            ImGui::TextWrapped("Numeric layer colors use a blue-to-yellow-to-red gradient. Effective ranges are percentile-clipped so extreme outliers do not flatten normal parcel colors.");
            ImGui::Separator();
            auto compact_money = [](double value) {
                std::ostringstream os;
                if (value >= 1000000000.0) os << "$" << std::fixed << std::setprecision(1) << (value / 1000000000.0) << "B";
                else if (value >= 1000000.0) os << "$" << std::fixed << std::setprecision(1) << (value / 1000000.0) << "M";
                else if (value >= 1000.0) os << "$" << std::fixed << std::setprecision(0) << (value / 1000.0) << "K";
                else os << "$" << std::fixed << std::setprecision(0) << value;
                return os.str();
            };
            auto compact_number = [](double value) {
                std::ostringstream os;
                if (std::abs(value) >= 1000000000.0) os << std::fixed << std::setprecision(1) << (value / 1000000000.0) << "B";
                else if (std::abs(value) >= 1000000.0) os << std::fixed << std::setprecision(1) << (value / 1000000.0) << "M";
                else if (std::abs(value) >= 1000.0) os << std::fixed << std::setprecision(0) << (value / 1000.0) << "K";
                else os << std::fixed << std::setprecision(2) << value;
                return os.str();
            };
            auto format_gradient_value = [&](const std::string& field, double value) {
                std::string lower_field = field;
                std::transform(lower_field.begin(), lower_field.end(), lower_field.begin(), [](unsigned char c) {
                    return (char)std::tolower(c);
                });
                if (lower_field.find("usd") != std::string::npos ||
                    lower_field.find("value") != std::string::npos ||
                    lower_field.find("amount") != std::string::npos ||
                    lower_field.find("cost") != std::string::npos ||
                    lower_field.find("price") != std::string::npos) {
                    return compact_money(value);
                }
                return compact_number(value);
            };
            int gradient_layer_count = 0;
            ImGui::BeginChild("gradient_scale_layers", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
            for (size_t li = 0; li < layers.size(); ++li) {
                const auto& layer = layers[li];
                if (!layer.enabled || layer.heatmap_field.empty()) continue;
                std::vector<float> values;
                values.reserve(layer.features.size());
                for (size_t fi = 0; fi < layer.features.size(); ++fi) {
                    const auto& fg = layer.features[fi];
                    float v = 0.0f;
                    if (tryGetFeaturePropertyFloat(fg, layer.heatmap_field, v)) values.push_back(v);
                }
                if (values.empty()) continue;
                gradient_layer_count++;
                std::sort(values.begin(), values.end());
                const float clip_pct = std::clamp(
                    li < layer_heatmap_percentile_clip.size()
                        ? layer_heatmap_percentile_clip[li]
                        : heatmap_percentile_clip,
                    50.0f,
                    100.0f);
                const size_t max_idx = values.size() - 1;
                const size_t clip_idx = clip_pct < 100.0f
                    ? (size_t)std::clamp((int)std::floor((clip_pct / 100.0f) * (double)max_idx), 0, (int)max_idx)
                    : max_idx;
                const float raw_min = values.front();
                const float raw_max = values.back();
                const float effective_min = raw_min;
                const float effective_max = std::max(effective_min, values[clip_idx]);
                const float median = values[max_idx / 2];
                const bool layer_heatmap_on = li < layer_heatmap_enabled.size() && layer_heatmap_enabled[li];
                const int aggregate_max_zoom = li < layer_heatmap_max_zoom.size() ? layer_heatmap_max_zoom[li] : 13;
                const int detail_min_zoom = li < layer_parcel_detail_min_zoom.size()
                    ? layer_parcel_detail_min_zoom[li]
                    : kParcelChoroplethMinZoom;
                const int aggregate_algo = (li < layer_heatmap_algo.size() && layer_heatmap_algo[li] >= 0)
                    ? layer_heatmap_algo[li]
                    : heatmap_algo;
                const bool parcel_choropleth_now =
                    layer.scale == "parcel" &&
                    !layer.heatmap_field.empty() &&
                    zoom >= detail_min_zoom;
                const bool aggregate_now =
                    layer_heatmap_on &&
                    zoom <= aggregate_max_zoom &&
                    !parcel_choropleth_now &&
                    isHeatmapAggregateMethod(aggregate_algo);
                std::string resolved = "per-feature fill";
                if (parcel_choropleth_now) resolved = "parcel choropleth";
                else if (aggregate_now) resolved = std::string("aggregate ") + aggregateStrategyName(aggregate_algo);

                if (ImGui::CollapsingHeader(layer.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::Text("Field: %s", layer.heatmap_field.c_str());
                    ImGui::Text("Resolved display: %s", resolved.c_str());
                    ImGui::Text("Samples: %zu", values.size());
                    ImGui::Text("Clip: %.0f%%", clip_pct);
                    ImGui::Text("Blue floor: %s", format_gradient_value(layer.heatmap_field, effective_min).c_str());
                    ImGui::Text("Yellow midpoint: %s", format_gradient_value(layer.heatmap_field, (effective_min + effective_max) * 0.5).c_str());
                    ImGui::Text("Red ceiling: %s", format_gradient_value(layer.heatmap_field, effective_max).c_str());
                    ImGui::TextDisabled("Median: %s", format_gradient_value(layer.heatmap_field, median).c_str());
                    ImGui::TextDisabled("Raw max: %s", format_gradient_value(layer.heatmap_field, raw_max).c_str());
                    if (raw_max > effective_max) {
                        ImGui::TextWrapped("Values above the red ceiling are clamped to red. This prevents outliers from making ordinary parcels all blue.");
                    }
                    if (layer.scale == "parcel") {
                        ImGui::TextDisabled("Aggregate max zoom: %d | Parcel detail min zoom: %d", aggregate_max_zoom, detail_min_zoom);
                    } else {
                        ImGui::TextDisabled("Aggregate max zoom: %d", aggregate_max_zoom);
                    }
                }
            }
            if (gradient_layer_count == 0) {
                ImGui::TextDisabled("No enabled numeric gradient layers are loaded.");
                ImGui::TextWrapped("Enable a layer with a heatmap field, such as Property Value (Parcels), to see its current gradient scale.");
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Owners")) {
            ImGui::TextUnformatted("Owner Rankings");
            ImGui::Separator();
            const char* sort_items[] = {"# Properties", "Area Owned", "Value Owned"};
            ImGui::Combo("Sort By", &owner_sort_mode, sort_items, IM_ARRAYSIZE(sort_items));
            if (owner_sorted_mode != owner_sort_mode) {
                std::stable_sort(owner_aggregates.begin(), owner_aggregates.end(), [&](const OwnerAggregate& a, const OwnerAggregate& b) {
                    auto tie_break = [&]() {
                        if (a.property_count != b.property_count) return a.property_count > b.property_count;
                        if (std::abs(a.area_m2 - b.area_m2) > 0.5) return a.area_m2 > b.area_m2;
                        if (std::abs(a.value_usd - b.value_usd) > 0.5) return a.value_usd > b.value_usd;
                        return a.owner < b.owner;
                    };
                    if (owner_sort_mode == 1 && std::abs(a.area_m2 - b.area_m2) > 0.5) return a.area_m2 > b.area_m2;
                    if (owner_sort_mode == 2 && std::abs(a.value_usd - b.value_usd) > 0.5) return a.value_usd > b.value_usd;
                    if (owner_sort_mode == 0 && a.property_count != b.property_count) return a.property_count > b.property_count;
                    return tie_break();
                });
                owner_sorted_mode = owner_sort_mode;
            }
            ImGui::Text("Owners: %zu", owner_aggregates.size());
            ImGui::Text("Selected: %zu", selected_owners.size());
            ImGui::SameLine();
            if (ImGui::Button("Clear Selection")) {
                selected_owners.clear();
                selected_owner_anchor = -1;
            }
            if (!selected_owners.empty() && filtered_aggregate_snapshot.valid) {
                ImGui::TextDisabled("Filtered totals | properties: %zu | area: %.0f m^2 | value: $%.0f",
                                    filtered_aggregate_snapshot.owner_property_count,
                                    filtered_aggregate_snapshot.owner_area_m2,
                                    filtered_aggregate_snapshot.owner_value_usd);
            }
            if (owner_aggregates.empty()) {
                const size_t parcel_count =
                    (parcel_layer_idx >= 0 && (size_t)parcel_layer_idx < layers.size()) ? layers[(size_t)parcel_layer_idx].features.size() : 0;
                const size_t real_property_count =
                    (real_property_layer_idx >= 0 && (size_t)real_property_layer_idx < layers.size()) ? layers[(size_t)real_property_layer_idx].features.size() : 0;
                ImGui::TextDisabled("Waiting for owner data: parcels=%zu real_property=%zu", parcel_count, real_property_count);
            }
            ImGui::BeginChild("owner_rankings", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
            const size_t max_rows = std::min<size_t>(500, owner_aggregates.size());
            for (size_t i = 0; i < max_rows; ++i) {
                const auto& r = owner_aggregates[i];
                const bool row_selected = selected_owners.find(r.owner) != selected_owners.end();
                ImGui::PushID((int)i);
                if (ImGui::Selectable(r.owner.c_str(), row_selected, ImGuiSelectableFlags_SpanAllColumns)) {
                    const bool shift = ImGui::GetIO().KeyShift;
                    const bool ctrl = ImGui::GetIO().KeyCtrl;
                    if (shift && selected_owner_anchor >= 0 && (size_t)selected_owner_anchor < max_rows) {
                        if (!ctrl) selected_owners.clear();
                        const size_t begin = std::min((size_t)selected_owner_anchor, i);
                        const size_t end = std::max((size_t)selected_owner_anchor, i);
                        for (size_t j = begin; j <= end; ++j) selected_owners.insert(owner_aggregates[j].owner);
                    } else if (ctrl) {
                        if (row_selected) selected_owners.erase(r.owner);
                        else selected_owners.insert(r.owner);
                        selected_owner_anchor = (int)i;
                    } else {
                        selected_owners.clear();
                        selected_owners.insert(r.owner);
                        selected_owner_anchor = (int)i;
                    }
                }
                ImGui::PopID();
                ImGui::TextDisabled("properties: %zu | area: %.0f m^2 | value: $%.0f", r.property_count, r.area_m2, r.value_usd);
                ImGui::Separator();
            }
            ImGui::EndChild();
        ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
        }
        ImGui::End();

        const float map_x = 440.0f;
        const float map_w = std::max(260.0f, (float)w - map_x - right_panel_w - 24.0f);
        ImGui::SetNextWindowPos(ImVec2(map_x, 12), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(map_w, (float)h - 24.0f), ImGuiCond_Always);
        ImGui::Begin("Map", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        if (ImGui::BeginTabBar("main_view_tabs")) {
        if (ImGui::BeginTabItem("Map")) {
        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImVec2 size = ImGui::GetContentRegionAvail();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImGui::InvisibleButton("map_canvas_input", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        const bool map_hovered = ImGui::IsItemHovered();
        const bool map_active = ImGui::IsItemActive();

        draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(242, 246, 250, 255));

        const int math_zoom = std::min(zoom, kMaxInternalMathZoom);
        const double zoom_scale = std::ldexp(1.0, zoom - math_zoom);
        auto wrap_world_x = [&](double x, int mz) -> double {
            const double period = 256.0 * (double)(1u << mz);
            x = std::fmod(x, period);
            if (x < 0.0) x += period;
            return x;
        };
        ImVec2 center_world = lonLatToWorldPx(center_lon, center_lat, math_zoom);
        center_world.x = (float)wrap_world_x((double)center_world.x, math_zoom);

        if (map_hovered) {
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f) {
                int next_zoom = std::clamp(zoom + (wheel > 0 ? 1 : -1), kMinZoom, kMaxZoom);
                if (next_zoom != zoom) {
                    const ImVec2 mouse = ImGui::GetIO().MousePos;
                    ImVec2 mouse_world = ImVec2(
                        center_world.x + (float)((mouse.x - (origin.x + size.x * 0.5f)) / zoom_scale),
                        center_world.y + (float)((mouse.y - (origin.y + size.y * 0.5f)) / zoom_scale));
                    mouse_world.x = (float)wrap_world_x((double)mouse_world.x, math_zoom);
                    ImVec2 ll = worldPxToLonLat(mouse_world, math_zoom);
                    zoom = next_zoom;
                    const int next_math_zoom = std::min(zoom, kMaxInternalMathZoom);
                    const double next_zoom_scale = std::ldexp(1.0, zoom - next_math_zoom);
                    ImVec2 mouse_world_new = lonLatToWorldPx(ll.x, ll.y, next_math_zoom);
                    center_world = ImVec2(
                        mouse_world_new.x - (float)((mouse.x - (origin.x + size.x * 0.5f)) / next_zoom_scale),
                        mouse_world_new.y - (float)((mouse.y - (origin.y + size.y * 0.5f)) / next_zoom_scale));
                    center_world.x = (float)wrap_world_x((double)center_world.x, next_math_zoom);
                    ImVec2 cll = worldPxToLonLat(center_world, next_math_zoom);
                    center_lon = cll.x;
                    center_lat = std::clamp((double)cll.y, -85.0, 85.0);
                }
            }

            if (map_active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
                ImVec2 d = ImGui::GetIO().MouseDelta;
                center_world.x -= (float)(d.x / zoom_scale);
                center_world.y -= (float)(d.y / zoom_scale);
                center_world.x = (float)wrap_world_x((double)center_world.x, math_zoom);
                ImVec2 ll = worldPxToLonLat(center_world, math_zoom);
                center_lon = ll.x;
                center_lat = std::clamp((double)ll.y, -85.0, 85.0);
            }
        }

        center_world = lonLatToWorldPx(center_lon, center_lat, math_zoom);
        center_world.x = (float)wrap_world_x((double)center_world.x, math_zoom);
        const float screen_cx = origin.x + size.x * 0.5f;
        const float screen_cy = origin.y + size.y * 0.5f;
        const float zsf = (float)zoom_scale;
        auto project_world = [&](const ImVec2& wp) -> ImVec2 {
            return ImVec2(
                screen_cx + (wp.x - center_world.x) * zsf,
                screen_cy + (wp.y - center_world.y) * zsf);
        };
        const ImVec2 mouse_screen = ImGui::GetIO().MousePos;
        const ImVec2 mouse_world(
            center_world.x + (float)((mouse_screen.x - (origin.x + size.x * 0.5f)) / zoom_scale),
            center_world.y + (float)((mouse_screen.y - (origin.y + size.y * 0.5f)) / zoom_scale));
        const ImVec2 mouse_ll = worldPxToLonLat(mouse_world, math_zoom);
        static ImVec2 context_ll(0.0f, 0.0f);
        if (map_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            context_ll = mouse_ll;
            ImGui::OpenPopup("map_context_menu");
        }
        if (ImGui::BeginPopup("map_context_menu")) {
            ImGui::Text("Lon: %.6f", context_ll.x);
            ImGui::Text("Lat: %.6f", context_ll.y);
            ImGui::Separator();
            if (ImGui::MenuItem("Open Google Maps Street View")) {
                std::ostringstream url;
                url << std::fixed << std::setprecision(7)
                    << "https://www.google.com/maps/@?api=1&map_action=pano&viewpoint="
                    << context_ll.y << "," << context_ll.x;
                openUrlInBrowser(url.str());
            }
            ImGui::EndPopup();
        }
        const double half_w_world = (size.x * 0.5) / zoom_scale;
        const double half_h_world = (size.y * 0.5) / zoom_scale;
        ImVec2 ll_a = worldPxToLonLat(ImVec2(center_world.x - (float)half_w_world, center_world.y - (float)half_h_world), math_zoom);
        ImVec2 ll_b = worldPxToLonLat(ImVec2(center_world.x + (float)half_w_world, center_world.y + (float)half_h_world), math_zoom);
        const float view_min_lon = std::min(ll_a.x, ll_b.x);
        const float view_max_lon = std::max(ll_a.x, ll_b.x);
        const float view_min_lat = std::min(ll_a.y, ll_b.y);
        const float view_max_lat = std::max(ll_a.y, ll_b.y);
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
        int min_x = (int)std::floor((center_world.x - half_w_world) / 256.0) - 1;
        int max_x = (int)std::floor((center_world.x + half_w_world) / 256.0) + 1;
        int min_y = (int)std::floor((center_world.y - half_h_world) / 256.0) - 1;
        int max_y = (int)std::floor((center_world.y + half_h_world) / 256.0) + 1;
        const int period = 1 << math_zoom;
        const int max_tile = period - 1;
        const auto now_basemap_check = std::chrono::steady_clock::now();
        if (basemap_availability_last_check.time_since_epoch().count() == 0 ||
            std::chrono::duration<double>(now_basemap_check - basemap_availability_last_check).count() > 5.0) {
            basemap_availability_last_check = now_basemap_check;
            topo_tiles_available_cached =
                fs::exists(root / "data" / "tiles_topo") ||
                fs::exists(root / "data" / "tiles_topographic");
            topo_vector_available_cached = fs::exists(root / "data" / "tiles_topo_vector.geojson");
            const bool topo_dir_is_preferred = fs::exists(root / "data" / "tiles_topo");
            const bool use_topo_for_cache =
                (std::clamp(app_settings.basemap_style, 0, 1) == 1) &&
                (topo_tiles_available_cached || topo_vector_available_cached);
            tile_root_dir_cached = use_topo_for_cache
                ? (topo_dir_is_preferred ? "tiles_topo" : "tiles_topographic")
                : "tiles";
            const bool use_topo_vector_for_cache =
                use_topo_for_cache && app_settings.topo_vector_enabled && topo_vector_available_cached;
            basemap_source_has_any_files_cached = use_topo_vector_for_cache;
            if (!use_topo_vector_for_cache) {
                std::error_code ec_walk;
                const fs::path basemap_dir = root / "data" / tile_root_dir_cached;
                if (fs::exists(basemap_dir, ec_walk) && !ec_walk) {
                    for (fs::recursive_directory_iterator it(basemap_dir, ec_walk), end; !ec_walk && it != end; it.increment(ec_walk)) {
                        if (it->is_regular_file(ec_walk)) {
                            basemap_source_has_any_files_cached = true;
                            break;
                        }
                    }
                }
            }
        }
        const bool topo_tiles_available = topo_tiles_available_cached;
        const bool topo_vector_available = topo_vector_available_cached;
        const bool use_topo = (std::clamp(app_settings.basemap_style, 0, 1) == 1) && (topo_tiles_available || topo_vector_available);
        const bool use_topo_vector = use_topo && app_settings.topo_vector_enabled && topo_vector_available;
        const std::string& tile_root_dir = tile_root_dir_cached;
        const bool basemap_source_has_any_files = basemap_source_has_any_files_cached;

        const auto tile_prof_begin = std::chrono::steady_clock::now();
        if (use_topo_vector) {
            const auto& topo_lines = getTopoVectorLines(root);
            const ImU32 line_col = app_settings.grayscale_basemap ? IM_COL32(98, 98, 98, 170) : IM_COL32(106, 84, 62, 168);
            for (const auto& line_ll : topo_lines) {
                if (line_ll.size() < 2) continue;
                for (size_t i = 1; i < line_ll.size(); ++i) {
                    const ImVec2 a_world = lonLatToWorldPx(line_ll[i - 1].x, line_ll[i - 1].y, math_zoom);
                    const ImVec2 b_world = lonLatToWorldPx(line_ll[i].x, line_ll[i].y, math_zoom);
                    const ImVec2 a = project_world(a_world);
                    const ImVec2 b = project_world(b_world);
                    draw->AddLine(a, b, line_col, 1.0f);
                    prof_tiles_drawn_frame++;
                }
            }
        } else {
            for (int tx = min_x; tx <= max_x; ++tx) {
                for (int ty = min_y; ty <= max_y; ++ty) {
                    int wrapped_x = tx;
                    while (wrapped_x < 0) wrapped_x += period;
                    while (wrapped_x > max_tile) wrapped_x -= period;
                    if (ty < 0 || ty > max_tile) continue;

                    TileSample sample = getTileSample(root, tile_root_dir, math_zoom, wrapped_x, ty);
                    if (!sample.tex) continue;

                    ImVec2 tile_world((float)(tx * 256), (float)(ty * 256));
                    ImVec2 p0 = project_world(tile_world);
                    ImVec2 p1(p0.x + (float)(256.0 * zoom_scale), p0.y + (float)(256.0 * zoom_scale));
                    draw->AddImage(
                        (ImTextureID)sample.tex->descriptor,
                        p0,
                        p1,
                        sample.uv0,
                        sample.uv1,
                        app_settings.grayscale_basemap ? IM_COL32(178, 178, 178, 255) : IM_COL32_WHITE);
                    if (app_settings.grayscale_basemap) {
                        draw->AddRectFilled(p0, p1, IM_COL32(244, 244, 244, 78));
                    }
                    prof_tiles_drawn_frame++;
                }
            }
        }
        prof_tile_ms_last.store(prof_ms_since(tile_prof_begin), std::memory_order_relaxed);
        if (!basemap_source_has_any_files || (size.x > 0.0f && size.y > 0.0f && prof_tiles_drawn_frame == 0)) {
            const ImVec2 warn_pos(origin.x + 14.0f, origin.y + 14.0f);
            const ImVec2 warn_sz(std::min(560.0f, size.x - 28.0f), 94.0f);
            draw->AddRectFilled(warn_pos, ImVec2(warn_pos.x + warn_sz.x, warn_pos.y + warn_sz.y), IM_COL32(120, 18, 14, 220), 6.0f);
            draw->AddRect(warn_pos, ImVec2(warn_pos.x + warn_sz.x, warn_pos.y + warn_sz.y), IM_COL32(255, 180, 170, 230), 6.0f, 0, 1.0f);
            draw->AddText(ImVec2(warn_pos.x + 10.0f, warn_pos.y + 10.0f), IM_COL32(255, 235, 232, 255), "Basemap unavailable");
            const std::string line1 = use_topo_vector
                ? std::string("No vector lines found: data/tiles_topo_vector.geojson")
                : ("No tiles found for source: data/" + tile_root_dir);
            draw->AddText(ImVec2(warn_pos.x + 10.0f, warn_pos.y + 32.0f), IM_COL32(255, 220, 216, 255), line1.c_str());
            draw->AddText(ImVec2(warn_pos.x + 10.0f, warn_pos.y + 52.0f), IM_COL32(255, 220, 216, 255),
                          "Open Library to download required data/tiles, or switch basemap source.");
            draw->AddText(ImVec2(warn_pos.x + 10.0f, warn_pos.y + 72.0f), IM_COL32(255, 220, 216, 255),
                          "Layer overlays may still render if local layer files are present.");
        }

        bool vacant_notice_enabled = vacant_notice_layer_idx >= 0 && layers[(size_t)vacant_notice_layer_idx].enabled;
        bool vacant_rehab_enabled = vacant_rehab_layer_idx >= 0 && layers[(size_t)vacant_rehab_layer_idx].enabled;
        bool tax_lien_enabled = tax_lien_layer_idx >= 0 && layers[(size_t)tax_lien_layer_idx].enabled;
        bool tax_sale_enabled = tax_sale_layer_idx >= 0 && layers[(size_t)tax_sale_layer_idx].enabled;
        const ImVec4 vacancy_notice_color = (vacant_notice_layer_idx >= 0) ? layers[(size_t)vacant_notice_layer_idx].color : ImVec4(1, 0, 0, 1);
        const ImVec4 vacancy_rehab_color = (vacant_rehab_layer_idx >= 0) ? layers[(size_t)vacant_rehab_layer_idx].color : ImVec4(0, 1, 1, 1);
        const int lod_ring_step = lodRingStepForZoom(math_zoom);
        const bool allow_parcel_scale_fill = math_zoom >= 14;
        auto is_parcel_scale_layer = [&](size_t layer_idx) -> bool {
            return ((int)layer_idx == parcel_layer_idx ||
                    (int)layer_idx == real_property_layer_idx ||
                    (int)layer_idx == vacant_notice_layer_idx ||
                    (int)layer_idx == vacant_rehab_layer_idx ||
                    (int)layer_idx == tax_lien_layer_idx ||
                    (int)layer_idx == tax_sale_layer_idx);
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
