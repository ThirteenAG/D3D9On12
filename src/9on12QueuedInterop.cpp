// Licensed under the MIT license.
// Queued interop (D3D9On12QueueInterop): copies between this device's resources and foreign
// D3D12 resources plus a fence signal/wait, carried in the D3D9 command stream by a marker render
// state and executed at that point, on the translation-layer worker when batching is enabled.
#include "pch.h"
#include "D3D9On12Extensions.h"
namespace D3D9on12
{
    namespace
    {
        std::mutex g_deviceRegistryLock;
        std::vector<Device*> g_deviceRegistry;

        void CopyWholeSubresource(ID3D12GraphicsCommandList* pList, ID3D12Resource* pDst, ID3D12Resource* pSrc)
        {
            D3D12_TEXTURE_COPY_LOCATION dst = {};
            dst.pResource = pDst;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION src = dst;
            src.pResource = pSrc;
            pList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }

        D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* pResource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
        {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = pResource;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = before;
            barrier.Transition.StateAfter = after;
            return barrier;
        }
    }

    void Device::RegisterDevice(Device* pDevice)
    {
        std::lock_guard<std::mutex> lock(g_deviceRegistryLock);
        g_deviceRegistry.push_back(pDevice);
    }

    void Device::UnregisterDevice(Device* pDevice)
    {
        {
            std::lock_guard<std::mutex> lock(g_deviceRegistryLock);
            g_deviceRegistry.erase(std::remove(g_deviceRegistry.begin(), g_deviceRegistry.end(), pDevice), g_deviceRegistry.end());
        }
    }


    void Device::ForEachRegisteredDevice(const std::function<void(Device&)>& fn)
    {
        std::lock_guard<std::mutex> lock(g_deviceRegistryLock);
        for (Device* pDevice : g_deviceRegistry)
        {
            fn(*pDevice);
        }
    }

    Device* Device::FindDeviceForD3D12Device(ID3D12Device* pD3D12Device)
    {
        if (!pD3D12Device)
        {
            return nullptr;
        }
        std::lock_guard<std::mutex> lock(g_deviceRegistryLock);
        for (Device* pDevice : g_deviceRegistry)
        {
            if (pDevice->m_pDevice.p == pD3D12Device)
            {
                return pDevice;
            }
        }
        return nullptr;
    }

    void Device::RegisterInteropResource(Resource* pResource)
    {
        auto* pTranslationLayerResource = pResource ? pResource->GetUnderlyingResource() : nullptr;
        ID3D12Resource* pUnderlying = pTranslationLayerResource ? pTranslationLayerResource->GetUnderlyingResource() : nullptr;
        if (!pUnderlying)
        {
            return;
        }
        std::lock_guard<std::mutex> lock(m_interopLock);
        m_interopResources[pUnderlying] = pResource;
    }

    void Device::UnregisterInteropResource(Resource* pResource)
    {
        std::lock_guard<std::mutex> lock(m_interopLock);
        for (auto it = m_interopResources.begin(); it != m_interopResources.end();)
        {
            it = (it->second == pResource) ? m_interopResources.erase(it) : std::next(it);
        }
        for (auto& pending : m_pendingInterop)
        {
            for (auto& pair : pending.second->m_pairs)
            {
                if (pair.m_pResource == pResource) pair.m_pResource = nullptr;
            }
        }
    }

    HRESULT Device::QueueInterop(const D3D9ON12_INTEROP_REQUEST& request, uint32_t* pToken)
    {
        if ((request.Operation != D3D9ON12_INTEROP_EXPORT_COPY && request.Operation != D3D9ON12_INTEROP_IMPORT_COPY) ||
            request.Count == 0 || request.Count > 8 || !request.GameResources || !request.ForeignResources || !request.Fence)
        {
            return E_INVALIDARG;
        }
        auto pending = std::make_unique<QueuedInteropRequest>();
        pending->m_operation = request.Operation;
        pending->m_pFence = request.Fence;
        pending->m_fenceValue = request.FenceValue;
        {
            std::lock_guard<std::mutex> lock(m_interopLock);
            for (UINT i = 0; i < request.Count; ++i)
            {
                if (!request.GameResources[i] || !request.ForeignResources[i])
                {
                    continue;
                }
                auto found = m_interopResources.find(request.GameResources[i]);
                if (found == m_interopResources.end())
                {
                    return E_FAIL;
                }
                pending->m_pairs.push_back({ found->second, request.GameResources[i], request.ForeignResources[i] });
            }
            if (pending->m_pairs.empty())
            {
                return E_INVALIDARG;
            }
            *pToken = m_nextInteropToken++;
            if (m_nextInteropToken == 0) m_nextInteropToken = 1;
            // Bound the number of tokens an application can leave unconsumed.
            if (m_pendingInterop.size() >= 64)
            {
                m_pendingInterop.erase(m_pendingInterop.begin());
            }
            m_pendingInterop.emplace(*pToken, std::move(pending));
        }
        return S_OK;
    }

    void Device::ExecuteQueuedInterop(D3D12TranslationLayer::ImmediateContext& context, QueuedInteropRequest& request)
    {
        const bool isExport = request.m_operation == D3D9ON12_INTEROP_EXPORT_COPY;
        const UINT graphicsMask = D3D12TranslationLayer::COMMAND_LIST_TYPE_GRAPHICS_MASK;

        if (!isExport)
        {
            // Everything recorded so far must not depend on the foreign content; the wait only
            // orders the copy (and everything after it) behind the other party's fence.
            context.Flush(graphicsMask);
            m_pCommandQueue->Wait(request.m_pFence, request.m_fenceValue);
        }

        D3D12_RESOURCE_BARRIER before[8];
        D3D12_RESOURCE_BARRIER after[8];
        UINT barriers = 0;
        UINT live = 0;
        for (auto& pair : request.m_pairs)
        {
            auto* pTranslationLayerResource = pair.m_pResource->GetUnderlyingResource();
            // A renamed/recreated resource no longer matches what the application unwrapped.
            if (!pTranslationLayerResource || pTranslationLayerResource->GetUnderlyingResource() != pair.m_pGameResource)
            {
                pair.m_pResource = nullptr;
                continue;
            }
            context.GetResourceStateManager().TransitionResource(pTranslationLayerResource,
                isExport ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_COPY_DEST);
            const D3D12_RESOURCE_STATES foreignState = isExport ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COPY_SOURCE;
            before[barriers] = Transition(pair.m_pForeignResource, D3D12_RESOURCE_STATE_COMMON, foreignState);
            after[barriers++] = Transition(pair.m_pForeignResource, foreignState, D3D12_RESOURCE_STATE_COMMON);
            ++live;
        }
        if (live)
        {
            // Binding-driven states are restored by the translation layer before the next draw.
            context.GetResourceStateManager().ApplyAllResourceTransitions();
            ID3D12GraphicsCommandList* pList = context.GetGraphicsCommandList();
            const UINT64 commandListID = context.GetCommandListID(D3D12TranslationLayer::COMMAND_LIST_TYPE::GRAPHICS);
            pList->ResourceBarrier(barriers, before);
            for (auto& pair : request.m_pairs)
            {
                if (!pair.m_pResource) continue;
                auto* pTranslationLayerResource = pair.m_pResource->GetUnderlyingResource();
                if (isExport)
                {
                    CopyWholeSubresource(pList, pair.m_pForeignResource, pair.m_pGameResource);
                }
                else
                {
                    pair.m_pResource->MarkGpuWritten();
                    CopyWholeSubresource(pList, pair.m_pGameResource, pair.m_pForeignResource);
                }
                pTranslationLayerResource->UsedInCommandList(D3D12TranslationLayer::COMMAND_LIST_TYPE::GRAPHICS, commandListID);
            }
            pList->ResourceBarrier(barriers, after);
        }

        if (isExport)
        {
            context.Flush(graphicsMask);
            m_pCommandQueue->Signal(request.m_pFence, request.m_fenceValue);
        }
    }

#if defined(D3D9ON12_BATCHED_RENDERING)
    namespace
    {
        struct QueuedInteropExtension final : D3D12TranslationLayer::BatchedExtension
        {
            void Dispatch(D3D12TranslationLayer::ImmediateContext& context, const void* pData, size_t) override
            {
                auto* pPayload = static_cast<const Payload*>(pData);
                std::unique_ptr<Device::QueuedInteropRequest> request(pPayload->m_pRequest);
                pPayload->m_pDevice->ExecuteQueuedInterop(context, *request);
            }
            struct Payload
            {
                Device* m_pDevice;
                Device::QueuedInteropRequest* m_pRequest;
            };
        } g_queuedInteropExtension;
    }
#endif

    bool Device::TryExecuteInteropMarker(UINT token)
    {
        std::unique_ptr<QueuedInteropRequest> request;
        {
            std::lock_guard<std::mutex> lock(m_interopLock);
            auto found = m_pendingInterop.find(token);
            if (found == m_pendingInterop.end())
            {
                return false;
            }
            request = std::move(found->second);
            m_pendingInterop.erase(found);
        }
#if defined(D3D9ON12_BATCHED_RENDERING)
        if (auto* pBatch = GetBatchedContext())
        {
            CheckBatchError();
            QueuedInteropExtension::Payload payload = { this, request.get() };
            const bool isExport = request->m_operation == D3D9ON12_INTEROP_EXPORT_COPY;
            pBatch->BatchExtension(&g_queuedInteropExtension, &payload, sizeof(payload));
            request.release(); // owned by the recorded command until it is dispatched
            if (isExport)
            {
                // The other party waits for the export's fence signal: hand the batch to the worker now instead of
                // when the application's next flush or a full batch submits it
                pBatch->SubmitBatch();
            }
            return true;
        }
#endif
        // Without a worker the DDI caller owns the ImmediateContext.
        ExecuteQueuedInterop(GetContext(), *request);
        return true;
    }
};
