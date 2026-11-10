/**
 * 60 fps: the game clock's minimum step.
 *
 * The game clock (singleton 0x38BA6C, ticked by sub_001A7450 once a frame) advances by the real time since the last
 * frame, but never by less than its minimum step (+0x18). The title sets that at boot (0x34A28) to 1000 / 30 = 33 ms
 * on an NTSC console (1000 / 25 = 40 ms on PAL), the length of a frame at its D3DPRESENT_INTERVAL_TWO rate. Shown at
 * 60 fps the clock would still step 33 ms a frame and the game would run double speed, so with frame_rate 60
 * (hppoa.toml, RECOMP_FRAME_RATE) the minimum becomes one 60 Hz frame and the clock follows real time again.
 * The present interval itself is hle_d3d8.cpp's CreateDevice.
 *
 * Cutscenes: their scripts put the clock in fixed-step mode (script command sub_000676A0 -> setter sub_00213E40,
 * +0x14 = ms a frame, 0 = real time again), so each frame adds that many ms however long it took: the cutscene is
 * locked to the frame rate the title runs at. At 60 fps the step is spread over two frames (33 -> 16, 17, 16, ...,
 * the odd ms carried, so nothing drifts from the dialogue); the getter sub_001E7640 still answers the script's own
 * value, which the title saves and puts back.
 *
 * Built into the recompile with --exclude-manual naming this file.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* the register names (eax, esp, ...) the runtime macros expand to */
#define RECOMP_GENERATED_CODE
#include "recomp/gen/recomp_types.h"

#define GAME_CLOCK 0x0038BA6Cu

uint32_t hppoa_FrameRate(void);  /* app.cpp: 30 or 60 */
uint32_t hppoa_SwapCount(void);  /* hle_d3d8.cpp: frames presented */
int hppoa_Sneaking(void);         /* hle_debug.c: the player is wall sneaking, run at 30 fps */
static uint32_t g_min_title;     /* the clock's minimum step as the title set it (33 ms NTSC, 40 PAL) */

/* a class's +0x18 setter (__thiscall, ecx = object, [esp+4] = value); the game clock's minimum step among them */
extern void sub_00096520_gen(void);
void sub_00096520(void)
{
    if (g_ecx == GAME_CLOCK) g_min_title = MEM32(g_esp + 4);
    if (g_ecx == GAME_CLOCK && hppoa_FrameRate() >= 60 && MEM32(g_esp + 4) > 1000 / 60)
        MEM32(g_esp + 4) = 1000 / 60;
    sub_00096520_gen();
}

static uint32_t g_fixed;  /* the game clock's fixed step as the title set it (ms a 30 fps frame), 0 real time */
static uint32_t g_carry;  /* at 60 fps: the odd ms of the last frame pair */

/* d3d12.cpp: a cutscene is playing (its script holds the game clock at a fixed step a frame; the clock's fixed mode
 * also takes steps of seconds, which are no cutscene's), or its letterbox is on screen (not every cutscene holds the
 * clock: Night 0's corridor with the twins does not) */
int hppoa_Letterbox(void);  /* d3d12.cpp */
int hppoa_Cutscene(void) { return (g_fixed > 0 && g_fixed <= 1000) || hppoa_Letterbox(); }

/* a class's +0x14 setter (__thiscall, [esp+4] = value); the game clock's fixed step among them */
extern void sub_00213E40_gen(void);
void sub_00213E40(void)
{
    if (g_ecx == GAME_CLOCK) {
        uint32_t v = MEM32(g_esp + 4);
        if (v != g_fixed) fprintf(stderr, "[CLOCK] fixed step %u ms (from %08X)\n", v, MEM32(g_esp));
        g_fixed = v, g_carry = 0;
    }
    sub_00213E40_gen();
}

/* a class's +0x14 getter (__thiscall); the game clock answers the title's own fixed step */
extern void sub_001E7640_gen(void);
void sub_001E7640(void)
{
    uint32_t clk = g_ecx;
    sub_001E7640_gen();
    if (clk == GAME_CLOCK) g_eax = g_fixed;
}

/* [LOADFPS]: return addresses of the loading loop's tick and present, a few each load */
static volatile int g_loading;
static int g_loading_shown;
static void load_stack(const char *what)
{
    char line[256];
    int len = snprintf(line, sizeof line, "[LOADFPS] %lu ms %s from", (unsigned long)GetTickCount(), what), found = 0;
    for (uint32_t i = 0; i < 96 && found < 10 && len < (int)sizeof line - 12; i++) {
        uint32_t v = MEM32(esp + i * 4);
        if (v >= 0x11005 && v < 0x264C64) len += snprintf(line + len, sizeof line - len, " %06X", v), found++;
    }
    fprintf(stderr, "%s\n", line);
}

/* hle_d3d8.cpp, at Swap: frames that came slowly, where from */
void hppoa_ClockOnSwap(void)
{
    static int shown;
    static DWORD last;
    DWORD now = GetTickCount(), gap = now - last;
    last = now;
    if (gap > 40 && gap < 200 && shown < 60) {
        char what[32];
        snprintf(what, sizeof what, "swap after %lu ms", (unsigned long)gap);
        shown++, load_stack(what);
    }
}

/* kernel_bridge.c, at KeDelayExecutionThread: each different place that sleeps 5 ms or more, with the time it asked */
void hppoa_ClockOnDelay(uint32_t interval)
{
    static uint32_t seen[96];
    static LONG lock;
    if (!interval || MEM32(interval + 4) != 0xFFFFFFFFu) return;
    uint32_t ms = (uint32_t)(0u - MEM32(interval)) / 10000u;
    if (ms < 5 || ms > 2000) return;
    uint32_t h = ms * 31u;
    for (uint32_t i = 0, n = 0; i < 64 && n < 4; i++) {
        uint32_t v = MEM32(esp + i * 4);
        if (v >= 0x11005 && v < 0x264C64) h = h * 33u + v, n++;
    }
    while (InterlockedExchange(&lock, 1)) Sleep(0);
    int k = 0;
    while (k < 96 && seen[k] && seen[k] != h) k++;
    int fresh = k < 96 && !seen[k];
    if (fresh) seen[k] = h;
    InterlockedExchange(&lock, 0);
    if (fresh) {
        char what[48];
        snprintf(what, sizeof what, "sleep %u ms tid %lu", ms, GetCurrentThreadId());
        load_stack(what);
    }
}

/* hle_debug.c's wrapper of the clock tick sub_001A7450, before each tick: at 60 fps a fixed step (the clock's
 * fixed mode is any step up to 10 s) goes half to each frame */
void hppoa_ClockBeforeTick(uint32_t clk)
{
    if (clk != GAME_CLOCK) return;
    /* a tick with no frame shown since the last one is the loading loop, which presents now and then. Nothing holds it
     * to the frame rate as the console's vsync did, and each tick moves the clock by the minimum step at least: spinning
     * freely it ran game time far ahead of real time -- 127 s in a second of loading -- and the cutscene after it played
     * fast to catch up, its speech left behind. A clock ahead of real time also makes the tick's unsigned step a jump of
     * 100 ms. So while the loop runs the minimum step is 1 ms: the clock follows real time however fast the loop goes,
     * and the loop only yields the core to the loader thread instead of waiting out 16 ms a tick, which held the
     * loading screen to 20 fps. RECOMP_LOAD_PACE=1: the old wait */
    static DWORD last_ms, loading_until;
    static uint32_t last_swaps;
    uint32_t swaps = hppoa_SwapCount(), min = MEM32(clk + 0x18);
    DWORD now = GetTickCount();
    static int old_pace = -1;
    if (old_pace < 0) { const char *e = getenv("RECOMP_LOAD_PACE"); old_pace = e && atoi(e) == 1; }
    if (old_pace) {
        if (last_ms && swaps == last_swaps && min && min <= 100 && now - last_ms < min) Sleep(min - (now - last_ms));
    } else if (last_ms && swaps == last_swaps && !MEM32(clk + 0x14)) {
        if ((int)(loading_until - now) <= 0) g_loading_shown = 0;
        loading_until = now + 200;
        g_loading = 1;
        if (g_loading_shown++ < 4) load_stack("tick");
        Sleep(1);
    }
    last_ms = GetTickCount(), last_swaps = swaps;
    if (g_loading && (int)(loading_until - last_ms) <= 0) {
        g_loading = 0;
        if (g_min_title && hppoa_FrameRate() < 60) MEM32(clk + 0x18) = g_min_title;
    }
    /* the wall sneak at 30 fps (hle_debug.c): the title's own minimum step while it lasts, one 60 Hz frame after */
    if (hppoa_FrameRate() >= 60 && g_min_title > 1000 / 60) {
        uint32_t want = hppoa_Sneaking() ? g_min_title : 1000 / 60;
        if (MEM32(clk + 0x18) != want) MEM32(clk + 0x18) = want;
    }
    if (g_loading) MEM32(clk + 0x18) = 1;
    if (!g_fixed || g_fixed > 10000) return;
    uint32_t step = g_fixed;
    if (hppoa_FrameRate() >= 60) {
        uint32_t total = g_fixed + g_carry;
        step = total / 2, g_carry = total % 2;
        if (!step) step = 1;  /* 0 would switch the clock to real time */
    }
    MEM32(clk + 0x14) = step;
}
