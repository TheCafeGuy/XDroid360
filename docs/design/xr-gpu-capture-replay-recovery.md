# XR-1 / XR-2: GPU capture-replay and GPU reset recovery (design, 2026-10-11)

Status: PROPOSED, needs user approval (architecture change; XR-1 also adds a JNI entry).
Motivation: GPU-H2 hangs (kgsl `gpu timeout`, no page fault) depend on CPU timing, so every hunt
step costs a user session, and every hang ends the session. Two systems, in this order.

## XR-1: on-device GPU capture and replay (repro without the user)

Goal: turn a hang or a one-frame glitch into a deterministic case that runs on the phone in a
loop with no guest CPU, and bisect it to one draw without user runs.

What exists (upstream, in our tree): `TraceWriter` (packets, memory reads deduplicated by hash,
`kRegisters`, `kEdramSnapshot`), `TracePlayer`, `trace_dump` (stop at a frame/command, dump
EDRAM/memory/images). The writer is compiled out of Release unless `XENIA_ENABLE_GPU_TRACE`.

Design:
1. **Capture build** (forensic, `(temp, revert-planned)` flag only, never compared with perf
   numbers): Release + `XENIA_ENABLE_GPU_TRACE`.
2. **Rolling capture** (new, xdroid-core + a small TraceWriter hook): the stream is cut into
   self-contained segments of N frames (default 30). Each segment starts with a register snapshot
   and an EDRAM snapshot, and memory reads are deduplicated only within the segment, so any
   segment replays on its own. Keep the last K segments (default 6, ~6 s at 30 FPS) on disk under
   `files/xdroid/traces/`, delete older ones. On DEVICE_LOST, a kgsl hang, or a glitch mark, the
   retained segments are frozen (renamed) instead of rotated. Writing happens on a worker thread
   from double buffers so the GPU thread only memcpys (timing perturbation is the main risk: the
   bug is a race; measured by comparing hang rate with capture on/off).
3. **Replay mode** (new): a debug entry that boots no title, loads a frozen segment and runs
   `TracePlayer` on the Vulkan backend in a loop, logging per-submission completion. A hang shows
   as a fence timeout/DEVICE_LOST in replay, which proves the command stream alone hangs (no CPU
   race needed) or not (then the race is guest-memory timing: compare replayed vs live memory).
4. **Auto-bisect** (`tools/device/replay_bisect.sh`): replays with `trace_dump_stop_command = N`,
   restarting the process after each hang, binary search to the first hanging command, then
   dumps that draw's state (shaders, bindings, RT keys, viewport, vertex ranges) to the log.

Cost: ~1-2 days of work. Payoff: GPU-H2 and HUD-1 become host-driven hunts; the same captures
serve as regression tests for every later GPU change ("replay the 10 known-bad segments").
Exit: one real hang captured, replayed to a hang on the phone, bisected to one command.

### Red team 2026-10-11 (Correctness, Platform, Cost/Scope): LOW on all three lenses

- The upstream writer is synchronous `fwrite` + snappy on the CP thread (trace_writer.cc:85-263);
  a full mission stream is est. 0.3-1.8 GB/s, so "the GPU thread only memcpys" needs a rewrite,
  and the perturbation can hide a race that fires after 0.5-4.5 min.
- A segment is not self-contained: data already valid in GPU shared memory before the cut, and
  GPU-written (resolve/memexport) ranges, are never in it (memory enters the trace only on upload,
  vulkan_shared_memory.cc). The EDRAM snapshot also erases host RT ownership state, which is
  exactly where the hang sits (ownership transfers at pass start).
- Replay without guest CPU cannot reproduce a timing race; a non-hanging replay is inconclusive
  unless it loops >= 25k frames (3x live MTBF), and bisect needs >= 9/10 replays to hang.
- Cheaper discriminator first: GPU breadcrumbs in the live run. Turnip on KGSL exposes
  `VK_AMD_buffer_marker` (third_party/mesa tu_device.cc:394), allowed inside a render pass; the
  marker goes into `draw_cs`, which Turnip replays per bin (and in the binning pass), so a
  per-pass "last draw done" slot holds the last draw completed in the current bin.

### Revised order

- **XR-1a GPU breadcrumbs** (cvar `vulkan_gpu_breadcrumbs`, default off, forensic: runs with it
  on are never compared with perf numbers). Per render pass a slot of 4 u32 in a host-visible
  buffer: `begin` (TOP marker after begin), `issued` (TOP before each guest draw), `done` (BOTTOM
  after the pass prologue and after each guest draw), `end` (BOTTOM after the pass end). A CPU
  ring keeps each draw's VS/PS hash, vertex count, primitive and index type. On device loss the
  oldest pass the CP reached but did not end is the stuck one; `done` names the last finished
  draw and the next draw is the suspect (`done` older than the prologue = stuck in the tile load,
  ownership transfers or clears). Values are global sequence numbers, so stale slots are visible.
  Kill/decide after 2 hangs: same draw (shader pair and position) both times -> stream-intrinsic,
  build XR-1b around it; different draws -> timing race, replay is not worth building; no hang in
  2 runs of >= 10 min -> the markers perturb, retry with `TU_DEBUG=nocb` as the variable.
- **XR-1b capture/replay v0** only if XR-1a names a stable draw: full stream
  (`trace_gpu_stream`) with stop-on-hang, replay through `trace_dump`; gates from the red team
  (shared-memory coverage check, MTBF shift < 2x with capture on, >= 9/10 replay hangs before
  bisect). Rolling segments only if the full stream proves too big or too perturbing.

## XR-2: GPU reset recovery (a hang costs a hitch, not the session)

Goal: on DEVICE_LOST, rebuild the Vulkan side and continue the game. The guest owns all
authoritative state (RAM, registers, ring buffer); the host GPU objects are caches of it.

Design:
1. On DEVICE_LOST (any wait/submit): stop the GPU command processor at the next packet boundary,
   mark the frame as lost, and keep the guest running (its waits on the GPU are fed by the CP; the
   CP is paused briefly, like a long frame).
2. Tear down every Vulkan object (presenter, caches, pipelines, shared memory buffer, device),
   create a new device on the same physical device, re-run the backend's initialization, and
   reload the VkPipelineCache blob so pipelines come back from the driver cache quickly.
3. Rebuild state lazily: shared memory pages marked invalid (re-uploaded from guest RAM on use),
   textures and render targets recreated on use; EDRAM content is lost, so the first frame after
   recovery may be black or partial (one-frame glitch instead of a crash).
4. Resume the CP; log `XDROID-GPU-RECOVERED n (ms)`; give up after 3 recoveries in 60 s (then the
   current behaviour: end the session with a crash report).

Risks: teardown paths in upstream assume process exit; threads (pipeline creation, readback)
must be quiesced first; Turnip/kgsl must allow a new device after a lost one in the same process
(check first with a 50-line test). Cost: ~2-4 days. Exit: forced hang (debug cvar that submits
an infinite-loop compute shader) recovers in < 2 s, 10 times in a row, game keeps running.

## Order and gates

XR-1 first (it also finds the root cause of the current hang); XR-2 after GFX-0 closes. Each gets
a `xdroid-redteam` pass before code, and its own branch.
