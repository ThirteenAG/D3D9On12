// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID lpReserved)
{
    // lpReserved is non-null for process termination (ExitProcess), when all other threads have already been killed.
    if (reason == DLL_PROCESS_DETACH && lpReserved != nullptr)
    {
        D3D9on12::Device::NoteProcessExit();
    }
    return TRUE;
}

extern "C" PFND3DDDI_OPENADAPTER Get9on12OpenAdapter()
{
    return D3D9on12::OpenAdapter;
}

const IID GUID_NULL = {};
