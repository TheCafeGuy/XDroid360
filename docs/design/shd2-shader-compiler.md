# SHD-2: a real compiler for Xenos shaders

Status: plan (2026-10-18). Owner: PC thread. Needs the maintainer's go (architecture change).

## P1 results (2026-10-19)

| change | flight ms | frames |
| --- | --- | --- |
| S1 structured jumps (v17, bb12ca710) | 19.34 -> 18.97 (base later 18.31) | identical; cine 1.75 both |
| no vertex bounds check (probe bit 1) | 18.31 -> 18.80 | identical (probe) -> no gain, rejected |
| two extra vertex swaps (probe bit 2, exact) | 18.84 -> 19.03 (3 rounds, medians) | identical -> one swap ~0.1 ms |
| alpha test function as spec constant (2b38ae300, reverted c9b33d712) | -0.07 ms, CI [-0.21, +0.08] | identical -> no gain |
| cheaper exact 21-bit rounding | not built: exact form saves 1 of 9 ops | - |

PS epilogue (alpha test, alpha to coverage, color exponent: ~90 ir3 lines of uniform branches on system
constants): ceiling already measured on 2026-10-15 by xdroid_shd_bound_noalpha, -0.55 ms of 27 ms (~2%,
includes early-Z/LRZ). PS rounding ceiling 0.29 ms. Every remaining P1 item is <= 2% (Amdahl < 1.1x, hard
rule 1): P1 is exhausted at flight ~18.3-18.8 ms. Next is P2 (XIR core) or a different target.

Main VS anatomy (ir3, 345 instr): every fetched word costs ~14 instructions: word address add, binding
index (addr >> 25, & 3, << 1 for the 4 x 128 MB shared-memory bindings, maxStorageBufferRange 128 MB),
binding < 4 and word < fetch end compares, their AND, an isam through the descriptor array, a select, and
the 8in32 swap (shrm, and, shlg, shl, shrg). Removing the end compare + select gained nothing, so per-word
ALU is not the VS bottleneck at this size. Probes that change vertex data are banned: forcing endianness
(xdroid_shd_bound_vendian=0) made garbage geometry, 66 ms frames, froze the phone UI. The exact
double-swap probe (36af08b1d, two extra swaps) kept frames identical: its first round reported stale
vsp1 logs (bench bug); vsp3 (3 rounds, fresh logs) gives +0.19 ms for two swaps. Vertex ALU (swap,
bounds) is not hot; the frame hash flips between 02c4ba44 and 58004e45 in both arms (bench nondeterminism).

Red team 2026-10-19 (Evidence, Platform, Prior art): LOW. No prior art; ir3 maps read-only SSBO loads
to isam on purpose (texture L1), ldg skips it; BDA drops robustness and adds int64 pairs. Gate: measure
first (perfcounters or exact probes, A/B/A >= 3 rounds) that fetch, not ALU, is what the VS waits on.

Candidate (Proposal Card): load shared memory through one buffer device address (ldg,
global path) instead of the 4-binding isam array: drops the binding math and the texture-pipe fetch.
Costs: bufferDeviceAddress feature, PhysicalStorageBuffer64 addressing (64-bit address math lowered to
32-bit pairs on ir3), base address in the system constants. Evidence still missing: whether isam vertex
fetch latency (not ALU) is what the VS waits on - get it from a Turnip perfcounter read, not a probe.

## P0 results (2026-10-19)

| Probe | Main PS ir3 instr | Flight GPU | Frames |
|---|---|---|---|
| base (v15) | 974 (body 778 + preamble) | 19.76 ms | - |
| integer-scale spec (kept, v16) | 882 (body 663) | 19.38 ms (-0.39) | identical |
| no 21-bit rounding (probe, not exact) | 872 | -0.29 ms ceiling | differ |
| native min/max (cvar, off) | - | tie | identical |
| anisotropy forced 1x (not kept) | - | -0.23 ms | differ |
| relaxed mul VS only / VS+PS (v15) | - | -0.46 / -1.2 ms | identical |

Reading: the ~970 count included the early preamble (once per draw); the per-pixel body is 663 against a
~300-350 floor, i.e. ~2x, not 10x. Removing PS selects alone does not move time; the VS-only relaxed-mul
win (-0.46 ms) says vertex work is a real share: Turnip's GMEM path runs a binning VS plus the full VS
(main pass ~340k indices/frame, VS ~420 ir3 instructions). Revised P1 order: (1) VS cost (binning
position path, vertex fetch, constant handling), (2) per-fetch specialization rest, (3) PS emulation.

## Why (measured)

- The flight frame is ~41% one render pass (1280x768, 2x MSAA, 629 draws), and that pass is shader work.
- Its main pixel shader (PS 35A80CB4 / C7A97572) is ~70 guest ALU pairs + 8 texture fetches. Our translation
  becomes **~970 ir3 instructions** (XDROID-SHSTAT, Turnip pipeline statistics), 12 registers, **8 waves per
  core** (light shaders get 16). Its SPIR-V has 215 blocks, 273 OpSelect, 417 loads, 385 access chains.
- Two small edits to the generated code today (21-bit rounding, "0 * x = 0") took 1.7 ms off the frame with
  bit-identical output. The specializations already done (SHD-1a/c/e) took ~3 ms. The direction pays; the
  patches are reaching their limit because the translator's shape is the problem.
- No upstream (Canary, Edge, mainline) has an IR/SSA Xenos compiler; this is new work (prior-art pass below).

## Where the 10x comes from (from the code, to be confirmed in P0)

1. **Every texture fetch decodes its fetch constant on the GPU** (6 words: dimension, size, format class,
   swizzle, signs, exponent adjust, LOD bias, filter flags, border...) and then emulates the result.
   8 fetches x ~60-80 instructions is most of the 970.
2. **Runtime control flow**: bool and loop constants are read from uniform buffers at run time; any jump
   turns the shader into a loop + switch on the program counter. The driver keeps every value live across
   that loop (phis at the header), which costs registers and so waves.
3. **Register file as an array** (Function-scope vec4[N]); fine when indices are static (NIR scalarizes it),
   expensive when the shader uses aL/a0 relative addressing or the PC loop blocks the analysis.
4. **Predication** (p0) lowered to selects around every predicated instruction, both sides computed.
5. **Generic ALU emulation**: vector+scalar co-issue, swizzles, write masks, retain_prev, saturate, all
   expressed as extract/insert/shuffle chains the driver must clean up.

## Target

Main-pass PS from ~970 to <= 350 ir3 instructions and 12-16 waves; main pass -30..40%; flight 19.8 ms ->
~16-17 ms; less GPU work at the 30 FPS cap = less energy. Frames bit-identical on every bench trace.

## Architecture

```
ucode (existing decoder, Shader analysis)
  -> XIR: SSA, scalar components, explicit predicates, typed values
  -> passes (with the draw's specialization state)
  -> SPIR-V emitter (reuses the current per-op exactness rules)
```

- **Specialization state** (pipeline key, the way SHD-1 already does signs/endianness): bool constants
  used, loop counts/start/step used, each fetched texture's static fields (dimension, format class, signs,
  swizzle, exponent adjust, filtering class), alpha test, interpolator setup. Float constants stay dynamic.
- **Passes**:
  1. Fold specialization constants; resolve bool/loop control flow at compile time.
  2. Structurize the remaining CFG into if/loop (Ryujinx `StructuredProgram`, MIT; ideas only from the
     GPL yuzu/Dolphin/RPCS3 work). Keep the PC switch only for irreducible flow.
  3. Registers to SSA; an array only for the range a relative access can reach.
  4. Predicates to branches or selects by cost.
  5. Texture fetch lowering: only the code the fetched texture's static fields need; dynamic fields
     (sizes, LOD bias) pre-decoded on the CPU into a compact per-draw buffer.
  6. CSE, DCE, constant folding, copy propagation on XIR (cheap, deterministic).
- **Generic fallback**: the current translator stays as the slow path. A draw whose specialized shader is
  still compiling uses it (Dolphin's ubershader model); a shader the new compiler can't prove gets it for
  good. Switch per shader hash (cvar + list) so a bad shader is bisectable (Edge's shader bisect tooling,
  0413471da).
- **Placement**: new compiler in `xdroid-core/shader/`; the upstream translator gets one hook.

## Phases and gates

| Phase | Work | Gate |
|---|---|---|
| P0 measure (1-2 days) | Top-20 pipeline table (SHSTAT x SHDRAW). Two cheap probes on the current translator: bool/loop constants as specialization constants; fetch static fields as spec + skip decode. | Probes show >= 1 ms on flight with identical frames, else stop and rethink |
| P1 fetch rework | Spec + CPU-decoded fetch constants in the current translator (biggest expected share). | Flight/cine identical, main PS instr count -40% |
| P2 XIR core | Decoder -> XIR -> SPIR-V for straight-line shaders, then structurizer + SSA registers, behind the per-shader switch. | All AC6 shaders compile; differential test vs old translator on every trace; frames identical |
| P3 specialization at scale | Async specialized variants + fallback, variant cache in the pipeline storage. | No new stutter (frame-time p99 equal), pipeline count bounded |
| P4 cleanup | Remove dead paths, docs, UPSTREAM_IMPROVEMENTS entry. | Build + lint, bench, the maintainer mission |

## Validation

- Bench frame hashes (flight, cine, hangar once the trace runs again) must stay identical at every step.
- Differential harness: compile each AC6 shader both ways and compare outputs on the trace's draws
  (replay with the per-shader switch flipped).
- SHSTAT before/after per shader: instructions, registers, waves, stalls.
- the maintainer mission at the end of each phase with the glitch recorder.

## Risks

- Exactness: Xenos semantics (NaN, -0, 21-bit rounding, co-issue order) must match; mitigated by reusing the
  emitter's per-op rules and the bit-identical gate.
- Compile stalls from more variants: async + fallback; spec keys only on fields AC6 actually varies.
- Upstream sync: the new code lives in xdroid-core; the old translator keeps tracking upstream.
- Size: P2 is the big one (weeks). P0/P1 deliver value on their own and decide whether P2 is worth it.

## Prior art (2026-10-18 scout)

- Canary/Edge/mainline 2025-06..2026-10: only accuracy fixes and narrow specializations
  (Edge 8d75a1779, 830e60518; Canary 7cd47947b, 3a44f20c7). No IR, no structurizer.
- Ryujinx Graphics.Shader (MIT): decode -> SSA IR -> passes -> StructuredProgram -> SPIR-V; specialization
  state keys the cache. Closest template.
- yuzu/sudachi shader_recompiler (GPL, ideas): goto elimination (Erosa-Hendren), SSA, const propagation.
- Dolphin (GPL, ideas): ubershader + async specialized shaders.
- hedge-dev XenosRecomp: Xenos -> HLSL text, no dynamic register indexing; license to check before reading.
- ir3: local arrays live in registers; relative access serializes on a0 and blocks SSA promotion.
