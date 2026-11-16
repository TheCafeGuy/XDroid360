/**
 * XDroid360: Ace Combat 6 (4E4D07D1) title-specific fixes.
 */

#ifndef XDROID_AC6_FIXES_H_
#define XDROID_AC6_FIXES_H_

#include <cstdint>

namespace xe {
class Emulator;
}

namespace xdroid {

// Registers the AC6 guest hooks when that title launches. Call once after the
// emulator is set up, before a title is launched.
void InstallAc6Fixes(xe::Emulator* emulator);

// True while the render thread's HUD pick waits for the main loop (HUD-1).
bool Ac6HudJoinWaiting();

}  // namespace xdroid

#endif  // XDROID_AC6_FIXES_H_
