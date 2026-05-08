#include "heatmap_gpu_aggregate.h"

#include "worldsim_app_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <vector>

namespace {

#if defined(WS3_GPU_SPLAT_SHADER_SPV)
static const char* kShaderPath = WS3_GPU_SPLAT_SHADER_SPV;
#else
static const char* kShaderPath = nullptr;
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
    VkCommandPool cmd_pool = VK_NULL_HANDLE;
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

void shutdownCtx() {
    if (g_ctx.pipeline) vkDestroyPipeline(g_Device, g_ctx.pipeline, g_Allocator);
    if (g_ctx.pipeline_layout) vkDestroyPipelineLayout(g_Device, g_ctx.pipeline_layout, g_Allocator);
    if (g_ctx.desc_layout) vkDestroyDescriptorSetLayout(g_Device, g_ctx.desc_layout, g_Allocator);
    if (g_ctx.desc_pool) vkDestroyDescriptorPool(g_Device, g_ctx.desc_pool, g_Allocator);
    if (g_ctx.cmd_pool) vkDestroyCommandPool(g_Device, g_ctx.cmd_pool, g_Allocator);
    g_ctx = {};
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
    auto spirv = loadSpirv(kShaderPath);
    if (spirv.empty()) {
        if (error) *error = "GPU splat shader SPIR-V unavailable";
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

    VkBuffer sample_buf = VK_NULL_HANDLE;
    VkDeviceMemory sample_mem = VK_NULL_HANDLE;
    VkBuffer accum_buf = VK_NULL_HANDLE;
    VkDeviceMemory accum_mem = VK_NULL_HANDLE;

    auto cleanup = [&]() {
        if (sample_buf) vkDestroyBuffer(g_Device, sample_buf, g_Allocator);
        if (sample_mem) vkFreeMemory(g_Device, sample_mem, g_Allocator);
        if (accum_buf) vkDestroyBuffer(g_Device, accum_buf, g_Allocator);
        if (accum_mem) vkFreeMemory(g_Device, accum_mem, g_Allocator);
    };

    if (!createBuffer(sample_bytes,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      sample_buf,
                      sample_mem)) {
        if (error) *error = "failed to create sample buffer";
        cleanup();
        return false;
    }
    if (!createBuffer(accum_bytes,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      accum_buf,
                      accum_mem)) {
        if (error) *error = "failed to create accum buffer";
        cleanup();
        return false;
    }

    void* sample_ptr = nullptr;
    if (vkMapMemory(g_Device, sample_mem, 0, sample_bytes, 0, &sample_ptr) != VK_SUCCESS) {
        if (error) *error = "failed to map sample buffer";
        cleanup();
        return false;
    }
    std::memcpy(sample_ptr, packed.data(), (size_t)sample_bytes);
    vkUnmapMemory(g_Device, sample_mem);

    void* accum_ptr = nullptr;
    if (vkMapMemory(g_Device, accum_mem, 0, accum_bytes, 0, &accum_ptr) != VK_SUCCESS) {
        if (error) *error = "failed to map accum buffer";
        cleanup();
        return false;
    }
    std::memset(accum_ptr, 0, (size_t)accum_bytes);
    vkUnmapMemory(g_Device, accum_mem);

    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = g_ctx.desc_pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &g_ctx.desc_layout;
    VkDescriptorSet desc = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(g_Device, &dsai, &desc) != VK_SUCCESS) {
        vkResetDescriptorPool(g_Device, g_ctx.desc_pool, 0);
        if (vkAllocateDescriptorSets(g_Device, &dsai, &desc) != VK_SUCCESS) {
            if (error) *error = "failed to allocate descriptor set";
            cleanup();
            return false;
        }
    }

    VkDescriptorBufferInfo sbi{};
    sbi.buffer = sample_buf;
    sbi.offset = 0;
    sbi.range = sample_bytes;

    VkDescriptorBufferInfo abi{};
    abi.buffer = accum_buf;
    abi.offset = 0;
    abi.range = accum_bytes;

    VkWriteDescriptorSet w0{};
    w0.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w0.dstSet = desc;
    w0.dstBinding = 0;
    w0.descriptorCount = 1;
    w0.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w0.pBufferInfo = &sbi;

    VkWriteDescriptorSet w1 = w0;
    w1.dstBinding = 1;
    w1.pBufferInfo = &abi;

    std::array<VkWriteDescriptorSet, 2> writes{w0, w1};
    vkUpdateDescriptorSets(g_Device, (uint32_t)writes.size(), writes.data(), 0, nullptr);

    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = g_ctx.cmd_pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(g_Device, &cbai, &cmd) != VK_SUCCESS) {
        if (error) *error = "failed to allocate command buffer";
        cleanup();
        return false;
    }

    VkCommandBufferBeginInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &cbi) != VK_SUCCESS) {
        if (error) *error = "vkBeginCommandBuffer failed";
        vkFreeCommandBuffers(g_Device, g_ctx.cmd_pool, 1, &cmd);
        cleanup();
        return false;
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_ctx.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_ctx.pipeline_layout, 0, 1, &desc, 0, nullptr);

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

    vkCmdPushConstants(cmd, g_ctx.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushRaw), &push);

    const uint32_t wg = 64;
    const uint32_t groups = (push.sample_count + wg - 1) / wg;
    vkCmdDispatch(cmd, std::max(1u, groups), 1, 1);

    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = accum_buf;
    barrier.offset = 0;
    barrier.size = accum_bytes;
    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT,
        0,
        0,
        nullptr,
        1,
        &barrier,
        0,
        nullptr);

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        if (error) *error = "vkEndCommandBuffer failed";
        vkFreeCommandBuffers(g_Device, g_ctx.cmd_pool, 1, &cmd);
        cleanup();
        return false;
    }

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VkResult submit_res = VK_SUCCESS;
    {
        std::lock_guard<std::mutex> qlk(g_QueueSubmitMutex);
        submit_res = vkQueueSubmit(g_Queue, 1, &si, VK_NULL_HANDLE);
    }
    if (submit_res != VK_SUCCESS) {
        if (error) *error = "vkQueueSubmit failed";
        vkFreeCommandBuffers(g_Device, g_ctx.cmd_pool, 1, &cmd);
        g_ctx.available = false;
        cleanup();
        return false;
    }
    VkResult idle_res = VK_SUCCESS;
    {
        std::lock_guard<std::mutex> qlk(g_QueueSubmitMutex);
        idle_res = vkQueueWaitIdle(g_Queue);
    }
    if (idle_res != VK_SUCCESS) {
        if (error) *error = "vkQueueWaitIdle failed";
        vkFreeCommandBuffers(g_Device, g_ctx.cmd_pool, 1, &cmd);
        g_ctx.available = false;
        cleanup();
        return false;
    }
    vkFreeCommandBuffers(g_Device, g_ctx.cmd_pool, 1, &cmd);

    if (vkMapMemory(g_Device, accum_mem, 0, accum_bytes, 0, &accum_ptr) != VK_SUCCESS) {
        if (error) *error = "failed to map accum readback";
        cleanup();
        return false;
    }

    const uint32_t* raw = reinterpret_cast<const uint32_t*>(accum_ptr);
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

    vkUnmapMemory(g_Device, accum_mem);
    cleanup();
    return true;
}
