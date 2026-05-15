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
#include "earcut.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;
ScreenshotRequestState g_ScreenshotState;


static bool writePpmRgb(
    const fs::path& out_path,
    const uint8_t* pixels,
    uint32_t width,
    uint32_t height,
    size_t row_pitch,
    VkFormat fmt,
    std::string& err) {
    std::ofstream out(out_path, std::ios::binary);
    if (!out) {
        err = "failed to open screenshot output";
        return false;
    }
    out << "P6\n" << width << " " << height << "\n255\n";
    const bool bgra = (fmt == VK_FORMAT_B8G8R8A8_UNORM || fmt == VK_FORMAT_B8G8R8A8_SRGB);
    const bool rgba = (fmt == VK_FORMAT_R8G8B8A8_UNORM || fmt == VK_FORMAT_R8G8B8A8_SRGB);
    if (!bgra && !rgba) {
        err = "unsupported swapchain format for screenshot";
        return false;
    }
    std::vector<uint8_t> line;
    line.resize((size_t)width * 3);
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* src = pixels + (size_t)y * row_pitch;
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* px = src + (size_t)x * 4;
            uint8_t r = rgba ? px[0] : px[2];
            uint8_t g = px[1];
            uint8_t b = rgba ? px[2] : px[0];
            size_t i = (size_t)x * 3;
            line[i + 0] = r;
            line[i + 1] = g;
            line[i + 2] = b;
        }
        out.write(reinterpret_cast<const char*>(line.data()), (std::streamsize)line.size());
    }
    if (!out.good()) {
        err = "failed writing screenshot file";
        return false;
    }
    return true;
}

static bool writePpmRgbResized(
    const fs::path& out_path,
    const uint8_t* pixels,
    uint32_t src_width,
    uint32_t src_height,
    size_t row_pitch,
    VkFormat fmt,
    uint32_t out_width,
    uint32_t out_height,
    std::string& err) {
    if (out_width == src_width && out_height == src_height) {
        return writePpmRgb(out_path, pixels, src_width, src_height, row_pitch, fmt, err);
    }

    std::ofstream out(out_path, std::ios::binary);
    if (!out) {
        err = "failed to open screenshot output";
        return false;
    }
    const bool bgra = (fmt == VK_FORMAT_B8G8R8A8_UNORM || fmt == VK_FORMAT_B8G8R8A8_SRGB);
    const bool rgba = (fmt == VK_FORMAT_R8G8B8A8_UNORM || fmt == VK_FORMAT_R8G8B8A8_SRGB);
    if (!bgra && !rgba) {
        err = "unsupported swapchain format for screenshot";
        return false;
    }

    out << "P6\n" << out_width << " " << out_height << "\n255\n";
    std::vector<uint8_t> line((size_t)out_width * 3);
    for (uint32_t y = 0; y < out_height; ++y) {
        const uint32_t sy0 = (uint32_t)(((uint64_t)y * src_height) / out_height);
        uint32_t sy1 = (uint32_t)(((uint64_t)(y + 1) * src_height + out_height - 1) / out_height);
        sy1 = std::max(sy0 + 1, std::min(sy1, src_height));
        for (uint32_t x = 0; x < out_width; ++x) {
            const uint32_t sx0 = (uint32_t)(((uint64_t)x * src_width) / out_width);
            uint32_t sx1 = (uint32_t)(((uint64_t)(x + 1) * src_width + out_width - 1) / out_width);
            sx1 = std::max(sx0 + 1, std::min(sx1, src_width));

            uint64_t r_sum = 0;
            uint64_t g_sum = 0;
            uint64_t b_sum = 0;
            uint64_t count = 0;
            for (uint32_t sy = sy0; sy < sy1; ++sy) {
                const uint8_t* row = pixels + (size_t)sy * row_pitch;
                for (uint32_t sx = sx0; sx < sx1; ++sx) {
                    const uint8_t* px = row + (size_t)sx * 4;
                    r_sum += rgba ? px[0] : px[2];
                    g_sum += px[1];
                    b_sum += rgba ? px[2] : px[0];
                    count++;
                }
            }
            const size_t i = (size_t)x * 3;
            line[i + 0] = (uint8_t)(r_sum / count);
            line[i + 1] = (uint8_t)(g_sum / count);
            line[i + 2] = (uint8_t)(b_sum / count);
        }
        out.write(reinterpret_cast<const char*>(line.data()), (std::streamsize)line.size());
    }
    if (!out.good()) {
        err = "failed writing screenshot file";
        return false;
    }
    return true;
}


VkAllocationCallbacks* g_Allocator = nullptr;
VkInstance g_Instance = VK_NULL_HANDLE;
VkPhysicalDevice g_PhysicalDevice = VK_NULL_HANDLE;
VkDevice g_Device = VK_NULL_HANDLE;
uint32_t g_QueueFamily = (uint32_t)-1;
VkQueue g_Queue = VK_NULL_HANDLE;
std::mutex g_QueueSubmitMutex;
VkDescriptorPool g_DescriptorPool = VK_NULL_HANDLE;
VkSampler g_TileSampler = VK_NULL_HANDLE;
static VkCommandPool g_UploadCommandPool = VK_NULL_HANDLE;
static VkCommandBuffer g_UploadCommandBuffer = VK_NULL_HANDLE;

struct ParcelGpuBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size_bytes = 0;
};

struct ParcelGpuBuffers {
    ParcelGpuBuffer positions;
    ParcelGpuBuffer indices;
    ParcelGpuBuffer line_indices;
    ParcelGpuBuffer vertex_feature_refs;
    ParcelGpuBuffer colors;
    ParcelGpuBuffer overlay_colors;
    ParcelGpuBuffer outline_colors;
    std::vector<ParcelRenderChunkRecord> chunks;
    uint32_t render_features = 0;
    uint32_t vertices = 0;
    uint32_t indices_count = 0;
    uint32_t line_indices_count = 0;
    std::string source_signature;
};

struct ParcelGpuDrawChunk {
    uint32_t first_index = 0;
    uint32_t index_count = 0;
};

struct ParcelGpuLineDrawChunk {
    uint32_t first_index = 0;
    uint32_t index_count = 0;
};

struct ParcelGpuDrawState {
    bool active = false;
    int math_zoom = 0;
    float zoom_scale = 1.0f;
    ImVec2 center_world = ImVec2(0.0f, 0.0f);
    ImVec2 viewport_origin = ImVec2(0.0f, 0.0f);
    ImVec2 viewport_size = ImVec2(0.0f, 0.0f);
    ImVec2 framebuffer_size = ImVec2(0.0f, 0.0f);
    std::vector<ParcelGpuDrawChunk> visible_chunks;
    std::vector<ParcelGpuLineDrawChunk> visible_line_chunks;
};

struct ParcelGpuPipeline {
    VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
    VkDescriptorSet base_descriptor_set = VK_NULL_HANDLE;
    VkDescriptorSet overlay_descriptor_set = VK_NULL_HANDLE;
    VkDescriptorSet outline_descriptor_set = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline fill_pipeline = VK_NULL_HANDLE;
    VkPipeline line_pipeline = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    bool descriptor_dirty = true;
};

struct ParcelGpuUploadContext {
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
};

struct ParcelGpuUploadPayload {
    ParcelGpuBuffers buffers;
};

struct RetiredParcelGpuPayload {
    ParcelGpuBuffers buffers;
    uint64_t retire_after_frame = 0;
};

struct ParcelGpuUploadResult {
    bool ok = false;
    std::string source_signature;
    std::string error;
    ParcelGpuUploadPayload payload;
};

static ParcelGpuBuffers g_ParcelGpuBuffers;
static ParcelGpuDrawState g_ParcelGpuDrawState;
static ParcelGpuPipeline g_ParcelGpuPipeline;
static VkCommandBuffer g_CurrentFrameRenderCommandBuffer = VK_NULL_HANDLE;
static VkRenderPass g_CurrentFrameRenderPass = VK_NULL_HANDLE;
static std::atomic<bool> g_ParcelGpuUploadStop{false};
static std::mutex g_ParcelGpuUploadRequestMutex;
static std::condition_variable g_ParcelGpuUploadCv;
static std::optional<ParcelRenderCacheBlob> g_ParcelGpuUploadPendingRequest;
static std::mutex g_ParcelGpuUploadResultMutex;
static std::optional<ParcelGpuUploadResult> g_ParcelGpuUploadCompletedResult;
static std::thread g_ParcelGpuUploadWorker;
static std::vector<RetiredParcelGpuPayload> g_RetiredParcelGpuPayloads;
static std::atomic<uint64_t> g_PresentedFrameSerial{0};
static VkDebugUtilsMessengerEXT g_DebugUtilsMessenger = VK_NULL_HANDLE;

static void submitUploadCommands(std::function<void(VkCommandBuffer)> record);

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

struct ParcelGpuPushConstants {
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
static bool g_MainSwapchainTransferSrcSupported = false;

std::unordered_map<std::string, TileCacheEntry> g_TileCache;
std::list<std::string> g_TileLRU;
bool g_EnableValidationLayers = false;
std::vector<TileTexture> g_RetiredTextures;
static int g_TextureRetireFrames = 0;

struct TopoVectorCache {
    fs::file_time_type mtime{};
    bool loaded = false;
    std::vector<std::vector<ImVec2>> lines_lonlat;
};
static TopoVectorCache g_TopoVectorCache;

void check_vk_result(VkResult err) {
    if (err == 0) return;
    std::fprintf(stderr, "[vulkan] VkResult=%d\n", err);
    if (err < 0) std::abort();
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

static uint32_t findMemoryType(uint32_t type_filter, VkMemoryPropertyFlags properties) {
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

static void destroyParcelGpuBuffer(ParcelGpuBuffer& b) {
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

static void destroyParcelGpuBuffers(ParcelGpuBuffers& buffers) {
    destroyParcelGpuBuffer(buffers.positions);
    destroyParcelGpuBuffer(buffers.indices);
    destroyParcelGpuBuffer(buffers.line_indices);
    destroyParcelGpuBuffer(buffers.vertex_feature_refs);
    destroyParcelGpuBuffer(buffers.colors);
    destroyParcelGpuBuffer(buffers.overlay_colors);
    destroyParcelGpuBuffer(buffers.outline_colors);
    buffers.chunks.clear();
    buffers.render_features = 0;
    buffers.vertices = 0;
    buffers.indices_count = 0;
    buffers.line_indices_count = 0;
    buffers.source_signature.clear();
}

static void retireParcelGpuBuffers(ParcelGpuBuffers&& buffers) {
    if (!buffers.positions.buffer &&
        !buffers.indices.buffer &&
        !buffers.line_indices.buffer &&
        !buffers.vertex_feature_refs.buffer &&
        !buffers.colors.buffer &&
        !buffers.overlay_colors.buffer &&
        !buffers.outline_colors.buffer) {
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
    auto it = g_RetiredParcelGpuPayloads.begin();
    while (it != g_RetiredParcelGpuPayloads.end()) {
        if (!force && presented_frame < it->retire_after_frame) {
            ++it;
            continue;
        }
        destroyParcelGpuBuffers(it->buffers);
        it = g_RetiredParcelGpuPayloads.erase(it);
    }
}

void drainRetiredParcelGpuResources() {
    drainRetiredParcelGpuPayloads(false);
}

static bool createHostVisibleParcelBuffer(
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    ParcelGpuBuffer& out,
    std::string* error) {
    destroyParcelGpuBuffer(out);
    createBuffer(
        size,
        usage,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        out.buffer,
        out.memory);
    if (!out.buffer || !out.memory) {
        if (error) *error = "failed to create host-visible parcel buffer";
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

static bool createParcelGpuUploadContext(ParcelGpuUploadContext& out, std::string* error) {
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

static void destroyParcelGpuUploadContext(ParcelGpuUploadContext& ctx) {
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

static bool uploadDeviceLocalParcelBuffer(
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
    createBuffer(
        size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        staging,
        staging_mem);
    if (!staging || !staging_mem) {
        if (error) *error = "failed to create staging buffer";
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

    createBuffer(
        size,
        usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        out.buffer,
        out.memory);
    if (!out.buffer || !out.memory) {
        if (error) *error = "failed to create device-local parcel buffer";
        vkDestroyBuffer(g_Device, staging, g_Allocator);
        vkFreeMemory(g_Device, staging_mem, g_Allocator);
        return false;
    }

    submitUploadCommands([&](VkCommandBuffer cmd) {
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
    createBuffer(
        size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        staging,
        staging_mem);
    if (!staging || !staging_mem) {
        if (error) *error = "failed to create staging buffer";
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

    createBuffer(
        size,
        usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        out.buffer,
        out.memory);
    if (!out.buffer || !out.memory) {
        if (error) *error = "failed to create device-local parcel buffer";
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

void clearParcelGpuBuffers() {
    destroyParcelGpuBuffers(g_ParcelGpuBuffers);
    drainRetiredParcelGpuPayloads(true);
    g_ParcelGpuPipeline.descriptor_dirty = true;
    clearParcelGpuDrawState();
}

static bool buildParcelGpuUploadPayload(
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
    if (!uploadDeviceLocalParcelBuffer(ctx, blob.vertices.data(), positions_size,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, out.buffers.positions, error) ||
        !uploadDeviceLocalParcelBuffer(ctx, blob.indices.data(), indices_size,
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, out.buffers.indices, error) ||
        !uploadDeviceLocalParcelBuffer(ctx, blob.line_indices.data(), line_indices_size,
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, out.buffers.line_indices, error) ||
        !uploadDeviceLocalParcelBuffer(ctx, blob.vertex_feature_refs.data(), refs_size,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, out.buffers.vertex_feature_refs, error) ||
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
    out.buffers.chunks = blob.chunks;
    out.buffers.source_signature = blob.source_signature;
    return true;
}

bool ensureParcelGpuBuffersResident(const ParcelRenderCacheBlob& blob, std::string* error) {
    if (!g_Device || !g_UploadCommandBuffer) {
        if (error) *error = "Vulkan device/upload command buffer is not ready";
        return false;
    }
    if (blob.vertices.empty() || blob.indices.empty() || blob.features.empty() ||
        blob.line_indices.empty() ||
        blob.vertex_feature_refs.size() != blob.vertices.size()) {
        if (error) *error = "parcel render cache blob is incomplete";
        return false;
    }
    if (g_ParcelGpuBuffers.source_signature == blob.source_signature &&
        g_ParcelGpuBuffers.vertices == blob.vertices.size() &&
        g_ParcelGpuBuffers.indices_count == blob.indices.size() &&
        g_ParcelGpuBuffers.line_indices_count == blob.line_indices.size() &&
        g_ParcelGpuBuffers.render_features == blob.features.size()) {
        return true;
    }

    ParcelGpuUploadPayload payload;
    payload.buffers = ParcelGpuBuffers{};
    ParcelGpuUploadContext ctx;
    ctx.command_pool = g_UploadCommandPool;
    ctx.command_buffer = g_UploadCommandBuffer;
    g_ParcelGpuPipeline.descriptor_dirty = true;
    if (!buildParcelGpuUploadPayload(ctx, blob, payload, error)) {
        return false;
    }
    clearParcelGpuBuffers();
    g_ParcelGpuBuffers = std::move(payload.buffers);
    g_ParcelGpuPipeline.descriptor_dirty = true;
    return true;
}

bool updateParcelGpuColorBuffer(const std::vector<ImU32>& colors_rgba, std::string* error) {
    if (!g_ParcelGpuBuffers.colors.mapped || g_ParcelGpuBuffers.render_features == 0) {
        if (error) *error = "parcel GPU color buffer is not resident";
        return false;
    }
    if (colors_rgba.size() != g_ParcelGpuBuffers.render_features) {
        if (error) *error = "parcel GPU color buffer size mismatch";
        return false;
    }
    std::memcpy(
        g_ParcelGpuBuffers.colors.mapped,
        colors_rgba.data(),
        colors_rgba.size() * sizeof(ImU32));
    return true;
}

bool updateParcelGpuOverlayColorBuffer(const std::vector<ImU32>& colors_rgba, std::string* error) {
    if (!g_ParcelGpuBuffers.overlay_colors.mapped || g_ParcelGpuBuffers.render_features == 0) {
        if (error) *error = "parcel GPU overlay color buffer is not resident";
        return false;
    }
    if (colors_rgba.size() != g_ParcelGpuBuffers.render_features) {
        if (error) *error = "parcel GPU overlay color buffer size mismatch";
        return false;
    }
    std::memcpy(
        g_ParcelGpuBuffers.overlay_colors.mapped,
        colors_rgba.data(),
        colors_rgba.size() * sizeof(ImU32));
    return true;
}

bool updateParcelGpuOutlineColorBuffer(const std::vector<ImU32>& colors_rgba, std::string* error) {
    if (!g_ParcelGpuBuffers.outline_colors.mapped || g_ParcelGpuBuffers.render_features == 0) {
        if (error) *error = "parcel GPU outline color buffer is not resident";
        return false;
    }
    if (colors_rgba.size() != g_ParcelGpuBuffers.render_features) {
        if (error) *error = "parcel GPU outline color buffer size mismatch";
        return false;
    }
    std::memcpy(
        g_ParcelGpuBuffers.outline_colors.mapped,
        colors_rgba.data(),
        colors_rgba.size() * sizeof(ImU32));
    return true;
}

ParcelGpuResidencyStatus getParcelGpuResidencyStatus() {
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
    out.colors = g_ParcelGpuBuffers.render_features;
    out.source_signature = g_ParcelGpuBuffers.source_signature;
    return out;
}

static void adoptParcelGpuUploadPayload(ParcelGpuUploadPayload&& payload) {
    retireParcelGpuBuffers(std::move(g_ParcelGpuBuffers));
    g_ParcelGpuBuffers = ParcelGpuBuffers{};
    g_ParcelGpuBuffers = std::move(payload.buffers);
    g_ParcelGpuPipeline.descriptor_dirty = true;
}

bool startParcelGpuUploadWorker(std::string* error) {
    if (g_ParcelGpuUploadWorker.joinable()) return true;
    g_ParcelGpuUploadStop.store(false, std::memory_order_relaxed);
    g_ParcelGpuUploadPendingRequest.reset();
    g_ParcelGpuUploadCompletedResult.reset();

    std::string ctx_error;
    ParcelGpuUploadContext startup_ctx;
    if (!createParcelGpuUploadContext(startup_ctx, &ctx_error)) {
        if (error) *error = ctx_error;
        return false;
    }

    g_ParcelGpuUploadWorker = std::thread([ctx = std::move(startup_ctx)]() mutable {
        while (!g_ParcelGpuUploadStop.load(std::memory_order_relaxed)) {
            ParcelRenderCacheBlob request_blob;
            {
                std::unique_lock<std::mutex> lk(g_ParcelGpuUploadRequestMutex);
                g_ParcelGpuUploadCv.wait(lk, [] {
                    return g_ParcelGpuUploadStop.load(std::memory_order_relaxed) || g_ParcelGpuUploadPendingRequest.has_value();
                });
                if (g_ParcelGpuUploadStop.load(std::memory_order_relaxed)) break;
                request_blob = std::move(*g_ParcelGpuUploadPendingRequest);
                g_ParcelGpuUploadPendingRequest.reset();
            }

            ParcelGpuUploadResult result;
            result.source_signature = request_blob.source_signature;
            result.ok = buildParcelGpuUploadPayload(ctx, request_blob, result.payload, &result.error);
            {
                std::lock_guard<std::mutex> lk(g_ParcelGpuUploadResultMutex);
                if (g_ParcelGpuUploadCompletedResult && g_ParcelGpuUploadCompletedResult->ok) {
                    destroyParcelGpuBuffers(g_ParcelGpuUploadCompletedResult->payload.buffers);
                }
                g_ParcelGpuUploadCompletedResult = std::move(result);
            }
        }
        destroyParcelGpuUploadContext(ctx);
    });
    return true;
}

void stopParcelGpuUploadWorker() {
    g_ParcelGpuUploadStop.store(true, std::memory_order_relaxed);
    g_ParcelGpuUploadCv.notify_all();
    if (g_ParcelGpuUploadWorker.joinable()) g_ParcelGpuUploadWorker.join();
    if (g_ParcelGpuUploadCompletedResult && g_ParcelGpuUploadCompletedResult->ok) {
        destroyParcelGpuBuffers(g_ParcelGpuUploadCompletedResult->payload.buffers);
    }
    g_ParcelGpuUploadCompletedResult.reset();
    g_ParcelGpuUploadPendingRequest.reset();
}

bool requestParcelGpuUpload(const ParcelRenderCacheBlob& blob, std::string* error) {
    if (!g_ParcelGpuUploadWorker.joinable()) {
        if (error) *error = "parcel GPU upload worker is not running";
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(g_ParcelGpuUploadRequestMutex);
        g_ParcelGpuUploadPendingRequest = blob;
    }
    g_ParcelGpuUploadCv.notify_one();
    return true;
}

bool drainParcelGpuUploadResults(const std::string* expected_signature, std::string* adopted_signature, std::string* error) {
    std::optional<ParcelGpuUploadResult> result;
    {
        std::lock_guard<std::mutex> lk(g_ParcelGpuUploadResultMutex);
        if (!g_ParcelGpuUploadCompletedResult.has_value()) return false;
        result = std::move(g_ParcelGpuUploadCompletedResult);
        g_ParcelGpuUploadCompletedResult.reset();
    }
    if (!result->ok) {
        if (error) *error = result->error;
        return false;
    }
    if (adopted_signature) *adopted_signature = result->source_signature;
    if (expected_signature && result->source_signature != *expected_signature) {
        destroyParcelGpuBuffers(result->payload.buffers);
        if (error) *error = "stale parcel GPU upload result discarded";
        return false;
    }
    adoptParcelGpuUploadPayload(std::move(result->payload));
    return true;
}

static std::vector<uint32_t> loadSpirvFile(const char* path) {
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
    return !(a_max_x < b_min_x || a_min_x > b_max_x || a_max_y < b_min_y || a_min_y > b_max_y);
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
    g_ParcelGpuPipeline.base_descriptor_set = VK_NULL_HANDLE;
    g_ParcelGpuPipeline.overlay_descriptor_set = VK_NULL_HANDLE;
    g_ParcelGpuPipeline.outline_descriptor_set = VK_NULL_HANDLE;
    g_ParcelGpuPipeline.render_pass = VK_NULL_HANDLE;
    g_ParcelGpuPipeline.descriptor_dirty = true;
}

static bool ensureParcelGpuDescriptorSet(std::string* error) {
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
    if (!g_ParcelGpuPipeline.base_descriptor_set || !g_ParcelGpuPipeline.overlay_descriptor_set || !g_ParcelGpuPipeline.outline_descriptor_set) {
        VkDescriptorSetLayout layouts[3] = {
            g_ParcelGpuPipeline.descriptor_set_layout,
            g_ParcelGpuPipeline.descriptor_set_layout,
            g_ParcelGpuPipeline.descriptor_set_layout
        };
        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = g_DescriptorPool;
        alloc_info.descriptorSetCount = 3;
        alloc_info.pSetLayouts = layouts;
        VkDescriptorSet sets[3]{};
        if (vkAllocateDescriptorSets(g_Device, &alloc_info, sets) != VK_SUCCESS) {
            if (error) *error = "vkAllocateDescriptorSets failed for parcel GPU pipeline";
            return false;
        }
        g_ParcelGpuPipeline.base_descriptor_set = sets[0];
        g_ParcelGpuPipeline.overlay_descriptor_set = sets[1];
        g_ParcelGpuPipeline.outline_descriptor_set = sets[2];
        g_ParcelGpuPipeline.descriptor_dirty = true;
    }
    if (!g_ParcelGpuPipeline.descriptor_dirty) return true;

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
    writes[0].dstSet = g_ParcelGpuPipeline.base_descriptor_set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &base_color_info;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = g_ParcelGpuPipeline.overlay_descriptor_set;
    writes[1].dstBinding = 0;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &overlay_color_info;
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = g_ParcelGpuPipeline.outline_descriptor_set;
    writes[2].dstBinding = 0;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo = &outline_color_info;
    vkUpdateDescriptorSets(g_Device, 3, writes, 0, nullptr);
    g_ParcelGpuPipeline.descriptor_dirty = false;
    return true;
}

static bool ensureParcelGpuPipeline(VkRenderPass render_pass, std::string* error) {
    if (!g_Device || !render_pass) {
        if (error) *error = "parcel GPU render pass/device is not ready";
        return false;
    }
    if (!ensureParcelGpuDescriptorSet(error)) return false;
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

    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
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
    if (!config.active) return true;
    if (!g_ParcelGpuBuffers.positions.buffer || !g_ParcelGpuBuffers.indices.buffer || g_ParcelGpuBuffers.chunks.empty()) {
        if (error) *error = "parcel GPU buffers are not resident";
        return false;
    }
    g_ParcelGpuDrawState.active = true;
    g_ParcelGpuDrawState.math_zoom = config.math_zoom;
    g_ParcelGpuDrawState.zoom_scale = config.zoom_scale;
    g_ParcelGpuDrawState.center_world = config.center_world;
    g_ParcelGpuDrawState.viewport_origin = config.viewport_origin;
    g_ParcelGpuDrawState.viewport_size = config.viewport_size;
    g_ParcelGpuDrawState.framebuffer_size = config.framebuffer_size;
    g_ParcelGpuDrawState.visible_chunks.reserve(g_ParcelGpuBuffers.chunks.size());
    g_ParcelGpuDrawState.visible_line_chunks.reserve(g_ParcelGpuBuffers.chunks.size());
    for (const ParcelRenderChunkRecord& chunk : g_ParcelGpuBuffers.chunks) {
        if (!rectsOverlap(
                chunk.min_lon,
                chunk.min_lat,
                chunk.max_lon,
                chunk.max_lat,
                config.view_min_lon,
                config.view_min_lat,
                config.view_max_lon,
                config.view_max_lat)) {
            continue;
        }
        if (chunk.index_count == 0) continue;
        g_ParcelGpuDrawState.visible_chunks.push_back(ParcelGpuDrawChunk{
            chunk.index_offset,
            chunk.index_count
        });
        if (chunk.line_index_count > 0) {
            g_ParcelGpuDrawState.visible_line_chunks.push_back(ParcelGpuLineDrawChunk{
                chunk.line_index_offset,
                chunk.line_index_count
            });
        }
    }
    return true;
}

void clearParcelGpuDrawState() {
    g_ParcelGpuDrawState = ParcelGpuDrawState{};
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

    ParcelGpuPushConstants push{};
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
        &g_ParcelGpuPipeline.base_descriptor_set,
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
    const ImU32* colors = static_cast<const ImU32*>(g_ParcelGpuBuffers.overlay_colors.mapped);
    for (uint32_t i = 0; i < g_ParcelGpuBuffers.render_features; ++i) {
        if ((colors[i] >> 24) != 0) return true;
    }
    return false;
}

static void renderParcelGpuOverlayDrawCallback(const ImDrawList*, const ImDrawCmd*) {
    if (!parcelGpuOverlayDrawActive() || !g_CurrentFrameRenderCommandBuffer) return;
    std::string pipeline_error;
    if (!ensureParcelGpuPipeline(g_CurrentFrameRenderPass, &pipeline_error)) {
        std::fprintf(stderr, "[worldsim3] Parcel GPU overlay pipeline unavailable: %s\n", pipeline_error.c_str());
        return;
    }

    ParcelGpuPushConstants push{};
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
        &g_ParcelGpuPipeline.overlay_descriptor_set,
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
    if (!parcelGpuDrawActive() || !g_ParcelGpuBuffers.outline_colors.mapped || g_ParcelGpuDrawState.visible_line_chunks.empty()) return false;
    const ImU32* colors = static_cast<const ImU32*>(g_ParcelGpuBuffers.outline_colors.mapped);
    for (uint32_t i = 0; i < g_ParcelGpuBuffers.render_features; ++i) {
        if ((colors[i] >> 24) != 0) return true;
    }
    return false;
}

static void renderParcelGpuOutlineDrawCallback(const ImDrawList*, const ImDrawCmd*) {
    if (!parcelGpuOutlineDrawActive() || !g_CurrentFrameRenderCommandBuffer) return;
    std::string pipeline_error;
    if (!ensureParcelGpuPipeline(g_CurrentFrameRenderPass, &pipeline_error)) {
        std::fprintf(stderr, "[worldsim3] Parcel GPU outline pipeline unavailable: %s\n", pipeline_error.c_str());
        return;
    }

    ParcelGpuPushConstants push{};
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
    vkCmdBindPipeline(g_CurrentFrameRenderCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, g_ParcelGpuPipeline.line_pipeline);
    vkCmdBindDescriptorSets(
        g_CurrentFrameRenderCommandBuffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        g_ParcelGpuPipeline.pipeline_layout,
        0,
        1,
        &g_ParcelGpuPipeline.outline_descriptor_set,
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

static uint32_t calcMipLevels(uint32_t w, uint32_t h) {
    uint32_t levels = 1;
    while (w > 1 || h > 1) {
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
        levels++;
    }
    return levels;
}

static void createImage(uint32_t width, uint32_t height, uint32_t mip_levels, VkImage& image, VkDeviceMemory& memory) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.extent = {width, height, 1};
    info.mipLevels = mip_levels;
    info.arrayLayers = 1;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check_vk_result(vkCreateImage(g_Device, &info, g_Allocator, &image));

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g_Device, image, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check_vk_result(vkAllocateMemory(g_Device, &alloc, g_Allocator, &memory));
    check_vk_result(vkBindImageMemory(g_Device, image, memory, 0));
}

static VkImageView createImageView(VkImage image, uint32_t mip_levels) {
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    info.subresourceRange.levelCount = mip_levels;
    info.subresourceRange.layerCount = 1;
    VkImageView out = VK_NULL_HANDLE;
    check_vk_result(vkCreateImageView(g_Device, &info, g_Allocator, &out));
    return out;
}

static void transitionImage(VkCommandBuffer cmd, VkImage image, uint32_t base_level, uint32_t level_count, VkImageLayout old_layout, VkImageLayout new_layout) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = base_level;
    barrier.subresourceRange.levelCount = level_count;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags src_stage;
    VkPipelineStageFlags dst_stage;
    if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED && new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

static void submitUploadCommands(std::function<void(VkCommandBuffer)> record) {
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

void destroyTileTextureNow(TileTexture& tex) {
    if (tex.descriptor) ImGui_ImplVulkan_RemoveTexture(tex.descriptor);
    if (tex.view) vkDestroyImageView(g_Device, tex.view, g_Allocator);
    if (tex.image) vkDestroyImage(g_Device, tex.image, g_Allocator);
    if (tex.memory) vkFreeMemory(g_Device, tex.memory, g_Allocator);
    tex = {};
}

void destroyTileTexture(TileTexture& tex) {
    if (!tex.descriptor && !tex.view && !tex.image && !tex.memory) return;
    g_RetiredTextures.push_back(tex);
    g_TextureRetireFrames = 8;
    tex = {};
}

void drainRetiredTextures(bool force) {
    if (force) {
        if (g_Device != VK_NULL_HANDLE) check_vk_result(vkDeviceWaitIdle(g_Device));
        for (auto& tex : g_RetiredTextures) destroyTileTextureNow(tex);
        g_RetiredTextures.clear();
        g_TextureRetireFrames = 0;
        return;
    }
    if (g_RetiredTextures.empty()) return;
    if (g_TextureRetireFrames > 0) {
        g_TextureRetireFrames--;
        return;
    }
    for (auto& tex : g_RetiredTextures) destroyTileTextureNow(tex);
    g_RetiredTextures.clear();
}

bool uploadRgbaTexture(const unsigned char* pixels, uint32_t w, uint32_t h, TileTexture& tex) {
    if (!pixels || w == 0 || h == 0) return false;
    destroyTileTexture(tex);

    const VkDeviceSize size = (VkDeviceSize)w * (VkDeviceSize)h * 4;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging, staging_mem);

    void* mapped = nullptr;
    check_vk_result(vkMapMemory(g_Device, staging_mem, 0, size, 0, &mapped));
    std::memcpy(mapped, pixels, (size_t)size);
    vkUnmapMemory(g_Device, staging_mem);

    tex.mip_levels = 1;
    createImage(w, h, tex.mip_levels, tex.image, tex.memory);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {w, h, 1};

    submitUploadCommands([&](VkCommandBuffer cmd) {
        transitionImage(cmd, tex.image, 0, tex.mip_levels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyBufferToImage(cmd, staging, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        transitionImage(cmd, tex.image, 0, tex.mip_levels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });

    vkDestroyBuffer(g_Device, staging, g_Allocator);
    vkFreeMemory(g_Device, staging_mem, g_Allocator);

    tex.view = createImageView(tex.image, tex.mip_levels);
    tex.descriptor = ImGui_ImplVulkan_AddTexture(g_TileSampler, tex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return tex.descriptor != VK_NULL_HANDLE;
}

static void touchLRU(const std::string& key) {
    auto it = g_TileCache.find(key);
    if (it == g_TileCache.end()) return;
    g_TileLRU.erase(it->second.lru_it);
    g_TileLRU.push_front(key);
    it->second.lru_it = g_TileLRU.begin();
}

static void evictIfNeeded() {
    while (g_TileCache.size() > kMaxTileCache) {
        const std::string key = g_TileLRU.back();
        g_TileLRU.pop_back();
        auto it = g_TileCache.find(key);
        if (it != g_TileCache.end()) {
            destroyTileTexture(it->second.tex);
            g_TileCache.erase(it);
        }
    }
}

static bool loadTileTexture(const fs::path& tile_path, const std::string& key) {
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load(tile_path.string().c_str(), &w, &h, &channels, STBI_rgb_alpha);
    if (!pixels) return false;

    const uint32_t mip_levels = calcMipLevels((uint32_t)w, (uint32_t)h);
    std::vector<std::vector<unsigned char>> mip_data;
    mip_data.reserve(mip_levels);
    mip_data.emplace_back((size_t)w * (size_t)h * 4);
    std::memcpy(mip_data[0].data(), pixels, mip_data[0].size());
    stbi_image_free(pixels);

    uint32_t mw = (uint32_t)w;
    uint32_t mh = (uint32_t)h;
    for (uint32_t m = 1; m < mip_levels; ++m) {
        uint32_t nw = std::max(1u, mw / 2);
        uint32_t nh = std::max(1u, mh / 2);
        std::vector<unsigned char> out((size_t)nw * (size_t)nh * 4);
        for (uint32_t y = 0; y < nh; ++y) {
            for (uint32_t x = 0; x < nw; ++x) {
                int acc[4] = {0, 0, 0, 0};
                for (uint32_t ky = 0; ky < 2; ++ky) {
                    for (uint32_t kx = 0; kx < 2; ++kx) {
                        uint32_t sx = std::min(mw - 1, x * 2 + kx);
                        uint32_t sy = std::min(mh - 1, y * 2 + ky);
                        size_t si = ((size_t)sy * mw + sx) * 4;
                        acc[0] += mip_data[m - 1][si + 0];
                        acc[1] += mip_data[m - 1][si + 1];
                        acc[2] += mip_data[m - 1][si + 2];
                        acc[3] += mip_data[m - 1][si + 3];
                    }
                }
                size_t di = ((size_t)y * nw + x) * 4;
                out[di + 0] = (unsigned char)(acc[0] / 4);
                out[di + 1] = (unsigned char)(acc[1] / 4);
                out[di + 2] = (unsigned char)(acc[2] / 4);
                out[di + 3] = (unsigned char)(acc[3] / 4);
            }
        }
        mip_data.emplace_back(std::move(out));
        mw = nw;
        mh = nh;
    }

    VkDeviceSize size = 0;
    for (const auto& m : mip_data) size += (VkDeviceSize)m.size();
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging, staging_mem);

    void* mapped = nullptr;
    check_vk_result(vkMapMemory(g_Device, staging_mem, 0, size, 0, &mapped));
    unsigned char* dst = static_cast<unsigned char*>(mapped);
    std::vector<VkBufferImageCopy> regions;
    regions.reserve(mip_levels);
    VkDeviceSize offset = 0;
    mw = (uint32_t)w;
    mh = (uint32_t)h;
    for (uint32_t m = 0; m < mip_levels; ++m) {
        std::memcpy(dst + offset, mip_data[m].data(), mip_data[m].size());
        VkBufferImageCopy region{};
        region.bufferOffset = offset;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = m;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {mw, mh, 1};
        regions.push_back(region);
        offset += (VkDeviceSize)mip_data[m].size();
        mw = std::max(1u, mw / 2);
        mh = std::max(1u, mh / 2);
    }
    vkUnmapMemory(g_Device, staging_mem);

    TileTexture tex;
    tex.mip_levels = mip_levels;
    createImage((uint32_t)w, (uint32_t)h, mip_levels, tex.image, tex.memory);

    submitUploadCommands([&](VkCommandBuffer cmd) {
        transitionImage(cmd, tex.image, 0, mip_levels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyBufferToImage(cmd, staging, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)regions.size(), regions.data());
        transitionImage(cmd, tex.image, 0, mip_levels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });

    vkDestroyBuffer(g_Device, staging, g_Allocator);
    vkFreeMemory(g_Device, staging_mem, g_Allocator);

    tex.view = createImageView(tex.image, mip_levels);
    tex.descriptor = ImGui_ImplVulkan_AddTexture(g_TileSampler, tex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    g_TileLRU.push_front(key);
    g_TileCache.emplace(key, TileCacheEntry{tex, g_TileLRU.begin()});
    evictIfNeeded();
    return true;
}

static TileTexture* getTileTexture(const fs::path& root, const std::string& tile_root_dir, int z, int x, int y) {
    const std::string key = tile_root_dir + ":" + std::to_string(z) + "/" + std::to_string(x) + "/" + std::to_string(y);
    auto it = g_TileCache.find(key);
    if (it != g_TileCache.end()) {
        touchLRU(key);
        return &it->second.tex;
    }

    if (!basemapTileExistsCached(root, tile_root_dir, z, x, y)) return nullptr;
    const fs::path tile_path = basemapTilePath(root, tile_root_dir, z, x, y);
    if (!loadTileTexture(tile_path, key)) {
        markBasemapTileMissing(tile_root_dir, z, x, y);
        return nullptr;
    }

    auto loaded = g_TileCache.find(key);
    return loaded == g_TileCache.end() ? nullptr : &loaded->second.tex;
}

TileSample getTileSample(
    const fs::path& root,
    const std::string& tile_root_dir,
    int z,
    int x,
    int y,
    int max_native_tile_zoom) {
    const int max_fetch_zoom = std::min(z, std::clamp(max_native_tile_zoom, kMinZoom, kMaxInternalMathZoom));
    for (int pz = max_fetch_zoom; pz >= kMinZoom; --pz) {
        const int dz = z - pz;
        if (dz < 0) continue;
        if (dz > 30) continue;
        const int scale = 1 << dz;
        const int parent_x = x / scale;
        const int parent_y = y / scale;
        const int ox = x % scale;
        const int oy = y % scale;
        TileTexture* parent = getTileTexture(root, tile_root_dir, pz, parent_x, parent_y);
        if (!parent) continue;

        const float step = 1.0f / (float)scale;
        ImVec2 uv0((float)ox * step, (float)oy * step);
        ImVec2 uv1(uv0.x + step, uv0.y + step);
        return TileSample{parent, uv0, uv1};
    }
    return {};
}

const std::vector<std::vector<ImVec2>>& getTopoVectorLines(const fs::path& root) {
    const fs::path p = root / "data" / "tiles_topo_vector.geojson";
    std::error_code ec;
    if (!fs::exists(p, ec) || ec) {
        g_TopoVectorCache.lines_lonlat.clear();
        g_TopoVectorCache.loaded = false;
        return g_TopoVectorCache.lines_lonlat;
    }
    const fs::file_time_type mt = fs::last_write_time(p, ec);
    if (!ec && g_TopoVectorCache.loaded && mt == g_TopoVectorCache.mtime) {
        return g_TopoVectorCache.lines_lonlat;
    }

    g_TopoVectorCache.lines_lonlat.clear();
    std::ifstream in(p);
    if (!in) {
        g_TopoVectorCache.loaded = false;
        return g_TopoVectorCache.lines_lonlat;
    }
    json j;
    try {
        in >> j;
    } catch (...) {
        g_TopoVectorCache.loaded = false;
        return g_TopoVectorCache.lines_lonlat;
    }
    if (!j.is_object() || !j.contains("features") || !j["features"].is_array()) {
        g_TopoVectorCache.loaded = false;
        return g_TopoVectorCache.lines_lonlat;
    }

    auto append_line = [&](const json& coords) {
        if (!coords.is_array()) return;
        std::vector<ImVec2> line;
        line.reserve(coords.size());
        for (const auto& pt : coords) {
            if (!pt.is_array() || pt.size() < 2 || !pt[0].is_number() || !pt[1].is_number()) continue;
            line.emplace_back((float)pt[0].get<double>(), (float)pt[1].get<double>());
        }
        if (line.size() >= 2) g_TopoVectorCache.lines_lonlat.push_back(std::move(line));
    };
    for (const auto& f : j["features"]) {
        if (!f.is_object() || !f.contains("geometry") || !f["geometry"].is_object()) continue;
        const auto& geom = f["geometry"];
        const std::string type = geom.value("type", "");
        if (!geom.contains("coordinates")) continue;
        const auto& c = geom["coordinates"];
        if (type == "LineString") {
            append_line(c);
        } else if (type == "MultiLineString" && c.is_array()) {
            for (const auto& line : c) append_line(line);
        } else if (type == "Polygon" && c.is_array()) {
            for (const auto& ring : c) append_line(ring);
        } else if (type == "MultiPolygon" && c.is_array()) {
            for (const auto& poly : c) {
                if (!poly.is_array()) continue;
                for (const auto& ring : poly) append_line(ring);
            }
        }
    }
    g_TopoVectorCache.mtime = mt;
    g_TopoVectorCache.loaded = true;
    return g_TopoVectorCache.lines_lonlat;
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
    if (g_Device != VK_NULL_HANDLE) check_vk_result(vkDeviceWaitIdle(g_Device));
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
    clearParcelGpuBuffers();
    drainRetiredParcelGpuPayloads(true);
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

void FrameRender(ImGui_ImplVulkanH_Window* wd, ImDrawData* draw_data) {
    VkSemaphore image_acquired_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    VkResult err = vkAcquireNextImageKHR(g_Device, wd->Swapchain, UINT64_MAX, image_acquired_semaphore, VK_NULL_HANDLE, &wd->FrameIndex);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
        g_SwapChainRebuild = true;
        return;
    }
    check_vk_result(err);

    ImGui_ImplVulkanH_Frame* fd = &wd->Frames[wd->FrameIndex];
    check_vk_result(vkWaitForFences(g_Device, 1, &fd->Fence, VK_TRUE, UINT64_MAX));
    check_vk_result(vkResetFences(g_Device, 1, &fd->Fence));
    check_vk_result(vkResetCommandPool(g_Device, fd->CommandPool, 0));

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk_result(vkBeginCommandBuffer(fd->CommandBuffer, &begin));

    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = wd->RenderPass;
    rp.framebuffer = fd->Framebuffer;
    rp.renderArea.extent.width = wd->Width;
    rp.renderArea.extent.height = wd->Height;
    rp.clearValueCount = 1;
    rp.pClearValues = &wd->ClearValue;
    vkCmdBeginRenderPass(fd->CommandBuffer, &rp, VK_SUBPASS_CONTENTS_INLINE);

    g_CurrentFrameRenderCommandBuffer = fd->CommandBuffer;
    g_CurrentFrameRenderPass = wd->RenderPass;
    ImGui_ImplVulkan_RenderDrawData(draw_data, fd->CommandBuffer);
    g_CurrentFrameRenderCommandBuffer = VK_NULL_HANDLE;
    g_CurrentFrameRenderPass = VK_NULL_HANDLE;
    vkCmdEndRenderPass(fd->CommandBuffer);
    check_vk_result(vkEndCommandBuffer(fd->CommandBuffer));

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &image_acquired_semaphore;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &fd->CommandBuffer;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &render_complete_semaphore;
    {
        std::lock_guard<std::mutex> qlk(g_QueueSubmitMutex);
        check_vk_result(vkQueueSubmit(g_Queue, 1, &submit, fd->Fence));
    }

    uint64_t shot_req_id = 0;
    bool shot_request_native = false;
    {
        std::lock_guard<std::mutex> lk(g_ScreenshotState.mutex);
        if (g_ScreenshotState.pending) {
            shot_req_id = g_ScreenshotState.req_id;
            shot_request_native = g_ScreenshotState.request_native;
        }
    }
    if (shot_req_id == 0) return;

    auto complete_screenshot = [&](
        bool ok,
        const std::string& path,
        const std::string& error,
        uint32_t native_width = 0,
        uint32_t native_height = 0,
        uint32_t logical_width = 0,
        uint32_t logical_height = 0,
        uint32_t output_width = 0,
        uint32_t output_height = 0,
        float framebuffer_scale_x = 1.0f,
        float framebuffer_scale_y = 1.0f) {
        std::lock_guard<std::mutex> lk(g_ScreenshotState.mutex);
        if (g_ScreenshotState.pending && g_ScreenshotState.req_id == shot_req_id) {
            g_ScreenshotState.pending = false;
            g_ScreenshotState.done_id = shot_req_id;
            g_ScreenshotState.ok = ok;
            g_ScreenshotState.path = path;
            g_ScreenshotState.error = error;
            g_ScreenshotState.native_width = native_width;
            g_ScreenshotState.native_height = native_height;
            g_ScreenshotState.logical_width = logical_width;
            g_ScreenshotState.logical_height = logical_height;
            g_ScreenshotState.output_width = output_width;
            g_ScreenshotState.output_height = output_height;
            g_ScreenshotState.framebuffer_scale_x = framebuffer_scale_x;
            g_ScreenshotState.framebuffer_scale_y = framebuffer_scale_y;
            g_ScreenshotState.cv.notify_all();
        }
    };

    check_vk_result(vkWaitForFences(g_Device, 1, &fd->Fence, VK_TRUE, UINT64_MAX));

    VkImage src_image = fd->Backbuffer;
    if (src_image == VK_NULL_HANDLE || wd->Width == 0 || wd->Height == 0) {
        complete_screenshot(false, "", "invalid backbuffer");
        return;
    }
    if (!g_MainSwapchainTransferSrcSupported) {
        complete_screenshot(false, "", "surface does not support swapchain transfer-source screenshots");
        return;
    }

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    const uint32_t width = wd->Width;
    const uint32_t height = wd->Height;
    const ImVec2 framebuffer_scale = draw_data ? draw_data->FramebufferScale : ImVec2(1.0f, 1.0f);
    const float scale_x = framebuffer_scale.x > 0.0f ? framebuffer_scale.x : 1.0f;
    const float scale_y = framebuffer_scale.y > 0.0f ? framebuffer_scale.y : 1.0f;
    const uint32_t logical_width = std::max(1u, (uint32_t)std::lround((float)width / scale_x));
    const uint32_t logical_height = std::max(1u, (uint32_t)std::lround((float)height / scale_y));
    const uint32_t output_width = shot_request_native ? width : std::min(width, logical_width);
    const uint32_t output_height = shot_request_native ? height : std::min(height, logical_height);
    const VkDeviceSize image_size = (VkDeviceSize)width * (VkDeviceSize)height * 4;
    createBuffer(
        image_size,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        staging,
        staging_mem);

    VkCommandPool cap_pool = VK_NULL_HANDLE;
    VkCommandBuffer cap_cmd = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = g_QueueFamily;
    check_vk_result(vkCreateCommandPool(g_Device, &pool_info, g_Allocator, &cap_pool));
    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = cap_pool;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;
    check_vk_result(vkAllocateCommandBuffers(g_Device, &alloc_info, &cap_cmd));

    VkCommandBufferBeginInfo begin2{};
    begin2.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin2.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk_result(vkBeginCommandBuffer(cap_cmd, &begin2));

    VkImageMemoryBarrier to_src{};
    to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_src.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_src.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.image = src_image;
    to_src.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_src.subresourceRange.levelCount = 1;
    to_src.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(
        cap_cmd,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &to_src);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cap_cmd, src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &region);

    VkImageMemoryBarrier back_to_present{};
    back_to_present.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    back_to_present.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back_to_present.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    back_to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back_to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    back_to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    back_to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    back_to_present.image = src_image;
    back_to_present.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    back_to_present.subresourceRange.levelCount = 1;
    back_to_present.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(
        cap_cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &back_to_present);
    check_vk_result(vkEndCommandBuffer(cap_cmd));

    VkSubmitInfo cap_submit{};
    cap_submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    cap_submit.commandBufferCount = 1;
    cap_submit.pCommandBuffers = &cap_cmd;
    {
        std::lock_guard<std::mutex> qlk(g_QueueSubmitMutex);
        check_vk_result(vkQueueSubmit(g_Queue, 1, &cap_submit, VK_NULL_HANDLE));
    }
    check_vk_result(vkQueueWaitIdle(g_Queue));

    std::string out_path;
    std::string capture_err;
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(g_Device, staging, &req);
    void* mapped = nullptr;
    check_vk_result(vkMapMemory(g_Device, staging_mem, 0, req.size, 0, &mapped));
    const fs::path shot_dir = fs::current_path() / "data" / "cache" / "screenshots";
    std::error_code ec;
    fs::create_directories(shot_dir, ec);
    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count();
    fs::path out_file = shot_dir / ("shot_" + std::to_string(ts) + ".ppm");
    const bool ok = writePpmRgbResized(
        out_file,
        static_cast<const uint8_t*>(mapped),
        width,
        height,
        (size_t)width * 4,
        wd->SurfaceFormat.format,
        output_width,
        output_height,
        capture_err);
    vkUnmapMemory(g_Device, staging_mem);
    if (ok) out_path = out_file.string();

    vkFreeCommandBuffers(g_Device, cap_pool, 1, &cap_cmd);
    vkDestroyCommandPool(g_Device, cap_pool, g_Allocator);
    vkDestroyBuffer(g_Device, staging, g_Allocator);
    vkFreeMemory(g_Device, staging_mem, g_Allocator);

    complete_screenshot(
        ok,
        out_path,
        capture_err,
        width,
        height,
        logical_width,
        logical_height,
        output_width,
        output_height,
        framebuffer_scale.x,
        framebuffer_scale.y);
}

void FramePresent(ImGui_ImplVulkanH_Window* wd) {
    if (g_SwapChainRebuild) return;
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    VkPresentInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &render_complete_semaphore;
    info.swapchainCount = 1;
    info.pSwapchains = &wd->Swapchain;
    info.pImageIndices = &wd->FrameIndex;
    VkResult err = VK_SUCCESS;
    {
        std::lock_guard<std::mutex> qlk(g_QueueSubmitMutex);
        err = vkQueuePresentKHR(g_Queue, &info);
    }
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
        g_SwapChainRebuild = true;
        return;
    }
    check_vk_result(err);
    wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->SemaphoreCount;
    g_PresentedFrameSerial.fetch_add(1, std::memory_order_relaxed);
}

void FrameRenderSecondary(ImGui_ImplVulkanH_Window* wd, ImDrawData* draw_data, bool& swapchain_rebuild) {
    VkSemaphore image_acquired_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    VkResult err = vkAcquireNextImageKHR(g_Device, wd->Swapchain, UINT64_MAX, image_acquired_semaphore, VK_NULL_HANDLE, &wd->FrameIndex);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
        swapchain_rebuild = true;
        return;
    }
    check_vk_result(err);

    ImGui_ImplVulkanH_Frame* fd = &wd->Frames[wd->FrameIndex];
    check_vk_result(vkWaitForFences(g_Device, 1, &fd->Fence, VK_TRUE, UINT64_MAX));
    check_vk_result(vkResetFences(g_Device, 1, &fd->Fence));
    check_vk_result(vkResetCommandPool(g_Device, fd->CommandPool, 0));

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk_result(vkBeginCommandBuffer(fd->CommandBuffer, &begin));

    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = wd->RenderPass;
    rp.framebuffer = fd->Framebuffer;
    rp.renderArea.extent.width = wd->Width;
    rp.renderArea.extent.height = wd->Height;
    rp.clearValueCount = 1;
    rp.pClearValues = &wd->ClearValue;
    vkCmdBeginRenderPass(fd->CommandBuffer, &rp, VK_SUBPASS_CONTENTS_INLINE);

    ImGui_ImplVulkan_RenderDrawData(draw_data, fd->CommandBuffer);
    vkCmdEndRenderPass(fd->CommandBuffer);
    check_vk_result(vkEndCommandBuffer(fd->CommandBuffer));

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &image_acquired_semaphore;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &fd->CommandBuffer;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &render_complete_semaphore;
    {
        std::lock_guard<std::mutex> qlk(g_QueueSubmitMutex);
        check_vk_result(vkQueueSubmit(g_Queue, 1, &submit, fd->Fence));
    }
}

void FramePresentSecondary(ImGui_ImplVulkanH_Window* wd, bool& swapchain_rebuild) {
    if (swapchain_rebuild) return;
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    VkPresentInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &render_complete_semaphore;
    info.swapchainCount = 1;
    info.pSwapchains = &wd->Swapchain;
    info.pImageIndices = &wd->FrameIndex;
    VkResult err = VK_SUCCESS;
    {
        std::lock_guard<std::mutex> qlk(g_QueueSubmitMutex);
        err = vkQueuePresentKHR(g_Queue, &info);
    }
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
        swapchain_rebuild = true;
        return;
    }
    check_vk_result(err);
    wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->SemaphoreCount;
}
