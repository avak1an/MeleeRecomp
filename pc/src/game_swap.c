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
        pc_swap32(&d->x8->x0.model_num);
        pc_swap32(&d->x8->x8.x8);
        /* per costume: four visibility tables of model_num {int n; TempS* e}
         * entries, each TempS being {int n; u8* dobj_indices} */
        if (d->x8->x0.vis_table != NULL && pc_swap_once(d->x8->x0.vis_table)) {
            int c, k, m, j;
            for (c = 0; c < costumes; c++) {
                for (k = 0; k < 4; k++) {
                    struct FtPartsVisLookup* lookup = d->x8->x0.vis_table[c][k];
                    if (lookup == NULL || !pc_swap_once(lookup)) {
                        continue;
                    }
                    for (m = 0; m < (int) d->x8->x0.model_num; m++) {
                        pc_swap32(&lookup[m].x0);
                        if (lookup[m].x4 != NULL && pc_swap_once(lookup[m].x4)) {
                            for (j = 0; j < lookup[m].x0; j++) {
                                pc_swap32(&lookup[m].x4[j].x0);
                            }
                        }
                    }
                }
            }
        }
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
    /* [4] parts tables, one per fighter kind: {u8* joint_to_part; u8* part_to_joint; u32 parts_num} */
    if (tables[4] != NULL && pc_swap_once(tables[4])) {
        FighterPartsTable** parts = (FighterPartsTable**) tables[4];
        for (i = 0; i < FTKIND_MAX; i++) {
            if (parts[i] != NULL && pc_swap_once(parts[i])) {
                pc_swap32(&parts[i]->parts_num);
            }
        }
    }
    /* [5] per kind {byte entries* x0; int x4} */
    if (tables[5] != NULL && pc_swap_once(tables[5])) {
        struct Fighter_804D6540_t** t = (struct Fighter_804D6540_t**) tables[5];
        for (i = 0; i < FTKIND_MAX; i++) {
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

/* Character-specific attribute block (ftData::ext_attr): floats and ints. */
void pc_swap_ext_attrs(void* attrs, size_t size)
{
    if (attrs != NULL && pc_swap_once(attrs)) {
        pc_swap32_range(attrs, size & ~(size_t) 3);
    }
}

/* Figatree (fighter animation) archives: the archive pre-pass relocated the
 * node and track pointers; the scalars of the tree and of every track are
 * swapped here. Tracks are counted by walking the -1 terminated node list. */
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
