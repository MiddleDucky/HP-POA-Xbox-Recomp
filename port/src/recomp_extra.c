/**
 * Functions the recompile missed, translated one at a time with tools.recomp -f from a disassembly seeded with
 * port/icall_seeds.json, until the next full regen makes them in src/recomp/gen (then delete them here and their
 * lines in recomp_manual.c's recomp_lookup_manual).
 *
 * sub_000777F0 (0x777F0 - 0x77A67, thiscall, 3 arguments, ret 12): reached only through a function pointer and right
 * after the previous function's switch table, so the disassembler read it as table bytes and the recompile had no
 * code for it; each call was dropped ([ICALL] Failed to resolve VA 0x000777F0, 405 times in charmsclassroom). A
 * character's interaction handler (+0xE38, +0x670, +0x900, actions 0x3B..0x40 through +0x120; sub_000805F0,
 * sub_00082D10): without it the Charms classroom's chests gave no Open option after the challenge (2026-11-03).
 */
#define RECOMP_GENERATED_CODE
#include "recomp/gen/recomp_funcs.h"
#include <math.h>

/**
 * sub_000777F0
 * Original: 0x000777F0 - 0x00077A67 (631 bytes, 206 insns)
 * Category: game_vtable
 * CC: cdecl, 0 params, returns int_or_void
 * Frame: fpo_leaf
 */
void sub_000777F0(void)
{
    uint32_t ebp = 0;
    ebp = g_ebp;  /* prologue saves caller's frame */
    int _flags = 0; /* fallback flag var */
    uint32_t _fa = 0, _fb = 0;
    int32_t _fas = 0, _fbs = 0;
    (void)_fa; (void)_fb; (void)_fas; (void)_fbs;
    #define fp_push(v) do { double _fp_value = (v); \
        g_fp_top = (g_fp_top + 7u) & 7u; \
        g_fp_stack[g_fp_top] = _fp_value; } while (0)
    #define fp_pop() (g_fp_top = (g_fp_top + 1u) & 7u)
    #define fp_top() g_fp_stack[g_fp_top]
    #define fp_st(i) g_fp_stack[(g_fp_top + (i)) & 7u]
    #define fp_st1() fp_st(1)

loc_000777F0: ;
    PUSH32(esp, ebp);
    ebp = esp;
    g_ebp = ebp; /* publish frame for frameless callees */
    g_seh_ebp = ebp;
    _fb = (uint32_t)(0x14) & 0xFFFFFFFFu; _fbs = (int32_t)(int32_t)(_fb); /* sub source, before the write */
    esp = esp - 0x14;
    _fa = (uint32_t)(esp) & 0xFFFFFFFFu; _fas = (int32_t)(int32_t)(_fa); /* sub result */
    PUSH32(esp, esi);
    esi = ecx;
    eax = MEM32(esi + 0x28);
    PUSH32(esp, 0);
    PUSH32(esp, eax);
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x00077804u); RECOMP_ABI_CALL(0x001BC360u, sub_001BC360); /* call 0x001BC360 */

loc_00077804: ;
    MEM32(ebp + -8) = eax;
    SET_LO8(eax, MEM8(esi + 0xE38));
    _fb = (uint32_t)(8) & 0xFFFFFFFFu; _fbs = (int32_t)(int32_t)(_fb); /* add source, before the write */
    esp = esp + 8;
    _fa = (uint32_t)(esp) & 0xFFFFFFFFu; _fas = (int32_t)(int32_t)(_fa); /* add result */
    _fa = (uint32_t)(LO8(eax)) & 0xFFu; _fb = (uint32_t)(0) & 0xFFu;
    _fas = (int32_t)(int8_t)(_fa); _fbs = (int32_t)(int8_t)(_fb); /* cmp LO8(eax), 0 (8-bit) */
    if (CMP_NE(_fa, _fb)) goto loc_00077A60; /* jne: not equal / not zero */

loc_00077818: ;
    eax = MEM32(esi + 0x670);
    _fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fb = (uint32_t)(0) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp eax, 0 (32-bit) */
    if (CMP_A(_fa, _fb)) goto loc_00077A60; /* ja: above (unsigned >) */

loc_00077826: ;
    _fa = (uint32_t)(MEM32(esi + 0x900)) & 0xFFFFFFFFu; _fb = (uint32_t)(1) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp MEM32(esi + 0x900), 1 (32-bit) */
    if (CMP_NE(_fa, _fb)) goto loc_0007787B; /* jne: not equal / not zero */

loc_0007782F: ;
    ecx = MEM32(esi + 0x120);
    SET_LO16(eax, MEM16(ecx + 0x10));
    _fa = (uint32_t)(LO16(eax)) & 0xFFFFu; _fb = (uint32_t)(0x3B) & 0xFFFFu;
    _fas = (int32_t)(int16_t)(_fa); _fbs = (int32_t)(int16_t)(_fb); /* cmp LO16(eax), 0x3B (16-bit) */
    if (CMP_EQ(_fa, _fb)) goto loc_00077A60; /* je: equal / zero */

loc_00077843: ;
    _fa = (uint32_t)(LO16(eax)) & 0xFFFFu; _fb = (uint32_t)(0x3C) & 0xFFFFu;
    _fas = (int32_t)(int16_t)(_fa); _fbs = (int32_t)(int16_t)(_fb); /* cmp LO16(eax), 0x3C (16-bit) */
    if (CMP_EQ(_fa, _fb)) goto loc_00077A60; /* je: equal / zero */

loc_0007784D: ;
    _fa = (uint32_t)(LO16(eax)) & 0xFFFFu; _fb = (uint32_t)(0x3D) & 0xFFFFu;
    _fas = (int32_t)(int16_t)(_fa); _fbs = (int32_t)(int16_t)(_fb); /* cmp LO16(eax), 0x3D (16-bit) */
    if (CMP_EQ(_fa, _fb)) goto loc_00077A60; /* je: equal / zero */

loc_00077857: ;
    _fa = (uint32_t)(LO16(eax)) & 0xFFFFu; _fb = (uint32_t)(0x40) & 0xFFFFu;
    _fas = (int32_t)(int16_t)(_fa); _fbs = (int32_t)(int16_t)(_fb); /* cmp LO16(eax), 0x40 (16-bit) */
    if (CMP_EQ(_fa, _fb)) goto loc_00077A60; /* je: equal / zero */

loc_00077861: ;
    edx = MEM32(ebp + 0x10);
    eax = MEM32(ebp + 0xC);
    ecx = MEM32(ebp + 8);
    PUSH32(esp, edx);
    PUSH32(esp, eax);
    PUSH32(esp, ecx);
    ecx = esi;
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x00077874u); RECOMP_ABI_CALL(0x000805F0u, sub_000805F0); /* call 0x000805F0 */

loc_00077874: ;
    POP32(esp, esi);
    esp = ebp;
    POP32(esp, ebp);
    esp += 16; return; /* ret 12 */

loc_0007787B: ;
    eax = MEM32(esi + 0xFC0);
    PUSH32(esp, eax);
    ecx = esi;
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x00077889u); RECOMP_ABI_CALL(0x000BA740u, sub_000BA740); /* call 0x000BA740 */

loc_00077889: ;
    _fa = (uint32_t)(LO8(eax)) & 0xFFu; _fb = (uint32_t)(0) & 0xFFu;
    _fas = (int32_t)(int8_t)(_fa); _fbs = (int32_t)(int8_t)(_fb); /* cmp LO8(eax), 0 (8-bit) */
    if (CMP_NE(_fa, _fb)) goto loc_00077A60; /* jne: not equal / not zero */

loc_00077891: ;
    edx = MEM32(ebp + 0x10);
    eax = MEM32(ebp + 8);
    PUSH32(esp, ebx);
    PUSH32(esp, edi);
    edi = MEM32(ebp + 0xC);
    PUSH32(esp, edx);
    PUSH32(esp, edi);
    PUSH32(esp, eax);
    ecx = esi;
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x000778A6u); RECOMP_ABI_CALL(0x00082D10u, sub_00082D10); /* call 0x00082D10 */

loc_000778A6: ;
    eax = edi + -1;
    _fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fb = (uint32_t)(5) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp eax, 5 (32-bit) */
    ebx = 2;
    MEM8(ebp + 0xB) = 1;
    MEM8(ebp + 0xF) = 0;
    if (CMP_A(_fa, _fb)) goto loc_000778E7; /* ja: above (unsigned >) */

loc_000778BB: ;
    { uint32_t _jt = MEM32(eax * 4 + 0x77A68); /* switch: 6 entries, 4 targets */
    if (_jt == 0x000778C2u) goto loc_000778C2;
    if (_jt == 0x000778CDu) goto loc_000778CD;
    if (_jt == 0x000778D4u) goto loc_000778D4;
    if (_jt == 0x000778E7u) goto loc_000778E7;
    g_seh_ebp = ebp; RECOMP_ITAIL(_jt); return; }

loc_000778C2: ;
    MEM8(ebp + 0xF) = 1;
    ebx = 2;
    goto loc_000778E7;

loc_000778CD: ;
    ebx = 1;
    goto loc_000778E7;

loc_000778D4: ;
    PUSH32(esp, 0);
    PUSH32(esp, 7);
    ecx = esi + 0xE78;
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x000778E3u); RECOMP_ABI_CALL(0x0006D580u, sub_0006D580); /* call 0x0006D580 */

loc_000778E3: ;
    MEM8(ebp + 0xB) = 0;

loc_000778E7: ;
    edi = MEM32(ebp + 0x10);
    ecx = MEM32(edi + 0x28);
    PUSH32(esp, 0);
    PUSH32(esp, ecx);
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x000778F5u); RECOMP_ABI_CALL(0x000CED60u, sub_000CED60); /* call 0x000CED60 */

loc_000778F5: ;
    _fb = (uint32_t)(8) & 0xFFFFFFFFu; _fbs = (int32_t)(int32_t)(_fb); /* add source, before the write */
    esp = esp + 8;
    _fa = (uint32_t)(esp) & 0xFFFFFFFFu; _fas = (int32_t)(int32_t)(_fa); /* add result */
    _fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fb = (uint32_t)(0) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp eax, 0 (32-bit) */
    if (CMP_EQ(_fa, _fb)) goto loc_00077930; /* je: equal / zero */

loc_000778FC: ;
    edx = MEM32(eax + 0x11C);
    ecx = MEM32(eax + 0x120);
    MEM32(ebp + -20) = edx;
    edx = MEM32(eax + 0x124);
    MEM32(ebp + -16) = ecx;
    ecx = MEM32(ebp + -20);
    eax = esi + 0x15C4;
    MEM32(ebp + -12) = edx;
    edx = MEM32(ebp + -16);
    MEM32(eax) = ecx;
    ecx = MEM32(ebp + -12);
    MEM32(eax + 4) = edx;
    MEM32(eax + 8) = ecx;
    goto loc_00077950;

loc_00077930: ;
    ecx = edi;
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x00077937u); RECOMP_ABI_CALL(0x0006AD20u, sub_0006AD20); /* call 0x0006AD20 */

loc_00077937: ;
    _fb = (uint32_t)(0x20) & 0xFFFFFFFFu; _fbs = (int32_t)(int32_t)(_fb); /* add source, before the write */
    eax = eax + 0x20;
    _fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fas = (int32_t)(int32_t)(_fa); /* add result */
    ecx = MEM32(eax);
    edx = esi + 0x15C4;
    MEM32(edx) = ecx;
    ecx = MEM32(eax + 4);
    MEM32(edx + 4) = ecx;
    eax = MEM32(eax + 8);
    MEM32(edx + 8) = eax;

loc_00077950: ;
    edx = MEM32(ebp + -8);
    ecx = ebp + -4;
    PUSH32(esp, ecx);
    PUSH32(esp, edx);
    PUSH32(esp, ebx);
    ecx = esi;
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x00077960u); RECOMP_ABI_CALL(0x0007C960u, sub_0007C960); /* call 0x0007C960 */

loc_00077960: ;
    eax = MEM32(esi + 0x120);
    _fa = (uint32_t)(MEM16(eax + 0x10)) & 0xFFFFu; _fb = (uint32_t)(0x3F) & 0xFFFFu;
    _fas = (int32_t)(int16_t)(_fa); _fbs = (int32_t)(int16_t)(_fb); /* cmp MEM16(eax + 0x10), 0x3F (16-bit) */
    if (CMP_EQ(_fa, _fb)) goto loc_00077A5E; /* je: equal / zero */

loc_00077971: ;
    fp_push(MEMF(0x2C8738)); /* fld float */
    ecx = MEM32(ebp + -8);
    fp_push(MEMF(ecx + 0x34)); /* fld float */
    g_fp_cmp = RECOMP_FCMP(fp_top(), fp_st1()); g_fp_cc = RECOMP_FCMP_CC(g_fp_cmp); fp_pop(); fp_pop(); /* fucompp  */
    eax = (eax & 0xFFFF0000u) | (uint32_t)(uint16_t)(((g_fp_top & 7u) << 11) | g_fp_cc); /* fnstsw ax <- fpu status */
    _fa = (uint32_t)(HI8(eax)) & 0xFFu; _fb = (uint32_t)(0x44) & 0xFFu;
    _fas = (int32_t)(int8_t)(_fa); _fbs = (int32_t)(int8_t)(_fb); /* test HI8(eax), 0x44 (8-bit) */
    if (RECOMP_PARITY8((_fa) & (_fb))) goto loc_0007799D; /* jp: parity */

loc_00077986: ;
    PUSH32(esp, 0x62);
    PUSH32(esp, 0x57E40);
    ecx = esi;
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x00077994u); RECOMP_ABI_CALL(0x0007FD70u, sub_0007FD70); /* call 0x0007FD70 */

loc_00077994: ;
    POP32(esp, edi);
    POP32(esp, ebx);
    POP32(esp, esi);
    esp = ebp;
    POP32(esp, ebp);
    esp += 16; return; /* ret 12 */

loc_0007799D: ;
    SET_LO16(eax, MEM16(ebp + -4));
    _fa = (uint32_t)(LO8(eax)) & 0xFFu; _fb = (uint32_t)(0) & 0xFFu;
    _fas = (int32_t)(int8_t)(_fa); _fbs = (int32_t)(int8_t)(_fb); /* cmp LO8(eax), 0 (8-bit) */
    if (CMP_EQ(_fa, _fb)) goto loc_00077A1D; /* je: equal / zero */

loc_000779A5: ;
    PUSH32(esp, 1);
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x000779ACu); RECOMP_ABI_CALL(0x001BC6B0u, sub_001BC6B0); /* call 0x001BC6B0 */

loc_000779AC: ;
    SET_LO8(eax, MEM8(ebp + 0xF));
    _fa = (uint32_t)(LO8(eax)) & 0xFFu; _fb = (uint32_t)(0) & 0xFFu;
    _fas = (int32_t)(int8_t)(_fa); _fbs = (int32_t)(int8_t)(_fb); /* cmp LO8(eax), 0 (8-bit) */
    if (CMP_EQ(_fa, _fb)) goto loc_00077A05; /* je: equal / zero */

loc_000779B3: ;
    ecx = esi;
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x000779BAu); RECOMP_ABI_CALL(0x0006AC80u, sub_0006AC80); /* call 0x0006AC80 */

loc_000779BA: ;
    ecx = MEM32(eax);
    edx = MEM32(eax + 4);
    eax = MEM32(eax + 8);
    MEM32(ebp + -20) = ecx;
    ecx = edi;
    MEM32(ebp + -16) = edx;
    MEM32(ebp + -12) = eax;
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x000779D2u); RECOMP_ABI_CALL(0x0006AC80u, sub_0006AC80); /* call 0x0006AC80 */

loc_000779D2: ;
    PUSH32(esp, eax);
    ecx = ebp + -20;
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x000779DBu); RECOMP_ABI_CALL(0x00070AC0u, sub_00070AC0); /* call 0x00070AC0 */

loc_000779DB: ;
    ecx = ebp + -20;
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x000779E3u); RECOMP_ABI_CALL(0x00026F70u, sub_00026F70); /* call 0x00026F70 */

loc_000779E3: ;
    ecx = MEM32(0x36AD60);
    PUSH32(esp, 0);
    PUSH32(esp, ecx);
    edx = ebp + -20;
    PUSH32(esp, edx);
    PUSH32(esp, 0x3F800000);
    ecx = esi;
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x000779FCu); RECOMP_ABI_CALL(0x000767B0u, sub_000767B0); /* call 0x000767B0 */

loc_000779FC: ;
    POP32(esp, edi);
    POP32(esp, ebx);
    POP32(esp, esi);
    esp = ebp;
    POP32(esp, ebp);
    esp += 16; return; /* ret 12 */

loc_00077A05: ;
    PUSH32(esp, 0);
    PUSH32(esp, 7);
    ecx = esi + 0xE78;
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x00077A14u); RECOMP_ABI_CALL(0x0006D580u, sub_0006D580); /* call 0x0006D580 */

loc_00077A14: ;
    POP32(esp, edi);
    POP32(esp, ebx);
    POP32(esp, esi);
    esp = ebp;
    POP32(esp, ebp);
    esp += 16; return; /* ret 12 */

loc_00077A1D: ;
    _fa = (uint32_t)(HI8(eax)) & 0xFFu; _fb = (uint32_t)(0) & 0xFFu;
    _fas = (int32_t)(int8_t)(_fa); _fbs = (int32_t)(int8_t)(_fb); /* cmp HI8(eax), 0 (8-bit) */
    if (CMP_EQ(_fa, _fb)) goto loc_00077A3F; /* je: equal / zero */

loc_00077A21: ;
    SET_LO8(edx, MEM8(esi + 0x904));
    PUSH32(esp, 0);
    SET_LO8(edx, LO8(edx) & 0xEF);
    _fa = (uint32_t)(LO8(edx)) & 0xFFu; _fas = (int32_t)(int8_t)(_fa); /* and result */
    PUSH32(esp, 8);
    ecx = esi + 0xE78;
    MEM8(esi + 0x904) = LO8(edx);
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x00077A3Fu); RECOMP_ABI_CALL(0x0006D580u, sub_0006D580); /* call 0x0006D580 */

loc_00077A3F: ;
    SET_LO8(eax, MEM8(ebp + 0xB));
    _fa = (uint32_t)(LO8(eax)) & 0xFFu; _fb = (uint32_t)(0) & 0xFFu;
    _fas = (int32_t)(int8_t)(_fa); _fbs = (int32_t)(int8_t)(_fb); /* cmp LO8(eax), 0 (8-bit) */
    if (CMP_EQ(_fa, _fb)) goto loc_00077A5E; /* je: equal / zero */

loc_00077A46: ;
    ecx = esi;
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x00077A4Du); RECOMP_ABI_CALL(0x00070E90u, sub_00070E90); /* call 0x00070E90 */

loc_00077A4D: ;
    _fa = (uint32_t)(LO8(eax)) & 0xFFu; _fb = (uint32_t)(0) & 0xFFu;
    _fas = (int32_t)(int8_t)(_fa); _fbs = (int32_t)(int8_t)(_fb); /* cmp LO8(eax), 0 (8-bit) */
    if (CMP_NE(_fa, _fb)) goto loc_00077A5E; /* jne: not equal / not zero */

loc_00077A51: ;
    eax = MEM32(0x3677B8);
    PUSH32(esp, eax);
    ecx = esi;
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x00077A5Eu); RECOMP_ABI_CALL(0x00072F40u, sub_00072F40); /* call 0x00072F40 */

loc_00077A5E: ;
    POP32(esp, edi);
    POP32(esp, ebx);

loc_00077A60: ;
    POP32(esp, esi);
    esp = ebp;
    POP32(esp, ebp);
    esp += 16; return; /* ret 12 */

    #undef fp_push
    #undef fp_pop
    #undef fp_top
    #undef fp_st
    #undef fp_st1
}

