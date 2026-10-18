# SHADER-1: shader/pipeline compile stutter (analysis + switches)

Status: code only, syntax-checked on host (clang -fsyntax-only, non-Android
defines), not built or run on the phone. Both behaviour switches default OFF.

## 1. What the tree already does (Edge upstream, Vulkan backend)

| Piece | Where | State |
|---|---|---|
| Persistent VkPipelineCache `{TITLE}.vk.bin` | `vulkan_pipeline_cache.cc` InitializeShaderStorage / WriteVkPipelineCacheLocked / MaybeSaveVkPipelineCache | On. Loaded at title start, saved periodically and at shutdown. Driver change is safe (vkCreatePipelineCache rejects a foreign header UUID). |
| Shader storage `.xsh` (guest ucode) + pipeline storage `.xpso` (descriptions) | `shader_storage.h`, `graphics_system.cc` `store_shaders=true` | On. Per title. This is the "stored per-game list". |
| Warm-up (pre-compile everything in `.xpso` at title start) | InitializeShaderStorage, `PushUnordered` | On, non-blocking: queued to the creation threads while the game boots. |
| Async creation threads | `pipeline_creation_queue.h`, `vulkan_pipeline_creation_threads` (Android forces 3 in `xdroid_app.cc`) | On (`async_shader_compilation=true`). |
| Placeholders while a pipeline builds | ConfigurePipeline: VS ucode interpreter + no-op PS, or real VS + no-op PS | On (`async_shader_vs_interpreter=true`). |
| Skip draws with no placeholder | `async_shader_skip_draws=true` | On (pop-in instead of stall). |

So the "obvious" fixes (disk cache, async, pre-warm) are already in. The
remaining stutter comes from the paths below, which still block the GPU
command-processor (CP) thread.

## 2. Remaining stall sources on the CP thread

1. **Await of the whole queue.** A draw into a target that is not redrawn every
   frame, small, or memexport (`stand_in_allowed == false`) calls
   `AwaitPipelineCompletion()`, which waits for **everything** queued, including
   the whole `.xpso` warm-up. Early in a mission that can be the entire warm-up.
   Same for `AwaitQueryResolve` (ZPD) draining before closing a submission.
2. **Warm-up blocks live pipelines (priority inversion).** The queue is one FIFO:
   the warm-up is pushed first, so a pipeline the game needs *now* (not in
   `.xpso` yet) builds after the whole warm-up. Meanwhile its draws use the
   placeholder (grey/flat output) or are skipped. This is a visual-glitch source
   for the first seconds/minutes of a session, not only a stall.
3. **Early submits held back during warm-up.** `CanEndSubmissionImmediately()`
   is false while *anything* is queued, warm-up included, so
   `submit_on_primary_buffer_end` does nothing during warm-up, and ZPD query
   opens defer.
4. **Placeholder pipelines are created synchronously** on the CP thread
   (`vkCreateGraphicsPipelines` with interpreter VS or real VS + no-op PS). On
   Turnip that is a real ir3 compile, ~1-10 ms each [verify on device].
5. **Draws with no pixel shader (depth-only, shadows) have no async path**
   (`CanCreatePipelineAsync` needs a PS): full translate + compile on the CP
   thread.
6. Translation on the CP thread when there's no async path
   (`translate_here`).

None of these were measured on the phone yet; the counter below is for that.

## 3. What this branch adds

### 3a. Counter (measurement): `xdroid_shader_stall_log` (default false, live)

Counting is always on but lives only on paths that already compile (one clock
read pair each), so a frame without new shaders costs nothing. The cvar only
gates the log. Per frame with anything to report:

```
ShaderStall f1234: 12.40 ms | translate 1/3.10 | sync_pso 1/6.20 | placeholder 2/3.10 | await 0/0.00 | query_drain 0/0.00 | skipped_draws 5 | submit_deferred 1 | queue live 3 total 812
```

Every 600 frames:

```
ShaderStall summary: 600 frames, 14 with stalls, 6 hitches (>= 4 ms), 48.2 ms total, worst frame 22.10 ms, 31 skipped draws
```

`xdroid_shader_stall_hitch_ms` (default 4) sets the hitch threshold.
`skipped_draws` and `submit_deferred` are not stalls (pop-in / lost early
submit) but are logged because they explain glitches and latency.

### 3b. `xdroid_pipeline_live_first` (default false; read when the warm-up queues, so set it before starting the title)

Warm-up requests go to a low-priority queue; creation threads take it only when
no live (game-drawn) request is queued. Fixes source 2. Never worse for a
warm-up entry the game is already drawing than one live queue length.

### 3c. `xdroid_pipeline_targeted_wait` (default false, live)

- A no-stand-in draw waits only until **its own** pipeline is stored
  (`PipelineCreationQueue::AwaitUntil`). Ordered publish means that also covers
  live pipelines the game drew before it, but not the warm-up. Fixes source 1.
- `IsCreatingPipelines()` / ZPD drain look only at live requests
  (`IsLiveBusy`, `AwaitLive`). Warm-up pipelines are not referenced by any
  recorded command, so they can't affect a submission. Fixes source 3.
- A wait is never longer than the old full-queue wait.

Files: `src/xenia/gpu/pipeline_creation_queue.h` (two queues, live counter,
`AwaitUntil`, `AwaitLive`, `IsLiveBusy`, `GetPendingCounts`; default behaviour
unchanged; D3D12 call sites compile unchanged),
`src/xenia/gpu/vulkan/vulkan_pipeline_cache.{h,cc}`,
`src/xenia/gpu/vulkan/vulkan_command_processor.{h,cc}`.

## 4. Effect on game timing (HUD-1 risk)

HUD-1 was a guest-side slot race, so anything that changes when the CP thread
retires work can move it.

- Counter: no timing effect (clock reads only on compile paths; one struct
  copy per frame).
- `targeted_wait`: **changes timing.** CP thread blocks shorter, submits earlier
  during warm-up, ZPD results arrive earlier. This is the intended gain but it
  shifts guest-visible GPU progress. Run the HUD-1 detector with it on.
- `live_first`: changes which pipelines exist when, so placeholders/skips
  shift between frames. Visual, not guest timing.
- Already in the tree, not new: async + skip draws means the first frames of a
  new effect render with a placeholder or not at all. If a one-frame glitch
  coincides with `skipped_draws > 0` or a `placeholder` in the stall log for that
  frame, it's this, not a guest race.

## 5. Suggested PC run (A/B/A, same scene)

1. Arm A: `xdroid_shader_stall_log=true`, others off. Cold `.vk.bin` (rename it)
   once, then warm. Mission start + 60 s.
2. Arm B: + `xdroid_pipeline_live_first=true` + `xdroid_pipeline_targeted_wait=true`.
3. Arm A again.
4. Compare: hitch frames, worst frame, skipped draws, FPS p95/p99, HUD-1
   detector hits. Kill criterion: no drop in hitch frames or worst frame on warm
   cache, or any new HUD-1/visual regression.

If the log shows `placeholder` or `sync_pso` dominating instead of `await`, the
next lever is in section 6, not these switches.

## 6. Next options (not implemented, need measurements first)

- **VK_EXT_graphics_pipeline_library** (Turnip supports it): pre-build VS/PS
  libraries and fast-link at draw time; placeholder and depth-only creation
  become a link instead of a compile (sources 4, 5). Larger change.
- **Promote on demand:** when the game hits a warm-up entry still queued,
  move it to the live queue (needs a taken-flag so it is not built twice).
- **Async path for depth-only draws** using real VS + no PS as its own
  placeholder (shadows pop in instead of stalling).
- Interpreter for VS with texture fetch/loops (fewer skipped draws).
