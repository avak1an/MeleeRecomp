/**
 * @file vi.c
 * Video interface. There is no display yet; VIWaitForRetrace() is the
 * frame boundary: it runs the engine's pre/post-retrace callbacks (which
 * sample controllers and flip frame buffers), drains completions, paces the
 * loop when asked to, and ends a bounded run.
 */
#include "pc_runtime.h"

#include <dolphin/gx/GXStruct.h>
#include <dolphin/vi.h>

#include <stdio.h>
#include <windows.h>

static VIRetraceCallback pre_cb;
static VIRetraceCallback post_cb;
static u32 retrace_count;
static void* next_fb;
static void* current_fb;
static LARGE_INTEGER last_frame;

void VIInit(void) {}

void VIConfigure(GXRenderModeObj* rm)
{
    (void) rm;
}

void VIFlush(void)
{
    current_fb = next_fb;
}

void VISetNextFrameBuffer(void* fb)
{
    next_fb = fb;
}

void* VIGetNextFrameBuffer(void)
{
    return next_fb;
}

void* VIGetCurrentFrameBuffer(void)
{
    return current_fb;
}

void VISetBlack(BOOL black)
{
    (void) black;
}

u32 VIGetRetraceCount(void)
{
    return retrace_count;
}

u32 VIGetNextField(void)
{
    return retrace_count & 1; /* VI_FIELD_ABOVE / BELOW */
}

u32 VIGetDTVStatus(void)
{
    return 0; /* no component cable: no progressive-scan prompt */
}

u32 VIGetTvFormat(void)
{
    return 0; /* VI_NTSC */
}

VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback cb)
{
    VIRetraceCallback old = pre_cb;
    pre_cb = cb;
    return old;
}

VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb)
{
    VIRetraceCallback old = post_cb;
    post_cb = cb;
    return old;
}

static void pace_to_60hz(void)
{
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    if (last_frame.QuadPart == 0) {
        QueryPerformanceCounter(&last_frame);
        return;
    }
    for (;;) {
        QueryPerformanceCounter(&now);
        if ((now.QuadPart - last_frame.QuadPart) * 60 >= freq.QuadPart) {
            break;
        }
        Sleep(1);
    }
    last_frame = now;
}

void VIWaitForRetrace(void)
{
    pc_frame_count++;
    retrace_count++;
    if (pc_config.realtime) {
        pace_to_60hz();
    }
    if (pre_cb != NULL) {
        pre_cb(retrace_count);
    }
    if (post_cb != NULL) {
        post_cb(retrace_count);
    }
    pc_pump();
    if (pc_config.max_frames > 0 && pc_frame_count >= (u32) pc_config.max_frames) {
        pc_exit(0);
    }
}
