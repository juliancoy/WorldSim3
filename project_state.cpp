#include "project_state.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <sstream>

using json = nlohmann::json;

namespace {
std::string trimCopy(std::string value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace((unsigned char)value[begin])) ++begin;
    size_t end = value.size();
    while (end > begin && std::isspace((unsigned char)value[end - 1])) --end;
    return value.substr(begin, end - begin);
}

std::string utcTimestampNow() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return os.str();
}

int documentVersion(const json& document) {
    if (document.contains("schema_version") && document["schema_version"].is_number_integer()) {
        return document["schema_version"].get<int>();
    }
    if (document.contains("version") && document["version"].is_number_integer()) {
        return document["version"].get<int>();
    }
    return 1;
}
}

std::filesystem::path worldsimProjectsDir(const std::filesystem::path* root) {
    return root ? *root / "data" / "projects" : std::filesystem::path("data/projects");
}

std::filesystem::path normalizeWorldsimProjectPath(const std::filesystem::path* root, const std::string& value) {
    std::filesystem::path path = trimCopy(value);
    if (path.empty()) path = "project.json";
    if (path.extension().empty()) path += ".json";
    if (path.is_relative()) path = worldsimProjectsDir(root) / path;
    return path.lexically_normal();
}

json buildWorldsimProjectDocument(json state_payload) {
    if (!state_payload.is_object()) state_payload = json::object();
    return {
        {"schema", kWorldsimProjectSchemaId},
        {"schema_version", kWorldsimProjectSchemaVersion},
        {"format", {
            {"application", "worldsim3"},
            {"kind", "project_state"},
            {"version", kWorldsimProjectSchemaVersion}
        }},
        {"metadata", {
            {"saved_at_utc", utcTimestampNow()},
            {"compatibility", {
                {"min_reader_schema_version", 1},
                {"current_writer_schema_version", kWorldsimProjectSchemaVersion}
            }}
        }},
        {"state", std::move(state_payload)}
    };
}

const json* worldsimProjectStatePayload(const json& document) {
    if (!document.is_object()) return nullptr;
    if (document.contains("state") && document["state"].is_object()) return &document["state"];
    return &document;
}

ProjectStateValidation validateWorldsimProjectDocument(const json& document) {
    ProjectStateValidation out;
    if (!document.is_object()) {
        out.ok = false;
        out.errors.push_back("Project file must be a JSON object.");
        return out;
    }

    out.schema_version = documentVersion(document);
    const std::string schema = document.value("schema", std::string());
    if (!schema.empty() && schema != kWorldsimProjectSchemaId) {
        out.ok = false;
        out.errors.push_back("Unsupported project schema: " + schema);
    }
    if (out.schema_version > kWorldsimProjectSchemaVersion) {
        out.ok = false;
        out.errors.push_back("Project schema is newer than this build supports.");
    }
    if (!document.contains("state")) {
        out.warnings.push_back("Loaded legacy v1 project without a state envelope.");
    } else if (!document["state"].is_object()) {
        out.ok = false;
        out.errors.push_back("Project state payload must be an object.");
    }

    const json* state = worldsimProjectStatePayload(document);
    if (!state || !state->is_object()) {
        out.ok = false;
        out.errors.push_back("Project state payload is missing.");
        return out;
    }
    for (const char* required : {"app_settings", "map", "layers"}) {
        if (!state->contains(required)) {
            out.warnings.push_back(std::string("Project state is missing optional section: ") + required);
        }
    }
    return out;
}

std::string summarizeProjectValidation(const ProjectStateValidation& validation) {
    if (!validation.ok && !validation.errors.empty()) return validation.errors.front();
    if (!validation.warnings.empty()) return validation.warnings.front();
    return {};
}
