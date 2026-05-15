#pragma once

#include "imgui.h"
#include "cache_io.h"

#include <cstdint>
#include <string>
#include <vector>

int runWorldSim3App(int argc, char** argv);

struct ParcelGpuResidencyStatus {
    bool resident = false;
    uint32_t render_features = 0;
    uint32_t vertices = 0;
    uint32_t indices = 0;
    uint32_t colors = 0;
    std::string source_signature;
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
