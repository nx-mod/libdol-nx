#include "guest_jmp.h"

#include "abi_bridge.h"
#include "fiber_manager.h"
#include "host_context.h"
#include "memory.h"
#include "runtime_log.h"

#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <vector>

#if defined(__SWITCH__)
#include <malloc.h>
void SwitchBootLogExternal(const char* text) noexcept;
#endif

namespace GuestJmp {
namespace {

// The jump buffer layout of the recognised pair: LR at +0, SP at +8.
constexpr uint32_t kBufferLr = 0;
constexpr uint32_t kBufferSp = 8;
constexpr std::size_t kStackSize = 8 * 1024 * 1024;  // as a guest thread's

struct Coroutine {
    HostContext::Handle handle = nullptr;
    bool owned = false;           // made here (a guest thread's own context is not)
    uint32_t startSp = 0;         // the guest stack it was started on
    CpuContext* ctx = nullptr;    // the register file it runs on
    std::jmp_buf start{};         // top of its entry, to start over
    uint32_t pendingBuffer = 0;   // resumed to longjmp into this guest buffer
    bool pendingStart = false;    // resumed to run from ctx->lr on a fresh stack
};

struct Armed {
    Coroutine* owner = nullptr;
    uint32_t lr = 0;
    uint32_t sp = 0;
    std::jmp_buf host{};
};

std::mutex g_mutex;
std::unordered_map<HostContext::Handle, Coroutine*> g_byHandle;
std::vector<Coroutine*> g_started;
std::unordered_map<uint32_t, Armed> g_armed;

Coroutine* CurrentCoroutine() {
    const HostContext::Handle current = HostContext::Current();
    std::lock_guard<std::mutex> lock(g_mutex);
    auto& slot = g_byHandle[current];
    if (!slot) {
        slot = new Coroutine();
        slot->handle = current;
    }
    return slot;
}

void Log(const char* text) {
#if defined(__SWITCH__)
    char line[160];
    std::snprintf(line, sizeof(line), "[jmp] %s", text);
    SwitchBootLogExternal(line);
#else
    RT_LOG(RT_TAG_OS) << "[jmp] " << text << std::endl;
#endif
}

[[noreturn]] void Fatal(const char* what) {
    Log(what);
    std::fflush(stderr);
    std::abort();
}

[[noreturn]] void ResumeHere(Coroutine* self);

// Hands the guest thread to `target` and parks `self` until a jump comes back.
[[noreturn]] void SwitchTo(Coroutine* self, Coroutine* target, CpuContext* ctx) {
    target->ctx = ctx;
    Fiber::GuestFiberManager::RebindHostContext(self->handle, target->handle);
    HostContext::Switch(target->handle);
    ResumeHere(self);
}

[[noreturn]] void ResumeHere(Coroutine* self) {
    if (self->pendingStart) {
        std::longjmp(self->start, 1);
    }
    const uint32_t buffer = self->pendingBuffer;
    self->pendingBuffer = 0;
    std::jmp_buf* host = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (auto it = g_armed.find(buffer); it != g_armed.end() && it->second.owner == self) {
            host = &it->second.host;
        }
    }
    if (!host) {
        Fatal("a parked stack resumed with nothing for it to do");
    }
    std::longjmp(*host, 1);
}

void Entry(void* argument) {
    auto* self = static_cast<Coroutine*>(argument);
    setjmp(self->start);
    self->pendingStart = false;
    {
        // Frames armed on this stack before it started over are gone.
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto it = g_armed.begin(); it != g_armed.end();) {
            it = it->second.owner == self ? g_armed.erase(it) : std::next(it);
        }
    }
    // The guest's `blr` out of the entry goes to LR, which still holds the entry
    // itself: on hardware the function starts over, and so does this.
    for (;;) {
        CpuContext* const ctx = self->ctx;
        InvokeIndirectCpu(ctx->lr, ctx);
    }
}

// Whether anything can still resume `c`: an armed buffer of its whose guest
// copy still holds the LR and SP its setjmp stored. Entries the guest has since
// overwritten or freed are dropped. Call with g_mutex held.
bool Reachable(Coroutine* c) {
    bool live = false;
    for (auto it = g_armed.begin(); it != g_armed.end();) {
        if (it->second.owner != c) {
            ++it;
            continue;
        }
        uint32_t lr = 0;
        uint32_t sp = 0;
        if (Memory::TryRead32(it->first + kBufferLr, lr) && Memory::TryRead32(it->first + kBufferSp, sp) &&
            lr == it->second.lr && sp == it->second.sp) {
            live = true;
            ++it;
        } else {
            it = g_armed.erase(it);
        }
    }
    return live;
}

Coroutine* StackFor(Coroutine* self, uint32_t sp) {
    std::lock_guard<std::mutex> lock(g_mutex);
    // The guest restarting a stack it ran before: whatever that coroutine was
    // doing is gone for the guest too, so it starts over.
    for (Coroutine* c : g_started) {
        if (c->startSp == sp) {
            return c;
        }
    }
    // A finished task's coroutine (nothing left that can jump back into it)
    // takes the new stack: a task system makes one per task.
    for (Coroutine* c : g_started) {
        if (c != self && !Reachable(c)) {
            c->startSp = sp;
            return c;
        }
    }
    auto* c = new Coroutine();
    c->owned = true;
    c->startSp = sp;
    c->handle = HostContext::Create(kStackSize, Entry, c);
    if (!c->handle) {
        delete c;
        return nullptr;
    }
    g_byHandle[c->handle] = c;
    g_started.push_back(c);
    char line[128];
#if defined(__SWITCH__)
    const struct mallinfo heap = mallinfo();
    std::snprintf(line, sizeof(line), "coroutine stacks: %zu (newest for guest sp 0x%08X), heap %zu MB in use",
                  g_started.size(), sp, static_cast<std::size_t>(heap.uordblks) >> 20);
#else
    std::snprintf(line, sizeof(line), "coroutine stacks: %zu (newest for guest sp 0x%08X)", g_started.size(), sp);
#endif
    Log(line);
    return c;
}

}  // namespace

std::jmp_buf* Arm(CpuContext* ctx) {
    Coroutine* self = CurrentCoroutine();
    std::lock_guard<std::mutex> lock(g_mutex);
    Armed& armed = g_armed[ctx->gpr[3]];
    armed.owner = self;
    armed.lr = ctx->lr;
    armed.sp = ctx->gpr[1];
    return &armed.host;
}

void Jump(CpuContext* ctx, uint32_t guestBuffer) {
    Coroutine* self = CurrentCoroutine();
    uint32_t lr = 0;
    uint32_t sp = 0;
    if (!Memory::TryRead32(guestBuffer + kBufferLr, lr) || !Memory::TryRead32(guestBuffer + kBufferSp, sp)) {
        Fatal("longjmp to an unreadable buffer");
    }

    Coroutine* owner = nullptr;
    std::jmp_buf* host = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (auto it = g_armed.find(guestBuffer);
            it != g_armed.end() && it->second.lr == lr && it->second.sp == sp) {
            owner = it->second.owner;
            host = &it->second.host;
        }
    }

    if (owner == self) {
        std::longjmp(*host, 1);
    }
    if (owner) {
        owner->pendingBuffer = guestBuffer;
        SwitchTo(self, owner, ctx);
    }

    // A buffer no host setjmp made as it is: run from its LR on its stack.
    Coroutine* fresh = StackFor(self, sp);
    if (!fresh) {
        Fatal("no memory for a coroutine stack");
    }
    fresh->ctx = ctx;
    if (fresh == self) {
        std::longjmp(self->start, 1);
    }
    fresh->pendingStart = true;
    SwitchTo(self, fresh, ctx);
}

}  // namespace GuestJmp
