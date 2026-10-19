# D3D12TranslationLayer fixes

Base commit: `bc726e2af71b0060b879661df62172a9379d1f43` from
https://github.com/microsoft/D3D12TranslationLayer.

`manifest.cmake` records the exact 112-file LF/CRLF input identities and each
patch's canonical LF hash and expected post-images. Preserve BOMs and final
newlines. Unknown or mixed content is rejected.

1. **Legacy present formats** wraps actual backbuffer metadata and converts
   incompatible format families through the blitter. Compatible copy/resolve
   paths remain unchanged. Selected independently by
   `D3D9ON12_TL_LEGACY_PRESENT_FIX`.
2. **Batch worker flush error** reports the first Flush exception while still
   signaling batch consumption, so the caller can receive the failure.
3. **Binding transition fast path** skips redundant binding-driven transition
   requests. Execution is independently controlled by
   `D3D9ON12_TL_BINDING_TRANSITION_FASTPATH`.
4. **Per-draw state** changes command recording pages, producer lock grouping,
   buffer address caching, empty transition handling and shader declaration/table
   bookkeeping. Root CB0 and recording groups default ON, with runtime opt-outs.
   Textually requires patches 2 and 3; legacy present remains independent.
5. **Upload pool concurrency** creates a missing resource outside the container
   lock, allowing concurrent returns during a slow allocation.
6. **Shared upload chunks** uses 2-MiB upload backing buffers and derives local
   offsets from their actual width. Readback and decoder sizes remain unchanged.
   `D3D9ON12_UPLOAD_HEAP_2MIB=0` restores 64 KiB. Requires patch 5.
7. **Pooled buffer updates** routes aliased staging/destination copies through a
   reusable DEFAULT buffer and restores pooled upload destinations to GENERIC_READ.
   Requires patch 6.
8. **GPU upload buffers** adds a dedicated fence-managed GPU-upload pool on
   supported devices, with allocation fallback to host upload memory. Requires
   patch 7.

`D3D9ON12_TL_PER_DRAW_STATE` selects patches 4 through 8 and their prerequisites.
CMake stages the selected series under the build directory, checks complete
post-images and source-file lists, then adds the dependency. An existing correct
stage is verified without touching its source timestamps. Input overrides are
read-only, and Git application is isolated from enclosing repositories and
inherited index/work-tree variables.

To prepare a standalone verified copy explicitly:

```powershell
cmake -DD3D9ON12_TL_INPUT=<clean-pinned-source> -DD3D9ON12_TL_OUTPUT=<new-output> -DD3D9ON12_TL_PATCHES=legacy-present -P cmake/StageD3D12TranslationLayer.cmake
```

Use `<new-output>/source` as `FETCHCONTENT_SOURCE_DIR_D3D12TRANSLATIONLAYER` with
`D3D9ON12_TL_SOURCE_POLICY=VERIFY_PREPATCHED` to build that exact selection. Normal
`STAGE` mode requires the clean pinned input. Input and output must be disjoint.

Update the pin, manifest, patch bodies and expected post-images together when
accepting a change. Run the staging regression in a new scratch directory, rebuild
both architectures with matching symbols, then run correctness and performance
controls. Build and regression commands are in the repository README.
