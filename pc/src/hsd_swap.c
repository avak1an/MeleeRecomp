/**
 * @file hsd_swap.c
 * Byte-swapping of HSD descriptors loaded from disc.
 *
 * Archives arrive big-endian. HSD_ArchiveParse (archive.c, TARGET_PC block)
 * swaps the archive header, its tables, and every pointer slot named by the
 * relocation table. Everything else, the scalar fields of the descriptors
 * the pointers lead to, is swapped here, on entry to the engine loader that
 * consumes each descriptor type. Loaders recurse through the object graph,
 * so each function only swaps its own struct (and the small helper structs
 * its loader reads inline) and relies on the other loaders for the rest.
 *
 * A descriptor can be reached more than once (shared textures, instanced
 * joints), so every function records the addresses it has swapped and
 * skips repeats. When a file's memory is freed the records for that range
 * must be dropped (pc_swap_forget_range) so a later file at the same
 * address is swapped again.
 */
#include "pc_runtime.h"

#include <pc_endian.h>
#include <pc_hsd_swap.h>

#include <sysdolphin/baselib/aobj.h>
#include <sysdolphin/baselib/cobj.h>
#include <sysdolphin/baselib/fobj.h>
#include <sysdolphin/baselib/fog.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/lobj.h>
#include <sysdolphin/baselib/mobj.h>
#include <sysdolphin/baselib/pobj.h>
#include <sysdolphin/baselib/robj.h>
#include <sysdolphin/baselib/spline.h>
#include <sysdolphin/baselib/tobj.h>
#include <sysdolphin/baselib/wobj.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- "Already swapped" registry: open-addressing hash set of addresses --- */

#define REG_BITS 20
#define REG_SIZE (1u << REG_BITS)
static uintptr_t* reg;
static u32 reg_used;

static u32 reg_hash(uintptr_t p)
{
    p ^= p >> 16;
    p *= 0x7feb352du;
    p ^= p >> 15;
    return (u32) p & (REG_SIZE - 1);
}

/// Returns true (and records p) if p has not been seen before. Descriptors
/// outside the emulated main memory are static data compiled into the game
/// (already in host order) and are never swapped.
static bool once(const void* ptr)
{
    uintptr_t p = (uintptr_t) ptr;
    u32 i;
    if (p < (uintptr_t) pc_mem_base() || p >= (uintptr_t) pc_mem_base() + pc_mem_size()) {
        return false;
    }
    if (reg == NULL) {
        reg = (uintptr_t*) calloc(REG_SIZE, sizeof(uintptr_t));
    }
    if (reg_used > REG_SIZE / 2) {
        fprintf(stderr, "[pc] swap registry full (%u entries)\n", reg_used);
        pc_exit(6);
    }
    for (i = reg_hash(p);; i = (i + 1) & (REG_SIZE - 1)) {
        if (reg[i] == 0) {
            reg[i] = p;
            reg_used++;
            return true;
        }
        if (reg[i] == p) {
            return false;
        }
    }
}

void pc_swap_forget_range(void* base, size_t size)
{
    uintptr_t lo = (uintptr_t) base, hi = lo + size;
    u32 i;
    if (reg == NULL) {
        return;
    }
    /* Deleting from an open-addressing table: rebuild without the range. */
    {
        uintptr_t* old = reg;
        reg = (uintptr_t*) calloc(REG_SIZE, sizeof(uintptr_t));
        reg_used = 0;
        for (i = 0; i < REG_SIZE; i++) {
            if (old[i] != 0 && !(old[i] >= lo && old[i] < hi)) {
                once((const void*) old[i]);
            }
        }
        free(old);
    }
}

/* --- Helpers -------------------------------------------------------------- */

static void swap_vec3(Vec3* v)
{
    pc_swapf(&v->x);
    pc_swapf(&v->y);
    pc_swapf(&v->z);
}

/* --- Joints and their reference objects -------------------------------- */

void pc_swap_joint(HSD_Joint* joint)
{
    if (joint == NULL || !once(joint)) {
        return;
    }
    pc_swap32(&joint->flags);
    swap_vec3(&joint->rotation);
    swap_vec3(&joint->scale);
    swap_vec3(&joint->position);
    if (joint->mtx != NULL && once(joint->mtx)) {
        pc_swap32_range(joint->mtx, sizeof(Mtx));
    }
    if ((joint->flags & JOBJ_SPLINE) && joint->u.spline != NULL) {
        pc_swap_spline(joint->u.spline);
    }
}

void pc_swap_spline(HSD_Spline* spline)
{
    int i;
    if (spline == NULL || !once(spline)) {
        return;
    }
    pc_swap16(&spline->numcv);
    pc_swapf(&spline->tension);
    pc_swapf(&spline->totalLength);
    if (spline->cv != NULL) {
        for (i = 0; i < spline->numcv; i++) {
            swap_vec3(&spline->cv[i]);
        }
    }
    if (spline->segLength != NULL) {
        pc_swap32_range(spline->segLength, (size_t) spline->numcv * sizeof(f32));
    }
    if (spline->segPoly != NULL) {
        pc_swap32_range(spline->segPoly, (size_t) spline->numcv * 5 * sizeof(f32));
    }
}

void pc_swap_robjdesc(HSD_RObjDesc* desc)
{
    for (; desc != NULL; desc = desc->next) {
        if (!once(desc)) {
            return;
        }
        pc_swap32(&desc->flags);
        switch (desc->flags & ROBJ_TYPE_MASK) {
        case REFTYPE_LIMIT:
            pc_swapf(&desc->u.limit);
            break;
        case REFTYPE_IKHINT:
            if (desc->u.ik_hint != NULL && once(desc->u.ik_hint)) {
                pc_swapf(&desc->u.ik_hint->bone_length);
                pc_swapf(&desc->u.ik_hint->rotate_x);
            }
            break;
        default:
            /* REFTYPE_JOBJ / EXP / BYTECODE hold pointers, already relocated. */
            break;
        }
    }
}

/* --- Camera and world objects ------------------------------------------- */

void pc_swap_wobjdesc(HSD_WObjDesc* desc)
{
    if (desc == NULL || !once(desc)) {
        return;
    }
    swap_vec3(&desc->pos);
}

void pc_swap_cobjdesc(HSD_CObjDesc* desc)
{
    HSD_CameraDescCommon* c;
    if (desc == NULL || !once(desc)) {
        return;
    }
    c = &desc->common;
    pc_swap16(&c->flags);
    pc_swap16(&c->projection_type);
    pc_swap16_range(&c->viewport, sizeof(c->viewport));
    pc_swap16_range(&c->scissor, sizeof(c->scissor));
    pc_swapf(&c->roll);
    pc_swapf(&c->nnear);
    pc_swapf(&c->ffar);
    if (c->up_vector != NULL && once(c->up_vector)) {
        swap_vec3(c->up_vector);
    }
    switch (c->projection_type) {
    case PROJ_PERSPECTIVE:
        pc_swapf(&desc->perspective.fov);
        pc_swapf(&desc->perspective.aspect);
        break;
    case PROJ_ORTHO:
    case PROJ_FRUSTUM:
        pc_swapf(&desc->frustum.top);
        pc_swapf(&desc->frustum.bottom);
        pc_swapf(&desc->frustum.left);
        pc_swapf(&desc->frustum.right);
        break;
    default:
        break;
    }
}

/* --- Lights and fog ------------------------------------------------------ */

void pc_swap_lightdesc(HSD_LightDesc* desc)
{
    for (; desc != NULL; desc = desc->next) {
        if (!once(desc)) {
            return;
        }
        pc_swap16(&desc->flags);
        pc_swap16(&desc->attnflags);
        /* color is four bytes */
        if (desc->u.p == NULL || !once(desc->u.p)) {
            continue;
        }
        switch (desc->flags & LOBJ_TYPE_MASK) {
        case LOBJ_POINT:
            if (desc->attnflags & LOBJ_LIGHT_ATTN) {
                pc_swap32_range(desc->u.attn, sizeof(HSD_LightAttn));
            } else {
                pc_swap32_range(desc->u.point, sizeof(HSD_LightPointDesc));
            }
            break;
        case LOBJ_SPOT:
            if (desc->attnflags != 0) {
                pc_swap32_range(desc->u.attn, sizeof(HSD_LightAttn));
            } else {
                pc_swap32_range(desc->u.spot, sizeof(HSD_LightSpotDesc));
            }
            break;
        default:
            /* ambient / infinite: u.shininess when the specular flag is set */
            if (desc->flags & LOBJ_SPECULAR) {
                pc_swapf(desc->u.shininess);
            }
            break;
        }
    }
}

void pc_swap_fogadjdesc(HSD_FogAdjDesc* desc)
{
    if (desc == NULL || !once(desc)) {
        return;
    }
    pc_swap16(&desc->center);
    pc_swap16(&desc->width);
    pc_swap32_range(desc->mtx, sizeof(Mtx44));
}

void pc_swap_fogdesc(HSD_FogDesc* desc)
{
    if (desc == NULL || !once(desc)) {
        return;
    }
    pc_swap32(&desc->type);
    pc_swapf(&desc->start);
    pc_swapf(&desc->end);
    pc_swap_fogadjdesc(desc->fogadjdesc);
}

/* --- Animation ------------------------------------------------------------ */

void pc_swap_aobjdesc(HSD_AObjDesc* desc)
{
    if (desc == NULL || !once(desc)) {
        return;
    }
    pc_swap32(&desc->flags);
    pc_swapf(&desc->end_frame);
    pc_swap32(&desc->obj_id);
}

void pc_swap_fobjdesc(HSD_FObjDesc* desc)
{
    /* The keyframe stream (ad) is a byte-oriented little-endian format and
     * needs no swapping; only the header words do. */
    for (; desc != NULL; desc = desc->next) {
        if (!once(desc)) {
            return;
        }
        pc_swap32(&desc->length);
        pc_swapf(&desc->startframe);
    }
}

/* --- Materials and textures --------------------------------------------- */

void pc_swap_mobjdesc(HSD_MObjDesc* desc)
{
    if (desc == NULL || !once(desc)) {
        return;
    }
    pc_swap32(&desc->rendermode);
    if (desc->mat != NULL && once(desc->mat)) {
        pc_swapf(&desc->mat->alpha);
        pc_swapf(&desc->mat->shininess);
    }
    /* pedesc is all bytes; renderdesc is not read by the loader */
}

void pc_swap_tlutdesc(HSD_TlutDesc* desc)
{
    if (desc == NULL || !once(desc)) {
        return;
    }
    pc_swap32(&desc->fmt);
    pc_swap32(&desc->tlut_name);
    pc_swap16(&desc->n_entries);
    /* lut data stays in GX order for the renderer */
}

void pc_swap_tobjtevdesc(HSD_TObjTevDesc* desc)
{
    if (desc == NULL || !once(desc)) {
        return;
    }
    pc_swap32(&desc->active);
}

static void swap_imagedesc(HSD_ImageDesc* im)
{
    if (im == NULL || !once(im)) {
        return;
    }
    pc_swap16(&im->width);
    pc_swap16(&im->height);
    pc_swap32(&im->format);
    pc_swap32(&im->mipmap);
    pc_swapf(&im->minLOD);
    pc_swapf(&im->maxLOD);
    /* image_ptr data stays in GX texture format */
}

void pc_swap_tobjdesc(HSD_TObjDesc* desc)
{
    for (; desc != NULL; desc = desc->next) {
        if (!once(desc)) {
            return;
        }
        pc_swap32(&desc->id);
        pc_swap32(&desc->src);
        swap_vec3(&desc->rotate);
        swap_vec3(&desc->scale);
        swap_vec3(&desc->translate);
        pc_swap32(&desc->wrap_s);
        pc_swap32(&desc->wrap_t);
        pc_swap32(&desc->blend_flags);
        pc_swapf(&desc->blending);
        pc_swap32(&desc->magFilt);
        swap_imagedesc(desc->imagedesc);
        if (desc->lod != NULL && once(desc->lod)) {
            pc_swap32(&desc->lod->minFilt);
            pc_swapf(&desc->lod->LODBias);
            pc_swap32(&desc->lod->max_anisotropy);
        }
        pc_swap_tlutdesc(desc->tlutdesc);
        pc_swap_tobjtevdesc(desc->tev);
    }
}

/* --- Polygons ------------------------------------------------------------- */

static void swap_vtxdesclist(HSD_VtxDescList* v)
{
    if (v == NULL || !once(v)) {
        return;
    }
    for (;;) {
        pc_swap32(&v->attr);
        if (v->attr == GX_VA_NULL) {
            break;
        }
        pc_swap32(&v->attr_type);
        pc_swap32(&v->comp_cnt);
        pc_swap32(&v->comp_type);
        pc_swap16(&v->stride);
        /* vertex arrays stay in GX order for the renderer */
        v++;
    }
}

void pc_swap_pobjdesc(HSD_PObjDesc* desc)
{
    for (; desc != NULL; desc = desc->next) {
        if (!once(desc)) {
            return;
        }
        pc_swap16(&desc->flags);
        pc_swap16(&desc->n_display);
        swap_vtxdesclist(desc->verts);
        /* display lists stay in GX order for the renderer */
        switch (pobj_type(desc)) {
        case POBJ_SHAPEANIM: {
            HSD_ShapeSetDesc* s = desc->u.shape_set;
            if (s != NULL && once(s)) {
                pc_swap16(&s->flags);
                pc_swap16(&s->nb_shape);
                pc_swap32(&s->nb_vertex_index);
                pc_swap32(&s->nb_normal_index);
                swap_vtxdesclist(s->vertex_desc);
                swap_vtxdesclist(s->normal_desc);
            }
            break;
        }
        case POBJ_ENVELOPE: {
            HSD_EnvelopeDesc** list = desc->u.envelope_p;
            if (list != NULL && once(list)) {
                for (; *list != NULL; list++) {
                    HSD_EnvelopeDesc* e = *list;
                    if (!once(e)) {
                        continue;
                    }
                    for (; e->joint != NULL; e++) {
                        pc_swapf(&e->weight);
                    }
                }
            }
            break;
        }
        default:
            break;
        }
    }
}

/* --- SIS text messages ----------------------------------------------------
 * A message is a byte stream: opcodes below 0x20 with 0-4 bytes of
 * parameters (some 16-bit), 16-bit glyph codes otherwise, terminated by
 * opcode 0. Opcodes 8 (jump) and 9 (call) carry a relocated pointer to
 * another message. See hsd_3A76.c for the interpreter. */
void pc_swap_sis_message(u8* p)
{
    if (p == NULL || !once(p)) {
        return;
    }
    for (;;) {
        u8 op = *p;
        if (op >= 0x20) {
            pc_swap16(p);
            p += 2;
            continue;
        }
        switch (op) {
        case 0:
            return;
        case 5:
            pc_swap16(p + 1);
            p += 3;
            break;
        case 6:
        case 7:
        case 10:
        case 14:
            pc_swap16(p + 1);
            pc_swap16(p + 3);
            p += 5;
            break;
        case 12:
            p += 4;
            break;
        case 8:
            pc_swap_sis_message(*(u8**) (p + 1));
            return;
        case 9:
            pc_swap_sis_message(*(u8**) (p + 1));
            p += 5;
            break;
        default:
            p += 1;
            break;
        }
    }
}

/* --- Animation sets ------------------------------------------------------
 * Most animation descriptors are pointer-only (relocated already) and lead
 * to AObj/FObj descriptors that the animation loaders swap. The two with
 * scalar fields are handled here. */

void pc_swap_animjoint(HSD_AnimJoint* anim)
{
    if (anim == NULL || !once(anim)) {
        return;
    }
    pc_swap32(&anim->flags);
}

void pc_swap_texanim(HSD_TexAnim* anim)
{
    for (; anim != NULL; anim = anim->next) {
        u32 i;
        if (!once(anim)) {
            return;
        }
        pc_swap32(&anim->id);
        pc_swap16(&anim->n_imagetbl);
        pc_swap16(&anim->n_tluttbl);
        if (anim->imagetbl != NULL) {
            for (i = 0; i < anim->n_imagetbl; i++) {
                swap_imagedesc(anim->imagetbl[i]);
            }
        }
        if (anim->tluttbl != NULL) {
            for (i = 0; i < anim->n_tluttbl; i++) {
                pc_swap_tlutdesc(anim->tluttbl[i]);
            }
        }
    }
}
