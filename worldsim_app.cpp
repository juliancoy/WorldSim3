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


VkAllocationCallbacks* g_Allocator = nullptr;
VkInstance g_Instance = VK_NULL_HANDLE;
VkPhysicalDevice g_PhysicalDevice = VK_NULL_HANDLE;
VkDevice g_Device = VK_NULL_HANDLE;
uint32_t g_QueueFamily = (uint32_t)-1;
VkQueue g_Queue = VK_NULL_HANDLE;
VkDescriptorPool g_DescriptorPool = VK_NULL_HANDLE;
VkSampler g_TileSampler = VK_NULL_HANDLE;
static VkCommandPool g_UploadCommandPool = VK_NULL_HANDLE;
static VkCommandBuffer g_UploadCommandBuffer = VK_NULL_HANDLE;
static VkDebugUtilsMessengerEXT g_DebugUtilsMessenger = VK_NULL_HANDLE;

ImGui_ImplVulkanH_Window g_MainWindowData;
int g_MinImageCount = 2;
bool g_SwapChainRebuild = false;

std::unordered_map<std::string, TileCacheEntry> g_TileCache;
std::list<std::string> g_TileLRU;
bool g_EnableValidationLayers = false;
std::vector<TileTexture> g_RetiredTextures;
static int g_TextureRetireFrames = 0;

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
    check_vk_result(vkQueueSubmit(g_Queue, 1, &submit, VK_NULL_HANDLE));
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

    const fs::path tile_path = root / "data" / tile_root_dir / std::to_string(z) / std::to_string(x) / (std::to_string(y) + ".png");
    if (!fs::exists(tile_path)) return nullptr;
    if (!loadTileTexture(tile_path, key)) return nullptr;

    auto loaded = g_TileCache.find(key);
    return loaded == g_TileCache.end() ? nullptr : &loaded->second.tex;
}

TileSample getTileSample(const fs::path& root, const std::string& tile_root_dir, int z, int x, int y) {
    if (z <= kMaxNativeTileZoom) {
        TileTexture* direct = getTileTexture(root, tile_root_dir, z, x, y);
        if (direct) return TileSample{direct, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f)};

        // Missing native-zoom tile: walk up parent levels and sample a sub-rect.
        for (int pz = z - 1; pz >= kMinZoom; --pz) {
            const int dz = z - pz;
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

    const int dz = z - kMaxNativeTileZoom;
    const int scale = 1 << dz;
    const int parent_x = x / scale;
    const int parent_y = y / scale;
    const int ox = x % scale;
    const int oy = y % scale;

    TileTexture* parent = getTileTexture(root, tile_root_dir, kMaxNativeTileZoom, parent_x, parent_y);
    if (!parent) return {};

    const float step = 1.0f / (float)scale;
    ImVec2 uv0((float)ox * step, (float)oy * step);
    ImVec2 uv1(uv0.x + step, uv0.y + step);
    return TileSample{parent, uv0, uv1};
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
    drainRetiredTextures(true);
}

void CleanupVulkan() {
    CleanupTileCache();
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
    check_vk_result(vkQueueSubmit(g_Queue, 1, &submit, fd->Fence));

    uint64_t shot_req_id = 0;
    {
        std::lock_guard<std::mutex> lk(g_ScreenshotState.mutex);
        if (g_ScreenshotState.pending) shot_req_id = g_ScreenshotState.req_id;
    }
    if (shot_req_id == 0) return;

    auto complete_screenshot = [&](bool ok, const std::string& path, const std::string& error) {
        std::lock_guard<std::mutex> lk(g_ScreenshotState.mutex);
        if (g_ScreenshotState.pending && g_ScreenshotState.req_id == shot_req_id) {
            g_ScreenshotState.pending = false;
            g_ScreenshotState.done_id = shot_req_id;
            g_ScreenshotState.ok = ok;
            g_ScreenshotState.path = path;
            g_ScreenshotState.error = error;
            g_ScreenshotState.cv.notify_all();
        }
    };

    check_vk_result(vkWaitForFences(g_Device, 1, &fd->Fence, VK_TRUE, UINT64_MAX));

    VkImage src_image = fd->Backbuffer;
    if (src_image == VK_NULL_HANDLE || wd->Width == 0 || wd->Height == 0) {
        complete_screenshot(false, "", "invalid backbuffer");
        return;
    }

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    const uint32_t width = wd->Width;
    const uint32_t height = wd->Height;
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
    check_vk_result(vkQueueSubmit(g_Queue, 1, &cap_submit, VK_NULL_HANDLE));
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
    const bool ok = writePpmRgb(
        out_file,
        static_cast<const uint8_t*>(mapped),
        width,
        height,
        (size_t)width * 4,
        wd->SurfaceFormat.format,
        capture_err);
    vkUnmapMemory(g_Device, staging_mem);
    if (ok) out_path = out_file.string();

    vkFreeCommandBuffers(g_Device, cap_pool, 1, &cap_cmd);
    vkDestroyCommandPool(g_Device, cap_pool, g_Allocator);
    vkDestroyBuffer(g_Device, staging, g_Allocator);
    vkFreeMemory(g_Device, staging_mem, g_Allocator);

    complete_screenshot(ok, out_path, capture_err);
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
    VkResult err = vkQueuePresentKHR(g_Queue, &info);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
        g_SwapChainRebuild = true;
        return;
    }
    check_vk_result(err);
    wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->SemaphoreCount;
}
