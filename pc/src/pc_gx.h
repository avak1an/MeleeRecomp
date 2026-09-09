/**
 * @file pc_gx.h
 * Internal interface between the GX state/object code (gx.c) and the
 * renderer (gx_render.c). Not included by game code.
 */
#ifndef PC_GX_H
#define PC_GX_H

#include <dolphin/gx.h>

/* GXTexObj is an opaque 32-byte block; this is what the PC build keeps in
 * it. GXTlutObj is 12 bytes. */
typedef struct PCTexObj {
    void* image;
    u16 width;
    u16 height;
    u8 format;
    u8 wrap_s;
    u8 wrap_t;
    u8 mipmap;
    u8 min_filt;
    u8 mag_filt;
    u8 pad[2];
    u32 tlut_name;
    void* user_data;
} PCTexObj;

typedef struct PCTlutObj {
    void* lut;
    u16 fmt;
    u16 n_entries;
    u32 pad;
} PCTlutObj;

/* Immediate-mode vertex data written through the GXPosition / GXColor /
 * GXTexCoord inline functions (see dolphin/gx/GXVert.h, TARGET_PC branch). */
void pc_gx_write_u8(u8 v);
void pc_gx_write_u16(u16 v);
void pc_gx_write_u32(u32 v);
void pc_gx_write_s8(s8 v);
void pc_gx_write_s16(s16 v);
void pc_gx_write_s32(s32 v);
void pc_gx_write_f32(f32 v);

/// Called once the window and GL context exist.
void pc_gx_render_init(void);

/// Called at the start of every frame (after GXCopyDisp presented the last).
void pc_gx_frame_begin(void);

#endif
