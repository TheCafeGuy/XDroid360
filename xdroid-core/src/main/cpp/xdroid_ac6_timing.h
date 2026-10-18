/**
 * XDroid360: Ace Combat 6 (4E4D07D1) frame-timing hooks (60 FPS probe and
 * slow-motion guard). See docs/design/ac6-60fps.md.
 */

#ifndef XDROID_AC6_TIMING_H_
#define XDROID_AC6_TIMING_H_

namespace xe {
class Emulator;
}

namespace xdroid {

// Installs the timing hooks when ac6_timing_hooks is set. Call from the AC6
// on_launch handler, after the executable layout check, before translation.
// Returns true if installed.
bool InstallAc6Timing(xe::Emulator* emulator);

}  // namespace xdroid

#endif  // XDROID_AC6_TIMING_H_
