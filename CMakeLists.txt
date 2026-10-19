# Copyright (c) Microsoft Corporation.
# Licensed under the MIT license.
cmake_minimum_required(VERSION 3.13)
project(d3d9on12)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Reject stale development configurations rather than silently producing a
# different binary after these experimental features were removed.
foreach(retired D3D9ON12_PROFILE D3D9ON12_PRIVATE_CONSTANTS D3D9ON12_PRIVATE_PIPELINE D3D9ON12_BUILD_FRONTEND D3D9ON12_INPUT_AUDIT D3D9ON12_TL_DEV_SOURCE)
    if(${retired})
        message(FATAL_ERROR "${retired} is no longer supported; configure a production build.")
    endif()
endforeach()

# A portable WDK header package can be used without installing the WDK.
set(D3D9ON12_WDK_ROOT "" CACHE PATH "WDK Include/<version> directory containing um and shared")
if(D3D9ON12_WDK_ROOT)
    foreach(header d3d10umddi.h d3d12TokenizedProgramFormat.hpp)
        if(NOT EXISTS "${D3D9ON12_WDK_ROOT}/um/${header}")
            message(FATAL_ERROR "Missing WDK header: ${D3D9ON12_WDK_ROOT}/um/${header}")
        endif()
    endforeach()
    include_directories(SYSTEM "${D3D9ON12_WDK_ROOT}/um" "${D3D9ON12_WDK_ROOT}/shared")
    list(APPEND CMAKE_REQUIRED_INCLUDES "${D3D9ON12_WDK_ROOT}/um" "${D3D9ON12_WDK_ROOT}/shared")
endif()

# The pinned PIX runtime package does not contain an x86 runtime.
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    option(USE_PIX "Enable PIX markers (requires a matching PIX runtime)" OFF)
endif()

option(D3D9ON12_BUILD_TESTS "Build the development runtime probes" OFF)

add_subdirectory(ShaderConverter)
add_subdirectory(src)

if(D3D9ON12_BUILD_TESTS)
    add_subdirectory(tests)
endif()
