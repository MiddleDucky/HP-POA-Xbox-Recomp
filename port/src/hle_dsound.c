/**
 * DirectSound (XDK 5849) on a host software mixer, in place of the emulated
 * MCPX APU.
 *
 * The recompiled XDK DirectSound drives the APU's registers and waits on its
 * interrupts for stream packets to complete. The emulated APU does not keep
 * real time, so XMV playback starved its audio stream and the intro stalled.
 * Here the public DirectSound entry points are implemented on the host:
 * buffers and stream packets are read straight from guest memory, mixed on a
 * mixer thread paced by XAudio2, and stream packets complete as they play.
 *
 * Logic ported from SCDA-XBERecomp's src/xdk/dsound.cpp (MIT, Copyright (c)
 * 2026 TGP482, Joshhhuaaa), which targets XDK 5849, by way of the HP: Chamber of
 * Secrets (XDK 5028) and Prisoner of Azkaban (XDK 5659) ports. The entry points
 * this title calls (buffers only: it streams nothing through DirectSound) and their argument counts were checked
 * against this XBE: addresses from XbSymbolDatabase, argument counts from each function's `ret n`.
 *
 * Every entry point keeps the generated XDK body as sub_X_gen, so
 * RECOMP_HLE_DSOUND=0 runs the original DirectSound on the emulated APU.
 * RECOMP_DSOUND_TRACE=1 logs the calls.
 *
 * Built into the recompile with --exclude-manual pointing at this file.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define COBJMACROS
#include <windows.h>
#include <xaudio2.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the register names (eax, esp, ...) that RECOMP_ICALL expands to */
#define RECOMP_GENERATED_CODE
#include "recomp/gen/recomp_types.h"

#ifdef _MSC_VER
#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")
#endif

uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);
void xbox_HeapFree(uint32_t xbox_va);

/* ── Guest constants (XDK 5849 DSOUND in this XBE) ───────────────────────── */


#define S_OK_           0x00000000u
#define E_FAIL_         0x80004005u
#define DSERR_INVALID   0x88780032u
#define XMEDIAPACKET_STATUS_PENDING  0x8000000Au
#define XMEDIAPACKET_STATUS_FLUSHED  0x80004004u

/* ── Arguments and returns, __stdcall on the guest stack ─────────────────── */

#define ARG(i)  MEM32(g_esp + 4 + 4 * (i))

static void ret_stdcall(uint32_t result, int nargs)
{
    g_eax = result;
    g_esp += 4 + 4 * nargs;
}

static int g_hle = -1, g_trace;

static int hle_on(void)
{
    if (g_hle < 0) {
        const char *e = getenv("RECOMP_HLE_DSOUND");
        const char *t = getenv("RECOMP_DSOUND_TRACE");
        g_hle = !(e && *e == '0');
        g_trace = t && *t && *t != '0';
        fprintf(stderr, "[DSOUND] %s\n", g_hle ? "host mixer (HLE)" : "XDK DirectSound on the emulated APU");
    }
    return g_hle;
}

int app_AudioTrace(void);  /* app.cpp: F10 Debug > Audio trace, switched while running */
static volatile int g_trace_ui;  /* app_AudioTrace(), read by the mixer once a pass */
#define TRACE(...) do { if (g_trace || g_trace_ui) { fprintf(stderr, "[DSOUND] " __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* ── Voices ─────────────────────────────────────────────────────────────── */

#define MIX_RATE        48000
/* Mixed per pass, ~5.3 ms. The music streams through a ~50 ms looping buffer that the title refills by polling
 * GetCurrentPosition; the console reads it sample by sample. 1024 frames (~21 ms) a pass moved the cursor and read
 * ahead in jumps that ate half of the title's refill margin, and a slow game frame then let the mixer lap the
 * writer: stale audio, heard as clicks. */
#define MIX_FRAMES      256
#define MAX_VOICES      256
#define MAX_PACKETS     64
#define MAX_DONE        1024
#define ADPCM_FRAMES    64   /* Xbox ADPCM plays 64 frames per 36-byte block, not IMA's 65 */

typedef struct {
    uint32_t data, size, completed_ptr, status_ptr, context;
} Packet;

typedef struct {
    uint32_t obj;
    int stream, refs;
    /* format */
    uint16_t tag, channels, bits, align;
    uint32_t rate, freq;
    /* buffer */
    uint32_t data, bytes, loop_start, loop_len, owned;
    int playing, looping, paused, active, ending;
    double pos;  /* frames */
    /* stream */
    uint32_t max_packets, callback, context;
    Packet packets[MAX_PACKETS];
    int npackets;
    /* levels */
    float volume, headroom;
    uint8_t nbins, bin[8];
    int32_t bin_mb[32];
    float gain[2][2];
    float gain6[6][6];  /* 5.1 output (render6): source channel -> speaker */
    int has_gain;
    int fading_out;  /* Stop: one more pass ramping to silence, then stopped */
    /* 3D (DSBCAPS_CTRL3D): position relative to the listener, which this title never moves */
    int is3d;
    float pos3d[3], min_dist, max_dist;
    /* loudness of what it played in the last mix pass (the speech follower below) */
    double lvl_acc;
    uint32_t lvl_n;
    float level;
    int quiet_at_arm;
    DWORD loud_since;  /* when its current loud stretch began, 0 while quiet */
    /* ADPCM block cache */
    int64_t block;
    int16_t decoded[65 * 8];
} Voice;

typedef struct {
    uint32_t voice, callback, context, packet_context, status;
} Done;

static CRITICAL_SECTION g_lock;
static Voice *g_voices[MAX_VOICES];
static Done g_done[MAX_DONE];
static int g_ndone;
static int g_started;
static uint32_t g_ds;

static float db(int32_t hundredths)
{
    return hundredths <= -10000 ? 0.0f : powf(10.0f, hundredths / 2000.0f);
}

static uint32_t frames_of(const Voice *v, uint32_t bytes)
{
    if (v->tag == 0x69) return bytes / (36u * v->channels) * ADPCM_FRAMES;
    return bytes / (v->align ? v->align : 1);
}

static uint32_t bytes_of(const Voice *v, uint32_t frames)
{
    if (v->tag == 0x69) return frames / ADPCM_FRAMES * 36 * v->channels;
    return frames * v->align;
}

static const int k_index[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
static const int k_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767};

static int clampi(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }

/* One 36-byte-per-channel IMA block: 65 frames decoded, ADPCM_FRAMES played. */
static void decode_block(const uint8_t *b, int ch, int16_t *out)
{
    for (int c = 0; c < ch; c++) {
        int pred = (int16_t)(b[c * 4] | b[c * 4 + 1] << 8), idx = b[c * 4 + 2] > 88 ? 88 : b[c * 4 + 2];
        const uint8_t *d = b + ch * 4;
        out[c] = (int16_t)pred;
        for (int k = 0; k < 8; k++)
            for (int j = 0; j < 8; j++) {
                int n = (d[(k * ch + c) * 4 + j / 2] >> ((j & 1) * 4)) & 15;
                int step = k_step[idx], diff = step >> 3;
                if (n & 1) diff += step >> 2;
                if (n & 2) diff += step >> 1;
                if (n & 4) diff += step;
                pred = clampi(n & 8 ? pred - diff : pred + diff, -32768, 32767);
                idx = clampi(idx + k_index[n], 0, 88);
                out[(1 + k * 8 + j) * ch + c] = (int16_t)pred;
            }
    }
}

/* Sample of channel c at frame f of the memory at guest address data. */
static float sample(Voice *v, uint32_t data, uint32_t f, int c)
{
    if (c >= v->channels) c = v->channels - 1;
    if (v->tag == 0x69) {
        int64_t at = (int64_t)data + (int64_t)(f / ADPCM_FRAMES) * 36 * v->channels;
        if (at != v->block) {
            decode_block((const uint8_t *)XBOX_PTR((uint32_t)at), v->channels > 8 ? 8 : v->channels, v->decoded);
            v->block = at;
        }
        return v->decoded[(f % ADPCM_FRAMES) * v->channels + c] / 32768.0f;
    }
    if (v->bits == 8) return ((int)MEM8(data + f * v->align + c) - 128) / 128.0f;
    return (int16_t)MEM16(data + f * v->align + c * 2) / 32768.0f;
}

/* Packet done: the XDK writes its sizes and status, then queues the stream's
 * callback for DirectSoundDoWork to run on the title's thread. */
/* One side of a frame. A buffer of more than two channels (this title's own mixer writes 5.1 into one 6 channel
 * buffer: front left, front right, centre, LFE, back left, back right) folds down to stereo with the ITU weights:
 * the centre carries the dialogue, and reading only the first two channels lost every spoken line. LFE is left out,
 * as in a standard fold-down. */
static float sample_lr(Voice *v, uint32_t data, uint32_t f, int side)
{
    if (v->channels <= 2) return sample(v, data, f, side);
    float x = sample(v, data, f, side) + 0.7071f * sample(v, data, f, 2);
    if (v->channels >= 6) x += 0.7071f * sample(v, data, f, 4 + side);
    return x;
}

static void complete(Voice *v, const Packet *pk, uint32_t status, uint32_t used)
{
    if (pk->completed_ptr) MEM32(pk->completed_ptr) = status == 0 ? pk->size : used;
    if (pk->status_ptr) MEM32(pk->status_ptr) = status;
    if (v->callback) {
        if (g_ndone < MAX_DONE) {
            Done d = {v->obj, v->callback, v->context, pk->context, status};
            g_done[g_ndone++] = d;
        } else {
            fprintf(stderr, "[DSOUND] completion queue full, packet callback dropped\n");
        }
    } else if (pk->context) {
        static int warned;
        if (!warned++) fprintf(stderr, "[DSOUND] packet with a completion event: not signalled (unimplemented)\n");
    }
}

static void pop_packet(Voice *v)
{
    memmove(&v->packets[0], &v->packets[1], (size_t)(v->npackets - 1) * sizeof(Packet));
    v->npackets--;
}

/* listener: distance and rolloff factors (IDirectSound_SetDistanceFactor / SetRolloffFactor) */
static float g_distance_factor = 1.0f, g_rolloff_factor = 1.0f;

/* A 3D voice's level on speaker bin b: inverse distance past the minimum distance (DS3D's model, held at the
 * maximum), panned on the x axis. The console's HRTF and doppler are not reproduced. */
static float atten3d(const Voice *v, uint32_t b)
{
    float d, g, pan, side;
    if (!v->is3d) return 1.0f;
    d = sqrtf(v->pos3d[0] * v->pos3d[0] + v->pos3d[1] * v->pos3d[1] + v->pos3d[2] * v->pos3d[2]) * g_distance_factor;
    if (d > v->max_dist) d = v->max_dist;
    g = d <= v->min_dist || v->min_dist <= 0 ? 1.0f : v->min_dist / (v->min_dist + g_rolloff_factor * (d - v->min_dist));
    pan = d > 0.001f ? v->pos3d[0] / (d / g_distance_factor) : 0.0f;  /* -1 left .. 1 right */
    if (b == 2 || b == 3) return g;
    side = b & 1 ? 1.0f + pan : 1.0f - pan;
    return g * (side > 1.0f ? 1.0f : side);
}

/* Gain of voice channel c on host left and right, from its mix bins. */
static void levels(Voice *v, float g[2][2])
{
    static const uint8_t front[2] = {0, 1};
    const uint8_t *bin = v->nbins ? v->bin : front;
    uint32_t n = v->nbins ? v->nbins : 2, voices = (v->channels - 1) / 2 + 1;
    uint32_t per = n / voices < 2 ? 2 : n / voices, step = v->channels == 1 ? 1 : 2;
    int speakers = 0;
    memset(g, 0, 4 * sizeof(float));
    /* muted speaker bins do not count: such a voice is heard through its effect send */
    for (uint32_t s = 0; s < n; s++) speakers |= bin[s] < 10 && bin[s] != 3 && v->bin_mb[bin[s]] > -10000;
    for (uint32_t c = 0; c < 2; c++)
        for (uint32_t s = c; c < v->channels && s < per && s < n; s += step) {
            uint32_t b = bin[s];
            float x;
            if (speakers && (b == 3 || b > 9)) continue;  /* sub and effect sends */
            x = db(v->bin_mb[b]) * v->volume * v->headroom * (b == 2 ? 0.707f : 1.0f) * atten3d(v, b);
            /* even bins are left, odd right, 2 is the centre */
            if (!speakers || b == 2 || !(b & 1)) g[c][0] = g[c][0] > x ? g[c][0] : x;
            if (!speakers || b == 2 || (b & 1)) g[c][1] = g[c][1] > x ? g[c][1] : x;
        }
}

/* xboxrecomp: 5.1 output (F10 Speakers): the bins are the console's speakers in the order a 5.1 channel mask has them
 * (front left, front right, centre, LFE, back left, back right), so each goes to its own speaker instead of the
 * stereo fold-down above; a buffer of more than two channels (a title's own 5.1 mix) goes channel by channel */
static int g_oc = 2;  /* output channels, set when the mixer starts */
int app_Speakers(void);  /* app.cpp: 2 or 6 */

static void levels6(Voice *v, float g[6][6])
{
    static const uint8_t front[2] = {0, 1};
    const uint8_t *bin = v->nbins ? v->bin : front;
    uint32_t n = v->nbins ? v->nbins : 2;
    int speakers = 0;
    memset(g, 0, 36 * sizeof(float));
    /* bins 6..9 are the crosstalk speakers (front L R, back L R): those positions */
    static const uint8_t spk[10] = {0, 1, 2, 3, 4, 5, 0, 1, 4, 5};
    for (uint32_t s = 0; s < n; s++) speakers |= bin[s] < 10 && v->bin_mb[bin[s]] > -10000;
    if (v->channels > 2) {
        for (uint32_t c = 0; c < v->channels && c < 6; c++) {
            uint32_t b = v->nbins > c ? bin[c] : c;
            if (b < 10) g[c][spk[b]] = db(v->bin_mb[b]) * v->volume * v->headroom * atten3d(v, (uint32_t)b);
        }
        return;
    }
    {
        uint32_t voices = (v->channels - 1) / 2 + 1, per = n / voices < 2 ? 2 : n / voices, step = v->channels == 1 ? 1 : 2;
        for (uint32_t c = 0; c < 2; c++)
            for (uint32_t s = c; c < v->channels && s < per && s < n; s += step) {
                uint32_t b = bin[s];
                float x = db(v->bin_mb[b]) * v->volume * v->headroom * atten3d(v, b);
                if (!speakers) {
                    /* heard only through its effect sends: on the front pair, as the stereo mix has it */
                    g[c][0] = g[c][0] > x ? g[c][0] : x;
                    g[c][1] = g[c][1] > x ? g[c][1] : x;
                } else if (b < 10) {
                    g[c][spk[b]] = g[c][spk[b]] > x ? g[c][spk[b]] : x;
                }
            }
    }
}

static void deactivate(Voice *v)
{
    if (v->active) v->active = v->paused = v->ending = 0;
}

/* xboxrecomp: 5.1 output (F10 Speakers = 5.1): render's mix with six outputs, apart from it so the stereo mix stays
 * the code it was (2026-10-24: folded into one, stereo crackled though no sum in it had changed) */
/* ── The speech follower: when a cutscene line's voice has gone quiet ──────────
 *
 * The title moves a speaker's lips with a generic talking face for a timer the speech channel keeps (hle_debug.c,
 * sub_0006FE40), longer than the words: Ron's "Are you all right up there?" (D3CARPE_03) talks on in silence. A line's
 * start arms this (hppoa_SpeechArm, on the game's thread); the first voice that turns loud within 0.8 s and was quiet
 * when it was armed is the line's voice (music and ambience are loud already, never picked); the game's thread then
 * reads how long that voice has been quiet (hppoa_SpeechSilentMs) and puts the face back to neutral. Levels are the
 * RMS of each voice's samples over a mix pass (256 frames), before its volume. */
#define SPEECH_LOUD 0.02f
static Voice *find(uint32_t obj);
static void init_lock(void);
#define SPEECH_SUSTAIN_MS 200  /* loud this long, unbroken, before a voice counts as the line's */
static volatile DWORD g_speech_arm_ms, g_speech_loud_ms;
static volatile uint32_t g_speech_obj;
static volatile int g_speech_heard;

static void speech_level(Voice *v)
{
    if (!v->lvl_n) return;
    v->level = (float)sqrt(v->lvl_acc / v->lvl_n);
    v->lvl_acc = 0, v->lvl_n = 0;
    DWORD now = GetTickCount();
    int loud = v->playing && v->level > SPEECH_LOUD;
    if (!loud) v->loud_since = 0;
    else if (!v->loud_since) v->loud_since = now;
    if (!g_speech_arm_ms) return;
    /* the line's voice: quiet when the line started, loud after it, and loud for SPEECH_SUSTAIN_MS on end (a click or a
     * short effect at the same moment is not it) */
    if (!g_speech_obj && loud && v->quiet_at_arm && v->loud_since >= g_speech_arm_ms &&
        now - v->loud_since >= SPEECH_SUSTAIN_MS && v->loud_since - g_speech_arm_ms < 1500) {
        g_speech_obj = v->obj;
        g_speech_loud_ms = now, g_speech_heard = 1;
        fprintf(stderr, "[LIPS] the line's voice: buffer %08X (%u Hz, %u ch), loud from %u ms after the line started\n",
                v->obj, v->rate, v->channels, (unsigned)(v->loud_since - g_speech_arm_ms));
    } else if (g_speech_obj == v->obj && loud) {
        g_speech_loud_ms = now;
    }
}

/* RECOMP_SPEECH_TRACE=1: [LIPSLVL] every 100 ms for 3 s after a line starts, each voice that is audible (level, rate,
 * channels, loud since, quiet when the line started): which voice is the line's and how long it really plays */
static void speech_levels_log(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_SPEECH_TRACE");
        on = e && *e && *e != '0';
    }
    static DWORD last;
    DWORD now = GetTickCount();
    if (!on || !g_speech_arm_ms || now - g_speech_arm_ms > 3000 || now - last < 100) return;
    last = now;
    char line[2048];
    int n = snprintf(line, sizeof(line), "[LIPSLVL] +%u ms:", (unsigned)(now - g_speech_arm_ms));
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < MAX_VOICES && n < (int)sizeof(line) - 96; i++) {
        Voice *v = g_voices[i];
        if (!v || !v->playing || v->level < 0.003f) continue;
        n += snprintf(line + n, sizeof(line) - (size_t)n, " %08X %uHz/%u lvl %.3f%s%s", v->obj, v->rate, v->channels,
                      v->level, v->loop_len || v->looping ? " loop" : "", v->quiet_at_arm ? " new" : " was-loud");
    }
    LeaveCriticalSection(&g_lock);
    fprintf(stderr, "%s\n", line);
}

void hppoa_SpeechArm(void)
{
    init_lock();
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < MAX_VOICES; i++)
        if (g_voices[i]) g_voices[i]->quiet_at_arm = !g_voices[i]->loud_since;
    g_speech_obj = 0, g_speech_heard = 0, g_speech_loud_ms = 0;
    g_speech_arm_ms = GetTickCount();
    LeaveCriticalSection(&g_lock);
}

/* ms the line's voice has been quiet (or stopped), -1 when no voice was found for it */
int hppoa_SpeechSilentMs(void)
{
    if (!g_speech_heard || !g_speech_obj) return -1;
    return (int)(GetTickCount() - g_speech_loud_ms);  /* a voice that stopped is quiet from its last loud pass */
}

static void render6(float *out, uint32_t frames)
{
    memset(out, 0, frames * 6 * sizeof(float));
    EnterCriticalSection(&g_lock);
    for (int vi = 0; vi < MAX_VOICES; vi++) {
        Voice *v = g_voices[vi];
        float g[6][6], dg[6][6];
        float e0[6] = {0}, e1[6] = {0};  /* 5.1: a multichannel buffer's channels past the first two */
        int ne = 0;
        double step;
        if (!v || v->paused || (!v->stream && !v->playing)) continue;
        memset(g, 0, sizeof(g));
        levels6(v, g);
        if (v->fading_out) memset(g, 0, sizeof(g));
        /* xboxrecomp: a voice that starts ramps up over this pass (~5 ms) instead of starting at full level: the
         * console's voice processor smooths level steps, and a sound starting or stopping mid waveform at once
         * was heard as a click every time */
        if (!v->has_gain) memset(v->gain6, 0, sizeof(v->gain6));
        v->has_gain = 1;
        for (int c = 0; c < 6; c++)
            for (int side = 0; side < 6; side++) dg[c][side] = (g[c][side] - v->gain6[c][side]) / (float)frames;
        step = (double)(v->freq ? v->freq : v->rate) / MIX_RATE;
        for (uint32_t i = 0; i < frames; i++) {
            uint32_t f = (uint32_t)v->pos;
            float a, b, a1, b1, t, x0, x1;
            if (v->stream) {
                while (v->npackets && f >= frames_of(v, v->packets[0].size)) {
                    Packet pk = v->packets[0];
                    pop_packet(v);
                    complete(v, &pk, 0, pk.size);
                    v->pos -= frames_of(v, pk.size);
                    f = (uint32_t)v->pos;
                }
                if (!v->npackets) {
                    if (v->active && !v->ending && g_trace) {
                        static int underruns;
                        if (underruns++ < 20) fprintf(stderr, "[DSOUND] stream %08X ran dry\n", v->obj);
                    }
                    v->pos = 0;
                    if (v->ending) deactivate(v);
                    break;
                }
                a = sample(v, v->packets[0].data, f, 0);
                b = sample(v, v->packets[0].data, f, 1);
                if (f + 1 < frames_of(v, v->packets[0].size)) {
                    a1 = sample(v, v->packets[0].data, f + 1, 0);
                    b1 = sample(v, v->packets[0].data, f + 1, 1);
                } else if (v->npackets > 1) {
                    a1 = sample(v, v->packets[1].data, 0, 0);
                    b1 = sample(v, v->packets[1].data, 0, 1);
                } else {
                    a1 = a;
                    b1 = b;
                }
            } else {
                uint32_t end = v->looping && v->loop_len ? frames_of(v, v->loop_start + v->loop_len) : frames_of(v, v->bytes);
                uint32_t nx;
                if (f >= end) {
                    uint32_t start;
                    if (!v->looping || !end) {
                        v->playing = 0;
                        v->pos = 0;
                        break;
                    }
                    start = v->loop_len ? frames_of(v, v->loop_start) : 0;
                    v->pos -= end - start;
                    f = (uint32_t)v->pos;
                }
                if (!v->data) break;
                nx = f + 1 < end ? f + 1 : v->looping && end ? (v->loop_len ? frames_of(v, v->loop_start) : 0) : f;
                if (v->channels > 2) {
                    /* 5.1: each channel to its own speaker */
                    ne = v->channels < 6 ? v->channels : 6;
                    a = sample(v, v->data, f, 0);
                    b = sample(v, v->data, f, 1);
                    a1 = sample(v, v->data, nx, 0);
                    b1 = sample(v, v->data, nx, 1);
                    for (int c = 2; c < ne; c++) e0[c] = sample(v, v->data, f, c), e1[c] = sample(v, v->data, nx, c);
                } else {
                    a = sample_lr(v, v->data, f, 0);
                    b = sample_lr(v, v->data, f, 1);
                    a1 = sample_lr(v, v->data, nx, 0);
                    b1 = sample_lr(v, v->data, nx, 1);
                }
            }
            t = (float)(v->pos - f);
            x0 = a + (a1 - a) * t;
            x1 = b + (b1 - b) * t;
            v->lvl_acc += (double)x0 * x0 + (double)x1 * x1, v->lvl_n += 2;
            {
                float *o = out + i * 6, xe[6] = {x0, x1, 0, 0, 0, 0};
                for (int c = 2; c < ne; c++) xe[c] = e0[c] + (e1[c] - e0[c]) * t;
                for (int c = 0; c < 6; c++)
                    for (int side = 0; side < 6; side++) v->gain6[c][side] += dg[c][side];
                for (int k = 0; k < 6; k++) {
                    float acc = 0;
                    for (int c = 0; c < (ne > 2 ? ne : 2); c++) acc += xe[c] * v->gain6[c][k];
                    o[k] += acc;
                }
            }
            v->pos += step;
        }
        memcpy(v->gain6, g, sizeof(g));
        speech_level(v);
        if (v->fading_out) {
            v->playing = 0;
            v->fading_out = 0;
            v->has_gain = 0;
        }
    }
    LeaveCriticalSection(&g_lock);
}

static void render(float *out, uint32_t frames)
{
    if (g_oc == 6) {
        render6(out, frames);
        return;
    }
    memset(out, 0, frames * 2 * sizeof(float));
    EnterCriticalSection(&g_lock);
    for (int vi = 0; vi < MAX_VOICES; vi++) {
        Voice *v = g_voices[vi];
        float g[2][2], dg[2][2];
        double step;
        if (!v || v->paused || (!v->stream && !v->playing)) continue;
        levels(v, g);
        if (v->fading_out) memset(g, 0, sizeof(g));
        /* xboxrecomp: a voice that starts ramps up over this pass (~5 ms) instead of starting at full level: the
         * console's voice processor smooths level steps, and a sound starting or stopping mid waveform at once
         * was heard as a click every time */
        if (!v->has_gain) memset(v->gain, 0, sizeof(v->gain));
        v->has_gain = 1;
        for (int c = 0; c < 2; c++)
            for (int side = 0; side < 2; side++) dg[c][side] = (g[c][side] - v->gain[c][side]) / (float)frames;
        step = (double)(v->freq ? v->freq : v->rate) / MIX_RATE;
        for (uint32_t i = 0; i < frames; i++) {
            uint32_t f = (uint32_t)v->pos;
            float a, b, a1, b1, t, x0, x1;
            if (v->stream) {
                while (v->npackets && f >= frames_of(v, v->packets[0].size)) {
                    Packet pk = v->packets[0];
                    pop_packet(v);
                    complete(v, &pk, 0, pk.size);
                    v->pos -= frames_of(v, pk.size);
                    f = (uint32_t)v->pos;
                }
                if (!v->npackets) {
                    if (v->active && !v->ending && g_trace) {
                        static int underruns;
                        if (underruns++ < 20) fprintf(stderr, "[DSOUND] stream %08X ran dry\n", v->obj);
                    }
                    v->pos = 0;
                    if (v->ending) deactivate(v);
                    break;
                }
                a = sample(v, v->packets[0].data, f, 0);
                b = sample(v, v->packets[0].data, f, 1);
                if (f + 1 < frames_of(v, v->packets[0].size)) {
                    a1 = sample(v, v->packets[0].data, f + 1, 0);
                    b1 = sample(v, v->packets[0].data, f + 1, 1);
                } else if (v->npackets > 1) {
                    a1 = sample(v, v->packets[1].data, 0, 0);
                    b1 = sample(v, v->packets[1].data, 0, 1);
                } else {
                    a1 = a;
                    b1 = b;
                }
            } else {
                uint32_t end = v->looping && v->loop_len ? frames_of(v, v->loop_start + v->loop_len) : frames_of(v, v->bytes);
                uint32_t nx;
                if (f >= end) {
                    uint32_t start;
                    if (!v->looping || !end) {
                        v->playing = 0;
                        v->pos = 0;
                        break;
                    }
                    start = v->loop_len ? frames_of(v, v->loop_start) : 0;
                    v->pos -= end - start;
                    f = (uint32_t)v->pos;
                }
                if (!v->data) break;
                a = sample_lr(v, v->data, f, 0);
                b = sample_lr(v, v->data, f, 1);
                nx = f + 1 < end ? f + 1 : v->looping && end ? (v->loop_len ? frames_of(v, v->loop_start) : 0) : f;
                a1 = sample_lr(v, v->data, nx, 0);
                b1 = sample_lr(v, v->data, nx, 1);
            }
            t = (float)(v->pos - f);
            x0 = a + (a1 - a) * t;
            x1 = b + (b1 - b) * t;
            v->lvl_acc += (double)x0 * x0 + (double)x1 * x1, v->lvl_n += 2;
            for (int c = 0; c < 2; c++)
                for (int side = 0; side < 2; side++) v->gain[c][side] += dg[c][side];
            out[i * 2] += x0 * v->gain[0][0] + x1 * v->gain[1][0];
            out[i * 2 + 1] += x0 * v->gain[0][1] + x1 * v->gain[1][1];
            v->pos += step;
        }
        memcpy(v->gain, g, sizeof(g));
        speech_level(v);
        if (v->fading_out) {
            v->playing = 0;
            v->fading_out = 0;
            v->has_gain = 0;
        }
    }
    LeaveCriticalSection(&g_lock);
}

/* ── Output: XAudio2, or a wall clock when there is no audio device ──────── */

/* Queued output: up to OUT_BUFS - 1 blocks of 256 frames in flight. ~80 ms (the Chamber of Secrets setting) put sound
 * noticeably behind the picture; RECOMP_AUDIO_LATENCY_MS sets it, default 32 ms (6 blocks). */
#define OUT_BUFS 16
static int g_out_bufs = OUT_BUFS;

static void init_latency(void)
{
    const char *e = getenv("RECOMP_AUDIO_LATENCY_MS");
    int ms = e && *e ? atoi(e) : 32;
    int n = ms * MIX_RATE / 1000 / MIX_FRAMES + 1;  /* blocks queued, plus the one being filled */
    g_out_bufs = n < 3 ? 3 : n > OUT_BUFS ? OUT_BUFS : n;
    fprintf(stderr, "[DSOUND] output queue %d x %d frames (~%d ms)\n", g_out_bufs - 1, MIX_FRAMES,
            (g_out_bufs - 1) * MIX_FRAMES * 1000 / MIX_RATE);
}

/* RECOMP_DSOUND_WAV=<file>: the mixed output as it goes to XAudio2, 16-bit stereo 48 kHz, for the first 3 minutes */
static FILE *g_wav;
static uint32_t g_wav_frames;

static void wav_write(const int16_t *pcm, uint32_t frames)
{
    static int opened;
    if (!opened) {
        const char *e = getenv("RECOMP_DSOUND_WAV");
        opened = 1;
        if (e && *e && (g_wav = fopen(e, "wb")) != NULL) {
            static const uint8_t hdr[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0,
                                            0, 1, 0, 2, 0, 0x80, 0xBB, 0, 0, 0, 0xEE, 2, 0, 4, 0, 16, 0, 'd', 'a', 't',
                                            'a', 0, 0, 0, 0};
            fwrite(hdr, 1, sizeof hdr, g_wav);
            fprintf(stderr, "[DSOUND] recording the mix to %s\n", e);
        }
    }
    if (!g_wav) return;
    fwrite(pcm, 4, frames, g_wav);
    g_wav_frames += frames;
    if (g_wav_frames % (MIX_RATE * 5) < frames || g_wav_frames >= MIX_RATE * 180) {  /* sizes kept current */
        uint32_t data = g_wav_frames * 4, riff = data + 36;
        long at = ftell(g_wav);
        fseek(g_wav, 4, SEEK_SET);
        fwrite(&riff, 4, 1, g_wav);
        fseek(g_wav, 40, SEEK_SET);
        fwrite(&data, 4, 1, g_wav);
        fseek(g_wav, at, SEEK_SET);
        fflush(g_wav);
        if (g_wav_frames >= MIX_RATE * 180) {
            fclose(g_wav);
            g_wav = NULL;
            fprintf(stderr, "[DSOUND] mix recording finished\n");
        }
    }
}

/* output health, reported every 10 s when anything went wrong */
static unsigned g_dry, g_clipped, g_mixed;
static DWORD g_report_ms;

static DWORD WINAPI mixer_thread(LPVOID unused)
{
    static float mix[MIX_FRAMES * 6];
    static int16_t pcm[OUT_BUFS][MIX_FRAMES * 6];
    IXAudio2 *xa2 = NULL;
    IXAudio2MasteringVoice *master = NULL;
    IXAudio2SourceVoice *src = NULL;
    WAVEFORMATEX wfx = {0}, *wfx_out = &wfx;
    LARGE_INTEGER freq, now, next;
    int nb = 0;
    (void)unused;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    init_latency();
    /* late wake ups of this thread are what empties the device queue; the CPU itself is mostly idle */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    g_report_ms = GetTickCount();
    g_oc = app_Speakers() == 6 ? 6 : 2;
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = (WORD)g_oc;
    wfx.nSamplesPerSec = MIX_RATE;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = (WORD)(2 * g_oc);
    wfx.nAvgBytesPerSec = MIX_RATE * 2 * (DWORD)g_oc;
    {
        /* 5.1: the speaker positions the bins stand for (front L R, centre, LFE, back L R) */
        static WAVEFORMATEXTENSIBLE wfe;
        static const GUID pcm_guid = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
        wfe.Format = wfx;
        wfe.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wfe.Format.cbSize = sizeof(wfe) - sizeof(WAVEFORMATEX);
        wfe.Samples.wValidBitsPerSample = 16;
        wfe.dwChannelMask = 0x3F;
        wfe.SubFormat = pcm_guid;
        if (g_oc == 6) wfx_out = &wfe.Format;
    }
    if (FAILED(XAudio2Create(&xa2, 0, XAUDIO2_DEFAULT_PROCESSOR)) ||
        FAILED(IXAudio2_CreateMasteringVoice(xa2, &master, (UINT32)g_oc, MIX_RATE, 0, NULL, NULL, 0)) ||
        FAILED(IXAudio2_CreateSourceVoice(xa2, &src, wfx_out, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL)) ||
        FAILED(IXAudio2SourceVoice_Start(src, 0, XAUDIO2_COMMIT_NOW))) {
        fprintf(stderr, "[DSOUND] XAudio2 unavailable: mixing on a wall clock, silently\n");
        src = NULL;
    } else {
        fprintf(stderr, "[DSOUND] XAudio2 output, %d Hz %s\n", MIX_RATE, g_oc == 6 ? "5.1" : "stereo");
    }

    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&next);
    for (;;) {
        g_trace_ui = app_AudioTrace();
        render(mix, MIX_FRAMES);
        speech_levels_log();
        for (int i = 0; i < MIX_FRAMES * g_oc; i++) {
            float s = mix[i] < -1.0f ? -1.0f : mix[i] > 1.0f ? 1.0f : mix[i];
            if (s != mix[i]) g_clipped++;
            pcm[nb][i] = (int16_t)(s * 32767.0f);
        }
        if (g_oc == 2) wav_write(pcm[nb], MIX_FRAMES);  /* RECOMP_DSOUND_WAV: stereo only */
        if (src) {
            XAUDIO2_BUFFER xb = {0};
            XAUDIO2_VOICE_STATE st;
            for (;;) {
                IXAudio2SourceVoice_GetState(src, &st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
                if (st.BuffersQueued < (UINT32)(g_out_bufs - 1)) break;
                Sleep(1);
            }
            if (st.BuffersQueued == 0 && g_mixed > (unsigned)g_out_bufs) g_dry++;  /* the device played everything: a gap */
            g_mixed++;
            {
                DWORD now_ms = GetTickCount();
                if (now_ms - g_report_ms >= 10000) {
                    if (g_dry || g_clipped)
                        fprintf(stderr, "[DSOUND] last %lus: %u output underruns, %u clipped samples\n",
                                (unsigned long)((now_ms - g_report_ms) / 1000), g_dry, g_clipped);
                    g_dry = g_clipped = 0;
                    g_report_ms = now_ms;
                }
            }
            xb.AudioBytes = MIX_FRAMES * 2 * (UINT32)g_oc;
            xb.pAudioData = (const BYTE *)pcm[nb];
            IXAudio2SourceVoice_SubmitSourceBuffer(src, &xb, NULL);
            nb = (nb + 1) % OUT_BUFS;
        } else {
            next.QuadPart += freq.QuadPart * MIX_FRAMES / MIX_RATE;
            for (;;) {
                QueryPerformanceCounter(&now);
                if (now.QuadPart >= next.QuadPart) break;
                Sleep(1);
            }
        }
    }
    return 0;
}

static void start(void)
{
    if (g_started) return;
    g_started = 1;
    CreateThread(NULL, 0, mixer_thread, NULL, 0, NULL);
}

/* ── Shared helpers ─────────────────────────────────────────────────────── */

static void read_format(Voice *v, uint32_t wfx)
{
    if (!wfx) return;
    v->tag = MEM16(wfx);
    v->channels = MEM16(wfx + 2) ? MEM16(wfx + 2) : 1;
    v->rate = MEM32(wfx + 4);
    v->align = MEM16(wfx + 12) ? MEM16(wfx + 12) : 1;
    v->bits = MEM16(wfx + 14);
    if (v->tag != 1 && v->tag != 0x69) fprintf(stderr, "[DSOUND] format tag 0x%X not supported\n", v->tag);
}

/* DSMIXBINS { count, pairs[] of { bin, volume } }: the voice's bins and volumes. */
static void set_bins(Voice *v, uint32_t mixbins)
{
    uint32_t n, pairs;
    v->nbins = 0;  /* none listed: front left and right */
    if (!mixbins) return;
    n = MEM32(mixbins);
    if (n > 8) n = 8;
    pairs = MEM32(mixbins + 4);
    for (uint32_t i = 0; i < n; i++) {
        v->bin[i] = MEM32(pairs + i * 8) & 31;
        v->bin_mb[v->bin[i]] = (int32_t)MEM32(pairs + i * 8 + 4);
    }
    v->nbins = (uint8_t)n;
}

/* 600 mB unless 3D, or a mix-in or effect-in buffer */
static float default_headroom(uint32_t flags) { return flags & 0x382010 ? 1.0f : db(-600); }

static Voice *find(uint32_t obj)
{
    for (int i = 0; i < MAX_VOICES; i++)
        if (g_voices[i] && g_voices[i]->obj == obj) return g_voices[i];
    return NULL;
}

static Voice *new_voice(uint32_t obj_size)
{
    for (int i = 0; i < MAX_VOICES; i++)
        if (!g_voices[i]) {
            Voice *v = calloc(1, sizeof(Voice));
            v->obj = xbox_HeapAlloc(obj_size, 16);
            memset((void *)XBOX_PTR(v->obj), 0, obj_size);
            v->refs = 1;
            v->tag = 1;
            v->channels = 1;
            v->bits = 16;
            v->align = 2;
            v->rate = MIX_RATE;
            v->max_packets = 1;
            v->volume = v->headroom = 1;
            v->block = -1;
            g_voices[i] = v;
            return v;
        }
    fprintf(stderr, "[DSOUND] out of voices (%d)\n", MAX_VOICES);
    return NULL;
}

static void free_voice(Voice *v)
{
    for (int i = 0; i < MAX_VOICES; i++)
        if (g_voices[i] == v) g_voices[i] = NULL;
    xbox_HeapFree(v->obj);
    free(v);
}

static void init_lock(void)
{
    static LONG once;
    if (!InterlockedExchange(&once, 1)) InitializeCriticalSection(&g_lock);
}

#define FALLBACK(addr) do { if (!hle_on()) { sub_##addr##_gen(); return; } init_lock(); } while (0)

/* ── Device ─────────────────────────────────────────────────────────────── */

/* DirectSoundCreate(pguid, ppDirectSound, pUnkOuter) */
extern void sub_002B874A_gen(void);
void sub_002B874A(void)
{
    FALLBACK(002B874A);
    EnterCriticalSection(&g_lock);
    start();
    if (!g_ds) {
        g_ds = xbox_HeapAlloc(0x40, 16);
        memset((void *)XBOX_PTR(g_ds), 0, 0x40);
    }
    MEM32(ARG(1)) = g_ds;
    LeaveCriticalSection(&g_lock);
    TRACE("DirectSoundCreate -> %08X", g_ds);
    ret_stdcall(S_OK_, 3);
}

/* IDirectSound_Release(this) */
extern void sub_002B6962_gen(void);
void sub_002B6962(void) { FALLBACK(002B6962); ret_stdcall(0, 1); }

/* IDirectSound_SetMixBinHeadroom(this, dwMixBinMask, dwHeadroom): the mixer applies no per-bin headroom */
extern void sub_002B752F_gen(void);
void sub_002B752F(void) { FALLBACK(002B752F); ret_stdcall(S_OK_, 3); }

/* IDirectSound_SetI3DL2Listener(this, pds3dl, dwApply): no room reverb here */
extern void sub_002B8584_gen(void);
void sub_002B8584(void) { FALLBACK(002B8584); ret_stdcall(S_OK_, 3); }

/* IDirectSound_DownloadEffectsImage(this, pvImageBuffer, dwImageSize,
 * pImageLoc, ppImageDesc). Effects run on the console's DSP and are not
 * reproduced; the title gets a zeroed descriptor with no effects. */
extern void sub_002B7508_gen(void);
void sub_002B7508(void)
{
    FALLBACK(002B7508);
    if (ARG(4)) {
        uint32_t desc = xbox_HeapAlloc(0x400, 16);
        memset((void *)XBOX_PTR(desc), 0, 0x400);
        MEM32(ARG(4)) = desc;
    }
    TRACE("DownloadEffectsImage(%u bytes): not reproduced", ARG(2));
    ret_stdcall(S_OK_, 5);
}

/* ── Buffers ────────────────────────────────────────────────────────────── */

/* DSBUFFERDESC { dwSize, dwFlags, dwBufferBytes, lpwfxFormat, lpMixBins, dwInputMixBin } */
static uint32_t new_buffer(uint32_t desc, uint32_t out)
{
    Voice *v;
    uint32_t flags = MEM32(desc + 4), bytes = MEM32(desc + 8);
    EnterCriticalSection(&g_lock);
    start();
    v = new_voice(0x40);
    if (!v) {
        LeaveCriticalSection(&g_lock);
        return E_FAIL_;
    }
    read_format(v, MEM32(desc + 12));
    set_bins(v, MEM32(desc + 16));
    v->headroom = default_headroom(flags);
    v->is3d = (flags & 0x10) != 0;  /* DSBCAPS_CTRL3D */
    v->min_dist = 1.0f;
    v->max_dist = 1e9f;
    if (bytes) {
        v->data = v->owned = xbox_HeapAlloc(bytes, 16);
        v->bytes = v->loop_len = bytes;
    }
    MEM32(out) = v->obj;
    LeaveCriticalSection(&g_lock);
    TRACE("CreateSoundBuffer flags=%X bytes=%u tag=%X ch=%u rate=%u -> %08X",
          flags, bytes, v->tag, v->channels, v->rate, v->obj);
    return S_OK_;
}

/* IDirectSound_CreateSoundBuffer(this, pdsbd, ppBuffer, pUnkOuter) */
extern void sub_002B8560_gen(void);
void sub_002B8560(void) { FALLBACK(002B8560); ret_stdcall(new_buffer(ARG(1), ARG(2)), 4); }

/* One buffer method: look the voice up under the lock, run the body. */
typedef uint32_t (*buf_fn)(Voice *v);

static void buffer_call(void (*gen)(void), buf_fn fn, const char *name, int nargs)
{
    Voice *v;
    uint32_t r;
    if (!hle_on()) { gen(); return; }
    init_lock();
    EnterCriticalSection(&g_lock);
    v = find(ARG(0));
    r = v ? fn(v) : E_FAIL_;
    LeaveCriticalSection(&g_lock);
    /* the calls a title makes every frame for every sound (status, cursor, volume, levels) are left out: logged, they were
     * ~100k lines a few minutes, and the log writes held up the title's music refills */
    if (strcmp(name, "GetStatus") && strcmp(name, "GetCurrentPosition") && strcmp(name, "SetVolume") &&
        strcmp(name, "SetMixBinVolumes"))
        TRACE("Buffer_%s(%08X, %08X, %08X) = %08X", name, ARG(0), nargs > 1 ? ARG(1) : 0, nargs > 2 ? ARG(2) : 0, r);
    ret_stdcall(r, nargs);
}

#define BUFFER(name) static uint32_t buf_##name(Voice *v)

BUFFER(Release)
{
    if (--v->refs > 0) return (uint32_t)v->refs;
    if (v->owned) xbox_HeapFree(v->owned);
    free_voice(v);
    return 0;
}

BUFFER(SetBufferData)
{
    if (ARG(1) == v->data && ARG(2) == v->bytes) return S_OK_;
    if (v->fading_out) v->playing = v->fading_out = 0;  /* new data: the old sound's fade ends here */
    if (v->owned && v->owned != ARG(1)) {
        xbox_HeapFree(v->owned);
        v->owned = 0;
    }
    v->data = ARG(1);
    if (!ARG(1) && ARG(2)) v->data = v->owned = xbox_HeapAlloc(ARG(2), 16);  /* a size with no data */
    v->bytes = v->loop_len = ARG(2);
    v->loop_start = 0;
    v->pos = 0;
    v->block = -1;
    return S_OK_;
}

/* Play(this, dwReserved1, dwReserved2, dwFlags): DSBPLAY_LOOPING = 1, DSBPLAY_FROMSTART = 2 */
/* Audio trace: what a sound's gains came to when it was stopped, as the per-frame volume calls are left out of the log.
 * A sound the title plays in full that nobody hears shows here as bins at -10000 mB, or a 3D attenuation near 0. */
static void trace_gains(Voice *v, const char *why)
{
    TRACE("%s gains: voice %08X bytes %08X vol %.3f headroom %.3f mB L %d R %d C %d LFE %d LS %d RS %d (nbins %u first %u) 3d %d pos %.1f %.1f %.1f dist %.1f..%.1f atten %.3f level %.4f",
          why, v->obj, v->bytes, v->volume, v->headroom, v->bin_mb[0], v->bin_mb[1], v->bin_mb[2], v->bin_mb[3], v->bin_mb[4], v->bin_mb[5],
          v->nbins, v->bin[0], v->is3d, v->pos3d[0], v->pos3d[1], v->pos3d[2], v->min_dist, v->max_dist, atten3d(v, 0), v->level);
}

BUFFER(Play)
{
    if (!v->playing || v->fading_out) v->has_gain = 0;  /* a start ramps up from silence */
    v->fading_out = 0;
    v->paused = 0;
    v->playing = 1;
    v->looping = ARG(3) & 1;
    if (ARG(3) & 2) v->pos = 0;
    return S_OK_;
}

BUFFER(Stop)
{
    trace_gains(v, "Stop");
    /* the console's voice processor ramps a voice down; cutting it mid waveform clicked */
    if (v->playing) v->fading_out = 1;
    return S_OK_;
}

BUFFER(SetLoopRegion)
{
    if (ARG(1) > v->bytes || ARG(2) > v->bytes - ARG(1)) return DSERR_INVALID;
    v->loop_start = ARG(1);
    v->loop_len = ARG(2) ? ARG(2) : v->bytes - ARG(1);
    return S_OK_;
}

/* DSBSTATUS_PLAYING = 1, DSBSTATUS_LOOPING = 4 */
BUFFER(GetStatus)
{
    int on = v->playing && !v->fading_out;
    MEM32(ARG(1)) = (on ? 1u : 0u) | (on && v->looping ? 4u : 0u);
    return S_OK_;
}

BUFFER(SetCurrentPosition)
{
    v->pos = frames_of(v, ARG(1));
    return S_OK_;
}

BUFFER(SetFrequency)
{
    v->freq = ARG(1);
    return S_OK_;
}

BUFFER(SetMixBins)
{
    set_bins(v, ARG(1));
    return S_OK_;
}


/* GetCurrentPosition(this, pdwPlayCursor, pdwWriteCursor): the write cursor a little ahead, as the APU reads ahead */
BUFFER(GetCurrentPosition)
{
    uint32_t p = bytes_of(v, (uint32_t)v->pos);
    if (ARG(1)) MEM32(ARG(1)) = p;
    if (ARG(2)) MEM32(ARG(2)) = v->bytes ? (p + bytes_of(v, ADPCM_FRAMES * 4)) % v->bytes : 0;
    return S_OK_;
}

/* Pause(this, dwPause): DSBPAUSE_RESUME 0, DSBPAUSE_PAUSE 1, DSBPAUSE_SYNCHPLAYBACK 2 (held until SynchPlayback,
 * which this title never calls: plays at once) */
BUFFER(Pause)
{
    v->paused = ARG(1) == 1;
    return S_OK_;
}

/* StopEx(this, rtTimeStamp (64-bit), dwFlags): DSBSTOPEX_IMMEDIATE 0, DSBSTOPEX_ENVELOPE 1 (the release ramp),
 * DSBSTOPEX_RELEASEWAVEFORM 2 (plays out past the loop end) */
BUFFER(StopEx)
{
    if (!v->playing) return S_OK_;
    trace_gains(v, "StopEx");
    if (ARG(3) & 2) v->looping = 0;
    else v->fading_out = 1;
    return S_OK_;
}

/* SetEG(this, pEnvelopeDesc), SetFilter(this, pFilterDesc): envelopes and filters are not reproduced */
BUFFER(Ignore) { (void)v; return S_OK_; }

/* SetMixBinVolumes(this, pMixBins): DSMIXBINS of { bin, volume } */
BUFFER(SetMixBinVolumes)
{
    uint32_t n, pairs, mixbins = ARG(1);
    if (!mixbins) return S_OK_;
    n = MEM32(mixbins);
    if (n > 32) n = 32;
    pairs = MEM32(mixbins + 4);
    for (uint32_t i = 0; i < n; i++) v->bin_mb[MEM32(pairs + i * 8) & 31] = (int32_t)MEM32(pairs + i * 8 + 4);
    return S_OK_;
}

/* ── Entry points: literal definitions, so --exclude-manual finds them ──── */

/* DirectSoundCreateBuffer(pdsbd, ppBuffer) */
extern void sub_002B8791_gen(void);
void sub_002B8791(void) { FALLBACK(002B8791); ret_stdcall(new_buffer(ARG(0), ARG(1)), 2); }

/* IDirectSoundBuffer_* */
extern void sub_002B6978_gen(void);
void sub_002B6978(void) { buffer_call(sub_002B6978_gen, buf_Release, "Release", 1); }
extern void sub_002B7FAE_gen(void);
void sub_002B7FAE(void) { buffer_call(sub_002B7FAE_gen, buf_SetBufferData, "SetBufferData", 3); }
extern void sub_002B75A3_gen(void);
void sub_002B75A3(void) { buffer_call(sub_002B75A3_gen, buf_Play, "Play", 4); }
extern void sub_002B75DF_gen(void);
void sub_002B75DF(void) { buffer_call(sub_002B75DF_gen, buf_StopEx, "StopEx", 4); }
extern void sub_002B7603_gen(void);
void sub_002B7603(void) { buffer_call(sub_002B7603_gen, buf_Pause, "Pause", 2); }
extern void sub_002B761F_gen(void);
void sub_002B761F(void) { buffer_call(sub_002B761F_gen, buf_SetLoopRegion, "SetLoopRegion", 3); }
extern void sub_002B763F_gen(void);
void sub_002B763F(void) { buffer_call(sub_002B763F_gen, buf_GetStatus, "GetStatus", 2); }
extern void sub_002B765B_gen(void);
void sub_002B765B(void) { buffer_call(sub_002B765B_gen, buf_GetCurrentPosition, "GetCurrentPosition", 3); }
extern void sub_002B767B_gen(void);
void sub_002B767B(void) { buffer_call(sub_002B767B_gen, buf_SetCurrentPosition, "SetCurrentPosition", 2); }
extern void sub_002B7A60_gen(void);
void sub_002B7A60(void) { buffer_call(sub_002B7A60_gen, buf_SetFrequency, "SetFrequency", 2); }
extern void sub_002B7A7C_gen(void);
void sub_002B7A7C(void) { buffer_call(sub_002B7A7C_gen, buf_SetMixBins, "SetMixBins", 2); }
extern void sub_002B7587_gen(void);
void sub_002B7587(void) { buffer_call(sub_002B7587_gen, buf_SetMixBinVolumes, "SetMixBinVolumes", 2); }
extern void sub_002B754F_gen(void);
void sub_002B754F(void) { buffer_call(sub_002B754F_gen, buf_Ignore, "SetEG", 2); }
extern void sub_002B756B_gen(void);
void sub_002B756B(void) { buffer_call(sub_002B756B_gen, buf_Ignore, "SetFilter", 2); }

/* F10 Debug > Audio trace also logs [SFX]: the sound engine's event objects (0x162670 prepare, 0x1629B0 and 0x162C10
 * play; game_vtable, thiscall), with what they answered and the title's return addresses on the guest stack. Sounds
 * that the DirectSound trace above never sees (a door, a vial: their samples are in memory and no Buffer_Play comes)
 * either show here as a refusal, or do not show, so the request never reached the engine. */
static void sfx_trace(const char *what, uint32_t self, uint32_t sp, int nargs)
{
    fprintf(stderr, "[SFX] %u ms %s obj %08X vt %08X flags %08X +8 %08X -> AL %u args", (unsigned)GetTickCount(), what,
            self, MEM32(self), MEM32(self + 0x40), MEM32(self + 8), g_eax & 0xFF);
    for (int i = 0; i < nargs; i++) fprintf(stderr, " %08X", MEM32(sp + 4 + 4 * (uint32_t)i));
    fprintf(stderr, " from");
    int n = 0;
    for (uint32_t i = 0; i < 256 && n < 8; i++) {
        uint32_t v = MEM32(sp + 4 * i);
        if (v < 0x11005 || v >= 0x264C64) continue;
        if (MEM8(v - 5) == 0xE8 || MEM8(v - 2) == 0xFF || MEM8(v - 3) == 0xFF || MEM8(v - 6) == 0xFF)
            fprintf(stderr, " %08X", v), n++;
    }
    fputc('\n', stderr);
}

#define SFX_HOOK(addr, what, nargs)                                     \
    extern void sub_##addr##_gen(void);                                 \
    void sub_##addr(void)                                               \
    {                                                                   \
        uint32_t self = g_ecx, sp = g_esp;                              \
        sub_##addr##_gen();                                             \
        if (g_trace || g_trace_ui) {                                    \
            sfx_trace(what, self, sp, nargs);                           \
        }                                                               \
    }
SFX_HOOK(00162670, "prepare", 3)
SFX_HOOK(001629B0, "play A", 1)
SFX_HOOK(00162C10, "play B", 1)

/* The title's "SOUND - Trying to stream or sentence an incompatible format" repeats every frame in some levels
 * (dementorsbattle, patronuschallenge, hex_spring_d_ctc_hd, shriekingshacktunnel). sub_001624A0 (thiscall: source,
 * stream index, out) reads a stream's format bits from its source through vfunc +0xC: rate, channels and sample type
 * go to out; a sample type of 0 is that message. [SNDFMT], always on: each source and index once when it succeeds,
 * and the first 3 failures with a count, so a failing stream can be told from the ones that work. */
uint32_t g_sfx_fmt;
extern void sub_001624A0_gen(void);
void sub_001624A0(void)
{
    enum { kSeen = 64 };
    static struct { uint32_t self, index, fails; int ok; } seen[kSeen];
    uint32_t self = g_ecx, sp = g_esp, stream = g_esi, index = MEM32(sp + 4), out = MEM32(sp + 8);
    g_sfx_fmt = 0;
    sub_001624A0_gen();
    int ok = g_eax != 0, i;
    for (i = 0; i < kSeen; i++)
        if (!seen[i].self || (seen[i].self == self && seen[i].index == index)) break;
    if (i == kSeen) return;
    seen[i].self = self, seen[i].index = index;
    if (ok ? seen[i].ok++ : seen[i].fails++ >= 3 && seen[i].fails % 600) return;
    fprintf(stderr, "[SNDFMT] %u ms %s source %08X vt %08X index %08X stream %08X vt %08X bits %08X rate %u ch %u type %u fails %u from",
            (unsigned)GetTickCount(), ok ? "ok" : "BAD", self, MEM32(self), index, stream, MEM32(stream), g_sfx_fmt,
            MEM16(out), MEM8(out + 2), MEM8(out + 3), seen[i].fails);
    int n = 0;
    for (uint32_t k = 0; k < 256 && n < 6; k++) {
        uint32_t v = MEM32(sp + 4 * k);
        if (v < 0x11005 || v >= 0x264C64) continue;
        if (MEM8(v - 5) == 0xE8 || MEM8(v - 2) == 0xFF || MEM8(v - 3) == 0xFF || MEM8(v - 6) == 0xFF)
            fprintf(stderr, " %08X", v), n++;
    }
    fputc('\n', stderr);
}
