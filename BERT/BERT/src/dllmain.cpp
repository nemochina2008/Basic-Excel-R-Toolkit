// dllmain.cpp : Defines the entry point for the DLL application.
#include "stdafx.h"

HMODULE global_module_handle;

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    global_module_handle = hModule;
    return TRUE;
}

