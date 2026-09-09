/**
 * @file gx.c
 * Graphics processor glue that has to behave even without a renderer: the
 * fifo object handed out by GXInit, the draw-done notification the engine
 * waits on before flipping frame buffers, and the write-gather pipe.
 *
 * The write-gather pipe (GXWGFifo) is a memory-mapped register the game
 * writes vertex data and GX commands into. Here it is a plain variable and
 * every write is discarded. Milestone 3 turns this into a command recorder
 * feeding the renderer.
 */
#include "pc_runtime.h"

#include <Runtime/platform.h>
#include <dolphin/gx.h>
#include <dolphin/gx/GXFifo.h>
#include <dolphin/gx/GXManage.h>
#include <dolphin/gx/GXTexture.h>
#include <dolphin/gx/GXVert.h>

#include <string.h>

volatile PPCWGPipe GXWGFifo;

static GXFifoObj fifo_obj;
static GXDrawDoneCallback draw_done_cb;
static int draw_done_pending;
static u16 draw_sync_token;

GXFifoObj* GXInit(void* base, u32 size)
{
    (void) base;
    (void) size;
    return &fifo_obj;
}

GXFifoObj* GXGetCPUFifo(void)
{
    return &fifo_obj;
}

GXFifoObj* GXGetGPFifo(void)
{
    return &fifo_obj;
}

GXDrawDoneCallback GXSetDrawDoneCallback(GXDrawDoneCallback cb)
{
    GXDrawDoneCallback old = draw_done_cb;
    draw_done_cb = cb;
    return old;
}

/* The GP finishes instantly; the callback is delivered from the pump so the
 * engine sees it at the same points it would see the interrupt. */
void GXSetDrawDone(void)
{
    draw_done_pending = 1;
}

bool pc_gx_pump(void)
{
    if (!draw_done_pending) {
        return false;
    }
    draw_done_pending = 0;
    if (draw_done_cb != NULL) {
        draw_done_cb();
    }
    return true;
}

void GXWaitDrawDone(void)
{
    pc_gx_pump();
}

void GXDrawDone(void)
{
    GXSetDrawDone();
    pc_gx_pump();
}

void GXSetDrawSync(u16 token)
{
    draw_sync_token = token;
}

u16 GXReadDrawSync(void)
{
    return draw_sync_token;
}

void GXFlush(void) {}

/* --- Texture objects ----------------------------------------------------
 * GXTexObj is an opaque 32-byte block. The console packs hardware register
 * images into it; here it holds the plain parameters so the getters (and,
 * later, the renderer) can read them back. */

typedef struct PCTexObj {
    void* image;
    u16 width;
    u16 height;
    u32 format;
    u32 wrap_s;
    u32 wrap_t;
    u32 tlut_name;
    u8 mipmap;
    u8 pad[3];
    void* user_data;
} PCTexObj;

STATIC_ASSERT(sizeof(PCTexObj) <= sizeof(GXTexObj));

void GXInitTexObj(GXTexObj* obj, void* image_ptr, u16 width, u16 height, GXTexFmt format,
                  GXTexWrapMode wrap_s, GXTexWrapMode wrap_t, u8 mipmap)
{
    PCTexObj* t = (PCTexObj*) obj;
    memset(t, 0, sizeof(*t));
    t->image = image_ptr;
    t->width = width;
    t->height = height;
    t->format = format;
    t->wrap_s = wrap_s;
    t->wrap_t = wrap_t;
    t->mipmap = mipmap;
}

void GXInitTexObjCI(GXTexObj* obj, void* image_ptr, u16 width, u16 height, GXTexFmt format,
                    GXTexWrapMode wrap_s, GXTexWrapMode wrap_t, u8 mipmap, u32 tlut_name)
{
    GXInitTexObj(obj, image_ptr, width, height, format, wrap_s, wrap_t, mipmap);
    ((PCTexObj*) obj)->tlut_name = tlut_name;
}

void GXInitTexObjLOD(GXTexObj* obj, GXTexFilter min_filt, GXTexFilter mag_filt, f32 min_lod,
                     f32 max_lod, f32 lod_bias, u8 bias_clamp, u8 do_edge_lod,
                     GXAnisotropy max_aniso)
{
    (void) obj; (void) min_filt; (void) mag_filt; (void) min_lod; (void) max_lod;
    (void) lod_bias; (void) bias_clamp; (void) do_edge_lod; (void) max_aniso;
}

void GXInitTexObjData(GXTexObj* obj, void* image_ptr)
{
    ((PCTexObj*) obj)->image = image_ptr;
}

void GXInitTexObjWrapMode(GXTexObj* obj, GXTexWrapMode sm, GXTexWrapMode tm)
{
    ((PCTexObj*) obj)->wrap_s = sm;
    ((PCTexObj*) obj)->wrap_t = tm;
}

void GXInitTexObjTlut(GXTexObj* obj, u32 tlut_name)
{
    ((PCTexObj*) obj)->tlut_name = tlut_name;
}

void GXInitTexObjUserData(GXTexObj* obj, void* user_data)
{
    ((PCTexObj*) obj)->user_data = user_data;
}

void* GXGetTexObjUserData(const GXTexObj* obj)
{
    return ((const PCTexObj*) obj)->user_data;
}

u16 GXGetTexObjWidth(const GXTexObj* to)
{
    return ((const PCTexObj*) to)->width;
}

u16 GXGetTexObjHeight(const GXTexObj* to)
{
    return ((const PCTexObj*) to)->height;
}

GXTexFmt GXGetTexObjFmt(const GXTexObj* to)
{
    return (GXTexFmt) ((const PCTexObj*) to)->format;
}

/* Tile geometry per format (extern/dolphin/src/dolphin/gx/GXTexture.c). */
static void tex_tile_shift(u32 fmt, u32* row, u32* col)
{
    switch (fmt) {
    case GX_TF_I4:
    case 0x8:
    case GX_TF_CMPR:
    case GX_CTF_R4:
    case GX_CTF_Z4:
        *row = 3;
        *col = 3;
        break;
    case GX_TF_I8:
    case GX_TF_IA4:
    case 0x9:
    case GX_TF_Z8:
    case GX_CTF_RA4:
    case GX_TF_A8:
    case GX_CTF_R8:
    case GX_CTF_G8:
    case GX_CTF_B8:
    case GX_CTF_Z8M:
    case GX_CTF_Z8L:
        *row = 3;
        *col = 2;
        break;
    case GX_TF_IA8:
    case GX_TF_RGB565:
    case GX_TF_RGB5A3:
    case GX_TF_RGBA8:
    case 0xA:
    case GX_TF_Z16:
    case GX_TF_Z24X8:
    case GX_CTF_RA8:
    case GX_CTF_RG8:
    case GX_CTF_GB8:
    case GX_CTF_Z16L:
        *row = 2;
        *col = 2;
        break;
    default:
        *row = *col = 0;
        break;
    }
}

u32 GXGetTexBufferSize(u16 width, u16 height, u32 format, u8 mipmap, u8 max_lod)
{
    u32 sx, sy, tile_bytes, size = 0, nx, ny, level;
    tex_tile_shift(format, &sx, &sy);
    tile_bytes = (format == GX_TF_RGBA8 || format == GX_TF_Z24X8) ? 64 : 32;
    if (mipmap == 1) {
        for (level = 0; level < max_lod; level++) {
            nx = (width + (1u << sx) - 1) >> sx;
            ny = (height + (1u << sy) - 1) >> sy;
            size += tile_bytes * nx * ny;
            if (width == 1 && height == 1) {
                break;
            }
            width = width > 1 ? width >> 1 : 1;
            height = height > 1 ? height >> 1 : 1;
        }
    } else {
        nx = (width + (1u << sx) - 1) >> sx;
        ny = (height + (1u << sy) - 1) >> sy;
        size = nx * ny * tile_bytes;
    }
    return size;
}
