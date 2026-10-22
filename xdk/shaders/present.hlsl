// Draws a decoded frame: bicubic (Catmull-Rom) luma scaling with optional sharpening, bilinear
// chroma, BT.601 limited-range YUV to RGB. At 1:1 scale the bicubic weights reduce to the source
// pixel, so this is exact when no scaling is needed.

sampler2D TY : register(s0);    // luma (plane or deblocked texture), point-sampled
sampler2D TU : register(s1);    // chroma planes, bilinear
sampler2D TV : register(s2);

float4 Size   : register(c0);   // luma width, height, 1/width, 1/height
float4 Params : register(c1);   // x: sharpening amount (0 = off)

float4 CatmullRom(float t)
{
    float t2 = t * t, t3 = t2 * t;
    return 0.5 * float4(-t3 + 2.0 * t2 - t,
                        3.0 * t3 - 5.0 * t2 + 2.0,
                        -3.0 * t3 + 4.0 * t2 + t,
                        t3 - t2);
}

float Tap(float2 base, float x, float y)
{
    return tex2D(TY, (base + float2(x, y) + 0.5) * Size.zw).r;
}

float4 main(float2 uv : TEXCOORD0) : COLOR
{
    float2 pos = uv * Size.xy - 0.5;
    float2 base = floor(pos);
    float2 f = pos - base;
    float4 wx = CatmullRom(f.x), wy = CatmullRom(f.y);

    float4 r0 = float4(Tap(base, -1, -1), Tap(base, 0, -1), Tap(base, 1, -1), Tap(base, 2, -1));
    float4 r1 = float4(Tap(base, -1,  0), Tap(base, 0,  0), Tap(base, 1,  0), Tap(base, 2,  0));
    float4 r2 = float4(Tap(base, -1,  1), Tap(base, 0,  1), Tap(base, 1,  1), Tap(base, 2,  1));
    float4 r3 = float4(Tap(base, -1,  2), Tap(base, 0,  2), Tap(base, 1,  2), Tap(base, 2,  2));
    float y = dot(wy, float4(dot(wx, r0), dot(wx, r1), dot(wx, r2), dot(wx, r3)));

    // Sharpening: push the bicubic result away from the bilinear one.
    float bilinear = lerp(lerp(r1.y, r1.z, f.x), lerp(r2.y, r2.z, f.x), f.y);
    y = saturate(y + Params.x * (y - bilinear));

    float yl = 1.164383 * (y - 0.062745);
    float u = tex2D(TU, uv).r - 0.501961;
    float v = tex2D(TV, uv).r - 0.501961;
    return float4(yl + 1.596027 * v, yl - 0.391762 * u - 0.812968 * v, yl + 2.017232 * u, 1.0);
}
