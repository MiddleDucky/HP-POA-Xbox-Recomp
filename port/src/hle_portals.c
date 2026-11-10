/**
 * Every room reachable through doorways drawn, not only those whose doorway is on screen.
 *
 * Each frame sub_00052020 walks the rooms from the camera's (sub_00051DC0, recursive): for each doorway of a room, the
 * room behind is added (its contents tested against a screen rectangle, sub_00052640) if the camera is on the
 * doorway's front side (sub_00058170), the doorway is in the view (sub_0004E710: 2 outside), it projects to a
 * rectangle (sub_00051A60, min at +0 and max at +0x10, float4 each) and that overlaps the rectangle it was reached
 * through (sub_00051810, the overlap written out). A room behind a doorway just off screen was left out, then popped
 * in as the camera turned (2026-10-26). Here the three view tests pass for the walk's calls and the room behind takes
 * the whole rectangle it was reached through, so every room beyond a doorway the camera faces is drawn, its contents
 * still tested against the walk's view (160 degrees, hle_widescreen.c). The facing test stays: it keeps the walk from
 * turning back into the rooms it came from. RECOMP_DRAW_ALL_ROOMS=0 keeps the game's walk.
 *
 * Built into the recompile with --exclude-manual naming this file; gen/recomp_0001.c's bodies are renamed _gen by
 * hand until then.
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

/* the portal walk's return addresses after each test */
#define RET_IN_VIEW  0x00051F3Eu
#define RET_PROJECT  0x00051F91u
#define RET_OVERLAP  0x00051FE1u

static int draw_all(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_DRAW_ALL_ROOMS");
        on = !(e && e[0] == '0');
        fprintf(stderr, "[ROOMS] %s\n", on ? "every room beyond a doorway the camera faces is drawn"
                                           : "rooms drawn as the game decides (doorways on screen)");
    }
    return on;
}

/* (matrix, flags): 2 the doorway outside the view */
extern void sub_0004E710_gen(void);
void sub_0004E710(void)
{
    uint32_t ret = MEM32(g_esp);
    sub_0004E710_gen();
    if (ret == RET_IN_VIEW && g_eax == 2 && draw_all()) g_eax = 1;
}

/* (doorway, matrix, rectangle out): false when it projects to nothing */
extern void sub_00051A60_gen(void);
void sub_00051A60(void)
{
    uint32_t ret = MEM32(g_esp);
    sub_00051A60_gen();
    if (ret == RET_PROJECT && draw_all()) SET_LO8(g_eax, 1);
}

/* (rectangle, rectangle reached through, overlap out): cdecl, the arguments still on the stack after the call */
extern void sub_00051810_gen(void);
void sub_00051810(void)
{
    uint32_t ret = MEM32(g_esp), from = MEM32(g_esp + 8), out = MEM32(g_esp + 12);
    sub_00051810_gen();
    if (ret != RET_OVERLAP || !draw_all()) return;
    for (uint32_t i = 0; i < 32; i += 4) MEM32(out + i) = MEM32(from + i);
    SET_LO8(g_eax, 1);
}


/* Level streaming: a quick step back through a doorway held, not loaded at once. The streaming manager's update
 * (sub_00036010, this = manager) finds the player's room (sub_00051510) and, when it differs from the room it stored
 * at +0xC, stores the new one and calls sub_00034FF0(old room, new room), which loads that doorway's package,
 * Portal_<old>_<new> (sub_000519C0) over the one before. The two directions are different packages: RoomB_C 621 KB,
 * RoomC_B 1.3 KB. Stairs that straddle two rooms (the Marauder's Map, RoomB and G3) changed the room back and forth as
 * the player moved, and the next area's view came and went with each load. 9ac3a79 skipped every later crossing
 * between the same two rooms, either way, keeping the first one's package; but a real step back then kept the wrong
 * one: Night 2's lower dungeon showed the area beyond a doorway black (2026-11-01, RoomB_C and RoomD_C never loaded).
 * Now a crossing straight back through the doorway just loaded, within RECOMP_VIEW_HOLD_MS (default 1500) of that
 * load, is held: the manager's room goes back to the old one and sub_00034FF0's "nothing to do" (AL 1, no flags) is
 * returned, so the game asks again next frame; once the player has stayed past the hold, the crossing loads as the
 * game does, and stepping back first needs no load (the package in memory is the right one). RECOMP_KEEP_VIEWS=0:
 * every crossing loads at once, as the game. */
#include <string.h>
#include <ctype.h>

static int keep_views(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_KEEP_VIEWS");
        on = !(e && e[0] == '0');
    }
    return on;
}

static DWORD view_hold_ms(void)
{
    static long ms = -1;
    if (ms < 0) {
        const char *e = getenv("RECOMP_VIEW_HOLD_MS");
        ms = e && *e ? atol(e) : 1500;
        if (ms < 0) ms = 0;
    }
    return (DWORD)ms;
}

extern void sub_00034FF0_gen(void);
void sub_00034FF0(void)
{
    static uint32_t loaded_from, loaded_to;  /* the crossing whose package is in memory */
    static DWORD loaded_t;
    static uint32_t held_from, held_to;
    uint32_t self = g_ecx, from = MEM32(g_esp + 4), to = MEM32(g_esp + 8);
    DWORD now = GetTickCount();
    if (keep_views() && from && to && from == loaded_to && to == loaded_from && now - loaded_t < view_hold_ms()) {
        if (held_from != from || held_to != to)
            fprintf(stderr, "[STREAM] crossing %08X -> %08X: held (back through the doorway loaded %u ms ago)\n", from,
                    to, (unsigned)(now - loaded_t));
        held_from = from, held_to = to;
        MEM32(self + 0xC) = from;  /* the manager's room as before: it asks again next frame */
        SET_LO8(g_eax, 1);
        g_esp += 12;  /* ret 8 */
        return;
    }
    fprintf(stderr, "[STREAM] crossing %08X -> %08X: its view loaded%s\n", from, to,
            held_from == from && held_to == to ? " (after the hold)" : "");
    held_from = held_to = 0;
    loaded_from = from, loaded_to = to, loaded_t = now;
    sub_00034FF0_gen();
}

/* debug: who opens a level's packages. The kernel's path hook (kernel_path.c) is set here; for a path under
 * \data\packages\level\ the title's return addresses on the guest stack are logged, call sites only */
extern void (*g_xbox_path_hook)(const char *xbox_path);
static void level_path_hook(const char *path)
{
    if (!strstr(path, "\\packages\\level\\")) return;
    fprintf(stderr, "[STREAM] open %s from", path);
    int n = 0;
    for (uint32_t i = 0; i < 1024 && n < 16; i++) {
        uint32_t v = MEM32(g_esp + 4 * i);
        if (v < 0x11005 || v >= 0x264C64) continue;
        if (MEM8(v - 5) == 0xE8 || MEM8(v - 2) == 0xFF || MEM8(v - 3) == 0xFF || MEM8(v - 6) == 0xFF)
            fprintf(stderr, " %08X", v), n++;
    }
    fprintf(stderr, "\n");
}
__attribute__((constructor)) static void level_path_hook_init(void) { g_xbox_path_hook = level_path_hook; }

/* A material's lookup of an instance's own entry (sub_0003A260, stdcall: material, instance; ret 8): the material's
 * +0x80 set, its list at +0x58 linked through +0x58, the entry with +0x4C == instance (else the set itself). Night 2's
 * lower dungeon crashed twice in this walk (2026-10-28, 2026-11-01), in the n2lowdung_04 cutscene as the camera
 * crossed into room A and Portal_RoomA_B streamed in: a link read 0xB7F2E800 (a float, -2.9e-5), an entry whose memory
 * had been freed and reused while the material still listed it. Each link is checked here before the game's walk:
 * one outside the Xbox's memory ends the walk with no entry (the answer for an instance that has none) and logs
 * [STALE] with the material, instance, caller and the chain, so the next run says whose entry it was. */
static int guest_ram(uint32_t a)
{
    return (a >= 0x00010000u && a < 0x04000000u - 0x60u) || (a >= 0x80000000u && a < 0x84000000u - 0x60u);
}

extern void sub_0003A260_gen(void);
void sub_0003A260(void)
{
    uint32_t ret = MEM32(g_esp), mat = MEM32(g_esp + 4), inst = MEM32(g_esp + 8);
    uint32_t set = guest_ram(mat) ? MEM32(mat + 0x80) : 0, bad = 0, prev = set;
    int n = 0;
    if (set && !guest_ram(set)) bad = set, prev = mat;
    for (uint32_t e = set && !bad ? MEM32(set + 0x58) : 0; e && n < 4096; e = MEM32(e + 0x58), n++) {
        if (!guest_ram(e)) { bad = e; break; }
        prev = e;
    }
    if (!bad) { sub_0003A260_gen(); return; }
    static int lines;
    if (lines++ < 40) {
        fprintf(stderr, "[STALE] material %08X instance %08X from %08X: set %08X, link %d of its list %08X (in %08X), "
                "game time %u ms; chain:", mat, inst, ret, set, n, bad, prev, MEM32(0x0038BA6Cu + 4));
        int k = 0;
        for (uint32_t e = guest_ram(set) ? MEM32(set + 0x58) : 0; e && guest_ram(e) && k < 12; e = MEM32(e + 0x58), k++)
            fprintf(stderr, " %08X(key %08X +48 %08X +20 %08X)", e, MEM32(e + 0x4C), MEM32(e + 0x48), MEM32(e + 0x20));
        fprintf(stderr, "\n");
        fflush(stderr);
    }
    g_eax = 0;
    g_esp += 12;  /* ret 8 */
}
