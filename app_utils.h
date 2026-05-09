#pragma once

#include "types.h"

#include <atomic>
#include <filesystem>
#include <initializer_list>
#include <mutex>
#include <string>
#include <vector>

struct BootstrapProgress {
    std::atomic<bool> running{false};
    std::atomic<bool> done{false};
    std::atomic<bool> failed{false};
    std::atomic<int> phase{0};
    std::atomic<size_t> done_items{0};
    std::atomic<size_t> total_items{0};
    std::atomic<size_t> skipped_layers{0};
    std::atomic<size_t> skipped_tiles{0};
    std::vector<std::string> skipped_layer_files;
    std::string status;
    std::string error;
    std::mutex msg_mutex;
};

const char* categoryToString(LayerDef::Category c);
std::pair<int, int> deg2num(double lat_deg, double lon_deg, int zoom);
std::filesystem::path resolveAppRoot(const std::filesystem::path& start, const char* argv0 = nullptr);
void setBootstrapStatus(BootstrapProgress& bp, const std::string& s);
std::string readTextFile(const std::filesystem::path& p);
void collectTodoWork(const std::string& todo_text, std::vector<std::string>& past, std::vector<std::string>& future);
std::string toLowerAscii(std::string s);
bool containsCaseInsensitive(const std::string& haystack, const std::string& needle);
std::string normalizeAddressSearchText(const std::string& s);
int addressSearchScore(const std::string& address, const std::string& query);
bool addressMatchesSearch(const std::string& address, const std::string& query);
int extractYearMaybe(const std::string& s);
double parseNumericField(const std::string& s);
std::string formatUsNumber(double value, int decimals = 0);
std::string formatUsd(double value, int decimals = 0);
std::string trimDisplayValue(std::string s);
std::string firstDisplayProperty(const LayerDef::FeatureGeom& fg, std::initializer_list<const char*> keys);
std::string blockLotJoinKeyFromParts(const std::string& block, const std::string& lot);
std::string featureBlockLotJoinKey(const LayerDef::FeatureGeom& fg);
void openUrlInBrowser(const std::string& url);
