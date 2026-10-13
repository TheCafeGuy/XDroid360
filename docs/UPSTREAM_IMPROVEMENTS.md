# Improvements over upstream xenia-edge

Changes made in XDroid360 (fork point `77f2cca80` of `has207/xenia-edge:edge`)
that are not Android-only and may be worth upstreaming. Each entry: what, why,
evidence, status. Device for all evidence: Adreno 750 (SD8 Gen 3), Turnip on
kgsl, Ace Combat 6. Status: **validated** (measured on device), **candidate**
(reasoned, not yet measured), **rejected** (kept here so nobody retries it).

## Vulkan

- **Queue global priority** (`vulkan_queue_global_priority`, default `high`,
  `src/xenia/ui/vulkan/vulkan_device.cc`). Requests
  `VK_QUEUE_GLOBAL_PRIORITY_HIGH` via `VK_KHR/EXT_global_priority`, falling
  back to the default if the driver refuses. Intended against kgsl
  `Preemption Fault` GPU hangs when system UI preempts long submissions.
  Correction (2026-10-08): Turnip on kgsl accepts the extension but ignores the
  priority (`submitqueue_priority_count = 1`, the kgsl context is created
  without priority flags), so it is a no-op there; preemption faults came back.
  May still matter on msm/DRM or other drivers. Status: no effect on kgsl.

- **GPU-written completion hint** (`vulkan_gpu_completion_hint`,
  `vulkan_command_processor.cc` + `vulkan_gpu_completion_timeline.cc`). Each
  submission ends with a fill of its index into a host-coherent word, and the
  timeline queries a fence only once that word reports the submission done.
  Why: `vkGetFenceStatus` sleeps in the kernel on Turnip/kgsl, and a second
  thread blocking in `vkWaitForFences` hung the GPU within minutes; polling at a
  fixed interval delayed completion by up to the interval (mission 26 -> 20
  FPS). The fence stays the authority, so a premature value only costs a query.
  Status: candidate (measurement in progress).

- **Primitive processor cache invalidation**
  (`src/xenia/gpu/primitive_processor.cc`): an overlap test compared
  `entry_end > physical_address_end` instead of `> physical_address_start`,
  so writes covering the start of a cached conversion did not drop it; also
  drops a conversion whose source was written while it was being built.
  Status: candidate (logic fix, no isolated measurement).

- **SPIR-V loop start skips before pushing** (`spirv_loop_skip_without_push`,
  default on, `SpirvShaderTranslator::ProcessLoopStartInstruction`). The SPIR-V
  translator pushed the loop count and aL stacks before the zero-count skip,
  and the skip label is past `loop_end`, the only pop. A skipped loop nested in
  another left a zero on top: the outer `loop_end` decremented it to
  0xFFFFFFFF and kept iterating (each re-entry pushes another zero), with the
  skipped loop's aL. The DXBC translator checks before pushing ("Skip the loop
  without pushing if the count is zero from the beginning"); Canary's and
  Edge's SPIR-V translators have the bug. Evidence: every kgsl GPU hang and
  timeout logged in AC6 missions (15 events, 7 sessions) has `RBBM_STATUS`
  0x00E70187/0x00E701A7: SP, HLSQ, VPC, UCHE, RB busy; RAS, TPL1, VFD, PC idle,
  i.e. resident pixel shader waves running an ALU/constant-load loop after
  rasterization ended. Status: candidate (code-proven, device run pending).

- **Main control-flow loop bound** (`spirv_max_main_loop_iterations`, default
  2^17, twice two fully nested 255-iteration loops). Every guest jump and
  loop end goes through the translated shader's main loop; an invocation
  gives up after the bound, so a runaway guest loop
  costs a glitch instead of a GPU hang. At 2^20 a runaway over a few thousand
  pixels still outlasted the kgsl timeout (recovered "GPU hang detected"
  followed by a fatal timeout). Upstream has no bound. Status: candidate.

- **SPIR-V `setp_clr` marks the predicate written**
  (`spirv_shader_translator_alu.cc`). Every other predicate write sets
  `predicate_written`; `setp_clr` did not, so later `(p0)` instructions and
  jumps in the same exec skipped the re-check and ran on the stale predicate.
  The DXBC translator sets it. Status: candidate (code-proven).

- **SPIR-V `cube` face id under the W mask** (`spirv_shader_translator_alu.cc`).
  The Y-major branch computed the id only under the X mask; a destination
  writing W but not X merged a missing value into the phi (invalid SPIR-V,
  lost draw). Status: candidate (code-proven).

- **SPIR-V 2D register gradients** (`spirv_shader_translator_fetch.cc`): H and
  V registers were swapped for 2D fetches (3D/cube and DXBC map H to ddx).
  Status: candidate (code-proven).

- **SPIR-V stacked layer clamp for unnormalized fetches**: the normalized
  path clamped the array layer, the unnormalized one did not (Inf/NaN Z =
  undefined layer); DXBC clamps both. Status: candidate (code-proven).

- **Auto-indexed line loops closed with an index buffer**
  (`primitive_processor.cc`). The strip's excess vertex must be vertex 0; only
  the DXBC translator mapped `line_loop_closing_index` to 0, so on SPIR-V
  backends the closing segment went to vertex N. A CPU index buffer
  [0..N-1, 0] works with every translator. Status: candidate (code-proven).

## CPU

- **Guest hook registry** (`Processor::SetGuestHook`, entry or replacement
  hooks emitted in `PPCHIRBuilder::Emit`). Lets per-title fixes run host code
  at a guest function entry or replace it, without patching guest memory.
  Used for an Ace Combat 6 race fix ported from the AC6 static recomp.
  Status: validated.

- **`mtfsf` field mask** (`InstrEmit_mtfsfx`, `ppc_emit_fpu.cc`). The FM bits
  were mapped to FPSCR fields in reverse (FM bit 0 picked the RN nibble), so
  only `mtfsf 0xFF` (the one form the tests used) worked: `mtfsf 0x01` to set
  the rounding mode changed the exception bits instead. Test:
  `ppc/testing/instr_mtfsf.s`. Evidence: PowerISA mtfsf definition (sweep
  2026-10-09). Status: candidate.

## VFS

- **Content extraction reports I/O errors**
  (`VirtualFileSystem::ExtractContentFile` / `ExtractContentFiles`). Upstream
  ignores `fwrite`/`fclose` results and each file's status, so a full disk
  installs a truncated package that reports success; a read returning 0 bytes
  loops forever. Now: `X_STATUS_DISK_FULL` / `X_STATUS_END_OF_FILE`, and the
  first failing file stops the extraction, read errors are no longer ignored,
  and a partly written file is deleted (the on-demand extraction in
  `xam_content.cc` skips files that exist, so it would have reused a
  truncated one). Also benefits the desktop
  installer in `Emulator::InstallContentPackage`, which already checks the
  status. Evidence: code reading (sweep 2026-10-09). Status: candidate.

## Kernel

- **Timer APC thread released outside `timer_lock_`** (`XTimer::SetTimer` /
  `Cancel`, `src/xenia/kernel/xtimer.cc`). Both dropped the previous APC
  thread's reference while holding `timer_lock_`. For an exited thread that last
  release runs `~XThread` -> `UnregisterThread`, which takes the global lock,
  while `CancelAll` (KernelState teardown) takes the global lock and then each
  `timer_lock_`: an ABBA deadlock when a guest thread re-arms a timer as the
  title closes. `RemoveApc` now hands the reference back so callers release it
  after unlocking. Evidence: code reading of the lock order (sweep 2026-10-09),
  commit 4011527f5. Status: candidate.

## Emulator

- **`Emulator::Pause`/`Resume` are unsafe** (not changed upstream-side; the
  Android app no longer calls them, `XDroidApp::PauseEmulation`). `Pause`
  suspends the GPU thread, then waits on `AudioSystem::Pause`'s fence, which
  never signals while the audio worker is inside a guest callback waiting on
  guest threads that wait on the GPU; it host-suspends threads at arbitrary
  points; under the guest scheduler it calls `thread()->Suspend` on fibers,
  whose `thread()` is null. `Resume` resumes only threads with
  `!is_running()` (ada971aef), but `running_` means alive, not unsuspended,
  so suspended live threads are never resumed. Evidence: device session
  2026-10-11 09:00, the scheduler watchdog reported fibers still switching
  with no frame presented after the pause request, and the game never
  presented again. Status: candidate (code-proven).

## Rejected (do not retry without new evidence)

- `db16cyc` spin hint as `yield` instead of `isb`: `yield` is a NOP on
  Cortex-X/A7xx, so guest spin loops only spin faster and burn more power.
  Upstream `isb` kept.
- Asynchronous fence waiter thread (`vulkan_async_fence_waiter`): faster, but
  hung the GPU on Turnip/kgsl every 2-10 min. Kept, default off.
- SPIR-V vertex shaders do not wrap the offset vertex index to 24 bits nor clamp it to
  VGT_MIN/MAX_VTX_INDX (the DXBC translator does, RemapAndConvertVertexIndices); our 3fdbf8577
  reuses the tessellation min/max system constant (set every draw).
- Turnip a7xx (A750, 26.2.3): compute reads of a 2x MSAA D32S8 depth attachment right after its
  render pass hang the GPU at a low rate; candidate driver cause = concurrent resolves
  (RB_CCU_CNTL CONCURRENT_RESOLVE_MODE_2). TU_DEBUG=noconcurrentresolves ran two full missions
  with 0 hangs (2026-10-12) and is now our default (xdroid_driver.cc). Report to Mesa.
