// Compile-and-run check of core against a fake guest memory (host build).
#include "wiinx/core/guest.hpp"
#include "wiinx/core/native.hpp"

#include <cassert>
#include <cstdio>
#include <vector>

using namespace wiinx;

struct TestLayout {
    static constexpr Field<u8, 0x00> flag{};
    static constexpr Field<u32, 0x04> value{};
    static constexpr Field<f32, 0x08> scale{};
    static constexpr Field<Mtx34, 0x10> mtx{};
};

int main() {
    std::vector<u8> memory(0x100, 0);
    Host h;
    h.memory = memory.data();
    set_host(h);

    Guest<TestLayout> obj{0x20};
    obj.set(TestLayout::value, 0x11223344u);
    assert(memory[0x24] == 0x11 && memory[0x27] == 0x44);  // big-endian in memory
    assert(obj[TestLayout::value] == 0x11223344u);

    obj.set(TestLayout::scale, 1.5f);
    assert(obj[TestLayout::scale] == 1.5f);

    Mtx34 m{};
    m.m[2][3] = -7.25f;
    obj.set(TestLayout::mtx, m);
    assert(obj[TestLayout::mtx].m[2][3] == -7.25f);

    assert(find_native("nw4r::lyt::Pane::CalculateMtx", "nw4r.lyt@2008-03-08") == nullptr);
    static constexpr LibVersion kOs{"rvl.os@test", "rvl.os", "test"};
    const LibVersion* builds[] = {&kOs};
    set_detected(builds);
    assert(detected("rvl.os") == &kOs && detected("nw4r.lyt") == nullptr);
    std::puts("core ok");
}
