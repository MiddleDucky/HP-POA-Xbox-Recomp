/**
 * True 16:9: the game's own cameras made wide (aspect '16:9' in hppoa.toml / the launcher; 4:3 is left alone).
 *
 * The renderer (singleton [0x30F948]) describes the screen it draws to in a 0x38 byte block at +0x2E14, made by its
 * init sub_00034750, which writes the screen's aspect, 4/3, to the block's +0x34. Every camera takes its aspect from
 * there (sub_000264F0: a w x h target's aspect = (w / h) scaled by +0x34), the projection and the view frustum the
 * title culls with alike. That alone kept the console's width and cut the height (the camera's angle is the horizontal
 * one, issue #2), so screen cameras also get their angle widened by the same 4/3 (hor_plus, below): the console's
 * height, wider sides, and the culling frustum made from the same matrix widens with them. 2D stays 4:3 shaped through
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

static float hor_plus;          /* below: Hor+, the screen camera's tangent factor (1 at 4:3) */
static int cut_bars_hidden(void); /* below: F10 Cutscenes without black bars, in 16:9 */

/* the lens as a % of the game's tangent, Hor+ in it. Cutscenes (issue #2, 2026-11-15: "weird fov"): on the console
 * a 4:3 cutscene is drawn between the game's black bars, the picture between them 16:9 (1440 x 822 of 1080 rows in
 * the issue's shots, the bars ending at row 129). With the bars hidden in 16:9, the shot fills the screen as that
 * band: the game's own lens, no Hor+ (the console's width; 0.75 of its height shown, 810 rows against the band's
 * 822). With the bars drawn, Hor+ keeps the band the console's height between them */
static float fov_percent(void)
{
    static int cutscene, logged = -1;
    static float from = 100.0f, now = 100.0f;
    static ULONGLONG t0;
    if (!hppoa_Paused()) cutscene = hppoa_Cutscene();
    if (cutscene != logged) {
        logged = cutscene;
        fprintf(stderr, "[FOV] %s\n", cutscene ? (cut_bars_hidden() ? "cutscene: the game's lens, as the console's band"
                                                                    : "cutscene: the game's lens, Hor+")
                                            : "gameplay: the slider's lens");
    }
    if (cutscene) {  /* into a cutscene at once: its shots are framed for the game's lens */
        const float lens = cut_bars_hidden() ? 100.0f : 100.0f * hor_plus;
        from = now = lens;
        t0 = 0;
        return lens;
    }
    const float want = (float)hppoa_FovPercent() * hor_plus;
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

/* Hor+ (issue #2, 2026-11-15: 16:9 zoomed in, cutscenes and play alike). The camera's +0x6C is its HORIZONTAL angle:
 * the perspective builder sub_000376E0 writes m[0] = m[5] = 1 / tan(+0x6C / 2), then m[5] *= the aspect sub_000264F0
 * gave. With that aspect 16:9 the width stayed the console's and the height was cut to 3/4 of it (the picture 4/3
 * larger). So a camera drawing to the screen has its half angle's tangent widened by aspect / (4/3) as well: m[5]
 * comes back to the console's (4/3) / tan, the console's height, and the width grows to 16:9. Set by sub_000264F0
 * for each screen camera (its +0x34 times 3/4, 1 at 4:3); the portal walk uses the last one */
static float hor_plus = 1.0f;
static float cons_aspect = 1.0f;

/* RECOMP_HOR_PLUS=0: no Hor+ (16:9 as before, the console's width); RECOMP_PORTRAIT_LENS=0: portraits drawn with the
 * main projection. Read once; for telling apart what each part does (pop-in while turning, 2026-11-15) */
static int env_on(const char *name)
{
    const char *e = getenv(name);
    const int on = !(e && e[0] == '0');
    fprintf(stderr, "[WIDE] %s %s\n", name, on ? "on" : "off (environment)");
    return on;
}
static int hor_plus_on(void)
{
    static int on = -1;
    if (on < 0) on = env_on("RECOMP_HOR_PLUS");
    return on;
}
static int portrait_lens_on(void)
{
    static int on = -1;
    if (on < 0) on = env_on("RECOMP_PORTRAIT_LENS");
    return on;
}  /* the screen camera's aspect back to the console's 4:3: (4/3) / +0x34 */

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

/* Cameras drawing into a texture: the animated portraits (the knight, the Fat Lady) are live scenes drawn into a
 * texture shown in the frame (issue #3, 2026-11-15: squished at 16:9). sub_00031B40 sets a camera up with the aspect
 * sub_000264F0 (w, h: the viewport) gives, (w / 640) / (h / 480) times the screen's +0x34: w / h on the console's
 * 4:3, but 4/3 times too wide for a texture once +0x34 is 16:9, the portrait's scene squeezed to 3/4 of its width.
 * A texture is not shown stretched to 16:9 as the screen is, so it keeps w / h. Told by the target: the screen when
 * the game draws to a back buffer or its viewport is the whole back buffer (either, so the main view stays wide
 * whatever order the game sets them in). The FOV slider leaves such a camera alone too: the projection builders
 * sub_000376E0 / sub_000377B0 come right after in sub_00031B40 and take the mark */
int hppoa_TargetIsScreen(uint32_t *w, uint32_t *h); /* hle_d3d8.cpp */
static int texture_cam;

extern void sub_000264F0_gen(void);
void sub_000264F0(void)
{
    const float vw = MEMF(g_esp + 4), vh = MEMF(g_esp + 8);
    sub_000264F0_gen();
    texture_cam = 0;
    if (!hppoa_Widescreen() || !(vw > 0.0f) || !(vh > 0.0f)) {
        hor_plus = 1.0f;
        return;
    }
    /* the screen's factor, from what it returned: (w / 640) / (h / 480) times it. Read from [ecx + 0x34] on entry it
     * came out 0 (log 2026-11-15 18:53): the hook's ecx is not the object the function reads, so Hor+ and the
     * portraits' console lens never took effect */
    const float scr = (float)(g_fp_stack[g_fp_top] / ((vw / 640.0) / (vh / 480.0)));
    uint32_t sw, sh;
    const int rt = hppoa_TargetIsScreen(&sw, &sh);
    const int full = fabsf(vw - (float)sw) < 0.5f && fabsf(vh - (float)sh) < 0.5f;
    static float seen[16][3];
    static int nseen;
    int known = 0;
    for (int i = 0; i < nseen && !known; i++) known = seen[i][0] == vw && seen[i][1] == vh && seen[i][2] == scr;
    if (!known && nseen < 16) {  /* each size and factor once: the main view and a portrait take turns every frame */
        seen[nseen][0] = vw;
        seen[nseen][1] = vh;
        seen[nseen++][2] = scr;
        fprintf(stderr, "[WIDE] camera %.0fx%.0f, back buffer %ux%u, target %s, factor %.4f: %s\n", vw, vh, sw, sh,
                rt ? "screen" : "texture", scr, rt || full ? "16:9" : "its own shape");
    }
    if (!(scr > 0.1f)) return;
    if (rt || full) {  /* the screen: Hor+, the console's height kept */
        hor_plus = hor_plus_on() ? scr * 0.75f : 1.0f;
        cons_aspect = (4.0f / 3.0f) / scr;
        return;
    }
    texture_cam = 1;
    g_fp_stack[g_fp_top] = g_fp_stack[g_fp_top] * (4.0 / 3.0) / scr;
}

/* The portraits (issue #3, 2026-11-15: still squished after the camera fix above, F12 183835): the portrait pass
 * sub_00038E00 has no lens of its own. It sets the portrait's render target (sub_00052FA0) and view (sub_0002F420,
 * the view saved by sub_0002F860 and set back after) and draws with the projection in effect, the main camera's at
 * the renderer's +0x290: the console's 4:3 lens there, ours 16:9, Hor+ and the FOV slider's, the portrait's scene
 * squeezed. So each time a screen camera's projection is built, the console's is built as well (the game's angle,
 * the 4:3 aspect, into the same matrix, then the real one over it), and the portrait pass is given that one while
 * it draws: sub_0002F520 (thiscall renderer, matrix, 1: stores +0x290 after copying the old one to 0x30F960, sets the
 * D3D projection, builds the culling frustum) with the console's matrix before, the main one after, 0x30F960 as it
 * was. Only when +0x290 is the matrix built here, so any other projection the pass may meet is left alone */
static uint32_t cons_proj[16], wide_proj[16];
static int cons_valid;

typedef struct Regs { uint32_t eax, ebx, ecx, edx, esi, edi, esp; } Regs;
static void regs_save(Regs *r)
{
    r->eax = g_eax, r->ebx = g_ebx, r->ecx = g_ecx, r->edx = g_edx, r->esi = g_esi, r->edi = g_edi, r->esp = g_esp;
}
static void regs_load(const Regs *r)
{
    g_eax = r->eax, g_ebx = r->ebx, g_ecx = r->ecx, g_edx = r->edx, g_esi = r->esi, g_edi = r->edi, g_esp = r->esp;
}

static void fov_call(void (*gen)(void))
{
    if (texture_cam) {  /* a portrait's camera: the game's lens */
        texture_cam = 0;
        gen();
        return;
    }
    /* the builders: thiscall (ecx = camera), [esp+4] the matrix out, [esp+8] the aspect; ret 8. sub_000376E0 writes
     * a scratch value over [esp+4], so both arguments are put back for the second run */
    Regs r;
    regs_save(&r);
    const uint32_t out = MEM32(r.esp + 4), aspect = MEM32(r.esp + 8);
    if (out && hppoa_Widescreen()) {
        MEMF(r.esp + 8) = MEMF(r.esp + 8) * cons_aspect;
        gen();
        for (int i = 0; i < 16; i++) cons_proj[i] = MEM32(out + 4 * i);
        regs_load(&r);
        MEM32(r.esp + 4) = out;
        MEM32(r.esp + 8) = aspect;
    }
    fov_call_on(r.ecx, gen);
    if (out && hppoa_Widescreen()) {
        for (int i = 0; i < 16; i++) wide_proj[i] = MEM32(out + 4 * i);
        cons_valid = 1;
    }
}

extern void sub_0002F520(void);
static void set_projection(uint32_t renderer, const uint32_t *m)
{
    Regs r;
    regs_save(&r);
    g_esp -= 64;  /* the matrix, on the guest stack below the caller's */
    const uint32_t mat = g_esp;
    for (int i = 0; i < 16; i++) MEM32(mat + 4 * i) = m[i];
    g_esp -= 12;
    MEM32(g_esp) = 0;  /* return address, unused */
    MEM32(g_esp + 4) = mat;
    MEM32(g_esp + 8) = 1;
    g_ecx = renderer;
    sub_0002F520();
    regs_load(&r);
}

extern void sub_0002A5C0(void);
extern void sub_00038E00_gen(void);
void sub_00038E00(void)
{
    const uint32_t renderer = MEM32(0x30F948u);
    int swap = renderer && cons_valid && hppoa_Widescreen() && portrait_lens_on();
    for (int i = 0; swap && i < 16; i++) swap = MEM32(renderer + 0x290 + 4 * i) == wide_proj[i];
    static int logged = -1;
    if (swap != logged) {
        logged = swap;
        fprintf(stderr, "[WIDE] portrait pass: %s\n", swap ? "the console's 4:3 lens" : "the projection as it is");
    }
    if (!swap) {
        sub_00038E00_gen();
        return;
    }
    uint32_t prev[16];
    for (int i = 0; i < 16; i++) prev[i] = MEM32(0x30F960u + 4 * i);
    set_projection(renderer, cons_proj);
    sub_00038E00_gen();
    set_projection(renderer, wide_proj);
    for (int i = 0; i < 16; i++) MEM32(0x30F960u + 4 * i) = prev[i];
    /* the pass ends with sub_0002A5C0 (no arguments): the view and projection in effect (sub_0002F860 /
     * sub_0002F840) multiplied (sub_0001ADC0) and handed to sub_00047040, the frame's view-projection the scene is
     * culled with. Run there, it took the console's 4:3 lens: Hor+'s sides culled for the rest of the frame, things
     * drawn only once inside the console's width as the camera turned (2026-11-15, gone with RECOMP_HOR_PLUS=0). Run
     * again with the main projection back */
    Regs r;
    regs_save(&r);
    g_esp -= 4;
    MEM32(g_esp) = 0;  /* return address, unused */
    sub_0002A5C0();
    regs_load(&r);
}

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
