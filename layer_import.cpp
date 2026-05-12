#include "layer_import.h"

#include <zlib.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
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

uint16_t le16(const std::vector<uint8_t>& b, size_t off) {
    if (off + 2 > b.size()) throw std::runtime_error("unexpected EOF");
    return (uint16_t)b[off] | ((uint16_t)b[off + 1] << 8);
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

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
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
    put("jurisdiction", jurisdiction);
    put("source_file", source_file);
    for (const auto& kv : props) put(kv.first, kv.second);
    auto get = [&](std::initializer_list<const char*> keys) -> std::string {
        for (const char* k : keys) {
            auto it = props.find(k);
            if (it != props.end() && !it->second.empty()) return it->second;
        }
        return {};
    };
    const std::string acct = get({"ACCTID", "ACCOUNTID", "ACCOUNT_ID"});
    const std::string parcel = get({"PARCEL", "MAP", "LOT"});
    put("source_parcel_id", !acct.empty() ? acct : parcel);
    put("account_id", acct);
    put("blocklot", !acct.empty() ? acct : parcel);
    put("address", get({"ADDRESS", "ADDR", "PREMISE_ADDRESS"}));
    put("owner", get({"OWNNAME1", "OWNER", "OWNER_NAME"}));
    put("sdat_link", get({"SDAT_Link", "SDAT_LINK"}));
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

void writeArcgisFeatureLayerGeoJson(const std::string& service_url, const fs::path& out_path) {
    if (service_url.empty()) throw std::runtime_error("missing ArcGIS service URL");
    json ids = json::parse(httpPostForm(service_url + "/query", {
        {"where", "1=1"},
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
    constexpr size_t page_size = 2000;
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
            normalizeBaltimoreCountyArcgisFeature(feature);
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
}

bool layerHasImportSource(const LayerDef& layer) {
    if (layer.import_type == "arcgis_feature_layer") return !layer.import_service_url.empty();
    return (layer.import_type == "zipped_shapefile" || layer.import_type == "socrata_csv_properties") &&
        !layer.import_url.empty();
}

VersionedDownloadResult downloadOrImportLayer(const LayerDef& layer, const fs::path& out_path, const fs::path& root) {
    if (!layer.source_url.empty()) {
        return downloadUrlVersioned(layer.source_url, out_path, root / "data" / "versions");
    }
    VersionedDownloadResult res;
    if (!layerHasImportSource(layer)) {
        res.message = "no source URL or import source";
        return res;
    }
    if (layer.import_type == "socrata_csv_properties") {
        const fs::path csv_path = root / "data" / "imports" / (out_path.filename().string() + ".source.csv");
        VersionedDownloadResult dl = downloadUrlVersioned(layer.import_url, csv_path, root / "data" / "versions");
        if (!dl.ok) return dl;
        try {
            writeSocrataHowardPropertyGeoJson(csv_path, out_path);
            res.ok = true;
            res.changed = true;
            res.not_modified = false;
            res.message = "imported Socrata CSV properties via " + dl.message;
        } catch (const std::exception& e) {
            res.ok = false;
            res.message = std::string("import failed: ") + e.what();
        }
        return res;
    }
    if (layer.import_type == "arcgis_feature_layer") {
        try {
            writeArcgisFeatureLayerGeoJson(layer.import_service_url, out_path);
            res.ok = true;
            res.changed = true;
            res.not_modified = false;
            res.message = "imported ArcGIS feature layer";
        } catch (const std::exception& e) {
            res.ok = false;
            res.message = std::string("import failed: ") + e.what();
        }
        return res;
    }
    if (layer.import_type != "zipped_shapefile" ||
        (layer.import_source_crs != "EPSG:2248" && layer.import_source_crs != "EPSG:26985")) {
        res.message = "unsupported import type/source CRS";
        return res;
    }
    const fs::path archive_path = root / "data" / "imports" / (out_path.filename().string() + ".source.zip");
    VersionedDownloadResult dl = downloadUrlVersioned(layer.import_url, archive_path, root / "data" / "versions");
    if (!dl.ok) return dl;
    try {
        const auto members = extractShapefileMembers(archive_path, layer.import_shapefile.empty() ? "Property.shp" : layer.import_shapefile);
        const auto dbf = parseDbf(members.at(".dbf"));
        const std::string collection_name = lower(out_path.stem().string());
        const std::string source_file = layer.import_shapefile.empty() ? "Property.shp" : layer.import_shapefile;
        const std::string jurisdiction =
            layer.name.size() > 8 && layer.name.ends_with(" Parcels")
                ? layer.name.substr(0, layer.name.size() - 8)
                : layer.name;
        writeStateplaneParcelShapefileGeoJson(
            members.at(".shp"),
            dbf,
            out_path,
            collection_name,
            jurisdiction,
            source_file,
            layer.import_source_crs == "EPSG:2248");
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
