/*
 * GPU presentation of decoded frames. See gpu.h.
 *
 * The shaders are the precompiled YUV->RGB pixel shader and pass-through
 * vertex shader from Ced2911's MPlayer port for libxenon (libvo/vo_xenon.c,
 * GPLv2+). libxenon ships no shader compiler, so they're used as-is.
 */
#include "gpu.h"

#include <stdio.h>
#include <string.h>
#include <xenos/xe.h>
#include <xenos/xenos.h>
#include <xenos/edram.h>

/* ps_3_0: samplers s0=Y, s1=U, s2=V; BT.601 limited range -> RGB. */
static const uint32_t ps_yuv[] = {
    0x102a1100, 0x00000110, 0x000000c4, 0x00000000, 0x00000024, 0x000000c4,
    0x000000ec, 0x00000000, 0x00000000, 0x0000009c, 0x0000001c, 0x0000008d,
    0xffff0300, 0x00000003, 0x0000001c, 0x00000000, 0x00000086, 0x00000058,
    0x00030001, 0x00010000, 0x00000064, 0x00000000, 0x00000074, 0x00030002,
    0x00010000, 0x00000064, 0x00000000, 0x0000007d, 0x00030000, 0x00010000,
    0x00000064, 0x00000000, 0x55546578, 0x74757265, 0x00ababab, 0x0004000c,
    0x00010001, 0x00010000, 0x00000000, 0x56546578, 0x74757265, 0x00595465,
    0x78747572, 0x65007073, 0x5f335f30, 0x00322e30, 0x2e323033, 0x35332e30,
    0x00ababab, 0x00000000, 0x00000001, 0x00000000, 0x00000000, 0x00000014,
    0x01fc0010, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000040,
    0x00000084, 0x10000200, 0x00000004, 0x00000000, 0x00000821, 0x00010001,
    0x00000001, 0x00003050, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0xbf5ee978, 0xbf8a76c8, 0x3fcc49ba, 0x400126e9, 0x3ec83127, 0x3f5020c5,
    0x3f94fdf4, 0x00000000, 0xbd94fdf4, 0xbe483127, 0xbed020c5, 0x00000000,
    0x00153002, 0x00001200, 0xc4000000, 0x00005005, 0x00002200, 0x00000000,
    0x10282001, 0x1f1ffff8, 0x00004000, 0x10182001, 0x1f1fffc7, 0x00004000,
    0x10082001, 0x1f1ffe3f, 0x00004000, 0xc8070000, 0x0062bec0, 0x8b02feff,
    0xc8030001, 0x00b01ab0, 0x8b02fdfd, 0xc88dc000, 0x00c6c6b0, 0xab02fe01,
    0x64800000, 0x00000061, 0xe2000000, 0xc8028000, 0x021bc600, 0xe0000000,
    0x00000000, 0x00000000, 0x00000000
};

/* vs_3_0: passes POSITION (float4) and TEXCOORD0 (float2) through. */
static const uint32_t vs_passthrough[] = {
    0x102a1101, 0x00000090, 0x00000060, 0x00000000, 0x00000024, 0x00000000,
    0x00000058, 0x00000000, 0x00000000, 0x00000030, 0x0000001c, 0x00000023,
    0xfffe0300, 0x00000000, 0x00000000, 0x00000000, 0x0000001c, 0x76735f33,
    0x5f300032, 0x2e302e32, 0x31313139, 0x2e3000ab, 0x00000000, 0x00000060,
    0x00010001, 0x00000000, 0x00000000, 0x00000821, 0x00000001, 0x00000002,
    0x00000001, 0x00000290, 0x00100003, 0x00305004, 0x00003050, 0x00001006,
    0x30052003, 0x00001200, 0xc2000000, 0x00001005, 0x00001200, 0xc4000000,
    0x00001006, 0x00002200, 0x00000000, 0x05f81000, 0x00000688, 0x00000000,
    0x05f80000, 0x00000fc8, 0x00000000, 0xc80f803e, 0x00000000, 0xe2010100,
    0xc8038000, 0x00b0b000, 0xe2000000, 0x00000000, 0x00000000, 0x00000000
};

struct vertex {
    float x, y, z, w;
    float u, v;
};

static struct XenosDevice xe_dev;
static struct XenosDevice *xe;
static struct XenosShader *ps, *vs;
static struct XenosVertexBuffer *vb;
static struct XenosSurface *rt;              /* EDRAM render target description */
static struct XenosSurface *buffers[2];      /* scanout buffers we flip between */
static struct XenosSurface *tex_y, *tex_u, *tex_v;
static int tex_w, tex_h;
static int back;
static bool ready;

bool gpu_init(void)
{
    static const struct XenosVBFFormat vbf = {
        2, {
            { XE_USAGE_POSITION, 0, XE_TYPE_FLOAT4 },
            { XE_USAGE_TEXCOORD, 0, XE_TYPE_FLOAT2 },
        }
    };

    xe = &xe_dev;
    Xe_Init(xe);
    rt = Xe_GetFramebufferSurface(xe);
    Xe_SetRenderTarget(xe, rt);

    ps = Xe_LoadShaderFromMemory(xe, (void *)ps_yuv);
    vs = Xe_LoadShaderFromMemory(xe, (void *)vs_passthrough);
    if (ps == NULL || vs == NULL)
        return false;
    Xe_InstantiateShader(xe, ps, 0);
    Xe_InstantiateShader(xe, vs, 0);
    Xe_ShaderApplyVFetchPatches(xe, vs, 0, &vbf);

    edram_init(xe);

    vb = Xe_CreateVertexBuffer(xe, 3 * sizeof(struct vertex));
    buffers[0] = Xe_CreateTexture(xe, rt->width, rt->height, 0, XE_FMT_8888 | XE_FMT_BGRA, 1);
    buffers[1] = Xe_CreateTexture(xe, rt->width, rt->height, 0, XE_FMT_8888 | XE_FMT_BGRA, 1);
    if (vb == NULL || buffers[0] == NULL || buffers[1] == NULL)
        return false;

    Xe_SetClearColor(xe, 0xFF000000);
    ready = true;
    return true;
}

bool gpu_ready(void)
{
    return ready;
}

static struct XenosSurface *make_plane(int w, int h)
{
    struct XenosSurface *s = Xe_CreateTexture(xe, w, h, 1, XE_FMT_8, 0);

    if (s) {
        s->u_addressing = XE_TEXADDR_CLAMP;
        s->v_addressing = XE_TEXADDR_CLAMP;
        s->use_filtering = 1;
    }
    return s;
}

static void destroy_planes(void)
{
    if (tex_y) Xe_DestroyTexture(xe, tex_y);
    if (tex_u) Xe_DestroyTexture(xe, tex_u);
    if (tex_v) Xe_DestroyTexture(xe, tex_v);
    tex_y = tex_u = tex_v = NULL;
}

/* Clears EDRAM and resolves (black) into the given buffer. */
static void clear_into(struct XenosSurface *s)
{
    Xe_InvalidateState(xe);
    Xe_ResolveInto(xe, s, XE_SOURCE_COLOR, XE_CLEAR_COLOR | XE_CLEAR_DS);
    Xe_Execute(xe);
    Xe_Sync(xe);
}

bool gpu_stream_begin(int width, int height)
{
    struct vertex *v;
    float sx = 1.0f, sy = 1.0f;
    float screen_aspect, video_aspect;

    if (!ready)
        return false;

    if (width != tex_w || height != tex_h || tex_y == NULL) {
        destroy_planes();
        tex_y = make_plane(width, height);
        tex_u = make_plane(width / 2, height / 2);
        tex_v = make_plane(width / 2, height / 2);
        if (!tex_y || !tex_u || !tex_v) {
            destroy_planes();
            return false;
        }
        tex_w = width;
        tex_h = height;
    }

    /* Aspect-correct quad in clip space; RECTLIST takes TL, BL, TR. */
    screen_aspect = (float)rt->width / (float)rt->height;
    video_aspect = (float)width / (float)height;
    if (screen_aspect > video_aspect)
        sx = video_aspect / screen_aspect;
    else
        sy = screen_aspect / video_aspect;

    v = Xe_VB_Lock(xe, vb, 0, 3 * sizeof(struct vertex), XE_LOCK_WRITE);
    v[0] = (struct vertex){ -sx,  sy, 0, 1, 0, 0 };
    v[1] = (struct vertex){ -sx, -sy, 0, 1, 0, 1 };
    v[2] = (struct vertex){  sx,  sy, 0, 1, 1, 0 };
    Xe_VB_Unlock(xe, vb);

    /* Black letterbox bars in both buffers, then show one of them. */
    clear_into(buffers[0]);
    clear_into(buffers[1]);
    Xe_SetFrameBufferSurface(xe, buffers[0]);
    back = 1;
    return true;
}

static void upload_plane(struct XenosSurface *s, const uint8_t *src, int linesize, int w, int h)
{
    uint8_t *dst = Xe_Surface_LockRect(xe, s, 0, 0, 0, 0, XE_LOCK_WRITE);
    int y;

    if (linesize == s->wpitch && w == s->wpitch) {
        memcpy(dst, src, (size_t)w * h);
    } else {
        for (y = 0; y < h; y++)
            memcpy(dst + y * s->wpitch, src + y * linesize, w);
    }
    Xe_Surface_Unlock(xe, s);   /* flushes the CPU cache for the GPU */
}

void gpu_present(const AVFrame *f, bool vsync)
{
    if (!ready || tex_y == NULL)
        return;

    upload_plane(tex_y, f->data[0], f->linesize[0], tex_w, tex_h);
    upload_plane(tex_u, f->data[1], f->linesize[1], tex_w / 2, tex_h / 2);
    upload_plane(tex_v, f->data[2], f->linesize[2], tex_w / 2, tex_h / 2);

    Xe_InvalidateState(xe);
    Xe_SetClearColor(xe, 0xFF000000);
    Xe_SetCullMode(xe, XE_CULL_NONE);
    Xe_SetZEnable(xe, 0);
    Xe_SetZWrite(xe, 0);
    Xe_SetAlphaTestEnable(xe, 0);
    Xe_SetBlendOp(xe, XE_BLENDOP_ADD);
    Xe_SetSrcBlend(xe, XE_BLEND_ONE);
    Xe_SetDestBlend(xe, XE_BLEND_ZERO);

    Xe_SetStreamSource(xe, 0, vb, 0, sizeof(struct vertex) / 4);
    Xe_SetShader(xe, SHADER_TYPE_PIXEL, ps, 0);
    Xe_SetShader(xe, SHADER_TYPE_VERTEX, vs, 0);
    Xe_SetTexture(xe, 0, tex_y);
    Xe_SetTexture(xe, 1, tex_u);
    Xe_SetTexture(xe, 2, tex_v);
    Xe_DrawPrimitive(xe, XE_PRIMTYPE_RECTLIST, 0, 1);

    /* Render into the hidden buffer and wait for it, then flip: the frame
     * goes on screen as soon as it's drawn (no extra frame of latency). */
    Xe_ResolveInto(xe, buffers[back], XE_SOURCE_COLOR, XE_CLEAR_COLOR | XE_CLEAR_DS);
    Xe_Execute(xe);
    Xe_Sync(xe);

    if (vsync)
        while (!Xe_IsVBlank(xe))
            ;
    Xe_SetFrameBufferSurface(xe, buffers[back]);
    back ^= 1;
}

void gpu_stream_end(void)
{
    if (!ready)
        return;
    /* default_fb is the surface the text console draws into. */
    Xe_SetFrameBufferSurface(xe, &xe->default_fb);
}
