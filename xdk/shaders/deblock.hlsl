// GPU deblocking for streams decoded without H.264's in-loop filter ("Fast decode").
//
// One pass filters the edges in one direction (Dir = (1,0): across vertical edges, (0,1): across
// horizontal edges); running both passes approximates the in-loop filter. It is H.264's normal
// filter (spec 8.7.2.3) on the luma plane at the 8-pixel grid (8x8 transform and macroblock edges),
// with alpha/beta/tc0 looked up from the QP of the two macroblocks. Boundary strength isn't known
// after decoding: macroblock edges use bS 2, internal edges bS 1. Values are 0..255 like the decoder.

sampler2D Src : register(s0);   // luma, point-sampled
sampler2D QP  : register(s1);   // QP of each 16x16 macroblock, point-sampled
sampler2D Lut : register(s2);   // 64x1: alpha, beta, tc0 (bS 1), tc0 (bS 2), point-sampled

float4 Size   : register(c0);   // width, height, 1/width, 1/height
float4 MbSize : register(c1);   // 1/macroblocks across, 1/macroblocks down
float4 Dir    : register(c2);   // xy: step across the edges

float Luma(float2 pix)
{
    return floor(tex2D(Src, (pix + 0.5) * Size.zw).r * 255.0 + 0.5);
}

float MbQP(float2 pix)
{
    return floor(tex2D(QP, (floor(pix / 16.0) + 0.5) * MbSize.xy).r * 255.0 + 0.5);
}

float4 main(float2 uv : TEXCOORD0) : COLOR
{
    float2 pix = floor(uv * Size.xy);
    float2 step = Dir.xy;
    float c = dot(pix, step);                       // coordinate across the edges
    float m = c - 8.0 * floor(c / 8.0);             // position inside the 8-pixel block
    float e = (m < 4.0) ? c - m : c + 8.0 - m;      // first pixel after the nearest edge (q0)
    float2 q0p = pix + step * (e - c);

    float p2 = Luma(q0p - 3.0 * step), p1 = Luma(q0p - 2.0 * step), p0 = Luma(q0p - step);
    float q0 = Luma(q0p), q1 = Luma(q0p + step), q2 = Luma(q0p + 2.0 * step);
    float cur = Luma(pix);

    float qp = floor((MbQP(q0p - step) + MbQP(q0p) + 1.0) / 2.0);
    float4 lut = floor(tex2D(Lut, float2((qp + 0.5) / 64.0, 0.5)) * 255.0 + 0.5);
    float alpha = lut.r, beta = lut.g;
    float mbEdge = (e - 16.0 * floor(e / 16.0)) < 0.5 ? 1.0 : 0.0;
    float tc0 = lerp(lut.b, lut.a, mbEdge);

    float extent = dot(Size.xy, step);
    float filter = (abs(p0 - q0) < alpha) * (abs(p1 - p0) < beta) * (abs(q1 - q0) < beta) *
                   (e > 0.5) * (e < extent - 0.5) * ((m < 2.0) + (m > 5.0));

    float ap = abs(p2 - p0) < beta ? 1.0 : 0.0;
    float aq = abs(q2 - q0) < beta ? 1.0 : 0.0;
    float tc = tc0 + ap + aq;
    float delta = clamp(floor(((q0 - p0) * 4.0 + (p1 - q1) + 4.0) / 8.0), -tc, tc);
    float avg = floor((p0 + q0 + 1.0) / 2.0);
    float p1n = p1 + ap * clamp(floor((p2 + avg - 2.0 * p1) / 2.0), -tc0, tc0);
    float q1n = q1 + aq * clamp(floor((q2 + avg - 2.0 * q1) / 2.0), -tc0, tc0);
    float p0n = clamp(p0 + delta, 0.0, 255.0);
    float q0n = clamp(q0 - delta, 0.0, 255.0);

    // This pixel is q0, q1, p1 or p0 of its edge (m = 0, 1, 6, 7); the others don't change.
    float res = (m < 0.5) ? q0n : (m < 1.5) ? q1n : (m > 6.5) ? p0n : (m > 5.5) ? p1n : cur;
    res = lerp(cur, res, filter);
    return (res / 255.0).xxxx;
}
