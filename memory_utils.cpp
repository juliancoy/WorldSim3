#include "memory_utils.h"

#include <cstdlib>

#if defined(__linux__)
#include <malloc.h>
#endif

void configureProcessAllocatorForWorldSim() {
#if defined(__linux__)
    int arena_max = 2;
    if (const char* env = std::getenv("WS3_MALLOC_ARENA_MAX")) {
        const int parsed = std::atoi(env);
        if (parsed > 0) arena_max = parsed;
    }
    mallopt(M_ARENA_MAX, arena_max);

    // Keep large hydration/cache transients from staying resident indefinitely.
    mallopt(M_TRIM_THRESHOLD, 256 * 1024);
    mallopt(M_MMAP_THRESHOLD, 1024 * 1024);
#endif
}

ProcessMemoryTrimResult trimProcessHeap() {
    ProcessMemoryTrimResult result;
#if defined(__linux__)
    result.supported = true;
    result.attempted = true;
    result.released = malloc_trim(0) != 0;
#endif
    return result;
}
