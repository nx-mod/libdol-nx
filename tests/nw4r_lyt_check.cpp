// Pane::CalculateMtx on a fake two-pane tree, for both nw4r LYT builds.
#include "wiinx/accel/nw4r.hpp"
#include "wiinx/core/guest.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace wiinx;

namespace {
std::vector<u8> g_memory(0x10000, 0);
u32 g_gpr[32];
int g_calls = 0;

u32 gpr(Cpu*, int i) { return g_gpr[i]; }
void set_gpr(Cpu*, int i, u32 v) { g_gpr[i] = v; }

const Native* g_native = nullptr;
void call(Cpu* cpu, GuestAddr target) {
    ++g_calls;
    assert(target == 0x9000);  // the child's vtable slot 0x10
    g_native->entry(cpu);      // a plain pane's CalculateMtx is this same function
}

void put_u32(u32 a, u32 v) { store<u32>(a, v); }
void put_f32(u32 a, f32 v) { store<f32>(a, v); }

bool near(f32 a, f32 b) { return std::fabs(a - b) < 1e-5f; }

// Builds parent at 0x1000, child at 0x2000, DrawInfo at 0x3000; alpha fields
// at `alphaAt` (0xB4 for 2007, 0xB8 for 2008).
void run(const char* version, u32 alphaAt) {
    std::fill(g_memory.begin(), g_memory.end(), 0);
    g_calls = 0;
    g_native = find_native("nw4r::lyt::Pane::CalculateMtx", version);
    assert(g_native != nullptr);

    const u32 parent = 0x1000, child = 0x2000, info = 0x3000;
    // Child list: parent's sentinel node at +0x14 -> child's link at child+4 -> back.
    put_u32(parent + 0x14, child + 0x04);
    put_u32(child + 0x04, parent + 0x14);
    put_u32(child + 0x14, child + 0x14);  // child has no children
    put_u32(child + 0x0C, parent);
    // Child vtable at 0x8000, CalculateMtx slot at +0x10 -> 0x9000.
    put_u32(child + 0x00, 0x8000);
    put_u32(0x8000 + 0x10, 0x9000);

    for (u32 pane : {parent, child}) {
        put_f32(pane + 0x44, 1.0f);  // scale x
        put_f32(pane + 0x48, 1.0f);  // scale y
        store<u8>(pane + alphaAt + 3, 0x01 | 0x02);  // flag: visible, influenced alpha
    }
    put_f32(parent + 0x2C, 10.0f);  // translate x
    put_f32(child + 0x30, 5.0f);    // translate y
    put_f32(parent + 0x44, 2.0f);   // parent scale x
    store<u8>(parent + alphaAt, 128);
    store<u8>(child + alphaAt, 255);

    // DrawInfo: identity view, global alpha 1, alpha influence on.
    Mtx34 identity{};
    identity.m[0][0] = identity.m[1][1] = identity.m[2][2] = 1.0f;
    store<Mtx34>(info + 0x04, identity);
    put_f32(info + 0x4C, 1.0f);
    store<u8>(info + 0x50, 0x40);

    g_gpr[3] = parent;
    g_gpr[4] = info;
    g_native->entry(nullptr);

    const Mtx34 pg = load<Mtx34>(parent + 0x84);
    const Mtx34 cg = load<Mtx34>(child + 0x84);
    assert(g_calls == 1);
    assert(near(pg.m[0][0], 2.0f) && near(pg.m[0][3], 10.0f));
    // Child global = parent global * child local: x scaled by the parent's 2.
    assert(near(cg.m[0][0], 2.0f) && near(cg.m[1][3], 5.0f) && near(cg.m[0][3], 10.0f));
    // Parent is a root: its global alpha is its own. The child, influenced
    // by a parent at 128, gets 255 * 128/255.
    assert(load<u8>(parent + alphaAt + 1) == 128);
    assert(load<u8>(child + alphaAt + 1) == 128);
    // DrawInfo's global alpha is restored afterwards.
    assert(near(load<f32>(info + 0x4C), 1.0f));
    std::printf("%s ok\n", version);
}
}  // namespace

int main() {
    Host h;
    h.memory = g_memory.data();
    h.gpr = gpr;
    h.set_gpr = set_gpr;
    h.call = call;
    set_host(h);
    add_natives(nw4r::natives());

    run("nw4r.lyt@2007-06-08", 0xB4);
    run("nw4r.lyt@2008-03-08", 0xB8);
}
