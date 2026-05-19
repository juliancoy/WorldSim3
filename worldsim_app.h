#pragma once

#include "imgui.h"
#include "cache_io.h"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

int runWorldSim3App(int argc, char** argv);

struct ParcelGpuResidencyStatus {
    bool resident = false;
    uint32_t render_features = 0;
    uint32_t vertices = 0;
    uint32_t indices = 0;
    uint32_t line_indices = 0;
    uint32_t colors = 0;
    uint32_t visible_chunks = 0;
    uint32_t visible_line_chunks = 0;
    bool draw_active = false;
    bool overlay_active = false;
    bool outline_active = false;
    uint64_t device_local_bytes = 0;
    uint64_t host_visible_bytes = 0;
    uint64_t retired_device_local_bytes = 0;
    uint64_t retired_host_visible_bytes = 0;
    std::string source_signature;
};

struct GpuProfilerHeapSnapshot {
    uint32_t index = 0;
    uint64_t size_bytes = 0;
    bool device_local = false;
    uint32_t memory_type_count = 0;
    uint32_t host_visible_type_count = 0;
    uint32_t host_coherent_type_count = 0;
};

struct GpuProfilerLiveSnapshot {
    std::string physical_device_name;
    std::vector<GpuProfilerHeapSnapshot> heaps;
    uint64_t parcel_device_local_bytes = 0;
    uint64_t parcel_host_visible_bytes = 0;
    uint64_t retired_parcel_device_local_bytes = 0;
    uint64_t retired_parcel_host_visible_bytes = 0;
    size_t tile_cache_entries = 0;
    size_t retired_texture_count = 0;
    bool parcel_gpu_resident = false;
    bool parcel_gpu_draw_active = false;
    bool oom_active = false;
    uint64_t oom_generation = 0;
    std::string last_error;
    std::vector<std::string> recent_events;
};

struct ParcelGpuDrawConfig {
    bool active = false;
    int math_zoom = 0;
    float zoom_scale = 1.0f;
    ImVec2 center_world = ImVec2(0.0f, 0.0f);
    ImVec2 viewport_origin = ImVec2(0.0f, 0.0f);
    ImVec2 viewport_size = ImVec2(0.0f, 0.0f);
    ImVec2 framebuffer_size = ImVec2(0.0f, 0.0f);
    float view_min_lon = 0.0f;
    float view_min_lat = 0.0f;
    float view_max_lon = 0.0f;
    float view_max_lat = 0.0f;
};

bool ensureParcelGpuBuffersResident(const ParcelRenderCacheBlob& blob, std::string* error = nullptr);
bool updateParcelGpuColorBuffer(const std::vector<ImU32>& colors_rgba, std::string* error = nullptr);
bool updateParcelGpuOverlayColorBuffer(const std::vector<ImU32>& colors_rgba, std::string* error = nullptr);
bool updateParcelGpuOutlineColorBuffer(const std::vector<ImU32>& colors_rgba, std::string* error = nullptr);
void clearParcelGpuBuffers();
ParcelGpuResidencyStatus getParcelGpuResidencyStatus();
GpuProfilerLiveSnapshot getGpuProfilerLiveSnapshot();
void recordGpuProfilerEvent(const std::string& label);
bool consumeGpuProfilerTabSelectionRequest();
void clearGpuProfilerAlertState();
bool configureParcelGpuDrawState(const ParcelGpuDrawConfig& config, std::string* error = nullptr);
void clearParcelGpuDrawState();
bool parcelGpuDrawActive();
void enqueueParcelGpuDraw(ImDrawList* draw_list);
bool parcelGpuOverlayDrawActive();
void enqueueParcelGpuOverlayDraw(ImDrawList* draw_list);
bool parcelGpuOutlineDrawActive();
void enqueueParcelGpuOutlineDraw(ImDrawList* draw_list);
void drainRetiredParcelGpuResources();
bool startParcelGpuUploadWorker(std::string* error = nullptr);
void stopParcelGpuUploadWorker();
bool requestParcelGpuUpload(const ParcelRenderCacheBlob& blob, std::string* error = nullptr);
bool drainParcelGpuUploadResults(const std::string* expected_signature = nullptr, std::string* adopted_signature = nullptr, std::string* error = nullptr);

bool ensureZoningGpuBuffersResident(size_t layer_idx, const ParcelRenderCacheBlob& blob, std::string* error = nullptr);
bool updateZoningGpuColorBuffer(size_t layer_idx, const std::vector<ImU32>& colors_rgba, std::string* error = nullptr);
bool updateZoningGpuOutlineColorBuffer(size_t layer_idx, const std::vector<ImU32>& colors_rgba, std::string* error = nullptr);
void clearZoningGpuBuffers(size_t layer_idx);
void clearAllZoningGpuBuffers();
bool configureZoningGpuDrawState(size_t layer_idx, const ParcelGpuDrawConfig& config, std::string* error = nullptr);
void clearZoningGpuDrawState(size_t layer_idx);
void clearAllZoningGpuDrawStates();
bool zoningGpuDrawActive(size_t layer_idx);
void enqueueZoningGpuDraw(ImDrawList* draw_list, size_t layer_idx);
bool zoningGpuOutlineDrawActive(size_t layer_idx);
void enqueueZoningGpuOutlineDraw(ImDrawList* draw_list, size_t layer_idx);

bool ensureCrimePointGpuBuffersResident(
    const std::string& source_signature,
    const std::vector<ImVec2>& lonlat_positions,
    std::string* error = nullptr);
bool updateCrimePointGpuColorBuffer(const std::vector<ImU32>& colors_rgba, std::string* error = nullptr);
bool updateCrimePointGpuGlyphBuffer(const std::vector<uint32_t>& glyph_codes, std::string* error = nullptr);
void clearCrimePointGpuBuffers();
bool configureCrimePointGpuDrawState(const ParcelGpuDrawConfig& config, std::string* error = nullptr);
void clearCrimePointGpuDrawState();
bool crimePointGpuDrawActive();
void enqueueCrimePointGpuDraw(ImDrawList* draw_list);
