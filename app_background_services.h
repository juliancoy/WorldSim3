#pragma once

#include "dataset_lan_api.h"
#include "status_api_context_builder.h"

#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

struct AppBackgroundServices {
    HoverDebugState hover_debug_state;
    std::mutex p2p_mutex;
    std::unordered_map<std::string, std::vector<nlohmann::json>> p2p_mailbox;
    std::thread status_api_worker;
    std::thread dataset_api_worker;
    std::thread lan_discovery_worker;
};

struct AppBackgroundServicesBootstrapInput {
    const char* app_version = nullptr;
    int protocol_version = 0;
    const std::filesystem::path* root = nullptr;
    std::atomic<bool>* stop = nullptr;
    StatusApiContextFactoryInput status_api = {};
};

void startAppBackgroundServices(
    const AppBackgroundServicesBootstrapInput& input,
    AppBackgroundServices& out);
