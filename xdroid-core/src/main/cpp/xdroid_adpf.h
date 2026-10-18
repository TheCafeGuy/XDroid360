/**
 * XDroid360: per-frame ADPF performance hint and thermal tier (ADPF-1).
 */

#ifndef XDROID_ADPF_H_
#define XDROID_ADPF_H_

namespace xe {
class Emulator;
}  // namespace xe

namespace xdroid {

// Starts (once per process) the thread that runs the native performance hint
// sessions and samples the thermal headroom. Idle (one wake per second) while
// xdroid_adpf is 0.
void StartAdpf();

// The emulator whose kernel resolves the guest audio producer's dispatch
// thread (xdroid_adpf_audio_guest_tid); null before it is destroyed. Blocks
// until the ADPF thread no longer uses the previous one.
void AdpfSetEmulator(xe::Emulator* emulator);

// Frame cap the thermal headroom forecast allows right now: 60, 40 or 30, or
// 0 if unknown (no thermal API, or xdroid_adpf is 0). Advisory: logged only,
// for the adaptive 60/40/30 target (P60-3) to consume.
int AdpfRecommendedFpsCap();

}  // namespace xdroid

#endif  // XDROID_ADPF_H_
