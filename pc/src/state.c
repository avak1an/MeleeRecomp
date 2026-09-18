/**
 * @file state.c
 * Save and restore of the whole game state, for rollback.
 *
 * A snapshot holds, in this order:
 *  - the game's writable sections of the executable (its and the engine's
 *    globals; the runtime's own globals are in sections of their own, see
 *    pc_sections.h, and are not part of it),
 *  - the console's main memory (the arena),
 *  - the runtime state the game can observe: registered by the modules
 *    that own it (the frame counter and clock, the OS arena bounds and
 *    alarms, the queued disc, ARAM and card completions, the byte-swap
 *    registries, the audio voices the game holds pointers to).
 *
 * Restoring a snapshot puts every one of those back as it was; the host
 * side (renderer caches, the audio device, files, windows) is left alone
 * and catches up on its own. A snapshot is taken and restored from the
 * same place, VIWaitForRetrace: the game's frame loop sits there between
 * frames, so its stack holds nothing the restored globals disagree with.
 *
 * Registered ranges added after a snapshot was taken (the byte-swap
 * registries are allocated on first use) keep their current content on
 * restore; every such range exists long before any match.
 */
#include "pc_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <intrin.h>

typedef struct {
    uint8_t* base;
    size_t size;
    char name[24];
} StateRange;

static StateRange game_regions[16]; /* sections + arena */
static int n_game_regions;
static StateRange ranges[96]; /* registered runtime state */
static int n_ranges;

static void add_game_region(void* base, size_t size, const char* name)
{
    if (n_game_regions < (int) (sizeof(game_regions) / sizeof(game_regions[0])) && size > 0) {
        game_regions[n_game_regions].base = (uint8_t*) base;
        game_regions[n_game_regions].size = size;
        snprintf(game_regions[n_game_regions].name, sizeof(game_regions[0].name), "%s", name);
        n_game_regions++;
    }
}

static void collect_game_regions(void)
{
    uint8_t* module = (uint8_t*) GetModuleHandleA(NULL);
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*) module;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*) (module + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    unsigned i;
    if (n_game_regions != 0) {
        return;
    }
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        DWORD flags = sec[i].Characteristics;
        char name[9];
        if (!(flags & IMAGE_SCN_MEM_WRITE) || (flags & IMAGE_SCN_MEM_DISCARDABLE)) {
            continue;
        }
        memcpy(name, sec[i].Name, 8);
        name[8] = 0;
        if (strcmp(name, ".pcdata") == 0 || strcmp(name, ".pcbss") == 0) {
            continue;
        }
        add_game_region(module + sec[i].VirtualAddress, sec[i].Misc.VirtualSize, name);
    }
    add_game_region(pc_mem_base(), pc_mem_size(), "arena");
}

int pc_state_game_regions(const uint8_t** bases, size_t* sizes, const char** names, int max)
{
    int i;
    collect_game_regions();
    for (i = 0; i < n_game_regions && i < max; i++) {
        bases[i] = game_regions[i].base;
        sizes[i] = game_regions[i].size;
        names[i] = game_regions[i].name;
    }
    return i;
}

void pc_state_register(void* p, size_t n, const char* name)
{
    int i;
    for (i = 0; i < n_ranges; i++) {
        if (strcmp(ranges[i].name, name) == 0) {
            /* re-registered after a reallocation: the name stays, the
             * address and size follow */
            ranges[i].base = (uint8_t*) p;
            ranges[i].size = n;
            return;
        }
    }
    if (n_ranges == (int) (sizeof(ranges) / sizeof(ranges[0]))) {
        fprintf(stderr, "[pc] state: too many registered ranges (%s)\n", name);
        return;
    }
    ranges[n_ranges].base = (uint8_t*) p;
    ranges[n_ranges].size = n;
    snprintf(ranges[n_ranges].name, sizeof(ranges[0].name), "%s", name);
    n_ranges++;
}

void pc_state_init(void)
{
    collect_game_regions();
    pc_runtime_register_state();
    pc_vi_register_state();
    pc_dvd_register_state();
    pc_aram_register_state();
    pc_card_register_state();
    pc_swap_register_state();
    pc_ax_register_state();
    pc_gx_register_state();
    pc_net_register_state();
}

/* --- The stack and the registers ----------------------------------------
 * The game's frame loop is nested in scene loops that keep locals on the
 * stack (which scene runs, loop counters), so restoring the globals alone
 * would resume a frame-56 world inside a frame-64 loop. A snapshot
 * therefore also holds the stack from the saving call's stack pointer to
 * the thread's stack base, and the callee-saved registers and return
 * address of that call: a restore copies the stack back from a stack of
 * its own and jumps into the saved call, which then returns a second
 * time. */
typedef struct {
    uint32_t esp, ebp, ebx, esi, edi, eip, seh;
} CpuContext;

/// Records the registers; returns 0 now and 1 when resumed by ctx_resume.
static __declspec(naked) int ctx_capture(CpuContext* c)
{
    __asm {
        mov eax, [esp + 4]
        mov [eax], esp
        mov [eax + 4], ebp
        mov [eax + 8], ebx
        mov [eax + 12], esi
        mov [eax + 16], edi
        mov ecx, [esp]
        mov [eax + 20], ecx
        mov ecx, fs:[0]
        mov [eax + 24], ecx
        xor eax, eax
        ret
    }
}

/// Jumps into the call that captured `c` (its stack bytes must be back in
/// place); ctx_capture then returns 1. The saved return address is jumped
/// to rather than popped: the calls made after the capture reuse that
/// stack slot, so the snapshot's copy of it is stale.
static __declspec(naked) void ctx_resume(const CpuContext* c)
{
    __asm {
        mov eax, [esp + 4]
        mov ecx, [eax + 24]
        mov fs:[0], ecx
        mov ebp, [eax + 4]
        mov ebx, [eax + 8]
        mov esi, [eax + 12]
        mov edi, [eax + 16]
        mov esp, [eax]
        add esp, 4
        mov ecx, [eax + 20]
        mov eax, 1
        jmp ecx
    }
}

static uint8_t* stack_base(void)
{
    return (uint8_t*) (uintptr_t) __readfsdword(0x04); /* NT_TIB.StackBase */
}

/* The restore of the stack bytes runs on this stack, so the copy cannot
 * overwrite the frames doing it. */
static uint8_t restore_stack[64 * 1024];
static const uint8_t* restore_src;

/* Snapshot layout: the header, the game regions, the registered ranges in
 * registration order, then the stack bytes. */
typedef struct {
    uint32_t magic;
    uint32_t n_ranges;
    uint64_t size;
    uint32_t frame;
    uint32_t stack_size;
    CpuContext ctx;
} SnapshotHeader;

#define SNAPSHOT_MAGIC 0x4D454C45u /* MELE */

size_t pc_state_size(void)
{
    size_t total = sizeof(SnapshotHeader);
    int i;
    collect_game_regions();
    for (i = 0; i < n_game_regions; i++) {
        total += game_regions[i].size;
    }
    for (i = 0; i < n_ranges; i++) {
        total += ranges[i].size;
    }
    /* the stack's used part, generously: what is above this frame */
    total += (size_t) (stack_base() - (uint8_t*) &total) + 4096;
    return total;
}

int pc_state_save(void* dst, size_t cap)
{
    uint8_t* out = (uint8_t*) dst;
    SnapshotHeader* h = (SnapshotHeader*) out;
    size_t need = sizeof(*h), stack_size;
    int i;
    collect_game_regions();
    for (i = 0; i < n_game_regions; i++) {
        need += game_regions[i].size;
    }
    for (i = 0; i < n_ranges; i++) {
        need += ranges[i].size;
    }
    if (ctx_capture(&h->ctx) != 0) {
        return 2; /* resumed by a restore: the world is the saved one again */
    }
    /* from above the capture's return-address slot to the stack base */
    stack_size = (size_t) (stack_base() - (const uint8_t*) (uintptr_t) (h->ctx.esp + 4));
    need += stack_size;
    if (cap < need) {
        return 0;
    }
    h->magic = SNAPSHOT_MAGIC;
    h->n_ranges = (uint32_t) n_ranges;
    h->size = need;
    h->frame = pc_frame_count;
    h->stack_size = (uint32_t) stack_size;
    out += sizeof(*h);
    for (i = 0; i < n_game_regions; i++) {
        memcpy(out, game_regions[i].base, game_regions[i].size);
        out += game_regions[i].size;
    }
    for (i = 0; i < n_ranges; i++) {
        memcpy(out, ranges[i].base, ranges[i].size);
        out += ranges[i].size;
    }
    memcpy(out, (const void*) (uintptr_t) (h->ctx.esp + 4), stack_size);
    return 1;
}

static void restore_on_own_stack(void)
{
    const SnapshotHeader* h = (const SnapshotHeader*) restore_src;
    const uint8_t* in = restore_src + sizeof(*h);
    int i;
    for (i = 0; i < n_game_regions; i++) {
        in += game_regions[i].size;
    }
    for (i = 0; i < (int) h->n_ranges; i++) {
        in += ranges[i].size;
    }
    memcpy((void*) (uintptr_t) (h->ctx.esp + 4), in, h->stack_size);
    ctx_resume(&h->ctx);
}

static __declspec(naked) void enter_restore(void)
{
    __asm {
        lea esp, [restore_stack + 65536 - 64]
        call restore_on_own_stack
        int 3
    }
}

int pc_state_load(const void* src)
{
    const uint8_t* in = (const uint8_t*) src;
    const SnapshotHeader* h = (const SnapshotHeader*) in;
    int i;
    if (h->magic != SNAPSHOT_MAGIC || h->n_ranges > (uint32_t) n_ranges) {
        fprintf(stderr, "[pc] state: not a snapshot of this process\n");
        return 0;
    }
    in += sizeof(*h);
    for (i = 0; i < n_game_regions; i++) {
        memcpy(game_regions[i].base, in, game_regions[i].size);
        in += game_regions[i].size;
    }
    for (i = 0; i < (int) h->n_ranges; i++) {
        memcpy(ranges[i].base, in, ranges[i].size);
        in += ranges[i].size;
    }
    pc_card_after_restore();
    /* the stack and registers: does not return */
    restore_src = (const uint8_t*) src;
    enter_restore();
    return 1;
}

uint32_t pc_state_frame_of(const void* src)
{
    return ((const SnapshotHeader*) src)->frame;
}
