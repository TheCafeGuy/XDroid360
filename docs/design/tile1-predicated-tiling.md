# TILE-1: one host pass for AC6's predicated tiles

Status: research + bound experiment ready (2026-10-16). Architecture change: needs the maintainer's approval before Phase 2.

## Evidence

- GPU-BENCH pass profile (b9eb447ea+, 2026-10-16, `docs/runs/2026-10-16/flightprof.log`, `cineprof.log`):
  - Flight, median frame 25.7 ms: two `guest 640x768 d=629 end=barrier` passes = 5.9 + 5.5 = 11.4 ms (44%).
    Resolves 1.56 ms in-pass + 2.30 ms gaps.
  - Heavy cutscene frame (p90, 61.5 ms): the same two passes (d=422) = 15.4 + 13.4 = 28.7 ms (47%).
- Trace walk (`scratch/tile1/xtr_tiles.py`, `xtr_diff3.py` on `docs/runs/2026-10-15/traces/4E4D07D1_stream.xtr`):
  - Tile A: `SET_BIN_SELECT 0x80000003`; tile B: `0x0000000C`. Both replay the **same two indirect buffers**, with 628 draws and an identical draw signature.
  - Per-tile predicated `SET_CONSTANT`s (12 per tile) differ only in:
    - `PA_SC_WINDOW_OFFSET`: A 0, B x = -640;
    - `PA_SC_WINDOW_SCISSOR_TL/BR`: A (0,0)-(640,720), B (640,0)-(1280,720);
    - `RB_COPY_DEST_BASE` of one resolve: 0x1B290000 vs 0x1B2A4000.
    The other resolve destinations are shared.
  - Surface: `RB_SURFACE_INFO 0x0A010280`, pitch 640, 2x MSAA. 1280x720 2xMSAA color+depth (~14.7 MB) does not fit the 10 MB EDRAM, hence the tiles.
  - Tile A only:
    - per draw: `EVENT_WRITE SCREEN_EXT_INIT` + `EVENT_WRITE_EXT SCREEN_EXT_RPT` (screen extents into a 16 B/draw array);
    - once: `INTERRUPT` and two `WAIT_REG_MEM`.
    Reading: D3D's CPU callback turns the extents into per-draw bin masks and patches the IB's `SET_BIN_MASK_LO` payloads before tile B runs. In tile A the payload is the 0xFFFFFFFF template; in tile B it is 0x8000000F for every draw, because Xenia fakes full-screen extents (`pm4_command_processor_implement.h` `EVENT_WRITE_EXT`). So no draw is ever culled from tile B.
- Prior art: none found for merging tiles (Xenia mainline/Canary/Edge, forks). Our only tiling code is the negative viewport origin fix (`draw_util.cc:52,467`).

## Options

1. **Merge (TILE-1 proper).** On tile A:
   - render with window offset 0 and scissor = union, into a host-only 1280-wide render target (not mapped to EDRAM);
   - resolve the union rectangle to each destination;
   - skip tile B's packets, keeping their non-draw side effects. Tile B has none beyond its predicated constants and resolves, which tile A's merged resolves cover.

   Saves tile B's vertex work and pass overhead. Pixel work is unchanged. Hard part: the RT cache's EDRAM ownership model (pitch 640 surfaces, EDRAM transfers, later passes that may read the EDRAM tile B left behind).
2. **Real extents.** Write each draw's true screen bounding box in `EVENT_WRITE_EXT`, so D3D culls draws that miss tile B. That is generic and keeps the EDRAM model, but the bbox needs vertex work on the CPU or a mid-frame GPU readback (stall). The saving is only the draws fully on the left half. Likely small for terrain and sky, larger for HUD and small objects.
3. **Vertex output reuse** (transform feedback across tiles). The window offset is applied in the VS (NDC offset), so it would need a post-VS fixup. More complex than 1 and needs an extension that is optional on other drivers.

## Phase 1: bound experiment (no approval needed, wrong output on purpose)

`xdroid_tile_bound` (Vulkan section, bench only):
- 1 = skip every draw of tile B and later. This is the most any merge can save.
- 2 = tile B with an empty scissor. Vertex work only, so B(2) − B(1) ≈ the VS cost that a merge saves; B(base) − B(2) ≈ the pixel cost that moves into tile A.

Bench:
```
tools/device/benchab.sh ... flight.xtr: base / xdroid_tile_bound=1 / =2 (A/B/A), + cine.xtr
```
Decision:
- Merge gain ≈ base − tile2-pixels-moved; go to Phase 2 when the flight saving is ≥ 2.5 ms (Amdahl ≥ 1.1x of 25.7 ms).
- If the bound shows the tile B pass is mostly pixel work, kill option 1 and look at option 2's HUD/object share.

## Phase 2 sketch (after approval)

- Detection: generic, with no title ID.
  - A frame segment with `bin_select != all ones` whose second select replays the same IB address/size list as the first.
  - Per-tile register deltas are limited to {WINDOW_OFFSET, WINDOW_SCISSOR, SCREEN_SCISSOR, COPY_DEST_BASE/PITCH}. Anything else refuses the merge for that frame.
  - Tile A must not contain memexport or occlusion queries (counted once vs per tile).
- RT: a host RT keyed by the union extent, not backed by EDRAM. Resolves from it go through the direct-resolve path into each tile's destination rectangle.
- The fallback is today's path, per frame.
- Red team before code (`xdroid-redteam`). Checks:
  - lenses: EDRAM reads after the tiles; the D3D CPU callback still running.
  - a missing tile-B CPU wait.
  - frame hashes on all 3 traces (flight, hangar, cine) must be byte-identical.
