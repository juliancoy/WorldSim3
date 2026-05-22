#include "event_sectors.h"

#include "app_utils.h"
#include "feature_props.h"

#include <nlohmann/json.hpp>

namespace {
using json = nlohmann::json;

ImVec4 hexColor(const char* hex) {
    auto chan = [&](int idx) {
        const std::string s(hex + idx, 2);
        return std::stoi(s, nullptr, 16) / 255.0f;
    };
    return ImVec4(chan(1), chan(3), chan(5), 1.0f);
}

std::vector<std::string> extractEventTags(const LayerDef::FeatureRecord& fg) {
    std::vector<std::string> out;
    const std::string raw = firstDisplayProperty(fg, {"tags", "Tags", "tag", "category", "Category"});
    if (raw.empty()) return out;
    try {
        json parsed = json::parse(raw);
        if (parsed.is_array()) {
            for (const auto& item : parsed) {
                if (item.is_string()) out.push_back(trimDisplayValue(item.get<std::string>()));
            }
            return out;
        }
        if (parsed.is_string()) {
            out.push_back(trimDisplayValue(parsed.get<std::string>()));
            return out;
        }
    } catch (...) {
    }
    size_t start = 0;
    while (start < raw.size()) {
        size_t end = raw.find(',', start);
        std::string token = trimDisplayValue(raw.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (!token.empty()) out.push_back(token);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (out.empty()) out.push_back(trimDisplayValue(raw));
    return out;
}
}

const std::vector<EventSectorDef>& communitySectorDefs() {
    static const std::vector<EventSectorDef> defs = {
        {"Technology", hexColor("#2563eb"), {"Technology", "Tech Skills", "AI", "Data Science", "Cybersecurity", "Cloud & Platform", "DevOps", "Software Development", "Web Development", "JavaScript", "Python", "Ruby", "Product", "UX", "Game Development", "Technical Writing", "Open Source", "Tech Community"}},
        {"Education", hexColor("#1d4ed8"), {"Education", "Science", "Lifelong Learning", "Youth Education"}},
        {"Entrepreneurship", hexColor("#8b4513"), {"Entrepreneurship", "Business", "Startup", "Career Growth", "Professional Networking"}},
        {"Economics", hexColor("#cbd5e1"), {"Economics", "Economic Development"}},
        {"Finance", hexColor("#facc15"), {"Finance", "Crypto & Web3"}},
        {"Health", hexColor("#0f766e"), {"Health", "Wellness", "Health & Wellness"}},
        {"Politics", hexColor("#dc2626"), {"Politics", "Civic Tech", "Policy"}},
        {"Culture", hexColor("#7c3aed"), {"Culture", "Community", "Community Organizing", "Code Collective & Partners"}},
        {"Faith", hexColor("#111111"), {"Faith", "Religion", "Faith & Spirituality"}},
        {"Environment", hexColor("#16a34a"), {"Environment", "Water", "Water & Environment", "Climate", "Climate & Energy", "Energy", "Infrastructure", "Safety & Stability"}},
        {"Makerspace", hexColor("#ea580c"), {"Makerspace", "Robotics"}},
        {"Other", hexColor("#6b7280"), {}}
    };
    return defs;
}

void ensureCommunitySectorFilterDefaults(std::unordered_map<std::string, bool>& enabled) {
    for (const auto& def : communitySectorDefs()) {
        if (enabled.find(def.label) == enabled.end()) enabled[def.label] = true;
    }
}

bool isCommunitySectorEventLayer(const LayerDef& layer) {
    return layer.duckdb_role == "point_event" ||
           containsCaseInsensitive(layer.subcategory, "event") ||
           containsCaseInsensitive(layer.name, "event");
}

std::string classifyCommunitySector(const LayerDef::FeatureRecord& fg) {
    const std::vector<std::string> tags = extractEventTags(fg);
    for (const auto& def : communitySectorDefs()) {
        if (def.label == "Other") continue;
        for (const std::string& tag : tags) {
            if (containsCaseInsensitive(tag, def.label)) return def.label;
            for (const std::string& match : def.matches) {
                if (containsCaseInsensitive(tag, match)) return def.label;
            }
        }
    }
    return "Other";
}
