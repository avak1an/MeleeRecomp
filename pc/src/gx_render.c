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
    u8 pnmtx;
    u8 texmtx[8];
} Vertex;

typedef struct GLVertex {
    float pos[3];
    float col[2][4];
    float tex[8][2];
} GLVertex;

static u8 imm_buf[1 << 20];
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

static u32 vertex_stream_size(u8 vat);
static int rendering; /* GL context exists */
static u32 frame_no;
static int stats_draws, stats_verts;
static int debug_log;  /* MELEE_GX_DEBUG: log the first draws of a frame */
static int debug_flat; /* MELEE_GX_FLAT: magenta fragments, no alpha test */
int pc_debug_in_fighter;      /* set by the fighter draw routine: 1 + kind while its model is drawn */
static int fighter_draws;     /* draws issued for fighters this frame */
int pc_debug_fighter_counts[4]; /* jobj / dobj / pobj / display-list calls while drawing fighters */
static int debug_nocull;      /* MELEE_GX_NOCULL: never cull faces */
static int debug_noalpha;     /* MELEE_GX_NOALPHA: skip the alpha test */
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
    if (vcd == GX_INDEX8) {
        return 1;
    }
    if (vcd == GX_INDEX16) {
        return 2;
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
        n = attr_comps(attr, f);
        for (i = 0; i < 3; i++) {
            v->nrm[i] = read_comp(p + i * comp_size(f->type), f->type, f->frac, big);
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
static const u8* decode_vertex(const u8* p, u8 vat, int big, Vertex* v)
{
    u32 attr;
    memset(v, 0, sizeof(*v));
    v->col[0][0] = v->col[0][1] = v->col[0][2] = v->col[0][3] = 1.0f;
    v->col[1][0] = v->col[1][1] = v->col[1][2] = v->col[1][3] = 1.0f;
    v->pnmtx = (u8) gx.current_mtx;
    for (attr = 0; attr < NUM_ATTR; attr++) {
        u8 vcd = gx.vcd[attr];
        const VtxAttrFmt* f = &gx.vat[vat][attr];
        if (vcd == GX_NONE) {
            continue;
        }
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

static void texgen(const Vertex* v, const float vpos[3], const float vnrm[3], int i, float out[2])
{
    const TexGen* g = &gx.texgen[i];
    float in[4], s, t, q;
    const float(*m)[4];
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
        out[0] = in[0];
        out[1] = in[1];
        return;
    }
    m = (const float(*)[4]) gx.mtx[g->mtx];
    s = m[0][0] * in[0] + m[0][1] * in[1] + m[0][2] * in[2] + m[0][3] * in[3];
    t = m[1][0] * in[0] + m[1][1] * in[1] + m[1][2] * in[2] + m[1][3] * in[3];
    if (g->type == GX_TG_MTX3x4) {
        q = m[2][0] * in[0] + m[2][1] * in[1] + m[2][2] * in[2] + m[2][3] * in[3];
        if (q != 0.0f) {
            s /= q;
            t /= q;
        }
    }
    out[0] = s;
    out[1] = t;
}

static void transform_vertex(const Vertex* v, GLVertex* out)
{
    float pos[3], nrm[3];
    const float(*pm)[4];
    u32 slot = v->pnmtx;
    int i;

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
    }
    memcpy(out->pos, pos, sizeof(pos));

    for (i = 0; i < 2; i++) {
        float c[4], a[4];
        if (i < gx.num_chans) {
            light_channel(&gx.chan[i], i, v->col[i], gx.mat_color[i], gx.amb_color[i], pos, nrm, c);
            light_channel(&gx.chan[2 + i], 2 + i, v->col[i], gx.mat_color[i], gx.amb_color[i], pos,
                          nrm, a);
            out->col[i][0] = c[0];
            out->col[i][1] = c[1];
            out->col[i][2] = c[2];
            out->col[i][3] = a[3];
        } else {
            memcpy(out->col[i], v->col[i], sizeof(out->col[i]));
        }
    }
    for (i = 0; i < 8; i++) {
        if (i < gx.num_texgen) {
            texgen(v, pos, nrm, i, out->tex[i]);
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
    GLuint tex;
    u32 last_frame;
} TexEntry;

#define TEX_CACHE 1024
static TexEntry tex_cache[TEX_CACHE];

/* EFB copies: a destination pointer that GXCopyTex wrote maps to a GL
 * texture holding the copied pixels. */
typedef struct CopyEntry {
    const void* dest;
    GLuint tex;
    u16 width, height;
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

static GLenum gl_wrap(u8 w)
{
    return w == GX_CLAMP ? GL_CLAMP_TO_EDGE : w == GX_MIRROR ? GL_MIRRORED_REPEAT : GL_REPEAT;
}

static GLuint bind_texture(const PCTexObj* t)
{
    u32 i, free_slot = TEX_CACHE;
    const PCTlutObj* tlut = t->tlut_name < 20 ? &gx.tlut[t->tlut_name] : NULL;
    const void* lut = tlut ? tlut->lut : NULL;
    TexEntry* e = NULL;

    for (i = 0; i < COPY_CACHE; i++) {
        if (copy_cache[i].dest == t->image && copy_cache[i].tex != 0) {
            glBindTexture(GL_TEXTURE_2D, copy_cache[i].tex);
            goto params;
        }
    }
    for (i = 0; i < TEX_CACHE; i++) {
        if (tex_cache[i].tex == 0) {
            if (free_slot == TEX_CACHE) {
                free_slot = i;
            }
            continue;
        }
        if (tex_cache[i].image == t->image && tex_cache[i].width == t->width &&
            tex_cache[i].height == t->height && tex_cache[i].format == t->format &&
            tex_cache[i].tlut_name == t->tlut_name && tex_cache[i].lut == lut) {
            e = &tex_cache[i];
            break;
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
            glDeleteTextures(1, &tex_cache[oldest].tex);
            memset(&tex_cache[oldest], 0, sizeof(TexEntry));
            free_slot = oldest;
        }
        e = &tex_cache[free_slot];
        pixels = decode_texture(t, &w, &h);
        e->image = t->image;
        e->width = t->width;
        e->height = t->height;
        e->format = t->format;
        e->tlut_name = t->tlut_name;
        e->lut = lut;
        glGenTextures(1, &e->tex);
        glBindTexture(GL_TEXTURE_2D, e->tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei) w, (GLsizei) h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, pixels);
        free(pixels);
    }
    e->last_frame = frame_no;
    glBindTexture(GL_TEXTURE_2D, e->tex);
params:
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gl_wrap(t->wrap_s));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gl_wrap(t->wrap_t));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, t->min_filt == GX_NEAR ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, t->mag_filt == GX_NEAR ? GL_NEAREST : GL_LINEAR);
    return 0;
}

/* --- TEV shader generation ---------------------------------------------- */

typedef struct ShaderKey {
    TevStage tev[16];
    u8 num_tev;
    u8 alpha_comp0, alpha_op, alpha_comp1;
    u8 swap_table[4][4];
    u8 texmap_valid[8];
} ShaderKey;

typedef struct Shader {
    ShaderKey key;
    GLuint prog;
    GLint u_proj, u_tex[8], u_kcolor, u_tevreg, u_aref;
    GLint a_pos, a_col0, a_col1, a_tex[8];
    int used;
} Shader;

#define MAX_SHADERS 256
static Shader shaders[MAX_SHADERS];
static int num_shaders;

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
    if (s->map < 8 && k->texmap_valid[s->map] && s->coord < 8) {
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
        "void main() {\n"
        "    vec4 p = u_proj * vec4(a_pos, 1.0);\n"
        "    p.z = p.z * 2.0 + p.w;\n" /* GX clip depth [-w,0] -> GL [-w,w] */
        "    gl_Position = p;\n"
        "    v_col0 = a_col0; v_col1 = a_col1;\n"
        "    v_tex0 = a_tex0; v_tex1 = a_tex1; v_tex2 = a_tex2; v_tex3 = a_tex3;\n"
        "    v_tex4 = a_tex4; v_tex5 = a_tex5; v_tex6 = a_tex6; v_tex7 = a_tex7;\n"
        "}\n";

    memset(&key, 0, sizeof(key));
    key.num_tev = gx.num_tev;
    for (i = 0; i < gx.num_tev; i++) {
        key.tev[i] = gx.tev[i];
    }
    key.alpha_comp0 = gx.alpha_comp0;
    key.alpha_op = gx.alpha_op;
    key.alpha_comp1 = gx.alpha_comp1;
    memcpy(key.swap_table, gx.swap_table, sizeof(key.swap_table));
    for (i = 0; i < 8; i++) {
        key.texmap_valid[i] = gx.texmap[i].image != NULL;
    }
    for (s = 0; s < num_shaders; s++) {
        if (memcmp(&shaders[s].key, &key, sizeof(key)) == 0) {
            return &shaders[s];
        }
    }
    if (num_shaders == MAX_SHADERS) {
        num_shaders = 0; /* crude: start over */
    }
    sh = &shaders[num_shaders++];
    memset(sh, 0, sizeof(*sh));
    sh->key = key;

    shader_len = 0;
    emit("#version 120\n");
    for (i = 0; i < 8; i++) {
        emit("uniform sampler2D u_tex%d;\n", i);
    }
    emit("uniform vec4 u_kcolor[4];\n"
         "uniform vec4 u_tevreg[4];\n"
         "uniform vec4 u_aref;\n"
         "varying vec4 v_col0, v_col1;\n"
         "varying vec2 v_tex0, v_tex1, v_tex2, v_tex3, v_tex4, v_tex5, v_tex6, v_tex7;\n"
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
    if (debug_flat) {
        emit("    gl_FragColor = vec4(1.0, 0.0, 1.0, 1.0);\n}\n");
    } else {
        emit("    gl_FragColor = clamp(prev, 0.0, 1.0);\n}\n");
    }

    vs = compile(GL_VERTEX_SHADER, vsrc);
    fs = compile(GL_FRAGMENT_SHADER, shader_src);
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
    sh->a_pos = pc_glGetAttribLocation(sh->prog, "a_pos");
    sh->a_col0 = pc_glGetAttribLocation(sh->prog, "a_col0");
    sh->a_col1 = pc_glGetAttribLocation(sh->prog, "a_col1");
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

static void apply_raster_state(void)
{
    int vx, vy, vw, vh;
    float sx, sy;
    pc_window_viewport(&vx, &vy, &vw, &vh);
    sx = (float) vw / EFB_W;
    sy = (float) vh / EFB_H;

    glViewport(vx + (GLint) (gx.vp[0] * sx), vy + (GLint) ((EFB_H - gx.vp[1] - gx.vp[3]) * sy),
               (GLsizei) (gx.vp[2] * sx), (GLsizei) (gx.vp[3] * sy));
    glDepthRange(gx.vp[4], gx.vp[5]);
    glEnable(GL_SCISSOR_TEST);
    glScissor(vx + (GLint) (gx.scissor[0] * sx),
              vy + (GLint) ((EFB_H - (int) gx.scissor[1] - (int) gx.scissor[3]) * sy),
              (GLsizei) (gx.scissor[2] * sx), (GLsizei) (gx.scissor[3] * sy));

    if (gx.cull == GX_CULL_NONE || debug_nocull) {
        glDisable(GL_CULL_FACE);
    } else {
        glEnable(GL_CULL_FACE);
        glFrontFace(GL_CW);
        glCullFace(gx.cull == GX_CULL_FRONT ? GL_FRONT : gx.cull == GX_CULL_BACK ? GL_BACK : GL_FRONT_AND_BACK);
    }
    if (gx.z_enable) {
        static const GLenum funcs[8] = { GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL,
                                         GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS };
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(funcs[gx.z_func & 7]);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(gx.z_update ? GL_TRUE : GL_FALSE);
    glColorMask(gx.color_update, gx.color_update, gx.color_update, gx.alpha_update);

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
}

static void ensure_capacity(u32 nverts)
{
    if (nverts > glverts_cap) {
        glverts_cap = nverts + 1024;
        glverts = (GLVertex*) realloc(glverts, glverts_cap * sizeof(GLVertex));
    }
    if (nverts * 2 > indices_cap) {
        indices_cap = nverts * 2 + 64;
        indices = (u16*) realloc(indices, indices_cap * sizeof(u16));
    }
}

/// Draw nverts vertices from a stream in the given byte order.
static void draw_stream(u8 prim, u8 vat, const u8* stream, u32 nverts, int big)
{
    Shader* sh;
    const u8* p = stream;
    u32 i;
    int t;
    GLenum mode;
    float proj[16];

    if (!rendering || nverts == 0) {
        return;
    }
    if (nverts > MAX_VERTS) {
        nverts = MAX_VERTS;
    }
    ensure_capacity(nverts);
    for (i = 0; i < nverts; i++) {
        Vertex v;
        p = decode_vertex(p, vat, big, &v);
        transform_vertex(&v, &glverts[i]);
    }
    if (pc_debug_in_fighter) {
        fighter_draws++;
    }
    if ((debug_log && stats_draws < 8) || (debug_log_frame != 0 && pc_frame_count == debug_log_frame)) {
        const GLVertex* g = &glverts[0];
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
                "col0=(%.2f %.2f %.2f %.2f) tex0=(%.2f %.2f) tev=%u map0=%p chans=%u texgen=%u "
                "vp=(%.0f %.0f %.0f %.0f) cull=%u z=%u/%u/%u blend=%u alpha=%u/%u/%u/%u proj=%u\n",
                prim, vat, nverts, big, x, y, z, cx, cy, cz, cw, g->col[0][0], g->col[0][1],
                g->col[0][2], g->col[0][3], g->tex[0][0], g->tex[0][1], gx.num_tev, gx.texmap[0].image,
                gx.num_chans, gx.num_texgen, gx.vp[0], gx.vp[1], gx.vp[2], gx.vp[3], gx.cull,
                gx.z_enable, gx.z_func, gx.z_update, gx.blend_mode, gx.alpha_comp0, gx.alpha_ref0,
                gx.alpha_op, gx.alpha_comp1, gx.proj_type);
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

    apply_raster_state();
    sh = get_shader();
    pc_glUseProgram(sh->prog);

    /* projection, column-major for GL */
    for (i = 0; i < 4; i++) {
        proj[i * 4 + 0] = gx.proj[0][i];
        proj[i * 4 + 1] = gx.proj[1][i];
        proj[i * 4 + 2] = gx.proj[2][i];
        proj[i * 4 + 3] = gx.proj[3][i];
    }
    pc_glUniformMatrix4fv(sh->u_proj, 1, GL_FALSE, proj);
    pc_glUniform4fv(sh->u_kcolor, 4, &gx.kcolor[0][0]);
    pc_glUniform4fv(sh->u_tevreg, 4, &gx.tev_reg[0][0]);
    {
        float aref[4] = { gx.alpha_ref0 / 255.0f, gx.alpha_ref1 / 255.0f, 0, 0 };
        pc_glUniform4fv(sh->u_aref, 1, aref);
    }
    for (t = 0; t < 8; t++) {
        if (sh->u_tex[t] >= 0 && gx.texmap[t].image != NULL) {
            pc_glActiveTexture(GL_TEXTURE0 + t);
            bind_texture(&gx.texmap[t]);
            pc_glUniform1i(sh->u_tex[t], t);
        }
    }
    pc_glActiveTexture(GL_TEXTURE0);

    if (sh->a_pos >= 0) {
        pc_glEnableVertexAttribArray(sh->a_pos);
        pc_glVertexAttribPointer(sh->a_pos, 3, GL_FLOAT, GL_FALSE, sizeof(GLVertex), &glverts[0].pos);
    }
    if (sh->a_col0 >= 0) {
        pc_glEnableVertexAttribArray(sh->a_col0);
        pc_glVertexAttribPointer(sh->a_col0, 4, GL_FLOAT, GL_FALSE, sizeof(GLVertex), &glverts[0].col[0]);
    }
    if (sh->a_col1 >= 0) {
        pc_glEnableVertexAttribArray(sh->a_col1);
        pc_glVertexAttribPointer(sh->a_col1, 4, GL_FLOAT, GL_FALSE, sizeof(GLVertex), &glverts[0].col[1]);
    }
    for (t = 0; t < 8; t++) {
        if (sh->a_tex[t] >= 0) {
            pc_glEnableVertexAttribArray(sh->a_tex[t]);
            pc_glVertexAttribPointer(sh->a_tex[t], 2, GL_FLOAT, GL_FALSE, sizeof(GLVertex), &glverts[0].tex[t]);
        }
    }

    switch (prim) {
    case GX_QUADS: {
        u32 n = 0, q;
        for (q = 0; q + 3 < nverts; q += 4) {
            indices[n++] = (u16) q;
            indices[n++] = (u16) (q + 1);
            indices[n++] = (u16) (q + 2);
            indices[n++] = (u16) q;
            indices[n++] = (u16) (q + 2);
            indices[n++] = (u16) (q + 3);
        }
        glDrawElements(GL_TRIANGLES, (GLsizei) n, GL_UNSIGNED_SHORT, indices);
        mode = 0;
        break;
    }
    case GX_TRIANGLES: mode = GL_TRIANGLES; break;
    case GX_TRIANGLESTRIP: mode = GL_TRIANGLE_STRIP; break;
    case GX_TRIANGLEFAN: mode = GL_TRIANGLE_FAN; break;
    case GX_LINES: mode = GL_LINES; break;
    case GX_LINESTRIP: mode = GL_LINE_STRIP; break;
    case GX_POINTS: mode = GL_POINTS; break;
    default: mode = 0; break;
    }
    if (mode != 0) {
        glDrawArrays(mode, 0, (GLsizei) nverts);
    }
    if (sh->a_pos >= 0) pc_glDisableVertexAttribArray(sh->a_pos);
    if (sh->a_col0 >= 0) pc_glDisableVertexAttribArray(sh->a_col0);
    if (sh->a_col1 >= 0) pc_glDisableVertexAttribArray(sh->a_col1);
    for (t = 0; t < 8; t++) {
        if (sh->a_tex[t] >= 0) pc_glDisableVertexAttribArray(sh->a_tex[t]);
    }
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
        draw_stream(prim, vat, p, n, 1);
        p += n * vsize;
    }
}

/* --- Vertex descriptors and arrays ------------------------------------- */

void GXClearVtxDesc(void)
{
    memset(gx.vcd, GX_NONE, sizeof(gx.vcd));
}

void GXSetVtxDesc(GXAttr attr, GXAttrType type)
{
    if ((u32) attr < NUM_ATTR) {
        gx.vcd[attr] = (u8) type;
    }
}

void GXSetVtxAttrFmt(GXVtxFmt vtxfmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac)
{
    if ((u32) vtxfmt < 8 && (u32) attr < NUM_ATTR) {
        VtxAttrFmt* f = &gx.vat[vtxfmt][attr];
        f->cnt = (u8) cnt;
        f->type = (u8) type;
        f->frac = frac;
    }
}

void GXSetArray(GXAttr attr, const void* base_ptr, u8 stride)
{
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
}

void GXLoadTexMtxImm(f32 mtx[][4], u32 id, GXTexMtxType type)
{
    u32 rows = type == GX_MTX2x4 ? 2 : 3;
    if (id + rows <= MTX_ROWS) {
        memcpy(gx.mtx[id], mtx, rows * 4 * sizeof(float));
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
    imm_flush();
    gx.num_tev = nStages > 16 ? 16 : nStages;
}

void GXSetTevOrder(GXTevStageID stage, GXTexCoordID coord, GXTexMapID map, GXChannelID color)
{
    TevStage* s = &gx.tev[stage & 15];
    s->coord = (u8) coord;
    s->map = (u8) (map & 0xFF);
    s->chan = (u8) color;
}

void GXSetTevColorIn(GXTevStageID stage, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c, GXTevColorArg d)
{
    TevStage* s = &gx.tev[stage & 15];
    s->ca[0] = (u8) a;
    s->ca[1] = (u8) b;
    s->ca[2] = (u8) c;
    s->ca[3] = (u8) d;
}

void GXSetTevAlphaIn(GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d)
{
    TevStage* s = &gx.tev[stage & 15];
    s->aa[0] = (u8) a;
    s->aa[1] = (u8) b;
    s->aa[2] = (u8) c;
    s->aa[3] = (u8) d;
}

void GXSetTevColorOp(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID out_reg)
{
    TevStage* s = &gx.tev[stage & 15];
    s->cop = (u8) op;
    s->cbias = (u8) bias;
    s->cscale = (u8) scale;
    s->cclamp = clamp;
    s->creg = (u8) out_reg;
}

void GXSetTevAlphaOp(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID out_reg)
{
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
    r[0] = color.r / 255.0f;
    r[1] = color.g / 255.0f;
    r[2] = color.b / 255.0f;
    r[3] = color.a / 255.0f;
}

void GXSetTevKColorSel(GXTevStageID stage, GXTevKColorSel sel)
{
    gx.tev[stage & 15].kcsel = (u8) sel;
}

void GXSetTevKAlphaSel(GXTevStageID stage, GXTevKAlphaSel sel)
{
    gx.tev[stage & 15].kasel = (u8) sel;
}

void GXSetTevSwapMode(GXTevStageID stage, GXTevSwapSel ras_sel, GXTevSwapSel tex_sel)
{
    gx.tev[stage & 15].ras_swap = (u8) ras_sel;
    gx.tev[stage & 15].tex_swap = (u8) tex_sel;
}

void GXSetTevSwapModeTable(GXTevSwapSel table, GXTevColorChan red, GXTevColorChan green, GXTevColorChan blue, GXTevColorChan alpha)
{
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
    (void) tev_stage;
}

void GXSetTevIndirect(GXTevStageID tev_stage, GXIndTexStageID ind_stage, GXIndTexFormat format, GXIndTexBiasSel bias_sel, GXIndTexMtxID matrix_sel, GXIndTexWrap wrap_s, GXIndTexWrap wrap_t, GXBool add_prev, GXBool utc_lod, GXIndTexAlphaSel alpha_sel)
{
    (void) tev_stage; (void) ind_stage; (void) format; (void) bias_sel; (void) matrix_sel;
    (void) wrap_s; (void) wrap_t; (void) add_prev; (void) utc_lod; (void) alpha_sel;
}

void GXSetNumIndStages(u8 n) { (void) n; }
void GXSetIndTexOrder(GXIndTexStageID s, GXTexCoordID c, GXTexMapID m) { (void) s; (void) c; (void) m; }
void GXSetIndTexCoordScale(GXIndTexStageID s, GXIndTexScale a, GXIndTexScale b) { (void) s; (void) a; (void) b; }
void GXSetIndTexMtx(GXIndTexMtxID id, f32 offset[2][3], s8 scale_exp) { (void) id; (void) offset; (void) scale_exp; }

/* --- Texture coordinate generation and channels ------------------------- */

void GXSetNumTexGens(u8 nTexGens)
{
    gx.num_texgen = nTexGens > 8 ? 8 : nTexGens;
}

void GXSetTexCoordGen2(GXTexCoordID dst_coord, GXTexGenType func, GXTexGenSrc src_param, u32 mtx, GXBool normalize, u32 pt_texmtx)
{
    TexGen* g = &gx.texgen[dst_coord & 7];
    g->type = (u8) func;
    g->src = (u8) src_param;
    g->mtx = mtx;
    g->normalize = normalize;
    g->pt_mtx = pt_texmtx;
}

void GXSetNumChans(u8 nChans)
{
    gx.num_chans = nChans > 2 ? 2 : nChans;
}

void GXSetChanCtrl(GXChannelID chan, GXBool enable, GXColorSrc amb_src, GXColorSrc mat_src, u32 light_mask, GXDiffuseFn diff_fn, GXAttnFn attn_fn)
{
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
    if (chan == GX_COLOR0 || chan == GX_COLOR0A0) set_color4(gx.amb_color[0], amb_color);
    if (chan == GX_COLOR1 || chan == GX_COLOR1A1) set_color4(gx.amb_color[1], amb_color);
    if (chan == GX_ALPHA0) gx.amb_color[0][3] = amb_color.a / 255.0f;
    if (chan == GX_ALPHA1) gx.amb_color[1][3] = amb_color.a / 255.0f;
}

void GXSetChanMatColor(GXChannelID chan, GXColor mat_color)
{
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
}

/* --- Textures and palettes --------------------------------------------------- */

void GXLoadTexObj(GXTexObj* obj, GXTexMapID id)
{
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
            glDeleteTextures(1, &tex_cache[i].tex);
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
    pc_window_size(&w, &h);
    row_bytes = ((u32) w * 3 + 3) & ~3u;
    size = row_bytes * (u32) h;
    pixels = (u8*) calloc(size, 1);
    if (pixels == NULL) {
        return;
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, w, h, GL_BGR_EXT, GL_UNSIGNED_BYTE, pixels);
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
    if (pc_config.screenshot_dir != NULL && frame_no % 60 == 0) {
        save_screenshot();
    }
    pc_window_present();
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
    if (e->tex == 0) {
        glGenTextures(1, &e->tex);
    }
    e->dest = dest;
    e->width = tex_copy.wd;
    e->height = tex_copy.ht;
    {
        int vx, vy, vw, vh;
        pc_window_viewport(&vx, &vy, &vw, &vh);
        sx = (float) vw / EFB_W;
        sy = (float) vh / EFB_H;
        glBindTexture(GL_TEXTURE_2D, e->tex);
        glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, vx + (GLint) (tex_copy.left * sx),
                         vy + (GLint) ((EFB_H - tex_copy.top - tex_copy.ht) * sy), (GLsizei) (tex_copy.wd * sx),
                         (GLsizei) (tex_copy.ht * sy), 0);
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
    debug_log_frame = getenv("MELEE_GX_LOG_FRAME") != NULL ? (u32) strtoul(getenv("MELEE_GX_LOG_FRAME"), NULL, 0) : 0;
    rendering = pc_window_ready();
    if (rendering) {
        glEnable(GL_DEPTH_TEST);
        do_clear();
    }
}

void pc_gx_frame_begin(void) {}
