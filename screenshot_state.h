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
    bool ok = false;
    std::string path;
    std::string error;
};
