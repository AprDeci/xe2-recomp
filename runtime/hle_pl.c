/* nn::pl - the system's shared fonts, natively; and VR mode, which is never on.
 *
 * The firmware stores each system font as a .bfttf: an 8-byte header, then a TrueType font
 * XORed word by word (big-endian) with a key derived from the header's magic. The pl service
 * decodes them into memory shared with every application, back to back, and reports each
 * font as an offset just past its header - so the game sees plain TrueType. Here the same
 * files, taken from the firmware (dump/fonts, see tools/nca.py), are decoded the same way. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>

#include "host.h"

/* By nn::pl::SharedFontType: Standard, ChineseSimple, ChineseSimpleExtension,
 * ChineseTraditional, Korean, NintendoExtension. */
static const char *const files[] = {
    "nintendo_udsg-r_std_003.bfttf", "nintendo_udsg-r_org_zh-cn_003.bfttf", "nintendo_udsg-r_ext_zh-cn_003.bfttf",
    "nintendo_udjxh-db_zh-tw_003.bfttf", "nintendo_udsg-r_ko_003.bfttf", "nintendo_ext_003.bfttf",
};
#define FONTS (sizeof files / sizeof *files)
#define AREA_SIZE 0x1200000   /* 18 MB, as on the console */
#define DECODED_MAGIC 0x7F9A0218u   /* what the first header word decodes to */

static struct { u64 address, size; } font[FONTS];
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK load_fonts(INIT_ONCE *o, void *p, void **ctx) {
    (void)o; (void)p; (void)ctx;
    if (VirtualAlloc((void *)SHARED_BASE, AREA_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != (void *)SHARED_BASE)
        rt_fatal("cannot map the shared font area");
    kernel_add_region(SHARED_BASE, AREA_SIZE, 6 /* shared */, 1 /* read */);
    u64 at = SHARED_BASE;
    for (u32 i = 0; i < FONTS; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/../fonts/%s", rt_data_dir, files[i]);
        FILE *f = fopen(path, "rb");
        if (!f) { fprintf(stderr, "[pl] %s is missing; that font is empty\n", path); continue; }
        size_t n = fread(P(void, at), 1, (size_t)(SHARED_BASE + AREA_SIZE - at), f);
        fclose(f);
        if (n < 8) continue;
        u32 *w = P(u32, at), key = _byteswap_ulong(w[0]) ^ DECODED_MAGIC;
        w[0] = _byteswap_ulong(DECODED_MAGIC);
        for (size_t k = 2; k < n / 4; k++) w[k] ^= _byteswap_ulong(key);
        font[i].address = at + 8;
        font[i].size = n - 8;
        at += (n + 3) & ~3ull;
    }
    return TRUE;
}

static u32 font_type(Cpu *c) {
    InitOnceExecuteOnce(&once, load_fonts, NULL, NULL);
    return (u32)c->x[0] < FONTS ? (u32)c->x[0] : 0;
}

HLE(_ZN2nn2pl21RequestSharedFontLoadENS0_14SharedFontTypeE) { font_type(c); }
HLE(_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE) { c->x[0] = font[font_type(c)].size ? 1 : 0; }
HLE(_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE) { c->x[0] = font[font_type(c)].address; }
HLE(_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE) { c->x[0] = font[font_type(c)].size; }

HLE(_ZN2nn2pl8IsVrModeEv) { c->x[0] = 0; }
HLE(_ZN2nn2pl11BeginVrModeEv) { (void)c; }
HLE(_ZN2nn2pl9EndVrModeEv) { (void)c; }
