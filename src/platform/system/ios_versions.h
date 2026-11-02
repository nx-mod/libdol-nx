#pragma once

// What each IOS leaves in low memory for the SDK to read: its version and date (0x80003140/44), and the
// sizes of the IPC buffer and the block IOS keeps for itself at the top of MEM2. Every title asks for its
// own IOS in its TMD (recomp.yml `project.ios`), and the SDK checks it: a 2009 SDK refuses IOS36's.
//
// From Dolphin Emulator's Source/Core/Core/IOS/VersionInfo.cpp (ios_memory_values), which were read out of
// the IOS binaries themselves. Copyright Dolphin Emulator Project. SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdint>

namespace IosVersions {

struct Ios {
    uint16_t number;
    uint32_t version;       // 0x80003140
    uint32_t date;          // 0x80003144
    uint32_t ipcSize;       // the IPC buffer, just below the reserved block
    uint32_t reservedSize;  // what IOS keeps at the top of MEM2 (0: nothing)
};

inline constexpr Ios kTable[] = {
    {  4, 0x00040003u, 0x00081006u, 0x20000u, 0x00000u},
    {  9, 0x0009040Au, 0x00030110u, 0x20000u, 0x00000u},
    { 11, 0x000B000Au, 0x00102506u, 0x20000u, 0x00000u},
    { 12, 0x000C020Eu, 0x00030110u, 0x20000u, 0x00000u},
    { 13, 0x000D0408u, 0x00030110u, 0x20000u, 0x00000u},
    { 14, 0x000E0408u, 0x00030110u, 0x20000u, 0x00000u},
    { 15, 0x000F0408u, 0x00030110u, 0x20000u, 0x00000u},
    { 17, 0x00110408u, 0x00030110u, 0x20000u, 0x00000u},
    { 20, 0x0014000Cu, 0x00102506u, 0x20000u, 0x00000u},
    { 21, 0x0015040Fu, 0x00030110u, 0x20000u, 0x00000u},
    { 22, 0x0016050Eu, 0x00030110u, 0x20000u, 0x00000u},
    { 28, 0x001C070Fu, 0x00030110u, 0x20000u, 0x20000u},
    { 30, 0x001E0A10u, 0x00040308u, 0x20000u, 0x20000u},
    { 31, 0x001F0E18u, 0x00030110u, 0x20000u, 0x20000u},
    { 33, 0x00210E18u, 0x00030110u, 0x20000u, 0x20000u},
    { 34, 0x00220E18u, 0x00030110u, 0x20000u, 0x20000u},
    { 35, 0x00230E18u, 0x00030110u, 0x20000u, 0x20000u},
    { 36, 0x00240E18u, 0x00030110u, 0x20000u, 0x20000u},
    { 37, 0x0025161Fu, 0x00030110u, 0x20000u, 0x20000u},
    { 38, 0x0026101Cu, 0x00030110u, 0x20000u, 0x20000u},
    { 40, 0x00280911u, 0x00022308u, 0x20000u, 0x20000u},
    { 41, 0x00290E17u, 0x00030110u, 0x20000u, 0x20000u},
    { 43, 0x002B0E17u, 0x00030110u, 0x20000u, 0x20000u},
    { 45, 0x002D0E17u, 0x00030110u, 0x20000u, 0x20000u},
    { 46, 0x002E0E17u, 0x00030110u, 0x20000u, 0x20000u},
    { 48, 0x0030101Cu, 0x00030110u, 0x20000u, 0x20000u},
    { 50, 0x00321319u, 0x00101008u, 0x20000u, 0x20000u},
    { 51, 0x00331219u, 0x00071108u, 0x20000u, 0x20000u},
    { 52, 0x0034161Du, 0x00101008u, 0x20000u, 0x20000u},
    { 53, 0x0035161Fu, 0x00030110u, 0x20000u, 0x20000u},
    { 55, 0x0037161Fu, 0x00030110u, 0x20000u, 0x20000u},
    { 56, 0x0038161Eu, 0x00030110u, 0x20000u, 0x20000u},
    { 57, 0x0039171Fu, 0x00030110u, 0x20000u, 0x20000u},
    { 58, 0x003A1820u, 0x00030110u, 0x20000u, 0x20000u},
    { 59, 0x003B2421u, 0x00101811u, 0x20000u, 0x20000u},
    { 60, 0x003C181Eu, 0x00112408u, 0x20000u, 0x20000u},
    { 61, 0x003D161Eu, 0x00030110u, 0x20000u, 0x20000u},
    { 62, 0x003E191Eu, 0x00022712u, 0x20000u, 0x20000u},
    { 70, 0x00461A1Fu, 0x00060309u, 0x20000u, 0x20000u},
    { 80, 0x00501B20u, 0x00030310u, 0x20000u, 0x20000u},
    {257, 0x00000707u, 0x00082209u, 0x20000u, 0x00000u},
};

inline const Ios* Find(uint16_t number) {
    for (const Ios& ios : kTable) {
        if (ios.number == number) return &ios;
    }
    return nullptr;
}

}  // namespace IosVersions
