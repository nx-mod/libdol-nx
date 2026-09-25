// The External Interface: the bus the console hangs its RTC, its SRAM and its
// memory cards off, and the transport a game uses to reach them.
//
// These sat with the OS natives for as long as there was nothing on the bus to
// answer. What a device says lives with that device - SRAM is libwii-nx's
// system/sram.cpp, beside SYSCONF - and what is here is only the bus: who is
// selected, what was asked of them, and moving the bytes.

#include "hle_stubs.h"
#include "abi_bridge.h"
#include "memory.h"
#include "runtime_log.h"

#include "sram.h"

#include <cstdint>
#include <cstring>

// EXI: early hardware init touches Hollywood registers (0xCD00xxxx). Provide a no-op stub.
extern "C" uint32_t EXIInit_80168fa0()
{
    RT_LOG(RT_TAG_OS) << "EXIInit_80168fa0 called: skipping MMIO register setup" << std::endl;
    return 0;
}


// ----------------------------------------------------------------------------
// The RTC/SRAM device on EXI channel 0, device 1.
//
// SRAM is the 64 bytes the console keeps its own settings in - language, video
// mode, screen offset, sound mode - and every game reads it during OSInit. The
// transfer is a command word written with EXIImm, then the payload moved with
// EXIDma, and a stub that reports success without moving any bytes leaves the
// guest reading whatever was in its buffer. Mega Man 9 then re-reads forever,
// which is a black screen and a busy loop through DCInvalidateRange.
//
// Serving the bytes here, rather than replacing each game's __OSInitSram, keeps
// this free of any game's addresses: the guest's own code does the read and
// stores the result wherever that game happens to keep it.
// ----------------------------------------------------------------------------

namespace {

constexpr uint32_t kExiRtcChannel = 0;
constexpr uint32_t kExiRtcDevice = 1;
// Command words the device answers, as the SDK writes them: the offset within
// the device shifted into place, with bit 31 clear for a read.
constexpr uint32_t kExiCmdRtcCounter = 0x20000000u;
constexpr uint32_t kExiCmdSram = 0x20000100u;

struct ExiDeviceSelection {
    bool selected = false;
    uint32_t channel = 0;
    uint32_t device = 0;
    uint32_t command = 0;
    bool commandKnown = false;
};

ExiDeviceSelection g_exiSelection;

// Copies what the selected device would return for `command`, or reports that
// this device has nothing to say - in which case the caller keeps its old
// behaviour of reporting success without moving bytes.
bool ExiReadSelectedDevice(uint32_t guestBuffer, uint32_t length)
{
    if (!g_exiSelection.selected || !g_exiSelection.commandKnown) return false;
    if (g_exiSelection.channel != kExiRtcChannel || g_exiSelection.device != kExiRtcDevice) return false;

    const uint32_t command = g_exiSelection.command;
    const uint8_t* source = nullptr;
    size_t available = 0;
    uint8_t rtc[4] = {0, 0, 0, 0};
    if (command == kExiCmdSram) {
        source = wiinx::system::sram::Image();
        available = wiinx::system::sram::kSize;
    } else if (command == kExiCmdRtcCounter) {
        const uint32_t counter = wiinx::system::sram::RtcCounter();
        rtc[0] = static_cast<uint8_t>(counter >> 24);
        rtc[1] = static_cast<uint8_t>(counter >> 16);
        rtc[2] = static_cast<uint8_t>(counter >> 8);
        rtc[3] = static_cast<uint8_t>(counter);
        source = rtc;
        available = sizeof(rtc);
    } else {
        return false;
    }

    try {
        for (uint32_t i = 0; i < length; ++i) {
            ::Memory::Write8(guestBuffer + i, i < available ? source[i] : 0u);
        }
    } catch (const ::Memory::AccessViolation&) {
        RT_LOG(RT_TAG_OS) << "EXI: could not write " << length << " byte(s) to guest buffer 0x"
                          << std::hex << guestBuffer << std::dec << std::endl;
        return false;
    }
    static int logged = 0;
    if (logged++ < 4) {
        RT_LOG(RT_TAG_OS) << "EXI: served " << length << " byte(s) of "
                          << (command == kExiCmdSram ? "SRAM" : "RTC")
                          << " to 0x" << std::hex << guestBuffer << std::dec << std::endl;
    }
    return true;
}

}  // namespace

// ----------------------------------------------------------------------------
// EXISelect / EXIDeselect - HLE Stubs (0x801689d0 / 0x80168b00)
// Real implementation touches MMIO at 0xCD0068xx; stub returns 1 (success).
// ----------------------------------------------------------------------------

extern "C" uint32_t EXISelect_801689d0(uint32_t channel, uint32_t device, uint32_t frequency)
{
    // Log occasionally to avoid spam if polled frequently
    g_exiSelection.selected = true;
    g_exiSelection.channel = channel;
    g_exiSelection.device = device;
    g_exiSelection.commandKnown = false;

    static int log_counter = 0;
    if (log_counter++ < 10) {
        RT_LOG(RT_TAG_OS) << "EXISelect_801689d0 called: channel=" << channel
                  << " device=" << device << " freq=" << frequency 
                  << " (stubbed success)" << std::endl;
    }
    return 1; // Return 1 (true) to indicate successful selection
}

// Body for the EXI stubs that take only a channel and report success: the real
// ops drive Hollywood MMIO we do not emulate, and the callers loop until they
// see success. Each keeps its own log budget and wording; the registrations
// stay spelled out below because the translator scans them by text.
#define EXI_CHANNEL_STUB(name, logLimit, channelLabel)                    \
    extern "C" uint32_t name(uint32_t channel)                            \
    {                                                                     \
        static int log_count = 0;                                         \
        if (log_count++ < (logLimit)) {                                   \
            RT_LOG(RT_TAG_OS) << #name " called: " channelLabel           \
                      << channel << " (stubbed success)" << std::endl;    \
        }                                                                 \
        return 1;                                                         \
    }

EXI_CHANNEL_STUB(EXIDeselect_80168b00, 10, "channel=")

// Register the functions
PPC_NATIVE_OVERRIDE(801689D0, EXISelect_801689d0, uint32_t, (uint32_t channel, uint32_t device, uint32_t frequency), (channel, device, frequency));
PPC_NATIVE_OVERRIDE(80168B00, EXIDeselect_80168b00, uint32_t, (uint32_t channel), (channel));

// ----------------------------------------------------------------------------
// SetExiInterruptMask (0x80167e78): stubbed no-op, our fake EXI devices need no interrupt masking.
// ----------------------------------------------------------------------------
extern "C" void SetExiInterruptMask_80167e78(uint32_t channel, uint32_t exi_struct_ptr)
{
    // channel: r3 (0, 1, 2)
    // exi_struct_ptr: r4
    // This function is void and typically just modifies internal OS masks.
    // We treat it as a successful no-op.
}

// Register the function
PPC_NATIVE_OVERRIDE_VOID(80167E78, SetExiInterruptMask_80167e78, (uint32_t channel, uint32_t exi_struct_ptr), (channel, exi_struct_ptr));

// ----------------------------------------------------------------------------
// EXI Transaction Stubs (Imm, Dma, Sync, Unlock)
// ----------------------------------------------------------------------------

// RVL__EXIImm / EXIImm
// Address: 0x80167f68
// Behavior: Performs an Immediate transfer (1-4 bytes) over EXI.
//           Stub: Return 1 (success). If it's a read, we clear the buffer.
extern "C" uint32_t EXIImm_80167f68(uint32_t channel, uint32_t buffer, uint32_t length, uint32_t type, uint32_t callback)
{
    // type: 0=Read, 1=Write, 2=RW
    // A four byte write is how the SDK names what it wants from the selected
    // device; everything after it reads from that offset.
    if (type == 1 && length == 4) {
        try {
            g_exiSelection.command = ::Memory::Read32(buffer);
            g_exiSelection.commandKnown = true;
        } catch (const ::Memory::AccessViolation&) {
            g_exiSelection.commandKnown = false;
        }
    }
    if (type == 0 || type == 2) {
        // Answer from the device when we have one, and otherwise keep the old
        // behaviour: zero the buffer rather than leave the guest reading itself.
        if (!ExiReadSelectedDevice(buffer, length)) {
            try {
                for (uint32_t i = 0; i < length; ++i) {
                    ::Memory::Write8(buffer + i, 0);
                }
            } catch (...) {
                RT_LOG(RT_TAG_OS) << "EXIImm: Failed to write to guest buffer 0x" << std::hex << buffer << std::dec << std::endl;
            }
        }
    }
    
    // Log only occasionally
    static int log_count = 0;
    if (log_count++ < 5) {
        RT_LOG(RT_TAG_OS) << "EXIImm_80167f68 called: chan=" << channel << " len=" << length << " type=" << type << " (stubbed success)" << std::endl;
    }
    return 1; // Success
}

// RVL__EXIDma / EXIDma
// Address: 0x80168288
// Behavior: Performs a DMA transfer over EXI.
//           Stub: Return 1 (success).
extern "C" uint32_t EXIDma_80168288(uint32_t channel, uint32_t buffer, uint32_t length, uint32_t type, uint32_t callback)
{
    // type: 0=Read, 1=Write. A read is the payload half of the transfer the
    // command word opened, and is where SRAM actually arrives.
    if (type == 0) {
        ExiReadSelectedDevice(buffer, length);
    }

    static int log_count = 0;
    if (log_count++ < 5) {
        RT_LOG(RT_TAG_OS) << "EXIDma_80168288 called: chan=" << channel << " len=" << length << " (stubbed success)" << std::endl;
    }
    return 1; // Success
}

// RVL__EXISync / EXISync
// Address: 0x80168380
// Behavior: Waits for the current EXI transfer to complete.
//           Stub: Return 1 (success) immediately.
EXI_CHANNEL_STUB(EXISync_80168380, 5, "chan=")

// RVL__EXIUnlock / EXIUnlock
// Address: 0x80169260
// Behavior: Unlocks the EXI channel and triggers any pending callbacks.
//           Stub: Return 1 (success) to bypass internal callback logic that causes the 0x0 crash.
EXI_CHANNEL_STUB(EXIUnlock_80169260, 5, "chan=")

PPC_NATIVE_OVERRIDE(80168FA0, EXIInit_80168fa0, uint32_t, (), ());

PPC_NATIVE_OVERRIDE(80167F68, EXIImm_80167f68, uint32_t, (uint32_t channel, uint32_t buffer, uint32_t length, uint32_t type, uint32_t callback), (channel, buffer, length, type, callback));

PPC_NATIVE_OVERRIDE(80168288, EXIDma_80168288, uint32_t, (uint32_t channel, uint32_t buffer, uint32_t length, uint32_t type, uint32_t callback), (channel, buffer, length, type, callback));

PPC_NATIVE_OVERRIDE(80168380, EXISync_80168380, uint32_t, (uint32_t channel), (channel));

PPC_NATIVE_OVERRIDE(80169260, EXIUnlock_80169260, uint32_t, (uint32_t channel), (channel));
