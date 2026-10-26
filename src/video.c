/*
 * H.264 software decode + presentation.
 *
 *   hw thread 0 (Moonlight)  --packet ring-->  hw 2: FFmpeg decode, with slice
 *                                               workers on hw 3, 5 and 1
 *   hw 2  --3-slot frame mailbox-->  hw 4: present the frame
 *
 * Only the decoder thread unrefs AVFrames; the renderer just marks slots
 * free, which keeps FFmpeg's buffer pools on one side of the handoff.
 *
 * Three ways to present (enum video_renderer):
 *  - GPU (default): planes uploaded as textures, a Xenos shader converts and
 *    scales (gpu.c);
 *  - CPU: lookup-table conversion straight into the tiled scanout buffer the
 *    text console uses, with a 2x2 fast path when the stream matches the
 *    display size;
 *  - CPU + hardware colour (experimental): raw Y/U/V bytes packed per pixel,
 *    converted by the display controller's colour matrix at scanout.
 */
#include "video.h"
#include "gpu.h"
#include "hw.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <ppc/atomic.h>
#include <ppc/cache.h>

#define PKT_SLOTS        8
#define FRAME_SLOTS      3
#define DECODE_THREADS   4   /* hw thread 2 + hwpthread workers on 3, 5, 1 */
#define SLICES_PER_FRAME 4

extern void xenos_write32(int reg, uint32_t val);
extern uint32_t xenos_read32(int reg);

struct pkt_slot {
    uint8_t *data;
    int cap;
    int len;
    int keyframe;
};

/* ---- shared between hardware threads ---- */
static struct pkt_slot pkts[PKT_SLOTS];
static volatile uint32_t pkt_w, pkt_r;

static AVFrame *frames[FRAME_SLOTS];
static unsigned int mailbox_lock;
static volatile int slot_pending = -1;     /* decoded, waiting for render */
static volatile int slot_rendering = -1;   /* owned by the renderer */

static volatile int running;
static volatile int need_idr;
static struct video_stats stats;

/* ---- decoder state (hw 2 only, set up on hw 0 before start) ---- */
static AVCodecContext *codec_ctx;
static AVPacket *av_pkt;
static AVFrame *dec_frame;
static struct video_options options = { .fast_decode = true, .renderer = RENDERER_GPU, .vsync = true };
static int active_renderer;

/* ---- display ---- */
struct ati_info {
    uint32_t unknown1[4];
    uint32_t base;
    uint32_t unknown2[8];
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

static uint32_t *fb;
static int fb_w, fb_h, fb_pitch;   /* fb_pitch: width rounded up to 32 */
static int vid_w, vid_h;
static int rect_x0, rect_y0, rect_x1, rect_y1;
static bool one_to_one;            /* stream exactly fills the display */
static bool hw_color;              /* colour matrix programmed for packed YUV */
static uint32_t *tile_x;           /* B(x) term of the tiling formula */
static uint16_t *src_x, *src_y;    /* nearest-neighbour scale maps */

static int32_t lut_y[256], lut_rv[256], lut_gu[256], lut_gv[256], lut_bu[256];
/* Clamp tables pre-shifted into the framebuffer's (b<<24)|(g<<16)|(r<<8) layout. */
static uint32_t clamp_r[1024], clamp_g[1024], clamp_b[1024];

#define CSC_REG_FIRST 0x6380   /* D1GRPH_COLOR_MATRIX_TRANSFORMATION_CNTL */
#define CSC_REG_COUNT 14       /* CNTL, 12 coefficients, 0x63b4 */
static uint32_t saved_csc[CSC_REG_COUNT];
static bool csc_saved;

const char *video_renderer_name(int renderer)
{
    switch (renderer) {
    case RENDERER_GPU:         return "GPU";
    case RENDERER_CPU:         return "CPU";
    case RENDERER_CPU_HWCOLOR: return "CPU + display colour matrix (experimental)";
    default:                   return "?";
    }
}

void video_set_options(const struct video_options *opts)
{
    options = *opts;
}

void video_get_display_size(int *w, int *h)
{
    volatile struct ati_info *ai = (volatile struct ati_info *)0xec806100UL;

    *w = (int)ai->width;
    *h = (int)ai->height;
}

void video_get_stats(struct video_stats *out)
{
    *out = stats;
}

bool video_has_overlay(void)
{
    return false;
}

void video_set_overlay(const uint32_t *px, int w, int h)
{
    (void)px;
    (void)w;
    (void)h;
}

static void build_color_luts(void)
{
    int i;

    /* BT.601 limited range, 8.8 fixed point. */
    for (i = 0; i < 256; i++) {
        lut_y[i] = 298 * (i - 16) + 128;
        lut_rv[i] = 409 * (i - 128);
        lut_gu[i] = -100 * (i - 128);
        lut_gv[i] = -208 * (i - 128);
        lut_bu[i] = 516 * (i - 128);
    }
    for (i = 0; i < 1024; i++) {
        int v = i - 512;
        uint32_t c = v < 0 ? 0 : v > 255 ? 255 : (uint32_t)v;
        clamp_r[i] = c << 8;
        clamp_g[i] = c << 16;
        clamp_b[i] = c << 24;
    }
}

/* ------------------------------------------------- display colour matrix */

/* Coefficients: 1.0 = 2^24, offsets: 1.0 = 2^26; negatives are 27-bit two's
 * complement with bit 31 set (derived from the values libxenon programs). */
static uint32_t csc_encode(double v, double one)
{
    long long i = llround(v * one);

    if (i >= 0)
        return (uint32_t)i & 0x07FFFFFF;
    return 0x80000000u | ((uint32_t)i & 0x07FFFFFF);
}

static double csc_decode(uint32_t r, double one)
{
    int32_t v = (int32_t)(r & 0x07FFFFFF);

    if (r & 0x80000000u)
        v -= 0x08000000;
    return v / one;
}

static void csc_restore(void)
{
    int i;

    if (!csc_saved)
        return;
    for (i = 0; i < CSC_REG_COUNT; i++)
        xenos_write32(CSC_REG_FIRST + i * 4, saved_csc[i]);
    csc_saved = false;
}

/*
 * Programs the display colour matrix so that a pixel stored as
 * (V<<24)|(U<<16)|(Y<<8) is shown as the right colour. Matrix rows are the
 * output channels, columns the input bytes in R,G,B order (= Y,U,V here).
 * Returns false (leaving the hardware untouched) for unrecognised output modes.
 */
static bool csc_program_yuv(void)
{
    /* BT.601 limited-range YCbCr -> full-range RGB, inputs/outputs in 0..1. */
    static const double a[3][3] = {
        { 1.16438,  0.00000,  1.59603 },
        { 1.16438, -0.39176, -0.81297 },
        { 1.16438,  2.01723,  0.00000 },
    };
    static const double b[3] = { -0.874202, 0.531668, -1.08563 };
    double m[3][3], off[3];
    uint32_t coef11;
    int i, r, c;

    for (i = 0; i < CSC_REG_COUNT; i++)
        saved_csc[i] = xenos_read32(CSC_REG_FIRST + i * 4);
    csc_saved = true;
    coef11 = saved_csc[1];

    if (coef11 == 0x00702000) {
        /* Component (YCbCr) output: the hardware wants Cr,Y,Cb out, which
         * is our input already, just reordered. */
        memset(m, 0, sizeof(m));
        m[0][2] = 1.0;   /* Cr <- V */
        m[1][0] = 1.0;   /* Y  <- Y */
        m[2][1] = 1.0;   /* Cb <- U */
        off[0] = off[1] = off[2] = 0.0;
    } else if (saved_csc[2] == 0 && saved_csc[3] == 0 && saved_csc[5] == 0) {
        /* RGB output with libxenon's diagonal scale + offset (limited-range
         * RGB for HDMI): compose it after our YUV->RGB conversion. */
        double s = csc_decode(coef11, 16777216.0);
        double o = csc_decode(saved_csc[4], 67108864.0);

        for (r = 0; r < 3; r++) {
            for (c = 0; c < 3; c++)
                m[r][c] = s * a[r][c];
            off[r] = s * b[r] + o;
        }
    } else {
        csc_saved = false;
        return false;
    }

    xenos_write32(CSC_REG_FIRST, 1);
    for (r = 0; r < 3; r++) {
        for (c = 0; c < 3; c++)
            xenos_write32(CSC_REG_FIRST + 4 + (r * 4 + c) * 4, csc_encode(m[r][c], 16777216.0));
        xenos_write32(CSC_REG_FIRST + 4 + (r * 4 + 3) * 4, csc_encode(off[r], 67108864.0));
    }
    xenos_write32(CSC_REG_FIRST + 13 * 4, 0);
    return true;
}

/* ------------------------------------------------------------ display */

/* Tiled framebuffer index of row y (A(y) term); add tile_x[x] ^ ((y&8)<<2). */
static inline uint32_t tile_row(int y)
{
    return (uint32_t)((y >> 5) * 32 * fb_pitch + ((y & 1) << 2) + (((y & 31) >> 1) << 6));
}

static bool setup_display(int w, int h)
{
    volatile struct ati_info *ai = (volatile struct ati_info *)0xec806100UL;
    int x, y, dw, dh, rows;
    uint32_t black;

    fb = (uint32_t *)(uintptr_t)(ai->base | 0x80000000);
    fb_w = (int)ai->width;
    fb_h = (int)ai->height;
    fb_pitch = ((fb_w + 31) >> 5) << 5;
    vid_w = w;
    vid_h = h;

    /* Fit the stream inside the display, preserving aspect ratio. */
    if ((int64_t)fb_w * h > (int64_t)fb_h * w) {
        dh = fb_h;
        dw = (int)((int64_t)fb_h * w / h);
    } else {
        dw = fb_w;
        dh = (int)((int64_t)fb_w * h / w);
    }
    rect_x0 = (fb_w - dw) / 2;
    rect_y0 = (fb_h - dh) / 2;
    rect_x1 = rect_x0 + dw;
    rect_y1 = rect_y0 + dh;
    one_to_one = dw == w && dh == h && rect_x0 == 0 && rect_y0 == 0 && (w & 1) == 0 && (h & 1) == 0;

    free(tile_x);
    free(src_x);
    free(src_y);
    tile_x = malloc(sizeof(*tile_x) * fb_pitch);
    src_x = malloc(sizeof(*src_x) * fb_pitch);
    src_y = malloc(sizeof(*src_y) * (((fb_h + 31) >> 5) << 5));
    if (!tile_x || !src_x || !src_y)
        return false;

    for (x = 0; x < fb_pitch; x++) {
        tile_x[x] = (uint32_t)(((x >> 5) << 10) + (x & 3) + (((x & 31) >> 2) << 3));
        src_x[x] = (x >= rect_x0 && x < rect_x1) ? (uint16_t)((int64_t)(x - rect_x0) * w / dw) : 0;
    }
    for (y = 0; y < fb_h; y++)
        src_y[y] = (y >= rect_y0 && y < rect_y1) ? (uint16_t)((int64_t)(y - rect_y0) * h / dh) : 0;

    hw_color = options.renderer == RENDERER_CPU_HWCOLOR && csc_program_yuv();

    /* Black out the surface once; letterbox bars stay black. In packed-YUV
     * mode black is Y=16, U=V=128. */
    black = hw_color ? ((128u << 24) | (128u << 16) | (16u << 8)) : 0;
    rows = ((fb_h + 31) >> 5) << 5;
    for (x = 0; x < fb_pitch * rows; x++)
        fb[x] = black;
    memdcbst(fb, fb_pitch * rows * 4);
    return true;
}

/* Any scale, one pixel at a time. */
static void render_rows_scaled(const AVFrame *f, int y0, int y1)
{
    const int ls0 = f->linesize[0], ls1 = f->linesize[1], ls2 = f->linesize[2];
    int y, x;

    for (y = y0; y < y1; y++) {
        int sy = src_y[y];
        const uint8_t *yr = f->data[0] + sy * ls0;
        const uint8_t *ur = f->data[1] + (sy >> 1) * ls1;
        const uint8_t *vr = f->data[2] + (sy >> 1) * ls2;
        uint32_t *row = fb + tile_row(y);
        uint32_t xr = (uint32_t)((y & 8) << 2);

        if (hw_color) {
            for (x = rect_x0; x < rect_x1; x++) {
                int sx = src_x[x];
                row[tile_x[x] ^ xr] = ((uint32_t)vr[sx >> 1] << 24) | ((uint32_t)ur[sx >> 1] << 16) |
                                      ((uint32_t)yr[sx] << 8);
            }
            continue;
        }
        for (x = rect_x0; x < rect_x1; x++) {
            int sx = src_x[x];
            int Y = lut_y[yr[sx]];
            int U = ur[sx >> 1], V = vr[sx >> 1];

            row[tile_x[x] ^ xr] = clamp_r[((Y + lut_rv[V]) >> 8) + 512] |
                                  clamp_g[((Y + lut_gu[U] + lut_gv[V]) >> 8) + 512] |
                                  clamp_b[((Y + lut_bu[U]) >> 8) + 512];
        }
    }
}

/* Stream == display: 2x2 blocks share one chroma sample (4:2:0). */
static void render_rows_1to1(const AVFrame *f, int y0, int y1)
{
    const int ls0 = f->linesize[0], ls1 = f->linesize[1], ls2 = f->linesize[2];
    int y, x;

    for (y = y0; y < y1; y += 2) {
        const uint8_t *ya = f->data[0] + y * ls0;
        const uint8_t *yb = ya + ls0;
        const uint8_t *ur = f->data[1] + (y >> 1) * ls1;
        const uint8_t *vr = f->data[2] + (y >> 1) * ls2;
        uint32_t *ra = fb + tile_row(y), *rb = fb + tile_row(y + 1);
        uint32_t xa = (uint32_t)((y & 8) << 2), xb = (uint32_t)(((y + 1) & 8) << 2);

        if (hw_color) {
            for (x = 0; x < vid_w; x += 2) {
                uint32_t uv = ((uint32_t)vr[x >> 1] << 24) | ((uint32_t)ur[x >> 1] << 16);
                uint32_t t0 = tile_x[x], t1 = tile_x[x + 1];

                ra[t0 ^ xa] = uv | ((uint32_t)ya[x] << 8);
                ra[t1 ^ xa] = uv | ((uint32_t)ya[x + 1] << 8);
                rb[t0 ^ xb] = uv | ((uint32_t)yb[x] << 8);
                rb[t1 ^ xb] = uv | ((uint32_t)yb[x + 1] << 8);
            }
            continue;
        }
        for (x = 0; x < vid_w; x += 2) {
            int U = ur[x >> 1], V = vr[x >> 1];
            int cr = lut_rv[V] + (512 << 8);
            int cg = lut_gu[U] + lut_gv[V] + (512 << 8);
            int cb = lut_bu[U] + (512 << 8);
            uint32_t t0 = tile_x[x], t1 = tile_x[x + 1];
            int Y;

            Y = lut_y[ya[x]];
            ra[t0 ^ xa] = clamp_r[(Y + cr) >> 8] | clamp_g[(Y + cg) >> 8] | clamp_b[(Y + cb) >> 8];
            Y = lut_y[ya[x + 1]];
            ra[t1 ^ xa] = clamp_r[(Y + cr) >> 8] | clamp_g[(Y + cg) >> 8] | clamp_b[(Y + cb) >> 8];
            Y = lut_y[yb[x]];
            rb[t0 ^ xb] = clamp_r[(Y + cr) >> 8] | clamp_g[(Y + cg) >> 8] | clamp_b[(Y + cb) >> 8];
            Y = lut_y[yb[x + 1]];
            rb[t1 ^ xb] = clamp_r[(Y + cr) >> 8] | clamp_g[(Y + cg) >> 8] | clamp_b[(Y + cb) >> 8];
        }
    }
}

static void render_frame(const AVFrame *f)
{
    int band, first = rect_y0 >> 5, last = (rect_y1 + 31) >> 5;

    for (band = first; band < last; band++) {
        int y0 = band * 32, y1 = y0 + 32;

        if (y0 < rect_y0)
            y0 = rect_y0;
        if (y1 > rect_y1)
            y1 = rect_y1;
        if (one_to_one)
            render_rows_1to1(f, y0, y1);
        else
            render_rows_scaled(f, y0, y1);
        memdcbst(fb + band * 32 * fb_pitch, 32 * fb_pitch * 4);
    }
}

/* ---------------------------------------------------- hw thread tasks */

static void decoder_publish(void)
{
    int s;

    lock(&mailbox_lock);
    /* Pick a slot that is neither queued nor being drawn. */
    for (s = 0; s < FRAME_SLOTS; s++)
        if (s != slot_pending && s != slot_rendering)
            break;
    unlock(&mailbox_lock);

    av_frame_unref(frames[s]);
    av_frame_move_ref(frames[s], dec_frame);

    lock(&mailbox_lock);
    slot_pending = s;   /* an unrendered older frame is simply superseded */
    unlock(&mailbox_lock);
}

static void decoder_task(void)
{
    while (running) {
        struct pkt_slot *p;
        uint64_t t0;
        int ret;

        if (pkt_r == pkt_w) {
            hw_relax();
            continue;
        }
        hw_lwsync();
        p = &pkts[pkt_r % PKT_SLOTS];

        av_pkt->data = p->data;
        av_pkt->size = p->len;
        av_pkt->flags = p->keyframe ? AV_PKT_FLAG_KEY : 0;

        t0 = hw_now_us();
        ret = avcodec_send_packet(codec_ctx, av_pkt);   /* copies the data */
        hw_lwsync();
        pkt_r++;

        if (ret < 0) {
            stats.decode_errors++;
            need_idr = 1;
            continue;
        }
        while (avcodec_receive_frame(codec_ctx, dec_frame) == 0) {
            stats.decoded++;
            stats.avg_decode_us = (stats.avg_decode_us * 15 + (uint32_t)(hw_now_us() - t0)) / 16;
            if (dec_frame->format != AV_PIX_FMT_YUV420P && dec_frame->format != AV_PIX_FMT_YUVJ420P) {
                av_frame_unref(dec_frame);
                stats.decode_errors++;
                continue;
            }
            decoder_publish();
        }
    }
}

static void render_task(void)
{
    while (running) {
        int s;
        uint64_t t0;

        lock(&mailbox_lock);
        s = slot_pending;
        if (s >= 0) {
            slot_pending = -1;
            slot_rendering = s;
        }
        unlock(&mailbox_lock);

        if (s < 0) {
            hw_relax();
            continue;
        }

        t0 = hw_now_us();
        if (active_renderer == RENDERER_GPU)
            gpu_present(frames[s], options.vsync);
        else
            render_frame(frames[s]);
        stats.rendered++;
        stats.avg_render_us = (stats.avg_render_us * 15 + (uint32_t)(hw_now_us() - t0)) / 16;

        lock(&mailbox_lock);
        slot_rendering = -1;
        unlock(&mailbox_lock);
    }
}

/* ---------------------------------------------------- Moonlight callbacks */

static int video_setup(int format, int width, int height, int fps, void *ctx, int dr_flags)
{
    const AVCodec *codec;
    int i;
    (void)fps;
    (void)ctx;
    (void)dr_flags;

    if (!(format & VIDEO_FORMAT_MASK_H264)) {
        printf("video: unsupported format 0x%x (only H.264 is supported)\n", format);
        return -1;
    }

    /* The console isn't safe to use from the decoder's hardware thread. */
    av_log_set_level(AV_LOG_QUIET);

    memset(&stats, 0, sizeof(stats));
    video_get_display_size(&stats.display_w, &stats.display_h);

    /* GPU presentation if it initialised; otherwise fall back to the CPU. */
    active_renderer = options.renderer;
    if (active_renderer == RENDERER_GPU && !gpu_stream_begin(width, height))
        active_renderer = RENDERER_CPU;
    if (active_renderer != RENDERER_GPU) {
        build_color_luts();
        if (!setup_display(width, height)) {
            printf("video: out of memory\n");
            return -1;
        }
        if (active_renderer == RENDERER_CPU_HWCOLOR && !hw_color)
            active_renderer = RENDERER_CPU;   /* unrecognised output mode */
    }
    stats.renderer = active_renderer;

    codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (codec == NULL) {
        printf("video: FFmpeg was built without the H.264 decoder\n");
        return -1;
    }
    codec_ctx = avcodec_alloc_context3(codec);
    if (codec_ctx == NULL)
        return -1;
    codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    codec_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    /*
     * The host encodes SLICES_PER_FRAME slices per frame (see capabilities);
     * FFmpeg decodes them in parallel on this thread plus hwpthread workers.
     * Slice threading adds no latency.
     */
    codec_ctx->thread_type = FF_THREAD_SLICE;
    codec_ctx->thread_count = DECODE_THREADS;
    codec_ctx->width = width;
    codec_ctx->height = height;
    codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    if (options.fast_decode)
        codec_ctx->skip_loop_filter = AVDISCARD_ALL;

    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        printf("video: avcodec_open2 failed\n");
        avcodec_free_context(&codec_ctx);
        return -1;
    }
    stats.decode_threads = codec_ctx->active_thread_type == FF_THREAD_SLICE ? codec_ctx->thread_count : 1;

    av_pkt = av_packet_alloc();
    dec_frame = av_frame_alloc();
    for (i = 0; i < FRAME_SLOTS; i++)
        frames[i] = av_frame_alloc();
    for (i = 0; i < PKT_SLOTS; i++) {
        pkts[i].cap = 512 * 1024;
        pkts[i].data = malloc(pkts[i].cap + AV_INPUT_BUFFER_PADDING_SIZE);
        pkts[i].len = 0;
        if (pkts[i].data == NULL)
            return -1;
    }

    pkt_w = pkt_r = 0;
    slot_pending = slot_rendering = -1;
    need_idr = 0;
    return 0;
}

static void video_start(void)
{
    running = 1;
    hw_sync();
    hw_start(HW_THREAD_DECODE, decoder_task);
    hw_start(HW_THREAD_RENDER, render_task);
}

static void video_stop(void)
{
    running = 0;
    hw_sync();
    while (hw_running(HW_THREAD_DECODE) || hw_running(HW_THREAD_RENDER))
        ;
}

static void video_cleanup(void)
{
    int i;

    for (i = 0; i < FRAME_SLOTS; i++)
        av_frame_free(&frames[i]);
    av_frame_free(&dec_frame);
    av_packet_free(&av_pkt);
    avcodec_free_context(&codec_ctx);
    for (i = 0; i < PKT_SLOTS; i++) {
        free(pkts[i].data);
        pkts[i].data = NULL;
    }
    /* Give the text console its framebuffer and normal colours back. */
    if (active_renderer == RENDERER_GPU)
        gpu_stream_end();
    csc_restore();
    hw_color = false;
}

static int video_submit(PDECODE_UNIT du)
{
    struct pkt_slot *p;
    PLENTRY e;
    int off = 0;

    stats.received++;
    stats.received_bytes += du->fullLength;

    if (need_idr) {
        need_idr = 0;
        return DR_NEED_IDR;
    }
    if (pkt_w - pkt_r >= PKT_SLOTS) {
        /* Decoder is behind. Dropping breaks the reference chain, so ask for an IDR. */
        stats.dropped++;
        return DR_NEED_IDR;
    }

    p = &pkts[pkt_w % PKT_SLOTS];
    if (du->fullLength > p->cap) {
        uint8_t *n = realloc(p->data, du->fullLength + AV_INPUT_BUFFER_PADDING_SIZE);
        if (n == NULL) {
            stats.dropped++;
            return DR_NEED_IDR;
        }
        p->data = n;
        p->cap = du->fullLength;
    }

    for (e = du->bufferList; e != NULL; e = e->next) {
        memcpy(p->data + off, e->data, e->length);
        off += e->length;
    }
    memset(p->data + off, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    p->len = off;
    p->keyframe = du->frameType == FRAME_TYPE_IDR;

    hw_sync();
    pkt_w++;
    return DR_OK;
}

void video_init_callbacks(DECODER_RENDERER_CALLBACKS *cb)
{
    LiInitializeVideoCallbacks(cb);
    cb->setup = video_setup;
    cb->start = video_start;
    cb->stop = video_stop;
    cb->cleanup = video_cleanup;
    cb->submitDecodeUnit = video_submit;
    /* Submit runs on Moonlight's receive coroutine; it only copies into the ring. */
    cb->capabilities = CAPABILITY_DIRECT_SUBMIT | CAPABILITY_SLICES_PER_FRAME(SLICES_PER_FRAME);
}
