/**
 * The diagnostics report, the game's side (hppoa_diag.exe is the other, src/diag).
 *
 * A thread of its own (not the title's, so a game whose title threads are stuck still answers) keeps the shared
 * block in diag_shm.h up to date and writes reports: run\reports\<date_time>\ gets
 *   state.txt      frames, fps, the game clock, the settings in force, every RECOMP_* variable
 *   stacks.txt     every thread's native call stack, held locks, the I/O workers (profiler.c)
 *   frame_NN\      a burst of consecutive frame captures (frame.txt with every draw's state, targets, textures)
 *   logs           last_run.log (run_win.sh), hppoa_run.log (the launcher), xbox_kernel.log, hppoa.toml, debug.env
 * The tool asks through the block and adds the user's note, guest memory and, for a hung or crashed game, what it
 * can gather from outside. Without the tool Ctrl+Shift+R in the game writes the same report (8 frames).
 *
 * Nothing here wraps a guest function, so the recompile does not need --exclude-manual for this file.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diag_shm.h"

extern ptrdiff_t g_xbox_mem_offset;
void hppoa_StackDumpTo(const char *path);                  /* profiler.c */
uint32_t hppoa_SwapCount(void);                            /* hle_d3d8.cpp */
uint32_t hppoa_MsSinceSwap(void);
float hppoa_Fps(void);
void hppoa_CaptureBurst(const char *dir, int frames);
uint32_t hppoa_CaptureBursts(void);
uint32_t hppoa_FrameRate(void);                            /* app.cpp */
double xbox_VblankHz(void);                                /* kernel_bridge.c */

static HppoaDiagShm *g_d;

static uint32_t guest32(uint32_t va)
{
    return g_xbox_mem_offset ? *(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + va) : 0;
}

static void copy_in(const char *from, const char *dir, const char *name, FILE *index)
{
    char to[MAX_PATH];
    snprintf(to, sizeof to, "%s\\%s", dir, name);
    BOOL ok = CopyFileA(from, to, FALSE);
    fprintf(index, "  %-16s %s\n", name, ok ? "copied" : "missing");
}

static void write_state(const char *dir)
{
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\state.txt", dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "time %04u-%02u-%02u %02u:%02u:%02u.%03u\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
            st.wSecond, st.wMilliseconds);
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExA(exe, GetFileExInfoStandard, &fa)) {
        FILETIME lt;
        SYSTEMTIME bt;
        FileTimeToLocalFileTime(&fa.ftLastWriteTime, &lt);
        FileTimeToSystemTime(&lt, &bt);
        fprintf(f, "exe %s, %lu bytes, built %04u-%02u-%02u %02u:%02u:%02u\n", exe, (unsigned long)fa.nFileSizeLow,
                bt.wYear, bt.wMonth, bt.wDay, bt.wHour, bt.wMinute, bt.wSecond);
    }
    fprintf(f, "image base %llx, guest -> host offset %llx\n", (unsigned long long)(uintptr_t)GetModuleHandleA(NULL),
            (unsigned long long)g_xbox_mem_offset);
    fprintf(f, "frames presented %u, %u ms since the last, %.1f fps\n", hppoa_SwapCount(), hppoa_MsSinceSwap(),
            hppoa_Fps());
    fprintf(f, "frame rate setting %u, vblank %.3f Hz\n", hppoa_FrameRate(), xbox_VblankHz());
    uint32_t c = HPPOA_CLOCK_VA;
    fprintf(f, "game clock %08X: frames %u time %u ms step %u div %u origin %u fixed %u min %u\n", c, guest32(c),
            guest32(c + 4), guest32(c + 8), guest32(c + 0xC), guest32(c + 0x10), guest32(c + 0x14), guest32(c + 0x18));
    fprintf(f, "\nenvironment (RECOMP_*, WINE*):\n");
    char *env = GetEnvironmentStringsA();
    for (char *e = env; e && *e; e += strlen(e) + 1)
        if (!strncmp(e, "RECOMP_", 7) || !strncmp(e, "WINE", 4)) fprintf(f, "  %s\n", e);
    if (env) FreeEnvironmentStringsA(env);
    fclose(f);
}

/* the game's part of a report into dir (which exists); returns once the frames are captured or 15 s passed */
static void write_report(const char *dir, int frames, char *result, size_t len)
{
    fflush(stdout);
    fflush(stderr);
    write_state(dir);
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\stacks.txt", dir);
    hppoa_StackDumpTo(path);

    uint32_t before = hppoa_CaptureBursts();
    hppoa_CaptureBurst(dir, frames);
    int got = 0;
    for (int i = 0; i < 150 && !got; i++, Sleep(100)) got = hppoa_CaptureBursts() != before;

    /* the logs last, so they hold the capture's own lines */
    fflush(stderr);
    snprintf(path, sizeof path, "%s\\files.txt", dir);
    FILE *index = fopen(path, "w");
    if (index) {
        fprintf(index, "copied by the game:\n");
        copy_in("..\\logs\\last_run.log", dir, "last_run.log", index);
        copy_in("hppoa_run.log", dir, "hppoa_run.log", index);
        copy_in("xbox_kernel.log", dir, "xbox_kernel.log", index);
        copy_in("hppoa.toml", dir, "hppoa.toml", index);
        copy_in("debug.env", dir, "debug.env", index);
        fclose(index);
    }
    snprintf(result, len, "state, stacks, logs; %d frame%s %s", frames, frames == 1 ? "" : "s",
             got ? "captured" : "NOT captured (no frame presented in 15 s: loading or hung)");
    fprintf(stderr, "[DIAG] report %s: %s\n", dir, result);
}

static void make_dir(char *out, size_t len)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(out, len, "%s\\reports", g_d->exe_dir);
    CreateDirectoryA(out, NULL);
    snprintf(out, len, "%s\\reports\\%04u%02u%02u_%02u%02u%02u", g_d->exe_dir, st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);
    CreateDirectoryA(out, NULL);
}

static void finished(const char *dir)
{
    const char *name = strrchr(dir, '\\');
    snprintf(g_d->last_report, sizeof g_d->last_report, "%s", name ? name + 1 : dir);
    g_d->reports++;
}

/* RECOMP_DIAG_SNAP=<s>,<s>,...: guest memory 0..64 MB and 0x80000000..+64 MB (128 MB) to run\snaps\snap_<s>.bin that many seconds after boot (a
 * scripted run's before / after pictures, diffed on the Linux side) */
static void snapshots(void)
{
    static int n = -1, next;
    static unsigned at[16];
    static DWORD t0;
    if (n < 0) {
        n = 0, t0 = GetTickCount();
        const char *e = getenv("RECOMP_DIAG_SNAP");
        for (const char *p = e; p && *p && n < 16; p = strchr(p, ',') ? strchr(p, ',') + 1 : "") at[n++] = (unsigned)atoi(p);
    }
    if (next >= n || !g_xbox_mem_offset || GetTickCount() - t0 < at[next] * 1000u) return;
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\snaps", g_d->exe_dir);
    CreateDirectoryA(path, NULL);
    snprintf(path, sizeof path, "%s\\snaps\\snap_%u.bin", g_d->exe_dir, at[next]);
    FILE *f = fopen(path, "wb");
    if (f) {
        /* 0x00000000 and the contiguous window at 0x80000000 (its own storage here), 64 MB each, page by page:
         * a page not mapped is written as zeros so offsets stay put */
        static uint8_t zero[4096];
        for (int w = 0; w < 2; w++)
            for (uint32_t pg = 0; pg < 0x04000000u; pg += 4096) {
                const uint8_t *src = (const uint8_t *)(uintptr_t)g_xbox_mem_offset + (w ? 0x80000000u : 0) + pg;
                MEMORY_BASIC_INFORMATION mi;
                int ok = VirtualQuery(src, &mi, sizeof mi) && mi.State == MEM_COMMIT &&
                         !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
                fwrite(ok ? src : zero, 1, 4096, f);
            }
        fclose(f);
    }
    fprintf(stderr, "[DIAG] snapshot %s at %lu ms\n", path, (unsigned long)(GetTickCount() - t0));
    next++;
}

/* RECOMP_DIAG_WATCHSTR=<text>@<s>: that many seconds after boot, the first copy of the string <text> in guest
 * memory (0..64 MB, then the contiguous window) gets a read/write hardware watch (main.c): which code reads it */
void hppoa_WatchArm(uint32_t va, int read);
static void watch_string(void)
{
    static int state = -1;
    static char text[128];
    static unsigned at;
    static int skip;
    static DWORD t0;
    if (state < 0) {
        state = 0, t0 = GetTickCount();
        const char *e = getenv("RECOMP_DIAG_WATCHSTR");
        const char *amp = e ? strrchr(e, '@') : NULL;
        if (amp && amp - e < (int)sizeof text) {
            memcpy(text, e, (size_t)(amp - e)), text[amp - e] = 0, at = (unsigned)atoi(amp + 1), state = 1;
            const char *hash = strchr(amp, '#');  /* <text>@<s>#<n>: the n-th copy (1 the first) */
            skip = hash ? atoi(hash + 1) - 1 : 0;
        }
    }
    if (state != 1 || GetTickCount() - t0 < at * 1000u || !g_xbox_mem_offset) return;
    state = 2;
    size_t n = strlen(text);  /* without its NUL: a copy may be length-prefixed */
    const uint32_t bases[2] = {0x80000000u, 0x00010000u};
    for (int w = 0; w < 2; w++)
        for (uint32_t pg = 0; pg < 0x03FF0000u; pg += 4096) {
            const uint8_t *p = (const uint8_t *)(uintptr_t)g_xbox_mem_offset + bases[w] + pg;
            MEMORY_BASIC_INFORMATION mi;
            if (!VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                continue;
            for (uint32_t k = 0; k + n <= 4096; k++)
                if (p[k] == (uint8_t)text[0] && !memcmp(p + k, text, n) && skip-- <= 0) {
                    uint32_t va = bases[w] + pg + k;
                    fprintf(stderr, "[DIAG] watch string \"%s\" at %08X\n", text, va);
                    hppoa_WatchArm(va, 1);
                    return;
                }
        }
    fprintf(stderr, "[DIAG] watch string \"%s\" not found\n", text);
}

static DWORD WINAPI diag_thread(LPVOID arg)
{
    (void)arg;
    int was_down = 0;
    for (unsigned pass = 0;; pass++) {
        Sleep(20);  /* the hotkey's poll; the status every fifth pass */
        HppoaDiagShm *d = g_d;
        int down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
                   (GetAsyncKeyState('R') & 0x8000);
        int pressed = down && !was_down;
        was_down = down;
        /* RECOMP_DIAG_SELFTEST=<s>: Ctrl+Shift+R pressed that long after boot (a scripted test of this path) */
        static DWORD selftest = 1;
        if (selftest == 1) {
            const char *e = getenv("RECOMP_DIAG_SELFTEST");
            selftest = e && atoi(e) > 0 ? GetTickCount() + (DWORD)atoi(e) * 1000u : 0;
        }
        if (selftest && (int)(GetTickCount() - selftest) >= 0) selftest = 0, pressed = 1;
        snapshots();
        watch_string();
        if (!pressed && pass % 5) continue;
        d->heartbeat++;
        d->mem_offset = (uint64_t)g_xbox_mem_offset;
        d->swaps = hppoa_SwapCount();
        d->ms_since_swap = hppoa_MsSinceSwap();
        d->fps = hppoa_Fps();
        d->clock_frames = guest32(HPPOA_CLOCK_VA);
        d->clock_time = guest32(HPPOA_CLOCK_VA + 4);
        d->clock_step = guest32(HPPOA_CLOCK_VA + 8);
        d->clock_min = guest32(HPPOA_CLOCK_VA + 0x18);
        d->frame_rate = hppoa_FrameRate();

        /* the tool asks */
        uint32_t seq = d->report_seq;
        if (seq != d->report_done) {
            char dir[260], result[256];
            memcpy(dir, d->report_dir, sizeof dir);
            dir[sizeof dir - 1] = 0;
            uint32_t n = d->report_frames;
            write_report(dir, n >= 1 && n <= 16 ? (int)n : 8, result, sizeof result);
            memcpy(d->report_result, result, sizeof result);
            finished(dir);
            d->report_done = seq;
            continue;
        }
        /* Ctrl+Shift+R in the game: a live tool makes the report (with its note and memory), else the game alone */
        int tool = d->tool_heartbeat && GetTickCount() - d->tool_heartbeat < 3000;
        if (pressed && tool) {
            d->hotkey_seq++;
        } else if (pressed) {
            char dir[260], result[256];
            make_dir(dir, sizeof dir);
            write_report(dir, 8, result, sizeof result);
            char path[MAX_PATH];
            snprintf(path, sizeof path, "%s\\report.txt", dir);
            FILE *f = fopen(path, "w");
            if (f) {
                fprintf(f, "hppoa diagnostics report (Ctrl+Shift+R in the game, no diagnostics window)\n%s\n",
                        result);
                fclose(f);
            }
            finished(dir);
        }
    }
    return 0;
}

/* main.c, once guest memory is mapped */
void hppoa_DiagInit(void)
{
    HANDLE m = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(HppoaDiagShm),
                                  HPPOA_DIAG_NAME);
    if (!m) return;
    g_d = (HppoaDiagShm *)MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(HppoaDiagShm));
    if (!g_d) return;
    memset(g_d, 0, sizeof *g_d);
    g_d->pid = GetCurrentProcessId();
    g_d->mem_offset = (uint64_t)g_xbox_mem_offset;
    g_d->image_base = (uint64_t)(uintptr_t)GetModuleHandleA(NULL);
    DWORD n = GetModuleFileNameA(NULL, g_d->exe_dir, sizeof g_d->exe_dir);
    char *slash = n ? strrchr(g_d->exe_dir, '\\') : NULL;
    if (slash) *slash = 0;
    g_d->version = HPPOA_DIAG_VERSION;
    MemoryBarrier();
    g_d->magic = HPPOA_DIAG_MAGIC;
    CloseHandle(CreateThread(NULL, 0, diag_thread, NULL, 0, NULL));
}
