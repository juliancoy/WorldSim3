#pragma once

#include <cstddef>

struct ProcessMemoryTrimResult {
    bool supported = false;
    bool attempted = false;
    bool released = false;
};

void configureProcessAllocatorForWorldSim();
ProcessMemoryTrimResult trimProcessHeap();

template <typename T>
void releaseContainerStorage(T& value) {
    T empty;
    value.swap(empty);
}
