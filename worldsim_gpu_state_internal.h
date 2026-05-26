#pragma once

#include "cache_io.h"
#include "layer_geometry.h"

#include "imgui.h"

#include <array>
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

struct ParcelGpuBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size_bytes = 0;
};

struct ZoningOutlineFeatureGpuRecord {
    uint32_t feature_idx = 0;
    uint32_t vertex_offset = 0;
    uint32_t vertex_count = 0;
    uint32_t index_offset = 0;
    uint32_t index_count = 0;
    uint32_t line_index_offset = 0;
    uint32_t line_index_count = 0;
    uint32_t pad0 = 0;
    float min_lon = 0.0f;
    float min_lat = 0.0f;
    float max_lon = 0.0f;
    float max_lat = 0.0f;
};

struct ParcelGpuBuffers {
    ParcelGpuBuffer positions;
    ParcelGpuBuffer indices;
    ParcelGpuBuffer line_indices;
    ParcelGpuBuffer vertex_feature_refs;
    ParcelGpuBuffer colors;
    ParcelGpuBuffer overlay_colors;
    ParcelGpuBuffer outline_colors;
    ParcelGpuBuffer outline_feature_records;
    std::vector<ParcelRenderFeatureRecord> features;
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
    ImVec2 center_lonlat = ImVec2(0.0f, 0.0f);
    ImVec2 center_world = ImVec2(0.0f, 0.0f);
    ImVec2 viewport_origin = ImVec2(0.0f, 0.0f);
    ImVec2 viewport_size = ImVec2(0.0f, 0.0f);
    ImVec2 framebuffer_size = ImVec2(0.0f, 0.0f);
    std::vector<ParcelGpuDrawChunk> visible_chunks;
    std::vector<ParcelGpuLineDrawChunk> visible_line_chunks;
};

struct ParcelGpuPipeline {
    VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
    std::vector<std::array<VkDescriptorSet, 3>> descriptor_sets_by_frame;
    std::vector<bool> descriptor_dirty_by_frame;
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

struct CrimePointGpuBuffers {
    ParcelGpuBuffer positions;
    ParcelGpuBuffer feature_refs;
    ParcelGpuBuffer colors;
    ParcelGpuBuffer glyph_codes;
    uint32_t render_features = 0;
    std::string source_signature;
};

struct PointLayerGpuBuffers {
    ParcelGpuBuffer positions;
    ParcelGpuBuffer feature_refs;
    ParcelGpuBuffer colors;
    ParcelGpuBuffer glyph_codes;
    uint32_t render_features = 0;
    std::string source_signature;
};

struct PolylineLayerGpuBuffers {
    ParcelGpuBuffer positions;
    ParcelGpuBuffer feature_refs;
    ParcelGpuBuffer line_indices;
    ParcelGpuBuffer colors;
    std::vector<GeometryArtifactChunkRecord> chunks;
    uint32_t render_features = 0;
    uint32_t vertices = 0;
    uint32_t line_indices_count = 0;
    std::string source_signature;
};

struct PointGpuLayerDescriptors {
    std::vector<VkDescriptorSet> descriptor_sets_by_frame;
    std::vector<bool> descriptor_dirty_by_frame;
    bool descriptor_dirty = true;
};

struct PolylineGpuLayerDescriptors {
    std::vector<std::array<VkDescriptorSet, 3>> descriptor_sets_by_frame;
    std::vector<bool> descriptor_dirty_by_frame;
    bool descriptor_dirty = true;
};

struct CrimePointGpuDrawState {
    bool active = false;
    int math_zoom = 0;
    float zoom_scale = 1.0f;
    ImVec2 center_world = ImVec2(0.0f, 0.0f);
    ImVec2 viewport_origin = ImVec2(0.0f, 0.0f);
    ImVec2 viewport_size = ImVec2(0.0f, 0.0f);
    ImVec2 framebuffer_size = ImVec2(0.0f, 0.0f);
};

struct CrimePointGpuPipeline {
    VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> descriptor_sets_by_frame;
    std::vector<bool> descriptor_dirty_by_frame;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    bool descriptor_dirty = true;
};

struct GpuPickBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size_bytes = 0;
};

struct GpuPickImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
};

struct GpuPickPolygonPipeline {
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

struct GpuPickPointPipeline {
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

struct GpuPickPointBuffers {
    GpuPickBuffer positions;
    GpuPickBuffer feature_refs;
    uint32_t feature_count = 0;
    std::string source_signature;
};

struct GpuPickResources {
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    GpuPickImage target;
    GpuPickBuffer readback;
    GpuPickPolygonPipeline polygon_pipeline;
    GpuPickPointPipeline point_pipeline;
    GpuPickPointBuffers point_buffers;
};

struct CrimePointGpuPushConstants {
    float center_world[2];
    float viewport_origin[2];
    float viewport_size[2];
    float framebuffer_size[2];
    float math_zoom;
    float zoom_scale;
    float marker_radius_px;
};

struct ZoningGpuLayerDescriptors {
    std::vector<std::array<VkDescriptorSet, 3>> descriptor_sets_by_frame;
    std::vector<bool> descriptor_dirty_by_frame;
    bool descriptor_dirty = true;
};

struct ZoningGpuLayerState {
    ParcelGpuBuffers buffers;
    bool outline_has_visible_colors = false;
    std::vector<ImU32> outline_colors_cpu;
    std::vector<ParcelGpuBuffer> outline_indirect_commands_by_frame;
    std::vector<uint32_t> outline_indirect_capacity_by_frame;
    std::vector<uint32_t> outline_indirect_count_by_frame;
    std::vector<VkDescriptorSet> outline_compute_descriptor_sets_by_frame;
    std::vector<bool> outline_compute_descriptor_dirty_by_frame;
    bool outline_compute_descriptor_dirty = true;
    bool outline_compute_pending = false;
    float outline_compute_view_min_lon = 0.0f;
    float outline_compute_view_min_lat = 0.0f;
    float outline_compute_view_max_lon = 0.0f;
    float outline_compute_view_max_lat = 0.0f;
    std::vector<VkDrawIndexedIndirectCommand> outline_indirect_scratch;
    ParcelGpuDrawState draw_state;
    ZoningGpuLayerDescriptors descriptors;
};

struct ZoningOutlineIndirectComputePipeline {
    VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

struct ZoningOutlineIndirectPushConstants {
    uint32_t feature_count = 0;
    float view_min_lon = 0.0f;
    float view_min_lat = 0.0f;
    float view_max_lon = 0.0f;
    float view_max_lat = 0.0f;
};

struct ParcelGpuUploadPayload {
    ParcelGpuBuffers buffers;
};

struct RetiredParcelGpuPayload {
    ParcelGpuBuffers buffers;
    uint64_t retire_after_frame = 0;
};

struct GpuProfilerAlertState {
    bool oom_active = false;
    uint64_t oom_generation = 0;
    bool tab_selection_requested = false;
    std::string last_error;
};

struct GpuProfilerEvent {
    std::chrono::steady_clock::time_point at{};
    std::string label;
};

struct ParcelGpuUploadResult {
    bool ok = false;
    std::string source_signature;
    std::string error;
    ParcelGpuUploadPayload payload;
};
