#include "app_utils.h"

#include "feature_props.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <numbers>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;

const char* categoryToString(LayerDef::Category c) {
    switch (c) {
        case LayerDef::Category::Housing: return "housing";
        case LayerDef::Category::PublicHealth: return "public_health";
        case LayerDef::Category::Infrastructure: return "infrastructure";
        case LayerDef::Category::Zoning: return "zoning";
        case LayerDef::Category::Safety: return "safety";
    }
    return "unknown";
}

std::pair<int, int> deg2num(double lat_deg, double lon_deg, int zoom) {
    double lat_rad = lat_deg * std::numbers::pi / 180.0;
    double n = std::pow(2.0, zoom);
    int xtile = (int)((lon_deg + 180.0) / 360.0 * n);
    int ytile = (int)((1.0 - std::asinh(std::tan(lat_rad)) / std::numbers::pi) / 2.0 * n);
    return {xtile, ytile};
}

std::filesystem::path resolveAppRoot(const fs::path& start, const char* argv0) {
    auto has_manifest = [](const fs::path& p) {
        return fs::exists(p / "layers_manifest.json") || fs::exists(p / "scripts" / "layers_manifest.json");
    };
    auto climb = [&](fs::path p) -> fs::path {
        std::error_code ec;
        p = fs::weakly_canonical(p, ec);
        if (ec) p = fs::absolute(p);
        for (;;) {
            if (has_manifest(p)) return p;
            if (!p.has_parent_path() || p.parent_path() == p) break;
            p = p.parent_path();
        }
        return {};
    };

    if (fs::path found = climb(start); !found.empty()) return found;
    if (argv0 && *argv0) {
        fs::path exe_path(argv0);
        if (exe_path.has_parent_path()) {
            if (fs::path found = climb(exe_path.parent_path()); !found.empty()) return found;
        }
    }
    return start;
}

void setBootstrapStatus(BootstrapProgress& bp, const std::string& s) {
    std::lock_guard<std::mutex> lk(bp.msg_mutex);
    bp.status = s;
}

std::string readTextFile(const fs::path& p) {
    std::ifstream in(p);
    if (!in) return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void collectTodoWork(const std::string& todo_text, std::vector<std::string>& past, std::vector<std::string>& future) {
    std::istringstream in(todo_text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("- [x]", 0) == 0 || line.rfind("- [X]", 0) == 0) {
            past.push_back(line.substr(5));
            continue;
        }
        if (line.rfind("- ", 0) == 0) {
            future.push_back(line.substr(2));
            continue;
        }
    }
}

std::string toLowerAscii(std::string s) {
    for (char& ch : s) ch = (char)std::tolower((unsigned char)ch);
    return s;
}

bool containsCaseInsensitive(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    const std::string h = toLowerAscii(haystack);
    const std::string n = toLowerAscii(needle);
    return h.find(n) != std::string::npos;
}

std::string normalizeAddressSearchText(const std::string& s) {
    static const std::unordered_map<std::string, std::string> kCanonical{
        {"avenue", "ave"}, {"av", "ave"},
        {"boulevard", "blvd"}, {"boul", "blvd"},
        {"circle", "cir"}, {"court", "ct"}, {"drive", "dr"},
        {"highway", "hwy"}, {"lane", "ln"}, {"parkway", "pkwy"},
        {"place", "pl"}, {"road", "rd"}, {"square", "sq"},
        {"street", "st"}, {"terrace", "ter"}, {"trail", "trl"},
        {"north", "n"}, {"south", "s"}, {"east", "e"}, {"west", "w"},
        {"northeast", "ne"}, {"northwest", "nw"}, {"southeast", "se"}, {"southwest", "sw"},
        {"apartment", "apt"}, {"unit", "apt"}, {"suite", "ste"},
    };

    std::string cleaned;
    cleaned.reserve(s.size());
    for (unsigned char ch : s) {
        if (std::isalnum(ch)) cleaned.push_back((char)std::tolower(ch));
        else cleaned.push_back(' ');
    }

    std::istringstream in(cleaned);
    std::ostringstream out;
    std::string token;
    bool first = true;
    while (in >> token) {
        if (token.size() > 2) {
            const std::string suffix = token.substr(token.size() - 2);
            const bool ordinal_suffix = suffix == "st" || suffix == "nd" || suffix == "rd" || suffix == "th";
            if (ordinal_suffix && std::all_of(token.begin(), token.end() - 2, [](unsigned char ch) {
                    return std::isdigit(ch) != 0;
                })) {
                token.resize(token.size() - 2);
            }
        }
        auto it = kCanonical.find(token);
        if (it != kCanonical.end()) token = it->second;
        if (!first) out << ' ';
        out << token;
        first = false;
    }
    return out.str();
}

int addressSearchScore(const std::string& address, const std::string& query) {
    const std::string q = trimDisplayValue(query);
    if (q.empty()) return 0;
    const std::string address_lc = toLowerAscii(address);
    const std::string q_lc = toLowerAscii(q);
    if (address_lc == q_lc) return 120;

    const std::string normalized_address = normalizeAddressSearchText(address);
    const std::string normalized_query = normalizeAddressSearchText(q);
    if (normalized_query.empty()) return 0;
    if (normalized_address == normalized_query) return 110;
    if (normalized_address.rfind(normalized_query, 0) == 0) return 100;
    if (normalized_address.find(normalized_query) != std::string::npos) return 90;

    std::istringstream tokens(normalized_query);
    std::string token;
    int token_count = 0;
    while (tokens >> token) {
        token_count++;
        if (normalized_address.find(token) == std::string::npos) return 0;
    }
    return token_count > 0 ? 40 + token_count : 0;
}

bool addressMatchesSearch(const std::string& address, const std::string& query) {
    if (trimDisplayValue(query).empty()) return true;
    if (containsCaseInsensitive(address, query)) return true;
    return addressSearchScore(address, query) > 0;
}

int extractYearMaybe(const std::string& s) {
    for (size_t i = 0; i + 3 < s.size(); ++i) {
        if (!std::isdigit((unsigned char)s[i]) ||
            !std::isdigit((unsigned char)s[i + 1]) ||
            !std::isdigit((unsigned char)s[i + 2]) ||
            !std::isdigit((unsigned char)s[i + 3])) continue;
        const int y = (s[i] - '0') * 1000 + (s[i + 1] - '0') * 100 + (s[i + 2] - '0') * 10 + (s[i + 3] - '0');
        if (y >= 1900 && y <= 2100) return y;
    }
    return -1;
}

double parseNumericField(const std::string& s) {
    std::string cleaned;
    cleaned.reserve(s.size());
    for (char ch : s) {
        if (std::isdigit((unsigned char)ch) || ch == '.' || ch == '-' || ch == '+') cleaned.push_back(ch);
    }
    if (cleaned.empty() || cleaned == "-" || cleaned == "+") return 0.0;
    try {
        return std::stod(cleaned);
    } catch (...) {
        return 0.0;
    }
}

std::string trimDisplayValue(std::string s) {
    auto is_ws = [](unsigned char ch) { return std::isspace(ch) != 0; };
    while (!s.empty() && is_ws((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && is_ws((unsigned char)s.back())) s.pop_back();
    return s;
}

std::string firstDisplayProperty(const LayerDef::FeatureGeom& fg, std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        std::string v = trimDisplayValue(getPropertyValue(fg, key));
        if (!v.empty()) return v;
    }
    return "";
}

std::string blockLotJoinKeyFromParts(const std::string& block, const std::string& lot) {
    std::string b = normalizeJoinKey(block);
    std::string l = normalizeJoinKey(lot);
    if (b.empty() || l.empty()) return "";
    return b + l;
}

std::string featureBlockLotJoinKey(const LayerDef::FeatureGeom& fg) {
    std::string bl = normalizeJoinKey(getPropertyValue(fg, "BLOCKLOT"));
    if (!bl.empty()) return bl;
    bl = normalizeJoinKey(getPropertyValue(fg, "PIN"));
    if (!bl.empty()) return bl;
    bl = normalizeJoinKey(getPropertyValue(fg, "pin"));
    if (!bl.empty()) return bl;
    bl = blockLotJoinKeyFromParts(getPropertyValue(fg, "BLOCK"), getPropertyValue(fg, "LOT"));
    if (!bl.empty()) return bl;
    return blockLotJoinKeyFromParts(getPropertyValue(fg, "block"), getPropertyValue(fg, "lot"));
}

void openUrlInBrowser(const std::string& url) {
#if defined(_WIN32)
    std::string cmd = "start \"\" \"" + url + "\"";
#elif defined(__APPLE__)
    std::string cmd = "open \"" + url + "\"";
#else
    std::string cmd = "xdg-open \"" + url + "\"";
#endif
    int rc = std::system(cmd.c_str());
    (void)rc;
}
