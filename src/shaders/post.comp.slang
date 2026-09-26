/*
 * Copyright (c) 2023-2026, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-FileCopyrightText: Copyright (c) 2023-2026, NVIDIA CORPORATION.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "shaderio.h"

// clang-format off
[[vk::binding(BINDING_FRAME_INFO_UBO, 0)]]  ConstantBuffer<FrameInfo> frameInfo;
#if STEREO_MULTIVIEW_POST
[[vk::binding(POST_BINDING_MAIN_IMAGE, 0)]] RWTexture2DArray<float4>  mainColorImage;
[[vk::binding(POST_BINDING_AUX1_IMAGE, 0)]] RWTexture2DArray<float4>  aux1ColorImage;
[[vk::binding(POST_BINDING_PREVIEW_IMAGE, 0)]] RWTexture2DArray<float4> previewColorImage;
#else
[[vk::binding(POST_BINDING_MAIN_IMAGE, 0)]] RWTexture2D<float4>       mainColorImage;
[[vk::binding(POST_BINDING_AUX1_IMAGE, 0)]] RWTexture2D<float4>       aux1ColorImage;
[[vk::binding(POST_BINDING_PREVIEW_IMAGE, 0)]] RWTexture2D<float4>    previewColorImage;
#endif
// clang-format on

static const float kEpsilon = 1.0e-5F;

float getCurvedScreenTotalLength()
{
  const float3 sectionLength = max(frameInfo.curvedScreenSectionLength, float3(0.0));
  return max(sectionLength.x + sectionLength.y + sectionLength.z, kEpsilon);
}

float3 getCurvedScreenPoint(float x, float s)
{
  const float3 sectionLength = max(frameInfo.curvedScreenSectionLength, float3(0.0));
  const float  section0      = sectionLength.x;
  const float  section1      = sectionLength.y;
  const float  radius        = max(frameInfo.curvedScreenRadius, kEpsilon);
  const float  bottomHeight  = frameInfo.curvedScreenBottomHeight;
  const float  distance      = frameInfo.curvedScreenToScreenDistance;
  const float3 offset        = frameInfo.curvedScreenOffset;
  const float  totalLength   = getCurvedScreenTotalLength();
  const float  clampedS      = clamp(s, 0.0, totalLength);

  float3 point;
  point.x = offset.x + x;

  if(clampedS <= section0)
  {
    point.y = bottomHeight - offset.y;
    point.z = radius + section0 - distance - clampedS - offset.z;
  }
  else if(clampedS <= section0 + section1)
  {
    const float theta = (clampedS - section0) / radius;
    point.y = bottomHeight + radius * (1.0 - cos(theta)) - offset.y;
    point.z = radius - distance - radius * sin(theta) - offset.z;
  }
  else
  {
    point.y = bottomHeight + radius + (clampedS - section0 - section1) - offset.y;
    point.z = -distance - offset.z;
  }

  return point;
}

float3 getCurvedScreenEye(uint eyeLayer)
{
#if STEREO_MULTIVIEW_POST
  if(frameInfo.stereoEnabled != 0)
    return frameInfo.stereoCurvedScreenHeadPosition[min(eyeLayer, 1u)];
#endif
  return frameInfo.curvedScreenHeadPosition;
}

bool computeVirtualPlaneUv(float3 screenPoint, uint eyeLayer, out float2 uv)
{
  const float width       = max(frameInfo.curvedScreenWidth, kEpsilon);
  const float totalLength = getCurvedScreenTotalLength();

  const float3 bottomLeft  = getCurvedScreenPoint(-0.5 * width, 0.0);
  const float3 bottomRight = getCurvedScreenPoint(0.5 * width, 0.0);
  const float3 topRight    = getCurvedScreenPoint(0.5 * width, totalLength);
  const float3 topLeft     = getCurvedScreenPoint(-0.5 * width, totalLength);

  const float3 origin = 0.25 * (bottomLeft + bottomRight + topRight + topLeft);
  const float3 xAxis  = normalize(bottomRight - bottomLeft);
  const float3 yAxis  = normalize(topLeft - bottomLeft);
  const float3 zAxis  = normalize(cross(xAxis, yAxis));

  const float3 eyePoint = getCurvedScreenEye(eyeLayer);
  const float3 eyeLocal = float3(dot(eyePoint - origin, xAxis),
                                 dot(eyePoint - origin, yAxis),
                                 dot(eyePoint - origin, zAxis));
  const float3 targetLocal = float3(dot(screenPoint - origin, xAxis),
                                    dot(screenPoint - origin, yAxis),
                                    dot(screenPoint - origin, zAxis));
  const float3 rayLocal = targetLocal - eyeLocal;

  if(abs(rayLocal.z) < kEpsilon)
  {
    uv = float2(0.0);
    return false;
  }

  const float t = -eyeLocal.z / rayLocal.z;
  if(t < 0.0)
  {
    uv = float2(0.0);
    return false;
  }

  const float3 hitLocal    = eyeLocal + t * rayLocal;
  const float  planeWidth  = max(length(bottomRight - bottomLeft), kEpsilon);
  const float  planeHeight = max(length(topLeft - bottomLeft), kEpsilon);

  uv = float2((hitLocal.x + 0.5 * planeWidth) / planeWidth,
              (hitLocal.y + 0.5 * planeHeight) / planeHeight);

  return uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0;
}

float4 readAuxColor(int2 pixel, uint eyeLayer)
{
#if STEREO_MULTIVIEW_POST
  return aux1ColorImage[int3(pixel, int(eyeLayer))];
#else
  return aux1ColorImage[pixel];
#endif
}

float4 readMainColor(int2 pixel, uint eyeLayer)
{
#if STEREO_MULTIVIEW_POST
  return mainColorImage[int3(pixel, int(eyeLayer))];
#else
  return mainColorImage[pixel];
#endif
}

void writeMainColor(int2 pixel, uint eyeLayer, float4 color)
{
#if STEREO_MULTIVIEW_POST
  mainColorImage[int3(pixel, int(eyeLayer))] = color;
#else
  mainColorImage[pixel] = color;
#endif
}

void writePreviewColor(int2 pixel, uint eyeLayer, float4 color)
{
#if STEREO_MULTIVIEW_POST
  previewColorImage[int3(pixel, int(eyeLayer))] = color;
#else
  previewColorImage[pixel] = color;
#endif
}

float4 sampleAuxColor(float2 planeUv, uint eyeLayer)
{
  const int2 viewport = int2(frameInfo.viewport);
  const int2 maxPixel = max(viewport - int2(1, 1), int2(0, 0));

  // planeUv.y is bottom-up like OSG texture coordinates; storage image rows are top-down.
  const float2 imageUv = clamp(float2(planeUv.x, 1.0 - planeUv.y), float2(0.0), float2(1.0));
  const float2 source  = imageUv * float2(maxPixel);
  const int2   p0      = int2(floor(source));
  const int2   p1      = min(p0 + int2(1, 1), maxPixel);
  const float2 fracUv  = frac(source);

  const float4 c00 = readAuxColor(p0, eyeLayer);
  const float4 c10 = readAuxColor(int2(p1.x, p0.y), eyeLayer);
  const float4 c01 = readAuxColor(int2(p0.x, p1.y), eyeLayer);
  const float4 c11 = readAuxColor(p1, eyeLayer);

  return lerp(lerp(c00, c10, fracUv.x), lerp(c01, c11, fracUv.x), fracUv.y);
}

float4 warpCurvedScreenPixel(int2 pixel, uint eyeLayer)
{
  const float2 pixelCenter = float2(pixel) + 0.5;
  const float  width       = max(frameInfo.curvedScreenWidth, kEpsilon);
  const float  totalLength = getCurvedScreenTotalLength();
  const float  x           = (pixelCenter.x / frameInfo.viewport.x - 0.5) * width;
  const float  s           = (1.0 - pixelCenter.y / frameInfo.viewport.y) * totalLength;
  const float3 screenPoint = getCurvedScreenPoint(x, s);

  float2 planeUv;
  if(!computeVirtualPlaneUv(screenPoint, eyeLayer, planeUv))
    return float4(0.0, 0.0, 0.0, 1.0);

  return sampleAuxColor(planeUv, eyeLayer);
}

float4 computeOutputColor(int2 pixel, uint eyeLayer)
{
  if(frameInfo.curvedScreenWarpEnabled != 0)
  {
    float4 finalColor = warpCurvedScreenPixel(pixel, eyeLayer);

    if(frameInfo.frameSampleId > 0)
    {
      const float a = 1.0F / float(frameInfo.frameSampleId + 1);
      finalColor    = lerp(readMainColor(pixel, eyeLayer), finalColor, a);
    }

    return finalColor;
  }

  if(frameInfo.frameSampleId > 0)
  {
    const float  a         = 1.0F / float(frameInfo.frameSampleId + 1);
    const float4 mainColor = readMainColor(pixel, eyeLayer);
    const float4 aux1Color = readAuxColor(pixel, eyeLayer);
    return lerp(mainColor, aux1Color, a);
  }

  return readMainColor(pixel, eyeLayer);
}

float3 mapStereoPresentationColor(float3 color)
{
  color = max(color, float3(0.0));

  const float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
  if(luminance <= kEpsilon)
    return float3(0.0);

  const float mappedLuminance = luminance / (1.0 + luminance);
  return saturate(color * (mappedLuminance / luminance));
}

float4 makeStereoPresentationColor(float4 color)
{
  return float4(mapStereoPresentationColor(color.rgb), 1.0);
}

[numthreads(32, 32, 1)]
[shader("compute")]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
  const int2 pixel = int2(dispatchThreadID.xy);
  const uint eyeLayer = dispatchThreadID.z;

  if(pixel.x >= int(frameInfo.viewport.x) || pixel.y >= int(frameInfo.viewport.y))
    return;

  const float4 finalColor = computeOutputColor(pixel, eyeLayer);

  if(frameInfo.curvedScreenWarpEnabled != 0 || frameInfo.frameSampleId > 0)
    writeMainColor(pixel, eyeLayer, finalColor);

#if STEREO_MULTIVIEW_POST
  if(frameInfo.stereoEnabled != 0)
  {
    writePreviewColor(pixel, eyeLayer, makeStereoPresentationColor(finalColor));
  }
#endif
}
