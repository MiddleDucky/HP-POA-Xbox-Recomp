/*
 * hppoa_diag.exe: the diagnostics window. Runs beside hppoa.exe (run_launcher.sh starts it with the launcher; start
 * either first) and talks to it through the shared block in diag_shm.h; the game writes its part of a report on a
 * thread of its own (hle_diag.c).
 *
 *   Status        running / fps / frames / the game clock; "NOT PRESENTING" when frames stop (a hang or a load)
 *   Save report   run\reports\<date_time>: the note typed here, the game's state, every thread's stack, a burst of
 *                 consecutive frame captures (each draw's full state, targets, textures with content hashes), the
 *                 logs and settings; optionally guest memory. Ctrl+Shift+R does the same from inside the game.
 *                 A game that does not answer (hung) still gets its threads' addresses, sampled from here, and logs.
 *   Crash report  when the game ends with an error code the logs and the code are saved at once, by themselves
 *   Traces        switches written to run\debug.env, read when the game next starts
 *
 * The report's folder name is what to tell Claude; port/tools/diag/report.py reads it on the Linux side.
 *
 * hppoa_diag.exe --report <s> [--quit]: a report <s> seconds after the game attaches, then (with --quit) closes once
 * it is saved: a scripted self-test.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../diag_shm.h"

enum {
    ID_TIMER = 1, ID_STATUS = 100, ID_NOTE, ID_FRAMES, ID_MEMORY, ID_REPORT, ID_RESULT, ID_OPEN,
    ID_TRACE0 = 120, WM_REPORT_DONE = WM_APP + 1,
};

/* debug.env switches: read when the game starts */
static const struct { const char *label, *line; } kTraces[] = {
    {"Game clock trace ([CLOCK] once a second)", "RECOMP_CLOCK_TRACE=1"},
    {"Renderer debug keys (F7 depth always, F8 depth log, F9 no alpha test)", "RECOMP_DEBUG_KEYS=1"},
    {"Direct3D call trace (verbose, slows the game)", "RECOMP_D3D_TRACE=1"},
    {"DirectSound trace (verbose)", "RECOMP_DSOUND_TRACE=1"},
};
#define NTRACES (int)(sizeof(kTraces) / sizeof(kTraces[0]))

static char g_dir[MAX_PATH];  /* run\, beside this exe */
static HWND g_wnd, g_status, g_note, g_frames, g_memory, g_report, g_result, g_trace[NTRACES];
static HANDLE g_map, g_proc;
static HppoaDiagShm *g_d;
static uint32_t g_pid, g_last_beat, g_beat_tick, g_hotkey_seen;
static volatile LONG g_busy;
static char g_msg[512];
static int g_auto_after = -1, g_auto_quit;  /* --report <s> [--quit] */
static DWORD g_attached_tick;

/* ── the game ─────────────────────────────────────────────────────────────── */

static void detach(void)
{
    if (g_d) UnmapViewOfFile(g_d);
    if (g_map) CloseHandle(g_map);
    if (g_proc) CloseHandle(g_proc);
    g_d = NULL, g_map = g_proc = NULL, g_pid = 0;
}

static int attach(void)
{
    if (g_d && g_d->magic == HPPOA_DIAG_MAGIC && g_d->pid == g_pid) return 1;
    detach();
    g_map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, HPPOA_DIAG_NAME);
    if (!g_map) return 0;
    g_d = (HppoaDiagShm *)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(HppoaDiagShm));
    if (!g_d || g_d->magic != HPPOA_DIAG_MAGIC || g_d->version != HPPOA_DIAG_VERSION) {
        detach();
        return 0;
    }
    g_pid = g_d->pid;
    g_hotkey_seen = g_d->hotkey_seq;
    g_attached_tick = GetTickCount();
    g_proc = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, g_pid);
    return 1;
}

/* the game's diag thread passed in the last 2 s */
static int game_answers(void)
{
    return g_d && GetTickCount() - g_beat_tick < 2000;
}

/* ── reports ──────────────────────────────────────────────────────────────── */

static void line(FILE *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
}

static int copy_in(const char *rel, const char *dir, const char *name)
{
    char from[MAX_PATH], to[MAX_PATH];
    snprintf(from, sizeof from, "%s\\%s", g_dir, rel);
    snprintf(to, sizeof to, "%s\\%s", dir, name);
    return CopyFileA(from, to, FALSE);
}

static void new_dir(char *dir, size_t len, const char *suffix)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(dir, len, "%s\\reports", g_dir);
    CreateDirectoryA(dir, NULL);
    snprintf(dir, len, "%s\\reports\\%04u%02u%02u_%02u%02u%02u%s", g_dir, st.wYear, st.wMonth, st.wDay, st.wHour,
             st.wMinute, st.wSecond, suffix);
    CreateDirectoryA(dir, NULL);
}

static const char *base(const char *dir)
{
    const char *s = strrchr(dir, '\\');
    return s ? s + 1 : dir;
}

/* every thread's instruction pointer, sampled 10 times from outside (a game whose own threads cannot answer) */
static void sample_threads(DWORD pid, const char *dir, FILE *rep)
{
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\stacks_external.txt", dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# thread: rip rsp [8 stack qwords] per sample; image base %llx\n",
            g_d ? (unsigned long long)g_d->image_base : 0ull);
    int threads = 0;
    for (int pass = 0; pass < 10; pass++, Sleep(100)) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te;
        te.dwSize = sizeof te;
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid) continue;
            HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
            if (!t) continue;
            if (SuspendThread(t) != (DWORD)-1) {
                CONTEXT c;
                memset(&c, 0, sizeof c);
                c.ContextFlags = CONTEXT_FULL;
                if (GetThreadContext(t, &c)) {
                    uint64_t stack[8] = {0};
                    SIZE_T got = 0;
                    if (g_proc) ReadProcessMemory(g_proc, (void *)c.Rsp, stack, sizeof stack, &got);
                    fprintf(f, "thread %lu: %llx %llx", (unsigned long)te.th32ThreadID, (unsigned long long)c.Rip,
                            (unsigned long long)c.Rsp);
                    for (int i = 0; i < 8; i++) fprintf(f, " %llx", (unsigned long long)stack[i]);
                    fputc('\n', f);
                    if (!pass) threads++;
                }
                ResumeThread(t);
            }
            CloseHandle(t);
        }
        CloseHandle(snap);
    }
    fclose(f);
    line(rep, "threads sampled from outside: %d (stacks_external.txt)", threads);
}

static size_t dump_guest(uint32_t va, uint32_t size, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    static uint8_t buf[1 << 20];
    size_t total = 0;
    for (uint32_t o = 0; o < size; o += sizeof buf) {
        SIZE_T got = 0;
        if (!ReadProcessMemory(g_proc, (void *)(uintptr_t)(g_d->mem_offset + va + o), buf, sizeof buf, &got))
            memset(buf, 0, sizeof buf), got = sizeof buf;  /* unmapped pages: zeros, offsets stay right */
        else total += got;
        fwrite(buf, 1, sizeof buf, f);
    }
    fclose(f);
    return total;
}

static void copy_logs(const char *dir, FILE *rep)
{
    line(rep, "copied here: last_run.log %s, hppoa_run.log %s, xbox_kernel.log %s, hppoa.toml %s, debug.env %s",
         copy_in("..\\logs\\last_run.log", dir, "last_run.log") ? "yes" : "missing",
         copy_in("hppoa_run.log", dir, "hppoa_run.log") ? "yes" : "missing",
         copy_in("xbox_kernel.log", dir, "xbox_kernel.log") ? "yes" : "missing",
         copy_in("hppoa.toml", dir, "hppoa.toml") ? "yes" : "missing",
         copy_in("debug.env", dir, "debug.env") ? "yes" : "missing");
}

typedef struct {
    char note[4096];
    int frames, memory;
} ReportArgs;

static DWORD WINAPI report_worker(LPVOID arg)
{
    ReportArgs *a = (ReportArgs *)arg;
    char dir[MAX_PATH], path[MAX_PATH];
    new_dir(dir, sizeof dir, "");
    snprintf(path, sizeof path, "%s\\note.txt", dir);
    FILE *n = fopen(path, "w");
    if (n) fprintf(n, "%s\n", a->note[0] ? a->note : "(no note)"), fclose(n);

    snprintf(path, sizeof path, "%s\\report.txt", dir);
    FILE *f = fopen(path, "w");
    if (!f) {
        snprintf(g_msg, sizeof g_msg, "Could not create %s", dir);
        PostMessageA(g_wnd, WM_REPORT_DONE, 0, 0);
        free(a);
        return 0;
    }
    line(f, "hppoa diagnostics report %s", base(dir));
    line(f, "note: %s", a->note[0] ? a->note : "(none)");
    int game = attach();
    if (game) {
        HppoaDiagShm *d = g_d;
        line(f, "game pid %u, %u frames presented, %u ms since the last, %.1f fps, frame rate setting %u", d->pid,
             d->swaps, d->ms_since_swap, d->fps, d->frame_rate);
        line(f, "game clock: frames %u time %u ms step %u min %u", d->clock_frames, d->clock_time, d->clock_step,
             d->clock_min);
    } else {
        line(f, "game: not running (no shared block)");
    }

    int answered = 0;
    if (game && game_answers()) {
        snprintf(g_d->report_dir, sizeof g_d->report_dir, "%s", dir);
        g_d->report_frames = (uint32_t)a->frames;
        uint32_t seq = ++g_d->report_seq;
        for (int i = 0; i < 250 && !answered; i++, Sleep(100)) answered = g_d->report_done == seq;
        line(f, "game's part: %s", answered ? g_d->report_result : "NO ANSWER in 25 s");
    } else if (game) {
        line(f, "game's part: none, its diagnostics thread is not running (the process is stuck)");
    }
    if (game && !answered) {
        sample_threads(g_pid, dir, f);
        copy_logs(dir, f);
    }
    if (game && a->memory && g_proc) {
        snprintf(path, sizeof path, "%s\\guest_00000000.bin", dir);
        size_t lo = dump_guest(0x00000000u, 0x04000000u, path);
        line(f, "guest memory 0x00000000..0x04000000: %zu MB read (guest_00000000.bin)", lo >> 20);
    }
    fclose(f);
    snprintf(g_msg, sizeof g_msg, "Saved: run\\reports\\%s  -  tell Claude this name", base(dir));
    PostMessageA(g_wnd, WM_REPORT_DONE, 0, 0);
    free(a);
    return 0;
}

static void start_report(void)
{
    if (InterlockedExchange(&g_busy, 1)) return;
    ReportArgs *a = (ReportArgs *)calloc(1, sizeof *a);
    GetWindowTextA(g_note, a->note, sizeof a->note);
    static const int kFrames[] = {1, 4, 8, 16};
    LRESULT sel = SendMessageA(g_frames, CB_GETCURSEL, 0, 0);
    a->frames = kFrames[sel >= 0 && sel < 4 ? sel : 2];
    a->memory = SendMessageA(g_memory, BM_GETCHECK, 0, 0) == BST_CHECKED;
    EnableWindow(g_report, FALSE);
    SetWindowTextA(g_result, "Collecting... (keep the game running; a few seconds)");
    CloseHandle(CreateThread(NULL, 0, report_worker, a, 0, NULL));
}

/* the game's console log holds a [CRASH] report in its last 64 KB: the launcher's hppoa_run.log or run_win.sh's
 * last_run.log, whichever was written last (the other is an older run's) */
static int log_has_crash(void)
{
    const char *logs[] = {"hppoa_run.log", "..\\logs\\last_run.log"};
    char path[MAX_PATH] = "";
    FILETIME best = {0, 0};
    for (int i = 0; i < 2; i++) {
        char p[MAX_PATH];
        WIN32_FILE_ATTRIBUTE_DATA fa;
        snprintf(p, sizeof p, "%s\\%s", g_dir, logs[i]);
        if (GetFileAttributesExA(p, GetFileExInfoStandard, &fa) && CompareFileTime(&fa.ftLastWriteTime, &best) > 0)
            best = fa.ftLastWriteTime, snprintf(path, sizeof path, "%s", p);
    }
    FILE *f = path[0] ? fopen(path, "rb") : NULL;
    if (!f) return 0;
    static char buf[65536 + 1];
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, n > 65536 ? n - 65536 : 0, SEEK_SET);
    size_t got = fread(buf, 1, 65536, f);
    fclose(f);
    for (size_t k = 0; k + 7 <= got; k++)
        if (!memcmp(buf + k, "[CRASH]", 7)) return 1;
    return 0;
}

/* the game ended: crashed (an exception's code, or [CRASH] in its log), the logs and the code into a crash report */
static void game_ended(void)
{
    DWORD code = 0;
    GetExitCodeProcess(g_proc, &code);
    /* a crash is an exception's code, or the game's own [CRASH] report in its log; closing the window ends the
     * process with 0 or 3 */
    if (code < 0xC0000000u && !log_has_crash()) {
        detach();
        snprintf(g_msg, sizeof g_msg, "The game quit (code %lu).", (unsigned long)code);
        SetWindowTextA(g_result, g_msg);
        return;
    }
    char dir[MAX_PATH], path[MAX_PATH];
    new_dir(dir, sizeof dir, "_crash");
    snprintf(path, sizeof path, "%s\\report.txt", dir);
    FILE *f = fopen(path, "w");
    if (f) {
        line(f, "hppoa diagnostics crash report %s", base(dir));
        line(f, "the game (pid %u) ended with code %08lX", g_pid, (unsigned long)code);
        if (g_d)
            line(f, "last seen: %u frames presented, %u ms since the last, %.1f fps, game clock time %u ms",
                 g_d->swaps, g_d->ms_since_swap, g_d->fps, g_d->clock_time);
        Sleep(500);  /* the log writers close */
        copy_logs(dir, f);
        fclose(f);
    }
    detach();
    snprintf(g_msg, sizeof g_msg, "The game CRASHED (code %08lX). Saved: run\\reports\\%s  -  tell Claude this name",
             (unsigned long)code, base(dir));
    SetWindowTextA(g_result, g_msg);
    FLASHWINFO fw = {sizeof fw, g_wnd, FLASHW_ALL | FLASHW_TIMERNOFG, 0, 0};
    FlashWindowEx(&fw);
}

/* ── traces (debug.env) ───────────────────────────────────────────────────── */

static void env_path(char *out, size_t len) { snprintf(out, len, "%s\\debug.env", g_dir); }

static int env_has(const char *want)
{
    char path[MAX_PATH], l[512];
    env_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    int found = 0;
    while (f && !found && fgets(l, sizeof l, f)) {
        l[strcspn(l, "\r\n")] = 0;
        found = !strcmp(l, want);
    }
    if (f) fclose(f);
    return found;
}

/* rewrites debug.env: its other lines kept, each switch's line there or not */
static void env_save(void)
{
    char path[MAX_PATH], l[512];
    env_path(path, sizeof path);
    static char keep[64][512];
    int nkeep = 0;
    FILE *f = fopen(path, "r");
    while (f && nkeep < 64 && fgets(l, sizeof l, f)) {
        l[strcspn(l, "\r\n")] = 0;
        int ours = 0;
        for (int i = 0; i < NTRACES; i++) {
            size_t k = strchr(kTraces[i].line, '=') - kTraces[i].line + 1;
            ours |= !strncmp(l, kTraces[i].line, k);
        }
        if (!ours) snprintf(keep[nkeep++], sizeof keep[0], "%s", l);
    }
    if (f) fclose(f);
    f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < nkeep; i++) fprintf(f, "%s\n", keep[i]);
    for (int i = 0; i < NTRACES; i++)
        if (SendMessageA(g_trace[i], BM_GETCHECK, 0, 0) == BST_CHECKED) fprintf(f, "%s\n", kTraces[i].line);
    fclose(f);
}

/* ── window ───────────────────────────────────────────────────────────────── */

static void refresh(void)
{
    char b[512];
    if (g_d && g_proc && WaitForSingleObject(g_proc, 0) == WAIT_OBJECT_0) {
        game_ended();
        return;
    }
    if (!attach()) {
        SetWindowTextA(g_status, "Game: not running yet (start it with run_launcher.sh; this window attaches by itself)");
        return;
    }
    HppoaDiagShm *d = g_d;
    if (d->heartbeat != g_last_beat) g_last_beat = d->heartbeat, g_beat_tick = GetTickCount();
    d->tool_heartbeat = GetTickCount();
    if (g_auto_after >= 0 && GetTickCount() - g_attached_tick >= (DWORD)g_auto_after * 1000u) {
        g_auto_after = -1;
        SetWindowTextA(g_note, "(scripted self-test: hppoa_diag.exe --report)");
        start_report();
    }
    if (d->hotkey_seq != g_hotkey_seen) {  /* Ctrl+Shift+R in the game */
        g_hotkey_seen = d->hotkey_seq;
        start_report();
    }
    const char *state = !game_answers()          ? "PROCESS STUCK (its diagnostics thread stopped)"
                        : d->ms_since_swap > 3000 ? "NOT PRESENTING (loading, or hung)"
                                                  : "running";
    snprintf(b, sizeof b,
             "Game: %s   (pid %u)\r\n%.0f fps   frames %u   game clock %u.%03u s, step %u ms (min %u)   setting %u fps"
             "   reports %u%s%s",
             state, d->pid, d->fps, d->swaps, d->clock_time / 1000, d->clock_time % 1000, d->clock_step, d->clock_min,
             d->frame_rate, d->reports, d->last_report[0] ? ", last " : "", d->last_report);
    SetWindowTextA(g_status, b);
}

static HWND add(const char *cls, const char *text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExA(!strcmp(cls, "EDIT") ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y,
                             w, h, g_wnd, (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return c;
}

static void build(void)
{
    int y = 10;
    g_status = add("STATIC", "", 0, 10, y, 620, 34, ID_STATUS);
    y += 42;
    add("STATIC", "What went wrong? Where, what you did, what you expected (optional, saved with the report):", 0, 10,
        y, 620, 16, 0);
    y += 18;
    g_note = add("EDIT", "", ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_WANTRETURN, 10, y, 620, 90, ID_NOTE);
    y += 98;
    add("STATIC", "Frames to capture:", 0, 10, y + 4, 100, 16, 0);
    g_frames = add("COMBOBOX", "", CBS_DROPDOWNLIST, 112, y, 70, 120, ID_FRAMES);
    const char *fr[] = {"1", "4", "8", "16"};
    for (int i = 0; i < 4; i++) SendMessageA(g_frames, CB_ADDSTRING, 0, (LPARAM)fr[i]);
    SendMessageA(g_frames, CB_SETCURSEL, 2, 0);
    g_memory = add("BUTTON", "Guest memory too (64 MB)", BS_AUTOCHECKBOX, 200, y + 2, 180, 20, ID_MEMORY);
    y += 30;
    g_report = add("BUTTON", "Save report  (Ctrl+Shift+R, also in the game)", BS_DEFPUSHBUTTON, 10, y, 300, 30,
                   ID_REPORT);
    add("BUTTON", "Open reports folder", 0, 320, y, 150, 30, ID_OPEN);
    y += 36;
    g_result = add("STATIC", "Press Save report while the problem is on screen.", 0, 10, y, 620, 32, ID_RESULT);
    y += 40;
    add("STATIC", "Traces (written to debug.env, take effect the next time the game starts):", 0, 10, y, 620, 16, 0);
    y += 20;
    for (int i = 0; i < NTRACES; i++, y += 22) {
        g_trace[i] = add("BUTTON", kTraces[i].label, BS_AUTOCHECKBOX, 20, y, 600, 20, ID_TRACE0 + i);
        SendMessageA(g_trace[i], BM_SETCHECK, env_has(kTraces[i].line) ? BST_CHECKED : BST_UNCHECKED, 0);
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
        case WM_CREATE:
            g_wnd = h;
            build();
            SetTimer(h, ID_TIMER, 250, NULL);
            refresh();
            return 0;
        case WM_TIMER:
            refresh();
            return 0;
        case WM_COMMAND:
            if (LOWORD(w) == ID_REPORT) start_report();
            if (LOWORD(w) == ID_OPEN) {
                char p[MAX_PATH];
                snprintf(p, sizeof p, "%s\\reports", g_dir);
                CreateDirectoryA(p, NULL);
                ShellExecuteA(h, "open", p, NULL, NULL, SW_SHOWNORMAL);
            }
            if (LOWORD(w) >= ID_TRACE0 && LOWORD(w) < ID_TRACE0 + NTRACES) env_save();
            return 0;
        case WM_REPORT_DONE:
            SetWindowTextA(g_result, g_msg);
            EnableWindow(g_report, TRUE);
            InterlockedExchange(&g_busy, 0);
            if (g_auto_quit) DestroyWindow(h);
            return 0;
        case WM_CTLCOLORSTATIC:
            SetBkMode((HDC)w, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        case WM_DESTROY:
            if (g_d) g_d->tool_heartbeat = 0;
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    (void)prev;
    const char *ra = strstr(cmd, "--report ");
    if (ra) g_auto_after = atoi(ra + 9);
    g_auto_quit = strstr(cmd, "--quit") != NULL;
    DWORD n = GetModuleFileNameA(NULL, g_dir, sizeof g_dir);
    char *slash = n ? strrchr(g_dir, '\\') : NULL;
    if (slash) *slash = 0;
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = "hppoa_diag";
    RegisterClassA(&wc);
    RECT r = {0, 0, 640, 430};
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    CreateWindowA("hppoa_diag", "HP: Prisoner of Azkaban - diagnostics", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                  WS_MINIMIZEBOX | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL,
                  NULL, inst, NULL);
    (void)show;
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        /* Ctrl+Shift+R here too */
        if (msg.message == WM_KEYDOWN && msg.wParam == 'R' && (GetKeyState(VK_CONTROL) & 0x8000) &&
            (GetKeyState(VK_SHIFT) & 0x8000)) {
            start_report();
            continue;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_TAB && IsDialogMessageA(g_wnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    detach();
    return 0;
}
