/**
 * XDroid360: live cvar changes for on-device A/B runs (PES-1).
 */

#ifndef XDROID_LIVE_H_
#define XDROID_LIVE_H_

#include <filesystem>
#include <string>

namespace xe::gpu {
class GraphicsSystem;
}  // namespace xe::gpu

namespace xdroid {

// Starts (once per process) a thread that polls `path` and applies the cvars it
// sets while a title runs, so tools/device/liveab.sh can alternate A and B
// segments within one flight instead of restarting the app for each. Only
// cvars whose users read them on every use are applied; other keys are logged
// as segment markers. Removes a stale file left by a previous session first.
void StartLiveConfig(const std::filesystem::path& path);

// GPU-BENCH capture: once set, `xdroid_gpu_trace_frames = N` in live.toml
// streams about N frames into <storage>/traces/ (needs a build with
// XENIA_ENABLE_GPU_TRACE; the trace writer is compiled out otherwise).
void SetLiveGraphicsSystem(xe::gpu::GraphicsSystem* graphics_system);
// The graphics system set above, or null.
xe::gpu::GraphicsSystem* GetLiveGraphicsSystem();

// Applies TOML text ("name = value" lines) as a live.toml change would: live
// cvars switch, other keys are only logged. GPU-BENCH uses it to alternate
// arms within one replay (xdroid_gpu_bench_arms).
void ApplyLiveText(const std::string& text);

}  // namespace xdroid

#endif  // XDROID_LIVE_H_
