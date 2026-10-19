// Licensed under the MIT license.
#include "pch.h"
#include "D3D9On12Extensions.h"
// Whether the installed D3D12 runtime offers the private present interfaces (9on12PrivatePresent.h) that
// exclusive fullscreen needs. Probed once on a throw-away device of the default adapter; failures mean "no".
static bool RuntimeOffersPrivatePresent()
{
    static const bool available = []()
    {
        CComPtr<ID3D12Device> pDevice;
        if (SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&pDevice))))
        {
            D3D12_COMMAND_QUEUE_DESC queueDesc = {};
            CComPtr<ID3D12CommandQueue> pQueue;
            CComPtr<ID3D12CommandAllocator> pAllocator;
            CComPtr<ID3D12GraphicsCommandList> pList;
            CComPtr<D3D9on12::ID3D12CommandQueuePrivatePresent> pQueuePresent;
            CComPtr<D3D9on12::ID3D12CommandListPresent> pListPresent;
            return SUCCEEDED(pDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&pQueue))) &&
                SUCCEEDED(pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&pAllocator))) &&
                SUCCEEDED(pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, pAllocator, nullptr, IID_PPV_ARGS(&pList))) &&
                SUCCEEDED(pQueue->QueryInterface(&pQueuePresent)) && SUCCEEDED(pList->QueryInterface(&pListPresent));
        }
        return false;
    }();
    return available;
}

extern "C" uint32_t WINAPI D3D9On12Capabilities(uint32_t abiVersion)
{
    switch (abiVersion)
    {
    case 1u: return D3D9ON12_CAP_SAMPLER_ADDRESS_VALIDATION;
    case 2u: return D3D9ON12_CAP_SAMPLER_ADDRESS_VALIDATION | D3D9ON12_CAP_QUEUED_INTEROP |
        (RuntimeOffersPrivatePresent() ? D3D9ON12_CAP_EXCLUSIVE_FULLSCREEN : 0u);
    default: return 0u;
    }
}

extern "C" HRESULT WINAPI D3D9On12QueueInterop(const D3D9ON12_INTEROP_REQUEST* request, uint32_t* token)
{
    if (!request || !token || request->StructSize != sizeof(*request))
    {
        return E_INVALIDARG;
    }
    *token = 0;
    D3D9on12::Device* pDevice = D3D9on12::Device::FindDeviceForD3D12Device(request->Device);
    if (!pDevice)
    {
        return E_NOINTERFACE;
    }
    return pDevice->QueueInterop(*request, token);
}
