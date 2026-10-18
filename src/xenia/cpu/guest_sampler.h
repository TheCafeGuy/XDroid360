/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_GUEST_SAMPLER_H_
#define XENIA_CPU_GUEST_SAMPLER_H_

#include <cstdint>

namespace xe {
namespace cpu {
class Processor;
namespace ppc {
struct PPCContext_s;
}  // namespace ppc

// XDroid360 CPU-1: in-process CPU sampler, for devices where simpleperf is not
// available. Each registered host thread gets a timer on its own CPU-time
// clock, so samples land only while the thread really burns CPU (a thread
// sleeping in a wait is never interrupted). A sample is the interrupted host
// PC; the report maps it to the guest instruction, splits spin-waits (code next
// to a db16cyc hint, or a configured range) from real work, and charges host
// helper time to the guest call site that entered it. Off unless
// guest_sampler_hz is nonzero; switching it is live. Linux/Android only, no-op
// elsewhere.
namespace sampler {

// Called on the thread itself, once, before it runs guest code.
void RegisterCurrentThread(Processor* processor, const char* name);

// The guest thread now running on this host thread (fiber switch or bind).
// context is null when no guest thread runs (the scheduler's idle fiber).
void SetCurrentGuestThread(uint32_t guest_thread_id,
                           ppc::PPCContext_s* context);

// Translation time: a db16cyc spin hint was emitted at this guest address.
void NoteSpinHint(uint32_t guest_address);

}  // namespace sampler
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_GUEST_SAMPLER_H_
