#define NOMINMAX
#include "../include/9on12AllocationInfoCache.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <wrl/client.h>

int main(int argc, char** argv)
{
    D3D9on12::AllocationInfoCache<1> cache;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = desc.Height = 128; desc.DepthOrArraySize = 1;
    desc.MipLevels = 1; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    unsigned calls = 0;
    auto query = [&] { ++calls; return D3D12_RESOURCE_ALLOCATION_INFO{65536,65536}; };
    cache.Get(0, desc, query); cache.Get(0, desc, query);
    if (calls != 1) return 1;
    // Every descriptor field and the node mask must participate in equality,
    // even when all descriptions collide in the same cache slot.
    for (unsigned field = 0; field != 12; ++field)
    {
        auto changed = desc; UINT mask = 0;
        switch (field)
        {
        case 0: changed.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D; break;
        case 1: changed.Alignment = 4096; break;
        case 2: ++changed.Width; break;
        case 3: ++changed.Height; break;
        case 4: ++changed.DepthOrArraySize; break;
        case 5: ++changed.MipLevels; break;
        case 6: changed.Format = DXGI_FORMAT_BC1_UNORM; break;
        case 7: ++changed.SampleDesc.Count; break;
        case 8: ++changed.SampleDesc.Quality; break;
        case 9: changed.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; break;
        case 10: changed.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET; break;
        case 11: mask = 1; break;
        }
        const auto before = calls;
        cache.Get(mask, changed, query); cache.Get(mask, changed, query);
        cache.Get(0, desc, query);
        if (calls != before + 2) return 2;
    }
    auto invalid = desc; invalid.Width = 0;
    auto failure = [&] { ++calls; return D3D12_RESOURCE_ALLOCATION_INFO{UINT64_MAX,0}; };
    const auto before = calls;
    cache.Get(0, invalid, failure); cache.Get(0, invalid, failure);
    if (calls != before + 2) return 3;
    std::puts("Allocation-info cache: collisions, all descriptor fields, node masks and invalid-query retries PASS");
    if (argc == 1 || std::strcmp(argv[1], "--native-benchmark")) return 0;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) return 4;
    D3D12_FEATURE_DATA_D3D12_OPTIONS16 options{};
    const auto support = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16, &options, sizeof(options));
    std::printf("GPU upload feature hr=%08lx supported=%d\n", static_cast<unsigned long>(support), options.GPUUploadHeapSupported);
    D3D9on12::AllocationInfoCache<> nativeCache;
    UINT64 checksum = 0;
    for (bool cached : {false,true})
    {
        const auto start = std::chrono::steady_clock::now();
        for (unsigned i = 0; i != 10000; ++i)
        {
            auto d = desc; d.Width = d.Height = 128u << (i % 4);
            d.Format = i & 4 ? DXGI_FORMAT_BC3_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
            auto call = [&] { return device->GetResourceAllocationInfo(0, 1, &d); };
            auto info = cached ? nativeCache.Get(0, d, call) : call();
            if (info.SizeInBytes == UINT64_MAX) return 5;
            checksum += info.SizeInBytes;
        }
        std::printf("10000 allocation queries cached=%d ms=%.3f checksum=%llu\n", cached,
            std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count(),
            static_cast<unsigned long long>(checksum));
    }
    return 0;
}
