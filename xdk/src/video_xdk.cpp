/*
 * H.264 decoding with XBMC-360's FFmpeg 1.2 and presentation with Direct3D.
 *
 * Moonlight's decoder thread calls video_submit() for each frame, which
 * decodes it (slices in parallel on hardware threads 1-4) and copies the
 * picture into a staging slot. The render thread (hardware thread 5) uploads
 * the newest slot into Y/U/V textures and draws them with the YUV shader, so
 * waiting for the GPU or v-sync never delays decoding.
 *
 * Also here: the VMX self-test at startup, a recording of the first frames
 * of a stream (moonlight\capture.bin), and the decoder benchmark that replays
 * that recording with and without the VMX code.
 */
#include "xdk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/config.h"
#include "../../src/input.h"
#include "../../src/platform.h"
#include "../../src/video.h"
#include "../../src/ui/ui.h"
#include "../ffmpeg_xbox/xbox_dsp.h"
#include "deblock_tables.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/log.h>
extern AVCodec ff_h264_decoder;

/* XBMC-360's static pthreads-win32 (FFmpeg's worker threads) doesn't set
 * itself up: until this runs, its global critical sections are zero-filled
 * (which reads as "locked", so the first pthread_create waits forever). */
int ptw32_processInitialize(void);
}

#define FRAME_SLOTS      3
/*
 * Slice threads = slices per frame (FFmpeg decodes slices in batches of one per
 * thread, so extra slices wouldn't run in parallel). xb_thread.c puts worker i
 * on hardware thread (i + 1) % 6: 4 threads use 1-4; 5 adds thread 5, which
 * otherwise only presents frames and mixes audio; 6 adds thread 0 (network,
 * audio decode, this decoder thread, which waits while the workers run).
 */
#define MIN_THREADS 4
#define MAX_THREADS_HW 6
#define STATS_INTERVAL_MS 2000

#define CAPTURE_FILE     "capture.bin"
#define CAPTURE_FRAMES   600                 /* 20 s at 30 FPS */
#define CAPTURE_MAX      (40 * 1024 * 1024)

struct staging {
    uint8_t *plane[3];
    uint8_t *qp;            /* QP of each macroblock, for GPU deblocking */
};

static struct video_options options = { true, RENDERER_GPU, true, 1, 5 };
static struct video_stats stats;

static AVCodecContext *codec_ctx;
static AVFrame *dec_frame;
static uint8_t *pkt_buf;
static int pkt_cap;

static int vid_w, vid_h, chroma_w, chroma_h;
static struct staging slots[FRAME_SLOTS];
static CRITICAL_SECTION mailbox_lock;
static int slot_pending = -1, slot_rendering = -1;
static HANDLE frame_event, render_thread;
static volatile bool running;

/* Two texture sets: the GPU reads one while the next frame fills the other. */
static D3DTexture *tex[2][3];
static D3DTexture *qp_tex[2];
static DWORD tex_fence[2];
static int tex_next;

/*
 * GPU deblocking (when the decoder skips the in-loop filter): two passes over the luma plane
 * into an EDRAM render target, each resolved into a texture; the presentation shader then reads
 * the result. The render target may share EDRAM with the back buffer: it's only used before the
 * back buffer is drawn each frame.
 */
static int mb_w, mb_h;
static bool gpu_deblock;
static float sharpen;
static D3DSurface *deblock_rt, *back_buffer;
static D3DTexture *deblock_tex[2];
static D3DTexture *lut_tex;

static LARGE_INTEGER qpc_freq;

/* Periodic stats in the debug log. */
static DWORD stats_last_ms;
static struct video_stats stats_last;
static uint64_t decode_us_sum;

/* Recording of the stream for the benchmark: "MLC1", width, height, count,
 * then per frame a 32-bit length and the H.264 access unit. */
static uint8_t *cap_buf;
static uint32_t cap_len, cap_frames;

static void vlog(const char *fmt, ...)
{
    char line[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    plat_log(line);
}

static uint64_t now_us(void)
{
    LARGE_INTEGER t;

    QueryPerformanceCounter(&t);
    return (uint64_t)(t.QuadPart * 1000000 / qpc_freq.QuadPart);
}

const char *video_renderer_name(int renderer)
{
    (void)renderer;
    return "GPU";
}

void video_set_options(const struct video_options *opts)
{
    options = *opts;
}

static int decode_threads(void)
{
    if (options.decode_threads < MIN_THREADS || options.decode_threads > MAX_THREADS_HW)
        return 5;
    return options.decode_threads;
}

void video_get_display_size(int *w, int *h)
{
    *w = GFX_WIDTH;
    *h = GFX_HEIGHT;
}

void video_get_stats(struct video_stats *out)
{
    *out = stats;
}

/* ---------------------------------------------------- performance overlay */

/* The UI thread hands over pixels; the render thread uploads them into one of
 * two textures (the GPU may still be reading the other) and blends it over
 * each frame. */
#define OVERLAY_X 48   /* inside the TV's title-safe area */
#define OVERLAY_Y 36

static CRITICAL_SECTION overlay_lock;
static uint32_t *ov_px;
static int ov_w, ov_h, ov_cap;
static bool ov_visible, ov_dirty;
static D3DTexture *ov_tex[2];
static int ov_tex_w[2], ov_tex_h[2];
static DWORD ov_fence[2];
static int ov_cur = -1;   /* texture holding the current pixels */

bool video_has_overlay(void)
{
    return true;
}

void video_set_overlay(const uint32_t *px, int w, int h)
{
    EnterCriticalSection(&overlay_lock);
    if (px == NULL || w <= 0 || h <= 0) {
        ov_visible = false;
    } else {
        if (w * h > ov_cap) {
            uint32_t *n = (uint32_t *)realloc(ov_px, (size_t)w * h * 4);
            if (n == NULL) {
                LeaveCriticalSection(&overlay_lock);
                return;
            }
            ov_px = n;
            ov_cap = w * h;
        }
        memcpy(ov_px, px, (size_t)w * h * 4);
        ov_w = w;
        ov_h = h;
        ov_visible = ov_dirty = true;
    }
    LeaveCriticalSection(&overlay_lock);
}

/* Render thread, device held, back buffer bound. */
static void draw_overlay(D3DDevice *d)
{
    int w, h;

    EnterCriticalSection(&overlay_lock);
    if (!ov_visible) {
        LeaveCriticalSection(&overlay_lock);
        return;
    }
    if (ov_dirty) {
        int k = ov_cur < 0 ? 0 : ov_cur ^ 1;

        if (ov_fence[k])
            d->BlockOnFence(ov_fence[k]);
        if (ov_tex[k] && (ov_tex_w[k] != ov_w || ov_tex_h[k] != ov_h)) {
            ov_tex[k]->Release();
            ov_tex[k] = NULL;
        }
        if (ov_tex[k] == NULL &&
            SUCCEEDED(d->CreateTexture(ov_w, ov_h, 1, 0, D3DFMT_LIN_A8R8G8B8, 0, &ov_tex[k], NULL))) {
            ov_tex_w[k] = ov_w;
            ov_tex_h[k] = ov_h;
        }
        if (ov_tex[k]) {
            gfx_upload(ov_tex[k], ov_px, ov_w * 4, ov_w * 4, ov_h);
            ov_cur = k;
        }
        ov_dirty = false;
    }
    LeaveCriticalSection(&overlay_lock);
    if (ov_cur < 0)
        return;

    w = ov_tex_w[ov_cur];
    h = ov_tex_h[ov_cur];
    d->SetTexture(0, ov_tex[ov_cur]);
    gfx_set_filter(d, 0, false);   /* 1:1 pixels */
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    gfx_draw_rect(d, GFX_SHADER_OVERLAY, (float)OVERLAY_X, (float)OVERLAY_Y, (float)(OVERLAY_X + w),
                  (float)(OVERLAY_Y + h));
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    d->SetTexture(0, NULL);
    ov_fence[ov_cur] = d->InsertFence();
}

/* With the device held: the textures belong to the stream's lifetime. */
static void free_overlay(D3DDevice *d)
{
    int k;

    for (k = 0; k < 2; k++) {
        if (ov_tex[k]) {
            if (ov_fence[k])
                d->BlockOnFence(ov_fence[k]);
            ov_tex[k]->Release();
        }
        ov_tex[k] = NULL;
        ov_fence[k] = 0;
    }
    ov_cur = -1;
    EnterCriticalSection(&overlay_lock);
    ov_dirty = ov_visible;   /* re-upload if it's still meant to show */
    LeaveCriticalSection(&overlay_lock);
}

static void selftest_log(const char *line)
{
    plat_log(line);
}

void video_xdk_init(void)
{
    ptw32_processInitialize();
    avcodec_register(&ff_h264_decoder);
    av_log_set_level(AV_LOG_QUIET);
    InitializeCriticalSection(&mailbox_lock);
    InitializeCriticalSection(&overlay_lock);
    frame_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    QueryPerformanceFrequency(&qpc_freq);

    /* Only VMX functions that match FFmpeg's C code exactly get installed. */
    ff_xbox_dsp_selftest(selftest_log);
    ff_xbox_vmx_flags = XVMX_ALL;
}

static AVCodecContext *open_h264(int width, int height, int threads, bool fast_decode)
{
    AVCodecContext *c = avcodec_alloc_context3(&ff_h264_decoder);

    if (c == NULL)
        return NULL;
    c->flags |= CODEC_FLAG_LOW_DELAY;
    c->flags2 |= CODEC_FLAG2_FAST;
    /* The host sends one slice per thread (see capabilities), which the
     * worker threads decode in parallel. No added latency. */
    c->thread_type = FF_THREAD_SLICE;
    c->thread_count = threads;
    c->width = width;
    c->height = height;
    c->pix_fmt = AV_PIX_FMT_YUV420P;
    if (fast_decode)
        c->skip_loop_filter = AVDISCARD_ALL;
    if (avcodec_open2(c, &ff_h264_decoder, NULL) < 0) {
        av_free(c);
        return NULL;
    }
    return c;
}

static void close_h264(AVCodecContext **c)
{
    if (*c) {
        avcodec_close(*c);
        av_free(*c);
        *c = NULL;
    }
}

/* ---------------------------------------------------- render thread */

static void set_constant(D3DDevice *d, int reg, float x, float y, float z, float w)
{
    float v[4] = { x, y, z, w };
    d->SetPixelShaderConstantF(reg, v, 1);
}

/* Two deblocking passes over the luma texture; returns the filtered texture. */
static D3DTexture *deblock(D3DDevice *d, D3DTexture *luma, D3DTexture *qp)
{
    D3DVIEWPORT9 vp = { 0, 0, (DWORD)vid_w, (DWORD)vid_h, 0.0f, 1.0f };
    D3DVIEWPORT9 full = { 0, 0, GFX_WIDTH, GFX_HEIGHT, 0.0f, 1.0f };
    int pass, i;

    d->SetRenderTarget(0, deblock_rt);
    d->SetViewport(&vp);
    d->SetTexture(1, qp);
    d->SetTexture(2, lut_tex);
    for (i = 0; i < 3; i++)
        gfx_set_filter(d, i, false);
    set_constant(d, 0, (float)vid_w, (float)vid_h, 1.0f / vid_w, 1.0f / vid_h);
    set_constant(d, 1, 1.0f / mb_w, 1.0f / mb_h, 0, 0);
    for (pass = 0; pass < 2; pass++) {
        d->SetTexture(0, pass == 0 ? luma : deblock_tex[0]);
        set_constant(d, 2, pass == 0 ? 1.0f : 0.0f, pass == 0 ? 0.0f : 1.0f, 0, 0);
        gfx_draw_target(d, GFX_SHADER_DEBLOCK);
        d->SetTexture(0, NULL);   /* the next resolve may overwrite it */
        d->Resolve(D3DRESOLVE_RENDERTARGET0, NULL, deblock_tex[pass], NULL, 0, 0, NULL, 1.0f, 0, NULL);
    }
    d->SetRenderTarget(0, back_buffer);
    d->SetViewport(&full);
    return deblock_tex[1];
}

static void draw_frame(const struct staging *s)
{
    D3DDevice *d = gfx_begin();
    D3DTexture *luma;
    int k = tex_next, i;

    tex_next ^= 1;
    if (tex_fence[k])
        d->BlockOnFence(tex_fence[k]);
    gfx_upload(tex[k][0], s->plane[0], vid_w, vid_w, vid_h);
    gfx_upload(tex[k][1], s->plane[1], chroma_w, chroma_w, chroma_h);
    gfx_upload(tex[k][2], s->plane[2], chroma_w, chroma_w, chroma_h);
    if (gpu_deblock)
        gfx_upload(qp_tex[k], s->qp, mb_w, mb_w, mb_h);

    /* The staging slot can be refilled now. */
    EnterCriticalSection(&mailbox_lock);
    slot_rendering = -1;
    LeaveCriticalSection(&mailbox_lock);

    luma = gpu_deblock ? deblock(d, tex[k][0], qp_tex[k]) : tex[k][0];

    d->SetTexture(0, luma);
    d->SetTexture(1, tex[k][1]);
    d->SetTexture(2, tex[k][2]);
    gfx_set_filter(d, 0, false);   /* the shader does its own bicubic filtering */
    gfx_set_filter(d, 1, true);
    gfx_set_filter(d, 2, true);
    set_constant(d, 0, (float)vid_w, (float)vid_h, 1.0f / vid_w, 1.0f / vid_h);
    set_constant(d, 1, sharpen, 0, 0, 0);
    gfx_draw_rect(d, GFX_SHADER_PRESENT, 0, 0, GFX_WIDTH, GFX_HEIGHT);
    for (i = 0; i < 3; i++)
        d->SetTexture(i, NULL);
    draw_overlay(d);
    tex_fence[k] = d->InsertFence();
    d->Present(NULL, NULL, NULL, NULL);
    gfx_end();
}

static DWORD WINAPI render_main(LPVOID arg)
{
    D3DDevice *d;
    (void)arg;

    d = gfx_begin();
    d->SetRenderState(D3DRS_PRESENTINTERVAL, options.vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE);
    gfx_end();

    while (running) {
        uint64_t t0;
        int s;

        WaitForSingleObject(frame_event, 100);
        EnterCriticalSection(&mailbox_lock);
        s = slot_pending;
        if (s >= 0) {
            slot_pending = -1;
            slot_rendering = s;
        }
        LeaveCriticalSection(&mailbox_lock);
        if (s < 0)
            continue;

        t0 = now_us();
        draw_frame(&slots[s]);
        stats.rendered++;
        stats.avg_render_us = (stats.avg_render_us * 15 + (uint32_t)(now_us() - t0)) / 16;
    }

    d = gfx_begin();
    d->SetRenderState(D3DRS_PRESENTINTERVAL, D3DPRESENT_INTERVAL_ONE);
    gfx_end();
    return 0;
}

/* ---------------------------------------------------- decoding */

static void copy_plane(uint8_t *dst, int w, int h, const uint8_t *src, int pitch)
{
    int y;

    for (y = 0; y < h; y++)
        memcpy(dst + y * w, src + y * pitch, w);
}

static void publish(const AVFrame *f)
{
    int s;

    EnterCriticalSection(&mailbox_lock);
    /* Pick a slot that is neither queued nor being uploaded. */
    for (s = 0; s < FRAME_SLOTS; s++)
        if (s != slot_pending && s != slot_rendering)
            break;
    if (slot_pending >= 0)
        stats.dropped++;   /* the renderer didn't get to the previous frame */
    LeaveCriticalSection(&mailbox_lock);

    copy_plane(slots[s].plane[0], vid_w, vid_h, f->data[0], f->linesize[0]);
    copy_plane(slots[s].plane[1], chroma_w, chroma_h, f->data[1], f->linesize[1]);
    copy_plane(slots[s].plane[2], chroma_w, chroma_h, f->data[2], f->linesize[2]);
    if (gpu_deblock) {
        if (f->qscale_table)
            copy_plane(slots[s].qp, mb_w, mb_h, (const uint8_t *)f->qscale_table, f->qstride);
        else
            memset(slots[s].qp, 26, mb_w * mb_h);   /* a typical streaming QP */
    }

    EnterCriticalSection(&mailbox_lock);
    slot_pending = s;
    LeaveCriticalSection(&mailbox_lock);
    SetEvent(frame_event);
}

static void log_stats(void)
{
    DWORD now = GetTickCount(), dt = now - stats_last_ms;
    uint32_t decoded = stats.decoded - stats_last.decoded;

    if (dt < STATS_INTERVAL_MS)
        return;
    vlog("stats: %.1f fps in, %.1f decoded, %.1f shown; decode %.2f ms avg; render %.2f ms; "
         "dropped %u, errors %u\n",
         (stats.received - stats_last.received) * 1000.0 / dt, decoded * 1000.0 / dt,
         (stats.rendered - stats_last.rendered) * 1000.0 / dt,
         decoded ? decode_us_sum / 1000.0 / decoded : 0.0, stats.avg_render_us / 1000.0,
         stats.dropped - stats_last.dropped, stats.decode_errors - stats_last.decode_errors);
    stats_last = stats;
    stats_last_ms = now;
    decode_us_sum = 0;
}

/* ---------------------------------------------------- recording */

static void capture_start(void)
{
    /* Record once; delete moonlight\capture.bin to record again. */
    if (storage_dir() == NULL || plat_benchmark_available())
        return;
    cap_buf = (uint8_t *)malloc(CAPTURE_MAX);
    cap_len = 16;
    cap_frames = 0;
}

static void capture_frame(const uint8_t *data, int len)
{
    if (cap_buf == NULL || cap_frames >= CAPTURE_FRAMES || cap_len + 4 + len > CAPTURE_MAX)
        return;
    memcpy(cap_buf + cap_len, &len, 4);   /* big-endian, as everything here */
    memcpy(cap_buf + cap_len + 4, data, len);
    cap_len += 4 + len;
    cap_frames++;
}

static void capture_finish(void)
{
    uint32_t hdr[4];

    if (cap_buf == NULL)
        return;
    if (cap_frames >= 60) {
        memcpy(&hdr[0], "MLC1", 4);
        hdr[1] = vid_w;
        hdr[2] = vid_h;
        hdr[3] = cap_frames;
        memcpy(cap_buf, hdr, sizeof(hdr));
        if (storage_write(CAPTURE_FILE, cap_buf, cap_len))
            vlog("capture: saved %u frames (%u KB) for the decoder benchmark\n", cap_frames, cap_len / 1024);
        else
            vlog("capture: couldn't save\n");
    }
    free(cap_buf);
    cap_buf = NULL;
}

/* ---------------------------------------------------- GPU picture filters */

/* Creates what GPU deblocking needs; without it (or with the in-loop filter on) frames are
 * presented directly. Called with the device held. */
static void setup_gpu_filters(D3DDevice *d)
{
    D3DSURFACE_PARAMETERS sp;
    D3DLOCKED_RECT lr;
    bool ok = true;
    int i, q;

    sharpen = options.gpu_filter >= 2 ? 0.5f : 0.0f;
    gpu_deblock = false;
    if (!options.fast_decode || options.gpu_filter < 1)
        return;   /* the decoder deblocks already, or the user turned it off */

    mb_w = (vid_w + 15) / 16;
    mb_h = (vid_h + 15) / 16;
    for (i = 0; i < FRAME_SLOTS; i++) {
        slots[i].qp = (uint8_t *)malloc(mb_w * mb_h);
        ok = ok && slots[i].qp;
    }
    for (i = 0; i < 2 && ok; i++)
        ok = SUCCEEDED(d->CreateTexture(mb_w, mb_h, 1, 0, D3DFMT_LIN_L8, 0, &qp_tex[i], NULL)) &&
             SUCCEEDED(d->CreateTexture(vid_w, vid_h, 1, 0, D3DFMT_A8R8G8B8, 0, &deblock_tex[i], NULL));

    if (ok && lut_tex == NULL) {
        /* 64x1 lookup: alpha, beta, tc0 (bS 1), tc0 (bS 2) for QP 0..51 */
        ok = SUCCEEDED(d->CreateTexture(64, 1, 1, 0, D3DFMT_LIN_A8R8G8B8, 0, &lut_tex, NULL)) &&
             SUCCEEDED(lut_tex->LockRect(0, &lr, NULL, 0));
        if (ok) {
            uint32_t *px = (uint32_t *)lr.pBits;
            for (q = 0; q < 64; q++) {
                const unsigned char *e = deblock_lut[q < 52 ? q : 51];
                px[q] = ((uint32_t)e[3] << 24) | ((uint32_t)e[0] << 16) | ((uint32_t)e[1] << 8) | e[2];
            }
            lut_tex->UnlockRect(0);
        }
    }

    /* At EDRAM offset 0: 1080p needs 8 MB of the 10 MB. */
    memset(&sp, 0, sizeof(sp));
    ok = ok && SUCCEEDED(d->CreateRenderTarget(vid_w, vid_h, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE,
                                               &deblock_rt, &sp)) &&
         SUCCEEDED(d->GetRenderTarget(0, &back_buffer));
    gpu_deblock = ok;
    vlog("video: GPU deblocking %s, sharpening %.1f\n", ok ? "on" : "unavailable", sharpen);
}

/* ---------------------------------------------------- Moonlight callbacks */

static void free_resources(void)
{
    D3DDevice *d;
    int i, k;

    close_h264(&codec_ctx);
    if (dec_frame)
        avcodec_free_frame(&dec_frame);
    free(pkt_buf);
    pkt_buf = NULL;
    for (i = 0; i < FRAME_SLOTS; i++) {
        for (k = 0; k < 3; k++) {
            free(slots[i].plane[k]);
            slots[i].plane[k] = NULL;
        }
        free(slots[i].qp);
        slots[i].qp = NULL;
    }
    d = gfx_begin();
    free_overlay(d);
    for (i = 0; i < 2; i++) {
        for (k = 0; k < 3; k++) {
            if (tex[i][k])
                tex[i][k]->Release();
            tex[i][k] = NULL;
        }
        if (qp_tex[i])
            qp_tex[i]->Release();
        if (deblock_tex[i])
            deblock_tex[i]->Release();
        qp_tex[i] = deblock_tex[i] = NULL;
        tex_fence[i] = 0;
    }
    if (deblock_rt)
        deblock_rt->Release();
    if (back_buffer)
        back_buffer->Release();
    deblock_rt = back_buffer = NULL;
    gpu_deblock = false;
    gfx_end();
}

static int video_setup(int format, int width, int height, int fps, void *ctx, int dr_flags)
{
    D3DDevice *d;
    bool ok = true;
    int i, k;
    (void)fps;
    (void)ctx;
    (void)dr_flags;

    if (!(format & VIDEO_FORMAT_MASK_H264))
        return -1;

    memset(&stats, 0, sizeof(stats));
    stats.display_w = GFX_WIDTH;
    stats.display_h = GFX_HEIGHT;
    stats.renderer = RENDERER_GPU;
    stats_last = stats;
    stats_last_ms = GetTickCount();
    decode_us_sum = 0;
    vid_w = width;
    vid_h = height;
    chroma_w = (width + 1) / 2;
    chroma_h = (height + 1) / 2;

    vlog("video: setup %dx%d, format 0x%x, vmx flags %d\n", width, height, format, ff_xbox_vmx_flags);
    codec_ctx = open_h264(width, height, decode_threads(), options.fast_decode);
    dec_frame = avcodec_alloc_frame();
    if (codec_ctx == NULL || dec_frame == NULL) {
        vlog("video: couldn't open the decoder\n");
        free_resources();
        return -1;
    }
    stats.decode_threads = (codec_ctx->active_thread_type & FF_THREAD_SLICE) ? codec_ctx->thread_count : 1;

    pkt_cap = 1024 * 1024;
    pkt_buf = (uint8_t *)malloc(pkt_cap + FF_INPUT_BUFFER_PADDING_SIZE);
    for (i = 0; i < FRAME_SLOTS; i++) {
        slots[i].plane[0] = (uint8_t *)malloc(vid_w * vid_h);
        slots[i].plane[1] = (uint8_t *)malloc(chroma_w * chroma_h);
        slots[i].plane[2] = (uint8_t *)malloc(chroma_w * chroma_h);
        ok = ok && slots[i].plane[0] && slots[i].plane[1] && slots[i].plane[2];
    }

    d = gfx_begin();
    for (i = 0; i < 2 && ok; i++)
        for (k = 0; k < 3 && ok; k++)
            ok = SUCCEEDED(d->CreateTexture(k ? chroma_w : vid_w, k ? chroma_h : vid_h, 1, 0, D3DFMT_LIN_L8, 0,
                                            &tex[i][k], NULL));
    if (ok)
        setup_gpu_filters(d);
    gfx_end();
    if (!ok || pkt_buf == NULL) {
        vlog("video: out of memory for buffers/textures\n");
        free_resources();
        return -1;
    }
    vlog("video: ready, %d decode threads\n", stats.decode_threads);

    slot_pending = slot_rendering = -1;
    tex_next = 0;
    capture_start();
    return 0;
}

static void video_start(void)
{
    running = true;
    render_thread = CreateThread(NULL, 64 * 1024, render_main, NULL, CREATE_SUSPENDED, NULL);
    if (render_thread) {
        XSetThreadProcessor(render_thread, HW_THREAD_RENDER);
        /* A slice worker may share this hardware thread (5 or 6 decoder
         * threads); a finished frame shouldn't wait for its time slice. */
        SetThreadPriority(render_thread, THREAD_PRIORITY_ABOVE_NORMAL);
        ResumeThread(render_thread);
    }
}

static void video_stop(void)
{
    running = false;
    if (render_thread) {
        SetEvent(frame_event);
        WaitForSingleObject(render_thread, INFINITE);
        CloseHandle(render_thread);
        render_thread = NULL;
    }
}

static void video_cleanup(void)
{
    vlog("video: stream ended: %u received, %u decoded, %u shown, %u dropped, %u errors\n",
         stats.received, stats.decoded, stats.rendered, stats.dropped, stats.decode_errors);
    capture_finish();
    free_resources();
}

static int video_submit(PDECODE_UNIT du)
{
    AVPacket pkt;
    PLENTRY e;
    uint64_t t0, dt;
    int off = 0, got = 0, ret;

    stats.received++;
    stats.received_bytes += du->fullLength;
    if (du->fullLength > pkt_cap) {
        uint8_t *n = (uint8_t *)realloc(pkt_buf, du->fullLength + FF_INPUT_BUFFER_PADDING_SIZE);
        if (n == NULL)
            return DR_NEED_IDR;
        pkt_buf = n;
        pkt_cap = du->fullLength;
    }
    for (e = du->bufferList; e != NULL; e = e->next) {
        memcpy(pkt_buf + off, e->data, e->length);
        off += e->length;
    }
    memset(pkt_buf + off, 0, FF_INPUT_BUFFER_PADDING_SIZE);
    capture_frame(pkt_buf, off);

    av_init_packet(&pkt);
    pkt.data = pkt_buf;
    pkt.size = off;
    pkt.flags = du->frameType == FRAME_TYPE_IDR ? AV_PKT_FLAG_KEY : 0;

    t0 = now_us();
    ret = avcodec_decode_video2(codec_ctx, dec_frame, &got, &pkt);
    dt = now_us() - t0;
    if (ret < 0) {
        if (stats.decode_errors++ < 5)
            vlog("video: decode error %d on frame %d\n", ret, du->frameNumber);
        log_stats();
        return DR_NEED_IDR;
    }
    if (got) {
        if (stats.decoded == 0)
            vlog("video: first frame decoded (%u us)\n", (unsigned)dt);
        stats.decoded++;
        stats.avg_decode_us = (stats.avg_decode_us * 15 + (uint32_t)dt) / 16;
        decode_us_sum += dt;
        if (dec_frame->format == AV_PIX_FMT_YUV420P || dec_frame->format == AV_PIX_FMT_YUVJ420P)
            publish(dec_frame);
        else
            stats.decode_errors++;
    }
    log_stats();
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
    /* No direct submit: Moonlight's own decoder thread calls video_submit. */
    cb->capabilities = CAPABILITY_SLICES_PER_FRAME(decode_threads());
}

/* ---------------------------------------------------- benchmark */

/* threads 0 = the "Decoder threads" setting. The rows with a fixed count only
 * run when the recording has that many slices per frame and it isn't the
 * setting already (with fewer slices, extra threads have nothing to do). */
static const struct {
    const char *name;
    int flags;
    int threads;
} bench_configs[] = {
    { "Plain C", 0, 0 },
    { "Vector luma motion comp.", XVMX_QPEL, 0 },
    { "Vector chroma motion comp.", XVMX_CHROMA, 0 },
    { "Prefetch", XVMX_PREFETCH, 0 },
    { "Vector inverse transform", XVMX_IDCT, 0 },
    { "All vector code", XVMX_ALL, 0 },
    { "All vector code, 4 threads", XVMX_ALL, 4 },
    { "All vector code, 5 threads", XVMX_ALL, 5 },
    { "All vector code, 6 threads", XVMX_ALL, 6 },
    { "Plain C (again)", 0, 0 },
};
#define BENCH_RUNS (sizeof(bench_configs) / sizeof(bench_configs[0]))

struct bench {
    uint8_t *data;              /* capture file */
    size_t len;
    uint32_t w, h, frames;
    uint32_t *ref_hash;         /* per frame, from the first (C) run */
    volatile int run;           /* configuration in progress */
    volatile int frame;
    int slices;                 /* per frame in the recording */
    bool skip[BENCH_RUNS];
    double ms[BENCH_RUNS], worst_ms[BENCH_RUNS];
    uint32_t mismatches[BENCH_RUNS], decoded[BENCH_RUNS];
    volatile bool done;
};

/* FNV-1a over the visible picture, 32 bits at a time. */
static uint32_t frame_hash(const AVFrame *f, int w, int h)
{
    uint32_t hash = 2166136261u;
    int p, y, x;

    for (p = 0; p < 3; p++) {
        int pw = p ? (w + 1) / 2 : w, ph = p ? (h + 1) / 2 : h;
        for (y = 0; y < ph; y++) {
            const uint8_t *row = f->data[p] + y * f->linesize[p];
            for (x = 0; x + 4 <= pw; x += 4)
                hash = (hash ^ *(const uint32_t *)(row + x)) * 16777619u;
            for (; x < pw; x++)
                hash = (hash ^ row[x]) * 16777619u;
        }
    }
    return hash;
}

static void bench_run(struct bench *b, int run)
{
    AVCodecContext *c;
    AVFrame *frame = avcodec_alloc_frame();
    uint8_t *buf = (uint8_t *)malloc(2 * 1024 * 1024 + FF_INPUT_BUFFER_PADDING_SIZE);
    const uint8_t *p = b->data + 16, *end = b->data + b->len;
    uint64_t total = 0, worst = 0;
    uint32_t i, out = 0;

    ff_xbox_vmx_flags = bench_configs[run].flags;
    c = open_h264(b->w, b->h, bench_configs[run].threads ? bench_configs[run].threads : decode_threads(), true);
    if (c == NULL || frame == NULL || buf == NULL) {
        close_h264(&c);
        if (frame)
            avcodec_free_frame(&frame);
        free(buf);
        return;
    }

    for (i = 0; i < b->frames; i++) {
        AVPacket pkt;
        uint32_t len;
        uint64_t t0, dt;
        int got = 0;

        b->frame = i;
        if (end - p < 4)
            break;   /* truncated recording */
        memcpy(&len, p, 4);
        if (len > 2 * 1024 * 1024 || len > (uint32_t)(end - p - 4))
            break;
        memcpy(buf, p + 4, len);
        memset(buf + len, 0, FF_INPUT_BUFFER_PADDING_SIZE);
        p += 4 + len;

        av_init_packet(&pkt);
        pkt.data = buf;
        pkt.size = len;
        t0 = now_us();
        avcodec_decode_video2(c, frame, &got, &pkt);
        dt = now_us() - t0;
        total += dt;
        if (dt > worst)
            worst = dt;
        if (got && out < b->frames) {
            uint32_t hsh = frame_hash(frame, frame->width, frame->height);
            if (run == 0)
                b->ref_hash[out] = hsh;
            else if (hsh != b->ref_hash[out])
                b->mismatches[run]++;
            out++;
        }
    }
    b->decoded[run] = out;
    b->ms[run] = i ? total / 1000.0 / i : 0;
    b->worst_ms[run] = worst / 1000.0;
    vlog("bench: %-28s %6.2f ms/frame (worst %6.2f), %u frames, %u differ from C\n", bench_configs[run].name,
         b->ms[run], b->worst_ms[run], out, b->mismatches[run]);

    close_h264(&c);
    avcodec_free_frame(&frame);
    free(buf);
}

static DWORD WINAPI bench_main(LPVOID arg)
{
    struct bench *b = (struct bench *)arg;
    int saved = ff_xbox_vmx_flags, run;

    for (run = 0; run < (int)BENCH_RUNS; run++) {
        if (b->skip[run])
            continue;
        b->run = run;
        bench_run(b, run);
    }
    ff_xbox_vmx_flags = saved;
    b->done = true;
    return 0;
}

/* Slices (NAL types 1 and 5) in one Annex B access unit. */
static int count_slices(const uint8_t *p, uint32_t len)
{
    uint32_t i;
    int n = 0;

    for (i = 0; i + 3 < len; i++) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            int type = p[i + 3] & 0x1F;
            if (type == 1 || type == 5)
                n++;
            i += 2;
        }
    }
    return n;
}

bool plat_benchmark_available(void)
{
    char path[160];

    if (storage_dir() == NULL)
        return false;
    snprintf(path, sizeof(path), "%s\\%s", storage_dir(), CAPTURE_FILE);
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

void plat_run_benchmark(void)
{
    struct bench b;
    struct ui_stat rows[BENCH_RUNS + 1];
    HANDLE th;
    size_t len = 0;
    char msg[480];
    int i, n = 0;

    memset(&b, 0, sizeof(b));
    b.data = (uint8_t *)storage_read(CAPTURE_FILE, &len);
    if (b.data == NULL || len < 16 || memcmp(b.data, "MLC1", 4) != 0) {
        free(b.data);
        ui_draw_message(ICON_ERROR, "No recording", "Stream for 20 seconds first; the console records it for the benchmark.");
        ui_present();
        while (!(input_menu_pressed() & (A_FLAG | B_FLAG)))
            Sleep(16);
        return;
    }
    memcpy(&b.w, b.data + 4, 4);
    memcpy(&b.h, b.data + 8, 4);
    memcpy(&b.frames, b.data + 12, 4);
    b.len = len;
    if (b.frames > CAPTURE_FRAMES)
        b.frames = CAPTURE_FRAMES;   /* damaged header; the loop also stops at the end of the data */
    b.ref_hash = (uint32_t *)calloc(b.frames + 1, sizeof(uint32_t));
    if (b.ref_hash == NULL) {
        free(b.data);
        return;
    }
    {
        uint32_t first;

        memcpy(&first, b.data + 16, 4);
        if (len >= 20 && first <= len - 20)
            b.slices = count_slices(b.data + 20, first);
    }
    for (i = 0; i < (int)BENCH_RUNS; i++) {
        int t = bench_configs[i].threads;
        b.skip[i] = t != 0 && (t == decode_threads() || t > b.slices);
    }
    vlog("bench: %ux%u, %u frames, threads %d\n", b.w, b.h, b.frames, decode_threads());

    th = CreateThread(NULL, 256 * 1024, bench_main, &b, CREATE_SUSPENDED, NULL);
    XSetThreadProcessor(th, 0);
    ResumeThread(th);
    while (!b.done) {
        _snprintf(msg, sizeof(msg), "%s: frame %d of %u", bench_configs[b.run].name, b.frame + 1, b.frames);
        msg[sizeof(msg) - 1] = '\0';
        ui_draw_busy("Decoder benchmark", msg, GetTickCount());
        ui_present();
        Sleep(100);
    }
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);

    for (i = 0; i < (int)BENCH_RUNS; i++) {
        if (b.skip[i])
            continue;
        rows[n].label = bench_configs[i].name;
        _snprintf(rows[n].value, sizeof(rows[n].value), "%.2f ms%s", b.ms[i],
                  b.mismatches[i] ? " (picture differs!)" : "");
        rows[n].value[sizeof(rows[n].value) - 1] = '\0';
        n++;
    }
    _snprintf(msg, sizeof(msg), "Average decode time per frame over %u recorded frames (%ux%u, %d slices, "
              "%d threads). Under 33 ms keeps up with 30 FPS, under 16.7 ms with 60 FPS.%s", b.frames, b.w, b.h,
              b.slices, decode_threads(), b.slices < decode_threads() ?
              " This recording has fewer slices than threads: delete capture.bin and stream again to measure "
              "the extra threads." : "");
    msg[sizeof(msg) - 1] = '\0';
    ui_draw_stats("Decoder benchmark", rows, n, msg);
    ui_present();
    while (!(input_menu_pressed() & (A_FLAG | B_FLAG)))
        Sleep(16);

    free(b.ref_hash);
    free(b.data);
}
