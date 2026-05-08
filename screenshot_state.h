#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>

struct ScreenshotRequestState {
    std::mutex mutex;
    std::condition_variable cv;
    bool pending = false;
    uint64_t req_id = 0;
    uint64_t done_id = 0;
    bool request_native = false;
    bool ok = false;
    std::string path;
    std::string error;
    uint32_t native_width = 0;
    uint32_t native_height = 0;
    uint32_t logical_width = 0;
    uint32_t logical_height = 0;
    uint32_t output_width = 0;
    uint32_t output_height = 0;
    float framebuffer_scale_x = 1.0f;
    float framebuffer_scale_y = 1.0f;
};
