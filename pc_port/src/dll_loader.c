/*
 * dll_loader.c — OS-specific shared library loading.
 *
 * This file ONLY includes OS headers. It must NOT include any game or
 * decomp headers, because <windows.h> conflicts with PSX type definitions
 * (EnterCriticalSection macro, 'byte' typedef, etc.).
 */
#include "dll_loader.h"
#include <stdio.h>
#include <stdint.h>

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
 * PS Vita: no dlopen()-equivalent exists in the stock newlib/vitasdk
 * environment, BUT the kernel's native module loader
 * (sceKernelLoadStartModule) does everything dlopen would need for our
 * purposes: it maps the target .suprx, applies its relocations against the
 * running process (the exact mechanism every taiHEN plugin/.suprx homebrew
 * add-on uses), calls its module_start entrypoint, and hands back a module
 * ID we can later unload with sceKernelStopUnloadModule.
 *
 * The one real gap vs. dlopen/dlsym is symbol resolution direction:
 *   - IMPORTS (map -> exe):  handled by Sony's own module import-binding,
 *     driven by the stub library the map module links against (generated
 *     by vita-elf-export + vita-libs-gen from vita_export/map_api_exports.yml
 *     — see pc_port/CMakeLists.txt's map_stub_lib target). This resolves
 *     purely at link time on our side; sceKernelLoadStartModule itself does
 *     the actual NID-based binding against the running exe at load time.
 *   - EXPORTS (exe -> map), i.e. "give me g_MapOverlayHeader_<name> after
 *     loading": there is no by-name lookup analogous to dlsym for a
 *     just-loaded user module without extra taiHEN NID-lookup machinery.
 *     We sidestep that entirely with a PUSH model instead: each map's
 *     module_start calls MapOverlay_VitaRegister(&g_MapOverlayHeader_<name>)
 *     (imported from the exe via the same stub lib), which
 *     sceKernelLoadStartModule runs synchronously before returning here — so
 *     by the time DllLoader_Open returns, map_overlay_loader.c already has
 *     the pointer waiting for it. See MapOverlay_Load's __vita__ branch.
 *
 * DllHandle is the SceUID module ID (sceKernelLoadStartModule's return
 * value), stuffed into the opaque void* the rest of the codebase already
 * expects.
 */
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>

static char s_vitaDllError[128] = { 0 };

DllHandle DllLoader_Open(const char* path)
{
    SceUID modid;
    int status = 0;

    modid = sceKernelLoadStartModule(path, 0, NULL, 0, NULL, &status);
    if (modid < 0)
    {
        snprintf(s_vitaDllError, sizeof(s_vitaDllError),
                 "sceKernelLoadStartModule error 0x%08X", (unsigned)modid);
        return NULL;
    }
    if (status != 0)
    {
        /* Module loaded and its module_start ran, but returned a nonzero
         * status -- treat as a load failure and unwind, same as the
         * modid < 0 case, rather than handing back a half-good handle. */
        snprintf(s_vitaDllError, sizeof(s_vitaDllError),
                 "module_start returned status 0x%08X", (unsigned)status);
        sceKernelStopUnloadModule(modid, 0, NULL, 0, NULL, NULL);
        return NULL;
    }

    return (DllHandle)(intptr_t)modid;
}

void* DllLoader_GetSymbol(DllHandle handle, const char* name)
{
    /* Not supported on this platform -- see the push-model note above.
     * MapOverlay_Load's __vita__ branch never calls this; any other caller
     * hitting it is a platform-support gap that should be logged. */
    (void)handle;
    (void)name;
    snprintf(s_vitaDllError, sizeof(s_vitaDllError),
             "DllLoader_GetSymbol is not supported on Vita (push-model registration only)");
    return NULL;
}

void DllLoader_Close(DllHandle handle)
{
    SceUID modid = (SceUID)(intptr_t)handle;
    int status = 0;
    if (handle)
        sceKernelStopUnloadModule(modid, 0, NULL, 0, NULL, &status);
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
