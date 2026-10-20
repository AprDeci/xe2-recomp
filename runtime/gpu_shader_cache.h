/* The disk cache of compiled shaders (DXBC files), shared by the renderer (gpu_d3d11.c) and the
 * ahead-of-time compiler (tools/shaderaot.c), so that both key, name and compile a program alike.
 *
 * A file is "<dir>/v<version>_<vs|ps|cs>_<program hash>_<vs outputs>_<jump tables' hash>.dxbc".
 * The ahead-of-time compiler dates its files 2001-01-01 until the game first uses them (the
 * renderer then dates them now): the start's prewarm makes only shaders the game has used. */
#pragma once
#include <stdio.h>
#include <string.h>
#include <d3dcompiler.h>

#include "gpu_shader.h"

/* Bump when the translator changes what it generates: older files are then ignored. */
#define SHADER_CACHE_VERSION 10
#define SHADER_CACHE_UNPLAYED 0x01C07385C89DC000ull   /* FILETIME 2001-01-01 */
#define SHADER_CACHE_PLAYED   0x01C1925741648000ull   /* written after 2002-01-01: the game used it */

/* FNV-1a over a program's bytes (a vertex or pixel program: its header and code). */
static inline u64 shader_program_hash(const u8 *p, u32 size) {
    u64 h = 1469598103934665603ull;
    for (u32 i = 0; i < size; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static inline void shader_cache_file(char *out, size_t n, const char *dir, int stage, u64 hash, u32 vs_outputs, u64 table_hash) {
    snprintf(out, n, "%s/v%u_%s_%016llx_%08x_%016llx.dxbc", dir, SHADER_CACHE_VERSION,
             stage == SHADER_VERTEX ? "vs" : stage == SHADER_PIXEL ? "ps" : "cs", (unsigned long long)hash, vs_outputs,
             (unsigned long long)table_hash);
}

/* fxc as the renderer runs it: dispatch-loop programs ("npc_") without fxc's optimiser (they
 * compile much faster, and the driver optimises the bytecode anyway); any other program again
 * without it when the optimiser fails (it has internal errors on some). */
static inline HRESULT shader_cache_compile(const char *hlsl, const char *name, int stage, ID3DBlob **code, ID3DBlob **errors) {
    const char *target = stage == SHADER_VERTEX ? "vs_5_0" : stage == SHADER_PIXEL ? "ps_5_0" : "cs_5_0";
    UINT flags = strstr(hlsl, "npc_") ? D3DCOMPILE_SKIP_OPTIMIZATION : D3DCOMPILE_OPTIMIZATION_LEVEL1;
    *code = *errors = NULL;
    HRESULT hr = D3DCompile(hlsl, strlen(hlsl), name, NULL, NULL, "main", target, flags, 0, code, errors);
    if (FAILED(hr) && flags != D3DCOMPILE_SKIP_OPTIMIZATION) {
        if (*errors) (*errors)->lpVtbl->Release(*errors);
        *errors = NULL;
        hr = D3DCompile(hlsl, strlen(hlsl), name, NULL, NULL, "main", target, D3DCOMPILE_SKIP_OPTIMIZATION, 0, code, errors);
    }
    return hr;
}
