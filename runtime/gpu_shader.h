/* Maxwell shader programs translated to HLSL (Shader Model 5) for the D3D11 back end.
 *
 * Resource layout the generated code expects:
 *   t0..t17    constant buffers 0..17 of the stage, as raw buffers
 *   t18..t49   vertex streams 0..31 (vertex shaders), as raw buffers
 *   t64+i      texture i of the shader's texture table, s0+i its sampler
 *   t96+i      storage buffer i (ShaderInfo.sb_offset), as raw buffers (compute: u0+i, writable)
 *   b0         per-draw parameters (vertex attribute formats, viewport fix-up), see DrawParams */
#pragma once
#include "host.h"

enum { SHADER_VERTEX, SHADER_PIXEL, SHADER_COMPUTE };

enum { TEX_1D, TEX_ARRAY_1D, TEX_2D, TEX_ARRAY_2D, TEX_3D, TEX_ARRAY_3D, TEX_CUBE, TEX_ARRAY_CUBE };

typedef struct ShaderTexture {
    u32 handle_offset;   /* byte offset of the texture handle in the bindless constant buffer */
    u8 type;             /* TEX_* */
    u8 shadow;           /* depth compare */
} ShaderTexture;

typedef struct ShaderInfo {
    int stage;
    u32 cbuf_mask;                 /* constant buffer slots read */
    u32 cbuf_extent[18];           /* bytes of each read at most (~0u: indexed, so all of it) */
    u32 inputs;                    /* generic attributes read (vertex: from streams; pixel: interpolated) */
    u32 outputs;                   /* vertex: generic attributes written */
    u8 interp[32];                 /* pixel: per generic input, 1 flat, 2 perspective, 3 linear */
    u32 color_mask;                /* pixel: render targets written */
    int writes_depth;
    ShaderTexture tex[32];
    u32 ntex;
    int kills;
    char unsupported[256];         /* first instruction the translator could not handle, if any */
    u32 ntables;                   /* indirect branches' jump tables, read from constant buffers */
    struct { u32 bank, offset, count; } table[4];
    u32 sb_count;                  /* global memory: the storage buffers it may fall in, */
    u32 sb_offset[16];             /* by the offset of their descriptor in constant buffer 0 */
    int stores;                    /* compute: writes global memory */
} ShaderInfo;

/* NVN describes storage buffers in constant buffer 0, 16 bytes each from SB_FIRST: the GPU
 * address (64 bits), then the size. Global memory accesses are resolved against them. */
#define SB_FIRST 0x110
#define SB_END   0x610

/* Translate one program (the 0x50-byte header, then code). For a pixel shader,
 * `vs_outputs` is the generic-attribute mask the paired vertex shader writes: the pixel
 * shader's input signature is made identical to it, as Direct3D requires. Returns
 * malloc'd HLSL source. */
char *shader_hlsl(const u8 *program, u32 size, int stage, u32 vs_outputs, ShaderInfo *info);

/* The same, able to read the stage's constant buffers: indirect branches (BRX) take their
 * targets from a jump table in one, and knowing them keeps the generated code small. */
typedef u32 CbufReader(void *user, u32 bank, u32 offset);
char *shader_hlsl_ex(const u8 *program, u32 size, int stage, u32 vs_outputs, ShaderInfo *info, CbufReader *read, void *user);

/* A compute program (no header: pass it with 0x50 bytes of anything in front, so that its
 * addresses line up with those of the other stages), its workgroup size and shared memory. */
char *shader_hlsl_cs(const u8 *program, u32 size, const u32 local[3], u32 shared, ShaderInfo *info, CbufReader *read, void *user);

/* Per-draw parameters (cbuffer b0), shared by both stages. */
typedef struct DrawParams {
    u32 attr[32][4];       /* vertex attribute: format word, stream stride, instance divisor (0: per vertex), stream bound */
    float fix[4];          /* clip-space fix-up: x sign, y sign, z scale, z offset */
    float frag[4];         /* fragment y: flip (nonzero), render target height */
    u32 base[4];           /* base vertex, base instance */
    u32 swz[2][32];        /* per texture of each stage (vertex, pixel): component sources, 3 bits each */
    u32 sb[2][16][4];      /* per storage buffer of each stage: GPU address low, high, size */
} DrawParams;

/* Bytes of a program up to its end (header included), reading at most `max`. */
u32 shader_size(const u8 *program, u32 max);
