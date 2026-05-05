#include "arkavo_realtime_client.h"
#include "arkavo_rtc_session_manager.h"
#include "arkavo_signaling_transport_curl.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {
std::atomic<bool> g_stop{false};

void onSignal(int) {
    g_stop.store(true, std::memory_order_relaxed);
}

struct Args {
    std::string room = "worldsim-connectivity-test";
    std::string url = "wss://signaling.arkavo.org/";
    std::string send_peer;
    std::filesystem::path send_file;
    int timeout_seconds = 120;
};

void usage(const char* argv0) {
    std::cerr
        << "Usage: " << argv0 << " [--room ROOM] [--url WSS_URL] [--timeout SECONDS]\\n"
        << "       " << argv0 << " --room ROOM --send-peer PEER_ID --send-file PATH\\n\\n"
        << "Run two instances with the same --room to verify signaling, ICE, and data channel setup.\\n";
}

bool parseArgs(int argc, char** argv, Args& args) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](std::string& out) -> bool {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };
        if (a == "--room") {
            if (!need(args.room)) return false;
        } else if (a == "--url") {
            if (!need(args.url)) return false;
        } else if (a == "--send-peer") {
            if (!need(args.send_peer)) return false;
        } else if (a == "--send-file") {
            std::string p;
            if (!need(p)) return false;
            args.send_file = p;
        } else if (a == "--timeout") {
            std::string v;
            if (!need(v)) return false;
            args.timeout_seconds = std::stoi(v);
        } else if (a == "--help" || a == "-h") {
            usage(argv[0]);
            std::exit(0);
        } else {
            return false;
        }
    }
    return !args.room.empty() && !args.url.empty();
}
}

int main(int argc, char** argv) {
    Args args;
    if (!parseArgs(argc, argv, args)) {
        usage(argv[0]);
        return 2;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    ArkavoRealtimeClient::Config cfg;
    cfg.room_id = args.room;
    cfg.signaling_url = args.url;

    auto transport = std::make_unique<ArkavoSignalingTransportCurl>();
    ArkavoRealtimeClient client(cfg, std::move(transport));
    ArkavoRtcSessionManager rtc(client);

    std::atomic<size_t> open_channels{0};
    std::atomic<size_t> received_files{0};
    std::atomic<bool> sent_file{false};

    client.on_log = [](const std::string& msg) {
        std::cout << "[signaling] " << msg << std::endl;
    };
    client.on_error = [](const std::string& msg) {
        std::cerr << "[signaling:error] " << msg << std::endl;
    };
    client.on_peer_should_connect = [&](const std::string& peer_id, bool initiator) {
        std::cout << "[peer] connect " << peer_id << " initiator=" << (initiator ? "true" : "false") << std::endl;
        rtc.connectPeer(peer_id, initiator);
    };
    client.on_peer_left = [&](const std::string& peer_id) {
        std::cout << "[peer] left " << peer_id << std::endl;
        rtc.removePeer(peer_id);
    };
    client.on_signal_payload = [&](const std::string& peer_id, const nlohmann::json& payload) {
        rtc.handleSignal(peer_id, payload);
    };

    rtc.on_log = [&](const std::string& msg) {
        std::cout << "[rtc] " << msg << std::endl;
        open_channels.store(rtc.connectedPeers().size(), std::memory_order_relaxed);
    };
    rtc.on_error = [](const std::string& msg) {
        std::cerr << "[rtc:error] " << msg << std::endl;
    };
    rtc.on_file_received = [&](const std::string& peer_id, const std::filesystem::path& path) {
        received_files.fetch_add(1, std::memory_order_relaxed);
        std::cout << "[file] received from " << peer_id << ": " << path << std::endl;
    };

    std::string err;
    if (!client.start(err)) {
        std::cerr << "Initial connect failed: " << err << std::endl;
        // The client will continue reconnecting, so this is not fatal for a network test.
    }

    const auto start = std::chrono::steady_clock::now();
    auto last_report = start;
    while (!g_stop.load(std::memory_order_relaxed)) {
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_report).count() >= 2) {
            last_report = now;
            std::cout
                << "[status] connected=" << (client.isConnected() ? "yes" : "no")
                << " self=" << (client.selfPeerId().empty() ? "(none)" : client.selfPeerId())
                << " peers=" << client.peers().size()
                << " open_channels=" << open_channels.load(std::memory_order_relaxed)
                << " received_files=" << received_files.load(std::memory_order_relaxed)
                << std::endl;
        }

        if (!args.send_peer.empty() && !args.send_file.empty() && !sent_file.load(std::memory_order_relaxed)) {
            if (!std::filesystem::exists(args.send_file)) {
                std::cerr << "[file:error] missing file: " << args.send_file << std::endl;
                sent_file.store(true, std::memory_order_relaxed);
            } else if (!rtc.connectedPeers().empty()) {
                std::string send_err;
                if (rtc.sendFile(args.send_peer, args.send_file, send_err)) {
                    std::cout << "[file] sent " << args.send_file << " to " << args.send_peer << std::endl;
                } else {
                    std::cerr << "[file:error] send failed: " << send_err << std::endl;
                }
                sent_file.store(true, std::memory_order_relaxed);
            }
        }

        if (std::chrono::duration_cast<std::chrono::seconds>(now - start).count() >= args.timeout_seconds) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    rtc.closeAll();
    client.stop();

    const bool ok = !client.selfPeerId().empty();
    std::cout << "[result] signaling_seen=" << (ok ? "yes" : "no")
              << " open_channels=" << open_channels.load(std::memory_order_relaxed)
              << " received_files=" << received_files.load(std::memory_order_relaxed)
              << std::endl;
    return ok ? 0 : 1;
}
