/*
 * Internal interfaces of the Xbox 360 SDK build (xdk/src/*.cpp).
 */
#pragma once

#include <xtl.h>

/*
 * Hardware thread layout while streaming (three cores, two threads each):
 *   0  UI, moonlight-common-c's network threads, controller input
 *   1-4  FFmpeg slice workers (fixed by XBMC-360's libavcodec/xb_thread.c)
 *   5  video presentation and XAudio2
 */
#define HW_THREAD_RENDER 5
#define HW_THREAD_ART    2   /* box art decoding; only runs outside a stream */

/* Output size of the UI and video; the console's scaler fits it to the TV. */
#define GFX_WIDTH  1280
#define GFX_HEIGHT 720

/* Decoder set-up and the VMX self-test; once at startup, after the network
 * (the self-test reports to the debug log). */
void video_xdk_init(void);

bool gfx_init(void);

/* Exclusive use of the D3D device from the calling thread (UI or video). */
D3DDevice *gfx_begin(void);
void gfx_end(void);

/* Pixel shaders. The caller sets textures and constants:
 *   RGB      s0 = RGB texture (UI)
 *   PRESENT  s0 = luma (point), s1/s2 = U/V (bilinear); c0 = luma size, c1.x = sharpening
 *   DEBLOCK  s0 = luma, s1 = macroblock QPs, s2 = threshold LUT (all point);
 *            c0 = luma size, c1 = 1/macroblock counts, c2 = edge direction  (xdk/shaders)
 *   OVERLAY  s0 = RGB texture, output alpha 0.85 (needs alpha blending on) */
enum gfx_shader { GFX_SHADER_RGB, GFX_SHADER_PRESENT, GFX_SHADER_DEBLOCK, GFX_SHADER_OVERLAY, GFX_SHADER_COUNT };

/* Draws a rectangle (pixel coordinates of the 1280x720 back buffer) with a pixel shader. */
void gfx_draw_rect(D3DDevice *dev, int shader, float x0, float y0, float x1, float y1);
/* Covers the whole current render target, whatever its size. */
void gfx_draw_target(D3DDevice *dev, int shader);
/* Bilinear or point sampling for a sampler. */
void gfx_set_filter(D3DDevice *dev, int sampler, bool linear);

/* Copies rows into a locked linear texture. */
void gfx_upload(D3DTexture *tex, const void *src, int src_pitch, int row_bytes, int rows);
