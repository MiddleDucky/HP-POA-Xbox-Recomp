/**
 * True 16:9: the game's own cameras made wide (aspect '16:9' in hppoa.toml / the launcher; 4:3 is left alone).
 *
 * The renderer (singleton [0x30F948]) describes the screen it draws to in a 0x38 byte block at +0x2E14, made by its
 * init sub_00034750, which writes the screen's aspect, 4/3, to the block's +0x34. Every camera takes its aspect from
 * there (sub_000264F0: a w x h target's aspect = (w / h) scaled by +0x34), the projection and the view frustum the
 * title culls with alike, so 16/9 there widens both: no objects popping in at the sides. 2D stays 4:3 shaped through
 * the renderer's Squeeze2D.
 *
 * Built into the recompile with --exclude-manual naming this file.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>

/* the register names (eax, esp, ...) the runtime macros expand to */
#define RECOMP_GENERATED_CODE
#include "recomp/gen/recomp_types.h"

int hppoa_Widescreen(void);  /* hle_d3d8.cpp: the run is 16:9 */

/* the renderer's init (__thiscall, ecx = renderer) */
extern void sub_00034750_gen(void);
void sub_00034750(void)
{
    uint32_t self = g_ecx;
    sub_00034750_gen();
    uint32_t screen = MEM32(self + 0x2E14);
    if (!hppoa_Widescreen() || !screen) return;
    float wide = 16.0f / 9.0f;
    uint32_t bits;
    memcpy(&bits, &wide, 4);
    MEM32(screen + 0x34) = bits;
    fprintf(stderr, "[WIDE] screen %08X aspect 16:9\n", screen);
}

/* Field of view (F10 Graphics, % of the game's). A camera keeps its vertical angle in degrees at +0x6C (+0x70 near,
 * +0x74 far, +0x80 set: orthographic). Its two projection builders, sub_000376E0 and sub_000377B0 (__thiscall, ecx =
 * camera), read it; the frustum the title culls with is made from the matrix they write (sub_0002F520 on it right
 * after, in sub_00031B40), so both widen together. The portal walk (sub_00051DC0, from sub_00052020: a room behind a
 * doorway is drawn only if the doorway is on screen) projects with a matrix of its own, sub_000518D0, from the
 * renderer's current camera ([0x30F948] + 0xCC): widened too, or a room seen through a doorway in the widened band
 * stayed undrawn, the clear colour showing through (2026-10-26). The angle is widened for the call only: the camera
 * keeps the game's, and nothing else the game does with it changes. The percentage scales the half angle's tangent,
 * so 120 % shows 20 % more of the scene each way. */
uint32_t hppoa_FovPercent(void);  /* app.cpp: 80 to 130, 100 as the game */
int hppoa_Cutscene(void);          /* hle_clock.c: a cutscene holds the game clock */
void hppoa_SetCutsceneBars(int on); /* hle_d3d8.cpp: this frame's draws are a cutscene's, its letterbox not drawn */
int hppoa_Paused(void);            /* hle_quit.c: the pause menu is up */

/* the slider's share for this camera now: cutscenes keep the game's lens, their shots framed by hand (at 130 % the
 * twins were seen walking on past where the console's shot loses them, 2026-10-26). Pausing holds the game clock as
 * a cutscene does: while paused the lens stays as it was (the pause menu went back to the game's).
 * From a cutscene back to play the lens eases to the slider's over kLensMs instead of changing in a frame: at 80 %
 * the picture jumped a quarter closer as the HUD came in, the camera itself not moving (F12 224431 / 224433,
 * 2026-11-10) */
static const float kLensMs = 600.0f;

static float fov_percent(void)
{
    static int cutscene, logged = -1;
    static float from = 100.0f, now = 100.0f;
    static ULONGLONG t0;
    if (!hppoa_Paused()) cutscene = hppoa_Cutscene();
    if (cutscene != logged) {
        logged = cutscene;
        fprintf(stderr, "[FOV] %s\n", cutscene ? "cutscene: the game's lens" : "gameplay: the slider's lens");
    }
    if (cutscene) {  /* into a cutscene at once: its shots are framed for the game's lens */
        from = now = 100.0f;
        t0 = 0;
        return 100.0f;
    }
    const float want = (float)hppoa_FovPercent();
    if (want != from && t0 == 0) t0 = GetTickCount64();  /* a change: ease from where it is */
    if (t0) {
        float x = (float)(GetTickCount64() - t0) / kLensMs;
        if (x >= 1.0f || want == now) {
            from = now = want;
            t0 = 0;
        } else {
            x = x * x * (3 - 2 * x);
            now = from + (want - from) * x;
            if (fabsf(want - from) < 0.01f) t0 = 0;
            return now;
        }
    }
    return now = from = want;
}

static void fov_call_on(uint32_t cam, void (*gen)(void))
{
    float pct = fov_percent();
    uint32_t bits = MEM32(cam + 0x6C);
    float fov;
    memcpy(&fov, &bits, 4);
    if (fabsf(pct - 100.0f) < 0.01f || MEM8(cam + 0x80) || !(fov > 1.0f && fov < 170.0f)) {
        gen();
        return;
    }
    float wide = atanf(tanf(fov * 3.14159265f / 360.0f) * pct / 100.0f) * 360.0f / 3.14159265f;
    uint32_t wbits;
    memcpy(&wbits, &wide, 4);
    MEM32(cam + 0x6C) = wbits;
    gen();
    MEM32(cam + 0x6C) = bits;
}

static void fov_call(void (*gen)(void)) { fov_call_on(g_ecx, gen); }

/* Cutscenes without their letterbox (by request, 2026-11-09): the bars across the top and bottom are left out
 * (d3d12.cpp, HiddenDraw), the scene drawn behind them shown to the screen's edges. The lens is the game's 16:9 one
 * as before (a 4:3 lens shown as the frame's 16:9 band was tried and dropped). The camera's projection builders tag
 * the draws after them, so the renderer sees it in order. RECOMP_CUTSCENE_BARS=1: the bars as the game draws them.
 * Held while paused, as fov_percent: the pause menu over a cutscene keeps it */
int hppoa_CutsceneBarsHidden(void); /* app.cpp: F10 Enhancements > Cutscenes without black bars (2026-11-10) */
static int cut_bars_hidden(void)
{
    static int env = -1, held, logged = -1;
    if (env < 0) {
        const char *e = getenv("RECOMP_CUTSCENE_BARS");
        env = e && e[0] == '1';
        if (env) fprintf(stderr, "[CUTBARS] cutscene letterbox drawn (RECOMP_CUTSCENE_BARS=1)\n");
    }
    const int on = !env && hppoa_CutsceneBarsHidden();
    /* not waiting for the cutscene: the game is known to be in one by its bars being drawn (hle_clock.c's
     * hppoa_Cutscene, d3d12.cpp's NoteLetterbox), so the first frames of them, fading in, showed and went (a flicker
     * at each cutscene's start, 2026-11-09). HiddenDraw's own shape test (untextured bands across the frame's top or
     * bottom, under a third of its height) is what tells the bars; they are still noted for hppoa_Cutscene */
    if (!hppoa_Paused()) held = on && hppoa_Widescreen();
    if (held != logged) {
        logged = held;
        fprintf(stderr, "[CUTBARS] %s\n", held ? "letterbox hidden (16:9)" : "off");
    }
    return held;
}

extern void sub_000376E0_gen(void);
void sub_000376E0(void)
{
    hppoa_SetCutsceneBars(cut_bars_hidden());
    fov_call(sub_000376E0_gen);
}
extern void sub_000377B0_gen(void);
void sub_000377B0(void)
{
    hppoa_SetCutsceneBars(cut_bars_hidden());
    fov_call(sub_000377B0_gen);
}

/* the portal walk's projection (no arguments): the renderer's current camera, at RECOMP_PORTAL_ANGLE degrees (default
 * 160; 0: the view's own angle) when that is wider than the view. A doorway just off screen then passes as well, so a
 * room the camera turns towards is drawn already rather than a frame or more late (2026-10-26, rooms popping in at
 * the screen's edges). Each room's contents are tested against the walk's rectangles in this projection
 * (sub_00052640), so the width reaches them too; hle_portals.c lets every doorway through */
static float portal_angle(void)
{
    static float a = -1.0f;
    if (a < 0.0f) {
        const char *e = getenv("RECOMP_PORTAL_ANGLE");
        a = e ? (float)atof(e) : 160.0f;
        if (a < 0.0f) a = 0.0f;
        if (a > 170.0f) a = 170.0f;
        fprintf(stderr, "[WIDE] doorways tested at %.0f degrees%s\n", a, a > 0.0f ? "" : " (the view's)");
    }
    return a;
}

extern void sub_000518D0_gen(void);
void sub_000518D0(void)
{
    uint32_t cam = MEM32(MEM32(0x30F948u) + 0xCC);
    if (!cam) {
        sub_000518D0_gen();
        return;
    }
    float fov, want = portal_angle();
    uint32_t bits = MEM32(cam + 0x6C);
    memcpy(&fov, &bits, 4);
    /* the view's angle with the slider's widening, as the projection builders see it */
    float view = fov;
    float pct = fov_percent();
    if (fabsf(pct - 100.0f) >= 0.01f && fov > 1.0f && fov < 170.0f)
        view = atanf(tanf(fov * 3.14159265f / 360.0f) * pct / 100.0f) * 360.0f / 3.14159265f;
    if (MEM8(cam + 0x80) || !(fov > 1.0f && fov < 170.0f) || want <= view) {
        fov_call_on(cam, sub_000518D0_gen);
        return;
    }
    uint32_t wbits;
    memcpy(&wbits, &want, 4);
    MEM32(cam + 0x6C) = wbits;
    sub_000518D0_gen();
    MEM32(cam + 0x6C) = bits;
}

/* Cutscene shots turned to keep out what 16:9 shows past the console's frame. sub_000376B0 (__thiscall, ecx = camera,
 * [esp+4] = out) writes the camera's view matrix (D3D, row vectors) into the renderer at +0x1A8 before the renderer
 * sets it and builds the culling frustum from it, so a turn made here moves both. A shot is told by the eye (from the
 * matrix) within a radius, in a cutscene. RECOMP_SHOT_TURN=0: as the game. */
typedef struct Turn {
    const char *name;
    float eye[3], radius, yaw;  /* yaw in degrees, positive to the left */
} Turn;

static const Turn kTurns[] = {
    /* Night 0, the corridor with the twins (Fred and George's shop talk): the twin leaving through the door on the
     * right walked on into the open doorway at the screen's edge and vanished there (2026-10-26, F12 141641 and
     * 143311: eyes 738.7 2532.6 3243.5 and 627.1 2556.1 3248.5). At the game's lens the door's frame
     * is on screen with a sliver of its opening at the edge: 2.5 degrees keeps the whole frame and none of the opening
     * (previews reprojected from F12 143311 at 0, 1.5, 2.5, 3.5; 5 and more moved the frame towards the middle) */
    {"corridor, the twins leaving", {683.0f, 2544.0f, 3246.0f}, 150.0f, 2.5f},
};

static int shot_turn_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_SHOT_TURN");
        on = !(e && e[0] == '0');
    }
    return on;
}

extern void sub_000376B0_gen(void);
void sub_000376B0(void)
{
    uint32_t out = MEM32(g_esp + 4);
    sub_000376B0_gen();
    static const Turn *logged;
    const Turn *hit = NULL;
    float m[16];
    if (shot_turn_enabled() && hppoa_Cutscene() && out) {
        for (int i = 0; i < 16; i++) {
            uint32_t b = MEM32(out + 4u * i);
            memcpy(&m[i], &b, 4);
        }
        float eye[3];
        for (int i = 0; i < 3; i++) eye[i] = -(m[12] * m[i * 4] + m[13] * m[i * 4 + 1] + m[14] * m[i * 4 + 2]);
        for (size_t k = 0; k < sizeof kTurns / sizeof kTurns[0] && !hit; k++) {
            float d = 0;
            for (int i = 0; i < 3; i++) d += (eye[i] - kTurns[k].eye[i]) * (eye[i] - kTurns[k].eye[i]);
            if (d <= kTurns[k].radius * kTurns[k].radius) hit = &kTurns[k];
        }
    }
    if (hit != logged) {
        logged = hit;
        fprintf(stderr, "[SHOT] %s\n", hit ? hit->name : "no shot turned");
    }
    if (!hit) return;
    /* view space turned about its up axis: v' = v R, a point at the yaw to the left coming to the middle */
    float a = hit->yaw * 3.14159265f / 180.0f, c = cosf(a), sn = sinf(a);
    for (int r = 0; r < 4; r++) {
        float x = m[r * 4], z = m[r * 4 + 2];
        m[r * 4] = x * c + z * sn;
        m[r * 4 + 2] = -x * sn + z * c;
    }
    for (int i = 0; i < 16; i++) {
        uint32_t b;
        memcpy(&b, &m[i], 4);
        MEM32(out + 4u * i) = b;
    }
}
