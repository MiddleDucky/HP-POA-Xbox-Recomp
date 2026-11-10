// The title's Direct3D 8 calls drawn with Direct3D 12
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <map>
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <set>

extern "C" {
#include "video/video_player.h"
}

#include <imgui.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>

#include "app/app.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/pipeline_cache.h"
#include "gpu/shaders.h"
#include "gpu/smaa_shader.h"
#include "gpu/smaa_textures.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"

using Microsoft::WRL::ComPtr;

namespace gpu {

namespace {

// xboxrecomp: a pending capture burst (RequestCaptureBurst), taken by the next Present
std::mutex g_burst_mu;
std::string g_burst_dir;
int g_burst_frames = 0;

bool TakeBurst(std::filesystem::path& dir, int& frames) {
    std::lock_guard<std::mutex> l(g_burst_mu);
    if (!g_burst_frames) return false;
    dir = std::filesystem::path(g_burst_dir);
    frames = g_burst_frames;
    g_burst_frames = 0;
    return true;
}

constexpr int kFrames = 3;
constexpr uint64_t kUploadSize = 64ull << 20;
// a draw starting with less than this left in the frame's ring waits for the GPU and starts the ring over
constexpr uint64_t kUploadReserve = 8ull << 20;
constexpr uint32_t kSrvCount = 3 * 32768;
constexpr uint32_t kSamplerCount = 2048;
constexpr uint32_t kRtvCount = 1024;
constexpr uint32_t kDsvCount = 256;
// post pass views after the back buffers': bloom pair, exposure, ambient occlusion pair, SMAA's three, HDR10
// output's frame and panel
constexpr uint32_t kBloomRtv = 0;
constexpr uint32_t kExposureRtv = 2;
constexpr uint32_t kAoRtv = 3;
constexpr uint32_t kSmaaRtv = 5;
constexpr uint32_t kOutRtv = 8;
constexpr uint32_t kPostRtvs = 10;
constexpr uint32_t kConsoleShadows = 512;  // the shadow resolution setting that keeps the console's look
constexpr DXGI_FORMAT kBgra = DXGI_FORMAT_B8G8R8A8_UNORM;
// Shader model 3.0 HDR: the scene target holds colour over kHeadroom so lights add past 1 while alpha, which the
// game's blends use as a factor, keeps its 0..1 clamp; tone mapping brings it back when the frame is read
constexpr DXGI_FORMAT kHdrFormat = DXGI_FORMAT_R16G16B16A16_UNORM;
constexpr float kHeadroom = 8.0f;
// HDR10 output: back buffers in PQ over BT.2020; SDR white and peak in nits when Windows can't tell
constexpr DXGI_FORMAT kHdr10Format = DXGI_FORMAT_R10G10B10A2_UNORM;
constexpr float kDefaultPaperNits = 200.0f;
constexpr float kDefaultPeakNits = 1000.0f;
constexpr float kSdrWhiteUnit = 0.08f;  // Windows' SDR white level: 1000 is 80 nits
// longest a frame missing draws still on the build threads keeps the last whole frame on screen instead
constexpr auto kHoldMax = std::chrono::milliseconds(50);

const char* kBlitHlsl = R"(
cbuffer B : register(b1) { float4 uvrect; float4 opts; float4 cb0; float4 cb1; float4 cb2; float4 grade; float4 sharp; };
Texture2D img : register(t0);
Texture2D lut : register(t1);
SamplerState ls : register(s0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uvrect.xy + t * uvrect.zw;
    return o;
}
float4 psmain(O i) : SV_Target {
    float4 c = 0;
    int n = max((int)opts.z, 1);
    float2 px = fwidth(i.uv);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) c += img.SampleLevel(ls, i.uv + px * ((float2(x, y) + 0.5) / n - 0.5), 0);
    c /= n * n;
    // xboxrecomp: contrast adaptive sharpening (after AMD's CAS): the four neighbours a source texel or an output
    // pixel away, whichever is wider, weighted by how much room the local contrast leaves; sharp.x the amount
    if (sharp.x > 0) {
        float w, h;
        img.GetDimensions(w, h);
        float2 d = max(px, 1 / float2(w, h));
        float3 a = img.SampleLevel(ls, i.uv - float2(0, d.y), 0).rgb, b = img.SampleLevel(ls, i.uv - float2(d.x, 0), 0).rgb;
        float3 e = img.SampleLevel(ls, i.uv + float2(d.x, 0), 0).rgb, f = img.SampleLevel(ls, i.uv + float2(0, d.y), 0).rgb;
        float3 mn = min(c.rgb, min(min(a, b), min(e, f))), mx = max(c.rgb, max(max(a, b), max(e, f)));
        float3 amp = sqrt(saturate(min(mn, 2 - mx) / max(mx, 1e-4)));
        float3 k = amp * -lerp(0.125, 0.2, sharp.x);
        c.rgb = saturate((c.rgb + (a + b + e + f) * k) / (1 + 4 * k));
    }
    // xboxrecomp: colour grading: grade.x saturation, grade.y vibrance (dull colours more), grade.z warmth (-1..1)
    if (grade.w > 0) {
        float l = dot(c.rgb, float3(0.2126, 0.7152, 0.0722));
        float chroma = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
        float s = grade.x * (1 + grade.y * (1 - saturate(chroma * 2)));
        c.rgb = saturate(lerp(l.xxx, c.rgb, s));
        c.rgb = saturate(c.rgb * (1 + grade.z * float3(0.08, 0, -0.08)));
    }
    // xboxrecomp: vignette (sharp.y) over the shown picture's corners, film grain (sharp.z) moving each frame (sharp.w)
    if (sharp.y > 0 || sharp.z > 0) {
        float2 t = (i.uv - uvrect.xy) / uvrect.zw - 0.5;
        t.x *= uvrect.z / uvrect.w * 1.333;
        c.rgb *= 1 - sharp.y * 0.6 * smoothstep(0.35, 0.95, length(t) * 1.25);
        float2 g = floor(i.p.xy) + frac(sharp.w * float2(0.618034, 0.414214)) * 1000;
        float noise = frac(sin(dot(g, float2(12.9898, 78.233))) * 43758.5453) - 0.5;
        c.rgb = saturate(c.rgb + noise * sharp.z * 0.12 * (1 - abs(dot(c.rgb, 1.0 / 3) * 2 - 1) * 0.5));
    }
    c.rgb *= opts.w;
    // xboxrecomp: a colour-blind correction (cb0..2 the rows of a linear RGB matrix, cb0.w on), before the ramp
    if (cb0.w > 0) {
        float3 l = pow(saturate(c.rgb), 2.2);
        l = float3(dot(cb0.xyz, l), dot(cb1.xyz, l), dot(cb2.xyz, l));
        c.rgb = pow(saturate(l), 1 / 2.2);
    }
    if (opts.x > 0) c.rgb = float3(lut.Sample(ps, float2(c.r, 0.5)).r, lut.Sample(ps, float2(c.g, 0.5)).g, lut.Sample(ps, float2(c.b, 0.5)).b);
    return float4(c.rgb, opts.y > 0 ? c.a : 1);
}
)";

// the HDR resolve: exposure, bloom of what passes white, tone mapping
const char* kPostHlsl = R"(
cbuffer P : register(b1) { float4 p0; float4 p1; };
Texture2D src : register(t0);
Texture2D bloom : register(t1);
Texture2D<float> expo : register(t2);
SamplerState ls : register(s0);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = t;
    return o;
}
static const float3 kLum = float3(0.2126, 0.7152, 0.0722);
// p0: headroom, key, lowest and highest exposure; the log average luminance over a grid
float exposure(O i) : SV_Target {
    float s = 0;
    for (int y = 0; y < 18; y++)
        for (int x = 0; x < 32; x++)
            s += log(max(dot(src.SampleLevel(ls, (float2(x, y) + 0.5) / float2(32, 18), 0).rgb * p0.x, kLum), 1e-3));
    return clamp(p0.y / exp(s / 576), p0.z, p0.w);
}
// p1: source texel; a quarter size copy of what passes 1 after exposure
float4 bright(O i) : SV_Target {
    float3 c = 0;
    for (int k = 0; k < 4; k++) c += src.SampleLevel(ls, i.uv + p1.xy * float2(k & 1 ? 1 : -1, k & 2 ? 1 : -1), 0).rgb;
    return float4(max(c * 0.25 * p0.x * expo.Load(int3(0, 0, 0)) - 1, 0), 1);
}
// xboxrecomp: the scene bloom's (no headroom: the console's colours end at white). p0.x the knee; what passes it,
// rising softly from it, at a quarter size
float4 lbright(O i) : SV_Target {
    float3 c = 0;
    for (int k = 0; k < 4; k++) c += src.SampleLevel(ls, i.uv + p1.xy * float2(k & 1 ? 1 : -1, k & 2 ? 1 : -1), 0).rgb;
    c *= 0.25;
    float l = max(c.r, max(c.g, c.b)), t = saturate((l - p0.x) / (1 - p0.x));
    return float4(c * t * t, 1);
}
// p1: step; 9 gaussian taps in 5 bilinear reads
float4 blur(O i) : SV_Target {
    float3 c = src.SampleLevel(ls, i.uv, 0).rgb * 0.227027;
    c += (src.SampleLevel(ls, i.uv + p1.xy * 1.384615, 0).rgb + src.SampleLevel(ls, i.uv - p1.xy * 1.384615, 0).rgb) * 0.316216;
    c += (src.SampleLevel(ls, i.uv + p1.xy * 3.230769, 0).rgb + src.SampleLevel(ls, i.uv - p1.xy * 3.230769, 0).rgb) * 0.070270;
    return float4(c, 1);
}
// p0: headroom, tone mapping, bloom strength; linear up to the knee, highlights roll off towards white
float4 tone(O i) : SV_Target {
    float4 s = src.Load(int3(i.p.xy, 0));
    float3 c = s.rgb * p0.x, b = bloom.SampleLevel(ls, i.uv, 0).rgb * p0.z;
    if (p0.y > 0) {
        const float a = 0.75;
        c = c * expo.Load(int3(0, 0, 0)) + b;
        c = min(c, a) + (1 - a) * (1 - exp(min(a - c, 0) / (1 - a)));
    } else {
        c += b;
    }
    return float4(saturate(c), s.a);
}
)";

// HDR10 output: the frame with the panel over it, its highlights stretched towards the display's peak
const char* kHdrHlsl = R"(
cbuffer P : register(b1) { float4 p0; float4 p1; };
Texture2D frame : register(t0);
Texture2D ui : register(t1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = t;
    return o;
}
static const float3 kLum = float3(0.2126, 0.7152, 0.0722);
static const float3x3 k709To2020 = {0.6274, 0.3293, 0.0433, 0.0691, 0.9195, 0.0114, 0.0164, 0.0880, 0.8956};
// gamma 2.2 as SDR monitors show it; sRGB's linear toe lifts the darks
float3 to_linear(float3 c) {
    return pow(c, 2.2);
}
// inverse Reinhard on luminance so hues keep: the brighter, the more it gains, white reaches most
float3 expand(float3 c, float most) {
    return c / (1 - dot(c, kLum) * (1 - 1 / most));
}
float3 pq(float3 nits) {
    float3 y = pow(saturate(nits / 10000), 0.1593017578125);
    return pow((0.8359375 + 18.8515625 * y) / (1 + 18.6875 * y), 78.84375);
}
// p0: SDR white nits, peak nits
float4 hdr10(O i) : SV_Target {
    int3 px = int3(i.p.xy, 0);
    float3 f = min(expand(to_linear(frame.Load(px).rgb), max(p0.y / p0.x, 1)) * p0.x, p0.y);
    float4 u = ui.Load(px);
    float3 panel = to_linear(u.a > 0 ? u.rgb / u.a : 0) * p0.x;
    return float4(pq(mul(k709To2020, lerp(f, panel, u.a))), 1);
}
)";

// the output showing monitor mon, false when no adapter has it
bool FindOutput(IDXGIFactory4* factory, HMONITOR mon, DXGI_OUTPUT_DESC1& desc) {
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; a++) {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; o++) {
            ComPtr<IDXGIOutput6> output6;
            if (FAILED(output.As(&output6)) || FAILED(output6->GetDesc1(&desc))) continue;
            if (desc.Monitor == mon) return true;
        }
    }
    return false;
}

// Windows' SDR content brightness on monitor mon, in nits
float SdrWhiteNits(HMONITOR mon) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    UINT32 path_count = 0;
    UINT32 mode_count = 0;
    if (!GetMonitorInfoW(mon, &info)) return kDefaultPaperNits;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) {
        return kDefaultPaperNits;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    LONG queried = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(),
                                      nullptr);
    if (queried != ERROR_SUCCESS) return kDefaultPaperNits;
    for (UINT32 i = 0; i < path_count; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = paths[i].sourceInfo.adapterId;
        source.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;
        if (std::wcscmp(source.viewGdiDeviceName, info.szDevice) != 0) continue;
        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof(white);
        white.header.adapterId = paths[i].targetInfo.adapterId;
        white.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&white.header) != ERROR_SUCCESS) break;
        return float(white.SDRWhiteLevel) * kSdrWhiteUnit;
    }
    return kDefaultPaperNits;
}

const char* kSmaaConfig = R"(
cbuffer P : register(b1) { float4 metrics; };
#define SMAA_RT_METRICS metrics
#define SMAA_CUSTOM_SL
#define SMAA_THRESHOLD 0.05
#define SMAA_MAX_SEARCH_STEPS 16
#define SMAA_MAX_SEARCH_STEPS_DIAG 8
#define SMAA_CORNER_ROUNDING 25
SamplerState LinearSampler : register(s0);
SamplerState PointSampler : register(s1);
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)
#define SMAASample(tex, coord) tex.Sample(LinearSampler, coord)
#define SMAASamplePoint(tex, coord) tex.Sample(PointSampler, coord)
#define SMAASampleOffset(tex, coord, offset) tex.Sample(LinearSampler, coord, offset)
#define SMAAGather(tex, coord) tex.Gather(LinearSampler, coord, 0)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
)";

// after kSmaaHlsl: edges from the frame's luma, blend weights from edges, area and search, then the frame blended
const char* kSmaaPasses = R"(
Texture2D tex0 : register(t0);
Texture2D tex1 : register(t1);
Texture2D tex2 : register(t2);
struct SmaaEdgeV { float4 p : SV_Position; float2 uv : TEXCOORD0; float4 o[3] : TEXCOORD1; };
struct SmaaWeightV { float4 p : SV_Position; float2 uv : TEXCOORD0; float2 pix : TEXCOORD1; float4 o[3] : TEXCOORD2; };
struct SmaaBlendV { float4 p : SV_Position; float2 uv : TEXCOORD0; float4 o : TEXCOORD1; };
float4 corner(uint id, out float2 uv) {
    uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
SmaaEdgeV vs_edges(uint id : SV_VertexID) {
    SmaaEdgeV o;
    o.p = corner(id, o.uv);
    SMAAEdgeDetectionVS(o.uv, o.o);
    return o;
}
SmaaWeightV vs_weights(uint id : SV_VertexID) {
    SmaaWeightV o;
    o.p = corner(id, o.uv);
    SMAABlendingWeightCalculationVS(o.uv, o.pix, o.o);
    return o;
}
SmaaBlendV vs_blend(uint id : SV_VertexID) {
    SmaaBlendV o;
    o.p = corner(id, o.uv);
    SMAANeighborhoodBlendingVS(o.uv, o.o);
    return o;
}
float4 smaa_edges(SmaaEdgeV i) : SV_Target { return float4(SMAALumaEdgeDetectionPS(i.uv, i.o, tex0), 0, 0); }
float4 smaa_weights(SmaaWeightV i) : SV_Target { return SMAABlendingWeightCalculationPS(i.uv, i.pix, i.o, tex0, tex1, tex2, 0); }
float4 smaa_blend(SmaaBlendV i) : SV_Target { return SMAANeighborhoodBlendingPS(i.uv, i.o, tex0, tex1); }
)";

// from the scene's depth: occlusion pass, then a blur across and one down that multiplies it into the colour
// stencil bit 0 is the world, the sky is drawn where it is clear and counts as the far plane
const char* kAoHlsl = R"(
cbuffer A : register(b1) { float4 proj; float4 zp; float4 vp; float4 opt; float4 fog; };
// xboxrecomp (hppoa): NOSTENCIL, every pixel's depth as it is (this title marks nothing in the stencil; a sky drawn
// without depth leaves the clear, the far plane)
#ifdef MS
Texture2DMS<float> depth : register(t0);
Texture2DMS<uint2> stencil : register(t2);
#ifdef NOSTENCIL
float Raw(int2 p) { return depth.Load(p, 0); }
#else
float Raw(int2 p) { return stencil.Load(p, 0).g & 1 ? depth.Load(p, 0) : 1; }
#endif
#else
Texture2D<float> depth : register(t0);
Texture2D<uint2> stencil : register(t2);
#ifdef NOSTENCIL
float Raw(int2 p) { return depth.Load(int3(p, 0)); }
#else
float Raw(int2 p) { return stencil.Load(int3(p, 0)).g & 1 ? depth.Load(int3(p, 0)) : 1; }
#endif
#endif
Texture2D<float2> ao : register(t1);
Texture2D<float4> scene : register(t3);  // xboxrecomp: the frame's colour, for dof
float4 vs(uint id : SV_VertexID) : SV_Position { return float4(float2((id << 1) & 2, id & 2) * float2(2, -2) + float2(-1, 1), 0, 1); }
// view space position of a pixel. proj: 1 / scale xy, centre xy; zp: z = x / (depth - y); vp: viewport in pixels
float3 View(float2 px) {
    int2 p = clamp(int2(px), int2(vp.xy), int2(vp.xy + vp.zw) - 1);
    float z = zp.x / (Raw(p) - zp.y);
    float2 ndc = (p + 0.5 - vp.xy) / vp.zw * float2(2, -2) + float2(-1, 1);
    return float3((ndc - proj.zw) * proj.xy * z, z);
}
static const float kDither[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
// normal from the nearer neighbour each way: no bend over depth edges
float3 Normal(float2 px, float3 c) {
    float3 l = View(px - float2(1, 0)), r = View(px + float2(1, 0));
    float3 u = View(px - float2(0, 1)), d = View(px + float2(0, 1));
    float3 dx = abs(r.z - c.z) < abs(c.z - l.z) ? r - c : c - l;
    float3 dy = abs(d.z - c.z) < abs(c.z - u.z) ? d - c : c - u;
    return normalize(cross(dx, dy));
}
// opt: world radius, most pixels it may span; out: occlusion, view z
float2 occlusion(float4 pos : SV_Position) : SV_Target {
    float3 c = View(pos.xy);
    float2 rad = opt.x * 0.5 * vp.zw / (proj.xy * c.z);
    float span = max(rad.x, rad.y);
    if (span < 1) return float2(1, c.z);
    float3 n = Normal(pos.xy, c);
    float k = min(1, opt.y / span), R = opt.x * k;
    int2 cell = int2(pos.xy) & 3;
    float turn = kDither[cell.y * 4 + cell.x] * (6.2831853 / 16), a = 0;
    for (int i = 0; i < 16; i++) {
        float f = (i + 0.5) / 16, t = i * 2.3999632 + turn;
        float3 v = (View(pos.xy + float2(cos(t), sin(t)) * rad * (k * f)) - c) / R;
        float vv = dot(v, v), w = max(1 - vv, 0);
        a += w * w * w * max((dot(v, n) - 0.05) / (vv + 0.01), 0);
    }
    return float2(max(1 - a * 0.25, 0), c.z);
}
static const float kHalfPi = 1.5707963;
// GTAO (Jimenez 2016) at XeGTAO's high preset: 3 slices, 3 steps a side, squared step spread, falloff over the outer
// 61.5% of the radius, power 2.2. Per slice the highest horizon each side, the visible arc between integrated against
// the normal projected into the slice
float2 gtao(float4 pos : SV_Position) : SV_Target {
    float3 c = View(pos.xy);
    float2 rad = opt.x * 0.5 * vp.zw / (proj.xy * c.z);
    float span = max(rad.x, rad.y);
    if (span < 1) return float2(1, c.z);
    float3 n = Normal(pos.xy, c);
    float3 v = normalize(-c);
    float k = min(1, opt.y / span), R = opt.x * k;
    int2 cell = int2(pos.xy) & 3;
    float slice_noise = kDither[cell.y * 4 + cell.x] / 16;
    float step_noise = kDither[cell.x * 4 + cell.y] / 16;
    float vis = 0;
    for (int s = 0; s < 3; s++) {
        float phi = (s + slice_noise) * (kHalfPi * 2 / 3);
        float3 dir = float3(cos(phi), sin(phi), 0);
        // in pixels; screen y runs down, view y up
        float2 omega = float2(dir.x, -dir.y) * rad * k;
        float3 ortho = dir - dot(dir, v) * v;
        float3 axis = normalize(cross(ortho, v));
        float3 pn = n - axis * dot(n, axis);
        float pn_len = max(length(pn), 1e-4);
        float cos_n = saturate(dot(pn, v) / pn_len);
        float an = sign(dot(ortho, pn)) * acos(cos_n);
        // x: the +omega side, y: the -omega side; lowest is flat along the slice's tangent
        float2 low = float2(cos(an + kHalfPi), cos(an - kHalfPi));
        float2 hc = low;
        for (int j = 0; j < 3; j++) {
            float f = (j + step_noise) / 3;
            // at least 1.3 pixels out, off the centre pixel
            float2 off = omega * (f * f + 1.3 / length(omega));
            float3 d0 = View(pos.xy + off) - c;
            float3 d1 = View(pos.xy - off) - c;
            float2 len = max(float2(length(d0), length(d1)), 1e-4);
            float2 w = saturate((R - len) / (R * 0.615));
            float2 sc = float2(dot(d0, v), dot(d1, v)) / len;
            hc = max(hc, lerp(low, sc, w));
        }
        float h0 = an + clamp(-acos(hc.y) - an, -kHalfPi, kHalfPi);
        float h1 = an + clamp(acos(hc.x) - an, -kHalfPi, kHalfPi);
        float sin_n = sin(an);
        float arc0 = (cos_n + 2 * h0 * sin_n - cos(2 * h0 - an)) / 4;
        float arc1 = (cos_n + 2 * h1 * sin_n - cos(2 * h1 - an)) / 4;
        vis += pn_len * (arc0 + arc1);
    }
    return float2(pow(saturate(vis / 3), 2.2), c.z);
}
// opt.zw: step; 5 taps average the 4 pixel dither, depth edges left out
float Smooth(float2 pos, out float z) {
    int2 p = int2(pos), lo = int2(vp.xy), hi = int2(vp.xy + vp.zw) - 1;
    z = ao.Load(int3(p, 0)).y;
    float sum = 0, n = 0;
    for (int i = -2; i <= 2; i++) {
        float2 s = ao.Load(int3(clamp(p + i * int2(opt.zw), lo, hi), 0));
        float w = (abs(i) == 2 ? 0.5 : 1) * saturate(2 - abs(s.y - z) / (z * 0.02));
        sum += s.x * w;
        n += w;
    }
    return sum / n;
}
float2 occlusion_blur(float4 pos : SV_Position) : SV_Target {
    float z, a = Smooth(pos.xy, z);
    return float2(a, z);
}
// fog: the game's, already blended over the scene, fades the occlusion out as it thickens
float4 occlusion_apply(float4 pos : SV_Position) : SV_Target {
    float z, a = Smooth(pos.xy, z);
    float fogged = fog.z * saturate(fog.x * z + fog.y);
    return float4(lerp(a, 1, fogged).xxx, 1);
}
)";

float Bits(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

void Color(uint32_t argb, float* o) {
    o[0] = ((argb >> 16) & 255) / 255.0f;
    o[1] = ((argb >> 8) & 255) / 255.0f;
    o[2] = (argb & 255) / 255.0f;
    o[3] = (argb >> 24) / 255.0f;
}

constexpr uint32_t kBlendDestColor = 0x306;  // D3DBLEND_DESTCOLOR

D3D12_BLEND Blend(uint32_t v, bool alpha) {
    switch (v) {
        case 0: return D3D12_BLEND_ZERO;
        case 1: return D3D12_BLEND_ONE;
        case 0x300: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
        case 0x301: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
        case 0x302: return D3D12_BLEND_SRC_ALPHA;
        case 0x303: return D3D12_BLEND_INV_SRC_ALPHA;
        case 0x304: return D3D12_BLEND_DEST_ALPHA;
        case 0x305: return D3D12_BLEND_INV_DEST_ALPHA;
        case kBlendDestColor: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
        case 0x307: return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
        case 0x308: return D3D12_BLEND_SRC_ALPHA_SAT;
        case 0x8001: case 0x8003: return D3D12_BLEND_BLEND_FACTOR;
        case 0x8002: case 0x8004: return D3D12_BLEND_INV_BLEND_FACTOR;
        default: return D3D12_BLEND_ONE;
    }
}

D3D12_BLEND_OP BlendOp(uint32_t v) {
    switch (v) {
        case 0x800A: return D3D12_BLEND_OP_SUBTRACT;
        case 0x800B: return D3D12_BLEND_OP_REV_SUBTRACT;
        case 0x8007: return D3D12_BLEND_OP_MIN;
        case 0x8008: return D3D12_BLEND_OP_MAX;
        default: return D3D12_BLEND_OP_ADD;
    }
}

D3D12_COMPARISON_FUNC Cmp(uint32_t v) {
    uint32_t n = v >= 0x200 && v <= 0x207 ? v - 0x200 : 7;
    return D3D12_COMPARISON_FUNC(n + 1);
}

D3D12_STENCIL_OP StencilOp(uint32_t v) {
    switch (v) {
        case 0: return D3D12_STENCIL_OP_ZERO;
        case 0x1E01: return D3D12_STENCIL_OP_REPLACE;
        case 0x1E02: return D3D12_STENCIL_OP_INCR_SAT;
        case 0x1E03: return D3D12_STENCIL_OP_DECR_SAT;
        case 0x150A: return D3D12_STENCIL_OP_INVERT;
        case 0x8507: return D3D12_STENCIL_OP_INCR;
        case 0x8508: return D3D12_STENCIL_OP_DECR;
        default: return D3D12_STENCIL_OP_KEEP;
    }
}

D3D12_TEXTURE_ADDRESS_MODE Address(uint32_t v) {
    switch (v) {
        case 2: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        case 3: case 5: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        case 4: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        default: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    }
}

DXGI_FORMAT HostFormat(Host h) {
    switch (h) {
        case Host::BC1: return DXGI_FORMAT_BC1_UNORM;
        case Host::BC2: return DXGI_FORMAT_BC2_UNORM;
        case Host::BC3: return DXGI_FORMAT_BC3_UNORM;
        case Host::R32F: return DXGI_FORMAT_R32_FLOAT;
        default: return DXGI_FORMAT_B8G8R8A8_UNORM;
    }
}

constexpr uint32_t kFmtD16 = 0x2C;     // D3DFMT_D16
constexpr uint32_t kFmtLinD16 = 0x30;  // D3DFMT_LIN_D16

// TexInfo::shadow of a depth texture
uint8_t ShadowDepthKind(uint32_t format) { return format == kFmtD16 || format == kFmtLinD16 ? 2 : 1; }

// bytes per row of a 32 bit surface in guest memory, rounded to 64
uint32_t GuestPitch(uint32_t width) { return (width * 4 + 63) & ~63u; }

// rows of a texture upload start D3D12_TEXTURE_DATA_PITCH_ALIGNMENT apart
uint32_t UploadPitch(uint32_t row_bytes) { return (row_bytes + 255) & ~255u; }

D3D12_RESOURCE_DESC BufferDesc(uint64_t bytes) {
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = bytes;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return bd;
}

// one level, one sample: callers set Flags and SampleDesc where they differ
D3D12_RESOURCE_DESC Texture2DDesc(DXGI_FORMAT fmt, uint64_t w, uint32_t h) {
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w;
    td.Height = h;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    return td;
}

struct Target {
    ComPtr<ID3D12Resource> res;
    bool depth = false;
    uint32_t gw = 0;
    uint32_t gh = 0;
    uint32_t w = 0;
    uint32_t h = 0;
    uint32_t view = 0;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    uint32_t data = 0;
    uint32_t bytes = 0;
    uint64_t epoch = 0;  // last lock seen
    uint64_t sig = 0;    // guest memory as last matched
    bool drawn = false;           // the GPU wrote it since the last readback
    std::vector<uint8_t> read;    // what Sync left in guest memory
    // single sample copy draws read: of the target being drawn to, or a multisampled one resolved
    ComPtr<ID3D12Resource> fb;
    D3D12_RESOURCE_STATES fb_state = D3D12_RESOURCE_STATE_COPY_DEST;
    uint32_t samples = 1;         // above 1 on the back buffer sized targets under MSAA
    DXGI_FORMAT fmt = kBgra;
    bool hdr = false;              // colour held over kHeadroom until resolved
    uint64_t hdr_frame = ~0ull;    // the present count it last went HDR, once a frame
    uint64_t scene_frame = ~0ull;  // xboxrecomp: the present count it last had 3D drawn in it (SceneEnd)
    uint64_t bloom_frame = ~0ull;  // xboxrecomp: the present count its scene's passes (SceneEnd) last ran
};

// MemSig hashes a surface up to kSigWholeMax bytes whole, a bigger one as evenly spaced samples
constexpr uint32_t kSigWholeMax = 65536;
constexpr uint32_t kSigSamples = 64;
constexpr uint32_t kSigSampleBytes = 64;

uint64_t MemSig(uint32_t data, uint32_t total) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(data));
    uint64_t sig = Hash(&total, sizeof(total));
    if (total <= kSigWholeMax) return Hash(p, total, sig);
    for (uint32_t i = 0; i < kSigSamples; i++) {
        const uint8_t* sample = p + uint64_t(total - kSigSampleBytes) * i / (kSigSamples - 1);
        sig = Hash(sample, kSigSampleBytes, sig);
    }
    return sig;
}

// MemSig samples a big surface and misses small writes (XFONT text): after a Sync every byte is compared
bool Changed(const Target& t) {
    if (t.drawn || t.read.empty()) return false;
    uint32_t row = t.gw * 4;
    uint32_t pitch = GuestPitch(t.gw);
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++) {
        if (std::memcmp(p + size_t(y) * pitch, &t.read[size_t(y) * row], row)) return true;
    }
    return false;
}

// a lock alone proves nothing, reads lock too: only changed bytes mean the CPU wrote it
bool LatchCpuWrite(Target& t) {
    uint64_t e = DirtyEpoch(t.data);
    if (e <= t.epoch) return false;
    t.epoch = e;
    uint64_t sig = MemSig(t.data, t.bytes);
    if (sig == t.sig && !Changed(t)) return false;
    t.sig = sig;
    return true;
}

struct Tex {
    ComPtr<ID3D12Resource> res;
    TextureDesc d;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    uint32_t levels = 1;
    uint64_t sig = 0;
    uint64_t frame = 0;
    bool norm = false;  // a normalization cube
    uint64_t hd = 0;    // xboxrecomp: the replacement in place of the guest's pixels (hd_textures), by its hash
};

struct PsoKey {
    uint64_t vs;
    uint64_t ps;
    uint32_t regs;
    uint32_t topo;
    uint32_t has_rt;
    uint32_t has_ds;
    uint32_t elements[16];  // per register: DXGI format, buffer << 8, offset << 16
    uint32_t blend;
    uint32_t src;
    uint32_t dst;
    uint32_t op;
    uint32_t wmask;
    uint32_t zen;
    uint32_t zwrite;
    uint32_t zfunc;
    uint32_t sten;
    uint32_t sfunc;
    uint32_t smask;
    uint32_t swmask;
    uint32_t sfail;
    uint32_t szfail;
    uint32_t spass;
    uint32_t cull;
    uint32_t ccw;
    uint32_t fill;
    int32_t zbias;
    float zslope;
    uint32_t rtfmt;
    // sample count, 0 without: the bytes the key's padding held, so pipelines cached before it still match
    uint32_t msaa;
};
static_assert(sizeof(PsoKey) == 184);

// xboxrecomp: a pipeline as pipelines_list.bin keeps it: the key, and the source checks its shaders are found by in
// the shader cache (FindShader), so a PC that never ran the game makes it before the title asks
struct PipelineRecord {
    PsoKey k;
    uint64_t vs_check;
    uint64_t ps_check;
};
static_assert(sizeof(PipelineRecord) == 200);

// Blit's settings past the source and its rectangle
struct BlitOptions {
    bool gamma = false;       // through the gamma lookup
    bool alpha = false;       // the source's alpha, else 1
    uint32_t taps = 1;        // n x n box filter per pixel
    float mul = 1;            // colour scale
    DXGI_FORMAT rtv = kBgra;  // the target's format
    bool over = false;        // premultiplied over the target, which keeps its alpha
    uint32_t samples = 1;     // the target's sample count
};

// a full screen pass's target
struct PostTarget {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    DXGI_FORMAT fmt;
    uint32_t w;
    uint32_t h;
    uint32_t samples = 1;
};

// what a full screen pass reads at t0 to t2
struct PostInputs {
    ID3D12Resource* res[3];
    DXGI_FORMAT fmt[3];
};

// DrawPostPass's settings past its target, inputs and constants
struct PostOptions {
    float blend = -1;  // blend factor over the target, below 0 the pass overwrites it
    const char* hlsl = kPostHlsl;
    const char* vs_entry = "vs";
};

// Shrink's settings past the source and the size
struct ShrinkOptions {
    uint32_t taps = 1;
    // into: a copy from an earlier call to draw over again, in state into_state
    ID3D12Resource* into = nullptr;
    D3D12_RESOURCE_STATES into_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    float mul = 1;
};

// a draw's four texture stages: BindStageTexturesAndTargets fills them, the constants, views and samplers read them
struct DrawTextures {
    TexInfo info[4];
    D3D12_SHADER_RESOURCE_VIEW_DESC sv[4];
    ID3D12Resource* res[4];
    bool rt[4] = {};     // stage samples a render target
    float wide[4] = {};  // host texels per console texel of a target read into a smaller one, 0 otherwise
};

// xboxrecomp: 16:9, what the frame being drawn holds so far (Squeeze2D), turned over at Present
struct WideFrame {
    uint32_t scene = 0;       // perspective draws (a menu has a few: the inventory's fog)
    bool backdrop = false;    // a picture across the frame went up (menus, the spellbook, movies, loading screens)
    bool cut = false;         // a cutscene: the last frame had its bars, also sliding in (they come after its HUD)
    bool bars = false;        // this frame's
    float bars_h = 0;         // this frame's bars' height together
    float bars_h_prev = 0;    // the last frame's
    bool bars_out = false;    // the bars slide out: the cutscene is over, gameplay shows at once without them
    float vp_y = 0;           // the title's viewport for the frame (its rows, full width): a TV's overscan above and
    float vp_h = 0;           // below it, 0 0 none seen
    float view_y = 0;         // the last frame's vp_y, vp_h (before this one's first 2D or scene draw)
    float view_h = 0;
    bool panel = false;       // a menu's panel over the scene (the pause menu's): its 2D goes about the middle with it
};

// xboxrecomp: HP:PS's cutscene bars in its 640x480 layout, inside its viewport: rows 0-35 and 395-480. The picture
// between is 16:9 once the frame is shown wide; a fixed band, so the zoom does not follow the bars as they slide.
constexpr float kCutTop = 35.0f, kCutBottom = 395.0f;

// a draw's shaders: what they are made from, their cache keys and the hashes Register returned
struct DrawShaders {
    bool prog = false;  // a vertex program, else the fixed function pipeline
    bool quad = false;
    bool fixed_transform = false;  // fixed function on untransformed vertices: the shader reads matrices and lights
    FixedKey fk;
    uint64_t vkey = 0;
    uint64_t vh = 0;
    uint64_t pkey = 0;
    uint64_t ph = 0;
    bool wdepth = false;  // xboxrecomp: the pixel shader writes log2(1 + w) as depth (UseLogDepth)
};

// xboxrecomp: which draws write depth from w. HP:CoS draws most of the world W buffered (D3DZB_USEW) and other
// passes (the spell book's light beams, effects) with plain Z in the same frame and depth buffer. Our W draws store
// log2(1 + w) / 20 and Z draws z / w, two scales that do not compare: a Z pass passed against W depth only close to
// the camera. RECOMP_LOGDEPTH_ALL=1 (hppoa default off) puts every 3D draw with depth on the log2(1 + w) scale;
// otherwise only ZEnable == 2 draws use it. Pretransformed (screen space) draws keep their z.
// xboxrecomp debug keys, read once a frame in Present: F8 flips RECOMP_LOGDEPTH_ALL, F9 turns the alpha test off,
// F10 passes every depth test
int g_dbg_logall = -1;
bool g_dbg_noatest = false;
bool g_dbg_zalways = false;

bool UseLogDepth(const State& s, const Assembled& a) {
    int& all = g_dbg_logall;
    if (all < 0) {
        // xboxrecomp (hppoa): off unless asked. HP:CoS mixed W- and z-buffered draws in one buffer and needed every
        // 3D draw on the log scale; HP:PS z-buffers, and its pretransformed quads write plain z that log depth draws
        // were compared against (the spellbook and inventory book failed the depth test behind such a quad)
        const char* e = std::getenv("RECOMP_LOGDEPTH_ALL");
        all = e && *e && *e != '0';
    }
    if (a.pretransformed) return false;
    return all ? s.rs[kRsZEnable] != 0 : WBuffered(s);
}

// the render target's console size in pixels, or the depth buffer's without one
struct GuestSize {
    uint32_t w;
    uint32_t h;
};

class D3D12Backend final : public Backend {
public:
    bool Init(uint32_t width, uint32_t height) override;
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override;
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override;
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) override;
    void Present(const Surface& front, bool pillarbox) override;
    void Sync(uint32_t data, uint32_t size) override;
    void SyncLate(uint32_t data) override;
    bool DeliverLate(uint32_t data) override;
    void RecordLate(uint32_t data) override;
    void Flush() override;
    void SetGamma(const uint8_t* ramp) override;
    void Occlusion(const State& s, const float p[kOcclusionParams]) override;
    void OcclusionOn(const Surface& rts, const Surface& zss, const Viewport& v, const float p[kOcclusionParams]);

private:
    struct Frame {
        ComPtr<ID3D12CommandAllocator> alloc;
        ComPtr<ID3D12Resource> upload;
        uint8_t* map = nullptr;
        uint64_t used = 0;
        uint64_t fence = 0;
        std::vector<ComPtr<ID3D12Resource>> garbage;
        uint32_t srv = 0;
    };
    struct Alloc {
        ID3D12Resource* res;
        uint64_t off;
        uint8_t* cpu;
        D3D12_GPU_VIRTUAL_ADDRESS gpu;
    };

    Alloc Upload(uint64_t size, uint64_t align = 256);
    Frame& CurrentFrame() { return frames_[frame_ % kFrames]; }
    uint32_t AllocSrvSlots(uint32_t n);
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(ID3D12DescriptorHeap* h, uint32_t i, uint32_t inc) {
        auto c = h->GetCPUDescriptorHandleForHeapStart();
        c.ptr += size_t(i) * inc;
        return c;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(ID3D12DescriptorHeap* h, uint32_t i, uint32_t inc) {
        auto g = h->GetGPUDescriptorHandleForHeapStart();
        g.ptr += uint64_t(i) * inc;
        return g;
    }
    void Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES& cur, D3D12_RESOURCE_STATES want);
    void Begin();
    void Submit(bool wait);
    void WaitFence(uint64_t value);
    void SetFullViewport(uint32_t w, uint32_t h);
    void SetBlitTarget(D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t w, uint32_t h);
    void Blit(ID3D12Resource* src, DXGI_FORMAT fmt, const float uvrect[4], const BlitOptions& o = {});
    uint32_t EnsureBlitSamplers();
    void DrawFullScreen(ID3D12PipelineState* pso, D3D12_GPU_VIRTUAL_ADDRESS cb, uint32_t srv_slot,
                        std::optional<uint32_t> sampler_slot = {});
    ID3D12Resource* EnsureFeedbackCopy(Target& t);
    // what a read of a target takes, and the state it is in
    struct Readable {
        ID3D12Resource* res;
        D3D12_RESOURCE_STATES* state;
    };
    Readable ReadableResource(Target& t);
    uint32_t Samples(uint32_t want);
    void EnterHdr(Target& t);
    bool DrawPostPass(const char* entry, const PostTarget& out, const PostInputs& in, const float p[8],
                      const PostOptions& o = {});
    ID3D12PipelineState* EnsurePostPipeline(const char* entry, const PostTarget& out, const PostOptions& o);
    D3D12_CPU_DESCRIPTOR_HANDLE PostRtv(uint32_t i);
    bool CreatePostTarget(ComPtr<ID3D12Resource>& r, D3D12_RESOURCE_STATES& st, DXGI_FORMAT f, uint32_t w, uint32_t h,
                          uint32_t view);
    // Resolve, then its steps in call order
    void Resolve(Target& t);
    bool EnsureBloomTargets(uint32_t bw, uint32_t bh);
    bool EnsureExposureTarget();
    float UpdateExposureBlend();
    void DrawBloom(const PostInputs& in, const Target& t, uint32_t bw, uint32_t bh);
    bool BloomForScene(const State& s, const Assembled& a);
    bool GameBloomComposite(const State& s, const Assembled& a, const PsoKey& k);
    // xboxrecomp (hppoa): the end of a frame's 3D scene: occlusion, cutscene depth of field, bloom
    void SceneEnd(Target& t);
    float scene_proj_[16] = {};          // the scene's last fixed function 3D draw: its projection,
    Viewport scene_vp_;                  // viewport,
    Surface scene_rt_, scene_zs_;        // target and depth,
    uint32_t scene_fog_[5] = {};         // fog: enable, table mode, start, end, density
    uint64_t scene_cam_frame_ = ~0ull;   // and the present count it was drawn in
    // Smaa, then its steps in call order, each callee right after its caller
    struct SmaaLookup {
        DXGI_FORMAT fmt;
        uint32_t w;
        uint32_t h;
        uint32_t bpp;  // bytes per texel
        const uint8_t* data;
    };
    ID3D12Resource* Smaa(ID3D12Resource* src, DXGI_FORMAT fmt, uint32_t w, uint32_t h);
    bool EnsureSmaaLookups();
    bool CreateLookupTexture(ComPtr<ID3D12Resource>& r, const SmaaLookup& l);
    bool EnsureSmaaTargets(DXGI_FORMAT fmt, uint32_t w, uint32_t h);
    bool DrawSmaaPasses(ID3D12Resource* src, DXGI_FORMAT fmt, uint32_t w, uint32_t h, const char* hlsl);
    // a pass's shader entry, target and what it reads
    // depth: the scene's, read by the first pass; src: the pass before; multiply: onto the scene
    struct AoPass {
        const char* entry;
        DXGI_FORMAT out;
        uint32_t samples = 1;
        bool multiply = false;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv;
        ID3D12Resource* depth = nullptr;
        ID3D12Resource* src = nullptr;
        ID3D12Resource* color = nullptr;  // xboxrecomp: kAoHlsl's scene (dof)
        float step_x = 0;
        float step_y = 0;
    };
    // what the passes share: kAoHlsl's constants, the scene rectangle and viewport, the depth's sample count
    struct AoFrame {
        float c[20];
        D3D12_RECT rect;
        D3D12_VIEWPORT vp;
        uint32_t depth_samples;
        bool gtao;
    };
    // Occlusion, in order, each callee right after its caller
    bool EnsureAoTargets(uint32_t w, uint32_t h);
    void DrawOcclusionPasses(Target& rt, Target& ds, const AoFrame& f);
    bool DrawAoPass(const AoFrame& f, const AoPass& pass);
    ID3D12PipelineState* EnsureAoPipeline(const AoPass& pass, bool ms);
    void CreateAoViews(uint32_t slot, const AoPass& pass, bool ms);
    // Draw, in call order, each callee right after its caller
    bool BindTargets(const State& s);
    bool EnterHdrForScene(const State& s, const Assembled& a);
    uint64_t EnsureVertexShader(const State& s, const Assembled& a, const DrawShaders& sh);
    bool BindStageTexturesAndTargets(const State& s, DrawTextures& stages);
    ID3D12Resource* BindTexture(const State& s, int stage, DrawTextures& stages);
    ID3D12Resource* BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, DrawTextures& stages);
    ID3D12Resource* BindGuestTexture(int stage, const TextureDesc& d, const FormatInfo& fi, DrawTextures& stages,
                                     uint32_t palette = 0);
    bool CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi);
    void UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p, bool same,
                       const uint32_t* palette = nullptr);
    void UploadTextureLevel(Tex& tx, uint32_t f, uint32_t l, const std::vector<uint8_t>& px, uint32_t pitch);
    bool BindHdTexture(Tex& tx, const TextureDesc& d, uint64_t hd, const uint32_t* palette = nullptr);
    uint64_t EnsurePixelShader(const PixelKey& pk, uint64_t pkey);
    void BuildPipelineKey(const State& s, const Assembled& a, uint64_t vh, uint64_t ph, PsoKey& k);
    void KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k);
    ID3D12PipelineState* EnsurePipeline(const PsoKey& k, bool wait = false);
    void TakeBuilt();
    void RecordPipelineKey(const PsoKey& k);
    void StartPrebuild();
    bool prebuild_started_ = false;
    ComPtr<ID3D12PipelineState> CreatePipeline(const PsoKey& k, uint64_t h, ID3DBlob* vs, ID3DBlob* ps, ID3DBlob* gs,
                                               bool compile, bool& pending);
    void FillPipelineDesc(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps, D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) const;
    void UploadConstants(const State& s, const DrawShaders& sh, const PsoKey& k, const DrawTextures& stages,
                         GuestSize size, DrawConstants& c);
    void FillHdrConstants(const PsoKey& k, DrawConstants& c);
    float out_scale_ = 1.0f;  // xboxrecomp (hppoa): this draw's colour times this (the game's bloom, GameBloomComposite)
    void FillTextureConstants(const State& s, const DrawTextures& stages, DrawConstants& c);
    void FillTexelConstants(int i, const TextureDesc& d, const DrawTextures& stages, DrawConstants& c);
    void CaptureDraw(const State& s, uint32_t prim, uint32_t count, const PsoKey& k, const DrawConstants& c,
                     const DrawTextures& stages);
    void CaptureTextures(const State& s, const DrawTextures& stages);
    void EndDrawCapture(const State& s, const Assembled& a, uint32_t count, bool prog, const DrawConstants& c);
    void CaptureInputs(const State& s, const Assembled& a, const DrawConstants& c);
    void CaptureShaders(const State& s, const Assembled& a, const DrawShaders& sh, const PixelKey& pk);
    void CaptureShader(const char* kind, uint64_t key, const std::string& src);
    void SetTextureViewsAndSamplers(const State& s, const Assembled& a, DrawTextures& stages);
    uint32_t EnsureStageSamplers(const State& s, const DrawTextures& stages, bool clamp);
    void SetViewportScissor(const State& s, uint32_t gw, const float squeeze[4]);
    void SetBlendFactorStencilRef(const State& s);
    void DrawGeometry(const Assembled& a);
    void UploadVertexBuffers(const Assembled& a);
    // shaders by source hash: Register keeps the source, EnsureShaderCode finds the bytecode on disk or compiles it;
    // with compile false it only looks, and sets pending where the compiler is needed
    uint64_t Register(std::string src, const char* entry, const char* profile);
    ID3DBlob* EnsureShaderCode(uint64_t hash, bool compile, bool& pending);
    ID3DBlob* Compile(const std::string& src, const char* entry, const char* profile);
    ID3D12PipelineState* EnsureBlitPipeline(DXGI_FORMAT rtv, bool over, uint32_t samples);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC FullScreenPipelineDesc(ID3DBlob* vs, ID3DBlob* ps, DXGI_FORMAT rtv,
                                                              uint32_t samples) const;
    // Init, in order, each callee right after its caller
    bool CreateDevice();
    bool CreateSwapChain(HWND hwnd);
    void CreateHeaps();
    uint32_t CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE t, uint32_t n, bool visible, ComPtr<ID3D12DescriptorHeap>& out);
    void CreateBackBufferViews();
    bool CreateRootSignature();
    void CreateFrames();
    void CreateGammaLut();
    void LoadSettings();
    void LatchFeatureSettings(const app::Settings& set);
    // Present, in call order, each callee right after its caller; DrawFrame runs the ones after it to RenderUi
    void FinishCapture(const Surface& front);
    void StartCapture();
    void DetectHdr();
    void ResizeSwap();
    bool ResizeBackBuffers(uint32_t w, uint32_t h, DXGI_FORMAT fmt, DXGI_COLOR_SPACE_TYPE space);
    void UploadGammaRamp(uint32_t brightness, uint32_t contrast);
    void DrawFrame(Target* t, bool pillarbox, uint32_t bi);
    ID3D12Resource* PrepareShownFrame(Target* t);
    void DrawSdrFrame(Target* t, ID3D12Resource* r, bool pillarbox, D3D12_CPU_DESCRIPTOR_HANDLE bb);
    bool EnsureOutTargets();
    void DrawHdrFrame(Target* t, ID3D12Resource* r, bool pillarbox, D3D12_CPU_DESCRIPTOR_HANDLE bb);
    void BlitFrame(const Target& t, ID3D12Resource* r, bool pillarbox, DXGI_FORMAT rtv = kBgra);
    void RenderUi();
    void RenderOverlay();
    void RenderPanel();
    void ReleaseOldTextures();
    void ResetFrame();
    void CountFps();
    void ApplySettings();
    ComPtr<ID3D12Resource> Shrink(ID3D12Resource* src, D3D12_RESOURCE_STATES& st, DXGI_FORMAT fmt, uint32_t w,
                                  uint32_t h, const ShrinkOptions& o = {});
    bool ReadBack(ID3D12Resource* res, uint32_t w, uint32_t h, std::vector<uint8_t>& out);
    void CapSave(Target& t, const char* name);
    void CapStep(const Surface& rt);
    // a resource CapSave shrinks to w x h and writes, in state *state, colour times mul
    struct CapImage {
        ID3D12Resource* res;
        D3D12_RESOURCE_STATES* state;
        DXGI_FORMAT fmt;
        uint32_t w;
        uint32_t h;
        float mul = 1;
    };
    void CapSave(const CapImage& img, const char* name);
    // a surface's place in targets_: its key, console size and entry, end when it has none
    struct TargetSlot {
        uint32_t data;
        uint64_t key;
        uint32_t gw;
        uint32_t gh;
        std::unordered_map<uint64_t, Target>::iterator it;
        bool sized;  // the entry is there at the surface's size
    };
    Target* FindTarget(const Surface& s, bool depth);
    Target* EnsureTarget(const Surface& s, bool depth);
    // EnsureTarget's steps, in call order
    TargetSlot FindTargetSlot(const Surface& s, bool depth);
    void ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it);
    bool CreateTargetResource(Target& t, bool shadow, bool blur);
    bool CreateTargetView(Target& t);
    // UploadTarget, then its step
    void UploadTarget(Target& t, const Surface& s);
    ComPtr<ID3D12Resource> CreatePixelTexture(const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w, uint32_t h);
    // Clear's arguments past the state
    struct ClearArgs {
        uint32_t count;
        const uint8_t* rects;
        uint32_t flags;
        uint32_t color;
        float z;
        uint32_t stencil;
    };
    void ClearTarget(Target& t, const State& s, const ClearArgs& c, std::vector<D3D12_RECT>& rs);
    // Copy's guest lists: count src_rects, dst_points 0 or one per rect
    struct CopyList {
        uint32_t count;
        uint32_t src_rects;
        uint32_t dst_points;
    };
    void CopyByDraw(const Target& src_t, ID3D12Resource* src_res, D3D12_RESOURCE_STATES& src_state, Target& dst_t,
                    const CopyList& l);
    void CopyByRegion(const Target& src_t, ID3D12Resource* src_res, D3D12_RESOURCE_STATES& src_state, Target& dst_t,
                      const CopyList& l);
    // the target as a guest sized BGRA copy source, and the shrunk copy that holds it when it is not one already
    struct CopySource {
        ID3D12Resource* res;
        ComPtr<ID3D12Resource> down;
    };
    CopySource PrepareCopySource(Target& t, const ShrinkOptions& o);
    // RecordLate's steps, under late_lock_
    struct Late;
    void ResetLate(Late& l, const Target& t);
    ID3D12Resource* PrepareLateSource(Target& t, Late& l);
    void RecordLateCopy(Late& l, uint32_t slot, ID3D12Resource* src);

    ComPtr<IDXGIFactory4> factory_;
    ComPtr<ID3D12Device> dev_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<IDXGISwapChain3> swap_;
    ComPtr<ID3D12Resource> bb_[kFrames];
    ComPtr<ID3D12Fence> fence_;
    HANDLE event_ = nullptr;
    uint64_t fence_value_ = 0;
    Frame frames_[kFrames];
    uint64_t frame_ = 0;
    ComPtr<ID3D12GraphicsCommandList> cl_;
    ComPtr<ID3D12DescriptorHeap> srv_heap_;
    ComPtr<ID3D12DescriptorHeap> smp_heap_;
    ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    ComPtr<ID3D12DescriptorHeap> dsv_heap_;
    ComPtr<ID3D12DescriptorHeap> ui_heap_;
    // xboxrecomp: the movie overlay (XMV frames from hle_video.c), drawn over the frame where the ImGui menu was
    ComPtr<ID3D12Resource> ov_tex_;
    D3D12_RESOURCE_STATES ov_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
    uint32_t ov_w_ = 0, ov_h_ = 0, ov_version_ = 0xFFFFFFFFu;
    bool ov_shown_ = false;
    float ov_rect_[4] = {};  // display pixels: x, y, w, h of fb_w_ x fb_h_
    uint32_t ov_fb_w_ = 640, ov_fb_h_ = 480;
    void OverlayLatch(const XboxPresentInfo& pi);
    uint32_t srv_inc_ = 0;
    uint32_t smp_inc_ = 0;
    uint32_t rtv_inc_ = 0;
    uint32_t dsv_inc_ = 0;
    std::vector<uint32_t> free_rtv_;
    std::vector<uint32_t> free_dsv_;
    ComPtr<ID3D12RootSignature> root_;
    struct Source {
        std::string text;
        std::string entry;
        std::string profile;
        ComPtr<ID3DBlob> code;
        uint64_t check = 0;  // xboxrecomp: the source's second hash, kept for pipelines_list.bin
        uint8_t state = 0;  // 0 source only, 1 compiling, 2 done (no code: it failed)
        // xboxrecomp: code from base_shaders.bin (built on another PC); the text stays for when the driver refuses it
        bool base = false;
        bool no_base = false;  // refused once: compiled here from now on
    };
    // a base shader the driver refused: compiled here from its source next time; true when hash was one
    bool DropBaseCode(uint64_t hash);
    std::vector<ComPtr<ID3DBlob>> retired_code_;  // refused base bytecode, kept: other threads may still read it
    std::unordered_map<uint64_t, Source> shaders_;  // build threads too, under shader_lock_
    std::mutex shader_lock_;
    std::condition_variable shader_done_;
    std::unordered_map<uint64_t, uint64_t> shader_src_;  // vertex and pixel program keys to source hash
    std::unordered_map<uint64_t, uint64_t> line_gs_;     // a vertex shader's LineGsHlsl, for line draws
    std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> psos_;
    // async shaders: a pipeline that needs a compiler is built on the build threads, its draws skipped until it is back
    bool async_ = false;
    bool skipped_frame_ = false;
    bool shown_ = false;
    std::chrono::steady_clock::time_point shown_t_;
    std::unordered_set<uint64_t> building_;
    std::vector<std::pair<uint64_t, ComPtr<ID3D12PipelineState>>> built_;
    std::mutex built_lock_;
    std::atomic<bool> built_ready_{false};
    std::unordered_map<uint64_t, uint32_t> samplers_;
    uint32_t smp_used_ = 0;
    std::unordered_map<uint64_t, Target> targets_;
    // xboxrecomp: by data address, and for P8 also the palette's (one texture can be drawn with several)
    std::unordered_map<uint64_t, Tex> textures_;
    uint64_t tex_key_ = 0;  // the entry CreateTexture fills
    std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> blit_psos_;
    // shader model 3.0 features, from the settings at each present
    bool sm3_ = false;
    bool hdr_ = false;
    bool tone_ = false;
    // HDR10 output: the window's display, the output asked and in use, the frame and panel targets it draws first
    HMONITOR hdr_mon_ = nullptr;
    bool hdr_display_ = false;
    bool hdr_want_ = false;
    bool hdr_out_ = false;
    float paper_nits_ = kDefaultPaperNits;
    float peak_nits_ = kDefaultPeakNits;
    ComPtr<ID3D12Resource> out_[2];
    D3D12_RESOURCE_STATES out_state_[2] = {};
    uint32_t out_w_ = 0;
    uint32_t out_h_ = 0;
    uint32_t msaa_ = 1;      // samples in use
    uint32_t msaa_set_ = 0;  // app::Settings::msaa they came from
    // resolve: quarter size bloom pair, the exposure pixel kept across frames
    ComPtr<ID3D12Resource> bloom_[2];
    ComPtr<ID3D12Resource> expo_;
    D3D12_RESOURCE_STATES bloom_state_[2] = {};
    D3D12_RESOURCE_STATES expo_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
    uint32_t post_rtv_ = 0;
    uint32_t bloom_w_ = 0;
    uint32_t bloom_h_ = 0;
    bool expo_fresh_ = true;
    // ambient occlusion: the raw pass and its first blur, each with view z beside it
    ComPtr<ID3D12Resource> ao_[2];
    D3D12_RESOURCE_STATES ao_state_[2] = {};
    uint32_t ao_w_ = 0;
    uint32_t ao_h_ = 0;
    // SMAA: edges, blend weights and the blended frame, at the frame's size; its two lookups
    ComPtr<ID3D12Resource> smaa_[3];
    ComPtr<ID3D12Resource> smaa_lut_[2];
    D3D12_RESOURCE_STATES smaa_state_[3] = {};
    uint32_t smaa_w_ = 0;
    uint32_t smaa_h_ = 0;
    DXGI_FORMAT smaa_fmt_ = DXGI_FORMAT_UNKNOWN;
    std::chrono::steady_clock::time_point expo_t_ = std::chrono::steady_clock::now();
    // one frame capture: every draw in a text file, each target as the frame leaves it, the textures it read
    FILE* cap_ = nullptr;
    WideFrame wf_;                  // xboxrecomp: 16:9, the frame being drawn
    float shown_crop_[4] = {};  // xboxrecomp: 16:9, the shown frame's part filling the screen (WideFrame::crop)
    std::filesystem::path cap_dir_;
    uint32_t cap_i_ = 0;
    uint32_t cap_rt_ = 0;
    bool cap_draws_ = false;  // xboxrecomp: RECOMP_CAP_DRAWS, the target before every draw
    // xboxrecomp: the states as the last captured draw left them (each draw's line lists what changed)
    std::array<uint32_t, 0x800> cap_simple_{};
    std::array<uint32_t, kRsMax> cap_rs_{};
    std::array<std::array<uint32_t, kTssCount>, 4> cap_tss_{};
    std::array<float, 17> cap_mat_{};  // xboxrecomp: the material and lights as last captured
    std::array<std::array<uint32_t, 26>, 8> cap_lights_{};
    uint32_t cap_light_enable_ = 0;
    // xboxrecomp (hppoa): the lever panel textures' data addresses, and the state its fading draw is drawn with
    std::unordered_set<uint32_t> lever_panels_;
    State lever_state_;
    uint32_t lever_base_ = 0;  // the last finished lever's grey texture (guest header)
    // a lever part (panel data << 32 | vertex count) -> its finished lever's grey texture (header)
    std::unordered_map<uint64_t, uint32_t> lever_pairs_;
    const State& LeverPanelFade(const State& s, uint32_t count);
    bool LeverPanelKnown(uint32_t data) const;
    State skin_state_;  // xboxrecomp (hppoa): a draw without its second pass of its own texture
    const State& NoSecondSkinPass(const State& s);
    struct WindowLight {
        uint32_t tex = 0;
        std::array<uint32_t, kTssCount> tss{};
        std::array<float, 16> tm{};
        uint64_t ms = 0;
    } window_;  // xboxrecomp (hppoa): the train's window light stage as last drawn (WindowLightOnCharacters)
    State window_state_;
    const State& WindowLightOnCharacters(const State& s);
    // xboxrecomp: a burst of captures (RequestCaptureBurst)
    std::filesystem::path burst_dir_;
    int burst_left_ = 0, burst_i_ = 0;
    void CaptureStates(const State& s);
    void CaptureClip(const State& s, const Assembled& a);
    void CaptureLighting(const State& s, const Assembled& a, const float* wv);
    std::unordered_set<uint32_t> cap_seen_;
    std::vector<uint32_t> cap_sampled_;  // xboxrecomp: render targets sampled as textures this capture
    std::unordered_set<uint64_t> cap_sh_;
    struct CapTex {
        ComPtr<ID3D12Resource> res;
        DXGI_FORMAT fmt;
        uint32_t data;
        uint32_t w;
        uint32_t h;
    };
    std::vector<CapTex> cap_tex_;
    ComPtr<ID3D12Resource> lut_;
    D3D12_RESOURCE_STATES lut_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
    uint32_t ramp_set_ = ~0u;  // brightness << 8 | contrast the lookup holds
    // xboxrecomp: the title's own ramp (SetGammaRamp), red, green and blue, 256 entries each; the settings go on top
    uint8_t ramp_[768] = {};  // identity from Init
    bool ramp_dirty_ = false;
    bool gamma_ = false;
    Target* bound_rt_ = nullptr;
    Target* bound_ds_ = nullptr;
    Target* draw_rt_ = nullptr;  // the draw's targets, the bound ones go when a texture's copy is drawn
    Target* draw_ds_ = nullptr;
    uint32_t guest_w_ = 640;
    uint32_t guest_h_ = 480;
    uint32_t bb_w_ = 0;
    uint32_t bb_h_ = 0;
    float scale_ = 1.0f;
    uint32_t shadow_res_ = 0;  // app::Settings::shadow_resolution the targets were made for
    // surfaces locked without a wait: copies in flight, the newest finished one goes to guest memory
    struct Late {
        ComPtr<ID3D12Resource> rb[kFrames];
        ComPtr<ID3D12Resource> down;
        D3D12_RESOURCE_STATES down_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
        uint64_t fence[kFrames] = {};
        uint64_t frame[kFrames] = {};
        uint64_t delivered = 0;
        uint32_t gw = 0;
        uint32_t gh = 0;
        uint32_t row_pitch = 0;
        uint32_t next = 0;
    };
    std::unordered_map<uint32_t, Late> late_;
    std::mutex late_lock_;                      // DeliverLate runs on the title's thread
    std::atomic<uint64_t> presents_seen_{0};    // presents_ for DeliverLate
    Assembled asm_;  // kept between draws for its buffers
    bool tearing_ = false;
    uint64_t presents_ = 0;
    std::chrono::steady_clock::time_point fps_t_ = std::chrono::steady_clock::now();
    uint64_t fps_n_ = 0;
};

bool D3D12Backend::Init(uint32_t width, uint32_t height) {
    guest_w_ = width;
    guest_h_ = height;
    for (int i = 0; i < 768; i++) ramp_[i] = uint8_t(i);  // identity until the title sets its ramp
    HWND hwnd = static_cast<HWND>(app::Window());
    if (!CreateDevice()) return false;
    if (!CreateSwapChain(hwnd)) return false;
    CreateHeaps();
    CreateBackBufferViews();
    if (!CreateRootSignature()) return false;
    CreateFrames();
    CreateGammaLut();
    app::InitUi();
    ImGui_ImplDX12_Init(dev_.Get(), kFrames, DXGI_FORMAT_B8G8R8A8_UNORM, ui_heap_.Get(),
                        ui_heap_->GetCPUDescriptorHandleForHeapStart(), ui_heap_->GetGPUDescriptorHandleForHeapStart());
    LoadSettings();
    Begin();
    LOGI("d3d12: %ux%u guest, %ux%u window, render scale %.3f", width, height, bb_w_, bb_h_, scale_);
    return true;
}

bool D3D12Backend::CreateDevice() {
    if (GetEnvironmentVariableW(L"SCDA_D3D12_DEBUG", nullptr, 0)) {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer();
    }
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_)))) return false;
    // dedicated GPU first on hybrid laptops
    ComPtr<IDXGIFactory6> f6;
    ComPtr<IDXGIAdapter1> adapter;
    if (SUCCEEDED(factory_.As(&f6))) {
        f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
    }
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev_))) &&
        FAILED(D3D12CreateDevice((adapter = nullptr).Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev_)))) {
        LOGE("d3d12: no device");
        return false;
    }
    DXGI_ADAPTER_DESC1 ad{};
    if (adapter) adapter->GetDesc1(&ad);
    LOGR("D3D12 adapter: %ls", adapter ? ad.Description : L"default");
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    dev_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_));
    ComPtr<IDXGIFactory5> f5;
    BOOL tear = FALSE;
    if (SUCCEEDED(factory_.As(&f5))) f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tear, sizeof(tear));
    tearing_ = tear;
    return true;
}

bool D3D12Backend::CreateSwapChain(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    bb_w_ = std::max<LONG>(rc.right - rc.left, 1);
    bb_h_ = std::max<LONG>(rc.bottom - rc.top, 1);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = bb_w_;
    sd.Height = bb_h_;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kFrames;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    ComPtr<IDXGISwapChain1> sc1;
    HRESULT hr = factory_->CreateSwapChainForHwnd(queue_.Get(), hwnd, &sd, nullptr, nullptr, &sc1);
    if (FAILED(hr)) {
        LOGE("d3d12: no swap chain (hr %08lX, window %p %ldx%ld, tearing %d)", (unsigned long)hr, (void*)hwnd,
             (long)bb_w_, (long)bb_h_, (int)tearing_);
        return false;
    }
    factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    sc1.As(&swap_);
    return true;
}

uint32_t D3D12Backend::CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE t, uint32_t n, bool visible,
                                  ComPtr<ID3D12DescriptorHeap>& out) {
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = t;
    hd.NumDescriptors = n;
    hd.Flags = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    dev_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&out));
    return dev_->GetDescriptorHandleIncrementSize(t);
}

void D3D12Backend::CreateBackBufferViews() {
    for (int i = 0; i < kFrames; i++) {
        swap_->GetBuffer(i, IID_PPV_ARGS(&bb_[i]));
        dev_->CreateRenderTargetView(bb_[i].Get(), nullptr, Cpu(rtv_heap_.Get(), kRtvCount + i, rtv_inc_));
    }
}

void D3D12Backend::CreateHeaps() {
    srv_inc_ = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvCount, true, srv_heap_);
    smp_inc_ = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerCount, true, smp_heap_);
    rtv_inc_ = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvCount + kFrames + kPostRtvs, false, rtv_heap_);
    post_rtv_ = kRtvCount + kFrames;
    dsv_inc_ = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kDsvCount, false, dsv_heap_);
    CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, true, ui_heap_);
    for (uint32_t i = kRtvCount; i-- > 0;) free_rtv_.push_back(i);
    for (uint32_t i = kDsvCount; i-- > 0;) free_dsv_.push_back(i);
}

bool D3D12Backend::CreateRootSignature() {
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 4;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    ranges[1].NumDescriptors = 4;
    D3D12_ROOT_PARAMETER params[4] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 1;
    for (int i = 0; i < 2; i++) {
        params[2 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2 + i].DescriptorTable.NumDescriptorRanges = 1;
        params[2 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
        params[2 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 4;
    rsd.pParameters = params;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> rs;
    ComPtr<ID3DBlob> err;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs, &err))) return false;
    dev_->CreateRootSignature(0, rs->GetBufferPointer(), rs->GetBufferSize(), IID_PPV_ARGS(&root_));
    cache::Open(dev_.Get(), Hash(rs->GetBufferPointer(), rs->GetBufferSize()));
    return true;
}

void D3D12Backend::CreateFrames() {
    for (auto& f : frames_) {
        dev_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.alloc));
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC bd = BufferDesc(kUploadSize);
        dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                      IID_PPV_ARGS(&f.upload));
        f.upload->Map(0, nullptr, reinterpret_cast<void**>(&f.map));
    }
    dev_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    dev_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frames_[0].alloc.Get(), nullptr, IID_PPV_ARGS(&cl_));
}

// gamma ramp as a 256 x 1 lookup
void D3D12Backend::CreateGammaLut() {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_B8G8R8A8_UNORM, 256, 1);
    dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                  IID_PPV_ARGS(&lut_));
}

void D3D12Backend::LoadSettings() {
    scale_ = app::RenderScale(guest_h_);
    shadow_res_ = app::Get().shadow_resolution;
    const app::Settings& set = app::Get();
    sm3_ = set.shader_model >= 3;
    hdr_ = sm3_ && set.hdr_rendering;
    LatchFeatureSettings(set);
    msaa_set_ = set.msaa;
    msaa_ = Samples(msaa_set_);
}

// tone mapping and async shaders from the settings; sm3_ is set before
void D3D12Backend::LatchFeatureSettings(const app::Settings& set) {
    tone_ = sm3_ && set.tone_mapping;
    async_ = set.async_shaders;
}

void D3D12Backend::Begin() {
    ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get(), smp_heap_.Get()};
    cl_->SetDescriptorHeaps(2, heaps);
    cl_->SetGraphicsRootSignature(root_.Get());
    bound_rt_ = bound_ds_ = nullptr;
}

// xboxrecomp (hppoa): what a frame costs the GPU's feed, per frame over 10 s ([GPUSTAT]): submissions, the ones the CPU
// waited out to idle and how long, full-target resolves / copies for a read of a target, region copies, post passes,
// read-backs. A 2004 title should need few of each; many per frame keeps the GPU's clocks pinned (bursts and waits)
namespace {
struct GpuStat {
    uint64_t frames, draws, submits, idle_waits, idle_ns, resolves, copies, region_copies, post, readbacks, flushes;
    uint64_t resolve_bytes, copy_bytes;
    std::map<uint64_t, uint64_t> sizes;  // w << 32 | h << 8 | samples: feedback copies of that size
} g_gs;
std::chrono::steady_clock::time_point g_gs_t0 = std::chrono::steady_clock::now();
void GpuStatFrame() {
    g_gs.frames++;
    auto now = std::chrono::steady_clock::now();
    if (now - g_gs_t0 < std::chrono::seconds(10) || !g_gs.frames) return;
    g_gs_t0 = now;
    double n = double(g_gs.frames);
    std::fprintf(stderr,
                 "[GPUSTAT] per frame (%llu frames): %.0f draws, %.1f submits, %.2f waits for GPU idle (%.2f ms), "
                 "%.2f flushes, %.1f target resolves, %.1f target copies, %.1f region copies, %.1f post passes, "
                 "%.2f read-backs; feedback copies %.1f MB, resolves %.1f MB\n",
                 (unsigned long long)g_gs.frames, g_gs.draws / n, g_gs.submits / n, g_gs.idle_waits / n,
                 g_gs.idle_ns / n / 1e6, g_gs.flushes / n, g_gs.resolves / n, g_gs.copies / n, g_gs.region_copies / n,
                 g_gs.post / n, g_gs.readbacks / n, g_gs.copy_bytes / n / 1048576.0, g_gs.resolve_bytes / n / 1048576.0);
    std::string sz;
    for (auto& [k, c] : g_gs.sizes) {
        char b[64];
        std::snprintf(b, sizeof(b), " %ux%ux%u:%.1f", uint32_t(k >> 32), uint32_t(k >> 8) & 0xFFFFFF, uint32_t(k & 0xFF), c / n);
        sz += b;
    }
    std::fprintf(stderr, "[GPUSTAT] feedback copies per frame by size (w x h x samples: count):%s\n", sz.c_str());
    g_gs = {};
}
}  // namespace

void D3D12Backend::Submit(bool wait) {
    g_gs.submits++;
    cl_->Close();
    ID3D12CommandList* lists[] = {cl_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    queue_->Signal(fence_.Get(), ++fence_value_);
    Frame& f = CurrentFrame();
    f.fence = fence_value_;
    if (wait) {
        auto t0 = std::chrono::steady_clock::now();
        WaitFence(fence_value_);
        g_gs.idle_waits++;
        g_gs.idle_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
        f.alloc->Reset();
        f.garbage.clear();
        f.used = 0;
        f.srv = 0;
        cl_->Reset(f.alloc.Get(), nullptr);
        Begin();
    }
}

void D3D12Backend::Flush() {
    g_gs.flushes++;
    Submit(true);
}

void D3D12Backend::WaitFence(uint64_t value) {
    fence_->SetEventOnCompletion(value, event_);
    WaitForSingleObject(event_, INFINITE);
}

void D3D12Backend::SetFullViewport(uint32_t w, uint32_t h) {
    D3D12_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    D3D12_RECT scissor{0, 0, LONG(w), LONG(h)};
    cl_->RSSetViewports(1, &vp);
    cl_->RSSetScissorRects(1, &scissor);
}

// one w x h colour target for a Blit, the draw's bound targets forgotten
void D3D12Backend::SetBlitTarget(D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t w, uint32_t h) {
    cl_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    bound_rt_ = bound_ds_ = nullptr;
    SetFullViewport(w, h);
}

D3D12Backend::Alloc D3D12Backend::Upload(uint64_t size, uint64_t align) {
    Frame& f = CurrentFrame();
    uint64_t o = (f.used + align - 1) & ~(align - 1);
    if (o + size <= kUploadSize) {
        f.used = o + size;
        return {f.upload.Get(), o, f.map + o, f.upload->GetGPUVirtualAddress() + o};
    }
    // too big for the ring: a buffer of its own, released with the frame
    ComPtr<ID3D12Resource> r;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
    constexpr uint64_t kOwnAlign = 0x10000;  // a committed buffer's size granularity
    D3D12_RESOURCE_DESC bd = BufferDesc((size + kOwnAlign - 1) & ~(kOwnAlign - 1));
    dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                  IID_PPV_ARGS(&r));
    uint8_t* p = nullptr;
    r->Map(0, nullptr, reinterpret_cast<void**>(&p));
    f.garbage.push_back(r);
    return {r.Get(), 0, p, r->GetGPUVirtualAddress()};
}

uint32_t D3D12Backend::AllocSrvSlots(uint32_t n) {
    Frame& f = CurrentFrame();
    const uint32_t per = kSrvCount / kFrames;
    if (f.srv + n > per) {
        LOG_FIRST(4, "d3d12: descriptor ring wrapped within a frame");
        f.srv = 0;
    }
    uint32_t i = uint32_t(frame_ % kFrames) * per + f.srv;
    f.srv += n;
    return i;
}

void D3D12Backend::Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES& cur, D3D12_RESOURCE_STATES want) {
    if (cur == want) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = cur;
    b.Transition.StateAfter = want;
    cl_->ResourceBarrier(1, &b);
    cur = want;
}

// a resized target gives back its view and its resources
void D3D12Backend::ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it) {
    if (bound_rt_ == &it->second || bound_ds_ == &it->second) bound_rt_ = bound_ds_ = nullptr;
    (it->second.depth ? free_dsv_ : free_rtv_).push_back(it->second.view);
    CurrentFrame().garbage.push_back(it->second.res);
    if (it->second.fb) CurrentFrame().garbage.push_back(it->second.fb);
    targets_.erase(it);
}

// the host texture of a new target, at render scale or the shadow resolution
bool D3D12Backend::CreateTargetResource(Target& t, bool shadow, bool blur) {
    float sc = shadow && shadow_res_ ? float(shadow_res_) / t.gw : scale_;
    t.w = std::max(1u, uint32_t(std::lround(t.gw * sc)));
    t.h = std::max(1u, uint32_t(std::lround(t.gh * sc)));
    if (shadow) LOGI("d3d12: shadow map %ux%u", t.w, t.h);
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_UNKNOWN, t.w, t.h);
    // the scene, its depth and the colour surfaces drawn with that depth; the game's own maps and chains stay as
    // they are
    bool screen = t.gw == guest_w_ && t.gh == guest_h_;
    t.samples = screen ? msaa_ : 1;
    td.SampleDesc.Count = t.samples;
    D3D12_CLEAR_VALUE cv{};
    if (t.depth) {
        td.Format = DXGI_FORMAT_R24G8_TYPELESS;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        cv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        cv.DepthStencil.Depth = 1;
        t.state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    } else {
        // the back buffer sized colour targets: the scene and the game's copies of it
        t.fmt = hdr_ && !blur && screen ? kHdrFormat : kBgra;
        td.Format = t.fmt;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        cv.Format = td.Format;
        t.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    return SUCCEEDED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, t.state, &cv,
                                                   IID_PPV_ARGS(&t.res)));
}

// a view from the free pool, cleared; false when the pool is empty
bool D3D12Backend::CreateTargetView(Target& t) {
    auto& pool = t.depth ? free_dsv_ : free_rtv_;
    if (pool.empty()) return false;
    t.view = pool.back();
    pool.pop_back();
    if (t.depth) {
        D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dv.ViewDimension = t.samples > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
        dev_->CreateDepthStencilView(t.res.Get(), &dv, Cpu(dsv_heap_.Get(), t.view, dsv_inc_));
        float one = 1;
        cl_->ClearDepthStencilView(Cpu(dsv_heap_.Get(), t.view, dsv_inc_),
                                   D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, one, 0, 0, nullptr);
    } else {
        dev_->CreateRenderTargetView(t.res.Get(), nullptr, Cpu(rtv_heap_.Get(), t.view, rtv_inc_));
        float black[4] = {0, 0, 0, 0};
        cl_->ClearRenderTargetView(Cpu(rtv_heap_.Get(), t.view, rtv_inc_), black, 0, nullptr);
    }
    return true;
}

// targets_ key of a surface's colour or depth target
uint64_t TargetKey(uint32_t data, bool depth) { return uint64_t(data) << 1 | (depth ? 1 : 0); }

// the shadow map's depth surface, or with depth false its colour surface
bool IsShadowSurface(const Surface& s, bool depth) {
    const ShadowTargets& sh = g_shadow;
    return sh.blur_hi && s.data == (depth ? sh.map : sh.color);
}

D3D12Backend::TargetSlot D3D12Backend::FindTargetSlot(const Surface& s, bool depth) {
    TargetSlot slot;
    slot.data = mem::VirtualFromPhysical(s.data);
    slot.key = TargetKey(slot.data, depth);
    SurfaceSize(s, slot.gw, slot.gh);
    slot.it = targets_.find(slot.key);
    slot.sized = slot.it != targets_.end() && slot.it->second.gw == slot.gw && slot.it->second.gh == slot.gh;
    return slot;
}

// the surface's target at its size, null when there is none
Target* D3D12Backend::FindTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    // the shadow map's colour surface has no memory and colour writes off: depth alone is bound
    if (IsShadowSurface(s, depth) && !depth) return nullptr;
    TargetSlot slot = FindTargetSlot(s, depth);
    return slot.sized ? &slot.it->second : nullptr;
}

// the surface's target, made again at its size when that changed
Target* D3D12Backend::EnsureTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    // the shadow map's colour surface has no memory and colour writes off: depth alone is bound
    const ShadowTargets& sh = g_shadow;
    bool shadow = IsShadowSurface(s, depth);
    if (shadow && !depth) return nullptr;
    // the edge blur reads the scene: it follows the screen, unless the console's shadows are asked for
    bool blur = sh.blur_hi && !depth && shadow_res_ != kConsoleShadows && s.data >= sh.blur_lo && s.data < sh.blur_hi;
    TargetSlot slot = FindTargetSlot(s, depth);
    if (slot.sized) return &slot.it->second;
    if (slot.it != targets_.end()) ReleaseTarget(slot.it);
    Target t;
    t.depth = depth;
    t.gw = slot.gw;
    t.gh = slot.gh;
    if (!CreateTargetResource(t, shadow, blur)) return nullptr;
    // an empty pool drops the new resource with t
    if (!CreateTargetView(t)) return nullptr;
    t.data = slot.data;
    t.bytes = GuestPitch(slot.gw) * slot.gh;
    if (depth) t.bytes = slot.gw * slot.gh * 4;
    t.epoch = DirtyEpoch(slot.data);
    t.sig = MemSig(slot.data, t.bytes);
    auto& ref = targets_[slot.key] = std::move(t);
    return &ref;
}

// the sample count the adapter has for the scene's formats, the one asked for or the next under it
uint32_t D3D12Backend::Samples(uint32_t want) {
    uint32_t n = std::clamp(want, 1u, 8u);
    auto has = [&](DXGI_FORMAT f) {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q{f, n};
        return SUCCEEDED(dev_->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &q, sizeof(q))) &&
               q.NumQualityLevels;
    };
    while (n > 1 && !(has(kBgra) && has(kHdrFormat) && has(DXGI_FORMAT_D24_UNORM_S8_UINT))) n--;
    LOGI("d3d12: MSAA %ux (asked %u)", n, want);
    return n;
}

bool D3D12Backend::BindTargets(const State& s) {
    Target* rt = EnsureTarget(s.rt, false);
    Target* ds = s.rs[kRsZEnable] || s.rs[kRsStencilEnable] ? EnsureTarget(s.zs, true) : nullptr;
    if (rt && ds && (rt->w != ds->w || rt->h != ds->h || rt->samples != ds->samples)) ds = nullptr;
    if (!rt && !ds) return false;
    if (rt) {
        if (LatchCpuWrite(*rt)) UploadTarget(*rt, s.rt);
        Transition(rt->res.Get(), rt->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        rt->drawn = true;
    }
    if (ds) {
        Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        ds->drawn = true;
    }
    if (rt != bound_rt_ || ds != bound_ds_) {
        D3D12_CPU_DESCRIPTOR_HANDLE rh = rt ? Cpu(rtv_heap_.Get(), rt->view, rtv_inc_) : D3D12_CPU_DESCRIPTOR_HANDLE{};
        D3D12_CPU_DESCRIPTOR_HANDLE dh = ds ? Cpu(dsv_heap_.Get(), ds->view, dsv_inc_) : D3D12_CPU_DESCRIPTOR_HANDLE{};
        cl_->OMSetRenderTargets(rt ? 1 : 0, rt ? &rh : nullptr, FALSE, ds ? &dh : nullptr);
        bound_rt_ = rt;
        bound_ds_ = ds;
    }
    return true;
}

// the pixels the CPU changed since the last Sync, opaque, the rest 0; t.read takes them
std::vector<uint8_t> LatchCpuChanges(Target& t) {
    std::vector<uint8_t> px(t.read.size(), 0);
    uint32_t guest_pitch = GuestPitch(t.gw);
    const uint8_t* guest = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++) {
        for (uint32_t x = 0; x < t.gw; x++) {
            const uint8_t* c = guest + size_t(y) * guest_pitch + x * 4;
            size_t i = (size_t(y) * t.gw + x) * 4;
            if (!std::memcmp(c, &t.read[i], 4)) continue;
            std::memcpy(&t.read[i], c, 4);
            std::memcpy(&px[i], c, 3);
            px[i + 3] = 255;
        }
    }
    return px;
}

// BGRA pixels into a new w x h texture, left as a shader resource
ComPtr<ID3D12Resource> D3D12Backend::CreatePixelTexture(const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w,
                                                        uint32_t h) {
    ComPtr<ID3D12Resource> tex;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_B8G8R8A8_UNORM, w, h);
    dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                  IID_PPV_ARGS(&tex));
    uint32_t row = UploadPitch(w * 4);
    Alloc a = Upload(uint64_t(row) * h, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    for (uint32_t y = 0; y < h; y++) std::memcpy(a.cpu + size_t(y) * row, &px[size_t(y) * pitch], w * 4);
    D3D12_TEXTURE_COPY_LOCATION dst{tex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION src{a.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    src.PlacedFootprint.Offset = a.off;
    src.PlacedFootprint.Footprint = {DXGI_FORMAT_B8G8R8A8_UNORM, w, h, 1, row};
    cl_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COPY_DEST;
    Transition(tex.Get(), st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return tex;
}

// CPU pixels of a color surface into its host target, scaled
void D3D12Backend::UploadTarget(Target& t, const Surface& s) {
    TextureDesc d;
    if (!Describe(s.header, d) || Format(d.format).depth) return;
    std::vector<uint8_t> px;
    uint32_t pitch;
    // after a Sync only the pixels the CPU changed go over the picture, which stays at render scale
    bool over = !t.drawn && d.width == t.gw && d.height == t.gh && t.read.size() == size_t(t.gw) * t.gh * 4;
    if (over) {
        LOG_FIRST(4, "d3d12: CPU pixels drawn over target %08X", t.data);
        pitch = t.gw * 4;
        px = LatchCpuChanges(t);
    } else {
        DecodeLevel(d, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr, px, pitch);
    }
    ComPtr<ID3D12Resource> tex = CreatePixelTexture(px, pitch, d.width, d.height);
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    SetBlitTarget(Cpu(rtv_heap_.Get(), t.view, rtv_inc_), t.w, t.h);
    float uv[4] = {0, 0, 1, 1};
    float mul = t.hdr ? 1 / kHeadroom : 1;
    Blit(tex.Get(), kBgra, uv, {.alpha = true, .mul = mul, .rtv = t.fmt, .over = over, .samples = t.samples});
    CurrentFrame().garbage.push_back(tex);
    t.epoch = DirtyEpoch(t.data);
    t.sig = MemSig(t.data, t.bytes);
}

// samplers_ key of the full screen passes' samplers, apart from the stage samplers' hashed keys
constexpr uint64_t kBlitSamplerKey = 0xB117;

// four clamp samplers: linear at slot 0, point at slots 1 to 3
uint32_t D3D12Backend::EnsureBlitSamplers() {
    uint64_t key = kBlitSamplerKey;
    auto it = samplers_.find(key);
    uint32_t si;
    if (it == samplers_.end()) {
        if (smp_used_ + 4 > kSamplerCount) {
            samplers_.clear();
            smp_used_ = 0;
        }
        si = smp_used_;
        smp_used_ += 4;
        for (int i = 0; i < 4; i++) {
            D3D12_SAMPLER_DESC sd{};
            sd.Filter = i == 0 ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sd.MaxLOD = D3D12_FLOAT32_MAX;
            dev_->CreateSampler(&sd, Cpu(smp_heap_.Get(), si + i, smp_inc_));
        }
        samplers_[key] = si;
    } else {
        si = it->second;
    }
    return si;
}

// the full screen triangle: cb at b0 and b1, views at srv_slot; without a sampler slot EnsureBlitSamplers gives it
void D3D12Backend::DrawFullScreen(ID3D12PipelineState* pso, D3D12_GPU_VIRTUAL_ADDRESS cb, uint32_t srv_slot,
                                  std::optional<uint32_t> sampler_slot) {
    cl_->SetPipelineState(pso);
    cl_->SetGraphicsRootConstantBufferView(0, cb);
    cl_->SetGraphicsRootConstantBufferView(1, cb);
    cl_->SetGraphicsRootDescriptorTable(2, Gpu(srv_heap_.Get(), srv_slot, srv_inc_));
    // without a slot the passes take the blit samplers, looked up at this point as they always were
    uint32_t si = sampler_slot ? *sampler_slot : EnsureBlitSamplers();
    cl_->SetGraphicsRootDescriptorTable(3, Gpu(smp_heap_.Get(), si, smp_inc_));
    cl_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl_->DrawInstanced(3, 1, 0, 0);
}

// xboxrecomp: colour-blind correction as one linear RGB matrix (daltonizing): what a viewer with the deficiency loses,
// c - S c (S Machado, Oliveira and Fernandes' simulation at full severity), moved by E into channels they still tell
// apart and added back, scaled by the strength: M = I + k E (I - S). Rows into out[0..11], out[3] 1 when on.
void ColorBlindMatrix(const app::Settings& set, float out[12]) {
    for (int i = 0; i < 12; i++) out[i] = 0;
    if (!set.colorblind || set.colorblind > 3 || !set.colorblind_strength) return;
    static const float kSim[3][9] = {
        {0.152286f, 1.052583f, -0.204868f, 0.114503f, 0.786281f, 0.099216f, -0.003882f, -0.048116f, 1.051998f},
        {0.367322f, 0.860646f, -0.227968f, 0.280085f, 0.672501f, 0.047413f, -0.011820f, 0.042940f, 0.968881f},
        {1.255528f, -0.076749f, -0.178779f, -0.078411f, 0.930809f, 0.147602f, 0.004733f, 0.691367f, 0.303900f}};
    // red-green losses go to green and blue; a blue-yellow loss to red and green
    static const float kErrRG[9] = {0, 0, 0, 0.7f, 1, 0, 0.7f, 0, 1};
    static const float kErrBY[9] = {1, 0, 0.7f, 0, 1, 0.7f, 0, 0, 0};
    const float* S = kSim[set.colorblind - 1];
    const float* E = set.colorblind == app::kCbTritanopia ? kErrBY : kErrRG;
    float k = std::min(set.colorblind_strength, 100u) / 100.0f, d[9];
    for (int i = 0; i < 9; i++) d[i] = (i % 4 == 0 ? 1.0f : 0.0f) - S[i];  // I - S
    for (int r = 0; r < 3; r++)
        for (int col = 0; col < 3; col++) {
            float e = 0;
            for (int j = 0; j < 3; j++) e += E[r * 3 + j] * d[j * 3 + col];
            out[r * 4 + col] = (r == col ? 1.0f : 0.0f) + k * e;
        }
    out[3] = 1;
}

// xboxrecomp: the grading sliders off their neutral places
bool GradingOn(const app::Settings& s) { return s.saturation != 100 || s.vibrance != 0 || s.warmth != 50; }

void D3D12Backend::Blit(ID3D12Resource* src, DXGI_FORMAT fmt, const float uvrect[4], const BlitOptions& o) {
    uint32_t slot = AllocSrvSlots(2);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = fmt;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    dev_->CreateShaderResourceView(src, &sv, Cpu(srv_heap_.Get(), slot, srv_inc_));
    sv.Format = kBgra;
    dev_->CreateShaderResourceView(lut_.Get(), &sv, Cpu(srv_heap_.Get(), slot + 1, srv_inc_));
    uint32_t si = EnsureBlitSamplers();
    Alloc cb = Upload(256, 256);
    float c[28] = {uvrect[0], uvrect[1], uvrect[2], uvrect[3],
                   o.gamma ? 1.0f : 0.0f, o.alpha ? 1.0f : 0.0f, float(o.taps), o.mul};
    // xboxrecomp: the colour-blind correction, sharpening and grading on the finished frame (the pass through the
    // gamma lookup)
    if (o.gamma) {
        const app::Settings& set = app::Get();
        ColorBlindMatrix(set, c + 8);
        if (GradingOn(set)) {
            c[20] = float(std::min(set.saturation, 200u)) / 100.0f;
            c[21] = float(std::min(set.vibrance, 100u)) / 100.0f;
            c[22] = (float(std::min(set.warmth, 100u)) - 50.0f) / 50.0f;
            c[23] = 1;
        }
        c[24] = float(std::min(set.sharpen, 100u)) / 100.0f;
        c[25] = float(std::min(set.vignette, 100u)) / 100.0f;
        c[26] = float(std::min(set.grain, 100u)) / 100.0f;
        c[27] = float(presents_ % 997);
    }
    std::memcpy(cb.cpu, c, sizeof(c));
    DrawFullScreen(EnsureBlitPipeline(o.rtv, o.over, o.samples), cb.gpu, slot, si);
}

constexpr uint64_t kSourceCheck = 0x5CDA0002C0FFEE11ull;  // seed of the second source hash the disk cache keeps

ComPtr<ID3DBlob> CompileHlsl(const std::string& src, const std::string& entry, const std::string& profile,
                             uint64_t hash) {
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> err;
    HRESULT hr = D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, entry.c_str(), profile.c_str(),
                            D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &err);
    if (FAILED(hr)) {
        LOGE("d3d12: %s compile failed: %s", profile.c_str(),
             err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        LOGE("%s", src.c_str());
        return nullptr;
    }
    cache::AddShader(hash, Hash(src.data(), src.size(), kSourceCheck), code.Get());
    return code;
}

uint64_t D3D12Backend::Register(std::string src, const char* entry, const char* profile) {
    // one source can hold several entry points
    uint64_t profile_seed = Hash(profile, std::strlen(profile));
    uint64_t entry_seed = Hash(entry, std::strlen(entry), profile_seed);
    uint64_t hash = Hash(src.data(), src.size(), entry_seed);
    std::lock_guard lock(shader_lock_);
    auto [it, fresh] = shaders_.try_emplace(hash);
    if (fresh) {
        it->second.check = Hash(src.data(), src.size(), kSourceCheck);
        it->second.text = std::move(src);
        it->second.entry = entry;
        it->second.profile = profile;
    }
    return hash;
}

ID3DBlob* D3D12Backend::EnsureShaderCode(uint64_t hash, bool compile, bool& pending) {
    std::unique_lock lock(shader_lock_);
    Source& s = shaders_[hash];
    if (s.state == 0 && !s.no_base) {
        s.code = cache::FindShader(hash, Hash(s.text.data(), s.text.size(), kSourceCheck), &s.base);
        if (s.code) {
            s.state = 2;
            if (!s.base) s.text = {};
        }
    }
    while (s.state == 1) {
        if (!compile) {
            pending = true;
            return nullptr;
        }
        shader_done_.wait(lock);
    }
    if (s.state == 2) return s.code.Get();
    if (!compile) {
        pending = true;
        return nullptr;
    }
    s.state = 1;
    std::string text = std::move(s.text);
    lock.unlock();
    ComPtr<ID3DBlob> code = CompileHlsl(text, s.entry, s.profile, hash);
    lock.lock();
    s.code = std::move(code);
    s.state = 2;
    shader_done_.notify_all();
    return s.code.Get();
}

// xboxrecomp (from HP:PS): shipped bytecode the driver will not take goes, and this PC's compiler makes it, once
bool D3D12Backend::DropBaseCode(uint64_t hash) {
    std::lock_guard lock(shader_lock_);
    auto it = shaders_.find(hash);
    if (it == shaders_.end() || !it->second.base || it->second.state != 2) return false;
    Source& s = it->second;
    LOGW("shader cache: driver refused base shader %016llx (%s), compiling it here", (unsigned long long)hash,
         s.profile.c_str());
    retired_code_.push_back(std::move(s.code));
    s.base = false;
    s.no_base = true;
    s.state = 0;
    return true;
}

ID3DBlob* D3D12Backend::Compile(const std::string& src, const char* entry, const char* profile) {
    uint64_t hash = Register(src, entry, profile);
    bool pending = false;
    return EnsureShaderCode(hash, true, pending);
}

// a full screen pass's pipeline: solid, no culling, one target, writing all channels; callers set the blend
D3D12_GRAPHICS_PIPELINE_STATE_DESC D3D12Backend::FullScreenPipelineDesc(ID3DBlob* vs, ID3DBlob* ps, DXGI_FORMAT rtv,
                                                                        uint32_t samples) const {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = root_.Get();
    d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = rtv;
    d.SampleDesc.Count = samples;
    return d;
}

ID3D12PipelineState* D3D12Backend::EnsureBlitPipeline(DXGI_FORMAT rtv, bool over, uint32_t samples) {
    auto& pso = blit_psos_[uint64_t(rtv) | uint64_t(over) << 32 | uint64_t(samples) << 40];
    if (pso) return pso.Get();
    ID3DBlob* vs = Compile(kBlitHlsl, "vs", "vs_5_0");
    ID3DBlob* ps = Compile(kBlitHlsl, "psmain", "ps_5_0");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = FullScreenPipelineDesc(vs, ps, rtv, samples);
    if (over) {
        auto& blend = d.BlendState.RenderTarget[0];
        // premultiplied colour, the target keeps its alpha
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_ONE;
        blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
        blend.DestBlendAlpha = D3D12_BLEND_ONE;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
    return pso.Get();
}

// DrawPostPass's pipeline for the pass, kept in blit_psos_; null when it can't be made
ID3D12PipelineState* D3D12Backend::EnsurePostPipeline(const char* entry, const PostTarget& out,
                                                      const PostOptions& o) {
    uint64_t seed = uint64_t(out.fmt) << 1 | (o.blend >= 0 ? 1 : 0) | uint64_t(out.samples) << 40;
    auto& pso = blit_psos_[Hash(entry, std::strlen(entry), seed)];
    if (pso) return pso.Get();
    ID3DBlob* vs = Compile(o.hlsl, o.vs_entry, "vs_5_0");
    ID3DBlob* ps = Compile(o.hlsl, entry, "ps_5_0");
    if (!vs || !ps) return nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = FullScreenPipelineDesc(vs, ps, out.fmt, out.samples);
    if (o.blend >= 0) {
        auto& b = d.BlendState.RenderTarget[0];
        b.BlendEnable = TRUE;
        b.SrcBlend = b.SrcBlendAlpha = D3D12_BLEND_BLEND_FACTOR;
        b.DestBlend = b.DestBlendAlpha = D3D12_BLEND_INV_BLEND_FACTOR;
        b.BlendOp = b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    if (FAILED(dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) return nullptr;
    return pso.Get();
}

bool D3D12Backend::DrawPostPass(const char* entry, const PostTarget& out, const PostInputs& in, const float p[8],
                                const PostOptions& o) {
    ID3D12PipelineState* pso = EnsurePostPipeline(entry, out, o);
    if (!pso) return false;
    g_gs.post++;
    uint32_t slot = AllocSrvSlots(4);
    for (int i = 0; i < 4; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = i < 3 && in.res[i] ? in.fmt[i] : kBgra;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        dev_->CreateShaderResourceView(i < 3 ? in.res[i] : nullptr, &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
    Alloc cb = Upload(256, 256);
    std::memcpy(cb.cpu, p, 8 * sizeof(float));
    cl_->OMSetRenderTargets(1, &out.rtv, FALSE, nullptr);
    SetFullViewport(out.w, out.h);
    if (o.blend >= 0) {
        float f[4] = {o.blend, o.blend, o.blend, o.blend};
        cl_->OMSetBlendFactor(f);
    }
    DrawFullScreen(pso, cb.gpu, slot);
    bound_rt_ = bound_ds_ = nullptr;
    return true;
}

// a single sample copy of a colour target to read: while drawing to it, or resolved when it is multisampled
ID3D12Resource* D3D12Backend::EnsureFeedbackCopy(Target& t) {
    bool ms = t.samples > 1;
    if (!t.fb) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td = t.res->GetDesc();
        td.Flags = D3D12_RESOURCE_FLAG_NONE;
        td.SampleDesc.Count = 1;
        td.Alignment = 0;
        t.fb_state = D3D12_RESOURCE_STATE_COPY_DEST;
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, t.fb_state, nullptr,
                                                 IID_PPV_ARGS(&t.fb)))) {
            return nullptr;
        }
    } else if (ms && t.state == D3D12_RESOURCE_STATE_RESOLVE_SOURCE) {
        // every write takes the target out of this state: the last resolve still holds
        Transition(t.fb.Get(), t.fb_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        return t.fb.Get();
    }
    Transition(t.res.Get(), t.state, ms ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE : D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(t.fb.Get(), t.fb_state, ms ? D3D12_RESOURCE_STATE_RESOLVE_DEST : D3D12_RESOURCE_STATE_COPY_DEST);
    if (ms) cl_->ResolveSubresource(t.fb.Get(), 0, t.res.Get(), 0, t.fmt), g_gs.resolves++;
    else cl_->CopyResource(t.fb.Get(), t.res.Get()), g_gs.copies++;
    (ms ? g_gs.resolve_bytes : g_gs.copy_bytes) += uint64_t(t.w) * t.h * 4 * t.samples;
    g_gs.sizes[uint64_t(t.w) << 32 | uint64_t(t.h) << 8 | t.samples]++;
    Transition(t.fb.Get(), t.fb_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return t.fb.Get();
}

// what a read of the target takes, and its state: the target itself, or a multisampled one's resolved copy
D3D12Backend::Readable D3D12Backend::ReadableResource(Target& t) {
    D3D12_RESOURCE_STATES* state = t.samples > 1 ? &t.fb_state : &t.state;
    if (t.samples == 1) return {t.res.Get(), state};
    if (t.depth) LOG_FIRST(4, "d3d12: multisampled depth %08X read, left out", t.data);
    return {t.depth ? nullptr : EnsureFeedbackCopy(t), state};
}

// the first 3D draw of a frame: what the target holds goes over the headroom
void D3D12Backend::EnterHdr(Target& t) {
    t.hdr_frame = presents_;
    ID3D12Resource* fb = EnsureFeedbackCopy(t);
    if (!fb) return;
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    SetBlitTarget(Cpu(rtv_heap_.Get(), t.view, rtv_inc_), t.w, t.h);
    float uv[4] = {0, 0, 1, 1};
    Blit(fb, t.fmt, uv, {.alpha = true, .mul = 1 / kHeadroom, .rtv = t.fmt, .samples = t.samples});
    t.hdr = true;
}

// HDR resolve: bloom at a quarter of the target, exposure adapting over about a second
constexpr uint32_t kBloomShrink = 4;
constexpr DXGI_FORMAT kBloomFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT kExposureFormat = DXGI_FORMAT_R16_FLOAT;
constexpr float kExposureRate = 1.5f;     // per second
constexpr float kExposureMaxStep = 0.25f;  // longest frame time one step follows, in seconds
constexpr float kExposureKey = 0.4f;       // the average luminance exposure aims for
constexpr float kLowestExposure = 0.7f;    // under tone mapping; without it exposure stays at the highest
constexpr float kHighestExposure = 1.0f;
constexpr float kBloomStrength = 0.6f;

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Backend::PostRtv(uint32_t i) {
    return Cpu(rtv_heap_.Get(), post_rtv_ + i, rtv_inc_);
}

// a post pass target in the render target state with its view at PostRtv(view); the one it replaces goes to garbage
bool D3D12Backend::CreatePostTarget(ComPtr<ID3D12Resource>& r, D3D12_RESOURCE_STATES& st, DXGI_FORMAT f, uint32_t w,
                                    uint32_t h, uint32_t view) {
    if (r) CurrentFrame().garbage.push_back(r);
    r.Reset();
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(f, w, h);
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    st = D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, st, nullptr, IID_PPV_ARGS(&r)))) {
        return false;
    }
    dev_->CreateRenderTargetView(r.Get(), nullptr, PostRtv(view));
    return true;
}

bool D3D12Backend::EnsureBloomTargets(uint32_t bw, uint32_t bh) {
    if (bloom_[0] && bloom_w_ == bw && bloom_h_ == bh) return true;
    bloom_w_ = bw;
    bloom_h_ = bh;
    for (int i = 0; i < 2; i++) {
        if (!CreatePostTarget(bloom_[i], bloom_state_[i], kBloomFormat, bw, bh, kBloomRtv + i)) return false;
    }
    return true;
}

bool D3D12Backend::EnsureExposureTarget() {
    if (expo_) return true;
    if (!CreatePostTarget(expo_, expo_state_, kExposureFormat, 1, 1, kExposureRtv)) return false;
    // new memory can hold NaN, which the blend would keep forever
    float one[4] = {1, 1, 1, 1};
    cl_->ClearRenderTargetView(PostRtv(kExposureRtv), one, 0, nullptr);
    expo_fresh_ = true;
    return true;
}

// how far this frame's exposure moves towards the scene's, as a blend factor
float D3D12Backend::UpdateExposureBlend() {
    auto now = std::chrono::steady_clock::now();
    float dt = std::clamp(std::chrono::duration<float>(now - expo_t_).count(), 0.0f, kExposureMaxStep);
    expo_t_ = now;
    float a = expo_fresh_ ? 1.0f : 1.0f - std::exp(-dt * kExposureRate);
    expo_fresh_ = false;
    return a;
}

// bright parts of in's source into bloom_[0], blurred across then down, left readable
void D3D12Backend::DrawBloom(const PostInputs& in, const Target& t, uint32_t bw, uint32_t bh) {
    Transition(bloom_[0].Get(), bloom_state_[0], D3D12_RESOURCE_STATE_RENDER_TARGET);
    float pb[8] = {kHeadroom, 0, 0, 0, 1.0f / t.w, 1.0f / t.h};
    DrawPostPass("bright", {PostRtv(kBloomRtv), kBloomFormat, bw, bh}, in, pb);
    for (int pass = 0; pass < 2; pass++) {
        int from = pass;
        int to = 1 - pass;
        Transition(bloom_[from].Get(), bloom_state_[from], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Transition(bloom_[to].Get(), bloom_state_[to], D3D12_RESOURCE_STATE_RENDER_TARGET);
        PostInputs b{{bloom_[from].Get(), nullptr, nullptr}, {kBloomFormat, kBloomFormat, kBloomFormat}};
        float pp[8] = {0, 0, 0, 0, pass ? 0 : 1.0f / bw, pass ? 1.0f / bh : 0};
        DrawPostPass("blur", {PostRtv(kBloomRtv + to), kBloomFormat, bw, bh}, b, pp);
    }
    Transition(bloom_[0].Get(), bloom_state_[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

// the frame is read: exposure, bloom and tone mapping take the target back to 0..1
void D3D12Backend::Resolve(Target& t) {
    if (cap_) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_hdr_%08X", cap_i_, t.data);
        CapSave(t, name);
    }
    t.hdr = false;
    ID3D12Resource* fb = EnsureFeedbackCopy(t);
    if (!fb) return;
    uint32_t bw = std::max(1u, t.w / kBloomShrink);
    uint32_t bh = std::max(1u, t.h / kBloomShrink);
    if (!EnsureBloomTargets(bw, bh)) return;
    if (!EnsureExposureTarget()) return;
    float a = UpdateExposureBlend();
    PostInputs in{{fb, nullptr, nullptr}, {t.fmt, kBloomFormat, kExposureFormat}};
    Transition(expo_.Get(), expo_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
    float lowest = tone_ ? kLowestExposure : kHighestExposure;
    float pe[8] = {kHeadroom, kExposureKey, lowest, kHighestExposure};
    DrawPostPass("exposure", {PostRtv(kExposureRtv), kExposureFormat, 1, 1}, in, pe, {.blend = a});
    Transition(expo_.Get(), expo_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    in.res[2] = expo_.Get();
    DrawBloom(in, t, bw, bh);
    in.res[1] = bloom_[0].Get();
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    float pt[8] = {kHeadroom, tone_ ? 1.0f : 0.0f, kBloomStrength, 0};
    DrawPostPass("tone", {Cpu(rtv_heap_.Get(), t.view, rtv_inc_), t.fmt, t.w, t.h, t.samples}, in, pt);
}

// SMAA's own targets: edges, then blend weights, then the blended frame in the frame's format
constexpr DXGI_FORMAT kSmaaEdgeFormat = DXGI_FORMAT_R8G8_UNORM;
constexpr DXGI_FORMAT kSmaaWeightFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

// a lookup texture filled from l's data, left in the pixel shader resource state
bool D3D12Backend::CreateLookupTexture(ComPtr<ID3D12Resource>& r, const SmaaLookup& l) {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(l.fmt, l.w, l.h);
    D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COPY_DEST;
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, st, nullptr, IID_PPV_ARGS(&r)))) {
        return false;
    }
    uint32_t row = l.w * l.bpp;
    uint32_t pitch = UploadPitch(row);
    Alloc a = Upload(uint64_t(pitch) * l.h, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    for (uint32_t y = 0; y < l.h; y++) std::memcpy(a.cpu + size_t(y) * pitch, l.data + size_t(y) * row, row);
    D3D12_TEXTURE_COPY_LOCATION dl{r.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION sl{a.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    sl.PlacedFootprint.Offset = a.off;
    sl.PlacedFootprint.Footprint = {l.fmt, l.w, l.h, 1, pitch};
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    Transition(r.Get(), st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return true;
}

bool D3D12Backend::EnsureSmaaLookups() {
    // smaa_textures.h: area 160 x 560 in two channels, search 64 x 16 in one
    constexpr SmaaLookup area{DXGI_FORMAT_R8G8_UNORM, 160, 560, 2, kSmaaArea};
    constexpr SmaaLookup search{DXGI_FORMAT_R8_UNORM, 64, 16, 1, kSmaaSearch};
    static_assert(sizeof(kSmaaArea) == area.w * area.h * area.bpp);
    static_assert(sizeof(kSmaaSearch) == search.w * search.h * search.bpp);
    if (smaa_lut_[1]) return true;
    if (!CreateLookupTexture(smaa_lut_[0], area) || !CreateLookupTexture(smaa_lut_[1], search)) {
        smaa_lut_[0].Reset();
        smaa_lut_[1].Reset();
        return false;
    }
    return true;
}

bool D3D12Backend::EnsureSmaaTargets(DXGI_FORMAT fmt, uint32_t w, uint32_t h) {
    if (smaa_[2] && smaa_w_ == w && smaa_h_ == h && smaa_fmt_ == fmt) return true;
    smaa_w_ = smaa_h_ = 0;
    for (int i = 0; i < 3; i++) {
        if (smaa_[i]) CurrentFrame().garbage.push_back(smaa_[i]);
        smaa_[i].Reset();
    }
    const DXGI_FORMAT f[3] = {kSmaaEdgeFormat, kSmaaWeightFormat, fmt};
    for (int i = 0; i < 3; i++) {
        if (!CreatePostTarget(smaa_[i], smaa_state_[i], f[i], w, h, kSmaaRtv + i)) {
            for (auto& r : smaa_) r.Reset();
            return false;
        }
    }
    smaa_w_ = w;
    smaa_h_ = h;
    smaa_fmt_ = fmt;
    LOGI("d3d12: SMAA %ux%u", w, h);
    return true;
}

// edges, blend weights, then the blend of src into smaa_[2], left readable
bool D3D12Backend::DrawSmaaPasses(ID3D12Resource* src, DXGI_FORMAT fmt, uint32_t w, uint32_t h, const char* hlsl) {
    // edge detection discards where there is no edge, so the edges start clear each frame
    const float clear[4] = {};
    float p[8] = {1.0f / w, 1.0f / h, float(w), float(h)};
    Transition(smaa_[0].Get(), smaa_state_[0], D3D12_RESOURCE_STATE_RENDER_TARGET);
    cl_->ClearRenderTargetView(PostRtv(kSmaaRtv), clear, 0, nullptr);
    PostInputs edges{{src, nullptr, nullptr}, {fmt, fmt, fmt}};
    PostTarget edges_out{PostRtv(kSmaaRtv), kSmaaEdgeFormat, w, h};
    if (!DrawPostPass("smaa_edges", edges_out, edges, p, {.hlsl = hlsl, .vs_entry = "vs_edges"})) return false;
    Transition(smaa_[0].Get(), smaa_state_[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(smaa_[1].Get(), smaa_state_[1], D3D12_RESOURCE_STATE_RENDER_TARGET);
    cl_->ClearRenderTargetView(PostRtv(kSmaaRtv + 1), clear, 0, nullptr);
    PostInputs weights{{smaa_[0].Get(), smaa_lut_[0].Get(), smaa_lut_[1].Get()},
                       {kSmaaEdgeFormat, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8_UNORM}};
    PostTarget weights_out{PostRtv(kSmaaRtv + 1), kSmaaWeightFormat, w, h};
    if (!DrawPostPass("smaa_weights", weights_out, weights, p, {.hlsl = hlsl, .vs_entry = "vs_weights"})) return false;
    Transition(smaa_[1].Get(), smaa_state_[1], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(smaa_[2].Get(), smaa_state_[2], D3D12_RESOURCE_STATE_RENDER_TARGET);
    PostInputs blend{{src, smaa_[1].Get(), nullptr}, {fmt, kSmaaWeightFormat, fmt}};
    PostTarget blend_out{PostRtv(kSmaaRtv + 2), fmt, w, h};
    if (!DrawPostPass("smaa_blend", blend_out, blend, p, {.hlsl = hlsl, .vs_entry = "vs_blend"})) return false;
    Transition(smaa_[2].Get(), smaa_state_[2], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return true;
}

// the frame as it is shown, src in the pixel shader resource state: SMAA's blend of it, null when it can't run
ID3D12Resource* D3D12Backend::Smaa(ID3D12Resource* src, DXGI_FORMAT fmt, uint32_t w, uint32_t h) {
    static const std::string hlsl = std::string(kSmaaConfig) + kSmaaHlsl + kSmaaPasses;
    if (!EnsureSmaaLookups()) return nullptr;
    if (!EnsureSmaaTargets(fmt, w, h)) return nullptr;
    if (!DrawSmaaPasses(src, fmt, w, h, hlsl.c_str())) return nullptr;
    return smaa_[2].Get();
}

// ambient occlusion's target format, and the formats its passes read at t0 to t3: depth, the pass before, stencil,
// then a fourth slot no pass reads
constexpr DXGI_FORMAT kAoFormat = DXGI_FORMAT_R16G16_FLOAT;
constexpr DXGI_FORMAT kAoViewFormats[4] = {DXGI_FORMAT_R24_UNORM_X8_TYPELESS, kAoFormat,
                                           DXGI_FORMAT_X24_TYPELESS_G8_UINT, kBgra};
constexpr float kAoMaxSpan = 0.15f;  // most pixels the radius may span, as a share of the viewport height
constexpr float kAoRadius = 80.0f;   // world units, the engine's are cm

// the viewport in the target's pixels, clamped to it
D3D12_RECT ScaledViewportRect(const Viewport& v, const Target& rt) {
    float sc = float(rt.w) / rt.gw;
    return {std::clamp<LONG>(LONG(v.x * sc), 0, rt.w), std::clamp<LONG>(LONG(v.y * sc), 0, rt.h),
            std::clamp<LONG>(LONG((v.x + v.w) * sc), 0, rt.w), std::clamp<LONG>(LONG((v.y + v.h) * sc), 0, rt.h)};
}

bool D3D12Backend::EnsureAoTargets(uint32_t w, uint32_t h) {
    if (ao_[1] && ao_w_ == w && ao_h_ == h) return true;
    ao_w_ = w;
    ao_h_ = h;
    for (int i = 0; i < 2; i++) {
        if (!CreatePostTarget(ao_[i], ao_state_[i], kAoFormat, ao_w_, ao_h_, kAoRtv + i)) return false;
    }
    return true;
}

// the pass's pipeline, kept in blit_psos_; ms: the depth it reads is multisampled
ID3D12PipelineState* D3D12Backend::EnsureAoPipeline(const AoPass& pass, bool ms) {
    uint64_t seed = uint64_t(pass.out) | uint64_t(ms) << 32 | uint64_t(pass.samples) << 40;
    auto& pso = blit_psos_[Hash(pass.entry, std::strlen(pass.entry), seed)];
    if (pso) return pso.Get();
    std::string text = std::string(ms ? "#define MS 1\n" : "") + "#define NOSTENCIL 1\n" + kAoHlsl;
    ID3DBlob* vs = Compile(text, "vs", "vs_5_0");
    ID3DBlob* ps = Compile(text, pass.entry, "ps_5_0");
    if (!vs || !ps) return nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = FullScreenPipelineDesc(vs, ps, pass.out, pass.samples);
    if (pass.multiply) {
        // the alpha the game's blends read stays
        auto& b = d.BlendState.RenderTarget[0];
        b.BlendEnable = TRUE;
        b.SrcBlend = b.SrcBlendAlpha = D3D12_BLEND_ZERO;
        b.DestBlend = D3D12_BLEND_SRC_COLOR;
        b.DestBlendAlpha = D3D12_BLEND_ONE;
        b.BlendOp = b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    if (FAILED(dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) return nullptr;
    return pso.Get();
}

void D3D12Backend::CreateAoViews(uint32_t slot, const AoPass& pass, bool ms) {
    ID3D12Resource* reads[4] = {pass.depth, pass.src, pass.depth, pass.color};
    for (int i = 0; i < 4; i++) {
        ID3D12Resource* r = reads[i];
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = r ? kAoViewFormats[i] : kBgra;
        // multisampled: the depth and its stencil; the colour dof reads is the resolved copy
        sv.ViewDimension = r && ms && (i == 0 || i == 2) ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (sv.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D) {
            sv.Texture2D.MipLevels = 1;
            sv.Texture2D.PlaneSlice = r && i == 2;  // the stencil plane
        }
        dev_->CreateShaderResourceView(r, &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
}

bool D3D12Backend::DrawAoPass(const AoFrame& f, const AoPass& pass) {
    bool ms = pass.depth && f.depth_samples > 1;
    ID3D12PipelineState* pso = EnsureAoPipeline(pass, ms);
    if (!pso) return false;
    uint32_t slot = AllocSrvSlots(4);
    CreateAoViews(slot, pass, ms);
    decltype(f.c) c;
    std::memcpy(c, f.c, sizeof(c));
    // kAoHlsl's opt.zw: this pass's blur step
    c[14] = pass.step_x;
    c[15] = pass.step_y;
    Alloc cb = Upload(256, 256);
    std::memcpy(cb.cpu, c, sizeof(c));
    cl_->OMSetRenderTargets(1, &pass.rtv, FALSE, nullptr);
    cl_->RSSetViewports(1, &f.vp);
    cl_->RSSetScissorRects(1, &f.rect);
    DrawFullScreen(pso, cb.gpu, slot);
    return true;
}

// occlusion from ds's depth, blurred across, then blurred down and multiplied into rt
void D3D12Backend::DrawOcclusionPasses(Target& rt, Target& ds, const AoFrame& f) {
    bound_rt_ = bound_ds_ = nullptr;
    Transition(ds.res.Get(), ds.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(ao_[0].Get(), ao_state_[0], D3D12_RESOURCE_STATE_RENDER_TARGET);
    const char* entry = f.gtao ? "gtao" : "occlusion";
    AoPass occlusion{.entry = entry, .out = kAoFormat, .rtv = PostRtv(kAoRtv), .depth = ds.res.Get()};
    if (!DrawAoPass(f, occlusion)) return;
    Transition(ao_[0].Get(), ao_state_[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(ao_[1].Get(), ao_state_[1], D3D12_RESOURCE_STATE_RENDER_TARGET);
    AoPass blur{.entry = "occlusion_blur", .out = kAoFormat, .rtv = PostRtv(kAoRtv + 1), .src = ao_[0].Get(),
                .step_x = 1};
    if (!DrawAoPass(f, blur)) return;
    Transition(ao_[1].Get(), ao_state_[1], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(rt.res.Get(), rt.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE scene = Cpu(rtv_heap_.Get(), rt.view, rtv_inc_);
    AoPass apply{.entry = "occlusion_apply", .out = rt.fmt, .samples = rt.samples, .multiply = true, .rtv = scene,
                 .src = ao_[1].Get(), .step_y = 1};
    DrawAoPass(f, apply);
    rt.drawn = true;
}

// p: 1 / the projection's x and y scale, its x and y centre, then view z = p[4] / (ndc depth - p[5])
void D3D12Backend::Occlusion(const State& s, const float p[kOcclusionParams]) { OcclusionOn(s.rt, s.zs, s.viewport, p); }

// xboxrecomp: Occlusion for a target and depth named directly (the scene's end, SceneEnd)
void D3D12Backend::OcclusionOn(const Surface& rts, const Surface& zss, const Viewport& v, const float p[kOcclusionParams]) {
    Target* rt = FindTarget(rts, false);
    Target* ds = FindTarget(zss, true);
    // the frame's own scene only, views drawn to smaller targets are left
    if (!rt || !ds || rt->gw != guest_w_ || rt->gh != guest_h_ || ds->w != rt->w || ds->h != rt->h ||
        ds->samples != rt->samples) {
        return;
    }
    float zr = v.max_z - v.min_z;
    D3D12_RECT rect = ScaledViewportRect(v, *rt);
    if (rect.right <= rect.left || rect.bottom <= rect.top || zr <= 0) return;
    if (!EnsureAoTargets(rt->w, rt->h)) return;
    if (cap_) {
        CapStep(rts);
        fprintf(cap_, "%04u occlusion rt %08X ds %08X\n", cap_i_++, rts.data, zss.data);
    }
    LOG_FIRST(1, "d3d12: ambient occlusion %s %ux%u, %u samples, projection %.4f %.4f, view z %.6f / (depth - %.8f)",
              p[9] ? "GTAO" : "SSAO", rt->w, rt->h, ds->samples, 1 / p[0], 1 / p[1], p[4], p[5]);
    float rect_w = float(rect.right - rect.left);
    float rect_h = float(rect.bottom - rect.top);
    // kAoHlsl's proj, zp, vp, opt and fog; each pass sets its blur step in opt.zw
    AoFrame f{.c = {p[0], p[1], p[2], p[3], p[4] * zr, v.min_z + p[5] * zr, 0, 0,
                    float(rect.left), float(rect.top), rect_w, rect_h, kAoRadius, kAoMaxSpan * rect_h, 0, 0,
                    p[6], p[7], p[8], 0},
              .rect = rect,
              .vp = {0, 0, float(rt->w), float(rt->h), 0, 1},
              .depth_samples = ds->samples,
              .gtao = p[9] != 0};
    DrawOcclusionPasses(*rt, *ds, f);
}

void D3D12Backend::TakeBuilt() {
    std::lock_guard lock(built_lock_);
    built_ready_ = false;
    for (auto& [h, pso] : built_) {
        // a pipeline made meanwhile without waiting stays: command lists hold it
        psos_.emplace(h, std::move(pso));
        building_.erase(h);
    }
    built_.clear();
}

ID3D12PipelineState* D3D12Backend::EnsurePipeline(const PsoKey& k, bool wait) {
    if (built_ready_.load(std::memory_order_acquire)) TakeBuilt();
    uint64_t h = Hash(&k, sizeof(k));
    auto it = psos_.find(h);
    if (it != psos_.end()) return it->second.Get();
    bool async = async_ && !cap_ && !wait;
    if (async && building_.count(h)) {
        skipped_frame_ = true;
        return nullptr;
    }
    bool pending = false;
    ID3DBlob* vs = EnsureShaderCode(k.vs, !async, pending);
    ID3DBlob* ps = EnsureShaderCode(k.ps, !async, pending);
    auto git = k.topo == uint32_t(Topology::Lines) ? line_gs_.find(k.vs) : line_gs_.end();
    uint64_t gh = git != line_gs_.end() ? git->second : 0;
    ID3DBlob* gs = gh ? EnsureShaderCode(gh, !async, pending) : nullptr;
    ComPtr<ID3D12PipelineState> pso;
    if (!pending && vs && ps && (gs || !gh)) pso = CreatePipeline(k, h, vs, ps, gs, !async, pending);
    if (pending) {
        building_.insert(h);
        skipped_frame_ = true;
        cache::Post([this, k, h, gh] {
            bool unused = false;
            ID3DBlob* vs = EnsureShaderCode(k.vs, true, unused);
            ID3DBlob* ps = EnsureShaderCode(k.ps, true, unused);
            ID3DBlob* gs = gh ? EnsureShaderCode(gh, true, unused) : nullptr;
            ComPtr<ID3D12PipelineState> pso;
            if (vs && ps && (gs || !gh)) pso = CreatePipeline(k, h, vs, ps, gs, true, unused);
            std::lock_guard lock(built_lock_);
            built_.emplace_back(h, std::move(pso));
            built_ready_.store(true, std::memory_order_release);
        });
        return nullptr;
    }
    return (psos_[h] = std::move(pso)).Get();
}

// the key's vertex attributes, ATTR0 to ATTR15 in register order; returns how many
uint32_t FillInputLayout(const PsoKey& k, D3D12_INPUT_ELEMENT_DESC el[16]) {
    uint32_t n = 0;
    for (uint32_t r = 0; r < 16; r++) {
        if (!((k.regs >> r) & 1)) continue;
        uint32_t e = k.elements[r];
        el[n++] = {"ATTR", r, DXGI_FORMAT(e & 0xFF), (e >> 8) & 0xFF, e >> 16,
                   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
    }
    return n;
}

D3D12_PRIMITIVE_TOPOLOGY_TYPE TopologyType(uint32_t topo) {
    switch (topo) {
        case 0: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        case 1: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        default: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
}

constexpr uint32_t kFillWireframe = 0x1B01;  // D3DFILL_WIREFRAME
constexpr uint32_t kCullCw = 0x900;          // D3DCULL_CW

void FillDepthStencilDesc(const PsoKey& k, D3D12_DEPTH_STENCIL_DESC& ds) {
    ds.DepthEnable = k.zen;
    ds.DepthWriteMask = k.zwrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = Cmp(k.zfunc);
    ds.StencilEnable = k.sten;
    ds.StencilReadMask = UINT8(k.smask);
    ds.StencilWriteMask = UINT8(k.swmask);
    D3D12_DEPTH_STENCILOP_DESC so{StencilOp(k.sfail), StencilOp(k.szfail), StencilOp(k.spass), Cmp(k.sfunc)};
    ds.FrontFace = ds.BackFace = so;
}

// everything of the pipeline but its input layout
void D3D12Backend::FillPipelineDesc(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps,
                                    D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) const {
    d.pRootSignature = root_.Get();
    d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    auto& b = d.BlendState.RenderTarget[0];
    b.BlendEnable = k.blend;
    b.SrcBlend = Blend(k.src, false);
    b.DestBlend = Blend(k.dst, false);
    b.BlendOp = BlendOp(k.op);
    b.SrcBlendAlpha = Blend(k.src, true);
    b.DestBlendAlpha = Blend(k.dst, true);
    b.BlendOpAlpha = BlendOp(k.op);
    b.RenderTargetWriteMask = UINT8(k.wmask);
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = k.fill == kFillWireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = k.cull ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
    d.RasterizerState.FrontCounterClockwise = k.ccw;
    d.RasterizerState.DepthBias = k.zbias;
    d.RasterizerState.SlopeScaledDepthBias = k.zslope;
    // xboxrecomp: the NV2A clips no geometry by depth, it clamps (as xemu's depth clamp). A draw without the depth test
    // (the shadow map's silhouette, rendered from the light with depths past its far plane) is not cut down to the
    // parts whose depth lands in 0..1 -- kept to those, Harry's shadow was only its outline
    d.RasterizerState.DepthClipEnable = k.zen ? TRUE : FALSE;
    d.RasterizerState.MultisampleEnable = k.msaa != 0;
    FillDepthStencilDesc(k, d.DepthStencilState);
    d.PrimitiveTopologyType = TopologyType(k.topo);
    d.NumRenderTargets = k.has_rt ? 1 : 0;
    d.RTVFormats[0] = k.has_rt ? DXGI_FORMAT(k.rtfmt) : DXGI_FORMAT_UNKNOWN;
    d.DSVFormat = k.has_ds ? DXGI_FORMAT_D24_UNORM_S8_UINT : DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = std::max(k.msaa, 1u);
}

// from the library, else from the driver when compile is set (any thread)
ComPtr<ID3D12PipelineState> D3D12Backend::CreatePipeline(const PsoKey& k, uint64_t h, ID3DBlob* vs, ID3DBlob* ps,
                                                         ID3DBlob* gs, bool compile, bool& pending) {
    D3D12_INPUT_ELEMENT_DESC el[16];
    uint32_t n = FillInputLayout(k, el);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.InputLayout = {el, n};
    FillPipelineDesc(k, vs, ps, d);
    if (gs) d.GS = {gs->GetBufferPointer(), gs->GetBufferSize()};
    // the bytecode's own checksum: another compiler's output for the same source is another pipeline
    auto sum = [](ID3DBlob* b) { return Hash(static_cast<const uint8_t*>(b->GetBufferPointer()) + 4, 16); };
    uint64_t name = Hash(&h, sizeof(h), sum(vs) ^ sum(ps) * 3 ^ (gs ? sum(gs) * 5 : 0));
    ComPtr<ID3D12PipelineState> pso = cache::LoadPipeline(name, d);
    if (pso) {
        RecordPipelineKey(k);
        return pso;
    }
    if (!compile) {
        pending = true;
        return nullptr;
    }
    HRESULT made = dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
    if (FAILED(made)) {
        // xboxrecomp: bytecode from base_shaders.bin this driver won't take: this PC's compiler makes it, once
        bool vs_base = DropBaseCode(k.vs);
        bool ps_base = DropBaseCode(k.ps);
        if (vs_base || ps_base) {
            bool unused = false;
            ID3DBlob* vs2 = EnsureShaderCode(k.vs, true, unused);
            ID3DBlob* ps2 = EnsureShaderCode(k.ps, true, unused);
            if (vs2 && ps2) return CreatePipeline(k, h, vs2, ps2, gs, compile, pending);
        }
        // xboxrecomp: which pipeline and why, as its draws are skipped from now on (psos_ keeps the null): 32 failed
        // at every start on an RTX 3060 under vkd3d-proton (2026-11-01), unnamed. VKD3D_DEBUG=warn says more there
        LOGE("d3d12: pipeline %016llX creation failed (0x%08lX), its draws are skipped: vs %016llX ps %016llX%s, %u "
             "target(s) format %d, depth format %d, %u sample(s), topology type %d", (unsigned long long)h,
             (unsigned long)made, (unsigned long long)k.vs, (unsigned long long)k.ps, gs ? " with a line gs" : "",
             d.NumRenderTargets, int(d.RTVFormats[0]), int(d.DSVFormat), d.SampleDesc.Count, int(d.PrimitiveTopologyType));
        return nullptr;
    }
    cache::StorePipeline(name, pso.Get());
    RecordPipelineKey(k);
    return pso;
}

// xboxrecomp: the pipeline into pipelines_list.bin once (any thread)
void D3D12Backend::RecordPipelineKey(const PsoKey& k) {
    PipelineRecord r{k, 0, 0};
    {
        std::lock_guard lock(shader_lock_);
        auto v = shaders_.find(k.vs), p = shaders_.find(k.ps);
        if (v == shaders_.end() || p == shaders_.end()) return;
        r.vs_check = v->second.check;
        r.ps_check = p->second.check;
    }
    cache::RecordPipeline(Hash(&k, sizeof(k)), &r);
    gpu::g_pipelines_recorded = uint32_t(cache::RecordedPipelineCount());
}

// RECOMP_LINE_GS=0: lines drawn by D3D12 itself (one host pixel, centres at .5), for comparison
static bool LineGsOn() {
    static const bool on = [] { const char* e = std::getenv("RECOMP_LINE_GS"); return !(e && *e == '0'); }();
    return on;
}

// xboxrecomp: every recorded pipeline made on the build threads behind whatever the title asks for, from the shader
// cache's bytecode (shaders.bin, base_shaders.bin), and kept in the pipeline library: the first time a level shows an
// effect it is ready instead of compiled then (the stutter, the held frames). Records for another anti-aliasing
// sample count than this one are left; a record whose shaders are not in the cache is skipped. RECOMP_PREBUILD=0: off
void D3D12Backend::StartPrebuild() {
    cache::OpenPipelineList(sizeof(PipelineRecord));
    gpu::g_pipelines_recorded = uint32_t(cache::RecordedPipelineCount());
    if (const char* e = std::getenv("RECOMP_PREBUILD"); e && *e == '0') return;
    uint32_t msaa = app::Get().msaa;
    std::vector<PipelineRecord> list;
    for (const auto& bytes : cache::RecordedPipelines()) {
        PipelineRecord r;
        std::memcpy(&r, bytes.data(), sizeof(r));
        if (r.k.msaa && r.k.msaa != msaa) continue;
        // a line pipeline carries LineGsHlsl, which the record has no code for: made when first drawn (and left out
        // of the count, or the startup note waited for it forever)
        if (LineGsOn() && r.k.topo == uint32_t(Topology::Lines)) continue;
        list.push_back(r);
    }
    if (list.empty()) return;
    gpu::g_prebuild_total = uint32_t(list.size());
    auto t0 = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
    LOGI("[PIPELINES] making %zu recorded pipelines ahead of need", list.size());
    for (const PipelineRecord& r : list) {
        cache::PostLow([this, r, t0] {
            uint64_t h = Hash(&r.k, sizeof(r.k));
            ComPtr<ID3DBlob> vs = cache::FindShader(r.k.vs, r.vs_check), ps = cache::FindShader(r.k.ps, r.ps_check);
            ComPtr<ID3D12PipelineState> pso;
            bool unused = false;
            if (vs && ps) pso = CreatePipeline(r.k, h, vs.Get(), ps.Get(), nullptr, true, unused);
            if (pso) {
                std::lock_guard lock(built_lock_);
                built_.emplace_back(h, std::move(pso));
                built_ready_.store(true, std::memory_order_release);
            } else {
                gpu::g_prebuild_failed++;  // shaders not in this cache, or the driver refused it: made on demand
            }
            if (++gpu::g_prebuild_done == gpu::g_prebuild_total)
                LOGI("[PIPELINES] %u made ahead in %.1f s, %u could not be (left to be made on demand)",
                     gpu::g_prebuild_total.load() - gpu::g_prebuild_failed.load(),
                     std::chrono::duration<double>(std::chrono::steady_clock::now() - *t0).count(),
                     gpu::g_prebuild_failed.load());
        });
    }
}

TexKind TextureKind(const TextureDesc& d) {
    if (d.cube) return kTexCube;
    if (d.depth > 1) return kTex3D;
    return kTex2D;
}

// console formats without an alpha channel: R5G6B5, X1R5G5B5, X8R8G8B8 and their linear forms
bool NoAlpha(uint32_t format) {
    return format == 0x05 || format == 0x03 || format == 0x07 || format == 0x11 || format == 0x1C || format == 0x1E;
}

constexpr float kUpscaledMin = 1.01f;  // host texels per console texel past rounding: the target is upscaled

ID3D12Resource* D3D12Backend::BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t,
                                                DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    D3D12_SHADER_RESOURCE_VIEW_DESC& sv = stages.sv[stage];
    if (&t == draw_ds_) return nullptr;
    bool feedback = &t == draw_rt_;
    stages.rt[stage] = true;
    if (!t.depth && LatchCpuWrite(t)) {
        Surface su;
        su.header = s.textures[size_t(stage)];
        UploadTarget(t, su);
    }
    info.kind = kTex2D;
    // a pass shrinking it averages each tap over a console texel, else upscaled taps skip texels and ghost
    Target* rt = FindTarget(s.rt, false);
    float k = float(t.w) / t.gw;
    if (!t.depth && k > kUpscaledMin && rt && rt != &t && (t.gw > rt->gw || t.gh > rt->gh)) {
        stages.wide[stage] = k;
        info.flags |= kTexWide;
    }
    if (t.hdr) info.flags |= kTexHdr;
    sv.Format = t.depth ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : t.fmt;
    // xboxrecomp: a target read as a format without alpha reads alpha 1 on the console, whatever the host target
    // holds there. HP:CoS draws its static scenery with a pixel shader that multiplies alpha by its R5G6B5 shadow
    // target's and alpha tests the product: the clear left 0 there and every ground and wall pixel was discarded.
    // (in the shader, kTexOpaque: a swizzled view made vkd3d's sampler border colour black)
    if (!t.depth && NoAlpha(d.format)) info.flags |= kTexOpaque;
    // the console reads the pixels it is about to overwrite: give the draw a copy
    if (feedback) return EnsureFeedbackCopy(t);
    if (t.depth) info.shadow = ShadowDepthKind(d.format);
    Readable r = ReadableResource(t);
    if (r.res) Transition(r.res, *r.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return r.res;
}

bool SameTexture(const Tex& tx, const TextureDesc& d) {
    return tx.res && tx.d.format == d.format && tx.d.width == d.width && tx.d.height == d.height &&
           tx.d.depth == d.depth && tx.d.levels == d.levels && tx.d.cube == d.cube && tx.d.pitch == d.pitch;
}

// false when the texture could not be made, its entry dropped
bool D3D12Backend::CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi) {
    if (tx.res) CurrentFrame().garbage.push_back(tx.res);
    tx = Tex{};
    tx.d = d;
    tx.fmt = HostFormat(fi.host);
    bool bc = fi.compressed;
    uint32_t w = d.width;
    uint32_t h = d.height;
    tx.levels = d.levels;
    if (bc && ((w & 3) || (h & 3))) {
        w = (w + 3) & ~3u;
        h = (h + 3) & ~3u;
        tx.levels = 1;
    }
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td{};
    td.Dimension = d.depth > 1 ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w;
    td.Height = h;
    td.DepthOrArraySize = UINT16(d.cube ? 6 : d.depth);
    td.MipLevels = UINT16(tx.levels);
    td.Format = tx.fmt;
    td.SampleDesc.Count = 1;
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&tx.res)))) {
        textures_.erase(tex_key_);
        return false;
    }
    return true;
}

void UpdateNormCube(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint32_t f, const std::vector<uint8_t>& px,
                   uint32_t pitch) {
    // a normalization cube: each face's centre points along its axis, 255 or 0 there, 128 across
    if (!f) tx.norm = fi.host == Host::BGRA8 && d.width >= 8 && px.size() >= size_t(d.width / 2 + 1) * pitch;
    const uint8_t* c = tx.norm ? &px[size_t(d.width / 2) * pitch + d.width / 2 * 4] : nullptr;
    for (uint32_t ch = 0; c && ch < 3; ch++) {
        int want = 128;
        if (ch == f / 2) want = f & 1 ? 0 : 255;
        tx.norm &= std::abs(c[2 - ch] - want) <= 16;
    }
}

void CopyLevelRows(uint8_t* out, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp, UINT rows, UINT64 row_size,
                   const std::vector<uint8_t>& px, uint32_t pitch) {
    uint32_t slices = fp.Footprint.Depth;
    size_t src_rows = pitch ? px.size() / pitch / std::max(slices, 1u) : 0;
    for (uint32_t z = 0; z < slices; z++) {
        for (uint32_t y = 0; y < rows; y++) {
            uint8_t* dst = out + (size_t(z) * rows + y) * fp.Footprint.RowPitch;
            if (y < src_rows) {
                std::memcpy(dst, &px[(size_t(z) * src_rows + y) * pitch], std::min<size_t>(pitch, row_size));
            } else {
                std::memset(dst, 0, size_t(row_size));
            }
        }
    }
}

void D3D12Backend::UploadTextureLevel(Tex& tx, uint32_t f, uint32_t l, const std::vector<uint8_t>& px,
                                      uint32_t pitch) {
    D3D12_RESOURCE_DESC rd = tx.res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 row_size;
    UINT64 bytes;
    uint32_t sub = f * tx.levels + l;
    dev_->GetCopyableFootprints(&rd, sub, 1, 0, &fp, &rows, &row_size, &bytes);
    Alloc a = Upload(bytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    CopyLevelRows(a.cpu, fp, rows, row_size, px, pitch);
    fp.Offset = a.off;
    D3D12_TEXTURE_COPY_LOCATION dl{tx.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    dl.SubresourceIndex = sub;
    D3D12_TEXTURE_COPY_LOCATION sl{a.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    sl.PlacedFootprint = fp;
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
}

// xboxrecomp: the loading screen's picture (sprites/load_hognight.tga, 512x512 P8) has the loading bar's empty track
// painted into it (rows 423-434, columns 46-466) under the bar the title draws (62-578 x 400-404 of its 640x480).
// Shown 16:9 its middle 360 lines fill the screen (Squeeze2D) and the painted track no longer sits under the drawn
// bar; it is painted over from the rows just above and below. Only while the loading screen is up, only a 512x512
// palette texture, and only when the track is there: a bright line on a dark picture.
extern "C" int hppoa_LoadingScreen(void);
static void RemoveLoadingBarTrack(const TextureDesc& d, std::vector<uint8_t>& px, uint32_t pitch) {
    if (d.format != 0x0B || d.width != 512 || d.height != 512 || pitch < 512 * 4 || px.size() < size_t(pitch) * 512)
        return;
    if (!hppoa_LoadingScreen()) return;
    auto lum = [&](uint32_t x, uint32_t y) {
        const uint8_t* q = &px[size_t(y) * pitch + x * 4];
        return (q[0] + q[1] + q[2]) / 3;
    };
    uint32_t line = 0, below = 0;
    for (uint32_t x = 60; x < 450; x++) line += lum(x, 431), below += lum(x, 437);
    if (!(line > 390 * 30 && line > below * 4)) return;
    const uint32_t y0 = 420, y1 = 437;  // the rows kept above and below the track
    for (uint32_t x = 40; x < 472; x++) {
        const uint8_t* a = &px[size_t(y0) * pitch + x * 4];
        const uint8_t* b = &px[size_t(y1) * pitch + x * 4];
        for (uint32_t y = y0 + 1; y < y1; y++) {
            uint8_t* q = &px[size_t(y) * pitch + x * 4];
            uint32_t t = (y - y0) * 256 / (y1 - y0);
            for (int c = 0; c < 4; c++) q[c] = uint8_t((a[c] * (256 - t) + b[c] * t) >> 8);
        }
    }
    static bool told;
    if (!told) told = true, fprintf(stderr, "[LOAD] painted loading bar track left out of the loading picture\n");
}

// xboxrecomp (hppoa): the Glacius lever's front panel (128x32 DXT3, level 0 hashed AE31DA34, FNV-1a as the captures
// hash it): black around its red indicator dot. The finished lever draws its grey texture lit and adds this panel over
// it (the black adds nothing); before that the panel is drawn alone, texture * TFACTOR, and the title fades the factor
// down, so the lever's front goes black before the grey one replaces it. LeverPanelFade draws that fade toward the
// lever's lit colour instead.
static bool LeverPanel(const TextureDesc& d, const uint8_t* p) {
    if (d.format != 0x0E || d.width != 128 || d.height != 32 || d.cube || !p) return false;
    uint32_t n = LevelSize(d, 0), h = 2166136261u;
    for (uint32_t k = 0; k < n; k++) h = (h ^ p[k]) * 16777619u;
    return h == 0xAE31DA34u;
}

void D3D12Backend::UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p, bool same,
                                 const uint32_t* palette) {
    D3D12_RESOURCE_STATES st = same ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COPY_DEST;
    Transition(tx.res.Get(), st, D3D12_RESOURCE_STATE_COPY_DEST);
    uint32_t faces = d.cube ? 6 : 1;
    std::vector<uint8_t> px;
    // new contents: a panel found again by its finished lever (or its hash), the grey texture with it
    for (auto it = lever_pairs_.begin(); it != lever_pairs_.end();)
        it = it->first >> 32 == d.data ? lever_pairs_.erase(it) : std::next(it);
    if (LeverPanel(d, p)) lever_panels_.insert(d.data), lever_base_ = 0;
    else lever_panels_.erase(d.data);
    for (uint32_t f = 0; f < faces; f++) {
        for (uint32_t l = 0; l < tx.levels; l++) {
            uint32_t pitch;
            DecodeLevel(d, l, f, p, palette, px, pitch);
            if (!l && !d.cube) RemoveLoadingBarTrack(d, px, pitch);
            if (d.cube && !l) UpdateNormCube(tx, d, fi, f, px, pitch);
            UploadTextureLevel(tx, f, l, px, pitch);
        }
    }
    Transition(tx.res.Get(), st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

// xboxrecomp (hppoa): replacement textures. hd_textures\<format>_<width>x<height>_<hash>.bgra beside the executable holds a
// picture for the guest texture whose level 0 has that FNV-1a 64 hash: "HDT1", u32 width, u32 height, then BGRA8 rows. The game
// still sees its own texture; only what is sampled changes, so any size fits (UVs are 0..1). A level is only hashed when a
// file exists for its format and size, so a game without the folder pays nothing. F10 > Enhancements > HD textures.
extern "C" int hppoa_HDTextures(void);

static uint64_t HdSizeKey(uint32_t format, uint32_t w, uint32_t h) {
    return uint64_t(format) << 40 | uint64_t(w) << 20 | h;
}

struct HdIndex {
    std::unordered_map<uint64_t, std::filesystem::path> files;  // by the guest level 0's hash
    std::unordered_set<uint64_t> sizes;
    bool trace = false;
};

static HdIndex& Hd() {
    static HdIndex* idx = [] {
        auto* x = new HdIndex;
        const char* e = std::getenv("RECOMP_HD_TRACE");
        x->trace = e && e[0] == '1';
        std::error_code ec;
        for (const auto& f : std::filesystem::directory_iterator("hd_textures", ec)) {
            unsigned fmt, w, h;
            unsigned long long hash;
            if (std::sscanf(f.path().filename().string().c_str(), "%x_%ux%u_%llx.bgra", &fmt, &w, &h, &hash) != 4) continue;
            x->files[hash] = f.path();
            x->sizes.insert(HdSizeKey(fmt, w, h));
        }
        fprintf(stderr, "[HDTEX] %zu replacement textures in hd_textures\n", x->files.size());
        return x;
    }();
    return *idx;
}

// the replacement for this texture's present contents, 0 for none
static uint64_t HdLookup(const TextureDesc& d, const uint8_t* p) {
    if (!hppoa_HDTextures() || d.cube || d.depth > 1 || d.linear || !p) return 0;
    HdIndex& x = Hd();
    if (!x.sizes.count(HdSizeKey(d.format, d.width, d.height))) return 0;
    uint64_t h = 14695981039346656037ull;
    for (uint32_t n = LevelSize(d, 0), k = 0; k < n; k++) h = (h ^ p[k]) * 1099511628211ull;
    bool found = x.files.count(h) != 0;
    if (x.trace) fprintf(stderr, "[HDTEX] %02X %ux%u at %08X hash %016llX %s\n", d.format, d.width, d.height, d.data,
                         static_cast<unsigned long long>(h), found ? "replaced" : "no file");
    return found ? h : 0;
}

// xboxrecomp (hppoa): Button prompts (F10 Enhancements; input.cpp's app_PromptsKbm). With keyboard prompts the HUD's
// button pictures (the A B X Y circles round Harry's portrait, its spells beside them; 32x32 ARGB, level 0's FNV-1a
// 32 hashes as F12's frame.txt prints them, the same in every run) are drawn as the key bound to that button: a key
// cap with its name, a mouse with that button lit for a mouse button. Each keeps its button's colour as its rim, the
// colour the game ties to the spell on it. Through the replacement path (BindHdTexture), 128x128
extern "C" int app_PromptsKbm(void);               // input.cpp: keyboard and mouse prompts this run
extern "C" uint32_t app_PadInputKey(int pad_input);  // input.cpp: the key bound to a pad input (0 A .. 3 Y)

// a picture of several keys: the movement keys (bindings 16 up, 17 down, 18 left, 19 right), the d-pad's (8 up,
// 9 down, 10 left, 11 right), as small caps in a cross
constexpr int kPromptMove = 100, kPromptDpad = 101;
// the title screen's "Press START" (P8 512x64): "Press" kept, START drawn as the key bound to it
constexpr int kPromptPressStart = 102;
extern "C" uint32_t app_KeyBinding(int binding);  // input.cpp: the key of any binding

struct PromptButton {
    uint32_t hash;
    int input;            // kPadInputNames: 0 A, 1 B, 2 X, 3 Y
    uint8_t r, g, b;      // its colour
};
static const PromptButton kPromptButtons[] = {
    {0x2FC33386u, 0, 0x4C, 0xB0, 0x2C},  // A, green
    {0xDA4A70B1u, 1, 0xD0, 0x2C, 0x2C},  // B, red
    {0x26074503u, 2, 0x2C, 0x58, 0xD0},  // X, blue
    {0xFD673456u, 3, 0xE8, 0xC0, 0x20},  // Y, yellow
    // 2026-11-10: the pictures the text puts in a button character's place (apt_text.txt's U+E001 to U+E009: the
    // tutorials, the inventory, the pause menu), lighter rimmed than the HUD's, by F12 captures' hashes. With these
    // drawn as keys the text keeps its button characters (input.cpp's GlyphIcon) instead of naming the keys in words
    {0x1234B5C9u, 0, 0x4C, 0xB0, 0x2C},  // A in text
    {0x1CF15D4Du, 1, 0xD0, 0x2C, 0x2C},  // B in text
    {0x3881E74Eu, 2, 0x2C, 0x58, 0xD0},  // X in text
    {0xE53A974Du, 3, 0xE8, 0xC0, 0x20},  // Y in text
    {0x38CC4906u, 7, 0x90, 0x90, 0x90},  // right trigger in text ("Pull and hold right trigger")
    {0x2BDC5813u, 7, 0x90, 0x90, 0x90},  // right trigger, the same way round
    {0x11553103u, 6, 0x90, 0x90, 0x90},  // left trigger, mirrored
    // from poa_base.big's text icons (EA SHPX, DXT5 32x32: the FNV-1a of its 1024 bytes at 0x80 is the hash the
    // game's upload gives; A B X Y and R1 checked against the captures'), 2026-11-10
    {0x8E36A035u, 6, 0x90, 0x90, 0x90},  // L1.xsh, left trigger
    {0x5F99E172u, 6, 0x90, 0x90, 0x90},  // L2.xsh, left trigger
    {0x2507E4C0u, 7, 0x90, 0x90, 0x90},  // R2.xsh, right trigger
    {0xF73EAD2Au, 12, 0x90, 0x90, 0x90},  // start.xsh
    {0xFDCDC7DDu, 13, 0x90, 0x90, 0x90},  // select.xsh, BACK
    {0x85A85830u, kPromptMove, 0x90, 0x90, 0x90},  // analog_left.xsh (analog_right.xsh is the same picture): W A S D
    {0x104B630Au, kPromptDpad, 0x90, 0x90, 0x90},  // dpad.xsh: the d-pad's four keys
    {0x73010598u, kPromptPressStart, 0x90, 0x90, 0x90},  // the title screen's "Press START" (P8 indices' hash)
};

// the replacement key for this texture (bit 63 set, the button's index below), 0 for none
static uint64_t PromptLookup(const TextureDesc& d, const uint8_t* p) {
    const bool icon = d.format == 0x0F && d.width == 32 && d.height == 32;
    const bool press = d.format == 0x0B && d.width == 512 && d.height == 64;  // "Press START"
    if (!(icon || press) || d.cube || d.depth > 1 || !p || !app_PromptsKbm()) return 0;
    uint32_t h = 2166136261u;
    for (uint32_t n = LevelSize(d, 0), k = 0; k < n; k++) h = (h ^ p[k]) * 16777619u;
    for (size_t i = 0; i < sizeof kPromptButtons / sizeof kPromptButtons[0]; i++)
        if (kPromptButtons[i].hash == h) return 1ull << 63 | i;
    return 0;
}

// coverage of a rounded rectangle (centre cx cy, half sizes hx hy, corner radius r) at a pixel centre, anti-aliased
static float RoundRect(float x, float y, float cx, float cy, float hx, float hy, float r) {
    float qx = std::fabs(x - cx) - (hx - r), qy = std::fabs(y - cy) - (hy - r);
    float out = std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - r;
    return std::clamp(0.5f - out, 0.0f, 1.0f);
}

static void Over(uint8_t* px, float a, uint8_t r, uint8_t g, uint8_t b) {  // BGRA, straight alpha
    if (a <= 0.0f) return;
    float da = px[3] / 255.0f, oa = a + da * (1 - a);
    auto mix = [&](uint8_t dst, uint8_t src) { return uint8_t((src * a + dst * da * (1 - a)) / oa + 0.5f); };
    px[0] = mix(px[0], b), px[1] = mix(px[1], g), px[2] = mix(px[2], r), px[3] = uint8_t(oa * 255 + 0.5f);
}

// the label's coverage (white on black through GDI), its font shrunk until it fits w x h
static std::vector<uint8_t> LabelMask(const char* text, int size, int w, int h) {
    std::vector<uint8_t> m(size_t(size) * size);
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dc || !bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        if (dc) DeleteDC(dc);
        return m;
    }
    HGDIOBJ old = SelectObject(dc, bmp);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    for (int pt = std::max(h, 8);; pt -= 2) {
        HFONT f = CreateFontA(-pt, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                              ANTIALIASED_QUALITY, DEFAULT_PITCH, "Arial");
        HGDIOBJ of = SelectObject(dc, f);
        SIZE sz{};
        GetTextExtentPoint32A(dc, text, int(std::strlen(text)), &sz);
        if (sz.cx <= w || pt - 2 < 8) {  // the smallest size is drawn whether it fits or not
            std::memset(bits, 0, size_t(size) * size * 4);
            RECT rc{0, 0, size, size};
            DrawTextA(dc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            GdiFlush();
            const uint8_t* b = static_cast<const uint8_t*>(bits);
            for (size_t k = 0; k < m.size(); k++) m[k] = std::max({b[k * 4], b[k * 4 + 1], b[k * 4 + 2]});
            SelectObject(dc, of);
            DeleteObject(f);
            break;
        }
        SelectObject(dc, of);
        DeleteObject(f);
    }
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    return m;
}

// a key as a round button, as the Xbox's own are (HP:PS's and HP:CoS's the same, 2026-11-10): in the N x N BGRA
// picture px, centred at cx cy with radius R, a dark edge, a pale ring and a face of the button's colour lighter toward
// the top left, with the key's name (white with a shadow, dark on a light colour), or for a mouse button a small mouse
// on the face with that button lit in a darker shade of the colour. The shapes are given for R 62 and scaled
static void PromptDisc(std::vector<uint8_t>& px, int N, float cx, float cy, float R, uint32_t vk, uint8_t r, uint8_t g,
                       uint8_t b) {
    const bool mouse = vk == 0x01 || vk == 0x02 || vk == 0x04;
    const float s = R / 62.0f, ms = s * 0.6f;  // the disc's and the mouse's scale
    // a rounded rectangle about the disc's centre, its sizes given at scale 1 and drawn at k
    auto rr = [&](float fx, float fy, float k, float hx, float hy, float rad) {
        return RoundRect(fx, fy, cx, cy, hx * k, hy * k, rad * k);
    };
    auto lift = [](uint8_t c, float hl) { return uint8_t(c + (255 - c) * hl + 0.5f); };
    const int x0 = std::max(0, int(cx - R - 2)), x1 = std::min(N, int(cx + R + 2));
    const int y0 = std::max(0, int(cy - R - 2)), y1 = std::min(N, int(cy + R + 2));
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            uint8_t* o = &px[(size_t(y) * N + x) * 4];
            const float fx = x + 0.5f, fy = y + 0.5f;
            Over(o, rr(fx, fy, s, 62, 62, 62), 0x20, 0x1C, 0x18);
            Over(o, rr(fx, fy, s, 58, 58, 58), 0xE8, 0xDC, 0xB0);
            const float hl =
                std::clamp(1.0f - std::hypot(fx - (cx - 18 * s), fy - (cy - 20 * s)) / (60 * s), 0.0f, 1.0f) * 0.45f;
            Over(o, rr(fx, fy, s, 49, 49, 49), lift(r, hl), lift(g, hl), lift(b, hl));
            if (!mouse) continue;
            // its body, the button pressed, the lines between the buttons (at least a pixel wide however small)
            const float u = 64 + (fx - cx) / ms, v = 64 + (fy - cy) / ms;
            Over(o, rr(fx, fy, ms, 40, 58, 38), 0x20, 0x1C, 0x18);
            Over(o, rr(fx, fy, ms, 34, 52, 32), 0xFF, 0xFC, 0xF4);
            const bool lit = vk == 0x04 ? std::fabs(u - 64) < 7 && v > 20 && v < 52
                                        : (vk == 0x01 ? u < 62 : u > 66) && v < 58;
            if (lit) Over(o, rr(fx, fy, ms, 34, 52, 32), uint8_t(r * 0.6f), uint8_t(g * 0.6f), uint8_t(b * 0.6f));
            if (std::fabs(fy - (cy - 4 * ms)) < std::max(2.5f * ms, 0.75f) && std::fabs(u - 64) < 34)
                Over(o, 1.0f, 0x20, 0x1C, 0x18);
            if (std::fabs(fx - cx) < std::max(2.0f * ms, 0.75f) && v > 12 && v < 60) Over(o, 1.0f, 0x20, 0x1C, 0x18);
        }
    }
    if (mouse) return;
    std::string label = app::KeyName(vk);
    if (label == "Escape") label = "Esc";
    else if (label == "Backspace") label = "Bksp";
    else if (label == "CapsLock") label = "Caps";
    else if (label == "Mouse4") label = "M4";
    else if (label == "Mouse5") label = "M5";
    // the name in the face (a chord of it at the text's height), centred on the disc
    std::vector<uint8_t> m = LabelMask(label.c_str(), N, int(78 * s), int((label.size() == 1 ? 60 : 34) * s));
    const int ox = int(cx) - N / 2, oy = int(cy) - N / 2;
    const bool light = 0.299f * r + 0.587f * g + 0.114f * b > 160;
    const int sh = std::max(1, int(3 * s + 0.5f));
    for (int pass = light ? 1 : 0; pass < 2; pass++)  // the shadow down and to the right, then the name
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++) {
                const uint8_t c = m[size_t(y) * N + x];
                const int qx = x + ox + (pass ? 0 : sh), qy = y + oy + (pass ? 0 : sh);
                if (!c || qx < 0 || qy < 0 || qx >= N || qy >= N) continue;
                uint8_t* o = &px[(size_t(qy) * N + qx) * 4];
                if (!pass) Over(o, c / 255.0f * 0.8f, 0x20, 0x1C, 0x18);
                else if (light) Over(o, c / 255.0f, 0x22, 0x1E, 0x18);
                else Over(o, c / 255.0f, 0xFF, 0xFF, 0xFF);
            }
}

// "Press START" with START as its key (12, START's binding): the game's "Press" (texels 168 to 220, rows 11 to 33 of
// 512x64; START was 226 to 303) and a disc the text's height beside it, the two centred where the words were. Drawn
// at 4x so the disc keeps its edge on a large screen; the game's pixels are only scaled (bilinear). The game draws
// the picture's texels 1.46 times as tall as wide (492 x 44 of them on a 420 x 55 quad of its 640 x 480), so the disc
// is drawn that much wider than tall to be round on screen
static bool PressStartPicture(const TextureDesc& d, const uint32_t* palette, const PromptButton& pb,
                              std::vector<uint8_t>& px, uint32_t* w, uint32_t* h) {
    const uint32_t vk = app_PadInputKey(12);
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
    if (!vk || !palette || !p) return false;
    std::vector<uint8_t> src;
    uint32_t pitch = 0;
    DecodeLevel(d, 0, 0, p, palette, src, pitch);
    if (pitch < 512 * 4 || src.size() < size_t(pitch) * 64) return false;
    constexpr int K = 4, W = 512 * K, H = 64 * K;
    constexpr float aspect = (54.946f / 44) / (420.14f / 492);  // a texel's height over its width on screen
    constexpr float press_l = 168, press_r = 221, gap = 7, dia_v = 27, dia = dia_v * aspect, mid = (168 + 304) / 2.0f,
                    cy = 22;
    const float left = mid - (press_r - press_l + gap + dia) / 2, shift = left - press_l;
    *w = W, *h = H;
    px.assign(size_t(W) * H * 4, 0);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            // the source texel under this pixel's centre, "Press" moved by shift; nothing right of it
            const float u = (x + 0.5f) / K - 0.5f - shift, v = (y + 0.5f) / K - 0.5f;
            if (u < press_l - 2 || u > press_r + 1) continue;
            const int u0 = int(std::floor(u)), v0 = int(std::floor(v));
            const float fu = u - u0, fv = v - v0;
            float acc[4] = {};
            for (int j = 0; j < 2; j++)
                for (int i = 0; i < 2; i++) {
                    const int su = std::clamp(u0 + i, 0, 511), sv = std::clamp(v0 + j, 0, 63);
                    if (su > press_r) continue;  // START's first column stays out
                    const float wgt = (i ? fu : 1 - fu) * (j ? fv : 1 - fv);
                    const uint8_t* s = &src[size_t(sv) * pitch + su * 4];
                    for (int c = 0; c < 3; c++) acc[c] += s[c] * s[3] * wgt;  // colour weighted by alpha
                    acc[3] += s[3] * wgt;
                }
            uint8_t* o = &px[(size_t(y) * W + x) * 4];
            if (acc[3] <= 0) continue;
            for (int c = 0; c < 3; c++) o[c] = uint8_t(std::min(255.0f, acc[c] / acc[3] + 0.5f));
            o[3] = uint8_t(std::min(255.0f, acc[3] + 0.5f));
        }
    // the key: drawn round in a square of its own (dia across), then laid over squeezed to dia_v tall
    constexpr int N = 192;
    std::vector<uint8_t> key(size_t(N) * N * 4, 0);
    PromptDisc(key, N, N / 2.0f, N / 2.0f, dia * K / 2.0f, vk, pb.r, pb.g, pb.b);
    const float kcx = (left + press_r - press_l + gap + dia / 2) * K, kcy = cy * K;
    for (int qy = std::max(0, int(kcy - N / 2 / aspect) - 1); qy < std::min(H, int(kcy + N / 2 / aspect) + 2); qy++)
        for (int qx = std::max(0, int(kcx) - N / 2); qx < std::min(W, int(kcx) + N / 2 + 1); qx++) {
            // the square's point under this pixel's centre, bilinear (colour weighted by alpha)
            const float u = qx + 0.5f - kcx + N / 2.0f - 0.5f, v = (qy + 0.5f - kcy) * aspect + N / 2.0f - 0.5f;
            const int u0 = int(std::floor(u)), v0 = int(std::floor(v));
            const float fu = u - u0, fv = v - v0;
            float acc[4] = {};
            for (int j = 0; j < 2; j++)
                for (int i = 0; i < 2; i++) {
                    const int su = u0 + i, sv = v0 + j;
                    if (su < 0 || sv < 0 || su >= N || sv >= N) continue;
                    const float wgt = (i ? fu : 1 - fu) * (j ? fv : 1 - fv);
                    const uint8_t* k = &key[(size_t(sv) * N + su) * 4];
                    for (int c = 0; c < 3; c++) acc[c] += k[c] * k[3] * wgt;
                    acc[3] += k[3] * wgt;
                }
            if (acc[3] < 0.5f) continue;
            Over(&px[(size_t(qy) * W + qx) * 4], std::min(1.0f, acc[3] / 255), uint8_t(acc[2] / acc[3] + 0.5f),
                 uint8_t(acc[1] / acc[3] + 0.5f), uint8_t(acc[0] / acc[3] + 0.5f));
        }
    static uint32_t told;
    if (told != vk) {
        told = vk;
        fprintf(stderr, "[PROMPTS] title screen's Press START drawn with %s\n", app::KeyName(vk));
    }
    return true;
}

static bool PromptPicture(int index, std::vector<uint8_t>& px, uint32_t* w, uint32_t* h,
                          const TextureDesc& d, const uint32_t* palette) {
    if (index < 0 || index >= int(sizeof kPromptButtons / sizeof kPromptButtons[0])) return false;
    const PromptButton& pb = kPromptButtons[index];
    if (pb.input == kPromptPressStart) return PressStartPicture(d, palette, pb, px, w, h);
    constexpr int N = 128;
    if (pb.input == kPromptMove || pb.input == kPromptDpad) {
        // up, left, down, right
        const int b0 = pb.input == kPromptMove ? 16 : 8;
        const int binds[4] = {b0, pb.input == kPromptMove ? 18 : 10, pb.input == kPromptMove ? 17 : 9,
                              pb.input == kPromptMove ? 19 : 11};
        uint32_t keys[4];
        for (int i = 0; i < 4; i++)
            if (!(keys[i] = app_KeyBinding(binds[i]))) return false;  // one unbound: the game's own picture
        *w = *h = N;
        px.assign(size_t(N) * N * 4, 0);
        // four small discs in a cross, a little apart
        const float at[4][2] = {{64, 44}, {28, 84}, {64, 84}, {100, 84}};
        for (int i = 0; i < 4; i++) PromptDisc(px, N, at[i][0], at[i][1], 17, keys[i], pb.r, pb.g, pb.b);
        return true;
    }
    const uint32_t vk = app_PadInputKey(pb.input);
    if (!vk) return false;  // unbound: the game's own button
    *w = *h = N;
    px.assign(size_t(N) * N * 4, 0);
    PromptDisc(px, N, 64, 64, 62, vk, pb.r, pb.g, pb.b);
    static uint32_t told[sizeof kPromptButtons / sizeof kPromptButtons[0]];
    if (told[index] != vk) {
        told[index] = vk;
        fprintf(stderr, "[PROMPTS] HUD button %s drawn as %s\n", app::kPadInputNames[pb.input], app::KeyName(vk));
    }
    return true;
}

// false when it could not be used; the caller's texture may then have no resource
bool D3D12Backend::BindHdTexture(Tex& tx, const TextureDesc& d, uint64_t hd, const uint32_t* palette) {
    uint32_t head[3] = {};
    std::vector<uint8_t> px;
    if (hd >> 63) {  // a button's keyboard picture (PromptLookup)
        if (!PromptPicture(int(hd & 0xFF), px, &head[1], &head[2], d, palette)) return false;
    } else {
        HdIndex& x = Hd();
        auto it = x.files.find(hd);
        if (it == x.files.end()) return false;
        FILE* f = _wfopen(it->second.c_str(), L"rb");
        bool ok = f && std::fread(head, 4, 3, f) == 3 && head[0] == 0x31544448u && head[1] && head[2] &&
                  head[1] <= 16384 && head[2] <= 16384;
        if (ok) {
            px.resize(size_t(head[1]) * head[2] * 4);
            ok = std::fread(px.data(), 1, px.size(), f) == px.size();
        }
        if (f) std::fclose(f);
        if (!ok) {
            fprintf(stderr, "[HDTEX] bad file %s, left out\n", it->second.string().c_str());
            x.files.erase(it);
            return false;
        }
    }
    uint32_t w = head[1], h = head[2], levels = 1;
    for (uint32_t m = std::max(w, h); m > 1; m >>= 1) levels++;
    if (tx.res) CurrentFrame().garbage.push_back(tx.res);
    tx = Tex{};
    tx.d = d;
    tx.fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
    tx.levels = levels;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w;
    td.Height = h;
    td.DepthOrArraySize = 1;
    td.MipLevels = UINT16(levels);
    td.Format = tx.fmt;
    td.SampleDesc.Count = 1;
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&tx.res)))) {
        return false;
    }
    for (uint32_t l = 0;; l++) {
        UploadTextureLevel(tx, 0, l, px, w * 4);
        if (l + 1 == levels) break;
        // the next level, a 2x2 box
        uint32_t nw = std::max(w / 2, 1u), nh = std::max(h / 2, 1u);
        std::vector<uint8_t> next(size_t(nw) * nh * 4);
        for (uint32_t y = 0; y < nh; y++) {
            const uint8_t* r0 = &px[size_t(std::min(y * 2, h - 1)) * w * 4];
            const uint8_t* r1 = &px[size_t(std::min(y * 2 + 1, h - 1)) * w * 4];
            uint8_t* o = &next[size_t(y) * nw * 4];
            for (uint32_t c = 0; c < nw * 4; c++) {
                uint32_t a0 = c / 4 * 2, a1 = std::min(a0 + 1, w - 1), ch = c % 4;
                o[c] = uint8_t((r0[a0 * 4 + ch] + r0[a1 * 4 + ch] + r1[a0 * 4 + ch] + r1[a1 * 4 + ch] + 2) >> 2);
            }
        }
        px.swap(next);
        w = nw, h = nh;
    }
    D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COPY_DEST;
    Transition(tx.res.Get(), st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    tx.hd = hd;
    return true;
}

void FillTextureView(const Tex& tx, const TextureDesc& d, D3D12_SHADER_RESOURCE_VIEW_DESC& sv) {
    sv.Format = tx.fmt;
    if (d.cube) {
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        sv.TextureCube.MipLevels = tx.levels;
    } else if (d.depth > 1) {
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        sv.Texture3D.MipLevels = tx.levels;
    } else {
        sv.Texture2D.MipLevels = tx.levels;
    }
}

// a texture in guest memory: made again when its layout changed, uploaded again when its bytes did
ID3D12Resource* D3D12Backend::BindGuestTexture(int stage, const TextureDesc& d, const FormatInfo& fi,
                                               DrawTextures& stages, uint32_t palette) {
    TexInfo& info = stages.info[stage];
    // xboxrecomp: a P8 texture's colours are its palette's 256 D3DCOLORs (B, G, R, A in memory, as BGRA8)
    const uint32_t* pal = palette ? static_cast<const uint32_t*>(kernel::HostPtr(palette)) : nullptr;
    tex_key_ = d.data | (pal ? uint64_t(palette) << 32 : 0);
    Tex& tx = textures_[tex_key_];
    uint32_t total = TotalSize(d);
    bool same = SameTexture(tx, d);
    if (!same && !CreateTexture(tx, d, fi)) return nullptr;
    // cheap change check once per frame: lock epochs plus a sample of the bytes
    if (tx.frame != presents_ + 1 || !same) {
        tx.frame = presents_ + 1;
        const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
        uint64_t sig = MemSig(d.data, total) ^ DirtyEpoch(d.data);
        if (pal) sig ^= Hash(pal, 256 * 4) * 31;
        if (hppoa_HDTextures()) sig ^= 0x48445445585431ull;  // switching the option looks again
        // a button's picture drawn as its key: a new binding (F10) draws it again at once, not at the HUD's next upload
        // and a change of device (Automatic: the keys while keyboard and mouse are in use, the game's own with a pad)
        if ((d.format == 0x0F && d.width == 32 && d.height == 32) || (d.format == 0x0B && d.width == 512 && d.height == 64)) {
            if (app_PromptsKbm())
                for (int b = 0; b < 24; b++) sig ^= uint64_t(app_KeyBinding(b)) * (0x9E3779B97F4A7C15ull >> (b & 31)) ^ uint64_t(b) << 40;
            else
                sig ^= 0x50414442ull;
        }
        if (sig != tx.sig || !same) {
            uint64_t hd = PromptLookup(d, p);  // a P8 one only for "Press START"
            if (!hd && !pal) hd = HdLookup(d, p);
            if (hd && (hd != tx.hd || !same) && !BindHdTexture(tx, d, hd, pal)) {
                hd = 0;
                if (!tx.res) same = false;
                if (!tx.res && !CreateTexture(tx, d, fi)) return nullptr;
            }
            if (!hd) {
                if (tx.hd) {  // back to the guest's own pixels, in the guest's own layout
                    same = false;
                    if (!CreateTexture(tx, d, fi)) return nullptr;
                }
                UploadTexture(tx, d, fi, p, same, pal);
            }
            tx.frame = presents_ + 1;
            tx.sig = sig;
        }
    }
    FillTextureView(tx, d, stages.sv[stage]);
    if (fi.depth) info.shadow = ShadowDepthKind(d.format);
    if (tx.norm && d.cube && hdr_) info.flags |= kTexNormCube;
    return tx.res.Get();
}

constexpr uint32_t kTaddressBorderTex = 4;  // D3DTADDRESS_BORDER

ID3D12Resource* D3D12Backend::BindTexture(const State& s, int stage, DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    D3D12_SHADER_RESOURCE_VIEW_DESC& sv = stages.sv[stage];
    info = {};
    sv = {};
    stages.rt[stage] = false;
    stages.wide[stage] = 0;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sv.Texture2D.MipLevels = 1;
    TextureDesc d;
    if (!s.textures[size_t(stage)] || !Describe(s.textures[size_t(stage)], d)) return nullptr;
    const FormatInfo& fi = Format(d.format);
    info.kind = TextureKind(d);
    info.rect = d.linear;
    // xboxrecomp: border addressing in the shader (kTexBorderU), the sampler clamps
    if (info.kind == kTex2D && !fi.depth) {
        const auto& ts = s.tss[size_t(stage)];
        if (ts[kTssAddressU] == kTaddressBorderTex) info.flags |= kTexBorderU;
        if (ts[kTssAddressV] == kTaddressBorderTex) info.flags |= kTexBorderV;
    }

    // rendered by the GPU: sample the target itself; DXT or P8 data loaded over a target's memory is not it (no
    // target is palettized: the pause menu's title shadow, P8 where a 512x128 target was, read it as a black box)
    auto tit = targets_.find(TargetKey(d.data, fi.depth));
    if (tit != targets_.end() && !d.cube && d.depth == 1 && !fi.compressed && d.format != 0x0B) {
        return BindTargetTexture(s, stage, d, tit->second, stages);
    }
    // xboxrecomp (hppoa): the game's own 2D texture with a linear magnify filter (2: D3DTEXF_LINEAR, or anisotropic):
    // drawn bicubic where it is stretched (kTexSmooth, smoothtex). Point filtered ones stay as the game asks
    if (info.kind == kTex2D && !d.linear && !fi.depth && s.tss[size_t(stage)][kTssMagFilter] >= 2u &&
        app::Get().smooth_textures)
        info.flags |= kTexSmooth;
    uint32_t palette = 0;
    if (d.format == 0x0B && s.palettes[size_t(stage)])  // P8: D3DPalette { Common, Data (physical), Lock }
        palette = mem::VirtualFromPhysical(kernel::R32(s.palettes[size_t(stage)] + 4));
    return BindGuestTexture(stage, d, fi, stages, palette);
}

// D3DTEXTUREFILTERTYPE and D3DTEXTUREADDRESS values the samplers test
constexpr uint32_t kTexfLinear = 2;
constexpr uint32_t kTexfAnisotropic = 3;
constexpr uint32_t kTaddressWrap = 1;
constexpr uint32_t kTaddressClamp = 3;

D3D12_SAMPLER_DESC StageSampler(const State& s, int i, const DrawTextures& stages, bool clamp, uint32_t aniso) {
    D3D12_SAMPLER_DESC sd = {};
    const auto& t = s.tss[size_t(i)];
    uint32_t mag = t[kTssMagFilter];
    uint32_t min = t[kTssMinFilter];
    uint32_t mip = t[kTssMipFilter];
    auto lin = [](uint32_t f) { return f >= kTexfLinear; };
    D3D12_FILTER_TYPE fmin = lin(min) ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE fmag = lin(mag) ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE fmip = mip == kTexfLinear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    sd.Filter = D3D12_FILTER(fmin << 4 | fmag << 2 | fmip);
    bool anisotropic = min == kTexfAnisotropic || mag == kTexfAnisotropic;
    uint32_t an = anisotropic ? std::max<uint32_t>(t[kTssMaxAnisotropy], 1) : 1;
    // forced only where the title filters across mip levels and the texture has them
    if (aniso && lin(min) && mip && stages.sv[i].Texture2D.MipLevels > 1 && !stages.info[i].shadow) an = aniso;
    if (an > 1) {
        sd.Filter = D3D12_FILTER_ANISOTROPIC;
        sd.MaxAnisotropy = std::min<uint32_t>(an, 16);
    }
    // clamp: the draw's coordinates all lie in 0..1 (UnitTexcoords), so wrap is read as clamp
    sd.AddressU = Address(clamp && t[kTssAddressU] == kTaddressWrap ? kTaddressClamp : t[kTssAddressU]);
    sd.AddressV = Address(clamp && t[kTssAddressV] == kTaddressWrap ? kTaddressClamp : t[kTssAddressV]);
    // xboxrecomp: the shader does the border (kTexBorderU/V)
    if (stages.info[i].flags & kTexBorderU) sd.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    if (stages.info[i].flags & kTexBorderV) sd.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sd.AddressW = Address(t[kTssAddressW]);
    sd.MipLODBias = Bits(t[kTssMipLodBias]);
    sd.MinLOD = float(t[kTssMaxMipLevel]);
    sd.MaxLOD = mip ? D3D12_FLOAT32_MAX : sd.MinLOD;
    Color(t[kTssBorderColor], sd.BorderColor);
    if (stages.info[i].shadow) {
        // the console tests texel OP r, D3D12 tests r OP texel
        static const uint8_t swap[8] = {0, 4, 2, 6, 1, 5, 3, 7};
        sd.Filter = lin(min) || lin(mag) ? D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT
                                         : D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
        sd.ComparisonFunc = D3D12_COMPARISON_FUNC(swap[Cmp(s.rs[kRsShadowFunc]) - 1] + 1);
        sd.MaxAnisotropy = 0;
    }
    return sd;
}

uint32_t D3D12Backend::EnsureStageSamplers(const State& s, const DrawTextures& stages, bool clamp) {
    D3D12_SAMPLER_DESC sd[4];
    uint32_t aniso = app::Get().aniso;
    for (int i = 0; i < 4; i++) sd[i] = StageSampler(s, i, stages, clamp, aniso);
    uint64_t key = Hash(sd, sizeof(sd));
    auto it = samplers_.find(key);
    if (it != samplers_.end()) return it->second;
    if (smp_used_ + 4 > kSamplerCount) {
        samplers_.clear();
        smp_used_ = 0;
    }
    uint32_t base = smp_used_;
    smp_used_ += 4;
    for (int i = 0; i < 4; i++) dev_->CreateSampler(&sd[i], Cpu(smp_heap_.Get(), base + i, smp_inc_));
    samplers_[key] = base;
    return base;
}

uint32_t SimpleState(const State& s, uint32_t method) {
    return s.simple[method >> 2];
}

// HDR starts at a frame's first 3D draw into the HDR scene target; screen space quads never start it: a loading
// screen's fade redraws over the last tone mapped picture every frame
bool D3D12Backend::EnterHdrForScene(const State& s, const Assembled& a) {
    bool scene_target = bound_rt_ && bound_ds_ && bound_rt_->fmt == kHdrFormat;
    bool not_started = scene_target && !bound_rt_->hdr && bound_rt_->hdr_frame != presents_;
    bool depth_written_3d = !a.pretransformed && s.rs[kRsZEnable] && SimpleState(s, kMZWriteEnable);
    if (not_started && depth_written_3d) {
        EnterHdr(*bound_rt_);
        if (!BindTargets(s)) return false;
    }
    return true;
}

// D3DTRANSFORMSTATETYPE indices into s.transforms
constexpr size_t kTsView = 2;
constexpr size_t kTsProjection = 3;
constexpr size_t kTsTexture0 = 16;
constexpr size_t kTsWorld = 256;

// xboxrecomp (hppoa): a frame's 3D scene goes into the guest sized target with depth written; the first 2D draw after
// it there (pretransformed, or no depth test, sampling no render target) is the HUD's: the scene is bloomed first
bool D3D12Backend::BloomForScene(const State& s, const Assembled& a) {
    Target* t = bound_rt_;
    if (!t || t->gw != guest_w_ || t->gh != guest_h_ || t->hdr) return true;
    bool flat = a.pretransformed || !s.rs[kRsZEnable];
    if (!flat) {
        if (bound_ds_ && SimpleState(s, kMZWriteEnable)) {
            t->scene_frame = presents_;
            // the camera, from a fixed function draw (a vertex program keeps its projection in its constants)
            bool prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
            if (!prog) {
                std::memcpy(scene_proj_, s.transforms[kTsProjection].data(), sizeof(scene_proj_));
                scene_vp_ = s.viewport;
                scene_rt_ = s.rt;
                scene_zs_ = s.zs;
                scene_fog_[0] = s.rs[kRsFogEnable];
                scene_fog_[1] = s.rs[kRsFogTableMode];
                scene_fog_[2] = s.rs[kRsFogStart];
                scene_fog_[3] = s.rs[kRsFogEnd];
                scene_fog_[4] = s.rs[kRsFogDensity];
                scene_cam_frame_ = presents_;
            }
        }
        return true;
    }
    if (t->scene_frame != presents_ || t->bloom_frame == presents_) return true;
    // a view drawn into a texture laid over the scene is not the HUD: the train's window shows the landscape drawn
    // first, then the compartment after it (the scene ends at the depth clear before the HUD, Clear)
    for (uint32_t tex : s.textures) {
        TextureDesc d;
        if (tex && Describe(tex, d) && targets_.count(TargetKey(d.data, Format(d.format).depth))) return true;
    }
    SceneEnd(*t);
    return bound_rt_ == t || BindTargets(s);
}

extern "C" int hppoa_Cutscene(void);  // hle_clock.c: the game clock in a cutscene's fixed step

// xboxrecomp (hppoa): the end of a frame's 3D scene. Its passes (seam fill, ambient occlusion, cutscene depth of
// field) are gone (by request, 2026-11-09); what is left marks the target's scene done for the frame
void D3D12Backend::SceneEnd(Target& t) {
    t.bloom_frame = presents_;  // the scene's passes done for this frame
}

constexpr uint32_t kQuadVertices = 6;  // a quad drawn as two triangles

DrawShaders ClassifyShaders(const State& s, const Assembled& a, uint32_t count) {
    DrawShaders sh;
    // a shader made from a declaration alone (no program) is the fixed function pipeline on those inputs
    sh.prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    // up to a quad with no depth test: it may be a pass over the whole target, see QuadHlsl
    sh.quad = count <= kQuadVertices && !s.rs[kRsZEnable];
    sh.fixed_transform = !sh.prog && !a.pretransformed;
    if (!sh.prog) sh.fk = MakeFixedKey(s);
    return sh;
}

// the program's hash is cached in the guest shader object
void EnsureProgramHash(const State& s) {
    if (!s.vs->hash) s.vs->hash = Hash(s.vs->program.data(), s.vs->program.size() * 4) | 1;
}

// vertex shader cache key salts
constexpr uint64_t kDeclarationSalt = 0x10000;    // fixed function seed, above the 16 bit register mask
constexpr uint64_t kPretransformedSalt = 0x5A5A;  // screen space fixed function vertices, apart from untransformed
constexpr uint64_t kQuadSalt = 0xC3C30000;        // the QuadHlsl variant, apart from the plain shader

uint64_t VertexKey(const State& s, const Assembled& a, const DrawShaders& sh) {
    uint64_t seed = sh.prog ? s.vs->hash ^ a.regs : Hash(&s.vs_handle, 4, a.regs | kDeclarationSalt);
    uint64_t vkey = Hash(&a.decode, 8, seed);
    if (!sh.prog && a.pretransformed) vkey ^= kPretransformedSalt;
    if (sh.quad) vkey ^= kQuadSalt;
    if (!sh.prog) vkey = Hash(&sh.fk, sizeof(sh.fk), vkey);
    return vkey;
}

std::string VertexHlsl(const State& s, const Assembled& a, const DrawShaders& sh) {
    if (sh.prog) return VertexProgramHlsl(s.vs->program, a.regs, a.decode);
    return FixedVertexHlsl(a.regs, a.pretransformed, a.decode, sh.fk);
}

uint64_t D3D12Backend::EnsureVertexShader(const State& s, const Assembled& a, const DrawShaders& sh) {
    auto vit = shader_src_.find(sh.vkey);
    uint64_t vh;
    if (vit != shader_src_.end()) {
        vh = vit->second;
    } else {
        std::string src = VertexHlsl(s, a, sh);
        vh = Register(sh.quad ? QuadHlsl(std::move(src)) : src, "main", "vs_5_0");
        shader_src_[sh.vkey] = vh;
    }
    // the vertex shader's text is gone once compiled: the line shader is made from a fresh copy
    if (a.topology == Topology::Lines && LineGsOn() && !line_gs_.count(vh)) {
        std::string src = VertexHlsl(s, a, sh);
        line_gs_[vh] = Register(LineGsHlsl(sh.quad ? QuadHlsl(std::move(src)) : src), "gsline", "gs_5_0");
    }
    return vh;
}

bool D3D12Backend::BindStageTexturesAndTargets(const State& s, DrawTextures& stages) {
    draw_rt_ = bound_rt_;
    draw_ds_ = bound_ds_;
    for (int i = 0; i < 4; i++) stages.res[i] = BindTexture(s, i, stages);
    // textures may have recorded copies that unbound the targets
    return BindTargets(s);
}

constexpr uint64_t kPixelKeySalt = 0x9E3779B97F4A7C15ull;

uint64_t D3D12Backend::EnsurePixelShader(const PixelKey& pk, uint64_t pkey) {
    // pixel shaders share the map with vertex shaders, the salt keeps their keys apart
    auto pit = shader_src_.find(pkey ^ kPixelKeySalt);
    if (pit != shader_src_.end()) return pit->second;
    uint64_t ph = Register(PixelHlsl(pk), "main", "ps_5_0");
    shader_src_[pkey ^ kPixelKeySalt] = ph;
    return ph;
}

void KeyBlend(const State& s, PsoKey& k) {
    k.blend = SimpleState(s, kMBlendEnable) != 0;
    if (k.blend) {
        k.src = SimpleState(s, kMSrcBlend);
        k.dst = SimpleState(s, kMDestBlend);
        k.op = SimpleState(s, kMBlendOp);
    }
    uint32_t cw = SimpleState(s, kMColorWriteEnable);
    // the console's enables are one per byte, A R G B; D3D12 wants bits R G B A
    k.wmask = ((cw >> 16) & 1) | ((cw >> 8) & 1) << 1 | (cw & 1) << 2 | ((cw >> 24) & 1) << 3;
}

constexpr float kDepth24Max = 16777215.0f;  // 2^24 minus 1: a 24 bit depth buffer's units per 1.0

void D3D12Backend::KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k) {
    k.zen = s.rs[kRsZEnable] != 0;
    if (k.zen) {
        k.zwrite = SimpleState(s, kMZWriteEnable) != 0;
        k.zfunc = g_dbg_zalways ? 0x207u : SimpleState(s, kMZFunc);  // GL_ALWAYS
    }
    k.sten = s.rs[kRsStencilEnable] != 0;
    if (k.sten) {
        k.sfunc = SimpleState(s, kMStencilFunc);
        k.smask = SimpleState(s, kMStencilMask) & 0xFF;
        k.swmask = SimpleState(s, kMStencilWriteMask) & 0xFF;
        k.sfail = s.rs[kRsStencilFail];
        k.szfail = SimpleState(s, kMStencilZFail);
        k.spass = SimpleState(s, kMStencilPass);
    }
    // polygon offset: bias in depth buffer units, the target is always 24 bit; the slope is per console pixel,
    // and an upscaled target's pixels are smaller
    if (a.topology == Topology::Triangles && SimpleState(s, kMSolidOffsetEnable) && !UseLogDepth(s, a)) {
        k.zbias = int32_t(std::lround(Bits(SimpleState(s, kMDepthOffset)) * kDepth24Max / s.zscale));
        k.zslope = Bits(SimpleState(s, kMSlopeScale)) * (bound_ds_ ? float(bound_ds_->w) / bound_ds_->gw : 1.0f);
    }
}

void KeyRasterizer(const State& s, const Assembled& a, PsoKey& k) {
    uint32_t cull = s.rs[kRsCullMode];
    k.cull = cull != 0 && a.topology == Topology::Triangles;
    k.ccw = cull == kCullCw;  // culling clockwise leaves counter clockwise as the front
    k.fill = s.rs[kRsFillMode];
}

void D3D12Backend::BuildPipelineKey(const State& s, const Assembled& a, uint64_t vh, uint64_t ph, PsoKey& k) {
    std::memset(&k, 0, sizeof(k));
    k.vs = vh;
    k.ps = ph;
    k.regs = a.regs;
    for (int r = 0; r < 16; r++) {
        if ((a.regs >> r) & 1) {
            const VertexElement& e = a.elements[size_t(r)];
            k.elements[r] = e.format | uint32_t(e.buffer) << 8 | uint32_t(e.offset) << 16;
        }
    }
    k.topo = uint32_t(a.topology);
    k.has_rt = bound_rt_ != nullptr;
    k.has_ds = bound_ds_ != nullptr;
    k.rtfmt = bound_rt_ ? bound_rt_->fmt : 0;
    uint32_t samples = bound_rt_ ? bound_rt_->samples : bound_ds_->samples;
    k.msaa = samples > 1 ? samples : 0;
    KeyBlend(s, k);
    if (k.has_ds) KeyDepthStencil(s, a, k);
    KeyRasterizer(s, a, k);
}

// alpha test off: compare always, in Cmp() - 1 terms like the enabled test
constexpr float kAlphaTestAlways = float(D3D12_COMPARISON_FUNC_ALWAYS - 1);

void FillSurfaceConstants(const State& s, GuestSize size, DrawConstants& c) {
    c.surf[0] = float(size.w);
    c.surf[1] = float(size.h);
    c.surf[2] = 1.0f / s.zscale;
    if (s.rs[kRsFogEnable]) {
        c.fog[0] = Bits(s.rs[kRsFogStart]);
        c.fog[1] = Bits(s.rs[kRsFogEnd]);
        c.fog[2] = Bits(s.rs[kRsFogDensity]);
        c.fog[3] = float(s.rs[kRsFogTableMode]);
    }
    Color(s.rs[kRsFogColor], c.fog_color);
    c.misc[0] = float(SimpleState(s, kMAlphaRef) & 0xFF);
    c.misc[1] = SimpleState(s, kMAlphaTestEnable) && !g_dbg_noatest ? float(Cmp(SimpleState(s, kMAlphaFunc)) - 1) : kAlphaTestAlways;
    c.misc[2] = float(Cmp(s.rs[kRsShadowFunc]) - 1);
    c.misc[3] = s.rs[kRsSpecularEnable] ? 1.0f : 0.0f;
}

void FillCombinerConstants(const State& s, DrawConstants& c) {
    for (int i = 0; i < 8; i++) {
        Color(SimpleState(s, kMPsConstant0 + i * 4), c.pc[i * 2]);
        Color(SimpleState(s, kMPsConstant1 + i * 4), c.pc[i * 2 + 1]);
    }
    Color(SimpleState(s, kMPsFinalConstant0), c.pc[16]);
    Color(SimpleState(s, kMPsFinalConstant1), c.pc[17]);
    Color(s.rs[kRsTextureFactor], c.tfactor);
}

void D3D12Backend::FillHdrConstants(const PsoKey& k, DrawConstants& c) {
    // colour lands over the headroom unless the blend multiplies it with what the target holds
    uint32_t src_factor = k.blend ? k.src : 1;
    c.hdr[0] = bound_rt_ && bound_rt_->hdr && src_factor != 0 && src_factor != kBlendDestColor ? 1 / kHeadroom : 1;
    c.hdr[1] = kHeadroom;
}

constexpr float kGlowBoxTexels = 3.0f;  // console texels across the box each wide tap averages

void D3D12Backend::FillTexelConstants(int i, const TextureDesc& d, const DrawTextures& stages, DrawConstants& c) {
    c.texsize[i][0] = float(d.width);
    c.texsize[i][1] = float(d.height);
    // the console samples pixels at integer positions, D3D12 half a pixel on: targets are read half a texel back
    if (!stages.rt[i]) return;
    c.texsize[i][2] = -0.5f / float(d.width);
    c.texsize[i][3] = -0.5f / float(d.height);
    // a 3 console texel box per tap keeps the console chain's glow width and brightness
    if (stages.wide[i] > 0) {
        float n = std::ceil(kGlowBoxTexels * stages.wide[i] * 0.5f);
        c.spread[i][0] = kGlowBoxTexels / (n * float(d.width));
        c.spread[i][1] = kGlowBoxTexels / (n * float(d.height));
        c.spread[i][2] = n;
    }
}

void D3D12Backend::FillTextureConstants(const State& s, const DrawTextures& stages, DrawConstants& c) {
    for (int i = 0; i < 4; i++) {
        TextureDesc d;
        if (s.textures[size_t(i)] && Describe(s.textures[size_t(i)], d)) FillTexelConstants(i, d, stages, c);
        const auto& t = s.tss[size_t(i)];
        c.bump[i][0] = Bits(t[kTssBumpEnvMat00]);
        c.bump[i][1] = Bits(t[kTssBumpEnvMat01]);
        c.bump[i][2] = Bits(t[kTssBumpEnvMat10]);
        c.bump[i][3] = Bits(t[kTssBumpEnvMat11]);
        c.bumpl[i][0] = Bits(t[kTssBumpEnvLScale]);
        c.bumpl[i][1] = Bits(t[kTssBumpEnvLOffset]);
        Color(t[kTssBorderColor], c.border[i]);
        c.bumpl[i][2] = 0;  // shaders.cpp: set, the stage takes its texture as grey (unused: skin as the console's)
    }
}

void MatMul(const float* x, const float* y, float* o) {
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 4; col++) {
            o[r * 4 + col] = x[r * 4] * y[col] + x[r * 4 + 1] * y[4 + col] + x[r * 4 + 2] * y[8 + col] +
                             x[r * 4 + 3] * y[12 + col];
        }
    }
}

// HLSL mul(row vector, matrix) with column major packing wants the transpose
void Transpose(const float* m, float* o) {
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 4; col++) o[col * 4 + r] = m[r * 4 + col];
    }
}

void InverseTranspose3x3(const float* wv, float* n) {
    float det = wv[0] * (wv[5] * wv[10] - wv[6] * wv[9]) - wv[1] * (wv[4] * wv[10] - wv[6] * wv[8]) +
                wv[2] * (wv[4] * wv[9] - wv[5] * wv[8]);
    float id = det != 0 ? 1.0f / det : 0.0f;
    for (int r = 0; r < 3; r++) {
        for (int col = 0; col < 3; col++) {
            int r1 = (r + 1) % 3;
            int r2 = (r + 2) % 3;
            int c1 = (col + 1) % 3;
            int c2 = (col + 2) % 3;
            n[r * 4 + col] = (wv[r1 * 4 + c1] * wv[r2 * 4 + c2] - wv[r1 * 4 + c2] * wv[r2 * 4 + c1]) * id;
        }
    }
}

// xboxrecomp (hppoa): true 16:9 widens the camera (hle_video.c: x scale * 3/4), and with it the near plane: close to
// a wall its sides reached through where the 4:3 view's did not, showing outside the level. The widened camera's
// near distance (10 for the world camera) is cut to RECOMP_WIDE_NEAR times itself (default 0.75, 1 = the title's),
// so its near plane is no wider than the console's. The far plane stays the title's.
float WideNearScale() {
    static float k = -1;
    if (k < 0) {
        const char* e = std::getenv("RECOMP_WIDE_NEAR");
        k = e && *e ? float(std::atof(e)) : 0.75f;
        if (!(k > 0.0f && k <= 1.0f)) k = 1.0f;
    }
    return k;
}

// the projection to draw with: the title's, its near plane brought in for a camera widened to 16:9
void DrawProjection(const float* p, float* out) {
    std::copy(p, p + 16, out);
    float k = WideNearScale();
    // a D3D perspective projection (w = view z): z' = a z + b, near n = -b / a, far f = a n / (a - 1)
    if (k == 1.0f || p[11] != 1.0f || p[15] != 0.0f || !(p[5] > 0.0f) || !(p[10] > 1.0f) || !(p[14] < 0.0f)) return;
    if (std::fabs(p[0] / p[5] - 0.5625f) > 0.01f) return;  // only a 4:3 pair (x = y * 3/4) widened by a further 3/4
    double a = p[10], n = -p[14] / a, f = a * n / (a - 1.0);
    double n2 = n * k, a2 = f / (f - n2);
    out[10] = float(a2);
    out[14] = float(-n2 * a2);
}

bool HudView(const State& s, uint32_t frame_w);

void FillTransformConstants(const State& s, GuestSize size, DrawConstants& c) {
    uint32_t gw = size.w;
    uint32_t gh = size.h;
    // world, view, projection, then the viewport inside the target
    float wv[16];
    float wvp[16];
    float proj[16];
    DrawProjection(s.transforms[kTsProjection].data(), proj);
    // xboxrecomp (hppoa): a view in the HUD (the map) as 4:3 drew it, squeezed with the HUD after (SqueezeCos)
    if (HudView(s, gw))
        for (int k : {0, 4, 8, 12}) proj[k] *= 4.0f / 3.0f;
    MatMul(s.transforms[kTsWorld].data(), s.transforms[kTsView].data(), wv);
    MatMul(wv, proj, wvp);
    const auto& v = s.viewport;
    float sx = float(v.w) / gw;
    float sy = float(v.h) / gh;
    float ox = (2.0f * v.x + v.w) / gw - 1;
    float oy = 1 - (2.0f * v.y + v.h) / gh;
    float vpm[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, v.max_z - v.min_z, 0, ox, oy, v.min_z, 1};
    float full[16];
    MatMul(wvp, vpm, full);
    Transpose(full, c.wvp);
    Transpose(wv, c.wv);
    // normals take the inverse transpose of the upper 3x3
    float n[16] = {};
    InverseTranspose3x3(wv, n);
    Transpose(n, c.wvn);
    for (int t = 0; t < 4; t++) Transpose(s.transforms[kTsTexture0 + size_t(t)].data(), c.tm[t]);
}

// D3DLIGHT8 as floats: Type, Diffuse[4], Specular[4], Ambient[4], Position[3], Direction[3], Range, Falloff,
// Attenuation0 to 2, Theta, Phi
constexpr int kLightFloats = 26;
constexpr int kLightDiffuse = 1;
constexpr int kLightSpecular = 5;
constexpr int kLightAmbient = 9;
constexpr int kLightPosition = 13;
constexpr int kLightDirection = 16;
constexpr int kLightRange = 19;
constexpr int kLightFalloff = 20;
constexpr int kLightAttenuation0 = 21;
constexpr int kLightAttenuation1 = 22;
constexpr int kLightAttenuation2 = 23;
constexpr int kLightTheta = 24;
constexpr int kLightPhi = 25;

void LightToView(const std::array<uint32_t, kLightFloats>& l, const float* vm, float (&o)[7][4]) {
    float f[kLightFloats];
    std::memcpy(f, l.data(), sizeof(f));
    for (int j = 0; j < 4; j++) {
        o[0][j] = f[kLightDiffuse + j];
        o[1][j] = f[kLightSpecular + j];
        o[2][j] = f[kLightAmbient + j];
    }
    const float* pos = f + kLightPosition;
    const float* dir = f + kLightDirection;
    for (int j = 0; j < 3; j++) {
        o[3][j] = pos[0] * vm[j] + pos[1] * vm[4 + j] + pos[2] * vm[8 + j] + vm[12 + j];
        o[4][j] = dir[0] * vm[j] + dir[1] * vm[4 + j] + dir[2] * vm[8 + j];
    }
    o[3][3] = f[kLightRange];
    o[4][3] = f[kLightFalloff];
    o[5][0] = f[kLightAttenuation0];
    o[5][1] = f[kLightAttenuation1];
    o[5][2] = f[kLightAttenuation2];
    o[6][0] = std::cos(f[kLightTheta] * 0.5f);
    o[6][1] = std::cos(f[kLightPhi] * 0.5f);
}

void FillLightConstants(const State& s, const FixedKey& fk, DrawConstants& c) {
    std::memcpy(c.mat, s.material.data(), sizeof(float) * 16);
    c.mat[4][0] = s.material[16];
    Color(s.rs[kRsAmbient], c.amb);
    // lights to view space
    const float* vm = s.transforms[kTsView].data();
    for (int i = 0; i < 8; i++) {
        if (!((fk.types >> (i * 2)) & 3)) continue;
        LightToView(s.lights[size_t(i)], vm, c.lt[i]);
    }
}

void D3D12Backend::UploadConstants(const State& s, const DrawShaders& sh, const PsoKey& k, const DrawTextures& stages,
                                   GuestSize size, DrawConstants& c) {
    Alloc vc = Upload(sizeof(s.vsc), 256);
    std::memcpy(vc.cpu, s.vsc.data(), sizeof(s.vsc));
    Alloc dc = Upload(sizeof(DrawConstants), 256);
    FillSurfaceConstants(s, size, c);
    if (sh.wdepth) {
        c.wbuf[0] = 1;
        if (SimpleState(s, kMSolidOffsetEnable)) {
            c.wbuf[1] = Bits(SimpleState(s, kMDepthOffset)) / s.zscale;
            // per console pixel, and an upscaled target's pixels are smaller
            c.wbuf[2] = Bits(SimpleState(s, kMSlopeScale)) * (bound_ds_ ? float(bound_ds_->w) / bound_ds_->gw : 1.0f);
        }
    }
    FillCombinerConstants(s, c);
    FillHdrConstants(k, c);
    c.hdr[0] *= out_scale_;
    FillTextureConstants(s, stages, c);
    if (sh.fixed_transform) {
        FillTransformConstants(s, size, c);
        FillLightConstants(s, sh.fk, c);
    }
    std::memcpy(dc.cpu, &c, sizeof(c));
    cl_->SetGraphicsRootConstantBufferView(0, vc.gpu);
    cl_->SetGraphicsRootConstantBufferView(1, dc.gpu);
}

void D3D12Backend::CaptureTextures(const State& s, const DrawTextures& stages) {
    for (int i = 0; i < 4; i++) {
        ID3D12Resource* res = stages.res[i];
        TextureDesc d;
        if (!s.textures[size_t(i)] || !Describe(s.textures[size_t(i)], d)) continue;
        bool tg = targets_.count(TargetKey(d.data, Format(d.format).depth));
        fprintf(cap_, " | t%d %08X fmt %X %ux%u%s%s", i, d.data, d.format, d.width, d.height, tg ? " target" : "",
                res ? "" : " unbound");
        // xboxrecomp: addressing and border colour, what a sample past the edge returns
        const auto& t = s.tss[size_t(i)];
        fprintf(cap_, " addr %u %u border %08X", t[kTssAddressU], t[kTssAddressV], t[kTssBorderColor]);
        // xboxrecomp: level 0's guest bytes hashed (FNV-1a): the same address with other contents across frames
        if (!tg) {
            const uint8_t* g = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
            uint32_t n = std::min<uint32_t>(LevelSize(d, 0), 4u << 20), h = 2166136261u;
            for (uint32_t k = 0; g && k < n; k++) h = (h ^ g[k]) * 16777619u;
            fprintf(cap_, " hash %08X", h);
        }
        // xboxrecomp: a P8 texture's palette and how many of its colours the alpha test would reject (alpha < 128)
        if (d.format == 0x0B) {
            uint32_t pal = s.palettes[size_t(i)];
            if (!pal) {
                fprintf(cap_, " pal none");
            } else {
                uint32_t data = mem::VirtualFromPhysical(kernel::R32(pal + 4));
                const uint32_t* c = static_cast<const uint32_t*>(kernel::HostPtr(data));
                int low = 0;
                for (int k = 0; k < 256; k++) low += (c[k] >> 24) < 128;
                fprintf(cap_, " pal %08X data %08X alpha<128 %d/256 c0 %08X c1 %08X", kernel::R32(pal), data, low, c[0],
                        c[1]);
            }
        }
        if (res && !tg && stages.info[i].kind == kTex2D && cap_seen_.insert(d.data).second) {
            cap_tex_.push_back({res, stages.sv[i].Format, d.data, d.width, d.height});
        }
        // xboxrecomp: a render target sampled as a texture, saved as the frame leaves it (FinishCapture; saving it
        // here, mid-draw, broke the draw's bound state): a lookup the title rendered (the shadow's falloff ramp)
        if (tg && cap_seen_.insert(d.data).second) cap_sampled_.push_back(d.data);
    }
}

// s.vsc holds the 192 vertex shader constants in hardware order: D3D register + 96
constexpr int kVsConstants = 192;
constexpr int kVsConstantBias = 96;

void D3D12Backend::CaptureInputs(const State& s, const Assembled& a, const DrawConstants& c) {
    fprintf(cap_, "\n     verts");
    uint32_t n = 0;
    for (int r = 0; r < 16; r++) n += (a.regs >> r) & 1;
    for (size_t v = 0; v < std::min<size_t>(4, n ? a.vertices.size() / (n * 4) : 0); v++) {
        fprintf(cap_, " [");
        for (uint32_t k = 0; k < n * 4; k++) fprintf(cap_, " %g", a.vertices[v * n * 4 + k]);
        fprintf(cap_, " ]");
    }
    // xboxrecomp: raw float registers (a quad's corners) and the texture rectangles QuadHlsl clamps to
    if (a.vertices.empty() && a.vertex_count <= 6) {
        for (int r = 0; r < 16; r++) {
            const VertexElement& e = a.elements[size_t(r)];
            const VertexBuffer& vb = a.buffers[e.buffer];
            if (!(a.regs >> r & 1) || !vb.src || !vb.stride || uint64_t(a.vertex_count) * vb.stride > vb.bytes) continue;
            if (e.format != DXGI_FORMAT_R32G32_FLOAT && e.format != DXGI_FORMAT_R32G32B32_FLOAT &&
                e.format != DXGI_FORMAT_R32G32B32A32_FLOAT) {
                continue;
            }
            fprintf(cap_, "\n     v%d", r);
            for (uint32_t v = 0; v < a.vertex_count; v++) {
                float f[2];
                std::memcpy(f, vb.src + size_t(v) * vb.stride + e.offset, sizeof(f));
                fprintf(cap_, " (%g %g)", f[0], f[1]);
            }
        }
    }
    if (c.qrect[2] > c.qrect[0]) fprintf(cap_, "\n     qrect %g %g %g %g", c.qrect[0], c.qrect[1], c.qrect[2], c.qrect[3]);
    // the texture transforms in use: coordinate source, flags (count, projected) and the matrix
    for (int t = 0; t < 4; t++) {
        uint32_t ttf = s.tss[size_t(t)][kTssTextureTransformFlags], tci = s.tss[size_t(t)][kTssTexCoordIndex];
        if (!(ttf & 7)) continue;
        fprintf(cap_, "\n     tex%d tci %08X ttf %X tm", t, tci, ttf);
        for (float f : s.transforms[kTsTexture0 + size_t(t)]) fprintf(cap_, " %g", f);
    }
    // xboxrecomp: 16:9, the fixed function's world * view * projection (Squeeze2D's 2D test) and the squeeze applied
    if (!a.pretransformed) {
        float wv[16], m[16];
        MatMul(s.transforms[kTsWorld].data(), s.transforms[kTsView].data(), wv);
        MatMul(wv, s.transforms[kTsProjection].data(), m);
        fprintf(cap_, "\n     wvp");
        for (float f : m) fprintf(cap_, " %g", f);
        fprintf(cap_, "\n     proj");
        for (float f : s.transforms[kTsProjection]) fprintf(cap_, " %g", f);
    }
    if (c.squeeze[0] != 0)
        fprintf(cap_, "\n     squeeze %g about %g, y %g about %g", c.squeeze[0], c.squeeze[1], c.squeeze[2], c.squeeze[3]);
    for (int t = 0; t < 4; t++) {
        if (c.uvrect[t][2] > c.uvrect[t][0]) {
            fprintf(cap_, "\n     uvrect%d %g %g %g %g", t, c.uvrect[t][0], c.uvrect[t][1], c.uvrect[t][2], c.uvrect[t][3]);
        }
    }
    for (int r = 0; r < kVsConstants; r++) {
        const auto& v = s.vsc[r];
        if (v[0] || v[1] || v[2] || v[3]) {
            fprintf(cap_, "\n     c%d %g %g %g %g", r - kVsConstantBias, v[0], v[1], v[2], v[3]);
        }
    }
    fprintf(cap_, "\n     pc");
    for (size_t r = 0; r < std::size(c.pc); r++) {
        fprintf(cap_, " (%.3f %.3f %.3f %.3f)", c.pc[r][0], c.pc[r][1], c.pc[r][2], c.pc[r][3]);
    }
}

// the draw's line: its state, then its textures
void D3D12Backend::CaptureDraw(const State& s, uint32_t prim, uint32_t count, const PsoKey& k, const DrawConstants& c,
                               const DrawTextures& stages) {
    fprintf(cap_,
            "%04u draw rt %08X ds %08X prim %u count %u vs %08X ps %08X blend %u %X %X zen %u %X zwrite %u "
            "zbias %d %.2f cull %u cw %X c0 %.3f %.3f %.3f %.3f tf %08X atest %g %g fog %g %g %g %g fc %.3f %.3f %.3f "
            "fill %X %X logic %X eaa %X",
            cap_i_++, s.rt.data, bound_ds_ ? s.zs.data : 0, prim, count, s.vs_handle, s.ps ? s.ps->handle : 0, k.blend,
            k.src, k.dst, k.zen, k.zfunc, k.zwrite, k.zbias, k.zslope, k.cull, k.wmask, c.pc[0][0], c.pc[0][1],
            c.pc[0][2], c.pc[0][3], s.rs[kRsTextureFactor], c.misc[1], c.misc[0], c.fog[0], c.fog[1], c.fog[2],
            c.fog[3], c.fog_color[0], c.fog_color[1], c.fog_color[2], s.rs[kRsFillMode], s.rs[kRsBackFillMode],
            s.rs[kRsLogicOp], s.rs[kRsEdgeAntiAlias]);
    CaptureTextures(s, stages);
    CaptureStates(s);
}

// xboxrecomp: every render state, simple state and texture stage state this draw changed since the last captured one
// (the first draw lists all that are not 0): rNNN = D3DRS index, sXXX = NV2A method, tS.NN = stage S's state NN
void D3D12Backend::CaptureStates(const State& s) {
    fprintf(cap_, "\n     st");
    for (size_t i = 0; i < s.rs.size(); i++)
        if (s.rs[i] != cap_rs_[i]) fprintf(cap_, " r%zu=%X", i, s.rs[i]);
    for (size_t i = 0; i < s.simple.size(); i++)
        if (s.simple[i] != cap_simple_[i]) fprintf(cap_, " s%zX=%X", i * 4, s.simple[i]);
    for (size_t t = 0; t < 4; t++)
        for (size_t i = 0; i < size_t(kTssCount); i++)
            if (s.tss[t][i] != cap_tss_[t][i]) fprintf(cap_, " t%zu.%zu=%X", t, i, s.tss[t][i]);
    fprintf(cap_, " vp %u %u %u %u %g %g", s.viewport.x, s.viewport.y, s.viewport.w, s.viewport.h, s.viewport.min_z,
            s.viewport.max_z);
    // xboxrecomp: a lit draw's material and enabled lights (D3DLIGHT8 floats after the type), when they changed
    if (s.rs[kRsLighting]) {
        if (s.material != cap_mat_) {
            fprintf(cap_, "\n     mat");
            for (float f : s.material) fprintf(cap_, " %g", f);
            cap_mat_ = s.material;
        }
        for (int i = 0; i < 8; i++) {
            bool on = (s.light_enable >> i) & 1;
            if (on == ((cap_light_enable_ >> i) & 1) && (!on || s.lights[size_t(i)] == cap_lights_[size_t(i)])) continue;
            fprintf(cap_, "\n     light%d %s type %u", i, on ? "on" : "off", s.lights[size_t(i)][0]);
            if (on) {
                float f[26];
                std::memcpy(f, s.lights[size_t(i)].data(), sizeof(f));
                for (int j = 1; j < 26; j++) fprintf(cap_, " %g", f[j]);
            }
            cap_lights_[size_t(i)] = s.lights[size_t(i)];
        }
        cap_light_enable_ = s.light_enable;
    }
    cap_rs_ = s.rs;
    cap_simple_ = s.simple;
    cap_tss_ = s.tss;
}

// a cap_sh_ entry apart from the shader keys: this vertex program's inputs were captured
constexpr uint64_t kInputsCapturedTag = 0xC0;

// small draws, and a vertex program's first draw: its inputs and constants; then ends the draw's line
// xboxrecomp: a fixed function draw's positions through world * view * projection on the CPU: where its depth lands
// (z / w outside 0..1 is cut away by a depth clipping rasteriser; the console's is not one) and w <= 0 (behind)
void D3D12Backend::CaptureClip(const State& s, const Assembled& a) {
    if (!(a.regs & 1)) return;
    float wv[16], m[16];
    MatMul(s.transforms[kTsWorld].data(), s.transforms[kTsView].data(), wv);
    MatMul(wv, s.transforms[kTsProjection].data(), m);
    auto pos = [&](uint32_t v, float out[3]) {
        if (!a.vertices.empty()) {
            uint32_t n = 0;
            for (int r = 0; r < 16; r++) n += (a.regs >> r) & 1;
            if ((v + 1) * n * 4 > a.vertices.size()) return false;
            std::memcpy(out, &a.vertices[v * n * 4], 12);  // register 0 first
            return true;
        }
        const VertexElement& e = a.elements[0];
        const VertexBuffer& b = a.buffers[e.buffer];
        if (!b.src || !b.stride || (v + 1) * b.stride > b.bytes || e.format != DXGI_FORMAT_R32G32B32_FLOAT) return false;
        std::memcpy(out, b.src + v * b.stride + e.offset, 12);
        return true;
    };
    uint32_t n = a.index_count ? a.index_count : a.draw_count, seen = 0, below = 0, above = 0, behind = 0;
    float zmin = 1e30f, zmax = -1e30f, wmin = 1e30f, wmax = -1e30f;
    float xmin = 1e30f, xmax = -1e30f, ymin = 1e30f, ymax = -1e30f;  // xboxrecomp: in front of the camera, guest pixels
    // stages whose coordinates are generated from the camera space position (D3DTSS_TCI_CAMERASPACEPOSITION): their
    // u, v through the stage's texture matrix
    float tc[4][4];
    bool gen[4];
    for (int t = 0; t < 4; t++) {
        gen[t] = (s.tss[size_t(t)][kTssTexCoordIndex] >> 16) == 2 && s.textures[size_t(t)];
        tc[t][0] = tc[t][2] = 1e30f, tc[t][1] = tc[t][3] = -1e30f;
    }
    for (uint32_t i = 0; i < n && i < 65536; i++) {
        uint32_t v;
        if (a.index_count && a.index_src)
            v = a.index16 ? static_cast<const uint16_t*>(a.index_src)[i] : static_cast<const uint32_t*>(a.index_src)[i];
        else v = i;
        float p[3];
        if (!pos(uint32_t(int32_t(v) + (a.vertices.empty() ? a.base_vertex : 0)), p)) continue;
        for (int t = 0; t < 4; t++) {
            if (!gen[t]) continue;
            float c[3];
            for (int k = 0; k < 3; k++) c[k] = p[0] * wv[k] + p[1] * wv[4 + k] + p[2] * wv[8 + k] + wv[12 + k];
            const float* tm = s.transforms[kTsTexture0 + size_t(t)].data();
            float u = c[0] * tm[0] + c[1] * tm[4] + c[2] * tm[8] + tm[12];
            float v = c[0] * tm[1] + c[1] * tm[5] + c[2] * tm[9] + tm[13];
            tc[t][0] = std::min(tc[t][0], u), tc[t][1] = std::max(tc[t][1], u);
            tc[t][2] = std::min(tc[t][2], v), tc[t][3] = std::max(tc[t][3], v);
        }
        float z = p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + m[14];
        float w = p[0] * m[3] + p[1] * m[7] + p[2] * m[11] + m[15];
        seen++;
        if (w <= 0) { behind++; continue; }
        float zw = z / w;
        zmin = std::min(zmin, zw), zmax = std::max(zmax, zw), wmin = std::min(wmin, w), wmax = std::max(wmax, w);
        below += zw < 0, above += zw > 1;
        float x = (p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12]) / w;
        float y = (p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13]) / w;
        x = s.viewport.x + (x + 1) * 0.5f * s.viewport.w, y = s.viewport.y + (1 - y) * 0.5f * s.viewport.h;
        xmin = std::min(xmin, x), xmax = std::max(xmax, x), ymin = std::min(ymin, y), ymax = std::max(ymax, y);
    }
    if (seen)
        fprintf(cap_, "\n     clip %u verts: z/w %g..%g, w %g..%g; z<0 %u, z>1 %u, w<=0 %u", seen, zmin, zmax, wmin, wmax,
                below, above, behind);
    if (seen > behind) fprintf(cap_, "; screen x %.0f..%.0f y %.0f..%.0f", xmin, xmax, ymin, ymax);
    for (int t = 0; t < 4; t++)
        if (gen[t] && seen) fprintf(cap_, "\n     texgen t%d u %g..%g v %g..%g", t, tc[t][0], tc[t][1], tc[t][2], tc[t][3]);
    CaptureLighting(s, a, wv);
}

// xboxrecomp: a lit fixed function draw's first vertices lit on the CPU as D3D lights them (view space, point and
// directional lights, no specular): the normal as read, then each light's N.L and attenuation, then the diffuse sum,
// to check the shader's oD0 against
void D3D12Backend::CaptureLighting(const State& s, const Assembled& a, const float* wv) {
    if (!s.rs[kRsLighting] || a.pretransformed || !(a.regs & 4) || !s.light_enable) return;
    auto reg = [&](uint32_t v, int r, float out[3]) {
        if (!a.vertices.empty()) {
            uint32_t n = 0, at = 0;
            for (int k = 0; k < 16; k++) n += (a.regs >> k) & 1, at += k < r ? (a.regs >> k) & 1 : 0;
            if ((v + 1) * n * 4 > a.vertices.size()) return false;
            std::memcpy(out, &a.vertices[(v * n + at) * 4], 12);
            return true;
        }
        const VertexElement& e = a.elements[size_t(r)];
        const VertexBuffer& b = a.buffers[e.buffer];
        if (!b.src || !b.stride || (v + 1) * b.stride > b.bytes || e.format != DXGI_FORMAT_R32G32B32_FLOAT) return false;
        std::memcpy(out, b.src + v * b.stride + e.offset, 12);
        return true;
    };
    float n3[16] = {};
    InverseTranspose3x3(wv, n3);
    const float* vm = s.transforms[kTsView].data();
    float lt[8][7][4] = {};
    for (int i = 0; i < 8; i++)
        if ((s.light_enable >> i) & 1) LightToView(s.lights[size_t(i)], vm, lt[i]);
    uint32_t n = a.index_count ? a.index_count : a.draw_count;
    for (uint32_t i = 0; i < n && i < 3; i++) {
        uint32_t v = i;
        if (a.index_count && a.index_src)
            v = a.index16 ? static_cast<const uint16_t*>(a.index_src)[i] : static_cast<const uint32_t*>(a.index_src)[i];
        v = uint32_t(int32_t(v) + (a.vertices.empty() ? a.base_vertex : 0));
        float p[3], nr[3], vp[3], vn[3];
        if (!reg(v, 0, p) || !reg(v, 2, nr)) {
            fprintf(cap_, "\n     lit v%u: normal not readable (format %d)", v, int(a.elements[2].format));
            continue;
        }
        for (int k = 0; k < 3; k++) {
            vp[k] = p[0] * wv[k] + p[1] * wv[4 + k] + p[2] * wv[8 + k] + wv[12 + k];
            vn[k] = nr[0] * n3[k] + nr[1] * n3[4 + k] + nr[2] * n3[8 + k];
        }
        float len = std::sqrt(vn[0] * vn[0] + vn[1] * vn[1] + vn[2] * vn[2]);
        if (s.rs[kRsNormalizeNormals] && len > 0)
            for (float& f : vn) f /= len;
        fprintf(cap_, "\n     lit v%u n %g %g %g |vn| %g", v, nr[0], nr[1], nr[2], len);
        float ld[3] = {};
        for (int l = 0; l < 8; l++) {
            if (!((s.light_enable >> l) & 1)) continue;
            uint32_t type = s.lights[size_t(l)][0];
            float d[3], att = 1, dist = 0;
            if (type == 3) {
                for (int k = 0; k < 3; k++) d[k] = -lt[l][4][k];
            } else {
                for (int k = 0; k < 3; k++) d[k] = lt[l][3][k] - vp[k];
                dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                float q = lt[l][5][0] + lt[l][5][1] * dist + lt[l][5][2] * dist * dist;
                att = dist > lt[l][3][3] ? 0 : 1 / std::max(q, 1e-6f);
            }
            float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            float nl = dl > 0 ? (vn[0] * d[0] + vn[1] * d[1] + vn[2] * d[2]) / dl : 0;
            for (int k = 0; k < 3; k++) ld[k] += att * lt[l][0][k] * std::max(nl, 0.0f);
            fprintf(cap_, "; L%d nl %.3f dist %.0f att %.4f", l, nl, dist, att);
        }
        fprintf(cap_, "; ld %.3f %.3f %.3f", ld[0], ld[1], ld[2]);
    }
}

void D3D12Backend::EndDrawCapture(const State& s, const Assembled& a, uint32_t count, bool prog,
                                  const DrawConstants& c) {
    uint64_t inputs_key = uint64_t(s.vs_handle) << 32 | kInputsCapturedTag;
    // xboxrecomp: RECOMP_CAP_VERTS=<texture address, hex>: every draw sampling it on stage 0 has all its inputs
    // dumped, vertex by vertex (positions, colours, coordinates: per tile seams in a ground layer);
    // RECOMP_CAP_VERTS=add: every additive (ONE, ONE) textured draw instead (the address moves with each load)
    static const uint32_t cap_verts = [] {
        char e[32] = {};
        if (!GetEnvironmentVariableA("RECOMP_CAP_VERTS", e, sizeof(e))) return 0u;
        return _stricmp(e, "add") == 0 ? 1u : uint32_t(strtoul(e, nullptr, 16));
    }();
    TextureDesc td;
    bool dump = false;
    if (cap_verts == 1)
        dump = s.textures[0] && SimpleState(s, kMBlendEnable) && SimpleState(s, kMSrcBlend) == 1 &&
               SimpleState(s, kMDestBlend) == 1;
    else if (cap_verts)
        dump = s.textures[0] && Describe(s.textures[0], td) && td.data == cap_verts;
    if (count <= kQuadVertices || dump || (prog && cap_sh_.insert(inputs_key).second)) CaptureInputs(s, a, c);
    if (!prog && !a.pretransformed) CaptureClip(s, a);
    if (dump) {
        fprintf(cap_, "\n     all draw_count %u index_count %u base %d", a.draw_count, a.index_count, a.base_vertex);
        for (int r = 0; r < 16; r++) {
            if (!((a.regs >> r) & 1)) continue;
            const VertexElement& e = a.elements[size_t(r)];
            const VertexBuffer& b = a.buffers[e.buffer];
            fprintf(cap_, "\n     v%d fmt %u stride %u bytes %u:", r, e.format, b.stride, b.bytes);
            for (uint32_t v = 0; v < 4096 && b.src && b.stride && (v + 1) * b.stride <= b.bytes; v++) {
                const uint8_t* p = b.src + v * b.stride + e.offset;
                if (e.format == DXGI_FORMAT_B8G8R8A8_UNORM || e.format == DXGI_FORMAT_R8G8B8A8_UNORM) {
                    uint32_t u;
                    std::memcpy(&u, p, 4);
                    fprintf(cap_, " %08X", u);
                } else {
                    float f[3];
                    std::memcpy(f, p, sizeof(f));
                    if (e.format == DXGI_FORMAT_R32G32_FLOAT) fprintf(cap_, " (%g %g)", f[0], f[1]);
                    else fprintf(cap_, " (%g %g %g)", f[0], f[1], f[2]);
                }
            }
        }
        // a converted draw instead: 4 floats a register
        uint32_t n = 0;
        for (int r = 0; r < 16; r++) n += (a.regs >> r) & 1;
        if (!a.vertices.empty() && n) {
            fprintf(cap_, "\n     converted %zu:", a.vertices.size() / (n * 4));
            for (size_t v = 0; v < std::min<size_t>(4096, a.vertices.size() / (n * 4)); v++) {
                fprintf(cap_, " [");
                for (uint32_t k = 0; k < n * 4; k++) fprintf(cap_, " %g", a.vertices[v * n * 4 + k]);
                fprintf(cap_, " ]");
            }
        }
    }
    // xboxrecomp: the raw inputs of a programmable draw (the converted path above is empty when the input
    // assembler reads the title's buffers as they are): per register its format, buffer and offset, and the
    // first three vertices as floats
    if (prog && a.vertices.empty()) {
        fprintf(cap_, "\n     raw draw_count %u index_count %u base %d", a.draw_count, a.index_count, a.base_vertex);
        for (int r = 0; r < 16; r++) {
            if (!((a.regs >> r) & 1)) continue;
            const VertexElement& e = a.elements[size_t(r)];
            const VertexBuffer& b = a.buffers[e.buffer];
            fprintf(cap_, "\n     v%d fmt %u buf %u off %u stride %u bytes %u decode %X:", r, e.format, e.buffer,
                    e.offset, b.stride, b.bytes, unsigned(a.decode >> (r * 4)) & 0xF);
            for (uint32_t v = 0; v < 3 && b.src && (v + 1) * b.stride <= b.bytes; v++) {
                float f[4];
                std::memcpy(f, b.src + v * b.stride + e.offset, sizeof(f));
                fprintf(cap_, " [%g %g %g %g]", f[0], f[1], f[2], f[3]);
            }
        }
    }
    fputc('\n', cap_);
}

// each shader once, as translated
void D3D12Backend::CaptureShader(const char* kind, uint64_t key, const std::string& src) {
    if (!cap_sh_.insert(key).second) return;
    char name[48];
    snprintf(name, sizeof(name), "%s_%016llX.hlsl", kind, (unsigned long long)key);
    if (FILE* f = _wfopen((cap_dir_ / name).c_str(), L"w")) {
        fwrite(src.data(), 1, src.size(), f);
        fclose(f);
    }
    fprintf(cap_, "     %s\n", name);
}

void D3D12Backend::CaptureShaders(const State& s, const Assembled& a, const DrawShaders& sh, const PixelKey& pk) {
    std::string src = VertexHlsl(s, a, sh);
    CaptureShader("vs", sh.vkey, sh.quad ? QuadHlsl(std::move(src)) : src);
    CaptureShader("ps", sh.pkey, PixelHlsl(pk));
}

D3D12_SRV_DIMENSION NullViewDimension(uint8_t kind) {
    if (kind == kTexCube) return D3D12_SRV_DIMENSION_TEXTURECUBE;
    if (kind == kTex3D) return D3D12_SRV_DIMENSION_TEXTURE3D;
    return D3D12_SRV_DIMENSION_TEXTURE2D;
}

// A small draw whose every float2 input lies in 0..1: sprites, HUD pieces, text, full screen passes. The console's
// pixel centres sample such a quad from its first texel to half a texel short of its far edge; upscaled, the far
// edge's last half texel is drawn too, and wrapped it blends in the opposite edge (the crosshair ticks' dark lines).
// Clamped, it takes the edge texel instead, which a wrapped texture used once over 0..1 has there anyway.
// xboxrecomp: also draws converted on the CPU (their elements are float4) and larger ones: a sky's panels each map their
// texture once over 0..1, and wrapped, the filter (16x anisotropic upscaled) drew the far edge in as a line along every
// panel's seam.
bool UnitTexcoords(const Assembled& a) {
    constexpr float kLo = -1e-4f;
    constexpr float kHi = 1.0001f;
    constexpr uint32_t kSmallDrawVertices = 16384;  // most vertices a draw has for this
    if (!a.vertex_count || a.vertex_count > kSmallDrawVertices) return false;
    bool any = false;
    for (int r = 0; r < 16; r++) {
        const VertexElement& e = a.elements[size_t(r)];
        if (!(a.regs >> r & 1)) continue;
        // float2, or the converted path's float4 (any element but the position in r 0 that leaves 0..1, a normal
        // say, rules the draw out)
        if (r == 0 || (e.format != DXGI_FORMAT_R32G32_FLOAT && e.format != DXGI_FORMAT_R32G32B32A32_FLOAT)) continue;
        const VertexBuffer& vb = a.buffers[e.buffer];
        if (!vb.src || !vb.stride || uint64_t(a.vertex_count) * vb.stride > vb.bytes) return false;
        for (uint32_t v = 0; v < a.vertex_count; v++) {
            float uv[2];
            std::memcpy(uv, vb.src + size_t(v) * vb.stride + e.offset, sizeof(uv));
            if (!(uv[0] >= kLo && uv[0] <= kHi && uv[1] >= kLo && uv[1] <= kHi)) return false;
        }
        any = true;
    }
    return any;
}

// xboxrecomp: the texture rectangle (u0 v0 u1 v1) of input register r over a draw's vertices, for QuadHlsl's qclamp
bool RegisterRect(const Assembled& a, int r, float out[4]) {
    if (!(a.regs >> r & 1) || !a.vertex_count) return false;
    const VertexElement& e = a.elements[size_t(r)];
    if (e.format != DXGI_FORMAT_R32G32_FLOAT && e.format != DXGI_FORMAT_R32G32B32_FLOAT &&
        e.format != DXGI_FORMAT_R32G32B32A32_FLOAT) {
        return false;
    }
    const VertexBuffer& vb = a.buffers[e.buffer];
    if (!vb.src || !vb.stride || uint64_t(a.vertex_count) * vb.stride > vb.bytes) return false;
    out[0] = out[1] = 1e30f;
    out[2] = out[3] = -1e30f;
    for (uint32_t v = 0; v < a.vertex_count; v++) {
        float uv[2];
        std::memcpy(uv, vb.src + size_t(v) * vb.stride + e.offset, sizeof(uv));
        if (!(std::isfinite(uv[0]) && std::isfinite(uv[1]))) return false;
        out[0] = std::min(out[0], uv[0]);
        out[1] = std::min(out[1], uv[1]);
        out[2] = std::max(out[2], uv[0]);
        out[3] = std::max(out[3], uv[1]);
    }
    return out[2] > out[0] && out[3] > out[1];
}

// xboxrecomp (hppoa): every vertex on the rectangle's left or right side: an upright quad, not a turned sprite
bool UprightRect(const Assembled& a, int r, const float rect[4]) {
    const VertexElement& e = a.elements[size_t(r)];
    const VertexBuffer& vb = a.buffers[e.buffer];
    for (uint32_t v = 0; v < a.vertex_count; v++) {
        float x;
        std::memcpy(&x, vb.src + size_t(v) * vb.stride + e.offset, sizeof(x));
        if (std::fabs(x - rect[0]) > 0.01f && std::fabs(x - rect[2]) > 0.01f) return false;
    }
    return true;
}

// xboxrecomp: for a vertex program, the input register each oT is a plain copy of (the title's 2D programs:
// mov oT0, v4), -1 where it is computed; looked at once per program
std::array<int, 5> CopiedTexcoords(const State& s, const Assembled& a, const DrawShaders& sh) {
    static std::unordered_map<uint64_t, std::array<int, 5>> known;
    auto it = known.find(sh.vkey);
    if (it != known.end()) return it->second;
    std::array<int, 5> r{-1, -1, -1, -1, -1};
    std::string src = VertexHlsl(s, a, sh);
    for (int k = 0; k < 5; k++) {  // oT0 to oT3, then oPos
        std::string write = k < 4 ? "        oT" + std::to_string(k) + "." : std::string("        oPos."),
                    whole = write + "xyzw = mr.xyzw;\n";
        size_t at = src.find(write);
        if (at == std::string::npos || src.find(write, at + 1) != std::string::npos) continue;  // none, or several
        if (src.compare(at, whole.size(), whole) != 0) continue;
        size_t line = src.rfind("precise float4 mr = ((i.v", at);
        if (line == std::string::npos) continue;
        int reg = -1;
        char tail[16] = {};
        if (std::sscanf(src.c_str() + line, "precise float4 mr = ((i.v%d).%15[^)]", &reg, tail) == 2 &&
            std::strcmp(tail, "xyzw") == 0 && src.find('\n', line) + 1 == at) {
            r[size_t(k)] = reg;
        }
    }
    known.emplace(sh.vkey, r);
    if (r[0] >= 0 || r[1] >= 0 || r[4] >= 0) {
        LOG_FIRST(16, "d3d12: quad program %016llX copies texcoords from v%d v%d v%d v%d, position from v%d",
                  (unsigned long long)sh.vkey, r[0], r[1], r[2], r[3], r[4]);
    }
    return r;
}

// xboxrecomp: a single quad whose coordinates reach the pixel shader unchanged (fixed function, or a program that
// copies them): each stage's texture rectangle, so the pixel shader keeps to the part the console samples
// (QuadHlsl's qclamp)
void QuadTextureRects(const State& s, const Assembled& a, const DrawShaders& sh, DrawConstants& c) {
    if (!sh.quad) return;
    if (sh.prog) {
        std::array<int, 5> reg = CopiedTexcoords(s, a, sh);
        for (int t = 0; t < 4; t++) {
            if (reg[size_t(t)] >= 0 && !RegisterRect(a, reg[size_t(t)], c.uvrect[t]))
                std::memset(c.uvrect[t], 0, sizeof(c.uvrect[t]));
        }
        // a program's oPos is in screen space on the console: copied, it is the quad's pixel rectangle
        if (reg[4] >= 0 && !RegisterRect(a, reg[4], c.qrect)) std::memset(c.qrect, 0, sizeof(c.qrect));
    } else if (a.pretransformed && !RegisterRect(a, 0, c.qrect)) {
        std::memset(c.qrect, 0, sizeof(c.qrect));
    }
    if (sh.prog) return;
    for (int t = 0; t < 4; t++) {
        uint32_t tci = s.tss[size_t(t)][kTssTexCoordIndex], ttf = s.tss[size_t(t)][kTssTextureTransformFlags];
        if (!a.pretransformed && ((tci >> 16) || ttf)) continue;  // generated or transformed coordinates
        if (!RegisterRect(a, 9 + int(tci & 3), c.uvrect[t])) std::memset(c.uvrect[t], 0, sizeof(c.uvrect[t]));
    }
}

// xboxrecomp: 16:9, the part of the frame filling the screen (x y w h; w 0 the whole frame): a cutscene's picture
// between its bars where they end up, else the title's viewport (rows vy, vh) without its overscan, the sides cut as
// much to keep the shape
void WideCrop(float fw, float fh, float vy, float vh, bool cut, float out[4]) {
    out[1] = out[2] = out[3] = 0;
    if (cut) {
        out[1] = vy + kCutTop / 480.0f * vh;
        out[3] = (kCutBottom - kCutTop) / 480.0f * vh;
        out[2] = std::min(out[3] * 4 / 3, fw);
    } else if (vh < fh) {
        out[1] = vy;
        out[3] = vh;
        out[2] = fw * vh / fh;
    }
    out[0] = (fw - out[2]) * 0.5f;
}

// a register's first vertex (x y), as RegisterRect reads them
bool RegisterFirst(const Assembled& a, int r, float out[2]) {
    if (!(a.regs >> r & 1) || !a.vertex_count) return false;
    const VertexElement& e = a.elements[size_t(r)];
    const VertexBuffer& vb = a.buffers[e.buffer];
    if (!vb.src || !vb.stride || vb.bytes < e.offset + 8u) return false;
    std::memcpy(out, vb.src + e.offset, 8);
    return std::isfinite(out[0]) && std::isfinite(out[1]);
}

// xboxrecomp: true 16:9. The title's cameras (hle_video.c's Camera_SetScale) draw the scene anamorphically into its
// 640x480 frame, which is shown 16:9 wide; 2D laid out for 4:3 keeps its shape by being drawn 3/4 as wide. A 2D draw
// is a pretransformed one, a program that copies its position (screen space on the console) or, HP:PS's way, the
// fixed function pipeline through an orthographic matrix. Pictures across the frame (movies, menu backdrops, the
// loading screens) stay 4:3 in the middle; untextured ones (fades, the cutscene bars) fill the 16:9 width. In
// gameplay (the last frame drew the scene, nothing full frame drawn yet) the HUD wholly in the left or right third
// keeps its distance from that edge of the 16:9 screen instead of the 4:3 one; the rest is squeezed about the middle.
// A cutscene (black bars above and below) is shown zoomed: the picture between its bars is 16:9 once the frame is
// shown wide, Present fills the screen with it, so its 2D (the subtitles, in the lower bar) is drawn into the picture
// at the size it has on screen otherwise, and its HUD is left out (returns true: the draw is skipped). Elsewhere Present shows the title's viewport, leaving out the rows of TV
// overscan above and below it (black lines on a PC), and as much off the sides to keep the shape.
static std::unordered_set<uint32_t> spun;  // textures seen as turned quads (the aiming circle)
extern "C" int hppoa_MenuMovie(void);  // hle_video.c: a movie is playing (the title's movie player is up)
extern "C" int hppoa_InGame(void);  // hle_quit.c: the game itself on screen, not its menus
extern "C" int hppoa_Paused(void);  // hle_quit.c: the pause menu (or a menu opened from it) is up
extern "C" int hppoa_Inventory(void);  // hle_quit.c: the inventory is up
extern "C" int hppoa_HideSaveIcon(void);  // app.cpp: F10 Enhancements > Hide the saving / loading compass
extern "C" int hppoa_MapScreen(void);  // hle_quit.c: the inventory's map page is up
extern "C" int hppoa_CutsceneScreen(void);  // hle_quit.c: "widescreen" or "pre_ig"
extern "C" int hppoa_GameScreen(void);  // hle_quit.c: the menus on "ig", paused or not (pausing's first frames)
extern "C" int hppoa_LegalScreen(void);  // hle_quit.c: the legal notices at start
extern "C" int hppoa_LoadingScreen(void);  // hle_video.c: the loading screen's thread is drawing
extern "C" int hppoa_LoadingCount(void);   // hle_video.c: loading screens so far
extern "C" int hppoa_TextIcon(uint32_t tex);
extern "C" void hppoa_TextIconDump(void);  // hle_video.c: a button icon text puts in its lines
// xboxrecomp (hppoa): true 16:9 as the CoS port does it. The cameras are widened (hle_widescreen.c) and the 640x480
// frame is shown 16:9 wide; 2D laid out for 4:3 (HUD, text, menus) is drawn 3/4 as wide about the middle so it keeps
// its shape. A 2D draw is a pretransformed one, a program copying its position (screen space on the console) or a
// fixed function draw through an orthographic matrix. One spanning the frame's width (backdrops, fades, full screen
// passes) still fills it, except a movie's picture (hle_video.c: the title's movie player is up): kept 4:3.
// The Philosopher's Stone rules below (cutscene bars cropped, the menu movie zoomed to fill) are not used.
// copy: the draw samples a render target (Swap's copy of the finished frame into the display buffer: already squeezed)
// xboxrecomp (hppoa): 16:9, a view drawn into a small viewport of the HUD, the map in the top left corner: an
// orthographic camera over its own viewport (81x81 in the 4:3 frame), drawn over the map's 2D panel. Its camera was
// widened with every other (x scale 9/16 of y's): FillTransformConstants takes that back, and SqueezeCos squeezes it
// with the HUD about the same side, so the map and Harry's arrow sit in the panel; they were a quarter of the
// panel's width to the right of it, the arrow at its edge (2026-10-26)
bool HudView(const State& s, uint32_t frame_w) {
    if (app::BootAspect() != app::AspectMode::Wide || !hppoa_InGame()) return false;
    const auto& v = s.viewport;
    if (v.w * 2 > frame_w || v.h * 8 > frame_w * 3) return false;  // under half the 4:3 frame each way
    const float* p = s.transforms[kTsProjection].data();
    bool ortho = p[3] == 0.0f && p[7] == 0.0f && p[11] == 0.0f && p[15] == 1.0f && p[5] != 0.0f;
    return ortho && std::fabs(std::fabs(p[0] / p[5]) - 0.5625f) < 0.01f;
}

// xboxrecomp (hppoa): a cutscene's letterbox, two pretransformed bars the frame's width at its top and bottom
// (Night 0's corridor with the twins: rows 0 to 57.5 and 371 to 480, no fixed game clock to tell it otherwise). The
// time each was last drawn: hppoa_Letterbox
static std::atomic<uint64_t> g_bar_top_ms{0}, g_bar_bottom_ms{0};

// xboxrecomp (hppoa): the most opaque vertex's alpha in register r's D3DCOLOR, 255 when it has none
uint32_t MaxVertexAlpha(const Assembled& a, int r) {
    if (!(a.regs >> r & 1) || !a.vertex_count) return 255;
    const VertexElement& e = a.elements[size_t(r)];
    if (e.format != DXGI_FORMAT_B8G8R8A8_UNORM && e.format != DXGI_FORMAT_R8G8B8A8_UNORM) return 255;
    const VertexBuffer& vb = a.buffers[e.buffer];
    if (!vb.src || !vb.stride || uint64_t(a.vertex_count) * vb.stride > vb.bytes) return 255;
    uint32_t m = 0;
    for (uint32_t v = 0; v < a.vertex_count; v++) m = std::max<uint32_t>(m, vb.src[size_t(v) * vb.stride + e.offset + 3]);
    return m;
}

// xboxrecomp (hppoa): the train keeps drawing its letterbox in play, the same bars as its cutscenes' and not seen:
// the whole level counted as a cutscene (the game's lens, and with the 4:3 lens cutscenes the frame cropped to their
// band). Bars nearly transparent are not a letterbox; each change of the bars' alpha is logged ([BARS])
void NoteLetterbox(const Assembled& a, float fw) {
    float r[4];
    if (!a.pretransformed || !RegisterRect(a, 0, r) || r[0] > 0.5f || r[2] < fw - 1.0f) return;
    const float fh = fw * 0.75f, h = r[3] - r[1];
    if (h < fh / 16.0f || h > fh / 3.0f) return;
    const uint32_t alpha = MaxVertexAlpha(a, 3);
    static std::atomic<uint32_t> told{~0u};
    if (told.exchange(alpha) != alpha) fprintf(stderr, "[BARS] letterbox alpha %u%s\n", alpha, alpha < 16 ? " (not shown)" : "");
    if (alpha < 16) return;
    if (r[1] <= 0.5f) g_bar_top_ms = GetTickCount64();
    else if (r[3] >= fh - 1.0f) g_bar_bottom_ms = GetTickCount64();
}

// xboxrecomp (hppoa): the menus' selected item comes toward the screen: the game draws its picture of text a second
// time, larger and fading (main menu's Load Game and Bonus Activities, the saved games, the options; F12 215914). It
// grows about the text's left end, so in 16:9 (by request, 2026-11-10) the copy drifts right of the item; it is moved
// to grow about the text's middle instead. A copy: in a menu, a picture already drawn this frame drawn again 3 to 300 %
// larger, both ways alike, over the first. The text's middle is its letters' (alpha) middle in the picture, a 512x64
// one holding a short word at its left. Only sideways: squeeze's anchor moves (x' = (x - a) s + a, a' = a - s dx /
// (1 - s)), the constants as they were, nothing new in the shaders
struct ZoomBase {
    uint64_t frame;
    float r[4];   // the first draw's rectangle on screen (the game's 640x480)
    float uv[4];  // and its texture's
};
static std::unordered_map<uint64_t, ZoomBase> zoom_bases;

// the letters' middle across the picture (0..1 of its width), from level 0's alpha; -1 when it has none to go by
static float TextMiddleU(const TextureDesc& d, uint32_t palette) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
    const uint32_t* pal = palette ? static_cast<const uint32_t*>(kernel::HostPtr(palette)) : nullptr;
    if (!p || (d.format == 0x0B && !pal) || d.cube || d.depth > 1) return -1;
    std::vector<uint8_t> px;
    uint32_t pitch = 0;
    DecodeLevel(d, 0, 0, p, pal, px, pitch);
    if (pitch < d.width * 4 || px.size() < size_t(pitch) * d.height) return -1;
    int x0 = int(d.width), x1 = -1;
    for (uint32_t y = 0; y < d.height; y++)
        for (uint32_t x = 0; x < d.width; x++)
            if (px[size_t(y) * pitch + x * 4 + 3] >= 64) x0 = std::min(x0, int(x)), x1 = std::max(x1, int(x));
    if (x1 < x0) return -1;
    return (x0 + x1 + 1) * 0.5f / float(d.width);
}

static void CentreZoomCopy(const State& s, const Assembled& a, const DrawShaders& sh, uint64_t frame, DrawConstants& c) {
    if (app::BootAspect() != app::AspectMode::Wide || (hppoa_InGame() && !hppoa_Paused())) return;
    if (!s.textures[0] || a.vertex_count != 4 && a.vertex_count != 3 && a.vertex_count != 6) return;
    int reg = -1, treg = -1;
    if (sh.prog) {
        const auto cr = CopiedTexcoords(s, a, sh);
        reg = cr[4], treg = cr[0];
    } else if (a.pretransformed) {
        reg = 0, treg = 9;
    }
    float r[4], uv[4];
    if (reg < 0 || treg < 0 || !RegisterRect(a, reg, r) || !RegisterRect(a, treg, uv)) return;
    TextureDesc d;
    if (!Describe(s.textures[0], d)) return;
    // P8: D3DPalette { Common, Data (physical), Lock }, its colours at Data
    const uint32_t palette =
        d.format == 0x0B && s.palettes[0] ? mem::VirtualFromPhysical(kernel::R32(s.palettes[0] + 4)) : 0;
    const uint64_t key = uint64_t(d.data) | uint64_t(palette) << 32;
    auto it = zoom_bases.find(key);
    if (it == zoom_bases.end() || it->second.frame != frame) {  // the first this frame: the item itself
        if (zoom_bases.size() > 4096) zoom_bases.clear();
        zoom_bases[key] = ZoomBase{frame, {r[0], r[1], r[2], r[3]}, {uv[0], uv[1], uv[2], uv[3]}};
        return;
    }
    const ZoomBase& b = it->second;
    const float bw = b.r[2] - b.r[0], bh = b.r[3] - b.r[1], kx = (r[2] - r[0]) / bw, ky = (r[3] - r[1]) / bh;
    if (bw < 8 || bh < 4 || kx < 1.03f || kx > 4.0f || std::fabs(kx / ky - 1) > 0.03f) return;
    for (int i = 0; i < 4; i++)
        if (std::fabs(uv[i] - b.uv[i]) > 1e-3f) return;  // another part of the picture
    if (r[0] > b.r[2] || r[2] < b.r[0] || r[1] > b.r[3] || r[3] < b.r[1]) return;  // not over the item
    if (!(c.squeeze[0] > 0.0f && c.squeeze[0] < 1.0f)) return;
    const float mu = TextMiddleU(d, palette);
    if (mu < 0) return;
    const float cx = b.r[0] + (mu - b.uv[0]) / (b.uv[2] - b.uv[0]) * bw;  // the text's middle on screen
    const float dx = cx + (b.r[0] - cx) * kx - r[0];                         // where its left should be
    c.squeeze[1] -= c.squeeze[0] * dx / (1 - c.squeeze[0]);
    static float told = 1e30f;
    if (std::fabs(told - dx) > 4) {
        told = dx;
        fprintf(stderr, "[ZOOM] a menu item's copy at %.1fx moved %.1f to grow about its text's middle\n", kx, dx);
    }
}

// scene: the frame has 3D drawn in it so far (BloomForScene's scene_frame)
void SqueezeCos(const State& s, const Assembled& a, const DrawShaders& sh, uint32_t frame_w, bool copy, uint64_t frame,
                bool scene, DrawConstants& c) {
    NoteLetterbox(a, float(frame_w));
    if (app::BootAspect() != app::AspectMode::Wide) return;
    const float fw = float(frame_w);
    if (sh.fixed_transform && HudView(s, frame_w)) {
        const auto& v = s.viewport;
        float x0 = float(v.x), x1 = float(v.x + v.w);
        c.squeeze[0] = 0.75f;
        c.squeeze[1] = fw * 0.5f;
        if (x0 < fw / 3.0f && x1 <= fw * (2.0f / 3.0f)) c.squeeze[1] = 0.0f;
        else if (x0 >= fw / 3.0f && x1 > fw * (2.0f / 3.0f)) c.squeeze[1] = fw;
        return;
    }
    int reg = -1;
    if (sh.prog) reg = CopiedTexcoords(s, a, sh)[4];
    else if (a.pretransformed) reg = 0;
    float r[4];
    bool flat = reg >= 0 && RegisterRect(a, reg, r);
    if (!flat && sh.fixed_transform) {
        float wv[16], m[16], p[4];
        MatMul(s.transforms[kTsWorld].data(), s.transforms[kTsView].data(), wv);
        MatMul(wv, s.transforms[kTsProjection].data(), m);
        const auto& v = s.viewport;
        bool ortho = std::fabs(m[3]) <= 1e-6f && std::fabs(m[7]) <= 1e-6f && std::fabs(m[11]) <= 1e-6f && m[15] > 0.0f;
        if (ortho && RegisterRect(a, 0, p)) {
            r[0] = r[1] = 1e30f, r[2] = r[3] = -1e30f;
            for (int k = 0; k < 4; k++) {
                float x = p[k & 1 ? 2 : 0], y = p[k & 2 ? 3 : 1];
                float sx = ((x * m[0] + y * m[4] + m[12]) / m[15] + 1) * 0.5f * float(v.w) + float(v.x);
                float sy = (1 - (x * m[1] + y * m[5] + m[13]) / m[15]) * 0.5f * float(v.h) + float(v.y);
                r[0] = std::min(r[0], sx), r[2] = std::max(r[2], sx);
                r[1] = std::min(r[1], sy), r[3] = std::max(r[3], sy);
            }
            flat = true;
        }
    }
    // the legal notices at start are a picture across the frame as a movie is: kept 4:3 with it
    const bool movie = (hppoa_MenuMovie() || hppoa_LegalScreen()) && !copy;
    // xboxrecomp (hppoa): also one as wide as the frame but offset: the menus' dark gradient behind their items slides
    // in (the options menu's at 2.5 to 642.5); narrowed about the middle it ended in a hard edge a quarter of the way
    // into the left side band, the band itself left undarkened
    // xboxrecomp (hppoa): a dialog over the menus (the save's "Are you sure you want to overwrite": a parchment panel
    // the frame's height, 61 to 573 by 3.2 to 485, F12 124506). The rest of the frame's 2D is the dialog's: narrowed
    // about the middle as one 4:3 picture, the pause column's move to the left (below) left out, and its answers'
    // strips too (the selected one, its text scaled up, runs 1.5 to 663: it was taken for a full width backdrop and
    // left unnarrowed, the other moved left as a pause item). The panel starts 3 rows down, a line of the border
    // above showing: it is stretched up to the frame's top
    static uint64_t dialog_frame = ~0ull;
    const float fh0 = fw * 0.75f;
    bool dialog_panel = flat && !movie && !s.pillar && r[1] <= 8.0f && r[3] >= fh0 - 8.0f && r[2] - r[0] >= fw * 0.6f &&
                        r[2] - r[0] <= fw * 0.95f && std::fabs((r[0] + r[2]) * 0.5f - fw * 0.5f) <= 8.0f;
    if (dialog_panel) dialog_frame = frame;
    // only the dialog's own small 2D (its text and answer strips): not the copy of the finished menu frame the game
    // draws across the whole screen afterwards (narrowed again, everything showed twice: F12 124926)
    const bool in_dialog = flat && !movie && !copy && dialog_frame == frame && !dialog_panel && r[3] - r[1] < fh0 * 0.25f;
    if (flat && !movie && !in_dialog && ((r[0] <= 1.0f && r[2] >= fw - 1.0f) || r[2] - r[0] >= fw - 1.0f)) return;
    // a 4:3 screen (hle_d3d8.cpp's Swap) or a movie narrows everything in the frame, 3D too; otherwise only the 2D
    if (!s.pillar && !movie && !flat) return;
    c.squeeze[0] = 0.75f;
    c.squeeze[1] = fw * 0.5f;
    if (dialog_panel && r[1] > -0.5f && r[3] > r[1]) {
        c.squeeze[2] = (r[3] + 0.5f) / (r[3] - r[1]);  // its top to the frame's, its foot where it was
        c.squeeze[3] = r[3];
    }
    if (in_dialog) return;
    // xboxrecomp (hppoa): the inventory's map page, its map wider on the 16:9 screen (by request, 2026-11-09; names
    // crowded over each other, F12 135414). The map's parchment (a 256x256 picture at 124 to 530 by 107 to 401) and
    // the rooms drawn over it (through an orthographic camera) keep the 4:3 frame's width, stretched to the screen's;
    // what sits on it (the names, 256x32 pictures of text, and the dots and Harry's mark) goes to its place on that
    // wider map keeping its own shape: narrowed about its own middle. The title, the room's name and the bars above
    // and below as the rest of the page
    if (flat && !movie && !s.pillar && hppoa_MapScreen()) {
        static uint64_t map_frame = ~0ull;
        static float map[4];
        const float mw = r[2] - r[0], mh = r[3] - r[1];
        const bool parchment = a.pretransformed && mw >= fw * 0.5f && mh >= fh0 * 0.5f && r[0] >= fw * 0.1f &&
                               r[2] <= fw * 0.9f && r[1] >= fh0 * 0.15f && r[3] <= fh0 * 0.9f;
        if (parchment) {
            map_frame = frame;
            std::copy(r, r + 4, map);
            c.squeeze[0] = 0.0f;
            return;
        }
        const float cx = (r[0] + r[2]) * 0.5f, cy = (r[1] + r[3]) * 0.5f;
        if (map_frame == frame && cx >= map[0] && cx <= map[2] && cy >= map[1] && cy <= map[3]) {
            if (!a.pretransformed && mw >= 64.0f) c.squeeze[0] = 0.0f;  // the rooms
            else c.squeeze[1] = cx;
            return;
        }
    }
    // xboxrecomp: in the game itself (not its menus) HUD wholly in the left or right third keeps its distance from
    // that edge of the 16:9 screen instead of the 4:3 picture's: the A B X Y buttons round Harry's portrait (health)
    // sat a fifth of the way in from the right. Menus and cutscenes keep the middle. A side's HUD may reach into the
    // middle third without crossing it: the action prompt under the A button ("Open", "Dodge") is wider than the
    // button and stayed in the middle while the button went to the edge. 2D centred on the frame is no side's
    // A button icon in a line of text ("press [icon] to go to the Inventory") is drawn after the text's block, inside
    // it: it goes with the block (the middle), not with the side third it happens to fall in
    static uint64_t block_frame = ~0ull;
    static float block[4];
    // a menu's panel that starts at a side of the 4:3 picture (the pause menu's dark semicircle, from the left edge)
    // starts at that side of the 16:9 screen, not a strip of the scene in from it. An upright quad exactly at the
    // edge only: the selection glow is a turning sprite, and on some frames a corner of it meets the edge
    if (flat && !movie && !s.pillar && !hppoa_InGame() && reg >= 0 && UprightRect(a, reg, r)) {
        if (std::fabs(r[0] + 0.5f) <= 0.5f && r[2] <= fw * 0.5f) c.squeeze[1] = 0.0f;
        else if (std::fabs(r[2] - (fw - 0.5f)) <= 0.5f && r[0] >= fw * 0.5f) c.squeeze[1] = fw;
    }
    // xboxrecomp (hppoa): with the half moon panel gone (HiddenDraw), the pause menu's column down the left (its title,
    // items and their sparkles: starting in the left third) keeps its distance from the 16:9 screen's left edge instead
    // of the 4:3 picture's. Told by where they start: an item's text strip reaches well past the middle (Resume Game's
    // 107 to 650), and by their middle Save Game (100 to 555, middle 327.5) stayed behind its sparkle. The "A to
    // select" tag stays in the middle: it is below the column (rows 385 on), and while it grows its text's middle is
    // as far off the frame's as Save Game's. Other centred 2D (within 4 pixels) stays too. The main menu as it was
    // (by request, 2026-11-09). By the middle row, not the bottom: the selected item's glow is two turning quads drawn
    // a triangle at a time, and as it turned a triangle's corner passed row 360: that triangle went back about the
    // middle, its other half to the left (F12 004111: Quit Game's glow split, 469 to row 371 and 470 to row 320)
    const float fh = fw * 0.75f;
    // xboxrecomp (hppoa): the HUD over the scene, as below; also from PauseGame until the menus leave "ig" (and from
    // their return until UnpauseGame), "Pausing" up: the A B X Y buttons went back in to the 4:3 picture's side then
    const bool hud = flat && !movie && !s.pillar && hppoa_GameScreen() && scene;
    // xboxrecomp (hppoa): the compass the game turns while it saves or loads (a 64x64 picture at 63.5 to 127.5, top
    // left), on a black frame (no 3D): kept to the 16:9 screen's left edge, as the HUD is. Neither rule here took it
    // (no scene, and the menus on "ig" or "loading"): it was narrowed about the middle and came in to the 4:3
    // picture's corner, a frame after one at the edge (F12 004246, entering a level)
    if (flat && !movie && !s.pillar && !scene && c.squeeze[1] == fw * 0.5f && r[2] <= fw / 3.0f && r[3] <= fh / 3.0f &&
        std::fabs((r[2] - r[0]) - 64.0f) <= 1.0f && std::fabs((r[3] - r[1]) - 64.0f) <= 1.0f) {
        TextureDesc d;
        if (s.textures[0] && Describe(s.textures[0], d) && d.width == 64 && d.height == 64) c.squeeze[1] = 0.0f;
    }
    // not the inventory, which pauses the game too: its left page (Quests, the item grid, the books and "Press"
    // on the hint bar) went to the screen's edge, away from the rest of the page (F12 104712)
    if (!hud && flat && !movie && !s.pillar && hppoa_Paused() && !hppoa_Inventory() && c.squeeze[1] == fw * 0.5f && r[0] < fw / 3.0f &&
        (r[1] + r[3]) * 0.5f < fh * 0.75f && std::fabs((r[0] + r[2]) * 0.5f - fw * 0.5f) > 4.0f) {
        c.squeeze[1] = 0.0f;
    }
    // xboxrecomp (hppoa): and only over the scene. Saving from the pause menu draws its compass icon (x 63 to 127) on
    // a black frame while the menus still say "ig" for a moment: it went to the screen's left side, then back in
    // with the 4:3 picture once they moved on. A frame without 3D is a menu's, whatever the menus last named
    if (hud) {
        // a cutscene's subtitle (the game's text, rendered to a P8 texture) stays about the middle: a short line's box
        // (254.5 to 539.5) starts in the middle third and ends in the right third, and went to the right side with the
        // HUD once cutscenes counted as the game's screen; a long line crosses the middle and stayed (F12 005645,
        // 2026-11-10). In play the action prompts (text too) keep their side
        if (hppoa_CutsceneScreen()) {
            TextureDesc d;
            if (s.textures[0] && Describe(s.textures[0], d) && d.format == 0x0B) return;
        }
        const bool centred = std::fabs((r[0] + r[2]) * 0.5f - fw * 0.5f) <= 16.0f;
        const bool in_block = block_frame == frame && r[0] >= block[0] && r[2] <= block[2] && r[1] >= block[1] &&
                              r[3] <= block[3];
        if (in_block) return;
        if (!centred && r[0] >= fw / 3.0f && r[2] > fw * (2.0f / 3.0f)) {
            c.squeeze[1] = fw;
        } else if (!centred && r[0] < fw / 3.0f && r[2] <= fw * (2.0f / 3.0f)) {
            c.squeeze[1] = 0.0f;
        } else if (r[2] - r[0] >= 64.0f) {  // a block of text (or other wide 2D) left in the middle
            block_frame = frame;
            std::copy(r, r + 4, block);
        }
    }
}

[[maybe_unused]] bool Squeeze2D(const State& s, const Assembled& a, const DrawShaders& sh, uint32_t frame_w,
                                uint32_t frame_h, WideFrame& wf, DrawConstants& c) {
    if (app::BootAspect() != app::AspectMode::Wide) return false;
    const float fw = float(frame_w), fh = float(frame_h);
    if (s.viewport.x == 0 && float(s.viewport.w) == fw && s.viewport.y > 0 && s.viewport.y <= 48)
        wf.vp_y = float(s.viewport.y), wf.vp_h = float(s.viewport.h);
    int reg = -1;
    if (sh.prog) reg = CopiedTexcoords(s, a, sh)[4];
    else if (a.pretransformed) reg = 0;
    float r[4];  // the draw's rectangle in frame pixels
    bool flat = reg >= 0 && RegisterRect(a, reg, r);
    bool marker = false;
    if (sh.fixed_transform) {
        float wv[16], m[16];
        MatMul(s.transforms[kTsWorld].data(), s.transforms[kTsView].data(), wv);
        MatMul(wv, s.transforms[kTsProjection].data(), m);
        const auto& v = s.viewport;
        if (std::fabs(m[3]) > 1e-6f || std::fabs(m[7]) > 1e-6f || std::fabs(m[11]) > 1e-6f || !(m[15] > 0.0f)) {
            // a menu's own camera (hle_video.c: left 4:3, its x scale tagged two float steps up): its 3D (the
            // inventory's hourglass and books) goes with the menu's 2D, placed by where its middle lands
            static const float kMenuX = std::nextafter(std::nextafter(2.4f, 3.0f), 3.0f);
            const float* pm = s.transforms[kTsProjection].data();
            bool menu = pm[0] == kMenuX && pm[5] == 3.2f;
            if (!menu || !(m[15] > 1e-6f)) {
                wf.scene++;  // the scene through the widened camera, left as it is
                return false;
            }
            float px = (m[12] / m[15] + 1) * 0.5f * float(v.w) + float(v.x);
            float py = (1 - m[13] / m[15]) * 0.5f * float(v.h) + float(v.y);
            r[0] = r[2] = px;
            r[1] = r[3] = py;
            flat = true;
        }
        // 2D; a long text batch's rectangle may not be readable: taken as a point in the middle (no backdrop or bar)
        float p[4];
        if (!flat && RegisterRect(a, 0, p)) {
            r[0] = r[1] = 1e30f;
            r[2] = r[3] = -1e30f;
            for (int k = 0; k < 4; k++) {
                float x = p[k & 1 ? 2 : 0], y = p[k & 2 ? 3 : 1];
                float sx = ((x * m[0] + y * m[4] + m[12]) / m[15] + 1) * 0.5f * float(v.w) + float(v.x);
                float sy = (1 - (x * m[1] + y * m[5] + m[13]) / m[15]) * 0.5f * float(v.h) + float(v.y);
                r[0] = std::min(r[0], sx), r[2] = std::max(r[2], sx);
                r[1] = std::min(r[1], sy), r[3] = std::max(r[3], sy);
            }
            // a single quad not starting at its top left corner is turned: a mark the title spins on the scene (the
            // aiming circle), placed through the widened camera
            float f[2];
            if (a.vertex_count == 6 && RegisterFirst(a, 0, f)) {
                float fx = ((f[0] * m[0] + f[1] * m[4] + m[12]) / m[15] + 1) * 0.5f * float(v.w) + float(v.x);
                float fy = (1 - (f[0] * m[1] + f[1] * m[5] + m[13]) / m[15]) * 0.5f * float(v.h) + float(v.y);
                marker = fx > r[0] + 0.5f || fy > r[1] + 0.5f;
                // the aiming circle spins through upright too (a frame now and then of its lock-on): a texture once
                // seen turned stays a mark, or that frame would go with the HUD's side and show beside the target
                if (s.textures[0]) {
                    if (marker) spun.insert(s.textures[0]);
                    else if (spun.count(s.textures[0])) marker = true;  // upright it is ~6% wider than tall
                }
            }
        } else if (!flat) {
            r[0] = r[2] = fw * 0.5f;
            r[1] = r[3] = fh * 0.5f;
        }
        flat = true;
    }
    if (!flat) {
        // a 4:3 screen (hle_d3d8.cpp's Swap) narrows everything else in the frame, 3D too
        if (s.pillar) c.squeeze[0] = 0.75f, c.squeeze[1] = fw * 0.5f;
        return false;
    }
    if (r[0] <= 1.0f && r[2] >= fw - 1.0f) {
        if (!s.textures[0]) {
            // a band along the top or the bottom of the viewport (inside the overscan rows): a cutscene's bars
            // also while they slide in or out: the fixed band WideCrop shows leaves them out
            const float vt = float(s.viewport.y), vb = float(s.viewport.y + s.viewport.h);
            // an opaque fill over the whole frame hides the scene drawn so far: a menu over it (the Remembrall's,
            // under its fog); the game's fades blend
            if (r[1] <= vt + 1.0f && r[3] >= vb - 1.0f && !SimpleState(s, kMBlendEnable)) wf.scene = 0;
            if (r[3] - r[1] < fh * 0.25f && (r[1] <= vt + 1.0f || r[3] >= vb - 1.0f)) {
                wf.bars = true;
                wf.bars_h += r[3] - r[1];
                return wf.bars_out;
            }
            return false;
        }
        // over the scene (gameplay, a cutscene) a picture across the frame is a screen effect: left as drawn
        if (wf.scene >= 16) return false;
        wf.backdrop = true;
        // the main menu's moving background (frontend.xmv, a 640x480 YUY2 frame) and the loading screen: the full
        // 16:9 width, their middle 360 lines (4/3 as tall about the middle) instead of the 4:3 picture between bars
        TextureDesc d;
        // the loading screen's picture too (over most of the frame's height: not its bar or icon)
        const bool movie =
            hppoa_MenuMovie() && Describe(s.textures[0], d) && d.format == 0x24 && d.width == 640 && d.height == 480;
        if (movie || (hppoa_LoadingScreen() && r[3] - r[1] >= fh * 0.9f)) {
            // RECOMP_CAP_LOADING=1: the frame after the picture first goes up in each load is captured
            static const bool cap_loading = GetEnvironmentVariableW(L"RECOMP_CAP_LOADING", nullptr, 0) != 0;
            static int loads_seen = -1;
            if (cap_loading && !movie && loads_seen != hppoa_LoadingCount()) {
                loads_seen = hppoa_LoadingCount();
                g_capture = true;
            }
            c.squeeze[0] = 1.0f;
            c.squeeze[1] = fw * 0.5f;
            c.squeeze[2] = 4.0f / 3.0f;
            c.squeeze[3] = (r[1] + r[3]) * 0.5f;
            return false;
        }
    }
    // a panel over half the frame or more each way over the scene (the pause menu's box, 60-580 x 64-440): a menu is
    // up, and its text and marks after it in the frame are squeezed about the middle with it, not kept to a side
    if (wf.scene >= 16 && !wf.backdrop && r[2] - r[0] >= fw * 0.5f && r[3] - r[1] >= fh * 0.5f) wf.panel = true;
    // a menu's mist (the Remembrall's fog: soft puffs drawn several times their texture's size, past the frame's
    // sides) spreads over the 16:9 width; what sits on it keeps its 4:3 shape
    if (wf.scene < 16 && !wf.backdrop && a.vertex_count == 6 && s.textures[0]) {
        TextureDesc d;
        if (Describe(s.textures[0], d) && r[2] - r[0] >= 2.0f * float(d.width) && r[3] - r[1] >= 2.0f * float(d.height))
            return false;
    }
    // where the 2D goes in gameplay (the scene drawn before it in this frame): 3/4 as wide. The HUD on the
    // left or right side keeps its distance from that side of what is shown (the overscan crop's); the rest, and the
    // marks the title places on the scene through the widened camera (the aiming circle), keep their own middle. On a
    // 4:3 screen (no scene) everything is squeezed about the frame's middle. As the line x' = sx * x + bx, y' = ...
    const bool live = wf.scene >= 16 && !wf.backdrop;
    const float vy = wf.vp_h > 0 ? wf.vp_y : wf.view_h > 0 ? wf.view_y : 0;
    const float vh = wf.vp_h > 0 ? wf.vp_h : wf.view_h > 0 ? wf.view_h : fh;
    float gc[4];  // gameplay's crop
    WideCrop(fw, fh, vy, vh, false, gc);
    const float gcw = gc[2] > 0 ? gc[2] : fw, gx = gc[2] > 0 ? gc[0] : 0;
    // a side's HUD also reaches into the middle third without crossing it (a boss's health bar out from its
    // portrait): it goes with that side
    // 2D centred on the frame is no side's (a subtitle a third of the frame wide: its shadow, a pixel or two to the
    // right, would cross into the right third while the line itself does not, and the two part)
    const bool centred = std::fabs((r[0] + r[2]) * 0.5f - fw * 0.5f) <= 16.0f;
    const bool left = !centred && r[0] < fw / 3 && r[2] <= fw * 2 / 3;
    const bool right = !centred && r[0] >= fw / 3 && r[2] > fw * 2 / 3;
    // a button icon in a line of text (hle_video.c): goes with its line, which is no side's HUD; a cutscene's subtitle
    // is centred, so the icon squeezes about the middle with it (an A at the line's start fell in the left third and
    // was left out as HUD)
    const bool icon = s.textures[0] && hppoa_TextIcon(s.textures[0]);
    float anchor = fw * 0.5f;
    if (live) {
        if (icon && wf.cut) anchor = fw * 0.5f;
        else if (marker) anchor = (r[0] + r[2]) * 0.5f;
        else if (wf.panel) anchor = fw * 0.5f;
        else if (left) anchor = gx;
        else if (right) anchor = fw - gx;
        else anchor = (r[0] + r[2]) * 0.5f;
    }
    // a cutscene's HUD (health, buttons: the sides' 2D over the scene) is not shown; its subtitles are
    if (wf.cut && live && !marker && !icon && !wf.panel && (left || right)) {
        static std::set<uint32_t> told;  // each texture once: what a cutscene left out (an icon not known as one?)
        if (told.insert(s.textures[0]).second)
            fprintf(stderr, "[WIDE] cutscene HUD left out: t0 %08X rect %.0f %.0f %.0f %.0f verts %u\n", s.textures[0], r[0],
                    r[1], r[2], r[3], a.vertex_count), hppoa_TextIconDump();
        return true;
    }
    float sx = 0.75f, bx = anchor * (1 - sx), sy = 1, by = 0;
    if (wf.cut && !wf.backdrop) {
        // a zoomed cutscene: from the gameplay crop to the cutscene's, so the HUD sits
        // where gameplay has it and the subtitles (in the lower bar) come up into the picture
        float cr[4];
        WideCrop(fw, fh, vy, vh, true, cr);
        float kx = cr[2] / gcw, ky = cr[3] / vh;
        bx = cr[0] + kx * (bx - gx);
        sx *= kx;
        by = cr[1] - ky * vy;
        sy = ky;
    }
    c.squeeze[0] = sx;
    c.squeeze[1] = bx / (1 - sx);
    if (std::fabs(sy - 1) > 1e-4f) c.squeeze[2] = sy, c.squeeze[3] = by / (1 - sy);
    return false;
}

// a stage without a texture gets a null view of its kind; the samplers read the views after
void D3D12Backend::SetTextureViewsAndSamplers(const State& s, const Assembled& a, DrawTextures& stages) {
    uint32_t slot = AllocSrvSlots(4);
    for (int i = 0; i < 4; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC& sv = stages.sv[i];
        if (!stages.res[i]) {
            sv.ViewDimension = NullViewDimension(stages.info[i].kind);
            sv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        }
        dev_->CreateShaderResourceView(stages.res[i], &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
    cl_->SetGraphicsRootDescriptorTable(2, Gpu(srv_heap_.Get(), slot, srv_inc_));
    uint32_t samplers = EnsureStageSamplers(s, stages, UnitTexcoords(a));
    cl_->SetGraphicsRootDescriptorTable(3, Gpu(smp_heap_.Get(), samplers, smp_inc_));
}

// viewport covers the target, the guest viewport is in the shader; scissor clips to it
void D3D12Backend::SetViewportScissor(const State& s, uint32_t gw, const float squeeze[4]) {
    float W = float(bound_rt_ ? bound_rt_->w : bound_ds_->w);
    float H = float(bound_rt_ ? bound_rt_->h : bound_ds_->h);
    D3D12_VIEWPORT vp{0, 0, W, H, 0, 1};
    cl_->RSSetViewports(1, &vp);
    float sc = W / gw;
    float r[4] = {float(s.viewport.x), float(s.viewport.y), float(s.viewport.x + s.viewport.w),
                  float(s.viewport.y + s.viewport.h)};
    if (s.scissor_count) {
        r[0] = std::max(r[0], float(s.scissors[0]));
        r[1] = std::max(r[1], float(s.scissors[1]));
        r[2] = std::min(r[2], float(s.scissors[2]));
        r[3] = std::min(r[3], float(s.scissors[3]));
    }
    // xboxrecomp (hppoa): a menu's 2D clipped only by the frame's sides, which on the console are the TV's edges
    const bool whole_width = r[0] <= 0.0f && r[2] >= float(gw);
    // xboxrecomp: 16:9, a squeezed 2D draw's clip rectangle (a text box's) moves with it
    if (squeeze[0] != 0) for (int k : {0, 2}) r[k] = (r[k] - squeeze[1]) * squeeze[0] + squeeze[1];
    // xboxrecomp (hppoa): ...and those edges stay the screen's. The pause menu slides its items in from past the
    // frame's left side (x -109, -37, then 28); squeezed about the middle that side is a quarter of the way into the
    // left panel (stretched to the screen's edge), and "Save Game" came into view cut at a line there for a moment.
    // Menus only, and not on a 4:3 screen, whose sides are its black bars
    // (about either side too: the menus' left column, kept to the screen's left edge)
    if (squeeze[0] != 0 && whole_width && !s.pillar && !hppoa_InGame()) {
        r[0] = 0.0f;
        r[2] = float(gw);
    }
    if (squeeze[2] != 0) for (int k : {1, 3}) r[k] = (r[k] - squeeze[3]) * squeeze[2] + squeeze[3];
    LONG x0 = LONG(r[0] * sc), y0 = LONG(r[1] * sc), x1 = LONG(r[2] * sc), y1 = LONG(r[3] * sc);
    D3D12_RECT rect{std::clamp<LONG>(x0, 0, LONG(W)), std::clamp<LONG>(y0, 0, LONG(H)),
                    std::clamp<LONG>(x1, 0, LONG(W)), std::clamp<LONG>(y1, 0, LONG(H))};
    cl_->RSSetScissorRects(1, &rect);
}

void D3D12Backend::SetBlendFactorStencilRef(const State& s) {
    float bf[4];
    Color(SimpleState(s, kMBlendColor), bf);
    cl_->OMSetBlendFactor(bf);
    cl_->OMSetStencilRef(SimpleState(s, kMStencilRef) & 0xFF);
}

D3D_PRIMITIVE_TOPOLOGY PrimitiveTopology(const Assembled& a) {
    if (a.topology == Topology::Points) return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    if (a.topology == Topology::Lines) {
        return a.strip ? D3D_PRIMITIVE_TOPOLOGY_LINESTRIP : D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    }
    return a.strip ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
}

void D3D12Backend::UploadVertexBuffers(const Assembled& a) {
    D3D12_VERTEX_BUFFER_VIEW vbv[16];
    for (uint32_t i = 0; i < a.buffer_count; i++) {
        const VertexBuffer& b = a.buffers[i];
        Alloc vb = Upload(b.bytes, 16);
        std::memcpy(vb.cpu, b.src, b.bytes);
        vbv[i] = {vb.gpu, b.bytes, b.stride};
    }
    cl_->IASetVertexBuffers(0, a.buffer_count, vbv);
}

void D3D12Backend::DrawGeometry(const Assembled& a) {
    UploadVertexBuffers(a);
    cl_->IASetPrimitiveTopology(PrimitiveTopology(a));
    if (a.index_count) {
        uint32_t ibytes = a.index_count * (a.index16 ? 2 : 4);
        Alloc ib = Upload(ibytes, 4);
        std::memcpy(ib.cpu, a.index_src, ibytes);
        D3D12_INDEX_BUFFER_VIEW ibv{ib.gpu, ibytes, a.index16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT};
        cl_->IASetIndexBuffer(&ibv);
        cl_->DrawIndexedInstanced(a.index_count, 1, 0, a.base_vertex, 0);
    } else {
        cl_->DrawInstanced(a.draw_count, 1, 0, 0);
    }
}

// xboxrecomp: why draws were left out, logged every 10 s while any are (a missing floor or wall is one of these)
namespace {
enum DropReason { kDropAssemble, kDropTargets, kDropHdr, kDropTextures, kDropPipeline, kDropReasons };
uint64_t g_drops[kDropReasons], g_draws;
void CountDrop(int why) {
    static auto t0 = std::chrono::steady_clock::now();
    if (why >= 0) g_drops[why]++;
    g_draws++;
    auto now = std::chrono::steady_clock::now();
    if (now - t0 < std::chrono::seconds(10)) return;
    t0 = now;
    uint64_t any = 0;
    for (uint64_t d : g_drops) any += d;
    if (any)
        LOGI("d3d12: %llu draws, left out: %llu assembly, %llu targets, %llu hdr, %llu textures, %llu pipeline",
             (unsigned long long)g_draws, (unsigned long long)g_drops[0], (unsigned long long)g_drops[1],
             (unsigned long long)g_drops[2], (unsigned long long)g_drops[3], (unsigned long long)g_drops[4]);
    std::memset(g_drops, 0, sizeof(g_drops));
    g_draws = 0;
}
}  // namespace

// xboxrecomp (hppoa): a lever's panel drawn alone, texture * TFACTOR with the title fading the factor to black. The
// finished lever draws its grey texture (stage 0, MODULATE2X with diffuse) and adds the panel * TFACTOR on stage 1
// (MULTIPLYADD); each such draw pairs a panel with its grey texture (levers have panels of their own). The panel drawn
// alone is drawn the same way over its grey texture, so the grey is there from the start and only the panel's dot
// fades. A panel known only by its hash (AE31DA34), before its finished lever has been seen, goes over the last grey
// texture seen, or with none over the lit colour alone (diffuse + panel * TFACTOR).
// a panel some finished lever part was seen with
bool D3D12Backend::LeverPanelKnown(uint32_t data) const {
    for (const auto& [k, v] : lever_pairs_)
        if (k >> 32 == data) return true;
    return false;
}

const State& D3D12Backend::LeverPanelFade(const State& s, uint32_t count) {
    // the panel drawn alone is opaque: a blended draw (the glow added over the face) is left as it is
    if (!s.textures[0] || s.simple[0x304 / 4]) return s;
    TextureDesc d;
    const auto& t0 = s.tss[0];
    const auto& t1 = s.tss[1];
    if (s.textures[1] && t0[kTssColorOp] == 5 && t1[kTssColorOp] == 23 && (t1[kTssColorArg1] & 0x3F) == 2 &&
        (t1[kTssColorArg2] & 0x3F) == 3 && Describe(s.textures[1], d)) {
        uint64_t key = uint64_t(d.data) << 32 | count;
        auto it = lever_pairs_.find(key);
        if (it == lever_pairs_.end() || it->second != s.textures[0]) {
            lever_pairs_[key] = s.textures[0];
            static int left = 40;
            if (left > 0) left--, fprintf(stderr, "[LEVER] panel %08X (%ux%u fmt %X), %u vertices, seen over grey texture %08X\n",
                                          d.data, d.width, d.height, d.format, count, s.textures[0]);
        }
        lever_base_ = s.textures[0];
        return s;
    }
    if (lever_pairs_.empty() && lever_panels_.empty()) return s;
    if (!Describe(s.textures[0], d)) return s;
    auto pair = lever_pairs_.find(uint64_t(d.data) << 32 | count);
    if (pair == lever_pairs_.end() && !lever_panels_.count(d.data) && !LeverPanelKnown(d.data)) return s;
    if (!s.rs[kRsLighting] || t0[kTssColorOp] != 4 || (t0[kTssColorArg1] & 0x3F) != 2 ||
        (t0[kTssColorArg2] & 0x3F) != 3 || t1[kTssColorOp] > 1) {
        // a panel on stage 0 in a draw this does not take: what the fade does that it misses
        static int left = 40;
        if (left > 0 && !(s.simple[0x304 / 4] && t0[kTssColorOp] == 4 && (t0[kTssColorArg2] & 0x3F) == 3)) {
            left--;
            fprintf(stderr, "[LEVER] panel draw left as is: lighting %u, stage 0 op %u args %X %X %X alpha op %u, stage 1 op %u, "
                    "tfactor %08X, blend %u\n", s.rs[kRsLighting], t0[kTssColorOp], t0[kTssColorArg0], t0[kTssColorArg1],
                    t0[kTssColorArg2], t0[kTssAlphaOp], t1[kTssColorOp], s.rs[kRsTextureFactor], s.simple[0x304 / 4]);
        }
        return s;
    }
    uint32_t grey = pair != lever_pairs_.end() ? pair->second : lever_base_;
    TextureDesc b;
    bool base = grey && Describe(grey, b) && b.data != d.data;
    static std::unordered_set<uint64_t> told;  // once per panel and kind
    int kind = pair != lever_pairs_.end() ? 0 : base ? 1 : 2;
    if (told.size() < 64 && told.insert(uint64_t(d.data) << 2 | uint64_t(kind)).second) {
        fprintf(stderr, "[LEVER] panel %08X drawn over %s, not black\n", d.data,
                pair != lever_pairs_.end() ? "its finished lever's grey texture"
                : base ? "the last grey texture seen (its own finished lever not seen yet)"
                       : "its lit colour (no finished lever seen yet)");
    }
    lever_state_ = s;
    auto& t = lever_state_.tss;
    if (!base) {
        t[0][kTssColorOp] = 23, t[0][kTssColorArg0] = 0, t[0][kTssColorArg1] = 2, t[0][kTssColorArg2] = 3;
        return lever_state_;
    }
    t[1] = t[0];  // the panel moves to stage 1 with its sampling and coordinates
    lever_state_.transforms[kTsTexture0 + 1] = s.transforms[kTsTexture0];
    lever_state_.textures[1] = s.textures[0];
    lever_state_.textures[0] = grey;
    t[0][kTssColorOp] = 5, t[0][kTssColorArg1] = 2, t[0][kTssColorArg2] = 0;
    t[1][kTssColorOp] = 23, t[1][kTssColorArg0] = 1, t[1][kTssColorArg1] = 2, t[1][kTssColorArg2] = 3;
    t[1][kTssAlphaOp] = 1;
    return lever_state_;
}

// xboxrecomp (hppoa): characters as in play. In cutscenes the title draws faces, hands and robes with a second pass of
// their own texture (stage 1 MODULATE2X by the stage 0 texture, same coordinates), which play never uses on them: it
// squares the texture (orange faces), and since the characters' materials leave stage 1 to the object drawn before,
// it came and went from frame to frame as the draw order changed (the train cutscene's blink). That stage passes the
// colour on unchanged (SELECTARG2 of the current colour), so they look as they do in play, and later stages (the
// window light) still apply.
const State& D3D12Backend::NoSecondSkinPass(const State& s) {
    if (!s.textures[0]) return s;
    const auto& t0 = s.tss[0];
    for (int i = 1; i < 4; i++) {
        const auto& t = s.tss[size_t(i)];
        if (t[kTssColorOp] <= 1) return s;
        if (s.textures[size_t(i)] != s.textures[0] || t[kTssColorOp] != 5 || t[kTssTexCoordIndex] != t0[kTssTexCoordIndex])
            continue;
        static bool told;
        if (!told) told = true, fprintf(stderr, "[SKIN] a character's second pass of its own texture left out (as in play)\n");
        if (&s != &skin_state_) skin_state_ = s;
        auto& u = skin_state_.tss[size_t(i)];
        u[kTssColorOp] = 3, u[kTssColorArg2] = 1;
        u[kTssAlphaOp] = 3, u[kTssAlphaArg2] = 1;
        return skin_state_;
    }
    return s;
}

// xboxrecomp (hppoa): the train's window light on the characters in cutscenes. The title adds the passing windows'
// light to the walls (and some characters) as a texture projected from the camera space position (an ADD stage,
// D3DTSS_TCI_CAMERASPACEPOSITION, through a texture matrix in camera space). The last such stage seen is kept, and a
// cutscene's character draw without one (lit, or unlit with no vertex colours) gets it on its first free stage: the
// same projection, so the light sweeps over the characters with the walls.
const State& D3D12Backend::WindowLightOnCharacters(const State& s) {
    for (int i = 1; i < 4; i++) {
        const auto& t = s.tss[size_t(i)];
        if (t[kTssColorOp] <= 1) break;
        if (t[kTssColorOp] == 7 && (t[kTssTexCoordIndex] >> 16) == 2 && s.textures[size_t(i)]) {
            window_.tex = s.textures[size_t(i)], window_.tss = t, window_.tm = s.transforms[kTsTexture0 + size_t(i)];
            window_.ms = GetTickCount64();
            return s;
        }
    }
    if (!window_.tex || GetTickCount64() - window_.ms > 200 || !s.textures[0] || !hppoa_Cutscene()) return s;
    bool fvf = !(s.vs_handle & 1);
    if (!s.rs[kRsLighting] && !(fvf && !(s.vs_handle & 0x40))) return s;
    int j = 1;
    while (j < 4 && s.tss[size_t(j)][kTssColorOp] > 1) j++;
    if (j >= 4) return s;
    static bool told;
    if (!told) told = true, fprintf(stderr, "[TRAIN] cutscene: the window light added to the characters\n");
    if (&s != &window_state_) window_state_ = s;
    window_state_.tss[size_t(j)] = window_.tss;
    window_state_.tss[size_t(j)][kTssAlphaOp] = 3, window_state_.tss[size_t(j)][kTssAlphaArg2] = 1;
    window_state_.textures[size_t(j)] = window_.tex;
    window_state_.transforms[kTsTexture0 + size_t(j)] = window_.tm;
    if (j < 3) window_state_.tss[size_t(j + 1)][kTssColorOp] = 1, window_state_.tss[size_t(j + 1)][kTssAlphaOp] = 1;
    return window_state_;
}

// xboxrecomp (hppoa): the game's own bloom (system_bloomrendermanagerstartup.hpb). The frame's bright parts go to a
// 256x256 target, halved down a chain (128, 64, 32) and blurred, and one quad over the whole frame adds the four back
// (blend ONE ONE, F12 004111's draw 0454). That quad's colour is scaled by F10's Bloom (0 leaves it out, 100 as the
// console, 200 twice). Told by the blend, the full frame quad and its textures: two or more, all of them render
// targets smaller than the frame
bool D3D12Backend::GameBloomComposite(const State& s, const Assembled& a, const PsoKey& k) {
    if (!k.blend || k.src != 1 || k.dst != 1 || a.vertex_count != 4 || !a.pretransformed) return false;
    if (!bound_rt_ || bound_rt_->gw != guest_w_ || bound_rt_->gh != guest_h_) return false;
    float r[4];
    if (!RegisterRect(a, 0, r) || r[0] > 0.5f || r[1] > 0.5f || r[2] < float(guest_w_) - 1.0f ||
        r[3] < float(guest_h_) - 1.0f)
        return false;
    int n = 0;
    for (int i = 0; i < 4; i++) {
        if (!s.textures[i]) continue;
        TextureDesc d;
        if (!Describe(s.textures[i], d) || !targets_.count(TargetKey(d.data, false)) || d.width >= guest_w_) return false;
        n++;
    }
    return n >= 2;
}

// xboxrecomp (hppoa): draws left out on purpose, by request (2026-11-09):
//  - a cutscene's letterbox (State::cutscene_bars): the two untextured bands across the frame's top and bottom, also
//    while they slide; the scene is drawn behind them, so the picture reaches the screen's edges. Still
//    noted, hppoa_Letterbox (and so hppoa_Cutscene) goes by them
//  - the pause menu's dark half moon panel down the left side (the main menu keeps it): its DXT5 128x256 texture, told by its
//    level 0 bytes (FNV-1a 073195DD, as F12's frame.txt prints it) since its address moves from run to run
bool HiddenDraw(const State& s, const Assembled& a, uint32_t gw, uint32_t gh) {
    const float fw = float(gw), fh = float(gh);
    TextureDesc d;
    // the bars are untextured, or (the train's) a black DXT5 4x4: a texture that small is a colour, not a picture
    // (the pause menu's parchment strips across the top and bottom are 512x64)
    const bool plain = !s.textures[0] || (Describe(s.textures[0], d) && d.width <= 8 && d.height <= 8);
    if (s.cutscene_bars && app::BootAspect() == app::AspectMode::Wide && a.pretransformed && plain && gw && gh) {
        float r[4];
        if (RegisterRect(a, 0, r) && r[0] <= 0.5f && r[2] >= fw - 1.0f && r[3] - r[1] <= fh / 3.0f &&
            (r[1] <= 0.5f || r[3] >= fh - 1.0f)) {
            NoteLetterbox(a, fw);
            return true;
        }
    }
    // the compass the game turns in the top left corner while it saves or loads (a 64x64 picture at 63.5 to 127.5
    // each way, F12 004246), left out with F10's Hide the saving / loading compass (by request, 2026-11-09: it went
    // to the 4:3 picture's corner as a cutscene started). Upright and that size wherever it is drawn
    if (a.pretransformed && s.textures[0] && hppoa_HideSaveIcon() && Describe(s.textures[0], d) && d.width == 64 &&
        d.height == 64) {
        float r[4];
        if (RegisterRect(a, 0, r) && std::fabs(r[0] - 63.5f) <= 2.0f && std::fabs(r[1] - 63.5f) <= 2.0f &&
            std::fabs(r[2] - 127.5f) <= 2.0f && std::fabs(r[3] - 127.5f) <= 2.0f)
            return true;
    }
    // 2D placed off the 4:3 frame's side, not seen on the Xbox: narrowed about the middle it came into the 16:9
    // screen's side bands (the map page's "MapText9", "MapText10", at 637 to 856, under its bottom bar: F12 135414)
    if (app::BootAspect() == app::AspectMode::Wide && a.pretransformed && s.textures[0] && gw) {
        float r[4];
        if (RegisterRect(a, 0, r) && (r[0] >= fw - 4.0f || r[2] <= 4.0f)) return true;
    }
    if (hppoa_Paused() && s.textures[0] && Describe(s.textures[0], d) && d.format == 0x0F && d.width == 128 &&
        d.height == 256) {
        static uint32_t addr, hash;
        if (d.data != addr) {
            const uint8_t* g = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
            uint32_t n = std::min<uint32_t>(LevelSize(d, 0), 4u << 20), h = 2166136261u;
            for (uint32_t k = 0; g && k < n; k++) h = (h ^ g[k]) * 16777619u;
            addr = d.data;
            hash = h;
        }
        if (hash == 0x073195DDu) return true;
    }
    return false;
}

void D3D12Backend::Draw(const State& s0, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    const State& s = WindowLightOnCharacters(NoSecondSkinPass(LeverPanelFade(s0, count)));
    if (cap_) CapStep(s.rt);
    Assembled& a = asm_;
    bool assembled = Assemble(s, prim, start, count, indices, a);
    if (!assembled) return CountDrop(kDropAssemble);
    if (HiddenDraw(s, a, guest_w_, guest_h_)) return;
    // a versus level's first frame draws everything once (thousands of draws, over 64 MB): past the ring every upload
    // was a buffer of its own, seconds of creating and then freeing them
    if (CurrentFrame().used > kUploadSize - kUploadReserve) Submit(true);
    if (!BindTargets(s)) return CountDrop(kDropTargets);
    if (!EnterHdrForScene(s, a)) return CountDrop(kDropHdr);
    if (!BloomForScene(s, a)) return CountDrop(kDropTargets);

    // vertex shader
    DrawShaders sh = ClassifyShaders(s, a, count);
    if (sh.prog) EnsureProgramHash(s);
    sh.vkey = VertexKey(s, a, sh);
    sh.vh = EnsureVertexShader(s, a, sh);

    // stage textures, then the pixel shader made for them
    DrawTextures stages;
    if (!BindStageTexturesAndTargets(s, stages)) return CountDrop(kDropTextures);
    PixelKey pk = MakePixelKey(s, stages.info);
    sh.wdepth = UseLogDepth(s, a);
    pk.wbuf = sh.wdepth;
    pk.quad = sh.quad;
    sh.pkey = Hash(&pk, sizeof(pk));
    sh.ph = EnsurePixelShader(pk, sh.pkey);

    // pipeline
    PsoKey k;
    BuildPipelineKey(s, a, sh.vh, sh.ph, k);
    // a quad is a picture or a pass over the screen: left out it shows what lies under it, so it waits for its pipeline
    ID3D12PipelineState* pso = EnsurePipeline(k, count <= kQuadVertices);
    if (!pso) return CountDrop(kDropPipeline);
    CountDrop(-1);
    g_gs.draws++;
    cl_->SetPipelineState(pso);

    // constants
    GuestSize size;
    SurfaceSize(bound_rt_ ? s.rt : s.zs, size.w, size.h);
    DrawConstants c{};
    QuadTextureRects(s, a, sh, c);
    if (bound_rt_ && bound_rt_->gw == guest_w_ && bound_rt_->gh == guest_h_) {
        TextureDesc t0;
        bool copy = s.textures[0] && Describe(s.textures[0], t0) && targets_.count(TargetKey(t0.data, false));
        SqueezeCos(s, a, sh, guest_w_, copy, frame_, bound_rt_->scene_frame == presents_, c);
        if (!copy) CentreZoomCopy(s, a, sh, frame_, c);
    }
    // xboxrecomp (hppoa): F10's Bloom, the game's own bloom's strength
    out_scale_ = 1.0f;
    if (GameBloomComposite(s, a, k)) {
        const uint32_t pct = std::min(app::Get().game_bloom, 200u);
        static uint32_t told = ~0u;
        if (told != pct) {
            told = pct;
            fprintf(stderr, "[BLOOM] the game's bloom at %u %%\n", pct);
        }
        if (!pct) return;
        out_scale_ = float(pct) / 100.0f;
    }
    UploadConstants(s, sh, k, stages, size, c);
    out_scale_ = 1.0f;

    if (cap_) {
        CaptureDraw(s, prim, count, k, c, stages);
        if (count > kQuadVertices && c.squeeze[0] != 0)  // xboxrecomp: 16:9, CaptureInputs prints it for quads
            fprintf(cap_, "\n     squeeze %g about %g, y %g about %g", c.squeeze[0], c.squeeze[1], c.squeeze[2], c.squeeze[3]);
        EndDrawCapture(s, a, count, sh.prog, c);
        CaptureShaders(s, a, sh, pk);
    }

    // texture views and samplers, viewport and scissor, blend factor and stencil reference
    SetTextureViewsAndSamplers(s, a, stages);
    SetViewportScissor(s, size.w, c.squeeze);
    SetBlendFactorStencilRef(s);

    // geometry
    DrawGeometry(a);
}

// a guest rect scaled to the target and clamped to it
D3D12_RECT ScaledRect(const Target& t, int x0, int y0, int x1, int y1) {
    float sc = float(t.w) / t.gw;
    return {std::clamp<LONG>(LONG(x0 * sc), 0, t.w), std::clamp<LONG>(LONG(y0 * sc), 0, t.h),
            std::clamp<LONG>(LONG(x1 * sc), 0, t.w), std::clamp<LONG>(LONG(y1 * sc), 0, t.h)};
}

// rs: the rects buffer, reused across targets
void D3D12Backend::ClearTarget(Target& t, const State& s, const ClearArgs& c, std::vector<D3D12_RECT>& rs) {
    rs.clear();
    if (c.count && c.rects) {
        for (uint32_t i = 0; i < c.count; i++) {
            int32_t r[4];
            std::memcpy(r, c.rects + size_t(i) * sizeof(r), sizeof(r));
            rs.push_back(ScaledRect(t, r[0], r[1], r[2], r[3]));
        }
    } else {
        const auto& v = s.viewport;
        rs.push_back(ScaledRect(t, int(v.x), int(v.y), int(v.x + v.w), int(v.y + v.h)));
    }
    if (t.depth) {
        Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        D3D12_CLEAR_FLAGS cf = D3D12_CLEAR_FLAGS(((c.flags & kClearZ) ? D3D12_CLEAR_FLAG_DEPTH : 0) |
                                                 ((c.flags & kClearStencil) ? D3D12_CLEAR_FLAG_STENCIL : 0));
        cl_->ClearDepthStencilView(Cpu(dsv_heap_.Get(), t.view, dsv_inc_), cf, std::clamp(c.z, 0.0f, 1.0f),
                                   UINT8(c.stencil), UINT(rs.size()), rs.data());
    } else {
        // pixels the CPU wrote before this clear go in first, a later upload would paint over the clear
        if (LatchCpuWrite(t)) UploadTarget(t, s.rt);
        Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        float col[4];
        Color(c.color, col);
        for (int i = 0; t.hdr && i < 3; i++) col[i] /= kHeadroom;
        cl_->ClearRenderTargetView(Cpu(rtv_heap_.Get(), t.view, rtv_inc_), col, UINT(rs.size()), rs.data());
    }
    t.drawn = true;
}

void D3D12Backend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                         uint32_t stencil) {
    if (cap_) {
        CapStep(s.rt);
        fprintf(cap_, "%04u clear rt %08X ds %08X flags %X color %08X z %.3f\n", cap_i_++, s.rt.data, s.zs.data, flags,
                color, z);
    }
    // xboxrecomp (hppoa): clearing the scene's depth (before the HUD) ends the scene: occlusion needs that depth
    if ((flags & kClearZ) && s.zs.data == scene_zs_.data && scene_cam_frame_ == presents_) {
        Target* t = FindTarget(scene_rt_, false);
        if (t && t->scene_frame == presents_ && t->bloom_frame != presents_) SceneEnd(*t);
        bound_rt_ = bound_ds_ = nullptr;
    }
    Target* rt = (flags & kClearTarget) ? EnsureTarget(s.rt, false) : nullptr;
    Target* ds = (flags & (kClearZ | kClearStencil)) ? EnsureTarget(s.zs, true) : nullptr;
    uint32_t gw;
    uint32_t gh;
    SurfaceSize(rt ? s.rt : s.zs, gw, gh);
    std::vector<D3D12_RECT> rs;
    ClearArgs c{.count = count, .rects = rects, .flags = flags, .color = color, .z = z, .stencil = stencil};
    for (Target* t : {rt, ds}) {
        if (t) ClearTarget(*t, s, c, rs);
    }
}

// CopyRects' guest lists: src_rects RECTs, dst_points POINTs, all int32
constexpr uint32_t kGuestRectBytes = 16;
constexpr uint32_t kRectLeft = 0;
constexpr uint32_t kRectTop = 4;
constexpr uint32_t kRectRight = 8;
constexpr uint32_t kRectBottom = 12;
constexpr uint32_t kGuestPointBytes = 8;
constexpr uint32_t kPointX = 0;
constexpr uint32_t kPointY = 4;

struct GuestRect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
};

struct GuestPoint {
    int32_t x;
    int32_t y;
};

GuestRect ReadGuestRect(uint32_t rects, uint32_t i) {
    uint32_t r = rects + i * kGuestRectBytes;
    GuestRect g;
    g.left = int32_t(kernel::R32(r + kRectLeft));
    g.top = int32_t(kernel::R32(r + kRectTop));
    g.right = int32_t(kernel::R32(r + kRectRight));
    g.bottom = int32_t(kernel::R32(r + kRectBottom));
    return g;
}

GuestPoint ReadGuestPoint(uint32_t points, uint32_t i) {
    uint32_t p = points + i * kGuestPointBytes;
    GuestPoint g;
    g.x = int32_t(kernel::R32(p + kPointX));
    g.y = int32_t(kernel::R32(p + kPointY));
    return g;
}

// a drawn copy's rect in guest pixels: the source corners and where it lands
struct DrawnRect {
    float x0 = 0;
    float y0 = 0;
    float x1 = 0;
    float y1 = 0;
    float dx = 0;
    float dy = 0;
};

// without dst_points the rect lands where it is
DrawnRect ReadDrawnRect(uint32_t src_rects, uint32_t dst_points, uint32_t i) {
    GuestRect g = ReadGuestRect(src_rects, i);
    DrawnRect r;
    r.x0 = float(g.left);
    r.y0 = float(g.top);
    r.x1 = float(g.right);
    r.y1 = float(g.bottom);
    r.dx = r.x0;
    r.dy = r.y0;
    if (dst_points) {
        GuestPoint p = ReadGuestPoint(dst_points, i);
        r.dx = float(p.x);
        r.dy = float(p.y);
    }
    return r;
}

// a guest coordinate read as unsigned, times the host scale
UINT ScaleUnsigned(int32_t v, float sc) { return UINT(uint32_t(v) * sc); }

// a drawn copy's taps round down a ratio a hair over a whole number
constexpr float kTapsSlack = 1e-3f;

// one Blit per rect: for formats that differ or a multisampled destination
void D3D12Backend::CopyByDraw(const Target& src_t, ID3D12Resource* src_res, D3D12_RESOURCE_STATES& src_state,
                              Target& dst_t, const CopyList& l) {
    Transition(src_res, src_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(dst_t.res.Get(), dst_t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Cpu(rtv_heap_.Get(), dst_t.view, rtv_inc_);
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    float src_scale = float(src_t.w) / src_t.gw;
    float dst_scale = float(dst_t.w) / dst_t.gw;
    bool listed = l.count && l.src_rects;
    uint32_t n = listed ? l.count : 1;
    for (uint32_t i = 0; i < n; i++) {
        float whole_w = float(std::min(src_t.gw, dst_t.gw));
        float whole_h = float(std::min(src_t.gh, dst_t.gh));
        DrawnRect r{.x0 = 0, .y0 = 0, .x1 = whole_w, .y1 = whole_h};
        if (listed) r = ReadDrawnRect(l.src_rects, l.dst_points, i);
        if (r.x1 <= r.x0 || r.y1 <= r.y0) continue;
        D3D12_VIEWPORT vp{r.dx * dst_scale, r.dy * dst_scale, (r.x1 - r.x0) * dst_scale, (r.y1 - r.y0) * dst_scale,
                          0, 1};
        LONG right = std::min(LONG(vp.TopLeftX + vp.Width), LONG(dst_t.w));
        LONG bottom = std::min(LONG(vp.TopLeftY + vp.Height), LONG(dst_t.h));
        D3D12_RECT scissor{LONG(vp.TopLeftX), LONG(vp.TopLeftY), right, bottom};
        if (scissor.right <= scissor.left || scissor.bottom <= scissor.top) continue;
        cl_->RSSetViewports(1, &vp);
        cl_->RSSetScissorRects(1, &scissor);
        float uv[4] = {r.x0 / src_t.gw, r.y0 / src_t.gh, (r.x1 - r.x0) / src_t.gw, (r.y1 - r.y0) / src_t.gh};
        uint32_t taps = uint32_t(std::ceil(std::max(src_scale / dst_scale, 1.0f) - kTapsSlack));
        float mul = src_t.fmt != dst_t.fmt && dst_t.hdr ? 1 / kHeadroom : 1;
        BlitOptions o{.alpha = true, .taps = taps, .mul = mul, .rtv = dst_t.fmt, .samples = dst_t.samples};
        Blit(src_res, src_t.fmt, uv, o);
    }
    dst_t.drawn = true;
    bound_rt_ = bound_ds_ = nullptr;
}

// one CopyTextureRegion per rect, clamped to both targets
void D3D12Backend::CopyByRegion(const Target& src_t, ID3D12Resource* src_res, D3D12_RESOURCE_STATES& src_state,
                                Target& dst_t, const CopyList& l) {
    Transition(src_res, src_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(dst_t.res.Get(), dst_t.state, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dl{dst_t.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION sl{src_res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    float dst_scale = float(dst_t.w) / dst_t.gw;
    bool listed = l.count && l.src_rects;
    uint32_t n = listed ? l.count : 1;
    for (uint32_t i = 0; i < n; i++) {
        D3D12_BOX b{0, 0, 0, std::min(src_t.w, dst_t.w), std::min(src_t.h, dst_t.h), 1};
        uint32_t dx = 0;
        uint32_t dy = 0;
        if (listed) {
            GuestRect r = ReadGuestRect(l.src_rects, i);
            b.left = ScaleUnsigned(r.left, dst_scale);
            b.top = ScaleUnsigned(r.top, dst_scale);
            b.right = std::min(ScaleUnsigned(r.right, dst_scale), src_t.w);
            b.bottom = std::min(ScaleUnsigned(r.bottom, dst_scale), src_t.h);
            dx = b.left;
            dy = b.top;
            if (l.dst_points) {
                GuestPoint p = ReadGuestPoint(l.dst_points, i);
                dx = ScaleUnsigned(p.x, dst_scale);
                dy = ScaleUnsigned(p.y, dst_scale);
            }
        }
        if (b.right <= b.left || b.bottom <= b.top || dx >= dst_t.w || dy >= dst_t.h) continue;
        b.right = std::min<UINT>(b.right, b.left + dst_t.w - dx);
        b.bottom = std::min<UINT>(b.bottom, b.top + dst_t.h - dy);
        cl_->CopyTextureRegion(&dl, dx, dy, 0, &sl, &b);
        g_gs.region_copies++;
    }
    dst_t.drawn = true;
    bound_rt_ = bound_ds_ = nullptr;
}

void D3D12Backend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects,
                        uint32_t dst_points) {
    Target* src_t = FindTarget(src, false);
    if (cap_) {
        const char* from = "memory";
        if (src_t) from = src_t->drawn ? "target" : "idle target";
        fprintf(cap_, "%04u copy %08X %s to %08X\n", cap_i_++, src.data, from, dst.data);
    }
    // pixels the CPU made: the copy happens in memory, textures and targets pick it up from there
    if (!src_t || !src_t->drawn) {
        if (!CopySurfaceMemory(src, dst, count, src_rects, dst_points)) {
            LOG_FIRST(8, "d3d12: CopyRects %08X to %08X skipped", src.data, dst.data);
        } else if (Target* dst_t = FindTarget(dst, false)) {
            UploadTarget(*dst_t, dst);
        }
        return;
    }
    if (src_t->hdr) Resolve(*src_t);
    Target* dst_t = EnsureTarget(dst, false);
    if (!dst_t || src_t == dst_t) return;
    Readable src_r = ReadableResource(*src_t);
    if (!src_r.res) return;
    CopyList l{.count = count, .src_rects = src_rects, .dst_points = dst_points};
    if (src_t->fmt != dst_t->fmt || dst_t->samples > 1) {
        // formats differ, or the samples of a multisampled target have to be written: drawn across
        CopyByDraw(*src_t, src_r.res, *src_r.state, *dst_t, l);
        return;
    }
    CopyByRegion(*src_t, src_r.res, *src_r.state, *dst_t, l);
    if (cap_) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_copy_%08X", cap_i_, dst_t->data);
        CapSave(*dst_t, name);
    }
}

// the output the window's display has: a factory made before Windows HDR was switched no longer tells
void D3D12Backend::DetectHdr() {
    HMONITOR mon = MonitorFromWindow(static_cast<HWND>(app::Window()), MONITOR_DEFAULTTONEAREST);
    if (mon == hdr_mon_ && factory_->IsCurrent()) return;
    hdr_mon_ = mon;
    ComPtr<IDXGIFactory4> fresh;
    if (!factory_->IsCurrent() && SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&fresh)))) factory_ = fresh;
    DXGI_OUTPUT_DESC1 desc{};
    bool found = FindOutput(factory_.Get(), mon, desc);
    hdr_display_ = found && desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    peak_nits_ = found && desc.MaxLuminance > 0 ? desc.MaxLuminance : kDefaultPeakNits;
    paper_nits_ = SdrWhiteNits(mon);
    app::g_hdr_display = hdr_display_;
    LOGR("Display HDR: %s, peak %.0f nits, SDR white %.0f nits", hdr_display_ ? "on" : "off", peak_nits_,
         paper_nits_);
}

void D3D12Backend::ResizeSwap() {
    HWND hwnd = static_cast<HWND>(app::Window());
    RECT rc;
    GetClientRect(hwnd, &rc);
    uint32_t w = std::max<LONG>(rc.right - rc.left, 1);
    uint32_t h = std::max<LONG>(rc.bottom - rc.top, 1);
    // xboxrecomp: HDR10 output removed (2026-10-24, as HP:CoS): always SDR, also on a display with Windows HDR on
    bool hdr = false;
    if (w == bb_w_ && h == bb_h_ && hdr == hdr_want_) return;
    Flush();
    for (auto& f : frames_) {
        if (f.fence) WaitFence(f.fence);
    }
    for (auto& b : bb_) b.Reset();
    hdr_want_ = hdr;
    hdr_out_ = hdr && ResizeBackBuffers(w, h, kHdr10Format, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
    if (!hdr_out_) ResizeBackBuffers(w, h, kBgra, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    if (hdr && !hdr_out_) LOGR("Display HDR: the swap chain can't show HDR10, SDR it is");
    CreateBackBufferViews();
    bb_w_ = w;
    bb_h_ = h;
}

// false when the swap chain can't present in that colour space
bool D3D12Backend::ResizeBackBuffers(uint32_t w, uint32_t h, DXGI_FORMAT fmt, DXGI_COLOR_SPACE_TYPE space) {
    UINT flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    if (FAILED(swap_->ResizeBuffers(kFrames, w, h, fmt, flags))) return false;
    UINT support = 0;
    swap_->CheckColorSpaceSupport(space, &support);
    if (!(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) return false;
    return SUCCEEDED(swap_->SetColorSpace1(space));
}

void D3D12Backend::FinishCapture(const Surface& front) {
    CapStep(Surface{});
    if (Target* f = FindTarget(front, false)) CapSave(*f, "final");
    for (uint32_t data : cap_sampled_) {
        auto it = targets_.find(TargetKey(data, false));
        if (it == targets_.end()) continue;
        char name[32];
        snprintf(name, sizeof(name), "sampled_%08X", data);
        CapSave(it->second, name);
    }
    cap_sampled_.clear();
    for (auto& t : cap_tex_) {
        char name[32];
        snprintf(name, sizeof(name), "tex_%08X", t.data);
        D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        CapSave({.res = t.res.Get(), .state = &st, .fmt = t.fmt, .w = t.w, .h = t.h}, name);
    }
    fclose(cap_);
    cap_ = nullptr;
    cap_tex_.clear();
    LOGI("d3d12: frame captured to %s", cap_dir_.string().c_str());
}

void D3D12Backend::StartCapture() {
    SYSTEMTIME tm;
    GetLocalTime(&tm);
    char stamp[32];
    snprintf(stamp, sizeof(stamp), "%02u%02u%02u", tm.wHour, tm.wMinute, tm.wSecond);
    std::error_code ec;
    cap_dir_ = app::ExeDir() / "captures" / stamp;
    // xboxrecomp: several captures in one second (automatic ones) each get their own folder
    for (int n = 2; std::filesystem::exists(cap_dir_, ec) && n < 100; n++)
        cap_dir_ = app::ExeDir() / "captures" / (std::string(stamp) + "_" + std::to_string(n));
    std::filesystem::create_directories(cap_dir_, ec);
    if (burst_left_ > 0) {  // xboxrecomp: a frame of a burst goes in the report's folder
        char sub[16];
        snprintf(sub, sizeof(sub), "frame_%02d", burst_i_++);
        cap_dir_ = burst_dir_ / sub;
        burst_left_--;
    }
    std::filesystem::create_directories(cap_dir_, ec);
    cap_ = _wfopen((cap_dir_ / "frame.txt").c_str(), L"w");
    cap_i_ = cap_rt_ = 0;
    cap_rs_.fill(0);
    cap_simple_.fill(0);
    for (auto& t : cap_tss_) t.fill(0);
    cap_mat_.fill(0);
    for (auto& l : cap_lights_) l.fill(0);
    cap_light_enable_ = 0;
    cap_draws_ = GetEnvironmentVariableW(L"RECOMP_CAP_DRAWS", nullptr, 0) != 0;
    cap_seen_.clear();
    cap_sh_.clear();
}

// Brightness and contrast range from 0 to 100, 50/50 leaves the image unchanged.
// Unreal's PC ramp offsets brightness and scales contrast linearly, graying blacks
// and clipping both ends. Here, black and white stay fixed: contrast uses an S-curve
// around mid-gray (slope 0.5–2), while brightness adjusts gamma from 2 to 0.5.
// Both match Unreal's ramp at quarter gray.
// xboxrecomp: the curve applies after the title's own ramp, which HP:CoS sets (SCDA replaced its game's)
void D3D12Backend::UploadGammaRamp(uint32_t brightness, uint32_t contrast) {
    constexpr uint32_t kRampBytes = 256 * 4;  // one BGRA texel per ramp entry
    ramp_set_ = brightness << 8 | contrast;
    ramp_dirty_ = false;
    float k = std::exp2((float(contrast) - 50.0f) / 50.0f);
    float g = std::exp2((50.0f - float(brightness)) / 50.0f);
    Alloc a = Upload(kRampBytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    bool identity = true;
    auto curve = [&](uint8_t in) {
        float x = in / 255.0f;
        float y = x < 0.5f ? 0.5f * std::pow(2.0f * x, k) : 1.0f - 0.5f * std::pow(2.0f - 2.0f * x, k);
        return uint8_t(std::lround(std::clamp(std::pow(y, g), 0.0f, 1.0f) * 255.0f));
    };
    for (int i = 0; i < 256; i++) {
        uint8_t* p = a.cpu + i * 4;
        p[0] = curve(ramp_[512 + i]);
        p[1] = curve(ramp_[256 + i]);
        p[2] = curve(ramp_[i]);
        p[3] = 255;
        identity &= p[0] == i && p[1] == i && p[2] == i;
    }
    gamma_ = !identity;
    Transition(lut_.Get(), lut_state_, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dl{lut_.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION sl{a.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    sl.PlacedFootprint.Offset = a.off;
    sl.PlacedFootprint.Footprint = {DXGI_FORMAT_B8G8R8A8_UNORM, 256, 1, 1, kRampBytes};
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
}

// what the frame shows: the target made readable, SMAA's blend of it when that is on
ID3D12Resource* D3D12Backend::PrepareShownFrame(Target* t) {
    Readable shown = t ? ReadableResource(*t) : Readable{nullptr, nullptr};
    ID3D12Resource* r = shown.res;
    if (r) {
        Transition(r, *shown.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (app::Get().smaa) {
            if (ID3D12Resource* aa = Smaa(r, t->fmt, t->w, t->h)) r = aa;
        }
    }
    return r;
}

void D3D12Backend::BlitFrame(const Target& t, ID3D12Resource* r, bool pillarbox, DXGI_FORMAT rtv) {
    float aspect = app::Aspect();
    float w = float(bb_w_);
    float h = float(bb_h_);
    float vw = std::min(w, h * aspect);
    float vh = vw / aspect;
    if (pillarbox) vw = vh * 4.0f / 3.0f;
    float uv[4] = {0, 0, 1, 1};
    // xboxrecomp: 16:9, the frame's part Present picked (a zoomed cutscene, or the viewport without its overscan)
    if (shown_crop_[2] > 0 && !pillarbox && app::BootAspect() == app::AspectMode::Wide && guest_w_ && guest_h_) {
        uv[0] = shown_crop_[0] / float(guest_w_);
        uv[1] = shown_crop_[1] / float(guest_h_);
        uv[2] = shown_crop_[2] / float(guest_w_);
        uv[3] = shown_crop_[3] / float(guest_h_);
    }
    D3D12_VIEWPORT vp{(w - vw) / 2, (h - vh) / 2, vw, vh, 0, 1};
    D3D12_RECT scissor{0, 0, LONG(bb_w_), LONG(bb_h_)};
    cl_->RSSetViewports(1, &vp);
    cl_->RSSetScissorRects(1, &scissor);
    // xboxrecomp: the colour-blind filter is in the gamma pass, which a neutral ramp (CoS's own at default brightness)
    // would skip; the lookup is identity then, so only the filter changes the picture
    // (and the sharpening and grading, xboxrecomp)
    const app::Settings& set = app::Get();
    // xboxrecomp (hppoa): black while the camera returns after a cutscene, then faded in (hle_widescreen.c)
    if (r) Blit(r, t.fmt, uv, {.gamma = gamma_ || set.colorblind != 0 || set.sharpen != 0 || GradingOn(set) ||
                                          set.vignette != 0 || set.grain != 0, .mul = 1.0f, .rtv = rtv});
}

// the overlay as the window holds it: a new frame goes up to ov_tex_, under the window's overlay lock
void D3D12Backend::OverlayLatch(const XboxPresentInfo& pi) {
    ov_shown_ = pi.ov_on && pi.ov && pi.ov_w && pi.ov_h;
    if (!ov_shown_) return;
    ov_fb_w_ = pi.fb_w ? pi.fb_w : 640;
    ov_fb_h_ = pi.fb_h ? pi.fb_h : 480;
    ov_rect_[0] = float(pi.ov_x);
    ov_rect_[1] = float(pi.ov_y);
    ov_rect_[2] = float(pi.ov_dw);
    ov_rect_[3] = float(pi.ov_dh);
    if (!ov_tex_ || ov_w_ != pi.ov_w || ov_h_ != pi.ov_h) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td = Texture2DDesc(kBgra, pi.ov_w, pi.ov_h);
        if (ov_tex_) CurrentFrame().garbage.push_back(ov_tex_);
        ov_tex_ = nullptr;
        ov_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, ov_state_, nullptr,
                                                 IID_PPV_ARGS(&ov_tex_)))) {
            ov_shown_ = false;
            return;
        }
        ov_w_ = pi.ov_w;
        ov_h_ = pi.ov_h;
        ov_version_ = pi.ov_version - 1;
    }
    if (pi.ov_version == ov_version_) return;
    ov_version_ = pi.ov_version;
    uint32_t pitch = (ov_w_ * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    Alloc a = Upload(uint64_t(pitch) * ov_h_, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    for (uint32_t y = 0; y < ov_h_; y++) {
        uint8_t* row = a.cpu + size_t(y) * pitch;
        std::memcpy(row, pi.ov + size_t(y) * ov_w_, ov_w_ * 4);
        for (uint32_t x = 0; x < ov_w_; x++) row[x * 4 + 3] = 0xFF;
    }
    Transition(ov_tex_.Get(), ov_state_, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dl{ov_tex_.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION sl{a.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    sl.PlacedFootprint.Offset = a.off;
    sl.PlacedFootprint.Footprint = {kBgra, ov_w_, ov_h_, 1, pitch};
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
}

// xboxrecomp: the movie overlay scaled into the 4:3 picture, then SCDA's options panel
void D3D12Backend::RenderUi() {
    RenderOverlay();
    RenderPanel();
}

void D3D12Backend::RenderOverlay() {
    xbox_FramebufferWindowOverlayRead(
        [](const XboxPresentInfo* pi, void* self) { static_cast<D3D12Backend*>(self)->OverlayLatch(*pi); }, this);
    if (!ov_shown_ || !ov_tex_) return;
    Transition(ov_tex_.Get(), ov_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    float w = float(bb_w_), h = float(bb_h_);
    float vh = std::min(h, w * 3.0f / 4.0f), vw = vh * 4.0f / 3.0f;
    float x0 = (w - vw) / 2, y0 = (h - vh) / 2;
    float sx = vw / float(ov_fb_w_), sy = vh / float(ov_fb_h_);
    D3D12_VIEWPORT vp{x0 + ov_rect_[0] * sx, y0 + ov_rect_[1] * sy, ov_rect_[2] * sx, ov_rect_[3] * sy, 0, 1};
    D3D12_RECT scissor{0, 0, LONG(bb_w_), LONG(bb_h_)};
    cl_->RSSetViewports(1, &vp);
    cl_->RSSetScissorRects(1, &scissor);
    float uv[4] = {0, 0, 1, 1};
    Blit(ov_tex_.Get(), kBgra, uv, {});
}

// the options panel and fps counter, over everything
void D3D12Backend::RenderPanel() {
    std::lock_guard lock(app::UiLock());
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    app::DrawUi();
    ImGui::Render();
    ID3D12DescriptorHeap* uh[] = {ui_heap_.Get()};
    cl_->SetDescriptorHeaps(1, uh);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cl_.Get());
}

void D3D12Backend::DrawFrame(Target* t, bool pillarbox, uint32_t bi) {
    D3D12_RESOURCE_STATES bs = D3D12_RESOURCE_STATE_PRESENT;
    ID3D12Resource* r = PrepareShownFrame(t);
    Transition(bb_[bi].Get(), bs, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Cpu(rtv_heap_.Get(), kRtvCount + bi, rtv_inc_);
    // without its targets an HDR10 back buffer stays black: the SDR pipelines don't match its format
    if (!hdr_out_) DrawSdrFrame(t, r, pillarbox, rh);
    else if (EnsureOutTargets()) DrawHdrFrame(t, r, pillarbox, rh);
    Transition(bb_[bi].Get(), bs, D3D12_RESOURCE_STATE_PRESENT);
}

void D3D12Backend::DrawSdrFrame(Target* t, ID3D12Resource* r, bool pillarbox, D3D12_CPU_DESCRIPTOR_HANDLE bb) {
    float black[4] = {0, 0, 0, 1};
    cl_->ClearRenderTargetView(bb, black, 0, nullptr);
    cl_->OMSetRenderTargets(1, &bb, FALSE, nullptr);
    bound_rt_ = bound_ds_ = nullptr;
    if (t) BlitFrame(*t, r, pillarbox);
    RenderUi();
}

bool D3D12Backend::EnsureOutTargets() {
    if (out_[0] && out_[1] && out_w_ == bb_w_ && out_h_ == bb_h_) return true;
    out_w_ = bb_w_;
    out_h_ = bb_h_;
    return CreatePostTarget(out_[0], out_state_[0], kHdrFormat, bb_w_, bb_h_, kOutRtv) &&
           CreatePostTarget(out_[1], out_state_[1], kBgra, bb_w_, bb_h_, kOutRtv + 1);
}

// the frame and the panel each into a target of their own, then both into the back buffer as HDR10; the panel
// stays at SDR white
void D3D12Backend::DrawHdrFrame(Target* t, ID3D12Resource* r, bool pillarbox, D3D12_CPU_DESCRIPTOR_HANDLE bb) {
    D3D12_CPU_DESCRIPTOR_HANDLE frame = PostRtv(kOutRtv);
    D3D12_CPU_DESCRIPTOR_HANDLE panel = PostRtv(kOutRtv + 1);
    float clear[4] = {0, 0, 0, 0};
    for (int i = 0; i < 2; i++) Transition(out_[i].Get(), out_state_[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
    cl_->ClearRenderTargetView(frame, clear, 0, nullptr);
    cl_->ClearRenderTargetView(panel, clear, 0, nullptr);
    cl_->OMSetRenderTargets(1, &frame, FALSE, nullptr);
    bound_rt_ = bound_ds_ = nullptr;
    if (t) BlitFrame(*t, r, pillarbox, kHdrFormat);
    cl_->OMSetRenderTargets(1, &panel, FALSE, nullptr);
    RenderUi();
    Begin();  // RenderUi left ImGui's heap and root signature bound
    for (int i = 0; i < 2; i++) Transition(out_[i].Get(), out_state_[i], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    PostInputs in{{out_[0].Get(), out_[1].Get(), nullptr}, {kHdrFormat, kBgra, kBgra}};
    float p[8] = {paper_nits_, peak_nits_, 0, 0};
    DrawPostPass("hdr10", {bb, kHdr10Format, bb_w_, bb_h_}, in, p, {.hlsl = kHdrHlsl});
}

void D3D12Backend::ReleaseOldTextures() {
    constexpr uint64_t kSweepEvery = 60;    // presents between sweeps
    constexpr size_t kTexturesKept = 2048;  // no sweep below this many
    constexpr uint64_t kIdleFrames = 600;
    // textures unused for ten seconds go, levels stream theirs in and out
    if (presents_ % kSweepEvery != 0 || textures_.size() <= kTexturesKept) return;
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (it->second.frame + kIdleFrames < presents_) {
            CurrentFrame().garbage.push_back(it->second.res);
            it = textures_.erase(it);
        } else {
            ++it;
        }
    }
}

void D3D12Backend::ResetFrame() {
    Frame& f = CurrentFrame();
    if (f.fence && fence_->GetCompletedValue() < f.fence) WaitFence(f.fence);
    f.alloc->Reset();
    f.garbage.clear();
    f.used = 0;
    f.srv = 0;
    cl_->Reset(f.alloc.Get(), nullptr);
    Begin();
}

void D3D12Backend::CountFps() {
    fps_n_++;
    auto now = std::chrono::steady_clock::now();
    double el = std::chrono::duration<double>(now - fps_t_).count();
    if (el >= 0.5) {
        app::g_fps = float(fps_n_ / el);
        fps_n_ = 0;
        fps_t_ = now;
    }
}

void D3D12Backend::Present(const Surface& front, bool pillarbox) {
    if (!prebuild_started_) {  // xboxrecomp: the settings are read by now (the sample count)
        prebuild_started_ = true;
        StartPrebuild();
    }
    static const bool dbg_keys_env = GetEnvironmentVariableW(L"RECOMP_DEBUG_KEYS", nullptr, 0) != 0;
    const bool dbg_keys = dbg_keys_env || app::Get().debug_keys;  // xboxrecomp: or F10's Debug tab
    if (dbg_keys && (GetAsyncKeyState(VK_F8) & 1)) {
        g_dbg_logall = g_dbg_logall == 1 ? 0 : 1;
        LOGI("debug: log depth for all draws %s", g_dbg_logall ? "on" : "off");
    }
    if (dbg_keys && (GetAsyncKeyState(VK_F9) & 1)) {
        g_dbg_noatest = !g_dbg_noatest;
        LOGI("debug: alpha test %s", g_dbg_noatest ? "off" : "on");
    }
    if (dbg_keys && (GetAsyncKeyState(VK_F7) & 1)) {  // F10 is the options panel
        g_dbg_zalways = !g_dbg_zalways;
        LOGI("debug: depth test %s", g_dbg_zalways ? "always passes" : "on");
    }
    if (cap_) {
        FinishCapture(front);
        // xboxrecomp: a burst's frames back to back; the last one tells the report it is done
        if (burst_left_ > 0) StartCapture();
        else if (burst_i_) burst_i_ = 0, g_capture_bursts++;
    } else if (TakeBurst(burst_dir_, burst_left_)) {
        burst_i_ = 0;
        StartCapture();
#ifdef HPPOA_RELEASE
    } else if (g_capture.exchange(false)) {  // xboxrecomp: no F12 in a release build; the Debug tab's button still captures
#else
    } else if ((GetAsyncKeyState(VK_F12) & 1) || g_capture.exchange(false)) {  // xboxrecomp: F12 captures a frame
#endif
        StartCapture();
    }
    DetectHdr();
    ResizeSwap();
    Target* t = EnsureTarget(front, false);
    if (t && LatchCpuWrite(*t)) UploadTarget(*t, front);
    if (t && t->hdr) Resolve(*t);
    // xboxrecomp: a frame with a scene and no HUD after it is bloomed here
    if (t && t->scene_frame == presents_ && t->bloom_frame != presents_) SceneEnd(*t);
    if (const app::Settings& s = app::Get(); ramp_dirty_ || (s.brightness << 8 | s.contrast) != ramp_set_)
        UploadGammaRamp(s.brightness, s.contrast);
    Transition(lut_.Get(), lut_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // a frame missing draws still on the build threads is not shown, the last whole one stays up: otherwise a new
    // effect's intermediate passes flash on screen
    auto now_t = std::chrono::steady_clock::now();
    bool hold = skipped_frame_ && shown_ && now_t - shown_t_ < kHoldMax;
    skipped_frame_ = false;
    // xboxrecomp: 16:9, the finished frame's kind goes with it to the screen and into the next one's Squeeze2D
    {
        // bars growing or still: a cutscene; shrinking: it is over (and stays so until they are gone)
        bool out = wf_.bars && (wf_.bars_out || wf_.bars_h < wf_.bars_h_prev - 0.01f);
        WideFrame next{.cut = wf_.bars && !out, .bars_h_prev = wf_.bars_h, .bars_out = out, .view_y = wf_.vp_y,
                       .view_h = wf_.vp_h};
        float gh = float(guest_h_);
        float cr[4] = {};
        if (gh > 0 && (wf_.cut || wf_.vp_h > 0))
            WideCrop(float(guest_w_), gh, wf_.vp_h > 0 ? wf_.vp_y : 0, wf_.vp_h > 0 ? wf_.vp_h : gh, wf_.cut, cr);
        std::memcpy(shown_crop_, cr, sizeof(shown_crop_));
        wf_ = next;
    }
    uint32_t bi = swap_->GetCurrentBackBufferIndex();
    if (!hold) DrawFrame(t, pillarbox, bi);
    GpuStatFrame();
    Submit(false);
    if (!hold) {
        bool vsync = app::Get().vsync;
        swap_->Present(vsync ? 1 : 0, !vsync && tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
        shown_ = true;
        shown_t_ = now_t;
    }

    presents_++;
    presents_seen_.store(presents_);
    frame_++;
    ReleaseOldTextures();
    ResetFrame();
    CountFps();
    ApplySettings();
}

void D3D12Backend::ApplySettings() {
    app::Poll();
    float want = app::RenderScale(guest_h_);
    const app::Settings& set = app::Get();
    uint32_t shadows = set.shadow_resolution;
    sm3_ = set.shader_model >= 3;
    LatchFeatureSettings(set);
    cache::SaveWhenIdle();
    bool hdr = sm3_ && set.hdr_rendering;
    uint32_t msaa = msaa_;
    if (set.msaa != msaa_set_) {
        msaa_set_ = set.msaa;
        msaa = Samples(msaa_set_);
    }
    if (std::fabs(want - scale_) > 1e-4f || shadows != shadow_res_ || hdr != hdr_ || msaa != msaa_) {
        hdr_ = hdr;
        msaa_ = msaa;
        expo_fresh_ = true;
        Flush();
        for (auto& [key, tg] : targets_) (tg.depth ? free_dsv_ : free_rtv_).push_back(tg.view);
        targets_.clear();
        scale_ = want;
        shadow_res_ = shadows;
        bound_rt_ = bound_ds_ = nullptr;
    }
}

// the target resolved and made readable, then shrunk with o when it is not guest sized BGRA; res null on failure
D3D12Backend::CopySource D3D12Backend::PrepareCopySource(Target& t, const ShrinkOptions& o) {
    if (t.hdr) Resolve(t);
    Readable r = ReadableResource(t);
    CopySource out{r.res, nullptr};
    if (!r.res) return out;
    if (t.w != t.gw || t.h != t.gh || t.fmt != kBgra) {
        out.down = Shrink(r.res, *r.state, t.fmt, t.gw, t.gh, o);
        out.res = out.down.Get();
    } else {
        Transition(r.res, *r.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    return out;
}

// the CPU reads a surface the GPU drew: bring the pixels back
void D3D12Backend::Sync(uint32_t data, uint32_t) {
    auto it = targets_.find(TargetKey(data, false));
    if (it == targets_.end() || !it->second.drawn) return;
    g_gs.readbacks++;
    Target& t = it->second;
    // src.down, when there is one, stays alive until ReadBack is done
    CopySource src = PrepareCopySource(t, {});
    if (!src.res) return;
    std::vector<uint8_t> px;
    if (!ReadBack(src.res, t.gw, t.gh, px)) return;
    // color targets are 32 bit linear in guest memory, the pitch rounds to 64 bytes
    uint32_t pitch = GuestPitch(t.gw);
    uint8_t* guest = static_cast<uint8_t*>(kernel::HostPtr(data));
    for (uint32_t y = 0; y < t.gh; y++) std::memcpy(guest + size_t(y) * pitch, &px[size_t(y) * t.gw * 4], t.gw * 4);
    t.read = std::move(px);
    t.drawn = false;
    t.sig = MemSig(t.data, t.bytes);
}

// A NOOVERWRITE lock: the console reads whatever the GPU has finished, so a surface locked every frame gets the newest
// finished copy (a frame or two old) and the CPU never waits on the GPU; one locked now and then still waits in Sync
void D3D12Backend::SyncLate(uint32_t data) {
    if (!DeliverLate(data)) Sync(data, 0);
    RecordLate(data);
}

bool D3D12Backend::DeliverLate(uint32_t data) {
    std::lock_guard lock(late_lock_);
    auto it = late_.find(data);
    if (it == late_.end()) return false;
    Late& l = it->second;
    uint64_t done = fence_->GetCompletedValue();
    uint64_t now = presents_seen_.load();
    int best = -1;
    for (int i = 0; i < kFrames; i++) {
        bool finished = l.fence[i] && l.fence[i] <= done;
        bool recent = now - l.frame[i] <= uint64_t(kFrames) + 1;
        bool newest = best < 0 || l.fence[i] > l.fence[best];
        if (finished && recent && newest) best = i;
    }
    if (best < 0) {
        return false;
    }
    if (l.fence[best] > l.delivered) {
        uint8_t* p = nullptr;
        // the last row is not padded to the pitch: a range to the end of the pitch fails to map
        D3D12_RANGE rr{0, size_t(l.row_pitch) * (l.gh - 1) + l.gw * 4};
        if (FAILED(l.rb[best]->Map(0, &rr, reinterpret_cast<void**>(&p)))) return false;
        uint32_t pitch = GuestPitch(l.gw);
        uint8_t* dst = static_cast<uint8_t*>(kernel::HostPtr(data));
        for (uint32_t y = 0; y < l.gh; y++) std::memcpy(dst + size_t(y) * pitch, p + size_t(y) * l.row_pitch, l.gw * 4);
        D3D12_RANGE none{0, 0};
        l.rb[best]->Unmap(0, &none);
        l.delivered = l.fence[best];
    }
    return true;
}

// a slot set for another size: its buffers go with the frame
void D3D12Backend::ResetLate(Late& l, const Target& t) {
    auto& garbage = CurrentFrame().garbage;
    for (auto& r : l.rb) {
        if (r) garbage.push_back(r);
    }
    if (l.down) garbage.push_back(l.down);
    l = Late{};
    l.gw = t.gw;
    l.gh = t.gh;
}

// the target as a guest sized BGRA copy source, shrunk into l.down when it is not one already
ID3D12Resource* D3D12Backend::PrepareLateSource(Target& t, Late& l) {
    ShrinkOptions reuse{.into = l.down.Get(), .into_state = l.down_state};
    CopySource src = PrepareCopySource(t, reuse);
    if (src.down) {
        l.down = src.down;
        l.down_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }
    return src.res;
}

// src into the readback buffer of the slot, made on first use
void D3D12Backend::RecordLateCopy(Late& l, uint32_t slot, ID3D12Resource* src) {
    D3D12_RESOURCE_DESC rd = src->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 row_size;
    UINT64 bytes;
    dev_->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &row_size, &bytes);
    if (!l.rb[slot]) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC bd = BufferDesc(bytes);
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&l.rb[slot])))) {
            return;
        }
    }
    D3D12_TEXTURE_COPY_LOCATION dl{l.rb[slot].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dl.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION sl{src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    l.row_pitch = fp.Footprint.RowPitch;
    l.fence[slot] = fence_value_ + 1;  // the next Submit carries it
    l.frame[slot] = presents_;
    l.next = (slot + 1) % kFrames;
}

void D3D12Backend::RecordLate(uint32_t data) {
    auto it = targets_.find(TargetKey(data, false));
    if (it == targets_.end()) return;
    Target& t = it->second;
    // what DeliverLate or Sync left in guest memory is no CPU write to upload back
    t.sig = MemSig(t.data, t.bytes);
    t.read.clear();
    std::lock_guard lock(late_lock_);
    Late& l = late_[data];
    if (l.gw != t.gw || l.gh != t.gh) ResetLate(l, t);
    // a copy for a later lock, unless the GPU is a whole ring behind
    uint32_t slot = l.next;
    if (l.fence[slot] > fence_->GetCompletedValue()) return;
    ID3D12Resource* src = PrepareLateSource(t, l);
    if (!src) return;
    RecordLateCopy(l, slot, src);
}

// filters src into a new w x h target, left as a copy source
ComPtr<ID3D12Resource> D3D12Backend::Shrink(ID3D12Resource* src, D3D12_RESOURCE_STATES& st, DXGI_FORMAT fmt,
                                            uint32_t w, uint32_t h, const ShrinkOptions& o) {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_B8G8R8A8_UNORM, w, h);
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv{};
    cv.Format = td.Format;
    ComPtr<ID3D12Resource> down = o.into;
    D3D12_RESOURCE_STATES down_state = o.into ? o.into_state : D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (free_rtv_.empty()) return nullptr;
    if (down) {
        Transition(down.Get(), down_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    } else if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                                    D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&down)))) {
        return nullptr;
    }
    // borrows the next free view without taking it, only for this recording
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Cpu(rtv_heap_.Get(), free_rtv_.back(), rtv_inc_);
    dev_->CreateRenderTargetView(down.Get(), nullptr, rh);
    Transition(src, st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    SetBlitTarget(rh, w, h);
    float uv[4] = {0, 0, 1, 1};
    Blit(src, fmt, uv, {.alpha = true, .taps = o.taps, .mul = o.mul});
    Transition(down.Get(), down_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    return down;
}

// a BGRA copy source to the CPU, rows packed; waits for the GPU
bool D3D12Backend::ReadBack(ID3D12Resource* res, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    D3D12_RESOURCE_DESC rd = res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 row_size;
    UINT64 bytes;
    dev_->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &row_size, &bytes);
    ComPtr<ID3D12Resource> rb;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd = BufferDesc(bytes);
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&rb)))) {
        return false;
    }
    D3D12_TEXTURE_COPY_LOCATION dl{rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dl.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION sl{res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    Flush();
    uint8_t* p = nullptr;
    rb->Map(0, nullptr, reinterpret_cast<void**>(&p));
    out.resize(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; y++) std::memcpy(&out[size_t(y) * w * 4], p + size_t(y) * fp.Footprint.RowPitch, w * 4);
    rb->Unmap(0, nullptr);
    return true;
}

// BMP file and info header: byte offsets of the fields CapSave writes
constexpr int kBmpHeaderBytes = 54;
constexpr int kBmpFileSize = 2;
constexpr int kBmpPixelOffset = 10;
constexpr int kBmpInfoSize = 14;
constexpr int kBmpWidth = 18;
constexpr int kBmpHeight = 22;  // negative: rows top down
constexpr int kBmpPlanes = 26;
constexpr int kBmpBitCount = 28;
constexpr int kBmpImageSize = 34;
constexpr uint32_t kBmpInfoBytes = 40;

void D3D12Backend::CapSave(const CapImage& img, const char* name) {
    ComPtr<ID3D12Resource> down = Shrink(img.res, *img.state, img.fmt, img.w, img.h, {.mul = img.mul});
    std::vector<uint8_t> px;
    if (!down || !ReadBack(down.Get(), img.w, img.h, px)) return;
    FILE* f = _wfopen((cap_dir_ / (std::string(name) + ".bmp")).c_str(), L"wb");
    if (!f) return;
    uint8_t hd[kBmpHeaderBytes] = {'B', 'M'};
    auto put = [&](int o, uint32_t v) { std::memcpy(hd + o, &v, 4); };
    put(kBmpFileSize, kBmpHeaderBytes + uint32_t(px.size()));
    put(kBmpPixelOffset, kBmpHeaderBytes);
    put(kBmpInfoSize, kBmpInfoBytes);
    put(kBmpWidth, img.w);
    put(kBmpHeight, uint32_t(-int32_t(img.h)));
    hd[kBmpPlanes] = 1;
    hd[kBmpBitCount] = 32;
    put(kBmpImageSize, uint32_t(px.size()));
    fwrite(hd, 1, kBmpHeaderBytes, f);
    fwrite(px.data(), 1, px.size(), f);
    fclose(f);
}

void D3D12Backend::CapSave(Target& t, const char* name) {
    Readable r = ReadableResource(t);
    if (!r.res) return;
    DXGI_FORMAT fmt = t.depth ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : t.fmt;
    float mul = t.hdr ? kHeadroom : 1;
    CapSave({.res = r.res, .state = r.state, .fmt = fmt, .w = t.gw, .h = t.gh, .mul = mul}, name);
}

// the frame moves to another target: save the one it leaves
void D3D12Backend::CapStep(const Surface& rt) {
    uint32_t d = rt.header ? mem::VirtualFromPhysical(rt.data) : 0;
    bool same = d == cap_rt_;
    if (same && !(cap_draws_ && d)) return;
    auto it = targets_.find(TargetKey(cap_rt_, false));
    if (cap_rt_ && it != targets_.end()) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_%08X", cap_i_, cap_rt_);
        CapSave(it->second, name);
    }
    auto z = targets_.find(TargetKey(cap_rt_, true));
    if (!same && cap_rt_ && z != targets_.end()) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_%08X_z", cap_i_, cap_rt_);
        CapSave(z->second, name);
    }
    cap_rt_ = d;
}

void D3D12Backend::SetGamma(const uint8_t* ramp) {
    std::memcpy(ramp_, ramp, sizeof(ramp_));
    ramp_dirty_ = true;
}

}  // namespace

std::unique_ptr<Backend> CreateBackend() { return std::make_unique<D3D12Backend>(); }

void RequestCaptureBurst(const char* dir, int frames) {
    std::lock_guard<std::mutex> l(g_burst_mu);
    g_burst_dir = dir;
    g_burst_frames = std::clamp(frames, 1, 16);
}

}  // namespace gpu

// xboxrecomp (hppoa): hle_clock.c, a cutscene's letterbox on screen: both bars drawn in the last 100 ms, and then held
// until a bar has been missing for a second (the train cutscene's bars come and go between frames; each gap switched
// the cutscene's lens, window light and held lights off and on, and the characters blinked)
extern "C" int hppoa_Letterbox(void) {
    static std::atomic<bool> on{false};
    uint64_t now = GetTickCount64(), top = now - gpu::g_bar_top_ms.load(), bottom = now - gpu::g_bar_bottom_ms.load();
    bool v = (top < 100 && bottom < 100) || (on.load() && top < 1000 && bottom < 1000);
    on.store(v);
    return v;
}
