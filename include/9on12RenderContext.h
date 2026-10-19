// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once

#if defined(D3D9ON12_BATCHED_RENDERING)
namespace D3D9on12
{
    // Facade in front of the translation layer: state, draws and (optionally) native
    // clears/copies are recorded into the BatchedContext and replayed by its worker.
    // Everything else goes through Device::GetContext(), which drains the worker
    // before handing out the ImmediateContext.
    class RenderContext
    {
        D3D12TranslationLayer::ImmediateContext* immediate = nullptr;
        D3D12TranslationLayer::BatchedContext* batched = nullptr;
#if defined(D3D12TL_RECORDING_GROUP_SUPPORTED)
        bool recordingGroups = false;
#endif
    public:
        void Reset(D3D12TranslationLayer::ImmediateContext* i, D3D12TranslationLayer::BatchedContext* b, bool groups = false)
        {
            immediate = i; batched = b;
#if defined(D3D12TL_RECORDING_GROUP_SUPPORTED)
            recordingGroups = groups;
#else
            UNREFERENCED_PARAMETER(groups);
#endif
        }
#if defined(D3D12TL_RECORDING_GROUP_SUPPORTED)
        auto BeginRecordingGroup()
        {
            return D3D12TranslationLayer::BatchedContext::RecordingScope(recordingGroups ? batched : nullptr);
        }
#endif
#define FORWARD_RENDER(name) \
        template<typename... Args> void name(Args&&... args) \
        { if (batched) batched->name(std::forward<Args>(args)...); else immediate->name(std::forward<Args>(args)...); }
        FORWARD_RENDER(SetPipelineState)
        FORWARD_RENDER(DrawInstanced)
        FORWARD_RENDER(DrawIndexedInstanced)
        FORWARD_RENDER(IaSetTopology)
        FORWARD_RENDER(IaSetVertexBuffers)
        FORWARD_RENDER(IaSetIndexBuffer)
        FORWARD_RENDER(OMSetBlendFactor)
        FORWARD_RENDER(SetScissorRectEnable)
        FORWARD_RENDER(ClearRenderTargetView)
        FORWARD_RENDER(ClearDepthStencilView)
        FORWARD_RENDER(ResourceCopy)
        FORWARD_RENDER(ResourceCopyRegion)
        FORWARD_RENDER(ResourceResolveSubresource)
        FORWARD_RENDER(ResourceUpdateSubresourceUP)
#undef FORWARD_RENDER
#define FORWARD_STAGE(name) \
        template<D3D12TranslationLayer::EShaderStage Stage, typename... Args> void name(Args&&... args) \
        { if (batched) batched->name<Stage>(std::forward<Args>(args)...); else immediate->name<Stage>(std::forward<Args>(args)...); }
        FORWARD_STAGE(SetShaderResources)
        FORWARD_STAGE(SetSamplers)
        FORWARD_STAGE(SetConstantBuffers)
#undef FORWARD_STAGE
        void OMSetStencilRef(UINT reference)
        {
            if (batched) batched->OMSetStencilRef(reference); else immediate->OMSetStencilRef(reference);
        }
        void SetViewport(UINT index, const D3D12_VIEWPORT* viewport)
        {
            Check9on12(index == 0);
            if (batched) batched->SetViewports(1, viewport); else immediate->SetViewport(index, viewport);
        }
        void SetScissorRect(UINT index, const D3D12_RECT* rect)
        {
            Check9on12(index == 0);
            if (batched) batched->SetScissorRects(1, rect); else immediate->SetScissorRect(index, rect);
        }
        void OMSetRenderTargets(D3D12TranslationLayer::RTV** views, UINT count, D3D12TranslationLayer::DSV* depth)
        {
            if (batched) batched->OMSetRenderTargets(views, count, depth, nullptr, nullptr, 0, 0);
            else immediate->OMSetRenderTargets(views, count, depth);
        }
    };
}
#endif
