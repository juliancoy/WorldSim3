#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

struct VersionedDownloadResult {
    bool ok = false;
    bool not_modified = false;
    bool changed = false;
    std::string message;
};

enum class FreshnessState {
    Unknown,
    UpToDate,
    UpdateAvailable,
    NotTrackable,
    Error
};

struct FreshnessCheckResult {
    FreshnessState state = FreshnessState::Unknown;
    bool ok = false;
    std::string message;
};

bool downloadUrlToFile(const std::string& url, const std::filesystem::path& out_path, std::string& err);
VersionedDownloadResult downloadUrlVersioned(
    const std::string& url,
    const std::filesystem::path& out_path,
    const std::filesystem::path& versions_root);
FreshnessCheckResult checkUrlFreshnessVersioned(
    const std::string& url,
    const std::filesystem::path& out_path,
    const std::filesystem::path& versions_root);
std::vector<nlohmann::json> readLayerManifestEntries(const std::filesystem::path& root);
