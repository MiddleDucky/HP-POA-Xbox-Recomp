/**
 * NV2A on the host GPU, through Direct3D 11. See nv2a_gpu.h.
 *
 * What lives where:
 *   - Render targets and depth/stencil buffers are D3D11 textures keyed by
 *     the guest address the title draws into, at RECOMP_GPU_SCALE times the
 *     guest resolution (default 2). Nothing is written back to guest memory;
 *     a render target the title later samples as a texture is bound directly.
 *   - Textures are decoded from guest memory once and cached, keyed by
 *     address and format and re-checked by content hash once per frame.
 *     DXT goes up as BC1/2/3 unchanged; everything else through the same
 *     texel decoder the software rasteriser uses.
 *   - The register combiners are one pixel shader that reads the combiner
 *     words from constants, so no state change ever compiles a shader.
 *   - The window thread presents (xbox_FramebufferWindowSetPresenter): the
 *     last finished frame, scaled to the window with the RECOMP_ASPECT rules,
 *     and the movie overlay.
 *
 * Every use of the immediate context is under one lock: the executor draws
 * from its thread and the window thread presents from its own.
 */
#if defined(_WIN32)

#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <d3dcompiler.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_gpu.h"
#include "../d3d/d3d8_swizzle.h"
#include "../video/video_player.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define GPU_LOG(...) fprintf(stderr, "[GPU11] " __VA_ARGS__)

static CRITICAL_SECTION s_lock;
static int s_state = -1;               /* -1 unknown, 0 off/failed, 1 on */
static int s_scale = 2;

static ID3D11Device        *s_dev;
static ID3D11DeviceContext *s_ctx;
static ID3D11DeviceContext1 *s_ctx1;

static ID3D11VertexShader *s_vs;
static ID3D11PixelShader  *s_ps;
static ID3D11InputLayout  *s_layout;
static ID3D11VertexShader *s_vs_blit;
static ID3D11PixelShader  *s_ps_blit;
static ID3D11Buffer *s_cb, *s_cb_blit;
static ID3D11Buffer *s_vb, *s_ib;
static UINT s_vb_pos = 0xFFFFFFFFu, s_ib_pos = 0xFFFFFFFFu;  /* first map discards */
static struct { uint32_t draws, no_rt, map_fail, cleared, flips_found, flips_missing; } s_stat;
#define GPU_VB_BYTES (32u << 20)
#define GPU_IB_BYTES (8u << 20)

static ID3D11ShaderResourceView *s_white2d, *s_whitecube;
static ID3D11SamplerState *s_blit_sampler;
static ID3D11RasterizerState *s_rs_blit;
static ID3D11BlendState *s_bs_opaque;
static ID3D11DepthStencilState *s_ds_off;

static uint32_t s_frame;               /* bumped at every flip */

/* ── Shaders ─────────────────────────────────────────────────────────────── */

static const char s_hlsl[] =
"cbuffer Draw : register(b0) {\n"
"  float4 vp;        // guest w, h, 1/zmax, 0\n"
"  uint4  ctl;       // combiner control, stage program, final0, final1\n"
"  uint4  cw[8];     // colour icw, alpha icw, colour ocw, alpha ocw\n"
"  float4 fac0[8];\n"
"  float4 fac1[8];\n"
"  float4 fin_c0, fin_c1, fogc;\n"
"  uint4  misc;      // rc_seen, alpha test func (0 off), alpha ref, clip plane mode\n"
"  float4 tsz[4];    // xy coordinate scale, z usable, w cube\n"
"};\n"
"struct VSI { float4 p:POSITION; float4 d0:COLOR0; float4 d1:COLOR1;\n"
"  float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
"  float fog:TEXCOORD4; };\n"
"struct PSI { float4 p:SV_Position; float4 d0:COLOR0; float4 d1:COLOR1;\n"
"  float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
"  float fog:TEXCOORD4; };\n"
"PSI vs_main(VSI i) {\n"
"  PSI o; float w = i.p.w;\n"
"  float x = i.p.x / vp.x * 2.0 - 1.0, y = 1.0 - i.p.y / vp.y * 2.0;\n"
"  o.p = float4(x * w, y * w, i.p.z * vp.z * w, w);\n"
"  o.d0 = saturate(i.d0); o.d1 = saturate(i.d1);\n"
"  o.t0 = i.t0; o.t1 = i.t1; o.t2 = i.t2; o.t3 = i.t3; o.fog = i.fog; return o; }\n"
"Texture2D T0:register(t0); Texture2D T1:register(t1); Texture2D T2:register(t2); Texture2D T3:register(t3);\n"
"TextureCube C0:register(t4); TextureCube C1:register(t5); TextureCube C2:register(t6); TextureCube C3:register(t7);\n"
"SamplerState S0:register(s0); SamplerState S1:register(s1); SamplerState S2:register(s2); SamplerState S3:register(s3);\n"
"static float4 R[16];\n"
"float map_in(float x, uint b) { float p = max(x, 0.0);\n"
"  switch ((b >> 5) & 7) { case 0: return p; case 1: return 1.0 - saturate(x);\n"
"  case 2: return 2.0 * p - 1.0; case 3: return -2.0 * p + 1.0; case 4: return p - 0.5;\n"
"  case 5: return -p + 0.5; case 6: return x; default: return -x; } }\n"
"float map_out(float x, uint m) { if (m == 0x08) return x - 0.5; if (m == 0x10) return x * 2.0;\n"
"  if (m == 0x18) return (x - 0.5) * 2.0; if (m == 0x20) return x * 4.0; if (m == 0x30) return x * 0.5; return x; }\n"
"float3 rgb_in(uint b) { float4 r = R[b & 15]; float3 v = (b & 0x10) ? r.aaa : r.rgb;\n"
"  return float3(map_in(v.x, b), map_in(v.y, b), map_in(v.z, b)); }\n"
"float a_in(uint b) { float4 r = R[b & 15]; return map_in((b & 0x10) ? r.a : r.b, b); }\n"
"#define FETCH(st, T, C, S, tcin) { uint mode = (ctl.y >> (st * 5)) & 31; float4 tc = tcin; \\\n"
"  float4 r = float4(0, 0, 0, 1); \\\n"
"  if (mode == 1 || mode == 2) { float q = tc.w != 0.0 ? tc.w : 1.0; \\\n"
"    r = tsz[st].z > 0.0 ? T.Sample(S, tc.xy / q * tsz[st].xy) : float4(1, 1, 1, 1); } \\\n"
"  else if (mode == 3) { r = tsz[st].z <= 0.0 ? float4(1, 1, 1, 1) : (tsz[st].w > 0.0 ? C.Sample(S, tc.xyz) \\\n"
"                                    : T.Sample(S, tc.xy * tsz[st].xy)); } \\\n"
"  else if (mode == 4) r = saturate(tc); \\\n"
"  else if (mode == 5) { r = float4(0, 0, 0, 0); [unroll] for (uint j = 0; j < 4; j++) { \\\n"
"    bool ge = ((misc.w >> (st * 4 + j)) & 1) != 0; if (ge ? tc[j] >= 0.0 : tc[j] < 0.0) discard; } } \\\n"
"  else if (mode != 0) r = float4(0, 0, 0, 0); \\\n"
"  t[st] = r; }\n"
"float4 ps_main(PSI i) : SV_Target {\n"
"  float4 t[4];\n"
"  FETCH(0, T0, C0, S0, i.t0) FETCH(1, T1, C1, S1, i.t1) FETCH(2, T2, C2, S2, i.t2) FETCH(3, T3, C3, S3, i.t3)\n"
"  float4 o;\n"
"  if (misc.x == 0) {\n"
"    o = ((ctl.y & 31) != 0) ? t[0] * i.d0 : i.d0;\n"
"  } else {\n"
"    [unroll] for (uint k = 0; k < 16; k++) R[k] = float4(0, 0, 0, 0);\n"
"    R[3] = float4(fogc.rgb, saturate(i.fog)); R[4] = i.d0; R[5] = i.d1;\n"
"    R[8] = t[0]; R[9] = t[1]; R[10] = t[2]; R[11] = t[3];\n"
"    R[12].a = (ctl.y & 31) != 0 ? t[0].a : 1.0;\n"
"    uint n = min(ctl.x & 0xFF, 8u), fl = ctl.x >> 8;\n"
"    [loop] for (uint s = 0; s < n; s++) {\n"
"      uint icw = cw[s].x, aicw = cw[s].y, ocw = cw[s].z, aocw = cw[s].w;\n"
"      uint f = ocw >> 12, af = aocw >> 12;\n"
"      R[1] = fac0[(fl & 0x010) ? s : 0]; R[2] = fac1[(fl & 0x100) ? s : 0];\n"
"      bool mux = (fl & 1) ? (R[12].a >= 0.5) : ((uint(R[12].a * 255.0) & 1) != 0);\n"
"      float3 A = rgb_in(icw >> 24), B = rgb_in(icw >> 16), C = rgb_in(icw >> 8), D = rgb_in(icw);\n"
"      float aA = a_in(aicw >> 24), aB = a_in(aicw >> 16), aC = a_in(aicw >> 8), aD = a_in(aicw);\n"
"      float3 pab = (f & 2) ? dot(A, B).xxx : A * B;\n"
"      float3 pcd = (f & 1) ? dot(C, D).xxx : C * D;\n"
"      float3 sum = (f & 4) ? (mux ? pcd : pab) : pab + pcd;\n"
"      uint m = f & 0x38;\n"
"      float3 ab = clamp(float3(map_out(pab.x, m), map_out(pab.y, m), map_out(pab.z, m)), -1.0, 1.0);\n"
"      float3 cd = clamp(float3(map_out(pcd.x, m), map_out(pcd.y, m), map_out(pcd.z, m)), -1.0, 1.0);\n"
"      float3 ms = clamp(float3(map_out(sum.x, m), map_out(sum.y, m), map_out(sum.z, m)), -1.0, 1.0);\n"
"      float apab = aA * aB, apcd = aC * aD;\n"
"      float asum = (af & 4) ? (mux ? apcd : apab) : apab + apcd;\n"
"      uint am = af & 0x38;\n"
"      float aab = clamp(map_out(apab, am), -1.0, 1.0), acd = clamp(map_out(apcd, am), -1.0, 1.0);\n"
"      float ams = clamp(map_out(asum, am), -1.0, 1.0);\n"
"      uint d;\n"
"      d = (ocw >> 4) & 15; if (d) { R[d].rgb = ab; if (f & 0x80) R[d].a = ab.z; }\n"
"      d = ocw & 15;        if (d) { R[d].rgb = cd; if (f & 0x40) R[d].a = cd.z; }\n"
"      d = (ocw >> 8) & 15; if (d) R[d].rgb = ms;\n"
"      d = (aocw >> 4) & 15; if (d) R[d].a = aab;\n"
"      d = aocw & 15;        if (d) R[d].a = acd;\n"
"      d = (aocw >> 8) & 15; if (d) R[d].a = ams;\n"
"      R[0] = float4(0, 0, 0, 0);\n"
"    }\n"
"    if (ctl.z != 0 || ctl.w != 0) {\n"
"      uint f0 = ctl.z, f1 = ctl.w, ff = f1 & 0xFF;\n"
"      R[1] = fin_c0; R[2] = fin_c1;\n"
"      float3 a = (ff & 0x40) ? 1.0 - R[5].rgb : R[5].rgb;\n"
"      float3 b = (ff & 0x20) ? 1.0 - R[12].rgb : R[12].rgb;\n"
"      R[14] = float4((ff & 0x80) ? saturate(a + b) : a + b, 0.0);\n"
"      float3 E = rgb_in(f1 >> 24), F = rgb_in(f1 >> 16);\n"
"      R[15] = float4(E * F, 0.0);\n"
"      float3 A = rgb_in(f0 >> 24), B = rgb_in(f0 >> 16), C = rgb_in(f0 >> 8), D = rgb_in(f0);\n"
"      o = float4(D + C * (1.0 - A) + B * A, a_in(f1 >> 8));\n"
"    } else {\n"
"      o = R[12];\n"
"    }\n"
"  }\n"
"  if (vp.w == 1.0) o = float4(1, 0, 0, 1); else if (vp.w == 2.0) o = float4(i.d0.rgb, 1); else if (vp.w == 3.0) o = float4(t[0].rgb, 1);\n"
"  o = saturate(o);\n"
"  if (misc.y != 0) {\n"
"    int v = int(o.a * 255.0 + 0.5), r = int(misc.z & 0xFF); bool ok = true;\n"
"    switch (misc.y) { case 0x200: ok = false; break; case 0x201: ok = v < r; break;\n"
"      case 0x202: ok = v == r; break; case 0x203: ok = v <= r; break; case 0x204: ok = v > r; break;\n"
"      case 0x205: ok = v != r; break; case 0x206: ok = v >= r; break; default: break; }\n"
"    if (!ok) discard;\n"
"  }\n"
"  return o; }\n"
/* Presenting: a quad over `rect` (NDC x0,y0,x1,y1) sampling t0. */
"cbuffer Blit : register(b1) { float4 rect; };\n"
"struct BV { float4 p:SV_Position; float2 uv:TEXCOORD0; };\n"
"BV vs_blit(uint id : SV_VertexID) { BV o; float2 c = float2(id & 1, id >> 1);\n"
"  o.p = float4(lerp(rect.x, rect.z, c.x), lerp(rect.y, rect.w, c.y), 0.0, 1.0); o.uv = float2(c.x, c.y); return o; }\n"
"Texture2D BT:register(t0); SamplerState BS:register(s0);\n"
"float4 ps_blit(BV i) : SV_Target { return float4(BT.Sample(BS, i.uv).rgb, 1.0); }\n";

typedef struct {
    float    vp[4];
    uint32_t ctl[4];
    uint32_t cw[8][4];
    float    fac0[8][4], fac1[8][4];
    float    fin_c0[4], fin_c1[4], fogc[4];
    uint32_t misc[4];
    float    tsz[4][4];
} DrawCB;

typedef HRESULT (WINAPI *pD3DCompile_t)(const void *, SIZE_T, const char *,
    const D3D_SHADER_MACRO *, ID3DInclude *, const char *, const char *,
    UINT, UINT, ID3DBlob **, ID3DBlob **);

static ID3DBlob *compile(pD3DCompile_t fn, const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = fn(s_hlsl, sizeof(s_hlsl) - 1, "nv2a", NULL, NULL, entry, target,
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        GPU_LOG("shader %s failed (0x%08lX): %s\n", entry, (unsigned long)hr,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        if (err) ID3D10Blob_Release(err);
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return code;
}

static ID3D11ShaderResourceView *solid_srv(int cube)
{
    static const uint32_t white[6] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                                      0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA sd[6];
    ID3D11Texture2D *t = NULL;
    ID3D11ShaderResourceView *v = NULL;
    int i;

    memset(&td, 0, sizeof(td));
    td.Width = td.Height = 1;
    td.MipLevels = 1;
    td.ArraySize = cube ? 6 : 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    for (i = 0; i < 6; i++) {
        sd[i].pSysMem = &white[i];
        sd[i].SysMemPitch = 4;
        sd[i].SysMemSlicePitch = 4;
    }
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, sd, &t)))
        return NULL;
    ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)t, NULL, &v);
    ID3D11Texture2D_Release(t);
    return v;
}

static ID3D11Buffer *make_buffer(UINT bytes, UINT bind)
{
    D3D11_BUFFER_DESC bd;
    ID3D11Buffer *b = NULL;
    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = bytes;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = bind;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &b);
    return b;
}

static int gpu_create(void)
{
    static const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got;
    HMODULE dc;
    pD3DCompile_t compile_fn;
    ID3DBlob *vs, *ps, *vsb, *psb;
    HRESULT hr;

    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, want, 2,
                           D3D11_SDK_VERSION, &s_dev, &got, &s_ctx);
    if (FAILED(hr))
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, want + 1, 1,
                               D3D11_SDK_VERSION, &s_dev, &got, &s_ctx);
    if (FAILED(hr)) {
        GPU_LOG("D3D11CreateDevice failed (0x%08lX); GPU rendering off\n", (unsigned long)hr);
        return 0;
    }
    ID3D11DeviceContext_QueryInterface(s_ctx, &IID_ID3D11DeviceContext1, (void **)&s_ctx1);

    dc = LoadLibraryA("d3dcompiler_47.dll");
    compile_fn = dc ? (pD3DCompile_t)(void *)GetProcAddress(dc, "D3DCompile") : NULL;
    if (!compile_fn) {
        GPU_LOG("d3dcompiler_47.dll not available; GPU rendering off\n");
        return 0;
    }
    vs = compile(compile_fn, "vs_main", "vs_5_0");
    ps = compile(compile_fn, "ps_main", "ps_5_0");
    vsb = compile(compile_fn, "vs_blit", "vs_5_0");
    psb = compile(compile_fn, "ps_blit", "ps_5_0");
    if (!vs || !ps || !vsb || !psb)
        return 0;
    ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vs),
                                    ID3D10Blob_GetBufferSize(vs), NULL, &s_vs);
    ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(ps),
                                   ID3D10Blob_GetBufferSize(ps), NULL, &s_ps);
    ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vsb),
                                    ID3D10Blob_GetBufferSize(vsb), NULL, &s_vs_blit);
    ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(psb),
                                   ID3D10Blob_GetBufferSize(psb), NULL, &s_ps_blit);
    {
        static const D3D11_INPUT_ELEMENT_DESC el[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,   D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16,  D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR",    1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32,  D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48,  D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 64,  D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 80,  D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 96,  D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 4, DXGI_FORMAT_R32_FLOAT,          0, 112, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        ID3D11Device_CreateInputLayout(s_dev, el, 8, ID3D10Blob_GetBufferPointer(vs),
                                       ID3D10Blob_GetBufferSize(vs), &s_layout);
    }
    ID3D10Blob_Release(vs); ID3D10Blob_Release(ps);
    ID3D10Blob_Release(vsb); ID3D10Blob_Release(psb);
    if (!s_vs || !s_ps || !s_layout || !s_vs_blit || !s_ps_blit) {
        GPU_LOG("shader objects could not be created; GPU rendering off\n");
        return 0;
    }

    s_cb = make_buffer((sizeof(DrawCB) + 15) & ~15u, D3D11_BIND_CONSTANT_BUFFER);
    s_cb_blit = make_buffer(16, D3D11_BIND_CONSTANT_BUFFER);
    s_vb = make_buffer(GPU_VB_BYTES, D3D11_BIND_VERTEX_BUFFER);
    s_ib = make_buffer(GPU_IB_BYTES, D3D11_BIND_INDEX_BUFFER);
    s_white2d = solid_srv(0);
    s_whitecube = solid_srv(1);
    {
        D3D11_SAMPLER_DESC sd;
        D3D11_RASTERIZER_DESC rd;
        D3D11_BLEND_DESC bd;
        D3D11_DEPTH_STENCIL_DESC dd;
        memset(&sd, 0, sizeof(sd));
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        ID3D11Device_CreateSamplerState(s_dev, &sd, &s_blit_sampler);
        memset(&rd, 0, sizeof(rd));
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs_blit);
        memset(&bd, 0, sizeof(bd));
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        ID3D11Device_CreateBlendState(s_dev, &bd, &s_bs_opaque);
        memset(&dd, 0, sizeof(dd));
        ID3D11Device_CreateDepthStencilState(s_dev, &dd, &s_ds_off);
    }
    if (!s_cb || !s_cb_blit || !s_vb || !s_ib || !s_white2d || !s_whitecube) {
        GPU_LOG("buffers could not be created; GPU rendering off\n");
        return 0;
    }
    GPU_LOG("Direct3D 11 renderer up (feature level %x, scale %dx)\n", (unsigned)got, s_scale);
    return 1;
}

static int gpu_present(void *hwnd, const XboxPresentInfo *pi);

int nv2a_gpu_enabled(void)
{
    if (s_state < 0) {
        const char *e = getenv("RECOMP_GPU");
        const char *sc = getenv("RECOMP_GPU_SCALE");
        InitializeCriticalSection(&s_lock);
        s_state = 0;
        if (sc && atoi(sc) >= 1 && atoi(sc) <= 8)
            s_scale = atoi(sc);
        if (e && *e && *e != '0') {
            EnterCriticalSection(&s_lock);
            s_state = gpu_create();
            LeaveCriticalSection(&s_lock);
            if (s_state)
                xbox_FramebufferWindowSetPresenter(gpu_present);
        }
    }
    return s_state;
}

/* ── Render targets and depth buffers ────────────────────────────────────── */

typedef struct {
    uint32_t base, gw, gh;             /* guest address and size          */
    ID3D11Texture2D *tex;
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *srv;
    uint32_t used;
} GpuRT;

typedef struct {
    uint32_t base, w, h;               /* host size: matches its target   */
    ID3D11Texture2D *tex;
    ID3D11DepthStencilView *dsv;
    uint32_t used;
} GpuDS;

#define GPU_MAX_RT 32
static GpuRT s_rt[GPU_MAX_RT];
static GpuDS s_ds[GPU_MAX_RT];

static void rt_release(GpuRT *r)
{
    if (r->srv) ID3D11ShaderResourceView_Release(r->srv);
    if (r->rtv) ID3D11RenderTargetView_Release(r->rtv);
    if (r->tex) ID3D11Texture2D_Release(r->tex);
    memset(r, 0, sizeof(*r));
}

static GpuRT *rt_find(uint32_t base)
{
    int i;
    for (i = 0; i < GPU_MAX_RT; i++)
        if (s_rt[i].tex && s_rt[i].base == base)
            return &s_rt[i];
    return NULL;
}

/* The target at `base`, at least gw x gh guest pixels. Grown in place (old
 * contents copied) when a later draw needs more. */
static GpuRT *rt_get(uint32_t base, uint32_t gw, uint32_t gh)
{
    GpuRT *r = rt_find(base), *slot = NULL;
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *tex = NULL;
    int i;

    if (r && r->gw >= gw && r->gh >= gh) {
        r->used = s_frame;
        return r;
    }
    if (r) {
        if (gw < r->gw) gw = r->gw;
        if (gh < r->gh) gh = r->gh;
        slot = r;
    } else {
        for (i = 0; i < GPU_MAX_RT && !slot; i++)
            if (!s_rt[i].tex)
                slot = &s_rt[i];
        if (!slot) {                       /* evict the least recently used */
            slot = &s_rt[0];
            for (i = 1; i < GPU_MAX_RT; i++)
                if (s_rt[i].used < slot->used)
                    slot = &s_rt[i];
            rt_release(slot);
        }
    }
    memset(&td, 0, sizeof(td));
    td.Width = gw * (uint32_t)s_scale;
    td.Height = gh * (uint32_t)s_scale;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &tex)))
        return NULL;
    if (r && r->tex) {
        D3D11_BOX box = {0, 0, 0, r->gw * (UINT)s_scale, r->gh * (UINT)s_scale, 1};
        ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)tex, 0, 0, 0, 0,
                                                  (ID3D11Resource *)r->tex, 0, &box);
        rt_release(r);
    }
    slot->base = base;
    slot->gw = gw;
    slot->gh = gh;
    slot->tex = tex;
    slot->used = s_frame;
    ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)tex, NULL, &slot->rtv);
    ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)tex, NULL, &slot->srv);
    if (!r) {
        float black[4] = {0, 0, 0, 1};
        ID3D11DeviceContext_ClearRenderTargetView(s_ctx, slot->rtv, black);
    }
    return slot;
}

static GpuDS *ds_get(uint32_t base, uint32_t w, uint32_t h)
{
    GpuDS *slot = NULL;
    D3D11_TEXTURE2D_DESC td;
    int i;

    for (i = 0; i < GPU_MAX_RT; i++)
        if (s_ds[i].tex && s_ds[i].base == base) {
            if (s_ds[i].w == w && s_ds[i].h == h) {
                s_ds[i].used = s_frame;
                return &s_ds[i];
            }
            slot = &s_ds[i];
            break;
        }
    if (!slot) {
        for (i = 0; i < GPU_MAX_RT && !slot; i++)
            if (!s_ds[i].tex)
                slot = &s_ds[i];
        if (!slot) {
            slot = &s_ds[0];
            for (i = 1; i < GPU_MAX_RT; i++)
                if (s_ds[i].used < slot->used)
                    slot = &s_ds[i];
        }
    }
    if (slot->dsv) ID3D11DepthStencilView_Release(slot->dsv);
    if (slot->tex) ID3D11Texture2D_Release(slot->tex);
    memset(slot, 0, sizeof(*slot));
    memset(&td, 0, sizeof(td));
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &slot->tex)))
        return NULL;
    ID3D11Device_CreateDepthStencilView(s_dev, (ID3D11Resource *)slot->tex, NULL, &slot->dsv);
    ID3D11DeviceContext_ClearDepthStencilView(s_ctx, slot->dsv,
                                              D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    slot->base = base;
    slot->w = w;
    slot->h = h;
    slot->used = s_frame;
    return slot;
}

/* The guest size a draw needs: the surface pitch's width, and at least the
 * clip rectangle. */
static void target_size(const Nv2aGpuState *st, uint32_t *gw, uint32_t *gh)
{
    uint32_t w = st->bpp ? st->pitch / st->bpp : 0;
    uint32_t h = st->clip_y + st->clip_h;
    if (w < st->clip_x + st->clip_w) w = st->clip_x + st->clip_w;
    if (!w) w = 640;
    if (!h) h = 480;
    *gw = w;
    *gh = h;
}

/* ── Textures ────────────────────────────────────────────────────────────── */

typedef struct {
    uint32_t offset, color, width, height, pitch, levels, cube, palette;
    uint64_t hash;
    uint32_t checked;                  /* frame the hash was last verified */
    uint32_t used;
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
} GpuTex;

#define GPU_MAX_TEX 1024
static GpuTex s_tex[GPU_MAX_TEX];

static uint32_t texel_bytes(uint32_t fmt)
{
    switch (fmt) {
    case 0x00: case 0x01: case 0x0B: case 0x13: case 0x19: case 0x1F:
        return 1;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x10: case 0x11:
    case 0x1C: case 0x1D: case 0x24: case 0x25:
        return 2;
    default:
        return 4;
    }
}

static int is_dxt(uint32_t fmt) { return d3d8_format_dxt_block_bytes(fmt) != 0; }
static int is_normalised(uint32_t fmt) { return d3d8_format_is_swizzled(fmt) || is_dxt(fmt); }

/* Bytes of one face, all levels, unaligned. */
static uint32_t face_bytes(const Texture *t, uint32_t levels)
{
    uint32_t bb = d3d8_format_dxt_block_bytes(t->color), lv, total = 0;
    if (!is_normalised(t->color))
        return t->pitch * t->height;
    for (lv = 0; lv < levels; lv++) {
        uint32_t w = t->width >> lv, h = t->height >> lv;
        if (!w) w = 1;
        if (!h) h = 1;
        total += bb ? ((w + 3) / 4) * ((h + 3) / 4) * bb : w * h * texel_bytes(t->color);
    }
    return total;
}

static uint64_t hash_mem(const uint8_t *p, uint32_t n, uint64_t h)
{
    uint32_t i;
    for (i = 0; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    for (; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

static uint64_t tex_hash(const Texture *t, uint32_t levels, uint32_t faces, uint32_t stride)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint64_t h = 0xCBF29CE484222325ull;
    uint32_t f, n = face_bytes(t, levels);
    for (f = 0; f < faces; f++)
        h = hash_mem(mem + t->offset + f * stride, n, h);
    if (t->color == 0x0B && t->palette)
        h = hash_mem(mem + t->palette, 1024, h);
    return h;
}

static ID3D11ShaderResourceView *tex_upload(GpuTex *e, const Texture *t, uint32_t levels,
                                            uint32_t faces, uint32_t stride)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA *sd;
    uint32_t bb = d3d8_format_dxt_block_bytes(t->color);
    int bc = bb && (t->width % 4) == 0 && (t->height % 4) == 0;
    uint32_t f, lv, n = 0;
    uint32_t *pix = NULL;
    size_t total = 0;

    if (e->srv) { ID3D11ShaderResourceView_Release(e->srv); e->srv = NULL; }
    if (e->tex) { ID3D11Texture2D_Release(e->tex); e->tex = NULL; }

    sd = (D3D11_SUBRESOURCE_DATA *)calloc((size_t)faces * levels, sizeof(*sd));
    if (!sd)
        return NULL;
    if (!bc) {
        for (lv = 0; lv < levels; lv++) {
            uint32_t w = t->width >> lv, h = t->height >> lv;
            total += (size_t)(w ? w : 1) * (h ? h : 1);
        }
        pix = (uint32_t *)malloc(total * faces * 4);
        if (!pix) { free(sd); return NULL; }
    }
    {
        size_t at = 0;
        for (f = 0; f < faces; f++) {
            uint32_t off = 0;
            for (lv = 0; lv < levels; lv++) {
                uint32_t w = t->width >> lv, h = t->height >> lv;
                if (!w) w = 1;
                if (!h) h = 1;
                if (bc) {
                    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
                    sd[n].pSysMem = mem + t->offset + f * stride + off;
                    sd[n].SysMemPitch = bw * bb;
                    sd[n].SysMemSlicePitch = bw * bh * bb;
                    off += bw * bh * bb;
                } else {
                    Texture lt = *t;
                    uint32_t x, y, *out = pix + at;
                    lt.offset = t->offset + f * stride + off;
                    lt.width = w;
                    lt.height = h;
                    for (y = 0; y < h; y++)
                        for (x = 0; x < w; x++)
                            if (!nv2a_tex_texel(&lt, 0, x, y, &out[y * w + x]))
                                out[y * w + x] = 0xFFFFFFFFu;
                    sd[n].pSysMem = out;
                    sd[n].SysMemPitch = w * 4;
                    sd[n].SysMemSlicePitch = w * h * 4;
                    at += (size_t)w * h;
                    off += bb ? ((w + 3) / 4) * ((h + 3) / 4) * bb : w * h * texel_bytes(t->color);
                }
                n++;
            }
        }
    }
    memset(&td, 0, sizeof(td));
    td.Width = t->width;
    td.Height = t->height;
    td.MipLevels = levels;
    td.ArraySize = faces;
    td.Format = !bc ? DXGI_FORMAT_B8G8R8A8_UNORM
              : bb == 8 ? DXGI_FORMAT_BC1_UNORM
              : t->color == 0x0E ? DXGI_FORMAT_BC2_UNORM : DXGI_FORMAT_BC3_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = faces == 6 ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &td, sd, &e->tex)))
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)e->tex, NULL, &e->srv);
    free(sd);
    free(pix);
    return e->srv;
}

static uint32_t face_stride(const Texture *t, uint32_t levels)
{
    return (face_bytes(t, levels) + 127u) & ~127u;
}

/* The view for texture stage `t`, uploading or refreshing it as needed.
 * *coord gets the coordinate scale: 1/size for texel-addressed (linear)
 * textures, 1 for normalised ones. */
static ID3D11ShaderResourceView *tex_get(const Texture *t, float coord[2], int *cube)
{
    GpuTex *e = NULL, *slot = NULL;
    GpuRT *rt;
    uint32_t levels, faces, stride;
    int i;

    *cube = 0;
    coord[0] = coord[1] = 1.0f;
    if (!t->valid || !t->width || !t->height)
        return NULL;
    if (!is_normalised(t->color)) {
        coord[0] = 1.0f / (float)t->width;
        coord[1] = 1.0f / (float)t->height;
    }
    /* Something the title rendered: sample the target itself. */
    rt = rt_find(t->offset);
    if (rt) {
        if (!is_normalised(t->color)) {
            coord[0] = 1.0f / (float)rt->gw;
            coord[1] = 1.0f / (float)rt->gh;
        }
        return rt->srv;
    }
    levels = is_normalised(t->color) && t->levels ? t->levels : 1;
    if (levels > 12) levels = 12;
    while (levels > 1 && ((t->width >> (levels - 1)) == 0 && (t->height >> (levels - 1)) == 0))
        levels--;
    faces = t->cube ? 6 : 1;
    stride = face_stride(t, levels);
    *cube = t->cube;

    for (i = 0; i < GPU_MAX_TEX; i++) {
        GpuTex *c = &s_tex[i];
        if (c->srv && c->offset == t->offset && c->color == t->color && c->width == t->width
            && c->height == t->height && c->pitch == t->pitch && c->levels == levels
            && c->cube == (uint32_t)t->cube && c->palette == t->palette) {
            e = c;
            break;
        }
        if (!c->srv && !slot)
            slot = c;
    }
    if (e) {
        if (e->checked != s_frame) {
            uint64_t h = tex_hash(t, levels, faces, stride);
            e->checked = s_frame;
            if (h != e->hash) {
                e->hash = h;
                tex_upload(e, t, levels, faces, stride);
            }
        }
        e->used = s_frame;
        return e->srv;
    }
    if (!slot) {
        slot = &s_tex[0];
        for (i = 1; i < GPU_MAX_TEX; i++)
            if (s_tex[i].used < slot->used)
                slot = &s_tex[i];
    }
    if (slot->srv) ID3D11ShaderResourceView_Release(slot->srv);
    if (slot->tex) ID3D11Texture2D_Release(slot->tex);
    memset(slot, 0, sizeof(*slot));
    slot->offset = t->offset; slot->color = t->color;
    slot->width = t->width; slot->height = t->height; slot->pitch = t->pitch;
    slot->levels = levels; slot->cube = (uint32_t)t->cube; slot->palette = t->palette;
    slot->hash = tex_hash(t, levels, faces, stride);
    slot->checked = slot->used = s_frame;
    return tex_upload(slot, t, levels, faces, stride);
}

/* ── State objects, cached by the NV2A values that make them ─────────────── */

static D3D11_TEXTURE_ADDRESS_MODE addr_mode(uint32_t m)
{
    switch (m) {
    case 1:  return D3D11_TEXTURE_ADDRESS_WRAP;
    case 2:  return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 4:  return D3D11_TEXTURE_ADDRESS_BORDER;
    default: return D3D11_TEXTURE_ADDRESS_CLAMP;
    }
}

static ID3D11SamplerState *sampler_get(const Texture *t, int levels_ok)
{
    static struct { uint32_t key; ID3D11SamplerState *s; } cache[64];
    static int n;
    uint32_t mag = (t->filter >> 24) & 0xF, min = (t->filter >> 16) & 0xFF;
    uint32_t key = (t->addr_u & 0xF) | (t->addr_v & 0xF) << 4 | mag << 8 | min << 12
                 | (uint32_t)levels_ok << 20;
    D3D11_SAMPLER_DESC sd;
    int i, lin_min, lin_mag, mip, lin_mip;

    for (i = 0; i < n; i++)
        if (cache[i].key == key)
            return cache[i].s;
    lin_mag = mag != 1;
    lin_min = min == 2 || min == 4 || min == 6 || min == 0;
    mip = levels_ok && min >= 3;
    lin_mip = min == 5 || min == 6;
    memset(&sd, 0, sizeof(sd));
    sd.Filter = (D3D11_FILTER)((lin_min ? 0x10 : 0) | (lin_mag ? 0x4 : 0) | (lin_mip ? 0x1 : 0));
    sd.AddressU = addr_mode(t->addr_u);
    sd.AddressV = addr_mode(t->addr_v);
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = mip ? D3D11_FLOAT32_MAX : 0.0f;
    sd.MaxAnisotropy = 1;
    if (n == 64)
        n = 0;                              /* never in practice; just recycle */
    if (cache[n].s)
        ID3D11SamplerState_Release(cache[n].s);
    cache[n].key = key;
    cache[n].s = NULL;
    ID3D11Device_CreateSamplerState(s_dev, &sd, &cache[n].s);
    return cache[n++].s;
}

static D3D11_BLEND blend_factor(uint32_t f, int alpha)
{
    switch (f) {
    case 0x0000: return D3D11_BLEND_ZERO;
    case 0x0001: return D3D11_BLEND_ONE;
    case 0x0300: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 0x0301: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 0x0302: return D3D11_BLEND_SRC_ALPHA;
    case 0x0303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x0304: return D3D11_BLEND_DEST_ALPHA;
    case 0x0305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x0306: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 0x0307: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 0x0308: return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    default:     return D3D11_BLEND_ONE;
    }
}

static D3D11_BLEND_OP blend_op(uint32_t e)
{
    switch (e) {
    case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX;
    default:     return D3D11_BLEND_OP_ADD;
    }
}

static ID3D11BlendState *blend_get(const Nv2aGpuState *st)
{
    static struct { uint64_t key; ID3D11BlendState *s; } cache[128];
    static int n;
    uint32_t mask = ((st->color_mask >> 16) & 1) | ((st->color_mask >> 7) & 2)
                  | ((st->color_mask << 2) & 4) | ((st->color_mask >> 21) & 8);
    uint64_t key = st->blend_enable
        ? ((uint64_t)(st->blend_sfactor & 0xFFFF) << 32 | (uint64_t)(st->blend_dfactor & 0xFFFF) << 16
           | (st->blend_equation & 0xFFF0) | 0x8)
        : 0;
    D3D11_BLEND_DESC bd;
    int i;

    key = key << 4 | mask;
    for (i = 0; i < n; i++)
        if (cache[i].key == key)
            return cache[i].s;
    memset(&bd, 0, sizeof(bd));
    bd.RenderTarget[0].BlendEnable = st->blend_enable ? TRUE : FALSE;
    bd.RenderTarget[0].SrcBlend = blend_factor(st->blend_sfactor, 0);
    bd.RenderTarget[0].DestBlend = blend_factor(st->blend_dfactor, 0);
    bd.RenderTarget[0].BlendOp = blend_op(st->blend_equation);
    bd.RenderTarget[0].SrcBlendAlpha = blend_factor(st->blend_sfactor, 1);
    bd.RenderTarget[0].DestBlendAlpha = blend_factor(st->blend_dfactor, 1);
    bd.RenderTarget[0].BlendOpAlpha = blend_op(st->blend_equation);
    bd.RenderTarget[0].RenderTargetWriteMask = (UINT8)mask;
    if (n == 128) n = 0;
    if (cache[n].s) ID3D11BlendState_Release(cache[n].s);
    cache[n].key = key;
    cache[n].s = NULL;
    ID3D11Device_CreateBlendState(s_dev, &bd, &cache[n].s);
    return cache[n++].s;
}

static D3D11_COMPARISON_FUNC cmp_func(uint32_t f)
{
    return (f >= 0x200 && f <= 0x207) ? (D3D11_COMPARISON_FUNC)(f - 0x200 + 1)
                                      : D3D11_COMPARISON_ALWAYS;
}

static D3D11_STENCIL_OP stencil_op(uint32_t op)
{
    switch (op) {
    case 0x0000: return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    default:     return D3D11_STENCIL_OP_KEEP;
    }
}

static ID3D11DepthStencilState *depth_get(const Nv2aGpuState *st, int have_ds)
{
    static struct { uint32_t k[4]; ID3D11DepthStencilState *s; } cache[128];
    static int n;
    uint32_t k[4];
    D3D11_DEPTH_STENCIL_DESC dd;
    int i;

    if (!have_ds)
        return s_ds_off;
    k[0] = (st->depth_test ? 1u : 0u) | (st->depth_mask ? 2u : 0u) | (st->depth_func & 0xF) << 4
         | (st->stencil_test ? 0x100u : 0u) | (st->stencil_func & 0xF) << 12;
    k[1] = (st->stencil_write_mask & 0xFF) | (st->stencil_func_mask & 0xFF) << 8;
    k[2] = (st->stencil_fail & 0xFFFF) | st->stencil_zfail << 16;
    k[3] = st->stencil_zpass;
    for (i = 0; i < n; i++)
        if (!memcmp(cache[i].k, k, sizeof(k)))
            return cache[i].s;
    memset(&dd, 0, sizeof(dd));
    dd.DepthEnable = (st->depth_test || st->depth_mask) ? TRUE : FALSE;
    dd.DepthWriteMask = st->depth_mask ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = st->depth_test ? cmp_func(st->depth_func) : D3D11_COMPARISON_ALWAYS;
    dd.StencilEnable = st->stencil_test ? TRUE : FALSE;
    dd.StencilReadMask = (UINT8)st->stencil_func_mask;
    dd.StencilWriteMask = (UINT8)st->stencil_write_mask;
    dd.FrontFace.StencilFunc = cmp_func(st->stencil_func);
    dd.FrontFace.StencilFailOp = stencil_op(st->stencil_fail);
    dd.FrontFace.StencilDepthFailOp = stencil_op(st->stencil_zfail);
    dd.FrontFace.StencilPassOp = stencil_op(st->stencil_zpass);
    dd.BackFace = dd.FrontFace;
    if (n == 128) n = 0;
    if (cache[n].s) ID3D11DepthStencilState_Release(cache[n].s);
    memcpy(cache[n].k, k, sizeof(k));
    cache[n].s = NULL;
    ID3D11Device_CreateDepthStencilState(s_dev, &dd, &cache[n].s);
    return cache[n++].s;
}

static ID3D11RasterizerState *raster_get(const Nv2aGpuState *st)
{
    static ID3D11RasterizerState *cache[8];
    /* Screen space has y pointing down, the reverse of NDC, so the winding
     * the title calls front is the one D3D sees as the other. */
    int cull = !st->cull_enable ? 0 : st->cull_face == 0x404 ? 1 : st->cull_face == 0x405 ? 2 : 0;
    int ccw = st->front_face == 0x901;
    int k = cull * 2 + ccw;
    if (!cache[k]) {
        D3D11_RASTERIZER_DESC rd;
        memset(&rd, 0, sizeof(rd));
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = cull == 1 ? D3D11_CULL_FRONT : cull == 2 ? D3D11_CULL_BACK : D3D11_CULL_NONE;
        rd.FrontCounterClockwise = ccw ? FALSE : TRUE;
        rd.DepthClipEnable = TRUE;
        rd.ScissorEnable = TRUE;
        ID3D11Device_CreateRasterizerState(s_dev, &rd, &cache[k]);
    }
    return cache[k];
}

/* ── Drawing ─────────────────────────────────────────────────────────────── */

static void unpack_argb(uint32_t c, float o[4])
{
    o[0] = (float)((c >> 16) & 0xFF) / 255.0f;
    o[1] = (float)((c >> 8) & 0xFF) / 255.0f;
    o[2] = (float)(c & 0xFF) / 255.0f;
    o[3] = (float)(c >> 24) / 255.0f;
}

/* Bind the target (and depth) the state names; returns it. */
static GpuRT *bind_target(const Nv2aGpuState *st, int want_depth, GpuDS **ds_out)
{
    uint32_t gw, gh;
    GpuRT *rt;
    GpuDS *ds = NULL;
    D3D11_VIEWPORT vp;
    D3D11_RECT sc;

    target_size(st, &gw, &gh);
    rt = rt_get(st->color_base, gw, gh);
    if (!rt)
        return NULL;
    if (want_depth && st->zeta_base)
        ds = ds_get(st->zeta_base, rt->gw * (uint32_t)s_scale, rt->gh * (uint32_t)s_scale);
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &rt->rtv, ds ? ds->dsv : NULL);
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    vp.Width = (float)(rt->gw * (uint32_t)s_scale);
    vp.Height = (float)(rt->gh * (uint32_t)s_scale);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    sc.left = (LONG)(st->clip_x * (uint32_t)s_scale);
    sc.top = (LONG)(st->clip_y * (uint32_t)s_scale);
    sc.right = (LONG)((st->clip_x + st->clip_w) * (uint32_t)s_scale);
    sc.bottom = (LONG)((st->clip_y + st->clip_h) * (uint32_t)s_scale);
    ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, &sc);
    if (ds_out)
        *ds_out = ds;
    return rt;
}

void nv2a_gpu_draw(const Nv2aGpuState *st, const Nv2aGpuVertex *v,
                   uint32_t nv, const uint32_t *idx, uint32_t nidx)
{
    D3D11_MAPPED_SUBRESOURCE m;
    GpuRT *rt;
    GpuDS *ds = NULL;
    ID3D11ShaderResourceView *srv[8];
    ID3D11SamplerState *smp[4];
    UINT vbytes = nv * (UINT)sizeof(Nv2aGpuVertex), ibytes = nidx * 4u, stride, offset;
    int s, need_depth;

    static int dbg = -1;

    if (s_state != 1 || !nv || !nidx || vbytes > GPU_VB_BYTES || ibytes > GPU_IB_BYTES)
        return;
    /* RECOMP_GPU_DEBUG: bit 0 no depth/stencil, bit 1 no culling, bits 4-5
     * what the pixel shader outputs (1 red, 2 diffuse, 3 texture 0). */
    if (dbg < 0)
        dbg = getenv("RECOMP_GPU_DEBUG") ? (int)strtol(getenv("RECOMP_GPU_DEBUG"), NULL, 0) : 0;
    EnterCriticalSection(&s_lock);
    need_depth = !(dbg & 1) && (st->depth_test || st->depth_mask || st->stencil_test);
    rt = bind_target(st, need_depth, &ds);
    if (!rt) {
        s_stat.no_rt++;
        LeaveCriticalSection(&s_lock);
        return;
    }

    /* Vertices and indices into the ring buffers. */
    {
        D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
        if (s_vb_pos == 0xFFFFFFFFu || s_vb_pos + vbytes > GPU_VB_BYTES) { s_vb_pos = 0; mode = D3D11_MAP_WRITE_DISCARD; }
        if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vb, 0, mode, 0, &m))) {
            s_stat.map_fail++;
            LeaveCriticalSection(&s_lock);
            return;
        }
        memcpy((uint8_t *)m.pData + s_vb_pos, v, vbytes);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vb, 0);
        mode = D3D11_MAP_WRITE_NO_OVERWRITE;
        if (s_ib_pos == 0xFFFFFFFFu || s_ib_pos + ibytes > GPU_IB_BYTES) { s_ib_pos = 0; mode = D3D11_MAP_WRITE_DISCARD; }
        if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_ib, 0, mode, 0, &m))) {
            s_stat.map_fail++;
            LeaveCriticalSection(&s_lock);
            return;
        }
        memcpy((uint8_t *)m.pData + s_ib_pos, idx, ibytes);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_ib, 0);
    }

    /* Textures, then the constants that describe them. */
    {
        DrawCB cb;
        uint32_t i;
        memset(&cb, 0, sizeof(cb));
        cb.vp[0] = (float)rt->gw;
        cb.vp[1] = (float)rt->gh;
        cb.vp[2] = st->zmax > 0.0f ? 1.0f / st->zmax : 1.0f / 16777215.0f;
        cb.vp[3] = (float)((dbg >> 4) & 3);
        cb.ctl[0] = st->rc.control;
        cb.ctl[1] = st->rc.stage_program;
        cb.ctl[2] = st->rc.final0;
        cb.ctl[3] = st->rc.final1;
        for (i = 0; i < 8; i++) {
            cb.cw[i][0] = st->rc.color_icw[i];
            cb.cw[i][1] = st->rc.alpha_icw[i];
            cb.cw[i][2] = st->rc.color_ocw[i];
            cb.cw[i][3] = st->rc.alpha_ocw[i];
            unpack_argb(st->rc.factor0[i], cb.fac0[i]);
            unpack_argb(st->rc.factor1[i], cb.fac1[i]);
        }
        unpack_argb(st->rc.final_c0, cb.fin_c0);
        unpack_argb(st->rc.final_c1, cb.fin_c1);
        cb.fogc[0] = (float)(st->fog_color & 0xFF) / 255.0f;   /* R in bits 0-7 */
        cb.fogc[1] = (float)((st->fog_color >> 8) & 0xFF) / 255.0f;
        cb.fogc[2] = (float)((st->fog_color >> 16) & 0xFF) / 255.0f;
        cb.fogc[3] = (float)(st->fog_color >> 24) / 255.0f;
        cb.misc[0] = st->rc_seen ? 1u : 0u;
        cb.misc[1] = st->alpha_test ? (st->alpha_func ? st->alpha_func : 0x207u) : 0u;
        cb.misc[2] = st->alpha_ref;
        cb.misc[3] = st->clip_plane_mode;
        for (s = 0; s < 4; s++) {
            uint32_t mode = (st->rc.stage_program >> (s * 5)) & 0x1F;
            int cube = 0;
            float coord[2] = {1.0f, 1.0f};
            ID3D11ShaderResourceView *view = NULL;
            if (mode && st->tex[s].valid)
                view = tex_get(&st->tex[s], coord, &cube);
            /* Without combiners, stage 0 is what the fallback samples. */
            if (!view && !st->rc_seen && s == 0 && st->tex[0].valid) {
                view = tex_get(&st->tex[0], coord, &cube);
                if (view)
                    cb.ctl[1] |= 1;
            }
            srv[s] = (view && !cube) ? view : s_white2d;
            srv[4 + s] = (view && cube) ? view : s_whitecube;
            smp[s] = sampler_get(&st->tex[s], st->tex[s].levels > 1);
            cb.tsz[s][0] = coord[0];
            cb.tsz[s][1] = coord[1];
            cb.tsz[s][2] = view ? 1.0f : 0.0f;
            cb.tsz[s][3] = cube ? 1.0f : 0.0f;
        }
        if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_cb, 0,
                                           D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            LeaveCriticalSection(&s_lock);
            return;
        }
        memcpy(m.pData, &cb, sizeof(cb));
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_cb, 0);
    }
    /* A target sampled while it is bound for drawing reads nothing in D3D;
     * the title does that only for effects that read the previous frame,
     * so draw without it rather than not at all. */
    for (s = 0; s < 8; s++)
        if (srv[s] == rt->srv)
            srv[s] = (s < 4) ? s_white2d : s_whitecube;

    stride = sizeof(Nv2aGpuVertex);
    offset = s_vb_pos;
    ID3D11DeviceContext_IASetInputLayout(s_ctx, s_layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vb, &stride, &offset);
    ID3D11DeviceContext_IASetIndexBuffer(s_ctx, s_ib, DXGI_FORMAT_R32_UINT, s_ib_pos);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &s_cb);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_cb);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 8, srv);
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 4, smp);
    if (dbg & 2) {
        Nv2aGpuState nc = *st;
        nc.cull_enable = 0;
        ID3D11DeviceContext_RSSetState(s_ctx, raster_get(&nc));
    } else {
        ID3D11DeviceContext_RSSetState(s_ctx, raster_get(st));
    }
    {
        float bf[4];
        unpack_argb(st->blend_color, bf);
        ID3D11DeviceContext_OMSetBlendState(s_ctx, blend_get(st), bf, 0xFFFFFFFFu);
    }
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, depth_get(st, ds != NULL),
                                               st->stencil_ref & 0xFF);
    ID3D11DeviceContext_DrawIndexed(s_ctx, nidx, 0, 0);
    s_stat.draws++;
    s_vb_pos += (vbytes + 255u) & ~255u;
    s_ib_pos += (ibytes + 255u) & ~255u;
    LeaveCriticalSection(&s_lock);
}

void nv2a_gpu_clear(const Nv2aGpuState *st, uint32_t param,
                    uint32_t color, uint32_t zstencil)
{
    GpuRT *rt;
    GpuDS *ds = NULL;
    int want_ds = (param & 3) != 0;

    if (s_state != 1)
        return;
    EnterCriticalSection(&s_lock);
    rt = bind_target(st, want_ds, &ds);
    s_stat.cleared++;
    if (rt && (param & 0xF0)) {
        float c[4];
        int whole = st->clip_x == 0 && st->clip_y == 0
                 && st->clip_x + st->clip_w >= rt->gw && st->clip_y + st->clip_h >= rt->gh;
        unpack_argb(color, c);
        if (whole || !s_ctx1) {
            ID3D11DeviceContext_ClearRenderTargetView(s_ctx, rt->rtv, c);
        } else {
            D3D11_RECT r;
            r.left = (LONG)(st->clip_x * (uint32_t)s_scale);
            r.top = (LONG)(st->clip_y * (uint32_t)s_scale);
            r.right = (LONG)((st->clip_x + st->clip_w) * (uint32_t)s_scale);
            r.bottom = (LONG)((st->clip_y + st->clip_h) * (uint32_t)s_scale);
            ID3D11DeviceContext1_ClearView(s_ctx1, (ID3D11View *)rt->rtv, c, &r, 1);
        }
    }
    if (ds) {
        UINT flags = ((param & 1) ? D3D11_CLEAR_DEPTH : 0) | ((param & 2) ? D3D11_CLEAR_STENCIL : 0);
        float z = st->zmax <= 65535.0f ? (float)(zstencil & 0xFFFF) / 65535.0f
                                       : (float)(zstencil >> 8) / 16777215.0f;
        ID3D11DeviceContext_ClearDepthStencilView(s_ctx, ds->dsv, flags, z, (UINT8)(zstencil & 0xFF));
    }
    LeaveCriticalSection(&s_lock);
}

/* ── Presenting ──────────────────────────────────────────────────────────── */

static ID3D11Texture2D *s_shown;          /* the last finished frame */
static ID3D11ShaderResourceView *s_shown_srv;
static uint32_t s_shown_w, s_shown_h, s_shown_version;

static ID3D11Texture2D *s_ovtex;
static ID3D11ShaderResourceView *s_ovsrv;
static uint32_t s_ov_w, s_ov_h, s_ov_version_up = 0xFFFFFFFFu;

static IDXGISwapChain *s_swap;
static ID3D11RenderTargetView *s_back;
static HWND s_swap_hwnd;
static UINT s_swap_w, s_swap_h;

void nv2a_gpu_flip(uint32_t color_base)
{
    GpuRT *rt;

    if (s_state != 1)
        return;
    EnterCriticalSection(&s_lock);
    s_frame++;
    rt = rt_find(color_base);
    if (rt) s_stat.flips_found++; else s_stat.flips_missing++;
    if (s_frame % 300 == 0) {
        int i, nrt = 0;
        for (i = 0; i < GPU_MAX_RT; i++) if (s_rt[i].tex) nrt++;
        GPU_LOG("frame %u: %u draws, %u without a target, %u map failures, %u clears;"
                " flips %u shown / %u with no target (shown %08X); %d targets\n",
                s_frame, s_stat.draws, s_stat.no_rt, s_stat.map_fail, s_stat.cleared,
                s_stat.flips_found, s_stat.flips_missing, color_base, nrt);
        memset(&s_stat, 0, sizeof(s_stat));
    }
    if (rt) {
        uint32_t w = rt->gw * (uint32_t)s_scale, h = rt->gh * (uint32_t)s_scale;
        if (!s_shown || s_shown_w != w || s_shown_h != h) {
            D3D11_TEXTURE2D_DESC td;
            if (s_shown_srv) ID3D11ShaderResourceView_Release(s_shown_srv);
            if (s_shown) ID3D11Texture2D_Release(s_shown);
            s_shown = NULL;
            s_shown_srv = NULL;
            memset(&td, 0, sizeof(td));
            td.Width = w;
            td.Height = h;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_shown)))
                ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_shown, NULL,
                                                      &s_shown_srv);
            s_shown_w = w;
            s_shown_h = h;
        }
        if (s_shown) {
            ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)s_shown,
                                             (ID3D11Resource *)rt->tex);
            s_shown_version++;
        }
    }
    LeaveCriticalSection(&s_lock);
}

static int swap_ready(HWND hwnd, UINT w, UINT h)
{
    if (s_swap && s_swap_hwnd == hwnd && s_swap_w == w && s_swap_h == h)
        return 1;
    if (s_back) { ID3D11RenderTargetView_Release(s_back); s_back = NULL; }
    if (s_swap && s_swap_hwnd == hwnd) {
        if (FAILED(IDXGISwapChain_ResizeBuffers(s_swap, 0, w, h, DXGI_FORMAT_UNKNOWN, 0)))
            return 0;
    } else {
        IDXGIDevice *dxd = NULL;
        IDXGIAdapter *ad = NULL;
        IDXGIFactory *fac = NULL;
        DXGI_SWAP_CHAIN_DESC sd;
        HRESULT hr;
        if (s_swap) { IDXGISwapChain_Release(s_swap); s_swap = NULL; }
        ID3D11Device_QueryInterface(s_dev, &IID_IDXGIDevice, (void **)&dxd);
        if (dxd) IDXGIDevice_GetAdapter(dxd, &ad);
        if (ad) IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory, (void **)&fac);
        if (!fac) {
            if (ad) IDXGIAdapter_Release(ad);
            if (dxd) IDXGIDevice_Release(dxd);
            return 0;
        }
        memset(&sd, 0, sizeof(sd));
        sd.BufferDesc.Width = w;
        sd.BufferDesc.Height = h;
        sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.OutputWindow = hwnd;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        hr = IDXGIFactory_CreateSwapChain(fac, (IUnknown *)s_dev, &sd, &s_swap);
        if (FAILED(hr)) {
            sd.BufferCount = 1;
            sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            hr = IDXGIFactory_CreateSwapChain(fac, (IUnknown *)s_dev, &sd, &s_swap);
        }
        IDXGIFactory_MakeWindowAssociation(fac, hwnd, DXGI_MWA_NO_ALT_ENTER);
        IDXGIFactory_Release(fac);
        IDXGIAdapter_Release(ad);
        IDXGIDevice_Release(dxd);
        if (FAILED(hr)) {
            GPU_LOG("CreateSwapChain failed (0x%08lX)\n", (unsigned long)hr);
            s_swap = NULL;
            return 0;
        }
        s_swap_hwnd = hwnd;
    }
    {
        ID3D11Texture2D *bb = NULL;
        IDXGISwapChain_GetBuffer(s_swap, 0, &IID_ID3D11Texture2D, (void **)&bb);
        if (!bb)
            return 0;
        ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)bb, NULL, &s_back);
        ID3D11Texture2D_Release(bb);
    }
    s_swap_w = w;
    s_swap_h = h;
    return s_back != NULL;
}

/* Largest num:den rectangle in w x h, centred. */
static void fit(UINT w, UINT h, UINT num, UINT den, float r[4])
{
    float dw, dh;
    if ((unsigned long long)w * den > (unsigned long long)h * num) {
        dh = (float)h; dw = (float)h * num / den;
    } else {
        dw = (float)w; dh = (float)w * den / num;
    }
    r[0] = ((float)w - dw) * 0.5f;
    r[1] = ((float)h - dh) * 0.5f;
    r[2] = r[0] + dw;
    r[3] = r[1] + dh;
}

static void blit(ID3D11ShaderResourceView *srv, const float px[4], UINT w, UINT h)
{
    D3D11_MAPPED_SUBRESOURCE m;
    float ndc[4];
    ndc[0] = px[0] / (float)w * 2.0f - 1.0f;
    ndc[1] = 1.0f - px[1] / (float)h * 2.0f;
    ndc[2] = px[2] / (float)w * 2.0f - 1.0f;
    ndc[3] = 1.0f - px[3] / (float)h * 2.0f;
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_cb_blit, 0,
                                       D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return;
    memcpy(m.pData, ndc, sizeof(ndc));
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_cb_blit, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 1, 1, &s_cb_blit);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &srv);
    ID3D11DeviceContext_Draw(s_ctx, 4, 0);
}

static void ov_upload(const XboxPresentInfo *pi)
{
    D3D11_MAPPED_SUBRESOURCE m;
    uint32_t y;
    if (!s_ovtex || s_ov_w != pi->ov_w || s_ov_h != pi->ov_h) {
        D3D11_TEXTURE2D_DESC td;
        if (s_ovsrv) ID3D11ShaderResourceView_Release(s_ovsrv);
        if (s_ovtex) ID3D11Texture2D_Release(s_ovtex);
        s_ovtex = NULL;
        s_ovsrv = NULL;
        memset(&td, 0, sizeof(td));
        td.Width = pi->ov_w;
        td.Height = pi->ov_h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_ovtex)))
            return;
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_ovtex, NULL, &s_ovsrv);
        s_ov_w = pi->ov_w;
        s_ov_h = pi->ov_h;
    }
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_ovtex, 0,
                                       D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return;
    for (y = 0; y < pi->ov_h; y++)
        memcpy((uint8_t *)m.pData + (size_t)y * m.RowPitch, pi->ov + (size_t)y * pi->ov_w,
               (size_t)pi->ov_w * 4);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_ovtex, 0);
    s_ov_version_up = pi->ov_version;
}

/* RECOMP_GPU_SHOT=<file>: when the file exists, the next presented image
 * is written to <file>.bmp and the file removed -- what the window shows,
 * read from the GPU, for when a screen grab of a D3D window comes out black. */
static void shot_maybe(UINT w, UINT h)
{
    static const char *flag = (const char *)-1;
    ID3D11Texture2D *bb = NULL, *st = NULL;
    D3D11_TEXTURE2D_DESC td;
    D3D11_MAPPED_SUBRESOURCE m;
    char path[MAX_PATH];
    FILE *f;
    UINT y;

    if (flag == (const char *)-1)
        flag = getenv("RECOMP_GPU_SHOT");
    if (!flag || GetFileAttributesA(flag) == INVALID_FILE_ATTRIBUTES)
        return;
    DeleteFileA(flag);
    IDXGISwapChain_GetBuffer(s_swap, 0, &IID_ID3D11Texture2D, (void **)&bb);
    if (!bb)
        return;
    ID3D11Texture2D_GetDesc(bb, &td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &st))) {
        ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)st, (ID3D11Resource *)bb);
        if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
            snprintf(path, sizeof(path), "%s.bmp", flag);
            f = fopen(path, "wb");
            if (f) {
                uint32_t hdr[13], size = w * h * 4;
                uint16_t magic = 0x4D42;
                memset(hdr, 0, sizeof(hdr));
                hdr[0] = 54 + size; hdr[2] = 54; hdr[3] = 40;
                hdr[4] = w; hdr[5] = (uint32_t)-(int32_t)h;
                hdr[6] = 1 | (32 << 16); hdr[8] = size;
                fwrite(&magic, 2, 1, f);
                fwrite(hdr, 4, 13, f);
                for (y = 0; y < h; y++)
                    fwrite((uint8_t *)m.pData + (size_t)y * m.RowPitch, 4, w, f);
                fclose(f);
            }
            ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)st, 0);
        }
        ID3D11Texture2D_Release(st);
    }
    ID3D11Texture2D_Release(bb);
}

static int gpu_present(void *hwnd_, const XboxPresentInfo *pi)
{
    static uint32_t last_frame = 0xFFFFFFFFu, last_ov = 0xFFFFFFFFu;
    static int last_ov_on = -1;
    HWND hwnd = (HWND)hwnd_;
    RECT c;
    UINT w, h;
    float img[4], r[4];
    D3D11_VIEWPORT vp;
    float black[4] = {0, 0, 0, 1};

    if (s_state != 1)
        return 0;
    GetClientRect(hwnd, &c);
    w = (UINT)(c.right - c.left);
    h = (UINT)(c.bottom - c.top);
    if (!w || !h)
        return 1;
    EnterCriticalSection(&s_lock);
    if (s_shown_version == last_frame && pi->ov_version == last_ov && pi->ov_on == last_ov_on
        && s_swap && s_swap_w == w && s_swap_h == h) {
        LeaveCriticalSection(&s_lock);
        return 1;
    }
    if (!swap_ready(hwnd, w, h)) {
        LeaveCriticalSection(&s_lock);
        return 0;
    }
    last_frame = s_shown_version;
    last_ov = pi->ov_version;
    last_ov_on = pi->ov_on;

    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s_back, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s_back, black);
    vp.TopLeftX = vp.TopLeftY = 0;
    vp.Width = (float)w;
    vp.Height = (float)h;
    vp.MinDepth = 0;
    vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(s_ctx, s_rs_blit);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, s_bs_opaque, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, s_ds_off, 0);
    ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_vs_blit, NULL, 0);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_ps_blit, NULL, 0);
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, &s_blit_sampler);

    if (pi->aspect == 1) {
        img[0] = 0; img[1] = 0; img[2] = (float)w; img[3] = (float)h;
    } else if (pi->aspect == 2) {
        fit(w, h, 16, 9, img);
    } else {
        fit(w, h, pi->fb_w, pi->fb_h, img);
    }
    {
        int full = pi->ov_on && pi->ov_x <= 0 && pi->ov_y <= 0
                && pi->ov_x + pi->ov_dw >= (int)pi->fb_w && pi->ov_y + pi->ov_dh >= (int)pi->fb_h;
        if (!(full && pi->aspect == 2) && s_shown_srv)
            blit(s_shown_srv, img, w, h);
        if (pi->ov_on && pi->ov && pi->ov_w && pi->ov_h) {
            float sx = (img[2] - img[0]) / (float)pi->fb_w, sy = (img[3] - img[1]) / (float)pi->fb_h;
            if (pi->ov_version != s_ov_version_up || !s_ovsrv)
                ov_upload(pi);
            if (full && pi->aspect == 2) {
                /* A movie is 4:3: centred in the 16:9 picture, black beside it. */
                float iw = img[2] - img[0], ih = img[3] - img[1];
                fit((UINT)iw, (UINT)ih, 4, 3, r);
                r[0] += img[0]; r[2] += img[0]; r[1] += img[1]; r[3] += img[1];
            } else {
                float x0 = (float)pi->ov_x, x1 = (float)(pi->ov_x + pi->ov_dw);
                if (pi->aspect == 2) {          /* anamorphic: squeeze about the centre */
                    float cx = (float)pi->fb_w * 0.5f;
                    x0 = cx + (x0 - cx) * 0.75f;
                    x1 = cx + (x1 - cx) * 0.75f;
                }
                r[0] = img[0] + x0 * sx;
                r[2] = img[0] + x1 * sx;
                r[1] = img[1] + (float)pi->ov_y * sy;
                r[3] = img[1] + (float)(pi->ov_y + pi->ov_dh) * sy;
            }
            if (s_ovsrv)
                blit(s_ovsrv, r, w, h);
        }
    }
    {
        ID3D11ShaderResourceView *none = NULL;
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &none);
    }
    shot_maybe(w, h);
    IDXGISwapChain_Present(s_swap, 0, 0);
    LeaveCriticalSection(&s_lock);
    return 1;
}

#else  /* !_WIN32 */

#include "nv2a_gpu.h"
int  nv2a_gpu_enabled(void) { return 0; }
void nv2a_gpu_draw(const Nv2aGpuState *st, const Nv2aGpuVertex *v, uint32_t nv,
                   const uint32_t *idx, uint32_t nidx)
{ (void)st; (void)v; (void)nv; (void)idx; (void)nidx; }
void nv2a_gpu_clear(const Nv2aGpuState *st, uint32_t param, uint32_t color, uint32_t zs)
{ (void)st; (void)param; (void)color; (void)zs; }
void nv2a_gpu_flip(uint32_t color_base) { (void)color_base; }

#endif
