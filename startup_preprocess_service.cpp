#include "startup_preprocess_service.h"

#include "worldsim_app_internal.h"

#include "app_utils.h"
#include "cache_io.h"
#include "dataset_library.h"
#include "duckdb_analytics.h"
#include "layer_import.h"
#include "layer_registry.h"
#include "layer_runtime.h"
#include "layer_state_io.h"
#include "render_routing.h"
#include "ui_fonts.h"
#include "ui_theme.h"

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_vulkan.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
struct StartupPreprocessRunResult {
    int exit_code = 1;
    fs::path log_path;
};

struct StartupPreprocessSequenceResult {
    int exit_code = 1;
    fs::path log_path;
};

GeometryArtifactClass startupGeometryClassForLayer(const LayerDef& layer, bool is_parcel_layer) {
    if (is_parcel_layer) return GeometryArtifactClass::Polygon;
    if (layerUsesPointGeometry(layer)) return GeometryArtifactClass::Point;
    if (layerUsesPolylineGeometry(layer)) return GeometryArtifactClass::Polyline;
    return GeometryArtifactClass::Polygon;
}

std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (char ch : s) {
        if (ch == '\'') out += "'\\''";
        else out.push_back(ch);
    }
    out += "'";
    return out;
}

std::string currentExecutablePath(const char* argv0) {
#if defined(__linux__)
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    if (!ec && !p.empty()) return p.string();
#endif
    return argv0 && *argv0 ? std::string(argv0) : std::string("./worldsim3");
}

fs::path startupPreprocessLogPath(const fs::path& root) {
    return root / "data" / "logs" / "startup_preprocess_latest.log";
}

StartupPreprocessRunResult runStartupPreprocessSubprocess(
    const fs::path& root,
    const std::string& command,
    bool append_log = false,
    std::vector<std::string>* ui_log_lines = nullptr,
    std::mutex* ui_log_mutex = nullptr,
    std::atomic<bool>* done = nullptr,
    std::atomic<int>* exit_code = nullptr) {
    StartupPreprocessRunResult result;
    result.log_path = startupPreprocessLogPath(root);
    std::error_code ec;
    fs::create_directories(result.log_path.parent_path(), ec);
    const auto log_mode = append_log ? (std::ios::out | std::ios::app) : (std::ios::out | std::ios::trunc);
    std::ofstream log(result.log_path, log_mode);

    auto emit = [&](const std::string& line) {
        std::cerr << "[worldsim3-preprocess] " << line << "\n";
        if (log) {
            log << line << "\n";
            log.flush();
        }
        if (ui_log_lines && ui_log_mutex) {
            std::lock_guard<std::mutex> lk(*ui_log_mutex);
            ui_log_lines->push_back(line);
            if (ui_log_lines->size() > 400) ui_log_lines->erase(ui_log_lines->begin(), ui_log_lines->begin() + 80);
        }
    };

    emit("log=" + result.log_path.string());
    emit("$ " + command);
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        emit("failed to start preprocess subprocess");
        result.exit_code = 127;
        if (exit_code) exit_code->store(result.exit_code, std::memory_order_relaxed);
        if (done) done->store(true, std::memory_order_relaxed);
        return result;
    }

    char buf[512];
    while (fgets(buf, sizeof(buf), pipe)) {
        std::string line(buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        emit(line);
    }
    const int rc = pclose(pipe);
    result.exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : 1;
    emit("exit_code=" + std::to_string(result.exit_code));
    if (exit_code) exit_code->store(result.exit_code, std::memory_order_relaxed);
    if (done) done->store(true, std::memory_order_relaxed);
    return result;
}

std::vector<StartupPreprocessCommandStep> buildStartupPreprocessCommands(
    const fs::path& root,
    const StartupPreprocessPlan& plan,
    int reserve_cores,
    const char* argv0,
    bool rebuild_all_artifacts_from_scratch) {
    std::vector<StartupPreprocessCommandStep> steps;
    std::vector<LayerDef> layers = loadManifest(root);
    const std::string exe = currentExecutablePath(argv0);

    if (rebuild_all_artifacts_from_scratch) {
        std::string command = shellQuote(exe) + " --build-geometry-duckdb-artifacts --from-scratch";
        if (reserve_cores > 0) command += " --reserve-cores " + std::to_string(reserve_cores);
        command += " 2>&1";
        steps.push_back(StartupPreprocessCommandStep{
            "rebuild",
            "all",
            command
        });
        return steps;
    }

    auto geometry_command_for_layer = [&](const std::string& layer_file) -> std::string {
        const LayerDef* layer = nullptr;
        for (const auto& candidate : layers) {
            if (candidate.file == layer_file) {
                layer = &candidate;
                break;
            }
        }
        if (!layer) return {};
        std::string command = shellQuote(exe) + " ";
        if (layerUsesPointGeometry(*layer)) {
            command += "--compile-point-geometry " + shellQuote(layer_file);
        } else if (layerUsesPolylineGeometry(*layer)) {
            command += "--compile-polyline-geometry " + shellQuote(layer_file);
        } else {
            command += "--compile-polygon-geometry " + shellQuote(layer_file);
        }
        command += " 2>&1";
        return command;
    };

    for (const auto& issue : plan.issues) {
        if (issue.kind != "geometry") continue;
        const std::string command = geometry_command_for_layer(issue.layer_file);
        if (command.empty()) continue;
        steps.push_back(StartupPreprocessCommandStep{
            issue.kind,
            issue.layer_file,
            command
        });
    }

    if (plan.duckdb_required) {
        std::string command = shellQuote(exe) + " --rebuild-duckdb-analytics";
        if (reserve_cores > 0) command += " --reserve-cores " + std::to_string(reserve_cores);
        command += " 2>&1";
        steps.push_back(StartupPreprocessCommandStep{
            "duckdb",
            "data/worldsim.duckdb",
            command
        });
    }

    if (steps.empty() && plan.required) {
        std::string command = shellQuote(exe) + " --build-geometry-duckdb-artifacts";
        if (reserve_cores > 0) command += " --reserve-cores " + std::to_string(reserve_cores);
        command += " 2>&1";
        steps.push_back(StartupPreprocessCommandStep{
            "fallback",
            "all",
            command
        });
    }

    return steps;
}

StartupPreprocessSequenceResult runStartupPreprocessCommandSequence(
    const fs::path& root,
    const std::vector<StartupPreprocessCommandStep>& steps,
    std::vector<std::string>* ui_log_lines = nullptr,
    std::mutex* ui_log_mutex = nullptr,
    std::atomic<bool>* done = nullptr,
    std::atomic<int>* exit_code = nullptr) {
    StartupPreprocessSequenceResult result;
    result.exit_code = 0;
    for (size_t i = 0; i < steps.size(); ++i) {
        if (ui_log_lines && ui_log_mutex) {
            std::lock_guard<std::mutex> lk(*ui_log_mutex);
            ui_log_lines->push_back(
                "[step " + std::to_string(i + 1) + "/" + std::to_string(steps.size()) + "] " +
                steps[i].kind + " " + steps[i].layer_file);
        }
        StartupPreprocessRunResult one = runStartupPreprocessSubprocess(
            root,
            steps[i].command,
            i > 0,
            ui_log_lines,
            ui_log_mutex,
            nullptr,
            nullptr);
        result.log_path = one.log_path;
        result.exit_code = one.exit_code;
        if (one.exit_code != 0) break;
    }
    if (exit_code) exit_code->store(result.exit_code, std::memory_order_relaxed);
    if (done) done->store(true, std::memory_order_relaxed);
    return result;
}

void applyPersistedLayerEnabledStateForPreflight(const fs::path& root, std::vector<LayerDef>& layers) {
    std::ifstream in(root / "data" / "layer_ui_state.json");
    if (!in) return;
    json j;
    try {
        in >> j;
    } catch (...) {
        return;
    }
    if (!j.contains("layers") || !j["layers"].is_object()) return;
    const auto& obj = j["layers"];
    for (auto& layer : layers) {
        auto it = obj.find(layer.file);
        if (it != obj.end() && it->is_boolean()) layer.enabled = it->get<bool>();
    }
}

bool persistedGeometryArtifactReady(
    const fs::path& root,
    const LayerDef& layer,
    size_t layer_idx,
    int primary_parcel_idx,
    const std::string& sig,
    fs::path& out_path) {
    const bool is_primary_parcel_layer =
        primary_parcel_idx >= 0 && static_cast<int>(layer_idx) == primary_parcel_idx;
    const GeometryArtifactClass cls = startupGeometryClassForLayer(layer, is_primary_parcel_layer);
    const LayerRenderRoute render_route = classifyLayerRenderRoute(layer_idx, layer, primary_parcel_idx);
    out_path = geometryArtifactCachePathForLayerFile(
        root,
        layer.file,
        cls,
        layerRenderRouteArtifactName(render_route));
    if (cls == GeometryArtifactClass::Point) {
        return validateBinaryPointGeometryArtifactHeader(out_path, sig);
    }
    if (cls == GeometryArtifactClass::Polyline) {
        return validateBinaryPolylineGeometryArtifactHeader(out_path, sig);
    }
    if (cls == GeometryArtifactClass::Polygon) {
        return validateBinaryPolygonGeometryArtifactHeader(out_path, sig);
    }
    return false;
}

bool startupSourceMissingIsNonBlocking(const fs::path& root, const LayerDef& layer) {
    if (layerRuntimeSourceMaterialized(root, layer)) return false;
    if (!layerHasImportSource(layer) && layer.source_url.empty() && layer.source_urls.empty()) return false;
    return !layerHasLocalSsotSource(root, layer);
}

void uploadCurrentImGuiFonts(ImGui_ImplVulkanH_Window& wd) {
    VkCommandPool command_pool = wd.Frames[wd.FrameIndex].CommandPool;
    VkCommandBuffer command_buffer = wd.Frames[wd.FrameIndex].CommandBuffer;
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
}  // namespace

void printStartupPreprocessPlan(const StartupPreprocessPlan& plan, std::ostream& out) {
    out << "[worldsim3] startup preprocess required=" << (plan.required ? "true" : "false")
        << " duckdb_required=" << (plan.duckdb_required ? "true" : "false")
        << " issues=" << plan.issues.size() << "\n";
    for (const auto& issue : plan.issues) {
        out << "[worldsim3] preprocess issue kind=" << issue.kind
            << " layer=" << issue.layer_file
            << " message=\"" << issue.message << "\"";
        if (!issue.artifact_path.empty()) out << " artifact=" << issue.artifact_path;
        out << "\n";
    }
}

StartupPreprocessPlan inspectStartupPreprocessPlan(const fs::path& root) {
    StartupPreprocessPlan plan;
    std::vector<LayerDef> layers = loadManifest(root);
    applyPersistedLayerEnabledStateForPreflight(root, layers);
    LayerRegistry registry;
    registry.refresh(root, layers);
    const int primary_parcel_idx = registry.indices().parcel_layer_idx;

    for (size_t i = 0; i < layers.size(); ++i) {
        const LayerDef& layer = layers[i];
        if (!layer.enabled) continue;
        const fs::path layer_path = resolveStoredLayerPath(root, layer);
        std::string sig;
        if (!resolveLayerSourceSignature(layer_path, sig, nullptr)) {
            if (startupSourceMissingIsNonBlocking(root, layer)) continue;
            plan.required = true;
            plan.issues.push_back(StartupPreprocessIssue{
                "source",
                layer.file,
                "source signature unavailable; preprocessing cannot build this layer until the canonical/input artifact exists",
                canonicalLayerPathForFile(root, layer.file).string()
            });
            continue;
        }
        fs::path artifact_path;
        if (!persistedGeometryArtifactReady(root, layer, i, primary_parcel_idx, sig, artifact_path)) {
            plan.required = true;
            plan.issues.push_back(StartupPreprocessIssue{
                "geometry",
                layer.file,
                "compiled geometry artifact missing or stale",
                artifact_path.string()
            });
        }
    }

    DuckDbAnalytics analytics(root);
    const bool duckdb_stale = analytics.needsRebuild(layers);
    const bool duckdb_valid = !duckdb_stale && analytics.validateExistingCache();
    if (!duckdb_valid) {
        plan.required = true;
        plan.duckdb_required = true;
        plan.issues.push_back(StartupPreprocessIssue{
            "duckdb",
            "data/worldsim.duckdb",
            duckdb_stale ? "DuckDB semantic artifact missing or stale" : analytics.status().message,
            analytics.status().db_path
        });
    }

    return plan;
}

int runStartupPreprocessWindow(
    const fs::path& root,
    const AppSettings& app_settings,
    const StartupPreprocessPlan& initial_plan,
    const WorldsimCliOptions& cli_options,
    const char* argv0) {
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(880, 520, "WorldSim3 Preprocess Required", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }

    uint32_t extensions_count = 0;
    const char** extensions = glfwGetRequiredInstanceExtensions(&extensions_count);
    SetupVulkan(extensions, extensions_count);

    VkSurfaceKHR surface;
    VkResult err = glfwCreateWindowSurface(g_Instance, window, g_Allocator, &surface);
    if (err != VK_SUCCESS) return 1;
    ImGui_ImplVulkanH_Window pre_wd{};
    int w = 0;
    int h = 0;
    glfwGetFramebufferSize(window, &w, &h);
    SetupVulkanWindow(&pre_wd, surface, w, h);

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
    init_info.RenderPass = pre_wd.RenderPass;
    init_info.MinImageCount = g_MinImageCount;
    init_info.ImageCount = pre_wd.ImageCount;
    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.CheckVkResultFn = check_vk_result;
    ImGui_ImplVulkan_Init(&init_info);
    uploadCurrentImGuiFonts(pre_wd);

    std::mutex log_mutex;
    std::vector<std::string> log_lines;
    std::atomic<bool> worker_done{false};
    std::atomic<int> worker_exit{-1};
    fs::path preprocess_log_path = startupPreprocessLogPath(root);
    const int reserve_cores = cli_options.reserve_cores_set ? cli_options.reserve_cores : app_settings.reserve_cpu_cores;
    const std::vector<StartupPreprocessCommandStep> steps =
        buildStartupPreprocessCommands(
            root,
            initial_plan,
            reserve_cores,
            argv0,
            cli_options.rebuild_all_artifacts_from_scratch);

    std::thread worker([&] {
        StartupPreprocessSequenceResult result = runStartupPreprocessCommandSequence(
            root,
            steps,
            &log_lines,
            &log_mutex,
            &worker_done,
            &worker_exit);
        preprocess_log_path = result.log_path;
    });

    bool proceed = false;
    bool quit = false;
    bool swapchain_rebuild = false;
    while (!glfwWindowShouldClose(window) && !quit && !proceed) {
        glfwPollEvents();
        int fb_w = 0;
        int fb_h = 0;
        glfwGetFramebufferSize(window, &fb_w, &fb_h);
        if (fb_w > 0 && fb_h > 0 && (fb_w != pre_wd.Width || fb_h != pre_wd.Height)) {
            check_vk_result(vkDeviceWaitIdle(g_Device));
            ImGui_ImplVulkan_SetMinImageCount(g_MinImageCount);
            ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, &pre_wd, g_QueueFamily, g_Allocator, fb_w, fb_h, g_MinImageCount);
            pre_wd.FrameIndex = 0;
        }

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize, ImGuiCond_Always);
        ImGui::Begin("Preprocess", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
        ImGui::TextUnformatted("WorldSim3 preprocess is running before the main UI starts.");
        ImGui::TextWrapped("Runtime rendering no longer builds missing geometry, DuckDB, or property artifacts. The main UI will start only after this separate preprocessing step completes.");
        ImGui::Separator();
        ImGui::Text("Initial work items: %zu", initial_plan.issues.size());
        ImGui::SameLine();
        ImGui::Text("DuckDB: %s", initial_plan.duckdb_required ? "required" : "current");
        ImGui::Text("Planned startup commands: %zu", steps.size());
        ImGui::TextDisabled("Log: %s", preprocess_log_path.string().c_str());
        if (ImGui::BeginChild("preprocess_issues", ImVec2(0, 120), true)) {
            for (const auto& issue : initial_plan.issues) {
                ImGui::BulletText("[%s] %s", issue.kind.c_str(), issue.layer_file.c_str());
                if (!issue.message.empty()) ImGui::TextWrapped("  %s", issue.message.c_str());
                if (!issue.artifact_path.empty()) ImGui::TextDisabled("  %s", issue.artifact_path.c_str());
            }
        }
        ImGui::EndChild();
        ImGui::SeparatorText("Preprocess Log");
        if (!worker_done.load(std::memory_order_relaxed)) {
            ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(-1, 0), "running");
        } else {
            const int rc = worker_exit.load(std::memory_order_relaxed);
            ImGui::Text("preprocess exit code: %d", rc);
            if (rc == 0) {
                ImGui::TextColored(ImVec4(0.25f, 0.75f, 0.35f, 1.0f), "Artifacts prepared. Launching main UI.");
                proceed = true;
            } else {
                ImGui::TextColored(ImVec4(0.90f, 0.25f, 0.18f, 1.0f), "Preprocess failed. Main UI will not start with stale artifacts.");
                ImGui::TextWrapped("See terminal output or %s", preprocess_log_path.string().c_str());
                if (ImGui::Button("Quit")) quit = true;
            }
        }
        if (ImGui::BeginChild("preprocess_log", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
            std::lock_guard<std::mutex> lk(log_mutex);
            const size_t start = log_lines.size() > 120 ? log_lines.size() - 120 : 0;
            for (size_t i = start; i < log_lines.size(); ++i) ImGui::TextUnformatted(log_lines[i].c_str());
            if (!worker_done.load(std::memory_order_relaxed)) ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
        ImGui::End();
        ImGui::Render();
        FrameRenderSecondary(&pre_wd, ImGui::GetDrawData(), swapchain_rebuild);
        FramePresentSecondary(&pre_wd, swapchain_rebuild);
        if (swapchain_rebuild) swapchain_rebuild = false;
        if (proceed) std::this_thread::sleep_for(std::chrono::milliseconds(250));
        else std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    if (worker.joinable()) worker.join();
    const int rc = worker_exit.load(std::memory_order_relaxed);
    check_vk_result(vkDeviceWaitIdle(g_Device));
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    ImGui_ImplVulkanH_DestroyWindow(g_Instance, g_Device, &pre_wd, g_Allocator);
    CleanupVulkan();
    glfwDestroyWindow(window);
    glfwTerminate();
    return (proceed && rc == 0) ? 0 : 1;
}

int runStartupPreprocessCli(
    const fs::path& root,
    const AppSettings& app_settings,
    const StartupPreprocessPlan& plan,
    const WorldsimCliOptions& cli_options,
    const char* argv0) {
    printStartupPreprocessPlan(plan, std::cerr);
    if (!plan.required && !cli_options.rebuild_all_artifacts_from_scratch) {
        std::cout << json{
            {"mode", "startup-preprocess"},
            {"ok", true},
            {"required", false},
            {"message", "startup artifacts are current"}
        }.dump(2) << '\n';
        return 0;
    }

    const int reserve_cores = cli_options.reserve_cores_set ? cli_options.reserve_cores : app_settings.reserve_cpu_cores;
    const std::vector<StartupPreprocessCommandStep> steps =
        buildStartupPreprocessCommands(
            root,
            plan,
            reserve_cores,
            argv0,
            cli_options.rebuild_all_artifacts_from_scratch);
    StartupPreprocessSequenceResult result = runStartupPreprocessCommandSequence(root, steps);
    std::cout << json{
        {"mode", "startup-preprocess"},
        {"ok", result.exit_code == 0},
        {"required", plan.required || cli_options.rebuild_all_artifacts_from_scratch},
        {"forced_rebuild_from_scratch", cli_options.rebuild_all_artifacts_from_scratch},
        {"exit_code", result.exit_code},
        {"log_path", result.log_path.string()},
        {"step_count", steps.size()}
    }.dump(2) << '\n';
    return result.exit_code == 0 ? 0 : result.exit_code;
}
