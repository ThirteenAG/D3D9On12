// Public D3D9 regression for asynchronous draw/query ordering and query lifetime.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <d3d9on12.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
using Microsoft::WRL::ComPtr;
#define CHECK(call) do { HRESULT result = (call); if (FAILED(result)) { \
    std::printf("%s failed: %08lx\n", #call, static_cast<unsigned long>(result)); return 1; } } while (0)

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    auto argument = [&](const char* value)
    { for (int i = 1; i < argc; ++i) if (!strcmp(argv[i], value)) return true; return false; };
    bool native = argument("--native"), debug = argument("--debug");
    const bool abandonBegunQuery = !argument("--no-abandon");
    if (debug && !native)
    {
        ComPtr<ID3D12Debug> layer;
        CHECK(D3D12GetDebugInterface(IID_PPV_ARGS(&layer)));
        layer->EnableDebugLayer();
    }
    wchar_t path[MAX_PATH]{};
    GetSystemDirectoryW(path, MAX_PATH);
    wcscat_s(path, L"\\d3d9.dll");
    HMODULE runtime = LoadLibraryW(path);
    ComPtr<IDirect3D9> d3d;
    if (native)
    {
        auto create = reinterpret_cast<IDirect3D9* (WINAPI*)(UINT)>(GetProcAddress(runtime, "Direct3DCreate9"));
        if (!create) return 1;
        d3d.Attach(create(D3D_SDK_VERSION));
    }
    else
    {
        auto create = reinterpret_cast<PFN_Direct3DCreate9On12>(GetProcAddress(runtime, "Direct3DCreate9On12"));
        if (!create) return 1;
        D3D9ON12_ARGS args{}; args.Enable9On12 = TRUE;
        d3d.Attach(create(D3D_SDK_VERSION, &args, 1));
    }
    if (!d3d) return 1;
    HWND window = CreateWindowW(L"STATIC", L"D3D9On12 query ordering", WS_OVERLAPPED,
        0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE; pp.hDeviceWindow = window; pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = pp.BackBufferHeight = 64; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    ComPtr<IDirect3DDevice9> device;
    CHECK(d3d->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device));
    GetModuleFileNameW(GetModuleHandleW(L"d3d9on12.dll"), path, MAX_PATH);
    std::printf("native=%u abandon=%u driver=%ls\n", native, abandonBegunQuery, native ? L"system D3D9" : path);
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12InfoQueue> info;
    if (!native)
    {
        ComPtr<IDirect3DDevice9On12> interop;
        CHECK(device.As(&interop));
        CHECK(interop->GetD3D12Device(IID_PPV_ARGS(&device12)));
        if (debug) { CHECK(device12.As(&info)); info->ClearStoredMessages(); }
    }
    CHECK(device->SetVertexShader(nullptr)); CHECK(device->SetPixelShader(nullptr));
    CHECK(device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE));
    CHECK(device->SetTexture(0, nullptr));
    CHECK(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
    CHECK(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE));
    CHECK(device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1));
    CHECK(device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE));
    CHECK(device->SetRenderState(D3DRS_ZENABLE, FALSE));
    CHECK(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
    CHECK(device->SetRenderState(D3DRS_LIGHTING, FALSE));
    CHECK(device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE));
    struct Vertex { float x, y, z, rhw; DWORD color; };
    const Vertex vertices[] = { {-.5f,-.5f,.5f,1,0xff804020}, {63.5f,-.5f,.5f,1,0xff804020},
        {-.5f,63.5f,.5f,1,0xff804020}, {63.5f,63.5f,.5f,1,0xff804020} };
    struct Pair { ComPtr<IDirect3DQuery9> zero, visible; };
    std::vector<Pair> queries(128);
    for (auto& pair : queries)
    {
        CHECK(device->CreateQuery(D3DQUERYTYPE_OCCLUSION, &pair.zero));
        CHECK(device->CreateQuery(D3DQUERYTYPE_OCCLUSION, &pair.visible));
    }
    ComPtr<IDirect3DQuery9> discarded;
    CHECK(device->CreateQuery(D3DQUERYTYPE_OCCLUSION, &discarded));
    CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1, 0));
    CHECK(device->BeginScene());
    constexpr UINT outsideDraws = 23, insideDraws = 7;
    // No readback, Present, or explicit flush between the batches. Zero queries
    // must exclude the neighboring queued draws; visible queries must include all 7.
    for (auto& pair : queries)
    {
        for (UINT i = 0; i < outsideDraws; ++i)
            CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(Vertex)));
        CHECK(pair.zero->Issue(D3DISSUE_BEGIN)); CHECK(pair.zero->Issue(D3DISSUE_END));
        CHECK(pair.visible->Issue(D3DISSUE_BEGIN));
        for (UINT i = 0; i < insideDraws; ++i)
            CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(Vertex)));
        CHECK(pair.visible->Issue(D3DISSUE_END));
    }
    std::printf("occlusion scopes queued\n");
    // --no-abandon retains ordering/readback coverage while isolating the
    // release-without-End trigger from the other query and temporary-IB paths.
    if (abandonBegunQuery)
    {
        CHECK(discarded->Issue(D3DISSUE_BEGIN));
        for (UINT i = 0; i < outsideDraws; ++i)
            CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(Vertex)));
        discarded.Reset();
        std::printf("begun query discarded\n");
    }
    else
    {
        discarded.Reset();
        std::printf("begun query discard skipped\n");
    }
    // Indexed triangle fans allocate a temporary converted IB whose last CPU ref
    // may disappear before the asynchronous draw executes.
    const Vertex fan[] = { vertices[0], vertices[1], vertices[3], vertices[2] };
    const WORD indices[] = { 0, 1, 2, 3 };
    if (!argument("--no-fans")) for (UINT i = 0; i < 64; ++i)
        CHECK(device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLEFAN, 0, 4, 2, indices,
            D3DFMT_INDEX16, fan, sizeof(Vertex)));
    CHECK(device->EndScene());
    std::printf("indexed triangle fans queued\n");
    UINT failures = 0;
    auto result = [&](IDirect3DQuery9* query, DWORD expected, UINT index, const char* kind)
    {
        DWORD samples = 0xdeadbeef;
        ULONGLONG deadline = GetTickCount64() + 10000;
        HRESULT hr;
        do { hr = query->GetData(&samples, sizeof(samples), D3DGETDATA_FLUSH);
            if (hr == S_FALSE) Sleep(0); } while (hr == S_FALSE && GetTickCount64() < deadline);
        if (hr != S_OK || samples != expected)
        {
            std::printf("query[%u] %s hr=%08lx samples=%lu expected=%lu FAIL\n", index, kind,
                static_cast<unsigned long>(hr), samples, expected);
            ++failures;
        }
    };
    for (UINT i = 0; i < queries.size(); ++i)
    {
        result(queries[i].zero.Get(), 0, i, "zero");
        result(queries[i].visible.Get(), 64 * 64 * insideDraws, i, "visible");
    }
    queries.clear();
    ComPtr<IDirect3DSurface9> target, readback;
    CHECK(device->GetRenderTarget(0, &target));
    CHECK(device->CreateOffscreenPlainSurface(64, 64, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr));
    CHECK(device->GetRenderTargetData(target.Get(), readback.Get()));
    D3DLOCKED_RECT pixels{}; CHECK(readback->LockRect(&pixels, nullptr, D3DLOCK_READONLY));
    DWORD center = *reinterpret_cast<DWORD*>(static_cast<BYTE*>(pixels.pBits) + 32 * pixels.Pitch + 32 * 4);
    if ((center & 0xffffff) != 0x804020) { ++failures; std::printf("readback=%08lx FAIL\n", center); }
    CHECK(readback->UnlockRect());
    CHECK(device->Present(nullptr, nullptr, nullptr, nullptr));
    UINT validationErrors = 0;
    if (info) for (UINT64 i = 0; i < info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i)
    {
        SIZE_T size = 0; CHECK(info->GetMessage(i, nullptr, &size));
        auto bytes = std::make_unique<BYTE[]>(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.get());
        CHECK(info->GetMessage(i, message, &size));
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
        { ++validationErrors; std::printf("D3D12 error %u: %s\n", message->ID, message->pDescription); }
    }
    std::printf("queries=%zu failures=%u validationErrors=%u removal=%08lx\n", size_t(256), failures,
        validationErrors, device12 ? static_cast<ULONG>(device12->GetDeviceRemovedReason()) : 0ul);
    DestroyWindow(window);
    return failures || validationErrors ? 1 : 0;
}
