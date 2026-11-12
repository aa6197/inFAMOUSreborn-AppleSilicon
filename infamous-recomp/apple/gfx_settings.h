/* Graphics settings the player picks (Apple port only).
 *   iOS / iPadOS: the Settings app, inFamous page (ios/Settings.bundle).
 *   macOS: the Graphics menu (mac/mac_app.mm); saved in the app bundle only.
 * Both keep the choice in NSUserDefaults. At start it becomes environment,
 * before launch.env and settings.ini are read; a value already in the
 * environment (a launcher, a test run) wins. A change while the game runs
 * takes effect at once (port_gfx_apply_live).
 *
 * "Shadows": 2 on (default), 1 near only, 0 off. The sun's shadow is four
 * depth maps a frame, ~40% of all draws (rsx_draw_engine.c, SHADOWS /
 * SHADOW_MAPS). Near only keeps the first map, the one around the player. */
#pragma once
#import <Foundation/Foundation.h>
#include <stdio.h>
#include <stdlib.h>

extern "C" void rsx_engine_set_shadow_maps(int n);
extern "C" int  rsx_engine_shadow_maps(void);

enum { PORT_GFX_SHADOWS_OFF = 0, PORT_GFX_SHADOWS_NEAR = 1, PORT_GFX_SHADOWS_ON = 2 };
#define PORT_GFX_NEAR_MAPS 1

/* Shadow maps drawn per frame for a setting: -1 all. */
static inline int port_gfx_shadow_maps(long shadows)
{
    return shadows <= PORT_GFX_SHADOWS_OFF ? 0 : shadows == PORT_GFX_SHADOWS_NEAR ? PORT_GFX_NEAR_MAPS : -1;
}

/* The engine's current maps per frame as a setting. */
static inline long port_gfx_shadows_now(void)
{
    const int n = rsx_engine_shadow_maps();
    return n < 0 ? PORT_GFX_SHADOWS_ON : n == 0 ? PORT_GFX_SHADOWS_OFF : PORT_GFX_SHADOWS_NEAR;
}

static inline const char* port_gfx_shadows_name(long shadows)
{
    return shadows <= PORT_GFX_SHADOWS_OFF ? "off" : shadows == PORT_GFX_SHADOWS_NEAR ? "near only" : "on";
}

/* The saved choice, or -1 when the player never changed it. */
static inline long port_gfx_saved_shadows(void)
{
    id v = [NSUserDefaults.standardUserDefaults objectForKey:@"Shadows"];
    if (![v respondsToSelector:@selector(integerValue)]) return -1;
    const long s = (long)[v integerValue];
    return s < PORT_GFX_SHADOWS_OFF ? PORT_GFX_SHADOWS_OFF : s > PORT_GFX_SHADOWS_ON ? PORT_GFX_SHADOWS_ON : s;
}

static long s_port_gfx_applied = -1;

/* At start, before launch.env. */
static inline void port_gfx_apply_env(void)
{
    const long sh = port_gfx_saved_shadows();
    s_port_gfx_applied = sh;
    if (sh < 0 || sh == PORT_GFX_SHADOWS_ON) return;
    if (getenv("SHADOW_MAPS") || getenv("SHADOWS_PHASE")) return;   /* a test run's own choice */
    if (sh == PORT_GFX_SHADOWS_OFF) {
        setenv("SHADOWS", "0", 0);   /* before launch.env, so its SHADOWS=1 does not win */
    } else {
        char b[8];
        snprintf(b, sizeof b, "%d", PORT_GFX_NEAR_MAPS);
        setenv("SHADOW_MAPS", b, 0);
    }
    fprintf(stderr, "[gfx] Shadows: %s (saved setting)\n", port_gfx_shadows_name(sh));
}

/* While the game runs: apply a choice saved since the last call. */
static inline void port_gfx_apply_live(void)
{
    const long sh = port_gfx_saved_shadows();
    if (sh < 0 || sh == s_port_gfx_applied) return;
    s_port_gfx_applied = sh;
    rsx_engine_set_shadow_maps(port_gfx_shadow_maps(sh));
    fprintf(stderr, "[gfx] Shadows: %s (changed)\n", port_gfx_shadows_name(sh));
}

/* macOS menu: apply now and save (save = false outside the app bundle). */
static inline void port_gfx_set_shadows(long sh, bool save)
{
    if (save) [NSUserDefaults.standardUserDefaults setInteger:sh forKey:@"Shadows"];
    s_port_gfx_applied = sh;
    rsx_engine_set_shadow_maps(port_gfx_shadow_maps(sh));
    fprintf(stderr, "[gfx] Shadows: %s (menu)\n", port_gfx_shadows_name(sh));
}
