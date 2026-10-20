/* Native side of the whole-function differential tester: calls production-lifted functions
 * from given CPU states and records the state at return for comparison with a reference
 * emulator. Usage: functest <image> <scratch.bin> <in.bin> <out.bin> */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#include "recomp.h"

#define BASE 0x7100000000ull
#define SCRATCH 0x7200000000ull

extern const struct RtFunc { u64 addr; GuestFn *fn; } rt_funcs[];
extern const u32 rt_func_count;

struct In { u32 idx, pad; u64 x[31], sp, v[64]; };
#define DIRTY_KEEP 1024
struct Out { u64 x[31], sp, v[64], dirty_off, dirty_len, dirty_crc, fault; u8 dirty[DIRTY_KEEP]; };

_Noreturn void rt_trap(Cpu *c, u64 pc, const char *why) {
    (void)c; (void)pc; (void)why;
    RaiseException(0xE0000001, 0, 0, NULL);
    abort();
}

GuestFn *rt_lookup(u64 addr) { rt_trap(NULL, addr, "indirect call inside a leaf-function test"); }
GuestFn *rt_lookup_cached(u64 addr, RtCache *cache) { (void)cache; return rt_lookup(addr); }

static u32 crc_table[256];
static u32 crc32(const u8 *p, size_t n) {
    u32 crc = 0xFFFFFFFFu;
    while (n--) crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static void *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    _fseeki64(f, 0, SEEK_END);
    *size = (size_t)_ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    void *p = malloc(*size ? *size : 1);
    if (fread(p, 1, *size, f) != *size) { fprintf(stderr, "short read: %s\n", path); exit(2); }
    fclose(f);
    return p;
}

static int run(GuestFn *fn, Cpu *cpu) {
    __try {
        fn(cpu);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: functest <image> <scratch.bin> <in.bin> <out.bin>\n"); return 2; }
    for (u32 i = 0; i < 256; i++) {
        u32 c = i;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
        crc_table[i] = c;
    }
    /* The image is mapped read-only: a test that writes a global is rejected by the reference
     * side, so any write here is a real divergence and shows up as a fault. */
    HANDLE file = CreateFileA(argv[1], GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    HANDLE map = file == INVALID_HANDLE_VALUE ? NULL : CreateFileMappingA(file, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!map || MapViewOfFileEx(map, FILE_MAP_READ, 0, 0, 0, (void *)BASE) != (void *)BASE) {
        fprintf(stderr, "cannot map %s at %#llx\n", argv[1], BASE);
        return 2;
    }
    size_t scratch_size, n;
    u8 *pristine = read_file(argv[2], &scratch_size);
    u8 *scratch = VirtualAlloc((void *)SCRATCH, scratch_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (scratch != (u8 *)SCRATCH) { fprintf(stderr, "scratch setup failed\n"); return 2; }
    memcpy(scratch, pristine, scratch_size);

    struct In *in = read_file(argv[3], &n);
    size_t count = n / sizeof *in;
    struct Out *out = calloc(count ? count : 1, sizeof *out);
    for (size_t t = 0; t < count; t++) {
        Cpu cpu = {0};
        memcpy(cpu.x, in[t].x, sizeof cpu.x);
        cpu.sp = in[t].sp;
        memcpy(cpu.v, in[t].v, sizeof cpu.v);
        struct Out *o = &out[t];
        o->fault = in[t].idx >= rt_func_count ? 2 : run(rt_funcs[in[t].idx].fn, &cpu);
        memcpy(o->x, cpu.x, sizeof cpu.x);
        o->sp = cpu.sp;
        memcpy(o->v, cpu.v, sizeof cpu.v);
        o->dirty_off = ~0ull;
        if (memcmp(scratch, pristine, scratch_size)) {
            size_t lo = 0, hi = scratch_size;
            while (scratch[lo] == pristine[lo]) lo++;
            while (scratch[hi - 1] == pristine[hi - 1]) hi--;
            lo &= ~(size_t)7;                 /* whole 8-byte lanes, so floats can be compared as values */
            hi = (hi + 7) & ~(size_t)7;
            o->dirty_off = lo;
            o->dirty_len = hi - lo;
            o->dirty_crc = crc32(scratch + lo, hi - lo);
            memcpy(o->dirty, scratch + lo, hi - lo < DIRTY_KEEP ? hi - lo : DIRTY_KEEP);
            memcpy(scratch + lo, pristine + lo, hi - lo);
        }
    }
    FILE *f = fopen(argv[4], "wb");
    if (!f || fwrite(out, sizeof *out, count, f) != count) { perror(argv[4]); return 2; }
    fclose(f);
    return 0;
}
