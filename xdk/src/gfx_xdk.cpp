/*
 * Direct3D on the Xbox 360: one 1280x720 device shared by the UI (main
 * thread) and video presentation (render thread), a fullscreen-quad vertex
 * shader, and pixel shaders for RGB textures and YUV 4:2:0 planes.
 *
 * The device isn't thread-safe: each user takes it with gfx_begin(), which
 * also moves D3D's thread ownership to the caller.
 */
#include "xdk.h"

#include <d3dx9shader.h>
#include <string.h>

#include "../../src/ui/ui.h"

static D3DDevice *dev;
static CRITICAL_SECTION dev_lock;
static D3DVertexDeclaration *decl;
static D3DVertexShader *vs;
static D3DPixelShader *ps[2];

struct vertex {
    float x, y;     /* clip space */
    float u, v;
};

static const char vs_src[] =
    "struct V { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
    "V main(V v) { return v; }\n";

static const char ps_rgb_src[] =
    "sampler rgb : register(s0);\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR { return float4(tex2D(rgb, uv).rgb, 1); }\n";

/* BT.601 limited range, which is what the stream is configured for. */
static const char ps_yuv_src[] =
    "sampler ty : register(s0);\n"
    "sampler tu : register(s1);\n"
    "sampler tv : register(s2);\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
    "    float y = 1.164383 * (tex2D(ty, uv).r - 0.062745);\n"
    "    float u = tex2D(tu, uv).r - 0.501961;\n"
    "    float v = tex2D(tv, uv).r - 0.501961;\n"
    "    return float4(y + 1.596027 * v, y - 0.391762 * u - 0.812968 * v, y + 2.017232 * u, 1);\n"
    "}\n";

static ID3DXBuffer *compile(const char *src, const char *profile)
{
    ID3DXBuffer *code = NULL, *errors = NULL;
    HRESULT hr = D3DXCompileShader(src, (UINT)strlen(src), NULL, NULL, "main", profile, 0, &code, &errors, NULL);

    if (errors) {
        OutputDebugStringA((const char *)errors->GetBufferPointer());
        errors->Release();
    }
    return SUCCEEDED(hr) ? code : NULL;
}

bool gfx_init(void)
{
    static const D3DVERTEXELEMENT9 elements[] = {
        { 0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 8, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        D3DDECL_END()
    };
    const char *ps_src[2] = { ps_rgb_src, ps_yuv_src };
    Direct3D *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp;
    ID3DXBuffer *code;
    int i;

    if (d3d == NULL)
        return false;
    ZeroMemory(&pp, sizeof(pp));
    pp.BackBufferWidth = GFX_WIDTH;
    pp.BackBufferHeight = GFX_HEIGHT;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.MultiSampleType = D3DMULTISAMPLE_NONE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    if (FAILED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)))
        return false;

    if (FAILED(dev->CreateVertexDeclaration(elements, &decl)))
        return false;
    code = compile(vs_src, "vs_3_0");
    if (code == NULL || FAILED(dev->CreateVertexShader((const DWORD *)code->GetBufferPointer(), &vs)))
        return false;
    code->Release();
    for (i = 0; i < 2; i++) {
        code = compile(ps_src[i], "ps_3_0");
        if (code == NULL || FAILED(dev->CreatePixelShader((const DWORD *)code->GetBufferPointer(), &ps[i])))
            return false;
        code->Release();
    }
    for (i = 0; i < 3; i++) {
        dev->SetSamplerState(i, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        dev->SetSamplerState(i, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        dev->SetSamplerState(i, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->SetSamplerState(i, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(i, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);

    InitializeCriticalSection(&dev_lock);
    dev->ReleaseThreadOwnership();   /* gfx_begin() takes it per use */
    return true;
}

D3DDevice *gfx_begin(void)
{
    EnterCriticalSection(&dev_lock);
    dev->AcquireThreadOwnership();
    return dev;
}

void gfx_end(void)
{
    dev->ReleaseThreadOwnership();
    LeaveCriticalSection(&dev_lock);
}

void gfx_draw_rect(D3DDevice *d, int shader, float x0, float y0, float x1, float y1)
{
    struct vertex q[4];
    float l = x0 / GFX_WIDTH * 2 - 1, r = x1 / GFX_WIDTH * 2 - 1;
    float t = 1 - y0 / GFX_HEIGHT * 2, b = 1 - y1 / GFX_HEIGHT * 2;

    q[0].x = l; q[0].y = t; q[0].u = 0; q[0].v = 0;
    q[1].x = r; q[1].y = t; q[1].u = 1; q[1].v = 0;
    q[2].x = l; q[2].y = b; q[2].u = 0; q[2].v = 1;
    q[3].x = r; q[3].y = b; q[3].u = 1; q[3].v = 1;
    d->SetVertexDeclaration(decl);
    d->SetVertexShader(vs);
    d->SetPixelShader(ps[shader]);
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(q[0]));
}

void gfx_upload(D3DTexture *tex, const void *src, int src_pitch, int row_bytes, int rows)
{
    D3DLOCKED_RECT lr;
    const BYTE *s = (const BYTE *)src;
    BYTE *d;
    int y;

    if (FAILED(tex->LockRect(0, &lr, NULL, 0)))
        return;
    d = (BYTE *)lr.pBits;
    for (y = 0; y < rows; y++)
        memcpy(d + y * lr.Pitch, s + y * src_pitch, row_bytes);
    tex->UnlockRect(0);
}

/* ------------------------------------------------------------ UI */

/* Two textures, so the CPU fills one while the GPU may still read the other. */
static D3DTexture *ui_tex[2];
static DWORD ui_fence[2];
static int ui_next;

void ui_present(void)
{
    canvas_t *c = ui_canvas();
    D3DDevice *d = gfx_begin();
    int k = ui_next;

    ui_next ^= 1;
    if (ui_tex[k] == NULL &&
        FAILED(d->CreateTexture(c->w, c->h, 1, 0, D3DFMT_LIN_A8R8G8B8, 0, &ui_tex[k], NULL))) {
        ui_tex[k] = NULL;
        gfx_end();
        return;
    }
    if (ui_fence[k])
        d->BlockOnFence(ui_fence[k]);
    gfx_upload(ui_tex[k], c->px, c->w * 4, c->w * 4, c->h);

    d->SetTexture(0, ui_tex[k]);
    gfx_draw_rect(d, GFX_SHADER_RGB, 0, 0, GFX_WIDTH, GFX_HEIGHT);
    d->SetTexture(0, NULL);
    ui_fence[k] = d->InsertFence();
    d->Present(NULL, NULL, NULL, NULL);
    gfx_end();
}
