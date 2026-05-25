#include "app_window_service.h"

#include "ui_fonts.h"
#include "ui_theme.h"
#include "worldsim_app.h"

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_vulkan.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>

namespace {
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

bool initializeAppWindows(const AppSettings& app_settings, AppWindowServiceContext& out, std::string* error) {
    if (!glfwInit()) {
        if (error) *error = "glfwInit failed";
        return false;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    int initial_window_w = 1600;
    int initial_window_h = 1000;
    if (GLFWmonitor* monitor = glfwGetPrimaryMonitor()) {
        int work_x = 0;
        int work_y = 0;
        int work_w = 0;
        int work_h = 0;
        glfwGetMonitorWorkarea(monitor, &work_x, &work_y, &work_w, &work_h);
        if (work_w > 0 && work_h > 0) {
            initial_window_w = std::min(initial_window_w, std::max(640, (int)std::floor((float)work_w * 0.90f)));
            initial_window_h = std::min(initial_window_h, std::max(420, (int)std::floor((float)work_h * 0.85f)));
        }
    }

    out.initial_window_w = initial_window_w;
    out.initial_window_h = initial_window_h;
    out.main_window = glfwCreateWindow(initial_window_w, initial_window_h, "Baltimore Vulkan Map", nullptr, nullptr);
    if (!out.main_window) {
        if (error) *error = "glfwCreateWindow failed for main window";
        glfwTerminate();
        return false;
    }

    uint32_t extensions_count = 0;
    const char** extensions = glfwGetRequiredInstanceExtensions(&extensions_count);
    SetupVulkan(extensions, extensions_count);

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkResult err = glfwCreateWindowSurface(g_Instance, out.main_window, g_Allocator, &surface);
    if (err != VK_SUCCESS) {
        if (error) *error = "glfwCreateWindowSurface failed for main window";
        return false;
    }

    glfwGetFramebufferSize(out.main_window, &out.main_framebuffer_w, &out.main_framebuffer_h);
    out.main_window_data = &g_MainWindowData;
    SetupVulkanWindow(out.main_window_data, surface, out.main_framebuffer_w, out.main_framebuffer_h);

    {
        std::string parcel_upload_error;
        if (!startParcelGpuUploadWorker(&parcel_upload_error)) {
            std::fprintf(stderr, "[worldsim3] Failed to start parcel GPU upload worker: %s\n", parcel_upload_error.c_str());
        }
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    out.main_imgui_context = ImGui::GetCurrentContext();
    configureWorldsimFonts();
    applyWorldsimUiTheme(app_settings.dark_mode);

    ImGui_ImplGlfw_InitForVulkan(out.main_window, true);
    ImGui_ImplVulkan_InitInfo init_info{};
    init_info.Instance = g_Instance;
    init_info.PhysicalDevice = g_PhysicalDevice;
    init_info.Device = g_Device;
    init_info.QueueFamily = g_QueueFamily;
    init_info.Queue = g_Queue;
    init_info.DescriptorPool = g_DescriptorPool;
    init_info.RenderPass = out.main_window_data->RenderPass;
    init_info.MinImageCount = g_MinImageCount;
    init_info.ImageCount = out.main_window_data->ImageCount;
    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.CheckVkResultFn = check_vk_result;
    ImGui_ImplVulkan_Init(&init_info);
    uploadCurrentImGuiFonts(*out.main_window_data);

    out.download_queue_window = glfwCreateWindow(500, 320, "Download Queue", nullptr, nullptr);
    if (!out.download_queue_window) {
        if (error) *error = "glfwCreateWindow failed for download queue window";
        return false;
    }
    int main_x = 0;
    int main_y = 0;
    glfwGetWindowPos(out.main_window, &main_x, &main_y);
    glfwSetWindowPos(out.download_queue_window, main_x + 1620, main_y + 96);
    VkSurfaceKHR download_queue_surface = VK_NULL_HANDLE;
    err = glfwCreateWindowSurface(g_Instance, out.download_queue_window, g_Allocator, &download_queue_surface);
    if (err != VK_SUCCESS) {
        if (error) *error = "glfwCreateWindowSurface failed for download queue window";
        return false;
    }
    glfwGetFramebufferSize(
        out.download_queue_window,
        &out.download_queue_framebuffer_w,
        &out.download_queue_framebuffer_h);
    SetupVulkanWindow(
        &out.download_queue_window_data,
        download_queue_surface,
        out.download_queue_framebuffer_w,
        out.download_queue_framebuffer_h);

    out.download_queue_imgui_context = ImGui::CreateContext();
    ImGui::SetCurrentContext(out.download_queue_imgui_context);
    configureWorldsimFonts();
    applyWorldsimUiTheme(app_settings.dark_mode);
    ImGui_ImplGlfw_InitForVulkan(out.download_queue_window, false);
    glfwSetWindowUserPointer(out.download_queue_window, out.download_queue_imgui_context);
    glfwSetWindowFocusCallback(out.download_queue_window, [](GLFWwindow* cb_window, int focused) {
        ImGui::SetCurrentContext(static_cast<ImGuiContext*>(glfwGetWindowUserPointer(cb_window)));
        ImGui_ImplGlfw_WindowFocusCallback(cb_window, focused);
    });
    glfwSetCursorEnterCallback(out.download_queue_window, [](GLFWwindow* cb_window, int entered) {
        ImGui::SetCurrentContext(static_cast<ImGuiContext*>(glfwGetWindowUserPointer(cb_window)));
        ImGui_ImplGlfw_CursorEnterCallback(cb_window, entered);
    });
    glfwSetCursorPosCallback(out.download_queue_window, [](GLFWwindow* cb_window, double x, double y) {
        ImGui::SetCurrentContext(static_cast<ImGuiContext*>(glfwGetWindowUserPointer(cb_window)));
        ImGui_ImplGlfw_CursorPosCallback(cb_window, x, y);
    });
    glfwSetMouseButtonCallback(out.download_queue_window, [](GLFWwindow* cb_window, int button, int action, int mods) {
        ImGui::SetCurrentContext(static_cast<ImGuiContext*>(glfwGetWindowUserPointer(cb_window)));
        ImGui_ImplGlfw_MouseButtonCallback(cb_window, button, action, mods);
    });
    glfwSetScrollCallback(out.download_queue_window, [](GLFWwindow* cb_window, double xoffset, double yoffset) {
        ImGui::SetCurrentContext(static_cast<ImGuiContext*>(glfwGetWindowUserPointer(cb_window)));
        ImGui_ImplGlfw_ScrollCallback(cb_window, xoffset, yoffset);
    });
    glfwSetKeyCallback(out.download_queue_window, [](GLFWwindow* cb_window, int key, int scancode, int action, int mods) {
        ImGui::SetCurrentContext(static_cast<ImGuiContext*>(glfwGetWindowUserPointer(cb_window)));
        ImGui_ImplGlfw_KeyCallback(cb_window, key, scancode, action, mods);
    });
    glfwSetCharCallback(out.download_queue_window, [](GLFWwindow* cb_window, unsigned int c) {
        ImGui::SetCurrentContext(static_cast<ImGuiContext*>(glfwGetWindowUserPointer(cb_window)));
        ImGui_ImplGlfw_CharCallback(cb_window, c);
    });
    ImGui_ImplVulkan_InitInfo queue_init_info = init_info;
    queue_init_info.RenderPass = out.download_queue_window_data.RenderPass;
    queue_init_info.ImageCount = out.download_queue_window_data.ImageCount;
    ImGui_ImplVulkan_Init(&queue_init_info);
    uploadCurrentImGuiFonts(out.download_queue_window_data);

    ImGui::SetCurrentContext(out.main_imgui_context);
    return true;
}
