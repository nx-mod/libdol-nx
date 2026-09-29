#pragma once

// Press-to-bind setup for joysticks SDL either doesn't recognize as gamepads or
// recognizes with a mapping that lacks the analog stick (e.g. raphnet adapters).
// The wizard produces a standard SDL gamepad mapping, applies it live, and
// persists it to gamecontrollerdb.txt in the user data directory.
// Switch has no SDL and its pads need no mapping, so there it does nothing.
#if !defined(__SWITCH__)
#include <SDL3/SDL_events.h>
#endif

namespace controller_mapping_wizard {

#if defined(__SWITCH__)
inline void LoadPersistedMappings() {}
inline void DrawSetupList() {}
inline void Draw() {}
inline bool IsActive() { return false; }
#else
void LoadPersistedMappings();
void HandleSdlEvent(const SDL_Event& event);

// Lists devices that need setup inside the controller settings menu.
void DrawSetupList();
// Draws the wizard window when active; call once per overlay frame.
void Draw();

bool IsActive();
#endif

} // namespace controller_mapping_wizard
