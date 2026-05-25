#include "worldsim_app_internal.h"

#include <algorithm>
#include <cstring>

void clearParcelGpuBuffers() {
    retireParcelGpuBuffers(std::move(g_ParcelGpuBuffers));
    g_ParcelGpuBuffers = ParcelGpuBuffers{};
    g_ParcelGpuOverlayHasVisibleColors = false;
    g_ParcelGpuOutlineHasVisibleColors = false;
    g_ParcelGpuPipeline.descriptor_dirty = true;
    if (!g_ParcelGpuPipeline.descriptor_dirty_by_frame.empty()) {
        std::fill(
            g_ParcelGpuPipeline.descriptor_dirty_by_frame.begin(),
            g_ParcelGpuPipeline.descriptor_dirty_by_frame.end(),
            true);
    }
    clearParcelGpuDrawState();
    drainRetiredParcelGpuResources();
    publishParcelGpuStatusSnapshot();
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
    publishParcelGpuStatusSnapshot();
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
    publishParcelGpuStatusSnapshot();
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
    g_ParcelGpuOverlayHasVisibleColors = std::any_of(colors_rgba.begin(), colors_rgba.end(), [](ImU32 color) {
        return (color >> 24) != 0;
    });
    publishParcelGpuStatusSnapshot();
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
    g_ParcelGpuOutlineHasVisibleColors = std::any_of(colors_rgba.begin(), colors_rgba.end(), [](ImU32 color) {
        return (color >> 24) != 0;
    });
    publishParcelGpuStatusSnapshot();
    return true;
}

ParcelGpuResidencyStatus getParcelGpuResidencyStatus() {
    std::lock_guard<std::mutex> lk(g_ParcelGpuStatusMutex);
    return g_ParcelGpuStatusSnapshot;
}

GpuProfilerLiveSnapshot getGpuProfilerLiveSnapshot() {
    GpuProfilerLiveSnapshot out;

    if (g_PhysicalDevice != VK_NULL_HANDLE) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(g_PhysicalDevice, &props);
        out.physical_device_name = props.deviceName;

        VkPhysicalDeviceMemoryProperties mem{};
        vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &mem);
        out.heaps.reserve(mem.memoryHeapCount);
        for (uint32_t heap_idx = 0; heap_idx < mem.memoryHeapCount; ++heap_idx) {
            GpuProfilerHeapSnapshot heap;
            heap.index = heap_idx;
            heap.size_bytes = mem.memoryHeaps[heap_idx].size;
            heap.device_local = (mem.memoryHeaps[heap_idx].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
            for (uint32_t type_idx = 0; type_idx < mem.memoryTypeCount; ++type_idx) {
                if (mem.memoryTypes[type_idx].heapIndex != heap_idx) continue;
                heap.memory_type_count++;
                if (mem.memoryTypes[type_idx].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) heap.host_visible_type_count++;
                if (mem.memoryTypes[type_idx].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) heap.host_coherent_type_count++;
            }
            out.heaps.push_back(heap);
        }
    }

    {
        std::lock_guard<std::mutex> lk(g_ParcelGpuStatusMutex);
        out.parcel_gpu_resident = g_ParcelGpuStatusSnapshot.resident;
        out.parcel_gpu_draw_active = g_ParcelGpuStatusSnapshot.draw_active;
        out.parcel_device_local_bytes = g_ParcelGpuStatusSnapshot.device_local_bytes;
        out.parcel_host_visible_bytes = g_ParcelGpuStatusSnapshot.host_visible_bytes;
        out.retired_parcel_device_local_bytes = g_ParcelGpuStatusSnapshot.retired_device_local_bytes;
        out.retired_parcel_host_visible_bytes = g_ParcelGpuStatusSnapshot.retired_host_visible_bytes;
    }
    out.tile_cache_entries = g_TileCache.size();
    out.retired_texture_count = g_RetiredTextures.size();
    {
        std::lock_guard<std::mutex> lk(g_GpuProfilerAlertMutex);
        out.oom_active = g_GpuProfilerAlertState.oom_active;
        out.oom_generation = g_GpuProfilerAlertState.oom_generation;
        out.last_error = g_GpuProfilerAlertState.last_error;
    }
    {
        std::lock_guard<std::mutex> lk(g_GpuProfilerEventMutex);
        const auto now = std::chrono::steady_clock::now();
        out.recent_events.reserve(g_GpuProfilerEvents.size());
        for (auto it = g_GpuProfilerEvents.rbegin(); it != g_GpuProfilerEvents.rend(); ++it) {
            const double age_s = std::chrono::duration<double>(now - it->at).count();
            char buf[384];
            std::snprintf(buf, sizeof(buf), "%.1fs ago: %s", age_s, it->label.c_str());
            out.recent_events.push_back(buf);
        }
    }
    return out;
}

bool consumeGpuProfilerTabSelectionRequest() {
    std::lock_guard<std::mutex> lk(g_GpuProfilerAlertMutex);
    const bool requested = g_GpuProfilerAlertState.tab_selection_requested;
    g_GpuProfilerAlertState.tab_selection_requested = false;
    return requested;
}

void clearGpuProfilerAlertState() {
    std::lock_guard<std::mutex> lk(g_GpuProfilerAlertMutex);
    g_GpuProfilerAlertState.oom_active = false;
    g_GpuProfilerAlertState.last_error.clear();
}

static void adoptParcelGpuUploadPayload(ParcelGpuUploadPayload&& payload) {
    retireParcelGpuBuffers(std::move(g_ParcelGpuBuffers));
    g_ParcelGpuBuffers = ParcelGpuBuffers{};
    g_ParcelGpuBuffers = std::move(payload.buffers);
    clearGpuProfilerAlertState();
    recordGpuProfilerEvent("parcel GPU upload adopted");
    g_ParcelGpuPipeline.descriptor_dirty = true;
    publishParcelGpuStatusSnapshot();
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
