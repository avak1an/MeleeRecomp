/**
 * @file pc_endian.h
 * Byte-order helpers for the PC build. Data read from the disc is
 * big-endian; the host is little-endian. Game and engine code that consumes
 * disc data swaps it in place at the point of first use, inside
 * `#ifdef TARGET_PC` blocks, using these helpers.
 *
 * Only include this header from such blocks; it is not on the include path
 * of the GameCube build.
 */
#ifndef PC_ENDIAN_H
#define PC_ENDIAN_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define PC_BSWAP16(x) ((uint16_t) (((uint16_t) (x) >> 8) | ((uint16_t) (x) << 8)))
#define PC_BSWAP32(x)                                                                             \
    ((uint32_t) (((uint32_t) (x) >> 24) | (((uint32_t) (x) >> 8) & 0xFF00u) |                    \
                 (((uint32_t) (x) << 8) & 0xFF0000u) | ((uint32_t) (x) << 24)))

static __inline void pc_swap16(void* p)
{
    uint16_t* v = (uint16_t*) p;
    *v = PC_BSWAP16(*v);
}

static __inline void pc_swap32(void* p)
{
    uint32_t* v = (uint32_t*) p;
    *v = PC_BSWAP32(*v);
}

/// Swap every 16-bit word in [p, p + bytes).
static __inline void pc_swap16_range(void* p, size_t bytes)
{
    uint16_t* v = (uint16_t*) p;
    size_t n = bytes / 2, i;
    for (i = 0; i < n; i++) {
        v[i] = PC_BSWAP16(v[i]);
    }
}

/// Swap every 32-bit word in [p, p + bytes).
static __inline void pc_swap32_range(void* p, size_t bytes)
{
    uint32_t* v = (uint32_t*) p;
    size_t n = bytes / 4, i;
    for (i = 0; i < n; i++) {
        v[i] = PC_BSWAP32(v[i]);
    }
}

/// Swap a float in place (same bytes as a 32-bit swap).
#define pc_swapf(p) pc_swap32(p)

#endif /* PC_ENDIAN_H */
