#include "gear_panel.h"

#include "env_config.h"
#include "imgui.h"
#include "ui_theme.h"

#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

void drawGearPanel(
    bool* show_sources_panel,
    const std::filesystem::path& root,
    AppSettings* app_settings,
    ImGuiContext* main_imgui_context,
    ImGuiContext* queue_imgui_context,
    BootstrapProgress& bootstrap,
    const std::function<void()>& rescan_local_data) {
    if (!show_sources_panel || !*show_sources_panel) return;

    ImGui::SetNextWindowSize(ImVec2(540, 420), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Gear Panel", show_sources_panel, ImGuiWindowFlags_NoCollapse)) {
        if (ImGui::BeginTabBar("gear_tabs")) {
            if (ImGui::BeginTabItem("Interface")) {
                if (app_settings) {
                    bool dark_mode = app_settings->dark_mode;
                    if (ImGui::Checkbox("Dark mode", &dark_mode)) {
                        app_settings->dark_mode = dark_mode;
                        ImGuiContext* previous_context = ImGui::GetCurrentContext();
                        if (main_imgui_context) {
                            ImGui::SetCurrentContext(main_imgui_context);
                            applyWorldsimUiTheme(app_settings->dark_mode);
                        }
                        if (queue_imgui_context) {
                            ImGui::SetCurrentContext(queue_imgui_context);
                            applyWorldsimUiTheme(app_settings->dark_mode);
                        }
                        ImGui::SetCurrentContext(previous_context);
                        saveAppSettings(root, *app_settings);
                    }
                    if (ImGui::SliderFloat("Map polygon fill opacity", &app_settings->map_polygon_fill_opacity, 0.0f, 1.0f, "%.2f")) {
                        saveAppSettings(root, *app_settings);
                    }
                    if (ImGui::SliderFloat("Map polygon outline thickness", &app_settings->map_polygon_outline_thickness, 1.0f, 6.0f, "%.1f px")) {
                        saveAppSettings(root, *app_settings);
                    }
                    ImGui::TextDisabled("Applies immediately and persists in data/app_settings.json.");
                } else {
                    ImGui::TextDisabled("App settings unavailable.");
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Credentials")) {
                if (app_settings) {
                    static std::array<char, 256> census_key_buffer{};
                    static std::string loaded_key_value;
                    static bool show_census_key = false;
                    const std::string env_key_value = loadDotEnvValue(root, "CENSUS_API_KEY");
                    if (loaded_key_value != env_key_value) {
                        loaded_key_value = env_key_value;
                        std::fill(census_key_buffer.begin(), census_key_buffer.end(), '\0');
                        const size_t copy_len = std::min(loaded_key_value.size(), census_key_buffer.size() - 1);
                        if (copy_len > 0) std::memcpy(census_key_buffer.data(), loaded_key_value.data(), copy_len);
                    }
                    ImGui::TextUnformatted("Census API Key");
                    ImGui::TextDisabled("Stored in the repo .env file and used when shell environment variables are not set.");
                    ImGuiInputTextFlags input_flags = show_census_key ? ImGuiInputTextFlags_None : ImGuiInputTextFlags_Password;
                    ImGui::SetNextItemWidth(-1.0f);
                    ImGui::InputText("##census_api_key", census_key_buffer.data(), census_key_buffer.size(), input_flags);
                    ImGui::Checkbox("Show key", &show_census_key);
                    if (ImGui::Button("Save Census Key")) {
                        std::string error;
                        if (setDotEnvValue(root, "CENSUS_API_KEY", census_key_buffer.data(), &error)) {
                            loaded_key_value = census_key_buffer.data();
                            applyDotEnvEnvironment(root);
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Clear Census Key")) {
                        std::string error;
                        removeDotEnvValue(root, "CENSUS_API_KEY", &error);
                        loaded_key_value.clear();
                        std::fill(census_key_buffer.begin(), census_key_buffer.end(), '\0');
                    }
                    ImGui::TextDisabled("Persists in .env as the single source of truth.");
                } else {
                    ImGui::TextDisabled("App settings unavailable.");
                }
                ImGui::EndTabItem();
            }
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
                if (rescan_local_data) {
                    if (ImGui::Button("Rescan Local Data")) {
                        rescan_local_data();
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("Refreshes detected local layer files.");
                }
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
