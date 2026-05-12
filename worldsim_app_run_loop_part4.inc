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
        FeatureFilterContextFactoryInput filter_input;
        filter_input.layers = &layers;
        filter_input.map_filters = &map_filter_state;
        filter_input.real_property_by_blocklot = &real_property_by_blocklot;
        filter_input.parcel_vac_notice_by_feature = &parcel_vac_notice_by_feature;
        filter_input.parcel_vac_rehab_by_feature = &parcel_vac_rehab_by_feature;
        filter_input.real_property_layer_idx = real_property_layer_idx;
        filter_input.parcel_layer_idx = parcel_layer_idx;
        filter_input.crime_nibrs_layer_idx = crime_nibrs_layer_idx;
        filter_input.crime_legacy_layer_idx = crime_legacy_layer_idx;
        filter_input.query_layers = &query_layers;
        FeatureFilterContext filter_ctx = makeFeatureFilterContext(filter_input);
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
        HeatmapLayerPolicyContext heatmap_policy;
        heatmap_policy.layers = &layers;
        heatmap_policy.layer_heatmap_enabled = &layer_heatmap_enabled;
        heatmap_policy.layer_heatmap_algo = &layer_heatmap_algo;
        heatmap_policy.layer_heatmap_max_zoom = &layer_heatmap_max_zoom;
        heatmap_policy.layer_parcel_detail_min_zoom = &layer_parcel_detail_min_zoom;
        heatmap_policy.layer_heatmap_cell_px = &layer_heatmap_cell_px;
        heatmap_policy.layer_heatmap_bandwidth_px = &layer_heatmap_bandwidth_px;
        heatmap_policy.layer_heatmap_blur_sigma_px = &layer_heatmap_blur_sigma_px;
        heatmap_policy.layer_heatmap_percentile_clip = &layer_heatmap_percentile_clip;
        heatmap_policy.layer_heatmap_zoom_adaptive_bandwidth = &layer_heatmap_zoom_adaptive_bandwidth;
        heatmap_policy.layer_heatmap_multires_enabled = &layer_heatmap_multires_enabled;
        heatmap_policy.layer_heatmap_multires_blend = &layer_heatmap_multires_blend;
        heatmap_policy.zoom = zoom;
        heatmap_policy.heatmap_algo = heatmap_algo;
        heatmap_policy.global_heat_cell_px = global_heat_cell_px;
        heatmap_policy.heatmap_bandwidth_px = heatmap_bandwidth_px;
        heatmap_policy.heatmap_blur_sigma_px = heatmap_blur_sigma_px;
        heatmap_policy.heatmap_percentile_clip = heatmap_percentile_clip;
        heatmap_policy.heatmap_zoom_adaptive_bandwidth = heatmap_zoom_adaptive_bandwidth;
        heatmap_policy.heatmap_multires_enabled = heatmap_multires_enabled;
        heatmap_policy.heatmap_multires_blend = heatmap_multires_blend;
        heatmap_policy.heatmap_allow_cpu_fallback = heatmap_allow_cpu_fallback;
        bool smooth_only_heatmap = true;
        bool any_active_heatmap = false;
        bool any_active_gpu_splat = false;
        for (size_t i = 0; i < layers.size(); ++i) {
            if (!layerUsesHeatmapAggregate(heatmap_policy, i)) continue;
            any_active_heatmap = true;
            const int aggregate_algo = resolveLayerAggregateAlgo(heatmap_policy, i);
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
        const std::function<bool(size_t)> layer_uses_heatmap_aggregate_fn =
            [&](size_t layer_idx) { return layerUsesHeatmapAggregate(heatmap_policy, layer_idx); };
        HeatmapKeyBuilderContext heatmap_key_ctx;
        heatmap_key_ctx.root = root;
        heatmap_key_ctx.layers = &layers;
        heatmap_key_ctx.query_layers = &query_layers;
        heatmap_key_ctx.selected_owners = &selected_owners;
        heatmap_key_ctx.layer_heatmap_use_gradient = &layer_heatmap_use_gradient;
        heatmap_key_ctx.layer_heatmap_algo = &layer_heatmap_algo;
        heatmap_key_ctx.layer_normalize_mode = &layer_normalize_mode;
        heatmap_key_ctx.layer_heatmap_max_zoom = &layer_heatmap_max_zoom;
        heatmap_key_ctx.layer_parcel_detail_min_zoom = &layer_parcel_detail_min_zoom;
        heatmap_key_ctx.layer_heatmap_cell_px = &layer_heatmap_cell_px;
        heatmap_key_ctx.layer_heatmap_bandwidth_px = &layer_heatmap_bandwidth_px;
        heatmap_key_ctx.layer_heatmap_blur_sigma_px = &layer_heatmap_blur_sigma_px;
        heatmap_key_ctx.layer_heatmap_percentile_clip = &layer_heatmap_percentile_clip;
        heatmap_key_ctx.layer_heatmap_zoom_adaptive_bandwidth = &layer_heatmap_zoom_adaptive_bandwidth;
        heatmap_key_ctx.layer_heatmap_multires_enabled = &layer_heatmap_multires_enabled;
        heatmap_key_ctx.layer_heatmap_multires_blend = &layer_heatmap_multires_blend;
        heatmap_key_ctx.layer_uses_heatmap_aggregate = &layer_uses_heatmap_aggregate_fn;
        heatmap_key_ctx.zoom = zoom;
        heatmap_key_ctx.math_zoom = math_zoom;
        heatmap_key_ctx.heatmap_algo = heatmap_algo;
        heatmap_key_ctx.effective_heatmap_quality_preset = effective_heatmap_quality_preset;
        heatmap_key_ctx.global_heat_cell_px = global_heat_cell_px;
        heatmap_key_ctx.heatmap_bandwidth_px = heatmap_bandwidth_px;
        heatmap_key_ctx.heatmap_blur_sigma_px = heatmap_blur_sigma_px;
        heatmap_key_ctx.heatmap_percentile_clip = heatmap_percentile_clip;
        heatmap_key_ctx.heatmap_zoom_adaptive_bandwidth = heatmap_zoom_adaptive_bandwidth;
        heatmap_key_ctx.heatmap_multires_enabled = heatmap_multires_enabled;
        heatmap_key_ctx.heatmap_multires_blend = heatmap_multires_blend;
        heatmap_key_ctx.heatmap_allow_cpu_fallback = heatmap_allow_cpu_fallback;
        heatmap_key_ctx.filter_enabled = filter_enabled;
        heatmap_key_ctx.filter_blocklot = filter_blocklot;
        heatmap_key_ctx.filter_status = filter_status;
        heatmap_key_ctx.filter_address = filter_address;
        heatmap_key_ctx.filter_owner = filter_owner;
        heatmap_key_ctx.filter_zip = filter_zip;
        heatmap_key_ctx.filter_use_date = filter_use_date;
        heatmap_key_ctx.filter_year_min = filter_year_min;
        heatmap_key_ctx.filter_year_max = filter_year_max;
        heatmap_key_ctx.crime_filter_enabled = crime_filter_enabled;
        heatmap_key_ctx.crime_filter_use_year = crime_filter_use_year;
        heatmap_key_ctx.crime_year_min = crime_year_min;
        heatmap_key_ctx.crime_year_max = crime_year_max;
        heatmap_key_ctx.crime_filter_homicide = crime_filter_homicide;
        heatmap_key_ctx.crime_filter_robbery = crime_filter_robbery;
        heatmap_key_ctx.crime_filter_assault = crime_filter_assault;
        heatmap_key_ctx.crime_filter_burglary = crime_filter_burglary;
        heatmap_key_ctx.crime_filter_theft = crime_filter_theft;
        heatmap_key_ctx.crime_filter_auto_theft = crime_filter_auto_theft;
        heatmap_key_ctx.crime_filter_drug = crime_filter_drug;
        heatmap_key_ctx.crime_filter_shooting = crime_filter_shooting;
        const uint64_t heatmap_view_key = buildHeatmapKey(heatmap_key_ctx, true);
        const uint64_t heatmap_data_key = buildHeatmapKey(heatmap_key_ctx, false);
        const uint64_t heatmap_key = high_quality_gpu_aggregate ? heatmap_data_key : heatmap_view_key;
        const auto heat_prof_begin = std::chrono::steady_clock::now();
        const HeatmapCacheLookup heatmap_cache_lookup = prepareHeatmapAggregateCache(
            root,
            heatmap_runtime,
            any_active_heatmap,
            smooth_only_heatmap,
            heatmap_key);
        prof_heatmap_ms_last.store(prof_ms_since(heat_prof_begin), std::memory_order_relaxed);
        const CachedAggregateTexture* cached_aggregate_for_key = heatmap_cache_lookup.cached_aggregate_for_key;
        const bool can_use_cached_heatmap = any_active_heatmap && heatmap_cache_lookup.can_use_cached_heatmap;
        const bool aggregate_generation_pending = heatmap_cache_lookup.aggregate_generation_pending;
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
        RenderPlanBuilderContext render_plan_ctx;
        render_plan_ctx.layers = &layers;
        render_plan_ctx.raw_source_layer_policy = raw_source_layer_policy;
        render_plan_ctx.zoning_layer_idx = zoning_layer_idx;
        render_plan_ctx.vacant_notice_enabled = vacant_notice_enabled;
        render_plan_ctx.vacant_rehab_enabled = vacant_rehab_enabled;
        render_plan_ctx.tax_lien_enabled = tax_lien_enabled;
        render_plan_ctx.tax_sale_enabled = tax_sale_enabled;
        render_plan_ctx.vacant_notice_layer_idx = vacant_notice_layer_idx;
        render_plan_ctx.vacant_rehab_layer_idx = vacant_rehab_layer_idx;
        render_plan_ctx.tax_lien_layer_idx = tax_lien_layer_idx;
        render_plan_ctx.tax_sale_layer_idx = tax_sale_layer_idx;
        render_plan_ctx.is_parcel_related_layer = [&](size_t layer_idx) { return isParcelRelatedLayer(filter_ctx, layer_idx); };
        render_plan_ctx.layer_uses_heatmap_aggregate = [&](size_t layer_idx) { return layerUsesHeatmapAggregate(heatmap_policy, layer_idx); };
        render_plan_ctx.layer_uses_lod_geometry = [&](size_t layer_idx) { return layerUsesLodGeometry(heatmap_policy, layer_idx); };
        const RenderPlan render_plan = buildRenderPlan(render_plan_ctx);
        const bool vacant_notice_overlay_enabled = render_plan.overlays.vacant_notice_overlay_enabled;
        const bool vacant_rehab_overlay_enabled = render_plan.overlays.vacant_rehab_overlay_enabled;
        const bool tax_lien_overlay_enabled = render_plan.overlays.tax_lien_overlay_enabled;
        const bool tax_sale_overlay_enabled = render_plan.overlays.tax_sale_overlay_enabled;
        RenderLayerPassContext layer_pass_ctx;
        layer_pass_ctx.draw = draw;
        layer_pass_ctx.origin = origin;
        layer_pass_ctx.size = size;
        layer_pass_ctx.math_zoom = math_zoom;
        layer_pass_ctx.zoom_scale = zoom_scale;
        layer_pass_ctx.lod_ring_step = lod_ring_step;
        layer_pass_ctx.parcel_layer_idx = parcel_layer_idx;
        layer_pass_ctx.zoning_layer_idx = zoning_layer_idx;
        layer_pass_ctx.vacant_notice_layer_idx = vacant_notice_layer_idx;
        layer_pass_ctx.vacant_rehab_layer_idx = vacant_rehab_layer_idx;
        layer_pass_ctx.vacancy_notice_color = &vacancy_notice_color;
        layer_pass_ctx.vacancy_rehab_color = &vacancy_rehab_color;
        layer_pass_ctx.vacant_notice_overlay_enabled = vacant_notice_overlay_enabled;
        layer_pass_ctx.vacant_rehab_overlay_enabled = vacant_rehab_overlay_enabled;
        layer_pass_ctx.view_min_lon = view_min_lon;
        layer_pass_ctx.view_min_lat = view_min_lat;
        layer_pass_ctx.view_max_lon = view_max_lon;
        layer_pass_ctx.view_max_lat = view_max_lat;
        layer_pass_ctx.should_recompute_heatmap = should_recompute_heatmap;
        layer_pass_ctx.high_quality_gpu_aggregate = high_quality_gpu_aggregate;
        layer_pass_ctx.smooth_only_heatmap = smooth_only_heatmap;
        layer_pass_ctx.can_use_cached_heatmap = can_use_cached_heatmap;
        layer_pass_ctx.layers = &layers;
        layer_pass_ctx.layer_spatial = &layer_spatial;
        layer_pass_ctx.layer_fill_enabled = &layer_fill_enabled;
        layer_pass_ctx.layer_heatmap_use_gradient = &layer_heatmap_use_gradient;
        layer_pass_ctx.layer_normalize_mode = &layer_normalize_mode;
        layer_pass_ctx.layer_heatmap_percentile_clip = &layer_heatmap_percentile_clip;
        layer_pass_ctx.parcel_vac_notice_by_feature = &parcel_vac_notice_by_feature;
        layer_pass_ctx.parcel_vac_rehab_by_feature = &parcel_vac_rehab_by_feature;
        layer_pass_ctx.zoning_zone_enabled = &zoning_zone_enabled;
        layer_pass_ctx.zoning_zone_color = &zoning_zone_color;
        layer_pass_ctx.heatmap_policy = &heatmap_policy;
        layer_pass_ctx.render_plan = &render_plan;
        layer_pass_ctx.raw_source_layer_policy = raw_source_layer_policy;
        layer_pass_ctx.heat_samples = &heat_samples;
        layer_pass_ctx.projection = &projection_cache;
        layer_pass_ctx.feature_passes_filters = feature_passes_filters;
        layer_pass_ctx.query_map_color = query_map_color;
        layer_pass_ctx.should_fill_layer_polygon = should_fill_layer_polygon;
        layer_pass_ctx.project_world = project_world;
        layer_pass_ctx.prof_features_considered_frame = &prof_features_considered_frame;
        layer_pass_ctx.prof_features_drawn_frame = &prof_features_drawn_frame;
        runRenderLayerPass(layer_pass_ctx);
        prof_layer_ms_last.store(prof_ms_since(layer_prof_begin), std::memory_order_relaxed);
        prof_heat_samples_last.store(heat_samples.size(), std::memory_order_relaxed);
        HeatmapFramePassContext heatmap_frame_ctx;
        heatmap_frame_ctx.root = &root;
        heatmap_frame_ctx.runtime = &heatmap_runtime;
        heatmap_frame_ctx.heat_samples = &heat_samples;
        heatmap_frame_ctx.draw = draw;
        heatmap_frame_ctx.origin = origin;
        heatmap_frame_ctx.size = size;
        heatmap_frame_ctx.view_min_lon = view_min_lon;
        heatmap_frame_ctx.view_min_lat = view_min_lat;
        heatmap_frame_ctx.view_max_lon = view_max_lon;
        heatmap_frame_ctx.view_max_lat = view_max_lat;
        heatmap_frame_ctx.zoom = zoom;
        heatmap_frame_ctx.math_zoom = math_zoom;
        heatmap_frame_ctx.heatmap_algo = heatmap_algo;
        heatmap_frame_ctx.global_heat_cell = global_heat_cell;
        heatmap_frame_ctx.heatmap_bandwidth_px = heatmap_bandwidth_px;
        heatmap_frame_ctx.heatmap_blur_sigma_px = heatmap_blur_sigma_px;
        heatmap_frame_ctx.heatmap_percentile_clip = heatmap_percentile_clip;
        heatmap_frame_ctx.heatmap_zoom_adaptive_bandwidth = heatmap_zoom_adaptive_bandwidth;
        heatmap_frame_ctx.heatmap_multires_enabled = heatmap_multires_enabled;
        heatmap_frame_ctx.heatmap_multires_blend = heatmap_multires_blend;
        heatmap_frame_ctx.should_recompute_heatmap = should_recompute_heatmap;
        heatmap_frame_ctx.any_active_heatmap = any_active_heatmap;
        heatmap_frame_ctx.any_active_gpu_splat = any_active_gpu_splat;
        heatmap_frame_ctx.smooth_only_heatmap = smooth_only_heatmap;
        heatmap_frame_ctx.can_use_cached_heatmap = can_use_cached_heatmap;
        heatmap_frame_ctx.high_quality_gpu_aggregate = high_quality_gpu_aggregate;
        heatmap_frame_ctx.heatmap_key = heatmap_key;
        heatmap_frame_ctx.smooth_heat_raster_base_px = smooth_heat_raster_base_px;
        heatmap_frame_ctx.smooth_heat_raster_max_px = smooth_heat_raster_max_px;
        heatmap_frame_ctx.cached_aggregate_for_key = cached_aggregate_for_key;
        heatmap_frame_ctx.project_world = project_world;
        heatmap_frame_ctx.prof_heatmap_gpu_splat_active = &prof_heatmap_gpu_splat_active;
        heatmap_frame_ctx.prof_heatmap_high_quality = &prof_heatmap_high_quality;
        heatmap_frame_ctx.prof_heatmap_cache_valid = &prof_heatmap_cache_valid;
        heatmap_frame_ctx.prof_heatmap_texture_resident = &prof_heatmap_texture_resident;
        heatmap_frame_ctx.prof_heatmap_async_inflight = &prof_heatmap_async_inflight;
        heatmap_frame_ctx.prof_heatmap_cache_key = &prof_heatmap_cache_key;
        heatmap_frame_ctx.prof_heatmap_texture_cache_entries = &prof_heatmap_texture_cache_entries;
        runHeatmapFramePass(heatmap_frame_ctx);

        const auto overlay_prof_begin = std::chrono::steady_clock::now();
        RenderTailPassContext render_tail_ctx;
        render_tail_ctx.draw = draw;
        render_tail_ctx.origin = origin;
        render_tail_ctx.size = size;
        render_tail_ctx.zoom = zoom;
        render_tail_ctx.math_zoom = math_zoom;
        render_tail_ctx.parcel_layer_idx = parcel_layer_idx;
        render_tail_ctx.parcel_parameter_mode = parcel_parameter_mode;
        render_tail_ctx.vacant_notice_layer_idx = vacant_notice_layer_idx;
        render_tail_ctx.vacant_rehab_layer_idx = vacant_rehab_layer_idx;
        render_tail_ctx.tax_lien_layer_idx = tax_lien_layer_idx;
        render_tail_ctx.tax_sale_layer_idx = tax_sale_layer_idx;
        render_tail_ctx.vacant_notice_overlay_enabled = vacant_notice_overlay_enabled;
        render_tail_ctx.vacant_rehab_overlay_enabled = vacant_rehab_overlay_enabled;
        render_tail_ctx.tax_lien_overlay_enabled = tax_lien_overlay_enabled;
        render_tail_ctx.tax_sale_overlay_enabled = tax_sale_overlay_enabled;
        render_tail_ctx.vacancy_notice_color = &vacancy_notice_color;
        render_tail_ctx.vacancy_rehab_color = &vacancy_rehab_color;
        render_tail_ctx.layers = &layers;
        render_tail_ctx.layer_fill_enabled = &layer_fill_enabled;
        render_tail_ctx.parcel_vac_notice_by_feature = &parcel_vac_notice_by_feature;
        render_tail_ctx.parcel_vac_rehab_by_feature = &parcel_vac_rehab_by_feature;
        render_tail_ctx.parcel_tax_lien_by_feature = &parcel_tax_lien_by_feature;
        render_tail_ctx.parcel_tax_sale_by_feature = &parcel_tax_sale_by_feature;
        render_tail_ctx.unified_parcels = &unified_parcels;
        render_tail_ctx.selected_parcel_indices = &selected_parcel_indices;
        render_tail_ctx.projection = &projection_cache;
        render_tail_ctx.feature_passes_filters = feature_passes_filters;
        render_tail_ctx.should_fill_layer_polygon = should_fill_layer_polygon;
        render_tail_ctx.project_world = project_world;
        const RenderTailPassResult render_tail_result = runRenderTailPass(render_tail_ctx);
        visible_vacant_parcels_last_frame.store(render_tail_result.visible_vacant_parcels, std::memory_order_relaxed);
        prof_overlay_ms_last.store(prof_ms_since(overlay_prof_begin), std::memory_order_relaxed);
        render_fill_attempts_last_frame.store(render_tail_result.fill_stats.attempts, std::memory_order_relaxed);
        render_fill_success_last_frame.store(render_tail_result.fill_stats.success, std::memory_order_relaxed);
        render_fill_no_triangles_last_frame.store(render_tail_result.fill_stats.no_triangles, std::memory_order_relaxed);
        render_fill_bad_indices_last_frame.store(render_tail_result.fill_stats.bad_indices, std::memory_order_relaxed);

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
