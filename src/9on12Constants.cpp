// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"

namespace D3D9on12
{
    __declspec(noinline) HRESULT TraceCpuOnlySetter(const char* name) noexcept
    {
        // Keep allocating diagnostic strings and their exception frame off the
        // ordinary CPU setter path, while preserving opt-in DDI spew.
        try
        {
            PrintDebugMessage(std::string("Command ID: ") + std::to_string(InterlockedIncrement(&g_CommandID)));
            PrintDebugMessage(WarningStrings::g_cDDIEntryHeader + std::string(name));
            return S_OK;
        }
        catch (std::bad_alloc&) { return E_OUTOFMEMORY; }
        catch (_com_error& error) { return TranslateDxgiHrToD3D9(error.Error()); }
    }

    _Check_return_ HRESULT APIENTRY SetVertexShaderConstF(_In_ HANDLE hDevice, _In_ const D3DDDIARG_SETVERTEXSHADERCONST* pRegisters, _In_ const VOID* pData)
    {
        D3D9ON12_CPU_SETTER_ENTRYPOINT();

        Device* pDevice = Device::GetDeviceFromHandle(hDevice);
        if (pDevice == nullptr || pRegisters == nullptr || (pRegisters->Count > 0 && pData == nullptr))
        {
            RETURN_E_INVALIDARG_AND_CHECK();
        }

        pDevice->GetConstantsManager().GetVertexShaderConstants().GetConstantBufferData(ShaderConv::CB_FLOAT).SetData(pData, pRegisters->Register, pRegisters->Count);

        return S_OK;
    }

    _Check_return_ HRESULT APIENTRY SetVertexShaderConstI(_In_ HANDLE hDevice, _In_ CONST D3DDDIARG_SETVERTEXSHADERCONSTI* pRegisters, _In_ CONST INT* pData)
    {
        D3D9ON12_CPU_SETTER_ENTRYPOINT();
        Device* pDevice = Device::GetDeviceFromHandle(hDevice);
        if (pDevice == nullptr || pRegisters == nullptr || (pRegisters->Count > 0 && pData == nullptr))
        {
            RETURN_E_INVALIDARG_AND_CHECK();
        }

        pDevice->GetConstantsManager().GetVertexShaderConstants().GetConstantBufferData(ShaderConv::CB_INT).SetData(pData, pRegisters->Register, pRegisters->Count);

        return S_OK;
    }

    _Check_return_ HRESULT APIENTRY SetVertexShaderConstB(_In_ HANDLE hDevice, _In_ CONST D3DDDIARG_SETVERTEXSHADERCONSTB* pRegisters, _In_ CONST BOOL* pData)
    {
        D3D9ON12_CPU_SETTER_ENTRYPOINT();
        Device* pDevice = Device::GetDeviceFromHandle(hDevice);
        if (pDevice == nullptr || pRegisters == nullptr || (pRegisters->Count > 0 && pData == nullptr))
        {
            RETURN_E_INVALIDARG_AND_CHECK();
        }

        pDevice->GetConstantsManager().GetVertexShaderConstants().GetConstantBufferData(ShaderConv::CB_BOOL).SetData(pData, pRegisters->Register, pRegisters->Count);
        return S_OK;
    }

    _Check_return_ HRESULT APIENTRY SetPixelShaderConstF(_In_ HANDLE hDevice, _In_ const D3DDDIARG_SETPIXELSHADERCONST* pRegisters, _In_ const FLOAT* pData)
    {
        D3D9ON12_CPU_SETTER_ENTRYPOINT();

        Device* pDevice = Device::GetDeviceFromHandle(hDevice);
        if (pDevice == nullptr || pRegisters == nullptr || (pRegisters->Count > 0 && pData == nullptr))
        {
            RETURN_E_INVALIDARG_AND_CHECK();
        }

        pDevice->GetConstantsManager().GetPixelShaderConstants().GetConstantBufferData(ShaderConv::CB_FLOAT).SetData(pData, pRegisters->Register, pRegisters->Count);

        return S_OK;
    }

    _Check_return_ HRESULT APIENTRY SetPixelShaderConstI(_In_ HANDLE hDevice, _In_ CONST D3DDDIARG_SETPIXELSHADERCONSTI* pRegisters, _In_ CONST INT* pData)
    {
        D3D9ON12_CPU_SETTER_ENTRYPOINT();
        Device* pDevice = Device::GetDeviceFromHandle(hDevice);
        if (pDevice == nullptr || pRegisters == nullptr || (pRegisters->Count > 0 && pData == nullptr))
        {
            RETURN_E_INVALIDARG_AND_CHECK();
        }

        pDevice->GetConstantsManager().GetPixelShaderConstants().GetConstantBufferData(ShaderConv::CB_INT).SetData(pData, pRegisters->Register, pRegisters->Count);

        return S_OK;
    }

    _Check_return_ HRESULT APIENTRY SetPixelShaderConstB(_In_ HANDLE hDevice, _In_ CONST D3DDDIARG_SETPIXELSHADERCONSTB* pRegisters, _In_ CONST BOOL* pData)
    {
        D3D9ON12_CPU_SETTER_ENTRYPOINT();
        Device* pDevice = Device::GetDeviceFromHandle(hDevice);
        if (pDevice == nullptr || pRegisters == nullptr || (pRegisters->Count > 0 && pData == nullptr))
        {
            RETURN_E_INVALIDARG_AND_CHECK();
        }

        pDevice->GetConstantsManager().GetPixelShaderConstants().GetConstantBufferData(ShaderConv::CB_BOOL).SetData(pData, pRegisters->Register, pRegisters->Count);
        return S_OK;
    }

    HRESULT ConstantsManager::Init()
    {
        m_nullCB.m_buffer = m_nullCB.m_allocator.Allocate(g_cMaxConstantBufferSize);

        memset(m_nullCB.m_buffer.m_pMappedAddress, 0, g_cMaxConstantBufferSize);

        //We should never be writing to the null CB
        m_nullCB.m_buffer.m_pMappedAddress = nullptr;

        // There must be a resource bound to each root constant slot
        m_vertexShaderData.NullOutBindings(m_device, m_nullCB);
        m_pixelShaderData.NullOutBindings(m_device, m_nullCB);

        return S_OK;
    }

    void ConstantsManager::Destroy()
    {
        m_vertexShaderData.Destroy();
        m_geometryShaderData.Destroy();
        m_pixelShaderData.Destroy();
    }

    void ConstantBufferBinding::Destroy()
    {
        m_allocator.Destroy();
    }

    void ConstantBufferBinding::Version(const void* pData, UINT dataSize)
    {
        m_buffer = m_allocator.Allocate(dataSize);

        if (pData)
        {
            memcpy(m_buffer.m_pMappedAddress, pData, dataSize);
        }
    }

    void ConstantsManager::BindToPipeline(Device& device, ConstantBufferBinding& buffer, D3D12TranslationLayer::EShaderStage shaderStage)
    {
        ConstantBufferBinding* buffers[] = { &buffer };
        BindToPipeline(device, buffers, 1, 1, shaderStage);
    }

    void ConstantsManager::BindToPipeline(Device& device, ConstantBufferBinding* const* buffers, UINT count, UINT dirtyMask, D3D12TranslationLayer::EShaderStage shaderStage)
    {
        if (!dirtyMask) return;
        UINT first = 0, end = count;
        while (!(dirtyMask & BIT(first))) ++first;
        while (!(dirtyMask & BIT(end - 1))) --end;
        D3D12TranslationLayer::Resource* resources[3];
        UINT offsets[3], sizes[3];
        Check9on12(count <= ARRAYSIZE(resources));
        for (UINT i = first; i < end; ++i)
        {
            resources[i] = buffers[i]->m_buffer.m_pResource;
            offsets[i] = buffers[i]->m_buffer.m_offsetFromBase / (sizeof(FLOAT) * 4);
            sizes[i] = g_cMaxConstantBufferSize / (sizeof(FLOAT) * 4);
        }
        const UINT startSlot = buffers[first]->m_shaderRegister;
        switch (shaderStage)
        {
        case D3D12TranslationLayer::e_VS:
            device.GetRenderContext().SetConstantBuffers<D3D12TranslationLayer::e_VS>(startSlot, end - first, resources + first, offsets + first, sizes + first);
            break;
        case D3D12TranslationLayer::e_GS:
            device.GetRenderContext().SetConstantBuffers<D3D12TranslationLayer::e_GS>(startSlot, end - first, resources + first, offsets + first, sizes + first);
            break;
        case D3D12TranslationLayer::e_PS:
            device.GetRenderContext().SetConstantBuffers<D3D12TranslationLayer::e_PS>(startSlot, end - first, resources + first, offsets + first, sizes + first);
            break;
        default:
            assert(false);
        }
    }

    void ConstantsManager::StageConstants::Destroy()
    {
        m_floats.Destroy();
        m_integers.Destroy();
        m_booleans.Destroy();
    }

    void ConstantsManager::PixelShaderConstants::Destroy()
    {
        StageConstants::Destroy();
        m_extension1.Destroy();
        m_extension2.Destroy();
        m_extension3.Destroy();
    }

    void ConstantsManager::VertexShaderConstants::Destroy()
    {
        StageConstants::Destroy();
        m_extension.Destroy();
    }

    void ConstantsManager::GeometryShaderConstants::Destroy()
    {
        m_extension.Destroy();
    }

    void ConstantsManager::StageConstants::UpdateAppVisibleAndBindToPipeline(Device& device, UINT maxFloats, UINT maxInts, UINT maxBools, bool force)
    {
        Check9on12(maxFloats % 4 == 0);
        Check9on12(maxInts % 4 == 0);
        UINT dirty = force ? 7 : 0;
        if (m_floats.UpdateData(device, maxFloats / 4) == Result::S_CHANGE) dirty |= BIT(0);
        if (m_integers.UpdateData(device, maxInts / 4) == Result::S_CHANGE) dirty |= BIT(1);
        if (m_booleans.UpdateData(device, maxBools) == Result::S_CHANGE) dirty |= BIT(2);
        ConstantBufferBinding* bindings[] = { &m_floats.m_binding, &m_integers.m_binding, &m_booleans.m_binding };
        BindToPipeline(device, bindings, ARRAYSIZE(bindings), dirty, m_shaderType);
    }

    void ConstantsManager::StageConstants::NullOutBindings(Device& /*device*/, ConstantBufferBinding& nullCB)
    {
        m_floats.ReplaceResource(nullCB);
        m_integers.ReplaceResource(nullCB);
        m_booleans.ReplaceResource(nullCB);
    }

    void ConstantsManager::BindShaderConstants()
    {
        D3D12VertexShader* pVs = m_device.GetPipelineState().GetVertexStage().GetCurrentD3D12VertexShader();
        D3D12PixelShader* pPs = m_device.GetPipelineState().GetPixelStage().GetCurrentD3D12PixelShader();

        m_vertexShaderData.UpdateAppVisibleAndBindToPipeline(m_device, pVs->m_floatConstsUsed, pVs->m_intConstsUsed, pVs->m_boolConstsUsed);
        m_pixelShaderData.UpdateAppVisibleAndBindToPipeline(m_device, pPs->m_floatConstsUsed, pPs->m_intConstsUsed, pPs->m_boolConstsUsed);

        //Bind Internal Extension Constants (used by the Shader Converter)
        {
            if (m_vertexShaderData.m_dirty)
            {
                BindToPipeline(m_device, m_vertexShaderData.m_extension, D3D12TranslationLayer::EShaderStage::e_VS);
                m_vertexShaderData.m_dirty = false;
            }

            if (m_geometryShaderData.m_dirty)
            {
                BindToPipeline(m_device, m_geometryShaderData.m_extension, D3D12TranslationLayer::EShaderStage::e_GS);
                m_geometryShaderData.m_dirty = false;
            }

            if (m_pixelShaderData.m_dirty)
            {
                ConstantBufferBinding* bindings[] = { &m_pixelShaderData.m_extension1, &m_pixelShaderData.m_extension2, &m_pixelShaderData.m_extension3 };
                BindToPipeline(m_device, bindings, ARRAYSIZE(bindings), m_pixelShaderData.m_dirty, D3D12TranslationLayer::e_PS);
                m_pixelShaderData.m_dirty = 0;
            }
        }
    }

    void ConstantsManager::UpdateVertexShaderExtension(const ShaderConv::VSCBExtension& data)
    {
        m_vertexShaderData.m_extension.Version(&data, sizeof(data));
        m_vertexShaderData.m_dirty = true;
    }

    void ConstantsManager::UpdateGeometryShaderExtension(const ShaderConv::VSCBExtension& data)
    {
        m_geometryShaderData.m_extension.Version(&data, sizeof(data));
        m_geometryShaderData.m_dirty = true;
    }

    Result ConstantsManager::UpdatePixelShaderExtension(const ShaderConv::eConstantBuffers extension, const void* pData, size_t dataSize)
    {
        ConstantBufferBinding* pBinding;
        switch (extension)
        {
        case ShaderConv::CB_PS_EXT:
            pBinding = &m_pixelShaderData.m_extension1;
            break;
        case ShaderConv::CB_PS_EXT2:
            pBinding = &m_pixelShaderData.m_extension2;
            break;
        case ShaderConv::CB_PS_EXT3:
            pBinding = &m_pixelShaderData.m_extension3;
            break;
        default:
            Check9on12(false);
            pBinding = nullptr;
            return Result::E_INVALID_ARG;
        }
        assert(dataSize <= UINT_MAX);
        pBinding->Version(pData, static_cast<UINT>(dataSize));

        m_pixelShaderData.m_dirty |= BIT(extension - ShaderConv::CB_PS_EXT);

        return Result::S_SUCCESS;
    }
}
