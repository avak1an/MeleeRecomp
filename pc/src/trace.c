/**
 * @file trace.c
 * Function-entry ring buffer for crash reports. Built into every
 * configuration; only fills up when the tree is compiled with
 * -DMELEE_TRACE_FUNCS=ON (clang's -finstrument-functions), otherwise
 * pc_trace_dump() prints nothing.
 */
#include "pc_runtime.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <dbghelp.h>

#define TRACE_RING 256

#if defined(__clang__)
#define NO_TRACE __attribute__((no_instrument_function))
#else
#define NO_TRACE
#endif

static void* ring[TRACE_RING];
static unsigned ring_pos;
static int ring_depth;

NO_TRACE void __cyg_profile_func_enter(void* fn, void* callsite)
{
    (void) callsite;
    ring[ring_pos++ & (TRACE_RING - 1)] = fn;
    ring_depth++;
}

NO_TRACE static void report_smashed(void* fn, void* ret)
{
    HANDLE proc = GetCurrentProcess();
    union {
        SYMBOL_INFO info;
        char buf[sizeof(SYMBOL_INFO) + 256];
    } sym;
    DWORD64 disp = 0;
    pc_trace_dump();
    SymInitialize(proc, NULL, TRUE);
    memset(&sym, 0, sizeof(sym));
    sym.info.SizeOfStruct = sizeof(SYMBOL_INFO);
    sym.info.MaxNameLen = 255;
    fprintf(stderr, "[pc] stack smash: %s is about to return to %p\n",
            SymFromAddr(proc, (DWORD64) (uintptr_t) fn, &disp, &sym.info) ? sym.info.Name : "?",
            ret);
    pc_exit(9);
}

NO_TRACE void __cyg_profile_func_exit(void* fn, void* callsite)
{
    /* The instrumented function is about to return: its return address is
     * our caller's return address. A smashed frame shows up here, one
     * function before it would crash. */
#if defined(__clang__)
    void* ret = __builtin_return_address(1);
#else
    void* ret = (void*) 0x10000; /* never instrumented under cl.exe */
#endif
    (void) callsite;
    ring_depth--;
    if ((uintptr_t) ret < 0x10000) {
        report_smashed(fn, ret);
    }
}

NO_TRACE void pc_trace_dump(void)
{
    HANDLE proc = GetCurrentProcess();
    union {
        SYMBOL_INFO info;
        char buf[sizeof(SYMBOL_INFO) + 256];
    } sym;
    /* Snapshot first: everything below is instrumented and would overwrite
     * the interesting tail of the ring. */
    static void* snap[TRACE_RING];
    unsigned snap_pos = ring_pos;
    unsigned n = snap_pos < TRACE_RING ? snap_pos : TRACE_RING;
    unsigned i;
    memcpy(snap, ring, sizeof(snap));
    if (n == 0) {
        return;
    }
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(proc, NULL, TRUE);
    fprintf(stderr, "[pc] last %u function entries (oldest first):\n", n > 96 ? 96u : n);
    for (i = (n > 96 ? n - 96 : 0); i < n; i++) {
        void* fn = snap[(snap_pos - n + i) & (TRACE_RING - 1)];
        DWORD64 disp = 0;
        memset(&sym, 0, sizeof(sym));
        sym.info.SizeOfStruct = sizeof(SYMBOL_INFO);
        sym.info.MaxNameLen = 255;
        if (SymFromAddr(proc, (DWORD64) (uintptr_t) fn, &disp, &sym.info)) {
            fprintf(stderr, "    %s\n", sym.info.Name);
        } else {
            fprintf(stderr, "    %p\n", fn);
        }
    }
}
