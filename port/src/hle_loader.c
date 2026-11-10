/**
 * The game's asynchronous file reads (level packages): streamed faster than once a frame (sub_0022EC00, below).
 *
 * sub_00225130 (slot +0x40 of the file interface at 0x2ED220; cdecl-like, ret 12: file, buffer, size) puts one read
 * into a single request slot (0x30D9F0 file, 0x30D9F4 buffer, 0x30D9F8 size) and wakes the loader thread, which
 * reads it (sub_00225980 -> sub_00228E20 -> ReadFile) and calls back. A 1 MB level package came in 8 KB requests one
 * to three frames apart, about a second in all, and a doorway into the area it holds showed nothing meanwhile
 * (2026-10-26). RECOMP_LOADER_TRACE=1 logs each request ([REQ]: thread, size, the return addresses on the guest
 * stack), the first 40 after each pause of 300 ms, to find what paces them.
 *
 * Built into the recompile with --exclude-manual naming this file; gen/recomp_0015.c's body is renamed
 * sub_00225130_gen by hand until then.
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

static int trace_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv("RECOMP_LOADER_TRACE") != NULL;
    return on;
}

/* guest code runs from the XBE's text: return addresses on the stack fall in it */
static int code_address(uint32_t v) { return v >= 0x00011000u && v < 0x00270000u; }

extern void sub_00225130_gen(void);
void sub_00225130(void)
{
    if (trace_on()) {
        static ULONGLONG last;
        static int shown;
        ULONGLONG now = GetTickCount64();
        if (now - last > 300) shown = 0;
        last = now;
        if (shown++ < 40) {
            char line[512];
            int len = snprintf(line, sizeof line, "  [REQ] tid=%lu t=%llu file=%08X buf=%08X size=%u stack:",
                               GetCurrentThreadId(), now, MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(g_esp + 12));
            int found = 0;
            for (uint32_t i = 0; i < 96 && found < 10 && len < (int)sizeof line - 12; i++) {
                uint32_t v = MEM32(g_esp + i * 4);
                if (code_address(v)) len += snprintf(line + len, sizeof line - len, " %08X", v), found++;
            }
            fprintf(stderr, "%s\n", line);
        }
    }
    sub_00225130_gen();
}

/* A level package streamed into memory: the task sub_0022EC00 (registered each frame by sub_0022F2E0 while a stream
 * at [0x30DE90] is open) takes the finished reads (sub_0022E9F0), copies them into the package's buffer (sub_0022EB20:
 * +0x0C where, +0x24 bytes still to come) and queues new ones (sub_0022EA70: 8 KB blocks, at most 4 in flight, +0x30
 * queued and +0x32 done), then ends the stream once all of it is in (unregisters, [0x30DE90] = 0). Once a frame, 4
 * blocks at most: a 1 MB room took 1.0 to 1.3 s here, a doorway into it showing nothing meanwhile (2026-10-26; the
 * console, at 30 frames, would take longer still). The task is run again within the frame while the stream makes
 * progress, the loader thread given the core in between to finish the reads it was handed, for up to
 * RECOMP_PACKAGE_MS (default 6; 0: once a frame, as the game) */
#define PACKAGE_STREAM 0x30DE90u

static double package_budget_ms(void)
{
    static double ms = -1.0;
    if (ms < 0.0) {
        const char *e = getenv("RECOMP_PACKAGE_MS");
        ms = e ? atof(e) : 6.0;
        if (ms < 0.0) ms = 0.0;
        if (ms > 50.0) ms = 50.0;
        fprintf(stderr, "[LOAD] level packages streamed for up to %.1f ms a frame%s\n", ms, ms > 0.0 ? "" : " (once a frame, as the game)");
    }
    return ms;
}

extern void sub_0022EC00_gen(void);
void sub_0022EC00(void)
{
    uint32_t ret = MEM32(g_esp);  /* the task list's call; each repeat returns there too */
    sub_0022EC00_gen();
    double budget = package_budget_ms();
    if (budget <= 0.0) return;
    LARGE_INTEGER f, t0, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    int idle = 0;
    for (;;) {
        uint32_t st = MEM32(PACKAGE_STREAM);
        if (!st) break;  /* all of it in */
        QueryPerformanceCounter(&t);
        if ((double)(t.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart >= budget) break;
        uint32_t done = MEM16(st + 0x32), left = MEM32(st + 0x24);
        SwitchToThread();  /* the loader thread shares the guest core */
        g_esp -= 4;
        MEM32(g_esp) = ret;
        sub_0022EC00_gen();
        st = MEM32(PACKAGE_STREAM);
        if (st && MEM16(st + 0x32) == done && MEM32(st + 0x24) == left) {
            if (++idle >= 64) break;  /* nothing finishing: the rest next frame */
        } else {
            idle = 0;
        }
    }
}
