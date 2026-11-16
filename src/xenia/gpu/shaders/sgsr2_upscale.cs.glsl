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

float FastLanczos(float base)
{
	float y = base - 1.0f;
	float y2 = y * y;
	float y_temp = 0.75f * y + y2;
	return y_temp * y2;
}

vec3 DecodeColor(uint sample32)
{
	uint x11 = sample32 >> 21u;
	uint y11 = sample32 & (2047u << 10u);
	uint z10 = sample32 & 1023u;
	vec3 samplecolor;
	samplecolor.x = (float(x11) * (1.0 / 2047.5));
	samplecolor.y = (float(y11) * (4.76953602e-7)) - 0.5;
	samplecolor.z = (float(z10) * (1.0 / 1023.5)) - 0.5;

	return samplecolor;
}

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
  uint hudMask;
} params;
layout(set = 0, binding = 5) uniform sampler2D MotionDepthClipAlphaBuffer;
layout(set = 0, binding = 6) uniform usampler2D YCoCgColor;
layout(set = 0, binding = 7) uniform sampler2D PrevHistoryOutput;
layout(set = 0, binding = 8, rgb10_a2) uniform writeonly image2D SceneColorOutput;
layout(set = 0, binding = 9, rgba16f) uniform writeonly image2D HistoryOutput;

void main()
{
    float Biasmax_viewportXScale = min(float(params.displaySize.x) / float(params.renderSize.x), 1.99);  //Biasmax_viewportXScale
    float scalefactor = min(20.0, pow((float(params.displaySize.x) / float(params.renderSize.x)) * (float(params.displaySize.y) / float(params.renderSize.y)), 3.0));
    float f2 = params.preExposure;            //1.0;   //preExposure
    vec2 HistoryInfoViewportSizeInverse = params.displaySizeRcp;
    vec2 HistoryInfoViewportSize = vec2(params.displaySize);
    vec2 InputJitter = params.jitterOffset;
    vec2 InputInfoViewportSize = vec2(params.renderSize);
    vec2 Hruv = (vec2(gl_GlobalInvocationID.xy) + vec2(0.5)) * HistoryInfoViewportSizeInverse;
    vec2 Jitteruv;
    Jitteruv.x = clamp(Hruv.x + (InputJitter.x * params.renderSizeRcp.x), 0.0, 1.0);
    Jitteruv.y = clamp(Hruv.y + (InputJitter.y * params.renderSizeRcp.y), 0.0, 1.0);

    ivec2 InputPos = ivec2(Jitteruv * InputInfoViewportSize);
    vec4 mda = textureLod(MotionDepthClipAlphaBuffer, Jitteruv, 0.0).xyzw;
    vec2 Motion = mda.xy;

    ///ScreenPosToViewportScale&Bias
    vec2 PrevUV;
    PrevUV.x = clamp(-0.5 * Motion.x + Hruv.x, 0.0, 1.0);
    PrevUV.y = clamp(0.5 * Motion.y + Hruv.y, 0.0, 1.0);

    float depthfactor = mda.z;

    vec4 History = textureLod(PrevHistoryOutput, PrevUV, 0.0);
    vec3 HistoryColor = History.xyz;
    float Historyw = History.w;
    float Wfactor = clamp(abs(Historyw), 0.0, 1.0);

    /////upsample and compute box
    vec4 Upsampledcw = vec4(0.0);
    float kernelfactor = clamp(Wfactor + float(params.reset), 0.0, 1.0);
    float biasmax = Biasmax_viewportXScale - Biasmax_viewportXScale * kernelfactor;
    float biasmin = max(1.0f, 0.3 + 0.3 * biasmax);
    float biasfactor = max(0.25f * depthfactor, kernelfactor);
    float kernelbias = mix(biasmax, biasmin, biasfactor);
    float motion_viewport_len = length(Motion * HistoryInfoViewportSize);
    float curvebias = mix(-2.0, -3.0, clamp(motion_viewport_len * 0.02, 0.0, 1.0));

    vec3 rectboxcenter = vec3(0.0);
    vec3 rectboxvar = vec3(0.0);
    float rectboxweight = 0.0;
    vec2 srcpos = vec2(InputPos) + vec2(0.5) - InputJitter;
    vec2 srcOutputPos = Hruv * InputInfoViewportSize;

    kernelbias *= 0.5f;
    float kernelbias2 = kernelbias * kernelbias;
    vec2 srcpos_srcOutputPos = srcpos - srcOutputPos;

    ivec2 InputPosBtmRight = ivec2(1) + InputPos;
    vec2 gatherCoord = vec2(InputPos) * params.renderSizeRcp;
    uint btmRight = texelFetch(YCoCgColor, InputPosBtmRight, 0).x;
    uvec4 topleft = textureGather(YCoCgColor, gatherCoord);
    uvec2 topRight;
    uvec2 bottomLeft;

    uint sameCameraFrmNum = params.sameCameraFrmNum;

	if (sameCameraFrmNum!=0u)
	{
		topRight = textureGather(YCoCgColor, gatherCoord + vec2(params.renderSizeRcp.x, 0.0)).yz;
		bottomLeft = textureGather(YCoCgColor, gatherCoord + vec2(0.0, params.renderSizeRcp.y)).xy;
	}
	else
	{
		uvec2 btmRight = textureGather(YCoCgColor, gatherCoord + vec2(params.renderSizeRcp.x, params.renderSizeRcp.y)).xz;
		bottomLeft.y = btmRight.x;
		topRight.x = btmRight.y;
	}

    vec3 rectboxmin;
    vec3 rectboxmax;
    {
        vec3 samplecolor = DecodeColor(bottomLeft.y);
        vec2 baseoffset = srcpos_srcOutputPos + vec2(0.0, 1.0);
        float baseoffset_dot = dot(baseoffset, baseoffset);
        float base = clamp(baseoffset_dot * kernelbias2, 0.0f, 1.0f);
        float weight = FastLanczos(base);
        Upsampledcw += vec4(samplecolor * weight, weight);
        float boxweight = exp(baseoffset_dot * curvebias);
        rectboxmin = samplecolor;
        rectboxmax = samplecolor;
        vec3 wsample = samplecolor * boxweight;
        rectboxcenter += wsample;
        rectboxvar += (samplecolor * wsample);
        rectboxweight += boxweight;
    }
    {
        vec3 samplecolor = DecodeColor(topRight.x);
        vec2 baseoffset = srcpos_srcOutputPos + vec2(1.0, 0.0);
        float baseoffset_dot = dot(baseoffset, baseoffset);
        float base = clamp(baseoffset_dot * kernelbias2, 0.0f, 1.0f);
        float weight = FastLanczos(base);
        Upsampledcw += vec4(samplecolor * weight, weight);
        float boxweight = exp(baseoffset_dot * curvebias);
        rectboxmin = min(rectboxmin, samplecolor);
        rectboxmax = max(rectboxmax, samplecolor);
        vec3 wsample = samplecolor * boxweight;
        rectboxcenter += wsample;
        rectboxvar += (samplecolor * wsample);
        rectboxweight += boxweight;
    }
    {
        vec3 samplecolor = DecodeColor(topleft.x);
        vec2 baseoffset = srcpos_srcOutputPos + vec2(-1.0, 0.0);
        float baseoffset_dot = dot(baseoffset, baseoffset);
        float base = clamp(baseoffset_dot * kernelbias2, 0.0f, 1.0f);
        float weight = FastLanczos(base);
        Upsampledcw += vec4(samplecolor * weight, weight);
        float boxweight = exp(baseoffset_dot * curvebias);
        rectboxmin = min(rectboxmin, samplecolor);
        rectboxmax = max(rectboxmax, samplecolor);
        vec3 wsample = samplecolor * boxweight;
        rectboxcenter += wsample;
        rectboxvar += (samplecolor * wsample);
        rectboxweight += boxweight;
    }
    {
        vec3 samplecolor = DecodeColor(topleft.y);
        vec2 baseoffset = srcpos_srcOutputPos;
        float baseoffset_dot = dot(baseoffset, baseoffset);
        float base = clamp(baseoffset_dot * kernelbias2, 0.0f, 1.0f);
        float weight = FastLanczos(base);
        Upsampledcw += vec4(samplecolor * weight, weight);
        float boxweight = exp(baseoffset_dot * curvebias);
        rectboxmin = min(rectboxmin, samplecolor);
        rectboxmax = max(rectboxmax, samplecolor);
        vec3 wsample = samplecolor * boxweight;
        rectboxcenter += wsample;
        rectboxvar += (samplecolor * wsample);
        rectboxweight += boxweight;
    }
    {
        vec3 samplecolor = DecodeColor(topleft.z);
        vec2 baseoffset = srcpos_srcOutputPos + vec2(0.0, -1.0);
        float baseoffset_dot = dot(baseoffset, baseoffset);
        float base = clamp(baseoffset_dot * kernelbias2, 0.0f, 1.0f);
        float weight = FastLanczos(base);
        Upsampledcw += vec4(samplecolor * weight, weight);
        float boxweight = exp(baseoffset_dot * curvebias);
        rectboxmin = min(rectboxmin, samplecolor);
        rectboxmax = max(rectboxmax, samplecolor);
        vec3 wsample = samplecolor * boxweight;
        rectboxcenter += wsample;
        rectboxvar += (samplecolor * wsample);
        rectboxweight += boxweight;
    }

    if (sameCameraFrmNum!=0u)
    {
        {
            vec3 samplecolor = DecodeColor(btmRight);
            vec2 baseoffset = srcpos_srcOutputPos + vec2(1.0, 1.0);
            float baseoffset_dot = dot(baseoffset, baseoffset);
            float base = clamp(baseoffset_dot * kernelbias2, 0.0, 1.0);
            float weight = FastLanczos(base);
            Upsampledcw += vec4(samplecolor * weight, weight);
            float boxweight = exp(baseoffset_dot * curvebias);
            rectboxmin = min(rectboxmin, samplecolor);
            rectboxmax = max(rectboxmax, samplecolor);
            vec3 wsample = samplecolor * boxweight;
            rectboxcenter += wsample;
            rectboxvar += (samplecolor * wsample);
            rectboxweight += boxweight;
        }
        {
            vec3 samplecolor = DecodeColor(bottomLeft.x);
            vec2 baseoffset = srcpos_srcOutputPos + vec2(-1.0, 1.0);
            float baseoffset_dot = dot(baseoffset, baseoffset);
            float base = clamp(baseoffset_dot * kernelbias2, 0.0f, 1.0f);
            float weight = FastLanczos(base);
            Upsampledcw += vec4(samplecolor * weight, weight);
            float boxweight = exp(baseoffset_dot * curvebias);
            rectboxmin = min(rectboxmin, samplecolor);
            rectboxmax = max(rectboxmax, samplecolor);
            vec3 wsample = samplecolor * boxweight;
            rectboxcenter += wsample;
            rectboxvar += (samplecolor * wsample);
            rectboxweight += boxweight;
        }
        {
            vec3 samplecolor = DecodeColor(topRight.y);
            vec2 baseoffset = srcpos_srcOutputPos + vec2(1.0, -1.0);
            float baseoffset_dot = dot(baseoffset, baseoffset);
            float base = clamp(baseoffset_dot * kernelbias2, 0.0f, 1.0f);
            float weight = FastLanczos(base);
            Upsampledcw += vec4(samplecolor * weight, weight);
            float boxweight = exp(baseoffset_dot * curvebias);
            rectboxmin = min(rectboxmin, samplecolor);
            rectboxmax = max(rectboxmax, samplecolor);
            vec3 wsample = samplecolor * boxweight;
            rectboxcenter += wsample;
            rectboxvar += (samplecolor * wsample);
            rectboxweight += boxweight;
        }

        {
            vec3 samplecolor = DecodeColor(topleft.w);
            vec2 baseoffset = srcpos_srcOutputPos + vec2(-1.0, -1.0);
            float baseoffset_dot = dot(baseoffset, baseoffset);
            float base = clamp(baseoffset_dot * kernelbias2, 0.0f, 1.0f);
            float weight = FastLanczos(base);
            Upsampledcw += vec4(samplecolor * weight, weight);
            float boxweight = exp(baseoffset_dot * curvebias);
            rectboxmin = min(rectboxmin, samplecolor);
            rectboxmax = max(rectboxmax, samplecolor);
            vec3 wsample = samplecolor * boxweight;
            rectboxcenter += wsample;
            rectboxvar += (samplecolor * wsample);
            rectboxweight += boxweight;
        }
    }

    rectboxweight = 1.0 / rectboxweight;
    rectboxcenter *= rectboxweight;
    rectboxvar *= rectboxweight;
    rectboxvar = sqrt(abs(rectboxvar - rectboxcenter * rectboxcenter));

    Upsampledcw.xyz =  clamp(Upsampledcw.xyz / Upsampledcw.w, rectboxmin-vec3(0.05), rectboxmax+vec3(0.05));
    Upsampledcw.w = Upsampledcw.w * (1.0f / 3.0f) ;

	float OneMinusWfactor = 1.0f - Wfactor;

    float baseupdate = OneMinusWfactor - OneMinusWfactor * depthfactor;
    baseupdate = min(baseupdate, mix(baseupdate, Upsampledcw.w *10.0f, clamp(10.0f* motion_viewport_len, 0.0, 1.0)));
    baseupdate = min(baseupdate, mix(baseupdate, Upsampledcw.w, clamp(motion_viewport_len *0.05f, 0.0, 1.0)));
    float basealpha = baseupdate;

    const float EPSILON = 1.192e-07f;
    float boxscale = max(depthfactor, clamp(motion_viewport_len * 0.05f, 0.0, 1.0));
    float boxsize = mix(scalefactor, 1.0f, boxscale);
    vec3 sboxvar = rectboxvar * boxsize;
    vec3 boxmin = rectboxcenter - sboxvar;
    vec3 boxmax = rectboxcenter + sboxvar;
    rectboxmax = min(rectboxmax, boxmax);
    rectboxmin = max(rectboxmin, boxmin);

    // HUD/UI mask: screen-static overlay pixels carry the sky's camera motion;
    // when the same-pixel history matches the current color much better than
    // the reprojected one, the pixel is static, so use that history with zero motion.
    bool hudStatic = false;
    if (params.hudMask != 0u && (abs(mda.x) + abs(mda.y)) > 0.000001) {
        vec3 StaticHistory = textureLod(PrevHistoryOutput, Hruv, 0.0).xyz;
        vec3 movedErr = HistoryColor - Upsampledcw.xyz;
        vec3 staticErr = StaticHistory - Upsampledcw.xyz;
        bool staticFits = dot(staticErr, staticErr) < 0.25 * dot(movedErr, movedErr);
        if (staticFits) {
            HistoryColor = StaticHistory;
            hudStatic = true;
        }
    }
    vec3 clampedcolor = clamp(HistoryColor, rectboxmin, rectboxmax);
    float startLerpValue = params.MinLerpContribution; //params.MinLerpContribution; //MinLerpContribution;
    if (!hudStatic && (abs(mda.x) + abs(mda.y)) > 0.000001) startLerpValue = 0.0;
	float lerpcontribution = (any(greaterThan(rectboxmin, HistoryColor)) || any(greaterThan(HistoryColor, rectboxmax))) ? startLerpValue : 1.0f;

	HistoryColor = mix(clampedcolor, HistoryColor, clamp(lerpcontribution, 0.0, 1.0));
    float basemin = min(basealpha, 0.1f);
    basealpha = mix(basemin, basealpha, clamp(lerpcontribution, 0.0, 1.0));

    ////blend color
    float alphasum = max(EPSILON, basealpha + Upsampledcw.w);
    float alpha = clamp(Upsampledcw.w / alphasum + float(params.reset), 0.0, 1.0);
    Upsampledcw.xyz = mix(HistoryColor, Upsampledcw.xyz, alpha);
    
    imageStore(HistoryOutput, ivec2(gl_GlobalInvocationID.xy), vec4(Upsampledcw.xyz, Wfactor));

    ////ycocg to rgb
    float x_z = Upsampledcw.x - Upsampledcw.z;
    Upsampledcw.xyz = vec3(
        clamp(x_z + Upsampledcw.y, 0.0, 1.0),
        clamp(Upsampledcw.x + Upsampledcw.z, 0.0, 1.0),
        clamp(x_z - Upsampledcw.y, 0.0, 1.0));

    float compMax = max(Upsampledcw.x, Upsampledcw.y);
    compMax = clamp(max(compMax, Upsampledcw.z), 0.0f, 1.0f);
    float scale = params.preExposure /  ((1.0f + 600.0f / 65504.0f) - compMax);

    Upsampledcw.xyz = Upsampledcw.xyz * scale;
    imageStore(SceneColorOutput, ivec2(gl_GlobalInvocationID.xy), vec4(Upsampledcw.xyz, 1.0));

}