// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "SwapChainHelper.hpp"

namespace D3D9on12
{
    _Check_return_ HRESULT APIENTRY Present1(_In_ HANDLE hDevice, _In_ D3DDDIARG_PRESENT1* pPresentArgs)
    {
        D3D9on12_DDI_ENTRYPOINT_START(TRUE);
        Device* pDevice = Device::GetDeviceFromHandle(hDevice);

        pDevice->m_pUMDPresentArgs = pPresentArgs;

        // The runtime and 9on12's KmdPresent will handle filling this out with meaningful data,
        // we need to go through the callback because it's the only way to get access to the runtime's
        // D3DKMT_PRESENT data
        D3DDDICB_PRESENT callbackArgs = {};
        pDevice->GetRuntimeCallbacks().pfnPresentCb(pDevice->GetRuntimeHandle(), &callbackArgs);

        D3D9on12_DDI_ENTRYPOINT_END_AND_RETURN_HR(S_OK);
    }

    _Check_return_ HRESULT APIENTRY KmdPresent(_In_ HANDLE hDevice, D3DKMT_PRESENT *pKMTArgs)
    {
        D3D9on12_DDI_ENTRYPOINT_START(TRUE);
        Device* pDevice = Device::GetDeviceFromHandle(hDevice);
        if (pDevice == nullptr || pDevice->m_pUMDPresentArgs == nullptr || pKMTArgs == nullptr)
        {
            RETURN_E_INVALIDARG_AND_CHECK();
        }
        
        HRESULT hr = pDevice->Present(*pDevice->m_pUMDPresentArgs, pKMTArgs);

        // This pointer becomes invalid after present since it's owned by the runtime
        pDevice->m_pUMDPresentArgs = nullptr;
        D3D9on12_DDI_ENTRYPOINT_END_AND_RETURN_HR(hr);
    }

    _Check_return_ HRESULT APIENTRY CloseAndSubmitGraphicsCommandListForPresent(
        _In_ HANDLE hDevice,
        BOOL commandsAdded,
        _In_reads_(numSrcSurfaces) const D3DDDIARG_PRESENTSURFACE* pSrcSurfaces,
        UINT numSrcSurfaces,
        _In_opt_ HANDLE hDestResource,
        _In_ D3DKMT_PRESENT* pKMTPresent)
    {
        D3D9on12_DDI_ENTRYPOINT_START(TRUE);
        Device* pDevice = Device::GetDeviceFromHandle(hDevice);
        if (pDevice == nullptr)
        {
            RETURN_E_INVALIDARG_AND_CHECK();
        }

        HRESULT hr = pDevice->CloseAndSubmitGraphicsCommandListForPresent(commandsAdded, pSrcSurfaces, numSrcSurfaces, hDestResource, pKMTPresent);

        D3D9on12_DDI_ENTRYPOINT_END_AND_RETURN_HR(hr);
    }

    HRESULT Device::Present(CONST D3DDDIARG_PRESENT1& PresentArgs, D3DKMT_PRESENT* pKMTArgs)
    {
        Check9on12(PresentArgs.SrcResources == 1);

        Resource* pSource = Resource::GetResourceFromHandle(PresentArgs.phSrcResources[0].hResource);
        Resource* pDest = Resource::GetResourceFromHandle(PresentArgs.hDstResource);
        if (pSource)
        {
            const D3DDDIARG_PRESENTSURFACE* presentSurfaces = PresentArgs.phSrcResources;
            bool bPresentWithSystemMemory = pSource->IsSystemMemory();
            bool sourceIsCompatibleWithPresent = !bPresentWithSystemMemory && !pSource->NeedsSwapRBOutputChannels();

            if (pDest)
            {
                // We can't use a typeless or msaa resources when doing a blt-model present.
                // We handle this by copying it over to a backing resource with a 
                // single-count and fully qualified format.
                sourceIsCompatibleWithPresent = sourceIsCompatibleWithPresent && IsCompatibleAsSourceForBltPresent(pSource->GetDesc());
            }
            else
            {
                sourceIsCompatibleWithPresent = sourceIsCompatibleWithPresent && !IsTypelessFormat(pSource->GetDesc().Format);
            }
            
            if (!sourceIsCompatibleWithPresent)
            {
                pSource = pSource->GetBackingPlainResource( true );
                // Need to reference the new resource in present surface 0
                m_d3d9PresentSurfaces.clear();
                for (UINT i = 0; i < PresentArgs.SrcResources; i++)
                {
                    m_d3d9PresentSurfaces.push_back(PresentArgs.phSrcResources[i]);
                }
                m_d3d9PresentSurfaces[0].hResource = Resource::GetHandleFromResource(pSource);
                presentSurfaces = m_d3d9PresentSurfaces.data();
            }

            HRESULT hr = E_FAIL;
            if (GetAdapter().m_bSupportsNewPresent)
            {          
                try
                {
                    auto forwardToPresent9On12CB = [adapter = GetAdapter(), runtimeDeviceHandle = GetRuntimeHandle(), pArgs = PresentArgs, presentSurfaces]
                    (D3D12TranslationLayer::PresentCBArgs& args) {
                        D3D9ON12_PRESENTCB_ARGS cbArgs = {};
                        cbArgs.flipInterval = args.flipInterval;
                        cbArgs.pSrcSurfaces = presentSurfaces;
                        cbArgs.numSrcSurfaces = args.numSrcSurfaces;
                        cbArgs.hDestResource = pArgs.hDstResource;
                        cbArgs.pGraphicsCommandList = args.pGraphicsCommandList;
                        cbArgs.pGraphicsCommandQueue = args.pGraphicsCommandQueue;
                        cbArgs.pKMTPresent = args.pKMTPresent;
                        cbArgs.vidPnSourceId = args.vidPnSourceId;
                        return (*adapter.m_privateCallbacks.pfnPresentCB)(runtimeDeviceHandle, &cbArgs);
                    };

                    // Convert present Surfaces into d3d12TranslationLayer format
                    m_d3d12tlPresentSurfaces.clear();
                    for (UINT i = 0; i < PresentArgs.SrcResources; i++)
                    {
                        auto presentSurface = presentSurfaces[i];
                        auto resource = Resource::GetResourceFromHandle(presentSurface.hResource);
                        D3D12TranslationLayer::PresentSurface surface = D3D12TranslationLayer::PresentSurface(resource->GetUnderlyingResource(), presentSurface.SubResourceIndex);
                        m_d3d12tlPresentSurfaces.push_back(surface);
                    }

                    GetContext().Present(
                        m_d3d12tlPresentSurfaces.data(),
                        PresentArgs.SrcResources,
                        pDest ? pDest->GetUnderlyingResource() : nullptr,
                        PresentArgs.FlipInterval,
                        pSource->GetVidPnSourceId(),
                        pKMTArgs,
                        false,
                        forwardToPresent9On12CB);

                    hr = S_OK;
                }
                catch (_com_error& hrEx)
                {
                    hr = hrEx.Error();
                }
                catch (std::bad_alloc&)
                {
                    hr = E_OUTOFMEMORY;
                }
            }
            else
            {
                // Fallback to old present path
                // A primary or the runtime's displayable buffer flipped without a destination (exclusive fullscreen):
                // the kernel flip goes through the D3D12 runtime's private present interfaces. hWindow alone doesn't
                // tell: after a Reset from a window the runtime keeps passing the device window with the exclusive
                // buffers, and presenting those through a DXGI swap chain on that window never completes (every frame
                // waited out the latency gate, the game's device stayed lost).
                if (pSource->GetVidPnSourceId() != D3DDDI_ID_UNINITIALIZED || pKMTArgs->hWindow == nullptr ||
                    (pSource->IsDisplayableSurface() && !pDest))
                {
                    NoteExclusivePresent();
                    hr = PrivatePresent(pSource, presentSurfaces[0].SubResourceIndex, pKMTArgs);
                }
                else
                {
                    m_pWindowPresentSource = Resource::GetResourceFromHandle(PresentArgs.phSrcResources[0].hResource);
                    auto swapChainManager = GetSwapChainManager();
                    D3D12TranslationLayer::Resource* pSrcTranslationLayerResource = pSource->GetUnderlyingResource();

                    auto pSwapChain = swapChainManager->GetSwapChainForWindow(pKMTArgs->hWindow, *pSrcTranslationLayerResource);
                    LeaveWindowToRuntime(pSwapChain, pKMTArgs->hWindow);
                    ApplyHdrToSwapChain(pSwapChain, *pSource);
                    auto swapChainHelper = D3D12TranslationLayer::SwapChainHelper(pSwapChain);
                    GetContext().m_MaxFrameLatencyHelper.WaitForMaximumFrameLatency();

                    hr = swapChainHelper.StandardPresent(GetContext(), pKMTArgs, *pSrcTranslationLayerResource);
                }
            }
            
            m_lockedResourceRanges.GetLocked()->clear();

            return hr;
        }
        else
        {
            return E_INVALIDARG;
        }
    }


    bool Device::SupportsPrivatePresent()
    {
        if (m_privatePresentState == 0)
        {
            ID3D12CommandQueue* pQueue = m_pImmediateContext->GetCommandQueue(D3D12TranslationLayer::COMMAND_LIST_TYPE::GRAPHICS);
            ID3D12GraphicsCommandList* pList = m_pImmediateContext->GetGraphicsCommandList();
            CComPtr<ID3D12CommandQueuePrivatePresent> pQueuePresent;
            CComPtr<ID3D12CommandListPresent> pListPresent;
            const bool available = pQueue != nullptr && pList != nullptr &&
                SUCCEEDED(pQueue->QueryInterface(&pQueuePresent)) && SUCCEEDED(pList->QueryInterface(&pListPresent));
            m_pPrivateQueuePresent = available ? pQueuePresent : nullptr;
            m_privatePresentState = available ? 1 : 2;
        }
        return m_privatePresentState == 1;
    }

    // Mirrors Windows' in-box driver (D3D12TranslationLayer::ImmediateContextInternal::Present): transition the
    // surface to PRESENT, let the command list record the driver's present DDI and fill the present data, execute,
    // then let the queue submit the kernel present from the runtime's D3DKMT_PRESENT.
    HRESULT Device::PrivatePresent(Resource* pSource, UINT subresource, D3DKMT_PRESENT* pKMTArgs)
    {
        if (!SupportsPrivatePresent())
        {
            return E_NOTIMPL;
        }
        D3D12TranslationLayer::ImmediateContext& context = GetContext();
        const auto graphics = D3D12TranslationLayer::COMMAND_LIST_TYPE::GRAPHICS;
        if (!pKMTArgs->Flags.RedirectedFlip)
        {
            context.m_MaxFrameLatencyHelper.WaitForMaximumFrameLatency();
        }

        D3D12TranslationLayer::Resource* pResource = pSource->GetUnderlyingResource();
        for (UINT plane = 0; plane < pResource->AppDesc()->NonOpaquePlaneCount(); ++plane)
        {
            const UINT planeSubresource = D3D12TranslationLayer::ConvertSubresourceIndexAddPlane(subresource, pResource->AppDesc()->SubresourcesPerPlane(), plane);
            context.GetResourceStateManager().TransitionSubresource(pResource, planeSubresource, D3D12_RESOURCE_STATE_PRESENT, graphics,
                D3D12TranslationLayer::SubresourceTransitionFlags::StateMatchExact);
        }
        context.GetResourceStateManager().ApplyAllResourceTransitions();

        CComPtr<ID3D12CommandListPresent> pListPresent;
        ThrowFailure(context.GetGraphicsCommandList()->QueryInterface(&pListPresent));

        D3D12_PRIVATE_PRESENT present = {};
        present.Flags = 1;
        present.VidPnSourceId = pSource->GetVidPnSourceId() != D3DDDI_ID_UNINITIALIZED ? pSource->GetVidPnSourceId() : pKMTArgs->VidPnSourceId;
        D3D12_PRIVATE_PRESENTSURFACE surface = { pResource->GetUnderlyingResource(), subresource };
        BOOL commandsAdded = FALSE;
        pListPresent->Present(context.GetCommandQueue(graphics), &surface, 1, nullptr, nullptr, 0,
            pKMTArgs->Flags.Flip ? D3D12_PRIVATE_PRESENT_FLAG_FLIP : D3D12_PRIVATE_PRESENT_FLAG_BLT,
            (UINT)pKMTArgs->FlipInterval, &commandsAdded, &present);
        if (commandsAdded)
        {
            context.AdditionalCommandsAdded(graphics);
        }
        context.Flush(D3D12TranslationLayer::COMMAND_LIST_TYPE_GRAPHICS_MASK);

        const UINT64 commandListID = context.GetCommandListID(graphics);
        pResource->UsedInCommandList(graphics, commandListID);
        if (!pKMTArgs->Flags.RedirectedFlip)
        {
            // The frame's GPU work is in the lists submitted so far (the open list is empty after the flush). The
            // runtime polls IsMaximumFrameLatencyReached before each D3DPRESENT_DONOTWAIT present and answers
            // D3DERR_WASSTILLDRAWING without calling the driver while it is reached; recording the open list's
            // ID would never complete when the application presents without rendering, and the game's retry
            // loop would spin for ever.
            context.m_MaxFrameLatencyHelper.RecordPresentFenceValue(commandListID - 1);
        }
        HRESULT hr = m_pPrivateQueuePresent->SubmitPresent(pKMTArgs, &present);
        context.PrepForCommandQueueSync(D3D12TranslationLayer::COMMAND_LIST_TYPE_GRAPHICS_MASK);
        return hr;
    }

    bool Device::IsPresentLatencyReached()
    {
        auto& latency = GetFrameLatencyHelper();
        bool reached = latency.IsMaximumFrameLatencyReached();
        if (reached)
        {
            // The runtime refuses D3DPRESENT_DONOTWAIT presents while this is true and the application may stop
            // submitting work until a present succeeds: put everything recorded so far on the GPU so the outstanding
            // presents can complete.
            GetContextNoDrain().Flush(D3D12TranslationLayer::COMMAND_LIST_TYPE_GRAPHICS_MASK);
            reached = latency.IsMaximumFrameLatencyReached();
        }
        if (!reached)
        {
            m_latencyReachedSinceQpc = 0;
            return false;
        }
        // Still reached: the GPU has not finished the recorded work. A queue stuck behind a wait that never
        // completes (a client's fence, a lost device) must not become an endless D3DERR_WASSTILLDRAWING; after a
        // bounded time the latency gate opens and the present proceeds (or fails) on its own.
        LARGE_INTEGER now = {}, frequency = {};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        if (m_latencyReachedSinceQpc == 0)
        {
            m_latencyReachedSinceQpc = now.QuadPart;
            return true;
        }
        const LONGLONG elapsedMs = (now.QuadPart - m_latencyReachedSinceQpc) * 1000 / std::max<LONGLONG>(frequency.QuadPart, 1);
        if (elapsedMs < 2000)
        {
            return true;
        }
        m_latencyReachedSinceQpc = 0;
        return false;
    }

    // DXGI watches the window of a swap chain for Alt+Enter and window changes and switches the swap chain to its own
    // fullscreen state. The window and its fullscreen state belong to the D3D9 runtime and the application: a real
    // Alt+Enter on a window that had a windowed present made DXGI take the output, and the runtime's following Reset to
    // exclusive fullscreen failed with D3DERR_DEVICELOST (the application's own Alt+Enter handling, e.g. a game's, never
    // went back to fullscreen). Once per swap chain.
    void Device::LeaveWindowToRuntime(IDXGISwapChain3* pSwapChain, HWND hWindow)
    {
        if (!pSwapChain || pSwapChain == m_pWindowAssociatedSwapChain)
        {
            return;
        }
        m_pWindowAssociatedSwapChain = pSwapChain;
        CComPtr<IDXGIFactory1> pFactory;
        if (SUCCEEDED(pSwapChain->GetParent(IID_PPV_ARGS(&pFactory))))
        {
            pFactory->MakeWindowAssociation(hWindow, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
        }
    }

    void Device::ReleaseWindowSwapChains(Resource* presentingResource)
    {
        std::lock_guard<std::mutex> lock(m_SwapChainManagerMutex);
        if (presentingResource && presentingResource != m_pWindowPresentSource)
        {
            return;
        }
        // Reset can replace the driver device before the old one is released. The old HWND's flip
        // chain must retire with its presenting buffers, rather than keeping the replacement out.
        if (m_SwapChainManager)
        {
            GetContextForDestruction();
            m_SwapChainManager.reset();
            GetContextNoDrain().TrimDeletedObjects();
        }
        m_pWindowPresentSource = nullptr;
        m_pWindowAssociatedSwapChain = nullptr;
        std::lock_guard<std::mutex> hdrLock(m_hdrLock);
        m_pHdrSwapChain = nullptr;
        m_hdrActive = false;
    }

    std::shared_ptr<D3D12TranslationLayer::SwapChainManager> Device::GetSwapChainManager()
    {
        std::lock_guard lock( m_SwapChainManagerMutex );
        if (!m_SwapChainManager)
        {
            m_SwapChainManager = std::make_shared<D3D12TranslationLayer::SwapChainManager>( GetContext() );
        }
        return m_SwapChainManager;
    }

    HRESULT Device::CloseAndSubmitGraphicsCommandListForPresent(
        BOOL commandsAdded,
        _In_reads_(numSrcSurfaces) const D3DDDIARG_PRESENTSURFACE* pSrcSurfaces,
        UINT numSrcSurfaces,
        _In_opt_ HANDLE hDestResource,
        _In_ D3DKMT_PRESENT* pKMTPresent)
    {
        D3D12TranslationLayer::ImmediateContext& immediateContext = GetContext();
        m_d3d12tlPresentSurfaces.clear();
        for (UINT i = 0; i < numSrcSurfaces; i++)
        {
            auto presentSurface = pSrcSurfaces[i];
            auto resource = Resource::GetResourceFromHandle(presentSurface.hResource);
            D3D12TranslationLayer::PresentSurface surface = D3D12TranslationLayer::PresentSurface(resource->GetUnderlyingResource(), presentSurface.SubResourceIndex);
            m_d3d12tlPresentSurfaces.push_back(surface);
        }

        D3D12TranslationLayer::Resource* pD3d12tlDestResource = nullptr;
        if (hDestResource != NULL)
        {
            pD3d12tlDestResource = Resource::GetResourceFromHandle(hDestResource)->GetUnderlyingResource();
        }
        return immediateContext.CloseAndSubmitGraphicsCommandListForPresent(
            commandsAdded,
            m_d3d12tlPresentSurfaces.data(),
            numSrcSurfaces,
            pD3d12tlDestResource,
            pKMTPresent);
    }
};