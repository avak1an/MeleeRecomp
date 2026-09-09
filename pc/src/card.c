/**
 * @file card.c
 * Memory card API. There is no card on PC yet: every call reports
 * CARD_RESULT_NOCARD, which is a state the game already handles (it is what
 * a GameCube with empty slots returns). Saving to a file comes with
 * milestone 4.
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

void CARDInit(void) {}

int CARDProbe(long chan)
{
    return 0;
}

s32 CARDProbeEx(s32 chan, s32* memSize, s32* sectorSize)
{
    return CARD_RESULT_NOCARD;
}

s32 CARDMountAsync(s32 chan, void* workArea, CARDCallback detachCallback, CARDCallback attachCallback)
{
    pc_stub_hit("CARDMountAsync");
    return CARD_RESULT_NOCARD;
}

s32 CARDUnmount(s32 chan)
{
    return CARD_RESULT_NOCARD;
}

s32 CARDCheckAsync(s32 chan, CARDCallback callback)
{
    return CARD_RESULT_NOCARD;
}

s32 CARDCreateAsync(s32 chan, const char* fileName, u32 size, CARDFileInfo* fileInfo, CARDCallback callback)
{
    return CARD_RESULT_NOCARD;
}

s32 CARDDeleteAsync(s32 chan, char* fileName, CARDCallback callback)
{
    return CARD_RESULT_NOCARD;
}

s32 CARDOpen(s32 chan, char* fileName, CARDFileInfo* fileInfo)
{
    return CARD_RESULT_NOCARD;
}

s32 CARDFastOpen(s32 chan, s32 fileNo, CARDFileInfo* fileInfo)
{
    return CARD_RESULT_NOCARD;
}

s32 CARDClose(CARDFileInfo* fileInfo)
{
    return CARD_RESULT_NOCARD;
}

s32 CARDFreeBlocks(s32 chan, s32* byteNotUsed, s32* filesNotUsed)
{
    return CARD_RESULT_NOCARD;
}

s32 CARDGetStatus(s32 chan, s32 fileNo, CARDStat* stat)
{
    return CARD_RESULT_NOCARD;
}

s32 CARDSetStatusAsync(s32 chan, s32 fileNo, CARDStat* stat, CARDCallback callback)
{
    return CARD_RESULT_NOCARD;
}

long CARDGetXferredBytes(long chan)
{
    return 0;
}

long CARDRead(struct CARDFileInfo* fileInfo, void* buf, long length, long offset)
{
    return CARD_RESULT_NOCARD;
}

s32 CARDReadAsync(CARDFileInfo* fileInfo, void* buf, s32 length, s32 offset, CARDCallback callback)
{
    return CARD_RESULT_NOCARD;
}

long CARDWrite(struct CARDFileInfo* fileInfo, void* buf, long length, long offset)
{
    return CARD_RESULT_NOCARD;
}

long CARDWriteAsync(struct CARDFileInfo* fileInfo, void* buf, long length, long offset,
                    void (*callback)(long, long))
{
    return CARD_RESULT_NOCARD;
}
