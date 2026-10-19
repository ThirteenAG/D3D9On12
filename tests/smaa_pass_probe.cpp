// Licensed under the MIT license.
// Development probe: fixed-input SMAA passes using separately supplied shader resources.
// No game source changes or game process required; the ASI is mapped as data only.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <d3d9on12.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <array>
#include <vector>
#include <string>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include "probe_driver_identity.h"
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
UINT Width = 384, Height = 216;
struct ApiError { const char* operation; HRESULT hr; };
#define HR(expression) do { HRESULT probeCheckedHr = (expression); if (FAILED(probeCheckedHr)) throw ApiError{#expression, probeCheckedHr}; } while(0)

struct Module
{
    HMODULE value = nullptr;
    ~Module() { if (value) FreeLibrary(value); }
};
struct Window
{
    HWND value = CreateWindowW(L"STATIC", L"Fixed-input SMAA probe", WS_OVERLAPPED,
        0, 0, Width, Height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ~Window() { if (value) DestroyWindow(value); }
};
const void* Resource(HMODULE module, UINT id, DWORD* bytes = nullptr)
{
    HRSRC item = FindResourceW(module, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
    if (!item) throw ApiError{"FindResource", HRESULT_FROM_WIN32(GetLastError())};
    HGLOBAL loaded = LoadResource(module, item);
    const void* data = loaded ? LockResource(loaded) : nullptr;
    DWORD size = SizeofResource(module, item);
    if (!data || !size) throw ApiError{"LoadResource", E_FAIL};
    if (bytes) *bytes = size;
    return data;
}
using CreateTextureFromMemory = HRESULT(WINAPI*)(IDirect3DDevice9*, const void*, UINT,
    UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, DWORD, DWORD, D3DCOLOR,
    void*, PALETTEENTRY*, IDirect3DTexture9**);
struct ShaderBuffer : IUnknown
{
    virtual void* STDMETHODCALLTYPE GetBufferPointer() = 0;
    virtual DWORD STDMETHODCALLTYPE GetBufferSize() = 0;
};
using CompileResourceShader = HRESULT(WINAPI*)(HMODULE, LPCWSTR, const void*, void*,
    LPCSTR, LPCSTR, DWORD, ShaderBuffer**, ShaderBuffer**, IUnknown**);
ComPtr<IDirect3DTexture9> Lookup(IDirect3DDevice9* device, HMODULE resources,
    CreateTextureFromMemory create, UINT id, UINT width, UINT height)
{
    DWORD bytes = 0;
    const void* data = Resource(resources, id, &bytes);
    ComPtr<IDirect3DTexture9> texture;
    // D3DX_FILTER_LINEAR=3, matching postfx.ixx's lookup texture loader.
    HR(create(device, data, bytes, width, height, 1, 0, D3DFMT_UNKNOWN,
        D3DPOOL_MANAGED, 3, 3, 0, nullptr, nullptr, &texture));
    D3DSURFACE_DESC desc{};
    HR(texture->GetLevelDesc(0, &desc));
    std::printf("lookup id=%u dimensions=%ux%u format=%u\n", id, desc.Width, desc.Height, desc.Format);
    return texture;
}
ComPtr<IDirect3DTexture9> Target(IDirect3DDevice9* device, D3DFORMAT format)
{
    ComPtr<IDirect3DTexture9> result;
    HR(device->CreateTexture(Width, Height, 1, D3DUSAGE_RENDERTARGET, format,
        D3DPOOL_DEFAULT, &result, nullptr));
    return result;
}
std::vector<DWORD> Pixels(IDirect3DDevice9* device, IDirect3DSurface9* source)
{
    D3DSURFACE_DESC desc{};
    HR(source->GetDesc(&desc));
    ComPtr<IDirect3DSurface9> copy;
    HR(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format,
        D3DPOOL_SYSTEMMEM, &copy, nullptr));
    HR(device->GetRenderTargetData(source, copy.Get()));
    D3DLOCKED_RECT rect{};
    HR(copy->LockRect(&rect, nullptr, D3DLOCK_READONLY));
    std::vector<DWORD> result(desc.Width * desc.Height);
    for (UINT y = 0; y < desc.Height; ++y)
        std::memcpy(result.data() + y * desc.Width,
            static_cast<const BYTE*>(rect.pBits) + y * rect.Pitch, desc.Width * 4);
    HR(copy->UnlockRect());
    return result;
}
void Save(const fs::path& path, const std::vector<DWORD>& pixels)
{
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("Cannot create output file");
    out.write(reinterpret_cast<const char*>(pixels.data()), pixels.size() * sizeof(DWORD));
    if (!out) throw std::runtime_error("Cannot write output file");
}
constexpr D3DSAMPLERSTATETYPE SamplerFields[] = {D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER,
    D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_ADDRESSW};
struct SamplerSnapshot
{
    DWORD values[5][6]{}, srgbTexture = 0, srgbWrite = 0;
    void Capture(IDirect3DDevice9* device)
    {
        for (UINT slot = 0; slot < 5; ++slot)
            for (UINT state = 0; state < 6; ++state)
                HR(device->GetSamplerState(slot, SamplerFields[state], &values[slot][state]));
        HR(device->GetSamplerState(0, D3DSAMP_SRGBTEXTURE, &srgbTexture));
        HR(device->GetRenderState(D3DRS_SRGBWRITEENABLE, &srgbWrite));
    }
    void RestoreAndCheck(IDirect3DDevice9* device, bool legacy)
    {
        HR(device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, srgbTexture));
        HR(device->SetRenderState(D3DRS_SRGBWRITEENABLE, srgbWrite));
        for (UINT slot = 0; slot < 5; ++slot)
            for (UINT state = 0; state < 6; ++state)
                HR(device->SetSamplerState(slot, SamplerFields[state], legacy ? srgbTexture : values[slot][state]));
        SamplerSnapshot actual; actual.Capture(device);
        for (UINT slot = 0; slot < 5; ++slot)
            for (UINT state = 0; state < 6; ++state)
                if (actual.values[slot][state] != values[slot][state])
                {
                    std::printf("sampler restoration mismatch slot=%u field=%u actual=%lu expected=%lu legacy=%u\n",
                        slot, static_cast<UINT>(SamplerFields[state]), actual.values[slot][state], values[slot][state], legacy);
                    throw std::runtime_error("Sampler state restoration differs from captured state");
                }
        if (actual.srgbTexture != srgbTexture || actual.srgbWrite != srgbWrite)
            throw std::runtime_error("SRGB state restoration mismatch");
    }
};
void SmaaSamplers(IDirect3DDevice9* device)
{
    for (UINT slot = 0; slot <= 4; ++slot)
    {
        HR(device->SetSamplerState(slot, D3DSAMP_MINFILTER, slot == 3 ? D3DTEXF_POINT : D3DTEXF_LINEAR));
        HR(device->SetSamplerState(slot, D3DSAMP_MAGFILTER, slot == 3 ? D3DTEXF_POINT : D3DTEXF_LINEAR));
        HR(device->SetSamplerState(slot, D3DSAMP_MIPFILTER, (slot == 0 || slot == 3) ? D3DTEXF_POINT : D3DTEXF_LINEAR));
        HR(device->SetSamplerState(slot, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP));
        HR(device->SetSamplerState(slot, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));
        HR(device->SetSamplerState(slot, D3DSAMP_ADDRESSW, (slot == 2 || slot == 3) ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP));
        HR(device->SetSamplerState(slot, D3DSAMP_SRGBTEXTURE, FALSE));
    }
}
int Run(int argc, wchar_t** argv)
{
    bool native = false, debug = false, legacyRestore = false, compileShaders = false;
    fs::path resourcePath, output, dxvkPath, inputPath;
    for (int i = 1; i < argc; ++i)
    {
        std::wstring arg = argv[i];
        if (arg == L"--native") native = true;
        else if (arg == L"--debug") debug = true;
        else if (arg == L"--legacy-restore") legacyRestore = true;
        else if (arg == L"--compile-shaders") compileShaders = true;
        else if (arg == L"--resources" && i + 1 < argc) resourcePath = argv[++i];
        else if (arg == L"--out" && i + 1 < argc) output = argv[++i];
        else if (arg == L"--input-bgra" && i + 1 < argc) inputPath = argv[++i];
        else if (arg == L"--width" && i + 1 < argc) Width = static_cast<UINT>(std::stoul(argv[++i]));
        else if (arg == L"--height" && i + 1 < argc) Height = static_cast<UINT>(std::stoul(argv[++i]));
        else if (arg == L"--dxvk" && i + 1 < argc) { native = true; dxvkPath = argv[++i]; }
        else throw std::runtime_error("Unknown or incomplete argument");
    }
    if (resourcePath.empty() || output.empty())
        throw std::runtime_error("Required: --resources <frozen.asi> --out <new output directory>");
    if (!Width || !Height || Width > 4096 || Height > 4096)
        throw std::runtime_error("Dimensions must be 1..4096");
    if (!fs::create_directory(output)) throw std::runtime_error("Output directory must be new");
    Module resources{LoadLibraryExW(resourcePath.c_str(), nullptr,
        LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE)};
    if (!resources.value) throw ApiError{"LoadLibraryEx(resources as data)", HRESULT_FROM_WIN32(GetLastError())};
    wchar_t systemDirectory[MAX_PATH]{};
    if (!GetSystemDirectoryW(systemDirectory, MAX_PATH)) throw std::runtime_error("GetSystemDirectory failed");
    Module runtime{LoadLibraryW((dxvkPath.empty() ? fs::path(systemDirectory) / L"d3d9.dll" : dxvkPath).c_str())};
    Module d3dx{LoadLibraryW((fs::path(systemDirectory) / L"d3dx9_43.dll").c_str())};
    if (!runtime.value || !d3dx.value) throw std::runtime_error("D3D9/D3DX9 runtime unavailable");
    auto loadTexture = reinterpret_cast<CreateTextureFromMemory>(GetProcAddress(d3dx.value, "D3DXCreateTextureFromFileInMemoryEx"));
    if (!loadTexture) throw std::runtime_error("D3DX texture loader unavailable");
    if (debug && !native)
    {
        ComPtr<ID3D12Debug> validation;
        HR(D3D12GetDebugInterface(IID_PPV_ARGS(&validation)));
        validation->EnableDebugLayer();
    }
    ComPtr<IDirect3D9> d3d;
    if (!native && !ProbeLocalDriver()) throw std::runtime_error("Local on12 driver unavailable");
    if (native)
    {
        auto create = reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(runtime.value, "Direct3DCreate9"));
        if (!create) throw std::runtime_error("Direct3DCreate9 unavailable");
        d3d.Attach(create(D3D_SDK_VERSION));
    }
    else
    {
        auto create = reinterpret_cast<PFN_Direct3DCreate9On12>(GetProcAddress(runtime.value, "Direct3DCreate9On12"));
        if (!create) throw std::runtime_error("Direct3DCreate9On12 unavailable");
        D3D9ON12_ARGS args{}; args.Enable9On12 = TRUE;
        d3d.Attach(create(D3D_SDK_VERSION, &args, 1));
    }
    if (!d3d) throw std::runtime_error("D3D9 creation failed");
    Window window;
    if (!window.value) throw std::runtime_error("Window creation failed");
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE; pp.hDeviceWindow = window.value;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = Width; pp.BackBufferHeight = Height;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    ComPtr<IDirect3DDevice9> device;
    HR(d3d->CreateDevice(0, D3DDEVTYPE_HAL, window.value, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device));
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12InfoQueue> messages;
    if (!native)
    {
        HR(ProbeRequireActualDriver(device.Get()));
        std::puts("actual device driver identity PASS");
        ComPtr<IDirect3DDevice9On12> interop;
        HR(device.As(&interop)); HR(interop->GetD3D12Device(IID_PPV_ARGS(&device12)));
        if (debug) { HR(device12.As(&messages)); HR(messages->SetMessageCountLimit(4096)); }
        wchar_t loaded[MAX_PATH]{};
        GetModuleFileNameW(GetModuleHandleW(L"d3d9on12.dll"), loaded, MAX_PATH);
        std::printf("on12 driver=%ls\n", loaded);
    }
    ComPtr<IDirect3DTexture9> color;
    HR(device->CreateTexture(Width, Height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &color, nullptr));
    std::vector<DWORD> input(Width * Height);
    for (UINT y = 0; y < Height; ++y)
        for (UINT x = 0; x < Width; ++x)
        {
            DWORD pixel;
            if (x < Width / 3)
            {
                UINT step = 1 + y / 36;
                pixel = ((x / step + y / step) & 1) ? 0xffeeeeee : 0xff101010;
            }
            else if (x < Width * 2 / 3)
            {
                UINT gray = (x * 193 + y * 17) & 255;
                pixel = 0xff000000 | gray * 0x010101;
                if (((x - Width / 3) * 3) > y) pixel = 0xffb04c21;
            }
            else
            {
                int dx = static_cast<int>(x) - 320, dy = static_cast<int>(y) - 108;
                pixel = (dx * dx + dy * dy < 80 * 80) ? 0xff20b0e0 : 0xffd04020;
                if (x % 19 == 0 || y % 23 == 0) pixel = 0xffededed;
            }
            input[y * Width + x] = pixel;
        }
    if (!inputPath.empty())
    {
        if (fs::file_size(inputPath) != input.size() * sizeof(DWORD))
            throw std::runtime_error("Input BGRA byte count differs from specified dimensions");
        std::ifstream file(inputPath, std::ios::binary);
        file.read(reinterpret_cast<char*>(input.data()), input.size() * sizeof(DWORD));
        if (!file) throw std::runtime_error("Cannot read input BGRA file");
    }
    Save(output / L"input.bgra", input);
    D3DLOCKED_RECT locked{};
    HR(color->LockRect(0, &locked, nullptr, 0));
    for (UINT y = 0; y < Height; ++y)
        std::memcpy(static_cast<BYTE*>(locked.pBits) + y * locked.Pitch, input.data() + y * Width, Width * 4);
    HR(color->UnlockRect(0));
    auto area = Lookup(device.Get(), resources.value, loadTexture, 103, 160, 560);
    auto search = Lookup(device.Get(), resources.value, loadTexture, 104, 64, 16);
    std::array<ComPtr<IDirect3DPixelShader9>, 3> ps;
    std::array<ComPtr<IDirect3DVertexShader9>, 3> vs;
    auto compile = reinterpret_cast<CompileResourceShader>(GetProcAddress(d3dx.value, "D3DXCompileShaderFromResourceW"));
    const char* pixelEntries[] = {"DX9_SMAALumaEdgeDetectionPS", "DX9_SMAABlendingWeightCalculationPS", "DX9_SMAANeighborhoodBlendingPS"};
    const char* vertexEntries[] = {"DX9_SMAAEdgeDetectionVS", "DX9_SMAABlendingWeightCalculationVS", "DX9_SMAANeighborhoodBlendingVS"};
    auto code = [&](UINT resourceId, const char* entry, const char* profile, ComPtr<ShaderBuffer>& compiled) -> const DWORD*
    {
        if (!compileShaders) return static_cast<const DWORD*>(Resource(resources.value, resourceId));
        if (!compile) throw std::runtime_error("D3DX shader compiler unavailable");
        ComPtr<ShaderBuffer> errors;
        HRESULT result = compile(resources.value, MAKEINTRESOURCEW(102), nullptr, nullptr,
            entry, profile, 0, &compiled, &errors, nullptr);
        if (FAILED(result))
        {
            if (errors) std::printf("compile %s: %.*s\n", entry, static_cast<int>(errors->GetBufferSize()),
                static_cast<const char*>(errors->GetBufferPointer()));
            throw ApiError{entry, result};
        }
        std::ofstream bytes(output / (std::string(entry) + ".dx9"), std::ios::binary);
        bytes.write(static_cast<const char*>(compiled->GetBufferPointer()), compiled->GetBufferSize());
        if (!bytes) throw std::runtime_error("Cannot save compiled shader bytecode");
        return static_cast<const DWORD*>(compiled->GetBufferPointer());
    };
    for (UINT i = 0; i < 3; ++i)
    {
        ComPtr<ShaderBuffer> pixelCode, vertexCode;
        HR(device->CreatePixelShader(code(2102 + i, pixelEntries[i], "ps_3_0", pixelCode), &ps[i]));
        HR(device->CreateVertexShader(code(2105 + i, vertexEntries[i], "vs_3_0", vertexCode), &vs[i]));
    }
    std::array<ComPtr<IDirect3DTexture9>, 3> targets = {
        Target(device.Get(), D3DFMT_X8R8G8B8), Target(device.Get(), D3DFMT_A8R8G8B8), Target(device.Get(), D3DFMT_X8R8G8B8)};
    std::array<ComPtr<IDirect3DSurface9>, 3> surfaces;
    for (UINT i = 0; i < 3; ++i) HR(targets[i]->GetSurfaceLevel(0, &surfaces[i]));
    struct Vertex { float x, y, z, w, u, v; };
    const float left = -1.f - 1.f / Width, right = 1.f - 1.f / Width;
    const float top = 1.f + 1.f / Height, bottom = -1.f + 1.f / Height;
    const Vertex vertices[] = { {left,top,0,1,0,0}, {right,top,0,1,1,0}, {left,bottom,0,1,0,1},
        {left,bottom,0,1,0,1}, {right,top,0,1,1,0}, {right,bottom,0,1,1,1} };
    ComPtr<IDirect3DVertexBuffer9> buffer;
    HR(device->CreateVertexBuffer(sizeof(vertices), D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &buffer, nullptr));
    void* vertexData = nullptr;
    HR(buffer->Lock(0, sizeof(vertices), &vertexData, 0));
    std::memcpy(vertexData, vertices, sizeof(vertices)); HR(buffer->Unlock());
    const D3DVERTEXELEMENT9 elements[] = {
        {0,0,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},
        {0,16,D3DDECLTYPE_FLOAT2,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,0}, D3DDECL_END() };
    ComPtr<IDirect3DVertexDeclaration9> declaration;
    HR(device->CreateVertexDeclaration(elements, &declaration));
    HR(device->SetVertexDeclaration(declaration.Get()));
    HR(device->SetStreamSource(0, buffer.Get(), 0, sizeof(Vertex)));
    HR(device->SetRenderState(D3DRS_ZENABLE, FALSE));
    HR(device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE));
    HR(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
    HR(device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE));
    HR(device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE));
    HR(device->SetRenderState(D3DRS_FOGENABLE, FALSE));
    HR(device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE));
    HR(device->SetRenderState(D3DRS_COLORWRITEENABLE, 15));
    const float metrics[] = {static_cast<float>(Width), static_cast<float>(Height), 1.f/Width, 1.f/Height};
    HR(device->SetVertexShaderConstantF(44, metrics, 1));
    HR(device->SetPixelShaderConstantF(44, metrics, 1));
    for (UINT slot = 0; slot <= 4; ++slot)
    {
        // Distinct valid seeds expose restoration aliasing without submitting invalid sampler enums.
        HR(device->SetSamplerState(slot, D3DSAMP_MINFILTER, slot & 1 ? D3DTEXF_POINT : D3DTEXF_LINEAR));
        HR(device->SetSamplerState(slot, D3DSAMP_MAGFILTER, slot & 1 ? D3DTEXF_LINEAR : D3DTEXF_POINT));
        HR(device->SetSamplerState(slot, D3DSAMP_MIPFILTER, slot % 3));
        HR(device->SetSamplerState(slot, D3DSAMP_ADDRESSU, 1 + slot % 3));
        HR(device->SetSamplerState(slot, D3DSAMP_ADDRESSV, 1 + (slot + 1) % 3));
        HR(device->SetSamplerState(slot, D3DSAMP_ADDRESSW, 1 + (slot + 2) % 3));
    }
    HR(device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, TRUE));
    HR(device->SetTexture(0, color.Get())); HR(device->SetTexture(2, area.Get())); HR(device->SetTexture(3, search.Get()));
    std::array<std::vector<DWORD>, 3> first;
    const wchar_t* names[] = {L"edges", L"weights", L"final"};
    for (UINT frame = 0; frame < 3; ++frame)
    {
        SamplerSnapshot previous; previous.Capture(device.Get());
        SmaaSamplers(device.Get());
        HR(device->SetTexture(1, nullptr)); HR(device->SetTexture(4, nullptr));
        HR(device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE));
        HR(device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE));
        HR(device->BeginScene());
        for (UINT pass = 0; pass < 3; ++pass)
        {
            HR(device->SetRenderTarget(0, surfaces[pass].Get()));
            D3DVIEWPORT9 viewport{0,0,Width,Height,0,1}; HR(device->SetViewport(&viewport));
            HR(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 0, 0));
            HR(device->SetPixelShader(ps[pass].Get())); HR(device->SetVertexShader(vs[pass].Get()));
            if (pass == 1) HR(device->SetTexture(1, targets[0].Get()));
            if (pass == 2)
            {
                HR(device->SetTexture(4, targets[1].Get()));
                HR(device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, TRUE));
                HR(device->SetRenderState(D3DRS_SRGBWRITEENABLE, TRUE));
            }
            HR(device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2));
        }
        HR(device->EndScene());
        // Read only after all three passes; intermediate readbacks cannot hide ordering defects.
        for (UINT pass = 0; pass < 3; ++pass)
        {
            auto pixels = Pixels(device.Get(), surfaces[pass].Get());
            if (!frame) first[pass] = pixels;
            else if (pixels != first[pass]) throw std::runtime_error("Fixed-input repeated frame mismatch");
            Save(output / (std::wstring(names[pass]) + L"-" + std::to_wstring(frame) + L".bgra"), pixels);
            size_t nonzero = 0; for (DWORD pixel : pixels) if (pixel & (pass == 1 ? 0xffffffffu : 0xffffffu)) ++nonzero;
            std::printf("frame=%u pass=%u nonzero=%zu/%zu\n", frame, pass, nonzero, pixels.size());
            if (!nonzero) throw std::runtime_error("Empty SMAA pass output");
        }
        previous.RestoreAndCheck(device.Get(), legacyRestore);
    }
    UINT errors = 0;
    if (messages)
    {
        for (UINT64 i = 0; i < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i)
        {
            SIZE_T size = 0; HR(messages->GetMessage(i, nullptr, &size));
            std::vector<BYTE> storage(size); auto message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            HR(messages->GetMessage(i, message, &size));
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
            { ++errors; std::printf("D3D12 error %u: %s\n", message->ID, message->pDescription); }
        }
    }
    if (device12) HR(device12->GetDeviceRemovedReason());
    if (errors) throw std::runtime_error("D3D12 validation errors");
    std::printf("PASS %ux%u three SMAA passes, three identical frames, validationErrors=%u\n", Width, Height, errors);
    return 0;
}
int wmain(int argc, wchar_t** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    try { return Run(argc, argv); }
    catch (const ApiError& error) { std::printf("FAIL %s hr=%08lx\n", error.operation, static_cast<ULONG>(error.hr)); }
    catch (const std::exception& error) { std::printf("FAIL %s\n", error.what()); }
    return 1;
}
