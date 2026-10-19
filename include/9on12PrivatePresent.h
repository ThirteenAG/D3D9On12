// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once

// Private D3D12 runtime interfaces used by Windows' in-box d3d9on12.dll to present primaries (exclusive
// fullscreen back buffers): the D3D12 runtime performs the kernel present from the D3DKMT_PRESENT the D3D9
// runtime prepared, so no window or DXGI swap chain is involved. Observed in D3D12Core.dll 10.0.26100.9549
// (x86 and x64; names from Microsoft's public symbols); undocumented and may change, so every use is
// behind a QueryInterface check and a failure degrades to a clean error, never a hang.
namespace D3D9on12
{
    struct D3D12_PRIVATE_PRESENTSURFACE
    {
        ID3D12Resource* pResource;
        UINT Subresource;
    };

    // Filled by ID3D12CommandListPresent::Present and consumed by SubmitPresent: allocation handles, context objects,
    // broadcast arrays, private driver data, sync-interval override, VidPn source. Layout read from D3D12Core.dll
    // 10.0.26100.9549 (CGraphicsCommandList::Present / CGraphicsCommandQueue::SubmitPresent): 812 bytes on x86,
    // 1080 bytes on x64. Only Flags (in: 1) and VidPnSourceId are set by the caller; the rest is zero-initialised.
    struct D3D12_PRIVATE_PRESENT
    {
        UINT hSourceAllocation;
        UINT hDestinationAllocation;
        UINT ContextCount;              // hardware-queue presents
#if defined(_WIN64)
        UINT Reserved12;
        UINT BroadcastContextCount;     // byte 16
        UINT Reserved20;
#else
        UINT BroadcastContextCount;
#endif
        void* Contexts[64];             // context objects (x86 byte 16, x64 byte 24)
        UINT BroadcastSource[64];       // x86 byte 272, x64 byte 536
        UINT BroadcastDestination[64];  // x86 byte 528, x64 byte 792
        const void* pPrivateDriverData; // x86 dword 196, x64 qword 131
        UINT PrivateDriverDataSize;     // x86 dword 197, x64 dword 264
        UINT Flags;                     // x86 dword 198, x64 dword 265: in 1, out from the kernel present
        UINT Reserved;                  // x86 dword 199, x64 dword 266
        UINT SyncIntervalOverrideValid; // x86 dword 200, x64 dword 267
        UINT SyncIntervalOverride;      // x86 dword 201, x64 dword 268
        UINT VidPnSourceId;             // x86 dword 202, x64 dword 269 (in)
    };
#if defined(_WIN64)
    static_assert(sizeof(D3D12_PRIVATE_PRESENT) == 1080, "D3D12_PRIVATE_PRESENT must match the 26100 x64 runtime");
#else
    static_assert(sizeof(D3D12_PRIVATE_PRESENT) == 0x32C, "D3D12_PRIVATE_PRESENT must match the 26100 x86 runtime");
#endif

    // ID3D12CommandQueue private interface (CCommandQueue<...>). IID {c2c09af9-15ce-4c87-bb98-44323032fa7f}
    MIDL_INTERFACE("c2c09af9-15ce-4c87-bb98-44323032fa7f")
    ID3D12CommandQueuePrivatePresent : public IUnknown
    {
        // Submits the present prepared by ID3D12CommandListPresent::Present. The runtime copies pKMTPresent,
        // replaces hContext and the allocation handles with the D3D12 device's and calls D3DKMTPresent (or the
        // hardware-queue variant); the HRESULT is the translated NTSTATUS (S_OK, S_PRESENT_OCCLUDED, ...).
        virtual HRESULT STDMETHODCALLTYPE SubmitPresent(_In_ const void* pKMTPresent, _Inout_ D3D12_PRIVATE_PRESENT* pPresent) = 0;
    };

    // ID3D12GraphicsCommandList private interface ID3D12CommandListPresent_RS5. IID {b0b9e70e-59ce-4415-8116-c8c826b1e599}
    MIDL_INTERFACE("b0b9e70e-59ce-4415-8116-c8c826b1e599")
    ID3D12CommandListPresent : public IUnknown
    {
        virtual void STDMETHODCALLTYPE Blt(_In_ ID3D12Resource* pSource, _In_opt_ const RECT* pSourceRect, _In_opt_ const RECT* pDestinationRect,
            UINT SourceSubresource, _In_ ID3D12Resource* pDestination, _In_opt_ const RECT* pClipRect, _In_opt_ const void* pSubresourceBltMap,
            UINT DestinationSubresource, UINT Flags) = 0;
        // Records the driver's present DDI into the command list for the surfaces and fills pPresent. PresentFlags:
        // 2 = flip, 4 = blt. *pCommandsAdded tells whether the command list now has commands to execute first.
        virtual void STDMETHODCALLTYPE Present(_In_ ID3D12CommandQueue* pCommandQueue, _In_reads_(NumSurfaces) const D3D12_PRIVATE_PRESENTSURFACE* pSurfaces,
            UINT NumSurfaces, _In_opt_ ID3D12Resource* pDestination, _In_opt_ const RECT* pDestinationRect, UINT Reserved, UINT PresentFlags,
            UINT FlipInterval, _Out_ BOOL* pCommandsAdded, _Inout_ D3D12_PRIVATE_PRESENT* pPresent) = 0;
    };

    constexpr UINT D3D12_PRIVATE_PRESENT_FLAG_FLIP = 2;
    constexpr UINT D3D12_PRIVATE_PRESENT_FLAG_BLT = 4;
}
