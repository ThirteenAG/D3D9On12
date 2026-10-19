// Public D3D9Ex regression for latency changes concurrent with queued rendering.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <d3d9on12.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include "D3D9On12Extensions.h"
using Microsoft::WRL::ComPtr;
#define CHECK(call) do { HRESULT checked = (call); if (FAILED(checked)) { \
    std::printf("FAIL %s hr=%08lx\n", #call, static_cast<ULONG>(checked)); return 1; } } while (0)
namespace {
struct Watchdog {
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread thread;
    Watchdog() : thread([this] {
        if (WaitForSingleObject(done, 45000) != WAIT_OBJECT_0) {
            std::puts("FAIL latency probe watchdog"); TerminateProcess(GetCurrentProcess(), 124);
        }
    }) {}
    ~Watchdog() { SetEvent(done); thread.join(); CloseHandle(done); }
};
struct LatencyWriter {
    ComPtr<IDirect3DDevice9Ex> device;
    std::atomic<bool> stop{false};
    std::atomic<HRESULT> error{S_OK};
    std::atomic<UINT> calls{0};
    std::thread thread;
    explicit LatencyWriter(IDirect3DDevice9Ex* p) : device(p), thread([this] {
        UINT value = 1;
        while (!stop.load()) {
            const HRESULT hr = device->SetMaximumFrameLatency(value);
            if (FAILED(hr)) { error.store(hr); break; }
            ++calls; value = value % 3 + 1; Sleep(1);
        }
    }) {}
    void Stop() { stop.store(true); if (thread.joinable()) thread.join(); }
    ~LatencyWriter() { Stop(); }
};
HRESULT PresentEventually(IDirect3DDevice9Ex* device, UINT& retries) {
    const ULONGLONG deadline = GetTickCount64() + 1000;
    for (;;) {
        const HRESULT hr = device->PresentEx(nullptr, nullptr, nullptr, nullptr, D3DPRESENT_DONOTWAIT);
        if (hr != D3DERR_WASSTILLDRAWING) return hr;
        ++retries;
        if (GetTickCount64() >= deadline) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        Sleep(1);
    }
}
}
int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    bool native = false, debug = false;
    for (int i = 1; i < argc; ++i) { native |= !std::strcmp(argv[i], "--native"); debug |= !std::strcmp(argv[i], "--debug"); }
    Watchdog watchdog;
    if (debug && !native) {
        ComPtr<ID3D12Debug> layer; CHECK(D3D12GetDebugInterface(IID_PPV_ARGS(&layer))); layer->EnableDebugLayer();
    }
    HMODULE driver = nullptr;
    if (!native) {
        wchar_t filename[MAX_PATH]{}; GetModuleFileNameW(nullptr, filename, MAX_PATH);
        std::wstring path(filename); path.resize(path.find_last_of(L"\\/") + 1); path += L"d3d9on12.dll";
        driver = LoadLibraryW(path.c_str());
        if (!driver) { std::printf("FAIL local driver load error=%lu\n", GetLastError()); return 1; }
        std::printf("driver=%ls\n", path.c_str());
    }
    wchar_t system[MAX_PATH]{}; GetSystemDirectoryW(system, MAX_PATH); wcscat_s(system, L"\\d3d9.dll");
    HMODULE runtime = LoadLibraryW(system);
    if (!runtime) return 1;
    ComPtr<IDirect3D9Ex> d3d;
    if (native) {
        using Create = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
        auto create = reinterpret_cast<Create>(GetProcAddress(runtime, "Direct3DCreate9Ex"));
        if (!create) return 1; CHECK(create(D3D_SDK_VERSION, &d3d));
    } else {
        auto create = reinterpret_cast<PFN_Direct3DCreate9On12Ex>(GetProcAddress(runtime, "Direct3DCreate9On12Ex"));
        if (!create) return 1;
        D3D9ON12_ARGS args{}; args.Enable9On12 = TRUE; CHECK(create(D3D_SDK_VERSION, &args, 1, &d3d));
    }
    HWND window = CreateWindowW(L"STATIC", L"D3D9 latency probe", WS_OVERLAPPEDWINDOW,
        10, 10, 192, 128, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window) return 1;
    ShowWindow(window, SW_SHOWNOACTIVATE);
    D3DPRESENT_PARAMETERS pp{}; pp.Windowed = TRUE; pp.hDeviceWindow = window;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferWidth = 128; pp.BackBufferHeight = 64;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferCount = 2; pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    ComPtr<IDirect3DDevice9Ex> device;
    CHECK(d3d->CreateDeviceEx(0, D3DDEVTYPE_HAL, window,
        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED, &pp, nullptr, &device));
    ComPtr<ID3D12InfoQueue> queue;
    if (!native) {
        ComPtr<IDirect3DDevice9On12> interop; CHECK(device.As(&interop));
        ComPtr<ID3D12Device> underlying; CHECK(interop->GetD3D12Device(IID_PPV_ARGS(&underlying)));
        // A malformed request reaches E_INVALIDARG only after this driver's
        // registry recognizes the actual D3D12 device; an inbox device returns E_NOINTERFACE.
        auto identify = reinterpret_cast<PFN_D3D9On12QueueInterop>(GetProcAddress(driver, D3D9ON12_QUEUE_INTEROP_EXPORT));
        if (!identify) return 1;
        D3D9ON12_INTEROP_REQUEST request{}; request.StructSize = sizeof(request); request.Device = underlying.Get();
        UINT token = 0;
        if (identify(&request, &token) != E_INVALIDARG || token != 0) { std::puts("FAIL actual device driver identity"); return 1; }
        std::puts("actual device driver identity PASS");
        if (debug) { CHECK(underlying.As(&queue)); }
    }
    ComPtr<IDirect3DSurface9> target, backBuffer;
    CHECK(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer));
    CHECK(device->CreateRenderTarget(128, 64, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &target, nullptr));
    CHECK(device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE));
    CHECK(device->SetRenderState(D3DRS_ZENABLE, FALSE));
    struct Vertex { float x,y,z,rhw; DWORD color; };
    const Vertex vertices[] = {{0,0,.5f,1,0xffffffff},{32,0,.5f,1,0xffffffff},{0,32,.5f,1,0xffffffff}};
    UINT retries = 0;
    LatencyWriter writer(device.Get());
    for (UINT frame = 0; frame < 64; ++frame) {
        CHECK(device->SetRenderTarget(0, target.Get()));
        CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff20c060, 1, 0));
        CHECK(device->BeginScene());
        for (UINT draw = 0; draw < 96; ++draw) CHECK(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, vertices, sizeof(Vertex)));
        CHECK(device->EndScene());
        CHECK(device->SetRenderTarget(0, backBuffer.Get()));
        CHECK(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xff204060, 1, 0));
        CHECK(PresentEventually(device.Get(), retries));
        MSG message; while (PeekMessageW(&message, window, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
    }
    writer.Stop(); CHECK(writer.error.load());
    if (!writer.calls.load()) { std::puts("FAIL no concurrent latency changes"); return 1; }
    CHECK(device->SetMaximumFrameLatency(3)); UINT latency = 0; CHECK(device->GetMaximumFrameLatency(&latency));
    if (latency != 3) return 1;
    ComPtr<IDirect3DSurface9> readback;
    CHECK(device->CreateOffscreenPlainSurface(128, 64, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr));
    CHECK(device->GetRenderTargetData(target.Get(), readback.Get()));
    D3DLOCKED_RECT locked{}; CHECK(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY));
    const DWORD pixel = *reinterpret_cast<DWORD*>(static_cast<BYTE*>(locked.pBits) + 32 * locked.Pitch + 96 * 4) & 0xffffff;
    CHECK(readback->UnlockRect());
    if (pixel != 0x20c060) { std::printf("FAIL retained pixel=%06lx\n", pixel); return 1; }
    // No new rendering must still make progress, including after an Ex reset.
    for (UINT i = 0; i < 32; ++i) CHECK(PresentEventually(device.Get(), retries));
    target.Reset(); backBuffer.Reset(); CHECK(device->ResetEx(&pp, nullptr));
    for (UINT i = 0; i < 32; ++i) CHECK(PresentEventually(device.Get(), retries));
    readback.Reset(); writer.device.Reset(); device.Reset(); d3d.Reset();
    UINT errors = 0, metadataFallbacks = 0;
    if (queue) for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size = 0; queue->GetMessage(i, nullptr, &size); std::unique_ptr<BYTE[]> data(new BYTE[size]);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(data.get());
        if (SUCCEEDED(queue->GetMessage(i, message, &size)) && message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
            // The pinned TL probes optional D3D11 flags for native Ex back
            // buffers, then intentionally falls back to the D3D12 description.
            // Both the original handoff driver and this version report this
            // exact recoverable lookup for their two initial/reset buffers.
            if (message->ID == D3D12_MESSAGE_ID_REFLECTSHAREDPROPERTIES_INVALIDOBJECT &&
                std::strstr(message->pDescription, "Resource provided was not shared by D3D11, or with a D3D11 desc.")) {
                ++metadataFallbacks;
            } else {
                ++errors; std::printf("validation id=%u: %s\n", static_cast<UINT>(message->ID), message->pDescription);
            }
        }
    }
    if (metadataFallbacks > 4) { ++errors; std::puts("FAIL unexpected number of optional metadata probes"); }
    std::printf("frames=64 latencyChanges=%u retries=%u pixel=%06lx metadataFallbacks=%u validationErrors=%u %s\n",
        writer.calls.load(), retries, pixel, metadataFallbacks, errors, errors ? "FAIL" : "PASS");
    DestroyWindow(window);
    return errors ? 1 : 0;
}
