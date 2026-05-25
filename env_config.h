#pragma once

#include <filesystem>
#include <string>
#include <string_view>

std::filesystem::path dotEnvPath(const std::filesystem::path& root);
std::string loadDotEnvValue(const std::filesystem::path& root, std::string_view key);
bool setDotEnvValue(
    const std::filesystem::path& root,
    std::string_view key,
    const std::string& value,
    std::string* error = nullptr);
bool removeDotEnvValue(
    const std::filesystem::path& root,
    std::string_view key,
    std::string* error = nullptr);
void applyDotEnvEnvironment(const std::filesystem::path& root);
