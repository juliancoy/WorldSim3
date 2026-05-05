#pragma once

#include "arkavo_realtime_client.h"

#include <rtc/rtc.hpp>

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class ArkavoRtcSessionManager {
public:
    explicit ArkavoRtcSessionManager(ArkavoRealtimeClient& signaling);
    ~ArkavoRtcSessionManager();

    ArkavoRtcSessionManager(const ArkavoRtcSessionManager&) = delete;
    ArkavoRtcSessionManager& operator=(const ArkavoRtcSessionManager&) = delete;

    void connectPeer(const std::string& peer_id, bool initiator);
    void removePeer(const std::string& peer_id);
    void handleSignal(const std::string& from_peer_id, const nlohmann::json& payload);
    bool sendFile(const std::string& peer_id, const std::filesystem::path& path, std::string& err);
    void closeAll();

    std::vector<std::string> connectedPeers() const;

    std::function<void(const std::string&)> on_log;
    std::function<void(const std::string&)> on_error;
    std::function<void(const std::string& peer_id, const std::filesystem::path& path)> on_file_received;

private:
    struct IncomingTransfer {
        std::string id;
        std::string name;
        std::string mime_type;
        uint64_t size = 0;
        uint64_t received = 0;
        std::filesystem::path out_path;
        std::ofstream* stream = nullptr;
    };

    struct Peer {
        std::string id;
        std::shared_ptr<rtc::PeerConnection> pc;
        std::shared_ptr<rtc::DataChannel> dc;
        std::unique_ptr<IncomingTransfer> incoming;
        bool channel_open = false;
    };

    rtc::Configuration buildRtcConfig() const;
    void attachPeerCallbacks(const std::shared_ptr<Peer>& peer);
    void attachDataChannel(const std::shared_ptr<Peer>& peer, std::shared_ptr<rtc::DataChannel> dc);
    void handleDataMessage(const std::shared_ptr<Peer>& peer, const std::variant<rtc::binary, rtc::string>& message);
    void finishIncoming(const std::shared_ptr<Peer>& peer);
    static bool validSignalPayload(const nlohmann::json& payload);

private:
    ArkavoRealtimeClient& signaling_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<Peer>> peers_;
};
