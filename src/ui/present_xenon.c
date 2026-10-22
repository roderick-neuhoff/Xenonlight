/*
 * Shows the UI canvas: converts 0xAARRGGBB pixels to the scanout format
 * ((b<<24)|(g<<16)|(r<<8)) and scatters them into Xenos' 32x32-tiled
 * framebuffer, the one the text console also uses.
 */
#include "ui.h"

#include <stdint.h>
#include <stdlib.h>
#include <ppc/cache.h>

struct ati_info {
    uint32_t unknown1[4];
    uint32_t base;
    uint32_t unknown2[8];
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

static uint32_t *tile_x;
static int tile_pitch;

void ui_present(void)
{
    volatile struct ati_info *ai = (volatile struct ati_info *)0xec806100UL;
    canvas_t *c = ui_canvas();
    uint32_t *fb = (uint32_t *)(uintptr_t)(ai->base | 0x80000000);
    int fb_w = (int)ai->width, fb_h = (int)ai->height;
    int pitch = ((fb_w + 31) >> 5) << 5;
    int w = c->w < fb_w ? c->w : fb_w, h = c->h < fb_h ? c->h : fb_h;
    int x, y;

    if (tile_pitch != pitch) {
        free(tile_x);
        tile_x = malloc(sizeof(*tile_x) * pitch);
        if (tile_x == NULL) {
            tile_pitch = 0;
            return;
        }
        for (x = 0; x < pitch; x++)
            tile_x[x] = (uint32_t)(((x >> 5) << 10) + (x & 3) + (((x & 31) >> 2) << 3));
        tile_pitch = pitch;
    }

    for (y = 0; y < h; y++) {
        const uint32_t *src = c->px + y * c->w;
        uint32_t *row = fb + (y >> 5) * 32 * pitch + ((y & 1) << 2) + (((y & 31) >> 1) << 6);
        uint32_t xr = (uint32_t)((y & 8) << 2);

        for (x = 0; x < w; x++) {
            uint32_t v = src[x];
            row[tile_x[x] ^ xr] = ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v & 0xFF0000) >> 8);
        }
        if ((y & 31) == 31 || y == h - 1)
            memdcbst(fb + (y >> 5) * 32 * pitch, 32 * pitch * 4);
    }
}
