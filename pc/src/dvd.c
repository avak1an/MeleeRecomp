/**
 * @file dvd.c
 * Disc access. Milestone 1 has no disc: every lookup fails and every read
 * completes immediately with an error, so the game's own error paths run
 * instead of spinning forever on a callback that never comes. Milestone 2
 * replaces this with reads from an extracted disc directory.
 */
#include "pc_runtime.h"

#include <dolphin/dvd.h>

#include <stdio.h>

static int reported;

static void report_once(const char* what)
{
    if (!reported) {
        reported = 1;
        fprintf(stderr, "[pc] DVD: first disc lookup: %s (not found)\n", what);
    }
}

/* A read means the game cannot continue without real disc data; end the run
 * here with a clear message and a backtrace of who asked. */
static void stop_at_first_read(const char* what)
{
    fprintf(stderr,
            "[pc] DVD: the game requested its first disc read (%s).\n"
            "[pc] Disc access is milestone 2; stopping here.\n",
            what);
    pc_exit(5);
}

void DVDInit(void) {}

long DVDGetDriveStatus(void)
{
    pc_stub_hit("DVDGetDriveStatus");
    return DVD_STATE_END;
}

BOOL DVDCheckDisk(void)
{
    return 1;
}

struct DVDDiskID* DVDGetCurrentDiskID(void)
{
    static struct DVDDiskID id;
    return &id;
}

s32 DVDConvertPathToEntrynum(const char* pathPtr)
{
    pc_stub_hit("DVDConvertPathToEntrynum");
    report_once(pathPtr);
    return -1;
}

BOOL DVDFastOpen(s32 entrynum, DVDFileInfo* fileInfo)
{
    pc_stub_hit("DVDFastOpen");
    report_once("open by entry number");
    fileInfo->startAddr = 0;
    fileInfo->length = 0;
    fileInfo->callback = NULL;
    return 0;
}

BOOL DVDOpen(char* fileName, DVDFileInfo* fileInfo)
{
    pc_stub_hit("DVDOpen");
    report_once(fileName);
    fileInfo->startAddr = 0;
    fileInfo->length = 0;
    fileInfo->callback = NULL;
    return 0;
}

BOOL DVDClose(DVDFileInfo* fileInfo)
{
    pc_stub_hit("DVDClose");
    return 1;
}

BOOL DVDReadAsyncPrio(DVDFileInfo* fileInfo, void* addr, s32 length, s32 offset,
                      DVDCallback callback, s32 prio)
{
    pc_stub_hit("DVDReadAsyncPrio");
    stop_at_first_read("async read");
    return 0;
}

long DVDReadPrio(struct DVDFileInfo* fileInfo, void* addr, long length, long offset, long prio)
{
    pc_stub_hit("DVDReadPrio");
    stop_at_first_read("read");
    return -1;
}
