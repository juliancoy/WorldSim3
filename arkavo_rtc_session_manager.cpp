#include "arkavo_rtc_session_manager.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

using json = nlohmann::json;

ArkavoRtcSessionManager::ArkavoRtcSessionManager(ArkavoRealtimeClient& signaling)
    : signaling_(signaling) {}

ArkavoRtcSessionManager::~ArkavoRtcSessionManager() {
    closeAll();
}

void ArkavoRtcSessionManager::connectPeer(const std::string& peer_id, bool initiator) {
    std::shared_ptr<Peer> peer;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = peers_.find(peer_id);
        if (it != peers_.end()) return;

        peer = std::make_shared<Peer>();
        peer->id = peer_id;
        peer->pc = std::make_shared<rtc::PeerConnection>(buildRtcConfig());
        peers_[peer_id] = peer;
    }

    attachPeerCallbacks(peer);
    if (initiator) {
        auto dc = peer->pc->createDataChannel("files");
        attachDataChannel(peer, dc);
    }
}

void ArkavoRtcSessionManager::removePeer(const std::string& peer_id) {
    std::shared_ptr<Peer> peer;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = peers_.find(peer_id);
        if (it == peers_.end()) return;
        peer = it->second;
        peers_.erase(it);
    }
    if (peer->dc) peer->dc->close();
    if (peer->pc) peer->pc->close();
}

void ArkavoRtcSessionManager::handleSignal(const std::string& from_peer_id, const json& payload) {
    if (!validSignalPayload(payload)) return;

    std::shared_ptr<Peer> peer;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = peers_.find(from_peer_id);
        if (it != peers_.end()) peer = it->second;
    }
    if (!peer) {
        connectPeer(from_peer_id, false);
        std::lock_guard<std::mutex> lk(mu_);
        peer = peers_.at(from_peer_id);
    }

    try {
        if (payload.contains("description")) {
            const auto& d = payload["description"];
            const std::string type = d.value("type", std::string());
            const std::string sdp = d.value("sdp", std::string());
            if (type.empty() || sdp.empty()) return;
            peer->pc->setRemoteDescription(rtc::Description(sdp, type));
        } else if (payload.contains("candidate")) {
            const auto& c = payload["candidate"];
            const std::string candidate = c.value("candidate", std::string());
            const std::string mid = c.value("sdpMid", c.value("mid", std::string()));
            if (candidate.empty() || mid.empty()) return;
            peer->pc->addRemoteCandidate(rtc::Candidate(candidate, mid));
        }
    } catch (const std::exception& e) {
        if (on_error) on_error(std::string("Arkavo RTC signal failed: ") + e.what());
    }
}

bool ArkavoRtcSessionManager::sendFile(const std::string& peer_id, const std::filesystem::path& path, std::string& err) {
    std::shared_ptr<Peer> peer;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = peers_.find(peer_id);
        if (it == peers_.end()) {
            err = "peer not found";
            return false;
        }
        peer = it->second;
    }
    if (!peer->dc || !peer->channel_open) {
        err = "data channel not open";
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "file open failed";
        return false;
    }

    const uint64_t size = (uint64_t)std::filesystem::file_size(path);
    const std::string id = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    json meta = {
        {"type", "file-meta"},
        {"transferId", id},
        {"name", path.filename().string()},
        {"mimeType", "application/octet-stream"},
        {"size", size}
    };
    peer->dc->send(meta.dump());

    std::vector<std::byte> buf(64 * 1024);
    while (in.good()) {
        in.read(reinterpret_cast<char*>(buf.data()), (std::streamsize)buf.size());
        std::streamsize got = in.gcount();
        if (got <= 0) break;
        rtc::binary chunk(buf.begin(), buf.begin() + got);
        peer->dc->send(chunk);
    }
    peer->dc->send(json{{"type", "file-end"}, {"transferId", id}}.dump());
    return true;
}

void ArkavoRtcSessionManager::closeAll() {
    std::vector<std::shared_ptr<Peer>> peers;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& kv : peers_) peers.push_back(kv.second);
        peers_.clear();
    }
    for (auto& p : peers) {
        if (p->dc) p->dc->close();
        if (p->pc) p->pc->close();
    }
}

std::vector<std::string> ArkavoRtcSessionManager::connectedPeers() const {
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& kv : peers_) {
        if (kv.second->channel_open) out.push_back(kv.first);
    }
    return out;
}

rtc::Configuration ArkavoRtcSessionManager::buildRtcConfig() const {
    rtc::Configuration cfg;
    auto ice = signaling_.currentIce();
    if (ice) {
        for (const auto& url : ice->turn) {
            rtc::IceServer server(url);
            if (!ice->username.empty()) server.username = ice->username;
            if (!ice->credential.empty()) server.password = ice->credential;
            cfg.iceServers.push_back(std::move(server));
        }
        for (const auto& url : ice->stun) cfg.iceServers.emplace_back(url);
    } else {
        cfg.iceServers.emplace_back("stun:stun.l.google.com:19302");
    }
    return cfg;
}

void ArkavoRtcSessionManager::attachPeerCallbacks(const std::shared_ptr<Peer>& peer) {
    std::weak_ptr<Peer> weak = peer;
    peer->pc->onLocalDescription([this, weak](rtc::Description desc) {
        auto p = weak.lock();
        if (!p) return;
        json payload = {
            {"description", {
                {"type", desc.typeString()},
                {"sdp", std::string(desc)}
            }}
        };
        std::string err;
        if (!signaling_.sendSignal(p->id, payload, err) && on_error) on_error(err);
    });
    peer->pc->onLocalCandidate([this, weak](rtc::Candidate cand) {
        auto p = weak.lock();
        if (!p) return;
        json payload = {
            {"candidate", {
                {"candidate", cand.candidate()},
                {"sdpMid", cand.mid()}
            }}
        };
        std::string err;
        if (!signaling_.sendSignal(p->id, payload, err) && on_error) on_error(err);
    });
    peer->pc->onDataChannel([this, weak](std::shared_ptr<rtc::DataChannel> dc) {
        auto p = weak.lock();
        if (p) attachDataChannel(p, std::move(dc));
    });
    peer->pc->onStateChange([this, weak](rtc::PeerConnection::State state) {
        auto p = weak.lock();
        if (!p) return;
        if (on_log) on_log("RTC peer " + p->id + " state " + std::to_string((int)state));
    });
}

void ArkavoRtcSessionManager::attachDataChannel(const std::shared_ptr<Peer>& peer, std::shared_ptr<rtc::DataChannel> dc) {
    peer->dc = dc;
    std::weak_ptr<Peer> weak = peer;
    dc->onOpen([this, weak]() {
        auto p = weak.lock();
        if (!p) return;
        p->channel_open = true;
        if (on_log) on_log("Arkavo data channel open: " + p->id);
    });
    dc->onClosed([this, weak]() {
        auto p = weak.lock();
        if (!p) return;
        p->channel_open = false;
        if (on_log) on_log("Arkavo data channel closed: " + p->id);
    });
    dc->onMessage([this, weak](std::variant<rtc::binary, rtc::string> message) {
        auto p = weak.lock();
        if (p) handleDataMessage(p, message);
    });
}

void ArkavoRtcSessionManager::handleDataMessage(const std::shared_ptr<Peer>& peer, const std::variant<rtc::binary, rtc::string>& message) {
    if (std::holds_alternative<rtc::string>(message)) {
        json j = json::parse(std::get<rtc::string>(message), nullptr, false);
        if (j.is_discarded() || !j.is_object()) return;
        const std::string type = j.value("type", std::string());
        if (type == "file-meta") {
            auto incoming = std::make_unique<IncomingTransfer>();
            incoming->id = j.value("transferId", std::string());
            incoming->name = j.value("name", std::string("received.bin"));
            incoming->mime_type = j.value("mimeType", std::string("application/octet-stream"));
            incoming->size = j.value("size", 0ull);
            std::filesystem::create_directories("data/received");
            incoming->out_path = std::filesystem::path("data/received") / incoming->name;
            incoming->stream = new std::ofstream(incoming->out_path, std::ios::binary);
            peer->incoming = std::move(incoming);
        } else if (type == "file-end") {
            finishIncoming(peer);
        }
        return;
    }

    if (!peer->incoming || !peer->incoming->stream || !*peer->incoming->stream) return;
    const auto& b = std::get<rtc::binary>(message);
    peer->incoming->stream->write(reinterpret_cast<const char*>(b.data()), (std::streamsize)b.size());
    peer->incoming->received += b.size();
}

void ArkavoRtcSessionManager::finishIncoming(const std::shared_ptr<Peer>& peer) {
    if (!peer->incoming) return;
    if (peer->incoming->stream) {
        peer->incoming->stream->close();
        delete peer->incoming->stream;
        peer->incoming->stream = nullptr;
    }
    if (on_file_received) on_file_received(peer->id, peer->incoming->out_path);
    peer->incoming.reset();
}

bool ArkavoRtcSessionManager::validSignalPayload(const json& payload) {
    if (!payload.is_object()) return false;
    if (payload.contains("description")) {
        const auto& d = payload["description"];
        return d.is_object() && d.contains("type") && d["type"].is_string() &&
               d.contains("sdp") && d["sdp"].is_string();
    }
    if (payload.contains("candidate")) {
        const auto& c = payload["candidate"];
        return c.is_object() && c.contains("candidate") && c["candidate"].is_string();
    }
    return false;
}
