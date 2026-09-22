#pragma once

// Derives a stable synthetic Wii console identity from the Switch account
// that launched this title, so the same profile always regenerates the same
// NAND setting.txt (console_identity.h) and NWC24 user id
// (network_core.cpp's RuntimeGeneratedUserId) even after the local managed
// NAND directory is wiped or copied to a new SD card. Wii games (and
// Wiimmfi) identify the console purely through that NAND-derived info - not
// through anything Switch-specific - so this only has to produce a stable
// seed once per account, not implement any part of the Wii identity format
// itself.
//
// Nintendo Switch Online is never contacted: accountGetPreselectedUser /
// accountGetLastOpenedUser only read the locally cached profile id already
// selected for this launch.

#if defined(__SWITCH__)

#include <switch.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

namespace RuntimeSwitchAccount {

// The 128-bit id of the profile this launch is running under: whatever the
// title-launch applet preselected, or (the common homebrew/hbmenu case,
// where nothing preselects a user) the last profile opened on the console.
// Cached for the process lifetime; nullopt only if account service startup
// fails outright or the console has no usable profile at all.
inline std::optional<std::array<uint64_t, 2>> CurrentUid() {
    static const std::optional<std::array<uint64_t, 2>> cached = [] {
        std::optional<std::array<uint64_t, 2>> result;
        if (R_SUCCEEDED(accountInitialize(AccountServiceType_Application))) {
            AccountUid uid{};
            Result rc = accountGetPreselectedUser(&uid);
            if (R_FAILED(rc) || !accountUidIsValid(&uid)) {
                rc = accountGetLastOpenedUser(&uid);
            }
            if (R_SUCCEEDED(rc) && accountUidIsValid(&uid)) {
                result = std::array<uint64_t, 2>{uid.uid[0], uid.uid[1]};
            }
            // Left initialized: NWC24/console-identity lookups may run this
            // path again later in the process, and libnx tears the service
            // down at exit regardless.
        }
        return result;
    }();
    return cached;
}

// FNV-1a-64 over the two UID words. Not cryptographic - this only needs to
// turn "same account" into "same seed", not resist a hostile input.
inline uint64_t HashUid(const std::array<uint64_t, 2>& uid) {
    uint64_t hash = 1469598103934665603ull;  // FNV offset basis
    for (const uint64_t word : uid) {
        for (int byte = 0; byte < 8; ++byte) {
            hash ^= (word >> (byte * 8)) & 0xFFu;
            hash *= 1099511628211ull;  // FNV prime
        }
    }
    return hash;
}

// A 9-digit numeric NAND serial (RuntimeNandSettings' SERNO format) derived
// from the active account. nullopt when no account is available, in which
// case the caller keeps the existing time-seeded serial.
inline std::optional<std::string> DerivedNandSerial() {
    const auto uid = CurrentUid();
    if (!uid) return std::nullopt;
    // +1 keeps this out of "000000000", which RuntimeNandSettings::HasIdentity
    // rejects as an invalid serial.
    const uint32_t serial = static_cast<uint32_t>(HashUid(*uid) % 999999999ull) + 1;
    char buffer[10];
    std::snprintf(buffer, sizeof(buffer), "%09u", serial);
    return std::string(buffer);
}

// A stable NWC24 user id, independent of the serial above (this hashes the
// UID words in the opposite order) so the two draw from different parts of
// the same account seed instead of one being a visible function of the
// other.
inline std::optional<uint64_t> DerivedUserId() {
    const auto uid = CurrentUid();
    if (!uid) return std::nullopt;
    return HashUid({(*uid)[1], (*uid)[0]});
}

}  // namespace RuntimeSwitchAccount

#endif  // __SWITCH__
