/**
 * @file aram.c
 * Auxiliary RAM (the GameCube's 16 MB audio/scratch memory) and its DMA
 * request queue. ARAM addresses are offsets into a host buffer; transfers
 * are plain copies whose callbacks complete from pc_pump().
 */
#include "pc_runtime.h"

#include <dolphin/ar.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARAM_SIZE 0x1000000u
#define ARAM_INTERNAL 0x4000u /* reserved by the SDK for the DSP */

static u8* aram;
static u32* stack_index;
static u32 stack_pointer;
static u32 stack_count;
static int initialized;

u32 ARInit(u32* stack_index_addr, u32 num_entries)
{
    (void) num_entries;
    if (aram == NULL) {
        aram = (u8*) calloc(ARAM_SIZE, 1);
    }
    stack_index = stack_index_addr;
    stack_pointer = ARAM_INTERNAL;
    stack_count = 0;
    initialized = 1;
    return stack_pointer;
}

int ARCheckInit(void)
{
    return initialized;
}

u32 ARGetBaseAddress(void)
{
    return ARAM_INTERNAL;
}

u32 ARGetSize(void)
{
    return ARAM_SIZE;
}

u32 ARAlloc(u32 length)
{
    u32 addr = stack_pointer;
    if (stack_pointer + length > ARAM_SIZE) {
        fprintf(stderr, "[pc] ARAlloc: out of ARAM (%u bytes requested)\n", length);
        pc_exit(6);
    }
    stack_pointer += length;
    if (stack_index != NULL) {
        stack_index[stack_count] = length;
    }
    stack_count++;
    return addr;
}

u32 ARFree(u32* length)
{
    if (stack_count > 0) {
        stack_count--;
        if (stack_index != NULL) {
            stack_pointer -= stack_index[stack_count];
            if (length != NULL) {
                *length = stack_index[stack_count];
            }
        }
    }
    return stack_pointer;
}

static void ar_copy(u32 type, u32 mainmem, u32 aram_addr, u32 length)
{
    if (aram_addr + length > ARAM_SIZE) {
        fprintf(stderr, "[pc] ARAM transfer out of range: 0x%x+%u\n", aram_addr, length);
        pc_exit(6);
    }
    if (type == ARAM_DIR_MRAM_TO_ARAM) {
        memcpy(aram + aram_addr, (void*) (uintptr_t) mainmem, length);
    } else {
        memcpy((void*) (uintptr_t) mainmem, aram + aram_addr, length);
    }
}

void ARStartDMA(u32 type, u32 mainmem_addr, u32 aram_addr, u32 length)
{
    ar_copy(type, mainmem_addr, aram_addr, length);
}

u32 ARGetDMAStatus(void)
{
    return 0; /* never busy: transfers are synchronous */
}

/* --- Request queue -------------------------------------------------------- */

#define MAX_ARQ 64
static ARQRequest* arq_pending[MAX_ARQ];
static int arq_head, arq_count;

void ARQInit(void) {}

void ARQPostRequest(struct ARQRequest* request, u32 owner, u32 type, u32 priority, u32 source,
                    u32 dest, u32 length, ARQCallback callback)
{
    request->owner = owner;
    request->type = type;
    request->priority = priority;
    request->source = source;
    request->dest = dest;
    request->length = length;
    request->callback = callback;
    if (type == ARAM_DIR_MRAM_TO_ARAM) {
        ar_copy(type, source, dest, length);
    } else {
        ar_copy(type, dest, source, length);
    }
    if (arq_count >= MAX_ARQ) {
        fprintf(stderr, "[pc] ARQ: too many outstanding requests\n");
        pc_exit(6);
    }
    arq_pending[(arq_head + arq_count) % MAX_ARQ] = request;
    arq_count++;
}

void ARQRemoveRequest(struct ARQRequest* request)
{
    int i;
    for (i = 0; i < arq_count; i++) {
        int idx = (arq_head + i) % MAX_ARQ;
        if (arq_pending[idx] == request) {
            arq_pending[idx] = NULL;
        }
    }
}

bool pc_arq_pump(void)
{
    bool ran = false;
    while (arq_count > 0) {
        ARQRequest* r = arq_pending[arq_head];
        arq_head = (arq_head + 1) % MAX_ARQ;
        arq_count--;
        if (r != NULL && r->callback != NULL) {
            r->callback(r);
            ran = true;
        }
    }
    return ran;
}
