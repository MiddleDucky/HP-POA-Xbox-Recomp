/**
 * NV2A on the host GPU (Direct3D 11).
 *
 * The pushbuffer executor (nv2a_pb_exec.c) decodes the title's commands and
 * runs its vertex programs on the CPU; with RECOMP_GPU=1 the result goes here
 * instead of to the software rasteriser. This module draws it with D3D11:
 * render targets and depth buffers live on the GPU, the register combiners
 * are one shader driven by constants (no per-state shader compiles), and the
 * finished frame goes to the window as a texture rather than through guest
 * memory.
 *
 * Under Wine, D3D11 is fastest through DXVK (D3D11 on Vulkan); Wine's own
 * d3d11 works too.
 */
#ifndef XBOXRECOMP_NV2A_GPU_H
#define XBOXRECOMP_NV2A_GPU_H

#include <stdint.h>
#include "nv2a_combiner.h"
#include "nv2a_vsh_interp.h"

/* One texture stage, as the title programmed it. */
typedef struct {
    uint32_t offset;                    /* guest address of texel (0,0)  */
    uint32_t width, height;             /* from FORMAT or IMAGE_RECT     */
    uint32_t pitch;                     /* bytes per row, from CONTROL1  */
    uint32_t color;                     /* NV097 colour-format code      */
    uint32_t addr_u, addr_v;            /* wrap mode per axis            */
    uint32_t palette;                   /* guest address of the CLUT, P8 */
    uint32_t levels;                    /* mip levels, from FORMAT       */
    uint32_t filter;                    /* SET_TEXTURE_FILTER            */
    int      cube;                      /* FORMAT: six faces, not one    */
    int      valid;
} Texture;

/* Everything one draw depends on, snapshotted from the executor's state. */
typedef struct {
    /* Where it draws. Offsets are resolved guest addresses. */
    uint32_t color_base, pitch, bpp;
    uint32_t clip_x, clip_y, clip_w, clip_h;
    uint32_t zeta_base;                 /* 0: no depth buffer             */
    float    zmax;                      /* depth range: 0xFFFF or 0xFFFFFF */

    uint32_t color_mask;                /* A<<24 R<<16 G<<8 B, bit 0 each  */
    uint32_t blend_enable, blend_sfactor, blend_dfactor, blend_equation;
    uint32_t blend_color;
    uint32_t depth_test, depth_func, depth_mask;
    uint32_t stencil_test, stencil_write_mask, stencil_func, stencil_ref;
    uint32_t stencil_func_mask, stencil_fail, stencil_zfail, stencil_zpass;
    uint32_t cull_enable, cull_face, front_face;
    uint32_t alpha_test, alpha_func, alpha_ref;
    uint32_t fog_color;                 /* R in bits 0-7                  */

    Nv2aCombiner rc;
    int      rc_seen;
    uint32_t clip_plane_mode;
    Texture  tex[4];
} Nv2aGpuState;

/* One vertex as the GPU takes it: the program's outputs (screen-space
 * position with the clip-space w, colours, texture coordinates) and the fog
 * factor the fixed-function fog unit computed from oFog. */
typedef struct {
    float pos[4];
    float d0[4], d1[4];
    float tex[4][4];
    float fog;
} Nv2aGpuVertex;

int  nv2a_gpu_enabled(void);

/* A triangle list: `idx` indexes `v`. */
void nv2a_gpu_draw(const Nv2aGpuState *st, const Nv2aGpuVertex *v,
                   uint32_t nv, const uint32_t *idx, uint32_t nidx);

/* CLEAR_SURFACE: param bits 0 Z, 1 stencil, 4-7 colour channels. */
void nv2a_gpu_clear(const Nv2aGpuState *st, uint32_t param,
                    uint32_t color, uint32_t zstencil);

/* The title finished the frame in the surface at `color_base`. */
void nv2a_gpu_flip(uint32_t color_base);

/* Texel decoder the upload path uses, implemented by the executor (the same
 * one the software rasteriser samples through). */
int nv2a_tex_texel(const Texture *t, uint32_t face_offset, uint32_t u,
                   uint32_t v, uint32_t *argb);

#endif
