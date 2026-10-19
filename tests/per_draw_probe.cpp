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
#define CHECK(call) do { const HRESULT hr = (call); if (FAILED(hr)) { \
    std::printf("%s failed: %08lx\n", #call, static_cast<unsigned long>(hr)); return 2; } } while (0)

int main(int argc, char** argv)
{
    bool native = false, validation = false, stressDescriptors = false;
    for (int i = 1; i < argc; ++i) {
        native |= !std::strcmp(argv[i], "--native");
        validation |= !std::strcmp(argv[i], "--debug");
        stressDescriptors |= !std::strcmp(argv[i], "--stress-descriptors");
    }
    if (validation) {
        ComPtr<ID3D12Debug> debug;
        CHECK(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        debug->EnableDebugLayer();
    }
    wchar_t path[MAX_PATH]{};
    if (!native && !ProbeLocalDriver()) return 2;
    GetSystemDirectoryW(path, MAX_PATH); wcscat_s(path, L"\\d3d9.dll");
    HMODULE runtime = LoadLibraryW(path);
    if (!runtime) return 2;
    ComPtr<IDirect3D9> d3d;
    if (native) {
        auto create = reinterpret_cast<IDirect3D9* (WINAPI*)(UINT)>(GetProcAddress(runtime, "Direct3DCreate9"));
        if (!create) return 2;
        d3d.Attach(create(D3D_SDK_VERSION));
    } else {
        auto create = reinterpret_cast<PFN_Direct3DCreate9On12>(GetProcAddress(runtime, "Direct3DCreate9On12"));
        if (!create) return 2;
        D3D9ON12_ARGS args{}; args.Enable9On12 = TRUE;
        d3d.Attach(create(D3D_SDK_VERSION, &args, 1));
    }
    if (!d3d) return 2;
    HWND window = CreateWindowW(L"STATIC", L"D3D9 per-draw probe", WS_OVERLAPPED,
        0, 0, 192, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE; pp.hDeviceWindow = window; pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = 192; pp.BackBufferHeight = 64; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    ComPtr<IDirect3DDevice9> device;
    CHECK(d3d->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device));
    
    if (!native) {
        CHECK(ProbeRequireActualDriver(device.Get()));
        std::puts("actual device driver identity PASS");
        GetModuleFileNameW(GetModuleHandleW(L"d3d9on12.dll"), path, MAX_PATH);
        std::printf("driver=%ls\n", path);
    }
    ComPtr<ID3D12InfoQueue> info;
    if (validation && !native) {
        ComPtr<IDirect3DDevice9On12> on12; ComPtr<ID3D12Device> device12;
        CHECK(device.As(&on12)); CHECK(on12->GetD3D12Device(IID_PPV_ARGS(&device12)));
        CHECK(device12.As(&info)); info->ClearStoredMessages();
    }
    // Both stages use all three app constant slots. The larger PS also uses c31;
    // switching back and forth tests pending writes beyond a shader's prefix.
    const char vsText[] =
        "vs_3_0\ndcl_position v0\ndcl_position o0\ndcl_color0 o1\n"
        "def c1,0,0,0,0\nmov o0,v0\nmov r0,c1\nif b0\n"
        "loop aL,i0\nadd r0,r0,c0\nendloop\nendif\nmov o1,r0\n";
    const char psSmallText[] =
        "ps_3_0\ndcl_color0 v0\ndef c1,0,0,0,0\nmov r0,c1\nif b0\n"
        "loop aL,i0\nadd r0,r0,c0\nendloop\nendif\nmul oC0,r0,v0\n";
    const char psLargeText[] =
        "ps_3_0\ndcl_color0 v0\ndef c1,0,0,0,0\nmov r0,c1\nif b0\n"
        "loop aL,i0\nadd r0,r0,c0\nendloop\nendif\nmul r0,r0,c31\nmul oC0,r0,v0\n";
    const char relativeVsText[] =
        "vs_3_0\ndcl_position v0\ndcl_position o0\ndcl_color0 o1\n"
        "def c2,1,0,0,0\nmova a0.x,c2.x\nmov o0,v0\nmov o1,c254[a0.x]\n";
    const char sampledPsText[] =
        "ps_3_0\ndcl_color0 v0\ndcl_2d s0\ntexld r0,v0,s0\nmov oC0,r0\n";
    ComPtr<ID3DBlob> vsCode, smallCode, largeCode, relativeCode, sampledCode, errors;
    using Assemble = HRESULT (WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, UINT, ID3DBlob**, ID3DBlob**);
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    auto assemble = compiler ? reinterpret_cast<Assemble>(GetProcAddress(compiler, "D3DAssemble")) : nullptr;
    if (!assemble) return 2;
    auto compile = [&](const char* text, ID3DBlob** code) {
        errors.Reset();
        HRESULT hr = assemble(text, std::strlen(text), nullptr, nullptr, nullptr, 0, code, &errors);
        if (errors) std::printf("%s\n", static_cast<const char*>(errors->GetBufferPointer()));
        return hr;
    };
    CHECK(compile(vsText, &vsCode));
    errors.Reset();
    CHECK(compile(psSmallText, &smallCode));
    errors.Reset();
    CHECK(compile(psLargeText, &largeCode));
    CHECK(compile(relativeVsText, &relativeCode));
    CHECK(compile(sampledPsText, &sampledCode));
    ComPtr<IDirect3DVertexShader9> vs, relativeVS;
    ComPtr<IDirect3DPixelShader9> psSmall, psLarge, psSampled;
    CHECK(device->CreateVertexShader(static_cast<const DWORD*>(vsCode->GetBufferPointer()), &vs));
    CHECK(device->CreateVertexShader(static_cast<const DWORD*>(relativeCode->GetBufferPointer()), &relativeVS));
    CHECK(device->CreatePixelShader(static_cast<const DWORD*>(smallCode->GetBufferPointer()), &psSmall));
    CHECK(device->CreatePixelShader(static_cast<const DWORD*>(largeCode->GetBufferPointer()), &psLarge));
    CHECK(device->CreatePixelShader(static_cast<const DWORD*>(sampledCode->GetBufferPointer()), &psSampled));
    CHECK(device->SetVertexShader(vs.Get())); CHECK(device->SetFVF(D3DFVF_XYZW));
    CHECK(device->SetRenderState(D3DRS_ZENABLE, FALSE));
    CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
    CHECK(device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE));
    struct Vertex { float x, y, z, w; };
    const Vertex vertices[] = {{-1,1,.5f,1},{1,1,.5f,1},{-1,-1,.5f,1},{1,-1,.5f,1}};
    ComPtr<IDirect3DVertexBuffer9> vertexBuffer;
    CHECK(device->CreateVertexBuffer(sizeof(vertices), 0, 0, D3DPOOL_MANAGED, &vertexBuffer, nullptr));
    void* mapped = nullptr;
    CHECK(vertexBuffer->Lock(0, 0, &mapped, 0));
    std::memcpy(mapped, vertices, sizeof(vertices));
    CHECK(vertexBuffer->Unlock());
    ComPtr<IDirect3DTexture9> texture;
    CHECK(device->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr));
    D3DLOCKED_RECT texturePixels{};
    CHECK(texture->LockRect(0, &texturePixels, nullptr, 0));
    *static_cast<DWORD*>(texturePixels.pBits) = 0xff00ff00;
    CHECK(texture->UnlockRect(0));
    CHECK(device->SetTexture(0, texture.Get()));
    CHECK(device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT));
    CHECK(device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
    CHECK(device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE));
    const float white[] = {1,1,1,1}, red[] = {1,0,0,1}, green[] = {0,1,0,1}, blue[] = {0,0,1,1};
    const int one[] = {1,0,1,0}, two[] = {2,0,1,0};
    const BOOL yes = TRUE, no = FALSE;
    CHECK(device->SetVertexShaderConstantF(0, white, 1));
    CHECK(device->SetVertexShaderConstantI(0, one, 1));
    CHECK(device->SetVertexShaderConstantB(0, &yes, 1));
    CHECK(device->SetPixelShaderConstantF(0, white, 1));
    CHECK(device->SetPixelShaderConstantF(31, red, 1));
    CHECK(device->SetPixelShaderConstantI(0, one, 1));
    CHECK(device->SetPixelShaderConstantB(0, &yes, 1));
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1, 0));
    CHECK(device->BeginScene());
    // Establish real SRV/sampler tables, then leave the same bindings unused
    // through ordinary shaders and (in stress mode) a descriptor-heap rollover.
    CHECK(device->SetPixelShader(psSampled.Get()));
    CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(Vertex)));
    
    const DWORD expected[] = {0xFFFFFF,0xFF0000,0xFFFFFF,0x00FF00,0x000000,0x0000FF,0xFFFFFF,0xFF0000,0x000000,stressDescriptors ? 0x00FF00u : 0xFFFFFFu,0x0000FF,0x00FF00};
    for (UINT tile = 0; tile < ARRAYSIZE(expected); ++tile) {
        CHECK(device->SetPixelShader((tile == 1 || tile == 3 || tile == 5 || tile >= 10) ? psLarge.Get() : psSmall.Get()));
        // c31 writes must survive psSmall shaders and subsequent used-range growth.
        if (tile == 2) CHECK(device->SetPixelShaderConstantF(31, green, 1));
        if (tile == 4) {
            CHECK(device->SetPixelShaderConstantF(31, blue, 1));
            CHECK(device->SetPixelShaderConstantB(0, &no, 1));
        }
        if (tile == 5) CHECK(device->SetPixelShaderConstantB(0, &yes, 1));
        if (tile == 7) {
            const float halfRed[] = {.5f,0,0,.5f};
            CHECK(device->SetPixelShaderConstantF(0, halfRed, 1));
            CHECK(device->SetPixelShaderConstantI(0, two, 1));
        }
        if (tile == 8) CHECK(device->SetVertexShaderConstantB(0, &no, 1));
        if (tile == 9) {
            const float half[] = {.5f,.5f,.5f,.5f};
            CHECK(device->SetPixelShaderConstantF(0, white, 1));
            CHECK(device->SetPixelShaderConstantI(0, one, 1));
            CHECK(device->SetVertexShaderConstantF(0, half, 1));
            CHECK(device->SetVertexShaderConstantI(0, two, 1));
            CHECK(device->SetVertexShaderConstantB(0, &yes, 1));
        }
        if (tile == 11) CHECK(device->SetPixelShaderConstantF(31, green, 1));
        D3DVIEWPORT9 viewport{tile * 16,0,16,64,0,1}; CHECK(device->SetViewport(&viewport));
        // Alternate runtime-provided upload data and an ordinary persistent VB.
        // Constant/state packets must remain ordered across both draw paths.
        if (tile == 6 || tile == 10) {
            // Point expansion activates the generated GS, then ordinary
            // triangles deactivate it. A later point must restore its bindings.
            const float pointSize = 64.0f;
            DWORD pointSizeBits = 0; std::memcpy(&pointSizeBits, &pointSize, sizeof(pointSize));
            CHECK(device->SetRenderState(D3DRS_POINTSIZE, pointSizeBits));
            CHECK(device->SetRenderState(D3DRS_POINTSPRITEENABLE, TRUE));
            const Vertex center{0,0,.5f,1};
            CHECK(device->DrawPrimitiveUP(D3DPT_POINTLIST, 1, &center, sizeof(Vertex)));
            CHECK(device->SetRenderState(D3DRS_POINTSPRITEENABLE, FALSE));
        } else if (tile & 1) {
            CHECK(device->SetStreamSource(0, vertexBuffer.Get(), 0, sizeof(Vertex)));
            CHECK(device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2));
        } else {
            CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(Vertex)));
        }
        if (tile == 9 && stressDescriptors) {
            // Change an integer CB as well as CB0: this exhausts the view heap
            // even if CB0 uses a root descriptor and the remaining table has
            // only three entries. Tile 10 then reactivates GS with unchanged data.
            for (UINT draw = 0; draw < 400000; ++draw) {
                CHECK(device->SetPixelShaderConstantF(0, draw & 1 ? white : red, 1));
                CHECK(device->SetPixelShaderConstantI(0, draw & 1 ? one : two, 1));
                CHECK(device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2));
            }
            // Only CB0 changes here. No viewport, VB, shader or table-tail
            // update may be needed to make this green draw reach the GPU.
            CHECK(device->SetPixelShaderConstantF(0, green, 1));
            CHECK(device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2));
            CHECK(device->SetPixelShaderConstantF(0, white, 1));
        }
    }
    // A wide write changes only c255. The short shader must retain its prefix,
    // and a later relative shader must see the tail after the used range grows.
    float fullVertexConstants[256 * 4];
    CHECK(device->GetVertexShaderConstantF(0, fullVertexConstants, 256));
    std::memcpy(fullVertexConstants + 255 * 4, white, sizeof(float) * 4);
    CHECK(device->SetVertexShaderConstantF(0, fullVertexConstants, 256));
    D3DVIEWPORT9 prefixViewport{96,0,16,64,0,1}; CHECK(device->SetViewport(&prefixViewport));
    CHECK(device->SetPixelShader(psSmall.Get()));
    CHECK(device->SetStreamSource(0, vertexBuffer.Get(), 0, sizeof(Vertex)));
    CHECK(device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2));
    // Grow VS CB0 from a short prefix to all 256 registers, then dynamically
    // address the last register. Its white VS output must preserve the red PS.
    CHECK(device->SetVertexShader(relativeVS.Get()));
    D3DVIEWPORT9 relativeViewport{176,0,16,64,0,1}; CHECK(device->SetViewport(&relativeViewport));
    CHECK(device->SetPixelShader(psLarge.Get()));
    CHECK(device->SetPixelShaderConstantF(31, red, 1));
    CHECK(device->SetStreamSource(0, vertexBuffer.Get(), 0, sizeof(Vertex)));
    CHECK(device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2));
    // Leave the left half red to verify the relative VS load, and overwrite
    // the right half green through SRV/sampler bindings that stayed unchanged.
    // Their bucket sizes match those of the resource-free shaders.
    D3DVIEWPORT9 sampledViewport{184,0,8,64,0,1}; CHECK(device->SetViewport(&sampledViewport));
    CHECK(device->SetPixelShader(psSampled.Get()));
    CHECK(device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2));
    // c15 lies inside the large PS prefix but is a hole in its read mask.
    // A later shader with a shorter prefix must still observe the pending write.
    ComPtr<ID3DBlob> holeCode;
    ComPtr<IDirect3DPixelShader9> holePS;
    CHECK(compile("ps_3_0\nmov oC0,c15\n", &holeCode));
    CHECK(device->CreatePixelShader(static_cast<const DWORD*>(holeCode->GetBufferPointer()), &holePS));
    D3DVIEWPORT9 holeViewport{0,0,8,64,0,1}; CHECK(device->SetViewport(&holeViewport));
    CHECK(device->SetPixelShader(psLarge.Get()));
    CHECK(device->SetPixelShaderConstantF(15, blue, 1));
    CHECK(device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2));
    CHECK(device->SetPixelShader(holePS.Get()));
    CHECK(device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2));
    // Revisit texture/sampler combinations, then change descriptor contents
    // through DISCARD and object destruction. Render above the existing tiles.
    ComPtr<IDirect3DTexture9> changingTextures[4];
    const DWORD initialColors[] = {0xffff0000,0xff00ff00,0xff0000ff,0xffffffff};
    for (UINT i = 0; i < 4; ++i) {
        CHECK(device->CreateTexture(1,1,1,D3DUSAGE_DYNAMIC,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&changingTextures[i],nullptr));
        D3DLOCKED_RECT data{}; CHECK(changingTextures[i]->LockRect(0,&data,nullptr,D3DLOCK_DISCARD));
        *static_cast<DWORD*>(data.pBits) = initialColors[i];
        CHECK(changingTextures[i]->UnlockRect(0));
    }
    CHECK(device->SetPixelShader(psSampled.Get()));
    for (UINT repeat = 0; repeat < 64; ++repeat) for (UINT i = 0; i < 4; ++i) {
        D3DVIEWPORT9 top{48*i,0,48,16,0,1}; CHECK(device->SetViewport(&top));
        CHECK(device->SetTexture(0,changingTextures[i].Get()));
        CHECK(device->SetSamplerState(0,D3DSAMP_ADDRESSU,i & 1 ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP));
        CHECK(device->SetSamplerState(0,D3DSAMP_ADDRESSV,i & 1 ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP));
        CHECK(device->DrawPrimitive(D3DPT_TRIANGLESTRIP,0,2));
    }
    CHECK(device->SetTexture(0,changingTextures[0].Get()));
    D3DLOCKED_RECT renamed{}; CHECK(changingTextures[0]->LockRect(0,&renamed,nullptr,D3DLOCK_DISCARD));
    *static_cast<DWORD*>(renamed.pBits) = 0xffffff00;
    CHECK(changingTextures[0]->UnlockRect(0));
    D3DVIEWPORT9 topFirst{0,0,48,16,0,1}; CHECK(device->SetViewport(&topFirst));
    CHECK(device->DrawPrimitive(D3DPT_TRIANGLESTRIP,0,2));
    changingTextures[2].Reset();
    CHECK(device->CreateTexture(1,1,1,D3DUSAGE_DYNAMIC,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&changingTextures[2],nullptr));
    CHECK(changingTextures[2]->LockRect(0,&renamed,nullptr,D3DLOCK_DISCARD));
    *static_cast<DWORD*>(renamed.pBits) = 0xffff00ff;
    CHECK(changingTextures[2]->UnlockRect(0));
    CHECK(device->SetTexture(0,changingTextures[2].Get()));
    D3DVIEWPORT9 topThird{96,0,48,16,0,1}; CHECK(device->SetViewport(&topThird));
    CHECK(device->DrawPrimitive(D3DPT_TRIANGLESTRIP,0,2));
    // UV=(1,1): point wrap reads top-left red, clamp reads bottom-right blue.
    ComPtr<IDirect3DTexture9> addressing;
    CHECK(device->CreateTexture(2,2,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&addressing,nullptr));
    CHECK(addressing->LockRect(0,&renamed,nullptr,0));
    auto row = static_cast<BYTE*>(renamed.pBits);
    reinterpret_cast<DWORD*>(row)[0]=0xffff0000; reinterpret_cast<DWORD*>(row)[1]=0xff00ff00;
    reinterpret_cast<DWORD*>(row+renamed.Pitch)[0]=0xffffffff; reinterpret_cast<DWORD*>(row+renamed.Pitch)[1]=0xff0000ff;
    CHECK(addressing->UnlockRect(0));
    CHECK(device->SetTexture(0,addressing.Get()));
    D3DVIEWPORT9 topFourth{144,0,48,16,0,1}; CHECK(device->SetViewport(&topFourth));
    for (UINT repeat=0; repeat<12; ++repeat) {
        CHECK(device->SetSamplerState(0,D3DSAMP_ADDRESSU,repeat & 1 ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP));
        CHECK(device->SetSamplerState(0,D3DSAMP_ADDRESSV,repeat & 1 ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP));
        CHECK(device->DrawPrimitive(D3DPT_TRIANGLESTRIP,0,2));
    }
    CHECK(device->EndScene());
    ComPtr<IDirect3DSurface9> target, readback;
    CHECK(device->GetRenderTarget(0, &target));
    CHECK(device->CreateOffscreenPlainSurface(192,64,D3DFMT_X8R8G8B8,D3DPOOL_SYSTEMMEM,&readback,nullptr));
    CHECK(device->GetRenderTargetData(target.Get(), readback.Get()));
    D3DLOCKED_RECT pixels{}; CHECK(readback->LockRect(&pixels,nullptr,D3DLOCK_READONLY));
    bool matches = true;
    const DWORD expectedTop[] = {0xffff00,0x00ff00,0xff00ff,0x0000ff};
    for (UINT i=0; i<4; ++i) {
        const DWORD pixel = *reinterpret_cast<DWORD*>(static_cast<BYTE*>(pixels.pBits)+8*pixels.Pitch+(48*i+24)*4)&0xffffff;
        matches &= pixel==expectedTop[i];
        std::printf("descriptor reuse tile%u pixel=%06lx expected=%06lx %s\n",i,pixel,expectedTop[i],pixel==expectedTop[i]?"PASS":"FAIL");
    }
    const DWORD holePixel = *reinterpret_cast<DWORD*>(static_cast<BYTE*>(pixels.pBits) + 32 * pixels.Pitch + 4 * 4) & 0xffffff;
    matches &= holePixel == 0x0000ff;
    std::printf("pending PS hole pixel=%06lx expected=0000ff %s\n", holePixel, holePixel == 0x0000ff ? "PASS" : "FAIL");
    const DWORD relativePixel = *reinterpret_cast<DWORD*>(static_cast<BYTE*>(pixels.pBits) + 32 * pixels.Pitch + 180 * 4) & 0xffffff;
    matches &= relativePixel == 0xff0000;
    std::printf("relative VS pixel=%06lx expected=ff0000 %s\n", relativePixel, relativePixel == 0xff0000 ? "PASS" : "FAIL");
    for (UINT tile = 0; tile < ARRAYSIZE(expected); ++tile) {
        DWORD pixel = *reinterpret_cast<DWORD*>(static_cast<BYTE*>(pixels.pBits) + 32 * pixels.Pitch + (tile * 16 + 8) * 4) & 0xFFFFFF;
        const bool pass = pixel == expected[tile]; matches &= pass;
        std::printf("constant tile%u pixel=%06lx expected=%06lx %s\n", tile, pixel, expected[tile], pass ? "PASS" : "FAIL");
    }
    CHECK(readback->UnlockRect());
    UINT validationErrors = 0;
    if (info) {
        for (UINT64 i = 0; i < info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T size = 0; CHECK(info->GetMessage(i, nullptr, &size));
            auto storage = std::make_unique<BYTE[]>(size);
            auto message = reinterpret_cast<D3D12_MESSAGE*>(storage.get());
            CHECK(info->GetMessage(i, message, &size));
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                ++validationErrors; std::printf("D3D12 error %u: %s\n", message->ID, message->pDescription);
            }
        }
        std::printf("Validation errors=%u\n", validationErrors);
    }
    
    DestroyWindow(window);
    return matches && !validationErrors ? 0 : 1;
}
