/**
 * XDroid360: live cvar changes for on-device A/B runs (PES-1).
 */

#ifndef XDROID_LIVE_H_
#define XDROID_LIVE_H_

#include <filesystem>

namespace xdroid {

// Starts (once per process) a thread that polls `path` and applies the cvars it
// sets while a title runs, so tools/device/liveab.sh can alternate A and B
// segments within one flight instead of restarting the app for each. Only
// cvars whose users read them on every use are applied; other keys are logged
// as segment markers. Removes a stale file left by a previous session first.
void StartLiveConfig(const std::filesystem::path& path);

}  // namespace xdroid

#endif  // XDROID_LIVE_H_
