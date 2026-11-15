/**
 * Video-side hooks the renderer (renderer/gpu/d3d12.cpp) asks this title about.
 *
 * The renderer came over from the Philosopher's Stone port, where its 16:9
 * 2D rules need to know when the main menu's background movie or a loading
 * screen is up and which textures are text icons. HP: Prisoner of Azkaban
 * plays its movies (EA .mad files) through its own decoder into textures and
 * has none of those hooks found yet, so each answers "no" and the renderer
 * treats every frame alike.
 *
 * sub_001AC0A0 (the script's PlayMovie) is wrapped below, so regen.sh lists
 * this file with --exclude-manual.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RECOMP_GENERATED_CODE
#include "recomp/gen/recomp_types.h"

/* F10 Enhancements, "Skip the EA and Warner Bros. logos". The boot script plays its movies through PlayMovie
 * (sub_001AC0A0, thiscall, ret 8: the name, as "<Movie>%s.mad" spells it, and a second argument): eac_usa / eac_eng
 * (EA), wb, then intro. PlayMovie marks the movie object playing (its byte at ecx), sets the script's IsMoviePlaying
 * to true and makes the player ([0x364120], sub_0005DBE0). Each frame the main loop (0x1DEEA) asks the player whether
 * it is still going (sub_0005DE00: no player is "ended"); once it is not, it runs StopMovie (sub_001AC100) and sets
 * IsMoviePlaying false, which is what the script waits for. So a logo still goes through PlayMovie, and its player is
 * stopped at once (sub_0005DC90, the same stop StopMovie calls): the next frame ends it the game's own way. Returning
 * without PlayMovie left the script waiting on a movie that never ended (hung on challengemovie, 2026-11-15).
 * intro (the story's opening) still plays. */
extern int hppoa_SkipIntro(void);  /* renderer/app/app.cpp */
extern void sub_0005DC90(void);    /* the movie player's stop */

extern void sub_001AC0A0_gen(void);
void sub_001AC0A0(void)
{
    uint32_t name = MEM32(g_esp + 4);
    int skip = 0;
    if (name && hppoa_SkipIntro()) {
        const char *n = (const char *)XBOX_PTR(name);
        skip = _stricmp(n, "wb") == 0 || _strnicmp(n, "eac", 3) == 0;
        if (skip) fprintf(stderr, "[MOVIE] skipped %s (skip_intro)\n", n);
    }
    sub_001AC0A0_gen();
    if (skip && MEM32(0x364120u)) {
        /* every register as PlayMovie left it: its caller may read them */
        uint32_t s_esp = g_esp, s_eax = g_eax, s_ebx = g_ebx, s_ecx = g_ecx, s_edx = g_edx, s_esi = g_esi,
                 s_edi = g_edi;
        g_esp -= 4;
        MEM32(g_esp) = 0;  /* return address, unused */
        sub_0005DC90();
        g_esp = s_esp, g_eax = s_eax, g_ebx = s_ebx, g_ecx = s_ecx, g_edx = s_edx, g_esi = s_esi, g_edi = s_edi;
    }
}

extern ptrdiff_t g_xbox_mem_offset;

/* a movie is playing: the title's movie player (made at "-=+ Movie Start +=-" in sub_0005DBE0, [0x364120], cleared
 * at its stop). The renderer keeps its picture 4:3 in a 16:9 run */
int hppoa_MenuMovie(void)
{
    return g_xbox_mem_offset && *(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + 0x364120u) != 0;
}

/* the loading screen's thread is drawing it */
int hppoa_LoadingScreen(void) { return 0; }

/* loading screens so far (d3d12.cpp's RECOMP_CAP_LOADING captures the first frame of each) */
int hppoa_LoadingCount(void) { return 0; }

/* the texture is a button icon that text puts in its lines (kept out of the cutscene HUD hiding) */
int hppoa_TextIcon(uint32_t tex)
{
    (void)tex;
    return 0;
}

void hppoa_TextIconDump(void) {}
