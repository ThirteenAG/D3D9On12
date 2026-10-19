// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <D3D12TranslationLayerDependencyIncludes.h>
#include <D3D12TranslationLayerIncludes.h>
#include <future>
#include <chrono>
#include <thread>
#include <cstdio>

using Pool = D3D12TranslationLayer::CMultiLevelPool<std::unique_ptr<int>, 65536>;

int main()
{
    Pool pool(100, true);
    unsigned creations = 0;
    auto create = [&](UINT64) { ++creations; return std::make_unique<int>(7); };
    pool.ReturnToPool(65536, std::make_unique<int>(1), 1);
    if (*pool.RetrieveFromPool(65536, 1, create) != 1 || creations) return 1;

    // Existing, empty bucket: the expensive factory must not serialize a
    // concurrent return behind it. All waits are bounded and threads joined
    // even when this regression is present in the original translation layer.
    std::promise<void> entered, release, returned;
    auto enteredFuture = entered.get_future();
    auto releaseFuture = release.get_future();
    auto returnedFuture = returned.get_future();
    std::unique_ptr<int> allocated;
    std::thread allocator([&] {
        allocated = pool.RetrieveFromPool(65536, 1, [&](UINT64) {
            entered.set_value(); releaseFuture.wait(); return std::make_unique<int>(2);
        });
    });
    if (enteredFuture.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
    {
        release.set_value(); allocator.join(); std::puts("FAIL factory did not start"); return 1;
    }
    std::thread returner([&] { pool.ReturnToPool(65536, std::make_unique<int>(3), 1); returned.set_value(); });
    bool progressed = returnedFuture.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready;
    release.set_value(); allocator.join(); returner.join();
    if (!progressed) { std::puts("FAIL resource creation held the shared pool lock"); return 1; }
    if (!allocated || *allocated != 2 || *pool.RetrieveFromPool(65536, 1, create) != 3 || creations) return 1;

    // A buffer returned for a later fence must remain unavailable until that
    // fence completes. Taking the allocation path must preserve the old entry.
    pool.ReturnToPool(65536, std::make_unique<int>(4), 10);
    if (*pool.RetrieveFromPool(65536, 9, create) != 7 || creations != 1) return 1;
    if (*pool.RetrieveFromPool(65536, 10, create) != 4 || creations != 1) return 1;

    bool threw = false;
    try { pool.RetrieveFromPool(65536, 10, [](UINT64) -> std::unique_ptr<int> { throw std::bad_alloc(); }); }
    catch (const std::bad_alloc&) { threw = true; }
    if (!threw) return 1;
    pool.ReturnToPool(65536, std::make_unique<int>(5), 10);
    if (*pool.RetrieveFromPool(65536, 10, create) != 5 || creations != 1) return 1;
    std::puts("PASS pool concurrency, completed fences and allocation failure recovery");
    return 0;
}
