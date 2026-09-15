/**
 * @file vi.c
 * Video interface. There is no display yet; VIWaitForRetrace() is the
 * frame boundary: it runs the engine's pre/post-retrace callbacks (which
 * sample controllers and flip frame buffers), drains completions, paces the
 * loop when asked to, and ends a bounded run.
 */
#include "pc_gl.h"
#include "pc_runtime.h"
#include <pc_hsd_swap.h>

#include <dolphin/gx/GXStruct.h>
#include <dolphin/vi.h>

#include <stdio.h>
#include <windows.h>
#include <mmsystem.h>

extern void pc_ax_frame(void);

/* --char: the character forced on port 1 (see Player_SetPlayerCharacter) */
int pc_debug_p1_char(void)
{
    return pc_config.p1_char;
}

int pc_debug_p2_char(void)
{
    return pc_config.p2_char;
}
extern int pc_window_vsync_hz(void);

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

/* Holds each frame to 1/60 s. When the swap chain is already synchronized
 * to a multiple of 60 Hz (see pc_window_vsync_hz) it does the pacing and
 * this is skipped: two pacers fighting over the same frame drop to 30 Hz.
 * The period is fixed, not measured from the previous frame, so the
 * average rate is exact; after a stall the deadline is reset instead of
 * catching up. */
static void pace_to_60hz(void)
{
    static LARGE_INTEGER freq, deadline;
    static int period_set;
    LARGE_INTEGER now;
    if (pc_window_vsync_hz() == 60) {
        return;
    }
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    if (!period_set) {
        timeBeginPeriod(1); /* 1 ms Sleep granularity */
        period_set = 1;
    }
    QueryPerformanceCounter(&now);
    if (deadline.QuadPart == 0 || now.QuadPart > deadline.QuadPart + freq.QuadPart / 10) {
        deadline = now; /* first frame, or more than 100 ms behind */
    }
    deadline.QuadPart += freq.QuadPart / 60;
    for (;;) {
        LONGLONG left;
        QueryPerformanceCounter(&now);
        left = deadline.QuadPart - now.QuadPart;
        if (left <= 0) {
            break;
        }
        if (left > freq.QuadPart / 500) {
            Sleep(1); /* more than 2 ms to go */
        } else {
            YieldProcessor(); /* spin out the last stretch */
        }
    }
}

/* Frame-time accounting for paced runs: how many frames of the last few
 * seconds took longer than a retrace (the game then runs below 60 Hz and
 * the audio stutters), reported only when it happened. */
static void frame_stats(void)
{
    static LARGE_INTEGER freq, last, report;
    static u32 frames, slow;
    static double worst;
    LARGE_INTEGER now;
    double ms;
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    QueryPerformanceCounter(&now);
    if (last.QuadPart != 0) {
        ms = (double) (now.QuadPart - last.QuadPart) * 1000.0 / (double) freq.QuadPart;
        frames++;
        if (ms > 17.5) {
            slow++;
            if (ms > worst) {
                worst = ms;
            }
        }
    }
    if (report.QuadPart == 0) {
        report = now;
    } else if (now.QuadPart - report.QuadPart > freq.QuadPart * 5) {
        if (slow != 0) {
            fprintf(stderr, "[pc] frame %u: %u of the last %u frames took longer than 1/60 s (worst %.1f ms)\n",
                    pc_frame_count, slow, frames, worst);
        }
        report = now;
        frames = slow = 0;
        worst = 0.0;
    }
    last = now;
}

void VIWaitForRetrace(void)
{
    pc_frame_count++;
    retrace_count++;
    pc_state_frame();
    if (!pc_window_pump()) {
        fprintf(stderr, "[pc] window closed\n");
        pc_exit(0);
    }
    if (pc_config.realtime) {
        frame_stats(); /* before pacing: measures the work, not the wait */
        pace_to_60hz();
    }
    if (pre_cb != NULL) {
        pre_cb(retrace_count);
    }
    if (post_cb != NULL) {
        post_cb(retrace_count);
    }
    pc_pump();
    pc_ax_frame();
    pc_swap_verify_relocs("retrace");
    if (pc_config.item_kind >= 0 && pc_frame_count == (u32) pc_config.item_frame) {
        extern void pc_debug_spawn_item(int kind, int slot);
        pc_debug_spawn_item(pc_config.item_kind, pc_config.item_slot);
    }
    if (pc_config.kill_slots != 0 && pc_frame_count >= (u32) pc_config.kill_frame &&
        (pc_frame_count - (u32) pc_config.kill_frame) % (u32) pc_config.kill_every == 0)
    {
        extern void pc_debug_kill_fighter(int slot);
        int slot;
        for (slot = 0; slot < 6; slot++) {
            if (pc_config.kill_slots & (1 << slot)) {
                pc_debug_kill_fighter(slot);
            }
        }
    }
    if (pc_config.max_frames > 0 && pc_frame_count >= (u32) pc_config.max_frames) {
        pc_exit(0);
    }
}
