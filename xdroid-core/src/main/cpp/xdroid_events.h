/**
 * XDroid360 EVT-0: the event bus of the automatic bug detectors.
 *
 * Detectors report through XdroidGpuHooks::on_event (xe::gpu::XdroidEvent).
 * The bus counts every occurrence per (type, signature) in a lock-free table,
 * logs the first one as an XDROID-EVT line, and turns a new event of
 * severity 2+ into an automatic mark: the same 6 s frames + audio capture as
 * a user mark, at most one every 30 s. Counts that changed are logged as
 * XDROID-EVT-SUM lines once a minute.
 */

#ifndef XDROID_EVENTS_H_
#define XDROID_EVENTS_H_

#include <cstdint>
#include <string>

namespace xdroid {

// Installs the bus into the GPU hooks (once per process).
void InstallEventBus();

// The log frame and details of a pending automatic mark, if any; clears it.
bool TakeAutoMark(uint64_t& frame, std::string& text);

// Logs the counts that changed since the last call.
void LogEventSummary();

}  // namespace xdroid

#endif  // XDROID_EVENTS_H_
