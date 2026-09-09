/**
 * @file pc_platform.h
 * Force-included into every translation unit of the PC build
 * (see pc/CMakeLists.txt). Anything the GameCube toolchain provided
 * implicitly, or that MSVC spells differently, is patched up here so the
 * game sources themselves stay untouched wherever possible.
 */
#ifndef PC_PLATFORM_H
#define PC_PLATFORM_H

#define TARGET_PC 1

/* MSVC-only for now. Keep the checks so a future clang build gets a clear
 * error instead of silently compiling something wrong. */
#if !defined(_MSC_VER)
#error "pc_platform.h currently supports MSVC only"
#endif

#if defined(_WIN64) || defined(__LP64__) || (defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ != 4)
#error "The PC build must be 32-bit: HSD archives relocate 32-bit pointers in place"
#endif

/* Alignment attributes: the GameCube needs 32-byte alignment for DMA; the PC
 * build does not, and MSVC puts __declspec(align) in a different position,
 * so the attribute is compiled out. Defined here so <dolphin/types.h> picks
 * it up (it only defines it when not already defined). */
#define ATTRIBUTE_ALIGN(num)

/* Attribute spellings that <Runtime/platform.h> resolves to GNU syntax. */
#define ATTRIBUTE_NORETURN __declspec(noreturn)
#define DOLPHIN_ATTRIBUTE_NORETURN __declspec(noreturn)
#define UNUSED

/* PowerPC intrinsics used directly by the game/engine sources.
 * __frsqrte is the reciprocal square-root *estimate*; callers refine it with
 * Newton-Raphson steps, so returning the exact value is fine. */
#define __frsqrte(x) (1.0 / sqrt((double) (x)))
#define __fabs(x) fabs((double) (x))
#define __fabsf(x) fabsf(x)
#define sqrtf__Ff(x) sqrtf(x)
#define sqrtf_accurate(x) sqrtf(x)

/* MSVC's math.h only exposes M_PI and friends with this defined. */
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef _CRT_NONSTDC_NO_WARNINGS
#define _CRT_NONSTDC_NO_WARNINGS
#endif

#include <math.h>
#include <stddef.h>
#include <stdint.h>

/* The game ships its own implementations of these (src/melee/lb/lbtrigf.c,
 * lb_00CE.c), matching the GameCube's results. The host CRT defines some of
 * them inline in <math.h>, so rename the game's versions and route every
 * caller to them. */
#define atan2f melee_atan2f
#define acosf melee_acosf
#define asinf melee_asinf
#define expf melee_expf
#define powf melee_powf
float melee_atan2f(float y, float x);
float melee_acosf(float x);
float melee_asinf(float x);
float melee_expf(float x);
float melee_powf(float x, float y);

/* Metrowerks setjmp: the game embeds __jmp_buf (a 248-byte PowerPC register
 * image) inside its own structures and calls __setjmp/longjmp on it. On PC we
 * keep the struct shape (so struct layouts and sizes are unchanged) and store
 * the host jmp_buf inside it. See pc/src/pc_setjmp.c. */
#define PC_JMP_BUF_HOST_BYTES 248

#endif /* PC_PLATFORM_H */
