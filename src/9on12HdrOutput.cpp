// Licensed under the MIT license.
// HDR output (D3D9On12SetHdrOutput / D3D9On12QueryHdrOutput, include/D3D9On12Extensions.h).
//
// The D3D9 runtime refuses 16-bit float back buffers (CheckDeviceType and CreateDevice answer D3DERR_NOTAVAILABLE /
// D3DERR_INVALIDCALL for D3DFMT_A16B16G16R16F, windowed or not) and has no color space of its own, so the driver
// changes the storage of the swap chain buffers it is asked to create while HDR output is enabled, and the color space
// of the DXGI swap chain it presents them with in a window.
//
// The runtime's swap chain buffers are told apart from the application's render targets by how the runtime creates
// them (observed on the Windows 11 26100 d3d9.dll): render targets in D3DDDIPOOL_LOCALVIDMEM that are not lockable
// (application render targets and render-target textures are D3DDDIPOOL_VIDEOMEMORY); windowed buffers are surfaces,
// exclusive fullscreen buffers displayable shared textures.
#include "pch.h"
#include <dxgi1_6.h>

namespace D3D9on12
{
    namespace
    {
        std::mutex g_hdrDefaultLock;
        D3D9ON12_HDR_OUTPUT g_hdrDefault = { sizeof(D3D9ON12_HDR_OUTPUT) };
        UINT g_hdrDefaultVersion = 0;

        bool ValidLuminance(float value)
        {
            return value >= 0.0f && value <= 100000.0f;
        }

        UINT16 ClampToUInt16(float value)
        {
            return static_cast<UINT16>(std::clamp(value + 0.5f, 0.0f, 65535.0f));
        }

        // The outputs of a DXGI factory, kept until the factory is no longer current: turning Windows HDR on or off, or
        // changing the displays, makes it out of date (a new one has the new state). Never destroyed: no DXGI calls from
        // the DLL's static destructors at process exit.
        struct DisplayCache
        {
            std::mutex m_lock;
            CComPtr<IDXGIFactory1> m_pFactory;
            std::vector<CComPtr<IDXGIOutput6>> m_outputs;
        };

        // The display the window is on mostly (DXGI_OUTPUT_DESC1 has the color space Windows drives it in)
        bool QueryDisplay(HWND window, DXGI_OUTPUT_DESC1& display)
        {
            HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
            if (!monitor)
            {
                return false;
            }
            static DisplayCache& cache = *new DisplayCache;
            std::lock_guard<std::mutex> lock(cache.m_lock);
            if (!cache.m_pFactory || !cache.m_pFactory->IsCurrent())
            {
                cache.m_pFactory.Release();
                cache.m_outputs.clear();
                if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&cache.m_pFactory))))
                {
                    return false;
                }
                CComPtr<IDXGIAdapter1> pAdapter;
                for (UINT adapter = 0; cache.m_pFactory->EnumAdapters1(adapter, &pAdapter) != DXGI_ERROR_NOT_FOUND; ++adapter, pAdapter.Release())
                {
                    CComPtr<IDXGIOutput> pOutput;
                    for (UINT output = 0; pAdapter->EnumOutputs(output, &pOutput) != DXGI_ERROR_NOT_FOUND; ++output, pOutput.Release())
                    {
                        CComPtr<IDXGIOutput6> pOutput6;
                        if (SUCCEEDED(pOutput->QueryInterface(&pOutput6)))
                        {
                            cache.m_outputs.push_back(pOutput6);
                        }
                    }
                }
            }
            for (auto& pOutput : cache.m_outputs)
            {
                if (SUCCEEDED(pOutput->GetDesc1(&display)) && display.Monitor == monitor)
                {
                    return true;
                }
            }
            return false;
        }
    }

    void Device::InitHdrOutput()
    {
        D3D9ON12_HDR_OUTPUT output;
        UINT version;
        {
            std::lock_guard<std::mutex> lock(g_hdrDefaultLock);
            output = g_hdrDefault;
            version = g_hdrDefaultVersion;
        }
        std::lock_guard<std::mutex> lock(m_hdrLock);
        m_hdrOutput = output;
        m_hdrVersion = version;
    }

    HRESULT Device::SetHdrOutput(ID3D12Device* pD3D12Device, const D3D9ON12_HDR_OUTPUT& output)
    {
        const bool enable = (output.Flags & D3D9ON12_HDR_OUTPUT_ENABLE) != 0;
        if (output.StructSize != sizeof(output) || (output.Flags & ~D3D9ON12_HDR_OUTPUT_ENABLE) != 0 ||
            (enable ? output.ColorSpace != D3D9ON12_HDR_COLOR_SPACE_SCRGB_LINEAR : output.ColorSpace > D3D9ON12_HDR_COLOR_SPACE_SCRGB_LINEAR) ||
            !ValidLuminance(output.MaxLuminance) || !ValidLuminance(output.MinLuminance) ||
            !ValidLuminance(output.MaxContentLightLevel) || !ValidLuminance(output.MaxFrameAverageLightLevel))
        {
            return E_INVALIDARG;
        }

        auto apply = [&output](Device& device)
        {
            std::lock_guard<std::mutex> lock(device.m_hdrLock);
            device.m_hdrOutput = output;
            // Never 0, the version a swap chain that got no metadata yet has
            device.m_hdrVersion = device.m_hdrVersion + 1 ? device.m_hdrVersion + 1 : 1;
        };

        if (!pD3D12Device)
        {
            {
                std::lock_guard<std::mutex> lock(g_hdrDefaultLock);
                g_hdrDefault = output;
                g_hdrDefaultVersion = g_hdrDefaultVersion + 1 ? g_hdrDefaultVersion + 1 : 1;
            }
            ForEachRegisteredDevice([&apply](Device& device) { apply(device); });
            return S_OK;
        }

        bool found = false;
        ForEachRegisteredDevice([&](Device& device)
        {
            if (device.m_pDevice.p == pD3D12Device)
            {
                apply(device);
                found = true;
            }
        });
        return found ? S_OK : E_NOINTERFACE;
    }

    HRESULT Device::QueryHdrOutput(ID3D12Device* pD3D12Device, HWND window, D3D9ON12_HDR_OUTPUT_STATUS& status)
    {
        if (status.StructSize != sizeof(status))
        {
            return E_INVALIDARG;
        }
        status = { sizeof(status) };
        status.ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;

        if (window)
        {
            DXGI_OUTPUT_DESC1 display = {};
            if (QueryDisplay(window, display))
            {
                if (display.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
                {
                    status.Flags |= D3D9ON12_HDR_STATUS_DISPLAY_HDR;
                }
                status.DisplayMaxLuminance = display.MaxLuminance;
                status.DisplayMinLuminance = display.MinLuminance;
                status.DisplayMaxFullFrameLuminance = display.MaxFullFrameLuminance;
            }
        }

        if (!pD3D12Device)
        {
            return S_OK;
        }
        bool found = false;
        ForEachRegisteredDevice([&](Device& device)
        {
            if (found || device.m_pDevice.p != pD3D12Device)
            {
                return;
            }
            found = true;
            std::lock_guard<std::mutex> lock(device.m_hdrLock);
            status.Flags |= (device.m_hdrOutput.Flags & D3D9ON12_HDR_OUTPUT_ENABLE) ? D3D9ON12_HDR_STATUS_ENABLED : 0u;
            status.Flags |= device.m_hdrFloatBuffers ? D3D9ON12_HDR_STATUS_FLOAT_BUFFERS : 0u;
            status.Flags |= device.m_hdrActive ? D3D9ON12_HDR_STATUS_ACTIVE : 0u;
            status.Flags |= device.m_hdrExclusiveBuffers ? D3D9ON12_HDR_STATUS_EXCLUSIVE : 0u;
            status.ColorSpace = device.m_hdrColorSpace;
        });
        return found ? S_OK : E_NOINTERFACE;
    }

    bool Device::StoreSwapChainBufferAsFloat(const D3DDDIARG_CREATERESOURCE2& createArgs)
    {
        const bool displayable = createArgs.Flags2.IsDisplayable || createArgs.Flags.Primary;
        const bool swapChainBuffer = createArgs.Flags.RenderTarget && createArgs.Flags.NotLockable && !createArgs.Flags.ZBuffer &&
            createArgs.Pool == D3DDDIPOOL_LOCALVIDMEM && createArgs.MultisampleType == D3DDDIMULTISAMPLE_NONE &&
            createArgs.SurfCount == 1 && (displayable || !createArgs.Flags.Texture) &&
            (createArgs.Format == D3DDDIFMT_A8R8G8B8 || createArgs.Format == D3DDDIFMT_X8R8G8B8);
        if (!swapChainBuffer)
        {
            return false;
        }

        std::lock_guard<std::mutex> lock(m_hdrLock);
        // The runtime creates all buffers of a swap chain at once (device creation, Reset): the last ones count
        m_hdrExclusiveBuffers = displayable;
        m_hdrFloatBuffers = (m_hdrOutput.Flags & D3D9ON12_HDR_OUTPUT_ENABLE) && !displayable;
        return m_hdrFloatBuffers;
    }

    void Device::ApplyHdrToSwapChain(IDXGISwapChain3* pSwapChain, Resource& /*source*/)
    {
        if (!pSwapChain)
        {
            return;
        }
        DXGI_SWAP_CHAIN_DESC1 desc = {};
        if (FAILED(pSwapChain->GetDesc1(&desc)))
        {
            return;
        }

        std::lock_guard<std::mutex> lock(m_hdrLock);
        const bool isFloat = desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
        const DXGI_COLOR_SPACE_TYPE wanted = isFloat ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        const bool sameBuffers = pSwapChain == m_pHdrSwapChain && desc.Width == m_hdrSwapChainDesc.Width &&
            desc.Height == m_hdrSwapChainDesc.Height && desc.Format == m_hdrSwapChainDesc.Format;
        if (pSwapChain != m_pHdrSwapChain)
        {
            // A new swap chain starts in the default color space
            m_hdrColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        }
        m_pHdrSwapChain = pSwapChain;
        m_hdrSwapChainDesc = desc;

        // The SDR path calls nothing; float buffers get their color space again after a resize
        if (wanted != m_hdrColorSpace || (isFloat && !sameBuffers))
        {
            UINT support = 0;
            if (SUCCEEDED(pSwapChain->CheckColorSpaceSupport(wanted, &support)) &&
                (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) &&
                SUCCEEDED(pSwapChain->SetColorSpace1(wanted)))
            {
                m_hdrColorSpace = wanted;
            }
            m_hdrAppliedVersion = 0;
        }

        m_hdrActive = isFloat && m_hdrColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
        if (m_hdrActive && m_hdrAppliedVersion != m_hdrVersion)
        {
            m_hdrAppliedVersion = m_hdrVersion;
            CComPtr<IDXGISwapChain4> pSwapChain4;
            if (SUCCEEDED(pSwapChain->QueryInterface(&pSwapChain4)))
            {
                // Content in BT.709 primaries with a D65 white point (scRGB), luminance in nits
                DXGI_HDR_METADATA_HDR10 metadata = {};
                metadata.RedPrimary[0] = 32000;     // 0.640
                metadata.RedPrimary[1] = 16500;     // 0.330
                metadata.GreenPrimary[0] = 15000;   // 0.300
                metadata.GreenPrimary[1] = 30000;   // 0.600
                metadata.BluePrimary[0] = 7500;     // 0.150
                metadata.BluePrimary[1] = 3000;     // 0.060
                metadata.WhitePoint[0] = 15635;     // 0.3127
                metadata.WhitePoint[1] = 16450;     // 0.3290
                metadata.MaxMasteringLuminance = static_cast<UINT>(m_hdrOutput.MaxLuminance + 0.5f);
                metadata.MinMasteringLuminance = static_cast<UINT>(m_hdrOutput.MinLuminance * 10000.0f + 0.5f);
                metadata.MaxContentLightLevel = ClampToUInt16(m_hdrOutput.MaxContentLightLevel);
                metadata.MaxFrameAverageLightLevel = ClampToUInt16(m_hdrOutput.MaxFrameAverageLightLevel);
                const bool any = metadata.MaxMasteringLuminance || metadata.MaxContentLightLevel || metadata.MaxFrameAverageLightLevel;
                pSwapChain4->SetHDRMetaData(any ? DXGI_HDR_METADATA_TYPE_HDR10 : DXGI_HDR_METADATA_TYPE_NONE, any ? sizeof(metadata) : 0, any ? &metadata : nullptr);
            }
        }
    }

    void Device::NoteExclusivePresent()
    {
        std::lock_guard<std::mutex> lock(m_hdrLock);
        m_hdrActive = false;
        // A window swap chain made later is a new one
        m_pHdrSwapChain = nullptr;
    }
}
