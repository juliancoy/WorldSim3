#pragma once

#include <string>

#if defined(__APPLE__) || defined(__linux__)
#include <pthread.h>
#endif

inline void setCurrentThreadName(const char* name) {
    if (!name || !*name) return;
#if defined(__APPLE__)
    pthread_setname_np(name);
#elif defined(__linux__)
    std::string clipped(name);
    if (clipped.size() > 15) clipped.resize(15);
    pthread_setname_np(pthread_self(), clipped.c_str());
#else
    (void)name;
#endif
}
