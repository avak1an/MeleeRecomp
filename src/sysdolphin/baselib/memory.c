#include "memory.h"
#ifdef TARGET_PC
#include <pc_hsd_swap.h>
#endif

#include <Runtime/platform.h>

#include "debug.h"
#include "initialize.h"
#include <dolphin/os/OSAlloc.h>

void HSD_Free(void* ptr)
{
#ifdef TARGET_PC
    /* the block may hold a parsed archive: drop its swap/relocation
     * records so the memory can be reused (the OSAlloc cell header, 32
     * bytes before the block, holds the cell size) */
    if (ptr != NULL && pc_swap_is_archive(ptr)) {
        long cell_size = *(long*) ((u8*) ptr - 0x20 + 8);
        if (cell_size > 0x20) {
            pc_swap_forget_range(ptr, (size_t) (cell_size - 0x20));
        }
    }
#endif
    OSFreeToHeap(HSD_GetHeap(), ptr);
}

void* HSD_MemAlloc(ssize_t size)
{
    void* adr;

    if (size <= 0) {
        return NULL;
    }

    adr = OSAllocFromHeap(HSD_GetHeap(), size);
    HSD_ASSERT(52, adr);

    return adr;
}
