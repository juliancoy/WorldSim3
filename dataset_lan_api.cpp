#include "dataset_lan_api.h"

#include "net_http_utils.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <sstream>
#include <system_error>

using json = nlohmann::json;
namespace fs = std::filesystem;

std::thread startDatasetApiWorker(DatasetLanApiContext ctx) {
    return std::thread([ctx]() mutable {

        auto& hydration_stop = *ctx.stop;
        auto& root = ctx.root;
        auto& p2p_mutex = *ctx.p2p_mutex;
        auto& p2p_mailbox = *ctx.p2p_mailbox;
        const char* kAppVersion = ctx.app_version;
        const int kProtocolVersion = ctx.protocol_version;

        if (!initNetworkSockets()) return;
        NetSocket server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd == kInvalidNetSocket) return;
        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(8788);
        if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
            netClose(server_fd);
            return;
        }
        if (listen(server_fd, 16) != 0) {
            netClose(server_fd);
            return;
        }
        auto parse_q = [](const std::string& query, const std::string& key) -> std::string {
            size_t pos = 0;
            while (pos < query.size()) {
                size_t amp = query.find('&', pos);
                std::string kv = query.substr(pos, (amp == std::string::npos ? query.size() : amp) - pos);
                size_t eq = kv.find('=');
                std::string k = eq == std::string::npos ? kv : kv.substr(0, eq);
                if (k == key) return eq == std::string::npos ? std::string() : urlDecode(kv.substr(eq + 1));
                if (amp == std::string::npos) break;
                pos = amp + 1;
            }
            return {};
        };
        auto send_json = [&](NetSocket cfd, int code, const json& j) {
            const std::string body = j.dump();
            const char* reason = code == 200 ? "OK" : (code == 404 ? "Not Found" : "Bad Request");
            std::ostringstream os;
            os << "HTTP/1.1 " << code << " " << reason << "\r\n"
               << "Content-Type: application/json\r\n"
               << "Access-Control-Allow-Origin: *\r\n"
               << "Content-Length: " << body.size() << "\r\n"
               << "Connection: close\r\n\r\n" << body;
            const std::string resp = os.str();
            (void)writeAll(cfd, resp.data(), resp.size());
        };
        while (!hydration_stop.load(std::memory_order_relaxed)) {
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(server_fd, &readfds);
            timeval tv{};
            tv.tv_sec = 1;
            int sel = select(static_cast<int>(server_fd + 1), &readfds, nullptr, nullptr, &tv);
            if (sel <= 0) continue;
            NetSocket client_fd = accept(server_fd, nullptr, nullptr);
            if (client_fd == kInvalidNetSocket) continue;

            char buf[4096];
            NetSSize n = netRead(client_fd, buf, sizeof(buf) - 1);
            if (n <= 0) {
                netClose(client_fd);
                continue;
            }
            buf[n] = '\0';
            std::string req(buf);
            size_t method_sp = req.find(' ');
            size_t path_sp = method_sp == std::string::npos ? std::string::npos : req.find(' ', method_sp + 1);
            std::string path_q = (method_sp != std::string::npos && path_sp != std::string::npos)
                ? req.substr(method_sp + 1, path_sp - method_sp - 1)
                : "/";
            size_t qpos = path_q.find('?');
            std::string path = path_q.substr(0, qpos);
            std::string query = qpos == std::string::npos ? std::string() : path_q.substr(qpos + 1);

            if (path == "/datasets") {
                json out;
                out["ok"] = true;
                out["app_version"] = kAppVersion;
                out["protocol_version"] = kProtocolVersion;
                out["root"] = (root / "data").string();
                out["files"] = json::array();
                std::error_code ec;
                for (fs::recursive_directory_iterator it(root / "data", ec), end; it != end && !ec; it.increment(ec)) {
                    if (ec || !it->is_regular_file()) continue;
                    const fs::path p = it->path();
                    json row;
                    row["path"] = fs::relative(p, root).string();
                    row["size_bytes"] = (uint64_t)it->file_size();
                    row["mtime_unix"] = (int64_t)std::chrono::duration_cast<std::chrono::seconds>(
                        it->last_write_time().time_since_epoch()).count();
                    out["files"].push_back(std::move(row));
                }
                send_json(client_fd, 200, out);
            } else if (path == "/dataset/file") {
                const std::string rel = parse_q(query, "path");
                fs::path p = root / rel;
                std::error_code ec;
                p = fs::weakly_canonical(p, ec);
                const fs::path data_root = fs::weakly_canonical(root / "data", ec);
                bool ok_path = !ec && !rel.empty() &&
                    p.string().find(data_root.string()) == 0 &&
                    fs::exists(p) && fs::is_regular_file(p);
                if (!ok_path) {
                    send_json(client_fd, 404, json{{"ok", false}, {"error", "file not found"}});
                } else {
                    std::ifstream in(p, std::ios::binary);
                    if (!in) {
                        send_json(client_fd, 404, json{{"ok", false}, {"error", "file open failed"}});
                    } else {
                        const uint64_t fsz = (uint64_t)fs::file_size(p);
                        std::ostringstream hs;
                        hs << "HTTP/1.1 200 OK\r\n"
                           << "Content-Type: application/octet-stream\r\n"
                           << "Access-Control-Allow-Origin: *\r\n"
                           << "Content-Length: " << fsz << "\r\n"
                           << "Connection: close\r\n\r\n";
                        const std::string hdr = hs.str();
                        if (writeAll(client_fd, hdr.data(), hdr.size())) {
                            char fbuf[1 << 15];
                            while (in.good()) {
                                in.read(fbuf, sizeof(fbuf));
                                std::streamsize got = in.gcount();
                                if (got > 0 && !writeAll(client_fd, fbuf, (size_t)got)) break;
                            }
                        }
                    }
                }
            } else if (path == "/p2p/register") {
                const std::string peer = parse_q(query, "peer");
                if (peer.empty()) send_json(client_fd, 400, json{{"ok", false}, {"error", "missing peer"}});
                else {
                    std::lock_guard<std::mutex> lk(p2p_mutex);
                    p2p_mailbox[peer];
                    send_json(client_fd, 200, json{{"ok", true}, {"peer", peer}});
                }
            } else if (path == "/p2p/publish") {
                const std::string to = parse_q(query, "to");
                const std::string from = parse_q(query, "from");
                const std::string type = parse_q(query, "type");
                const std::string payload = parse_q(query, "payload");
                if (to.empty() || from.empty() || type.empty()) {
                    send_json(client_fd, 400, json{{"ok", false}, {"error", "missing to/from/type"}});
                } else {
                    json msg{
                        {"from", from},
                        {"type", type},
                        {"payload", payload},
                        {"ts_unix", std::time(nullptr)}
                    };
                    std::lock_guard<std::mutex> lk(p2p_mutex);
                    p2p_mailbox[to].push_back(std::move(msg));
                    send_json(client_fd, 200, json{{"ok", true}});
                }
            } else if (path == "/p2p/poll") {
                const std::string peer = parse_q(query, "peer");
                if (peer.empty()) send_json(client_fd, 400, json{{"ok", false}, {"error", "missing peer"}});
                else {
                    json out;
                    out["ok"] = true;
                    out["peer"] = peer;
                    out["messages"] = json::array();
                    {
                        std::lock_guard<std::mutex> lk(p2p_mutex);
                        auto it = p2p_mailbox.find(peer);
                        if (it != p2p_mailbox.end()) {
                            out["messages"] = it->second;
                            it->second.clear();
                        }
                    }
                    send_json(client_fd, 200, out);
                }
            } else {
                send_json(client_fd, 404, json{{"ok", false}, {"error", "not found"}});
            }
            netClose(client_fd);
        }
        netClose(server_fd);
    
    });
}

std::thread startLanDiscoveryWorker(DatasetLanApiContext ctx) {
    return std::thread([ctx]() mutable {

        auto& hydration_stop = *ctx.stop;
        const char* kAppVersion = ctx.app_version;
        const int kProtocolVersion = ctx.protocol_version;

        if (!initNetworkSockets()) return;
        NetSocket sock = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (sock == kInvalidNetSocket) return;
        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(8789);
        if (bind(sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
            netClose(sock);
            return;
        }
        while (!hydration_stop.load(std::memory_order_relaxed)) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(sock, &rfds);
            timeval tv{};
            tv.tv_sec = 1;
            int sel = select(static_cast<int>(sock + 1), &rfds, nullptr, nullptr, &tv);
            if (sel <= 0) continue;
            sockaddr_in src{};
            NetSockLen slen = sizeof(src);
            char buf[2048];
            NetSSize n = netRecvFrom(sock, buf, sizeof(buf) - 1, (sockaddr*)&src, &slen);
            if (n <= 0) continue;
            buf[n] = '\0';
            std::string msg(buf);
            if (msg.find("WS3_DISCOVER_V1") != 0) continue;
            char ipbuf[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &src.sin_addr, ipbuf, sizeof(ipbuf));
            json out{
                {"ok", true},
                {"app", "worldsim3"},
                {"app_version", kAppVersion},
                {"protocol_version", kProtocolVersion},
                {"status_port", 8787},
                {"dataset_port", 8788},
                {"discovery_port", 8789},
                {"ip", std::string(ipbuf)}
            };
            std::string body = out.dump();
            (void)netSendTo(sock, body.data(), body.size(), (sockaddr*)&src, slen);
        }
        netClose(sock);
    
    });
}
