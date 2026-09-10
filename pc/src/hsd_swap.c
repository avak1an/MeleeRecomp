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
bool pc_swap_ptr_ok(const void* ptr)
{
    uintptr_t p = (uintptr_t) ptr;
    return ptr == NULL ||
           (p >= (uintptr_t) pc_mem_base() && p < (uintptr_t) pc_mem_base() + pc_mem_size());
}

static bool once(const void* ptr)
{
    uintptr_t p = (uintptr_t) ptr;
    u32 i;
    if (pc_debug_watch != NULL) {
        char tag[32];
        snprintf(tag, sizeof(tag), "once %p", ptr);
        pc_debug_watch_check(tag);
        if (ptr == (const void*) pc_debug_watch) {
            fprintf(stderr, "[pc] watch %p: once() called (%s)\n", ptr,
                    pc_swap_is_done(ptr) ? "already registered" : "first time");
            pc_print_backtrace();
        }
    }
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

bool pc_swap_once(const void* p)
{
    return once(p);
}

bool pc_swap_is_done(const void* ptr)
{
    uintptr_t p = (uintptr_t) ptr;
    u32 i;
    if (reg == NULL) {
        return false;
    }
    for (i = reg_hash(p);; i = (i + 1) & (REG_SIZE - 1)) {
        if (reg[i] == 0) {
            return false;
        }
        if (reg[i] == p) {
            return true;
        }
    }
}

/* Pointer slots relocated by HSD_ArchiveParse. A few descriptor fields hold
 * either a pointer or a plain integer (HSD_AObjDesc::obj_id); only the
 * latter must be swapped, and this set tells them apart. */
static uintptr_t* reloc_reg;
static u32 reloc_used;

/* Relocation targets (the objects the slots point at), kept as (target,
 * slot) pairs so a swapper can find where the next referenced object
 * starts after a given address. Sorted lazily. */
#define MAX_RELOC_TARGETS (1u << 18)
static struct {
    uintptr_t target, slot;
} * reloc_targets;
static u32 reloc_target_count;
static bool reloc_targets_sorted;

static int cmp_target(const void* a, const void* b)
{
    uintptr_t ta = *(const uintptr_t*) a, tb = *(const uintptr_t*) b;
    return ta < tb ? -1 : ta > tb ? 1 : 0;
}

void pc_swap_note_reloc_target(const void* slot, const void* target)
{
    if (reloc_targets == NULL) {
        reloc_targets = calloc(MAX_RELOC_TARGETS, sizeof(*reloc_targets));
    }
    if (reloc_target_count < MAX_RELOC_TARGETS) {
        reloc_targets[reloc_target_count].target = (uintptr_t) target;
        reloc_targets[reloc_target_count].slot = (uintptr_t) slot;
        reloc_target_count++;
        reloc_targets_sorted = false;
    }
}

/* Extents of the parsed archives, so an unsized block never runs past the
 * end of its file into whatever was allocated after it. */
#define MAX_ARCHIVES 256
static struct {
    uintptr_t lo, hi;
} archives[MAX_ARCHIVES];
static u32 archive_count;

void pc_swap_note_archive(const void* base, size_t size)
{
    uintptr_t lo = (uintptr_t) base;
    u32 i;
    for (i = 0; i < archive_count; i++) {
        if (archives[i].lo == lo) {
            archives[i].hi = lo + size;
            return;
        }
    }
    if (archive_count < MAX_ARCHIVES) {
        archives[archive_count].lo = lo;
        archives[archive_count].hi = lo + size;
        archive_count++;
    }
}

/// True when p is the start of a parsed archive that was not forgotten.
bool pc_swap_is_archive(const void* p)
{
    u32 i;
    for (i = 0; i < archive_count; i++) {
        if (archives[i].lo == (uintptr_t) p) {
            return true;
        }
    }
    return false;
}

const void* pc_swap_next_object(const void* start, const void* limit)
{
    uintptr_t s = (uintptr_t) start, l = (uintptr_t) limit;
    u32 lo, hi;
    for (lo = 0; lo < archive_count; lo++) {
        if (s >= archives[lo].lo && s < archives[lo].hi && archives[lo].hi < l) {
            l = archives[lo].hi;
            limit = (const void*) l;
        }
    }
    if (reloc_targets == NULL || reloc_target_count == 0) {
        return limit;
    }
    if (!reloc_targets_sorted) {
        qsort(reloc_targets, reloc_target_count, sizeof(*reloc_targets), cmp_target);
        reloc_targets_sorted = true;
    }
    /* first target > start */
    lo = 0;
    hi = reloc_target_count;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (reloc_targets[mid].target <= s) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < reloc_target_count && reloc_targets[lo].target < l) {
        return (const void*) reloc_targets[lo].target;
    }
    return limit;
}

/* MELEE_ARCHIVE_CHECK=1: once per frame, verify that every relocated
 * pointer slot of the parsed archives still holds the value the parser
 * wrote. A slot that changed was overwritten by something else (or its
 * archive was freed and the memory reused). Reports the first few. */
void pc_swap_verify_relocs(const char* tag)
{
    static int enabled = -1;
    static int reports;
    u32 i;
    if (enabled < 0) {
        enabled = getenv("MELEE_ARCHIVE_CHECK") != NULL;
    }
    if (!enabled || reloc_targets == NULL || reports >= 20) {
        return;
    }
    /* an archive whose header no longer states its size was freed through
     * a path without a hook and its memory reused: drop it silently */
    for (i = 0; i < archive_count;) {
        if (*(const u32*) archives[i].lo != (u32) (archives[i].hi - archives[i].lo)) {
            pc_swap_forget_range((void*) archives[i].lo, archives[i].hi - archives[i].lo);
        } else {
            i++;
        }
    }
    for (i = 0; i < reloc_target_count; i++) {
        const uintptr_t* slot = (const uintptr_t*) reloc_targets[i].slot;
        if (*slot != reloc_targets[i].target) {
            fprintf(stderr, "[pc] archive check (%s): slot %p holds %08x, parser wrote %08x\n", tag,
                    (const void*) slot, (unsigned) *slot, (unsigned) reloc_targets[i].target);
            reloc_targets[i].target = *slot; /* report each change once */
            if (++reports >= 20) {
                fprintf(stderr, "[pc] archive check: further reports suppressed\n");
                break;
            }
        }
    }
}

void pc_swap_note_reloc_slot(const void* ptr)
{
    uintptr_t p = (uintptr_t) ptr;
    u32 i;
    if (reloc_reg == NULL) {
        reloc_reg = (uintptr_t*) calloc(REG_SIZE, sizeof(uintptr_t));
    }
    if (reloc_used > REG_SIZE / 2) {
        fprintf(stderr, "[pc] relocation registry full (%u entries)\n", reloc_used);
        pc_exit(6);
    }
    for (i = reg_hash(p);; i = (i + 1) & (REG_SIZE - 1)) {
        if (reloc_reg[i] == 0) {
            reloc_reg[i] = p;
            reloc_used++;
            return;
        }
        if (reloc_reg[i] == p) {
            return;
        }
    }
}

bool pc_swap_is_reloc_slot(const void* ptr)
{
    uintptr_t p = (uintptr_t) ptr;
    u32 i;
    if (reloc_reg == NULL) {
        return false;
    }
    for (i = reg_hash(p);; i = (i + 1) & (REG_SIZE - 1)) {
        if (reloc_reg[i] == 0) {
            return false;
        }
        if (reloc_reg[i] == p) {
            return true;
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
    {
        u32 n = 0;
        for (i = 0; i < archive_count; i++) {
            if (!(archives[i].lo >= lo && archives[i].lo < hi)) {
                archives[n++] = archives[i];
            }
        }
        archive_count = n;
    }
    if (reloc_targets != NULL) {
        u32 n = 0;
        for (i = 0; i < reloc_target_count; i++) {
            if (!(reloc_targets[i].slot >= lo && reloc_targets[i].slot < hi)) {
                reloc_targets[n++] = reloc_targets[i];
            }
        }
        reloc_target_count = n;
    }
    if (reloc_reg != NULL) {
        uintptr_t* old = reloc_reg;
        reloc_reg = (uintptr_t*) calloc(REG_SIZE, sizeof(uintptr_t));
        reloc_used = 0;
        for (i = 0; i < REG_SIZE; i++) {
            if (old[i] != 0 && !(old[i] >= lo && old[i] < hi)) {
                pc_swap_note_reloc_slot((const void*) old[i]);
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

/// A whole joint descriptor tree (child/next), for trees the game reads
/// directly instead of loading them (fighter pose blends).
void pc_swap_joint_tree(HSD_Joint* joint)
{
    for (; joint != NULL; joint = joint->next) {
        if (!pc_swap_ptr_ok(joint) || pc_swap_is_done(joint)) {
            return;
        }
        pc_swap_joint(joint);
        pc_swap_joint_tree(joint->child);
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
    if (spline->numcv < 0 || spline->numcv > 4096) {
        fprintf(stderr, "[pc] swap: implausible spline with %d control points, skipping\n", spline->numcv);
        return;
    }
    /* numcv control points (a Bezier spline stores three per segment plus
     * one), numcv cumulative segment lengths and numcv - 1 segment
     * polynomials of five coefficients. */
    if (spline->cv != NULL && once(spline->cv)) {
        int ncv = spline->type == 1 ? 3 * (spline->numcv - 1) + 1 : spline->numcv;
        for (i = 0; i < ncv; i++) {
            swap_vec3(&spline->cv[i]);
        }
    }
    pc_debug_watch_check("spline cv");
    if (spline->segLength != NULL && once(spline->segLength)) {
        pc_swap32_range(spline->segLength, (size_t) spline->numcv * sizeof(f32));
    }
    pc_debug_watch_check("spline segLength");
    if (spline->segPoly != NULL && spline->numcv > 1 && once(spline->segPoly)) {
        pc_swap32_range(spline->segPoly, (size_t) (spline->numcv - 1) * 5 * sizeof(f32));
    }
    pc_debug_watch_check("spline segPoly");
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
    /* obj_id is either an object id or a relocated HSD_Joint pointer */
    if (!pc_swap_is_reloc_slot(&desc->obj_id)) {
        pc_swap32(&desc->obj_id);
    }
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

/// Image descriptors reached outside a TObj (sprites, EFB copies).
void pc_swap_imagedesc(HSD_ImageDesc* im)
{
    swap_imagedesc(im);
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
    HSD_VtxDescList* head = v;
    if (v == NULL) {
        return;
    }
    /* Checked per entry, not per list: a polygon's list may start in the
     * middle of another polygon's (shared tails, e.g. MnSlMap.usd). Once an
     * entry has been swapped the rest of its list has been too. */
    for (; once(v);) {
        pc_swap32(&v->attr);
        if (v->attr == GX_VA_NULL) {
            break;
        }
        if (v->attr > GX_VA_MAX_ATTR || v - head >= 32) {
            /* not a vertex attribute list (or one already in host order):
             * stop before the walk runs into the data that follows */
            fprintf(stderr, "[pc] vertex attribute list at %p: entry %d has attribute %08x, stopping\n",
                    (void*) head, (int) (v - head), (unsigned) v->attr);
            pc_print_backtrace();
            pc_swap32(&v->attr);
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

/* --- Particle data banks --------------------------------------------------
 * The particle system (particle.c, psInitDataBankLocate) uses its own raw
 * bank format: a command bank of offsets to HSD_PSCmdList headers, a
 * texture bank of HSD_PSTexGroup descriptors and a form bank of
 * HSD_PSFormGroup tables. Everything is 32-bit words except the version and
 * the palette fields; the command bytecode after each header is read
 * byte-wise by the interpreter and is left alone. */
static void swap_ps_cmdlist(u8* cmd)
{
    if (!once(cmd)) {
        return;
    }
    pc_swap16_range(cmd, 8);       /* type, texGroup, genLife, life */
    pc_swap32_range(cmd + 8, 0x34); /* kind and the twelve floats */
}

void pc_swap_ps_banks(void* cmdBank, void* texBank, s32* formBank)
{
    u32* w = (u32*) cmdBank;
    u32 num_groups = 0;
    u32 i;

    if (w != NULL) {
        bool fresh = once(w);
        if (pc_debug_gx) {
            fprintf(stderr, "[gx] particle banks cmd=%p tex=%p form=%p words %08x %08x %08x%s\n",
                    cmdBank, texBank, (void*) formBank, w[0], w[1], w[2],
                    fresh ? "" : " (seen before)");
        }
        if (!fresh) {
            w = NULL;
        }
    }
    if (w != NULL) {
        u16 version;
        pc_swap16(w);
        version = *(u16*) w;
        pc_swap32(&w[1]);
        pc_swap32(&w[2]);
        if (version == 0) {
            u32 n = w[1];
            for (i = 1; i < n; i++) {
                pc_swap32(&w[2 + i]);
            }
            for (i = 0; i < n; i++) {
                if (w[2 + i] != 0) {
                    swap_ps_cmdlist((u8*) cmdBank + w[2 + i]);
                }
            }
        } else if (version >= 0x40 && version < 0x44) {
            u32 nb = w[2];
            for (i = 0; i < nb; i++) {
                pc_swap32(&w[3 + i]);
            }
            for (i = 0; i < nb; i++) {
                if (w[3 + i] != 0) {
                    swap_ps_cmdlist((u8*) cmdBank + w[3 + i]);
                }
            }
        }
    }

    if (pc_debug_gx && cmdBank != NULL) {
        fprintf(stderr, "[gx]   after cmd swap: %08x\n", *(u32*) cmdBank);
    }
    w = (u32*) texBank;
    if (w != NULL && once(w)) {
        pc_swap32(&w[0]);
        num_groups = w[0];
        for (i = 1; i <= num_groups; i++) {
            pc_swap32(&w[i]);
        }
        for (i = 1; i <= num_groups; i++) {
            u32* g;
            u32 num, fmt, entries;
            u16 palnum, palflag;
            if (w[i] == 0) {
                continue;
            }
            g = (u32*) ((u8*) texBank + w[i]);
            pc_swap32_range(g, 20); /* num fmt tlutfmt width height */
            pc_swap16_range(g + 5, 4); /* palnum palflag */
            num = g[0];
            fmt = g[1];
            palnum = ((u16*) (g + 5))[0];
            palflag = ((u16*) (g + 5))[1];
            entries = num;
            if (fmt == 8 || fmt == 9 || fmt == 10) {
                entries += (palflag & 1) ? 1 : (palnum != 0 ? palnum : num);
            }
            pc_swap32_range(g + 6, entries * 4);
        }
    } else if (w != NULL) {
        num_groups = w[0];
    }

    if (pc_debug_gx && cmdBank != NULL) {
        fprintf(stderr, "[gx]   after tex swap: %08x\n", *(u32*) cmdBank);
    }
    if (formBank != NULL && once(formBank)) {
        pc_swap32(&formBank[0]);
        for (i = 1; i <= num_groups; i++) {
            pc_swap32(&formBank[i]);
        }
        for (i = 1; i <= num_groups; i++) {
            u32* fg;
            if (formBank[i] == 0) {
                continue;
            }
            fg = (u32*) ((u8*) formBank + formBank[i]);
            pc_swap32(&fg[0]);
            pc_swap32_range(fg + 1, fg[0] * 4);
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
