#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <d3d9on12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <vector>
#include "probe_driver_identity.h"
#include <cstdio>
#include <cstring>
using Microsoft::WRL::ComPtr;
#define CHECK(call) do { const HRESULT taskResult = (call); if (FAILED(taskResult)) { std::printf("%s failed: %08lx\n", #call, static_cast<unsigned long>(taskResult)); return 2; } } while (0)

static bool HasArg(int argc, char** argv, const char* expected) {
    for (int i = 1; i < argc; ++i) if (!std::strcmp(argv[i], expected)) return true;
    return false;
}

// Public D3D9 API probe: static stream 0 positions, system-memory colors in stream
// 1 or 2, and four draws that modify only color storage and the viewport. The
// first draw distinguishes skipped stream-2 upload from the later dirty-bit loss.
int main(int argc, char** argv) {
    const bool on12 = HasArg(argc, argv, "--on12");
    const bool writeOnly = HasArg(argc, argv, "--write-only");
    const bool validation = HasArg(argc, argv, "--debug");
    if (validation) {
        ComPtr<ID3D12Debug> debug;
        CHECK(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        debug->EnableDebugLayer();
    }
    if (on12 && !ProbeLocalDriver()) return 2;
    const UINT colorStream = HasArg(argc, argv, "--stream15") ? 15u : HasArg(argc, argv, "--stream2") ? 2u : 1u;
    const bool software = HasArg(argc, argv, "--software");
    const D3DPOOL colorPool = HasArg(argc, argv, "--default") ? D3DPOOL_DEFAULT :
        HasArg(argc, argv, "--managed") ? D3DPOOL_MANAGED : D3DPOOL_SYSTEMMEM;
    std::printf("renderer=%s colorStream=%u colorPool=%u software=%u\n", on12 ? "on12" : "native", colorStream, colorPool, software);
    wchar_t taskRuntimePath[MAX_PATH]{};
    GetSystemDirectoryW(taskRuntimePath, MAX_PATH);
    wcscat_s(taskRuntimePath, L"\\d3d9.dll");
    HMODULE runtime = LoadLibraryW(taskRuntimePath);
    if (!runtime) return 2;
    ComPtr<IDirect3D9> d3d;
    if (on12) {
        auto create = reinterpret_cast<PFN_Direct3DCreate9On12>(GetProcAddress(runtime, "Direct3DCreate9On12"));
        if (!create) return 2;
        D3D9ON12_ARGS args{}; args.Enable9On12 = TRUE;
        d3d.Attach(create(D3D_SDK_VERSION, &args, 1));
    } else {
        using Create = IDirect3D9* (WINAPI*)(UINT);
        auto create = reinterpret_cast<Create>(GetProcAddress(runtime, "Direct3DCreate9"));
        if (!create) return 2;
        d3d.Attach(create(D3D_SDK_VERSION));
    }
    if (!d3d) return 2;
    HWND window = CreateWindowW(L"STATIC", L"D3D9 multistream probe", WS_OVERLAPPED,
        0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window) return 2;
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE; pp.hDeviceWindow = window; pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = pp.BackBufferHeight = 64; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    ComPtr<IDirect3DDevice9> device;
    const DWORD flags = software ? D3DCREATE_SOFTWARE_VERTEXPROCESSING : D3DCREATE_HARDWARE_VERTEXPROCESSING;
    CHECK(d3d->CreateDevice(0, D3DDEVTYPE_HAL, window, flags, &pp, &device));
    wchar_t driverPath[MAX_PATH]{};
    if (GetModuleHandleW(L"d3d9on12.dll")) GetModuleFileNameW(GetModuleHandleW(L"d3d9on12.dll"), driverPath, MAX_PATH);
    std::printf("on12Module=%ls\n", driverPath);
    ComPtr<ID3D12InfoQueue> info;
    if (on12) {
        CHECK(ProbeRequireActualDriver(device.Get()));
        if (validation) {
            ComPtr<IDirect3DDevice9On12> interop; ComPtr<ID3D12Device> native;
            CHECK(device.As(&interop)); CHECK(interop->GetD3D12Device(IID_PPV_ARGS(&native)));
            CHECK(native.As(&info)); info->ClearStoredMessages();
        }
    }
    const char vsSource[] = "struct Out { float4 position:POSITION0; float4 color:COLOR0; }; Out main(float4 position:POSITION0,float4 color:COLOR0) { Out result;result.position=position;result.color=color;return result; }";
    const char psSource[] = "float4 main(float4 color:COLOR0):COLOR0{return color;}";
    ComPtr<ID3DBlob> vsCode, psCode, errors;
    HRESULT compiled = D3DCompile(vsSource, std::strlen(vsSource), nullptr, nullptr, nullptr, "main", "vs_3_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsCode, &errors);
    if (errors) std::printf("vs: %s\n", static_cast<const char*>(errors->GetBufferPointer()));
    CHECK(compiled); errors.Reset();
    compiled = D3DCompile(psSource, std::strlen(psSource), nullptr, nullptr, nullptr, "main", "ps_3_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psCode, &errors);
    if (errors) std::printf("ps: %s\n", static_cast<const char*>(errors->GetBufferPointer()));
    CHECK(compiled);
    ComPtr<IDirect3DVertexShader9> vs; ComPtr<IDirect3DPixelShader9> ps;
    CHECK(device->CreateVertexShader(static_cast<const DWORD*>(vsCode->GetBufferPointer()), &vs));
    CHECK(device->CreatePixelShader(static_cast<const DWORD*>(psCode->GetBufferPointer()), &ps));
    CHECK(device->SetVertexShader(vs.Get())); CHECK(device->SetPixelShader(ps.Get()));
    const D3DVERTEXELEMENT9 elements[] = {
        {0,0,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},
        {static_cast<WORD>(colorStream),0,D3DDECLTYPE_D3DCOLOR,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_COLOR,0}, D3DDECL_END()
    };
    ComPtr<IDirect3DVertexDeclaration9> decl;
    CHECK(device->CreateVertexDeclaration(elements, &decl)); CHECK(device->SetVertexDeclaration(decl.Get()));
    struct Position { float x,y,z,w; };
    const Position positions[] = {{-1,1,.5f,1},{1,1,.5f,1},{-1,-1,.5f,1},{1,-1,.5f,1}};
    const bool sharedBuffer = HasArg(argc, argv, "--shared-buffer");
    const bool views = HasArg(argc, argv, "--views") || sharedBuffer;
    const UINT colorStart = sharedBuffer ? sizeof(positions) : 0;
    ComPtr<IDirect3DVertexBuffer9> positionVB, colorVB;
    const DWORD positionUsage = (software ? D3DUSAGE_SOFTWAREPROCESSING : 0) | (writeOnly ? D3DUSAGE_WRITEONLY : 0);
    CHECK(device->CreateVertexBuffer(sizeof(positions) + (sharedBuffer ? 32 * sizeof(DWORD) : 0),
        positionUsage, 0, sharedBuffer ? colorPool : D3DPOOL_MANAGED, &positionVB, nullptr));
    void* mapped = nullptr;
    CHECK(positionVB->Lock(0, 0, &mapped, 0)); std::memcpy(mapped, positions, sizeof(positions)); CHECK(positionVB->Unlock());
    const DWORD colorUsage = positionUsage;
    const DWORD colors[] = {0xFF00FF00,0xFFFF0000,0xFF0000FF,0xFFFFFFFF};
    if (sharedBuffer) colorVB = positionVB;
    else CHECK(device->CreateVertexBuffer((views ? 32 : 4) * sizeof(DWORD), colorUsage, 0, colorPool, &colorVB, nullptr));
    if (views) {
        // Four immutable regions in one resource, with alternating packed and
        // padded vertices. Queued draws must retain both their offset and stride.
        CHECK(colorVB->Lock(0, 0, &mapped, 0));
        auto data = reinterpret_cast<DWORD*>(static_cast<BYTE*>(mapped) + colorStart);
        for (UINT i = 0; i < 32; ++i) data[i] = 0xFFFF00FF;
        for (UINT tile = 0; tile < 4; ++tile)
            for (UINT i = 0; i < 4; ++i) data[tile * 8 + i * (tile & 1 ? 2 : 1)] = colors[tile];
        CHECK(colorVB->Unlock());
    }
    CHECK(device->SetStreamSource(0, positionVB.Get(), 0, sizeof(Position)));
    CHECK(device->SetStreamSource(colorStream, colorVB.Get(), colorStart, sizeof(DWORD)));
    CHECK(device->SetRenderState(D3DRS_ZENABLE, FALSE)); CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
    CHECK(device->SetRenderState(D3DRS_LIGHTING, FALSE)); CHECK(device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE));
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1, 0)); CHECK(device->BeginScene());
    for (UINT tile = 0; tile < 4; ++tile) {
        if (!views) {
            CHECK(colorVB->Lock(0, 0, &mapped, 0));
            for (UINT i = 0; i < 4; ++i) static_cast<DWORD*>(mapped)[i] = colors[tile];
            CHECK(colorVB->Unlock());
        }
        D3DVIEWPORT9 viewport{tile*16,0,16,64,0.0f,1.0f}; CHECK(device->SetViewport(&viewport));
        // Repeating the same bind exercises driver-side equality filtering. It
        // must still reupload system-memory content every draw as required.
        if (HasArg(argc, argv, "--rebind"))
            CHECK(device->SetStreamSource(colorStream, nullptr, 0, 0));
        CHECK(device->SetStreamSource(colorStream, colorVB.Get(), colorStart + (views ? tile * 8 * sizeof(DWORD) : 0),
            sizeof(DWORD) * (views && (tile & 1) ? 2 : 1)));
        CHECK(device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2));
    }
    CHECK(device->EndScene());
    ComPtr<IDirect3DSurface9> target, readback;
    CHECK(device->GetRenderTarget(0, &target));
    CHECK(device->CreateOffscreenPlainSurface(64,64,D3DFMT_X8R8G8B8,D3DPOOL_SYSTEMMEM,&readback,nullptr));
    CHECK(device->GetRenderTargetData(target.Get(), readback.Get()));
    D3DLOCKED_RECT pixels{}; CHECK(readback->LockRect(&pixels,nullptr,D3DLOCK_READONLY));
    bool allMatches = true;
    for (UINT tile = 0; tile < 4; ++tile) {
        const DWORD pixel = *reinterpret_cast<DWORD*>(static_cast<BYTE*>(pixels.pBits) + 32 * pixels.Pitch + (tile*16+8)*4);
        const bool matches = (pixel & 0xFFFFFF) == (colors[tile] & 0xFFFFFF);
        allMatches &= matches;
        std::printf("tile%u actual=%08lx expected=%08lx %s\n", tile, static_cast<unsigned long>(pixel), static_cast<unsigned long>(colors[tile]), matches ? "PASS" : "FAIL");
    }
    CHECK(readback->UnlockRect());
    unsigned validationErrors = 0;
    if (info) {
        for (UINT64 i = 0; i < info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T size = 0; CHECK(info->GetMessage(i, nullptr, &size));
            std::vector<BYTE> storage(size);
            auto message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            CHECK(info->GetMessage(i, message, &size));
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                ++validationErrors; std::printf("D3D12 error %u: %s\n", message->ID, message->pDescription);
            }
        }
        std::printf("Validation errors=%u\n", validationErrors);
    }
    allMatches &= validationErrors == 0;
    DestroyWindow(window);
    std::printf("result=%s\n", allMatches ? "PASS" : "FAIL");
    return allMatches ? 0 : 1;
}
