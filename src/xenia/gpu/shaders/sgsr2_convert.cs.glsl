/**
 * Ported from Snapdragon Game Super Resolution 2 (sgsr/v2, 2-pass compute),
 * commit d926f074bcb9d714e179f1ce0fcb9ee2eeb5074e.
 * Copyright (c) 2024, Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause (docs/licenses/snapdragon-gsr-LICENSE.txt)
 * Xenia changes: one descriptor set shared by both passes, velocity buffer
 * dropped (motion always comes from clipToPrevClip), reverse-Z depth, output
 * written as rgb10_a2 for the guest output.
 */
#version 460

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(set = 0, binding = 0, std140) uniform Params {
  uvec2 renderSize;
  uvec2 displaySize;
  vec2 renderSizeRcp;
  vec2 displaySizeRcp;
  vec2 jitterOffset;
  vec4 clipToPrevClip[4];
  float preExposure;
  float cameraFovAngleHor;
  float cameraNear;
  float MinLerpContribution;
  uint sameCameraFrmNum;
  uint reset;
} params;
layout(set = 0, binding = 1) uniform sampler2D InputColor;
layout(set = 0, binding = 2) uniform sampler2D InputDepth;
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D MotionDepthClipAlphaBuffer;
layout(set = 0, binding = 4, r32ui) uniform writeonly uimage2D YCoCgColor;

void main()
{
    mediump float Exposure_co_rcp = params.preExposure;
    vec2 ViewportSizeInverse = params.renderSizeRcp.xy;
    uvec2 InputPos = gl_GlobalInvocationID.xy;

    vec2 gatherCoord = vec2(gl_GlobalInvocationID.xy) * ViewportSizeInverse;
    vec2 ViewportUV = gatherCoord + vec2(0.5) * ViewportSizeInverse;

    //derived from ffx_fsr2_reconstruct_dilated_velocity_and_previous_depth.h
    //FindNearestDepth

    vec4 topleft = textureGather(InputDepth, gatherCoord, 0);
    vec2 v10 = vec2(ViewportSizeInverse.x*2.0, 0.0);
    vec4 topRight = textureGather(InputDepth,(gatherCoord+v10), 0);
    vec2 v12 = vec2(0.0, ViewportSizeInverse.y*2.0);
	vec4 bottomLeft = textureGather(InputDepth,(gatherCoord+v12), 0);
	vec2 v14 = vec2(ViewportSizeInverse.x*2.0, ViewportSizeInverse.y*2.0);
	vec4 bottomRight = textureGather(InputDepth,(gatherCoord+v14), 0);
	float maxC = max(max(max(topleft.y,topRight.x),bottomLeft.z),bottomRight.w);
	float topleft4 = max(max(max(topleft.y,topleft.x),topleft.z),topleft.w);
	float nearestDepth = max(bottomLeft.w,max(max(maxC,topleft4),topRight.w));

    float depthclip = 0.0;
    if (maxC > 1.0e-05f)
    {
        float topRight4 = max(max(max(topRight.y,topRight.x),topRight.z),topRight.w);
        float bottomLeft4 = max(max(max(bottomLeft.y,bottomLeft.x),bottomLeft.z),bottomLeft.w);
        float bottomRight4 = max(max(max(bottomRight.y,bottomRight.x),bottomRight.z),bottomRight.w);

        float Wdepth = 0.0;
        float Ksep = 1.37e-05f;
        float Kfov = params.cameraFovAngleHor;
        float diagonal_length = length(vec2(params.renderSize));
        float Ksep_Kfov_diagonal = Ksep * Kfov * diagonal_length;

		float Depthsep = Ksep_Kfov_diagonal * maxC;
		float EPSILON = 1.19e-07f;
		Wdepth += clamp((Depthsep / (abs(maxC - topleft4) + EPSILON)), 0.0, 1.0);
		Wdepth += clamp((Depthsep / (abs(maxC - topRight4) + EPSILON)), 0.0, 1.0);
		Wdepth += clamp((Depthsep / (abs(maxC - bottomLeft4) + EPSILON)), 0.0, 1.0);
		Wdepth += clamp((Depthsep / (abs(maxC - bottomRight4) + EPSILON)), 0.0, 1.0);
        depthclip = clamp(1.0f - Wdepth*0.25, 0.0, 1.0);
    }

    //refer to ue/fsr2 PostProcessFFX_FSR2ConvertVelocity.usf, and using nearest depth for dilated motion

    vec2 ScreenPos = vec2(2.0f * ViewportUV.x - 1.0f, 1.0f - 2.0f * ViewportUV.y);
    vec3 Position = vec3(ScreenPos, nearestDepth);
    vec4 PreClip = params.clipToPrevClip[3] + ((params.clipToPrevClip[2] * Position.z) + ((params.clipToPrevClip[1] * ScreenPos.y) + (params.clipToPrevClip[0] * ScreenPos.x)));
    vec2 motion = Position.xy - PreClip.xy / PreClip.w;

    ////////////compute luma
    mediump vec3 Colorrgb = texelFetch(InputColor, ivec2(InputPos), 0).xyz;

    ///simple tonemap
    float ColorMax = max(max(Colorrgb.x, Colorrgb.y), Colorrgb.z) + Exposure_co_rcp;
    Colorrgb /= vec3(ColorMax);

    vec3 Colorycocg;
    Colorycocg.x = 0.25 * (Colorrgb.x + 2.0 * Colorrgb.y + Colorrgb.z);
    Colorycocg.y = clamp(0.5 * Colorrgb.x + 0.5 - 0.5 * Colorrgb.z, 0.0, 1.0);
    Colorycocg.z = clamp(Colorycocg.x + Colorycocg.y - Colorrgb.x, 0.0, 1.0);

    //now color YCoCG all in the range of [0,1]
    uint x11 = uint(Colorycocg.x * 2047.5);
    uint y11 = uint(Colorycocg.y * 2047.5);
    uint z10 = uint(Colorycocg.z * 1023.5);

    imageStore(YCoCgColor, ivec2(gl_GlobalInvocationID.xy), uvec4(((x11 << 21u) | (y11 << 10u)) | z10));

    mediump vec4 v29 = vec4(motion, depthclip, ColorMax);
    imageStore(MotionDepthClipAlphaBuffer, ivec2(gl_GlobalInvocationID.xy), v29);
}