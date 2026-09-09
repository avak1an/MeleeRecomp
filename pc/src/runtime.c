/**
 * @file runtime.c
 * Core OS replacement: emulated main memory and arena, console output,
 * panics, timing, and the per-frame hook that ends a bounded run.
 *
 * Everything else in the Dolphin SDK is a generated stub for now
 * (pc/generated/stubs.c); functions get real implementations here as later
 * milestones need them.
 */
#include "pc_runtime.h"

#include <dolphin/os.h>
#include <dolphin/vi.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <dbghelp.h>

PCConfig pc_config;

/* --- Memory ---------------------------------------------------------------
 * The retail GameCube has 24 MB of main memory; the game also checks for a
 * 48 MB simulated size on development kits. We hand out a 24 MB arena. */
#define PC_MEM_SIZE (24u * 1024u * 1024u)

static u8* mem_base;
static void* arena_lo;
static void* arena_hi;

u32 __OSBusClock = 162000000; /* used by the OS_TIMER_CLOCK macros */

static LARGE_INTEGER qpc_freq;
static LARGE_INTEGER qpc_start;
static u32 frame_count;

/* --- Stub bookkeeping ------------------------------------------------- */
#define MAX_STUB_NAMES 1024
/* A stub hit this often in one run means the game is waiting on it. */
#define PC_SPIN_LIMIT 2000000u
static const char* stub_names[MAX_STUB_NAMES];
static u32 stub_counts[MAX_STUB_NAMES];
static int stub_used;

void pc_stub_hit(const char* name)
{
    int i;
    for (i = 0; i < stub_used; i++) {
        if (stub_names[i] == name) {
            if (++stub_counts[i] == PC_SPIN_LIMIT) {
                fprintf(stderr,
                        "[pc] %s was called %u times: the game is spinning on an "
                        "unimplemented SDK function\n",
                        name, PC_SPIN_LIMIT);
                pc_exit(4);
            }
            return;
        }
    }
    if (stub_used < MAX_STUB_NAMES) {
        stub_names[stub_used] = name;
        stub_counts[stub_used] = 1;
        stub_used++;
    }
    if (pc_config.log_stubs) {
        fprintf(stderr, "[stub] %s\n", name);
    }
}

static void print_stub_summary(void)
{
    int i;
    if (stub_used == 0) {
        return;
    }
    fprintf(stderr, "\n[pc] %d distinct SDK stubs were called:\n", stub_used);
    for (i = 0; i < stub_used; i++) {
        fprintf(stderr, "  %8u  %s\n", stub_counts[i], stub_names[i]);
    }
}

void pc_print_backtrace(void)
{
    void* frames[48];
    USHORT n, i;
    HANDLE proc = GetCurrentProcess();
    static int sym_ready;
    union {
        SYMBOL_INFO info;
        char buf[sizeof(SYMBOL_INFO) + 256];
    } sym;
    IMAGEHLP_LINE line;
    DWORD displacement = 0;

    if (!sym_ready) {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS);
        sym_ready = SymInitialize(proc, NULL, TRUE) ? 1 : -1;
    }
    n = CaptureStackBackTrace(1, 48, frames, NULL);
    fprintf(stderr, "[pc] backtrace (%u frames):\n", n);
    for (i = 0; i < n; i++) {
        DWORD64 addr = (DWORD64) (uintptr_t) frames[i];
        DWORD64 sym_disp = 0;
        memset(&sym, 0, sizeof(sym));
        sym.info.SizeOfStruct = sizeof(SYMBOL_INFO);
        sym.info.MaxNameLen = 255;
        line.SizeOfStruct = sizeof(line);
        if (sym_ready > 0 && SymFromAddr(proc, addr, &sym_disp, &sym.info)) {
            if (SymGetLineFromAddr(proc, (DWORD) addr, &displacement, &line)) {
                fprintf(stderr, "  #%-2u %s+0x%x  (%s:%lu)\n", i, sym.info.Name,
                        (unsigned) sym_disp, line.FileName, line.LineNumber);
            } else {
                fprintf(stderr, "  #%-2u %s+0x%x\n", i, sym.info.Name, (unsigned) sym_disp);
            }
        } else {
            fprintf(stderr, "  #%-2u %p\n", i, frames[i]);
        }
    }
}

__declspec(noreturn) void pc_exit(int status)
{
    if (status != 0) {
        pc_print_backtrace();
    }
    fprintf(stderr, "[pc] exiting after %u frame(s), status %d\n", frame_count, status);
    print_stub_summary();
    fflush(stdout);
    fflush(stderr);
    exit(status);
}

void pc_runtime_init(void)
{
    mem_base = (u8*) VirtualAlloc(NULL, PC_MEM_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (mem_base == NULL) {
        fprintf(stderr, "[pc] failed to allocate %u bytes of main memory\n", PC_MEM_SIZE);
        exit(1);
    }
    arena_lo = mem_base;
    arena_hi = mem_base + PC_MEM_SIZE;
    QueryPerformanceFrequency(&qpc_freq);
    QueryPerformanceCounter(&qpc_start);
    /* OSReport output must survive a hard exit or an external kill. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
}

/* --- OS ----------------------------------------------------------------- */

void OSInit(void)
{
    OSReport("[pc] OSInit: arena %p-%p (%u MB)\n", arena_lo, arena_hi, PC_MEM_SIZE >> 20);
}

u32 OSGetPhysicalMemSize(void)
{
    return PC_MEM_SIZE;
}

u32 OSGetConsoleSimulatedMemSize(void)
{
    return PC_MEM_SIZE;
}

unsigned long OSGetConsoleType(void)
{
    return 0x10000006; /* OS_CONSOLE_RETAIL1 */
}

void* OSGetArenaHi(void)
{
    return arena_hi;
}

void* OSGetArenaLo(void)
{
    return arena_lo;
}

void OSSetArenaHi(void* p)
{
    arena_hi = p;
}

void OSSetArenaLo(void* p)
{
    arena_lo = p;
}

void* OSAllocFromArenaLo(u32 size, u32 align)
{
    u8* p = (u8*) (((uintptr_t) arena_lo + align - 1) & ~(uintptr_t) (align - 1));
    arena_lo = (u8*) (((uintptr_t) p + size + align - 1) & ~(uintptr_t) (align - 1));
    return p;
}

void* OSAllocFromArenaHi(u32 size, u32 align)
{
    u8* hi = (u8*) ((uintptr_t) arena_hi & ~(uintptr_t) (align - 1));
    u8* p = (u8*) (((uintptr_t) hi - size) & ~(uintptr_t) (align - 1));
    arena_hi = p;
    return p;
}

void OSReport(char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
}

void OSVReport(char* fmt, va_list ap)
{
    vfprintf(stdout, fmt, ap);
}

__declspec(noreturn) void OSPanic(char* file, int line, char* msg, ...)
{
    va_list ap;
    fflush(stdout);
    fprintf(stderr, "[pc] OSPanic at %s:%d: ", file, line);
    va_start(ap, msg);
    vfprintf(stderr, msg, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    pc_exit(3);
}

static OSTime host_ticks(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    /* convert host counter to GameCube timer ticks (bus clock / 4) */
    return (OSTime) ((now.QuadPart - qpc_start.QuadPart) * (OSTime) (__OSBusClock / 4) /
                     qpc_freq.QuadPart);
}

OSTick OSGetTick(void)
{
    return (OSTick) host_ticks();
}

OSTime OSGetTime(void)
{
    return host_ticks();
}

/* Interrupt masking has no meaning here; the game only pairs these calls. */
BOOL OSEnableInterrupts(void)
{
    return 1;
}

BOOL OSDisableInterrupts(void)
{
    return 1;
}

BOOL OSRestoreInterrupts(BOOL level)
{
    return level;
}

unsigned long OSGetProgressiveMode(void)
{
    return 0;
}

unsigned long OSGetSoundMode(void)
{
    return 1; /* stereo */
}

/* --- VI ----------------------------------------------------------------- */

void VIWaitForRetrace(void)
{
    frame_count++;
    if (pc_config.max_frames > 0 && frame_count >= (u32) pc_config.max_frames) {
        pc_exit(0);
    }
}
