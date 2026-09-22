#include "host_context.h"

#if defined(__SWITCH__)
#include <atomic>
#include <cstdio>
void SwitchBootLogExternal(const char* text) noexcept;
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif (defined(__APPLE__) || defined(__SWITCH__)) && defined(__aarch64__)
#if defined(__SWITCH__)
#include <switch.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

extern "C" void mkw_co_switch(void** targetSp, void** sourceSp);
extern "C" void* mkw_co_init(void* stackTop, void (*entry)(void*), void* argument);
#elif defined(__linux__)
#include <libco.h>

#include <cstdlib>
#include <unordered_map>
#else
#error "HostContext needs a supported cooperative-context backend"
#endif

namespace HostContext {

#if defined(_WIN32)

namespace {
thread_local bool g_convertedScheduler = false;
}

bool InitializeScheduler(Handle* scheduler)
{
    void* context = ConvertThreadToFiber(nullptr);
    g_convertedScheduler = context != nullptr;
    if (!context) {
        context = GetCurrentFiber();
    }
    *scheduler = context;
    return context != nullptr;
}

void ShutdownScheduler(Handle scheduler)
{
    if (scheduler && g_convertedScheduler) {
        ConvertFiberToThread();
    }
    g_convertedScheduler = false;
}

Handle Create(std::size_t stackSize, Entry entry, void* argument)
{
    return CreateFiber(stackSize, entry, argument);
}

void Destroy(Handle context)
{
    if (context) {
        DeleteFiber(context);
    }
}

bool IsCurrent(Handle context)
{
    return context != nullptr && GetCurrentFiber() == context;
}

void Switch(Handle target)
{
    SwitchToFiber(target);
}

#elif (defined(__APPLE__) || defined(__SWITCH__)) && defined(__aarch64__)

namespace {
struct Context {
    void* savedStackPointer = nullptr;
    void* stack = nullptr;
    std::size_t stackSize = 0;
};

// Guest scheduling is confined to the initialized main host thread. Keeping
// this as ordinary process state also avoids relying on Darwin TLS internals
// while executing on a manually managed stack.
Context* g_current = nullptr;
}

bool InitializeScheduler(Handle* scheduler)
{
    auto* context = new Context();
    g_current = context;
    *scheduler = context;
    return true;
}

void ShutdownScheduler(Handle scheduler)
{
    auto* context = static_cast<Context*>(scheduler);
    if (g_current == context) {
        g_current = nullptr;
    }
    delete context;
}

Handle Create(std::size_t stackSize, Entry entry, void* argument)
{
    auto* context = new Context();
#if defined(__SWITCH__)
    const std::size_t guardSize = 0x1000;
#else
    const std::size_t guardSize = static_cast<std::size_t>(getpagesize());
#endif
    const std::size_t totalSize = stackSize + guardSize;
#if defined(__SWITCH__)
    // Back the coroutine stack with ordinary page-aligned heap memory.
    //
    // This used virtmemFindStack, which only *finds* a free slice of address
    // space in the stack region: it maps nothing, and also expects the caller
    // to hold the virtmem lock and add a reservation. In practice it returned
    // null for the first thread the game created, so that thread got no fiber,
    // SelectThread fell back to loading its registers onto the main thread's
    // stack, and the main thread could never be resumed - the boot deadlock.
    //
    // These are cooperative stacks switched by our own mkw_co_switch, not
    // kernel threads, so any RW memory works and the stack region is not
    // required. The cost is no guard page, so an overflow corrupts the heap
    // instead of faulting; the 1 MiB size already carries generous headroom.
    (void)guardSize;
    context->stack = std::aligned_alloc(0x1000, stackSize);
    if (!context->stack) {
        delete context;
        return nullptr;
    }
#else
    context->stack = mmap(nullptr, totalSize, PROT_READ | PROT_WRITE,
                          MAP_ANON | MAP_PRIVATE, -1, 0);
    if (context->stack == MAP_FAILED) {
        delete context;
        return nullptr;
    }
    // Fault on stack overflow instead of corrupting the preceding mapping.
    if (mprotect(context->stack, guardSize, PROT_NONE) != 0) {
        munmap(context->stack, totalSize);
        delete context;
        return nullptr;
    }
#endif
    context->stackSize = totalSize;

    // virtmemFindStack's guard pages sit outside the returned `stackSize`-byte
    // slice on both sides (unlike the mmap path below, whose single guard is
    // folded into `totalSize` at the low end); the usable region therefore
    // ends at `stackSize`, not `totalSize` - using the latter here put the
    // initial stack pointer `guardSize` bytes into the trailing unmapped
    // guard page, so mkw_co_init's first register-save write faulted.
#if defined(__SWITCH__)
    auto* stackTop = static_cast<char*>(context->stack) + stackSize;
#else
    auto* stackTop = static_cast<char*>(context->stack) + totalSize;
#endif
    context->savedStackPointer = mkw_co_init(stackTop, entry, argument);
    return context;
}

void Destroy(Handle context)
{
    auto* nativeContext = static_cast<Context*>(context);
    if (!nativeContext) {
        return;
    }
    if (nativeContext->stack) {
#if defined(__SWITCH__)
        std::free(nativeContext->stack);
#else
        munmap(nativeContext->stack, nativeContext->stackSize);
#endif
    }
    delete nativeContext;
}

bool IsCurrent(Handle context)
{
    return context != nullptr && context == g_current;
}

void Switch(Handle target)
{
    auto* destination = static_cast<Context*>(target);
    Context* source = g_current;
#if defined(__SWITCH__)
    // A silent no-op here leaves the selected guest thread dequeued but never
    // resumed, which is what deadlocks boot. Log when it happens.
    if (!destination || destination == source) {
        static std::atomic<int> noopLog{0};
        const int index = noopLog.fetch_add(1, std::memory_order_relaxed);
        if (index < 10) {
            char trace[144];
            std::snprintf(trace, sizeof(trace), "[ctx] Switch NO-OP #%d dest=%p cur=%p", index,
                          static_cast<const void*>(destination), static_cast<const void*>(source));
            SwitchBootLogExternal(trace);
        }
    }
#endif
    if (!destination || destination == source) {
        return;
    }

    g_current = destination;
    mkw_co_switch(&destination->savedStackPointer, &source->savedStackPointer);
    g_current = source;
}

#elif defined(__linux__)

namespace {
struct Context {
    cothread_t native = nullptr;
    Entry entry = nullptr;
    void* argument = nullptr;
    bool ownsNative = false;
};

thread_local Context* g_current = nullptr;
thread_local std::unordered_map<cothread_t, Context*> g_contexts;

void ContextEntry()
{
    const auto found = g_contexts.find(co_active());
    if (found == g_contexts.end() || !found->second || !found->second->entry) {
        std::abort();
    }

    Context* context = found->second;
    g_current = context;
    context->entry(context->argument);

    // A guest fiber must return through FiberProc's scheduler handoff. There
    // is no valid native caller to return to from libco's entry trampoline.
    std::abort();
}
} // namespace

bool InitializeScheduler(Handle* scheduler)
{
    auto* context = new Context();
    context->native = co_active();
    if (!context->native) {
        delete context;
        return false;
    }

    g_current = context;
    g_contexts.emplace(context->native, context);
    *scheduler = context;
    return true;
}

void ShutdownScheduler(Handle scheduler)
{
    auto* context = static_cast<Context*>(scheduler);
    if (!context) {
        return;
    }

    g_contexts.erase(context->native);
    if (g_current == context) {
        g_current = nullptr;
    }
    delete context;
}

Handle Create(std::size_t stackSize, Entry entry, void* argument)
{
    auto* context = new Context();
    context->entry = entry;
    context->argument = argument;
    context->native = co_create(static_cast<unsigned int>(stackSize), ContextEntry);
    context->ownsNative = context->native != nullptr;
    if (!context->native) {
        delete context;
        return nullptr;
    }

    g_contexts.emplace(context->native, context);
    return context;
}

void Destroy(Handle context)
{
    auto* nativeContext = static_cast<Context*>(context);
    if (!nativeContext) {
        return;
    }

    g_contexts.erase(nativeContext->native);
    if (nativeContext->ownsNative) {
        co_delete(nativeContext->native);
    }
    delete nativeContext;
}

bool IsCurrent(Handle context)
{
    return context != nullptr && context == g_current;
}

void Switch(Handle target)
{
    auto* destination = static_cast<Context*>(target);
    Context* source = g_current;
    if (!destination || destination == source) {
        return;
    }

    g_current = destination;
    co_switch(destination->native);
    g_current = source;
}

#endif

} // namespace HostContext
