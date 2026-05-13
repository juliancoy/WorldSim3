#pragma once

#include "imgui.h"

#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

struct LanPeerInfo {
    std::string ip;
    std::string app_name;
    std::string app_version;
    int protocol_version = 0;
    int dataset_port = 0;
    bool protocol_match = false;
};

struct LanDiscoveryContext {
    std::vector<LanPeerInfo>* peers = nullptr;
    std::string* scan_status = nullptr;
    std::chrono::steady_clock::time_point* last_scan_at = nullptr;
    int protocol_version = 0;
};

size_t scanLanPeers(LanDiscoveryContext& ctx, int timeout_ms, bool update_status_text);

struct LanDiscoveryPanelContext {
    LanDiscoveryContext* discovery = nullptr;
    const std::vector<LanPeerInfo>* peers = nullptr;
    const std::string* scan_status = nullptr;
};

void drawLanDiscoveryPanel(LanDiscoveryPanelContext& ctx);
