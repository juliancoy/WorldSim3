#include "lan_discovery.h"

#include "net_http_utils.h"

#include <algorithm>
#include <cstring>
#include <nlohmann/json.hpp>
#include <unordered_set>

using json = nlohmann::json;

size_t scanLanPeers(LanDiscoveryContext& ctx, int timeout_ms, bool update_status_text) {
    if (!ctx.peers || !ctx.scan_status || !ctx.last_scan_at) return size_t{0};
    ctx.peers->clear();
    if (!initNetworkSockets()) {
        if (update_status_text) *ctx.scan_status = "Scan failed: network init error";
        return size_t{0};
    }
    NetSocket s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s == kInvalidNetSocket) {
        if (update_status_text) *ctx.scan_status = "Scan failed: socket error";
        return size_t{0};
    }
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on));
#if defined(_WIN32)
    DWORD rcv_to = (DWORD)std::max(50, timeout_ms);
#else
    timeval rcv_to{};
    rcv_to.tv_sec = std::max(0, timeout_ms) / 1000;
    rcv_to.tv_usec = (std::max(0, timeout_ms) % 1000) * 1000;
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rcv_to), sizeof(rcv_to));
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(8789);
    dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    const char* probe = "WS3_DISCOVER_V1";
    (void)netSendTo(s, probe, std::strlen(probe), (sockaddr*)&dst, sizeof(dst));
    std::unordered_set<std::string> seen;
    for (;;) {
        sockaddr_in src{};
        NetSockLen slen = sizeof(src);
        char rbuf[4096];
        NetSSize rn = netRecvFrom(s, rbuf, sizeof(rbuf) - 1, (sockaddr*)&src, &slen);
        if (rn <= 0) break;
        rbuf[rn] = '\0';
        json jr = json::parse(std::string(rbuf), nullptr, false);
        if (jr.is_discarded()) continue;
        char ipbuf[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &src.sin_addr, ipbuf, sizeof(ipbuf));
        const std::string ip(ipbuf);
        if (seen.count(ip)) continue;
        seen.insert(ip);
        LanPeerInfo p;
        p.ip = ip;
        p.app_name = jr.value("app", "worldsim3");
        p.app_version = jr.value("app_version", "");
        p.protocol_version = jr.value("protocol_version", 0);
        p.dataset_port = jr.value("dataset_port", 8788);
        p.protocol_match = (p.protocol_version == ctx.protocol_version);
        ctx.peers->push_back(std::move(p));
    }
    netClose(s);
    *ctx.last_scan_at = std::chrono::steady_clock::now();
    size_t compatible = 0;
    for (const auto& p : *ctx.peers) if (p.protocol_match) compatible++;
    if (update_status_text) {
        *ctx.scan_status = "Peers: " + std::to_string(ctx.peers->size()) +
                           " | Compatible protocol: " + std::to_string(compatible);
    }
    return compatible;
}

void drawLanDiscoveryPanel(LanDiscoveryPanelContext& ctx) {
    if (!ctx.discovery || !ctx.peers || !ctx.scan_status) return;

    if (ImGui::Button("Scan LAN Peers")) (void)scanLanPeers(*ctx.discovery, 700, true);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", ctx.scan_status->c_str());
    if (!ctx.peers->empty()) {
        ImGui::BeginChild("lan_peer_list", ImVec2(0, 90), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        for (const auto& p : *ctx.peers) {
            ImGui::Text("%s:%d | program %s %s | protocol %d %s",
                p.ip.c_str(),
                p.dataset_port,
                p.app_name.empty() ? "(unknown)" : p.app_name.c_str(),
                p.app_version.empty() ? "(unknown)" : p.app_version.c_str(),
                p.protocol_version,
                p.protocol_match ? "[OK]" : "[MISMATCH]");
        }
        ImGui::EndChild();
        std::unordered_map<std::string, int> program_counts;
        for (const auto& p : *ctx.peers) {
            const std::string name = p.app_name.empty() ? "(unknown)" : p.app_name;
            const std::string ver = p.app_version.empty() ? "(unknown)" : p.app_version;
            program_counts[name + " " + ver]++;
        }
        ImGui::TextDisabled("Peer Programs");
        ImGui::BeginChild("lan_program_list", ImVec2(0, 70), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        for (const auto& kv : program_counts) {
            ImGui::Text("%s | peers %d", kv.first.c_str(), kv.second);
        }
        ImGui::EndChild();
    }
}
