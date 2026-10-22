/*
 * Minimal 2D software renderer for the UI. Pixels are 0xAARRGGBB in native
 * byte order; the canvas itself is opaque (destination alpha is ignored).
 * Shapes are drawn from signed distance functions with 1px anti-aliasing.
 * Platform independent: the same code renders the PC preview images.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ARGB(a, r, g, b) (((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define RGB(r, g, b)     ARGB(255, r, g, b)
#define HEX(v)           (0xFF000000u | (uint32_t)(v))
#define WITH_ALPHA(c, a) (((c) & 0x00FFFFFFu) | ((uint32_t)(a) << 24))

typedef struct {
    uint32_t *px;
    int w, h;              /* pixels; stride == w */
    int cx0, cy0, cx1, cy1; /* clip rectangle, [x0,x1) x [y0,y1) */
} canvas_t;

/* Signed distance (pixels, negative inside) of point (x, y) to a shape. */
typedef float (*sdf_fn)(float x, float y, const void *ctx);

bool cv_init(canvas_t *c, int w, int h);
void cv_free(canvas_t *c);
void cv_clip(canvas_t *c, int x, int y, int w, int h);
void cv_unclip(canvas_t *c);

uint32_t cv_mix(uint32_t dst, uint32_t src, unsigned alpha);

void cv_copy(canvas_t *dst, const canvas_t *src);
void cv_fill(canvas_t *c, int x, int y, int w, int h, uint32_t argb);
void cv_vgradient(canvas_t *c, int x, int y, int w, int h, uint32_t top, uint32_t bottom);

void cv_rrect(canvas_t *c, float x, float y, float w, float h, float r, uint32_t argb);
void cv_rrect_outline(canvas_t *c, float x, float y, float w, float h, float r, float thickness, uint32_t argb);
/* Soft drop shadow around a rounded rectangle. */
void cv_shadow(canvas_t *c, float x, float y, float w, float h, float r, float blur, uint32_t argb);

void cv_circle(canvas_t *c, float cx, float cy, float radius, uint32_t argb);
void cv_ring(canvas_t *c, float cx, float cy, float radius, float thickness, uint32_t argb);
void cv_line(canvas_t *c, float x0, float y0, float x1, float y1, float thickness, uint32_t argb);
void cv_triangle(canvas_t *c, float x0, float y0, float x1, float y1, float x2, float y2, uint32_t argb);

/* Fills the bounding box using an arbitrary distance function. */
void cv_sdf(canvas_t *c, float bx, float by, float bw, float bh, sdf_fn fn, const void *ctx, uint32_t argb);

/* 8-bit coverage mask (e.g. a glyph) tinted with argb. */
void cv_mask(canvas_t *c, int x, int y, const uint8_t *mask, int mw, int mh, int mstride, uint32_t argb);

/* Pre-scaled image (iw x ih, ARGB) at (x, y), optional rounded corners. */
void cv_image(canvas_t *c, const uint32_t *img, int iw, int ih, int x, int y, float radius, unsigned alpha);

/* Scales src to exactly dw x dh, cropping to fill ("cover"), bilinear. */
uint32_t *image_scale_cover(const uint32_t *src, int sw, int sh, int dw, int dh);

/* Distance helpers usable from custom sdf_fn callbacks. */
float sd_round_box(float px, float py, float cx, float cy, float hw, float hh, float r);
float sd_segment(float px, float py, float ax, float ay, float bx, float by);
