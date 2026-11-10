/**
 * Debug wrappers (log only; every call still runs the generated body).
 *
 * RECOMP_CLOCK_TRACE=1 logs [CLOCK] once a second per clock object for the game clock tick sub_001A7450 (this =
 * ecx): +0 frame count, +4 time (ms), +8 last step (ms), +0xC ticks per ms (QueryPerformanceCounter divisor), +0x10
 * origin, +0x14 fixed step (ms, 0 = real time), +0x18 minimum step.
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
#include <string.h>

/* the register names (eax, esp, ...) the runtime macros expand to */
#define RECOMP_GENERATED_CODE
#include "recomp/gen/recomp_types.h"

static int clock_trace(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_CLOCK_TRACE");
        on = e && *e && *e != '0';
    }
    return on;
}

/* game clock tick, __thiscall (ecx = clock) */
extern void sub_001A7450_gen(void);
void hppoa_ClockBeforeTick(uint32_t clk);  /* hle_clock.c: 60 fps cutscenes */
uint32_t hppoa_SwapCount(void);            /* hle_d3d8.cpp */
void hppoa_TrainerAfterTick(uint32_t clk, uint32_t t0);  /* hle_trainer.c: game speed, god mode, frozen values */

/* the game clock's second: ticks by caller, frames presented, game time against real time. Logged every second with
 * RECOMP_CLOCK_TRACE, and by itself ([CLOCK!]) for a second with more ticks than frames presented while presenting */
static void clock_second(uint32_t clk, uint32_t caller)
{
    static uint32_t callers[4], ticks[4], other, swaps0, time0;
    static DWORD t0;
    int k;
    for (k = 0; k < 4 && callers[k] && callers[k] != caller; k++) {}
    if (k < 4) callers[k] = caller, ticks[k]++;
    else other++;
    DWORD now = GetTickCount();
    if (!t0) t0 = now, swaps0 = hppoa_SwapCount(), time0 = MEM32(clk + 4);
    if (now - t0 < 1000) return;
    uint32_t n = other, swaps = hppoa_SwapCount() - swaps0, game = MEM32(clk + 4) - time0;
    for (k = 0; k < 4; k++) n += ticks[k];
    if (clock_trace() || (swaps >= 10 && n > swaps + swaps / 5)) {
        fprintf(stderr, "[CLOCK%s] %u ms real: %u frames presented, %u ticks (", clock_trace() ? "" : "!",
                (unsigned)(now - t0), swaps, n);
        for (k = 0; k < 4 && callers[k]; k++) fprintf(stderr, "%s%08X x%u", k ? ", " : "", callers[k], ticks[k]);
        fprintf(stderr, "%s), game time +%u ms, step %u fixed %u min %u\n", other ? ", others" : "", game,
                MEM32(clk + 8), MEM32(clk + 0x14), MEM32(clk + 0x18));
    }
    memset(callers, 0, sizeof callers), memset(ticks, 0, sizeof ticks), other = 0;
    t0 = now, swaps0 = hppoa_SwapCount(), time0 = MEM32(clk + 4);
}

void sub_001A7450(void)
{
    uint32_t clk = g_ecx, caller = MEM32(g_esp);
    hppoa_ClockBeforeTick(clk);
    uint32_t t0 = MEM32(clk + 4);
    sub_001A7450_gen();
    hppoa_TrainerAfterTick(clk, t0);
    if (clk == 0x0038BA6Cu) clock_second(clk, caller);
}


/* The title's own diagnostics. Its printf (sub_0013F8E5, the CRT's, onto the guest stdout at 0x303CC0, which goes
 * nowhere) is what the debug print pointer [0x309440] holds: 188 calls such as the audio bank's "Bank not ready, when
 * trying to play event!" (sub_0015B570) and "Failed to load bank '%s'". Each message also goes to the run log as a
 * [GAME] line, formatted here from the guest's arguments (cdecl: [esp+4] the format, the values after it; %f and its
 * kin take 8 bytes, %s a guest string). RECOMP_GAME_PRINT=0 leaves them out. */
static int game_print(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_GAME_PRINT");
        on = !(e && e[0] == '0');
    }
    return on;
}

static void guest_str(uint32_t p, char *out, size_t n)
{
    size_t i = 0;
    if (p)
        for (; i + 1 < n; i++) {
            char c = (char)MEM8(p + (uint32_t)i);
            if (!c) break;
            out[i] = c;
        }
    out[i] = 0;
}

extern void sub_0013F8E5_gen(void);
void sub_0013F8E5(void)
{
    uint32_t ret = MEM32(g_esp), fmt_p = MEM32(g_esp + 4), arg = g_esp + 8;
    sub_0013F8E5_gen();
    if (!game_print()) return;
    char fmt[512], out[1024];
    guest_str(fmt_p, fmt, sizeof(fmt));
    size_t o = 0;
    for (const char *f = fmt; *f && o + 1 < sizeof(out);) {
        if (*f != '%') {
            out[o++] = *f++;
            continue;
        }
        if (f[1] == '%') {
            out[o++] = '%', f += 2;
            continue;
        }
        /* one conversion: flags, width, precision, size, then its letter */
        char spec[32];
        size_t s = 0;
        spec[s++] = *f++;
        while (*f && strchr("-+ #0123456789.", *f) && s < 20) spec[s++] = *f++;
        while (*f && strchr("hlLIwq", *f)) f++;  /* sizes: everything here is 32 bit but %f's 64 */
        char conv = *f ? *f++ : 0;
        if (!conv) break;
        char piece[600];
        if (strchr("fFeEgGaA", conv)) {
            uint64_t bits = (uint64_t)MEM32(arg) | (uint64_t)MEM32(arg + 4) << 32;
            double d;
            memcpy(&d, &bits, 8);
            arg += 8;
            spec[s++] = conv, spec[s] = 0;
            snprintf(piece, sizeof(piece), spec, d);
        } else if (conv == 's') {
            char str[512];
            guest_str(MEM32(arg), str, sizeof(str));
            arg += 4;
            spec[s++] = 's', spec[s] = 0;
            snprintf(piece, sizeof(piece), spec, str);
        } else {
            uint32_t v = MEM32(arg);
            arg += 4;
            spec[s++] = conv == 'p' ? 'X' : conv, spec[s] = 0;
            snprintf(piece, sizeof(piece), spec, conv == 'c' ? (unsigned)(v & 0xFF) : v);
        }
        for (const char *p = piece; *p && o + 1 < sizeof(out);) out[o++] = *p++;
    }
    while (o && (out[o - 1] == '\n' || out[o - 1] == '\r')) o--;
    out[o] = 0;
    if (o) fprintf(stderr, "[GAME] %s (from %08X)\n", out, ret);
}

/* RECOMP_SNEAK_TRACE=1: Harry's wall sneak (shimmy) at 60 fps. At 30 fps (the console's rate) he moves along every
 * ledge; at 60 he can stop part way and push neither left nor right, so a check in the sneak code answers differently
 * with the half-size steps. Each function below logs [SNEAK] when its result (eax) changes for the same caller, with
 * the object (ecx) and the frame; the collision helpers (sub_000D2170, sub_000D46C0, the corner probe) only while a
 * sneak function is on the stack. Functions: the sneak states sub_000FF510 .. sub_00100790 (called through the
 * character's state table, WallSneak* tuning values at 0x36A960 .. 0x36AF58), the wall probe sub_000A9320 with its
 * penetration tolerance (WallSneakPenetrationIota) and corner check sub_000A8E70 (WallSneakCornerCheckPenetrationIota)
 * and their callers sub_000A9690 / sub_000A96A4, sub_000A9140 (from sub_00100630). */
static int sneak_trace(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_SNEAK_TRACE");
        on = e && *e && *e != '0';
        if (on) fprintf(stderr, "[SNEAK] trace on: results of the wall sneak code as they change\n");
    }
    return on;
}

static int g_sneak_depth;
static uint32_t g_sneak_lines;

/* calls per second of each traced function ([SNEAKCALLS], printed each second in which the idle or move state ran):
 * which gate the idle state stops at while Harry is stuck, which change-only lines cannot show */
#define SNEAK_FNS 32
static struct { const char *name; unsigned n; } g_sneak_calls[SNEAK_FNS];
static void sneak_count(const char *name)
{
    static DWORD sec;
    DWORD now = GetTickCount();
    if (now / 1000 != sec) {
        char line[1024];
        int n = 0, any = 0;
        n += snprintf(line, sizeof(line), "[SNEAKCALLS] %u ms last second:", (unsigned)now);
        for (int i = 0; i < SNEAK_FNS && g_sneak_calls[i].name; i++) {
            if (!g_sneak_calls[i].n) continue;
            if (n < (int)sizeof(line) - 40) n += snprintf(line + n, sizeof(line) - (size_t)n, " %s x%u", g_sneak_calls[i].name + 4, g_sneak_calls[i].n);
            if (!strcmp(g_sneak_calls[i].name, "sub_000FFA80") || !strcmp(g_sneak_calls[i].name, "sub_000FFFE0")) any = 1;
            g_sneak_calls[i].n = 0;
        }
        if (any) fprintf(stderr, "%s\n", line);
        sec = now / 1000;
    }
    for (int i = 0; i < SNEAK_FNS; i++) {
        if (!g_sneak_calls[i].name) g_sneak_calls[i].name = name;
        if (g_sneak_calls[i].name == name) { g_sneak_calls[i].n++; return; }
    }
}

static void sneak_log(const char *name, int helper, uint32_t ret, uint32_t self, uint32_t res)
{
    /* the last result per (function, caller), a small table */
    static struct { const char *name; uint32_t ret, res; int used; } last[256];
    static DWORD second;
    if (helper && !g_sneak_depth) return;
    uint32_t h = (uint32_t)(((uintptr_t)name >> 4) ^ ret * 2654435761u) & 255u;
    for (int i = 0; i < 256; i++, h = (h + 1) & 255u) {
        if (!last[h].used) {
            last[h].used = 1, last[h].name = name, last[h].ret = ret, last[h].res = res ^ 1u;  /* first call logs */
        }
        if (last[h].name == name && last[h].ret == ret) break;
    }
    if (last[h].res == res) return;
    last[h].res = res;
    DWORD now = GetTickCount();
    if (now / 1000 != second) second = now / 1000, g_sneak_lines = 0;
    if (++g_sneak_lines > 400) return;  /* a flood: 400 lines a second at most */
    fprintf(stderr, "[SNEAK] %u ms %s from %08X this %08X -> %08X%s\n", (unsigned)now, name, ret, self, res,
            g_sneak_depth ? "" : " (outside sneak)");
}

/* the sneak state object's fields that a sneak function changed: 0x1800 bytes at ecx (Harry's character: the sneak's
 * wall normal and facing are at +0x14D4 .. +0x14EC), compared with what the last
 * sneak call left there (one object at a time), each changed dword as hex and as a float */
static void sneak_diff(const char *name, uint32_t self)
{
    static uint32_t obj, snap[0x600];
    if (!self) return;
    if (self != obj) {
        obj = self;
        for (int i = 0; i < 0x600; i++) snap[i] = MEM32(self + (uint32_t)i * 4);
        fprintf(stderr, "[SNEAKOBJ] %s: object %08X\n", name, self);
        return;
    }
    char line[6144];
    int n = snprintf(line, sizeof(line), "[SNEAKOBJ] %u ms %s:", (unsigned)GetTickCount(), name), any = 0;
    for (int i = 0; i < 0x600 && n < (int)sizeof(line) - 64; i++) {
        uint32_t v = MEM32(self + (uint32_t)i * 4);
        if (v == snap[i]) continue;
        float fo, fn;
        memcpy(&fo, &snap[i], 4), memcpy(&fn, &v, 4);
        n += snprintf(line + n, sizeof(line) - (size_t)n, " +%03X %08X>%08X (%g>%g)", i * 4, snap[i], v, fo, fn);
        snap[i] = v, any = 1;
    }
    if (any && ++g_sneak_lines <= 400) fprintf(stderr, "%s\n", line);
}

/* 60 fps: the wall sneak at the console's 30. At 60 fps Harry can stop part way along a ledge and push neither way, with
 * a stutter at the spot's edges; at 30 fps (and on the console) he never does. The sneak's idle and move states
 * (sub_000FFA80, sub_000FFFE0) run each frame the player's character is on a wall: while they do, frames are paced to
 * two vblanks (hle_d3d8.cpp's Pace) and the game clock takes the title's own 33 ms minimum step (hle_clock.c), so the
 * sneak runs exactly as on the console. Only in the Carpe Retractum challenge (the level folder at 0x30FB44, the
 * title's own "...\\level\\carperetractum_cpe\\", built into each package path by sub_000519C0), where Harry got
 * stuck; every other wall sneak stays at 60. RECOMP_SNEAK_30=0 keeps 60 fps there too. */
static DWORD g_sneak_seen;

/* the level loaded is the Carpe Retractum challenge: its folder name in the level path at 0x30FB44 */
static int in_carpe_retractum(void)
{
    static const char want[] = "carperetractum";
    size_t k = 0;
    for (uint32_t i = 0; i < 0x100; i++) {
        char c = (char)MEM8(0x30FB44 + i);
        if (!c) return 0;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        k = c == want[k] ? k + 1 : (c == want[0] ? 1 : 0);
        if (!want[k]) return 1;
    }
    return 0;
}
static void sneak_note(uint32_t self)
{
    uint32_t table = MEM32(0x3660C8);
    if (!table) return;
    uint32_t h = MEM16(table + 0xE6);  /* player 0's character handle (sub_00070F20) */
    if (h && MEM32(0x3BDCF0 + h * 4) == self) g_sneak_seen = GetTickCount();
}
int hppoa_Sneaking(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_SNEAK_30");
        on = !(e && e[0] == '0');
    }
    static int was;
    int now = on && g_sneak_seen && GetTickCount() - g_sneak_seen < 150 && in_carpe_retractum();
    if (now != was) {
        fprintf(stderr, "[SNEAK30] %s\n", now ? "wall sneak: 30 fps as on the console" : "wall sneak over: back to 60 fps");
        was = now;
    }
    return now;
}

#define SNEAK_WRAP(addr, helper)                                        \
    extern void sub_##addr##_gen(void);                                 \
    void sub_##addr(void)                                               \
    {                                                                   \
        if (!(helper)) sneak_note(g_ecx);                               \
        if (!sneak_trace()) {                                           \
            sub_##addr##_gen();                                         \
            return;                                                     \
        }                                                               \
        uint32_t ret = MEM32(g_esp), self = g_ecx;                      \
        if (!(helper) || g_sneak_depth) sneak_count("sub_" #addr);      \
        if (!(helper)) g_sneak_depth++;                                 \
        sub_##addr##_gen();                                             \
        if (!(helper)) g_sneak_depth--;                                 \
        sneak_log("sub_" #addr, helper, ret, self, g_eax);              \
        if (!(helper)) sneak_diff("sub_" #addr, self);                  \
    }

SNEAK_WRAP(000FF510, 0)
SNEAK_WRAP(000FF5F0, 0)
SNEAK_WRAP(000FFA80, 0)
SNEAK_WRAP(000FFFE0, 0)
SNEAK_WRAP(00100001, 0)
SNEAK_WRAP(00100630, 0)
SNEAK_WRAP(00100790, 0)
SNEAK_WRAP(000A9140, 1)
SNEAK_WRAP(000A9320, 1)
SNEAK_WRAP(000A8E70, 1)
SNEAK_WRAP(000A9690, 1)
SNEAK_WRAP(000A96A4, 1)
SNEAK_WRAP(000D2170, 1)
SNEAK_WRAP(000D46C0, 1)
/* the idle state's gates (sub_000FFA80): sub_000BA820 = (sub_00073C50() == 1) stops it, sub_000A2C00 compares the
 * pushed direction with the blocked one (+0x14F0), sub_000FF330 / sub_000FF420 are its two outcomes */
SNEAK_WRAP(000BA820, 1)
SNEAK_WRAP(00073C50, 1)
SNEAK_WRAP(000A2C00, 1)
SNEAK_WRAP(000FF330, 1)
SNEAK_WRAP(000FF420, 1)

/* the character's state request (state id, flags, blend): the idle state asks for 0x4E, the sneak move, with 0x6001 */
extern void sub_00075F60_gen(void);
void sub_00075F60(void)
{
    if (!sneak_trace() || !g_sneak_depth) { sub_00075F60_gen(); return; }
    uint32_t st = MEM32(g_esp + 4), fl = MEM32(g_esp + 8), bl = MEM32(g_esp + 12), ret = MEM32(g_esp), self = g_ecx;
    sneak_count("sub_00075F60");
    sub_00075F60_gen();
    static uint32_t last_st, last_res, last_ret;
    if (st != last_st || g_eax != last_res || ret != last_ret)
        fprintf(stderr, "[SNEAK] %u ms state request %X flags %X blend %X on %08X from %08X -> %08X\n", (unsigned)GetTickCount(),
                st, fl, bl, self, ret, g_eax);
    last_st = st, last_res = g_eax, last_ret = ret;
}

/* RECOMP_SPEECH_TRACE=1: a cutscene's lines and the speakers' faces ([SPEECH]). In D3CARPE_03 (Harry in the cage)
 * Ron's "Are you all right up there?" (SV3_RON_D3CARPER_024) is dropped at 30 fps and on the console, and at 60 fps
 * his face keeps talking after it until Harry's line. A speech channel starts a line with sub_00070380 (refused when
 * sub_000A6E80 finds the speaker among the three slots at 0x372CBC), which first cleans up the channel's last line with
 * sub_00070230: the speaker's face back to neutral (sub_000A7300 with "") and the sound stopped. Events reach the
 * channel through sub_000A6B40 (type 0..15, two values); one of them, through sub_000A6A20, also cleans up when it
 * names the speaker. Logged: each start with its speaker, refusals, cleanups with the sound handle, every face
 * animation asked for by name, and each event with its type and values. */
static int speech_trace(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_SPEECH_TRACE");
        on = e && *e && *e != '0';
        if (on) fprintf(stderr, "[SPEECH] trace on: speech lines, refusals, cleanups, faces and channel events\n");
    }
    return on;
}

/* Enhancement: no "time to turn in" nagging. Once a day's tasks are done the level scripts' Go To Bed speech has the
 * trio say, again and again, the lines of the trio's conversation bank (0x1E9, eAudioCategory_HryRonHer_Conversation,
 * data/english/speech/audiohrhconvs.ids) SV3_*_HRHENDDAY_001..003: "I can't think of anything else to do.", "There's
 * nothing more to do, I think it's time to turn in.", "I want to turn in.", "I'm really tired." (ids 139-141 Hermione,
 * 612-614 Harry, 1051-1053 Ron). The same speech's suggestions stay (go and find Girding Potion, visit Fred and
 * George: 004 on). On night 1 Neville, taken back to the common room, keeps asking "Can we go back now, please?",
 * "I just want to go to bed, can we go now?" (S_NevilleToCommonBank, SV3_NEV_N1MUGGLE_016..019: bank 0x1E3,
 * eAudioCategory_Night1_Conversation, ids 65-68). A speech channel's start of one of them (sub_00070380(?, bank, id,
 * speaker, ...)) is refused as the title refuses a line whose speaker is busy (AL 0). RECOMP_TURN_IN_LINES=1 keeps
 * them. */
int hppoa_QuietTurnIn(void);  /* app.cpp: F10 Enhancements > Leave out "time to turn in" lines */
static int end_day_line(uint32_t bank, uint32_t id)
{
    static int keep = -1;
    if (keep < 0) {
        const char *e = getenv("RECOMP_TURN_IN_LINES");
        keep = e && e[0] == '1';
    }
    if (keep || !hppoa_QuietTurnIn()) return 0;
    if (bank == 0x1E9) {
        if ((id >= 139 && id <= 141) || (id >= 612 && id <= 614) || (id >= 1051 && id <= 1053)) return 1;
        /* the trio's let's-go nagging, HRHPRSUIT_001, 002, 020 ("Can we go now?", "We need to go.", "C'mon let's
         * go.", "We're wasting too much time.", "What are we doing hanging around here?"; the same speech's way
         * hints, "It isn't this way" / "This is the way" / "wrong floor", stay), and HRHRETDORM_001..003 ("I'm
         * tired. Can we go back to the dormitory now?", "It's time we went back.") */
        static const uint16_t kGoLines[] = {328, 329, 347, 786, 787, 805, 1244, 1245, 1263,
                                            411, 412, 413, 863, 864, 865, 1324, 1325, 1326};
        for (size_t i = 0; i < sizeof kGoLines / sizeof kGoLines[0]; i++)
            if (id == kGoLines[i]) return 1;
        return 0;
    }
    /* night 1: Neville's go-to-bed lines (65-68) and Harry's "Can we go now?" (SV3_HRY_N1MUGGLE_017, 39) */
    if (bank == 0x1E3) return (id >= 65 && id <= 68) || id == 39;
    return 0;
}

extern void sub_00070380_gen(void);
static void lips_line_started(uint32_t channel);
void sub_00070380(void)
{
    if (end_day_line(MEM32(g_esp + 8), MEM32(g_esp + 12))) {
        static unsigned told;
        if (told++ < 20)
            fprintf(stderr, "[NAG] %u ms line %X/%u left out (F10 Enhancements > Leave out \"time to turn in\" lines)\n",
                    (unsigned)GetTickCount(), MEM32(g_esp + 8), MEM32(g_esp + 12));
        g_eax &= 0xFFFFFF00u;  /* AL 0: not started */
        g_esp += 28;           /* ret 24 */
        return;
    }
    if (!speech_trace()) {
        uint32_t ch0 = g_ecx;
        sub_00070380_gen();
        if (g_eax & 0xFF) lips_line_started(ch0);
        return;
    }
    uint32_t ch = g_ecx, ret = MEM32(g_esp), a[6];
    for (int i = 0; i < 6; i++) a[i] = MEM32(g_esp + 4 + (uint32_t)i * 4);
    uint32_t speaker = a[3] ? MEM16(a[3] + 4) : 0;
    sub_00070380_gen();
    if (g_eax & 0xFF) lips_line_started(ch);
    if (g_eax & 0xFF) fprintf(stderr, "[SPEECH] countdown %u ms\n", MEM32(ch + 4));
    fprintf(stderr, "[SPEECH] %u ms start channel %08X speaker %u -> %s (args %08X %08X %08X %08X %08X %08X, from %08X)\n",
            (unsigned)GetTickCount(), ch, speaker, (g_eax & 0xFF) ? "playing" : "REFUSED", a[0], a[1], a[2], a[3], a[4],
            a[5], ret);
}

extern void sub_00070230_gen(void);
void sub_00070230(void)
{
    if (speech_trace())
        fprintf(stderr, "[SPEECH] %u ms cleanup channel %08X speaker %u sound %08X (from %08X)\n", (unsigned)GetTickCount(),
                g_ecx, MEM16(g_ecx + 0x1C), MEM32(g_ecx + 8), MEM32(g_esp));
    sub_00070230_gen();
}

extern void sub_000A6E80_gen(void);
void sub_000A6E80(void)
{
    uint32_t who = MEM16(g_esp + 4), ret = MEM32(g_esp);
    sub_000A6E80_gen();
    if (speech_trace() && (g_eax & 0xFF))
        fprintf(stderr, "[SPEECH] %u ms speaker %u is in the busy slots %u %u %u: line refused (from %08X)\n",
                (unsigned)GetTickCount(), who, MEM16(0x372CBC), MEM16(0x372CBE), MEM16(0x372CC0), ret);
}

/* Lips that stop with the voice. A line's face is the generic talking one (no line has its own), held until the speech
 * channel's countdown runs out: the channel's +4, ms left (sub_00212110 takes each frame's time off, sub_00212130 asks
 * whether any is left), and when none is, the channel's update sub_0006FE40 ends the line itself and puts the speaker's
 * face back to neutral (sub_000A7300 with "", from 0x7001F). That countdown is longer than the words: Ron's "Are you
 * all right up there?" (D3CARPE_03) talked on in silence. A line that starts on a channel (sub_00070380) arms the
 * DirectSound mixer (hle_dsound.c), which picks the line's voice; once that voice has been quiet 0.4 s, the channel's
 * countdown is set to its end before the channel's next update, so the title ends the line as it always does (calling
 * the neutral face from here instead released the face twice and crashed). When no voice was found, the countdown
 * runs as the title set it. RECOMP_LIPS_FOLLOW_VOICE=0 leaves every line to its countdown. */
void hppoa_SpeechArm(void);       /* hle_dsound.c */
int hppoa_SpeechSilentMs(void);  /* hle_dsound.c: -1 with no voice found */
static uint32_t g_lips_channel;  /* the channel whose line is followed, 0 none */
static uint32_t g_lips_speaker;  /* its speaker's handle (channel +0x1C once the line's face is on), for the log */
static DWORD g_lips_start;

static int lips_follow(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_LIPS_FOLLOW_VOICE");
        on = !(e && e[0] == '0');
    }
    return on;
}

/* sub_00070380 (wrapped above for the trace) calls this when a line starts playing on a channel */
static void lips_line_started(uint32_t channel)
{
    if (!lips_follow()) return;
    g_lips_channel = channel, g_lips_speaker = MEM16(channel + 0x1C), g_lips_start = GetTickCount();
    hppoa_SpeechArm();
}

extern void sub_000A7300_gen(void);
void sub_000A7300(void)
{
    if (speech_trace()) {
        char name[96];
        guest_str(MEM32(g_esp + 4), name, sizeof(name));
        fprintf(stderr, "[SPEECH] %u ms face of %08X: \"%s\"%s (from %08X)\n", (unsigned)GetTickCount(), g_ecx, name,
                name[0] ? "" : " (neutral)", MEM32(g_esp));
    }
    sub_000A7300_gen();
}

/* the speech channel's update, each frame: the followed line's countdown ended first when its voice has gone quiet */
extern void sub_0006FE40_gen(void);
void sub_0006FE40(void)
{
    uint32_t ch = g_ecx;
    if (ch && ch == g_lips_channel && lips_follow()) {
        if (!MEM32(ch + 4)) {
            g_lips_channel = 0;  /* the line ended on its own (a new line on the channel re-arms) */
        } else {
            int quiet = hppoa_SpeechSilentMs();
            if (!g_lips_speaker) g_lips_speaker = MEM16(ch + 0x1C);
            if (quiet >= 400 && GetTickCount() - g_lips_start >= 500) {
                fprintf(stderr, "[LIPS] channel %08X speaker %u: the voice went quiet %d ms ago, %u ms into the line; "
                        "its countdown ended (%u ms were left)\n", ch, g_lips_speaker, quiet,
                        (unsigned)(GetTickCount() - g_lips_start), MEM32(ch + 4));
                MEM32(ch + 4) = 0;
                g_lips_channel = 0;
            }
        }
    }
    sub_0006FE40_gen();
}

extern void sub_000A6B40_gen(void);
void sub_000A6B40(void)
{
    static DWORD sec;
    static unsigned n;
    DWORD now = GetTickCount();
    if (now / 1000 != sec) sec = now / 1000, n = 0;
    if (speech_trace() && ++n <= 100)  /* at most 100 event lines a second */
        fprintf(stderr, "[SPEECH] %u ms event %u (%08X %08X) on %08X (from %08X)\n", (unsigned)GetTickCount(),
                MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(g_esp + 12), g_ecx, MEM32(g_esp));
    sub_000A6B40_gen();
}

/* RECOMP_STATE_TRACE=1: [STATE], each character state change (sub_00075EA0, thiscall, the state machine at character
 * +0xE78, new state) with the title's return addresses. Same format as the console recording over xbdm, so a Carpe
 * Retractum pull at a Red Cap's shield (states 97, 353, 3 from sub_000FB620..sub_000FBBC0) can be compared line by line. */
extern void sub_00075EA0_gen(void);
void sub_00075EA0(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("RECOMP_STATE_TRACE"); on = e && e[0] == '1'; }
    if (on) {
        uint32_t sp = g_esp;
        fprintf(stderr, "[STATE] %u ms ecx %08X -> %u from", (unsigned)GetTickCount(), g_ecx, MEM32(sp + 4));
        int n = 0;
        for (uint32_t i = 0; i < 0x180 / 4 && n < 7; i++) {
            uint32_t v = MEM32(sp + 4 * i);
            if (v >= 0x11005 && v < 0x264C64 && (v & 0xFFFF0000u) != 0x00040000u) fprintf(stderr, " %06X", v), n++;
        }
        fputc('\n', stderr);
    }
    sub_00075EA0_gen();
}

/* RECOMP_CARPE_TRACE=1: [CARPE], each frame of Harry's Carpe Retractum pull. sub_000FC200 reads the left stick's vector through
 * sub_000AFC10 (object +0x14: x, 0, y) and moves to the tug (sub_000FCC80, then sub_000FC2C0) when y <= -0.99, 0x2D3BDC. Console and
 * 30 fps go there in about 0.13 s; 60 fps stays. The line gives y as the state reads it, and the pad before and after hle_input.c's
 * stick compensation. */
extern volatile short g_stick_raw[2], g_stick_scaled[2];
uint32_t hppoa_SwapCount(void);
extern void sub_000AFC10_gen(void);
void sub_000AFC10(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("RECOMP_CARPE_TRACE"); on = e && e[0] == '1'; }
    uint32_t ret = MEM32(g_esp), out = MEM32(g_esp + 4);
    sub_000AFC10_gen();
    if (on && (ret == 0xFC223 || ret == 0xFC2E3))
        fprintf(stderr, "[CARPE] %u ms f%u %s x %.4f y %.4f raw %d %d scaled %d %d\n", (unsigned)GetTickCount(), hppoa_SwapCount(),
                ret == 0xFC223 ? "FC200" : "FC2C0", MEMF(out), MEMF(out + 8), g_stick_raw[0], g_stick_raw[1], g_stick_scaled[0], g_stick_scaled[1]);
}
