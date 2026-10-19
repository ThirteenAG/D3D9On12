// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once
#include <array>
#include <atomic>
#include <mutex>

namespace D3D9on12
{
    class Device;

    class FastUploadAllocator
    {
    public:
        struct SubBuffer
        {
            SubBuffer() :
                m_pResource(0),
                m_pMappedAddress(nullptr),
                m_offsetFromBase(0){};

            SubBuffer(D3D12TranslationLayer::Resource *pResource, void* mapped, UINT offset) :
                m_pResource(pResource), m_pMappedAddress(mapped), m_offsetFromBase(offset){};

            D3D12TranslationLayer::Resource *m_pResource;
            void* m_pMappedAddress;
            UINT m_offsetFromBase;
        };

        FastUploadAllocator(Device& device, UINT size, UINT alignment, bool bDeferDestroyDuringRealloc = false);
        ~FastUploadAllocator();

        SubBuffer Allocate(UINT size);
        void Destroy();
        void ClearDeferredDestroyedResource();

    private:
        void Realloc();
        void RetireArena(D3D12TranslationLayer::Resource* resource, void* mapped);
        unique_unbind_resourceptr TakeRetiredArena();
        struct RetiredArena
        {
            D3D12TranslationLayer::Resource* resource = nullptr;
            UINT64 size = 0;
            void* mapped = nullptr;
            std::array<UINT64, (UINT)D3D12TranslationLayer::COMMAND_LIST_TYPE::MAX_VALID> fences{};
        };
        std::array<RetiredArena, 2> m_retiredArenas{};
        std::mutex m_retiredMutex;
        std::atomic<UINT> m_pendingRetirements{0};
        const bool m_reuseArenas;


        Device& m_parentDevice;
        const bool m_bDeferDestroyDuringRealloc;
        UINT m_size;
        const UINT m_alignmentRequired;
        UINT m_spaceUsed;

        unique_unbind_resourceptr m_pResource;
        void* m_pMappedAddress;

        unique_unbind_resourceptr m_pDeferredDestroyedResource;
    };
};