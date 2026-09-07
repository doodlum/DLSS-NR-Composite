#pragma once

// Minimal NGX declarations used by the direct D3D12 path. The declaration order is
// ABI-significant: it mirrors nvsdk_ngx.h so the virtual parameter interface has
// the same layout without requiring the Streamline or NGX SDK packages.

#include <d3d11.h>
#include <d3d12.h>

using NVSDK_NGX_Result = int;
constexpr NVSDK_NGX_Result kNgxSuccess = 1;

struct NVSDK_NGX_PathListInfo
{
    const wchar_t* const* Path;
    unsigned int Length;
};

using NVSDK_NGX_AppLogCallback = void(*)(const char*, int, int);
struct NVSDK_NGX_LoggingInfo
{
    NVSDK_NGX_AppLogCallback LoggingCallback;
    int MinimumLoggingLevel;
    bool DisableOtherLoggingSinks;
};

struct NVSDK_NGX_FeatureCommonInfo
{
    NVSDK_NGX_PathListInfo PathListInfo;
    void* InternalData;
    NVSDK_NGX_LoggingInfo LoggingInfo;
};

struct NVSDK_NGX_Handle
{
    unsigned int Id;
};

struct NVSDK_NGX_Parameter
{
    virtual void Set(const char* name, unsigned long long value) = 0;
    virtual void Set(const char* name, float value) = 0;
    virtual void Set(const char* name, double value) = 0;
    virtual void Set(const char* name, unsigned int value) = 0;
    virtual void Set(const char* name, int value) = 0;
    virtual void Set(const char* name, ID3D11Resource* value) = 0;
    virtual void Set(const char* name, ID3D12Resource* value) = 0;
    virtual void Set(const char* name, void* value) = 0;

    virtual NVSDK_NGX_Result Get(const char* name, unsigned long long* value) const = 0;
    virtual NVSDK_NGX_Result Get(const char* name, float* value) const = 0;
    virtual NVSDK_NGX_Result Get(const char* name, double* value) const = 0;
    virtual NVSDK_NGX_Result Get(const char* name, unsigned int* value) const = 0;
    virtual NVSDK_NGX_Result Get(const char* name, int* value) const = 0;
    virtual NVSDK_NGX_Result Get(const char* name, ID3D11Resource** value) const = 0;
    virtual NVSDK_NGX_Result Get(const char* name, ID3D12Resource** value) const = 0;
    virtual NVSDK_NGX_Result Get(const char* name, void** value) const = 0;

    virtual void Reset() = 0;
};

using PFN_NGX_D3D12_InitExt = NVSDK_NGX_Result(*)(unsigned long long, const wchar_t*, ID3D12Device*, int, const void*);
using PFN_NGX_D3D12_GetParameters = NVSDK_NGX_Result(*)(NVSDK_NGX_Parameter**);
using PFN_NGX_D3D12_AllocateParameters = NVSDK_NGX_Result(*)(NVSDK_NGX_Parameter**);
using PFN_NGX_D3D12_DestroyParameters = NVSDK_NGX_Result(*)(NVSDK_NGX_Parameter*);
using PFN_NGX_D3D12_CreateFeature = NVSDK_NGX_Result(*)(ID3D12GraphicsCommandList*, int, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using PFN_NGX_D3D12_EvaluateFeature = NVSDK_NGX_Result(*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, void*);
using PFN_NGX_D3D12_ReleaseFeature = NVSDK_NGX_Result(*)(NVSDK_NGX_Handle*);
using PFN_NGX_D3D12_Shutdown1 = NVSDK_NGX_Result(*)(ID3D12Device*);

inline const char* NgxResultName(NVSDK_NGX_Result result)
{
    switch (static_cast<unsigned int>(result))
    {
    case 0x00000001: return "Success";
    case 0xBAD00001: return "FeatureNotSupported";
    case 0xBAD00002: return "PlatformError";
    case 0xBAD00003: return "FeatureAlreadyExists";
    case 0xBAD00004: return "FeatureNotFound";
    case 0xBAD00005: return "InvalidParameter";
    case 0xBAD00006: return "ScratchBufferTooSmall";
    case 0xBAD00007: return "NotInitialized";
    case 0xBAD00008: return "UnsupportedInputFormat";
    case 0xBAD00009: return "RWFlagMissing";
    case 0xBAD0000A: return "MissingInput";
    case 0xBAD0000B: return "UnableToInitializeFeature";
    case 0xBAD0000C: return "OutOfDate";
    case 0xBAD0000D: return "OutOfGPUMemory";
    case 0xBAD0000E: return "UnsupportedFormat";
    case 0xBAD0000F: return "UnableToWriteToAppDataPath";
    case 0xBAD00010: return "UnsupportedParameter";
    case 0xBAD00011: return "Denied";
    case 0xBAD00012: return "NotImplemented";
    default: return "Unknown";
    }
}
