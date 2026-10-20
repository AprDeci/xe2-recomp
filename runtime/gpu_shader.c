/* Maxwell (SM 5.x) shader programs to HLSL, Shader Model 5.
 *
 * Instruction set: opcodes from the top 16 bits (most specific pattern wins); operand fields
 * as decoded by the yuzu project's translator. A program is a 0x50-byte header (SPH), then
 * 64-bit instructions in groups of four whose first is scheduling control.
 *
 * Model: every general register is a uint variable, every predicate a bool. Floating point
 * values are reinterpreted with asfloat/asuint, so registers keep exact bits. Constant
 * buffers are raw buffers (any offset, out of range reads 0). Vertex shaders pull their
 * attributes from raw vertex-stream buffers themselves, decoding every Maxwell format.
 *
 * Control flow: a program without branches is emitted straight. Otherwise its basic blocks
 * become cases of a switch inside a loop driven by a `pc` variable, with the SSY/PBK
 * reconvergence targets on small per-thread stacks - per-thread execution makes SYNC and
 * BRK plain jumps to the saved target. Pixel shaders in that form sample with explicit LOD
 * zero (gradients are unavailable inside a loop). */
#define _CRT_SECURE_NO_WARNINGS
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_shader.h"

/* The Linux build compiles HLSL with vkd3d-shader, which lacks a few things fxc has
 * (CalculateLevelOfDetail, RWByteAddressBuffer.Load, gathers with four offsets): for it the
 * translator writes those the long way. SSBU_HLSL_FOR_VKD3D does the same on Windows, to test. */
#if !defined(_WIN32) || defined(SSBU_HLSL_FOR_VKD3D)
#define VKD3D_FORMS 1
#else
#define VKD3D_FORMS 0
#endif

#define BITS(w, pos, n) ((u32)(((w) >> (pos)) & ((1ull << (n)) - 1)))
#define RZ 255
#define PT 7

/* ---- opcodes ----------------------------------------------------------------------- */
#define OPCODES(X) \
    X(ALD, "1110111111011---") X(AST, "1110111111110---") X(BAR, "1111000010101---") \
    X(BFE_reg, "0101110000000---") X(BFE_cbuf, "0100110000000---") X(BFE_imm, "0011100-00000---") \
    X(BFI_reg, "0101101111110---") X(BFI_rc, "0101001111110---") X(BFI_cr, "0100101111110---") X(BFI_imm, "0011011-11110---") \
    X(BRA, "111000100100----") X(BRK, "111000110100----") X(BRX, "111000100101----") \
    X(DEPBAR, "1111000011110---") X(EXIT, "111000110000----") \
    X(F2F_reg, "0101110010101---") X(F2F_cbuf, "0100110010101---") X(F2F_imm, "0011100-10101---") \
    X(F2I_reg, "0101110010110---") X(F2I_cbuf, "0100110010110---") X(F2I_imm, "0011100-10110---") \
    X(FADD_reg, "0101110001011---") X(FADD_cbuf, "0100110001011---") X(FADD_imm, "0011100-01011---") X(FADD32I, "000010----------") \
    X(FFMA_reg, "010110011-------") X(FFMA_rc, "010100011-------") X(FFMA_cr, "010010011-------") X(FFMA_imm, "0011001-1-------") \
    X(FFMA32I, "000011----------") \
    X(FMNMX_reg, "0101110001100---") X(FMNMX_cbuf, "0100110001100---") X(FMNMX_imm, "0011100-01100---") \
    X(FMUL_reg, "0101110001101---") X(FMUL_cbuf, "0100110001101---") X(FMUL_imm, "0011100-01101---") X(FMUL32I, "00011110--------") \
    X(CSET, "0101000010011---") X(CSETP, "0101000010100---")     X(FSET_reg, "01011000--------") X(FSET_cbuf, "01001000--------") X(FSET_imm, "0011000---------") \
    X(FSETP_reg, "010110111011----") X(FSETP_cbuf, "010010111011----") X(FSETP_imm, "0011011-1011----") \
    X(HADD2_reg, "0101110100010---") X(HADD2_cbuf, "0111101-1-------") X(HADD2_imm, "0111101-0-------") X(HADD2_32I, "0010110---------") \
    X(HFMA2_reg, "0101110100000---") X(HFMA2_rc, "01100---1-------") X(HFMA2_cr, "01110---1-------") X(HFMA2_imm, "01110---0-------") \
    X(HFMA2_32I, "0010100---------") \
    X(HSET2_reg, "0101110100011---") X(HSET2_cbuf, "0111110-1-------") X(HSET2_imm, "0111110-0-------")     X(HSETP2_reg, "0101110100100---") X(HSETP2_cbuf, "0111111-1-------") X(HSETP2_imm, "0111111-0-------")     X(HMUL2_reg, "0101110100001---") X(HMUL2_cbuf, "0111100-1-------") X(HMUL2_imm, "0111100-0-------") X(HMUL2_32I, "0010101---------") \
    X(I2F_reg, "0101110010111---") X(I2F_cbuf, "0100110010111---") X(I2F_imm, "0011100-10111---") \
    X(I2I_reg, "0101110011100---") X(I2I_cbuf, "0100110011100---") X(I2I_imm, "0011100-11100---") \
    X(IADD_reg, "0101110000010---") X(IADD_cbuf, "0100110000010---") X(IADD_imm, "0011100-00010---") X(IADD32I, "0001110---------") \
    X(IMNMX_reg, "0101110000100---") X(IMNMX_cbuf, "0100110000100---") X(IMNMX_imm, "0011100-00100---") \
    X(IPA, "11100000--------") \
    X(ISCADD_reg, "0101110000011---") X(ISCADD_cbuf, "0100110000011---") X(ISCADD_imm, "0011100-00011---") X(ISCADD32I, "000101----------") \
    X(ISET_reg, "010110110101----") X(ISET_cbuf, "010010110101----") X(ISET_imm, "0011011-0101----") \
    X(ISETP_reg, "010110110110----") X(ISETP_cbuf, "010010110110----") X(ISETP_imm, "0011011-0110----") \
    X(KIL, "111000110011----") X(LDC, "1110111110010---") X(LDG, "1110111011010---") X(STG, "1110111011011---") X(LDS, "1110111101001---") X(STS, "1110111101011---") X(LDL, "1110111101000---") X(STL, "1110111101010---") \
    X(LOP_reg, "0101110001000---") X(LOP_cbuf, "0100110001000---") X(LOP_imm, "0011100-01000---") X(LOP32I, "000001----------") \
    X(LOP3_reg, "0101101111100---") X(LOP3_cbuf, "0000001---------") X(LOP3_imm, "001111----------") \
    X(MEMBAR, "1110111110011---") \
    X(MOV_reg, "0101110010011---") X(MOV_cbuf, "0100110010011---") X(MOV_imm, "0011100-10011---") X(MOV32I, "000000010000----") \
    X(MUFU, "0101000010000---") X(NOP, "0101000010110---") X(PBK, "111000101010----") \
    X(PSET, "0101000010001---") X(PSETP, "0101000010010---") \
    X(R2P_reg, "0101110011110---") X(R2P_cbuf, "0100110011110---") X(R2P_imm, "0011100-11110---") \
    X(IADD3_reg, "010111001100----") X(IADD3_cbuf, "010011001100----") X(IADD3_imm, "0011100-1100----") \
    X(TMML, "1101111101011---") \
    X(RRO_reg, "0101110010010---") X(RRO_cbuf, "0100110010010---") X(RRO_imm, "0011100-10010---") \
    X(S2R, "1111000011001---") \
    X(SEL_reg, "0101110010100---") X(SEL_cbuf, "0100110010100---") X(SEL_imm, "0011100-10100---") \
    X(SHL_reg, "0101110001001---") X(SHL_cbuf, "0100110001001---") X(SHL_imm, "0011100-01001---") \
    X(SHR_reg, "0101110000101---") X(SHR_cbuf, "0100110000101---") X(SHR_imm, "0011100-00101---") \
    X(SSY, "111000101001----") X(SYNC, "1111000011111---") \
    X(TEX, "11000-----------") X(TEXS, "1101-00---------") X(TLD4, "110010----------") X(TLD4S, "11011111-0------") \
    X(TLDS, "1101-01---------") X(TXQ, "1101111101001---") \
    X(VOTE, "0101000011011---") X(VOTE_vtg, "0101000011100---") \
    X(XMAD_reg, "0101101100------") X(XMAD_rc, "010100010-------") X(XMAD_cr, "0100111---------") X(XMAD_imm, "0011011-00------")

enum {
    OP_UNKNOWN,
#define ENUM(name, pat) OP_##name,
    OPCODES(ENUM)
#undef ENUM
    OP_COUNT
};
static const struct { const char *name, *pattern; } OPS[] = {
    {"?", ""},
#define ROW(name, pat) {#name, pat},
    OPCODES(ROW)
#undef ROW
};
static struct { u16 mask, value, op, bits; } table[OP_COUNT];
static u32 table_size;

static int by_specificity(const void *a, const void *b) {
    return (int)((const __typeof__(table[0]) *)b)->bits - (int)((const __typeof__(table[0]) *)a)->bits;
}

static void init_table(void) {
    if (table_size) return;
    for (u32 i = 1; i < OP_COUNT; i++) {
        u16 mask = 0, value = 0;
        for (int b = 0; b < 16; b++) {
            char ch = OPS[i].pattern[b];
            mask = (u16)(mask << 1 | (ch != '-'));
            value = (u16)(value << 1 | (ch == '1'));
        }
        table[table_size++] = (__typeof__(table[0])){mask, value, (u16)i, (u16)__builtin_popcount(mask)};
    }
    qsort(table, table_size, sizeof table[0], by_specificity);
}

static u32 decode(u64 w) {
    u16 top = (u16)(w >> 48);
    for (u32 i = 0; i < table_size; i++)
        if ((top & table[i].mask) == table[i].value) return table[i].op;
    return OP_UNKNOWN;
}

/* ---- output text ----------------------------------------------------------------- */
typedef struct Str { char *s; size_t n, cap; } Str;

static void put(Str *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (b->n + need + 1 > b->cap) {
        b->cap = (b->n + need + 1) * 2 + 4096;
        b->s = realloc(b->s, b->cap);
    }
    va_start(ap, fmt);
    vsnprintf(b->s + b->n, need + 1, fmt, ap);
    va_end(ap);
    b->n += need;
}

/* Short-lived strings for operand expressions. */
static char *tmp(const char *fmt, ...) {
    static __declspec(thread) char ring[64][512];
    static __declspec(thread) u32 next;
    char *s = ring[next++ & 63];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s, 512, fmt, ap);
    va_end(ap);
    return s;
}

/* ---- translation context ------------------------------------------------------------- */
typedef struct Insn { u32 addr; u64 w; u32 op; } Insn;

typedef struct Ctx {
    int stage;
    const u8 *prog;
    Insn *ins;
    u32 n;
    Str body;
    u8 reg_used[256], pred_used[8];
    ShaderInfo *info;
    int dispatch;                 /* blocks in a switch loop */
    u8 *leader;                   /* per instruction: starts a block */
    u32 out_attr_mask;            /* vertex: generic attributes written (from AST) */
    int uses_cc;
    int uses_gmem;                /* global memory loads (LDG) */
    int uses_smem;                /* compute: shared memory */
    int uses_lmem;                /* local (per-thread) memory */
    u32 local[3], shared;         /* compute: workgroup size, shared memory bytes */
    u32 vs_outputs;               /* pixel: the paired vertex shader's outputs */
    CbufReader *read_cbuf;        /* optional: constant buffer contents, for jump tables */
    void *read_user;
    int indent;
} Ctx;

static void line(Ctx *c, const char *fmt, ...) {
    char text[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    put(&c->body, "%*s%s\n", c->indent * 2, "", text);
}

static void unsupported(Ctx *c, const Insn *in, const char *why) {
    if (!c->info->unsupported[0])
        snprintf(c->info->unsupported, sizeof c->info->unsupported, "%s at %#x (%016llx): %s", OPS[in->op].name, in->addr,
                 in->w, why);
    line(c, "/* unsupported: %s %016llx (%s) */", OPS[in->op].name, in->w, why);
}

/* Registers: RZ reads as zero; writes to it vanish. */
static const char *R(Ctx *c, u32 r) {
    if (r == RZ) return "0u";
    c->reg_used[r] = 1;
    return tmp("r%u", r);
}

static const char *F(Ctx *c, u32 r) { return r == RZ ? "0.0f" : tmp("asfloat(%s)", R(c, r)); }

static const char *PRED(Ctx *c, u32 p, int neg) {
    if (p == PT) return neg ? "false" : "true";
    c->pred_used[p] = 1;
    return tmp(neg ? "!p%u" : "p%u", p);
}

static void set_u(Ctx *c, u32 r, const char *expr) {
    if (r != RZ) line(c, "%s = %s;", R(c, r), expr);
}

static void set_f(Ctx *c, u32 r, const char *expr) {
    if (r != RZ) line(c, "%s = asuint(%s);", R(c, r), expr);
}

static void set_p(Ctx *c, u32 p, const char *expr) {
    if (p != PT) line(c, "%s = %s;", PRED(c, p, 0), expr);
}

static const char *cbuf(Ctx *c, u32 bank, u32 offset) {
    c->info->cbuf_mask |= 1u << bank;
    if (c->info->cbuf_extent[bank] < (offset & ~3u) + 4) c->info->cbuf_extent[bank] = (offset & ~3u) + 4;
    if (bank == 0 && offset >= SB_FIRST && offset < SB_END) {   /* a storage buffer descriptor: note it */
        u32 o = offset & ~15u, k = 0;
        while (k < c->info->sb_count && c->info->sb_offset[k] != o) k++;
        if (k == c->info->sb_count && k < 16) c->info->sb_offset[c->info->sb_count++] = o;
    }
    return tmp("cb%u.Load(%uu)", bank, offset & ~3u);
}

/* Operand forms. */
enum { FORM_REG, FORM_CBUF, FORM_IMM, FORM_RC, FORM_CR, FORM_32I };

static u32 imm20(u64 w) { u32 v = BITS(w, 20, 19); return BITS(w, 56, 1) ? v | 0xFFF80000u : v; }
static u32 fimm20(u64 w) { return BITS(w, 20, 19) << 12 | BITS(w, 56, 1) << 31; }

/* Operand B, as uint. */
static const char *b_u(Ctx *c, u64 w, int form) {
    switch (form) {
    case FORM_REG: return R(c, BITS(w, 20, 8));
    case FORM_CBUF: case FORM_CR: return cbuf(c, BITS(w, 34, 5), BITS(w, 20, 14) * 4);
    case FORM_RC: return R(c, BITS(w, 39, 8));
    case FORM_IMM: return tmp("0x%08xu", imm20(w));
    case FORM_32I: return tmp("0x%08xu", BITS(w, 20, 32));
    }
    return "0u";
}

static const char *b_f(Ctx *c, u64 w, int form) {
    if (form == FORM_IMM) return tmp("asfloat(0x%08xu)", fimm20(w));
    return tmp("asfloat(%s)", b_u(c, w, form));
}

/* Operand C of three-operand instructions, as uint. */
static const char *c_u(Ctx *c, u64 w, int form) {
    if (form == FORM_RC) return cbuf(c, BITS(w, 34, 5), BITS(w, 20, 14) * 4);
    return R(c, BITS(w, 39, 8));
}

static const char *absneg(const char *x, int abs, int neg) {
    if (abs) x = tmp("abs(%s)", x);
    if (neg) x = tmp("(-%s)", x);
    return x;
}

static int form_of(u32 op) {
    const char *n = OPS[op].name, *u = strrchr(n, '_');
    size_t len = strlen(n);
    if (len > 3 && !strcmp(n + len - 3, "32I")) return FORM_32I;   /* IADD32I, FMUL32I, HADD2_32I, ... */
    if (!u) return FORM_REG;
    if (!strcmp(u, "_reg")) return FORM_REG;
    if (!strcmp(u, "_cbuf")) return FORM_CBUF;
    if (!strcmp(u, "_imm")) return FORM_IMM;
    if (!strcmp(u, "_rc")) return FORM_RC;
    if (!strcmp(u, "_cr")) return FORM_CR;
    if (!strcmp(u, "_32I")) return FORM_32I;
    return FORM_REG;
}

/* ---- comparisons ------------------------------------------------------------------ */
static const char *fcompare(u32 op, const char *a, const char *b) {
    switch (op) {
    case 0: return "false";
    case 1: return tmp("(%s < %s)", a, b);
    case 2: return tmp("(%s == %s)", a, b);
    case 3: return tmp("(%s <= %s)", a, b);
    case 4: return tmp("(%s > %s)", a, b);
    case 5: return tmp("(%s != %s && !isnan(%s) && !isnan(%s))", a, b, a, b);
    case 6: return tmp("(%s >= %s)", a, b);
    case 7: return tmp("(!isnan(%s) && !isnan(%s))", a, b);
    case 8: return tmp("(isnan(%s) || isnan(%s))", a, b);
    case 9: return tmp("!(%s >= %s)", a, b);
    case 10: return tmp("!(%s != %s && !isnan(%s) && !isnan(%s))", a, b, a, b);
    case 11: return tmp("!(%s > %s)", a, b);
    case 12: return tmp("!(%s <= %s)", a, b);
    case 13: return tmp("(%s != %s)", a, b);
    case 14: return tmp("!(%s < %s)", a, b);
    }
    return "true";
}

static const char *icompare(u32 op, int is_signed, const char *a, const char *b) {
    if (is_signed) { a = tmp("asint(%s)", a); b = tmp("asint(%s)", b); }
    static const char *const ops[] = {"", "<", "==", "<=", ">", "!=", ">="};
    if (op == 0) return "false";
    if (op == 7) return "true";
    return tmp("(%s %s %s)", a, ops[op], b);
}

static const char *combine(u32 bop, const char *x, const char *pred) {
    if (!strcmp(pred, "true") && bop == 0) return x;
    static const char *const ops[] = {"&&", "||", "!="};
    return tmp("(%s %s %s)", x, ops[bop < 3 ? bop : 0], pred);
}

/* ---- textures ------------------------------------------------------------------------ */
static u32 texture_slot(Ctx *c, u32 handle_offset, u32 type, int shadow) {
    ShaderInfo *i = c->info;
    for (u32 k = 0; k < i->ntex; k++)
        if (i->tex[k].handle_offset == handle_offset && i->tex[k].type == type && i->tex[k].shadow == shadow) return k;
    if (i->ntex == 32) return 31;
    i->tex[i->ntex] = (ShaderTexture){handle_offset, (u8)type, (u8)shadow};
    return i->ntex++;
}

/* Can this stage compute gradients here? (Implicit-LOD sampling needs them.) */
static int gradients(Ctx *c) { return c->stage == SHADER_PIXEL && !c->dispatch; }

static const char *coord_of(u32 type, const char *x, const char *y, const char *z, const char *layer) {
    switch (type) {
    case TEX_1D: return x;
    case TEX_ARRAY_1D: return tmp("float2(%s, %s)", x, layer);
    case TEX_2D: return tmp("float2(%s, %s)", x, y);
    case TEX_ARRAY_2D: return tmp("float3(%s, %s, %s)", x, y, layer);
    case TEX_3D: case TEX_CUBE: return tmp("float3(%s, %s, %s)", x, y, z);
    case TEX_ARRAY_CUBE: return tmp("float4(%s, %s, %s, %s)", x, y, z, layer);
    }
    return x;
}

/* A sample expression. lod_mode: 0 implicit, 1 zero, 2 bias, 3 explicit. */
static const char *sample(Ctx *c, u32 k, u32 type, const char *coord, int lod_mode, const char *lod, const char *dref,
                          const char *offset) {
    const char *t = tmp("tex%u", k), *s = tmp("smp%u", k);
    if ((lod_mode == 0 || lod_mode == 2) && !gradients(c)) lod_mode = 1;
    const char *off = offset ? tmp(", %s", offset) : "";
    if (dref) {   /* depth compare: one value */
        if (lod_mode == 0 || lod_mode == 2) return tmp("%s.SampleCmp(%s, %s, %s%s).xxxx", t, s, coord, dref, off);
        return tmp("%s.SampleCmpLevelZero(%s, %s, %s%s).xxxx", t, s, coord, dref, off);
    }
    (void)type;
    switch (lod_mode) {
    case 0: return tmp("%s.Sample(%s, %s%s)", t, s, coord, off);
    case 2: return tmp("%s.SampleBias(%s, %s, %s%s)", t, s, coord, lod, off);
    case 3: return tmp("%s.SampleLevel(%s, %s, %s%s)", t, s, coord, lod, off);
    }
    return tmp("%s.SampleLevel(%s, %s, 0%s)", t, s, coord, off);
}

/* Write the components selected by `mask` of float4 variable `v` to the TEXS-style
 * destinations: slots 0,1 -> a, a+1; slots 2,3 -> b, b+1 (or packed halves for F16). */
static void store_split(Ctx *c, const char *v, u32 mask, u32 da, u32 db, int f16) {
    const char *comp[4] = {"x", "y", "z", "w"};
    const char *sel[4];
    u32 n = 0;
    for (u32 i = 0; i < 4; i++) if (mask >> i & 1) sel[n++] = comp[i];
    if (f16) {
        if (n >= 1) set_u(c, da, tmp("f32tof16(%s.%s) | (%s << 16)", v, sel[0], n >= 2 ? tmp("f32tof16(%s.%s)", v, sel[1]) : "0u"));
        if (n >= 3) set_u(c, db, tmp("f32tof16(%s.%s) | (%s << 16)", v, sel[2], n >= 4 ? tmp("f32tof16(%s.%s)", v, sel[3]) : "0u"));
        return;
    }
    u32 dst[4] = {da, da == RZ ? RZ : da + 1, db, db == RZ ? RZ : db + 1};
    for (u32 i = 0; i < n; i++) set_f(c, dst[i], tmp("%s.%s", v, sel[i]));
}

/* Consecutive destinations for the components selected by mask (TEX, TLD4). */
static void store_seq(Ctx *c, const char *v, u32 mask, u32 d) {
    const char *comp[4] = {"x", "y", "z", "w"};
    for (u32 i = 0; i < 4; i++)
        if (mask >> i & 1) { set_f(c, d, tmp("%s.%s", v, comp[i])); if (d != RZ) d++; }
}

static const char *layer_of(Ctx *c, u32 r) { return tmp("(float)(%s & 0xFFFFu)", R(c, r)); }

static u32 tex_counter;

static void emit_texs(Ctx *c, const Insn *in) {
    u64 w = in->w;
    u32 da = BITS(w, 0, 8), a = BITS(w, 8, 8), b = BITS(w, 20, 8), db = BITS(w, 28, 8);
    u32 handle = BITS(w, 36, 13) * 4, swz = BITS(w, 50, 3), enc = BITS(w, 53, 4);
    int f16 = !BITS(w, 59, 1);
    static const u8 RG[8] = {1, 2, 4, 8, 3, 9, 10, 12}, RGBA[8] = {7, 11, 13, 14, 15, 15, 15, 15};
    u32 mask = db == RZ ? RG[swz] : RGBA[swz];
    u32 type = TEX_2D; int lod = 0; const char *coord, *lodx = NULL, *dref = NULL;
    switch (enc) {
    case 0: type = TEX_1D; lod = 1; coord = F(c, a); break;
    case 1: coord = coord_of(TEX_2D, F(c, a), F(c, b), 0, 0); break;
    case 2: lod = 1; coord = coord_of(TEX_2D, F(c, a), F(c, b), 0, 0); break;
    case 3: lod = 3; lodx = F(c, b); coord = coord_of(TEX_2D, F(c, a), F(c, a + 1), 0, 0); break;
    case 4: dref = F(c, b); coord = coord_of(TEX_2D, F(c, a), F(c, a + 1), 0, 0); break;
    case 5: lod = 3; lodx = F(c, b); dref = F(c, b + 1); coord = coord_of(TEX_2D, F(c, a), F(c, a + 1), 0, 0); break;
    case 6: lod = 1; dref = F(c, b); coord = coord_of(TEX_2D, F(c, a), F(c, a + 1), 0, 0); break;
    case 7: type = TEX_ARRAY_2D; coord = coord_of(type, F(c, a + 1), F(c, b), 0, layer_of(c, a)); break;
    case 8: type = TEX_ARRAY_2D; lod = 1; coord = coord_of(type, F(c, a + 1), F(c, b), 0, layer_of(c, a)); break;
    case 9: type = TEX_ARRAY_2D; lod = 1; dref = F(c, b + 1); coord = coord_of(type, F(c, a + 1), F(c, b), 0, layer_of(c, a)); break;
    case 10: type = TEX_3D; coord = coord_of(type, F(c, a), F(c, a + 1), F(c, b), 0); break;
    case 11: type = TEX_3D; lod = 1; coord = coord_of(type, F(c, a), F(c, a + 1), F(c, b), 0); break;
    case 12: type = TEX_CUBE; coord = coord_of(type, F(c, a), F(c, a + 1), F(c, b), 0); break;
    case 13: type = TEX_CUBE; lod = 3; lodx = F(c, b + 1); coord = coord_of(type, F(c, a), F(c, a + 1), F(c, b), 0); break;
    default: unsupported(c, in, "TEXS encoding"); return;
    }
    u32 k = texture_slot(c, handle, type, dref != NULL);
    const char *v = tmp("t%u", tex_counter++);
    line(c, dref ? "{ float4 %s = %s;" : "{ float4 %s = swz(%s, %uu);", v, sample(c, k, type, coord, lod, lodx, dref, NULL), k);
    if (dref) line(c, "%s.w = 1.0f;", v);
    c->indent++;
    store_split(c, v, mask, da, db, f16);
    c->indent--;
    line(c, "}");
}

static void emit_tex(Ctx *c, const Insn *in) {
    u64 w = in->w;
    u32 d = BITS(w, 0, 8), a = BITS(w, 8, 8), m = BITS(w, 20, 8), type = BITS(w, 28, 3), mask = BITS(w, 31, 4);
    u32 handle = BITS(w, 36, 13) * 4, dc = BITS(w, 50, 1), aoffi = BITS(w, 54, 1), blod = BITS(w, 55, 3);
    const char *x, *y = 0, *z = 0, *layer = 0;
    switch (type) {
    case TEX_1D: x = F(c, a); break;
    case TEX_ARRAY_1D: x = F(c, a + 1); layer = layer_of(c, a); break;
    case TEX_2D: x = F(c, a); y = F(c, a + 1); break;
    case TEX_ARRAY_2D: x = F(c, a + 1); y = F(c, a + 2); layer = layer_of(c, a); break;
    case TEX_3D: case TEX_CUBE: x = F(c, a); y = F(c, a + 1); z = F(c, a + 2); break;
    case TEX_ARRAY_CUBE: x = F(c, a + 1); y = F(c, a + 2); z = F(c, a + 3); layer = layer_of(c, a); break;
    default: unsupported(c, in, "TEX type"); return;
    }
    int lod = blod == 1 ? 1 : blod == 2 || blod == 6 ? 2 : blod == 3 || blod == 7 ? 3 : 0;
    const char *lodx = NULL, *dref = NULL, *offset = NULL;
    if (lod >= 2) lodx = F(c, m++);
    if (aoffi) {
        const char *o = R(c, m++);
        offset = tmp("int2(((int)(%s << 28)) >> 28, ((int)(%s << 24)) >> 28)", o, o);   /* 4-bit signed */
    }
    if (dc) dref = F(c, m++);
    u32 k = texture_slot(c, handle, type, dc);
    const char *coord = coord_of(type, x, y, z, layer);
    const char *v = tmp("t%u", tex_counter++);
    if (offset) {   /* Sample needs constant offsets: shift the coordinates instead */
        line(c, "{ float tw_, th_, tl_; tex%u.GetDimensions(0, tw_, th_, tl_);", k);
        coord = coord_of(type, tmp("(%s + float(%s.x) / tw_)", x, offset), y ? tmp("(%s + float(%s.y) / th_)", y, offset) : 0, z, layer);
        offset = NULL;
    } else line(c, "{");
    line(c, dref ? "  float4 %s = %s;" : "  float4 %s = swz(%s, %uu);", v, sample(c, k, type, coord, lod, lodx, dref, offset), k);
    if (dref) line(c, "  %s.w = 1.0f;", v);
    c->indent++;
    store_seq(c, v, mask, d);
    c->indent--;
    line(c, "}");
}

static const char *gather(Ctx *c, u32 k, u32 comp, const char *coord, const char *dref, const char *offset) {
    static const char *const ch[4] = {"Red", "Green", "Blue", "Alpha"};
    const char *off = offset ? tmp(", %s", offset) : "";
    if (dref) return tmp("tex%u.GatherCmp(smp%u, %s, %s%s)", k, k, coord, dref, off);
    return tmp("tex%u.Gather%s(smp%u, %s%s)", k, ch[comp & 3], k, coord, off);
}

static void emit_tld4(Ctx *c, const Insn *in, int scalar) {
    u64 w = in->w;
    const char *v = tmp("t%u", tex_counter++);
    if (scalar) {   /* TLD4S: 2D only */
        u32 da = BITS(w, 0, 8), a = BITS(w, 8, 8), b = BITS(w, 20, 8), db = BITS(w, 28, 8), handle = BITS(w, 36, 13) * 4;
        u32 dc = BITS(w, 50, 1), aoffi = BITS(w, 51, 1), comp = BITS(w, 52, 2), f16 = BITS(w, 55, 1);
        const char *coord, *dref = NULL, *offset = NULL;
        if (!aoffi && !dc) coord = coord_of(TEX_2D, F(c, a), F(c, b), 0, 0);
        else {
            coord = coord_of(TEX_2D, F(c, a), F(c, a + 1), 0, 0);
            if (aoffi) {
                const char *o = R(c, b);
                offset = tmp("int2(((int)(%s << 26)) >> 26, ((int)(%s << 18)) >> 26)", o, o);
                if (dc) dref = F(c, b + 1);
            } else dref = F(c, b);
        }
        u32 k = texture_slot(c, handle, TEX_2D, dc);
        line(c, "{ float4 %s = %s;", v, gather(c, k, comp, coord, dref, offset));
        c->indent++;
        store_split(c, v, 15, da, db, f16);
        c->indent--;
        line(c, "}");
        return;
    }
    u32 d = BITS(w, 0, 8), a = BITS(w, 8, 8), m = BITS(w, 20, 8), type = BITS(w, 28, 3), mask = BITS(w, 31, 4);
    u32 handle = BITS(w, 36, 13) * 4, dc = BITS(w, 50, 1), offmode = BITS(w, 54, 2), comp = BITS(w, 56, 2);
    const char *x, *y = 0, *z = 0, *layer = 0, *offset = NULL, *dref = NULL;
    switch (type) {
    case TEX_2D: x = F(c, a); y = F(c, a + 1); break;
    case TEX_ARRAY_2D: x = F(c, a + 1); y = F(c, a + 2); layer = layer_of(c, a); break;
    case TEX_CUBE: x = F(c, a); y = F(c, a + 1); z = F(c, a + 2); break;
    case TEX_ARRAY_CUBE: x = F(c, a + 1); y = F(c, a + 2); z = F(c, a + 3); layer = layer_of(c, a); break;
    default: unsupported(c, in, "TLD4 type"); return;
    }
    if (offmode == 1) {
        const char *o = R(c, m++);
        offset = tmp("int2(((int)(%s << 26)) >> 26, ((int)(%s << 18)) >> 26)", o, o);
    } else if (offmode == 2) {   /* a separate offset per texel: signed 6-bit x, y pairs in two registers */
        const char *o0 = R(c, m), *o1 = R(c, m + 1);
        m += 2;
        offset = tmp("int2(((int)(%s << 26)) >> 26, ((int)(%s << 18)) >> 26), int2(((int)(%s << 10)) >> 26, ((int)(%s << 2)) >> 26), "
                     "int2(((int)(%s << 26)) >> 26, ((int)(%s << 18)) >> 26), int2(((int)(%s << 10)) >> 26, ((int)(%s << 2)) >> 26)",
                     o0, o0, o0, o0, o1, o1, o1, o1);
    }
    if (dc) dref = F(c, m);
    u32 k = texture_slot(c, handle, type, dc);
    if (offmode == 2 && VKD3D_FORMS) {   /* four gathers, each with its own offset, one component of each (what the four-offset form returns) */
        const char *o0 = R(c, m - 2), *o1 = R(c, m - 1);   /* (the two offset registers read above) */
        static const char *const part[4][2] = {{"<< 26", "<< 18"}, {"<< 10", "<< 2"}, {"<< 26", "<< 18"}, {"<< 10", "<< 2"}};
        static const char *const xyzw[4] = {"x", "y", "z", "w"};
        line(c, "{ float4 %s;", v);
        for (u32 i = 0; i < 4; i++) {
            const char *o = i < 2 ? o0 : o1;
            const char *one = tmp("int2(((int)(%s %s)) >> 26, ((int)(%s %s)) >> 26)", o, part[i][0], o, part[i][1]);
            line(c, "  %s.%s = (%s).%s;", v, xyzw[i], gather(c, k, comp, coord_of(type, x, y, z, layer), dref, one), xyzw[i]);
        }
    } else
    line(c, "{ float4 %s = %s;", v, gather(c, k, comp, coord_of(type, x, y, z, layer), dref, offset));
    c->indent++;
    store_seq(c, v, mask, d);
    c->indent--;
    line(c, "}");
}

static void emit_tlds(Ctx *c, const Insn *in) {
    u64 w = in->w;
    u32 da = BITS(w, 0, 8), a = BITS(w, 8, 8), b = BITS(w, 20, 8), db = BITS(w, 28, 8);
    u32 handle = BITS(w, 36, 13) * 4, swz = BITS(w, 50, 3), enc = BITS(w, 53, 4);
    int f16 = !BITS(w, 59, 1);
    static const u8 RG[8] = {1, 2, 4, 8, 3, 9, 10, 12}, RGBA[8] = {7, 11, 13, 14, 15, 15, 15, 15};
    u32 mask = db == RZ ? RG[swz] : RGBA[swz];
    u32 type = TEX_2D;
    const char *load;
    const char *ia = tmp("asint(%s)", R(c, a));
    switch (enc) {
    case 0: type = TEX_1D; load = tmp("int2(%s, 0)", ia); break;
    case 1: type = TEX_1D; load = tmp("int2(%s, asint(%s))", ia, R(c, b)); break;
    case 2: load = tmp("int3(%s, asint(%s), 0)", ia, R(c, b)); break;
    case 4: load = tmp("int3(%s, asint(%s), 0)", ia, R(c, a + 1)); break;   /* offset ignored */
    case 5: load = tmp("int3(%s, asint(%s), asint(%s))", ia, R(c, a + 1), R(c, b)); break;
    case 7: type = TEX_3D; load = tmp("int4(%s, asint(%s), asint(%s), 0)", ia, R(c, a + 1), R(c, b)); break;
    case 8: type = TEX_ARRAY_2D; load = tmp("int4(asint(%s), asint(%s), (int)(%s & 0xFFFFu), 0)", R(c, b), R(c, b + 1), R(c, a)); break;
    case 12: load = tmp("int3(%s, asint(%s), asint(%s))", ia, R(c, a + 1), R(c, b)); break;
    default: unsupported(c, in, "TLDS encoding"); return;
    }
    u32 k = texture_slot(c, handle, type, 0);
    const char *v = tmp("t%u", tex_counter++);
    line(c, "{ float4 %s = swz(tex%u.Load(%s), %uu);", v, k, load, k);
    c->indent++;
    store_split(c, v, mask, da, db, f16);
    c->indent--;
    line(c, "}");
}

/* ---- attributes -------------------------------------------------------------------- */
static const char *const XYZW[4] = {"x", "y", "z", "w"};

/* A vertex shader input attribute (ALD), as uint. */
static const char *vs_input(Ctx *c, u32 addr) {
    if (addr >= 0x80 && addr < 0x280) {
        u32 n = (addr - 0x80) >> 4;
        c->info->inputs |= 1u << n;
        return tmp("va%u.%s", n, XYZW[(addr >> 2) & 3]);
    }
    if (addr == 0x2F8) return "(iid + params.base.y)";
    if (addr == 0x2FC) return "vid";
    return "0u";
}

static void vs_output(Ctx *c, u32 addr, const char *value) {
    if (addr >= 0x70 && addr < 0x80) line(c, "o.pos.%s = asfloat(%s);", XYZW[(addr >> 2) & 3], value);
    else if (addr >= 0x80 && addr < 0x280) {
        u32 n = (addr - 0x80) >> 4;
        c->out_attr_mask |= 1u << n;
        line(c, "o.a%u.%s = asfloat(%s);", n, XYZW[(addr >> 2) & 3], value);
    }
    /* point size, layer, viewport index, clip distances: not forwarded */
}

/* A pixel shader input (IPA), as float, before the IPA modifiers. */
static const char *ps_input(Ctx *c, u32 addr, int *perspective) {
    *perspective = 0;
    if (addr >= 0x80 && addr < 0x280) {
        u32 n = (addr - 0x80) >> 4;
        c->info->inputs |= 1u << n;
        u32 comp = (addr >> 2) & 3, mode = (c->prog[0x18 + n] >> (comp * 2)) & 3;
        *perspective = mode == 2;
        if (!(c->vs_outputs >> n & 1)) return "0.0f";
        return tmp("i.a%u.%s", n, XYZW[comp]);
    }
    switch (addr) {
    case 0x70: return "i.pos.x";
    case 0x74: return "(params.frag[0] != 0 ? params.frag[1] - i.pos.y : i.pos.y)";
    case 0x78: return "i.pos.z";
    case 0x7C: return "(1.0f / i.clipw)";
    case 0x3FC: return "asfloat(front ? 0xFFFFFFFFu : 0u)";
    case 0x2E0: case 0x2E4: return "0.0f";
    }
    return "0.0f";
}

static void emit_exit(Ctx *c) {
    if (c->stage == SHADER_COMPUTE) {
        line(c, "return;");
        return;
    }
    if (c->stage == SHADER_VERTEX) {
        line(c, "return finish(o);");
        return;
    }
    const u8 *h = c->prog;
    u32 target = *(const u32 *)(h + 0x48), flags = *(const u32 *)(h + 0x4C), reg = 0;
    line(c, "{ PSOut o = (PSOut)0;");
    for (u32 rt = 0; rt < 8; rt++) {
        u32 m = (target >> (rt * 4)) & 15;
        if (!m) continue;
        c->info->color_mask |= 1u << rt;
        for (u32 k = 0; k < 4; k++)
            if (m >> k & 1) line(c, "  o.c%u.%s = %s;", rt, XYZW[k], F(c, reg++));
    }
    if (flags & 2) { c->info->writes_depth = 1; line(c, "  o.depth = %s;", F(c, reg + 1)); }
    line(c, "  return o; }");
}

/* ---- control flow ------------------------------------------------------------------- */
static i32 branch_offset(u64 w) { i32 o = (i32)BITS(w, 20, 24); return o & 0x800000 ? o - 0x1000000 : o; }

/* Every fourth word, from the code's start after the 0x50-byte header, holds scheduling
 * controls for the next three instructions; a branch to one goes to the first of them. */
static u32 align_target(u32 t) { return (t & 31) == 0x10 ? t + 8 : t; }
static u32 branch_target(const Insn *in) { return align_target(in->addr + 8 + (u32)branch_offset(in->w)); }

/* A control instruction's flow test, as an expression; 0 when it can never pass. Tests on
 * the condition codes read the flags the CC forms of instructions set; the culling tests
 * (CSM/FCSM, fed by VOTE.VTG in vertex shaders) never pass, as nothing is culled early. */
static int flow_true(Ctx *c, const Insn *in, const char **cond) {
    u32 test = BITS(in->w, 0, 5);
    switch (test) {
    case 15: *cond = "true"; return 1;
    case 0: case 16: case 18: case 21: case 23: case 24: case 25: case 26: case 27: case 28: case 29:
        *cond = "false"; return 0;
    }
    c->uses_cc = 1;
    switch (test) {
    case 1: case 9: *cond = "cc_s"; break;                     /* LT */
    case 2: case 10: *cond = "cc_z"; break;                    /* EQ */
    case 3: case 11: *cond = "(cc_s || cc_z)"; break;          /* LE */
    case 4: case 12: *cond = "(!cc_s && !cc_z)"; break;        /* GT */
    case 5: case 13: *cond = "!cc_z"; break;                   /* NE */
    case 6: case 14: *cond = "!cc_s"; break;                   /* GE */
    case 17: *cond = "(cc_c == 0u)"; break;                    /* LO: carry clear */
    case 19: *cond = "(cc_c == 0u || cc_z)"; break;            /* LS */
    case 20: *cond = "(cc_c != 0u && !cc_z)"; break;           /* HI */
    case 22: *cond = "(cc_c != 0u)"; break;                    /* HS */
    case 30: *cond = "(cc_s || cc_z)"; break;                  /* RLE */
    case 31: *cond = "(!cc_s && !cc_z)"; break;                /* RGT */
    default: *cond = "false"; return 0;                        /* NUM, NaN */
    }
    return 1;
}

static void goto_pc(Ctx *c, u32 target) { line(c, "npc_ = 0x%xu; break;", target); }

/* ---- instructions ------------------------------------------------------------------- */
static const char *half(const char *x, u32 swizzle, int hi) {   /* one half of a packed pair, as float */
    switch (swizzle) {
    case 0: return tmp(hi ? "f16tof32(%s >> 16)" : "f16tof32(%s)", x);   /* H1_H0 */
    case 1: return tmp("asfloat(%s)", x);                                  /* F32 */
    case 2: return tmp("f16tof32(%s)", x);                                 /* H0_H0 */
    }
    return tmp("f16tof32(%s >> 16)", x);                                   /* H1_H1 */
}

static void merge_halves(Ctx *c, u32 d, const char *lo, const char *hi, u32 merge) {
    if (d == RZ) return;
    switch (merge) {
    case 0: set_u(c, d, tmp("f32tof16(%s) | (f32tof16(%s) << 16)", lo, hi)); break;
    case 1: set_f(c, d, lo); break;
    case 2: set_u(c, d, tmp("(%s & 0xFFFF0000u) | f32tof16(%s)", R(c, d), lo)); break;
    default: set_u(c, d, tmp("(%s & 0x0000FFFFu) | (f32tof16(%s) << 16)", R(c, d), hi)); break;
    }
}

/* Half-precision pair operations: kind 0 add, 1 mul, 2 fma. */
static void emit_half(Ctx *c, const Insn *in, int kind) {
    u64 w = in->w;
    int form = form_of(in->op);
    u32 d = BITS(w, 0, 8);
    const char *a = R(c, BITS(w, 8, 8)), *b, *cc = NULL;
    u32 swa = BITS(w, 47, 2), swb = 0, swc = 0, merge = BITS(w, 49, 2), sat = 0;
    int abs_a = 0, neg_a = 0, abs_b = 0, neg_b = 0, neg_c = 0;
    if (kind == 0) {   /* HADD2 */
        neg_a = BITS(w, 43, 1); abs_a = BITS(w, 44, 1);
        switch (form) {
        case FORM_REG: b = R(c, BITS(w, 20, 8)); sat = BITS(w, 32, 1); neg_b = BITS(w, 31, 1); abs_b = BITS(w, 30, 1); swb = BITS(w, 28, 2); break;
        case FORM_CBUF: b = cbuf(c, BITS(w, 34, 5), BITS(w, 20, 14) * 4); sat = BITS(w, 52, 1); abs_b = BITS(w, 54, 1); neg_b = BITS(w, 56, 1); swb = 1; break;
        case FORM_IMM:
            b = tmp("0x%08xu", BITS(w, 20, 9) << 6 | BITS(w, 29, 1) << 15 | BITS(w, 30, 9) << 22 | BITS(w, 56, 1) << 31);
            sat = BITS(w, 52, 1); break;
        default: b = tmp("0x%08xu", BITS(w, 20, 32)); sat = BITS(w, 52, 1); neg_a = BITS(w, 56, 1); abs_a = 0; swa = BITS(w, 53, 2); merge = 0; break;
        }
    } else if (kind == 1) {   /* HMUL2 */
        abs_a = BITS(w, 44, 1);
        switch (form) {
        case FORM_REG: b = R(c, BITS(w, 20, 8)); sat = BITS(w, 32, 1); neg_b = BITS(w, 31, 1); abs_b = BITS(w, 30, 1); swb = BITS(w, 28, 2); break;
        case FORM_CBUF: b = cbuf(c, BITS(w, 34, 5), BITS(w, 20, 14) * 4); sat = BITS(w, 52, 1); abs_b = BITS(w, 54, 1); neg_a = BITS(w, 43, 1); swb = 1; break;
        case FORM_IMM:
            b = tmp("0x%08xu", BITS(w, 20, 9) << 6 | BITS(w, 29, 1) << 15 | BITS(w, 30, 9) << 22 | BITS(w, 56, 1) << 31);
            sat = BITS(w, 52, 1); neg_a = BITS(w, 43, 1); break;
        default: b = tmp("0x%08xu", BITS(w, 20, 32)); sat = BITS(w, 52, 1); abs_a = 0; swa = BITS(w, 53, 2); merge = 0; break;
        }
    } else {   /* HFMA2 */
        switch (form) {
        case FORM_REG:
            b = R(c, BITS(w, 20, 8)); cc = R(c, BITS(w, 39, 8)); swb = BITS(w, 28, 2); neg_b = BITS(w, 31, 1);
            neg_c = BITS(w, 30, 1); sat = BITS(w, 32, 1); swc = BITS(w, 35, 2); break;
        case FORM_RC:
            b = R(c, BITS(w, 39, 8)); cc = cbuf(c, BITS(w, 34, 5), BITS(w, 20, 14) * 4); neg_c = BITS(w, 51, 1);
            sat = BITS(w, 52, 1); swb = BITS(w, 53, 2); neg_b = BITS(w, 56, 1); swc = 1; break;
        case FORM_CR:
            b = cbuf(c, BITS(w, 34, 5), BITS(w, 20, 14) * 4); cc = R(c, BITS(w, 39, 8)); neg_c = BITS(w, 51, 1);
            sat = BITS(w, 52, 1); swc = BITS(w, 53, 2); neg_b = BITS(w, 56, 1); swb = 1; break;
        case FORM_IMM:
            b = tmp("0x%08xu", BITS(w, 20, 9) << 6 | BITS(w, 29, 1) << 15 | BITS(w, 30, 9) << 22 | BITS(w, 56, 1) << 31);
            cc = R(c, BITS(w, 39, 8)); neg_c = BITS(w, 51, 1); sat = BITS(w, 52, 1); swc = BITS(w, 53, 2); break;
        default:
            b = tmp("0x%08xu", BITS(w, 20, 32)); cc = R(c, BITS(w, 0, 8)); neg_c = BITS(w, 52, 1); swa = BITS(w, 53, 2);
            merge = 0; break;
        }
    }
    const char *lo, *hi;
    const char *a0 = absneg(half(a, swa, 0), abs_a, neg_a), *a1 = absneg(half(a, swa, 1), abs_a, neg_a);
    const char *b0 = absneg(half(b, swb, 0), abs_b, neg_b), *b1 = absneg(half(b, swb, 1), abs_b, neg_b);
    if (kind == 0) { lo = tmp("(%s + %s)", a0, b0); hi = tmp("(%s + %s)", a1, b1); }
    else if (kind == 1) { lo = tmp("(%s * %s)", a0, b0); hi = tmp("(%s * %s)", a1, b1); }
    else {
        const char *c0 = absneg(half(cc, swc, 0), 0, neg_c), *c1 = absneg(half(cc, swc, 1), 0, neg_c);
        lo = tmp("mad(%s, %s, %s)", a0, b0, c0); hi = tmp("mad(%s, %s, %s)", a1, b1, c1);
    }
    if (sat) { lo = tmp("saturate(%s)", lo); hi = tmp("saturate(%s)", hi); }
    line(c, "{ float lo_ = %s, hi_ = %s;", lo, hi);
    c->indent++;
    merge_halves(c, d, "lo_", "hi_", merge);
    c->indent--;
    line(c, "}");
}

static void emit_xmad(Ctx *c, const Insn *in) {
    u64 w = in->w;
    int form = form_of(in->op);
    u32 d = BITS(w, 0, 8), sa = BITS(w, 48, 1), sb = BITS(w, 49, 1), ha = BITS(w, 53, 1);
    u32 hb = 0, psl = 0, mrg = 0, x = 0, mode = 0;
    const char *b, *cc;
    switch (form) {
    case FORM_REG: hb = BITS(w, 35, 1); psl = BITS(w, 36, 1); mrg = BITS(w, 37, 1); x = BITS(w, 38, 1); mode = BITS(w, 50, 3);
        b = R(c, BITS(w, 20, 8)); cc = R(c, BITS(w, 39, 8)); break;
    case FORM_RC: mode = BITS(w, 50, 2); hb = BITS(w, 52, 1); x = BITS(w, 54, 1);
        b = R(c, BITS(w, 39, 8)); cc = cbuf(c, BITS(w, 34, 5), BITS(w, 20, 14) * 4); break;
    case FORM_CR: mode = BITS(w, 50, 2); hb = BITS(w, 52, 1); x = BITS(w, 54, 1); psl = BITS(w, 55, 1); mrg = BITS(w, 56, 1);
        b = cbuf(c, BITS(w, 34, 5), BITS(w, 20, 14) * 4); cc = R(c, BITS(w, 39, 8)); break;
    default: psl = BITS(w, 36, 1); mrg = BITS(w, 37, 1); x = BITS(w, 38, 1); mode = BITS(w, 50, 3);
        b = tmp("0x%xu", BITS(w, 20, 16)); cc = R(c, BITS(w, 39, 8)); break;
    }
    const char *a = R(c, BITS(w, 8, 8));
    const char *oa = sa ? tmp("asuint(((int)(%s << %u)) >> 16)", a, ha ? 0 : 16) : tmp(ha ? "(%s >> 16)" : "(%s & 0xFFFFu)", a);
    const char *ob = sb ? tmp("asuint(((int)(%s << %u)) >> 16)", b, hb ? 0 : 16) : tmp(hb ? "(%s >> 16)" : "(%s & 0xFFFFu)", b);
    const char *oc;
    switch (mode) {
    case 1: oc = tmp("(%s & 0xFFFFu)", cc); break;
    case 2: oc = tmp("(%s >> 16)", cc); break;
    case 4: oc = tmp("(%s + (%s << 16))", cc, b); break;
    default: oc = cc; break;
    }
    const char *prod = tmp("(%s * %s)", oa, ob);
    if (psl) prod = tmp("(%s << 16)", prod);
    const char *sum = tmp("(%s + %s%s)", prod, oc, x ? " + cc_c" : "");
    if (x) c->uses_cc = 1;
    if (mrg) sum = tmp("((%s & 0xFFFFu) | (%s << 16))", sum, b);
    set_u(c, d, sum);
}

static void emit_insn(Ctx *c, const Insn *in) {
    u64 w = in->w;
    int form = form_of(in->op);
    u32 d = BITS(w, 0, 8), ra = BITS(w, 8, 8);
    switch (in->op) {
    /* -- floating point -- */
    case OP_FADD_reg: case OP_FADD_cbuf: case OP_FADD_imm: {
        const char *a = absneg(F(c, ra), BITS(w, 46, 1), BITS(w, 48, 1)), *b = absneg(b_f(c, w, form), BITS(w, 49, 1), BITS(w, 45, 1));
        const char *r = tmp("(%s + %s)", a, b);
        set_f(c, d, BITS(w, 50, 1) ? tmp("saturate(%s)", r) : r);
        break;
    }
    case OP_FADD32I: {
        const char *a = absneg(F(c, ra), BITS(w, 54, 1), BITS(w, 56, 1)), *b = absneg(b_f(c, w, FORM_32I), BITS(w, 57, 1), BITS(w, 53, 1));
        set_f(c, d, tmp("(%s + %s)", a, b));
        break;
    }
    case OP_FMUL_reg: case OP_FMUL_cbuf: case OP_FMUL_imm: case OP_FMUL32I: {
        int is32 = in->op == OP_FMUL32I;
        const char *a = F(c, ra), *b = absneg(b_f(c, w, form), 0, is32 ? 0 : BITS(w, 48, 1));
        u32 fmz = is32 ? BITS(w, 53, 2) : BITS(w, 44, 2), sat = is32 ? BITS(w, 55, 1) : BITS(w, 50, 1);
        static const char *const scale[8] = {"", " * 0.5f", " * 0.25f", " * 0.125f", " * 8.0f", " * 4.0f", " * 2.0f", ""};
        const char *sa = is32 ? "" : scale[BITS(w, 41, 3)];
        const char *r = tmp("((%s%s) * %s)", a, sa, b);
        if (fmz == 2 && !sat) r = tmp("((%s == 0.0f || %s == 0.0f) ? 0.0f : %s)", a, b, r);
        set_f(c, d, sat ? tmp("saturate(%s)", r) : r);
        break;
    }
    case OP_FFMA_reg: case OP_FFMA_rc: case OP_FFMA_cr: case OP_FFMA_imm: case OP_FFMA32I: {
        int is32 = in->op == OP_FFMA32I;
        const char *a = absneg(F(c, ra), 0, is32 ? BITS(w, 56, 1) : 0);
        const char *b = is32 ? b_f(c, w, FORM_32I) : absneg(b_f(c, w, form), 0, BITS(w, 48, 1));
        const char *cc = absneg(is32 ? F(c, d) : tmp("asfloat(%s)", c_u(c, w, form)), 0, is32 ? BITS(w, 57, 1) : BITS(w, 49, 1));
        u32 fmz = BITS(w, 53, 2), sat = is32 ? BITS(w, 55, 1) : BITS(w, 50, 1);
        const char *r = tmp("mad(%s, %s, %s)", a, b, cc);
        if (fmz == 2 && !sat) r = tmp("((%s == 0.0f || %s == 0.0f) ? %s : %s)", a, b, cc, r);
        set_f(c, d, sat ? tmp("saturate(%s)", r) : r);
        break;
    }
    case OP_MUFU: {
        const char *x = absneg(F(c, ra), BITS(w, 46, 1), BITS(w, 48, 1)), *r;
        switch (BITS(w, 20, 4)) {
        case 0: r = tmp("cos(%s)", x); break;
        case 1: r = tmp("sin(%s)", x); break;
        case 2: r = tmp("exp2(%s)", x); break;
        case 3: r = tmp("log2(%s)", x); break;
        case 4: r = tmp("(1.0f / %s)", x); break;
        case 5: r = tmp("rsqrt(%s)", x); break;
        case 8: r = tmp("sqrt(%s)", x); break;
        default: unsupported(c, in, "MUFU operation"); return;
        }
        set_f(c, d, BITS(w, 50, 1) ? tmp("saturate(%s)", r) : r);
        break;
    }
    case OP_RRO_reg: case OP_RRO_cbuf: case OP_RRO_imm:
        set_f(c, d, absneg(b_f(c, w, form), BITS(w, 49, 1), BITS(w, 45, 1)));
        break;
    case OP_FMNMX_reg: case OP_FMNMX_cbuf: case OP_FMNMX_imm: {
        const char *a = absneg(F(c, ra), BITS(w, 46, 1), BITS(w, 48, 1)), *b = absneg(b_f(c, w, form), BITS(w, 49, 1), BITS(w, 45, 1));
        const char *p = PRED(c, BITS(w, 39, 3), BITS(w, 42, 1));
        set_f(c, d, tmp("(%s ? min(%s, %s) : max(%s, %s))", p, a, b, a, b));
        break;
    }
    case OP_FSETP_reg: case OP_FSETP_cbuf: case OP_FSETP_imm: {
        const char *a = absneg(F(c, ra), BITS(w, 7, 1), BITS(w, 43, 1)), *b = absneg(b_f(c, w, form), BITS(w, 44, 1), BITS(w, 6, 1));
        const char *cmp = fcompare(BITS(w, 48, 4), a, b), *bp = PRED(c, BITS(w, 39, 3), BITS(w, 42, 1));
        u32 bop = BITS(w, 45, 2);
        line(c, "{ bool cmp_ = %s;", cmp);
        c->indent++;
        set_p(c, BITS(w, 3, 3), combine(bop, "cmp_", bp));
        set_p(c, BITS(w, 0, 3), combine(bop, "!cmp_", bp));
        c->indent--;
        line(c, "}");
        break;
    }
    case OP_FSET_reg: case OP_FSET_cbuf: case OP_FSET_imm: {
        const char *a = absneg(F(c, ra), BITS(w, 54, 1), BITS(w, 43, 1)), *b = absneg(b_f(c, w, form), BITS(w, 44, 1), BITS(w, 53, 1));
        const char *r = combine(BITS(w, 45, 2), fcompare(BITS(w, 48, 4), a, b), PRED(c, BITS(w, 39, 3), BITS(w, 42, 1)));
        set_u(c, d, tmp("(%s ? %s : 0u)", r, BITS(w, 52, 1) ? "0x3F800000u" : "0xFFFFFFFFu"));
        break;
    }
    case OP_CSET: case OP_CSETP: {   /* a condition code test, combined with a predicate */
        Insn t = *in;
        const char *cond;
        t.w = BITS(w, 8, 5);
        if (!flow_true(c, &t, &cond)) cond = "false";
        const char *pred = PRED(c, BITS(w, 39, 3), BITS(w, 42, 1));
        if (in->op == OP_CSET)
            set_u(c, d, tmp("(%s ? %s : 0u)", combine(BITS(w, 45, 2), cond, pred), BITS(w, 44, 1) ? "0x3F800000u" : "0xFFFFFFFFu"));
        else {
            line(c, "{ bool t_ = %s;", cond);
            set_p(c, BITS(w, 3, 3), combine(BITS(w, 45, 2), "t_", pred));
            set_p(c, BITS(w, 0, 3), combine(BITS(w, 45, 2), "!t_", pred));
            line(c, "}");
        }
        break;
    }
    case OP_HSET2_reg: case OP_HSET2_cbuf: case OP_HSET2_imm:
    case OP_HSETP2_reg: case OP_HSETP2_cbuf: case OP_HSETP2_imm: {   /* compare two half pairs */
        int setp = in->op == OP_HSETP2_reg || in->op == OP_HSETP2_cbuf || in->op == OP_HSETP2_imm;
        const char *b;
        u32 swb, abs_b = 0, neg_b = 0, cmp, flag;   /* flag: HSET2 BF (1.0 for true), HSETP2 H_AND */
        if (form == FORM_REG) {
            b = R(c, BITS(w, 20, 8)); swb = BITS(w, 28, 2); abs_b = BITS(w, 30, 1); neg_b = BITS(w, 31, 1);
            cmp = BITS(w, 35, 4); flag = BITS(w, 49, 1);
        } else if (form == FORM_CBUF) {
            b = cbuf(c, BITS(w, 34, 5), BITS(w, 20, 14) * 4); swb = 1; cmp = BITS(w, 49, 4); flag = BITS(w, 53, 1);
            neg_b = BITS(w, 56, 1); abs_b = setp ? BITS(w, 54, 1) : 0;
        } else {
            b = tmp("0x%08xu", BITS(w, 20, 9) << 6 | BITS(w, 29, 1) << 15 | BITS(w, 30, 9) << 22 | BITS(w, 56, 1) << 31);
            swb = 0; cmp = BITS(w, 49, 4); flag = BITS(w, 53, 1);
        }
        const char *a = R(c, ra);
        u32 swa = BITS(w, 47, 2), abs_a = BITS(w, 44, 1), neg_a = BITS(w, 43, 1);
        const char *pred = PRED(c, BITS(w, 39, 3), BITS(w, 42, 1));
        const char *lo = combine(BITS(w, 45, 2), fcompare(cmp, absneg(half(a, swa, 0), abs_a, neg_a), absneg(half(b, swb, 0), abs_b, neg_b)), pred);
        const char *hi = combine(BITS(w, 45, 2), fcompare(cmp, absneg(half(a, swa, 1), abs_a, neg_a), absneg(half(b, swb, 1), abs_b, neg_b)), pred);
        if (!setp) {
            const char *t = flag ? "0x3C00u" : "0xFFFFu";
            set_u(c, d, tmp("((%s ? %s : 0u) | ((%s ? %s : 0u) << 16))", lo, t, hi, t));
        } else {
            line(c, "{ bool lo_ = %s, hi_ = %s;", lo, hi);
            if (flag) { set_p(c, BITS(w, 3, 3), "(lo_ && hi_)"); set_p(c, BITS(w, 0, 3), "!(lo_ && hi_)"); }
            else { set_p(c, BITS(w, 3, 3), "lo_"); set_p(c, BITS(w, 0, 3), "hi_"); }
            line(c, "}");
        }
        break;
    }
    case OP_F2F_reg: case OP_F2F_cbuf: case OP_F2F_imm: {
        u32 dst = BITS(w, 8, 2), src = BITS(w, 10, 2), rop = BITS(w, 39, 4) & 0xB;
        const char *x = src == 1 ? half(b_u(c, w, form), 0, BITS(w, 41, 1)) : b_f(c, w, form);
        if (src == 1) x = tmp(BITS(w, 41, 1) ? "f16tof32(%s >> 16)" : "f16tof32(%s)", b_u(c, w, form));
        x = absneg(x, BITS(w, 49, 1), BITS(w, 45, 1));
        if (src == dst) {
            switch (rop) {
            case 8: x = tmp("round(%s)", x); break;
            case 9: x = tmp("floor(%s)", x); break;
            case 10: x = tmp("ceil(%s)", x); break;
            case 11: x = tmp("trunc(%s)", x); break;
            }
        }
        if (BITS(w, 50, 1)) x = tmp("saturate(%s)", x);
        if (dst == 1) set_u(c, d, tmp("f32tof16(%s)", x));
        else set_f(c, d, x);
        break;
    }
    case OP_F2I_reg: case OP_F2I_cbuf: case OP_F2I_imm: {
        u32 dst = BITS(w, 8, 2), src = BITS(w, 10, 2), is_signed = BITS(w, 12, 1), rnd = BITS(w, 39, 2);
        const char *x = src == 1 ? tmp(BITS(w, 41, 1) ? "f16tof32(%s >> 16)" : "f16tof32(%s)", b_u(c, w, form)) : b_f(c, w, form);
        x = absneg(x, BITS(w, 45, 1), BITS(w, 49, 1));
        static const char *const rf[4] = {"round", "floor", "ceil", "trunc"};
        x = tmp("%s(%s)", rf[rnd], x);
        if (dst == 1) x = is_signed ? tmp("clamp(%s, -32768.0f, 32767.0f)", x) : tmp("clamp(%s, 0.0f, 65535.0f)", x);
        set_u(c, d, is_signed ? tmp("asuint((int)%s)", x) : tmp("(uint)%s", x));
        break;
    }
    case OP_I2F_reg: case OP_I2F_cbuf: case OP_I2F_imm: {
        u32 ff = BITS(w, 8, 2), fmt = BITS(w, 10, 2), is_signed = BITS(w, 13, 1), sel = BITS(w, 41, 2);
        const char *x = b_u(c, w, form);
        if (fmt == 0) x = is_signed ? tmp("(((int)(%s << %u)) >> 24)", x, 24 - sel * 8) : tmp("((%s >> %u) & 0xFFu)", x, sel * 8);
        else if (fmt == 1) x = is_signed ? tmp("(((int)(%s << %u)) >> 16)", x, 16 - sel * 8) : tmp("((%s >> %u) & 0xFFFFu)", x, sel * 8);
        else if (is_signed) x = tmp("asint(%s)", x);
        if (BITS(w, 49, 1)) x = is_signed ? tmp("abs(%s)", x) : x;
        const char *f = tmp("(float)(%s)", x);
        if (BITS(w, 45, 1)) f = tmp("(-%s)", f);
        if (ff == 1) set_u(c, d, tmp("f32tof16(%s)", f));
        else set_f(c, d, f);
        break;
    }
    case OP_I2I_reg: case OP_I2I_cbuf: case OP_I2I_imm: {
        u32 dfmt = BITS(w, 8, 2), sfmt = BITS(w, 10, 2), dsig = BITS(w, 12, 1), ssig = BITS(w, 13, 1), sel = BITS(w, 41, 3);
        const char *x = b_u(c, w, form);
        u32 bits = sfmt == 0 ? 8 : sfmt == 1 ? 16 : 32;
        if (bits < 32) x = ssig ? tmp("asuint(((int)(%s << %u)) >> %u)", x, 32 - bits - sel * 8, 32 - bits) : tmp("((%s >> %u) & 0x%xu)", x, sel * 8, (1u << bits) - 1);
        if (BITS(w, 49, 1)) x = tmp("asuint(abs(asint(%s)))", x);
        if (BITS(w, 45, 1)) x = tmp("asuint(-asint(%s))", x);
        u32 dbits = dfmt == 0 ? 8 : dfmt == 1 ? 16 : 32;
        if (BITS(w, 50, 1) && dbits < 32)
            x = dsig ? tmp("asuint(clamp(asint(%s), %d, %d))", x, -(1 << (dbits - 1)), (1 << (dbits - 1)) - 1)
                     : tmp("(uint)clamp(asint(%s), 0, %d)", x, (1 << dbits) - 1);
        else if (dbits < 32) x = tmp("(%s & 0x%xu)", x, (1u << dbits) - 1);
        set_u(c, d, x);
        break;
    }
    /* -- half precision -- */
    case OP_HADD2_reg: case OP_HADD2_cbuf: case OP_HADD2_imm: case OP_HADD2_32I: emit_half(c, in, 0); break;
    case OP_HMUL2_reg: case OP_HMUL2_cbuf: case OP_HMUL2_imm: case OP_HMUL2_32I: emit_half(c, in, 1); break;
    case OP_HFMA2_reg: case OP_HFMA2_rc: case OP_HFMA2_cr: case OP_HFMA2_imm: case OP_HFMA2_32I: emit_half(c, in, 2); break;
    /* -- integer -- */
    case OP_IADD_reg: case OP_IADD_cbuf: case OP_IADD_imm: case OP_IADD32I: {
        int is32 = in->op == OP_IADD32I;
        u32 po = is32 ? BITS(w, 55, 2) == 3 : BITS(w, 48, 2) == 3;
        u32 neg_a = is32 ? (BITS(w, 56, 1) && !po) : BITS(w, 49, 1), neg_b = is32 ? 0 : (BITS(w, 48, 1) && !po);
        u32 x = is32 ? BITS(w, 53, 1) : BITS(w, 43, 1), cc = is32 ? BITS(w, 52, 1) : BITS(w, 47, 1);
        const char *a = R(c, ra), *b = b_u(c, w, form);
        if (neg_a) a = tmp("(0u - %s)", a);
        if (neg_b) b = tmp("(0u - %s)", b);
        if (x || cc) {
            c->uses_cc = 1;
            line(c, "{ uint a_ = %s, b_ = %s, s_ = a_ + b_, t_ = s_ + %s;", a, b, x ? "cc_c" : "0u");
            if (cc) line(c, "  cc_c = (s_ < a_ || t_ < s_) ? 1u : 0u; cc_z = t_ == 0u; cc_s = (int)t_ < 0;");
            line(c, "  %s }", d == RZ ? "" : tmp("%s = t_%s;", R(c, d), po ? " + 1u" : ""));
        } else set_u(c, d, tmp("(%s + %s%s)", a, b, po ? " + 1u" : ""));
        break;
    }
    case OP_ISCADD_reg: case OP_ISCADD_cbuf: case OP_ISCADD_imm: case OP_ISCADD32I: {
        int is32 = in->op == OP_ISCADD32I;
        u32 scale = is32 ? BITS(w, 53, 5) : BITS(w, 39, 5);
        u32 neg_b = is32 ? 0 : BITS(w, 48, 1), neg_a = is32 ? 0 : BITS(w, 49, 1), po = neg_a && neg_b;
        const char *a = R(c, ra), *b = b_u(c, w, form);
        if (po) b = tmp("(%s + 1u)", b);
        else {
            if (neg_a) a = tmp("(0u - %s)", a);
            if (neg_b) b = tmp("(0u - %s)", b);
        }
        set_u(c, d, tmp("((%s << %uu) + %s)", a, scale, b));
        break;
    }
    case OP_IMNMX_reg: case OP_IMNMX_cbuf: case OP_IMNMX_imm: {
        const char *a = R(c, ra), *b = b_u(c, w, form), *p = PRED(c, BITS(w, 39, 3), BITS(w, 42, 1));
        if (BITS(w, 48, 1)) set_u(c, d, tmp("asuint(%s ? min(asint(%s), asint(%s)) : max(asint(%s), asint(%s)))", p, a, b, a, b));
        else set_u(c, d, tmp("(%s ? min(%s, %s) : max(%s, %s))", p, a, b, a, b));
        break;
    }
    case OP_ISETP_reg: case OP_ISETP_cbuf: case OP_ISETP_imm: {
        const char *cmp = icompare(BITS(w, 49, 3), BITS(w, 48, 1), R(c, ra), b_u(c, w, form)), *bp = PRED(c, BITS(w, 39, 3), BITS(w, 42, 1));
        u32 bop = BITS(w, 45, 2);
        if (BITS(w, 43, 1)) line(c, "/* ISETP.X treated as a plain compare */");
        line(c, "{ bool cmp_ = %s;", cmp);
        c->indent++;
        set_p(c, BITS(w, 3, 3), combine(bop, "cmp_", bp));
        set_p(c, BITS(w, 0, 3), combine(bop, "!cmp_", bp));
        c->indent--;
        line(c, "}");
        break;
    }
    case OP_ISET_reg: case OP_ISET_cbuf: case OP_ISET_imm: {
        const char *r = combine(BITS(w, 45, 2), icompare(BITS(w, 49, 3), BITS(w, 48, 1), R(c, ra), b_u(c, w, form)),
                                PRED(c, BITS(w, 39, 3), BITS(w, 42, 1)));
        set_u(c, d, tmp("(%s ? %s : 0u)", r, BITS(w, 44, 1) ? "0x3F800000u" : "0xFFFFFFFFu"));
        break;
    }
    case OP_LOP_reg: case OP_LOP_cbuf: case OP_LOP_imm: case OP_LOP32I: {
        int is32 = in->op == OP_LOP32I;
        u32 op = is32 ? BITS(w, 53, 2) : BITS(w, 41, 2);
        const char *a = R(c, ra), *b = b_u(c, w, form);
        if (is32 ? BITS(w, 55, 1) : BITS(w, 39, 1)) a = tmp("(~%s)", a);
        if (is32 ? BITS(w, 56, 1) : BITS(w, 40, 1)) b = tmp("(~%s)", b);
        static const char *const ops[3] = {"&", "|", "^"};
        const char *r = op == 3 ? b : tmp("(%s %s %s)", a, ops[op], b);
        u32 pop = is32 ? 0 : BITS(w, 44, 2), dp = is32 ? PT : BITS(w, 48, 3);
        if (pop || dp != PT) {
            line(c, "{ uint l_ = %s;", r);
            set_u(c, d, "l_");
            set_p(c, dp, pop == 0 ? "false" : pop == 1 ? "true" : pop == 2 ? "(l_ == 0u)" : "(l_ != 0u)");
            line(c, "}");
        } else set_u(c, d, r);
        break;
    }
    case OP_LOP3_reg: case OP_LOP3_cbuf: case OP_LOP3_imm: {
        u32 lut = in->op == OP_LOP3_reg ? BITS(w, 28, 8) : BITS(w, 48, 8);
        set_u(c, d, tmp("lop3(%s, %s, %s, %uu)", R(c, ra), b_u(c, w, in->op == OP_LOP3_imm ? FORM_IMM : form), R(c, BITS(w, 39, 8)), lut));
        break;
    }
    case OP_SHL_reg: case OP_SHL_cbuf: case OP_SHL_imm: {
        const char *a = R(c, ra), *s = b_u(c, w, form);
        if (BITS(w, 39, 1)) set_u(c, d, tmp("(%s << (%s & 31u))", a, s));
        else set_u(c, d, tmp("(%s < 32u ? (%s << %s) : 0u)", s, a, s));
        break;
    }
    case OP_SHR_reg: case OP_SHR_cbuf: case OP_SHR_imm: {
        const char *a = R(c, ra), *s = b_u(c, w, form);
        if (BITS(w, 40, 1)) a = tmp("reversebits(%s)", a);
        int sg = BITS(w, 48, 1);
        const char *v = sg ? tmp("asuint(asint(%s) >> %s)", a, BITS(w, 39, 1) ? tmp("(%s & 31u)", s) : tmp("min(%s, 31u)", s))
                           : (BITS(w, 39, 1) ? tmp("(%s >> (%s & 31u))", a, s) : tmp("(%s < 32u ? (%s >> %s) : 0u)", s, a, s));
        set_u(c, d, v);
        break;
    }
    case OP_BFE_reg: case OP_BFE_cbuf: case OP_BFE_imm: {
        const char *base = R(c, ra), *b = b_u(c, w, form);
        if (BITS(w, 40, 1)) base = tmp("reversebits(%s)", base);
        line(c, "{ uint o_ = %s & 0xFFu, n_ = (%s >> 8) & 0xFFu;", b, b);
        if (BITS(w, 48, 1)) set_u(c, d, tmp("(n_ == 0u ? 0u : o_ >= 32u ? ((int)%s < 0 ? 0xFFFFFFFFu : 0u) : "
                                            "asuint(((int)(%s << (32u - min(o_ + n_, 32u)))) >> (32u - min(n_, 32u - o_) )))", base, base));
        else set_u(c, d, tmp("(n_ == 0u || o_ >= 32u ? 0u : (%s >> o_) & (n_ >= 32u ? 0xFFFFFFFFu : ((1u << n_) - 1u)))", base));
        line(c, "}");
        break;
    }
    case OP_BFI_reg: case OP_BFI_rc: case OP_BFI_cr: case OP_BFI_imm: {
        const char *ins = R(c, ra), *src, *base;
        switch (form) {
        case FORM_REG: src = R(c, BITS(w, 20, 8)); base = R(c, BITS(w, 39, 8)); break;
        case FORM_RC: src = R(c, BITS(w, 39, 8)); base = cbuf(c, BITS(w, 34, 5), BITS(w, 20, 14) * 4); break;
        case FORM_CR: src = cbuf(c, BITS(w, 34, 5), BITS(w, 20, 14) * 4); base = R(c, BITS(w, 39, 8)); break;
        default: src = tmp("0x%08xu", imm20(w)); base = R(c, BITS(w, 39, 8)); break;
        }
        line(c, "{ uint o_ = %s & 0xFFu, n_ = min((%s >> 8) & 0xFFu, 32u - min(o_, 32u));", src, src);
        set_u(c, d, tmp("(o_ >= 32u ? %s : ((%s & ~(((n_ >= 32u) ? 0xFFFFFFFFu : ((1u << n_) - 1u)) << o_)) | "
                        "((%s & ((n_ >= 32u) ? 0xFFFFFFFFu : ((1u << n_) - 1u))) << o_)))", base, base, ins));
        line(c, "}");
        break;
    }
    case OP_XMAD_reg: case OP_XMAD_rc: case OP_XMAD_cr: case OP_XMAD_imm: emit_xmad(c, in); break;
    case OP_MOV_reg: case OP_MOV_cbuf: case OP_MOV_imm: set_u(c, d, b_u(c, w, form)); break;
    case OP_MOV32I: set_u(c, d, b_u(c, w, FORM_32I)); break;
    case OP_SEL_reg: case OP_SEL_cbuf: case OP_SEL_imm:
        set_u(c, d, tmp("(%s ? %s : %s)", PRED(c, BITS(w, 39, 3), BITS(w, 42, 1)), R(c, ra), b_u(c, w, form)));
        break;
    case OP_R2P_reg: case OP_R2P_cbuf: case OP_R2P_imm: {   /* predicates from register bits, under a mask */
        if (BITS(w, 40, 1)) { unsupported(c, in, "R2P.CC"); break; }
        u32 sh = BITS(w, 41, 2) * 8;
        const char *mask = b_u(c, w, form), *src = R(c, ra);
        for (u32 p = 0; p < 7; p++)
            line(c, "if ((%s >> %uu) & 1u) %s = ((%s >> %uu) & 1u) != 0u;", mask, p + sh, PRED(c, p, 0), src, p + sh);
        break;
    }
    case OP_IADD3_reg: case OP_IADD3_cbuf: case OP_IADD3_imm: {
        const char *a = R(c, ra), *b = b_u(c, w, form), *cc = R(c, BITS(w, 39, 8));
        if (form == FORM_REG) {   /* halves, and a 16-bit shift of the partial sum */
            static const char *const hv[3] = {"%s", "(%s & 0xFFFFu)", "(%s >> 16)"};
            u32 ha = BITS(w, 35, 2), hb = BITS(w, 33, 2), hc = BITS(w, 31, 2);
            if (ha && ha < 3) a = tmp(hv[ha], a);
            if (hb && hb < 3) b = tmp(hv[hb], b);
            if (hc && hc < 3) cc = tmp(hv[hc], cc);
        }
        if (BITS(w, 51, 1)) a = tmp("(0u - %s)", a);
        if (BITS(w, 50, 1)) b = tmp("(0u - %s)", b);
        if (BITS(w, 49, 1)) cc = tmp("(0u - %s)", cc);
        const char *ab = tmp("(%s + %s)", a, b);
        u32 shift = form == FORM_REG ? BITS(w, 37, 2) : 0;
        if (shift == 1) ab = tmp("(%s >> 16)", ab);
        else if (shift == 2) ab = tmp("(%s << 16)", ab);
        set_u(c, d, tmp("(%s + %s)", ab, cc));
        break;
    }
    case OP_TMML: {   /* level of detail, computed and unclamped, as 8.8 fixed point */
        u32 a = BITS(w, 8, 8), type = BITS(w, 28, 3), mask = BITS(w, 31, 4), handle = BITS(w, 36, 13) * 4;
        const char *coord;
        switch (type) {
        case TEX_1D: coord = F(c, a); break;
        case TEX_2D: coord = coord_of(TEX_2D, F(c, a), F(c, a + 1), 0, 0); break;
        case TEX_ARRAY_2D: coord = tmp("float2(%s, %s)", F(c, a + 1), F(c, a + 2)); break;
        case TEX_3D: case TEX_CUBE: coord = tmp("float3(%s, %s, %s)", F(c, a), F(c, a + 1), F(c, a + 2)); break;
        default: unsupported(c, in, "TMML type"); return;
        }
        u32 k = texture_slot(c, handle, type, 0);
        const char *v = tmp("t%u", tex_counter++);
        if (gradients(c) && VKD3D_FORMS) {
            /* the level from the texel-space derivatives of the coordinates, as for isotropic
             * filtering (cube maps: on the face the direction points at), clamped to the mips */
            switch (type) {
            case TEX_1D:
                line(c, "{ float lw_, ll_; tex%u.GetDimensions(0, lw_, ll_); float lc_ = (%s) * lw_; float lr_ = max(ddx(lc_) * ddx(lc_), ddy(lc_) * ddy(lc_));", k, coord);
                break;
            case TEX_2D:
                line(c, "{ float lw_, lh_, ll_; tex%u.GetDimensions(0, lw_, lh_, ll_); float2 lc_ = (%s) * float2(lw_, lh_);", k, coord);
                line(c, "  float2 lx_ = ddx(lc_), ly_ = ddy(lc_); float lr_ = max(dot(lx_, lx_), dot(ly_, ly_));");
                break;
            case TEX_ARRAY_2D:
                line(c, "{ float lw_, lh_, le_, ll_; tex%u.GetDimensions(0, lw_, lh_, le_, ll_); float2 lc_ = (%s) * float2(lw_, lh_);", k, coord);
                line(c, "  float2 lx_ = ddx(lc_), ly_ = ddy(lc_); float lr_ = max(dot(lx_, lx_), dot(ly_, ly_));");
                break;
            case TEX_3D:
                line(c, "{ float lw_, lh_, ld_, ll_; tex%u.GetDimensions(0, lw_, lh_, ld_, ll_); float3 lc_ = (%s) * float3(lw_, lh_, ld_);", k, coord);
                line(c, "  float3 lx_ = ddx(lc_), ly_ = ddy(lc_); float lr_ = max(dot(lx_, lx_), dot(ly_, ly_));");
                break;
            default:   /* TEX_CUBE */
                line(c, "{ float lw_, lh_, ll_; tex%u.GetDimensions(0, lw_, lh_, ll_); float3 ld_ = (%s);", k, coord);
                line(c, "  float3 lc_ = ld_ / max(max(abs(ld_.x), abs(ld_.y)), max(abs(ld_.z), 1e-20f)) * (0.5f * lw_);");
                line(c, "  float3 lx_ = ddx(lc_), ly_ = ddy(lc_); float lr_ = max(dot(lx_, lx_), dot(ly_, ly_));");
                break;
            }
            line(c, "  float lu_ = 0.5f * log2(max(lr_, 1e-20f));");
            line(c, "  float4 %s = float4(clamp(lu_, 0.0f, max(ll_ - 1.0f, 0.0f)), lu_, 0, 0);", v);
        } else if (gradients(c))
            line(c, "{ float4 %s = float4(tex%u.CalculateLevelOfDetail(smp%u, %s), tex%u.CalculateLevelOfDetailUnclamped(smp%u, %s), 0, 0);",
                 v, k, k, coord, k, k, coord);
        else line(c, "{ float4 %s = 0;", v);
        c->indent++;
        u32 dst = d;
        for (u32 i = 0; i < 4; i++)
            if (mask >> i & 1) { set_u(c, dst, tmp("(uint)(%s.%s * 256.0f)", v, XYZW[i])); if (dst != RZ) dst++; }
        c->indent--;
        line(c, "}");
        break;
    }
    case OP_PSET: {   /* predicates combined into a register */
        const char *pa = PRED(c, BITS(w, 12, 3), BITS(w, 15, 1)), *pb = PRED(c, BITS(w, 29, 3), BITS(w, 32, 1));
        const char *pc_ = PRED(c, BITS(w, 39, 3), BITS(w, 42, 1));
        const char *r = combine(BITS(w, 45, 2), tmp("(%s)", combine(BITS(w, 24, 2), pa, pb)), pc_);
        set_u(c, d, tmp("(%s ? %s : 0u)", r, BITS(w, 44, 1) ? "0x3F800000u" : "0xFFFFFFFFu"));
        break;
    }
    case OP_PSETP: {
        const char *pa = PRED(c, BITS(w, 12, 3), BITS(w, 15, 1)), *pb = PRED(c, BITS(w, 29, 3), BITS(w, 32, 1));
        const char *pc_ = PRED(c, BITS(w, 39, 3), BITS(w, 42, 1));
        line(c, "{ bool l_ = %s;", combine(BITS(w, 24, 2), pa, pb));
        set_p(c, BITS(w, 3, 3), combine(BITS(w, 45, 2), "l_", pc_));
        set_p(c, BITS(w, 0, 3), combine(BITS(w, 45, 2), "!l_", pc_));
        line(c, "}");
        break;
    }
    /* -- memory and attributes -- */
    case OP_LDC: {
        u32 idx = BITS(w, 36, 5), size = BITS(w, 48, 3), mode = BITS(w, 44, 2);
        i32 off = (i32)(BITS(w, 20, 16) << 16) >> 16;
        if (mode != 0) { unsupported(c, in, "LDC addressing mode"); break; }
        c->info->cbuf_mask |= 1u << idx;
        if (ra != RZ) {   /* indexed: all of the buffer may be read - unless this is a jump table's load (found first) */
            u32 end = ~0u;
            for (u32 t = 0; t < c->info->ntables; t++)
                if (c->info->table[t].bank == idx && (i32)c->info->table[t].offset == off) end = (u32)off + 4 * c->info->table[t].count;
            if (end == ~0u) c->info->cbuf_extent[idx] = ~0u;
            else if (c->info->cbuf_extent[idx] < end) c->info->cbuf_extent[idx] = end;
        } else if (off >= 0 && c->info->cbuf_extent[idx] < ((u32)off & ~7u) + 8) c->info->cbuf_extent[idx] = ((u32)off & ~7u) + 8;
        line(c, "{ uint addr_ = %s + %du;", R(c, ra), off);
        if (size == 4) set_u(c, d, tmp("cb%u.Load(addr_ & ~3u)", idx));
        else if (size == 5) { set_u(c, d, tmp("cb%u.Load(addr_ & ~7u)", idx)); set_u(c, d == RZ ? RZ : d + 1, tmp("cb%u.Load((addr_ & ~7u) + 4u)", idx)); }
        else {
            u32 bits = size < 2 ? 8 : 16, sg = size & 1;
            const char *word = tmp("(cb%u.Load(addr_ & ~3u) >> ((addr_ & 3u) * 8u))", idx);
            set_u(c, d, sg ? tmp("asuint(((int)(%s << %u)) >> %u)", word, 32 - bits, 32 - bits) : tmp("(%s & 0x%xu)", word, (1u << bits) - 1));
        }
        line(c, "}");
        break;
    }
    case OP_STG: {   /* global memory, compute only: found in one of the storage buffers (gstore_) */
        if (c->stage != SHADER_COMPUTE) { unsupported(c, in, "STG outside a compute shader"); break; }
        u32 e = BITS(w, 45, 1), size = BITS(w, 48, 3);
        i32 off = (i32)(BITS(w, 20, 24) << 8) >> 8;
        c->uses_gmem = 1;
        c->info->stores = 1;
        line(c, "{ uint b_ = %s, lo_ = b_ + 0x%08xu, hi_ = %s;", R(c, ra), (u32)off, e && ra != RZ ? R(c, ra + 1) : "0u");
        line(c, off >= 0 ? "  hi_ += lo_ < b_ ? 1u : 0u;" : "  hi_ -= lo_ > b_ ? 1u : 0u;");
        if (size < 4) {
            u32 bits = size < 2 ? 8 : 16;
            line(c, "  gstore_part_(lo_, hi_, %s, %uu);", R(c, d), bits);
        } else {
            u32 words = size == 4 ? 1 : size == 5 ? 2 : 4;
            for (u32 i = 0; i < words; i++)
                line(c, "  gstore_(lo_ + %uu, hi_ + (lo_ + %uu < lo_ ? 1u : 0u), %s);", 4 * i, 4 * i, d == RZ ? "0u" : R(c, d + i));
        }
        line(c, "}");
        break;
    }
    case OP_LDS: case OP_STS: case OP_LDL: case OP_STL: {   /* shared or local memory, as words */
        int local = in->op == OP_LDL || in->op == OP_STL, load = in->op == OP_LDS || in->op == OP_LDL;
        if (!local && c->stage != SHADER_COMPUTE) { unsupported(c, in, "shared memory outside a compute shader"); break; }
        u32 size = BITS(w, 48, 3);
        i32 off = (i32)(BITS(w, 20, 24) << 8) >> 8;
        const char *arr = local ? "lmem_" : "smem_", *words_ = local ? "LMEM_WORDS" : "SMEM_WORDS";
        if (local) c->uses_lmem = 1; else c->uses_smem = 1;
        line(c, "{ uint a_ = %s + 0x%08xu;", R(c, ra), (u32)off);
        if (size < 4) {
            u32 bits = size < 2 ? 8 : 16, sg = size & 1, mask = (1u << bits) - 1;
            if (load) {
                const char *word = tmp("(%s[(a_ >> 2) %% %s] >> ((a_ & 3u) * 8u))", arr, words_);
                set_u(c, d, sg ? tmp("asuint(((int)(%s << %u)) >> %u)", word, 32 - bits, 32 - bits) : tmp("(%s & 0x%xu)", word, mask));
            } else
                line(c, "  { uint i_ = (a_ >> 2) %% %s, s_ = (a_ & 3u) * 8u; %s[i_] = (%s[i_] & ~(0x%xu << s_)) | ((%s & 0x%xu) << s_); }",
                     words_, arr, arr, mask, R(c, d), mask);
        } else {
            u32 words = size == 4 ? 1 : size == 5 ? 2 : 4;
            for (u32 i = 0; i < words && d + i < RZ; i++) {
                if (load) set_u(c, d + i, tmp("%s[((a_ >> 2) + %uu) %% %s]", arr, i, words_));
                else line(c, "  %s[((a_ >> 2) + %uu) %% %s] = %s;", arr, i, words_, d == RZ ? "0u" : R(c, d + i));
            }
        }
        line(c, "}");
        break;
    }
    case OP_LDG: {   /* global memory, found in one of the storage buffers (gload_) */
        u32 e = BITS(w, 45, 1), size = BITS(w, 48, 3);
        i32 off = (i32)(BITS(w, 20, 24) << 8) >> 8;
        c->uses_gmem = 1;
        line(c, "{ uint b_ = %s, lo_ = b_ + 0x%08xu, hi_ = %s;", R(c, ra), (u32)off, e && ra != RZ ? R(c, ra + 1) : "0u");
        line(c, off >= 0 ? "  hi_ += lo_ < b_ ? 1u : 0u;" : "  hi_ -= lo_ > b_ ? 1u : 0u;");
        if (size < 4) {
            u32 bits = size < 2 ? 8 : 16, sg = size & 1;
            const char *word = "(gload_(lo_ & ~3u, hi_) >> ((lo_ & 3u) * 8u))";
            set_u(c, d, sg ? tmp("asuint(((int)(%s << %u)) >> %u)", word, 32 - bits, 32 - bits) : tmp("(%s & 0x%xu)", word, (1u << bits) - 1));
        } else {
            u32 words = size == 4 ? 1 : size == 5 ? 2 : 4;
            for (u32 i = 0; i < words && d != RZ && d + i < RZ; i++)
                set_u(c, d + i, i ? tmp("gload_(lo_ + %uu, hi_ + (lo_ + %uu < lo_ ? 1u : 0u))", 4 * i, 4 * i) : "gload_(lo_, hi_)");
        }
        line(c, "}");
        break;
    }
    case OP_ALD: {
        if (c->stage != SHADER_VERTEX) { unsupported(c, in, "ALD outside a vertex shader"); break; }
        if (BITS(w, 8, 8) != RZ) { unsupported(c, in, "indexed ALD"); break; }
        u32 off = BITS(w, 20, 10), n = BITS(w, 47, 2) + 1;
        for (u32 e = 0; e < n; e++) set_u(c, d == RZ ? RZ : d + e, vs_input(c, off + 4 * e));
        break;
    }
    case OP_AST: {
        if (c->stage != SHADER_VERTEX) { unsupported(c, in, "AST outside a vertex shader"); break; }
        u32 off = BITS(w, 20, 10), n = BITS(w, 47, 2) + 1;
        for (u32 e = 0; e < n; e++) vs_output(c, off + 4 * e, R(c, d == RZ ? RZ : d + e));
        break;
    }
    case OP_IPA: {
        if (c->stage != SHADER_PIXEL) { unsupported(c, in, "IPA outside a pixel shader"); break; }
        u32 attr = BITS(w, 30, 8) * 4, mode = BITS(w, 54, 2);
        int persp;
        const char *v = ps_input(c, attr, &persp);
        if (persp) v = tmp("(%s * (1.0f / i.clipw))", v);
        if (mode == 1) v = tmp("(%s * %s)", v, F(c, BITS(w, 20, 8)));
        if (BITS(w, 51, 1)) v = tmp("saturate(%s)", v);
        if (attr == 0x3FC) set_u(c, d, "(front ? 0xFFFFFFFFu : 0u)");
        else set_f(c, d, v);
        break;
    }
    case OP_S2R: {
        u32 sr = BITS(w, 20, 8);
        switch (sr) {
        case 0: set_u(c, d, c->stage == SHADER_COMPUTE ? "(tid_.x & 31u)" : "0u"); break;   /* lane */
        case 0x20: set_u(c, d, "(tid_.x | (tid_.y << 16) | (tid_.z << 26))"); break;  /* thread id, packed */
        case 0x21: case 0x22: case 0x23: set_u(c, d, tmp("tid_.%s", XYZW[sr - 0x21])); break;
        case 0x25: case 0x26: case 0x27: set_u(c, d, tmp("cta_.%s", XYZW[sr - 0x25])); break;
        case 0x28: set_u(c, d, tmp("0x%xu", c->local[0] | c->local[1] << 16 | c->local[2] << 26)); break;
        case 18: set_u(c, d, "0x3F800000u"); break;                                /* y direction: +1 */
        case 19: set_u(c, d, "0u"); break;                                         /* thread kill */
        case 17: case 29: set_u(c, d, "0u"); break;                                /* invocation id/info */
        case 30: case 31: set_u(c, d, "0x3F800000u"); break;                       /* w scale factors: 1.0 */
        default: set_u(c, d, "0u"); line(c, "/* S2R SR%u reads 0 */", sr); break;
        }
        break;
    }
    case OP_KIL: {
        const char *cond;
        if (flow_true(c, in, &cond)) {
            c->info->kills = 1;
            if (!strcmp(cond, "true")) line(c, "discard;");
            else line(c, "if (%s) discard;", cond);
        }
        break;
    }
    case OP_TEXS: emit_texs(c, in); break;
    case OP_TEX: emit_tex(c, in); break;
    case OP_TLD4: emit_tld4(c, in, 0); break;
    case OP_TLD4S: emit_tld4(c, in, 1); break;
    case OP_TLDS: emit_tlds(c, in); break;
    case OP_DEPBAR: case OP_NOP: case OP_BAR: case OP_MEMBAR: case OP_VOTE_vtg: break;
    case OP_VOTE: set_u(c, d, "0xFFFFFFFFu"); break;
    default: unsupported(c, in, "instruction"); break;
    }
}

/* ---- whole programs ------------------------------------------------------------------- */
static int find(Ctx *c, u32 addr) {
    for (u32 i = 0; i < c->n; i++) if (c->ins[i].addr == addr) return (int)i;
    return -1;
}

static int is_branchy(u32 op) {
    return op == OP_BRA || op == OP_BRX || op == OP_SSY || op == OP_PBK || op == OP_SYNC || op == OP_BRK;
}

/* Decode up to the end of the program: the 'BRA to self' padding, or an unconditional EXIT
 * that no branch target lies beyond. */
static void decode_program(Ctx *c, const u8 *prog, u32 size) {
    u32 cap = 256, furthest = 0;
    c->ins = malloc(sizeof *c->ins * cap);
    for (u32 a = 0x50; a + 8 <= size; a += 8) {
        if (((a - 0x50) / 8) % 4 == 0) continue;   /* scheduling control word */
        u64 w = *(const u64 *)(prog + a);
        if (w == 0xE2400FFFFF87000Full) break;
        if (c->n == cap) c->ins = realloc(c->ins, sizeof *c->ins * (cap *= 2));
        Insn in = {a, w, decode(w)};
        c->ins[c->n++] = in;
        if (in.op == OP_BRA || in.op == OP_SSY || in.op == OP_PBK) {
            u32 t = branch_target(&in);
            if (t > furthest) furthest = t;
        }
        if (in.op == OP_BRX) furthest = size;
        if (in.op == OP_EXIT && BITS(w, 16, 4) == 7 && BITS(w, 0, 5) == 15 && a >= furthest) break;
    }
}

static void emit_guarded(Ctx *c, const Insn *in) {
    u32 p = BITS(in->w, 16, 3), neg = BITS(in->w, 19, 1);
    line(c, "// %04x: %016llx %s%s%u %s", in->addr, in->w, p == PT && !neg ? "" : "@", neg ? "!P" : "P", p, OPS[in->op].name);
    if (p == PT && neg) return;   /* never executes */
    int guarded = p != PT;
    if (guarded) { line(c, "if (%s) {", PRED(c, p, neg)); c->indent++; }
    emit_insn(c, in);
    if (guarded) { c->indent--; line(c, "}"); }
}

/* Emit one control-flow instruction in dispatch mode; returns 1 if it ends the block. */
static int emit_flow(Ctx *c, const Insn *in, u32 next) {
    u32 p = BITS(in->w, 16, 3), neg = BITS(in->w, 19, 1);
    line(c, "// %04x: %016llx %s%s%u %s (test %u)", in->addr, in->w, p == PT && !neg ? "" : "@", neg ? "!P" : "P", p,
         OPS[in->op].name, BITS(in->w, 0, 5));
    if (p == PT && neg) return 0;
    const char *cond;
    int taken = flow_true(c, in, &cond);
    const char *guard = p == PT ? NULL : PRED(c, p, neg);
    if (strcmp(cond, "true")) guard = guard ? tmp("(%s && %s)", guard, cond) : cond;   /* predicate and flow test */
    if (!next) next = 0xFFFFFFFFu;
    switch (in->op) {
    case OP_SSY: line(c, "ssy_[ssp_ & 15u] = 0x%xu; ssp_ = ssp_ + 1u;", branch_target(in)); return 0;
    case OP_PBK: line(c, "pbk_[psp_ & 15u] = 0x%xu; psp_ = psp_ + 1u;", branch_target(in)); return 0;
    case OP_BRA:
        if (!taken) return 0;
        if (!guard) { goto_pc(c, branch_target(in)); return 1; }
        line(c, "npc_ = %s ? 0x%xu : 0x%xu; break;", guard, branch_target(in), next);   /* (fxc mishandles break in an if) */
        return 1;
    case OP_SYNC: case OP_BRK: {
        if (!taken) return 0;
        const char *pop = in->op == OP_SYNC ? "ssp_ = ssp_ - 1u; npc_ = ssy_[ssp_ & 15u];" : "psp_ = psp_ - 1u; npc_ = pbk_[psp_ & 15u];";
        if (!guard) line(c, "%s break;", pop);
        else {   /* as selects: fxc fails internally on assignments under the condition */
            const char *sp = in->op == OP_SYNC ? "ssp_" : "psp_", *stack = in->op == OP_SYNC ? "ssy_" : "pbk_";
            line(c, "{ bool g_ = %s; uint t_ = %s - 1u; uint to_ = %s[t_ & 15u]; %s = g_ ? t_ : %s; npc_ = g_ ? to_ : 0x%xu; } break;",
                 guard, sp, stack, sp, sp, next);
        }
        return 1;
    }
    case OP_BRX:
        line(c, "{ uint t_ = %s + 0x%xu; npc_ = (t_ & 31u) == 16u ? t_ + 8u : t_; } break;", R(c, BITS(in->w, 8, 8)),
             in->addr + 8 + (u32)branch_offset(in->w));
        return 1;
    case OP_EXIT:
        if (!taken) return 0;
        if (!guard) { emit_exit(c); return 1; }
        line(c, "if (%s) {", guard);
        c->indent++;
        emit_exit(c);
        c->indent--;
        line(c, "}");
        return 0;
    }
    return 0;
}


/* Jump tables of indirect branches. The compiler emits
 *     IMNMX.U32 rI, rI, N-1 ;  SHL rI, rI, 2 ;  LDC rT, c[bank][rI + off] ;  BRX rT - k
 * with the table's entries relative to the BRX. If every BRX of the program resolves, only
 * their targets need to be blocks; otherwise every instruction must be one. */
static int brx_resolved(Ctx *c) {
    if (!c->read_cbuf) return 0;
    int all = 1;
    for (u32 i = 0; i < c->n; i++) {
        if (c->ins[i].op != OP_BRX) continue;
        u32 r = BITS(c->ins[i].w, 8, 8), bank = 0, off = 0, index_reg = RZ, count = 0;
        int j = (int)i - 1, found = 0;
        for (; j >= 0 && j > (int)i - 24; j--)
            if (c->ins[j].op == OP_LDC && BITS(c->ins[j].w, 0, 8) == r) {
                bank = BITS(c->ins[j].w, 36, 5);
                off = (u32)((i32)(BITS(c->ins[j].w, 20, 16) << 16) >> 16);
                index_reg = BITS(c->ins[j].w, 8, 8);
                found = 1;
                break;
            }
        for (int k = j - 1; found && k >= 0 && k > j - 24; k--)
            if ((c->ins[k].op == OP_IMNMX_imm) && BITS(c->ins[k].w, 0, 8) == index_reg) { count = imm20(c->ins[k].w) + 1; break; }
        if (!found || !count || count > 256) { all = 0; continue; }
        if (c->info->ntables < 4) {
            c->info->table[c->info->ntables].bank = bank; c->info->table[c->info->ntables].offset = off;
            c->info->table[c->info->ntables++].count = count;
        }
        for (u32 e = 0; e < count; e++) {
            u32 t = align_target(c->read_cbuf(c->read_user, bank, off + 4 * e) + c->ins[i].addr + 8 + (u32)branch_offset(c->ins[i].w));
            int at = find(c, t);
            if (at < 0) { all = 0; break; }
            c->leader[at] = 1;
        }
    }
    return all;
}

static int brx_checked, brx_ok;
static int brx_targets(Ctx *c, u32 i) {
    (void)i;
    if (!brx_checked) { brx_checked = 1; brx_ok = brx_resolved(c); }
    return brx_ok;
}

static void emit_body(Ctx *c) {
    int branchy = 0, brx = 0;
    for (u32 i = 0; i < c->n; i++) { branchy |= is_branchy(c->ins[i].op); brx |= c->ins[i].op == OP_BRX; }
    c->dispatch = branchy;
    if (!branchy) {
        for (u32 i = 0; i < c->n; i++) {
            const Insn *in = &c->ins[i];
            if (in->op == OP_EXIT) emit_flow(c, in, 0);
            else emit_guarded(c, in);
        }
        emit_exit(c);
        return;
    }
    /* blocks */
    c->leader = calloc(c->n + 1, 1);
    u8 *ssy_target = calloc(c->n + 1, 1), *pbk_target = calloc(c->n + 1, 1);
    c->leader[0] = 1;
    brx_checked = 0;
    for (u32 i = 0; i < c->n; i++) {
        const Insn *in = &c->ins[i];
        if (brx && !brx_targets(c, i)) c->leader[i] = 1;
        if (in->op == OP_BRA || in->op == OP_SSY || in->op == OP_PBK) {
            int t = find(c, branch_target(in));
            if (t >= 0) {
                c->leader[t] = 1;
                if (in->op == OP_SSY) ssy_target[t] = 1;
                if (in->op == OP_PBK) pbk_target[t] = 1;
            }
        }
        if ((in->op == OP_BRA || in->op == OP_BRX || in->op == OP_SYNC || in->op == OP_BRK || in->op == OP_EXIT) && i + 1 < c->n)
            c->leader[i + 1] = 1;
    }
    line(c, "uint pc_ = 0x%xu; uint ssy_[16], pbk_[16]; uint ssp_ = 0u, psp_ = 0u;", c->ins[0].addr);
    line(c, "[loop] for (uint guard_ = 0u; guard_ < 1000000u; guard_++) {");
    c->indent++;
    line(c, "uint npc_ = pc_;");
    line(c, "switch (pc_) {");
    int open = 0, ended = 0;
    for (u32 i = 0; i < c->n; i++) {
        const Insn *in = &c->ins[i];
        if (c->leader[i]) {
            if (open) { if (!ended) goto_pc(c, in->addr); c->indent--; line(c, "}"); }
            line(c, "case 0x%xu: {", in->addr);
            c->indent++;
            open = 1; ended = 0;
            if (ssy_target[i]) line(c, "if (ssp_ > 0u && ssy_[(ssp_ - 1u) & 15u] == 0x%xu) ssp_ = ssp_ - 1u;", in->addr);
            if (pbk_target[i]) line(c, "if (psp_ > 0u && pbk_[(psp_ - 1u) & 15u] == 0x%xu) psp_ = psp_ - 1u;", in->addr);
        }
        if (ended) continue;   /* unreachable within the block */
        switch (in->op) {
        case OP_BRA: case OP_BRX: case OP_SSY: case OP_PBK: case OP_SYNC: case OP_BRK: case OP_EXIT:
            ended = emit_flow(c, in, i + 1 < c->n ? c->ins[i + 1].addr : 0);
            break;
        default: emit_guarded(c, in); break;
        }
    }
    if (open) {
        if (!ended) emit_exit(c);
        c->indent--;
        line(c, "}");
    }
    line(c, "default: {");
    c->indent++;
    emit_exit(c);
    c->indent--;
    line(c, "}");
    line(c, "}");
    line(c, "pc_ = npc_;");
    c->indent--;
    line(c, "}");
    emit_exit(c);
    free(ssy_target);
    free(pbk_target);
}

static const char VERTEX_FETCH[] =
    "uint4 decode_attr(ByteAddressBuffer b, uint addr, uint fmt) {\n"
    "  uint size = (fmt >> 21) & 0x3Fu, type = (fmt >> 27) & 7u, n = 4u, bits = 32u;\n"
    "  bool packed = false;\n"
    "  switch (size) {\n"
    "  case 0x01u: n = 4u; bits = 32u; break; case 0x02u: n = 3u; bits = 32u; break;\n"
    "  case 0x03u: n = 4u; bits = 16u; break; case 0x04u: n = 2u; bits = 32u; break;\n"
    "  case 0x05u: n = 3u; bits = 16u; break; case 0x0Au: n = 4u; bits = 8u; break;\n"
    "  case 0x0Fu: n = 2u; bits = 16u; break; case 0x12u: n = 1u; bits = 32u; break;\n"
    "  case 0x13u: n = 3u; bits = 8u; break; case 0x18u: n = 2u; bits = 8u; break;\n"
    "  case 0x1Bu: n = 1u; bits = 16u; break; case 0x1Du: n = 1u; bits = 8u; break;\n"
    "  case 0x32u: n = 2u; bits = 8u; break; case 0x33u: n = 4u; bits = 8u; break;\n"
    "  case 0x34u: n = 1u; bits = 8u; break; case 0x30u: n = 4u; bits = 10u; packed = true; break;\n"
    "  default: return uint4(0u, 0u, 0u, 0x3F800000u);\n"
    "  }\n"
    "  uint base = addr & ~3u, sh = (addr & 3u) * 8u;\n"
    "  uint4 w = b.Load4(base); uint w4 = b.Load(base + 16u);\n"
    "  uint words[5] = {w.x, w.y, w.z, w.w, w4};\n"
    "  bool is_float = type == 7u || type == 1u || type == 2u || type == 5u || type == 6u;\n"
    "  uint4 o = uint4(0u, 0u, 0u, is_float ? 0x3F800000u : 1u);\n"
    "  [unroll] for (uint i = 0u; i < 4u; i++) {\n"
    "    if (i >= n) break;\n"
    "    uint cb = packed && i == 3u ? 2u : bits;\n"
    "    uint bo = sh + (packed ? i * 10u : i * bits), wi = bo >> 5, s = bo & 31u;\n"
    "    uint v = words[wi] >> s;\n"
    "    if (s + cb > 32u) v |= words[wi + 1u] << (32u - s);\n"
    "    if (cb < 32u) v &= (1u << cb) - 1u;\n"
    "    int sv = cb < 32u ? ((int)(v << (32u - cb))) >> (32u - cb) : asint(v);\n"
    "    float scale = cb < 32u ? (float)((1u << cb) - 1u) : 4294967295.0f;\n"
    "    uint r = v;\n"
    "    switch (type) {\n"
    "    case 1u: r = asuint(max((float)sv / (scale * 0.5f - 0.5f), -1.0f)); break;\n"
    "    case 2u: r = asuint((float)v / scale); break;\n"
    "    case 3u: r = asuint(sv); break;\n"
    "    case 5u: r = asuint((float)v); break;\n"
    "    case 6u: r = asuint((float)sv); break;\n"
    "    case 7u: r = cb == 16u ? asuint(f16tof32(v)) : v; break;\n"
    "    }\n"
    "    o[i] = r;\n"
    "  }\n"
    "  if ((fmt >> 31) != 0u) o = o.zyxw;\n"
    "  return o;\n"
    "}\n";

static const char LOP3[] =
    "uint lop3(uint a, uint b, uint c, uint lut) {\n"
    "  uint r = 0u;\n"
    "  if (lut & 0x01u) r |= ~a & ~b & ~c; if (lut & 0x02u) r |= ~a & ~b & c;\n"
    "  if (lut & 0x04u) r |= ~a & b & ~c;  if (lut & 0x08u) r |= ~a & b & c;\n"
    "  if (lut & 0x10u) r |= a & ~b & ~c;  if (lut & 0x20u) r |= a & ~b & c;\n"
    "  if (lut & 0x40u) r |= a & b & ~c;   if (lut & 0x80u) r |= a & b & c;\n"
    "  return r;\n"
    "}\n";

static const char *texture_decl(u32 type, int shadow) {
    switch (type) {
    case TEX_1D: return "Texture1D<float4>";
    case TEX_ARRAY_1D: return "Texture1DArray<float4>";
    case TEX_ARRAY_2D: return "Texture2DArray<float4>";
    case TEX_3D: return "Texture3D<float4>";
    case TEX_CUBE: return "TextureCube<float4>";
    case TEX_ARRAY_CUBE: return "TextureCubeArray<float4>";
    }
    (void)shadow;
    return "Texture2D<float4>";
}

char *shader_hlsl(const u8 *program, u32 size, int stage, u32 vs_outputs, ShaderInfo *info) {
    return shader_hlsl_ex(program, size, stage, vs_outputs, info, NULL, NULL);
}

static char *translate(const u8 *program, u32 size, int stage, u32 vs_outputs, ShaderInfo *info, CbufReader *read, void *user,
                       const u32 local[3], u32 shared) {
    init_table();
    memset(info, 0, sizeof *info);
    info->stage = stage;
    Ctx ctx = {0}, *c = &ctx;
    if (local) { c->local[0] = local[0]; c->local[1] = local[1]; c->local[2] = local[2]; }
    c->shared = shared;
    c->read_cbuf = read;
    c->read_user = user;
    c->stage = stage;
    c->prog = program;
    c->info = info;
    c->vs_outputs = vs_outputs;
    c->indent = 1;
    decode_program(c, program, size);
    emit_body(c);

    Str out = {0};
    put(&out, "// Maxwell %s shader, %u instructions\n", stage == SHADER_VERTEX ? "vertex" : stage == SHADER_PIXEL ? "pixel" : "compute", c->n);
    put(&out, "cbuffer Params : register(b0) { struct { uint4 attr[32]; float4 fix; float4 frag; uint4 base; uint4 swz[16]; uint4 sb[32]; } params; };\n");
    put(&out, "float comp_(float4 v, uint s) { return s == 2u ? v.x : s == 3u ? v.y : s == 4u ? v.z : s == 5u ? v.w : s >= 6u ? 1.0f : 0.0f; }\n"
              "float4 swz(float4 v, uint k) { uint s = params.swz[%u + (k >> 2)][k & 3u];\n"
              "  return float4(comp_(v, s & 7u), comp_(v, (s >> 3) & 7u), comp_(v, (s >> 6) & 7u), comp_(v, (s >> 9) & 7u)); }\n",
        stage == SHADER_VERTEX ? 0 : 8);
    for (u32 b = 0; b < 18; b++)
        if (info->cbuf_mask >> b & 1) put(&out, "ByteAddressBuffer cb%u : register(t%u);\n", b, b);
    for (u32 k = 0; k < info->ntex; k++) {
        put(&out, "%s tex%u : register(t%u);\n", texture_decl(info->tex[k].type, info->tex[k].shadow), k, 64 + k);
        put(&out, "%s smp%u : register(s%u);\n", info->tex[k].shadow ? "SamplerComparisonState" : "SamplerState", k, k);
    }
    if (strstr(c->body.s ? c->body.s : "", "lop3(")) put(&out, "%s", LOP3);
    if (!c->uses_gmem) info->sb_count = 0;
    else {   /* a global address is looked up in each storage buffer in turn; outside them, 0 */
        u32 first = stage == SHADER_PIXEL ? 16 : 0;
        /* (vkd3d-shader cannot read a RWByteAddressBuffer: there, the same words as a structured buffer) */
        int words = stage == SHADER_COMPUTE && VKD3D_FORMS;
        for (u32 k = 0; k < info->sb_count; k++)
            put(&out, stage != SHADER_COMPUTE ? "ByteAddressBuffer gm%u : register(t%u);\n" : words ? "RWStructuredBuffer<uint> gm%u : register(u%u);\n"
                      : "RWByteAddressBuffer gm%u : register(u%u);\n", k, stage == SHADER_COMPUTE ? k : 96 + k);
        put(&out, "uint gload_(uint lo, uint hi) {\n");
        for (u32 k = 0; k < info->sb_count; k++)
            put(&out, words ? "  { uint4 s = params.sb[%u]; uint d = lo - s.x; if (hi - s.y - (lo < s.x ? 1u : 0u) == 0u && d < s.z) return gm%u[d >> 2u]; }\n"
                            : "  { uint4 s = params.sb[%u]; uint d = lo - s.x; if (hi - s.y - (lo < s.x ? 1u : 0u) == 0u && d < s.z) return gm%u.Load(d); }\n",
                first + k, k);
        put(&out, "  return 0u;\n}\n");
        if (stage == SHADER_COMPUTE) {
            put(&out, "void gstore_(uint lo, uint hi, uint v) {\n");
            for (u32 k = 0; k < info->sb_count; k++)
                put(&out, words ? "  { uint4 s = params.sb[%u]; uint d = lo - s.x; if (hi - s.y - (lo < s.x ? 1u : 0u) == 0u && d < s.z) { gm%u[d >> 2u] = v; return; } }\n"
                                : "  { uint4 s = params.sb[%u]; uint d = lo - s.x; if (hi - s.y - (lo < s.x ? 1u : 0u) == 0u && d < s.z) { gm%u.Store(d, v); return; } }\n",
                    first + k, k);
            put(&out, "}\n");
            put(&out, "void gstore_part_(uint lo, uint hi, uint v, uint bits) {\n"
                      "  uint m = (1u << bits) - 1u, sh = (lo & 3u) * 8u, w = gload_(lo & ~3u, hi);\n"
                      "  gstore_(lo & ~3u, hi, (w & ~(m << sh)) | ((v & m) << sh));\n}\n");
        }
    }
    if (stage == SHADER_COMPUTE) {
        u32 words = c->shared / 4 ? c->shared / 4 : 1;
        put(&out, "#define SMEM_WORDS %uu\n", words);
        if (c->uses_smem) put(&out, "groupshared uint smem_[%u];\n", words);
        put(&out, "#define LMEM_WORDS 256u\n");
        put(&out, "[numthreads(%u, %u, %u)]\nvoid main(uint3 tid_ : SV_GroupThreadID, uint3 cta_ : SV_GroupID) {\n",
            c->local[0] ? c->local[0] : 1, c->local[1] ? c->local[1] : 1, c->local[2] ? c->local[2] : 1);
        for (u32 r = 0; r < 255; r++) if (c->reg_used[r]) put(&out, "  uint r%u = 0u;\n", r);
        for (u32 p = 0; p < 7; p++) if (c->pred_used[p]) put(&out, "  bool p%u = false;\n", p);
        if (c->uses_cc) put(&out, "  uint cc_c = 0u; bool cc_z = false, cc_s = false;\n");
        if (c->uses_lmem) put(&out, "  uint lmem_[LMEM_WORDS];\n");
        put(&out, "%s}\n", c->body.s ? c->body.s : "");
        free(c->body.s);
        free(c->ins);
        free(c->leader);
        return out.s;
    }

    /* the interface between the stages: identical on both sides */
    u32 outputs = stage == SHADER_VERTEX ? c->out_attr_mask : vs_outputs;
    info->outputs = stage == SHADER_VERTEX ? c->out_attr_mask : 0;
    put(&out, "struct VSOut {\n  float4 pos : SV_Position;\n  float clipw : CLIPW;\n");
    for (u32 n = 0; n < 32; n++) {
        if (!(outputs >> n & 1)) continue;
        const char *interp = "";
        if (stage == SHADER_PIXEL) {
            u8 m = program[0x18 + n];
            u32 mode = 0;
            for (u32 k = 0; k < 4 && !mode; k++) mode = (m >> (k * 2)) & 3;
            info->interp[n] = (u8)mode;
            interp = mode == 1 ? "nointerpolation " : mode == 3 ? "noperspective " : "";
        }
        put(&out, "  %sfloat4 a%u : ATTR%u;\n", interp, n, n);
    }
    put(&out, "};\n");

    if (stage == SHADER_VERTEX) {
        put(&out, "%s", VERTEX_FETCH);
        for (u32 n = 0; n < 32; n++) {
            if (!(info->inputs >> n & 1)) continue;
            put(&out, "ByteAddressBuffer vb%u : register(t%u);\n", n, 18 + n);
            put(&out, "uint4 fetch%u(uint vid, uint iid) {\n"
                      "  uint4 a = params.attr[%u];\n"
                      "  if ((a.x & 0x40u) != 0u || a.w == 0u) return uint4(0u, 0u, 0u, 0x3F800000u);\n"
                      "  uint index = a.z != 0u ? params.base.y + iid / a.z : vid;\n"
                      "  return decode_attr(vb%u, index * a.y + ((a.x >> 7) & 0x3FFFu), a.x);\n"
                      "}\n", n, n, n);
        }
        put(&out, "VSOut finish(VSOut o) {\n"
                  "  o.clipw = o.pos.w;\n"
                  "  o.pos = float4(o.pos.x * params.fix.x, o.pos.y * params.fix.y, o.pos.z * params.fix.z + o.pos.w * params.fix.w, o.pos.w);\n"
                  "  return o;\n}\n");
        put(&out, "VSOut main(uint vid : SV_VertexID, uint iid : SV_InstanceID) {\n");
        put(&out, "  VSOut o = (VSOut)0; o.pos.w = 1.0f;\n");
        for (u32 n = 0; n < 32; n++)
            if (info->inputs >> n & 1) put(&out, "  uint4 va%u = fetch%u(vid, iid);\n", n, n);
    } else {
        u32 target = *(const u32 *)(program + 0x48), flags = *(const u32 *)(program + 0x4C);
        put(&out, "struct PSOut {\n");
        for (u32 rt = 0; rt < 8; rt++)
            if ((target >> (rt * 4)) & 15) put(&out, "  float4 c%u : SV_Target%u;\n", rt, rt);
        if (flags & 2) put(&out, "  float depth : SV_Depth;\n");
        put(&out, "};\n");
        put(&out, "PSOut main(VSOut i, bool front : SV_IsFrontFace) {\n");
    }
    for (u32 r = 0; r < 255; r++) if (c->reg_used[r]) put(&out, "  uint r%u = 0u;\n", r);
    for (u32 p = 0; p < 7; p++) if (c->pred_used[p]) put(&out, "  bool p%u = false;\n", p);
    if (c->uses_cc) put(&out, "  uint cc_c = 0u; bool cc_z = false, cc_s = false;\n");
    put(&out, "%s}\n", c->body.s ? c->body.s : "");
    free(c->body.s);
    free(c->ins);
    free(c->leader);
    return out.s;
}

char *shader_hlsl_ex(const u8 *program, u32 size, int stage, u32 vs_outputs, ShaderInfo *info, CbufReader *read, void *user) {
    return translate(program, size, stage, vs_outputs, info, read, user, NULL, 0);
}

char *shader_hlsl_cs(const u8 *program, u32 size, const u32 local[3], u32 shared, ShaderInfo *info, CbufReader *read, void *user) {
    return translate(program, size, SHADER_COMPUTE, 0, info, read, user, local, shared);
}

u32 shader_size(const u8 *program, u32 max) {
    init_table();
    Ctx c = {0};
    decode_program(&c, program, max);
    u32 end = c.n ? c.ins[c.n - 1].addr + 8 : 0x50;
    free(c.ins);
    return end;
}
