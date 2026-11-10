/*
 * The diagnostics tool's shared block: hppoa.exe creates the named mapping "Local\hppoa_diag" at boot (hle_diag.c);
 * hppoa_diag.exe (src/diag) opens it, shows the status the game writes and asks for reports, which the game writes
 * from a thread of its own (so a game whose title threads are stuck still answers).
 */
#ifndef HPPOA_DIAG_SHM_H
#define HPPOA_DIAG_SHM_H

#include <stdint.h>

#define HPPOA_DIAG_NAME    "Local\\hppoa_diag"
#define HPPOA_DIAG_MAGIC   0x47444F50u  /* "PODG" */
#define HPPOA_DIAG_VERSION 1u

/* the game clock (hle_clock.c) */
#define HPPOA_CLOCK_VA 0x0038BA6Cu

typedef struct HppoaDiagShm {
    uint32_t magic, version;
    uint32_t pid;                  /* hppoa.exe */
    uint32_t pad0;
    uint64_t mem_offset;           /* host address = guest address + mem_offset */
    uint64_t image_base;           /* hppoa.exe's load address (stack addresses are relative to it) */
    char exe_dir[260];             /* run\, where reports go */

    /* game -> tool, refreshed about 10 times a second by the diag thread */
    volatile uint32_t heartbeat;   /* counts the diag thread's passes: stops only if the whole process is stuck */
    volatile uint32_t swaps;       /* frames presented */
    volatile uint32_t ms_since_swap;
    volatile float fps;
    volatile uint32_t clock_frames, clock_time, clock_step, clock_min;  /* the game clock (+0, +4, +8, +0x18) */
    volatile uint32_t frame_rate;  /* the setting: 30 or 60 */
    volatile uint32_t reports;     /* reports written so far (the hotkey's too) */
    volatile uint32_t hotkey_seq;  /* bumped when Ctrl+Shift+R is pressed in the game while the tool is live */
    char last_report[64];          /* its folder's name under run\reports */

    /* tool -> game: bump report_seq with report_dir and the frame count set; the game writes its part there and
     * sets report_done = report_seq once the frames are captured (or it gave up waiting for them) */
    volatile uint32_t tool_heartbeat;  /* the tool's GetTickCount: Ctrl+Shift+R goes to a live tool (with its note) */
    volatile uint32_t report_seq, report_done;
    volatile uint32_t report_frames;   /* consecutive frames to capture, 1..16 */
    char report_dir[260];
    char report_result[256];           /* the game's one-line summary of its part */
} HppoaDiagShm;

#endif
