#include "canvas.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

bool cv_init(canvas_t *c, int w, int h)
{
    c->px = (uint32_t *)malloc((size_t)w * h * sizeof(uint32_t));
    c->w = w;
    c->h = h;
    cv_unclip(c);
    return c->px != NULL;
}

void cv_free(canvas_t *c)
{
    free(c->px);
    c->px = NULL;
}

void cv_clip(canvas_t *c, int x, int y, int w, int h)
{
    c->cx0 = imax(0, x);
    c->cy0 = imax(0, y);
    c->cx1 = imin(c->w, x + w);
    c->cy1 = imin(c->h, y + h);
}

void cv_unclip(canvas_t *c)
{
    c->cx0 = c->cy0 = 0;
    c->cx1 = c->w;
    c->cy1 = c->h;
}

uint32_t cv_mix(uint32_t dst, uint32_t src, unsigned a)
{
    uint32_t rb, g;

    if (a >= 255)
        return src | 0xFF000000u;
    if (a == 0)
        return dst;
    rb = dst & 0x00FF00FF;
    g = dst & 0x0000FF00;
    rb += (((src & 0x00FF00FF) - rb) * a) >> 8;
    g += (((src & 0x0000FF00) - g) * a) >> 8;
    return 0xFF000000u | (rb & 0x00FF00FF) | (g & 0x0000FF00);
}

static inline void plot(canvas_t *c, int x, int y, uint32_t argb, unsigned cov)
{
    unsigned a = ((argb >> 24) * cov + 127) / 255;
    uint32_t *p = &c->px[y * c->w + x];
    *p = cv_mix(*p, argb, a);
}

void cv_copy(canvas_t *dst, const canvas_t *src)
{
    memcpy(dst->px, src->px, (size_t)src->w * src->h * sizeof(uint32_t));
}

void cv_fill(canvas_t *c, int x, int y, int w, int h, uint32_t argb)
{
    int x0 = imax(x, c->cx0), y0 = imax(y, c->cy0);
    int x1 = imin(x + w, c->cx1), y1 = imin(y + h, c->cy1);
    unsigned a = argb >> 24;
    int i, j;

    for (j = y0; j < y1; j++) {
        uint32_t *row = &c->px[j * c->w];
        if (a == 255) {
            for (i = x0; i < x1; i++)
                row[i] = argb;
        } else {
            for (i = x0; i < x1; i++)
                row[i] = cv_mix(row[i], argb, a);
        }
    }
}

void cv_vgradient(canvas_t *c, int x, int y, int w, int h, uint32_t top, uint32_t bottom)
{
    int j;

    for (j = 0; j < h; j++) {
        unsigned t = h > 1 ? (unsigned)(j * 255 / (h - 1)) : 0;
        cv_fill(c, x, y + j, w, 1, cv_mix(top, bottom, t));
    }
}

/* ------------------------------------------------------------ SDF shapes */

float sd_round_box(float px, float py, float cx, float cy, float hw, float hh, float r)
{
    float qx = fabsf(px - cx) - hw + r;
    float qy = fabsf(py - cy) - hh + r;
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float inside = qx > qy ? qx : qy;

    return sqrtf(ox * ox + oy * oy) + (inside < 0 ? inside : 0) - r;
}

float sd_segment(float px, float py, float ax, float ay, float bx, float by)
{
    float pax = px - ax, pay = py - ay, bax = bx - ax, bay = by - ay;
    float len2 = bax * bax + bay * bay;
    float h = len2 > 0 ? clampf((pax * bax + pay * bay) / len2, 0, 1) : 0;
    float dx = pax - bax * h, dy = pay - bay * h;

    return sqrtf(dx * dx + dy * dy);
}

void cv_sdf(canvas_t *c, float bx, float by, float bw, float bh, sdf_fn fn, const void *ctx, uint32_t argb)
{
    int x0 = imax((int)floorf(bx) - 1, c->cx0), y0 = imax((int)floorf(by) - 1, c->cy0);
    int x1 = imin((int)ceilf(bx + bw) + 1, c->cx1), y1 = imin((int)ceilf(by + bh) + 1, c->cy1);
    int i, j;

    for (j = y0; j < y1; j++) {
        for (i = x0; i < x1; i++) {
            float d = fn(i + 0.5f, j + 0.5f, ctx);
            float cov = clampf(0.5f - d, 0, 1);
            if (cov > 0)
                plot(c, i, j, argb, (unsigned)(cov * 255.0f + 0.5f));
        }
    }
}

struct rrect_ctx { float cx, cy, hw, hh, r, t; };

static float rrect_sdf(float x, float y, const void *p)
{
    const struct rrect_ctx *k = (const struct rrect_ctx *)p;
    return sd_round_box(x, y, k->cx, k->cy, k->hw, k->hh, k->r);
}

static float rrect_outline_sdf(float x, float y, const void *p)
{
    const struct rrect_ctx *k = (const struct rrect_ctx *)p;
    return fabsf(sd_round_box(x, y, k->cx, k->cy, k->hw, k->hh, k->r) + k->t * 0.5f) - k->t * 0.5f;
}

/*
 * Rounded rectangles row by row: pixels within the corner radius of the left
 * and right edges use the exact SDF; the run in between is only limited
 * vertically, so it gets one coverage value for the whole run.
 * outline_t == 0 fills; otherwise draws an outline of that thickness.
 */
static void rrect_rows(canvas_t *c, float x, float y, float w, float h, float r, float outline_t, uint32_t argb)
{
    struct rrect_ctx k;
    float edge;
    int j, j0, j1;

    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    k.cx = x + w / 2;
    k.cy = y + h / 2;
    k.hw = w / 2;
    k.hh = h / 2;
    k.r = r;
    k.t = outline_t;
    edge = fmaxf(r, outline_t) + 1;

    j0 = imax((int)floorf(y) - 1, c->cy0);
    j1 = imin((int)ceilf(y + h) + 1, c->cy1);
    for (j = j0; j < j1; j++) {
        float py = j + 0.5f;
        float dv = fmaxf(y - py, py - (y + h));          /* vertical signed distance */
        float d_mid = outline_t > 0 ? fabsf(dv + outline_t * 0.5f) - outline_t * 0.5f : dv;
        float cov_mid = clampf(0.5f - d_mid, 0, 1);
        int xa = imax((int)floorf(x) - 1, c->cx0);
        int xb = imin((int)ceilf(x + edge) + 1, c->cx1);
        int xc = imax((int)floorf(x + w - edge) - 1, c->cx0);
        int xd = imin((int)ceilf(x + w) + 1, c->cx1);
        int i;

        if (xb >= xc) {
            xb = xd;    /* narrow shape: SDF across the whole row */
            xc = xd;
        }
        for (i = xa; i < xb; i++) {
            float d = outline_t > 0 ? rrect_outline_sdf(i + 0.5f, py, &k) : rrect_sdf(i + 0.5f, py, &k);
            float cov = clampf(0.5f - d, 0, 1);
            if (cov > 0)
                plot(c, i, j, argb, (unsigned)(cov * 255.0f + 0.5f));
        }
        if (xc > xb && cov_mid > 0) {
            if (cov_mid >= 1.0f && (argb >> 24) == 255) {
                uint32_t *row = &c->px[j * c->w];
                for (i = xb; i < xc; i++)
                    row[i] = argb;
            } else {
                unsigned cv8 = (unsigned)(cov_mid * 255.0f + 0.5f);
                for (i = xb; i < xc; i++)
                    plot(c, i, j, argb, cv8);
            }
        }
        for (i = imax(xc, xb); i < xd; i++) {
            float d = outline_t > 0 ? rrect_outline_sdf(i + 0.5f, py, &k) : rrect_sdf(i + 0.5f, py, &k);
            float cov = clampf(0.5f - d, 0, 1);
            if (cov > 0)
                plot(c, i, j, argb, (unsigned)(cov * 255.0f + 0.5f));
        }
    }
}

void cv_rrect(canvas_t *c, float x, float y, float w, float h, float r, uint32_t argb)
{
    rrect_rows(c, x, y, w, h, r, 0, argb);
}

void cv_rrect_outline(canvas_t *c, float x, float y, float w, float h, float r, float thickness, uint32_t argb)
{
    rrect_rows(c, x, y, w, h, r, thickness, argb);
}

struct shadow_ctx { float cx, cy, hw, hh, r, blur; };

void cv_shadow(canvas_t *c, float x, float y, float w, float h, float r, float blur, uint32_t argb)
{
    struct shadow_ctx k = { x + w / 2, y + h / 2, w / 2, h / 2, r, blur };
    int x0 = imax((int)(x - blur), c->cx0), y0 = imax((int)(y - blur), c->cy0);
    int x1 = imin((int)(x + w + blur) + 1, c->cx1), y1 = imin((int)(y + h + blur) + 1, c->cy1);
    int i, j;

    for (j = y0; j < y1; j++) {
        bool mid_row = j > y + r + blur && j < y + h - r - blur;

        for (i = x0; i < x1; i++) {
            float d, t;
            /* Deep inside the card: covered later, skip without any maths. */
            if (mid_row && i > x + blur && i < x + w - blur)
                continue;
            d = sd_round_box(i + 0.5f, j + 0.5f, k.cx, k.cy, k.hw, k.hh, k.r);
            if (d < -blur)
                continue;
            t = clampf(1.0f - (d + blur * 0.25f) / blur, 0, 1);
            t = t * t;
            if (t > 0)
                plot(c, i, j, argb, (unsigned)(t * 255.0f));
        }
    }
}

struct circle_ctx { float cx, cy, r, t; };

static float circle_sdf(float x, float y, const void *p)
{
    const struct circle_ctx *k = (const struct circle_ctx *)p;
    float dx = x - k->cx, dy = y - k->cy;
    return sqrtf(dx * dx + dy * dy) - k->r;
}

static float ring_sdf(float x, float y, const void *p)
{
    const struct circle_ctx *k = (const struct circle_ctx *)p;
    float dx = x - k->cx, dy = y - k->cy;
    return fabsf(sqrtf(dx * dx + dy * dy) - k->r) - k->t * 0.5f;
}

void cv_circle(canvas_t *c, float cx, float cy, float radius, uint32_t argb)
{
    struct circle_ctx k = { cx, cy, radius, 0 };
    cv_sdf(c, cx - radius, cy - radius, radius * 2, radius * 2, circle_sdf, &k, argb);
}

void cv_ring(canvas_t *c, float cx, float cy, float radius, float thickness, uint32_t argb)
{
    struct circle_ctx k = { cx, cy, radius, thickness };
    float e = radius + thickness;
    cv_sdf(c, cx - e, cy - e, e * 2, e * 2, ring_sdf, &k, argb);
}

struct line_ctx { float ax, ay, bx, by, t; };

static float line_sdf(float x, float y, const void *p)
{
    const struct line_ctx *k = (const struct line_ctx *)p;
    return sd_segment(x, y, k->ax, k->ay, k->bx, k->by) - k->t * 0.5f;
}

void cv_line(canvas_t *c, float x0, float y0, float x1, float y1, float thickness, uint32_t argb)
{
    struct line_ctx k = { x0, y0, x1, y1, thickness };
    float minx = fminf(x0, x1) - thickness, miny = fminf(y0, y1) - thickness;
    float maxx = fmaxf(x0, x1) + thickness, maxy = fmaxf(y0, y1) + thickness;
    cv_sdf(c, minx, miny, maxx - minx, maxy - miny, line_sdf, &k, argb);
}

struct tri_ctx { float x[3], y[3]; };

static float tri_sdf(float px, float py, const void *p)
{
    const struct tri_ctx *k = (const struct tri_ctx *)p;
    float d = 1e9f, s = 0;
    int i;

    for (i = 0; i < 3; i++) {
        int j = (i + 1) % 3;
        float e = sd_segment(px, py, k->x[i], k->y[i], k->x[j], k->y[j]);
        float cross = (k->x[j] - k->x[i]) * (py - k->y[i]) - (k->y[j] - k->y[i]) * (px - k->x[i]);
        if (e < d)
            d = e;
        s += cross > 0 ? 1 : -1;
    }
    /* Inside when the point is on the same side of all three edges. */
    return (s == 3 || s == -3) ? -d : d;
}

void cv_triangle(canvas_t *c, float x0, float y0, float x1, float y1, float x2, float y2, uint32_t argb)
{
    struct tri_ctx k = { { x0, x1, x2 }, { y0, y1, y2 } };
    float minx = fminf(x0, fminf(x1, x2)), maxx = fmaxf(x0, fmaxf(x1, x2));
    float miny = fminf(y0, fminf(y1, y2)), maxy = fmaxf(y0, fmaxf(y1, y2));
    cv_sdf(c, minx, miny, maxx - minx, maxy - miny, tri_sdf, &k, argb);
}

/* ------------------------------------------------------------ bitmaps */

void cv_mask(canvas_t *c, int x, int y, const uint8_t *mask, int mw, int mh, int mstride, uint32_t argb)
{
    int i0 = imax(0, c->cx0 - x), j0 = imax(0, c->cy0 - y);
    int i1 = imin(mw, c->cx1 - x), j1 = imin(mh, c->cy1 - y);
    int i, j;

    for (j = j0; j < j1; j++) {
        const uint8_t *m = mask + j * mstride;
        for (i = i0; i < i1; i++)
            if (m[i])
                plot(c, x + i, y + j, argb, m[i]);
    }
}

void cv_image(canvas_t *c, const uint32_t *img, int iw, int ih, int x, int y, float radius, unsigned alpha)
{
    int i0 = imax(0, c->cx0 - x), j0 = imax(0, c->cy0 - y);
    int i1 = imin(iw, c->cx1 - x), j1 = imin(ih, c->cy1 - y);
    float hw = iw / 2.0f, hh = ih / 2.0f;
    int i, j;

    for (j = j0; j < j1; j++) {
        uint32_t *row = &c->px[(y + j) * c->w + x];
        const uint32_t *src = img + j * iw;
        bool corner_row = radius > 0 && (j < radius || j >= ih - radius);

        for (i = i0; i < i1; i++) {
            unsigned a = ((src[i] >> 24) * alpha) / 255;
            if (corner_row && (i < radius || i >= iw - radius)) {
                float d = sd_round_box(i + 0.5f, j + 0.5f, hw, hh, hw, hh, radius);
                a = (unsigned)(a * clampf(0.5f - d, 0, 1));
            }
            row[i] = cv_mix(row[i], src[i], a);
        }
    }
}

uint32_t *image_scale_cover(const uint32_t *src, int sw, int sh, int dw, int dh)
{
    uint32_t *dst = (uint32_t *)malloc((size_t)dw * dh * sizeof(uint32_t));
    float scale, ox, oy;
    int x, y;

    if (dst == NULL)
        return NULL;
    /* Scale so the image covers dw x dh, then centre-crop. */
    scale = fmaxf((float)dw / sw, (float)dh / sh);
    ox = (sw - dw / scale) / 2;
    oy = (sh - dh / scale) / 2;

    for (y = 0; y < dh; y++) {
        float sy = oy + (y + 0.5f) / scale - 0.5f;
        int y0 = (int)floorf(sy);
        float fy = sy - y0;
        int ya = y0 < 0 ? 0 : y0 >= sh ? sh - 1 : y0;
        int yb = y0 + 1 < 0 ? 0 : y0 + 1 >= sh ? sh - 1 : y0 + 1;

        for (x = 0; x < dw; x++) {
            float sx = ox + (x + 0.5f) / scale - 0.5f;
            int x0 = (int)floorf(sx);
            float fx = sx - x0;
            int xa = x0 < 0 ? 0 : x0 >= sw ? sw - 1 : x0;
            int xb = x0 + 1 < 0 ? 0 : x0 + 1 >= sw ? sw - 1 : x0 + 1;
            uint32_t p00 = src[ya * sw + xa], p01 = src[ya * sw + xb];
            uint32_t p10 = src[yb * sw + xa], p11 = src[yb * sw + xb];
            uint32_t out = 0;
            int ch;

            for (ch = 0; ch < 32; ch += 8) {
                float a = ((p00 >> ch) & 255) * (1 - fx) + ((p01 >> ch) & 255) * fx;
                float b = ((p10 >> ch) & 255) * (1 - fx) + ((p11 >> ch) & 255) * fx;
                out |= (uint32_t)(a * (1 - fy) + b * fy + 0.5f) << ch;
            }
            dst[y * dw + x] = out;
        }
    }
    return dst;
}
