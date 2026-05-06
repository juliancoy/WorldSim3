#pragma once

#include <filesystem>
#include <string>

int runLayerDownloadCli(const std::filesystem::path& root, const std::string& phase, bool include_large);
void preloadLayersFromEnvironment(const std::filesystem::path& root);
bool envEnabled(const char* name);
