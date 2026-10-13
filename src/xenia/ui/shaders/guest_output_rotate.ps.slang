/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Copies the image painted in the logical orientation into a swapchain image
// that has the display's native orientation (pre-rotation), 1:1, so the
// compositor can scan the swapchain image out without rotating it on the GPU.

#include "xesl.xesli"

push_const_begin_xe(b0, space0)
  // 16 used by the vertex shader (GLSL push constant offsets are across
  // stages).
  // Logical width - 1, height - 1.
  block_offset_member_xe(16, c0.x, int2_xe, xe_rotate_source_max)
  // x: quarter turns clockwise of the surface transform (1, 2 or 3).
  block_offset_member_xe(24, c0.z, int2_xe, xe_rotate_quarter_turns)
push_const_end_xe

entry_outputs_begin_xe
  entry_out_target_xe(float4_xe, xe_rotate_color, 0)
entry_outputs_end_stage_inputs_begin_xe
entry_stage_inputs_end_bindings_begin_pixel_xe
  push_const_binding_xe(buffer(0))
  entry_binding_next_xe
  texture_xe(texture_2d_xe, xe_rotate_source, 0, 0, t0, space0, texture(0))
entry_bindings_end_inputs_begin_xe
  entry_in_pixel_coord_xe
entry_inputs_end_code_begin_xe
{
  int2_xe pixel = int2_xe(in_pixel_coord_xe.xy);
  int2_xe source_max = push_const_xe(xe_rotate_source_max);
  int quarter_turns = push_const_xe(xe_rotate_quarter_turns).x;
  // The logical pixel that the transform puts at this native pixel.
  int2_xe source = pixel;
  if (quarter_turns == 1) {
    source = int2_xe(pixel.y, source_max.y - pixel.x);
  } else if (quarter_turns == 2) {
    source = int2_xe(source_max.x - pixel.x, source_max.y - pixel.y);
  } else if (quarter_turns == 3) {
    source = int2_xe(source_max.x - pixel.y, pixel.x);
  }
  out_xe(xe_rotate_color) =
      float4_xe(texel_fetch_2d_xe(xe_rotate_source, source, 0).rgb, 1.0f);
}
entry_code_end_xe
