/**
 * @file runtime.c
 * Core OS replacement: emulated main memory and arena, console output,
 * panics, timing, alarms, and the completion pump that stands in for
 * interrupts.
 *
 * Everything else in the Dolphin SDK is either implemented in a sibling
 * file (vi.c, dvd.c, aram.c, gx.c, pad.c, card.c) or is a generated stub in
 * pc/generated/stubs.c.
 */
#include "pc_runtime.h"

#include <dolphin/os.h>
#include <dolphin/os/OSAlarm.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>
#include <dbghelp.h>

PCConfig pc_config;
uint32_t pc_frame_count;
const unsigned int* pc_debug_watch = NULL;
static unsigned int pc_debug_watch_last;
static const unsigned int* pc_debug_watch_last_ptr;
static char pc_debug_watch_prev_tag[128];

void (*pc_swap_hook)(const void* p, size_t bytes) = NULL;

/// --watch: report any swap helper whose range covers the watched word.
static void watch_swap_hook(const void* p, size_t bytes)
{
    uintptr_t lo = (uintptr_t) p, hi = lo + bytes, w = (uintptr_t) pc_debug_watch;
    if (w + 4 > lo && w < hi) {
        fprintf(stderr, "[pc] watch %p: swap of %u byte(s) at %p\n", (const void*) w,
                (unsigned) bytes, p);
        pc_print_backtrace();
    }
}

void pc_debug_watch_install(void)
{
    if (pc_debug_watch != NULL) {
        pc_swap_hook = watch_swap_hook;
    }
}

void pc_debug_watch_check(const char* tag)
{
    if (pc_debug_watch == NULL) {
        return;
    }
    if (pc_debug_watch != pc_debug_watch_last_ptr) {
        pc_debug_watch_last_ptr = pc_debug_watch;
        pc_debug_watch_last = *pc_debug_watch;
        fprintf(stderr, "[pc] watch %p = %08x (%s)\n", (const void*) pc_debug_watch,
                pc_debug_watch_last, tag);
    } else if (*pc_debug_watch != pc_debug_watch_last) {
        fprintf(stderr, "[pc] watch %p changed %08x -> %08x (between \"%s\" and \"%s\")\n",
                (const void*) pc_debug_watch, pc_debug_watch_last, *pc_debug_watch,
                pc_debug_watch_prev_tag, tag);
        pc_print_backtrace();
        pc_debug_watch_last = *pc_debug_watch;
    }
    strncpy(pc_debug_watch_prev_tag, tag, sizeof(pc_debug_watch_prev_tag) - 1);
}

int pc_debug_gx;

/* --- Memory ---------------------------------------------------------------
 * The retail GameCube has 24 MB of main memory; the game also checks for a
 * 48 MB simulated size on development kits. We hand out a 24 MB arena. */
#define PC_MEM_SIZE (24u * 1024u * 1024u)

static u8* mem_base;
static void* arena_lo;
static void* arena_hi;

u32 __OSBusClock = 162000000; /* used by the OS_TIMER_CLOCK macros */
u32 __OSCoreClock = 486000000;

static LARGE_INTEGER qpc_freq;
static LARGE_INTEGER qpc_start;

uint8_t* pc_mem_base(void)
{
    return mem_base;
}

size_t pc_mem_size(void)
{
    return PC_MEM_SIZE;
}

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

const char* pc_symbol_name(const void* addr)
{
    static char name[96];
    HANDLE proc = GetCurrentProcess();
    static int sym_ready;
    union {
        SYMBOL_INFO info;
        char buf[sizeof(SYMBOL_INFO) + 256];
    } sym;
    DWORD64 disp = 0;
    if (!sym_ready) {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS);
        sym_ready = SymInitialize(proc, NULL, TRUE) ? 1 : -1;
    }
    memset(&sym, 0, sizeof(sym));
    sym.info.SizeOfStruct = sizeof(SYMBOL_INFO);
    sym.info.MaxNameLen = 255;
    if (sym_ready > 0 && SymFromAddr(proc, (DWORD64) (uintptr_t) addr, &disp, &sym.info)) {
        snprintf(name, sizeof(name), "%s+0x%x", sym.info.Name, (unsigned) disp);
    } else {
        snprintf(name, sizeof(name), "%p", addr);
    }
    return name;
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

/* Hardware exceptions (access violations from mis-swapped data, mostly).
 * Walk the stack from the exception context so the faulting frame is the
 * first one printed, then leave through pc_exit. */
static void print_exception_backtrace(CONTEXT* ctx, HANDLE thread)
{
    HANDLE proc = GetCurrentProcess();
    STACKFRAME64 frame;
    CONTEXT c = *ctx;
    int i;
    union {
        SYMBOL_INFO info;
        char buf[sizeof(SYMBOL_INFO) + 256];
    } sym;
    IMAGEHLP_LINE line;
    DWORD displacement = 0;

    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS);
    SymInitialize(proc, NULL, TRUE);
    memset(&frame, 0, sizeof(frame));
    if (c.Eip < 0x1000) {
        /* A call through a NULL (or garbage) function pointer: the return
         * address is still on top of the stack, so resume the walk from the
         * caller. */
        c.Eip = *(DWORD*) (uintptr_t) c.Esp;
        c.Esp += 4;
        fprintf(stderr, "[pc] (called through a null function pointer from the frame below)\n");
    }
    frame.AddrPC.Offset = c.Eip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = c.Ebp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = c.Esp;
    frame.AddrStack.Mode = AddrModeFlat;
    fprintf(stderr, "[pc] backtrace from exception context:\n");
    for (i = 0; i < 48; i++) {
        DWORD64 sym_disp = 0;
        if (!StackWalk64(IMAGE_FILE_MACHINE_I386, proc, thread, &frame, &c, NULL,
                         SymFunctionTableAccess64, SymGetModuleBase64, NULL) ||
            frame.AddrPC.Offset == 0) {
            break;
        }
        memset(&sym, 0, sizeof(sym));
        sym.info.SizeOfStruct = sizeof(SYMBOL_INFO);
        sym.info.MaxNameLen = 255;
        line.SizeOfStruct = sizeof(line);
        if (SymFromAddr(proc, frame.AddrPC.Offset, &sym_disp, &sym.info)) {
            if (SymGetLineFromAddr(proc, (DWORD) frame.AddrPC.Offset, &displacement, &line)) {
                fprintf(stderr, "  #%-2d %s+0x%x  (%s:%lu)\n", i, sym.info.Name,
                        (unsigned) sym_disp, line.FileName, line.LineNumber);
            } else {
                fprintf(stderr, "  #%-2d %s+0x%x\n", i, sym.info.Name, (unsigned) sym_disp);
            }
        } else {
            fprintf(stderr, "  #%-2d 0x%08x\n", i, (unsigned) frame.AddrPC.Offset);
        }
    }
}

static LONG WINAPI crash_handler(EXCEPTION_POINTERS* ep)
{
    EXCEPTION_RECORD* er = ep->ExceptionRecord;
    pc_trace_dump(); /* first: it snapshots the function ring before we call anything */
    fprintf(stderr, "\n[pc] hardware exception 0x%08lx at 0x%08x", er->ExceptionCode,
            (unsigned) (uintptr_t) er->ExceptionAddress);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        fprintf(stderr, ": %s of address 0x%08x",
                er->ExceptionInformation[0] == 0 ? "read" : er->ExceptionInformation[0] == 1 ? "write" : "execute",
                (unsigned) er->ExceptionInformation[1]);
    }
    fprintf(stderr, "\n");
    print_exception_backtrace(ep->ContextRecord, GetCurrentThread());
    fprintf(stderr, "[pc] exiting after %u frame(s), status 7\n", pc_frame_count);
    print_stub_summary();
    fflush(stderr);
    _exit(7);
    return EXCEPTION_EXECUTE_HANDLER;
}

extern void pc_ax_shutdown(void);

/// Address of a global by name ("stage_info" or "stage_info+0x1c"), 0 if
/// the symbols do not resolve it.
uintptr_t pc_symbol_address(const char* spec)
{
    char name[256];
    const char* plus = strchr(spec, '+');
    uintptr_t offset = 0;
    size_t n = plus != NULL ? (size_t) (plus - spec) : strlen(spec);
    struct {
        SYMBOL_INFO info;
        char pad[256];
    } sym;
    if (n >= sizeof(name)) {
        return 0;
    }
    memcpy(name, spec, n);
    name[n] = '\0';
    if (plus != NULL) {
        offset = (uintptr_t) strtoul(plus + 1, NULL, 0);
    }
    pc_symbol_name(NULL); /* initializes the symbol handler */
    memset(&sym, 0, sizeof(sym));
    sym.info.SizeOfStruct = sizeof(SYMBOL_INFO);
    sym.info.MaxNameLen = 255;
    if (!SymFromName(GetCurrentProcess(), name, &sym.info)) {
        return 0;
    }
    return (uintptr_t) sym.info.Address + offset;
}

const char* pc_exe_dir(void)
{
    static char dir[MAX_PATH + 1];
    if (dir[0] == '\0') {
        DWORD n = GetModuleFileNameA(NULL, dir, sizeof(dir));
        char* slash;
        if (n == 0 || n >= sizeof(dir)) {
            strcpy(dir, ".");
            return dir;
        }
        slash = strrchr(dir, '\\');
        if (slash != NULL) {
            *slash = '\0';
        } else {
            strcpy(dir, ".");
        }
    }
    return dir;
}

int pc_ptr_readable(const void* p, size_t bytes)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (p == NULL || VirtualQuery(p, &mbi, sizeof(mbi)) == 0) {
        return 0;
    }
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
        return 0;
    }
    return (const u8*) p + bytes <= (const u8*) mbi.BaseAddress + mbi.RegionSize;
}

__declspec(noreturn) void pc_exit(int status)
{
    if (status != 0) {
        pc_print_backtrace();
    }
    fprintf(stderr, "[pc] exiting after %u frame(s), status %d\n", pc_frame_count, status);
    print_stub_summary();
    pc_ax_shutdown();
    fflush(stdout);
    fflush(stderr);
    exit(status);
}

/* Watchdog: a helper thread that reports where the game is stuck when no
 * frame has completed for a while (MELEE_WATCHDOG seconds, default 20). */
static HANDLE main_thread;

static DWORD WINAPI watchdog_main(LPVOID arg)
{
    uint32_t last = pc_frame_count;
    unsigned quiet = 0, limit = 20;
    const char* env = getenv("MELEE_WATCHDOG");
    (void) arg;
    if (env != NULL && atoi(env) > 0) {
        limit = (unsigned) atoi(env);
    }
    for (;;) {
        Sleep(1000);
        if (pc_frame_count != last) {
            last = pc_frame_count;
            quiet = 0;
            continue;
        }
        if (++quiet >= limit) {
            CONTEXT ctx;
            memset(&ctx, 0, sizeof(ctx));
            ctx.ContextFlags = CONTEXT_FULL;
            SuspendThread(main_thread);
            if (GetThreadContext(main_thread, &ctx)) {
                fprintf(stderr, "[pc] watchdog: no frame completed for %u s (frame %u); main thread is at:\n",
                        quiet, (unsigned) pc_frame_count);
                print_exception_backtrace(&ctx, main_thread);
            }
            fflush(stderr);
            ExitProcess(8);
        }
    }
}

void pc_runtime_init(void)
{
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &main_thread, 0,
                    FALSE, DUPLICATE_SAME_ACCESS);
    CreateThread(NULL, 0, watchdog_main, NULL, 0, NULL);
    /* The game tells main memory from ARAM by address (main memory is
     * 0x80000000 and up on the console), so map the arena at the console's
     * address. That needs the /LARGEADDRESSAWARE 32-bit process the linker
     * flags request; fall back to any address, with a warning, if the range
     * is taken. */
    mem_base = (u8*) VirtualAlloc((void*) 0x80000000u, PC_MEM_SIZE, MEM_RESERVE | MEM_COMMIT,
                                  PAGE_READWRITE);
    if (mem_base == NULL) {
        fprintf(stderr, "[pc] warning: could not map main memory at 0x80000000 (error %lu); "
                        "address-based memory checks in the game may misfire\n",
                GetLastError());
        mem_base = (u8*) VirtualAlloc(NULL, PC_MEM_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }
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
    SetUnhandledExceptionFilter(crash_handler);
}

/* --- Completion pump ---------------------------------------------------- */

bool pc_pump(void)
{
    bool any = false;
    bool ran;
    int guard = 0;
    do {
        ran = false;
        ran |= pc_dvd_pump();
        ran |= pc_arq_pump();
        ran |= pc_gx_pump();
        ran |= pc_alarm_pump();
        ran |= pc_card_pump();
        any |= ran;
    } while (ran && ++guard < 100000);
    return any;
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

/* --- Time ---------------------------------------------------------------- */

static OSTime host_ticks(void)
{
    LARGE_INTEGER now;
    static OSTime virtual_calls;
    if (!pc_config.realtime) {
        /* Unpaced runs (headless tests, scripted routes) use a clock locked
         * to the frame counter so alarms and timeouts fire on the same frame
         * every run regardless of host speed. Each query also advances it a
         * little so loops that spin on the tick counter still terminate. */
        virtual_calls += 40;
        return (OSTime) pc_frame_count * (OSTime) (__OSBusClock / 4 / 60) + virtual_calls;
    }
    QueryPerformanceCounter(&now);
    /* convert host counter to GameCube timer ticks (bus clock / 4) */
    return (OSTime) ((now.QuadPart - qpc_start.QuadPart) * (OSTime) (__OSBusClock / 4) /
                     qpc_freq.QuadPart);
}

OSTick OSGetTick(void)
{
    return (OSTick) host_ticks();
}

/// The game seeds its RNG from the tick counter at boot; --seed overrides.
unsigned pc_game_seed(void)
{
    return pc_config.seed != 0 ? pc_config.seed : (unsigned) host_ticks();
}

OSTime OSGetTime(void)
{
    return host_ticks();
}

void OSTicksToCalendarTime(OSTime ticks, OSCalendarTime* td)
{
    /* The console counts ticks since 2000-01-01 00:00:00. Derive the
     * calendar from the ticks so unpaced runs stay repeatable (the virtual
     * clock starts at that date); real-time runs also start there. */
    time_t secs = (time_t) 946684800 + (time_t) (ticks / (OSTime) (__OSBusClock / 4));
    struct tm* t = gmtime(&secs);
    memset(td, 0, sizeof(*td));
    if (t != NULL) {
        td->sec = t->tm_sec;
        td->min = t->tm_min;
        td->hour = t->tm_hour;
        td->mday = t->tm_mday;
        td->mon = t->tm_mon;
        td->year = t->tm_year + 1900;
        td->wday = t->tm_wday;
        td->yday = t->tm_yday;
    }
    td->msec = (int) ((ticks / (OSTime) (__OSBusClock / 4000)) % 1000);
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

/* --- Alarms ---------------------------------------------------------------
 * A doubly linked list ordered by fire time, serviced from pc_pump(). The
 * game uses one-shot alarms for staged memory copies and periodic ones for
 * controller sampling and movie playback. */

static OSAlarm* alarm_head;
static OSContext alarm_context; /* handlers get a context pointer; unused */

/* There is one thread and its register image is never inspected for real;
 * the debug code only pokes fpscr in it. */
OSContext* OSGetCurrentContext(void)
{
    return &alarm_context;
}

static void alarm_unlink(OSAlarm* a)
{
    if (a->prev != NULL) {
        a->prev->next = a->next;
    } else if (alarm_head == a) {
        alarm_head = a->next;
    }
    if (a->next != NULL) {
        a->next->prev = a->prev;
    }
    a->prev = a->next = NULL;
}

static void alarm_insert(OSAlarm* a)
{
    OSAlarm* it = alarm_head;
    OSAlarm* last = NULL;
    while (it != NULL && it->fire <= a->fire) {
        last = it;
        it = it->next;
    }
    a->prev = last;
    a->next = it;
    if (last != NULL) {
        last->next = a;
    } else {
        alarm_head = a;
    }
    if (it != NULL) {
        it->prev = a;
    }
}

static bool alarm_linked(OSAlarm* a)
{
    OSAlarm* it;
    for (it = alarm_head; it != NULL; it = it->next) {
        if (it == a) {
            return true;
        }
    }
    return false;
}

void OSInitAlarm(void)
{
    alarm_head = NULL;
}

void OSCreateAlarm(OSAlarm* alarm)
{
    if (alarm_linked(alarm)) {
        alarm_unlink(alarm);
    }
    alarm->handler = NULL;
    alarm->prev = alarm->next = NULL;
    alarm->period = 0;
}

void OSSetAlarm(OSAlarm* alarm, OSTime tick, OSAlarmHandler handler)
{
    if (alarm_linked(alarm)) {
        alarm_unlink(alarm);
    }
    alarm->handler = handler;
    alarm->period = 0;
    alarm->fire = host_ticks() + tick;
    alarm_insert(alarm);
}

void OSSetPeriodicAlarm(OSAlarm* alarm, OSTime start, OSTime period, OSAlarmHandler handler)
{
    OSTime now = host_ticks();
    if (alarm_linked(alarm)) {
        alarm_unlink(alarm);
    }
    alarm->handler = handler;
    alarm->start = start;
    alarm->period = period;
    alarm->fire = start;
    if (period > 0) {
        while (alarm->fire <= now) {
            alarm->fire += period;
        }
    }
    alarm_insert(alarm);
}

void OSCancelAlarm(OSAlarm* alarm)
{
    if (alarm_linked(alarm)) {
        alarm_unlink(alarm);
    }
    alarm->handler = NULL;
}

bool pc_alarm_pump(void)
{
    bool ran = false;
    OSTime now = host_ticks();
    while (alarm_head != NULL && alarm_head->fire <= now) {
        OSAlarm* a = alarm_head;
        OSAlarmHandler handler = a->handler;
        alarm_unlink(a);
        if (a->period > 0) {
            a->fire += a->period;
            if (a->fire <= now) {
                a->fire = now + a->period; /* don't try to catch up */
            }
            alarm_insert(a);
        }
        if (handler != NULL) {
            handler(a, &alarm_context);
            ran = true;
        }
    }
    return ran;
}
