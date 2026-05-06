#pragma once

#include <filesystem>
#include <string>

struct WorldsimCliOptions {
    bool show_help = false;
    bool run_vacancy_selftest = false;
    bool run_download_layers = false;
    bool include_large_downloads = false;
    std::string download_phase;
};

WorldsimCliOptions parseWorldsimCliOptions(int argc, char** argv);
int runWorldsimCliImmediate(const std::filesystem::path& root, const WorldsimCliOptions& options);
void printWorldsimUsage();
