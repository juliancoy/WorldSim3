#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
struct Options {
    fs::path coc_geojson = "data/layers/hud_coc_boundaries_maryland.geojson";
    fs::path inbox_dir = "data/inbox/hud_pit";
    std::optional<fs::path> pit_csv;
    fs::path out_geojson = "data/layers/hud_pit_homelessness_2007_2024_by_coc.geojson";
};

std::string trim(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

std::string toUpper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::toupper(c); });
    return s;
}

std::string normalizeCoC(std::string v) {
    v = toUpper(trim(std::move(v)));
    v.erase(std::remove(v.begin(), v.end(), ' '), v.end());
    std::regex pat("^([A-Z]{2})[-_]?([0-9]{3})$");
    std::smatch m;
    if (std::regex_match(v, m, pat)) return m[1].str() + "-" + m[2].str();
    return v;
}

bool parseDouble(const std::string& s, double& out) {
    std::string t = trim(s);
    if (t.empty()) return false;
    t.erase(std::remove(t.begin(), t.end(), ','), t.end());
    char* end = nullptr;
    out = std::strtod(t.c_str(), &end);
    return end && *end == '\0';
}

std::vector<std::string> parseCsvLine(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool in_quotes = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '"') {
            if (in_quotes && i + 1 < line.size() && line[i + 1] == '"') {
                cur.push_back('"');
                ++i;
            } else {
                in_quotes = !in_quotes;
            }
        } else if (c == ',' && !in_quotes) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

fs::path resolvePitCsv(const Options& opt) {
    if (opt.pit_csv.has_value()) return *opt.pit_csv;
    if (!fs::exists(opt.inbox_dir)) return {};
    fs::path best;
    fs::file_time_type best_time{};
    for (const auto& ent : fs::directory_iterator(opt.inbox_dir)) {
        if (!ent.is_regular_file()) continue;
        const auto ext = toUpper(ent.path().extension().string());
        if (ext != ".CSV") continue;
        auto t = ent.last_write_time();
        if (best.empty() || t > best_time) {
            best = ent.path();
            best_time = t;
        }
    }
    return best;
}

int findColumn(const std::vector<std::string>& headers, const std::vector<std::string>& candidates) {
    std::vector<std::string> norm;
    norm.reserve(headers.size());
    for (const auto& h : headers) norm.push_back(toUpper(trim(h)));
    for (size_t i = 0; i < norm.size(); ++i) {
        for (const auto& c : candidates) {
            if (norm[i] == toUpper(c)) return (int)i;
        }
    }
    for (size_t i = 0; i < norm.size(); ++i) {
        for (const auto& c : candidates) {
            if (norm[i].find(toUpper(c)) != std::string::npos) return (int)i;
        }
    }
    return -1;
}

std::map<int, int> findYearColumns(const std::vector<std::string>& headers) {
    std::map<int, int> year_to_col;
    std::regex ypat("(20[0-9]{2})");
    for (size_t i = 0; i < headers.size(); ++i) {
        std::smatch m;
        const std::string h = trim(headers[i]);
        if (std::regex_search(h, m, ypat)) {
            int y = std::stoi(m[1].str());
            if (y >= 2000 && y <= 2035 && !year_to_col.count(y)) year_to_col[y] = (int)i;
        }
    }
    return year_to_col;
}

Options parseArgs(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << flag << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--coc-geojson") opt.coc_geojson = next("--coc-geojson");
        else if (a == "--pit-csv") opt.pit_csv = fs::path(next("--pit-csv"));
        else if (a == "--inbox-dir") opt.inbox_dir = next("--inbox-dir");
        else if (a == "--out") opt.out_geojson = next("--out");
        else if (a == "--help" || a == "-h") {
            std::cout
                << "Usage: worldsim_hud_pit_builder [--pit-csv FILE.csv] [--inbox-dir DIR] [--coc-geojson FILE] [--out FILE]\n"
                << "If --pit-csv is not provided, newest CSV from data/inbox/hud_pit is used.\n";
            std::exit(0);
        } else {
            std::cerr << "Unknown arg: " << a << "\n";
            std::exit(2);
        }
    }
    return opt;
}
} // namespace

int main(int argc, char** argv) {
    const Options opt = parseArgs(argc, argv);
    const fs::path pit_csv = resolvePitCsv(opt);
    if (pit_csv.empty() || !fs::exists(pit_csv)) {
        std::cerr << "No PIT CSV found. Place CSV in data/inbox/hud_pit or pass --pit-csv.\n";
        return 2;
    }
    if (!fs::exists(opt.coc_geojson)) {
        std::cerr << "Missing CoC geometry: " << opt.coc_geojson << "\n";
        return 2;
    }

    std::ifstream csv_in(pit_csv);
    if (!csv_in) {
        std::cerr << "Failed to open CSV: " << pit_csv << "\n";
        return 2;
    }

    std::string line;
    if (!std::getline(csv_in, line)) {
        std::cerr << "CSV is empty: " << pit_csv << "\n";
        return 2;
    }
    std::vector<std::string> headers = parseCsvLine(line);
    int coc_col = findColumn(headers, {"COCNUM", "CoC Number", "CoC", "HudNum"});
    if (coc_col < 0) {
        std::cerr << "Could not find CoC column in CSV header.\n";
        return 2;
    }
    const auto year_cols = findYearColumns(headers);
    if (year_cols.size() < 3) {
        std::cerr << "Could not find PIT year columns in CSV header.\n";
        return 2;
    }

    std::unordered_map<std::string, std::map<int, double>> agg;
    size_t rows = 0;
    while (std::getline(csv_in, line)) {
        auto cols = parseCsvLine(line);
        if ((int)cols.size() <= coc_col) continue;
        const std::string coc = normalizeCoC(cols[(size_t)coc_col]);
        if (coc.empty()) continue;
        rows++;
        auto& ymap = agg[coc];
        for (const auto& [year, idx] : year_cols) {
            if ((int)cols.size() <= idx) continue;
            double v = 0.0;
            if (parseDouble(cols[(size_t)idx], v)) ymap[year] += v;
        }
    }

    std::ifstream gj_in(opt.coc_geojson);
    if (!gj_in) {
        std::cerr << "Failed to open GeoJSON: " << opt.coc_geojson << "\n";
        return 2;
    }
    json fc;
    gj_in >> fc;
    if (!fc.contains("features") || !fc["features"].is_array()) {
        std::cerr << "Invalid GeoJSON features array.\n";
        return 2;
    }

    int min_year = year_cols.begin()->first;
    int max_year = year_cols.rbegin()->first;
    size_t matched = 0;
    for (auto& feat : fc["features"]) {
        auto& p = feat["properties"];
        const std::string coc = normalizeCoC(p.value("COCNUM", p.value("HudNum", std::string())));
        auto it = agg.find(coc);
        if (it == agg.end()) continue;
        matched++;
        const auto& ymap = it->second;
        double first_val = 0.0, last_val = 0.0;
        for (const auto& [y, _] : year_cols) {
            auto v_it = ymap.find(y);
            double v = (v_it == ymap.end()) ? 0.0 : v_it->second;
            p["PIT_" + std::to_string(y) + "_Total"] = v;
            if (y == min_year) first_val = v;
            if (y == max_year) last_val = v;
        }
        p["PIT_First_Year"] = min_year;
        p["PIT_Last_Year"] = max_year;
        p["PIT_Change_Abs"] = last_val - first_val;
        if (first_val > 0.0) p["PIT_Change_Pct"] = ((last_val - first_val) / first_val) * 100.0;
        else p["PIT_Change_Pct"] = nullptr;
    }

    fc["name"] = "HUD PIT Homelessness Trends by CoC (2007-2024)";
    fc["source"] = pit_csv.string();
    fc["join_key"] = "COCNUM";
    fc["matched_features"] = matched;
    fc["rows_in_pit_csv"] = rows;

    fs::create_directories(opt.out_geojson.parent_path());
    std::ofstream out(opt.out_geojson);
    out << fc.dump();

    std::cout << "rows in PIT CSV: " << rows << "\n";
    std::cout << "matched CoC features: " << matched << " / " << fc["features"].size() << "\n";
    std::cout << "source PIT CSV: " << pit_csv << "\n";
    std::cout << "wrote: " << opt.out_geojson << "\n";
    return 0;
}
