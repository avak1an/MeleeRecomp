/**
 * @file game_swap.c
 * Byte-swapping of the game's own data structures loaded from disc. The HSD
 * descriptors these tables point to (joints, cameras, lights, fog, splines,
 * animations) are swapped by the engine loader hooks in hsd_swap.c; this
 * file handles the scalar fields of the game-side tables themselves.
 */
#include "pc_runtime.h"

#include <stdio.h>

#include <pc_endian.h>
#include <pc_game_swap.h>
#include <pc_hsd_swap.h>

#include <melee/gr/types.h>
#include <melee/mp/types.h>
#include <melee/ft/types.h>
#include <melee/ft/dobjlist.h>
#include <melee/ft/fighter.h>
#include <melee/lb/lbanim.h>
#include <melee/lb/types.h>
#include <melee/it/forward.h>
#include <melee/it/it_3F14.h>
#include <melee/it/types.h>
#include <melee/ft/kinds/ftCommon/types.h>

/* Counts read from disc data drive swap loops; a mis-identified table would
 * otherwise turn into a huge swap that corrupts memory. */
static bool sane_count(int n, int max, const char* what)
{
    if (n < 0 || n > max) {
        fprintf(stderr, "[pc] swap: implausible %s count %d (max %d), skipping\n", what, n, max);
        return false;
    }
    return true;
}

/* --- Stage ------------------------------------------------------------------ */

static void swap_ground_param(GroundParam* p)
{
    int i;
    if (p == NULL || !pc_swap_once(p)) {
        return;
    }
    pc_swapf(&p->y);
    pc_swap16(&p->x4);
    pc_swap16(&p->x8);
    pc_swap16(&p->xA);
    pc_swap32(&p->xC);
    pc_swap32(&p->x10);
    pc_swap32(&p->x14);
    pc_swap32_range(&p->x18, 5 * 4); /* x18 .. x28 */
    pc_swap16(&p->x2E);
    pc_swap32_range(&p->x30, 3 * 4);
    pc_swap32_range(&p->x3C, 4 * 4);
    pc_swap32(&p->x4C_fixed_cam);
    pc_swap32_range(&p->x50, 6 * 4);
    pc_swap16(&p->x68);
    pc_swap16_range(p->x6A, sizeof(p->x6A));
    pc_swap32(&p->stage_param_count);
    if (p->stage_params != NULL && sane_count(p->stage_param_count, 64, "stage param") &&
        pc_swap_once(p->stage_params)) {
        for (i = 0; i < p->stage_param_count; i++) {
            StageParam* s = &p->stage_params[i];
            pc_swap32(&s->stkind);
            pc_swap32(&s->x4);
            pc_swap32(&s->x8);
            pc_swap32(&s->xC);
            pc_swap32(&s->x10);
            pc_swap16(&s->x14);
            pc_swap16(&s->x16);
            pc_swap16(&s->x18);
            pc_swap16_range(s->x1A, sizeof(s->x1A));
        }
    }
    /* the GXColor fields are bytes */
}

static void swap_map_coll(MapCollData* c)
{
    int i;
    if (c == NULL || !pc_swap_once(c)) {
        return;
    }
    pc_swap32(&c->vert_count);
    pc_swap32(&c->line_count);
    pc_swap16_range(&c->floor_start, 10 * 2);
    pc_swap32(&c->joint_count);
    /* x2C is not part of the on-disc struct (the next block starts there) */
    if (c->verts != NULL && sane_count(c->vert_count, 8192, "coll vert") && pc_swap_once(c->verts)) {
        pc_swap32_range(c->verts, (size_t) c->vert_count * sizeof(Vec2));
    }
    pc_debug_watch_check("coll verts");
    if (c->lines != NULL && sane_count(c->line_count, 8192, "coll line") && pc_swap_once(c->lines)) {
        pc_swap16_range(c->lines, (size_t) c->line_count * sizeof(MapLine));
    }
    pc_debug_watch_check("coll lines");
    if (c->joints != NULL && sane_count(c->joint_count, 512, "coll joint") && pc_swap_once(c->joints)) {
        for (i = 0; i < c->joint_count; i++) {
            MapJoint* j = &c->joints[i];
            pc_swap16_range(&j->floor_start, 10 * 2);
            pc_swap32_range(&j->left_bound, 4 * 4);
            pc_swap16(&j->vtx_start);
            pc_swap16(&j->vtx_count);
        }
    }
}

static void swap_map_head(UnkStageDat* h)
{
    s32 i;
    if (h == NULL || !pc_swap_once(h)) {
        return;
    }
    pc_swap32(&h->unk4);
    pc_swap32(&h->unkC);
    pc_swap32(&h->unk14);
    pc_swap32(&h->unk1C);
    pc_swap32(&h->unk24);
    pc_swap32(&h->unk2C);
    if (pc_debug_gx) {
        fprintf(stderr,
                "[gx] map_head %p: gp=%p/%d models=%p/%d splines=%p/%d lights=%p/%d "
                "shadows=%p/%d x28=%p/%d\n",
                (void*) h, h->unk0, h->unk4, (void*) h->unk8, h->unkC, (void*) h->unk10,
                h->unk14, h->unk18, h->unk1C, (void*) h->unk20, h->unk24, (void*) h->unk28,
                h->unk2C);
        if (h->unk28 != NULL) {
            for (i = 0; i < h->unk2C && i < 8; i++) {
                fprintf(stderr, "[gx]   x28[%d] = %p\n", i, (void*) h->unk28[i]);
            }
        }
    }

    /* general points: {HSD_Joint* joint; s16* pairs; s32 pair_count}, the
     * pairs being (joint index, general point id) */
    if (h->unk0 != NULL && pc_swap_once(h->unk0)) {
        for (i = 0; i < h->unk4; i++) {
            struct {
                void* joint;
                s16* pairs;
                s32 pair_count;
            }* e = (void*) ((u8*) h->unk0 + i * 12);
            pc_swap32(&e->pair_count);
            if (e->pairs != NULL && sane_count(e->pair_count, 512, "general point pair") &&
                pc_swap_once(e->pairs)) {
                pc_swap16_range(e->pairs, (size_t) e->pair_count * 2 * sizeof(s16));
            }
        }
    }
    pc_debug_watch_check("map_head general points");
    /* model set entries: the joints/animations/camera/fog/lights they point
     * to go through the engine loaders; only the two counts here */
    if (h->unk8 != NULL && pc_swap_once(h->unk8)) {
        for (i = 0; i < h->unkC; i++) {
            struct UnkStageDat_x8_t* e = &h->unk8[i];
            pc_swap32(&e->unk24);
            pc_swap32(&e->x30);
            if (e->unk20 != NULL && sane_count(e->unk24, 512, "stage joint") && pc_swap_once(e->unk20)) {
                pc_swap16_range(e->unk20, (size_t) e->unk24 * sizeof(GrJoint));
            }
            pc_debug_watch_check("map_head model entry");
        }
    }
    pc_debug_watch_check("map_head models");
    if (h->unk10 != NULL && pc_swap_once(h->unk10)) {
        for (i = 0; i < h->unk14; i++) {
            pc_swap_spline(h->unk10[i]);
        }
    }
    pc_debug_watch_check("map_head splines");
    /* light override entries {HSD_LightDesc* desc; u8 a:1, b:1, c:1; pad[3]}
     * are read with console bit order through a TARGET_PC definition in
     * ground.c, because unk1C over-counts the array (the game reads past its
     * end into the tables that follow) so the bytes cannot be swapped safely. */
    pc_debug_watch_check("map_head lights");
    if (h->unk20 != NULL && pc_swap_once(h->unk20)) {
        for (i = 0; i < h->unk24; i++) {
            /* pointer, then a bit-field byte */
            pc_swap_bits8((unsigned char*) &h->unk20[i] + 4);
        }
    }
    /* x28: a mix of joint and material descriptors that grDatFiles_801C6228
     * flags before their loaders run; it sets the bit in disc byte order on
     * PC, so nothing to do here. */
}

/* A stage's extra map_head (Pokemon Stadium loads one per transformation)
 * goes through the same swap; once-guarded. */
void pc_swap_map_head(struct UnkStageDat* map_head)
{
    swap_map_head(map_head);
}

void pc_swap_stage_data(struct UnkStageDat* map_head, struct GroundParam* param,
                        struct MapCollData* coll, void** itemdata)
{
    pc_debug_watch_check("before stage swap");
    swap_map_head(map_head);
    pc_debug_watch_check("map_head");
    swap_ground_param(param);
    pc_debug_watch_check("ground param");
    swap_map_coll(coll);
    pc_debug_watch_check("coll data");
    if (itemdata != NULL && pc_swap_once(itemdata)) {
        int i;
        for (i = 0; itemdata[i] != NULL; i++) {
            if (pc_swap_once(itemdata[i])) {
                pc_swap32(itemdata[i]); /* s32 kind, then an Article pointer */
            }
        }
    }
    pc_debug_watch_check("itemdata");
}

/* --- Fighter ----------------------------------------------------------------
 * The "ftData" root of Pl*.dat: attributes, animation tables, hurt boxes and
 * assorted tables of floats. Tables whose length is not known yet (x1C,
 * x28, x48, x50, ext_attr) are left alone until code that reads them is
 * reached. */

static void swap_anim_table(struct Fighter_WaitAnimData* t, int count)
{
    int i;
    if (t == NULL || !pc_swap_once(t)) {
        return;
    }
    for (i = 0; i < count; i++) {
        pc_swap32(&t[i].x4);
        pc_swap32(&t[i].x8);
        pc_swap32(&t[i].x10_animCurrFlags);
        pc_swap32(&t[i].x14);
    }
}

/* A parts descriptor {u32 model_num; vis_table[costume][4]}: per costume
 * four visibility tables of model_num {int n; TempS* e} entries, each TempS
 * being {int n; u8* dobj_indices}. The count is swapped once (the once()
 * registry keys on the descriptor). */
static void swap_parts_desc(struct FtPartsDesc* desc, int costumes)
{
    int c, k, m, j;
    if (desc == NULL) {
        return; /* the caller's once() covers the descriptor */
    }
    pc_swap32(&desc->model_num);
    if (desc->model_num > 64) {
        return; /* not a descriptor after all */
    }
    if (desc->vis_table != NULL && pc_swap_ptr_ok(desc->vis_table) && pc_swap_once(desc->vis_table)) {
        for (c = 0; c < costumes; c++) {
            for (k = 0; k < 4; k++) {
                struct FtPartsVisLookup* lookup = desc->vis_table[c][k];
                if (lookup == NULL || !pc_swap_ptr_ok(lookup) || !pc_swap_once(lookup)) {
                    continue;
                }
                for (m = 0; m < (int) desc->model_num; m++) {
                    pc_swap32(&lookup[m].x0);
                    if (lookup[m].x4 != NULL && pc_swap_ptr_ok(lookup[m].x4) && pc_swap_once(lookup[m].x4)) {
                        for (j = 0; j < lookup[m].x0; j++) {
                            pc_swap32(&lookup[m].x4[j].x0);
                        }
                    }
                }
            }
        }
    }
}

/* A parts descriptor read by ftParts_8007487C that no loader swapped yet
 * (Kirby's copy hats come in several layouts): the count is swapped when
 * it does not look like a small count already, and the visibility tables
 * of costume 0 and of the costume in use are swapped once each. The
 * fighter's own descriptor is registered by pc_swap_ftdata under the same
 * address and is left alone here. */
void pc_swap_parts_desc_lazy(struct FtPartsDesc* desc, int costume_id)
{
    int c, k, m, j;
    if (desc == NULL || !pc_swap_ptr_ok(desc) || !pc_swap_once(desc)) {
        return;
    }
    if (desc->model_num > 64) {
        pc_swap32(&desc->model_num);
    }
    if (desc->model_num > 64 || desc->vis_table == NULL || !pc_swap_ptr_ok(desc->vis_table)) {
        return;
    }
    for (c = 0; c <= costume_id && c < 8; c++) {
        if (c != 0 && c != costume_id) {
            continue;
        }
        for (k = 0; k < 4; k++) {
            struct FtPartsVisLookup* lookup = desc->vis_table[c][k];
            if (lookup == NULL || !pc_swap_ptr_ok(lookup) || !pc_swap_once(lookup)) {
                continue;
            }
            for (m = 0; m < (int) desc->model_num; m++) {
                pc_swap32(&lookup[m].x0);
                if (lookup[m].x4 != NULL && pc_swap_ptr_ok(lookup[m].x4) && pc_swap_once(lookup[m].x4)) {
                    for (j = 0; j < lookup[m].x0; j++) {
                        pc_swap32(&lookup[m].x4[j].x0);
                    }
                }
            }
        }
    }
}

/* Kirby's copy hats (PlKb*.dat): {HSD_Joint* hat_joint; FtPartsDesc desc;
 * ftDynamics* hat_dynamics[5]}, one costume. Swapped when a hat is put on. */
static void swap_ft_dynamics(struct ftDynamics* dyn)
{
    int i;
    if (dyn == NULL || !pc_swap_ptr_ok(dyn) || !pc_swap_once(dyn)) {
        return;
    }
    pc_swap32(&dyn->dynamicsNum);
    pc_swap32(&dyn->x4);
    if (dyn->ftDynamicBones != NULL && pc_swap_ptr_ok(dyn->ftDynamicBones) &&
        sane_count(dyn->dynamicsNum, Ft_Dynamics_NumMax, "hat dynamics") && pc_swap_once(dyn->ftDynamicBones)) {
        for (i = 0; i < dyn->dynamicsNum && i < Ft_Dynamics_NumMax; i++) {
            struct BoneDynamicsDesc* b = &dyn->ftDynamicBones->array[i];
            pc_swap32(&b->bone_id);
            pc_swap_dynamics_desc(&b->dyn_desc);
        }
    }
}

/* The five slots after the parts descriptor mean different things per hat
 * (masks, joints, lookups, colours); only the ones the code reads as bone
 * dynamics are swapped: Kirby's own hat's slot 1 and Pichu's slot 4. */
void pc_swap_kirby_hat(struct KirbyHatStruct* hat, int kind)
{
    if (hat == NULL || !pc_swap_ptr_ok(hat) || !pc_swap_once(hat)) {
        return;
    }
    /* the parts descriptor is swapped by pc_swap_parts_desc_lazy when
     * ftParts_8007487C reads it: the hat structs differ per kind and some
     * of them start with the descriptor itself */
    if (kind == FTKIND_KIRBY) {
        swap_ft_dynamics(hat->hat_dynamics[1]);
    } else if (kind == FTKIND_PICHU) {
        swap_ft_dynamics(hat->hat_dynamics[4]);
    }
}

void pc_swap_ftdata(struct ftData* d, int anim_count, int alt_anim_count, int costumes)
{
    int i;
    if (d == NULL || !pc_swap_once(d)) {
        return;
    }
    if (d->x0 != NULL && pc_swap_once(d->x0)) {
        pc_swap32_range(d->x0, 0x180); /* floats and ints, then one byte */
    }
    if (d->x8 != NULL && pc_swap_once(d->x8)) {
        swap_parts_desc(&d->x8->x0, costumes);
        pc_swap32(&d->x8->x8.x8);
        /* per costume: x8 texture-object indices (u16) */
        if (d->x8->x8.xC != NULL && pc_swap_once(d->x8->x8.xC)) {
            for (i = 0; i < costumes; i++) {
                u16* idx = d->x8->x8.xC[i];
                if (idx != NULL && pc_swap_once(idx)) {
                    pc_swap16_range(idx, (size_t) d->x8->x8.x8 * sizeof(u16));
                }
            }
        }
    }
    swap_anim_table(d->xC, anim_count);
    swap_anim_table(d->x14, alt_anim_count);
    /* x24: wait animation table {int anim_id; int weight}, anim_id -1 ends */
    if (d->x24 != NULL && pc_swap_once(d->x24)) {
        s32* w = (s32*) d->x24;
        for (i = 0; i < 64; i += 2) {
            pc_swap32(&w[i]);
            if (w[i] == -1) {
                break;
            }
            pc_swap32(&w[i + 1]);
        }
    }
    /* part animation slots (five): {u16 x0; u16 x2; u8* parts; HSD_AnimJoint** anims} */
    if (d->x1C != NULL && pc_swap_once(d->x1C)) {
        for (i = 0; i < 5; i++) {
            if (d->x1C[i] != NULL && pc_swap_once(d->x1C[i])) {
                pc_swap16(&d->x1C[i]->x0);
                pc_swap16(&d->x1C[i]->x2);
            }
        }
    }
    if (d->x20 != NULL && pc_swap_once(d->x20)) {
        pc_swapf(&d->x20->x8);
        /* x0: a table of pose joint trees the guard / special blends read
         * as descriptors (never loaded as JObjs); the table ends where the
         * next referenced object starts */
        if (d->x20->x0 != NULL && pc_swap_ptr_ok(d->x20->x0) && pc_swap_once(d->x20->x0)) {
            HSD_Joint** tbl = d->x20->x0;
            const u8* end = (const u8*) pc_swap_next_object(tbl, (const u8*) tbl + 16 * sizeof(*tbl));
            int k;
            for (k = 0; (const u8*) &tbl[k] + sizeof(*tbl) <= end && k < 16; k++) {
                if (pc_swap_is_reloc_slot(&tbl[k])) {
                    pc_swap_joint_tree(tbl[k]);
                }
            }
            if (getenv("MELEE_TRACE_SWAP") != NULL) {
                fprintf(stderr, "[pc] ftData %p pose table %p: %d entries (end %p)\n", (void*) d, (void*) tbl, k,
                        (const void*) end);
            }
        }
    }
    if (d->x2C != NULL && pc_swap_once(d->x2C)) {
        pc_swap32(&d->x2C->dynamicsNum);
        pc_swap32(&d->x2C->x4);
        /* dynamics hit entries {int bone; Vec3; float} */
        if (d->x2C->x8 != NULL && sane_count(d->x2C->x4, 32, "dynamics hit") && pc_swap_once(d->x2C->x8)) {
            pc_swap32_range(d->x2C->x8, (size_t) d->x2C->x4 * sizeof(struct ftData_x38));
        }
        /* bone dynamics: {bone_id; DynamicsDesc} per entry */
        if (d->x2C->ftDynamicBones != NULL && sane_count(d->x2C->dynamicsNum, Ft_Dynamics_NumMax, "dynamics") &&
            pc_swap_once(d->x2C->ftDynamicBones)) {
            for (i = 0; i < d->x2C->dynamicsNum && i < Ft_Dynamics_NumMax; i++) {
                struct BoneDynamicsDesc* b = &d->x2C->ftDynamicBones->array[i];
                pc_swap32(&b->bone_id);
                pc_swap_dynamics_desc(&b->dyn_desc);
            }
        }
    }
    if (d->x30 != NULL && pc_swap_once(d->x30)) {
        pc_swap32(&d->x30->count);
        if (d->x30->inits != NULL && sane_count(d->x30->count, 32, "hurtbox") && pc_swap_once(d->x30->inits)) {
            pc_swap32_range(d->x30->inits, (size_t) d->x30->count * sizeof(ftHurtboxInit));
        }
    }
    if (d->x34 != NULL && pc_swap_once(d->x34)) {
        pc_swap32_range(d->x34, 2 * 4);
    }
    if (d->x38 != NULL && pc_swap_once(d->x38)) {
        pc_swap32_range(d->x38, 2 * 5 * 4); /* two entries (Fighter::x1614) */
    }
    if (d->x3C != NULL && pc_swap_once(d->x3C)) {
        pc_swap32_range(d->x3C, 6 * 4);
    }
    if (d->x40 != NULL && pc_swap_once(d->x40)) {
        pc_swap32_range(d->x40, 12 * 4);
    }
    if (d->x44 != NULL && pc_swap_once(d->x44)) {
        pc_swap16_range(d->x44, 6 * 2);
        pc_swap32_range(&d->x44->unkC, 4 * 4);
    }
    /* sound table: fourteen words that are either ints or relocated
     * pointers to {int num; s32* ids} lists (which ones is not fully known) */
    if (d->x4C_sfx != NULL && pc_swap_once(d->x4C_sfx)) {
        u32* w = (u32*) d->x4C_sfx;
        for (i = 0; i < 14; i++) {
            if (pc_swap_is_reloc_slot(&w[i])) {
                FtSFXArr* arr = (FtSFXArr*) w[i];
                if (arr != NULL && pc_swap_once(arr)) {
                    pc_swap32(&arr->num);
                    if (arr->sfx_ids != NULL && sane_count(arr->num, 256, "sfx id") &&
                        pc_swap_once(arr->sfx_ids)) {
                        pc_swap32_range(arr->sfx_ids, (size_t) arr->num * sizeof(s32));
                    }
                }
            } else {
                pc_swap32(&w[i]);
            }
        }
    }
    /* x54 is declared int but ftCo_09F7.c reads it as a pointer to a table
     * of five ints (an effect part list): relocated, so swap the table */
    if (pc_swap_is_reloc_slot(&d->x54)) {
        int* table = (int*) (uintptr_t) d->x54;
        if (table != NULL && pc_swap_ptr_ok(table) && pc_swap_once(table)) {
            pc_swap32_range(table, 5 * sizeof(int));
        }
    } else {
        pc_swap32(&d->x54);
    }
    if (d->x58 != NULL && pc_swap_once(d->x58)) {
        pc_swapf(&d->x58->x4);
        pc_swapf(&d->x58->xC);
        pc_swapf(&d->x58->x18);
    }
}

/* Fighter common data (PlCo.dat "ftLoadCommonData"): 23 pointers to shared
 * tables. Only the ones with a known layout are swapped so far. */
void pc_swap_ft_common(void** tables)
{
    int i;
    if (tables == NULL || !pc_swap_once(tables)) {
        return;
    }
    /* [0] ftCommonData: 0x818 bytes of floats and ints, four bytes at 0x6EC */
    if (tables[0] != NULL && pc_swap_once(tables[0])) {
        pc_swap32_range(tables[0], 0x6EC);
        pc_swap32_range((u8*) tables[0] + 0x6F0, 0x818 - 0x6F0);
    }
    /* [1] item throw attributes: {velocity_mul, angle, x8} per throw motion
     * state (ftCo_MS_LightThrowF .. ftCo_MS_HeavyThrowLw4) */
    if (tables[1] != NULL && pc_swap_once(tables[1])) {
        pc_swap32_range(tables[1], 26 * 3 * sizeof(f32));
    }
    /* [2] item swing animation speeds, float[swing type][5]; [3] a float
     * list (ft_0881.c): both run to the next object in the file. Without
     * [2] a fan swing ran at a denormal speed and never ended. */
    for (i = 2; i <= 3; i++) {
        if (tables[i] != NULL && pc_swap_ptr_ok(tables[i]) && pc_swap_once(tables[i])) {
            const u8* end = (const u8*) pc_swap_next_object(tables[i], (const u8*) tables[i] + 4096);
            pc_swap32_range(tables[i], (size_t) (end - (const u8*) tables[i]) & ~(size_t) 3);
        }
    }
    /* [4] parts tables, one per fighter kind: {u8* joint_to_part; u8* part_to_joint; u32 parts_num}.
     * Both per-kind tables have one entry more than FTKIND_MAX: a thrown
     * fighter's animation is looked up with kind 33 ("no kind"), so the
     * arrays are walked to their end (the next relocated object). */
    if (tables[4] != NULL && pc_swap_once(tables[4])) {
        FighterPartsTable** parts = (FighterPartsTable**) tables[4];
        const void* end = pc_swap_next_object(parts, (const u8*) parts + 64 * sizeof(void*));
        for (i = 0; (const void*) &parts[i + 1] <= end && pc_swap_is_reloc_slot(&parts[i]); i++) {
            if (parts[i] != NULL && pc_swap_once(parts[i])) {
                pc_swap32(&parts[i]->parts_num);
            }
        }
    }
    /* [5] per kind {byte entries* x0; int x4} */
    if (tables[5] != NULL && pc_swap_once(tables[5])) {
        struct Fighter_804D6540_t** t = (struct Fighter_804D6540_t**) tables[5];
        const void* end = pc_swap_next_object(t, (const u8*) t + 64 * sizeof(void*));
        for (i = 0; (const void*) &t[i + 1] <= end && pc_swap_is_reloc_slot(&t[i]); i++) {
            if (t[i] != NULL && pc_swap_once(t[i])) {
                pc_swap32(&t[i]->x4);
            }
        }
    }
    /* [10], [11] shake tables {Vec2* x0; int x4} */
    for (i = 10; i <= 11; i++) {
        struct Fighter_ShakeTable_t* t = (struct Fighter_ShakeTable_t*) tables[i];
        if (t != NULL && pc_swap_once(t)) {
            pc_swap32(&t->x4);
            if (t->x0 != NULL && sane_count(t->x4, 256, "shake table") && pc_swap_once(t->x0)) {
                pc_swap32_range(t->x0, (size_t) t->x4 * sizeof(Vec2));
            }
        }
    }
    /* [12] .. [15] float tables */
    if (tables[12] != NULL && pc_swap_once(tables[12])) {
        pc_swap32_range(tables[12], sizeof(struct Fighter_804D6524_t));
    }
    if (tables[13] != NULL && pc_swap_once(tables[13])) {
        pc_swap32_range(tables[13], sizeof(struct Fighter_804D6520_t));
    }
    if (tables[14] != NULL && pc_swap_once(tables[14])) {
        pc_swap32_range(tables[14], sizeof(struct Fighter_804D651C_t));
    }
    if (tables[15] != NULL && pc_swap_once(tables[15])) {
        pc_swap32_range(tables[15], sizeof(struct Fighter_804D6518_t));
    }
    /* [21] crowd reaction thresholds: 0x44 bytes of ints and floats */
    /* [21] crowd reaction thresholds: 0x44 bytes of ints and floats (the
     * table's own extent bounds it, pointer slots are left alone) */
    if (tables[21] != NULL && pc_swap_ptr_ok(tables[21]) && pc_swap_once(tables[21])) {
        u8* b = (u8*) tables[21];
        const u8* end = (const u8*) pc_swap_next_object(b, b + 0x44);
        int k;
        for (k = 0; b + k * 4 + 4 <= end; k++) {
            if (!pc_swap_is_reloc_slot(b + k * 4)) {
                pc_swap32(b + k * 4);
            }
        }
    }
    /* [22] CPU attack selection: per-kind lists of ftCo_AttackEntry (nine
     * 32-bit words each, ended by a zero command), per-kind distance
     * thresholds and six weapon reach floats. Every array is bounded by
     * the next relocated object and a record is only swapped while it
     * looks like one (command below 0x1000, CPU level 0..9), so a list
     * that turns out to be something else (the byte command scripts) is
     * left alone. Unswapped, every distance and timing test the CPU made
     * ran on garbage. */
    if (tables[22] != NULL && pc_swap_once(tables[22])) {
        struct Fighter_804D64FC_t* t = (struct Fighter_804D64FC_t*) tables[22];
        void** lists[5];
        int li, k;
        lists[0] = t->x4;
        lists[1] = t->x8;
        lists[2] = t->x10;
        lists[3] = t->x18;
        lists[4] = t->x1C;
        for (li = 0; li < 5; li++) {
            const void* arr_end;
            if (lists[li] == NULL || !pc_swap_ptr_ok(lists[li])) {
                continue;
            }
            arr_end = pc_swap_next_object(lists[li], (const u8*) lists[li] + 64 * sizeof(void*));
            for (k = 0; (const void*) &lists[li][k + 1] <= arr_end && pc_swap_is_reloc_slot(&lists[li][k]); k++) {
                u32* e = (u32*) lists[li][k];
                const u32* end;
                if (e == NULL || !pc_swap_ptr_ok(e) || !pc_swap_once(e)) {
                    continue;
                }
                end = (const u32*) pc_swap_next_object(e, (const u8*) e + 256 * 9 * sizeof(u32));
                for (; e + 9 <= end; e += 9) {
                    u32 cmd = PC_BSWAP32(e[0]), level = PC_BSWAP32(e[8]);
                    if (cmd == 0) {
                        pc_swap32_range(e, 9 * sizeof(u32));
                        break;
                    }
                    if (cmd >= 0x1000 || level > 9) {
                        break; /* not an attack list */
                    }
                    pc_swap32_range(e, 9 * sizeof(u32));
                }
            }
        }
        if (t->x20 != NULL && pc_swap_ptr_ok(t->x20) && pc_swap_once(t->x20)) {
            const u8* end = (const u8*) pc_swap_next_object(t->x20, (const u8*) t->x20 + 64 * sizeof(f32));
            pc_swap32_range(t->x20, (size_t) (end - (const u8*) t->x20) & ~(size_t) 3);
        }
        if (t->x24 != NULL && pc_swap_ptr_ok(t->x24) && pc_swap_once(t->x24)) {
            const u8* end = (const u8*) pc_swap_next_object(t->x24, (const u8*) t->x24 + 6 * sizeof(f32));
            pc_swap32_range(t->x24, (size_t) (end - (const u8*) t->x24) & ~(size_t) 3);
        }
        if (getenv("MELEE_TRACE_SWAP") != NULL) {
            const s32* e = t->x4 != NULL ? (const s32*) t->x4[0] : NULL;
            fprintf(stderr, "[pc] PlCo cpu tables: x4 %p x8 %p x20 %p (%g %g %g ...) x24 %p (%g %g) first ground entry %p: cmd %d x04 %d x08 %g level %d\n",
                    (void*) t->x4, (void*) t->x8, (void*) t->x20, t->x20 != NULL ? t->x20[0] : 0.0f,
                    t->x20 != NULL ? t->x20[1] : 0.0f, t->x20 != NULL ? t->x20[2] : 0.0f, t->x24,
                    t->x24 != NULL ? ((float*) t->x24)[0] : 0.0f, t->x24 != NULL ? ((float*) t->x24)[1] : 0.0f,
                    (const void*) e, e != NULL ? e[0] : 0, e != NULL ? e[1] : 0, e != NULL ? ((const float*) e)[2] : 0.0f,
                    e != NULL ? e[8] : 0);
        }
    }
}

/* A thrown fighter's animation is looked up with kind 33 ("no kind"),
 * one past the last fighter kind, and the per-kind tables of PlCo.dat do
 * have that extra entry. The table walk above may stop before it (the
 * entry's own data can sit right behind the pointer array), so the entry
 * is swapped here, lazily, when it is first used. */
void pc_swap_ft_kind_entry(int kind)
{
    if (kind < 0 || kind > 63) {
        return;
    }
    if (Fighter_804D6540 != NULL && pc_swap_ptr_ok(&Fighter_804D6540[kind]) &&
        pc_swap_is_reloc_slot(&Fighter_804D6540[kind])) {
        struct Fighter_804D6540_t* e = Fighter_804D6540[kind];
        if (e != NULL && pc_swap_ptr_ok(e) && pc_swap_once(e)) {
            pc_swap32(&e->x4);
        }
    }
    if (ftPartsTable != NULL && pc_swap_ptr_ok(&ftPartsTable[kind]) && pc_swap_is_reloc_slot(&ftPartsTable[kind])) {
        FighterPartsTable* t = ftPartsTable[kind];
        if (t != NULL && pc_swap_ptr_ok(t) && pc_swap_once(t)) {
            pc_swap32(&t->parts_num);
        }
    }
}

/* --- Dynamics (bone physics) descriptors ----------------------------------
 * {DynamicsData* data; u32 count; Vec3 pos}; on disc `data` leads to an
 * array of count lb_00F9_UnkDesc1Inner records, all floats. */
void pc_swap_dynamics_desc(struct DynamicsDesc* desc)
{
    if (desc == NULL || !pc_swap_once(desc)) {
        return;
    }
    pc_swap32(&desc->count);
    pc_swap32_range(&desc->pos, sizeof(Vec3));
    if (desc->data != NULL && sane_count(desc->count, 64, "dynamics data") && pc_swap_once(desc->data)) {
        pc_swap32_range(desc->data, (size_t) desc->count * sizeof(struct lb_00F9_UnkDesc1Inner));
    }
}

/* A stage hazard's hit description (lbColl_80008D30_arg1: nine 32-bit
 * words) that a stage's collision callback points the fighter code at. */
void pc_swap_hazard_hit(void* hit)
{
    u8* b = (u8*) hit;
    int i;
    if (hit == NULL || !pc_swap_ptr_ok(hit) || !pc_swap_once(hit)) {
        return;
    }
    for (i = 0; i < 9; i++) {
        if (!pc_swap_is_reloc_slot(b + i * 4)) {
            pc_swap32(b + i * 4);
        }
    }
}

/* --- Items ------------------------------------------------------------------ */

/* the Item struct is shared with the console layout; a mismatch here would
 * make item code read fields from the wrong offsets */
typedef char pc_item_layout_check[(offsetof(Item, xC40) == 0xC40 && offsetof(Item, xC4_article_data) == 0xC4) ? 1 : -1];


static void collect_article_bounds(struct Article* a);
static void swap_special_blocks(void);

void pc_swap_stage_article(struct Article* a)
{
    collect_article_bounds(a);
    pc_swap_article(a);
    swap_special_blocks();
}

void pc_swap_article(struct Article* a)
{
    if (a == NULL || !pc_swap_once(a)) {
        return;
    }
    if (pc_debug_gx) {
        fprintf(stderr, "[gx] article %p: attr %p special %p hurt %p states %p model %p dyn %p\n",
                (void*) a, (void*) a->x0_common_attr, a->x4_specialAttributes, (void*) a->x8_hurtbones,
                (void*) a->xC_itemStates, (void*) a->x10_modelDesc, (void*) a->x14_dynamics);
    }
    /* common attributes: two bit-field bytes (read with console bit order
     * through a TARGET_PC definition), two bytes, then 32-bit fields */
    if (a->x0_common_attr != NULL && pc_swap_once(a->x0_common_attr)) {
        pc_swap32_range((u8*) a->x0_common_attr + 4, sizeof(ItemAttr) - 4);
    }
    /* x4 special attributes are item specific: swapped by the item's own code */
    if (a->x8_hurtbones != NULL && pc_swap_once(a->x8_hurtbones)) {
        pc_swap32(&a->x8_hurtbones->count);
        if (a->x8_hurtbones->descs != NULL && sane_count(a->x8_hurtbones->count, 32, "item hurtbox") &&
            pc_swap_once(a->x8_hurtbones->descs)) {
            pc_swap32_range(a->x8_hurtbones->descs,
                            (size_t) a->x8_hurtbones->count * sizeof(ItHurtBoneDesc));
        }
    }
    /* xC item states: pointers only */
    if (a->x10_modelDesc != NULL && pc_swap_once(a->x10_modelDesc)) {
        pc_swap32(&a->x10_modelDesc->x4_bone_count);
        pc_swap32(&a->x10_modelDesc->x8_bone_attach_id);
        /* xC is a byte read with shifts and masks */
    }
    if (a->x14_dynamics != NULL && pc_swap_once(a->x14_dynamics)) {
        int i;
        pc_swap32(&a->x14_dynamics->count);
        if (a->x14_dynamics->dyn_descs != NULL && sane_count(a->x14_dynamics->count, 32, "item dynamics") &&
            pc_swap_once(a->x14_dynamics->dyn_descs)) {
            for (i = 0; i < a->x14_dynamics->count; i++) {
                pc_swap32(&a->x14_dynamics->dyn_descs[i].bone_id);
                pc_swap_dynamics_desc(&a->x14_dynamics->dyn_descs[i].dyn_desc);
            }
        }
        /* itcoll.c reads the same block as ItCollDynamics: after the bone
         * dynamics come a hit-collision count (+8) and its descriptors
         * (+0xC: bone id, offset, size = five words each) */
        {
            u8* b = (u8*) a->x14_dynamics;
            s32* coll_count = (s32*) (b + 8);
            u8** coll_descs = (u8**) (b + 12);
            if (!pc_swap_is_reloc_slot(coll_count)) {
                pc_swap32(coll_count);
            }
            if (*coll_descs != NULL && pc_swap_ptr_ok(*coll_descs) && sane_count(*coll_count, 32, "item hit dynamics") &&
                pc_swap_once(*coll_descs)) {
                pc_swap32_range(*coll_descs, (size_t) *coll_count * 5 * sizeof(u32));
            }
        }
    }
}

/* Item-specific attribute blocks (Article::x4_specialAttributes) have no
 * recorded size: each item's code casts them to its own struct. In the
 * archives they are stored back to back, so a block ends where the next
 * block of any article begins. The blocks are floats and ints. */
#define MAX_SPECIAL_BLOCKS 512
static uintptr_t special_blocks[MAX_SPECIAL_BLOCKS];
static uintptr_t special_bounds[MAX_SPECIAL_BLOCKS * 6];
static int special_block_count, special_bound_count;

static void note_bound(uintptr_t p)
{
    if (p != 0 && special_bound_count < MAX_SPECIAL_BLOCKS * 6) {
        special_bounds[special_bound_count++] = p;
    }
}

static void collect_article_bounds(struct Article* a)
{
    if (a == NULL || !pc_swap_ptr_ok(a)) {
        return;
    }
    if (a->x4_specialAttributes != NULL && special_block_count < MAX_SPECIAL_BLOCKS) {
        special_blocks[special_block_count++] = (uintptr_t) a->x4_specialAttributes;
    }
    note_bound((uintptr_t) a->x0_common_attr);
    note_bound((uintptr_t) a->x8_hurtbones);
    note_bound((uintptr_t) a->xC_itemStates);
    note_bound((uintptr_t) a->x10_modelDesc);
    note_bound((uintptr_t) a->x14_dynamics);
    note_bound((uintptr_t) a);
    if (a->x8_hurtbones != NULL && pc_swap_ptr_ok(a->x8_hurtbones)) {
        note_bound((uintptr_t) a->x8_hurtbones->descs);
    }
    if (a->x10_modelDesc != NULL && pc_swap_ptr_ok(a->x10_modelDesc)) {
        note_bound((uintptr_t) a->x10_modelDesc->x0_joint);
    }
    if (a->x14_dynamics != NULL && pc_swap_ptr_ok(a->x14_dynamics)) {
        note_bound((uintptr_t) a->x14_dynamics->dyn_descs);
    }
    if (a->xC_itemStates != NULL && pc_swap_ptr_ok(a->xC_itemStates)) {
        int k;
        for (k = 0; k < 8; k++) {
            struct ItemStateDesc* st = &a->xC_itemStates->x0_itemStateDesc[k];
            note_bound((uintptr_t) st->x0_anim_joint);
            note_bound((uintptr_t) st->x4_matanim_joint);
            note_bound((uintptr_t) st->x8_parameters);
            note_bound((uintptr_t) st->xC_script);
        }
    }
}

static void swap_special_blocks(void)
{
    int i, j;
    for (i = 0; i < special_block_count; i++) {
        uintptr_t start = special_blocks[i], end = start + 0x400;
        for (j = 0; j < special_block_count; j++) {
            if (special_blocks[j] > start && special_blocks[j] < end) {
                end = special_blocks[j];
            }
        }
        for (j = 0; j < special_bound_count; j++) {
            if (special_bounds[j] > start && special_bounds[j] < end) {
                end = special_bounds[j];
            }
        }
        /* the next object referenced from elsewhere in the file also ends
         * the block (anim joints, joints, ... that follow it) */
        end = (uintptr_t) pc_swap_next_object((void*) start, (void*) end);
        if (getenv("MELEE_TRACE_SWAP") != NULL) {
            fprintf(stderr, "[pc] item attribute block %p: %u bytes\n", (void*) start, (unsigned) (end - start));
        }
        /* swap the words up to the next block, leaving relocated pointer
         * slots (a few blocks point at sub-tables) alone */
        if (pc_swap_once((void*) start)) {
            for (j = 0; start + j * 4 + 4 <= end; j++) {
                if (!pc_swap_is_reloc_slot((void*) (start + j * 4))) {
                    pc_swap32((void*) (start + j * 4));
                }
            }
        }
    }
    special_block_count = special_bound_count = 0;
}

static void swap_article_table(struct Article** t, int count)
{
    int i;
    if (t == NULL || !pc_swap_once(t)) {
        return;
    }
    for (i = 0; i < count; i++) {
        collect_article_bounds(t[i]);
    }
    for (i = 0; i < count; i++) {
        pc_swap_article(t[i]);
    }
}

void pc_swap_item_common(struct it_804D6D20_t* root)
{
    if (root == NULL || !pc_swap_once(root)) {
        return;
    }
    if (root->x0 != NULL && pc_swap_once(root->x0)) {
        u8* c = (u8*) root->x0;
        pc_swap32_range(c, 0x48);
        pc_swap32_range(c + 0x4C, 0xE4 - 0x4C);
        pc_swap32_range(c + 0xE8, 4);
        pc_swap32_range(c + 0xF0, sizeof(ItemCommonData) - 0xF0);
    }
    swap_article_table(root->x4, It_Kind_Kuriboh);
    swap_article_table(root->x8, It_PKind_Start - It_Kind_Kuriboh);
    swap_article_table(root->xC, It_Kind_Old_Kuri - It_PKind_Start);
    swap_special_blocks();
    if (root->x10 != NULL && pc_swap_once(root->x10)) {
        pc_swap32_range(root->x10, sizeof(it_804D6D40_t));
    }
}

/* --- Animation command scripts ----------------------------------------------
 * Fighter subaction and item state scripts are streams of 32-bit big-endian
 * words holding bit-packed commands. The interpreter views them through the
 * CmdUnion bit-fields (declared in console order on PC) and, in a few
 * places, as raw half-words and bytes (CMD_HALF / CMD_BYTE), so every word
 * is swapped to host order here, walking the script by command length.
 * Pointer words (subroutine and goto targets) were relocated by the archive
 * pre-pass and are followed instead of swapped. */

/* lengths in words of fighter commands 10.. (ftAction_803C0870) */
static const u8 ft_cmd_len[49] = { 5, 5, 1, 1, 1, 1, 1, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                   1, 1, 1, 1, 1, 1, 1, 3, 1, 1, 1, 7, 4, 1, 1, 1, 1,
                                   1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 3, 3, 2, 1, 4 };
/* lengths in words of item commands 10.. (it_803F22A8 handlers) */
static const u8 it_cmd_len[16] = { 5, 6, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
/* lengths in words of colour overlay commands 10.. (lb_803BA248 handlers);
 * command 10 ends the script without advancing */
static const u8 overlay_cmd_len[11] = { 0, 1, 1, 2, 2, 2, 1, 1, 2, 2, 1 };

void pc_swap_script(void* start, int kind)
{
    u32* p = (u32*) start;
    for (;;) {
        u32 be, opcode, len, i;
        if (p == NULL || !pc_swap_ptr_ok(p) || !pc_swap_once(p)) {
            return; /* end of memory, or already swapped from here on */
        }
        be = PC_BSWAP32(*p);
        opcode = be >> 26;
        if (opcode == 0) {
            pc_swap32(p);
            return;
        }
        if (opcode < 10) {
            static const u8 generic_len[10] = { 1, 1, 1, 1, 1, 2, 1, 2, 1, 1 };
            len = generic_len[opcode];
            pc_swap32(p);
            if (opcode == 5 || opcode == 7) {
                /* subroutine / goto: relocated pointer in the second word */
                if (!pc_swap_is_reloc_slot(p + 1) || !pc_swap_ptr_ok((void*) p[1])) {
                    fprintf(stderr, "[pc] script %p: %s target word %08x at %p is not a relocated pointer\n",
                            start, opcode == 5 ? "subroutine" : "goto", p[1], (void*) (p + 1));
                }
                pc_swap_script((void*) p[1], kind);
                if (opcode == 7) {
                    return;
                }
            }
            p += len;
            continue;
        }
        if (kind == PC_SCRIPT_FIGHTER) {
            if (opcode - 10 >= sizeof(ft_cmd_len)) {
                return;
            }
            len = ft_cmd_len[opcode - 10];
        } else if (kind == PC_SCRIPT_OVERLAY) {
            if (opcode - 10 < sizeof(overlay_cmd_len)) {
                len = overlay_cmd_len[opcode - 10];
                if (len == 0) {
                    pc_swap32(p);
                    return;
                }
            } else {
                len = 1; /* stage specific command (grMaterial_801C9490) */
            }
        } else {
            if (opcode - 10 >= sizeof(it_cmd_len)) {
                return;
            }
            len = it_cmd_len[opcode - 10];
            if (len == 0) {
                /* it_8027978C: a sub-opcode in bits 25..18 selects the length */
                u32 sub = (be >> 18) & 0xFF;
                len = (sub < 3 || sub == 10 || sub == 11) ? 3 : 2;
            }
        }
        for (i = 0; i < len; i++) {
            if (!pc_swap_is_reloc_slot(p + i)) {
                pc_swap32(p + i);
            }
        }
        p += len;
    }
}

/* Character-specific attribute block (ftData::ext_attr): floats and ints.
 * sword_off >= 0: the offset of a SwordAttrs block {f32 x0, x4; u8 x8..x10
 * (alpha and colour of the sword trail); pad; int x14; f32 x18, x1C}, whose
 * three byte-filled words are put back in file order. */
void pc_swap_ext_attrs(void* attrs, size_t size, int sword_off)
{
    if (attrs != NULL && pc_swap_once(attrs)) {
        pc_swap32_range(attrs, size & ~(size_t) 3);
        if (sword_off >= 0 && (size_t) sword_off + 0x14 <= size) {
            u8* s = (u8*) attrs + sword_off;
            pc_swap32(s + 8);
            pc_swap32(s + 0xC);
            pc_swap32(s + 0x10);
        }
    }
}

/* Figatree (fighter animation) archives: the archive pre-pass relocated the
 * node and track pointers; the scalars of the tree and of every track are
 * swapped here. Tracks are counted by walking the -1 terminated node list. */
/* Trophy tables from TyDatai.dat (toy.c, tydisplay.c):
 * - tyInitModelTbl, tyInitModelDTbl: rows of {s32 id, s32, f32 x6, s8 x4},
 *   terminated by id == -1;
 * - tyModelSortTbl: one row of six s16 per trophy (TY_TROPHY_COUNT);
 * - tyExpDifferentTbl, tyNoGetUsTbl: s16 lists terminated by -1;
 * - tyDisplayModelTbl, tyDisplayModelUsTbl: rows of {s32 id, u8, u8, pad,
 *   f32, f32}, terminated by id == -1. */
static void swap_s16_list(void* list)
{
    s16* v = (s16*) list;
    int n;
    if (v == NULL || !pc_swap_ptr_ok(v) || !pc_swap_once(v)) {
        return;
    }
    for (n = 0; n < 1024; n++) {
        pc_swap16(&v[n]);
        if (v[n] == -1) {
            break;
        }
    }
}

static void swap_id_rows(void* table, int row_bytes, int words)
{
    u8* row = (u8*) table;
    int n;
    if (row == NULL || !pc_swap_ptr_ok(row) || !pc_swap_once(row)) {
        return;
    }
    for (n = 0; n < 1024; n++, row += row_bytes) {
        int i;
        for (i = 0; i < words; i++) {
            pc_swap32(row + i * 4);
        }
        if (*(s32*) row == -1) {
            break;
        }
    }
    if (getenv("MELEE_TRACE_SWAP") != NULL) {
        fprintf(stderr, "[pc] trophy table at %p: %d rows of %d bytes\n", table, n, row_bytes);
    }
}

void pc_swap_trophy_tables(void* init_tbl, void* init_d_tbl, void* sort_tbl, void* exp_tbl,
                           void* no_get_us_tbl, void* display_tbl, void* display_us_tbl)
{
    swap_id_rows(init_tbl, 0x24, 8);    /* the four s8 at the end stay */
    swap_id_rows(init_d_tbl, 0x24, 8);
    if (sort_tbl != NULL && pc_swap_ptr_ok(sort_tbl) && pc_swap_once(sort_tbl)) {
        s16* v = (s16*) sort_tbl;
        int n;
        for (n = 0; n < 293 * 6; n++) {
            pc_swap16(&v[n]);
        }
    }
    swap_s16_list(exp_tbl);
    swap_s16_list(no_get_us_tbl);
    /* display rows: id, two bytes and padding, two floats */
    {
        void* tables[2];
        int t;
        tables[0] = display_tbl;
        tables[1] = display_us_tbl;
        for (t = 0; t < 2; t++) {
            u8* row = (u8*) tables[t];
            int n;
            if (row == NULL || !pc_swap_ptr_ok(row) || !pc_swap_once(row)) {
                continue;
            }
            for (n = 0; n < 1024; n++, row += 0x10) {
                pc_swap32(row);
                pc_swap32(row + 8);
                pc_swap32(row + 12);
                if (*(s32*) row == -1) {
                    break;
                }
            }
        }
    }
}

/* LbRb.dat's lbRumbleData: entries of {u16* list, u8 priority, u8} up to
 * the next object in the file; each list is u16 words (3-bit command in
 * the top bits, count below) ending with a command-0 word. */
void pc_swap_rumble_data(void* table)
{
    u8* e = (u8*) table;
    const u8* end;
    int n;
    if (e == NULL || !pc_swap_ptr_ok(e) || !pc_swap_once(e)) {
        return;
    }
    end = (const u8*) pc_swap_next_object(e, e + 256 * 8);
    for (n = 0; e + 8 <= end && n < 256; n++, e += 8) {
        u16* w = *(u16**) e;
        int k;
        if (w == NULL || !pc_swap_ptr_ok(w) || !pc_swap_once(w)) {
            continue;
        }
        for (k = 0; k < 4096; k++) {
            pc_swap16(&w[k]);
            if (((w[k] >> 13) & 7) == 0) {
                break;
            }
        }
    }
    if (getenv("MELEE_TRACE_SWAP") != NULL) {
        fprintf(stderr, "[pc] rumble table %p: %d entries\n", table, n);
    }
}

void pc_swap_figatree(struct FigaTree* tree)
{
    s8* node;
    int count = 0, i;
    if (tree == NULL || !pc_swap_once(tree)) {
        return;
    }
    pc_swap32(&tree->type);
    pc_swap32(&tree->flags);
    pc_swapf(&tree->frames);
    if (tree->nodes == NULL || tree->tracks == NULL) {
        return;
    }
    for (node = tree->nodes; *node != -1; node++) {
        count += *node;
    }
    if (pc_swap_once(tree->tracks)) {
        for (i = 0; i < count; i++) {
            pc_swap16(&tree->tracks[i].length);
            pc_swap16(&tree->tracks[i].startframe);
        }
    }
    if (pc_debug_gx) {
        fprintf(stderr,
                "[gx] figatree %p: type %d flags %08x frames %g nodes %p tracks %p (%d) "
                "track0 len %u start %u type %u ad %p\n",
                (void*) tree, tree->type, tree->flags, tree->frames, (void*) tree->nodes,
                (void*) tree->tracks, count, tree->tracks[0].length, tree->tracks[0].startframe,
                tree->tracks[0].obj_type, (void*) tree->tracks[0].ad_head);
    }
}
