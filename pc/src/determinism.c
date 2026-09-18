/**
 * @file determinism.c
 * Per-frame hashes and dumps of the game's memory, for the determinism
 * check that netplay needs: two runs fed the same inputs must keep the
 * same state frame after frame.
 *
 * The state is the console's main memory (the arena mapped at 0x80000000)
 * plus the executable's writable sections, where the game's and the
 * engine's globals live. The runtime's own variables live there too, so a
 * diff between two runs lists a few expected differences (timers, the
 * hash file handle) next to the real ones; the symbol names tell them
 * apart.
 *
 *   --state-hash FILE     one line per frame: the frame number and a hash
 *   --state-dump N:FILE   at frame N, write the whole state to FILE
 *   --state-diff N:FILE   at frame N, compare the state with FILE and print
 *                         the differing places, symbolized where possible
 */
#include "pc_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <dbghelp.h>

typedef struct {
    const uint8_t* base;
    size_t size;
    char name[12];
} Region;

static Region regions[24];
static int n_regions;
static FILE* hash_file;

/* Runtime variables that legitimately differ between two runs (host heap
 * pointers, handles, the stack cookie, the host clock's origin); they are
 * left out of the hash and the diff. Found with --state-diff between two
 * identical runs: everything else matched. */
static const char* const ignored_symbols[] = {
    "__security_cookie", "__security_cookie_complement", "aram", "image", "disc", "fst", "prof_ms", "reg",
    "reloc_reg", "pc_config", "main_thread", "qpc_start", "sym_ready", "hash_file", "regions", "n_regions",
    "ignored", "n_ignored", "time_epoch", "name", "fst_strings", "reloc_targets",
};

/* MELEE_STATE_IGNORE=entry,entry,... adds entries without a rebuild. */
static const char* all_entries[96];
static int n_entries;

static void collect_entries(void)
{
    static char env_copy[1024];
    const char* env = getenv("MELEE_STATE_IGNORE");
    size_t i;
    n_entries = 0;
    for (i = 0; i < sizeof(ignored_symbols) / sizeof(ignored_symbols[0]); i++) {
        all_entries[n_entries++] = ignored_symbols[i];
    }
    if (env != NULL) {
        char* tok;
        snprintf(env_copy, sizeof(env_copy), "%s", env);
        for (tok = strtok(env_copy, ","); tok != NULL && n_entries < (int) (sizeof(all_entries) / sizeof(all_entries[0]));
             tok = strtok(NULL, ","))
        {
            all_entries[n_entries++] = tok;
        }
    }
}

typedef struct {
    const uint8_t* lo;
    const uint8_t* hi;
} Range;

static Range ignored[128];
static int n_ignored;

/* Every symbol of the module is enumerated, because several of the ignored
 * variables are file-scope statics with the same name in different files
 * (a lookup by name finds only one of them). */
/* An entry is a symbol name, or "name+OFFSET:LENGTH" for part of one. */
static BOOL CALLBACK ignored_enum(PSYMBOL_INFO info, ULONG size, PVOID ctx)
{
    int i;
    (void) ctx;
    for (i = 0; i < n_entries; i++) {
        const char* entry = all_entries[i];
        size_t n = strcspn(entry, "+");
        unsigned off = 0, len = 0;
        if (strlen(info->Name) != n || strncmp(info->Name, entry, n) != 0) {
            continue;
        }
        if (entry[n] == '+') {
            if (sscanf(entry + n + 1, "%i:%u", (int*) &off, &len) != 2) {
                continue;
            }
        }
        if (n_ignored < (int) (sizeof(ignored) / sizeof(ignored[0]))) {
            ignored[n_ignored].lo = (const uint8_t*) (uintptr_t) info->Address + off;
            ignored[n_ignored].hi = ignored[n_ignored].lo + (len != 0 ? len : size != 0 ? size : 4);
            n_ignored++;
        }
        break;
    }
    return TRUE;
}

static void collect_ignored(void)
{
    collect_entries();
    pc_symbol_name(NULL); /* initializes the symbol handler */
    SymEnumSymbols(GetCurrentProcess(), (ULONG64) (uintptr_t) GetModuleHandleA(NULL), "*", ignored_enum, NULL);
}

/// Is `p` inside an ignored variable? Returns the end of that variable.
static const uint8_t* ignored_end(const uint8_t* p)
{
    int i;
    for (i = 0; i < n_ignored; i++) {
        if (p >= ignored[i].lo && p < ignored[i].hi) {
            return ignored[i].hi;
        }
    }
    return NULL;
}

/// The first ignored range starting at or after `p` and before `end`.
static const Range* next_ignored(const uint8_t* p, const uint8_t* end)
{
    const Range* best = NULL;
    int i;
    for (i = 0; i < n_ignored; i++) {
        if (ignored[i].hi > p && ignored[i].lo < end && (best == NULL || ignored[i].lo < best->lo)) {
            best = &ignored[i];
        }
    }
    return best;
}

static void add_region(const void* base, size_t size, const char* name)
{
    if (n_regions < (int) (sizeof(regions) / sizeof(regions[0])) && size > 0) {
        regions[n_regions].base = (const uint8_t*) base;
        regions[n_regions].size = size;
        snprintf(regions[n_regions].name, sizeof(regions[n_regions].name), "%s", name);
        n_regions++;
    }
}

static void collect_regions(void)
{
    const uint8_t* module = (const uint8_t*) GetModuleHandleA(NULL);
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*) module;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*) (module + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    unsigned i;
    if (n_regions != 0) {
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
        /* the runtime's own globals (pc_sections.h) are host-side state:
         * renderer caches, the audio device, timers; MELEE_STATE_ALL=1
         * includes them */
        if ((strcmp(name, ".pcdata") == 0 || strcmp(name, ".pcbss") == 0) && getenv("MELEE_STATE_ALL") == NULL) {
            continue;
        }
        add_region(module + sec[i].VirtualAddress, sec[i].Misc.VirtualSize, name);
    }
    add_region(pc_mem_base(), pc_mem_size(), "arena");
    collect_ignored();
}

static uint64_t hash_bytes(uint64_t h, const uint8_t* p, size_t n)
{
    size_t i;
    for (i = 0; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    for (; i < n; i++) {
        h = (h ^ p[i]) * 0x100000001B3ull;
    }
    return h;
}

static uint64_t state_hash(void)
{
    uint64_t h = 0xCBF29CE484222325ull;
    int r;
    for (r = 0; r < n_regions; r++) {
        const uint8_t* p = regions[r].base;
        const uint8_t* end = p + regions[r].size;
        while (p < end) {
            const Range* skip = next_ignored(p, end);
            const uint8_t* stop = skip != NULL && skip->lo > p ? skip->lo : end;
            if (skip != NULL && skip->lo <= p) {
                p = skip->hi < end ? skip->hi : end;
                continue;
            }
            h = hash_bytes(h, p, (size_t) (stop - p));
            p = stop;
        }
    }
    return h;
}

static void state_dump(const char* path)
{
    FILE* f = fopen(path, "wb");
    int r;
    if (f == NULL) {
        fprintf(stderr, "[pc] state: cannot write %s\n", path);
        return;
    }
    fprintf(f, "melee-state %d\n", n_regions);
    for (r = 0; r < n_regions; r++) {
        fprintf(f, "%s %p %u\n", regions[r].name, (const void*) regions[r].base, (unsigned) regions[r].size);
    }
    for (r = 0; r < n_regions; r++) {
        fwrite(regions[r].base, 1, regions[r].size, f);
    }
    fclose(f);
    fprintf(stderr, "[pc] state: frame %u written to %s\n", pc_frame_count, path);
}

static void state_diff(const char* path)
{
    FILE* f = fopen(path, "rb");
    int count, r, shown = 0, total = 0;
    char line[128];
    if (f == NULL) {
        fprintf(stderr, "[pc] state: cannot read %s\n", path);
        return;
    }
    if (fgets(line, sizeof(line), f) == NULL || sscanf(line, "melee-state %d", &count) != 1 || count != n_regions) {
        fprintf(stderr, "[pc] state: %s is not a dump of this layout\n", path);
        fclose(f);
        return;
    }
    for (r = 0; r < n_regions; r++) {
        char name[16];
        void* base;
        unsigned size;
        if (fgets(line, sizeof(line), f) == NULL || sscanf(line, "%15s %p %u", name, &base, &size) != 3 ||
            base != (const void*) regions[r].base || size != regions[r].size)
        {
            fprintf(stderr, "[pc] state: %s has a different layout (region %d)\n", path, r);
            fclose(f);
            return;
        }
    }
    for (r = 0; r < n_regions; r++) {
        uint8_t* other = (uint8_t*) malloc(regions[r].size);
        size_t i;
        if (other == NULL || fread(other, 1, regions[r].size, f) != regions[r].size) {
            fprintf(stderr, "[pc] state: short read in %s\n", path);
            free(other);
            break;
        }
        for (i = 0; i < regions[r].size;) {
            size_t start, end;
            const uint8_t* skip_to;
            if (other[i] == regions[r].base[i]) {
                i++;
                continue;
            }
            skip_to = ignored_end(regions[r].base + i);
            if (skip_to != NULL) {
                i = (size_t) (skip_to - regions[r].base);
                continue;
            }
            /* one differing range: extend while the next 16 bytes hold a
             * difference too */
            start = i;
            end = i;
            while (end < regions[r].size) {
                size_t k, next = 0;
                for (k = end; k < end + 16 && k < regions[r].size; k++) {
                    if (other[k] != regions[r].base[k]) {
                        next = k + 1;
                    }
                }
                if (next == 0) {
                    break;
                }
                end = next;
            }
            {
                const uint8_t* p = regions[r].base + start;
                const char* sym = strcmp(regions[r].name, "arena") == 0 ? "" : pc_symbol_name(p);
                total++;
                if (shown < 80) {
                    fprintf(stderr, "[pc] state diff: %-8s %p +%-6u %s | now %02x%02x%02x%02x was %02x%02x%02x%02x\n",
                            regions[r].name, (const void*) p, (unsigned) (end - start), sym, p[0], p[1], p[2], p[3],
                            other[start], other[start + 1], other[start + 2], other[start + 3]);
                    shown++;
                }
            }
            i = end;
        }
        free(other);
    }
    fclose(f);
    fprintf(stderr, "[pc] state: frame %u, %d differing range(s) against %s%s\n", pc_frame_count, total, path,
            total > shown ? " (first 80 shown)" : "");
}

/* --- Rollback self-test -------------------------------------------------
 * --rollback-test K: at every frame that is an odd multiple of K the state
 * is saved; at the next even multiple it is restored, so the game runs the
 * last K frames again; their hashes must equal the ones recorded the first
 * time. A snapshot is only restored from the retrace site it was taken at. */
static uint64_t* frame_hashes;
static uint32_t frame_hashes_cap;
static void* snapshot;
static size_t snapshot_cap;
static const void* snapshot_site;
static uint32_t replay_until;
static int replay_mismatches, replays, saves;
static double save_ms;

/// Returns 1 when the call resumed from a restore (the frame is now the
/// snapshot's; nothing else must run for it).
static int rollback_test(const void* site, uint64_t h)
{
    uint32_t k = (uint32_t) pc_config.rollback_test, f = pc_frame_count;
    if (f >= frame_hashes_cap) {
        uint32_t cap = f + 4096;
        frame_hashes = (uint64_t*) realloc(frame_hashes, cap * sizeof(uint64_t));
        memset(frame_hashes + frame_hashes_cap, 0, (cap - frame_hashes_cap) * sizeof(uint64_t));
        frame_hashes_cap = cap;
    }
    if (replay_until != 0) {
        if (frame_hashes[f] != h) {
            fprintf(stderr, "[pc] rollback: frame %u differs after re-simulation (%016llx, was %016llx)\n", f,
                    (unsigned long long) h, (unsigned long long) frame_hashes[f]);
            replay_mismatches++;
        }
        if (f >= replay_until) {
            replay_until = 0;
            replays++;
        }
        return 0;
    }
    frame_hashes[f] = h;
    if (f % (2 * k) == k) {
        size_t need = pc_state_size();
        int r;
        if (need > snapshot_cap) {
            free(snapshot);
            snapshot = malloc(need);
            snapshot_cap = snapshot != NULL ? need : 0;
        }
        if (snapshot == NULL) {
            return 0;
        }
        {
            LARGE_INTEGER t0, t1, fq;
            QueryPerformanceFrequency(&fq);
            QueryPerformanceCounter(&t0);
            r = pc_state_save(snapshot, snapshot_cap);
            QueryPerformanceCounter(&t1);
            if (r == 1) {
                save_ms += (double) (t1.QuadPart - t0.QuadPart) * 1000.0 / (double) fq.QuadPart;
                saves++;
            }
        }
        if (r == 2) {
            if (getenv("MELEE_TRACE_ROLLBACK") != NULL) {
                fprintf(stderr, "[pc] rollback: resumed at frame %u\n", pc_frame_count);
            }
            return 1; /* restored: frame f - k begins again */
        }
        if (getenv("MELEE_TRACE_ROLLBACK") != NULL) {
            fprintf(stderr, "[pc] rollback: frame %u: snapshot %s (%u bytes)\n", f, r == 1 ? "taken" : "FAILED",
                    (unsigned) need);
        }
        if (r == 1) {
            snapshot_site = site;
        }
    } else if (f % (2 * k) == 0 && f > 0 && snapshot != NULL && pc_state_frame_of(snapshot) == f - k) {
        if (snapshot_site != site) {
            fprintf(stderr, "[pc] rollback: frame %u: retrace site differs from frame %u's, not restored\n", f, f - k);
            return 0;
        }
        replay_until = f;
        if (getenv("MELEE_TRACE_ROLLBACK") != NULL) {
            fprintf(stderr, "[pc] rollback: frame %u: restoring frame %u\n", f, f - k);
        }
        pc_state_load(snapshot); /* resumes inside the save at frame f - k */
        replay_until = 0;
    }
    return 0;
}

void pc_state_frame(const void* site)
{
    static int inited;
    if (pc_config.state_hash == NULL && pc_config.state_dump == NULL && pc_config.state_diff == NULL &&
        pc_config.rollback_test == 0)
    {
        return;
    }
    if (!inited) {
        inited = 1;
        pc_state_init();
    }
    collect_regions();
    if (pc_config.rollback_test > 0 && getenv("MELEE_TRACE_ROLLBACK") != NULL) {
        extern int pc_gx_draws_this_frame(void);
        fprintf(stderr, "[pc] rollback: retrace %u, %d draws since the last present%s" "%c", pc_frame_count,
                pc_gx_draws_this_frame(), replay_until != 0 ? " (replay)" : "", 10);
    }
    if (pc_config.rollback_test > 0 && rollback_test(site, state_hash())) {
        return; /* just restored: this frame's bookkeeping was done the first time */
    }
    if (pc_config.state_hash != NULL) {
        if (hash_file == NULL) {
            hash_file = fopen(pc_config.state_hash, "w");
            if (hash_file == NULL) {
                fprintf(stderr, "[pc] state: cannot write %s\n", pc_config.state_hash);
                pc_config.state_hash = NULL;
                return;
            }
        }
        fprintf(hash_file, "%u %016llx\n", pc_frame_count, (unsigned long long) state_hash());
    }
    /* with the rollback test, a dump is taken on the first pass over a
     * frame and a diff on the re-simulation of it, so
     * --state-dump N:F --state-diff N:F shows what a restore left different */
    if (pc_config.state_dump != NULL && pc_frame_count == (uint32_t) pc_config.state_dump_frame && replay_until == 0) {
        state_dump(pc_config.state_dump);
    }
    if (pc_config.state_diff != NULL && pc_frame_count == (uint32_t) pc_config.state_diff_frame &&
        (pc_config.rollback_test == 0 || replay_until != 0))
    {
        state_diff(pc_config.state_diff);
    }
}

void pc_state_close(void)
{
    if (hash_file != NULL) {
        fclose(hash_file);
        hash_file = NULL;
    }
    if (pc_config.rollback_test > 0) {
        fprintf(stderr,
                "[pc] rollback: %d re-simulation(s) of %d frames, %d frame hash mismatch(es); %d snapshot(s) of %u KB, "
                "%.1f ms each\n",
                replays, pc_config.rollback_test, replay_mismatches, saves, (unsigned) (snapshot_cap / 1024),
                saves != 0 ? save_ms / saves : 0.0);
    }
}
