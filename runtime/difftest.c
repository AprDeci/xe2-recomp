/* Native side of the differential tester: runs lifted single instructions / short
 * snippets from given CPU states and records the resulting state for comparison with a
 * reference AArch64 emulator. Usage: difftest <image> <scratch.bin> <in.bin> <out.bin> */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#include "recomp.h"

#define BASE 0x7100000000ull
#define SCRATCH 0x7200000000ull
#define SCRATCH_SIZE 0x20000

typedef struct TestIO { u32 n, z, c, v; u64 pc; } TestIO;
typedef void TestFn(Cpu *, TestIO *);
extern TestFn *const t_funcs[];
extern const u32 t_count;

struct In { u32 idx, pad; u64 x[31], sp, nzcv, v[64]; };
struct Out { u64 x[31], sp, nzcv, v[64], pc, dirty_off, dirty_len; u8 dirty[64]; u64 fault; };

_Noreturn void rt_trap(Cpu *c, u64 pc, const char *why) {
    (void)c; (void)pc; (void)why;
    RaiseException(0xE0000001, 0, 0, NULL);
    abort();
}

GuestFn *rt_lookup(u64 addr) { rt_trap(NULL, addr, "indirect call inside a test"); }
GuestFn *rt_lookup_cached(u64 addr, RtCache *cache) { (void)cache; return rt_lookup(addr); }

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

static int run(TestFn *fn, Cpu *cpu, TestIO *io) {
    __try {
        fn(cpu, io);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: difftest <image> <scratch.bin> <in.bin> <out.bin>\n"); return 2; }

    HANDLE file = CreateFileA(argv[1], GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    HANDLE map = file == INVALID_HANDLE_VALUE ? NULL : CreateFileMappingA(file, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!map || MapViewOfFileEx(map, FILE_MAP_READ, 0, 0, 0, (void *)BASE) != (void *)BASE) {
        fprintf(stderr, "cannot map %s at %#llx\n", argv[1], BASE);
        return 2;
    }
    u8 *scratch = VirtualAlloc((void *)SCRATCH, SCRATCH_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    size_t n;
    u8 *pristine = read_file(argv[2], &n);
    if (scratch != (u8 *)SCRATCH || n != SCRATCH_SIZE) { fprintf(stderr, "scratch setup failed\n"); return 2; }
    memcpy(scratch, pristine, SCRATCH_SIZE);

    struct In *in = read_file(argv[3], &n);
    size_t count = n / sizeof *in;
    struct Out *out = calloc(count ? count : 1, sizeof *out);
    for (size_t t = 0; t < count; t++) {
        Cpu cpu = {0};
        TestIO io = {(u32)(in[t].nzcv >> 3) & 1, (u32)(in[t].nzcv >> 2) & 1, (u32)(in[t].nzcv >> 1) & 1,
                     (u32)in[t].nzcv & 1, 0};
        memcpy(cpu.x, in[t].x, sizeof cpu.x);
        cpu.sp = in[t].sp;
        memcpy(cpu.v, in[t].v, sizeof cpu.v);
        struct Out *o = &out[t];
        o->fault = in[t].idx >= t_count ? 2 : run(t_funcs[in[t].idx], &cpu, &io);
        memcpy(o->x, cpu.x, sizeof cpu.x);
        o->sp = cpu.sp;
        o->nzcv = (u64)io.n << 3 | io.z << 2 | io.c << 1 | io.v;
        memcpy(o->v, cpu.v, sizeof cpu.v);
        o->pc = io.pc;
        o->dirty_off = ~0ull;
        if (memcmp(scratch, pristine, SCRATCH_SIZE)) {
            size_t lo = 0, hi = SCRATCH_SIZE;
            while (scratch[lo] == pristine[lo]) lo++;
            while (scratch[hi - 1] == pristine[hi - 1]) hi--;
            o->dirty_off = lo;
            o->dirty_len = hi - lo;
            memcpy(o->dirty, scratch + lo, hi - lo < 64 ? hi - lo : 64);
            memcpy(scratch + lo, pristine + lo, hi - lo);
        }
    }
    FILE *f = fopen(argv[4], "wb");
    if (!f || fwrite(out, sizeof *out, count, f) != count) { perror(argv[4]); return 2; }
    fclose(f);
    return 0;
}
