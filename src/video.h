#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <Limelight.h>

enum video_renderer {
    RENDERER_GPU,          /* Xenos shader does YUV->RGB and scaling (default) */
    RENDERER_CPU,          /* lookup-table conversion on hardware thread 4 */
    RENDERER_CPU_HWCOLOR,  /* experimental: display colour matrix converts */
    RENDERER_COUNT
};

struct video_options {
    bool fast_decode;   /* skip the H.264 deblocking filter (big speedup) */
    int renderer;       /* enum video_renderer */
    bool vsync;         /* GPU renderer: flip on vertical blank (no tearing) */
};

const char *video_renderer_name(int renderer);
void video_set_options(const struct video_options *opts);
void video_init_callbacks(DECODER_RENDERER_CALLBACKS *cb);

/* Display geometry of the Xenos scanout surface (valid after console_init). */
void video_get_display_size(int *w, int *h);

struct video_stats {
    uint32_t received, decoded, rendered, dropped, decode_errors;
    uint32_t avg_decode_us, avg_render_us;
    int display_w, display_h;
    int decode_threads;     /* FFmpeg threads actually active */
    int renderer;           /* renderer actually used */
};
void video_get_stats(struct video_stats *out);
