#include "cpu_affinity.h"

#include <algorithm>
#include <string>

#if defined(__linux__)
#include <sched.h>
#include <unistd.h>
#endif

bool applyReservedCpuCores(int reserve_cores, std::string& message) {
    if (reserve_cores <= 0) {
        message.clear();
        return true;
    }

#if defined(__linux__)
    cpu_set_t current_mask;
    CPU_ZERO(&current_mask);
    if (sched_getaffinity(0, sizeof(current_mask), &current_mask) != 0) {
        message = "sched_getaffinity failed";
        return false;
    }

    int available = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &current_mask)) available++;
    }
    if (available <= 1) {
        message = "CPU affinity unchanged: only one CPU is available";
        return false;
    }

    const int to_reserve = std::min(reserve_cores, available - 1);
    cpu_set_t new_mask = current_mask;
    int reserved = 0;
    std::string reserved_list;
    for (int cpu = 0; cpu < CPU_SETSIZE && reserved < to_reserve; ++cpu) {
        if (!CPU_ISSET(cpu, &new_mask)) continue;
        CPU_CLR(cpu, &new_mask);
        if (!reserved_list.empty()) reserved_list += ",";
        reserved_list += std::to_string(cpu);
        reserved++;
    }

    if (sched_setaffinity(0, sizeof(new_mask), &new_mask) != 0) {
        message = "sched_setaffinity failed";
        return false;
    }

    message = "reserved CPU core" + std::string(reserved == 1 ? " " : "s ") +
        reserved_list + " for other OS processes";
    return true;
#else
    message = "CPU reservation is only supported on Linux";
    return false;
#endif
}
