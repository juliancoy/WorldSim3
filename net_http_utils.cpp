#include "net_http_utils.h"

#include <atomic>
#include <mutex>

bool initNetworkSockets() {
#if defined(_WIN32)
    static std::once_flag once;
    static std::atomic<bool> ok{false};
    std::call_once(once, []() {
        WSADATA data{};
        ok.store(WSAStartup(MAKEWORD(2, 2), &data) == 0, std::memory_order_relaxed);
    });
    return ok.load(std::memory_order_relaxed);
#else
    return true;
#endif
}

void shutdownNetworkSockets() {
#if defined(_WIN32)
    WSACleanup();
#endif
}

int netClose(NetSocket fd) {
#if defined(_WIN32)
    return closesocket(fd);
#else
    return close(fd);
#endif
}

NetSSize netRead(NetSocket fd, char* data, size_t len) {
#if defined(_WIN32)
    return recv(fd, data, static_cast<int>(len), 0);
#else
    return read(fd, data, len);
#endif
}

NetSSize netRecvFrom(NetSocket fd, char* data, size_t len, sockaddr* from, NetSockLen* from_len) {
#if defined(_WIN32)
    return recvfrom(fd, data, static_cast<int>(len), 0, from, from_len);
#else
    return recvfrom(fd, data, len, 0, from, from_len);
#endif
}

NetSSize netSendTo(NetSocket fd, const char* data, size_t len, const sockaddr* to, NetSockLen to_len) {
#if defined(_WIN32)
    return sendto(fd, data, static_cast<int>(len), 0, to, to_len);
#else
    return sendto(fd, data, len, 0, to, to_len);
#endif
}

bool writeAll(NetSocket fd, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        NetSSize n = 0;
#if defined(_WIN32)
        n = send(fd, data + sent, static_cast<int>(len - sent), 0);
#else
        n = write(fd, data + sent, len - sent);
#endif
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

std::string urlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    auto hexv = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+' ) out.push_back(' ');
        else if (s[i] == '%' && i + 2 < s.size()) {
            int a = hexv(s[i + 1]);
            int b = hexv(s[i + 2]);
            if (a >= 0 && b >= 0) {
                out.push_back((char)((a << 4) | b));
                i += 2;
            } else out.push_back(s[i]);
        } else out.push_back(s[i]);
    }
    return out;
}

std::string urlEncodeComponent(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        const bool safe =
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~' || c == '/';
        if (safe) out.push_back((char)c);
        else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0x0F]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}
