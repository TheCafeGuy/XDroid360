/**
 * XDroid360: copies sample 0 of the multisampled host depth render target into
 * the R32F depth image read by sgsr2_convert (same descriptor set).
 */
#version 460

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(set = 0, binding = 10) uniform sampler2DMS xe_src_depth;
layout(set = 0, binding = 11, r32f) uniform writeonly image2D xe_dst_depth;
layout(push_constant) uniform Size {
  uvec2 size;
} xe_size;

void main() {
  uvec2 id = gl_GlobalInvocationID.xy;
  if (any(greaterThanEqual(id, xe_size.size))) {
    return;
  }
  imageStore(xe_dst_depth, ivec2(id), vec4(texelFetch(xe_src_depth, ivec2(id), 0).r));
}
