#include "worldsim_dataset_bootstrap.h"

#include "app_utils.h"
#include "dataset_library.h"

#include <curl/curl.h>

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace fs = std::filesystem;

int runLayerDownloadCli(const fs::path& root, const std::string& phase, bool include_large) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    std::cout << "Using app root: " << root.string() << "\n";
    std::cout << "Using manifest phase: " << (phase.empty() ? "all" : phase) << "\n";
    LayerDownloadSummary summary = downloadLayerManifestPhase(
        root,
        phase.empty() ? "all" : phase,
        include_large,
        [](size_t i, size_t total, const std::string& msg) {
            std::cout << "[" << i << "/" << total << "] " << msg << "\n";
        });
    curl_global_cleanup();
    std::cout << "Done. downloaded=" << summary.downloaded
              << " skipped=" << summary.skipped
              << " failed=" << summary.failed
              << " total=" << summary.total << "\n";
    return summary.failed == 0 ? 0 : 1;
}

bool envEnabled(const char* name) {
    const char* value = std::getenv(name);
    if (!value || !*value) return false;
    std::string s = toLowerAscii(value);
    return s != "0" && s != "false" && s != "no" && s != "off";
}

void preloadLayersFromEnvironment(const fs::path& root) {
    if (!envEnabled("WORLD_SIM3_PRELOAD_DATA")) return;

    const char* phase_env = std::getenv("WORLD_SIM3_PRELOAD_PHASE");
    const std::string preload_phase = (phase_env && *phase_env) ? std::string(phase_env) : "all";
    LayerDownloadSummary summary = downloadLayerManifestPhase(
        root,
        preload_phase,
        envEnabled("WORLD_SIM3_INCLUDE_LARGE"),
        [](size_t i, size_t total, const std::string& msg) {
            std::cout << "[preload " << i << "/" << total << "] " << msg << "\n";
        });
    if (summary.failed > 0) {
        std::cerr << "Preload completed with " << summary.failed << " failed downloads.\n";
    }
}
