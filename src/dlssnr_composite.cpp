// dlssnr_composite: runs DLSS-NR at half resolution and writes
//   output = native + bilinear_up(NR_output - NR_input)
// Hooks NVSDK_NGX_D3D12_{Create,Evaluate,Release}Feature in every NGX module.
// Loads as a ReShade addon (overlay with an on/off toggle), as an ASI plugin
// or as a plain DLL; without ReShade it is always on.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <d3d12.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#include <MinHook.h>

#include "ngx_minimal.h"
#include "Reduce.h"    // fxc output: kReduceCS[]
#include "Compose.h"   // fxc output: kComposeCS[]

extern "C" IMAGE_DOS_HEADER __ImageBase;
extern "C" __declspec(dllexport) const char* NAME = "DLSS-NR Composite";
extern "C" __declspec(dllexport) const char* DESCRIPTION = "DLSS Neural Rendering at half resolution, composited onto the native frame.";

namespace
{
// ---------------------------------------------------------------- log
std::wstring g_logPath;
std::mutex g_logMutex;
bool g_reshade = false;   // registered with ReShade (overlay, log mirror)

void Log(reshade::log::level level, const char* fmt, ...)
{
    char line[1024];
    va_list args; va_start(args, fmt); std::vsnprintf(line, sizeof(line), fmt, args); va_end(args);
    if (g_reshade) reshade::log::message(level, line);
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (FILE* f = g_logPath.empty() ? nullptr : _wfopen(g_logPath.c_str(), L"ab")) { std::fputs(line, f); std::fputc('\n', f); std::fclose(f); }
}
#define LOG(...) Log(reshade::log::level::info, __VA_ARGS__)
#define WARN(...) Log(reshade::log::level::warning, __VA_ARGS__)

// ---------------------------------------------------------------- NGX
constexpr NVSDK_NGX_Result kFail = static_cast<NVSDK_NGX_Result>(0xBAD00001);
constexpr int kNeuralRenderingFeature = 18;
constexpr uint32_t kMinWidth = 640;
// Reduced-stream parameter calibration (fitted so the half-resolution result matches the native look).
constexpr float kIntensityBias = 0.168f;

using PFN_Create = NVSDK_NGX_Result(*)(ID3D12GraphicsCommandList*, int, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using PFN_Evaluate = NVSDK_NGX_Result(*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, void*);
using PFN_Release = NVSDK_NGX_Result(*)(NVSDK_NGX_Handle*);
using PFN_AllocateParameters = NVSDK_NGX_Result(*)(NVSDK_NGX_Parameter**);
using PFN_DestroyParameters = NVSDK_NGX_Result(*)(NVSDK_NGX_Parameter*);
using PFN_LoadLibraryExW = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);

bool GetNumber(const NVSDK_NGX_Parameter* p, const char* name, double& out)
{
    float f; if (p->Get(name, &f) == kNgxSuccess) { out = f; return true; }
    unsigned int ui; if (p->Get(name, &ui) == kNgxSuccess) { out = ui; return true; }
    int i; if (p->Get(name, &i) == kNgxSuccess) { out = i; return true; }
    unsigned long long ull; if (p->Get(name, &ull) == kNgxSuccess) { out = double(ull); return true; }
    double d; if (p->Get(name, &d) == kNgxSuccess) { out = d; return true; }
    return false;
}
ID3D12Resource* GetResource(const NVSDK_NGX_Parameter* p, const char* name)
{
    ID3D12Resource* r = nullptr;
    return p->Get(name, &r) == kNgxSuccess ? r : nullptr;
}
void CopyNumber(const NVSDK_NGX_Parameter* from, NVSDK_NGX_Parameter* to, const char* name)
{
    float f; if (from->Get(name, &f) == kNgxSuccess) { to->Set(name, f); return; }
    unsigned int ui; if (from->Get(name, &ui) == kNgxSuccess) { to->Set(name, ui); return; }
    int i; if (from->Get(name, &i) == kNgxSuccess) { to->Set(name, i); return; }
    unsigned long long ull; if (from->Get(name, &ull) == kNgxSuccess) { to->Set(name, ull); return; }
}
float Calibrate(double v, float scale, float bias = 0.0f)
{
    return std::clamp(static_cast<float>(1.0 + (v - 1.0) * scale) - bias, 0.0f, 2.0f);
}

// ---------------------------------------------------------------- shaders (src/dlssnr_composite.hlsl, embedded)
struct Constants { uint32_t fullWidth, fullHeight, lowWidth, lowHeight, hasMotion, pad[3]; };

struct Gpu
{
    ID3D12Device* device = nullptr;
    ID3D12RootSignature* rootSignature = nullptr;
    ID3D12PipelineState *reduce = nullptr, *compose = nullptr;
    bool ready = false, failed = false;
    void Release()
    {
        for (IUnknown* u : { (IUnknown*)reduce, (IUnknown*)compose, (IUnknown*)rootSignature }) if (u) u->Release();
        reduce = compose = nullptr; rootSignature = nullptr; ready = false;
    }
};

bool CreatePipeline(Gpu& gpu, const void* code, size_t size, ID3D12PipelineState** out)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = gpu.rootSignature;
    desc.CS = { code, size };
    return SUCCEEDED(gpu.device->CreateComputePipelineState(&desc, IID_PPV_ARGS(out)));
}

// root constants b0; table: SRV t0..t2, UAV u0..u2; static linear clamp sampler s0
bool InitGpu(Gpu& gpu, ID3D12Device* device)
{
    if (gpu.ready) return gpu.device == device;
    if (gpu.failed) return false;
    gpu.failed = true;
    gpu.device = device;

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0 };
    ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, 3 };
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = { 0, 0, sizeof(Constants) / 4 };
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = { 2, ranges };
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rs{ 2, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3DBlob* blob = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr))) return false;
    HRESULT hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&gpu.rootSignature));
    blob->Release();
    if (FAILED(hr)) return false;
    if (!CreatePipeline(gpu, kReduceCS, sizeof(kReduceCS), &gpu.reduce) ||
        !CreatePipeline(gpu, kComposeCS, sizeof(kComposeCS), &gpu.compose)) { WARN("pipeline creation failed"); return false; }
    gpu.failed = false;
    gpu.ready = true;
    return true;
}

// ---------------------------------------------------------------- resources
DXGI_FORMAT ViewFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    default: return f;
    }
}
bool SupportedColorFormat(DXGI_FORMAT f)
{
    f = ViewFormat(f);
    return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_R16G16B16A16_FLOAT ||
        f == DXGI_FORMAT_R10G10B10A2_UNORM || f == DXGI_FORMAT_R11G11B10_FLOAT;
}
// half-resolution copies of 10/11-bit formats are 16-bit float
DXGI_FORMAT ReducedFormat(DXGI_FORMAT f)
{
    f = ViewFormat(f);
    return (f == DXGI_FORMAT_R10G10B10A2_UNORM || f == DXGI_FORMAT_R11G11B10_FLOAT) ? DXGI_FORMAT_R16G16B16A16_FLOAT : f;
}
void Transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r, D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES to)
{
    if (state == to) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, state, to };
    cmd->ResourceBarrier(1, &b);
    state = to;
}
ID3D12Resource* CreateTexture(ID3D12Device* device, uint32_t w, uint32_t h, DXGI_FORMAT format, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = w; desc.Height = h; desc.DepthOrArraySize = 1;
    desc.MipLevels = 1; desc.Format = format; desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* r = nullptr;
    if (SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&r))))
        r->SetName(name);
    return r;
}

// ---------------------------------------------------------------- features
struct NgxModule
{
    HMODULE module = nullptr;
    std::wstring name;
    PFN_Create createOriginal = nullptr;
    PFN_Evaluate evaluateOriginal = nullptr;
    PFN_Release releaseOriginal = nullptr;
    PFN_AllocateParameters allocate = nullptr;
    PFN_DestroyParameters destroy = nullptr;
};

// descriptor sets per evaluation (reduce, compose), 6 descriptors each, in a ring so frames in flight never share
constexpr uint32_t kSetsPerFrame = 2, kDescriptorsPerSet = 6, kRing = 16;
constexpr uint32_t kHeapDescriptors = kRing * kSetsPerFrame * kDescriptorsPerSet;

struct Feature
{
    NgxModule* owner = nullptr;
    NgxModule* paramOwner = nullptr;
    NVSDK_NGX_Handle* real = nullptr;
    NVSDK_NGX_Handle* nativeReal = nullptr;      // the game's own native feature, created when the toggle is off
    bool nativeFirst = true;
    NVSDK_NGX_Parameter* gameParams = nullptr;
    NVSDK_NGX_Handle handle{};
    NVSDK_NGX_Parameter* params = nullptr;
    uint32_t nativeW = 0, nativeH = 0, lowW = 0, lowH = 0;
    bool resourcesReady = false, firstEvaluate = true;
    ID3D12Resource *lowColor = nullptr, *lowOutput = nullptr, *lowMotion = nullptr, *lowDepth = nullptr;
    D3D12_RESOURCE_STATES lowColorState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS, lowOutputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        lowMotionState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS, lowDepthState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ID3D12DescriptorHeap* heap = nullptr;
    uint32_t descriptorSize = 0, ring = 0;
    unsigned long long evaluations = 0;
    ~Feature()
    {
        for (IUnknown* u : { (IUnknown*)lowColor, (IUnknown*)lowOutput, (IUnknown*)lowMotion, (IUnknown*)lowDepth, (IUnknown*)heap }) if (u) u->Release();
        if (params && paramOwner) paramOwner->destroy(params);
    }
};

struct State
{
    std::mutex mutex;
    std::vector<std::unique_ptr<NgxModule>> modules;
    std::unordered_map<const NVSDK_NGX_Handle*, std::unique_ptr<Feature>> features;
    Gpu gpu;
    uint32_t nextId = 0x4E525200;
    PFN_LoadLibraryExW loadLibraryExW = nullptr;
    unsigned long long evaluations = 0, intercepted = 0;
    bool enabled = true;   // ReShade overlay toggle; off = the game's own native NR
    // released features are destroyed a few presents later: their last command list may still be executing
    unsigned presents = 0;
    std::vector<std::pair<unsigned, std::unique_ptr<Feature>>> graveyard;
} g;

void BuryDueFeatures(bool all)
{
    std::vector<std::unique_ptr<Feature>> due;
    std::lock_guard<std::mutex> lock(g.mutex);
    for (size_t i = 0; i < g.graveyard.size();)
    {
        if (all || g.graveyard[i].first <= g.presents) { due.push_back(std::move(g.graveyard[i].second)); g.graveyard.erase(g.graveyard.begin() + i); }
        else ++i;
    }
}

bool PrepareResources(Feature& f, ID3D12Device* device, ID3D12Resource* color, ID3D12Resource* output, ID3D12Resource* motion)
{
    if (f.resourcesReady) return true;
    const DXGI_FORMAT cf = color->GetDesc().Format, of = output->GetDesc().Format;
    if (!SupportedColorFormat(cf) || !SupportedColorFormat(of)) { WARN("unsupported colour/output format %d/%d", int(cf), int(of)); return false; }
    DXGI_FORMAT motionFormat = DXGI_FORMAT_R16G16_FLOAT;
    if (motion && ViewFormat(motion->GetDesc().Format) == DXGI_FORMAT_R32G32_FLOAT) motionFormat = DXGI_FORMAT_R32G32_FLOAT;
    f.lowColor = CreateTexture(device, f.lowW, f.lowH, ReducedFormat(cf), L"dlssnr_composite low colour");
    f.lowOutput = CreateTexture(device, f.lowW, f.lowH, ReducedFormat(of), L"dlssnr_composite low output");
    f.lowMotion = CreateTexture(device, f.lowW, f.lowH, motionFormat, L"dlssnr_composite low motion");
    f.lowDepth = CreateTexture(device, f.lowW, f.lowH, DXGI_FORMAT_R32_FLOAT, L"dlssnr_composite low depth");
    D3D12_DESCRIPTOR_HEAP_DESC hd{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kHeapDescriptors, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&f.heap));
    f.descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (!f.lowColor || !f.lowOutput || !f.lowMotion || !f.lowDepth || !f.heap) { WARN("resource creation failed"); return false; }
    f.resourcesReady = true;
    LOG("resources ready: %ux%u -> %ux%u, formats %d/%d", f.nativeW, f.nativeH, f.lowW, f.lowH, int(cf), int(of));
    return true;
}

// descriptor set `set`: SRVs t0..t2, UAV u0 = colour target, u1/u2 = reduced motion/depth
D3D12_GPU_DESCRIPTOR_HANDLE WriteSet(Feature& f, ID3D12Device* device, uint32_t set,
    ID3D12Resource* srv0, ID3D12Resource* srv1, ID3D12Resource* srv2, ID3D12Resource* uavColor)
{
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = f.heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += size_t(set) * kDescriptorsPerSet * f.descriptorSize;
    for (ID3D12Resource* r : { srv0, srv1, srv2 })
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = ViewFormat(r->GetDesc().Format);
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(r, &sd, cpu);
        cpu.ptr += f.descriptorSize;
    }
    for (ID3D12Resource* r : { uavColor, f.lowMotion, f.lowDepth })
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = ViewFormat(r->GetDesc().Format);
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(r, nullptr, &ud, cpu);
        cpu.ptr += f.descriptorSize;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = f.heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += UINT64(set) * kDescriptorsPerSet * f.descriptorSize;
    return gpu;
}

void Dispatch(ID3D12GraphicsCommandList* cmd, ID3D12PipelineState* pso, const Constants& c, D3D12_GPU_DESCRIPTOR_HANDLE table, uint32_t w, uint32_t h)
{
    cmd->SetPipelineState(pso);
    cmd->SetComputeRoot32BitConstants(0, sizeof(c) / 4, &c, 0);
    cmd->SetComputeRootDescriptorTable(1, table);
    cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
}

NVSDK_NGX_Result EvaluateReduced(Feature& f, ID3D12GraphicsCommandList* cmd, const NVSDK_NGX_Parameter* p, void* callback)
{
    ID3D12Resource* color = GetResource(p, "DLSSNR.Color");
    ID3D12Resource* output = GetResource(p, "DLSSNR.Output");
    ID3D12Resource* motion = GetResource(p, "DLSSNR.MVec");
    if (!color || !output) return kFail;
    ID3D12Device* device = nullptr;
    cmd->GetDevice(IID_PPV_ARGS(&device));
    if (device) device->Release();
    if (!device || !InitGpu(g.gpu, device) || !PrepareResources(f, device, color, output, motion)) return kFail;

    const Constants c{ f.nativeW, f.nativeH, f.lowW, f.lowH, motion ? 1u : 0u, {} };
    const uint32_t base = (f.ring++ % kRing) * kSetsPerFrame;
    ID3D12DescriptorHeap* heaps[] = { f.heap };

    // reduce the game's inputs
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(g.gpu.rootSignature);
    Transition(cmd, f.lowColor, f.lowColorState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(cmd, f.lowMotion, f.lowMotionState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(cmd, f.lowDepth, f.lowDepthState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Dispatch(cmd, g.gpu.reduce, c, WriteSet(f, device, base, color, motion ? motion : color, color, f.lowColor), f.lowW, f.lowH);
    Transition(cmd, f.lowColor, f.lowColorState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(cmd, f.lowMotion, f.lowMotionState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(cmd, f.lowDepth, f.lowDepthState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(cmd, f.lowOutput, f.lowOutputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // the real feature on the reduced grid
    NVSDK_NGX_Parameter* q = f.params;
    q->Set("DLSSNR.Color", f.lowColor); q->Set("DLSSNR.Output", f.lowOutput);
    q->Set("DLSSNR.MVec", f.lowMotion); q->Set("DLSSNR.Depth", f.lowDepth);
    for (const char* n : { "DLSSNR.Width", "DLSSNR.InputWidth", "DLSSNR.OutputWidth", "DLSSNR.Output.Width" }) q->Set(n, f.lowW);
    for (const char* n : { "DLSSNR.Height", "DLSSNR.InputHeight", "DLSSNR.OutputHeight", "DLSSNR.Output.Height" }) q->Set(n, f.lowH);
    for (const char* b : { "DLSSNR.Color", "DLSSNR.MVec", "DLSSNR.Depth" })
    {
        const std::string s(b);
        q->Set((s + "SubrectBaseX").c_str(), 0u); q->Set((s + "SubrectBaseY").c_str(), 0u);
        q->Set((s + "SubrectWidth").c_str(), f.lowW); q->Set((s + "SubrectHeight").c_str(), f.lowH);
    }
    double jx = 0, jy = 0, sx = 1, sy = 1, reset = 0, inverted = 0;
    if (!GetNumber(p, "DLSSNR.JitterOffsetX", jx)) GetNumber(p, "Jitter.Offset.X", jx);
    if (!GetNumber(p, "DLSSNR.JitterOffsetY", jy)) GetNumber(p, "Jitter.Offset.Y", jy);
    GetNumber(p, "DLSSNR.MVecScaleX", sx); GetNumber(p, "DLSSNR.MVecScaleY", sy);
    if (!GetNumber(p, "DLSSNR.Reset", reset)) GetNumber(p, "Reset", reset);
    GetNumber(p, "DLSSNR.DepthInverted", inverted);
    q->Set("Jitter.Offset.X", float(jx * 0.5)); q->Set("Jitter.Offset.Y", float(jy * 0.5));
    q->Set("DLSSNR.JitterOffsetX", float(jx * 0.5)); q->Set("DLSSNR.JitterOffsetY", float(jy * 0.5));
    q->Set("DLSSNR.MVecScaleX", float(sx * 0.5)); q->Set("DLSSNR.MVecScaleY", float(sy * 0.5));
    q->Set("DLSSNR.DepthInverted", int(inverted));
    const int resetNow = (reset != 0.0 || f.firstEvaluate) ? 1 : 0;
    q->Set("Reset", resetNow); q->Set("DLSSNR.Reset", resetNow);
    q->Set("DLSSNR.ScalingRatio", 0.5f);
    q->Set("DLSSNR.Enabled", 1);
    f.firstEvaluate = false;
    const NVSDK_NGX_Result result = f.owner->evaluateOriginal(cmd, f.real, q, callback);
    if (result != kNgxSuccess) return result;

    // composite (NGX changed the command list state; rebind)
    Transition(cmd, f.lowOutput, f.lowOutputState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(g.gpu.rootSignature);
    Dispatch(cmd, g.gpu.compose, c, WriteSet(f, device, base + 1, f.lowColor, f.lowOutput, color, output), f.nativeW, f.nativeH);
    ++f.evaluations; ++g.evaluations;
    return kNgxSuccess;
}

// ---------------------------------------------------------------- hooks
NVSDK_NGX_Result HookedCreate(NgxModule& m, ID3D12GraphicsCommandList* cmd, int feature, NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle)
{
    double width = 0, height = 0, ratio = 1.0;
    ID3D12Device* device = nullptr;
    NgxModule* paramOwner = nullptr;
    if (feature == kNeuralRenderingFeature && parameters && handle && cmd)
    {
        if (!GetNumber(parameters, "DLSSNR.Width", width)) GetNumber(parameters, "Width", width);
        if (!GetNumber(parameters, "DLSSNR.Height", height)) GetNumber(parameters, "Height", height);
        GetNumber(parameters, "DLSSNR.ScalingRatio", ratio);
        cmd->GetDevice(IID_PPV_ARGS(&device));
        if (device) device->Release();
        std::lock_guard<std::mutex> lock(g.mutex);
        for (const auto& mod : g.modules) if (mod->allocate && mod->destroy) { paramOwner = mod.get(); break; }
    }
    const bool reduce = width >= kMinWidth && height >= 16 && std::fabs(ratio - 1.0) < 1e-3 && device && paramOwner && InitGpu(g.gpu, device);
    if (!reduce) return m.createOriginal(cmd, feature, parameters, handle);

    auto f = std::make_unique<Feature>();
    f->owner = &m;
    f->gameParams = parameters;
    f->nativeW = uint32_t(width); f->nativeH = uint32_t(height);
    f->lowW = (f->nativeW + 1) / 2; f->lowH = (f->nativeH + 1) / 2;
    if (paramOwner->allocate(&f->params) != kNgxSuccess || !f->params) return m.createOriginal(cmd, feature, parameters, handle);
    f->paramOwner = paramOwner;
    NVSDK_NGX_Parameter* q = f->params;
    for (const char* n : { "CreationNodeMask", "VisibilityNodeMask", "DLSSNR.Hint.Render.Preset", "PerfQualityValue",
        "DLSSNR.UseAutoMask", "DLSSNR.UICorrection", "DLSSNR.Scale", "DLSSNR.Upscaling", "DLSSNR.Style" })
        CopyNumber(parameters, q, n);
    void* cb = nullptr;
    if (parameters->Get("DLSSNRComputeScalingRatioCallback", &cb) == kNgxSuccess && cb) q->Set("DLSSNRComputeScalingRatioCallback", cb);
    for (const char* n : { "Width", "DLSSNR.Width", "DLSSNR.InputWidth", "DLSSNR.OutputWidth", "DLSSNR.Output.Width", "OutWidth" }) q->Set(n, f->lowW);
    for (const char* n : { "Height", "DLSSNR.Height", "DLSSNR.InputHeight", "DLSSNR.OutputHeight", "DLSSNR.Output.Height", "OutHeight" }) q->Set(n, f->lowH);
    double intensity = 1, tone = 1, structure = 1, global = 1, skin = -1;
    GetNumber(parameters, "DLSSNR.Intensity", intensity); GetNumber(parameters, "DLSSNR.LocalToneStrength", tone);
    GetNumber(parameters, "DLSSNR.LocalStructureStrength", structure); GetNumber(parameters, "DLSSNR.GlobalToneStrength", global);
    GetNumber(parameters, "DLSSNR.SkinStructureStrength", skin);
    q->Set("DLSSNR.Intensity", Calibrate(intensity, 0.56f, kIntensityBias));
    q->Set("DLSSNR.LocalToneStrength", Calibrate(tone, 0.582f));
    q->Set("DLSSNR.LocalStructureStrength", Calibrate(structure, 0.479f));
    q->Set("DLSSNR.GlobalToneStrength", Calibrate(global, 0.40f));
    q->Set("DLSSNR.SkinStructureStrength", float(std::clamp(skin, -1.0, 2.0)));
    q->Set("DLSSNR.ScalingRatio", 0.5f);
    q->Set("DLSSNR.Enabled", 1);
    const NVSDK_NGX_Result result = m.createOriginal(cmd, feature, q, &f->real);
    if (result != kNgxSuccess || !f->real)
    {
        WARN("reduced feature creation failed (0x%08X); native feature created instead", unsigned(result));
        return m.createOriginal(cmd, feature, parameters, handle);
    }
    std::lock_guard<std::mutex> lock(g.mutex);
    f->handle.Id = g.nextId++;
    *handle = &f->handle;
    ++g.intercepted;
    LOG("NR feature %ux%u intercepted (%ls): real feature at %ux%u", f->nativeW, f->nativeH, m.name.c_str(), f->lowW, f->lowH);
    g.features[*handle] = std::move(f);
    return kNgxSuccess;
}

NVSDK_NGX_Result HookedEvaluate(NgxModule& m, ID3D12GraphicsCommandList* cmd, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* parameters, void* callback)
{
    Feature* f = nullptr;
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        auto it = g.features.find(handle);
        if (it != g.features.end()) f = it->second.get();
    }
    if (!f || !parameters || !cmd) return m.evaluateOriginal(cmd, handle, parameters, callback);
    if (!g_reshade) { ++g.presents; BuryDueFeatures(false); }   // no present event without ReShade: evaluations are the clock
    if (!g.enabled)
    {
        // toggle off: the game's native-resolution NR on a feature made from its own creation parameters
        if (!f->nativeReal && m.createOriginal(cmd, kNeuralRenderingFeature, f->gameParams, &f->nativeReal) != kNgxSuccess)
            f->nativeReal = nullptr;
        if (!f->nativeReal) return EvaluateReduced(*f, cmd, parameters, callback);
        NVSDK_NGX_Parameter* p = const_cast<NVSDK_NGX_Parameter*>(parameters);
        if (f->nativeFirst) { p->Set("Reset", 1); p->Set("DLSSNR.Reset", 1); }
        const NVSDK_NGX_Result result = m.evaluateOriginal(cmd, f->nativeReal, parameters, callback);
        if (f->nativeFirst) { p->Set("Reset", 0); p->Set("DLSSNR.Reset", 0); f->nativeFirst = false; }
        f->firstEvaluate = true;
        return result;
    }
    f->nativeFirst = true;
    return EvaluateReduced(*f, cmd, parameters, callback);
}

NVSDK_NGX_Result HookedRelease(NgxModule& m, NVSDK_NGX_Handle* handle)
{
    std::unique_ptr<Feature> f;
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        auto it = g.features.find(handle);
        if (it != g.features.end()) { f = std::move(it->second); g.features.erase(it); }
    }
    if (!f) return m.releaseOriginal(handle);
    if (f->nativeReal) m.releaseOriginal(f->nativeReal);
    const NVSDK_NGX_Result result = m.releaseOriginal(f->real);
    LOG("NR feature released after %llu reduced evaluations", f->evaluations);
    std::lock_guard<std::mutex> lock(g.mutex);
    g.graveyard.emplace_back(g.presents + 8, std::move(f));
    return result;
}

// MinHook detours carry no context: one trampoline set per hooked module
constexpr size_t kMaxModules = 4;
NgxModule* g_slots[kMaxModules] = {};
template<size_t I> NVSDK_NGX_Result CreateThunk(ID3D12GraphicsCommandList* c, int f, NVSDK_NGX_Parameter* p, NVSDK_NGX_Handle** h) { return HookedCreate(*g_slots[I], c, f, p, h); }
template<size_t I> NVSDK_NGX_Result EvaluateThunk(ID3D12GraphicsCommandList* c, const NVSDK_NGX_Handle* h, const NVSDK_NGX_Parameter* p, void* cb) { return HookedEvaluate(*g_slots[I], c, h, p, cb); }
template<size_t I> NVSDK_NGX_Result ReleaseThunk(NVSDK_NGX_Handle* h) { return HookedRelease(*g_slots[I], h); }
const PFN_Create kCreateThunks[] = { CreateThunk<0>, CreateThunk<1>, CreateThunk<2>, CreateThunk<3> };
const PFN_Evaluate kEvaluateThunks[] = { EvaluateThunk<0>, EvaluateThunk<1>, EvaluateThunk<2>, EvaluateThunk<3> };
const PFN_Release kReleaseThunks[] = { ReleaseThunk<0>, ReleaseThunk<1>, ReleaseThunk<2>, ReleaseThunk<3> };

bool IsNgxModuleName(const std::wstring& lower)
{
    return lower == L"_nvngx.dll" || lower == L"nvngx.dll" || lower.rfind(L"nvngx.dll_", 0) == 0;
}

void HookModule(HMODULE module, const std::wstring& name)
{
    std::lock_guard<std::mutex> lock(g.mutex);
    for (const auto& m : g.modules) if (m->module == module) return;
    if (g.modules.size() >= kMaxModules) return;
    void* create = GetProcAddress(module, "NVSDK_NGX_D3D12_CreateFeature");
    void* evaluate = GetProcAddress(module, "NVSDK_NGX_D3D12_EvaluateFeature");
    void* release = GetProcAddress(module, "NVSDK_NGX_D3D12_ReleaseFeature");
    if (!create || !evaluate || !release) return;
    auto m = std::make_unique<NgxModule>();
    m->module = module; m->name = name;
    m->allocate = reinterpret_cast<PFN_AllocateParameters>(GetProcAddress(module, "NVSDK_NGX_D3D12_AllocateParameters"));
    m->destroy = reinterpret_cast<PFN_DestroyParameters>(GetProcAddress(module, "NVSDK_NGX_D3D12_DestroyParameters"));
    const size_t slot = g.modules.size();
    g_slots[slot] = m.get();
    if (MH_CreateHook(create, reinterpret_cast<LPVOID>(kCreateThunks[slot]), reinterpret_cast<LPVOID*>(&m->createOriginal)) != MH_OK ||
        MH_CreateHook(evaluate, reinterpret_cast<LPVOID>(kEvaluateThunks[slot]), reinterpret_cast<LPVOID*>(&m->evaluateOriginal)) != MH_OK ||
        MH_CreateHook(release, reinterpret_cast<LPVOID>(kReleaseThunks[slot]), reinterpret_cast<LPVOID*>(&m->releaseOriginal)) != MH_OK ||
        MH_EnableHook(MH_ALL_HOOKS) != MH_OK)
    {
        WARN("hooking %ls failed", name.c_str());
        g_slots[slot] = nullptr;
        return;
    }
    LOG("hooked %ls", name.c_str());
    g.modules.push_back(std::move(m));
}

std::wstring LowerName(const wchar_t* path)
{
    std::wstring n = std::filesystem::path(path).filename().wstring();
    for (auto& c : n) c = towlower(c);
    return n;
}

void ScanModules()
{
    HMODULE modules[1024];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) return;
    for (size_t i = 0; i < std::min<size_t>(needed / sizeof(HMODULE), 1024); ++i)
    {
        wchar_t path[MAX_PATH] = {};
        if (!GetModuleFileNameW(modules[i], path, MAX_PATH)) continue;
        const std::wstring name = LowerName(path);
        if (IsNgxModuleName(name)) HookModule(modules[i], name);
    }
}

HMODULE WINAPI HookedLoadLibraryExW(LPCWSTR fileName, HANDLE file, DWORD flags)
{
    HMODULE module = g.loadLibraryExW(fileName, file, flags);
    if (module && fileName)
    {
        const std::wstring name = LowerName(fileName);
        if (IsNgxModuleName(name)) HookModule(module, name);
    }
    return module;
}

unsigned g_presentCounter = 0;
void OnPresent(reshade::api::command_queue*, reshade::api::swapchain*, const reshade::api::rect*, const reshade::api::rect*, uint32_t, const reshade::api::rect*)
{
    ++g.presents;
    BuryDueFeatures(false);
    if ((++g_presentCounter % 30) == 0) ScanModules();
}
void OnInitDevice(reshade::api::device*) { ScanModules(); }
void OnOverlay(reshade::api::effect_runtime*)
{
    ImGui::Checkbox("Half-resolution NR + composite", &g.enabled);
    ImGui::Text("NR features intercepted: %llu, reduced evaluations: %llu", g.intercepted, g.evaluations);
    std::lock_guard<std::mutex> lock(g.mutex);
    for (const auto& kv : g.features)
        ImGui::Text("%ux%u -> %ux%u", kv.second->nativeW, kv.second->nativeH, kv.second->lowW, kv.second->lowH);
}
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
    {
        DisableThreadLibraryCalls(hModule);
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), path, MAX_PATH);
        g_logPath = (std::filesystem::path(path).parent_path() / L"dlssnr_composite.log").wstring();
        g_reshade = reshade::register_addon(hModule);   // false without ReShade: no overlay, always on
        LOG("dlssnr_composite loaded (%s)", g_reshade ? "ReShade addon" : "plain DLL / ASI");
        if (g_reshade)
        {
            reshade::register_event<reshade::addon_event::init_device>(OnInitDevice);
            reshade::register_event<reshade::addon_event::present>(OnPresent);
            reshade::register_overlay("DLSS-NR Composite", OnOverlay);
        }
        if (MH_Initialize() == MH_OK)
        {
            void* target = GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "LoadLibraryExW");
            if (target && MH_CreateHook(target, reinterpret_cast<LPVOID>(&HookedLoadLibraryExW), reinterpret_cast<LPVOID*>(&g.loadLibraryExW)) == MH_OK)
                MH_EnableHook(target);
        }
        ScanModules();
        break;
    }
    case DLL_PROCESS_DETACH:
        BuryDueFeatures(true);
        g.gpu.Release();
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        if (g_reshade) reshade::unregister_addon(hModule);
        break;
    }
    return TRUE;
}
