# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.
include_guard(GLOBAL)
include(FetchContent)
include("${CMAKE_CURRENT_LIST_DIR}/StageD3D12TranslationLayer.cmake")

option(D3D9ON12_TL_LEGACY_PRESENT_FIX "Correct incompatible formats in legacy TL Present" ON)
# Performance: ResourceStateManager skips binding-driven transition requests that are complete
# no-ops (resource already exclusively in a covering state and already accounted in the current
# graphics command list). Measured on GTA IV: ~0.8 ms/frame less on the TL worker. Patch 0003.
option(D3D9ON12_TL_BINDING_TRANSITION_FASTPATH "Skip redundant binding-driven resource transitions in the TL" ON)
option(D3D9ON12_TL_PER_DRAW_STATE "Use verified per-draw recording and binding-state changes in the TL" ON)
set(D3D9ON12_TL_SOURCE_POLICY "STAGE" CACHE STRING "TL fix input policy: STAGE or VERIFY_PREPATCHED")
set_property(CACHE D3D9ON12_TL_SOURCE_POLICY PROPERTY STRINGS STAGE VERIFY_PREPATCHED)

function(d3d9on12_add_verified_translation_layer)
    set(patches)
    if(D3D9ON12_TL_LEGACY_PRESENT_FIX)
        list(APPEND patches legacy-present)
    endif()
    # The worker Flush error-handling fix belongs to the batching worker.
    if(D3D9ON12_BATCHED_RENDERING OR D3D9ON12_TL_PER_DRAW_STATE)
        list(APPEND patches batch-worker-flush)
    endif()
    if(D3D9ON12_TL_BINDING_TRANSITION_FASTPATH OR D3D9ON12_TL_PER_DRAW_STATE)
        list(APPEND patches binding-transition-fastpath)
    endif()
    # 0004 modifies files already changed by 0002/0003. Select their textual
    # prerequisites even when the corresponding runtime paths are disabled.
    if(D3D9ON12_TL_PER_DRAW_STATE)
        list(APPEND patches per-draw-state upload-pool-concurrency shared-upload-chunks pooled-buffer-updates gpu-upload-buffers)
    endif()
    set(D3D12TL_EXPERIMENTAL_LEGACY_PRESENT_FORMATS "${D3D9ON12_TL_LEGACY_PRESENT_FIX}")
    set(populate_only)
    if(patches AND NOT CMAKE_VERSION VERSION_LESS 3.18)
        list(APPEND populate_only SOURCE_SUBDIR .d3d9on12-populate-only)
    endif()
    FetchContent_Declare(d3d12translationlayer
        GIT_REPOSITORY https://github.com/microsoft/D3D12TranslationLayer.git
        GIT_TAG bc726e2af71b0060b879661df62172a9379d1f43
        ${populate_only})
    if(NOT patches)
        FetchContent_MakeAvailable(d3d12translationlayer)
        message(STATUS "TL fixes disabled; direct source: ${d3d12translationlayer_SOURCE_DIR}")
        return()
    endif()
    if(NOT D3D9ON12_TL_SOURCE_POLICY MATCHES "^(STAGE|VERIFY_PREPATCHED)$")
        message(FATAL_ERROR "D3D9ON12_TL_SOURCE_POLICY must be STAGE or VERIFY_PREPATCHED")
    endif()
    if(D3D9ON12_TL_SOURCE_POLICY STREQUAL "VERIFY_PREPATCHED" AND NOT FETCHCONTENT_SOURCE_DIR_D3D12TRANSLATIONLAYER)
        message(FATAL_ERROR "VERIFY_PREPATCHED requires an explicit FETCHCONTENT_SOURCE_DIR_D3D12TRANSLATIONLAYER")
    endif()
    if(CMAKE_VERSION VERSION_LESS 3.18)
        FetchContent_GetProperties(d3d12translationlayer)
        if(NOT d3d12translationlayer_POPULATED)
            FetchContent_Populate(d3d12translationlayer)
        endif()
    else()
        if(FETCHCONTENT_SOURCE_DIR_D3D12TRANSLATIONLAYER AND EXISTS "${FETCHCONTENT_SOURCE_DIR_D3D12TRANSLATIONLAYER}/.d3d9on12-populate-only/CMakeLists.txt")
            message(FATAL_ERROR "Reserved populate-only path exists in TL override")
        endif()
        # A documented non-existent SOURCE_SUBDIR populates without adding TL.
        FetchContent_MakeAvailable(d3d12translationlayer)
    endif()
    if(TARGET d3d12translationlayer OR TARGET d3d12translationlayer_wdk)
        message(FATAL_ERROR "TL was already added before patch verification; use a source override rather than an already-created target")
    endif()
    d3d9on12_prepare_translation_layer(INPUT "${d3d12translationlayer_SOURCE_DIR}"
        OUTPUT "${CMAKE_BINARY_DIR}/_deps/d3d12translationlayer-patched/@SIGNATURE@"
        POLICY "${D3D9ON12_TL_SOURCE_POLICY}" PATCHES ${patches}
        OUT_SOURCE source OUT_SIGNATURE signature)
    string(SUBSTRING "${signature}" 0 16 short_signature)
    message(STATUS "TL clean input: ${d3d12translationlayer_SOURCE_DIR}")
    message(STATUS "TL effective source: ${source}")
    message(STATUS "TL selected patches: ${patches}; signature: ${signature}")
    include("${_D3D9ON12_TL_PATCH_DIRECTORY}/manifest.cmake")
    set(verify_inputs "${_D3D9ON12_TL_PATCH_DIRECTORY}/manifest.cmake")
    foreach(id IN LISTS patches)
        string(REPLACE "-" "_" key "${id}")
        string(TOUPPER "${key}" key)
        list(APPEND verify_inputs "${_D3D9ON12_TL_PATCH_DIRECTORY}/${D3D9ON12_TL_${key}_PATCH}")
    endforeach()
    foreach(entry IN LISTS D3D9ON12_TL_BASE_FILES)
        _d3d9on12_tl_entry("${entry}" path lf crlf)
        list(APPEND verify_inputs "${d3d12translationlayer_SOURCE_DIR}/${path}" "${source}/${path}")
    endforeach()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${verify_inputs})
    set(D3D9ON12_TL_EFFECTIVE_SOURCE "${source}" PARENT_SCOPE)
    set(D3D9ON12_TL_PATCH_SIGNATURE "${signature}" PARENT_SCOPE)
    add_subdirectory("${source}" "${d3d12translationlayer_BINARY_DIR}/${short_signature}")
    if(D3D9ON12_TL_BINDING_TRANSITION_FASTPATH)
        # The patch adds guarded code only; the definition that enables it comes from here so
        # the patch stays disjoint from the other TL patches' files.
        target_compile_definitions(d3d12translationlayer PUBLIC D3D12TL_BINDING_TRANSITION_FASTPATH)
    endif()
endfunction()
