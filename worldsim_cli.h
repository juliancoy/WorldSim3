#pragma once

#include <filesystem>
#include <string>

struct WorldsimCliOptions {
    bool show_help = false;
    bool run_vacancy_selftest = false;
    bool run_cache_selftest = false;
    bool run_warm_hydration_cache = false;
    bool run_warm_hydration_cache_all = false;
    bool run_triangulation_cache_selftest = false;
    bool run_warm_parcel_render_cache = false;
    bool run_warm_parcel_render_cache_all = false;
    bool run_projection_cache_selftest = false;
    bool run_projection_fill_cache_selftest = false;
    bool run_projection_color_cache_selftest = false;
    bool run_parcel_render_cache_selftest = false;
    bool run_triangulation_apply_selftest = false;
    bool run_spatial_index_selftest = false;
    bool run_layer_profile_selftest = false;
    bool run_layer_runtime_status_selftest = false;
    bool run_parcel_gpu_cpu_bypass_selftest = false;
    bool run_render_policy_selftest = false;
    bool run_render_plan_selftest = false;
    bool run_canonical_parcel_binary_selftest = false;
    bool run_inspect_canonical_parcel_binary = false;
    bool run_validate_canonical_parcel_binary = false;
    bool run_parcel_artifact_health = false;
    bool run_warm_parcel_runtime_stack = false;
    bool run_warm_triangulation_cache = false;
    bool run_warm_triangulation_cache_all = false;
    bool run_download_layers = false;
    bool run_build_parcel_matched_layers = false;
    bool force_build_parcel_matched_layers = false;
    bool include_large_downloads = false;
    int reserve_cores = 0;
    bool reserve_cores_set = false;
    std::string download_phase;
    std::string warm_hydration_cache_file;
    std::string warm_triangulation_cache_file;
    std::string warm_parcel_render_cache_file;
    std::string warm_parcel_runtime_stack_file;
    std::string canonical_parcel_binary_file;
};

WorldsimCliOptions parseWorldsimCliOptions(int argc, char** argv);
int runWorldsimCliImmediate(const std::filesystem::path& root, const WorldsimCliOptions& options);
void printWorldsimUsage();
