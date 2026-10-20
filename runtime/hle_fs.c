/* nn::fs, natively.
 *
 *   rom:     the game's RomFS, read in place from its NCA (the section is not encrypted), so
 *            the 16.9 GB data.arc is served by positional reads with no extraction; or, with
 *            an update applied (tools/update.py), pieced together from several files by a
 *            layer map - the base's NCA and the update's own data
 *   save:    save data, a host folder per user (native-port/save/<user>)
 *   cache:   cache storage, a host folder (native-port/cache)
 *
 * Paths are "<mount>:/<path>". Handles are indices into a table; reads are positional, so
 * any number of game threads can read at once. Results use the SDK's file-system codes so
 * the game's own error handling (first boot: no save yet, and so on) takes its usual paths. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>

#include "host.h"

#define R_PATH_NOT_FOUND RESULT(2, 1)
#define R_PATH_EXISTS RESULT(2, 2)
#define R_OUT_OF_RANGE RESULT(2, 3005)
#define R_NOT_MOUNTED RESULT(2, 1001)
#define R_TOO_MANY_HANDLES RESULT(2, 6604)
#define R_INVALID_HANDLE RESULT(2, 6602)
#define R_MOUNT_EXISTS RESULT(2, 60)
#define R_UNSUPPORTED RESULT(2, 6300)

enum { DIR_ENTRY = 0, FILE_ENTRY = 1 };          /* nn::fs::DirectoryEntryType */
enum { OPEN_READ = 1, OPEN_WRITE = 2, OPEN_APPEND = 4 };
enum { LIST_DIRS = 1, LIST_FILES = 2 };

/* ---- the RomFS -------------------------------------------------------------------- */
/* A layer: image bytes [at, the next layer's at) are source `src` from `off` on (LAYER_ZERO:
 * zeros). The map file (tools/nca.py write_layers): "SSBUROM1", u64 image size, u32 count,
 * u32 0, then the layers in image order. */
typedef struct { u64 at, off; u32 src, pad; } RomLayer;
#define LAYER_ZERO 0xFFFFFFFFu
#define ROM_SOURCES 8

static struct {
    HANDLE file;
    u64 base;           /* plain: where the RomFS image starts in the NCA */
    HANDLE sources[ROM_SOURCES];   /* layered: the files the layers come from */
    RomLayer *layers;
    u32 nlayers;
    u64 size;           /* layered: the image's size */
    u8 *dirs, *files;   /* directory and file metadata tables */
    u64 data;           /* file data offset within the image */
} rom;

typedef struct { u32 parent, sibling, child_dir, child_file, hash, name_len; char name[]; } RomDir;
typedef struct { u32 parent, sibling; u64 offset, size; u32 hash, name_len; char name[]; } RomFile;
#define NONE 0xFFFFFFFFu

static int pread_all(HANDLE h, u64 offset, void *buf, u64 size) {
    u8 *p = buf;
    while (size) {
        OVERLAPPED o = {0};
        o.Offset = (DWORD)offset;
        o.OffsetHigh = (DWORD)(offset >> 32);
        DWORD chunk = size > (1u << 30) ? (1u << 30) : (DWORD)size, got = 0;
        if (!ReadFile(h, p, chunk, &got, &o) || !got) return 0;
        p += got; offset += got; size -= got;
    }
    return 1;
}

/* Read image bytes [at, at + size). */
static int rom_read(u64 at, void *buf, u64 size) {
    if (!rom.layers) return pread_all(rom.file, rom.base + at, buf, size);
    if (at + size > rom.size) return 0;
    u32 lo = 0, hi = rom.nlayers;   /* the last layer starting at or before `at` */
    while (hi - lo > 1) {
        u32 mid = (lo + hi) / 2;
        if (rom.layers[mid].at <= at) lo = mid; else hi = mid;
    }
    u8 *p = buf;
    for (u32 k = lo; size; k++) {
        const RomLayer *l = &rom.layers[k];
        u64 end = k + 1 < rom.nlayers ? rom.layers[k + 1].at : rom.size;
        u64 n = end - at < size ? end - at : size;
        if (l->src == LAYER_ZERO) memset(p, 0, n);
        else if (!pread_all(rom.sources[l->src], l->off + (at - l->at), p, n)) return 0;
        p += n; at += n; size -= n;
    }
    return 1;
}

static HANDLE rom_file_open(const char *path) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, NULL);
    if (h == INVALID_HANDLE_VALUE) rt_fatal("cannot open the game's RomFS at %s", path);
    return h;
}

/* romfs.cfg: "<file>\n<byte offset>\n" (the image in place in one file), or "layered\n<layer
 * map>\n<source 0>\n<source 1>...\n" (tools/update.py). */
static void rom_open(void) {
    char cfg[1024], path[1024];
    snprintf(cfg, sizeof cfg, "%s/romfs.cfg", rt_data_dir);
    FILE *f = fopen(cfg, "r");
    if (!f || !fgets(path, sizeof path, f)) rt_fatal("cannot read %s", cfg);
    path[strcspn(path, "\r\n")] = 0;
    if (!strcmp(path, "layered")) {
        char map[1024];
        if (!fgets(map, sizeof map, f)) rt_fatal("%s: no layer map", cfg);
        map[strcspn(map, "\r\n")] = 0;
        u32 n = 0;
        while (n < ROM_SOURCES && fgets(path, sizeof path, f)) {
            path[strcspn(path, "\r\n")] = 0;
            if (path[0]) rom.sources[n++] = rom_file_open(path);
        }
        HANDLE m = rom_file_open(map);
        u8 head[24];
        if (!pread_all(m, 0, head, sizeof head) || memcmp(head, "SSBUROM1", 8)) rt_fatal("%s: not a layer map", map);
        memcpy(&rom.size, head + 8, 8);
        memcpy(&rom.nlayers, head + 16, 4);
        rom.layers = malloc(sizeof *rom.layers * rom.nlayers);
        if (!rom.nlayers || !pread_all(m, sizeof head, rom.layers, sizeof *rom.layers * rom.nlayers))
            rt_fatal("%s: cannot read its %u layers", map, rom.nlayers);
        CloseHandle(m);
        for (u32 k = 0; k < rom.nlayers; k++)
            if (rom.layers[k].src != LAYER_ZERO && rom.layers[k].src >= n) rt_fatal("%s: layer %u names source %u of %u", map, k, rom.layers[k].src, n);
        fprintf(stderr, "[fs] RomFS: %llu bytes in %u layers from %u files (%s)\n", rom.size, rom.nlayers, n, cfg);
    } else {
        if (fscanf(f, "%llu", &rom.base) != 1) rt_fatal("cannot read %s", cfg);
        rom.file = rom_file_open(path);
    }
    fclose(f);
    u64 h[10];
    if (!rom_read(0, h, sizeof h) || h[0] != 0x50) rt_fatal("%s: no RomFS image there", cfg);
    rom.dirs = malloc(h[4]);
    rom.files = malloc(h[8]);
    if (!rom_read(h[3], rom.dirs, h[4]) || !rom_read(h[7], rom.files, h[8]))
        rt_fatal("cannot read the RomFS tables");
    rom.data = h[9];
}

static RomDir *rom_dir(u32 off) { return (RomDir *)(rom.dirs + off); }
static RomFile *rom_file(u32 off) { return (RomFile *)(rom.files + off); }

/* Resolve a RomFS path: sets *dir or *file (offsets into the tables); returns 0 if absent. */
static int rom_lookup(const char *path, u32 *dir, u32 *file) {
    u32 d = 0;
    *file = NONE;
    while (*path == '/') path++;
    while (*path) {
        size_t n = strcspn(path, "/");
        u32 next = NONE;
        for (u32 k = rom_dir(d)->child_dir; k != NONE; k = rom_dir(k)->sibling)
            if (rom_dir(k)->name_len == n && !memcmp(rom_dir(k)->name, path, n)) { next = k; break; }
        if (next == NONE) {
            if (path[n]) return 0;   /* only the last component may be a file */
            for (u32 k = rom_dir(d)->child_file; k != NONE; k = rom_file(k)->sibling)
                if (rom_file(k)->name_len == n && !memcmp(rom_file(k)->name, path, n)) { *file = k; return 1; }
            return 0;
        }
        d = next;
        path += n;
        while (*path == '/') path++;
    }
    *dir = d;
    return 1;
}

/* ---- mounts and handles ---------------------------------------------------------- */
typedef enum { M_ROM = 1, M_HOST } MountKind;
static struct Mount { char name[32]; MountKind kind; char root[512]; } mounts[16];

typedef struct Handle {
    int used, kind, mode;
    HANDLE host;              /* host file */
    u64 offset, size;         /* rom file: data offset in the RomFS image and length */
    char **names; u8 *types; u64 *sizes; i64 count, next;   /* directory listing */
} Handle;
static Handle handles[512];
static CRITICAL_SECTION lock;
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init_fs(PINIT_ONCE o, PVOID p, PVOID *ctx) {
    (void)o; (void)p; (void)ctx;
    InitializeCriticalSection(&lock);
    rom_open();
    return TRUE;
}
static void ready(void) { InitOnceExecuteOnce(&once, init_fs, NULL, NULL); }

static struct Mount *find_mount(const char *path, const char **rest) {
    const char *colon = strchr(path, ':');
    if (!colon) return NULL;
    for (int i = 0; i < 16; i++)
        if (mounts[i].kind && strlen(mounts[i].name) == (size_t)(colon - path) && !memcmp(mounts[i].name, path, colon - path)) {
            *rest = colon + 1;
            return &mounts[i];
        }
    return NULL;
}

static u32 add_mount(const char *name, MountKind kind, const char *root) {
    EnterCriticalSection(&lock);
    for (int i = 0; i < 16; i++)
        if (mounts[i].kind && !strcmp(mounts[i].name, name)) { LeaveCriticalSection(&lock); return R_MOUNT_EXISTS; }
    for (int i = 0; i < 16; i++)
        if (!mounts[i].kind) {
            snprintf(mounts[i].name, sizeof mounts[i].name, "%s", name);
            mounts[i].kind = kind;
            snprintf(mounts[i].root, sizeof mounts[i].root, "%s", root);
            if (kind == M_HOST) {   /* create the folder chain */
                char tmp[512];
                snprintf(tmp, sizeof tmp, "%s", root);
                for (char *p = tmp + 3; *p; p++)
                    if (*p == '/' || *p == '\\') { char k = *p; *p = 0; CreateDirectoryA(tmp, NULL); *p = k; }
                CreateDirectoryA(tmp, NULL);
            }
            LeaveCriticalSection(&lock);
            return 0;
        }
    LeaveCriticalSection(&lock);
    return R_MOUNT_EXISTS;
}

static void host_path(char *out, size_t n, struct Mount *m, const char *rest) {
    while (*rest == '/') rest++;
    snprintf(out, n, "%s/%s", m->root, rest);
    for (char *p = out; *p; p++) if (*p == '/') *p = '\\';
}

static u64 new_handle(Handle h) {
    EnterCriticalSection(&lock);
    for (u32 i = 1; i < 512; i++)
        if (!handles[i].used) {
            handles[i] = h;
            handles[i].used = 1;
            LeaveCriticalSection(&lock);
            return 0x46530000ull | i;
        }
    LeaveCriticalSection(&lock);
    return 0;
}

static Handle *get_handle(u64 value) {
    u32 i = (u32)(value & 0xFFFF);
    return (value >> 16) == 0x4653 && i < 512 && handles[i].used ? &handles[i] : NULL;
}

static const char *data_root(char *out, size_t n, const char *leaf) {   /* native-port/<leaf>, or under SSBU_DATA_ROOT (test runs) */
    const char *root = getenv("SSBU_DATA_ROOT");
    if (root) snprintf(out, n, "%s/%s", root, leaf);
    else snprintf(out, n, "%s/../../%s", rt_data_dir, leaf);
    return out;
}

/* ---- mounting -------------------------------------------------------------------- */
HLE(_ZN2nn2fs12SetAllocatorEPFPvmEPFvS1_mE) { (void)c; ready(); }
HLE(_ZN2nn2fs22QueryMountRomCacheSizeEPm) { ready(); st64(c->x[0], 0x1000); c->x[0] = 0; }
HLE(_ZN2nn2fs8MountRomEPKcPvm) { ready(); c->x[0] = add_mount(P(char, c->x[0]), M_ROM, ""); }
HLE(_ZN2nn2fs17MountCacheStorageEPKc) {
    char root[512];
    ready();
    c->x[0] = add_mount(P(char, c->x[0]), M_HOST, data_root(root, sizeof root, "cache"));
}
HLE(_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE) {
    char leaf[64], root[512];
    ready();
    u64 uid[2] = {ld64(c->x[1]), ld64(c->x[1] + 8)};
    account_save_id(uid);   /* (a netplay install's own id keeps the usual save) */
    snprintf(leaf, sizeof leaf, "save/%016llx%016llx", uid[0], uid[1]);
    c->x[0] = add_mount(P(char, c->x[0]), M_HOST, data_root(root, sizeof root, leaf));
}
HLE(_ZN2nn2fs14EnsureSaveDataERKNS_7account3UidE) { ready(); c->x[0] = 0; }
HLE(_ZN2nn2fs18IsSaveDataExistingERKNS_7account3UidE) { ready(); c->x[0] = 1; }
HLE(_ZN2nn2fs15GetSaveDataSizeEPlS1_RKNS_7account3UidE) {
    st64(c->x[0], 64ull << 20);
    if (c->x[1]) st64(c->x[1], 64ull << 20);
    c->x[0] = 0;
}
HLE(_ZN2nn2fs7UnmountEPKc) {
    ready();
    EnterCriticalSection(&lock);
    for (int i = 0; i < 16; i++)
        if (mounts[i].kind && !strcmp(mounts[i].name, P(char, c->x[0]))) mounts[i].kind = 0;
    LeaveCriticalSection(&lock);
}
HLE(_ZN2nn2fs6CommitEPKc) { c->x[0] = 0; }   /* host writes are already durable */
/* fs::CommitSaveData(name): the save-game flush (XC2's title state calls it after saving).
 * The save mount is an M_HOST directory, so files are already on disk; report success instead
 * of reaching the SDK body, which aborts when its journal/server state is absent. */
HLE(_ZN2nn2fs14CommitSaveDataEPKc) { c->x[0] = 0; }

/* ---- queries --------------------------------------------------------------------- */
HLE(_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc) {
    const char *rest;
    ready();
    struct Mount *m = find_mount(P(char, c->x[1]), &rest);
    if (!m) { c->x[0] = R_NOT_MOUNTED; return; }
    if (m->kind == M_ROM) {
        u32 d, f;
        if (!rom_lookup(rest, &d, &f)) { c->x[0] = R_PATH_NOT_FOUND; return; }
        st32(c->x[0], f == NONE ? DIR_ENTRY : FILE_ENTRY);
    } else {
        char path[1024];
        host_path(path, sizeof path, m, rest);
        DWORD a = GetFileAttributesA(path);
        if (a == INVALID_FILE_ATTRIBUTES) { c->x[0] = R_PATH_NOT_FOUND; return; }
        st32(c->x[0], a & FILE_ATTRIBUTE_DIRECTORY ? DIR_ENTRY : FILE_ENTRY);
    }
    c->x[0] = 0;
}

/* ---- files ----------------------------------------------------------------------- */
HLE(_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci) {
    const char *rest;
    ready();
    struct Mount *m = find_mount(P(char, c->x[1]), &rest);
    int mode = (int)c->x[2];
    if (!m) { c->x[0] = R_NOT_MOUNTED; return; }
    Handle h = {0};
    h.kind = FILE_ENTRY;
    h.mode = mode;
    if (m->kind == M_ROM) {
        u32 d, f;
        if (!rom_lookup(rest, &d, &f) || f == NONE) { c->x[0] = R_PATH_NOT_FOUND; return; }
        if (mode & OPEN_WRITE) { c->x[0] = R_UNSUPPORTED; return; }
        h.offset = rom.data + rom_file(f)->offset;
        h.size = rom_file(f)->size;
    } else {
        char path[1024];
        host_path(path, sizeof path, m, rest);
        h.host = CreateFileA(path, GENERIC_READ | (mode & OPEN_WRITE ? GENERIC_WRITE : 0), FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, OPEN_EXISTING, 0, NULL);
        if (h.host == INVALID_HANDLE_VALUE) { c->x[0] = R_PATH_NOT_FOUND; return; }
    }
    u64 v = new_handle(h);
    if (!v) { if (h.host) CloseHandle(h.host); c->x[0] = R_TOO_MANY_HANDLES; return; }
    st64(c->x[0], v);
    c->x[0] = 0;
}

static u64 file_size(Handle *h) {
    if (!h->host) return h->size;
    LARGE_INTEGER s;
    GetFileSizeEx(h->host, &s);
    return (u64)s.QuadPart;
}

/* Read up to `size` bytes at `offset`; returns the count, or ~0 on a host error. */
static u64 read_at(Handle *h, i64 offset, void *buf, u64 size) {
    u64 total = file_size(h);
    if (offset < 0 || (u64)offset > total) return ~0ull;
    if (size > total - (u64)offset) size = total - (u64)offset;
    if (!size) return 0;
    int ok = h->host ? pread_all(h->host, (u64)offset, buf, size) : rom_read(h->offset + (u64)offset, buf, size);
    return ok ? size : ~0ull;
}

HLE(_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm) {   /* (handle, offset, buffer, size): all or nothing */
    Handle *h = get_handle(c->x[0]);
    if (!h) { c->x[0] = R_INVALID_HANDLE; return; }
    u64 n = read_at(h, (i64)c->x[1], P(void, c->x[2]), c->x[3]);
    c->x[0] = n == c->x[3] ? 0 : R_OUT_OF_RANGE;
}
HLE(_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm) {  /* (read*, handle, offset, buffer, size) */
    Handle *h = get_handle(c->x[1]);
    if (!h) { c->x[0] = R_INVALID_HANDLE; return; }
    u64 n = read_at(h, (i64)c->x[2], P(void, c->x[3]), c->x[4]);
    if (n == ~0ull) { c->x[0] = R_OUT_OF_RANGE; return; }
    st64(c->x[0], n);
    c->x[0] = 0;
}
HLE(_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE) {   /* (handle, offset, data, size, option) */
    Handle *h = get_handle(c->x[0]);
    if (!h || !h->host || !(h->mode & OPEN_WRITE)) { c->x[0] = R_INVALID_HANDLE; return; }
    OVERLAPPED o = {0};
    o.Offset = (DWORD)c->x[1];
    o.OffsetHigh = (DWORD)(c->x[1] >> 32);
    DWORD put = 0;
    WriteFile(h->host, P(void, c->x[2]), (DWORD)c->x[3], &put, &o);
    c->x[0] = put == (DWORD)c->x[3] ? 0 : R_OUT_OF_RANGE;
}
HLE(_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE) {
    Handle *h = get_handle(c->x[1]);
    if (!h) { c->x[0] = R_INVALID_HANDLE; return; }
    st64(c->x[0], file_size(h));
    c->x[0] = 0;
}
HLE(_ZN2nn2fs9CloseFileENS0_10FileHandleE) {
    Handle *h = get_handle(c->x[0]);
    if (!h) return;
    if (h->host) CloseHandle(h->host);
    EnterCriticalSection(&lock);
    memset(h, 0, sizeof *h);
    LeaveCriticalSection(&lock);
}
HLE(_ZN2nn2fs10CreateFileEPKcl) {   /* (path, size) */
    const char *rest;
    ready();
    struct Mount *m = find_mount(P(char, c->x[0]), &rest);
    if (!m || m->kind != M_HOST) { c->x[0] = m ? R_UNSUPPORTED : R_NOT_MOUNTED; return; }
    char path[1024];
    host_path(path, sizeof path, m, rest);
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { c->x[0] = GetLastError() == ERROR_FILE_EXISTS ? R_PATH_EXISTS : R_PATH_NOT_FOUND; return; }
    LARGE_INTEGER size;
    size.QuadPart = (i64)c->x[1];
    SetFilePointerEx(f, size, NULL, FILE_BEGIN);
    SetEndOfFile(f);
    CloseHandle(f);
    c->x[0] = 0;
}
HLE(_ZN2nn2fs10DeleteFileEPKc) {
    const char *rest;
    ready();
    struct Mount *m = find_mount(P(char, c->x[0]), &rest);
    if (!m || m->kind != M_HOST) { c->x[0] = m ? R_UNSUPPORTED : R_NOT_MOUNTED; return; }
    char path[1024];
    host_path(path, sizeof path, m, rest);
    c->x[0] = DeleteFileA(path) ? 0 : R_PATH_NOT_FOUND;
}
HLE(_ZN2nn2fs15CreateDirectoryEPKc) {
    const char *rest;
    ready();
    struct Mount *m = find_mount(P(char, c->x[0]), &rest);
    if (!m || m->kind != M_HOST) { c->x[0] = m ? R_UNSUPPORTED : R_NOT_MOUNTED; return; }
    char path[1024];
    host_path(path, sizeof path, m, rest);
    c->x[0] = CreateDirectoryA(path, NULL) ? 0 : GetLastError() == ERROR_ALREADY_EXISTS ? R_PATH_EXISTS : R_PATH_NOT_FOUND;
}
HLE(_ZN2nn2fs15DeleteDirectoryEPKc) {
    const char *rest;
    ready();
    struct Mount *m = find_mount(P(char, c->x[0]), &rest);
    if (!m || m->kind != M_HOST) { c->x[0] = m ? R_UNSUPPORTED : R_NOT_MOUNTED; return; }
    char path[1024];
    host_path(path, sizeof path, m, rest);
    c->x[0] = RemoveDirectoryA(path) ? 0 : R_PATH_NOT_FOUND;
}

/* ---- directories ----------------------------------------------------------------- */
static void listing_add(Handle *h, const char *name, size_t len, int type, u64 size) {
    h->names = realloc(h->names, sizeof *h->names * (h->count + 1));
    h->types = realloc(h->types, h->count + 1);
    h->sizes = realloc(h->sizes, sizeof *h->sizes * (h->count + 1));
    h->names[h->count] = malloc(len + 1);
    memcpy(h->names[h->count], name, len);
    h->names[h->count][len] = 0;
    h->types[h->count] = (u8)type;
    h->sizes[h->count++] = size;
}

HLE(_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci) {   /* (handle*, path, mode) */
    const char *rest;
    ready();
    struct Mount *m = find_mount(P(char, c->x[1]), &rest);
    int mode = (int)c->x[2];
    if (!m) { c->x[0] = R_NOT_MOUNTED; return; }
    Handle h = {0};
    h.kind = DIR_ENTRY;
    if (m->kind == M_ROM) {
        u32 d, f;
        if (!rom_lookup(rest, &d, &f) || f != NONE) { c->x[0] = R_PATH_NOT_FOUND; return; }
        if (mode & LIST_DIRS)
            for (u32 k = rom_dir(d)->child_dir; k != NONE; k = rom_dir(k)->sibling)
                listing_add(&h, rom_dir(k)->name, rom_dir(k)->name_len, DIR_ENTRY, 0);
        if (mode & LIST_FILES)
            for (u32 k = rom_dir(d)->child_file; k != NONE; k = rom_file(k)->sibling)
                listing_add(&h, rom_file(k)->name, rom_file(k)->name_len, FILE_ENTRY, rom_file(k)->size);
    } else {
        char path[1024];
        host_path(path, sizeof path, m, rest);
        strcat(path, "\\*");
        WIN32_FIND_DATAA fd;
        HANDLE fh = FindFirstFileA(path, &fd);
        if (fh == INVALID_HANDLE_VALUE) { c->x[0] = R_PATH_NOT_FOUND; return; }
        do {
            int dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            if (mode & (dir ? LIST_DIRS : LIST_FILES))
                listing_add(&h, fd.cFileName, strlen(fd.cFileName), dir ? DIR_ENTRY : FILE_ENTRY,
                            (u64)fd.nFileSizeHigh << 32 | fd.nFileSizeLow);
        } while (FindNextFileA(fh, &fd));
        FindClose(fh);
    }
    u64 v = new_handle(h);
    if (!v) { c->x[0] = R_TOO_MANY_HANDLES; return; }
    st64(c->x[0], v);
    c->x[0] = 0;
}
HLE(_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl) {   /* (count*, entries, handle, max) */
    Handle *h = get_handle(c->x[2]);
    if (!h) { c->x[0] = R_INVALID_HANDLE; return; }
    i64 n = 0;
    for (; n < (i64)c->x[3] && h->next < h->count; n++, h->next++) {   /* DirectoryEntry: 0x310 bytes */
        u64 e = c->x[1] + 0x310ull * (u64)n;
        memset(P(void, e), 0, 0x310);
        strncpy(P(char, e), h->names[h->next], 0x300);
        st8(e + 0x304, h->types[h->next]);
        st64(e + 0x308, h->sizes[h->next]);
    }
    st64(c->x[0], (u64)n);
    c->x[0] = 0;
}
HLE(_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE) {
    Handle *h = get_handle(c->x[1]);
    if (!h) { c->x[0] = R_INVALID_HANDLE; return; }
    st64(c->x[0], (u64)h->count);
    c->x[0] = 0;
}
HLE(_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE) {
    Handle *h = get_handle(c->x[0]);
    if (!h) return;
    for (i64 i = 0; i < h->count; i++) free(h->names[i]);
    free(h->names); free(h->types); free(h->sizes);
    EnterCriticalSection(&lock);
    memset(h, 0, sizeof *h);
    LeaveCriticalSection(&lock);
}
