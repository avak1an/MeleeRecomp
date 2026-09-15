/**
 * @file gx_render.c
 * GX on OpenGL.
 *
 * Geometry arrives two ways: immediate mode (GXBegin followed by component
 * writes from the inline functions in GXVert.h) and display lists (the same
 * command format, big-endian, straight from disc). Both are decoded with the
 * current vertex descriptor (GXSetVtxDesc), attribute formats
 * (GXSetVtxAttrFmt) and index arrays (GXSetArray) into a plain vertex, which
 * is then transformed on the CPU the way the console's XF unit would:
 * position/normal matrices from the matrix memory, per-vertex lighting from
 * the channel controls, texture-coordinate generation. The fragment side is
 * a GLSL shader generated from the TEV stage configuration.
 *
 * Coordinate conventions: GX view space matches GL (right-handed, camera
 * along -Z). GX clip-space depth is [-w, 0]; the vertex shader remaps it to
 * GL's [-w, w]. The GX viewport origin is the top-left corner.
 */
#include "pc_gl.h"
#include "pc_gx.h"
#include <Runtime/platform.h>
#include "pc_runtime.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#define EFB_W 640
#define EFB_H 480
#define NUM_ATTR 26 /* GX_VA_MAX_ATTR */
#define MTX_ROWS 128
#define MAX_VERTS 65536

/* --- State ------------------------------------------------------------ */

typedef struct VtxAttrFmt {
    u8 cnt, type, frac;
} VtxAttrFmt;

typedef struct TevStage {
    u8 ca[4], aa[4];          /* color / alpha inputs a,b,c,d */
    u8 cop, cbias, cscale, cclamp, creg;
    u8 aop, abias, ascale, aclamp, areg;
    u8 coord, map, chan;      /* GXSetTevOrder */
    u8 kcsel, kasel;
    u8 ras_swap, tex_swap;
    /* GXSetTevIndirect: the texture coordinate is offset by an indirect
     * texture lookup (heat haze, water); ind_coord/ind_map are resolved
     * from the indirect stage when the shader key is built */
    u8 ind_on, ind_stage, ind_fmt, ind_bias, ind_mtx, ind_addprev, ind_coord, ind_map;
} TevStage;

typedef struct ChanCtrl {
    u8 enable, amb_src, mat_src, diff_fn, attn_fn;
    u32 light_mask;
} ChanCtrl;

typedef struct Light {
    float pos[3], dir[3];
    float color[4];
    float a0, a1, a2, k0, k1, k2;
} Light;

typedef struct TexGen {
    u8 type, src, normalize;
    u32 mtx, pt_mtx;
} TexGen;

static struct {
    u8 vcd[NUM_ATTR];
    VtxAttrFmt vat[8][NUM_ATTR];
    const u8* array_base[NUM_ATTR];
    u8 array_stride[NUM_ATTR];

    float mtx[MTX_ROWS][4];   /* position/texture matrix memory, rows of 4 */
    float nrm[32][3][3];      /* normal matrix memory */
    u32 current_mtx;

    float proj[4][4];
    u8 proj_type;
    float vp[6];              /* left top wd ht near far */
    u32 scissor[4];

    u8 cull;
    u8 z_enable, z_func, z_update;
    u8 blend_mode, blend_src, blend_dst, logic_op;
    u8 color_update, alpha_update;
    u8 alpha_comp0, alpha_ref0, alpha_op, alpha_comp1, alpha_ref1;
    GXColor clear_color;
    u32 clear_z;

    TevStage tev[16];
    u8 num_tev, num_texgen, num_chans;
    struct {
        u8 coord, map;        /* GXSetIndTexOrder */
    } ind_order[4];
    float ind_mtx[4][2][3];   /* GXSetIndTexMtx, scaled by 2^scale_exp; [0] is GX_ITM_OFF */
    TexGen texgen[8];
    ChanCtrl chan[4];         /* COLOR0, COLOR1, ALPHA0, ALPHA1 */
    float mat_color[2][4], amb_color[2][4];
    float tev_reg[4][4];      /* PREV, REG0, REG1, REG2 */
    float kcolor[4][4];
    u8 swap_table[4][4];
    Light lights[8];

    PCTexObj texmap[8];
    PCTlutObj tlut[20];
    u8 fog_type;
    float fog_start, fog_end, fog_near, fog_far;
    float fog_color[4];
} gx;

/* --- Vertex stream ------------------------------------------------------ */

typedef struct Vertex {
    float pos[3];
    float col[2][4];
    float tex[8][2];
    float nrm[3];
    float bnm[3], tan[3]; /* GX_VA_NBT: binormal and tangent (emboss bump mapping) */
    u8 pnmtx;
    u8 texmtx[8];
} Vertex;

typedef struct GLVertex {
    float pos[3];
    float col[2][4];
    float tex[8][2];
} GLVertex;

/* GPU vertex path (the default): the vertex goes up untransformed and a
 * generated vertex shader does the matrix, the lighting and the texture
 * coordinate generation, reading matrices, lights and material colours
 * from blocks in a float texture. A block is appended only when its GX
 * state changed since the last draw, so a batch spans any number of
 * matrix loads. blk = (matrix block, texture-matrix block, light block,
 * material block) as texel indices; pnm = the vertex's position matrix
 * row (GX_PNMTX0..9 = 0, 3, ..., 27). MELEE_GX_CPU=1 selects the CPU
 * path (transform_vertex), which is also what the draw log measures. */
typedef struct GLVertexG {
    float pos[3];
    float nrm[3], bnm[3], tan[3];
    float col[2][4];
    float tex[8][2];
    float blk[4];
    float pnm;
} GLVertexG;

static int gpu_path = -1;
static GLVertexG* gverts;
static u32 gverts_cap;

/* Vertex ring: with GL 4.4 the vertices of every batch are written straight
 * into a persistently mapped buffer and drawn from there with a base
 * vertex, so a draw call copies nothing (the driver's copy of client-side
 * arrays was the largest cost of a draw). The ring has three regions
 * guarded by fences: a batch never straddles two, and a region is only
 * written again once the draws that read it last time are done. Without
 * the extensions (or with MELEE_GX_NORING=1) the client arrays stay. */
#define RING_BYTES (48u << 20)
#define RING_REGIONS 3
static int use_ring = -1;
static u8* ring_map;
static GLuint ring_vbo;
static u32 ring_stride, ring_verts, ring_region_verts;
static u32 ring_base_v, ring_pos_v; /* the current batch's first vertex; the next free one */
static int ring_region = -1;
static GLsync ring_fence[RING_REGIONS];
#define PARAM_W 1024
#define PARAM_H 1024
#define PARAM_TEXELS (PARAM_W * PARAM_H)
#define PARAM_UNIT 8
#define PN_BLOCK 60   /* matrix rows 0..29 + normal matrices 0..9 (3 rows each) */
#define TX_BLOCK 164  /* matrix rows 30..127 + normal matrices 10..31 */
#define LT_BLOCK 32   /* 8 lights x 4 texels */
#define MT_BLOCK 4    /* material 0, 1, ambient 0, 1 */
static float* param_img;          /* CPU copy of the parameter texture */
static u32 param_pos, param_uploaded; /* texel cursors */
static GLuint param_tex;
static int blk_idx[4] = { -1, -1, -1, -1 };
static int blk_dirty[4] = { 1, 1, 1, 1 };

static u8 imm_buf[1 << 20];
static Vertex first_vertex; /* first vertex of the current draw (draw log) */
static u32 imm_len;
static u32 imm_expected;
static u8 imm_prim, imm_vat;
static u16 imm_nverts;
static int imm_active;
static u32 imm_vsize;

static GLVertex* glverts;
static u32 glverts_cap;
static u16* indices;
static u32 indices_cap;
/* Consecutive draws whose GL state is identical are merged into one
 * glDrawElements: their vertices accumulate in glverts and their triangles
 * in indices, and every GL state change, copy, clear, readback or present
 * flushes them first. A results screen has 6800 draws a frame, most of
 * them strips of one model sharing every state. */
static u32 batch_verts, batch_idx, batch_min; /* batch_min: first vertex the batch refers to */
static u32 stats_gl_draws;
static float aniso_max; /* the driver's anisotropic filtering limit (0 = none) */
static double prof_ms[9]; /* frame, draws, shader, texture, copy, present, vertex decode+transform, param upload, gl draw */
static GLuint unit_tex[9];      /* the texture bound to each unit (0xFFFFFFFF = unknown); 8 = the parameter texture */
static int active_unit;
static void flush_batch(void);
static void use_unit(int unit);
static void bind_unit(int unit, GLuint tex);
static void forget_texture(GLuint tex);

static u32 vertex_stream_size(u8 vat);
static int rendering; /* GL context exists */
static u32 frame_no;
static int stats_draws, stats_verts;
static int debug_log;  /* MELEE_GX_DEBUG: log the first draws of a frame */
static int debug_flat; /* MELEE_GX_FLAT: magenta fragments, no alpha test */
int pc_debug_in_fighter;      /* set by the fighter draw routine: 1 + kind while its model is drawn */
int pc_debug_in_item;         /* set by the item draw routine: 1 + kind while its model is drawn */
unsigned int pc_debug_rendermode; /* the last material's render mode (HSD_MObjSetup) */
unsigned char pc_debug_mat_colors[16];
static int fighter_draws;     /* draws issued for fighters this frame */
int pc_debug_fighter_counts[4]; /* jobj / dobj / pobj / display-list calls while drawing fighters */
static int debug_nocull;      /* MELEE_GX_NOCULL: never cull faces */
static int debug_noalpha;     /* MELEE_GX_NOALPHA: skip the alpha test */
static int debug_litonly;     /* MELEE_GX_LITONLY: textures read as white (shows lighting only) */
static u32 debug_log_frame;   /* MELEE_GX_LOG_FRAME=N: log every draw of frame N */

/* --- Helpers ------------------------------------------------------------ */

static u32 be16(const u8* p)
{
    return ((u32) p[0] << 8) | p[1];
}
static u32 be32(const u8* p)
{
    return ((u32) p[0] << 24) | ((u32) p[1] << 16) | ((u32) p[2] << 8) | p[3];
}
static float bef32(const u8* p)
{
    union {
        u32 u;
        float f;
    } u;
    u.u = be32(p);
    return u.f;
}

static u32 comp_size(u8 type)
{
    switch (type) {
    case GX_U8:
    case GX_S8:
        return 1;
    case GX_U16:
    case GX_S16:
        return 2;
    default:
        return 4;
    }
}

static u32 color_size(u8 type)
{
    switch (type) {
    case GX_RGB565:
    case GX_RGBA4:
        return 2;
    case GX_RGB8:
    case GX_RGBA6:
        return 3;
    default:
        return 4;
    }
}

static u32 attr_comps(u8 attr, const VtxAttrFmt* f)
{
    switch (attr) {
    case GX_VA_POS:
        return f->cnt == GX_POS_XY ? 2 : 3;
    case GX_VA_NRM:
    case GX_VA_NBT: /* normal, binormal, tangent: nine components */
        return f->cnt == GX_NRM_XYZ ? 3 : 9;
    case GX_VA_CLR0:
    case GX_VA_CLR1:
        return 1;
    default:
        return f->cnt == GX_TEX_S ? 1 : 2;
    }
}

/// Bytes one attribute occupies in a vertex stream (direct data or index).
static u32 attr_stream_size(u8 attr, u8 vcd, const VtxAttrFmt* f)
{
    if (vcd == GX_NONE) {
        return 0;
    }
    if (attr <= GX_VA_TEX7MTXIDX) {
        return 1;
    }
    if (vcd == GX_INDEX8 || vcd == GX_INDEX16) {
        u32 n = vcd == GX_INDEX8 ? 1 : 2;
        return attr == GX_VA_NRM && f->cnt == GX_NRM_NBT3 ? 3 * n : n; /* NBT3: one index per vector */
    }
    if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
        return color_size(f->type);
    }
    return attr_comps(attr, f) * comp_size(f->type);
}

static float read_comp(const u8* p, u8 type, u8 frac, int big)
{
    float v;
    switch (type) {
    case GX_U8:
        v = (float) p[0];
        break;
    case GX_S8:
        v = (float) (s8) p[0];
        break;
    case GX_U16:
        v = (float) (big ? be16(p) : *(const u16*) p);
        break;
    case GX_S16:
        v = (float) (s16) (big ? be16(p) : *(const u16*) p);
        break;
    default:
        return big ? bef32(p) : *(const float*) p;
    }
    return frac ? v / (float) (1u << frac) : v;
}

static void read_color(const u8* p, u8 type, int big, float out[4])
{
    u32 v;
    switch (type) {
    case GX_RGB565:
        v = big ? be16(p) : *(const u16*) p;
        out[0] = ((v >> 11) & 31) / 31.0f;
        out[1] = ((v >> 5) & 63) / 63.0f;
        out[2] = (v & 31) / 31.0f;
        out[3] = 1.0f;
        break;
    case GX_RGB8:
        out[0] = p[0] / 255.0f;
        out[1] = p[1] / 255.0f;
        out[2] = p[2] / 255.0f;
        out[3] = 1.0f;
        break;
    case GX_RGBX8:
        out[0] = p[0] / 255.0f;
        out[1] = p[1] / 255.0f;
        out[2] = p[2] / 255.0f;
        out[3] = 1.0f;
        break;
    case GX_RGBA4:
        v = big ? be16(p) : *(const u16*) p;
        out[0] = ((v >> 12) & 15) / 15.0f;
        out[1] = ((v >> 8) & 15) / 15.0f;
        out[2] = ((v >> 4) & 15) / 15.0f;
        out[3] = (v & 15) / 15.0f;
        break;
    case GX_RGBA6:
        v = ((u32) p[0] << 16) | ((u32) p[1] << 8) | p[2];
        if (!big) {
            v = ((u32) p[2] << 16) | ((u32) p[1] << 8) | p[0];
        }
        out[0] = ((v >> 18) & 63) / 63.0f;
        out[1] = ((v >> 12) & 63) / 63.0f;
        out[2] = ((v >> 6) & 63) / 63.0f;
        out[3] = (v & 63) / 63.0f;
        break;
    default: /* RGBA8: immediate writes are GXColor4u8 -> r,g,b,a bytes, or
                GXColor1u32 -> host u32 0xRRGGBBAA */
        if (big) {
            out[0] = p[0] / 255.0f;
            out[1] = p[1] / 255.0f;
            out[2] = p[2] / 255.0f;
            out[3] = p[3] / 255.0f;
        } else {
            out[0] = p[0] / 255.0f;
            out[1] = p[1] / 255.0f;
            out[2] = p[2] / 255.0f;
            out[3] = p[3] / 255.0f;
        }
        break;
    }
}

/* --- Vertex decoding ------------------------------------------------------ */

static const u8* decode_attr_data(u8 attr, const u8* p, const VtxAttrFmt* f, int big, Vertex* v)
{
    u32 i, n;
    switch (attr) {
    case GX_VA_POS:
        n = attr_comps(attr, f);
        v->pos[2] = 0.0f;
        for (i = 0; i < n; i++) {
            v->pos[i] = read_comp(p + i * comp_size(f->type), f->type, f->frac, big);
        }
        return p + n * comp_size(f->type);
    case GX_VA_NRM:
    case GX_VA_NBT:
        /* an NBT vertex carries the normal first, then the binormal and
         * tangent that the emboss bump texgen offsets along */
        n = attr_comps(attr, f);
        for (i = 0; i < 3; i++) {
            v->nrm[i] = read_comp(p + i * comp_size(f->type), f->type, f->frac, big);
        }
        if (n == 9) {
            for (i = 0; i < 3; i++) {
                v->bnm[i] = read_comp(p + (3 + i) * comp_size(f->type), f->type, f->frac, big);
                v->tan[i] = read_comp(p + (6 + i) * comp_size(f->type), f->type, f->frac, big);
            }
        }
        return p + n * comp_size(f->type);
    case GX_VA_CLR0:
    case GX_VA_CLR1:
        read_color(p, f->type, big, v->col[attr - GX_VA_CLR0]);
        return p + color_size(f->type);
    default: {
        int t = attr - GX_VA_TEX0;
        n = attr_comps(attr, f);
        v->tex[t][1] = 0.0f;
        for (i = 0; i < n; i++) {
            v->tex[t][i] = read_comp(p + i * comp_size(f->type), f->type, f->frac, big);
        }
        return p + n * comp_size(f->type);
    }
    }
}

/// Decode one vertex from a stream; returns the advanced stream pointer.
/* the attributes present in the vertex stream, in stream order (built once
 * per draw instead of scanning all 26 descriptors for every vertex) */
static u8 active_attr[NUM_ATTR];
static u32 n_active_attr;

static void build_active_attrs(void)
{
    u32 attr;
    n_active_attr = 0;
    for (attr = 0; attr < NUM_ATTR; attr++) {
        if (gx.vcd[attr] != GX_NONE) {
            active_attr[n_active_attr++] = (u8) attr;
        }
    }
}

static const u8* decode_vertex(const u8* p, u8 vat, int big, Vertex* v)
{
    u32 k;
    memset(v, 0, sizeof(*v));
    v->col[0][0] = v->col[0][1] = v->col[0][2] = v->col[0][3] = 1.0f;
    v->col[1][0] = v->col[1][1] = v->col[1][2] = v->col[1][3] = 1.0f;
    v->pnmtx = (u8) gx.current_mtx;
    for (k = 0; k < n_active_attr; k++) {
        u32 attr = active_attr[k];
        u8 vcd = gx.vcd[attr];
        const VtxAttrFmt* f = &gx.vat[vat][attr];
        if (attr == GX_VA_PNMTXIDX) {
            v->pnmtx = *p++;
            continue;
        }
        if (attr <= GX_VA_TEX7MTXIDX) {
            v->texmtx[attr - GX_VA_TEX0MTXIDX] = *p++;
            continue;
        }
        if (vcd == GX_DIRECT) {
            p = decode_attr_data(attr, p, f, big, v);
        } else if (attr == GX_VA_NRM && f->cnt == GX_NRM_NBT3) {
            /* three indices, one each for the normal, binormal and tangent,
             * each three components into the same array */
            u32 k;
            for (k = 0; k < 3; k++) {
                u32 idx = vcd == GX_INDEX8 ? *p : (big ? be16(p) : *(const u16*) p);
                const u8* base = gx.array_base[attr];
                p += vcd == GX_INDEX8 ? 1 : 2;
                if (base != NULL) {
                    const u8* q = base + idx * gx.array_stride[attr] + k * 3 * comp_size(f->type);
                    float* dst = k == 0 ? v->nrm : k == 1 ? v->bnm : v->tan;
                    u32 c;
                    for (c = 0; c < 3; c++) {
                        dst[c] = read_comp(q + c * comp_size(f->type), f->type, f->frac, 1);
                    }
                }
            }
        } else {
            u32 idx = vcd == GX_INDEX8 ? *p : (big ? be16(p) : *(const u16*) p);
            const u8* base = gx.array_base[attr];
            p += vcd == GX_INDEX8 ? 1 : 2;
            if (base != NULL) {
                /* array data is always in console (big-endian) order */
                decode_attr_data(attr, base + idx * gx.array_stride[attr], f, 1, v);
            }
        }
    }
    return p;
}

/* --- Transform, lighting, texgen ------------------------------------------ */

static void mtx_mul_point(const float m[][4], const float in[3], float out[3])
{
    out[0] = m[0][0] * in[0] + m[0][1] * in[1] + m[0][2] * in[2] + m[0][3];
    out[1] = m[1][0] * in[0] + m[1][1] * in[1] + m[1][2] * in[2] + m[1][3];
    out[2] = m[2][0] * in[0] + m[2][1] * in[1] + m[2][2] * in[2] + m[2][3];
}

static void normalize3(float v[3])
{
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-12f) {
        v[0] /= l;
        v[1] /= l;
        v[2] /= l;
    }
}

static float clamp01(float x)
{
    return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
}

/// GX per-vertex lighting for one channel (color or alpha part).
static void light_channel(const ChanCtrl* c, int chan, const float* vtx_col, const float* mat_reg,
                          const float* amb_reg, const float pos[3], const float nrm[3],
                          float out[4])
{
    const float* mat = c->mat_src == GX_SRC_VTX ? vtx_col : mat_reg;
    float illum[4];
    int i;
    if (!c->enable) {
        memcpy(out, mat, 4 * sizeof(float));
        return;
    }
    memcpy(illum, c->amb_src == GX_SRC_VTX ? vtx_col : amb_reg, 4 * sizeof(float));
    for (i = 0; i < 8; i++) {
        const Light* l;
        float ldir[3], dist, attn, diff;
        if (!(c->light_mask & (1u << i))) {
            continue;
        }
        l = &gx.lights[i];
        ldir[0] = l->pos[0] - pos[0];
        ldir[1] = l->pos[1] - pos[1];
        ldir[2] = l->pos[2] - pos[2];
        dist = sqrtf(ldir[0] * ldir[0] + ldir[1] * ldir[1] + ldir[2] * ldir[2]);
        if (dist > 1e-12f) {
            ldir[0] /= dist;
            ldir[1] /= dist;
            ldir[2] /= dist;
        }
        attn = 1.0f;
        if (c->attn_fn == GX_AF_SPOT) {
            float cosa = -(ldir[0] * l->dir[0] + ldir[1] * l->dir[1] + ldir[2] * l->dir[2]);
            float a = l->a0 + l->a1 * cosa + l->a2 * cosa * cosa;
            float k = l->k0 + l->k1 * dist + l->k2 * dist * dist;
            if (a < 0.0f) {
                a = 0.0f;
            }
            attn = k > 1e-12f ? a / k : 0.0f;
        } else if (c->attn_fn == GX_AF_SPEC) {
            /* Specular light: the position is the direction to the light
             * (scaled far away), the direction is the half-angle vector; the
             * angle attenuation runs over N.H and, with a diffuse function,
             * the distance coefficients are used normalized (as the
             * hardware does). */
            float nl = ldir[0] * nrm[0] + ldir[1] * nrm[1] + ldir[2] * nrm[2];
            float h = nl >= 0.0f ? l->dir[0] * nrm[0] + l->dir[1] * nrm[1] + l->dir[2] * nrm[2] : 0.0f;
            float a, k, k0 = l->k0, k1 = l->k1, k2 = l->k2;
            if (h < 0.0f) {
                h = 0.0f;
            }
            if (c->diff_fn != GX_DF_NONE) {
                float m = sqrtf(k0 * k0 + k1 * k1 + k2 * k2);
                if (m > 1e-12f) {
                    k0 /= m;
                    k1 /= m;
                    k2 /= m;
                }
            }
            a = l->a0 + l->a1 * h + l->a2 * h * h;
            k = k0 + k1 * h + k2 * h * h;
            if (a < 0.0f) {
                a = 0.0f;
            }
            attn = k > 1e-12f ? a / k : 0.0f;
        }
        diff = ldir[0] * nrm[0] + ldir[1] * nrm[1] + ldir[2] * nrm[2];
        if (c->diff_fn == GX_DF_NONE) {
            diff = 1.0f;
        } else if (c->diff_fn == GX_DF_CLAMP && diff < 0.0f) {
            diff = 0.0f;
        }
        illum[0] += attn * diff * l->color[0];
        illum[1] += attn * diff * l->color[1];
        illum[2] += attn * diff * l->color[2];
        illum[3] += attn * diff * l->color[3];
    }
    (void) chan;
    out[0] = mat[0] * clamp01(illum[0]);
    out[1] = mat[1] * clamp01(illum[1]);
    out[2] = mat[2] * clamp01(illum[2]);
    out[3] = mat[3] * clamp01(illum[3]);
}

static void texgen(const Vertex* v, const float vpos[3], const float vnrm[3], const float vbnm[3],
                   const float vtan[3], const GLVertex* sofar, int i, float out[2])
{
    const TexGen* g = &gx.texgen[i];
    float in[4], s, t, q;
    const float(*m)[4];
    if (g->type >= GX_TG_BUMP0 && g->type <= GX_TG_BUMP7) {
        /* emboss bump mapping: an earlier coordinate shifted along the
         * eye-space binormal and tangent by the direction to a light (the
         * barrel and other NBT items sample their height map twice, at the
         * plain and the shifted coordinate, and subtract) */
        int srcc = g->src - GX_TG_TEXCOORD0;
        const Light* l = &gx.lights[g->type - GX_TG_BUMP0];
        float ld[3];
        ld[0] = l->pos[0] - vpos[0];
        ld[1] = l->pos[1] - vpos[1];
        ld[2] = l->pos[2] - vpos[2];
        normalize3(ld);
        if (srcc < 0 || srcc >= i) {
            out[0] = out[1] = 0.0f;
            return;
        }
        out[0] = sofar->tex[srcc][0] + ld[0] * vbnm[0] + ld[1] * vbnm[1] + ld[2] * vbnm[2];
        out[1] = sofar->tex[srcc][1] + ld[0] * vtan[0] + ld[1] * vtan[1] + ld[2] * vtan[2];
        return;
    }
    if (g->src >= GX_TG_TEX0 && g->src <= GX_TG_TEX7) {
        in[0] = v->tex[g->src - GX_TG_TEX0][0];
        in[1] = v->tex[g->src - GX_TG_TEX0][1];
        in[2] = 1.0f;
        in[3] = 1.0f;
    } else if (g->src == GX_TG_POS) {
        in[0] = vpos[0];
        in[1] = vpos[1];
        in[2] = vpos[2];
        in[3] = 1.0f;
    } else if (g->src == GX_TG_NRM) {
        in[0] = vnrm[0];
        in[1] = vnrm[1];
        in[2] = vnrm[2];
        in[3] = 1.0f;
    } else {
        out[0] = out[1] = 0.0f;
        return;
    }
    if (g->normalize) {
        normalize3(in);
    }
    if (g->mtx >= MTX_ROWS - 2) {
        /* GX_IDENTITY: the input passes through, but the post-transform
         * matrix below still applies (toon shading maps the normal through
         * it: identity texture matrix, then s = a * nx + b) */
        s = in[0];
        t = in[1];
        q = g->type == GX_TG_MTX3x4 ? in[2] : 1.0f;
    } else {
        m = (const float(*)[4]) gx.mtx[g->mtx];
        s = m[0][0] * in[0] + m[0][1] * in[1] + m[0][2] * in[2] + m[0][3] * in[3];
        t = m[1][0] * in[0] + m[1][1] * in[1] + m[1][2] * in[2] + m[1][3] * in[3];
        q = 1.0f;
        if (g->type == GX_TG_MTX3x4) {
            q = m[2][0] * in[0] + m[2][1] * in[1] + m[2][2] * in[2] + m[2][3] * in[3];
        }
    }
    /* dual transform: the post-transform matrix (GX_PTTEXMTX0..19) is applied
     * to the generated (s, t, q). HSD loads every texture's own matrix
     * (animation translate/scale/rotate, reflection maps) there. */
    if (g->pt_mtx != GX_PTIDENTITY && g->pt_mtx + 3 <= MTX_ROWS) {
        const float(*pm)[4] = (const float(*)[4]) gx.mtx[g->pt_mtx];
        float ps = pm[0][0] * s + pm[0][1] * t + pm[0][2] * q + pm[0][3];
        float pt = pm[1][0] * s + pm[1][1] * t + pm[1][2] * q + pm[1][3];
        float pq = pm[2][0] * s + pm[2][1] * t + pm[2][2] * q + pm[2][3];
        s = ps;
        t = pt;
        q = pq;
    }
    if (q != 0.0f && q != 1.0f) {
        s /= q;
        t /= q;
    }
    out[0] = s;
    out[1] = t;
}

static int xform_want_nbt; /* a bump texgen is active: transform the binormal and tangent too */

static void transform_vertex(const Vertex* v, GLVertex* out)
{
    float pos[3], nrm[3], bnm[3], tan[3];
    const float(*pm)[4];
    u32 slot = v->pnmtx;
    int i, want_nbt = xform_want_nbt;

    if (slot + 2 >= MTX_ROWS) {
        slot = 0;
    }
    pm = (const float(*)[4]) gx.mtx[slot];
    mtx_mul_point(pm, v->pos, pos);
    {
        const float(*nm)[3] = (const float(*)[3]) gx.nrm[(slot / 3) & 31];
        nrm[0] = nm[0][0] * v->nrm[0] + nm[0][1] * v->nrm[1] + nm[0][2] * v->nrm[2];
        nrm[1] = nm[1][0] * v->nrm[0] + nm[1][1] * v->nrm[1] + nm[1][2] * v->nrm[2];
        nrm[2] = nm[2][0] * v->nrm[0] + nm[2][1] * v->nrm[1] + nm[2][2] * v->nrm[2];
        normalize3(nrm);
        if (want_nbt) {
            int k;
            for (k = 0; k < 3; k++) {
                bnm[k] = nm[k][0] * v->bnm[0] + nm[k][1] * v->bnm[1] + nm[k][2] * v->bnm[2];
                tan[k] = nm[k][0] * v->tan[0] + nm[k][1] * v->tan[1] + nm[k][2] * v->tan[2];
            }
            /* not normalized: their length sets the bump depth (Dolphin
             * normalizes only the normal) */
        } else {
            bnm[0] = bnm[1] = bnm[2] = 0.0f;
            tan[0] = tan[1] = tan[2] = 0.0f;
        }
    }
    memcpy(out->pos, pos, sizeof(pos));

    for (i = 0; i < 2; i++) {
        float c[4], a[4];
        if (i < gx.num_chans) {
            const ChanCtrl* ac = &gx.chan[2 + i];
            light_channel(&gx.chan[i], i, v->col[i], gx.mat_color[i], gx.amb_color[i], pos, nrm, c);
            out->col[i][0] = c[0];
            out->col[i][1] = c[1];
            out->col[i][2] = c[2];
            if (ac->enable) {
                light_channel(ac, 2 + i, v->col[i], gx.mat_color[i], gx.amb_color[i], pos, nrm, a);
                out->col[i][3] = a[3];
            } else {
                out->col[i][3] = ac->mat_src == GX_SRC_VTX ? v->col[i][3] : gx.mat_color[i][3];
            }
        } else {
            memcpy(out->col[i], v->col[i], sizeof(out->col[i]));
        }
    }
    for (i = 0; i < 8; i++) {
        if (i < gx.num_texgen) {
            texgen(v, pos, nrm, bnm, tan, out, i, out->tex[i]);
        } else {
            out->tex[i][0] = v->tex[i][0];
            out->tex[i][1] = v->tex[i][1];
        }
    }
}

/* --- Textures ------------------------------------------------------------ */

typedef struct TexEntry {
    const void* image;
    u16 width, height;
    u8 format;
    u32 tlut_name;
    const void* lut;
    u32 lut_hash; /* palettes are rewritten in place (player colours, ...) */
    GLuint tex;
    u32 last_frame;
    u8 has_mips; /* mip levels generated from level 0 */
    u8 params_set, p_wrap_s, p_wrap_t, p_mag;
    GLenum p_min;
} TexEntry;

static u32 hash_bytes(const void* p, size_t n)
{
    const u8* b = (const u8*) p;
    u32 h = 2166136261u;
    size_t i;
    for (i = 0; i < n; i++) {
        h = (h ^ b[i]) * 16777619u;
    }
    return h;
}

/* the same hash a word at a time (a shader key is 600 bytes and the
 * results screen looks one up for each of its 6800 draws) */
static u32 hash_words(const void* p, size_t n)
{
    const u8* b = (const u8*) p;
    u32 h = 2166136261u;
    size_t i, nw = n / 4;
    for (i = 0; i < nw; i++) {
        u32 w;
        memcpy(&w, b + i * 4, 4);
        h = (h ^ w) * 16777619u;
    }
    for (i = nw * 4; i < n; i++) {
        h = (h ^ b[i]) * 16777619u;
    }
    return h;
}

/* A palette's hash is part of a texture's cache identity (palettes are
 * rewritten in place); it is computed once per frame per palette, not once
 * per draw. */
static u32 lut_hash_of(const void* lut, u32 bytes)
{
    static struct {
        const void* lut;
        u32 bytes, hash, frame;
    } memo[128];
    u32 i = (((u32) (uintptr_t) lut) >> 5) & 127;
    if (memo[i].lut == lut && memo[i].bytes == bytes && memo[i].frame == frame_no + 1) {
        return memo[i].hash;
    }
    memo[i].lut = lut;
    memo[i].bytes = bytes;
    memo[i].frame = frame_no + 1;
    memo[i].hash = hash_words(lut, bytes);
    return memo[i].hash;
}

#define TEX_CACHE 1024
static TexEntry tex_cache[TEX_CACHE];
/* buckets of candidate entries by image pointer (index + 1, 0 = empty) */
#define TEX_BUCKETS 2048
#define TEX_WAYS 4
static u16 tex_buckets[TEX_BUCKETS][TEX_WAYS];

static u32 tex_bucket_of(const void* image)
{
    u32 v = (u32) (uintptr_t) image;
    return ((v >> 5) ^ (v >> 13) ^ (v >> 21)) & (TEX_BUCKETS - 1);
}

static void tex_bucket_add(u32 index)
{
    u16* b = tex_buckets[tex_bucket_of(tex_cache[index].image)];
    int w;
    for (w = TEX_WAYS - 1; w > 0; w--) {
        b[w] = b[w - 1];
    }
    b[0] = (u16) (index + 1);
}

static void tex_bucket_remove(u32 index)
{
    u16* b = tex_buckets[tex_bucket_of(tex_cache[index].image)];
    int w;
    for (w = 0; w < TEX_WAYS; w++) {
        if (b[w] == index + 1) {
            b[w] = 0;
        }
    }
}
static int debug_nocache = -1;

/* EFB copies: a destination pointer that GXCopyTex wrote maps to a GL
 * texture holding the copied pixels. */
typedef struct CopyEntry {
    const void* dest;
    GLuint tex;
    u16 width, height;
    int gl_w, gl_h; /* the texture's current storage (window pixels) */
    u8 params_set, p_wrap_s, p_wrap_t, p_min, p_mag;
} CopyEntry;
#define COPY_CACHE 32
static CopyEntry copy_cache[COPY_CACHE];
static struct {
    u16 left, top, wd, ht;
    u16 dst_wd, dst_ht;
    u8 fmt, mipmap;
} tex_copy;

static u32 rgb5a3(u32 v)
{
    u32 r, g, b, a;
    if (v & 0x8000) {
        r = (v >> 10) & 31;
        g = (v >> 5) & 31;
        b = v & 31;
        r = (r << 3) | (r >> 2);
        g = (g << 3) | (g >> 2);
        b = (b << 3) | (b >> 2);
        a = 255;
    } else {
        a = (v >> 12) & 7;
        r = (v >> 8) & 15;
        g = (v >> 4) & 15;
        b = v & 15;
        r = r * 17;
        g = g * 17;
        b = b * 17;
        a = (a << 5) | (a << 2) | (a >> 1);
    }
    return r | (g << 8) | (b << 16) | (a << 24);
}

static u32 rgb565(u32 v)
{
    u32 r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);
    return r | (g << 8) | (b << 16) | 0xFF000000u;
}

static u32 tlut_lookup(const PCTlutObj* t, u32 idx)
{
    const u8* p;
    u32 v;
    if (t == NULL || t->lut == NULL || idx >= t->n_entries) {
        return 0xFF000000u | (idx * 0x010101);
    }
    p = (const u8*) t->lut + idx * 2;
    v = be16(p);
    switch (t->fmt) {
    case GX_TL_IA8:
        return ((v & 0xFF) * 0x010101) | ((v >> 8) << 24);
    case GX_TL_RGB565:
        return rgb565(v);
    default:
        return rgb5a3(v);
    }
}

/// Decode a GX texture (level 0) into RGBA8 (R first in memory).
static u32* decode_texture(const PCTexObj* t, u32* out_w, u32* out_h)
{
    u32 w = t->width, h = t->height, x, y;
    u32 bw, bh; /* tile size */
    const u8* src = (const u8*) t->image;
    u32* out;
    const PCTlutObj* tlut = t->tlut_name < 20 ? &gx.tlut[t->tlut_name] : NULL;

    switch (t->format) {
    case GX_TF_I4:
    case GX_TF_C4:
    case GX_TF_CMPR:
        bw = 8;
        bh = 8;
        break;
    case GX_TF_I8:
    case GX_TF_IA4:
    case GX_TF_C8:
        bw = 8;
        bh = 4;
        break;
    default:
        bw = 4;
        bh = 4;
        break;
    }
    *out_w = w;
    *out_h = h;
    out = (u32*) malloc(w * h * 4);
    if (out == NULL || src == NULL) {
        free(out);
        return NULL;
    }
    for (y = 0; y < h; y += bh) {
        for (x = 0; x < w; x += bw) {
            u32 tx = x / bw, ty = y / bh, tiles_per_row = (w + bw - 1) / bw;
            u32 tile = ty * tiles_per_row + tx;
            u32 i, j;
            switch (t->format) {
            case GX_TF_I4: {
                const u8* p = src + tile * 32;
                for (j = 0; j < 8; j++)
                    for (i = 0; i < 8; i++) {
                        u32 b = p[j * 4 + i / 2];
                        u32 v = (i & 1) ? (b & 15) : (b >> 4);
                        v *= 17;
                        if (x + i < w && y + j < h)
                            out[(y + j) * w + x + i] = v | (v << 8) | (v << 16) | (v << 24);
                    }
                break;
            }
            case GX_TF_I8: {
                const u8* p = src + tile * 32;
                for (j = 0; j < 4; j++)
                    for (i = 0; i < 8; i++) {
                        u32 v = p[j * 8 + i];
                        if (x + i < w && y + j < h)
                            out[(y + j) * w + x + i] = v | (v << 8) | (v << 16) | (v << 24);
                    }
                break;
            }
            case GX_TF_IA4: {
                const u8* p = src + tile * 32;
                for (j = 0; j < 4; j++)
                    for (i = 0; i < 8; i++) {
                        u32 b = p[j * 8 + i];
                        u32 v = (b & 15) * 17, a = (b >> 4) * 17;
                        if (x + i < w && y + j < h)
                            out[(y + j) * w + x + i] = v | (v << 8) | (v << 16) | (a << 24);
                    }
                break;
            }
            case GX_TF_IA8: {
                const u8* p = src + tile * 32;
                for (j = 0; j < 4; j++)
                    for (i = 0; i < 4; i++) {
                        u32 a = p[(j * 4 + i) * 2], v = p[(j * 4 + i) * 2 + 1];
                        if (x + i < w && y + j < h)
                            out[(y + j) * w + x + i] = v | (v << 8) | (v << 16) | (a << 24);
                    }
                break;
            }
            case GX_TF_RGB565: {
                const u8* p = src + tile * 32;
                for (j = 0; j < 4; j++)
                    for (i = 0; i < 4; i++)
                        if (x + i < w && y + j < h)
                            out[(y + j) * w + x + i] = rgb565(be16(p + (j * 4 + i) * 2));
                break;
            }
            case GX_TF_RGB5A3: {
                const u8* p = src + tile * 32;
                for (j = 0; j < 4; j++)
                    for (i = 0; i < 4; i++)
                        if (x + i < w && y + j < h)
                            out[(y + j) * w + x + i] = rgb5a3(be16(p + (j * 4 + i) * 2));
                break;
            }
            case GX_TF_RGBA8: {
                const u8* p = src + tile * 64;
                for (j = 0; j < 4; j++)
                    for (i = 0; i < 4; i++) {
                        u32 a = p[(j * 4 + i) * 2], r = p[(j * 4 + i) * 2 + 1];
                        u32 g = p[32 + (j * 4 + i) * 2], b = p[32 + (j * 4 + i) * 2 + 1];
                        if (x + i < w && y + j < h)
                            out[(y + j) * w + x + i] = r | (g << 8) | (b << 16) | (a << 24);
                    }
                break;
            }
            case GX_TF_C4: {
                const u8* p = src + tile * 32;
                for (j = 0; j < 8; j++)
                    for (i = 0; i < 8; i++) {
                        u32 b = p[j * 4 + i / 2];
                        u32 v = (i & 1) ? (b & 15) : (b >> 4);
                        if (x + i < w && y + j < h)
                            out[(y + j) * w + x + i] = tlut_lookup(tlut, v);
                    }
                break;
            }
            case GX_TF_C8: {
                const u8* p = src + tile * 32;
                for (j = 0; j < 4; j++)
                    for (i = 0; i < 8; i++)
                        if (x + i < w && y + j < h)
                            out[(y + j) * w + x + i] = tlut_lookup(tlut, p[j * 8 + i]);
                break;
            }
            case GX_TF_C14X2: {
                const u8* p = src + tile * 32;
                for (j = 0; j < 4; j++)
                    for (i = 0; i < 4; i++)
                        if (x + i < w && y + j < h)
                            out[(y + j) * w + x + i] =
                                tlut_lookup(tlut, be16(p + (j * 4 + i) * 2) & 0x3FFF);
                break;
            }
            case GX_TF_CMPR: {
                /* 8x8 tile = four DXT1 4x4 blocks: (0,0) (4,0) (0,4) (4,4) */
                const u8* p = src + tile * 32;
                u32 blk;
                for (blk = 0; blk < 4; blk++) {
                    const u8* b = p + blk * 8;
                    u32 c0 = be16(b), c1 = be16(b + 2);
                    u32 pal[4];
                    u32 bx = x + (blk & 1) * 4, by = y + (blk >> 1) * 4;
                    u32 r0 = (c0 >> 11) & 31, g0 = (c0 >> 5) & 63, b0 = c0 & 31;
                    u32 r1 = (c1 >> 11) & 31, g1 = (c1 >> 5) & 63, b1 = c1 & 31;
                    r0 = (r0 << 3) | (r0 >> 2); g0 = (g0 << 2) | (g0 >> 4); b0 = (b0 << 3) | (b0 >> 2);
                    r1 = (r1 << 3) | (r1 >> 2); g1 = (g1 << 2) | (g1 >> 4); b1 = (b1 << 3) | (b1 >> 2);
                    pal[0] = r0 | (g0 << 8) | (b0 << 16) | 0xFF000000u;
                    pal[1] = r1 | (g1 << 8) | (b1 << 16) | 0xFF000000u;
                    if (c0 > c1) {
                        pal[2] = ((2 * r0 + r1) / 3) | (((2 * g0 + g1) / 3) << 8) |
                                 (((2 * b0 + b1) / 3) << 16) | 0xFF000000u;
                        pal[3] = ((r0 + 2 * r1) / 3) | (((g0 + 2 * g1) / 3) << 8) |
                                 (((b0 + 2 * b1) / 3) << 16) | 0xFF000000u;
                    } else {
                        pal[2] = ((r0 + r1) / 2) | (((g0 + g1) / 2) << 8) | (((b0 + b1) / 2) << 16) |
                                 0xFF000000u;
                        pal[3] = 0;
                    }
                    for (j = 0; j < 4; j++) {
                        u32 row = b[4 + j];
                        for (i = 0; i < 4; i++) {
                            u32 idx = (row >> (6 - 2 * i)) & 3;
                            if (bx + i < w && by + j < h)
                                out[(by + j) * w + bx + i] = pal[idx];
                        }
                    }
                }
                break;
            }
            default:
                for (j = 0; j < bh; j++)
                    for (i = 0; i < bw; i++)
                        if (x + i < w && y + j < h)
                            out[(y + j) * w + x + i] = 0xFFFF00FFu;
                break;
            }
        }
    }
    return out;
}

/* MELEE_GX_DUMP_TEX=DIR writes every decoded texture as RGB and alpha PGM/PPM
 * files named by image address, size and format. */
static void dump_texture(const PCTexObj* t, const u32* pixels, u32 w, u32 h)
{
    static const char* dir;
    static int checked;
    char path[512];
    FILE* f;
    u32 i;
    if (!checked) {
        checked = 1;
        dir = getenv("MELEE_GX_DUMP_TEX");
    }
    if (dir == NULL) {
        return;
    }
    if (t->image == NULL || pixels == NULL) {
        return;
    }
    snprintf(path, sizeof(path), "%s/tex_%p_%ux%u_f%u.ppm", dir, t->image, w, h, t->format);
    f = fopen(path, "wb");
    if (f != NULL) {
        fprintf(f, "P6 %u %u 255 ", w, h);
        for (i = 0; i < w * h; i++) {
            fputc(pixels[i] & 0xFF, f);
            fputc((pixels[i] >> 8) & 0xFF, f);
            fputc((pixels[i] >> 16) & 0xFF, f);
        }
        fclose(f);
    }
    snprintf(path, sizeof(path), "%s/tex_%p_%ux%u_f%u_alpha.pgm", dir, t->image, w, h, t->format);
    f = fopen(path, "wb");
    if (f != NULL) {
        fprintf(f, "P5 %u %u 255 ", w, h);
        for (i = 0; i < w * h; i++) {
            fputc(pixels[i] >> 24, f);
        }
        fclose(f);
    }
}

static GLenum gl_wrap(u8 w)
{
    return w == GX_CLAMP ? GL_CLAMP_TO_EDGE : w == GX_MIRROR ? GL_MIRRORED_REPEAT : GL_REPEAT;
}

static GLuint bind_texture(const PCTexObj* t, int unit)
{
    u32 i, free_slot = TEX_CACHE;
    if (debug_nocache < 0) {
        debug_nocache = getenv("MELEE_GX_NOCACHE") != NULL;
    }
    const PCTlutObj* tlut = t->tlut_name < 20 ? &gx.tlut[t->tlut_name] : NULL;
    const void* lut = tlut ? tlut->lut : NULL;
    u32 lut_hash = lut != NULL ? lut_hash_of(lut, tlut->n_entries * 2) : 0;
    TexEntry* e = NULL;

    for (i = 0; i < COPY_CACHE; i++) {
        CopyEntry* c = &copy_cache[i];
        if (c->dest == t->image && c->tex != 0) {
            bind_unit(unit, c->tex);
            if (!c->params_set || c->p_wrap_s != t->wrap_s || c->p_wrap_t != t->wrap_t ||
                c->p_min != t->min_filt || c->p_mag != t->mag_filt)
            {
                flush_batch();
                use_unit(unit);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gl_wrap(t->wrap_s));
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gl_wrap(t->wrap_t));
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, t->min_filt == GX_NEAR ? GL_NEAREST : GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, t->mag_filt == GX_NEAR ? GL_NEAREST : GL_LINEAR);
                c->params_set = 1;
                c->p_wrap_s = t->wrap_s;
                c->p_wrap_t = t->wrap_t;
                c->p_min = t->min_filt;
                c->p_mag = t->mag_filt;
            }
            return 0;
        }
    }
    {
        u16* b = tex_buckets[tex_bucket_of(t->image)];
        int w;
        for (w = 0; w < TEX_WAYS && e == NULL; w++) {
            if (b[w] == 0) {
                continue;
            }
            i = (u32) b[w] - 1;
            if (tex_cache[i].tex != 0 && tex_cache[i].image == t->image && tex_cache[i].width == t->width &&
                tex_cache[i].height == t->height && tex_cache[i].format == t->format &&
                tex_cache[i].tlut_name == t->tlut_name && tex_cache[i].lut == lut &&
                tex_cache[i].lut_hash == lut_hash)
            {
                if (debug_nocache) {
                    /* MELEE_GX_NOCACHE=1: decode every texture on every use */
                    forget_texture(tex_cache[i].tex);
                    glDeleteTextures(1, &tex_cache[i].tex);
                    tex_bucket_remove(i);
                    memset(&tex_cache[i], 0, sizeof(TexEntry));
                    continue;
                }
                e = &tex_cache[i];
            }
        }
    }
    if (e == NULL) {
        for (i = 0; i < TEX_CACHE; i++) {
            if (tex_cache[i].tex == 0) {
                free_slot = i;
                break;
            }
        }
    }
    if (e == NULL) {
        u32 w, h;
        u32* pixels;
        if (free_slot == TEX_CACHE) {
            /* evict the least recently used */
            u32 oldest = 0;
            for (i = 1; i < TEX_CACHE; i++) {
                if (tex_cache[i].last_frame < tex_cache[oldest].last_frame) {
                    oldest = i;
                }
            }
            forget_texture(tex_cache[oldest].tex);
            glDeleteTextures(1, &tex_cache[oldest].tex);
            tex_bucket_remove(oldest);
            memset(&tex_cache[oldest], 0, sizeof(TexEntry));
            free_slot = oldest;
        }
        e = &tex_cache[free_slot];
        pixels = decode_texture(t, &w, &h);
        dump_texture(t, pixels, w, h);
        e->image = t->image;
        tex_bucket_add((u32) (e - tex_cache));
        e->width = t->width;
        e->height = t->height;
        e->format = t->format;
        e->tlut_name = t->tlut_name;
        e->lut = lut;
        e->lut_hash = lut_hash;
        glGenTextures(1, &e->tex);
        flush_batch();
        use_unit(unit);
        glBindTexture(GL_TEXTURE_2D, e->tex);
        unit_tex[unit] = e->tex;
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei) w, (GLsizei) h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, pixels);
        free(pixels);
    }
    e->last_frame = frame_no;
    bind_unit(unit, e->tex);
    /* the console's mip levels are on disc too, but generating them from
     * level 0 is close enough and needs no extra decoding */
    if (t->mipmap && t->min_filt >= GX_NEAR_MIP_NEAR && !e->has_mips && pc_glGenerateMipmap != NULL) {
        flush_batch();
        use_unit(unit);
        pc_glGenerateMipmap(GL_TEXTURE_2D);
        e->has_mips = 1;
        e->params_set = 0;
    }
    {
        GLenum min_filter;
        switch (e->has_mips ? t->min_filt : (t->min_filt == GX_NEAR ? GX_NEAR : GX_LINEAR)) {
        case GX_NEAR: min_filter = GL_NEAREST; break;
        case GX_NEAR_MIP_NEAR: min_filter = GL_NEAREST_MIPMAP_NEAREST; break;
        case GX_LIN_MIP_NEAR: min_filter = GL_LINEAR_MIPMAP_NEAREST; break;
        case GX_NEAR_MIP_LIN: min_filter = GL_NEAREST_MIPMAP_LINEAR; break;
        case GX_LIN_MIP_LIN: min_filter = GL_LINEAR_MIPMAP_LINEAR; break;
        default: min_filter = GL_LINEAR; break;
        }
        /* sampler parameters belong to the texture object: set once, then
         * only when the game asks for different ones */
        if (!e->params_set || e->p_min != min_filter || e->p_wrap_s != t->wrap_s || e->p_wrap_t != t->wrap_t ||
            e->p_mag != t->mag_filt)
        {
            flush_batch();
            use_unit(unit);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint) min_filter);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gl_wrap(t->wrap_s));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gl_wrap(t->wrap_t));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, t->mag_filt == GX_NEAR ? GL_NEAREST : GL_LINEAR);
            if (aniso_max > 1.0f && pc_config.aniso > 1) {
                float a = (float) pc_config.aniso;
                glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, a > aniso_max ? aniso_max : a);
            }
            e->params_set = 1;
            e->p_min = min_filter;
            e->p_wrap_s = t->wrap_s;
            e->p_wrap_t = t->wrap_t;
            e->p_mag = t->mag_filt;
        }
    }
    return 0;
}

/* --- TEV shader generation ---------------------------------------------- */

/* what the generated vertex shader depends on (GPU path only) */
typedef struct VSKey {
    u8 on, num_texgen, num_chans, want_nbt;
    struct {
        u8 type, src, normalize, pad;
        u32 mtx, pt;
    } texgen[8];
    struct {
        u8 enable, amb_src, mat_src, diff_fn, attn_fn, pad[3];
        u32 light_mask;
    } chan[4];
} VSKey;

typedef struct ShaderKey {
    TevStage tev[16];
    u8 num_tev;
    u8 alpha_comp0, alpha_op, alpha_comp1;
    u8 swap_table[4][4];
    u8 texmap_valid[8];
    u8 fog_type;
    VSKey vs;
} ShaderKey;

typedef struct Shader {
    ShaderKey key;
    GLuint prog;
    GLint u_proj, u_tex[8], u_kcolor, u_tevreg, u_aref, u_fog, u_fogcolor;
    GLint u_indmtx, u_texsize; /* -1 unless a stage uses indirect texturing */
    float last_indmtx[18], last_texsize[16];
    GLint a_pos, a_col0, a_col1, a_tex[8];
    GLint a_nrm, a_bnm, a_tan, a_blk, a_pnm; /* GPU path attributes */
    int used;
    /* the uniform values last uploaded, so unchanged ones are skipped */
    int uniforms_valid;
    float last_proj[16], last_kcolor[16], last_tevreg[16], last_aref[4], last_fog[4], last_fogcolor[4];
} Shader;

#define MAX_SHADERS 256
static Shader shaders[MAX_SHADERS];
static int num_shaders;
/* the key's hash picks a bucket of candidates (index + 1, 0 = empty); a
 * frame has a thousand draws and comparing every key against every
 * shader was the single largest cost of a four-player match */
#define SHADER_BUCKETS 1024
#define SHADER_WAYS 4
static u16 shader_buckets[SHADER_BUCKETS][SHADER_WAYS];
static Shader* last_shader; /* consecutive draws mostly share a key */
static GLuint cur_prog;     /* the program in use */
static int key_dirty = 1;    /* some key state changed since last_shader was chosen */

static char shader_src[32768];
static int shader_len;

static void emit(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    shader_len += vsnprintf(shader_src + shader_len, sizeof(shader_src) - shader_len, fmt, ap);
    va_end(ap);
}

static const char* swizzle_for(const u8 tbl[4])
{
    static const char ch[4] = { 'r', 'g', 'b', 'a' };
    static char s[8][5];
    static int n;
    char* out = s[n++ & 7];
    out[0] = ch[tbl[0] & 3];
    out[1] = ch[tbl[1] & 3];
    out[2] = ch[tbl[2] & 3];
    out[3] = ch[tbl[3] & 3];
    out[4] = '\0';
    return out;
}

static void emit_konst_color(u8 sel)
{
    static const char* frac[8] = { "1.0", "0.875", "0.75", "0.625", "0.5", "0.375", "0.25", "0.125" };
    if (sel < 8) {
        emit("vec3(%s)", frac[sel]);
    } else if (sel >= 0x0C && sel <= 0x0F) {
        emit("u_kcolor[%d].rgb", sel - 0x0C);
    } else if (sel >= 0x10) {
        static const char ch[4] = { 'r', 'g', 'b', 'a' };
        emit("vec3(u_kcolor[%d].%c)", (sel - 0x10) & 3, ch[(sel - 0x10) >> 2]);
    } else {
        emit("vec3(0.0)");
    }
}

static void emit_konst_alpha(u8 sel)
{
    static const char* frac[8] = { "1.0", "0.875", "0.75", "0.625", "0.5", "0.375", "0.25", "0.125" };
    if (sel < 8) {
        emit("%s", frac[sel]);
    } else if (sel >= 0x10) {
        static const char ch[4] = { 'r', 'g', 'b', 'a' };
        emit("u_kcolor[%d].%c", (sel - 0x10) & 3, ch[(sel - 0x10) >> 2]);
    } else {
        emit("0.0");
    }
}

static void emit_color_arg(u8 arg, const TevStage* s)
{
    switch (arg) {
    case GX_CC_CPREV: emit("prev.rgb"); break;
    case GX_CC_APREV: emit("prev.aaa"); break;
    case GX_CC_C0: emit("reg0.rgb"); break;
    case GX_CC_A0: emit("reg0.aaa"); break;
    case GX_CC_C1: emit("reg1.rgb"); break;
    case GX_CC_A1: emit("reg1.aaa"); break;
    case GX_CC_C2: emit("reg2.rgb"); break;
    case GX_CC_A2: emit("reg2.aaa"); break;
    case GX_CC_TEXC: emit("tex.rgb"); break;
    case GX_CC_TEXA: emit("tex.aaa"); break;
    case GX_CC_RASC: emit("ras.rgb"); break;
    case GX_CC_RASA: emit("ras.aaa"); break;
    case GX_CC_ONE: emit("vec3(1.0)"); break;
    case GX_CC_HALF: emit("vec3(0.5)"); break;
    case GX_CC_KONST: emit_konst_color(s->kcsel); break;
    case GX_CC_TEXRRR: emit("tex.rrr"); break;
    case GX_CC_TEXGGG: emit("tex.ggg"); break;
    case GX_CC_TEXBBB: emit("tex.bbb"); break;
    default: emit("vec3(0.0)"); break;
    }
}

static void emit_alpha_arg(u8 arg, const TevStage* s)
{
    switch (arg) {
    case GX_CA_APREV: emit("prev.a"); break;
    case GX_CA_A0: emit("reg0.a"); break;
    case GX_CA_A1: emit("reg1.a"); break;
    case GX_CA_A2: emit("reg2.a"); break;
    case GX_CA_TEXA: emit("tex.a"); break;
    case GX_CA_RASA: emit("ras.a"); break;
    case GX_CA_KONST: emit_konst_alpha(s->kasel); break;
    default: emit("0.0"); break;
    }
}

static const char* reg_name(u8 reg)
{
    switch (reg) {
    case GX_TEVREG0: return "reg0";
    case GX_TEVREG1: return "reg1";
    case GX_TEVREG2: return "reg2";
    default: return "prev";
    }
}

static void emit_stage(const ShaderKey* k, int i)
{
    const TevStage* s = &k->tev[i];
    const char* scale[4] = { "1.0", "2.0", "4.0", "0.5" };
    const char* bias[3] = { "0.0", "0.5", "-0.5" };

    emit("    {\n");
    /* texture sample */
    if (!debug_litonly && s->map < 8 && k->texmap_valid[s->map] && s->coord < 8 && s->ind_on &&
        s->ind_map < 8 && k->texmap_valid[s->ind_map] && s->ind_coord < 8)
    {
        /* indirect texturing: the indirect texture's (a, b, g) are the
         * (s, t, u) inputs, 8-bit values with an optional bias of -128,
         * multiplied by the indirect matrix into an offset in texels of
         * this stage's texture */
        int b = s->ind_bias;
        float full = s->ind_fmt == GX_ITF_8 ? 255.0f : s->ind_fmt == GX_ITF_5 ? 31.0f : s->ind_fmt == GX_ITF_4 ? 15.0f : 7.0f;
        float bias = s->ind_fmt == GX_ITF_8 ? 128.0f : 1.0f;
        emit("        vec3 indc = texture2D(u_tex%d, v_tex%d).abg * %.1f - vec3(%.1f, %.1f, %.1f);\n", s->ind_map,
             s->ind_coord, full, (b & 1) ? bias : 0.0f, (b & 2) ? bias : 0.0f, (b & 4) ? bias : 0.0f);
        if (s->ind_mtx >= 1 && s->ind_mtx <= 3) {
            emit("        vec2 induv = v_tex%d + vec2(dot(u_indmtx[%d], indc), dot(u_indmtx[%d], indc)) / u_texsize[%d];\n",
                 s->coord, (s->ind_mtx - 1) * 2, (s->ind_mtx - 1) * 2 + 1, s->map);
        } else {
            emit("        vec2 induv = v_tex%d;\n", s->coord);
        }
        emit("        tex = texture2D(u_tex%d, induv).%s;\n", s->map, swizzle_for(k->swap_table[s->tex_swap & 3]));
    } else if (!debug_litonly && s->map < 8 && k->texmap_valid[s->map] && s->coord < 8) {
        emit("        tex = texture2D(u_tex%d, v_tex%d).%s;\n", s->map, s->coord,
             swizzle_for(k->swap_table[s->tex_swap & 3]));
    } else {
        emit("        tex = vec4(1.0);\n");
    }
    /* rasterized color */
    switch (s->chan) {
    case GX_COLOR0A0: emit("        ras = v_col0.%s;\n", swizzle_for(k->swap_table[s->ras_swap & 3])); break;
    case GX_COLOR1A1: emit("        ras = v_col1.%s;\n", swizzle_for(k->swap_table[s->ras_swap & 3])); break;
    case GX_COLOR0: emit("        ras = vec4(v_col0.rgb, 1.0);\n"); break;
    case GX_COLOR1: emit("        ras = vec4(v_col1.rgb, 1.0);\n"); break;
    case GX_ALPHA0: emit("        ras = vec4(v_col0.a);\n"); break;
    case GX_ALPHA1: emit("        ras = vec4(v_col1.a);\n"); break;
    default: emit("        ras = vec4(0.0);\n"); break;
    }
    /* color combine */
    emit("        vec3 ca = "); emit_color_arg(s->ca[0], s); emit(";\n");
    emit("        vec3 cb = "); emit_color_arg(s->ca[1], s); emit(";\n");
    emit("        vec3 cc = "); emit_color_arg(s->ca[2], s); emit(";\n");
    emit("        vec3 cd = "); emit_color_arg(s->ca[3], s); emit(";\n");
    emit("        float aa = "); emit_alpha_arg(s->aa[0], s); emit(";\n");
    emit("        float ab = "); emit_alpha_arg(s->aa[1], s); emit(";\n");
    emit("        float ac = "); emit_alpha_arg(s->aa[2], s); emit(";\n");
    emit("        float ad = "); emit_alpha_arg(s->aa[3], s); emit(";\n");
    if (s->cop == GX_TEV_ADD || s->cop == GX_TEV_SUB) {
        emit("        vec3 cres = (cd %s mix(ca, cb, cc) + %s) * %s;\n",
             s->cop == GX_TEV_SUB ? "-" : "+", bias[s->cbias % 3], scale[s->cscale & 3]);
    } else {
        /* compare ops */
        switch (s->cop) {
        case GX_TEV_COMP_R8_GT: emit("        vec3 cres = cd + ((ca.r > cb.r) ? cc : vec3(0.0));\n"); break;
        case GX_TEV_COMP_R8_EQ: emit("        vec3 cres = cd + ((abs(ca.r - cb.r) < 0.002) ? cc : vec3(0.0));\n"); break;
        case GX_TEV_COMP_GR16_GT: emit("        vec3 cres = cd + ((ca.g * 256.0 + ca.r > cb.g * 256.0 + cb.r) ? cc : vec3(0.0));\n"); break;
        case GX_TEV_COMP_GR16_EQ: emit("        vec3 cres = cd + ((abs(ca.g * 256.0 + ca.r - cb.g * 256.0 - cb.r) < 0.002) ? cc : vec3(0.0));\n"); break;
        case GX_TEV_COMP_BGR24_GT: emit("        vec3 cres = cd + ((dot(ca, vec3(1.0, 256.0, 65536.0)) > dot(cb, vec3(1.0, 256.0, 65536.0))) ? cc : vec3(0.0));\n"); break;
        case GX_TEV_COMP_BGR24_EQ: emit("        vec3 cres = cd + ((abs(dot(ca, vec3(1.0, 256.0, 65536.0)) - dot(cb, vec3(1.0, 256.0, 65536.0))) < 0.002) ? cc : vec3(0.0));\n"); break;
        case GX_TEV_COMP_RGB8_GT: emit("        vec3 cres = cd + vec3(greaterThan(ca, cb)) * cc;\n"); break;
        default: emit("        vec3 cres = cd + vec3(lessThan(abs(ca - cb), vec3(0.002))) * cc;\n"); break;
        }
    }
    if (s->aop == GX_TEV_ADD || s->aop == GX_TEV_SUB) {
        emit("        float ares = (ad %s mix(aa, ab, ac) + %s) * %s;\n",
             s->aop == GX_TEV_SUB ? "-" : "+", bias[s->abias % 3], scale[s->ascale & 3]);
    } else if (s->aop == GX_TEV_COMP_A8_GT || s->aop == GX_TEV_COMP_R8_GT ||
               s->aop == GX_TEV_COMP_GR16_GT || s->aop == GX_TEV_COMP_BGR24_GT) {
        emit("        float ares = ad + ((aa > ab) ? ac : 0.0);\n");
    } else {
        emit("        float ares = ad + ((abs(aa - ab) < 0.002) ? ac : 0.0);\n");
    }
    if (s->cclamp) {
        emit("        cres = clamp(cres, 0.0, 1.0);\n");
    } else {
        emit("        cres = clamp(cres, -4.0, 4.0);\n");
    }
    if (s->aclamp) {
        emit("        ares = clamp(ares, 0.0, 1.0);\n");
    } else {
        emit("        ares = clamp(ares, -4.0, 4.0);\n");
    }
    emit("        %s.rgb = cres;\n", reg_name(s->creg));
    emit("        %s.a = ares;\n", reg_name(s->areg));
    emit("    }\n");
}

static const char* cmp_expr(u8 comp, const char* ref)
{
    static char buf[4][64];
    static int n;
    char* b = buf[n++ & 3];
    switch (comp) {
    case GX_NEVER: strcpy(b, "false"); break;
    case GX_LESS: snprintf(b, 64, "(prev.a < %s)", ref); break;
    case GX_EQUAL: snprintf(b, 64, "(abs(prev.a - %s) < 0.002)", ref); break;
    case GX_LEQUAL: snprintf(b, 64, "(prev.a <= %s + 0.001)", ref); break;
    case GX_GREATER: snprintf(b, 64, "(prev.a > %s)", ref); break;
    case GX_NEQUAL: snprintf(b, 64, "(abs(prev.a - %s) >= 0.002)", ref); break;
    case GX_GEQUAL: snprintf(b, 64, "(prev.a >= %s - 0.001)", ref); break;
    default: strcpy(b, "true"); break;
    }
    return b;
}

static GLuint compile(GLenum type, const char* src)
{
    GLuint sh = pc_glCreateShader(type);
    GLint ok = 0;
    pc_glShaderSource(sh, 1, &src, NULL);
    pc_glCompileShader(sh);
    pc_glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        pc_glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        fprintf(stderr, "[pc] GL: shader compile error:\n%s\n--- source ---\n%s\n", log, src);
    }
    return sh;
}

/* The vertex shader of the GPU path, mirroring transform_vertex(),
 * light_channel() and texgen() step for step. P(i) reads texel i of the
 * parameter texture; M(r) a row of GX matrix memory and N(n, r) a row of
 * normal matrix n, both split between the two matrix blocks. */
static void emit_vs(const VSKey* k)
{
    int i, j;
    shader_len = 0;
    emit("#version 120\n"
         "uniform mat4 u_proj;\n"
         "uniform sampler2D u_params;\n"
         "attribute vec3 a_pos, a_nrm, a_bnm, a_tan;\n"
         "attribute vec4 a_col0, a_col1, a_blk;\n"
         "attribute float a_pnm;\n"
         "attribute vec2 a_tex0, a_tex1, a_tex2, a_tex3, a_tex4, a_tex5, a_tex6, a_tex7;\n"
         "varying vec4 v_col0, v_col1;\n"
         "varying vec2 v_tex0, v_tex1, v_tex2, v_tex3, v_tex4, v_tex5, v_tex6, v_tex7;\n"
         "varying float v_depth;\n"
         "vec4 P(float i) { float y = floor((i + 0.5) * (1.0 / %d.0)); float x = i - y * %d.0;\n"
         "    return texture2D(u_params, vec2((x + 0.5) * (1.0 / %d.0), (y + 0.5) * (1.0 / %d.0))); }\n"
         "vec4 M(float r) { return r < 30.0 ? P(a_blk.x + r) : P(a_blk.y + (r - 30.0)); }\n"
         "vec4 N(float n, float r) { return n < 10.0 ? P(a_blk.x + 30.0 + n * 3.0 + r) : P(a_blk.y + 98.0 + (n - 10.0) * 3.0 + r); }\n"
         "vec3 safe_normalize(vec3 v) { float l = length(v); return l > 1e-12 ? v / l : v; }\n"
         "void main() {\n"
         "    vec4 ip = vec4(a_pos, 1.0);\n"
         "    vec3 pos = vec3(dot(M(a_pnm), ip), dot(M(a_pnm + 1.0), ip), dot(M(a_pnm + 2.0), ip));\n"
         "    float nn = floor(a_pnm / 3.0 + 0.5);\n"
         "    vec3 n0 = N(nn, 0.0).xyz, n1 = N(nn, 1.0).xyz, n2 = N(nn, 2.0).xyz;\n"
         "    vec3 nrm = safe_normalize(vec3(dot(n0, a_nrm), dot(n1, a_nrm), dot(n2, a_nrm)));\n",
         PARAM_W, PARAM_W, PARAM_W, PARAM_H);
    if (k->want_nbt) {
        emit("    vec3 bnm = vec3(dot(n0, a_bnm), dot(n1, a_bnm), dot(n2, a_bnm));\n"
             "    vec3 tan = vec3(dot(n0, a_tan), dot(n1, a_tan), dot(n2, a_tan));\n");
    }
    /* colour channels: rgb from channel i, alpha from channel 2 + i */
    for (i = 0; i < 2; i++) {
        if (i >= k->num_chans) {
            emit("    vec4 c%d = a_col%d;\n", i, i);
            continue;
        }
        for (j = i; j < 4; j += 2) {
            const VSKey* kk = k;
            u8 en = kk->chan[j].enable, mat_src = kk->chan[j].mat_src, amb_src = kk->chan[j].amb_src;
            u8 diff_fn = kk->chan[j].diff_fn, attn_fn = kk->chan[j].attn_fn;
            u32 mask = kk->chan[j].light_mask;
            int l;
            emit("    vec4 ch%d;\n    {\n", j);
            if (mat_src == GX_SRC_VTX) {
                emit("        vec4 mat = a_col%d;\n", i);
            } else {
                emit("        vec4 mat = P(a_blk.w + %d.0);\n", i);
            }
            if (!en) {
                emit("        ch%d = mat;\n", j);
            } else {
                if (amb_src == GX_SRC_VTX) {
                    emit("        vec4 illum = a_col%d;\n", i);
                } else {
                    emit("        vec4 illum = P(a_blk.w + %d.0);\n", 2 + i);
                }
                for (l = 0; l < 8; l++) {
                    if (!(mask & (1u << l))) {
                        continue;
                    }
                    emit("        {\n"
                         "            vec4 L0 = P(a_blk.z + %d.0), L1 = P(a_blk.z + %d.0), L2 = P(a_blk.z + %d.0), L3 = P(a_blk.z + %d.0);\n"
                         "            vec3 ldir = L0.xyz - pos;\n"
                         "            float dist = length(ldir);\n"
                         "            if (dist > 1e-12) ldir /= dist;\n"
                         "            float attn = 1.0;\n",
                         l * 4, l * 4 + 1, l * 4 + 2, l * 4 + 3);
                    if (attn_fn == GX_AF_SPOT) {
                        emit("            {\n"
                             "                float cosa = -dot(ldir, L1.xyz);\n"
                             "                float a = max(L0.w + L1.w * cosa + L3.w * cosa * cosa, 0.0);\n"
                             "                float kk = L3.x + L3.y * dist + L3.z * dist * dist;\n"
                             "                attn = kk > 1e-12 ? a / kk : 0.0;\n"
                             "            }\n");
                    } else if (attn_fn == GX_AF_SPEC) {
                        emit("            {\n"
                             "                float nl = dot(ldir, nrm);\n"
                             "                float h = nl >= 0.0 ? max(dot(L1.xyz, nrm), 0.0) : 0.0;\n"
                             "                vec3 kv = L3.xyz;\n");
                        if (diff_fn != GX_DF_NONE) {
                            emit("                { float m = length(kv); if (m > 1e-12) kv /= m; }\n");
                        }
                        emit("                float a = max(L0.w + L1.w * h + L3.w * h * h, 0.0);\n"
                             "                float kk = kv.x + kv.y * h + kv.z * h * h;\n"
                             "                attn = kk > 1e-12 ? a / kk : 0.0;\n"
                             "            }\n");
                    }
                    if (diff_fn == GX_DF_NONE) {
                        emit("            float diff = 1.0;\n");
                    } else if (diff_fn == GX_DF_CLAMP) {
                        emit("            float diff = max(dot(ldir, nrm), 0.0);\n");
                    } else {
                        emit("            float diff = dot(ldir, nrm);\n");
                    }
                    emit("            illum += attn * diff * L2;\n"
                         "        }\n");
                }
                emit("        ch%d = mat * clamp(illum, 0.0, 1.0);\n", j);
            }
            emit("    }\n");
        }
        emit("    vec4 c%d = vec4(ch%d.rgb, ch%d.a);\n", i, i, i + 2);
    }
    emit("    v_col0 = c0;\n    v_col1 = c1;\n");
    /* texture coordinates */
    for (i = 0; i < 8; i++) {
        u8 type, src;
        if (i >= k->num_texgen) {
            emit("    v_tex%d = a_tex%d;\n", i, i);
            continue;
        }
        type = k->texgen[i].type;
        src = k->texgen[i].src;
        emit("    {\n");
        if (type >= GX_TG_BUMP0 && type <= GX_TG_BUMP7) {
            int srcc = (int) src - GX_TG_TEXCOORD0;
            int l = type - GX_TG_BUMP0;
            if (srcc < 0 || srcc >= i || !k->want_nbt) {
                emit("        v_tex%d = vec2(0.0);\n", i);
            } else {
                emit("        vec3 ld = safe_normalize(P(a_blk.z + %d.0).xyz - pos);\n"
                     "        v_tex%d = v_tex%d + vec2(dot(ld, bnm), dot(ld, tan));\n",
                     l * 4, i, srcc);
            }
            emit("    }\n");
            continue;
        }
        if (src >= GX_TG_TEX0 && src <= GX_TG_TEX7) {
            emit("        vec4 tin = vec4(a_tex%d, 1.0, 1.0);\n", src - GX_TG_TEX0);
        } else if (src == GX_TG_POS) {
            emit("        vec4 tin = vec4(pos, 1.0);\n");
        } else if (src == GX_TG_NRM) {
            emit("        vec4 tin = vec4(nrm, 1.0);\n");
        } else {
            emit("        v_tex%d = vec2(0.0);\n    }\n", i);
            continue;
        }
        if (k->texgen[i].normalize) {
            emit("        tin.xyz = safe_normalize(tin.xyz);\n");
        }
        if (k->texgen[i].mtx >= MTX_ROWS - 2) {
            emit("        float s = tin.x, t = tin.y, q = %s;\n", type == GX_TG_MTX3x4 ? "tin.z" : "1.0");
        } else {
            emit("        float s = dot(M(%u.0), tin), t = dot(M(%u.0), tin);\n", k->texgen[i].mtx,
                 k->texgen[i].mtx + 1);
            if (type == GX_TG_MTX3x4) {
                emit("        float q = dot(M(%u.0), tin);\n", k->texgen[i].mtx + 2);
            } else {
                emit("        float q = 1.0;\n");
            }
        }
        if (k->texgen[i].pt != GX_PTIDENTITY && k->texgen[i].pt + 3 <= MTX_ROWS) {
            emit("        {\n"
                 "            vec4 stq = vec4(s, t, q, 1.0);\n"
                 "            float ps = dot(M(%u.0), stq), pt = dot(M(%u.0), stq), pq = dot(M(%u.0), stq);\n"
                 "            s = ps; t = pt; q = pq;\n"
                 "        }\n",
                 k->texgen[i].pt, k->texgen[i].pt + 1, k->texgen[i].pt + 2);
        }
        emit("        if (q != 0.0 && q != 1.0) { s /= q; t /= q; }\n"
             "        v_tex%d = vec2(s, t);\n"
             "    }\n", i);
    }
    emit("    vec4 p = u_proj * vec4(pos, 1.0);\n"
         "    p.z = p.z * 2.0 + p.w;\n"
         "    gl_Position = p;\n"
         "    v_depth = -pos.z;\n"
         "}\n");
}

static Shader* get_shader(void)
{
    ShaderKey key;
    int i, s;
    Shader* sh;
    GLuint vs, fs;
    static const char* vsrc =
        "#version 120\n"
        "uniform mat4 u_proj;\n"
        "attribute vec3 a_pos;\n"
        "attribute vec4 a_col0;\n"
        "attribute vec4 a_col1;\n"
        "attribute vec2 a_tex0, a_tex1, a_tex2, a_tex3, a_tex4, a_tex5, a_tex6, a_tex7;\n"
        "varying vec4 v_col0, v_col1;\n"
        "varying vec2 v_tex0, v_tex1, v_tex2, v_tex3, v_tex4, v_tex5, v_tex6, v_tex7;\n"
        "varying float v_depth;\n"
        "void main() {\n"
        "    vec4 p = u_proj * vec4(a_pos, 1.0);\n"
        "    p.z = p.z * 2.0 + p.w;\n" /* GX clip depth [-w,0] -> GL [-w,w] */
        "    gl_Position = p;\n"
        "    v_depth = -a_pos.z;\n" /* eye-space distance, for fog */
        "    v_col0 = a_col0; v_col1 = a_col1;\n"
        "    v_tex0 = a_tex0; v_tex1 = a_tex1; v_tex2 = a_tex2; v_tex3 = a_tex3;\n"
        "    v_tex4 = a_tex4; v_tex5 = a_tex5; v_tex6 = a_tex6; v_tex7 = a_tex7;\n"
        "}\n";

    if (!key_dirty && last_shader != NULL) {
        return last_shader; /* nothing that goes into the key changed */
    }
    key_dirty = 0;
    memset(&key, 0, sizeof(key));
    key.num_tev = gx.num_tev;
    for (i = 0; i < gx.num_tev; i++) {
        key.tev[i] = gx.tev[i];
        if (key.tev[i].ind_on) {
            key.tev[i].ind_coord = gx.ind_order[key.tev[i].ind_stage].coord;
            key.tev[i].ind_map = gx.ind_order[key.tev[i].ind_stage].map;
        }
    }
    key.alpha_comp0 = gx.alpha_comp0;
    key.alpha_op = gx.alpha_op;
    key.alpha_comp1 = gx.alpha_comp1;
    memcpy(key.swap_table, gx.swap_table, sizeof(key.swap_table));
    for (i = 0; i < 8; i++) {
        key.texmap_valid[i] = gx.texmap[i].image != NULL;
    }
    key.fog_type = gx.fog_type;
    if (gpu_path > 0) {
        key.vs.on = 1;
        key.vs.num_texgen = gx.num_texgen;
        key.vs.num_chans = gx.num_chans;
        for (i = 0; i < gx.num_texgen; i++) {
            key.vs.texgen[i].type = gx.texgen[i].type;
            key.vs.texgen[i].src = gx.texgen[i].src;
            key.vs.texgen[i].normalize = gx.texgen[i].normalize;
            key.vs.texgen[i].mtx = gx.texgen[i].mtx;
            key.vs.texgen[i].pt = gx.texgen[i].pt_mtx;
            if (gx.texgen[i].type >= GX_TG_BUMP0 && gx.texgen[i].type <= GX_TG_BUMP7) {
                key.vs.want_nbt = 1;
            }
        }
        for (i = 0; i < 4; i++) {
            if ((i & 1) < gx.num_chans) {
                key.vs.chan[i].enable = gx.chan[i].enable;
                key.vs.chan[i].amb_src = gx.chan[i].amb_src;
                key.vs.chan[i].mat_src = gx.chan[i].mat_src;
                key.vs.chan[i].diff_fn = gx.chan[i].diff_fn;
                key.vs.chan[i].attn_fn = gx.chan[i].attn_fn;
                key.vs.chan[i].light_mask = gx.chan[i].light_mask & 0xFF;
            }
        }
    }
    if (last_shader != NULL && memcmp(&last_shader->key, &key, sizeof(key)) == 0) {
        return last_shader;
    }
    {
        /* only the stages in use are hashed (equal keys have equal counts) */
        u32 h = hash_words(key.tev, (size_t) key.num_tev * sizeof(TevStage)) ^
                hash_words(&key.num_tev, sizeof(key) - offsetof(ShaderKey, num_tev));
        u16* bucket = shader_buckets[h & (SHADER_BUCKETS - 1)];
        int w;
        for (w = 0; w < SHADER_WAYS; w++) {
            if (bucket[w] != 0 && memcmp(&shaders[bucket[w] - 1].key, &key, sizeof(key)) == 0) {
                last_shader = &shaders[bucket[w] - 1];
                return last_shader;
            }
        }
        if (num_shaders == MAX_SHADERS) {
            num_shaders = 0; /* crude: start over */
            memset(shader_buckets, 0, sizeof(shader_buckets));
            last_shader = NULL;
        }
        sh = &shaders[num_shaders++];
        memset(sh, 0, sizeof(*sh));
        sh->key = key;
        last_shader = sh;
        for (w = SHADER_WAYS - 1; w > 0; w--) {
            bucket[w] = bucket[w - 1];
        }
        bucket[0] = (u16) num_shaders; /* index + 1 */
    }

    shader_len = 0;
    emit("#version 120\n");
    for (i = 0; i < 8; i++) {
        emit("uniform sampler2D u_tex%d;\n", i);
    }
    emit("uniform vec3 u_indmtx[6];\n" /* GX_ITM_0..2, two rows each, scaled */
         "uniform vec2 u_texsize[8];\n"
         "uniform vec4 u_kcolor[4];\n"
         "uniform vec4 u_tevreg[4];\n"
         "uniform vec4 u_aref;\n"
         "uniform vec4 u_fog;\n" /* start, end, unused, unused */
         "uniform vec4 u_fogcolor;\n"
         "varying vec4 v_col0, v_col1;\n"
         "varying vec2 v_tex0, v_tex1, v_tex2, v_tex3, v_tex4, v_tex5, v_tex6, v_tex7;\n"
         "varying float v_depth;\n"
         "void main() {\n"
         "    vec4 prev = u_tevreg[0];\n"
         "    vec4 reg0 = u_tevreg[1];\n"
         "    vec4 reg1 = u_tevreg[2];\n"
         "    vec4 reg2 = u_tevreg[3];\n"
         "    vec4 tex = vec4(1.0);\n"
         "    vec4 ras = vec4(1.0);\n");
    if (key.num_tev == 0) {
        emit("    prev = v_col0;\n");
    }
    for (i = 0; i < key.num_tev; i++) {
        emit_stage(&key, i);
    }
    /* alpha compare */
    {
        const char* c0 = cmp_expr(key.alpha_comp0, "u_aref.x");
        const char* c1 = cmp_expr(key.alpha_comp1, "u_aref.y");
        const char* op;
        switch (key.alpha_op) {
        case GX_AOP_OR: op = "||"; break;
        case GX_AOP_XOR: op = "!="; break;
        case GX_AOP_XNOR: op = "=="; break;
        default: op = "&&"; break;
        }
        if (!debug_flat && !debug_noalpha) {
            emit("    if (!(%s %s %s)) discard;\n", c0, op, c1);
        }
    }
    if (key.fog_type != GX_FOG_NONE && !debug_flat) {
        /* the fog factor from eye distance, the way the console's fog
         * unit derives it from depth; the exponential curves use the
         * hardware's fixed steepness of 8 */
        emit("    float fz = clamp((v_depth - u_fog.x) / max(u_fog.y - u_fog.x, 0.0001), 0.0, 1.0);\n");
        switch (key.fog_type) {
        case GX_FOG_EXP: emit("    float ff = 1.0 - exp2(-8.0 * fz);\n"); break;
        case GX_FOG_EXP2: emit("    float ff = 1.0 - exp2(-8.0 * fz * fz);\n"); break;
        case GX_FOG_REVEXP: emit("    float ff = exp2(-8.0 * (1.0 - fz));\n"); break;
        case GX_FOG_REVEXP2: emit("    float ff = exp2(-8.0 * (1.0 - fz) * (1.0 - fz));\n"); break;
        default: emit("    float ff = fz;\n"); break;
        }
        emit("    prev.rgb = mix(prev.rgb, u_fogcolor.rgb, ff);\n");
    }
    if (debug_flat) {
        emit("    gl_FragColor = vec4(1.0, 0.0, 1.0, 1.0);\n}\n");
    } else {
        emit("    gl_FragColor = clamp(prev, 0.0, 1.0);\n}\n");
    }

    fs = compile(GL_FRAGMENT_SHADER, shader_src);
    if (key.vs.on) {
        static int dumped;
        emit_vs(&key.vs);
        if (getenv("MELEE_GX_DUMP_VS") != NULL && dumped++ < 64) {
            fprintf(stderr, "[gx] vertex shader %d:" "%c" "%s--- end ---" "%c", dumped, 10, shader_src, 10);
        }
        vs = compile(GL_VERTEX_SHADER, shader_src);
    } else {
        vs = compile(GL_VERTEX_SHADER, vsrc);
    }
    sh->prog = pc_glCreateProgram();
    pc_glAttachShader(sh->prog, vs);
    pc_glAttachShader(sh->prog, fs);
    pc_glLinkProgram(sh->prog);
    pc_glDeleteShader(vs);
    pc_glDeleteShader(fs);
    sh->u_proj = pc_glGetUniformLocation(sh->prog, "u_proj");
    sh->u_kcolor = pc_glGetUniformLocation(sh->prog, "u_kcolor");
    sh->u_tevreg = pc_glGetUniformLocation(sh->prog, "u_tevreg");
    sh->u_aref = pc_glGetUniformLocation(sh->prog, "u_aref");
    sh->u_fog = pc_glGetUniformLocation(sh->prog, "u_fog");
    sh->u_fogcolor = pc_glGetUniformLocation(sh->prog, "u_fogcolor");
    sh->u_indmtx = pc_glGetUniformLocation(sh->prog, "u_indmtx");
    sh->u_texsize = pc_glGetUniformLocation(sh->prog, "u_texsize");
    sh->a_pos = pc_glGetAttribLocation(sh->prog, "a_pos");
    sh->a_col0 = pc_glGetAttribLocation(sh->prog, "a_col0");
    sh->a_col1 = pc_glGetAttribLocation(sh->prog, "a_col1");
    sh->a_nrm = pc_glGetAttribLocation(sh->prog, "a_nrm");
    sh->a_bnm = pc_glGetAttribLocation(sh->prog, "a_bnm");
    sh->a_tan = pc_glGetAttribLocation(sh->prog, "a_tan");
    sh->a_blk = pc_glGetAttribLocation(sh->prog, "a_blk");
    sh->a_pnm = pc_glGetAttribLocation(sh->prog, "a_pnm");
    {
        GLint u = pc_glGetUniformLocation(sh->prog, "u_params");
        if (u >= 0) {
            GLint prev_prog = (GLint) cur_prog;
            pc_glUseProgram(sh->prog);
            pc_glUniform1i(u, PARAM_UNIT);
            pc_glUseProgram((GLuint) prev_prog);
        }
    }
    for (i = 0; i < 8; i++) {
        char name[16];
        snprintf(name, sizeof(name), "u_tex%d", i);
        sh->u_tex[i] = pc_glGetUniformLocation(sh->prog, name);
        snprintf(name, sizeof(name), "a_tex%d", i);
        sh->a_tex[i] = pc_glGetAttribLocation(sh->prog, name);
    }
    sh->used = 1;
    return sh;
}

/* --- Drawing --------------------------------------------------------------- */

/* The GL state last applied, so a draw only issues the calls whose
 * values changed (a frame has a thousand draws and most of them share
 * their state with the previous one). Anything that touches this state
 * behind the cache's back (clears, copies, presents) resets `valid`. */
static struct {
    int valid;
    GLint vp[4];
    float depth_range[2];
    GLint scissor[4];
    int cull; /* -1 off, else the GL face */
    int z_enable, z_func, z_update;
    int color_update, alpha_update;
    int blend_mode, blend_src, blend_dst, logic_op;
} rs;
/* cur_prog (the program in use) is declared with the shader cache */
static Shader* attrib_shader;   /* whose vertex attributes are set up ... */
static void* attrib_base;       /* ... for this vertex buffer */

/* --- The scene framebuffer ------------------------------------------------
 * The frame is rendered into an off-screen framebuffer of its own size:
 * `--internal N` times 640x480, or the window's 4:3 area when N is 0 (the
 * default). With `--msaa` it is multisampled and resolved on the way out.
 * The EFB copies, the screenshots and the present read it back; the
 * present scales it into the window. Without the framebuffer extensions
 * (or if creating it fails) the frame is drawn straight into the window
 * as before. */
static GLuint scene_fbo, scene_color, scene_depth, resolve_fbo, resolve_color;
static int scene_w, scene_h, scene_samples, scene_failed;

static void scene_destroy(void)
{
    if (scene_fbo != 0) {
        pc_glDeleteFramebuffers(1, &scene_fbo);
        pc_glDeleteRenderbuffers(1, &scene_color);
        pc_glDeleteRenderbuffers(1, &scene_depth);
    }
    if (resolve_fbo != 0) {
        pc_glDeleteFramebuffers(1, &resolve_fbo);
        pc_glDeleteRenderbuffers(1, &resolve_color);
    }
    scene_fbo = scene_color = scene_depth = resolve_fbo = resolve_color = 0;
    scene_w = scene_h = scene_samples = 0;
}

static int scene_create(int w, int h, int samples)
{
    pc_glGenFramebuffers(1, &scene_fbo);
    pc_glGenRenderbuffers(1, &scene_color);
    pc_glGenRenderbuffers(1, &scene_depth);
    pc_glBindRenderbuffer(GL_RENDERBUFFER, scene_color);
    if (samples > 0) {
        pc_glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, w, h);
    } else {
        pc_glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
    }
    pc_glBindRenderbuffer(GL_RENDERBUFFER, scene_depth);
    if (samples > 0) {
        pc_glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, w, h);
    } else {
        pc_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    }
    pc_glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo);
    pc_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, scene_color);
    pc_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, scene_depth);
    if (pc_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        pc_glBindFramebuffer(GL_FRAMEBUFFER, 0);
        scene_destroy();
        return 0;
    }
    if (samples > 0) {
        pc_glGenFramebuffers(1, &resolve_fbo);
        pc_glGenRenderbuffers(1, &resolve_color);
        pc_glBindRenderbuffer(GL_RENDERBUFFER, resolve_color);
        pc_glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
        pc_glBindFramebuffer(GL_FRAMEBUFFER, resolve_fbo);
        pc_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, resolve_color);
        if (pc_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            pc_glBindFramebuffer(GL_FRAMEBUFFER, 0);
            scene_destroy();
            return 0;
        }
    }
    pc_glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo);
    scene_w = w;
    scene_h = h;
    scene_samples = samples;
    glGetError();
    return 1;
}

/* (Re)create the scene framebuffer for the current window and options. */
static void ensure_scene_target(void)
{
    int vx, vy, vw, vh, w, h, samples;
    if (!scene_failed && getenv("MELEE_GX_NOFBO") != NULL) {
        scene_failed = 1; /* MELEE_GX_NOFBO=1: draw straight into the window */
    }
    if (scene_failed || pc_glGenFramebuffers == NULL || pc_glGenRenderbuffers == NULL ||
        pc_glBlitFramebuffer == NULL || pc_glFramebufferRenderbuffer == NULL)
    {
        return;
    }
    pc_window_viewport(&vx, &vy, &vw, &vh);
    if (pc_config.internal_scale > 0) {
        w = EFB_W * pc_config.internal_scale;
        h = EFB_H * pc_config.internal_scale;
    } else {
        w = vw;
        h = vh;
    }
    samples = pc_config.msaa;
    if (samples == 1) {
        samples = 0;
    }
    if (samples > 0) {
        GLint max_samples = 0;
        glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
        if (samples > max_samples) {
            samples = max_samples;
        }
    }
    if (scene_fbo != 0 && scene_w == w && scene_h == h && scene_samples == samples) {
        return;
    }
    flush_batch();
    scene_destroy();
    if (!scene_create(w, h, samples) && !(samples > 0 && scene_create(w, h, 0))) {
        scene_failed = 1;
        fprintf(stderr, "[gx] scene framebuffer %dx%d could not be created: drawing into the window\n", w, h);
        return;
    }
    rs.valid = 0;
    fprintf(stderr, "[gx] rendering at %dx%d%s%s\n", scene_w, scene_h,
            scene_samples > 0 ? ", multisampled x" : "", scene_samples > 0 ? "" : "");
    if (scene_samples > 0) {
        fprintf(stderr, "[gx] anti-aliasing: %d samples\n", scene_samples);
    }
}

static void bind_scene(void)
{
    pc_glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo);
}

/* The rectangle the frame is rendered into: the whole scene framebuffer,
 * or the window's 4:3 area when drawing straight into the window. */
static void target_rect(int* x, int* y, int* w, int* h)
{
    if (scene_fbo == 0 && !scene_failed && rendering) {
        ensure_scene_target();
    }
    if (scene_fbo != 0) {
        *x = 0;
        *y = 0;
        *w = scene_w;
        *h = scene_h;
    } else {
        pc_window_viewport(x, y, w, h);
    }
}

/* A framebuffer the region can be read from: the scene itself, or, when it
 * is multisampled, the resolve buffer with that region resolved into it. */
static GLuint scene_read_fbo(GLint x, GLint y, GLsizei w, GLsizei h)
{
    if (scene_fbo != 0 && scene_samples > 0) {
        flush_batch();
        glDisable(GL_SCISSOR_TEST);
        rs.valid = 0;
        pc_glBindFramebuffer(GL_READ_FRAMEBUFFER, scene_fbo);
        pc_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolve_fbo);
        pc_glBlitFramebuffer(x, y, x + w, y + h, x, y, x + w, y + h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        return resolve_fbo;
    }
    return scene_fbo;
}

/* Scale the finished frame into the window. */
static void present_scene(void)
{
    int vx, vy, vw, vh;
    GLuint src;
    if (scene_fbo == 0) {
        return;
    }
    flush_batch();
    pc_window_viewport(&vx, &vy, &vw, &vh);
    src = scene_read_fbo(0, 0, scene_w, scene_h);
    glDisable(GL_SCISSOR_TEST);
    rs.valid = 0;
    pc_glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
    pc_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    pc_glBlitFramebuffer(0, 0, scene_w, scene_h, vx, vy, vx + vw, vy + vh, GL_COLOR_BUFFER_BIT,
                         (vw == scene_w && vh == scene_h) ? GL_NEAREST : GL_LINEAR);
}

static void apply_raster_state(void)
{
    int vx, vy, vw, vh;
    float sx, sy;
    GLint vp[4], sc[4];
    int cull;
    target_rect(&vx, &vy, &vw, &vh);
    sx = (float) vw / EFB_W;
    sy = (float) vh / EFB_H;

    vp[0] = vx + (GLint) (gx.vp[0] * sx);
    vp[1] = vy + (GLint) ((EFB_H - gx.vp[1] - gx.vp[3]) * sy);
    vp[2] = (GLint) (gx.vp[2] * sx);
    vp[3] = (GLint) (gx.vp[3] * sy);
    if (!rs.valid || memcmp(vp, rs.vp, sizeof(vp)) != 0) {
        flush_batch();
        glViewport(vp[0], vp[1], (GLsizei) vp[2], (GLsizei) vp[3]);
        memcpy(rs.vp, vp, sizeof(vp));
    }
    if (!rs.valid || rs.depth_range[0] != gx.vp[4] || rs.depth_range[1] != gx.vp[5]) {
        flush_batch();
        glDepthRange(gx.vp[4], gx.vp[5]);
        rs.depth_range[0] = gx.vp[4];
        rs.depth_range[1] = gx.vp[5];
    }
    sc[0] = vx + (GLint) (gx.scissor[0] * sx);
    sc[1] = vy + (GLint) ((EFB_H - (int) gx.scissor[1] - (int) gx.scissor[3]) * sy);
    sc[2] = (GLint) (gx.scissor[2] * sx);
    sc[3] = (GLint) (gx.scissor[3] * sy);
    if (!rs.valid || memcmp(sc, rs.scissor, sizeof(sc)) != 0) {
        flush_batch();
        glEnable(GL_SCISSOR_TEST);
        glScissor(sc[0], sc[1], (GLsizei) sc[2], (GLsizei) sc[3]);
        memcpy(rs.scissor, sc, sizeof(sc));
    }

    cull = (gx.cull == GX_CULL_NONE || debug_nocull) ? -1
           : gx.cull == GX_CULL_FRONT                 ? GL_FRONT
           : gx.cull == GX_CULL_BACK                  ? GL_BACK
                                                      : GL_FRONT_AND_BACK;
    if (!rs.valid || cull != rs.cull) {
        flush_batch();
        if (cull < 0) {
            glDisable(GL_CULL_FACE);
        } else {
            glEnable(GL_CULL_FACE);
            glFrontFace(GL_CW);
            glCullFace((GLenum) cull);
        }
        rs.cull = cull;
    }
    if (!rs.valid || rs.z_enable != gx.z_enable || rs.z_func != gx.z_func) {
        static const GLenum funcs[8] = { GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL,
                                         GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS };
        flush_batch();
        if (gx.z_enable) {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(funcs[gx.z_func & 7]);
        } else {
            glDisable(GL_DEPTH_TEST);
        }
        rs.z_enable = gx.z_enable;
        rs.z_func = gx.z_func;
    }
    if (!rs.valid || rs.z_update != gx.z_update) {
        flush_batch();
        glDepthMask(gx.z_update ? GL_TRUE : GL_FALSE);
        rs.z_update = gx.z_update;
    }
    if (!rs.valid || rs.color_update != gx.color_update || rs.alpha_update != gx.alpha_update) {
        flush_batch();
        glColorMask(gx.color_update, gx.color_update, gx.color_update, gx.alpha_update);
        rs.color_update = gx.color_update;
        rs.alpha_update = gx.alpha_update;
    }

    if (!rs.valid || rs.blend_mode != gx.blend_mode || rs.blend_src != gx.blend_src || rs.blend_dst != gx.blend_dst ||
        rs.logic_op != gx.logic_op)
    {
        flush_batch();
        if (gx.blend_mode == GX_BM_NONE) {
            glDisable(GL_BLEND);
            glDisable(GL_COLOR_LOGIC_OP);
        } else if (gx.blend_mode == GX_BM_LOGIC) {
            static const GLenum ops[16] = { GL_CLEAR, GL_AND, GL_AND_REVERSE, GL_COPY, GL_AND_INVERTED,
                                            GL_NOOP, GL_XOR, GL_OR, GL_NOR, GL_EQUIV, GL_INVERT,
                                            GL_OR_REVERSE, GL_COPY_INVERTED, GL_OR_INVERTED, GL_NAND, GL_SET };
            glDisable(GL_BLEND);
            glEnable(GL_COLOR_LOGIC_OP);
            glLogicOp(ops[gx.logic_op & 15]);
        } else {
            static const GLenum factors[8] = { GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR,
                                               GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA,
                                               GL_ONE_MINUS_DST_ALPHA };
            GLenum src = factors[gx.blend_src & 7], dst = factors[gx.blend_dst & 7];
            /* GX_BL_DSTCLR / INVDSTCLR share codes with SRCCLR for the source factor */
            if (gx.blend_src == GX_BL_SRCCLR) src = GL_DST_COLOR;
            if (gx.blend_src == GX_BL_INVSRCCLR) src = GL_ONE_MINUS_DST_COLOR;
            glDisable(GL_COLOR_LOGIC_OP);
            glEnable(GL_BLEND);
            if (gx.blend_mode == GX_BM_SUBTRACT) {
                pc_glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
                glBlendFunc(GL_ONE, GL_ONE);
            } else {
                pc_glBlendEquation(GL_FUNC_ADD);
                glBlendFunc(src, dst);
            }
        }
        rs.blend_mode = gx.blend_mode;
        rs.blend_src = gx.blend_src;
        rs.blend_dst = gx.blend_dst;
        rs.logic_op = gx.logic_op;
    }
    rs.valid = 1;
}

static void ensure_capacity(u32 nverts, u32 nindices)
{
    if (nverts > glverts_cap && !(use_ring > 0 && gpu_path <= 0)) {
        if (use_ring <= 0) {
            flush_batch(); /* the attribute pointers refer to the old buffer */
        }
        glverts_cap = nverts + 4096;
        glverts = (GLVertex*) realloc(glverts, glverts_cap * sizeof(GLVertex));
    }
    if (gpu_path > 0 && use_ring <= 0 && nverts > gverts_cap) {
        flush_batch();
        gverts_cap = nverts + 4096;
        gverts = (GLVertexG*) realloc(gverts, gverts_cap * sizeof(GLVertexG));
    }
    if (nindices > indices_cap) {
        indices_cap = nindices + 4096;
        indices = (u16*) realloc(indices, indices_cap * sizeof(u16));
    }
}

static double prof_now(void);

static void upload_params(void)
{
    if (gpu_path > 0 && param_uploaded < param_pos) {
        double t0 = prof_now();
        u32 a = param_uploaded, b = param_pos;
        use_unit(PARAM_UNIT);
        /* only the texels written since the last upload, row by row */
        while (a < b) {
            u32 y = a / PARAM_W, x = a % PARAM_W, n = b - a;
            if (n > PARAM_W - x) {
                n = PARAM_W - x;
            }
            glTexSubImage2D(GL_TEXTURE_2D, 0, (GLint) x, (GLint) y, (GLsizei) n, 1, GL_RGBA, GL_FLOAT,
                            param_img + (size_t) a * 4);
            a += n;
        }
        param_uploaded = param_pos;
        prof_ms[7] += prof_now() - t0;
    }
}

static void flush_batch(void)
{
    if (batch_idx != 0) {
        double t0;
        upload_params();
        t0 = prof_now();
        if (use_ring > 0) {
            pc_glDrawElementsBaseVertex(GL_TRIANGLES, (GLsizei) batch_idx, GL_UNSIGNED_SHORT, indices, (GLint) ring_base_v);
        } else if (pc_glDrawRangeElements != NULL) {
            /* the vertex range spares the driver a scan of the indices
             * before it copies the client-side arrays */
            pc_glDrawRangeElements(GL_TRIANGLES, batch_min, batch_verts - 1, (GLsizei) batch_idx, GL_UNSIGNED_SHORT, indices);
        } else {
            glDrawElements(GL_TRIANGLES, (GLsizei) batch_idx, GL_UNSIGNED_SHORT, indices);
        }
        prof_ms[8] += prof_now() - t0;
        stats_gl_draws++;
    }
    if (use_ring > 0) {
        /* the batch's vertices stay until their region is reused; the base
         * moves with the cursor so a flush with nothing pending changes
         * nothing */
        ring_pos_v = ring_base_v + batch_verts;
        ring_base_v = ring_pos_v;
    }
    batch_idx = 0;
    batch_verts = 0;
}

static void ring_init(void)
{
    const char* ver = (const char*) glGetString(GL_VERSION);
    use_ring = 0;
    if (getenv("MELEE_GX_NORING") != NULL || pc_glBufferStorage == NULL || pc_glMapBufferRange == NULL ||
        pc_glDrawElementsBaseVertex == NULL || pc_glFenceSync == NULL || pc_glClientWaitSync == NULL ||
        pc_glGenBuffers == NULL || pc_glBindBuffer == NULL || ver == NULL || ver[0] < '4')
    {
        fprintf(stderr, "[gx] vertex buffer: client arrays\n");
        return;
    }
    ring_stride = gpu_path > 0 ? sizeof(GLVertexG) : sizeof(GLVertex);
    ring_verts = RING_BYTES / ring_stride;
    ring_region_verts = ring_verts / RING_REGIONS;
    pc_glGenBuffers(1, &ring_vbo);
    pc_glBindBuffer(GL_ARRAY_BUFFER, ring_vbo);
    pc_glBufferStorage(GL_ARRAY_BUFFER, (GLsizeiptr) RING_BYTES, NULL,
                       GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
    ring_map = (u8*) pc_glMapBufferRange(GL_ARRAY_BUFFER, 0, (GLsizeiptr) RING_BYTES,
                                          GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
    if (ring_map == NULL || glGetError() != GL_NO_ERROR) {
        pc_glBindBuffer(GL_ARRAY_BUFFER, 0);
        fprintf(stderr, "[gx] vertex buffer: client arrays (the mapped ring could not be created)\n");
        return;
    }
    use_ring = 1;
    ring_region = -1;
    ring_base_v = ring_pos_v = 0;
    attrib_shader = NULL; /* pointers become buffer offsets */
    fprintf(stderr, "[gx] vertex buffer: persistently mapped ring (%u MB)\n", RING_BYTES >> 20);
}

/* Room in the ring for the next primitive's vertices; positions the batch. */
static void ring_begin(u32 nverts)
{
    u32 end, region;
    if (batch_idx == 0 && batch_verts == 0) {
        ring_base_v = ring_pos_v;
    }
    end = ring_base_v + batch_verts + nverts;
    region = (end - 1) / ring_region_verts;
    if (region >= RING_REGIONS || (int) region != ring_region) {
        /* leaving a region: draw what is pending, fence the region left
         * behind, and start on the next one at its beginning once the
         * draws that used it a cycle ago are done */
        flush_batch();
        if (ring_region >= 0) {
            if (ring_fence[ring_region] != NULL) {
                pc_glDeleteSync(ring_fence[ring_region]);
            }
            ring_fence[ring_region] = pc_glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        }
        if (region >= RING_REGIONS) {
            region = 0;
        }
        if (ring_fence[region] != NULL) {
            pc_glClientWaitSync(ring_fence[region], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
            pc_glDeleteSync(ring_fence[region]);
            ring_fence[region] = NULL;
        }
        ring_region = (int) region;
        ring_base_v = ring_pos_v = region * ring_region_verts;
    }
    if (gpu_path > 0) {
        gverts = (GLVertexG*) (ring_map + (size_t) ring_base_v * ring_stride);
    } else {
        glverts = (GLVertex*) (ring_map + (size_t) ring_base_v * ring_stride);
    }
}

/* Append a block of texels to the parameter texture; returns its index. */
static u32 param_append(const float* data, u32 texels)
{
    u32 at;
    if (param_pos + texels > PARAM_TEXELS) {
        /* wrap: everything pending is drawn first, so nothing refers to
         * the rows about to be rewritten */
        flush_batch();
        param_pos = param_uploaded = 0;
        blk_idx[0] = blk_idx[1] = blk_idx[2] = blk_idx[3] = -1;
    }
    at = param_pos;
    memcpy(param_img + (size_t) at * 4, data, (size_t) texels * 16);
    param_pos += texels;
    return at;
}

/* The blocks a draw's vertices refer to, rewritten when their state changed. */
static void ensure_blocks(void)
{
    static float buf[TX_BLOCK * 4];
    int i, j;
    if (param_pos + PN_BLOCK + TX_BLOCK + LT_BLOCK + MT_BLOCK > PARAM_TEXELS) {
        /* wrap before any block of this draw is written, so a draw never
         * refers to blocks on both sides of the wrap */
        flush_batch();
        param_pos = param_uploaded = 0;
        blk_idx[0] = blk_idx[1] = blk_idx[2] = blk_idx[3] = -1;
    }
    if (blk_dirty[0] || blk_idx[0] < 0) {
        memcpy(buf, gx.mtx[0], 30 * 4 * sizeof(float));
        for (i = 0; i < 10; i++) {
            for (j = 0; j < 3; j++) {
                float* t = buf + (30 + i * 3 + j) * 4;
                t[0] = gx.nrm[i][j][0];
                t[1] = gx.nrm[i][j][1];
                t[2] = gx.nrm[i][j][2];
                t[3] = 0.0f;
            }
        }
        blk_idx[0] = (int) param_append(buf, PN_BLOCK);
        blk_dirty[0] = 0;
    }
    if (blk_dirty[1] || blk_idx[1] < 0) {
        memcpy(buf, gx.mtx[30], 98 * 4 * sizeof(float));
        for (i = 0; i < 22; i++) {
            for (j = 0; j < 3; j++) {
                float* t = buf + (98 + i * 3 + j) * 4;
                t[0] = gx.nrm[10 + i][j][0];
                t[1] = gx.nrm[10 + i][j][1];
                t[2] = gx.nrm[10 + i][j][2];
                t[3] = 0.0f;
            }
        }
        blk_idx[1] = (int) param_append(buf, TX_BLOCK);
        blk_dirty[1] = 0;
    }
    if (blk_dirty[2] || blk_idx[2] < 0) {
        for (i = 0; i < 8; i++) {
            const Light* l = &gx.lights[i];
            float* t = buf + i * 16;
            t[0] = l->pos[0]; t[1] = l->pos[1]; t[2] = l->pos[2]; t[3] = l->a0;
            t[4] = l->dir[0]; t[5] = l->dir[1]; t[6] = l->dir[2]; t[7] = l->a1;
            t[8] = l->color[0]; t[9] = l->color[1]; t[10] = l->color[2]; t[11] = l->color[3];
            t[12] = l->k0; t[13] = l->k1; t[14] = l->k2; t[15] = l->a2;
        }
        blk_idx[2] = (int) param_append(buf, LT_BLOCK);
        blk_dirty[2] = 0;
    }
    if (blk_dirty[3] || blk_idx[3] < 0) {
        memcpy(buf, gx.mat_color[0], 4 * sizeof(float));
        memcpy(buf + 4, gx.mat_color[1], 4 * sizeof(float));
        memcpy(buf + 8, gx.amb_color[0], 4 * sizeof(float));
        memcpy(buf + 12, gx.amb_color[1], 4 * sizeof(float));
        blk_idx[3] = (int) param_append(buf, MT_BLOCK);
        blk_dirty[3] = 0;
    }
}

static void fill_gvertex(const Vertex* v, GLVertexG* g)
{
    u32 slot = v->pnmtx;
    memcpy(g->pos, v->pos, sizeof(g->pos));
    memcpy(g->nrm, v->nrm, sizeof(g->nrm));
    memcpy(g->bnm, v->bnm, sizeof(g->bnm));
    memcpy(g->tan, v->tan, sizeof(g->tan));
    memcpy(g->col, v->col, sizeof(g->col));
    memcpy(g->tex, v->tex, sizeof(g->tex));
    g->blk[0] = (float) blk_idx[0];
    g->blk[1] = (float) blk_idx[1];
    g->blk[2] = (float) blk_idx[2];
    g->blk[3] = (float) blk_idx[3];
    g->pnm = (float) (slot < 30 ? slot : 0);
}

static void use_unit(int unit)
{
    if (active_unit != unit) {
        pc_glActiveTexture(GL_TEXTURE0 + unit);
        active_unit = unit;
    }
}

/* bind a texture to a unit, if it is not there already */
static void bind_unit(int unit, GLuint tex)
{
    if (unit_tex[unit] != tex) {
        flush_batch();
        use_unit(unit);
        glBindTexture(GL_TEXTURE_2D, tex);
        unit_tex[unit] = tex;
    }
}

/* a texture is about to be deleted or re-uploaded */
static void forget_texture(GLuint tex)
{
    int u;
    flush_batch();
    for (u = 0; u < 9; u++) {
        if (unit_tex[u] == tex) {
            unit_tex[u] = 0xFFFFFFFFu;
        }
    }
}

/// Draw nverts vertices from a stream in the given byte order.
/* MELEE_GX_PROFILE=1: where a frame's wall time goes, printed every 300
 * frames (draws = vertex decode, transform, lighting and GL submission;
 * shader and texture = the lookups within them; copy = EFB copies;
 * present = the swap, which includes the vsync wait). */
/* prof_ms is declared with the batching state */
static int prof_on = -1;

static double prof_now(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    QueryPerformanceCounter(&now);
    return (double) now.QuadPart * 1000.0 / (double) freq.QuadPart;
}

static Shader* draw_setup(void);
static void draw_prims(Shader* sh, u8 prim, u8 vat, const u8* stream, u32 nverts, int big);

/* MELEE_GX_NOSHADOW=1: skip the shadow-map passes (256x256 viewport) */
static int skip_shadow_pass(void)
{
    static int noshadow = -1;
    if (noshadow < 0) {
        noshadow = getenv("MELEE_GX_NOSHADOW") != NULL;
    }
    return noshadow && gx.vp[2] == 256.0f && gx.vp[3] == 256.0f;
}

static void draw_stream(u8 prim, u8 vat, const u8* stream, u32 nverts, int big)
{
    double t0 = prof_now();
    Shader* sh;
    if (skip_shadow_pass()) {
        return;
    }
    sh = draw_setup();
    if (sh != NULL) {
        draw_prims(sh, prim, vat, stream, nverts, big);
    }
    prof_ms[1] += prof_now() - t0;
}

#ifndef GL_RGBA32F
#define GL_RGBA32F 0x8814
#endif

/* First draw: the GPU path needs float textures (GL 3.0, or the ARB
 * extension every driver of the last fifteen years has). */
static void gpu_path_init(void)
{
    const char* ver = (const char*) glGetString(GL_VERSION);
    const char* ext = (const char*) glGetString(GL_EXTENSIONS);
    int ok = (ver != NULL && ver[0] >= '3') || (ext != NULL && strstr(ext, "GL_ARB_texture_float") != NULL);
    if (gpu_path == 0 || !ok) {
        gpu_path = 0;
        fprintf(stderr, "[gx] vertex path: CPU%s\n", ok ? " (MELEE_GX_CPU)" : " (no float textures)");
        return;
    }
    param_img = (float*) calloc((size_t) PARAM_TEXELS * 4, sizeof(float));
    if (param_img == NULL) {
        gpu_path = 0;
        return;
    }
    glGenTextures(1, &param_tex);
    use_unit(PARAM_UNIT);
    glBindTexture(GL_TEXTURE_2D, param_tex);
    unit_tex[PARAM_UNIT] = param_tex;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, PARAM_W, PARAM_H, 0, GL_RGBA, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (glGetError() != GL_NO_ERROR) {
        gpu_path = 0;
        fprintf(stderr, "[gx] vertex path: CPU (the parameter texture could not be created)\n");
        return;
    }
    gpu_path = 1;
    blk_dirty[0] = blk_dirty[1] = blk_dirty[2] = blk_dirty[3] = 1;
    key_dirty = 1;
    fprintf(stderr, "[gx] vertex path: GPU\n");
}

static void aniso_init(void)
{
    const char* ext = (const char*) glGetString(GL_EXTENSIONS);
    if (ext != NULL && strstr(ext, "GL_EXT_texture_filter_anisotropic") != NULL) {
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &aniso_max);
        if (pc_config.aniso > 1) {
            fprintf(stderr, "[gx] anisotropic filtering: x%d (the driver allows x%.0f)\n", pc_config.aniso, aniso_max);
        }
    }
}

/* what the attribute pointers were last set up for */
static void* vbuf_id(void)
{
    if (use_ring > 0) {
        return (void*) 1; /* offsets into the ring: fixed */
    }
    return gpu_path > 0 ? (void*) gverts : (void*) glverts;
}

/* the vertex attribute pointers of a program, into the current vertex buffer */
static void setup_attribs(Shader* sh)
{
    int t;
    const u8* vbase = use_ring > 0 ? NULL : (gpu_path > 0 ? (const u8*) gverts : (const u8*) glverts);
    {
        /* the vertex layout is fixed; only the attribute locations depend
         * on the program and the pointers on the (rarely reallocated)
         * vertex buffer */
        Shader* old = attrib_shader;
        flush_batch();
        if (old != NULL) {
            if (old->a_pos >= 0) pc_glDisableVertexAttribArray(old->a_pos);
            if (old->a_col0 >= 0) pc_glDisableVertexAttribArray(old->a_col0);
            if (old->a_col1 >= 0) pc_glDisableVertexAttribArray(old->a_col1);
            if (old->a_nrm >= 0) pc_glDisableVertexAttribArray(old->a_nrm);
            if (old->a_bnm >= 0) pc_glDisableVertexAttribArray(old->a_bnm);
            if (old->a_tan >= 0) pc_glDisableVertexAttribArray(old->a_tan);
            if (old->a_blk >= 0) pc_glDisableVertexAttribArray(old->a_blk);
            if (old->a_pnm >= 0) pc_glDisableVertexAttribArray(old->a_pnm);
            for (t = 0; t < 8; t++) {
                if (old->a_tex[t] >= 0) pc_glDisableVertexAttribArray(old->a_tex[t]);
            }
        }
        if (gpu_path > 0) {
            const GLsizei st = sizeof(GLVertexG);
#define GATTR(loc, n, field) \
    if ((loc) >= 0) { \
        pc_glEnableVertexAttribArray(loc); \
        pc_glVertexAttribPointer(loc, n, GL_FLOAT, GL_FALSE, st, vbase + offsetof(GLVertexG, field)); \
    }
            GATTR(sh->a_pos, 3, pos);
            GATTR(sh->a_nrm, 3, nrm);
            GATTR(sh->a_bnm, 3, bnm);
            GATTR(sh->a_tan, 3, tan);
            GATTR(sh->a_col0, 4, col[0]);
            GATTR(sh->a_col1, 4, col[1]);
            GATTR(sh->a_blk, 4, blk);
            GATTR(sh->a_pnm, 1, pnm);
            for (t = 0; t < 8; t++) {
                GATTR(sh->a_tex[t], 2, tex[t]);
            }
#undef GATTR
        } else {
            if (sh->a_pos >= 0) {
                pc_glEnableVertexAttribArray(sh->a_pos);
                pc_glVertexAttribPointer(sh->a_pos, 3, GL_FLOAT, GL_FALSE, sizeof(GLVertex), vbase + offsetof(GLVertex, pos));
            }
            if (sh->a_col0 >= 0) {
                pc_glEnableVertexAttribArray(sh->a_col0);
                pc_glVertexAttribPointer(sh->a_col0, 4, GL_FLOAT, GL_FALSE, sizeof(GLVertex), vbase + offsetof(GLVertex, col[0]));
            }
            if (sh->a_col1 >= 0) {
                pc_glEnableVertexAttribArray(sh->a_col1);
                pc_glVertexAttribPointer(sh->a_col1, 4, GL_FLOAT, GL_FALSE, sizeof(GLVertex), vbase + offsetof(GLVertex, col[1]));
            }
            for (t = 0; t < 8; t++) {
                if (sh->a_tex[t] >= 0) {
                    pc_glEnableVertexAttribArray(sh->a_tex[t]);
                    pc_glVertexAttribPointer(sh->a_tex[t], 2, GL_FLOAT, GL_FALSE, sizeof(GLVertex), vbase + offsetof(GLVertex, tex[t]));
                }
            }
        }
        attrib_base = vbuf_id();
        attrib_shader = sh;
    }
}

/* Everything a draw needs before its vertices: raster state, program,
 * uniforms, textures, attribute pointers, parameter blocks. A display
 * list does this once for all its primitives. Returns NULL when there is
 * nothing to render into. */
static Shader* draw_setup(void)
{
    Shader* sh;
    int i, t;
    float proj[16];

    if (!rendering) {
        return NULL;
    }
    if (gpu_path < 0) {
        gpu_path_init();
        aniso_init();
    }
    if (use_ring < 0) {
        ring_init();
    }
    build_active_attrs();
    xform_want_nbt = 0;
    for (i = 0; i < gx.num_texgen; i++) {
        if (gx.texgen[i].type >= GX_TG_BUMP0 && gx.texgen[i].type <= GX_TG_BUMP7) {
            xform_want_nbt = 1;
        }
    }
    apply_raster_state();
    {
        double t0 = prof_now();
        sh = get_shader();
        prof_ms[2] += prof_now() - t0;
    }
    if (sh->prog != cur_prog) {
        flush_batch();
        pc_glUseProgram(sh->prog);
        cur_prog = sh->prog;
        if (!sh->uniforms_valid) {
            /* the sampler units never change */
            for (t = 0; t < 8; t++) {
                if (sh->u_tex[t] >= 0) {
                    pc_glUniform1i(sh->u_tex[t], t);
                }
            }
        }
    }

    /* projection, column-major for GL */
    for (i = 0; i < 4; i++) {
        proj[i * 4 + 0] = gx.proj[0][i];
        proj[i * 4 + 1] = gx.proj[1][i];
        proj[i * 4 + 2] = gx.proj[2][i];
        proj[i * 4 + 3] = gx.proj[3][i];
    }
    if (!sh->uniforms_valid || memcmp(proj, sh->last_proj, sizeof(proj)) != 0) {
        flush_batch();
        pc_glUniformMatrix4fv(sh->u_proj, 1, GL_FALSE, proj);
        memcpy(sh->last_proj, proj, sizeof(proj));
    }
    if (!sh->uniforms_valid || memcmp(&gx.kcolor[0][0], sh->last_kcolor, sizeof(sh->last_kcolor)) != 0) {
        flush_batch();
        pc_glUniform4fv(sh->u_kcolor, 4, &gx.kcolor[0][0]);
        memcpy(sh->last_kcolor, &gx.kcolor[0][0], sizeof(sh->last_kcolor));
    }
    if (!sh->uniforms_valid || memcmp(&gx.tev_reg[0][0], sh->last_tevreg, sizeof(sh->last_tevreg)) != 0) {
        flush_batch();
        pc_glUniform4fv(sh->u_tevreg, 4, &gx.tev_reg[0][0]);
        memcpy(sh->last_tevreg, &gx.tev_reg[0][0], sizeof(sh->last_tevreg));
    }
    {
        float aref[4] = { gx.alpha_ref0 / 255.0f, gx.alpha_ref1 / 255.0f, 0, 0 };
        if (!sh->uniforms_valid || memcmp(aref, sh->last_aref, sizeof(aref)) != 0) {
            flush_batch();
            pc_glUniform4fv(sh->u_aref, 1, aref);
            memcpy(sh->last_aref, aref, sizeof(aref));
        }
    }
    if (sh->key.fog_type != GX_FOG_NONE) {
        float fog[4] = { gx.fog_start, gx.fog_end, gx.fog_near, gx.fog_far };
        if (!sh->uniforms_valid || memcmp(fog, sh->last_fog, sizeof(fog)) != 0) {
            flush_batch();
            pc_glUniform4fv(sh->u_fog, 1, fog);
            memcpy(sh->last_fog, fog, sizeof(fog));
        }
        if (!sh->uniforms_valid || memcmp(gx.fog_color, sh->last_fogcolor, sizeof(sh->last_fogcolor)) != 0) {
            flush_batch();
            pc_glUniform4fv(sh->u_fogcolor, 1, gx.fog_color);
            memcpy(sh->last_fogcolor, gx.fog_color, sizeof(sh->last_fogcolor));
        }
    }
    if (sh->u_indmtx >= 0) {
        float m[18];
        for (i = 0; i < 3; i++) {
            memcpy(m + i * 6, gx.ind_mtx[i + 1][0], 3 * sizeof(float));
            memcpy(m + i * 6 + 3, gx.ind_mtx[i + 1][1], 3 * sizeof(float));
        }
        if (!sh->uniforms_valid || memcmp(m, sh->last_indmtx, sizeof(m)) != 0) {
            flush_batch();
            pc_glUniform3fv(sh->u_indmtx, 6, m);
            memcpy(sh->last_indmtx, m, sizeof(m));
        }
    }
    if (sh->u_texsize >= 0) {
        float ts[16];
        for (t = 0; t < 8; t++) {
            ts[t * 2] = gx.texmap[t].width > 0 ? (float) gx.texmap[t].width : 1.0f;
            ts[t * 2 + 1] = gx.texmap[t].height > 0 ? (float) gx.texmap[t].height : 1.0f;
        }
        if (!sh->uniforms_valid || memcmp(ts, sh->last_texsize, sizeof(ts)) != 0) {
            flush_batch();
            pc_glUniform2fv(sh->u_texsize, 8, ts);
            memcpy(sh->last_texsize, ts, sizeof(ts));
        }
    }
    sh->uniforms_valid = 1;
    for (t = 0; t < 8; t++) {
        if (sh->u_tex[t] >= 0 && gx.texmap[t].image != NULL) {
            double t0 = prof_now();
            bind_texture(&gx.texmap[t], t);
            prof_ms[3] += prof_now() - t0;
        }
    }

    if (sh != attrib_shader || vbuf_id() != attrib_base) {
        setup_attribs(sh);
    }
    if (gpu_path > 0) {
        ensure_blocks();
    }
    return sh;
}

/* One primitive's vertices into the batch (after draw_setup). */
static void draw_prims(Shader* sh, u8 prim, u8 vat, const u8* stream, u32 nverts, int big)
{
    const u8* p = stream;
    u32 i, base;
    GLenum mode;

    if (nverts == 0) {
        return;
    }
    if (nverts > MAX_VERTS) {
        nverts = MAX_VERTS;
    }
    if (batch_verts + nverts > 65535) {
        flush_batch(); /* 16-bit indices */
    }
    ensure_capacity(batch_verts + nverts, batch_idx + nverts * 3);
    if (use_ring > 0) {
        ring_begin(nverts);
    }
    if (vbuf_id() != attrib_base) {
        setup_attribs(sh); /* the vertex buffer moved */
    }
    base = batch_verts;
    {
        double t0 = prof_now();
        int logging = (debug_log && stats_draws < 8) || (debug_log_frame != 0 && pc_frame_count == debug_log_frame);
        for (i = 0; i < nverts; i++) {
            Vertex v;
            p = decode_vertex(p, vat, big, &v);
            if (i == 0) {
                first_vertex = v; /* for the draw log */
            }
            if (gpu_path > 0) {
                fill_gvertex(&v, &gverts[base + i]);
                if (logging) {
                    transform_vertex(&v, &glverts[base + i]); /* eye-space positions for the log */
                }
            } else {
                transform_vertex(&v, &glverts[base + i]);
            }
        }
        prof_ms[6] += prof_now() - t0;
    }
    if ((debug_log && stats_draws < 8) || (debug_log_frame != 0 && pc_frame_count == debug_log_frame)) {
        float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
        int k;
        for (i = 0; i < nverts; i++) {
            for (k = 0; k < 3; k++) {
                lo[k] = glverts[base + i].pos[k] < lo[k] ? glverts[base + i].pos[k] : lo[k];
                hi[k] = glverts[base + i].pos[k] > hi[k] ? glverts[base + i].pos[k] : hi[k];
            }
        }
        fprintf(stderr, "[gx]   bbox eye=(%.1f %.1f %.1f)..(%.1f %.1f %.1f)\n", lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
        {
            /* the same in EFB pixels (vertices in front of the camera) */
            float slo[2] = { 1e30f, 1e30f }, shi[2] = { -1e30f, -1e30f };
            int any = 0;
            for (i = 0; i < nverts; i++) {
                const float* pp = glverts[i].pos;
                float cx = gx.proj[0][0] * pp[0] + gx.proj[0][1] * pp[1] + gx.proj[0][2] * pp[2] + gx.proj[0][3];
                float cy = gx.proj[1][0] * pp[0] + gx.proj[1][1] * pp[1] + gx.proj[1][2] * pp[2] + gx.proj[1][3];
                float cw = gx.proj[3][0] * pp[0] + gx.proj[3][1] * pp[1] + gx.proj[3][2] * pp[2] + gx.proj[3][3];
                if (cw > 0.0f) {
                    float x = gx.vp[0] + (cx / cw * 0.5f + 0.5f) * gx.vp[2];
                    float y = gx.vp[1] + (0.5f - cy / cw * 0.5f) * gx.vp[3];
                    slo[0] = x < slo[0] ? x : slo[0];
                    shi[0] = x > shi[0] ? x : shi[0];
                    slo[1] = y < slo[1] ? y : slo[1];
                    shi[1] = y > shi[1] ? y : shi[1];
                    any = 1;
                }
            }
            if (any) {
                fprintf(stderr, "[gx]   screen=(%.0f %.0f)..(%.0f %.0f)\n", slo[0], slo[1], shi[0], shi[1]);
            }
        }
        if (gx.vcd[GX_VA_PNMTXIDX] != GX_NONE) {
            /* per-vertex position matrices: which slots, and their contents */
            u32 used = 0;
            const u8* q = stream;
            for (i = 0; i < nverts; i++) {
                Vertex v;
                q = decode_vertex(q, vat, big, &v);
                if (v.pnmtx / 3 < 32) {
                    used |= 1u << (v.pnmtx / 3);
                }
            }
            for (k = 0; k < 32; k++) {
                if (used & (1u << k)) {
                    fprintf(stderr, "[gx]   pnmtx %d: [%.2f %.2f %.2f %.2f] [%.2f %.2f %.2f %.2f] [%.2f %.2f %.2f %.2f]\n", k * 3,
                            gx.mtx[k * 3][0], gx.mtx[k * 3][1], gx.mtx[k * 3][2], gx.mtx[k * 3][3], gx.mtx[k * 3 + 1][0],
                            gx.mtx[k * 3 + 1][1], gx.mtx[k * 3 + 1][2], gx.mtx[k * 3 + 1][3], gx.mtx[k * 3 + 2][0],
                            gx.mtx[k * 3 + 2][1], gx.mtx[k * 3 + 2][2], gx.mtx[k * 3 + 2][3]);
                }
            }
        }
    }
    if (pc_debug_in_fighter) {
        fighter_draws++;
    }
    if ((debug_log && stats_draws < 8) || (debug_log_frame != 0 && pc_frame_count == debug_log_frame)) {
        const GLVertex* g = &glverts[base];
        if (pc_debug_in_item) {
            fprintf(stderr, "[gx] (item kind %d rendermode %08x amb %02x%02x%02x dif %02x%02x%02x) ", pc_debug_in_item - 1,
                    pc_debug_rendermode, pc_debug_mat_colors[0], pc_debug_mat_colors[1], pc_debug_mat_colors[2],
                    pc_debug_mat_colors[4], pc_debug_mat_colors[5], pc_debug_mat_colors[6]);
        }
        if (pc_debug_in_fighter) {
            fprintf(stderr, "[gx] (fighter kind %d) ", pc_debug_in_fighter - 1);
        }
        float x = g->pos[0], y = g->pos[1], z = g->pos[2];
        float cx = gx.proj[0][0] * x + gx.proj[0][1] * y + gx.proj[0][2] * z + gx.proj[0][3];
        float cy = gx.proj[1][0] * x + gx.proj[1][1] * y + gx.proj[1][2] * z + gx.proj[1][3];
        float cz = gx.proj[2][0] * x + gx.proj[2][1] * y + gx.proj[2][2] * z + gx.proj[2][3];
        float cw = gx.proj[3][0] * x + gx.proj[3][1] * y + gx.proj[3][2] * z + gx.proj[3][3];
        fprintf(stderr,
                "[gx] draw prim=%02x vat=%u n=%u big=%d v0=(%.2f %.2f %.2f) clip=(%.2f %.2f %.2f w=%.2f) "
                "col0=(%.2f %.2f %.2f %.2f) tex0=(%.2f %.2f) tev=%u map0=%p fmt=%u %ux%u tlut=%u/%p/%u/%u chans=%u texgen=%u "
                "vp=(%.0f %.0f %.0f %.0f) cull=%u z=%u/%u/%u blend=%u(%u,%u) alpha=%u/%u/%u/%u proj=%u\n",
                prim, vat, nverts, big, x, y, z, cx, cy, cz, cw, g->col[0][0], g->col[0][1],
                g->col[0][2], g->col[0][3], g->tex[0][0], g->tex[0][1], gx.num_tev, gx.texmap[0].image,
                gx.texmap[0].format, gx.texmap[0].width, gx.texmap[0].height, gx.texmap[0].tlut_name,
                gx.texmap[0].tlut_name < 20 ? gx.tlut[gx.texmap[0].tlut_name].lut : NULL,
                gx.texmap[0].tlut_name < 20 ? gx.tlut[gx.texmap[0].tlut_name].fmt : 0,
                gx.texmap[0].tlut_name < 20 ? gx.tlut[gx.texmap[0].tlut_name].n_entries : 0, gx.num_chans, gx.num_texgen, gx.vp[0], gx.vp[1], gx.vp[2], gx.vp[3], gx.cull,
                gx.z_enable, gx.z_func, gx.z_update, gx.blend_mode, gx.blend_src, gx.blend_dst, gx.alpha_comp0, gx.alpha_ref0,
                gx.alpha_op, gx.alpha_comp1, gx.proj_type);
        {
            u32 st;
            for (st = 0; st < gx.num_tev && st < 16; st++) {
                const TevStage* t = &gx.tev[st];
                fprintf(stderr,
                        "[gx]   tev%u: c=(%u %u %u %u) op=%u bias=%u scale=%u reg=%u kc=%u | a=(%u %u %u %u) op=%u "
                        "reg=%u ka=%u | coord=%u map=%u chan=%u\n",
                        st, t->ca[0], t->ca[1], t->ca[2], t->ca[3], t->cop, t->cbias, t->cscale, t->creg, t->kcsel,
                        t->aa[0], t->aa[1], t->aa[2], t->aa[3], t->aop, t->areg, t->kasel, t->coord, t->map,
                        t->chan);
            }
            fprintf(stderr, "[gx]   kcolor0=(%.2f %.2f %.2f %.2f) kcolor1=(%.2f %.2f %.2f %.2f) tevreg0=(%.2f %.2f %.2f "
                            "%.2f) tevreg1=(%.2f %.2f %.2f %.2f)\n",
                    gx.kcolor[0][0], gx.kcolor[0][1], gx.kcolor[0][2], gx.kcolor[0][3], gx.kcolor[1][0],
                    gx.kcolor[1][1], gx.kcolor[1][2], gx.kcolor[1][3], gx.tev_reg[0][0], gx.tev_reg[0][1],
                    gx.tev_reg[0][2], gx.tev_reg[0][3], gx.tev_reg[1][0], gx.tev_reg[1][1], gx.tev_reg[1][2],
                    gx.tev_reg[1][3]);
        }
        if (gx.num_chans > 0) {
            const ChanCtrl* c = &gx.chan[0];
            const Light* l = &gx.lights[0];
            fprintf(stderr,
                    "[gx]   chan0: en=%u amb_src=%u mat_src=%u mask=%02x diff=%u attn=%u amb=(%.2f %.2f %.2f) "
                    "mat=(%.2f %.2f %.2f) light0 pos=(%.1f %.1f %.1f) dir=(%.2f %.2f %.2f) col=(%.2f %.2f %.2f) "
                    "light1 pos=(%.1f %.1f %.1f) col=(%.2f %.2f %.2f) "
                    "v0 nrm=(%.2f %.2f %.2f) vcol=(%.2f %.2f %.2f) lit=(%.2f %.2f %.2f)\n",
                    c->enable, c->amb_src, c->mat_src, c->light_mask, c->diff_fn, c->attn_fn, gx.amb_color[0][0],
                    gx.amb_color[0][1], gx.amb_color[0][2], gx.mat_color[0][0], gx.mat_color[0][1],
                    gx.mat_color[0][2], l->pos[0], l->pos[1], l->pos[2], l->dir[0], l->dir[1], l->dir[2],
                    l->color[0], l->color[1], l->color[2], gx.lights[1].pos[0], gx.lights[1].pos[1],
                    gx.lights[1].pos[2], gx.lights[1].color[0], gx.lights[1].color[1], gx.lights[1].color[2],
                    first_vertex.nrm[0], first_vertex.nrm[1], first_vertex.nrm[2],
                    first_vertex.col[0][0], first_vertex.col[0][1], first_vertex.col[0][2], g->col[0][0], g->col[0][1],
                    g->col[0][2]);
            if (first_vertex.bnm[0] != 0.0f || first_vertex.bnm[1] != 0.0f || first_vertex.tan[0] != 0.0f) {
                fprintf(stderr, "[gx]   v0 nbt: b=(%.3f %.3f %.3f) t=(%.3f %.3f %.3f) bump uv=(%.3f %.3f)\n",
                        first_vertex.bnm[0], first_vertex.bnm[1], first_vertex.bnm[2], first_vertex.tan[0],
                        first_vertex.tan[1], first_vertex.tan[2], g->tex[2][0], g->tex[2][1]);
            }
        }
        if (gx.num_chans > 1) {
            const ChanCtrl* c = &gx.chan[1];
            u32 li;
            fprintf(stderr, "[gx]   chan1: en=%u amb_src=%u mat_src=%u mask=%02x diff=%u attn=%u amb=(%.2f %.2f %.2f) "
                            "mat=(%.2f %.2f %.2f) lit1=(%.2f %.2f %.2f)",
                    c->enable, c->amb_src, c->mat_src, c->light_mask, c->diff_fn, c->attn_fn, gx.amb_color[1][0],
                    gx.amb_color[1][1], gx.amb_color[1][2], gx.mat_color[1][0], gx.mat_color[1][1],
                    gx.mat_color[1][2], g->col[1][0], g->col[1][1], g->col[1][2]);
            for (li = 0; li < 8; li++) {
                const Light* l = &gx.lights[li];
                if (c->light_mask & (1u << li)) {
                    fprintf(stderr, " L%u pos=(%.1f %.1f %.1f) dir=(%.2f %.2f %.2f) col=(%.2f %.2f %.2f) a=(%g %g %g) k=(%g %g %g)",
                            li, l->pos[0], l->pos[1], l->pos[2], l->dir[0], l->dir[1], l->dir[2], l->color[0],
                            l->color[1], l->color[2], l->a0, l->a1, l->a2, l->k0, l->k1, l->k2);
                }
            }
            fprintf(stderr, "\n");
        }
        for (u32 tgi = 0; tgi < gx.num_texgen && tgi < 8; tgi++) {
            const TexGen* tg = &gx.texgen[tgi];
            fprintf(stderr, "[gx]   texgen%u: src=%u type=%u mtx=%u pt=%u", tgi, tg->src, tg->type, tg->mtx, tg->pt_mtx);
            if (tg->mtx < MTX_ROWS - 2) {
                const float(*m)[4] = (const float(*)[4]) gx.mtx[tg->mtx];
                fprintf(stderr, " m=[%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]", m[0][0], m[0][1], m[0][2], m[0][3],
                        m[1][0], m[1][1], m[1][2], m[1][3]);
            }
            if (tg->pt_mtx != GX_PTIDENTITY && tg->pt_mtx + 3 <= MTX_ROWS) {
                const float(*m)[4] = (const float(*)[4]) gx.mtx[tg->pt_mtx];
                fprintf(stderr, " pt=[%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]", m[0][0], m[0][1], m[0][2], m[0][3],
                        m[1][0], m[1][1], m[1][2], m[1][3]);
            }
            fprintf(stderr, " v0tex=(%.2f %.2f)\n", glverts[0].tex[tgi][0], glverts[0].tex[tgi][1]);
        }
        {
            u32 a;
            fprintf(stderr, "[gx]   vcd:");
            for (a = 0; a < NUM_ATTR; a++) {
                if (gx.vcd[a] != GX_NONE) {
                    const VtxAttrFmt* f = &gx.vat[vat][a];
                    fprintf(stderr, " %u=%u(cnt%u,type%u,frac%u)", a, gx.vcd[a], f->cnt, f->type, f->frac);
                }
            }
            fprintf(stderr, " vsize=%u imm_len=%u expected=%u bytes0=%02x%02x%02x%02x %02x%02x%02x%02x\n",
                    vertex_stream_size(vat), imm_len, imm_expected, stream[0], stream[1], stream[2],
                    stream[3], stream[4], stream[5], stream[6], stream[7]);
            fprintf(stderr, "[gx]   curmtx=%u rows: [%.2f %.2f %.2f %.2f] [%.2f %.2f %.2f %.2f] [%.2f %.2f %.2f %.2f]\n",
                    gx.current_mtx, gx.mtx[gx.current_mtx][0], gx.mtx[gx.current_mtx][1],
                    gx.mtx[gx.current_mtx][2], gx.mtx[gx.current_mtx][3], gx.mtx[gx.current_mtx + 1][0],
                    gx.mtx[gx.current_mtx + 1][1], gx.mtx[gx.current_mtx + 1][2], gx.mtx[gx.current_mtx + 1][3],
                    gx.mtx[gx.current_mtx + 2][0], gx.mtx[gx.current_mtx + 2][1], gx.mtx[gx.current_mtx + 2][2],
                    gx.mtx[gx.current_mtx + 2][3]);
        }
    }

    if (batch_idx == 0) {
        batch_min = base;
    }
    switch (prim) {
    case GX_QUADS: {
        u32 q;
        for (q = 0; q + 3 < nverts; q += 4) {
            indices[batch_idx++] = (u16) (base + q);
            indices[batch_idx++] = (u16) (base + q + 1);
            indices[batch_idx++] = (u16) (base + q + 2);
            indices[batch_idx++] = (u16) (base + q);
            indices[batch_idx++] = (u16) (base + q + 2);
            indices[batch_idx++] = (u16) (base + q + 3);
        }
        break;
    }
    case GX_TRIANGLES:
        for (i = 0; i + 2 < nverts; i += 3) {
            indices[batch_idx++] = (u16) (base + i);
            indices[batch_idx++] = (u16) (base + i + 1);
            indices[batch_idx++] = (u16) (base + i + 2);
        }
        break;
    case GX_TRIANGLESTRIP:
        for (i = 2; i < nverts; i++) {
            /* alternate the first two so every triangle keeps the strip's winding */
            indices[batch_idx++] = (u16) (base + i - ((i & 1) ? 1 : 2));
            indices[batch_idx++] = (u16) (base + i - ((i & 1) ? 2 : 1));
            indices[batch_idx++] = (u16) (base + i);
        }
        break;
    case GX_TRIANGLEFAN:
        for (i = 2; i < nverts; i++) {
            indices[batch_idx++] = (u16) base;
            indices[batch_idx++] = (u16) (base + i - 1);
            indices[batch_idx++] = (u16) (base + i);
        }
        break;
    default:
        switch (prim) {
        case GX_LINES: mode = GL_LINES; break;
        case GX_LINESTRIP: mode = GL_LINE_STRIP; break;
        case GX_POINTS: mode = GL_POINTS; break;
        default: mode = 0; break;
        }
        flush_batch();
        if (mode != 0) {
            glDrawArrays(mode, (GLint) (use_ring > 0 ? ring_base_v + base : base), (GLsizei) nverts);
            stats_gl_draws++;
        }
        if (use_ring > 0) {
            ring_pos_v = ring_base_v + base + nverts; /* keep them until the region comes round */
            ring_base_v = ring_pos_v;
        }
        base = 0;
        nverts = 0; /* nothing of it stays in the batch */
        break;
    }
    batch_verts = base + nverts;
    stats_draws++;
    stats_verts += nverts;
}

static u32 vertex_stream_size(u8 vat)
{
    u32 attr, size = 0;
    for (attr = 0; attr < NUM_ATTR; attr++) {
        size += attr_stream_size((u8) attr, gx.vcd[attr], &gx.vat[vat][attr]);
    }
    return size;
}

static void imm_flush(void)
{
    if (imm_active && imm_vsize > 0) {
        u32 n = imm_len / imm_vsize;
        if (n > imm_nverts) {
            n = imm_nverts;
        }
        draw_stream(imm_prim, imm_vat, imm_buf, n, 0);
    }
    imm_active = 0;
    imm_len = 0;
}

/* --- Immediate-mode writes ---------------------------------------------- */

static void imm_put(const void* p, u32 n)
{
    if (!imm_active || imm_len + n > sizeof(imm_buf)) {
        return;
    }
    memcpy(imm_buf + imm_len, p, n);
    imm_len += n;
    if (imm_len >= imm_expected) {
        imm_flush();
    }
}

void pc_gx_write_u8(u8 v) { imm_put(&v, 1); }
void pc_gx_write_u16(u16 v) { imm_put(&v, 2); }
void pc_gx_write_u32(u32 v)
{
    /* GXColor1u32 packs 0xRRGGBBAA: store as r,g,b,a bytes */
    u8 b[4] = { (u8) (v >> 24), (u8) (v >> 16), (u8) (v >> 8), (u8) v };
    imm_put(b, 4);
}
void pc_gx_write_s8(s8 v) { imm_put(&v, 1); }
void pc_gx_write_s16(s16 v) { imm_put(&v, 2); }
void pc_gx_write_s32(s32 v) { imm_put(&v, 4); }
void pc_gx_write_f32(f32 v) { imm_put(&v, 4); }

void GXBegin(GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts)
{
    imm_flush();
    imm_prim = (u8) type;
    imm_vat = (u8) vtxfmt & 7;
    imm_nverts = nverts;
    imm_vsize = vertex_stream_size(imm_vat);
    imm_expected = imm_vsize * nverts;
    imm_len = 0;
    imm_active = nverts > 0 && imm_vsize > 0;
}

/* --- Display lists ---------------------------------------------------------- */

void GXCallDisplayList(void* list, u32 nbytes)
{
    if (pc_debug_in_fighter) {
        pc_debug_fighter_counts[3]++;
    }
    const u8* p = (const u8*) list;
    const u8* end = p + nbytes;
    static int logged;
    imm_flush();
    if (debug_log && logged < 6) {
        u32 a;
        logged++;
        fprintf(stderr, "[gx] display list %p bytes=%u head=%02x%02x%02x%02x vcd:", list, nbytes, p[0],
                p[1], p[2], p[3]);
        for (a = 0; a < NUM_ATTR; a++) {
            if (gx.vcd[a] != GX_NONE) {
                fprintf(stderr, " %u=%u(%u,%u,%u)", a, gx.vcd[a], gx.vat[p[0] & 7][a].cnt,
                        gx.vat[p[0] & 7][a].type, gx.vat[p[0] & 7][a].frac);
            }
        }
        fprintf(stderr, " vsize=%u\n", vertex_stream_size(p[0] & 7));
    }
    Shader* sh = NULL;
    int setup_done = 0;
    double t0 = prof_now();
    while (p + 3 <= end) {
        u8 op = *p;
        u8 prim = op & 0xF8, vat = op & 7;
        u32 n, vsize;
        if (op == 0) { /* NOP padding */
            p++;
            continue;
        }
        if (prim < GX_QUADS || prim > GX_POINTS) {
            if (debug_log) {
                fprintf(stderr, "[gx] display list: unexpected opcode %02x at offset %u\n", op,
                        (u32) (p - (const u8*) list));
            }
            break; /* not a primitive; register loads are not expected here */
        }
        n = be16(p + 1);
        p += 3;
        vsize = vertex_stream_size(vat);
        if (vsize == 0 || p + n * vsize > end) {
            if (debug_log) {
                fprintf(stderr, "[gx] display list: prim %02x n=%u vsize=%u overruns (%u left)\n", prim,
                        n, vsize, (u32) (end - p));
            }
            break;
        }
        if (!setup_done) {
            /* the state is set up once for every primitive of the list */
            setup_done = 1;
            if (!skip_shadow_pass()) {
                sh = draw_setup();
            }
        }
        if (sh != NULL) {
            draw_prims(sh, prim, vat, p, n, 1);
        }
        p += n * vsize;
    }
    prof_ms[1] += prof_now() - t0;
}

/* --- Vertex descriptors and arrays ------------------------------------- */

void GXClearVtxDesc(void)
{
    memset(gx.vcd, GX_NONE, sizeof(gx.vcd));
}

/* GX_VA_NBT is the normal attribute with nine components (normal, binormal,
 * tangent): it shares the normal's descriptor, format and array, and sits
 * between the position and the colours in the vertex stream */
static GXAttr attr_alias(GXAttr attr)
{
    return attr == GX_VA_NBT ? GX_VA_NRM : attr;
}

void GXSetVtxDesc(GXAttr attr, GXAttrType type)
{
    attr = attr_alias(attr);
    if ((u32) attr < NUM_ATTR) {
        gx.vcd[attr] = (u8) type;
    }
}

void GXSetVtxAttrFmt(GXVtxFmt vtxfmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac)
{
    attr = attr_alias(attr);
    if ((u32) vtxfmt < 8 && (u32) attr < NUM_ATTR) {
        VtxAttrFmt* f = &gx.vat[vtxfmt][attr];
        f->cnt = (u8) cnt;
        f->type = (u8) type;
        f->frac = frac;
    }
}

void GXSetArray(GXAttr attr, const void* base_ptr, u8 stride)
{
    attr = attr_alias(attr);
    if ((u32) attr < NUM_ATTR) {
        gx.array_base[attr] = (const u8*) base_ptr;
        gx.array_stride[attr] = stride;
    }
}

void GXInvalidateVtxCache(void) {}

/* --- Matrices ------------------------------------------------------------------ */

void GXLoadPosMtxImm(f32 mtx[3][4], u32 id)
{
    static int logged;
    if (id + 2 < MTX_ROWS) {
        memcpy(gx.mtx[id], mtx, 3 * 4 * sizeof(float));
        blk_dirty[id < 30 ? 0 : 1] = 1;
    }
    if (debug_log && logged < 3) {
        logged++;
        fprintf(stderr, "[gx] GXLoadPosMtxImm id=%u [%.2f %.2f %.2f %.2f] [%.2f %.2f %.2f %.2f] [%.2f %.2f %.2f %.2f]\n",
                id, mtx[0][0], mtx[0][1], mtx[0][2], mtx[0][3], mtx[1][0], mtx[1][1], mtx[1][2], mtx[1][3],
                mtx[2][0], mtx[2][1], mtx[2][2], mtx[2][3]);
        pc_print_backtrace();
    }
}

void GXLoadNrmMtxImm(f32 mtx[3][4], u32 id)
{
    u32 slot = (id / 3) & 31;
    int i, j;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            gx.nrm[slot][i][j] = mtx[i][j];
        }
    }
    blk_dirty[slot < 10 ? 0 : 1] = 1;
}

void GXLoadTexMtxImm(f32 mtx[][4], u32 id, GXTexMtxType type)
{
    u32 rows = type == GX_MTX2x4 ? 2 : 3;
    if (id + rows <= MTX_ROWS) {
        memcpy(gx.mtx[id], mtx, rows * 4 * sizeof(float));
        blk_dirty[id < 30 ? 0 : 1] = 1;
    }
}

void GXSetCurrentMtx(u32 id)
{
    gx.current_mtx = id;
}

void GXSetProjection(f32 mtx[4][4], GXProjectionType type)
{
    memcpy(gx.proj, mtx, sizeof(gx.proj));
    gx.proj_type = (u8) type;
}

void GXGetProjectionv(f32* p)
{
    p[0] = (f32) gx.proj_type;
    if (gx.proj_type == GX_PERSPECTIVE) {
        p[1] = gx.proj[0][0];
        p[2] = gx.proj[0][2];
        p[3] = gx.proj[1][1];
        p[4] = gx.proj[1][2];
    } else {
        p[1] = gx.proj[0][0];
        p[2] = gx.proj[0][3];
        p[3] = gx.proj[1][1];
        p[4] = gx.proj[1][3];
    }
    p[5] = gx.proj[2][2];
    p[6] = gx.proj[2][3];
}

void GXSetViewport(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz)
{
    if (debug_log_frame != 0 && pc_frame_count == debug_log_frame) {
        fprintf(stderr, "[gx] GXSetViewport (%.0f %.0f %.0f %.0f)\n", left, top, wd, ht);
    }
    gx.vp[0] = left;
    gx.vp[1] = top;
    gx.vp[2] = wd;
    gx.vp[3] = ht;
    gx.vp[4] = nearz;
    gx.vp[5] = farz;
}

void GXSetViewportJitter(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz, u32 field)
{
    (void) field;
    GXSetViewport(left, top, wd, ht, nearz, farz);
}

void GXGetViewportv(f32* vp)
{
    memcpy(vp, gx.vp, sizeof(gx.vp));
}

void GXSetScissor(u32 left, u32 top, u32 wd, u32 ht)
{
    gx.scissor[0] = left;
    gx.scissor[1] = top;
    gx.scissor[2] = wd;
    gx.scissor[3] = ht;
}

void GXProject(f32 x, f32 y, f32 z, f32 mtx[3][4], f32* pm, f32* vp, f32* sx, f32* sy, f32* sz)
{
    float in[3] = { x, y, z }, v[3];
    float xc, yc, zc, wc, xn, yn, zn;
    mtx_mul_point(mtx, in, v);
    if (pm[0] == GX_PERSPECTIVE) {
        xc = pm[1] * v[0] + pm[2] * v[2];
        yc = pm[3] * v[1] + pm[4] * v[2];
        zc = pm[5] * v[2] + pm[6];
        wc = -v[2];
    } else {
        xc = pm[1] * v[0] + pm[2];
        yc = pm[3] * v[1] + pm[4];
        zc = pm[5] * v[2] + pm[6];
        wc = 1.0f;
    }
    if (wc == 0.0f) {
        wc = 1e-6f;
    }
    xn = xc / wc;
    yn = yc / wc;
    zn = zc / wc;
    *sx = vp[0] + vp[2] * (xn * 0.5f + 0.5f);
    *sy = vp[1] + vp[3] * (0.5f - yn * 0.5f);
    *sz = vp[4] + (vp[5] - vp[4]) * (zn + 1.0f);
}

/* --- Raster state ----------------------------------------------------------- */

void GXSetCullMode(GXCullMode mode)
{
    imm_flush();
    gx.cull = (u8) mode;
}

void GXSetZMode(GXBool compare_enable, GXCompare func, GXBool update_enable)
{
    gx.z_enable = compare_enable;
    gx.z_func = (u8) func;
    gx.z_update = update_enable;
}

void GXSetZCompLoc(GXBool before_tex)
{
    (void) before_tex;
}

void GXSetBlendMode(GXBlendMode type, GXBlendFactor src_factor, GXBlendFactor dst_factor, GXLogicOp op)
{
    gx.blend_mode = (u8) type;
    gx.blend_src = (u8) src_factor;
    gx.blend_dst = (u8) dst_factor;
    gx.logic_op = (u8) op;
}

void GXSetColorUpdate(GXBool update_enable)
{
    gx.color_update = update_enable;
}

void GXSetAlphaUpdate(GXBool update_enable)
{
    gx.alpha_update = update_enable;
}

void GXSetAlphaCompare(GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1, u8 ref1)
{
    key_dirty = 1;
    gx.alpha_comp0 = (u8) comp0;
    gx.alpha_ref0 = ref0;
    gx.alpha_op = (u8) op;
    gx.alpha_comp1 = (u8) comp1;
    gx.alpha_ref1 = ref1;
}

void GXSetDstAlpha(GXBool enable, u8 alpha)
{
    (void) enable;
    (void) alpha;
}

void GXSetDither(GXBool dither)
{
    (void) dither;
}

void GXSetLineWidth(u8 width, GXTexOffset texOffsets)
{
    (void) texOffsets;
    if (rendering) {
        glLineWidth(width / 6.0f > 0.0f ? width / 6.0f : 1.0f);
    }
}

void GXSetPointSize(u8 pointSize, GXTexOffset texOffsets)
{
    (void) texOffsets;
    if (rendering) {
        glPointSize(pointSize / 6.0f > 0.0f ? pointSize / 6.0f : 1.0f);
    }
}

void GXSetCopyClear(GXColor clear_clr, u32 clear_z)
{
    gx.clear_color = clear_clr;
    gx.clear_z = clear_z;
}

void GXSetFog(GXFogType type, f32 startz, f32 endz, f32 nearz, f32 farz, GXColor color)
{
    key_dirty = 1;
    gx.fog_type = (u8) type;
    gx.fog_start = startz;
    gx.fog_end = endz;
    gx.fog_near = nearz;
    gx.fog_far = farz;
    gx.fog_color[0] = color.r / 255.0f;
    gx.fog_color[1] = color.g / 255.0f;
    gx.fog_color[2] = color.b / 255.0f;
    gx.fog_color[3] = color.a / 255.0f;
}

/* --- TEV ------------------------------------------------------------------------ */

void GXSetNumTevStages(u8 nStages)
{
    key_dirty = 1;
    imm_flush();
    gx.num_tev = nStages > 16 ? 16 : nStages;
}

void GXSetTevOrder(GXTevStageID stage, GXTexCoordID coord, GXTexMapID map, GXChannelID color)
{
    key_dirty = 1;
    TevStage* s = &gx.tev[stage & 15];
    s->coord = (u8) coord;
    s->map = (u8) (map & 0xFF);
    s->chan = (u8) color;
}

void GXSetTevColorIn(GXTevStageID stage, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c, GXTevColorArg d)
{
    key_dirty = 1;
    TevStage* s = &gx.tev[stage & 15];
    s->ca[0] = (u8) a;
    s->ca[1] = (u8) b;
    s->ca[2] = (u8) c;
    s->ca[3] = (u8) d;
}

void GXSetTevAlphaIn(GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d)
{
    key_dirty = 1;
    TevStage* s = &gx.tev[stage & 15];
    s->aa[0] = (u8) a;
    s->aa[1] = (u8) b;
    s->aa[2] = (u8) c;
    s->aa[3] = (u8) d;
}

void GXSetTevColorOp(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID out_reg)
{
    key_dirty = 1;
    TevStage* s = &gx.tev[stage & 15];
    s->cop = (u8) op;
    s->cbias = (u8) bias;
    s->cscale = (u8) scale;
    s->cclamp = clamp;
    s->creg = (u8) out_reg;
}

void GXSetTevAlphaOp(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID out_reg)
{
    key_dirty = 1;
    TevStage* s = &gx.tev[stage & 15];
    s->aop = (u8) op;
    s->abias = (u8) bias;
    s->ascale = (u8) scale;
    s->aclamp = clamp;
    s->areg = (u8) out_reg;
}

void GXSetTevOp(GXTevStageID id, GXTevMode mode)
{
    GXTevColorArg cc = GX_CC_RASC;
    GXTevAlphaArg ca = GX_CA_RASA;
    if (id != GX_TEVSTAGE0) {
        cc = GX_CC_CPREV;
        ca = GX_CA_APREV;
    }
    switch (mode) {
    case GX_MODULATE:
        GXSetTevColorIn(id, GX_CC_ZERO, GX_CC_TEXC, cc, GX_CC_ZERO);
        GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_TEXA, ca, GX_CA_ZERO);
        break;
    case GX_DECAL:
        GXSetTevColorIn(id, cc, GX_CC_TEXC, GX_CC_TEXA, GX_CC_ZERO);
        GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, ca);
        break;
    case GX_BLEND:
        GXSetTevColorIn(id, cc, GX_CC_ONE, GX_CC_TEXC, GX_CC_ZERO);
        GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_TEXA, ca, GX_CA_ZERO);
        break;
    case GX_REPLACE:
        GXSetTevColorIn(id, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
        GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
        break;
    default: /* GX_PASSCLR */
        GXSetTevColorIn(id, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, cc);
        GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, ca);
        break;
    }
    GXSetTevColorOp(id, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaOp(id, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}

void GXSetTevColor(GXTevRegID id, GXColor color)
{
    float* r = gx.tev_reg[id & 3];
    r[0] = color.r / 255.0f;
    r[1] = color.g / 255.0f;
    r[2] = color.b / 255.0f;
    r[3] = color.a / 255.0f;
}

void GXSetTevColorS10(GXTevRegID id, GXColorS10 color)
{
    float* r = gx.tev_reg[id & 3];
    r[0] = color.r / 255.0f;
    r[1] = color.g / 255.0f;
    r[2] = color.b / 255.0f;
    r[3] = color.a / 255.0f;
}

void GXSetTevKColor(GXTevKColorID id, GXColor color)
{
    float* r = gx.kcolor[id & 3];
    if (debug_log_frame != 0 && pc_frame_count == debug_log_frame) {
        fprintf(stderr, "[gx] GXSetTevKColor %u = %02x%02x%02x%02x\n", (unsigned) id, color.r, color.g, color.b, color.a);
    }
    r[0] = color.r / 255.0f;
    r[1] = color.g / 255.0f;
    r[2] = color.b / 255.0f;
    r[3] = color.a / 255.0f;
}

void GXSetTevKColorSel(GXTevStageID stage, GXTevKColorSel sel)
{
    key_dirty = 1;
    gx.tev[stage & 15].kcsel = (u8) sel;
}

void GXSetTevKAlphaSel(GXTevStageID stage, GXTevKAlphaSel sel)
{
    key_dirty = 1;
    gx.tev[stage & 15].kasel = (u8) sel;
}

void GXSetTevSwapMode(GXTevStageID stage, GXTevSwapSel ras_sel, GXTevSwapSel tex_sel)
{
    key_dirty = 1;
    gx.tev[stage & 15].ras_swap = (u8) ras_sel;
    gx.tev[stage & 15].tex_swap = (u8) tex_sel;
}

void GXSetTevSwapModeTable(GXTevSwapSel table, GXTevColorChan red, GXTevColorChan green, GXTevColorChan blue, GXTevColorChan alpha)
{
    key_dirty = 1;
    gx.swap_table[table & 3][0] = (u8) red;
    gx.swap_table[table & 3][1] = (u8) green;
    gx.swap_table[table & 3][2] = (u8) blue;
    gx.swap_table[table & 3][3] = (u8) alpha;
}

void GXSetTevClampMode(int stage, int mode)
{
    (void) stage;
    (void) mode;
}

void GXSetTevDirect(GXTevStageID tev_stage)
{
    key_dirty = 1;
    TevStage* t = &gx.tev[tev_stage & 15];
    t->ind_on = 0;
    t->ind_stage = t->ind_fmt = t->ind_bias = t->ind_mtx = t->ind_addprev = 0;
    t->ind_coord = t->ind_map = 0xFF;
}

void GXSetTevIndirect(GXTevStageID tev_stage, GXIndTexStageID ind_stage, GXIndTexFormat format, GXIndTexBiasSel bias_sel, GXIndTexMtxID matrix_sel, GXIndTexWrap wrap_s, GXIndTexWrap wrap_t, GXBool add_prev, GXBool utc_lod, GXIndTexAlphaSel alpha_sel)
{
    key_dirty = 1;
    TevStage* t = &gx.tev[tev_stage & 15];
    (void) wrap_s; (void) wrap_t; (void) utc_lod; (void) alpha_sel;
    t->ind_on = 1;
    t->ind_stage = (u8) (ind_stage & 3);
    t->ind_fmt = (u8) format;
    t->ind_bias = (u8) bias_sel;
    /* only the static matrices; the dynamic S/T ones fall back to none */
    t->ind_mtx = matrix_sel >= GX_ITM_0 && matrix_sel <= GX_ITM_2 ? (u8) matrix_sel : 0;
    t->ind_addprev = (u8) (add_prev != 0);
    t->ind_coord = t->ind_map = 0xFF;
}

void GXSetNumIndStages(u8 n) { (void) n; }

void GXSetIndTexOrder(GXIndTexStageID s, GXTexCoordID c, GXTexMapID m)
{
    key_dirty = 1;
    gx.ind_order[s & 3].coord = (u8) c;
    gx.ind_order[s & 3].map = (u8) m;
}

void GXSetIndTexCoordScale(GXIndTexStageID s, GXIndTexScale a, GXIndTexScale b) { (void) s; (void) a; (void) b; }

void GXSetIndTexMtx(GXIndTexMtxID id, f32 offset[2][3], s8 scale_exp)
{
    float k = scale_exp >= 0 ? (float) (1 << scale_exp) : 1.0f / (float) (1 << -scale_exp);
    int r, c;
    if (id < GX_ITM_0 || id > GX_ITM_2) {
        return;
    }
    for (r = 0; r < 2; r++) {
        for (c = 0; c < 3; c++) {
            gx.ind_mtx[id][r][c] = offset[r][c] * k;
        }
    }
}

/* --- Texture coordinate generation and channels ------------------------- */

void GXSetNumTexGens(u8 nTexGens)
{
    key_dirty = 1;
    gx.num_texgen = nTexGens > 8 ? 8 : nTexGens;
}

void GXSetTexCoordGen2(GXTexCoordID dst_coord, GXTexGenType func, GXTexGenSrc src_param, u32 mtx, GXBool normalize, u32 pt_texmtx)
{
    key_dirty = 1;
    TexGen* g = &gx.texgen[dst_coord & 7];
    g->type = (u8) func;
    g->src = (u8) src_param;
    g->mtx = mtx;
    g->normalize = normalize;
    g->pt_mtx = pt_texmtx;
}

void GXSetNumChans(u8 nChans)
{
    key_dirty = 1;
    gx.num_chans = nChans > 2 ? 2 : nChans;
}

void GXSetChanCtrl(GXChannelID chan, GXBool enable, GXColorSrc amb_src, GXColorSrc mat_src, u32 light_mask, GXDiffuseFn diff_fn, GXAttnFn attn_fn)
{
    key_dirty = 1;
    int first = 0, last = 0, i;
    switch (chan) {
    case GX_COLOR0: first = last = 0; break;
    case GX_COLOR1: first = last = 1; break;
    case GX_ALPHA0: first = last = 2; break;
    case GX_ALPHA1: first = last = 3; break;
    case GX_COLOR0A0: first = 0; last = 2; break;
    case GX_COLOR1A1: first = 1; last = 3; break;
    default: return;
    }
    for (i = first; i <= last; i += 2) {
        ChanCtrl* c = &gx.chan[i];
        c->enable = enable;
        c->amb_src = (u8) amb_src;
        c->mat_src = (u8) mat_src;
        c->light_mask = light_mask;
        c->diff_fn = (u8) diff_fn;
        c->attn_fn = (u8) attn_fn;
    }
}

static void set_color4(float* dst, GXColor c)
{
    dst[0] = c.r / 255.0f;
    dst[1] = c.g / 255.0f;
    dst[2] = c.b / 255.0f;
    dst[3] = c.a / 255.0f;
}

void GXSetChanAmbColor(GXChannelID chan, GXColor amb_color)
{
    blk_dirty[3] = 1;
    if (chan == GX_COLOR0 || chan == GX_COLOR0A0) set_color4(gx.amb_color[0], amb_color);
    if (chan == GX_COLOR1 || chan == GX_COLOR1A1) set_color4(gx.amb_color[1], amb_color);
    if (chan == GX_ALPHA0) gx.amb_color[0][3] = amb_color.a / 255.0f;
    if (chan == GX_ALPHA1) gx.amb_color[1][3] = amb_color.a / 255.0f;
}

void GXSetChanMatColor(GXChannelID chan, GXColor mat_color)
{
    blk_dirty[3] = 1;
    if (chan == GX_COLOR0 || chan == GX_COLOR0A0) set_color4(gx.mat_color[0], mat_color);
    if (chan == GX_COLOR1 || chan == GX_COLOR1A1) set_color4(gx.mat_color[1], mat_color);
    if (chan == GX_ALPHA0) gx.mat_color[0][3] = mat_color.a / 255.0f;
    if (chan == GX_ALPHA1) gx.mat_color[1][3] = mat_color.a / 255.0f;
}

/* --- Lights ------------------------------------------------------------------ */

typedef struct PCLightObj {
    Light l;
} PCLightObj;

STATIC_ASSERT(sizeof(PCLightObj) <= sizeof(GXLightObj));

void GXInitLightAttn(GXLightObj* lt_obj, f32 a0, f32 a1, f32 a2, f32 k0, f32 k1, f32 k2)
{
    Light* l = &((PCLightObj*) lt_obj)->l;
    l->a0 = a0; l->a1 = a1; l->a2 = a2; l->k0 = k0; l->k1 = k1; l->k2 = k2;
}

void GXInitLightSpot(GXLightObj* lt_obj, f32 cutoff, GXSpotFn spot_func)
{
    /* Approximation: a flat cone. */
    Light* l = &((PCLightObj*) lt_obj)->l;
    float r = cutoff * 3.14159265f / 180.0f, cr = cosf(r);
    (void) spot_func;
    if (cutoff <= 0.0f || cutoff > 90.0f) {
        l->a0 = 1.0f; l->a1 = 0.0f; l->a2 = 0.0f;
        return;
    }
    l->a0 = -cr / (1.0f - cr);
    l->a1 = 1.0f / (1.0f - cr);
    l->a2 = 0.0f;
}

void GXInitLightDistAttn(GXLightObj* lt_obj, f32 ref_dist, f32 ref_br, GXDistAttnFn dist_func)
{
    Light* l = &((PCLightObj*) lt_obj)->l;
    float k0 = 1.0f, k1 = 0.0f, k2 = 0.0f;
    if (ref_dist >= 0.0f && ref_br > 0.0f && ref_br < 1.0f) {
        switch (dist_func) {
        case GX_DA_GENTLE: k0 = 1.0f; k1 = (1.0f - ref_br) / (ref_br * ref_dist); k2 = 0.0f; break;
        case GX_DA_MEDIUM: k0 = 1.0f; k1 = 0.5f * (1.0f - ref_br) / (ref_br * ref_dist); k2 = 0.5f * (1.0f - ref_br) / (ref_br * ref_dist * ref_dist); break;
        case GX_DA_STEEP: k0 = 1.0f; k1 = 0.0f; k2 = (1.0f - ref_br) / (ref_br * ref_dist * ref_dist); break;
        default: break;
        }
    }
    l->k0 = k0; l->k1 = k1; l->k2 = k2;
}

void GXInitLightPos(GXLightObj* lt_obj, f32 x, f32 y, f32 z)
{
    Light* l = &((PCLightObj*) lt_obj)->l;
    l->pos[0] = x; l->pos[1] = y; l->pos[2] = z;
}

void GXInitLightDir(GXLightObj* lt_obj, f32 nx, f32 ny, f32 nz)
{
    Light* l = &((PCLightObj*) lt_obj)->l;
    l->dir[0] = nx; l->dir[1] = ny; l->dir[2] = nz;
}

void GXInitLightColor(GXLightObj* lt_obj, GXColor color)
{
    set_color4(((PCLightObj*) lt_obj)->l.color, color);
}

void GXLoadLightObjImm(GXLightObj* lt_obj, GXLightID light)
{
    int i;
    for (i = 0; i < 8; i++) {
        if (light & (1u << i)) {
            gx.lights[i] = ((PCLightObj*) lt_obj)->l;
        }
    }
    blk_dirty[2] = 1;
}

/* --- Textures and palettes --------------------------------------------------- */

void GXLoadTexObj(GXTexObj* obj, GXTexMapID id)
{
    key_dirty = 1;
    if ((u32) id < 8) {
        imm_flush();
        gx.texmap[id] = *(PCTexObj*) obj;
    }
}

void GXLoadTlut(GXTlutObj* tlut_obj, u32 tlut_name)
{
    if (tlut_name < 20) {
        gx.tlut[tlut_name] = *(PCTlutObj*) tlut_obj;
    }
}

void GXInvalidateTexAll(void) {}

/// Drops cached uploads of textures whose image lies in [addr, addr+bytes):
/// the game rewrites some textures in place (movie frames, EFB copies) and
/// announces it with a data-cache store or flush.
void pc_gx_texture_changed(const void* addr, u32 bytes)
{
    const u8* lo = (const u8*) addr;
    const u8* hi = lo + bytes;
    u32 i;
    for (i = 0; i < TEX_CACHE; i++) {
        const u8* img = (const u8*) tex_cache[i].image;
        if (tex_cache[i].tex != 0 && img >= lo && img < hi) {
            forget_texture(tex_cache[i].tex);
            glDeleteTextures(1, &tex_cache[i].tex);
            tex_bucket_remove(i);
            memset(&tex_cache[i], 0, sizeof(TexEntry));
        }
    }
}

void DCStoreRange(void* addr, u32 nBytes)
{
    pc_gx_texture_changed(addr, nBytes);
}

void DCFlushRange(void* addr, u32 nBytes)
{
    pc_gx_texture_changed(addr, nBytes);
}

/* --- Frame buffer copies --------------------------------------------------- */

static void do_clear(void)
{
    if (debug_log_frame != 0 && pc_frame_count == debug_log_frame) {
        fprintf(stderr, "[gx] clear color %02x%02x%02x%02x z %06x scissor=(%u %u %u %u)\n", gx.clear_color.r, gx.clear_color.g,
                gx.clear_color.b, gx.clear_color.a, gx.clear_z, gx.scissor[0], gx.scissor[1], gx.scissor[2], gx.scissor[3]);
    }
    flush_batch();
    rs.valid = 0;
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearColor(gx.clear_color.r / 255.0f, gx.clear_color.g / 255.0f, gx.clear_color.b / 255.0f,
                 gx.clear_color.a / 255.0f);
    glClearDepth(gx.clear_z == 0xFFFFFF ? 1.0 : gx.clear_z / 16777215.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

/* Dump the back buffer as a 24-bit BMP (bottom-up rows, which is what
 * glReadPixels produces). */
static void save_screenshot(void)
{
    int w, h, y;
    char path[1024];
    FILE* f;
    u8* pixels;
    u8 hdr[54];
    u32 row_bytes, size;
    if (scene_fbo != 0) {
        w = scene_w;
        h = scene_h;
    } else {
        pc_window_size(&w, &h);
    }
    row_bytes = ((u32) w * 3 + 3) & ~3u;
    size = row_bytes * (u32) h;
    pixels = (u8*) calloc(size, 1);
    if (pixels == NULL) {
        return;
    }
    flush_batch();
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    if (scene_fbo != 0) {
        pc_glBindFramebuffer(GL_READ_FRAMEBUFFER, scene_read_fbo(0, 0, w, h));
        glReadBuffer(GL_COLOR_ATTACHMENT0);
    } else {
        glReadBuffer(GL_BACK);
    }
    glReadPixels(0, 0, w, h, GL_BGR_EXT, GL_UNSIGNED_BYTE, pixels);
    if (scene_fbo != 0) {
        pc_glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo);
    }
    snprintf(path, sizeof(path), "%s/frame%05u.bmp", pc_config.screenshot_dir, frame_no);
    f = fopen(path, "wb");
    if (f == NULL) {
        free(pixels);
        return;
    }
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B';
    hdr[1] = 'M';
    *(u32*) (hdr + 2) = 54 + size;
    *(u32*) (hdr + 10) = 54;
    *(u32*) (hdr + 14) = 40;
    *(s32*) (hdr + 18) = w;
    *(s32*) (hdr + 22) = h;
    *(u16*) (hdr + 26) = 1;
    *(u16*) (hdr + 28) = 24;
    *(u32*) (hdr + 34) = size;
    fwrite(hdr, 1, sizeof(hdr), f);
    for (y = 0; y < h; y++) {
        fwrite(pixels + (size_t) y * row_bytes, 1, row_bytes, f);
    }
    fclose(f);
    free(pixels);
    fprintf(stderr,
            "[pc] wrote %s (%d draws, %d vertices, %d fighter draws; fighter jobj %d dobj %d pobj %d dl %d)\n",
            path, stats_draws, stats_verts, fighter_draws, pc_debug_fighter_counts[0],
            pc_debug_fighter_counts[1], pc_debug_fighter_counts[2], pc_debug_fighter_counts[3]);
}

void GXCopyDisp(void* dest, GXBool clear)
{
    (void) dest;
    if (!rendering) {
        return;
    }
    imm_flush();
    if (debug_log_frame != 0 && pc_frame_count == debug_log_frame) {
        fprintf(stderr, "[gx] GXCopyDisp clear=%d (present)\n", (int) clear);
    }
    if (pc_config.screenshot_dir != NULL && pc_config.screenshot_every > 0 && frame_no >= (u32) pc_config.screenshot_from &&
        (frame_no - (u32) pc_config.screenshot_from) % (u32) pc_config.screenshot_every == 0)
    {
        save_screenshot();
    }
    flush_batch();
    {
        static double frame_start;
        double t0 = prof_now(), t1;
        present_scene();
        pc_window_present();
        ensure_scene_target(); /* the window may have been resized */
        bind_scene();
        t1 = prof_now();
        prof_ms[5] += t1 - t0;
        if (prof_on < 0) {
            prof_on = getenv("MELEE_GX_PROFILE") != NULL;
        }
        if (frame_start != 0.0) {
            prof_ms[0] += t1 - frame_start;
        }
        frame_start = t1;
        if (prof_on && frame_no % 300 == 299) {
            fprintf(stderr,
                    "[gx] profile over %u frames: frame %.2f ms = draws %.2f (vertices %.2f, shader %.2f, texture %.2f) + "
                    "copy %.2f + present %.2f + rest %.2f; %u GL draws/frame (params %.2f, draw calls %.2f)\n",
                    300u, prof_ms[0] / 300, prof_ms[1] / 300, prof_ms[6] / 300, prof_ms[2] / 300, prof_ms[3] / 300,
                    prof_ms[4] / 300, prof_ms[5] / 300, (prof_ms[0] - prof_ms[1] - prof_ms[4] - prof_ms[5]) / 300,
                    stats_gl_draws / 300, prof_ms[7] / 300, prof_ms[8] / 300);
            stats_gl_draws = 0;
            memset(prof_ms, 0, sizeof(prof_ms));
        }
    }
    frame_no++;
    if (clear) {
        do_clear();
    }
    stats_draws = 0;
    stats_verts = 0;
    fighter_draws = 0;
    memset(pc_debug_fighter_counts, 0, sizeof(pc_debug_fighter_counts));
}

void GXSetTexCopySrc(u16 left, u16 top, u16 wd, u16 ht)
{
    tex_copy.left = left;
    tex_copy.top = top;
    tex_copy.wd = wd;
    tex_copy.ht = ht;
}

void GXSetTexCopyDst(u16 wd, u16 ht, GXTexFmt fmt, GXBool mipmap)
{
    tex_copy.dst_wd = wd;
    tex_copy.dst_ht = ht;
    tex_copy.fmt = (u8) fmt;
    tex_copy.mipmap = mipmap;
}

void GXCopyTex(void* dest, GXBool clear)
{
    int ww, wh, i, slot = -1;
    float sx, sy;
    CopyEntry* e;
    if (!rendering) {
        return;
    }
    imm_flush();
    for (i = 0; i < COPY_CACHE; i++) {
        if (copy_cache[i].dest == dest) {
            slot = i;
            break;
        }
        if (slot < 0 && copy_cache[i].dest == NULL) {
            slot = i;
        }
    }
    if (slot < 0) {
        slot = 0;
    }
    e = &copy_cache[slot];
    if (debug_log_frame != 0 && pc_frame_count == debug_log_frame) {
        fprintf(stderr, "[gx] GXCopyTex dest=%p src=(%u %u %u %u) dst=%ux%u clear=%d vp=(%.0f %.0f %.0f %.0f)\n", dest,
                tex_copy.left, tex_copy.top, tex_copy.wd, tex_copy.ht, (unsigned) tex_copy.wd, (unsigned) tex_copy.ht, (int) clear,
                gx.vp[0], gx.vp[1], gx.vp[2], gx.vp[3]);
    }
    if (e->tex == 0) {
        glGenTextures(1, &e->tex);
    }
    e->dest = dest;
    e->width = tex_copy.wd;
    e->height = tex_copy.ht;
    {
        int vx, vy, vw, vh;
        target_rect(&vx, &vy, &vw, &vh);
        sx = (float) vw / EFB_W;
        sy = (float) vh / EFB_H;
        /* GX textures start at the top row, GL framebuffers at the bottom
         * one: read the region back and flip it, so the copy samples like
         * the console's (the 1-P pause panel and the results screens draw
         * text into such copies) */
        GLint rx = vx + (GLint) (tex_copy.left * sx);
        GLint ry = vy + (GLint) ((EFB_H - tex_copy.top - tex_copy.ht) * sy);
        GLsizei rw = (GLsizei) (tex_copy.wd * sx), rh = (GLsizei) (tex_copy.ht * sy);
        static u8* buf;
        static size_t cap;
        size_t row = (size_t) rw * 4, need = row * (size_t) rh;
        static int noblit = -1;
        if (noblit < 0) {
            noblit = getenv("MELEE_GX_NOBLIT") != NULL; /* the readback path instead */
        }
        if (rw > 0 && rh > 0 && !noblit && pc_glBlitFramebuffer != NULL && pc_glGenFramebuffers != NULL) {
            /* blit into the texture through a framebuffer object, flipped
             * on the way; no readback, no stall */
            static GLuint fbo;
            double t0 = prof_now();
            if (fbo == 0) {
                pc_glGenFramebuffers(1, &fbo);
            }
            forget_texture(e->tex);
            use_unit(0);
            glBindTexture(GL_TEXTURE_2D, e->tex);
            unit_tex[0] = e->tex;
            if (e->gl_w != rw || e->gl_h != rh) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rw, rh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
                e->gl_w = rw;
                e->gl_h = rh;
            }
            {
                GLuint src = scene_read_fbo(rx, ry, rw, rh);
                pc_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
                pc_glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, e->tex, 0);
                pc_glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
            }
            glDisable(GL_SCISSOR_TEST);
            pc_glBlitFramebuffer(rx, ry, rx + rw, ry + rh, 0, rh, rw, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            pc_glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo);
            rs.valid = 0;
            prof_ms[4] += prof_now() - t0;
        } else if (rw > 0 && rh > 0) {
            e->gl_w = e->gl_h = 0;
            if (need > cap) {
                free(buf);
                buf = (u8*) malloc(need);
                cap = buf != NULL ? need : 0;
            }
            if (buf != NULL) {
                GLsizei y;
                double t0 = prof_now();
                flush_batch();
                if (scene_fbo != 0) {
                    pc_glBindFramebuffer(GL_READ_FRAMEBUFFER, scene_read_fbo(rx, ry, rw, rh));
                }
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(rx, ry, rw, rh, GL_RGBA, GL_UNSIGNED_BYTE, buf);
                if (scene_fbo != 0) {
                    pc_glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo);
                }
                prof_ms[4] += prof_now() - t0;
                for (y = 0; y < rh / 2; y++) {
                    u8* a = buf + (size_t) y * row;
                    u8* b = buf + (size_t) (rh - 1 - y) * row;
                    size_t k;
                    for (k = 0; k < row; k += 4) {
                        u32 t = *(u32*) (a + k);
                        *(u32*) (a + k) = *(u32*) (b + k);
                        *(u32*) (b + k) = t;
                    }
                }
                forget_texture(e->tex);
                use_unit(0);
                glBindTexture(GL_TEXTURE_2D, e->tex);
                unit_tex[0] = e->tex;
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rw, rh, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
            }
        }
    }
    if (clear) {
        do_clear();
    }
}

void GXSetDispCopySrc(u16 left, u16 top, u16 wd, u16 ht) { (void) left; (void) top; (void) wd; (void) ht; }
void GXSetDispCopyDst(u16 wd, u16 ht) { (void) wd; (void) ht; }
u32 GXSetDispCopyYScale(f32 vscale) { return (u32) (EFB_H * vscale + 0.5f); }
void GXSetDispCopyGamma(GXGamma g) { (void) g; }
void GXSetCopyClamp(GXFBClamp c) { (void) c; }
void GXSetCopyFilter(GXBool aa, const u8 sample_pattern[12][2], GXBool vf, const u8 vfilter[7])
{
    (void) aa; (void) sample_pattern; (void) vf; (void) vfilter;
}
void GXPixModeSync(void) {}
void GXSetPixelFmt(GXPixelFmt pix_fmt, GXZFmt16 z_fmt) { (void) pix_fmt; (void) z_fmt; }
void GXSetFieldMode(GXBool field_mode, GXBool half_aspect_ratio) { (void) field_mode; (void) half_aspect_ratio; }
void GXSetMisc(GXMiscToken token, u32 val) { (void) token; (void) val; }
void GXSetZTexture(GXZTexOp op, GXTexFmt fmt, u32 bias) { (void) op; (void) fmt; (void) bias; }
void GXSetFogRangeAdj(GXBool enable, u16 center, GXFogAdjTable* table) { (void) enable; (void) center; (void) table; }
void GXInitFogAdjTable(GXFogAdjTable* table, u16 width, f32 projmtx[4][4]) { (void) table; (void) width; (void) projmtx; }
void GXEnableTexOffsets(GXTexCoordID coord, u8 line_enable, u8 point_enable) { (void) coord; (void) line_enable; (void) point_enable; }

/* --- Init ---------------------------------------------------------------------- */

void pc_gx_render_init(void)
{
    gpu_path = getenv("MELEE_GX_CPU") != NULL ? 0 : -1; /* decided on first use */
    int i;
    memset(&gx, 0, sizeof(gx));
    for (i = 0; i < MTX_ROWS; i++) {
        gx.mtx[i][i % 3] = 1.0f;
    }
    for (i = 0; i < 32; i++) {
        gx.nrm[i][0][0] = gx.nrm[i][1][1] = gx.nrm[i][2][2] = 1.0f;
    }
    for (i = 0; i < 4; i++) {
        gx.swap_table[i][0] = 0;
        gx.swap_table[i][1] = 1;
        gx.swap_table[i][2] = 2;
        gx.swap_table[i][3] = 3;
    }
    gx.vp[2] = EFB_W;
    gx.vp[3] = EFB_H;
    gx.vp[5] = 1.0f;
    gx.scissor[2] = EFB_W;
    gx.scissor[3] = EFB_H;
    gx.color_update = 1;
    gx.alpha_update = 1;
    gx.z_enable = 1;
    gx.z_func = GX_LEQUAL;
    gx.z_update = 1;
    gx.alpha_comp0 = GX_ALWAYS;
    gx.alpha_comp1 = GX_ALWAYS;
    gx.alpha_op = GX_AOP_AND;
    gx.num_chans = 0;
    gx.clear_z = 0xFFFFFF;
    gx.proj[0][0] = gx.proj[1][1] = gx.proj[2][2] = gx.proj[3][3] = 1.0f;
    debug_log = getenv("MELEE_GX_DEBUG") != NULL;
    pc_debug_gx = debug_log;
    debug_flat = getenv("MELEE_GX_FLAT") != NULL;
    debug_nocull = getenv("MELEE_GX_NOCULL") != NULL;
    debug_noalpha = getenv("MELEE_GX_NOALPHA") != NULL;
    debug_litonly = getenv("MELEE_GX_LITONLY") != NULL;
    debug_log_frame = getenv("MELEE_GX_LOG_FRAME") != NULL ? (u32) strtoul(getenv("MELEE_GX_LOG_FRAME"), NULL, 0) : 0;
    rendering = pc_window_ready();
    if (rendering) {
        glEnable(GL_DEPTH_TEST);
        do_clear();
    }
}

void pc_gx_frame_begin(void) {}
