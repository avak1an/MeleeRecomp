/**
 * @file card.c
 * Memory card API backed by files on disk. Slot A holds a virtual 64 Mbit
 * card whose files live in the saves directory (`--saves DIR`, default
 * `saves` next to the executable), one `.sav` file per card file: a small
 * host-order header carrying the directory entry (name, size, time, icon and
 * comment locations) followed by the raw data the game wrote. Slot B is
 * always empty.
 *
 * The SDK completes asynchronous requests from interrupt handlers; here the
 * callbacks are queued and delivered from pc_pump(), after the requesting
 * call has returned, which is what the game's state machines expect.
 */
#include "pc_runtime.h"

#include <dolphin/card.h>
#include <dolphin/card/CARDBios.h>
#include <dolphin/card/CARDCheck.h>
#include <dolphin/card/CARDCreate.h>
#include <dolphin/card/CARDDelete.h>
#include <dolphin/card/CARDMount.h>
#include <dolphin/card/CARDOpen.h>
#include <dolphin/card/CARDRdwr.h>
#include <dolphin/card/CARDRead.h>
#include <dolphin/card/CARDStat.h>
#include <dolphin/card/CARDWrite.h>
#include <dolphin/dvd.h>
#include <dolphin/os.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define PC_CARD_SECTOR 8192u
#define PC_CARD_BLOCKS 1019u /* a 64 Mbit card minus its five system blocks */
#define PC_CARD_MEMSIZE 64
#define PC_CARD_MAGIC "MPCS"

struct pc_card_file {
    int used;
    char name[CARD_FILENAME_MAX + 1];
    u32 length;
    u32 time;
    u8 gameName[4];
    u8 company[2];
    u8 bannerFormat;
    u32 iconAddr;
    u16 iconFormat;
    u16 iconSpeed;
    u32 commentAddr;
    u8* data;
};

struct pc_card_header {
    char magic[4];
    u32 version;
    char name[CARD_FILENAME_MAX];
    u32 length;
    u32 time;
    u8 gameName[4];
    u8 company[2];
    u8 bannerFormat;
    u8 pad;
    u32 iconAddr;
    u16 iconFormat;
    u16 iconSpeed;
    u32 commentAddr;
    u8 reserved[32];
};

static struct pc_card_file files[CARD_MAX_FILE];
static int loaded;
static int mounted;
static s32 xferred;

/* --- deferred completions ----------------------------------------------- */

static struct {
    CARDCallback cb;
    s32 result;
} queue[32];
static int queue_len;

static void defer(CARDCallback cb, s32 result)
{
    if (cb == NULL) {
        return;
    }
    if (queue_len == (int) (sizeof(queue) / sizeof(queue[0]))) {
        fprintf(stderr, "[pc] card: completion queue overflow\n");
        return;
    }
    queue[queue_len].cb = cb;
    queue[queue_len].result = result;
    queue_len++;
}

bool pc_card_pump(void)
{
    CARDCallback cbs[32];
    s32 results[32];
    int i, n = queue_len;
    if (n == 0) {
        return false;
    }
    /* callbacks may queue new requests, so drain into locals first */
    for (i = 0; i < n; i++) {
        cbs[i] = queue[i].cb;
        results[i] = queue[i].result;
    }
    queue_len = 0;
    for (i = 0; i < n; i++) {
        cbs[i](0, results[i]);
    }
    return true;
}

/* --- disk storage ------------------------------------------------------- */

/// `--saves DIR`, else a `saves` directory next to the executable.
static const char* save_dir(void)
{
    static char dir[MAX_PATH + 16];
    if (pc_config.save_dir != NULL) {
        return pc_config.save_dir;
    }
    if (dir[0] == '\0') {
        snprintf(dir, sizeof(dir), "%s/saves", pc_exe_dir());
    }
    return dir;
}

static void file_path(const char* name, char* out, size_t out_size)
{
    char safe[CARD_FILENAME_MAX + 1];
    size_t i;
    for (i = 0; i < CARD_FILENAME_MAX && name[i] != '\0'; i++) {
        char c = name[i];
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                 c == '.' || c == '_' || c == '-';
        safe[i] = ok ? c : '_';
    }
    safe[i] = '\0';
    snprintf(out, out_size, "%s/%s.sav", save_dir(), safe);
}

static void store(const struct pc_card_file* f)
{
    struct pc_card_header h;
    char path[MAX_PATH + 64];
    FILE* fp;
    CreateDirectoryA(save_dir(), NULL);
    file_path(f->name, path, sizeof(path));
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, PC_CARD_MAGIC, 4);
    h.version = 1;
    strncpy(h.name, f->name, CARD_FILENAME_MAX);
    h.length = f->length;
    h.time = f->time;
    memcpy(h.gameName, f->gameName, 4);
    memcpy(h.company, f->company, 2);
    h.bannerFormat = f->bannerFormat;
    h.iconAddr = f->iconAddr;
    h.iconFormat = f->iconFormat;
    h.iconSpeed = f->iconSpeed;
    h.commentAddr = f->commentAddr;
    fp = fopen(path, "wb");
    if (fp == NULL) {
        fprintf(stderr, "[pc] card: cannot write %s\n", path);
        return;
    }
    fwrite(&h, 1, sizeof(h), fp);
    fwrite(f->data, 1, f->length, fp);
    fclose(fp);
}

static void unlink_file(const struct pc_card_file* f)
{
    char path[MAX_PATH + 64];
    file_path(f->name, path, sizeof(path));
    DeleteFileA(path);
}

static int free_slot(void)
{
    int i;
    for (i = 0; i < CARD_MAX_FILE; i++) {
        if (!files[i].used) {
            return i;
        }
    }
    return -1;
}

static void load_one(const char* path)
{
    struct pc_card_header h;
    FILE* fp = fopen(path, "rb");
    struct pc_card_file* f;
    int slot;
    if (fp == NULL) {
        return;
    }
    if (fread(&h, 1, sizeof(h), fp) != sizeof(h) || memcmp(h.magic, PC_CARD_MAGIC, 4) != 0 ||
        h.length == 0 || h.length % PC_CARD_SECTOR != 0 || h.length > PC_CARD_BLOCKS * PC_CARD_SECTOR)
    {
        fprintf(stderr, "[pc] card: ignoring %s (not a save file)\n", path);
        fclose(fp);
        return;
    }
    slot = free_slot();
    if (slot < 0) {
        fclose(fp);
        return;
    }
    f = &files[slot];
    memset(f, 0, sizeof(*f));
    memcpy(f->name, h.name, CARD_FILENAME_MAX);
    f->name[CARD_FILENAME_MAX] = '\0';
    f->length = h.length;
    f->time = h.time;
    memcpy(f->gameName, h.gameName, 4);
    memcpy(f->company, h.company, 2);
    f->bannerFormat = h.bannerFormat;
    f->iconAddr = h.iconAddr;
    f->iconFormat = h.iconFormat;
    f->iconSpeed = h.iconSpeed;
    f->commentAddr = h.commentAddr;
    f->data = (u8*) calloc(f->length, 1);
    if (f->data == NULL) {
        fclose(fp);
        return;
    }
    fread(f->data, 1, f->length, fp);
    fclose(fp);
    f->used = 1;
}

static void load_all(void)
{
    char pattern[MAX_PATH + 16];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int count = 0;
    if (loaded) {
        return;
    }
    loaded = 1;
    snprintf(pattern, sizeof(pattern), "%s/*.sav", save_dir());
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        char path[MAX_PATH + 64];
        snprintf(path, sizeof(path), "%s/%s", save_dir(), fd.cFileName);
        load_one(path);
        count++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (count != 0 && !pc_config.quiet_stubs) {
        fprintf(stderr, "[pc] card: %d save file(s) in %s\n", count, save_dir());
    }
}

/* --- helpers ------------------------------------------------------------- */

static s32 check_chan(s32 chan)
{
    if (chan != 0) {
        return CARD_RESULT_NOCARD;
    }
    if (!mounted) {
        return CARD_RESULT_NOCARD;
    }
    return CARD_RESULT_READY;
}

static int find_file(const char* name)
{
    int i;
    for (i = 0; i < CARD_MAX_FILE; i++) {
        if (files[i].used && strncmp(files[i].name, name, CARD_FILENAME_MAX) == 0) {
            return i;
        }
    }
    return -1;
}

static u32 used_blocks(void)
{
    u32 n = 0;
    int i;
    for (i = 0; i < CARD_MAX_FILE; i++) {
        if (files[i].used) {
            n += files[i].length / PC_CARD_SECTOR;
        }
    }
    return n;
}

static int file_count(void)
{
    int n = 0, i;
    for (i = 0; i < CARD_MAX_FILE; i++) {
        n += files[i].used;
    }
    return n;
}

static u32 now_seconds(void)
{
    return (u32) (OSGetTime() / (OS_BUS_CLOCK / 4));
}

static struct pc_card_file* open_file(const CARDFileInfo* fi)
{
    if (fi == NULL || fi->chan != 0 || fi->fileNo < 0 || fi->fileNo >= CARD_MAX_FILE ||
        !files[fi->fileNo].used)
    {
        return NULL;
    }
    return &files[fi->fileNo];
}

/* --- API ----------------------------------------------------------------- */

void CARDInit(void) {}

int CARDProbe(long chan)
{
    return chan == 0;
}

s32 CARDProbeEx(s32 chan, s32* memSize, s32* sectorSize)
{
    if (chan != 0) {
        return CARD_RESULT_NOCARD;
    }
    if (memSize != NULL) {
        *memSize = PC_CARD_MEMSIZE;
    }
    if (sectorSize != NULL) {
        *sectorSize = (s32) PC_CARD_SECTOR;
    }
    return CARD_RESULT_READY;
}

s32 CARDMountAsync(s32 chan, void* workArea, CARDCallback detachCallback, CARDCallback attachCallback)
{
    (void) workArea;
    (void) detachCallback;
    if (chan != 0) {
        return CARD_RESULT_NOCARD;
    }
    load_all();
    mounted = 1;
    defer(attachCallback, CARD_RESULT_READY);
    return CARD_RESULT_READY;
}

s32 CARDUnmount(s32 chan)
{
    if (chan != 0) {
        return CARD_RESULT_NOCARD;
    }
    mounted = 0;
    return CARD_RESULT_READY;
}

s32 CARDCheckAsync(s32 chan, CARDCallback callback)
{
    s32 r = check_chan(chan);
    if (r < 0) {
        return r;
    }
    defer(callback, CARD_RESULT_READY);
    return CARD_RESULT_READY;
}

s32 CARDFormatAsync(s32 chan, CARDCallback callback)
{
    int i;
    if (chan != 0) {
        return CARD_RESULT_NOCARD;
    }
    load_all();
    for (i = 0; i < CARD_MAX_FILE; i++) {
        if (files[i].used) {
            unlink_file(&files[i]);
            free(files[i].data);
            memset(&files[i], 0, sizeof(files[i]));
        }
    }
    mounted = 1;
    defer(callback, CARD_RESULT_READY);
    return CARD_RESULT_READY;
}

s32 CARDFreeBlocks(s32 chan, s32* byteNotUsed, s32* filesNotUsed)
{
    s32 r = check_chan(chan);
    if (r < 0) {
        return r;
    }
    if (byteNotUsed != NULL) {
        *byteNotUsed = (s32) ((PC_CARD_BLOCKS - used_blocks()) * PC_CARD_SECTOR);
    }
    if (filesNotUsed != NULL) {
        *filesNotUsed = CARD_MAX_FILE - file_count();
    }
    return CARD_RESULT_READY;
}

s32 CARDCreateAsync(s32 chan, const char* fileName, u32 size, CARDFileInfo* fileInfo, CARDCallback callback)
{
    s32 r = check_chan(chan);
    const DVDDiskID* id;
    struct pc_card_file* f;
    int slot;
    if (r < 0) {
        return r;
    }
    if (size == 0 || size % PC_CARD_SECTOR != 0 || strlen(fileName) > CARD_FILENAME_MAX) {
        return CARD_RESULT_FATAL_ERROR;
    }
    if (find_file(fileName) >= 0) {
        return CARD_RESULT_EXIST;
    }
    slot = free_slot();
    if (slot < 0) {
        return CARD_RESULT_NOENT;
    }
    if (used_blocks() + size / PC_CARD_SECTOR > PC_CARD_BLOCKS) {
        return CARD_RESULT_INSSPACE;
    }
    f = &files[slot];
    memset(f, 0, sizeof(*f));
    f->data = (u8*) calloc(size, 1);
    if (f->data == NULL) {
        return CARD_RESULT_INSSPACE;
    }
    strncpy(f->name, fileName, CARD_FILENAME_MAX);
    f->length = size;
    id = DVDGetCurrentDiskID();
    memcpy(f->gameName, id->gameName, 4);
    memcpy(f->company, id->company, 2);
    f->iconAddr = 0xFFFFFFFFu;
    f->commentAddr = 0xFFFFFFFFu;
    f->time = now_seconds();
    f->used = 1;
    store(f);
    fileInfo->chan = 0;
    fileInfo->fileNo = slot;
    fileInfo->offset = 0;
    fileInfo->length = (s32) size;
    fileInfo->iBlock = 0;
    defer(callback, CARD_RESULT_READY);
    return CARD_RESULT_READY;
}

s32 CARDDeleteAsync(s32 chan, char* fileName, CARDCallback callback)
{
    s32 r = check_chan(chan);
    int i;
    if (r < 0) {
        return r;
    }
    i = find_file(fileName);
    if (i < 0) {
        return CARD_RESULT_NOFILE;
    }
    unlink_file(&files[i]);
    free(files[i].data);
    memset(&files[i], 0, sizeof(files[i]));
    defer(callback, CARD_RESULT_READY);
    return CARD_RESULT_READY;
}

s32 CARDRenameAsync(s32 chan, const char* oldName, const char* newName, CARDCallback callback)
{
    s32 r = check_chan(chan);
    int i;
    if (r < 0) {
        return r;
    }
    if (strlen(newName) > CARD_FILENAME_MAX) {
        return CARD_RESULT_NAMETOOLONG;
    }
    i = find_file(oldName);
    if (i < 0) {
        return CARD_RESULT_NOFILE;
    }
    if (find_file(newName) >= 0) {
        return CARD_RESULT_EXIST;
    }
    unlink_file(&files[i]);
    memset(files[i].name, 0, sizeof(files[i].name));
    strncpy(files[i].name, newName, CARD_FILENAME_MAX);
    files[i].time = now_seconds();
    store(&files[i]);
    defer(callback, CARD_RESULT_READY);
    return CARD_RESULT_READY;
}

s32 CARDOpen(s32 chan, char* fileName, CARDFileInfo* fileInfo)
{
    s32 r = check_chan(chan);
    int i;
    if (r < 0) {
        return r;
    }
    i = find_file(fileName);
    if (i < 0) {
        return CARD_RESULT_NOFILE;
    }
    fileInfo->chan = 0;
    fileInfo->fileNo = i;
    fileInfo->offset = 0;
    fileInfo->length = (s32) files[i].length;
    fileInfo->iBlock = 0;
    return CARD_RESULT_READY;
}

s32 CARDFastOpen(s32 chan, s32 fileNo, CARDFileInfo* fileInfo)
{
    s32 r = check_chan(chan);
    if (r < 0) {
        return r;
    }
    if (fileNo < 0 || fileNo >= CARD_MAX_FILE) {
        return CARD_RESULT_FATAL_ERROR;
    }
    if (!files[fileNo].used) {
        return CARD_RESULT_NOFILE;
    }
    fileInfo->chan = 0;
    fileInfo->fileNo = fileNo;
    fileInfo->offset = 0;
    fileInfo->length = (s32) files[fileNo].length;
    fileInfo->iBlock = 0;
    return CARD_RESULT_READY;
}

s32 CARDClose(CARDFileInfo* fileInfo)
{
    (void) fileInfo;
    return CARD_RESULT_READY;
}

static s32 transfer(CARDFileInfo* fileInfo, void* buf, s32 length, s32 offset, int write)
{
    struct pc_card_file* f;
    s32 r = check_chan(fileInfo != NULL ? fileInfo->chan : -1);
    if (r < 0) {
        return r;
    }
    f = open_file(fileInfo);
    if (f == NULL) {
        return CARD_RESULT_NOFILE;
    }
    if (length < 0 || offset < 0 || (u32) offset + (u32) length > f->length) {
        return CARD_RESULT_LIMIT;
    }
    if (write) {
        memcpy(f->data + offset, buf, (size_t) length);
        store(f);
    } else {
        memcpy(buf, f->data + offset, (size_t) length);
    }
    fileInfo->offset = offset + length;
    xferred = length;
    return CARD_RESULT_READY;
}

long CARDRead(struct CARDFileInfo* fileInfo, void* buf, long length, long offset)
{
    return transfer(fileInfo, buf, (s32) length, (s32) offset, 0);
}

s32 CARDReadAsync(CARDFileInfo* fileInfo, void* buf, s32 length, s32 offset, CARDCallback callback)
{
    s32 r = transfer(fileInfo, buf, length, offset, 0);
    if (r == CARD_RESULT_READY) {
        defer(callback, CARD_RESULT_READY);
    }
    return r;
}

long CARDWrite(struct CARDFileInfo* fileInfo, void* buf, long length, long offset)
{
    return transfer(fileInfo, buf, (s32) length, (s32) offset, 1);
}

long CARDWriteAsync(struct CARDFileInfo* fileInfo, void* buf, long length, long offset,
                    CARDCallback callback)
{
    s32 r = transfer(fileInfo, buf, (s32) length, (s32) offset, 1);
    if (r == CARD_RESULT_READY) {
        defer(callback, CARD_RESULT_READY);
    }
    return r;
}

long CARDGetXferredBytes(long chan)
{
    (void) chan;
    return xferred;
}

/* Mirrors the SDK's UpdateIconOffsets: where the banner, icons and data
 * start inside the file, derived from the directory entry. */
static void update_icon_offsets(const struct pc_card_file* f, CARDStat* stat)
{
    u32 offset = f->iconAddr;
    int icon_tlut = 0, i;
    if (offset == 0xFFFFFFFFu) {
        stat->bannerFormat = 0;
        stat->iconFormat = 0;
        stat->iconSpeed = 0;
        offset = 0;
    }
    switch (stat->bannerFormat & CARD_STAT_BANNER_MASK) {
    case CARD_STAT_BANNER_C8:
        stat->offsetBanner = offset;
        offset += CARD_BANNER_WIDTH * CARD_BANNER_HEIGHT;
        stat->offsetBannerTlut = offset;
        offset += 2 * 256;
        break;
    case CARD_STAT_BANNER_RGB5A3:
        stat->offsetBanner = offset;
        offset += 2 * CARD_BANNER_WIDTH * CARD_BANNER_HEIGHT;
        stat->offsetBannerTlut = 0xFFFFFFFFu;
        break;
    default:
        stat->offsetBanner = 0xFFFFFFFFu;
        stat->offsetBannerTlut = 0xFFFFFFFFu;
        break;
    }
    for (i = 0; i < CARD_ICON_MAX; i++) {
        switch ((stat->iconFormat >> (2 * i)) & CARD_STAT_ICON_MASK) {
        case CARD_STAT_ICON_C8:
            stat->offsetIcon[i] = offset;
            offset += CARD_ICON_WIDTH * CARD_ICON_HEIGHT;
            icon_tlut = 1;
            break;
        case CARD_STAT_ICON_RGB5A3:
            stat->offsetIcon[i] = offset;
            offset += 2 * CARD_ICON_WIDTH * CARD_ICON_HEIGHT;
            break;
        default:
            stat->offsetIcon[i] = 0xFFFFFFFFu;
            break;
        }
    }
    if (icon_tlut) {
        stat->offsetIconTlut = offset;
        offset += 2 * 256;
    } else {
        stat->offsetIconTlut = 0xFFFFFFFFu;
    }
    stat->offsetData = offset;
}

s32 CARDGetStatus(s32 chan, s32 fileNo, CARDStat* stat)
{
    s32 r = check_chan(chan);
    const struct pc_card_file* f;
    if (r < 0) {
        return r;
    }
    if (fileNo < 0 || fileNo >= CARD_MAX_FILE) {
        return CARD_RESULT_FATAL_ERROR;
    }
    f = &files[fileNo];
    if (!f->used) {
        return CARD_RESULT_NOFILE;
    }
    memset(stat, 0, sizeof(*stat));
    memcpy(stat->fileName, f->name, CARD_FILENAME_MAX);
    stat->length = f->length;
    stat->time = f->time;
    memcpy(stat->gameName, f->gameName, 4);
    memcpy(stat->company, f->company, 2);
    stat->bannerFormat = f->bannerFormat;
    stat->iconAddr = f->iconAddr;
    stat->iconFormat = f->iconFormat;
    stat->iconSpeed = f->iconSpeed;
    stat->commentAddr = f->commentAddr;
    update_icon_offsets(f, stat);
    return CARD_RESULT_READY;
}

s32 CARDSetStatusAsync(s32 chan, s32 fileNo, CARDStat* stat, CARDCallback callback)
{
    s32 r = check_chan(chan);
    struct pc_card_file* f;
    if (r < 0) {
        return r;
    }
    if (fileNo < 0 || fileNo >= CARD_MAX_FILE) {
        return CARD_RESULT_FATAL_ERROR;
    }
    f = &files[fileNo];
    if (!f->used) {
        return CARD_RESULT_NOFILE;
    }
    f->bannerFormat = stat->bannerFormat;
    f->iconAddr = stat->iconAddr;
    f->iconFormat = stat->iconFormat;
    f->iconSpeed = stat->iconSpeed;
    f->commentAddr = stat->commentAddr;
    if (f->iconAddr == 0xFFFFFFFFu) {
        f->iconSpeed = (u16) ((f->iconSpeed & ~CARD_STAT_SPEED_MASK) | CARD_STAT_SPEED_FAST);
    }
    f->time = now_seconds();
    update_icon_offsets(f, stat);
    store(f);
    defer(callback, CARD_RESULT_READY);
    return CARD_RESULT_READY;
}
