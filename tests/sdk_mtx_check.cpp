// PSMTXConcat against matrices whose product is known, and against the
// property every concatenation has: identity on either side changes nothing.
#include "wiinx/accel/sdk.hpp"
#include "wiinx/core/guest.hpp"
#include "wiinx/core/native.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace wiinx;

namespace {
std::vector<u8> g_memory(0x10000, 0);
u32 g_gpr[32];

u32 gpr(Cpu*, int i) { return g_gpr[i]; }
void set_gpr(Cpu*, int i, u32 v) { g_gpr[i] = v; }

void write_mtx(GuestAddr at, const float m[3][4]) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) store<f32>(at + (r * 4 + c) * 4, m[r][c]);
}

void read_mtx(GuestAddr at, float out[3][4]) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) out[r][c] = load<f32>(at + (r * 4 + c) * 4);
}

bool same(const float a[3][4], const float b[3][4]) {
    return std::memcmp(a, b, sizeof(float) * 12) == 0;
}
}  // namespace

int main() {
    Host h;
    h.memory = g_memory.data();
    h.gpr = gpr;
    h.set_gpr = set_gpr;
    set_host(h);

    constexpr GuestAddr kA = 0x100, kB = 0x200, kOut = 0x300;

    // A rotation-and-translation against a scale: a product worked out by hand.
    const float a[3][4] = {{0, -1, 0, 5}, {1, 0, 0, 6}, {0, 0, 1, 7}};
    const float b[3][4] = {{2, 0, 0, 0}, {0, 3, 0, 0}, {0, 0, 4, 0}};
    write_mtx(kA, a);
    write_mtx(kB, b);
    sdk::mtx::concat_at(kA, kB, kOut);

    float got[3][4];
    read_mtx(kOut, got);
    const float expect[3][4] = {{0, -3, 0, 5}, {2, 0, 0, 6}, {0, 0, 4, 7}};
    assert(same(got, expect));

    // Identity on the right, then on the left.
    const float identity[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    write_mtx(kB, identity);
    sdk::mtx::concat_at(kA, kB, kOut);
    read_mtx(kOut, got);
    assert(same(got, a));

    write_mtx(kB, identity);
    sdk::mtx::concat_at(kB, kA, kOut);
    read_mtx(kOut, got);
    assert(same(got, a));

    // Concatenating a matrix with itself: the original reads both operands
    // before it stores, so aliasing the destination must not corrupt it.
    write_mtx(kA, a);
    sdk::mtx::concat_at(kA, kA, kA);
    read_mtx(kA, got);
    const float squared[3][4] = {{-1, 0, 0, -1}, {0, -1, 0, 11}, {0, 0, 1, 14}};
    assert(same(got, squared));

    // Scaling every row, translation column included, and then undoing it.
    write_mtx(kA, a);
    g_gpr[3] = kA; g_gpr[4] = kOut;
    Host withFloats = h;
    static double fprs[32] = {};
    withFloats.fpr = [](Cpu*, int i) { return fprs[i]; };
    withFloats.set_fpr = [](Cpu*, int i, double v) { fprs[i] = v; };
    set_host(withFloats);
    add_natives(sdk::natives());

    fprs[1] = 2.0; fprs[2] = 4.0; fprs[3] = 0.5;
    const Native* scaleApply = find_native("PSMTXScaleApply", sdk::kMtx_Any.id);
    assert(scaleApply != nullptr);
    scaleApply->entry(nullptr);
    read_mtx(kOut, got);
    for (int r = 0; r < 3; ++r) {
        const float by = r == 0 ? 2.0f : (r == 1 ? 4.0f : 0.5f);
        for (int c = 0; c < 4; ++c) {
            assert(got[r][c] == a[r][c] * by);
        }
    }

    // A translation applied on top of an existing one adds to it and leaves
    // the rotation alone.
    fprs[1] = 1.0; fprs[2] = -2.0; fprs[3] = 3.0;
    const Native* transApply = find_native("PSMTXTransApply", sdk::kMtx_Any.id);
    assert(transApply != nullptr);
    transApply->entry(nullptr);
    read_mtx(kOut, got);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            assert(got[r][c] == a[r][c]);
        }
    }
    assert(got[0][3] == a[0][3] + 1.0f);
    assert(got[1][3] == a[1][3] - 2.0f);
    assert(got[2][3] == a[2][3] + 3.0f);

    std::printf("PSMTXConcat, ScaleApply, TransApply ok\n");
    return 0;
}
