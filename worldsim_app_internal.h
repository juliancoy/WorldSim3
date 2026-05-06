#pragma once

#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"
#include "screenshot_state.h"

#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

struct TileTexture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSet descriptor = VK_NULL_HANDLE;
    uint32_t mip_levels = 1;
};

struct TileCacheEntry {
    TileTexture tex;
    std::list<std::string>::iterator lru_it;
};

struct TileSample {
    TileTexture* tex = nullptr;
    ImVec2 uv0 = ImVec2(0.0f, 0.0f);
    ImVec2 uv1 = ImVec2(1.0f, 1.0f);
};

inline constexpr const char* kAppVersion = "0.1.0";
inline constexpr int kProtocolVersion = 1;
inline constexpr int kMinZoom = 11;
inline constexpr int kMaxZoom = 64;
inline constexpr int kMaxNativeTileZoom = 18;
inline constexpr int kMaxInternalMathZoom = 22;
inline constexpr size_t kMaxTileCache = 320;
inline constexpr size_t kMaxSmoothHeatSamplesPerLayer = 50000;
inline constexpr int kSmoothHeatRasterBasePx = 1536;
inline constexpr int kSmoothHeatRasterMaxPx = 2048;

extern ScreenshotRequestState g_ScreenshotState;
extern VkAllocationCallbacks* g_Allocator;
extern VkInstance g_Instance;
extern VkPhysicalDevice g_PhysicalDevice;
extern VkDevice g_Device;
extern uint32_t g_QueueFamily;
extern VkQueue g_Queue;
extern VkDescriptorPool g_DescriptorPool;
extern VkSampler g_TileSampler;
extern ImGui_ImplVulkanH_Window g_MainWindowData;
extern int g_MinImageCount;
extern bool g_SwapChainRebuild;
extern std::unordered_map<std::string, TileCacheEntry> g_TileCache;
extern std::list<std::string> g_TileLRU;
extern bool g_EnableValidationLayers;
extern std::vector<TileTexture> g_RetiredTextures;

void check_vk_result(VkResult err);
void SetupVulkan(const char** extensions, uint32_t extensions_count);
void SetupVulkanWindow(ImGui_ImplVulkanH_Window* wd, VkSurfaceKHR surface, int width, int height);
void CleanupVulkanWindow();
void CleanupVulkan();
void FrameRender(ImGui_ImplVulkanH_Window* wd, ImDrawData* draw_data);
void FramePresent(ImGui_ImplVulkanH_Window* wd);
void drainRetiredTextures(bool force = false);
void destroyTileTexture(TileTexture& tex);
void destroyTileTextureNow(TileTexture& tex);
bool uploadRgbaTexture(const unsigned char* pixels, uint32_t w, uint32_t h, TileTexture& tex);
TileSample getTileSample(const std::filesystem::path& root, int z, int x, int y);
