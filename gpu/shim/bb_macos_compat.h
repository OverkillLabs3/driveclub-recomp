// bbport (macOS): force-included into the GPU library on macOS (gpu/CMakeLists.txt), like
// bb_win_compat.h on Windows. The few Linux calls the video core uses, mapped to Darwin.
#pragma once
#if defined(__APPLE__)
#include <pthread.h>
#include <stdint.h>
#include <sys/types.h>

// Linux thread ids are used only as labels (diagnostics, the pipeline stage owners).
static inline pid_t bb_mac_gettid(void) {
    uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    return static_cast<pid_t>(id);
}
#define gettid bb_mac_gettid
#endif
