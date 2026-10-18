# TEX-1: texture loads in flight (census 2026-10-15)

## Evidence (ce2 flight, build cd12a0bcc, xdroid_tex_census)

- 46-55 texture loads per flight frame, 27-31 MB per frame, 300-850 us CPU per frame (census hashing included).
- 90% are GPU-invalidated: render-to-texture outputs of the post-processing chain, reloaded every frame.
  Top keys per 300 frames: 1280x720 k_8_8_8_8 (2/frame, 3.6 MB each), 320x180 (3/frame), five 160x90
  (2/frame each), 208x144, and k_32_FLOAT 64x64/16x16/4x4 (luminance chain). All tiled, 1 level.
- 10% are CPU-invalidated, and 100% of those reloads brought back identical bytes (same XXH3): static DXT5
  textures (128-256 px, 8-9 levels) 1-1.5 times per frame, and a 4096x1536 DXT5 mip chain (2.7 MB) once per frame.
- XR-4 Phase 0: texload pass ends ~20/frame, 2.75 ms/frame of GPU between passes.

## TEX-1a: skip identical CPU reloads (97db686eb, live arm, default off)

Hash after RequestRange, skip the untile when the hash equals the bytes already in the host texture.
Expected: ~10% of loads and bytes, a few texload pass ends per frame, ~0.3 ms CPU. Small (Amdahl < 1.1x on
its own); kept because it is cheap and exact. Test: liveab `xdroid_tex_skip_same` false/true, census on,
watch `skipped=`, texload pass ends, gpu_ms, mW, and the glitch catcher (any stale texture = visible).

## TEX-1b: render-to-texture without the round trip (design, needs approval: architecture change)

Today each post-processing step: draw into EDRAM RT -> resolve (EDRAM -> shared memory, tiled) -> the next
draw samples it -> texture cache untiles shared memory -> VkImage. Two full copies of every target per frame,
and the untile ends the render pass.

Options, cheapest first:
1. Batch the loads: request all textures of the next draws before the pass starts, so loads stop ending
   passes one by one (pass ends, not bytes). Needs a look-ahead the command processor does not have; likely
   only within one draw's texture set today.
2. Resolve straight into the texture: when a resolve's destination range matches a cached texture key
   (same base, pitch, format, tiling), write the VkImage in the resolve shader too and mark the texture up to
   date for that range. Removes the untile (~26 MB/frame of reads + writes) and its pass break; the shared
   memory write stays for CPU readback and other formats. Risk: aliasing (two keys on one range), partial
   resolves, MSAA and scaled resolves. Prior art: none found in Canary/Edge/mainline to port.
3. Keep the GPU-written data in the host image and untile lazily only when a different key reads the range.

Recommendation: measure TEX-1a first; then prototype option 2 for the 1280x720 and 160x90 chain only, behind
a live arm, with the glitch catcher as the correctness gate.
