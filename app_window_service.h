#pragma once

#include "app_settings.h"
#include "worldsim_app_internal.h"

#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <string>

struct AppWindowServiceContext {
    int initial_window_w = 0;
    int initial_window_h = 0;
    GLFWwindow* main_window = nullptr;
    ImGui_ImplVulkanH_Window* main_window_data = nullptr;
    ImGuiContext* main_imgui_context = nullptr;
    int main_framebuffer_w = 0;
    int main_framebuffer_h = 0;

    GLFWwindow* download_queue_window = nullptr;
    ImGui_ImplVulkanH_Window download_queue_window_data{};
    ImGuiContext* download_queue_imgui_context = nullptr;
    bool download_queue_swapchain_rebuild = false;
    int download_queue_framebuffer_w = 0;
    int download_queue_framebuffer_h = 0;
};

bool initializeAppWindows(const AppSettings& app_settings, AppWindowServiceContext& out, std::string* error);
