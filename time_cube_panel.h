#pragma once

#include "time_cube.h"
#include "types.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct TimeCubePanelContext {
    TimeCubeService* service = nullptr;
    std::vector<LayerDef>* layers = nullptr;
    TimeCubeResult* result = nullptr;
    bool* loaded = nullptr;
    std::string* status = nullptr;
    std::mutex* mutex = nullptr;
    std::thread* worker = nullptr;
    std::atomic<bool>* running = nullptr;
    std::atomic<bool>* done = nullptr;
    std::vector<bool>* selected = nullptr;
    int* year_min = nullptr;
    int* year_max = nullptr;
    int* normalize_mode = nullptr;
    bool* show_excluded = nullptr;
};

void drawTimeCubeTab(TimeCubePanelContext& ctx);
