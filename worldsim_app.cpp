#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_vulkan.h"
#include "layer_state_io.h"
#include "feature_props.h"
#include "geo.h"
#include "cache_io.h"
#include "arkavo_realtime_client.h"
#include "arkavo_signaling_transport_curl.h"
#include "arkavo_rtc_session_manager.h"
#include "time_cube.h"
#include "app_settings.h"
#include "dataset_library.h"
#include "net_http_utils.h"
#include "profiling.h"
#include "layer_runtime.h"
#include "layer_geometry.h"
#include "layer_workers.h"
#include "heatmap_render.h"
#include "heatmap_gpu_aggregate.h"
#include "time_cube_panel.h"
#include "policy_panel.h"
#include "model_tabs_panel.h"
#include "worldsim_app.h"
#include "app_utils.h"
#include "zoning.h"
#include "vacancy_overlay.h"
#include "status_api.h"
#include "dataset_lan_api.h"
#include "screenshot_state.h"
#include "worldsim_gpu_state_internal.h"
#include "worldsim_app_internal.h"
#include "tiles.h"

#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <nlohmann/json.hpp>
#include <curl/curl.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <list>
#include <optional>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <functional>
#include <array>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <future>
#include <deque>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <cctype>
#include <cfloat>
#include <random>
using json = nlohmann::json;
namespace fs = std::filesystem;
ScreenshotRequestState g_ScreenshotState;


VkAllocationCallbacks* g_Allocator = nullptr;
VkInstance g_Instance = VK_NULL_HANDLE;
VkPhysicalDevice g_PhysicalDevice = VK_NULL_HANDLE;
VkDevice g_Device = VK_NULL_HANDLE;
uint32_t g_QueueFamily = (uint32_t)-1;
VkQueue g_Queue = VK_NULL_HANDLE;
std::mutex g_QueueSubmitMutex;
VkDescriptorPool g_DescriptorPool = VK_NULL_HANDLE;
VkSampler g_TileSampler = VK_NULL_HANDLE;
VkCommandPool g_UploadCommandPool = VK_NULL_HANDLE;
VkCommandBuffer g_UploadCommandBuffer = VK_NULL_HANDLE;

ParcelGpuBuffers g_ParcelGpuBuffers;
bool g_ParcelGpuOverlayHasVisibleColors = false;
bool g_ParcelGpuOutlineHasVisibleColors = false;
static bool g_MultiDrawIndirectEnabled = false;
static uint32_t g_MaxDrawIndirectCount = 1;
ParcelGpuDrawState g_ParcelGpuDrawState;
ParcelGpuPipeline g_ParcelGpuPipeline;
static ParcelGpuPipeline g_ZoningGpuPipeline;
static ZoningOutlineIndirectComputePipeline g_ZoningOutlineIndirectComputePipeline;
std::unordered_map<size_t, ZoningGpuLayerState> g_ZoningGpuLayers;
float g_MapPolygonOutlineThickness = 2.0f;
bool g_WideLinesEnabled = false;
float g_MinSupportedLineWidth = 1.0f;
float g_MaxSupportedLineWidth = 1.0f;
VkCommandBuffer g_CurrentFrameRenderCommandBuffer = VK_NULL_HANDLE;
VkRenderPass g_CurrentFrameRenderPass = VK_NULL_HANDLE;
uint32_t g_CurrentFrameRenderIndex = 0;
std::atomic<bool> g_ParcelGpuUploadStop{false};
std::mutex g_ParcelGpuUploadRequestMutex;
std::condition_variable g_ParcelGpuUploadCv;
std::optional<ParcelRenderCacheBlob> g_ParcelGpuUploadPendingRequest;
std::mutex g_ParcelGpuUploadResultMutex;
std::optional<ParcelGpuUploadResult> g_ParcelGpuUploadCompletedResult;
std::thread g_ParcelGpuUploadWorker;
std::vector<RetiredParcelGpuPayload> g_RetiredParcelGpuPayloads;
std::mutex g_ParcelGpuStatusMutex;
ParcelGpuResidencyStatus g_ParcelGpuStatusSnapshot;
std::mutex g_GpuProfilerAlertMutex;
GpuProfilerAlertState g_GpuProfilerAlertState;
std::mutex g_GpuProfilerEventMutex;
std::deque<GpuProfilerEvent> g_GpuProfilerEvents;
std::atomic<uint64_t> g_PresentedFrameSerial{0};
static VkDebugUtilsMessengerEXT g_DebugUtilsMessenger = VK_NULL_HANDLE;

uint64_t parcelDeviceLocalBytes(const ParcelGpuBuffers& buffers) {
    return
        (uint64_t)buffers.positions.size_bytes +
        (uint64_t)buffers.indices.size_bytes +
        (uint64_t)buffers.line_indices.size_bytes +
        (uint64_t)buffers.vertex_feature_refs.size_bytes +
        (uint64_t)buffers.outline_feature_records.size_bytes;
}

uint64_t parcelHostVisibleBytes(const ParcelGpuBuffers& buffers) {
    return
        (uint64_t)buffers.colors.size_bytes +
        (uint64_t)buffers.overlay_colors.size_bytes +
        (uint64_t)buffers.outline_colors.size_bytes;
}

void recordGpuProfilerEvent(const std::string& label) {
    if (label.empty()) return;
    std::lock_guard<std::mutex> lk(g_GpuProfilerEventMutex);
    g_GpuProfilerEvents.push_back(GpuProfilerEvent{std::chrono::steady_clock::now(), label});
    while (g_GpuProfilerEvents.size() > 24) g_GpuProfilerEvents.pop_front();
}

static void noteGpuProfilerAlert(VkResult err, const std::string& stage) {
    std::lock_guard<std::mutex> lk(g_GpuProfilerAlertMutex);
    g_GpuProfilerAlertState.last_error = stage + " failed with VkResult=" + std::to_string((int)err);
    recordGpuProfilerEvent(g_GpuProfilerAlertState.last_error);
    if (err == VK_ERROR_OUT_OF_DEVICE_MEMORY || err == VK_ERROR_OUT_OF_HOST_MEMORY) {
        g_GpuProfilerAlertState.oom_active = true;
        g_GpuProfilerAlertState.oom_generation++;
        g_GpuProfilerAlertState.tab_selection_requested = true;
    }
}

void publishParcelGpuStatusSnapshot() {
    ParcelGpuResidencyStatus out;
    out.resident =
        g_ParcelGpuBuffers.positions.buffer &&
        g_ParcelGpuBuffers.indices.buffer &&
        g_ParcelGpuBuffers.line_indices.buffer &&
        g_ParcelGpuBuffers.vertex_feature_refs.buffer &&
        g_ParcelGpuBuffers.colors.buffer &&
        g_ParcelGpuBuffers.overlay_colors.buffer &&
        g_ParcelGpuBuffers.outline_colors.buffer;
    out.render_features = g_ParcelGpuBuffers.render_features;
    out.vertices = g_ParcelGpuBuffers.vertices;
    out.indices = g_ParcelGpuBuffers.indices_count;
    out.line_indices = g_ParcelGpuBuffers.line_indices_count;
    out.colors = g_ParcelGpuBuffers.render_features;
    out.visible_chunks = static_cast<uint32_t>(g_ParcelGpuDrawState.visible_chunks.size());
    out.visible_line_chunks = static_cast<uint32_t>(g_ParcelGpuDrawState.visible_line_chunks.size());
    out.draw_active =
        g_ParcelGpuDrawState.active &&
        !g_ParcelGpuDrawState.visible_chunks.empty() &&
        g_ParcelGpuBuffers.positions.buffer &&
        g_ParcelGpuBuffers.indices.buffer &&
        g_ParcelGpuBuffers.vertex_feature_refs.buffer &&
        g_ParcelGpuBuffers.colors.buffer;
    out.overlay_active = out.draw_active && g_ParcelGpuBuffers.overlay_colors.mapped && g_ParcelGpuOverlayHasVisibleColors;
    out.outline_active =
        out.draw_active &&
        g_ParcelGpuBuffers.outline_colors.mapped &&
        !g_ParcelGpuDrawState.visible_line_chunks.empty() &&
        g_ParcelGpuOutlineHasVisibleColors;
    out.device_local_bytes = parcelDeviceLocalBytes(g_ParcelGpuBuffers);
    out.host_visible_bytes = parcelHostVisibleBytes(g_ParcelGpuBuffers);
    for (const auto& retired : g_RetiredParcelGpuPayloads) {
        out.retired_device_local_bytes += parcelDeviceLocalBytes(retired.buffers);
        out.retired_host_visible_bytes += parcelHostVisibleBytes(retired.buffers);
    }
    out.source_signature = g_ParcelGpuBuffers.source_signature;

    std::lock_guard<std::mutex> lk(g_ParcelGpuStatusMutex);
    g_ParcelGpuStatusSnapshot = std::move(out);
}

#if defined(WS3_PARCEL_GPU_VERT_SPV)
static const char* kParcelGpuVertShaderPath = WS3_PARCEL_GPU_VERT_SPV;
#else
static const char* kParcelGpuVertShaderPath = nullptr;
#endif

#if defined(WS3_PARCEL_GPU_FRAG_SPV)
static const char* kParcelGpuFragShaderPath = WS3_PARCEL_GPU_FRAG_SPV;
#else
static const char* kParcelGpuFragShaderPath = nullptr;
#endif

#if defined(WS3_CRIME_POINT_GPU_VERT_SPV)
static const char* kCrimePointGpuVertShaderPath = WS3_CRIME_POINT_GPU_VERT_SPV;
#else
static const char* kCrimePointGpuVertShaderPath = nullptr;
#endif

#if defined(WS3_CRIME_POINT_GPU_FRAG_SPV)
static const char* kCrimePointGpuFragShaderPath = WS3_CRIME_POINT_GPU_FRAG_SPV;
#else
static const char* kCrimePointGpuFragShaderPath = nullptr;
#endif

#if defined(WS3_ZONING_OUTLINE_INDIRECT_SHADER_SPV)
static const char* kZoningOutlineIndirectShaderPath = WS3_ZONING_OUTLINE_INDIRECT_SHADER_SPV;
#else
static const char* kZoningOutlineIndirectShaderPath = nullptr;
#endif

struct ParcelGpuPushConstants {
    float center_lonlat[2];
    float center_world[2];
    float viewport_origin[2];
    float viewport_size[2];
    float framebuffer_size[2];
    float math_zoom = 0.0f;
    float zoom_scale = 1.0f;
};

ImGui_ImplVulkanH_Window g_MainWindowData;
int g_MinImageCount = 2;
bool g_SwapChainRebuild = false;
bool g_MainSwapchainTransferSrcSupported = false;

std::unordered_map<std::string, TileCacheEntry> g_TileCache;
std::list<std::string> g_TileLRU;
bool g_EnableValidationLayers = false;
std::vector<TileTexture> g_RetiredTextures;
std::atomic<bool> g_VulkanDeviceLost{false};

bool check_vk_result_allow_device_loss(VkResult err) {
    if (err == 0) return true;
    std::fprintf(stderr, "[vulkan] VkResult=%d\n", err);
    if (err == VK_ERROR_DEVICE_LOST) {
        g_VulkanDeviceLost.store(true, std::memory_order_relaxed);
        return false;
    }
    if (err < 0) std::abort();
    return true;
}

void check_vk_result(VkResult err) {
    (void)check_vk_result_allow_device_loss(err);
}

static float resolvedMapPolygonOutlineThickness() {
    const float requested = std::clamp(g_MapPolygonOutlineThickness, 1.0f, 8.0f);
    if (!g_WideLinesEnabled) return 1.0f;
    return std::clamp(requested, g_MinSupportedLineWidth, g_MaxSupportedLineWidth);
}

static VKAPI_ATTR VkBool32 VKAPI_CALL DebugUtilsCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT* callback_data,
    void* user_data) {
    (void)severity;
    (void)type;
    (void)user_data;
    std::fprintf(stderr, "[vulkan validation] %s\n", callback_data->pMessage);
    return VK_FALSE;
}

static bool IsInstanceLayerAvailable(const char* layer_name) {
    uint32_t layer_count = 0;
    vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
    std::vector<VkLayerProperties> layers(layer_count);
    vkEnumerateInstanceLayerProperties(&layer_count, layers.data());
    for (const auto& layer : layers) {
        if (std::strcmp(layer.layerName, layer_name) == 0) return true;
    }
    return false;
}

static bool IsInstanceExtensionAvailable(const char* ext_name) {
    uint32_t ext_count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> exts(ext_count);
    vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, exts.data());
    for (const auto& ext : exts) {
        if (std::strcmp(ext.extensionName, ext_name) == 0) return true;
    }
    return false;
}

static void CreateDebugUtilsMessenger() {
    if (!g_EnableValidationLayers) return;
    auto create_fn = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_Instance, "vkCreateDebugUtilsMessengerEXT");
    if (!create_fn) return;
    VkDebugUtilsMessengerCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = DebugUtilsCallback;
    check_vk_result(create_fn(g_Instance, &info, g_Allocator, &g_DebugUtilsMessenger));
}

uint32_t findMemoryType(uint32_t type_filter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++) {
        if ((type_filter & (1u << i)) && (mem.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    std::abort();
}

static void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& memory) {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check_vk_result(vkCreateBuffer(g_Device, &info, g_Allocator, &buffer));

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_Device, buffer, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = findMemoryType(req.memoryTypeBits, properties);
    check_vk_result(vkAllocateMemory(g_Device, &alloc, g_Allocator, &memory));
    check_vk_result(vkBindBufferMemory(g_Device, buffer, memory, 0));
}

bool tryCreateBuffer(
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties,
    VkBuffer& buffer,
    VkDeviceMemory& memory,
    std::string* error) {
    buffer = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;

    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult err = vkCreateBuffer(g_Device, &info, g_Allocator, &buffer);
    if (err != VK_SUCCESS) {
        noteGpuProfilerAlert(err, "vkCreateBuffer");
        if (error) *error = "vkCreateBuffer failed with VkResult=" + std::to_string((int)err);
        return false;
    }

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(g_Device, buffer, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = findMemoryType(req.memoryTypeBits, properties);
    err = vkAllocateMemory(g_Device, &alloc, g_Allocator, &memory);
    if (err != VK_SUCCESS) {
        noteGpuProfilerAlert(err, "vkAllocateMemory");
        if (error) *error = "vkAllocateMemory failed with VkResult=" + std::to_string((int)err);
        vkDestroyBuffer(g_Device, buffer, g_Allocator);
        buffer = VK_NULL_HANDLE;
        return false;
    }

    err = vkBindBufferMemory(g_Device, buffer, memory, 0);
    if (err != VK_SUCCESS) {
        noteGpuProfilerAlert(err, "vkBindBufferMemory");
        if (error) *error = "vkBindBufferMemory failed with VkResult=" + std::to_string((int)err);
        vkFreeMemory(g_Device, memory, g_Allocator);
        vkDestroyBuffer(g_Device, buffer, g_Allocator);
        memory = VK_NULL_HANDLE;
        buffer = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

void destroyParcelGpuBuffer(ParcelGpuBuffer& b) {
    if (b.mapped && b.memory) {
        vkUnmapMemory(g_Device, b.memory);
        b.mapped = nullptr;
    }
    if (b.buffer) {
        vkDestroyBuffer(g_Device, b.buffer, g_Allocator);
        b.buffer = VK_NULL_HANDLE;
    }
    if (b.memory) {
        vkFreeMemory(g_Device, b.memory, g_Allocator);
        b.memory = VK_NULL_HANDLE;
    }
    b.size_bytes = 0;
}

static void destroyGpuPickBuffer(GpuPickBuffer& b) {
    if (b.buffer) {
        vkDestroyBuffer(g_Device, b.buffer, g_Allocator);
        b.buffer = VK_NULL_HANDLE;
    }
    if (b.memory) {
        vkFreeMemory(g_Device, b.memory, g_Allocator);
        b.memory = VK_NULL_HANDLE;
    }
    b.size_bytes = 0;
}

static bool createGpuPickBuffer(
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties,
    GpuPickBuffer& out,
    std::string* error) {
    destroyGpuPickBuffer(out);
    if (!tryCreateBuffer(size, usage, properties, out.buffer, out.memory, error)) return false;
    out.size_bytes = size;
    return true;
}

void destroyParcelGpuBuffers(ParcelGpuBuffers& buffers) {
    destroyParcelGpuBuffer(buffers.positions);
    destroyParcelGpuBuffer(buffers.indices);
    destroyParcelGpuBuffer(buffers.line_indices);
    destroyParcelGpuBuffer(buffers.vertex_feature_refs);
    destroyParcelGpuBuffer(buffers.colors);
    destroyParcelGpuBuffer(buffers.overlay_colors);
    destroyParcelGpuBuffer(buffers.outline_colors);
    destroyParcelGpuBuffer(buffers.outline_feature_records);
    buffers.features.clear();
    buffers.chunks.clear();
    buffers.render_features = 0;
    buffers.vertices = 0;
    buffers.indices_count = 0;
    buffers.line_indices_count = 0;
    buffers.source_signature.clear();
}

static void destroyZoningGpuLayerState(ZoningGpuLayerState& state) {
    destroyParcelGpuBuffers(state.buffers);
    for (ParcelGpuBuffer& buffer : state.outline_indirect_commands_by_frame) {
        destroyParcelGpuBuffer(buffer);
    }
    if (g_Device && g_DescriptorPool && !state.outline_compute_descriptor_sets_by_frame.empty()) {
        vkFreeDescriptorSets(
            g_Device,
            g_DescriptorPool,
            (uint32_t)state.outline_compute_descriptor_sets_by_frame.size(),
            state.outline_compute_descriptor_sets_by_frame.data());
    }
    state = ZoningGpuLayerState{};
}

static void destroyPolylineLayerGpuBuffers(PolylineLayerGpuBuffers& buffers) {
    destroyParcelGpuBuffer(buffers.positions);
    destroyParcelGpuBuffer(buffers.feature_refs);
    destroyParcelGpuBuffer(buffers.line_indices);
    destroyParcelGpuBuffer(buffers.colors);
    buffers.chunks.clear();
    buffers.render_features = 0;
    buffers.vertices = 0;
    buffers.line_indices_count = 0;
    buffers.source_signature.clear();
}

void waitForParcelGpuDeviceIdle() {
    if (g_Device == VK_NULL_HANDLE) return;
    std::lock_guard<std::mutex> qlk(g_QueueSubmitMutex);
    check_vk_result(vkDeviceWaitIdle(g_Device));
}

void retireParcelGpuBuffers(ParcelGpuBuffers&& buffers) {
    if (!buffers.positions.buffer &&
        !buffers.indices.buffer &&
        !buffers.line_indices.buffer &&
        !buffers.vertex_feature_refs.buffer &&
        !buffers.colors.buffer &&
        !buffers.overlay_colors.buffer &&
        !buffers.outline_colors.buffer &&
        !buffers.outline_feature_records.buffer) {
        return;
    }
    RetiredParcelGpuPayload retired;
    retired.buffers = std::move(buffers);
    retired.retire_after_frame = g_PresentedFrameSerial.load(std::memory_order_relaxed) + std::max<uint64_t>(2, (uint64_t)g_MinImageCount + 1);
    g_RetiredParcelGpuPayloads.push_back(std::move(retired));
}

static void drainRetiredParcelGpuPayloads(bool force = false) {
    if (g_RetiredParcelGpuPayloads.empty()) return;
    const uint64_t presented_frame = g_PresentedFrameSerial.load(std::memory_order_relaxed);
    bool has_eligible = force;
    if (!has_eligible) {
        for (const auto& retired : g_RetiredParcelGpuPayloads) {
            if (presented_frame >= retired.retire_after_frame) {
                has_eligible = true;
                break;
            }
        }
    }
    if (!has_eligible) return;
    waitForParcelGpuDeviceIdle();
    bool changed = false;
    auto it = g_RetiredParcelGpuPayloads.begin();
    while (it != g_RetiredParcelGpuPayloads.end()) {
        if (!force && presented_frame < it->retire_after_frame) {
            ++it;
            continue;
        }
        destroyParcelGpuBuffers(it->buffers);
        it = g_RetiredParcelGpuPayloads.erase(it);
        changed = true;
    }
    if (changed) publishParcelGpuStatusSnapshot();
}

void drainRetiredParcelGpuResources() {
    drainRetiredParcelGpuPayloads(false);
}

bool createHostVisibleParcelBuffer(
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    ParcelGpuBuffer& out,
    std::string* error) {
    destroyParcelGpuBuffer(out);
    if (!tryCreateBuffer(
            size,
            usage,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            out.buffer,
            out.memory,
            error)) {
        return false;
    }
    if (vkMapMemory(g_Device, out.memory, 0, size, 0, &out.mapped) != VK_SUCCESS) {
        if (error) *error = "failed to map host-visible parcel buffer";
        destroyParcelGpuBuffer(out);
        return false;
    }
    out.size_bytes = size;
    return true;
}

static bool createDeviceLocalParcelBuffer(
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    ParcelGpuBuffer& out,
    std::string* error) {
    destroyParcelGpuBuffer(out);
    if (!tryCreateBuffer(
            size,
            usage,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            out.buffer,
            out.memory,
            error)) {
        return false;
    }
    out.size_bytes = size;
    return true;
}

bool createParcelGpuUploadContext(ParcelGpuUploadContext& out, std::string* error) {
    if (!g_Device) {
        if (error) *error = "Vulkan device is not ready";
        return false;
    }
    VkCommandPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.queueFamilyIndex = g_QueueFamily;
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    if (vkCreateCommandPool(g_Device, &pool, g_Allocator, &out.command_pool) != VK_SUCCESS) {
        if (error) *error = "vkCreateCommandPool failed for parcel upload worker";
        return false;
    }
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = out.command_pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(g_Device, &alloc, &out.command_buffer) != VK_SUCCESS) {
        if (error) *error = "vkAllocateCommandBuffers failed for parcel upload worker";
        vkDestroyCommandPool(g_Device, out.command_pool, g_Allocator);
        out.command_pool = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

static void submitSharedUploadCommands(std::function<void(VkCommandBuffer)> record) {
    check_vk_result(vkResetCommandPool(g_Device, g_UploadCommandPool, 0));

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk_result(vkBeginCommandBuffer(g_UploadCommandBuffer, &begin));
    record(g_UploadCommandBuffer);
    check_vk_result(vkEndCommandBuffer(g_UploadCommandBuffer));

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &g_UploadCommandBuffer;
    {
        std::lock_guard<std::mutex> qlk(g_QueueSubmitMutex);
        check_vk_result(vkQueueSubmit(g_Queue, 1, &submit, VK_NULL_HANDLE));
    }
    check_vk_result(vkQueueWaitIdle(g_Queue));
}

void destroyParcelGpuUploadContext(ParcelGpuUploadContext& ctx) {
    if (ctx.command_pool) {
        vkDestroyCommandPool(g_Device, ctx.command_pool, g_Allocator);
        ctx.command_pool = VK_NULL_HANDLE;
        ctx.command_buffer = VK_NULL_HANDLE;
    }
}

static void submitUploadCommands(ParcelGpuUploadContext& ctx, std::function<void(VkCommandBuffer)> record) {
    check_vk_result(vkResetCommandPool(g_Device, ctx.command_pool, 0));

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk_result(vkBeginCommandBuffer(ctx.command_buffer, &begin));
    record(ctx.command_buffer);
    check_vk_result(vkEndCommandBuffer(ctx.command_buffer));

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &ctx.command_buffer;
    {
        std::lock_guard<std::mutex> qlk(g_QueueSubmitMutex);
        check_vk_result(vkQueueSubmit(g_Queue, 1, &submit, VK_NULL_HANDLE));
        check_vk_result(vkQueueWaitIdle(g_Queue));
    }
}

bool uploadDeviceLocalParcelBuffer(
    const void* src,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    ParcelGpuBuffer& out,
    std::string* error) {
    destroyParcelGpuBuffer(out);
    if (!src || size == 0) {
        if (error) *error = "invalid parcel buffer upload input";
        return false;
    }

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    std::string create_error;
    if (!tryCreateBuffer(
            size,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            staging,
            staging_mem,
            &create_error)) {
        if (error) *error = "failed to create staging buffer: " + create_error;
        return false;
    }

    void* mapped = nullptr;
    if (vkMapMemory(g_Device, staging_mem, 0, size, 0, &mapped) != VK_SUCCESS) {
        if (error) *error = "failed to map staging buffer";
        vkDestroyBuffer(g_Device, staging, g_Allocator);
        vkFreeMemory(g_Device, staging_mem, g_Allocator);
        return false;
    }
    std::memcpy(mapped, src, static_cast<size_t>(size));
    vkUnmapMemory(g_Device, staging_mem);

    if (!tryCreateBuffer(
            size,
            usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            out.buffer,
            out.memory,
            &create_error)) {
        if (error) *error = "failed to create device-local parcel buffer: " + create_error;
        vkDestroyBuffer(g_Device, staging, g_Allocator);
        vkFreeMemory(g_Device, staging_mem, g_Allocator);
        return false;
    }

    submitSharedUploadCommands([&](VkCommandBuffer cmd) {
        VkBufferCopy copy{};
        copy.size = size;
        vkCmdCopyBuffer(cmd, staging, out.buffer, 1, &copy);
    });

    vkDestroyBuffer(g_Device, staging, g_Allocator);
    vkFreeMemory(g_Device, staging_mem, g_Allocator);
    out.size_bytes = size;
    return true;
}

static bool uploadDeviceLocalParcelBuffer(
    ParcelGpuUploadContext& ctx,
    const void* src,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    ParcelGpuBuffer& out,
    std::string* error) {
    destroyParcelGpuBuffer(out);
    if (!src || size == 0) {
        if (error) *error = "invalid parcel buffer upload input";
        return false;
    }

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    std::string create_error;
    if (!tryCreateBuffer(
            size,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            staging,
            staging_mem,
            &create_error)) {
        if (error) *error = "failed to create staging buffer: " + create_error;
        return false;
    }

    void* mapped = nullptr;
    if (vkMapMemory(g_Device, staging_mem, 0, size, 0, &mapped) != VK_SUCCESS) {
        if (error) *error = "failed to map staging buffer";
        vkDestroyBuffer(g_Device, staging, g_Allocator);
        vkFreeMemory(g_Device, staging_mem, g_Allocator);
        return false;
    }
    std::memcpy(mapped, src, static_cast<size_t>(size));
    vkUnmapMemory(g_Device, staging_mem);

    if (!tryCreateBuffer(
            size,
            usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            out.buffer,
            out.memory,
            &create_error)) {
        if (error) *error = "failed to create device-local parcel buffer: " + create_error;
        vkDestroyBuffer(g_Device, staging, g_Allocator);
        vkFreeMemory(g_Device, staging_mem, g_Allocator);
        return false;
    }

    submitUploadCommands(ctx, [&](VkCommandBuffer cmd) {
        VkBufferCopy copy{};
        copy.size = size;
        vkCmdCopyBuffer(cmd, staging, out.buffer, 1, &copy);
    });

    vkDestroyBuffer(g_Device, staging, g_Allocator);
    vkFreeMemory(g_Device, staging_mem, g_Allocator);
    out.size_bytes = size;
    return true;
}

bool buildParcelGpuUploadPayload(
    ParcelGpuUploadContext& ctx,
    const ParcelRenderCacheBlob& blob,
    ParcelGpuUploadPayload& out,
    std::string* error) {
    if (blob.vertices.empty() || blob.indices.empty() || blob.line_indices.empty() || blob.features.empty() ||
        blob.vertex_feature_refs.size() != blob.vertices.size()) {
        if (error) *error = "parcel render cache blob is incomplete";
        return false;
    }

    destroyParcelGpuBuffers(out.buffers);
    const VkDeviceSize positions_size = sizeof(ImVec2) * blob.vertices.size();
    const VkDeviceSize indices_size = sizeof(uint32_t) * blob.indices.size();
    const VkDeviceSize line_indices_size = sizeof(uint32_t) * blob.line_indices.size();
    const VkDeviceSize refs_size = sizeof(uint32_t) * blob.vertex_feature_refs.size();
    const VkDeviceSize colors_size = sizeof(ImU32) * blob.features.size();
    std::vector<ZoningOutlineFeatureGpuRecord> outline_features;
    outline_features.reserve(blob.features.size());
    for (const ParcelRenderFeatureRecord& feature : blob.features) {
        ZoningOutlineFeatureGpuRecord gpu_feature{};
        gpu_feature.feature_idx = feature.feature_idx;
        gpu_feature.vertex_offset = feature.vertex_offset;
        gpu_feature.vertex_count = feature.vertex_count;
        gpu_feature.index_offset = feature.index_offset;
        gpu_feature.index_count = feature.index_count;
        gpu_feature.line_index_offset = feature.line_index_offset;
        gpu_feature.line_index_count = feature.line_index_count;
        gpu_feature.min_lon = feature.min_lon;
        gpu_feature.min_lat = feature.min_lat;
        gpu_feature.max_lon = feature.max_lon;
        gpu_feature.max_lat = feature.max_lat;
        outline_features.push_back(gpu_feature);
    }
    const VkDeviceSize outline_features_size = sizeof(ZoningOutlineFeatureGpuRecord) * outline_features.size();
    if (!uploadDeviceLocalParcelBuffer(ctx, blob.vertices.data(), positions_size,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, out.buffers.positions, error) ||
        !uploadDeviceLocalParcelBuffer(ctx, blob.indices.data(), indices_size,
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, out.buffers.indices, error) ||
        !uploadDeviceLocalParcelBuffer(ctx, blob.line_indices.data(), line_indices_size,
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, out.buffers.line_indices, error) ||
        !uploadDeviceLocalParcelBuffer(ctx, blob.vertex_feature_refs.data(), refs_size,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, out.buffers.vertex_feature_refs, error) ||
        !uploadDeviceLocalParcelBuffer(ctx, outline_features.data(), outline_features_size,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, out.buffers.outline_feature_records, error) ||
        !createHostVisibleParcelBuffer(colors_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, out.buffers.colors, error) ||
        !createHostVisibleParcelBuffer(colors_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, out.buffers.overlay_colors, error) ||
        !createHostVisibleParcelBuffer(colors_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, out.buffers.outline_colors, error)) {
        destroyParcelGpuBuffers(out.buffers);
        return false;
    }

    std::vector<ImU32> default_colors(blob.features.size(), IM_COL32(0, 0, 0, 0));
    std::memcpy(out.buffers.colors.mapped, default_colors.data(), static_cast<size_t>(colors_size));
    std::memcpy(out.buffers.overlay_colors.mapped, default_colors.data(), static_cast<size_t>(colors_size));
    std::memcpy(out.buffers.outline_colors.mapped, default_colors.data(), static_cast<size_t>(colors_size));
    out.buffers.render_features = static_cast<uint32_t>(blob.features.size());
    out.buffers.vertices = static_cast<uint32_t>(blob.vertices.size());
    out.buffers.indices_count = static_cast<uint32_t>(blob.indices.size());
    out.buffers.line_indices_count = static_cast<uint32_t>(blob.line_indices.size());
    out.buffers.features = blob.features;
    out.buffers.chunks = blob.chunks;
    out.buffers.source_signature = blob.source_signature;
    return true;
}

std::vector<uint32_t> loadSpirvFile(const char* path) {
    if (!path || !*path) return {};
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    in.seekg(0, std::ios::end);
    const std::streamsize size = in.tellg();
    if (size <= 0 || (size % 4) != 0) return {};
    in.seekg(0, std::ios::beg);
    std::vector<uint32_t> code((size_t)size / 4);
    in.read(reinterpret_cast<char*>(code.data()), size);
    if (!in) return {};
    return code;
}

static bool rectsOverlap(
    float a_min_x,
    float a_min_y,
    float a_max_x,
    float a_max_y,
    float b_min_x,
    float b_min_y,
    float b_max_x,
    float b_max_y) {
    return !(a_max_x < b_min_x || b_max_x < a_min_x || a_max_y < b_min_y || b_max_y < a_min_y);
}

static void destroyParcelGpuPipeline() {
    if (g_ParcelGpuPipeline.fill_pipeline) {
        vkDestroyPipeline(g_Device, g_ParcelGpuPipeline.fill_pipeline, g_Allocator);
        g_ParcelGpuPipeline.fill_pipeline = VK_NULL_HANDLE;
    }
    if (g_ParcelGpuPipeline.line_pipeline) {
        vkDestroyPipeline(g_Device, g_ParcelGpuPipeline.line_pipeline, g_Allocator);
        g_ParcelGpuPipeline.line_pipeline = VK_NULL_HANDLE;
    }
    if (g_ParcelGpuPipeline.pipeline_layout) {
        vkDestroyPipelineLayout(g_Device, g_ParcelGpuPipeline.pipeline_layout, g_Allocator);
        g_ParcelGpuPipeline.pipeline_layout = VK_NULL_HANDLE;
    }
    if (g_ParcelGpuPipeline.descriptor_set_layout) {
        vkDestroyDescriptorSetLayout(g_Device, g_ParcelGpuPipeline.descriptor_set_layout, g_Allocator);
        g_ParcelGpuPipeline.descriptor_set_layout = VK_NULL_HANDLE;
    }
    g_ParcelGpuPipeline.descriptor_sets_by_frame.clear();
    g_ParcelGpuPipeline.descriptor_dirty_by_frame.clear();
    g_ParcelGpuPipeline.render_pass = VK_NULL_HANDLE;
    g_ParcelGpuPipeline.descriptor_dirty = true;
}

static void destroyZoningGpuPipeline() {
    if (g_ZoningGpuPipeline.fill_pipeline) {
        vkDestroyPipeline(g_Device, g_ZoningGpuPipeline.fill_pipeline, g_Allocator);
        g_ZoningGpuPipeline.fill_pipeline = VK_NULL_HANDLE;
    }
    if (g_ZoningGpuPipeline.line_pipeline) {
        vkDestroyPipeline(g_Device, g_ZoningGpuPipeline.line_pipeline, g_Allocator);
        g_ZoningGpuPipeline.line_pipeline = VK_NULL_HANDLE;
    }
    if (g_ZoningGpuPipeline.pipeline_layout) {
        vkDestroyPipelineLayout(g_Device, g_ZoningGpuPipeline.pipeline_layout, g_Allocator);
        g_ZoningGpuPipeline.pipeline_layout = VK_NULL_HANDLE;
    }
    if (g_ZoningGpuPipeline.descriptor_set_layout) {
        vkDestroyDescriptorSetLayout(g_Device, g_ZoningGpuPipeline.descriptor_set_layout, g_Allocator);
        g_ZoningGpuPipeline.descriptor_set_layout = VK_NULL_HANDLE;
    }
    g_ZoningGpuPipeline.descriptor_sets_by_frame.clear();
    g_ZoningGpuPipeline.descriptor_dirty_by_frame.clear();
    g_ZoningGpuPipeline.render_pass = VK_NULL_HANDLE;
    g_ZoningGpuPipeline.descriptor_dirty = true;
}

static void destroyZoningOutlineIndirectComputePipeline() {
    if (g_ZoningOutlineIndirectComputePipeline.pipeline) {
        vkDestroyPipeline(g_Device, g_ZoningOutlineIndirectComputePipeline.pipeline, g_Allocator);
        g_ZoningOutlineIndirectComputePipeline.pipeline = VK_NULL_HANDLE;
    }
    if (g_ZoningOutlineIndirectComputePipeline.pipeline_layout) {
        vkDestroyPipelineLayout(g_Device, g_ZoningOutlineIndirectComputePipeline.pipeline_layout, g_Allocator);
        g_ZoningOutlineIndirectComputePipeline.pipeline_layout = VK_NULL_HANDLE;
    }
    if (g_ZoningOutlineIndirectComputePipeline.descriptor_set_layout) {
        vkDestroyDescriptorSetLayout(g_Device, g_ZoningOutlineIndirectComputePipeline.descriptor_set_layout, g_Allocator);
        g_ZoningOutlineIndirectComputePipeline.descriptor_set_layout = VK_NULL_HANDLE;
    }
}

uint32_t parcelGpuDescriptorFrameCount() {
    return std::max<uint32_t>(1, g_MainWindowData.ImageCount);
}

static bool ensureParcelGpuDescriptorSetsAllocated(std::string* error) {
    if (!g_Device || !g_DescriptorPool || !g_ParcelGpuBuffers.colors.buffer || !g_ParcelGpuBuffers.overlay_colors.buffer || !g_ParcelGpuBuffers.outline_colors.buffer) {
        if (error) *error = "parcel GPU descriptor prerequisites are not ready";
        return false;
    }
    if (!g_ParcelGpuPipeline.descriptor_set_layout) {
        VkDescriptorSetLayoutBinding color_binding{};
        color_binding.binding = 0;
        color_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        color_binding.descriptorCount = 1;
        color_binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

        VkDescriptorSetLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.bindingCount = 1;
        layout_info.pBindings = &color_binding;
        if (vkCreateDescriptorSetLayout(g_Device, &layout_info, g_Allocator, &g_ParcelGpuPipeline.descriptor_set_layout) != VK_SUCCESS) {
            if (error) *error = "vkCreateDescriptorSetLayout failed for parcel GPU pipeline";
            return false;
        }
    }
    const uint32_t frame_count = parcelGpuDescriptorFrameCount();
    if (g_ParcelGpuPipeline.descriptor_sets_by_frame.size() != frame_count) {
        std::vector<VkDescriptorSetLayout> layouts((size_t)frame_count * 3, g_ParcelGpuPipeline.descriptor_set_layout);
        std::vector<VkDescriptorSet> sets(layouts.size(), VK_NULL_HANDLE);
        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = g_DescriptorPool;
        alloc_info.descriptorSetCount = (uint32_t)layouts.size();
        alloc_info.pSetLayouts = layouts.data();
        if (vkAllocateDescriptorSets(g_Device, &alloc_info, sets.data()) != VK_SUCCESS) {
            if (error) *error = "vkAllocateDescriptorSets failed for parcel GPU pipeline";
            return false;
        }
        g_ParcelGpuPipeline.descriptor_sets_by_frame.assign(frame_count, {});
        g_ParcelGpuPipeline.descriptor_dirty_by_frame.assign(frame_count, true);
        for (uint32_t frame = 0; frame < frame_count; ++frame) {
            g_ParcelGpuPipeline.descriptor_sets_by_frame[frame] = {
                sets[(size_t)frame * 3 + 0],
                sets[(size_t)frame * 3 + 1],
                sets[(size_t)frame * 3 + 2]
            };
        }
        g_ParcelGpuPipeline.descriptor_dirty = true;
    }
    if (g_ParcelGpuPipeline.descriptor_dirty) {
        if (g_ParcelGpuPipeline.descriptor_dirty_by_frame.size() != frame_count) {
            g_ParcelGpuPipeline.descriptor_dirty_by_frame.assign(frame_count, true);
        } else {
            std::fill(
                g_ParcelGpuPipeline.descriptor_dirty_by_frame.begin(),
                g_ParcelGpuPipeline.descriptor_dirty_by_frame.end(),
                true);
        }
        g_ParcelGpuPipeline.descriptor_dirty = false;
    }
    return true;
}

static bool ensureParcelGpuDescriptorSet(std::string* error, std::array<VkDescriptorSet, 3>* out_sets) {
    if (!ensureParcelGpuDescriptorSetsAllocated(error)) return false;
    if (!out_sets || g_ParcelGpuPipeline.descriptor_sets_by_frame.empty()) {
        if (error) *error = "parcel GPU descriptor set output is unavailable";
        return false;
    }
    const uint32_t frame_count = (uint32_t)g_ParcelGpuPipeline.descriptor_sets_by_frame.size();
    const uint32_t frame_index = frame_count > 0 ? (g_CurrentFrameRenderIndex % frame_count) : 0;
    *out_sets = g_ParcelGpuPipeline.descriptor_sets_by_frame[frame_index];
    if (frame_index >= g_ParcelGpuPipeline.descriptor_dirty_by_frame.size() ||
        !g_ParcelGpuPipeline.descriptor_dirty_by_frame[frame_index]) {
        return true;
    }

    VkDescriptorBufferInfo base_color_info{};
    base_color_info.buffer = g_ParcelGpuBuffers.colors.buffer;
    base_color_info.offset = 0;
    base_color_info.range = g_ParcelGpuBuffers.colors.size_bytes;
    VkDescriptorBufferInfo overlay_color_info{};
    overlay_color_info.buffer = g_ParcelGpuBuffers.overlay_colors.buffer;
    overlay_color_info.offset = 0;
    overlay_color_info.range = g_ParcelGpuBuffers.overlay_colors.size_bytes;
    VkDescriptorBufferInfo outline_color_info{};
    outline_color_info.buffer = g_ParcelGpuBuffers.outline_colors.buffer;
    outline_color_info.offset = 0;
    outline_color_info.range = g_ParcelGpuBuffers.outline_colors.size_bytes;

    VkWriteDescriptorSet writes[3]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = (*out_sets)[0];
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &base_color_info;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = (*out_sets)[1];
    writes[1].dstBinding = 0;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &overlay_color_info;
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = (*out_sets)[2];
    writes[2].dstBinding = 0;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo = &outline_color_info;
    vkUpdateDescriptorSets(g_Device, 3, writes, 0, nullptr);
    g_ParcelGpuPipeline.descriptor_dirty_by_frame[frame_index] = false;
    return true;
}

static bool ensureParcelGpuPipeline(VkRenderPass render_pass, std::string* error) {
    if (!g_Device || !render_pass) {
        if (error) *error = "parcel GPU render pass/device is not ready";
        return false;
    }
    if (!ensureParcelGpuDescriptorSetsAllocated(error)) return false;
    if (g_ParcelGpuPipeline.fill_pipeline && g_ParcelGpuPipeline.line_pipeline && g_ParcelGpuPipeline.render_pass == render_pass) return true;

    if (g_ParcelGpuPipeline.fill_pipeline) {
        vkDestroyPipeline(g_Device, g_ParcelGpuPipeline.fill_pipeline, g_Allocator);
        g_ParcelGpuPipeline.fill_pipeline = VK_NULL_HANDLE;
    }
    if (g_ParcelGpuPipeline.line_pipeline) {
        vkDestroyPipeline(g_Device, g_ParcelGpuPipeline.line_pipeline, g_Allocator);
        g_ParcelGpuPipeline.line_pipeline = VK_NULL_HANDLE;
    }
    if (g_ParcelGpuPipeline.pipeline_layout) {
        vkDestroyPipelineLayout(g_Device, g_ParcelGpuPipeline.pipeline_layout, g_Allocator);
        g_ParcelGpuPipeline.pipeline_layout = VK_NULL_HANDLE;
    }

    const std::vector<uint32_t> vert_code = loadSpirvFile(kParcelGpuVertShaderPath);
    const std::vector<uint32_t> frag_code = loadSpirvFile(kParcelGpuFragShaderPath);
    if (vert_code.empty() || frag_code.empty()) {
        if (error) *error = "parcel GPU shader SPIR-V is unavailable";
        return false;
    }

    VkShaderModuleCreateInfo shader_info{};
    shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shader_info.codeSize = vert_code.size() * sizeof(uint32_t);
    shader_info.pCode = vert_code.data();
    VkShaderModule vert_shader = VK_NULL_HANDLE;
    if (vkCreateShaderModule(g_Device, &shader_info, g_Allocator, &vert_shader) != VK_SUCCESS) {
        if (error) *error = "vkCreateShaderModule failed for parcel vertex shader";
        return false;
    }
    shader_info.codeSize = frag_code.size() * sizeof(uint32_t);
    shader_info.pCode = frag_code.data();
    VkShaderModule frag_shader = VK_NULL_HANDLE;
    if (vkCreateShaderModule(g_Device, &shader_info, g_Allocator, &frag_shader) != VK_SUCCESS) {
        vkDestroyShaderModule(g_Device, vert_shader, g_Allocator);
        if (error) *error = "vkCreateShaderModule failed for parcel fragment shader";
        return false;
    }

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(ParcelGpuPushConstants);

    VkPipelineLayoutCreateInfo pipeline_layout_info{};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &g_ParcelGpuPipeline.descriptor_set_layout;
    pipeline_layout_info.pushConstantRangeCount = 1;
    pipeline_layout_info.pPushConstantRanges = &push_range;
    if (vkCreatePipelineLayout(g_Device, &pipeline_layout_info, g_Allocator, &g_ParcelGpuPipeline.pipeline_layout) != VK_SUCCESS) {
        vkDestroyShaderModule(g_Device, vert_shader, g_Allocator);
        vkDestroyShaderModule(g_Device, frag_shader, g_Allocator);
        if (error) *error = "vkCreatePipelineLayout failed for parcel GPU pipeline";
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_shader;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_shader;
    stages[1].pName = "main";

    VkVertexInputBindingDescription bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].stride = sizeof(ImVec2);
    bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    bindings[1].binding = 1;
    bindings[1].stride = sizeof(uint32_t);
    bindings[1].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0].location = 0;
    attrs[0].binding = 0;
    attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[0].offset = 0;
    attrs[1].location = 1;
    attrs[1].binding = 1;
    attrs[1].format = VK_FORMAT_R32_UINT;
    attrs[1].offset = 0;

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 2;
    vertex_input.pVertexBindingDescriptions = bindings;
    vertex_input.vertexAttributeDescriptionCount = 2;
    vertex_input.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.lineWidth = 1.0f;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo msaa{};
    msaa.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    msaa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.blendEnable = VK_TRUE;
    blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT |
        VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT |
        VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;

    const VkDynamicState dynamic_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_LINE_WIDTH
    };
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = (uint32_t)IM_ARRAYSIZE(dynamic_states);
    dynamic.pDynamicStates = dynamic_states;

    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &raster;
    pipeline_info.pMultisampleState = &msaa;
    pipeline_info.pColorBlendState = &blend;
    pipeline_info.pDynamicState = &dynamic;
    pipeline_info.layout = g_ParcelGpuPipeline.pipeline_layout;
    pipeline_info.renderPass = render_pass;
    pipeline_info.subpass = 0;

    VkPipeline fill_pipeline = VK_NULL_HANDLE;
    const VkResult fill_result =
        vkCreateGraphicsPipelines(g_Device, VK_NULL_HANDLE, 1, &pipeline_info, g_Allocator, &fill_pipeline);
    VkPipeline line_pipeline = VK_NULL_HANDLE;
    if (fill_result == VK_SUCCESS) {
        VkPipelineInputAssemblyStateCreateInfo line_assembly = assembly;
        line_assembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        VkPipelineRasterizationStateCreateInfo line_raster = raster;
        line_raster.lineWidth = 1.0f;
        pipeline_info.pInputAssemblyState = &line_assembly;
        pipeline_info.pRasterizationState = &line_raster;
        const VkResult line_result =
            vkCreateGraphicsPipelines(g_Device, VK_NULL_HANDLE, 1, &pipeline_info, g_Allocator, &line_pipeline);
        if (line_result != VK_SUCCESS) {
            vkDestroyPipeline(g_Device, fill_pipeline, g_Allocator);
            fill_pipeline = VK_NULL_HANDLE;
        }
    }
    vkDestroyShaderModule(g_Device, vert_shader, g_Allocator);
    vkDestroyShaderModule(g_Device, frag_shader, g_Allocator);
    if (!fill_pipeline || !line_pipeline) {
        if (error) *error = "vkCreateGraphicsPipelines failed for parcel GPU pipeline";
        vkDestroyPipelineLayout(g_Device, g_ParcelGpuPipeline.pipeline_layout, g_Allocator);
        g_ParcelGpuPipeline.pipeline_layout = VK_NULL_HANDLE;
        return false;
    }
    g_ParcelGpuPipeline.fill_pipeline = fill_pipeline;
    g_ParcelGpuPipeline.line_pipeline = line_pipeline;
    g_ParcelGpuPipeline.render_pass = render_pass;
    return true;
}

bool configureParcelGpuDrawState(const ParcelGpuDrawConfig& config, std::string* error) {
    g_ParcelGpuDrawState = ParcelGpuDrawState{};
    if (!config.active) {
        publishParcelGpuStatusSnapshot();
        return true;
    }
    if (!g_ParcelGpuBuffers.positions.buffer || !g_ParcelGpuBuffers.indices.buffer || g_ParcelGpuBuffers.chunks.empty()) {
        if (error) *error = "parcel GPU buffers are not resident";
        publishParcelGpuStatusSnapshot();
        return false;
    }
    g_ParcelGpuDrawState.active = true;
    g_ParcelGpuDrawState.math_zoom = config.math_zoom;
    g_ParcelGpuDrawState.zoom_scale = config.zoom_scale;
    g_ParcelGpuDrawState.center_lonlat = config.center_lonlat;
    g_ParcelGpuDrawState.center_world = config.center_world;
    g_ParcelGpuDrawState.viewport_origin = config.viewport_origin;
    g_ParcelGpuDrawState.viewport_size = config.viewport_size;
    g_ParcelGpuDrawState.framebuffer_size = config.framebuffer_size;
    g_ParcelGpuDrawState.visible_chunks.reserve(g_ParcelGpuBuffers.chunks.size());
    g_ParcelGpuDrawState.visible_line_chunks.reserve(g_ParcelGpuBuffers.chunks.size());
    const float lon_span = std::max(0.0f, config.view_max_lon - config.view_min_lon);
    const float lat_span = std::max(0.0f, config.view_max_lat - config.view_min_lat);
    const float lon_pad = std::max(
        0.0001f,
        lon_span * (2.0f / std::max(1.0f, config.viewport_size.x)));
    const float lat_pad = std::max(
        0.0001f,
        lat_span * (2.0f / std::max(1.0f, config.viewport_size.y)));
    for (const ParcelRenderChunkRecord& chunk : g_ParcelGpuBuffers.chunks) {
        if (!rectsOverlap(
                chunk.min_lon,
                chunk.min_lat,
                chunk.max_lon,
                chunk.max_lat,
                config.view_min_lon - lon_pad,
                config.view_min_lat - lat_pad,
                config.view_max_lon + lon_pad,
                config.view_max_lat + lat_pad)) {
            continue;
        }
        if (chunk.index_count > 0) {
            g_ParcelGpuDrawState.visible_chunks.push_back(ParcelGpuDrawChunk{
                chunk.index_offset,
                chunk.index_count
            });
        }
        if (chunk.line_index_count > 0) {
            g_ParcelGpuDrawState.visible_line_chunks.push_back(ParcelGpuLineDrawChunk{
                chunk.line_index_offset,
                chunk.line_index_count
            });
        }
    }
    publishParcelGpuStatusSnapshot();
    return true;
}

void clearParcelGpuDrawState() {
    g_ParcelGpuDrawState = ParcelGpuDrawState{};
    publishParcelGpuStatusSnapshot();
}

bool parcelGpuDrawActive() {
    return g_ParcelGpuDrawState.active &&
        !g_ParcelGpuDrawState.visible_chunks.empty() &&
        g_ParcelGpuBuffers.positions.buffer &&
        g_ParcelGpuBuffers.indices.buffer &&
        g_ParcelGpuBuffers.vertex_feature_refs.buffer &&
        g_ParcelGpuBuffers.colors.buffer;
}

static void renderParcelGpuDrawCallback(const ImDrawList*, const ImDrawCmd*) {
    if (!parcelGpuDrawActive() || !g_CurrentFrameRenderCommandBuffer) return;
    std::string pipeline_error;
    if (!ensureParcelGpuPipeline(g_CurrentFrameRenderPass, &pipeline_error)) {
        std::fprintf(stderr, "[worldsim3] Parcel GPU pipeline unavailable: %s\n", pipeline_error.c_str());
        return;
    }
    std::array<VkDescriptorSet, 3> descriptor_sets{};
    if (!ensureParcelGpuDescriptorSet(&pipeline_error, &descriptor_sets)) {
        std::fprintf(stderr, "[worldsim3] Parcel GPU descriptors unavailable: %s\n", pipeline_error.c_str());
        return;
    }

    ParcelGpuPushConstants push{};
    push.center_lonlat[0] = g_ParcelGpuDrawState.center_lonlat.x;
    push.center_lonlat[1] = g_ParcelGpuDrawState.center_lonlat.y;
    push.center_world[0] = g_ParcelGpuDrawState.center_world.x;
    push.center_world[1] = g_ParcelGpuDrawState.center_world.y;
    push.viewport_origin[0] = g_ParcelGpuDrawState.viewport_origin.x;
    push.viewport_origin[1] = g_ParcelGpuDrawState.viewport_origin.y;
    push.viewport_size[0] = g_ParcelGpuDrawState.viewport_size.x;
    push.viewport_size[1] = g_ParcelGpuDrawState.viewport_size.y;
    push.framebuffer_size[0] = std::max(1.0f, g_ParcelGpuDrawState.framebuffer_size.x);
    push.framebuffer_size[1] = std::max(1.0f, g_ParcelGpuDrawState.framebuffer_size.y);
    push.math_zoom = (float)g_ParcelGpuDrawState.math_zoom;
    push.zoom_scale = g_ParcelGpuDrawState.zoom_scale;

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = push.framebuffer_size[0];
    viewport.height = push.framebuffer_size[1];
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(g_CurrentFrameRenderCommandBuffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset.x = std::max(0, (int32_t)std::floor(g_ParcelGpuDrawState.viewport_origin.x));
    scissor.offset.y = std::max(0, (int32_t)std::floor(g_ParcelGpuDrawState.viewport_origin.y));
    const uint32_t max_width = (uint32_t)std::max(0.0f, push.framebuffer_size[0] - (float)scissor.offset.x);
    const uint32_t max_height = (uint32_t)std::max(0.0f, push.framebuffer_size[1] - (float)scissor.offset.y);
    scissor.extent.width = std::min((uint32_t)std::max(0.0f, std::ceil(g_ParcelGpuDrawState.viewport_size.x)), max_width);
    scissor.extent.height = std::min((uint32_t)std::max(0.0f, std::ceil(g_ParcelGpuDrawState.viewport_size.y)), max_height);
    if (scissor.extent.width == 0 || scissor.extent.height == 0) return;
    vkCmdSetScissor(g_CurrentFrameRenderCommandBuffer, 0, 1, &scissor);

    const VkBuffer vertex_buffers[] = {
        g_ParcelGpuBuffers.positions.buffer,
        g_ParcelGpuBuffers.vertex_feature_refs.buffer
    };
    const VkDeviceSize offsets[] = {0, 0};
    vkCmdBindPipeline(g_CurrentFrameRenderCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, g_ParcelGpuPipeline.fill_pipeline);
    vkCmdBindDescriptorSets(
        g_CurrentFrameRenderCommandBuffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        g_ParcelGpuPipeline.pipeline_layout,
        0,
        1,
        &descriptor_sets[0],
        0,
        nullptr);
    vkCmdPushConstants(
        g_CurrentFrameRenderCommandBuffer,
        g_ParcelGpuPipeline.pipeline_layout,
        VK_SHADER_STAGE_VERTEX_BIT,
        0,
        sizeof(push),
        &push);
    vkCmdBindVertexBuffers(g_CurrentFrameRenderCommandBuffer, 0, 2, vertex_buffers, offsets);
    vkCmdBindIndexBuffer(g_CurrentFrameRenderCommandBuffer, g_ParcelGpuBuffers.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
    for (const ParcelGpuDrawChunk& chunk : g_ParcelGpuDrawState.visible_chunks) {
        vkCmdDrawIndexed(g_CurrentFrameRenderCommandBuffer, chunk.index_count, 1, chunk.first_index, 0, 0);
    }
}

void enqueueParcelGpuDraw(ImDrawList* draw_list) {
    if (!draw_list || !parcelGpuDrawActive()) return;
    draw_list->AddCallback(renderParcelGpuDrawCallback, nullptr);
    draw_list->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

bool parcelGpuOverlayDrawActive() {
    if (!parcelGpuDrawActive() || !g_ParcelGpuBuffers.overlay_colors.mapped) return false;
    return g_ParcelGpuOverlayHasVisibleColors;
}

static void renderParcelGpuOverlayDrawCallback(const ImDrawList*, const ImDrawCmd*) {
    if (!parcelGpuOverlayDrawActive() || !g_CurrentFrameRenderCommandBuffer) return;
    std::string pipeline_error;
    if (!ensureParcelGpuPipeline(g_CurrentFrameRenderPass, &pipeline_error)) {
        std::fprintf(stderr, "[worldsim3] Parcel GPU overlay pipeline unavailable: %s\n", pipeline_error.c_str());
        return;
    }
    std::array<VkDescriptorSet, 3> descriptor_sets{};
    if (!ensureParcelGpuDescriptorSet(&pipeline_error, &descriptor_sets)) {
        std::fprintf(stderr, "[worldsim3] Parcel GPU overlay descriptors unavailable: %s\n", pipeline_error.c_str());
        return;
    }

    ParcelGpuPushConstants push{};
    push.center_lonlat[0] = g_ParcelGpuDrawState.center_lonlat.x;
    push.center_lonlat[1] = g_ParcelGpuDrawState.center_lonlat.y;
    push.center_world[0] = g_ParcelGpuDrawState.center_world.x;
    push.center_world[1] = g_ParcelGpuDrawState.center_world.y;
    push.viewport_origin[0] = g_ParcelGpuDrawState.viewport_origin.x;
    push.viewport_origin[1] = g_ParcelGpuDrawState.viewport_origin.y;
    push.viewport_size[0] = g_ParcelGpuDrawState.viewport_size.x;
    push.viewport_size[1] = g_ParcelGpuDrawState.viewport_size.y;
    push.framebuffer_size[0] = std::max(1.0f, g_ParcelGpuDrawState.framebuffer_size.x);
    push.framebuffer_size[1] = std::max(1.0f, g_ParcelGpuDrawState.framebuffer_size.y);
    push.math_zoom = (float)g_ParcelGpuDrawState.math_zoom;
    push.zoom_scale = g_ParcelGpuDrawState.zoom_scale;

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = push.framebuffer_size[0];
    viewport.height = push.framebuffer_size[1];
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(g_CurrentFrameRenderCommandBuffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset.x = std::max(0, (int32_t)std::floor(g_ParcelGpuDrawState.viewport_origin.x));
    scissor.offset.y = std::max(0, (int32_t)std::floor(g_ParcelGpuDrawState.viewport_origin.y));
    const uint32_t max_width = (uint32_t)std::max(0.0f, push.framebuffer_size[0] - (float)scissor.offset.x);
    const uint32_t max_height = (uint32_t)std::max(0.0f, push.framebuffer_size[1] - (float)scissor.offset.y);
    scissor.extent.width = std::min((uint32_t)std::max(0.0f, std::ceil(g_ParcelGpuDrawState.viewport_size.x)), max_width);
    scissor.extent.height = std::min((uint32_t)std::max(0.0f, std::ceil(g_ParcelGpuDrawState.viewport_size.y)), max_height);
    if (scissor.extent.width == 0 || scissor.extent.height == 0) return;
    vkCmdSetScissor(g_CurrentFrameRenderCommandBuffer, 0, 1, &scissor);

    const VkBuffer vertex_buffers[] = {
        g_ParcelGpuBuffers.positions.buffer,
        g_ParcelGpuBuffers.vertex_feature_refs.buffer
    };
    const VkDeviceSize offsets[] = {0, 0};
    vkCmdBindPipeline(g_CurrentFrameRenderCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, g_ParcelGpuPipeline.fill_pipeline);
    vkCmdBindDescriptorSets(
        g_CurrentFrameRenderCommandBuffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        g_ParcelGpuPipeline.pipeline_layout,
        0,
        1,
        &descriptor_sets[1],
        0,
        nullptr);
    vkCmdPushConstants(
        g_CurrentFrameRenderCommandBuffer,
        g_ParcelGpuPipeline.pipeline_layout,
        VK_SHADER_STAGE_VERTEX_BIT,
        0,
        sizeof(push),
        &push);
    vkCmdBindVertexBuffers(g_CurrentFrameRenderCommandBuffer, 0, 2, vertex_buffers, offsets);
    vkCmdBindIndexBuffer(g_CurrentFrameRenderCommandBuffer, g_ParcelGpuBuffers.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
    for (const ParcelGpuDrawChunk& chunk : g_ParcelGpuDrawState.visible_chunks) {
        vkCmdDrawIndexed(g_CurrentFrameRenderCommandBuffer, chunk.index_count, 1, chunk.first_index, 0, 0);
    }
}

void enqueueParcelGpuOverlayDraw(ImDrawList* draw_list) {
    if (!draw_list || !parcelGpuOverlayDrawActive()) return;
    draw_list->AddCallback(renderParcelGpuOverlayDrawCallback, nullptr);
    draw_list->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

bool parcelGpuOutlineDrawActive() {
    if (!g_ParcelGpuDrawState.active ||
        !g_ParcelGpuBuffers.positions.buffer ||
        !g_ParcelGpuBuffers.line_indices.buffer ||
        !g_ParcelGpuBuffers.vertex_feature_refs.buffer ||
        !g_ParcelGpuBuffers.outline_colors.mapped ||
        g_ParcelGpuDrawState.visible_line_chunks.empty()) {
        return false;
    }
    return g_ParcelGpuOutlineHasVisibleColors;
}

static void renderParcelGpuOutlineDrawCallback(const ImDrawList*, const ImDrawCmd*) {
    if (!parcelGpuOutlineDrawActive() || !g_CurrentFrameRenderCommandBuffer) return;
    std::string pipeline_error;
    if (!ensureParcelGpuPipeline(g_CurrentFrameRenderPass, &pipeline_error)) {
        std::fprintf(stderr, "[worldsim3] Parcel GPU outline pipeline unavailable: %s\n", pipeline_error.c_str());
        return;
    }
    std::array<VkDescriptorSet, 3> descriptor_sets{};
    if (!ensureParcelGpuDescriptorSet(&pipeline_error, &descriptor_sets)) {
        std::fprintf(stderr, "[worldsim3] Parcel GPU outline descriptors unavailable: %s\n", pipeline_error.c_str());
        return;
    }

    ParcelGpuPushConstants push{};
    push.center_lonlat[0] = g_ParcelGpuDrawState.center_lonlat.x;
    push.center_lonlat[1] = g_ParcelGpuDrawState.center_lonlat.y;
    push.center_world[0] = g_ParcelGpuDrawState.center_world.x;
    push.center_world[1] = g_ParcelGpuDrawState.center_world.y;
    push.viewport_origin[0] = g_ParcelGpuDrawState.viewport_origin.x;
    push.viewport_origin[1] = g_ParcelGpuDrawState.viewport_origin.y;
    push.viewport_size[0] = g_ParcelGpuDrawState.viewport_size.x;
    push.viewport_size[1] = g_ParcelGpuDrawState.viewport_size.y;
    push.framebuffer_size[0] = std::max(1.0f, g_ParcelGpuDrawState.framebuffer_size.x);
    push.framebuffer_size[1] = std::max(1.0f, g_ParcelGpuDrawState.framebuffer_size.y);
    push.math_zoom = (float)g_ParcelGpuDrawState.math_zoom;
    push.zoom_scale = g_ParcelGpuDrawState.zoom_scale;

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = push.framebuffer_size[0];
    viewport.height = push.framebuffer_size[1];
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(g_CurrentFrameRenderCommandBuffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset.x = std::max(0, (int32_t)std::floor(g_ParcelGpuDrawState.viewport_origin.x));
    scissor.offset.y = std::max(0, (int32_t)std::floor(g_ParcelGpuDrawState.viewport_origin.y));
    const uint32_t max_width = (uint32_t)std::max(0.0f, push.framebuffer_size[0] - (float)scissor.offset.x);
    const uint32_t max_height = (uint32_t)std::max(0.0f, push.framebuffer_size[1] - (float)scissor.offset.y);
    scissor.extent.width = std::min((uint32_t)std::max(0.0f, std::ceil(g_ParcelGpuDrawState.viewport_size.x)), max_width);
    scissor.extent.height = std::min((uint32_t)std::max(0.0f, std::ceil(g_ParcelGpuDrawState.viewport_size.y)), max_height);
    if (scissor.extent.width == 0 || scissor.extent.height == 0) return;
    vkCmdSetScissor(g_CurrentFrameRenderCommandBuffer, 0, 1, &scissor);
    vkCmdSetLineWidth(g_CurrentFrameRenderCommandBuffer, resolvedMapPolygonOutlineThickness());

    const VkBuffer vertex_buffers[] = {
        g_ParcelGpuBuffers.positions.buffer,
        g_ParcelGpuBuffers.vertex_feature_refs.buffer
    };
    const VkDeviceSize offsets[] = {0, 0};
    vkCmdBindPipeline(g_CurrentFrameRenderCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, g_ParcelGpuPipeline.line_pipeline);
    vkCmdBindDescriptorSets(
        g_CurrentFrameRenderCommandBuffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        g_ParcelGpuPipeline.pipeline_layout,
        0,
        1,
        &descriptor_sets[2],
        0,
        nullptr);
    vkCmdPushConstants(
        g_CurrentFrameRenderCommandBuffer,
        g_ParcelGpuPipeline.pipeline_layout,
        VK_SHADER_STAGE_VERTEX_BIT,
        0,
        sizeof(push),
        &push);
    vkCmdBindVertexBuffers(g_CurrentFrameRenderCommandBuffer, 0, 2, vertex_buffers, offsets);
    vkCmdBindIndexBuffer(g_CurrentFrameRenderCommandBuffer, g_ParcelGpuBuffers.line_indices.buffer, 0, VK_INDEX_TYPE_UINT32);
    for (const ParcelGpuLineDrawChunk& chunk : g_ParcelGpuDrawState.visible_line_chunks) {
        vkCmdDrawIndexed(g_CurrentFrameRenderCommandBuffer, chunk.index_count, 1, chunk.first_index, 0, 0);
    }
}

void enqueueParcelGpuOutlineDraw(ImDrawList* draw_list) {
    if (!draw_list || !parcelGpuOutlineDrawActive()) return;
    draw_list->AddCallback(renderParcelGpuOutlineDrawCallback, nullptr);
    draw_list->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

static ZoningGpuLayerState* findZoningGpuLayerState(size_t layer_idx) {
    auto it = g_ZoningGpuLayers.find(layer_idx);
    return it != g_ZoningGpuLayers.end() ? &it->second : nullptr;
}

bool ensureZoningGpuBuffersResident(size_t layer_idx, const ParcelRenderCacheBlob& blob, std::string* error) {
    if (!g_Device || !g_UploadCommandBuffer) {
        if (error) *error = "Vulkan device/upload command buffer is not ready";
        return false;
    }
    if (blob.vertices.empty() || blob.indices.empty() || blob.features.empty() ||
        blob.line_indices.empty() || blob.vertex_feature_refs.size() != blob.vertices.size()) {
        if (error) *error = "zoning render cache blob is incomplete";
        return false;
    }
    ZoningGpuLayerState& layer_state = g_ZoningGpuLayers[layer_idx];
    if (layer_state.buffers.source_signature == blob.source_signature &&
        layer_state.buffers.vertices == blob.vertices.size() &&
        layer_state.buffers.indices_count == blob.indices.size() &&
        layer_state.buffers.line_indices_count == blob.line_indices.size() &&
        layer_state.buffers.render_features == blob.features.size()) {
        return true;
    }
    ParcelGpuUploadPayload payload;
    ParcelGpuUploadContext ctx;
    ctx.command_pool = g_UploadCommandPool;
    ctx.command_buffer = g_UploadCommandBuffer;
    if (!buildParcelGpuUploadPayload(ctx, blob, payload, error)) return false;
    clearZoningGpuBuffers(layer_idx);
    ZoningGpuLayerState& dst = g_ZoningGpuLayers[layer_idx];
    dst.buffers = std::move(payload.buffers);
    dst.outline_has_visible_colors = false;
    dst.descriptors.descriptor_dirty = true;
    dst.outline_compute_descriptor_dirty = true;
    return true;
}

bool updateZoningGpuColorBuffer(size_t layer_idx, const std::vector<ImU32>& colors_rgba, std::string* error) {
    ZoningGpuLayerState* layer_state = findZoningGpuLayerState(layer_idx);
    if (!layer_state || !layer_state->buffers.colors.mapped || layer_state->buffers.render_features == 0) {
        if (error) *error = "zoning GPU color buffer is not resident";
        return false;
    }
    if (colors_rgba.size() != layer_state->buffers.render_features) {
        if (error) *error = "zoning GPU color buffer size mismatch";
        return false;
    }
    std::memcpy(layer_state->buffers.colors.mapped, colors_rgba.data(), colors_rgba.size() * sizeof(ImU32));
    return true;
}

bool updateZoningGpuOutlineColorBuffer(size_t layer_idx, const std::vector<ImU32>& colors_rgba, std::string* error) {
    ZoningGpuLayerState* layer_state = findZoningGpuLayerState(layer_idx);
    if (!layer_state || !layer_state->buffers.outline_colors.mapped || layer_state->buffers.render_features == 0) {
        if (error) *error = "zoning GPU outline color buffer is not resident";
        return false;
    }
    if (colors_rgba.size() != layer_state->buffers.render_features) {
        if (error) *error = "zoning GPU outline color buffer size mismatch";
        return false;
    }
    std::memcpy(layer_state->buffers.outline_colors.mapped, colors_rgba.data(), colors_rgba.size() * sizeof(ImU32));
    layer_state->outline_colors_cpu = colors_rgba;
    layer_state->outline_compute_descriptor_dirty = true;
    layer_state->outline_has_visible_colors = std::any_of(colors_rgba.begin(), colors_rgba.end(), [](ImU32 color) {
        return (color >> 24) != 0;
    });
    return true;
}

void clearZoningGpuBuffers(size_t layer_idx) {
    auto it = g_ZoningGpuLayers.find(layer_idx);
    if (it == g_ZoningGpuLayers.end()) return;
    destroyZoningGpuLayerState(it->second);
    g_ZoningGpuLayers.erase(it);
}

void clearAllZoningGpuBuffers() {
    for (auto& kv : g_ZoningGpuLayers) {
        destroyZoningGpuLayerState(kv.second);
    }
    g_ZoningGpuLayers.clear();
}

static bool ensureZoningGpuDescriptorSetsAllocated(size_t layer_idx, ZoningGpuLayerState& layer_state, std::string* error) {
    if (!g_Device || !g_DescriptorPool || !layer_state.buffers.colors.buffer || !layer_state.buffers.outline_colors.buffer) {
        if (error) *error = "zoning GPU descriptor prerequisites are not ready";
        return false;
    }
    if (!g_ZoningGpuPipeline.descriptor_set_layout) {
        VkDescriptorSetLayoutBinding color_binding{};
        color_binding.binding = 0;
        color_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        color_binding.descriptorCount = 1;
        color_binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        VkDescriptorSetLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.bindingCount = 1;
        layout_info.pBindings = &color_binding;
        if (vkCreateDescriptorSetLayout(g_Device, &layout_info, g_Allocator, &g_ZoningGpuPipeline.descriptor_set_layout) != VK_SUCCESS) {
            if (error) *error = "vkCreateDescriptorSetLayout failed for zoning GPU pipeline";
            return false;
        }
    }
    const uint32_t frame_count = parcelGpuDescriptorFrameCount();
    if (layer_state.descriptors.descriptor_sets_by_frame.size() != frame_count) {
        std::vector<VkDescriptorSetLayout> layouts((size_t)frame_count * 2, g_ZoningGpuPipeline.descriptor_set_layout);
        std::vector<VkDescriptorSet> sets(layouts.size(), VK_NULL_HANDLE);
        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = g_DescriptorPool;
        alloc_info.descriptorSetCount = (uint32_t)layouts.size();
        alloc_info.pSetLayouts = layouts.data();
        if (vkAllocateDescriptorSets(g_Device, &alloc_info, sets.data()) != VK_SUCCESS) {
            if (error) *error = "vkAllocateDescriptorSets failed for zoning GPU pipeline";
            return false;
        }
        layer_state.descriptors.descriptor_sets_by_frame.assign(frame_count, {});
        layer_state.descriptors.descriptor_dirty_by_frame.assign(frame_count, true);
        for (uint32_t frame = 0; frame < frame_count; ++frame) {
            layer_state.descriptors.descriptor_sets_by_frame[frame] = {
                sets[(size_t)frame * 2 + 0],
                VK_NULL_HANDLE,
                sets[(size_t)frame * 2 + 1]
            };
        }
        layer_state.descriptors.descriptor_dirty = true;
    }
    if (layer_state.descriptors.descriptor_dirty) {
        if (layer_state.descriptors.descriptor_dirty_by_frame.size() != frame_count) {
            layer_state.descriptors.descriptor_dirty_by_frame.assign(frame_count, true);
        } else {
            std::fill(layer_state.descriptors.descriptor_dirty_by_frame.begin(), layer_state.descriptors.descriptor_dirty_by_frame.end(), true);
        }
        layer_state.descriptors.descriptor_dirty = false;
    }
    return true;
}

static bool ensureZoningGpuDescriptorSet(size_t layer_idx, ZoningGpuLayerState& layer_state, std::string* error, std::array<VkDescriptorSet, 3>* out_sets) {
    if (!ensureZoningGpuDescriptorSetsAllocated(layer_idx, layer_state, error)) return false;
    if (!out_sets || layer_state.descriptors.descriptor_sets_by_frame.empty()) {
        if (error) *error = "zoning GPU descriptor set output is unavailable";
        return false;
    }
    const uint32_t frame_count = (uint32_t)layer_state.descriptors.descriptor_sets_by_frame.size();
    const uint32_t frame_index = frame_count > 0 ? (g_CurrentFrameRenderIndex % frame_count) : 0;
    *out_sets = layer_state.descriptors.descriptor_sets_by_frame[frame_index];
    if (frame_index >= layer_state.descriptors.descriptor_dirty_by_frame.size() ||
        !layer_state.descriptors.descriptor_dirty_by_frame[frame_index]) {
        return true;
    }
    VkDescriptorBufferInfo base_color_info{};
    base_color_info.buffer = layer_state.buffers.colors.buffer;
    base_color_info.offset = 0;
    base_color_info.range = layer_state.buffers.colors.size_bytes;
    VkDescriptorBufferInfo outline_color_info{};
    outline_color_info.buffer = layer_state.buffers.outline_colors.buffer;
    outline_color_info.offset = 0;
    outline_color_info.range = layer_state.buffers.outline_colors.size_bytes;
    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = (*out_sets)[0];
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &base_color_info;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = (*out_sets)[2];
    writes[1].dstBinding = 0;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &outline_color_info;
    vkUpdateDescriptorSets(g_Device, 2, writes, 0, nullptr);
    layer_state.descriptors.descriptor_dirty_by_frame[frame_index] = false;
    return true;
}

static bool ensureZoningGpuPipeline(VkRenderPass render_pass, std::string* error) {
    if (!g_Device || !render_pass) {
        if (error) *error = "zoning GPU render pass/device is not ready";
        return false;
    }
    if (g_ZoningGpuPipeline.fill_pipeline && g_ZoningGpuPipeline.line_pipeline && g_ZoningGpuPipeline.render_pass == render_pass) return true;
    if (g_ZoningGpuPipeline.fill_pipeline) vkDestroyPipeline(g_Device, g_ZoningGpuPipeline.fill_pipeline, g_Allocator);
    if (g_ZoningGpuPipeline.line_pipeline) vkDestroyPipeline(g_Device, g_ZoningGpuPipeline.line_pipeline, g_Allocator);
    if (g_ZoningGpuPipeline.pipeline_layout) vkDestroyPipelineLayout(g_Device, g_ZoningGpuPipeline.pipeline_layout, g_Allocator);
    g_ZoningGpuPipeline.fill_pipeline = VK_NULL_HANDLE;
    g_ZoningGpuPipeline.line_pipeline = VK_NULL_HANDLE;
    g_ZoningGpuPipeline.pipeline_layout = VK_NULL_HANDLE;

    const std::vector<uint32_t> vert_code = loadSpirvFile(kParcelGpuVertShaderPath);
    const std::vector<uint32_t> frag_code = loadSpirvFile(kParcelGpuFragShaderPath);
    if (vert_code.empty() || frag_code.empty()) {
        if (error) *error = "zoning GPU shader SPIR-V is unavailable";
        return false;
    }
    VkShaderModuleCreateInfo shader_info{};
    shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shader_info.codeSize = vert_code.size() * sizeof(uint32_t);
    shader_info.pCode = vert_code.data();
    VkShaderModule vert_shader = VK_NULL_HANDLE;
    if (vkCreateShaderModule(g_Device, &shader_info, g_Allocator, &vert_shader) != VK_SUCCESS) {
        if (error) *error = "vkCreateShaderModule failed for zoning vertex shader";
        return false;
    }
    shader_info.codeSize = frag_code.size() * sizeof(uint32_t);
    shader_info.pCode = frag_code.data();
    VkShaderModule frag_shader = VK_NULL_HANDLE;
    if (vkCreateShaderModule(g_Device, &shader_info, g_Allocator, &frag_shader) != VK_SUCCESS) {
        vkDestroyShaderModule(g_Device, vert_shader, g_Allocator);
        if (error) *error = "vkCreateShaderModule failed for zoning fragment shader";
        return false;
    }
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(ParcelGpuPushConstants);
    VkPipelineLayoutCreateInfo pipeline_layout_info{};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &g_ZoningGpuPipeline.descriptor_set_layout;
    pipeline_layout_info.pushConstantRangeCount = 1;
    pipeline_layout_info.pPushConstantRanges = &push_range;
    if (vkCreatePipelineLayout(g_Device, &pipeline_layout_info, g_Allocator, &g_ZoningGpuPipeline.pipeline_layout) != VK_SUCCESS) {
        vkDestroyShaderModule(g_Device, vert_shader, g_Allocator);
        vkDestroyShaderModule(g_Device, frag_shader, g_Allocator);
        if (error) *error = "vkCreatePipelineLayout failed for zoning GPU pipeline";
        return false;
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_shader;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_shader;
    stages[1].pName = "main";
    VkVertexInputBindingDescription bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].stride = sizeof(ImVec2);
    bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    bindings[1].binding = 1;
    bindings[1].stride = sizeof(uint32_t);
    bindings[1].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0].location = 0;
    attrs[0].binding = 0;
    attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[0].offset = 0;
    attrs[1].location = 1;
    attrs[1].binding = 1;
    attrs[1].format = VK_FORMAT_R32_UINT;
    attrs[1].offset = 0;
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 2;
    vertex_input.pVertexBindingDescriptions = bindings;
    vertex_input.vertexAttributeDescriptionCount = 2;
    vertex_input.pVertexAttributeDescriptions = attrs;
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.lineWidth = 1.0f;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    VkPipelineMultisampleStateCreateInfo msaa{};
    msaa.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    msaa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.blendEnable = VK_TRUE;
    blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    const VkDynamicState dynamic_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_LINE_WIDTH
    };
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = (uint32_t)IM_ARRAYSIZE(dynamic_states);
    dynamic.pDynamicStates = dynamic_states;
    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &raster;
    pipeline_info.pMultisampleState = &msaa;
    pipeline_info.pColorBlendState = &blend;
    pipeline_info.pDynamicState = &dynamic;
    pipeline_info.layout = g_ZoningGpuPipeline.pipeline_layout;
    pipeline_info.renderPass = render_pass;
    pipeline_info.subpass = 0;
    VkPipeline fill_pipeline = VK_NULL_HANDLE;
    const VkResult fill_result = vkCreateGraphicsPipelines(g_Device, VK_NULL_HANDLE, 1, &pipeline_info, g_Allocator, &fill_pipeline);
    VkPipeline line_pipeline = VK_NULL_HANDLE;
    if (fill_result == VK_SUCCESS) {
        VkPipelineInputAssemblyStateCreateInfo line_assembly = assembly;
        line_assembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        VkPipelineRasterizationStateCreateInfo line_raster = raster;
        line_raster.lineWidth = 1.0f;
        pipeline_info.pInputAssemblyState = &line_assembly;
        pipeline_info.pRasterizationState = &line_raster;
        if (vkCreateGraphicsPipelines(g_Device, VK_NULL_HANDLE, 1, &pipeline_info, g_Allocator, &line_pipeline) != VK_SUCCESS) {
            vkDestroyPipeline(g_Device, fill_pipeline, g_Allocator);
            fill_pipeline = VK_NULL_HANDLE;
        }
    }
    vkDestroyShaderModule(g_Device, vert_shader, g_Allocator);
    vkDestroyShaderModule(g_Device, frag_shader, g_Allocator);
    if (!fill_pipeline || !line_pipeline) {
        if (error) *error = "vkCreateGraphicsPipelines failed for zoning GPU pipeline";
        vkDestroyPipelineLayout(g_Device, g_ZoningGpuPipeline.pipeline_layout, g_Allocator);
        g_ZoningGpuPipeline.pipeline_layout = VK_NULL_HANDLE;
        return false;
    }
    g_ZoningGpuPipeline.fill_pipeline = fill_pipeline;
    g_ZoningGpuPipeline.line_pipeline = line_pipeline;
    g_ZoningGpuPipeline.render_pass = render_pass;
    return true;
}

static bool ensureZoningOutlineIndirectComputePipeline(std::string* error) {
    if (!g_Device) {
        if (error) *error = "Vulkan device is not ready";
        return false;
    }
    if (g_ZoningOutlineIndirectComputePipeline.pipeline) return true;
    const std::vector<uint32_t> comp_code = loadSpirvFile(kZoningOutlineIndirectShaderPath);
    if (comp_code.empty()) {
        if (error) *error = "zoning outline indirect compute shader SPIR-V is unavailable";
        return false;
    }

    VkDescriptorSetLayoutBinding bindings[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 3;
    layout_info.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(g_Device, &layout_info, g_Allocator, &g_ZoningOutlineIndirectComputePipeline.descriptor_set_layout) != VK_SUCCESS) {
        if (error) *error = "vkCreateDescriptorSetLayout failed for zoning outline indirect compute";
        return false;
    }

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(ZoningOutlineIndirectPushConstants);
    VkPipelineLayoutCreateInfo pipeline_layout_info{};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &g_ZoningOutlineIndirectComputePipeline.descriptor_set_layout;
    pipeline_layout_info.pushConstantRangeCount = 1;
    pipeline_layout_info.pPushConstantRanges = &push_range;
    if (vkCreatePipelineLayout(g_Device, &pipeline_layout_info, g_Allocator, &g_ZoningOutlineIndirectComputePipeline.pipeline_layout) != VK_SUCCESS) {
        if (error) *error = "vkCreatePipelineLayout failed for zoning outline indirect compute";
        destroyZoningOutlineIndirectComputePipeline();
        return false;
    }

    VkShaderModuleCreateInfo shader_info{};
    shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shader_info.codeSize = comp_code.size() * sizeof(uint32_t);
    shader_info.pCode = comp_code.data();
    VkShaderModule comp_shader = VK_NULL_HANDLE;
    if (vkCreateShaderModule(g_Device, &shader_info, g_Allocator, &comp_shader) != VK_SUCCESS) {
        if (error) *error = "vkCreateShaderModule failed for zoning outline indirect compute";
        destroyZoningOutlineIndirectComputePipeline();
        return false;
    }
    VkComputePipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_info.stage.module = comp_shader;
    pipeline_info.stage.pName = "main";
    pipeline_info.layout = g_ZoningOutlineIndirectComputePipeline.pipeline_layout;
    const VkResult result = vkCreateComputePipelines(g_Device, VK_NULL_HANDLE, 1, &pipeline_info, g_Allocator, &g_ZoningOutlineIndirectComputePipeline.pipeline);
    vkDestroyShaderModule(g_Device, comp_shader, g_Allocator);
    if (result != VK_SUCCESS) {
        if (error) *error = "vkCreateComputePipelines failed for zoning outline indirect compute";
        destroyZoningOutlineIndirectComputePipeline();
        return false;
    }
    return true;
}

static bool ensureZoningOutlineComputeDescriptorSet(
    ZoningGpuLayerState& layer_state,
    uint32_t frame_slot,
    std::string* error,
    VkDescriptorSet* out_set) {
    if (!g_DescriptorPool ||
        !layer_state.buffers.outline_feature_records.buffer ||
        !layer_state.buffers.outline_colors.buffer ||
        frame_slot >= layer_state.outline_indirect_commands_by_frame.size() ||
        !layer_state.outline_indirect_commands_by_frame[frame_slot].buffer) {
        if (error) *error = "zoning outline indirect compute descriptor prerequisites are unavailable";
        return false;
    }
    const uint32_t frame_count = parcelGpuDescriptorFrameCount();
    if (layer_state.outline_compute_descriptor_sets_by_frame.size() != frame_count) {
        if (!layer_state.outline_compute_descriptor_sets_by_frame.empty()) {
            vkFreeDescriptorSets(
                g_Device,
                g_DescriptorPool,
                (uint32_t)layer_state.outline_compute_descriptor_sets_by_frame.size(),
                layer_state.outline_compute_descriptor_sets_by_frame.data());
        }
        std::vector<VkDescriptorSetLayout> layouts(frame_count, g_ZoningOutlineIndirectComputePipeline.descriptor_set_layout);
        layer_state.outline_compute_descriptor_sets_by_frame.assign(frame_count, VK_NULL_HANDLE);
        VkDescriptorSetAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool = g_DescriptorPool;
        alloc.descriptorSetCount = frame_count;
        alloc.pSetLayouts = layouts.data();
        if (vkAllocateDescriptorSets(g_Device, &alloc, layer_state.outline_compute_descriptor_sets_by_frame.data()) != VK_SUCCESS) {
            layer_state.outline_compute_descriptor_sets_by_frame.clear();
            if (error) *error = "vkAllocateDescriptorSets failed for zoning outline indirect compute";
            return false;
        }
        layer_state.outline_compute_descriptor_dirty_by_frame.assign(frame_count, true);
        layer_state.outline_compute_descriptor_dirty = true;
    }
    if (layer_state.outline_compute_descriptor_dirty) {
        if (layer_state.outline_compute_descriptor_dirty_by_frame.size() != frame_count) {
            layer_state.outline_compute_descriptor_dirty_by_frame.assign(frame_count, true);
        } else {
            std::fill(layer_state.outline_compute_descriptor_dirty_by_frame.begin(), layer_state.outline_compute_descriptor_dirty_by_frame.end(), true);
        }
        layer_state.outline_compute_descriptor_dirty = false;
    }
    if (frame_slot >= layer_state.outline_compute_descriptor_sets_by_frame.size()) {
        if (error) *error = "zoning outline indirect compute frame slot is unavailable";
        return false;
    }
    *out_set = layer_state.outline_compute_descriptor_sets_by_frame[frame_slot];
    if (frame_slot < layer_state.outline_compute_descriptor_dirty_by_frame.size() &&
        !layer_state.outline_compute_descriptor_dirty_by_frame[frame_slot]) {
        return true;
    }

    VkDescriptorBufferInfo feature_info{};
    feature_info.buffer = layer_state.buffers.outline_feature_records.buffer;
    feature_info.offset = 0;
    feature_info.range = layer_state.buffers.outline_feature_records.size_bytes;
    VkDescriptorBufferInfo color_info{};
    color_info.buffer = layer_state.buffers.outline_colors.buffer;
    color_info.offset = 0;
    color_info.range = layer_state.buffers.outline_colors.size_bytes;
    VkDescriptorBufferInfo command_info{};
    command_info.buffer = layer_state.outline_indirect_commands_by_frame[frame_slot].buffer;
    command_info.offset = 0;
    command_info.range = layer_state.outline_indirect_commands_by_frame[frame_slot].size_bytes;
    VkWriteDescriptorSet writes[3]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = *out_set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &feature_info;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = *out_set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &color_info;
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = *out_set;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo = &command_info;
    vkUpdateDescriptorSets(g_Device, 3, writes, 0, nullptr);
    layer_state.outline_compute_descriptor_dirty_by_frame[frame_slot] = false;
    return true;
}

static uint32_t currentParcelGpuFrameSlot() {
    const uint32_t frame_count = parcelGpuDescriptorFrameCount();
    return frame_count > 0 ? (g_CurrentFrameRenderIndex % frame_count) : 0;
}

static bool ensureZoningOutlineIndirectFrameBuffers(ZoningGpuLayerState& layer_state, std::string* error) {
    const uint32_t frame_count = parcelGpuDescriptorFrameCount();
    if (layer_state.outline_indirect_commands_by_frame.size() == frame_count) return true;
    for (ParcelGpuBuffer& buffer : layer_state.outline_indirect_commands_by_frame) {
        destroyParcelGpuBuffer(buffer);
    }
    layer_state.outline_indirect_commands_by_frame.assign(frame_count, {});
    layer_state.outline_indirect_capacity_by_frame.assign(frame_count, 0);
    layer_state.outline_indirect_count_by_frame.assign(frame_count, 0);
    return true;
}

static bool ensureZoningOutlineIndirectCapacity(
    ZoningGpuLayerState& layer_state,
    uint32_t frame_slot,
    uint32_t command_count,
    bool device_local_indirect,
    std::string* error) {
    if (!ensureZoningOutlineIndirectFrameBuffers(layer_state, error)) return false;
    if (frame_slot >= layer_state.outline_indirect_commands_by_frame.size()) {
        if (error) *error = "zoning outline indirect frame slot is unavailable";
        return false;
    }
    if (command_count == 0) {
        layer_state.outline_indirect_count_by_frame[frame_slot] = 0;
        return true;
    }
    if (layer_state.outline_indirect_capacity_by_frame[frame_slot] >= command_count &&
        layer_state.outline_indirect_commands_by_frame[frame_slot].buffer &&
        (device_local_indirect || layer_state.outline_indirect_commands_by_frame[frame_slot].mapped)) {
        return true;
    }
    uint32_t capacity = std::max<uint32_t>(command_count, 256);
    const uint32_t previous = layer_state.outline_indirect_capacity_by_frame[frame_slot];
    if (previous > 0) {
        capacity = std::max<uint32_t>(capacity, previous + previous / 2);
    }
    const VkDeviceSize buffer_size = sizeof(VkDrawIndexedIndirectCommand) * (VkDeviceSize)capacity;
    const bool created = device_local_indirect
        ? createDeviceLocalParcelBuffer(
            buffer_size,
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            layer_state.outline_indirect_commands_by_frame[frame_slot],
            error)
        : createHostVisibleParcelBuffer(
            buffer_size,
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
            layer_state.outline_indirect_commands_by_frame[frame_slot],
            error);
    if (!created) {
        layer_state.outline_indirect_capacity_by_frame[frame_slot] = 0;
        return false;
    }
    layer_state.outline_indirect_capacity_by_frame[frame_slot] = capacity;
    layer_state.outline_compute_descriptor_dirty = true;
    return true;
}

static bool updateZoningOutlineIndirectCommands(
    ZoningGpuLayerState& layer_state,
    const ParcelGpuDrawConfig& config,
    float lon_pad,
    float lat_pad,
    std::string* error) {
    layer_state.outline_indirect_scratch.clear();
    if (g_MultiDrawIndirectEnabled &&
        kZoningOutlineIndirectShaderPath &&
        layer_state.buffers.outline_feature_records.buffer &&
        layer_state.buffers.render_features <= g_MaxDrawIndirectCount) {
        const uint32_t command_count = layer_state.outline_has_visible_colors
            ? layer_state.buffers.render_features
            : 0;
        const uint32_t frame_count = parcelGpuDescriptorFrameCount();
        for (uint32_t frame_slot = 0; frame_slot < frame_count; ++frame_slot) {
            if (!ensureZoningOutlineIndirectCapacity(layer_state, frame_slot, command_count, true, error)) return false;
            if (frame_slot < layer_state.outline_indirect_count_by_frame.size()) {
                layer_state.outline_indirect_count_by_frame[frame_slot] = command_count;
            }
        }
        layer_state.outline_compute_pending = command_count > 0;
        layer_state.outline_compute_view_min_lon = config.view_min_lon - lon_pad;
        layer_state.outline_compute_view_min_lat = config.view_min_lat - lat_pad;
        layer_state.outline_compute_view_max_lon = config.view_max_lon + lon_pad;
        layer_state.outline_compute_view_max_lat = config.view_max_lat + lat_pad;
        return true;
    }

    if (!layer_state.outline_has_visible_colors ||
        layer_state.outline_colors_cpu.size() != layer_state.buffers.features.size()) {
        if (ensureZoningOutlineIndirectFrameBuffers(layer_state, error)) {
            std::fill(
                layer_state.outline_indirect_count_by_frame.begin(),
                layer_state.outline_indirect_count_by_frame.end(),
                0);
        }
        layer_state.outline_compute_pending = false;
        return true;
    }

    for (const ParcelRenderChunkRecord& chunk : layer_state.buffers.chunks) {
        if (!rectsOverlap(chunk.min_lon, chunk.min_lat, chunk.max_lon, chunk.max_lat,
                config.view_min_lon - lon_pad, config.view_min_lat - lat_pad,
                config.view_max_lon + lon_pad, config.view_max_lat + lat_pad)) {
            continue;
        }
        const uint32_t feature_end = std::min<uint32_t>(
            (uint32_t)layer_state.buffers.features.size(),
            chunk.feature_offset + chunk.feature_count);
        for (uint32_t feature_i = chunk.feature_offset; feature_i < feature_end; ++feature_i) {
            const ParcelRenderFeatureRecord& feature = layer_state.buffers.features[feature_i];
            if (feature.line_index_count == 0) continue;
            if ((layer_state.outline_colors_cpu[feature_i] >> 24) == 0) continue;
            if (!rectsOverlap(feature.min_lon, feature.min_lat, feature.max_lon, feature.max_lat,
                    config.view_min_lon - lon_pad, config.view_min_lat - lat_pad,
                    config.view_max_lon + lon_pad, config.view_max_lat + lat_pad)) {
                continue;
            }
            VkDrawIndexedIndirectCommand draw{};
            draw.indexCount = feature.line_index_count;
            draw.instanceCount = 1;
            draw.firstIndex = feature.line_index_offset;
            draw.vertexOffset = 0;
            draw.firstInstance = 0;
            layer_state.outline_indirect_scratch.push_back(draw);
        }
    }

    const uint32_t command_count = (uint32_t)layer_state.outline_indirect_scratch.size();
    const uint32_t frame_count = parcelGpuDescriptorFrameCount();
    for (uint32_t frame_slot = 0; frame_slot < frame_count; ++frame_slot) {
        if (!ensureZoningOutlineIndirectCapacity(layer_state, frame_slot, command_count, false, error)) return false;
        layer_state.outline_indirect_count_by_frame[frame_slot] = command_count;
        if (command_count > 0) {
            std::memcpy(
                layer_state.outline_indirect_commands_by_frame[frame_slot].mapped,
                layer_state.outline_indirect_scratch.data(),
                sizeof(VkDrawIndexedIndirectCommand) * (size_t)command_count);
        }
    }
    return true;
}

void recordZoningOutlineIndirectComputeDispatches(VkCommandBuffer cmd) {
    if (!cmd || !g_MultiDrawIndirectEnabled || !kZoningOutlineIndirectShaderPath || g_ZoningGpuLayers.empty()) return;
    std::string compute_error;
    if (!ensureZoningOutlineIndirectComputePipeline(&compute_error)) {
        const uint32_t frame_slot = currentParcelGpuFrameSlot();
        for (auto& kv : g_ZoningGpuLayers) {
            ZoningGpuLayerState& layer_state = kv.second;
            layer_state.outline_compute_pending = false;
            if (frame_slot < layer_state.outline_indirect_count_by_frame.size()) {
                layer_state.outline_indirect_count_by_frame[frame_slot] = 0;
            }
        }
        return;
    }
    VkMemoryBarrier host_to_compute{};
    host_to_compute.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    host_to_compute.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    host_to_compute.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        1,
        &host_to_compute,
        0,
        nullptr,
        0,
        nullptr);
    bool recorded_dispatch = false;
    for (auto& kv : g_ZoningGpuLayers) {
        ZoningGpuLayerState& layer_state = kv.second;
        if (!layer_state.outline_compute_pending) continue;
        const uint32_t frame_slot = currentParcelGpuFrameSlot();
        if (frame_slot >= layer_state.outline_indirect_count_by_frame.size() ||
            layer_state.outline_indirect_count_by_frame[frame_slot] == 0) {
            layer_state.outline_compute_pending = false;
            continue;
        }
        VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
        if (!ensureZoningOutlineComputeDescriptorSet(layer_state, frame_slot, &compute_error, &descriptor_set)) {
            layer_state.outline_compute_pending = false;
            layer_state.outline_indirect_count_by_frame[frame_slot] = 0;
            continue;
        }
        ZoningOutlineIndirectPushConstants push{};
        push.feature_count = layer_state.buffers.render_features;
        push.view_min_lon = layer_state.outline_compute_view_min_lon;
        push.view_min_lat = layer_state.outline_compute_view_min_lat;
        push.view_max_lon = layer_state.outline_compute_view_max_lon;
        push.view_max_lat = layer_state.outline_compute_view_max_lat;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_ZoningOutlineIndirectComputePipeline.pipeline);
        vkCmdBindDescriptorSets(
            cmd,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            g_ZoningOutlineIndirectComputePipeline.pipeline_layout,
            0,
            1,
            &descriptor_set,
            0,
            nullptr);
        vkCmdPushConstants(
            cmd,
            g_ZoningOutlineIndirectComputePipeline.pipeline_layout,
            VK_SHADER_STAGE_COMPUTE_BIT,
            0,
            sizeof(push),
            &push);
        vkCmdDispatch(cmd, (push.feature_count + 127u) / 128u, 1, 1);
        layer_state.outline_compute_pending = false;
        recorded_dispatch = true;
    }
    if (!recorded_dispatch) return;
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
        0,
        1,
        &barrier,
        0,
        nullptr,
        0,
        nullptr);
}

bool configureZoningGpuDrawState(size_t layer_idx, const ParcelGpuDrawConfig& config, std::string* error) {
    ZoningGpuLayerState* layer_state = findZoningGpuLayerState(layer_idx);
    if (!layer_state) {
        if (!config.active) return true;
        if (error) *error = "zoning GPU buffers are not resident";
        return false;
    }
    layer_state->draw_state = ParcelGpuDrawState{};
    if (!config.active) return true;
    if (!layer_state->buffers.positions.buffer || !layer_state->buffers.indices.buffer || layer_state->buffers.chunks.empty()) {
        if (error) *error = "zoning GPU buffers are not resident";
        return false;
    }
    layer_state->draw_state.active = true;
    layer_state->draw_state.math_zoom = config.math_zoom;
    layer_state->draw_state.zoom_scale = config.zoom_scale;
    layer_state->draw_state.center_lonlat = config.center_lonlat;
    layer_state->draw_state.center_world = config.center_world;
    layer_state->draw_state.viewport_origin = config.viewport_origin;
    layer_state->draw_state.viewport_size = config.viewport_size;
    layer_state->draw_state.framebuffer_size = config.framebuffer_size;
    layer_state->draw_state.visible_chunks.reserve(layer_state->buffers.chunks.size());
    layer_state->draw_state.visible_line_chunks.reserve(layer_state->buffers.chunks.size());
    const float lon_span = std::max(0.0f, config.view_max_lon - config.view_min_lon);
    const float lat_span = std::max(0.0f, config.view_max_lat - config.view_min_lat);
    const float lon_pad = std::max(0.0001f, lon_span * (2.0f / std::max(1.0f, config.viewport_size.x)));
    const float lat_pad = std::max(0.0001f, lat_span * (2.0f / std::max(1.0f, config.viewport_size.y)));
    for (const ParcelRenderChunkRecord& chunk : layer_state->buffers.chunks) {
        if (!rectsOverlap(chunk.min_lon, chunk.min_lat, chunk.max_lon, chunk.max_lat,
                config.view_min_lon - lon_pad, config.view_min_lat - lat_pad,
                config.view_max_lon + lon_pad, config.view_max_lat + lat_pad)) {
            continue;
        }
        if (chunk.index_count > 0) {
            layer_state->draw_state.visible_chunks.push_back(ParcelGpuDrawChunk{chunk.index_offset, chunk.index_count});
        }
        if (chunk.line_index_count > 0) {
            layer_state->draw_state.visible_line_chunks.push_back(ParcelGpuLineDrawChunk{chunk.line_index_offset, chunk.line_index_count});
        }
    }
    if (!updateZoningOutlineIndirectCommands(*layer_state, config, lon_pad, lat_pad, error)) return false;
    return true;
}

void clearZoningGpuDrawState(size_t layer_idx) {
    ZoningGpuLayerState* layer_state = findZoningGpuLayerState(layer_idx);
    if (!layer_state) return;
    layer_state->draw_state = ParcelGpuDrawState{};
}

void clearAllZoningGpuDrawStates() {
    for (auto& kv : g_ZoningGpuLayers) {
        kv.second.draw_state = ParcelGpuDrawState{};
    }
}

bool zoningGpuDrawActive(size_t layer_idx) {
    const ZoningGpuLayerState* layer_state = findZoningGpuLayerState(layer_idx);
    return layer_state &&
        layer_state->draw_state.active &&
        !layer_state->draw_state.visible_chunks.empty() &&
        layer_state->buffers.positions.buffer &&
        layer_state->buffers.indices.buffer &&
        layer_state->buffers.vertex_feature_refs.buffer &&
        layer_state->buffers.colors.buffer;
}

static void renderZoningGpuDrawCallback(const ImDrawList*, const ImDrawCmd* cmd) {
    const size_t layer_idx = (size_t)(uintptr_t)cmd->UserCallbackData;
    ZoningGpuLayerState* layer_state = findZoningGpuLayerState(layer_idx);
    if (!layer_state || !zoningGpuDrawActive(layer_idx) || !g_CurrentFrameRenderCommandBuffer) return;
    std::array<VkDescriptorSet, 3> descriptor_sets{};
    std::string pipeline_error;
    if (!ensureZoningGpuDescriptorSet(layer_idx, *layer_state, &pipeline_error, &descriptor_sets)) return;
    if (!ensureZoningGpuPipeline(g_CurrentFrameRenderPass, &pipeline_error)) return;
    ParcelGpuPushConstants push{};
    push.center_lonlat[0] = layer_state->draw_state.center_lonlat.x;
    push.center_lonlat[1] = layer_state->draw_state.center_lonlat.y;
    push.center_world[0] = layer_state->draw_state.center_world.x;
    push.center_world[1] = layer_state->draw_state.center_world.y;
    push.viewport_origin[0] = layer_state->draw_state.viewport_origin.x;
    push.viewport_origin[1] = layer_state->draw_state.viewport_origin.y;
    push.viewport_size[0] = layer_state->draw_state.viewport_size.x;
    push.viewport_size[1] = layer_state->draw_state.viewport_size.y;
    push.framebuffer_size[0] = std::max(1.0f, layer_state->draw_state.framebuffer_size.x);
    push.framebuffer_size[1] = std::max(1.0f, layer_state->draw_state.framebuffer_size.y);
    push.math_zoom = (float)layer_state->draw_state.math_zoom;
    push.zoom_scale = layer_state->draw_state.zoom_scale;
    VkViewport viewport{0.0f, 0.0f, push.framebuffer_size[0], push.framebuffer_size[1], 0.0f, 1.0f};
    vkCmdSetViewport(g_CurrentFrameRenderCommandBuffer, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.offset.x = std::max(0, (int32_t)std::floor(layer_state->draw_state.viewport_origin.x));
    scissor.offset.y = std::max(0, (int32_t)std::floor(layer_state->draw_state.viewport_origin.y));
    const uint32_t max_width = (uint32_t)std::max(0.0f, push.framebuffer_size[0] - (float)scissor.offset.x);
    const uint32_t max_height = (uint32_t)std::max(0.0f, push.framebuffer_size[1] - (float)scissor.offset.y);
    scissor.extent.width = std::min((uint32_t)std::max(0.0f, std::ceil(layer_state->draw_state.viewport_size.x)), max_width);
    scissor.extent.height = std::min((uint32_t)std::max(0.0f, std::ceil(layer_state->draw_state.viewport_size.y)), max_height);
    if (scissor.extent.width == 0 || scissor.extent.height == 0) return;
    vkCmdSetScissor(g_CurrentFrameRenderCommandBuffer, 0, 1, &scissor);
    const VkBuffer vertex_buffers[] = {layer_state->buffers.positions.buffer, layer_state->buffers.vertex_feature_refs.buffer};
    const VkDeviceSize offsets[] = {0, 0};
    vkCmdBindPipeline(g_CurrentFrameRenderCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, g_ZoningGpuPipeline.fill_pipeline);
    vkCmdBindDescriptorSets(g_CurrentFrameRenderCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, g_ZoningGpuPipeline.pipeline_layout, 0, 1, &descriptor_sets[0], 0, nullptr);
    vkCmdPushConstants(g_CurrentFrameRenderCommandBuffer, g_ZoningGpuPipeline.pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
    vkCmdBindVertexBuffers(g_CurrentFrameRenderCommandBuffer, 0, 2, vertex_buffers, offsets);
    vkCmdBindIndexBuffer(g_CurrentFrameRenderCommandBuffer, layer_state->buffers.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
    for (const ParcelGpuDrawChunk& chunk : layer_state->draw_state.visible_chunks) {
        vkCmdDrawIndexed(g_CurrentFrameRenderCommandBuffer, chunk.index_count, 1, chunk.first_index, 0, 0);
    }
}

void enqueueZoningGpuDraw(ImDrawList* draw_list, size_t layer_idx) {
    if (!draw_list || !zoningGpuDrawActive(layer_idx)) return;
    draw_list->AddCallback(renderZoningGpuDrawCallback, (void*)(uintptr_t)layer_idx);
    draw_list->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

bool zoningGpuOutlineDrawActive(size_t layer_idx) {
    const ZoningGpuLayerState* layer_state = findZoningGpuLayerState(layer_idx);
    if (!layer_state) return false;
    const uint32_t frame_slot = currentParcelGpuFrameSlot();
    const bool use_gpu_compute_indirect =
        g_MultiDrawIndirectEnabled &&
        kZoningOutlineIndirectShaderPath &&
        layer_state->buffers.render_features <= g_MaxDrawIndirectCount;
    const bool indirect_active =
        use_gpu_compute_indirect &&
        frame_slot < layer_state->outline_indirect_count_by_frame.size() &&
        layer_state->outline_indirect_count_by_frame[frame_slot] > 0 &&
        frame_slot < layer_state->outline_indirect_commands_by_frame.size() &&
        layer_state->outline_indirect_commands_by_frame[frame_slot].buffer;
    return layer_state->draw_state.active &&
        layer_state->buffers.positions.buffer &&
        layer_state->buffers.line_indices.buffer &&
        layer_state->buffers.vertex_feature_refs.buffer &&
        layer_state->buffers.outline_colors.mapped &&
        layer_state->outline_has_visible_colors &&
        (indirect_active || (!use_gpu_compute_indirect && !layer_state->draw_state.visible_line_chunks.empty()));
}

static void renderZoningGpuOutlineDrawCallback(const ImDrawList*, const ImDrawCmd* cmd) {
    const size_t layer_idx = (size_t)(uintptr_t)cmd->UserCallbackData;
    ZoningGpuLayerState* layer_state = findZoningGpuLayerState(layer_idx);
    if (!layer_state || !zoningGpuOutlineDrawActive(layer_idx) || !g_CurrentFrameRenderCommandBuffer) return;
    std::array<VkDescriptorSet, 3> descriptor_sets{};
    std::string pipeline_error;
    if (!ensureZoningGpuDescriptorSet(layer_idx, *layer_state, &pipeline_error, &descriptor_sets)) return;
    if (!ensureZoningGpuPipeline(g_CurrentFrameRenderPass, &pipeline_error)) return;
    ParcelGpuPushConstants push{};
    push.center_lonlat[0] = layer_state->draw_state.center_lonlat.x;
    push.center_lonlat[1] = layer_state->draw_state.center_lonlat.y;
    push.center_world[0] = layer_state->draw_state.center_world.x;
    push.center_world[1] = layer_state->draw_state.center_world.y;
    push.viewport_origin[0] = layer_state->draw_state.viewport_origin.x;
    push.viewport_origin[1] = layer_state->draw_state.viewport_origin.y;
    push.viewport_size[0] = layer_state->draw_state.viewport_size.x;
    push.viewport_size[1] = layer_state->draw_state.viewport_size.y;
    push.framebuffer_size[0] = std::max(1.0f, layer_state->draw_state.framebuffer_size.x);
    push.framebuffer_size[1] = std::max(1.0f, layer_state->draw_state.framebuffer_size.y);
    push.math_zoom = (float)layer_state->draw_state.math_zoom;
    push.zoom_scale = layer_state->draw_state.zoom_scale;
    VkViewport viewport{0.0f, 0.0f, push.framebuffer_size[0], push.framebuffer_size[1], 0.0f, 1.0f};
    vkCmdSetViewport(g_CurrentFrameRenderCommandBuffer, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.offset.x = std::max(0, (int32_t)std::floor(layer_state->draw_state.viewport_origin.x));
    scissor.offset.y = std::max(0, (int32_t)std::floor(layer_state->draw_state.viewport_origin.y));
    const uint32_t max_width = (uint32_t)std::max(0.0f, push.framebuffer_size[0] - (float)scissor.offset.x);
    const uint32_t max_height = (uint32_t)std::max(0.0f, push.framebuffer_size[1] - (float)scissor.offset.y);
    scissor.extent.width = std::min((uint32_t)std::max(0.0f, std::ceil(layer_state->draw_state.viewport_size.x)), max_width);
    scissor.extent.height = std::min((uint32_t)std::max(0.0f, std::ceil(layer_state->draw_state.viewport_size.y)), max_height);
    if (scissor.extent.width == 0 || scissor.extent.height == 0) return;
    vkCmdSetScissor(g_CurrentFrameRenderCommandBuffer, 0, 1, &scissor);
    vkCmdSetLineWidth(g_CurrentFrameRenderCommandBuffer, resolvedMapPolygonOutlineThickness());
    const VkBuffer vertex_buffers[] = {layer_state->buffers.positions.buffer, layer_state->buffers.vertex_feature_refs.buffer};
    const VkDeviceSize offsets[] = {0, 0};
    vkCmdBindPipeline(g_CurrentFrameRenderCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, g_ZoningGpuPipeline.line_pipeline);
    vkCmdBindDescriptorSets(g_CurrentFrameRenderCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, g_ZoningGpuPipeline.pipeline_layout, 0, 1, &descriptor_sets[2], 0, nullptr);
    vkCmdPushConstants(g_CurrentFrameRenderCommandBuffer, g_ZoningGpuPipeline.pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
    vkCmdBindVertexBuffers(g_CurrentFrameRenderCommandBuffer, 0, 2, vertex_buffers, offsets);
    vkCmdBindIndexBuffer(g_CurrentFrameRenderCommandBuffer, layer_state->buffers.line_indices.buffer, 0, VK_INDEX_TYPE_UINT32);
    const uint32_t frame_slot = currentParcelGpuFrameSlot();
    const bool use_gpu_compute_indirect =
        g_MultiDrawIndirectEnabled &&
        kZoningOutlineIndirectShaderPath &&
        layer_state->buffers.render_features <= g_MaxDrawIndirectCount;
    if (use_gpu_compute_indirect &&
        frame_slot < layer_state->outline_indirect_count_by_frame.size() &&
        frame_slot < layer_state->outline_indirect_commands_by_frame.size() &&
        layer_state->outline_indirect_count_by_frame[frame_slot] > 0 &&
        layer_state->outline_indirect_commands_by_frame[frame_slot].buffer) {
        vkCmdDrawIndexedIndirect(
            g_CurrentFrameRenderCommandBuffer,
            layer_state->outline_indirect_commands_by_frame[frame_slot].buffer,
            0,
            layer_state->outline_indirect_count_by_frame[frame_slot],
            sizeof(VkDrawIndexedIndirectCommand));
    } else {
        for (const ParcelGpuLineDrawChunk& chunk : layer_state->draw_state.visible_line_chunks) {
            vkCmdDrawIndexed(g_CurrentFrameRenderCommandBuffer, chunk.index_count, 1, chunk.first_index, 0, 0);
        }
    }
}

void enqueueZoningGpuOutlineDraw(ImDrawList* draw_list, size_t layer_idx) {
    if (!draw_list || !zoningGpuOutlineDrawActive(layer_idx)) return;
    draw_list->AddCallback(renderZoningGpuOutlineDrawCallback, (void*)(uintptr_t)layer_idx);
    draw_list->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

void SetupVulkan(const char** extensions, uint32_t extensions_count) {
    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "BaltimoreVulkanMap";
    app_info.apiVersion = VK_API_VERSION_1_1;

    std::vector<const char*> enabled_extensions;
    enabled_extensions.reserve(extensions_count + 1);
    for (uint32_t i = 0; i < extensions_count; ++i) enabled_extensions.push_back(extensions[i]);

    std::vector<const char*> enabled_layers;
    if (g_EnableValidationLayers && IsInstanceLayerAvailable("VK_LAYER_KHRONOS_validation")) {
        enabled_layers.push_back("VK_LAYER_KHRONOS_validation");
        if (IsInstanceExtensionAvailable(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            enabled_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
    } else {
        g_EnableValidationLayers = false;
    }

    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;
    create_info.enabledExtensionCount = (uint32_t)enabled_extensions.size();
    create_info.ppEnabledExtensionNames = enabled_extensions.data();
    create_info.enabledLayerCount = (uint32_t)enabled_layers.size();
    create_info.ppEnabledLayerNames = enabled_layers.empty() ? nullptr : enabled_layers.data();
    check_vk_result(vkCreateInstance(&create_info, g_Allocator, &g_Instance));
    CreateDebugUtilsMessenger();

    uint32_t gpu_count = 0;
    check_vk_result(vkEnumeratePhysicalDevices(g_Instance, &gpu_count, nullptr));
    std::vector<VkPhysicalDevice> gpus(gpu_count);
    check_vk_result(vkEnumeratePhysicalDevices(g_Instance, &gpu_count, gpus.data()));
    g_PhysicalDevice = gpus[0];

    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_PhysicalDevice, &count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(count);
    vkGetPhysicalDeviceQueueFamilyProperties(g_PhysicalDevice, &count, queues.data());
    for (uint32_t i = 0; i < count; i++) {
        if (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            g_QueueFamily = i;
            break;
        }
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = g_QueueFamily;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;

    const char* device_extensions[] = {"VK_KHR_swapchain"};
    VkPhysicalDeviceFeatures available_features{};
    vkGetPhysicalDeviceFeatures(g_PhysicalDevice, &available_features);
    VkPhysicalDeviceFeatures enabled_features{};
    enabled_features.samplerAnisotropy = available_features.samplerAnisotropy;
    enabled_features.multiDrawIndirect = available_features.multiDrawIndirect;
    enabled_features.wideLines = available_features.wideLines;
    g_MultiDrawIndirectEnabled = available_features.multiDrawIndirect == VK_TRUE;
    g_WideLinesEnabled = available_features.wideLines == VK_TRUE;
    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = 1;
    device_info.ppEnabledExtensionNames = device_extensions;
    device_info.pEnabledFeatures = &enabled_features;
    check_vk_result(vkCreateDevice(g_PhysicalDevice, &device_info, g_Allocator, &g_Device));
    vkGetDeviceQueue(g_Device, g_QueueFamily, 0, &g_Queue);

    VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLER, 1024},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4096},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2048},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2048},
    };
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 8192;
    pool_info.poolSizeCount = (uint32_t)IM_ARRAYSIZE(pool_sizes);
    pool_info.pPoolSizes = pool_sizes;
    check_vk_result(vkCreateDescriptorPool(g_Device, &pool_info, g_Allocator, &g_DescriptorPool));

    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler.minLod = 0.0f;
    sampler.maxLod = 16.0f;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(g_PhysicalDevice, &props);
    g_MaxDrawIndirectCount = props.limits.maxDrawIndirectCount;
    g_MinSupportedLineWidth = props.limits.lineWidthRange[0];
    g_MaxSupportedLineWidth = props.limits.lineWidthRange[1];
    sampler.anisotropyEnable = available_features.samplerAnisotropy ? VK_TRUE : VK_FALSE;
    sampler.maxAnisotropy = available_features.samplerAnisotropy ? std::min(8.0f, props.limits.maxSamplerAnisotropy) : 1.0f;
    check_vk_result(vkCreateSampler(g_Device, &sampler, g_Allocator, &g_TileSampler));

    VkCommandPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.queueFamilyIndex = g_QueueFamily;
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    check_vk_result(vkCreateCommandPool(g_Device, &pool, g_Allocator, &g_UploadCommandPool));

    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = g_UploadCommandPool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    check_vk_result(vkAllocateCommandBuffers(g_Device, &alloc, &g_UploadCommandBuffer));
}

void SetupVulkanWindow(ImGui_ImplVulkanH_Window* wd, VkSurfaceKHR surface, int width, int height) {
    wd->Surface = surface;
    VkBool32 res;
    vkGetPhysicalDeviceSurfaceSupportKHR(g_PhysicalDevice, g_QueueFamily, wd->Surface, &res);
    if (res != VK_TRUE) std::abort();
    VkSurfaceCapabilitiesKHR cap{};
    check_vk_result(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_PhysicalDevice, wd->Surface, &cap));
    if (wd == &g_MainWindowData) {
        g_MainSwapchainTransferSrcSupported = (cap.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    }

    const VkFormat request_surface_image_format[] = {
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_B8G8R8_UNORM,
        VK_FORMAT_R8G8B8_UNORM,
    };
    const VkColorSpaceKHR request_surface_color_space = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
    wd->SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(
        g_PhysicalDevice, wd->Surface, request_surface_image_format,
        (size_t)IM_ARRAYSIZE(request_surface_image_format),
        request_surface_color_space);

    const VkPresentModeKHR present_modes[] = {
        VK_PRESENT_MODE_MAILBOX_KHR,
        VK_PRESENT_MODE_IMMEDIATE_KHR,
        VK_PRESENT_MODE_FIFO_KHR,
    };
    wd->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(
        g_PhysicalDevice, wd->Surface, &present_modes[0], IM_ARRAYSIZE(present_modes));

    ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, wd, g_QueueFamily, g_Allocator, width, height, g_MinImageCount);
}

static void CleanupTileCache() {
    if (g_Device != VK_NULL_HANDLE && !g_VulkanDeviceLost.load(std::memory_order_relaxed)) {
        check_vk_result(vkDeviceWaitIdle(g_Device));
    }
    for (auto& kv : g_TileCache) destroyTileTextureNow(kv.second.tex);
    g_TileCache.clear();
    g_TileLRU.clear();
    clearBasemapTileDiskCache();
    drainRetiredTextures(true);
}

void CleanupVulkan() {
    CleanupTileCache();
    shutdownGpuSplatAggregate();
    stopParcelGpuUploadWorker();
    cleanupFeatureOverlayGpuServices();
    clearAllZoningGpuBuffers();
    clearParcelGpuBuffers();
    destroyGpuPickResources();
    drainRetiredParcelGpuPayloads(true);
    destroyZoningGpuPipeline();
    destroyZoningOutlineIndirectComputePipeline();
    destroyParcelGpuPipeline();
    if (g_UploadCommandPool) vkDestroyCommandPool(g_Device, g_UploadCommandPool, g_Allocator);
    if (g_TileSampler) vkDestroySampler(g_Device, g_TileSampler, g_Allocator);
    vkDestroyDescriptorPool(g_Device, g_DescriptorPool, g_Allocator);
    vkDestroyDevice(g_Device, g_Allocator);
    if (g_DebugUtilsMessenger != VK_NULL_HANDLE) {
        auto destroy_fn = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_Instance, "vkDestroyDebugUtilsMessengerEXT");
        if (destroy_fn) destroy_fn(g_Instance, g_DebugUtilsMessenger, g_Allocator);
    }
    vkDestroyInstance(g_Instance, g_Allocator);
}

void CleanupVulkanWindow() {
    ImGui_ImplVulkanH_DestroyWindow(g_Instance, g_Device, &g_MainWindowData, g_Allocator);
}
