/**
 * The trainer's game side (the window is renderer/app/trainer.cpp, F6). Everything here comes from the title's own code:
 *
 *   tuning variables  657 named values the title registers at start (trainer_vars.h, port/tools/trainer_vars.py):
 *                     NoClip, DebugMove, DebugRender, the targeting and camera debug switches, gravity, jump
 *                     heights, spell timings, item stores. Each is a plain global the game reads.
 *   debug menu        the front end asks the native DebugMenu (sub_00020310, an Apt FSCommand) and opens
 *                     POA_DEBUGMENU.swf (on the disc, gui/poa_debugmenu.big) when it answers "true" and the key
 *                     pressed is 'G' (RequestDebugMenu in POA_STARTUP_XBOX / POA_FE). The retail native always
 *                     answers "false". The pads send the key through the table at 0x30BFA0 (code 'g', the
 *                     title's button id 4). The menu's own natives (DM_*) are still in the title: chapter, room and
 *                     debug start point selection.
 *   characters        the character table [0x3660C8]: slot n's handle at +n*36+0xC, the object at
 *                     [0x3BDCF0 + handle*4]; the controlled one is player 0's handle at +0xE6 (sub_00070F20).
 *                     Slots 0 Harry, 1 Ron, 2 Hermione, 3 and 4 the other Ron and Hermione (the HUD's choice).
 *   health            the character's component 0x2D (sub_001BC360 over the list at +0x28): +0x34 now, +0x3C most.
 *   items             per character: one signed byte per item id at [character+0x6D4] + id; the title's shared
 *                     counts (beans, house points, cards, Folio Bruti pages): int16 at 0x3660E8 + id*2, 106 of
 *                     them (sub_00070640 / sub_00070710). Names: 32 bytes each at [[0x3660CC]+4] + id*32.
 *   game speed        the game clock 0x38BA6C (hle_clock.c): each tick's step scaled.
 *
 * Writes from the window go straight to guest memory; what has to run on the game's thread (Apt calls, keeping a
 * value frozen, god mode) is done from the clock tick (hle_debug.c) and the front end's pad poll.
 *
 * Built into the recompile with --exclude-manual naming this file; gen/recomp_0000.c's sub_00020310 and
 * gen/recomp_0011.c's sub_001ABDD0 are renamed _gen by hand until then.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

/* the register names (eax, esp, ...) the runtime macros expand to */
#define RECOMP_GENERATED_CODE
#include "recomp/gen/recomp_types.h"

#include "trainer.h"

#define GAME_CLOCK   0x0038BA6Cu
#define CHAR_TABLE   0x003660C8u
#define ITEM_DB      0x003660CCu
#define SHARED_ITEMS 0x003660E8u
#define OBJ_HANDLES  0x003BDCF0u
#define APT_SETVAR   0x0038A2F8u  /* the Apt variable setter (name, value), cdecl */
#define APT_KEY      0x00165910u  /* an Apt key event (code, 0 down / 1 up, pad) */
#define STR_TRUE     0x002C8730u
#define STR_DEBUGMENU 0x002C9360u
#define KEY_DEBUG    0x47u        /* 'G', the code RequestDebugMenu compares (the pads' table at 0x30BFA0 sends 'g') */

recomp_func_t recomp_lookup(uint32_t xbox_va);

static const HppoaTrainerVar g_vars[] = {
#define TV_BOOL  HPPOA_TV_BOOL
#define TV_FLOAT HPPOA_TV_FLOAT
#define TV_INT   HPPOA_TV_INT
#define TV_UINT  HPPOA_TV_UINT
#include "trainer_vars.h"
};

HppoaTrainer g_hppoa_trainer = {.speed = 1.0f};

int hppoa_TrainerVarCount(void) { return (int)(sizeof g_vars / sizeof g_vars[0]); }
const HppoaTrainerVar *hppoa_TrainerVar(int i) { return &g_vars[i]; }

/* ── guest memory, checked (the window reads pointers the game may be changing) ─── */

int hppoa_GuestOk(uint32_t va, uint32_t len)
{
    if (va < 0x10000u || va + len < va) return 0;
    MEMORY_BASIC_INFORMATION mi;
    const char *p = (const char *)XBOX_PTR(va);
    if (!VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT) return 0;
    if (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return p + len <= (const char *)mi.BaseAddress + mi.RegionSize;
}

void *hppoa_GuestPtr(uint32_t va) { return (void *)XBOX_PTR(va); }

uint32_t hppoa_GuestRead32(uint32_t va) { return hppoa_GuestOk(va, 4) ? MEM32(va) : 0; }

/* ── characters, health, items ───────────────────────────────────────────── */

static uint32_t object_of(uint32_t handle)
{
    if (!handle || handle >= 0x1000u) return 0;
    return hppoa_GuestRead32(OBJ_HANDLES + handle * 4);
}

uint32_t hppoa_TrainerCharacter(int slot)
{
    uint32_t tab = hppoa_GuestRead32(CHAR_TABLE);
    if (!tab || !hppoa_GuestOk(tab + (uint32_t)slot * 36 + 0xC, 2)) return 0;
    return object_of(MEM16(tab + (uint32_t)slot * 36 + 0xC));
}

uint32_t hppoa_TrainerPlayer(void)
{
    uint32_t tab = hppoa_GuestRead32(CHAR_TABLE);
    if (!tab || !hppoa_GuestOk(tab + 0xE6, 2)) return 0;
    return object_of(MEM16(tab + 0xE6));
}

/* the character's health component (type 0x2D in the component list at +0x28: count at +8, 8-byte entries
 * {type << 16 | index, object} from [+0xC]) */
uint32_t hppoa_TrainerHealth(uint32_t chr)
{
    uint32_t list = chr ? hppoa_GuestRead32(chr + 0x28) : 0;
    if (!list || !hppoa_GuestOk(list, 0x10)) return 0;
    uint32_t n = MEM32(list + 8), e = MEM32(list + 0xC);
    if (n > 256 || !hppoa_GuestOk(e, n * 8)) return 0;
    for (uint32_t i = 0; i < n; i++, e += 8)
        if ((MEM32(e) & 0xFFFF0000u) == 0x2D0000u) {
            uint32_t h = MEM32(e + 4);
            return hppoa_GuestOk(h, 0x40) ? h : 0;
        }
    return 0;
}

uint32_t hppoa_TrainerInventory(uint32_t chr)
{
    uint32_t inv = chr ? hppoa_GuestRead32(chr + 0x6D4) : 0;
    return inv && hppoa_GuestOk(inv, HPPOA_ITEM_IDS) ? inv : 0;
}

uint32_t hppoa_TrainerSharedItems(void) { return SHARED_ITEMS; }

int hppoa_TrainerItemName(int id, char *out, int len)
{
    out[0] = 0;
    uint32_t db = hppoa_GuestRead32(ITEM_DB), names = db ? hppoa_GuestRead32(db + 4) : 0;
    uint32_t at = names + (uint32_t)id * 32;
    if (!names || !hppoa_GuestOk(at, 32)) return 0;
    int i = 0;
    for (; i < len - 1 && i < 31; i++) {
        char c = (char)MEM8(at + (uint32_t)i);
        if (!c) break;
        if (c < 32 || c > 126) { i = 0; break; }
        out[i] = c;
    }
    out[i] = 0;
    return i;
}

int hppoa_TrainerInGame(void) { return hppoa_TrainerPlayer() != 0; }

/* ── game thread ─────────────────────────────────────────────────────────── */

/* a recompiled function called from here on the game's thread: the arguments pushed right to left, then a return
 * address, as `call` leaves them */
static uint32_t call_guest(uint32_t fn, int n, const uint32_t *args)
{
    recomp_func_t f = recomp_lookup(fn);
    if (!f) {
        fprintf(stderr, "[TRAINER] no recompiled function at %08X\n", fn);
        return 0;
    }
    /* every register as the hooked function left it: its caller may read eax */
    uint32_t esp = g_esp, ebx = g_ebx, esi = g_esi, edi = g_edi, ecx = g_ecx, edx = g_edx, eax = g_eax;
    for (int i = n; i-- > 0;) {
        g_esp -= 4;
        MEM32(g_esp) = args[i];
    }
    g_esp -= 4;
    MEM32(g_esp) = 0;
    f();
    uint32_t r = g_eax;
    g_esp = esp, g_ebx = ebx, g_esi = esi, g_edi = edi, g_ecx = ecx, g_edx = edx, g_eax = eax;
    return r;
}

static void apply_freezes(void)
{
    HppoaTrainer *t = &g_hppoa_trainer;
    for (int i = 0; i < t->freeze_count; i++) {
        HppoaFreeze *z = &t->freezes[i];
        if (!z->va || !hppoa_GuestOk(z->va, z->size)) continue;
        if (z->size == 1) MEM8(z->va) = (uint8_t)z->value;
        else if (z->size == 2) MEM16(z->va) = (uint16_t)z->value;
        else MEM32(z->va) = z->value;
    }
}

static void god_mode(void)
{
    for (int s = 0; s < HPPOA_CHARACTER_SLOTS; s++) {
        uint32_t h = hppoa_TrainerHealth(hppoa_TrainerCharacter(s));
        if (h && MEMF(h + 0x3C) > 0.0f && MEMF(h + 0x34) < MEMF(h + 0x3C)) MEMF(h + 0x34) = MEMF(h + 0x3C);
    }
}

/* hle_debug.c's clock tick wrapper, after each tick; t0 is the clock's time before it */
void hppoa_TrainerAfterTick(uint32_t clk, uint32_t t0)
{
    if (clk != GAME_CLOCK) return;
    HppoaTrainer *t = &g_hppoa_trainer;
    float speed = t->speed;
    if (speed > 0.0f && (speed < 0.99f || speed > 1.01f)) {
        uint32_t step = MEM32(clk + 4) - t0;
        if (step < 10000u) {
            static float carry;
            float want = (float)step * speed + carry;
            uint32_t scaled = (uint32_t)want;
            carry = want - (float)scaled;
            MEM32(clk + 4) = t0 + scaled;
            MEM32(clk + 8) = scaled;
        }
    }
    if (t->god_mode) god_mode();
    apply_freezes();
}

/* the native DebugMenu (FSCommand:DebugMenu): the retail body answers "false"; with the trainer's switch on the
 * answer is "true", as a development build's */
extern void sub_00020310_gen(void);
void sub_00020310(void)
{
    sub_00020310_gen();
    if (g_hppoa_trainer.debug_menu) {
        uint32_t args[2] = {STR_DEBUGMENU, STR_TRUE};
        call_guest(MEM32(APT_SETVAR), 2, args);
        fprintf(stderr, "[TRAINER] DebugMenu answered \"true\"\n");
    }
}

/* the front end's pad poll (Apt key events from the controllers): the trainer's "open the debug menu" presses the
 * debug key, down this frame and up the next */
extern void sub_001ABDD0_gen(void);
void sub_001ABDD0(void)
{
    sub_001ABDD0_gen();
    HppoaTrainer *t = &g_hppoa_trainer;
    static int up_next;
    if (up_next) {
        uint32_t args[3] = {KEY_DEBUG, 1, 0};
        call_guest(APT_KEY, 3, args);
        up_next = 0;
    } else if (t->press_debug_key) {
        t->press_debug_key = 0;
        uint32_t args[3] = {KEY_DEBUG, 0, 0};
        call_guest(APT_KEY, 3, args);
        up_next = 1;
        fprintf(stderr, "[TRAINER] debug menu key pressed\n");
    }
}

/* ── Turning response (F10 Enhancements, turn_response: 100 as the console, up to 300 %) ─────────────────────────
 *
 * Harry follows the stick at the game's own turn speeds (the same on the Xbox): walking 360 degrees a second,
 * running 350, sneaking and shuffling 90, and inside 80 degrees of the stick's direction (TurnSpeedModActive) that
 * speed shrinks with the angle left, to nothing (TurnSpeedModAtZero 0, AtMax 0.75): the last part of every turn
 * is a slow crawl, a third of a second's time constant running, and a quick flick of the stick is ahead of him for
 * most of a second. The speeds (turning while moving, pivoting from a stop: sub_000CC7B0's table) are scaled by
 * the setting and the easing zone narrowed by it, so the crawl's time constant falls with its square (200 %: a
 * quarter). The zone is copied into each character's motion controller ([+0xD60] + 0x78, sub_00072000) when it is
 * set up: the player's own is written too, so a change shows at once. These are the game's shared values: other
 * characters walking about turn as quickly. Written when the setting or the player changes; the F6 trainer edits
 * the same values (its own change lasts until this setting moves) */
int hppoa_TurnResponse(void);  /* app.cpp: the setting, 100 to 300 */
int hppoa_InstantStep(void);   /* app.cpp: the first step from standing without the game's wait */
int hppoa_TurnFullSpeed(void); /* app.cpp: running keeps its speed through turns */

/* Full speed while turning (F10 Enhancements, turn_full_speed; the Xbox slows). Each frame running loses speed in
 * proportion to how far Harry leans into a turn (sub_000CB2xx: step x TurnSpeedDragMult x |lean at +0x1474|), so a
 * circle run with the stick slows him the whole way round. On: TurnSpeedDragMult 0, the run's own speed throughout */
#define TURN_SPEED_DRAG 0x36AEFCu  /* TurnSpeedDragMult, 0.2 as the game */

/* Instant first step (F10 Enhancements, instant_step; the Xbox has the wait too). From standing, a push of the stick
 * waits before Harry goes: a shuffle phase (ShuffleTime, 100 ms) and, for a direction well away from his facing, a
 * pause before the turn on the spot (JumpTurnPivotDelayTime, 180 ms), the turn itself at 600 degrees a second and
 * the push off after it. On: no waits, the turn three times as quick, the push off twice as strong. Off: the game's */
static const struct { uint32_t va; float on, off; int is_uint; } kStepVars[] = {
    {0x36AD10u, 0.0f, 180.0f, 1},                   /* JumpTurnPivotDelayTime, ms */
    {0x36A930u, 0.0f, 100.0f, 1},                   /* ShuffleTime, ms */
    {0x36AD2Cu, 1800.0f * 1.7453292457503267e-05f, 600.0f * 1.7453292457503267e-05f, 0},  /* JumpTurnStandPivotSpeed */
    {0x36A944u, 1800.0f * 1.7453292457503267e-05f, 600.0f * 1.7453292457503267e-05f, 0},  /* JumpTurnRunPivotSpeed */
    {0x36AE48u, 2025.0f * 1.7453292457503267e-05f, 675.0f * 1.7453292457503267e-05f, 0},  /* JumpTurnRunQTurnPivotSpeed */
    {0x36AC50u, 10.0f, 5.0f, 0},                    /* JumpTurnPushOffAccel */
    {0x36AC7Cu, 4.5f, 2.25f, 0},                    /* JumpTurnRunQTurnPushOffAccel */
    {0x36AA90u, 2.0f, 1.0f, 0},                     /* JumpTurnRunQTurnShortPushOffAccel */
};

static const struct { uint32_t va; float deg; } kTurnSpeeds[] = {
    {0x36AB10u, 350.0f},  /* RunTurnSpeed */
    {0x36AE34u, 360.0f},  /* WalkTurnSpeed */
    {0x36AA7Cu, 90.0f},   /* SneakTurnSpeed */
    {0x36AEC4u, 90.0f},   /* ShuffleTurnSpeed */
    {0x36AE9Cu, 360.0f},  /* StandPivotSpeed */
    {0x36AD64u, 360.0f},  /* SneakPivotSpeed */
    {0x36A964u, 1080.0f}, /* WalkPivotSpeed */
    {0x36AC20u, 2160.0f}, /* RunPivotSpeed */
    {0x36AC8Cu, 90.0f},   /* ShufflePivotSpeed */
};
#define TURN_MOD_ACTIVE 0x36AA10u  /* TurnSpeedModActive, radians (80 degrees) */
static const float kDegPerMs = 1.7453292457503267e-05f;  /* the speeds' stored unit: radians a millisecond */
static const float kDeg = 0.01745329238474369f;

/* [MOVE]: what the player does after the stick leaves rest (finding the first step's delay). From the frame the
 * left stick passes a quarter of its range with Harry at rest, for 1.5 s: each change of his movement state
 * ([[chr + 0x105C]]: 2 sneak, 3 walk, 4 run, else standing or a turn; sub_000CC7B0), with the ms since the push.
 * RECOMP_MOVE_TRACE=1 on */
extern volatile short g_stick_scaled[2];  /* hle_input.c: the left stick as the game reads it */
static int g_move_active;
static ULONGLONG g_move_t0;
static void move_trace(uint32_t chr)
{
    static int on = -1, last_state = -2, was_out;
    int active = g_move_active;
    ULONGLONG t0 = g_move_t0;
    if (on < 0) {
        const char *e = getenv("RECOMP_MOVE_TRACE");
        on = e && e[0] == '1';
    }
    if (!on || !chr || !hppoa_GuestOk(chr + 0x105C, 4)) return;
    uint32_t sp = MEM32(chr + 0x105C);
    int state = sp && hppoa_GuestOk(sp, 4) ? (int)MEM32(sp) : -1;
    float sx = g_stick_scaled[0] / 32767.0f, sy = g_stick_scaled[1] / 32767.0f, m = sqrtf(sx * sx + sy * sy);
    int out = m > 0.25f;
    ULONGLONG now = GetTickCount64();
    if (out && !was_out && !active && state != 2 && state != 3 && state != 4) {
        active = g_move_active = 1;
        t0 = g_move_t0 = now;
        last_state = -2;
        fprintf(stderr, "[MOVE] stick out (%.2f %.2f) from rest, state %d\n", sx, sy, state);
    }
    was_out = out;
    if (!active) return;
    if (state != last_state) {
        fprintf(stderr, "[MOVE] +%llu ms state %d (stick %.2f %.2f)\n", (unsigned long long)(now - t0), state, sx, sy);
        last_state = state;
    }
    if (now - t0 > 1500) g_move_active = 0;
}

void hppoa_TurningTick(void)
{
    static int applied = 100;
    static uint32_t applied_ctl;
    int pct = hppoa_TurnResponse();
    if (pct < 100) pct = 100;
    if (pct > 300) pct = 300;
    float f = (float)pct / 100.0f, zone = 80.0f * kDeg / f;
    if (pct != applied) {
        for (size_t i = 0; i < sizeof kTurnSpeeds / sizeof kTurnSpeeds[0]; i++)
            MEMF(kTurnSpeeds[i].va) = kTurnSpeeds[i].deg * kDegPerMs * f;
        MEMF(TURN_MOD_ACTIVE) = zone;
        fprintf(stderr, "[TURN] turning response %d %%: run turn %.0f deg/s, easing inside %.0f degrees\n", pct,
                350.0f * f, 80.0f / f);
        applied = pct;
        applied_ctl = 0;
    }
    static int drag_applied = -1;
    int full = hppoa_TurnFullSpeed() ? 1 : 0;
    if (full != drag_applied) {
        MEMF(TURN_SPEED_DRAG) = full ? 0.0f : 0.2f;
        fprintf(stderr, "[TURN] running through turns: %s\n", full ? "at full speed" : "slowed as the game");
        drag_applied = full;
    }
    static int step_applied = -1;
    int step = hppoa_InstantStep() ? 1 : 0;
    if (step != step_applied) {
        for (size_t i = 0; i < sizeof kStepVars / sizeof kStepVars[0]; i++) {
            float v = step ? kStepVars[i].on : kStepVars[i].off;
            if (kStepVars[i].is_uint) MEM32(kStepVars[i].va) = (uint32_t)v;
            else MEMF(kStepVars[i].va) = v;
        }
        fprintf(stderr, "[TURN] first step from standing: %s\n", step ? "at once (no shuffle or pivot wait)" : "as the game");
        step_applied = step;
    }
    /* the player's motion controller keeps its own copy of the zone */
    uint32_t chr = hppoa_TrainerPlayer();
    uint32_t ctl = chr && hppoa_GuestOk(chr + 0xD60, 4) ? MEM32(chr + 0xD60) : 0;
    if (ctl && ctl != applied_ctl && hppoa_GuestOk(ctl + 0x78, 4)) {
        MEMF(ctl + 0x78) = zone;
        applied_ctl = ctl;
    }
    move_trace(chr);
}

/* Run from standing on a full push (with F10's Instant first step; the Xbox walks first). The gait is picked from the
 * character's gait table at +0x1090 (sub_000CCDB0 -> sub_002135F0: count, then 0x2C byte entries {gait, ..., range};
 * the first entry whose range holds the speed value; gaits 1 standing, 2 walk, 3 a step between, 4 run). From
 * standing the game ramps that value up from 0 at about 1.65 a second (F12-free [MOVE] trace, 2026-11-09: 0.11,
 * 0.14 ... 0.58 a frame apart), walking all the way, and runs only once it passes about 0.66: a third of a second
 * of walk before every run. For the player, with the left stick at running reach, a walk or between-step answer is
 * replaced by the gait the stick asks for (its reach past the 0.35 deadzone read as the table's value): a full push
 * runs, three quarters slow-runs, from the first step; the game's own answer stands when it is the faster */
extern volatile short g_stick_scaled[2];  /* hle_input.c: the left stick as the game reads it */
extern void sub_002135F0_gen(void);
void sub_002135F0(void)
{
    uint32_t tab = g_ecx;
    float value = MEMF(g_esp + 4);
    sub_002135F0_gen();
    uint32_t e = g_eax;
    if (g_move_active) {
        uint32_t pl = hppoa_TrainerPlayer();
        if (pl && tab == pl + 0x1090)
            fprintf(stderr, "[MOVE] +%llu ms gait lookup: value %.3f -> gait %d (stick %.2f)\n",
                    (unsigned long long)(GetTickCount64() - g_move_t0), value,
                    e && hppoa_GuestOk(e, 4) ? (int)MEM32(e) : -1,
                    sqrtf((float)g_stick_scaled[0] * g_stick_scaled[0] + (float)g_stick_scaled[1] * g_stick_scaled[1]) / 32767.0f);
    }
    if (!e || !hppoa_InstantStep() || !hppoa_GuestOk(e, 4)) return;
    uint32_t chr = hppoa_TrainerPlayer();
    if (!chr || tab != chr + 0x1090) return;
    /* only while the game's value climbs as it does from standing (about 1.65 a second): what Harry does when the
     * game slows him (a dementor near, the pumpkin head jinx: the value held, or climbing slower, below a run) is
     * the game's. The climb over the last tenth of a second of player lookups (two in one game tick read the same
     * value); a gap over 150 ms starts afresh */
    enum { kRing = 32 };
    static float ring_v[kRing];
    static double ring_t[kRing];
    static int ring_n, ramping, told_held;
    static LARGE_INTEGER freq;
    LARGE_INTEGER qpc;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&qpc);
    double now = (double)qpc.QuadPart / (double)freq.QuadPart;
    if (ring_n && now - ring_t[(ring_n - 1) % kRing] > 0.15) ring_n = 0;
    if (!ring_n) ramping = value < 0.3f;  /* a fresh start from (near) standing */
    ring_v[ring_n % kRing] = value, ring_t[ring_n % kRing] = now, ring_n++;
    int old = ring_n - 1;
    while (old > 0 && old > ring_n - kRing && now - ring_t[(old - 1) % kRing] <= 0.1) old--;
    double span = now - ring_t[old % kRing];
    if (span >= 0.05) {
        double rate = (value - ring_v[old % kRing]) / span;
        int was = ramping;
        ramping = rate >= 1.0;
        if (was && !ramping && value < 0.9f && !told_held) {
            told_held = 1;
            fprintf(stderr, "[MOVE] the game holds Harry's speed (value %.3f, climbing %.2f a second): its gait kept\n",
                    value, rate);
        }
    }
    if (!ramping) return;
    told_held = 0;
    if (hppoa_GuestOk(chr + 0x1024, 4) && (MEM32(chr + 0x1024) & 8)) return;  /* the game's no running */
    uint32_t gait = MEM32(e);
    if (gait != 2 && gait != 3) return;
    /* the gait the stick asks for: its reach past the game's 0.35 deadzone as the table's value (0 to 1) */
    float sx = g_stick_scaled[0] / 32767.0f, sy = g_stick_scaled[1] / 32767.0f;
    float want = (sqrtf(sx * sx + sy * sy) - 0.35f) / 0.65f;
    if (want <= 0.0f) return;
    if (want > 1.0f) want = 1.0f;
    uint32_t n = MEM32(tab), base = MEM32(tab + 4);
    if (n > 32 || !hppoa_GuestOk(base, n * 0x2C)) return;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t en = base + i * 0x2C;
        if (want >= MEMF(en + 8) && want <= MEMF(en + 0xC)) {
            if (MEM32(en) > gait) {
                static int told;
                if (!told) told = 1, fprintf(stderr, "[TURN] from standing: straight to the stick's gait (no walk first)\n");
                g_eax = en;
            }
            return;
        }
    }
}
