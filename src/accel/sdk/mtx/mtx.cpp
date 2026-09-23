// PSMTX: the SDK's paired-single matrix library, natively.
//
// These are hand-written assembly in Nintendo's SDK and identical in every
// build of it, which is why one native serves every game. The arithmetic is
// reproduced instruction for instruction from the published decompilation of
// the Dolphin SDK (doldecomp/dolsdk2004, src/mtx/mtx.c, CC0): every multiply
// and add below is fused, in the order ps_muls0/ps_madds0/ps_madds1 put them,
// because the results reach physics and replays, where a differing last bit is
// a different race.
#include "wiinx/accel/sdk.hpp"
#include "wiinx/core/guest.hpp"
#include "wiinx/core/host.hpp"

#include <cmath>

namespace wiinx::sdk::mtx {
namespace {

// A guest Mtx is 3 rows of 4 floats, row-major.
constexpr u32 kRowBytes = 4 * sizeof(f32);

f32 at(GuestAddr m, int row, int col) noexcept {
    return load<f32>(m + static_cast<u32>(row) * kRowBytes + static_cast<u32>(col) * 4);
}

void put(GuestAddr m, int row, int col, f32 value) noexcept {
    store<f32>(m + static_cast<u32>(row) * kRowBytes + static_cast<u32>(col) * 4, value);
}

// ab = a * b, as PSMTXConcat computes it. Reading a row of `a` into a pair and
// multiplying `b`'s rows by each of its lanes is what ps_muls0 and ps_madds
// do; the translation column adds a[i][3] through a fused add against the
// constant (0, 1), which is why it is an fma here too and not a plain add.
void concat(GuestAddr a, GuestAddr b, GuestAddr ab) noexcept {
    f32 out[3][4];
    for (int row = 0; row < 3; ++row) {
        const f32 a0 = at(a, row, 0);
        const f32 a1 = at(a, row, 1);
        const f32 a2 = at(a, row, 2);
        const f32 a3 = at(a, row, 3);
        for (int col = 0; col < 4; ++col) {
            f32 value = a0 * at(b, 0, col);
            value = std::fma(a1, at(b, 1, col), value);
            value = std::fma(a2, at(b, 2, col), value);
            if (col == 3) {
                value = std::fma(a3, 1.0f, value);
            }
            out[row][col] = value;
        }
    }
    // Written only once the reads are done: a game may concatenate a matrix
    // with itself, and the original reads all of both before storing.
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) {
            put(ab, row, col, out[row][col]);
        }
    }
}

void identity(GuestAddr m) noexcept {
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) {
            put(m, row, col, row == col ? 1.0f : 0.0f);
        }
    }
}

void PSMTXIdentity(Cpu* cpu) {
    const Host& h = host();
    identity(h.gpr(cpu, 3));
}

void PSMTXConcat(Cpu* cpu) {
    const Host& h = host();
    concat(h.gpr(cpu, 3), h.gpr(cpu, 4), h.gpr(cpu, 5));
}

void PSMTXConcatArray(Cpu* cpu) {
    const Host& h = host();
    const GuestAddr a = h.gpr(cpu, 3);
    GuestAddr src = h.gpr(cpu, 4);
    GuestAddr dst = h.gpr(cpu, 5);
    for (u32 i = h.gpr(cpu, 6); i != 0; --i) {
        concat(a, src, dst);
        src += 3 * kRowBytes;
        dst += 3 * kRowBytes;
    }
}

// The float arguments of these arrive in f1, f2, f3: the ABI passes a float as
// a double, which is what Host::fpr hands back.
f32 farg(const Host& h, Cpu* cpu, int index) noexcept {
    return static_cast<f32>(h.fpr(cpu, index));
}

void copy(GuestAddr src, GuestAddr dst) noexcept {
    if (src == dst) {
        return;
    }
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) {
            put(dst, row, col, at(src, row, col));
        }
    }
}

void PSMTXCopy(Cpu* cpu) {
    const Host& h = host();
    copy(h.gpr(cpu, 3), h.gpr(cpu, 4));
}

// The rotation part transposes; the translation column is dropped, as the
// original does - a transpose of a 3x4 affine matrix keeps only the 3x3.
void PSMTXTranspose(Cpu* cpu) {
    const Host& h = host();
    const GuestAddr src = h.gpr(cpu, 3);
    const GuestAddr dst = h.gpr(cpu, 4);
    f32 m[3][3];
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            m[row][col] = at(src, row, col);
        }
    }
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            put(dst, row, col, m[col][row]);
        }
        put(dst, row, 3, 0.0f);
    }
}

void PSMTXTrans(Cpu* cpu) {
    const Host& h = host();
    const GuestAddr m = h.gpr(cpu, 3);
    identity(m);
    put(m, 0, 3, farg(h, cpu, 1));
    put(m, 1, 3, farg(h, cpu, 2));
    put(m, 2, 3, farg(h, cpu, 3));
}

// The rotation part passes through untouched and the translation column gains
// the offset, which is what C_MTXTransApply does element by element.
void PSMTXTransApply(Cpu* cpu) {
    const Host& h = host();
    const GuestAddr src = h.gpr(cpu, 3);
    const GuestAddr dst = h.gpr(cpu, 4);
    const f32 offset[3] = {farg(h, cpu, 1), farg(h, cpu, 2), farg(h, cpu, 3)};
    f32 rot[3][3];
    f32 trans[3];
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            rot[row][col] = at(src, row, col);
        }
        trans[row] = at(src, row, 3);
    }
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            put(dst, row, col, rot[row][col]);
        }
        put(dst, row, 3, trans[row] + offset[row]);
    }
}

void PSMTXScale(Cpu* cpu) {
    const Host& h = host();
    const GuestAddr m = h.gpr(cpu, 3);
    identity(m);
    put(m, 0, 0, farg(h, cpu, 1));
    put(m, 1, 1, farg(h, cpu, 2));
    put(m, 2, 2, farg(h, cpu, 3));
}

// Every element of a row scaled, the translation column included.
void PSMTXScaleApply(Cpu* cpu) {
    const Host& h = host();
    const GuestAddr src = h.gpr(cpu, 3);
    const GuestAddr dst = h.gpr(cpu, 4);
    const f32 scale[3] = {farg(h, cpu, 1), farg(h, cpu, 2), farg(h, cpu, 3)};
    f32 out[3][4];
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) {
            out[row][col] = at(src, row, col) * scale[row];
        }
    }
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) {
            put(dst, row, col, out[row][col]);
        }
    }
}

}  // namespace

void concat_at(GuestAddr a, GuestAddr b, GuestAddr ab) noexcept { concat(a, b, ab); }

// MTX is the same assembly in every build of the SDK, so one entry serves all
// of them; a game's bindings say where it lives.
extern const Native kMtxNatives[] = {
    WIINX_NATIVE("PSMTXIdentity", kMtx_Any, PSMTXIdentity),
    WIINX_NATIVE("PSMTXConcat", kMtx_Any, PSMTXConcat),
    WIINX_NATIVE("PSMTXConcatArray", kMtx_Any, PSMTXConcatArray),
    WIINX_NATIVE("PSMTXCopy", kMtx_Any, PSMTXCopy),
    WIINX_NATIVE("PSMTXTranspose", kMtx_Any, PSMTXTranspose),
    WIINX_NATIVE("PSMTXTrans", kMtx_Any, PSMTXTrans),
    WIINX_NATIVE("PSMTXTransApply", kMtx_Any, PSMTXTransApply),
    WIINX_NATIVE("PSMTXScale", kMtx_Any, PSMTXScale),
    WIINX_NATIVE("PSMTXScaleApply", kMtx_Any, PSMTXScaleApply),
};
extern const std::size_t kMtxNativeCount = std::size(kMtxNatives);

}  // namespace wiinx::sdk::mtx
