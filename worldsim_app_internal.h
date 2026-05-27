#pragma once

#include "worldsim_app.h"
#include "worldsim_gpu_state_internal.h"

#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"
#include "screenshot_state.h"

#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <list>
#include <mutex>
#include <optional>
#include <atomic>
#include <string>
#include <thread>
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
inline constexpr int kMinZoom = 2;
inline constexpr int kMaxZoom = 24;
inline constexpr int kMaxNativeTileZoom = 18;
inline constexpr int kMaxSatelliteNativeTileZoom = 20;
inline constexpr int kMaxNightSatelliteNativeTileZoom = 7;
inline constexpr int kMaxInternalMathZoom = 24;
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
extern std::mutex g_QueueSubmitMutex;
extern VkDescriptorPool g_DescriptorPool;
extern VkSampler g_TileSampler;
extern VkCommandPool g_UploadCommandPool;
extern VkCommandBuffer g_UploadCommandBuffer;
extern ImGui_ImplVulkanH_Window g_MainWindowData;
extern int g_MinImageCount;
extern bool g_SwapChainRebuild;
extern bool g_MainSwapchainTransferSrcSupported;
extern VkCommandBuffer g_CurrentFrameRenderCommandBuffer;
extern VkRenderPass g_CurrentFrameRenderPass;
extern uint32_t g_CurrentFrameRenderIndex;
extern std::atomic<uint64_t> g_PresentedFrameSerial;
extern std::unordered_map<std::string, TileCacheEntry> g_TileCache;
extern std::list<std::string> g_TileLRU;
extern bool g_EnableValidationLayers;
extern std::vector<TileTexture> g_RetiredTextures;
extern std::atomic<bool> g_VulkanDeviceLost;
extern ParcelGpuBuffers g_ParcelGpuBuffers;
extern bool g_ParcelGpuOverlayHasVisibleColors;
extern bool g_ParcelGpuOutlineHasVisibleColors;
extern ParcelGpuDrawState g_ParcelGpuDrawState;
extern ParcelGpuPipeline g_ParcelGpuPipeline;
extern std::unordered_map<size_t, ZoningGpuLayerState> g_ZoningGpuLayers;
extern CrimePointGpuBuffers g_CrimePointGpuBuffers;
extern CrimePointGpuDrawState g_CrimePointGpuDrawState;
extern CrimePointGpuPipeline g_CrimePointGpuPipeline;
extern float g_MapPolygonOutlineThickness;
extern bool g_WideLinesEnabled;
extern float g_MinSupportedLineWidth;
extern float g_MaxSupportedLineWidth;
extern std::atomic<bool> g_ParcelGpuUploadStop;
extern std::mutex g_ParcelGpuUploadRequestMutex;
extern std::condition_variable g_ParcelGpuUploadCv;
extern std::optional<ParcelRenderCacheBlob> g_ParcelGpuUploadPendingRequest;
extern std::mutex g_ParcelGpuUploadResultMutex;
extern std::optional<ParcelGpuUploadResult> g_ParcelGpuUploadCompletedResult;
extern std::thread g_ParcelGpuUploadWorker;
extern std::vector<RetiredParcelGpuPayload> g_RetiredParcelGpuPayloads;
extern std::mutex g_ParcelGpuStatusMutex;
extern ParcelGpuResidencyStatus g_ParcelGpuStatusSnapshot;
extern std::mutex g_GpuProfilerAlertMutex;
extern GpuProfilerAlertState g_GpuProfilerAlertState;
extern std::mutex g_GpuProfilerEventMutex;
extern std::deque<GpuProfilerEvent> g_GpuProfilerEvents;
extern GpuPickResources g_GpuPickResources;

void check_vk_result(VkResult err);
bool check_vk_result_allow_device_loss(VkResult err);
void SetupVulkan(const char** extensions, uint32_t extensions_count);
void SetupVulkanWindow(ImGui_ImplVulkanH_Window* wd, VkSurfaceKHR surface, int width, int height);
void CleanupVulkanWindow();
void CleanupVulkan();
void FrameRender(ImGui_ImplVulkanH_Window* wd, ImDrawData* draw_data);
void FramePresent(ImGui_ImplVulkanH_Window* wd);
void FrameRenderSecondary(ImGui_ImplVulkanH_Window* wd, ImDrawData* draw_data, bool& swapchain_rebuild);
void FramePresentSecondary(ImGui_ImplVulkanH_Window* wd, bool& swapchain_rebuild);
void drainRetiredTextures(bool force = false);
void destroyTileTexture(TileTexture& tex);
void destroyTileTextureNow(TileTexture& tex);
bool finalizeTileTextureDescriptor(TileTexture& tex);
bool uploadRgbaTexture(const unsigned char* pixels, uint32_t w, uint32_t h, TileTexture& tex);
TileSample getTileSample(
    const std::filesystem::path& root,
    const std::string& tile_root_dir,
    int z,
    int x,
    int y,
    int max_native_tile_zoom = kMaxNativeTileZoom);
TileTexture* getExactImageTexture(
    const std::filesystem::path& image_path,
    const std::string& cache_key = "");
const std::vector<std::vector<ImVec2>>& getTopoVectorLines(const std::filesystem::path& root);
void recordZoningOutlineIndirectComputeDispatches(VkCommandBuffer cmd);
uint64_t parcelDeviceLocalBytes(const ParcelGpuBuffers& buffers);
uint64_t parcelHostVisibleBytes(const ParcelGpuBuffers& buffers);
void publishParcelGpuStatusSnapshot();
void destroyParcelGpuBuffer(ParcelGpuBuffer& b);
void destroyParcelGpuBuffers(ParcelGpuBuffers& buffers);
void waitForParcelGpuDeviceIdle();
bool createHostVisibleParcelBuffer(
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    ParcelGpuBuffer& out,
    std::string* error);
bool uploadDeviceLocalParcelBuffer(
    const void* src,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    ParcelGpuBuffer& out,
    std::string* error);
void retireParcelGpuBuffers(ParcelGpuBuffers&& buffers);
bool createParcelGpuUploadContext(ParcelGpuUploadContext& out, std::string* error);
void destroyParcelGpuUploadContext(ParcelGpuUploadContext& ctx);
bool buildParcelGpuUploadPayload(
    ParcelGpuUploadContext& ctx,
    const ParcelRenderCacheBlob& blob,
    ParcelGpuUploadPayload& out,
    std::string* error);
uint32_t parcelGpuDescriptorFrameCount();
uint32_t findMemoryType(uint32_t type_filter, VkMemoryPropertyFlags properties);
bool tryCreateBuffer(
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties,
    VkBuffer& buffer,
    VkDeviceMemory& memory,
    std::string* error);
std::vector<uint32_t> loadSpirvFile(const char* path);
void destroyGpuPickResources();
void cleanupFeatureOverlayGpuServices();
