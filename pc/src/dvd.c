/**
 * @file dvd.c
 * Disc access backed by a GameCube disc image (.iso/.gcm). The file system
 * table (FST) is read from the image and walked with the same algorithm as
 * the SDK, so entry numbers match the console exactly. Reads complete from
 * pc_pump() so callback chains never recurse.
 */
#include "pc_runtime.h"

#include <dolphin/dvd.h>

#include <ctype.h>
#include <direct.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdlib.h>
#include <string.h>

/* --- Disc image ---------------------------------------------------------- */

typedef struct FSTEntry {
    u32 isDirAndStringOff;
    u32 parentOrPosition;
    u32 nextEntryOrLength;
} FSTEntry;

static FILE* disc;
static u8 disc_header[0x440];
static FSTEntry* fst;
static char* fst_strings;
static u32 max_entry;
static u32 current_dir;

#define entryIsDir(i) (((fst[i].isDirAndStringOff & 0xff000000) == 0) ? 0 : 1)
#define stringOff(i) (fst[i].isDirAndStringOff & ~0xff000000)
#define parentDir(i) (fst[i].parentOrPosition)
#define nextDir(i) (fst[i].nextEntryOrLength)
#define filePosition(i) (fst[i].parentOrPosition)
#define fileLength(i) (fst[i].nextEntryOrLength)

static u32 be32(const u8* p)
{
    return ((u32) p[0] << 24) | ((u32) p[1] << 16) | ((u32) p[2] << 8) | p[3];
}

extern void pc_gx_texture_changed(const void* addr, u32 bytes);
static void apply_mods(void);

static void disc_read(u64 offset, void* dst, u32 length)
{
    if (_fseeki64(disc, (long long) offset, SEEK_SET) != 0 || fread(dst, 1, length, disc) != length) {
        fprintf(stderr, "[pc] DVD: short read at 0x%llx (%u bytes)\n", (unsigned long long) offset,
                length);
        pc_exit(6);
    }
    /* the game reloads some textures into the same buffer (character
     * portraits on the select screens): drop cached uploads of them */
    pc_gx_texture_changed(dst, length);
}

static const char* find_default_iso(void)
{
    static const char* candidates[] = {
        "GALE01.iso", "../GALE01.iso", "orig/GALE01.iso", "melee.iso", "../melee.iso", NULL,
    };
    const char* env = getenv("MELEE_ISO");
    int i;
    if (env != NULL && env[0] != '\0') {
        return env;
    }
    for (i = 0; candidates[i] != NULL; i++) {
        FILE* f = fopen(candidates[i], "rb");
        if (f != NULL) {
            fclose(f);
            return candidates[i];
        }
    }
    return NULL;
}

void pc_dvd_init(const char* path)
{
    u32 fst_offset, fst_size, i;
    if (path == NULL) {
        path = find_default_iso();
    }
    if (path == NULL) {
        fprintf(stderr,
                "[pc] No disc image found. Pass --iso <path to GALE01.iso> or set MELEE_ISO.\n");
        exit(2);
    }
    disc = fopen(path, "rb");
    if (disc == NULL) {
        fprintf(stderr, "[pc] Cannot open disc image %s\n", path);
        exit(2);
    }
    disc_read(0, disc_header, sizeof(disc_header));
    if (memcmp(disc_header, "GALE01", 6) != 0) {
        fprintf(stderr, "[pc] %s is not a GALE01 (Melee NTSC) disc image (id %.6s)\n", path,
                disc_header);
        exit(2);
    }
    fst_offset = be32(disc_header + 0x424);
    fst_size = be32(disc_header + 0x428);
    fst = (FSTEntry*) malloc(fst_size);
    disc_read(fst_offset, fst, fst_size);
    max_entry = be32((u8*) fst + 8);
    for (i = 0; i < max_entry; i++) {
        fst[i].isDirAndStringOff = be32((u8*) &fst[i].isDirAndStringOff);
        fst[i].parentOrPosition = be32((u8*) &fst[i].parentOrPosition);
        fst[i].nextEntryOrLength = be32((u8*) &fst[i].nextEntryOrLength);
    }
    fst_strings = (char*) fst + max_entry * sizeof(FSTEntry);
    current_dir = 0;
    fprintf(stderr, "[pc] DVD: %s (%.32s), %u FST entries\n", path, disc_header + 0x20, max_entry);
    apply_mods();
}

/* --- Extraction ------------------------------------------------------------
 * Dumps every file of the disc into a directory tree mirroring the disc,
 * plus sys/ (boot header, FST). Used for modding and for inspecting assets.
 */
static void mkdir_p(const char* path)
{
    char tmp[1024];
    size_t i;
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (i = 1; tmp[i] != '\0'; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            tmp[i] = '\0';
            _mkdir(tmp);
            tmp[i] = '/';
        }
    }
    _mkdir(tmp);
}

static void write_file(const char* path, u64 offset, u32 length)
{
    static u8 buf[1 << 20];
    FILE* out = fopen(path, "wb");
    if (out == NULL) {
        fprintf(stderr, "[pc] extract: cannot create %s\n", path);
        exit(2);
    }
    while (length > 0) {
        u32 chunk = length < sizeof(buf) ? length : (u32) sizeof(buf);
        disc_read(offset, buf, chunk);
        fwrite(buf, 1, chunk, out);
        offset += chunk;
        length -= chunk;
    }
    fclose(out);
}

static void extract_dir(u32 dir, const char* out_dir)
{
    u32 i = dir + 1;
    while (i < nextDir(dir)) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", out_dir, fst_strings + stringOff(i));
        if (entryIsDir(i)) {
            mkdir_p(path);
            extract_dir(i, path);
            i = nextDir(i);
        } else {
            write_file(path, filePosition(i), fileLength(i));
            i++;
        }
    }
}

void pc_dvd_extract(const char* out_dir)
{
    char path[1024];
    u32 fst_offset = be32(disc_header + 0x424);
    u32 fst_size = be32(disc_header + 0x428);
    u32 apploader_size;
    u8 hdr[0x20];

    snprintf(path, sizeof(path), "%s/sys", out_dir);
    mkdir_p(path);
    snprintf(path, sizeof(path), "%s/sys/boot.bin", out_dir);
    write_file(path, 0, 0x440);
    snprintf(path, sizeof(path), "%s/sys/bi2.bin", out_dir);
    write_file(path, 0x440, 0x2000);
    disc_read(0x2440, hdr, sizeof(hdr));
    apploader_size = be32(hdr + 0x14) + be32(hdr + 0x18);
    snprintf(path, sizeof(path), "%s/sys/apploader.img", out_dir);
    write_file(path, 0x2440, 0x20 + apploader_size);
    snprintf(path, sizeof(path), "%s/sys/fst.bin", out_dir);
    write_file(path, fst_offset, fst_size);
    {
        u32 dol_offset = be32(disc_header + 0x420);
        u8 dol[0x100];
        u32 i, end = 0;
        disc_read(dol_offset, dol, sizeof(dol));
        for (i = 0; i < 18; i++) { /* 7 text + 11 data sections */
            u32 off = be32(dol + i * 4), size = be32(dol + 0x90 + i * 4);
            if (off + size > end) {
                end = off + size;
            }
        }
        snprintf(path, sizeof(path), "%s/sys/main.dol", out_dir);
        write_file(path, dol_offset, end);
    }
    snprintf(path, sizeof(path), "%s/files", out_dir);
    mkdir_p(path);
    extract_dir(0, path);
    fprintf(stderr, "[pc] extracted %u entries to %s\n", max_entry, out_dir);
}

/* --- Loose-file overrides (mods) ------------------------------------------
 * A mod directory mirrors the disc: DIR/files/<disc path> (what --extract
 * writes) or DIR/<disc path>. Every disc file that exists in a mod is read
 * from the mod instead, with the mod file's size; the first mod given wins.
 * Files the disc does not have cannot be added (the game finds files through
 * the disc's own table). */

typedef struct Override {
    u32 entry;
    u32 start; /* the file's disc address, the key reads are matched on */
    u32 length;
    char* path;
} Override;

static Override* overrides;
static u32 n_overrides, cap_overrides;
static const char* mod_dirs[64];
static int n_mod_dirs;

void pc_dvd_add_mod(const char* dir)
{
    if (n_mod_dirs < (int) (sizeof(mod_dirs) / sizeof(mod_dirs[0]))) {
        mod_dirs[n_mod_dirs++] = _strdup(dir);
    }
}

/* MELEE_TRACE_DVD=1 logs every read with its source */
static int trace_dvd(void)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("MELEE_TRACE_DVD") != NULL;
    }
    return on;
}

static Override* find_override(u32 entry)
{
    u32 i;
    for (i = 0; i < n_overrides; i++) {
        if (overrides[i].entry == entry) {
            return &overrides[i];
        }
    }
    return NULL;
}

static u32 scan_mod_dir(const char* root, u32 dir, const char* rel)
{
    u32 i = dir + 1, added = 0;
    while (i < nextDir(dir)) {
        char sub[1024];
        const char* name = fst_strings + stringOff(i);
        if (rel[0] != '\0') {
            snprintf(sub, sizeof(sub), "%s/%s", rel, name);
        } else {
            snprintf(sub, sizeof(sub), "%s", name);
        }
        if (entryIsDir(i)) {
            added += scan_mod_dir(root, i, sub);
            i = nextDir(i);
        } else {
            char path[1024];
            struct _stat64 st;
            snprintf(path, sizeof(path), "%s/%s", root, sub);
            if (_stat64(path, &st) == 0 && (st.st_mode & _S_IFREG) && find_override(i) == NULL) {
                Override* o;
                if (n_overrides == cap_overrides) {
                    cap_overrides = cap_overrides ? cap_overrides * 2 : 64;
                    overrides = (Override*) realloc(overrides, cap_overrides * sizeof(Override));
                }
                o = &overrides[n_overrides++];
                o->entry = i;
                o->start = filePosition(i);
                o->length = (u32) st.st_size;
                o->path = _strdup(path);
                fileLength(i) = o->length; /* what DVDFastOpen reports */
                added++;
            }
            i++;
        }
    }
    return added;
}

static void apply_mods(void)
{
    int m;
    for (m = 0; m < n_mod_dirs; m++) {
        char files[1024];
        struct _stat64 st;
        const char* root = mod_dirs[m];
        u32 added;
        snprintf(files, sizeof(files), "%s/files", root);
        if (_stat64(files, &st) == 0 && (st.st_mode & _S_IFDIR)) {
            root = files;
        } else if (_stat64(mod_dirs[m], &st) != 0 || !(st.st_mode & _S_IFDIR)) {
            fprintf(stderr, "[pc] mod: %s is not a directory\n", mod_dirs[m]);
            continue;
        }
        added = scan_mod_dir(root, 0, "");
        fprintf(stderr, "[pc] mod: %s (%u file%s)\n", mod_dirs[m], added, added == 1 ? "" : "s");
    }
}

/* Reads from an override's host file; missing bytes read as zero. */
static void override_read(const Override* o, u32 offset, void* dst, u32 length)
{
    FILE* f = fopen(o->path, "rb");
    size_t got = 0;
    if (f != NULL) {
        if (_fseeki64(f, offset, SEEK_SET) == 0) {
            got = fread(dst, 1, length, f);
        }
        fclose(f);
    }
    if (got < length) {
        memset((u8*) dst + got, 0, length - got);
    }
    pc_gx_texture_changed(dst, length);
}

/* --- Path lookup (ported from extern/dolphin/src/dolphin/dvd/dvdfs.c) --- */

static BOOL isSame(const char* path, const char* string)
{
    while (*string != '\0') {
        if (tolower((unsigned char) *path++) != tolower((unsigned char) *string++)) {
            return 0;
        }
    }
    return (*path == '/' || *path == '\0') ? 1 : 0;
}

s32 DVDConvertPathToEntrynum(const char* pathPtr)
{
    const char* ptr;
    BOOL isDir;
    u32 length;
    u32 dirLookAt = current_dir;
    u32 i;

    if (pathPtr == NULL) {
        return -1;
    }
    for (;;) {
        if (*pathPtr == '\0') {
            return (s32) dirLookAt;
        } else if (*pathPtr == '/') {
            dirLookAt = 0;
            pathPtr++;
            continue;
        } else if (*pathPtr == '.') {
            if (pathPtr[1] == '.') {
                if (pathPtr[2] == '/') {
                    dirLookAt = parentDir(dirLookAt);
                    pathPtr += 3;
                    continue;
                } else if (pathPtr[2] == '\0') {
                    return (s32) parentDir(dirLookAt);
                }
            } else if (pathPtr[1] == '/') {
                pathPtr += 2;
                continue;
            } else if (pathPtr[1] == '\0') {
                return (s32) dirLookAt;
            }
        }
        for (ptr = pathPtr; *ptr != '\0' && *ptr != '/'; ptr++) {
        }
        isDir = (*ptr == '\0') ? 0 : 1;
        length = (u32) (ptr - pathPtr);

        for (i = dirLookAt + 1; i < nextDir(dirLookAt); i = entryIsDir(i) ? nextDir(i) : i + 1) {
            if (!entryIsDir(i) && isDir) {
                continue;
            }
            if (isSame(pathPtr, fst_strings + stringOff(i))) {
                break;
            }
        }
        if (i >= nextDir(dirLookAt)) {
            return -1;
        }
        if (!isDir) {
            return (s32) i;
        }
        dirLookAt = i;
        pathPtr += length + 1;
    }
}

/* --- Files ---------------------------------------------------------------- */

void DVDInit(void) {}

BOOL DVDCheckDisk(void)
{
    return 1;
}

struct DVDDiskID* DVDGetCurrentDiskID(void)
{
    return (struct DVDDiskID*) disc_header;
}

BOOL DVDFastOpen(s32 entrynum, DVDFileInfo* fileInfo)
{
    if (entrynum < 0 || (u32) entrynum >= max_entry || entryIsDir(entrynum)) {
        return 0;
    }
    fileInfo->startAddr = filePosition(entrynum);
    fileInfo->length = fileLength(entrynum);
    fileInfo->callback = NULL;
    fileInfo->cb.state = DVD_STATE_END;
    if (trace_dvd()) {
        const Override* o = find_override((u32) entrynum);
        fprintf(stderr, "[dvd] open %s (%u bytes)%s%c", fst_strings + stringOff(entrynum), fileInfo->length,
                o ? " from mod" : "", 10);
    }
    return 1;
}

BOOL DVDOpen(char* fileName, DVDFileInfo* fileInfo)
{
    s32 entry = DVDConvertPathToEntrynum(fileName);
    if (entry < 0) {
        fprintf(stderr, "[pc] DVDOpen: file '%s' was not found\n", fileName);
        return 0;
    }
    return DVDFastOpen(entry, fileInfo);
}

BOOL DVDClose(DVDFileInfo* fileInfo)
{
    (void) fileInfo;
    return 1;
}

/* Completed reads waiting for their callback. */
typedef struct PendingRead {
    DVDFileInfo* info;
    DVDCallback callback;
    s32 result;
} PendingRead;

#define MAX_PENDING 64
static PendingRead pending[MAX_PENDING];
static int pending_head, pending_count;

static s32 do_read(DVDFileInfo* fileInfo, void* addr, s32 length, s32 offset)
{
    if (offset < 0 || (u32) offset > fileInfo->length) {
        return -1;
    }
    if ((u32) (offset + length) > fileInfo->length) {
        length = (s32) (fileInfo->length - offset);
    }
    if (length > 0) {
        const Override* o = NULL;
        u32 i;
        for (i = 0; i < n_overrides; i++) {
            if (overrides[i].start == fileInfo->startAddr) {
                o = &overrides[i];
                break;
            }
        }
        if (trace_dvd()) {
            fprintf(stderr, "[dvd] read %s @%x+%d (%d bytes)%s" "%c", o ? o->path : "disc",
                    fileInfo->startAddr, offset, length, o ? " (mod)" : "", 10);
        }
        if (o != NULL) {
            override_read(o, (u32) offset, addr, (u32) length);
        } else {
            disc_read((u64) fileInfo->startAddr + (u32) offset, addr, (u32) length);
        }
    }
    return length;
}

BOOL DVDReadAsyncPrio(DVDFileInfo* fileInfo, void* addr, s32 length, s32 offset,
                      DVDCallback callback, s32 prio)
{
    s32 result;
    (void) prio;
    if (pending_count >= MAX_PENDING) {
        fprintf(stderr, "[pc] DVD: too many outstanding reads\n");
        pc_exit(6);
    }
    fileInfo->cb.state = DVD_STATE_BUSY;
    fileInfo->callback = callback;
    result = do_read(fileInfo, addr, length, offset);
    {
        PendingRead* p = &pending[(pending_head + pending_count) % MAX_PENDING];
        p->info = fileInfo;
        p->callback = callback;
        p->result = result;
        pending_count++;
    }
    return 1;
}

long DVDReadPrio(struct DVDFileInfo* fileInfo, void* addr, long length, long offset, long prio)
{
    (void) prio;
    return do_read(fileInfo, addr, (s32) length, (s32) offset);
}

bool pc_dvd_pump(void)
{
    bool ran = false;
    while (pending_count > 0) {
        PendingRead p = pending[pending_head];
        pending_head = (pending_head + 1) % MAX_PENDING;
        pending_count--;
        p.info->cb.state = DVD_STATE_END;
        if (p.callback != NULL) {
            p.callback(p.result, p.info);
        }
        ran = true;
    }
    return ran;
}

long DVDGetDriveStatus(void)
{
    pc_pump();
    return pending_count > 0 ? DVD_STATE_BUSY : DVD_STATE_END;
}
