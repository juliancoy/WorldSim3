#include "arkavo_realtime_client.h"

#include "thread_utils.h"

#include <algorithm>
#include <cctype>

using json = nlohmann::json;

ArkavoRealtimeClient::ArkavoRealtimeClient(Config cfg, std::unique_ptr<ArkavoSignalingTransport> transport)
    : cfg_(std::move(cfg)), transport_(std::move(transport)) {
    wireTransport();
}

ArkavoRealtimeClient::~ArkavoRealtimeClient() {
    stop();
}

void ArkavoRealtimeClient::wireTransport() {
    if (!transport_) return;
    transport_->on_open = [this]() { handleOpen(); };
    transport_->on_close = [this]() { handleClose(); };
    transport_->on_error = [this](const std::string& err) { handleError(err); };
    transport_->on_message = [this](const std::string& text) { handleMessage(text); };
}

bool ArkavoRealtimeClient::start(std::string& err) {
    if (!transport_) {
        err = "Arkavo signaling transport not configured";
        return false;
    }
    if (cfg_.room_id.empty()) {
        err = "room_id is required";
        return false;
    }
    intentional_stop_.store(false, std::memory_order_relaxed);
    running_.store(true, std::memory_order_relaxed);
    reconnect_attempt_.store(0, std::memory_order_relaxed);
    if (!transport_->connect(cfg_.signaling_url, err)) {
        scheduleReconnect();
        return false;
    }
    return true;
}

void ArkavoRealtimeClient::stop() {
    intentional_stop_.store(true, std::memory_order_relaxed);
    running_.store(false, std::memory_order_relaxed);
    connected_.store(false, std::memory_order_relaxed);
    cancelReconnect();
    if (transport_) transport_->close();
    {
        std::lock_guard<std::mutex> lk(mu_);
        peer_sessions_.clear();
        self_peer_id_.clear();
        ice_.reset();
    }
}

bool ArkavoRealtimeClient::isConnected() const {
    return connected_.load(std::memory_order_relaxed);
}

std::string ArkavoRealtimeClient::selfPeerId() const {
    std::lock_guard<std::mutex> lk(mu_);
    return self_peer_id_;
}

std::vector<ArkavoRealtimeClient::PeerSession> ArkavoRealtimeClient::peers() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<PeerSession> out;
    out.reserve(peer_sessions_.size());
    for (const auto& kv : peer_sessions_) out.push_back(kv.second);
    return out;
}

std::optional<ArkavoRealtimeClient::IceConfig> ArkavoRealtimeClient::currentIce() const {
    std::lock_guard<std::mutex> lk(mu_);
    return ice_;
}

bool ArkavoRealtimeClient::sendSignal(const std::string& target_peer_id, const json& payload, std::string& err) {
    if (!running_.load(std::memory_order_relaxed)) {
        err = "client not running";
        return false;
    }
    if (!payload.is_object()) {
        err = "signal payload must be JSON object";
        return false;
    }
    json out = {
        {"type", "signal"},
        {"targetPeerId", target_peer_id.empty() ? nullptr : json(target_peer_id)},
        {"payload", payload}
    };
    return sendJson(out, err);
}

void ArkavoRealtimeClient::handleOpen() {
    connected_.store(true, std::memory_order_relaxed);
    reconnect_attempt_.store(0, std::memory_order_relaxed);
    if (on_log) on_log("Arkavo signaling connected");
}

void ArkavoRealtimeClient::handleClose() {
    connected_.store(false, std::memory_order_relaxed);
    std::vector<std::string> removed_peers;
    {
        std::lock_guard<std::mutex> lk(mu_);
        removed_peers.reserve(peer_sessions_.size());
        for (const auto& kv : peer_sessions_) removed_peers.push_back(kv.first);
        peer_sessions_.clear();
    }
    for (const auto& peer_id : removed_peers) {
        if (on_peer_left) on_peer_left(peer_id);
    }
    if (on_log) on_log("Arkavo signaling closed");
    if (!intentional_stop_.load(std::memory_order_relaxed)) scheduleReconnect();
}

void ArkavoRealtimeClient::handleError(const std::string& err) {
    if (on_error) on_error(err);
}

void ArkavoRealtimeClient::handleMessage(const std::string& text) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return;
    const std::string type = j.value("type", std::string());
    bool ok = false;
    if (type == "hello") ok = parseHello(j);
    else if (type == "joined") ok = parseJoined(j);
    else if (type == "peer-joined") ok = parsePeerJoined(j);
    else if (type == "peer-left") ok = parsePeerLeft(j);
    else if (type == "signal") ok = parseSignal(j);
    if (!ok && on_error) on_error("Rejected invalid signaling message of type: " + type);
}

void ArkavoRealtimeClient::scheduleReconnect() {
    if (!running_.load(std::memory_order_relaxed)) return;
    bool expected = false;
    if (!reconnect_pending_.compare_exchange_strong(expected, true)) return;
    cancelReconnect();
    reconnect_thread_ = std::thread([this]() {
        setCurrentThreadName("ws3-ark-reconn");
        while (running_.load(std::memory_order_relaxed) && !intentional_stop_.load(std::memory_order_relaxed)) {
            int attempt = reconnect_attempt_.fetch_add(1, std::memory_order_relaxed);
            auto backoff = cfg_.reconnect_base * (1 << std::min(attempt, 12));
            if (backoff > cfg_.reconnect_max) backoff = cfg_.reconnect_max;
            std::this_thread::sleep_for(backoff);
            if (!running_.load(std::memory_order_relaxed) || intentional_stop_.load(std::memory_order_relaxed)) break;

            std::string err;
            if (transport_ && transport_->connect(cfg_.signaling_url, err)) {
                if (on_log) on_log("Arkavo reconnect attempted");
                break;
            }
            if (on_error && !err.empty()) on_error("Reconnect failed: " + err);
        }
        reconnect_pending_.store(false, std::memory_order_relaxed);
    });
}

void ArkavoRealtimeClient::cancelReconnect() {
    reconnect_pending_.store(false, std::memory_order_relaxed);
    if (reconnect_thread_.joinable() && reconnect_thread_.get_id() != std::this_thread::get_id()) {
        reconnect_thread_.join();
    }
}

bool ArkavoRealtimeClient::sendJson(const json& j, std::string& err) {
    if (!transport_) {
        err = "transport unavailable";
        return false;
    }
    return transport_->sendText(j.dump(), err);
}

bool ArkavoRealtimeClient::parseHello(const json& j) {
    if (!j.contains("peerId") || !j["peerId"].is_string()) return false;
    const std::string peer_id = j["peerId"].get<std::string>();
    if (!isSafePeerId(peer_id)) return false;

    IceConfig parsed_ice;
    if (j.contains("ice") && j["ice"].is_object()) {
        const auto& ice = j["ice"];
        if (ice.contains("stun") && ice["stun"].is_array()) {
            for (const auto& v : ice["stun"]) if (v.is_string()) parsed_ice.stun.push_back(v.get<std::string>());
        }
        if (ice.contains("turn") && ice["turn"].is_array()) {
            for (const auto& v : ice["turn"]) if (v.is_string()) parsed_ice.turn.push_back(v.get<std::string>());
        }
        parsed_ice.username = ice.value("username", std::string());
        parsed_ice.credential = ice.value("credential", std::string());
    }

    if (parsed_ice.stun.empty()) parsed_ice.stun.push_back("stun:stun.l.google.com:19302");

    {
        std::lock_guard<std::mutex> lk(mu_);
        self_peer_id_ = peer_id;
        ice_ = parsed_ice;
    }

    std::string err;
    const bool joined = sendJson(json{{"type", "join"}, {"roomId", cfg_.room_id}}, err);
    if (!joined && on_error) on_error("Failed to send join: " + err);
    return joined;
}

bool ArkavoRealtimeClient::parseJoined(const json& j) {
    if (j.contains("peers") && !j["peers"].is_array()) return false;
    std::string self;
    {
        std::lock_guard<std::mutex> lk(mu_);
        self = self_peer_id_;
    }
    if (!j.contains("peers")) return true;
    for (const auto& p : j["peers"]) {
        if (!p.is_string()) continue;
        const std::string peer_id = p.get<std::string>();
        if (!isSafePeerId(peer_id) || peer_id == self) continue;
        PeerSession ps;
        ps.peer_id = peer_id;
        ps.polite = self < peer_id;
        {
            std::lock_guard<std::mutex> lk(mu_);
            peer_sessions_[peer_id] = ps;
        }
        if (on_peer_should_connect) on_peer_should_connect(peer_id, self < peer_id);
    }
    return true;
}

bool ArkavoRealtimeClient::parsePeerJoined(const json& j) {
    if (!j.contains("peerId") || !j["peerId"].is_string()) return false;
    const std::string peer_id = j["peerId"].get<std::string>();
    if (!isSafePeerId(peer_id)) return false;
    std::string self;
    {
        std::lock_guard<std::mutex> lk(mu_);
        self = self_peer_id_;
    }
    if (peer_id == self) return true;
    PeerSession ps;
    ps.peer_id = peer_id;
    ps.polite = self < peer_id;
    {
        std::lock_guard<std::mutex> lk(mu_);
        peer_sessions_[peer_id] = ps;
    }
    if (on_peer_should_connect) on_peer_should_connect(peer_id, self < peer_id);
    return true;
}

bool ArkavoRealtimeClient::parsePeerLeft(const json& j) {
    if (!j.contains("peerId") || !j["peerId"].is_string()) return false;
    const std::string peer_id = j["peerId"].get<std::string>();
    if (!isSafePeerId(peer_id)) return false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        peer_sessions_.erase(peer_id);
    }
    if (on_peer_left) on_peer_left(peer_id);
    return true;
}

bool ArkavoRealtimeClient::parseSignal(const json& j) {
    if (!j.contains("fromPeerId") || !j["fromPeerId"].is_string()) return false;
    if (!j.contains("payload") || !j["payload"].is_object()) return false;
    const std::string from_peer_id = j["fromPeerId"].get<std::string>();
    if (!isSafePeerId(from_peer_id)) return false;
    if (on_signal_payload) on_signal_payload(from_peer_id, j["payload"]);
    return true;
}

bool ArkavoRealtimeClient::isSafePeerId(const std::string& s) {
    if (s.empty() || s.size() > 128) return false;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == ':' ) continue;
        return false;
    }
    return true;
}
