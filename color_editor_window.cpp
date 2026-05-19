#include "color_editor_window.h"

#include "app_settings.h"
#include "color_editor_ipc.h"
#include "map_render_utils.h"
#include "ui_fonts.h"
#include "ui_theme.h"
#include "worldsim_app_internal.h"

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_vulkan.h"

#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

namespace fs = std::filesystem;

namespace {
std::string compactNumber(double value) {
    char buffer[64];
    if (std::abs(value) >= 1000000000.0) {
        std::snprintf(buffer, sizeof(buffer), "%.1fB", value / 1000000000.0);
    } else if (std::abs(value) >= 1000000.0) {
        std::snprintf(buffer, sizeof(buffer), "%.1fM", value / 1000000.0);
    } else if (std::abs(value) >= 1000.0) {
        std::snprintf(buffer, sizeof(buffer), "%.0fK", value / 1000.0);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.2f", value);
    }
    return buffer;
}

void drawHistogram(const ColorEditorOptionSnapshot& option) {
    if (!option.histogram.valid || option.histogram.plot_bins.empty() || option.histogram.sample_count == 0) {
        ImGui::TextDisabled("No numeric samples available for this source.");
        return;
    }
    ImGui::Text("Samples: %zu", option.histogram.sample_count);
    ImGui::Text("Range: %s to %s",
        compactNumber(option.histogram.min_value).c_str(),
        compactNumber(option.histogram.max_value).c_str());
    ImGui::Text("Median: %s", compactNumber(option.histogram.median_value).c_str());
    ImGui::TextDisabled("Clip ceiling: %s", compactNumber(option.histogram.clipped_max_value).c_str());
    ImGui::InvisibleButton("##histogram", ImVec2(-1.0f, 96.0f));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    draw->AddRectFilled(min, max, IM_COL32(18, 22, 28, 255), 4.0f);
    draw->AddRect(min, max, IM_COL32(70, 76, 84, 255), 4.0f);
    const size_t n = option.histogram.plot_bins.size();
    if (n == 0 || option.histogram.max_bin <= 0.0f) return;
    const float bin_w = (max.x - min.x) / (float)n;
    for (size_t i = 0; i < n; ++i) {
        const float h = std::clamp(option.histogram.plot_bins[i] / (option.histogram.max_bin * 1.05f), 0.0f, 1.0f);
        const float x0 = min.x + bin_w * (float)i;
        const float x1 = x0 + std::max(1.0f, bin_w - 1.0f);
        const float y1 = max.y - 1.0f;
        const float y0 = y1 - h * (max.y - min.y - 2.0f);
        const double value_center = option.histogram.min_value + ((double)i + 0.5) * option.histogram.bin_width;
        float t = 0.0f;
        if (option.normalize_mode == 0) {
            const double denom = std::max(1e-9, option.histogram.clipped_max_value - option.histogram.min_value);
            t = std::clamp((float)((value_center - option.histogram.min_value) / denom), 0.0f, 1.0f);
        } else if (option.normalize_mode == 3) {
            const double pos = option.histogram.bin_width > 0.0 ? (value_center - option.histogram.min_value) / option.histogram.bin_width : 0.0;
            const size_t bin_idx = (size_t)std::clamp((int)std::floor(pos), 0, (int)option.histogram.bins.size() - 1);
            const double frac = std::clamp(pos - (double)bin_idx, 0.0, 1.0);
            const double before = bin_idx == 0 ? 0.0 : (double)option.histogram.cumulative_bins[bin_idx - 1];
            const double within = frac * (double)option.histogram.bins[bin_idx];
            const double denom = (double)std::max<size_t>(1, option.histogram.sample_count);
            const float percentile = std::clamp((float)((before + within) / denom), 0.0f, 1.0f);
            constexpr int kEqualCountZones = 8;
            const int zone_idx = std::clamp((int)std::floor(percentile * (float)kEqualCountZones), 0, kEqualCountZones - 1);
            t = (float)zone_idx / (float)(kEqualCountZones - 1);
        } else {
            const double pos = option.histogram.bin_width > 0.0 ? (value_center - option.histogram.min_value) / option.histogram.bin_width : 0.0;
            const size_t bin_idx = (size_t)std::clamp((int)std::floor(pos), 0, (int)option.histogram.bins.size() - 1);
            const double frac = std::clamp(pos - (double)bin_idx, 0.0, 1.0);
            const double before = bin_idx == 0 ? 0.0 : (double)option.histogram.cumulative_bins[bin_idx - 1];
            const double within = frac * (double)option.histogram.bins[bin_idx];
            const double denom = (double)std::max<size_t>(1, option.histogram.sample_count);
            t = std::clamp((float)((before + within) / denom), 0.0f, 1.0f);
        }
        t = applyPowerGamma(t, option.gamma);
        draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), ImGui::ColorConvertFloat4ToU32(heatColor(t)));
    }
}

const ColorEditorOptionSnapshot* findSelectedOption(const ColorEditorSnapshot& snapshot) {
    for (const auto& option : snapshot.options) {
        if (option.id == snapshot.selected_option_id) return &option;
    }
    return snapshot.options.empty() ? nullptr : &snapshot.options.front();
}

struct DraftColorEditorState {
    bool initialized = false;
    uint64_t revision = 0;
    bool outline_target = false;
    ImVec4 fill_color = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    ImVec4 outline_color = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    std::string selected_option_id = "static";
    std::vector<ColorEditorOptionSnapshot> options;
};

bool colorDiffers(const ImVec4& a, const ImVec4& b) {
    return
        std::abs(a.x - b.x) > 0.0001f ||
        std::abs(a.y - b.y) > 0.0001f ||
        std::abs(a.z - b.z) > 0.0001f ||
        std::abs(a.w - b.w) > 0.0001f;
}

bool optionSettingsDiffer(const ColorEditorOptionSnapshot& a, const ColorEditorOptionSnapshot& b) {
    return
        a.id != b.id ||
        a.normalize_mode != b.normalize_mode ||
        std::abs(a.percentile_clip - b.percentile_clip) > 0.0001f ||
        std::abs(a.gamma - b.gamma) > 0.0001f;
}

void syncDraftFromSnapshot(const ColorEditorSnapshot& snapshot, DraftColorEditorState& draft) {
    draft.initialized = true;
    draft.revision = snapshot.revision;
    draft.outline_target = snapshot.outline_target;
    draft.fill_color = snapshot.fill_color;
    draft.outline_color = snapshot.outline_color;
    draft.selected_option_id = snapshot.selected_option_id;
    draft.options = snapshot.options;
}

bool emitCommand(
    const ColorEditorSnapshot& snapshot,
    uint64_t& next_seq,
    const std::string& type,
    int control_layer_idx,
    const std::string& option_id,
    const ImVec4& color,
    int normalize_mode,
    float percentile_clip,
    float gamma) {
    if (snapshot.command_path.empty()) return false;
    ColorEditorCommand command;
    command.seq = ++next_seq;
    command.type = type;
    command.layer_idx = snapshot.layer_idx;
    command.control_layer_idx = control_layer_idx;
    command.option_id = option_id;
    command.color = color;
    command.normalize_mode = normalize_mode;
    command.percentile_clip = percentile_clip;
    command.gamma = gamma;
    return saveColorEditorCommand(snapshot.command_path, command);
}

bool draftIsDirty(const ColorEditorSnapshot& snapshot, const DraftColorEditorState& draft) {
    if (!draft.initialized) return false;
    if (snapshot.outline_target != draft.outline_target) return true;
    if (draft.outline_target) return colorDiffers(snapshot.outline_color, draft.outline_color);
    if (colorDiffers(snapshot.fill_color, draft.fill_color)) return true;
    if (snapshot.selected_option_id != draft.selected_option_id) return true;
    if (snapshot.options.size() != draft.options.size()) return true;
    for (size_t i = 0; i < snapshot.options.size(); ++i) {
        if (optionSettingsDiffer(snapshot.options[i], draft.options[i])) return true;
    }
    return false;
}

void applyDraft(
    ColorEditorSnapshot& snapshot,
    DraftColorEditorState& draft,
    uint64_t& next_seq) {
    if (draft.outline_target) {
        emitCommand(snapshot, next_seq, "outline_color", -1, "", draft.outline_color, 0, 100.0f, 1.0f);
        snapshot.outline_color = draft.outline_color;
        snapshot.revision += 1;
        syncDraftFromSnapshot(snapshot, draft);
        return;
    }

    const ColorEditorOptionSnapshot* selected_option = nullptr;
    for (const auto& option : draft.options) {
        if (option.id == draft.selected_option_id) {
            selected_option = &option;
            break;
        }
    }

    if (draft.selected_option_id != snapshot.selected_option_id) {
        emitCommand(
            snapshot,
            next_seq,
            "select_option",
            selected_option ? selected_option->control_layer_idx : -1,
            draft.selected_option_id,
            draft.fill_color,
            selected_option ? selected_option->normalize_mode : 0,
            selected_option ? selected_option->percentile_clip : 100.0f,
            selected_option ? selected_option->gamma : 1.0f);
    }

    if (colorDiffers(snapshot.fill_color, draft.fill_color)) {
        emitCommand(snapshot, next_seq, "fill_color", -1, "", draft.fill_color, 0, 100.0f, 1.0f);
    }

    for (size_t i = 0; i < snapshot.options.size() && i < draft.options.size(); ++i) {
        const auto& live = snapshot.options[i];
        const auto& edited = draft.options[i];
        if (live.id == "static" || live.id != edited.id) continue;
        if (edited.normalize_mode != live.normalize_mode) {
            emitCommand(snapshot, next_seq, "normalize_mode", edited.control_layer_idx, edited.id, draft.fill_color, edited.normalize_mode, edited.percentile_clip, edited.gamma);
        }
        if (std::abs(edited.percentile_clip - live.percentile_clip) > 0.0001f) {
            emitCommand(snapshot, next_seq, "percentile_clip", edited.control_layer_idx, edited.id, draft.fill_color, edited.normalize_mode, edited.percentile_clip, edited.gamma);
        }
        if (std::abs(edited.gamma - live.gamma) > 0.0001f) {
            emitCommand(snapshot, next_seq, "gamma", edited.control_layer_idx, edited.id, draft.fill_color, edited.normalize_mode, edited.percentile_clip, edited.gamma);
        }
    }

    snapshot.fill_color = draft.fill_color;
    snapshot.selected_option_id = draft.selected_option_id;
    snapshot.options = draft.options;
    snapshot.revision += 1;
    syncDraftFromSnapshot(snapshot, draft);
}

void drawColorEditorWindow(ColorEditorSnapshot& snapshot, uint64_t& next_seq) {
    static DraftColorEditorState draft;
    if (!draft.initialized || draft.revision != snapshot.revision || draft.outline_target != snapshot.outline_target) {
        syncDraftFromSnapshot(snapshot, draft);
    }

    ImGui::SetNextWindowSize(ImVec2(520.0f, 720.0f), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoMove;
    if (!ImGui::Begin("Layer Color Editor", nullptr, flags)) {
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted(snapshot.layer_name.c_str());
    ImGui::TextDisabled("%s", snapshot.outline_target ? "Outline color" : "Fill color");
    if (!snapshot.layer_file.empty()) {
        ImGui::TextDisabled("%s", snapshot.layer_file.c_str());
    }
    ImGui::Separator();

    if (snapshot.outline_target) {
        float rgba[4] = {
            draft.outline_color.x,
            draft.outline_color.y,
            draft.outline_color.z,
            draft.outline_color.w
        };
        if (ImGui::ColorPicker4("Outline color", rgba, ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_NoSidePreview)) {
            draft.outline_color = ImVec4(rgba[0], rgba[1], rgba[2], rgba[3]);
        }
        const bool dirty = draftIsDirty(snapshot, draft);
        ImGui::Separator();
        ImGui::BeginDisabled(!dirty);
        if (ImGui::Button("Apply")) applyDraft(snapshot, draft, next_seq);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Reset")) syncDraftFromSnapshot(snapshot, draft);
        ImGui::End();
        return;
    }

    float rgba[4] = {
        draft.fill_color.x,
        draft.fill_color.y,
        draft.fill_color.z,
        draft.fill_color.w
    };
    if (ImGui::ColorPicker4("Static color", rgba, ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_NoSidePreview)) {
        draft.fill_color = ImVec4(rgba[0], rgba[1], rgba[2], rgba[3]);
    }

    if (snapshot.supports_continuous && !snapshot.options.empty()) {
        ImGui::SeparatorText("Color Mode");
        for (auto& option : draft.options) {
            const bool selected = draft.selected_option_id == option.id;
            if (ImGui::RadioButton(option.label.c_str(), selected)) {
                draft.selected_option_id = option.id;
            }
        }

        ColorEditorSnapshot draft_view = snapshot;
        draft_view.selected_option_id = draft.selected_option_id;
        draft_view.options = draft.options;
        const ColorEditorOptionSnapshot* selected_option = findSelectedOption(draft_view);
        if (selected_option && selected_option->id != "static") {
            ImGui::SeparatorText("Continuous Scale");
            static const std::array<const char*, 4> kNormalizeItems = {
                "Absolute clipped range",
                "Histogram percentile",
                "Group/Zoning percentile",
                "Equal-count color bands"
            };
            auto editable_option = std::find_if(draft.options.begin(), draft.options.end(), [&](const auto& option) {
                return option.id == selected_option->id;
            });
            if (editable_option != draft.options.end()) {
                int normalize_mode = std::clamp(editable_option->normalize_mode, 0, (int)kNormalizeItems.size() - 1);
                const int normalize_count = (int)kNormalizeItems.size();
                if (ImGui::Combo("Normalize", &normalize_mode, kNormalizeItems.data(), normalize_count)) {
                    editable_option->normalize_mode = normalize_mode;
                }
                float clip = editable_option->percentile_clip;
                if (ImGui::SliderFloat("Clip", &clip, 50.0f, 100.0f, "%.0f%%")) {
                    editable_option->percentile_clip = clip;
                }
                float gamma = editable_option->gamma;
                if (ImGui::SliderFloat("Gamma", &gamma, 0.10f, 5.0f, "%.2f")) {
                    editable_option->gamma = gamma;
                }
                drawHistogram(*editable_option);
            }
        }
    }

    const bool dirty = draftIsDirty(snapshot, draft);
    ImGui::Separator();
    ImGui::BeginDisabled(!dirty);
    if (ImGui::Button("Apply")) applyDraft(snapshot, draft, next_seq);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Reset")) syncDraftFromSnapshot(snapshot, draft);

    ImGui::End();
}
}

int runColorEditorWindow(const fs::path& root, const fs::path& snapshot_path) {
    AppSettings app_settings;
    app_settings = loadAppSettings(root, app_settings);

    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(520, 720, "Layer Color Editor", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }

    uint32_t extensions_count = 0;
    const char** extensions = glfwGetRequiredInstanceExtensions(&extensions_count);
    SetupVulkan(extensions, extensions_count);

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkResult err = glfwCreateWindowSurface(g_Instance, window, g_Allocator, &surface);
    if (err != VK_SUCCESS) {
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window, &width, &height);
    ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
    SetupVulkanWindow(wd, surface, width, height);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    configureWorldsimFonts();
    applyWorldsimUiTheme(app_settings.dark_mode);

    ImGui_ImplGlfw_InitForVulkan(window, true);
    ImGui_ImplVulkan_InitInfo init_info{};
    init_info.Instance = g_Instance;
    init_info.PhysicalDevice = g_PhysicalDevice;
    init_info.Device = g_Device;
    init_info.QueueFamily = g_QueueFamily;
    init_info.Queue = g_Queue;
    init_info.DescriptorPool = g_DescriptorPool;
    init_info.RenderPass = wd->RenderPass;
    init_info.MinImageCount = g_MinImageCount;
    init_info.ImageCount = wd->ImageCount;
    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.CheckVkResultFn = check_vk_result;
    ImGui_ImplVulkan_Init(&init_info);

    {
        VkCommandPool command_pool = wd->Frames[wd->FrameIndex].CommandPool;
        VkCommandBuffer command_buffer = wd->Frames[wd->FrameIndex].CommandBuffer;
        check_vk_result(vkResetCommandPool(g_Device, command_pool, 0));
        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk_result(vkBeginCommandBuffer(command_buffer, &begin_info));
        ImGui_ImplVulkan_CreateFontsTexture();
        check_vk_result(vkEndCommandBuffer(command_buffer));
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command_buffer;
        {
            std::lock_guard<std::mutex> qlk(g_QueueSubmitMutex);
            check_vk_result(vkQueueSubmit(g_Queue, 1, &submit, VK_NULL_HANDLE));
            check_vk_result(vkDeviceWaitIdle(g_Device));
        }
        ImGui_ImplVulkan_DestroyFontsTexture();
    }

    ColorEditorSnapshot snapshot;
    loadColorEditorSnapshot(snapshot_path, snapshot);
    uint64_t next_seq = 0;
    if (!snapshot.command_path.empty()) {
        ColorEditorCommand existing_command;
        if (loadColorEditorCommand(snapshot.command_path, existing_command)) next_seq = existing_command.seq;
    }
    fs::file_time_type last_snapshot_write = fs::file_time_type::min();
    std::chrono::steady_clock::time_point last_snapshot_poll{};
    std::chrono::steady_clock::time_point last_render_at{};
    constexpr auto kSnapshotPollInterval = std::chrono::milliseconds(400);
    constexpr auto kIdleFrameInterval = std::chrono::milliseconds(125);

    while (!glfwWindowShouldClose(window)) {
        glfwWaitEventsTimeout(0.10);

        int fb_w = 0;
        int fb_h = 0;
        glfwGetFramebufferSize(window, &fb_w, &fb_h);
        if (fb_w > 0 && fb_h > 0 && (fb_w != wd->Width || fb_h != wd->Height)) {
            width = fb_w;
            height = fb_h;
            g_SwapChainRebuild = true;
        }
        if (g_SwapChainRebuild) {
            glfwGetFramebufferSize(window, &width, &height);
            if (width > 0 && height > 0) {
                ImGui_ImplVulkan_SetMinImageCount(g_MinImageCount);
                ImGui_ImplVulkanH_CreateOrResizeWindow(
                    g_Instance,
                    g_PhysicalDevice,
                    g_Device,
                    wd,
                    g_QueueFamily,
                    g_Allocator,
                    width,
                    height,
                    g_MinImageCount);
                wd->FrameIndex = 0;
                g_SwapChainRebuild = false;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (last_snapshot_poll.time_since_epoch().count() == 0 ||
            now - last_snapshot_poll >= kSnapshotPollInterval) {
            last_snapshot_poll = now;
            std::error_code time_ec;
            const auto write_time = fs::last_write_time(snapshot_path, time_ec);
            if (!time_ec && write_time != last_snapshot_write) {
                ColorEditorSnapshot updated_snapshot;
                if (loadColorEditorSnapshot(snapshot_path, updated_snapshot)) {
                    snapshot = std::move(updated_snapshot);
                    last_snapshot_write = write_time;
                }
            }
        }
        if (last_render_at.time_since_epoch().count() != 0 &&
            now - last_render_at < kIdleFrameInterval &&
            !g_SwapChainRebuild) {
            continue;
        }
        last_render_at = now;

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        drawColorEditorWindow(snapshot, next_seq);

        ImGui::Render();
        FrameRender(wd, ImGui::GetDrawData());
        FramePresent(wd);
    }

    check_vk_result(vkDeviceWaitIdle(g_Device));
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    CleanupVulkanWindow();
    CleanupVulkan();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
