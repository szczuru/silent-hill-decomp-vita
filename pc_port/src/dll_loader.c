/*
 * dll_loader.c — OS-specific shared library loading.
 *
 * This file ONLY includes OS headers. It must NOT include any game or
 * decomp headers, because <windows.h> conflicts with PSX type definitions
 * (EnterCriticalSection macro, 'byte' typedef, etc.).
 */
#include "dll_loader.h"
#include <stdio.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static char s_errorBuf[256] = {0};

DllHandle DllLoader_Open(const char* path)
{
    HMODULE h = LoadLibraryA(path);
    if (!h)
    {
        snprintf(s_errorBuf, sizeof(s_errorBuf), "LoadLibrary error %lu", GetLastError());
    }
    return (DllHandle)h;
}

void* DllLoader_GetSymbol(DllHandle handle, const char* name)
{
    FARPROC p = GetProcAddress((HMODULE)handle, name);
    if (!p)
    {
        snprintf(s_errorBuf, sizeof(s_errorBuf), "GetProcAddress error %lu", GetLastError());
    }
    return (void*)p;
}

void DllLoader_Close(DllHandle handle)
{
    if (handle)
        FreeLibrary((HMODULE)handle);
}

const char* DllLoader_GetError(void)
{
    return s_errorBuf;
}

#elif defined(__vita__)
/*
 * PS Vita: no dlopen()-equivalent for loading arbitrary native code at
 * runtime in the stock newlib/vitasdk environment (unlike Linux/macOS/
 * Windows). This first port milestone follows the same path the default
 * desktop PC build already uses: only map0_s00 is compiled directly into
 * the executable (SH_BUILD_MAP_DLLS is OFF by default there too — see
 * pc_port/CMakeLists.txt) and MapOverlay_Load already handles a load
 * failure for any other map by logging and returning NULL.
 *
 * A follow-up port milestone can revisit this to dynamically load the
 * other 42 map overlays via kubridge (which allows mapping+relocating a
 * SELF/velf at runtime with kernel help) instead of statically linking all
 * of them into one executable (which hits the same 500+ symbol collisions
 * the desktop build's comment above describes).
 */
static char s_vitaDllError[128] = "dynamic module loading is not supported on this platform";

DllHandle DllLoader_Open(const char* path)
{
    (void)path;
    return NULL;
}

void* DllLoader_GetSymbol(DllHandle handle, const char* name)
{
    (void)handle;
    (void)name;
    return NULL;
}

void DllLoader_Close(DllHandle handle)
{
    (void)handle;
}

const char* DllLoader_GetError(void)
{
    return s_vitaDllError;
}

#else /* POSIX */
#include <dlfcn.h>

DllHandle DllLoader_Open(const char* path)
{
    return dlopen(path, RTLD_NOW);
}

void* DllLoader_GetSymbol(DllHandle handle, const char* name)
{
    return dlsym(handle, name);
}

void DllLoader_Close(DllHandle handle)
{
    if (handle)
        dlclose(handle);
}

const char* DllLoader_GetError(void)
{
    const char* e = dlerror();
    return e ? e : "";
}

#endif
