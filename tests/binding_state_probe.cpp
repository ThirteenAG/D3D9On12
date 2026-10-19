// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <D3D12TranslationLayerDependencyIncludes.h>
#include <D3D12TranslationLayerIncludes.h>
#include <array>
#include <cstdio>
#include <vector>
using namespace D3D12TranslationLayer;

namespace {
struct View {
    CViewSubresourceSubset subset;
    unsigned role;
    bool bound = false;
};
constexpr unsigned Mips = 4, Arrays = 3, Planes = 2, Count = Mips * Arrays * Planes;
constexpr D3D12_RESOURCE_STATES states[] = {
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
    D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_UNORDERED_ACCESS
};
const CSubresourceBindings::BindFunc bind[] = {
    &CSubresourceBindings::PixelShaderResourceViewBound, &CSubresourceBindings::NonPixelShaderResourceViewBound,
    &CSubresourceBindings::RenderTargetViewBound, &CSubresourceBindings::UnorderedAccessViewBound
};
const CSubresourceBindings::BindFunc unbind[] = {
    &CSubresourceBindings::PixelShaderResourceViewUnbound, &CSubresourceBindings::NonPixelShaderResourceViewUnbound,
    &CSubresourceBindings::RenderTargetViewUnbound, &CSubresourceBindings::UnorderedAccessViewUnbound
};

bool Run(bool compact) {
    std::vector<unsigned char> memory(CResourceBindings::CalcPreallocationSize(Count));
    void* cursor = memory.data();
#if defined(D3D12TL_COMPACT_BINDING_REFS_SUPPORTED)
    CResourceBindings bindings(Count, 0, cursor, compact);
#else
    UNREFERENCED_PARAMETER(compact);
    CResourceBindings bindings(Count, 0, cursor);
#endif
    std::array<std::array<unsigned, 4>, Count> refs{};
    auto subset = [](UINT8 nm, UINT16 na, UINT8 np, UINT8 fm = 0, UINT16 fa = 0, UINT8 fp = 0) {
        return CViewSubresourceSubset(CSubresourceSubset(nm, na, np, fm, fa, fp), Mips, Arrays, Planes);
    };
    std::vector<View> views;
    for (unsigned role = 0; role < 4; ++role) {
        views.push_back({subset(Mips, Arrays, Planes), role});
        views.push_back({subset(Mips, Arrays, Planes), role});
        views.push_back({subset(2, 1, 1, 1, 1, 1), role});
        views.push_back({subset(1, 2, 1, 3, 0, 0), role});
    }
    unsigned step = 0;
    auto toggle = [&](unsigned index) {
        auto& view = views[index];
        if (view.bound) bindings.ViewUnboundCommon(view.subset, unbind[view.role]);
        else bindings.ViewBoundCommon(view.subset, bind[view.role]);
        for (auto range : view.subset) {
            for (UINT i = range.first; i < range.second; ++i) {
                if (view.bound) --refs[i][view.role];
                else ++refs[i][view.role];
            }
        }
        view.bound = !view.bound;
        unsigned partial = 0;
        for (auto& v : views) partial += v.bound && !v.subset.IsWholeResource();
        if (bindings.AreAllSubresourcesTheSame() != (partial == 0)) {
            std::printf("compact=%d step=%u uniform classification FAIL\n", compact, step);
            return false;
        }
        for (unsigned i = 0; i < Count; ++i) {
            unsigned expected = 0;
            for (unsigned role = 0; role < 4; ++role) if (refs[i][role]) expected |= states[role];
            if (!expected) expected = UNKNOWN_RESOURCE_STATE;
            const unsigned actual = bindings.GetD3D12ResourceUsageFromBindings(i);
            if (actual != expected) {
                std::printf("compact=%d step=%u subresource=%u actual=%x expected=%x FAIL\n",
                    compact, step, i, actual, expected);
                return false;
            }
        }
        ++step;
        return true;
    };
    // Whole refs, overlapping partial refs, removing the last partial, then
    // updating collapsed refs and expanding again must preserve every mip/plane.
    const unsigned sequence[] = {0,1,6,4,0,11,6,1,4,11,0,0,11,11};
    for (unsigned i : sequence) if (!toggle(i)) return false;
    for (unsigned i = 0; i < views.size(); ++i) if (views[i].bound && !toggle(i)) return false;
    unsigned random = 0x19fa3421;
    for (unsigned i = 0; i < 8192; ++i) {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        if (!toggle(random % static_cast<unsigned>(views.size()))) return false;
    }
    for (unsigned i = 0; i < views.size(); ++i) if (views[i].bound && !toggle(i)) return false;
    // A depth/stencil view can select different access modes for its planes.
    // Check these boolean refs through expansion and collapse too.
    auto whole = subset(Mips, Arrays, Planes);
    auto depth = subset(Mips, Arrays, 1, 0, 0, 0);
    auto stencil = subset(Mips, Arrays, 1, 0, 0, 1);
    bool ps = false, read = false, write = false;
    auto checkPlanes = [&] {
        for (unsigned i = 0; i < Count; ++i) {
            unsigned expected = ps ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : 0;
            if (read && i < Mips * Arrays) expected |= D3D12_RESOURCE_STATE_DEPTH_READ;
            if (write && i >= Mips * Arrays) expected |= D3D12_RESOURCE_STATE_DEPTH_WRITE;
            if (!expected) expected = UNKNOWN_RESOURCE_STATE;
            if (static_cast<unsigned>(bindings.GetD3D12ResourceUsageFromBindings(i)) != expected) {
                std::printf("compact=%d plane access subresource=%u FAIL\n", compact, i);
                return false;
            }
        }
        return true;
    };
    bindings.ViewBoundCommon(whole, &CSubresourceBindings::PixelShaderResourceViewBound); ps = true;
    if (!checkPlanes()) return false;
    bindings.ViewBoundCommon(depth, &CSubresourceBindings::ReadOnlyDepthStencilViewBound); read = true;
    if (!checkPlanes()) return false;
    bindings.ViewBoundCommon(stencil, &CSubresourceBindings::WritableDepthStencilViewBound); write = true;
    if (!checkPlanes()) return false;
    bindings.ViewUnboundCommon(depth, &CSubresourceBindings::DepthStencilViewUnbound); read = false;
    if (!checkPlanes()) return false;
    bindings.ViewUnboundCommon(stencil, &CSubresourceBindings::DepthStencilViewUnbound); write = false;
    if (!checkPlanes()) return false;
    bindings.ViewUnboundCommon(whole, &CSubresourceBindings::PixelShaderResourceViewUnbound); ps = false;
    if (!checkPlanes()) return false;
    std::printf("compact=%d steps=%u subresources=%u PASS\n", compact, step, Count);
    return true;
}
}
int main() {
    bool ok = Run(false);
#if defined(D3D12TL_COMPACT_BINDING_REFS_SUPPORTED)
    ok = Run(true) && ok;
#endif
    return ok ? 0 : 1;
}
