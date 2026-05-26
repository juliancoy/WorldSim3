#include "worldsim_app_internal.h"

#include <algorithm>
#include <cstring>
#include <limits>

#if defined(WS3_GPU_PICK_POLYGON_VERT_SPV)
static const char* kGpuPickPolygonVertShaderPath = WS3_GPU_PICK_POLYGON_VERT_SPV;
#else
static const char* kGpuPickPolygonVertShaderPath = nullptr;
#endif

#if defined(WS3_GPU_PICK_POLYGON_FRAG_SPV)
static const char* kGpuPickPolygonFragShaderPath = WS3_GPU_PICK_POLYGON_FRAG_SPV;
#else
static const char* kGpuPickPolygonFragShaderPath = nullptr;
#endif

#if defined(WS3_GPU_PICK_POINT_VERT_SPV)
static const char* kGpuPickPointVertShaderPath = WS3_GPU_PICK_POINT_VERT_SPV;
#else
static const char* kGpuPickPointVertShaderPath = nullptr;
#endif

#if defined(WS3_GPU_PICK_POINT_FRAG_SPV)
static const char* kGpuPickPointFragShaderPath = WS3_GPU_PICK_POINT_FRAG_SPV;
#else
static const char* kGpuPickPointFragShaderPath = nullptr;
#endif

struct GpuPickPushConstants {
    float center_lonlat[2];
    float center_world[2];
    float viewport_origin[2];
    float viewport_size[2];
    float framebuffer_size[2];
    float math_zoom = 0.0f;
    float zoom_scale = 1.0f;
    float pick_screen[2];
    float marker_radius_px = 5.0f;
};

GpuPickResources g_GpuPickResources;

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

static void destroyGpuPickTarget() {
    if (g_GpuPickResources.target.framebuffer) {
        vkDestroyFramebuffer(g_Device, g_GpuPickResources.target.framebuffer, g_Allocator);
        g_GpuPickResources.target.framebuffer = VK_NULL_HANDLE;
    }
    if (g_GpuPickResources.target.view) {
        vkDestroyImageView(g_Device, g_GpuPickResources.target.view, g_Allocator);
        g_GpuPickResources.target.view = VK_NULL_HANDLE;
    }
    if (g_GpuPickResources.target.image) {
        vkDestroyImage(g_Device, g_GpuPickResources.target.image, g_Allocator);
        g_GpuPickResources.target.image = VK_NULL_HANDLE;
    }
    if (g_GpuPickResources.target.memory) {
        vkFreeMemory(g_Device, g_GpuPickResources.target.memory, g_Allocator);
        g_GpuPickResources.target.memory = VK_NULL_HANDLE;
    }
}

static void destroyGpuPickPipelines() {
    if (g_GpuPickResources.polygon_pipeline.pipeline) {
        vkDestroyPipeline(g_Device, g_GpuPickResources.polygon_pipeline.pipeline, g_Allocator);
        g_GpuPickResources.polygon_pipeline.pipeline = VK_NULL_HANDLE;
    }
    if (g_GpuPickResources.polygon_pipeline.pipeline_layout) {
        vkDestroyPipelineLayout(g_Device, g_GpuPickResources.polygon_pipeline.pipeline_layout, g_Allocator);
        g_GpuPickResources.polygon_pipeline.pipeline_layout = VK_NULL_HANDLE;
    }
    if (g_GpuPickResources.point_pipeline.pipeline) {
        vkDestroyPipeline(g_Device, g_GpuPickResources.point_pipeline.pipeline, g_Allocator);
        g_GpuPickResources.point_pipeline.pipeline = VK_NULL_HANDLE;
    }
    if (g_GpuPickResources.point_pipeline.pipeline_layout) {
        vkDestroyPipelineLayout(g_Device, g_GpuPickResources.point_pipeline.pipeline_layout, g_Allocator);
        g_GpuPickResources.point_pipeline.pipeline_layout = VK_NULL_HANDLE;
    }
}

static void clearGpuPickPointBuffers() {
    destroyGpuPickBuffer(g_GpuPickResources.point_buffers.positions);
    destroyGpuPickBuffer(g_GpuPickResources.point_buffers.feature_refs);
    g_GpuPickResources.point_buffers.feature_count = 0;
    g_GpuPickResources.point_buffers.source_signature.clear();
}

void destroyGpuPickResources() {
    clearGpuPickPointBuffers();
    destroyGpuPickPipelines();
    destroyGpuPickBuffer(g_GpuPickResources.readback);
    destroyGpuPickTarget();
    if (g_GpuPickResources.render_pass) {
        vkDestroyRenderPass(g_Device, g_GpuPickResources.render_pass, g_Allocator);
        g_GpuPickResources.render_pass = VK_NULL_HANDLE;
    }
    if (g_GpuPickResources.command_pool) {
        vkDestroyCommandPool(g_Device, g_GpuPickResources.command_pool, g_Allocator);
        g_GpuPickResources.command_pool = VK_NULL_HANDLE;
        g_GpuPickResources.command_buffer = VK_NULL_HANDLE;
    }
}

static bool ensureGpuPickRenderPass(std::string* error) {
    if (g_GpuPickResources.render_pass) return true;
    VkAttachmentDescription color{};
    color.format = VK_FORMAT_R32_UINT;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 1;
    info.pAttachments = &color;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &dep;
    if (vkCreateRenderPass(g_Device, &info, g_Allocator, &g_GpuPickResources.render_pass) != VK_SUCCESS) {
        if (error) *error = "vkCreateRenderPass failed for GPU pick";
        return false;
    }
    return true;
}

static bool ensureGpuPickTarget(std::string* error) {
    if (g_GpuPickResources.target.framebuffer &&
        g_GpuPickResources.target.view &&
        g_GpuPickResources.target.image &&
        g_GpuPickResources.readback.buffer) {
        return true;
    }
    if (!ensureGpuPickRenderPass(error)) return false;
    destroyGpuPickTarget();
    destroyGpuPickBuffer(g_GpuPickResources.readback);

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R32_UINT;
    image_info.extent = {1, 1, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(g_Device, &image_info, g_Allocator, &g_GpuPickResources.target.image) != VK_SUCCESS) {
        if (error) *error = "vkCreateImage failed for GPU pick target";
        return false;
    }
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(g_Device, g_GpuPickResources.target.image, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(g_Device, &alloc, g_Allocator, &g_GpuPickResources.target.memory) != VK_SUCCESS) {
        if (error) *error = "vkAllocateMemory failed for GPU pick target";
        destroyGpuPickTarget();
        return false;
    }
    check_vk_result(vkBindImageMemory(g_Device, g_GpuPickResources.target.image, g_GpuPickResources.target.memory, 0));

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = g_GpuPickResources.target.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R32_UINT;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    if (vkCreateImageView(g_Device, &view_info, g_Allocator, &g_GpuPickResources.target.view) != VK_SUCCESS) {
        if (error) *error = "vkCreateImageView failed for GPU pick target";
        destroyGpuPickTarget();
        return false;
    }

    VkFramebufferCreateInfo fb_info{};
    fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb_info.renderPass = g_GpuPickResources.render_pass;
    fb_info.attachmentCount = 1;
    fb_info.pAttachments = &g_GpuPickResources.target.view;
    fb_info.width = 1;
    fb_info.height = 1;
    fb_info.layers = 1;
    if (vkCreateFramebuffer(g_Device, &fb_info, g_Allocator, &g_GpuPickResources.target.framebuffer) != VK_SUCCESS) {
        if (error) *error = "vkCreateFramebuffer failed for GPU pick target";
        destroyGpuPickTarget();
        return false;
    }

    if (!createGpuPickBuffer(
            sizeof(uint32_t),
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            g_GpuPickResources.readback,
            error)) {
        destroyGpuPickTarget();
        return false;
    }
    return true;
}

static bool ensureGpuPickCommandBuffer(std::string* error) {
    if (g_GpuPickResources.command_buffer && g_GpuPickResources.command_pool) return true;
    if (g_GpuPickResources.command_pool) {
        vkDestroyCommandPool(g_Device, g_GpuPickResources.command_pool, g_Allocator);
        g_GpuPickResources.command_pool = VK_NULL_HANDLE;
        g_GpuPickResources.command_buffer = VK_NULL_HANDLE;
    }
    VkCommandPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.queueFamilyIndex = g_QueueFamily;
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    if (vkCreateCommandPool(g_Device, &pool, g_Allocator, &g_GpuPickResources.command_pool) != VK_SUCCESS) {
        if (error) *error = "vkCreateCommandPool failed for GPU pick";
        return false;
    }
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = g_GpuPickResources.command_pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(g_Device, &alloc, &g_GpuPickResources.command_buffer) != VK_SUCCESS) {
        if (error) *error = "vkAllocateCommandBuffers failed for GPU pick";
        vkDestroyCommandPool(g_Device, g_GpuPickResources.command_pool, g_Allocator);
        g_GpuPickResources.command_pool = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

static bool ensureGpuPickPolygonPipeline(std::string* error) {
    if (g_GpuPickResources.polygon_pipeline.pipeline) return true;
    if (!ensureGpuPickRenderPass(error)) return false;
    const std::vector<uint32_t> vert_code = loadSpirvFile(kGpuPickPolygonVertShaderPath);
    const std::vector<uint32_t> frag_code = loadSpirvFile(kGpuPickPolygonFragShaderPath);
    if (vert_code.empty() || frag_code.empty()) {
        if (error) *error = "GPU pick polygon shader SPIR-V is unavailable";
        return false;
    }
    VkShaderModuleCreateInfo shader_info{};
    shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shader_info.codeSize = vert_code.size() * sizeof(uint32_t);
    shader_info.pCode = vert_code.data();
    VkShaderModule vert = VK_NULL_HANDLE;
    if (vkCreateShaderModule(g_Device, &shader_info, g_Allocator, &vert) != VK_SUCCESS) {
        if (error) *error = "vkCreateShaderModule failed for GPU pick polygon vertex shader";
        return false;
    }
    shader_info.codeSize = frag_code.size() * sizeof(uint32_t);
    shader_info.pCode = frag_code.data();
    VkShaderModule frag = VK_NULL_HANDLE;
    if (vkCreateShaderModule(g_Device, &shader_info, g_Allocator, &frag) != VK_SUCCESS) {
        vkDestroyShaderModule(g_Device, vert, g_Allocator);
        if (error) *error = "vkCreateShaderModule failed for GPU pick polygon fragment shader";
        return false;
    }

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push_range.size = sizeof(GpuPickPushConstants);
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;
    if (vkCreatePipelineLayout(g_Device, &layout_info, g_Allocator, &g_GpuPickResources.polygon_pipeline.pipeline_layout) != VK_SUCCESS) {
        vkDestroyShaderModule(g_Device, vert, g_Allocator);
        vkDestroyShaderModule(g_Device, frag, g_Allocator);
        if (error) *error = "vkCreatePipelineLayout failed for GPU pick polygon pipeline";
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
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
    attrs[1].location = 1;
    attrs[1].binding = 1;
    attrs[1].format = VK_FORMAT_R32_UINT;
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
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo msaa{};
    msaa.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    msaa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
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
    pipeline_info.layout = g_GpuPickResources.polygon_pipeline.pipeline_layout;
    pipeline_info.renderPass = g_GpuPickResources.render_pass;
    pipeline_info.subpass = 0;
    const VkResult result = vkCreateGraphicsPipelines(
        g_Device, VK_NULL_HANDLE, 1, &pipeline_info, g_Allocator, &g_GpuPickResources.polygon_pipeline.pipeline);
    vkDestroyShaderModule(g_Device, vert, g_Allocator);
    vkDestroyShaderModule(g_Device, frag, g_Allocator);
    if (result != VK_SUCCESS || !g_GpuPickResources.polygon_pipeline.pipeline) {
        if (error) *error = "vkCreateGraphicsPipelines failed for GPU pick polygon pipeline";
        if (g_GpuPickResources.polygon_pipeline.pipeline_layout) {
            vkDestroyPipelineLayout(g_Device, g_GpuPickResources.polygon_pipeline.pipeline_layout, g_Allocator);
            g_GpuPickResources.polygon_pipeline.pipeline_layout = VK_NULL_HANDLE;
        }
        return false;
    }
    return true;
}

static bool ensureGpuPickPointPipeline(std::string* error) {
    if (g_GpuPickResources.point_pipeline.pipeline) return true;
    if (!ensureGpuPickRenderPass(error)) return false;
    const std::vector<uint32_t> vert_code = loadSpirvFile(kGpuPickPointVertShaderPath);
    const std::vector<uint32_t> frag_code = loadSpirvFile(kGpuPickPointFragShaderPath);
    if (vert_code.empty() || frag_code.empty()) {
        if (error) *error = "GPU pick point shader SPIR-V is unavailable";
        return false;
    }
    VkShaderModuleCreateInfo shader_info{};
    shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shader_info.codeSize = vert_code.size() * sizeof(uint32_t);
    shader_info.pCode = vert_code.data();
    VkShaderModule vert = VK_NULL_HANDLE;
    if (vkCreateShaderModule(g_Device, &shader_info, g_Allocator, &vert) != VK_SUCCESS) {
        if (error) *error = "vkCreateShaderModule failed for GPU pick point vertex shader";
        return false;
    }
    shader_info.codeSize = frag_code.size() * sizeof(uint32_t);
    shader_info.pCode = frag_code.data();
    VkShaderModule frag = VK_NULL_HANDLE;
    if (vkCreateShaderModule(g_Device, &shader_info, g_Allocator, &frag) != VK_SUCCESS) {
        vkDestroyShaderModule(g_Device, vert, g_Allocator);
        if (error) *error = "vkCreateShaderModule failed for GPU pick point fragment shader";
        return false;
    }

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push_range.size = sizeof(GpuPickPushConstants);
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;
    if (vkCreatePipelineLayout(g_Device, &layout_info, g_Allocator, &g_GpuPickResources.point_pipeline.pipeline_layout) != VK_SUCCESS) {
        vkDestroyShaderModule(g_Device, vert, g_Allocator);
        vkDestroyShaderModule(g_Device, frag, g_Allocator);
        if (error) *error = "vkCreatePipelineLayout failed for GPU pick point pipeline";
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";
    VkVertexInputBindingDescription bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].stride = sizeof(ImVec2);
    bindings[0].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
    bindings[1].binding = 1;
    bindings[1].stride = sizeof(uint32_t);
    bindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0].location = 0;
    attrs[0].binding = 0;
    attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[1].location = 1;
    attrs[1].binding = 1;
    attrs[1].format = VK_FORMAT_R32_UINT;
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
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo msaa{};
    msaa.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    msaa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
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
    pipeline_info.layout = g_GpuPickResources.point_pipeline.pipeline_layout;
    pipeline_info.renderPass = g_GpuPickResources.render_pass;
    pipeline_info.subpass = 0;
    const VkResult result = vkCreateGraphicsPipelines(
        g_Device, VK_NULL_HANDLE, 1, &pipeline_info, g_Allocator, &g_GpuPickResources.point_pipeline.pipeline);
    vkDestroyShaderModule(g_Device, vert, g_Allocator);
    vkDestroyShaderModule(g_Device, frag, g_Allocator);
    if (result != VK_SUCCESS || !g_GpuPickResources.point_pipeline.pipeline) {
        if (error) *error = "vkCreateGraphicsPipelines failed for GPU pick point pipeline";
        if (g_GpuPickResources.point_pipeline.pipeline_layout) {
            vkDestroyPipelineLayout(g_Device, g_GpuPickResources.point_pipeline.pipeline_layout, g_Allocator);
            g_GpuPickResources.point_pipeline.pipeline_layout = VK_NULL_HANDLE;
        }
        return false;
    }
    return true;
}

static bool ensureGpuPickPointBuffers(
    const PointGeometryArtifact& artifact,
    const std::string& source_signature,
    std::string* error) {
    if (g_GpuPickResources.point_buffers.source_signature == source_signature &&
        g_GpuPickResources.point_buffers.feature_count == artifact.positions.size() &&
        g_GpuPickResources.point_buffers.positions.buffer &&
        g_GpuPickResources.point_buffers.feature_refs.buffer) {
        return true;
    }
    clearGpuPickPointBuffers();
    if (artifact.positions.empty() || artifact.feature_refs.size() != artifact.positions.size()) {
        if (error) *error = "point geometry artifact is invalid for GPU picking";
        return false;
    }
    if (!createGpuPickBuffer(
            sizeof(ImVec2) * artifact.positions.size(),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            g_GpuPickResources.point_buffers.positions,
            error) ||
        !createGpuPickBuffer(
            sizeof(uint32_t) * artifact.feature_refs.size(),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            g_GpuPickResources.point_buffers.feature_refs,
            error)) {
        clearGpuPickPointBuffers();
        return false;
    }

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    const VkDeviceSize positions_size = sizeof(ImVec2) * artifact.positions.size();
    const VkDeviceSize refs_size = sizeof(uint32_t) * artifact.feature_refs.size();
    const VkDeviceSize total_size = positions_size + refs_size;
    if (!tryCreateBuffer(
            total_size,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            staging,
            staging_mem,
            error)) {
        clearGpuPickPointBuffers();
        return false;
    }
    void* mapped = nullptr;
    if (vkMapMemory(g_Device, staging_mem, 0, total_size, 0, &mapped) != VK_SUCCESS) {
        if (error) *error = "failed to map point GPU pick staging buffer";
        vkDestroyBuffer(g_Device, staging, g_Allocator);
        vkFreeMemory(g_Device, staging_mem, g_Allocator);
        clearGpuPickPointBuffers();
        return false;
    }
    std::memcpy(mapped, artifact.positions.data(), (size_t)positions_size);
    std::memcpy(static_cast<uint8_t*>(mapped) + positions_size, artifact.feature_refs.data(), (size_t)refs_size);
    vkUnmapMemory(g_Device, staging_mem);

    if (!ensureGpuPickCommandBuffer(error)) {
        vkDestroyBuffer(g_Device, staging, g_Allocator);
        vkFreeMemory(g_Device, staging_mem, g_Allocator);
        clearGpuPickPointBuffers();
        return false;
    }
    check_vk_result(vkResetCommandPool(g_Device, g_GpuPickResources.command_pool, 0));
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk_result(vkBeginCommandBuffer(g_GpuPickResources.command_buffer, &begin));
    VkBufferCopy copy_positions{0, 0, positions_size};
    vkCmdCopyBuffer(
        g_GpuPickResources.command_buffer,
        staging,
        g_GpuPickResources.point_buffers.positions.buffer,
        1,
        &copy_positions);
    VkBufferCopy copy_refs{positions_size, 0, refs_size};
    vkCmdCopyBuffer(
        g_GpuPickResources.command_buffer,
        staging,
        g_GpuPickResources.point_buffers.feature_refs.buffer,
        1,
        &copy_refs);
    check_vk_result(vkEndCommandBuffer(g_GpuPickResources.command_buffer));
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &g_GpuPickResources.command_buffer;
    {
        std::lock_guard<std::mutex> qlk(g_QueueSubmitMutex);
        check_vk_result(vkQueueSubmit(g_Queue, 1, &submit, VK_NULL_HANDLE));
        check_vk_result(vkQueueWaitIdle(g_Queue));
    }
    vkDestroyBuffer(g_Device, staging, g_Allocator);
    vkFreeMemory(g_Device, staging_mem, g_Allocator);
    g_GpuPickResources.point_buffers.feature_count = (uint32_t)artifact.positions.size();
    g_GpuPickResources.point_buffers.source_signature = source_signature;
    return true;
}

static void fillGpuPickPushConstants(const GpuPickRequest& request, GpuPickPushConstants& push) {
    push.center_lonlat[0] = request.center_lonlat.x;
    push.center_lonlat[1] = request.center_lonlat.y;
    push.center_world[0] = request.center_world.x;
    push.center_world[1] = request.center_world.y;
    push.viewport_origin[0] = request.viewport_origin.x;
    push.viewport_origin[1] = request.viewport_origin.y;
    push.viewport_size[0] = request.viewport_size.x;
    push.viewport_size[1] = request.viewport_size.y;
    push.framebuffer_size[0] = std::max(1.0f, request.framebuffer_size.x);
    push.framebuffer_size[1] = std::max(1.0f, request.framebuffer_size.y);
    push.math_zoom = (float)request.math_zoom;
    push.zoom_scale = request.zoom_scale;
    push.pick_screen[0] = request.mouse_screen.x;
    push.pick_screen[1] = request.mouse_screen.y;
    push.marker_radius_px = request.marker_radius_px;
}

static bool gpuPickReadbackFeatureRef(uint32_t* out_feature_ref, std::string* error) {
    if (!out_feature_ref) {
        if (error) *error = "GPU pick output pointer is missing";
        return false;
    }
    if (!ensureGpuPickTarget(error) || !ensureGpuPickCommandBuffer(error)) return false;
    check_vk_result(vkResetCommandPool(g_Device, g_GpuPickResources.command_pool, 0));
    *out_feature_ref = std::numeric_limits<uint32_t>::max();
    return true;
}

static bool executeGpuPickPass(
    const GpuPickPushConstants& push,
    VkPipelineLayout pipeline_layout,
    VkPipeline pipeline,
    const VkBuffer* vertex_buffers,
    const VkDeviceSize* offsets,
    uint32_t vertex_buffer_count,
    VkBuffer index_buffer,
    uint32_t index_count,
    uint32_t instance_count,
    bool indexed,
    std::string* error,
    uint32_t* out_feature_ref) {
    if (!gpuPickReadbackFeatureRef(out_feature_ref, error)) return false;

    VkCommandBuffer cmd = g_GpuPickResources.command_buffer;
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk_result(vkBeginCommandBuffer(cmd, &begin));

    VkImageMemoryBarrier to_attachment{};
    to_attachment.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_attachment.srcAccessMask = 0;
    to_attachment.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    to_attachment.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_attachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_attachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_attachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_attachment.image = g_GpuPickResources.target.image;
    to_attachment.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_attachment.subresourceRange.levelCount = 1;
    to_attachment.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &to_attachment);

    VkClearValue clear{};
    clear.color.uint32[0] = std::numeric_limits<uint32_t>::max();
    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = g_GpuPickResources.render_pass;
    rp.framebuffer = g_GpuPickResources.target.framebuffer;
    rp.renderArea.extent = {1, 1};
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.width = 1.0f;
    viewport.height = 1.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent = {1, 1};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
    vkCmdBindVertexBuffers(cmd, 0, vertex_buffer_count, vertex_buffers, offsets);
    if (indexed) {
        vkCmdBindIndexBuffer(cmd, index_buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, index_count, instance_count, 0, 0, 0);
    } else {
        vkCmdDraw(cmd, 6, instance_count, 0, 0);
    }
    vkCmdEndRenderPass(cmd);

    VkImageMemoryBarrier to_transfer{};
    to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_transfer.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_transfer.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.image = g_GpuPickResources.target.image;
    to_transfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_transfer.subresourceRange.levelCount = 1;
    to_transfer.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &to_transfer);

    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {1, 1, 1};
    vkCmdCopyImageToBuffer(
        cmd,
        g_GpuPickResources.target.image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        g_GpuPickResources.readback.buffer,
        1,
        &copy);
    check_vk_result(vkEndCommandBuffer(cmd));

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    {
        std::lock_guard<std::mutex> qlk(g_QueueSubmitMutex);
        check_vk_result(vkQueueSubmit(g_Queue, 1, &submit, VK_NULL_HANDLE));
        check_vk_result(vkQueueWaitIdle(g_Queue));
    }

    void* mapped = nullptr;
    if (vkMapMemory(g_Device, g_GpuPickResources.readback.memory, 0, sizeof(uint32_t), 0, &mapped) != VK_SUCCESS) {
        if (error) *error = "failed to map GPU pick readback buffer";
        return false;
    }
    *out_feature_ref = *static_cast<const uint32_t*>(mapped);
    vkUnmapMemory(g_Device, g_GpuPickResources.readback.memory);
    return true;
}

bool gpuPickParcelFeature(
    const GpuPickRequest& request,
    size_t* out_feature_idx,
    std::string* out_entity_id,
    std::string* error) {
    if (!out_feature_idx) {
        if (error) *error = "GPU pick parcel output pointer is missing";
        return false;
    }
    *out_feature_idx = (size_t)-1;
    if (out_entity_id) out_entity_id->clear();
    if (!g_ParcelGpuBuffers.positions.buffer ||
        !g_ParcelGpuBuffers.vertex_feature_refs.buffer ||
        !g_ParcelGpuBuffers.indices.buffer ||
        g_ParcelGpuBuffers.indices_count == 0) {
        if (error) *error = "parcel GPU buffers are not resident for picking";
        return false;
    }
    if (!ensureGpuPickPolygonPipeline(error)) return false;
    GpuPickPushConstants push{};
    fillGpuPickPushConstants(request, push);
    const VkBuffer vertex_buffers[] = {
        g_ParcelGpuBuffers.positions.buffer,
        g_ParcelGpuBuffers.vertex_feature_refs.buffer
    };
    const VkDeviceSize offsets[] = {0, 0};
    uint32_t feature_ref = std::numeric_limits<uint32_t>::max();
    if (!executeGpuPickPass(
            push,
            g_GpuPickResources.polygon_pipeline.pipeline_layout,
            g_GpuPickResources.polygon_pipeline.pipeline,
            vertex_buffers,
            offsets,
            2,
            g_ParcelGpuBuffers.indices.buffer,
            g_ParcelGpuBuffers.indices_count,
            1,
            true,
            error,
            &feature_ref)) {
        return false;
    }
    if (feature_ref == std::numeric_limits<uint32_t>::max()) return true;
    if (feature_ref >= g_ParcelGpuBuffers.features.size()) {
        if (error) *error = "parcel GPU pick returned an out-of-range render feature reference";
        return false;
    }
    *out_feature_idx = static_cast<size_t>(g_ParcelGpuBuffers.features[feature_ref].feature_idx);
    if (out_entity_id) *out_entity_id = g_ParcelGpuBuffers.features[feature_ref].entity_id;
    return true;
}

bool gpuPickZoningFeature(
    size_t layer_idx,
    const GpuPickRequest& request,
    size_t* out_feature_idx,
    std::string* out_entity_id,
    std::string* error) {
    if (!out_feature_idx) {
        if (error) *error = "GPU pick zoning output pointer is missing";
        return false;
    }
    *out_feature_idx = (size_t)-1;
    if (out_entity_id) out_entity_id->clear();
    auto it = g_ZoningGpuLayers.find(layer_idx);
    if (it == g_ZoningGpuLayers.end()) {
        if (error) *error = "zoning GPU buffers are not resident for picking";
        return false;
    }
    ZoningGpuLayerState& layer_state = it->second;
    if (!layer_state.buffers.positions.buffer ||
        !layer_state.buffers.vertex_feature_refs.buffer ||
        !layer_state.buffers.indices.buffer ||
        layer_state.buffers.indices_count == 0) {
        if (error) *error = "zoning GPU buffers are incomplete for picking";
        return false;
    }
    if (!ensureGpuPickPolygonPipeline(error)) return false;
    GpuPickPushConstants push{};
    fillGpuPickPushConstants(request, push);
    const VkBuffer vertex_buffers[] = {
        layer_state.buffers.positions.buffer,
        layer_state.buffers.vertex_feature_refs.buffer
    };
    const VkDeviceSize offsets[] = {0, 0};
    uint32_t feature_ref = std::numeric_limits<uint32_t>::max();
    if (!executeGpuPickPass(
            push,
            g_GpuPickResources.polygon_pipeline.pipeline_layout,
            g_GpuPickResources.polygon_pipeline.pipeline,
            vertex_buffers,
            offsets,
            2,
            layer_state.buffers.indices.buffer,
            layer_state.buffers.indices_count,
            1,
            true,
            error,
            &feature_ref)) {
        return false;
    }
    if (feature_ref == std::numeric_limits<uint32_t>::max()) return true;
    *out_feature_idx = (size_t)feature_ref;
    if (out_entity_id && feature_ref < layer_state.buffers.features.size()) {
        *out_entity_id = layer_state.buffers.features[feature_ref].entity_id;
    }
    return true;
}

bool gpuPickPointFeature(
    const PointGeometryArtifact& artifact,
    const std::string& source_signature,
    const GpuPickRequest& request,
    size_t* out_feature_idx,
    std::string* error) {
    if (!out_feature_idx) {
        if (error) *error = "GPU pick point output pointer is missing";
        return false;
    }
    *out_feature_idx = (size_t)-1;
    if (!ensureGpuPickPointPipeline(error) || !ensureGpuPickPointBuffers(artifact, source_signature, error)) {
        return false;
    }
    GpuPickPushConstants push{};
    fillGpuPickPushConstants(request, push);
    const VkBuffer vertex_buffers[] = {
        g_GpuPickResources.point_buffers.positions.buffer,
        g_GpuPickResources.point_buffers.feature_refs.buffer
    };
    const VkDeviceSize offsets[] = {0, 0};
    uint32_t feature_ref = std::numeric_limits<uint32_t>::max();
    if (!executeGpuPickPass(
            push,
            g_GpuPickResources.point_pipeline.pipeline_layout,
            g_GpuPickResources.point_pipeline.pipeline,
            vertex_buffers,
            offsets,
            2,
            VK_NULL_HANDLE,
            0,
            g_GpuPickResources.point_buffers.feature_count,
            false,
            error,
            &feature_ref)) {
        return false;
    }
    if (feature_ref == std::numeric_limits<uint32_t>::max()) return true;
    *out_feature_idx = (size_t)feature_ref;
    return true;
}
