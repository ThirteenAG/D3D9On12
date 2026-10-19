// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#pragma once
#include <array>
#include <cstdint>
#include <mutex>
#include <d3d12.h>

namespace D3D9on12
{
    // Allocation requirements depend on the complete description and node mask.
    // Keep the cache per device and bound its memory independently of resource count.
    template<size_t Capacity = 256> class AllocationInfoCache
    {
        static_assert(Capacity && (Capacity & (Capacity - 1)) == 0);
        using Key = std::array<UINT64, 12>;
        struct Entry { Key key{}; D3D12_RESOURCE_ALLOCATION_INFO info{}; bool valid = false; };
        std::array<Entry, Capacity> m_entries{};
        std::mutex m_mutex;
    public:
        template<class Query> D3D12_RESOURCE_ALLOCATION_INFO Get(
            UINT nodeMask, const D3D12_RESOURCE_DESC& d, Query&& query)
        {
            const Key key = { UINT64(d.Dimension), d.Alignment, d.Width, d.Height,
                d.DepthOrArraySize, d.MipLevels, UINT64(d.Format), d.SampleDesc.Count,
                d.SampleDesc.Quality, UINT64(d.Layout), UINT64(d.Flags), nodeMask };
            UINT64 hash = 0;
            for (auto value : key) hash ^= value + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2);
            const auto slot = size_t(hash) & (Capacity - 1);
            {
                std::lock_guard lock(m_mutex);
                const auto& entry = m_entries[slot];
                if (entry.valid && entry.key == key) return entry.info;
            }
            // Native queries can be expensive. Other hits must remain available.
            auto info = query();
            if (info.SizeInBytes != UINT64_MAX && info.Alignment)
            {
                std::lock_guard lock(m_mutex);
                m_entries[slot] = { key, info, true };
            }
            return info;
        }
    };
}
