/*
 * VMX versions of the H.264 decoder's motion compensation for the Xbox 360
 * (Xenon), for XBMC-360's FFmpeg 1.2: luma quarter-pel (16x16 and 8x8, all
 * 16 positions, put and avg) and chroma eighth-pel (8 and 4 wide).
 *
 * Xenon's VMX has no integer multiplies, so the filters use shifts and adds:
 *   6-tap:   (a+f) - 5(b+e) + 20(c+d),  5x = 4x + x,  20x = 16x + 4x
 *   bilinear: (8-x)a + xb = 8a + x(b-a), with x(b-a) summed from the bits of x
 * The 2-D centre position stays in 16-bit lanes via the exact identity
 *   floor((a - 5b + 20c) / 16) = ((((a - b) >> 2) - b + c) >> 2) + c
 * (as in FFmpeg's x86 code). Everything is bit-exact with the C versions;
 * ff_xbox_dsp_selftest() checks that on the console before anything is used.
 *
 * C89: the SDK's C compiler doesn't take declarations after statements.
 */
#include <xtl.h>
#include <string.h>

#include "libavcodec/dsputil.h"
#include "libavcodec/h264chroma.h"
#include "libavcodec/h264dsp.h"
#include "libavcodec/h264qpel.h"
#include "libavcodec/videodsp.h"
#include "xbox_dsp.h"

int ff_xbox_vmx_flags = 0;

/* Set by the self-test: which functions matched C (bit per qpel position). */
static int tested;
static unsigned qpel_ok[2][2];     /* [size: 0 = 16, 1 = 8][0 = put, 1 = avg] */
static unsigned chroma_ok[2][2];   /* [0 = mc8, 1 = mc4][put, avg]: bit 0 */
static int idct_ok;                /* 4x4 IDCT, DC add and the macroblock loops */

#define LOADU(p) __vor(__lvlx((p), 0), __lvrx((p), 16))

/* ------------------------------------------------------------ stores */

static __forceinline void store16(__vector4 v, uint8_t *dst)
{
    if (((UINT_PTR)dst & 15) == 0) {
        __stvx(v, dst, 0);
    } else {
        __stvlx(v, dst, 0);
        __stvrx(v, dst, 16);
    }
}

/* Stores lanes 0..n-1 (n = 4 or 8) at a 4-byte-aligned dst. */
static __forceinline void store_words(__vector4 v, uint8_t *dst, const int n)
{
    v = __vperm(v, v, __lvsr(dst, 0));   /* move lane 0 to dst's position */
    __stvewx(v, dst, 0);
    if (n == 8)
        __stvewx(v, dst, 4);
}

static __forceinline void store_n(__vector4 v, uint8_t *dst, const int n)
{
    if (n == 16) {
        store16(v, dst);
    } else if (((UINT_PTR)dst & 3) == 0) {
        store_words(v, dst, n);
    } else {
        __declspec(align(16)) uint8_t tmp[16];
        __stvx(v, tmp, 0);
        memcpy(dst, tmp, n);
    }
}

/* ------------------------------------------------------------ luma */

/* (a+f) - 5(b+e) + 20(c+d) on 8 signed 16-bit lanes. */
static __forceinline __vector4 tap6(__vector4 a, __vector4 b, __vector4 c, __vector4 d,
                                    __vector4 e, __vector4 f, __vector4 v2, __vector4 v4)
{
    __vector4 s20 = __vadduhm(c, d);
    __vector4 s5 = __vadduhm(b, e);
    __vector4 t20 = __vadduhm(__vslh(s20, v4), __vslh(s20, v2));
    __vector4 t5 = __vadduhm(__vslh(s5, v2), s5);
    return __vadduhm(__vsubuhm(t20, t5), __vadduhm(a, f));
}

/*
 * Horizontal 6-tap of one row, unrounded: *hi = pixels 0-7, *lo = 8-15
 * (n == 16 only). *f0 / *f1 get the full-pel pixels at offsets 0 and 1.
 */
static __forceinline void hfilt(const uint8_t *src, const int n, __vector4 *hi, __vector4 *lo,
                                __vector4 *f0, __vector4 *f1, __vector4 v2, __vector4 v4)
{
    __vector4 z = __vzero();
    __vector4 a = LOADU(src - 2);
    __vector4 b = n == 16 ? LOADU(src + 14) : a;
    __vector4 m1 = __vsldoi(a, b, 1);
    __vector4 p0 = __vsldoi(a, b, 2);
    __vector4 p1 = __vsldoi(a, b, 3);
    __vector4 p2 = __vsldoi(a, b, 4);
    __vector4 p3 = __vsldoi(a, b, 5);

    *hi = tap6(__vmrghb(z, a), __vmrghb(z, m1), __vmrghb(z, p0), __vmrghb(z, p1),
               __vmrghb(z, p2), __vmrghb(z, p3), v2, v4);
    if (n == 16)
        *lo = tap6(__vmrglb(z, a), __vmrglb(z, m1), __vmrglb(z, p0), __vmrglb(z, p1),
                   __vmrglb(z, p2), __vmrglb(z, p3), v2, v4);
    *f0 = p0;
    *f1 = p1;
}

/* Vertical 6-tap over six rows of bytes, rounded and packed. */
static __forceinline __vector4 vfilt(__vector4 r0, __vector4 r1, __vector4 r2, __vector4 r3,
                                     __vector4 r4, __vector4 r5, const int n,
                                     __vector4 v2, __vector4 v4, __vector4 v5, __vector4 c16)
{
    __vector4 z = __vzero();
    __vector4 hi = tap6(__vmrghb(z, r0), __vmrghb(z, r1), __vmrghb(z, r2), __vmrghb(z, r3),
                        __vmrghb(z, r4), __vmrghb(z, r5), v2, v4);
    __vector4 lo;

    hi = __vsrah(__vadduhm(hi, c16), v5);
    if (n != 16)
        return __vpkshus(hi, hi);
    lo = tap6(__vmrglb(z, r0), __vmrglb(z, r1), __vmrglb(z, r2), __vmrglb(z, r3),
              __vmrglb(z, r4), __vmrglb(z, r5), v2, v4);
    lo = __vsrah(__vadduhm(lo, c16), v5);
    return __vpkshus(hi, lo);
}

/* Second (vertical) pass of the centre position on unrounded 6-tap rows. */
static __forceinline __vector4 hv2(__vector4 tB, __vector4 tA, __vector4 t0, __vector4 t1,
                                   __vector4 t2, __vector4 t3, __vector4 v2, __vector4 v6, __vector4 c32)
{
    __vector4 a = __vadduhm(tB, t3);
    __vector4 b = __vadduhm(tA, t2);
    __vector4 c = __vadduhm(t0, t1);
    __vector4 x = __vsrah(__vsubuhm(a, b), v2);

    x = __vsubuhm(x, b);
    x = __vaddshs(x, c);   /* saturation only where the result clips anyway */
    x = __vsrah(x, v2);
    x = __vadduhm(x, c);   /* = floor((a - 5b + 20c) / 16) */
    return __vsrah(__vadduhm(x, c32), v6);
}

static __forceinline __vector4 round5(__vector4 x, __vector4 v5, __vector4 c16)
{
    return __vsrah(__vadduhm(x, c16), v5);
}

/*
 * One n x n block (n = 16 or 8) at quarter-pel position (mx, my); avg
 * averages the result into dst. With constant arguments the compiler keeps
 * only the paths a position needs.
 */
static __forceinline void qpel(uint8_t *dst, const uint8_t *src, int stride,
                               const int n, const int mx, const int my, const int avg)
{
    const int need_h = mx != 0 && my != 2;
    const int need_v = my != 0 && mx != 2;
    const int need_hv = (mx == 2 && my != 0) || (my == 2 && mx != 0);
    const int h_row = my == 3;          /* H filter on row y or y + 1 */
    const int v_col = mx == 3;          /* V filter on column 0 or 1 */
    __vector4 v2 = __vspltish(2), v4 = __vspltish(4), v5 = __vspltish(5), v6 = __vspltish(6);
    __vector4 c16 = __vslh(__vspltish(1), v4);
    __vector4 c32 = __vadduhm(c16, c16);
    __vector4 r0, r1, r2, r3, r4, r5;                 /* V window: rows y-2 .. y+3 */
    __vector4 t0h, t1h, t2h, t3h, t4h, t5h;           /* HV window, unrounded 6-tap */
    __vector4 t0l, t1l, t2l, t3l, t4l, t5l;
    __vector4 hh, hl, f0, f1, res, hv, h, v;
    const uint8_t *vs = src + v_col;
    int y;

    r0 = r1 = r2 = r3 = r4 = r5 = __vzero();
    t0h = t1h = t2h = t3h = t4h = t5h = __vzero();
    t0l = t1l = t2l = t3l = t4l = t5l = __vzero();
    hh = hl = f0 = f1 = __vzero();

    if (need_v) {
        r0 = LOADU(vs - 2 * stride);
        r1 = LOADU(vs - stride);
        r2 = LOADU(vs);
        r3 = LOADU(vs + stride);
        r4 = LOADU(vs + 2 * stride);
    }
    if (need_hv) {
        hfilt(src - 2 * stride, n, &t0h, &t0l, &f0, &f1, v2, v4);
        hfilt(src - stride, n, &t1h, &t1l, &f0, &f1, v2, v4);
        hfilt(src, n, &t2h, &t2l, &f0, &f1, v2, v4);
        hfilt(src + stride, n, &t3h, &t3l, &f0, &f1, v2, v4);
        hfilt(src + 2 * stride, n, &t4h, &t4l, &f0, &f1, v2, v4);
    }

    for (y = 0; y < n; y++) {
        if (need_v)
            r5 = LOADU(vs + 3 * stride);
        if (need_hv)
            hfilt(src + 3 * stride, n, &t5h, &t5l, &f0, &f1, v2, v4);

        if (mx == 0 && my == 0) {
            res = LOADU(src);
        } else {
            if (need_h) {
                if (need_hv) {
                    hh = h_row ? t3h : t2h;
                    hl = h_row ? t3l : t2l;
                } else {
                    hfilt(src + h_row * stride, n, &hh, &hl, &f0, &f1, v2, v4);
                }
                hh = round5(hh, v5, c16);
                if (n == 16) {
                    hl = round5(hl, v5, c16);
                    h = __vpkshus(hh, hl);
                } else {
                    h = __vpkshus(hh, hh);
                }
            } else {
                h = __vzero();
            }
            v = need_v ? vfilt(r0, r1, r2, r3, r4, r5, n, v2, v4, v5, c16) : __vzero();
            if (need_hv) {
                hv = hv2(t0h, t1h, t2h, t3h, t4h, t5h, v2, v6, c32);
                hv = __vpkshus(hv, n == 16 ? hv2(t0l, t1l, t2l, t3l, t4l, t5l, v2, v6, c32) : hv);
            } else {
                hv = __vzero();
            }

            if (my == 0)
                res = mx == 2 ? h : __vavgub(h, mx == 3 ? f1 : f0);
            else if (mx == 0)
                res = my == 2 ? v : __vavgub(v, my == 3 ? r3 : r2);
            else if (need_hv)
                res = need_h ? __vavgub(hv, h) : need_v ? __vavgub(hv, v) : hv;
            else
                res = __vavgub(h, v);
        }

        if (avg)
            res = __vavgub(res, LOADU(dst));
        store_n(res, dst, n);

        r0 = r1; r1 = r2; r2 = r3; r3 = r4; r4 = r5;
        t0h = t1h; t1h = t2h; t2h = t3h; t3h = t4h; t4h = t5h;
        t0l = t1l; t1l = t2l; t2l = t3l; t3l = t4l; t4l = t5l;
        src += stride;
        vs += stride;
        dst += stride;
    }
}

#define QPEL_FN(OP, AVG, N, X, Y) \
static void OP ## _qpel ## N ## _mc ## X ## Y ## _vmx(uint8_t *dst, uint8_t *src, int stride) \
{ \
    qpel(dst, src, stride, N, X, Y, AVG); \
}

#define QPEL_ROW(OP, AVG, N, Y) \
    QPEL_FN(OP, AVG, N, 0, Y) QPEL_FN(OP, AVG, N, 1, Y) QPEL_FN(OP, AVG, N, 2, Y) QPEL_FN(OP, AVG, N, 3, Y)

#define QPEL_ALL(OP, AVG, N) \
    QPEL_ROW(OP, AVG, N, 0) QPEL_ROW(OP, AVG, N, 1) QPEL_ROW(OP, AVG, N, 2) QPEL_ROW(OP, AVG, N, 3)

QPEL_ALL(put, 0, 16)
QPEL_ALL(avg, 1, 16)
QPEL_ALL(put, 0, 8)
QPEL_ALL(avg, 1, 8)

/* FFmpeg's table order: index = x + 4 * y. */
#define QPEL_TAB(OP, N) { \
    OP ## _qpel ## N ## _mc00_vmx, OP ## _qpel ## N ## _mc10_vmx, OP ## _qpel ## N ## _mc20_vmx, OP ## _qpel ## N ## _mc30_vmx, \
    OP ## _qpel ## N ## _mc01_vmx, OP ## _qpel ## N ## _mc11_vmx, OP ## _qpel ## N ## _mc21_vmx, OP ## _qpel ## N ## _mc31_vmx, \
    OP ## _qpel ## N ## _mc02_vmx, OP ## _qpel ## N ## _mc12_vmx, OP ## _qpel ## N ## _mc22_vmx, OP ## _qpel ## N ## _mc32_vmx, \
    OP ## _qpel ## N ## _mc03_vmx, OP ## _qpel ## N ## _mc13_vmx, OP ## _qpel ## N ## _mc23_vmx, OP ## _qpel ## N ## _mc33_vmx }

static const qpel_mc_func vmx_qpel[2][2][16] = {
    { QPEL_TAB(put, 16), QPEL_TAB(avg, 16) },
    { QPEL_TAB(put, 8), QPEL_TAB(avg, 8) },
};

/* ------------------------------------------------------------ chroma */

/* m * d for m in 0..7, given masks of m's bits (all ones or zero). */
static __forceinline __vector4 mul_small(__vector4 d, __vector4 b1, __vector4 b2, __vector4 b4,
                                         __vector4 s1, __vector4 s2)
{
    return __vadduhm(__vadduhm(__vand(d, b1), __vand(__vslh(d, s1), b2)), __vand(__vslh(d, s2), b4));
}

/* (8-x)a + xb for one row of n (<= 8) pixels, as 16-bit lanes. */
static __forceinline __vector4 chroma_row(const uint8_t *src, __vector4 xb1, __vector4 xb2, __vector4 xb4,
                                          __vector4 s1, __vector4 s2, __vector4 s3)
{
    __vector4 z = __vzero();
    __vector4 p = LOADU(src);
    __vector4 a = __vmrghb(z, p);
    __vector4 b = __vmrghb(z, __vsldoi(p, p, 1));
    return __vadduhm(__vslh(a, s3), mul_small(__vsubuhm(b, a), xb1, xb2, xb4, s1, s2));
}

static __forceinline void chroma_mc(uint8_t *dst, const uint8_t *src, int stride, int h, int x, int y,
                                    const int n, const int avg)
{
    __vector4 ones = __vspltish(-1), z = __vzero();
    __vector4 s1 = __vspltish(1), s2 = __vspltish(2), s3 = __vspltish(3), s6 = __vspltish(6);
    __vector4 c32 = __vslh(s1, __vspltish(5));
    __vector4 xb1 = (x & 1) ? ones : z, xb2 = (x & 2) ? ones : z, xb4 = (x & 4) ? ones : z;
    __vector4 yb1 = (y & 1) ? ones : z, yb2 = (y & 2) ? ones : z, yb4 = (y & 4) ? ones : z;
    __vector4 h0 = chroma_row(src, xb1, xb2, xb4, s1, s2, s3), h1, r;
    int i;

    for (i = 0; i < h; i++) {
        src += stride;
        h1 = chroma_row(src, xb1, xb2, xb4, s1, s2, s3);
        /* 8 h0 + y (h1 - h0), then (+32) >> 6 */
        r = __vadduhm(__vslh(h0, s3), mul_small(__vsubuhm(h1, h0), yb1, yb2, yb4, s1, s2));
        r = __vsrah(__vadduhm(r, c32), s6);
        r = __vpkshus(r, r);
        if (avg)
            r = __vavgub(r, LOADU(dst));
        store_n(r, dst, n);
        h0 = h1;
        dst += stride;
    }
}

static void put_chroma_mc8_vmx(uint8_t *dst, uint8_t *src, int stride, int h, int x, int y)
{
    chroma_mc(dst, src, stride, h, x, y, 8, 0);
}

static void avg_chroma_mc8_vmx(uint8_t *dst, uint8_t *src, int stride, int h, int x, int y)
{
    chroma_mc(dst, src, stride, h, x, y, 8, 1);
}

static void put_chroma_mc4_vmx(uint8_t *dst, uint8_t *src, int stride, int h, int x, int y)
{
    chroma_mc(dst, src, stride, h, x, y, 4, 0);
}

static void avg_chroma_mc4_vmx(uint8_t *dst, uint8_t *src, int stride, int h, int x, int y)
{
    chroma_mc(dst, src, stride, h, x, y, 4, 1);
}

static const h264_chroma_mc_func vmx_chroma[2][2] = {
    { put_chroma_mc8_vmx, avg_chroma_mc8_vmx },
    { put_chroma_mc4_vmx, avg_chroma_mc4_vmx },
};

/* ------------------------------------------------------------ IDCT */

/* libavcodec/h264.h's scan8 (cache index of each 4x4 block's non-zero count). */
static const uint8_t xscan8[16 * 3] = {
    4 +  1 * 8, 5 +  1 * 8, 4 +  2 * 8, 5 +  2 * 8, 6 +  1 * 8, 7 +  1 * 8, 6 +  2 * 8, 7 +  2 * 8,
    4 +  3 * 8, 5 +  3 * 8, 4 +  4 * 8, 5 +  4 * 8, 6 +  3 * 8, 7 +  3 * 8, 6 +  4 * 8, 7 +  4 * 8,
    4 +  6 * 8, 5 +  6 * 8, 4 +  7 * 8, 5 +  7 * 8, 6 +  6 * 8, 7 +  6 * 8, 6 +  7 * 8, 7 +  7 * 8,
    4 +  8 * 8, 5 +  8 * 8, 4 +  9 * 8, 5 +  9 * 8, 6 +  8 * 8, 7 +  8 * 8, 6 +  9 * 8, 7 +  9 * 8,
    4 + 11 * 8, 5 + 11 * 8, 4 + 12 * 8, 5 + 12 * 8, 6 + 11 * 8, 7 + 11 * 8, 6 + 12 * 8, 7 + 12 * 8,
    4 + 13 * 8, 5 + 13 * 8, 4 + 14 * 8, 5 + 14 * 8, 6 + 13 * 8, 7 + 13 * 8, 6 + 14 * 8, 7 + 14 * 8,
};

/* Adds lanes 0..3 (signed 16-bit) to the 4 pixels at dst, clipping to 0..255. */
static __forceinline void add_row4(uint8_t *dst, __vector4 r)
{
    __vector4 p = __vmrghb(__vzero(), LOADU(dst));
    p = __vadduhm(p, r);
    store_n(__vpkshus(p, p), dst, 4);
}

/*
 * One 4x4 block, as ff_h264_idct_add: a vertical then a horizontal butterfly
 * pass (16-bit lanes, like the int16 block the C version stores between the
 * passes; H.264 bitstreams keep these values within 16 bits), >> 6, add,
 * clip, then clear the coefficients.
 */
static void idct4_add_vmx(uint8_t *dst, int16_t *block, int stride)
{
    __vector4 z = __vzero(), one = __vspltish(1), six = __vspltish(6);
    __vector4 c32 = __vsldoi(__vslh(one, __vspltish(5)), z, 14);   /* 32 in lane 0 only */
    __vector4 v0 = __vadduhm(__lvx(block, 0), c32), v1 = __lvx(block, 16);
    __vector4 r0 = v0, r1 = __vsldoi(v0, v0, 8), r2 = v1, r3 = __vsldoi(v1, v1, 8);
    __vector4 z0, z1, z2, z3, b0, b1, b2, b3, m0, m1, t01, t23, e0, e1, e2, e3;

    /* Columns: rows 0..3 of each column are lanes 0..3 of r0..r3. */
    z0 = __vadduhm(r0, r2);
    z1 = __vsubuhm(r0, r2);
    z2 = __vsubuhm(__vsrah(r1, one), r3);
    z3 = __vadduhm(r1, __vsrah(r3, one));
    b0 = __vadduhm(z0, z3);
    b1 = __vadduhm(z1, z2);
    b2 = __vsubuhm(z1, z2);
    b3 = __vsubuhm(z0, z3);

    /* Transpose so that lane i holds row i of the intermediate block. */
    m0 = __vmrghh(b0, b2);
    m1 = __vmrghh(b1, b3);
    t01 = __vmrghh(m0, m1);
    t23 = __vmrglh(m0, m1);
    e0 = t01;
    e1 = __vsldoi(t01, t01, 8);
    e2 = t23;
    e3 = __vsldoi(t23, t23, 8);

    z0 = __vadduhm(e0, e2);
    z1 = __vsubuhm(e0, e2);
    z2 = __vsubuhm(__vsrah(e1, one), e3);
    z3 = __vadduhm(e1, __vsrah(e3, one));
    add_row4(dst, __vsrah(__vadduhm(z0, z3), six));
    add_row4(dst + stride, __vsrah(__vadduhm(z1, z2), six));
    add_row4(dst + 2 * stride, __vsrah(__vsubuhm(z1, z2), six));
    add_row4(dst + 3 * stride, __vsrah(__vsubuhm(z0, z3), six));

    __stvx(z, block, 0);
    __stvx(z, block, 16);
}

/* DC-only block, as ff_h264_idct_dc_add: adds (block[0] + 32) >> 6 to all 16 pixels. */
static void idct_dc_add_vmx(uint8_t *dst, int16_t *block, int stride)
{
    __vector4 c32 = __vslh(__vspltish(1), __vspltish(5));
    __vector4 dc = __vsplth(__vsrah(__vadduhm(__lvx(block, 0), c32), __vspltish(6)), 0);
    int i;

    block[0] = 0;
    for (i = 0; i < 4; i++)
        add_row4(dst + i * stride, dc);
}

static void idct_add16_vmx(uint8_t *dst, const int *block_offset, int16_t *block, int stride,
                           const uint8_t nnzc[15 * 8])
{
    int i;

    for (i = 0; i < 16; i++) {
        int nnz = nnzc[xscan8[i]];
        if (nnz) {
            if (nnz == 1 && block[i * 16])
                idct_dc_add_vmx(dst + block_offset[i], block + i * 16, stride);
            else
                idct4_add_vmx(dst + block_offset[i], block + i * 16, stride);
        }
    }
}

static void idct_add16intra_vmx(uint8_t *dst, const int *block_offset, int16_t *block, int stride,
                                const uint8_t nnzc[15 * 8])
{
    int i;

    for (i = 0; i < 16; i++) {
        if (nnzc[xscan8[i]])
            idct4_add_vmx(dst + block_offset[i], block + i * 16, stride);
        else if (block[i * 16])
            idct_dc_add_vmx(dst + block_offset[i], block + i * 16, stride);
    }
}

/* 4:2:0 chroma: blocks 16..19 (U) and 32..35 (V). */
static void idct_add8_vmx(uint8_t **dest, const int *block_offset, int16_t *block, int stride,
                          const uint8_t nnzc[15 * 8])
{
    int i, j;

    for (j = 1; j < 3; j++)
        for (i = j * 16; i < j * 16 + 4; i++) {
            if (nnzc[xscan8[i]])
                idct4_add_vmx(dest[j - 1] + block_offset[i], block + i * 16, stride);
            else if (block[i * 16])
                idct_dc_add_vmx(dest[j - 1] + block_offset[i], block + i * 16, stride);
        }
}

/* ------------------------------------------------------------ init hooks */

void ff_h264dsp_init_xbox(H264DSPContext *c, const int bit_depth, const int chroma_format_idc)
{
    if (!tested || bit_depth != 8 || !(ff_xbox_vmx_flags & XVMX_IDCT) || !idct_ok)
        return;
    c->h264_idct_add = idct4_add_vmx;
    c->h264_idct_dc_add = idct_dc_add_vmx;
    c->h264_idct_add16 = idct_add16_vmx;
    c->h264_idct_add16intra = idct_add16intra_vmx;
    if (chroma_format_idc <= 1)
        c->h264_idct_add8 = idct_add8_vmx;
}

void ff_h264qpel_init_xbox(H264QpelContext *c, int bit_depth)
{
    int s, p;

    if (!tested || bit_depth != 8 || !(ff_xbox_vmx_flags & XVMX_QPEL))
        return;
    for (s = 0; s < 2; s++)
        for (p = 0; p < 16; p++) {
            if (qpel_ok[s][0] & (1u << p))
                c->put_h264_qpel_pixels_tab[s][p] = vmx_qpel[s][0][p];
            if (qpel_ok[s][1] & (1u << p))
                c->avg_h264_qpel_pixels_tab[s][p] = vmx_qpel[s][1][p];
        }
}

void ff_h264chroma_init_xbox(H264ChromaContext *c, int bit_depth)
{
    int s;

    if (!tested || bit_depth != 8 || !(ff_xbox_vmx_flags & XVMX_CHROMA))
        return;
    for (s = 0; s < 2; s++) {
        if (chroma_ok[s][0])
            c->put_h264_chroma_pixels_tab[s] = vmx_chroma[s][0];
        if (chroma_ok[s][1])
            c->avg_h264_chroma_pixels_tab[s] = vmx_chroma[s][1];
    }
}

void ff_videodsp_init_xbox(VideoDSPContext *ctx, int bpc)
{
    /* XBMC-360's dcbt prefetch: only a cache hint, it can't change results. */
    if (ff_xbox_vmx_flags & XVMX_PREFETCH)
        ff_videodsp_init_ppc(ctx, bpc);
}

/* ------------------------------------------------------------ self-test */

#define T_STRIDE 64
#define T_ROWS   48

static unsigned rng_state = 0x12345678;

static unsigned rng(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

static void fill(uint8_t *p, int len)
{
    int i;
    unsigned mode = rng() % 4;

    for (i = 0; i < len; i++) {
        /* Mostly random, sometimes extremes, which exercise clipping. */
        if (mode == 0)
            p[i] = (uint8_t)((rng() & 1) ? 255 : 0);
        else
            p[i] = (uint8_t)rng();
    }
}

static void report(void (*log)(const char *), const char *what, int n, int pos,
                   const uint8_t *ref, const uint8_t *got, int x, int y)
{
    char line[160];

    if (log == NULL)
        return;
    _snprintf(line, sizeof(line), "vmx selftest: %s%d pos %d MISMATCH at (%d,%d): C %d, VMX %d\n",
              what, n, pos, x, y, ref[y * T_STRIDE + x], got[y * T_STRIDE + x]);
    line[sizeof(line) - 1] = '\0';
    log(line);
}

/* Compares an n x n (w x h) region; returns 1 if equal. */
static int same(const uint8_t *a, const uint8_t *b, int w, int h, int *fx, int *fy)
{
    int x, y;

    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            if (a[y * T_STRIDE + x] != b[y * T_STRIDE + x]) {
                *fx = x;
                *fy = y;
                return 0;
            }
    return 1;
}

int ff_xbox_dsp_selftest(void (*log)(const char *line))
{
    static __declspec(align(16)) uint8_t src[T_STRIDE * T_ROWS];
    static __declspec(align(16)) uint8_t dref[T_STRIDE * 20], dvmx[T_STRIDE * 20];
    H264QpelContext qc;
    H264ChromaContext cc;
    int saved = ff_xbox_vmx_flags, failures = 0, s, op, p, trial, fx, fy;
    char line[160];

    /* C reference tables: the hooks don't touch them while flags are 0. */
    ff_xbox_vmx_flags = 0;
    ff_h264qpel_init(&qc, 8);
    ff_h264chroma_init(&cc, 8);
    ff_xbox_vmx_flags = saved;

    for (s = 0; s < 2; s++) {
        int n = s ? 8 : 16;
        for (op = 0; op < 2; op++) {
            qpel_ok[s][op] = 0;
            for (p = 0; p < 16; p++) {
                qpel_mc_func cf = op ? qc.avg_h264_qpel_pixels_tab[s][p] : qc.put_h264_qpel_pixels_tab[s][p];
                qpel_mc_func vf = vmx_qpel[s][op][p];
                int ok = 1;

                for (trial = 0; trial < 64 && ok; trial++) {
                    /* Rows -2..n+3 and columns -2..n+3 (+16 over-read) must stay inside src. */
                    uint8_t *sp = src + 3 * T_STRIDE + 3 + (trial & 15);
                    int doff = (n == 8 && (trial & 1)) ? 8 : 0;

                    fill(src, sizeof(src));
                    fill(dref, sizeof(dref));
                    memcpy(dvmx, dref, sizeof(dref));
                    cf(dref + doff, sp, T_STRIDE);
                    vf(dvmx + doff, sp, T_STRIDE);
                    if (!same(dref, dvmx, T_STRIDE, n, &fx, &fy)) {
                        report(log, op ? "avg_qpel" : "put_qpel", n, p, dref, dvmx, fx, fy);
                        ok = 0;
                    }
                }
                if (ok)
                    qpel_ok[s][op] |= 1u << p;
                else
                    failures++;
            }
        }
    }

    for (s = 0; s < 2; s++) {
        int n = s ? 4 : 8;
        for (op = 0; op < 2; op++) {
            h264_chroma_mc_func cf = op ? cc.avg_h264_chroma_pixels_tab[s] : cc.put_h264_chroma_pixels_tab[s];
            h264_chroma_mc_func vf = vmx_chroma[s][op];
            int ok = 1;

            for (trial = 0; trial < 64 * 3 && ok; trial++) {
                uint8_t *sp = src + 2 * T_STRIDE + (trial & 15);
                int h = (trial % 3 == 0) ? 2 : (trial % 3 == 1) ? 4 : 8;
                int x = (trial / 3) & 7, y = (trial / 24) & 7;
                int doff = (rng() & 1) ? n : 0;

                fill(src, sizeof(src));
                fill(dref, sizeof(dref));
                memcpy(dvmx, dref, sizeof(dref));
                cf(dref + doff, sp, T_STRIDE, h, x, y);
                vf(dvmx + doff, sp, T_STRIDE, h, x, y);
                if (!same(dref, dvmx, T_STRIDE, h, &fx, &fy)) {
                    report(log, op ? "avg_chroma" : "put_chroma", n, x + 8 * y, dref, dvmx, fx, fy);
                    ok = 0;
                }
            }
            chroma_ok[s][op] = ok;
            if (!ok)
                failures++;
        }
    }

    /* IDCT: the three per-macroblock loops against FFmpeg's C ones, with random
     * coefficients (within the 16-bit range H.264 guarantees), random non-zero
     * patterns including DC-only blocks, and FFmpeg's block offsets. */
    {
        static __declspec(align(16)) int16_t cref[48 * 16], cvmx[48 * 16];
        H264DSPContext hc;
        uint8_t nnz[15 * 8];
        uint8_t *dref2[2], *dvmx2[2];
        int offs[48], i, kind, ok = 1;

        ff_xbox_vmx_flags = 0;
        ff_h264dsp_init(&hc, 8, 1);
        ff_xbox_vmx_flags = saved;
        for (i = 0; i < 16; i++) {
            int d = xscan8[i] - xscan8[0];
            offs[i] = 4 * (d & 7) + 4 * T_STRIDE * (d >> 3);
        }
        for (i = 0; i < 4; i++)
            offs[16 + i] = offs[32 + i] = offs[i];
        dref2[0] = dref;
        dref2[1] = dref + 8;
        dvmx2[0] = dvmx;
        dvmx2[1] = dvmx + 8;

        for (trial = 0; trial < 300 && ok; trial++) {
            for (kind = 0; kind < 3 && ok; kind++) {
                fill(dref, sizeof(dref));
                memcpy(dvmx, dref, sizeof(dref));
                for (i = 0; i < 48 * 16; i++) {
                    unsigned r = rng();
                    cref[i] = (r & 3) ? 0 : (int16_t)((int)(r >> 4) % 400 - 200);
                }
                for (i = 0; i < 48; i++)
                    if (rng() % 3 == 0)
                        cref[i * 16] = (int16_t)((int)(rng() % 2000) - 1000);   /* strong DC */
                memcpy(cvmx, cref, sizeof(cref));
                for (i = 0; i < (int)sizeof(nnz); i++)
                    nnz[i] = (uint8_t)(rng() % 3);

                if (kind == 0) {
                    hc.h264_idct_add16(dref, offs, cref, T_STRIDE, nnz);
                    idct_add16_vmx(dvmx, offs, cvmx, T_STRIDE, nnz);
                } else if (kind == 1) {
                    hc.h264_idct_add16intra(dref, offs, cref, T_STRIDE, nnz);
                    idct_add16intra_vmx(dvmx, offs, cvmx, T_STRIDE, nnz);
                } else {
                    hc.h264_idct_add8(dref2, offs, cref, T_STRIDE, nnz);
                    idct_add8_vmx(dvmx2, offs, cvmx, T_STRIDE, nnz);
                }
                if (!same(dref, dvmx, T_STRIDE, 16, &fx, &fy)) {
                    report(log, kind == 0 ? "idct_add" : kind == 1 ? "idct_add16intra" : "idct_add8", 4, 0,
                           dref, dvmx, fx, fy);
                    ok = 0;
                } else if (memcmp(cref, cvmx, sizeof(cref)) != 0) {
                    if (log)
                        log("vmx selftest: idct leaves different coefficients behind\n");
                    ok = 0;
                }
            }
        }
        idct_ok = ok;
        if (!ok)
            failures++;
    }

    tested = 1;
    if (log) {
        _snprintf(line, sizeof(line), "vmx selftest: qpel16 put %04x avg %04x, qpel8 put %04x avg %04x, "
                  "chroma8 %d/%d, chroma4 %d/%d, idct %d, %d mismatches\n",
                  qpel_ok[0][0], qpel_ok[0][1], qpel_ok[1][0], qpel_ok[1][1],
                  chroma_ok[0][0], chroma_ok[0][1], chroma_ok[1][0], chroma_ok[1][1], idct_ok, failures);
        line[sizeof(line) - 1] = '\0';
        log(line);
    }
    return failures;
}
