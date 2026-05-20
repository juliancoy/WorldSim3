#include "heatmap_gpu_aggregate.h"

#include "aggregate_visualization_strategies.h"
#include "worldsim_app_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <condition_variable>
#include <mutex>
#include <map>
#include <memory>
#include <sstream>
#include <vector>

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

#if defined(WS3_GPU_SPLAT_SHADER_SPV)
static const char* kShaderPath = WS3_GPU_SPLAT_SHADER_SPV;
#else
static const char* kShaderPath = nullptr;
#endif

#if defined(WS3_GPU_SPLAT_SHADER_SRC)
static const char* kShaderSourcePath = WS3_GPU_SPLAT_SHADER_SRC;
#else
static const char* kShaderSourcePath = nullptr;
#endif

#if defined(WS3_GPU_SPLAT_RESOLVE_SHADER_SPV)
static const char* kResolveShaderPath = WS3_GPU_SPLAT_RESOLVE_SHADER_SPV;
#else
static const char* kResolveShaderPath = nullptr;
#endif

#if defined(WS3_GPU_SPLAT_RESOLVE_SHADER_SRC)
static const char* kResolveShaderSourcePath = WS3_GPU_SPLAT_RESOLVE_SHADER_SRC;
#else
static const char* kResolveShaderSourcePath = nullptr;
#endif

#if defined(WS3_GPU_SPLAT_HISTOGRAM_SHADER_SPV)
static const char* kHistogramShaderPath = WS3_GPU_SPLAT_HISTOGRAM_SHADER_SPV;
#else
static const char* kHistogramShaderPath = nullptr;
#endif

#if defined(WS3_GPU_SPLAT_HISTOGRAM_SHADER_SRC)
static const char* kHistogramShaderSourcePath = WS3_GPU_SPLAT_HISTOGRAM_SHADER_SRC;
#else
static const char* kHistogramShaderSourcePath = nullptr;
#endif

#if defined(WS3_GPU_SPLAT_HISTOGRAM_REDUCE_SHADER_SPV)
static const char* kHistogramReduceShaderPath = WS3_GPU_SPLAT_HISTOGRAM_REDUCE_SHADER_SPV;
#else
static const char* kHistogramReduceShaderPath = nullptr;
#endif

#if defined(WS3_GPU_SPLAT_HISTOGRAM_REDUCE_SHADER_SRC)
static const char* kHistogramReduceShaderSourcePath = WS3_GPU_SPLAT_HISTOGRAM_REDUCE_SHADER_SRC;
#else
static const char* kHistogramReduceShaderSourcePath = nullptr;
#endif

struct GpuSamplePacked {
    float a[4]; // lon, lat, r, g
    float b[4]; // b, prefer_gradient, pad, pad
};

struct GpuCtx {
    bool initialized = false;
    bool available = false;
    VkDescriptorSetLayout desc_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline bin_pipeline = VK_NULL_HANDLE;
    VkPipeline histogram_pipeline = VK_NULL_HANDLE;
    VkPipeline histogram_reduce_pipeline = VK_NULL_HANDLE;
    VkPipeline resolve_pipeline = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool = VK_NULL_HANDLE;
    VkCommandPool cmd_pool = VK_NULL_HANDLE;
};

struct GpuLayerResources {
    int layer_id = -1;
    uint64_t last_used_epoch = 0;
    uint32_t active_users = 0;
    std::mutex op_mutex;
    VkDescriptorSet desc_set = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkBuffer sample_buf = VK_NULL_HANDLE;
    VkDeviceMemory sample_mem = VK_NULL_HANDLE;
    void* sample_mapped = nullptr;
    VkDeviceSize sample_capacity = 0;
    VkBuffer accum_buf = VK_NULL_HANDLE;
    VkDeviceMemory accum_mem = VK_NULL_HANDLE;
    void* accum_mapped = nullptr;
    VkDeviceSize accum_capacity = 0;
    VkBuffer stats_buf = VK_NULL_HANDLE;
    VkDeviceMemory stats_mem = VK_NULL_HANDLE;
    void* stats_mapped = nullptr;
    VkDeviceSize stats_capacity = 0;
    VkBuffer texture_sample_staging_buf = VK_NULL_HANDLE;
    VkDeviceMemory texture_sample_staging_mem = VK_NULL_HANDLE;
    void* texture_sample_staging_mapped = nullptr;
    VkDeviceSize texture_sample_staging_capacity = 0;
    VkBuffer texture_sample_device_buf = VK_NULL_HANDLE;
    VkDeviceMemory texture_sample_device_mem = VK_NULL_HANDLE;
    VkDeviceSize texture_sample_device_capacity = 0;
    VkBuffer texture_accum_device_buf = VK_NULL_HANDLE;
    VkDeviceMemory texture_accum_device_mem = VK_NULL_HANDLE;
    VkDeviceSize texture_accum_device_capacity = 0;
    VkBuffer texture_stats_device_buf = VK_NULL_HANDLE;
    VkDeviceMemory texture_stats_device_mem = VK_NULL_HANDLE;
    VkDeviceSize texture_stats_device_capacity = 0;
    VkBuffer texture_histogram_buf = VK_NULL_HANDLE;
    VkDeviceMemory texture_histogram_mem = VK_NULL_HANDLE;
    VkDeviceSize texture_histogram_capacity = 0;
};

struct PooledAggregateTexture {
    TileTexture texture;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t last_used_epoch = 0;
};

constexpr size_t kMaxAggregateLayerResourceSlots = 12;
constexpr size_t kMaxPooledAggregateTextures = 12;
constexpr uint64_t kMaxPooledAggregateTextureBytes = 256ull * 1024ull * 1024ull;

GpuCtx g_ctx;
std::map<int, std::shared_ptr<GpuLayerResources>> g_layer_resources;
std::vector<PooledAggregateTexture> g_pooled_aggregate_textures;
std::mutex g_gpu_mutex;
std::condition_variable g_gpu_cv;
uint64_t g_gpu_resource_epoch = 0;
bool g_gpu_shutdown_requested = false;

uint32_t findMemoryType(uint32_t type_filter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
        if ((type_filter & (1u << i)) && (mem.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return UINT32_MAX;
}

bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& out_buf, VkDeviceMemory& out_mem) {
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g_Device, &bi, g_Allocator, &out_buf) != VK_SUCCESS) return false;

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(g_Device, out_buf, &req);
    uint32_t mt = findMemoryType(req.memoryTypeBits, properties);
    if (mt == UINT32_MAX) return false;

    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = mt;
    if (vkAllocateMemory(g_Device, &ai, g_Allocator, &out_mem) != VK_SUCCESS) return false;
    if (vkBindBufferMemory(g_Device, out_buf, out_mem, 0) != VK_SUCCESS) return false;
    return true;
}

uint64_t aggregateTextureBytes(uint32_t width, uint32_t height) {
    return uint64_t(width) * uint64_t(height) * 4ull;
}

void destroyBufferAndMemory(VkBuffer& buffer, VkDeviceMemory& memory) {
    if (buffer) {
        vkDestroyBuffer(g_Device, buffer, g_Allocator);
        buffer = VK_NULL_HANDLE;
    }
    if (memory) {
        vkFreeMemory(g_Device, memory, g_Allocator);
        memory = VK_NULL_HANDLE;
    }
}

bool createImage(
    uint32_t width,
    uint32_t height,
    VkImageUsageFlags usage,
    VkImage& out_image,
    VkDeviceMemory& out_memory) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    info.usage = usage;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(g_Device, &info, g_Allocator, &out_image) != VK_SUCCESS) return false;

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(g_Device, out_image, &req);
    uint32_t mt = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) return false;
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = mt;
    if (vkAllocateMemory(g_Device, &alloc, g_Allocator, &out_memory) != VK_SUCCESS) return false;
    if (vkBindImageMemory(g_Device, out_image, out_memory, 0) != VK_SUCCESS) return false;
    return true;
}

VkImageView createImageView(VkImage image) {
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    info.subresourceRange.levelCount = 1;
    info.subresourceRange.layerCount = 1;
    VkImageView out = VK_NULL_HANDLE;
    if (vkCreateImageView(g_Device, &info, g_Allocator, &out) != VK_SUCCESS) return VK_NULL_HANDLE;
    return out;
}

void transitionImage(
    VkCommandBuffer cmd,
    VkImage image,
    VkImageLayout old_layout,
    VkImageLayout new_layout,
    VkAccessFlags src_access,
    VkAccessFlags dst_access,
    VkPipelineStageFlags src_stage,
    VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

std::vector<uint32_t> loadSpirv(const char* path) {
    if (!path || !*path) return {};
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    in.seekg(0, std::ios::end);
    const std::streamsize sz = in.tellg();
    if (sz <= 0 || (sz % 4) != 0) return {};
    in.seekg(0, std::ios::beg);
    std::vector<uint32_t> code((size_t)sz / 4);
    in.read(reinterpret_cast<char*>(code.data()), sz);
    if (!in) return {};
    return code;
}

bool isExecutableFile(const std::filesystem::path& path) {
#if defined(_WIN32)
    return std::filesystem::exists(path);
#else
    return ::access(path.c_str(), X_OK) == 0;
#endif
}

std::filesystem::path findExecutableOnPath(const char* name) {
    if (!name || !*name) return {};
    const char* path_env = std::getenv("PATH");
    if (!path_env) return {};
    std::stringstream ss(path_env);
    std::string segment;
    while (std::getline(ss, segment, ':')) {
        if (segment.empty()) continue;
        std::filesystem::path candidate = std::filesystem::path(segment) / name;
        if (isExecutableFile(candidate)) return candidate;
    }
    return {};
}

bool runShaderCompiler(
    const std::filesystem::path& compiler,
    const std::filesystem::path& source_path,
    const std::filesystem::path& output_path,
    bool use_glslc,
    std::string* error) {
    if (compiler.empty()) {
        if (error) *error = "compiler path missing";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(output_path.parent_path(), ec);
#if defined(_WIN32)
    std::ostringstream cmd;
    cmd << '"' << compiler.string() << "\" ";
    if (use_glslc) cmd << "-fshader-stage=comp ";
    else cmd << "-V ";
    cmd << '"' << source_path.string() << "\" -o \"" << output_path.string() << '"';
    const int rc = std::system(cmd.str().c_str());
    if (rc != 0) {
        if (error) *error = "compiler exited non-zero";
        return false;
    }
    return true;
#else
    const std::string compiler_s = compiler.string();
    const std::string source_s = source_path.string();
    const std::string output_s = output_path.string();
    const char* argv_glslang[] = {
        compiler_s.c_str(),
        "-V",
        source_s.c_str(),
        "-o",
        output_s.c_str(),
        nullptr
    };
    const char* argv_glslc[] = {
        compiler_s.c_str(),
        "-fshader-stage=comp",
        source_s.c_str(),
        "-o",
        output_s.c_str(),
        nullptr
    };
    pid_t pid = fork();
    if (pid < 0) {
        if (error) *error = "fork failed";
        return false;
    }
    if (pid == 0) {
        execvp(compiler_s.c_str(), const_cast<char* const*>(use_glslc ? argv_glslc : argv_glslang));
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        if (error) *error = "waitpid failed";
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (error) *error = "compiler exited non-zero";
        return false;
    }
    return true;
#endif
}

std::filesystem::path runtimeShaderOutputPath() {
    if (kShaderPath && *kShaderPath) return std::filesystem::path(kShaderPath);
    const std::filesystem::path tmp_root = std::filesystem::temp_directory_path() / "worldsim3-generated-shaders";
    return tmp_root / "heatmap_splat.comp.spv";
}

std::filesystem::path runtimeResolveShaderOutputPath() {
    if (kResolveShaderPath && *kResolveShaderPath) return std::filesystem::path(kResolveShaderPath);
    const std::filesystem::path tmp_root = std::filesystem::temp_directory_path() / "worldsim3-generated-shaders";
    return tmp_root / "heatmap_splat_resolve.comp.spv";
}

std::filesystem::path runtimeHistogramShaderOutputPath() {
    if (kHistogramShaderPath && *kHistogramShaderPath) return std::filesystem::path(kHistogramShaderPath);
    const std::filesystem::path tmp_root = std::filesystem::temp_directory_path() / "worldsim3-generated-shaders";
    return tmp_root / "heatmap_splat_histogram.comp.spv";
}

std::filesystem::path runtimeHistogramReduceShaderOutputPath() {
    if (kHistogramReduceShaderPath && *kHistogramReduceShaderPath) return std::filesystem::path(kHistogramReduceShaderPath);
    const std::filesystem::path tmp_root = std::filesystem::temp_directory_path() / "worldsim3-generated-shaders";
    return tmp_root / "heatmap_splat_histogram_reduce.comp.spv";
}

std::vector<uint32_t> loadOrBuildSpirv(
    const std::filesystem::path& output_path,
    const char* shader_source_path,
    std::string* error) {
    auto code = loadSpirv(output_path.c_str());
    if (!code.empty()) return code;

    if (!shader_source_path || !*shader_source_path) {
        if (error) *error = "GPU splat shader source path unavailable";
        std::cerr << "[worldsim3] GPU splat shader SPIR-V missing and no shader source path was compiled in.\n";
        return {};
    }

    const std::filesystem::path source_path(shader_source_path);
    const std::filesystem::path glslang = findExecutableOnPath("glslangValidator");
    const std::filesystem::path glslc = findExecutableOnPath("glslc");

    std::cerr
        << "[worldsim3] GPU splat shader SPIR-V missing at " << output_path << ".\n"
        << "[worldsim3] Runtime shader compile fallback engaged for " << source_path << ".\n";
    if (glslang.empty()) {
        std::cerr << "[worldsim3] glslangValidator missing on PATH.\n";
    }
    if (glslc.empty()) {
        std::cerr << "[worldsim3] glslc missing on PATH.\n";
    }

    struct CompilerCandidate {
        std::filesystem::path path;
        bool use_glslc = false;
        const char* label = nullptr;
    };
    std::vector<CompilerCandidate> candidates;
    if (!glslang.empty()) candidates.push_back({glslang, false, "glslangValidator"});
    if (!glslc.empty()) candidates.push_back({glslc, true, "glslc"});
    if (candidates.empty()) {
        if (error) *error = "neither glslangValidator nor glslc is available on PATH";
        std::cerr << "[worldsim3] No runtime shader compiler is available. Install glslangValidator or glslc.\n";
        return {};
    }

    for (const auto& candidate : candidates) {
        std::string compile_error;
        std::cerr << "[worldsim3] Trying runtime shader compile with " << candidate.label << ".\n";
        if (runShaderCompiler(candidate.path, source_path, output_path, candidate.use_glslc, &compile_error)) {
            code = loadSpirv(output_path.c_str());
            if (!code.empty()) {
                std::cerr << "[worldsim3] Runtime shader compile succeeded with " << candidate.label
                          << "; wrote " << output_path << ".\n";
                return code;
            }
            std::cerr << "[worldsim3] " << candidate.label
                      << " reported success, but SPIR-V could not be loaded from " << output_path << ".\n";
        } else {
            std::cerr << "[worldsim3] Runtime shader compile failed with " << candidate.label
                      << ": " << compile_error << ".\n";
        }
    }

    if (error) *error = "runtime shader compile failed with all available compilers";
    return {};
}

void destroyLayerResources(GpuLayerResources& res);

void shutdownCtx() {
    if (g_Device != VK_NULL_HANDLE) {
        for (auto& pooled : g_pooled_aggregate_textures) {
            destroyTileTextureNow(pooled.texture);
        }
        for (auto& kv : g_layer_resources) {
            destroyLayerResources(*kv.second);
        }
    }
    g_pooled_aggregate_textures.clear();
    g_layer_resources.clear();
    if (g_ctx.bin_pipeline) vkDestroyPipeline(g_Device, g_ctx.bin_pipeline, g_Allocator);
    if (g_ctx.histogram_pipeline) vkDestroyPipeline(g_Device, g_ctx.histogram_pipeline, g_Allocator);
    if (g_ctx.histogram_reduce_pipeline) vkDestroyPipeline(g_Device, g_ctx.histogram_reduce_pipeline, g_Allocator);
    if (g_ctx.resolve_pipeline) vkDestroyPipeline(g_Device, g_ctx.resolve_pipeline, g_Allocator);
    if (g_ctx.pipeline_layout) vkDestroyPipelineLayout(g_Device, g_ctx.pipeline_layout, g_Allocator);
    if (g_ctx.desc_pool) vkDestroyDescriptorPool(g_Device, g_ctx.desc_pool, g_Allocator);
    if (g_ctx.desc_layout) vkDestroyDescriptorSetLayout(g_Device, g_ctx.desc_layout, g_Allocator);
    if (g_ctx.cmd_pool) vkDestroyCommandPool(g_Device, g_ctx.cmd_pool, g_Allocator);
    g_ctx = {};
}

bool ensurePersistentBuffer(
    VkDeviceSize required,
    VkBufferUsageFlags usage,
    VkBuffer& buffer,
    VkDeviceMemory& memory,
    void*& mapped,
    VkDeviceSize& capacity,
    const char* label,
    std::string* error) {
    if (required == 0) required = 1;
    if (buffer != VK_NULL_HANDLE && capacity >= required && mapped) return true;

    if (mapped && memory) {
        vkUnmapMemory(g_Device, memory);
        mapped = nullptr;
    }
    if (buffer) {
        vkDestroyBuffer(g_Device, buffer, g_Allocator);
        buffer = VK_NULL_HANDLE;
    }
    if (memory) {
        vkFreeMemory(g_Device, memory, g_Allocator);
        memory = VK_NULL_HANDLE;
    }
    capacity = 0;

    VkDeviceSize new_capacity = std::max(required, required + required / 2);
    new_capacity = std::max<VkDeviceSize>(new_capacity, 1024 * 1024);
    if (!createBuffer(
            new_capacity,
            usage,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            buffer,
            memory)) {
        if (error) *error = std::string("failed to create persistent ") + label + " buffer";
        return false;
    }
    if (vkMapMemory(g_Device, memory, 0, new_capacity, 0, &mapped) != VK_SUCCESS) {
        if (error) *error = std::string("failed to map persistent ") + label + " buffer";
        vkDestroyBuffer(g_Device, buffer, g_Allocator);
        vkFreeMemory(g_Device, memory, g_Allocator);
        buffer = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        mapped = nullptr;
        return false;
    }
    capacity = new_capacity;
    return true;
}

bool ensurePersistentDeviceLocalBuffer(
    VkDeviceSize required,
    VkBufferUsageFlags usage,
    VkBuffer& buffer,
    VkDeviceMemory& memory,
    VkDeviceSize& capacity,
    const char* label,
    std::string* error) {
    if (required == 0) required = 1;
    if (buffer != VK_NULL_HANDLE && capacity >= required) return true;
    destroyBufferAndMemory(buffer, memory);
    capacity = 0;

    VkDeviceSize new_capacity = std::max(required, required + required / 2);
    new_capacity = std::max<VkDeviceSize>(new_capacity, 1024 * 1024);
    if (!createBuffer(new_capacity, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, buffer, memory)) {
        if (error) *error = std::string("failed to create device-local ") + label + " buffer";
        return false;
    }
    capacity = new_capacity;
    return true;
}

bool ensurePersistentDescriptors(GpuLayerResources& res, std::string* error) {
    if (res.desc_set != VK_NULL_HANDLE) return true;
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = g_ctx.desc_pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &g_ctx.desc_layout;
    if (vkAllocateDescriptorSets(g_Device, &dsai, &res.desc_set) != VK_SUCCESS) {
        if (error) *error = "failed to allocate persistent descriptor set for layer resource";
        return false;
    }
    return true;
}

bool ensurePersistentCommandBuffer(GpuLayerResources& res, std::string* error) {
    if (res.cmd != VK_NULL_HANDLE) return true;
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = g_ctx.cmd_pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(g_Device, &cbai, &res.cmd) != VK_SUCCESS) {
        if (error) *error = "failed to allocate persistent command buffer for layer resource";
        return false;
    }
    return true;
}

bool ensurePersistentFence(GpuLayerResources& res, std::string* error) {
    if (res.fence != VK_NULL_HANDLE) return true;
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateFence(g_Device, &fci, g_Allocator, &res.fence) != VK_SUCCESS) {
        if (error) *error = "failed to allocate persistent fence for layer resource";
        return false;
    }
    return true;
}

void destroyLayerResources(GpuLayerResources& res) {
    if (res.sample_mapped && res.sample_mem) vkUnmapMemory(g_Device, res.sample_mem);
    if (res.accum_mapped && res.accum_mem) vkUnmapMemory(g_Device, res.accum_mem);
    if (res.stats_mapped && res.stats_mem) vkUnmapMemory(g_Device, res.stats_mem);
    if (res.texture_sample_staging_mapped && res.texture_sample_staging_mem) {
        vkUnmapMemory(g_Device, res.texture_sample_staging_mem);
    }
    destroyBufferAndMemory(res.sample_buf, res.sample_mem);
    destroyBufferAndMemory(res.accum_buf, res.accum_mem);
    destroyBufferAndMemory(res.stats_buf, res.stats_mem);
    destroyBufferAndMemory(res.texture_sample_staging_buf, res.texture_sample_staging_mem);
    destroyBufferAndMemory(res.texture_sample_device_buf, res.texture_sample_device_mem);
    destroyBufferAndMemory(res.texture_accum_device_buf, res.texture_accum_device_mem);
    destroyBufferAndMemory(res.texture_stats_device_buf, res.texture_stats_device_mem);
    destroyBufferAndMemory(res.texture_histogram_buf, res.texture_histogram_mem);
    if (res.fence) vkDestroyFence(g_Device, res.fence, g_Allocator);
    res.layer_id = -1;
    res.last_used_epoch = 0;
    res.active_users = 0;
    res.desc_set = VK_NULL_HANDLE;
    res.cmd = VK_NULL_HANDLE;
    res.fence = VK_NULL_HANDLE;
    res.sample_mapped = nullptr;
    res.sample_capacity = 0;
    res.accum_mapped = nullptr;
    res.accum_capacity = 0;
    res.stats_mapped = nullptr;
    res.stats_capacity = 0;
    res.texture_sample_staging_mapped = nullptr;
    res.texture_sample_staging_capacity = 0;
    res.texture_sample_device_capacity = 0;
    res.texture_accum_device_capacity = 0;
    res.texture_stats_device_capacity = 0;
    res.texture_histogram_capacity = 0;
}

void pruneLayerResources() {
    while (g_layer_resources.size() > kMaxAggregateLayerResourceSlots) {
        auto evict_it = g_layer_resources.end();
        for (auto it = g_layer_resources.begin(); it != g_layer_resources.end(); ++it) {
            if (!it->second || it->second->active_users != 0) continue;
            std::unique_lock<std::mutex> res_lock(it->second->op_mutex, std::try_to_lock);
            if (!res_lock.owns_lock()) continue;
            if (evict_it == g_layer_resources.end() ||
                it->second->last_used_epoch < evict_it->second->last_used_epoch) {
                evict_it = it;
            }
        }
        if (evict_it == g_layer_resources.end()) break;
        destroyLayerResources(*evict_it->second);
        g_layer_resources.erase(evict_it);
    }
}

void prunePooledAggregateTextures() {
    auto current_bytes = [&]() {
        uint64_t total = 0;
        for (const auto& pooled : g_pooled_aggregate_textures) {
            total += aggregateTextureBytes(pooled.width, pooled.height);
        }
        return total;
    };
    while (!g_pooled_aggregate_textures.empty() &&
           (g_pooled_aggregate_textures.size() > kMaxPooledAggregateTextures ||
            current_bytes() > kMaxPooledAggregateTextureBytes)) {
        auto evict_it = g_pooled_aggregate_textures.begin();
        for (auto it = g_pooled_aggregate_textures.begin(); it != g_pooled_aggregate_textures.end(); ++it) {
            if (it->last_used_epoch < evict_it->last_used_epoch) evict_it = it;
        }
        destroyTileTextureNow(evict_it->texture);
        g_pooled_aggregate_textures.erase(evict_it);
    }
}

std::shared_ptr<GpuLayerResources> getOrCreateLayerResources(int layer_id) {
    if (g_gpu_shutdown_requested) return nullptr;
    auto it = g_layer_resources.find(layer_id);
    if (it != g_layer_resources.end()) {
        it->second->last_used_epoch = ++g_gpu_resource_epoch;
        ++it->second->active_users;
        return it->second;
    }
    auto res = std::make_shared<GpuLayerResources>();
    res->layer_id = layer_id;
    res->last_used_epoch = ++g_gpu_resource_epoch;
    res->active_users = 1;
    auto [insert_it, inserted] = g_layer_resources.emplace(layer_id, res);
    (void)inserted;
    pruneLayerResources();
    return insert_it->second;
}

void releaseLayerResources(const std::shared_ptr<GpuLayerResources>& res) {
    if (!res) return;
    std::lock_guard<std::mutex> lk(g_gpu_mutex);
    auto it = g_layer_resources.find(res->layer_id);
    if (it == g_layer_resources.end()) return;
    if (it->second->active_users > 0) --it->second->active_users;
    it->second->last_used_epoch = ++g_gpu_resource_epoch;
    g_gpu_cv.notify_all();
}

bool tryAcquirePooledAggregateTexture(uint32_t width, uint32_t height, TileTexture& out_texture) {
    for (auto it = g_pooled_aggregate_textures.begin(); it != g_pooled_aggregate_textures.end(); ++it) {
        if (it->width != width || it->height != height) continue;
        out_texture = it->texture;
        g_pooled_aggregate_textures.erase(it);
        ++g_gpu_resource_epoch;
        return true;
    }
    return false;
}

bool initCtx(std::string* error) {
    if (g_ctx.initialized) return g_ctx.available;
    g_ctx.initialized = true;
    if (g_gpu_shutdown_requested) {
        if (error) *error = "GPU aggregate shutdown in progress";
        return false;
    }

    const char* env_disable = std::getenv("WS3_DISABLE_EXPERIMENTAL_GPU_AGGREGATE");
    if (env_disable && std::strcmp(env_disable, "1") == 0) {
        if (error) *error = "GPU aggregate disabled by WS3_DISABLE_EXPERIMENTAL_GPU_AGGREGATE=1";
        return false;
    }

    if (g_Device == VK_NULL_HANDLE || g_PhysicalDevice == VK_NULL_HANDLE || g_Queue == VK_NULL_HANDLE) {
        if (error) *error = "Vulkan device/queue unavailable";
        return false;
    }
    auto spirv = loadOrBuildSpirv(runtimeShaderOutputPath(), kShaderSourcePath, error);
    if (spirv.empty()) {
        if (error && error->empty()) *error = "GPU splat shader SPIR-V unavailable";
        return false;
    }
    auto histogram_spirv = loadOrBuildSpirv(runtimeHistogramShaderOutputPath(), kHistogramShaderSourcePath, error);
    if (histogram_spirv.empty()) {
        if (error && error->empty()) *error = "GPU splat histogram shader SPIR-V unavailable";
        return false;
    }
    auto histogram_reduce_spirv =
        loadOrBuildSpirv(runtimeHistogramReduceShaderOutputPath(), kHistogramReduceShaderSourcePath, error);
    if (histogram_reduce_spirv.empty()) {
        if (error && error->empty()) *error = "GPU splat histogram reduce shader SPIR-V unavailable";
        return false;
    }
    auto resolve_spirv = loadOrBuildSpirv(runtimeResolveShaderOutputPath(), kResolveShaderSourcePath, error);
    if (resolve_spirv.empty()) {
        if (error && error->empty()) *error = "GPU splat resolve shader SPIR-V unavailable";
        return false;
    }

    VkShaderModuleCreateInfo smi{};
    smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = spirv.size() * sizeof(uint32_t);
    smi.pCode = spirv.data();
    VkShaderModule bin_shader = VK_NULL_HANDLE;
    if (vkCreateShaderModule(g_Device, &smi, g_Allocator, &bin_shader) != VK_SUCCESS) {
        if (error) *error = "vkCreateShaderModule failed";
        return false;
    }
    smi.codeSize = histogram_spirv.size() * sizeof(uint32_t);
    smi.pCode = histogram_spirv.data();
    VkShaderModule histogram_shader = VK_NULL_HANDLE;
    if (vkCreateShaderModule(g_Device, &smi, g_Allocator, &histogram_shader) != VK_SUCCESS) {
        vkDestroyShaderModule(g_Device, bin_shader, g_Allocator);
        if (error) *error = "vkCreateShaderModule failed";
        return false;
    }
    smi.codeSize = histogram_reduce_spirv.size() * sizeof(uint32_t);
    smi.pCode = histogram_reduce_spirv.data();
    VkShaderModule histogram_reduce_shader = VK_NULL_HANDLE;
    if (vkCreateShaderModule(g_Device, &smi, g_Allocator, &histogram_reduce_shader) != VK_SUCCESS) {
        vkDestroyShaderModule(g_Device, bin_shader, g_Allocator);
        vkDestroyShaderModule(g_Device, histogram_shader, g_Allocator);
        if (error) *error = "vkCreateShaderModule failed";
        return false;
    }
    smi.codeSize = resolve_spirv.size() * sizeof(uint32_t);
    smi.pCode = resolve_spirv.data();
    VkShaderModule resolve_shader = VK_NULL_HANDLE;
    if (vkCreateShaderModule(g_Device, &smi, g_Allocator, &resolve_shader) != VK_SUCCESS) {
        vkDestroyShaderModule(g_Device, bin_shader, g_Allocator);
        vkDestroyShaderModule(g_Device, histogram_shader, g_Allocator);
        vkDestroyShaderModule(g_Device, histogram_reduce_shader, g_Allocator);
        if (error) *error = "vkCreateShaderModule failed";
        return false;
    }

    VkDescriptorSetLayoutBinding b0{};
    b0.binding = 0;
    b0.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b0.descriptorCount = 1;
    b0.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutBinding b1 = b0;
    b1.binding = 1;
    VkDescriptorSetLayoutBinding b2 = b0;
    b2.binding = 2;
    VkDescriptorSetLayoutBinding b3{};
    b3.binding = 3;
    b3.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b3.descriptorCount = 1;
    b3.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutBinding b4 = b0;
    b4.binding = 4;

    std::array<VkDescriptorSetLayoutBinding, 5> bindings{b0, b1, b2, b3, b4};
    VkDescriptorSetLayoutCreateInfo dli{};
    dli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dli.bindingCount = (uint32_t)bindings.size();
    dli.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(g_Device, &dli, g_Allocator, &g_ctx.desc_layout) != VK_SUCCESS) {
        vkDestroyShaderModule(g_Device, bin_shader, g_Allocator);
        vkDestroyShaderModule(g_Device, histogram_shader, g_Allocator);
        vkDestroyShaderModule(g_Device, histogram_reduce_shader, g_Allocator);
        vkDestroyShaderModule(g_Device, resolve_shader, g_Allocator);
        if (error) *error = "vkCreateDescriptorSetLayout failed";
        shutdownCtx();
        return false;
    }

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset = 0;
    pcr.size = 9 * sizeof(uint32_t);

    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &g_ctx.desc_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(g_Device, &pli, g_Allocator, &g_ctx.pipeline_layout) != VK_SUCCESS) {
        vkDestroyShaderModule(g_Device, bin_shader, g_Allocator);
        vkDestroyShaderModule(g_Device, histogram_shader, g_Allocator);
        vkDestroyShaderModule(g_Device, histogram_reduce_shader, g_Allocator);
        vkDestroyShaderModule(g_Device, resolve_shader, g_Allocator);
        if (error) *error = "vkCreatePipelineLayout failed";
        shutdownCtx();
        return false;
    }

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = bin_shader;
    stage.pName = "main";

    VkComputePipelineCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage = stage;
    cpi.layout = g_ctx.pipeline_layout;
    if (vkCreateComputePipelines(g_Device, VK_NULL_HANDLE, 1, &cpi, g_Allocator, &g_ctx.bin_pipeline) != VK_SUCCESS) {
        vkDestroyShaderModule(g_Device, bin_shader, g_Allocator);
        vkDestroyShaderModule(g_Device, histogram_shader, g_Allocator);
        vkDestroyShaderModule(g_Device, histogram_reduce_shader, g_Allocator);
        vkDestroyShaderModule(g_Device, resolve_shader, g_Allocator);
        if (error) *error = "vkCreateComputePipelines failed";
        shutdownCtx();
        return false;
    }
    stage.module = histogram_shader;
    if (vkCreateComputePipelines(g_Device, VK_NULL_HANDLE, 1, &cpi, g_Allocator, &g_ctx.histogram_pipeline) != VK_SUCCESS) {
        vkDestroyShaderModule(g_Device, bin_shader, g_Allocator);
        vkDestroyShaderModule(g_Device, histogram_shader, g_Allocator);
        vkDestroyShaderModule(g_Device, histogram_reduce_shader, g_Allocator);
        vkDestroyShaderModule(g_Device, resolve_shader, g_Allocator);
        if (error) *error = "vkCreateComputePipelines failed";
        shutdownCtx();
        return false;
    }
    stage.module = histogram_reduce_shader;
    if (vkCreateComputePipelines(g_Device, VK_NULL_HANDLE, 1, &cpi, g_Allocator, &g_ctx.histogram_reduce_pipeline) != VK_SUCCESS) {
        vkDestroyShaderModule(g_Device, bin_shader, g_Allocator);
        vkDestroyShaderModule(g_Device, histogram_shader, g_Allocator);
        vkDestroyShaderModule(g_Device, histogram_reduce_shader, g_Allocator);
        vkDestroyShaderModule(g_Device, resolve_shader, g_Allocator);
        if (error) *error = "vkCreateComputePipelines failed";
        shutdownCtx();
        return false;
    }
    stage.module = resolve_shader;
    if (vkCreateComputePipelines(g_Device, VK_NULL_HANDLE, 1, &cpi, g_Allocator, &g_ctx.resolve_pipeline) != VK_SUCCESS) {
        vkDestroyShaderModule(g_Device, bin_shader, g_Allocator);
        vkDestroyShaderModule(g_Device, histogram_shader, g_Allocator);
        vkDestroyShaderModule(g_Device, histogram_reduce_shader, g_Allocator);
        vkDestroyShaderModule(g_Device, resolve_shader, g_Allocator);
        if (error) *error = "vkCreateComputePipelines failed";
        shutdownCtx();
        return false;
    }
    vkDestroyShaderModule(g_Device, bin_shader, g_Allocator);
    vkDestroyShaderModule(g_Device, histogram_shader, g_Allocator);
    vkDestroyShaderModule(g_Device, histogram_reduce_shader, g_Allocator);
    vkDestroyShaderModule(g_Device, resolve_shader, g_Allocator);

    std::array<VkDescriptorPoolSize, 2> pool_sizes{};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_sizes[0].descriptorCount = 320;
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    pool_sizes[1].descriptorCount = 64;
    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.maxSets = 64;
    dpi.poolSizeCount = (uint32_t)pool_sizes.size();
    dpi.pPoolSizes = pool_sizes.data();
    if (vkCreateDescriptorPool(g_Device, &dpi, g_Allocator, &g_ctx.desc_pool) != VK_SUCCESS) {
        if (error) *error = "vkCreateDescriptorPool failed";
        shutdownCtx();
        return false;
    }

    VkCommandPoolCreateInfo cpool{};
    cpool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpool.queueFamilyIndex = g_QueueFamily;
    if (vkCreateCommandPool(g_Device, &cpool, g_Allocator, &g_ctx.cmd_pool) != VK_SUCCESS) {
        if (error) *error = "vkCreateCommandPool failed";
        shutdownCtx();
        return false;
    }

    g_ctx.available = true;
    return true;
}

} // namespace

bool buildGpuSplatAggregate(
    const std::vector<HeatSample>& group,
    int rw,
    int rh,
    float raster_min_lon,
    float raster_min_lat,
    float raster_max_lon,
    float raster_max_lat,
    float sigma_r,
    std::vector<float>& out_density,
    std::vector<float>& out_cr,
    std::vector<float>& out_cg,
    std::vector<float>& out_cb,
    std::vector<float>& out_cw,
    std::vector<float>& out_gv,
    std::vector<float>& out_sv,
    std::string* error) {
    if (group.empty() || rw <= 0 || rh <= 0) return false;

    std::vector<GpuSamplePacked> packed;
    packed.reserve(group.size());
    for (const auto& s : group) {
        GpuSamplePacked p{};
        p.a[0] = s.lon;
        p.a[1] = s.lat;
        p.a[2] = s.color.x;
        p.a[3] = s.color.y;
        p.b[0] = s.color.z;
        p.b[1] = s.prefer_gradient ? 1.0f : 0.0f;
        packed.push_back(p);
    }

    const VkDeviceSize sample_bytes = (VkDeviceSize)packed.size() * sizeof(GpuSamplePacked);
    const size_t pixel_count = (size_t)rw * (size_t)rh;
    const VkDeviceSize accum_uints = (VkDeviceSize)pixel_count * 7;
    const VkDeviceSize accum_bytes = accum_uints * sizeof(uint32_t);
    const VkDeviceSize stats_bytes = sizeof(uint32_t) * 2;
    const int layer_id = packed.empty() ? -1 : group.front().layer;
    std::shared_ptr<GpuLayerResources> res;
    {
        std::lock_guard<std::mutex> lk(g_gpu_mutex);
        if (!initCtx(error)) return false;
        if (!g_ctx.available) {
            if (error && error->empty()) *error = "GPU context unavailable";
            return false;
        }
        res = getOrCreateLayerResources(layer_id);
    }
    if (!res) {
        if (error) *error = "failed to obtain layer GPU resources";
        return false;
    }
    std::lock_guard<std::mutex> res_lk(res->op_mutex);
    auto release_res = [&]() { releaseLayerResources(res); };

    if (!ensurePersistentBuffer(
            sample_bytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            res->sample_buf,
            res->sample_mem,
            res->sample_mapped,
            res->sample_capacity,
            "sample",
            error)) {
        release_res();
        return false;
    }
    if (!ensurePersistentBuffer(
            accum_bytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            res->accum_buf,
            res->accum_mem,
            res->accum_mapped,
            res->accum_capacity,
            "accum",
            error)) {
        release_res();
        return false;
    }
    if (!ensurePersistentBuffer(
            stats_bytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            res->stats_buf,
            res->stats_mem,
            res->stats_mapped,
            res->stats_capacity,
            "stats",
            error)) {
        release_res();
        return false;
    }
    if (!ensurePersistentDescriptors(*res, error) ||
        !ensurePersistentCommandBuffer(*res, error) ||
        !ensurePersistentFence(*res, error)) {
        release_res();
        return false;
    }

    std::memcpy(res->sample_mapped, packed.data(), (size_t)sample_bytes);
    std::memset(res->accum_mapped, 0, (size_t)accum_bytes);
    std::memset(res->stats_mapped, 0, (size_t)stats_bytes);

    VkDescriptorBufferInfo sbi{};
    sbi.buffer = res->sample_buf;
    sbi.offset = 0;
    sbi.range = sample_bytes;

    VkDescriptorBufferInfo abi{};
    abi.buffer = res->accum_buf;
    abi.offset = 0;
    abi.range = accum_bytes;

    VkDescriptorBufferInfo tbi{};
    tbi.buffer = res->stats_buf;
    tbi.offset = 0;
    tbi.range = stats_bytes;

    VkWriteDescriptorSet w0{};
    w0.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w0.dstSet = res->desc_set;
    w0.dstBinding = 0;
    w0.descriptorCount = 1;
    w0.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w0.pBufferInfo = &sbi;

    VkWriteDescriptorSet w1 = w0;
    w1.dstBinding = 1;
    w1.pBufferInfo = &abi;

    VkWriteDescriptorSet w2 = w0;
    w2.dstBinding = 2;
    w2.pBufferInfo = &tbi;

    std::array<VkWriteDescriptorSet, 3> writes{w0, w1, w2};
    vkUpdateDescriptorSets(g_Device, (uint32_t)writes.size(), writes.data(), 0, nullptr);

    if (vkWaitForFences(g_Device, 1, &res->fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        if (error) *error = "vkWaitForFences failed";
        release_res();
        return false;
    }
    if (vkResetFences(g_Device, 1, &res->fence) != VK_SUCCESS) {
        if (error) *error = "vkResetFences failed";
        release_res();
        return false;
    }
    if (vkResetCommandBuffer(res->cmd, 0) != VK_SUCCESS) {
        if (error) *error = "vkResetCommandBuffer failed";
        release_res();
        return false;
    }
    VkCommandBufferBeginInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(res->cmd, &cbi) != VK_SUCCESS) {
        if (error) *error = "vkBeginCommandBuffer failed";
        release_res();
        return false;
    }

    vkCmdBindPipeline(res->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_ctx.bin_pipeline);
    vkCmdBindDescriptorSets(res->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_ctx.pipeline_layout, 0, 1, &res->desc_set, 0, nullptr);

    struct PushRaw {
        uint32_t sample_count;
        uint32_t width;
        uint32_t height;
        uint32_t radius;
        float min_lon;
        float min_lat;
        float max_lon;
        float max_lat;
        float sigma_r;
    } push{};

    push.sample_count = (uint32_t)packed.size();
    push.width = (uint32_t)rw;
    push.height = (uint32_t)rh;
    // Keep the compute dispatch bounded. The render builder applies the
    // Gaussian blur as a separable pass after this GPU binning step.
    push.radius = 0;
    push.min_lon = raster_min_lon;
    push.min_lat = raster_min_lat;
    push.max_lon = raster_max_lon;
    push.max_lat = raster_max_lat;
    push.sigma_r = std::max(1.0f, sigma_r);

    vkCmdPushConstants(res->cmd, g_ctx.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushRaw), &push);

    const uint32_t wg = 64;
    const uint32_t groups = (push.sample_count + wg - 1) / wg;
    vkCmdDispatch(res->cmd, std::max(1u, groups), 1, 1);

    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = res->accum_buf;
    barrier.offset = 0;
    barrier.size = accum_bytes;
    vkCmdPipelineBarrier(
        res->cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT,
        0,
        0,
        nullptr,
        1,
        &barrier,
        0,
        nullptr);

    if (vkEndCommandBuffer(res->cmd) != VK_SUCCESS) {
        if (error) *error = "vkEndCommandBuffer failed";
        release_res();
        return false;
    }

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &res->cmd;
    VkResult submit_res = VK_SUCCESS;
    {
        std::lock_guard<std::mutex> qlk(g_QueueSubmitMutex);
        submit_res = vkQueueSubmit(g_Queue, 1, &si, res->fence);
    }
    if (submit_res != VK_SUCCESS) {
        if (error) *error = "vkQueueSubmit failed";
        g_ctx.available = false;
        release_res();
        return false;
    }
    if (vkWaitForFences(g_Device, 1, &res->fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        if (error) *error = "vkWaitForFences after submit failed";
        g_ctx.available = false;
        release_res();
        return false;
    }

    const uint32_t* raw = reinterpret_cast<const uint32_t*>(res->accum_mapped);
    out_density.assign(pixel_count, 0.0f);
    out_cr.assign(pixel_count, 0.0f);
    out_cg.assign(pixel_count, 0.0f);
    out_cb.assign(pixel_count, 0.0f);
    out_cw.assign(pixel_count, 0.0f);
    out_gv.assign(pixel_count, 0.0f);
    out_sv.assign(pixel_count, 0.0f);

    for (size_t i = 0; i < pixel_count; ++i) {
        const size_t base = i * 7;
        const float d = (float)raw[base + 0] / 1024.0f;
        const float rw_sum = (float)raw[base + 1] / (255.0f * 1024.0f);
        const float gw_sum = (float)raw[base + 2] / (255.0f * 1024.0f);
        const float bw_sum = (float)raw[base + 3] / (255.0f * 1024.0f);
        const float cw = (float)raw[base + 4] / 1024.0f;
        out_density[i] = d;
        out_cr[i] = rw_sum;
        out_cg[i] = gw_sum;
        out_cb[i] = bw_sum;
        out_cw[i] = cw;
        out_gv[i] = (float)raw[base + 5];
        out_sv[i] = (float)raw[base + 6];
    }

    release_res();
    return true;
}

bool buildGpuSplatAggregateTexture(
    const std::vector<HeatSample>& group,
    int rw,
    int rh,
    float raster_min_lon,
    float raster_min_lat,
    float raster_max_lon,
    float raster_max_lat,
    float sigma_r,
    float percentile_clip,
    TileTexture& out_texture,
    std::string* error) {
    if (group.empty() || rw <= 0 || rh <= 0) return false;

    std::vector<GpuSamplePacked> packed;
    packed.reserve(group.size());
    for (const auto& s : group) {
        GpuSamplePacked p{};
        p.a[0] = s.lon;
        p.a[1] = s.lat;
        p.a[2] = s.color.x;
        p.a[3] = s.color.y;
        p.b[0] = s.color.z;
        p.b[1] = s.prefer_gradient ? 1.0f : 0.0f;
        packed.push_back(p);
    }

    const VkDeviceSize sample_bytes = (VkDeviceSize)packed.size() * sizeof(GpuSamplePacked);
    const size_t pixel_count = (size_t)rw * (size_t)rh;
    const VkDeviceSize accum_bytes = (VkDeviceSize)(pixel_count * 7) * sizeof(uint32_t);
    const VkDeviceSize stats_bytes = sizeof(uint32_t) * 2;
    const int layer_id = group.front().layer;
    std::shared_ptr<GpuLayerResources> res;
    TileTexture tex{};
    bool reusing_texture = false;
    {
        std::lock_guard<std::mutex> lk(g_gpu_mutex);
        if (!initCtx(error)) return false;
        if (!g_ctx.available) {
            if (error && error->empty()) *error = "GPU context unavailable";
            return false;
        }
        res = getOrCreateLayerResources(layer_id);
        tex.mip_levels = 1;
        reusing_texture = tryAcquirePooledAggregateTexture((uint32_t)rw, (uint32_t)rh, tex);
    }
    if (!res) {
        if (error) *error = "failed to obtain layer GPU resources";
        return false;
    }
    std::lock_guard<std::mutex> res_lk(res->op_mutex);
    auto release_res = [&]() { releaseLayerResources(res); };
    if (!ensurePersistentBuffer(
            sample_bytes,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            res->texture_sample_staging_buf,
            res->texture_sample_staging_mem,
            res->texture_sample_staging_mapped,
            res->texture_sample_staging_capacity,
            "texture sample staging",
            error) ||
        !ensurePersistentDeviceLocalBuffer(
            sample_bytes,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            res->texture_sample_device_buf,
            res->texture_sample_device_mem,
            res->texture_sample_device_capacity,
            "texture sample device",
            error) ||
        !ensurePersistentDeviceLocalBuffer(
            accum_bytes,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            res->texture_accum_device_buf,
            res->texture_accum_device_mem,
            res->texture_accum_device_capacity,
            "texture accum device",
            error) ||
        !ensurePersistentDeviceLocalBuffer(
            stats_bytes,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            res->texture_stats_device_buf,
            res->texture_stats_device_mem,
            res->texture_stats_device_capacity,
            "texture stats device",
            error) ||
        !ensurePersistentDeviceLocalBuffer(
            sizeof(uint32_t) * 256,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            res->texture_histogram_buf,
            res->texture_histogram_mem,
            res->texture_histogram_capacity,
            "texture histogram",
            error) ||
        !ensurePersistentDescriptors(*res, error) ||
        !ensurePersistentCommandBuffer(*res, error) ||
        !ensurePersistentFence(*res, error)) {
        release_res();
        return false;
    }

    std::memcpy(res->texture_sample_staging_mapped, packed.data(), (size_t)sample_bytes);
    if (!reusing_texture) {
        if (!createImage(
                (uint32_t)rw,
                (uint32_t)rh,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                tex.image,
                tex.memory)) {
            if (error) *error = "failed to create GPU aggregate texture image";
            release_res();
            return false;
        }
        tex.view = createImageView(tex.image);
        if (tex.view == VK_NULL_HANDLE) {
            if (error) *error = "failed to create GPU aggregate texture view";
            if (tex.image) vkDestroyImage(g_Device, tex.image, g_Allocator);
            if (tex.memory) vkFreeMemory(g_Device, tex.memory, g_Allocator);
            release_res();
            return false;
        }
    }

    VkDescriptorBufferInfo sbi{res->texture_sample_device_buf, 0, sample_bytes};
    VkDescriptorBufferInfo abi{res->texture_accum_device_buf, 0, accum_bytes};
    VkDescriptorBufferInfo tbi{res->texture_stats_device_buf, 0, stats_bytes};
    VkDescriptorBufferInfo hbi{res->texture_histogram_buf, 0, sizeof(uint32_t) * 256};
    VkDescriptorImageInfo iii{};
    iii.imageView = tex.view;
    iii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet w0{};
    w0.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w0.dstSet = res->desc_set;
    w0.dstBinding = 0;
    w0.descriptorCount = 1;
    w0.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w0.pBufferInfo = &sbi;

    VkWriteDescriptorSet w1 = w0;
    w1.dstBinding = 1;
    w1.pBufferInfo = &abi;

    VkWriteDescriptorSet w2 = w0;
    w2.dstBinding = 2;
    w2.pBufferInfo = &tbi;

    VkWriteDescriptorSet w3{};
    w3.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w3.dstSet = res->desc_set;
    w3.dstBinding = 3;
    w3.descriptorCount = 1;
    w3.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w3.pImageInfo = &iii;

    VkWriteDescriptorSet w4 = w0;
    w4.dstBinding = 4;
    w4.pBufferInfo = &hbi;

    std::array<VkWriteDescriptorSet, 5> writes{w0, w1, w2, w3, w4};
    vkUpdateDescriptorSets(g_Device, (uint32_t)writes.size(), writes.data(), 0, nullptr);

    if (vkWaitForFences(g_Device, 1, &res->fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS ||
        vkResetFences(g_Device, 1, &res->fence) != VK_SUCCESS ||
        vkResetCommandBuffer(res->cmd, 0) != VK_SUCCESS) {
        if (error) *error = "failed to prepare GPU aggregate command buffer";
        destroyTileTextureNow(tex);
        release_res();
        return false;
    }

    VkCommandBufferBeginInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(res->cmd, &cbi) != VK_SUCCESS) {
        if (error) *error = "vkBeginCommandBuffer failed";
        destroyTileTextureNow(tex);
        release_res();
        return false;
    }

    VkBufferCopy sample_copy{};
    sample_copy.size = sample_bytes;
    vkCmdCopyBuffer(res->cmd, res->texture_sample_staging_buf, res->texture_sample_device_buf, 1, &sample_copy);
    vkCmdFillBuffer(res->cmd, res->texture_accum_device_buf, 0, accum_bytes, 0);
    vkCmdFillBuffer(res->cmd, res->texture_stats_device_buf, 0, stats_bytes, 0);
    vkCmdFillBuffer(res->cmd, res->texture_histogram_buf, 0, sizeof(uint32_t) * 256, 0);

    std::array<VkBufferMemoryBarrier, 3> prep_barriers{};
    for (VkBufferMemoryBarrier& barrier : prep_barriers) {
        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    }
    prep_barriers[0].buffer = res->texture_sample_device_buf;
    prep_barriers[0].size = sample_bytes;
    prep_barriers[1].buffer = res->texture_accum_device_buf;
    prep_barriers[1].size = accum_bytes;
    prep_barriers[2].buffer = res->texture_stats_device_buf;
    prep_barriers[2].size = stats_bytes;
    vkCmdPipelineBarrier(
        res->cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        0,
        nullptr,
        (uint32_t)prep_barriers.size(),
        prep_barriers.data(),
        0,
        nullptr);

    transitionImage(
        res->cmd,
        tex.image,
        reusing_texture ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_GENERAL,
        reusing_texture ? VK_ACCESS_SHADER_READ_BIT : 0,
        VK_ACCESS_SHADER_WRITE_BIT,
        reusing_texture ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    struct BinPush {
        uint32_t sample_count;
        uint32_t width;
        uint32_t height;
        uint32_t radius;
        float min_lon;
        float min_lat;
        float max_lon;
        float max_lat;
        float sigma_r;
    } bin_push{};
    bin_push.sample_count = (uint32_t)packed.size();
    bin_push.width = (uint32_t)rw;
    bin_push.height = (uint32_t)rh;
    bin_push.radius = 0;
    bin_push.min_lon = raster_min_lon;
    bin_push.min_lat = raster_min_lat;
    bin_push.max_lon = raster_max_lon;
    bin_push.max_lat = raster_max_lat;
    bin_push.sigma_r = std::max(1.0f, sigma_r);

    vkCmdBindPipeline(res->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_ctx.bin_pipeline);
    vkCmdBindDescriptorSets(res->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_ctx.pipeline_layout, 0, 1, &res->desc_set, 0, nullptr);
    vkCmdPushConstants(res->cmd, g_ctx.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(BinPush), &bin_push);
    vkCmdDispatch(res->cmd, std::max(1u, (bin_push.sample_count + 63u) / 64u), 1, 1);

    std::array<VkBufferMemoryBarrier, 3> barriers{};
    barriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].buffer = res->texture_accum_device_buf;
    barriers[0].offset = 0;
    barriers[0].size = accum_bytes;
    barriers[1] = barriers[0];
    barriers[1].buffer = res->texture_stats_device_buf;
    barriers[1].size = stats_bytes;
    barriers[2] = barriers[0];
    barriers[2].buffer = res->texture_histogram_buf;
    barriers[2].size = sizeof(uint32_t) * 256;
    vkCmdPipelineBarrier(
        res->cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        0,
        nullptr,
        (uint32_t)barriers.size(),
        barriers.data(),
        0,
        nullptr);

    struct HistogramPush {
        uint32_t width;
        uint32_t height;
    } histogram_push{(uint32_t)rw, (uint32_t)rh};
    vkCmdBindPipeline(res->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_ctx.histogram_pipeline);
    vkCmdPushConstants(res->cmd, g_ctx.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(HistogramPush), &histogram_push);
    vkCmdDispatch(res->cmd, ((uint32_t)rw + 7u) / 8u, ((uint32_t)rh + 7u) / 8u, 1);

    std::array<VkBufferMemoryBarrier, 2> reduction_barriers{};
    for (VkBufferMemoryBarrier& barrier : reduction_barriers) {
        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    }
    reduction_barriers[0].buffer = res->texture_stats_device_buf;
    reduction_barriers[0].size = stats_bytes;
    reduction_barriers[1].buffer = res->texture_histogram_buf;
    reduction_barriers[1].size = sizeof(uint32_t) * 256;
    vkCmdPipelineBarrier(
        res->cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        0,
        nullptr,
        (uint32_t)reduction_barriers.size(),
        reduction_barriers.data(),
        0,
        nullptr);

    struct HistogramReducePush {
        float percentile_clip;
    } histogram_reduce_push{percentile_clip};
    vkCmdBindPipeline(res->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_ctx.histogram_reduce_pipeline);
    vkCmdPushConstants(
        res->cmd,
        g_ctx.pipeline_layout,
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        sizeof(HistogramReducePush),
        &histogram_reduce_push);
    vkCmdDispatch(res->cmd, 1, 1, 1);

    VkBufferMemoryBarrier resolve_prep_barrier{};
    resolve_prep_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    resolve_prep_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    resolve_prep_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    resolve_prep_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    resolve_prep_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    resolve_prep_barrier.buffer = res->texture_stats_device_buf;
    resolve_prep_barrier.offset = 0;
    resolve_prep_barrier.size = stats_bytes;
    vkCmdPipelineBarrier(
        res->cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        0,
        nullptr,
        1,
        &resolve_prep_barrier,
        0,
        nullptr);

    struct ResolvePush {
        uint32_t width;
        uint32_t height;
        uint32_t mode;
    } resolve_push{(uint32_t)rw, (uint32_t)rh, group.front().algo == kAggregateGpuSplatHue ? 1u : 0u};
    vkCmdBindPipeline(res->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_ctx.resolve_pipeline);
    vkCmdBindDescriptorSets(res->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_ctx.pipeline_layout, 0, 1, &res->desc_set, 0, nullptr);
    vkCmdPushConstants(res->cmd, g_ctx.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ResolvePush), &resolve_push);
    vkCmdDispatch(res->cmd, ((uint32_t)rw + 7u) / 8u, ((uint32_t)rh + 7u) / 8u, 1);

    transitionImage(
        res->cmd,
        tex.image,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    if (vkEndCommandBuffer(res->cmd) != VK_SUCCESS) {
        if (error) *error = "vkEndCommandBuffer failed";
        destroyTileTextureNow(tex);
        release_res();
        return false;
    }

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &res->cmd;
    VkResult submit_res = VK_SUCCESS;
    {
        std::lock_guard<std::mutex> qlk(g_QueueSubmitMutex);
        submit_res = vkQueueSubmit(g_Queue, 1, &si, res->fence);
    }
    if (submit_res != VK_SUCCESS || vkWaitForFences(g_Device, 1, &res->fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        if (error) *error = "GPU aggregate texture submit failed";
        destroyTileTextureNow(tex);
        g_ctx.available = false;
        release_res();
        return false;
    }

    destroyTileTexture(out_texture);
    out_texture = tex;
    release_res();
    return true;
}

bool recycleGpuAggregateTexture(TileTexture& texture, uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> lk(g_gpu_mutex);
    if (!texture.image || !texture.memory || !texture.view) return false;
    PooledAggregateTexture pooled;
    pooled.texture = texture;
    pooled.width = width;
    pooled.height = height;
    pooled.last_used_epoch = ++g_gpu_resource_epoch;
    g_pooled_aggregate_textures.push_back(std::move(pooled));
    prunePooledAggregateTextures();
    texture = {};
    return true;
}

void shutdownGpuSplatAggregate() {
    std::unique_lock<std::mutex> lk(g_gpu_mutex);
    if (!g_ctx.initialized) return;
    g_gpu_shutdown_requested = true;
    g_gpu_cv.wait(lk, []() {
        for (const auto& kv : g_layer_resources) {
            if (kv.second && kv.second->active_users != 0) return false;
        }
        return true;
    });
    if (g_Device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(g_Device);
    }
    shutdownCtx();
}
