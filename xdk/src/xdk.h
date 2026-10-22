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

/* Draws a textured rectangle (pixel coordinates) with one of the pixel shaders.
 * The caller sets the textures: RGB uses sampler 0, YUV uses 0 (Y), 1 (U), 2 (V). */
enum gfx_shader { GFX_SHADER_RGB, GFX_SHADER_YUV };
void gfx_draw_rect(D3DDevice *dev, int shader, float x0, float y0, float x1, float y1);

/* Copies rows into a locked linear texture. */
void gfx_upload(D3DTexture *tex, const void *src, int src_pitch, int row_bytes, int rows);
