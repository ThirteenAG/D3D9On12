// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"

namespace D3D9on12
{
    FastUploadAllocator::FastUploadAllocator(Device& device, UINT size, UINT alignment, bool bDeferDestroyDuringRealloc) :
        m_reuseArenas([] {
            char value[2]{};
            return !(GetEnvironmentVariableA("D3D9ON12_UPLOAD_ARENA_REUSE", value, ARRAYSIZE(value)) == 1 && value[0] == '0');
        }()),
        m_parentDevice(device),
        m_bDeferDestroyDuringRealloc(bDeferDestroyDuringRealloc),
        m_size(size),
        m_alignmentRequired(alignment),
        m_spaceUsed(size),
        m_pMappedAddress(nullptr)
    {
    }

    FastUploadAllocator::~FastUploadAllocator() { Destroy(); }

    void FastUploadAllocator::RetireArena(D3D12TranslationLayer::Resource* resource, void* mapped)
    {
        if (!m_reuseArenas) { CleanupResourceBindingsAndRelease{}(resource); return; }
        // This runs after the final recorded packet naming this arena.
        auto& context = *resource->m_pParent;
        context.ClearInputBindings(resource);
        context.ClearOutputBindings(resource);
        RetiredArena retired;
        retired.resource = resource;
        retired.mapped = mapped;
        retired.size = resource->Parent()->m_desc12.Width;
        for (UINT i = 0; i < retired.fences.size(); ++i) retired.fences[i] = resource->m_LastUsedCommandListID[i];
        std::lock_guard lock(m_retiredMutex);
        for (auto& slot : m_retiredArenas)
        {
            if (!slot.resource || slot.size != retired.size)
            {
                if (slot.resource) slot.resource->Release();
                slot = retired;
                return;
            }
        }
        // Keep a small fixed working set. Excess arenas retain normal deferred
        // destruction rather than making the cache grow with transient demand.
        resource->Release();
    }

    unique_unbind_resourceptr FastUploadAllocator::TakeRetiredArena()
    {
        if (!m_reuseArenas) return {};
        auto& context = m_parentDevice.GetContextNoDrain();
        std::lock_guard lock(m_retiredMutex);
        for (auto& slot : m_retiredArenas)
        {
            if (!slot.resource || slot.size != m_size) continue;
            bool complete = true;
            for (UINT i = 0; i < slot.fences.size(); ++i)
            {
                if (!slot.fences[i]) continue;
                const UINT64 completed = context.GetCompletedFenceValue((D3D12TranslationLayer::COMMAND_LIST_TYPE)i);
                complete &= completed != UINT64_MAX && completed >= slot.fences[i];
            }
            if (complete)
            {
                unique_unbind_resourceptr resource(slot.resource);
                slot.resource->Release(); // transfer the existing owned reference
                slot.resource = nullptr;
                m_pMappedAddress = slot.mapped;
                slot.mapped = nullptr;
                return resource;
            }
        }
        return {};
    }

    void FastUploadAllocator::ClearDeferredDestroyedResource()
    {
#if defined(D3D9ON12_BATCHED_RENDERING)
        if (auto* batch = m_parentDevice.GetBatchedContext())
        {
            if (m_pDeferredDestroyedResource)
            {
                // Queued packets may still reference the old arena: unbind and release it
                // after the batch retires. Detach before publishing, because a reentrant
                // query poll can submit the batch as soon as recording unlocks.
                auto* resource = m_pDeferredDestroyedResource.release();
                void* mapped = m_pMappedAddress;
                try
                {
                    if (m_reuseArenas)
                    {
                        m_pendingRetirements.fetch_add(1, std::memory_order_relaxed);
                        try
                        {
                            batch->AddPostBatchFunction([this, resource, mapped]() {
                                auto done = D3D12TranslationLayer::MakeScopeExit([this] {
                                    m_pendingRetirements.fetch_sub(1, std::memory_order_release);
                                });
                                RetireArena(resource, mapped);
                            });
                        }
                        catch (...)
                        {
                            m_pendingRetirements.fetch_sub(1, std::memory_order_release);
                            throw;
                        }
                    }
                    else batch->AddPostBatchFunction([resource]() { CleanupResourceBindingsAndRelease{}(resource); });
                }
                catch (...)
                {
                    // Not published. Restore ownership; unique_comptr::reset AddRefs, so
                    // balance the reference we detached.
                    m_pDeferredDestroyedResource.reset(resource);
                    resource->Release();
                    throw;
                }
            }
            return;
        }
#endif
        m_pDeferredDestroyedResource.reset(nullptr);
    }

    void FastUploadAllocator::Destroy()
    {
        const bool pending = m_pendingRetirements.load(std::memory_order_acquire) != 0;
        bool retired = false;
        {
            std::lock_guard lock(m_retiredMutex);
            for (auto& slot : m_retiredArenas) retired |= slot.resource != nullptr;
        }
        // Constructors can fail before the parent has an immediate context.
        if (!pending && !retired && !m_pResource && !m_pDeferredDestroyedResource) return;
#if defined(D3D9ON12_BATCHED_RENDERING)
        m_parentDevice.GetContextForDestruction();
#endif
        m_pResource.reset(nullptr);
        m_pDeferredDestroyedResource.reset(nullptr);
        std::lock_guard lock(m_retiredMutex);
        for (auto& slot : m_retiredArenas)
        {
            if (slot.resource) slot.resource->Release();
            slot.resource = nullptr;
        }
    }

    auto FastUploadAllocator::Allocate(UINT size) -> SubBuffer
    {
        SubBuffer bufferOut = {};
        const UINT alignedSize = Align(size, m_alignmentRequired);
        const ULONG64 totalSize = alignedSize + static_cast<ULONG64>(m_spaceUsed);

        if (totalSize > static_cast<ULONG64>(m_size))
        {
            if (size > m_size)
            {
                assert(size < MAXUINT32);
                m_size = CeilToClosestPowerOfTwo((UINT32)size);
            }

            Realloc();
        }

        bufferOut.m_pResource = m_pResource.get();
        bufferOut.m_pMappedAddress = (byte*)m_pMappedAddress + m_spaceUsed;

        auto offsetFromBase = (byte*)bufferOut.m_pMappedAddress - (byte*)m_pMappedAddress;
        assert(offsetFromBase >= 0 && offsetFromBase <= UINT_MAX);
        bufferOut.m_offsetFromBase = static_cast<UINT>(offsetFromBase);

        m_spaceUsed += alignedSize;

        assert(IsAligned(bufferOut.m_offsetFromBase, m_alignmentRequired));
        return bufferOut;
    }

    void FastUploadAllocator::Realloc()
    {
#if defined(D3D9ON12_BATCHED_RENDERING)
        if (m_parentDevice.GetBatchedContext())
        {
            // Rolling the arena over must not drain the worker (the constant rings roll over several times per
            // frame): the old arena is released after the batch that still references it, the new one is created
            // free-threaded and mapped directly (an upload buffer stays mapped for its lifetime).
            m_pDeferredDestroyedResource = std::move(m_pResource);
            ClearDeferredDestroyedResource();
            m_pMappedAddress = nullptr;
            D3D12TranslationLayer::ResourceCreationArgs args = GetCreateUploadBufferArgs(&m_parentDevice.GetDevice(), m_size, m_alignmentRequired);
            m_pResource = TakeRetiredArena();
            if (!m_pResource)
            {
                // Publish an arena only after mapping succeeds. A failed map
                // must not leave an unmapped resource eligible for retirement/reuse.
                unique_unbind_resourceptr newResource = D3D12TranslationLayer::Resource::CreateResource(&m_parentDevice.GetContextNoDrain(), args, D3D12TranslationLayer::ResourceAllocationContext::FreeThread);
                CD3DX12_RANGE emptyRange(0, 0);
                void* mapped = nullptr;
                ThrowFailure(newResource->GetUnderlyingResource()->Map(0, &emptyRange, &mapped));
                // The upload allocator keeps its own permanent map. Balance this
                // nested lookup while retaining the address for the arena's life.
                newResource->GetUnderlyingResource()->Unmap(0, &emptyRange);
                m_pMappedAddress = static_cast<BYTE*>(mapped) + newResource->GetSubresourcePlacement(0).Offset;
                m_pResource = std::move(newResource);
            }
            m_spaceUsed = 0;
            return;
        }
#endif
        if (m_bDeferDestroyDuringRealloc)
        {
            m_pDeferredDestroyedResource = std::move(m_pResource);
        }

        D3D12TranslationLayer::ResourceCreationArgs args = GetCreateUploadBufferArgs(&m_parentDevice.GetDevice(), m_size, m_alignmentRequired);
        m_pResource = D3D12TranslationLayer::Resource::CreateResource(&m_parentDevice.GetContext(), args, D3D12TranslationLayer::ResourceAllocationContext::ImmediateContextThreadTemporary);

        D3D12TranslationLayer::MappedSubresource MappedResult;
        m_parentDevice.GetContext().Map(m_pResource.get(), 0, D3D12TranslationLayer::MAP_TYPE_WRITE_DISCARD, false, nullptr, &MappedResult);
        m_pMappedAddress = MappedResult.pData;
        m_spaceUsed = 0;
    }

};
