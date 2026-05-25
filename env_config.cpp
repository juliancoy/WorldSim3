#include "env_config.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string trimCopy(std::string s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(start, end - start);
}

std::string unquoteEnvValue(std::string value) {
    value = trimCopy(std::move(value));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        std::string out;
        out.reserve(value.size() - 2);
        for (size_t i = 1; i + 1 < value.size(); ++i) {
            if (value[i] == '\\' && i + 2 < value.size()) {
                out.push_back(value[i + 1]);
                ++i;
                continue;
            }
            out.push_back(value[i]);
        }
        return out;
    }
    if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'') {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

std::string quoteEnvValue(const std::string& value) {
    const bool needs_quotes =
        value.empty() ||
        std::any_of(value.begin(), value.end(), [](unsigned char ch) {
            return std::isspace(ch) || ch == '#' || ch == '"' || ch == '\'' || ch == '\\';
        });
    if (!needs_quotes) return value;
    std::string out = "\"";
    for (char ch : value) {
        if (ch == '\\' || ch == '"') out.push_back('\\');
        out.push_back(ch);
    }
    out.push_back('"');
    return out;
}

bool parseEnvAssignment(std::string line, std::string& key, std::string& value) {
    line = trimCopy(std::move(line));
    if (line.empty() || line[0] == '#') return false;
    if (line.rfind("export ", 0) == 0) line = trimCopy(line.substr(7));
    const size_t eq = line.find('=');
    if (eq == std::string::npos) return false;
    key = trimCopy(line.substr(0, eq));
    if (key.empty()) return false;
    value = unquoteEnvValue(line.substr(eq + 1));
    return true;
}

bool mutateDotEnvFile(
    const fs::path& root,
    std::string_view key,
    const std::string* value,
    std::string* error) {
    const fs::path path = dotEnvPath(root);
    std::vector<std::string> lines;
    {
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) lines.push_back(line);
    }

    bool found = false;
    std::vector<std::string> out_lines;
    out_lines.reserve(lines.size() + (value ? 1u : 0u));
    for (const auto& raw_line : lines) {
        std::string parsed_key;
        std::string parsed_value;
        if (parseEnvAssignment(raw_line, parsed_key, parsed_value) && parsed_key == key) {
            if (!found && value) {
                out_lines.push_back(std::string(key) + "=" + quoteEnvValue(*value));
            }
            found = true;
            continue;
        }
        out_lines.push_back(raw_line);
    }
    if (!found && value) {
        out_lines.push_back(std::string(key) + "=" + quoteEnvValue(*value));
    }

    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out) {
        if (error) *error = "failed to write " + path.string();
        return false;
    }
    for (size_t i = 0; i < out_lines.size(); ++i) {
        out << out_lines[i];
        if (i + 1 < out_lines.size()) out << '\n';
    }
    return true;
}

void setProcessEnvIfMissing(const std::string& key, const std::string& value) {
    const char* existing = std::getenv(key.c_str());
    if (existing && *existing) return;
#if defined(_WIN32)
    _putenv_s(key.c_str(), value.c_str());
#else
    setenv(key.c_str(), value.c_str(), 0);
#endif
}

} // namespace

fs::path dotEnvPath(const fs::path& root) {
    return root / ".env";
}

std::string loadDotEnvValue(const fs::path& root, std::string_view key) {
    std::ifstream in(dotEnvPath(root));
    std::string line;
    while (std::getline(in, line)) {
        std::string parsed_key;
        std::string parsed_value;
        if (!parseEnvAssignment(line, parsed_key, parsed_value)) continue;
        if (parsed_key == key) return parsed_value;
    }
    return {};
}

bool setDotEnvValue(
    const fs::path& root,
    std::string_view key,
    const std::string& value,
    std::string* error) {
    return mutateDotEnvFile(root, key, &value, error);
}

bool removeDotEnvValue(
    const fs::path& root,
    std::string_view key,
    std::string* error) {
    return mutateDotEnvFile(root, key, nullptr, error);
}

void applyDotEnvEnvironment(const fs::path& root) {
    std::ifstream in(dotEnvPath(root));
    std::string line;
    while (std::getline(in, line)) {
        std::string key;
        std::string value;
        if (!parseEnvAssignment(line, key, value)) continue;
        setProcessEnvIfMissing(key, value);
    }
}
