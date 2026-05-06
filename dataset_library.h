#pragma once

#include <filesystem>
#include <functional>
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

struct LayerDownloadSummary {
    size_t total = 0;
    size_t downloaded = 0;
    size_t skipped = 0;
    size_t failed = 0;
    std::vector<std::string> messages;
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
std::filesystem::path layerManifestPathForPhase(const std::filesystem::path& root, const std::string& phase);
LayerDownloadSummary downloadLayerManifestPhase(
    const std::filesystem::path& root,
    const std::string& phase,
    bool include_large,
    const std::function<void(size_t, size_t, const std::string&)>& on_progress = {});
