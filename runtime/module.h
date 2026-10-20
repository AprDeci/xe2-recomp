/* What the recompiler emits for each module, for the host loader. */
#pragma once
#include "recomp.h"

typedef struct RtFunc { u64 addr; GuestFn *fn; } RtFunc;          /* guest entry -> native code */
typedef struct RtSym { const char *name; u64 addr; } RtSym;       /* exported symbol */
typedef struct RtReloc { u64 offset; i64 addend; u32 import; } RtReloc;   /* *(base + offset) = symbol + addend */

typedef struct RtModule {
    const char *name;
    u64 base, image_size, text_size, init;
    const RtFunc *funcs;
    u32 func_count;
    const RtSym *exports;
    u32 export_count;
    const char *const *imports;      /* every symbol a relocation refers to, own exports included */
    const u8 *import_weak;           /* weak references resolve to 0 when nothing defines them */
    u32 import_count;
    const RtReloc *relocs;
    u32 reloc_count;
    const char *build_id;            /* hex, trailing zeros dropped: identifies an NRO the game loads */
} RtModule;

/* Modules the game loads at run time (NROs), recompiled ahead of time at fixed addresses. */
extern const RtModule *const rt_nro_modules[];
extern const u32 rt_nro_count;
