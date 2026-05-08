#include "worldsim_cli.h"

#include "worldsim_dataset_bootstrap.h"
#include "worldsim_app.h"
#include "parcel_matched_layers.h"
#include "vacancy_overlay.h"

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <iostream>

namespace fs = std::filesystem;

WorldsimCliOptions parseWorldsimCliOptions(int argc, char** argv) {
    WorldsimCliOptions options;
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--vacancy-selftest") {
            options.run_vacancy_selftest = true;
            continue;
        }
        if (arg == "--download-layers") {
            options.run_download_layers = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                options.download_phase = argv[++i];
            } else {
                options.download_phase = "all";
            }
            continue;
        }
        if (arg == "--build-parcel-matched-layers") {
            options.run_build_parcel_matched_layers = true;
            continue;
        }
        if (arg == "--force-build-parcel-matched-layers") {
            options.run_build_parcel_matched_layers = true;
            options.force_build_parcel_matched_layers = true;
            continue;
        }
        if (arg.rfind("--download-layers=", 0) == 0) {
            options.run_download_layers = true;
            options.download_phase = arg.substr(std::strlen("--download-layers="));
            continue;
        }
        if (arg == "--include-large") {
            options.include_large_downloads = true;
            continue;
        }
        if (arg == "--reserve-one-core") {
            options.reserve_cores = 1;
            options.reserve_cores_set = true;
            continue;
        }
        if (arg == "--reserve-cores") {
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                options.reserve_cores = std::max(0, std::atoi(argv[++i]));
                options.reserve_cores_set = true;
            }
            continue;
        }
        if (arg.rfind("--reserve-cores=", 0) == 0) {
            options.reserve_cores = std::max(0, std::atoi(arg.c_str() + std::strlen("--reserve-cores=")));
            options.reserve_cores_set = true;
            continue;
        }
        if (arg == "--help" || arg == "-h") {
            options.show_help = true;
            continue;
        }
    }
    return options;
}

void printWorldsimUsage() {
    std::cout
        << "Usage: worldsim3 [--reserve-one-core|--reserve-cores N]\n"
        << "       worldsim3 [--download-layers [all|must-have|nice-to-have|heavy-data|capital-flows|extended-events|historical-high-quality|archival-research]] [--include-large]\n"
        << "       worldsim3 [--build-parcel-matched-layers|--force-build-parcel-matched-layers]\n"
        << "       worldsim3 --vacancy-selftest\n";
}

int runWorldsimCliImmediate(const fs::path& root, const WorldsimCliOptions& options) {
    if (options.show_help) {
        printWorldsimUsage();
        return 0;
    }
    if (options.run_vacancy_selftest) {
        return runVacancySelftest(root);
    }
    if (options.run_download_layers) {
        return runLayerDownloadCli(
            root,
            options.download_phase.empty() ? "all" : options.download_phase,
            options.include_large_downloads);
    }
    if (options.run_build_parcel_matched_layers) {
        ensureParcelMatchedEventLayers(root, options.force_build_parcel_matched_layers, &std::cout);
        return 0;
    }
    return -1;
}
