#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

struct ProjectStateValidation {
    bool ok = true;
    int schema_version = 0;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

constexpr const char* kWorldsimProjectSchemaId = "worldsim3.project";
constexpr int kWorldsimProjectSchemaVersion = 3;

std::filesystem::path worldsimProjectsDir(const std::filesystem::path* root);
std::filesystem::path normalizeWorldsimProjectPath(const std::filesystem::path* root, const std::string& value);
nlohmann::json buildWorldsimProjectDocument(nlohmann::json state_payload);
const nlohmann::json* worldsimProjectStatePayload(const nlohmann::json& document);
ProjectStateValidation validateWorldsimProjectDocument(const nlohmann::json& document);
std::string summarizeProjectValidation(const ProjectStateValidation& validation);
