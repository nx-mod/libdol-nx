// Raises the default stack for every thread created through pthread_create on
// Switch, via the linker's --wrap=pthread_create (see dawn/CMakeLists.txt).
//
// std::thread passes a null attribute, and libnx then falls back to a 128 KiB
// stack. Aurora's shader-compile workers run Tint there, and Tint's uniformity
// analysis recurses deeply enough on real MKW shaders to run straight off the
// end of it: the first crash on hardware had SP 33 KiB below the bottom of a
// 132 KiB stack region, inside tint::resolver::AnalyzeUniformity. Chromium runs
// Tint on multi-megabyte stacks for the same reason.
//
// Wrapping the one entry point covers every thread in the process - Aurora,
// Dawn, Tint and libstdc++ - rather than patching each spawner and missing one.
#if defined(__SWITCH__)

#include <pthread.h>
#include <switch.h>

#include <atomic>
#include <cstddef>
#include <cstdio>

// Defined at global scope in main.cpp.
void SwitchBootLogExternal(const char* text) noexcept;
bool SwitchDevLoggingEnabled() noexcept;

namespace {
// Core placement. libnx creates pthreads with cpuid -2, "the default core for
// the current process" - the game thread's core. Horizon also does not
// time-slice threads at normal priority on a core, so every worker (Dawn and
// Aurora frame encoding, presentation, the shader compiler, the audio mixer)
// shared core 0 with the game and ran only when the game thread blocked:
// "background" work was serial, and cores 1 and 2 sat idle.
//
// Core 0 stays the game thread's. Workers alternate their preferred core
// between 1 and 2 and may run on either.
constexpr u32 kWorkerCoreMask = (1u << 1) | (1u << 2);
std::atomic<unsigned> g_nextWorkerCore{0};

struct ThreadStart {
    void* (*start)(void*);
    void* arg;
};

void* WorkerTrampoline(void* raw) {
    const ThreadStart startInfo = *static_cast<ThreadStart*>(raw);
    delete static_cast<ThreadStart*>(raw);

    const int preferred = 1 + static_cast<int>(g_nextWorkerCore.fetch_add(1, std::memory_order_relaxed) % 2);
    const Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, preferred, kWorkerCoreMask);
    if (SwitchDevLoggingEnabled()) {
        char line[96];
        std::snprintf(line, sizeof(line), "[thread] worker -> core %d (rc=0x%X, now on %u)", preferred,
                      static_cast<unsigned>(rc), static_cast<unsigned>(svcGetCurrentProcessorNumber()));
        SwitchBootLogExternal(line);
    }
    return startInfo.start(startInfo.arg);
}
}  // namespace

extern "C" {
int __real_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*),
                          void* arg);

// Starts `start` through WorkerTrampoline so the new thread moves off core 0.
static int CreateOnWorkerCore(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*),
                              void* arg) {
    auto* startInfo = new ThreadStart{start, arg};
    const int result = __real_pthread_create(thread, attr, WorkerTrampoline, startInfo);
    if (result != 0) {
        delete startInfo;
    }
    return result;
}

// Callers that ask for a specific stack are left alone; only the unspecified
// default is raised. Stacks come from the heap, which has ample room: ~26
// worker threads at this size is roughly 100 MiB.
constexpr std::size_t kDefaultThreadStackSize = 4 * 1024 * 1024;

int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*start)(void*),
                          void* arg) {
    if (attr != nullptr) {
        std::size_t requested = 0;
        if (pthread_attr_getstacksize(attr, &requested) == 0 && requested >= kDefaultThreadStackSize) {
            return CreateOnWorkerCore(thread, attr, start, arg);
        }
    }

    pthread_attr_t sized;
    if (attr != nullptr) {
        sized = *attr;
    } else if (pthread_attr_init(&sized) != 0) {
        return CreateOnWorkerCore(thread, attr, start, arg);
    }
    pthread_attr_setstacksize(&sized, kDefaultThreadStackSize);
    const int result = CreateOnWorkerCore(thread, &sized, start, arg);
    if (attr == nullptr) {
        pthread_attr_destroy(&sized);
    }
    return result;
}
}

#endif // __SWITCH__
