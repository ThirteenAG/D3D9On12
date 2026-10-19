// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#include <d3d9.h>
#include <d3d9on12.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
using Microsoft::WRL::ComPtr;

namespace
{
    constexpr UINT Workers = 2, Rounds = 4, WorkerIterations = 96, MinimumFrames = 64;
    constexpr DWORD WatchdogMilliseconds = 45000;
    constexpr DWORD VertexFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;
    struct Vertex { float x, y, z, rhw; DWORD color; };

    HRESULT Failure(const char* operation, HRESULT result)
    {
        std::printf("FAIL %s hr=%08lx\n", operation, static_cast<ULONG>(result));
        return result;
    }
#define TRY_HR(call) do { HRESULT checked = (call); if (FAILED(checked)) return Failure(#call, checked); } while (0)

    class Watchdog
    {
        HANDLE finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE thread = nullptr;
        static DWORD WINAPI Run(void* context)
        {
            if (WaitForSingleObject(static_cast<HANDLE>(context), WatchdogMilliseconds) != WAIT_OBJECT_0)
            {
                const char message[] = "FAIL probe watchdog expired (blocked API or worker join)\r\n";
                DWORD written = 0;
                WriteFile(GetStdHandle(STD_ERROR_HANDLE), message, sizeof(message) - 1, &written, nullptr);
                TerminateProcess(GetCurrentProcess(), 124);
            }
            return 0;
        }
    public:
        Watchdog() { if (finished) thread = CreateThread(nullptr, 0, Run, finished, 0, nullptr); }
        bool Valid() const { return finished && thread; }
        ~Watchdog()
        {
            if (finished) SetEvent(finished);
            if (thread) { WaitForSingleObject(thread, INFINITE); CloseHandle(thread); }
            if (finished) CloseHandle(finished);
        }
    };

    struct SharedWorkers
    {
        IDirect3DDevice9* device = nullptr; // The main thread retains it until all workers join.
        std::atomic<bool> go{ false }, stop{ false };
        std::atomic<UINT> done{ 0 }, resourceIterations{ 0 };
        std::atomic<HRESULT> firstFailure{ S_OK };
    };
    struct WorkerContext { SharedWorkers* shared = nullptr; UINT index = 0; };

    HRESULT Work(WorkerContext& context)
    {
        auto& shared = *context.shared;
        ULONGLONG gateDeadline = GetTickCount64() + 5000;
        while (!shared.go.load() && !shared.stop.load())
        {
            if (GetTickCount64() >= gateDeadline) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            Sleep(0);
        }
        for (UINT iteration = 0; iteration < WorkerIterations && !shared.stop.load(); ++iteration)
        {
            // Each worker owns independent resources and never changes device bindings.
            ComPtr<IDirect3DVertexBuffer9> vertices;
            ComPtr<IDirect3DIndexBuffer9> indices;
            ComPtr<IDirect3DTexture9> texture;
            constexpr UINT BufferBytes = 8192, Half = BufferBytes / 2;
            TRY_HR(shared.device->CreateVertexBuffer(BufferBytes, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                VertexFvf, D3DPOOL_DEFAULT, &vertices, nullptr));
            TRY_HR(shared.device->CreateIndexBuffer(BufferBytes, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                D3DFMT_INDEX16, D3DPOOL_DEFAULT, &indices, nullptr));
            TRY_HR(shared.device->CreateTexture(16, 16, 1, 0, D3DFMT_A8R8G8B8,
                D3DPOOL_MANAGED, &texture, nullptr));
            for (UINT update = 0; update < 4 && !shared.stop.load(); ++update)
            {
                BYTE pattern = static_cast<BYTE>(iteration + update + context.index * 31);
                void* data = nullptr;
                TRY_HR(vertices->Lock(0, Half, &data, D3DLOCK_DISCARD));
                std::memset(data, pattern, Half);
                TRY_HR(vertices->Unlock());
                TRY_HR(vertices->Lock(Half, Half, &data, D3DLOCK_NOOVERWRITE));
                std::memset(data, pattern ^ 0x5a, Half);
                TRY_HR(vertices->Unlock());
                TRY_HR(indices->Lock(0, Half, &data, D3DLOCK_DISCARD));
                std::memset(data, pattern, Half);
                TRY_HR(indices->Unlock());
                TRY_HR(indices->Lock(Half, Half, &data, D3DLOCK_NOOVERWRITE));
                std::memset(data, pattern ^ 0xa5, Half);
                TRY_HR(indices->Unlock());
            }
            D3DLOCKED_RECT pixels{};
            TRY_HR(texture->LockRect(0, &pixels, nullptr, 0));
            for (UINT y = 0; y < 16; ++y)
                std::memset(static_cast<BYTE*>(pixels.pBits) + y * pixels.Pitch,
                    static_cast<int>(iteration + context.index), 16 * sizeof(DWORD));
            TRY_HR(texture->UnlockRect(0));
            ++shared.resourceIterations;
            Sleep(0);
        }
        return S_OK;
    }

    unsigned __stdcall Worker(void* value)
    {
        auto& context = *static_cast<WorkerContext*>(value);
        HRESULT result = Work(context);
        if (FAILED(result))
        {
            HRESULT expected = S_OK;
            context.shared->firstFailure.compare_exchange_strong(expected, result);
            context.shared->stop.store(true);
            std::printf("FAIL worker=%u hr=%08lx\n", context.index, static_cast<ULONG>(result));
        }
        ++context.shared->done;
        return FAILED(result) ? 1 : 0;
    }

    D3DPRESENT_PARAMETERS Parameters(HWND window, UINT width, UINT height)
    {
        D3DPRESENT_PARAMETERS parameters{};
        parameters.Windowed = TRUE;
        parameters.hDeviceWindow = window;
        parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
        parameters.BackBufferWidth = width;
        parameters.BackBufferHeight = height;
        parameters.BackBufferFormat = D3DFMT_X8R8G8B8;
        parameters.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        return parameters;
    }

    HRESULT DrawState(IDirect3DDevice9* device)
    {
        TRY_HR(device->SetVertexShader(nullptr));
        TRY_HR(device->SetPixelShader(nullptr));
        TRY_HR(device->SetFVF(VertexFvf));
        TRY_HR(device->SetTexture(0, nullptr));
        TRY_HR(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
        TRY_HR(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE));
        TRY_HR(device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1));
        TRY_HR(device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE));
        TRY_HR(device->SetRenderState(D3DRS_ZENABLE, FALSE));
        TRY_HR(device->SetRenderState(D3DRS_LIGHTING, FALSE));
        TRY_HR(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
        TRY_HR(device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE));
        TRY_HR(device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE));
        TRY_HR(device->SetRenderState(D3DRS_FOGENABLE, FALSE));
        TRY_HR(device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE));
        return S_OK;
    }

    HRESULT Buffer(IDirect3DDevice9* device, ComPtr<IDirect3DVertexBuffer9>& buffer)
    {
        TRY_HR(device->CreateVertexBuffer(4 * sizeof(Vertex), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
            VertexFvf, D3DPOOL_DEFAULT, &buffer, nullptr));
        return S_OK;
    }

    HRESULT Render(IDirect3DDevice9* device, IDirect3DVertexBuffer9* buffer,
        UINT width, UINT height, DWORD color)
    {
        const Vertex vertices[] = { {-.5f,-.5f,.5f,1,color}, {width-.5f,-.5f,.5f,1,color},
            {-.5f,height-.5f,.5f,1,color}, {width-.5f,height-.5f,.5f,1,color} };
        void* data = nullptr;
        TRY_HR(buffer->Lock(0, sizeof(vertices), &data, D3DLOCK_DISCARD));
        std::memcpy(data, vertices, sizeof(vertices));
        TRY_HR(buffer->Unlock());
        TRY_HR(device->SetStreamSource(0, buffer, 0, sizeof(Vertex)));
        TRY_HR(device->BeginScene());
        HRESULT drawResult = S_OK;
        for (UINT i = 0; i < 16 && SUCCEEDED(drawResult); ++i)
            drawResult = device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
        HRESULT endResult = device->EndScene();
        if (FAILED(drawResult)) return Failure("DrawPrimitive", drawResult);
        if (FAILED(endResult)) return Failure("EndScene", endResult);
        return S_OK;
    }

    HRESULT Readback(IDirect3DDevice9* device, UINT width, UINT height, DWORD expected)
    {
        // All surface references leave this scope before any Reset.
        ComPtr<IDirect3DSurface9> target, copy;
        TRY_HR(device->GetRenderTarget(0, &target));
        TRY_HR(device->CreateOffscreenPlainSurface(width, height, D3DFMT_X8R8G8B8,
            D3DPOOL_SYSTEMMEM, &copy, nullptr));
        TRY_HR(device->GetRenderTargetData(target.Get(), copy.Get()));
        D3DLOCKED_RECT pixels{};
        TRY_HR(copy->LockRect(&pixels, nullptr, D3DLOCK_READONLY));
        bool match = true;
        for (UINT y = 0; y < height && match; ++y)
        {
            auto row = reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(pixels.pBits) + y * pixels.Pitch);
            for (UINT x = 0; x < width; ++x)
            {
                if ((row[x] & 0xffffff) != (expected & 0xffffff))
                {
                    std::printf("FAIL pixel(%u,%u)=%08lx expected=%08lx\n", x, y,
                        static_cast<ULONG>(row[x]), static_cast<ULONG>(expected));
                    match = false;
                    break;
                }
            }
        }
        TRY_HR(copy->UnlockRect());
        return match ? S_OK : E_FAIL;
    }

    struct Validation
    {
        bool native = false, debug = false;
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12InfoQueue> messages;
        UINT64 cursor = 0;
        UINT errors = 0;

        HRESULT Check()
        {
            if (device)
            {
                HRESULT removal = device->GetDeviceRemovedReason();
                if (FAILED(removal)) return Failure("GetDeviceRemovedReason", removal);
            }
            if (messages)
            {
                if (messages->GetNumMessagesDiscardedByMessageCountLimit())
                    return Failure("D3D12 validation queue overflow", E_FAIL);
                UINT64 count = messages->GetNumStoredMessagesAllowedByRetrievalFilter();
                for (; cursor < count; ++cursor)
                {
                    SIZE_T bytes = 0;
                    TRY_HR(messages->GetMessage(cursor, nullptr, &bytes));
                    auto storage = std::make_unique<BYTE[]>(bytes);
                    auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.get());
                    TRY_HR(messages->GetMessage(cursor, message, &bytes));
                    if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
                    {
                        ++errors;
                        std::printf("D3D12 error %u: %s\n", message->ID, message->pDescription);
                    }
                }
            }
            return errors ? E_FAIL : S_OK;
        }

        HRESULT Refresh(IDirect3DDevice9* device9)
        {
            if (native) return S_OK;
            ComPtr<IDirect3DDevice9On12> interop;
            ComPtr<ID3D12Device> next;
            TRY_HR(device9->QueryInterface(IID_PPV_ARGS(&interop)));
            TRY_HR(interop->GetD3D12Device(IID_PPV_ARGS(&next)));
            if (next.Get() != device.Get())
            {
                TRY_HR(Check());
                device = next;
                messages.Reset();
                cursor = 0;
                if (debug)
                {
                    TRY_HR(device.As(&messages));
                    TRY_HR(messages->SetMessageCountLimit(4096));
                }
            }
            return Check();
        }
    };

    HRESULT ConcurrentRound(IDirect3DDevice9* device, UINT width, UINT height,
        DWORD color, UINT round)
    {
        ComPtr<IDirect3DVertexBuffer9> mainBuffer;
        TRY_HR(DrawState(device));
        TRY_HR(Buffer(device, mainBuffer));
        SharedWorkers shared;
        shared.device = device;
        std::array<WorkerContext, Workers> contexts{};
        std::array<HANDLE, Workers> threads{};
        UINT started = 0;
        HRESULT result = S_OK;
        for (UINT i = 0; i < Workers; ++i)
        {
            contexts[i] = { &shared, i };
            threads[i] = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, Worker, &contexts[i], 0, nullptr));
            if (!threads[i]) { result = E_FAIL; shared.stop.store(true); break; }
            ++started;
        }
        shared.go.store(true);
        UINT frames = 0;
        ULONGLONG deadline = GetTickCount64() + 15000;
        while (SUCCEEDED(result) && !shared.stop.load() &&
            (shared.done.load() < started || frames < MinimumFrames))
        {
            if (GetTickCount64() >= deadline || frames >= 65536)
            {
                result = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                shared.stop.store(true);
                break;
            }
            result = Render(device, mainBuffer.Get(), width, height, color);
            if (SUCCEEDED(result) && frames % 16 == 0)
                result = Readback(device, width, height, color);
            if (SUCCEEDED(result)) result = device->Present(nullptr, nullptr, nullptr, nullptr);
            ++frames;
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            { TranslateMessage(&message); DispatchMessageW(&message); }
            Sleep(0); // Let resource workers acquire the runtime's device lock.
        }
        if (FAILED(result)) shared.stop.store(true);
        if (started)
        {
            DWORD joined = WaitForMultipleObjects(started, threads.data(), TRUE, 10000);
            if (joined != WAIT_OBJECT_0)
            {
                const char message[] = "FAIL bounded worker join timed out\r\n";
                DWORD written = 0;
                WriteFile(GetStdHandle(STD_ERROR_HANDLE), message, sizeof(message) - 1, &written, nullptr);
                TerminateProcess(GetCurrentProcess(), 124);
            }
        }
        for (auto handle : threads) if (handle) CloseHandle(handle);
        if (FAILED(shared.firstFailure.load())) result = shared.firstFailure.load();
        if (SUCCEEDED(result) && shared.resourceIterations.load() != Workers * WorkerIterations)
            result = E_FAIL;
        if (SUCCEEDED(result))
        {
            result = Render(device, mainBuffer.Get(), width, height, color);
            if (SUCCEEDED(result)) result = Readback(device, width, height, color);
            if (SUCCEEDED(result)) result = device->Present(nullptr, nullptr, nullptr, nullptr);
        }
        // Workers have returned and released every resource before Reset is allowed.
        HRESULT unbind = device->SetStreamSource(0, nullptr, 0, 0);
        mainBuffer.Reset();
        if (SUCCEEDED(result)) result = unbind;
        std::printf("round=%u frames=%u workerIterations=%u result=%08lx\n", round,
            frames, shared.resourceIterations.load(), static_cast<ULONG>(result));
        return result;
    }

    HRESULT Run(bool native, bool debug)
    {
        if (debug && !native)
        {
            ComPtr<ID3D12Debug> layer;
            TRY_HR(D3D12GetDebugInterface(IID_PPV_ARGS(&layer)));
            layer->EnableDebugLayer();
        }
        wchar_t path[MAX_PATH]{};
        GetSystemDirectoryW(path, MAX_PATH);
        wcscat_s(path, L"\\d3d9.dll");
        HMODULE runtime = LoadLibraryW(path);
        if (!runtime) return HRESULT_FROM_WIN32(GetLastError());
        ComPtr<IDirect3D9> d3d;
        if (native)
        {
            auto create = reinterpret_cast<IDirect3D9* (WINAPI*)(UINT)>(GetProcAddress(runtime, "Direct3DCreate9"));
            if (!create) return E_NOINTERFACE;
            d3d.Attach(create(D3D_SDK_VERSION));
        }
        else
        {
            auto create = reinterpret_cast<PFN_Direct3DCreate9On12>(GetProcAddress(runtime, "Direct3DCreate9On12"));
            if (!create) return E_NOINTERFACE;
            D3D9ON12_ARGS arguments{};
            arguments.Enable9On12 = TRUE;
            d3d.Attach(create(D3D_SDK_VERSION, &arguments, 1));
        }
        if (!d3d) return E_FAIL;
        HWND window = CreateWindowW(L"STATIC", L"D3D9On12 multithread/reset probe", WS_OVERLAPPED,
            0, 0, 96, 96, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!window) return HRESULT_FROM_WIN32(GetLastError());
        UINT width = 64, height = 64;
        auto parameters = Parameters(window, width, height);
        ComPtr<IDirect3DDevice9> device;
        HRESULT result = d3d->CreateDevice(0, D3DDEVTYPE_HAL, window,
            D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED, &parameters, &device);
        std::printf("CreateDevice=%08lx flags=MULTITHREADED|HWVP\n", static_cast<ULONG>(result));
        Validation validation;
        validation.native = native;
        validation.debug = debug;
        if (SUCCEEDED(result)) result = validation.Refresh(device.Get());
        if (!native)
        {
            HMODULE driver = GetModuleHandleW(L"d3d9on12.dll");
            if (driver) GetModuleFileNameW(driver, path, MAX_PATH);
            else wcscpy_s(path, L"<unavailable>");
        }
        std::printf("native=%u driver=%ls\n", native, native ? L"system D3D9" : path);
        constexpr DWORD Colors[] = { 0xff203040, 0xffa03020, 0xff20b060, 0xff3050c0 };
        UINT resets = 0;
        for (UINT round = 0; round < Rounds && SUCCEEDED(result); ++round)
        {
            result = ConcurrentRound(device.Get(), width, height, Colors[round], round);
            if (SUCCEEDED(result)) result = validation.Check();
            for (UINT repeat = 0; repeat < 2 && SUCCEEDED(result); ++repeat)
            {
                // No workers, app-held backbuffers or default-pool buffers remain live.
                width = (round + repeat) % 2 ? 80 : 64;
                height = (round + repeat) % 2 ? 72 : 64;
                parameters = Parameters(window, width, height);
                result = device->Reset(&parameters); // Always on the device/window's owning thread.
                if (SUCCEEDED(result)) ++resets;
                if (SUCCEEDED(result)) result = validation.Refresh(device.Get());
                if (SUCCEEDED(result)) result = DrawState(device.Get());
                ComPtr<IDirect3DVertexBuffer9> buffer;
                if (SUCCEEDED(result)) result = Buffer(device.Get(), buffer);
                DWORD color = Colors[(round + repeat + 1) % _countof(Colors)];
                if (SUCCEEDED(result)) result = Render(device.Get(), buffer.Get(), width, height, color);
                if (SUCCEEDED(result)) result = Readback(device.Get(), width, height, color);
                if (SUCCEEDED(result)) result = device->Present(nullptr, nullptr, nullptr, nullptr);
                if (buffer)
                {
                    HRESULT unbind = device->SetStreamSource(0, nullptr, 0, 0);
                    if (SUCCEEDED(result)) result = unbind;
                    buffer.Reset();
                }
                std::printf("reset=%u dimensions=%ux%u result=%08lx\n", resets, width, height,
                    static_cast<ULONG>(result));
            }
        }
        if (SUCCEEDED(result)) result = validation.Check();
        device.Reset();
        HRESULT destructionValidation = validation.Check();
        if (SUCCEEDED(result)) result = destructionValidation;
        d3d.Reset();
        DestroyWindow(window);
        std::printf("resets=%u validationErrors=%u result=%08lx %s\n", resets, validation.errors,
            static_cast<ULONG>(result), SUCCEEDED(result) && resets == Rounds * 2 ? "PASS" : "FAIL");
        return SUCCEEDED(result) && resets == Rounds * 2 ? S_OK : FAILED(result) ? result : E_FAIL;
    }
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    bool native = false, debug = false;
    for (int i = 1; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--native")) native = true;
        else if (!std::strcmp(argv[i], "--debug")) debug = true;
        else { std::printf("Unknown argument: %s\n", argv[i]); return 2; }
    }
    Watchdog watchdog;
    if (!watchdog.Valid()) return 1;
    try { return SUCCEEDED(Run(native, debug)) ? 0 : 1; }
    catch (...) { std::printf("FAIL unexpected C++ exception\n"); return 1; }
}
