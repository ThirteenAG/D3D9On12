# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.
cmake_minimum_required(VERSION 3.14)
include(FetchContent)

file(GLOB SRC CONFIGURE_DEPENDS *.cpp)
file(GLOB INC ../include/*.h ../include/*.hpp ../interface/*.h *.h *.hpp)
file(GLOB INL ../include/*.inl *.inl)

file(GLOB_RECURSE EXTERNAL_INC ../external/*.h ../external/*.hpp)

add_library(d3d9on12 SHARED ${SRC} ${INC} ${INL} ${EXTERNAL_INC} d3d9on12.rc d3d9on12.def)

# Record rendering packets asynchronously when the runtime permits threading.
option(D3D9ON12_BATCHED_RENDERING "Record rendering through the translation-layer worker" ON)
option(D3D9ON12_BATCHED_CLEAR_COPY "Queue clears, copies and resolves" ON)
option(D3D9ON12_FLOAT_CONSTANT_ARENA_8MIB "Use 8-MiB float constant upload arenas" ON)
foreach(batch_option D3D9ON12_BATCHED_RENDERING D3D9ON12_BATCHED_CLEAR_COPY D3D9ON12_FLOAT_CONSTANT_ARENA_8MIB)
    if(${batch_option})
        if(NOT D3D9ON12_BATCHED_RENDERING AND NOT batch_option STREQUAL "D3D9ON12_BATCHED_RENDERING")
            message(FATAL_ERROR "${batch_option} requires D3D9ON12_BATCHED_RENDERING")
        endif()
        target_compile_definitions(d3d9on12 PRIVATE ${batch_option})
    endif()
endforeach()

if (CMAKE_VERSION VERSION_GREATER 3.16)
	target_precompile_headers(d3d9on12 PRIVATE ../include/pch.h)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/D3D12TranslationLayer.cmake")
d3d9on12_add_verified_translation_layer()

set(D3D9ON12_BUILD_REVISION "unknown" CACHE STRING "Source revision recorded in release metadata")
if(NOT D3D9ON12_BUILD_REVISION MATCHES "^[A-Za-z0-9._-]+$")
    message(FATAL_ERROR "Invalid D3D9ON12_BUILD_REVISION")
endif()
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(D3D9ON12_BUILD_ARCHITECTURE "Win32")
else()
    set(D3D9ON12_BUILD_ARCHITECTURE "x64")
endif()
configure_file(../cmake/BuildInfo.json.in "${CMAKE_BINARY_DIR}/D3D9On12-build.json" @ONLY)
if(D3D9ON12_TL_EFFECTIVE_SOURCE AND TARGET DirectX-Headers)
    get_target_property(d3d9on12_headers_source DirectX-Headers SOURCE_DIR)
    configure_file("${D3D9ON12_TL_EFFECTIVE_SOURCE}/LICENSE"
        "${CMAKE_BINARY_DIR}/licenses/D3D12TranslationLayer-LICENSE.txt" COPYONLY)
    configure_file("${d3d9on12_headers_source}/LICENSE"
        "${CMAKE_BINARY_DIR}/licenses/DirectX-Headers-LICENSE.txt" COPYONLY)
endif()

target_link_libraries(d3d9on12 d3d12translationlayer_wdk)
target_link_libraries(d3d9on12 d3d9on12_shaderconv)

if(TARGET dxbcsigner_static)
    target_link_libraries(d3d9on12 dxbcsigner_static)
else()
    message(STATUS "Adding DxbcSigner NuGet package...")
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/../packages.config ${CMAKE_CURRENT_BINARY_DIR}/packages.config COPYONLY)
    add_library(dxbcsigner INTERFACE IMPORTED GLOBAL)
    set_target_properties(dxbcsigner PROPERTIES INTERFACE_LINK_LIBRARIES "${CMAKE_BINARY_DIR}/packages/Microsoft.Direct3D.DxbcSigner.1.0.0/build/native/Microsoft.Direct3D.DxbcSigner.targets")
    set_target_properties(d3d9on12 PROPERTIES LINK_FLAGS "/DELAYLOAD:dxbcSigner.dll")
    target_link_libraries(d3d9on12 dxbcsigner)
endif()
target_include_directories(d3d9on12
    PRIVATE ../external
    PRIVATE ../include
    PUBLIC ../interface
    PRIVATE ./)

source_group(Inlines FILES ${INL})
source_group("Header Files\\External" FILES ${EXTERNAL_INC})

# VS linker read tracking can omit project libraries after a full LTCG link.
# Give each static dependency an explicit file-level edge to a tracked object.
if(CMAKE_GENERATOR MATCHES "^Visual Studio")
    set(d3d9on12_relink_dependencies)
    foreach(dependency IN ITEMS d3d12translationlayer d3d12translationlayer_wdk
            d3d9on12_shaderconv dxbcparser DirectX-Headers dxbcsigner_static)
        if(TARGET ${dependency})
            get_target_property(dependency_type ${dependency} TYPE)
            get_target_property(dependency_imported ${dependency} IMPORTED)
            if(dependency_type STREQUAL "STATIC_LIBRARY" AND NOT dependency_imported)
                list(APPEND d3d9on12_relink_dependencies ${dependency})
            endif()
        endif()
    endforeach()
    set(d3d9on12_relink_source "${CMAKE_CURRENT_BINARY_DIR}/StaticDependenciesRelink.cpp")
    add_custom_command(OUTPUT "${d3d9on12_relink_source}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${d3d9on12_relink_source}"
        DEPENDS ${d3d9on12_relink_dependencies}
        COMMENT "Refresh static-library link dependencies"
        VERBATIM)
    # Keep this empty translation unit free of the driver's PCH and its globals.
    set_source_files_properties("${d3d9on12_relink_source}" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_sources(d3d9on12 PRIVATE "${d3d9on12_relink_source}")
endif()
