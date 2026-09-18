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
    bool realtime;       ///< pace VIWaitForRetrace to 60 Hz (default for windowed runs)
    bool fast;           ///< --fast: windowed run without pacing
    bool autoplay;       ///< synthesize Start/A presses on port 1 to push through prompts
    const char* input_script; ///< file of scripted port-1 inputs (see pad.c)
    unsigned seed;       ///< if nonzero, the game's RNG seed (else the clock)
    bool headless;       ///< no window, no rendering
    const char* screenshot_dir; ///< if set, dump a BMP of every Nth frame here
    int screenshot_every;
    int screenshot_from; ///< --screenshot-from: first frame to save (default 0)
    int kill_slots; /* --kill: bit mask of player slots to KO repeatedly (0 = off) */
    int kill_frame; /* --kill: first frame */
    int kill_every; /* --kill: repeat period in frames */
    int item_kind;  /* --item: item kind to spawn next to player 1 (-1 = off) */
    int stage;      /* --stage: StKind forced on every VS/Classic match (0 = off) */
    int item_frame; /* --item: frame */       ///< N for the above (default 60)
    int item_slot;  /* --item: player slot the item appears next to (0 = player 1) */
    int p1_char;    /* --char: CharacterKind forced on port 1 at the character select (-1 = off) */
    int p2_char;    /* --char N,M: port 2's character when the select screen opens */
    const char* iso;     ///< path of the disc image (NULL = auto-detect)
    const char* save_dir; ///< memory card files (NULL = "saves")
    bool quiet_stubs;    ///< --quiet-stubs: also silences informational prints
    bool fullscreen;     ///< start in a borderless full-screen window
    int scale;           ///< initial window size, in multiples of 640x480 (default 1)
    int internal_scale;  ///< --internal: render at N x 640x480 (0 = the window's size)
    int msaa;            ///< --msaa: multisample anti-aliasing samples (0 = off)
    int aniso;           ///< --aniso: anisotropic texture filtering (0/1 = off)
    const char* keymap;  ///< keyboard layout file (see pad.c)
    int volume;          ///< audio output volume in percent (default 100)
    bool no_audio;       ///< do not open the audio device
    const char* state_hash; ///< --state-hash: per-frame hash file (determinism.c)
    const char* state_dump; ///< --state-dump N:FILE
    int state_dump_frame;
    const char* state_diff; ///< --state-diff N:FILE
    int state_diff_frame;
    int rollback_test; ///< --rollback-test K: every 2K frames, restore the state from K frames back and re-simulate
} PCConfig;

/// Per-frame state hash / dump / diff and the rollback self-test
/// (determinism.c), from VIWaitForRetrace; `site` is its caller.
void pc_state_frame(const void* site);
void pc_state_close(void);

/* --- Whole-state snapshots (state.c) ------------------------------------ */
/// Runtime state the game can observe, registered by its owner (address
/// and size; re-registering an address updates the size).
void pc_state_register(void* p, size_t n, const char* name);
/// Registers every module's state; call once at start-up.
void pc_state_init(void);
/// The game's writable sections and the arena.
int pc_state_game_regions(const uint8_t** bases, size_t* sizes, const char** names, int max);
size_t pc_state_size(void);
/// Copies the state (memory, registered ranges, the stack and registers of
/// this call) into `dst` (at least pc_state_size() bytes). Returns 1 when
/// saved, 0 if `cap` is too small, and 2 when this very call returns a
/// second time because pc_state_load restored its snapshot.
int pc_state_save(void* dst, size_t cap);
/// Restores a snapshot and resumes inside the pc_state_save call that took
/// it; returns 0 only if the snapshot is unusable.
int pc_state_load(const void* src);
uint32_t pc_state_frame_of(const void* src);
/* per-module registration, called by pc_state_init */
void pc_runtime_register_state(void);
void pc_vi_register_state(void);
void pc_dvd_register_state(void);
void pc_aram_register_state(void);
void pc_card_register_state(void);
void pc_swap_register_state(void);
void pc_ax_register_state(void);
void pc_gx_register_state(void);
/// After a restore: rewrites the card file if the undone frames had written it.
void pc_card_after_restore(void);

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
/// Address of a global by name, with an optional "+offset"; 0 if unknown.
uintptr_t pc_symbol_address(const char* spec);
/// Directory holding melee.exe (no trailing separator).
const char* pc_exe_dir(void);
/// Loose-file overrides: files under DIR (or DIR/files) replace the disc's.
void pc_dvd_add_mod(const char* dir);

/// True when [p, p + bytes) is mapped readable memory (diagnostics only).
int pc_ptr_readable(const void* p, size_t bytes);
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
bool pc_card_pump(void);

/// GameCube controllers on the official USB adapter (gcadapter.c).
struct PADStatus;
int pc_gcadapter_read(int port, struct PADStatus* st);
void pc_gcadapter_rumble(int port, int command);
int pc_gcadapter_present(void);

/// Keyboard layout file for port 1 (pad.c); returns false on errors.
int pc_pad_load_keymap(const char* path);

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
