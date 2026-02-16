// Terrain: vertex displacement from heightmap, same GBuffer output as GeometryPass
#include "LightingUtil.hlsl"
Texture2D gHeightMap : register(t0);
Texture2D gDiffuseMap : register(t1);
Texture2D gNormalMap : register(t2);

SamplerState gsamLinearClamp : register(s3);

cbuffer cbPerObject : register(b0)
{
    float4x4 gWorld;
    float4x4 gInvWorld;
    float4x4 gTexTransform;
    float4x4 gPrevWorld;
};

// Must match PassConstants layout exactly (View, InvView, Proj, InvProj, ViewProj, InvViewProj, EyePosW, ...)
cbuffer cbPass : register(b1)
{
    float4x4 gView;
    float4x4 gInvView;
    float4x4 gProj;
    float4x4 gInvProj;
    float4x4 gViewProj;
    float4x4 gInvViewProj;
    float3 gEyePosW;
    float cbPerObjectPad1;
    float2 gRenderTargetSize;
    float2 gInvRenderTargetSize;
    float gNearZ;
    float gFarZ;
    float gTotalTime;
    float gDeltaTime;
    float4 gAmbientLight;
    float4x4 gViewProjNoJitter;
    float4x4 gPrevViewProjNoJitter;
    float4x4 gPrevViewProj;
    float2 gCurrJitterUV;
    float2 gPrevJitterUV;
    float2 g_padTaa;
    Light gLights[MaxLights];
};

// gHeightScale = Y scale from gWorld (World matrix has scale (sx, heightScale, sz))
struct VertexIn
{
    float3 PosL : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC : TEXCOORD;
    float3 Tan : TANGENT;
};

struct VertexOut
{
    float4 PosH : SV_POSITION;
    float3 PosW : POSITION;
    float3 NormalW : NORMAL;
    float2 TexC : TEXCOORD;
    float3 Tan : TANGENT;
    float4 CurrClip : TEXCOORD1;
    float4 PrevClip : TEXCOORD2;
};

// Helper: sample height (0..1).
float2 AdjustToTexelCenters(float2 uv, uint w, uint h)
{
    float2 dims = float2((float)w, (float)h);
    uv = saturate(uv);
    // Map [0..1] -> [0.5/w .. (w-0.5)/w] to sample texel centers (avoids border artifacts on tile edges).
    return (uv * (dims - 1.0f) + 0.5f) / dims;
}

float SampleHeight(float2 uv)
{
    uint w, h;
    gHeightMap.GetDimensions(w, h);
    float2 uvc = AdjustToTexelCenters(uv, w, h);
    return gHeightMap.SampleLevel(gsamLinearClamp, uvc, 0).r;
}

// Edge morphing: produce heights matching a coarser grid along an edge.
float MorphEdgeV(float uFixed, float v, int step)
{
    const int gridRes = 64; // MUST match terrain mesh cells count in C++.
    int i = (int)round(saturate(v) * gridRes);
    int s = max(step, 1);
    int i0 = (i / s) * s;
    int i1 = min(i0 + s, gridRes);
    float t = (i1 == i0) ? 0.0f : (float)(i - i0) / (float)(i1 - i0);
    float2 uv0 = float2(uFixed, (float)i0 / (float)gridRes);
    float2 uv1 = float2(uFixed, (float)i1 / (float)gridRes);
    return lerp(SampleHeight(uv0), SampleHeight(uv1), t);
}

float MorphEdgeU(float u, float vFixed, int step)
{
    const int gridRes = 64; // MUST match terrain mesh cells count in C++.
    int i = (int)round(saturate(u) * gridRes);
    int s = max(step, 1);
    int i0 = (i / s) * s;
    int i1 = min(i0 + s, gridRes);
    float t = (i1 == i0) ? 0.0f : (float)(i - i0) / (float)(i1 - i0);
    float2 uv0 = float2((float)i0 / (float)gridRes, vFixed);
    float2 uv1 = float2((float)i1 / (float)gridRes, vFixed);
    return lerp(SampleHeight(uv0), SampleHeight(uv1), t);
}

VertexOut VS(VertexIn vin)
{
    VertexOut vout;
    float heightScale = gWorld._22; // Y scale from world matrix
    // Terrain params are packed into gTexTransform by CPU.
    // Access via [row][col] to avoid any confusion with _mn layout.
    float skirtDepthW = gTexTransform[2][2]; // packed from CPU (world units)
    float isSkirtVertex = step(0.5f, abs(vin.NormalL.x)); // 1 for skirt vertices (encoded in mesh)
    // Edge morphing steps (1 = off). These do NOT affect UV transform (we keep gTexTransform identity for UV).
    float stepL = gTexTransform[0][2]; // CPU: texT.m[0][2]
    float stepR = gTexTransform[1][2]; // CPU: texT.m[1][2]
    float stepB = gTexTransform[2][0]; // CPU: texT.m[2][0]
    float stepT = gTexTransform[2][1]; // CPU: texT.m[2][1]

    float2 uvH = vin.TexC;
    float h = SampleHeight(uvH);
    const float eps = 1e-6f;
    // Enable skirts only on edges that actually need seam hiding (neighbor is coarser).
    float needSkirtEdge = 0.0f;
    if (uvH.x <= eps && stepL > 1.5f) needSkirtEdge = 1.0f;
    if (uvH.x >= 1.0f - eps && stepR > 1.5f) needSkirtEdge = 1.0f;
    if (uvH.y <= eps && stepB > 1.5f) needSkirtEdge = 1.0f;
    if (uvH.y >= 1.0f - eps && stepT > 1.5f) needSkirtEdge = 1.0f;

    float useSkirt = isSkirtVertex * needSkirtEdge;

    if (stepL > 1.5f && uvH.x <= eps) h = MorphEdgeV(0.0f, uvH.y, (int)stepL);
    if (stepR > 1.5f && uvH.x >= 1.0f - eps) h = MorphEdgeV(1.0f, uvH.y, (int)stepR);
    if (stepB > 1.5f && uvH.y <= eps) h = MorphEdgeU(uvH.x, 0.0f, (int)stepB);
    if (stepT > 1.5f && uvH.y >= 1.0f - eps) h = MorphEdgeU(uvH.x, 1.0f, (int)stepT);

    float skirtLocal = (heightScale > 1e-6f) ? (skirtDepthW / heightScale) : 0.0f;
    // Center height around 0 so that world translation (node.Bounds.Center.y) matches the AABB.
    float3 posL = float3(vin.PosL.x, (h - 0.5f) - useSkirt * skirtLocal, vin.PosL.z);
    float4 posW = mul(float4(posL, 1.f), gWorld);
    vout.PosW = posW.xyz;
    vout.PosH = mul(posW, gViewProj);

    // Geometric normal from heightmap (used for fallback / velocity stability).
    uint hmW, hmH;
    gHeightMap.GetDimensions(hmW, hmH);
    float2 uv = uvH;
    float2 du = float2(1.f / max(1u, hmW), 0.f);
    float2 dv = float2(0.f, 1.f / max(1u, hmH));
    float hL = SampleHeight(uv - du);
    float hR = SampleHeight(uv + du);
    float hD = SampleHeight(uv - dv);
    float hU = SampleHeight(uv + dv);
    float3 nL = normalize(float3(-(hR - hL) * heightScale, 2.f, -(hU - hD) * heightScale));
    float3x3 invTrans = (float3x3)transpose(gInvWorld);
    vout.NormalW = normalize(mul(nL, invTrans));
    vout.Tan = float3(1, 0, 0);

    vout.TexC = mul(float4(vin.TexC, 0, 1), gTexTransform).xy;
    vout.CurrClip = mul(posW, gViewProjNoJitter);
    float4 prevW = mul(float4(posL, 1.f), gPrevWorld);
    vout.PrevClip = mul(prevW, gPrevViewProjNoJitter);
    return vout;
}

// Same PS output as GeometryPass for GBuffer
struct PSOutput
{
    float4 Albedo : SV_Target0;
    float4 Normal : SV_Target1;
    float4 Position : SV_Target2;
    float2 Velocity : SV_Target3;
};

// Material in same b2 as main geometry pass
cbuffer cbMaterial : register(b2)
{
    float4 gDiffuseAlbedo;
    float3 gFresnelR0;
    float gRoughness;
    float gMetallic;
    float3 _padM;
    float4x4 gMatTransform;
};

PSOutput PS(VertexOut pin)
{
    PSOutput outt;
    float4 diffuseTex = gDiffuseMap.Sample(gsamLinearClamp, pin.TexC);
    outt.Albedo = diffuseTex * gDiffuseAlbedo;
    outt.Albedo.a = gRoughness;
    float useNormalMap = gTexTransform[3][2]; // CPU: texT.m[3][2]
    float3 nW = normalize(pin.NormalW);
    if (useNormalMap > 0.5f)
    {
        float4 ns = gNormalMap.Sample(gsamLinearClamp, pin.TexC);
        float2 xy = ns.xy * 2.0f - 1.0f;
        float z = sqrt(saturate(1.0f - dot(xy, xy)));
        float3 nLocal = normalize(float3(xy.x, z, xy.y)); // map (x,z)->(x,y,z) for terrain plane
        float3x3 invTrans = (float3x3)transpose(gInvWorld);
        nW = normalize(mul(nLocal, invTrans));
    }
    outt.Normal = float4(nW, gMetallic);
    outt.Position = float4(pin.PosW, 1.f);
    float invWc = (abs(pin.CurrClip.w) > 1e-6f) ? (1.f / pin.CurrClip.w) : 0.f;
    float invWp = (abs(pin.PrevClip.w) > 1e-6f) ? (1.f / pin.PrevClip.w) : 0.f;
    float2 currNdc = pin.CurrClip.xy * invWc;
    float2 prevNdc = pin.PrevClip.xy * invWp;
    outt.Velocity = (prevNdc - currNdc) * float2(0.5f, -0.5f);
    return outt;
}
