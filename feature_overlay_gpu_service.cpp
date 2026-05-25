#include "worldsim_app_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>

CrimePointGpuBuffers g_CrimePointGpuBuffers;
std::unordered_map<size_t, PointLayerGpuBuffers> g_PointGpuLayers;
std::unordered_map<size_t, PolylineLayerGpuBuffers> g_PolylineGpuLayers;
std::unordered_map<size_t, CrimePointGpuDrawState> g_PointGpuLayerDrawStates;
std::unordered_map<size_t, PointGpuLayerDescriptors> g_PointGpuLayerDescriptors;
CrimePointGpuPipeline g_PointGpuPipeline;
std::unordered_map<size_t, ParcelGpuDrawState> g_PolylineGpuLayerDrawStates;
std::unordered_map<size_t, PolylineGpuLayerDescriptors> g_PolylineGpuLayerDescriptors;
ParcelGpuPipeline g_PolylineGpuPipeline;
CrimePointGpuDrawState g_CrimePointGpuDrawState;
CrimePointGpuPipeline g_CrimePointGpuPipeline;

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

struct ParcelGpuPushConstants {
    float center_world[2];
    float viewport_origin[2];
    float viewport_size[2];
    float framebuffer_size[2];
    float math_zoom = 0.0f;
    float zoom_scale = 1.0f;
};

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

static void destroyPointLayerGpuBuffers(PointLayerGpuBuffers& buffers) {
    destroyParcelGpuBuffer(buffers.positions);
    destroyParcelGpuBuffer(buffers.feature_refs);
    destroyParcelGpuBuffer(buffers.colors);
    destroyParcelGpuBuffer(buffers.glyph_codes);
    buffers.render_features = 0;
    buffers.source_signature.clear();
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

static void destroyPolylineLayerGpuPipeline() {
    if (g_PolylineGpuPipeline.fill_pipeline) {
        vkDestroyPipeline(g_Device, g_PolylineGpuPipeline.fill_pipeline, g_Allocator);
        g_PolylineGpuPipeline.fill_pipeline = VK_NULL_HANDLE;
    }
    if (g_PolylineGpuPipeline.line_pipeline) {
        vkDestroyPipeline(g_Device, g_PolylineGpuPipeline.line_pipeline, g_Allocator);
        g_PolylineGpuPipeline.line_pipeline = VK_NULL_HANDLE;
    }
    if (g_PolylineGpuPipeline.pipeline_layout) {
        vkDestroyPipelineLayout(g_Device, g_PolylineGpuPipeline.pipeline_layout, g_Allocator);
        g_PolylineGpuPipeline.pipeline_layout = VK_NULL_HANDLE;
    }
    if (g_PolylineGpuPipeline.descriptor_set_layout) {
        vkDestroyDescriptorSetLayout(g_Device, g_PolylineGpuPipeline.descriptor_set_layout, g_Allocator);
        g_PolylineGpuPipeline.descriptor_set_layout = VK_NULL_HANDLE;
    }
    g_PolylineGpuPipeline.descriptor_sets_by_frame.clear();
    g_PolylineGpuPipeline.descriptor_dirty_by_frame.clear();
    g_PolylineGpuPipeline.render_pass = VK_NULL_HANDLE;
    g_PolylineGpuPipeline.descriptor_dirty = true;
}

void destroyCrimePointGpuPipeline() {
    if (g_CrimePointGpuPipeline.pipeline) {
        vkDestroyPipeline(g_Device, g_CrimePointGpuPipeline.pipeline, g_Allocator);
        g_CrimePointGpuPipeline.pipeline = VK_NULL_HANDLE;
    }
    if (g_CrimePointGpuPipeline.pipeline_layout) {
        vkDestroyPipelineLayout(g_Device, g_CrimePointGpuPipeline.pipeline_layout, g_Allocator);
        g_CrimePointGpuPipeline.pipeline_layout = VK_NULL_HANDLE;
    }
    if (g_CrimePointGpuPipeline.descriptor_set_layout) {
        vkDestroyDescriptorSetLayout(g_Device, g_CrimePointGpuPipeline.descriptor_set_layout, g_Allocator);
        g_CrimePointGpuPipeline.descriptor_set_layout = VK_NULL_HANDLE;
    }
    g_CrimePointGpuPipeline.descriptor_sets_by_frame.clear();
    g_CrimePointGpuPipeline.descriptor_dirty_by_frame.clear();
    g_CrimePointGpuPipeline.render_pass = VK_NULL_HANDLE;
    g_CrimePointGpuPipeline.descriptor_dirty = true;
}

void destroyPointLayerGpuPipeline() {
    if (g_PointGpuPipeline.pipeline) {
        vkDestroyPipeline(g_Device, g_PointGpuPipeline.pipeline, g_Allocator);
        g_PointGpuPipeline.pipeline = VK_NULL_HANDLE;
    }
    if (g_PointGpuPipeline.pipeline_layout) {
        vkDestroyPipelineLayout(g_Device, g_PointGpuPipeline.pipeline_layout, g_Allocator);
        g_PointGpuPipeline.pipeline_layout = VK_NULL_HANDLE;
    }
    if (g_PointGpuPipeline.descriptor_set_layout) {
        vkDestroyDescriptorSetLayout(g_Device, g_PointGpuPipeline.descriptor_set_layout, g_Allocator);
        g_PointGpuPipeline.descriptor_set_layout = VK_NULL_HANDLE;
    }
    g_PointGpuPipeline.descriptor_sets_by_frame.clear();
    g_PointGpuPipeline.descriptor_dirty_by_frame.clear();
    g_PointGpuPipeline.render_pass = VK_NULL_HANDLE;
    g_PointGpuPipeline.descriptor_dirty = true;
}

bool ensureCrimePointGpuBuffersResident(
    const std::string& source_signature,
    const std::vector<ImVec2>& lonlat_positions,
    std::string* error) {
    if (!g_Device || !g_UploadCommandBuffer) {
        if (error) *error = "Vulkan device/upload command buffer is not ready";
        return false;
    }
    if (lonlat_positions.empty()) {
        if (error) *error = "crime point GPU positions are empty";
        return false;
    }
    if (g_CrimePointGpuBuffers.source_signature == source_signature &&
        g_CrimePointGpuBuffers.render_features == lonlat_positions.size() &&
        g_CrimePointGpuBuffers.positions.buffer &&
        g_CrimePointGpuBuffers.feature_refs.buffer &&
        g_CrimePointGpuBuffers.colors.buffer &&
        g_CrimePointGpuBuffers.glyph_codes.buffer) {
        return true;
    }
    clearCrimePointGpuBuffers();
    std::vector<uint32_t> refs(lonlat_positions.size());
    for (uint32_t i = 0; i < refs.size(); ++i) refs[i] = i;
    const VkDeviceSize positions_size = sizeof(ImVec2) * lonlat_positions.size();
    const VkDeviceSize refs_size = sizeof(uint32_t) * refs.size();
    const VkDeviceSize colors_size = sizeof(ImU32) * lonlat_positions.size();
    const VkDeviceSize glyphs_size = sizeof(uint32_t) * lonlat_positions.size();
    if (!uploadDeviceLocalParcelBuffer(
            lonlat_positions.data(),
            positions_size,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            g_CrimePointGpuBuffers.positions,
            error) ||
        !uploadDeviceLocalParcelBuffer(
            refs.data(),
            refs_size,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            g_CrimePointGpuBuffers.feature_refs,
            error) ||
        !createHostVisibleParcelBuffer(
            colors_size,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            g_CrimePointGpuBuffers.colors,
            error) ||
        !createHostVisibleParcelBuffer(
            glyphs_size,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            g_CrimePointGpuBuffers.glyph_codes,
            error)) {
        clearCrimePointGpuBuffers();
        return false;
    }
    std::vector<ImU32> default_colors(lonlat_positions.size(), IM_COL32(0, 0, 0, 0));
    std::vector<uint32_t> default_glyphs(lonlat_positions.size(), 0u);
    std::memcpy(g_CrimePointGpuBuffers.colors.mapped, default_colors.data(), (size_t)colors_size);
    std::memcpy(g_CrimePointGpuBuffers.glyph_codes.mapped, default_glyphs.data(), (size_t)glyphs_size);
    g_CrimePointGpuBuffers.render_features = (uint32_t)lonlat_positions.size();
    g_CrimePointGpuBuffers.source_signature = source_signature;
    g_CrimePointGpuPipeline.descriptor_dirty = true;
    return true;
}

bool updateCrimePointGpuColorBuffer(const std::vector<ImU32>& colors_rgba, std::string* error) {
    if (!g_CrimePointGpuBuffers.colors.mapped || g_CrimePointGpuBuffers.render_features == 0) {
        if (error) *error = "crime point GPU color buffer is not resident";
        return false;
    }
    if (colors_rgba.size() != g_CrimePointGpuBuffers.render_features) {
        if (error) *error = "crime point GPU color buffer size mismatch";
        return false;
    }
    std::memcpy(g_CrimePointGpuBuffers.colors.mapped, colors_rgba.data(), colors_rgba.size() * sizeof(ImU32));
    return true;
}

bool updateCrimePointGpuGlyphBuffer(const std::vector<uint32_t>& glyph_codes, std::string* error) {
    if (!g_CrimePointGpuBuffers.glyph_codes.mapped || g_CrimePointGpuBuffers.render_features == 0) {
        if (error) *error = "crime point GPU glyph buffer is not resident";
        return false;
    }
    if (glyph_codes.size() != g_CrimePointGpuBuffers.render_features) {
        if (error) *error = "crime point GPU glyph buffer size mismatch";
        return false;
    }
    std::memcpy(g_CrimePointGpuBuffers.glyph_codes.mapped, glyph_codes.data(), glyph_codes.size() * sizeof(uint32_t));
    return true;
}

void clearCrimePointGpuBuffers() {
    if (!g_CrimePointGpuBuffers.positions.buffer &&
        !g_CrimePointGpuBuffers.feature_refs.buffer &&
        !g_CrimePointGpuBuffers.colors.buffer &&
        !g_CrimePointGpuBuffers.glyph_codes.buffer) {
        return;
    }
    waitForParcelGpuDeviceIdle();
    destroyParcelGpuBuffer(g_CrimePointGpuBuffers.positions);
    destroyParcelGpuBuffer(g_CrimePointGpuBuffers.feature_refs);
    destroyParcelGpuBuffer(g_CrimePointGpuBuffers.colors);
    destroyParcelGpuBuffer(g_CrimePointGpuBuffers.glyph_codes);
    g_CrimePointGpuBuffers = CrimePointGpuBuffers{};
    g_CrimePointGpuPipeline.descriptor_dirty = true;
    clearCrimePointGpuDrawState();
}

bool ensurePointLayerGpuBuffersResident(
    size_t layer_idx,
    const PointGeometryArtifact& artifact,
    std::string* error) {
    if (!g_Device || !g_UploadCommandBuffer) {
        if (error) *error = "Vulkan device/upload command buffer is not ready";
        return false;
    }
    if (artifact.positions.empty() ||
        artifact.feature_refs.size() != artifact.positions.size() ||
        artifact.features.empty()) {
        if (error) *error = "point geometry artifact is incomplete";
        return false;
    }
    PointLayerGpuBuffers& layer_buffers = g_PointGpuLayers[layer_idx];
    if (layer_buffers.source_signature == artifact.header.source_signature &&
        layer_buffers.render_features == artifact.positions.size() &&
        layer_buffers.positions.buffer &&
        layer_buffers.feature_refs.buffer &&
        layer_buffers.colors.buffer &&
        layer_buffers.glyph_codes.buffer) {
        return true;
    }

    PointLayerGpuBuffers uploaded;
    const VkDeviceSize positions_size = sizeof(ImVec2) * artifact.positions.size();
    const VkDeviceSize refs_size = sizeof(uint32_t) * artifact.feature_refs.size();
    const VkDeviceSize colors_size = sizeof(ImU32) * artifact.features.size();
    const VkDeviceSize glyphs_size = sizeof(uint32_t) * artifact.features.size();
    if (!uploadDeviceLocalParcelBuffer(
            artifact.positions.data(),
            positions_size,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            uploaded.positions,
            error) ||
        !uploadDeviceLocalParcelBuffer(
            artifact.feature_refs.data(),
            refs_size,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            uploaded.feature_refs,
            error) ||
        !createHostVisibleParcelBuffer(colors_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, uploaded.colors, error) ||
        !createHostVisibleParcelBuffer(glyphs_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, uploaded.glyph_codes, error)) {
        destroyPointLayerGpuBuffers(uploaded);
        return false;
    }

    std::vector<ImU32> default_colors(artifact.features.size(), IM_COL32(0, 0, 0, 0));
    std::vector<uint32_t> default_glyphs(artifact.features.size(), 0u);
    std::memcpy(uploaded.colors.mapped, default_colors.data(), static_cast<size_t>(colors_size));
    std::memcpy(uploaded.glyph_codes.mapped, default_glyphs.data(), static_cast<size_t>(glyphs_size));
    uploaded.render_features = static_cast<uint32_t>(artifact.positions.size());
    uploaded.source_signature = artifact.header.source_signature;

    clearPointLayerGpuBuffers(layer_idx);
    g_PointGpuLayers[layer_idx] = std::move(uploaded);
    return true;
}

void clearPointLayerGpuBuffers(size_t layer_idx) {
    auto it = g_PointGpuLayers.find(layer_idx);
    if (it == g_PointGpuLayers.end()) return;
    waitForParcelGpuDeviceIdle();
    destroyPointLayerGpuBuffers(it->second);
    g_PointGpuLayers.erase(it);
    g_PointGpuLayerDescriptors.erase(layer_idx);
    g_PointGpuLayerDrawStates.erase(layer_idx);
}

void clearAllPointLayerGpuBuffers() {
    waitForParcelGpuDeviceIdle();
    for (auto& kv : g_PointGpuLayers) destroyPointLayerGpuBuffers(kv.second);
    g_PointGpuLayers.clear();
    g_PointGpuLayerDescriptors.clear();
    g_PointGpuLayerDrawStates.clear();
}

bool pointLayerGpuBuffersResident(size_t layer_idx) {
    auto it = g_PointGpuLayers.find(layer_idx);
    if (it == g_PointGpuLayers.end()) return false;
    const PointLayerGpuBuffers& layer_buffers = it->second;
    return layer_buffers.positions.buffer &&
        layer_buffers.feature_refs.buffer &&
        layer_buffers.colors.buffer &&
        layer_buffers.glyph_codes.buffer &&
        layer_buffers.render_features > 0;
}

bool ensurePolylineLayerGpuBuffersResident(
    size_t layer_idx,
    const PolylineGeometryArtifact& artifact,
    std::string* error) {
    if (!g_Device || !g_UploadCommandBuffer) {
        if (error) *error = "Vulkan device/upload command buffer is not ready";
        return false;
    }
    if (artifact.vertices.empty() ||
        artifact.feature_refs.size() != artifact.vertices.size() ||
        artifact.line_indices.empty() ||
        artifact.features.empty()) {
        if (error) *error = "polyline geometry artifact is incomplete";
        return false;
    }
    PolylineLayerGpuBuffers& layer_buffers = g_PolylineGpuLayers[layer_idx];
    if (layer_buffers.source_signature == artifact.header.source_signature &&
        layer_buffers.vertices == artifact.vertices.size() &&
        layer_buffers.line_indices_count == artifact.line_indices.size() &&
        layer_buffers.render_features == artifact.features.size() &&
        layer_buffers.positions.buffer &&
        layer_buffers.feature_refs.buffer &&
        layer_buffers.line_indices.buffer &&
        layer_buffers.colors.buffer) {
        return true;
    }

    PolylineLayerGpuBuffers uploaded;
    const VkDeviceSize positions_size = sizeof(ImVec2) * artifact.vertices.size();
    const VkDeviceSize refs_size = sizeof(uint32_t) * artifact.feature_refs.size();
    const VkDeviceSize line_indices_size = sizeof(uint32_t) * artifact.line_indices.size();
    const VkDeviceSize colors_size = sizeof(ImU32) * artifact.features.size();
    if (!uploadDeviceLocalParcelBuffer(
            artifact.vertices.data(),
            positions_size,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            uploaded.positions,
            error) ||
        !uploadDeviceLocalParcelBuffer(
            artifact.feature_refs.data(),
            refs_size,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            uploaded.feature_refs,
            error) ||
        !uploadDeviceLocalParcelBuffer(
            artifact.line_indices.data(),
            line_indices_size,
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            uploaded.line_indices,
            error) ||
        !createHostVisibleParcelBuffer(colors_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, uploaded.colors, error)) {
        destroyPolylineLayerGpuBuffers(uploaded);
        return false;
    }

    std::vector<ImU32> default_colors(artifact.features.size(), IM_COL32(0, 0, 0, 0));
    std::memcpy(uploaded.colors.mapped, default_colors.data(), static_cast<size_t>(colors_size));
    uploaded.chunks = artifact.chunks;
    uploaded.render_features = static_cast<uint32_t>(artifact.features.size());
    uploaded.vertices = static_cast<uint32_t>(artifact.vertices.size());
    uploaded.line_indices_count = static_cast<uint32_t>(artifact.line_indices.size());
    uploaded.source_signature = artifact.header.source_signature;

    clearPolylineLayerGpuBuffers(layer_idx);
    g_PolylineGpuLayers[layer_idx] = std::move(uploaded);
    return true;
}

void clearPolylineLayerGpuBuffers(size_t layer_idx) {
    auto it = g_PolylineGpuLayers.find(layer_idx);
    if (it == g_PolylineGpuLayers.end()) return;
    waitForParcelGpuDeviceIdle();
    destroyPolylineLayerGpuBuffers(it->second);
    g_PolylineGpuLayers.erase(it);
    g_PolylineGpuLayerDescriptors.erase(layer_idx);
    g_PolylineGpuLayerDrawStates.erase(layer_idx);
}

void clearAllPolylineLayerGpuBuffers() {
    waitForParcelGpuDeviceIdle();
    for (auto& kv : g_PolylineGpuLayers) destroyPolylineLayerGpuBuffers(kv.second);
    g_PolylineGpuLayers.clear();
    g_PolylineGpuLayerDescriptors.clear();
    g_PolylineGpuLayerDrawStates.clear();
}

bool polylineLayerGpuBuffersResident(size_t layer_idx) {
    auto it = g_PolylineGpuLayers.find(layer_idx);
    if (it == g_PolylineGpuLayers.end()) return false;
    const PolylineLayerGpuBuffers& layer_buffers = it->second;
    return layer_buffers.positions.buffer &&
        layer_buffers.feature_refs.buffer &&
        layer_buffers.line_indices.buffer &&
        layer_buffers.colors.buffer &&
        layer_buffers.render_features > 0 &&
        layer_buffers.line_indices_count > 0;
}

bool updatePointLayerGpuColorBuffer(size_t layer_idx, const std::vector<ImU32>& colors_rgba, std::string* error) {
    auto it = g_PointGpuLayers.find(layer_idx);
    if (it == g_PointGpuLayers.end() || !it->second.colors.mapped || it->second.render_features == 0) {
        if (error) *error = "point layer GPU color buffer is not resident";
        return false;
    }
    if (colors_rgba.size() != it->second.render_features) {
        if (error) *error = "point layer GPU color buffer size mismatch";
        return false;
    }
    std::memcpy(it->second.colors.mapped, colors_rgba.data(), colors_rgba.size() * sizeof(ImU32));
    g_PointGpuLayerDescriptors[layer_idx].descriptor_dirty = true;
    return true;
}

bool updatePointLayerGpuGlyphBuffer(size_t layer_idx, const std::vector<uint32_t>& glyph_codes, std::string* error) {
    auto it = g_PointGpuLayers.find(layer_idx);
    if (it == g_PointGpuLayers.end() || !it->second.glyph_codes.mapped || it->second.render_features == 0) {
        if (error) *error = "point layer GPU glyph buffer is not resident";
        return false;
    }
    if (glyph_codes.size() != it->second.render_features) {
        if (error) *error = "point layer GPU glyph buffer size mismatch";
        return false;
    }
    std::memcpy(it->second.glyph_codes.mapped, glyph_codes.data(), glyph_codes.size() * sizeof(uint32_t));
    g_PointGpuLayerDescriptors[layer_idx].descriptor_dirty = true;
    return true;
}

bool updatePolylineLayerGpuColorBuffer(size_t layer_idx, const std::vector<ImU32>& colors_rgba, std::string* error) {
    auto it = g_PolylineGpuLayers.find(layer_idx);
    if (it == g_PolylineGpuLayers.end() || !it->second.colors.mapped || it->second.render_features == 0) {
        if (error) *error = "polyline layer GPU color buffer is not resident";
        return false;
    }
    if (colors_rgba.size() != it->second.render_features) {
        if (error) *error = "polyline layer GPU color buffer size mismatch";
        return false;
    }
    std::memcpy(it->second.colors.mapped, colors_rgba.data(), colors_rgba.size() * sizeof(ImU32));
    g_PolylineGpuLayerDescriptors[layer_idx].descriptor_dirty = true;
    return true;
}

static bool ensurePointLayerGpuDescriptorSetsAllocated(size_t layer_idx, std::string* error) {
    auto buf_it = g_PointGpuLayers.find(layer_idx);
    if (!g_Device || !g_DescriptorPool || buf_it == g_PointGpuLayers.end() ||
        !buf_it->second.colors.buffer || !buf_it->second.glyph_codes.buffer) {
        if (error) *error = "point layer GPU descriptor prerequisites are not ready";
        return false;
    }
    if (!g_PointGpuPipeline.descriptor_set_layout) {
        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        VkDescriptorSetLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.bindingCount = 2;
        layout_info.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(g_Device, &layout_info, g_Allocator, &g_PointGpuPipeline.descriptor_set_layout) != VK_SUCCESS) {
            if (error) *error = "vkCreateDescriptorSetLayout failed for point layer GPU pipeline";
            return false;
        }
    }
    const uint32_t frame_count = parcelGpuDescriptorFrameCount();
    PointGpuLayerDescriptors& descriptors = g_PointGpuLayerDescriptors[layer_idx];
    if (descriptors.descriptor_sets_by_frame.size() != frame_count) {
        std::vector<VkDescriptorSetLayout> layouts((size_t)frame_count, g_PointGpuPipeline.descriptor_set_layout);
        std::vector<VkDescriptorSet> sets(layouts.size(), VK_NULL_HANDLE);
        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = g_DescriptorPool;
        alloc_info.descriptorSetCount = (uint32_t)layouts.size();
        alloc_info.pSetLayouts = layouts.data();
        if (vkAllocateDescriptorSets(g_Device, &alloc_info, sets.data()) != VK_SUCCESS) {
            if (error) *error = "vkAllocateDescriptorSets failed for point layer GPU pipeline";
            return false;
        }
        descriptors.descriptor_sets_by_frame = std::move(sets);
        descriptors.descriptor_dirty_by_frame.assign(frame_count, true);
        descriptors.descriptor_dirty = true;
    }
    if (descriptors.descriptor_dirty) {
        if (descriptors.descriptor_dirty_by_frame.size() != frame_count) {
            descriptors.descriptor_dirty_by_frame.assign(frame_count, true);
        } else {
            std::fill(descriptors.descriptor_dirty_by_frame.begin(), descriptors.descriptor_dirty_by_frame.end(), true);
        }
        descriptors.descriptor_dirty = false;
    }
    return true;
}

static bool ensurePointLayerGpuDescriptorSet(size_t layer_idx, std::string* error, VkDescriptorSet* out_set) {
    if (!ensurePointLayerGpuDescriptorSetsAllocated(layer_idx, error)) return false;
    PointGpuLayerDescriptors& descriptors = g_PointGpuLayerDescriptors[layer_idx];
    auto buf_it = g_PointGpuLayers.find(layer_idx);
    if (!out_set || descriptors.descriptor_sets_by_frame.empty() || buf_it == g_PointGpuLayers.end()) {
        if (error) *error = "point layer GPU descriptor set output is unavailable";
        return false;
    }
    const uint32_t frame_count = (uint32_t)descriptors.descriptor_sets_by_frame.size();
    const uint32_t frame_index = frame_count > 0 ? (g_CurrentFrameRenderIndex % frame_count) : 0;
    *out_set = descriptors.descriptor_sets_by_frame[frame_index];
    if (frame_index < descriptors.descriptor_dirty_by_frame.size() &&
        !descriptors.descriptor_dirty_by_frame[frame_index]) {
        return true;
    }
    const PointLayerGpuBuffers& buffers = buf_it->second;
    VkDescriptorBufferInfo color_info{};
    color_info.buffer = buffers.colors.buffer;
    color_info.offset = 0;
    color_info.range = buffers.colors.size_bytes;
    VkDescriptorBufferInfo glyph_info{};
    glyph_info.buffer = buffers.glyph_codes.buffer;
    glyph_info.offset = 0;
    glyph_info.range = buffers.glyph_codes.size_bytes;
    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = *out_set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &color_info;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = *out_set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &glyph_info;
    vkUpdateDescriptorSets(g_Device, 2, writes, 0, nullptr);
    if (frame_index < descriptors.descriptor_dirty_by_frame.size()) {
        descriptors.descriptor_dirty_by_frame[frame_index] = false;
    }
    return true;
}

static bool ensurePointLayerGpuPipeline(VkRenderPass render_pass, std::string* error) {
    if (!g_Device || !render_pass) {
        if (error) *error = "point layer GPU render pass/device is not ready";
        return false;
    }
    if (g_PointGpuPipeline.pipeline && g_PointGpuPipeline.render_pass == render_pass) return true;
    if (g_PointGpuPipeline.pipeline) vkDestroyPipeline(g_Device, g_PointGpuPipeline.pipeline, g_Allocator);
    if (g_PointGpuPipeline.pipeline_layout) vkDestroyPipelineLayout(g_Device, g_PointGpuPipeline.pipeline_layout, g_Allocator);
    g_PointGpuPipeline.pipeline = VK_NULL_HANDLE;
    g_PointGpuPipeline.pipeline_layout = VK_NULL_HANDLE;

    const std::vector<uint32_t> vert_code = loadSpirvFile(kCrimePointGpuVertShaderPath);
    const std::vector<uint32_t> frag_code = loadSpirvFile(kCrimePointGpuFragShaderPath);
    if (vert_code.empty() || frag_code.empty()) {
        if (error) *error = "point layer GPU shader SPIR-V is unavailable";
        return false;
    }
    VkShaderModuleCreateInfo shader_info{};
    shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shader_info.codeSize = vert_code.size() * sizeof(uint32_t);
    shader_info.pCode = vert_code.data();
    VkShaderModule vert_shader = VK_NULL_HANDLE;
    if (vkCreateShaderModule(g_Device, &shader_info, g_Allocator, &vert_shader) != VK_SUCCESS) {
        if (error) *error = "vkCreateShaderModule failed for point layer GPU vertex shader";
        return false;
    }
    shader_info.codeSize = frag_code.size() * sizeof(uint32_t);
    shader_info.pCode = frag_code.data();
    VkShaderModule frag_shader = VK_NULL_HANDLE;
    if (vkCreateShaderModule(g_Device, &shader_info, g_Allocator, &frag_shader) != VK_SUCCESS) {
        vkDestroyShaderModule(g_Device, vert_shader, g_Allocator);
        if (error) *error = "vkCreateShaderModule failed for point layer GPU fragment shader";
        return false;
    }
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(CrimePointGpuPushConstants);
    VkPipelineLayoutCreateInfo pipeline_layout_info{};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &g_PointGpuPipeline.descriptor_set_layout;
    pipeline_layout_info.pushConstantRangeCount = 1;
    pipeline_layout_info.pPushConstantRanges = &push_range;
    if (vkCreatePipelineLayout(g_Device, &pipeline_layout_info, g_Allocator, &g_PointGpuPipeline.pipeline_layout) != VK_SUCCESS) {
        vkDestroyShaderModule(g_Device, vert_shader, g_Allocator);
        vkDestroyShaderModule(g_Device, frag_shader, g_Allocator);
        if (error) *error = "vkCreatePipelineLayout failed for point layer GPU pipeline";
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
    pipeline_info.layout = g_PointGpuPipeline.pipeline_layout;
    pipeline_info.renderPass = render_pass;
    pipeline_info.subpass = 0;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result = vkCreateGraphicsPipelines(g_Device, VK_NULL_HANDLE, 1, &pipeline_info, g_Allocator, &pipeline);
    vkDestroyShaderModule(g_Device, vert_shader, g_Allocator);
    vkDestroyShaderModule(g_Device, frag_shader, g_Allocator);
    if (result != VK_SUCCESS || !pipeline) {
        if (error) *error = "vkCreateGraphicsPipelines failed for point layer GPU pipeline";
        if (g_PointGpuPipeline.pipeline_layout) vkDestroyPipelineLayout(g_Device, g_PointGpuPipeline.pipeline_layout, g_Allocator);
        g_PointGpuPipeline.pipeline_layout = VK_NULL_HANDLE;
        return false;
    }
    g_PointGpuPipeline.pipeline = pipeline;
    g_PointGpuPipeline.render_pass = render_pass;
    return true;
}

bool configurePointLayerGpuDrawState(size_t layer_idx, const ParcelGpuDrawConfig& config, std::string* error) {
    g_PointGpuLayerDrawStates[layer_idx] = CrimePointGpuDrawState{};
    if (!config.active) return true;
    auto it = g_PointGpuLayers.find(layer_idx);
    if (it == g_PointGpuLayers.end() ||
        !it->second.positions.buffer ||
        !it->second.feature_refs.buffer ||
        !it->second.colors.buffer ||
        !it->second.glyph_codes.buffer) {
        if (error) *error = "point layer GPU buffers are not resident";
        return false;
    }
    CrimePointGpuDrawState& draw_state = g_PointGpuLayerDrawStates[layer_idx];
    draw_state.active = true;
    draw_state.math_zoom = config.math_zoom;
    draw_state.zoom_scale = config.zoom_scale;
    draw_state.center_world = config.center_world;
    draw_state.viewport_origin = config.viewport_origin;
    draw_state.viewport_size = config.viewport_size;
    draw_state.framebuffer_size = config.framebuffer_size;
    return true;
}

void clearPointLayerGpuDrawState(size_t layer_idx) {
    g_PointGpuLayerDrawStates.erase(layer_idx);
}

void clearAllPointLayerGpuDrawStates() {
    g_PointGpuLayerDrawStates.clear();
}

bool pointLayerGpuDrawActive(size_t layer_idx) {
    auto draw_it = g_PointGpuLayerDrawStates.find(layer_idx);
    auto buf_it = g_PointGpuLayers.find(layer_idx);
    return draw_it != g_PointGpuLayerDrawStates.end() &&
        draw_it->second.active &&
        buf_it != g_PointGpuLayers.end() &&
        buf_it->second.positions.buffer &&
        buf_it->second.feature_refs.buffer &&
        buf_it->second.colors.buffer &&
        buf_it->second.glyph_codes.buffer &&
        buf_it->second.render_features > 0;
}

static void renderPointLayerGpuDrawCallback(const ImDrawList*, const ImDrawCmd* cmd) {
    const size_t layer_idx = (size_t)(uintptr_t)cmd->UserCallbackData;
    if (!pointLayerGpuDrawActive(layer_idx) || !g_CurrentFrameRenderCommandBuffer) return;
    auto draw_it = g_PointGpuLayerDrawStates.find(layer_idx);
    auto buf_it = g_PointGpuLayers.find(layer_idx);
    if (draw_it == g_PointGpuLayerDrawStates.end() || buf_it == g_PointGpuLayers.end()) return;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    std::string pipeline_error;
    if (!ensurePointLayerGpuDescriptorSet(layer_idx, &pipeline_error, &descriptor_set)) return;
    if (!ensurePointLayerGpuPipeline(g_CurrentFrameRenderPass, &pipeline_error)) return;
    CrimePointGpuPushConstants push{};
    push.center_world[0] = draw_it->second.center_world.x;
    push.center_world[1] = draw_it->second.center_world.y;
    push.viewport_origin[0] = draw_it->second.viewport_origin.x;
    push.viewport_origin[1] = draw_it->second.viewport_origin.y;
    push.viewport_size[0] = draw_it->second.viewport_size.x;
    push.viewport_size[1] = draw_it->second.viewport_size.y;
    push.framebuffer_size[0] = std::max(1.0f, draw_it->second.framebuffer_size.x);
    push.framebuffer_size[1] = std::max(1.0f, draw_it->second.framebuffer_size.y);
    push.math_zoom = (float)draw_it->second.math_zoom;
    push.zoom_scale = draw_it->second.zoom_scale;
    push.marker_radius_px = 5.0f;
    VkViewport viewport{0.0f, 0.0f, push.framebuffer_size[0], push.framebuffer_size[1], 0.0f, 1.0f};
    vkCmdSetViewport(g_CurrentFrameRenderCommandBuffer, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.offset.x = std::max(0, (int32_t)std::floor(draw_it->second.viewport_origin.x));
    scissor.offset.y = std::max(0, (int32_t)std::floor(draw_it->second.viewport_origin.y));
    const uint32_t max_width = (uint32_t)std::max(0.0f, push.framebuffer_size[0] - (float)scissor.offset.x);
    const uint32_t max_height = (uint32_t)std::max(0.0f, push.framebuffer_size[1] - (float)scissor.offset.y);
    scissor.extent.width = std::min((uint32_t)std::max(0.0f, std::ceil(draw_it->second.viewport_size.x)), max_width);
    scissor.extent.height = std::min((uint32_t)std::max(0.0f, std::ceil(draw_it->second.viewport_size.y)), max_height);
    if (scissor.extent.width == 0 || scissor.extent.height == 0) return;
    vkCmdSetScissor(g_CurrentFrameRenderCommandBuffer, 0, 1, &scissor);
    const VkBuffer vertex_buffers[] = {buf_it->second.positions.buffer, buf_it->second.feature_refs.buffer};
    const VkDeviceSize offsets[] = {0, 0};
    vkCmdBindPipeline(g_CurrentFrameRenderCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, g_PointGpuPipeline.pipeline);
    vkCmdBindDescriptorSets(g_CurrentFrameRenderCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, g_PointGpuPipeline.pipeline_layout, 0, 1, &descriptor_set, 0, nullptr);
    vkCmdPushConstants(g_CurrentFrameRenderCommandBuffer, g_PointGpuPipeline.pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
    vkCmdBindVertexBuffers(g_CurrentFrameRenderCommandBuffer, 0, 2, vertex_buffers, offsets);
    vkCmdDraw(g_CurrentFrameRenderCommandBuffer, 6, buf_it->second.render_features, 0, 0);
}

void enqueuePointLayerGpuDraw(ImDrawList* draw_list, size_t layer_idx) {
    if (!draw_list || !pointLayerGpuDrawActive(layer_idx)) return;
    draw_list->AddCallback(renderPointLayerGpuDrawCallback, (void*)(uintptr_t)layer_idx);
    draw_list->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

static bool ensurePolylineLayerGpuDescriptorSetsAllocated(size_t layer_idx, std::string* error) {
    auto buf_it = g_PolylineGpuLayers.find(layer_idx);
    if (!g_Device || !g_DescriptorPool || buf_it == g_PolylineGpuLayers.end() || !buf_it->second.colors.buffer) {
        if (error) *error = "polyline GPU descriptor prerequisites are not ready";
        return false;
    }
    if (!g_PolylineGpuPipeline.descriptor_set_layout) {
        VkDescriptorSetLayoutBinding color_binding{};
        color_binding.binding = 0;
        color_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        color_binding.descriptorCount = 1;
        color_binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        VkDescriptorSetLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.bindingCount = 1;
        layout_info.pBindings = &color_binding;
        if (vkCreateDescriptorSetLayout(g_Device, &layout_info, g_Allocator, &g_PolylineGpuPipeline.descriptor_set_layout) != VK_SUCCESS) {
            if (error) *error = "vkCreateDescriptorSetLayout failed for polyline GPU pipeline";
            return false;
        }
    }
    const uint32_t frame_count = parcelGpuDescriptorFrameCount();
    PolylineGpuLayerDescriptors& descriptors = g_PolylineGpuLayerDescriptors[layer_idx];
    if (descriptors.descriptor_sets_by_frame.size() != frame_count) {
        std::vector<VkDescriptorSetLayout> layouts((size_t)frame_count, g_PolylineGpuPipeline.descriptor_set_layout);
        std::vector<VkDescriptorSet> sets(layouts.size(), VK_NULL_HANDLE);
        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = g_DescriptorPool;
        alloc_info.descriptorSetCount = (uint32_t)layouts.size();
        alloc_info.pSetLayouts = layouts.data();
        if (vkAllocateDescriptorSets(g_Device, &alloc_info, sets.data()) != VK_SUCCESS) {
            if (error) *error = "vkAllocateDescriptorSets failed for polyline GPU pipeline";
            return false;
        }
        descriptors.descriptor_sets_by_frame.assign(frame_count, {});
        descriptors.descriptor_dirty_by_frame.assign(frame_count, true);
        for (uint32_t frame = 0; frame < frame_count; ++frame) {
            descriptors.descriptor_sets_by_frame[frame] = {sets[frame], VK_NULL_HANDLE, VK_NULL_HANDLE};
        }
        descriptors.descriptor_dirty = true;
    }
    if (descriptors.descriptor_dirty) {
        if (descriptors.descriptor_dirty_by_frame.size() != frame_count) {
            descriptors.descriptor_dirty_by_frame.assign(frame_count, true);
        } else {
            std::fill(descriptors.descriptor_dirty_by_frame.begin(), descriptors.descriptor_dirty_by_frame.end(), true);
        }
        descriptors.descriptor_dirty = false;
    }
    return true;
}

static bool ensurePolylineLayerGpuDescriptorSet(size_t layer_idx, std::string* error, VkDescriptorSet* out_set) {
    if (!ensurePolylineLayerGpuDescriptorSetsAllocated(layer_idx, error)) return false;
    PolylineGpuLayerDescriptors& descriptors = g_PolylineGpuLayerDescriptors[layer_idx];
    auto buf_it = g_PolylineGpuLayers.find(layer_idx);
    if (!out_set || descriptors.descriptor_sets_by_frame.empty() || buf_it == g_PolylineGpuLayers.end()) {
        if (error) *error = "polyline GPU descriptor set output is unavailable";
        return false;
    }
    const uint32_t frame_count = (uint32_t)descriptors.descriptor_sets_by_frame.size();
    const uint32_t frame_index = frame_count > 0 ? (g_CurrentFrameRenderIndex % frame_count) : 0;
    *out_set = descriptors.descriptor_sets_by_frame[frame_index][0];
    if (frame_index < descriptors.descriptor_dirty_by_frame.size() &&
        !descriptors.descriptor_dirty_by_frame[frame_index]) {
        return true;
    }
    VkDescriptorBufferInfo color_info{};
    color_info.buffer = buf_it->second.colors.buffer;
    color_info.offset = 0;
    color_info.range = buf_it->second.colors.size_bytes;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = *out_set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &color_info;
    vkUpdateDescriptorSets(g_Device, 1, &write, 0, nullptr);
    if (frame_index < descriptors.descriptor_dirty_by_frame.size()) {
        descriptors.descriptor_dirty_by_frame[frame_index] = false;
    }
    return true;
}

static bool ensurePolylineLayerGpuPipeline(VkRenderPass render_pass, std::string* error) {
    if (!g_Device || !render_pass) {
        if (error) *error = "polyline GPU render pass/device is not ready";
        return false;
    }
    if (g_PolylineGpuPipeline.line_pipeline && g_PolylineGpuPipeline.render_pass == render_pass) return true;
    if (g_PolylineGpuPipeline.line_pipeline) vkDestroyPipeline(g_Device, g_PolylineGpuPipeline.line_pipeline, g_Allocator);
    if (g_PolylineGpuPipeline.pipeline_layout) vkDestroyPipelineLayout(g_Device, g_PolylineGpuPipeline.pipeline_layout, g_Allocator);
    g_PolylineGpuPipeline.line_pipeline = VK_NULL_HANDLE;
    g_PolylineGpuPipeline.pipeline_layout = VK_NULL_HANDLE;

    const std::vector<uint32_t> vert_code = loadSpirvFile(kParcelGpuVertShaderPath);
    const std::vector<uint32_t> frag_code = loadSpirvFile(kParcelGpuFragShaderPath);
    if (vert_code.empty() || frag_code.empty()) {
        if (error) *error = "polyline GPU shader SPIR-V is unavailable";
        return false;
    }
    VkShaderModuleCreateInfo shader_info{};
    shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shader_info.codeSize = vert_code.size() * sizeof(uint32_t);
    shader_info.pCode = vert_code.data();
    VkShaderModule vert_shader = VK_NULL_HANDLE;
    if (vkCreateShaderModule(g_Device, &shader_info, g_Allocator, &vert_shader) != VK_SUCCESS) {
        if (error) *error = "vkCreateShaderModule failed for polyline GPU vertex shader";
        return false;
    }
    shader_info.codeSize = frag_code.size() * sizeof(uint32_t);
    shader_info.pCode = frag_code.data();
    VkShaderModule frag_shader = VK_NULL_HANDLE;
    if (vkCreateShaderModule(g_Device, &shader_info, g_Allocator, &frag_shader) != VK_SUCCESS) {
        vkDestroyShaderModule(g_Device, vert_shader, g_Allocator);
        if (error) *error = "vkCreateShaderModule failed for polyline GPU fragment shader";
        return false;
    }
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(ParcelGpuPushConstants);
    VkPipelineLayoutCreateInfo pipeline_layout_info{};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &g_PolylineGpuPipeline.descriptor_set_layout;
    pipeline_layout_info.pushConstantRangeCount = 1;
    pipeline_layout_info.pPushConstantRanges = &push_range;
    if (vkCreatePipelineLayout(g_Device, &pipeline_layout_info, g_Allocator, &g_PolylineGpuPipeline.pipeline_layout) != VK_SUCCESS) {
        vkDestroyShaderModule(g_Device, vert_shader, g_Allocator);
        vkDestroyShaderModule(g_Device, frag_shader, g_Allocator);
        if (error) *error = "vkCreatePipelineLayout failed for polyline GPU pipeline";
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
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
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
    pipeline_info.layout = g_PolylineGpuPipeline.pipeline_layout;
    pipeline_info.renderPass = render_pass;
    pipeline_info.subpass = 0;
    VkPipeline line_pipeline = VK_NULL_HANDLE;
    const VkResult result = vkCreateGraphicsPipelines(g_Device, VK_NULL_HANDLE, 1, &pipeline_info, g_Allocator, &line_pipeline);
    vkDestroyShaderModule(g_Device, vert_shader, g_Allocator);
    vkDestroyShaderModule(g_Device, frag_shader, g_Allocator);
    if (result != VK_SUCCESS || !line_pipeline) {
        if (error) *error = "vkCreateGraphicsPipelines failed for polyline GPU pipeline";
        if (g_PolylineGpuPipeline.pipeline_layout) vkDestroyPipelineLayout(g_Device, g_PolylineGpuPipeline.pipeline_layout, g_Allocator);
        g_PolylineGpuPipeline.pipeline_layout = VK_NULL_HANDLE;
        return false;
    }
    g_PolylineGpuPipeline.line_pipeline = line_pipeline;
    g_PolylineGpuPipeline.render_pass = render_pass;
    return true;
}

bool configurePolylineLayerGpuDrawState(size_t layer_idx, const ParcelGpuDrawConfig& config, std::string* error) {
    g_PolylineGpuLayerDrawStates[layer_idx] = ParcelGpuDrawState{};
    if (!config.active) return true;
    auto it = g_PolylineGpuLayers.find(layer_idx);
    if (it == g_PolylineGpuLayers.end() ||
        !it->second.positions.buffer ||
        !it->second.feature_refs.buffer ||
        !it->second.line_indices.buffer ||
        it->second.chunks.empty()) {
        if (error) *error = "polyline GPU buffers are not resident";
        return false;
    }
    ParcelGpuDrawState& draw_state = g_PolylineGpuLayerDrawStates[layer_idx];
    draw_state.active = true;
    draw_state.math_zoom = config.math_zoom;
    draw_state.zoom_scale = config.zoom_scale;
    draw_state.center_world = config.center_world;
    draw_state.viewport_origin = config.viewport_origin;
    draw_state.viewport_size = config.viewport_size;
    draw_state.framebuffer_size = config.framebuffer_size;
    draw_state.visible_line_chunks.reserve(it->second.chunks.size());
    const float lon_span = std::max(0.0f, config.view_max_lon - config.view_min_lon);
    const float lat_span = std::max(0.0f, config.view_max_lat - config.view_min_lat);
    const float lon_pad = std::max(0.0001f, lon_span * (2.0f / std::max(1.0f, config.viewport_size.x)));
    const float lat_pad = std::max(0.0001f, lat_span * (2.0f / std::max(1.0f, config.viewport_size.y)));
    for (const GeometryArtifactChunkRecord& chunk : it->second.chunks) {
        if (!rectsOverlap(chunk.min_lon, chunk.min_lat, chunk.max_lon, chunk.max_lat,
                config.view_min_lon - lon_pad, config.view_min_lat - lat_pad,
                config.view_max_lon + lon_pad, config.view_max_lat + lat_pad)) {
            continue;
        }
        if (chunk.index_count == 0) continue;
        draw_state.visible_line_chunks.push_back(ParcelGpuLineDrawChunk{chunk.index_offset, chunk.index_count});
    }
    return true;
}

void clearPolylineLayerGpuDrawState(size_t layer_idx) {
    g_PolylineGpuLayerDrawStates.erase(layer_idx);
}

void clearAllPolylineLayerGpuDrawStates() {
    g_PolylineGpuLayerDrawStates.clear();
}

bool polylineLayerGpuDrawActive(size_t layer_idx) {
    auto draw_it = g_PolylineGpuLayerDrawStates.find(layer_idx);
    auto buf_it = g_PolylineGpuLayers.find(layer_idx);
    return draw_it != g_PolylineGpuLayerDrawStates.end() &&
        draw_it->second.active &&
        !draw_it->second.visible_line_chunks.empty() &&
        buf_it != g_PolylineGpuLayers.end() &&
        buf_it->second.positions.buffer &&
        buf_it->second.feature_refs.buffer &&
        buf_it->second.line_indices.buffer &&
        buf_it->second.colors.buffer;
}

static void renderPolylineLayerGpuDrawCallback(const ImDrawList*, const ImDrawCmd* cmd) {
    const size_t layer_idx = (size_t)(uintptr_t)cmd->UserCallbackData;
    if (!polylineLayerGpuDrawActive(layer_idx) || !g_CurrentFrameRenderCommandBuffer) return;
    auto draw_it = g_PolylineGpuLayerDrawStates.find(layer_idx);
    auto buf_it = g_PolylineGpuLayers.find(layer_idx);
    if (draw_it == g_PolylineGpuLayerDrawStates.end() || buf_it == g_PolylineGpuLayers.end()) return;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    std::string pipeline_error;
    if (!ensurePolylineLayerGpuDescriptorSet(layer_idx, &pipeline_error, &descriptor_set)) return;
    if (!ensurePolylineLayerGpuPipeline(g_CurrentFrameRenderPass, &pipeline_error)) return;
    ParcelGpuPushConstants push{};
    push.center_world[0] = draw_it->second.center_world.x;
    push.center_world[1] = draw_it->second.center_world.y;
    push.viewport_origin[0] = draw_it->second.viewport_origin.x;
    push.viewport_origin[1] = draw_it->second.viewport_origin.y;
    push.viewport_size[0] = draw_it->second.viewport_size.x;
    push.viewport_size[1] = draw_it->second.viewport_size.y;
    push.framebuffer_size[0] = std::max(1.0f, draw_it->second.framebuffer_size.x);
    push.framebuffer_size[1] = std::max(1.0f, draw_it->second.framebuffer_size.y);
    push.math_zoom = (float)draw_it->second.math_zoom;
    push.zoom_scale = draw_it->second.zoom_scale;
    VkViewport viewport{0.0f, 0.0f, push.framebuffer_size[0], push.framebuffer_size[1], 0.0f, 1.0f};
    vkCmdSetViewport(g_CurrentFrameRenderCommandBuffer, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.offset.x = std::max(0, (int32_t)std::floor(draw_it->second.viewport_origin.x));
    scissor.offset.y = std::max(0, (int32_t)std::floor(draw_it->second.viewport_origin.y));
    const uint32_t max_width = (uint32_t)std::max(0.0f, push.framebuffer_size[0] - (float)scissor.offset.x);
    const uint32_t max_height = (uint32_t)std::max(0.0f, push.framebuffer_size[1] - (float)scissor.offset.y);
    scissor.extent.width = std::min((uint32_t)std::max(0.0f, std::ceil(draw_it->second.viewport_size.x)), max_width);
    scissor.extent.height = std::min((uint32_t)std::max(0.0f, std::ceil(draw_it->second.viewport_size.y)), max_height);
    if (scissor.extent.width == 0 || scissor.extent.height == 0) return;
    vkCmdSetScissor(g_CurrentFrameRenderCommandBuffer, 0, 1, &scissor);
    const VkBuffer vertex_buffers[] = {buf_it->second.positions.buffer, buf_it->second.feature_refs.buffer};
    const VkDeviceSize offsets[] = {0, 0};
    vkCmdBindPipeline(g_CurrentFrameRenderCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, g_PolylineGpuPipeline.line_pipeline);
    vkCmdBindDescriptorSets(g_CurrentFrameRenderCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, g_PolylineGpuPipeline.pipeline_layout, 0, 1, &descriptor_set, 0, nullptr);
    vkCmdPushConstants(g_CurrentFrameRenderCommandBuffer, g_PolylineGpuPipeline.pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
    vkCmdBindVertexBuffers(g_CurrentFrameRenderCommandBuffer, 0, 2, vertex_buffers, offsets);
    vkCmdBindIndexBuffer(g_CurrentFrameRenderCommandBuffer, buf_it->second.line_indices.buffer, 0, VK_INDEX_TYPE_UINT32);
    for (const ParcelGpuLineDrawChunk& chunk : draw_it->second.visible_line_chunks) {
        vkCmdDrawIndexed(g_CurrentFrameRenderCommandBuffer, chunk.index_count, 1, chunk.first_index, 0, 0);
    }
}

void enqueuePolylineLayerGpuDraw(ImDrawList* draw_list, size_t layer_idx) {
    if (!draw_list || !polylineLayerGpuDrawActive(layer_idx)) return;
    draw_list->AddCallback(renderPolylineLayerGpuDrawCallback, (void*)(uintptr_t)layer_idx);
    draw_list->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

static bool ensureCrimePointGpuDescriptorSetsAllocated(std::string* error) {
    if (!g_Device || !g_DescriptorPool || !g_CrimePointGpuBuffers.colors.buffer || !g_CrimePointGpuBuffers.glyph_codes.buffer) {
        if (error) *error = "crime point GPU descriptor prerequisites are not ready";
        return false;
    }
    if (!g_CrimePointGpuPipeline.descriptor_set_layout) {
        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        VkDescriptorSetLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.bindingCount = 2;
        layout_info.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(g_Device, &layout_info, g_Allocator, &g_CrimePointGpuPipeline.descriptor_set_layout) != VK_SUCCESS) {
            if (error) *error = "vkCreateDescriptorSetLayout failed for crime point GPU pipeline";
            return false;
        }
    }
    const uint32_t frame_count = parcelGpuDescriptorFrameCount();
    if (g_CrimePointGpuPipeline.descriptor_sets_by_frame.size() != frame_count) {
        std::vector<VkDescriptorSetLayout> layouts((size_t)frame_count, g_CrimePointGpuPipeline.descriptor_set_layout);
        std::vector<VkDescriptorSet> sets(layouts.size(), VK_NULL_HANDLE);
        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = g_DescriptorPool;
        alloc_info.descriptorSetCount = (uint32_t)layouts.size();
        alloc_info.pSetLayouts = layouts.data();
        if (vkAllocateDescriptorSets(g_Device, &alloc_info, sets.data()) != VK_SUCCESS) {
            if (error) *error = "vkAllocateDescriptorSets failed for crime point GPU pipeline";
            return false;
        }
        g_CrimePointGpuPipeline.descriptor_sets_by_frame = std::move(sets);
        g_CrimePointGpuPipeline.descriptor_dirty_by_frame.assign(frame_count, true);
        g_CrimePointGpuPipeline.descriptor_dirty = true;
    }
    if (g_CrimePointGpuPipeline.descriptor_dirty) {
        if (g_CrimePointGpuPipeline.descriptor_dirty_by_frame.size() != frame_count) {
            g_CrimePointGpuPipeline.descriptor_dirty_by_frame.assign(frame_count, true);
        } else {
            std::fill(
                g_CrimePointGpuPipeline.descriptor_dirty_by_frame.begin(),
                g_CrimePointGpuPipeline.descriptor_dirty_by_frame.end(),
                true);
        }
        g_CrimePointGpuPipeline.descriptor_dirty = false;
    }
    return true;
}

static bool ensureCrimePointGpuDescriptorSet(std::string* error, VkDescriptorSet* out_set) {
    if (!ensureCrimePointGpuDescriptorSetsAllocated(error)) return false;
    if (!out_set || g_CrimePointGpuPipeline.descriptor_sets_by_frame.empty()) {
        if (error) *error = "crime point GPU descriptor set output is unavailable";
        return false;
    }
    const uint32_t frame_count = (uint32_t)g_CrimePointGpuPipeline.descriptor_sets_by_frame.size();
    const uint32_t frame_index = frame_count > 0 ? (g_CurrentFrameRenderIndex % frame_count) : 0;
    *out_set = g_CrimePointGpuPipeline.descriptor_sets_by_frame[frame_index];
    if (frame_index >= g_CrimePointGpuPipeline.descriptor_dirty_by_frame.size() ||
        !g_CrimePointGpuPipeline.descriptor_dirty_by_frame[frame_index]) {
        return true;
    }
    VkDescriptorBufferInfo color_info{};
    color_info.buffer = g_CrimePointGpuBuffers.colors.buffer;
    color_info.offset = 0;
    color_info.range = g_CrimePointGpuBuffers.colors.size_bytes;
    VkDescriptorBufferInfo glyph_info{};
    glyph_info.buffer = g_CrimePointGpuBuffers.glyph_codes.buffer;
    glyph_info.offset = 0;
    glyph_info.range = g_CrimePointGpuBuffers.glyph_codes.size_bytes;
    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = *out_set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &color_info;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = *out_set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &glyph_info;
    vkUpdateDescriptorSets(g_Device, 2, writes, 0, nullptr);
    g_CrimePointGpuPipeline.descriptor_dirty_by_frame[frame_index] = false;
    return true;
}

static bool ensureCrimePointGpuPipeline(VkRenderPass render_pass, std::string* error) {
    if (!g_Device || !render_pass) {
        if (error) *error = "crime point GPU render pass/device is not ready";
        return false;
    }
    if (!ensureCrimePointGpuDescriptorSetsAllocated(error)) return false;
    if (g_CrimePointGpuPipeline.pipeline && g_CrimePointGpuPipeline.render_pass == render_pass) return true;
    if (g_CrimePointGpuPipeline.pipeline) {
        vkDestroyPipeline(g_Device, g_CrimePointGpuPipeline.pipeline, g_Allocator);
        g_CrimePointGpuPipeline.pipeline = VK_NULL_HANDLE;
    }
    if (g_CrimePointGpuPipeline.pipeline_layout) {
        vkDestroyPipelineLayout(g_Device, g_CrimePointGpuPipeline.pipeline_layout, g_Allocator);
        g_CrimePointGpuPipeline.pipeline_layout = VK_NULL_HANDLE;
    }

    const std::vector<uint32_t> vert_code = loadSpirvFile(kCrimePointGpuVertShaderPath);
    const std::vector<uint32_t> frag_code = loadSpirvFile(kCrimePointGpuFragShaderPath);
    if (vert_code.empty() || frag_code.empty()) {
        if (error) *error = "crime point GPU shader SPIR-V is unavailable";
        return false;
    }

    VkShaderModuleCreateInfo shader_info{};
    shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shader_info.codeSize = vert_code.size() * sizeof(uint32_t);
    shader_info.pCode = vert_code.data();
    VkShaderModule vert_shader = VK_NULL_HANDLE;
    if (vkCreateShaderModule(g_Device, &shader_info, g_Allocator, &vert_shader) != VK_SUCCESS) {
        if (error) *error = "vkCreateShaderModule failed for crime point vertex shader";
        return false;
    }
    shader_info.codeSize = frag_code.size() * sizeof(uint32_t);
    shader_info.pCode = frag_code.data();
    VkShaderModule frag_shader = VK_NULL_HANDLE;
    if (vkCreateShaderModule(g_Device, &shader_info, g_Allocator, &frag_shader) != VK_SUCCESS) {
        vkDestroyShaderModule(g_Device, vert_shader, g_Allocator);
        if (error) *error = "vkCreateShaderModule failed for crime point fragment shader";
        return false;
    }

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(CrimePointGpuPushConstants);

    VkPipelineLayoutCreateInfo pipeline_layout_info{};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &g_CrimePointGpuPipeline.descriptor_set_layout;
    pipeline_layout_info.pushConstantRangeCount = 1;
    pipeline_layout_info.pPushConstantRanges = &push_range;
    if (vkCreatePipelineLayout(g_Device, &pipeline_layout_info, g_Allocator, &g_CrimePointGpuPipeline.pipeline_layout) != VK_SUCCESS) {
        vkDestroyShaderModule(g_Device, vert_shader, g_Allocator);
        vkDestroyShaderModule(g_Device, frag_shader, g_Allocator);
        if (error) *error = "vkCreatePipelineLayout failed for crime point GPU pipeline";
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
    bindings[0].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
    bindings[1].binding = 1;
    bindings[1].stride = sizeof(uint32_t);
    bindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

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
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

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
    pipeline_info.layout = g_CrimePointGpuPipeline.pipeline_layout;
    pipeline_info.renderPass = render_pass;
    pipeline_info.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result =
        vkCreateGraphicsPipelines(g_Device, VK_NULL_HANDLE, 1, &pipeline_info, g_Allocator, &pipeline);
    vkDestroyShaderModule(g_Device, vert_shader, g_Allocator);
    vkDestroyShaderModule(g_Device, frag_shader, g_Allocator);
    if (result != VK_SUCCESS || !pipeline) {
        if (error) *error = "vkCreateGraphicsPipelines failed for crime point GPU pipeline";
        vkDestroyPipelineLayout(g_Device, g_CrimePointGpuPipeline.pipeline_layout, g_Allocator);
        g_CrimePointGpuPipeline.pipeline_layout = VK_NULL_HANDLE;
        return false;
    }
    g_CrimePointGpuPipeline.pipeline = pipeline;
    g_CrimePointGpuPipeline.render_pass = render_pass;
    return true;
}

bool configureCrimePointGpuDrawState(const ParcelGpuDrawConfig& config, std::string* error) {
    g_CrimePointGpuDrawState = CrimePointGpuDrawState{};
    if (!config.active) return true;
    if (!g_CrimePointGpuBuffers.positions.buffer ||
        !g_CrimePointGpuBuffers.feature_refs.buffer ||
        !g_CrimePointGpuBuffers.colors.buffer ||
        !g_CrimePointGpuBuffers.glyph_codes.buffer) {
        if (error) *error = "crime point GPU buffers are not resident";
        return false;
    }
    g_CrimePointGpuDrawState.active = true;
    g_CrimePointGpuDrawState.math_zoom = config.math_zoom;
    g_CrimePointGpuDrawState.zoom_scale = config.zoom_scale;
    g_CrimePointGpuDrawState.center_world = config.center_world;
    g_CrimePointGpuDrawState.viewport_origin = config.viewport_origin;
    g_CrimePointGpuDrawState.viewport_size = config.viewport_size;
    g_CrimePointGpuDrawState.framebuffer_size = config.framebuffer_size;
    return true;
}

void clearCrimePointGpuDrawState() {
    g_CrimePointGpuDrawState = CrimePointGpuDrawState{};
}

bool crimePointGpuDrawActive() {
    return g_CrimePointGpuDrawState.active &&
        g_CrimePointGpuBuffers.positions.buffer &&
        g_CrimePointGpuBuffers.feature_refs.buffer &&
        g_CrimePointGpuBuffers.colors.buffer &&
        g_CrimePointGpuBuffers.glyph_codes.buffer &&
        g_CrimePointGpuBuffers.render_features > 0;
}

static void renderCrimePointGpuDrawCallback(const ImDrawList*, const ImDrawCmd*) {
    if (!crimePointGpuDrawActive() || !g_CurrentFrameRenderCommandBuffer) return;
    std::string pipeline_error;
    if (!ensureCrimePointGpuPipeline(g_CurrentFrameRenderPass, &pipeline_error)) return;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    if (!ensureCrimePointGpuDescriptorSet(&pipeline_error, &descriptor_set)) return;
    CrimePointGpuPushConstants push{};
    push.center_world[0] = g_CrimePointGpuDrawState.center_world.x;
    push.center_world[1] = g_CrimePointGpuDrawState.center_world.y;
    push.viewport_origin[0] = g_CrimePointGpuDrawState.viewport_origin.x;
    push.viewport_origin[1] = g_CrimePointGpuDrawState.viewport_origin.y;
    push.viewport_size[0] = g_CrimePointGpuDrawState.viewport_size.x;
    push.viewport_size[1] = g_CrimePointGpuDrawState.viewport_size.y;
    push.framebuffer_size[0] = std::max(1.0f, g_CrimePointGpuDrawState.framebuffer_size.x);
    push.framebuffer_size[1] = std::max(1.0f, g_CrimePointGpuDrawState.framebuffer_size.y);
    push.math_zoom = (float)g_CrimePointGpuDrawState.math_zoom;
    push.zoom_scale = g_CrimePointGpuDrawState.zoom_scale;
    push.marker_radius_px = 5.0f;
    VkViewport viewport{0.0f, 0.0f, push.framebuffer_size[0], push.framebuffer_size[1], 0.0f, 1.0f};
    vkCmdSetViewport(g_CurrentFrameRenderCommandBuffer, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.offset.x = std::max(0, (int32_t)std::floor(g_CrimePointGpuDrawState.viewport_origin.x));
    scissor.offset.y = std::max(0, (int32_t)std::floor(g_CrimePointGpuDrawState.viewport_origin.y));
    const uint32_t max_width = (uint32_t)std::max(0.0f, push.framebuffer_size[0] - (float)scissor.offset.x);
    const uint32_t max_height = (uint32_t)std::max(0.0f, push.framebuffer_size[1] - (float)scissor.offset.y);
    scissor.extent.width = std::min((uint32_t)std::max(0.0f, std::ceil(g_CrimePointGpuDrawState.viewport_size.x)), max_width);
    scissor.extent.height = std::min((uint32_t)std::max(0.0f, std::ceil(g_CrimePointGpuDrawState.viewport_size.y)), max_height);
    if (scissor.extent.width == 0 || scissor.extent.height == 0) return;
    vkCmdSetScissor(g_CurrentFrameRenderCommandBuffer, 0, 1, &scissor);
    const VkBuffer vertex_buffers[] = {g_CrimePointGpuBuffers.positions.buffer, g_CrimePointGpuBuffers.feature_refs.buffer};
    const VkDeviceSize offsets[] = {0, 0};
    vkCmdBindPipeline(g_CurrentFrameRenderCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, g_CrimePointGpuPipeline.pipeline);
    vkCmdBindDescriptorSets(g_CurrentFrameRenderCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, g_CrimePointGpuPipeline.pipeline_layout, 0, 1, &descriptor_set, 0, nullptr);
    vkCmdPushConstants(g_CurrentFrameRenderCommandBuffer, g_CrimePointGpuPipeline.pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
    vkCmdBindVertexBuffers(g_CurrentFrameRenderCommandBuffer, 0, 2, vertex_buffers, offsets);
    vkCmdDraw(g_CurrentFrameRenderCommandBuffer, 6, g_CrimePointGpuBuffers.render_features, 0, 0);
}

void enqueueCrimePointGpuDraw(ImDrawList* draw_list) {
    if (!draw_list || !crimePointGpuDrawActive()) return;
    draw_list->AddCallback(renderCrimePointGpuDrawCallback, nullptr);
    draw_list->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

void cleanupFeatureOverlayGpuServices() {
    clearAllPointLayerGpuBuffers();
    clearAllPolylineLayerGpuBuffers();
    clearCrimePointGpuBuffers();
    destroyPointLayerGpuPipeline();
    destroyPolylineLayerGpuPipeline();
    destroyCrimePointGpuPipeline();
}
