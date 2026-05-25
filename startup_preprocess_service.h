#pragma once

#include "app_settings.h"
#include "worldsim_cli.h"

#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

struct StartupPreprocessIssue {
    std::string kind;
    std::string layer_file;
    std::string message;
    std::string artifact_path;
};

struct StartupPreprocessPlan {
    bool required = false;
    bool duckdb_required = false;
    std::vector<StartupPreprocessIssue> issues;
};

struct StartupPreprocessCommandStep {
    std::string kind;
    std::string layer_file;
    std::string command;
};

void printStartupPreprocessPlan(const StartupPreprocessPlan& plan, std::ostream& out);
StartupPreprocessPlan inspectStartupPreprocessPlan(const std::filesystem::path& root);
int runStartupPreprocessWindow(
    const std::filesystem::path& root,
    const AppSettings& app_settings,
    const StartupPreprocessPlan& initial_plan,
    const WorldsimCliOptions& cli_options,
    const char* argv0);
int runStartupPreprocessCli(
    const std::filesystem::path& root,
    const AppSettings& app_settings,
    const StartupPreprocessPlan& plan,
    const WorldsimCliOptions& cli_options,
    const char* argv0);
