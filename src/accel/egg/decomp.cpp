// EGG::Decomp::decodeSZS, natively.
//
// Every EAD game keeps its archives compressed and expands them through this
// one routine, so it runs for every course, menu and model a game loads. The
// expansion itself is in format/archive; this resolves the guest's buffers and
// runs it against them.
#include "wiinx/accel/egg.hpp"

#include "wiinx/core/guest.hpp"
#include "wiinx/core/host.hpp"
#include "wiinx/format/archive/yaz0.hpp"

namespace wiinx::egg::decomp {
namespace {

void report(const char* line) noexcept {
    if (const auto log = host().log) {
        log(line);
    }
}

// A compressed archive does not say how long it is, so find how much guest
// memory is readable from its start. The bound is generous: no archive a game
// loads in one call approaches it.
u32 readable_from(GuestAddr address) noexcept {
    const Host& h = host();
    if (h.valid == nullptr) {
        return 0;
    }
    u32 good = 0;
    u32 probe = 64u * 1024u;
    constexpr u32 kLimit = 32u * 1024u * 1024u;
    while (probe <= kLimit && h.valid(address, probe)) {
        good = probe;
        probe *= 2;
    }
    return good;
}

}  // namespace

u32 decode_szs(GuestAddr src, GuestAddr dst) noexcept {
    const Host& h = host();

    // The header says how large the result is, and everything below is bounded
    // by it.
    const u8* const header = h.pointer != nullptr ? h.pointer(src, archive::kYaz0HeaderSize) : nullptr;
    if (header == nullptr) {
        report("egg: decodeSZS: unreadable source");
        return 0;
    }
    const u32 expanded = archive::Yaz0ExpandedSize(header, archive::kYaz0HeaderSize);
    if (expanded == 0) {
        report("egg: decodeSZS: not a Yaz0 stream");
        return 0;
    }

    u8* const out = at(dst, expanded);
    if (out == nullptr) {
        report("egg: decodeSZS: destination does not fit");
        return 0;
    }

    const u32 readable = readable_from(src);
    const u8* const in = readable != 0 ? h.pointer(src, readable) : nullptr;
    if (in == nullptr) {
        report("egg: decodeSZS: source does not fit");
        return 0;
    }

    const u32 written = archive::Yaz0Decode(in, readable, out, expanded);
    if (written == 0) {
        report("egg: decodeSZS: malformed stream");
        return 0;
    }

    if (const auto notify = h.notify_write) {
        notify(dst, written);
    }
    return written;
}

}  // namespace wiinx::egg::decomp
