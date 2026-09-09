/**
 * @file pc_runtime.h
 * Internal interface of the PC runtime (shared by pc/src/*.c and the
 * generated stubs). Not included by game code.
 */
#ifndef PC_RUNTIME_H
#define PC_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct PCConfig {
    int max_frames;      ///< exit after this many VIWaitForRetrace calls (0 = never)
    bool log_stubs;      ///< print the first call of each stubbed SDK function
    bool realtime;       ///< pace VIWaitForRetrace to 60 Hz
    bool autoplay;       ///< synthesize Start/A presses on port 1 to push through prompts
    const char* input_script; ///< file of scripted port-1 inputs (see pad.c)
    unsigned seed;       ///< if nonzero, the game's RNG seed (else the clock)
    bool headless;       ///< no window, no rendering
    const char* screenshot_dir; ///< if set, dump a BMP of every 60th frame here
    const char* iso;     ///< path of the disc image (NULL = auto-detect)
} PCConfig;

extern PCConfig pc_config;

/// Set when MELEE_GX_DEBUG is in the environment; engine code may log.
extern int pc_debug_gx;

/// Debug aid: a word to watch while swapping; pc_debug_watch_check() reports
/// every change of *pc_debug_watch since the previous check.
extern const unsigned int* pc_debug_watch;
void pc_debug_watch_check(const char* tag);
void pc_debug_watch_install(void);

/// Number of VIWaitForRetrace calls so far.
extern uint32_t pc_frame_count;

/// Allocate the emulated main memory and set up the arena.
void pc_runtime_init(void);

/// Print the stub call summary and terminate with the given status.
__declspec(noreturn) void pc_exit(int status);

/// Print a symbolized stack trace of the calling thread to stderr.
void pc_print_backtrace(void);
/// Symbol name for a code address ("func+0x12"), in a static buffer.
const char* pc_symbol_name(const void* addr);

/// Print the most recent function entries (only when built with
/// MELEE_TRACE_FUNCS; otherwise prints nothing).
void pc_trace_dump(void);

/// Record a call into a stubbed SDK function (used by pc/generated/stubs.c).
void pc_stub_hit(const char* name);

/* --- Deferred completions ---------------------------------------------------
 * On the GameCube, disc reads, ARAM transfers, GX draw-done and alarms
 * complete from interrupt handlers. Here they complete from pc_pump(), which
 * every wait point calls (VIWaitForRetrace, DVDGetDriveStatus, ...). Each
 * subsystem queues its completions and pc_pump() drains them until no more
 * work is pending. */

/// Run all pending completions. Returns true if anything ran.
bool pc_pump(void);

bool pc_dvd_pump(void);
bool pc_alarm_pump(void);
bool pc_arq_pump(void);
bool pc_gx_pump(void);

/// Emulated main memory bounds (the GameCube arena).
uint8_t* pc_mem_base(void);
size_t pc_mem_size(void);

/// Open the disc image; exits with a message on failure.
void pc_dvd_init(const char* path);

/// Write every file of the opened disc into out_dir/files and out_dir/sys.
void pc_dvd_extract(const char* out_dir);

#endif
