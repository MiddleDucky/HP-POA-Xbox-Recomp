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
 * Nothing here wraps a guest function, so the recompile does not need
 * --exclude-manual for this file.
 */

#include <stddef.h>
#include <stdint.h>

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
