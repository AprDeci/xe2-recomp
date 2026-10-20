/* The Mii database (mii:e / mii:u), natively.
 *
 * The console keeps the user's Miis in a system save; here the database is empty, so the
 * game sees only the six built-in default Miis. BuildDefault/BuildRandom produce characters
 * the way the service does - as a CharInfo (0x58 bytes, layout taken from the SDK's own
 * CharInfoAccessor getters). */
#define _CRT_RAND_S
#include <stdlib.h>
#include <string.h>

#include "ipc.h"

#define SOURCE_DATABASE 1
#define SOURCE_DEFAULT  2
#define DEFAULT_COUNT   6

typedef struct CharInfo {
    u8 create_id[16];
    u16 nickname[11];
    u8 font_region, favorite_color, gender, height, build, type, region_move;
    u8 faceline_type, faceline_color, faceline_wrinkle, faceline_make;
    u8 hair_type, hair_color, hair_flip;
    u8 eye_type, eye_color, eye_scale, eye_aspect, eye_rotate, eye_x, eye_y;
    u8 eyebrow_type, eyebrow_color, eyebrow_scale, eyebrow_aspect, eyebrow_rotate, eyebrow_x, eyebrow_y;
    u8 nose_type, nose_scale, nose_y;
    u8 mouth_type, mouth_color, mouth_scale, mouth_aspect, mouth_y;
    u8 beard_color, beard_type, mustache_type, mustache_scale, mustache_y;
    u8 glass_type, glass_color, glass_scale, glass_y;
    u8 mole_type, mole_scale, mole_x, mole_y;
    u8 padding;
} CharInfo;
_Static_assert(sizeof(CharInfo) == 0x58, "CharInfo");
_Static_assert(offsetof(CharInfo, font_region) == 0x26 && offsetof(CharInfo, mole_y) == 0x56, "CharInfo layout");

static u32 random32(void) { unsigned v; rand_s(&v); return v; }

/* A plain, valid Mii: the default face with a given gender and favourite colour. */
static CharInfo make(int gender, int color) {
    CharInfo m = {
        .favorite_color = (u8)color, .gender = (u8)gender, .height = 64, .build = 64,
        .hair_type = gender ? 12 : 33, .hair_color = 1,
        .eye_type = gender ? 4 : 2, .eye_scale = 4, .eye_aspect = 3, .eye_rotate = gender ? 3 : 4, .eye_x = 2, .eye_y = 12,
        .eyebrow_type = gender ? 0 : 6, .eyebrow_color = 1, .eyebrow_scale = 4, .eyebrow_aspect = 3,
        .eyebrow_rotate = 6, .eyebrow_x = 2, .eyebrow_y = 10,
        .nose_type = 1, .nose_scale = 4, .nose_y = 9,
        .mouth_type = gender ? 1 : 23, .mouth_scale = 4, .mouth_aspect = 3, .mouth_y = 13,
        .mustache_scale = 4, .mustache_y = 10, .glass_scale = 4, .glass_y = 10,
        .mole_scale = 4, .mole_x = 2, .mole_y = 20,
    };
    static const char name[] = "no name";
    for (int i = 0; name[i]; i++) m.nickname[i] = (u16)name[i];
    return m;
}

static void new_id(CharInfo *m) {
    for (int i = 0; i < 16; i += 4) { u32 r = random32(); memcpy(m->create_id + i, &r, 4); }
    m->create_id[6] = (m->create_id[6] & 0x0F) | 0x40;   /* a version-4 UUID, as the console makes */
    m->create_id[8] = (m->create_id[8] & 0x3F) | 0x80;
}

static CharInfo build_default(u32 index) {
    CharInfo m = make(index >= 3, (int)(index % DEFAULT_COUNT) * 2);
    memset(m.create_id, 0, 16);
    m.create_id[0] = 0xD0 + (u8)index;                   /* fixed ids, so the defaults are stable */
    m.create_id[6] = 0x40; m.create_id[8] = 0x80;
    return m;
}

static CharInfo build_random(u32 age, u32 gender, u32 race) {
    (void)age; (void)race;
    CharInfo m = make(gender < 2 ? (int)gender : (int)(random32() & 1), (int)(random32() % 12));
    m.faceline_type = (u8)(random32() % 12);
    m.faceline_color = (u8)(random32() % 6);
    m.hair_type = (u8)(random32() % 132);
    m.hair_color = (u8)(random32() % 8);
    m.eye_type = (u8)(random32() % 60);
    m.eye_color = (u8)(random32() % 6);
    m.eyebrow_type = (u8)(random32() % 24);
    m.nose_type = (u8)(random32() % 18);
    m.mouth_type = (u8)(random32() % 36);
    m.height = (u8)(random32() % 128);
    m.build = (u8)(random32() % 128);
    new_id(&m);
    return m;
}

static u32 count(u32 source) { return source & SOURCE_DEFAULT ? DEFAULT_COUNT : 0; }

/* Get: fill the output buffer with CharInfos (or CharInfoElements, which add a u32 source). */
static void get(Ipc *ipc, u32 element_size) {
    u32 source = ipc_u32(ipc, 0);
    u64 size, b = ipc_out_buffer(ipc, 0, &size);
    u32 n = 0;
    for (u32 i = 0; i < count(source) && b && (n + 1) * element_size <= size; i++, n++) {
        CharInfo m = build_default(i);
        memcpy(P(void, b + (u64)n * element_size), &m, sizeof m);
        if (element_size > sizeof m) st32(b + (u64)n * element_size + sizeof m, SOURCE_DEFAULT);
    }
    if (b) ipc_wrote(ipc, 0, (u64)n * element_size);
    IPC_PUSH(ipc, n);
}

static void database(IpcObject *self, Ipc *ipc) {
    CharInfo m;
    switch (ipc->command) {
    case 0: IPC_PUSH(ipc, (u8)0); return;                                  /* IsUpdated */
    case 1: IPC_PUSH(ipc, (u8)0); return;                                  /* IsFullDatabase */
    case 2: IPC_PUSH(ipc, count(ipc_u32(ipc, 0))); return;                 /* GetCount */
    case 3: get(ipc, sizeof(CharInfo) + 4); return;                        /* Get (CharInfoElement) */
    case 4: get(ipc, sizeof(CharInfo)); return;                            /* Get1 (CharInfo) */
    case 5: ipc_push(ipc, ipc->in, sizeof(CharInfo)); return;             /* UpdateLatest: already latest */
    case 6: m = build_random(ipc_u32(ipc, 0), ipc_u32(ipc, 4), ipc_u32(ipc, 8)); ipc_push(ipc, &m, sizeof m); return;
    case 7: m = build_default(ipc_u32(ipc, 0) % DEFAULT_COUNT); ipc_push(ipc, &m, sizeof m); return;
    case 8: case 9: IPC_PUSH(ipc, (u32)0); return;                         /* Get2/Get3 (store data): none */
    case 11: IPC_PUSH(ipc, (i32)-1); return;                               /* FindIndex: not in the database */
    case 21: IPC_PUSH(ipc, (i32)-1); return;                               /* GetIndex */
    case 22: return;                                                       /* SetInterfaceVersion */
    case 23: case 24:                                                      /* Convert(Ver3StoreData / CoreData) */
        m = build_default(0); new_id(&m); ipc_push(ipc, &m, sizeof m); return;
    }
    ipc_unknown(ipc, self);
}

static void static_service(IpcObject *self, Ipc *ipc) {
    if (ipc->command == 0) {   /* GetDatabaseService(key code) */
        ipc_object(ipc, ipc_new_object("mii:IDatabaseService", database, NULL));
        return;
    }
    ipc_unknown(ipc, self);
}

IpcObject *service_mii(const char *name) {
    if (!strcmp(name, "mii:e") || !strcmp(name, "mii:u")) return ipc_new_object(name, static_service, NULL);
    return NULL;
}
