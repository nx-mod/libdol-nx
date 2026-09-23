// The C library's natives, against a guest memory that is a buffer.
//
// Each one is called the way the game calls it - arguments in r3 and up, the
// answer in r3 - and checked against what the standard says it does, including
// the parts of the standard that are surprising.
#include "wiinx/accel/sdk.hpp"
#include "wiinx/core/host.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
int gFailures = 0;

void Check(const char* what, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    gFailures += ok ? 0 : 1;
}

// A CPU that is nothing but registers, which is all these natives read.
struct Registers {
    wiinx::u32 gpr[32] = {};
};

std::vector<wiinx::u8> gMemory;
constexpr wiinx::GuestAddr kBase = 0x80000000;

wiinx::u8* Pointer(wiinx::GuestAddr address, wiinx::u32 size) {
    if (address < kBase || address - kBase + size > gMemory.size()) {
        return nullptr;
    }
    return gMemory.data() + (address - kBase);
}

wiinx::u32 Gpr(wiinx::Cpu* cpu, int index) {
    return reinterpret_cast<Registers*>(cpu)->gpr[index];
}

void SetGpr(wiinx::Cpu* cpu, int index, wiinx::u32 value) {
    reinterpret_cast<Registers*>(cpu)->gpr[index] = value;
}

// Runs the native registered under `name`, with these arguments.
wiinx::u32 Call(const char* name, std::initializer_list<wiinx::u32> arguments) {
    const wiinx::Native* native = wiinx::find_native(name, "msl@any");
    if (native == nullptr) {
        std::printf("  [FAIL] %s is not registered\n", name);
        gFailures++;
        return 0;
    }
    Registers registers;
    int index = 3;
    for (const wiinx::u32 argument : arguments) {
        registers.gpr[index++] = argument;
    }
    native->entry(reinterpret_cast<wiinx::Cpu*>(&registers));
    return registers.gpr[3];
}

// Puts a string in guest memory and gives back its address.
wiinx::GuestAddr Put(wiinx::u32 offset, const std::string& text) {
    std::memcpy(gMemory.data() + offset, text.data(), text.size() + 1);
    return kBase + offset;
}

std::string At(wiinx::GuestAddr address) {
    return std::string(reinterpret_cast<const char*>(gMemory.data() + (address - kBase)));
}
}  // namespace

int main() {
    using namespace wiinx;
    std::printf("wiinx::sdk C library check\n\n");

    gMemory.assign(0x1000, 0);
    Host host;
    host.pointer = &Pointer;
    host.gpr = &Gpr;
    host.set_gpr = &SetGpr;
    set_host(host);
    add_natives(sdk::natives());

    const GuestAddr hello = Put(0x000, "hello");
    const GuestAddr world = Put(0x100, "world");
    const GuestAddr room = kBase + 0x200;

    Check("strlen", Call("strlen", {hello}) == 5);
    Check("strcmp, the same", Call("strcmp", {hello, hello}) == 0);
    Check("strcmp, different", static_cast<std::int32_t>(Call("strcmp", {hello, world})) < 0);
    Check("strncmp stops where it is told",
          Call("strncmp", {Put(0x300, "abcXX"), Put(0x400, "abcYY"), 3}) == 0);
    Check("strncmp past that does not",
          static_cast<std::int32_t>(Call("strncmp", {kBase + 0x300, kBase + 0x400, 5})) < 0);

    Check("strcpy returns its destination", Call("strcpy", {room, hello}) == room);
    Check("  and copied the string", At(room) == "hello");

    Check("strcat appends", Call("strcat", {room, world}) == room);
    Check("  giving both", At(room) == "helloworld");

    {   // strncpy's own oddity: it pads to the length, and does not terminate
        // when the source is longer.
        std::memset(gMemory.data() + 0x500, 0xAA, 16);
        Call("strncpy", {kBase + 0x500, hello, 8});
        Check("strncpy pads with zeros",
              std::memcmp(gMemory.data() + 0x500, "hello\0\0\0", 8) == 0);

        std::memset(gMemory.data() + 0x600, 0xAA, 16);
        Call("strncpy", {kBase + 0x600, hello, 3});
        Check("strncpy leaves a long source unterminated",
              std::memcmp(gMemory.data() + 0x600, "hel", 3) == 0 && gMemory[0x603] == 0xAA);
    }

    Check("memcmp, the same", Call("memcmp", {hello, hello, 5}) == 0);
    Check("memcmp, different",
          static_cast<std::int32_t>(Call("memcmp", {hello, world, 5})) < 0);
    Check("memcmp of nothing is equal", Call("memcmp", {hello, world, 0}) == 0);

    Check("strchr finds a character", Call("strchr", {hello, 'l'}) == hello + 2);
    Check("strchr finds the terminator", Call("strchr", {hello, 0}) == hello + 5);
    Check("strchr answers nothing when there is none", Call("strchr", {hello, 'z'}) == 0);
    Check("strrchr finds the last", Call("strrchr", {hello, 'l'}) == hello + 3);

    Check("memcpy returns its destination",
          Call("memcpy", {kBase + 0x700, hello, 6}) == kBase + 0x700);
    Check("  and copied", At(kBase + 0x700) == "hello");
    Check("memset returns its destination", Call("memset", {kBase + 0x800, 0x41, 4}) == kBase + 0x800);
    Check("  and filled", std::memcmp(gMemory.data() + 0x800, "AAAA", 4) == 0);

    {   // Memory the guest cannot reach: answered, not faulted on.
        const GuestAddr nowhere = kBase + 0x100000;
        Check("a string outside guest memory is length nothing",
              Call("strlen", {nowhere}) == 0);
        Check("strcpy from nowhere still returns its destination",
              Call("strcpy", {room, nowhere}) == room);
        Check("strchr in nowhere finds nothing", Call("strchr", {nowhere, 'a'}) == 0);
    }

    std::printf("\n%s\n", gFailures == 0 ? "all checks passed" : "checks failed");
    return gFailures == 0 ? 0 : 1;
}
