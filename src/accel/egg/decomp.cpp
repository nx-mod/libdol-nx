// Yaz0 decompression, as EGG::Decomp does it.
//
// Every EAD game keeps its archives compressed this way and expands them
// through this one routine, so it runs for every course, menu and model a game
// loads. Native, it is a byte loop over host memory instead of a guest one.
//
// Reference: the format as described by EGG::Decomp in Kinoko
// (https://github.com/vabold/Kinoko, MIT), and the overrun reported in
// szsHaxx (https://github.com/vabold/szsHaxx).
#include "wiinx/accel/egg.hpp"

#include "wiinx/core/host.hpp"

namespace wiinx::egg::decomp {
namespace {

void report(const char* line) noexcept {
    if (const auto log = host().log) {
        log(line);
    }
}

}  // namespace

u32 decode_szs(GuestAddr src, GuestAddr dst) noexcept {
    // The header is "Yaz0", then the expanded size. Read it before anything
    // else: everything below is bounded by it.
    const u8* header = host().pointer != nullptr ? host().pointer(src, 16) : nullptr;
    if (header == nullptr) {
        report("egg: decodeSZS: unreadable source");
        return 0;
    }
    const u32 expanded = from_be<u32>(header + 4);

    u8* out = host().pointer != nullptr ? host().pointer(dst, expanded) : nullptr;
    if (out == nullptr) {
        report("egg: decodeSZS: destination does not fit");
        return 0;
    }

    // The stream is a group of eight codes at a time, each one either a byte to
    // copy or a back-reference into what has been written already.
    u32 read = 16;
    u32 written = 0;
    u32 mask = 0;
    u32 flags = 0;

    // The compressed stream is read in order and its length is not in the
    // header, so it is resolved a window at a time: one address lookup per
    // 64 KiB rather than one per byte, and a short window at the end of a
    // region where a whole one would not be readable.
    constexpr u32 kWindow = 0x10000;
    const u8* window = nullptr;
    u32 window_start = 0;
    u32 window_end = 0;

    const auto source_byte = [&](u32 offset) -> int {
        const u32 address = src + offset;
        if (address < window_start || address >= window_end) {
            u32 size = kWindow;
            const u8* mapped = nullptr;
            while (size != 0 && (mapped = host().pointer(address, size)) == nullptr) {
                size /= 2;
            }
            if (mapped == nullptr) {
                return -1;
            }
            window = mapped;
            window_start = address;
            window_end = address + size;
        }
        return window[address - window_start];
    };

    while (written < expanded) {
        if (mask == 0) {
            const int next = source_byte(read++);
            if (next < 0) {
                report("egg: decodeSZS: stream ends inside a group");
                return 0;
            }
            flags = static_cast<u32>(next);
            mask = 0x80;
        }

        if ((flags & mask) != 0) {
            const int literal = source_byte(read++);
            if (literal < 0) {
                report("egg: decodeSZS: stream ends inside a literal");
                return 0;
            }
            out[written++] = static_cast<u8>(literal);
        } else {
            const int high = source_byte(read);
            const int low = source_byte(read + 1);
            if (high < 0 || low < 0) {
                report("egg: decodeSZS: stream ends inside a reference");
                return 0;
            }
            read += 2;

            const u32 reference = (static_cast<u32>(high) << 8) | static_cast<u32>(low);
            const u32 distance = (reference & 0xFFF) + 1;
            if (distance > written) {
                // Reaching back before the output would read whatever the guest
                // left in front of the buffer.
                report("egg: decodeSZS: back-reference before the output");
                return 0;
            }

            u32 count = reference >> 12;
            if (count == 0) {
                const int extra = source_byte(read++);
                if (extra < 0) {
                    report("egg: decodeSZS: stream ends inside a long run");
                    return 0;
                }
                count = static_cast<u32>(extra) + 18;
            } else {
                count += 2;
            }
            if (written + count > expanded) {
                report("egg: decodeSZS: run overruns the expanded size");
                return 0;
            }

            // The runs overlap by design - a distance of one repeats a byte -
            // so this copies forward, one byte at a time.
            u32 from = written - distance;
            for (u32 index = 0; index < count; index++) {
                out[written++] = out[from++];
            }
        }

        mask >>= 1;
    }

    if (const auto notify = host().notify_write) {
        notify(dst, expanded);
    }
    return expanded;
}

}  // namespace wiinx::egg::decomp
