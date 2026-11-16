# A4: skip ownership transfers the next draw overwrites

Idea: Xendroid+ (phforner0) 17b844b93, 6ab5830f2 (reimplemented, no code taken).

## Evidence (A3 census, GPU-BENCH 2026-10-18, build 900ba45f1)

| trace  | transfers/frame | tiles/frame | overwrite by state | of which rect/quad |
|--------|-----------------|-------------|--------------------|--------------------|
| flight | 84.4            | 13166       | 32.0 (3591 tiles)  | 30.6 (3541 tiles)  |
| cine   | 42.7            | 9698        | 22.8 (2660 tiles)  | 22.8 (2660 tiles)  |

"Overwrite by state": full write mask, no blending, alpha test, A2C or pixel
kill; depth ALWAYS with writes and stencil off. Coverage is not checked yet.
Depth with stencil on: 19.1/frame (flight), excluded.

Cost being cut: transfers ~2.25 ms + ~1.1 ms of gaps per flight frame
(plan item U); 27% of the transferred tiles are in the candidate set, so the
bound is ~0.6-0.9 ms if every candidate is a full cover. Amdahl > 1.1x only
in GPU-transfer terms; on frame time it is ~3-4%, above the 0.3 ms gate.

## Mechanism

At `VulkanRenderTargetCache::Update`, before `PerformTransfersAndResolveClears`,
for each pending transfer of a render target the current draw overwrites by
state (the census predicate), drop the transfer when the draw provably covers
every pixel (all samples) of the transfer's tile range:

1. The draw is one rectangle (kRectangleList, 3 vertices) or one quad
   (kQuadList, 4 vertices), not indexed or with a trivially readable index
   buffer.
2. Run the vertex shader on the CPU for those vertices (ShaderInterpreter, as
   DrawExtentEstimator does for Y), apply the guest viewport/VTE transform,
   window offset and the half-pixel offset, and build the screen rectangle.
   For a quad, require the 4 corners to form an axis-aligned rectangle.
3. Intersect with the window and screen scissors; the transfer range (tiles
   start..end, converted to pixel rows/columns with the pitch and MSAA) must lie
   inside the covered rectangle. Partial cover: shrink the transfer to the
   uncovered rows only when the remainder is one contiguous tile range
   (second step, after the full-cover case is measured).
4. Vertex kill, a point size export, or any interpreter failure: keep the
   transfer.

Exact: the dropped pixels are rewritten by the draw before anything reads
them. Frames must stay bit-identical (gate).

## Arms and gate

- `xdroid_xfer_skip_overwritten` (live, default off) and a verify mode that
  performs the transfer anyway and counts how many it would have skipped.
- Census line gains `skipped=` so the bench shows the realised share.
- Gate: GPU-BENCH flight and cine frames identical, >= 0.3 ms on flight
  (plugged vs plugged), then default on in its own commit.

## Risks

- The CPU vertex shader reads vertex fetch constants and guest memory: a draw
  whose vertices the CPU cannot resolve keeps its transfer (no guessing).
- Depth: ALWAYS + write covers depth only; stencil-on draws stay excluded.
- MSAA: a covering rectangle writes every sample only without A2C and sample
  mask (RB_COLORCONTROL / PA_SC_AA_MASK must be full).
