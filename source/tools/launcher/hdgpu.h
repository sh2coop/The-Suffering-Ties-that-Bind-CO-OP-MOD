// The upscaler, in this program itself (2026-11-27): Real-ESRGAN's compact network "realesr-animevideov3" run with
// Direct3D 11 compute shaders, on the graphics card (or on the processor through Windows' own WARP device when the
// card cannot). Nothing outside Windows is run. The network (SRVGGNetCompact, from the Real-ESRGAN project, BSD-3
// licence, third_party\REALESRGAN_LICENSE.txt): a 3x3 convolution 3 -> 64 channels, 16 of 64 -> 64, each followed by
// a PReLU, a last one 64 -> 48, a pixel shuffle to 4x, plus the input repeated 4x; the "x2" model then takes that
// down to 2x with a bicubic (as its ncnn file does, Interp 0.5). Weights: the ncnn file realesr-animevideov3-x2.bin
// (each convolution: a 4-byte tag 0x01306B47, fp16 weights [out][in][3][3], fp32 bias; each PReLU: fp32 slopes).
//
// HdGpuOpen(weights) -> 1 ready; HdGpuUpscale(rgb, w, h, out, scale) writes the 2x or 4x picture (RGBA, alpha
// 255). 4x is the network's own result; 2x is that taken down by the bicubic, as the x2 model does.
#define COBJMACROS
#include <d3d11.h>
#include <d3dcompiler.h>

#define HDG_LAYERS 18
static const int kHdgIn[HDG_LAYERS] = {3, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64};
static const int kHdgOut[HDG_LAYERS] = {64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 48};

static const char kHdgConv[] =
    "cbuffer P : register(b0) { uint W; uint H; uint IC; uint WOFF; uint BOFF; uint AOFF; uint PRELU; uint pad; };\n"
    "StructuredBuffer<float> inb : register(t0);\n"
    "StructuredBuffer<float4> wts : register(t1);\n"
    "RWStructuredBuffer<float> outb : register(u0);\n"
    "[numthreads(8, 8, 1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x >= W || id.y >= H) return;\n"
    "    float4 acc[OC4];\n"
    "    [unroll] for (uint o = 0; o < OC4; o++) acc[o] = wts[BOFF + o];\n"
    "    int x = (int)id.x, y = (int)id.y;\n"
    "    for (uint ic = 0; ic < IC; ic++) {\n"
    "        uint plane = ic * W * H;\n"
    "        [unroll] for (int k = 0; k < 9; k++) {\n"
    "            int xx = x + k % 3 - 1, yy = y + k / 3 - 1;\n"
    "            float v = (xx >= 0 && yy >= 0 && xx < (int)W && yy < (int)H) ? inb[plane + yy * W + xx] : 0;\n"
    "            uint base = WOFF + (ic * 9 + k) * OC4;\n"
    "            [unroll] for (uint o = 0; o < OC4; o++) acc[o] += wts[base + o] * v;\n"
    "        }\n"
    "    }\n"
    "    uint at = id.y * W + id.x, size = W * H;\n"
    "    [unroll] for (uint o = 0; o < OC4; o++) {\n"
    "        float4 a = acc[o];\n"
    "        if (PRELU) { float4 s = wts[AOFF + o]; a = a > 0 ? a : a * s; }\n"
    "        outb[(o * 4 + 0) * size + at] = a.x; outb[(o * 4 + 1) * size + at] = a.y;\n"
    "        outb[(o * 4 + 2) * size + at] = a.z; outb[(o * 4 + 3) * size + at] = a.w;\n"
    "    }\n"
    "}\n";

// the 2x picture from the last layer (48 channels at W x H) and the input: the 4x value at (X, Y) is the shuffled
// channel plus the input pixel; the 2x pixel is the bicubic (a = -0.75) of the 4x one at half scale: weights
// -0.09375, 0.59375, 0.59375, -0.09375 over 4x pixels 2X-1 .. 2X+2 (edges clamped)
static const char kHdgFinish[] =
    "cbuffer P : register(b0) { uint W; uint H; uint S; uint pad1, pad2, pad3, pad4, pad5; };\n"
    "StructuredBuffer<float> last : register(t0);\n"
    "StructuredBuffer<float> img : register(t1);\n"
    "RWStructuredBuffer<uint> outb : register(u0);\n"
    "float3 Four(int X, int Y) {\n"
    "    X = clamp(X, 0, (int)W * 4 - 1); Y = clamp(Y, 0, (int)H * 4 - 1);\n"
    "    uint x = X / 4, y = Y / 4, sub = (Y % 4) * 4 + X % 4, size = W * H, at = y * W + x;\n"
    "    float3 c;\n"
    "    c.r = last[(0 * 16 + sub) * size + at] + img[0 * size + at];\n"
    "    c.g = last[(1 * 16 + sub) * size + at] + img[1 * size + at];\n"
    "    c.b = last[(2 * 16 + sub) * size + at] + img[2 * size + at];\n"
    "    return saturate(c);\n"
    "}\n"
    "[numthreads(8, 8, 1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x >= W * S || id.y >= H * S) return;\n"
    "    if (S == 4) {\n"
    "        uint3 f = (uint3)(Four((int)id.x, (int)id.y) * 255 + 0.5);\n"
    "        outb[id.y * W * 4 + id.x] = f.r | f.g << 8 | f.b << 16 | 0xFF000000u;\n"
    "        return;\n"
    "    }\n"
    "    const float k[4] = {-0.09375, 0.59375, 0.59375, -0.09375};\n"
    "    float3 sum = 0;\n"
    "    [unroll] for (int j = 0; j < 4; j++) {\n"
    "        float3 row = 0;\n"
    "        [unroll] for (int i = 0; i < 4; i++) row += k[i] * Four((int)id.x * 2 - 1 + i, (int)id.y * 2 - 1 + j);\n"
    "        sum += k[j] * row;\n"
    "    }\n"
    "    uint3 c = (uint3)(saturate(sum) * 255 + 0.5);\n"
    "    outb[id.y * W * 2 + id.x] = c.r | c.g << 8 | c.b << 16 | 0xFF000000u;\n"
    "}\n";

typedef HRESULT(WINAPI* HdgCompileFn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

typedef struct {
    ID3D11Device* dev;
    ID3D11DeviceContext* ctx;
    ID3D11ComputeShader *conv64, *conv48, *finish;
    ID3D11Buffer* weights;                  // float4s
    ID3D11ShaderResourceView* weights_view;
    ID3D11Buffer* params;
    ID3D11Buffer *img, *a, *b, *out, *staging;
    ID3D11ShaderResourceView *img_view, *a_view, *b_view;
    ID3D11UnorderedAccessView *a_uav, *b_uav, *out_uav;
    uint32_t cap;                           // pixels the buffers hold at 1x
    uint32_t woff[HDG_LAYERS], boff[HDG_LAYERS], aoff[HDG_LAYERS];
    int warp;
} HdGpu;
static HdGpu g_hdg;

static float HalfToFloat(uint16_t h) {
    uint32_t s = (uint32_t)(h & 0x8000) << 16, e = (h >> 10) & 31, m = h & 0x3FF, f;
    if (e == 0) {
        if (!m) f = s;
        else { e = 127 - 15 + 1; while (!(m & 0x400)) { m <<= 1; e--; } m &= 0x3FF; f = s | e << 23 | m << 13; }
    } else if (e == 31) f = s | 0x7F800000 | m << 13;
    else f = s | (e + 127 - 15) << 23 | m << 13;
    float out;
    memcpy(&out, &f, 4);
    return out;
}

static ID3D11ComputeShader* HdgShader(HdgCompileFn compile, const char* source, const char* oc4) {
    D3D_SHADER_MACRO defines[] = {{"OC4", oc4}, {NULL, NULL}};
    ID3DBlob *code = NULL, *errors = NULL;
    HRESULT hr = compile(source, strlen(source), "hd", oc4 ? defines : NULL, NULL, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        if (errors) { HdLog(L"shader: %hs", (const char*)ID3D10Blob_GetBufferPointer(errors)); ID3D10Blob_Release(errors); }
        return NULL;
    }
    if (errors) ID3D10Blob_Release(errors);
    ID3D11ComputeShader* cs = NULL;
    ID3D11Device_CreateComputeShader(g_hdg.dev, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), NULL, &cs);
    ID3D10Blob_Release(code);
    return cs;
}

static ID3D11Buffer* HdgBuffer(uint32_t count, uint32_t stride, UINT bind, D3D11_USAGE usage, UINT cpu, const void* data) {
    D3D11_BUFFER_DESC d = {0};
    d.ByteWidth = count * stride;
    d.Usage = usage;
    d.BindFlags = bind;
    d.CPUAccessFlags = cpu;
    if (bind & (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS)) {
        d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        d.StructureByteStride = stride;
    }
    D3D11_SUBRESOURCE_DATA init = {data, 0, 0};
    ID3D11Buffer* b = NULL;
    ID3D11Device_CreateBuffer(g_hdg.dev, &d, data ? &init : NULL, &b);
    return b;
}
static ID3D11ShaderResourceView* HdgView(ID3D11Buffer* b, uint32_t count) {
    D3D11_SHADER_RESOURCE_VIEW_DESC d = {0};
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    d.Buffer.NumElements = count;
    ID3D11ShaderResourceView* v = NULL;
    if (b) ID3D11Device_CreateShaderResourceView(g_hdg.dev, (ID3D11Resource*)b, &d, &v);
    return v;
}
static ID3D11UnorderedAccessView* HdgUav(ID3D11Buffer* b, uint32_t count) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC d = {0};
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    d.Buffer.NumElements = count;
    ID3D11UnorderedAccessView* v = NULL;
    if (b) ID3D11Device_CreateUnorderedAccessView(g_hdg.dev, (ID3D11Resource*)b, &d, &v);
    return v;
}
#define HDG_RELEASE(p) do { if (p) { IUnknown_Release((IUnknown*)(p)); (p) = NULL; } } while (0)

static void HdgFreeBuffers(void) {
    HDG_RELEASE(g_hdg.img_view); HDG_RELEASE(g_hdg.a_view); HDG_RELEASE(g_hdg.b_view);
    HDG_RELEASE(g_hdg.a_uav); HDG_RELEASE(g_hdg.b_uav); HDG_RELEASE(g_hdg.out_uav);
    HDG_RELEASE(g_hdg.img); HDG_RELEASE(g_hdg.a); HDG_RELEASE(g_hdg.b); HDG_RELEASE(g_hdg.out); HDG_RELEASE(g_hdg.staging);
    g_hdg.cap = 0;
}
// buffers for pictures of up to pixels (1x)
static int HdgEnsure(uint32_t pixels) {
    if (pixels <= g_hdg.cap) return 1;
    HdgFreeBuffers();
    uint32_t n = pixels;
    UINT both = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    g_hdg.img = HdgBuffer(n * 3, 4, D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0, NULL);
    g_hdg.a = HdgBuffer(n * 64, 4, both, D3D11_USAGE_DEFAULT, 0, NULL);
    g_hdg.b = HdgBuffer(n * 64, 4, both, D3D11_USAGE_DEFAULT, 0, NULL);
    g_hdg.out = HdgBuffer(n * 16, 4, D3D11_BIND_UNORDERED_ACCESS, D3D11_USAGE_DEFAULT, 0, NULL);
    g_hdg.staging = HdgBuffer(n * 16, 4, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, NULL);
    g_hdg.img_view = HdgView(g_hdg.img, n * 3);
    g_hdg.a_view = HdgView(g_hdg.a, n * 64);
    g_hdg.b_view = HdgView(g_hdg.b, n * 64);
    g_hdg.a_uav = HdgUav(g_hdg.a, n * 64);
    g_hdg.b_uav = HdgUav(g_hdg.b, n * 64);
    g_hdg.out_uav = HdgUav(g_hdg.out, n * 16);
    if (!g_hdg.img_view || !g_hdg.a_view || !g_hdg.b_view || !g_hdg.a_uav || !g_hdg.b_uav || !g_hdg.out_uav || !g_hdg.staging) {
        HdgFreeBuffers();
        return 0;
    }
    g_hdg.cap = pixels;
    return 1;
}

// the weights (the ncnn file's bytes) -> 1 ready
static int HdGpuOpen(const uint8_t* bin, size_t size) {
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    HdgCompileFn compile = compiler ? (HdgCompileFn)GetProcAddress(compiler, "D3DCompile") : NULL;
    if (!compile) { HdLog(L"d3dcompiler_47.dll is missing"); return 0; }
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0, got;
    const wchar_t* force_warp = _wgetenv(L"S2_HD_WARP");   // tests: the processor path
    HRESULT hr = force_warp ? E_FAIL : D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &level, 1, D3D11_SDK_VERSION, &g_hdg.dev, &got, &g_hdg.ctx);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, &level, 1, D3D11_SDK_VERSION, &g_hdg.dev, &got, &g_hdg.ctx);
        g_hdg.warp = 1;
    }
    if (FAILED(hr)) { HdLog(L"no Direct3D 11 device (0x%08lx)", hr); return 0; }

    // the weights as float4s: per layer [in][9][out/4], then the bias [out/4], then the PReLU slopes [out/4]
    uint32_t total = 0;
    for (int l = 0; l < HDG_LAYERS; l++) total += (kHdgIn[l] * 9 + 2) * (kHdgOut[l] / 4);
    float* w = (float*)calloc((size_t)total * 4, sizeof(float));
    size_t at = 0;
    uint32_t f4 = 0;
    int ok = 1;
    for (int l = 0; l < HDG_LAYERS && ok; l++) {
        int in = kHdgIn[l], oc = kHdgOut[l];
        uint32_t tag;
        if (at + 4 > size) { ok = 0; break; }
        memcpy(&tag, bin + at, 4);
        at += 4;
        size_t n = (size_t)oc * in * 9;
        if (tag != 0x01306B47 || at + n * 2 + oc * 4 > size) { ok = 0; break; }
        g_hdg.woff[l] = f4;
        for (int o = 0; o < oc; o++)
            for (int i = 0; i < in; i++)
                for (int k = 0; k < 9; k++) {
                    uint16_t h;
                    memcpy(&h, bin + at + (((size_t)o * in + i) * 9 + k) * 2, 2);
                    w[((size_t)f4 + (i * 9 + k) * (oc / 4)) * 4 + o] = HalfToFloat(h);
                }
        at += n * 2;
        f4 += in * 9 * (oc / 4);
        g_hdg.boff[l] = f4;
        memcpy(w + (size_t)f4 * 4, bin + at, oc * 4);
        at += oc * 4;
        f4 += oc / 4;
        g_hdg.aoff[l] = f4;
        if (l < HDG_LAYERS - 1) {
            if (at + oc * 4 > size) { ok = 0; break; }
            memcpy(w + (size_t)f4 * 4, bin + at, oc * 4);
            at += oc * 4;
        }
        f4 += oc / 4;
    }
    if (!ok || at != size) {
        HdLog(L"the upscaler's weights are not the expected file (%zu of %zu bytes read)", at, size);
        free(w);
        return 0;
    }
    g_hdg.weights = HdgBuffer(total, 16, D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_IMMUTABLE, 0, w);
    free(w);
    g_hdg.weights_view = HdgView(g_hdg.weights, total);
    g_hdg.params = HdgBuffer(1, 32, D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DEFAULT, 0, NULL);
    g_hdg.conv64 = HdgShader(compile, kHdgConv, "16");
    g_hdg.conv48 = HdgShader(compile, kHdgConv, "12");
    g_hdg.finish = HdgShader(compile, kHdgFinish, NULL);
    if (!g_hdg.weights_view || !g_hdg.params || !g_hdg.conv64 || !g_hdg.conv48 || !g_hdg.finish) { HdLog(L"the upscaler's shaders could not be made"); return 0; }
    HdLog(L"upscaler ready (%ls)", g_hdg.warp ? L"on the processor, WARP" : L"on the graphics card");
    return 1;
}

static void HdgParams(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g) {
    uint32_t p[8] = {a, b, c, d, e, f, g, 0};
    ID3D11DeviceContext_UpdateSubresource(g_hdg.ctx, (ID3D11Resource*)g_hdg.params, 0, NULL, p, 0, 0);
}

// rgb: w*h*3 bytes -> out: (scale w)*(scale h)*4 bytes (alpha 255), scale 2 or 4; -> 0 failed
static int HdGpuUpscale(const uint8_t* rgb, uint32_t w, uint32_t h, uint8_t* out, uint32_t scale) {
    uint32_t n = w * h;
    if (!HdgEnsure(n)) return 0;
    float* planes = (float*)malloc((size_t)n * 3 * sizeof(float));
    for (uint32_t i = 0; i < n; i++)
        for (int c = 0; c < 3; c++) planes[c * n + i] = rgb[i * 3 + c] / 255.0f;
    D3D11_BOX box = {0, 0, 0, n * 3 * 4, 1, 1};
    ID3D11DeviceContext_UpdateSubresource(g_hdg.ctx, (ID3D11Resource*)g_hdg.img, 0, &box, planes, 0, 0);
    free(planes);
    ID3D11DeviceContext* c = g_hdg.ctx;
    ID3D11DeviceContext_CSSetConstantBuffers(c, 0, 1, &g_hdg.params);
    ID3D11ShaderResourceView* none_srv[2] = {NULL, NULL};
    ID3D11UnorderedAccessView* none_uav = NULL;
    UINT gx = (w + 7) / 8, gy = (h + 7) / 8;
    for (int l = 0; l < HDG_LAYERS; l++) {
        ID3D11ShaderResourceView* src[2] = {l == 0 ? g_hdg.img_view : (l & 1) ? g_hdg.a_view : g_hdg.b_view, g_hdg.weights_view};
        ID3D11UnorderedAccessView* dst = (l & 1) ? g_hdg.b_uav : g_hdg.a_uav;
        HdgParams(w, h, kHdgIn[l], g_hdg.woff[l], g_hdg.boff[l], g_hdg.aoff[l], l < HDG_LAYERS - 1);
        ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 1, &none_uav, NULL);
        ID3D11DeviceContext_CSSetShaderResources(c, 0, 2, src);
        ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 1, &dst, NULL);
        ID3D11DeviceContext_CSSetShader(c, l == HDG_LAYERS - 1 ? g_hdg.conv48 : g_hdg.conv64, NULL, 0);
        ID3D11DeviceContext_Dispatch(c, gx, gy, 1);
        ID3D11DeviceContext_CSSetShaderResources(c, 0, 2, none_srv);
    }
    // layer 17 (odd) wrote b
    ID3D11ShaderResourceView* src[2] = {g_hdg.b_view, g_hdg.img_view};
    HdgParams(w, h, scale, 0, 0, 0, 0);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 1, &none_uav, NULL);
    ID3D11DeviceContext_CSSetShaderResources(c, 0, 2, src);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 1, &g_hdg.out_uav, NULL);
    ID3D11DeviceContext_CSSetShader(c, g_hdg.finish, NULL, 0);
    ID3D11DeviceContext_Dispatch(c, (w * scale + 7) / 8, (h * scale + 7) / 8, 1);
    ID3D11DeviceContext_CSSetShaderResources(c, 0, 2, none_srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 1, &none_uav, NULL);
    D3D11_BOX outbox = {0, 0, 0, n * scale * scale * 4, 1, 1};
    ID3D11DeviceContext_CopySubresourceRegion(c, (ID3D11Resource*)g_hdg.staging, 0, 0, 0, 0, (ID3D11Resource*)g_hdg.out, 0, &outbox);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(c, (ID3D11Resource*)g_hdg.staging, 0, D3D11_MAP_READ, 0, &m))) return 0;
    memcpy(out, m.pData, (size_t)n * scale * scale * 4);
    ID3D11DeviceContext_Unmap(c, (ID3D11Resource*)g_hdg.staging, 0);
    return 1;
}
