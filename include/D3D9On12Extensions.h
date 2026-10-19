// Licensed under the MIT license.
#pragma once
#include <stdint.h>

// Optional D3D9On12 extension interfaces. Windows/upstream drivers do not export these
// functions. Query the loaded driver module with GetProcAddress after device creation, outside
// DllMain, and keep the ordinary paths when an export or capability bit is absent.
#define D3D9ON12_CAPABILITIES_ABI 2u
#define D3D9ON12_CAP_SAMPLER_ADDRESS_VALIDATION (1u << 0)   // ABI 1 and later
#define D3D9ON12_CAP_QUEUED_INTEROP             (1u << 1)   // ABI 2 and later: D3D9On12QueueInterop
// ABI 2: the driver presents from an exclusive fullscreen device (primaries through the D3D12 runtime's private
// present interfaces). Reported only when the installed D3D12 runtime offers those interfaces;
// clients can create a windowed device when exclusive presentation is unavailable.
#define D3D9ON12_CAP_EXCLUSIVE_FULLSCREEN       (1u << 2)
#define D3D9ON12_CAPABILITIES_EXPORT "D3D9On12Capabilities"
#define D3D9ON12_QUEUE_INTEROP_EXPORT "D3D9On12QueueInterop"

// ABI 1 returns the ABI-1 bits, ABI 2 returns all bits above; other versions return 0.
typedef uint32_t (__stdcall* PFN_D3D9On12Capabilities)(uint32_t abiVersion);

// Queued interop: copies between D3D9 resources of this device and foreign D3D12 resources (for
// example textures shared by another process), with a fence signal or wait, recorded in order with
// the D3D9 command stream and executed by the driver without draining it on the calling thread.
//
// Usage: fill the request, call D3D9On12QueueInterop to receive a token, then issue
//   IDirect3DDevice9::SetRenderState((D3DRENDERSTATETYPE)D3D9ON12_INTEROP_MARKER_RENDERSTATE, token)
// on the same device. The runtime forwards that render state to the driver in command-stream order;
// the driver consumes the token there and performs the operation at that point of the stream. Each
// token is consumed once; an unconsumed token is dropped when the device is destroyed. The marker
// state itself is not applied as a render state.
//
// Game resources are identified by the ID3D12Resource obtained from
// IDirect3DDevice9On12::UnwrapUnderlyingResource (followed by ReturnUnderlyingResource); that call
// registers the resource with the driver. Foreign resources must be in D3D12_RESOURCE_STATE_COMMON
// when the operation executes and are left in COMMON. Only subresource 0 is copied, whole.
//
// EXPORT_COPY: after all D3D9 work recorded so far, copy GameResources[i] -> ForeignResources[i],
//   submit, then Signal(Fence, FenceValue) on the driver's command queue.
// IMPORT_COPY: submit the work recorded so far, Wait(Fence, FenceValue) on the driver's command
//   queue, then copy ForeignResources[i] -> GameResources[i]; later D3D9 use of the game resources
//   is ordered after the copy.
#define D3D9ON12_INTEROP_MARKER_RENDERSTATE 183u   // D3DRS_ADAPTIVETESS_W
#define D3D9ON12_INTEROP_EXPORT_COPY 1u
#define D3D9ON12_INTEROP_IMPORT_COPY 2u

struct ID3D12Device;
struct ID3D12Resource;
struct ID3D12Fence;

typedef struct D3D9ON12_INTEROP_REQUEST
{
    uint32_t StructSize;                      // sizeof(D3D9ON12_INTEROP_REQUEST)
    uint32_t Operation;                       // D3D9ON12_INTEROP_*_COPY
    ID3D12Device* Device;                     // from IDirect3DDevice9On12::GetD3D12Device, identifies the D3D9On12 device
    uint32_t Count;                           // number of resource pairs (1..8)
    uint32_t Reserved;
    ID3D12Resource* const* GameResources;     // Count entries, from UnwrapUnderlyingResource; null entries are skipped
    ID3D12Resource* const* ForeignResources;  // Count entries
    ID3D12Fence* Fence;
    uint64_t FenceValue;
} D3D9ON12_INTEROP_REQUEST;

// Returns S_OK and a token, E_INVALIDARG for a malformed request, E_NOINTERFACE when Device is not
// a D3D9On12 device of this driver, E_FAIL when a game resource was never unwrapped.
typedef HRESULT (__stdcall* PFN_D3D9On12QueueInterop)(const D3D9ON12_INTEROP_REQUEST* request, uint32_t* token);
