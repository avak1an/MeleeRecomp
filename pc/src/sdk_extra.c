/**
 * @file sdk_extra.c
 * Hand-written replacements for the few SDK, runtime, and libc symbols the
 * stub generator cannot derive from a header prototype: data objects,
 * function-pointer-typed parameters, and Metrowerks runtime helpers.
 */
#include "pc_runtime.h"

#include <dolphin/ax.h>
#include <dolphin/axfx.h>
#include <dolphin/gx/GXStruct.h>
#include <dolphin/os/OSThread.h>

/* --- GX render modes (extern/dolphin/src/dolphin/gx/GXFrameBuf.c) ------- */

GXRenderModeObj GXNtsc480IntDf = {
    0, 640, 480, 480, 40, 0, 640, 480, 1, 0, 0,
    { { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 },
      { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 } },
    { 8, 8, 10, 12, 10, 8, 8 },
};

GXRenderModeObj GXNtsc480Int = {
    0, 640, 480, 480, 40, 0, 640, 480, 1, 0, 0,
    { { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 },
      { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 } },
    { 0, 0, 21, 22, 21, 0, 0 },
};

GXRenderModeObj GXNtsc480Prog = {
    2, 640, 480, 480, 40, 0, 640, 480, 0, 0, 0,
    { { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 },
      { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 } },
    { 0, 0, 21, 22, 21, 0, 0 },
};

/* --- OS threads --------------------------------------------------------- */

int OSCreateThread(struct OSThread* thread, void* (*func)(void*), void* param, void* stack,
                   unsigned long stackSize, long priority, unsigned short attr)
{
    /* Threads are not started yet; the game creates them for video playback
     * and debugging. Reports success so callers keep going. */
    pc_stub_hit("OSCreateThread");
    return 1;
}

/* --- Metrowerks runtime and MSL data ----------------------------------- */

/// Double to unsigned 64-bit conversion helper emitted by the CW compiler.
u64 __cvt_dbl_usll(double d)
{
    if (d <= 0.0) {
        return 0;
    }
    if (d >= 18446744073709551615.0) {
        return ~(u64) 0;
    }
    return (u64) d;
}

/* src/MSL/float.c: FLT_MAX / +INF bit patterns read through a float pointer */
int MSL_TrigF_80400770[] = { 0x7FFFFFFF };
int MSL_TrigF_80400774[] = { 0x7F800000 };

/* Linker-provided stack bounds on the GameCube. The debug thread report
 * scans from _stack_end for the 0xAA fill pattern, so keep the two adjacent
 * and unpainted. */
unsigned char _stack_end[16];
unsigned char _stack_addr[16];
