// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once

namespace D3D9on12
{
    class Device;

    // Suballocates small DEFAULT-heap resources (streamed vertex/index buffers and textures) as placed
    // resources inside large heaps. CreateCommittedResource costs ~0.1 ms per call and streaming creates
    // thousands of resources in a frame; CreatePlacedResource in an existing heap is a fraction of that.
    //
    // Block lifetime: the pool attaches a private-data sentinel to every placed resource. D3D12 releases
    // the sentinel when the resource object is destroyed (which the translation layer only does after
    // the GPU is done with it), and the sentinel's final Release returns the block to the pool. No
    // aliasing barriers are needed because a block is never shared between two live resources.
    class PlacedResourcePool
    {
    public:
        explicit PlacedResourcePool(ID3D12Device& device);
        ~PlacedResourcePool();

        static bool IsEnabled();

        // True when the pool will handle a resource with these creation parameters.
        bool Accepts(const D3D12_RESOURCE_DESC& desc, const D3D12_HEAP_DESC& heapDesc) const;

        // Creates the resource in a pooled heap; falls back to CreateCommittedResource when heaps cannot be grown.
        HRESULT Create(const D3D12_RESOURCE_DESC& desc, const D3D12_HEAP_DESC& heapDesc, ID3D12Resource** ppResource);

        struct Chunk;
        void FreeBlock(Chunk* pChunk, UINT64 offset, UINT64 size);

        static constexpr UINT64 kChunkBytes = 64ull << 20;
        static constexpr UINT64 kMaxBufferBytes = 1ull << 20;
        static constexpr UINT64 kMaxTextureBytes = 4ull << 20;
        static constexpr UINT kMaxEmptyChunksPerClass = 1;

    private:
        enum Class { ClassBuffer = 0, ClassTexture = 1, ClassCount = 2 };
        static Class ClassOf(const D3D12_RESOURCE_DESC& desc) { return desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? ClassBuffer : ClassTexture; }

        bool TryAllocate(Class cls, UINT64 size, UINT64 alignment, Chunk** ppChunk, UINT64* pOffset);
        HRESULT AddChunk(Class cls, const D3D12_HEAP_PROPERTIES& properties, UINT64 minimumBytes);

        ID3D12Device& m_device;
        std::mutex m_mutex;
        std::vector<std::unique_ptr<Chunk>> m_chunks[ClassCount];
        std::vector<std::unique_ptr<Chunk>> m_retired;
    };
}
