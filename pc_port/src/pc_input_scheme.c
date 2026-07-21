/*
 * pc_input_scheme.c - control scheme -> PsyCross input mapping
 *
 * Shared by every port (moved out of main_pc.c so it also builds for
 * main_vita.c, since control_style.c calls Pc_ApplyActiveControlScheme /
 * Pc_ApplyClassicControlScheme unconditionally). On a platform with no
 * physical keyboard/mouse (Vita), the keyboard/mouse binds below still get
 * applied to g_cfg_keyboardMapping* / g_cfg_mouseButtonMask -- they're just
 * inert, since PsyX_pad.cpp's per-frame poll skips keyboard/mouse state that
 * doesn't exist on that platform. Only the controller mapping actually does
 * anything there, which is all that's needed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "pc_config.h"
#include <PsyX/PsyX_public.h>

/* Defined in main_pc.c / main_vita.c (per-platform entry point). */
extern int g_PcAllowDebugControls;
extern int g_PcUnlimitedEnemies;

static int Pc_ParseMouseName(const char* v)
{
    if (!v) return 0;
    if (SDL_strcasecmp(v, "MouseWheelUp")   == 0) return 6;
    if (SDL_strcasecmp(v, "MouseWheelDown") == 0) return 7;
    if ((v[0] == 'M' || v[0] == 'm') && (v[1] == 'o' || v[1] == 'O') &&
        (v[2] == 'u' || v[2] == 'U') && (v[3] == 's' || v[3] == 'S') &&
        (v[4] == 'e' || v[4] == 'E'))
    {
        switch (atoi(v + 5))
        {
            case 1: return SDL_BUTTON_LEFT;
            case 2: return SDL_BUTTON_RIGHT;
            case 3: return SDL_BUTTON_MIDDLE;
            case 4: return SDL_BUTTON_X1;
            case 5: return SDL_BUTTON_X2;
        }
    }
    return 0;
}

/* Apply a "key or mouse" bind value to a PSX-button slot: an SDL key name goes
 * into *kc (scancode); a "MouseN" value adds the PSX bit to the mouse mask and
 * leaves *kc unbound; "NONE"/empty = unbound. Used for BOTH the primary and the
 * secondary keyboard binds so the mouse can be a PRIMARY bind (e.g. modern
 * Fire = Left Mouse). The caller clears the mouse mask once before applying. */
static void Pc_ApplyKeyOrMouse(const char* v, unsigned short bit, int* kc)
{
    int mb;
    if (!v || !v[0] || strcmp(v, "NONE") == 0) { *kc = SDL_SCANCODE_UNKNOWN; return; }
    mb = Pc_ParseMouseName(v);
    if (mb > 0) { g_cfg_mouseButtonMask[mb] |= bit; *kc = SDL_SCANCODE_UNKNOWN; }
    else        { *kc = PsyX_LookupKeyboardMapping(v, SDL_SCANCODE_UNKNOWN); }
}

/* Apply ONE control scheme (classic or altcam) onto the PsyCross input mapping.
 * Rebuilds all four mappings from scratch each call (primary keyboard, secondary
 * keyboard, primary controller, secondary controller) + the mouse mask, so a
 * runtime scheme swap is just "re-run with the other scheme". Unbound = "NONE"
 * -> SDL_SCANCODE_UNKNOWN / BUTTON_INVALID; nothing falls back to a built-in
 * default, so the config is fully respected. Call via Pc_ApplyActiveControlScheme. */
static void Pc_ApplyControlConfig(const ControlScheme* s)
{
    extern int g_cfg_controllerMovement;
    int i;

    /* Reset the keyboard layers' mouse contribution; rebuilt from primary + secondary. */
    for (i = 0; i < 8; i++) g_cfg_mouseButtonMask[i] = 0;

    /* Primary keyboard (key OR mouse button). */
    Pc_ApplyKeyOrMouse(s->keyUp,       0x10,   &g_cfg_keyboardMapping.kc_dpad_up);
    Pc_ApplyKeyOrMouse(s->keyDown,     0x40,   &g_cfg_keyboardMapping.kc_dpad_down);
    Pc_ApplyKeyOrMouse(s->keyLeft,     0x80,   &g_cfg_keyboardMapping.kc_dpad_left);
    Pc_ApplyKeyOrMouse(s->keyRight,    0x20,   &g_cfg_keyboardMapping.kc_dpad_right);
    Pc_ApplyKeyOrMouse(s->keyCross,    0x4000, &g_cfg_keyboardMapping.kc_cross);
    Pc_ApplyKeyOrMouse(s->keyCircle,   0x2000, &g_cfg_keyboardMapping.kc_circle);
    Pc_ApplyKeyOrMouse(s->keyTriangle, 0x1000, &g_cfg_keyboardMapping.kc_triangle);
    Pc_ApplyKeyOrMouse(s->keySquare,   0x8000, &g_cfg_keyboardMapping.kc_square);
    Pc_ApplyKeyOrMouse(s->keyL1,       0x400,  &g_cfg_keyboardMapping.kc_l1);
    Pc_ApplyKeyOrMouse(s->keyR1,       0x800,  &g_cfg_keyboardMapping.kc_r1);
    Pc_ApplyKeyOrMouse(s->keyL2,       0x100,  &g_cfg_keyboardMapping.kc_l2);
    Pc_ApplyKeyOrMouse(s->keyR2,       0x200,  &g_cfg_keyboardMapping.kc_r2);
    Pc_ApplyKeyOrMouse(s->keyL3,       0x2,    &g_cfg_keyboardMapping.kc_l3);
    Pc_ApplyKeyOrMouse(s->keyR3,       0x4,    &g_cfg_keyboardMapping.kc_r3);
    Pc_ApplyKeyOrMouse(s->keyStart,    0x8,    &g_cfg_keyboardMapping.kc_start);
    Pc_ApplyKeyOrMouse(s->keySelect,   0x1,    &g_cfg_keyboardMapping.kc_select);

    /* Secondary keyboard (second key/mouse per action; AND-combined per frame). */
    Pc_ApplyKeyOrMouse(s->keyUp2,       0x10,   &g_cfg_keyboardMapping2.kc_dpad_up);
    Pc_ApplyKeyOrMouse(s->keyDown2,     0x40,   &g_cfg_keyboardMapping2.kc_dpad_down);
    Pc_ApplyKeyOrMouse(s->keyLeft2,     0x80,   &g_cfg_keyboardMapping2.kc_dpad_left);
    Pc_ApplyKeyOrMouse(s->keyRight2,    0x20,   &g_cfg_keyboardMapping2.kc_dpad_right);
    Pc_ApplyKeyOrMouse(s->keyCross2,    0x4000, &g_cfg_keyboardMapping2.kc_cross);
    Pc_ApplyKeyOrMouse(s->keyCircle2,   0x2000, &g_cfg_keyboardMapping2.kc_circle);
    Pc_ApplyKeyOrMouse(s->keyTriangle2, 0x1000, &g_cfg_keyboardMapping2.kc_triangle);
    Pc_ApplyKeyOrMouse(s->keySquare2,   0x8000, &g_cfg_keyboardMapping2.kc_square);
    Pc_ApplyKeyOrMouse(s->keyL12,       0x400,  &g_cfg_keyboardMapping2.kc_l1);
    Pc_ApplyKeyOrMouse(s->keyR12,       0x800,  &g_cfg_keyboardMapping2.kc_r1);
    Pc_ApplyKeyOrMouse(s->keyL22,       0x100,  &g_cfg_keyboardMapping2.kc_l2);
    Pc_ApplyKeyOrMouse(s->keyR22,       0x200,  &g_cfg_keyboardMapping2.kc_r2);
    Pc_ApplyKeyOrMouse(s->keyL32,       0x2,    &g_cfg_keyboardMapping2.kc_l3);
    Pc_ApplyKeyOrMouse(s->keyR32,       0x4,    &g_cfg_keyboardMapping2.kc_r3);
    Pc_ApplyKeyOrMouse(s->keyStart2,    0x8,    &g_cfg_keyboardMapping2.kc_start);
    Pc_ApplyKeyOrMouse(s->keySelect2,   0x1,    &g_cfg_keyboardMapping2.kc_select);

    /* Primary controller. */
    g_cfg_controllerMapping.gc_cross    = PsyX_LookupGameControllerMapping(s->padCross,    SDL_CONTROLLER_BUTTON_INVALID);
    g_cfg_controllerMapping.gc_circle   = PsyX_LookupGameControllerMapping(s->padCircle,   SDL_CONTROLLER_BUTTON_INVALID);
    g_cfg_controllerMapping.gc_triangle = PsyX_LookupGameControllerMapping(s->padTriangle, SDL_CONTROLLER_BUTTON_INVALID);
    g_cfg_controllerMapping.gc_square   = PsyX_LookupGameControllerMapping(s->padSquare,   SDL_CONTROLLER_BUTTON_INVALID);
    g_cfg_controllerMapping.gc_l1       = PsyX_LookupGameControllerMapping(s->padL1,       SDL_CONTROLLER_BUTTON_INVALID);
    g_cfg_controllerMapping.gc_r1       = PsyX_LookupGameControllerMapping(s->padR1,       SDL_CONTROLLER_BUTTON_INVALID);
    g_cfg_controllerMapping.gc_l2       = PsyX_LookupGameControllerMapping(s->padL2,       SDL_CONTROLLER_BUTTON_INVALID);
    g_cfg_controllerMapping.gc_r2       = PsyX_LookupGameControllerMapping(s->padR2,       SDL_CONTROLLER_BUTTON_INVALID);
    g_cfg_controllerMapping.gc_l3       = PsyX_LookupGameControllerMapping(s->padL3,       SDL_CONTROLLER_BUTTON_INVALID);
    g_cfg_controllerMapping.gc_r3       = PsyX_LookupGameControllerMapping(s->padR3,       SDL_CONTROLLER_BUTTON_INVALID);
    g_cfg_controllerMapping.gc_start    = PsyX_LookupGameControllerMapping(s->padStart,    SDL_CONTROLLER_BUTTON_INVALID);
    g_cfg_controllerMapping.gc_select   = PsyX_LookupGameControllerMapping(s->padSelect,   SDL_CONTROLLER_BUTTON_INVALID);

    /* Secondary controller (second button per action; AND-combined per frame).
     * dpad/axes of mapping2 stay BUTTON_INVALID (set once in PsyX init). */
    g_cfg_controllerMapping2.gc_cross    = PsyX_LookupGameControllerMapping(s->padCross2,    SDL_CONTROLLER_BUTTON_INVALID);
    g_cfg_controllerMapping2.gc_circle   = PsyX_LookupGameControllerMapping(s->padCircle2,   SDL_CONTROLLER_BUTTON_INVALID);
    g_cfg_controllerMapping2.gc_triangle = PsyX_LookupGameControllerMapping(s->padTriangle2, SDL_CONTROLLER_BUTTON_INVALID);
    g_cfg_controllerMapping2.gc_square   = PsyX_LookupGameControllerMapping(s->padSquare2,   SDL_CONTROLLER_BUTTON_INVALID);
    g_cfg_controllerMapping2.gc_l1       = PsyX_LookupGameControllerMapping(s->padL12,       SDL_CONTROLLER_BUTTON_INVALID);
    g_cfg_controllerMapping2.gc_r1       = PsyX_LookupGameControllerMapping(s->padR12,       SDL_CONTROLLER_BUTTON_INVALID);
    g_cfg_controllerMapping2.gc_l2       = PsyX_LookupGameControllerMapping(s->padL22,       SDL_CONTROLLER_BUTTON_INVALID);
    g_cfg_controllerMapping2.gc_r2       = PsyX_LookupGameControllerMapping(s->padR22,       SDL_CONTROLLER_BUTTON_INVALID);
    g_cfg_controllerMapping2.gc_l3       = PsyX_LookupGameControllerMapping(s->padL32,       SDL_CONTROLLER_BUTTON_INVALID);
    g_cfg_controllerMapping2.gc_r3       = PsyX_LookupGameControllerMapping(s->padR32,       SDL_CONTROLLER_BUTTON_INVALID);
    g_cfg_controllerMapping2.gc_start    = PsyX_LookupGameControllerMapping(s->padStart2,    SDL_CONTROLLER_BUTTON_INVALID);
    g_cfg_controllerMapping2.gc_select   = PsyX_LookupGameControllerMapping(s->padSelect2,   SDL_CONTROLLER_BUTTON_INVALID);

    g_PcAllowDebugControls   = g_PcConfig.allowDebugControls;
    g_PcUnlimitedEnemies     = g_PcConfig.unlimitedEnemies;
    g_cfg_controllerMovement = g_PcConfig.controllerMovement;
    g_cfg_allowMouseSecondary = 1; /* mouse + secondary binds always active */
}

/* Select + apply the control scheme matching the active camera mode: altcam for
 * any alternate/modern camera (g_DebugThirdPersonCam != 0), classic otherwise.
 * Called at boot and whenever the control style (camera) changes. */
void Pc_ApplyActiveControlScheme(void)
{
    extern int g_DebugThirdPersonCam;
    Pc_ApplyControlConfig(g_DebugThirdPersonCam ? &g_PcConfig.altcam : &g_PcConfig.classic);
}

/* Force the classic (default) control scheme regardless of the active camera.
 * Menus always navigate with the default binds, so an alternate camera's binds
 * (mouse-look / remapped buttons) don't leak into menu navigation. Restored to
 * the camera-matched scheme via Pc_ApplyActiveControlScheme on return to
 * gameplay (driven by Pc_ControlStyleUpdate). */
void Pc_ApplyClassicControlScheme(void)
{
    Pc_ApplyControlConfig(&g_PcConfig.classic);
}
