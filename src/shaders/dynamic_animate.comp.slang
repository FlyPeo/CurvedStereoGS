/*
 * CurvedStereoGS - dynamic 4DGS animate pass.
 *
 * Evaluates per-splat positions at the current animation time and writes them
 * into the live centers buffer consumed by sorting/rasterization
 * (docs/dynamic-4dgs.md). Phase 1 blends between two keyframe position blocks
 * from the .dgs pool; Phase 2 replaces the lerp with deformation-field
 * inference while keeping the same output contract.
 */

// Positions are packed as three consecutive 32-bit floats in both the .dgs
// stream and SplatSetVk::centersBuffer (12 bytes per splat). A StructuredBuffer
// of float3 has a 16-byte ArrayStride in SPIR-V, so use scalar buffers and
// assemble the triples explicitly to preserve the packed layout.
[[vk::binding(0, 0)]] StructuredBuffer<float>   keyframePool;   // packed xyz values, all frames back to back
[[vk::binding(1, 0)]] RWStructuredBuffer<float> outCenters;     // packed live xyz values

struct AnimatePush
{
    uint  prevFloatOffset; // scalar-float offset of keyframe k in the packed pool
    uint  nextFloatOffset; // scalar-float offset of keyframe k+1
    float alpha;           // blend factor in [0, 1]
    uint  splatCount;      // splats to animate (min of scene and sequence)
};

[[vk::push_constant]] ConstantBuffer<AnimatePush> pushConst;

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint i = tid.x;
    if(i >= pushConst.splatCount)
        return;

    const uint baseA = pushConst.prevFloatOffset + 3 * i;
    const uint baseB = pushConst.nextFloatOffset + 3 * i;
    const uint baseOut = 3 * i;
    outCenters[baseOut + 0] = lerp(keyframePool[baseA + 0], keyframePool[baseB + 0], pushConst.alpha);
    outCenters[baseOut + 1] = lerp(keyframePool[baseA + 1], keyframePool[baseB + 1], pushConst.alpha);
    outCenters[baseOut + 2] = lerp(keyframePool[baseA + 2], keyframePool[baseB + 2], pushConst.alpha);
}
