#include "worldsim_app_internal.h"

#include "tiles.h"

#include <nlohmann/json.hpp>

#include "stb_image.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
int g_TextureRetireFrames = 0;

struct TopoVectorCache {
    fs::file_time_type mtime{};
    bool loaded = false;
    std::vector<std::vector<ImVec2>> lines_lonlat;
};

TopoVectorCache g_TopoVectorCache;

uint32_t textureFindMemoryType(uint32_t type_filter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties mem_properties{};
    vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &mem_properties);
    for (uint32_t i = 0; i < mem_properties.memoryTypeCount; ++i) {
        if ((type_filter & (1u << i)) &&
            (mem_properties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    std::abort();
}

void createBuffer(
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties,
    VkBuffer& buffer,
    VkDeviceMemory& memory) {
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check_vk_result(vkCreateBuffer(g_Device, &buffer_info, g_Allocator, &buffer));

    VkMemoryRequirements mem_requirements{};
    vkGetBufferMemoryRequirements(g_Device, buffer, &mem_requirements);

    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_requirements.size;
    alloc_info.memoryTypeIndex = textureFindMemoryType(mem_requirements.memoryTypeBits, properties);
    check_vk_result(vkAllocateMemory(g_Device, &alloc_info, g_Allocator, &memory));
    check_vk_result(vkBindBufferMemory(g_Device, buffer, memory, 0));
}

uint32_t calcMipLevels(uint32_t w, uint32_t h) {
    uint32_t levels = 1;
    while (w > 1 || h > 1) {
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
        levels++;
    }
    return levels;
}

void createImage(uint32_t width, uint32_t height, uint32_t mip_levels, VkImage& image, VkDeviceMemory& memory) {
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

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(g_Device, image, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = textureFindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check_vk_result(vkAllocateMemory(g_Device, &alloc, g_Allocator, &memory));
    check_vk_result(vkBindImageMemory(g_Device, image, memory, 0));
}

VkImageView createImageView(VkImage image, uint32_t mip_levels) {
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

void transitionImage(
    VkCommandBuffer cmd,
    VkImage image,
    uint32_t base_level,
    uint32_t level_count,
    VkImageLayout old_layout,
    VkImageLayout new_layout) {
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

void submitUploadCommands(std::function<void(VkCommandBuffer)> record) {
    check_vk_result(vkResetCommandPool(g_Device, ::g_UploadCommandPool, 0));

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk_result(vkBeginCommandBuffer(::g_UploadCommandBuffer, &begin));
    record(::g_UploadCommandBuffer);
    check_vk_result(vkEndCommandBuffer(::g_UploadCommandBuffer));

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &::g_UploadCommandBuffer;
    {
        std::lock_guard<std::mutex> qlk(g_QueueSubmitMutex);
        check_vk_result(vkQueueSubmit(g_Queue, 1, &submit, VK_NULL_HANDLE));
    }
    check_vk_result(vkQueueWaitIdle(g_Queue));
}

void touchLRU(const std::string& key) {
    auto it = g_TileCache.find(key);
    if (it == g_TileCache.end()) return;
    g_TileLRU.erase(it->second.lru_it);
    g_TileLRU.push_front(key);
    it->second.lru_it = g_TileLRU.begin();
}

void evictIfNeeded() {
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

bool loadTileTexture(const fs::path& tile_path, const std::string& key) {
    int w = 0;
    int h = 0;
    int channels = 0;
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
    createBuffer(
        size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        staging,
        staging_mem);

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

TileTexture* getTileTexture(const fs::path& root, const std::string& tile_root_dir, int z, int x, int y) {
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
}  // namespace

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

bool finalizeTileTextureDescriptor(TileTexture& tex) {
    if (!tex.view) return false;
    if (tex.descriptor) return true;
    tex.descriptor = ImGui_ImplVulkan_AddTexture(g_TileSampler, tex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return tex.descriptor != VK_NULL_HANDLE;
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
    createBuffer(
        size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        staging,
        staging_mem);

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
    return finalizeTileTextureDescriptor(tex);
}

TileSample getTileSample(
    const fs::path& root,
    const std::string& tile_root_dir,
    int z,
    int x,
    int y,
    int max_native_tile_zoom) {
    const int max_fetch_zoom = std::min(z, std::clamp(max_native_tile_zoom, 0, kMaxInternalMathZoom));
    for (int pz = max_fetch_zoom; pz >= 0; --pz) {
        const int dz = z - pz;
        if (dz < 0 || dz > 30) continue;
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

TileTexture* getExactImageTexture(const fs::path& image_path, const std::string& cache_key) {
    const std::string key = cache_key.empty() ? image_path.string() : cache_key;
    auto it = g_TileCache.find(key);
    if (it != g_TileCache.end()) {
        touchLRU(key);
        return &it->second.tex;
    }
    std::error_code ec;
    if (!fs::exists(image_path, ec) || ec) return nullptr;
    if (!loadTileTexture(image_path, key)) return nullptr;
    auto loaded = g_TileCache.find(key);
    return loaded == g_TileCache.end() ? nullptr : &loaded->second.tex;
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
