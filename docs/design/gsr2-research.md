# GSR2: Snapdragon Game Super Resolution 2 for XDroid360 (research, no code)

Date: 2026-10-21. Follows docs/design/gsr-oem-coexistence.md (GSR-1 ported, off) and upscaling-fsr.md.

## 1. Source and license

SnapdragonStudios/snapdragon-gsr at `d926f074bcb9d714e179f1ce0fcb9ee2eeb5074e` (same commit GSR-1 came from),
`sgsr/v2/include/{glsl_2_pass_fs,glsl_2_pass_cs,glsl_3_pass_cs}`. BSD-3-Clause, Qualcomm Innovation Center; the
existing `docs/licenses/snapdragon-gsr-LICENSE.txt` covers it, `notices.tsv` needs a GSR 2 row. Port, keep the
header, credit the commit. No contact with upstream.

## 2. What GSR2 needs (2-pass-fs, the variant Qualcomm recommends for mobile)

| Input | Have it? | How we would get it |
|---|---|---|
| Color at render size | Yes | Guest frame, 1280x720 |
| Depth at render size, same frame | No (EDRAM only) | Copy the depth target when the world compositor draw runs (ucode `17e5e4ac3e713245`, already stamped) |
| Motion: velocity texture, or `clipToPrevClip` matrix (camera this frame vs last) | No | Convert pass reprojects depth with the matrix when velocity is empty. Matrix must come from AC6's vertex constants (location unknown; needs a constant dump). Zero matrix = no motion = ghosting in flight, where the whole screen moves |
| Sub-pixel jitter (Halton) of the projection | No | Add jitter to the NDC offset system constant for world draws only, stop at the compositor stamp so the HUD stays still. Changes every frame's image: gpubench frame hashes stop being a quality check |
| HUD / translucent mask | No | HUD is baked into the 720p frame. Mask = pixels that differ between the pre-HUD world copy and the final frame; no history there |

Cost (Qualcomm table, SD8 Gen 3 at max GPU clock, output 1260x2800): 2-pass-fs 0.9-1.1 ms, 2-pass-cs 1.8-2.0,
3-pass-cs 2.0-2.4. Our clocks are demand-driven (lower), so expect ~1-1.5 ms for 720p -> 1080p (inferred).
Today's present at 50% costs 0.35 ms/frame (DONE_LOG 2026-10-16), so GSR2 adds ~+1 ms GPU per frame.

## 3. Where a GPU gain could come from

- GSR2 only saves GPU if the game renders fewer pixels and GSR2 rebuilds them. AC6 renders natively at
  1280x720 with 4x MSAA; Xenia has no sub-1x internal scale (upscaling-fsr.md section 2: architecture change,
  weeks, glitch risk). MSAA-1X (-2.6 ms) was killed for garbage, not for jaggies, so "GSR2 as the AA" doesn't
  revive it.
- At native 720p, GSR2 is a quality feature: temporal AA (shimmer on terrain and wires) plus a sharper
  720 -> 1080/1440 upscale. It costs ~1 ms GPU and energy against the <= 18 ms target (flight ~18.8 ms now).
- GSR-1 (spatial, one pass) already ships off and was never measured on the phone.

## 4. Conflicts to settle with the maintainer

1. Quality rule (no quality loss, 720p floor) vs a lower internal resolution.
2. GPU target <= 18 ms vs +1 ms for GSR2.
3. Phone's own upscale/sharpening (OnePlus game assistant) vs ours: `present_oem_postprocess` exists; GSR2
   without its sharpening when the phone sharpens.
4. Motion source: AC6 camera matrices (AC6 only, best) vs image-based estimate (any game, more GPU, more ghosts).
5. Default on/off and the gate for it.
