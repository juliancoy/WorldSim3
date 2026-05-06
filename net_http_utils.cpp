#include "net_http_utils.h"

#include <unistd.h>

bool writeAll(int fd, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, data + sent, len - sent);
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
