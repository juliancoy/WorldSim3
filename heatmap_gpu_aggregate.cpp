#include "heatmap_gpu_aggregate.h"

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
#include <mutex>
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

struct GpuSamplePacked {
    float a[4]; // lon, lat, r, g
    float b[4]; // b, prefer_gradient, pad, pad
};

struct GpuCtx {
    bool initialized = false;
    bool available = false;
    VkDescriptorSetLayout desc_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool = VK_NULL_HANDLE;
    VkDescriptorSet desc_set = VK_NULL_HANDLE;
    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkBuffer sample_buf = VK_NULL_HANDLE;
    VkDeviceMemory sample_mem = VK_NULL_HANDLE;
    void* sample_mapped = nullptr;
    VkDeviceSize sample_capacity = 0;
    VkBuffer accum_buf = VK_NULL_HANDLE;
    VkDeviceMemory accum_mem = VK_NULL_HANDLE;
    void* accum_mapped = nullptr;
    VkDeviceSize accum_capacity = 0;
};

GpuCtx g_ctx;
std::mutex g_gpu_mutex;

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

std::vector<uint32_t> loadOrBuildSpirv(std::string* error) {
    const std::filesystem::path output_path = runtimeShaderOutputPath();
    auto code = loadSpirv(output_path.c_str());
    if (!code.empty()) return code;

    if (!kShaderSourcePath || !*kShaderSourcePath) {
        if (error) *error = "GPU splat shader source path unavailable";
        std::cerr << "[worldsim3] GPU splat shader SPIR-V missing and no shader source path was compiled in.\n";
        return {};
    }

    const std::filesystem::path source_path(kShaderSourcePath);
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

void shutdownCtx() {
    if (g_Device != VK_NULL_HANDLE) {
        if (g_ctx.sample_mapped && g_ctx.sample_mem) vkUnmapMemory(g_Device, g_ctx.sample_mem);
        if (g_ctx.accum_mapped && g_ctx.accum_mem) vkUnmapMemory(g_Device, g_ctx.accum_mem);
        if (g_ctx.sample_buf) vkDestroyBuffer(g_Device, g_ctx.sample_buf, g_Allocator);
        if (g_ctx.sample_mem) vkFreeMemory(g_Device, g_ctx.sample_mem, g_Allocator);
        if (g_ctx.accum_buf) vkDestroyBuffer(g_Device, g_ctx.accum_buf, g_Allocator);
        if (g_ctx.accum_mem) vkFreeMemory(g_Device, g_ctx.accum_mem, g_Allocator);
    }
    if (g_ctx.pipeline) vkDestroyPipeline(g_Device, g_ctx.pipeline, g_Allocator);
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

bool ensurePersistentDescriptors(std::string* error) {
    if (g_ctx.desc_set != VK_NULL_HANDLE) return true;
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = g_ctx.desc_pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &g_ctx.desc_layout;
    if (vkAllocateDescriptorSets(g_Device, &dsai, &g_ctx.desc_set) != VK_SUCCESS) {
        if (error) *error = "failed to allocate persistent descriptor set";
        return false;
    }
    return true;
}

bool ensurePersistentCommandBuffer(std::string* error) {
    if (g_ctx.cmd != VK_NULL_HANDLE) return true;
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = g_ctx.cmd_pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(g_Device, &cbai, &g_ctx.cmd) != VK_SUCCESS) {
        if (error) *error = "failed to allocate persistent command buffer";
        return false;
    }
    return true;
}

bool initCtx(std::string* error) {
    if (g_ctx.initialized) return g_ctx.available;
    g_ctx.initialized = true;

    const char* env_disable = std::getenv("WS3_DISABLE_EXPERIMENTAL_GPU_AGGREGATE");
    if (env_disable && std::strcmp(env_disable, "1") == 0) {
        if (error) *error = "GPU aggregate disabled by WS3_DISABLE_EXPERIMENTAL_GPU_AGGREGATE=1";
        return false;
    }

    if (g_Device == VK_NULL_HANDLE || g_PhysicalDevice == VK_NULL_HANDLE || g_Queue == VK_NULL_HANDLE) {
        if (error) *error = "Vulkan device/queue unavailable";
        return false;
    }
    auto spirv = loadOrBuildSpirv(error);
    if (spirv.empty()) {
        if (error && error->empty()) *error = "GPU splat shader SPIR-V unavailable";
        return false;
    }

    VkShaderModuleCreateInfo smi{};
    smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = spirv.size() * sizeof(uint32_t);
    smi.pCode = spirv.data();
    VkShaderModule shader = VK_NULL_HANDLE;
    if (vkCreateShaderModule(g_Device, &smi, g_Allocator, &shader) != VK_SUCCESS) {
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

    std::array<VkDescriptorSetLayoutBinding, 2> bindings{b0, b1};
    VkDescriptorSetLayoutCreateInfo dli{};
    dli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dli.bindingCount = (uint32_t)bindings.size();
    dli.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(g_Device, &dli, g_Allocator, &g_ctx.desc_layout) != VK_SUCCESS) {
        vkDestroyShaderModule(g_Device, shader, g_Allocator);
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
        vkDestroyShaderModule(g_Device, shader, g_Allocator);
        if (error) *error = "vkCreatePipelineLayout failed";
        shutdownCtx();
        return false;
    }

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shader;
    stage.pName = "main";

    VkComputePipelineCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage = stage;
    cpi.layout = g_ctx.pipeline_layout;
    if (vkCreateComputePipelines(g_Device, VK_NULL_HANDLE, 1, &cpi, g_Allocator, &g_ctx.pipeline) != VK_SUCCESS) {
        vkDestroyShaderModule(g_Device, shader, g_Allocator);
        if (error) *error = "vkCreateComputePipelines failed";
        shutdownCtx();
        return false;
    }
    vkDestroyShaderModule(g_Device, shader, g_Allocator);

    VkDescriptorPoolSize ps{};
    ps.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ps.descriptorCount = 4;
    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.maxSets = 2;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
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
    std::lock_guard<std::mutex> lk(g_gpu_mutex);

    if (!initCtx(error)) return false;
    if (!g_ctx.available) {
        if (error && error->empty()) *error = "GPU context unavailable";
        return false;
    }
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

    if (!ensurePersistentBuffer(
            sample_bytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            g_ctx.sample_buf,
            g_ctx.sample_mem,
            g_ctx.sample_mapped,
            g_ctx.sample_capacity,
            "sample",
            error)) {
        return false;
    }
    if (!ensurePersistentBuffer(
            accum_bytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            g_ctx.accum_buf,
            g_ctx.accum_mem,
            g_ctx.accum_mapped,
            g_ctx.accum_capacity,
            "accum",
            error)) {
        return false;
    }
    if (!ensurePersistentDescriptors(error) || !ensurePersistentCommandBuffer(error)) return false;

    std::memcpy(g_ctx.sample_mapped, packed.data(), (size_t)sample_bytes);
    std::memset(g_ctx.accum_mapped, 0, (size_t)accum_bytes);

    VkDescriptorBufferInfo sbi{};
    sbi.buffer = g_ctx.sample_buf;
    sbi.offset = 0;
    sbi.range = sample_bytes;

    VkDescriptorBufferInfo abi{};
    abi.buffer = g_ctx.accum_buf;
    abi.offset = 0;
    abi.range = accum_bytes;

    VkWriteDescriptorSet w0{};
    w0.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w0.dstSet = g_ctx.desc_set;
    w0.dstBinding = 0;
    w0.descriptorCount = 1;
    w0.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w0.pBufferInfo = &sbi;

    VkWriteDescriptorSet w1 = w0;
    w1.dstBinding = 1;
    w1.pBufferInfo = &abi;

    std::array<VkWriteDescriptorSet, 2> writes{w0, w1};
    vkUpdateDescriptorSets(g_Device, (uint32_t)writes.size(), writes.data(), 0, nullptr);

    if (vkResetCommandBuffer(g_ctx.cmd, 0) != VK_SUCCESS) {
        if (error) *error = "vkResetCommandBuffer failed";
        return false;
    }
    VkCommandBufferBeginInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(g_ctx.cmd, &cbi) != VK_SUCCESS) {
        if (error) *error = "vkBeginCommandBuffer failed";
        return false;
    }

    vkCmdBindPipeline(g_ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_ctx.pipeline);
    vkCmdBindDescriptorSets(g_ctx.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_ctx.pipeline_layout, 0, 1, &g_ctx.desc_set, 0, nullptr);

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

    vkCmdPushConstants(g_ctx.cmd, g_ctx.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushRaw), &push);

    const uint32_t wg = 64;
    const uint32_t groups = (push.sample_count + wg - 1) / wg;
    vkCmdDispatch(g_ctx.cmd, std::max(1u, groups), 1, 1);

    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = g_ctx.accum_buf;
    barrier.offset = 0;
    barrier.size = accum_bytes;
    vkCmdPipelineBarrier(
        g_ctx.cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT,
        0,
        0,
        nullptr,
        1,
        &barrier,
        0,
        nullptr);

    if (vkEndCommandBuffer(g_ctx.cmd) != VK_SUCCESS) {
        if (error) *error = "vkEndCommandBuffer failed";
        return false;
    }

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_ctx.cmd;
    VkResult submit_res = VK_SUCCESS;
    {
        std::lock_guard<std::mutex> qlk(g_QueueSubmitMutex);
        submit_res = vkQueueSubmit(g_Queue, 1, &si, VK_NULL_HANDLE);
    }
    if (submit_res != VK_SUCCESS) {
        if (error) *error = "vkQueueSubmit failed";
        g_ctx.available = false;
        return false;
    }
    VkResult idle_res = VK_SUCCESS;
    {
        std::lock_guard<std::mutex> qlk(g_QueueSubmitMutex);
        idle_res = vkQueueWaitIdle(g_Queue);
    }
    if (idle_res != VK_SUCCESS) {
        if (error) *error = "vkQueueWaitIdle failed";
        g_ctx.available = false;
        return false;
    }

    const uint32_t* raw = reinterpret_cast<const uint32_t*>(g_ctx.accum_mapped);
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

    return true;
}

void shutdownGpuSplatAggregate() {
    std::lock_guard<std::mutex> lk(g_gpu_mutex);
    if (!g_ctx.initialized) return;
    if (g_Device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(g_Device);
    }
    shutdownCtx();
}
