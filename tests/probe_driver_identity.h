// Licensed under the MIT license.
#pragma once
#include <windows.h>
#include <d3d9.h>
#include <d3d9on12.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <string>
#include "../include/D3D9On12Extensions.h"

inline HMODULE ProbeLocalDriver()
{
    static HMODULE module = [] {
        wchar_t filename[MAX_PATH]{};
        DWORD length = GetModuleFileNameW(nullptr, filename, MAX_PATH);
        if (!length || length >= MAX_PATH) return HMODULE(nullptr);
        std::wstring path(filename);
        auto slash = path.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return HMODULE(nullptr);
        path.resize(slash + 1); path += L"d3d9on12.dll";
        return LoadLibraryW(path.c_str());
    }();
    return module; // Keep the tested module loaded for the probe's lifetime.
}

inline HRESULT ProbeRequireActualDriver(IDirect3DDevice9* device)
{
    HMODULE module = ProbeLocalDriver();
    if (!module) return E_FAIL;
    auto identify = reinterpret_cast<PFN_D3D9On12QueueInterop>(GetProcAddress(module, D3D9ON12_QUEUE_INTEROP_EXPORT));
    if (!identify) return E_NOINTERFACE;
    Microsoft::WRL::ComPtr<IDirect3DDevice9On12> interop;
    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&interop));
    if (FAILED(hr)) return hr;
    Microsoft::WRL::ComPtr<ID3D12Device> underlying;
    hr = interop->GetD3D12Device(IID_PPV_ARGS(&underlying));
    if (FAILED(hr)) return hr;
    D3D9ON12_INTEROP_REQUEST request{};
    request.StructSize = sizeof request; request.Device = underlying.Get();
    UINT token = 0;
    // The malformed operation is rejected only after this driver's registry
    // recognizes the actual device. No rendering/queue operation is performed.
    return identify(&request, &token) == E_INVALIDARG && token == 0 ? S_OK : E_NOINTERFACE;
}
