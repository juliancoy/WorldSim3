#include "gear_panel.h"

#include "imgui.h"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

void drawGearPanel(bool* show_sources_panel, const std::filesystem::path& root, BootstrapProgress& bootstrap) {
    if (!show_sources_panel || !*show_sources_panel) return;

    ImGui::SetNextWindowSize(ImVec2(540, 420), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Gear Panel", show_sources_panel, ImGuiWindowFlags_NoCollapse)) {
        if (ImGui::BeginTabBar("gear_tabs")) {
            if (ImGui::BeginTabItem("Sources")) {
                std::vector<std::string> past_work;
                std::vector<std::string> future_work;
                std::vector<std::string> skipped_layer_files;
                std::string todo_text = readTextFile(root / "TODO.md");
                collectTodoWork(todo_text, past_work, future_work);
                {
                    std::lock_guard<std::mutex> lk(bootstrap.msg_mutex);
                    skipped_layer_files = bootstrap.skipped_layer_files;
                }
                size_t skipped_layers = bootstrap.skipped_layers.load(std::memory_order_relaxed);
                size_t skipped_tiles = bootstrap.skipped_tiles.load(std::memory_order_relaxed);
                ImGui::Text("Past: %zu", past_work.size());
                ImGui::SameLine();
                ImGui::Text("Future: %zu", future_work.size());
                ImGui::SameLine();
                ImGui::Text("Skipped: %zuL/%zuT", skipped_layers, skipped_tiles);
                ImGui::Separator();
                if (ImGui::BeginTabBar("work_tabs")) {
                    if (ImGui::BeginTabItem("Past Work")) {
                        ImGui::BeginChild("past_work_scroll", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
                        if (past_work.empty()) ImGui::TextDisabled("No completed checklist items found in TODO.md");
                        else for (const auto& item : past_work) ImGui::BulletText("%s", item.c_str());
                        ImGui::EndChild();
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Future Work")) {
                        ImGui::BeginChild("future_work_scroll", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
                        if (future_work.empty()) ImGui::TextDisabled("No pending items found in TODO.md");
                        else for (const auto& item : future_work) ImGui::BulletText("%s", item.c_str());
                        ImGui::EndChild();
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Skipped This Run")) {
                        ImGui::BeginChild("skipped_work_scroll", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
                        ImGui::Text("Layers already present (not downloaded this run): %zu", skipped_layers);
                        ImGui::Text("Tiles already present (not downloaded this run): %zu", skipped_tiles);
                        ImGui::Separator();
                        if (skipped_layer_files.empty()) ImGui::TextDisabled("No skipped layers recorded for this run.");
                        else {
                            ImGui::TextUnformatted("Skipped layer files:");
                            for (const auto& f : skipped_layer_files) ImGui::BulletText("%s", f.c_str());
                        }
                        ImGui::EndChild();
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Heatmap")) {
                ImGui::TextDisabled("Heatmap controls are per-layer only.");
                ImGui::TextWrapped("Open a layer's settings (gear) to edit aggregate method, bandwidth, blur, clip, adaptive bandwidth, and multi-res settings.");
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}
