// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <d3d9on12.h>
#include <d3dcompiler.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <memory>
#include "probe_driver_identity.h"
using Microsoft::WRL::ComPtr;
#define CHECK(call) do { HRESULT result = (call); if (FAILED(result)) { \
    std::printf("%s failed: %08lx\n", #call, static_cast<unsigned long>(result)); return 1; } } while (0)

// A sparse pixel sampler declaration requires valid descriptors for the entire
// declared table, including samplers the application has never explicitly set.
int main(int argc, char** argv)
{
    bool validation = false, bindingRuns = false, zeroAddress = false, largeUploads = false;
    for (int i = 1; i < argc; ++i) {
        validation |= std::strcmp(argv[i], "--debug") == 0;
        bindingRuns |= std::strcmp(argv[i], "--binding-runs") == 0;
        zeroAddress |= std::strcmp(argv[i], "--zero-address") == 0;
        largeUploads |= std::strcmp(argv[i], "--large-upload-offsets") == 0;
    }
    if (validation)
    {
        ComPtr<ID3D12Debug> debug;
        CHECK(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        debug->EnableDebugLayer();
    }
    wchar_t path[MAX_PATH]{};
    if (!ProbeLocalDriver()) return 1;
    GetSystemDirectoryW(path, MAX_PATH);
    wcscat_s(path, L"\\d3d9.dll");
    HMODULE runtime = LoadLibraryW(path);
    auto create = runtime ? reinterpret_cast<PFN_Direct3DCreate9On12>(GetProcAddress(runtime, "Direct3DCreate9On12")) : nullptr;
    if (!create) return 1;
    D3D9ON12_ARGS args{};
    args.Enable9On12 = TRUE;
    ComPtr<IDirect3D9> d3d;
    d3d.Attach(create(D3D_SDK_VERSION, &args, 1));
    if (!d3d) return 1;
    HWND window = CreateWindowW(L"STATIC", L"D3D9On12 sampler probe", WS_OVERLAPPED,
        0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.hDeviceWindow = window;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = pp.BackBufferHeight = 64;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    ComPtr<IDirect3DDevice9> device;
    CHECK(d3d->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device));
    CHECK(ProbeRequireActualDriver(device.Get()));
    GetModuleFileNameW(GetModuleHandleW(L"d3d9on12.dll"), path, MAX_PATH);
    std::printf("driver=%ls\n", path);
    ComPtr<ID3D12InfoQueue> info;
    if (validation)
    {
        ComPtr<IDirect3DDevice9On12> on12;
        ComPtr<ID3D12Device> device12;
        CHECK(device.As(&on12));
        CHECK(on12->GetD3D12Device(IID_PPV_ARGS(&device12)));
        CHECK(device12.As(&info));
        info->ClearStoredMessages();
    }
    const char source[] = "sampler2D image:register(s15); float4 tint:register(c0); float4 main(float2 uv:TEXCOORD0):COLOR0 { return tex2D(image,uv)*tint; }";
    const char runsSource[] = "sampler2D a:register(s0); sampler2D b:register(s1); sampler2D c:register(s3); sampler2D d:register(s15); float4 tint:register(c0); float4 main(float2 uv:TEXCOORD0):COLOR0 { return (tex2D(a,uv)+tex2D(b,uv)+tex2D(c,uv)+tex2D(d,uv))*.25*tint; }";
    const char* shaderSource = bindingRuns ? runsSource : source;
    ComPtr<ID3DBlob> code, errors;
    HRESULT compiled = D3DCompile(shaderSource, std::strlen(shaderSource), nullptr, nullptr, nullptr,
        "main", "ps_3_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (errors) std::printf("%s\n", static_cast<const char*>(errors->GetBufferPointer()));
    CHECK(compiled);
    ComPtr<IDirect3DPixelShader9> shader;
    CHECK(device->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &shader));
    ComPtr<IDirect3DTexture9> texture;
    CHECK(device->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr));
    D3DLOCKED_RECT texels{};
    CHECK(texture->LockRect(0, &texels, nullptr, 0));
    *static_cast<DWORD*>(texels.pBits) = 0xFF804020;
    CHECK(texture->UnlockRect(0));
    CHECK(device->SetTexture(15, texture.Get()));
    if (bindingRuns) {
        const DWORD slots[] = {0, 1, 3};
        for (DWORD slot : slots) CHECK(device->SetTexture(slot, texture.Get()));
    }
    CHECK(device->SetSamplerState(15, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP));
    CHECK(device->SetSamplerState(15, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));
    // Some games restore a zero address mode. The driver's existing invalid-value
    // fallback must include zero as well as values above MIRRORONCE.
    if (zeroAddress)
        CHECK(device->SetSamplerState(15, D3DSAMP_ADDRESSW, 0));
    // Otherwise AddressW and every unused sampler retain their D3D9 defaults.
    CHECK(device->SetPixelShader(shader.Get()));
    CHECK(device->SetVertexShader(nullptr));
    CHECK(device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1));
    CHECK(device->SetRenderState(D3DRS_ZENABLE, FALSE));
    CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
    CHECK(device->SetRenderState(D3DRS_LIGHTING, FALSE));
    CHECK(device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE));
    struct Vertex { float x, y, z, rhw, u, v; };
    ComPtr<IDirect3DTexture9> uploadTextures[320];
    const UINT selected[] = {0, 31, 128, 255, 319};
    auto uploadColor = [](UINT i) { return 0xFF000000u | (((i + 1) * 0x019D2Du) & 0xFFFFFFu); };
    if (largeUploads)
    {
        // Queue enough independent 64 KiB texture uploads to cross both the
        // former heap boundaries and a 16 MiB chunk. Sample early and late sources
        // after all writes, so incorrect suballocation offsets cannot alias.
        for (UINT i = 0; i < 320; ++i)
        {
            CHECK(device->CreateTexture(128, 128, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8,
                D3DPOOL_DEFAULT, &uploadTextures[i], nullptr));
            D3DLOCKED_RECT data{};
            CHECK(uploadTextures[i]->LockRect(0, &data, nullptr, D3DLOCK_DISCARD));
            for (UINT y = 0; y < 128; ++y)
            {
                auto row = reinterpret_cast<DWORD*>(static_cast<BYTE*>(data.pBits) + y * data.Pitch);
                for (UINT x = 0; x < 128; ++x) row[x] = uploadColor(i);
            }
            CHECK(uploadTextures[i]->UnlockRect(0));
        }
    }
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1, 0));
    CHECK(device->BeginScene());
    // Repeated and changed constants must keep earlier GPU draws intact without
    // an intervening readback. This also catches unsafe reuse of upload storage.
    const float tints[][4] = { {1,1,1,1}, {1,1,1,1}, {1,0,0,1}, {1,0,0,1}, {1,1,1,1} };
    for (UINT i = 0; i < 5; ++i)
    {
        if (largeUploads) CHECK(device->SetTexture(15, uploadTextures[selected[i]].Get()));
        if (bindingRuns) {
            // Exercise a contiguous pair, a gap, a sparse high slot and partial
            // updates that must preserve the other bound descriptors.
            DWORD slot = i == 1 ? 1u : i == 2 ? 3u : i == 3 ? 15u : 0u;
            CHECK(device->SetTexture(slot, nullptr));
            CHECK(device->SetTexture(slot, texture.Get()));
            CHECK(device->SetSamplerState(slot, D3DSAMP_ADDRESSU, i & 1 ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP));
        }
        const float left = float(i * 12) - .5f, right = float((i + 1) * 12) - .5f;
        const Vertex vertices[] = { {left,-.5f,.5f,1,0,0}, {right,-.5f,.5f,1,1,0},
            {left,63.5f,.5f,1,0,1}, {right,63.5f,.5f,1,1,1} };
        CHECK(device->SetPixelShaderConstantF(0, tints[i], 1));
        CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(Vertex)));
    }
    CHECK(device->EndScene());
    ComPtr<IDirect3DSurface9> target, readback;
    CHECK(device->GetRenderTarget(0, &target));
    CHECK(device->CreateOffscreenPlainSurface(64, 64, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr));
    CHECK(device->GetRenderTargetData(target.Get(), readback.Get()));
    D3DLOCKED_RECT pixels{};
    CHECK(readback->LockRect(&pixels, nullptr, D3DLOCK_READONLY));
    bool matches = true;
    for (UINT i = 0; i < 5; ++i)
    {
        DWORD pixel = *reinterpret_cast<DWORD*>(static_cast<BYTE*>(pixels.pBits) + 32 * pixels.Pitch + (i * 12 + 6) * sizeof(DWORD));
        DWORD expected = largeUploads ? (uploadColor(selected[i]) & 0xFFFFFF) : 0x804020;
        if (i == 2 || i == 3) expected &= 0xFF0000;
        bool correct = (pixel & 0xFFFFFF) == expected;
        matches = matches && correct;
        std::printf("Sparse sampler/constant tile%u pixel=%08lx %s\n", i,
            static_cast<unsigned long>(pixel), correct ? "PASS" : "FAIL");
    }
    CHECK(readback->UnlockRect());
    UINT validationErrors = 0;
    if (info)
    {
        for (UINT64 i = 0; i < info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i)
        {
            SIZE_T size = 0;
            CHECK(info->GetMessage(i, nullptr, &size));
            auto storage = std::make_unique<BYTE[]>(size);
            auto message = reinterpret_cast<D3D12_MESSAGE*>(storage.get());
            CHECK(info->GetMessage(i, message, &size));
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
            {
                ++validationErrors;
                std::printf("D3D12 error %u: %s\n", message->ID, message->pDescription);
            }
        }
        std::printf("Validation errors=%u\n", validationErrors);
    }
    DestroyWindow(window);
    return matches && validationErrors == 0 ? 0 : 1;
}
