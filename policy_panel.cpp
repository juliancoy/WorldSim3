#include "policy_panel.h"

#include "app_utils.h"
#include "feature_props.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using json = nlohmann::json;

void drawPolicyHierarchyTab(PolicyPanelContext& ctx) {
    auto& policy_hierarchy = *ctx.hierarchy;
    const bool policy_hierarchy_loaded = ctx.hierarchy_loaded;
    auto& policy_hierarchy_error = *ctx.hierarchy_error;
    char* policy_hierarchy_query = ctx.query;
    const size_t policy_hierarchy_query_capacity = ctx.query_capacity;
    auto& policy_hierarchy_scope = *ctx.scope;
    auto& public_servant_roster = *ctx.roster;
    auto& people_pay_cached_query = *ctx.people_pay_cached_query;
    auto& people_pay_cached_scope = *ctx.people_pay_cached_scope;
    auto& people_pay_cache_matched_count = *ctx.people_pay_cache_matched_count;
    auto& people_pay_visible_rows = *ctx.people_pay_visible_rows;
    auto& people_pay_cache_rebuilds = *ctx.people_pay_cache_rebuilds;
    auto& people_pay_rendered_rows_last = *ctx.people_pay_rendered_rows_last;
    auto& policy_viz_root = *ctx.viz_root;
    auto& policy_viz_cached_query = *ctx.viz_cached_query;
    auto& policy_viz_cached_scope = *ctx.viz_cached_scope;
    auto& policy_viz_cached_metric = *ctx.viz_cached_metric;
    auto& policy_viz_metric = *ctx.viz_metric;
    auto& policy_viz_cache_rebuilds = *ctx.viz_cache_rebuilds;
    auto& policy_viz_node_count = *ctx.viz_node_count;

    if (ImGui::BeginTabItem("Policy Hierarchy")) {
        ImGui::TextUnformatted("Policy and Authority Hierarchy");
        ImGui::Separator();
        ImGui::TextWrapped("Data-backed view of Maryland, Baltimore, and salient federal authority structures with federal pay schedule references.");
        ImGui::TextDisabled("Source: data/government/government_hierarchy_and_pay_2026.json");
        if (!policy_hierarchy_loaded) {
            ImGui::TextColored(ImVec4(0.75f, 0.22f, 0.16f, 1.0f), "Could not load hierarchy: %s", policy_hierarchy_error.c_str());
            ImGui::EndTabItem();
        } else {
            ImGui::InputTextWithHint("##policy_hierarchy_query", "Search agency, position, section, branch, schedule...", policy_hierarchy_query, policy_hierarchy_query_capacity);
            ImGui::SameLine();
            if (ImGui::Button("Clear")) policy_hierarchy_query[0] = '\0';
            const char* scopes[] = {"All", "Maryland", "Baltimore", "Federal"};
            ImGui::Combo("Scope", &policy_hierarchy_scope, scopes, IM_ARRAYSIZE(scopes));
            const std::string policy_query = trimDisplayValue(policy_hierarchy_query);
            auto item_matches = [&](const std::string& a, const std::string& b = {}, const std::string& c = {}, const std::string& d = {}) {
                if (policy_query.empty()) return true;
                return containsCaseInsensitive(a, policy_query) ||
                       containsCaseInsensitive(b, policy_query) ||
                       containsCaseInsensitive(c, policy_query) ||
                       containsCaseInsensitive(d, policy_query);
            };
            auto shorten_for_ui = [](const std::string& value, size_t max_chars = 72) {
                if (value.size() <= max_chars || max_chars <= 3) return value;
                return value.substr(0, max_chars - 3) + "...";
            };
            auto draw_clipped_text = [&](const std::string& value, size_t max_chars = 72) {
                const std::string full = value.empty() ? std::string("-") : value;
                const std::string shown = shorten_for_ui(full, max_chars);
                ImGui::TextUnformatted(shown.c_str());
                if (shown.size() != full.size() && ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 80.0f);
                    ImGui::TextUnformatted(full.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                }
            };
            auto draw_source_chip = [&](const std::string& value) {
                if (value.empty()) return;
                ImGui::TextDisabled("%s", shorten_for_ui(value, 64).c_str());
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 80.0f);
                    ImGui::TextUnformatted(value.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                }
            };
            auto roster_row_matches_query = [&](const PublicServantRosterRow& row) {
                if (policy_query.empty()) return true;
                return containsCaseInsensitive(row.jurisdiction, policy_query) ||
                       containsCaseInsensitive(row.employer, policy_query) ||
                       containsCaseInsensitive(row.person_name, policy_query) ||
                       containsCaseInsensitive(row.agency, policy_query) ||
                       containsCaseInsensitive(row.role_title, policy_query) ||
                       containsCaseInsensitive(row.pay_grade, policy_query) ||
                       containsCaseInsensitive(row.annual_salary, policy_query) ||
                       containsCaseInsensitive(row.gross_pay, policy_query) ||
                       containsCaseInsensitive(row.fiscal_year, policy_query) ||
                       containsCaseInsensitive(row.source_id, policy_query);
            };
            auto parse_pay_amount = [](const std::string& value) {
                std::string cleaned;
                cleaned.reserve(value.size());
                bool seen_digit = false;
                for (char c : value) {
                    if ((c >= '0' && c <= '9') || c == '.' || c == '-') {
                        cleaned.push_back(c);
                        if (c >= '0' && c <= '9') seen_digit = true;
                    }
                }
                if (!seen_digit) return 0.0;
                try { return std::stod(cleaned); } catch (...) { return 0.0; }
            };
            auto format_money_compact = [](double value) {
                std::ostringstream os;
                if (value >= 1000000000.0) os << "$" << std::fixed << std::setprecision(1) << (value / 1000000000.0) << "B";
                else if (value >= 1000000.0) os << "$" << std::fixed << std::setprecision(1) << (value / 1000000.0) << "M";
                else if (value >= 1000.0) os << "$" << std::fixed << std::setprecision(1) << (value / 1000.0) << "K";
                else os << "$" << std::fixed << std::setprecision(0) << value;
                return os.str();
            };
            auto sort_policy_viz = [&](auto&& self, PolicyVizNode& node) -> void {
                for (auto& child : node.children) self(self, child);
                std::sort(node.children.begin(), node.children.end(), [](const PolicyVizNode& a, const PolicyVizNode& b) {
                    if (a.value == b.value) return a.label < b.label;
                    return a.value > b.value;
                });
            };
            auto count_policy_viz = [&](auto&& self, const PolicyVizNode& node) -> size_t {
                size_t n = 1;
                for (const auto& child : node.children) n += self(self, child);
                return n;
            };
            auto rebuild_policy_viz = [&]() {
                struct Agg {
                    std::string label;
                    std::string parent;
                    size_t personnel = 0;
                    double pay_total = 0.0;
                };
                std::unordered_map<std::string, Agg> aggs;
                auto clean_label = [](const std::string& value, const char* fallback) {
                    std::string trimmed = trimDisplayValue(value);
                    return trimmed.empty() ? std::string(fallback) : trimmed;
                };
                auto add_agg = [&](const std::string& key, const std::string& parent, const std::string& label, double pay) {
                    Agg& agg = aggs[key];
                    if (agg.label.empty()) agg.label = label;
                    if (agg.parent.empty()) agg.parent = parent;
                    agg.personnel++;
                    agg.pay_total += pay;
                };
                for (const auto& row : public_servant_roster) {
                    if (policy_hierarchy_scope == 1 && row.jurisdiction != "Maryland") continue;
                    if (policy_hierarchy_scope == 2 && row.jurisdiction != "Baltimore City") continue;
                    if (policy_hierarchy_scope == 3 && row.jurisdiction != "Federal") continue;
                    if (!roster_row_matches_query(row)) continue;
                    const std::string jurisdiction = clean_label(row.jurisdiction, "Unknown Jurisdiction");
                    const std::string employer = clean_label(row.employer, "Unknown Employer");
                    const std::string agency = clean_label(row.agency, "Unknown Agency");
                    const std::string role = clean_label(row.role_title, "Unknown Role");
                    double pay = parse_pay_amount(row.annual_salary);
                    if (pay <= 0.0) pay = parse_pay_amount(row.gross_pay);
                    const std::string k_j = jurisdiction;
                    const std::string k_e = k_j + "\x1f" + employer;
                    const std::string k_a = k_e + "\x1f" + agency;
                    const std::string k_r = k_a + "\x1f" + role;
                    add_agg(k_j, "", jurisdiction, pay);
                    add_agg(k_e, k_j, employer, pay);
                    add_agg(k_a, k_e, agency, pay);
                    add_agg(k_r, k_a, role, pay);
                }
                std::unordered_map<std::string, std::vector<std::string>> children_by_parent;
                for (const auto& kv : aggs) children_by_parent[kv.second.parent].push_back(kv.first);
                auto build_node = [&](auto&& self, const std::string& key) -> PolicyVizNode {
                    const Agg& agg = aggs[key];
                    PolicyVizNode node;
                    node.label = agg.label;
                    node.personnel = agg.personnel;
                    node.pay_total = agg.pay_total;
                    node.value = (policy_viz_metric == 0) ? static_cast<double>(agg.personnel) : agg.pay_total;
                    for (const std::string& child_key : children_by_parent[key]) node.children.push_back(self(self, child_key));
                    return node;
                };
                policy_viz_root = {};
                policy_viz_root.label = "Public Sector";
                for (const std::string& key : children_by_parent[""]) policy_viz_root.children.push_back(build_node(build_node, key));
                for (const auto& child : policy_viz_root.children) {
                    policy_viz_root.personnel += child.personnel;
                    policy_viz_root.pay_total += child.pay_total;
                }
                policy_viz_root.value = (policy_viz_metric == 0) ? static_cast<double>(policy_viz_root.personnel) : policy_viz_root.pay_total;
                sort_policy_viz(sort_policy_viz, policy_viz_root);
                policy_viz_node_count = count_policy_viz(count_policy_viz, policy_viz_root);
                policy_viz_cached_query = policy_query;
                policy_viz_cached_scope = policy_hierarchy_scope;
                policy_viz_cached_metric = policy_viz_metric;
                policy_viz_cache_rebuilds++;
            };
            auto ensure_policy_viz = [&]() {
                if (policy_viz_cached_query != policy_query ||
                    policy_viz_cached_scope != policy_hierarchy_scope ||
                    policy_viz_cached_metric != policy_viz_metric) {
                    rebuild_policy_viz();
                }
            };
            auto value_label = [&]() {
                return policy_viz_metric == 0 ? std::string("personnel") : std::string("pay");
            };
            auto node_value_label = [&](const PolicyVizNode& node) {
                if (policy_viz_metric == 0) return std::to_string(node.personnel) + " people";
                return format_money_compact(node.pay_total);
            };
            auto draw_policy_tooltip = [&](const PolicyVizNode& node) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(node.label.c_str());
                ImGui::Separator();
                ImGui::Text("Personnel rows: %zu", node.personnel);
                ImGui::Text("Parsed pay total: %s", format_money_compact(node.pay_total).c_str());
                ImGui::Text("Children: %zu", node.children.size());
                ImGui::EndTooltip();
            };
            auto draw_treemap_node = [&](auto&& self, ImDrawList* dl, const PolicyVizNode& node, ImVec2 minp, ImVec2 maxp, int depth) -> void {
                if (maxp.x - minp.x < 2.0f || maxp.y - minp.y < 2.0f || node.value <= 0.0) return;
                const ImU32 base = ImGui::ColorConvertFloat4ToU32(colorFromStableKey(node.label));
                const float shade = std::clamp(0.92f - depth * 0.07f, 0.55f, 0.92f);
                ImVec4 c = ImGui::ColorConvertU32ToFloat4(base);
                c.x *= shade; c.y *= shade; c.z *= shade; c.w = 0.92f;
                dl->AddRectFilled(minp, maxp, ImGui::ColorConvertFloat4ToU32(c));
                dl->AddRect(minp, maxp, IM_COL32(255, 255, 255, 210));
                const float w = maxp.x - minp.x;
                const float h = maxp.y - minp.y;
                if (w > 86.0f && h > 34.0f) {
                    const std::string label = shorten_for_ui(node.label, static_cast<size_t>(std::max(10.0f, w / 7.5f)));
                    dl->AddText(ImVec2(minp.x + 5.0f, minp.y + 4.0f), IM_COL32(24, 28, 32, 255), label.c_str());
                    if (h > 52.0f) {
                        const std::string v = node_value_label(node);
                        dl->AddText(ImVec2(minp.x + 5.0f, minp.y + 21.0f), IM_COL32(40, 45, 50, 230), v.c_str());
                    }
                }
                ImGui::SetCursorScreenPos(minp);
                ImGui::PushID(&node);
                ImGui::InvisibleButton("treemap_node", ImVec2(w, h));
                if (ImGui::IsItemHovered()) draw_policy_tooltip(node);
                ImGui::PopID();
                if (node.children.empty() || depth >= 5 || w < 40.0f || h < 30.0f) return;
                const float pad = 3.0f;
                ImVec2 inner_min(minp.x + pad, minp.y + ((h > 58.0f) ? 38.0f : pad));
                ImVec2 inner_max(maxp.x - pad, maxp.y - pad);
                if (inner_max.x <= inner_min.x || inner_max.y <= inner_min.y) return;
                const double total = std::max(0.000001, node.value);
                float cursor = ((inner_max.x - inner_min.x) >= (inner_max.y - inner_min.y)) ? inner_min.x : inner_min.y;
                const bool split_x = (inner_max.x - inner_min.x) >= (inner_max.y - inner_min.y);
                size_t drawn = 0;
                for (const auto& child : node.children) {
                    if (child.value <= 0.0) continue;
                    drawn++;
                    if (drawn > 80) break;
                    const float frac = static_cast<float>(child.value / total);
                    if (split_x) {
                        const float nx = (drawn == node.children.size()) ? inner_max.x : std::min(inner_max.x, cursor + (inner_max.x - inner_min.x) * frac);
                        self(self, dl, child, ImVec2(cursor, inner_min.y), ImVec2(nx, inner_max.y), depth + 1);
                        cursor = nx;
                    } else {
                        const float ny = (drawn == node.children.size()) ? inner_max.y : std::min(inner_max.y, cursor + (inner_max.y - inner_min.y) * frac);
                        self(self, dl, child, ImVec2(inner_min.x, cursor), ImVec2(inner_max.x, ny), depth + 1);
                        cursor = ny;
                    }
                    if ((split_x && cursor >= inner_max.x - 1.0f) || (!split_x && cursor >= inner_max.y - 1.0f)) break;
                }
            };
            auto draw_arc_segment = [](ImDrawList* dl, ImVec2 center, float r0, float r1, float a0, float a1, ImU32 color) {
                if (a1 <= a0 || r1 <= r0) return;
                const int segments = std::clamp(static_cast<int>((a1 - a0) * r1 / 6.0f), 8, 96);
                std::vector<ImVec2> pts;
                pts.reserve(static_cast<size_t>(segments * 2 + 2));
                for (int i = 0; i <= segments; ++i) {
                    const float t = a0 + (a1 - a0) * (static_cast<float>(i) / static_cast<float>(segments));
                    pts.push_back(ImVec2(center.x + std::cos(t) * r1, center.y + std::sin(t) * r1));
                }
                for (int i = segments; i >= 0; --i) {
                    const float t = a0 + (a1 - a0) * (static_cast<float>(i) / static_cast<float>(segments));
                    pts.push_back(ImVec2(center.x + std::cos(t) * r0, center.y + std::sin(t) * r0));
                }
                dl->AddConvexPolyFilled(pts.data(), static_cast<int>(pts.size()), color);
                dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), IM_COL32(255, 255, 255, 190), ImDrawFlags_Closed, 1.0f);
            };
            auto draw_sunburst_node = [&](auto&& self, ImDrawList* dl, const PolicyVizNode& node, ImVec2 center, float a0, float a1, float ring, float ring_w, int depth) -> void {
                if (node.value <= 0.0 || a1 <= a0 || depth > 5) return;
                const float r0 = ring + depth * ring_w;
                const float r1 = r0 + ring_w - 2.0f;
                const ImU32 color = ImGui::ColorConvertFloat4ToU32(colorFromStableKey(node.label));
                draw_arc_segment(dl, center, r0, r1, a0, a1, color);
                const float mid = (a0 + a1) * 0.5f;
                const float mx = center.x + std::cos(mid) * ((r0 + r1) * 0.5f);
                const float my = center.y + std::sin(mid) * ((r0 + r1) * 0.5f);
                ImGui::SetCursorScreenPos(ImVec2(mx - 6.0f, my - 6.0f));
                ImGui::PushID(&node);
                ImGui::InvisibleButton("sunburst_node", ImVec2(12.0f, 12.0f));
                if (ImGui::IsItemHovered()) draw_policy_tooltip(node);
                ImGui::PopID();
                if ((a1 - a0) > 0.22f && depth <= 1) {
                    const std::string label = shorten_for_ui(node.label, 18);
                    dl->AddText(ImVec2(mx + 4.0f, my - 7.0f), IM_COL32(25, 28, 32, 230), label.c_str());
                }
                if (node.children.empty()) return;
                float cursor = a0;
                const double total = std::max(0.000001, node.value);
                size_t drawn = 0;
                for (const auto& child : node.children) {
                    if (child.value <= 0.0) continue;
                    drawn++;
                    if (drawn > 96) break;
                    const float na = cursor + (a1 - a0) * static_cast<float>(child.value / total);
                    self(self, dl, child, center, cursor, na, ring, ring_w, depth + 1);
                    cursor = na;
                }
            };
            auto count_section_items = [&](const char* jurisdiction) {
                size_t total = 0;
                const json& node = policy_hierarchy[jurisdiction];
                if (!node.contains("sections") || !node["sections"].is_object()) return total;
                for (auto it = node["sections"].begin(); it != node["sections"].end(); ++it) {
                    if (it.value().is_array()) total += it.value().size();
                }
                return total;
            };
            const size_t maryland_total = count_section_items("maryland");
            const size_t baltimore_total = count_section_items("baltimore");
            const size_t federal_positions = policy_hierarchy["federal"].value("salient_positions", json::array()).size();
            if (ImGui::BeginTabBar("policy_hierarchy_tabs")) {
                if (ImGui::BeginTabItem("Overview")) {
                    ImGui::Text("Generated: %s", policy_hierarchy.value("generated_at", "unknown").c_str());
                    ImGui::Text("Maryland entries: %zu", maryland_total);
                    ImGui::Text("Baltimore entries: %zu", baltimore_total);
                    ImGui::Text("Federal salient positions: %zu", federal_positions);
                    ImGui::SeparatorText("Use");
                    ImGui::TextWrapped("Use Hierarchy to find agencies/offices, Federal Positions to inspect role-to-pay references, and Pay Schedule to see Executive Schedule levels.");
                    ImGui::TextWrapped("This is an authority/reference model, not proof of reporting lines. Treat links and pay references as source-backed metadata.");
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Treemap")) {
                    ensure_policy_viz();
                    ImGui::TextDisabled("Nested boxes: jurisdiction -> employer -> agency -> role. Area is proportional to selected metric.");
                    ImGui::RadioButton("Personnel count", &policy_viz_metric, 0);
                    ImGui::SameLine();
                    ImGui::RadioButton("Parsed pay total", &policy_viz_metric, 1);
                    ensure_policy_viz();
                    ImGui::SameLine();
                    ImGui::TextDisabled("Metric: %s | nodes: %zu | rebuilds: %zu", value_label().c_str(), policy_viz_node_count, policy_viz_cache_rebuilds);
                    const ImVec2 canvas_size(-1.0f, 520.0f);
                    ImGui::BeginChild("policy_treemap_canvas", canvas_size, true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    const ImVec2 p0 = ImGui::GetCursorScreenPos();
                    const ImVec2 avail = ImGui::GetContentRegionAvail();
                    const ImVec2 p1(p0.x + std::max(100.0f, avail.x), p0.y + std::max(100.0f, avail.y));
                    dl->AddRectFilled(p0, p1, IM_COL32(246, 248, 242, 255));
                    if (policy_viz_root.value <= 0.0 || policy_viz_root.children.empty()) {
                        dl->AddText(ImVec2(p0.x + 16.0f, p0.y + 16.0f), IM_COL32(120, 80, 50, 255), "No matching personnel/pay rows for this scope and search.");
                    } else {
                        draw_treemap_node(draw_treemap_node, dl, policy_viz_root, ImVec2(p0.x + 8.0f, p0.y + 8.0f), ImVec2(p1.x - 8.0f, p1.y - 8.0f), 0);
                    }
                    ImGui::SetCursorScreenPos(p1);
                    ImGui::EndChild();
                    ImGui::TextDisabled("Pay totals are parsed from annual salary first, then gross pay. Rows without numeric pay still count for personnel.");
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Sunburst")) {
                    ensure_policy_viz();
                    ImGui::TextDisabled("Radial hierarchy: center=root, outer rings descend through jurisdiction, employer, agency, and role.");
                    ImGui::RadioButton("Personnel count", &policy_viz_metric, 0);
                    ImGui::SameLine();
                    ImGui::RadioButton("Parsed pay total", &policy_viz_metric, 1);
                    ensure_policy_viz();
                    ImGui::SameLine();
                    ImGui::TextDisabled("Metric: %s | nodes: %zu | rebuilds: %zu", value_label().c_str(), policy_viz_node_count, policy_viz_cache_rebuilds);
                    ImGui::BeginChild("policy_sunburst_canvas", ImVec2(0, 560), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    const ImVec2 p0 = ImGui::GetCursorScreenPos();
                    const ImVec2 avail = ImGui::GetContentRegionAvail();
                    const ImVec2 p1(p0.x + std::max(100.0f, avail.x), p0.y + std::max(100.0f, avail.y));
                    dl->AddRectFilled(p0, p1, IM_COL32(247, 246, 240, 255));
                    const ImVec2 center((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
                    const float max_r = std::min(p1.x - p0.x, p1.y - p0.y) * 0.46f;
                    if (policy_viz_root.value <= 0.0 || policy_viz_root.children.empty()) {
                        dl->AddText(ImVec2(p0.x + 16.0f, p0.y + 16.0f), IM_COL32(120, 80, 50, 255), "No matching personnel/pay rows for this scope and search.");
                    } else {
                        dl->AddCircleFilled(center, max_r * 0.10f, IM_COL32(55, 65, 60, 255), 48);
                        const std::string root_label = node_value_label(policy_viz_root);
                        dl->AddText(ImVec2(center.x - 42.0f, center.y - 7.0f), IM_COL32(255, 255, 255, 245), root_label.c_str());
                        constexpr float kPi = 3.14159265358979323846f;
                        draw_sunburst_node(draw_sunburst_node, dl, policy_viz_root, center, -kPi * 0.5f, kPi * 1.5f, max_r * 0.12f, max_r * 0.17f, 0);
                    }
                    ImGui::SetCursorScreenPos(p1);
                    ImGui::EndChild();
                    ImGui::TextDisabled("Hover labels are currently anchored on segment centroids; use Treemap for denser inspection.");
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Hierarchy")) {
                    ImGui::TextDisabled("Shape: jurisdiction -> section/branch -> office or position. Search keeps matching branches visible.");
                    ImGui::BeginChild("policy_hierarchy_tree", ImVec2(0, 420), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
                    auto section_has_match = [&](const std::string& section, const json& rows, const std::string& jurisdiction) {
                        if (policy_query.empty()) return true;
                        if (item_matches(section, jurisdiction)) return true;
                        if (!rows.is_array()) return false;
                        for (const auto& row : rows) {
                            if (item_matches(row.value("name", ""), row.value("url", ""), section, jurisdiction)) return true;
                        }
                        return false;
                    };
                    auto draw_jurisdiction_tree = [&](const char* key, const char* label) {
                        if ((policy_hierarchy_scope == 1 && std::string(key) != "maryland") ||
                            (policy_hierarchy_scope == 2 && std::string(key) != "baltimore") ||
                            policy_hierarchy_scope == 3) return;
                        const json& node = policy_hierarchy[key];
                        if (!node.contains("sections") || !node["sections"].is_object()) return;
                        size_t visible_sections = 0;
                        for (auto sec = node["sections"].begin(); sec != node["sections"].end(); ++sec) {
                            if (section_has_match(sec.key(), sec.value(), label)) visible_sections++;
                        }
                        if (visible_sections == 0 && !policy_query.empty()) return;
                        if (!policy_query.empty()) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                        if (ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen)) {
                            ImGui::TextDisabled("Source:");
                            ImGui::SameLine();
                            draw_source_chip(node.value("source", ""));
                            for (auto sec = node["sections"].begin(); sec != node["sections"].end(); ++sec) {
                                if (!sec.value().is_array()) continue;
                                if (!section_has_match(sec.key(), sec.value(), label)) continue;
                                std::string section_label = sec.key() + " (" + std::to_string(sec.value().size()) + ")";
                                if (!policy_query.empty()) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                                if (ImGui::TreeNode(section_label.c_str())) {
                                    for (const auto& row : sec.value()) {
                                        const std::string name = row.value("name", "");
                                        const std::string url = row.value("url", "");
                                        if (!item_matches(sec.key(), name, label, url)) continue;
                                        ImGui::BulletText("%s", name.c_str());
                                        if (!url.empty()) {
                                            ImGui::SameLine();
                                            draw_source_chip(url);
                                        }
                                    }
                                    ImGui::TreePop();
                                }
                            }
                            ImGui::TreePop();
                        }
                    };
                    auto draw_federal_tree = [&]() {
                        if (policy_hierarchy_scope == 1 || policy_hierarchy_scope == 2) return;
                        const json rows = policy_hierarchy["federal"].value("salient_positions", json::array());
                        std::unordered_map<std::string, std::vector<json>> by_branch;
                        for (const auto& row : rows) {
                            const std::string pos = row.value("position", "");
                            const std::string branch = row.value("branch", "Federal");
                            const std::string schedule = row.value("pay_schedule", "");
                            const std::string ref = row.value("pay_reference", "");
                            if (!item_matches(pos, branch, schedule, ref)) continue;
                            by_branch[branch].push_back(row);
                        }
                        if (by_branch.empty() && !policy_query.empty()) return;
                        if (!policy_query.empty()) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                        if (ImGui::TreeNodeEx("Federal", ImGuiTreeNodeFlags_DefaultOpen)) {
                            for (auto& kv : by_branch) {
                                std::string branch_label = kv.first + " (" + std::to_string(kv.second.size()) + ")";
                                if (!policy_query.empty()) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                                if (ImGui::TreeNode(branch_label.c_str())) {
                                    for (const auto& row : kv.second) {
                                        ImGui::BulletText("%s", row.value("position", "").c_str());
                                        ImGui::Indent();
                                        ImGui::TextDisabled("Pay: %s", row.value("pay_schedule", "").c_str());
                                        ImGui::TextDisabled("Reference:");
                                        ImGui::SameLine();
                                        draw_source_chip(row.value("pay_reference", ""));
                                        ImGui::Unindent();
                                    }
                                    ImGui::TreePop();
                                }
                            }
                            ImGui::TreePop();
                        }
                    };
                    draw_jurisdiction_tree("maryland", "Maryland");
                    draw_jurisdiction_tree("baltimore", "Baltimore");
                    draw_federal_tree();
                    ImGui::EndChild();
                    ImGui::SeparatorText("Search Results Table");
                    auto draw_jurisdiction = [&](const char* key, const char* label) {
                        if ((policy_hierarchy_scope == 1 && std::string(key) != "maryland") ||
                            (policy_hierarchy_scope == 2 && std::string(key) != "baltimore") ||
                            policy_hierarchy_scope == 3) return;
                        const json& node = policy_hierarchy[key];
                        if (!node.contains("sections") || !node["sections"].is_object()) return;
                        if (!ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen)) return;
                        ImGui::TextDisabled("Source:");
                        ImGui::SameLine();
                        draw_source_chip(node.value("source", ""));
                        if (ImGui::BeginTable((std::string("policy_table_") + key).c_str(), 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0, 260))) {
                            ImGui::TableSetupColumn("Section");
                            ImGui::TableSetupColumn("Name");
                            ImGui::TableSetupColumn("Jurisdiction");
                            ImGui::TableSetupColumn("URL");
                            ImGui::TableHeadersRow();
                            for (auto sec = node["sections"].begin(); sec != node["sections"].end(); ++sec) {
                                if (!sec.value().is_array()) continue;
                                for (const auto& row : sec.value()) {
                                    const std::string name = row.value("name", "");
                                    const std::string url = row.value("url", "");
                                    if (!item_matches(sec.key(), name, label, url)) continue;
                                    ImGui::TableNextRow();
                                    ImGui::TableSetColumnIndex(0); draw_clipped_text(sec.key(), 48);
                                    ImGui::TableSetColumnIndex(1); draw_clipped_text(name, 72);
                                    ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(label);
                                    ImGui::TableSetColumnIndex(3); draw_clipped_text(url, 80);
                                }
                            }
                            ImGui::EndTable();
                        }
                    };
                    draw_jurisdiction("maryland", "Maryland");
                    draw_jurisdiction("baltimore", "Baltimore");
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Federal Positions")) {
                    if (policy_hierarchy_scope == 1 || policy_hierarchy_scope == 2) {
                        ImGui::TextDisabled("Federal rows hidden by current scope.");
                    } else if (ImGui::BeginTable("federal_positions_table", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0, 420))) {
                        ImGui::TableSetupColumn("Position");
                        ImGui::TableSetupColumn("Branch");
                        ImGui::TableSetupColumn("Pay Schedule");
                        ImGui::TableSetupColumn("Reference");
                        ImGui::TableHeadersRow();
                        const json rows = policy_hierarchy["federal"].value("salient_positions", json::array());
                        for (const auto& row : rows) {
                            const std::string pos = row.value("position", "");
                            const std::string branch = row.value("branch", "");
                            const std::string schedule = row.value("pay_schedule", "");
                            const std::string ref = row.value("pay_reference", "");
                            if (!item_matches(pos, branch, schedule, ref)) continue;
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0); draw_clipped_text(pos, 72);
                            ImGui::TableSetColumnIndex(1); draw_clipped_text(branch, 48);
                            ImGui::TableSetColumnIndex(2); draw_clipped_text(schedule, 48);
                            ImGui::TableSetColumnIndex(3); draw_clipped_text(ref, 80);
                        }
                        ImGui::EndTable();
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("People & Pay")) {
                    ImGui::TextWrapped("Fetched public-sector roster. Search is capped for UI responsiveness; empty search shows the first matching rows by scope.");
                    ImGui::Text("Loaded roster rows: %zu", public_servant_roster.size());
                    ImGui::TextDisabled("Normalized files: data/public_servants/normalized_public_servants.jsonl and .csv");
                    constexpr size_t kPeoplePayDisplayCap = 5000;
                    if (people_pay_cached_query != policy_query || people_pay_cached_scope != policy_hierarchy_scope) {
                        people_pay_cached_query = policy_query;
                        people_pay_cached_scope = policy_hierarchy_scope;
                        people_pay_cache_matched_count = 0;
                        people_pay_visible_rows.clear();
                        people_pay_visible_rows.reserve(std::min(public_servant_roster.size(), kPeoplePayDisplayCap));
                        for (size_t row_index = 0; row_index < public_servant_roster.size(); ++row_index) {
                            const auto& row = public_servant_roster[row_index];
                            if (policy_hierarchy_scope == 1 && row.jurisdiction != "Maryland") continue;
                            if (policy_hierarchy_scope == 2 && row.jurisdiction != "Baltimore City") continue;
                            if (policy_hierarchy_scope == 3 && row.jurisdiction != "Federal") continue;
                            if (!roster_row_matches_query(row)) continue;
                            people_pay_cache_matched_count++;
                            if (people_pay_visible_rows.size() < kPeoplePayDisplayCap) people_pay_visible_rows.push_back(row_index);
                        }
                        people_pay_cache_rebuilds++;
                    }
                    people_pay_rendered_rows_last = 0;
                    if (ImGui::BeginTable("people_pay_table", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0, 440))) {
                        ImGui::TableSetupColumn("Jurisdiction");
                        ImGui::TableSetupColumn("Person");
                        ImGui::TableSetupColumn("Employer");
                        ImGui::TableSetupColumn("Agency");
                        ImGui::TableSetupColumn("Role");
                        ImGui::TableSetupColumn("Pay Grade");
                        ImGui::TableSetupColumn("Annual / Gross");
                        ImGui::TableSetupColumn("FY / Source");
                        ImGui::TableHeadersRow();
                        ImGuiListClipper clipper;
                        clipper.Begin(static_cast<int>(people_pay_visible_rows.size()));
                        while (clipper.Step()) {
                            for (int display_index = clipper.DisplayStart; display_index < clipper.DisplayEnd; ++display_index) {
                                const auto& row = public_servant_roster[people_pay_visible_rows[static_cast<size_t>(display_index)]];
                                const std::string person = row.person_name.empty() ? "(not in source)" : row.person_name;
                                const std::string grade = row.pay_grade.empty() ? "-" : row.pay_grade;
                                const std::string salary = "Annual: " + (row.annual_salary.empty() ? std::string("-") : row.annual_salary) +
                                                           " | Gross: " + (row.gross_pay.empty() ? std::string("-") : row.gross_pay);
                                const std::string source = row.fiscal_year + " | " + row.source_id;
                                people_pay_rendered_rows_last++;
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0); draw_clipped_text(row.jurisdiction, 32);
                                ImGui::TableSetColumnIndex(1); draw_clipped_text(person, 48);
                                ImGui::TableSetColumnIndex(2); draw_clipped_text(row.employer, 56);
                                ImGui::TableSetColumnIndex(3); draw_clipped_text(row.agency, 56);
                                ImGui::TableSetColumnIndex(4); draw_clipped_text(row.role_title, 72);
                                ImGui::TableSetColumnIndex(5); draw_clipped_text(grade, 32);
                                ImGui::TableSetColumnIndex(6); draw_clipped_text(salary, 64);
                                ImGui::TableSetColumnIndex(7); draw_clipped_text(source, 72);
                            }
                        }
                        ImGui::EndTable();
                    }
                    ImGui::TextDisabled("Matched rows: %zu | Cached rows: %zu | Rendered this frame: %zu | Cap: %zu | Cache rebuilds: %zu",
                                        people_pay_cache_matched_count, people_pay_visible_rows.size(), people_pay_rendered_rows_last,
                                        kPeoplePayDisplayCap, people_pay_cache_rebuilds);
                    ImGui::TextDisabled("Official bulk named salary source currently fetched: Baltimore City. Maryland state/university official pages fetched are salary schedules/structures, not bulk named rosters.");
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Pay Schedule")) {
                    const json pay = policy_hierarchy["federal"].value("pay_schedules", json::object());
                    const json exec = pay.value("executive_schedule", json::object());
                    ImGui::Text("Executive Schedule Year: %d", exec.value("year", 0));
                    ImGui::TextWrapped("Note: %s", exec.value("note", "").c_str());
                    ImGui::TextWrapped("Source: %s", exec.value("source", "").c_str());
                    if (ImGui::BeginTable("executive_schedule_table", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable)) {
                        ImGui::TableSetupColumn("Level");
                        ImGui::TableSetupColumn("Annual Pay");
                        ImGui::TableHeadersRow();
                        const json levels = exec.value("levels", json::object());
                        for (auto it = levels.begin(); it != levels.end(); ++it) {
                            const std::string level = it.key();
                            const std::string amount = it.value().is_number() ? std::to_string(it.value().get<int>()) : it.value().dump();
                            if (!item_matches(level, amount)) continue;
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(level.c_str());
                            ImGui::TableSetColumnIndex(1); ImGui::Text("$%s", amount.c_str());
                        }
                        ImGui::EndTable();
                    }
                    const json gs = pay.value("general_schedule", json::object());
                    ImGui::SeparatorText("General Schedule");
                    ImGui::Text("Year: %d | XML table count: %zu", gs.value("year", 0), gs.value("xml_tables", json::array()).size());
                    ImGui::TextWrapped("Note: %s", gs.value("note", "").c_str());
                    ImGui::TextWrapped("Source: %s", gs.value("source", "").c_str());
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Sources")) {
                    ImGui::TextWrapped("Maryland: %s", policy_hierarchy["maryland"].value("source", "").c_str());
                    ImGui::TextWrapped("Baltimore: %s", policy_hierarchy["baltimore"].value("source", "").c_str());
                    const json sources = policy_hierarchy["federal"].value("sources", json::array());
                    for (const auto& s : sources) {
                        if (s.is_string()) ImGui::TextWrapped("Federal: %s", s.get<std::string>().c_str());
                        else ImGui::TextWrapped("Federal: %s", s.dump().c_str());
                    }
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        ImGui::EndTabItem();
    }
}
