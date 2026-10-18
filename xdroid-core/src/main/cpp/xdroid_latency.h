// XDroid360 LAT-1: input and frame latency meter.
//
// Measures, on the steady clock:
// - swap -> cp: guest VdSwap to the command processor executing that swap;
// - cp -> present: that swap to vkQueuePresentKHR of the image showing it;
// - input -> read: a pad change to the first XInputGetState returning it;
// - input -> present: a pad change to the present of the first frame the
//   guest swapped after reading it.
// One XDROID-LAT line every 300 presents. Lock-free, never blocks.
#ifndef XDROID_LATENCY_H_
#define XDROID_LATENCY_H_

#include <cstdint>

namespace xdroid {

// Installs the command processor and presenter hooks (once).
void InstallLatencyMeter();

// Any thread that changes the pad state.
void LatNoteInputChange();
// Guest thread, XInputGetState returning |packet|.
void LatNoteInputRead(uint32_t packet);
// Guest thread, at every VdSwap.
void LatNoteGuestSwap();
// Guest thread, a ring buffer kick.
void LatNoteKick();
// CP thread, about to execute kicked commands.
void LatNoteCpExecute();

}  // namespace xdroid

#endif  // XDROID_LATENCY_H_
