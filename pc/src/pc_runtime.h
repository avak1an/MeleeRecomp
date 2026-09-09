/**
 * @file pc_runtime.h
 * Internal interface of the PC runtime (shared by pc/src/*.c and the
 * generated stubs). Not included by game code.
 */
#ifndef PC_RUNTIME_H
#define PC_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>

typedef struct PCConfig {
    int max_frames;  ///< exit after this many VIWaitForRetrace calls (0 = never)
    bool log_stubs;  ///< print the first call of each stubbed SDK function
} PCConfig;

extern PCConfig pc_config;

/// Allocate the emulated main memory and set up the arena.
void pc_runtime_init(void);

/// Print the stub call summary and terminate with the given status.
__declspec(noreturn) void pc_exit(int status);

/// Print a symbolized stack trace of the calling thread to stderr.
void pc_print_backtrace(void);

/// Record a call into a stubbed SDK function (used by pc/generated/stubs.c).
void pc_stub_hit(const char* name);

#endif
