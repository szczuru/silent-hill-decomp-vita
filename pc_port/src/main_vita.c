/*
 * main_vita.c - Silent Hill PS Vita Port entry point
 *
 * This is the Vita counterpart of main_pc.c: it initializes PsyCross
 * (SDL2 for window/pad plumbing + vitaGL for rendering, see
 * PsyCross/src/render/PsyX_render.cpp), points the game at its data
 * directory on the memory card, and calls into the same MainLoop() that
 * runs on every other port (PSX overlays are statically linked in here,
 * same as the desktop PC build).
 *
 * What's intentionally NOT ported yet (first milestone, see
 * docs/vita_port.md):
 *   - Only map0_s00 is compiled in (same limitation the default desktop
 *     CMake build has -- SH_BUILD_MAP_DLLS is off there too). The other 42
 *     maps need a native module-loading story (kubridge) instead of
 *     dlopen(), which is future work -- see dll_loader.c.
 *   - ffmpeg FMV fallback (desktop-only optional feature, headers-only /
 *     runtime-loaded there -- meaningless on a closed console).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <time.h>

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/sysmodule.h>
#include <psp2/appmgr.h>
#include <psp2/io/stat.h>

#include "common.h"
#include "game.h"
#include "gpu.h"
#include "sh_log.h"
#include "psx_memory.h"
#include "pc_config.h"
#include "map_registry.h"
#include "main/fsqueue.h"
#include "main/fileinfo.h"
#include "bodyprog/bodyprog.h"
#include "maps/shared/SysWork_StateStepIncrementAfterTime.h"

#include <libgpu.h>
#include <libgte.h>
#include <libetc.h>
#include <libspu.h>
#include <libcd.h>

#include <PsyX/PsyX_public.h>
#include <PsyX/PsyX_render.h> /* pulls in <vitaGL.h> -- glGetString/GL_RENDERER/GL_VERSION below */

/* PS Vita: bump the default newlib heap and main-thread stack. Read by the
 * vitasdk crt0/libc startup code (documented convention: any global with
 * exactly this name/type overrides the SCE_KERNEL_MODULE_INFO defaults).
 * Silent Hill decompresses/keeps large chunk textures + audio buffers
 * resident (see pc_config.c residentTextures/globalCharaPool), so the
 * stock ~64MB newlib heap general-homebrew default is not enough. 200MB
 * leaves headroom under the Vita's ~500MB usable budget for vitaGL's own
 * GPU-side pools (allocated separately via vglInitExtended, see
 * PsyX_render.cpp) and the ~26MB emulated PSX RAM window (psx_memory.c). */
unsigned int sceUserMainThreadStackSize = 1 * 1024 * 1024;
unsigned int sceLibcHeapSize = 200 * 1024 * 1024;

/* Forward declarations from game code */
extern void MainLoop(void);
extern void Fs_QueueInitialize(void);
extern void PcPort_InitCharaAnimInfo(void);

/* Overlay pointers from main.c - need runtime init on Vita (mirrors main_pc.c) */
extern void* g_OvlDynamic;
extern void* g_OvlBodyprog;

int g_PcAllowDebugControls = 0;
int g_PcUnlimitedEnemies = 0;

/* Demo play file buffer pointer - default PSX address needs runtime init */
typedef struct s_DemoFrameData s_DemoFrameData;
extern s_DemoFrameData* g_Demo_PlayFileBufferPtr;

FILE* g_ShDebugLog = NULL;
int   g_ShDebugEchoStdout = 0;
void (*g_ShOverlayPushLine)(const char* line) = NULL;
void (*g_ShOverlayToastLine)(const char* line) = NULL;

/* PS Vita save/log data directory. ux0:data/<TITLEID>/ is the conventional
 * homebrew location for a title's writable files (config, logs, saves) --
 * separate from app0: (the read-only VPK/eboot content mounted at launch).
 * Must match sce_sys/param.sfo's TITLE_ID (see cmake/vita_titleid.cmake). */
#define SH_VITA_DATA_DIR "ux0:data/SILENTHILLVITA"

static char s_logPath[128] = { 0 };
const char* SH_LogPath(void)
{
    if (!s_logPath[0])
        snprintf(s_logPath, sizeof(s_logPath), "%s/SilentHill.log", SH_VITA_DATA_DIR);
    return s_logPath;
}

void SH_DebugLogInit(void)
{
    if (!g_ShDebugLog)
    {
        sceIoMkdir(SH_VITA_DATA_DIR, 0777); /* no-op if it already exists */
        g_ShDebugLog = fopen(SH_LogPath(), "w");
        if (!g_ShDebugLog)
            g_ShDebugLog = stdout;
        else
        {
            static char s_logBuf[64 * 1024];
            setvbuf(g_ShDebugLog, s_logBuf, _IOFBF, sizeof(s_logBuf));
        }
    }
}

static void Sh_LogAtExitFlush(void)
{
    if (g_ShDebugLog && g_ShDebugLog != stdout)
        fflush(g_ShDebugLog);
}

void Sh_LogPeriodicFlush(void)
{
    static time_t s_lastFlush = 0;
    time_t now;
    if (!g_ShDebugLog || g_ShDebugLog == stdout)
        return;
    now = time(NULL);
    if (now == s_lastFlush)
        return;
    s_lastFlush = now;
    fflush(g_ShDebugLog);
}

/* Game data path - disc image + loose files live here on the memory card. */
static char g_GameDataPath[512] = SH_VITA_DATA_DIR "/gamedata";

const char* PcPort_GetGameDataPath(void)
{
    return g_GameDataPath;
}

unsigned int PcPort_FileTableStartSector(int fileIdx)
{
    return g_FileTable[fileIdx].startSector;
}

/* Resolved disc image path + region (mirrors main_pc.c's disc auto-detect,
 * minus the fan-translation re-remap path -- Vita users are expected to
 * provide a stock disc image; that shortcut can be ported later if there's
 * demand). */
static char g_GameDiscPath[600] = { 0 };
static int  g_DiscResolved      = 0;

static int Pc_DetectRegionFromBin(const char* path)
{
    FILE*         f = fopen(path, "rb");
    unsigned char sec[2048];
    unsigned int  rlba;
    unsigned      o;
    int           region = -1;

    if (!f)
        return -1;

    fseek(f, 16 * 2352 + 24, SEEK_SET);
    if (fread(sec, 1, 2048, f) != 2048) { fclose(f); return -1; }
    rlba = sec[156 + 2] | (sec[156 + 3] << 8) | (sec[156 + 4] << 16) | ((unsigned)sec[156 + 5] << 24);

    fseek(f, (long)rlba * 2352 + 24, SEEK_SET);
    if (fread(sec, 1, 2048, f) != 2048) { fclose(f); return -1; }

    for (o = 0; o + 33 < 2048; )
    {
        unsigned L  = sec[o];
        unsigned nl = sec[o + 32];
        if (L == 0)
            break;
        if (o + 33 + nl <= 2048 && nl >= 4)
        {
            if (memcmp(&sec[o + 33], "SLUS", 4) == 0) region = Region_USA;
            else if (memcmp(&sec[o + 33], "SLES", 4) == 0) region = Region_EUR;
            else if (memcmp(&sec[o + 33], "SLPM", 4) == 0 ||
                     memcmp(&sec[o + 33], "SLPS", 4) == 0 ||
                     memcmp(&sec[o + 33], "SIPS", 4) == 0)
                region = Region_JPN;
        }
        o += L;
    }

    fclose(f);
    return region;
}

static void Pc_ApplyDiscRegion(const char* discPath, e_GameRegion region)
{
    Fs_InitFileTableForRegion(region);
    SH_DBG("[REGION] applied region=%d (%s) disc=%s", (int)region,
           region == Region_EUR ? "EUR/PAL" : region == Region_JPN ? "NTSC-J" : "USA",
           (discPath && discPath[0]) ? discPath : "(none)");
}

/* Locate the disc image on ux0:data/SILENTHILLVITA/gamedata/. Priority:
 * USA, then PAL, then the long European name, then autodetect any other
 * .bin by its ISO boot serial -- same rule set as PcPort_GetGameDiscPath
 * in main_pc.c. */
const char* PcPort_GetGameDiscPath(void)
{
    static const struct { const char* name; int region; } s_known[] = {
        { "Silent Hill (USA).bin",                     Region_USA },
        { "Silent Hill (PAL).bin",                     Region_EUR },
        { "Silent Hill (Europe) (En,Fr,De,Es,It).bin", Region_EUR },
        { "Silent Hill (Japan).bin",                   Region_JPN },
    };
    char path[600];
    int  i;
    DIR* dir;

    if (g_DiscResolved)
        return g_GameDiscPath;
    g_DiscResolved = 1;

    for (i = 0; i < (int)(sizeof(s_known) / sizeof(s_known[0])); i++)
    {
        FILE* f;
        snprintf(path, sizeof(path), "%s/%s", g_GameDataPath, s_known[i].name);
        f = fopen(path, "rb");
        if (f)
        {
            int probed = Pc_DetectRegionFromBin(path);
            fclose(f);
            if (probed < 0)
                probed = s_known[i].region;
            snprintf(g_GameDiscPath, sizeof(g_GameDiscPath), "%s", path);
            Pc_ApplyDiscRegion(g_GameDiscPath, (e_GameRegion)probed);
            SH_LOG("Disc: %s (region %s)", s_known[i].name,
                   probed == Region_EUR ? "EUR/PAL" : probed == Region_JPN ? "NTSC-J" : "USA");
            return g_GameDiscPath;
        }
    }

    /* Autodetect any other .bin by its ISO boot serial. */
    dir = opendir(g_GameDataPath);
    if (dir)
    {
        struct dirent* ent;
        while ((ent = readdir(dir)) != NULL)
        {
            const char* nm = ent->d_name;
            size_t      l  = strlen(nm);
            if (l > 4 && (strcmp(nm + l - 4, ".bin") == 0 || strcmp(nm + l - 4, ".BIN") == 0))
            {
                int r;
                snprintf(path, sizeof(path), "%s/%s", g_GameDataPath, nm);
                r = Pc_DetectRegionFromBin(path);
                if (r >= Region_USA && r <= Region_JPN)
                {
                    snprintf(g_GameDiscPath, sizeof(g_GameDiscPath), "%s", path);
                    Pc_ApplyDiscRegion(g_GameDiscPath, (e_GameRegion)r);
                    SH_LOG("Disc autodetected: %s", g_GameDiscPath);
                    closedir(dir);
                    return g_GameDiscPath;
                }
            }
        }
        closedir(dir);
    }

    Fs_InitFileTableForRegion(Region_USA);
    g_GameDiscPath[0] = '\0';
    SH_WARN("No Silent Hill disc image (.bin) found in %s", g_GameDataPath);
    return g_GameDiscPath;
}

static void PrintBanner(void)
{
    printf("==============================================\n");
    printf("  Silent Hill - PS Vita Port\n");
    printf("  Based on the Silent Hill Decompilation PC Port\n");
    printf("==============================================\n\n");
}

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

    atexit(Sh_LogAtExitFlush);

    PrintBanner();

    sceIoMkdir(SH_VITA_DATA_DIR, 0777);
    sceIoMkdir(g_GameDataPath, 0777);

    /* config.cfg lives next to the save data, not next to the (read-only)
     * eboot -- same directory the log file and gamedata/ live in, so a
     * user only has to manage one folder on the memory card. */
    {
        char cfgPath[128];
        snprintf(cfgPath, sizeof(cfgPath), "%s/config.cfg", SH_VITA_DATA_DIR);
        PcConfig_Load(cfgPath);
    }

    if (g_PcConfig.enableDebugLog)
    {
        SH_DebugLogInit();
        SH_DBG("[SH] main() entered (log opened post-config)");
    }

    {
        extern float g_PsxPixelAspect;
        g_PsxPixelAspect = (320.0f / 224.0f) * (3.0f / 4.0f);
    }

    {
        extern int g_PcWidescreenMode;
        extern int g_PcMenuPillarbox;
        g_PcWidescreenMode = g_PcConfig.widescreenMode;
        g_PcMenuPillarbox  = g_PcConfig.menuPillarbox;
    }

    /* No external console on Vita, and critically no /dev/null either (that
     * path is a POSIX/Linux-Windows convention -- Vita's newlib only knows
     * ux0:/app0:/ur0:/etc device prefixes). Per the C standard, freopen()
     * closes the ORIGINAL stream even when it fails to open the new target,
     * so the previous "else freopen(\"/dev/null\", ...)" branch (taken by
     * default, since enableDebugLog defaults to 0) was leaving stdout/
     * stderr closed from the very start of main() on real hardware --
     * a very early, hard-to-diagnose failure. Only redirect when we have an
     * actual, valid target (the log file); otherwise just leave the
     * default stdio handles alone. */
    {
        if (g_PcConfig.enableDebugLog)
        {
            freopen(SH_LogPath(), "a", stdout);
            freopen(SH_LogPath(), "a", stderr);
            setvbuf(stdout, NULL, _IONBF, 0);
            setvbuf(stderr, NULL, _IONBF, 0);
        }

        {
            extern void DbgOverlay_PushLine(const char* line);
            extern void DbgOverlay_ToastLine(const char* line);
            g_ShOverlayPushLine  = DbgOverlay_PushLine;
            g_ShOverlayToastLine = DbgOverlay_ToastLine;
        }
        {
            extern void DbgOverlay_Render(void);
            extern void (*g_PsyX_PostCaptureHook)(void);
            g_PsyX_PostCaptureHook = DbgOverlay_Render;
        }
    }

    /* Vita's display is fixed at 960x544; ignore any windowWidth/Height the
     * config asks for (the Vita render context is always created at native
     * resolution -- see GR_InitialiseGLContext's __vita__ branch). */
    int windowWidth  = 960;
    int windowHeight = 544;

    SH_LOG("Game data path: %s", g_GameDataPath);

    SH_LOG("Initializing PSX memory emulation...");
    PsxMemory_Init();
    SH_LOG("PSX RAM base: %p", (void*)g_PsxRam);

    PcPort_InitCharaAnimInfo();
    extern void PcPort_InitSdBuffers(void);
    PcPort_InitSdBuffers();

    extern void AsRodata_Reformat(void);
    AsRodata_Reformat();

    extern void GroanerAnimInfos_Init(void);
    GroanerAnimInfos_Init();
    extern void BloodsuckerAnimInfos_Init(void);
    BloodsuckerAnimInfos_Init();
    extern void BloodyLisaAnimInfos_Init(void);
    BloodyLisaAnimInfos_Init();
    extern void AlessaAnimInfos_Init(void);
    AlessaAnimInfos_Init();
    extern void GhostChildAlessaAnimInfos_Init(void);
    GhostChildAlessaAnimInfos_Init();
    extern void LisaAnimInfos_Init(void);
    LisaAnimInfos_Init();
    extern void KaufmannAnimInfos_Init(void);
    KaufmannAnimInfos_Init();
    extern void DahliaAnimInfos_Init(void);
    DahliaAnimInfos_Init();
    extern void CatAnimInfos_Init(void);
    CatAnimInfos_Init();
    extern void PuppetNurseData_Init(void);
    PuppetNurseData_Init();
    extern void LarvalStalkerAnimInfos_Init(void);
    LarvalStalkerAnimInfos_Init();
    extern void HangedScratcherAnimInfos_Init(void);
    HangedScratcherAnimInfos_Init();
    extern void CreeperAnimInfos_Init(void);
    CreeperAnimInfos_Init();
    extern void SplitHeadAnimInfos_Init(void);
    SplitHeadAnimInfos_Init();
    extern void RomperAnimInfos_Init(void);
    RomperAnimInfos_Init();
    extern void LockerDeadBodyAnimInfos_Init(void);
    LockerDeadBodyAnimInfos_Init();
    extern void TwinfeelerAnimInfos_Init(void);
    TwinfeelerAnimInfos_Init();
    extern void FloatstingerAnimInfos_Init(void);
    FloatstingerAnimInfos_Init();
    extern void MonsterCybilAnimInfos_Init(void);
    MonsterCybilAnimInfos_Init();
    extern void FlaurosAnimInfos_Init(void);
    FlaurosAnimInfos_Init();
    extern void ParasiteAnimInfos_Init(void);
    ParasiteAnimInfos_Init();
    extern void GhostDoctorAnimInfos_Init(void);
    GhostDoctorAnimInfos_Init();
    extern void BloodyIncubatorAnimInfos_Init(void);
    BloodyIncubatorAnimInfos_Init();
    extern void IncubatorAnimInfos_Init(void);
    IncubatorAnimInfos_Init();
    extern void LittleIncubusAnimInfos_Init(void);
    LittleIncubusAnimInfos_Init();
    extern void IncubusAnimInfos_Init(void);
    IncubusAnimInfos_Init();
    extern void Unkkown23AnimInfos_Init(void);
    Unkkown23AnimInfos_Init();
    extern void Map6S04ExtraAnimInfos_Init(void);
    Map6S04ExtraAnimInfos_Init();

    {
        extern void* D_800ED230[2];
        D_800ED230[0] = FS_BUFFER_20;
        D_800ED230[1] = FS_BUFFER_18;
    }

#if VERSION_IS(JAP0)
    g_OvlDynamic  = PSX_ADDR(0x000CBAA8);
#else
    g_OvlDynamic  = PSX_ADDR(0x000C9578);
#endif
    g_OvlBodyprog = PSX_ADDR(0x00024B60);
    g_Demo_PlayFileBufferPtr = (s_DemoFrameData*)PSX_ADDR(0x000F5E00);

    PsyX_Log_SetStream(g_PcConfig.enableDebugLog ? g_ShDebugLog : NULL);

    SH_LOG("Initializing PsyCross (SDL2 + vitaGL)...");
    PsyX_Initialise("Silent Hill", windowWidth, windowHeight, 1 /* fullscreen: always, see note above */);
    SH_LOG("PsyCross initialized. Window: %dx%d", windowWidth, windowHeight);

    {
        const char* gl_renderer = (const char*)glGetString(GL_RENDERER);
        const char* gl_version  = (const char*)glGetString(GL_VERSION);
        SH_LOG("GL Renderer: %s", gl_renderer ? gl_renderer : "(null)");
        SH_LOG("GL Version:  %s", gl_version  ? gl_version  : "(null)");
    }

    /* Keyboard/mouse control-scheme binds are meaningless without a
     * keyboard/mouse; SDL2-vita's native game-controller mapping already
     * drives the pad from the PSX-button defaults PsyX_Initialise sets up
     * (see PsyCross/src/pad/PsyX_pad.cpp), so there's nothing to rebind here. */

    g_cfg_msaaSamples = 0; /* see GR_InitialiseRender's __vita__ branch */
    PsyX_ApplyVsync(g_PcConfig.vsync);
    SH_LOG("VSync: %s", g_PcConfig.vsync != 0 ? "on" : "off");

    switch (g_PcConfig.psxDither) {
    case 1:  g_cfg_psxDither = 1; g_cfg_bilinearFiltering = 0; break;
    case 2:  g_cfg_psxDither = 0; g_cfg_bilinearFiltering = 1; break;
    default: g_cfg_psxDither = 0; g_cfg_bilinearFiltering = 0; break;
    }
    g_cfg_menuFilter = g_PcConfig.menuFilter ? 1 : 0;
    g_cfg_disableDpadMovement = 0;

    g_PsxUsePgxp = g_PcConfig.usePgxp ? 1 : 0;
    g_cfg_postProcess = g_PcConfig.postProcess;
    {
        extern int g_cfg_tonemap;
        g_cfg_tonemap = g_PcConfig.tonemap;
    }
    {
        Pc_FlashlightModeApply(g_PcConfig.flashlightMode, 0);
    }

    {
        extern void  PsyX_SPUAL_SetAdsrEnabled(int on);
        extern void  PsyX_SPUAL_SetReverbDepthScale(float scale);
        PsyX_SPUAL_SetAdsrEnabled(g_PcConfig.adsr ? 1 : 0);
        if (g_PcConfig.reverbScale > 0.0f)
            PsyX_SPUAL_SetReverbDepthScale(g_PcConfig.reverbScale);
    }
    {
        extern void PsyX_SPUAL_SetOutputMode(int mode);
        PsyX_SPUAL_SetOutputMode(g_PcConfig.audioOutput);
    }
    {
        extern float g_PsyX_FlashlightIntensity, g_cfg_postProcessIntensity, g_cfg_tonemapIntensity;
        extern float g_PsyX_FlashlightSize;
        extern float g_PsyX_FlashlightIntensityFps, g_PsyX_FlashlightSizeFps;
        g_PsyX_FlashlightIntensity = g_PcConfig.flashlightIntensity;
        g_cfg_postProcessIntensity = g_PcConfig.postProcessIntensity;
        g_cfg_tonemapIntensity     = g_PcConfig.tonemapIntensity;
        {
            extern float g_cfg_brightness, g_cfg_contrast, g_cfg_saturation;
            g_cfg_brightness = g_PcConfig.brightness;
            g_cfg_contrast   = g_PcConfig.contrast;
            g_cfg_saturation = g_PcConfig.saturation;
        }
        g_PsyX_FlashlightSize      = g_PcConfig.flashlightSize;
        g_PsyX_FlashlightIntensityFps = g_PcConfig.flashlightIntensityFps;
        g_PsyX_FlashlightSizeFps      = g_PcConfig.flashlightSizeFps;
    }
    {
        extern float g_PcXaVolume;
        extern float g_PcFmvVolume;
        g_PcXaVolume = g_PcConfig.xaVolume;
        g_PcFmvVolume = g_PcConfig.fmvVolume;
    }

    SH_LOG("Initializing PSY-Q subsystems...");
    ResetCallback();
    SpuInit();

    SH_LOG("Initializing CD filesystem...");
    {
        const char* cdImagePath = PcPort_GetGameDiscPath();
        if (cdImagePath[0])
        {
            SH_LOG("CD image found, initializing CDFS...");
            PsyX_CDFS_Init(cdImagePath, 0, 0);
        }
        else
        {
            SH_WARN("Game will not be able to load assets without a disc image. "
                    "Copy your Silent Hill .bin to %s/", g_GameDataPath);
        }
    }

    { extern void CharaData_ApplyRegionPatches(void); CharaData_ApplyRegionPatches(); }
    { extern void Font_ApplyRegionPatches(void); Font_ApplyRegionPatches(); }
    { extern void Pc_LangInit(void); Pc_LangInit(); }

    CdInit();

    SH_LOG("Initializing GPU...");
    ResetGraph(0);
    SetGraphDebug(0);

    SH_LOG("Initializing filesystem queue...");
    Fs_QueueInitialize();

    {
        extern void Pc_Rando_Init(void);
        Pc_Rando_Init();
    }
    {
        extern void Pc_CharaGlobal_Open(void);
        Pc_CharaGlobal_Open();
    }

    SH_LOG("Initializing map registry...");
    MapRegistry_Init();
    SH_LOG("Active map: %s", g_PcConfig.mapName);

    SH_LOG("All subsystems initialized. Entering MainLoop...");

    MainLoop();

    SH_DBG("[SH] MainLoop exited normally. Shutting down...");
    PsyX_Shutdown();

    sceKernelExitProcess(0);
    return 0;
}
