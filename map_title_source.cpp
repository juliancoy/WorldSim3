#include "map_title_source.h"

#include "app_utils.h"

#include <algorithm>
#include <cctype>

namespace {
std::string trimCopy(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace((unsigned char)value[begin])) ++begin;
    size_t end = value.size();
    while (end > begin && std::isspace((unsigned char)value[end - 1])) --end;
    return value.substr(begin, end - begin);
}

std::string hostFromUrl(const std::string& url) {
    const size_t scheme = url.find("://");
    const size_t host_begin = scheme == std::string::npos ? 0 : scheme + 3;
    if (host_begin >= url.size()) return {};
    size_t host_end = url.find_first_of("/?#", host_begin);
    if (host_end == std::string::npos) host_end = url.size();
    return url.substr(host_begin, host_end - host_begin);
}

std::string titleCaseHostLabel(std::string host) {
    if (host.empty()) return host;
    std::replace(host.begin(), host.end(), '-', ' ');
    std::replace(host.begin(), host.end(), '.', ' ');
    bool new_word = true;
    for (char& c : host) {
        if (std::isspace((unsigned char)c)) {
            new_word = true;
            continue;
        }
        c = new_word ? (char)std::toupper((unsigned char)c) : (char)std::tolower((unsigned char)c);
        new_word = false;
    }
    return host;
}

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return value;
}

std::string inferAgencyFromLayerText(const LayerDef& layer) {
    const std::string text = lowerAscii(
        layer.name + " " +
        layer.description + " " +
        layer.source_url + " " +
        layer.reference_url + " " +
        layer.import_url + " " +
        layer.import_service_url);
    auto has = [&](const char* needle) {
        return text.find(needle) != std::string::npos;
    };

    if (has("mdot sha") || has("state highway administration") ||
        has("md_roadcenterlinescomprehensive") ||
        (has("mdgeodata.md.gov") && has("/transportation/"))) {
        return "Maryland Department of Transportation State Highway Administration (MDOT SHA)";
    }
    if (has("baltimore city health department")) return "Baltimore City Health Department";
    if (has("baltimore city open data") || has("data.baltimorecity.gov")) return "Baltimore City Open Data";
    if (has("baltimore county arcgis") || has("baltimorecountymd.gov")) return "Baltimore County Government ArcGIS REST";
    if (has("howard county")) return "Howard County Government";
    if (has("prince george") && has("planning department")) return "Prince George's County Planning Department";
    if (has("maryland open data") || has("opendata.maryland.gov")) return "Maryland Open Data";
    if (has("planning.maryland.gov")) return "Maryland Department of Planning";
    return {};
}

std::string inferAgencyFromUrl(const std::string& url) {
    std::string host = hostFromUrl(url);
    std::string lower = lowerAscii(url);
    if (lower.find("hud") != std::string::npos) return "Housing and Urban Development";
    if (lower.find("md_roadcenterlinescomprehensive") != std::string::npos ||
        (lower.find("mdgeodata.md.gov") != std::string::npos && lower.find("/transportation/") != std::string::npos)) {
        return "Maryland Department of Transportation State Highway Administration (MDOT SHA)";
    }
    if (lower.find("planning.maryland.gov") != std::string::npos) {
        return "Maryland Department of Planning";
    }
    if (lower.find("mdgeodata.md.gov") != std::string::npos) return "Maryland iMAP";
    if (lower.find("opendata.maryland.gov") != std::string::npos) return "Maryland Open Data";
    if (lower.find("baltimorecity.gov") != std::string::npos) return "Baltimore City Open Data";
    if (lower.find("baltimorecountymd.gov") != std::string::npos) return "Baltimore County Government ArcGIS REST";
    if (lower.find("howardcountymd.gov") != std::string::npos) return "Howard County Government";
    return titleCaseHostLabel(host);
}

bool layerHasSource(const LayerDef& layer) {
    if (!layer.source_urls.empty()) return true;
    return !trimCopy(layer.source_url).empty() ||
           !trimCopy(layer.reference_url).empty() ||
           !trimCopy(layer.import_url).empty() ||
           !trimCopy(layer.import_service_url).empty() ||
           !trimCopy(layer.name).empty();
}
}

std::string mapTitleSourceLabelForLayer(const LayerDef& layer) {
    if (const std::string label = inferAgencyFromLayerText(layer); !label.empty()) return label;
    for (const std::string& url : layer.source_urls) {
        if (const std::string label = inferAgencyFromUrl(url); !label.empty()) return label;
    }
    if (const std::string label = inferAgencyFromUrl(layer.source_url); !label.empty()) return label;
    if (const std::string label = inferAgencyFromUrl(layer.reference_url); !label.empty()) return label;
    if (const std::string label = inferAgencyFromUrl(layer.import_url); !label.empty()) return label;
    if (const std::string label = inferAgencyFromUrl(layer.import_service_url); !label.empty()) return label;
    return trimCopy(layer.name);
}

std::string mapTitleSourceDisplayName(const LayerDef& layer) {
    std::string name = trimCopy(layer.name);
    if (name.empty()) name = trimCopy(layer.file);
    const std::string source = mapTitleSourceLabelForLayer(layer);
    return source.empty() || source == name ? name : name + " - " + source;
}

std::vector<size_t> mapTitleSourceLayerCandidates(const std::vector<LayerDef>& layers, int parcel_layer_idx) {
    std::vector<size_t> out;
    for (size_t i = 0; i < layers.size(); ++i) {
        const LayerDef& layer = layers[i];
        if (!layerHasSource(layer)) continue;
        if (!layer.enabled && (int)i != parcel_layer_idx) continue;
        out.push_back(i);
    }
    return out;
}

size_t resolveMapTitleSourceLayerIndex(
    const std::vector<LayerDef>& layers,
    const std::string& selected_layer_file,
    int parcel_layer_idx) {
    const std::vector<size_t> candidates = mapTitleSourceLayerCandidates(layers, parcel_layer_idx);
    if (candidates.empty()) return (size_t)-1;
    if (!selected_layer_file.empty()) {
        for (const size_t idx : candidates) {
            if (idx < layers.size() && layers[idx].file == selected_layer_file) return idx;
        }
    }
    if (parcel_layer_idx >= 0) {
        for (const size_t idx : candidates) {
            if ((int)idx == parcel_layer_idx) return idx;
        }
    }
    return candidates.front();
}
