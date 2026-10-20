/* The rendering back end: Direct3D 11.
 *
 * The game's render targets live on the host GPU: each is a D3D11 texture keyed by the guest
 * address the console would have rendered into. When the display shows a buffer, the
 * matching texture is drawn into the window.
 *
 * Every entry point takes the back end's lock: GPU work arrives from the game's submitting
 * threads, frames from the display's vsync thread. */
#define COBJMACROS
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>

#include "debug_profile.h"
#include "display.h"
#include "gpu_backend.h"
#include "gpu_overlay.h"
#include "net.h"
#include "pad.h"
#include "gpu_shader.h"
#include "gpu_shader_cache.h"
#include "gpu_texture.h"
#include <math.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_5.h>

static HWND window;
static ID3D11Device *dev, *compile_dev;
static ID3D11DeviceContext *ctx;
static IDXGISwapChain1 *swapchain;
static UINT swapchain_flags;
/* The game's picture at its own size, before it is fitted into the window (screenshots read it). */
static ID3D11Texture2D *final_tex;
static ID3D11RenderTargetView *final_rtv;
static ID3D11ShaderResourceView *final_srv;
enum { FINAL_W = 1920, FINAL_H = 1080 };
static ID3D11VertexShader *blit_vs;
static ID3D11PixelShader *blit_ps, *blit2d_ps, *blit_alpha_ps;
static ID3D11SamplerState *linear_sampler;
static CRITICAL_SECTION lock;
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
static int ready;

u64 gfx_stat_clears, gfx_stat_presented;

/* ---- surfaces: render targets on the host ------------------------------------------- */
typedef struct Surface {
    u64 guest;
    u32 width, height, format, depth;
    DXGI_FORMAT storage, view;            /* the texture's format, and how it is rendered to and sampled */
    ID3D11Texture2D *tex;
    ID3D11RenderTargetView *rtv;
    ID3D11DepthStencilView *dsv;
    ID3D11ShaderResourceView *srv;
    ID3D11ShaderResourceView *srv_other;  /* 8-bit colour: the other reading of the bytes (sRGB / plain) */
    u64 used;
} Surface;

static Surface surfaces[2048];
static u32 nsurfaces;
static void bury(void *object);   /* (released after the frame is presented) */
static u64 tick;

/* Maxwell render target formats (color) to DXGI. */
static DXGI_FORMAT color_format(u32 f) {
    switch (f) {
    case 0xC0: case 0xC3: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case 0xC1: case 0xC4: return DXGI_FORMAT_R32G32B32A32_SINT;
    case 0xC2: case 0xC5: return DXGI_FORMAT_R32G32B32A32_UINT;
    case 0xC6: return DXGI_FORMAT_R16G16B16A16_UNORM;
    case 0xC7: return DXGI_FORMAT_R16G16B16A16_SNORM;
    case 0xC8: return DXGI_FORMAT_R16G16B16A16_SINT;
    case 0xC9: return DXGI_FORMAT_R16G16B16A16_UINT;
    case 0xCA: case 0xCE: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 0xCB: return DXGI_FORMAT_R32G32_FLOAT;
    case 0xCC: return DXGI_FORMAT_R32G32_SINT;
    case 0xCD: return DXGI_FORMAT_R32G32_UINT;
    case 0xCF: case 0xE6: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case 0xD0: case 0xE7: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case 0xD1: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case 0xD2: return DXGI_FORMAT_R10G10B10A2_UINT;
    case 0xD5: case 0xF9: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case 0xD6: case 0xFA: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case 0xD7: return DXGI_FORMAT_R8G8B8A8_SNORM;
    case 0xD8: return DXGI_FORMAT_R8G8B8A8_SINT;
    case 0xD9: return DXGI_FORMAT_R8G8B8A8_UINT;
    case 0xDA: return DXGI_FORMAT_R16G16_UNORM;
    case 0xDB: return DXGI_FORMAT_R16G16_SNORM;
    case 0xDC: return DXGI_FORMAT_R16G16_SINT;
    case 0xDD: return DXGI_FORMAT_R16G16_UINT;
    case 0xDE: return DXGI_FORMAT_R16G16_FLOAT;
    case 0xE0: return DXGI_FORMAT_R11G11B10_FLOAT;
    case 0xE3: return DXGI_FORMAT_R32_SINT;
    case 0xE4: return DXGI_FORMAT_R32_UINT;
    case 0xE5: return DXGI_FORMAT_R32_FLOAT;
    case 0xE8: return DXGI_FORMAT_B5G6R5_UNORM;
    case 0xE9: case 0xF8: return DXGI_FORMAT_B5G5R5A1_UNORM;
    case 0xEA: return DXGI_FORMAT_R8G8_UNORM;
    case 0xEB: return DXGI_FORMAT_R8G8_SNORM;
    case 0xEC: return DXGI_FORMAT_R8G8_SINT;
    case 0xED: return DXGI_FORMAT_R8G8_UINT;
    case 0xEE: return DXGI_FORMAT_R16_UNORM;
    case 0xEF: return DXGI_FORMAT_R16_SNORM;
    case 0xF0: return DXGI_FORMAT_R16_SINT;
    case 0xF1: return DXGI_FORMAT_R16_UINT;
    case 0xF2: return DXGI_FORMAT_R16_FLOAT;
    case 0xF3: return DXGI_FORMAT_R8_UNORM;
    case 0xF4: return DXGI_FORMAT_R8_SNORM;
    case 0xF5: return DXGI_FORMAT_R8_SINT;
    case 0xF6: return DXGI_FORMAT_R8_UINT;
    }
    return DXGI_FORMAT_UNKNOWN;
}

/* Maxwell depth formats: the texture's typeless storage, and how it is used as depth. */
static DXGI_FORMAT depth_format(u32 f, DXGI_FORMAT *typeless) {
    switch (f) {
    case 0x0A: *typeless = DXGI_FORMAT_R32_TYPELESS; return DXGI_FORMAT_D32_FLOAT;              /* Z32_FLOAT */
    case 0x13: *typeless = DXGI_FORMAT_R16_TYPELESS; return DXGI_FORMAT_D16_UNORM;              /* Z16_UNORM */
    case 0x14: case 0x15: case 0x16:                                                            /* Z24 + S8 variants */
        *typeless = DXGI_FORMAT_R24G8_TYPELESS; return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case 0x19: *typeless = DXGI_FORMAT_R32G8X24_TYPELESS; return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;  /* Z32 + S8 */
    }
    *typeless = DXGI_FORMAT_UNKNOWN;
    return DXGI_FORMAT_UNKNOWN;
}

static void release_views(Surface *s) {
    if (s->rtv) ID3D11RenderTargetView_Release(s->rtv);
    if (s->dsv) ID3D11DepthStencilView_Release(s->dsv);
    if (s->srv) ID3D11ShaderResourceView_Release(s->srv);
    if (s->srv_other) ID3D11ShaderResourceView_Release(s->srv_other);
    s->rtv = NULL; s->dsv = NULL; s->srv = NULL; s->srv_other = NULL;
}

static void release_surface(Surface *s) {
    release_views(s);
    if (s->tex) ID3D11Texture2D_Release(s->tex);
    memset(s, 0, sizeof *s);
}

/* 8-bit colour is stored typeless: the hardware writes and reads the same bytes as sRGB or as
 * plain values according to each use's format (render target, texture header), and a surface
 * keeps its contents across such changes. */
static DXGI_FORMAT storage_of(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    default: return f;
    }
}
static int is_srgb(DXGI_FORMAT f) { return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; }
static DXGI_FORMAT other_gamma(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

/* How a depth surface is sampled (shadow maps): its depth as the red channel. */
static DXGI_FORMAT depth_sampled(DXGI_FORMAT dsv) {
    switch (dsv) {
    case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

static ID3D11ShaderResourceView *make_srv(ID3D11Texture2D *tex, DXGI_FORMAT f) {
    D3D11_SHADER_RESOURCE_VIEW_DESC sd;
    memset(&sd, 0, sizeof sd);
    sd.Format = f;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView *v = NULL;
    if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)tex, &sd, &v))) return NULL;
    return v;
}

/* The views of a surface rendered as `view` (a colour format, or a depth one). */
static void make_views(Surface *s, DXGI_FORMAT view) {
    s->view = view;
    if (s->depth) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dd = {view, D3D11_DSV_DIMENSION_TEXTURE2D, 0};
        ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)s->tex, &dd, &s->dsv);
        DXGI_FORMAT sampled = depth_sampled(view);
        if (sampled != DXGI_FORMAT_UNKNOWN) s->srv = make_srv(s->tex, sampled);
    } else {
        D3D11_RENDER_TARGET_VIEW_DESC rd;
        memset(&rd, 0, sizeof rd);
        rd.Format = view;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)s->tex, &rd, &s->rtv);
        s->srv = make_srv(s->tex, view);
    }
}

/* A surface's bytes read as sRGB (srgb) or as plain values, for an 8-bit colour surface; its
 * own view otherwise. */
static ID3D11ShaderResourceView *surface_srv(Surface *s, int srgb) {
    DXGI_FORMAT other = other_gamma(s->view);
    if (other == DXGI_FORMAT_UNKNOWN || srgb == is_srgb(s->view)) return s->srv;
    if (!s->srv_other) s->srv_other = make_srv(s->tex, other);
    return s->srv_other ? s->srv_other : s->srv;
}

static void warn_format(const char *what, u32 f) {
    static u32 seen[64], n;
    for (u32 i = 0; i < n; i++) if (seen[i] == (f | (what[0] << 24))) return;
    if (n < 64) seen[n++] = f | (what[0] << 24);
    fprintf(stderr, "[gfx] unsupported %s format %#x\n", what, f);
}

/* The host texture standing for the render target at `guest`, created on first use. */
static Surface *surface(u64 guest, u32 width, u32 height, u32 format, int depth) {
    if (!guest || !width || !height || width > 16384 || height > 16384) return NULL;
    Surface *free_slot = NULL;
    for (u32 i = 0; i < nsurfaces; i++) {
        Surface *s = &surfaces[i];
        if (!s->tex) { if (!free_slot) free_slot = s; continue; }
        if (s->guest == guest && s->depth == depth) {
            if (s->width == width && s->height == height && s->format == format) { s->used = ++tick; return s; }
            DXGI_FORMAT now = depth ? DXGI_FORMAT_UNKNOWN : color_format(format);
            if (s->width == width && s->height == height && now != DXGI_FORMAT_UNKNOWN && storage_of(now) == s->storage) {
                bury(s->rtv); bury(s->srv); bury(s->srv_other);   /* the same bytes, now sRGB or plain: new views (the old may be bound) */
                s->rtv = NULL; s->srv = NULL; s->srv_other = NULL;
                make_views(s, now);
                s->format = format;
                s->used = ++tick;
                return s;
            }
            release_surface(s);   /* the memory now holds a different surface */
            if (!free_slot) free_slot = s;
        }
    }
    DXGI_FORMAT view, storage;
    if (depth) view = depth_format(format, &storage);
    else storage = storage_of(view = color_format(format));
    if (view == DXGI_FORMAT_UNKNOWN) { warn_format(depth ? "depth" : "color", format); return NULL; }

    Surface *s = free_slot;
    if (!s) {
        if (nsurfaces < sizeof surfaces / sizeof *surfaces) s = &surfaces[nsurfaces++];
        else {   /* evict the least recently used */
            s = &surfaces[0];
            for (u32 i = 1; i < nsurfaces; i++) if (surfaces[i].used < s->used) s = &surfaces[i];
            release_surface(s);
        }
    }
    D3D11_TEXTURE2D_DESC td = {width, height, 1, 1, storage, {1, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_SHADER_RESOURCE | (depth ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET), 0, 0};
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &s->tex))) { s->tex = NULL; return NULL; }
    s->depth = depth;
    s->storage = storage;
    make_views(s, view);
    s->guest = guest; s->width = width; s->height = height; s->format = format;
    s->used = ++tick;
    return s;
}

/* ---- window and device ------------------------------------------------------------ */
/* Debug keys, taken at the next present: F1 the overlay, F2 its details, F3 a screenshot,
 * F4 a log of the next frame's draws and its render targets, F5 depth sampling on/off, F6 a
 * 10-second sampling profile. */
static volatile LONG hotkeys;

#ifdef _WIN32
static LRESULT CALLBACK window_proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    LRESULT result;
    if (display_message(h, msg, w, l, &result)) return result;   /* size, full screen, pointer (gpu_display.c) */
    if (msg == WM_CLOSE) { fprintf(stderr, "[gfx] window closed\n"); ExitProcess(0); }
    if (msg == WM_KEYDOWN && w >= VK_F1 && w <= VK_F6 && !(l & (1 << 30))) InterlockedOr(&hotkeys, 1 << (w - VK_F1));
    return DefWindowProcW(h, msg, w, l);
}

static DWORD WINAPI window_thread(LPVOID done) {
    SetThreadDescription(GetCurrentThread(), L"window");
    WNDCLASSW wc = {CS_HREDRAW | CS_VREDRAW, window_proc, 0, 0, GetModuleHandleW(NULL), NULL,
                    LoadCursor(NULL, IDC_ARROW), NULL, NULL, L"ssbu"};
    RegisterClassW(&wc);
    window = display_create_window(wc.hInstance, L"ssbu", L"Super Smash Bros. Ultimate (native)");
    SetEvent((HANDLE)done);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    return 0;
}
#else
/* Linux: the window is SDL's, made and run by the main thread (linux/window.c), which passes
 * the debug keys on here; DXVK takes the SDL_Window as the swap chain's HWND. */
void *rt_window(void);
void gfx_hotkey(int n) { InterlockedOr(&hotkeys, 1 << n); }
#endif

static ID3DBlob *compile_hlsl(const char *src, const char *entry, const char *target) {
    ID3DBlob *code = NULL, *errors = NULL;
    if (FAILED(D3DCompile(src, strlen(src), NULL, NULL, NULL, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &errors))) {
        fprintf(stderr, "[gfx] shader compile failed: %s\n", errors ? (char *)ID3D10Blob_GetBufferPointer(errors) : "?");
        rt_fatal("cannot compile a built-in shader");
    }
    if (errors) ID3D10Blob_Release(errors);
    return code;
}

static const char BLIT_HLSL[] =
    "Texture2D t : register(t0); SamplerState s : register(s0);\n"
    "struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V vs(uint id : SV_VertexID) { V o; o.uv = float2((id << 1) & 2, id & 2);\n"
    "  o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
    "float4 ps(V i) : SV_Target { return float4(t.Sample(s, i.uv).rgb, 1); }\n"
    "float4 psa(V i) : SV_Target { float a = t.Sample(s, i.uv).a; return float4(a, a, a, 1); }\n"   /* debug: alpha as grey */
    "cbuffer K : register(b2) { float4 kcolor; };\n"
    "float4 psclear(V i) : SV_Target { return kcolor; }\n"   /* a clear of some channels: drawn, with a write mask */
    /* the 2D engine's blit: destination pixel p reads source point src0 + (p - dst0) * step */
    "cbuffer R : register(b1) { float4 src0_step; float4 dst0_inv; };\n"
    "float4 ps2d(V i) : SV_Target { return t.Sample(s, (src0_step.xy + (i.pos.xy - dst0_inv.xy) * src0_step.zw) * dst0_inv.zw); }\n";

static void start_compilers(void);                 /* (see "shaders") */
static DWORD WINAPI prewarm_main(void *unused);

/* Direct3D 11 is DXVK's: always on Linux; on Windows when d3d11.dll is not the system's (the
 * DXVK test builds of tools/linux/wintest.py). */
static int on_dxvk(void) {
#ifdef _WIN32
    wchar_t path[MAX_PATH], sys[MAX_PATH];
    HMODULE m = GetModuleHandleW(L"d3d11.dll");
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    return m && n && n < MAX_PATH && GetModuleFileNameW(m, path, MAX_PATH) && _wcsnicmp(path, sys, n) != 0;
#else
    return 1;
#endif
}

static BOOL CALLBACK init(PINIT_ONCE o, PVOID p, PVOID *c) {
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&lock);
#ifdef _WIN32
    HANDLE done = CreateEventW(NULL, TRUE, FALSE, NULL);
    CreateThread(NULL, 0, window_thread, done, 0, NULL);
    WaitForSingleObject(done, INFINITE);
#else
    window = (HWND)rt_window();
#endif
    if (!window) { fprintf(stderr, "[gfx] cannot create the window\n"); return TRUE; }

    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (getenv("SSBU_D3D_DEBUG")) flags |= D3D11_CREATE_DEVICE_DEBUG;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags, levels, 2, D3D11_SDK_VERSION, &dev, NULL, &ctx))) {
        fprintf(stderr, "[gfx] cannot create a Direct3D 11 device\n");
        return TRUE;
    }
    IDXGIDevice *dxgi_dev; IDXGIAdapter *adapter; IDXGIFactory2 *factory;
    ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&dxgi_dev);
    IDXGIDevice_GetAdapter(dxgi_dev, &adapter);
    IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2, (void **)&factory);
    /* NVIDIA's: a second device, with the driver's internal threading off, that compiles each
     * shader first on our compiler threads (see "shaders"). SSBU_COMPILE_DEVICE=0/1 overrides. */
    DXGI_ADAPTER_DESC ad;
    int nvidia = SUCCEEDED(IDXGIAdapter_GetDesc(adapter, &ad)) && ad.VendorId == 0x10DE;
#ifndef _WIN32
    {   /* (DXVK: which Vulkan device it chose) */
        char name[128];
        size_t i = 0;
        for (; i < 127 && ad.Description[i]; i++) name[i] = ad.Description[i] < 128 ? (char)ad.Description[i] : '?';
        name[i] = 0;
        fprintf(stderr, "[gfx] Direct3D 11 through DXVK on %s (vendor %#x)\n", name, ad.VendorId);
        if (ad.VendorId == 0x10005 || strstr(name, "llvmpipe") || strstr(name, "lavapipe"))
            fprintf(stderr, "[gfx] note: this is a software renderer (no Vulkan GPU driver found): expect a low frame rate\n");
    }
    nvidia = 0;   /* (the compile device is for NVIDIA's Direct3D driver; DXVK compiles its own way) */
#endif
    const char *cd = getenv("SSBU_COMPILE_DEVICE");
    if ((cd ? atoi(cd) : nvidia) &&
        FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, flags | D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS,
                                 levels, 2, D3D11_SDK_VERSION, &compile_dev, NULL, NULL)))
        compile_dev = NULL;
    /* The back buffer is the window's inside (display.ini sizes the window). Where the system
     * allows it, the swap chain may tear, which VRR pacing asks for at each Present. */
    IDXGIFactory5 *factory5;
    BOOL tearing = FALSE;
    if (SUCCEEDED(IDXGIFactory2_QueryInterface(factory, &IID_IDXGIFactory5, (void **)&factory5))) {
        if (FAILED(IDXGIFactory5_CheckFeatureSupport(factory5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing))) tearing = FALSE;
        IDXGIFactory5_Release(factory5);
    }
    swapchain_flags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    display_set_tearing(tearing != FALSE);
    DXGI_SWAP_CHAIN_DESC1 sd = {0, 0, DXGI_FORMAT_R8G8B8A8_UNORM, FALSE, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT, 2,
                                DXGI_SCALING_STRETCH, DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_ALPHA_MODE_UNSPECIFIED, swapchain_flags};
    if (FAILED(IDXGIFactory2_CreateSwapChainForHwnd(factory, (IUnknown *)dev, window, &sd, NULL, NULL, &swapchain))) {
        fprintf(stderr, "[gfx] cannot create the swap chain\n");
        return TRUE;
    }
    /* full screen is gpu_display.c's (a borderless window), never DXGI's own */
    IDXGIFactory2_MakeWindowAssociation(factory, window, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
    IDXGIDevice1 *dxgi_dev1;
    if (SUCCEEDED(IDXGIDevice_QueryInterface(dxgi_dev, &IID_IDXGIDevice1, (void **)&dxgi_dev1))) {
        IDXGIDevice1_SetMaximumFrameLatency(dxgi_dev1, (UINT)display_latency());
        IDXGIDevice1_Release(dxgi_dev1);
    }
    IDXGIFactory2_Release(factory); IDXGIAdapter_Release(adapter); IDXGIDevice_Release(dxgi_dev);

    ID3DBlob *vs = compile_hlsl(BLIT_HLSL, "vs", "vs_5_0"), *ps = compile_hlsl(BLIT_HLSL, "ps", "ps_5_0");
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vs), ID3D10Blob_GetBufferSize(vs), NULL, &blit_vs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(ps), ID3D10Blob_GetBufferSize(ps), NULL, &blit_ps);
    ID3D10Blob_Release(vs); ID3D10Blob_Release(ps);
    if ((ps = compile_hlsl(BLIT_HLSL, "psa", "ps_5_0"))) {
        ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(ps), ID3D10Blob_GetBufferSize(ps), NULL, &blit_alpha_ps);
        ID3D10Blob_Release(ps);
    }
    if ((ps = compile_hlsl(BLIT_HLSL, "ps2d", "ps_5_0"))) {
        ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(ps), ID3D10Blob_GetBufferSize(ps), NULL, &blit2d_ps);
        ID3D10Blob_Release(ps);
    }
    D3D11_SAMPLER_DESC smp = {D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
                              D3D11_TEXTURE_ADDRESS_CLAMP, 0, 1, D3D11_COMPARISON_NEVER, {0}, 0, D3D11_FLOAT32_MAX};
    ID3D11Device_CreateSamplerState(dev, &smp, &linear_sampler);
    D3D11_TEXTURE2D_DESC fd = {FINAL_W, FINAL_H, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &fd, NULL, &final_tex)) ||
        FAILED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)final_tex, NULL, &final_rtv)) ||
        FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)final_tex, NULL, &final_srv))) {
        fprintf(stderr, "[gfx] cannot create the picture's texture\n");
        return TRUE;
    }
    start_compilers();
    /* The prewarm is for a driver that compiles a shader when it is created (NVIDIA's Direct3D:
     * 190 ms at first use). DXVK queues a Vulkan pipeline library per shader on its own workers
     * instead: the whole cache at once kept them busy for minutes and the first frame waited
     * behind them (5,948 shaders: 600 CPU-seconds, no frame for over 30 s). So under DXVK it is
     * off unless SSBU_SHADER_PREWARM=1. */
    const char *prewarm = getenv("SSBU_SHADER_PREWARM");
    if (prewarm ? atoi(prewarm) != 0 : !on_dxvk()) CloseHandle(CreateThread(NULL, 0, prewarm_main, NULL, 0, NULL));
    else if (!prewarm) fprintf(stderr, "[gfx] prewarm: off under DXVK (SSBU_SHADER_PREWARM=1 turns it on)\n");
    ready = 1;
    fprintf(stderr, "[gfx] Direct3D 11 ready%s\n", compile_dev ? " (shaders compile on a device of their own first)" : "");
    return TRUE;
}

/* Time spent in the renderer (the outermost enter to its leave), for the overlay. */
static u32 enter_depth;
static LARGE_INTEGER enter_t0;
u64 gfx_stat_busy_ticks, gfx_stat_upload_bytes, gfx_stat_gpu_waits, gfx_stat_composed, gfx_stat_upload_kind[4];
static struct { u32 size, bank, extent; char shader[24]; } gfx_stat_biggest_cb;

static volatile DWORD last_enter_thread;
DWORD gfx_lock_owner(void) { return ready ? (DWORD)(uintptr_t)lock.OwningThread : 0; }
DWORD gfx_last_enter_thread(void) { return last_enter_thread; }

static int enter(void) {
    InitOnceExecuteOnce(&once, init, NULL, NULL);
    if (!ready) return 0;
    EnterCriticalSection(&lock);
    last_enter_thread = GetCurrentThreadId();
    if (!enter_depth++) QueryPerformanceCounter(&enter_t0);
    return 1;
}

static void leave(void) {
    if (!--enter_depth) {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        gfx_stat_busy_ticks += (u64)(t.QuadPart - enter_t0.QuadPart);
    }
    LeaveCriticalSection(&lock);
}

static float f32_of(u32 bits) { float f; memcpy(&f, &bits, 4); return f; }

/* ---- the 3D engine's work -------------------------------------------------------- */
#define RT(i, field) regs[0x200 + (i) * 0x10 + (field)]

static void clear_channels(Surface *s, const float color[4], u32 mask);   /* (below: it draws) */
static void forget_draw_state(void);
static int draw_logging;   /* (below) */
static void note_sampled(Surface *s);   /* (F4, below) */
static void save_sampled(void);

void gfx_clear(const u32 *regs, u32 arg) {
    if (!enter()) return;
    if (draw_logging) save_sampled();
    gfx_stat_clears++;
    u32 rt = (arg >> 6) & 0xF;
    if (draw_logging)
        fprintf(stderr, "[draw] clear rt %u (%#llx %ux%u fmt %#x) channels %#x%s, zt %#llx: color %g %g %g %g depth %g stencil %#x\n", rt,
                gpu_va_to_guest((u64)RT(rt & 7, 0) << 32 | RT(rt & 7, 1)), RT(rt & 7, 2), RT(rt & 7, 3), RT(rt & 7, 4), (arg >> 2) & 0xF,
                arg & 3 ? (arg & 1 ? " +depth" : " +stencil") : "", gpu_va_to_guest((u64)regs[0x3F8] << 32 | regs[0x3F9]),
                f32_of(regs[0x360]), f32_of(regs[0x361]), f32_of(regs[0x362]), f32_of(regs[0x363]), f32_of(regs[0x364]), regs[0x368] & 0xFF);
    if (arg & 0x3C && rt < 8) {   /* any of R, G, B, A */
        u64 va = (u64)RT(rt, 0) << 32 | RT(rt, 1);
        Surface *s = surface(gpu_va_to_guest(va), RT(rt, 2), RT(rt, 3), RT(rt, 4), 0);
        if (s) {
            float color[4] = {f32_of(regs[0x360]), f32_of(regs[0x361]), f32_of(regs[0x362]), f32_of(regs[0x363])};
            if (((arg >> 2) & 0xF) == 0xF) ID3D11DeviceContext_ClearRenderTargetView(ctx, s->rtv, color);
            else clear_channels(s, color, (arg >> 2) & 0xF);   /* only some channels: a masked draw */
        }
    }
    if (arg & 3) {   /* depth, stencil */
        u64 va = (u64)regs[0x3F8] << 32 | regs[0x3F9];
        Surface *s = surface(gpu_va_to_guest(va), regs[0x48A], regs[0x48B], regs[0x3FA], 1);
        if (s) ID3D11DeviceContext_ClearDepthStencilView(ctx, s->dsv, (arg & 1 ? D3D11_CLEAR_DEPTH : 0) | (arg & 2 ? D3D11_CLEAR_STENCIL : 0),
                                                         f32_of(regs[0x364]), (UINT8)regs[0x368]);
    }
    leave();
}

/* SSBU_DUMP_SHADERS=<dir>: save each distinct shader program a draw uses, as
 * <pipeline stage>_<hash>.bin (the 0x50-byte program header, then the code). */
static void dump_shaders(const u32 *regs) {
    static const char *dir;
    static int checked;
    static u64 seen[8192];
    static u32 nseen;
    if (!checked++) dir = getenv("SSBU_DUMP_SHADERS");
    if (!dir) return;
    u64 region = (u64)regs[M_PROGRAM_REGION] << 32 | regs[M_PROGRAM_REGION + 1];
    static int shown;
    if (shown++ < 0)
        for (u32 j = 0; j < 6; j++)
            fprintf(stderr, "[gfx] pipeline %u: %08x %08x %08x %08x %08x (region %#llx)\n", j, regs[M_PIPELINE(j)],
                    regs[M_PIPELINE(j) + 1], regs[M_PIPELINE(j) + 2], regs[M_PIPELINE(j) + 3], regs[M_PIPELINE(j) + 4], region);
    for (u32 j = 0; j < 6; j++) {
        if (j != 1 && !(regs[M_PIPELINE(j)] & 1)) continue;   /* vertex stage B is always on */
        u64 g = gpu_va_to_guest(region + regs[M_PIPELINE(j) + 1]);
        if (!g) continue;
        u32 size = 0x50;   /* up to the first EXIT that no branch jumps past, then the padding loop */
        while (size < 0x40000 && ld64(g + size) != 0xE2400FFFFF87000Full) size += 8;
        u64 h = 1469598103934665603ull;
        for (u32 i = 0; i < size; i++) h = (h ^ ld8(g + i)) * 1099511628211ull;
        h ^= j;
        int known = 0;
        for (u32 i = 0; i < nseen; i++) known |= seen[i] == h;
        if (known || nseen == 8192) continue;
        seen[nseen++] = h;
        char path[1024];
        snprintf(path, sizeof path, "%s/%u_%016llx.bin", dir, j, h);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(P(void, g), 1, size, f); fclose(f); }
    }
}

/* ---- shaders ------------------------------------------------------------------------ */
/* Translated at their first draw (on the submitting thread, well under a millisecond), then
 * made ready by compiler threads of our own: from the DXBC an earlier run kept on disk, else
 * from fxc's (seconds on large programs, minutes on some dispatch loops) - cache loads first.
 * Left to itself NVIDIA's driver compiles to GPU code after Create*Shader returns, on threads
 * with 64 KB stacks (which one of our programs overflowed), and the first draw with the shader
 * waits for that (the hitch when a move is first used). So a second device, with the driver's
 * threading off, compiles each one first - inside Create*Shader, on our thread, with our stack;
 * the rendering device keeps its threading (without it the renderer took twice as long a
 * frame). A draw whose
 * shaders are not ready is skipped. The disk cache's names, and fxc's flags, are
 * gpu_shader_cache.h's (shared with tools/shaderaot.c). */

typedef struct Shader {
    u64 hash, table_hash;
    u32 vs_outputs;
    int stage;
    volatile LONG state;   /* 0 compiling, 1 ready, 2 failed */
    void *obj;             /* ID3D11VertexShader, ID3D11PixelShader or ID3D11ComputeShader */
    char *hlsl;            /* while compiling */
    char name[64];
    ShaderInfo info;
    u32 written_mask, learn_runs;   /* compute: storage buffers it was seen to write, over its first runs */
    struct Shader *next_job;
    int cached;            /* an earlier run compiled it (its job is a load) */
    struct Prewarm *prewarm;   /* a prewarm job (not a shader of the game yet): the entry it fills */
} Shader;

static Shader **shader_cache;
static u32 shader_count, shader_cap;
u64 gfx_stat_shaders, gfx_stat_shader_failures;
static volatile LONG compiles_pending;

/* The compiler threads: a queue of cache loads, one of compiles. Big stacks (the driver's
 * compiler recurses over a program's blocks), below the game's threads in priority. */
#define COMPILERS_MAX 16
#define COMPILER_STACK (64ull << 20)
enum { STEP_IDLE, STEP_CACHE, STEP_FXC, STEP_DRIVER };
static SRWLOCK jobs_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE jobs_cv = CONDITION_VARIABLE_INIT;
/* [0] loads from the disk cache, [1] compiles, [2] prewarm (the disk cache at start; never
 * taken by the first thread, which stays free for the game's own shaders) */
static struct { Shader *head, *tail; } job_queue[3];
static struct Compiler {
    volatile DWORD thread;
    Shader *volatile job;
    volatile LONG step;
    volatile u64 since_ms;
    const void *volatile dxbc;   /* what the driver is compiling (STEP_DRIVER) */
    volatile size_t dxbc_size;
} compilers[COMPILERS_MAX];
static u32 ncompilers;

static const char *cache_dir(void) {
    static char dir[1024];
    if (!dir[0]) {   /* SSBU_SHADER_CACHE: another directory (an empty one: a first run's compiles) */
        if (getenv("SSBU_SHADER_CACHE")) snprintf(dir, sizeof dir, "%s", getenv("SSBU_SHADER_CACHE"));
        else snprintf(dir, sizeof dir, "%s/../shadercache", rt_data_dir);
        CreateDirectoryA(dir, NULL);
    }
    return dir;
}

static void cache_path(const Shader *s, char *path, size_t n) {
    shader_cache_file(path, n, cache_dir(), s->stage, s->hash, s->vs_outputs, s->table_hash);
}

/* A file the ahead-of-time compiler wrote (dated 2001), now used by the game: dated now, so the
 * next start's prewarm makes it. */
static void cache_touch(const char *path) {
    HANDLE h = CreateFileA(path, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME written, now;
    if (GetFileTime(h, NULL, NULL, &written) && ((u64)written.dwHighDateTime << 32 | written.dwLowDateTime) < SHADER_CACHE_PLAYED) {
        GetSystemTimeAsFileTime(&now);
        SetFileTime(h, NULL, NULL, &now);
    }
    CloseHandle(h);
}

static HRESULT create_on(ID3D11Device *d, int stage, const void *code, size_t size, void **obj) {
    return stage == SHADER_VERTEX ? ID3D11Device_CreateVertexShader(d, code, size, NULL, (ID3D11VertexShader **)obj)
         : stage == SHADER_PIXEL  ? ID3D11Device_CreatePixelShader(d, code, size, NULL, (ID3D11PixelShader **)obj)
                                  : ID3D11Device_CreateComputeShader(d, code, size, NULL, (ID3D11ComputeShader **)obj);
}

/* (on a compiler thread) the driver's compile of DXBC, noted for crash reports: on the compile
 * device, which compiles right here (its object is not needed: the rendering device finds the
 * GPU code in the driver's cache - measured 3.5 ms of its time a shader instead of 190 ms),
 * then the object the draws use */
static int create_noted(struct Compiler *me, Shader *s, const void *code, size_t size) {
    me->dxbc = code;
    me->dxbc_size = size;
    me->step = STEP_DRIVER;
    void *warm = NULL;
    if (compile_dev && SUCCEEDED(create_on(compile_dev, s->stage, code, size, &warm))) IUnknown_Release((IUnknown *)warm);
    int ok = SUCCEEDED(create_on(dev, s->stage, code, size, &s->obj));
    me->dxbc = NULL;
    return ok;
}

/* ---- prewarm: the disk cache made ready while the game starts --------------------------
 * Every DXBC file of this SHADER_CACHE_VERSION the game has used before (gpu_shader_cache.h),
 * newest first, is created on both devices by the compiler threads (queue 2, below the game's
 * own shaders) into a table by the cache's key.
 * When the game first uses a program, get_shader_ex still translates it (that gives its
 * interface), then takes the object from here instead of loading it, so the draw is not
 * skipped. SSBU_SHADER_PREWARM=0: no prewarm; under DXVK it is off unless =1 (see init). */
typedef struct Prewarm { u64 hash, table_hash; u32 vs_outputs; int stage; LONG state; void *obj; } Prewarm;
enum { PW_EMPTY, PW_QUEUED, PW_LOADING, PW_READY, PW_TAKEN, PW_FAILED };
#define PREWARM_SLOTS 32768
static Prewarm *volatile prewarm_table;
static SRWLOCK prewarm_lock = SRWLOCK_INIT;
static volatile LONG prewarm_total, prewarm_done, prewarm_ready;
static u64 prewarm_t0, prewarm_bytes0;

static u64 private_bytes(void) {
#ifdef _WIN32
    typedef struct { DWORD cb, faults; SIZE_T peak_ws, ws, a, b, c, d, pagefile, peak_pagefile, private_usage; } Counters;
    static BOOL (WINAPI *get)(HANDLE, Counters *, DWORD);
    if (!get) get = (BOOL (WINAPI *)(HANDLE, Counters *, DWORD))(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32GetProcessMemoryInfo");
    Counters c = {sizeof c};
    return get && get(GetCurrentProcess(), &c, sizeof c) ? c.private_usage : 0;
#else   /* (Linux: the process's anonymous memory in RAM) */
    FILE *f = fopen("/proc/self/status", "r");
    char line[128];
    u64 kb = 0;
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, "RssAnon:", 8)) { kb = strtoull(line + 8, NULL, 10); break; }
    if (f) fclose(f);
    return kb << 10;
#endif
}

/* (under prewarm_lock) the entry for a key; a new one when insert */
static Prewarm *prewarm_slot(int stage, u64 hash, u32 vs_outputs, u64 table_hash, int insert) {
    u64 k = (hash ^ table_hash * 31 ^ (u64)vs_outputs << 7 ^ (u64)stage) * 0x9E3779B97F4A7C15ull;
    for (u32 i = 0; i < PREWARM_SLOTS; i++) {
        Prewarm *p = &prewarm_table[((k >> 40) + i) & (PREWARM_SLOTS - 1)];
        if (p->state == PW_EMPTY) {
            if (!insert) return NULL;
            p->hash = hash; p->stage = stage; p->vs_outputs = vs_outputs; p->table_hash = table_hash;
            return p;
        }
        if (p->hash == hash && p->stage == stage && p->vs_outputs == vs_outputs && p->table_hash == table_hash) return p;
    }
    return NULL;
}

/* The game asks for a program: its object, if the prewarm made it; else NULL, and the prewarm
 * leaves it alone from now on (the game's own load takes over). */
static void *prewarm_take(const Shader *s) {
    if (!prewarm_table) return NULL;
    void *obj = NULL;
    AcquireSRWLockExclusive(&prewarm_lock);
    Prewarm *p = prewarm_slot(s->stage, s->hash, s->vs_outputs, s->table_hash, 0);
    if (p && p->state == PW_READY) { obj = p->obj; p->obj = NULL; }
    if (p && p->state != PW_FAILED) p->state = PW_TAKEN;
    ReleaseSRWLockExclusive(&prewarm_lock);
    return obj;
}

/* (on a compiler thread) one prewarm job: the file, then both devices' objects */
static void prewarm_job(struct Compiler *me, Shader *s) {
    Prewarm *p = s->prewarm;
    AcquireSRWLockExclusive(&prewarm_lock);
    int go = p->state == PW_QUEUED;
    if (go) p->state = PW_LOADING;
    ReleaseSRWLockExclusive(&prewarm_lock);
    void *obj = NULL;
    if (go) {
        char path[1200];
        cache_path(s, path, sizeof path);
        me->step = STEP_CACHE;
        FILE *f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            void *code = n > 0 ? malloc(n) : NULL;
            if (code && fread(code, 1, n, f) == (size_t)n && create_noted(me, s, code, n)) obj = s->obj;
            fclose(f);
            free(code);
        }
    }
    AcquireSRWLockExclusive(&prewarm_lock);
    if (go && p->state == PW_LOADING) {
        p->obj = obj;
        p->state = obj ? PW_READY : PW_FAILED;
        if (obj) prewarm_ready++;
        obj = NULL;
    }
    ReleaseSRWLockExclusive(&prewarm_lock);
    if (obj) IUnknown_Release((IUnknown *)obj);   /* the game loaded its own meanwhile */
    if (InterlockedIncrement(&prewarm_done) == prewarm_total)
        fprintf(stderr, "[gfx] prewarm: %ld of %ld shaders ready in %.1f s; private memory %.0f -> %.0f MB\n", (long)prewarm_ready, (long)prewarm_total,
                (GetTickCount64() - prewarm_t0) / 1000.0, prewarm_bytes0 / 1048576.0, private_bytes() / 1048576.0);
}

/* (on a compiler thread) the shader's GPU code: 1 ready, 2 failed */
static LONG make_ready(struct Compiler *me, Shader *s) {
    char path[1200];
    cache_path(s, path, sizeof path);
    me->step = STEP_CACHE;
    FILE *f = s->cached ? fopen(path, "rb") : NULL;
    if (f) {   /* compiled in an earlier run */
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        void *code = n > 0 ? malloc(n) : NULL;
        int ok = code && fread(code, 1, n, f) == (size_t)n && create_noted(me, s, code, n);
        fclose(f);
        free(code);
        if (ok) { cache_touch(path); return 1; }
    }
    me->step = STEP_FXC;
    ID3DBlob *code = NULL, *errors = NULL;
    HRESULT hr = shader_cache_compile(s->hlsl, s->name, s->stage, &code, &errors);
    LONG state = 2;
    if (SUCCEEDED(hr) && create_noted(me, s, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code))) {
        f = fopen(path, "wb");
        if (f) { fwrite(ID3D10Blob_GetBufferPointer(code), 1, ID3D10Blob_GetBufferSize(code), f); fclose(f); }
        state = 1;
    } else {
        fprintf(stderr, "[gfx] shader %s failed to compile:\n%.2000s\n", s->name,
                errors ? (char *)ID3D10Blob_GetBufferPointer(errors) : SUCCEEDED(hr) ? "(the driver refused its bytecode)" : "?");
    }
    if (code) ID3D10Blob_Release(code);
    if (errors) ID3D10Blob_Release(errors);
    return state;
}

static DWORD WINAPI compiler_main(void *arg) {
    struct Compiler *me = arg;
    me->thread = GetCurrentThreadId();
    SetThreadDescription(GetCurrentThread(), L"shader compiler");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    ULONG guarantee = 64 << 10;   /* room for the crash report, should a compile overflow even this stack */
    SetThreadStackGuarantee(&guarantee);
    int prewarms = me != &compilers[0];
    for (;;) {
        AcquireSRWLockExclusive(&jobs_lock);
        while (!job_queue[0].head && !job_queue[1].head && !(prewarms && job_queue[2].head))
            SleepConditionVariableSRW(&jobs_cv, &jobs_lock, INFINITE, 0);
        int q = job_queue[0].head ? 0 : job_queue[1].head ? 1 : 2;
        Shader *s = job_queue[q].head;
        if (!(job_queue[q].head = s->next_job)) job_queue[q].tail = NULL;
        ReleaseSRWLockExclusive(&jobs_lock);
        me->since_ms = GetTickCount64();
        me->job = s;
        if (s->prewarm) {
            prewarm_job(me, s);
            me->step = STEP_IDLE;
            me->job = NULL;
            free(s);
            continue;
        }
        LONG state = make_ready(me, s);
        me->step = STEP_IDLE;
        me->job = NULL;
        free(s->hlsl);
        s->hlsl = NULL;
        if (state == 1) InterlockedIncrement64((volatile LONG64 *)&gfx_stat_shaders);
        else InterlockedIncrement64((volatile LONG64 *)&gfx_stat_shader_failures);
        InterlockedExchange(&s->state, state);
        InterlockedDecrement(&compiles_pending);
    }
}

/* The compiler threads, started with the renderer (init). */
static void start_compilers(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const char *env = getenv("SSBU_COMPILE_THREADS");
    u32 n = env ? (u32)atoi(env) : si.dwNumberOfProcessors / 3;
    n = n < 2 ? 2 : n > COMPILERS_MAX ? COMPILERS_MAX : n;
    for (u32 i = 0; i < n; i++) {
        HANDLE t = CreateThread(NULL, COMPILER_STACK, compiler_main, &compilers[i], STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        if (!t) break;
        CloseHandle(t);
        ncompilers++;
    }
    if (!ncompilers) rt_fatal("cannot start the shader compiler threads (error %lu)", (unsigned long)GetLastError());
    fprintf(stderr, "[gfx] %u shader compiler threads\n", ncompilers);
}

typedef struct { char name[96]; u64 written; } Found;
static int newest_first(const void *a, const void *b) {
    u64 x = ((const Found *)a)->written, y = ((const Found *)b)->written;
    return x < y ? 1 : x > y ? -1 : 0;
}

/* (a thread of its own, at start) every file of the disk cache into the prewarm queue */
static DWORD WINAPI prewarm_main(void *unused) {
    (void)unused;
    SetThreadDescription(GetCurrentThread(), L"shader prewarm");
    Found *found = NULL;
    u32 n = 0, cap = 0;
    char pattern[1100];
    snprintf(pattern, sizeof pattern, "%s/v%u_*.dxbc", cache_dir(), SHADER_CACHE_VERSION);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    for (BOOL more = h != INVALID_HANDLE_VALUE; more; more = FindNextFileA(h, &fd)) {
        /* only what the game has used: not the ahead-of-time compiler's files it never asked for */
        if (((u64)fd.ftLastWriteTime.dwHighDateTime << 32 | fd.ftLastWriteTime.dwLowDateTime) < SHADER_CACHE_PLAYED) continue;
        if (n == cap) found = realloc(found, sizeof *found * (cap = cap * 2 + 1024));
        snprintf(found[n].name, sizeof found[n].name, "%s", fd.cFileName);
        found[n++].written = (u64)fd.ftLastWriteTime.dwHighDateTime << 32 | fd.ftLastWriteTime.dwLowDateTime;
    }
    if (h != INVALID_HANDLE_VALUE) FindClose(h);
    qsort(found, n, sizeof *found, newest_first);   /* the shaders of what was played last, first */
    prewarm_t0 = GetTickCount64();
    prewarm_bytes0 = private_bytes();
    Prewarm *table = calloc(PREWARM_SLOTS, sizeof *table);
    prewarm_table = table;
    Shader *head = NULL, *tail = NULL;
    u32 queued = 0;
    for (u32 i = 0; i < n && table; i++) {
        unsigned version, vs_outputs;
        char stage_name[4];
        unsigned long long hash, tables;
        if (sscanf(found[i].name, "v%u_%2[a-z]_%16llx_%8x_%16llx.dxbc", &version, stage_name, &hash, &vs_outputs, &tables) != 5) continue;
        int stage = !strcmp(stage_name, "vs") ? SHADER_VERTEX : !strcmp(stage_name, "ps") ? SHADER_PIXEL : !strcmp(stage_name, "cs") ? SHADER_COMPUTE : -1;
        if (stage < 0 || queued >= PREWARM_SLOTS / 2) continue;
        Shader *s = calloc(1, sizeof *s);
        s->hash = hash; s->stage = stage; s->vs_outputs = vs_outputs; s->table_hash = tables;
        snprintf(s->name, sizeof s->name, "%s_%016llx", stage_name, hash);
        AcquireSRWLockExclusive(&prewarm_lock);
        s->prewarm = prewarm_slot(stage, hash, vs_outputs, tables, 1);
        if (s->prewarm && s->prewarm->state == PW_EMPTY) s->prewarm->state = PW_QUEUED;
        else { free(s); s = NULL; }
        ReleaseSRWLockExclusive(&prewarm_lock);
        if (!s) continue;
        if (tail) tail->next_job = s; else head = s;
        tail = s;
        queued++;
    }
    free(found);
    if (!queued) return 0;
    prewarm_total = (LONG)queued;
    AcquireSRWLockExclusive(&jobs_lock);
    if (job_queue[2].tail) job_queue[2].tail->next_job = head; else job_queue[2].head = head;
    job_queue[2].tail = tail;
    ReleaseSRWLockExclusive(&jobs_lock);
    WakeAllConditionVariable(&jobs_cv);
    fprintf(stderr, "[gfx] prewarm: %u shaders from the disk cache, on %u threads\n", queued, ncompilers - 1);
    return 0;
}

/* Hand a shader to the compiler threads. A compute program's cache load goes to the front:
 * its draw waits for it. */
static void enqueue_shader(Shader *s) {
    InterlockedIncrement(&compiles_pending);
    int q = s->cached ? 0 : 1;
    s->next_job = NULL;
    AcquireSRWLockExclusive(&jobs_lock);
    if (q == 0 && s->stage == SHADER_COMPUTE) {
        if (!(s->next_job = job_queue[0].head)) job_queue[0].tail = s;
        job_queue[0].head = s;
    } else {
        if (job_queue[q].tail) job_queue[q].tail->next_job = s;
        else job_queue[q].head = s;
        job_queue[q].tail = s;
    }
    ReleaseSRWLockExclusive(&jobs_lock);
    WakeConditionVariable(&jobs_cv);
}

/* For a crash or freeze report: what each compiler thread is doing (to line, a line at a time),
 * its program kept next to the disk cache (shadercrash/) so it can be compiled again outside the
 * game. */
void gfx_report_compiles(void (*line)(const char *text)) {
    char text[600];
    static const char *const what[] = {"idle", "reading its DXBC from the disk cache", "fxc compiling its HLSL", "the driver compiling its DXBC"};
    DWORD self = GetCurrentThreadId();
    u64 now = GetTickCount64();
    int busy = 0;
    for (u32 i = 0; i < ncompilers; i++) {
        Shader *s = compilers[i].job;
        LONG step = compilers[i].step;
        if (!s || step == STEP_IDLE) continue;
        if (!busy++) line("shader compiler threads at the time:\n");
        char dir[1100], path[1200];
        snprintf(dir, sizeof dir, "%s/../shadercrash", rt_data_dir);
        CreateDirectoryA(dir, NULL);
        const char *saved = "";
        if (s->hlsl) {
            snprintf(path, sizeof path, "%s/%s_%08x_%016llx.hlsl", dir, s->name, s->vs_outputs, s->table_hash);
            FILE *f = fopen(path, "w");
            if (f) { fputs(s->hlsl, f); fclose(f); saved = "; HLSL saved"; }
        }
        const void *dxbc = compilers[i].dxbc;
        size_t size = compilers[i].dxbc_size;
        if (step == STEP_DRIVER && dxbc && size) {
            snprintf(path, sizeof path, "%s/%s_%08x_%016llx.dxbc", dir, s->name, s->vs_outputs, s->table_hash);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(dxbc, 1, size, f); fclose(f); saved = s->hlsl ? "; HLSL and DXBC saved" : "; DXBC saved"; }
        }
        snprintf(text, sizeof text, "  thread %lu%s: %s - %s, for %.1f s%s\n", (unsigned long)compilers[i].thread, compilers[i].thread == self ? " (this thread)" : "",
                 s->name, what[step], (now - compilers[i].since_ms) / 1000.0, saved);
        line(text);
    }
    if (busy) { snprintf(text, sizeof text, "  (programs in %s/../shadercrash)\n", rt_data_dir); line(text); }
}

typedef struct CbufSource { const CbBinding *cb; } CbufSource;   /* one stage's bindings */
static void read_gpu(u64 va, u32 size, void *dst);
static u32 read_cbuf_word(void *user, u32 bank, u32 offset) {
    const CbufSource *src = user;
    u32 v = 0;
    if (bank < 18 && src->cb[bank].address && offset + 4 <= src->cb[bank].size) read_gpu(src->cb[bank].address + offset, 4, &v);
    return v;
}

static u64 table_hash(const ShaderInfo *info, CbufSource *src) {
    u64 h = 0;
    for (u32 t = 0; t < info->ntables; t++)
        for (u32 e = 0; e < info->table[t].count; e++)
            h = (h ^ read_cbuf_word(src, info->table[t].bank, info->table[t].offset + 4 * e)) * 1099511628211ull;
    return h;
}

/* Programs by where they are: finding a shader by its contents means decoding the program for
 * its length and hashing all of it - per draw, per stage, far too slow - so each address
 * remembers the shader it last held, recognised again by a few of its words. */
typedef struct ShaderSlot { u64 g, key, print; u32 size; Shader *s; } ShaderSlot;
#define SHADER_SLOTS 16384
static ShaderSlot shader_slots[SHADER_SLOTS];

static u64 fingerprint(int stage, const u8 *p, u32 size) {   /* 32 words spread over the program (a compute one's code only) */
    if (stage == SHADER_COMPUTE) { p += 0x50; size -= 0x50; }
    u64 h = size;
    for (u32 i = 0; i < 32; i++) {
        u64 v;
        memcpy(&v, p + (((u64)size - 8) * i / 31 & ~7ull), 8);
        h = (h ^ v) * 1099511628211ull;
    }
    return h;
}

/* The shader for the program at guest address g, ready (state 1) or not. */
static Shader *find_shader_ex(u64 g, int stage, u32 vs_outputs, const CbBinding *stage_cb, const u32 *local, u32 shared) {
    /* a compute program has no header: it is translated as if it had one (never read) */
    const u8 *p = stage == SHADER_COMPUTE ? P(u8, g) - 0x50 : P(u8, g);
    u64 key = (u64)stage | (u64)vs_outputs << 4 | (stage == SHADER_COMPUTE ? ((u64)local[0] ^ (u64)local[1] << 11 ^ (u64)local[2] << 22 ^ (u64)shared << 33) << 36 : 0);
    ShaderSlot *slot = &shader_slots[((g >> 4) * 0x9E3779B97F4A7C15ull ^ key) >> 50 & (SHADER_SLOTS - 1)];
    if (slot->s && slot->g == g && slot->key == key && slot->print == fingerprint(stage, p, slot->size)) {
        Shader *s = slot->s;
        CbufSource src = {stage_cb};
        if (!s->info.ntables || s->table_hash == table_hash(&s->info, &src)) return s;
    }
    u32 size = shader_size(p, 0x40000), skip = stage == SHADER_COMPUTE ? 0x50 : 0;
    u64 h = shader_program_hash(p + skip, size - skip);
    if (stage == SHADER_COMPUTE) h = (h ^ ((u64)local[0] | (u64)local[1] << 16 | (u64)local[2] << 32 | (u64)shared << 40)) * 1099511628211ull;
    CbufSource src = {stage_cb};
    for (u32 i = 0; i < shader_count; i++) {
        Shader *s = shader_cache[i];
        if (s->hash != h || s->stage != stage || (stage == SHADER_PIXEL && s->vs_outputs != vs_outputs)) continue;
        if (s->info.ntables && s->table_hash != table_hash(&s->info, &src)) continue;
        *slot = (ShaderSlot){g, key, fingerprint(stage, p, size), size, s};
        return s;
    }
    if (shader_count == shader_cap) shader_cache = realloc(shader_cache, sizeof *shader_cache * (shader_cap = shader_cap * 2 + 64));
    Shader *s = calloc(1, sizeof *s);
    shader_cache[shader_count++] = s;
    s->hash = h; s->stage = stage; s->vs_outputs = vs_outputs;
    *slot = (ShaderSlot){g, key, fingerprint(stage, p, size), size, s};
    snprintf(s->name, sizeof s->name, "%s_%016llx", stage == SHADER_VERTEX ? "vs" : stage == SHADER_PIXEL ? "ps" : "cs", h);
    s->hlsl = stage == SHADER_COMPUTE ? shader_hlsl_cs(p, size, local, shared, &s->info, read_cbuf_word, &src)
                                      : shader_hlsl_ex(p, size, stage, vs_outputs, &s->info, read_cbuf_word, &src);
    const char *cdump = getenv("SSBU_DUMP_SHADERS");
    if (cdump && stage == SHADER_COMPUTE) {   /* C_<hash>_<local x>x<y>x<z>_<shared>.bin, behind 0x50 zero bytes */
        char path[1024];
        snprintf(path, sizeof path, "%s/C_%016llx_%ux%ux%u_%u.bin", cdump, h, local[0], local[1], local[2], shared);
        FILE *f = fopen(path, "wb");
        if (f) { static const u8 zero[0x50]; fwrite(zero, 1, 0x50, f); fwrite(p + 0x50, 1, size - 0x50, f); fclose(f); }
    }
    s->table_hash = table_hash(&s->info, &src);
    if (s->info.unsupported[0]) fprintf(stderr, "[gfx] %s: %s\n", s->name, s->info.unsupported);
    const char *find = getenv("SSBU_GFX_FIND");   /* debug: other copies of this program in guest memory */
    if (find && strstr(s->name, find) && size > 0x90) {
        fprintf(stderr, "[find] %s at %#llx, %#x bytes; around it:", s->name, g, size);
        for (u32 i = 0; i < 0x1000 / 4; i++) {
            u32 v = ((const u32 *)((g & ~0xFFFull)))[i];
            if (i % 8 == 0) fprintf(stderr, "\n[find]   %#llx:", (g & ~0xFFFull) + i * 4);
            fprintf(stderr, " %08x", v);
        }
        fprintf(stderr, "\n");
        MEMORY_BASIC_INFORMATION mbi;
        for (u64 a = ALIAS_BASE; a < HEAP_BASE + HEAP_LIMIT && VirtualQuery((void *)a, &mbi, sizeof mbi); a = (u64)mbi.BaseAddress + mbi.RegionSize) {
            if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) continue;
            const u8 *r = mbi.BaseAddress;
            for (u64 o = 0; o + 64 <= mbi.RegionSize; o += 16)
                if (!memcmp(r + o, p + 0x50, 64) && (u64)(r + o) != g + 0x50) {
                    fprintf(stderr, "[find]   copy at %#llx; bytes after its end:", (u64)(r + o) - 0x50);
                    const u32 *after = (const u32 *)(r + o - 0x50 + size);
                    for (u32 i = 0; i < 0x180 / 4 && o - 0x50 + size + i * 4 + 4 <= mbi.RegionSize; i++)
                        fprintf(stderr, "%s%08x", i % 8 ? " " : "\n[find]     ", after[i]);
                    fprintf(stderr, "\n");
                }
        }
    }
    static const char *dump;
    static int checked;
    if (!checked++) dump = getenv("SSBU_DUMP_HLSL");
    if (dump) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s.hlsl", dump, s->name);
        FILE *f = fopen(path, "w");
        if (f) { fputs(s->hlsl, f); fclose(f); }
    }
    void *made = prewarm_take(s);
    if (made) {   /* the prewarm made it: ready now */
        s->obj = made;
        free(s->hlsl);
        s->hlsl = NULL;
        InterlockedIncrement64((volatile LONG64 *)&gfx_stat_shaders);
        s->state = 1;
        return s;
    }
    char path[1200];
    cache_path(s, path, sizeof path);
    s->cached = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
    enqueue_shader(s);
    /* a compute program that was compiled before is waited for (a few ms): a skipped launch
     * leaves its outputs (skinned vertices) unwritten for a frame */
    if (stage == SHADER_COMPUTE && s->cached)
        for (u64 until = GetTickCount64() + 250; s->state == 0 && GetTickCount64() < until;) SwitchToThread();
    return s;
}

/* The ready shader for the program at guest address g, or NULL while it compiles. */
static Shader *get_shader_ex(u64 g, int stage, u32 vs_outputs, const CbBinding *stage_cb, const u32 *local, u32 shared) {
    Shader *s = find_shader_ex(g, stage, vs_outputs, stage_cb, local, shared);
    return s && s->state == 1 ? s : NULL;
}

/* ---- buffers: guest memory uploaded per draw ----------------------------------------- */
typedef struct Upload { ID3D11Buffer *buf; ID3D11ShaderResourceView *srv; u32 cap; } Upload;
static Upload uploads[3][128];   /* per stage (vertex, pixel, compute), per SRV slot */

/* Uploads up to 64 KB go into chunks of rings, one ring per chunk size, each chunk with a view
 * of its own made once: written without overwriting what the GPU may still be reading, so no
 * buffer is renamed per upload (thousands a frame); a ring is discarded (renamed) once it has
 * gone round - and only at the start of a draw, with room left for all its uploads, so no
 * draw's data goes with it. Needs the driver to allow that for buffers read through views. */
#define RING_CLASSES 5
static const u32 ring_chunk[RING_CLASSES] = {256, 1024, 4096, 16384, 65536};
static const u32 ring_count[RING_CLASSES] = {8192, 2048, 1024, 256, 64};
#define RING_ROOM 40   /* chunks of each size one draw may take, at most */
static struct { ID3D11Buffer *buf; ID3D11ShaderResourceView **srv; u32 next; } rings[RING_CLASSES];
static int rings_ok = -1;

static int rings_init(void) {
    D3D11_FEATURE_DATA_D3D11_OPTIONS o;
    memset(&o, 0, sizeof o);
    if (getenv("SSBU_NO_UPLOAD_RINGS") || FAILED(ID3D11Device_CheckFeatureSupport(dev, D3D11_FEATURE_D3D11_OPTIONS, &o, sizeof o)) ||
        !o.MapNoOverwriteOnDynamicBufferSRV)
        return 0;
    for (u32 c = 0; c < RING_CLASSES; c++) {
        D3D11_BUFFER_DESC bd = {ring_chunk[c] * ring_count[c], D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE, D3D11_CPU_ACCESS_WRITE,
                                D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
        if (FAILED(ID3D11Device_CreateBuffer(dev, &bd, NULL, &rings[c].buf))) return 0;
        rings[c].srv = calloc(ring_count[c], sizeof *rings[c].srv);
        for (u32 i = 0; i < ring_count[c]; i++) {
            D3D11_SHADER_RESOURCE_VIEW_DESC sd = {DXGI_FORMAT_R32_TYPELESS, D3D11_SRV_DIMENSION_BUFFEREX};
            sd.BufferEx.FirstElement = i * ring_chunk[c] / 4;
            sd.BufferEx.NumElements = ring_chunk[c] / 4;
            sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
            if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)rings[c].buf, &sd, &rings[c].srv[i]))) return 0;
        }
        rings[c].next = ring_count[c];   /* (the first upload discards) */
    }
    fprintf(stderr, "[gfx] upload rings ready\n");
    return 1;
}

/* At the start of a draw or dispatch: a ring without room for all of its uploads starts again. */
static void rings_start_draw(void) {
    if (rings_ok < 0) rings_ok = rings_init();
    if (!rings_ok) return;
    for (u32 c = 0; c < RING_CLASSES; c++)
        if (rings[c].next + RING_ROOM > ring_count[c]) rings[c].next = ring_count[c];
}

/* A chunk for `size` bytes, mapped for writing at *dst (unmap with ring_done(*cls)). */
static ID3D11ShaderResourceView *ring_alloc(u32 size, u8 **dst, u32 *cls) {
    u32 c = 0;
    while (c < RING_CLASSES && ring_chunk[c] < size) c++;
    if (c == RING_CLASSES || rings_ok != 1) return NULL;
    D3D11_MAP how = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (rings[c].next >= ring_count[c]) { rings[c].next = 0; how = D3D11_MAP_WRITE_DISCARD; }
    u32 i = rings[c].next++;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)rings[c].buf, 0, how, 0, &m))) return NULL;
    *dst = (u8 *)m.pData + (size_t)i * ring_chunk[c];
    *cls = c;
    return rings[c].srv[i];
}

static void ring_done(u32 c) { ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)rings[c].buf, 0); }

static ID3D11ShaderResourceView *ring_upload(const void *data, u32 size) {
    u8 *dst;
    u32 c;
    ID3D11ShaderResourceView *v = ring_alloc(size, &dst, &c);
    if (!v) return NULL;
    if (data) memcpy(dst, data, size);
    else memset(dst, 0, size);
    ring_done(c);
    return v;
}

static void read_gpu(u64 va, u32 size, void *dst);

/* Guest GPU memory [va, va + size) uploaded for a stage's slot: straight from guest memory into
 * a ring chunk when it fits one (else through a copy). */
static ID3D11ShaderResourceView *upload_raw(int stage, u32 slot, const void *data, u32 size);
static ID3D11ShaderResourceView *upload_gpu(int stage, u32 slot, u64 va, u32 size) {
    u32 padded = (size + 15) & ~15u;
    if (!padded) padded = 16;
    u8 *dst;
    u32 c;
    ID3D11ShaderResourceView *v = ring_alloc(padded, &dst, &c);
    if (v) {
        read_gpu(va, size, dst);
        if (padded > size) memset(dst + size, 0, padded - size);
        ring_done(c);
        gfx_stat_upload_bytes += padded;
        gfx_stat_upload_kind[slot < 18 ? 0 : slot < 96 ? 1 : 2] += padded;
        return v;
    }
    static u8 *tmp;
    static u32 tmp_cap;
    if (padded > tmp_cap) { tmp = realloc(tmp, padded); tmp_cap = padded; }
    read_gpu(va, size, tmp);
    if (padded > size) memset(tmp + size, 0, padded - size);
    return upload_raw(stage, slot, tmp, size);
}

static ID3D11ShaderResourceView *upload_raw(int stage, u32 slot, const void *data, u32 size) {
    Upload *u = &uploads[stage][slot];
    size = (size + 15) & ~15u;
    if (!size) size = 16;
    gfx_stat_upload_bytes += size;
    gfx_stat_upload_kind[slot < 18 ? 0 : slot < 96 ? 1 : 2] += size;   /* constant buffers, vertex streams, storage buffers */
    ID3D11ShaderResourceView *r = ring_upload(data, size);
    if (r) return r;
    if (size > u->cap) {
        if (u->srv) ID3D11ShaderResourceView_Release(u->srv);
        if (u->buf) ID3D11Buffer_Release(u->buf);
        u32 cap = 1024;
        while (cap < size) cap *= 2;
        D3D11_BUFFER_DESC bd = {cap, D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE, D3D11_CPU_ACCESS_WRITE,
                                D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
        if (FAILED(ID3D11Device_CreateBuffer(dev, &bd, NULL, &u->buf))) { u->cap = 0; u->buf = NULL; return NULL; }
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {DXGI_FORMAT_R32_TYPELESS, D3D11_SRV_DIMENSION_BUFFEREX};
        sd.BufferEx.FirstElement = 0;
        sd.BufferEx.NumElements = cap / 4;
        sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)u->buf, &sd, &u->srv);
        u->cap = cap;
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)u->buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return NULL;
    if (data) memcpy(m.pData, data, size);
    else memset(m.pData, 0, size);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)u->buf, 0);
    return u->srv;
}

static ID3D11Buffer *params_buffer, *index_buffer;
static u32 index_cap, index_pos, index_first;   /* the index ring: its size, where the next indices go, where this draw's start (in indices) */

/* Copy guest GPU memory [va, va + size) into dst (zeros where unmapped). */
static void read_gpu(u64 va, u32 size, void *dst) {
    u64 g = gpu_va_to_guest(va);
    if (!size) return;
    if (g && gpu_va_to_guest(va + size - 1) == g + size - 1) { memcpy(dst, P(void, g), size); return; }
    /* not one contiguous mapping: page by page, zeros where unmapped */
    for (u32 done = 0; done < size;) {
        u32 n = 0x1000 - (u32)((va + done) & 0xFFF);
        if (n > size - done) n = size - done;
        u64 p = gpu_va_to_guest(va + done);
        if (p) memcpy((u8 *)dst + done, P(void, p), n);
        else memset((u8 *)dst + done, 0, n);
        done += n;
    }
}

#define DIV_UP_(a, b) (((a) + (b) - 1) / (b))

static int draw_logging;   /* (defined below) */
int gfx_draw_logging(void) { return draw_logging; }

/* ---- GPU-resident buffers ---------------------------------------------------------- */
/* What compute programs write stays on the GPU: a buffer per guest range they write (and per
 * copy-engine destination of such data), used directly by later dispatches and by draws whose
 * vertex streams lie inside it; reads that only partly overlap one get the guest's data around
 * it. Guest memory under a region is stale; pages the CPU writes inside one (seen through the
 * write watch) are taken from guest memory. */
typedef struct ResidentView { u64 first; u32 bytes; ID3D11ShaderResourceView *srv; ID3D11UnorderedAccessView *uav; } ResidentView;
typedef struct Resident {
    u64 va, guest;
    u32 size;
    ID3D11Buffer *buf;
    ID3D11UnorderedAccessView *uav;
    ResidentView views[64];
    u32 nviews;
    /* Guest memory under the region's first and last partial pages (head and tail bytes), as
     * it was when the GPU copy last took it: the write watch says a page was written, these
     * say whether the bytes inside the region were (rather than someone's data next to it). */
    u8 *edge;
    u32 head, tail;
    u64 checked;   /* the submission it was last checked against the write watch in */
} Resident;
static Resident residents[512];
static u32 nresidents;
u64 gfx_stat_residents, gfx_stat_resident_pages;
extern u64 gpu_stat_batches;
static int watched(u64 g, u64 size);

/* Views and buffers given up during a frame may still be in this frame's bindings (or the
 * arrays they are being collected in): they are released after the present. */
static IUnknown **graveyard;
static u32 ngraves, graves_cap;
static void bury(void *object) {
    if (!object) return;
    if (ngraves == graves_cap) graveyard = realloc(graveyard, sizeof *graveyard * (graves_cap = graves_cap * 2 + 256));
    graveyard[ngraves++] = object;
}
static void release_graves(void) {
    for (u32 i = 0; i < ngraves; i++) graveyard[i]->lpVtbl->Release(graveyard[i]);
    ngraves = 0;
}

static void resident_drop(Resident *r) {
    for (u32 i = 0; i < r->nviews; i++) { bury(r->views[i].srv); bury(r->views[i].uav); }
    bury(r->uav);
    bury(r->buf);
    free(r->edge);
    *r = residents[--nresidents];
    memset(&residents[nresidents], 0, sizeof *residents);
}

static void resident_snapshot(Resident *r) {
    u32 head = (u32)(0x1000 - (r->guest & 0xFFF)) & 0xFFF;   /* up to the first page boundary */
    if (head > r->size) head = r->size;
    u32 tail = (u32)((r->guest + r->size) & 0xFFF);          /* from the last one */
    if (tail > r->size - head) tail = r->size - head;
    r->head = head;
    r->tail = tail;
    r->edge = realloc(r->edge, head + tail + 1);
    memcpy(r->edge, P(void, r->guest), head);
    memcpy(r->edge + head, P(void, r->guest + r->size - tail), tail);
}

/* Has the CPU written guest memory inside the region since its GPU copy last took it? Pages
 * wholly inside that it wrote are taken from guest memory; a page the region only partly
 * covers is taken only where the bytes inside the region changed - the CPU may have written
 * just its own data next to the region, or (memory reused) right over a region no program
 * writes any more. Checked when a draw or dispatch uses the region (once a submission) and
 * once a frame for all. */
static void resident_sync(Resident *r, int every_frame) {
    static void *pages[16384];
    static int no_pages = -1;   /* debug: leave resident regions alone */
    if (no_pages < 0) no_pages = getenv("SSBU_NO_RESIDENT_PAGES") != NULL;
    if (no_pages || (!every_frame && r->checked == gpu_stat_batches) || !r->edge || !watched(r->guest, r->size)) return;
    r->checked = gpu_stat_batches;
    ULONG_PTR count = 16384;
    ULONG granularity;
    if (GetWriteWatch(0, (void *)r->guest, r->size, pages, &count, &granularity) != 0 || !count) return;
    u64 end = r->guest + r->size;
    for (ULONG_PTR k = 0; k < count; k++) {
        u64 pg = (u64)pages[k], lo = pg > r->guest ? pg : r->guest, hi = pg + granularity < end ? pg + granularity : end;
        if (lo >= hi) continue;
        if (lo != pg || hi != pg + granularity) {   /* partly covered: compare the bytes inside */
            u8 *was = lo - r->guest < r->head ? r->edge + (lo - r->guest) : r->edge + r->head + (lo - (end - r->tail));
            if (!memcmp(was, P(void, lo), (size_t)(hi - lo))) continue;
            memcpy(was, P(void, lo), (size_t)(hi - lo));
        }
        D3D11_BOX box = {(UINT)(lo - r->guest), 0, 0, (UINT)(hi - r->guest), 1, 1};
        ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)r->buf, 0, &box, P(void, lo), 0, 0);
        gfx_stat_resident_pages++;
    }
    ResetWriteWatch((void *)r->guest, r->size);
}

static Resident *resident_covering(u64 lo, u64 hi) {
    for (u32 i = 0; i < nresidents; i++)
        if (residents[i].va <= lo && hi <= residents[i].va + residents[i].size) { resident_sync(&residents[i], 0); return &residents[i]; }
    return NULL;
}

/* A raw view of [va, va + bytes) inside r: for shaders to read (srv) or compute to write. */
static ResidentView *resident_viewx(Resident *r, u64 va, u32 bytes) {
    u64 first = (va - r->va) & ~3ull;
    bytes = (u32)((va + bytes - r->va - first + 3) & ~3ull);
    if (first + bytes > ((r->size + 15) & ~15u)) bytes = (u32)(((r->size + 15) & ~15u) - first);
    for (u32 i = 0; i < r->nviews; i++)
        if (r->views[i].first == first && r->views[i].bytes == bytes) return &r->views[i];
    if (r->nviews == 64) {
        ResidentView *o = &r->views[0];
        bury(o->srv);
        bury(o->uav);
        *o = r->views[--r->nviews];
    }
    ResidentView *v = &r->views[r->nviews];
    memset(v, 0, sizeof *v);
    v->first = first;
    v->bytes = bytes;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd;
    memset(&sd, 0, sizeof sd);
    sd.Format = DXGI_FORMAT_R32_TYPELESS;
    sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    sd.BufferEx.FirstElement = (UINT)(first / 4);
    sd.BufferEx.NumElements = bytes / 4;
    sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud;
    memset(&ud, 0, sizeof ud);
    ud.Format = DXGI_FORMAT_R32_TYPELESS;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.FirstElement = (UINT)(first / 4);
    ud.Buffer.NumElements = bytes / 4;
    ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)r->buf, &sd, &v->srv)) ||
        FAILED(ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)r->buf, &ud, &v->uav))) {
        if (v->srv) ID3D11ShaderResourceView_Release(v->srv);
        return NULL;
    }
    r->nviews++;
    return v;
}

static ID3D11ShaderResourceView *resident_view(Resident *r, u64 va, u32 bytes) {
    ResidentView *v = resident_viewx(r, va, bytes);
    return v ? v->srv : NULL;
}

static Resident *resident_create(u64 va, u32 size) {
    if (nresidents == 512) resident_drop(&residents[0]);
    Resident *r = &residents[nresidents];
    memset(r, 0, sizeof *r);
    u32 cap = (size + 15) & ~15u;
    D3D11_BUFFER_DESC bd = {cap, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, 0,
                            D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
    u8 *init = calloc(1, cap);
    read_gpu(va, size, init);   /* what the CPU put there */
    D3D11_SUBRESOURCE_DATA sd = {init, 0, 0};
    HRESULT hr = ID3D11Device_CreateBuffer(dev, &bd, &sd, &r->buf);
    free(init);
    if (FAILED(hr)) return NULL;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud;
    memset(&ud, 0, sizeof ud);
    ud.Format = DXGI_FORMAT_R32_TYPELESS;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = cap / 4;
    ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    if (FAILED(ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)r->buf, &ud, &r->uav))) { ID3D11Buffer_Release(r->buf); return NULL; }
    r->va = va;
    r->size = size;
    r->guest = gpu_va_to_guest(va);
    if (watched(r->guest, r->size)) resident_snapshot(r);
    nresidents++;
    gfx_stat_residents++;
    return r;
}

/* The resident region holding [va, va + size), made (or widened) as needed. Regions never
 * overlap: a range that overlaps some becomes one region over all of them, with the CPU's
 * data underneath and each old region's GPU data copied over it. */
static Resident *resident_acquire(u64 va, u32 size) {
    u64 lo = va, hi = va + size;
    int overlaps = 0;
    for (u32 i = 0; i < nresidents; i++) {
        Resident *r = &residents[i];
        if (r->va <= va && va + size <= r->va + r->size) return r;
        if (r->va < hi && va < r->va + r->size) {
            overlaps = 1;
            if (r->va < lo) lo = r->va;
            if (r->va + r->size > hi) hi = r->va + r->size;
        }
    }
    if (hi - lo > (256u << 20)) return NULL;
    Resident *n = resident_create(lo, (u32)(hi - lo));
    if (!n) return NULL;
    if (overlaps) {
        Resident merged = *n;   /* resident_drop moves entries around */
        n->buf = NULL; n->uav = NULL; n->edge = NULL;
        resident_drop(n);
        for (u32 i = 0; i < nresidents;) {
            Resident *r = &residents[i];
            if (r->va >= merged.va && r->va + r->size <= merged.va + merged.size && r->buf != merged.buf) {
                D3D11_BOX box = {0, 0, 0, (r->size + 3) & ~3u, 1, 1};
                ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)merged.buf, 0, (UINT)(r->va - merged.va), 0, 0,
                                                          (ID3D11Resource *)r->buf, 0, &box);
                resident_drop(r);
            } else i++;
        }
        residents[nresidents++] = merged;
        n = &residents[nresidents - 1];
    }
    if (n->guest >= ALIAS_BASE && n->guest + n->size <= ALIAS_BASE + ALIAS_SIZE) ResetWriteWatch((void *)n->guest, n->size);
    return n;
}

static Resident *resident_overlapping(u64 va, u32 size) {
    for (u32 i = 0; i < nresidents; i++)
        if (residents[i].va < va + size && va < residents[i].va + residents[i].size) { resident_sync(&residents[i], 0); return &residents[i]; }
    return NULL;
}

/* dst holds guest memory's [va, va + size): copy over it what resident regions hold of that. */
static void resident_patch(ID3D11Buffer *dst, u64 va, u64 size) {
    for (u32 i = 0; i < nresidents; i++) {
        Resident *r = &residents[i];
        u64 lo = va > r->va ? va : r->va, hi = va + size < r->va + r->size ? va + size : r->va + r->size;
        if (lo >= hi) continue;
        resident_sync(r, 0);
        D3D11_BOX box = {(UINT)(lo - r->va), 0, 0, (UINT)(hi - r->va), 1, 1};
        ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)dst, 0, (UINT)(lo - va), 0, 0, (ID3D11Resource *)r->buf, 0, &box);
    }
}

/* A range only partly held by resident regions, as the GPU sees it: the guest's data (in data)
 * with the regions' parts copied over it, in a GPU buffer per slot. Regions are never widened
 * for a read: what lies around them is the CPU's, and it keeps changing in guest memory. */
typedef struct Composed { ID3D11Buffer *buf; ID3D11ShaderResourceView *srv; u32 cap; } Composed;
static Composed composed[3][128];

static ID3D11ShaderResourceView *upload_composed(int stage, u32 slot, u64 va, const void *data, u32 size) {
    Composed *c = &composed[stage][slot];
    u32 need = (size + 15) & ~15u;
    gfx_stat_upload_bytes += size;
    gfx_stat_composed++;
    if (!need) return upload_raw(stage, slot, data, size);
    if (need > c->cap) {
        bury(c->srv);   /* may be bound still */
        bury(c->buf);
        c->srv = NULL; c->buf = NULL; c->cap = 0;
        u32 cap = 4096;
        while (cap < need) cap *= 2;
        D3D11_BUFFER_DESC bd = {cap, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
        if (FAILED(ID3D11Device_CreateBuffer(dev, &bd, NULL, &c->buf))) { c->buf = NULL; return NULL; }
        D3D11_SHADER_RESOURCE_VIEW_DESC sd;
        memset(&sd, 0, sizeof sd);
        sd.Format = DXGI_FORMAT_R32_TYPELESS;
        sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        sd.BufferEx.NumElements = cap / 4;
        sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)c->buf, &sd, &c->srv))) {
            ID3D11Buffer_Release(c->buf);
            c->buf = NULL; c->srv = NULL;
            return NULL;
        }
        c->cap = cap;
    }
    D3D11_BOX box = {0, 0, 0, size, 1, 1};
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)c->buf, 0, &box, data, 0, 0);
    resident_patch(c->buf, va, size);
    return c->srv;
}

/* The copy engine and resident data. */
int gfx_copy_resident(u64 dst_va, u64 src_va, u64 bytes) {
    if (!bytes || bytes > (256u << 20) || !enter()) return 0;
    Resident *src = resident_overlapping(src_va, (u32)bytes);
    if (!src || src_va < src->va || src_va + bytes > src->va + src->size) {
        leave();
        if (src) gfx_resident_flush(src_va, bytes);   /* partly resident: through guest memory */
        return 0;
    }
    Resident *dst = resident_acquire(dst_va, (u32)bytes);   /* may merge regions: look the source up again */
    src = resident_covering(src_va, src_va + bytes);
    if (!dst || !src) { leave(); return 0; }
    D3D11_BOX box = {(UINT)(src_va - src->va), 0, 0, (UINT)(src_va - src->va + bytes), 1, 1};
    if (dst != src)
        ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)dst->buf, 0, (UINT)(dst_va - dst->va), 0, 0,
                                                  (ID3D11Resource *)src->buf, 0, &box);
    else {   /* within one buffer: through a scratch one */
        static ID3D11Buffer *tmp;
        static u32 tmp_cap;
        if (bytes > tmp_cap) {
            if (tmp) ID3D11Buffer_Release(tmp);
            tmp_cap = (u32)((bytes + 0xFFFF) & ~0xFFFFull);
            D3D11_BUFFER_DESC bd = {tmp_cap, D3D11_USAGE_DEFAULT, 0, 0, 0, 0};
            if (FAILED(ID3D11Device_CreateBuffer(dev, &bd, NULL, &tmp))) { tmp = NULL; tmp_cap = 0; leave(); return 0; }
        }
        ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)tmp, 0, 0, 0, 0, (ID3D11Resource *)src->buf, 0, &box);
        D3D11_BOX all = {0, 0, 0, (UINT)bytes, 1, 1};
        ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)dst->buf, 0, (UINT)(dst_va - dst->va), 0, 0,
                                                  (ID3D11Resource *)tmp, 0, &all);
    }
    leave();
    return 1;
}

/* Bring the GPU's data for [va, va + bytes) back to guest memory (waits for the GPU). */
void gfx_resident_flush(u64 va, u64 bytes) {
    if (!enter()) return;
    for (u32 i = 0; i < nresidents; i++) {
        Resident *r = &residents[i];
        u64 lo = va > r->va ? va : r->va, hi = va + bytes < r->va + r->size ? va + bytes : r->va + r->size;
        if (lo >= hi) continue;
        resident_sync(r, 1);   /* (what the CPU wrote before, first: the write watch is reset below) */
        lo &= ~3ull;
        hi = (hi + 3) & ~3ull;
        D3D11_BUFFER_DESC sd = {(UINT)(hi - lo), D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
        ID3D11Buffer *st;
        if (FAILED(ID3D11Device_CreateBuffer(dev, &sd, NULL, &st))) continue;
        D3D11_BOX box = {(UINT)(lo - r->va), 0, 0, (UINT)(hi - r->va), 1, 1};
        ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)st, 0, 0, 0, 0, (ID3D11Resource *)r->buf, 0, &box);
        D3D11_MAPPED_SUBRESOURCE m;
        gfx_stat_gpu_waits++;
        if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
            for (u64 a = lo; a < hi; a += 4) {
                u64 g = gpu_va_to_guest(a);
                if (g) st32(g, ((const u32 *)m.pData)[(a - lo) / 4]);
            }
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
        }
        ID3D11Buffer_Release(st);
        if (r->guest >= ALIAS_BASE && r->guest + r->size <= ALIAS_BASE + ALIAS_SIZE) { ResetWriteWatch((void *)r->guest, r->size); resident_snapshot(r); }
    }
    leave();
}

static void kept_apply(u64 lo, u64 hi);
static void kept_flush(void);

/* Guest memory in [va, va + bytes) changed under a resident region: update the GPU's copy. */
void gfx_resident_written(u64 va, u64 bytes) {
    if (!bytes || bytes > (256u << 20) || !enter()) return;
    kept_flush();
    kept_apply(va, va + bytes);   /* (constant buffers kept on the GPU too) */
    for (u32 i = 0; i < nresidents; i++) {
        Resident *r = &residents[i];
        u64 lo = va > r->va ? va : r->va, hi = va + bytes < r->va + r->size ? va + bytes : r->va + r->size;
        if (lo >= hi) continue;
        resident_sync(r, 1);   /* (what the CPU wrote before, first: the write watch is reset below) */
        lo &= ~3ull;
        hi = (hi + 3) & ~3ull;
        if (hi > r->va + ((r->size + 15) & ~15u)) hi = r->va + ((r->size + 15) & ~15u);
        u8 *data = malloc((size_t)(hi - lo));
        read_gpu(lo, (u32)(hi - lo), data);
        D3D11_BOX box = {(UINT)(lo - r->va), 0, 0, (UINT)(hi - r->va), 1, 1};
        ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)r->buf, 0, &box, data, 0, 0);
        free(data);
        if (r->guest >= ALIAS_BASE && r->guest + r->size <= ALIAS_BASE + ALIAS_SIZE) { ResetWriteWatch((void *)r->guest, r->size); resident_snapshot(r); }
    }
    leave();
}

/* ---- textures and samplers ------------------------------------------------------------ */
typedef struct Texture {
    u64 guest, size;
    u32 tic[8];
    u64 sample_hash;
    int watched, stale;    /* in the write-watched alias area; written since it was read */
    ID3D11ShaderResourceView *srv;
    ID3D11Texture2D *tex;
    u64 used;
} Texture;
static Texture textures[4096];
static u32 ntextures;
u64 gfx_stat_textures, gfx_stat_watch_us;

/* Which textures has the CPU (or a copy) written since they were read? The alias area is
 * reserved with MEM_WRITE_WATCH, so Windows tracks the pages written. Once a frame, each
 * texture's range is asked about; only after every texture has been asked are the ranges
 * found written reset, so textures sharing memory all see the write. */
static int watched(u64 g, u64 size) { return g >= ALIAS_BASE && g + size <= ALIAS_BASE + ALIAS_SIZE; }

static void note_writes(void) {
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    static u8 written[4096];
    for (u32 i = 0; i < ntextures; i++) {
        Texture *t = &textures[i];
        written[i] = 0;
        if (!t->srv || !t->watched || t->stale) continue;
        void *page;
        ULONG_PTR count = 1;
        ULONG granularity;
        if (GetWriteWatch(0, (void *)t->guest, t->size, &page, &count, &granularity) == 0 && count) written[i] = 1;
    }
    for (u32 i = 0; i < ntextures; i++)
        if (written[i]) { textures[i].stale = 1; ResetWriteWatch((void *)textures[i].guest, textures[i].size); }
    /* GPU-resident buffers the CPU has written to (those no draw has checked yet) */
    for (u32 i = 0; i < nresidents; i++) resident_sync(&residents[i], 1);
    QueryPerformanceCounter(&t1);
    gfx_stat_watch_us += (u64)((t1.QuadPart - t0.QuadPart) * 1000000 / freq.QuadPart);
}

/* A sparse checksum of guest memory, to notice when a texture's pixels change. */
static u64 sample_hash(u64 g, u64 size) {
    u64 h = 1469598103934665603ull;
    const u8 *p = P(u8, g);
    u64 step = size / 64 ? size / 64 : 1;
    for (u64 i = 0; i < size; i += step) { u64 v; memcpy(&v, p + (i & ~7ull), 8); h = (h ^ v) * 1099511628211ull; }
    return h;
}

static void warn_texture(const TextureDesc *d) {
    static u32 seen[64], n;
    u32 key = d->format | d->type << 8 | d->header_version << 12;
    for (u32 i = 0; i < n; i++) if (seen[i] == key) return;
    if (n < 64) seen[n++] = key;
    fprintf(stderr, "[gfx] unsupported texture: format %#x, type %u, layout %u (%ux%u)\n", d->format, d->type, d->header_version,
            d->width, d->height);
}

/* The texture cache, indexed by guest address and texture header (draws look up thousands a
 * frame among thousands kept). Entries are checked against the texture they name, so a slot
 * given to another texture just stops matching; the index is rebuilt now and then. */
#define TEXTURE_INDEX 16384
static u16 texture_index[TEXTURE_INDEX];   /* textures[] index + 1 */
static u32 texture_index_adds;

static u32 texture_key(u64 g, const u32 *tic) {
    u64 h = g * 0x9E3779B97F4A7C15ull;
    for (u32 i = 0; i < 8; i++) h = (h ^ tic[i]) * 0x100000001B3ull;
    return (u32)(h >> 40) & (TEXTURE_INDEX - 1);
}

static Texture *texture_find(u64 g, const u32 *tic) {
    for (u32 k = texture_key(g, tic), n = 0; texture_index[k] && n < TEXTURE_INDEX; k = (k + 1) & (TEXTURE_INDEX - 1), n++) {
        Texture *t = &textures[texture_index[k] - 1];
        if (t->srv && t->guest == g && !memcmp(t->tic, tic, 32)) return t;
    }
    return NULL;
}

static void texture_index_put(Texture *t) {
    u32 k = texture_key(t->guest, t->tic);
    while (texture_index[k]) k = (k + 1) & (TEXTURE_INDEX - 1);
    texture_index[k] = (u16)(t - textures + 1);
}

static void texture_index_add(Texture *t) {
    if (++texture_index_adds >= TEXTURE_INDEX / 2) {   /* entries for textures since replaced pile up: start again */
        memset(texture_index, 0, sizeof texture_index);
        texture_index_adds = 0;
        for (u32 i = 0; i < ntextures; i++)
            if (textures[i].srv && &textures[i] != t) { texture_index_put(&textures[i]); texture_index_adds++; }
    }
    texture_index_put(t);
}

static int depth_sampling = -1;   /* depth buffers as textures (shadow maps, ...); SSBU_NO_DEPTH_SAMPLING or F5: off */

/* A 3D texture whose slices the GPU renders as separate 2D layers (a colour-grading LUT built by
 * 16 small draws, one per depth slice at va + k*0x200): assemble a real Texture3D from the
 * rendered slices so the shader's Texture3D sees all three axes. The slices live on the GPU, not
 * in guest memory, so a guest read would give black; instead each 2D layer surface is read back
 * and copied into a staging 3D texture of the same format, then into the assembled texture. The
 * texture is rebuilt when any slice's surface is re-rendered (its tick moves). D3D11 has no
 * Texture2D->Texture3D CopySubresourceRegion (resources must share type and size), so the copy
 * goes through a staging Texture3D - the one cross-type-free route. */
typedef struct Lut3D {
    u64 guest, sig;                 /* guest base, and a build signature (sum of slice ticks) */
    u32 w, h, d; DXGI_FORMAT fmt;
    ID3D11Texture3D *tex; ID3D11ShaderResourceView *srv;
} Lut3D;
static Lut3D luts[32];

static void release_lut(Lut3D *l) {
    if (l->srv) ID3D11ShaderResourceView_Release(l->srv);
    if (l->tex) ID3D11Texture3D_Release(l->tex);
    memset(l, 0, sizeof *l);
}

static ID3D11ShaderResourceView *assemble_sliced_3d(const TextureDesc *d, u64 g) {
    static int off = -1;
    if (off < 0) off = getenv("SSBU_NO_3D_ASSEMBLE") != NULL;
    if (off) return NULL;   /* debug: the old behaviour, for an A/B capture */
    if (d->format == 0x29 || d->format == 0x2A || d->format == 0x2B) return NULL;    /* depth: not this */
    /* Only the case proven by evidence: an 8-bit RGBA volume whose depth slices the GPU renders
     * as separate 16x16 RGBA8 (fmt 0xD5) surfaces. The 3D TIC decodes to RGBA8 (fmt 0x07/0x08,
     * 4 bytes/texel) while the produced slices are fmt 0xD5 = R8G8B8A8_UNORM, the same DXGI bytes,
     * so a straight slice copy preserves channels. Other 3D textures (single-channel R8 LUTs, ...)
     * keep the guest-read path below - their contents really are in guest memory. */
    if ((d->format != 0x07 && d->format != 0x08) || d->bytes != 4) return NULL;
    if (d->bw != 1 || d->bh != 1) return NULL;
    if (d->depth < 2 || d->depth > 128) return NULL;
    u32 zstride = 1u << (9 + d->block_height);      /* guest stride between depth slices, from the header */
    Lut3D *l = NULL;
    for (u32 i = 0; i < 32; i++) if (luts[i].tex && luts[i].guest == g && luts[i].w == d->width && luts[i].h == d->height && luts[i].d == d->depth) { l = &luts[i]; break; }
    /* every slice must be a live 2D colour surface of this size, at the derived stride */
    Surface *slice[128];
    u64 sig = 1469598103934665603ull;
    for (u32 k = 0; k < d->depth; k++) {
        Surface *s = NULL;
        for (u32 i = 0; i < nsurfaces; i++) {
            Surface *c = &surfaces[i];
            if (!c->tex || c->depth || !c->rtv || c->guest != g + (u64)k * zstride || c->width != d->width || c->height != d->height) continue;
            if (!s || c->used > s->used) s = c;
        }
        if (!s) return NULL;
        slice[k] = s;
        sig = (sig ^ s->used) * 1099511628211ull;
    }
    if (l && l->sig == sig && l->srv) return l->srv;   /* unchanged since it was built */
    if (!l) {
        for (u32 i = 0; i < 32 && !l; i++) if (!luts[i].tex) l = &luts[i];
        if (!l) { l = &luts[0]; release_lut(l); }
    } else release_lut(l);
    l->guest = g; l->w = d->width; l->h = d->height; l->d = d->depth; l->fmt = d->dxgi;
    D3D11_TEXTURE3D_DESC td = {d->width, d->height, d->depth, 1, d->dxgi, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
    if (FAILED(ID3D11Device_CreateTexture3D(dev, &td, NULL, &l->tex))) { l->tex = NULL; return NULL; }
    D3D11_TEXTURE2D_DESC s2 = {d->width, d->height, 1, 1, slice[0]->storage, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
    ID3D11Texture2D *stage2 = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &s2, NULL, &stage2))) { release_lut(l); return NULL; }
    u32 rowb = d->width * d->bytes, rows = d->height, slicebytes = rowb * rows;
    u8 *buf = malloc((size_t)slicebytes);
    int repaired = 1;
    for (u32 k = 0; k < d->depth && repaired; k++) {
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)stage2, (ID3D11Resource *)slice[k]->tex);   /* same format+size */
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)stage2, 0, D3D11_MAP_READ, 0, &m))) { repaired = 0; break; }
        for (u32 y = 0; y < rows; y++) memcpy(buf + (size_t)y * rowb, (const u8 *)m.pData + (size_t)y * m.RowPitch, rowb);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)stage2, 0);
        /* slice k into the default 3D texture at depth slice k (a box, CPU bytes; no 2D->3D copy involved) */
        D3D11_BOX box = {0, 0, k, d->width, d->height, k + 1};
        ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)l->tex, 0, &box, buf, rowb, slicebytes);
    }
    free(buf);
    ID3D11Texture2D_Release(stage2);
    if (!repaired) { release_lut(l); return NULL; }
    D3D11_SHADER_RESOURCE_VIEW_DESC vd;
    memset(&vd, 0, sizeof vd);
    vd.Format = d->dxgi; vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D; vd.Texture3D.MipLevels = 1;
    if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)l->tex, &vd, &l->srv))) {
        ID3D11Texture3D_Release(l->tex); l->tex = NULL; return NULL;
    }
    l->sig = sig;
    if (rt_trace) fprintf(stderr, "[t24] assembled 3D LUT from %u 2D slices: guest %#llx %ux%ux%u fmt %#x dxgi %#x\n",
            d->depth, g, d->width, d->height, d->depth, d->format, d->dxgi);
    {   /* t24 self-check (SSBU_T24_SELFCHECK): read the assembled volume back and prove each input
         * axis maps to its own output channel (R->R, G->G, B->B across the three rows through the
         * volume centre). A collapsed Z axis shows as B out constant along B in. One dump per
         * distinct save key is written to <dir>/lut3d_asm_<key>.txt; the checker is
         * tests/check_lut3d.py (exits non-zero when an axis is flat). */
        static const char *sc_dir = (const char *)1;
        if (sc_dir == (const char *)1) sc_dir = getenv("SSBU_T24_SELFCHECK");
        if (sc_dir && l->tex) {
            D3D11_TEXTURE3D_DESC rd = td; rd.Usage = D3D11_USAGE_STAGING; rd.BindFlags = 0; rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ID3D11Texture3D *stg = NULL;
            if (SUCCEEDED(ID3D11Device_CreateTexture3D(dev, &rd, NULL, &stg))) {
                ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)stg, (ID3D11Resource *)l->tex);
                D3D11_MAPPED_SUBRESOURCE m;
                if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &m))) {
                    char path[512]; snprintf(path, sizeof path, "%s/lut3d_asm_%u_%ux%ux%u.txt", sc_dir, d->format, d->width, d->height, d->depth);
                    FILE *f = fopen(path, "w");
                    if (f) {
                        fprintf(f, "# assembled 3D LUT from %u GPU-rendered 2D slices; TIC fmt %#x dxgi %#x %ux%ux%u depth %u\n", d->depth, d->format, d->dxgi, d->width, d->height, d->depth, d->depth);
                        fprintf(f, "# v[z][y][x] RGBA\n");
                        for (u32 z = 0; z < d->depth; z++)
                            for (u32 y = 0; y < d->height; y++) {
                                const u8 *row = (const u8 *)m.pData + (size_t)z * m.DepthPitch + (size_t)y * m.RowPitch;
                                for (u32 x = 0; x < d->width; x++) {
                                    const u8 *p = row + (size_t)x * d->bytes;
                                    fprintf(f, "%u %u %u %u\n", p[0], p[1], p[2], d->bytes > 3 ? p[3] : 255);
                                }
                            }
                        fclose(f);
                        fprintf(stderr, "[t24] selfcheck wrote %s\n", path);
                    }
                    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)stg, 0);
                }
                ID3D11Texture3D_Release(stg);
            }
            {   /* same-frame A/B control: the OLD path's data - texture_read_3d over the guest image.
                 * The GPU-rendered slices are not in guest memory, so this reads the 0x400-stride
                 * block-linear layout over zeros/stale bytes and its Z (blue) axis is flat. Dumped
                 * with the same checker so the fail-able test has a negative control that must FAIL. */
                u32 rowb = d->width * d->bytes, rows = d->height;
                u8 *lbuf = malloc((size_t)rowb * rows * d->depth + 64);
                if (lbuf) {
                    texture_read_3d(d, g, lbuf);
                    char path[512]; snprintf(path, sizeof path, "%s/lut3d_legacy_%u_%ux%ux%u.txt", sc_dir, d->format, d->width, d->height, d->depth);
                    FILE *f = fopen(path, "w");
                    if (f) {
                        fprintf(f, "# LEGACY texture_read_3d(guest) (old path) for the same TIC; expected to be a collapsed/black control\n");
                        fprintf(f, "# %ux%ux%u RGBA\n", d->width, d->height, d->depth);
                        for (u32 z = 0; z < d->depth; z++)
                            for (u32 y = 0; y < d->height; y++)
                                for (u32 x = 0; x < d->width; x++) {
                                    const u8 *p = lbuf + ((size_t)z * rows + y) * rowb + (size_t)x * d->bytes;
                                    fprintf(f, "%u %u %u %u\n", p[0], p[1], p[2], d->bytes > 3 ? p[3] : 255);
                                }
                        fclose(f);
                    }
                    free(lbuf);
                }
            }
        }
    }
    return l->srv;
}

static ID3D11ShaderResourceView *get_texture(const u32 *tic, u32 shader_type) {
    TextureDesc d;
    int ok = tic_decode(tic, &d);
    u64 g = gpu_va_to_guest(d.address);
    if (!g) return NULL;
    /* A render target at this address: use it directly, read as the texture header says (sRGB
     * or not) - the latest one, as memory is reused. A depth buffer only for a depth format of
     * its size (shadow maps): the same memory may hold a colour image by now. */
    if (depth_sampling < 0) depth_sampling = !getenv("SSBU_NO_DEPTH_SAMPLING");
    int depth_tic = d.format == 0x29 || d.format == 0x2A || d.format == 0x2B || d.format == 0x2F || d.format == 0x30 || d.format == 0x3A;
    Surface *rt = NULL;
    for (u32 i = 0; i < nsurfaces; i++) {
        Surface *s = &surfaces[i];
        if (!s->tex || s->guest != g || !s->srv) continue;
        if (s->depth && (!depth_sampling || !depth_tic || !ok || s->width != d.width || s->height != d.height)) continue;
        if (!rt || s->used > rt->used) rt = s;
    }
    int want3d = ok && d.type == TIC_3D;
    {   /* t24 diagnostic (SSBU_T24_DIAG): mapping + actual SRV dimension for the tone-map LUT and the
         * scene-colour buffer. Addresses are printed for diagnosis only; nothing is keyed on them. */
        static int diag = -1;
        if (diag < 0) diag = getenv("SSBU_T24_DIAG") ? atoi(getenv("SSBU_T24_DIAG")) : 0;
        static u64 logged[64]; static u32 nlogged;
        int interesting = ok && (d.type == TIC_3D || (d.width == 1280 && d.height == 720));
        int seen = 0; for (u32 i = 0; i < nlogged; i++) seen |= logged[i] == d.address;
        if (diag && interesting && !seen && nlogged < 64) {
            logged[nlogged++] = d.address;
            fprintf(stderr, "[t24] va %#llx -> guest %#llx  TIC fmt %#x type %u %ux%ux%u lv%u bh%u bd%u srgb%u dxgi %#x shader_type %u\n",
                    d.address, g, d.format, d.type, d.width, d.height, d.depth, d.levels, d.block_height, d.block_depth, d.srgb, d.dxgi, shader_type);
            fprintf(stderr, "[t24]   exact 2D surface match: %s", rt ? "yes" : "no");
            if (rt) fprintf(stderr, " guest %#llx %ux%u fmt %#x depth %d used %llu (SRV dim %s)", rt->guest, rt->width, rt->height, rt->format, rt->depth, rt->used, rt->depth ? "2D-depth" : "2D");
            fprintf(stderr, "%s\n", want3d ? "  -> a 2D SRV cannot serve a Texture3D sampler" : "");
        }
    }
    if (want3d) {   /* a 3D sampler needs a real 3D SRV: assemble the rendered 2D slices, never a single one */
        ID3D11ShaderResourceView *as = assemble_sliced_3d(&d, g);
        if (as) return as;
        /* no slice family at the derived stride: fall through to the guest 3D read (below), not the 2D surface */
    } else {
        if (rt && draw_logging) note_sampled(rt);
        {   /* t26 diagnostic (SSBU_T26_DIAG; default off, no behaviour change): resolve the GPU-VA ->
             * guest alias for a sampled 2D render target and show the surface actually matched. This is
             * the one runtime fact t25 could not read from logs: whether the composite scene-colour VA
             * (0x22bf10000) and the tone-map LUT VA (0x23dfb0000) alias a live surface, and which one. */
            static int diag26 = -1;
            if (diag26 < 0) { diag26 = getenv("SSBU_T26_DIAG") ? atoi(getenv("SSBU_T26_DIAG")) : 0; if (diag26) fprintf(stderr, "[t26] alias probe ENABLED\n"); }
            if (diag26 && rt) {
                static u64 logged26[1024]; static u32 nlog26;
                int seen = 0; for (u32 i = 0; i < nlog26; i++) seen |= logged26[i] == d.address;
                int mismatch = (rt->width != d.width || rt->height != d.height);
                if (!seen && nlog26 < 1024) {
                    logged26[nlog26++] = d.address;
                    fprintf(stderr, "[t26] tex va %#llx -> guest %#llx | TIC %ux%u fmt %#x type %u | surface guest %#llx %ux%u fmt %#x depth %d | %s\n",
                            d.address, g, d.width, d.height, d.format, d.type,
                            rt->guest, rt->width, rt->height, rt->format, rt->depth, mismatch ? "SIZE-MISMATCH(alias)" : "exact");
                }
            }
        }
        if (rt) return surface_srv(rt, (int)d.srgb);
    }
    if (!ok || d.type == TIC_1D_BUFFER || d.bytes == 0) { warn_texture(&d); return NULL; }
    u32 layers = d.type == TIC_CUBE ? 6 : d.type == TIC_CUBE_ARRAY ? 6 * d.depth : d.type == TIC_2D_ARRAY || d.type == TIC_1D_ARRAY ? d.depth : 1;
    if (d.type == TIC_3D) layers = 1;
    u64 size = d.type == TIC_3D ? texture_3d_size(&d) : texture_layer_size(&d) * layers;
    int watch = watched(g, size);
    u64 sh = watch ? 0 : sample_hash(g, size);
    Texture *free_slot = NULL, *lru = NULL, *hit = texture_find(g, tic);
    if (hit) {
        if (watch ? !hit->stale : hit->sample_hash == sh) { hit->used = ++tick; return hit->srv; }
        free_slot = hit;   /* contents changed: rebuild in place */
    } else
        for (u32 i = 0; i < ntextures; i++) {   /* a new one: a free slot, else the least recently used */
            Texture *t = &textures[i];
            if (!t->srv) { if (!free_slot) free_slot = t; continue; }
            if (!lru || t->used < lru->used) lru = t;
        }
    Texture *t = free_slot;
    if (!t) t = ntextures < 4096 ? &textures[ntextures++] : lru;
    if (t->srv) { ID3D11ShaderResourceView_Release(t->srv); ID3D11Texture2D_Release(t->tex); t->srv = NULL; }
    if (t != hit) { t->guest = g; memcpy(t->tic, tic, 32); texture_index_add(t); }
    if (d.type == TIC_3D) {   /* level 0 only, which is all a lookup table has */
        D3D11_TEXTURE3D_DESC td = {d.width, d.height, d.depth, 1, d.dxgi, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0};
        u32 rowb = DIV_UP_(d.width, d.bw) * d.bytes, rows = DIV_UP_(d.height, d.bh);
        u8 *buf = malloc((size_t)rowb * rows * d.depth + 64);
        texture_read_3d(&d, g, buf);
        D3D11_SUBRESOURCE_DATA init = {buf, rowb, rowb * rows};
        ID3D11Texture3D *tex3;
        HRESULT hr = ID3D11Device_CreateTexture3D(dev, &td, &init, &tex3);
        free(buf);
        if (FAILED(hr)) { warn_texture(&d); return NULL; }
        D3D11_SHADER_RESOURCE_VIEW_DESC sd;
        memset(&sd, 0, sizeof sd);
        sd.Format = d.dxgi;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
        sd.Texture3D.MipLevels = 1;
        if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)tex3, &sd, &t->srv))) {
            ID3D11Texture3D_Release(tex3); t->srv = NULL; return NULL;
        }
        t->tex = (ID3D11Texture2D *)tex3;   /* only ever released, and skipped by captures */
        t->guest = g; t->size = size; t->watched = watch; t->stale = 0;
        memcpy(t->tic, tic, 32); t->sample_hash = sh; t->used = ++tick;
        gfx_stat_textures++;
        return t->srv;
    }
    u32 levels = d.levels, max_levels = 1;
    while ((d.width >> max_levels) || (d.height >> max_levels)) max_levels++;
    if (levels > max_levels) levels = max_levels;
    D3D11_TEXTURE2D_DESC td = {d.width, d.height, levels, layers, d.dxgi, {1, 0}, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0,
                               layers == 6 || (d.type == TIC_CUBE_ARRAY) ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0};
    if (d.type != TIC_CUBE && d.type != TIC_CUBE_ARRAY) td.MiscFlags = 0;
    if (d.bw > 1 && ((d.width % 4) || (d.height % 4))) {   /* block-compressed sizes must be multiples of 4 at level 0 */
        td.Width = (d.width + 3) & ~3u; td.Height = (d.height + 3) & ~3u;
    }
    D3D11_SUBRESOURCE_DATA *init = calloc(levels * layers, sizeof *init);
    u8 **mem = calloc(levels * layers, sizeof *mem);
    for (u32 layer = 0; layer < layers; layer++)
        for (u32 l = 0; l < levels; l++) {
            u32 w = (d.width >> l) ? d.width >> l : 1, h = (d.height >> l) ? d.height >> l : 1;
            u32 bw = (w + d.bw - 1) / d.bw, bh = (h + d.bh - 1) / d.bh;
            u8 *buf = malloc((size_t)bw * bh * d.bytes + 64);
            texture_read(&d, g, layer, l, buf);
            gfx_stat_upload_bytes += (u64)bw * bh * d.bytes;
            gfx_stat_upload_kind[3] += (u64)bw * bh * d.bytes;
            init[layer * levels + l] = (D3D11_SUBRESOURCE_DATA){buf, bw * d.bytes, bw * bh * d.bytes};
            mem[layer * levels + l] = buf;
        }
    HRESULT hr = ID3D11Device_CreateTexture2D(dev, &td, init, &t->tex);
    for (u32 i = 0; i < levels * layers; i++) free(mem[i]);
    free(mem);
    free(init);
    if (FAILED(hr)) { warn_texture(&d); t->tex = NULL; return NULL; }
    D3D11_SHADER_RESOURCE_VIEW_DESC sd;
    memset(&sd, 0, sizeof sd);
    sd.Format = d.dxgi == DXGI_FORMAT_R24G8_TYPELESS ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : d.dxgi;
    switch (shader_type) {
    case TEX_ARRAY_2D: sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY; sd.Texture2DArray.MipLevels = levels; sd.Texture2DArray.ArraySize = layers; break;
    case TEX_CUBE: sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE; sd.TextureCube.MipLevels = levels; break;
    case TEX_ARRAY_CUBE: sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY; sd.TextureCubeArray.MipLevels = levels; sd.TextureCubeArray.NumCubes = layers / 6; break;
    default:
        if (layers > 1) { sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY; sd.Texture2DArray.MipLevels = levels; sd.Texture2DArray.ArraySize = layers; }
        else { sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = levels; }
    }
    if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)t->tex, &sd, &t->srv))) {
        ID3D11Texture2D_Release(t->tex); t->tex = NULL; t->srv = NULL; return NULL;
    }
    t->guest = g; t->size = size; t->watched = watch; t->stale = 0;
    memcpy(t->tic, tic, 32); t->sample_hash = sh; t->used = ++tick;
    gfx_stat_textures++;
    return t->srv;
}

typedef struct Sampler { u32 tsc[8]; ID3D11SamplerState *state; } Sampler;
static Sampler samplers[1024];
static u32 nsamplers;

static D3D11_TEXTURE_ADDRESS_MODE wrap_mode(u32 m) {
    switch (m) {
    case 0: return D3D11_TEXTURE_ADDRESS_WRAP;
    case 1: return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 3: case 6: return D3D11_TEXTURE_ADDRESS_BORDER;
    case 5: case 7: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
    }
    return D3D11_TEXTURE_ADDRESS_CLAMP;
}

static ID3D11SamplerState *get_sampler(const u32 *tsc) {
    for (u32 i = 0; i < nsamplers; i++) if (!memcmp(samplers[i].tsc, tsc, 32)) return samplers[i].state;
    D3D11_SAMPLER_DESC sd;
    memset(&sd, 0, sizeof sd);
    u32 mag = tsc[1] & 3, min = (tsc[1] >> 4) & 3, mip = (tsc[1] >> 6) & 3, aniso = (tsc[0] >> 20) & 7, cmp = (tsc[0] >> 9) & 1;
    int lin_mag = mag == 2, lin_min = min == 2, lin_mip = mip == 3;
    sd.Filter = (D3D11_FILTER)((lin_mip ? 1 : 0) | (lin_mag ? 4 : 0) | (lin_min ? 0x10 : 0));
    if (aniso) { sd.Filter = D3D11_FILTER_ANISOTROPIC; sd.MaxAnisotropy = 1u << aniso; }
    if (cmp) sd.Filter = (D3D11_FILTER)(sd.Filter | 0x80);
    sd.AddressU = wrap_mode(tsc[0] & 7); sd.AddressV = wrap_mode((tsc[0] >> 3) & 7); sd.AddressW = wrap_mode((tsc[0] >> 6) & 7);
    sd.ComparisonFunc = (D3D11_COMPARISON_FUNC)(((tsc[0] >> 10) & 7) + 1);
    i32 bias = (i32)(tsc[1] << 7) >> 19;   /* 13-bit signed, 8 fractional bits */
    sd.MipLODBias = bias / 256.0f;
    sd.MinLOD = (tsc[2] & 0xFFF) / 256.0f;
    sd.MaxLOD = mip == 1 ? 0.0f : ((tsc[2] >> 12) & 0xFFF) / 256.0f;
    memcpy(sd.BorderColor, &tsc[4], 16);
    ID3D11SamplerState *state = NULL;
    ID3D11Device_CreateSamplerState(dev, &sd, &state);
    if (nsamplers < 1024) { memcpy(samplers[nsamplers].tsc, tsc, 32); samplers[nsamplers++].state = state; }
    return state;
}

/* ---- fixed-function state ---------------------------------------------------------- */
static D3D11_COMPARISON_FUNC compare_func(u32 f) {
    if (f >= 0x200 && f <= 0x207) return (D3D11_COMPARISON_FUNC)(f - 0x200 + 1);   /* GL-style values */
    if (f >= 1 && f <= 8) return (D3D11_COMPARISON_FUNC)f;                          /* D3D-style */
    return D3D11_COMPARISON_ALWAYS;
}

static D3D11_STENCIL_OP stencil_op(u32 op) {
    switch (op) {
    case 1: case 0x1E00: return D3D11_STENCIL_OP_KEEP;
    case 2: case 0: return D3D11_STENCIL_OP_ZERO;
    case 3: case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 4: case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 5: case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 6: case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 7: case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 8: case 0x8508: return D3D11_STENCIL_OP_DECR;
    }
    return D3D11_STENCIL_OP_KEEP;
}

static D3D11_BLEND blend_factor(u32 f) {
    switch (f) {
    case 0x1: case 0x4000: return D3D11_BLEND_ZERO;
    case 0x2: case 0x4001: return D3D11_BLEND_ONE;
    case 0x3: case 0x4300: return D3D11_BLEND_SRC_COLOR;
    case 0x4: case 0x4301: return D3D11_BLEND_INV_SRC_COLOR;
    case 0x5: case 0x4302: return D3D11_BLEND_SRC_ALPHA;
    case 0x6: case 0x4303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x7: case 0x4304: return D3D11_BLEND_DEST_ALPHA;
    case 0x8: case 0x4305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x9: case 0x4306: return D3D11_BLEND_DEST_COLOR;
    case 0xA: case 0x4307: return D3D11_BLEND_INV_DEST_COLOR;
    case 0xB: case 0x4308: return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0xE: case 0xC001: case 0xC003: return D3D11_BLEND_BLEND_FACTOR;
    case 0xF: case 0xC002: case 0xC004: return D3D11_BLEND_INV_BLEND_FACTOR;
    case 0x10: case 0xC900: return D3D11_BLEND_SRC1_COLOR;
    case 0x11: case 0xC901: return D3D11_BLEND_INV_SRC1_COLOR;
    case 0x12: case 0xC902: return D3D11_BLEND_SRC1_ALPHA;
    case 0x13: case 0xC903: return D3D11_BLEND_INV_SRC1_ALPHA;
    }
    return D3D11_BLEND_ONE;
}

static D3D11_BLEND alpha_factor(D3D11_BLEND b) {   /* colour factors are not allowed for alpha */
    switch (b) {
    case D3D11_BLEND_SRC_COLOR: return D3D11_BLEND_SRC_ALPHA;
    case D3D11_BLEND_INV_SRC_COLOR: return D3D11_BLEND_INV_SRC_ALPHA;
    case D3D11_BLEND_DEST_COLOR: return D3D11_BLEND_DEST_ALPHA;
    case D3D11_BLEND_INV_DEST_COLOR: return D3D11_BLEND_INV_DEST_ALPHA;
    case D3D11_BLEND_SRC1_COLOR: return D3D11_BLEND_SRC1_ALPHA;
    case D3D11_BLEND_INV_SRC1_COLOR: return D3D11_BLEND_INV_SRC1_ALPHA;
    default: return b;
    }
}

static D3D11_BLEND_OP blend_op(u32 op) {
    switch (op) {
    case 2: case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 3: case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 4: case 0x8007: return D3D11_BLEND_OP_MIN;
    case 5: case 0x8008: return D3D11_BLEND_OP_MAX;
    }
    return D3D11_BLEND_OP_ADD;
}

/* State objects are cached by their descriptions. */
#define STATE_CACHE(Name, Desc, Iface, Create)                                                   \
    static struct { Desc desc; Iface *obj; } Name##_cache[512];                                    \
    static u32 Name##_count;                                                                      \
    static Iface *Name(const Desc *d) {                                                           \
        for (u32 i = 0; i < Name##_count; i++)                                                    \
            if (!memcmp(&Name##_cache[i].desc, d, sizeof *d)) return Name##_cache[i].obj;         \
        Iface *o = NULL;                                                                          \
        Create(dev, d, &o);                                                                       \
        if (Name##_count < 512) { Name##_cache[Name##_count].desc = *d; Name##_cache[Name##_count++].obj = o; } \
        return o;                                                                                 \
    }
STATE_CACHE(raster_state, D3D11_RASTERIZER_DESC, ID3D11RasterizerState, ID3D11Device_CreateRasterizerState)
STATE_CACHE(depth_state, D3D11_DEPTH_STENCIL_DESC, ID3D11DepthStencilState, ID3D11Device_CreateDepthStencilState)
STATE_CACHE(blend_state, D3D11_BLEND_DESC, ID3D11BlendState, ID3D11Device_CreateBlendState)

/* ---- draws ------------------------------------------------------------------------- */
u64 gfx_stat_drawn, gfx_stat_skipped, gfx_skip_shader, gfx_skip_topology, gfx_skip_target;
/* SSBU_GFX_DEBUG bits: 1 all colour channels written, 2 no depth/stencil test, 4 no culling,
 * 8 no blending, 16 log every draw of one frame (after SSBU_GFX_LOG_AT seconds) */
static u32 gfx_debug(void) {
    static int checked;
    static u32 v;
    if (!checked++ && getenv("SSBU_GFX_DEBUG")) v = (u32)strtoul(getenv("SSBU_GFX_DEBUG"), NULL, 0);
    return v;
}
static int draw_logging;
static u32 instance_counter;

static D3D11_PRIMITIVE_TOPOLOGY topology(u32 t) {
    switch (t) {
    case 0: return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
    case 1: return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
    case 3: return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case 4: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case 5: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    case 7: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;   /* QUADS: expanded to triangles by the caller */
    case 10: return D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ;
    case 11: return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ;
    case 12: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ;
    case 13: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ;
    }
    return D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
}

/* Constant buffers of 4 KB or more stay on the GPU between draws: NVN binds 64 KB for each
 * program's own constants, and sending that again for every draw took most of the frame. They
 * are kept as the GPU would see memory: writes in the command stream (constant loads, inline
 * and copy-engine writes - the HUD's positions change between draws that way) are applied to
 * them in order, and what the CPU wrote is found by the write watch, asked at a buffer's first
 * use in each submission; memory outside the watched area is never kept. */
typedef struct KeptCb { u64 va, guest, used, checked; u32 size, mapped; int stale; ID3D11Buffer *buf; ID3D11ShaderResourceView *srv; u8 *shadow; } KeptCb;
extern u64 gpu_stat_batches;   /* (gpu.c: submissions so far) */
#define KEPT_SETS 256
#define KEPT_WAYS 4
static KeptCb kept[KEPT_SETS * KEPT_WAYS];
static u64 kept_clock;
static u16 kept_blocks[1 << 16];   /* how many kept buffers touch each (hashed) 64 KB block of GPU VA */
#define KEPT_BLOCK(block) ((u32)((u64)(block) * 0x9E3779B97F4A7C15ull >> 48))
/* Command stream writes not yet applied to kept buffers: per thread (a command list runs on
 * the thread that submits it, its draws too), so extending the run needs no lock. */
static __declspec(thread) u64 pending_lo, pending_hi;
static int kept_verify = -1;   /* SSBU_KEPT_VERIFY (debugging): keep a copy of what each holds, and report draws that find memory different */
u64 gfx_stat_kept_mismatches, gfx_stat_kept_made, gfx_stat_kept_applied, gfx_stat_kept_reloaded;

static void kept_blocks_add(const KeptCb *e, int d) {
    for (u64 b = e->va >> 16; b <= (e->va + e->size - 1) >> 16; b++) kept_blocks[KEPT_BLOCK(b)] += (u16)d;
}

/* The command stream wrote [lo, hi) of GPU VA (guest memory has it already). */
static void kept_apply(u64 lo, u64 hi) {
    int any = 0;
    for (u64 b = lo >> 16; b <= (hi - 1) >> 16 && !any; b++) any = kept_blocks[KEPT_BLOCK(b)] != 0;
    if (!any) return;   /* (almost always: no kept buffer is near) */
    for (u32 i = 0; i < KEPT_SETS * KEPT_WAYS; i++) {
        KeptCb *e = &kept[i];
        if (!e->srv || e->stale || hi <= e->va || lo >= e->va + e->mapped) continue;
        u64 a = (lo > e->va ? lo : e->va) & ~3ull, z = (hi < e->va + e->mapped ? hi : e->va + e->mapped) + 3 & ~3ull;
        D3D11_BOX box = {(UINT)(a - e->va), 0, 0, (UINT)(z - e->va), 1, 1};
        ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)e->buf, 0, &box, P(void, e->guest + (a - e->va)), 0, 0);
        if (e->shadow) memcpy(e->shadow + (a - e->va), P(void, e->guest + (a - e->va)), (size_t)(z - a));
        gfx_stat_kept_applied++;
        gfx_stat_upload_bytes += z - a;
        gfx_stat_upload_kind[0] += z - a;
    }
}

static void kept_flush(void) {
    if (!pending_hi) return;
    u64 lo = pending_lo, hi = pending_hi;
    pending_hi = 0;
    kept_apply(lo, hi);
}

/* The command stream wrote guest memory at [va, va + bytes): runs of it are applied to the
 * kept buffers together, before anything is drawn. */
void gfx_stream_wrote(u64 va, u64 bytes) {
    if (pending_hi && pending_hi == va) { pending_hi += bytes; return; }
    if (pending_hi && enter()) { kept_flush(); leave(); }
    pending_lo = va;
    pending_hi = va + bytes;
}

/* A command list has been run: what it wrote reaches the kept buffers before another
 * thread's draws can use them. */
void gfx_stream_end(void) {
    if (pending_hi && enter()) { kept_flush(); leave(); }
}

static ID3D11ShaderResourceView *kept_cb(u64 va, u32 size) {
    KeptCb *set = &kept[((va >> 4) * 0x9E3779B97F4A7C15ull >> 56) * KEPT_WAYS], *e = NULL, *lru = &set[0];
    for (u32 w = 0; w < KEPT_WAYS && !e; w++) {
        if (set[w].srv && set[w].va == va && set[w].size == size) e = &set[w];
        else if (set[w].used < lru->used) lru = &set[w];
    }
    u64 g = gpu_va_to_guest(va);
    if (e && e->guest == g) {
        if (e->checked != gpu_stat_batches) {   /* first use in this submission: has the CPU written it since? */
            void *page;
            ULONG_PTR count = 1;
            ULONG granularity;
            if (GetWriteWatch(0, (void *)g, e->mapped, &page, &count, &granularity) == 0 && count) {
                e->stale = 1;
                ResetWriteWatch((void *)g, e->mapped);
            }
            e->checked = gpu_stat_batches;
        }
        if (e->stale) {   /* take it all again */
            D3D11_BOX box = {0, 0, 0, e->mapped, 1, 1};
            ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)e->buf, 0, &box, P(void, g), 0, 0);
            if (e->shadow) memcpy(e->shadow, P(void, g), e->mapped);
            gfx_stat_kept_reloaded++;
            gfx_stat_upload_bytes += e->mapped;
            gfx_stat_upload_kind[0] += e->mapped;
            e->stale = 0;
        }
        e->used = ++kept_clock;
        if (e->shadow && memcmp(e->shadow, P(void, g), e->mapped)) {
            u32 o = 0;
            while (o + 4 <= e->mapped && !memcmp(e->shadow + o, P(u8, g) + o, 4)) o += 4;
            if (gfx_stat_kept_mismatches++ < 200)
                fprintf(stderr, "[kept] va %#llx (%#x bytes) +%#x: kept %08x, memory %08x (submission %llu)\n", va, e->mapped, o,
                        ld32((u64)(e->shadow + o)), ld32(g + o), gpu_stat_batches);
        }
        return e->srv;
    }
    if (!g || size < 0x1000) return NULL;
    u32 mapped = 0x1000 - (u32)(va & 0xFFF);   /* (the 64 KB bound may run past what is mapped: that part reads as zeros) */
    while (mapped < size && gpu_va_to_guest(va + mapped) == g + mapped) mapped += 0x1000;
    if (mapped > size) mapped = size;
    if (!watched(g, mapped)) return NULL;
    if (!e) e = lru;
    if (e->srv) kept_blocks_add(e, -1);
    bury(e->srv);   /* (may be bound still) */
    bury(e->buf);
    free(e->shadow);
    memset(e, 0, sizeof *e);
    if (kept_verify < 0) kept_verify = getenv("SSBU_KEPT_VERIFY") != NULL;
    u32 cap = (size + 15) & ~15u;
    D3D11_BUFFER_DESC bd = {cap, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
    u8 *init = calloc(1, cap);
    memcpy(init, P(void, g), mapped);
    D3D11_SUBRESOURCE_DATA sd = {init, 0, 0};
    HRESULT hr = ID3D11Device_CreateBuffer(dev, &bd, &sd, &e->buf);
    free(init);
    if (FAILED(hr)) { e->buf = NULL; return NULL; }
    D3D11_SHADER_RESOURCE_VIEW_DESC vd;
    memset(&vd, 0, sizeof vd);
    vd.Format = DXGI_FORMAT_R32_TYPELESS;
    vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    vd.BufferEx.NumElements = cap / 4;
    vd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)e->buf, &vd, &e->srv))) { ID3D11Buffer_Release(e->buf); e->buf = NULL; return NULL; }
    e->va = va; e->guest = g; e->size = size; e->mapped = mapped; e->used = ++kept_clock; e->checked = gpu_stat_batches;
    if (kept_verify) { e->shadow = malloc(mapped); memcpy(e->shadow, P(void, g), mapped); }
    gfx_stat_kept_made++;
    kept_blocks_add(e, 1);
    gfx_stat_upload_bytes += size;
    gfx_stat_upload_kind[0] += size;
    return e->srv;
}

/* ---- draw state, set only when it changes ---------------------------------------------
 * A draw sets the whole pipeline; most of it is what the draw before set. What the context has
 * is mirrored here, and anything else that uses the context (clears, blits, compute, captures,
 * presenting) makes the mirror unknown, so the next draw sets everything again. Binding render
 * targets makes D3D drop shader resource bindings of the same textures, so a change of render
 * targets makes the resource bindings unknown too. */
#define UNKNOWN_PTR ((void *)1)
static struct {
    ID3D11ShaderResourceView *srv[2][112];
    ID3D11SamplerState *smp[2][16];
    ID3D11RenderTargetView *rtv[8];
    ID3D11DepthStencilView *dsv;
    D3D11_VIEWPORT vp;
    D3D11_RECT scissor;
    ID3D11RasterizerState *rs;
    ID3D11DepthStencilState *ds;
    UINT stencil_ref;
    ID3D11BlendState *bs;
    float blend_factor[4];
    void *vs, *ps;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    ID3D11Buffer *ib;
    DXGI_FORMAT ib_format;
    int fixed_set;   /* input layout and the per-draw constant buffer bound */
} cur;

static void forget_resource_bindings(void) {
    for (u32 s = 0; s < 2; s++) {
        for (u32 i = 0; i < 112; i++) cur.srv[s][i] = UNKNOWN_PTR;
        for (u32 i = 0; i < 16; i++) cur.smp[s][i] = UNKNOWN_PTR;
    }
}

static void forget_draw_state(void) {
    forget_resource_bindings();
    for (u32 i = 0; i < 8; i++) cur.rtv[i] = UNKNOWN_PTR;
    cur.dsv = UNKNOWN_PTR;
    cur.vp.Width = -1;
    cur.scissor.right = -1;
    cur.rs = UNKNOWN_PTR; cur.ds = UNKNOWN_PTR; cur.bs = UNKNOWN_PTR;
    cur.vs = cur.ps = UNKNOWN_PTR;
    cur.topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    cur.ib = UNKNOWN_PTR;
    cur.fixed_set = 0;
}

/* Bind views to a stage's slots [start, start + count): just the span that differs. */
static void set_srvs(int stage, u32 start, u32 count, ID3D11ShaderResourceView *const *v) {
    ID3D11ShaderResourceView **have = &cur.srv[stage == SHADER_VERTEX ? 0 : 1][start];
    u32 lo = count, hi = 0;
    for (u32 i = 0; i < count; i++)
        if (have[i] != v[i]) { if (lo == count) lo = i; hi = i + 1; }
    if (lo == count) return;
    memcpy(have + lo, v + lo, (hi - lo) * sizeof *v);
    if (stage == SHADER_VERTEX) ID3D11DeviceContext_VSSetShaderResources(ctx, start + lo, hi - lo, v + lo);
    else ID3D11DeviceContext_PSSetShaderResources(ctx, start + lo, hi - lo, v + lo);
}

static void set_samplers(int stage, u32 count, ID3D11SamplerState *const *v) {
    ID3D11SamplerState **have = cur.smp[stage == SHADER_VERTEX ? 0 : 1];
    u32 lo = count, hi = 0;
    for (u32 i = 0; i < count; i++)
        if (have[i] != v[i]) { if (lo == count) lo = i; hi = i + 1; }
    if (lo == count) return;
    memcpy(have + lo, v + lo, (hi - lo) * sizeof *v);
    if (stage == SHADER_VERTEX) ID3D11DeviceContext_VSSetSamplers(ctx, lo, hi - lo, v + lo);
    else ID3D11DeviceContext_PSSetSamplers(ctx, lo, hi - lo, v + lo);
}

/* Bind a stage's constant buffers and textures. */
static void bind_resources(const u32 *regs, const CbBinding cb[STAGES][18], int stage, const Shader *s, DrawParams *params) {
    int mstage = stage == SHADER_VERTEX ? STAGE_VERTEX : STAGE_FRAGMENT;
    ID3D11ShaderResourceView *srv[18] = {0};
    static u8 data[0x10000];
    (void)regs;
    for (u32 b = 0; b < 18; b++) {
        if (!(s->info.cbuf_mask >> b & 1)) continue;
        u32 size = cb[mstage][b].size;
        if (size > 0x10000) size = 0x10000;
        if (size > s->info.cbuf_extent[b]) size = s->info.cbuf_extent[b];   /* only what the program can read */
        if (size >= 0x1000 && cb[mstage][b].address && !draw_logging && (srv[b] = kept_cb(cb[mstage][b].address, size)))
            continue;   /* big ones stay on the GPU */
        if (size > gfx_stat_biggest_cb.size) {   /* (for the statistics: the largest one lately) */
            gfx_stat_biggest_cb.size = size;
            gfx_stat_biggest_cb.bank = b;
            gfx_stat_biggest_cb.extent = s->info.cbuf_extent[b];
            snprintf(gfx_stat_biggest_cb.shader, sizeof gfx_stat_biggest_cb.shader, "%s", s->name);
        }
        if (cb[mstage][b].address && size && !draw_logging) { srv[b] = upload_gpu(stage, b, cb[mstage][b].address, size); continue; }
        if (cb[mstage][b].address && size) read_gpu(cb[mstage][b].address, size, data);
        else { size = 16; memset(data, 0, 16); }
        if (size < 32) memset(data + size, 0, 32 - size);   /* (not the previous buffer's words: the log shows 8) */
        if (draw_logging) {
            const float *f = (const float *)data;
            fprintf(stderr, "[draw]   %s cb%u: va %#llx size %#x guest %#llx: %g %g %g %g | %g %g %g %g\n",
                    stage == SHADER_VERTEX ? "vs" : "ps", b, cb[mstage][b].address, cb[mstage][b].size,
                    gpu_va_to_guest(cb[mstage][b].address), f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
            const char *more = getenv("SSBU_GFX_LOG_CB");   /* this bank in full (first 64 words) */
            if (more && strtoul(more, NULL, 0) == b) {
                u32 before[64];
                read_gpu(cb[mstage][b].address - 0x100, 0x100, before);
                for (u32 i = 0; i < 64; i += 8)
                    fprintf(stderr, "[draw]     -%03x: %08x %08x %08x %08x %08x %08x %08x %08x\n", 0x100 - i * 4, before[i], before[i + 1],
                            before[i + 2], before[i + 3], before[i + 4], before[i + 5], before[i + 6], before[i + 7]);
                const u32 *u = (const u32 *)data;
                for (u32 i = 0; i < 64 && i * 4 < size; i += 8)
                    fprintf(stderr, "[draw]     +%03x: %08x %08x %08x %08x %08x %08x %08x %08x\n", i * 4, u[i], u[i + 1], u[i + 2], u[i + 3],
                            u[i + 4], u[i + 5], u[i + 6], u[i + 7]);
            }
        }
        srv[b] = upload_raw(stage, b, data, size);
    }
    set_srvs(stage, 0, 18, srv);

    /* storage buffers, for global memory loads: their descriptors are in constant buffer 0 */
    if (s->info.sb_count) {
        ID3D11ShaderResourceView *gm[16] = {0};
        static u8 *gdata;
        static u32 gcap;
        for (u32 k = 0; k < s->info.sb_count; k++) {
            u32 desc[4] = {0};
            if (cb[mstage][0].address) read_gpu(cb[mstage][0].address + s->info.sb_offset[k], 16, desc);
            u64 va = (u64)desc[1] << 32 | desc[0];
            /* the recorded size is not always the extent read: take the rest of the last page too */
            u64 end = (va + desc[2] + 0xFFF) & ~0xFFFull;
            u32 size = desc[2] && end - va <= (16u << 20) ? (u32)(end - va) : desc[2] ? 16u << 20 : 0;
            params->sb[stage][k][0] = desc[0];
            params->sb[stage][k][1] = desc[1];
            params->sb[stage][k][2] = size;
            if (draw_logging)
                fprintf(stderr, "[draw]   %s storage buffer %u (cb0+%#x): va %#llx size %#x\n", stage == SHADER_VERTEX ? "vs" : "ps", k,
                        s->info.sb_offset[k], va, desc[2]);
            if (!va || !size) continue;
            Resident *res = (va & 3) == 0 ? resident_covering(va, va + size) : NULL;   /* written by compute: the GPU's copy */
            if (res && (gm[k] = resident_view(res, va, size))) continue;
            int partly = resident_overlapping(va, size) != NULL;
            if (!partly && !draw_logging) { gm[k] = upload_gpu(stage, 96 + k, va, size); continue; }
            if (size > gcap) { gdata = realloc(gdata, size); gcap = size; }
            read_gpu(va, size, gdata);
            if (partly) {   /* partly: the guest's data around the GPU's */
                if ((gm[k] = upload_composed(stage, 96 + k, va, gdata, size))) continue;
            }
            if (draw_logging) {
                const u32 *u = (const u32 *)gdata;
                for (u32 i = 0; i < 32 && i * 4 < size; i += 8)
                    fprintf(stderr, "[draw]     +%03x: %08x %08x %08x %08x %08x %08x %08x %08x\n", i * 4, u[i], u[i + 1], u[i + 2], u[i + 3],
                            u[i + 4], u[i + 5], u[i + 6], u[i + 7]);
            }
            gm[k] = upload_raw(stage, 96 + k, gdata, size);
        }
        set_srvs(stage, 96, 16, gm);
    }

    ID3D11ShaderResourceView *tex[32] = {0};
    ID3D11SamplerState *smp[16] = {0};
    u32 tex_cb = regs[M_BINDLESS_TEXTURE] & 0x1F;
    u64 tic_pool = (u64)regs[M_TEX_HEADER_POOL] << 32 | regs[M_TEX_HEADER_POOL + 1];
    u64 tsc_pool = (u64)regs[M_TEX_SAMPLER_POOL] << 32 | regs[M_TEX_SAMPLER_POOL + 1];
    for (u32 k = 0; k < s->info.ntex; k++) {
        u32 handle = 0, tic[8], tsc[8];
        if (tex_cb < 18 && cb[mstage][tex_cb].address) read_gpu(cb[mstage][tex_cb].address + s->info.tex[k].handle_offset, 4, &handle);
        u32 tic_index = handle & 0xFFFFF, tsc_index = regs[M_SAMPLER_BINDING] & 1 ? tic_index : (handle >> 20) & 0xFFF;
        read_gpu(tic_pool + tic_index * 32ull, 32, tic);
        read_gpu(tsc_pool + tsc_index * 32ull, 32, tsc);
        tex[k] = get_texture(tic, s->info.tex[k].type);
        if (k < 16) smp[k] = get_sampler(tsc);
        params->swz[stage][k] = (tic[0] >> 19) & 0xFFF;
        if (draw_logging) {
            TextureDesc d;
            int ok = tic_decode(tic, &d);
            int rt = 0;
            for (u32 i = 0; i < nsurfaces && !rt; i++) rt = surfaces[i].tex && surfaces[i].guest == gpu_va_to_guest(d.address);
            fprintf(stderr, "[draw]   %s tex%u: cb%u+%#x handle %#x -> tic %u tsc %u: fmt %#x %ux%u type %u layout %u va %#llx swz %#x srgb %u "
                            "wrap %u/%u filter %#x %s%s%s\n",
                    stage == SHADER_VERTEX ? "vs" : "ps", k, tex_cb, s->info.tex[k].handle_offset, handle, tic_index, tsc_index,
                    d.format, d.width, d.height, d.type, d.header_version, d.address, (tic[0] >> 19) & 0xFFF, d.srgb, tsc[0] & 7,
                    (tsc[0] >> 3) & 7, tsc[1] & 0xFF, ok ? "" : "(format unknown) ", tex[k] ? "bound" : "NOT BOUND", rt ? " (render target)" : "");
        }
    }
    if (s->info.ntex) {
        set_srvs(stage, 64, 32, tex);
        set_samplers(stage, 16, smp);
    }
}

void gfx_draw(const u32 *regs, const CbBinding cb[STAGES][18], int indexed) {
    if (!enter()) return;
    kept_flush();
    if (draw_logging) save_sampled();
    static int state_known;
    if (!state_known) { forget_draw_state(); state_known = 1; }
    rings_start_draw();
    dump_shaders(regs);
    u32 begin = regs[M_BEGIN];
    if (begin & (1u << 26)) instance_counter++;
    else if (!(begin & (1u << 27))) instance_counter = 0;

    D3D11_PRIMITIVE_TOPOLOGY topo = topology(begin & 0xFFFF);
    u64 region = (u64)regs[M_PROGRAM_REGION] << 32 | regs[M_PROGRAM_REGION + 1];
    u64 vs_g = gpu_va_to_guest(region + regs[M_PIPELINE(1) + 1]);
    u64 ps_g = regs[M_PIPELINE(5)] & 1 ? gpu_va_to_guest(region + regs[M_PIPELINE(5) + 1]) : 0;
    /* the pixel shader is asked for once the vertex shader is translated (its outputs are the
     * pixel shader's inputs), not once it is compiled: both compile at the same time */
    Shader *vs_e = vs_g ? find_shader_ex(vs_g, SHADER_VERTEX, 0, cb[STAGE_VERTEX], NULL, 0) : NULL;
    Shader *ps_e = vs_e && vs_e->state != 2 && ps_g ? find_shader_ex(ps_g, SHADER_PIXEL, vs_e->info.outputs, cb[STAGE_FRAGMENT], NULL, 0) : NULL;
    Shader *vs = vs_e && vs_e->state == 1 ? vs_e : NULL, *ps = ps_e && ps_e->state == 1 ? ps_e : NULL;
    /* no fragment stage (depth only: shadow maps, depth pre-passes) draws with no pixel shader */
    if (!vs || (ps_g && !ps) || topo == D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED) {
        gfx_stat_skipped++;
        if (topo == D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED) gfx_skip_topology++; else gfx_skip_shader++;
        leave();
        return;
    }

    /* render targets */
    ID3D11RenderTargetView *rtv[8] = {0};
    ID3D11DepthStencilView *dsv = NULL;
    u32 sel = regs[M_CT_SELECT], count = sel & 15, rt_w = 0, rt_h = 0;
    for (u32 i = 0; i < count && i < 8; i++) {
        u32 ct = (sel >> (4 + 3 * i)) & 7;
        u64 va = (u64)regs[M_CT(ct)] << 32 | regs[M_CT(ct) + 1];
        if (!va || !regs[M_CT(ct) + 4]) continue;
        Surface *sf = surface(gpu_va_to_guest(va), regs[M_CT(ct) + 2], regs[M_CT(ct) + 3], regs[M_CT(ct) + 4], 0);
        if (sf) { rtv[i] = sf->rtv; if (!rt_w) { rt_w = sf->width; rt_h = sf->height; } }
    }
    if (regs[M_ZT_SELECT] & 1) {
        u64 va = (u64)regs[M_ZT_A] << 32 | regs[M_ZT_A + 1];
        Surface *sf = surface(gpu_va_to_guest(va), regs[M_ZT_SIZE], regs[M_ZT_SIZE + 1], regs[M_ZT_A + 2], 1);
        if (sf) { dsv = sf->dsv; if (!rt_w) { rt_w = sf->width; rt_h = sf->height; } }
    }
    if (!rt_w) { gfx_stat_skipped++; gfx_skip_target++; leave(); return; }
    /* (a render target bound as a texture of the same draw is unbound by D3D: fine) */
    if (memcmp(cur.rtv, rtv, sizeof rtv) || cur.dsv != dsv) {
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 8, rtv, dsv);
        memcpy(cur.rtv, rtv, sizeof rtv);
        cur.dsv = dsv;
        forget_resource_bindings();
    }

    /* viewport and the clip-space fix-up */
    DrawParams params;
    memset(&params, 0, sizeof params);
    float sx = f32_of(regs[M_VIEWPORT_SCALE(0)]), sy = f32_of(regs[M_VIEWPORT_SCALE(0) + 1]), sz = f32_of(regs[M_VIEWPORT_SCALE(0) + 2]);
    float ox = f32_of(regs[M_VIEWPORT_SCALE(0) + 3]), oy = f32_of(regs[M_VIEWPORT_SCALE(0) + 4]), oz = f32_of(regs[M_VIEWPORT_SCALE(0) + 5]);
    int lower_left = regs[M_WINDOW_ORIGIN] & 1;
    if (lower_left) { oy = (float)rt_h - oy; sy = -sy; }
    /* Depth: the hardware clips z to [-w, w] or [0, w] (VIEWPORT_Z_CLIP), then maps it with
     * the viewport's z scale and offset. D3D clips to [0, w], so a [-w, w] range is halved
     * into it, and the mapping becomes the viewport's depth range. */
    int minus_one = !(regs[M_VIEWPORT_Z_CLIP] & 1);
    float zmin = minus_one ? oz - sz : oz, zmax = oz + sz;
    if (zmin < 0) zmin = 0;
    if (zmin > 1) zmin = 1;
    if (zmax < 0) zmax = 0;
    if (zmax > 1) zmax = 1;
    D3D11_VIEWPORT vp = {ox - fabsf(sx), oy - fabsf(sy), 2 * fabsf(sx), 2 * fabsf(sy), zmin, zmax};
    if (vp.Width < 1) { vp.TopLeftX = 0; vp.Width = (float)rt_w; }
    if (vp.Height < 1) { vp.TopLeftY = 0; vp.Height = (float)rt_h; }
    if (memcmp(&cur.vp, &vp, sizeof vp)) { ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp); cur.vp = vp; }
    params.fix[0] = sx < 0 ? -1.0f : 1.0f;
    params.fix[1] = sy < 0 ? 1.0f : -1.0f;
    params.fix[2] = minus_one ? 0.5f : 1.0f;
    params.fix[3] = minus_one ? 0.5f : 0.0f;
    params.frag[0] = lower_left ? 1.0f : 0.0f;
    params.frag[1] = (float)rt_h;
    /* Facing: the hardware takes the signed area in window coordinates (x, y_w = scale * ndc + offset)
     * as GL does, counter-clockwise positive, and WINDOW_ORIGIN.FLIP_Y (bit 4) swaps the front face.
     * D3D judges the winding as seen on the target, which after the fix-up above is the window
     * area itself for a lower-left origin and its mirror image for an upper-left one. */
    int flipped = (!lower_left) != ((regs[M_WINDOW_ORIGIN] >> 4) & 1);

    {
        D3D11_RECT r = {0, 0, 16384, 16384};
        if (regs[M_SCISSOR(0)] & 1) {
            u32 h = regs[M_SCISSOR(0) + 1], v = regs[M_SCISSOR(0) + 2];
            r = (D3D11_RECT){(LONG)(h & 0xFFFF), (LONG)(v & 0xFFFF), (LONG)(h >> 16), (LONG)(v >> 16)};
            if (lower_left) { LONG t = (LONG)rt_h - r.bottom; r.bottom = (LONG)rt_h - r.top; r.top = t; }
        }
        if (memcmp(&cur.scissor, &r, sizeof r)) { ID3D11DeviceContext_RSSetScissorRects(ctx, 1, &r); cur.scissor = r; }
    }

    /* rasterizer, depth/stencil, blending */
    D3D11_RASTERIZER_DESC rd;
    memset(&rd, 0, sizeof rd);
    rd.FillMode = regs[M_FILL_MODE] == 0x1B01 ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    if (regs[M_CULL_ENABLE] & 1) {
        u32 face = regs[M_CULL_FACE];
        rd.CullMode = face == 0x404 ? D3D11_CULL_FRONT : face == 0x405 ? D3D11_CULL_BACK : D3D11_CULL_NONE;
    }
    rd.FrontCounterClockwise = (regs[M_FRONT_FACE] == 0x901) != flipped;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE;
    if (gfx_debug() & 4) rd.CullMode = D3D11_CULL_NONE;
    ID3D11RasterizerState *rs = raster_state(&rd);
    if (cur.rs != rs) { ID3D11DeviceContext_RSSetState(ctx, rs); cur.rs = rs; }

    D3D11_DEPTH_STENCIL_DESC dd;
    memset(&dd, 0, sizeof dd);
    dd.DepthEnable = (regs[M_DEPTH_TEST] & 1) && dsv;
    dd.DepthWriteMask = regs[M_DEPTH_WRITE] & 1 ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = compare_func(regs[M_DEPTH_FUNC]);
    dd.StencilEnable = (regs[M_STENCIL_TEST] & 1) && dsv;
    dd.StencilReadMask = (UINT8)regs[M_STENCIL_FUNC_MASK];
    dd.StencilWriteMask = (UINT8)regs[M_STENCIL_MASK];
    dd.FrontFace = (D3D11_DEPTH_STENCILOP_DESC){stencil_op(regs[M_STENCIL_OP_FAIL]), stencil_op(regs[M_STENCIL_OP_ZFAIL]),
                                                stencil_op(regs[M_STENCIL_OP_ZPASS]), compare_func(regs[M_STENCIL_FUNC])};
    dd.BackFace = dd.FrontFace;
    if (regs[M_TWO_SIDED_STENCIL] & 1)
        dd.BackFace = (D3D11_DEPTH_STENCILOP_DESC){stencil_op(regs[M_BACK_STENCIL_OP_FAIL]), stencil_op(regs[M_BACK_STENCIL_OP_ZFAIL]),
                                                   stencil_op(regs[M_BACK_STENCIL_OP_ZPASS]), compare_func(regs[M_BACK_STENCIL_FUNC])};
    if (gfx_debug() & 2) { dd.DepthEnable = FALSE; dd.StencilEnable = FALSE; }
    ID3D11DepthStencilState *ds = depth_state(&dd);
    if (cur.ds != ds || cur.stencil_ref != (regs[M_STENCIL_REF] & 0xFF)) {
        ID3D11DeviceContext_OMSetDepthStencilState(ctx, ds, regs[M_STENCIL_REF] & 0xFF);
        cur.ds = ds;
        cur.stencil_ref = regs[M_STENCIL_REF] & 0xFF;
    }

    D3D11_BLEND_DESC bd;
    memset(&bd, 0, sizeof bd);
    bd.IndependentBlendEnable = TRUE;
    for (u32 i = 0; i < 8; i++) {
        D3D11_RENDER_TARGET_BLEND_DESC *t = &bd.RenderTarget[i];
        u32 mask = regs[M_CT_WRITE(i)];
        t->RenderTargetWriteMask = (UINT8)((mask & 1 ? 1 : 0) | (mask & 0x10 ? 2 : 0) | (mask & 0x100 ? 4 : 0) | (mask & 0x1000 ? 8 : 0));
        t->BlendEnable = regs[M_BLEND_ENABLE(i)] & 1;
        if (!t->BlendEnable) continue;
        u32 base = regs[M_BLEND_STATE_PER_TARGET] & 1 ? M_BLEND_PER_TARGET(i) + 1 : 0;
        u32 cop, csrc, cdst, aop, asrc, adst;
        if (base) {
            cop = regs[base]; csrc = regs[base + 1]; cdst = regs[base + 2]; aop = regs[base + 3]; asrc = regs[base + 4]; adst = regs[base + 5];
            if (!(regs[base - 1] & 1)) { aop = cop; asrc = csrc; adst = cdst; }   /* the target's own "separate alpha" */
        }
        else {
            cop = regs[M_BLEND_COLOR_OP]; csrc = regs[M_BLEND_COLOR_SRC]; cdst = regs[M_BLEND_COLOR_DST];
            aop = regs[M_BLEND_ALPHA_OP]; asrc = regs[M_BLEND_ALPHA_SRC]; adst = regs[M_BLEND_ALPHA_DST];
            if (!(regs[M_BLEND_SEPARATE_ALPHA] & 1)) { aop = cop; asrc = csrc; adst = cdst; }
        }
        t->BlendOp = blend_op(cop); t->SrcBlend = blend_factor(csrc); t->DestBlend = blend_factor(cdst);
        t->BlendOpAlpha = blend_op(aop); t->SrcBlendAlpha = alpha_factor(blend_factor(asrc)); t->DestBlendAlpha = alpha_factor(blend_factor(adst));
    }
    float blend_const[4] = {f32_of(regs[M_BLEND_CONST(0)]), f32_of(regs[M_BLEND_CONST(1)]), f32_of(regs[M_BLEND_CONST(2)]), f32_of(regs[M_BLEND_CONST(3)])};
    for (u32 i = 0; i < 8; i++) {
        if (gfx_debug() & 1) bd.RenderTarget[i].RenderTargetWriteMask = 15;
        if (gfx_debug() & 8) bd.RenderTarget[i].BlendEnable = FALSE;
    }
    ID3D11BlendState *bs = blend_state(&bd);
    if (cur.bs != bs || memcmp(cur.blend_factor, blend_const, sizeof blend_const)) {
        ID3D11DeviceContext_OMSetBlendState(ctx, bs, blend_const, 0xFFFFFFFF);
        cur.bs = bs;
        memcpy(cur.blend_factor, blend_const, sizeof blend_const);
    }
    if (draw_logging)
        fprintf(stderr, "[draw] programs: region %#llx vs +%#x ps +%#x\n", region, regs[M_PIPELINE(1) + 1], regs[M_PIPELINE(5) + 1]);
    if (draw_logging)
        fprintf(stderr, "[draw] %s %s rt0 %#llx %ux%u fmt %#x (ct sel %#x) zt %d | vp %.0f,%.0f %.0fx%.0f fix %.2f %.2f %.2f %.2f | "
                "%s %u (first %u) topo %u | mask %#x blend %d | depth %d/%d func %#x | cull %d (en %#x face %#x front %#x origin %#x scale %g %g)\n",
                vs->name, ps ? ps->name : "(no ps)", gpu_va_to_guest((u64)regs[M_CT(sel >> 4 & 7)] << 32 | regs[M_CT(sel >> 4 & 7) + 1]), rt_w, rt_h, regs[M_CT(sel >> 4 & 7) + 4], sel, dsv != NULL,
                vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, params.fix[0], params.fix[1], params.fix[2], params.fix[3],
                indexed ? "indexed" : "arrays", indexed ? regs[M_DRAW_INDEX_COUNT] : regs[M_VERTEX_ARRAY_COUNT],
                indexed ? regs[M_INDEX_BUFFER + 5] : regs[M_VERTEX_ARRAY_START], begin & 0xFFFF, regs[M_CT_WRITE(0)],
                bd.RenderTarget[0].BlendEnable, dd.DepthEnable, regs[M_DEPTH_WRITE] & 1, regs[M_DEPTH_FUNC], rd.CullMode,
                regs[M_CULL_ENABLE], regs[M_CULL_FACE], regs[M_FRONT_FACE], regs[M_WINDOW_ORIGIN],
                f32_of(regs[M_VIEWPORT_SCALE(0)]), f32_of(regs[M_VIEWPORT_SCALE(0) + 1]));
    if (draw_logging) {
        const D3D11_RENDER_TARGET_BLEND_DESC *t = &bd.RenderTarget[0];
        u32 sh = regs[M_SCISSOR(0) + 1], sv = regs[M_SCISSOR(0) + 2];
        fprintf(stderr, "[draw]   scissor %s x %u-%u y %u-%u | stencil %d func %#x ref %#x mask %#x/%#x ops %#x %#x %#x | "
                "two-sided %u: back func %#x ref %#x mask %#x/%#x ops %#x %#x %#x | "
                "blend src %d dst %d op %d, alpha src %d dst %d op %d\n",
                regs[M_SCISSOR(0)] & 1 ? "on" : "off", sh & 0xFFFF, sh >> 16, sv & 0xFFFF, sv >> 16, dd.StencilEnable, regs[M_STENCIL_FUNC],
                regs[M_STENCIL_REF] & 0xFF, regs[M_STENCIL_FUNC_MASK] & 0xFF, regs[M_STENCIL_MASK] & 0xFF, regs[M_STENCIL_OP_FAIL],
                regs[M_STENCIL_OP_ZFAIL], regs[M_STENCIL_OP_ZPASS], regs[M_TWO_SIDED_STENCIL] & 1, regs[M_BACK_STENCIL_FUNC],
                regs[M_BACK_STENCIL_REF] & 0xFF, regs[M_BACK_STENCIL_FUNC_MASK] & 0xFF, regs[M_BACK_STENCIL_MASK] & 0xFF,
                regs[M_BACK_STENCIL_OP_FAIL], regs[M_BACK_STENCIL_OP_ZFAIL], regs[M_BACK_STENCIL_OP_ZPASS], t->SrcBlend, t->DestBlend, t->BlendOp,
                t->SrcBlendAlpha, t->DestBlendAlpha, t->BlendOpAlpha);
    }

    /* vertex streams: each attribute the shader reads gets its stream's data at t18 + n */
    ID3D11ShaderResourceView *vsrv[32] = {0}, *stream_srv[32] = {0};
    static u8 *stream_data;
    static u32 stream_cap;
    for (u32 n = 0; n < 32; n++) {
        if (!(vs->info.inputs >> n & 1)) continue;
        u32 fmt = regs[M_VERTEX_ATTRIBUTE(n)], j = fmt & 0x1F;
        u32 st = regs[M_VERTEX_STREAM(j)];
        u64 start = (u64)regs[M_VERTEX_STREAM(j) + 1] << 32 | regs[M_VERTEX_STREAM(j) + 2];
        u64 limit = (u64)regs[M_VERTEX_STREAM_LIMIT(j)] << 32 | regs[M_VERTEX_STREAM_LIMIT(j) + 1];
        params.attr[n][0] = fmt;
        params.attr[n][1] = st & 0xFFF;
        params.attr[n][2] = regs[M_VERTEX_STREAM_INSTANCED(j)] & 1 ? (regs[M_VERTEX_STREAM(j) + 3] ? regs[M_VERTEX_STREAM(j) + 3] : 1) : 0;
        params.attr[n][3] = (st >> 12) & 1 && start;
        if (!params.attr[n][3]) continue;
        u64 size = limit >= start ? limit - start + 1 : 0;
        if (size > (64u << 20)) size = 64u << 20;
        if (stream_srv[j]) { vsrv[n] = stream_srv[j]; continue; }   /* another attribute of the same stream */
        /* A stream inside data the GPU keeps (compute results) is read from there; one only
         * partly inside gets the guest's data around the GPU's. */
        Resident *res = (start & 3) == 0 ? resident_covering(start, start + size) : NULL;
        if (res) {
            vsrv[n] = stream_srv[j] = resident_view(res, start, (u32)size);
            if (draw_logging)
                fprintf(stderr, "[draw]   attr%u: stream %u at %#llx (+%#llx) from resident %#llx + %#x\n", n, j, start, size, res->va, res->size);
            if (vsrv[n]) continue;
        }
        Resident *part = resident_overlapping(start, (u32)size);
        if (!part && !draw_logging) { vsrv[n] = stream_srv[j] = upload_gpu(SHADER_VERTEX, 18 + n, start, (u32)size); continue; }
        if (size > stream_cap) { stream_data = realloc(stream_data, size + 16); stream_cap = (u32)size; }
        read_gpu(start, (u32)size, stream_data);
        vsrv[n] = stream_srv[j] = part ? upload_composed(SHADER_VERTEX, 18 + n, start, stream_data, (u32)size) : NULL;
        if (part && draw_logging)
            fprintf(stderr, "[draw]   attr%u: stream %u at %#llx (+%#llx) partly in resident %#llx + %#x\n", n, j, start, size, part->va, part->size);
        if (!vsrv[n]) vsrv[n] = stream_srv[j] = upload_raw(SHADER_VERTEX, 18 + n, stream_data, (u32)size);
        if (draw_logging) {
            u32 off = (fmt >> 7) & 0x3FFF, stride = st & 0xFFF;
            float f[6] = {0};
            for (u32 v = 0; v < 3; v++)
                if (off + stride * v + 8 <= size) memcpy(&f[2 * v], stream_data + off + stride * v, 8);
            fprintf(stderr, "[draw]   attr%u: fmt %#x (stream %u, offset %u, size %#x, type %u) stride %u, %llu bytes at %#llx: v0 %g %g v1 %g %g v2 %g %g\n",
                    n, fmt, j, off, (fmt >> 21) & 0x3F, (fmt >> 27) & 7, stride, size, start,
                    f[0], f[1], f[2], f[3], f[4], f[5]);
        }
    }
    set_srvs(SHADER_VERTEX, 18, 32, vsrv);
    params.base[0] = regs[M_GLOBAL_BASE_VERTEX];
    params.base[1] = regs[M_GLOBAL_BASE_INSTANCE] + instance_counter;

    bind_resources(regs, cb, SHADER_VERTEX, vs, &params);
    if (ps) bind_resources(regs, cb, SHADER_PIXEL, ps, &params);

    /* per-draw parameters */
    if (!params_buffer) {
        D3D11_BUFFER_DESC pd = {(sizeof(DrawParams) + 15) & ~15u, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
        ID3D11Device_CreateBuffer(dev, &pd, NULL, &params_buffer);
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)params_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &params, sizeof params);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)params_buffer, 0);
    }
    if (!cur.fixed_set) {
        ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &params_buffer);
        ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &params_buffer);
        ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
        cur.fixed_set = 1;
    }
    if (cur.vs != vs->obj) { ID3D11DeviceContext_VSSetShader(ctx, (ID3D11VertexShader *)vs->obj, NULL, 0); cur.vs = vs->obj; }
    void *ps_obj = ps ? ps->obj : NULL;
    if (cur.ps != ps_obj) { ID3D11DeviceContext_PSSetShader(ctx, (ID3D11PixelShader *)ps_obj, NULL, 0); cur.ps = ps_obj; }
    /* debug: SSBU_GFX_PS_FILE (HLSL, entry main) replaces the pixel shader named by SSBU_GFX_PS_FOR */
    static const char *ps_for = (const char *)1;
    static ID3D11PixelShader *ps_override;
    if (ps_for == (const char *)1) {
        ps_for = getenv("SSBU_GFX_PS_FOR");
        FILE *f = ps_for && getenv("SSBU_GFX_PS_FILE") ? fopen(getenv("SSBU_GFX_PS_FILE"), "rb") : NULL;
        if (f) {
            static char src[65536];
            size_t n = fread(src, 1, sizeof src - 1, f);
            fclose(f);
            src[n] = 0;
            ID3DBlob *b = compile_hlsl(src, "main", "ps_5_0");
            if (b) {
                ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &ps_override);
                ID3D10Blob_Release(b);
            }
        }
    }
    if (ps_override && ps && strstr(ps->name, ps_for)) { ID3D11DeviceContext_PSSetShader(ctx, ps_override, NULL, 0); cur.ps = ps_override; }
    static const char *magenta_only = (const char *)1;
    if (magenta_only == (const char *)1) magenta_only = getenv("SSBU_GFX_MAGENTA");   /* only draws with this pixel shader */
    if ((gfx_debug() & 32) && ps && (!magenta_only || strstr(ps->name, magenta_only))) {   /* debug: every pixel magenta, to see what geometry lands where */
        static ID3D11PixelShader *magenta;
        if (!magenta) {
            static const char src[] = "float4 main() : SV_Target { return float4(1, 0, 1, 1); }";
            ID3DBlob *b = compile_hlsl(src, "main", "ps_5_0");
            ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &magenta);
            ID3D10Blob_Release(b);
        }
        ID3D11DeviceContext_PSSetShader(ctx, magenta, NULL, 0);
        cur.ps = magenta;
    }
    if (cur.topo != topo) { ID3D11DeviceContext_IASetPrimitiveTopology(ctx, topo); cur.topo = topo; }

    u32 dbg_first = 0, dbg_count = 0;   /* the actual Draw/DrawIndexed arguments (QUADS are expanded) */

    if (indexed) {
        u32 fmt = regs[M_INDEX_BUFFER + 4], isz = fmt == 0 ? 1 : fmt == 1 ? 2 : 4, first = regs[M_INDEX_BUFFER + 5];
        u32 n = regs[M_DRAW_INDEX_COUNT];
        u64 va = ((u64)regs[M_INDEX_BUFFER] << 32 | regs[M_INDEX_BUFFER + 1]) + (u64)first * isz;
        if (!n || n > (16u << 20)) { gfx_stat_skipped++; leave(); return; }
        int quads = (begin & 0xFFFF) == 7;
        u32 out_n = quads ? (n / 4) * 6 : n;   /* QUADS: each 4 indices -> 2 triangles (6 indices) */
        /* indices go one after another into a ring (no renaming per draw): the draw starts at
         * its place in it, a new pass round discards */
        u32 bytes = out_n * (isz == 1 ? 2 : isz);
        if (bytes > index_cap) {
            if (index_buffer) bury(index_buffer);
            index_cap = 4u << 20;
            while (index_cap < bytes) index_cap *= 2;
            D3D11_BUFFER_DESC ib = {index_cap, D3D11_USAGE_DYNAMIC, D3D11_BIND_INDEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
            ID3D11Device_CreateBuffer(dev, &ib, NULL, &index_buffer);
            index_pos = index_cap;   /* (discard first) */
        }
        u32 unit = isz == 4 ? 4 : 2;
        index_pos = (index_pos + 3) & ~3u;
        D3D11_MAP how = D3D11_MAP_WRITE_NO_OVERWRITE;
        if (index_pos + bytes > index_cap) { index_pos = 0; how = D3D11_MAP_WRITE_DISCARD; }
        index_first = index_pos / unit;
        gfx_stat_upload_bytes += bytes;
        if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)index_buffer, 0, how, 0, &m))) {
            m.pData = (u8 *)m.pData + index_pos;
            index_pos += bytes;
            if (quads) {   /* read n indices, emit two triangles per quad */
                static u32 tmp32[1 << 15];
                if (n > (u32)(sizeof tmp32 / sizeof tmp32[0])) n = sizeof tmp32 / sizeof tmp32[0];
                if (isz == 4) read_gpu(va, n * 4, tmp32);
                else { static u16 t16[1 << 15]; if (isz == 1) { static u8 t8[1 << 15]; read_gpu(va, n, t8); for (u32 i = 0; i < n; i++) t16[i] = t8[i]; } else read_gpu(va, n * 2, t16); for (u32 i = 0; i < n; i++) tmp32[i] = t16[i]; }
                u32 o = 0;
                for (u32 q = 0; q + 3 < n; q += 4) {
                    u32 a = tmp32[q], b = tmp32[q + 1], c = tmp32[q + 2], d = tmp32[q + 3];
                    u32 tri[6] = {a, b, c, a, c, d};
                    for (u32 k = 0; k < 6; k++) { if (unit == 4) ((u32 *)m.pData)[o] = tri[k]; else ((u16 *)m.pData)[o] = (u16)tri[k]; o++; }
                }
                out_n = o;
            } else if (isz == 1) {   /* 8-bit indices widen to 16 */
                static u8 tmp8[1 << 16];
                u32 left = n, done = 0;
                while (left) {
                    u32 k = left > sizeof tmp8 ? (u32)sizeof tmp8 : left;
                    read_gpu(va + done, k, tmp8);
                    for (u32 i = 0; i < k; i++) ((u16 *)m.pData)[done + i] = tmp8[i];
                    done += k; left -= k;
                }
            } else read_gpu(va, n * isz, m.pData);
            if (draw_logging) {
                u32 ix[6] = {0};
                for (u32 i = 0; i < 6 && i < out_n; i++) ix[i] = isz == 4 ? ((u32 *)m.pData)[i] : ((u16 *)m.pData)[i];
                fprintf(stderr, "[draw]   indices (%u-byte): %u %u %u %u %u %u\n", isz, ix[0], ix[1], ix[2], ix[3], ix[4], ix[5]);
            }
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)index_buffer, 0);
        }
        DXGI_FORMAT ifmt = isz == 4 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
        if (cur.ib != index_buffer || cur.ib_format != ifmt) {
            ID3D11DeviceContext_IASetIndexBuffer(ctx, index_buffer, ifmt, 0);
            cur.ib = index_buffer;
            cur.ib_format = ifmt;
        }
        dbg_first = index_first;
        dbg_count = quads ? out_n : regs[M_DRAW_INDEX_COUNT];
    }
    /* Two-sided stencil whose back faces have their own reference or masks: D3D has one set for
     * both, so the front faces and the back faces are drawn apart, each with its own. (The UI
     * draws its masks unculled and relies on one face's write mask.) Which face is which differs
     * from culling: WINDOW_ORIGIN.FLIP_Y swaps the front face for culling (see the facing above)
     * but not for the stencil state - with it set, the hardware's "back" state is what D3D's
     * front faces get (seen on the main menu's tiles, whose fills need it). */
    u32 back_ref = regs[M_BACK_STENCIL_REF] & 0xFF, back_write = regs[M_BACK_STENCIL_MASK] & 0xFF, back_read = regs[M_BACK_STENCIL_FUNC_MASK] & 0xFF;
    int split = dd.StencilEnable && (regs[M_TWO_SIDED_STENCIL] & 1) &&
                (back_ref != (regs[M_STENCIL_REF] & 0xFF) || back_write != dd.StencilWriteMask || back_read != dd.StencilReadMask);
    static int force_swap = -2;   /* debug: SSBU_STENCIL_SWAP=0/1 overrides */
    if (force_swap == -2) force_swap = getenv("SSBU_STENCIL_SWAP") ? atoi(getenv("SSBU_STENCIL_SWAP")) != 0 : -1;
    int swap_faces = force_swap >= 0 ? force_swap : (int)((regs[M_WINDOW_ORIGIN] >> 4) & 1);
    for (int pass = 0; pass < (split ? 2 : 1); pass++) {
        if (split) {
            int back_faces = pass == 1, back = back_faces != swap_faces;
            if (rd.CullMode == (back_faces ? D3D11_CULL_BACK : D3D11_CULL_FRONT)) continue;   /* those faces are culled anyway */
            D3D11_RASTERIZER_DESC r = rd;
            D3D11_DEPTH_STENCIL_DESC d = dd;
            r.CullMode = back_faces ? D3D11_CULL_FRONT : D3D11_CULL_BACK;   /* just this pass's faces */
            if (back) { d.StencilReadMask = (UINT8)back_read; d.StencilWriteMask = (UINT8)back_write; }
            cur.rs = raster_state(&r);
            cur.ds = depth_state(&d);
            cur.stencil_ref = back ? back_ref : regs[M_STENCIL_REF] & 0xFF;
            ID3D11DeviceContext_RSSetState(ctx, cur.rs);
            ID3D11DeviceContext_OMSetDepthStencilState(ctx, cur.ds, cur.stencil_ref);
        }
        if (indexed) ID3D11DeviceContext_DrawIndexed(ctx, dbg_count, dbg_first, (INT)regs[M_GLOBAL_BASE_VERTEX]);
        else if ((begin & 0xFFFF) == 7) {
            /* QUADS without an index buffer: expand the vertex range 4 -> 6 (two triangles) */
            u32 base = regs[M_VERTEX_ARRAY_START], n = regs[M_VERTEX_ARRAY_COUNT], o = 0;
            static u32 qbuf_cap; static ID3D11Buffer *qbuf;
            u32 need = (n / 4) * 6 * 4;
            if (need > qbuf_cap) { if (qbuf) bury(qbuf); qbuf_cap = need < 4096 ? 4096 : need; D3D11_BUFFER_DESC ib = {qbuf_cap, D3D11_USAGE_DYNAMIC, D3D11_BIND_INDEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0}; ID3D11Device_CreateBuffer(dev, &ib, NULL, &qbuf); }
            D3D11_MAPPED_SUBRESOURCE qm;
            if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)qbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &qm))) {
                u32 *dst = (u32 *)qm.pData;
                for (u32 q = 0; q + 3 < n; q += 4) { u32 a = q, b = q + 1, c = q + 2, d = q + 3; dst[o++] = a; dst[o++] = b; dst[o++] = c; dst[o++] = a; dst[o++] = c; dst[o++] = d; }
                ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)qbuf, 0);
                ID3D11DeviceContext_IASetIndexBuffer(ctx, qbuf, DXGI_FORMAT_R32_UINT, 0);
                cur.ib = NULL; cur.ib_format = 0;
                ID3D11DeviceContext_DrawIndexed(ctx, o, 0, (INT)base);
            }
        }
        else ID3D11DeviceContext_Draw(ctx, regs[M_VERTEX_ARRAY_COUNT], regs[M_VERTEX_ARRAY_START]);
    }
    gfx_stat_drawn++;
    leave();
}

static void save_bmp(ID3D11Texture2D *src, const char *path);

/* With SSBU_CAPTURE_SURFACES as well: every colour render target, drawn into an RGBA8
 * image first (so any format can be saved), as surf_<frame>_<guest address>.bmp. */
typedef struct Texture Texture;
static void capture_textures(const char *dir, int frame);

/* A surface drawn into an RGBA8 image (so any format can be saved) with the given blit pixel
 * shader, saved as a BMP. Leaves the pipeline state changed. */
static void save_surface_bmp(Surface *s, ID3D11PixelShader *ps, const char *path) {
    D3D11_TEXTURE2D_DESC td = {s->width, s->height, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_RENDER_TARGET, 0, 0};
    ID3D11Texture2D *tmp;
    ID3D11RenderTargetView *rtv;
    if (!ps || FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &tmp))) return;
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)tmp, NULL, &rtv);
    D3D11_VIEWPORT vp = {0, 0, (float)s->width, (float)s->height, 0, 1};
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    D3D11_RASTERIZER_DESC rd = {D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0, 0, TRUE, FALSE, FALSE, FALSE};
    ID3D11DeviceContext_RSSetState(ctx, raster_state(&rd));
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_VSSetShader(ctx, blit_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11ShaderResourceView *raw = s->depth ? s->srv : surface_srv(s, 0);   /* the bytes as stored */
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &raw);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &linear_sampler);
    ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, NULL, 0);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    ID3D11ShaderResourceView *none = NULL;
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &none);
    save_bmp(tmp, path);
    ID3D11RenderTargetView_Release(rtv);
    ID3D11Texture2D_Release(tmp);
}

static void capture_surfaces(const char *dir, int frame) {
    capture_textures(dir, frame);
    for (u32 i = 0; i < nsurfaces; i++) {
        Surface *s = &surfaces[i];
        if (!s->tex || s->depth || !s->srv) continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/surf_%03d_%llx_fmt%x.bmp", dir, frame, s->guest, s->format);
        save_surface_bmp(s, blit_ps, path);
    }
}

/* F4: render targets that draws sample are saved as they are at that moment (a frame's
 * intermediate images: effects buffers, bloom levels), colour and alpha, as
 * in_<n>_<guest>_fmt<f>_<w>x<h>.bmp; the draw log says which draw sampled each. They are
 * saved just before the next piece of GPU work, so the sampling draw is undisturbed. */
static char sampled_dir[160];
static Surface *sampled_pending[16];
static int sampled_seq_of[16];
static u32 nsampled_pending;
static struct { u64 guest, used; } sampled_saved[256];
static u32 nsampled_saved;
static int sampled_seq;

static void note_sampled(Surface *s) {
    if (!sampled_dir[0] || nsampled_saved >= 256) return;
    for (u32 i = 0; i < nsampled_saved; i++)
        if (sampled_saved[i].guest == s->guest && sampled_saved[i].used == s->used) return;   /* (unchanged since saved) */
    for (u32 i = 0; i < nsampled_pending; i++) if (sampled_pending[i] == s) return;
    if (nsampled_pending == 16) return;
    sampled_saved[nsampled_saved].guest = s->guest;
    sampled_saved[nsampled_saved++].used = s->used;
    sampled_seq_of[nsampled_pending] = sampled_seq;
    sampled_pending[nsampled_pending++] = s;
    fprintf(stderr, "[draw]   (samples render target %#llx %ux%u fmt %#x: saved as in_%03d)\n", s->guest, s->width, s->height,
            s->format, sampled_seq++);
}

static void save_sampled(void) {
    if (nsampled_pending) forget_draw_state();
    for (u32 i = 0; i < nsampled_pending; i++) {
        Surface *s = sampled_pending[i];
        if (!s->tex || !s->srv) continue;
        char path[300];
        snprintf(path, sizeof path, "%s/in_%03d_%llx_fmt%x_%ux%u.bmp", sampled_dir, sampled_seq_of[i], s->guest, s->format, s->width, s->height);
        save_surface_bmp(s, blit_ps, path);
        if (!s->depth) {
            snprintf(path, sizeof path, "%s/in_%03d_%llx_fmt%x_%ux%u_alpha.bmp", sampled_dir, sampled_seq_of[i], s->guest, s->format, s->width, s->height);
            save_surface_bmp(s, blit_alpha_ps, path);
        }
    }
    nsampled_pending = 0;
}

/* SSBU_CAPTURE=<dir>: save the window's contents there every few seconds (frame_N.bmp). */
static void save_bmp(ID3D11Texture2D *back, const char *path) {
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D_GetDesc(back, &td);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
    ID3D11Texture2D *staging;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &staging))) return;
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)staging, (ID3D11Resource *)back);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m))) {
        FILE *f = fopen(path, "wb");
        if (f) {
            u32 w = td.Width, h = td.Height, row = w * 3, pad = (4 - row % 4) % 4, data = (row + pad) * h;
            u8 hdr[54] = {'B', 'M'};
            *(u32 *)(hdr + 2) = 54 + data; *(u32 *)(hdr + 10) = 54; *(u32 *)(hdr + 14) = 40;
            *(i32 *)(hdr + 18) = (i32)w; *(i32 *)(hdr + 22) = -(i32)h; *(u16 *)(hdr + 26) = 1; *(u16 *)(hdr + 28) = 24;
            *(u32 *)(hdr + 34) = data;
            fwrite(hdr, 1, 54, f);
            u8 *line = malloc(row + pad + 4);
            for (u32 y = 0; y < h; y++) {
                const u8 *p = (const u8 *)m.pData + (size_t)y * m.RowPitch;
                for (u32 x = 0; x < w; x++) { line[x * 3] = p[x * 4 + 2]; line[x * 3 + 1] = p[x * 4 + 1]; line[x * 3 + 2] = p[x * 4]; }
                memset(line + row, 0, pad);
                fwrite(line, 1, row + pad, f);
            }
            free(line);
            fclose(f);
        }
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 0);
    }
    ID3D11Texture2D_Release(staging);
}

static void capture(ID3D11Texture2D *back) {
    static const char *dir;
    static int checked, count;
    static u64 last;
    static int most = 40;   /* SSBU_CAPTURE_MAX: how many (at 6 MB each) */
    static u64 every = 3000000000ull;   /* SSBU_CAPTURE_EVERY: seconds apart */
    if (!checked++) {
        dir = getenv("SSBU_CAPTURE");
        if (getenv("SSBU_CAPTURE_MAX")) most = atoi(getenv("SSBU_CAPTURE_MAX"));
        if (getenv("SSBU_CAPTURE_EVERY")) every = (u64)(atof(getenv("SSBU_CAPTURE_EVERY")) * 1e9);
    }
    if (!dir || count >= most || rt_now_ns() - last < every) return;
    last = rt_now_ns();
    char path[1024];
    snprintf(path, sizeof path, "%s/frame_%03d.bmp", dir, count);
    save_bmp(back, path);
    if (getenv("SSBU_CAPTURE_SURFACES")) capture_surfaces(dir, count);
    count++;
}

/* 2D engine: source and destination surfaces (format, layout, block size, depth, layer,
 * pitch, width, height, address) at methods 0x80 and 0x8C; the blit rectangle and the
 * 32.32 fixed-point source position and step at 0x22C-0x237. */
/* ---- compute --------------------------------------------------------------------- */
/* A launch runs its program over the grid. Global memory is the storage buffers described in
 * constant buffer 0, uploaded as writable buffers; afterwards whatever the program changed is
 * written back to guest memory - word by word against what was uploaded, so that nothing the
 * CPU wrote meanwhile elsewhere in those buffers is overwritten. (The game skins its fighters
 * this way: the draws that follow read the results as vertex data.) */
u64 gfx_stat_dispatched;
static u64 first_dispatch_ns;
typedef struct RwBuf { ID3D11Buffer *buf, *staging; ID3D11UnorderedAccessView *uav; u32 cap; u8 *before; } RwBuf;
static RwBuf rwbufs[16];

static RwBuf *rw_upload(u32 k, const u8 *data, u32 size) {
    RwBuf *b = &rwbufs[k];
    u32 need = (size + 15) & ~15u;
    gfx_stat_upload_bytes += size;
    if (need > b->cap) {
        if (b->uav) ID3D11UnorderedAccessView_Release(b->uav);
        if (b->buf) ID3D11Buffer_Release(b->buf);
        if (b->staging) ID3D11Buffer_Release(b->staging);
        b->uav = NULL; b->buf = b->staging = NULL; b->cap = 0;
        u32 cap = 4096;
        while (cap < need) cap *= 2;
        D3D11_BUFFER_DESC bd = {cap, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, 0,
                                D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
        D3D11_BUFFER_DESC sd = {cap, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
        if (FAILED(ID3D11Device_CreateBuffer(dev, &bd, NULL, &b->buf)) || FAILED(ID3D11Device_CreateBuffer(dev, &sd, NULL, &b->staging)))
            return NULL;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud;
        memset(&ud, 0, sizeof ud);
        ud.Format = DXGI_FORMAT_R32_TYPELESS;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = cap / 4;
        ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        if (FAILED(ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)b->buf, &ud, &b->uav))) return NULL;
        b->before = realloc(b->before, cap);
        b->cap = cap;
    }
    memset(b->before, 0, need);
    memcpy(b->before, data, size);
    D3D11_BOX box = {0, 0, 0, need, 1, 1};
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)b->buf, 0, &box, b->before, 0, 0);
    return b;
}

/* Storage buffers a compute program only reads (the fighters' bind pose and skin weights,
 * the whole model's for every part skinned): kept on the GPU for as long as guest memory under
 * them goes unwritten (the write watch says), instead of uploaded in full for every dispatch. */
typedef struct InputBuf { u64 va, guest; u32 size; ID3D11Buffer *buf; ID3D11UnorderedAccessView *uav; u64 used; } InputBuf;
static InputBuf inputs[128];
static u64 input_clock;

static ID3D11UnorderedAccessView *input_uav(u64 va, u32 size) {
    u64 g = gpu_va_to_guest(va);
    if (!g || !size || !watched(g, size) || gpu_va_to_guest(va + size - 1) != g + size - 1) return NULL;   /* one watched piece only */
    InputBuf *b = NULL, *lru = &inputs[0];
    for (u32 i = 0; i < 128 && !b; i++) {
        if (inputs[i].buf && inputs[i].va == va && inputs[i].size == size && inputs[i].guest == g) b = &inputs[i];
        else if (inputs[i].used < lru->used) lru = &inputs[i];
    }
    u32 cap = (size + 15) & ~15u;
    if (b) {
        void *page;
        ULONG_PTR count = 1;
        ULONG granularity;
        if (GetWriteWatch(0, (void *)g, size, &page, &count, &granularity) == 0 && count) {   /* written since: take it again */
            D3D11_BOX box = {0, 0, 0, size, 1, 1};
            ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)b->buf, 0, &box, P(void, g), 0, 0);
            gfx_stat_upload_bytes += size;
            ResetWriteWatch((void *)g, size);
        }
        b->used = ++input_clock;
        return b->uav;
    }
    b = lru;
    bury(b->uav);   /* (may be bound still) */
    bury(b->buf);
    memset(b, 0, sizeof *b);
    D3D11_BUFFER_DESC bd = {cap, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
    u8 *init = calloc(1, cap);
    read_gpu(va, size, init);
    D3D11_SUBRESOURCE_DATA sd = {init, 0, 0};
    HRESULT hr = ID3D11Device_CreateBuffer(dev, &bd, &sd, &b->buf);
    free(init);
    if (FAILED(hr)) { b->buf = NULL; return NULL; }
    gfx_stat_upload_bytes += size;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud;
    memset(&ud, 0, sizeof ud);
    ud.Format = DXGI_FORMAT_R32_TYPELESS;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = cap / 4;
    ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    if (FAILED(ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)b->buf, &ud, &b->uav))) { ID3D11Buffer_Release(b->buf); b->buf = NULL; return NULL; }
    b->va = va; b->guest = g; b->size = size; b->used = ++input_clock;
    ResetWriteWatch((void *)g, size);
    return b->uav;
}

void gfx_dispatch(const u32 *regs, const u32 *q) {
    if (!enter()) return;
    kept_flush();
    if (draw_logging) save_sampled();
    forget_draw_state();   /* (binding outputs for compute drops their bindings as inputs) */
    rings_start_draw();
    u64 region = (u64)regs[M_PROGRAM_REGION] << 32 | regs[M_PROGRAM_REGION + 1];
    u64 g = gpu_va_to_guest(region + q[8]);
    u32 local[3] = {q[18] >> 16, q[19] & 0xFFFF, q[19] >> 16};
    u32 grid[3] = {q[12] & 0x7FFFFFFF, q[13] & 0xFFFF, q[13] >> 16};
    u32 shared = q[17] & 0x3FFFF;
    CbBinding cb[18];
    memset(cb, 0, sizeof cb);
    for (u32 i = 0; i < 8; i++)
        if (q[20] >> i & 1) { cb[i].address = (u64)(q[30 + 2 * i] & 0xFF) << 32 | q[29 + 2 * i]; cb[i].size = q[30 + 2 * i] >> 15; }
    if (!g || !grid[0] || !grid[1] || !grid[2] || !local[0]) { leave(); return; }
    Shader *cs = get_shader_ex(g, SHADER_COMPUTE, 0, cb, local, shared);
    if (!cs) { gfx_skip_shader++; leave(); return; }

    /* constant buffers */
    ID3D11ShaderResourceView *srv[18] = {0};
    static u8 data[0x10000];
    for (u32 b = 0; b < 18; b++) {
        if (!(cs->info.cbuf_mask >> b & 1)) continue;
        u32 size = cb[b].size > 0x10000 ? 0x10000 : cb[b].size;
        if (size > cs->info.cbuf_extent[b]) size = cs->info.cbuf_extent[b];
        if (cb[b].address && size) read_gpu(cb[b].address, size, data);
        else { size = 16; memset(data, 0, 16); }
        srv[b] = upload_raw(SHADER_COMPUTE, b, data, size);
    }
    ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 18, srv);

    /* storage buffers */
    DrawParams params;
    memset(&params, 0, sizeof params);
    if (!params_buffer) {
        D3D11_BUFFER_DESC pd = {(sizeof(DrawParams) + 15) & ~15u, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
        ID3D11Device_CreateBuffer(dev, &pd, NULL, &params_buffer);
    }
    ID3D11UnorderedAccessView *uav[16] = {0};
    RwBuf *bound[16] = {0};
    u64 va_of[16] = {0};
    u32 size_of[16] = {0};
    static u8 *gdata;
    static u32 gcap;
    static int no_resident = -1;   /* debug: always read results back to guest memory */
    if (no_resident < 0) no_resident = getenv("SSBU_NO_RESIDENT") != NULL;
    int learned = cs->learn_runs >= 8 && !no_resident;   /* until then, what it writes is read back and noted */
    static int log_left = -1;
    if (log_left < 0) log_left = getenv("SSBU_LOG_DISPATCH") ? atoi(getenv("SSBU_LOG_DISPATCH")) : 0;
    int logging = learned && log_left > 0 && log_left--;
    for (u32 k = 0; k < cs->info.sb_count; k++) {
        u32 desc[4] = {0};
        if (cb[0].address) read_gpu(cb[0].address + cs->info.sb_offset[k], 16, desc);
        u64 va = (u64)desc[1] << 32 | desc[0];
        u32 size = desc[2] > (64u << 20) ? 64u << 20 : desc[2];
        if (logging)
            fprintf(stderr, "[cs] %s grid %ux%ux%u: buffer %u (cb0+%#x) %#llx + %#x%s\n", cs->name, grid[0], grid[1], grid[2], k,
                    cs->info.sb_offset[k], va, desc[2], cs->written_mask >> k & 1 ? " written" : "");
        params.sb[0][k][0] = desc[0];
        params.sb[0][k][1] = desc[1];
        params.sb[0][k][2] = size;
        if (!va || !size) continue;
        Resident *over = resident_overlapping(va, size);
        if (learned) {
            /* what it writes stays on the GPU; what it only reads is the GPU's data where there is
             * some, the guest's elsewhere */
            Resident *res = cs->written_mask >> k & 1 ? resident_acquire(va, size) : over ? resident_covering(va, va + size) : NULL;
            ResidentView *v = res ? resident_viewx(res, va, size) : NULL;
            if (v) { uav[k] = v->uav; continue; }
            if (!(cs->written_mask >> k & 1) && !over && (uav[k] = input_uav(va, size))) continue;   /* only read: cached */
        } else if (over)
            gfx_resident_flush(va, size);   /* learning diffs it all against guest memory, which needs the GPU's data first */
        if (size > gcap) { gdata = realloc(gdata, size); gcap = size; }
        read_gpu(va, size, gdata);
        bound[k] = rw_upload(k, gdata, size);
        if (bound[k] && learned && over) resident_patch(bound[k]->buf, va, size);
        if (bound[k]) { uav[k] = bound[k]->uav; va_of[k] = va; size_of[k] = size; }
    }
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 16, uav, NULL);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)params_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &params, sizeof params);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)params_buffer, 0);
    }
    ID3D11DeviceContext_CSSetConstantBuffers(ctx, 0, 1, &params_buffer);
    ID3D11DeviceContext_CSSetShader(ctx, (ID3D11ComputeShader *)cs->obj, NULL, 0);
    ID3D11DeviceContext_Dispatch(ctx, grid[0], grid[1], grid[2]);
    ID3D11UnorderedAccessView *none[16] = {0};
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 16, none, NULL);
    gfx_stat_dispatched++;
    if (!first_dispatch_ns) first_dispatch_ns = rt_now_ns();
    if (draw_logging)
        fprintf(stderr, "[draw] dispatch %s grid %ux%ux%u, buffers %#llx+%#x %#llx+%#x %#llx+%#x (written %#x)\n", cs->name, grid[0], grid[1],
                grid[2], (u64)params.sb[0][0][1] << 32 | params.sb[0][0][0], params.sb[0][0][2], (u64)params.sb[0][1][1] << 32 | params.sb[0][1][0],
                params.sb[0][1][2], (u64)params.sb[0][2][1] << 32 | params.sb[0][2][0], params.sb[0][2][2], cs->written_mask);

    /* while learning, what it wrote goes back to guest memory; the buffers written are noted */
    if (cs->info.stores && !learned) cs->learn_runs++;
    if (cs->info.stores && !learned)
        for (u32 k = 0; k < 16; k++) {
            RwBuf *b = bound[k];
            if (!b) continue;
            u32 n = (size_of[k] + 3) & ~3u;
            D3D11_BOX box = {0, 0, 0, n, 1, 1};
            ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)b->staging, 0, 0, 0, 0, (ID3D11Resource *)b->buf, 0, &box);
            gfx_stat_gpu_waits++;
            if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)b->staging, 0, D3D11_MAP_READ, 0, &m))) continue;
            const u32 *now = m.pData, *was = (const u32 *)b->before;
            int changed = 0;
            for (u32 i = 0; i < size_of[k] / 4; i++) {
                if (now[i] == was[i]) continue;
                changed = 1;
                u64 gg = gpu_va_to_guest(va_of[k] + 4ull * i);
                if (gg) st32(gg, now[i]);
            }
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)b->staging, 0);
            if (changed) {
                cs->written_mask |= 1u << k;
                gfx_resident_written(va_of[k], size_of[k]);   /* a region over it takes the new data too */
            }
        }
    leave();
}

/* A clear of only some colour channels (mask: R 1, G 2, B 4, A 8): a full-target triangle of the
 * clear colour, written through that channel mask. */
static void clear_channels(Surface *s, const float color[4], u32 mask) {
    static ID3D11PixelShader *ps;
    forget_draw_state();
    static ID3D11Buffer *cb;
    if (!ps) {
        ID3DBlob *b = compile_hlsl(BLIT_HLSL, "psclear", "ps_5_0");
        if (b) { ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &ps); ID3D10Blob_Release(b); }
        D3D11_BUFFER_DESC bd = {16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
        ID3D11Device_CreateBuffer(dev, &bd, NULL, &cb);
    }
    if (!ps || !cb || !s->rtv) return;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    memcpy(m.pData, color, 16);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)cb, 0);
    ID3D11ShaderResourceView *none[96] = {0};
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 96, none);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &s->rtv, NULL);
    D3D11_VIEWPORT vp = {0, 0, (float)s->width, (float)s->height, 0, 1};
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    D3D11_RECT all = {0, 0, 16384, 16384};
    ID3D11DeviceContext_RSSetScissorRects(ctx, 1, &all);
    D3D11_RASTERIZER_DESC rd = {D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0, 0, TRUE, TRUE, FALSE, FALSE};
    ID3D11DeviceContext_RSSetState(ctx, raster_state(&rd));
    D3D11_BLEND_DESC bd;
    memset(&bd, 0, sizeof bd);
    bd.RenderTarget[0].RenderTargetWriteMask = (UINT8)mask;
    ID3D11DeviceContext_OMSetBlendState(ctx, blend_state(&bd), NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, NULL, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_VSSetShader(ctx, blit_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 2, 1, &cb);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
}

u64 gfx_stat_blits;
static ID3D11SamplerState *point_sampler;

void gfx_blit2d(const u32 *regs) {
    if (!enter()) return;
    if (draw_logging) save_sampled();
    forget_draw_state();
    gfx_stat_blits++;
    u64 src_va = (u64)regs[0x94] << 32 | regs[0x95], dst_va = (u64)regs[0x88] << 32 | regs[0x89];
    u64 src_g = gpu_va_to_guest(src_va), dst_g = gpu_va_to_guest(dst_va);
    Surface *src = NULL;
    for (u32 i = 0; i < nsurfaces; i++)
        if (surfaces[i].tex && !surfaces[i].depth && surfaces[i].guest == src_g) src = &surfaces[i];
    Surface *dst = surface(dst_g, regs[0x86], regs[0x87], regs[0x80], 0);
    if (draw_logging)
        fprintf(stderr, "[draw] 2D blit %#llx (%ux%u fmt %#x, %s) -> %#llx (%ux%u fmt %#x): dst rect %u,%u %ux%u\n", src_g, regs[0x92],
                regs[0x93], regs[0x8C], src ? "on the host" : "in memory", dst_g, regs[0x86], regs[0x87], regs[0x80], regs[0x22C],
                regs[0x22D], regs[0x22E], regs[0x22F]);
    if (!src || !dst) { leave(); return; }   /* sources only in guest memory: not yet */
    double du = regs[0x231] + regs[0x230] / 4294967296.0, dv = regs[0x233] + regs[0x232] / 4294967296.0;
    double sx = regs[0x235] + regs[0x234] / 4294967296.0, sy = (i32)regs[0x237] + regs[0x236] / 4294967296.0;
    u32 dx = regs[0x22C], dy = regs[0x22D], dw = regs[0x22E], dh = regs[0x22F];
    if (!point_sampler) {
        D3D11_SAMPLER_DESC sd = {D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
                                 D3D11_TEXTURE_ADDRESS_CLAMP, 0, 1, D3D11_COMPARISON_NEVER, {0}, 0, D3D11_FLOAT32_MAX};
        ID3D11Device_CreateSamplerState(dev, &sd, &point_sampler);
    }
    /* draw the source rectangle into the destination rectangle */
    ID3D11ShaderResourceView *none[96] = {0};
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 96, none);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &dst->rtv, NULL);
    D3D11_VIEWPORT vp = {(float)dx, (float)dy, (float)dw, (float)dh, 0, 1};
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    D3D11_RECT all = {0, 0, 16384, 16384};
    ID3D11DeviceContext_RSSetScissorRects(ctx, 1, &all);
    /* each destination pixel samples the source at src0 + (pixel - dst0) * step, in source pixels */
    static ID3D11Buffer *rect_cb;
    if (!rect_cb) {
        D3D11_BUFFER_DESC bd = {32, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
        ID3D11Device_CreateBuffer(dev, &bd, NULL, &rect_cb);
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (rect_cb && SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)rect_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        float r[8] = {(float)sx, (float)sy, (float)du, (float)dv, (float)dx, (float)dy, 1.0f / (float)src->width, 1.0f / (float)src->height};
        memcpy(m.pData, r, sizeof r);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)rect_cb, 0);
    }
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 1, 1, &rect_cb);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_VSSetShader(ctx, blit_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, blit2d_ps ? blit2d_ps : blit_ps, NULL, 0);
    ID3D11ShaderResourceView *in = surface_srv(src, is_srgb(dst->view));   /* read as the destination writes: bytes unchanged */
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &in);
    ID3D11SamplerState *smp = (regs[0x223] >> 4) & 1 ? linear_sampler : point_sampler;
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &smp);
    D3D11_RASTERIZER_DESC rd = {D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0, 0, TRUE, FALSE, FALSE, FALSE};
    ID3D11DeviceContext_RSSetState(ctx, raster_state(&rd));
    ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, NULL, 0);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, none);
    leave();
}

/* How far behind the GPU is: at each present, how many earlier frames it has yet to finish
 * (event queries, polled, never waited for). Around 2 or more means the GPU is the limit;
 * near 0 means the CPU side is. */
#define FRAME_EVENTS 8
static ID3D11Query *frame_events[FRAME_EVENTS];
static int frame_event_live[FRAME_EVENTS];
static u32 frame_event_cur;
static u64 gfx_stat_inflight, gfx_stat_inflight_frames, gfx_stat_present_ticks;

static void gpu_timer_frame(void) {
    if (!frame_events[0]) {
        for (u32 i = 0; i < FRAME_EVENTS; i++) {
            D3D11_QUERY_DESC q = {D3D11_QUERY_EVENT, 0};
            ID3D11Device_CreateQuery(dev, &q, &frame_events[i]);
        }
        if (!frame_events[FRAME_EVENTS - 1]) return;
    }
    u32 behind = 0;
    for (u32 i = 0; i < FRAME_EVENTS; i++) {
        if (!frame_event_live[i]) continue;
        BOOL done = FALSE;
        if (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)frame_events[i], &done, sizeof done, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && done)
            frame_event_live[i] = 0;
        else
            behind++;
    }
    gfx_stat_inflight += behind;
    gfx_stat_inflight_frames++;
    if (!frame_event_live[frame_event_cur]) {
        ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)frame_events[frame_event_cur]);
        frame_event_live[frame_event_cur] = 1;
    }
    frame_event_cur = (frame_event_cur + 1) % FRAME_EVENTS;
}

void watchdog_present(void);   /* debug_watchdog.c */
void hitch_present(void);      /* debug_profile.c */

void gfx_present(u64 guest, u32 width, u32 height) {
    (void)width; (void)height;
    if (!enter()) return;
    watchdog_present();
    hitch_present();
    if (draw_logging) save_sampled();
    forget_draw_state();
    gfx_stat_presented++;
    release_graves();
    note_writes();
    static int logged, frames_logged, frames_to_log;
    static const char *after_compute = (const char *)1;
    if (after_compute == (const char *)1) after_compute = getenv("SSBU_GFX_LOG_AFTER_COMPUTE");
    if (draw_logging) {
        fprintf(stderr, "[draw] -- frame presented from %#llx\n", guest);
        const char *n = getenv("SSBU_GFX_LOG_FRAMES");   /* how many frames to log (1; F4: 2) */
        if (++frames_logged >= (n ? atoi(n) : frames_to_log ? frames_to_log : 1)) { draw_logging = 0; logged = 1; frames_to_log = 0; }
    }
    else if (gfx_debug() & 16) {   /* SSBU_GFX_LOG_AT: the seconds to log a frame at (comma-separated; 10) */
        static double at[16];
        static int nat = -1, next;
        if (nat < 0) {
            const char *s = getenv("SSBU_GFX_LOG_AT");
            nat = 0;
            for (char *e; s && *s && nat < 16; s = *e ? e + 1 : e) { at[nat++] = strtod(s, &e); if (e == s) break; }
            if (!nat) at[nat++] = 10;
        }
        if (next < nat && rt_now_ns() > (u64)(at[next] * 1e9)) {
            next++;
            draw_logging = 1;
            frames_logged = 0;
            fprintf(stderr, "[draw] -- frame start (at %.1f s)\n", at[next - 1]);
        }
    } else if (!logged && first_dispatch_ns && after_compute &&
               rt_now_ns() > first_dispatch_ns + strtoull(after_compute, NULL, 0) * 1000000000ull) {
        draw_logging = 1;
        fprintf(stderr, "[draw] -- frame start (battle); %u resident regions:\n", nresidents);
        for (u32 i = 0; i < nresidents; i++) fprintf(stderr, "[draw]    resident %#llx + %#x\n", residents[i].va, residents[i].size);
    }
    /* the window's new size (gpu_display.c), while nothing holds the back buffer */
    unsigned bw, bh;
    if (display_take_size(&bw, &bh)) {
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
        ID3D11DeviceContext_Flush(ctx);
        HRESULT hr = IDXGISwapChain1_ResizeBuffers(swapchain, 0, bw, bh, DXGI_FORMAT_UNKNOWN, swapchain_flags);
        if (FAILED(hr)) fprintf(stderr, "[gfx] cannot resize the swap chain to %ux%u (%#lx)\n", bw, bh, (unsigned long)hr);
    }
    ID3D11Texture2D *back;
    IDXGISwapChain1_GetBuffer(swapchain, 0, &IID_ID3D11Texture2D, (void **)&back);
    D3D11_TEXTURE2D_DESC back_desc;
    ID3D11Texture2D_GetDesc(back, &back_desc);
    ID3D11RenderTargetView *rtv;
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)back, NULL, &rtv);
    Surface *src = NULL;
    for (u32 i = 0; i < nsurfaces; i++)
        if (surfaces[i].tex && !surfaces[i].depth && surfaces[i].guest == guest) src = &surfaces[i];
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_VSSetShader(ctx, blit_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, blit_ps, NULL, 0);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &linear_sampler);
    ID3D11DeviceContext_RSSetState(ctx, NULL);
    ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, NULL, 0);
    ID3D11ShaderResourceView *none = NULL;
    if (src) {   /* the game's picture at its own size */
        D3D11_VIEWPORT vp = {0, 0, FINAL_W, FINAL_H, 0, 1};
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &final_rtv, NULL);
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
        /* the bytes as they are: the display shows them as sRGB, which an sRGB framebuffer's already are */
        ID3D11ShaderResourceView *raw = surface_srv(src, 0);
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &raw);
        ID3D11DeviceContext_Draw(ctx, 3, 0);
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &none);
    } else {
        static const float grey[4] = {0.1f, 0.1f, 0.12f, 1};
        ID3D11DeviceContext_ClearRenderTargetView(ctx, final_rtv, grey);
    }
    capture(final_tex);
    {   /* t29: dumps the source picture (what the game handed to present) and the composited
         * final, at the same moment, to tell whether an anomaly is already in the game's own
         * surface or introduced by our present blit.  SSBU_T29_SRC_DIR=<dir>
         * SSBU_T29_SRC_AT=<seconds>: dump every half second from AT for 6 seconds. */
        const char *d29 = getenv("SSBU_T29_SRC_DIR");
        static int last29 = -1000;
        if (d29) {
            static double at29 = -1;
            if (at29 < 0) at29 = getenv("SSBU_T29_SRC_AT") ? atof(getenv("SSBU_T29_SRC_AT")) : 0;
            double t = rt_now_ns() / 1e9;
            int bucket = (int)(t * 2);
            if (at29 <= 0 || (t >= at29 && t < at29 + 6.0 && bucket != last29)) {
                last29 = bucket;
                char p1[1024], p2[1024];
                int cs = (int)(t * 100);
                if (src) {
                    snprintf(p1, sizeof p1, "%s/src_%03d_%llx_%ux%u_fmt%x.bmp", d29, cs, src->guest, src->width, src->height, src->format);
                    save_surface_bmp(src, blit_ps, p1);
                    fprintf(stderr, "[t29] t=%.2f present src guest %#llx %ux%u fmt %#x -> %s\n", t, src->guest, src->width, src->height, src->format, p1);
                } else {
                    fprintf(stderr, "[t29] t=%.2f present src: none (guest %#llx not a live surface)\n", t, guest);
                }
                snprintf(p2, sizeof p2, "%s/final_%03d.bmp", d29, cs);
                save_bmp(final_tex, p2);
            }
        }
    }
    {   /* t26 one-shot surface dump (SSBU_T26_SURF_AT=<seconds>, dir SSBU_T26_SURF_DIR):
         * proves the model MRT surfaces at that moment are populated and textured, not black. */
        static const char *sd26 = (const char *)1; static int done26;
        if (sd26 == (const char *)1) sd26 = getenv("SSBU_T26_SURF_DIR");
        if (sd26 && !done26) {
            static double at26 = -1;
            if (at26 < 0) at26 = getenv("SSBU_T26_SURF_AT") ? atof(getenv("SSBU_T26_SURF_AT")) : 0;
            if (at26 <= 0 || rt_now_ns() > (u64)(at26 * 1e9)) { done26 = 1; capture_surfaces(sd26, 0); fprintf(stderr, "[t26] surface dump -> %s\n", sd26); }
        }
    }
    /* then into the window: 16:9, centred, black around it */
    static const float black[4] = {0, 0, 0, 1};
    unsigned fit_x, fit_y, fit_w, fit_h;
    display_fit(back_desc.Width, back_desc.Height, &fit_x, &fit_y, &fit_w, &fit_h);
    D3D11_VIEWPORT wvp = {(float)fit_x, (float)fit_y, (float)fit_w, (float)fit_h, 0, 1};
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, black);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &wvp);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &final_srv);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &none);

    /* debug keys and the overlay */
    LONG keys = InterlockedExchange(&hotkeys, 0);
    static double f4_at = -1;   /* SSBU_F4_AT: the second to press F4 at (debug runs) */
    if (f4_at == -1) f4_at = getenv("SSBU_F4_AT") ? atof(getenv("SSBU_F4_AT")) : -2;
    if (f4_at >= 0 && rt_now_ns() > (u64)(f4_at * 1e9)) { f4_at = -2; keys |= 8; }
    static int overlay = -1, details;
    if (overlay < 0) overlay = !getenv("SSBU_OVERLAY") || atoi(getenv("SSBU_OVERLAY")) != 0;
    if (keys & 1) overlay = !overlay;
    if (keys & 2) { details = !details; overlay = 1; }
    if (keys & 4) {   /* F3: the frame as the game drew it (without the overlay) */
        CreateDirectoryA("screenshots", NULL);
        SYSTEMTIME st;
        GetLocalTime(&st);
        char path[128];
        snprintf(path, sizeof path, "screenshots/ssbu_%04u%02u%02u_%02u%02u%02u_%03u.bmp", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                 st.wSecond, st.wMilliseconds);
        save_bmp(final_tex, path);
        fprintf(stderr, "[gfx] screenshot: %s\n", path);
    }
    static char f4_dir[160];   /* F4's render targets go here, once the logged frame is drawn */
    if (f4_dir[0] && !draw_logging) {
        capture_surfaces(f4_dir, 0);
        fprintf(stderr, "[gfx] render targets of the logged frame: %s\n", f4_dir);
        f4_dir[0] = 0;
        sampled_dir[0] = 0;
    }
    if (keys & 8) {   /* F4: every draw of the next frame to the log, and its render targets to a folder */
        draw_logging = 1;
        frames_logged = 0;
        /* two presents' worth: the game presents in the middle of drawing a frame (after its
         * scene, before its effects, post-processing and HUD), so one would miss half */
        frames_to_log = 2;
        SYSTEMTIME st;
        GetLocalTime(&st);
        CreateDirectoryA("screenshots", NULL);
        snprintf(f4_dir, sizeof f4_dir, "screenshots/frame_%04u%02u%02u_%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        CreateDirectoryA(f4_dir, NULL);
        snprintf(sampled_dir, sizeof sampled_dir, "%s", f4_dir);
        nsampled_saved = nsampled_pending = 0;
        sampled_seq = 0;
        char screen[200];
        snprintf(screen, sizeof screen, "%s/screen.bmp", f4_dir);
        save_bmp(final_tex, screen);
        fprintf(stderr, "[draw] -- frame start (F4)\n");
    }
    if (keys & 16) {   /* F5: depth buffers sampled as textures, or not */
        if (depth_sampling < 0) depth_sampling = !getenv("SSBU_NO_DEPTH_SAMPLING");
        depth_sampling = !depth_sampling;
        fprintf(stderr, "[gfx] depth sampling %s (F5)\n", depth_sampling ? "on" : "off");
    }
    static double profile_at = -1;   /* SSBU_PROFILE_AT: the second to start one at (debug runs) */
    if (profile_at == -1) profile_at = getenv("SSBU_PROFILE_AT") ? atof(getenv("SSBU_PROFILE_AT")) : -2;
    if (profile_at >= 0 && rt_now_ns() > (u64)(profile_at * 1e9)) { profile_at = -2; keys |= 32; }
    if (keys & 32) profile_start(10.0);   /* F6 */
    static char detail[1800], hint[640];
    static u64 last_ns, last_presented, last_drawn, last_dispatched, last_skipped, last_watch, last_busy, last_upload, last_waits, last_composed, last_kind[4];
    u64 now = rt_now_ns();
    if (now - last_ns >= 500000000ull) {   /* the details, twice a second */
        double secs = (now - last_ns) / 1e9;
        u64 frames = gfx_stat_presented - last_presented;
        if (!frames) frames = 1;
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        static u64 last_inflight, last_inflight_frames, last_present_ticks;
        u64 gpu_frames = gfx_stat_inflight_frames - last_inflight_frames;
        snprintf(detail, sizeof detail,
                 "GPU behind by %.1f frames  Present blocked %.1f ms/frame\n"
                 "renderer %.1f ms/frame  uploads %.1f MB/frame (%llu composed)\n"
                 "  constants %.1f  vertices %.1f  storage %.1f  textures %.1f MB/frame\n"
                 "draws %llu/frame  compute %llu/frame  GPU waits %llu  skipped %.0f/s\n"
                 "shaders %llu (%ld compiling, prewarm %ld/%ld)  textures %u  surfaces %u\n"
                 "GPU-resident buffers %u  write watch %.1f ms/s  depth sampling %s\n"
                 "F1 overlay  F2 details  F3 screenshot  F4 log a frame\n"
                 "F5 depth sampling on/off  F6 profile 10 s%s",
                 gpu_frames ? (double)(gfx_stat_inflight - last_inflight) / gpu_frames : 0.0,
                 (gfx_stat_present_ticks - last_present_ticks) * 1000.0 / (double)freq.QuadPart / frames,
                 (gfx_stat_busy_ticks - last_busy) * 1000.0 / (double)freq.QuadPart / frames,
                 (gfx_stat_upload_bytes - last_upload) / 1048576.0 / frames, (gfx_stat_composed - last_composed) / frames,
                 (gfx_stat_upload_kind[0] - last_kind[0]) / 1048576.0 / frames, (gfx_stat_upload_kind[1] - last_kind[1]) / 1048576.0 / frames,
                 (gfx_stat_upload_kind[2] - last_kind[2]) / 1048576.0 / frames, (gfx_stat_upload_kind[3] - last_kind[3]) / 1048576.0 / frames,
                 (gfx_stat_drawn - last_drawn) / frames, (gfx_stat_dispatched - last_dispatched) / frames, gfx_stat_gpu_waits - last_waits,
                 (gfx_stat_skipped - last_skipped) / secs, gfx_stat_shaders, (long)compiles_pending, (long)prewarm_ready, (long)prewarm_total, ntextures, nsurfaces, nresidents,
                 (gfx_stat_watch_us - last_watch) / 1000.0 / secs, depth_sampling != 0 ? "on" : "off",
                 profile_running() ? "  (profiling...)" : "");
        char net[512];
        net_status(net, sizeof net);
        snprintf(hint, sizeof hint, "F2 details%s%s%s%s", depth_sampling == 0 ? "  depth sampling OFF" : "",
                 profile_running() ? "  profiling..." : "", net[0] ? "\n" : "", net);
        if (net[0]) { size_t l = strlen(detail); snprintf(detail + l, sizeof detail - l, "\n%s", net); }
        char pads[256];
        pad_describe(pads, sizeof pads);
        { size_t l = strlen(detail); snprintf(detail + l, sizeof detail - l, "\npads: %s", pads[0] ? pads : "none (keyboard only)"); }
        static int log_stats = -1, stat_tick;   /* SSBU_LOG_STATS: the details to the log too, every 5 s */
        if (log_stats < 0) log_stats = getenv("SSBU_LOG_STATS") != NULL;
        if (log_stats && ++stat_tick % 10 == 0) {
            char line[1024];
            snprintf(line, sizeof line, "%s", detail);
            for (char *nl = strchr(line, '\n'); nl; nl = strchr(nl, '\n')) *nl = '|';
            fprintf(stderr, "[stats] %.1f fps | %s | largest constants: %s cb%u, %#x bytes (extent %#x)\n", frames / secs, line,
                    gfx_stat_biggest_cb.shader, gfx_stat_biggest_cb.bank, gfx_stat_biggest_cb.size, gfx_stat_biggest_cb.extent);
            memset(&gfx_stat_biggest_cb, 0, sizeof gfx_stat_biggest_cb);
            u32 nk = 0;
            for (u32 i = 0; i < KEPT_SETS * KEPT_WAYS; i++) nk += kept[i].srv != NULL;
            fprintf(stderr, "[stats] kept constant buffers: %u; so far %llu made, %llu updated by the command stream, %llu taken again after CPU writes, %llu mismatches\n",
                    nk, gfx_stat_kept_made, gfx_stat_kept_applied, gfx_stat_kept_reloaded, gfx_stat_kept_mismatches);
        }
        last_ns = now; last_presented = gfx_stat_presented; last_drawn = gfx_stat_drawn;
        last_dispatched = gfx_stat_dispatched; last_skipped = gfx_stat_skipped; last_watch = gfx_stat_watch_us;
        memcpy(last_kind, gfx_stat_upload_kind, sizeof last_kind);
        last_busy = gfx_stat_busy_ticks; last_upload = gfx_stat_upload_bytes; last_waits = gfx_stat_gpu_waits; last_composed = gfx_stat_composed;
        last_inflight = gfx_stat_inflight; last_inflight_frames = gfx_stat_inflight_frames; last_present_ticks = gfx_stat_present_ticks;
    }
    overlay_present(dev, ctx, rtv, fit_x, fit_y, fit_w, fit_h, overlay, details ? detail : hint);

    ID3D11RenderTargetView_Release(rtv);
    ID3D11Texture2D_Release(back);
    gpu_timer_frame();
    LARGE_INTEGER p0, p1;
    QueryPerformanceCounter(&p0);
    IDXGISwapChain1_Present(swapchain, 0, display_present_flags());   /* (blocks while the GPU is frames behind) */
    QueryPerformanceCounter(&p1);
    gfx_stat_present_ticks += (u64)(p1.QuadPart - p0.QuadPart);
    leave();
}

int gfx_window_focused(void) { return window && GetForegroundWindow() == window; }
void *gfx_window_handle(void) { return window; }

/* The output (monitor) the swap chain is on, referenced; NULL before there is one (display pacing). */
void *gfx_display_output(void) {
    if (!ready || !enter()) return NULL;
    IDXGIOutput *output = NULL;
    if (FAILED(IDXGISwapChain1_GetContainingOutput(swapchain, &output))) output = NULL;
    leave();
    return output;
}

/* The plain 2D textures in the cache, drawn into RGBA8 images like the surfaces
 * (tex_<frame>_<guest address>_fmt<format>.bmp). */
static void capture_textures(const char *dir, int frame) {
    u32 saved = 0;
    const char *only = getenv("SSBU_CAPTURE_TEX_FMT");   /* just this texture format */
    for (u32 i = 0; i < ntextures && saved < 40; i++) {
        Texture *t = &textures[i];
        if (!t->srv) continue;
        if (only && (t->tic[0] & 0x7F) != strtoul(only, NULL, 0)) continue;
        D3D11_SHADER_RESOURCE_VIEW_DESC vd0;
        ID3D11ShaderResourceView_GetDesc(t->srv, &vd0);
        if (vd0.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE3D) continue;
        D3D11_TEXTURE2D_DESC src;
        ID3D11Texture2D_GetDesc(t->tex, &src);
        if (src.ArraySize != 1 || (src.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE)) continue;
        /* a plain 2D view of level 0, whatever view the shaders use */
        D3D11_SHADER_RESOURCE_VIEW_DESC vd;
        ID3D11ShaderResourceView_GetDesc(t->srv, &vd);
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        vd.Texture2D.MostDetailedMip = 0;
        vd.Texture2D.MipLevels = 1;
        ID3D11ShaderResourceView *view;
        if (FAILED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)t->tex, &vd, &view))) continue;
        D3D11_TEXTURE2D_DESC td = {src.Width, src.Height, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_RENDER_TARGET, 0, 0};
        ID3D11Texture2D *tmp;
        ID3D11RenderTargetView *rtv;
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &tmp))) { ID3D11ShaderResourceView_Release(view); continue; }
        ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)tmp, NULL, &rtv);
        D3D11_VIEWPORT vp = {0, 0, (float)src.Width, (float)src.Height, 0, 1};
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
        ID3D11DeviceContext_VSSetShader(ctx, blit_vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(ctx, blit_ps, NULL, 0);
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &view);
        ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &linear_sampler);
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xFFFFFFFF);
        ID3D11DeviceContext_Draw(ctx, 3, 0);
        char path[1024];
        snprintf(path, sizeof path, "%s/tex_%03d_%llx_fmt%x_%ux%u.bmp", dir, frame, t->guest, t->tic[0] & 0x7F, src.Width, src.Height);
        save_bmp(tmp, path);
        if (blit_alpha_ps) {   /* and its alpha */
            ID3D11DeviceContext_PSSetShader(ctx, blit_alpha_ps, NULL, 0);
            ID3D11DeviceContext_Draw(ctx, 3, 0);
            snprintf(path, sizeof path, "%s/tex_%03d_%llx_fmt%x_%ux%u_alpha.bmp", dir, frame, t->guest, t->tic[0] & 0x7F, src.Width, src.Height);
            save_bmp(tmp, path);
        }
        ID3D11ShaderResourceView *none = NULL;
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &none);
        saved++;
        ID3D11ShaderResourceView_Release(view);
        ID3D11RenderTargetView_Release(rtv);
        ID3D11Texture2D_Release(tmp);
    }
}
