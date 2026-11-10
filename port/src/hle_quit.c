/**
 * Quit To Desktop: a prompt on the front end and in the pause menu, "Back  Quit To Desktop", and an "are you sure"
 * box (A yes, B no) drawn by the options overlay (renderer/app/menu.cpp). The title's menus are Apt (Flash) movies that
 * keep their highlighted row to themselves, so the quit is a button of its own rather than a row in them.
 *
 *   main menu   the menus name each screen they show through the native SetTraceControlMode (sub_00020220):
 *               "fe" is the main menu (New Game, Load Game, Options); the title, "pressstart", the movies, loading
 *               and the game ("ig") are other names
 *   pause menu  the screen "options" between the natives PauseGame (sub_00021C90) and UnpauseGame (sub_00021D00)
 *
 * It used to read [0x376B2C] as the game state, but the front end stores its update there once (0x1B2A0) and nothing
 * changes it after, so the prompt stayed up everywhere (2026-10-24, from a trace of the natives: RECOMP_APT_TRACE=1).
 *
 * While the prompt is up the game does not see Back; while the box is up it sees the pad at rest (until every button
 * is let go after it closes, so B does not reach the menu under it).
 *
 * Built into the recompile with --exclude-manual naming this file.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* the register names (eax, esp, ...) the runtime macros expand to */
#define RECOMP_GENERATED_CODE
#include "recomp/gen/recomp_types.h"
#include "input/xinput_xbox.h"

#define PAD_START      0x0010u
#define PAD_BACK       0x0020u
#define PRESSED        30  /* analog button threshold, as XInput's */

void hppoa_QuitNow(void);   /* menu.cpp: the pipeline cache saved, then the title's own exit */

static volatile int g_paused;
static volatile int g_box;       /* 1 the box is up, 2 closed, the pad held at rest until let go */
static char g_screen[24];         /* the screen the menus last named (SetTraceControlMode) */
/* Only the top of the main menu and of the pause menu: A there opens a submenu (Options, Load Game, Save) or leaves
 * the menu, and the menus name no submenu, so the prompt goes until B comes back out of it or the menu names its
 * screen again (backing out of Load Game and Save does). B or Start on the top level leaves the menu (the title,
 * back to the game), named only once it has faded out: the prompt goes at once and comes back after kHideMs if the
 * screen is still up */
static volatile int g_sub;         /* in a submenu: A on the top level, not yet B */
static volatile DWORD g_hidden_at;
static char g_hidden_on[24];
enum { kHideMs = 1200 };

/* a guest string, at most n - 1 bytes */
static void guest_str(uint32_t va, char *out, int n)
{
    int i = 0;
    for (; va && i < n - 1; i++) {
        char c = (char)MEM8(va + (uint32_t)i);
        if (!c) break;
        out[i] = c;
    }
    out[i] = 0;
}

/* RECOMP_APT_TRACE=1: the menus' calls into the game (Flash natives), to tell their screens apart */
static int apt_trace(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("RECOMP_APT_TRACE"); on = e && *e && *e != '0'; }
    return on;
}
static void apt_log(const char *name)
{
    static const char *last;
    static unsigned repeats;
    if (!apt_trace()) return;
    if (name == last && ++repeats > 2) return;  /* the same native again and again: once or twice is enough */
    if (name != last) repeats = 0;
    last = name;
    char arg[48];
    guest_str(MEM32(g_esp + 4), arg, sizeof arg);
    for (char *c = arg; *c; c++) if ((unsigned char)*c < 32 || (unsigned char)*c > 126) { *c = 0; break; }
    fprintf(stderr, "[APT] %lu %s (%s) screen %s paused %d\n", (unsigned long)GetTickCount(), name, arg, g_screen,
            g_paused);
}
/* the natives watched (written out: the recompiler finds overrides by their definitions) */
extern void sub_00021A60_gen(void);
void sub_00021A60(void) { apt_log("StartGame"); sub_00021A60_gen(); }
extern void sub_0001E980_gen(void);
void sub_0001E980(void) { apt_log("EndGame"); sub_0001E980_gen(); }
extern void sub_0001FA80_gen(void);
void sub_0001FA80(void) { apt_log("GameFlow"); sub_0001FA80_gen(); }
extern void sub_0001FBD0_gen(void);
void sub_0001FBD0(void) { apt_log("MemCard"); sub_0001FBD0_gen(); }
extern void sub_00021E30_gen(void);
void sub_00021E30(void) { apt_log("RequestSaveGameAvailability"); sub_00021E30_gen(); }
extern void sub_0001F6F0_gen(void);
void sub_0001F6F0(void) { apt_log("PlayMovie"); sub_0001F6F0_gen(); }
extern void sub_0001F760_gen(void);
void sub_0001F760(void) { apt_log("StopMovie"); sub_0001F760_gen(); }
extern void sub_00022020_gen(void);
void sub_00022020(void) { apt_log("updateoptions"); sub_00022020_gen(); }
extern void sub_0001FF00_gen(void);
void sub_0001FF00(void) { apt_log("setoption"); sub_0001FF00_gen(); }
extern void sub_0001E860_gen(void);
void sub_0001E860(void) { apt_log("RenderGameWorld"); sub_0001E860_gen(); }
extern void sub_0001FB90_gen(void);
void sub_0001FB90(void) { apt_log("SkipCutscene"); sub_0001FB90_gen(); }
extern void sub_0001E8F0_gen(void);
void sub_0001E8F0(void) { apt_log("LoadTextIcons"); sub_0001E8F0_gen(); }
extern void sub_000204F0_gen(void);
void sub_000204F0(void) { apt_log("PadIconMapping"); sub_000204F0_gen(); }
extern void sub_00020330_gen(void);
void sub_00020330(void) { apt_log("GameMap"); sub_00020330_gen(); }
extern void sub_0001E700_gen(void);
void sub_0001E700(void) { apt_log("StartMusic"); sub_0001E700_gen(); }
extern void sub_0001E730_gen(void);
void sub_0001E730(void) { apt_log("StopMusic"); sub_0001E730_gen(); }
extern void sub_0001E770_gen(void);
void sub_0001E770(void) { apt_log("SetAmbience"); sub_0001E770_gen(); }
extern void sub_0001E840_gen(void);
void sub_0001E840(void) { apt_log("SuspendMusicAndAmbience"); sub_0001E840_gen(); }
extern void sub_0001E850_gen(void);
void sub_0001E850(void) { apt_log("ResumeMusicAndAmbience"); sub_0001E850_gen(); }
extern void sub_00020070_gen(void);
void sub_00020070(void) { apt_log("SetGlobalColour"); sub_00020070_gen(); }
extern void sub_000201B0_gen(void);
void sub_000201B0(void) { apt_log("UseGlobalColour"); sub_000201B0_gen(); }
extern void sub_0001E9D0_gen(void);
void sub_0001E9D0(void) { apt_log("AssignToButton"); sub_0001E9D0_gen(); }
extern void sub_00021DA0_gen(void);
void sub_00021DA0(void) { apt_log("SendHPSGameEvent"); sub_00021DA0_gen(); }
extern void sub_0001FF40_gen(void);
void sub_0001FF40(void) { apt_log("UpdateLocalisation"); sub_0001FF40_gen(); }
extern void sub_00020290_gen(void);
void sub_00020290(void) { apt_log("Chapter"); sub_00020290_gen(); }
extern void sub_0001EBD0_gen(void);
void sub_0001EBD0(void) { apt_log("IsItReadytoRestart"); sub_0001EBD0_gen(); }
extern void sub_0001E760_gen(void);
void sub_0001E760(void) { apt_log("EnterLoad4Audio"); sub_0001E760_gen(); }
extern void sub_0001F010_gen(void);
void sub_0001F010(void) { apt_log("Create3DModel"); sub_0001F010_gen(); }
extern void sub_00021E50_gen(void);
void sub_00021E50(void) { apt_log("Dispose3DModel"); sub_00021E50_gen(); }
/* character switching, every call ([SWAP]; apt_log keeps only two of a repeat): the HUD's RequestCharacterSwap and
 * CharacterSwitch natives, and the switch itself, sub_0006D500 (this = [0x3660C8], slot, arg): the slot's character
 * (word +0xE6 of its 0x60-byte entry), the switching flag (+0x1B0) and whether it switched (al) */
extern void sub_0001EE30_gen(void);
void sub_0001EE30(void)
{
    if (apt_trace()) fprintf(stderr, "[SWAP] %lu RequestCharacterSwap flags %08X\n", (unsigned long)GetTickCount(),
                             MEM32(0x30F1D0));
    sub_0001EE30_gen();
}
extern void sub_0001EE70_gen(void);
void sub_0001EE70(void)
{
    if (apt_trace()) fprintf(stderr, "[SWAP] %lu CharacterSwitch flags %08X\n", (unsigned long)GetTickCount(),
                             MEM32(0x30F1D0));
    sub_0001EE70_gen();
}
/* [MODEL]: a character's model built (sub_000A7020, this = the character, its model name at +0xFC, "<Models>%s.hpm");
 * only character models (aa...), to see when Harry's is built or rebuilt (Night 2: pyjamas, aa3_hry_pyjamas-game) */
extern void sub_000A7020_gen(void);
void sub_000A7020(void)
{
    if (apt_trace() && MEM32(ecx + 0xFC)) {
        char name[48];
        guest_str(MEM32(ecx + 0xFC), name, sizeof name);
        if (name[0] == 'a' && name[1] == 'a')
            fprintf(stderr, "[MODEL] %lu %s for %08X from %08X\n", (unsigned long)GetTickCount(), name, ecx,
                    MEM32(g_esp));
    }
    sub_000A7020_gen();
}
extern void sub_0006D500_gen(void);
void sub_0006D500(void)
{
    uint32_t self = ecx, slot = MEM32(g_esp + 4), arg = MEM32(g_esp + 8), ret = MEM32(g_esp);
    uint32_t who = slot < 8 ? MEM16(self + slot * 0x60 + 0xE6) : 0xFFFF;
    uint8_t switching = MEM8(self + 0x1B0);
    sub_0006D500_gen();
    if (apt_trace())
        fprintf(stderr, "[SWAP] %lu switch slot %u (character %u) arg %u from %08X: switching %u -> %s\n",
                (unsigned long)GetTickCount(), slot, who, arg, ret, switching, (eax & 0xFF) ? "switched" : "refused");
}
extern void sub_00020220_gen(void);
void sub_00020220(void)
{
    /* SetTraceControlMode(name): the screen the menus have moved to */
    guest_str(MEM32(g_esp + 4), g_screen, sizeof g_screen);
    g_sub = 0;
    if (!strcmp(g_screen, "fe")) g_paused = 0;  /* quitting from the pause menu to the front end calls no UnpauseGame */
    apt_log("SetTraceControlMode");
    sub_00020220_gen();
}
extern void sub_000203C0_gen(void);
void sub_000203C0(void) { apt_log("DebouncePads"); sub_000203C0_gen(); }
extern void sub_00021F40_gen(void);
void sub_00021F40(void) { apt_log("UpdateTutorial"); sub_00021F40_gen(); }
extern void sub_0001FEE0_gen(void);
void sub_0001FEE0(void) { apt_log("AmIReturningFromEyetoy"); sub_0001FEE0_gen(); }
extern void sub_0001EF10_gen(void);
void sub_0001EF10(void) { apt_log("SetMinigameSelection"); sub_0001EF10_gen(); }
extern void sub_0001E400_gen(void);
void sub_0001E400(void) { apt_log("UpdateMiniGameAvailability"); sub_0001E400_gen(); }

extern void sub_00021C90_gen(void);
void sub_00021C90(void)
{
    g_paused = 1;
    apt_log("PauseGame");
    sub_00021C90_gen();
}

extern void sub_00021D00_gen(void);
void sub_00021D00(void)
{
    g_paused = 0;
    apt_log("UnpauseGame");
    sub_00021D00_gen();
}

/* the game itself on screen (the menus' "ig", not paused): d3d12.cpp's 16:9 keeps the HUD at the screen's sides then */
int hppoa_InGame(void) { return !g_paused && !strcmp(g_screen, "ig"); }

/* d3d12.cpp: the menus on "ig", paused or not. PauseGame comes 170 to 310 ms before the menus move to "transition"
 * (and UnpauseGame about 100 ms after they are back on "ig"): the game still draws its HUD and "Pausing" over the
 * scene then, and with hppoa_InGame the HUD went back in to the 4:3 picture's sides for those frames */
/* also "widescreen" (a cutscene) and "pre_ig" (its end, back to the game): the HUD fading out as a cutscene starts
 * (the A B X Y buttons, the save icon) went in to the 4:3 picture's sides for those frames (2026-11-09) */
int hppoa_GameScreen(void)
{
    /* and "transition" unpaused: Back's way into the Remembrall (the inventory) starts on it over the game, and the
     * HUD went in to the 4:3 picture; its ring sliding in from past the frame's right side (623 to 772) came into
     * view with it, and stayed while the game turned the Remembrall down (before the train, F12 165604). The pause
     * menu's own transitions are paused, left as they were */
    return !strcmp(g_screen, "ig") || !strcmp(g_screen, "widescreen") || !strcmp(g_screen, "pre_ig") ||
           (!g_paused && !strcmp(g_screen, "transition"));
}

/* d3d12.cpp: a cutscene's screens ("widescreen", and "pre_ig" as it ends): the game's text there (subtitles) stays
 * about the middle, only the HUD's pictures go to the sides (2026-11-10) */
int hppoa_CutsceneScreen(void) { return !strcmp(g_screen, "widescreen") || !strcmp(g_screen, "pre_ig"); }

/* d3d12.cpp: the inventory (its own screen; it pauses the game too): its 2D stays narrowed about the middle, not
 * moved to the screen's left as the pause menu's column is */
int hppoa_Inventory(void) { return !strcmp(g_screen, "inventory"); }

/* the pause menu is up (PauseGame without UnpauseGame yet): hle_widescreen.c keeps the lens it had before */
int hppoa_Paused(void) { return g_paused; }

/* d3d12.cpp: the legal notices at start, kept 4:3 in 16:9 as the movies around them are */
int hppoa_LegalScreen(void) { return !strcmp(g_screen, "legalsandmemorycard"); }

/* 0 nothing, 1 the prompt, 2 the box (menu.cpp draws them) */
int hppoa_MenuMovie(void);  /* hle_video.c: a movie is playing over the menus */

int hppoa_QuitState(void)
{
    if (g_box == 1) return 2;
    if (g_sub) return 0;
    if (g_hidden_at) {
        if (GetTickCount() - g_hidden_at < kHideMs && !strcmp(g_screen, g_hidden_on)) return 0;
        g_hidden_at = 0;
    }
    /* the attract movie the front end plays after a while idle (hle_video.c): the menus still say "fe" under it */
    if (hppoa_MenuMovie()) return 0;
    if (!strcmp(g_screen, "fe")) return 1;
    return g_paused && !strcmp(g_screen, "options") ? 1 : 0;
}

static int any_down(const XBOX_GAMEPAD *g)
{
    if (g->wButtons) return 1;
    for (int i = 0; i < 8; i++)
        if (g->bAnalogButtons[i] > PRESSED) return 1;
    return 0;
}

/* hle_input.c: each pad as the game is about to read it */
void hppoa_QuitPad(int port, XBOX_GAMEPAD *g)
{
    static uint8_t prev_back[4], prev_a[4], prev_b[4], prev_start[4];
    if (port < 0 || port > 3) return;
    static int seen = -1;
    int now = hppoa_QuitState();
    if (now != seen) {
        seen = now;
        fprintf(stderr, "[QUIT] screen %s paused %d: prompt %s\n", g_screen, g_paused, now ? "on" : "off");
    }
    int back = (g->wButtons & PAD_BACK) != 0, a = g->bAnalogButtons[0] > PRESSED, b = g->bAnalogButtons[1] > PRESSED;
    int start = (g->wButtons & PAD_START) != 0;
    int back_edge = back && !prev_back[port], a_edge = a && !prev_a[port], b_edge = b && !prev_b[port];
    int start_edge = start && !prev_start[port];
    prev_back[port] = (uint8_t)back, prev_a[port] = (uint8_t)a, prev_b[port] = (uint8_t)b, prev_start[port] = (uint8_t)start;
    if (apt_trace() && (a_edge || b_edge || start_edge || back_edge))
        fprintf(stderr, "[APT] %lu pad %s%s%s%s screen %s paused %d\n", (unsigned long)GetTickCount(), a_edge ? "A" : "",
                b_edge ? "B" : "", start_edge ? "Start" : "", back_edge ? "Back" : "", g_screen, g_paused);

    if (g_box == 1) {
        if (a_edge) {
            fprintf(stderr, "[QUIT] quit to desktop\n");
            hppoa_QuitNow();
        }
        if (b_edge || back_edge) g_box = 2, fprintf(stderr, "[QUIT] box closed\n");
        memset(g, 0, sizeof *g);
        return;
    }
    if (g_box == 2) {
        if (any_down(g)) {
            memset(g, 0, sizeof *g);
            return;
        }
        g_box = 0;
    }
    int menu = !strcmp(g_screen, "fe") || (g_paused && !strcmp(g_screen, "options"));
    if (menu && g_sub) {
        if (b_edge) g_sub = 0;  /* back out of the submenu to the top level */
        return;
    }
    if (hppoa_QuitState() != 1) return;
    if (a_edge) {
        g_sub = 1;
        return;
    }
    if (b_edge || start_edge) {
        g_hidden_at = GetTickCount() | 1;
        memcpy(g_hidden_on, g_screen, sizeof g_hidden_on);
        return;
    }
    if (back_edge) {
        g_box = 1;
        fprintf(stderr, "[QUIT] box open\n");
        memset(g, 0, sizeof *g);
        return;
    }
    g->wButtons &= (uint16_t)~PAD_BACK;
}

/* RECOMP_APT_TRACE=1: [UIVAR] each variable the game sets in the HUD's Flash movie (sub_001AD3B0(name, value), the
 * pointer at [0x38A2F8]: "CharacterSwitch" = "true" and the like) when its value changes, to find which ones bring
 * up the help popups (the level scripts' PopupDialog_ShowText with GUI_..._HLPTXT keys) */
/* Enhancement: each tutorial popup once. Each frame the HUD's help line is set through sub_001AD3B0(
 * "Help_Text_Display", key) from 0x20BE5, or to "GUI_GENFRTXT_STRIDNONE_001" (0x2C86E8, nothing) from 0x20C09 when no
 * help is up; a popup holds its key 2 to 7 s. The tutorials come back day after day: GUI_GEN_HLPTXT_* ("You can move
 * the camera around using the right thumbstick", "Trolls can be distracted by the smell of Dungbombs"),
 * GUI_GENACT_HLPTXT_* ("Use the left thumbstick to steer the Patronus at the Dementor") and GUI_GENENDDAY_HLPTXT_* (the
 * END DAY reminder). The first showing of each runs as the game has it (its key written to tutorials_seen.txt beside
 * the exe at once; the same key back within kTipGraceMs of an interruption, a room name, is still that showing);
 * after that the key is shown as nothing, as the game shows it. The same line's room names (GUI_LOCTXT_*), card sets,
 * Lost and Found and house points are left alone. Deleting tutorials_seen.txt shows them all once more;
 * RECOMP_TUTORIAL_TIPS=1 keeps every showing. */
enum { kTipGraceMs = 1000, kTipsMax = 256 };
static char g_tips_seen[kTipsMax][32];
static int g_tips_n = -1;
static char g_tip_now[32];         /* the tutorial key on show now, its first showing */
static DWORD g_tip_left;           /* when the help line last moved off it */

static void tips_path(char *out, DWORD n)
{
    DWORD len = GetModuleFileNameA(NULL, out, n);
    char *slash = len ? strrchr(out, '\\') : NULL;
    strcpy(slash ? slash + 1 : out, "tutorials_seen.txt");
}

static int tip_seen(const char *key)
{
    if (g_tips_n < 0) {
        g_tips_n = 0;
        char path[MAX_PATH], line[64];
        tips_path(path, MAX_PATH);
        FILE *f = fopen(path, "r");
        if (f) {
            while (g_tips_n < kTipsMax && fgets(line, sizeof line, f)) {
                line[strcspn(line, "\r\n")] = 0;
                if (line[0]) snprintf(g_tips_seen[g_tips_n++], 32, "%s", line);
            }
            fclose(f);
        }
        fprintf(stderr, "[TIPS] %d tutorials already seen (%s)\n", g_tips_n, path);
    }
    for (int i = 0; i < g_tips_n; i++)
        if (!strcmp(g_tips_seen[i], key)) return 1;
    return 0;
}

static void tip_remember(const char *key)
{
    if (g_tips_n < kTipsMax) snprintf(g_tips_seen[g_tips_n++], 32, "%s", key);
    char path[MAX_PATH];
    tips_path(path, MAX_PATH);
    FILE *f = fopen(path, "a");
    if (f) fprintf(f, "%s\n", key), fclose(f);
}

static int tutorial_key(const char *v)
{
    return !strncmp(v, "GUI_GEN_HLPTXT_", 15) || !strncmp(v, "GUI_GENACT_HLPTXT_", 18) ||
           !strncmp(v, "GUI_GENENDDAY_HLPTXT_", 21);
}

int hppoa_TutorialsOnce(void);  /* app.cpp: F10 Enhancements > Show each tutorial once */
/* A minigame's tutorials always show (the user, 2026-11-03): the game names the minigame running through the HUD
 * variable "MiniGame_Type" (0x2C9850, set from 0x20693 each frame: "0" none, "3" owl racing), and while it is not "0"
 * the help line is left as it is and nothing is written to tutorials_seen.txt */
static int g_minigame;
/* the help line's value: left as it is, or 1 to show nothing in its place */
static int tip_hide(uint32_t value_va)
{
    static int keep = -1;
    if (keep < 0) {
        const char *e = getenv("RECOMP_TUTORIAL_TIPS");
        keep = e && e[0] == '1';
    }
    char v[32];
    guest_str(value_va, v, sizeof v);
    if (!tutorial_key(v)) {
        if (g_tip_now[0] && !g_tip_left) g_tip_left = GetTickCount() | 1;
        return 0;
    }
    if (keep || !hppoa_TutorialsOnce() || g_minigame) return 0;
    if (g_tip_now[0] && !strcmp(v, g_tip_now) && (!g_tip_left || GetTickCount() - g_tip_left < kTipGraceMs)) {
        g_tip_left = 0;
        return 0;  /* its first showing goes on */
    }
    if (!tip_seen(v)) {
        tip_remember(v);
        snprintf(g_tip_now, sizeof g_tip_now, "%s", v);
        g_tip_left = 0;
        fprintf(stderr, "[TIPS] %lu tutorial %s shown, its first time\n", (unsigned long)GetTickCount(), v);
        return 0;
    }
    static char told[32];
    if (strcmp(v, told)) {
        fprintf(stderr, "[TIPS] %lu tutorial %s left out, seen before (F10 Enhancements > Show each tutorial once)\n",
                (unsigned long)GetTickCount(), v);
        snprintf(told, sizeof told, "%s", v);
    }
    return 1;
}

/* the inventory's page: the game names the Flash movie it moves to in "TransitionTooMovie" (from 0x17D8C5:
 * POA_IG_INVENTORY.swf, POA_IG_TASKS.swf, POA_IG_MAPS.swf, POA_IG.swf back in the game). The map page: d3d12.cpp's
 * SqueezeCos widens its map */
static int g_map_page;
int hppoa_MapScreen(void) { return g_map_page && hppoa_Inventory(); }

const char *app_PromptVariant(const char *key);  /* renderer/app/input.cpp */
extern void sub_001AD3B0_gen(void);
void sub_001AD3B0(void)
{
    if (MEM32(g_esp + 4) && MEM32(g_esp + 8)) {
        char name[24];
        guest_str(MEM32(g_esp + 4), name, sizeof name);
        if (!strcmp(name, "TransitionTooMovie")) {
            char v[32];
            guest_str(MEM32(g_esp + 8), v, sizeof v);
            g_map_page = !_stricmp(v, "POA_IG_MAPS.swf");
        }
    }
    if (MEM32(g_esp + 4) == 0x2C9850u && MEM32(g_esp + 8)) {  /* "MiniGame_Type" */
        char v[8];
        guest_str(MEM32(g_esp + 8), v, sizeof v);
        int now = v[0] && strcmp(v, "0");
        if (now != g_minigame) fprintf(stderr, "[TIPS] minigame %s: its tutorials %s\n", v, now ? "always shown" : "once again");
        g_minigame = now;
    }
    if (MEM32(g_esp + 4) == 0x2C96A0u && MEM32(g_esp + 8) && tip_hide(MEM32(g_esp + 8)))  /* "Help_Text_Display" */
        MEM32(g_esp + 8) = 0x2C86E8u;  /* "GUI_GENFRTXT_STRIDNONE_001": no help */
    /* Button prompts: a text key with keyboard and controller variants in the text file (input.cpp's
     * PromptsOverride: KBM_... and PAD_... beside GUI_...) is given as the one for the device in use now, so the
     * tutorials switch with it. The key's own string is changed for the call only (the HUD keeps a copy) */
    uint32_t vkey = 0;
    if (MEM32(g_esp + 8)) {
        char key[48];
        guest_str(MEM32(g_esp + 8), key, sizeof key);
        const char *pre = key[0] == 'G' ? app_PromptVariant(key) : NULL;
        if (pre) {
            vkey = MEM32(g_esp + 8);
            for (int i = 0; i < 4; i++) MEM8(vkey + i) = (uint8_t)pre[i];
        }
    }
    if (apt_trace()) {
        char name[64], value[96];
        guest_str(MEM32(g_esp + 4), name, sizeof name);
        guest_str(MEM32(g_esp + 8), value, sizeof value);
        static char last_name[64], last_value[96];
        if (strcmp(name, last_name) || strcmp(value, last_value)) {
            fprintf(stderr, "[UIVAR] %lu %s = %s (from %08X)\n", (unsigned long)GetTickCount(), name, value,
                    MEM32(g_esp));
            strcpy(last_name, name), strcpy(last_value, value);
        }
    }
    sub_001AD3B0_gen();
    if (vkey) MEM8(vkey) = 'G', MEM8(vkey + 1) = 'U', MEM8(vkey + 2) = 'I', MEM8(vkey + 3) = '_';
}
