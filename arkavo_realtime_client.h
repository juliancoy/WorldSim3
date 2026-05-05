#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class ArkavoSignalingTransport {
public:
    virtual ~ArkavoSignalingTransport() = default;
    virtual bool connect(const std::string& url, std::string& err) = 0;
    virtual void close() = 0;
    virtual bool sendText(const std::string& text, std::string& err) = 0;

    std::function<void()> on_open;
    std::function<void(const std::string&)> on_message;
    std::function<void(const std::string&)> on_error;
    std::function<void()> on_close;
};

class ArkavoRealtimeClient {
public:
    struct IceConfig {
        std::vector<std::string> stun;
        std::vector<std::string> turn;
        std::string username;
        std::string credential;
    };

    struct Config {
        std::string signaling_url = "wss://signaling.arkavo.org/";
        std::string room_id;
        std::chrono::milliseconds reconnect_base{500};
        std::chrono::milliseconds reconnect_max{15000};
    };

    struct PeerSession {
        std::string peer_id;
        bool polite = false;
        bool making_offer = false;
        bool ignore_offer = false;
        bool connected = false;
    };

    explicit ArkavoRealtimeClient(Config cfg, std::unique_ptr<ArkavoSignalingTransport> transport);
    ~ArkavoRealtimeClient();

    ArkavoRealtimeClient(const ArkavoRealtimeClient&) = delete;
    ArkavoRealtimeClient& operator=(const ArkavoRealtimeClient&) = delete;

    bool start(std::string& err);
    void stop();

    bool isConnected() const;
    std::string selfPeerId() const;
    std::vector<PeerSession> peers() const;
    std::optional<IceConfig> currentIce() const;

    // Call from peer-connection layer to relay SDP/ICE through signaling.
    bool sendSignal(const std::string& target_peer_id, const nlohmann::json& payload, std::string& err);

    // Integration callbacks for native WebRTC implementation.
    std::function<void(const std::string& peer_id, bool initiator)> on_peer_should_connect;
    std::function<void(const std::string& peer_id)> on_peer_left;
    std::function<void(const std::string& from_peer_id, const nlohmann::json& payload)> on_signal_payload;
    std::function<void(const std::string&)> on_error;
    std::function<void(const std::string&)> on_log;

private:
    void wireTransport();
    void handleOpen();
    void handleClose();
    void handleError(const std::string& err);
    void handleMessage(const std::string& text);
    void scheduleReconnect();
    void cancelReconnect();

    bool sendJson(const nlohmann::json& j, std::string& err);
    bool parseHello(const nlohmann::json& j);
    bool parseJoined(const nlohmann::json& j);
    bool parsePeerJoined(const nlohmann::json& j);
    bool parsePeerLeft(const nlohmann::json& j);
    bool parseSignal(const nlohmann::json& j);

    static bool isSafePeerId(const std::string& s);

private:
    Config cfg_;
    std::unique_ptr<ArkavoSignalingTransport> transport_;

    mutable std::mutex mu_;
    std::string self_peer_id_;
    std::unordered_map<std::string, PeerSession> peer_sessions_;
    std::optional<IceConfig> ice_;

    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> intentional_stop_{false};
    std::atomic<int> reconnect_attempt_{0};
    std::thread reconnect_thread_;
    std::atomic<bool> reconnect_pending_{false};
};
