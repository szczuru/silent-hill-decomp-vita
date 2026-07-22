/*
 * map_overlay_loader.c — Dynamic map overlay loading via DLLs
 *
 * Uses dll_loader.h for OS calls so we never mix <windows.h> with
 * PSX decomp headers (they define conflicting types).
 */
#include "map_overlay_loader.h"
#include "map_registry.h"
#include "dll_loader.h"
#include "sh_log.h"
#include <string.h>

#ifdef _WIN32
#define DLL_EXT ".dll"
#elif defined(__APPLE__)
#define DLL_EXT ".dylib"
#elif defined(__vita__)
#define DLL_EXT ".suprx"
#else
#define DLL_EXT ".so"
#endif

#ifdef __vita__
/* Maps are shipped on the memory card alongside the disc image/config, NOT
 * bundled inside the .vpk (see docs/vita_port.md) -- same reasoning as the
 * disc image: they're large, and re-packaging/re-signing the .vpk for every
 * map addition/update would be needlessly slow. Must match
 * main_vita.c's SH_VITA_DATA_DIR. */
#define SH_VITA_MAPS_DIR "ux0:data/SILENTHILLVITA/maps"
#endif

/* Currently loaded overlay */
static DllHandle s_currentDll = NULL;
static char s_currentName[64] = { 0 };

#ifdef __vita__
/* PS Vita push-model registration slot: written by MapOverlay_VitaRegister,
 * which a map module's module_start() calls (imported via the generated
 * stub lib -- see vita_export/map_api_exports.yml and dll_loader.c). Cleared
 * before each sceKernelLoadStartModule so a module that fails to call it
 * (build/config mistake) is detected as a load failure rather than silently
 * reusing the previous map's header. */
static s_MapOverlayHdr* s_vitaPendingHeader = NULL;

void MapOverlay_VitaRegister(s_MapOverlayHdr* header)
{
    s_vitaPendingHeader = header;
}
#endif

/* The compiled-in map0_s00 (always available as fallback) */
extern s_MapOverlayHdr g_MapOverlayHeader_map0_s00;

s_MapOverlayHdr* MapOverlay_Load(e_MapIdx id)
{
    char dllPath[256];
    char symbolName[64];
    const char* mapName;
    s_MapOverlayHdr* header;

    mapName = MapRegistry_GetName(id);
    if (mapName == NULL || strcmp(mapName, "unknown") == 0)
    {
        SH_DBG("[MapOverlay] Unknown overlay ID %d", id);
        return NULL;
    }

    /* map0_s00 is compiled into the main executable — no DLL needed */
    if (id == MapIdx_MAP0_S00)
    {
        MapOverlay_Unload();
        snprintf(s_currentName, sizeof(s_currentName), "%s", mapName);
        SH_DBG("[MapOverlay] Using built-in %s", mapName);
        return &g_MapOverlayHeader_map0_s00;
    }

    /* Build DLL path: maps/<mapname>.dll (ux0:data/SILENTHILLVITA/maps/<mapname>.suprx on Vita) */
#ifdef __vita__
    snprintf(dllPath, sizeof(dllPath), "%s/%s%s", SH_VITA_MAPS_DIR, mapName, DLL_EXT);
#else
    snprintf(dllPath, sizeof(dllPath), "maps/%s%s", mapName, DLL_EXT);
#endif

    /* Build symbol name: g_MapOverlayHeader_<mapname> (unused on Vita --
     * push-model registration, see below) */
    snprintf(symbolName, sizeof(symbolName), "g_MapOverlayHeader_%s", mapName);
    (void)symbolName;

    /* Unload previous overlay */
    MapOverlay_Unload();

#ifdef __vita__
    /* Vita has no dlsym()-equivalent for a loaded .suprx module -- the
     * module pushes its header back to us instead, via module_start calling
     * the imported MapOverlay_VitaRegister (see dll_loader.c). Clear the
     * slot first so a module that doesn't call it is a detectable failure. */
    s_vitaPendingHeader = NULL;
#endif

    /* Load the DLL */
    s_currentDll = DllLoader_Open(dllPath);
    if (!s_currentDll)
    {
        SH_DBG("[MapOverlay] Failed to load %s (%s)", dllPath, DllLoader_GetError());
        return NULL;
    }

#ifdef __vita__
    /* sceKernelLoadStartModule (inside DllLoader_Open) calls module_start
     * synchronously and only returns once it's done, so the push-model
     * registration has already happened by this point -- no GetSymbol call
     * needed/possible on this platform. */
    header = s_vitaPendingHeader;
    if (!header)
    {
        SH_DBG("[MapOverlay] %s loaded but never called MapOverlay_VitaRegister "
                "(build mistake in the module?)", dllPath);
        DllLoader_Close(s_currentDll);
        s_currentDll = NULL;
        return NULL;
    }
#else
    /* Find the header symbol */
    header = (s_MapOverlayHdr*)DllLoader_GetSymbol(s_currentDll, symbolName);
    if (!header)
    {
        SH_DBG("[MapOverlay] Symbol '%s' not found in %s (%s)",
                symbolName, dllPath, DllLoader_GetError());
        DllLoader_Close(s_currentDll);
        s_currentDll = NULL;
        return NULL;
    }
#endif

    /* Sanitize raw PSX addresses in the header. Many map headers have
     * un-decompiled function pointers stored as raw 0x800XXXXX values.
     * These were valid on PSX but are garbage on PC-64bit. NULL them out. */
    {
        uintptr_t* fields = (uintptr_t*)header;
        size_t count = sizeof(s_MapOverlayHdr) / sizeof(uintptr_t);
        size_t nulled = 0;
        for (size_t i = 0; i < count; i++)
        {
            uintptr_t val = fields[i];
            /* Detect PSX address: fits in 32 bits and starts with 0x80 */
            if (val != 0 && val <= 0xFFFFFFFF && (val & 0xFF000000) == 0x80000000)
            {
                SH_DBG("[MapOverlay]   Nulling PSX addr 0x%08X at offset 0x%zX",
                        (unsigned)val, i * sizeof(uintptr_t));
                fields[i] = 0;
                nulled++;
            }
        }
        if (nulled > 0)
            SH_DBG("[MapOverlay]   Nulled %zu raw PSX addresses", nulled);
    }

    snprintf(s_currentName, sizeof(s_currentName), "%s", mapName);
#ifdef __vita__
    SH_DBG("[MapOverlay] Loaded %s from %s (push-registered)", mapName, dllPath);
#else
    SH_DBG("[MapOverlay] Loaded %s from %s", symbolName, dllPath);
#endif
    return header;
}

void MapOverlay_Unload(void)
{
    if (s_currentDll)
    {
        SH_DBG("[MapOverlay] Unloading %s", s_currentName);
        DllLoader_Close(s_currentDll);
        s_currentDll = NULL;
    }
    s_currentName[0] = '\0';
}

const char* MapOverlay_GetLoadedName(void)
{
    return s_currentName[0] ? s_currentName : NULL;
}
