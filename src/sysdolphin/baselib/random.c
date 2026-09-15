#include "random.h"

#ifdef TARGET_PC
#include "pc_runtime.h"
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>
/* MELEE_TRACE_RAND=1: every draw with its caller, to compare two runs */
static void trace_rand(const void* caller)
{
    static int trace = -1;
    if (trace < 0) {
        trace = getenv("MELEE_TRACE_RAND") != NULL;
    }
    if (trace) {
        fprintf(stderr, "[pc] rand: frame %u seed %08x from %s" "%c", pc_frame_count, *seed_ptr, pc_symbol_name(caller), 10);
    }
}
#define TRACE_RAND() trace_rand(_ReturnAddress())
#else
#define TRACE_RAND() (void) 0
#endif

u32 seed = 1;
u32* seed_ptr = &seed;

s32 HSD_Rand(void)
{
    TRACE_RAND();
    *seed_ptr = *seed_ptr * 214013 + 2531011;
    return *seed_ptr >> 0x10;
}

f32 HSD_Randf(void)
{
    TRACE_RAND();
    *seed_ptr = *seed_ptr * 214013 + 2531011;
    return (f32) (*seed_ptr >> 0x10) / (1 << 16);
}

s32 HSD_Randi(s32 max_val)
{
    return max_val * HSD_Rand() / (1 << 16);
}

void _HSD_RandForgetMemory(void* low, void* high)
{
    if (low <= (void*) seed_ptr && (void*) seed_ptr < high) {
        seed_ptr = &seed;
    }
    return;
}
