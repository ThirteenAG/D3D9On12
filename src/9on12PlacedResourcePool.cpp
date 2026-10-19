// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"

namespace D3D9on12
{
    // {7B1D2C0E-5A3F-4B7A-9E2D-0C6F1A8B3D41}
    static const GUID g_PlacedBlockSentinelGuid = { 0x7b1d2c0e, 0x5a3f, 0x4b7a, { 0x9e, 0x2d, 0x0c, 0x6f, 0x1a, 0x8b, 0x3d, 0x41 } };

    struct PlacedResourcePool::Chunk
    {
        CComPtr<ID3D12Heap> m_pHeap;
        UINT64 m_size = 0;
        UINT m_liveBlocks = 0;
        std::map<UINT64, UINT64> m_free; // offset -> size, non-adjacent after coalescing
    };

    namespace
    {
        // Held by the placed resource as private data; D3D12 releases it when the resource object dies.
        class BlockSentinel final : public IUnknown
        {
        public:
            BlockSentinel(PlacedResourcePool& pool, PlacedResourcePool::Chunk* pChunk, UINT64 offset, UINT64 size)
                : m_pool(pool), m_pChunk(pChunk), m_offset(offset), m_size(size) {}

            STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override
            {
                if (ppv == nullptr) return E_POINTER;
                if (InlineIsEqualUnknown(riid)) { *ppv = this; AddRef(); return S_OK; }
                *ppv = nullptr; return E_NOINTERFACE;
            }
            STDMETHOD_(ULONG, AddRef)() override { return (ULONG)InterlockedIncrement(&m_refCount); }
            STDMETHOD_(ULONG, Release)() override
            {
                const ULONG remaining = (ULONG)InterlockedDecrement(&m_refCount);
                if (remaining == 0)
                {
                    m_pool.FreeBlock(m_pChunk, m_offset, m_size);
                    delete this;
                }
                return remaining;
            }

        private:
            PlacedResourcePool& m_pool;
            PlacedResourcePool::Chunk* const m_pChunk;
            const UINT64 m_offset;
            const UINT64 m_size;
            volatile LONG m_refCount = 1;
        };

        UINT64 AlignUp(UINT64 value, UINT64 alignment) { return (value + alignment - 1) & ~(alignment - 1); }
    }

    PlacedResourcePool::PlacedResourcePool(ID3D12Device& device) : m_device(device) {}

    PlacedResourcePool::~PlacedResourcePool()
    {
        // Every placed resource has been destroyed by now (the translation layer context waits for the GPU and
        // trims its deletion queue before the device goes away); the heaps are released with the chunks.
    }

    bool PlacedResourcePool::IsEnabled()
    {
        static const bool enabled = []()
        {
            char value[8] = {};
            return !(GetEnvironmentVariableA("D3D9ON12_NO_PLACED_POOL", value, sizeof(value)) && value[0] == '1');
        }();
        return enabled;
    }

    bool PlacedResourcePool::Accepts(const D3D12_RESOURCE_DESC& desc, const D3D12_HEAP_DESC& heapDesc) const
    {
        if (heapDesc.Properties.Type != D3D12_HEAP_TYPE_DEFAULT) return false;
        if (heapDesc.Flags & (D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_ALLOW_DISPLAY | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER)) return false;
        if (desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER)) return false;
        if (desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality != 0) return false;
        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_UNKNOWN) return false;
        if (heapDesc.Alignment != 0 && heapDesc.Alignment != D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT) return false;
        if (heapDesc.SizeInBytes == 0 || heapDesc.SizeInBytes > (ClassOf(desc) == ClassBuffer ? kMaxBufferBytes : kMaxTextureBytes)) return false;
        return true;
    }

    bool PlacedResourcePool::TryAllocate(Class cls, UINT64 size, UINT64 alignment, Chunk** ppChunk, UINT64* pOffset)
    {
        // Best fit over the free ranges of every chunk, newest chunks last so old ones fill up and stay dense.
        Chunk* pBest = nullptr; UINT64 bestOffset = 0, bestWaste = ~0ull;
        std::map<UINT64, UINT64>::iterator bestIt;
        for (auto& chunk : m_chunks[cls])
        {
            for (auto it = chunk->m_free.begin(); it != chunk->m_free.end(); ++it)
            {
                const UINT64 aligned = AlignUp(it->first, alignment);
                if (aligned + size > it->first + it->second) continue;
                const UINT64 waste = it->second - size;
                if (waste < bestWaste) { bestWaste = waste; pBest = chunk.get(); bestIt = it; bestOffset = aligned; if (waste == 0) break; }
            }
            if (bestWaste == 0) break;
        }
        if (!pBest) return false;

        const UINT64 rangeOffset = bestIt->first, rangeSize = bestIt->second;
        pBest->m_free.erase(bestIt);
        if (bestOffset > rangeOffset) pBest->m_free.emplace(rangeOffset, bestOffset - rangeOffset);
        if (bestOffset + size < rangeOffset + rangeSize) pBest->m_free.emplace(bestOffset + size, rangeOffset + rangeSize - (bestOffset + size));
        pBest->m_liveBlocks++;
        *ppChunk = pBest; *pOffset = bestOffset;
        return true;
    }

    HRESULT PlacedResourcePool::AddChunk(Class cls, const D3D12_HEAP_PROPERTIES& properties, UINT64 minimumBytes)
    {
        auto chunk = std::make_unique<Chunk>();
        chunk->m_size = max(kChunkBytes, AlignUp(minimumBytes, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT));
        D3D12_HEAP_DESC heapDesc = {};
        heapDesc.SizeInBytes = chunk->m_size;
        heapDesc.Properties = properties;
        heapDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        heapDesc.Flags = cls == ClassBuffer ? D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS : D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
        HRESULT hr = m_device.CreateHeap(&heapDesc, IID_PPV_ARGS(&chunk->m_pHeap));
        if (FAILED(hr)) return hr;
        chunk->m_free.emplace(0ull, chunk->m_size);
        m_chunks[cls].push_back(std::move(chunk));
        return S_OK;
    }

    HRESULT PlacedResourcePool::Create(const D3D12_RESOURCE_DESC& desc, const D3D12_HEAP_DESC& heapDesc, ID3D12Resource** ppResource)
    {
        const Class cls = ClassOf(desc);
        const UINT64 size = AlignUp(heapDesc.SizeInBytes, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
        const UINT64 alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;

        Chunk* pChunk = nullptr; UINT64 offset = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_retired.clear();
            if (!TryAllocate(cls, size, alignment, &pChunk, &offset))
            {
                HRESULT hr = AddChunk(cls, heapDesc.Properties, size);
                if (FAILED(hr))
                {
                    // Out of memory for a new heap: fall back to a committed resource for this one.
                    D3D12_HEAP_PROPERTIES properties = heapDesc.Properties;
                    return m_device.CreateCommittedResource(&properties, heapDesc.Flags & ~D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(ppResource));
                }
                const bool allocated = TryAllocate(cls, size, alignment, &pChunk, &offset);
                assert(allocated); UNREFERENCED_PARAMETER(allocated);
            }
        }

        HRESULT hr;
        {
            hr = m_device.CreatePlacedResource(pChunk->m_pHeap, offset, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(ppResource));
        }
        if (FAILED(hr))
        {
            FreeBlock(pChunk, offset, size);
            return hr;
        }

        // The resource's private data owns the sentinel from here on; our own reference goes away after the hand-over.
        BlockSentinel* pSentinel = new BlockSentinel(*this, pChunk, offset, size);
        {
            hr = (*ppResource)->SetPrivateDataInterface(g_PlacedBlockSentinelGuid, pSentinel);
        }
        if (FAILED(hr))
        {
            // Nothing tracks the block without the sentinel: drop the placed resource, let the sentinel free the
            // block, and give this resource a committed allocation instead.
            (*ppResource)->Release(); *ppResource = nullptr;
            pSentinel->Release();
            D3D12_HEAP_PROPERTIES properties = heapDesc.Properties;
            return m_device.CreateCommittedResource(&properties, heapDesc.Flags & ~D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(ppResource));
        }
        pSentinel->Release();
        return S_OK;
    }

    void PlacedResourcePool::FreeBlock(Chunk* pChunk, UINT64 offset, UINT64 size)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto next = pChunk->m_free.lower_bound(offset);
        if (next != pChunk->m_free.end() && next->first == offset + size) { size += next->second; next = pChunk->m_free.erase(next); }
        if (next != pChunk->m_free.begin())
        {
            auto prev = std::prev(next);
            if (prev->first + prev->second == offset) { offset = prev->first; size += prev->second; pChunk->m_free.erase(prev); }
        }
        pChunk->m_free.emplace(offset, size);
        assert(pChunk->m_liveBlocks > 0);
        pChunk->m_liveBlocks--;

        if (pChunk->m_liveBlocks == 0)
        {
            // Keep a few empty chunks for the next streaming burst. Surplus ones are retired here and their heaps
            // released on the next Create, not inside the destruction of the resource that emptied them.
            for (auto& list : m_chunks)
            {
                auto it = std::find_if(list.begin(), list.end(), [pChunk](const std::unique_ptr<Chunk>& c) { return c.get() == pChunk; });
                if (it == list.end()) continue;
                UINT emptyChunks = 0;
                for (auto& c : list) if (c->m_liveBlocks == 0) emptyChunks++;
                if (emptyChunks > kMaxEmptyChunksPerClass) { m_retired.push_back(std::move(*it)); list.erase(it); }
                break;
            }
        }
    }
}
