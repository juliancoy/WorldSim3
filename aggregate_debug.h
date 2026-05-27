#pragma once

#include <atomic>
#include <cstdlib>
#include <cstring>

inline bool parseWorldsimDebugBool(const char* value) {
    return value &&
        value[0] != '\0' &&
        std::strcmp(value, "0") != 0 &&
        std::strcmp(value, "false") != 0 &&
        std::strcmp(value, "FALSE") != 0 &&
        std::strcmp(value, "off") != 0 &&
        std::strcmp(value, "OFF") != 0;
}

inline std::atomic<bool>& worldsimGpuAggregateDebugFlag() {
    static std::atomic<bool> enabled(parseWorldsimDebugBool(std::getenv("WORLD_SIM3_DEBUG_GPU_AGGREGATE")));
    return enabled;
}

inline bool worldsimGpuAggregateDebugEnabled() {
    return worldsimGpuAggregateDebugFlag().load(std::memory_order_relaxed);
}

inline void setWorldsimGpuAggregateDebug(bool enabled) {
    worldsimGpuAggregateDebugFlag().store(enabled, std::memory_order_relaxed);
}
