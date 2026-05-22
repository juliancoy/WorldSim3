#pragma once

#include "filters.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

struct RepeatableFilterFieldSpec {
    enum class Type {
        Text,
        Number,
        Boolean
    };

    std::string column;
    Type type = Type::Text;
};

std::string sanitizeRepeatableFilterId(const std::string& raw);
std::filesystem::path repeatableFilterDir(const std::filesystem::path& root);
std::filesystem::path repeatableFilterPath(const std::filesystem::path& root, const std::string& id);

const std::unordered_map<std::string, RepeatableFilterFieldSpec>& repeatableFilterFieldSpecs();

bool validateRepeatableFilterSpec(nlohmann::json spec, std::string& error);
std::string repeatableFilterSql(const nlohmann::json& spec);
nlohmann::json repeatableFilterSummary(const nlohmann::json& spec);
nlohmann::json loadRepeatableFilterSpec(const std::filesystem::path& root, const std::string& id);
std::vector<nlohmann::json> loadAllRepeatableFilterSpecs(const std::filesystem::path& root);
bool saveRepeatableFilterSpec(const std::filesystem::path& root, nlohmann::json spec, std::string& error);
bool deleteRepeatableFilterSpec(const std::filesystem::path& root, const std::string& id);

nlohmann::json queryExecutionSnapshotJson(const QueryExecutionContextSnapshot& snapshot);
nlohmann::json queryHistoryEntryJson(const QueryHistoryEntry& entry);
nlohmann::json makeSqlRepeatableFilterSpec(
    const QueryHistoryEntry& entry,
    const std::string& id,
    const std::string& name,
    int version,
    const std::string& source_kind);
