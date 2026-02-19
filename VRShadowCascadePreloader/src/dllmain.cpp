#include <Windows.h>
#include "cascade_patch.h"

// Defined in proxy.cpp
bool LoadRealVersionDll();
void CleanupProxy();

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);

        // Load real version.dll immediately (safe: it's a Known DLL, already mapped)
        // This must happen in DllMain to avoid loader lock deadlock when proxy
        // exports are called during other DLLs' initialization.
        LoadRealVersionDll();

        CascadePatch::Initialize();
        break;

    case DLL_PROCESS_DETACH:
        CascadePatch::Shutdown();
        CleanupProxy();
        break;
    }

    return TRUE;
}
