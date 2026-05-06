#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

struct PublicServantRosterRow {
    std::string source_id;
    std::string source_type;
    std::string jurisdiction;
    std::string employer;
    std::string agency;
    std::string person_name;
    std::string role_title;
    std::string pay_grade;
    std::string annual_salary;
    std::string gross_pay;
    std::string fiscal_year;
    std::string source_url;
    std::string provenance_note;
};

struct PolicyVizNode {
    std::string label;
    size_t personnel = 0;
    double pay_total = 0.0;
    double value = 0.0;
    std::vector<PolicyVizNode> children;
};

struct PolicyPanelContext {
    nlohmann::json* hierarchy = nullptr;
    bool hierarchy_loaded = false;
    std::string* hierarchy_error = nullptr;
    char* query = nullptr;
    size_t query_capacity = 0;
    int* scope = nullptr;
    std::vector<PublicServantRosterRow>* roster = nullptr;
    std::string* people_pay_cached_query = nullptr;
    int* people_pay_cached_scope = nullptr;
    size_t* people_pay_cache_matched_count = nullptr;
    std::vector<size_t>* people_pay_visible_rows = nullptr;
    size_t* people_pay_cache_rebuilds = nullptr;
    size_t* people_pay_rendered_rows_last = nullptr;
    PolicyVizNode* viz_root = nullptr;
    std::string* viz_cached_query = nullptr;
    int* viz_cached_scope = nullptr;
    int* viz_cached_metric = nullptr;
    int* viz_metric = nullptr;
    size_t* viz_cache_rebuilds = nullptr;
    size_t* viz_node_count = nullptr;
};

void drawPolicyHierarchyTab(PolicyPanelContext& ctx);
