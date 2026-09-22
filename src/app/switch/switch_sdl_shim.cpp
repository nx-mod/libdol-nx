// Switch has no SDL3 backend (SDL3 has no Horizon thread/video/joystick
// modules), so the runtime and aurora link against SDL3 *headers only*. This
// file defines the handful of SDL entry points that code still calls:
//   - SDL_IOStream: real, backed by stdio, because callers genuinely use it
//     to read and write files.
//   - joystick / gamepad / keyboard / mouse: report "nothing attached".
//     Actual Switch controller input goes through libnx PadState directly
//     (see wii_remote_input.cpp); nothing here needs to be a real device.
#if defined(__SWITCH__)

#include "switch_layout.h"
#include <SDL3/SDL.h>

#include <switch.h>

#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

struct SDL_IOStream {
    FILE* file;
    SDL_IOStatus status;
};

namespace {
// The game's folder in the wii-nx layout (switch_layout.h).
constexpr const char* kAppDataDir = WIINX_GAME_PATH("");
Uint32 g_nextUserEvent = 0x8000;
bool g_keyboardState[SDL_SCANCODE_COUNT] = {};
} // namespace

SDL_IOStream* SDL_IOFromFile(const char* file, const char* mode) {
    if (file == nullptr || mode == nullptr) {
        return nullptr;
    }
    FILE* f = std::fopen(file, mode);
    if (f == nullptr) {
        return nullptr;
    }
    return new SDL_IOStream{f, SDL_IO_STATUS_READY};
}

size_t SDL_ReadIO(SDL_IOStream* context, void* ptr, size_t size) {
    if (context == nullptr || ptr == nullptr || size == 0) {
        return 0;
    }
    const size_t n = std::fread(ptr, 1, size, context->file);
    if (n < size) {
        context->status = std::feof(context->file) ? SDL_IO_STATUS_EOF : SDL_IO_STATUS_ERROR;
    } else {
        context->status = SDL_IO_STATUS_READY;
    }
    return n;
}

size_t SDL_WriteIO(SDL_IOStream* context, const void* ptr, size_t size) {
    if (context == nullptr || ptr == nullptr || size == 0) {
        return 0;
    }
    const size_t n = std::fwrite(ptr, 1, size, context->file);
    context->status = n < size ? SDL_IO_STATUS_ERROR : SDL_IO_STATUS_READY;
    return n;
}

Sint64 SDL_SeekIO(SDL_IOStream* context, Sint64 offset, SDL_IOWhence whence) {
    if (context == nullptr) {
        return -1;
    }
    int origin = SEEK_SET;
    if (whence == SDL_IO_SEEK_CUR) {
        origin = SEEK_CUR;
    } else if (whence == SDL_IO_SEEK_END) {
        origin = SEEK_END;
    }
    if (std::fseek(context->file, static_cast<long>(offset), origin) != 0) {
        context->status = SDL_IO_STATUS_ERROR;
        return -1;
    }
    context->status = SDL_IO_STATUS_READY;
    return static_cast<Sint64>(std::ftell(context->file));
}

Sint64 SDL_TellIO(SDL_IOStream* context) {
    return context == nullptr ? -1 : static_cast<Sint64>(std::ftell(context->file));
}

bool SDL_CloseIO(SDL_IOStream* context) {
    if (context == nullptr) {
        return true;
    }
    const bool ok = std::fclose(context->file) == 0;
    delete context;
    return ok;
}

Sint64 SDL_GetIOSize(SDL_IOStream* context) {
    if (context == nullptr) {
        return -1;
    }
    const long pos = std::ftell(context->file);
    if (pos < 0 || std::fseek(context->file, 0, SEEK_END) != 0) {
        return -1;
    }
    const long end = std::ftell(context->file);
    std::fseek(context->file, pos, SEEK_SET);
    return static_cast<Sint64>(end);
}

SDL_IOStatus SDL_GetIOStatus(SDL_IOStream* context) {
    return context == nullptr ? SDL_IO_STATUS_ERROR : context->status;
}

bool SDL_ReadU32LE(SDL_IOStream* src, Uint32* value) {
    Uint8 b[4];
    if (SDL_ReadIO(src, b, sizeof(b)) != sizeof(b)) {
        return false;
    }
    if (value != nullptr) {
        *value = static_cast<Uint32>(b[0]) | (static_cast<Uint32>(b[1]) << 8) | (static_cast<Uint32>(b[2]) << 16) |
                 (static_cast<Uint32>(b[3]) << 24);
    }
    return true;
}

bool SDL_WriteU32LE(SDL_IOStream* dst, Uint32 value) {
    const Uint8 b[4] = {static_cast<Uint8>(value), static_cast<Uint8>(value >> 8), static_cast<Uint8>(value >> 16),
                        static_cast<Uint8>(value >> 24)};
    return SDL_WriteIO(dst, b, sizeof(b)) == sizeof(b);
}

bool SDL_WriteS32LE(SDL_IOStream* dst, Sint32 value) { return SDL_WriteU32LE(dst, static_cast<Uint32>(value)); }

bool SDL_WriteU8(SDL_IOStream* dst, Uint8 value) { return SDL_WriteIO(dst, &value, 1) == 1; }

const char* SDL_GetError(void) { return ""; }

char* SDL_GetPrefPath(const char*, const char*) { return strdup(kAppDataDir); }

const char* SDL_GetBasePath(void) { return kAppDataDir; }

void SDL_free(void* mem) { std::free(mem); }

char* SDL_strstr(const char* haystack, const char* needle) { return const_cast<char*>(std::strstr(haystack, needle)); }

void SDL_Delay(Uint32 ms) { svcSleepThread(static_cast<s64>(ms) * 1000000); }

Uint32 SDL_RegisterEvents(int numevents) {
    if (numevents <= 0) {
        return 0;
    }
    const Uint32 first = g_nextUserEvent;
    g_nextUserEvent += static_cast<Uint32>(numevents);
    return first;
}

bool SDL_SetCurrentThreadPriority(SDL_ThreadPriority) { return true; }

bool SDL_ShowCursor(void) { return true; }

bool SDL_HideCursor(void) { return true; }

SDL_MouseButtonFlags SDL_GetMouseState(float* x, float* y) {
    if (x != nullptr) {
        *x = 0.f;
    }
    if (y != nullptr) {
        *y = 0.f;
    }
    return 0;
}

SDL_Window* SDL_GetKeyboardFocus(void) { return nullptr; }

const bool* SDL_GetKeyboardState(int* numkeys) {
    if (numkeys != nullptr) {
        *numkeys = SDL_SCANCODE_COUNT;
    }
    return g_keyboardState;
}

const char* SDL_GetScancodeName(SDL_Scancode) { return ""; }

void SDL_GUIDToString(SDL_GUID, char* pszGUID, int cbGUID) {
    if (pszGUID == nullptr || cbGUID <= 0) {
        return;
    }
    std::snprintf(pszGUID, static_cast<size_t>(cbGUID), "00000000000000000000000000000000");
}

SDL_JoystickID* SDL_GetJoysticks(int* count) {
    if (count != nullptr) {
        *count = 0;
    }
    return nullptr;
}

const char* SDL_GetJoystickNameForID(SDL_JoystickID) { return nullptr; }

SDL_GUID SDL_GetJoystickGUIDForID(SDL_JoystickID) { return SDL_GUID{}; }

Sint16 SDL_GetJoystickAxis(SDL_Joystick*, int) { return 0; }

bool SDL_GetJoystickButton(SDL_Joystick*, int) { return false; }

int SDL_GetNumJoystickAxes(SDL_Joystick*) { return 0; }

int SDL_GetNumJoystickButtons(SDL_Joystick*) { return 0; }

SDL_Joystick* SDL_OpenJoystick(SDL_JoystickID) { return nullptr; }

void SDL_CloseJoystick(SDL_Joystick*) {}

bool SDL_IsGamepad(SDL_JoystickID) { return false; }

SDL_Gamepad* SDL_GetGamepadFromID(SDL_JoystickID) { return nullptr; }

SDL_Gamepad* SDL_GetGamepadFromPlayerIndex(int) { return nullptr; }

SDL_JoystickID SDL_GetGamepadID(SDL_Gamepad*) { return 0; }

const char* SDL_GetGamepadName(SDL_Gamepad*) { return nullptr; }

SDL_GamepadType SDL_GetGamepadType(SDL_Gamepad*) { return SDL_GAMEPAD_TYPE_UNKNOWN; }

SDL_Joystick* SDL_GetGamepadJoystick(SDL_Gamepad*) { return nullptr; }

char* SDL_GetGamepadMappingForID(SDL_JoystickID) { return nullptr; }

int SDL_GetGamepadPlayerIndex(SDL_Gamepad*) { return -1; }

bool SDL_SetGamepadPlayerIndex(SDL_Gamepad*, int) { return false; }

bool SDL_SetGamepadLED(SDL_Gamepad*, Uint8, Uint8, Uint8) { return false; }

bool SDL_GetGamepadButton(SDL_Gamepad*, SDL_GamepadButton) { return false; }

Sint16 SDL_GetGamepadAxis(SDL_Gamepad*, SDL_GamepadAxis) { return 0; }

int SDL_AddGamepadMapping(const char*) { return 0; }

// Horizon has no process spawning. Dear ImGui's default "open a URL" hook
// references these; report "unsupported" so it fails cleanly if ever invoked.
extern "C" {
pid_t waitpid(pid_t, int*, int) {
    errno = ENOSYS;
    return -1;
}

int execvp(const char*, char* const[]) {
    errno = ENOSYS;
    return -1;
}
}

#endif // __SWITCH__
