// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once
#include "SharedResourceHelpers.hpp"
#include "9on12RenderContext.h"
#include "9on12AllocationInfoCache.h"
namespace D3D9on12
{
    struct Context
    {
        HANDLE m_runtimeHandle;
    };

    class VideoDevice;

    class Device
    {
    public:
        Device( Adapter& Adapter, _Inout_ D3DDDIARG_CREATEDEVICE& CreateDeviceArgs );
        ~Device();

        static void ReportError(HRESULT /*hr*/) {};

        HRESULT Init(ID3D12Device *pDevice, ID3D12CommandQueue *pCommandQueue);

        HRESULT Destroy();
        HRESULT FlushWork(bool WaitOnCompletion, UINT FlushFlags = 0);

        // Queued interop (D3D9On12QueueInterop, include/D3D9On12Extensions.h): requests are
        // registered by token and executed when the marker render state carrying the token arrives
        // in the command stream, on the worker when batching is enabled.
        struct QueuedInteropRequest
        {
            // The D3D12 objects are referenced until the token executes, so a client releasing them early leaks
            // instead of making the worker copy from or wait on destroyed objects; m_pResource is cleared when
            // the D3D9 resource is destroyed (UnregisterInteropResource).
            struct Pair { Resource* m_pResource; CComPtr<ID3D12Resource> m_pGameResource; CComPtr<ID3D12Resource> m_pForeignResource; };
            UINT m_operation = 0;
            std::vector<Pair> m_pairs;
            CComPtr<ID3D12Fence> m_pFence;
            UINT64 m_fenceValue = 0;
        };
        static void RegisterDevice(Device* pDevice);
        static void UnregisterDevice(Device* pDevice);
        static Device* FindDeviceForD3D12Device(ID3D12Device* pD3D12Device);
        void RegisterInteropResource(Resource* pResource);
        void UnregisterInteropResource(Resource* pResource);
        HRESULT QueueInterop(const D3D9ON12_INTEROP_REQUEST& request, uint32_t* pToken);
        bool TryExecuteInteropMarker(UINT token);
        void ExecuteQueuedInterop(D3D12TranslationLayer::ImmediateContext& context, QueuedInteropRequest& request);
        HRESULT Present(CONST D3DDDIARG_PRESENT1& PresentArgs, D3DKMT_PRESENT *pKMTArgs);
        HRESULT CloseAndSubmitGraphicsCommandListForPresent(BOOL commandsAdded, _In_reads_(numSrcSurfaces) const D3DDDIARG_PRESENTSURFACE* pSrcSurfaces, UINT numSrcSurfaces, _In_opt_ HANDLE hDestResource, _In_ D3DKMT_PRESENT* pKMTPresent);

        void MarkStateAsDirty();

        static FORCEINLINE HANDLE GetHandleFromDevice(Device* pDevice){ return static_cast<HANDLE>(pDevice); }
        static FORCEINLINE Device* GetDeviceFromHandle(HANDLE hDevice){ return static_cast<Device*>(hDevice); }
        std::shared_ptr<D3D12TranslationLayer::SwapChainManager> GetSwapChainManager();
        // Presenting primaries (exclusive fullscreen) through the D3D12 runtime's private present interfaces
        // (9on12PrivatePresent.h) when the D3D9 runtime offers no present callback. False when the runtime
        // lacks the interfaces; primaries are then refused at creation so fullscreen devices fail cleanly.
        bool SupportsPrivatePresent();
        // IsMaximumFrameLatencyReached DDI: true while the translation layer's present ring is full, but never for
        // longer than a bounded time without progress (the runtime refuses DONOTWAIT presents while it is true).
        bool IsPresentLatencyReached();
        HRESULT PrivatePresent(Resource* pSource, UINT subresource, D3DKMT_PRESENT* pKMTArgs);

        ConstantsManager &GetConstantsManager() { return m_constantsManager; }

        UINT32 GetNodeMask() { return m_NodeMask; }

        Adapter& GetAdapter() { return m_Adapter; }
        HANDLE GetRuntimeHandle() { return m_runtimeHandle; }
        Context& GetRuntimeContext() { return m_runtimeContext; }

        PipelineState& GetPipelineState() {
            return m_pipelineState;
        }
        PipelineStateCache& GetPipelineStateCache() { return m_pipelineStateCache; }

        UINT GetD3D9ApiVersion() { return m_d3d9APIVersion; }

        void SetStreamFrequency(_In_range_(0, MAX_VERTEX_STREAMS - 1 ) UINT streamIndex, UINT divider)
        {
            Check9on12(streamIndex < MAX_VERTEX_STREAMS);
            UINT* frequencies=GetPointerToStreamFrequencies();
            if (frequencies[streamIndex] != divider){ GetPipelineState().MarkInputLayoutAsDirty(); }
            frequencies[streamIndex] = divider;
        }

        UINT GetStreamFrequency(_In_range_(0, MAX_VERTEX_STREAMS - 1) UINT streamIndex)
        {
            Check9on12(streamIndex < MAX_VERTEX_STREAMS);
            return GetPointerToStreamFrequencies()[streamIndex];
        }

        UINT* GetPointerToStreamFrequencies() {
            return m_streamFrequency;
        }

        WeakHash HashStreamFrequencyData(WeakHash inputHash);

        HRESULT ResolveDeferredState(OffsetArg BaseVertexStart, OffsetArg BaseIndexStart);

        D3DDDI_DEVICECALLBACKS& GetRuntimeCallbacks() { return m_Callbacks; }

        ID3D12CommandQueue& GetCommandQueue(){ return *m_pCommandQueue; }
        ID3D12Device &GetDevice() { return *m_pDevice; }
        D3D12_RESOURCE_ALLOCATION_INFO GetResourceAllocationInfo(const D3D12_RESOURCE_DESC& desc)
        {
            static const bool enabled = [] {
                char value[2]{};
                return !(GetEnvironmentVariableA("D3D9ON12_ALLOCATION_INFO_CACHE", value, ARRAYSIZE(value)) == 1 && value[0] == '0');
            }();
            auto query = [&] {  return GetDevice().GetResourceAllocationInfo(0, 1, &desc); };
            return enabled ? m_allocationInfoCache.Get(0, desc, query) : query();
        }
        D3D_FEATURE_LEVEL GetFeatureLevel() { return m_pImmediateContext->FeatureLevel();}
        
        CComPtr<ID3D12Device> m_pDevice;
        CComPtr<ID3D12CommandQueue> m_pCommandQueue;

        // Immediate access to the translation layer. With batching, queued work is
        // replayed (drained) first so the ImmediateContext is observed in order.
        D3D12TranslationLayer::ImmediateContext& GetContext()
        {
#if defined(D3D9ON12_BATCHED_RENDERING)
            if (m_pBatchedContext && !IsProcessExiting())
            {
                m_pBatchedContext->ProcessBatch();
                ThrowFailure(m_batchError.load());
            }
#endif
            return *m_pImmediateContext;
        }
        // The ImmediateContext without draining the worker: only for operations the translation layer
        // performs thread-safely while its worker records (FreeThread resource creation, direct maps of a
        // fresh upload buffer). Anything that observes or changes context state must use GetContext().
        D3D12TranslationLayer::ImmediateContext& GetContextNoDrain() noexcept {
            return *m_pImmediateContext;
        }
        D3D12TranslationLayer::MaxFrameLatencyHelper& GetFrameLatencyHelper()
        {
            return GetContext().m_MaxFrameLatencyHelper;
        }
        PlacedResourcePool* GetPlacedResourcePool() noexcept { return m_pPlacedResourcePool.get(); }
        // Process exit (ExitProcess -> DLL_PROCESS_DETACH with lpReserved set): the runtime and plugins still tear down objects from
        // their own detach handlers, but every other thread - including the batch worker - is already gone, so no path may wait
        // for the worker any more. Teardown then skips the drains and leaks the device instead of blocking the exit.
        static void NoteProcessExit() noexcept;
        static bool IsProcessExiting() noexcept;
        // Teardown variant: drains but never throws, so objects can still be released after a worker failure.
        D3D12TranslationLayer::ImmediateContext& GetContextForDestruction() noexcept
        {
#if defined(D3D9ON12_BATCHED_RENDERING)
            if (m_pBatchedContext && !IsProcessExiting())
            {
                try { m_pBatchedContext->ProcessBatch(); }
                catch (const _com_error&) {}
                catch (const std::bad_alloc&) {}
            }
#endif
            return *m_pImmediateContext;
        }
        // PSO cache ages are measured in graphics command-list IDs. An unbounded cache
        // never inspects ages, so it does not pay for a drain.
        UINT64 GetPipelineCacheTimestamp()
        {
#if defined(D3D9ON12_BATCHED_RENDERING)
            if (min(RegistryConstants::g_cPSOCacheTrimLimitSize, g_AppCompatInfo.PSOCacheTrimLimitSize) == MAXDWORD ||
                min(RegistryConstants::g_cPSOCacheTrimLimitAge, g_AppCompatInfo.PSOCacheTrimLimitAge) == MAXDWORD)
                return 0;
#endif
            return GetContext().GetCommandListID(D3D12TranslationLayer::COMMAND_LIST_TYPE::GRAPHICS);
        }
        // Recording facade for state/draw packets (asynchronous with batching).
        auto& GetRenderContext()
        {
#if defined(D3D9ON12_BATCHED_RENDERING)
            ThrowFailure(m_batchError.load());
            return m_renderContext;
#else
            return GetContext();
#endif
        }
        // Native GPU clears/copies/resolves. CPU uploads/readbacks and BlitHelper keep their immediate drains.
        auto& GetResourceCommandContext()
        {
#if defined(D3D9ON12_BATCHED_CLEAR_COPY)
            return GetRenderContext();
#else
            return GetContext();
#endif
        }
#if defined(D3D9ON12_BATCHED_RENDERING)
        D3D12TranslationLayer::BatchedContext* GetBatchedContext() {
            return m_pBatchedContext ? &*m_pBatchedContext : nullptr;
        }
        void CheckBatchError() const { ThrowFailure(m_batchError.load()); }
#endif
        
        D3D12TranslationLayer::SharedResourceHelpers& GetSharedResourceHelper() { return *m_pSharedResourceHelpers; }
        D3D12TranslationLayer::BlitHelper& GetBlitHelper() { return GetContext().m_BlitHelper; }

        HRESULT DrawTriangleFan(_In_ OffsetArg baseVertex, _In_ UINT primitiveCount);
        HRESULT DrawWireframeTriangleFanWithEdgeFlags(_In_ OffsetArg baseVertex, _In_ UINT primitiveCount, _In_ UINT edgeFlags);
        HRESULT DrawTriangleFanIndexed(_In_ OffsetArg baseVertex, _In_ UINT vertexCount, _In_ OffsetArg baseIndex, _In_ UINT primitiveCount);

        FastUploadAllocator& GetSystemMemoryAllocator() { return m_systemMemoryAllocator; }
        DataLogger &GetDataLogger() { return m_dataLogger; }
        template<D3D12TranslationLayer::EShaderStage ShaderStage>
        void SetConstantBuffer(UINT shaderRegister, D3D12TranslationLayer::Resource *pResource, UINT offsetInBytes)
        {
            const UINT CONSTANT_BUFFER_ELEMENT_SIZE = sizeof(FLOAT) * 4;
            UINT firstConstant = offsetInBytes / CONSTANT_BUFFER_ELEMENT_SIZE;
            UINT maxConstantBufferElements = ConstantsManager::g_cMaxConstantBufferSize / CONSTANT_BUFFER_ELEMENT_SIZE;
            GetRenderContext().SetConstantBuffers<ShaderStage>(shaderRegister, 1, &pResource, &firstConstant, &maxConstantBufferElements);
        }

        void EnsureVideoDevice();
        VideoDevice *GetVideoDevice();
        HRESULT DoBlit(_In_ CONST D3DDDIARG_BLT* pBltArgs, BOOL EnableAlpha);

        D3DDDIARG_PRESENT1* m_pUMDPresentArgs;

        std::mutex m_WrappingResourceSetLock;
        std::set<IUnknown*> m_WrappingResourceSet;

        std::mutex m_interopLock;
        std::unordered_map<ID3D12Resource*, Resource*> m_interopResources;
        std::map<UINT, std::unique_ptr<QueuedInteropRequest>> m_pendingInterop;
        UINT m_nextInteropToken = 1;

        ShaderConv::ShaderConverterAPI m_ShaderConvAPI;

        ShaderDedupe<VertexShader> m_VSDedupe;
        ShaderDedupe<PixelShader> m_PSDedupe;

        D3D12_FEATURE_DATA_D3D12_OPTIONS19 m_Options19;

        D3D12TranslationLayer::COptLockedContainer<std::unordered_map<Resource*, std::vector<LockRange>>> m_lockedResourceRanges;
        void SetDrawingPreTransformedVerts(bool preTransformedVerts);

    protected:
        virtual void LogCreateVideoDevice( HRESULT hr );

    private:

        struct  TriangleFanIndexBufferCache
        {
            
            InputBuffer highestPCIndexBufferTriFan;
            UINT highestPrimitiveCountTriFan;
            bool isTriFanCacheInitialized;

            TriangleFanIndexBufferCache() : highestPrimitiveCountTriFan(0), isTriFanCacheInitialized(false)
            {}
        } ;

        // Outlives the translation layer context: placed-resource blocks are returned while the context's deletion queue drains.
        std::unique_ptr<PlacedResourcePool> m_pPlacedResourcePool;
        AllocationInfoCache<> m_allocationInfoCache;

        // Make sure this is the last thing that gets called by the destructor
        std::optional<D3D12TranslationLayer::ImmediateContext> m_pImmediateContext;
#if defined(D3D9ON12_BATCHED_RENDERING)
        std::optional<D3D12TranslationLayer::BatchedContext> m_pBatchedContext;
        RenderContext m_renderContext;
        std::atomic<HRESULT> m_batchError{ S_OK };
        D3DDDI_CREATEDEVICEFLAGS m_runtimeCreateFlags{};
        UINT m_physicalCoreCount = 0;
#endif
        std::optional<D3D12TranslationLayer::SharedResourceHelpers> m_pSharedResourceHelpers;

        std::mutex m_SwapChainManagerMutex;
        std::shared_ptr<D3D12TranslationLayer::SwapChainManager> m_SwapChainManager;
        CComPtr<ID3D12CommandQueuePrivatePresent> m_pPrivateQueuePresent;
        int m_privatePresentState = 0; // 0 unknown, 1 available, 2 unavailable
        LONGLONG m_latencyReachedSinceQpc = 0;

        CComPtr<ID3DBlob> m_pSerializedLayout;
    
        Adapter& m_Adapter;
        HANDLE m_runtimeHandle;

        Context m_runtimeContext;

        D3DDDI_DEVICECALLBACKS m_Callbacks;

        ConstantsManager m_constantsManager;

        PipelineStateCache m_pipelineStateCache;
        PipelineState m_pipelineState;

        UINT32 m_NodeMask;

        UINT m_streamFrequency[MAX_VERTEX_STREAMS];

        UINT m_d3d9APIVersion;

        FastUploadAllocator m_systemMemoryAllocator;

        TriangleFanIndexBufferCache m_triangleFanIndexBufferCache;
        DataLogger m_dataLogger;

        BYTE m_pVideoDeviceSpace[sizeof(VideoDevice)];
        VideoDevice *m_pVideoDevice;

        //This should be cleared before each use. we're just saving the allocation
        std::vector<D3D12TranslationLayer::PresentSurface> m_d3d12tlPresentSurfaces;
        //This should be cleared before each use. we're just saving the allocation
        std::vector<D3DDDIARG_PRESENTSURFACE> m_d3d9PresentSurfaces;
    };
};
