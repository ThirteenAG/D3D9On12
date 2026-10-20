// Licensed under the MIT license.
// HDR output (D3D9On12SetHdrOutput / D3D9On12QueryHdrOutput): swap chain buffers stored as float keep values above
// 1.0 through rendering and StretchRect, the window's swap chain gets scRGB, and turning HDR output off goes back to
// 8-bit buffers at Reset. Runs beside the x86 driver; works without an HDR display (the display is only reported).
//   --window x,y   place the window there (on an HDR display) and keep it on screen for a moment after each present
//   --fullscreen N an exclusive fullscreen device on adapter N instead: HDR output stays off there (EXCLUSIVE)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <d3d9on12.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include "../include/D3D9On12Extensions.h"
#include "probe_driver_identity.h"

using Microsoft::WRL::ComPtr;

static float HalfToFloat(uint16_t h)
{
    const uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 31, mantissa = h & 1023;
    float value = exponent == 0 ? std::ldexp(static_cast<float>(mantissa), -24) :
        exponent == 31 ? INFINITY : std::ldexp(static_cast<float>(mantissa | 1024), static_cast<int>(exponent) - 25);
    return sign ? -value : value;
}

static int failures = 0;
static void Check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    failures += ok ? 0 : 1;
}

int main(int argc, char** argv)
{
    int windowX = 0, windowY = 0, fullscreenAdapter = -1;
    bool placed = false;
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::strcmp(argv[i], "--window") == 0 && sscanf_s(argv[i + 1], "%d,%d", &windowX, &windowY) == 2)
            placed = true;
        if (std::strcmp(argv[i], "--fullscreen") == 0)
            fullscreenAdapter = std::atoi(argv[i + 1]);
    }

    HMODULE driver = ProbeLocalDriver();
    if (!driver)
        return 6;
    auto capabilities = reinterpret_cast<PFN_D3D9On12Capabilities>(GetProcAddress(driver, D3D9ON12_CAPABILITIES_EXPORT));
    auto setHdr = reinterpret_cast<PFN_D3D9On12SetHdrOutput>(GetProcAddress(driver, D3D9ON12_SET_HDR_OUTPUT_EXPORT));
    auto queryHdr = reinterpret_cast<PFN_D3D9On12QueryHdrOutput>(GetProcAddress(driver, D3D9ON12_QUERY_HDR_OUTPUT_EXPORT));
    Check(capabilities && setHdr && queryHdr, "HDR exports");
    if (!capabilities || !setHdr || !queryHdr)
        return 4;
    Check((capabilities(3) & D3D9ON12_CAP_HDR_OUTPUT) && !(capabilities(2) & D3D9ON12_CAP_HDR_OUTPUT) &&
        capabilities(2) == (capabilities(3) & ~D3D9ON12_CAP_HDR_OUTPUT), "capability ABI 3 = ABI 2 + HDR_OUTPUT");

    D3D9ON12_HDR_OUTPUT output{ sizeof(output) };
    output.Flags = D3D9ON12_HDR_OUTPUT_ENABLE;
    output.ColorSpace = D3D9ON12_HDR_COLOR_SPACE_SCRGB_LINEAR;
    output.MaxLuminance = 1000.0f;
    output.MaxContentLightLevel = 1000.0f;
    output.MaxFrameAverageLightLevel = 200.0f;
    D3D9ON12_HDR_OUTPUT bad = output;
    bad.ColorSpace = 7;
    Check(setHdr(nullptr, &bad) == E_INVALIDARG, "malformed description refused");
    // Enabled for the devices created from now on
    Check(SUCCEEDED(setHdr(nullptr, &output)), "SetHdrOutput (process default)");

    wchar_t path[MAX_PATH]{};
    GetSystemDirectoryW(path, MAX_PATH);
    wcscat_s(path, L"\\d3d9.dll");
    HMODULE runtime = LoadLibraryW(path);
    auto create = reinterpret_cast<PFN_Direct3DCreate9On12>(GetProcAddress(runtime, "Direct3DCreate9On12"));
    D3D9ON12_ARGS args{};
    args.Enable9On12 = TRUE;
    ComPtr<IDirect3D9> d3d;
    d3d.Attach(create(D3D_SDK_VERSION, &args, 1));
    if (!d3d)
        return 2;

    const bool fullscreen = fullscreenAdapter >= 0;
    D3DDISPLAYMODE mode{};
    d3d->GetAdapterDisplayMode(fullscreen ? fullscreenAdapter : 0, &mode);
    const UINT width = fullscreen ? mode.Width : 641, height = fullscreen ? mode.Height : 359;
    HWND window = CreateWindowW(L"STATIC", L"D3D9On12 HDR output probe", WS_POPUP | WS_VISIBLE, windowX, windowY, width, height,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    D3D9ON12_HDR_OUTPUT_STATUS status{ sizeof(status) };
    Check(SUCCEEDED(queryHdr(nullptr, window, &status)), "QueryHdrOutput (display)");
    std::printf("display HDR=%d max=%.0f min=%.4f fullframe=%.0f nits\n", (status.Flags & D3D9ON12_HDR_STATUS_DISPLAY_HDR) ? 1 : 0,
        status.DisplayMaxLuminance, status.DisplayMinLuminance, status.DisplayMaxFullFrameLuminance);

    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = !fullscreen;
    pp.hDeviceWindow = window;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = width;
    pp.BackBufferHeight = height;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    pp.BackBufferCount = 1;
    pp.FullScreen_RefreshRateInHz = fullscreen ? mode.RefreshRate : 0;
    ComPtr<IDirect3DDevice9> device;
    HRESULT hr = d3d->CreateDevice(fullscreen ? fullscreenAdapter : 0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device);
    Check(SUCCEEDED(hr), "CreateDevice");
    if (FAILED(hr))
        return 3;
    ComPtr<IDirect3DDevice9On12> on12;
    ComPtr<ID3D12Device> d3d12;
    device.As(&on12);
    if (on12)
        on12->GetD3D12Device(IID_PPV_ARGS(&d3d12));
    Check(d3d12 != nullptr, "GetD3D12Device");

    ComPtr<ID3DBlob> code;
    const char* source = "float4 c0 : register(c0); float4 main() : COLOR0 { return c0; }";
    D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, "main", "ps_3_0", 0, 0, &code, nullptr);
    ComPtr<IDirect3DPixelShader9> shader;
    if (code)
        device->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &shader);
    Check(shader != nullptr, "pixel shader");

    // Draws the value over the back buffer, presents, and reads the back buffer back through a float render target
    auto frame = [&](float value, float* readBack) -> HRESULT
    {
        ComPtr<IDirect3DSurface9> backBuffer;
        device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
        device->SetRenderTarget(0, backBuffer.Get());
        device->BeginScene();
        const float constants[4] = { value, value * 0.5f, value * 0.25f, 1.0f };
        device->SetPixelShader(shader.Get());
        device->SetPixelShaderConstantF(0, constants, 1);
        device->SetFVF(D3DFVF_XYZRHW);
        const float w = static_cast<float>(width), h = static_cast<float>(height);
        const float quad[4][4] = { { -0.5f, -0.5f, 0, 1 }, { w - 0.5f, -0.5f, 0, 1 }, { -0.5f, h - 0.5f, 0, 1 }, { w - 0.5f, h - 0.5f, 0, 1 } };
        device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
        device->EndScene();

        ComPtr<IDirect3DSurface9> floatTarget, readback;
        device->CreateRenderTarget(width, height, D3DFMT_A16B16G16R16F, D3DMULTISAMPLE_NONE, 0, FALSE, &floatTarget, nullptr);
        device->CreateOffscreenPlainSurface(width, height, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &readback, nullptr);
        if (floatTarget && readback && SUCCEEDED(device->StretchRect(backBuffer.Get(), nullptr, floatTarget.Get(), nullptr, D3DTEXF_POINT)) &&
            SUCCEEDED(device->GetRenderTargetData(floatTarget.Get(), readback.Get())))
        {
            D3DLOCKED_RECT locked{};
            if (SUCCEEDED(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
            {
                auto pixel = reinterpret_cast<const uint16_t*>(static_cast<const BYTE*>(locked.pBits) + locked.Pitch * (height / 2)) + 4 * (width / 2);
                for (int c = 0; c < 4; ++c)
                    readBack[c] = HalfToFloat(pixel[c]);
                readback->UnlockRect();
            }
        }
        // The runtime still reports the original 8-bit format. Its ordinary screenshot path
        // must receive converted pixels, rather than the first four bytes of each float pixel.
        ComPtr<IDirect3DSurface9> readback8;
        D3DSURFACE_DESC backBufferDesc{};
        backBuffer->GetDesc(&backBufferDesc);
        if (backBufferDesc.Format == D3DFMT_A8R8G8B8 || backBufferDesc.Format == D3DFMT_X8R8G8B8)
        {
            Check(SUCCEEDED(device->CreateOffscreenPlainSurface(backBufferDesc.Width, backBufferDesc.Height, backBufferDesc.Format,
                D3DPOOL_SYSTEMMEM, &readback8, nullptr)), "8-bit readback surface");
            if (readback8)
            {
                Check(SUCCEEDED(device->GetRenderTargetData(backBuffer.Get(), readback8.Get())), "8-bit readback copy");
                D3DLOCKED_RECT locked{};
                if (SUCCEEDED(readback8->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
                {
                    auto pixel8 = static_cast<const BYTE*>(locked.pBits) + locked.Pitch * (height / 2) + 4 * (width / 2);
                    bool correct = true;
                    for (int c = 0; c < 4; ++c)
                    {
                        const int channel = c < 3 ? 2 - c : c;
                        const int expected = static_cast<int>(std::lround(std::fmin(std::fmax(constants[channel], 0.0f), 1.0f) * 255.0f));
                        correct = correct && std::abs(static_cast<int>(pixel8[c]) - expected) <= 1;
                    }
                    Check(correct, "8-bit readback converts float channels");
                    readback8->UnlockRect();
                }
                else
                    Check(false, "8-bit readback lock");
            }
        }
        HRESULT present = device->Present(nullptr, nullptr, nullptr, nullptr);
        if (placed || fullscreen)
            Sleep(3000);
        return present;
    };

    D3D9ON12_HDR_OUTPUT_STATUS deviceStatus{ sizeof(deviceStatus) };
    float pixel[4]{};
    // 12.5 = 1000 nits in scRGB
    hr = frame(12.5f, pixel);
    Check(SUCCEEDED(hr), "Present");
    queryHdr(d3d12.Get(), nullptr, &deviceStatus);
    std::printf("status flags=0x%X colorspace=%u read back %.3f %.3f %.3f %.3f\n", deviceStatus.Flags, deviceStatus.ColorSpace, pixel[0], pixel[1], pixel[2], pixel[3]);
    if (fullscreen)
    {
        Check((deviceStatus.Flags & D3D9ON12_HDR_STATUS_EXCLUSIVE) != 0, "exclusive fullscreen reported");
        Check(!(deviceStatus.Flags & D3D9ON12_HDR_STATUS_ACTIVE), "no scRGB swap chain in exclusive fullscreen");
    }
    else
    {
        Check((deviceStatus.Flags & (D3D9ON12_HDR_STATUS_ENABLED | D3D9ON12_HDR_STATUS_FLOAT_BUFFERS | D3D9ON12_HDR_STATUS_ACTIVE)) ==
            (D3D9ON12_HDR_STATUS_ENABLED | D3D9ON12_HDR_STATUS_FLOAT_BUFFERS | D3D9ON12_HDR_STATUS_ACTIVE), "enabled, float buffers, scRGB present");
        Check(deviceStatus.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, "swap chain color space G10 P709");
        Check(std::fabs(pixel[0] - 12.5f) < 0.01f && std::fabs(pixel[1] - 6.25f) < 0.01f && std::fabs(pixel[2] - 3.125f) < 0.01f,
            "back buffer keeps 12.5 / 6.25 / 3.125");
    }

    // The brightness changes without a Reset (metadata only)
    output.MaxLuminance = 600.0f;
    Check(SUCCEEDED(setHdr(d3d12.Get(), &output)), "SetHdrOutput (device)");
    Check(SUCCEEDED(frame(4.0f, pixel)), "Present after metadata change");

    // Off: 8-bit buffers again after Reset
    output.Flags = 0;
    Check(SUCCEEDED(setHdr(d3d12.Get(), &output)), "SetHdrOutput off");
    hr = device->Reset(&pp);
    Check(SUCCEEDED(hr), "Reset");
    hr = frame(12.5f, pixel);
    Check(SUCCEEDED(hr), "Present after Reset");
    queryHdr(d3d12.Get(), nullptr, &deviceStatus);
    std::printf("status flags=0x%X colorspace=%u read back %.3f %.3f %.3f %.3f\n", deviceStatus.Flags, deviceStatus.ColorSpace, pixel[0], pixel[1], pixel[2], pixel[3]);
    Check(!(deviceStatus.Flags & (D3D9ON12_HDR_STATUS_ENABLED | D3D9ON12_HDR_STATUS_FLOAT_BUFFERS | D3D9ON12_HDR_STATUS_ACTIVE)), "off: 8-bit buffers");
    Check(deviceStatus.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, "off: color space G22 P709");
    Check(std::fabs(pixel[0] - 1.0f) < 0.01f, "off: back buffer clips at 1.0");

    // On again
    output.Flags = D3D9ON12_HDR_OUTPUT_ENABLE;
    setHdr(d3d12.Get(), &output);
    hr = device->Reset(&pp);
    Check(SUCCEEDED(hr), "Reset (on again)");
    hr = frame(2.0f, pixel);
    Check(SUCCEEDED(hr), "Present (on again)");
    queryHdr(d3d12.Get(), nullptr, &deviceStatus);
    if (!fullscreen)
        Check((deviceStatus.Flags & D3D9ON12_HDR_STATUS_ACTIVE) && std::fabs(pixel[0] - 2.0f) < 0.01f, "on again: scRGB, 2.0 kept");

    for (int recovery = 0; !fullscreen && recovery < 2; ++recovery)
    {
        // Recovery can overlap old and new devices, after Reset alone or an exclusive present.
        // Neither path may leave the old window swap chain claiming the replacement's HWND.
        if (recovery)
        {
            D3DDISPLAYMODE desktop{};
            d3d->GetAdapterDisplayMode(0, &desktop);
            D3DPRESENT_PARAMETERS exclusive = pp;
            exclusive.Windowed = FALSE;
            exclusive.BackBufferWidth = desktop.Width;
            exclusive.BackBufferHeight = desktop.Height;
            exclusive.BackBufferFormat = desktop.Format;
            exclusive.FullScreen_RefreshRateInHz = desktop.RefreshRate;
            Check(SUCCEEDED(device->Reset(&exclusive)), "recovery: Reset exclusive");
            Check(SUCCEEDED(frame(0.25f, pixel)), "recovery: exclusive Present");
        }
        Check(SUCCEEDED(device->Reset(&pp)), "recovery: Reset windowed without presenting");
        ComPtr<IDirect3DDevice9> previousDevice = device;
        device.Reset();
        Check(SUCCEEDED(setHdr(nullptr, &output)), "recovery: HDR default enabled");
        hr = d3d->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device);
        Check(SUCCEEDED(hr), "recovery: replacement CreateDevice while previous device lives");
        if (SUCCEEDED(hr))
        {
            shader.Reset();
            device->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &shader);
            on12.Reset();
            d3d12.Reset();
            device.As(&on12);
            on12->GetD3D12Device(IID_PPV_ARGS(&d3d12));
            hr = frame(2.0f, pixel);
            Check(SUCCEEDED(hr), "recovery: replacement window Present");
            previousDevice.Reset();
            queryHdr(d3d12.Get(), nullptr, &deviceStatus);
            std::printf("recovery flags=0x%X colorspace=%u pixel=%.3f %.3f %.3f %.3f\n", deviceStatus.Flags,
                deviceStatus.ColorSpace, pixel[0], pixel[1], pixel[2], pixel[3]);
            Check((deviceStatus.Flags & D3D9ON12_HDR_STATUS_ACTIVE) && std::fabs(pixel[0] - 2.0f) < 0.01f,
                "recovery: replacement presents scRGB with highlights");
        }
    }

    // HDR output off for later devices of the process
    output.Flags = 0;
    setHdr(nullptr, &output);
    device.Reset();
    d3d.Reset();
    DestroyWindow(window);
    std::printf("failures=%d\n", failures);
    return failures ? 4 : 0;
}
