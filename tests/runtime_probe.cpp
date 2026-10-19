// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <d3d9on12.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "D3D9On12Extensions.h"
#include "probe_driver_identity.h"

// Run beside the x86 driver and its dependencies. No registry or system files change.
int main(int argc, char** argv)
{
    auto hasArgument = [&](const char* name)
    {
        for (int i = 1; i < argc; ++i)
            if (std::strcmp(argv[i], name) == 0) return true;
        return false;
    };
    HMODULE driver = ProbeLocalDriver();
    if (!driver)
    {
        std::printf("Local driver load failed: %lu\n", GetLastError());
        return 6;
    }
    wchar_t path[MAX_PATH]{};
    GetSystemDirectoryW(path, MAX_PATH);
    wcscat_s(path, L"\\d3d9.dll");
    HMODULE runtime = LoadLibraryW(path);
    auto create = reinterpret_cast<PFN_Direct3DCreate9On12>(GetProcAddress(runtime, "Direct3DCreate9On12"));
    if (!create)
        return 1;
    D3D9ON12_ARGS args{};
    args.Enable9On12 = TRUE;
    IDirect3D9* d3d = create(D3D_SDK_VERSION, &args, 1);
    if (!d3d)
        return 2;
    HWND window = CreateWindowW(L"STATIC", L"D3D9On12 runtime probe", WS_OVERLAPPED,
        0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.hDeviceWindow = window;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = 64;
    pp.BackBufferHeight = 64;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    IDirect3DDevice9* device = nullptr;
    HRESULT hr = d3d->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device);
    std::printf("CreateDevice=%08lx\n", static_cast<unsigned long>(hr));
    if (FAILED(hr))
    {
        d3d->Release();
        DestroyWindow(window);
        return 3;
    }
    GetModuleFileNameW(driver, path, MAX_PATH);
    std::printf("driver=%ls\n", path);
    IDirect3DDevice9On12* interop = nullptr;
    hr = device->QueryInterface(__uuidof(IDirect3DDevice9On12), reinterpret_cast<void**>(&interop));
    std::printf("Interop=%08lx\n", static_cast<unsigned long>(hr));
    int failures = FAILED(hr) ? 1 : 0;
    if (hasArgument("--require-local"))
    {
        wchar_t expected[MAX_PATH]{};
        GetModuleFileNameW(nullptr, expected, MAX_PATH);
        wchar_t* filename = wcsrchr(expected, L'\\');
        if (!filename)
            ++failures;
        else
        {
            *(filename + 1) = 0;
            wcscat_s(expected, L"d3d9on12.dll");
            const bool local = _wcsicmp(path, expected) == 0;
            std::printf("Local driver=%s\n", local ? "PASS" : "FAIL");
            failures += local ? 0 : 1;
            const bool actual = SUCCEEDED(ProbeRequireActualDriver(device));
            std::printf("Actual device driver=%s\n", actual ? "PASS" : "FAIL");
            failures += actual ? 0 : 1;
        }
    }
    if (interop)
        interop->Release();
    if (hasArgument("--require-extensions"))
    {
        auto capabilities = reinterpret_cast<PFN_D3D9On12Capabilities>(
            GetProcAddress(GetModuleHandleW(L"d3d9on12.dll"), D3D9ON12_CAPABILITIES_EXPORT));
        const bool valid = capabilities &&
            (capabilities(D3D9ON12_CAPABILITIES_ABI) & D3D9ON12_CAP_SAMPLER_ADDRESS_VALIDATION) &&
            capabilities(0) == 0 && capabilities(D3D9ON12_CAPABILITIES_ABI + 1) == 0;
        std::printf("D3D9On12 capability ABI=%s\n", valid ? "PASS" : "FAIL");
        failures += valid ? 0 : 1;
    }
    if (hasArgument("--require-release"))
    {
        bool release = true;
        for (const char* name : { "D3D9On12RegisterConstants", "D3D9On12QueueDraw", "D3D9On12StartDrawWorker" })
            release = release && GetProcAddress(driver, name) == nullptr;
        std::printf("Release export boundary=%s\n", release ? "PASS" : "FAIL");
        failures += release ? 0 : 1;
    }

    IDirect3DQuery9* timestamp = nullptr;
    hr = device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &timestamp);
    std::printf("CreateTimestamp=%08lx\n", static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr))
    {
        hr = timestamp->Issue(D3DISSUE_END);
        std::uint64_t first = 0xA5A5A5A5CCCCCCCCull;
        const ULONGLONG deadline = GetTickCount64() + 5000;
        while (SUCCEEDED(hr) && (hr = timestamp->GetData(&first, sizeof(first), D3DGETDATA_FLUSH)) == S_FALSE
            && GetTickCount64() < deadline)
            Sleep(1);
        std::uint64_t second = 0x5A5A5A5ACCCCCCCCull;
        HRESULT secondHr = timestamp->GetData(&second, sizeof(second), 0);
        bool complete = hr == S_OK && secondHr == S_OK && first == second;
        std::printf("Timestamp=%016llx second=%016llx status=%08lx/%08lx %s\n",
            first, second, static_cast<unsigned long>(hr), static_cast<unsigned long>(secondHr), complete ? "PASS" : "FAIL");
        failures += complete ? 0 : 1;
        timestamp->Release();
    }
    else
        ++failures;
    device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF204060, 1, 0);
    hr = device->Present(nullptr, nullptr, nullptr, nullptr);
    std::printf("Present=%08lx\n", static_cast<unsigned long>(hr));
    failures += FAILED(hr) ? 1 : 0;
    if (hasArgument("--reset"))
    {
        hr = device->Reset(&pp);
        std::printf("Reset=%08lx\n", static_cast<unsigned long>(hr));
        failures += FAILED(hr) ? 1 : 0;
        if (SUCCEEDED(hr))
        {
            device->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF604020, 1, 0);
            hr = device->Present(nullptr, nullptr, nullptr, nullptr);
            std::printf("PresentAfterReset=%08lx\n", static_cast<unsigned long>(hr));
            failures += FAILED(hr) ? 1 : 0;
        }
    }
    device->Release();
    d3d->Release();
    DestroyWindow(window);
    return failures ? 4 : 0;
}
