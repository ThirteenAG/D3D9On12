// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once

namespace D3D9on12
{
    class Device;

    struct InputBuffer
    {
        void InitWithUploadData(Device& device, UINT strideInBytes, CONST VOID* pDataToUpload = nullptr) noexcept;
        void InitWithResource(Device& device, Resource* pAppResource, UINT64 offset, UINT strideInBytes) noexcept;
        void InitAsPersistentTriangleFan(Device & device, UINT64 bufferSize, UINT strideInBytes);

        UINT GetOffsetInBytesAPI() { return m_bufferOffset; }
        UINT GetOffsetInBytesD3D12();
        UINT GetStrideInBytes() { return m_bufferStride; }

        void UpdatePersistentView(UINT offset, UINT stride) noexcept
        {
            Check9on12(m_pAppProvidedResource && !m_isSystemMemory);
            m_bufferOffset = offset;
            m_bufferStride = stride;
        }

        VOID* GetSystemMemoryBase() { return (byte*)m_pDeferredSystemMemoryData + m_bufferOffset; }
        VOID* GetTriangleFanMemory() { Check9on12(m_isTriangleFanIndexBuffer); return m_gpuBuffer.m_pMappedAddress; }

        HRESULT Upload(Device& device, OffsetArg baseVertexIndex, UINT vertexCount);
        void ResetUploadedData();

        bool IsSystemMemory() { return m_isSystemMemory; }
        bool IsTriangleFan() { return m_isTriangleFanIndexBuffer; }

        Resource* GetAppResource() { return m_pAppProvidedResource; }
        D3D12TranslationLayer::Resource* GetUnderlyingResource() const noexcept { return m_gpuBuffer.m_pResource; }

    private:
        Resource* m_pAppProvidedResource = nullptr;
        std::shared_ptr<D3D12TranslationLayer::Resource> m_pInternallyProvidedResource;

        const void* m_pDeferredSystemMemoryData = nullptr;
        bool m_isSystemMemory = false;
        bool m_isTriangleFanIndexBuffer = false;

        // Resolved GPU view: borrowed app TL resource, per-draw upload, or owned
        // triangle-fan storage. App-resource offsets/mapping remain zero/null.
        FastUploadAllocator::SubBuffer m_gpuBuffer = {};

        UINT m_bufferStride = 0;
        UINT m_bufferOffset = 0;
        // Only used for bounds checking in Upload() when possible. This default may not make sense if that changes.
        // Primarily a risk due to InputAssembly::Set*BufferUM not knowing buffer size.
        UINT m_sizeInBytes = UINT32_MAX;
    };

    class InputAssembly
    {
    public:
        InputAssembly(PipelineStateDirtyFlags& pipelineStateDirtyFlags, RasterStatesWrapper& rasterStates);

        InputLayout &GetInputLayout() { return *m_pInputLayout; }
        InputLayout* GetInputLayoutPointer() noexcept {return m_pInputLayout;}

        void SetVertexDeclaration(InputLayout *pInputLayout);

        UINT GetStream0Stride() { return m_inputStreams[0].GetStrideInBytes(); }

        // Binding masks and input views are preallocated CPU state. Keep these
        // setters nonthrowing; the DDI fast wrapper depends on that contract.
        HRESULT SetVertexBuffer(Device &device, Resource *pVertexBuffer, UINT vbSlot, UINT offset, UINT stride) noexcept;
        void SetVertexBufferUM(Device& device, UINT vbSlot, UINT stride, _In_ CONST VOID* pData) noexcept;
        HRESULT SetIndexBuffer(Device &device, Resource *pResource, UINT stride) noexcept;
        HRESULT SetIndexBufferUM(Device &device, UINT indexBufferStride, _In_ const void *pIndices) noexcept;

        HRESULT UploadDeferredInputBufferData(Device& device, OffsetArg baseVertexIndex, UINT vertexCount, OffsetArg baseIndexLocation, UINT indexCount);
        void ResetUploadBufferData();

        void SetPrimitiveTopology(Device &device, D3DPRIMITIVETYPE primitiveType);

        HRESULT ResolveDeferredState(Device &device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& psoDesc, OffsetArg BaseVertexStart, OffsetArg BaseIndexStart);

        void RestorePreviousIB(Device &device);

        InputBuffer& CurrentIndexBuffer() { return m_IndexBufferStack.top(); }
        InputBuffer& CurrentVertexBuffer(UINT slot) noexcept { return m_inputStreams[slot]; }
        
        static void CreateTriangleFanIndexBuffer(_In_ Device &device, _Out_ InputBuffer& targetBuffer, _In_ UINT& indexCount);
        static void CreateTriangleListIBFromTriangleFanIB(_In_ Device &device, 
            _In_ CONST void * pIndexBuffer, 
            _In_ UINT indexBufferStride, _In_ UINT baseIndexLocation, _In_ UINT indexCount, _Out_ InputBuffer & convertedBuffer);
        void PushNewIndexBuffer(_In_ InputBuffer & targetBuffer);

        void MarkIndexBufferDirty() { m_dirtyFlags.IndexBuffer = true; }
        void MarkVertexBufferDirty() { m_dirtyFlags.VertexBuffers = (1u << MAX_VERTEX_STREAMS) - 1; }
        UINT GetNumBoundStreams() { return m_numBoundVBs; }

    private:
        void ResolveVertexBuffers(Device& device, OffsetArg BaseVertexStart);

        InputBuffer m_inputStreams[MAX_VERTEX_STREAMS];
        UINT m_numBoundVBs;
        UINT m_systemMemoryStreams = 0;


        std::stack<InputBuffer> m_IndexBufferStack;
        D3D12_PRIMITIVE_TOPOLOGY m_topology;
        InputLayout *m_pInputLayout;

        PipelineStateDirtyFlags& m_dirtyFlags;
        RasterStatesWrapper& m_rasterStates;

        bool m_hasTLVertices;
    };

};
