#define WIN32_LEAN_AND_MEAN
#define NOGDI
#include <Windows.h>
#include "cascade_patch.h"

// Handle to the real version.dll
static HMODULE g_realVersion = nullptr;

// Function pointer types matching version.dll exports
typedef BOOL(WINAPI* GetFileVersionInfoA_t)(LPCSTR, DWORD, DWORD, LPVOID);
typedef int(WINAPI* GetFileVersionInfoByHandle_t)(int, LPCWSTR, int*, int*);
typedef BOOL(WINAPI* GetFileVersionInfoExA_t)(DWORD, LPCSTR, DWORD, DWORD, LPVOID);
typedef BOOL(WINAPI* GetFileVersionInfoExW_t)(DWORD, LPCWSTR, DWORD, DWORD, LPVOID);
typedef DWORD(WINAPI* GetFileVersionInfoSizeA_t)(LPCSTR, LPDWORD);
typedef DWORD(WINAPI* GetFileVersionInfoSizeExA_t)(DWORD, LPCSTR, LPDWORD);
typedef DWORD(WINAPI* GetFileVersionInfoSizeExW_t)(DWORD, LPCWSTR, LPDWORD);
typedef DWORD(WINAPI* GetFileVersionInfoSizeW_t)(LPCWSTR, LPDWORD);
typedef BOOL(WINAPI* GetFileVersionInfoW_t)(LPCWSTR, DWORD, DWORD, LPVOID);
typedef DWORD(WINAPI* VerFindFileA_t)(DWORD, LPCSTR, LPCSTR, LPCSTR, LPSTR, PUINT, LPSTR, PUINT);
typedef DWORD(WINAPI* VerFindFileW_t)(DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, PUINT, LPWSTR, PUINT);
typedef DWORD(WINAPI* VerInstallFileA_t)(DWORD, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPSTR, PUINT);
typedef DWORD(WINAPI* VerInstallFileW_t)(DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, PUINT);
typedef DWORD(WINAPI* VerLanguageNameA_t)(DWORD, LPSTR, DWORD);
typedef DWORD(WINAPI* VerLanguageNameW_t)(DWORD, LPWSTR, DWORD);
typedef BOOL(WINAPI* VerQueryValueA_t)(LPCVOID, LPCSTR, LPVOID*, PUINT);
typedef BOOL(WINAPI* VerQueryValueW_t)(LPCVOID, LPCWSTR, LPVOID*, PUINT);

// Function pointers for version.dll exports
static GetFileVersionInfoA_t g_GetFileVersionInfoA = nullptr;
static GetFileVersionInfoByHandle_t g_GetFileVersionInfoByHandle = nullptr;
static GetFileVersionInfoExA_t g_GetFileVersionInfoExA = nullptr;
static GetFileVersionInfoExW_t g_GetFileVersionInfoExW = nullptr;
static GetFileVersionInfoSizeA_t g_GetFileVersionInfoSizeA = nullptr;
static GetFileVersionInfoSizeExA_t g_GetFileVersionInfoSizeExA = nullptr;
static GetFileVersionInfoSizeExW_t g_GetFileVersionInfoSizeExW = nullptr;
static GetFileVersionInfoSizeW_t g_GetFileVersionInfoSizeW = nullptr;
static GetFileVersionInfoW_t g_GetFileVersionInfoW = nullptr;
static VerFindFileA_t g_VerFindFileA = nullptr;
static VerFindFileW_t g_VerFindFileW = nullptr;
static VerInstallFileA_t g_VerInstallFileA = nullptr;
static VerInstallFileW_t g_VerInstallFileW = nullptr;
static VerLanguageNameA_t g_VerLanguageNameA = nullptr;
static VerLanguageNameW_t g_VerLanguageNameW = nullptr;
static VerQueryValueA_t g_VerQueryValueA = nullptr;
static VerQueryValueW_t g_VerQueryValueW = nullptr;

// Called from DllMain - loads real version.dll and resolves all function pointers.
// Safe in DllMain because version.dll is a Known DLL (already mapped by the loader).
bool LoadRealVersionDll()
{
    char systemPath[MAX_PATH];
    GetSystemDirectoryA(systemPath, MAX_PATH);
    strcat_s(systemPath, "\\version.dll");

    g_realVersion = LoadLibraryA(systemPath);
    if (!g_realVersion) {
        return false;
    }

    g_GetFileVersionInfoA = (GetFileVersionInfoA_t)GetProcAddress(g_realVersion, "GetFileVersionInfoA");
    g_GetFileVersionInfoByHandle = (GetFileVersionInfoByHandle_t)GetProcAddress(g_realVersion, "GetFileVersionInfoByHandle");
    g_GetFileVersionInfoExA = (GetFileVersionInfoExA_t)GetProcAddress(g_realVersion, "GetFileVersionInfoExA");
    g_GetFileVersionInfoExW = (GetFileVersionInfoExW_t)GetProcAddress(g_realVersion, "GetFileVersionInfoExW");
    g_GetFileVersionInfoSizeA = (GetFileVersionInfoSizeA_t)GetProcAddress(g_realVersion, "GetFileVersionInfoSizeA");
    g_GetFileVersionInfoSizeExA = (GetFileVersionInfoSizeExA_t)GetProcAddress(g_realVersion, "GetFileVersionInfoSizeExA");
    g_GetFileVersionInfoSizeExW = (GetFileVersionInfoSizeExW_t)GetProcAddress(g_realVersion, "GetFileVersionInfoSizeExW");
    g_GetFileVersionInfoSizeW = (GetFileVersionInfoSizeW_t)GetProcAddress(g_realVersion, "GetFileVersionInfoSizeW");
    g_GetFileVersionInfoW = (GetFileVersionInfoW_t)GetProcAddress(g_realVersion, "GetFileVersionInfoW");
    g_VerFindFileA = (VerFindFileA_t)GetProcAddress(g_realVersion, "VerFindFileA");
    g_VerFindFileW = (VerFindFileW_t)GetProcAddress(g_realVersion, "VerFindFileW");
    g_VerInstallFileA = (VerInstallFileA_t)GetProcAddress(g_realVersion, "VerInstallFileA");
    g_VerInstallFileW = (VerInstallFileW_t)GetProcAddress(g_realVersion, "VerInstallFileW");
    g_VerLanguageNameA = (VerLanguageNameA_t)GetProcAddress(g_realVersion, "VerLanguageNameA");
    g_VerLanguageNameW = (VerLanguageNameW_t)GetProcAddress(g_realVersion, "VerLanguageNameW");
    g_VerQueryValueA = (VerQueryValueA_t)GetProcAddress(g_realVersion, "VerQueryValueA");
    g_VerQueryValueW = (VerQueryValueW_t)GetProcAddress(g_realVersion, "VerQueryValueW");

    return true;
}

// Exported functions - forward to real version.dll.
// Each export calls EnsureInitialized() to trigger one-time deferred setup
// (log file + worker thread) on first call.

extern "C" {

BOOL WINAPI GetFileVersionInfoA(LPCSTR lptstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData)
{
    CascadePatch::EnsureInitialized();
    if (g_GetFileVersionInfoA)
        return g_GetFileVersionInfoA(lptstrFilename, dwHandle, dwLen, lpData);
    return FALSE;
}

int WINAPI GetFileVersionInfoByHandle(int hMem, LPCWSTR lpFileName, int* lpnHandle, int* lpdwLen)
{
    CascadePatch::EnsureInitialized();
    if (g_GetFileVersionInfoByHandle)
        return g_GetFileVersionInfoByHandle(hMem, lpFileName, lpnHandle, lpdwLen);
    return 0;
}

BOOL WINAPI GetFileVersionInfoExA(DWORD dwFlags, LPCSTR lpwstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData)
{
    CascadePatch::EnsureInitialized();
    if (g_GetFileVersionInfoExA)
        return g_GetFileVersionInfoExA(dwFlags, lpwstrFilename, dwHandle, dwLen, lpData);
    return FALSE;
}

BOOL WINAPI GetFileVersionInfoExW(DWORD dwFlags, LPCWSTR lpwstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData)
{
    CascadePatch::EnsureInitialized();
    if (g_GetFileVersionInfoExW)
        return g_GetFileVersionInfoExW(dwFlags, lpwstrFilename, dwHandle, dwLen, lpData);
    return FALSE;
}

DWORD WINAPI GetFileVersionInfoSizeA(LPCSTR lptstrFilename, LPDWORD lpdwHandle)
{
    CascadePatch::EnsureInitialized();
    if (g_GetFileVersionInfoSizeA)
        return g_GetFileVersionInfoSizeA(lptstrFilename, lpdwHandle);
    return 0;
}

DWORD WINAPI GetFileVersionInfoSizeExA(DWORD dwFlags, LPCSTR lpwstrFilename, LPDWORD lpdwHandle)
{
    CascadePatch::EnsureInitialized();
    if (g_GetFileVersionInfoSizeExA)
        return g_GetFileVersionInfoSizeExA(dwFlags, lpwstrFilename, lpdwHandle);
    return 0;
}

DWORD WINAPI GetFileVersionInfoSizeExW(DWORD dwFlags, LPCWSTR lpwstrFilename, LPDWORD lpdwHandle)
{
    CascadePatch::EnsureInitialized();
    if (g_GetFileVersionInfoSizeExW)
        return g_GetFileVersionInfoSizeExW(dwFlags, lpwstrFilename, lpdwHandle);
    return 0;
}

DWORD WINAPI GetFileVersionInfoSizeW(LPCWSTR lptstrFilename, LPDWORD lpdwHandle)
{
    CascadePatch::EnsureInitialized();
    if (g_GetFileVersionInfoSizeW)
        return g_GetFileVersionInfoSizeW(lptstrFilename, lpdwHandle);
    return 0;
}

BOOL WINAPI GetFileVersionInfoW(LPCWSTR lptstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData)
{
    CascadePatch::EnsureInitialized();
    if (g_GetFileVersionInfoW)
        return g_GetFileVersionInfoW(lptstrFilename, dwHandle, dwLen, lpData);
    return FALSE;
}

DWORD WINAPI VerFindFileA(DWORD uFlags, LPCSTR szFileName, LPCSTR szWinDir, LPCSTR szAppDir, LPSTR szCurDir, PUINT lpuCurDirLen, LPSTR szDestDir, PUINT lpuDestDirLen)
{
    CascadePatch::EnsureInitialized();
    if (g_VerFindFileA)
        return g_VerFindFileA(uFlags, szFileName, szWinDir, szAppDir, szCurDir, lpuCurDirLen, szDestDir, lpuDestDirLen);
    return 0;
}

DWORD WINAPI VerFindFileW(DWORD uFlags, LPCWSTR szFileName, LPCWSTR szWinDir, LPCWSTR szAppDir, LPWSTR szCurDir, PUINT lpuCurDirLen, LPWSTR szDestDir, PUINT lpuDestDirLen)
{
    CascadePatch::EnsureInitialized();
    if (g_VerFindFileW)
        return g_VerFindFileW(uFlags, szFileName, szWinDir, szAppDir, szCurDir, lpuCurDirLen, szDestDir, lpuDestDirLen);
    return 0;
}

DWORD WINAPI VerInstallFileA(DWORD uFlags, LPCSTR szSrcFileName, LPCSTR szDestFileName, LPCSTR szSrcDir, LPCSTR szDestDir, LPCSTR szCurDir, LPSTR szTmpFile, PUINT lpuTmpFileLen)
{
    CascadePatch::EnsureInitialized();
    if (g_VerInstallFileA)
        return g_VerInstallFileA(uFlags, szSrcFileName, szDestFileName, szSrcDir, szDestDir, szCurDir, szTmpFile, lpuTmpFileLen);
    return 0;
}

DWORD WINAPI VerInstallFileW(DWORD uFlags, LPCWSTR szSrcFileName, LPCWSTR szDestFileName, LPCWSTR szSrcDir, LPCWSTR szDestDir, LPCWSTR szCurDir, LPWSTR szTmpFile, PUINT lpuTmpFileLen)
{
    CascadePatch::EnsureInitialized();
    if (g_VerInstallFileW)
        return g_VerInstallFileW(uFlags, szSrcFileName, szDestFileName, szSrcDir, szDestDir, szCurDir, szTmpFile, lpuTmpFileLen);
    return 0;
}

DWORD WINAPI VerLanguageNameA(DWORD wLang, LPSTR szLang, DWORD cchLang)
{
    CascadePatch::EnsureInitialized();
    if (g_VerLanguageNameA)
        return g_VerLanguageNameA(wLang, szLang, cchLang);
    return 0;
}

DWORD WINAPI VerLanguageNameW(DWORD wLang, LPWSTR szLang, DWORD cchLang)
{
    CascadePatch::EnsureInitialized();
    if (g_VerLanguageNameW)
        return g_VerLanguageNameW(wLang, szLang, cchLang);
    return 0;
}

BOOL WINAPI VerQueryValueA(LPCVOID pBlock, LPCSTR lpSubBlock, LPVOID* lplpBuffer, PUINT puLen)
{
    CascadePatch::EnsureInitialized();
    if (g_VerQueryValueA)
        return g_VerQueryValueA(pBlock, lpSubBlock, lplpBuffer, puLen);
    return FALSE;
}

BOOL WINAPI VerQueryValueW(LPCVOID pBlock, LPCWSTR lpSubBlock, LPVOID* lplpBuffer, PUINT puLen)
{
    CascadePatch::EnsureInitialized();
    if (g_VerQueryValueW)
        return g_VerQueryValueW(pBlock, lpSubBlock, lplpBuffer, puLen);
    return FALSE;
}

} // extern "C"

// Cleanup when DLL unloads
void CleanupProxy()
{
    if (g_realVersion) {
        FreeLibrary(g_realVersion);
        g_realVersion = nullptr;
    }
}
