// Compiled at build time (fxc, cs_5_1) and embedded in the DLL.
cbuffer Constants : register(b0) { uint2 FullSize; uint2 LowSize; uint HasMotion; };
Texture2D<float4>   Src0 : register(t0);   // reduce: native colour   | compose: reduced NR input
Texture2D<float4>   Src1 : register(t1);   // reduce: native motion   | compose: reduced NR output
Texture2D<float4>   Src2 : register(t2);   // compose: native colour
RWTexture2D<float4> OutColor  : register(u0);   // reduce: reduced colour | compose: game output
RWTexture2D<float2> OutMotion : register(u1);
RWTexture2D<float>  OutDepth  : register(u2);
SamplerState Linear : register(s0);

// One bilinear tap at the centre of each 2x2 footprint is the box average at 2:1.
// Motion vectors stay in native pixels (the reduced feature gets a halved scale);
// depth is a flat plane, the network's output does not depend on it at this ratio.
[numthreads(8, 8, 1)]
void Reduce(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= LowSize)) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(LowSize);
    OutColor[id.xy] = Src0.SampleLevel(Linear, uv, 0);
    if (HasMotion) OutMotion[id.xy] = Src1.SampleLevel(Linear, uv, 0).xy;
    OutDepth[id.xy] = 0.5;
}

// output = native + bilinear_up(NR_output - NR_input)
[numthreads(8, 8, 1)]
void Compose(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= FullSize)) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(FullSize);
    float4 native = Src2.Load(int3(id.xy, 0));
    float3 delta = Src1.SampleLevel(Linear, uv, 0).rgb - Src0.SampleLevel(Linear, uv, 0).rgb;
    OutColor[id.xy] = float4(max(native.rgb + delta, 0.0), native.a);
}
