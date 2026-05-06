#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct DatasetLanApiContext {
    const char* app_version = nullptr;
    int protocol_version = 0;
    std::filesystem::path root;
    std::atomic<bool>* stop = nullptr;
    std::mutex* p2p_mutex = nullptr;
    std::unordered_map<std::string, std::vector<nlohmann::json>>* p2p_mailbox = nullptr;
};

std::thread startDatasetApiWorker(DatasetLanApiContext ctx);
std::thread startLanDiscoveryWorker(DatasetLanApiContext ctx);
