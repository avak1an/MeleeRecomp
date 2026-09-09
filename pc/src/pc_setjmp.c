/**
 * @file pc_setjmp.c
 * Host-side longjmp for the Metrowerks __jmp_buf the game embeds in its
 * structures. __setjmp is a macro in Runtime/Gecko_setjmp.h (TARGET_PC).
 */
#include <Runtime/Gecko_setjmp.h>

#include <setjmp.h>

#undef longjmp

void pc_longjmp(__jmp_buf* env, int val)
{
    longjmp(*(jmp_buf*) env, val);
}
