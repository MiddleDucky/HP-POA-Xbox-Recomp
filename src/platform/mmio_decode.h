/*
 * mmio_decode.h -- one x86-64 instruction decoder for trapped MMIO.
 *
 * A device whose registers need semantics cannot be plain memory: the reads
 * have to be answered and the writes have to be seen. The way that works here
 * is to leave the page PAGE_NOACCESS, catch the access in a vectored handler,
 * decode the faulting instruction, service it against the device model, and
 * step over it.
 *
 * That decoder existed twice before this file -- once in nv2a_mmio_hook.c and
 * once in apu_mmio_hook.c -- as the same opcode table written out against two
 * different pairs of accessors. This is the same logic with the accessors
 * passed in, so a third device does not need a third copy. The two originals
 * still carry their own and can move onto this whenever they are next touched.
 *
 * Header-only and static inline: one function, one caller per device, and a
 * library for it would be more build wiring than code.
 *
 * The instructions covered are what the XDK device code actually emits against
 * registers -- moves both ways, the immediate forms, the zero-extending loads,
 * and the read-modify-write and flag-setting forms a poll loop is built from.
 * Anything outside that set returns 0 rather than guessing: stepping over an
 * instruction that was not understood corrupts the guest silently, which is
 * far worse than a fault naming the opcode.
 */
#ifndef MMIO_DECODE_H
#define MMIO_DECODE_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>

/* The register state the decoder reads and steps: the VEH's CONTEXT. */
typedef CONTEXT MMIO_CONTEXT;

#elif defined(__linux__) && defined(__x86_64__)
#include <ucontext.h>

/* Indices into uc_mcontext.gregs. These are the x86-64 kernel's sigcontext
 * order -- fixed ABI -- and glibc only names them (REG_RAX...) under
 * _GNU_SOURCE, which every includer would then have to define before its
 * first system header. */
enum {
    MMIO_GR_R8 = 0, MMIO_GR_R9, MMIO_GR_R10, MMIO_GR_R11, MMIO_GR_R12,
    MMIO_GR_R13, MMIO_GR_R14, MMIO_GR_R15, MMIO_GR_RDI, MMIO_GR_RSI,
    MMIO_GR_RBP, MMIO_GR_RBX, MMIO_GR_RDX, MMIO_GR_RAX, MMIO_GR_RCX,
    MMIO_GR_RSP, MMIO_GR_RIP, MMIO_GR_EFL, MMIO_GR_CSGSFS, MMIO_GR_ERR
};

/* Linux has no VEH; a SIGSEGV handler gets a ucontext_t instead. These are
 * the CONTEXT fields the decoder touches, under the same names, so the body
 * below is shared. mmio_ctx_load/mmio_ctx_store move them in and out of the
 * signal frame: load, emulate, and store only if the access was serviced. */
typedef struct {
    uint64_t Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi;
    uint64_t R8, R9, R10, R11, R12, R13, R14, R15;
    uint64_t Rip;
    uint32_t EFlags;
} MMIO_CONTEXT;

static inline void mmio_ctx_load(MMIO_CONTEXT *c, const ucontext_t *uc)
{
    const greg_t *g = uc->uc_mcontext.gregs;
    c->Rax = g[MMIO_GR_RAX]; c->Rcx = g[MMIO_GR_RCX]; c->Rdx = g[MMIO_GR_RDX];
    c->Rbx = g[MMIO_GR_RBX]; c->Rsp = g[MMIO_GR_RSP]; c->Rbp = g[MMIO_GR_RBP];
    c->Rsi = g[MMIO_GR_RSI]; c->Rdi = g[MMIO_GR_RDI];
    c->R8  = g[MMIO_GR_R8];  c->R9  = g[MMIO_GR_R9];  c->R10 = g[MMIO_GR_R10];
    c->R11 = g[MMIO_GR_R11]; c->R12 = g[MMIO_GR_R12]; c->R13 = g[MMIO_GR_R13];
    c->R14 = g[MMIO_GR_R14]; c->R15 = g[MMIO_GR_R15];
    c->Rip = g[MMIO_GR_RIP];
    c->EFlags = (uint32_t)g[MMIO_GR_EFL];
}

static inline void mmio_ctx_store(const MMIO_CONTEXT *c, ucontext_t *uc)
{
    greg_t *g = uc->uc_mcontext.gregs;
    g[MMIO_GR_RAX] = c->Rax; g[MMIO_GR_RCX] = c->Rcx; g[MMIO_GR_RDX] = c->Rdx;
    g[MMIO_GR_RBX] = c->Rbx; g[MMIO_GR_RSP] = c->Rsp; g[MMIO_GR_RBP] = c->Rbp;
    g[MMIO_GR_RSI] = c->Rsi; g[MMIO_GR_RDI] = c->Rdi;
    g[MMIO_GR_R8]  = c->R8;  g[MMIO_GR_R9]  = c->R9;  g[MMIO_GR_R10] = c->R10;
    g[MMIO_GR_R11] = c->R11; g[MMIO_GR_R12] = c->R12; g[MMIO_GR_R13] = c->R13;
    g[MMIO_GR_R14] = c->R14; g[MMIO_GR_R15] = c->R15;
    g[MMIO_GR_RIP] = c->Rip;
    g[MMIO_GR_EFL] = (g[MMIO_GR_EFL] & ~(greg_t)0xFFFFFFFF) | c->EFlags;
}

/* Whether the faulting access was a write: bit 1 of the page-fault code. */
static inline int mmio_ctx_fault_is_write(const ucontext_t *uc)
{
    return (uc->uc_mcontext.gregs[MMIO_GR_ERR] & 2) != 0;
}
#endif

#if defined(_WIN32) || (defined(__linux__) && defined(__x86_64__))

/* Service one register access. dev is passed straight back to the callbacks. */
typedef uint64_t (*mmio_read_fn)(void *dev, uint32_t off, int size);
typedef void     (*mmio_write_fn)(void *dev, uint32_t off, uint64_t val, int size);

static inline uint64_t *mmio_ctx_reg(MMIO_CONTEXT *c, int reg)
{
    switch (reg & 0xF) {
    case 0:  return (uint64_t *)&c->Rax;   case 1:  return (uint64_t *)&c->Rcx;
    case 2:  return (uint64_t *)&c->Rdx;   case 3:  return (uint64_t *)&c->Rbx;
    case 4:  return (uint64_t *)&c->Rsp;   case 5:  return (uint64_t *)&c->Rbp;
    case 6:  return (uint64_t *)&c->Rsi;   case 7:  return (uint64_t *)&c->Rdi;
    case 8:  return (uint64_t *)&c->R8;    case 9:  return (uint64_t *)&c->R9;
    case 10: return (uint64_t *)&c->R10;   case 11: return (uint64_t *)&c->R11;
    case 12: return (uint64_t *)&c->R12;   case 13: return (uint64_t *)&c->R13;
    case 14: return (uint64_t *)&c->R14;   case 15: return (uint64_t *)&c->R15;
    default: return NULL;
    }
}

static inline int mmio_modrm_len(const uint8_t *ip, int rex_b)
{
    uint8_t modrm = *ip;
    int mod = (modrm >> 6) & 3;
    int rm  = (modrm & 7) | (rex_b ? 8 : 0);
    int len = 1;

    if (mod == 3) return 1;
    if ((rm & 7) == 4) len += 1;                    /* SIB    */
    if (mod == 0 && (rm & 7) == 5) len += 4;        /* disp32 */
    else if (mod == 1) len += 1;
    else if (mod == 2) len += 4;
    return len;
}

/* Flags after a compare-shaped operation, so a poll loop branches correctly.
 * ZF, SF and CF only: those are what jz/jnz, js and jb/jae read, and inventing
 * an overflow flag nothing here sets is worse than leaving it alone. */
static inline void mmio_set_flags(MMIO_CONTEXT *ctx, uint64_t result, int size,
                                  int carry)
{
    ctx->EFlags &= ~(0x0001u | 0x0040u | 0x0080u | 0x0800u);
    if (size < 8)
        result &= (1ULL << (size * 8)) - 1;
    if (result == 0)
        ctx->EFlags |= 0x0040u;                                   /* ZF */
    if (result & (1ULL << (size * 8 - 1)))
        ctx->EFlags |= 0x0080u;                                   /* SF */
    if (carry)
        ctx->EFlags |= 0x0001u;                                   /* CF */
}

/* The eight classic ALU operations (the 00-3F opcode rows, and /0../7 of the
 * 80/81/83 group): result into *out, flags into ctx. ADC/SBB read CF. CMP
 * computes like SUB and the caller discards the result. */
static inline uint64_t mmio_alu(MMIO_CONTEXT *ctx, int alu, uint64_t a, uint64_t b,
                                int size)
{
    uint64_t mask = (size < 8) ? ((1ULL << (size * 8)) - 1) : ~0ULL;
    uint64_t cin = ctx->EFlags & 1u, r;
    int carry = 0;

    a &= mask; b &= mask;
    switch (alu) {
    case 0: r = a + b;       carry = (r & mask) < a; break;              /* ADD */
    case 1: r = a | b;       break;                                      /* OR  */
    case 2: r = a + b + cin; carry = (r & mask) < a || (cin && (r & mask) == a); break; /* ADC */
    case 3: r = a - b - cin; carry = a < b + cin || (cin && b == mask); break;        /* SBB */
    case 4: r = a & b;       break;                                      /* AND */
    case 5: case 7:
            r = a - b;       carry = a < b; break;                       /* SUB, CMP */
    default: r = a ^ b;      break;                                      /* XOR */
    }
    mmio_set_flags(ctx, r, size, carry);
    return r & mask;
}

/* Write a result into a register the way x86 does: 8 and 16 bit merge into
 * the old value, 32 bit zero-extends, 64 bit replaces. */
static inline void mmio_set_reg(MMIO_CONTEXT *ctx, int reg, uint64_t v, int size)
{
    uint64_t *dst = mmio_ctx_reg(ctx, reg);
    if (size == 1)      *dst = (*dst & ~0xFFULL)   | (v & 0xFF);
    else if (size == 2) *dst = (*dst & ~0xFFFFULL) | (v & 0xFFFF);
    else if (size == 4) *dst = v & 0xFFFFFFFFULL;
    else                *dst = v;
}

/* 1 if the instruction at ctx->Rip was serviced and Rip advanced past it. */
static inline int mmio_emulate(MMIO_CONTEXT *ctx, uint32_t off, void *dev,
                               mmio_read_fn rd, mmio_write_fn wr)
{
    const uint8_t *ip = (const uint8_t *)ctx->Rip;
    int prefix = 0, has66 = 0, rex = 0, has_rex = 0;
    const uint8_t *op;
    int size, rex_w, rex_r, rex_b, mlen, reg;

    if (!rd || !wr)
        return 0;

    for (;;) {
        uint8_t b = ip[prefix];
        if (b == 0x66)                   { has66 = 1; prefix++; }
        else if (b == 0xF2 || b == 0xF3) { prefix++; }
        else if (b >= 0x40 && b <= 0x4F) { rex = b; has_rex = 1; prefix++; }
        else break;
    }
    rex_w = has_rex && (rex & 0x08);
    rex_r = has_rex && (rex & 0x04);
    rex_b = has_rex && (rex & 0x01);

    op   = ip + prefix;
    size = has66 ? 2 : (rex_w ? 8 : 4);

    switch (op[0]) {
    case 0x88: case 0x89:                            /* MOV r/m, r   (write) */
        if (op[0] == 0x88) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        wr(dev, off, *mmio_ctx_reg(ctx, reg), size);
        ctx->Rip += prefix + 1 + mlen;
        return 1;

    case 0xC7:                                       /* MOV r/m, imm32       */
        mlen = mmio_modrm_len(op + 1, rex_b);
        wr(dev, off, *(const uint32_t *)(op + 1 + mlen), size);
        ctx->Rip += prefix + 1 + mlen + 4;
        return 1;

    case 0xC6:                                       /* MOV r/m8, imm8       */
        mlen = mmio_modrm_len(op + 1, rex_b);
        wr(dev, off, op[1 + mlen], 1);
        ctx->Rip += prefix + 1 + mlen + 1;
        return 1;

    case 0x8A: case 0x8B: {                          /* MOV r, r/m   (read)  */
        uint64_t v, *dst;
        if (op[0] == 0x8A) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        v    = rd(dev, off, size);
        dst  = mmio_ctx_reg(ctx, reg);
        if (size == 1)      *dst = (*dst & ~0xFFULL)   | (v & 0xFF);
        else if (size == 2) *dst = (*dst & ~0xFFFFULL) | (v & 0xFFFF);
        else if (size == 4) *dst = v & 0xFFFFFFFFULL;  /* 32-bit clears high */
        else                *dst = v;
        ctx->Rip += prefix + 1 + mlen;
        return 1;
    }

    case 0x84: case 0x85: {                          /* TEST r/m, r          */
        uint64_t m, r;
        if (op[0] == 0x84) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        m    = rd(dev, off, size);
        r    = *mmio_ctx_reg(ctx, reg);
        mmio_set_flags(ctx, m & r, size, 0);
        ctx->Rip += prefix + 1 + mlen;
        return 1;
    }

    case 0x38: case 0x39: {                          /* CMP r/m, r           */
        uint64_t m, r, mask;
        if (op[0] == 0x38) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        m    = rd(dev, off, size);
        r    = *mmio_ctx_reg(ctx, reg);
        mask = (size < 8) ? ((1ULL << (size * 8)) - 1) : ~0ULL;
        m &= mask; r &= mask;
        mmio_set_flags(ctx, m - r, size, m < r);
        ctx->Rip += prefix + 1 + mlen;
        return 1;
    }

    case 0x08: case 0x09:                            /* OR  r/m, r           */
        if (op[0] == 0x08) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        wr(dev, off, rd(dev, off, size) | *mmio_ctx_reg(ctx, reg), size);
        ctx->Rip += prefix + 1 + mlen;
        return 1;

    case 0x20: case 0x21:                            /* AND r/m, r           */
        if (op[0] == 0x20) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        wr(dev, off, rd(dev, off, size) & *mmio_ctx_reg(ctx, reg), size);
        ctx->Rip += prefix + 1 + mlen;
        return 1;

    /* The rest of the ALU rows, which GCC uses freely on a register it has
     * just loaded -- `and r8d, [rcx+rdx]` read the OHCI interrupt status in
     * a MinGW build and fell through to a crash. Even opcodes are the byte
     * forms; op & 2 says the register is the destination. ADC and SBB are
     * here for completeness, CMP writes nothing. */
    case 0x00: case 0x01: case 0x02: case 0x03:      /* ADD */
    case 0x0A: case 0x0B:                            /* OR  r, r/m */
    case 0x10: case 0x11: case 0x12: case 0x13:      /* ADC */
    case 0x18: case 0x19: case 0x1A: case 0x1B:      /* SBB */
    case 0x22: case 0x23:                            /* AND r, r/m */
    case 0x28: case 0x29: case 0x2A: case 0x2B:      /* SUB */
    case 0x30: case 0x31: case 0x32: case 0x33:      /* XOR */
    case 0x3A: case 0x3B: {                          /* CMP r, r/m */
        int alu = op[0] >> 3, to_reg = op[0] & 2;
        uint64_t m, r, v;
        if (!(op[0] & 1)) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        reg  = ((op[1] >> 3) & 7) | (rex_r ? 8 : 0);
        m    = rd(dev, off, size);
        r    = *mmio_ctx_reg(ctx, reg);
        v    = to_reg ? mmio_alu(ctx, alu, r, m, size) : mmio_alu(ctx, alu, m, r, size);
        if (alu != 7) {
            if (to_reg) mmio_set_reg(ctx, reg, v, size);
            else        wr(dev, off, v, size);
        }
        ctx->Rip += prefix + 1 + mlen;
        return 1;
    }

    case 0x80: case 0x81: case 0x83: {              /* ALU r/m, imm         */
        int alu = (op[1] >> 3) & 7, ilen;
        uint64_t imm, v;
        if (op[0] == 0x80) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        if (op[0] == 0x81 && size == 2) {
            imm = *(const uint16_t *)(op + 1 + mlen); ilen = 2;
        } else if (op[0] == 0x81) {
            imm = (uint64_t)(int64_t)*(const int32_t *)(op + 1 + mlen); ilen = 4;
        } else {
            imm = (uint64_t)(int64_t)*(const int8_t *)(op + 1 + mlen); ilen = 1;
        }
        v = mmio_alu(ctx, alu, rd(dev, off, size), imm, size);
        if (alu != 7)
            wr(dev, off, v, size);
        ctx->Rip += prefix + 1 + mlen + ilen;
        return 1;
    }

    case 0xF6: case 0xF7:                            /* TEST r/m, imm (/0)   */
        if ((op[1] >> 3) & 7)
            return 0;                                /* NOT/NEG/MUL/DIV: no  */
        if (op[0] == 0xF6) size = 1;
        mlen = mmio_modrm_len(op + 1, rex_b);
        {
            int ilen = size == 1 ? 1 : size == 2 ? 2 : 4;
            uint64_t imm = ilen == 1 ? op[1 + mlen]
                         : ilen == 2 ? *(const uint16_t *)(op + 1 + mlen)
                         : (uint64_t)(int64_t)*(const int32_t *)(op + 1 + mlen);
            mmio_set_flags(ctx, rd(dev, off, size) & imm, size, 0);
            ctx->Rip += prefix + 1 + mlen + ilen;
        }
        return 1;

    case 0x0F:
        if (op[1] == 0xB6 || op[1] == 0xB7) {        /* MOVZX r32, r/m8|16   */
            int s = (op[1] == 0xB6) ? 1 : 2;
            mlen  = mmio_modrm_len(op + 2, rex_b);
            reg   = ((op[2] >> 3) & 7) | (rex_r ? 8 : 0);
            *mmio_ctx_reg(ctx, reg) = rd(dev, off, s) & ((1ULL << (s * 8)) - 1);
            ctx->Rip += prefix + 2 + mlen;
            return 1;
        }
        return 0;

    default:
        return 0;
    }
}

#endif /* _WIN32 || Linux x86-64 */
#endif /* MMIO_DECODE_H */
