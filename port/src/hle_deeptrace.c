/**
 * HPPOA_CALL_PROFILE build, on top of profiler.c's counts: what each window between two F7s did.
 *
 *   calls_<tick>_N.trace  each call: time, frame, target, return address, ecx, three stack arguments, thread. The first
 *                         kCap calls per target only (the buffer must outlast the window: the event at its end is the one that matters), so a hot function cannot crowd the rest out.
 *   calls_<tick>_N.snap   every 100 ms: the game clock (0x38BA6C) and the first 0x2000 bytes of the kSnapObjects objects
 *                         most used as ecx, so a field that climbs (a tug of war's progress) shows at each frame rate.
 *
 * Reads only. Written from profiler.c's thread; the game writes to the other buffer meanwhile. A window over 60 s is the
 * run up to the first F7: its counts are kept, its detail dropped.
 */
#ifdef RECOMP_CALL_PROFILE
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <intrin.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECOMP_GENERATED_CODE
#include "recomp/gen/recomp_types.h"

uint32_t hppoa_SwapCount(void);  /* hle_d3d8.cpp: frames presented */

enum { kCap = 1500, kSlots = 0x400000, kRows = 6000000, kEcxSlots = 4096, kSnapObjects = 24, kSnapBytes = 0x2000 };
#define SNAP_BUFFER (384u * 1024u * 1024u)
#define GAME_CLOCK 0x0038BA6Cu

typedef struct { uint64_t tsc; uint32_t frame, va, ret, ecx, a[3], tid; } Row;  /* 40 bytes */
typedef struct { uint32_t magic, ms, frame, addr, len; } SnapHead;

static Row *g_rows[2];
static uint8_t *g_snap;
static volatile LONG g_active, g_next, g_snap_used;
static volatile LONG g_cap[kSlots];
static struct { volatile uint32_t ecx; volatile LONG n; } g_ecx_seen[kEcxSlots];
static LARGE_INTEGER g_qpc0, g_freq;
static uint64_t g_tsc0;
static HANDLE g_thread;

static int in_ram(uint32_t a) { return (a & 0xFC000000u) == 0x80000000u && a + kSnapBytes < 0x84000000u; }

void hppoa_DeepHit(uint32_t va)
{
    if (!g_rows[0] || va >= kSlots) return;
    uint32_t sp = g_esp, cx = g_ecx;
    if (in_ram(cx)) {
        uint32_t h = (cx * 2654435761u) >> 20;  /* 12 bits */
        if (g_ecx_seen[h].ecx == cx) g_ecx_seen[h].n++;
        else if (!g_ecx_seen[h].ecx) { g_ecx_seen[h].ecx = cx; g_ecx_seen[h].n = 1; }
    }
    if (g_cap[va] >= kCap) return;
    g_cap[va]++;
    LONG i = InterlockedIncrement(&g_next) - 1;
    if (i >= kRows) return;
    Row *r = &g_rows[g_active & 1][i];
    r->tsc = __rdtsc(); r->frame = hppoa_SwapCount(); r->va = va; r->ecx = cx; r->tid = GetCurrentThreadId();
    r->ret = MEM32(sp); r->a[0] = MEM32(sp + 4); r->a[1] = MEM32(sp + 8); r->a[2] = MEM32(sp + 12);
}

static void snap_one(uint32_t addr, uint32_t len, uint32_t ms, uint32_t frame)
{
    LONG need = (LONG)(sizeof(SnapHead) + len), at = InterlockedExchangeAdd(&g_snap_used, need);
    if ((uint32_t)at + (uint32_t)need > SNAP_BUFFER) return;
    SnapHead h = {0x50414E53u, ms, frame, addr, len};
    memcpy(g_snap + at, &h, sizeof h);
    memcpy(g_snap + at + sizeof h, (const void *)XBOX_PTR(addr), len);
}

static uint32_t now_ms(void)
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return (uint32_t)((q.QuadPart - g_qpc0.QuadPart) * 1000 / g_freq.QuadPart);
}

static DWORD WINAPI snap_thread(LPVOID unused)
{
    (void)unused;
    for (;; Sleep(100)) {
        if (!g_xbox_mem_offset || !hppoa_SwapCount()) continue;  /* guest memory not mapped yet, no frame shown yet */
        uint32_t top[kSnapObjects] = {0}, topn[kSnapObjects] = {0};
        for (int i = 0; i < kEcxSlots; i++) {
            uint32_t a = g_ecx_seen[i].ecx, n = (uint32_t)g_ecx_seen[i].n;
            if (!a) continue;
            for (int k = 0; k < kSnapObjects; k++)
                if (n > topn[k]) {
                    memmove(&top[k + 1], &top[k], sizeof(top[0]) * (size_t)(kSnapObjects - k - 1));
                    memmove(&topn[k + 1], &topn[k], sizeof(top[0]) * (size_t)(kSnapObjects - k - 1));
                    top[k] = a, topn[k] = n;
                    break;
                }
        }
        uint32_t ms = now_ms(), frame = hppoa_SwapCount();
        snap_one(GAME_CLOCK, 0x40, ms, frame);
        for (int k = 0; k < kSnapObjects; k++)
            if (top[k]) snap_one(top[k], kSnapBytes, ms, frame);
    }
    return 0;
}

/* profiler.c: F7. `base` is the dump's path without its extension, ms the window's length. */
void hppoa_DeepDump(const char *base, unsigned long ms)
{
    if (!g_rows[0]) return;
    LONG n = g_next, used = g_snap_used, was = g_active & 1;
    if (n > kRows) n = kRows;
    InterlockedExchange(&g_active, !was);
    InterlockedExchange(&g_next, 0);
    memset((void *)g_cap, 0, sizeof g_cap);
    if ((uint32_t)used > SNAP_BUFFER) used = SNAP_BUFFER;
    if (ms <= 60000) {
        char path[96];
        snprintf(path, sizeof path, "%s.trace", base);
        FILE *f = fopen(path, "wb");
        if (f) {
            LARGE_INTEGER q;
            QueryPerformanceCounter(&q);
            uint64_t tsc = __rdtsc(), hz = (uint64_t)((double)(tsc - g_tsc0) * (double)g_freq.QuadPart / (double)(q.QuadPart - g_qpc0.QuadPart));
            fwrite(&hz, sizeof hz, 1, f);
            fwrite(g_rows[was], sizeof(Row), (size_t)n, f);
            fclose(f);
        }
        snprintf(path, sizeof path, "%s.snap", base);
        f = fopen(path, "wb");
        if (f) { fwrite(g_snap, 1, (size_t)used, f); fclose(f); }
        fprintf(stderr, "[PROFILE] detail: %ld calls, %ld MB of snapshots, %s.trace/.snap\n", (long)n, (long)(used >> 20), base);
    } else {
        fprintf(stderr, "[PROFILE] window %lu ms: detail dropped (the run up to the first F7)\n", ms);
    }
    InterlockedExchange(&g_snap_used, 0);
    /* the object ranking starts over, so the snapshots follow what this window uses */
    for (int i = 0; i < kEcxSlots; i++) g_ecx_seen[i].n = 0, g_ecx_seen[i].ecx = 0;
}

void hppoa_DeepStart(void)
{
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_qpc0);
    g_tsc0 = __rdtsc();
    g_snap = VirtualAlloc(NULL, SNAP_BUFFER, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    Row *a = VirtualAlloc(NULL, sizeof(Row) * kRows, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    Row *b = VirtualAlloc(NULL, sizeof(Row) * kRows, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_snap || !a || !b) { fprintf(stderr, "[PROFILE] no memory for the detail buffers\n"); return; }
    g_rows[1] = b; g_rows[0] = a;
    g_thread = CreateThread(NULL, 0, snap_thread, NULL, 0, NULL);
}
#endif
