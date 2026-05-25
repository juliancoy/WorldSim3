#include "layer_import.h"
#include "app_utils.h"
#include "cache_io.h"
#include "layer_geometry.h"

#include <zlib.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
std::string previewBytesForError(const std::string& text, size_t max_len = 160) {
    std::string out;
    out.reserve(std::min(text.size(), max_len));
    for (char ch : text) {
        if (out.size() >= max_len) break;
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (ch == '\n' || ch == '\r' || ch == '\t') out.push_back(' ');
        else if (std::isprint(uch)) out.push_back(ch);
        else out.push_back('?');
    }
    return trimDisplayValue(out);
}

std::string trimLeadingWhitespace(std::string text) {
    size_t i = 0;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    return text.substr(i);
}

bool looksLikeMissingCensusKeyHtml(const std::string& text) {
    const std::string normalized = toLowerAscii(trimLeadingWhitespace(text));
    return normalized.find("<html") != std::string::npos &&
           normalized.find("missing key") != std::string::npos;
}

bool validateCensusAcsDownloadArtifact(const fs::path& path, std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "failed to open downloaded ACS response " + path.string();
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string raw = buffer.str();
    const std::string trimmed = trimLeadingWhitespace(raw);
    if (trimmed.empty()) {
        error = "ACS response is empty: " + path.string();
        return false;
    }
    if (looksLikeMissingCensusKeyHtml(trimmed)) {
        error =
            "ACS download rejected: Census returned a 'Missing Key' HTML page for " + path.string() +
            ". Configure WORLDSIM_CENSUS_API_KEY or CENSUS_API_KEY.";
        return false;
    }
    if (trimmed.front() != '[') {
        error =
            "ACS download rejected: expected a JSON header/data array in " + path.string() +
            " preview=\"" + previewBytesForError(trimmed) + "\"";
        return false;
    }
    return true;
}

std::string censusApiKeyFromEnvironment() {
    const char* key = std::getenv("WORLDSIM_CENSUS_API_KEY");
    if (key && *key) return std::string(key);
    key = std::getenv("CENSUS_API_KEY");
    if (key && *key) return std::string(key);
    return {};
}

std::string censusImportUrlWithKey(const LayerDef& layer) {
    std::string url = layer.import_url;
    if (url.empty()) return url;
    if (url.find("key=") != std::string::npos) return url;
    const std::string key = censusApiKeyFromEnvironment();
    if (key.empty()) return url;
    url += (url.find('?') == std::string::npos) ? '?' : '&';
    url += "key=" + key;
    return url;
}

bool sourceGeometryFilenameMatchesTargetLayer(
    const fs::path& source_geometry_path,
    const fs::path& target_layer_path) {
    return source_geometry_path.filename() == target_layer_path.filename();
}

struct ZipEntry {
    std::string name;
    uint16_t method = 0;
    uint32_t compressed_size = 0;
    uint32_t uncompressed_size = 0;
    uint32_t local_header_offset = 0;
};

struct DbfField {
    std::string name;
    char type = 'C';
    uint8_t length = 0;
};

struct PointD { double x = 0.0, y = 0.0; };

struct HttpResponse {
    long code = 0;
    std::string body;
};

void persistCanonicalLayerBinaryAndRemoveGeoJson(const fs::path& geojson_path);
void saveCanonicalLayerBinary(
    const fs::path& target_layer_path,
    const std::string& sig,
    const std::vector<LayerDef::FeatureRecord>& features,
    const std::vector<LayerDef::FeatureProperties>& feature_properties);
void saveCanonicalLayerBinaryForSourceGeometry(
    const fs::path& source_geometry_path,
    const fs::path& target_layer_path,
    const std::string& sig,
    const std::vector<LayerDef::FeatureRecord>& features,
    const std::vector<LayerDef::FeatureProperties>& feature_properties);
void appendGeoJsonFeatureRecords(
    const json& feature,
    std::vector<LayerDef::FeatureRecord>& features,
    std::vector<LayerDef::FeatureProperties>& feature_properties);
void buildArcgisFeatureLayerFeatures(
    const std::string& service_url,
    const std::string& where_clause,
    const std::string& normalizer,
    std::vector<LayerDef::FeatureRecord>& features,
    std::vector<LayerDef::FeatureProperties>& feature_properties);
void buildCensusAcsGeoJsonFeatures(
    const fs::path& geojson_path,
    const std::unordered_map<std::string, json>& acs_rows_by_geoid,
    std::vector<LayerDef::FeatureRecord>& features,
    std::vector<LayerDef::FeatureProperties>& feature_properties);
struct CensusAcsJoinStats;
CensusAcsJoinStats buildCensusAcsArcgisLayerFeatures(
    const std::string& service_url,
    const std::string& where_clause,
    const std::unordered_map<std::string, json>& acs_rows_by_geoid,
    std::vector<LayerDef::FeatureRecord>& features,
    std::vector<LayerDef::FeatureProperties>& feature_properties);
void buildXlsxPointTableFeatures(
    const fs::path& xlsx_path,
    const std::string& sheet_name,
    const std::string& lon_field,
    const std::string& lat_field,
    std::vector<LayerDef::FeatureRecord>& features,
    std::vector<LayerDef::FeatureProperties>& feature_properties);
void buildJsonPointFeedFeatures(
    const fs::path& json_path,
    const std::string& item_path,
    const std::string& lon_field,
    const std::string& lat_field,
    const std::string& base_url,
    std::vector<LayerDef::FeatureRecord>& features,
    std::vector<LayerDef::FeatureProperties>& feature_properties);
void buildSocrataHowardPropertyFeatures(
    const fs::path& csv_path,
    std::vector<LayerDef::FeatureRecord>& features,
    std::vector<LayerDef::FeatureProperties>& feature_properties);
std::string jsonText(const json& obj, std::initializer_list<const char*> keys);
json buildJoinedCensusAcsProperties(
    const json& feature_props,
    const json& acs_props);
std::vector<std::pair<std::string, std::string>> parcelPropertyPairs(
    const std::map<std::string, std::string>& props,
    const std::string& jurisdiction,
    const std::string& source_file);
void appendFeatureWithProperties(
    LayerDef::FeatureRecord&& fg,
    const std::vector<std::pair<std::string, std::string>>& props,
    std::vector<LayerDef::FeatureRecord>& features,
    std::vector<LayerDef::FeatureProperties>& feature_properties);

uint16_t le16(const std::vector<uint8_t>& b, size_t off) {
    if (off + 2 > b.size()) throw std::runtime_error("unexpected EOF");
    return (uint16_t)b[off] | ((uint16_t)b[off + 1] << 8);
}

void buildCensusAcsGeoJsonFeatures(
    const fs::path& geojson_path,
    const std::unordered_map<std::string, json>& acs_rows_by_geoid,
    std::vector<LayerDef::FeatureRecord>& features,
    std::vector<LayerDef::FeatureProperties>& feature_properties) {
    std::ifstream in(geojson_path);
    if (!in) throw std::runtime_error("failed to open census geometry source " + geojson_path.string());
    json root;
    in >> root;
    const json* source_features =
        root.contains("features") && root["features"].is_array() ? &root["features"] : nullptr;
    if (!source_features) throw std::runtime_error("census geometry source is not a feature collection");
    for (auto feature : *source_features) {
        if (!feature.is_object() || !feature.contains("properties")) continue;
        json& props = feature["properties"];
        const std::string geoid = jsonText(props, {"GEOID", "GEOID20", "GEOID10"});
        if (geoid.empty()) continue;
        const auto it = acs_rows_by_geoid.find(geoid);
        if (it == acs_rows_by_geoid.end()) continue;
        props = buildJoinedCensusAcsProperties(props, it->second);
        appendGeoJsonFeatureRecords(feature, features, feature_properties);
    }
    if (features.empty()) throw std::runtime_error("joined Census/ACS import produced no features");
}

uint32_t le32(const std::vector<uint8_t>& b, size_t off) {
    if (off + 4 > b.size()) throw std::runtime_error("unexpected EOF");
    return (uint32_t)b[off] | ((uint32_t)b[off + 1] << 8) | ((uint32_t)b[off + 2] << 16) | ((uint32_t)b[off + 3] << 24);
}

int32_t be32s(const std::vector<uint8_t>& b, size_t off) {
    if (off + 4 > b.size()) throw std::runtime_error("unexpected EOF");
    uint32_t v = ((uint32_t)b[off] << 24) | ((uint32_t)b[off + 1] << 16) | ((uint32_t)b[off + 2] << 8) | (uint32_t)b[off + 3];
    return (int32_t)v;
}

int32_t le32s(const std::vector<uint8_t>& b, size_t off) { return (int32_t)le32(b, off); }

double leDouble(const std::vector<uint8_t>& b, size_t off) {
    if (off + 8 > b.size()) throw std::runtime_error("unexpected EOF");
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | b[off + i];
    double d = 0.0;
    std::memcpy(&d, &v, sizeof(double));
    return d;
}

std::string trim(std::string s) {
    auto space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && space((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && space((unsigned char)s.back())) s.pop_back();
    return s;
}

std::string collapseSpaces(std::string s) {
    std::string out;
    out.reserve(s.size());
    bool prev_space = false;
    for (unsigned char ch : s) {
        if (std::isspace(ch)) {
            if (!prev_space && !out.empty()) out.push_back(' ');
            prev_space = true;
        } else {
            out.push_back((char)ch);
            prev_space = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string normalizeLocationKey(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char ch : s) {
        if (std::isalnum(ch)) out.push_back((char)std::tolower(ch));
        else out.push_back(' ');
    }
    out = collapseSpaces(out);
    static const std::vector<std::pair<std::string, std::string>> replacements = {
        {" boulevard ", " blvd "},
        {" avenue ", " ave "},
        {" street ", " st "},
        {" road ", " rd "},
        {" drive ", " dr "},
        {" lane ", " ln "},
        {" place ", " pl "},
        {" suite ", " ste "},
        {" junior ", " jr "},
        {" maryland ", " md "},
        {" united states ", " us "}
    };
    std::string padded = " " + out + " ";
    for (const auto& repl : replacements) {
        size_t pos = 0;
        while ((pos = padded.find(repl.first, pos)) != std::string::npos) {
            padded.replace(pos, repl.first.size(), repl.second);
            pos += repl.second.size();
        }
    }
    return trim(collapseSpaces(padded));
}

std::vector<std::string> locationKeysForItem(const json& item) {
    std::vector<std::string> keys;
    const json* loc = item.contains("location") && item["location"].is_object() ? &item["location"] : nullptr;
    auto push_key = [&](const std::string& raw) {
        const std::string key = normalizeLocationKey(raw);
        if (key.empty()) return;
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) keys.push_back(key);
    };
    if (loc) {
        push_key(loc->value("name", ""));
        push_key(loc->value("address", ""));
        push_key(loc->value("geocode_query", ""));
    }
    return keys;
}

std::string urlOrigin(const std::string& url) {
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) return {};
    const size_t host_end = url.find('/', scheme + 3);
    if (host_end == std::string::npos) return url;
    return url.substr(0, host_end);
}

std::string resolveRelativeUrl(const std::string& url, const std::string& base_url) {
    if (url.empty()) return {};
    if (url.find("://") != std::string::npos) return url;
    if (!url.empty() && url[0] == '/') {
        const std::string origin = urlOrigin(base_url);
        if (!origin.empty()) return origin + url;
    }
    return url;
}

std::vector<uint8_t> readFileBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("failed to open " + p.string());
    in.seekg(0, std::ios::end);
    const auto n = in.tellg();
    in.seekg(0);
    std::vector<uint8_t> out((size_t)n);
    if (!out.empty()) in.read((char*)out.data(), (std::streamsize)out.size());
    return out;
}

size_t curlWriteToString(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    const size_t n = size * nmemb;
    out->append(static_cast<const char*>(ptr), n);
    return n;
}

std::string urlEncode(CURL* curl, const std::string& s) {
    char* enc = curl_easy_escape(curl, s.c_str(), (int)s.size());
    if (!enc) return {};
    std::string out(enc);
    curl_free(enc);
    return out;
}

HttpResponse httpPostForm(const std::string& url, const std::vector<std::pair<std::string, std::string>>& params) {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl init failed");
    std::string body;
    for (size_t i = 0; i < params.size(); ++i) {
        if (i > 0) body.push_back('&');
        body += urlEncode(curl, params[i].first);
        body.push_back('=');
        body += urlEncode(curl, params[i].second);
    }
    HttpResponse res;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "worldsim3/1.0");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 180L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &res.body);
    CURLcode rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res.code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) throw std::runtime_error(std::string("http failed: ") + curl_easy_strerror(rc));
    if (res.code < 200 || res.code >= 300) throw std::runtime_error("http code " + std::to_string(res.code));
    return res;
}

HttpResponse httpGet(const std::string& url) {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl init failed");
    HttpResponse res;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "worldsim3/1.0");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 180L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &res.body);
    CURLcode rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res.code);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) throw std::runtime_error(std::string("http failed: ") + curl_easy_strerror(rc));
    if (res.code < 200 || res.code >= 300) throw std::runtime_error("http code " + std::to_string(res.code));
    return res;
}

bool writeTextFileIfChanged(const fs::path& path, const std::string& body, bool* changed) {
    if (changed) *changed = true;
    std::error_code ec;
    if (fs::exists(path, ec) && !ec) {
        std::ifstream in(path, std::ios::binary);
        if (in) {
            std::ostringstream existing;
            existing << in.rdbuf();
            if (existing.str() == body) {
                if (changed) *changed = false;
                return true;
            }
        }
    }
    fs::create_directories(path.parent_path());
    const fs::path tmp = path.string() + ".tmp";
    std::ofstream out(tmp, std::ios::binary);
    if (!out) return false;
    out.write(body.data(), (std::streamsize)body.size());
    out.close();
    if (!out.good()) return false;
    fs::rename(tmp, path, ec);
    return !ec;
}

std::vector<std::vector<std::string>> parseCsv(const std::vector<uint8_t>& bytes) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string field;
    bool quoted = false;
    for (size_t i = 0; i < bytes.size(); ++i) {
        const char ch = (char)bytes[i];
        if (quoted) {
            if (ch == '"') {
                if (i + 1 < bytes.size() && (char)bytes[i + 1] == '"') {
                    field.push_back('"');
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                field.push_back(ch);
            }
            continue;
        }
        if (ch == '"') {
            quoted = true;
        } else if (ch == ',') {
            row.push_back(std::move(field));
            field.clear();
        } else if (ch == '\n') {
            row.push_back(std::move(field));
            field.clear();
            if (!row.empty() && !row.back().empty() && row.back().back() == '\r') row.back().pop_back();
            rows.push_back(std::move(row));
            row.clear();
        } else {
            field.push_back(ch);
        }
    }
    if (!field.empty() || !row.empty()) {
        row.push_back(std::move(field));
        if (!row.empty() && !row.back().empty() && row.back().back() == '\r') row.back().pop_back();
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<ZipEntry> readZipDirectory(const std::vector<uint8_t>& zip) {
    if (zip.size() < 22) throw std::runtime_error("zip too small");
    size_t eocd = std::string::npos;
    const size_t min_pos = zip.size() > 66000 ? zip.size() - 66000 : 0;
    for (size_t i = zip.size() - 22; i + 1 > min_pos; --i) {
        if (le32(zip, i) == 0x06054b50) { eocd = i; break; }
        if (i == 0) break;
    }
    if (eocd == std::string::npos) throw std::runtime_error("zip EOCD not found");
    const uint16_t count = le16(zip, eocd + 10);
    size_t cd = le32(zip, eocd + 16);
    std::vector<ZipEntry> entries;
    for (uint16_t i = 0; i < count; ++i) {
        if (le32(zip, cd) != 0x02014b50) throw std::runtime_error("invalid zip central directory");
        ZipEntry e;
        e.method = le16(zip, cd + 10);
        e.compressed_size = le32(zip, cd + 20);
        e.uncompressed_size = le32(zip, cd + 24);
        const uint16_t name_len = le16(zip, cd + 28);
        const uint16_t extra_len = le16(zip, cd + 30);
        const uint16_t comment_len = le16(zip, cd + 32);
        e.local_header_offset = le32(zip, cd + 42);
        if (cd + 46 + name_len > zip.size()) throw std::runtime_error("invalid zip name");
        e.name.assign((const char*)zip.data() + cd + 46, name_len);
        entries.push_back(std::move(e));
        cd += 46 + name_len + extra_len + comment_len;
    }
    return entries;
}

std::vector<uint8_t> extractZipEntry(const std::vector<uint8_t>& zip, const ZipEntry& e) {
    size_t lh = e.local_header_offset;
    if (le32(zip, lh) != 0x04034b50) throw std::runtime_error("invalid zip local header");
    const uint16_t name_len = le16(zip, lh + 26);
    const uint16_t extra_len = le16(zip, lh + 28);
    const size_t data_off = lh + 30 + name_len + extra_len;
    if (data_off + e.compressed_size > zip.size()) throw std::runtime_error("zip entry exceeds archive");
    std::vector<uint8_t> out(e.uncompressed_size);
    if (e.method == 0) {
        if (e.compressed_size != e.uncompressed_size) throw std::runtime_error("stored zip size mismatch");
        std::memcpy(out.data(), zip.data() + data_off, out.size());
    } else if (e.method == 8) {
        z_stream zs{};
        zs.next_in = const_cast<Bytef*>(zip.data() + data_off);
        zs.avail_in = e.compressed_size;
        zs.next_out = out.data();
        zs.avail_out = out.size();
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) throw std::runtime_error("inflate init failed");
        const int rc = inflate(&zs, Z_FINISH);
        inflateEnd(&zs);
        if (rc != Z_STREAM_END) throw std::runtime_error("inflate failed for " + e.name);
    } else {
        throw std::runtime_error("unsupported zip compression method " + std::to_string(e.method));
    }
    return out;
}

std::unordered_map<std::string, std::vector<uint8_t>> extractZipEntriesByName(const fs::path& zip_path) {
    const auto zip = readFileBytes(zip_path);
    const auto entries = readZipDirectory(zip);
    std::unordered_map<std::string, std::vector<uint8_t>> out;
    out.reserve(entries.size());
    for (const auto& e : entries) {
        out.emplace(e.name, extractZipEntry(zip, e));
    }
    return out;
}

std::unordered_map<std::string, std::vector<uint8_t>> extractShapefileMembers(const fs::path& zip_path, const std::string& shapefile) {
    const std::string stem = lower(fs::path(shapefile).stem().string());
    const auto zip = readFileBytes(zip_path);
    const auto entries = readZipDirectory(zip);
    std::unordered_map<std::string, std::vector<uint8_t>> out;
    for (const auto& e : entries) {
        const fs::path p(e.name);
        if (lower(p.stem().string()) != stem) continue;
        const std::string ext = lower(p.extension().string());
        if (ext == ".shp" || ext == ".dbf") out[ext] = extractZipEntry(zip, e);
    }
    if (!out.count(".shp")) throw std::runtime_error("zip missing " + stem + ".shp");
    if (!out.count(".dbf")) throw std::runtime_error("zip missing " + stem + ".dbf");
    return out;
}

std::string xmlUnescape(std::string s) {
    auto replace_all = [&](const std::string& needle, const std::string& repl) {
        size_t pos = 0;
        while ((pos = s.find(needle, pos)) != std::string::npos) {
            s.replace(pos, needle.size(), repl);
            pos += repl.size();
        }
    };
    replace_all("&amp;", "&");
    replace_all("&lt;", "<");
    replace_all("&gt;", ">");
    replace_all("&quot;", "\"");
    replace_all("&apos;", "'");
    return s;
}

std::string xmlAttribute(const std::string& tag, const std::string& key) {
    const std::string needle = key + "=\"";
    const size_t pos = tag.find(needle);
    if (pos == std::string::npos) return {};
    const size_t start = pos + needle.size();
    const size_t end = tag.find('"', start);
    if (end == std::string::npos) return {};
    return xmlUnescape(tag.substr(start, end - start));
}

std::string xmlFirstText(const std::string& xml, const std::string& tag_name) {
    size_t pos = 0;
    std::string out;
    while (true) {
        const size_t open = xml.find("<" + tag_name, pos);
        if (open == std::string::npos) break;
        const size_t open_end = xml.find('>', open);
        if (open_end == std::string::npos) break;
        const size_t close = xml.find("</" + tag_name + ">", open_end + 1);
        if (close == std::string::npos) break;
        out += xml.substr(open_end + 1, close - open_end - 1);
        pos = close + tag_name.size() + 3;
    }
    return xmlUnescape(out);
}

std::vector<std::string> parseXlsxSharedStrings(const std::unordered_map<std::string, std::vector<uint8_t>>& members) {
    auto it = members.find("xl/sharedStrings.xml");
    if (it == members.end()) return {};
    const std::string xml(it->second.begin(), it->second.end());
    std::vector<std::string> out;
    size_t pos = 0;
    while (true) {
        const size_t open = xml.find("<si", pos);
        if (open == std::string::npos) break;
        const size_t open_end = xml.find('>', open);
        const size_t close = xml.find("</si>", open_end + 1);
        if (open_end == std::string::npos || close == std::string::npos) break;
        out.push_back(xmlFirstText(xml.substr(open_end + 1, close - open_end - 1), "t"));
        pos = close + 5;
    }
    return out;
}

std::string parseFirstWorkbookSheetTarget(const std::unordered_map<std::string, std::vector<uint8_t>>& members, const std::string& preferred_sheet_name) {
    auto workbook_it = members.find("xl/workbook.xml");
    auto rels_it = members.find("xl/_rels/workbook.xml.rels");
    if (workbook_it == members.end() || rels_it == members.end()) return "xl/worksheets/sheet1.xml";
    const std::string workbook_xml(workbook_it->second.begin(), workbook_it->second.end());
    const std::string rels_xml(rels_it->second.begin(), rels_it->second.end());
    std::string desired_rid;
    size_t pos = 0;
    while (true) {
        const size_t open = workbook_xml.find("<sheet", pos);
        if (open == std::string::npos) break;
        const size_t end = workbook_xml.find('>', open);
        if (end == std::string::npos) break;
        const std::string tag = workbook_xml.substr(open, end - open + 1);
        const std::string rid = xmlAttribute(tag, "r:id");
        const std::string name = xmlAttribute(tag, "name");
        if (desired_rid.empty() || (!preferred_sheet_name.empty() && name == preferred_sheet_name)) {
            desired_rid = rid;
            if (!preferred_sheet_name.empty() && name == preferred_sheet_name) break;
        }
        pos = end + 1;
    }
    if (desired_rid.empty()) return "xl/worksheets/sheet1.xml";
    pos = 0;
    while (true) {
        const size_t open = rels_xml.find("<Relationship", pos);
        if (open == std::string::npos) break;
        const size_t end = rels_xml.find("/>", open);
        if (end == std::string::npos) break;
        const std::string tag = rels_xml.substr(open, end - open + 2);
        if (xmlAttribute(tag, "Id") == desired_rid) {
            std::string target = xmlAttribute(tag, "Target");
            if (target.rfind("/xl/", 0) == 0) return target.substr(1);
            if (target.rfind("xl/", 0) == 0) return target;
            if (target.rfind("worksheets/", 0) == 0) return "xl/" + target;
            return "xl/" + target;
        }
        pos = end + 2;
    }
    return "xl/worksheets/sheet1.xml";
}

int xlsxColumnIndex(const std::string& cell_ref) {
    int idx = 0;
    bool saw_alpha = false;
    for (char ch : cell_ref) {
        if (std::isalpha((unsigned char)ch)) {
            saw_alpha = true;
            idx = idx * 26 + (std::toupper((unsigned char)ch) - 'A' + 1);
        } else {
            break;
        }
    }
    return saw_alpha ? (idx - 1) : -1;
}

bool tryParseDouble(const std::string& text, double& out) {
    try {
        size_t used = 0;
        out = std::stod(trim(text), &used);
        return used > 0;
    } catch (...) {
        return false;
    }
}

void appendGeoJsonFeatureRecords(
    const json& feature,
    std::vector<LayerDef::FeatureRecord>& features,
    std::vector<LayerDef::FeatureProperties>& feature_properties) {
    if (!feature.is_object() || !feature.contains("geometry")) return;
    const std::vector<LayerDef::FeatureRecord> records = extractFeatureRecords(feature["geometry"]);
    if (records.empty()) return;
    std::vector<std::pair<std::string, std::string>> props;
    if (feature.contains("properties") && feature["properties"].is_object()) {
        props.reserve(feature["properties"].size());
        for (auto it = feature["properties"].begin(); it != feature["properties"].end(); ++it) {
            props.push_back({it.key(), jsonValueToString(it.value())});
        }
    }
    for (auto record : records) appendFeatureWithProperties(std::move(record), props, features, feature_properties);
}

void writeXlsxPointTableGeoJson(
    const fs::path& xlsx_path,
    const fs::path& out_path,
    const std::string& sheet_name,
    const std::string& lon_field,
    const std::string& lat_field) {
    const auto members = extractZipEntriesByName(xlsx_path);
    const std::vector<std::string> shared_strings = parseXlsxSharedStrings(members);
    const std::string sheet_path = parseFirstWorkbookSheetTarget(members, sheet_name);
    auto sheet_it = members.find(sheet_path);
    if (sheet_it == members.end()) throw std::runtime_error("xlsx missing worksheet " + sheet_path);
    const std::string xml(sheet_it->second.begin(), sheet_it->second.end());

    std::vector<std::vector<std::string>> rows;
    size_t pos = 0;
    while (true) {
        const size_t row_open = xml.find("<row", pos);
        if (row_open == std::string::npos) break;
        const size_t row_open_end = xml.find('>', row_open);
        const size_t row_close = xml.find("</row>", row_open_end + 1);
        if (row_open_end == std::string::npos || row_close == std::string::npos) break;
        const std::string row_xml = xml.substr(row_open_end + 1, row_close - row_open_end - 1);
        std::vector<std::string> row;
        size_t cell_pos = 0;
        while (true) {
            const size_t cell_open = row_xml.find("<c", cell_pos);
            if (cell_open == std::string::npos) break;
            const size_t cell_open_end = row_xml.find('>', cell_open);
            if (cell_open_end == std::string::npos) break;
            const bool self_closing = cell_open_end > cell_open && row_xml[cell_open_end - 1] == '/';
            const std::string cell_tag = row_xml.substr(cell_open, cell_open_end - cell_open + 1);
            const int col_idx = xlsxColumnIndex(xmlAttribute(cell_tag, "r"));
            if (col_idx >= 0 && (size_t)(col_idx + 1) > row.size()) row.resize((size_t)col_idx + 1);
            std::string value;
            size_t next_pos = cell_open_end + 1;
            if (!self_closing) {
                const size_t cell_close = row_xml.find("</c>", cell_open_end + 1);
                if (cell_close == std::string::npos) break;
                const std::string cell_inner = row_xml.substr(cell_open_end + 1, cell_close - cell_open_end - 1);
                const std::string type = xmlAttribute(cell_tag, "t");
                if (type == "s") {
                    const std::string raw = xmlFirstText(cell_inner, "v");
                    double idx_num = 0.0;
                    if (tryParseDouble(raw, idx_num)) {
                        const size_t idx = (size_t)idx_num;
                        if (idx < shared_strings.size()) value = shared_strings[idx];
                    }
                } else if (type == "inlineStr") {
                    value = xmlFirstText(cell_inner, "t");
                } else {
                    value = xmlFirstText(cell_inner, "v");
                }
                next_pos = cell_close + 4;
            }
            if (col_idx >= 0) row[(size_t)col_idx] = trim(value);
            cell_pos = next_pos;
        }
        bool non_empty = false;
        for (const auto& field : row) {
            if (!field.empty()) {
                non_empty = true;
                break;
            }
        }
        if (non_empty) rows.push_back(std::move(row));
        pos = row_close + 6;
    }

    if (rows.empty()) throw std::runtime_error("xlsx worksheet has no rows");
    const std::vector<std::string>& headers = rows.front();
    if (headers.empty()) throw std::runtime_error("xlsx worksheet header row is empty");

    auto find_header_index = [&](const std::string& configured_name, std::initializer_list<const char*> fallbacks) -> int {
        if (!configured_name.empty()) {
            for (size_t i = 0; i < headers.size(); ++i) {
                if (trim(headers[i]) == configured_name) return (int)i;
            }
        }
        for (const char* candidate : fallbacks) {
            for (size_t i = 0; i < headers.size(); ++i) {
                if (lower(trim(headers[i])) == lower(candidate)) return (int)i;
            }
        }
        return -1;
    };

    const int lon_idx = find_header_index(lon_field, {"longitude", "lon", "x"});
    const int lat_idx = find_header_index(lat_field, {"latitude", "lat", "y"});
    if (lon_idx < 0 || lat_idx < 0) throw std::runtime_error("xlsx worksheet is missing configured lon/lat columns");

    json collection;
    collection["type"] = "FeatureCollection";
    collection["name"] = out_path.stem().string();
    collection["features"] = json::array();

    for (size_t row_idx = 1; row_idx < rows.size(); ++row_idx) {
        const auto& row = rows[row_idx];
        const auto field_at = [&](int idx) -> std::string {
            return (idx >= 0 && (size_t)idx < row.size()) ? trim(row[(size_t)idx]) : std::string();
        };
        double lon = 0.0;
        double lat = 0.0;
        if (!tryParseDouble(field_at(lon_idx), lon) || !tryParseDouble(field_at(lat_idx), lat)) continue;
        json feature;
        feature["type"] = "Feature";
        feature["geometry"] = {
            {"type", "Point"},
            {"coordinates", {lon, lat}}
        };
        json props = json::object();
        for (size_t i = 0; i < headers.size(); ++i) {
            const std::string key = trim(headers[i]);
            if (key.empty()) continue;
            props[key] = i < row.size() ? trim(row[i]) : "";
        }
        feature["properties"] = std::move(props);
        collection["features"].push_back(std::move(feature));
    }

    if (collection["features"].empty()) throw std::runtime_error("xlsx worksheet produced no point features");
    fs::create_directories(out_path.parent_path());
    std::ofstream out(out_path);
    if (!out) throw std::runtime_error("failed to open output " + out_path.string());
    out << collection.dump();
    out << "\n";
}

void buildXlsxPointTableFeatures(
    const fs::path& xlsx_path,
    const std::string& sheet_name,
    const std::string& lon_field,
    const std::string& lat_field,
    std::vector<LayerDef::FeatureRecord>& features,
    std::vector<LayerDef::FeatureProperties>& feature_properties) {
    const auto members = extractZipEntriesByName(xlsx_path);
    const std::vector<std::string> shared_strings = parseXlsxSharedStrings(members);
    const std::string sheet_path = parseFirstWorkbookSheetTarget(members, sheet_name);
    auto sheet_it = members.find(sheet_path);
    if (sheet_it == members.end()) throw std::runtime_error("xlsx missing worksheet " + sheet_path);
    const std::string xml(sheet_it->second.begin(), sheet_it->second.end());

    std::vector<std::vector<std::string>> rows;
    size_t pos = 0;
    while (true) {
        const size_t row_open = xml.find("<row", pos);
        if (row_open == std::string::npos) break;
        const size_t row_open_end = xml.find('>', row_open);
        const size_t row_close = xml.find("</row>", row_open_end + 1);
        if (row_open_end == std::string::npos || row_close == std::string::npos) break;
        const std::string row_xml = xml.substr(row_open_end + 1, row_close - row_open_end - 1);
        std::vector<std::string> row;
        size_t cell_pos = 0;
        while (true) {
            const size_t cell_open = row_xml.find("<c", cell_pos);
            if (cell_open == std::string::npos) break;
            const size_t cell_open_end = row_xml.find('>', cell_open);
            if (cell_open_end == std::string::npos) break;
            const bool self_closing = cell_open_end > cell_open && row_xml[cell_open_end - 1] == '/';
            const std::string cell_tag = row_xml.substr(cell_open, cell_open_end - cell_open + 1);
            const int col_idx = xlsxColumnIndex(xmlAttribute(cell_tag, "r"));
            if (col_idx >= 0 && (size_t)(col_idx + 1) > row.size()) row.resize((size_t)col_idx + 1);
            std::string value;
            size_t next_pos = cell_open_end + 1;
            if (!self_closing) {
                const size_t cell_close = row_xml.find("</c>", cell_open_end + 1);
                if (cell_close == std::string::npos) break;
                const std::string cell_inner = row_xml.substr(cell_open_end + 1, cell_close - cell_open_end - 1);
                const std::string type = xmlAttribute(cell_tag, "t");
                if (type == "s") {
                    const std::string raw = xmlFirstText(cell_inner, "v");
                    double idx_num = 0.0;
                    if (tryParseDouble(raw, idx_num)) {
                        const size_t idx = (size_t)idx_num;
                        if (idx < shared_strings.size()) value = shared_strings[idx];
                    }
                } else if (type == "inlineStr") {
                    value = xmlFirstText(cell_inner, "t");
                } else {
                    value = xmlFirstText(cell_inner, "v");
                }
                next_pos = cell_close + 4;
            }
            if (col_idx >= 0) row[(size_t)col_idx] = trim(value);
            cell_pos = next_pos;
        }
        bool non_empty = false;
        for (const auto& field : row) {
            if (!field.empty()) {
                non_empty = true;
                break;
            }
        }
        if (non_empty) rows.push_back(std::move(row));
        pos = row_close + 6;
    }

    if (rows.empty()) throw std::runtime_error("xlsx worksheet has no rows");
    const std::vector<std::string>& headers = rows.front();
    if (headers.empty()) throw std::runtime_error("xlsx worksheet header row is empty");

    auto find_header_index = [&](const std::string& configured_name, std::initializer_list<const char*> fallbacks) -> int {
        if (!configured_name.empty()) {
            for (size_t i = 0; i < headers.size(); ++i) {
                if (trim(headers[i]) == configured_name) return (int)i;
            }
        }
        for (const char* candidate : fallbacks) {
            for (size_t i = 0; i < headers.size(); ++i) {
                if (lower(trim(headers[i])) == lower(candidate)) return (int)i;
            }
        }
        return -1;
    };

    const int lon_idx = find_header_index(lon_field, {"longitude", "lon", "x"});
    const int lat_idx = find_header_index(lat_field, {"latitude", "lat", "y"});
    if (lon_idx < 0 || lat_idx < 0) throw std::runtime_error("xlsx worksheet is missing configured lon/lat columns");

    for (size_t row_idx = 1; row_idx < rows.size(); ++row_idx) {
        const auto& row = rows[row_idx];
        const auto field_at = [&](int idx) -> std::string {
            return (idx >= 0 && (size_t)idx < row.size()) ? trim(row[(size_t)idx]) : std::string();
        };
        double lon = 0.0;
        double lat = 0.0;
        if (!tryParseDouble(field_at(lon_idx), lon) || !tryParseDouble(field_at(lat_idx), lat)) continue;
        LayerDef::FeatureRecord fg{};
        fg.extent.min_lon = fg.extent.max_lon = (float)lon;
        fg.extent.min_lat = fg.extent.max_lat = (float)lat;
        std::vector<std::pair<std::string, std::string>> props;
        props.reserve(headers.size());
        for (size_t i = 0; i < headers.size(); ++i) {
            const std::string key = trim(headers[i]);
            if (key.empty()) continue;
            props.push_back({key, i < row.size() ? trim(row[i]) : ""});
        }
        appendFeatureWithProperties(std::move(fg), props, features, feature_properties);
    }
    if (features.empty()) throw std::runtime_error("xlsx worksheet produced no point features");
}

std::vector<std::map<std::string, std::string>> parseDbf(const std::vector<uint8_t>& dbf) {
    if (dbf.size() < 32) throw std::runtime_error("dbf too small");
    const uint32_t record_count = le32(dbf, 4);
    const uint16_t header_len = le16(dbf, 8);
    const uint16_t record_len = le16(dbf, 10);
    std::vector<DbfField> fields;
    for (size_t off = 32; off + 32 <= header_len && dbf[off] != 0x0d; off += 32) {
        DbfField f;
        size_t n = 0;
        while (n < 11 && dbf[off + n] != 0) n++;
        f.name.assign((const char*)dbf.data() + off, n);
        f.type = (char)dbf[off + 11];
        f.length = dbf[off + 16];
        if (!f.name.empty() && f.length > 0) fields.push_back(std::move(f));
    }
    std::vector<std::map<std::string, std::string>> records;
    records.reserve(record_count);
    for (uint32_t r = 0; r < record_count; ++r) {
        size_t off = header_len + (size_t)r * record_len;
        if (off + record_len > dbf.size()) break;
        std::map<std::string, std::string> props;
        if (dbf[off] == '*') { records.push_back(std::move(props)); continue; }
        size_t pos = off + 1;
        for (const auto& f : fields) {
            if (pos + f.length > off + record_len) break;
            std::string val((const char*)dbf.data() + pos, f.length);
            val = trim(val);
            props[f.name] = val;
            pos += f.length;
        }
        records.push_back(std::move(props));
    }
    return records;
}

PointD marylandStatePlaneToLonLat(double x_src, double y_src, bool us_feet) {
    constexpr double pi = 3.14159265358979323846;
    constexpr double a = 6378137.0;
    constexpr double inv_f = 298.257222101;
    constexpr double f = 1.0 / inv_f;
    constexpr double e = std::sqrt(f * (2.0 - f));
    constexpr double ft = 1200.0 / 3937.0;
    constexpr double phi1 = 38.3 * pi / 180.0;
    constexpr double phi2 = 39.45 * pi / 180.0;
    constexpr double phi0 = (37.0 + 40.0 / 60.0) * pi / 180.0;
    constexpr double lam0 = -77.0 * pi / 180.0;
    constexpr double fe = 400000.0; // meters, equivalent to 1,312,333.333 US ft
    constexpr double fn = 0.0;
    auto m = [](double phi) { return std::cos(phi) / std::sqrt(1.0 - e * e * std::sin(phi) * std::sin(phi)); };
    auto t = [](double phi) {
        const double s = std::sin(phi);
        return std::tan(pi / 4.0 - phi / 2.0) / std::pow((1.0 - e * s) / (1.0 + e * s), e / 2.0);
    };
    const double n = (std::log(m(phi1)) - std::log(m(phi2))) / (std::log(t(phi1)) - std::log(t(phi2)));
    const double F = m(phi1) / (n * std::pow(t(phi1), n));
    const double rho0 = a * F * std::pow(t(phi0), n);
    const double x = us_feet ? (x_src * ft) : x_src;
    const double y = us_feet ? (y_src * ft) : y_src;
    const double dx = x - fe;
    const double dy = rho0 - (y - fn);
    const double rho = std::copysign(std::sqrt(dx * dx + dy * dy), n);
    const double theta = std::atan2(dx, dy);
    const double tt = std::pow(rho / (a * F), 1.0 / n);
    double phi = pi / 2.0 - 2.0 * std::atan(tt);
    for (int i = 0; i < 8; ++i) {
        const double s = std::sin(phi);
        phi = pi / 2.0 - 2.0 * std::atan(tt * std::pow((1.0 - e * s) / (1.0 + e * s), e / 2.0));
    }
    const double lam = lam0 + theta / n;
    return {lam * 180.0 / pi, phi * 180.0 / pi};
}

PointD maryland2248ToLonLat(double x_ft, double y_ft) {
    return marylandStatePlaneToLonLat(x_ft, y_ft, true);
}

PointD maryland26985ToLonLat(double x_m, double y_m) {
    return marylandStatePlaneToLonLat(x_m, y_m, false);
}

std::string jsonEscape(const std::string& s) {
    std::ostringstream os;
    for (unsigned char c : s) {
        switch (c) {
            case '"': os << "\\\""; break;
            case '\\': os << "\\\\"; break;
            case '\b': os << "\\b"; break;
            case '\f': os << "\\f"; break;
            case '\n': os << "\\n"; break;
            case '\r': os << "\\r"; break;
            case '\t': os << "\\t"; break;
            default:
                if (c < 0x20) os << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (int)c << std::dec;
                else os << (char)c;
        }
    }
    return os.str();
}

void writeParcelProperties(
    std::ostream& out,
    const std::map<std::string, std::string>& props,
    const std::string& jurisdiction,
    const std::string& source_file) {
    bool first = true;
    auto put = [&](const std::string& k, const std::string& v) {
        if (!first) out << ',';
        first = false;
        out << '"' << jsonEscape(k) << "\":\"" << jsonEscape(v) << '"';
    };
    out << '{';
    for (const auto& kv : parcelPropertyPairs(props, jurisdiction, source_file)) put(kv.first, kv.second);
    out << '}';
}

void writeStateplaneParcelShapefileGeoJson(
    const std::vector<uint8_t>& shp,
    const std::vector<std::map<std::string, std::string>>& dbf,
    const fs::path& out_path,
    const std::string& collection_name,
    const std::string& jurisdiction,
    const std::string& source_file,
    bool us_feet) {
    if (shp.size() < 100 || be32s(shp, 0) != 9994) throw std::runtime_error("invalid shapefile header");
    fs::create_directories(out_path.parent_path());
    fs::path tmp = out_path;
    tmp += ".part";
    std::ofstream out(tmp);
    if (!out) throw std::runtime_error("failed to open output " + tmp.string());
    out << std::setprecision(10);
    out << "{\"type\":\"FeatureCollection\",\"name\":\"" << jsonEscape(collection_name) << "\",\"features\":[";
    bool first_feature = true;
    size_t record_index = 0;
    for (size_t off = 100; off + 8 <= shp.size();) {
        const int32_t content_words = be32s(shp, off + 4);
        const size_t content_off = off + 8;
        const size_t content_len = (size_t)content_words * 2;
        off = content_off + content_len;
        if (content_off + content_len > shp.size() || content_len < 4) break;
        const int32_t shape_type = le32s(shp, content_off);
        if (shape_type == 0) { record_index++; continue; }
        if (shape_type != 5 && shape_type != 15) { record_index++; continue; }
        if (content_len < 44) { record_index++; continue; }
        const int32_t num_parts = le32s(shp, content_off + 36);
        const int32_t num_points = le32s(shp, content_off + 40);
        if (num_parts <= 0 || num_points <= 0) { record_index++; continue; }
        const size_t parts_off = content_off + 44;
        const size_t points_off = parts_off + (size_t)num_parts * 4;
        if (points_off + (size_t)num_points * 16 > content_off + content_len) { record_index++; continue; }
        if (!first_feature) out << ',';
        first_feature = false;
        out << "{\"type\":\"Feature\",\"properties\":";
        if (record_index < dbf.size()) writeParcelProperties(out, dbf[record_index], jurisdiction, source_file);
        else out << "{\"jurisdiction\":\"" << jsonEscape(jurisdiction) << "\",\"source_file\":\"" << jsonEscape(source_file) << "\"}";
        out << ",\"geometry\":{\"type\":\"MultiPolygon\",\"coordinates\":[";
        for (int32_t part = 0; part < num_parts; ++part) {
            if (part > 0) out << ',';
            const int32_t start = le32s(shp, parts_off + (size_t)part * 4);
            const int32_t end = (part + 1 < num_parts) ? le32s(shp, parts_off + (size_t)(part + 1) * 4) : num_points;
            out << "[[";
            for (int32_t pi = start; pi < end; ++pi) {
                if (pi > start) out << ',';
                const double x = leDouble(shp, points_off + (size_t)pi * 16);
                const double y = leDouble(shp, points_off + (size_t)pi * 16 + 8);
                const PointD ll = us_feet ? maryland2248ToLonLat(x, y) : maryland26985ToLonLat(x, y);
                out << '[' << ll.x << ',' << ll.y << ']';
            }
            out << "]]";
        }
        out << "]}}";
        record_index++;
    }
    out << "]}\n";
    out.close();
    std::error_code ec;
    fs::rename(tmp, out_path, ec);
    if (ec) throw std::runtime_error("rename failed: " + ec.message());
}

void writeHowardShapefileGeoJson(const std::vector<uint8_t>& shp, const std::vector<std::map<std::string, std::string>>& dbf, const fs::path& out_path) {
    writeStateplaneParcelShapefileGeoJson(
        shp,
        dbf,
        out_path,
        "howard_county_parcels",
        "Howard County",
        "Property.shp",
        true);
}

std::vector<std::pair<std::string, std::string>> parcelPropertyPairs(
    const std::map<std::string, std::string>& props,
    const std::string& jurisdiction,
    const std::string& source_file) {
    std::vector<std::pair<std::string, std::string>> out;
    out.emplace_back("jurisdiction", jurisdiction);
    out.emplace_back("source_file", source_file);
    for (const auto& kv : props) out.push_back(kv);
    auto get = [&](std::initializer_list<const char*> keys) -> std::string {
        for (const char* k : keys) {
            auto it = props.find(k);
            if (it != props.end() && !it->second.empty()) return it->second;
        }
        return {};
    };
    const std::string acct = get({"ACCTID", "ACCOUNTID", "ACCOUNT_ID"});
    const std::string parcel = get({"PARCEL", "MAP", "LOT"});
    out.emplace_back("source_parcel_id", !acct.empty() ? acct : parcel);
    out.emplace_back("account_id", acct);
    out.emplace_back("blocklot", !acct.empty() ? acct : parcel);
    out.emplace_back("address", get({"ADDRESS", "ADDR", "PREMISE_ADDRESS"}));
    out.emplace_back("owner", get({"OWNNAME1", "OWNER", "OWNER_NAME"}));
    out.emplace_back("sdat_link", get({"SDAT_Link", "SDAT_LINK"}));
    return out;
}

void appendFeatureWithProperties(
    LayerDef::FeatureRecord&& fg,
    const std::vector<std::pair<std::string, std::string>>& props,
    std::vector<LayerDef::FeatureRecord>& features,
    std::vector<LayerDef::FeatureProperties>& feature_properties) {
    feature_properties.push_back(LayerDef::FeatureProperties{props});
    features.push_back(std::move(fg));
}

void buildStateplaneParcelShapefileFeatures(
    const std::vector<uint8_t>& shp,
    const std::vector<std::map<std::string, std::string>>& dbf,
    const std::string& jurisdiction,
    const std::string& source_file,
    bool us_feet,
    std::vector<LayerDef::FeatureRecord>& features,
    std::vector<LayerDef::FeatureProperties>& feature_properties) {
    if (shp.size() < 100 || be32s(shp, 0) != 9994) throw std::runtime_error("invalid shapefile header");
    size_t record_index = 0;
    for (size_t off = 100; off + 8 <= shp.size();) {
        const int32_t content_words = be32s(shp, off + 4);
        const size_t content_off = off + 8;
        const size_t content_len = (size_t)content_words * 2;
        off = content_off + content_len;
        if (content_off + content_len > shp.size() || content_len < 4) break;
        const int32_t shape_type = le32s(shp, content_off);
        if (shape_type == 0) { record_index++; continue; }
        if (shape_type != 5 && shape_type != 15) { record_index++; continue; }
        if (content_len < 44) { record_index++; continue; }
        const int32_t num_parts = le32s(shp, content_off + 36);
        const int32_t num_points = le32s(shp, content_off + 40);
        if (num_parts <= 0 || num_points <= 0) { record_index++; continue; }
        const size_t parts_off = content_off + 44;
        const size_t points_off = parts_off + (size_t)num_parts * 4;
        if (points_off + (size_t)num_points * 16 > content_off + content_len) { record_index++; continue; }

        LayerDef::FeatureRecord fg{};
        fg.rings.reserve((size_t)num_parts);
        bool has_extent = false;
        for (int32_t part = 0; part < num_parts; ++part) {
            const int32_t start = le32s(shp, parts_off + (size_t)part * 4);
            const int32_t end = (part + 1 < num_parts) ? le32s(shp, parts_off + (size_t)(part + 1) * 4) : num_points;
            std::vector<ImVec2> ring;
            ring.reserve((size_t)std::max(0, end - start));
            for (int32_t pi = start; pi < end; ++pi) {
                const double x = leDouble(shp, points_off + (size_t)pi * 16);
                const double y = leDouble(shp, points_off + (size_t)pi * 16 + 8);
                const PointD ll = us_feet ? maryland2248ToLonLat(x, y) : maryland26985ToLonLat(x, y);
                ring.emplace_back((float)ll.x, (float)ll.y);
                if (!has_extent) {
                    fg.extent.min_lon = fg.extent.max_lon = (float)ll.x;
                    fg.extent.min_lat = fg.extent.max_lat = (float)ll.y;
                    has_extent = true;
                } else {
                    fg.extent.min_lon = std::min(fg.extent.min_lon, (float)ll.x);
                    fg.extent.min_lat = std::min(fg.extent.min_lat, (float)ll.y);
                    fg.extent.max_lon = std::max(fg.extent.max_lon, (float)ll.x);
                    fg.extent.max_lat = std::max(fg.extent.max_lat, (float)ll.y);
                }
            }
            if (!ring.empty()) fg.rings.push_back(std::move(ring));
        }
        if (fg.rings.empty()) { record_index++; continue; }
        const std::vector<std::pair<std::string, std::string>> props =
            record_index < dbf.size()
                ? parcelPropertyPairs(dbf[record_index], jurisdiction, source_file)
                : std::vector<std::pair<std::string, std::string>>{
                    {"jurisdiction", jurisdiction},
                    {"source_file", source_file}
                };
        appendFeatureWithProperties(std::move(fg), props, features, feature_properties);
        record_index++;
    }
}

void writeSocrataHowardPropertyGeoJson(const fs::path& csv_path, const fs::path& out_path) {
    const auto rows = parseCsv(readFileBytes(csv_path));
    if (rows.empty()) throw std::runtime_error("Socrata CSV is empty");
    std::unordered_map<std::string, size_t> col;
    for (size_t i = 0; i < rows.front().size(); ++i) col[rows.front()[i]] = i;
    auto get = [&](const std::vector<std::string>& row, const char* name) -> std::string {
        auto it = col.find(name);
        if (it == col.end() || it->second >= row.size()) return {};
        return row[it->second];
    };
    auto put = [](std::ostream& out, bool& first, const std::string& k, const std::string& v) {
        if (!first) out << ',';
        first = false;
        out << '"' << jsonEscape(k) << "\":\"" << jsonEscape(v) << '"';
    };

    fs::create_directories(out_path.parent_path());
    fs::path tmp = out_path;
    tmp += ".part";
    std::ofstream out(tmp);
    if (!out) throw std::runtime_error("failed to open output " + tmp.string());
    out << "{\"type\":\"FeatureCollection\",\"name\":\"howard_county_real_property_assessments\",\"features\":[";
    bool first_feature = true;
    for (size_t r = 1; r < rows.size(); ++r) {
        const auto& row = rows[r];
        const std::string acct = get(row, "account_id_mdp_field_acctid");
        if (acct.empty()) continue;
        if (!first_feature) out << ',';
        first_feature = false;
        out << "{\"type\":\"Feature\",\"properties\":{";
        bool first_prop = true;
        put(out, first_prop, "jurisdiction", "Howard County");
        put(out, first_prop, "source_file", "Maryland Real Property Assessments");
        for (const auto& [name, idx] : col) {
            if (idx < row.size()) put(out, first_prop, name, row[idx]);
        }
        put(out, first_prop, "source_parcel_id", acct);
        put(out, first_prop, "account_id", acct);
        put(out, first_prop, "blocklot", acct);
        put(out, first_prop, "address", get(row, "mdp_street_address_mdp_field_address"));
        put(out, first_prop, "owner", "");
        put(out, first_prop, "land_value", get(row, "current_cycle_data_land_value_mdp_field_names_nfmlndvl_curlndvl_and_sallndvl_sdat_field_164"));
        put(out, first_prop, "improvement_value", get(row, "current_cycle_data_improvements_value_mdp_field_names_nfmimpvl_curimpvl_and_salimpvl_sdat_field_165"));
        put(out, first_prop, "current_value", get(row, "current_assessment_year_total_assessment_sdat_field_172"));
        put(out, first_prop, "sale_price", get(row, "sales_segment_1_consideration_mdp_field_considr1_sdat_field_90"));
        put(out, first_prop, "sale_date", get(row, "sales_segment_1_transfer_date_yyyy_mm_dd_mdp_field_tradate_sdat_field_89"));
        put(out, first_prop, "year_built", get(row, "c_a_m_a_system_data_year_built_yyyy_mdp_field_yearblt_sdat_field_235"));
        put(out, first_prop, "sdat_link", get(row, "real_property_search_link"));
        put(out, first_prop, "finder_online_link", get(row, "finder_online_link"));
        out << "},\"geometry\":null}";
    }
    out << "]}\n";
    out.close();
    std::error_code ec;
    fs::rename(tmp, out_path, ec);
    if (ec) throw std::runtime_error("rename failed: " + ec.message());
}

void buildSocrataHowardPropertyFeatures(
    const fs::path& csv_path,
    std::vector<LayerDef::FeatureRecord>& features,
    std::vector<LayerDef::FeatureProperties>& feature_properties) {
    const auto rows = parseCsv(readFileBytes(csv_path));
    if (rows.empty()) throw std::runtime_error("Socrata CSV is empty");
    std::unordered_map<std::string, size_t> col;
    for (size_t i = 0; i < rows.front().size(); ++i) col[rows.front()[i]] = i;
    auto get = [&](const std::vector<std::string>& row, const char* name) -> std::string {
        auto it = col.find(name);
        if (it == col.end() || it->second >= row.size()) return {};
        return row[it->second];
    };

    for (size_t r = 1; r < rows.size(); ++r) {
        const auto& row = rows[r];
        const std::string acct = get(row, "account_id_mdp_field_acctid");
        if (acct.empty()) continue;
        LayerDef::FeatureRecord fg{};
        std::vector<std::pair<std::string, std::string>> props;
        props.reserve(col.size() + 16);
        props.push_back({"jurisdiction", "Howard County"});
        props.push_back({"source_file", "Maryland Real Property Assessments"});
        for (const auto& [name, idx] : col) {
            if (idx < row.size()) props.push_back({name, row[idx]});
        }
        props.push_back({"source_parcel_id", acct});
        props.push_back({"account_id", acct});
        props.push_back({"blocklot", acct});
        props.push_back({"address", get(row, "mdp_street_address_mdp_field_address")});
        props.push_back({"owner", ""});
        props.push_back({"land_value", get(row, "current_cycle_data_land_value_mdp_field_names_nfmlndvl_curlndvl_and_sallndvl_sdat_field_164")});
        props.push_back({"improvement_value", get(row, "current_cycle_data_improvements_value_mdp_field_names_nfmimpvl_curimpvl_and_salimpvl_sdat_field_165")});
        props.push_back({"current_value", get(row, "current_assessment_year_total_assessment_sdat_field_172")});
        props.push_back({"sale_price", get(row, "sales_segment_1_consideration_mdp_field_considr1_sdat_field_90")});
        props.push_back({"sale_date", get(row, "sales_segment_1_transfer_date_yyyy_mm_dd_mdp_field_tradate_sdat_field_89")});
        props.push_back({"year_built", get(row, "c_a_m_a_system_data_year_built_yyyy_mdp_field_yearblt_sdat_field_235")});
        props.push_back({"sdat_link", get(row, "real_property_search_link")});
        props.push_back({"finder_online_link", get(row, "finder_online_link")});
        appendFeatureWithProperties(std::move(fg), props, features, feature_properties);
    }
    if (features.empty()) throw std::runtime_error("Socrata CSV produced no assessment records");
}

std::string jsonText(const json& props, std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        auto it = props.find(key);
        if (it == props.end() || it->is_null()) continue;
        if (it->is_string()) {
            std::string s = trim(it->get<std::string>());
            if (!s.empty()) return s;
        } else if (it->is_number_integer()) {
            return std::to_string(it->get<long long>());
        } else if (it->is_number_unsigned()) {
            return std::to_string(it->get<unsigned long long>());
        } else if (it->is_number_float()) {
            std::ostringstream os;
            os << it->get<double>();
            return os.str();
        }
    }
    return {};
}

void normalizeBaltimoreCountyArcgisFeature(json& feature) {
    if (!feature.contains("properties") || !feature["properties"].is_object()) return;
    json& props = feature["properties"];
    const std::string taxpin = jsonText(props, {"TAXPIN"});
    std::string owner = jsonText(props, {"FULL_OWNER_NAME", "OWNER_NA1", "OWNER"});
    const std::string owner2 = jsonText(props, {"OWNER_NA2"});
    if (!owner.empty() && !owner2.empty() && lower(owner).find(lower(owner2)) == std::string::npos) {
        owner += " " + owner2;
    }
    std::string address = jsonText(props, {"PREMISE_ADDRESS"});
    if (address.empty()) {
        const std::vector<std::string> parts = {
            jsonText(props, {"ST_NUM"}),
            jsonText(props, {"ST_DIR"}),
            jsonText(props, {"STREETNAME"}),
            jsonText(props, {"STREETTYPE"})
        };
        for (const std::string& p : parts) {
            if (p.empty()) continue;
            if (!address.empty()) address.push_back(' ');
            address += p;
        }
    }
    props["jurisdiction"] = "Baltimore County";
    props["source_file"] = "Baltimore County Tax parcel ArcGIS service";
    props["source_parcel_id"] = !taxpin.empty() ? taxpin : jsonText(props, {"PIN", "PARCEL_ASSET_ID", "OBJECTID"});
    props["account_id"] = taxpin;
    props["blocklot"] = taxpin;
    props["address"] = address;
    props["owner"] = owner;
    props["land_value"] = jsonText(props, {"LAND_VALUE"});
    props["improvement_value"] = jsonText(props, {"IMPROVEMENT_VALUE"});
    props["current_value"] = jsonText(props, {"TOTAL_VALUE"});
    props["sale_price"] = jsonText(props, {"SALE_PRICE"});
    props["sale_date"] = jsonText(props, {"SALE_DATE"});
    props["year_built"] = jsonText(props, {"YEAR_BUILT"});
    props["sdat_link"] = jsonText(props, {"URL"});
}

void maybeNormalizeArcgisFeature(const std::string& normalizer, json& feature) {
    if (normalizer.empty()) return;
    if (normalizer == "baltimore_county_parcels") {
        normalizeBaltimoreCountyArcgisFeature(feature);
        return;
    }
    throw std::runtime_error("unsupported ArcGIS feature normalizer: " + normalizer);
}

size_t arcgisServiceMaxRecordCount(const std::string& service_url);

struct CensusAcsJoinStats {
    size_t matched = 0;
    size_t missing = 0;
};

std::string jsonScalarToString(const json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_number_integer()) return std::to_string(value.get<int64_t>());
    if (value.is_number_unsigned()) return std::to_string(value.get<uint64_t>());
    if (value.is_number_float()) {
        std::ostringstream os;
        os << value.get<double>();
        return os.str();
    }
    if (value.is_boolean()) return value.get<bool>() ? "true" : "false";
    return {};
}

int64_t parseInt64Loose(const std::string& value) {
    if (value.empty()) return 0;
    try {
        return std::stoll(value);
    } catch (...) {
        return 0;
    }
}

std::string censusAcsMarginField(const std::string& estimate_field) {
    if (estimate_field.size() >= 2 && estimate_field.ends_with("E")) {
        return estimate_field.substr(0, estimate_field.size() - 1) + "M";
    }
    return {};
}

void setJsonString(json& props, const char* key, const std::string& value) {
    props[key] = value;
}

void setJsonInteger(json& props, const char* key, int64_t value) {
    props[key] = value;
}

void setJsonPercent(json& props, const char* key, int64_t numerator, int64_t denominator) {
    props[key] = denominator > 0 ? (100.0 * (double)numerator / (double)denominator) : 0.0;
}

std::unordered_map<std::string, json> parseCensusAcsRowsByGeoid(
    const fs::path& acs_json_path,
    const std::string& table,
    const std::string& year,
    const std::string& survey) {
    std::ifstream in(acs_json_path);
    if (!in) throw std::runtime_error("failed to open ACS response " + acs_json_path.string());
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string raw = buffer.str();
    const std::string trimmed = trimLeadingWhitespace(raw);
    if (trimmed.empty()) {
        throw std::runtime_error("ACS response is empty: " + acs_json_path.string());
    }
    if (looksLikeMissingCensusKeyHtml(trimmed)) {
        throw std::runtime_error(
            "ACS response is a Census 'Missing Key' HTML page, not JSON: " +
            acs_json_path.string() +
            ". Configure WORLDSIM_CENSUS_API_KEY or CENSUS_API_KEY and refresh the ACS source artifact.");
    }
    if (trimmed.front() != '[') {
        throw std::runtime_error(
            "ACS response is not a JSON header/data array: " +
            acs_json_path.string() +
            " preview=\"" + previewBytesForError(trimmed) + "\"");
    }

    json rows;
    try {
        rows = json::parse(trimmed, nullptr, true, true);
    } catch (const std::exception& e) {
        throw std::runtime_error(
            "failed to parse ACS JSON " + acs_json_path.string() +
            ": " + e.what() +
            " preview=\"" + previewBytesForError(trimmed) + "\"");
    }
    if (!rows.is_array() || rows.empty() || !rows[0].is_array()) {
        throw std::runtime_error("ACS response is not a header/data array");
    }
    const json& header_row = rows[0];
    std::vector<std::string> headers;
    headers.reserve(header_row.size());
    std::unordered_map<std::string, size_t> col_idx;
    for (size_t i = 0; i < header_row.size(); ++i) {
        const std::string name = jsonScalarToString(header_row[i]);
        headers.push_back(name);
        col_idx[name] = i;
    }
    auto field_at = [&](const json& row, const std::string& field) -> std::string {
        const auto it = col_idx.find(field);
        if (it == col_idx.end() || it->second >= row.size()) return {};
        return jsonScalarToString(row[it->second]);
    };

    std::unordered_map<std::string, json> out;
    out.reserve(rows.size() > 1 ? rows.size() - 1 : 0);
    for (size_t ri = 1; ri < rows.size(); ++ri) {
        const json& row = rows[ri];
        if (!row.is_array()) continue;
        const std::string state = field_at(row, "state");
        const std::string county = field_at(row, "county");
        const std::string tract = field_at(row, "tract");
        const std::string geoid = state + county + tract;
        if (geoid.empty()) continue;
        json props = json::object();
        props["acs_table"] = table;
        props["acs_year"] = year;
        props["acs_survey"] = survey;
        props["acs_geoid"] = geoid;

        if (table == "B02001") {
            const struct FieldMap { const char* field; const char* out_key; } maps[] = {
                {"B02001_001E", "total_population"},
                {"B02001_002E", "population_white_alone"},
                {"B02001_003E", "population_black_alone"},
                {"B02001_004E", "population_american_indian_alaska_native_alone"},
                {"B02001_005E", "population_asian_alone"},
                {"B02001_006E", "population_native_hawaiian_pacific_islander_alone"},
                {"B02001_007E", "population_other_race_alone"},
                {"B02001_008E", "population_two_or_more_races"}
            };
            int64_t total = 0;
            int64_t white = 0;
            int64_t black = 0;
            int64_t aian = 0;
            int64_t asian = 0;
            int64_t nhpi = 0;
            int64_t other = 0;
            int64_t multi = 0;
            for (const auto& map : maps) {
                const int64_t value = parseInt64Loose(field_at(row, map.field));
                setJsonInteger(props, map.out_key, value);
                const std::string margin_key = censusAcsMarginField(map.field);
                if (!margin_key.empty()) {
                    setJsonInteger(props, (std::string(map.out_key) + "_moe").c_str(), parseInt64Loose(field_at(row, margin_key)));
                }
                if (std::string(map.out_key) == "total_population") total = value;
                else if (std::string(map.out_key) == "population_white_alone") white = value;
                else if (std::string(map.out_key) == "population_black_alone") black = value;
                else if (std::string(map.out_key) == "population_american_indian_alaska_native_alone") aian = value;
                else if (std::string(map.out_key) == "population_asian_alone") asian = value;
                else if (std::string(map.out_key) == "population_native_hawaiian_pacific_islander_alone") nhpi = value;
                else if (std::string(map.out_key) == "population_other_race_alone") other = value;
                else if (std::string(map.out_key) == "population_two_or_more_races") multi = value;
            }
            setJsonPercent(props, "pct_white_alone", white, total);
            setJsonPercent(props, "pct_black_alone", black, total);
            setJsonPercent(props, "pct_american_indian_alaska_native_alone", aian, total);
            setJsonPercent(props, "pct_asian_alone", asian, total);
            setJsonPercent(props, "pct_native_hawaiian_pacific_islander_alone", nhpi, total);
            setJsonPercent(props, "pct_other_race_alone", other, total);
            setJsonPercent(props, "pct_two_or_more_races", multi, total);
            setJsonPercent(props, "pct_nonwhite", total - white, total);
        } else if (table == "B03003") {
            const int64_t total = parseInt64Loose(field_at(row, "B03003_001E"));
            const int64_t not_hisp = parseInt64Loose(field_at(row, "B03003_002E"));
            const int64_t hisp = parseInt64Loose(field_at(row, "B03003_003E"));
            setJsonInteger(props, "total_population", total);
            setJsonInteger(props, "population_not_hispanic_or_latino", not_hisp);
            setJsonInteger(props, "population_hispanic_or_latino", hisp);
            setJsonInteger(props, "total_population_moe", parseInt64Loose(field_at(row, "B03003_001M")));
            setJsonInteger(props, "population_not_hispanic_or_latino_moe", parseInt64Loose(field_at(row, "B03003_002M")));
            setJsonInteger(props, "population_hispanic_or_latino_moe", parseInt64Loose(field_at(row, "B03003_003M")));
            setJsonPercent(props, "pct_not_hispanic_or_latino", not_hisp, total);
            setJsonPercent(props, "pct_hispanic_or_latino", hisp, total);
        } else {
            throw std::runtime_error("unsupported ACS census table: " + table);
        }

        out.emplace(geoid, std::move(props));
    }
    if (out.empty()) throw std::runtime_error("ACS response produced no tract rows");
    return out;
}

json buildJoinedCensusAcsProperties(
    const json& feature_props,
    const json& acs_props) {
    json out = feature_props.is_object() ? feature_props : json::object();
    for (auto it = acs_props.begin(); it != acs_props.end(); ++it) {
        out[it.key()] = it.value();
    }
    return out;
}

CensusAcsJoinStats writeCensusAcsArcgisLayerGeoJson(
    const std::string& service_url,
    const std::string& where_clause,
    const fs::path& out_path,
    const std::unordered_map<std::string, json>& acs_rows_by_geoid) {
    if (service_url.empty()) throw std::runtime_error("missing ArcGIS service URL");
    const std::string where = where_clause.empty() ? "1=1" : where_clause;
    json ids = json::parse(httpPostForm(service_url + "/query", {
        {"where", where},
        {"returnIdsOnly", "true"},
        {"f", "json"}
    }).body);
    if (ids.contains("error")) throw std::runtime_error("ArcGIS object ID query failed: " + ids["error"].dump());
    std::vector<int64_t> object_ids;
    for (const auto& id : ids.value("objectIds", json::array())) object_ids.push_back(id.get<int64_t>());
    std::sort(object_ids.begin(), object_ids.end());
    if (object_ids.empty()) throw std::runtime_error("ArcGIS service returned no object IDs");

    fs::create_directories(out_path.parent_path());
    fs::path tmp = out_path;
    tmp += ".part";
    std::ofstream out(tmp);
    if (!out) throw std::runtime_error("failed to open output " + tmp.string());
    out << "{\"type\":\"FeatureCollection\",\"name\":\"" << jsonEscape(out_path.stem().string()) << "\",\"features\":[";
    bool first_feature = true;
    size_t written = 0;
    size_t missing = 0;
    const size_t page_size = std::max<size_t>(1, std::min<size_t>(1000, arcgisServiceMaxRecordCount(service_url)));
    for (size_t off = 0; off < object_ids.size(); off += page_size) {
        std::ostringstream id_list;
        const size_t end = std::min(object_ids.size(), off + page_size);
        for (size_t i = off; i < end; ++i) {
            if (i > off) id_list << ',';
            id_list << object_ids[i];
        }
        json page = json::parse(httpPostForm(service_url + "/query", {
            {"objectIds", id_list.str()},
            {"outFields", "*"},
            {"returnGeometry", "true"},
            {"outSR", "4326"},
            {"f", "geojson"}
        }).body);
        if (page.contains("error")) throw std::runtime_error("ArcGIS feature query failed: " + page["error"].dump());
        for (auto& feature : page.value("features", json::array())) {
            json& props = feature["properties"];
            const std::string geoid = jsonText(props, {"GEOID", "GEOID20", "GEOID10"});
            if (geoid.empty()) {
                missing++;
                continue;
            }
            const auto it = acs_rows_by_geoid.find(geoid);
            if (it == acs_rows_by_geoid.end()) {
                missing++;
                continue;
            }
            props = buildJoinedCensusAcsProperties(props, it->second);
            if (!first_feature) out << ',';
            first_feature = false;
            out << feature.dump();
            written++;
        }
    }
    out << "]}\n";
    out.close();
    if (written == 0) throw std::runtime_error("joined Census/ACS import produced no features");
    std::error_code ec;
    fs::rename(tmp, out_path, ec);
    if (ec) throw std::runtime_error("rename failed: " + ec.message());
    return CensusAcsJoinStats{written, missing};
}

CensusAcsJoinStats buildCensusAcsArcgisLayerFeatures(
    const std::string& service_url,
    const std::string& where_clause,
    const std::unordered_map<std::string, json>& acs_rows_by_geoid,
    std::vector<LayerDef::FeatureRecord>& features,
    std::vector<LayerDef::FeatureProperties>& feature_properties) {
    if (service_url.empty()) throw std::runtime_error("missing ArcGIS service URL");
    const std::string where = where_clause.empty() ? "1=1" : where_clause;
    json ids = json::parse(httpPostForm(service_url + "/query", {
        {"where", where},
        {"returnIdsOnly", "true"},
        {"f", "json"}
    }).body);
    if (ids.contains("error")) throw std::runtime_error("ArcGIS object ID query failed: " + ids["error"].dump());
    std::vector<int64_t> object_ids;
    for (const auto& id : ids.value("objectIds", json::array())) object_ids.push_back(id.get<int64_t>());
    std::sort(object_ids.begin(), object_ids.end());
    if (object_ids.empty()) throw std::runtime_error("ArcGIS service returned no object IDs");

    size_t written = 0;
    size_t missing = 0;
    const size_t page_size = std::max<size_t>(1, std::min<size_t>(1000, arcgisServiceMaxRecordCount(service_url)));
    for (size_t off = 0; off < object_ids.size(); off += page_size) {
        std::ostringstream id_list;
        const size_t end = std::min(object_ids.size(), off + page_size);
        for (size_t i = off; i < end; ++i) {
            if (i > off) id_list << ',';
            id_list << object_ids[i];
        }
        json page = json::parse(httpPostForm(service_url + "/query", {
            {"objectIds", id_list.str()},
            {"outFields", "*"},
            {"returnGeometry", "true"},
            {"outSR", "4326"},
            {"f", "geojson"}
        }).body);
        if (page.contains("error")) throw std::runtime_error("ArcGIS feature query failed: " + page["error"].dump());
        for (auto& feature : page.value("features", json::array())) {
            json& props = feature["properties"];
            const std::string geoid = jsonText(props, {"GEOID", "GEOID20", "GEOID10"});
            if (geoid.empty()) {
                missing++;
                continue;
            }
            const auto it = acs_rows_by_geoid.find(geoid);
            if (it == acs_rows_by_geoid.end()) {
                missing++;
                continue;
            }
            props = buildJoinedCensusAcsProperties(props, it->second);
            const size_t before = features.size();
            appendGeoJsonFeatureRecords(feature, features, feature_properties);
            written += features.size() - before;
        }
    }
    if (written == 0) throw std::runtime_error("joined Census/ACS import produced no features");
    return CensusAcsJoinStats{written, missing};
}

size_t arcgisServiceMaxRecordCount(const std::string& service_url) {
    try {
        json meta = json::parse(httpPostForm(service_url, {
            {"f", "json"}
        }).body);
        if (meta.contains("maxRecordCount") && meta["maxRecordCount"].is_number_integer()) {
            const int64_t value = meta["maxRecordCount"].get<int64_t>();
            if (value > 0) return (size_t)value;
        }
    } catch (...) {
    }
    return 1000;
}

void buildArcgisFeatureLayerFeatures(
    const std::string& service_url,
    const std::string& where_clause,
    const std::string& normalizer,
    std::vector<LayerDef::FeatureRecord>& features,
    std::vector<LayerDef::FeatureProperties>& feature_properties) {
    if (service_url.empty()) throw std::runtime_error("missing ArcGIS service URL");
    json ids = json::parse(httpPostForm(service_url + "/query", {
        {"where", where_clause.empty() ? "1=1" : where_clause},
        {"returnIdsOnly", "true"},
        {"f", "json"}
    }).body);
    if (ids.contains("error")) throw std::runtime_error("ArcGIS object ID query failed: " + ids["error"].dump());
    std::vector<int64_t> object_ids;
    for (const auto& id : ids.value("objectIds", json::array())) object_ids.push_back(id.get<int64_t>());
    std::sort(object_ids.begin(), object_ids.end());
    if (object_ids.empty()) throw std::runtime_error("ArcGIS service returned no object IDs");

    const size_t page_size = std::max<size_t>(1, std::min<size_t>(1000, arcgisServiceMaxRecordCount(service_url)));
    for (size_t off = 0; off < object_ids.size(); off += page_size) {
        std::ostringstream id_list;
        const size_t end = std::min(object_ids.size(), off + page_size);
        for (size_t i = off; i < end; ++i) {
            if (i > off) id_list << ',';
            id_list << object_ids[i];
        }
        json page = json::parse(httpPostForm(service_url + "/query", {
            {"objectIds", id_list.str()},
            {"outFields", "*"},
            {"returnGeometry", "true"},
            {"outSR", "4326"},
            {"f", "geojson"}
        }).body);
        if (page.contains("error")) throw std::runtime_error("ArcGIS feature query failed: " + page["error"].dump());
        for (auto& feature : page.value("features", json::array())) {
            maybeNormalizeArcgisFeature(normalizer, feature);
            appendGeoJsonFeatureRecords(feature, features, feature_properties);
        }
    }
    if (features.empty()) throw std::runtime_error("ArcGIS service returned no features");
}

void writeArcgisFeatureLayerGeoJson(
    const std::string& service_url,
    const std::string& where_clause,
    const fs::path& out_path,
    const std::string& normalizer) {
    if (service_url.empty()) throw std::runtime_error("missing ArcGIS service URL");
    json ids = json::parse(httpPostForm(service_url + "/query", {
        {"where", where_clause.empty() ? "1=1" : where_clause},
        {"returnIdsOnly", "true"},
        {"f", "json"}
    }).body);
    if (ids.contains("error")) throw std::runtime_error("ArcGIS object ID query failed: " + ids["error"].dump());
    std::vector<int64_t> object_ids;
    for (const auto& id : ids.value("objectIds", json::array())) object_ids.push_back(id.get<int64_t>());
    std::sort(object_ids.begin(), object_ids.end());
    if (object_ids.empty()) throw std::runtime_error("ArcGIS service returned no object IDs");

    fs::create_directories(out_path.parent_path());
    fs::path tmp = out_path;
    tmp += ".part";
    std::ofstream out(tmp);
    if (!out) throw std::runtime_error("failed to open output " + tmp.string());
    out << "{\"type\":\"FeatureCollection\",\"name\":\"" << jsonEscape(out_path.stem().string()) << "\",\"features\":[";
    bool first_feature = true;
    size_t written = 0;
    const size_t page_size = std::max<size_t>(1, std::min<size_t>(1000, arcgisServiceMaxRecordCount(service_url)));
    for (size_t off = 0; off < object_ids.size(); off += page_size) {
        std::ostringstream id_list;
        const size_t end = std::min(object_ids.size(), off + page_size);
        for (size_t i = off; i < end; ++i) {
            if (i > off) id_list << ',';
            id_list << object_ids[i];
        }
        json page = json::parse(httpPostForm(service_url + "/query", {
            {"objectIds", id_list.str()},
            {"outFields", "*"},
            {"returnGeometry", "true"},
            {"outSR", "4326"},
            {"f", "geojson"}
        }).body);
        if (page.contains("error")) throw std::runtime_error("ArcGIS feature query failed: " + page["error"].dump());
        for (auto& feature : page.value("features", json::array())) {
            maybeNormalizeArcgisFeature(normalizer, feature);
            if (!first_feature) out << ',';
            first_feature = false;
            out << feature.dump();
            written++;
        }
    }
    out << "]}\n";
    out.close();
    if (written == 0) throw std::runtime_error("ArcGIS service returned no features");
    std::error_code ec;
    fs::rename(tmp, out_path, ec);
    if (ec) throw std::runtime_error("rename failed: " + ec.message());
}

std::string shellQuote(const fs::path& path) {
    std::string s = path.string();
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

const json* jsonPathValue(const json& root, const std::string& path) {
    if (path.empty()) return &root;
    const json* current = &root;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t dot = path.find('.', start);
        const std::string key = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (key.empty() || !current->is_object()) return nullptr;
        auto it = current->find(key);
        if (it == current->end()) return nullptr;
        current = &(*it);
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return current;
}

bool jsonPathDouble(const json& root, const std::string& path, double& out) {
    const json* value = jsonPathValue(root, path);
    if (!value) return false;
    try {
        if (value->is_number()) {
            out = value->get<double>();
            return std::isfinite(out);
        }
        if (value->is_string()) {
            char* end = nullptr;
            const std::string s = value->get<std::string>();
            out = std::strtod(s.c_str(), &end);
            return end && end != s.c_str() && *end == '\0' && std::isfinite(out);
        }
    } catch (...) {
    }
    return false;
}

bool jsonPathDoubleAny(
    const json& root,
    const std::initializer_list<std::string_view>& paths,
    double& out) {
    for (std::string_view path : paths) {
        if (path.empty()) continue;
        if (jsonPathDouble(root, std::string(path), out)) return true;
    }
    return false;
}

void flattenJsonProperties(
    const json& value,
    const std::string& prefix,
    std::vector<std::pair<std::string, std::string>>& out) {
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            const std::string key = prefix.empty() ? it.key() : prefix + "." + it.key();
            flattenJsonProperties(it.value(), key, out);
        }
        return;
    }
    if (value.is_array()) {
        out.push_back({prefix, value.dump()});
        return;
    }
    out.push_back({prefix, jsonValueToString(value)});
}

void writeJsonPointFeedGeoJson(
    const fs::path& json_path,
    const fs::path& out_path,
    const std::string& item_path,
    const std::string& lon_field,
    const std::string& lat_field,
    const std::string& base_url) {
    std::ifstream in(json_path);
    if (!in) throw std::runtime_error("failed to open json feed");
    json root;
    in >> root;
    const json* items = item_path.empty() ? &root : jsonPathValue(root, item_path);
    if (!items || !items->is_array()) throw std::runtime_error("json feed items path is not an array");

    const fs::path tmp = out_path.string() + ".tmp";
    std::ofstream out(tmp);
    if (!out) throw std::runtime_error("failed to open geojson output");
    out << "{\"type\":\"FeatureCollection\",\"features\":[\n";
    bool first = true;
    size_t written = 0;
    std::unordered_map<std::string, std::pair<double, double>> inferred_coords_by_key;
    for (const auto& item : *items) {
        if (!item.is_object()) continue;
        double lon = 0.0;
        double lat = 0.0;
        if ((!jsonPathDouble(item, lon_field, lon) || !jsonPathDouble(item, lat_field, lat)) &&
            (!jsonPathDoubleAny(item, {"center.lon", "geometry.0.lon"}, lon) ||
             !jsonPathDoubleAny(item, {"center.lat", "geometry.0.lat"}, lat))) {
            continue;
        }
        for (const std::string& key : locationKeysForItem(item)) {
            inferred_coords_by_key.emplace(key, std::make_pair(lon, lat));
        }
    }
    for (const auto& item : *items) {
        if (!item.is_object()) continue;
        double lon = 0.0;
        double lat = 0.0;
        if ((!jsonPathDouble(item, lon_field, lon) || !jsonPathDouble(item, lat_field, lat)) &&
            (!jsonPathDoubleAny(item, {"center.lon", "geometry.0.lon"}, lon) ||
             !jsonPathDoubleAny(item, {"center.lat", "geometry.0.lat"}, lat))) {
            bool inferred = false;
            for (const std::string& key : locationKeysForItem(item)) {
                auto it = inferred_coords_by_key.find(key);
                if (it == inferred_coords_by_key.end()) continue;
                lon = it->second.first;
                lat = it->second.second;
                inferred = true;
                break;
            }
            if (!inferred) continue;
        }
        json props = json::object();
        std::vector<std::pair<std::string, std::string>> flat_props;
        flattenJsonProperties(item, "", flat_props);
        for (const auto& kv : flat_props) props[kv.first] = kv.second;
        const std::string org_image = item.value("orgImageUrl", "");
        const std::string image_url = item.value("imageUrl", "");
        const std::string resolved_org_image = resolveRelativeUrl(org_image, base_url);
        const std::string resolved_image = resolveRelativeUrl(image_url, base_url);
        if (!resolved_org_image.empty()) props["orgImageUrl"] = resolved_org_image;
        if (!resolved_image.empty()) props["imageUrl"] = resolved_image;
        if (!resolved_image.empty()) props["image_url_resolved"] = resolved_image;
        if (!resolved_org_image.empty()) props["org_image_url_resolved"] = resolved_org_image;
        json feature = {
            {"type", "Feature"},
            {"geometry", {
                {"type", "Point"},
                {"coordinates", {lon, lat}}
            }},
            {"properties", std::move(props)}
        };
        if (!first) out << ",\n";
        first = false;
        out << feature.dump();
        written++;
    }
    out << "\n]}\n";
    out.close();
    if (!out.good()) throw std::runtime_error("failed writing geojson");
    if (written == 0) throw std::runtime_error("json feed produced no point features");
    std::error_code ec;
    fs::rename(tmp, out_path, ec);
    if (ec) throw std::runtime_error("rename failed: " + ec.message());
}

void buildJsonPointFeedFeatures(
    const fs::path& json_path,
    const std::string& item_path,
    const std::string& lon_field,
    const std::string& lat_field,
    const std::string& base_url,
    std::vector<LayerDef::FeatureRecord>& features,
    std::vector<LayerDef::FeatureProperties>& feature_properties) {
    std::ifstream in(json_path);
    if (!in) throw std::runtime_error("failed to open json feed");
    json root;
    in >> root;
    const json* items = item_path.empty() ? &root : jsonPathValue(root, item_path);
    if (!items || !items->is_array()) throw std::runtime_error("json feed items path is not an array");

    std::unordered_map<std::string, std::pair<double, double>> inferred_coords_by_key;
    for (const auto& item : *items) {
        if (!item.is_object()) continue;
        double lon = 0.0;
        double lat = 0.0;
        if ((!jsonPathDouble(item, lon_field, lon) || !jsonPathDouble(item, lat_field, lat)) &&
            (!jsonPathDoubleAny(item, {"center.lon", "geometry.0.lon"}, lon) ||
             !jsonPathDoubleAny(item, {"center.lat", "geometry.0.lat"}, lat))) {
            continue;
        }
        for (const std::string& key : locationKeysForItem(item)) {
            inferred_coords_by_key.emplace(key, std::make_pair(lon, lat));
        }
    }

    for (const auto& item : *items) {
        if (!item.is_object()) continue;
        double lon = 0.0;
        double lat = 0.0;
        if ((!jsonPathDouble(item, lon_field, lon) || !jsonPathDouble(item, lat_field, lat)) &&
            (!jsonPathDoubleAny(item, {"center.lon", "geometry.0.lon"}, lon) ||
             !jsonPathDoubleAny(item, {"center.lat", "geometry.0.lat"}, lat))) {
            bool inferred = false;
            for (const std::string& key : locationKeysForItem(item)) {
                auto it = inferred_coords_by_key.find(key);
                if (it == inferred_coords_by_key.end()) continue;
                lon = it->second.first;
                lat = it->second.second;
                inferred = true;
                break;
            }
            if (!inferred) continue;
        }

        LayerDef::FeatureRecord fg{};
        fg.extent.min_lon = fg.extent.max_lon = (float)lon;
        fg.extent.min_lat = fg.extent.max_lat = (float)lat;
        std::vector<std::pair<std::string, std::string>> props;
        flattenJsonProperties(item, "", props);
        const std::string org_image = item.value("orgImageUrl", "");
        const std::string image_url = item.value("imageUrl", "");
        const std::string resolved_org_image = resolveRelativeUrl(org_image, base_url);
        const std::string resolved_image = resolveRelativeUrl(image_url, base_url);
        if (!resolved_org_image.empty()) props.push_back({"orgImageUrl", resolved_org_image});
        if (!resolved_image.empty()) props.push_back({"imageUrl", resolved_image});
        if (!resolved_image.empty()) props.push_back({"image_url_resolved", resolved_image});
        if (!resolved_org_image.empty()) props.push_back({"org_image_url_resolved", resolved_org_image});
        appendFeatureWithProperties(std::move(fg), props, features, feature_properties);
    }
    if (features.empty()) throw std::runtime_error("json feed produced no point features");
}
}

bool layerHasImportSource(const LayerDef& layer) {
    if (layer.import_type == "arcgis_feature_layer") return !layer.import_service_url.empty();
    if (layer.import_type == "census_acs_tract_demographics") {
        return !layer.import_service_url.empty() && !layer.import_url.empty() &&
            !layer.import_table.empty() && !layer.import_year.empty() && !layer.import_survey.empty();
    }
    if (layer.import_type == "overpass_json_point_feed") {
        return !layer.import_url.empty() && !layer.import_query.empty();
    }
    return (layer.import_type == "zipped_shapefile" || layer.import_type == "socrata_csv_properties" ||
            layer.import_type == "xlsx_point_table" || layer.import_type == "json_point_feed") &&
           !layer.import_url.empty();
}

namespace {
fs::path sourceGeometryArtifactPath(const fs::path& root, const LayerDef& layer) {
    return provenanceSourceArtifactPath(root, layer, layer.file);
}

std::vector<fs::path> localImportArtifactCandidates(const fs::path& root, const LayerDef& layer) {
    std::vector<fs::path> out;
    auto append_name = [&](const std::string& name) {
        if (name.empty()) return;
        out.push_back(provenanceSourceArtifactPath(root, layer, name));
    };
    if (!layer.import_artifact_file.empty()) append_name(layer.import_artifact_file);
    const fs::path stored_path = resolveStoredLayerPath(root, layer);
    const std::string filename = stored_path.filename().string();
    const std::string stem = stored_path.stem().string();
    if (layer.import_type == "socrata_csv_properties") {
        append_name(filename + ".source.csv");
        append_name(layer.file + ".source.csv");
    } else if (layer.import_type == "xlsx_point_table") {
        append_name(stem + ".xlsx");
        append_name(filename + ".xlsx");
        append_name(layer.file + ".source.xlsx");
    } else if (layer.import_type == "json_point_feed" || layer.import_type == "overpass_json_point_feed") {
        append_name(stem + ".json");
        append_name(filename + ".json");
        append_name(filename + ".source.json");
        append_name(layer.file + ".json");
    } else if (layer.import_type == "zipped_shapefile") {
        append_name(filename + ".source.zip");
        append_name(stem + ".zip");
    } else if (layer.import_type == "census_acs_tract_demographics") {
        append_name(stem + ".acs.json");
        append_name(filename + ".acs.json");
    }
    std::vector<fs::path> deduped;
    deduped.reserve(out.size());
    for (const auto& path : out) {
        if (std::find(deduped.begin(), deduped.end(), path) == deduped.end()) deduped.push_back(path);
    }
    return deduped;
}
}

std::vector<fs::path> layerLocalSsotArtifactPaths(
    const fs::path& root,
    const LayerDef& layer) {
    std::vector<fs::path> out;
    if (!layer.source_url.empty() && layer.file.ends_with(".geojson")) {
        out.push_back(sourceGeometryArtifactPath(root, layer));
    } else if (layer.import_type == "arcgis_feature_layer") {
        out.push_back(sourceGeometryArtifactPath(root, layer));
    } else if (layer.import_type == "census_acs_tract_demographics") {
        out.push_back(sourceGeometryArtifactPath(root, layer));
        const auto extras = localImportArtifactCandidates(root, layer);
        out.insert(out.end(), extras.begin(), extras.end());
    } else {
        out = localImportArtifactCandidates(root, layer);
    }
    std::vector<fs::path> deduped;
    deduped.reserve(out.size());
    for (const auto& path : out) {
        if (std::find(deduped.begin(), deduped.end(), path) == deduped.end()) deduped.push_back(path);
    }
    return deduped;
}

bool layerHasLocalSsotSource(
    const fs::path& root,
    const LayerDef& layer) {
    const auto paths = layerLocalSsotArtifactPaths(root, layer);
    if (paths.empty()) return false;
    std::error_code ec;
    const bool require_all = layer.import_type == "census_acs_tract_demographics";
    bool any = false;
    for (const auto& path : paths) {
        const bool exists = fs::exists(path, ec) && !ec;
        if (require_all && !exists) return false;
        if (exists) any = true;
        ec.clear();
    }
    return any;
}

bool loadLayerFeaturesFromLocalSsotSource(
    const fs::path& root,
    const LayerDef& layer,
    std::vector<LayerDef::FeatureRecord>& features,
    std::vector<LayerDef::FeatureProperties>& feature_properties,
    std::string& source_used,
    std::string& error) {
    features.clear();
    feature_properties.clear();
    try {
        if (!layer.source_url.empty() && layer.file.ends_with(".geojson")) {
            const fs::path source_path = sourceGeometryArtifactPath(root, layer);
            features = loadLayerPointsFromFile(source_path, &feature_properties);
            source_used = "ssot_source_geojson:" + source_path.filename().string();
            return true;
        }
        if (layer.import_type == "arcgis_feature_layer") {
            const fs::path source_path = sourceGeometryArtifactPath(root, layer);
            features = loadLayerPointsFromFile(source_path, &feature_properties);
            source_used = "ssot_arcgis_geojson:" + source_path.filename().string();
            return true;
        }
        if (layer.import_type == "census_acs_tract_demographics") {
            const fs::path source_path = sourceGeometryArtifactPath(root, layer);
            fs::path acs_path;
            for (const auto& candidate : localImportArtifactCandidates(root, layer)) {
                if (candidate.extension() == ".json" && candidate.filename() != source_path.filename()) {
                    acs_path = candidate;
                    break;
                }
            }
            if (acs_path.empty()) {
                error = "missing census ACS source artifact";
                return false;
            }
            const auto acs_rows = parseCensusAcsRowsByGeoid(
                acs_path,
                layer.import_table,
                layer.import_year,
                layer.import_survey);
            buildCensusAcsGeoJsonFeatures(source_path, acs_rows, features, feature_properties);
            source_used =
                "ssot_census_sources:" + source_path.filename().string() + "+" + acs_path.filename().string();
            return true;
        }
        return loadLayerFeaturesFromLocalImportArtifact(
            root, layer, features, feature_properties, source_used, error);
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool layerHasLocalImportArtifact(
    const fs::path& root,
    const LayerDef& layer,
    fs::path* out_path) {
    std::error_code ec;
    for (const auto& candidate : localImportArtifactCandidates(root, layer)) {
        if (fs::exists(candidate, ec) && !ec) {
            if (out_path) *out_path = candidate;
            return true;
        }
        ec.clear();
    }
    return false;
}

bool loadLayerFeaturesFromLocalImportArtifact(
    const fs::path& root,
    const LayerDef& layer,
    std::vector<LayerDef::FeatureRecord>& features,
    std::vector<LayerDef::FeatureProperties>& feature_properties,
    std::string& source_used,
    std::string& error) {
    features.clear();
    feature_properties.clear();
    fs::path artifact_path;
    if (!layerHasLocalImportArtifact(root, layer, &artifact_path)) {
        error = "no local import artifact";
        return false;
    }
    try {
        if (layer.import_type == "census_acs_tract_demographics") {
            const fs::path source_path = sourceGeometryArtifactPath(root, layer);
            fs::path acs_path;
            for (const auto& candidate : localImportArtifactCandidates(root, layer)) {
                if (candidate.extension() == ".json" && candidate.filename() != source_path.filename()) {
                    acs_path = candidate;
                    break;
                }
            }
            if (acs_path.empty()) {
                error = "missing census ACS source artifact";
                return false;
            }
            const auto acs_rows = parseCensusAcsRowsByGeoid(
                acs_path,
                layer.import_table,
                layer.import_year,
                layer.import_survey);
            buildCensusAcsGeoJsonFeatures(source_path, acs_rows, features, feature_properties);
        } else if (layer.import_type == "socrata_csv_properties") {
            buildSocrataHowardPropertyFeatures(artifact_path, features, feature_properties);
        } else if (layer.import_type == "xlsx_point_table") {
            buildXlsxPointTableFeatures(
                artifact_path,
                layer.import_sheet_name,
                layer.import_lon_field,
                layer.import_lat_field,
                features,
                feature_properties);
        } else if (layer.import_type == "json_point_feed" || layer.import_type == "overpass_json_point_feed") {
            buildJsonPointFeedFeatures(
                artifact_path,
                layer.import_item_path,
                layer.import_lon_field,
                layer.import_lat_field,
                layer.import_url,
                features,
                feature_properties);
        } else if (layer.import_type == "zipped_shapefile") {
            const auto members = extractShapefileMembers(
                artifact_path,
                layer.import_shapefile.empty() ? "Property.shp" : layer.import_shapefile);
            const auto dbf = parseDbf(members.at(".dbf"));
            const std::string source_file = layer.import_shapefile.empty() ? "Property.shp" : layer.import_shapefile;
            const std::string jurisdiction =
                layer.name.size() > 8 && layer.name.ends_with(" Parcels")
                    ? layer.name.substr(0, layer.name.size() - 8)
                    : layer.name;
            buildStateplaneParcelShapefileFeatures(
                members.at(".shp"),
                dbf,
                jurisdiction,
                source_file,
                layer.import_source_crs == "EPSG:2248" ||
                    layer.import_source_crs == "EPSG:6488" ||
                    layer.import_source_crs == "ESRI:103069",
                features,
                feature_properties);
        } else {
            error = "unsupported local import artifact type";
            return false;
        }
        source_used = "local_import_artifact:" + artifact_path.filename().string();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

VersionedDownloadResult downloadOrImportLayer(
    const LayerDef& layer,
    const fs::path& out_path,
    const fs::path& root,
    const DownloadProgressCallback& on_progress) {
    if (!layer.source_url.empty()) {
        if (out_path.extension() == ".geojson") {
            const fs::path source_path = provenanceSourceArtifactPath(
                root,
                layer,
                out_path.filename().string());
            VersionedDownloadResult res = downloadUrlVersioned(layer.source_url, source_path, root / "data" / "versions", on_progress);
            if (!res.ok) return res;
            try {
                std::vector<LayerDef::FeatureProperties> feature_properties;
                const std::vector<LayerDef::FeatureRecord> features =
                    loadLayerPointsFromFile(source_path, &feature_properties);
                saveCanonicalLayerBinaryForSourceGeometry(
                    source_path,
                    out_path,
                    fileSignature(source_path),
                    features,
                    feature_properties);
                res.message = "materialized canonical layer binary via " + res.message;
            } catch (const std::exception& e) {
                res.ok = false;
                res.message = std::string("canonicalization failed: ") + e.what();
            }
            return res;
        }
        return downloadUrlVersioned(layer.source_url, out_path, root / "data" / "versions", on_progress);
    }
    VersionedDownloadResult res;
    if (!layerHasImportSource(layer)) {
        res.message = "no source URL or import source";
        return res;
    }
    if (layer.import_type == "socrata_csv_properties") {
        const fs::path csv_path = provenanceSourceArtifactPath(root, layer, out_path.filename().string() + ".source.csv");
        VersionedDownloadResult dl = downloadUrlVersioned(layer.import_url, csv_path, root / "data" / "versions", on_progress);
        if (!dl.ok) return dl;
        std::error_code ec;
        fs::remove(out_path, ec);
        fs::remove(fs::path(out_path.string() + ".canonical.bin"), ec);
        res.ok = true;
        res.changed = dl.changed;
        res.not_modified = dl.not_modified;
        res.message = "downloaded Socrata CSV source artifact for DuckDB ingest via " + dl.message;
        return res;
    }
    if (layer.import_type == "arcgis_feature_layer") {
        try {
            const fs::path source_path = provenanceSourceArtifactPath(root, layer, out_path.filename().string());
            writeArcgisFeatureLayerGeoJson(
                layer.import_service_url,
                layer.import_where,
                source_path,
                layer.import_normalizer);
            std::vector<LayerDef::FeatureRecord> features;
            std::vector<LayerDef::FeatureProperties> feature_properties;
            features = loadLayerPointsFromFile(source_path, &feature_properties);
            saveCanonicalLayerBinaryForSourceGeometry(
                source_path,
                out_path,
                fileSignature(source_path),
                features,
                feature_properties);
            res.ok = true;
            res.changed = true;
            res.not_modified = false;
            res.message = "imported ArcGIS feature layer as canonical layer binary";
        } catch (const std::exception& e) {
            res.ok = false;
            res.message = std::string("import failed: ") + e.what();
        }
        return res;
    }
    if (layer.import_type == "census_acs_tract_demographics") {
        fs::path artifact_name =
            !layer.import_artifact_file.empty()
                ? fs::path(layer.import_artifact_file)
                : fs::path(out_path.stem().string() + ".acs.json");
        const fs::path acs_json_path = provenanceSourceArtifactPath(root, layer, artifact_name.string());
        VersionedDownloadResult dl = downloadUrlVersioned(
            censusImportUrlWithKey(layer),
            acs_json_path,
            root / "data" / "versions",
            {},
            validateCensusAcsDownloadArtifact);
        if (!dl.ok) return dl;
        try {
            const fs::path source_path = provenanceSourceArtifactPath(root, layer, out_path.filename().string());
            const auto acs_rows = parseCensusAcsRowsByGeoid(
                acs_json_path,
                layer.import_table,
                layer.import_year,
                layer.import_survey);
            writeArcgisFeatureLayerGeoJson(
                layer.import_service_url,
                layer.import_where,
                source_path,
                "");
            std::vector<LayerDef::FeatureRecord> features;
            std::vector<LayerDef::FeatureProperties> feature_properties;
            buildCensusAcsGeoJsonFeatures(source_path, acs_rows, features, feature_properties);
            saveCanonicalLayerBinaryForSourceGeometry(
                source_path,
                out_path,
                fileSignature(source_path),
                features,
                feature_properties);
            res.ok = true;
            res.changed = true;
            res.not_modified = false;
            std::ostringstream msg;
            msg << "imported Census ACS tract demographics via " << dl.message
                << " using persisted geometry + ACS source artifacts";
            res.message = msg.str();
        } catch (const std::exception& e) {
            res.ok = false;
            res.message = std::string("import failed: ") + e.what();
        }
        return res;
    }
    if (layer.import_type == "xlsx_point_table") {
        fs::path artifact_name =
            !layer.import_artifact_file.empty()
                ? fs::path(layer.import_artifact_file)
                : fs::path(out_path.stem().string() + ".xlsx");
        const fs::path xlsx_path = provenanceSourceArtifactPath(root, layer, artifact_name.string());
        VersionedDownloadResult dl = downloadUrlVersioned(layer.import_url, xlsx_path, root / "data" / "versions", on_progress);
        if (!dl.ok) return dl;
        try {
            std::vector<LayerDef::FeatureRecord> features;
            std::vector<LayerDef::FeatureProperties> feature_properties;
            buildXlsxPointTableFeatures(
                xlsx_path,
                layer.import_sheet_name,
                layer.import_lon_field,
                layer.import_lat_field,
                features,
                feature_properties);
            saveCanonicalLayerBinary(out_path, fileSignature(xlsx_path), features, feature_properties);
            res.ok = true;
            res.changed = true;
            res.not_modified = false;
            res.message = "imported XLSX point table via " + dl.message;
        } catch (const std::exception& e) {
            res.ok = false;
            res.message = std::string("import failed: ") + e.what();
        }
        return res;
    }
    if (layer.import_type == "json_point_feed") {
        fs::path artifact_name =
            !layer.import_artifact_file.empty()
                ? fs::path(layer.import_artifact_file)
                : fs::path(out_path.stem().string() + ".json");
        const fs::path json_path = provenanceSourceArtifactPath(root, layer, artifact_name.string());
        VersionedDownloadResult dl = downloadUrlVersioned(layer.import_url, json_path, root / "data" / "versions", on_progress);
        if (!dl.ok) return dl;
        try {
            std::vector<LayerDef::FeatureRecord> features;
            std::vector<LayerDef::FeatureProperties> feature_properties;
            buildJsonPointFeedFeatures(
                json_path,
                layer.import_item_path,
                layer.import_lon_field,
                layer.import_lat_field,
                layer.import_url,
                features,
                feature_properties);
            saveCanonicalLayerBinary(out_path, fileSignature(json_path), features, feature_properties);
            res.ok = true;
            res.changed = true;
            res.not_modified = false;
            res.message = "imported JSON point feed via " + dl.message;
        } catch (const std::exception& e) {
            res.ok = false;
            res.message = std::string("import failed: ") + e.what();
        }
        return res;
    }
    if (layer.import_type == "overpass_json_point_feed") {
        fs::path artifact_name =
            !layer.import_artifact_file.empty()
                ? fs::path(layer.import_artifact_file)
                : fs::path(out_path.stem().string() + ".json");
        const fs::path json_path = provenanceSourceArtifactPath(root, layer, artifact_name.string());
        try {
            const HttpResponse resp = httpPostForm(layer.import_url, {
                {"data", layer.import_query}
            });
            bool body_changed = true;
            if (!writeTextFileIfChanged(json_path, resp.body, &body_changed)) {
                throw std::runtime_error("failed to write overpass artifact");
            }
            std::vector<LayerDef::FeatureRecord> features;
            std::vector<LayerDef::FeatureProperties> feature_properties;
            buildJsonPointFeedFeatures(
                json_path,
                layer.import_item_path,
                layer.import_lon_field,
                layer.import_lat_field,
                layer.import_url,
                features,
                feature_properties);
            saveCanonicalLayerBinary(out_path, fileSignature(json_path), features, feature_properties);
            res.ok = true;
            res.changed = body_changed;
            res.not_modified = !body_changed;
            res.message = body_changed ? "imported Overpass JSON point feed" : "checked Overpass JSON point feed";
        } catch (const std::exception& e) {
            res.ok = false;
            res.message = std::string("import failed: ") + e.what();
        }
        return res;
    }
    if (layer.import_type != "zipped_shapefile" ||
        (layer.import_source_crs != "EPSG:2248" &&
         layer.import_source_crs != "EPSG:26985" &&
         layer.import_source_crs != "EPSG:6488" &&
         layer.import_source_crs != "ESRI:103069")) {
        res.message = "unsupported import type/source CRS";
        return res;
    }
    const fs::path archive_path = provenanceSourceArtifactPath(root, layer, out_path.filename().string() + ".source.zip");
    VersionedDownloadResult dl = downloadUrlVersioned(layer.import_url, archive_path, root / "data" / "versions", on_progress);
    if (!dl.ok) return dl;
    try {
        const auto members = extractShapefileMembers(archive_path, layer.import_shapefile.empty() ? "Property.shp" : layer.import_shapefile);
        const auto dbf = parseDbf(members.at(".dbf"));
        const std::string source_file = layer.import_shapefile.empty() ? "Property.shp" : layer.import_shapefile;
        const std::string jurisdiction =
            layer.name.size() > 8 && layer.name.ends_with(" Parcels")
                ? layer.name.substr(0, layer.name.size() - 8)
                : layer.name;
        std::vector<LayerDef::FeatureRecord> features;
        std::vector<LayerDef::FeatureProperties> feature_properties;
        buildStateplaneParcelShapefileFeatures(
            members.at(".shp"),
            dbf,
            jurisdiction,
            source_file,
            layer.import_source_crs == "EPSG:2248" ||
                layer.import_source_crs == "EPSG:6488" ||
                layer.import_source_crs == "ESRI:103069",
            features,
            feature_properties);
        saveCanonicalLayerBinary(out_path, fileSignature(archive_path), features, feature_properties);
        res.ok = true;
        res.changed = true;
        res.not_modified = false;
        res.message = "imported zipped shapefile via " + dl.message;
    } catch (const std::exception& e) {
        res.ok = false;
        res.message = std::string("import failed: ") + e.what();
    }
    return res;
}
namespace {
void saveCanonicalLayerBinaryForSourceGeometry(
    const fs::path& source_geometry_path,
    const fs::path& target_layer_path,
    const std::string& sig,
    const std::vector<LayerDef::FeatureRecord>& features,
    const std::vector<LayerDef::FeatureProperties>& feature_properties) {
    if (!sourceGeometryFilenameMatchesTargetLayer(source_geometry_path, target_layer_path)) {
        throw std::runtime_error(
            "source geometry filename must match target layer filename before writing canonical Vulkan binary");
    }
    saveCanonicalLayerBinary(target_layer_path, sig, features, feature_properties);
}

void saveCanonicalLayerBinary(
    const fs::path& target_layer_path,
    const std::string& sig,
    const std::vector<LayerDef::FeatureRecord>& features,
    const std::vector<LayerDef::FeatureProperties>& feature_properties) {
    const fs::path canonical_path = fs::path(target_layer_path.string() + ".canonical.bin");
    std::vector<LayerDef::FeatureRecord> canonical_features = features;
    ensureFeatureIdentityForLayerFile(target_layer_path.filename().string(), canonical_features);
    saveBinaryCanonicalFeatureCollection(canonical_path, sig, canonical_features, &feature_properties);
    std::error_code ec;
    fs::remove(target_layer_path, ec);
}

void persistCanonicalLayerBinaryAndRemoveGeoJson(const fs::path& geojson_path) {
    if (geojson_path.extension() != ".geojson") return;
    std::vector<LayerDef::FeatureProperties> feature_properties;
    std::vector<LayerDef::FeatureRecord> features = loadLayerPointsFromFile(geojson_path, &feature_properties);
    ensureFeatureIdentityForLayerFile(geojson_path.filename().string(), features);
    const std::string sig = fileSignature(geojson_path);
    const fs::path canonical_path = fs::path(geojson_path.string() + ".canonical.bin");
    saveBinaryCanonicalFeatureCollection(canonical_path, sig, features, &feature_properties);
    std::error_code ec;
    fs::remove(geojson_path, ec);
}
}
