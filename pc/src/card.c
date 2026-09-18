/**
 * @file card.c
 * Memory card API backed by a card image in the console's own format.
 *
 * Slot A is a 64 Mbit card kept as `MemoryCardA.USA.raw` in the saves
 * directory (`--saves DIR`, default `saves` next to the executable): the
 * same layout Dolphin and a real GameCube use (a header block, two copies
 * of the directory, two copies of the block allocation table, then 8 KB
 * data blocks), all big-endian, so the file can be opened in Dolphin's
 * memory card manager or copied into its card folder as it is, and a card
 * from Dolphin can be dropped here. `.gci` files found in the saves
 * directory are imported into the image once (and renamed
 * `.gci.imported`). Slot B is always empty.
 *
 * The data blocks hold exactly what the game writes through CARDWrite;
 * save_swap.c keeps the payloads in console byte order.
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
#include <time.h>
#include <windows.h>

#define BLOCK 8192u
#define CARD_MBIT 64u
#define TOTAL_BLOCKS (CARD_MBIT * 16u)                    /* 1024 */
#define DATA_BLOCKS (TOTAL_BLOCKS - CARD_NUM_SYSTEM_BLOCK) /* 1019 */
#define IMAGE_BYTES (TOTAL_BLOCKS * BLOCK)                 /* 8 MB */
#define IMAGE_NAME "MemoryCardA.USA.raw"

/* byte offsets inside the system blocks */
#define HDR_SIZE_MB 0x22
#define HDR_ENCODING 0x24
#define HDR_CHECKSUM 0x1FC
#define DIR_ENTRIES 127
#define DIR_CHECKCODE 0x1FFA
#define DIR_CHECKSUM 0x1FFC
#define BAT_CHECKSUM 0
#define BAT_CHECKCODE 4
#define BAT_FREE 6
#define BAT_LASTSLOT 8
#define BAT_MAP 10
#define FAT_LAST 0xFFFFu

static int trace = -1; /* MELEE_TRACE_CARD=1: every API call */
#define TRACE(...) do { if (trace < 0) trace = getenv("MELEE_TRACE_CARD") != NULL; if (trace) { fprintf(stderr, "[pc] card: " __VA_ARGS__); fputc(10, stderr); } } while (0)
static u8* image;   /* the whole card */
static u32 mem_writes, disk_writes; /* see pc_card_after_restore */
static int loaded;  /* image read or formatted */
static int mounted;
static s32 xferred;
static char image_path_buf[MAX_PATH + 64];

/* --- deferred completions ----------------------------------------------- */

static struct {
    CARDCallback cb;
    s32 result;
} queue[32];
static int queue_len;

static void defer(CARDCallback cb, s32 result)
{
    if (cb == NULL || queue_len == 32) {
        return;
    }
    queue[queue_len].cb = cb;
    queue[queue_len].result = result;
    queue_len++;
}

/// Delivers queued completions; returns whether any ran.
bool pc_card_pump(void)
{
    int n = queue_len, i;
    CARDCallback cbs[32];
    s32 results[32];
    if (n == 0) {
        return false;
    }
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

/* --- big-endian access ---------------------------------------------------- */

static u16 rd16(const u8* p)
{
    return (u16) ((p[0] << 8) | p[1]);
}

static u32 rd32(const u8* p)
{
    return ((u32) p[0] << 24) | ((u32) p[1] << 16) | ((u32) p[2] << 8) | p[3];
}

static void wr16(u8* p, u16 v)
{
    p[0] = (u8) (v >> 8);
    p[1] = (u8) v;
}

static void wr32(u8* p, u32 v)
{
    p[0] = (u8) (v >> 24);
    p[1] = (u8) (v >> 16);
    p[2] = (u8) (v >> 8);
    p[3] = (u8) v;
}

static u8* block(u32 n)
{
    return image + (size_t) n * BLOCK;
}

/* The SDK's __CARDCheckSum: sums of the 16-bit words and of their
 * complements, 0xFFFF folded to 0. */
static void checksum(const u8* p, u32 bytes, u16* sum, u16* inv)
{
    u32 i, s = 0, v = 0;
    for (i = 0; i + 1 < bytes; i += 2) {
        u16 w = rd16(p + i);
        s += w;
        v += (u16) ~w;
    }
    *sum = (u16) s;
    *inv = (u16) v;
    if (*sum == 0xFFFF) {
        *sum = 0;
    }
    if (*inv == 0xFFFF) {
        *inv = 0;
    }
}

static int header_ok(void)
{
    u16 sum, inv;
    checksum(block(0), HDR_CHECKSUM, &sum, &inv);
    return rd16(block(0) + HDR_CHECKSUM) == sum && rd16(block(0) + HDR_CHECKSUM + 2) == inv;
}

static int dir_ok(u32 b)
{
    u16 sum, inv;
    checksum(block(b), DIR_CHECKSUM, &sum, &inv);
    return rd16(block(b) + DIR_CHECKSUM) == sum && rd16(block(b) + DIR_CHECKSUM + 2) == inv;
}

static int bat_ok(u32 b)
{
    u16 sum, inv;
    checksum(block(b) + BAT_CHECKCODE, BLOCK - BAT_CHECKCODE, &sum, &inv);
    return rd16(block(b) + BAT_CHECKSUM) == sum && rd16(block(b) + BAT_CHECKSUM + 2) == inv;
}

/* The live copies: chosen once when the image is loaded (the one with the
 * newer counter whose checksum holds), then modified in place and sealed
 * by commit_system(), which also refreshes the other copy. */
static u32 cur_dir = 1, cur_bat = 3;

void pc_card_register_state(void)
{
    pc_state_register(queue, sizeof(queue), "card queue");
    pc_state_register(&queue_len, sizeof(queue_len), "card queue len");
    pc_state_register(&mounted, sizeof(mounted), "card mounted");
    pc_state_register(&xferred, sizeof(xferred), "card xferred");
    pc_state_register(&cur_dir, sizeof(cur_dir), "card dir copy");
    pc_state_register(&cur_bat, sizeof(cur_bat), "card bat copy");
    pc_state_register(&mem_writes, sizeof(mem_writes), "card writes");
}

static void choose_copies(void)
{
    int ok1 = dir_ok(1), ok2 = dir_ok(2), ok3 = bat_ok(3), ok4 = bat_ok(4);
    if (ok1 && ok2) {
        cur_dir = (s16) (rd16(block(2) + DIR_CHECKCODE) - rd16(block(1) + DIR_CHECKCODE)) > 0 ? 2 : 1;
    } else {
        cur_dir = ok2 ? 2 : 1;
    }
    if (ok3 && ok4) {
        cur_bat = (s16) (rd16(block(4) + BAT_CHECKCODE) - rd16(block(3) + BAT_CHECKCODE)) > 0 ? 4 : 3;
    } else {
        cur_bat = ok4 ? 4 : 3;
    }
}

static u32 dir_block(void)
{
    return cur_dir;
}

static u32 bat_block(void)
{
    return cur_bat;
}

static u8* dir_entry(u32 i)
{
    return block(dir_block()) + i * 64;
}

static u16 fat_get(u32 blk)
{
    return rd16(block(bat_block()) + BAT_MAP + (blk - CARD_NUM_SYSTEM_BLOCK) * 2);
}

/* --- the image on disk ----------------------------------------------------- */

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

static const char* image_path(void)
{
    if (image_path_buf[0] == '\0') {
        snprintf(image_path_buf, sizeof(image_path_buf), "%s/%s", save_dir(), IMAGE_NAME);
    }
    return image_path_buf;
}

/* Writes are counted twice: mem_writes is part of the game's state and
 * rolls back with it, disk_writes is what the file has seen. When a restore
 * leaves the two apart, the file holds blocks from the undone frames and is
 * rewritten whole (pc_card_after_restore). */
void pc_card_after_restore(void)
{
    if (mem_writes != disk_writes && image != NULL && loaded) {
        FILE* fp = fopen(image_path(), "wb");
        if (fp != NULL) {
            fwrite(image, 1, IMAGE_BYTES, fp);
            fclose(fp);
        }
        disk_writes = mem_writes;
    }
}

static void write_range(u32 first_block, u32 count)
{
    FILE* fp;
    mem_writes++;
    disk_writes++;
    CreateDirectoryA(save_dir(), NULL);
    fp = fopen(image_path(), "r+b");
    if (fp == NULL) {
        fp = fopen(image_path(), "wb");
        if (fp == NULL) {
            fprintf(stderr, "[pc] card: cannot write %s\n", image_path());
            return;
        }
        fwrite(image, 1, IMAGE_BYTES, fp);
        fclose(fp);
        return;
    }
    fseek(fp, (long) (first_block * BLOCK), SEEK_SET);
    fwrite(block(first_block), 1, (size_t) count * BLOCK, fp);
    fclose(fp);
}

/* Seal the system blocks after a change: both copies of the directory and
 * of the allocation table get the bumped counter and fresh checksums. */
static void commit_dir(void)
{
    u16 sum, inv;
    u32 src_dir = dir_block();
    u16 code = (u16) (rd16(block(src_dir) + DIR_CHECKCODE) + 1);
    wr16(block(src_dir) + DIR_CHECKCODE, code);
    checksum(block(src_dir), DIR_CHECKSUM, &sum, &inv);
    wr16(block(src_dir) + DIR_CHECKSUM, sum);
    wr16(block(src_dir) + DIR_CHECKSUM + 2, inv);
    memcpy(block(src_dir == 1 ? 2 : 1), block(src_dir), BLOCK);
    write_range(1, 2);
}

static void commit_system(void)
{
    u16 sum, inv;
    u32 src_bat = bat_block();
    u16 code;
    commit_dir();
    /* allocation table */
    code = (u16) (rd16(block(src_bat) + BAT_CHECKCODE) + 1);
    wr16(block(src_bat) + BAT_CHECKCODE, code);
    checksum(block(src_bat) + BAT_CHECKCODE, BLOCK - BAT_CHECKCODE, &sum, &inv);
    wr16(block(src_bat) + BAT_CHECKSUM, sum);
    wr16(block(src_bat) + BAT_CHECKSUM + 2, inv);
    memcpy(block(src_bat == 3 ? 4 : 3), block(src_bat), BLOCK);
    write_range(0, CARD_NUM_SYSTEM_BLOCK);
}

static u32 now_seconds(void)
{
    /* seconds since 2000-01-01 00:00 UTC, as the console counts: from the
     * game's clock (OSGetTime), so a seeded run stamps the same times every
     * time and the game's state stays deterministic */
    return (u32) OSTicksToSeconds(OSGetTime());
}

static void format_image(void)
{
    u8* h = block(0);
    u16 sum, inv;
    u32 i;
    u64 t = (u64) now_seconds() * (OS_BUS_CLOCK / 4);
    memset(image, 0, IMAGE_BYTES);
    /* header: serial from the format time, size, ASCII encoding */
    memset(h, 0xFF, BLOCK);
    for (i = 0; i < 12; i++) {
        h[i] = (u8) ((t >> ((i % 8) * 8)) ^ (0x5A + i * 17));
    }
    for (i = 0; i < 8; i++) {
        h[12 + i] = (u8) (t >> (56 - i * 8));
    }
    wr32(h + 20, 0);
    wr32(h + 24, 0);
    wr32(h + 28, 0);
    wr16(h + 0x20, 0);
    wr16(h + HDR_SIZE_MB, CARD_MBIT);
    wr16(h + HDR_ENCODING, 0);
    checksum(h, HDR_CHECKSUM, &sum, &inv);
    wr16(h + HDR_CHECKSUM, sum);
    wr16(h + HDR_CHECKSUM + 2, inv);
    /* directories: empty entries are 0xFF */
    memset(block(1), 0xFF, BLOCK);
    wr16(block(1) + DIR_CHECKCODE, 0);
    checksum(block(1), DIR_CHECKSUM, &sum, &inv);
    wr16(block(1) + DIR_CHECKSUM, sum);
    wr16(block(1) + DIR_CHECKSUM + 2, inv);
    memcpy(block(2), block(1), BLOCK);
    /* allocation tables: all data blocks free */
    memset(block(3), 0, BLOCK);
    wr16(block(3) + BAT_CHECKCODE, 0);
    wr16(block(3) + BAT_FREE, DATA_BLOCKS);
    wr16(block(3) + BAT_LASTSLOT, CARD_NUM_SYSTEM_BLOCK - 1);
    checksum(block(3) + BAT_CHECKCODE, BLOCK - BAT_CHECKCODE, &sum, &inv);
    wr16(block(3) + BAT_CHECKSUM, sum);
    wr16(block(3) + BAT_CHECKSUM + 2, inv);
    memcpy(block(4), block(3), BLOCK);
    cur_dir = 1;
    cur_bat = 3;
}

static int entry_used(u32 i)
{
    return dir_entry(i)[0] != 0xFF;
}

static u32 entry_blocks(u32 i)
{
    return rd16(dir_entry(i) + 0x38);
}

static int find_file(const char* name)
{
    u32 i;
    for (i = 0; i < DIR_ENTRIES; i++) {
        if (entry_used(i) && strncmp((const char*) dir_entry(i) + 8, name, CARD_FILENAME_MAX) == 0) {
            return (int) i;
        }
    }
    return -1;
}

static u32 free_blocks(void)
{
    return rd16(block(bat_block()) + BAT_FREE);
}

static int file_count(void)
{
    int n = 0;
    u32 i;
    for (i = 0; i < DIR_ENTRIES; i++) {
        n += entry_used(i);
    }
    return n;
}

/* Allocate `count` data blocks as a chain; returns the first block or 0. */
static u32 alloc_chain(u32 count)
{
    u8* bat = block(bat_block());
    u32 first = 0, prev = 0, got = 0, b;
    if (count == 0 || count > free_blocks()) {
        return 0;
    }
    for (b = CARD_NUM_SYSTEM_BLOCK; b < TOTAL_BLOCKS && got < count; b++) {
        if (rd16(bat + BAT_MAP + (b - CARD_NUM_SYSTEM_BLOCK) * 2) != 0) {
            continue;
        }
        if (prev == 0) {
            first = b;
        } else {
            wr16(bat + BAT_MAP + (prev - CARD_NUM_SYSTEM_BLOCK) * 2, (u16) b);
        }
        wr16(bat + BAT_MAP + (b - CARD_NUM_SYSTEM_BLOCK) * 2, FAT_LAST);
        prev = b;
        got++;
    }
    wr16(bat + BAT_FREE, (u16) (free_blocks() - count));
    wr16(bat + BAT_LASTSLOT, (u16) prev);
    return first;
}

static void free_chain(u32 first)
{
    u8* bat = block(bat_block());
    u32 b = first, n = 0;
    while (b >= CARD_NUM_SYSTEM_BLOCK && b < TOTAL_BLOCKS && n < DATA_BLOCKS) {
        u32 next = rd16(bat + BAT_MAP + (b - CARD_NUM_SYSTEM_BLOCK) * 2);
        wr16(bat + BAT_MAP + (b - CARD_NUM_SYSTEM_BLOCK) * 2, 0);
        n++;
        if (next == FAT_LAST || next == 0) {
            break;
        }
        b = next;
    }
    wr16(bat + BAT_FREE, (u16) (free_blocks() + n));
}

/* the physical block holding logical block `index` of a file */
static u32 file_block(u32 entry, u32 index)
{
    u32 b = rd16(dir_entry(entry) + 0x36), i;
    for (i = 0; i < index; i++) {
        b = fat_get(b);
        if (b == FAT_LAST || b < CARD_NUM_SYSTEM_BLOCK || b >= TOTAL_BLOCKS) {
            return 0;
        }
    }
    return b >= CARD_NUM_SYSTEM_BLOCK && b < TOTAL_BLOCKS ? b : 0;
}

/* --- GCI import ------------------------------------------------------------ */

static void import_gci(const char* path)
{
    FILE* fp = fopen(path, "rb");
    u8 hdr[64];
    u32 blocks, first, i;
    char name[CARD_FILENAME_MAX + 1];
    char renamed[MAX_PATH + 32];
    int slot = -1;
    if (fp == NULL) {
        return;
    }
    if (fread(hdr, 1, 64, fp) != 64) {
        fclose(fp);
        return;
    }
    memcpy(name, hdr + 8, CARD_FILENAME_MAX);
    name[CARD_FILENAME_MAX] = '\0';
    blocks = rd16(hdr + 0x38);
    if (blocks == 0 || blocks > DATA_BLOCKS) {
        fclose(fp);
        return;
    }
    if (find_file(name) >= 0) {
        fprintf(stderr, "[pc] card: %s not imported, the card already has \"%s\"\n", path, name);
        fclose(fp);
        return;
    }
    for (i = 0; i < DIR_ENTRIES; i++) {
        if (!entry_used(i)) {
            slot = (int) i;
            break;
        }
    }
    if (slot < 0 || blocks > free_blocks()) {
        fprintf(stderr, "[pc] card: no room to import %s\n", path);
        fclose(fp);
        return;
    }
    first = alloc_chain(blocks);
    if (first == 0) {
        fclose(fp);
        return;
    }
    memcpy(dir_entry((u32) slot), hdr, 64);
    wr16(dir_entry((u32) slot) + 0x36, (u16) first);
    for (i = 0; i < blocks; i++) {
        u32 b = file_block((u32) slot, i);
        if (b == 0 || fread(block(b), 1, BLOCK, fp) != BLOCK) {
            break;
        }
    }
    fclose(fp);
    commit_system();
    write_range(0, TOTAL_BLOCKS);
    snprintf(renamed, sizeof(renamed), "%s.imported", path);
    MoveFileA(path, renamed);
    fprintf(stderr, "[pc] card: imported %s (\"%s\", %u blocks)\n", path, name, blocks);
}

static void import_all_gci(void)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char pattern[MAX_PATH + 16];
    snprintf(pattern, sizeof(pattern), "%s/*.gci", save_dir());
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        char path[MAX_PATH + 300];
        snprintf(path, sizeof(path), "%s/%s", save_dir(), fd.cFileName);
        import_gci(path);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void warn_old_saves(void)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char pattern[MAX_PATH + 16];
    snprintf(pattern, sizeof(pattern), "%s/*.sav", save_dir());
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    FindClose(h);
    fprintf(stderr,
            "[pc] card: the .sav files in %s are from an earlier version and are not read any more; "
            "the save now lives in %s\n",
            save_dir(), IMAGE_NAME);
}

static void load_image(void)
{
    FILE* fp;
    if (loaded) {
        return;
    }
    loaded = 1;
    if (image == NULL) {
        image = (u8*) calloc(IMAGE_BYTES, 1);
        if (image != NULL) {
            pc_state_register(image, IMAGE_BYTES, "card image");
        }
        if (image == NULL) {
            return;
        }
    }
    fp = fopen(image_path(), "rb");
    if (fp != NULL) {
        size_t n = fread(image, 1, IMAGE_BYTES, fp);
        fclose(fp);
        if (n == IMAGE_BYTES && header_ok() && (dir_ok(1) || dir_ok(2)) && (bat_ok(3) || bat_ok(4)) &&
            rd16(block(0) + HDR_SIZE_MB) == CARD_MBIT)
        {
            choose_copies();
            fprintf(stderr, "[pc] card: %s, %d file(s), %u of %u blocks free\n", image_path(), file_count(),
                    free_blocks(), DATA_BLOCKS);
            import_all_gci();
            return;
        }
        {
            char bad[MAX_PATH + 80];
            snprintf(bad, sizeof(bad), "%s.bad", image_path());
            MoveFileExA(image_path(), bad, MOVEFILE_REPLACE_EXISTING);
            fprintf(stderr, "[pc] card: %s is not a usable %u Mbit card image, moved aside as %s\n", image_path(),
                    CARD_MBIT, bad);
        }
    }
    format_image();
    write_range(0, TOTAL_BLOCKS);
    fprintf(stderr, "[pc] card: formatted a new %u Mbit card image at %s\n", CARD_MBIT, image_path());
    warn_old_saves();
    import_all_gci();
}

/* --- helpers --------------------------------------------------------------- */

static s32 check_chan(s32 chan)
{
    if (chan != 0 || !mounted || image == NULL) {
        return CARD_RESULT_NOCARD;
    }
    return CARD_RESULT_READY;
}

static s32 open_entry(const CARDFileInfo* fi)
{
    if (fi == NULL || fi->chan != 0 || fi->fileNo < 0 || fi->fileNo >= DIR_ENTRIES || !entry_used((u32) fi->fileNo)) {
        return -1;
    }
    return fi->fileNo;
}

/* Mirrors the SDK's UpdateIconOffsets: where the banner, icons and data
 * start inside the file, derived from the directory entry. */
static void update_icon_offsets(CARDStat* stat)
{
    u32 offset = stat->iconAddr;
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
        *memSize = CARD_MBIT;
    }
    if (sectorSize != NULL) {
        *sectorSize = (s32) BLOCK;
    }
    return CARD_RESULT_READY;
}

s32 CARDMountAsync(s32 chan, void* workArea, CARDCallback detachCallback, CARDCallback attachCallback)
{
    TRACE("CARDMountAsync chan %d", chan);
    (void) workArea;
    (void) detachCallback;
    if (chan != 0) {
        return CARD_RESULT_NOCARD;
    }
    load_image();
    if (image == NULL) {
        return CARD_RESULT_NOCARD;
    }
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
    TRACE("CARDCheckAsync chan %d", chan);
    s32 r = check_chan(chan);
    if (r < 0) {
        return r;
    }
    defer(callback, CARD_RESULT_READY);
    return CARD_RESULT_READY;
}

s32 CARDFormatAsync(s32 chan, CARDCallback callback)
{
    TRACE("CARDFormatAsync chan %d", chan);
    if (chan != 0) {
        return CARD_RESULT_NOCARD;
    }
    load_image();
    if (image == NULL) {
        return CARD_RESULT_NOCARD;
    }
    format_image();
    write_range(0, TOTAL_BLOCKS);
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
        *byteNotUsed = (s32) (free_blocks() * BLOCK);
    }
    if (filesNotUsed != NULL) {
        *filesNotUsed = DIR_ENTRIES - file_count();
    }
    return CARD_RESULT_READY;
}

s32 CARDCreateAsync(s32 chan, const char* fileName, u32 size, CARDFileInfo* fileInfo, CARDCallback callback)
{
    TRACE("CARDCreateAsync \"%s\" %u bytes", fileName, size);
    s32 r = check_chan(chan);
    const DVDDiskID* id;
    u8* e;
    u32 i, first, blocks;
    int slot = -1;
    if (r < 0) {
        return r;
    }
    if (size == 0 || size % BLOCK != 0 || strlen(fileName) > CARD_FILENAME_MAX) {
        return CARD_RESULT_FATAL_ERROR;
    }
    if (find_file(fileName) >= 0) {
        return CARD_RESULT_EXIST;
    }
    for (i = 0; i < DIR_ENTRIES; i++) {
        if (!entry_used(i)) {
            slot = (int) i;
            break;
        }
    }
    if (slot < 0) {
        return CARD_RESULT_NOENT;
    }
    blocks = size / BLOCK;
    if (blocks > free_blocks()) {
        return CARD_RESULT_INSSPACE;
    }
    first = alloc_chain(blocks);
    if (first == 0) {
        return CARD_RESULT_INSSPACE;
    }
    e = dir_entry((u32) slot);
    memset(e, 0, 64);
    id = DVDGetCurrentDiskID();
    memcpy(e, id->gameName, 4);
    memcpy(e + 4, id->company, 2);
    e[6] = 0xFF;
    e[7] = 0; /* banner format */
    memset(e + 8, 0, CARD_FILENAME_MAX);
    strncpy((char*) e + 8, fileName, CARD_FILENAME_MAX);
    wr32(e + 0x28, now_seconds());
    wr32(e + 0x2C, 0xFFFFFFFFu); /* icon address */
    wr16(e + 0x30, 0);           /* icon formats */
    wr16(e + 0x32, 0);           /* icon speeds */
    e[0x34] = CARD_ATTR_PUBLIC;
    e[0x35] = 0; /* copy counter */
    wr16(e + 0x36, (u16) first);
    wr16(e + 0x38, (u16) blocks);
    wr16(e + 0x3A, 0xFFFF);
    wr32(e + 0x3C, 0xFFFFFFFFu); /* comment address */
    for (i = 0; i < blocks; i++) {
        u32 b = file_block((u32) slot, i);
        if (b != 0) {
            memset(block(b), 0, BLOCK);
        }
    }
    commit_system();
    fileInfo->chan = 0;
    fileInfo->fileNo = slot;
    fileInfo->offset = 0;
    fileInfo->length = (s32) size;
    fileInfo->iBlock = (u16) first;
    defer(callback, CARD_RESULT_READY);
    return CARD_RESULT_READY;
}

s32 CARDDeleteAsync(s32 chan, char* fileName, CARDCallback callback)
{
    TRACE("CARDDeleteAsync \"%s\"", fileName);
    s32 r = check_chan(chan);
    int i;
    if (r < 0) {
        return r;
    }
    i = find_file(fileName);
    if (i < 0) {
        return CARD_RESULT_NOFILE;
    }
    free_chain(rd16(dir_entry((u32) i) + 0x36));
    memset(dir_entry((u32) i), 0xFF, 64);
    commit_system();
    defer(callback, CARD_RESULT_READY);
    return CARD_RESULT_READY;
}

s32 CARDRenameAsync(s32 chan, const char* oldName, const char* newName, CARDCallback callback)
{
    TRACE("CARDRenameAsync \"%s\" -> \"%s\"", oldName, newName);
    s32 r = check_chan(chan);
    int i;
    u8* e;
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
    e = dir_entry((u32) i);
    memset(e + 8, 0, CARD_FILENAME_MAX);
    strncpy((char*) e + 8, newName, CARD_FILENAME_MAX);
    wr32(e + 0x28, now_seconds());
    commit_system();
    defer(callback, CARD_RESULT_READY);
    return CARD_RESULT_READY;
}

static void fill_info(int entry, CARDFileInfo* fileInfo)
{
    fileInfo->chan = 0;
    fileInfo->fileNo = entry;
    fileInfo->offset = 0;
    fileInfo->length = (s32) (entry_blocks((u32) entry) * BLOCK);
    fileInfo->iBlock = rd16(dir_entry((u32) entry) + 0x36);
}

s32 CARDOpen(s32 chan, char* fileName, CARDFileInfo* fileInfo)
{
    TRACE("CARDOpen \"%s\"", fileName);
    s32 r = check_chan(chan);
    int i;
    if (r < 0) {
        return r;
    }
    i = find_file(fileName);
    if (i < 0) {
        return CARD_RESULT_NOFILE;
    }
    fill_info(i, fileInfo);
    return CARD_RESULT_READY;
}

s32 CARDFastOpen(s32 chan, s32 fileNo, CARDFileInfo* fileInfo)
{
    TRACE("CARDFastOpen file %d", fileNo);
    s32 r = check_chan(chan);
    if (r < 0) {
        return r;
    }
    if (fileNo < 0 || fileNo >= DIR_ENTRIES) {
        return CARD_RESULT_FATAL_ERROR;
    }
    if (!entry_used((u32) fileNo)) {
        return CARD_RESULT_NOFILE;
    }
    fill_info(fileNo, fileInfo);
    return CARD_RESULT_READY;
}

s32 CARDClose(CARDFileInfo* fileInfo)
{
    (void) fileInfo;
    return CARD_RESULT_READY;
}

static s32 transfer(CARDFileInfo* fileInfo, void* buf, s32 length, s32 offset, int write)
{
    s32 r = check_chan(fileInfo != NULL ? fileInfo->chan : -1);
    s32 entry;
    u32 total, done = 0;
    TRACE("%s file %d: %d bytes at %d", write ? "write" : "read", fileInfo != NULL ? fileInfo->fileNo : -1, length, offset);
    if (r < 0) {
        return r;
    }
    entry = open_entry(fileInfo);
    if (entry < 0) {
        return CARD_RESULT_NOFILE;
    }
    total = entry_blocks((u32) entry) * BLOCK;
    if (length < 0 || offset < 0 || (u32) offset + (u32) length > total) {
        return CARD_RESULT_LIMIT;
    }
    while (done < (u32) length) {
        u32 pos = (u32) offset + done;
        u32 b = file_block((u32) entry, pos / BLOCK);
        u32 in = pos % BLOCK, n = BLOCK - in;
        if (b == 0) {
            return CARD_RESULT_BROKEN;
        }
        if (n > (u32) length - done) {
            n = (u32) length - done;
        }
        if (write) {
            memcpy(block(b) + in, (const u8*) buf + done, n);
            write_range(b, 1);
        } else {
            memcpy((u8*) buf + done, block(b) + in, n);
        }
        done += n;
    }
    if (write) {
        /* as the SDK's write completion does: the entry's modification
         * time, and the directory sealed into the other copy */
        wr32(dir_entry((u32) entry) + 0x28, now_seconds());
        commit_dir();
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

s32 CARDGetStatus(s32 chan, s32 fileNo, CARDStat* stat)
{
    TRACE("CARDGetStatus file %d", fileNo);
    s32 r = check_chan(chan);
    const u8* e;
    if (r < 0) {
        return r;
    }
    if (fileNo < 0 || fileNo >= DIR_ENTRIES) {
        return CARD_RESULT_FATAL_ERROR;
    }
    if (!entry_used((u32) fileNo)) {
        return CARD_RESULT_NOFILE;
    }
    e = dir_entry((u32) fileNo);
    memset(stat, 0, sizeof(*stat));
    memcpy(stat->fileName, e + 8, CARD_FILENAME_MAX);
    stat->length = rd16(e + 0x38) * BLOCK;
    stat->time = rd32(e + 0x28);
    memcpy(stat->gameName, e, 4);
    memcpy(stat->company, e + 4, 2);
    stat->bannerFormat = e[7];
    stat->iconAddr = rd32(e + 0x2C);
    stat->iconFormat = rd16(e + 0x30);
    stat->iconSpeed = rd16(e + 0x32);
    stat->commentAddr = rd32(e + 0x3C);
    update_icon_offsets(stat);
    return CARD_RESULT_READY;
}

s32 CARDSetStatusAsync(s32 chan, s32 fileNo, CARDStat* stat, CARDCallback callback)
{
    TRACE("CARDSetStatusAsync file %d icon %08x comment %08x banner %u fmt %04x", fileNo, stat->iconAddr, stat->commentAddr, stat->bannerFormat, stat->iconFormat);
    s32 r = check_chan(chan);
    u8* e;
    u16 speed;
    if (r < 0) {
        return r;
    }
    if (fileNo < 0 || fileNo >= DIR_ENTRIES) {
        return CARD_RESULT_FATAL_ERROR;
    }
    if (!entry_used((u32) fileNo)) {
        return CARD_RESULT_NOFILE;
    }
    e = dir_entry((u32) fileNo);
    e[7] = stat->bannerFormat;
    wr32(e + 0x2C, stat->iconAddr);
    wr16(e + 0x30, stat->iconFormat);
    speed = stat->iconSpeed;
    if (stat->iconAddr == 0xFFFFFFFFu) {
        speed = (u16) ((speed & ~CARD_STAT_SPEED_MASK) | CARD_STAT_SPEED_FAST);
    }
    wr16(e + 0x32, speed);
    wr32(e + 0x3C, stat->commentAddr);
    wr32(e + 0x28, now_seconds());
    update_icon_offsets(stat);
    commit_system();
    defer(callback, CARD_RESULT_READY);
    return CARD_RESULT_READY;
}
